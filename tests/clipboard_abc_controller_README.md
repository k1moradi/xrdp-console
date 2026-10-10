# Firefox clipboard A/B/C: offline-only controller

**Status: BLOCKED. No live execution backend or --execute escape hatch.**
This change does not run Xvfb, Firefox, Qt, chansrv, xrdp or a clipboard peer.

## Source and test identities

A — Qt ScreenGrab QClipboard::setPixmap(), default Qt owner mode.
B — Qt QMimeData with only the same encoded image/png bytes, --png-only mode.
C — patched chansrv delayed CLIPRDR image/png owner.

The Qt/test source is stacked on draft PR #34. The corrected chansrv
patch series is **separate**: draft PR #32, commit
447447ff68fe34cf2391ca9c908c4bd993ed4ef2. Building chansrv from
the Qt test branch must never substitute for this corrected patch source.

Approved synthetic input: 2,401,598 PNG bytes, 1000x800,
SHA-256 d9b7864e95e934ee999ee333ce9bf86adcf823aaca271634bafb8d8b9d3f6c22.

## New Qt owner control protocol

Existing invocation remains:

    qt6-screengrab-clipboard-owner [--png-only] approved.png

The original invocation remains limited to 45 seconds. A future controller
may use:

    qt6-screengrab-clipboard-owner --controlled [--png-only] approved.png

After preflight, approved PNG verification and successful clipboard
ownership, the process writes XRDP_CONSOLE_QT_OWNER_READY followed by
a newline to stdout and flushes. Controlled mode fails before Qt/X11
initialization unless stdin is a dedicated FIFO pipe, and then watches it:
byte q or EOF requests clean event-loop exit. Unexpected input also quits.
The new mode has a separate 180-second hard maximum. The controller must
attest the spawned process, private Xvfb PID/cookie, and generation independently:
a READY line is not X11 requestor identity evidence.

The updated C++23 source has NOT been compiled on the Linux host;
the previous successful Qt native build applied to an older source.

## Read-only manifest policy

Run only the offline planner:

    python3 -B tests/clipboard_abc_controller.py --manifest path/to/manifest.json

It reads an existing manifest and owned local artifacts. It never writes,
executes, accesses a display or opens a socket, and ALWAYS returns exit
status 2 even if local validation passes. It accepts no --execute option.

Required JSON keys: schema=1; release_root (existing caller-owned .release);
run_root (owned mode-0700 private run directory); display (:191 to :249);
xauthority (owned private mode-0600 file); home (private directory);
socket_dir (owned private mode-0700 directory); fixture (verified PNG);
rdp_listener="disabled"; private_binaries containing exactly qt_owner
and chansrv with absolute inside-.release paths, expected SHA-256
and pinned 40-hex source commits. Chansrv source must be the PR #32 commit
listed above. Paths with symlinks, parent traversal and shared sockets
outside the run directory are rejected. Physical DISPLAY is never inherited.

The proposed child environment is built from an explicit allowlist and
contains only PATH, LANG, HOME, DISPLAY, XAUTHORITY, QT_QPA_PLATFORM and
XRDP_CONSOLE_RELEASE_ROOT. It does not copy the caller's physical display,
session bus, Wayland variables, XDG runtime paths or LD_LIBRARY_PATH.

Manifest validation does not prove a complete ELF dependency closure
or that a private X11 process is running. It intentionally reports:
authenticated Xvfb PID/socket/cookie attestation; private chansrv library
and IPC readiness; independent Qt/chansrv TARGETS observation; verified
Firefox requestor PID identity; and explicit authorization as **unmet gates**.

## Test-only lifecycle core

CaseCoordinator accepts injectable OwnerPort and BrowserPort interfaces
without any OS spawn mechanism. Mock-only tests verify:

- Exact sequential A -> B -> C with fresh increasing generations
- No overlapping selection owners or concurrent cases
- Owner readiness and liveness before/after browser startup
- Bounded browser trusted paste and receipt
- Browser and owner cleanup on startup errors and missing receipts
- Permanent blocking of later legs if cleanup fails or a child remains alive
- Paste-event evidence never mislabeled as PNG File acceptance
- Complete trusted events are classified by the **existing Firefox receipt
  validator**, including synchronous getAsFile state, actual File MIME,
  complete File read, digest shape, PNG signature, decode and image dimensions.
- A/B Qt legs accept valid images despite legitimate PNG re-encoding.
  Chansrv C additionally requires the exact approved encoded size and SHA-256.
- An untrusted or synthetic event, or a missing complete paste, invalidates
  the experiment rather than merely advancing to the next owner.
- A failed ScreenGrab A reference prevents a misleading B/C comparison.
  A B or C File failure is retained as a negative result, not a false success.

The separate compare_file_acceptance() helper only accepts three ordered,
strictly increasing, complete-case results. It reports **hypotheses**, not
assertions about unmeasured CLIPRDR delays or X11 requestor identity:

| A pixmap | B Qt PNG-only | C chansrv | Evidence interpretation |
|---|---|---|---|
| Accepted | Accepted | Rejected | Remote owner/delivery path suspect |
| Accepted | Rejected | Rejected | Qt pixmap MIME conversion/offer suspect |
| Accepted | Accepted | Accepted | Approved synthetic Firefox PNG File route works |
| Rejected | Any | Any | Baseline invalid; stop further case execution |
| Accepted | Rejected | Accepted | Mixed results; inconclusive |

These results remain offline **until** a separately authorized controller
actually supplies browser receipts. They do not prove macOS screenshot paste,
RDP integration, Firefox requestor attribution, or intended UI acceptance.

There is deliberately no private chansrv runtime adapter until Codex
provides verified startup, private library closure, sockets and rollback.

## Known host blockers — separate proof required

Codex's 2026-10-10 readiness report says the old loader can reuse a physical
display, omits chansrv XAUTHORITY, checks shared /run/xrdp/sockdir and
does not establish a loopback-only listener. The existing chansrv binary
resolves protected-prefix libraries; an alternate build has nonprivate
absolute RUNPATH. A shared owner/browser/peer controller and reliable Firefox
requestor attribution do not yet exist on that host.

Do NOT use the historical loader as an adapter. No private Xvfb trial, real
clipboard read, GUI browser or synthetic remote peer may be started without
a separate authorization. All output is metadata-only. Even a future
synthetic A/B/C success would not establish real Mac screenshot acceptance.

## Offline tests

The new clipboard-abc-controller-safety-unit CTest uses mocks and fixture
stubs; it does not launch software. It is registered alongside the existing
qt6-screengrab-owner-integrity-unit and firefox-consumer-classification-unit.
