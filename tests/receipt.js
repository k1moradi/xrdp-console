/* Diagnostic-only, privacy-preserving DOM image receipt. No external network calls.
 * Invoke via a real paste event to retain isTrusted; fixtureSelfTest is NOT a paste.
 */
(function () {
  'use strict';
  const LIMIT_BYTES = 64 * 1024 * 1024;
  const EXPECTED_SIGNATURE = [137, 80, 78, 71, 13, 10, 26, 10];
  let sequence = 0;
  let current = {phase: 'waiting'};

  function publish(report) {
    // Publish a metadata-only clone, not Blob/File/ArrayBuffer references.
    current = Object.freeze({...report});
    window.__clipboardImageReceipt = current;
  }

  function nowMs() {
    return performance.now();
  }

  function errorCode(e, step) {
    const name = e && typeof e.name === 'string' ? e.name : 'UnknownError';
    return `${step}:${name.replace(/[^A-Za-z0-9_]/g, '').slice(0, 48)}`;
  }

  async function inspect(file, record, id) {
    const report = {...record, phase: 'reading', readError: null,
      digestError: null, decodeError: null, signatureValid: null,
      sha256: null, readBytes: null, decodedWidth: null, decodedHeight: null,
      ihdrWidth: null, ihdrHeight: null, readMs: null,
      digestMs: null, decodeMs: null};
    if (id === sequence) publish(report);
    if (!file) {
      if (id === sequence) publish({...report, phase: 'complete'});
      return;
    }
    if (!Number.isSafeInteger(file.size) || file.size > LIMIT_BYTES) {
      publish({...report, phase: 'complete', readError: 'file-size-limit'});
      return;
    }
    let bytes;
    let stageStart = nowMs();
    try {
      bytes = await file.arrayBuffer();
      if (id !== sequence) return;
      report.readMs = Math.round(nowMs() - stageStart);
      report.readBytes = bytes.byteLength;
      const view = new Uint8Array(bytes);
      report.signatureValid = view.length >= 8 &&
        EXPECTED_SIGNATURE.every((v, i) => view[i] === v);
      if (report.signatureValid && view.length >= 24) {
        const ihdr = new DataView(bytes);
        report.ihdrWidth = ihdr.getUint32(16, false);
        report.ihdrHeight = ihdr.getUint32(20, false);
      }
    } catch (error) {
      if (id === sequence) publish({...report, phase: 'complete',
        readError: errorCode(error, 'arrayBuffer')});
      return;
    }
    if (id !== sequence) return;
    report.phase = 'hashing';
    publish(report);
    stageStart = nowMs();
    try {
      if (!globalThis.crypto || !globalThis.crypto.subtle ||
          typeof globalThis.crypto.subtle.digest !== 'function') {
        report.digestError = 'SubtleCrypto-unavailable';
      } else {
        const digest = await crypto.subtle.digest('SHA-256', bytes);
        report.sha256 = Array.from(new Uint8Array(digest),
          b => b.toString(16).padStart(2, '0')).join('');
      }
    } catch (error) {
      report.digestError = errorCode(error, 'digest');
    }
    report.digestMs = Math.round(nowMs() - stageStart);
    if (id !== sequence) return;
    report.phase = 'decoding';
    publish(report);
    stageStart = nowMs();
    try {
      if (!report.signatureValid) {
        report.decodeError = 'invalid-png-signature';
      } else if (typeof createImageBitmap !== 'function') {
        report.decodeError = 'createImageBitmap-unavailable';
      } else {
        const bitmap = await createImageBitmap(file);
        try {
          report.decodedWidth = bitmap.width;
          report.decodedHeight = bitmap.height;
        } finally {
          bitmap.close();
        }
      }
    } catch (error) {
      report.decodeError = errorCode(error, 'createImageBitmap');
    }
    report.decodeMs = Math.round(nowMs() - stageStart);
    if (id === sequence) publish({...report, phase: 'complete'});
  }

  function capturePaste(event) {
    // getAsFile is intentionally called synchronously in this trusted handler.
    const items = Array.from(event.clipboardData?.items || []);
    const imageItems = items.filter(item => item.kind === 'file' &&
      item.type.startsWith('image/'));
    let file = null;
    let getAsFileError = null;
    try {
      if (imageItems.length) file = imageItems[0].getAsFile();
    } catch (error) {
      getAsFileError = errorCode(error, 'getAsFile');
    }
    const id = ++sequence;
    const meta = {
      source: 'paste', phase: 'captured', trusted: event.isTrusted,
      types: Array.from(event.clipboardData?.types || []),
      items: items.map(x => ({kind: x.kind, type: x.type})),
      imageItemCount: imageItems.length,
      getAsFileNull: file === null, getAsFileError,
      filesLength: event.clipboardData?.files.length ?? -1,
      fileType: file ? file.type : null,
      fileSize: file ? file.size : null,
      selectionGeneration: null, // Browser DOM API does not expose X11 generation.
    };
    event.preventDefault(); // Do not insert any clipboard contents into the page.
    publish(meta);
    void inspect(file, meta, id);
  }

  // Synthetic fixture route: checks hashing/decoding only; is NOT a trusted paste.
  async function fixtureSelfTest(url) {
    const response = await fetch(url, {cache: 'no-store'});
    if (!response.ok) throw new Error(`Fixture HTTP ${response.status}`);
    const blob = await response.blob();
    const file = new File([blob], 'synthetic.png', {type: 'image/png'});
    const id = ++sequence;
    const meta = {source: 'synthetic-validation', trusted: false,
      phase: 'captured', types: [], items: [], imageItemCount: 0,
      getAsFileNull: false, getAsFileError: null, filesLength: 0,
      fileType: file.type, fileSize: file.size, selectionGeneration: null};
    publish(meta);
    await inspect(file, meta, id);
    return current;
  }

  window.ClipboardImageReceipt = Object.freeze({fixtureSelfTest,
    getLastReport: () => current});
  window.addEventListener('paste', capturePaste, true);
  publish({phase: 'waiting'});
})();
