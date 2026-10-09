# Firefox trusted-paste adapter for the frozen xrdp loader

**Status:** offline adapter plus strict INCR ledger, not a production fix. Tests pass here;
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
- `firefox_x11_delivery_ledger.py`: fail-closed X11 requestor/property/generation and exact PNG byte ledger.
- `integrate_frozen_loader.py`: **review-only** frozen-Git-blob-checked source
  transformer; writes a new source file and unified diff; refuses in-place edits.
- `tests/test_delivery_ledger.py`: 17 focused retry, property, generation, INCR completion and timeout tests.
- `tests/test_adapter.py`: 13 Python tests including a real authenticated
  private Xvfb, wrong-cookie rejection, HTTP service and fake W3C WebDriver.
- `tests/test_browser_receipt.cjs` and `tests/synthetic.png`: Node VM test for
  correct File digest/decode, null File, missing crypto and stale async events.

## Local offline tests

```bash
python3 -m unittest discover -s tests -p 'test_*.py' -v
node tests/test_browser_receipt.cjs
python3 -m py_compile firefox_chansrv_consumer.py integrate_frozen_loader.py
```

`pytest` and Firefox are **not** required for these offline tests.
The optional private-Xvfb test needs `Xvfb`, `xauth`, `xdpyinfo`.

## Codex integration (only in an isolated frozen source worktree)

1. Get the frozen source inputs and build the baseline candidate after explicit
   authorization for any missing downloads. Do not use production chansrv.
2. **Preferred GitHub handoff:** check out draft PR #1 branch
   `diagnostics/firefox-chansrv-loader-adapter-20261008` in a separate disposable
   worktree. The loader and companion Python files are already integrated there.
   Do not apply the generator on top of this branch.

   **Alternative for the exact frozen base:** copy the three companion files
   (`firefox_chansrv_consumer.py`, `firefox_x11_delivery_ledger.py`, `receipt.js`)
   alongside the frozen loader, then use `integrate_frozen_loader.py`:

```bash
cp firefox_chansrv_consumer.py "$FROZEN_WORKTREE/tests/"
cp receipt.js "$FROZEN_WORKTREE/tests/"
cp firefox_x11_delivery_ledger.py "$FROZEN_WORKTREE/tests/"
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
  response and each same-generation INCR transaction is acknowledged, its
  requestor/property pair matches, and the logged chunks total the exact
  expected synthetic PNG size. A 350 ms quiet interval precedes cleanup.
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

- The full 8,442-line frozen loader Git blob was fetched through the GitHub
  connector; all four anchors matched, and the opt-in loader source was
  committed on PR #1. The 13+17 offline regressions and bytecode compilation
  passed, but the complete candidate-chansrv runtime has **not** run.
- Need the pinned xrdp 0.10.6.1 and FreeRDP 3.31.0 build artifacts to run
  the complete candidate-chansrv test. No network/download was attempted here.
- Need the actual Firefox 157.0.1/geckodriver host to demonstrate a trusted
  browser event; fake W3C endpoint and Node DOM VM do not establish it.

Only after the real candidate test reproduces `getAsFile() === null` should
we propose a minimal corrective production patch.

## Audit update — per-transfer INCR ledger

Before this update, the Firefox branch checked only a set of X11 requestor
IDs against a set of terminator-ack IDs. That could mistake an unrelated
property/generation or a reused XID for a completed screenshot transfer.
The loader now uses `firefox_x11_delivery_ledger.py` and rejects such logs.
This hardens the **experiment**, not production screenshot delivery.

## 2026-10-08 PNG browser integrity and size-matrix update

**This is diagnostic-only. Screenshot pasting is not yet fixed.** The rebuilt
frozen candidate passed 12/12 Firefox trusted-paste trials with a 1,049,471-byte
synthetic PNG even at 2,550 ms. This new test matrix checks whether the larger
realistically compressed PNG sizes or byte integrity change that outcome.

The browser receipt now:

- Calls `getAsFile()` synchronously in the trusted event.
- Reads its bytes in memory only, with a 64-MiB upper bound.
- Computes SHA-256 using WebCrypto or a bounded pure-JavaScript fallback,
  explicitly reporting `digestMethod=webcrypto|js-fallback`.
- Requires the browser decoder's dimensions to match the PNG IHDR dimensions.
- Prevents an older, asynchronous paste from overwriting a later report.
- Returns `PNG_DIMENSION_MISMATCH` or `PNG_UNEXPECTED_DIMENSIONS` rather than
  `READABLE_PNG_FILE` when decoded dimensions are absent or unexpected.
- Retains the strict requestor/property/generation and exact-byte INCR ledger.

### Verified synthetic fixture identities

| Fixture | Bytes | Decoded dimensions | SHA-256 |
|---|---:|---|---|
| Rebuilt candidate fixture | 1,049,471 | 512 × 512 RGBA | `c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c` |
| Mac-size control, synthetic | 2,286,451 | 1000 × 760 RGB | `cca28eec0cce17ae047221aa3177ed1765ad6d5884b7dc60df2ce3a3ff3a7cf4` |
| Exact previous standalone synthetic | 2,401,598 | 1000 × 800 RGB | `d9b7864e95e934ee999ee333ce9bf86adcf823aaca271634bafb8d8b9d3f6c22` |
| Oversize stress control | 3,241,953 | 1200 × 900 RGB | `ba8246c60e667f7cf553d6369887e7c976d58529faad60977f6681635d07e106` |

The Mac-size control's IDAT contains 2,281,461 bytes; the remaining minor
ancillary chunk padding reaches the precise screenshot-like encoded size.
**The control is not the user's original screenshot** and cannot prove the
original compressed pixels were valid. All fixtures here are synthetic.

### Generate, decode and test offline

The generator and full decoder test require Pillow for Python. Node.js 22+
runs the browser receipt VM tests with an actual zlib PNG-inflate oracle.

```sh
# Work only in the disposable diagnostic build directory
python3 tests/make_png_fixtures.py --output build/fixture-matrix
python3 -m unittest discover -s tests -p 'test_png_fixture_integrity.py' -v
XRDP_CONSOLE_TEST_PNG_FIXTURE_DIR=build/fixture-matrix \
  node tests/test_png_browser_receipt.cjs
