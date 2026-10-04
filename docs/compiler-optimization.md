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
