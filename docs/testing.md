# Test plan

The test suite focuses on the direct-X11 Console module: XCB connection and
authentication, XDamage, persistent XShm capture, cursor/input, presentation,
clipboard, bounded RDPGFX, and lifecycle/reconnect behavior. Unit and
authenticated-Xvfb tests run without modifying the host xrdp service. The RDP
loader integration tests use the project's isolated, H.264-enabled FreeRDP
client; Microsoft-client interoperability remains a manual Windows/macOS gate.

Scaled-output, scroll-copy, and verified bitmap-cache performance paths
default to requested/enabled, with actual use gated by negotiated capabilities
and per-path safety checks. For these three paths, an unset value enables the
request, exact `1` explicitly enables it, and any other explicit value
disables it. Cache observation is diagnostic-only and remains disabled unless
`XRDP_CONSOLE_CLIENT_CACHE_OBSERVE=1` is explicitly set. None of these switches
modifies client capability advertisements.

| Gate | Default behavior | Explicit `=0` behavior |
| --- | --- | --- |
| `XRDP_CONSOLE_CLIENT_SCALE` | Attempt client-side mapping only if negotiated capabilities and preflight allow it; otherwise server-side scaling. | Keep server-side scaling. |
| `XRDP_CONSOLE_CLIENT_SCROLL` | Refine scheduler-selected H.264 runs only after high-confidence matching and exact pixel verification. | Disable SurfaceToSurface reuse. |
| `XRDP_CONSOLE_CLIENT_CACHE_OBSERVE` | Disabled; exact `1` observes cache behavior without changing rendering. | Disable observation. |
| `XRDP_CONSOLE_CLIENT_CACHE` | Use bounded ACK-tracked cache only with known capacity, eligible geometry, and safe residency; otherwise H.264. | Disable verified cache. |

For live validation, exercise the combined default path first, then isolate
each performance optimization by setting the other two performance gates to
`0`; an all-off baseline sets the three performance gates to `0`. Keep cache
observation unset unless collecting diagnostics. Disconnect clients before
restarting xrdp. Confirm the server remains on port 3389 and inspect per-session requested-policy,
negotiated-capability, and actual-path logs. A previously observed Microsoft
macOS client negotiated RDPGFX 10.7 flags `0x82`, including the scaled-map
disable bit, so server-side scaling is the required fallback for that client.
Do not override that negotiated restriction. FreeRDP loader tests do not
replace manual Windows/macOS validation, especially for scroll tearing, input
responsiveness, clipboard, resize, and reconnect.

The `xrdp-loader-gfx-h264-coherence*` tests are a temporal correctness stress
test, not a timing benchmark. A 512x384 X11 stimulus scrolls per-tile 8-bit
generation markers at 60 Hz; the H.264 FreeRDP client's complete framebuffer
is sampled 2,000 times per run. Each snapshot must have matching generations
across columns and consecutive generations down rows. The matrix includes an
unloaded default-policy run and single-CPU-contention runs with default policy,
scroll reuse disabled, cache disabled, and both disabled. Every run checks the
server's requested policy log so the A/B arms cannot silently collapse to the
same configuration. First-failure PPM screenshots and per-run xrdp/FreeRDP
logs are retained under `build-direct-console/test-artifacts/`.

For direct AVC420, the module captures one immutable XShm snapshot covering
all pending H.264 damage tiles and the scaler filter footprint for their
AVC420-aligned output. It derives tile fingerprints and NV12 updates from that
same view and does not issue another source capture until all selected tiles
have been converted. Full invalidation still captures the full source. The
snapshot arena is capped at 32 MiB; if the geometry exceeds that budget or the
arena cannot be allocated, direct H.264 is not used and the negotiated GFX
path falls back to Planar rather than reverting to mixed-time per-tile
captures. Before asynchronous submission, a full-stride NV12 mapping is
allocated but only AVC420 encode rectangles are copied into it. The pinned
xrdp x264 and OpenH264 backends consume those rectangles from the mapping;
untouched anonymous pages stay demand-zero. The encode-rectangle list passed
to xrdp must remain the source of truth for this sparse snapshot.

The canonical `scripts/build-direct-console.sh` first builds the pinned client
with `scripts/build-test-freerdp.sh` if it is not already present, then passes
that private executable to CMake. CMake never discovers a client from `PATH`.
It runs `/buildconfig` during configuration and requires
`WITH_GFX_H264=ON` plus an H.264 decoder backend. A custom client can be
selected with `XRDP_CONSOLE_FREERDP_EXECUTABLE` or the matching CMake cache
option, but it must pass the same check. Missing or incapable clients fail
configuration, and the H.264 loader smoke fails rather than skipping if its
capability precondition is not met. The private build does not replace the
distribution FreeRDP package. A passing H.264 smoke validates this FreeRDP
client/server path only; it does not replace manual Windows/macOS testing.

The RFB helper tests listed below cover retained measurement utilities only;
they do not exercise or provide a production display transport.

