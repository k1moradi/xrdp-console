/* Test-only Firefox trusted PNG paste receipt. No raw pixels or bytes exported.
 * SHA-256 is available even in contexts without WebCrypto (bounded JS fallback).
 */
(function () {
  'use strict';
  const LIMIT_BYTES = 64 * 1024 * 1024;
  const EXPECTED_SIGNATURE = [137, 80, 78, 71, 13, 10, 26, 10];
  const K = new Uint32Array([
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
  ]);
  let sequence = 0;
  let current = Object.freeze({phase: 'waiting'});
  // performance.now() is local to this document; do not compare its values
  // with chansrv CLOCK_MONOTONIC without an independently measured bridge.
  let lastTrustedShortcutMonoMs = null;
  function elapsedMs(start, end) {
    if (!Number.isFinite(start) || !Number.isFinite(end) || end < start)
      return null;
    return Math.round((end - start) * 1000) / 1000;
  }
  // Event-only metadata. No navigator.clipboard access, image bytes, key
  // content, page text, or X11 requestor identity is captured here.
  const observer = {trustedPasteShortcuts: 0, pasteEvents: 0,
    trustedPasteEvents: 0, lastShortcutWallMs: null, lastPasteWallMs: null};
  const editor = document.getElementById('editor');
  function notePasteShortcut(event) {
    if (event.isTrusted && (event.ctrlKey || event.metaKey) &&
        (event.code === 'KeyV' ||
         String(event.key || '').toLowerCase() === 'v')) {
      observer.trustedPasteShortcuts++;
      observer.lastShortcutWallMs = Date.now();
      lastTrustedShortcutMonoMs = performance.now();
    }
  }
  function getLastReport() {
    return {...current, observer: {...observer,
      editorFocused: document.activeElement === editor,
      documentHasFocus: document.hasFocus(),
      visibilityState: document.visibilityState}};
  }

  function publish(report, id) {
    if (id != null && id !== sequence) return;
    // Metadata-only shallow copy. No File/Blob/ArrayBuffer escapes.
    current = Object.freeze({...report});
    window.__clipboardImageReceipt = current;
  }
  function errorCode(error, step) {
    const name = error && typeof error.name === 'string' ? error.name : 'UnknownError';
    return step + ':' + name.replace(/[^A-Za-z0-9_]/g, '').slice(0, 48);
  }
  function rotr(x, n) { return (x >>> n) | (x << (32 - n)); }
  function sha256JS(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length > LIMIT_BYTES)
      throw new RangeError('digest input exceeds bounded Uint8Array');
    const n = bytes.length;
    const padded = Math.ceil((n + 9) / 64) * 64;
    const msg = new Uint8Array(padded);
    msg.set(bytes); msg[n] = 0x80;
    const view = new DataView(msg.buffer);
    const bitLength = n * 8; // n <= 64 MiB, hence exact IEEE-754 integer
    view.setUint32(padded - 8, Math.floor(bitLength / 4294967296), false);
    view.setUint32(padded - 4, bitLength >>> 0, false);
    const h = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
    const words = new Uint32Array(64);
    for (let base = 0; base < padded; base += 64) {
      for (let t = 0; t < 16; t++) words[t] = view.getUint32(base + 4*t, false);
      for (let t = 16; t < 64; t++) {
        const p = words[t-15], q = words[t-2];
        const s0 = rotr(p,7) ^ rotr(p,18) ^ (p >>> 3);
        const s1 = rotr(q,17) ^ rotr(q,19) ^ (q >>> 10);
        words[t] = (words[t-16] + s0 + words[t-7] + s1) >>> 0;
      }
      let [a,b,c,d,e,f,g,z] = h;
      for (let t = 0; t < 64; t++) {
        const s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        const ch = (e & f) ^ (~e & g);
        const t1 = (z + s1 + ch + K[t] + words[t]) >>> 0;
        const s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (s0 + maj) >>> 0;
        z=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;
      }
      const v=[a,b,c,d,e,f,g,z];
      for (let t=0; t<8; t++) h[t]=(h[t]+v[t])>>>0;
    }
    return Array.from(h, word => word.toString(16).padStart(8,'0')).join('');
  }
  function digestHex(value) {
    return Array.from(new Uint8Array(value), b => b.toString(16).padStart(2,'0')).join('');
  }
  async function hashBytes(buffer, report) {
    if (globalThis.crypto && globalThis.crypto.subtle &&
        typeof globalThis.crypto.subtle.digest === 'function') {
      try {
        report.sha256 = digestHex(await globalThis.crypto.subtle.digest('SHA-256', buffer));
        report.digestMethod = 'webcrypto';
        return;
      } catch (error) {
        report.digestWarning = errorCode(error, 'webcrypto');
      }
    }
    report.sha256 = sha256JS(new Uint8Array(buffer));
    report.digestMethod = 'js-fallback';
  }
  async function inspect(file, record, id) {
    const report = {...record, phase: 'reading', readError: null,
      digestError: null, digestWarning: null, digestMethod: null,
      decodeError: null, signatureValid: null, sha256: null, readBytes: null,
      decodedWidth: null, decodedHeight: null, ihdrWidth: null, ihdrHeight: null,
      readMs: null, digestMs: null, decodeMs: null};
    publish(report, id);
    if (!file) { publish({...report, phase:'complete'}, id); return; }
    if (!Number.isSafeInteger(file.size) || file.size <= 0 || file.size > LIMIT_BYTES) {
      publish({...report, phase:'complete', readError:'file-size-limit'}, id);
      return;
    }
    let bytes;
    let stageStart = performance.now();
    try {
      bytes = await file.arrayBuffer();
      if (id !== sequence) return;
      report.readMs = Math.round(performance.now() - stageStart);
      report.readBytes = bytes.byteLength;
      if (bytes.byteLength !== file.size || bytes.byteLength > LIMIT_BYTES) {
        publish({...report,phase:'complete',readError:'read-size-mismatch'}, id);
        return;
      }
      const view = new Uint8Array(bytes);
      report.signatureValid = view.length >= 8 &&
        EXPECTED_SIGNATURE.every((v, i) => view[i] === v);
      if (report.signatureValid && view.length >= 24) {
        const ihdr = new DataView(bytes);
        report.ihdrWidth = ihdr.getUint32(16, false);
        report.ihdrHeight = ihdr.getUint32(20, false);
      }
    } catch (error) {
      publish({...report, phase:'complete',readError:errorCode(error,'arrayBuffer')}, id);
      return;
    }
    if (id !== sequence) return;
    publish({...report, phase:'hashing'}, id);
    stageStart = performance.now();
    try { await hashBytes(bytes, report); }
    catch (error) { report.digestError = errorCode(error,'digest'); }
    report.digestMs = Math.round(performance.now() - stageStart);
    if (id !== sequence) return;
    publish({...report, phase:'decoding'}, id);
    stageStart = performance.now();
    try {
      if (!report.signatureValid) report.decodeError = 'invalid-png-signature';
      else if (typeof createImageBitmap !== 'function')
        report.decodeError = 'createImageBitmap-unavailable';
      else {
        const bitmap = await createImageBitmap(file);
        try {
          report.decodedWidth = bitmap.width;
          report.decodedHeight = bitmap.height;
          if (!Number.isSafeInteger(report.ihdrWidth) ||
              !Number.isSafeInteger(report.ihdrHeight) ||
              report.ihdrWidth < 1 || report.ihdrHeight < 1 ||
              report.ihdrWidth !== bitmap.width || report.ihdrHeight !== bitmap.height)
            report.decodeError = 'decoded-dimensions-mismatch';
        } finally { bitmap.close(); }
      }
    } catch (error) { report.decodeError = errorCode(error,'createImageBitmap'); }
    report.decodeMs = Math.round(performance.now() - stageStart);
    publish({...report,phase:'complete'}, id);
  }
  function capturePaste(event) {
    // This measures only time between our own trusted keydown listener
    // and paste listener, not the full Firefox/GTK native clipboard wait.
    const pasteStartMonoMs = performance.now();
    const shortcutToPasteMs = event.isTrusted
      ? elapsedMs(lastTrustedShortcutMonoMs, pasteStartMonoMs) : null;
    // Consume this candidate pairing. A later paste cannot borrow it.
    lastTrustedShortcutMonoMs = null;
    observer.pasteEvents++;
    if (event.isTrusted) observer.trustedPasteEvents++;
    observer.lastPasteWallMs = Date.now();
    // Synchronous getAsFile() in a genuine WebDriver/user paste event only.
    const items = Array.from(event.clipboardData?.items || []);
    const imageItems = items.filter(item => item.kind === 'file' && item.type === 'image/png');
    let file = null, getAsFileError = null;
    // No PNG item means getAsFile() was NEVER invoked. The old probe
    // reported getAsFileNull:true for both that case and an actual null
    // return; do not conflate MIME discovery with File materialization.
    const getAsFileInvoked = imageItems.length > 0;
    const getAsFileStartMonoMs = getAsFileInvoked ? performance.now() : null;
    try { if (getAsFileInvoked) file = imageItems[0].getAsFile(); }
    catch (error) { getAsFileError = errorCode(error,'getAsFile'); }
    const getAsFileElapsedMs = getAsFileInvoked
      ? elapsedMs(getAsFileStartMonoMs, performance.now()) : null;
    const id = ++sequence;
    const meta = {source:'paste',phase:'captured',trusted:event.isTrusted,
      types:Array.from(event.clipboardData?.types || []),
      items:items.map(item => ({kind:item.kind,type:item.type})),
      imageItemCount:imageItems.length,getAsFileInvoked,
      shortcutToPasteMs, getAsFileElapsedMs,
      getAsFileNull:getAsFileInvoked ? file===null : null,
      getAsFileError,filesLength:event.clipboardData?.files.length ?? -1,
      fileType:file?.type ?? null,fileSize:file?.size ?? null,
      selectionGeneration:null};
    event.preventDefault();
    publish(meta,id);
    void inspect(file,meta,id);
  }
  // Separate self-test; NEVER describes a trusted paste.
  async function fixtureSelfTest(url) {
    const response = await fetch(url,{cache:'no-store'});
    if (!response.ok) throw new Error('Fixture HTTP ' + response.status);
    const blob = await response.blob();
    const file = new File([blob],'synthetic.png',{type:'image/png'});
    const id = ++sequence;
    const meta = {source:'synthetic-validation',trusted:false,
      phase:'captured',types:[],items:[],imageItemCount:0,
      getAsFileInvoked:false,getAsFileNull:null,
      shortcutToPasteMs:null,getAsFileElapsedMs:null,
      getAsFileError:null,filesLength:0,
      fileType:file.type,fileSize:file.size,selectionGeneration:null};
    publish(meta,id);await inspect(file,meta,id);return current;
  }
  window.ClipboardImageReceipt = Object.freeze({fixtureSelfTest,
    getLastReport});
  window.addEventListener('keydown',notePasteShortcut,true);
  window.addEventListener('paste',capturePaste,true);
  publish({phase:'waiting'});
})();
