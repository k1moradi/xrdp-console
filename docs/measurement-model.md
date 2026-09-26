# Measurement model

The supported benchmark backend is direct-X11. VNC/RFB modes described below
are deprecated, opt-in historical comparisons and are never used by
production or deployment scripts.

Input-roundtrip runs use four timestamps. The key injector records **T0**
immediately before submitting the synthetic key to the isolated RDP client's
X server. The physical X11 stimulus records **T1_event** immediately after it
dequeues the corresponding `KeyPress`, then records **T1_draw_done** after
`XSync()` confirms that its marker draw has been processed by the physical X
server. The pixel probe records **T2** when the changed marker is readable in
the FreeRDP output display.

The report therefore separates:

```text
T0 -> T1_event       RDP input, xrdp, and physical X event delivery
T1_event -> T1_draw  local X11 marker rendering
T1_draw -> T2        capture, RDP encoding, transport, and client display
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

Graphics and input-roundtrip runs default to `--backend direct-x11 --transport
rdp`. This mode does not start x11vnc or the RFB relay. It loads the first-party
XCB/XDamage/XShm module into the private xrdp build and uses module code `21`.
The direct benchmark's default graphics request is standard RemoteFX
(`--direct-graphics-transport rfx`); `classic` and `gfx-planar` are explicit
alternatives. This controlled benchmark does not exercise the production
H.264/AVC420 path. By default, its client window is set to the physical X11
geometry. `--direct-allow-scaled-presentation` allows a different initial
presentation size, while `--direct-dynamic-resizing` exercises client monitor
resize requests:

```text
GL swap-complete -> T2   direct XCB/XDamage/XShm -> requested RDP graphics path -> FreeRDP presentation
```

Both modes use the marker stimulus and FreeRDP pixel probe. The supported
direct-X11 mode measures `T1_draw -> T2`; the VNC backend remains only as a
deprecated, opt-in historical comparison. In input-roundtrip mode, the module
forwards the RDP keyboard event to XTest and the same physical X11 marker
measures `T0 -> T1_event -> T1_draw -> T2`.
The loader smoke test separately verifies the graphics vertical path with a
known client-visible pixel assertion. Do not interpret a benchmark's requested
transport as the production Microsoft-client negotiation result; use the
module's negotiated-capability and actual-output log records for that.

For opt-in direct-module pipeline attribution, pass
`--xrdp-env XRDP_CONSOLE_PROFILE=1`. The private xrdp log then emits one
`XRDP_CONSOLE_PROFILE` record per approximately one-second window and a final
partial window. Its counters distinguish Damage wake-ups and the server-region
snapshot from captured and successfully painted area, and report presentation
batches, `server_paint_rect()` calls, and the uncompressed bytes handed to
xrdp. In H.264 mode, the same record additionally reports synchronous XShm
capture call count/total/max time, scale-plus-NV12 conversion attempt
count/total/max time and successfully converted pixels, full-frame
copy-plus-submit time, and submit-to-frame-ack wait. The
acknowledgement wait includes asynchronous server encoding, transport, and
client decode/acknowledgement; it is not a direct client-display timestamp.
Profiling is disabled by default. `damage_wakeups` is the delta-region
notification count; `damage_snapshot_pixels` sums the bounded delta rectangles
after local coalescing, so overlapping rectangles can make it exceed unique
changed-pixel area.

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

## Controlled memory pressure

The local direct-X11 RFX benchmark accepts
`--memory-pressure-mib N` for a bounded, page-touched anonymous allocation
held by a helper process during measurement. `0` is the no-pressure baseline;
positive values are limited to 1536 MiB and only accepted for direct-X11/RDP
RemoteFX `graphics-under-churn` or `input-roundtrip` runs. The helper is
installed with the benchmark in its `helpers` subdirectory and needs no
`stress-ng` dependency.

The benchmark requires preflight `MemAvailable >= requested MiB + 512 MiB`,
then waits one second and refuses to allocate if swap-in or swap-out is already
active. During allocation and measurement it monitors `/proc/meminfo` and
`/proc/vmstat`, releases the allocation and aborts if available memory falls
below 512 MiB or either swap counter advances. Consequently, higher matrix
levels can be refused without starting the RDP run; do not bypass that safety
decision. An unexpected exit of the pressure helper also invalidates and
aborts the measurement. For a zero-pressure control, `MEMORY control_valid=1`
means neither swap I/O counter advanced from before to after the run. Existing
`SwapUsed` alone does not invalidate a control. Output records
before/during/after memory and swap snapshots, observed minimum
available memory, swap extrema, process-tree RSS/CPU/major-fault deltas, and
the time from FreeRDP termination to xrdp's module-cleanup log marker. That
marker measures cleanup onset, not completion of the module destructor.

For the memory-resilience gate, first establish a zero-pressure run with
`MEMORY control_valid=1`. Then use an adaptive ladder (for example 128, 256,
384, then 512 MiB), running graphics and input modes independently at each
accepted level while keeping the 30-fps workload and other settings fixed.
Stop when a level is refused or trips the safety guard; do not bypass it. For
example, a 128 MiB attempt is:

```bash
python3 -B tools/benchmark/xrdp_console_bench.py \
  --backend direct-x11 --transport rdp --mode graphics-under-churn \
  --direct-graphics-transport rfx --fps 30 --duration 20 \
  --memory-pressure-mib 128 --repetitions 1

python3 -B tools/benchmark/xrdp_console_bench.py \
  --backend direct-x11 --transport rdp --mode input-roundtrip \
  --direct-graphics-transport rfx --fps 30 --input-churn-fps 30 \
  --duration 20 --memory-pressure-mib 128 --repetitions 1
```

Compare zero-pressure and accepted pressure runs for misses, latency percentiles,
xrdp major faults/RSS/CPU, disconnect cleanup time, and whether memory and
swap return to baseline after the helper releases its allocation. This is a
host-local resilience check, not a substitute for the outstanding Microsoft
Windows and macOS client interoperability tests.
