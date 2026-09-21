# Test plan

The repository keeps focused behavioral tests for the benchmark helpers and
the private VNC client. The tests are intentionally unprivileged; end-to-end
display tests remain separate because they require the physical X11 display,
Xauthority, x11vnc, and client binaries.

| Test | Coverage |
| --- | --- |
| `network-unit` | namespace command construction, cached-sudo failure boundary, and localhost no-privilege behavior |
| `measurement-profile-unit` | valid profile parsing, malformed records, marker-state matching, failed-flush filtering, stale/out-of-window points, reordered logs, and timestamp-count errors |
| `rfb-client-unit` | RFB KeyEvent wire encoding for both press and release |
| `proxy-backpressure-unit` | bounded relay buffering, selector write readiness, and peer cleanup |
| `python-syntax` | source compilation for the installed Python helpers |
| `systemd-unit` | clean SIGINT shutdown contract, no exit-status masking, and compatibility drop-in parity |
| `damage-region-unit` | clipping, overlap/adjacency coalescing, empty input, and bounded full-screen fallback |
| `x11-damage-integration` | authenticated-Xvfb XDamage resource setup, raw rectangle delivery, persistent XShm capture of a known pixel, acknowledge/re-arm, and teardown |
| `x11-cursor-integration` | authenticated-Xvfb XFixes cursor-image capture, ARGB-to-xrdp conversion bounds, and cursor-change notification delivery |
| `x11-input-integration` | authenticated-Xvfb XTest keyboard and pointer event delivery to a focused X11 window |
| `module-lifecycle` | C++23 XCB module construction, authenticated-Xvfb connect/reconnect, fd-0 handling, geometry setup, dead-server failure, teardown, and wait-state preservation |
| `xrdp-loader-smoke` | generated xrdp loading the module through FreeRDP, accepting the initial cursor update, then drawing a known red/blue source marker and asserting that the expected pixel reaches the FreeRDP framebuffer |

The marker correlation contract is:

1. A point record must be a successful RAW paint with a classified red/blue
   state.
2. Its monotonic paint time must fall between the physical draw and observed
   client-visible timestamps.
3. Its decoded marker state must equal the expected state for that sample.
4. Failed, malformed, stale, and wrong-state records are ignored rather than
   paired opportunistically.

The graphical benchmark smoke runs are documented in
[`docs/measurement-model.md`](measurement-model.md). They validate process
startup/cleanup and the complete private path, but are not part of CTest because
they depend on the physical display stack. The `xrdp-loader-smoke` test is a
smaller private-server ABI and pixel-path check; it uses the generated xrdp
install and FreeRDP, with `xvfb-run` when no display is available. The
graphical benchmark also supports `--backend direct-x11` for the
XCB/XDamage/XShm comparison, and its input-roundtrip mode now exercises the
first-party XTest controller. Both modes force the client geometry to the
physical display.