```

The exact previous standalone PNG is intentionally **not regenerated under
the same hash**. When the prior synthetic fixture is available locally:

```sh
python3 tests/make_png_fixtures.py --output build/fixture-matrix \
  --existing-standalone /path/to/exact/2401598-byte/synthetic.png
XRDP_CONSOLE_TEST_STANDALONE_PNG=/path/to/exact/2401598-byte/synthetic.png \
  python3 -m unittest discover -s tests -p 'test_png_fixture_integrity.py' -v
```

For candidate-chansrv tests, select the verified fixture without changing
the existing FreeRDP peer or production chansrv. This option works only when
the Firefox consumer is explicitly enabled and rejects any file whose exact
SHA-256 does not match the four permitted **synthetic** identities:

```sh
export XRDP_CONSOLE_TEST_FIREFOX_CONSUMER=1
export XRDP_CONSOLE_TEST_BROWSER_PNG_FIXTURE="$PWD/build/fixture-matrix/mac_size_control_2286451.png"
export XRDP_CONSOLE_TEST_DELAY_PNG_RESPONSE_MS=2550
# Run the existing candidate loader's full binary arguments,
# followed by --clipboard-delayed-png-response.
```

Do not apply the frozen integration generator **on top of** the already
patched diagnostic PR branch. Its five source transformations are reproduced
in the committed PR loader and were verified byte-for-byte against the frozen
source. No production install or activation is authorized.

**Limitations:** Python unit tests and Node VM tests do not establish
Firefox 157.0.1's real browser behavior; full Firefox-through-chansrv
acceptance remains Codex's separately isolated experiment. WebCrypto fallback
runs after the synchronous paste handler, so its execution time does not
accelerate clipboard materialization. Full browser decode of the original
Mac screenshot is still unverified.

### Independent native libpng validation

The four fixture byte-identities above were also passed through the previously
implemented `png_gate_synthetic_cli` native C11/libpng diagnostic, separately
from Pillow and Node. All four reported `envelope=1 decoded=1`:

- 1,049,471 bytes → decoded 512×512.
- 2,286,451 bytes → decoded 1000×760.
- 2,401,598 bytes → decoded 1000×800.
- 3,241,953 bytes → decoded 1200×900.

The native diagnostic CTest `full_idat_differential` passed.
This establishes full native decode of **the synthetic fixtures only**. It
does not decode or validate the original macOS screenshot. No native gate is
introduced in the production clipboard path.

## October 9: trusted paste-stage and generation-anchored evidence

A post-reboot screenshot attempt produced an image-bearing CLIPRDR generation,
verified X11 clipboard owner and successful `TARGETS` request, but **no
correlated image/png or image/bmp X11 request**. Without the user paste timestamp
or Firefox requestor identity, those records cannot establish which browser
paste occurred. Do not infer PNG failure from a TARGETS-only trace.

The diagnostic receipt now logs **only event metadata**: trusted Ctrl/Meta+V
shortcut count, paste-event count, trusted paste-event count, focus/visibility,
and local browser wall-clock timestamps. It does not call navigator.clipboard,
read key text, or access image bytes except within an actual paste handler.

The browser result distinguishes:

- `NO_TRUSTED_SHORTCUT_OBSERVED`: page did not observe the shortcut;
  this does not establish whether a shortcut was sent to a different window.
- `TRUSTED_SHORTCUT_NO_PASTE_EVENT`: page observed a trusted paste shortcut
  but no paste event by the bounded timeout.
- `UNTRUSTED_PASTE_EVENT_ONLY`: an event fired but was not trusted.
- `PASTE_EVENT_NOT_COMPLETED`: a paste event fired, but File inspection
  did not finish by the bounded timeout.
- `NO_IMAGE_PNG_ITEM`: trusted paste completed without an image/png File item.
- `TRUSTED_PASTE_NULL_FILE`: trusted paste exposed the item but getAsFile()
  synchronously returned null.

The X11 metadata analyzer also reports `targets_request_count` and
`targets_response_count` for the announced generation. It reports PNG
`format_data_request_ns` and `response_complete_ns` **only** after a
same-generation image/png X11 request and before the next format list;
format IDs are reused across clipboard generations. Timestamp association is
best-effort metadata, not a proof of browser PID identity or wire-level
request ID.

Offline checks for this change (execute from repository root):

```sh
python3 -B tests/test_firefox_consumer_integrity.py
node tests/test_firefox_receipt_event_stages.cjs
# When approved synthetic PNG fixtures have been generated:
node tests/test_png_browser_receipt.cjs
```

This is a diagnostic-only stacked draft branch. It does not change chansrv,
the Firefox browser, the physical DISPLAY, or an active clipboard. A meaningful
macOS acceptance experiment still requires explicit permission and a genuine,
timestamped user screenshot and trusted Firefox paste; XIDs alone are not
Firefox identification.

### Physical-desktop observation page (not the synthetic Xvfb test)

`tests/firefox_manual_paste_probe.html` is a local `file://` receipt
page for an **explicitly user-initiated** Mac screenshot paste into Firefox
over Windows App. Unlike the Xvfb/WebDriver adapter, it observes the actual
Firefox running on the physical Linux session; it does not connect to
chansrv, attach to another window, or read the clipboard independently.

