# Pinned xrdp dependency

The shared-console backend uses a small patch series on top of pristine xrdp
0.10.6.1. The upstream source is generated below `build/_deps/`; it is not
first-party source and must not be edited by hand or copied into the source
tree.

## Manifest

| field | value |
| --- | --- |
| upstream | xrdp `0.10.6.1` |
| source ref | `80c52879fead585d6347df5b3c587718adf8f5a5` |
| archive | `https://github.com/neutrinolabs/xrdp/releases/download/v0.10.6.1/xrdp-0.10.6.1.tar.gz` |
| SHA-256 | `2f7beb5a3b2529c8d72dc0df9b8cdca31ab0e0c14d1e3421210f5e6ec0ab3b75` |
| patch list | `patches/xrdp/series` |
| source tree | `build/_deps/xrdp-src-<state-tag>` |
| build tree | `build/_deps/xrdp-build-<state-tag>` |
| install tree | `build/_deps/xrdp-install` |

The source, build, and ExternalProject stamp directories are keyed by the
generated state hash. CMake watches `series`, every listed patch, and the
patch-application script; changing any of them selects a fresh extracted
source tree and rebuilds the dependency instead of applying a new patch on
top of stale generated state. The current full values are available as the
internal `XRDP_CONSOLE_XRDP_PATCHSET_HASH` and
`XRDP_CONSOLE_XRDP_STATE_HASH` cache entries.

The CMake integration is opt-in because the upstream build is intentionally
serial on this 3.7 GiB host:

```sh
cmake -S . -B build-xrdp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXRDP_CONSOLE_NATIVE=ON \
  -DXRDP_CONSOLE_BUILD_XRDP=ON
cmake --build build-xrdp --parallel 1
```

The dependency build uses `-O3 -march=native -mtune=native` by default.
Configure
`XRDP_CONSOLE_XRDP_CPPFLAGS`, `XRDP_CONSOLE_XRDP_LDFLAGS`, and
`XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH` when development libraries are installed
under a private prefix. A normal system installation should provide
`libxkbfile-dev`, `libx264-dev`, JPEG, FreeType, OpenSSL, and the other xrdp
build dependencies. The private install is never activated by CMake.

## Retained patch rationale

The series is deliberately small and applies in this order:

| patch | purpose | reason retained |
| --- | --- | --- |
| `0001-xrdp-resize-state-and-failure-recovery.patch` | VNC resize state and error recovery | Prevents the fixed-console session from losing its graphics state after a client resize or failed update. |
| `0002-xrdp-fixed-console-vnc-path.patch` | fixed geometry, direct bitmap path, error propagation | Keeps the physical X11 framebuffer authoritative and makes VNC update failures visible to the session. |
The former input-first transport check is deliberately not in the canonical
series. It changes the global xrdp process loop and remains an experiment
until a clean A/B benchmark demonstrates a durable benefit.

These two patches are production compatibility changes, not benchmark knobs.
The old
fork's profiling records, incremental parser, request-ahead scheduling,
progressive flush, variable RAW quantum, first-frame/cache experiments, and
experimental GFX flow-control code are intentionally not in the series.

## Classification of the old fork

* **KEEP:** the two patches listed above; they are required by the measured
  fixed-console VNC product path.
* **TOOLING:** profiling and benchmark-only changes; these belong in the
  benchmark or diagnostic tools and remain opt-in.
* **DELETE:** parser, request-ahead, progressive-flush, cache, and GFX
  experiments that did not establish a robust latency win or were unstable.
  They must not become permanent xrdp maintenance burden.

The migration is complete: a pristine archive plus this series configures,
builds, passes `make check`, installs `xrdp` and `libvnc.so`, and the
resulting artifacts are used by the private activation workflow. Generated
dependency state belongs below `build/_deps/` and is not committed.
