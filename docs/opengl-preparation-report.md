# OpenGL 3.3 preparation and pixel-pipeline profile

## Scope and baseline

This is a profiling/preparation change only. It does not add OpenGL to
`libxrdp_console.so`, alter the CPU/H.264 runtime path, or deploy/restart xrdp.
Both new tools are opt-in CMake targets, defaulting OFF. The normal production
module has no OpenGL link or runtime dependency.

Baseline inspected: `63e72b345fcba26994a89e324a641d0258df445e`
(`Fix H.264 first-frame starvation`). The machine was running an RDP session
during final checks; full project build and full CTest were therefore deferred
to avoid competing with that session. The new tools themselves were built with
`-Wall -Wextra -Wpedantic -Werror`.

## Host and OpenGL capability

| Item | Observed |
| --- | --- |
| CPU | Intel Core i3 M 330, 2 cores / 4 threads, 2.13 GHz |
| Caches | 512 KiB L2 per core; 3 MiB shared L3 |
| Memory | 3.7 GiB RAM; at inventory time 1.7 GiB available and 868 MiB swap used |
| GPU | AMD/ATI RV710/M92 Mobility Radeon HD 4330/4350/4550, PCI `1002:9552` |
| Kernel driver | `radeon` |
| GL renderer | Mesa, `AMD RV710 (DRM 2.51.0 / 7.0.0-34-generic)` |
| Mesa | `26.0.8-1ubuntu0.3` |
| GLX | 1.4 |
| Required target context | OpenGL 3.3 Core: created successfully |
| Highest core context request observed | 3.3; higher requests through 4.6 were not created |
| GLSL | 3.30 |
| Limits | 8192 max texture dimension; 8 color attachments; 8 draw buffers |
| FBO/PBO/sync | FBO, pixel-pack buffers, map-buffer-range, and GL sync fences present |
| Timer query | Core/extension and entry points present; 64-bit timestamp counter; nonblocking availability smoke succeeded |
| Sampler objects | Entry points and create/configure/bind smoke succeeded |
| Other | Texture rectangles, `GLX_EXT_texture_from_pixmap`, XComposite, timer query present |
| NV12 target formats | R8, RG8 and RGBA8 framebuffer completeness tests all succeeded |

`glxinfo` is not installed. The isolated GLX probe creates the requested 3.3
contexts and queries the context directly; it does not add a dependency on
`glxinfo` or any GL library to the production module. `glReadPixels` measured
RGBA and BGRA transfer performance below. The BGRA result was slightly faster
in this run, but this is only an FBO readback probe, not an end-to-end GPU
pipeline result.

