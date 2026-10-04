# Compiler optimization experiments

This records focused compiler experiments on the host at GCC 15.2.0 and Clang
23.1.3. The source was `eff26b6d366ede6345f1cdd30540be85f558bb69`. These
results do not enable LTO or PGO in the normal build.

## Libtool install relink notices

The retained build log is
`/home/keivan/checkpoints/xrdp-console/2026-10-03/rebuild-after-main-2026-10-03.log`.
Its seven notices were emitted by `make install` for `libvnc.la`, `libxup.la`,
`libmc.la`, `libipm.la`, `libxrdp.la`, `libsesman.la`, and `libxrdpapi.la`.

The source `.la` files have `installed=no` and `dependency_libs` entries such
as the generated build-tree `common/libcommon.la` and `libipm/libipm.la`. During
install, libtool reruns those links against the installed `.la` files. The
installed metadata then has `installed=yes` and points at
`xrdp-install/lib/xrdp/libcommon.la` (and `libipm.la`). The installed shared
objects have the expected private-prefix RUNPATH and depend on `libcommon.so.0`.

This is normal libtool relocation work, not a failed link or a defect in the
server. The build now documents why the message appears. The relink is retained
because suppressing it would leave build-tree paths in the installed metadata.
The notices are expected when a fresh generated xrdp tree is installed; they
are not warnings from compiling the first-party C++ code.

## LTO and PGO validation

The production Console is a loadable module, so its final artifact must remain
shared. To verify `-flto` with static linking, three standalone project tests
were built as fully static executables with GCC and Clang. Both builds used
`-flto`; Clang used LLD. In addition, separate LTO builds compiled the actual
first-party pixel-pipeline benchmark and these test targets:

* `presentation-transform-unit`
* `h264-latest-frame-unit`
* `optimization-pipeline-unit`
* `pixel-pipeline-bench-cli`

The static LTO builds passed the three executable tests under CTest (3/3 for
each compiler). The dynamic LTO builds and both PGO-use builds passed the four
focused tests (4/4). For GCC, CMake's IPO option selected `-flto=auto`; for
Clang the build used `-flto -fuse-ld=lld`.

GCC PGO used `-fprofile-generate=<profile-dir>`, trained by the three unit
tests and 100 samples of the pixel-pipeline benchmark, then rebuilt in the
same CMake directory with `-fprofile-use=<profile-dir>` and
`-fprofile-correction`. Training created 31 `.gcda` files. Clang used
`-fprofile-instr-generate`, collected 16 raw profiles from those same tests and
benchmark, merged them with `llvm-profdata-23`, then rebuilt with
`-fprofile-instr-use=<merged.profdata>`.

The benchmark used the live 1920x1080 X11 root and the existing Full HD
presentation workload: a 1512x949 requested client area with a 1512x850 image
viewport. Each configuration was run three times with 100 samples per run.
The table reports the median of each run's `wall_p50_us`, in microseconds.

| GCC 15.2.0 | No LTO | LTO | LTO + PGO |
| --- | ---: | ---: | ---: |
| Full-grid tile fingerprint | 2135.6 | 2139.6 | 2119.5 |
| Exact scroll reuse classification | 3122.1 | 3031.9 | 3029.1 |
| Presentation scaling | 2527.1 | 2606.6 | 2302.5 |
| Fused scale + direct NV12 | 5340.1 | 5192.0 | 5251.5 |
| Sparse encode-rectangle snapshot | 175.1 | 178.3 | 181.6 |

| Clang 23.1.3 | No LTO | LTO | LTO + PGO |
| --- | ---: | ---: | ---: |
| Full-grid tile fingerprint | 2060.8 | 2036.1 | 2326.6 |
| Exact scroll reuse classification | 3037.9 | 3068.4 | 3102.6 |
| Presentation scaling | 2197.4 | 2350.5 | 2210.1 |
| Fused scale + direct NV12 | 4634.7 | 4454.7 | 4484.0 |
| Sparse encode-rectangle snapshot | 177.6 | 177.2 | 176.0 |

GCC LTO improved the fused NV12 and exact-scroll stage medians by about 3%;
GCC PGO improved the presentation-scaling median by about 9% but was neutral
or slightly slower in several other stages. Clang LTO improved the fused NV12
median by about 4% but slowed presentation scaling by about 7%. Clang PGO did
not improve the full-grid or scroll stages. The x264 encoder is a system shared
library in these benchmark builds and is outside this first-party LTO/PGO
measurement.

These results are mixed and limited to the local CPU pipeline. They do not
measure an end-to-end RDP session or prove that either compiler option improves
client-perceived latency. Keep LTO and PGO opt-in until a representative RDP
workload shows a repeatable end-to-end win. Build directories, CMake caches,
CTest logs, profile data, and benchmark output are retained under
`/home/keivan/checkpoints/xrdp-console/2026-10-03/build-optimization/`.

## Current-main compiler follow-up (2026-10-04)

These follow-up checks used current `main` at
`6ab6a75fb27c682dde798ae34094decd6752290e`. Raw build logs and benchmark runs
are under
`/home/keivan/checkpoints/xrdp-console/2026-10-04/compiler-optimization/`.

### Libtool relink warnings

