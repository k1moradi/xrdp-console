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
`libxkbfile-dev`, `libx264-dev`, `libfuse3-dev`, JPEG, FreeType, OpenSSL, and the
other xrdp build dependencies. FUSE is explicitly enabled for chansrv file
clipboard support. The private install is never activated by CMake.

## Retained patch rationale

The series is deliberately small and applies in this order:

| patch | purpose | reason retained |
| --- | --- | --- |
| `0001-xrdp-resize-state-and-failure-recovery.patch` | VNC resize state and error recovery | Prevents the fixed-console session from losing its graphics state after a client resize or failed update. |
| `0002-xrdp-fixed-console-vnc-path.patch` | fixed geometry, direct bitmap path, end-to-end update error propagation, and first-party capability code `21` | Keeps the physical X11 framebuffer authoritative, makes update failures visible to the session, and gives the direct module an explicit complete-framebuffer/smooth-scroll classification. |
| `0003-xrdp-console-input-priority.patch` | console-only transport priority | Drains a bounded burst of queued RDP input and disconnects before direct-X11 backend work, then performs one transport check afterward, while preserving the legacy service order for other module codes. |
| `0004-xrdp-console-own-rfx-encoder.patch` | first-party synchronous RFX ownership | Prevents the asynchronous generic encoder thread from retaining pixel buffers owned by the direct module. |
| `0005-librfxcodec-unaligned-stream-access.patch` | defined unaligned RFX stream access | Removes UBSan-confirmed misaligned typed loads/stores in the x86 stream macros while preserving wire bytes. |
| `0006-xrdp-unaligned-stream-access.patch` | defined unaligned xrdp stream access | Removes UBSan-confirmed potentially misaligned typed loads/stores from `common/parse.h` and unchecked UTF-16 stream helpers in `common/parse.c` while preserving little-endian wire bytes. |
| `0007-xrdp-keyboard-layout-hex-conversion.patch` | defined keyboard-layout parsing | Replaces two `g_htoi()` calls with the existing `g_atoix()` parser after UBSan found invalid 32-bit shifts for normal `0x`-prefixed keyboard-layout IDs. Custom bare hexadecimal values such as `409` now parse as decimal; the distributed configuration uses the supported `0x00000409` form. |
| `0008-xrdp-console-planar-frame-ids.patch` | Console GFX Planar fallback frame IDs and acknowledgement logging | Gives each Planar frame a monotonically advancing ID and avoids logging a missing generic encoder for every acknowledgement when Console code `21` intentionally owns the graphics path. |
| `0009-xrdp-console-preserve-gfx-dirty-regions.patch` | Console GFX Planar dirty-region preservation | Prevents sparse Console damage from becoming a large bounding-box Planar encode; legacy module codes retain their existing behavior. |

These nine patches are production compatibility and correctness changes, not
benchmark knobs.
The old
fork's profiling records, incremental parser, request-ahead scheduling,
progressive flush, variable RAW quantum, first-frame/cache experiments, and
experimental GFX flow-control code are intentionally not in the series.

## Classification of the old fork

* **KEEP:** the nine patches listed above; they are required by the measured
  fixed-console/direct-console product path and address concrete transport,
  parser, codec, and keyboard-layout correctness issues.
* **TOOLING:** profiling and benchmark-only changes; these belong in the
  benchmark or diagnostic tools and remain opt-in.
* **DELETE:** parser, request-ahead, progressive-flush, cache, and GFX
  experiments that did not establish a robust latency win or were unstable.
  They must not become permanent xrdp maintenance burden.

The migration is complete: a pristine archive plus this series configures,
builds, passes `make check`, installs `xrdp` and `libvnc.so`, and the
resulting artifacts are used by the private activation workflow. Generated
dependency state belongs below `build/_deps/` and is not committed.