| Test | Coverage |
| --- | --- |
| `network-unit` | namespace command construction, cached-sudo failure boundary, and localhost no-privilege behavior |
| `measurement-profile-unit` | valid profile parsing, malformed records, marker-state matching, failed-flush filtering, stale/out-of-window points, reordered logs, and timestamp-count errors |
| `h264-frame-coherence-oracle-unit` | generation-sample parsing, modulo wrap, horizontal partial updates, vertical discontinuity, and malformed marker/grid rejection |
| `rfb-client-unit` | legacy benchmark-only RFB KeyEvent encoding |
| `proxy-backpressure-unit` | legacy benchmark-only bounded RFB relay buffering and cleanup |
| `python-syntax` | source compilation for the installed Python helpers |
| `systemd-unit` | direct Console service paths, native build/activation pairing, and port/client safety checks |
| `damage-region-unit` | clipping, cost-aware overlap/adjacency coalescing, empty input, bounded sparse fragmentation, and explicit full-screen invalidation |
| `x11-damage-integration` | authenticated-Xvfb XDamage setup, idle no-op snapshots, delta-rectangle coalescing and sparse-root preservation, snapshot/re-arm, persistent XShm capture of a known pixel, and teardown |
| `x11-cursor-integration` | authenticated-Xvfb XFixes cursor-image capture, ARGB-to-xrdp conversion bounds, cursor-change notification delivery, and non-fatal oversized-cursor fallback |
| `x11-input-integration` | authenticated-Xvfb XTest keyboard and pointer event delivery to a focused X11 window, including held-key typematic with duplicate makes and teardown release of held keys/buttons |
| `module-lifecycle` | C++23 XCB module construction, authenticated-Xvfb connect/reconnect, fd-0 handling, geometry setup, dead-server failure, teardown, and wait-state preservation |
| `interaction-priority-unit` | pointer/focus bounds, scroll clearing of cursor-local priority, and keyboard-focus priority recovery |
| `h264-latest-frame-unit` | generation-safe H.264 tile scheduling, including a mixed-age page-scroll reproduction and the scroll-priority regression |
| `client-scaled-output-plan-unit` / `scaled-output-capability-policy-unit` | gated client-scale eligibility, geometry planning, and safe fallback decisions |
| `scroll-motion-observer-unit` / `scroll-copy-plan-unit` | bounded scroll-motion observation and conservative copy-run classification |
| `gfx-bitmap-cache-observer-unit` / `verified-bitmap-cache16-unit` | cache observations, byte-verified bounded slots, ACK residency, and fallback state |
| `xrdp-upstream-unit` | serial pinned-xrdp `make check`, including Console dirty-region, pacing, and RDPGFX acknowledgement telemetry regressions |
| `xrdp-loader-smoke` | generated xrdp loading the module through FreeRDP, accepting the initial cursor update, then drawing a known red/blue source marker and asserting that the expected pixel reaches the FreeRDP framebuffer |
| `xrdp-loader-gfx-h264-odd-scaled-smoke` | standard H.264 GFX AVC420 loader/pixel smoke at odd scaled presentation geometry; CMake requires the selected FreeRDP's H.264 decoder before registering the test |
| `xrdp-loader-gfx-h264-randr-resize` | changes the X11 source from 1024x768 to 1920x1080 during an H.264 session, then checks the queued remote resize, FreeRDP window geometry, rendered pixels, and connection; requires Xephyr and is skipped when it is unavailable |
| `xrdp-loader-gfx-h264-randr-resize-no-dynamic-resolution` | repeats the source RandR resize without enabling FreeRDP's client-driven Dynamic Resolution option, while checking the same server-initiated resize result |
| `xrdp-loader-gfx-h264-coherence*` | repeated whole-client-frame H.264 coherence checks under normal/contended CPU and a four-arm scroll/cache A/B; preserves first torn-frame screenshots and server/client diagnostics |

The marker correlation contract is:

1. A point record must be a successful RAW paint with a classified red/blue
   state.
2. Its monotonic paint time must fall between the physical draw and observed
   client-visible timestamps.
3. Its decoded marker state must equal the expected state for that sample.
4. Failed, malformed, stale, and wrong-state records are ignored rather than
   paired opportunistically.

The direct-X11 graphical benchmark smoke runs are documented in
[`docs/measurement-model.md`](measurement-model.md). They validate process
startup/cleanup and the complete private path, but are not part of CTest because
they depend on the physical display stack. The `xrdp-loader-smoke` test is a
smaller private-server ABI and pixel-path check; it uses the generated xrdp
install and FreeRDP, with `xvfb-run` when no display is available. The
graphical benchmark defaults to `--backend direct-x11`; its input-roundtrip
mode exercises the first-party XTest controller. By default it matches client
geometry to the physical display; explicit options cover initial scaled
presentation and source-driven server resize. The RandR loader tests run with
FreeRDP both with and without its client-driven Dynamic Resolution option; the
second case still advertises the separate RDP desktop-resize capability. These
private tests cover the xrdp request path, not live Windows App or MSTSC
behavior.