A fresh isolated build of pinned xrdp 0.10.6.1 reproduced exactly seven
`libtool: warning: relinking` messages, for `libvnc.la`, `libxup.la`,
`libmc.la`, `libipm.la`, `libxrdp.la`, `libsesman.la`, and `libxrdpapi.la`.
There were no other warning or error lines in the captured build log.

The source `.la` metadata for those libraries says `installed=no` and records
dependencies such as the build-tree `common/libcommon.la` and `libipm/libipm.la`.
GNU libtool sets `need_relink=yes` when it sees an uninstalled `.la` dependency,
then runs the stored link command during `make install`. This rewrites the
installed `.la` dependency paths to the private install prefix and relinks the
shared object for that prefix. For example, the build-tree `libvnc.so` RUNPATH
contained both `common/.libs` and the private prefix; the installed library's
RUNPATH contains only the private prefix. Installed `.la` files are marked
`installed=yes`, and none retain an `xrdp-build-*` path.

These are install-time relocation warnings, not first-party compiler warnings
or failed links. The relink is producing correct runtime metadata and should
remain enabled; suppressing it or copying the pre-install shared objects would
leave build-tree paths in the private runtime. No source-level libtool defect
was found to fix.

### Static LTO coverage

The static LTO checks were rerun against current sources with both GCC 15.2.0
and Clang 23.1.3. GCC used `-static -flto`; Clang used `-static -flto` with
LLD. These four test executables were confirmed by `file` to be statically
linked, and all four passed under each compiler:

* `presentation-transform-unit`
* `h264-latest-frame-unit`
* `optimization-pipeline-unit`
* `scroll-motion-observer-unit`

This verifies static LTO for first-party test executables. The production
`xrdp-console` artifact is a loadable `MODULE`, so the production artifact
remains a shared object; it cannot be substituted by a fully static executable.

### Current GCC LTO + PGO pipeline result

The current-source benchmark compared GCC Release without LTO to GCC Release
with CMake IPO/LTO and profile use. The benchmark ran under Xvfb at 1920x1080
with the 1512x949 presentation request and 1512x850 image viewport. Each
configuration had three runs of 100 samples; values below are the median of
the three per-run `wall_p50_us` measurements. All six runs produced the same
output checksum (`595530542`).

| Stage | GCC no LTO | GCC LTO + PGO | Change |
| --- | ---: | ---: | ---: |
| Full-grid tile fingerprint | 2072.1 us | 2119.7 us | +2.3% |
| Presentation scaling | 2516.0 us | 2300.8 us | -8.6% |
| Full-frame BGRA to NV12 | 5277.7 us | 5444.0 us | +3.2% |
| Fused scale + direct NV12 | 5286.2 us | 5179.2 us | -2.0% |
| x264 software frame encode | 8428.3 us | 8544.5 us | +1.4% |

The measured gain is concentrated in presentation scaling; other stages are
mixed. The x264 encoder is a system shared library and is not instrumented by
this PGO build. `stage-statistics-unit` and `pixel-pipeline-bench-cli` both
passed in the profile-generate and profile-use builds.

The production module was also built as a shared object with GCC LTO and PGO.
Profile generation exercised it with
`xrdp-loader-gfx-h264-fullhd-source-smoke`; the same test passed after the
profile-use rebuild. The isolated pinned xrdp install reproduced the seven
relink warnings above. All 37 module translation units and both diagnostic
helpers had profile data, and the final profile-use build emitted no compiler
warnings. The H.264 loader test passed in 4.6 seconds after profile use.

This remains an opt-in experiment: the scaling gain is useful, but the overall
pipeline has no broad, end-to-end win established by these measurements.

## Scroll-observer snapshot allocation

The scroll observer keeps two contiguous BGRA history buffers. Before this
change, `configure()` value-initialized both buffers. At 1920x1080 that writes
16,588,800 bytes before the observer has received a frame. The buffers now use
`std::make_unique_for_overwrite<std::byte[]>`. A first full-frame capture
overwrites its entire working buffer; a first partial capture still explicitly
clears the working buffer in `beginEpisode()`. The previous buffer is first
read only after it has been swapped with a fully initialized working buffer.
The allocation remains bounded by the existing 16 MiB snapshot limit.

The new `scroll_observer_configure` stage was measured with 100 samples per
run, one warmup, the live 1920x1080 X11 source, GCC Release with
`-O3 -march=native -mtune=native`, and one benchmark process pinned to CPU 0.
The stage includes observer destruction. The table reports each run's wall
p50 in microseconds:

| Configuration | Run 1 | Run 2 | Run 3 | Median |
| --- | ---: | ---: | ---: | ---: |
| Value-initialized buffers | 4086.984 | 4206.776 | 4126.591 | 4126.591 |
| Overwrite allocation | 1.670 | 1.670 | 1.635 | 1.670 |

The measured setup cost fell by about 4.1 ms and no longer touches the two
frame-sized buffers during configuration. The sub-2-microsecond result is
close to the benchmark's clock and allocation overhead, so it should be read
as “eager frame clearing removed,” not as a precise allocation-only cost.
This measures observer setup, not end-to-end RDP latency or later capture and
scroll-processing costs. Raw runs are retained under
`/home/keivan/checkpoints/xrdp-console/2026-10-04/runtime-optimization/`.
