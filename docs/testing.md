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
| `popup-background-epoch-unit` | strict decoding of test-controlled background epochs, including stale, malformed, and partially filtered markers |
| `h264-latest-frame-unit` | generation-safe H.264 tile scheduling, including a mixed-age page-scroll reproduction and the scroll-priority regression |
| `client-scaled-output-plan-unit` / `scaled-output-capability-policy-unit` | gated client-scale eligibility, geometry planning, and safe fallback decisions |
| `scroll-motion-observer-unit` / `scroll-copy-plan-unit` | bounded scroll-motion observation and conservative copy-run classification |
| `gfx-bitmap-cache-observer-unit` / `verified-bitmap-cache16-unit` | cache observations, byte-verified bounded slots, ACK residency, and fallback state |
| `xrdp-upstream-unit` | serial pinned-xrdp `make check`, including Console dirty-region, pacing, and RDPGFX acknowledgement telemetry regressions |
| `xrdp-loader-smoke` | generated xrdp loading the module through FreeRDP, accepting the initial cursor update, then drawing a known red/blue source marker and asserting that the expected pixel reaches the FreeRDP framebuffer |
| `xrdp-loader-gfx-h264-odd-scaled-smoke` | standard H.264 GFX AVC420 loader/pixel smoke at odd scaled presentation geometry; CMake requires the selected FreeRDP's H.264 decoder before registering the test |
| `xrdp-loader-gfx-h264-fullhd-source-cpu-contention` | scaled 1920x1080-to-1512x949 H.264 full-screen update burst while the client/server share one CPU; requires the latest drawn color to reach the FreeRDP framebuffer within 1000 ms |
| `xrdp-loader-gfx-h264-popup-ui-stress` | repeatedly opens and closes a synthetic launcher popup with remote taskbar clicks over scaled H.264, one-CPU contention, and a changing 20 Hz full-screen scene; each opening must show the expected generation and match a source-side popup reference within measured color-error limits in 1000 ms, then remain coherent during 250 ms of sampled updates |
| `lxqt-menu-quality-calibration-unit` | calibrates the fast and full pixel oracles against fourteen distinct saved-good H.264 captures plus pre-open, saved live-corruption, shifted, blank, and deliberately corrupted menu images |
| `lxqt-menu-quality-mapping-unit` | checks the shared area-weighted pixel mapping for identity, 1366-to-1364 scaling, downscaling, and viewport offsets |
| `lxqt-menu-quality-interactive-protocol` | exercises the persistent menu observer before a source exists, source-window replacement, fresh capture IDs, rejection/recovery, and shutdown |
| `popup-background-epoch-xvfb` | draws distinct background epochs on Xvfb and verifies their decoded XGetImage samples appear within one second |
| `xrdp-loader-gfx-h264-lxqt-menu-stress` | opens the real LXQt Fancy Menu at least 20 times over scaled H.264 and runs for at least 75 seconds by default under one-CPU contention and a changing 20 Hz background; each opening must pass same-frame full-region quality within 1000 ms and remain coherent during three later full-region captures |
| `xrdp-loader-gfx-h264-randr-resize` | changes the X11 source from 1024x768 to 1920x1080 during an H.264 session, then checks the queued remote resize, FreeRDP window geometry, rendered pixels, and connection; requires Xephyr and is skipped when it is unavailable |
| `xrdp-loader-gfx-h264-randr-resize-no-dynamic-resolution` | repeats the source RandR resize without enabling FreeRDP's client-driven Dynamic Resolution option, while checking the same server-initiated resize result |
| `xrdp-loader-gfx-h264-coherence*` | repeated whole-client-frame H.264 coherence checks under normal/contended CPU and a four-arm scroll/cache A/B; preserves first torn-frame screenshots and server/client diagnostics |

