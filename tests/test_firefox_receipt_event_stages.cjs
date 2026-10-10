/* Offline event-stage tests only: no X11, clipboard reads, or PNG payloads. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, 'receipt.js'), 'utf8');

function createPage() {
  const handlers = new Map();
  let nowMs = 10;
  const editor = {};
  const document = {
    activeElement: editor,
    visibilityState: 'visible',
    hasFocus: () => true,
    getElementById: name => name === 'editor' ? editor : null
  };
  const window = {
    addEventListener: (name, handler) => handlers.set(name, handler)
  };
  const context = {
    window, document, Date, Uint8Array, Uint32Array, DataView, Array, Number,
    performance: {now: () => nowMs},
    crypto: {subtle: null},
    fetch: () => { throw Error('test must not fetch the clipboard'); },
    File: class {}, Promise
  };
  context.globalThis = context;
  vm.runInNewContext(source, context, {filename: 'receipt.js'});
  return {handlers, document,
    advanceClock(ms) {
      assert(Number.isFinite(ms) && ms >= 0);
      nowMs += ms;
    },
    receipt: () => window.ClipboardImageReceipt.getLastReport()};
}

function main() {
  const page = createPage();
  let report = page.receipt();
  assert.equal(report.phase, 'waiting');
  assert.equal(report.observer.pasteEvents, 0);
  assert.equal(report.observer.trustedPasteShortcuts, 0);
  assert.equal(report.observer.editorFocused, true);
  assert.equal(report.observer.documentHasFocus, true);

  page.handlers.get('keydown')({
    isTrusted: false, ctrlKey: true, metaKey: false,
    code: 'KeyV', key: 'v'
  });
  page.handlers.get('keydown')({
    isTrusted: true, ctrlKey: false, metaKey: false,
    code: 'KeyV', key: 'v'
  });
  assert.equal(page.receipt().observer.trustedPasteShortcuts, 0);

  page.handlers.get('keydown')({
    isTrusted: true, ctrlKey: true, metaKey: false,
    code: 'KeyV', key: 'v'
  });
  report = page.receipt();
  assert.equal(report.phase, 'waiting');
  assert.equal(report.observer.trustedPasteShortcuts, 1);
  assert.equal(report.observer.pasteEvents, 0);
  assert.equal(typeof report.observer.lastShortcutWallMs, 'number');

  page.advanceClock(275);
  let prevented = false;
  page.handlers.get('paste')({
    isTrusted: true,
    preventDefault() { prevented = true; },
    clipboardData: {
      types: ['text/plain'],
      items: [{kind: 'string', type: 'text/plain'}],
      files: []
    }
  });
  report = page.receipt();
  assert.equal(prevented, true);
  assert.equal(report.phase, 'complete');
  assert.equal(report.observer.pasteEvents, 1);
  assert.equal(report.observer.trustedPasteEvents, 1);
  assert.equal(report.imageItemCount, 0);
  assert.equal(report.getAsFileInvoked, false);
  assert.equal(report.getAsFileNull, null);
  assert.equal(report.shortcutToPasteMs, 275);
  assert.equal(report.getAsFileElapsedMs, null);
  assert.equal(typeof report.observer.lastPasteWallMs, 'number');
  assert.equal(report.items[0].type, 'text/plain');

  const nullPage = createPage();
  let nullCalls = 0;
  nullPage.handlers.get('paste')({
    isTrusted: true,
    preventDefault() {},
    clipboardData: {
      types: ['Files'],
      files: [{}],
      items: [{kind: 'file', type: 'image/png',
        getAsFile: () => {
          nullCalls++; nullPage.advanceClock(87.25); return null;
        }}]
    }
  });
  report = nullPage.receipt();
  assert.equal(report.phase, 'complete');
  assert.equal(report.imageItemCount, 1);
  assert.equal(nullCalls, 1);
  assert.equal(report.getAsFileInvoked, true);
  assert.equal(report.getAsFileNull, true);
  assert.equal(report.shortcutToPasteMs, null);
  assert.equal(report.getAsFileElapsedMs, 87.25);
  assert.equal(report.observer.pasteEvents, 1);
  assert.equal(report.observer.trustedPasteEvents, 1);
  assert.equal(report.observer.trustedPasteShortcuts, 0);

  const exceptionPage = createPage();
  let exceptionCalls = 0;
  exceptionPage.handlers.get('paste')({
    isTrusted: true,
    preventDefault() {},
    clipboardData: {
      types: ['Files'], files: [],
      items: [{kind: 'file', type: 'image/png',
        getAsFile: () => {
          exceptionCalls++;
          exceptionPage.advanceClock(43.125);
          throw new TypeError('synthetic offline exception');
        }}]
    }
  });
  report = exceptionPage.receipt();
  assert.equal(exceptionCalls, 1);
  assert.equal(report.getAsFileInvoked, true);
  assert.equal(report.getAsFileNull, true);
  assert.equal(report.getAsFileError, 'getAsFile:TypeError');
  assert.equal(report.shortcutToPasteMs, null);
  assert.equal(report.getAsFileElapsedMs, 43.125);

  const filePage = createPage();
  let fileCalls = 0;
  filePage.handlers.get('paste')({
    isTrusted: true,
    preventDefault() {},
    clipboardData: {
      types: ['Files'], files: [{}],
      items: [{kind: 'file', type: 'image/png',
        getAsFile: () => {
          fileCalls++;
          return {size: 8, type: 'image/png',
            arrayBuffer: async () => new ArrayBuffer(8)};
        }}]
    }
  });
  report = filePage.receipt();
  assert.equal(fileCalls, 1);
  assert.equal(report.getAsFileInvoked, true);
  assert.equal(report.getAsFileNull, false);
  assert.equal(report.fileType, 'image/png');
  // Byte integrity is handled by the async stage and classifier, not by
  // successful synchronous construction of an in-memory File-like object.

  // The same keyboard shortcut must never be attributed to a second
  // paste event; the observer consumes its event-pairing candidate.
  page.advanceClock(5);
  page.handlers.get('paste')({
    isTrusted: true, preventDefault() {},
    clipboardData: {types: [], items: [], files: []}
  });
  report = page.receipt();
  assert.equal(report.shortcutToPasteMs, null);

  // An untrusted synthetic keydown or paste must not create the timing
  // of an authenticated user/WebDriver paste. Durations stay same-page.
  const untrusted = createPage();
  untrusted.handlers.get('keydown')({
    isTrusted: false, ctrlKey: true, code: 'KeyV', key: 'v'
  });
  untrusted.advanceClock(500);
  untrusted.handlers.get('paste')({
    isTrusted: true, preventDefault() {},
    clipboardData: {types: [], items: [], files: []}
  });
  assert.equal(untrusted.receipt().shortcutToPasteMs, null);

  const other = createPage();
  other.handlers.get('paste')({
    isTrusted: false,
    preventDefault() {},
    clipboardData: {types: [], items: [], files: []}
  });
  report = other.receipt();
  assert.equal(report.observer.pasteEvents, 1);
  assert.equal(report.observer.trustedPasteEvents, 0);

  console.log('PASS: Firefox receipt distinguishes absent PNG, invoked null, exception and File');
}
main();
