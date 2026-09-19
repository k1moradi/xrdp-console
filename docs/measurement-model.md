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

The pixel probe polls at a configurable interval (`--poll-ms`, default 3 ms),
so T2 includes at most one polling interval of observation quantization. The
benchmark reports nearest-rank p95 and p99 values. An input event is not
injected until the previous event has either completed or timed out; the
configured input rate is therefore a target cadence rather than a guarantee
when a path stalls.

The private IPv6-to-IPv4 RFB relay uses bounded per-direction buffers and
selector write readiness. It closes a run if a peer leaves more than 16 MiB
queued, which prevents a stalled destination from turning into unbounded
memory growth or a busy-spin CPU artifact.
