# Measurement model

Input-roundtrip runs use four timestamps. The key injector records **T0**
immediately before submitting the synthetic key to the isolated RDP client's
X server. The physical X11 stimulus records **T1_event** immediately after it
dequeues the corresponding `KeyPress`, then records **T1_draw_done** after
`XSync()` confirms that its marker draw has been processed by the physical X
server. The pixel probe records **T2** when the changed marker is readable in
the FreeRDP output display.

The report therefore separates:

```text
T0 -> T1_event       RDP input, xrdp, VNC, and physical X event delivery
T1_event -> T1_draw  local X11 marker rendering
T1_draw -> T2        capture, VNC/RDP encoding, transport, and client display
T0 -> T2             full interactive round trip
```

Input-roundtrip reports also include per-process CPU/RSS and a best-effort
`rdp-private-wire` transport line with wire bytes/sec, retransmissions, send
queue, and RTT. Values unavailable from unprivileged `ss` are reported as
`NA`.

The pixel probe polls at a configurable interval (`--poll-ms`, default 3 ms),
so T2 includes at most one polling interval of observation quantization. The
benchmark reports nearest-rank p95 and p99 values. An input event is not
injected until the previous event has either completed or timed out; the
configured input rate is therefore a target cadence rather than a guarantee
when a path stalls.

Graphics runs also support `--transport rfb`. This mode starts an isolated
loopback `x11vnc -nopw`, requests RAW RFB rectangles, and records **T2_rfb**
when the marker pixel's bytes arrive at the benchmark socket:

```text
GL swap-complete -> T2_rfb   direct-VNC wire-visible latency
GL swap-complete -> T2        full private x11vnc -> xrdp -> RDP-client path
```

The direct value is a transport baseline, not a desktop-window paint or
compositor-present measurement. It uses the same physical X display and GL
stimulus as the RDP mode, while the private `-nopw` listener is unrelated to
the production x11vnc authentication. Each graphics report includes p50, p95,
p99, maximum, misses, GL render time, process CPU/RSS, and best-effort TCP
wire bytes/sec, retransmissions, send queue, and RTT. `ss`-unavailable fields
are reported as `NA` rather than inferred.

Graphics runs also support `--backend direct-x11 --transport rdp`. This mode
does not start x11vnc or the RFB relay. It loads the first-party
XCB/XDamage/XShm module into the private xrdp build, disables GFX/drdynvc and
dynamic resizing, and forces the FreeRDP window to the physical X11 geometry:

```text
GL swap-complete -> T2   direct XCB/XDamage/XShm -> classic bitmap -> FreeRDP presentation
```

The direct backend is graphics-only until XTest input support is implemented.
It uses the same marker stimulus and FreeRDP pixel probe as the VNC backend, so
`T1_draw -> T2` is the controlled comparison for the capture/output path. The
loader smoke test separately verifies the same vertical path with one known
red/blue marker assertion.

Input-roundtrip also supports `--transport rfb --mode input-roundtrip`. The
benchmark sends the same F9 RFB KeyEvent pulse as the RDP input path to the
private `-nopw` listener, while the physical X11 stimulus and marker probe
remain unchanged. It therefore reports the same four stages above, with
`T0 -> T1_event` measuring direct-RFB input delivery and `T1_draw -> T2`
measuring direct-RFB returned graphics. This is a lower-bound comparison for
the RDP path because it omits xrdp, FreeRDP, and the RDP client presentation
stage.

The benchmark also supports `--transport vnc-viewer --mode input-roundtrip`.
This starts `xtigervncviewer` on a private Xvfb display and injects the same
F9 pulse into the viewer window. Its returned-graphics timestamp is the
marker observed in the viewer's X11 window:

```text
T1_draw -> T2_rfb       direct RFB bytes at the benchmark socket
T1_draw -> T2_viewer    real VNC viewer decode and X11 presentation
T1_draw -> T2            FreeRDP decode and RDP-client presentation
```

The viewer path is useful for separating VNC-client presentation cost from
the server-side path. It is still a private comparison path; it does not
replace the production VNC listener or the complete RDP measurement.

The generated xrdp dependency intentionally excludes the old
`XRDP_VNC_PROFILE`/`VNC_SCHED` server instrumentation. The benchmark's
profile parser remains available for historical logs, but these records are
not emitted by the current native daemon. Server-side attribution is therefore
not part of the current measurement contract; add it later as a separately
justified diagnostic change.

The direct-RFB T2 is the target pixel observed in decoded RFB bytes, whereas
the RDP T2 is the pixel observed in the FreeRDP X11 window. Direct RFB is
therefore a lower-bound transport/capture baseline rather than an exactly
equivalent viewer-present measurement.

The private IPv6-to-IPv4 RFB relay uses bounded per-direction buffers and
selector write readiness. It closes a run if a peer leaves more than 16 MiB
queued, which prevents a stalled destination from turning into unbounded
memory growth or a busy-spin CPU artifact.
