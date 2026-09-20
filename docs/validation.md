# Validation record

The measurements in this file are historical results from the pre-1.1
benchmark harness. They remain useful for comparison, but must be recollected
with the current harness before being used as quantitative evidence. The
current harness now uses an event-driven, bounded RFB relay, reports separate
input-event and X11-draw timestamps, accounts for the private proxy/Xvfb/
chansrv processes, and protects active Unix sockets and private ports.

The source tree was consolidated after the external consultation and the
clean network matrix collected on 2026-09-17. All cases used a private xrdp
instance, classic RFX with global `[Channels]` `drdynvc=false` and `[Console]`
`channel.drdynvc=false`, and untouched production ports.
Values below are milliseconds; p99 and maximum are shown because the user
reported intermittent lag rather than only a high median.

| churn | one-way delay | input p50/p95/p99 | graphics p50/p95/p99 | round trip p50/p95/p99 | misses |
| ---: | ---: | ---: | ---: | ---: | --- |
| 0 fps | 0 ms | 2.3/40.5/41.0 | 62.0/67.4/71.4 | 65.2/103.3/104.2 | return 2 |
| 0 fps | 2.5 ms | 4.8/34.8/90.4 | 62.9/66.8/100.3 | 67.8/100.3/190.8 | return 1 |
| 0 fps | 5 ms | 7.1/8.6/35.6 | 66.5/74.5/138.5 | 73.8/83.3/145.7 | none |
| 15 fps | 0 ms | 14.4/48.4/85.7 | 81.3/178.5/190.5 | 94.4/197.9/274.5 | none |
| 15 fps | 2.5 ms | 57.5/81.8/88.5 | 98.4/178.6/193.5 | 141.3/244.1/262.2 | none |
| 15 fps | 5 ms | 57.8/92.8/198.6 | 97.7/185.0/224.0 | 137.8/254.0/392.2 | none |
| 30 fps | 0 ms | 37.4/65.0/92.4 | 108.4/197.1/215.9 | 148.1/234.4/278.3 | none |
| 30 fps | 2.5 ms | 34.3/85.2/105.6 | 110.4/172.4/200.1 | 144.9/222.3/263.6 | none |
| 30 fps | 5 ms | 38.6/89.2/120.2 | 118.9/199.2/242.2 | 160.9/259.5/273.8 | none |

The clean run confirms three practical conclusions:

- compositor churn, rather than the small synthetic network delay, dominates
  the input and graphics tails;
- the classic RFX console path remains measurable and stable enough for an
  A/B harness, but it is not equivalent to a hardware video encoder;
- p99 and missed-return counters must be retained in every comparison because
  the median alone hides the intermittent stalls.

The machine has hardware OpenGL through Mesa/radeon, but Vulkan exposes only
llvmpipe for its RV710 device. x11vnc captures X11 pixels; it cannot offload
its framebuffer readback to a Vulkan video encoder. The benchmark therefore
reports local GL render time separately from capture, xrdp encoding, and RDP
presentation time.

The optimized daemon is generated from the hash-pinned xrdp archive and
`patches/xrdp/series`. The retained series contains only the fixed-console
resize/error-recovery, direct VNC path, and input transport checks; the old
profiling, parser, request-ahead, progressive-flush, cache, and experimental
GFX changes are not permanent dependency patches. The clean build and unit
suite pass with `-O3 -march=native`; a live Mac reconnect is still needed to
validate the black-screen symptom on this machine.
