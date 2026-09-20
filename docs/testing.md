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

The marker correlation contract is:

1. A point record must be a successful RAW paint with a classified red/blue
   state.
2. Its monotonic paint time must fall between the physical draw and observed
   client-visible timestamps.
3. Its decoded marker state must equal the expected state for that sample.
4. Failed, malformed, stale, and wrong-state records are ignored rather than
   paired opportunistically.

The graphical smoke runs are documented in
[`docs/measurement-model.md`](measurement-model.md). They validate process
startup/cleanup and the complete private path, but are not part of CTest
because they depend on host display state.