When a separate real-client trial is authorized, open this file in Firefox
directly, focus its contenteditable box, copy a **fresh non-sensitive**
Mac screenshot, then paste once. Read/copy only the displayed metadata:
paste event, `types`, item kinds/MIME types, `getAsFileNull`, PNG
size/decoding status, focus/visibility and wall-clock event timestamps.
No screenshot image or digest is displayed, uploaded, or saved by the page.
The receipt script holds image bytes only transiently in browser memory while
a genuine paste event is being inspected; it does not poll clipboard contents.

Before or immediately after the trial, record the **exact wall-clock
timestamp** and the target Firefox page. Correlate it with a narrow chansrv
log interval containing generation, X11 requestor/property, `TARGETS` and
image requests, format-data requests and responses, and X11 delivery. The
browser page does **not** know the X11 requestor XID or generation itself,
and wall-clock proximity alone is not sufficient proof of requestor identity.

Do not switch the host checkout, change services, restart xrdp, or run a
live paste as part of the offline PR validation.


### TARGETS offer-level evidence (October 9)

The metadata analyzer now includes `targets_responses` for a single CLIPRDR
format-list generation. Each response records only requestor XID, advertised
X11 atom names, the logged truncation flag, and result status. The aggregate
`png_target_advertised` is **true** only when a successful TARGETS response
actually contains `image/png`; **false** only when successful, complete
responses prove it absent; otherwise it is **null** (unknown).

An image-bearing CLIPRDR format ID does not by itself prove `image/png`
was offered to Firefox. Conversely, an `image/png` TARGETS entry does not
prove Firefox requested or received PNG bytes. These records must still be
correlated with one authorized trusted paste and browser requestor; no X11
target name, ID, or wall-clock proximity establishes browser identity alone.

The reporting is metadata-only: no clipboard data, PNG bytes, hashes, or
screenshot pixels are read or returned by this parser.
