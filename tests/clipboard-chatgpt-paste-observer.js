/*
 * Paste this into Firefox DevTools Console on the ChatGPT tab before testing.
 * It observes trusted DOM paste events without cancelling or modifying them.
 * It logs file metadata and SHA-256 only; clipboard bytes are never uploaded.
 */
(() => {
  "use strict";

  const key = "__xrdpClipboardPasteObserver";
  const previous = window[key];
  if (previous && typeof previous.remove === "function") {
    previous.remove();
  }

  const events = [];
  let sequence = 0;

  const listener = (event) => {
    const clipboard = event.clipboardData;
    const items = clipboard ? Array.from(clipboard.items) : [];
    const fileEntries = items.flatMap((item, itemIndex) => {
      if (item.kind !== "file") {
        return [];
      }

      const file = item.getAsFile();
      if (!file) {
        return [{ itemIndex, file: null, unavailable: true }];
      }

      return [{
        itemIndex,
        file,
        name: file.name,
        type: file.type,
        size: file.size,
        sha256: "pending",
      }];
    });

    const record = {
      sequence: ++sequence,
      wallTime: new Date().toISOString(),
      monotonicMs: Number(performance.now().toFixed(3)),
      trusted: event.isTrusted,
      target: event.target instanceof Element
        ? `${event.target.tagName.toLowerCase()}${event.target.id ? `#${event.target.id}` : ""}`
        : String(event.target),
      types: clipboard ? Array.from(clipboard.types) : [],
      items: items.map((item) => ({ kind: item.kind, type: item.type })),
      files: fileEntries.map(({ file: _file, ...metadata }) => metadata),
    };

    events.push(record);
    console.info("XRDP_PASTE_OBSERVER_EVENT", JSON.stringify(record));

    for (const fileEntry of fileEntries) {
      if (!fileEntry.file) {
        continue;
      }

      fileEntry.file.arrayBuffer().then(async (buffer) => {
        const digest = await globalThis.crypto.subtle.digest(
          "SHA-256", buffer);
        fileEntry.sha256 = Array.from(new Uint8Array(digest),
          (byte) => byte.toString(16).padStart(2, "0")).join("");
        console.info("XRDP_PASTE_OBSERVER_FILE", JSON.stringify({
          sequence: record.sequence,
          itemIndex: fileEntry.itemIndex,
          name: fileEntry.name,
          type: fileEntry.type,
          size: fileEntry.size,
          sha256: fileEntry.sha256,
        }));
      }).catch((error) => {
        fileEntry.sha256 = `error:${String(error)}`;
        console.info("XRDP_PASTE_OBSERVER_FILE", JSON.stringify({
          sequence: record.sequence,
          itemIndex: fileEntry.itemIndex,
          error: String(error),
        }));
      });
    }
  };

  window.addEventListener("paste", listener, true);
  window[key] = {
    events,
    remove() {
      window.removeEventListener("paste", listener, true);
      delete window[key];
    },
  };

  console.info("XRDP_PASTE_OBSERVER_ARMED", {
    instructions: "Click the ChatGPT composer and paste normally; observer does not preventDefault.",
  });
})();
