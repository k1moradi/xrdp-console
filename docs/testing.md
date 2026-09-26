# Test plan

The test suite focuses on the direct-X11 Console module: XCB connection and
authentication, XDamage, persistent XShm capture, cursor/input, presentation,
clipboard, bounded RDPGFX, and lifecycle/reconnect behavior. Unit and
authenticated-Xvfb tests run without modifying the host xrdp service. The
optional local RDP loader smoke uses FreeRDP when available. Microsoft-client
interoperability remains a manual Windows/macOS test gate.

The scaled-output, scroll-copy, bitmap-cache observation, and verified
bitmap-cache paths are individually gated by exact service environment values:

| Gate | Behavior when unset | Behavior when set to `1` |
| --- | --- | --- |
| `XRDP_CONSOLE_CLIENT_SCALE` | Keep server-side presentation scaling. | Attempt client-side scaled output only when negotiated RDPGFX capabilities are eligible; otherwise fall back safely. |
| `XRDP_CONSOLE_CLIENT_SCROLL` | Do not emit SurfaceToSurface scroll reuse. | Refine scheduler-selected H.264 runs only after high-confidence motion matching and exact pixel verification. |
| `XRDP_CONSOLE_CLIENT_CACHE_OBSERVE` | Do not collect bitmap-cache observations. | Observe reuse candidates without changing rendering. |
| `XRDP_CONSOLE_CLIENT_CACHE` | Do not use the verified bitmap cache. | Use bounded, ACK-tracked verified cache entries with H.264 fallback. |

For live testing, first run with all four variables unset, then set exactly one
to `1` per run and restart xrdp between runs. Confirm the server remains on
port 3389. Verify negotiated capabilities and path-selection logs for every
session. A previously observed Microsoft macOS client advertised RDPGFX 10.7
flags `0x82`, which makes scaled-output mapping ineligible; seeing the
server-scaled fallback in that case is expected. FreeRDP loader tests do not
replace manual Windows/macOS validation, especially for scroll tearing, input
responsiveness, clipboard, resize, and reconnect.

CTest discovers `xfreerdp3` or `xfreerdp` from `PATH` by default. To select a
specific executable, configure with
`-DXRDP_CONSOLE_FREERDP_EXECUTABLE=/absolute/path/to/xfreerdp3`, or set the same
environment variable when running `scripts/build-direct-console.sh`. This
allows a private test client to coexist with the distribution package.

`xrdp-loader-gfx-h264-odd-scaled-smoke` is capability-gated. It runs only when
the selected FreeRDP reports `WITH_GFX_H264=ON` and a supported H.264 decoder
backend in `/buildconfig`; otherwise it returns CTest's configured skip code.
Ubuntu's packaged FreeRDP on this development host currently lacks those flags.
For an end-to-end H.264 test, install the FreeRDP/FFmpeg build dependencies
listed in the README with apt, build the isolated client using
`scripts/build-test-freerdp.sh`, then pass its printed path through
`XRDP_CONSOLE_FREERDP_EXECUTABLE`. That script uses a private install prefix and
does not replace system FreeRDP. The H.264 CTest must actually run and pass
before claiming H.264 client interoperability; a skip is not such evidence.

The RFB helper tests listed below cover retained measurement utilities only;
they do not exercise or provide a production display transport.

| Test | Coverage |
| --- | --- |
| `network-unit` | namespace command construction, cached-sudo failure boundary, and localhost no-privilege behavior |
| `measurement-profile-unit` | valid profile parsing, malformed records, marker-state matching, failed-flush filtering, stale/out-of-window points, reordered logs, and timestamp-count errors |
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
| `xrdp-loader-gfx-h264-odd-scaled-smoke` | capability-gated standard H.264 GFX AVC420 loader/pixel smoke at odd scaled presentation geometry; skipped if the selected FreeRDP lacks a compiled H.264 GFX decoder |

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
presentation and dynamic client resize.
