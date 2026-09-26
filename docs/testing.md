# Test plan

The test suite focuses on the direct-X11 Console module: XCB connection and
authentication, XDamage, persistent XShm capture, cursor/input, presentation,
clipboard, bounded RDPGFX, and lifecycle/reconnect behavior. Unit and
authenticated-Xvfb tests run without modifying the host xrdp service. The
optional local RDP loader smoke uses FreeRDP when available. Microsoft-client
interoperability remains a manual Windows/macOS test gate.

Some isolated benchmark tests still cover the deprecated, opt-in VNC/RFB
comparison mode. They are not runtime, packaging, or deployment requirements.

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
| `xrdp-upstream-unit` | serial pinned-xrdp `make check`, including Console dirty-region, pacing, and RDPGFX acknowledgement telemetry regressions |
| `xrdp-loader-smoke` | generated xrdp loading the module through FreeRDP, accepting the initial cursor update, then drawing a known red/blue source marker and asserting that the expected pixel reaches the FreeRDP framebuffer |

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