OpenGL 3.3 is the new **optional accelerator target**, with CPU fallback
remaining authoritative. Khronos documents timestamp and elapsed timer queries
as available since GL 3.3; query availability can be polled before retrieving
the value, which avoids turning measurement into a synchronization point.
[Khronos query-object reference](https://wikis.khronos.org/opengl/Query_Object),
[glQueryCounter reference](https://wikis.khronos.org/opengl/GLAPI/glQueryCounter).
The probe's one `glClear` timestamp interval (~0.62 ms) is a capability smoke,
not a useful rendering-cost estimate.

## CPU pipeline benchmark

Command used:

```text
./bin/xrdp-pixel-pipeline-bench --samples 100
```

The physical output was on (`LVDS 1366x768`, DPMS monitor on). Each row below
is 100 steady-state samples after one excluded warm-up. Percentiles are
nearest-rank. The benchmark uses the production H.264 presentation planner for
source `1366x768` and requested RDP presentation `1512x949`: an even `1512x948`
NV12 frame, with viewport `(0,50) 1512x850` and neutral-black letterboxing.

| Stage | Work per sample | Wall p50 / p95 / p99 (ms) | Thread CPU p50 (ms) | Estimated effective traffic |
| --- | ---: | ---: | ---: | ---: |
| XShm full-frame capture | 1,049,088 px | 2.142 / 12.023 / 17.538 | 0.081 | 2.36 GB/s modeled read+write; mostly server/synchronization wait, not client CPU |
| Tile fingerprint grid | 1,049,088 px / 264 tiles | 8.823 / 10.654 / 11.542 | 8.809 | 0.46 GB/s modeled source reads; ~99% of one core |
| Nearest presentation scale | 1,285,200 px | 4.903 / 6.570 / 8.476 | 4.899 | 2.03 GB/s modeled BGRA read+write; ~one core |
| BGRA → NV12 full range | 1,049,088 px | 10.755 / 13.014 / 13.573 | 10.751 | 0.53 GB/s modeled reads+writes; ~one core |
| Scale + NV12 conversion | 1,285,200 px | 21.005 / 25.888 / 27.530 | 20.983 | 0.33 GB/s modeled reads+writes; ~one core |
| Persistent NV12 rectangle update | 256×128 = 32,768 px | 0.446 / 0.645 / 0.784 | 0.444 | 0.38 GB/s modeled reads+writes |
| Observer episode shadow copy + 64×64 patch | full 1366×768 shadow plus patch | 1.386 / 2.868 / 3.579 | 1.382 | 5.19 GB/s modeled read+write; likely copy/cache-bandwidth sensitive |
| mmap frame snapshot allocation/copy/release | 2,150,064-byte NV12 frame | 3.467 / 4.190 / 4.348 | 3.464 | 1.25 GB/s modeled copy traffic; includes mapping allocation and unmap |
| RDPGFX AVC420 command construction | 313-byte command, 16 rectangle records | 0.00222 / 0.00223 / 0.00228 | 0.00135 | Not a pixel-processing stage; no pixel throughput claim |
| Software x264 steady P frame | 1,285,200 visible px; 2,188,800-byte 16-aligned NV12 input | 11.820 / 14.300 / 16.151 | 11.807 | ~one core; synthetic moving 64×64 marker, first I-frame excluded |

The byte rates are traffic estimates from the benchmark's declared source and
destination sizes, **not** hardware memory-controller counters. `perf stat`
cannot access PMU counters on this host (`perf_event_paranoid=4`); cache-miss
and physical memory-bandwidth claims are therefore unverified. The memory-copy
figure is effective traffic for the timed copy operation, not a DRAM bandwidth
measurement.

### GL transfer and overlap probe

The probe used a 40-sample three-PBO ring, no `glFinish`, 4,196,352 bytes per
readback, and a synthetic 64 KiB CPU copy between submissions. It was a static
FBO readback test; there was no source texture upload, shader scaling, or NV12
render pass.

| Operation | p50 | p95 | Interpretation |
| --- | ---: | ---: | --- |
| Synchronous RGBA readback | 1.781 ms | 2.131 ms | `glReadPixels` wall time |
| Synchronous BGRA readback | 1.591 ms | 1.900 ms | ~2.41 GiB/s effective for this transfer |
| PBO submit + fence call | 1.484 ms | 1.540 ms | CPU submission call remains material |
| Submit to first signaled poll at slot reuse | 4.629 ms | 4.906 ms | Upper bound to readiness; includes work until the 3-slot ring revisits the slot, not exact GPU duration |
| Fence client-wait call | 0.021 ms | 0.027 ms | Short wait after that slot-reuse poll |

The PBO result demonstrates a working asynchronous mechanism and low wait time
at slot reuse, but not that GPU processing is faster. Its overlap workload was
only a synthetic 64 KiB memcpy; the submit call itself took about 2 ms. An
actual upload → shader → PBO-readback benchmark with timer queries is still
needed before claiming acceleration.

## Runtime evidence and RDPGFX visibility

The available Linux session logs cover 259 one-second profile windows from
2026-09-25 22:24–22:28 PDT. That deployed module logged build prefix
`02d954859e20`, **not** the inspected `63e72b3` baseline, so these are useful
workload context, not a baseline-vs-patch comparison. Per-call p50/p95 were not
recorded. Across the one-second windows:

| Existing aggregate | p50 | p95 | p99 | Max |
| --- | ---: | ---: | ---: | ---: |
| Reported damage pixels/window | 1,005,376 | 1,006,016 | 2,010,752 | 2,054,464 |
| Captured pixels/window | 1,110,740 | 1,322,402 | 1,442,786 | 1,735,720 |
| H.264 converted pixels/window | 15,552 | 123,984 | 835,776 | 1,351,544 |
| H.264 conversion CPU time/window | 0.506 ms | 3.726 ms | 21.194 ms | 47.457 ms |
| H.264 submit call time/window | 14.705 ms | 23.724 ms | 30.537 ms | 32.271 ms |
| Submit-to-`mod_frame_ack` elapsed/window | 90.005 ms | 217.830 ms | 424.888 ms | 1,375.621 ms |

This profile says full-frame conversion can spike during a full repaint, but
the median conversion workload is small after fingerprint dedup. It does not
support moving hashing or every ordinary dirty update to the GPU. The
submit-to-`mod_frame_ack` aggregate is elapsed time until the producer-window
release callback; it does not isolate encoder completion from client/network
frame acknowledgement. Do not interpret it as pure GPU or pure network time.

The server log identifies the client as `Keivans-MacBook`, says it supports
GFX, reports a 1512×949 single-screen surface, and records H.264/AVC420 output.
It also logs client requests to turn frame ACKs off. The available logs do not
expose whether this client advertised `SURFACETOSURFACE`, surface/cache copy
commands, `MAP_SURFACE_TO_SCALED_OUTPUT`, AVC444, or cache import. The pinned
xrdp source can serialize SurfaceToSurface, but that is not evidence that this
client negotiated or supports it. Windows 11 client capability details are
also not available in these logs.

## GPU candidate ranking

1. **Fused scale + BT.709 NV12 — highest potential, conditional.** The full
   output path costs ~21.0 ms p50 and one CPU core. A fragment pass can map the
   source and emit Y plus subsampled UV without a full-size intermediate. Use
   R8 and RG8 targets, PBO readback, fences, and the 3.3 timestamp-query ring.
   First validate a dirty-region shader against the CPU reference and include
   upload, readback, and CPU submission in the comparison. Runtime medians
   show this may mostly benefit full refreshes, not small deduplicated damage.
2. **BGRA→NV12 alone — plausible, but lower than fused.** Full-frame CPU cost
   is ~10.8 ms p50. A standalone GPU pass is worth comparing only if a fused
   pass is not feasible or measurements show conversion remains material under
   live damage.
3. **Presentation scaling — plausible, conditional.** Nearest scaling costs
   ~4.9 ms p50 for the complete viewport. Shader scaling is natural, but
   shipping pixels back to CPU/x264 can erase the gain; the client-side scaled
   surface capability is unknown.
4. **X11 capture — low priority.** XShm wall p50 is ~2.1 ms, but process CPU
   p50 is ~0.08 ms. Texture-from-pixmap/Composite could avoid a handoff, but
   changes capture ownership/compositor semantics and targets a mostly
   synchronization-bound stage.
5. **Shadow copies and mmap snapshot — medium CPU-copy cost, poor GL target.**
   Profile/ownership work or a protocol-side copy elimination is more direct
   than uploading CPU data to a GPU and reading it back.
6. **Tile fingerprinting and scroll verification — poor GPU candidates.**
   These are bounded CPU-resident comparisons/reductions; GL 3.3 has no compute
   shaders, and transfer/reduction overhead is unmeasured. Preserve CPU logic.
7. **x264 — not a GL candidate.** This GPU/driver path has no usable H.264
   hardware encoder; x264 remains CPU work.
8. **RDPGFX command construction — not a GPU candidate.** At ~2.2 µs p50 it is
   negligible; protocol capability could remove work, but the relevant client
   capabilities are not logged.

## Future overlap shape and patch order

The safe future pipeline is bounded and generation-aware:

```text
XShm capture + fingerprint on CPU
    → copy/upload into a fixed GL-owned slot before the XShm arena is reused
    → submit scale/Y/UV shader work and timestamp queries
    → CPU services XDamage, RDP input, generation bookkeeping, and later tiles
    → poll fences/query availability without blocking
    → map completed PBO into bounded NV12 storage
    → discard stale generations; give current NV12 to x264
```

No asynchronous job may retain a borrowed XShm view across a later capture.
Use a fixed ring, explicit slot generation, and CPU fallback. Do not call
`glFinish` or fetch unavailable query results in the hot path.

Recommended order:

1. Keep the current first-paint/frame-window/scroll work isolated and land its
   correctness changes first.
2. Add an opt-in GL 3.3 microbenchmark with a nonblocking timer-query ring and
   an actual upload → fused scale/NV12 → PBO-readback shader. Compare wall,
   CPU-submission, GPU-query, fence-wait, and end-to-end durations against the
   CPU reference.
3. Only if that wins at live dirty-area distributions, define the smallest
   bounded async backend seam and add a CPU implementation/test as the
   correctness oracle.
4. Keep production GL optional with startup capability checks and immediate
   CPU fallback; test Microsoft clients and memory pressure before enabling it.

No production backend interface was added in this patch. The runtime evidence
shows very different full-refresh and ordinary-damage costs, and the H.264
generation/frame-window path is being changed separately. Fixing the upload,
slot ownership, and stale-generation contract in a standalone GL benchmark
first avoids freezing a generic runtime API before the dataflow is stable.

## Patch contents and validation

The preparation patch adds:

- an opt-in 100-sample CPU stage benchmark, using the actual H.264 presentation
  plan and reporting wall/thread-CPU percentiles plus modeled byte traffic;
- bounded duration-summary arithmetic and CLI tests;
- an opt-in GLX capability/readback probe that requires a real 3.3 Core
  context, checks formats/PBO/fences/samplers/timer queries, and exits 77 when
  the target context is unavailable;
- optional CMake handling so normal builds do not require the new GL probe or
  OpenGL development files; pre-existing GL benchmark helpers are skipped
  when GL development files are absent;
- this report.

Executed successfully:

```text
git diff --check
cmake --build /home/keivan/xrdp-console-clean-build --target \
  xrdp-pixel-pipeline-bench xrdp-gl-capability-probe stage-statistics-unit --parallel 2
ctest --test-dir /home/keivan/xrdp-console-clean-build --output-on-failure \
  -R '^(stage-statistics-unit|pixel-pipeline-bench-cli|gl-capability-probe)$'
```

The focused tests passed 3/3. The 100-sample pipeline benchmark and live GL
probe also completed successfully. Full project build, full CTest, ASan/UBSan,
and TSan were not run. Full CTest was deferred while a native RDP session was
active; no service restart or deployment was performed.

## Next task under 15 minutes

Add and test a fixed three-slot OpenGL timestamp-query ring to the opt-in GL
benchmark, using a trivial FBO pass only to validate enqueue/poll/result
availability without blocking. Then use the same ring for the first real fused
shader measurement. Do not connect it to the production module yet.
