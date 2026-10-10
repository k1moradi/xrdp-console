# ScreenGrab / Qt6 versus chansrv: source-level image clipboard comparison

**Scope:** Read-only source comparison and a test-only reference owner for a
future *operator-attested private Xvfb* A/B trial. No real clipboard access,
RDP integration, native Qt build, installed-service change or deployment has
been performed as part of this change.

## Source revisions reviewed

The upstream repositories below are **references**, not proof of the Qt/Firefox
versions installed on the Linux host. Record those local versions before a test.

- LXQt ScreenGrab master at `d1ae5a3bde595a2fd0af23824d4da0a6bc534275`:
  [`src/core/core.cpp:635-639`](https://github.com/lxqt/screengrab/blob/d1ae5a3bde595a2fd0af23824d4da0a6bc534275/src/core/core.cpp#L635-L639).
- QtBase dev at `86d3568c39d5008b58b29ad62f11e70bc6c42c49`:
  [`qinternalmimedata.cpp:127-139,161-201`](https://github.com/qt/qtbase/blob/86d3568c39d5008b58b29ad62f11e70bc6c42c49/src/gui/kernel/qinternalmimedata.cpp#L127-L201),
  [`qxcbclipboard.cpp:298-342,407-466,499-620`](https://github.com/qt/qtbase/blob/86d3568c39d5008b58b29ad62f11e70bc6c42c49/src/plugins/platforms/xcb/qxcbclipboard.cpp#L407-L466),
  [`qxcbmime.cpp:48-123`](https://github.com/qt/qtbase/blob/86d3568c39d5008b58b29ad62f11e70bc6c42c49/src/plugins/platforms/xcb/qxcbmime.cpp#L48-L123).
- xrdp-console candidate `b0ca7f59b2c8ef389cad5f18b33d06c8ab375401`:
  [patch 0028](https://github.com/k1moradi/xrdp-console/blob/b0ca7f59b2c8ef389cad5f18b33d06c8ab375401/patches/xrdp/0028-xrdp-console-resize-fail-closed-and-clipboard-images.patch),
  [patch 0036](https://github.com/k1moradi/xrdp-console/blob/b0ca7f59b2c8ef389cad5f18b33d06c8ab375401/patches/xrdp/0036-xrdp-chansrv-prefer-png-target.patch),
  [patch 0039](https://github.com/k1moradi/xrdp-console/blob/b0ca7f59b2c8ef389cad5f18b33d06c8ab375401/patches/xrdp/0039-xrdp-chansrv-log-targets-response.patch),
  [patch 0053](https://github.com/k1moradi/xrdp-console/blob/b0ca7f59b2c8ef389cad5f18b33d06c8ab375401/patches/xrdp/0053-xrdp-chansrv-qt-like-x11-owner-contract.patch).
- Browser reference: Mozilla
  [`widget/gtk/nsClipboard.cpp`, `HasSuitableData()`](https://searchfox.org/firefox-main/source/widget/gtk/nsClipboard.cpp#1268-1369)
  and [image fetch](https://searchfox.org/firefox-main/source/widget/gtk/nsClipboard.cpp#1593-1624).
  The upstream `widget.gtk.clipboard_timeout_ms` default is 1000 ms at the
  reviewed Mozilla revision; do **not** assume the host has identical behavior.

## Exact behavior difference matrix

| Stage | ScreenGrab / Qt6 reference | Patched chansrv | Interpretation |
|---|---|---|---|
| Copy API | `QApplication::clipboard()->setPixmap(*_pixelMap, QClipboard::Clipboard)` | Remote CLIPRDR Format List installs X11 CLIPBOARD owner | Local Qt has a live pixmap; chansrv may defer image bytes until a remote format request |
| Selection owner | Qt owns CLIPBOARD with an X11 timestamp and XFixes notifications | Chansrv owns CLIPBOARD with generation tracking and deferred owner restoration | An owner-change or stale-generation race remains possible; owner installation alone does not prove Firefox consumed this generation |
| `TARGETS` | Qt's `QInternalMimeData::formatsHelper()` expands `application/x-qt-image` into locally writable image formats, including PNG where supported | Patch 0028 advertises PNG when offered by the peer and BMP when DIB is offered; patch 0036 prefers PNG order | **Real difference**, but offering other codecs is not yet proven necessary for Firefox |
| `MULTIPLE`/`SAVE_TARGETS` | Qt advertises and implements both | Patch 0053 intentionally omits `MULTIPLE` because delayed CLIPRDR cannot safely fulfill concurrent conversions; does not advertise `SAVE_TARGETS` | Do not copy Qt capability claims without matching semantics |
| `property=None` and timestamps | Qt substitutes target as property for an obsolete requestor, rejects stale requests | Patch 0053 implements comparable property substitution and wrapped timestamp checks | These specific Qt-like changes already exist; not proof of full parity |
| Image request | Qt can encode requested `image/png` from the local pixmap synchronously | Chansrv may issue CLIPRDR Format Data Request and then INCR delivery | **Timing hypothesis:** Firefox may stop waiting before delayed bytes become a usable File |
| Firefox image discovery | Mozilla filters X11 `TARGETS` by MIME name, then fetches an image flavor | Existing generation-5 TARGETS had 4 atoms, with two names unresolved in old logging | The old numeric atoms do not establish whether `image/png` was present |
| Browser receipt | Trusted `paste` needs `kind=file`, readable `getAsFile()`, valid PNG and UI acceptance | Previous tests showed either no image item or a null File, in different experiments | Must not merge these two observed outcomes into one root cause |

**PNG byte-identity caveat:** ScreenGrab calls `QClipboard::setPixmap()`,
not `setMimeData()` with the original encoded PNG. Qt may *re-encode*
the pixmap using QImageWriter when Firefox requests `image/png`. The
output can be pixel-equivalent but have a different SHA-256, compressed
size, or PNG chunk layout.

Use **different acceptance rules** for the two matched private Xvfb legs:

- **A / Qt6 owner**: verify trusted Firefox paste, `kind=file`,
  `type=image/png`, non-null `getAsFile()`, bounded File, matching
  File/readback byte counts, valid digest shape, PNG signature, successful
  decode, and expected dimensions. The helper already verifies the
  original synthetic PNG input's digest. The browser File's encoded
  digest and size need **not** equal the source fixture. This proves
  Firefox can accept a Qt image, *not* a pixel-by-pixel identity hash.
- **B / chansrv owner**: require the same and verify exact encoded PNG
  size and SHA-256 agreement with the CLIPRDR synthetic fixture.

`run_firefox_chansrv_timing(..., require_exact_png_encoding=False)`
selects the Qt control's structural validation and isolated
`firefox-qt6-control` profile directory.
The default `require_exact_png_encoding=True` retains strict remote
byte identity and the separate `firefox-consumer` profile directory.
The same already-attested private Xvfb and input image are required for
both cases; the owner must be sequentially replaced by the operator.

The reviewed Mozilla source explicitly matches X11 target names for image
flavors, so `image/png` is the **first format to verify**. Adding
`application/x-qt-image`, JPEG, GIF or `SAVE_TARGETS` to chansrv without
serving them correctly would make its advertisement misleading. Likewise,
`MULTIPLE` requires real concurrent conversion handling rather than an atom.

## New test-only Qt6 owner

`tests/helpers/qt6_screengrab_clipboard_owner.cpp` mirrors the actual
ScreenGrab `setPixmap()` call with an **existing allowlisted synthetic PNG**.
It does not take screenshots, inspect clipboard contents, or touch an existing
clipboard. The Qt event loop owns the QMimeData for a bounded 45-second period.
The helper accepts only the four SHA-256/size/dimension identities already
allowlisted by `tests/firefox_chansrv_consumer.py`.

Before `QApplication` initializes and can connect to X11, it rejects:

- displays outside private `:191`–`:249` slots;
- non-`xcb` Qt backends or inherited Wayland display;
- missing or unowned existing canonical `.release` directory;
- XAUTHORITY outside that root, an invalid authority, or one not owned by caller;
- absent/oversized/nonallowlisted PNG files outside that same root.

**Important limitation:** The executable cannot independently attest its Xvfb
PID. It must **only** be launched by an operator-controlled private Xvfb
harness which has already checked the exact process identity, command line,
MIT-MAGIC-COOKIE, `-nolisten tcp`, and rollback. Reuse
`verify_isolated_xvfb()` in `tests/firefox_chansrv_consumer.py`; never
execute the helper against the physical display or arbitrary X server.

If the *already installed* Qt6 development packages are available and the
operator has verified the existing workspace, a **build-only** invocation is:

```sh
# Run in the candidate checkout; do not build into a dated directory.
test -d "$XRDP_CONSOLE_RELEASE_ROOT" || exit 1
test "${XRDP_CONSOLE_RELEASE_ROOT##*/}" = ".release" || exit 1
c++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -O2 \
  tests/helpers/qt6_screengrab_clipboard_owner.cpp \
  $(pkg-config --cflags --libs Qt6Widgets) \
  -o "$XRDP_CONSOLE_RELEASE_ROOT/qt6-screengrab-clipboard-owner"
```

This documentation is **not permission to run the owner, Firefox, chansrv or
private RDP**. No Qt6 compiler/development headers were available to this
ChatGPT analysis environment, so native Qt compilation is still unverified.

## One matched, authorized A/B experiment

After explicit authorization, the Codex host operator should use the **same**
authenticated private source Xvfb, Firefox binary/profile configuration,
trusted-paste page, approved synthetic image, contenteditable target and
keyboard shortcut across two sequential cases. Never run two clipboard owners
at once, and create a fresh clipboard generation for each case.

**A — Qt6/ScreenGrab reference.** Use the test-only helper. Record X11
CLIPBOARD owner, timestamp, the exact `TARGETS` atoms and order, Firefox's
trusted-paste event, item kind/types, synchronous `getAsFile()` result,
byte count/digest and PNG decode metadata. No image bytes in logs.

**B — Existing patched chansrv.** Use the same approved synthetic PNG through
the isolated CLIPRDR test peer. Record generation, owner, the corrected PR #27
TARGETS names/IDs, Firefox requestor XID, X11 target/property,
SelectionNotify/INCR completion, request-to-response timing and matching
trusted-paste receipt.

### Fail-closed log attribution

Use `correlate_metadata(chansrv_log, peer_log, format_id,
expected_generation, receipt["classification"])` followed by
`diagnose_clipboard_boundary(stages)` to identify the first **observed**
failure boundary. Correlating the global TARGETS count with a browser
receipt alone does not identify the X11 requestor: without independent
Firefox requestor attestation, the classifier returns
`REQUESTOR_IDENTITY_NOT_ATTESTED`. A separately verified Firefox XID
may be supplied as `attested_browser_requestor="0x..."`; this still does
not exclude other browser windows.

The correlator now uses exact numeric CLIPRDR format IDs (not prefix
substrings), refuses to assign duplicate peer timestamps to one paste,
and reports matched `png_x11_argument_issue` only for the same XID,
property and generation. Hash agreement at that stage means the PNG
**arguments were issued** to XChangeProperty: no X11 receipt, GTK decode,
or Firefox File acceptance is inferred from it. No new service logging,
clipboard payload capture, or runtime actions are required for this
offline evidence check.

Interpret **the first demonstrated difference**, in this order:

1. If Firefox receives no trusted paste event in one run, fix the event/focus
   precondition; it is not evidence of an image-format failure.
2. If `image/png` is not offered in the complete, resolved B TARGETS while A
   offers it, investigate format advertisement or clipboard generation.
3. If PNG is offered but Firefox never requests it in B, examine cached
   TARGETS, owner-change timing and Firefox's chosen flavor, rather than
   changing CLIPRDR bytes.
4. If Firefox requests PNG in both and B's SelectionNotify/data arrives after
   a timeout while A is prompt, test the delayed-render timing hypothesis
   without relaxing Firefox's timeout as the production remedy.
5. If complete image delivery arrives in time in B but Firefox has no usable
   File, investigate image validity, browser File construction and consumer
   semantics before changing the transport.

The gate remains: **fresh real Mac screenshot → trusted Firefox image File →
valid PNG decode → intended UI acceptance**, with no text regression. This
A/B diagnostic does not satisfy that final acceptance by itself.

## Safety and reporting

Do not touch physical `:0`, the live user clipboard or existing Firefox.
Do not install/restart xrdp/chansrv, merge a PR, alter pinned dependencies or
remove agent worktrees. Build under the existing `.release` only. Report
precise source SHAs, host versions, commands, case metadata, first failing
boundary and cleanup; preserve no clipboard payloads.
