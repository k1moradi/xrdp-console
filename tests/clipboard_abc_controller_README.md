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

## C-leg requires a private RDP endpoint: offline schema 2

The former schema-1 manifest remains available as a **legacy, offline
planner** with `rdp_listener="disabled"`. It cannot describe, run or
validate a chansrv C-leg. Codex confirmed a synthetic CLIPRDR offer needs
a genuinely private xrdp endpoint forwarding a channel to an externally
started private chansrv via `chansrvport=DISPLAY(...)`.

Schema 2 adds a mandatory `private_rdp_endpoint` to the existing manifest,
and requires `rdp_listener="private-loopback-unverified"`. The endpoint
contains these strictly checked fields:

- `bind_address="127.0.0.1"` and nonprivileged integer `port`:
  the **requested** bind, NOT proof of the listener's actual kernel address
- `session_route="external-chansrv"`: deliberately refuses to invent
  a sesman session
- `client_display`: a separate :191–:249 Xvfb display, distinct from
  source `display`, with its own private `client_xauthority`
- `config_path` and `config_sha256`: an existing, pinned private
  xrdp config under the owned run directory; the helper does **not**
  assert its listener URI syntax has been verified by native xrdp
- `chansrvport`: exact DISPLAY(source-display-number,current-uid) route
- `artifacts`: xrdp, module, peer and rdp_client, each with an
  existing private path, SHA-256 and 40-hex source-commit identity;
  xrdp is pinned to corrected PR #32's source identity

The source controller validates those shapes but **never launches** any
of them, checks listener state, opens CLIPRDR, or connects to either X11
display. A manifest's binary `source_commit` and a SHA-256 do not
independently prove a trusted source-to-binary build transcript.

Schema 2 returns `private_rdp_endpoint.listener_verified=false` and
`runtime_authorized=false` even when every local field is well formed.
All host gates for private xrdp/module/channel forwarding, sesman
requirements, ELF dependencies, cookie rejection and Firefox browser
acceptance remain unresolved. Do not treat the new schema as a runtime
backend. Its purpose is to prevent accidentally treating a Qt-only
controller as a complete A/B/C transport test.

## Schema-2 private-build / IPC coherence (PR #44)

The offline C-leg contract must refer to the **same** socket directory
that corrected xrdp's opt-in private CMake build compiles into chansrv.
The old schema-2 form had an incompatible socket directory under the
experiment's per-leg run_root; PR #40 instead compiles
`XRDP_SOCKET_ROOT_PATH` to
`<release_root>/xrdp-console/socket-root` where release_root is
the already-owned `.release` parent.

Schema 2 now additionally requires `private_build` with:

- `root`: the existing, owned `.release/xrdp-console` directory
- `source_commit`: an explicitly reviewed **actual private-build commit**,
  currently PR #40, PR #42 or PR #43; it is **not** the PR #32 base commit
- `state_hash`: 64-hex CMake source/patch/configuration state digest
- `install_prefix`: the existing matched hash-keyed private installation
  ending in `xrdp-install-<first-16-state-hash-hex>`
- `compiled_socket_root`: exactly `<root>/socket-root`, identical to
  top-level `socket_dir` and distinct from the per-leg browser run directory
- `compiled_runstate`: exactly `<root>/runstate`
- `compiled_pid_path`: exactly `<install_prefix>/var/run`, derived from
  the xrdp Autotools localstatedir rather than runstatedir

The actual private chansrv, xrdp and native module binaries must all
carry that **same private build commit** and exist under the matched
installation prefix; the browser, Qt and synthetic peer remain separate
test roles. An xrdp/module/chansrv artifact whose filename is merely
under the broader `.release` area is not a matched private build.

The offline validator checks field/path/hash identities and rejects
mixed-socket, mixed-source, swapped-module, /run and protected-prefix
mismatches. **It does not independently verify** the Git ancestry,
CMakeCache hash, build transcript, RUNPATH/dlopen, native IPC handshake,
listener binding or actual runtime process identity; those remain
separate host gates. PR #43 provides a read-only configuration-provenance
audit of actual Git, 52 ordered patches and CMakeCache. A manifest
cannot replace that external evidence. All execution is still disabled.

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

## Browser origin, environment and cleanup tightening

The browser helper now exposes `serve_receipt_origin()`: the future caller
can hold one `ReceiptOriginLease` across A, B and C, rather than starting
three unrelated receipt HTTP servers. The lease points only to
`127.0.0.1`, is tied to the creating process and becomes invalid after
shutdown. The older one-leg `serve_receipt_page()` context manager remains
as a wrapper for compatibility. No HTTP server was launched by these
offline changes.

`run_firefox_chansrv_timing()` can consume a caller-held
`receipt_origin` and unique bounded `case_id` for each leg. It creates a
fresh profile root for each case. Browser children receive an explicit
environment allowlist with private `DISPLAY`, private `XAUTHORITY`,
`HOME`, per-case XDG/cache/profile paths, loopback WebDriver and
`GDK_BACKEND=x11`. Nothing is copied from the user's X11/session bus,
Wayland environment or `LD_LIBRARY_PATH`. The requested
`widget.gtk.clipboard_timeout_ms` is recorded **separately** from its
unverified applied value. The browser helper still does **not** attest the
actual browser preference readback or Firefox X11 requestor PID.

`stop_group()` no longer silently returns after the process-group leader
has exited. If the group might still have descendants, or an unknown
session/group would be signalled, it fails closed instead. A group leader
being reaped is not proof that all Firefox descendants have stopped.
A fully executable backend still requires independently attested process
group/cgroup or pidfd identity and complete post-cleanup inspection.

The old `start_authenticated_source_xvfb()` helper now **always refuses**:
its filesystem scan followed by a launch could race another display owner,
and the old implementation did not verify negative-cookie rejection.
An atomic caller-owned allocator and authenticated private Xvfb socket
attestation are prerequisites for restoring a real start path. The
read-only `verify_isolated_xvfb()` checks the expected :191–:249 range,
paired `-auth` and `-nolisten tcp` arguments, and process UID, but it
does **not** substitute for socket-ownership or cookie-negation evidence.

All added tests use only mocked process/HTTP/X11 boundaries; no private
server, Firefox or clipboard operation was executed.

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
