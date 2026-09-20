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

Input-roundtrip also supports `--transport rfb --mode input-roundtrip`. The
benchmark sends the same F9 RFB KeyEvent pulse as the RDP input path to the
private `-nopw` listener, while the physical X11 stimulus and marker probe
remain unchanged. It therefore reports the same four stages above, with
`T0 -> T1_event` measuring direct-RFB input delivery and `T1_draw -> T2`
measuring direct-RFB returned graphics. This is a lower-bound comparison for
the RDP path because it omits xrdp, FreeRDP, and the RDP client presentation
stage.

The optimized xrdp candidate also has an opt-in server profile. Set
`XRDP_VNC_PROFILE=1` only on the private daemon with the benchmark's
`--xrdp-env XRDP_VNC_PROFILE=1` option. It emits aggregate `VNC_PERF` lines,
rather than restoring per-PDU INFO logging. The VNC-side line measures the
framebuffer parser through `server_end_update`; the painter-side line reports
raw framebuffer bytes copied, copy time, bitmap-path time, encode time, send
time, PDU count, and RDP bitmap stream bytes. These are server-stage timings,
not a replacement for the client-visible T2 measurement. `wire_bytes` in
these lines is the RDP bitmap PDU stream size and excludes TCP/TLS framing.
The same profile emits `VNC_SCHED` per-update lines with `seq`, request
`wait_us`, parser/process `process_us`, server flush `flush_us`, the gap to
the next request, and `rects`/`raw_bytes`. These fields distinguish time spent
waiting for a requested update from time processing or flushing it. Because
the profile logs on the update path, profile-enabled latency is not a clean
performance baseline; keep profiling disabled for A/B latency comparisons.

With `XRDP_VNC_PROGRESSIVE_FLUSH=1`, the incremental RAW path may close and
reopen the classic direct-bitmap painter after each configured byte threshold
while the current RAW rectangle is incomplete. `server_paint_rect()` remains
the backing-store update, no extra RFB `FramebufferUpdateRequest` is sent,
and the final logical update still has exactly one next request. The opt-in
switch is ignored unless incremental parsing, direct bitmap output, and
non-GFX unsuppressed output are all active. `VNC_SCHED` adds
`first_flush_us` (update header to the first completed intermediate flush),
`progressive_flushes`, `bytes_before_first_flush`, and `logical_update_us`
(update header to the final `server_end_update`).

The direct-RFB T2 is the target pixel observed in decoded RFB bytes, whereas
the RDP T2 is the pixel observed in the FreeRDP X11 window. Direct RFB is
therefore a lower-bound transport/capture baseline rather than an exactly
equivalent viewer-present measurement.

The private IPv6-to-IPv4 RFB relay uses bounded per-direction buffers and
selector write readiness. It closes a run if a peer leaves more than 16 MiB
queued, which prevents a stalled destination from turning into unbounded
memory growth or a busy-spin CPU artifact.
