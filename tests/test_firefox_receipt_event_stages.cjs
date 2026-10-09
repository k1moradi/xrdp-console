/* Offline event-stage tests only: no X11, clipboard reads, or PNG payloads. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, 'receipt.js'), 'utf8');

function createPage() {
  const handlers = new Map();
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
    performance: {now: () => 10},
    crypto: {subtle: null},
    fetch: () => { throw Error('test must not fetch the clipboard'); },
    File: class {}, Promise
  };
  context.globalThis = context;
  vm.runInNewContext(source, context, {filename: 'receipt.js'});
  return {handlers, document, receipt: () =>
    window.ClipboardImageReceipt.getLastReport()};
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
  assert.equal(report.getAsFileNull, true);
  assert.equal(typeof report.observer.lastPasteWallMs, 'number');
  assert.equal(report.items[0].type, 'text/plain');

  const nullPage = createPage();
  nullPage.handlers.get('paste')({
    isTrusted: true,
    preventDefault() {},
    clipboardData: {
      types: ['Files'],
      files: [{}],
      items: [{kind: 'file', type: 'image/png',
        getAsFile: () => null}]
    }
  });
  report = nullPage.receipt();
  assert.equal(report.phase, 'complete');
  assert.equal(report.imageItemCount, 1);
  assert.equal(report.getAsFileNull, true);
  assert.equal(report.observer.pasteEvents, 1);
  assert.equal(report.observer.trustedPasteEvents, 1);
  assert.equal(report.observer.trustedPasteShortcuts, 0);

  const other = createPage();
  other.handlers.get('paste')({
    isTrusted: false,
    preventDefault() {},
    clipboardData: {types: [], items: [], files: []}
  });
  report = other.receipt();
  assert.equal(report.observer.pasteEvents, 1);
  assert.equal(report.observer.trustedPasteEvents, 0);

  console.log('PASS: Firefox receipt distinguishes shortcut, paste event, and null File');
}
main();
