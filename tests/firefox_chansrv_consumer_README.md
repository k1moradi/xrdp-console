# Firefox trusted-paste adapter for the frozen xrdp loader

**Status:** offline adapter implementation, not a production fix. Tests pass here;
**the full Firefox-through-chansrv integration remains UNEXECUTED** pending the
frozen xrdp / FreeRDP candidate on the Codex host.

## Why this exists

The real macOS screenshot paste produced a trusted Firefox `paste` with an
`image/png` item but `getAsFile() === null`. An isolated X11 owner reproduced
that symptom after ~2.1 seconds even when X11 INCR later completed. The frozen
`tests/test_xrdp_loader.py` already exercises delayed Format Data Responses
via the synthetic FreeRDP CLIPRDR peer, but uses a non-Firefox X11 requestor.
This adapter switches only that consumer to a real WebDriver Ctrl+V paste,
while retaining the existing synthetic peer and default deterministic test.

The existing receipt script is adapted from the independent DOM receipt probe.
It runs `getAsFile()` synchronously inside the **real trusted paste handler**;
reads the File asynchronously, computes SHA-256, checks signature and fully
attempts browser PNG decoding. File bytes never leave the browser except as
metadata and digest. Any pasted image is synthetic.

## Source and provenance

- Frozen `k1moradi/xrdp-console` commit:
  `0ee616424b3290b50c2aec48491918eb1076916c`
- Required loader Git blob:
  `34ce4a5c0ea97634f253a687bfc6c461c3061b50`
- Existing frozen loader function:
  `assert_clipboard_delayed_png_response_session()` around line 3378.
- Existing synthetic peer:
  `tests/helpers/freerdp_cliprdr_overlap_peer.cpp`.
- Reuses `XRDP_CONSOLE_TEST_DELAY_PNG_RESPONSE_MS` without modifying peer.

## Contents

- `firefox_chansrv_consumer.py`: authenticated-Xvfb Firefox adapter, W3C
  WebDriver client, trusted Ctrl+V, browser receipt, metadata correlation.
- `receipt.js`: synchronous DOM File capture, asynchronous SHA-256 and decode.
- `integrate_frozen_loader.py`: **review-only** frozen-Git-blob-checked source
  transformer; writes a new source file and unified diff; refuses in-place edits.
- `tests/test_adapter.py`: 13 Python tests including a real authenticated
  private Xvfb, wrong-cookie rejection, HTTP service and fake W3C WebDriver.
- `tests/test_browser_receipt.cjs` and `tests/synthetic.png`: Node VM test for
  correct File digest/decode, null File, missing crypto and stale async events.

## Local offline tests

```bash
python3 -m unittest discover -s tests -p test_adapter.py -v
node tests/test_browser_receipt.cjs
python3 -m py_compile firefox_chansrv_consumer.py integrate_frozen_loader.py
```

`pytest` and Firefox are **not** required for these offline tests.
The optional private-Xvfb test needs `Xvfb`, `xauth`, `xdpyinfo`.

## Codex integration (only in an isolated frozen source worktree)

1. Get the frozen source inputs and build the baseline candidate after explicit
   authorization for any missing downloads. Do not use production chansrv.
2. In a new detached build worktree at frozen commit, copy the two adapter files:

```bash
cp firefox_chansrv_consumer.py "$FROZEN_WORKTREE/tests/"
cp receipt.js "$FROZEN_WORKTREE/tests/"
python3 integrate_frozen_loader.py \
  --source "$FROZEN_WORKTREE/tests/test_xrdp_loader.py" \
  --output "$FROZEN_WORKTREE/build/test_xrdp_loader.firefox.py" \
  --patch "$FROZEN_WORKTREE/build/firefox-loader-review.patch"
python3 -m py_compile "$FROZEN_WORKTREE/build/test_xrdp_loader.firefox.py"
git -C "$FROZEN_WORKTREE" apply --check build/firefox-loader-review.patch
# After code review, apply only in this isolated worktree:
git -C "$FROZEN_WORKTREE" apply build/firefox-loader-review.patch
```

**NOTE:** The generator output and patch are review artifacts. The adapter itself
must reside in `tests/`, next to the patched loader, for the Python import to
resolve. Do not execute the standalone output from an unrelated directory as
though it had the repository's original relative paths.

3. Run the previously documented loader's *existing* required binary arguments.
   The browser option is an **environment flag**, not a new positional argument:

```bash
export XRDP_CONSOLE_TEST_FIREFOX_CONSUMER=1
export XRDP_CONSOLE_TEST_FIREFOX_BINARY=/path/to/verified/firefox
export XRDP_CONSOLE_TEST_GECKODRIVER=/path/to/verified/geckodriver
export XRDP_CONSOLE_TEST_DELAY_PNG_RESPONSE_MS=2100
python3 "$FROZEN_WORKTREE/tests/test_xrdp_loader.py" \
  "$MODULE" "$XRDP" "$INSTALL_ROOT" "$FREERDP" "$PIXEL_PROBE" \
  "$STIMULUS" "$CLIPBOARD_HELPER" "$OVERLAP_PEER" \
  --clipboard-delayed-png-response
```

Required parameters are the same eight positions as the frozen loader test,
followed by the existing sole clipboard mode at the end. Use the binaries and
paths from the isolated candidate build; never substitute a production path.

4. Execute 500, 1800, 2100, and 2550 ms with three independent fresh-loader
   runs each. Each run creates its own private authenticated Xvfb, synthetic
   clipboard generation, and disposable Firefox profile.

The default deterministic loader remains unchanged whenever
`XRDP_CONSOLE_TEST_FIREFOX_CONSUMER` is unset.

## Evidence and safety boundaries

- A successful WebDriver /actions key sequence is **not** treated as a trusted
  paste unless the DOM reports `trusted=true` with image/png file item.
- The adapter reports `TRUSTED_PASTE_NULL_FILE` separately from no event,
  File-read failure, SHA failure, invalid PNG or a readable decoded PNG.
- The Firefox process stays alive until the delayed synthetic peer sends its
  response and each observed same-generation INCR requestor is acknowledged.
- X11 requestor XIDs are *not* automatically attributed to Firefox PID.
- Stage timestamps are only reported where existing chansrv/peer logs supply
  monotonic evidence. Current RDP VC first/last-fragment diagnostic lines lack
  monotonic times; the adapter returns `null`, not invented values.
- Xvfb authentication is added only to the opt-in Firefox mode; wrong cookie
  rejected in local integration testing. Other loader modes are unaffected.
- Browser profiles and stdout stay in the loader's disposable test directory;
  the browser does not persist image pixels. The loopback server serves only
  a static contenteditable page and receipt JS.
- No installed xrdp executable, service, live DISPLAY, real screenshot or
  protected branch is accessed by the adapter code.

## Remaining gates

- Need a local copy of the **exact frozen loader Git blob** to execute the
  full `integrate_frozen_loader.py` generation against all 8,442 lines.
  Source anchors and frozen blob were independently verified from GitHub,
  and a syntactically valid local miniature was exercised; this is **not** a
  substitute for a full frozen-file integration build.
- Need the pinned xrdp 0.10.6.1 and FreeRDP 3.31.0 build artifacts to run
  the complete candidate-chansrv test. No network/download was attempted here.
- Need the actual Firefox 157.0.1/geckodriver host to demonstrate a trusted
  browser event; fake W3C endpoint and Node DOM VM do not establish it.

Only after the real candidate test reproduces `getAsFile() === null` should
we propose a minimal corrective production patch.