The popup UI stress test uses a synthetic Xlib launcher, a locally launched
xrdp instance, and the project-built FreeRDP client on the same Linux host. It
checks the complete popup region against a source-side reference using
area-weighted scaling and bounded decoded RGB error, and retains the source
reference, summary, failure screenshots, and a representative passing client
frame under
`build-direct-console/test-artifacts/xrdp-loader-gfx-h264-popup-ui-stress/`.
Menu structure is sampled during a 250 ms stability window while the background
changes at 20 Hz. The pixel reference is checked when the popup opens and at
the end of that window; the test does not continuously record every displayed
frame.

Build and run this test with:

```bash
cmake --build build-direct-console --target \
  xrdp-console x11-popup-menu-stress x11-popup-ui-probe
ctest --test-dir build-direct-console --output-on-failure \
  -R '^xrdp-loader-gfx-h264-popup-ui-stress$'
```

This test measures the local H.264 path under single-CPU contention. It does
not display the real desktop Start menu, emulate WAN packet loss or bandwidth
limits, or exercise Microsoft Remote Desktop/Windows App input, decoding, and
scaling behavior. A pass is useful server-path evidence, but it does not
reproduce or rule out the reported minute-long delay or corruption with a
different client or network. Validate that issue separately with the same
Microsoft client and connection path.

The LXQt menu stress test exercises a real application popup, rather than the
separate synthetic popup transport test. It starts `lxqt-panel` with its Fancy
Menu plugin on the test source display, fills the menu with 32 isolated
desktop-entry fixtures, and opens it through remote pointer input. Within the
1000 ms latency budget, a fast pixel probe checks text-row samples from the
source menu against their mapped pixels in the decoded client frame. If they
match, the probe runs the full-region quality checks on those same captured
images, so a later recovered frame cannot mask a broken first rendering. It
then keeps the menu open for three additional full-region captures to detect
later partial redraws or corruption. The default endurance campaign runs at
least 20 open/close cycles over at least 75 seconds, with one-CPU contention
and a 20 Hz background update. A unique test-controlled background epoch must
reach the decoded client before each open and return within one second after
each close. A missing epoch is retained as a failure with the decoded screen,
timings, source/client captures, and server/client logs. Saved-image
calibration runs independently of LXQt and keeps thresholds fixed across
known-good and known-bad pairs. The test is registered when both `lxqt-panel` and
`dbus-run-session` are installed. Install `lxqt-panel` on another machine if
CMake does not register this test there. It uses the project-built FreeRDP
client on the same host; it does not reproduce Microsoft Remote Desktop's
decoder/window-system behavior or the user's exact desktop-entry catalog. Its
source/client PPM captures and summary are written to
`build-direct-console/test-artifacts/xrdp-loader-gfx-h264-lxqt-menu-stress/`.
For each cycle, the first loading frame and the first populated-but-mismatched
frame are saved as paired source/client PPMs, when those states occur.
Set `XRDP_CONSOLE_LXQT_MENU_STRESS_CYCLES` to a value from 1 to 20 for a longer
stress run; this sets the minimum cycle count. The test also runs for at least
`XRDP_CONSOLE_LXQT_MIN_STRESS_SECONDS` seconds, defaulting to 75. Set that
value to 0 only for short debugging runs. `XRDP_CONSOLE_LXQT_STABILITY_SAMPLES`
controls the extra full-region captures per open menu and defaults to 3.

The calibration fixture provenance and measured threshold distributions are
documented in `tests/fixtures/lxqt-menu-quality/README.md`. The saved pairs
calibrate the oracle for the project-built FreeRDP/X11 path; they do not
establish behavior in the Microsoft Windows App or macOS Remote Desktop.

The RandR resize cases require Xephyr and xrandr. On Debian/Ubuntu install
`xserver-xephyr` and `x11-xserver-utils`, then reconfigure the build so CTest
refreshes its test registry. If Xephyr is unavailable, the RandR tests are
reported as skipped rather than executed.

Build and run the real-menu stress test with:

```bash
cmake --build build-direct-console --target \
  xrdp-console x11-popup-ui-probe x11-popup-menu-stress \
  x11-lxqt-menu-quality-probe
ctest --test-dir build-direct-console --output-on-failure \
  -R '^xrdp-loader-gfx-h264-lxqt-menu-stress$'
```

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
