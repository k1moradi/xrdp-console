# Private xrdp build-only FreeRDP gate and module staging

This is a **source-level/build-only workflow** on the corrected chansrv patch
line: PR #32 → #40 → #42 → #43 → this change. It cannot authorize X11,
Firefox, a real RDP session, clipboard access, or production installation.

## What changed

The new option `XRDP_CONSOLE_PRIVATE_BUILD_ONLY=ON` is permitted only with
`XRDP_CONSOLE_PRIVATE_XRDP_BUILD=ON`, `XRDP_CONSOLE_BUILD_XRDP=ON`, and
`BUILD_TESTING=ON`. A fresh CMake build directory is required to switch
profiles. The private build-only profile skips **registration** of loader tests
that require a verified H.264 FreeRDP executable, and does not run
`xfreerdp /buildconfig` at CMake configuration time.

The existing FreeRDP `WITH_GFX_H264=ON` plus decoder-backend checks remain
mandatory for **normal** RDP-loader integration test configurations, including
normal builds that opt into the private xrdp dependency without build-only mode.
The private build-only profile is not evidence of video, RDP or clipboard
acceptance. It retains `BUILD_TESTING=ON`, existing CTest offline/unit
registration and the pinned upstream xrdp test option.

**Do not run unfiltered CTest** on the user's host. Some pre-existing tests
exercise Xvfb, network or RDP. Codex may select only individually reviewed
offline/unit tests and the upstream xrdp unit suite.

## Native build-only configuration example (NOT EXECUTED)

These are options for Codex's separately authorized host-local build-only
activity, not a production or runtime installation command.

```sh
REL=/home/keivan/3DImageMaster/.release/xrdp-console
cmake -S "$REL/source" -B "$REL/build/abc-private-verified" \
  -DXRDP_CONSOLE_BUILD_XRDP=ON \
  -DXRDP_CONSOLE_PRIVATE_XRDP_BUILD=ON \
  -DXRDP_CONSOLE_PRIVATE_BUILD_ONLY=ON \
  -DXRDP_CONSOLE_PRIVATE_RELEASE_ROOT="$REL" \
  -DXRDP_CONSOLE_XRDP_DEPS_ROOT="$REL/build/abc-private-verified/_deps" \
  -DXRDP_CONSOLE_RUN_XRDP_TESTS=ON \
  -DBUILD_TESTING=ON
cmake --build "$REL/build/abc-private-verified" --target xrdp_upstream
cmake --build "$REL/build/abc-private-verified" --target xrdp-console
cmake --build "$REL/build/abc-private-verified" --target xrdp-console-private-stage
```

The source checkout must match the reviewed current private-build commit
and pass its actual Git/ordered 52-patch/CMake configuration provenance
checks. Host ownership, symlinks, ambient compiler environment, installed
ELF libraries and all path closures must be independently verified.

## Explicit private module stage

Normal `install(TARGETS xrdp-console)` still targets the normal project
install path; it is **not** used for private build-only staging.

`xrdp-console-private-stage` is an explicit named build target and depends
on both `xrdp_upstream` and `xrdp-console`. It runs a pure CMake script to
copy **the actual built first-party** `libxrdp_console.so` to:

`<hash-keyed-private-xrdp-install>/lib/xrdp/libxrdp_console.so`

The guarded stage refuses noncanonical paths, symlink ancestors,
missing private xrdp artifacts, wrong module name, untagged install
directories, system or protected prefixes and differently hashed existing
modules. It logs the SHA-256 of the staged bytes. It does not run any target
binary, create IPC, start a listener, install services or write into
`/usr/local`, `/run` or `build-direct-console/_deps/xrdp-install`.

`cmake --install` and generic packaging targets are **not** authorized by
this procedure.

The staged module's ELF RUNPATH and xrdp source identity still require
separate native static inspection. A successful stage does **not** prove
the loader can open the module, or that its synthetic CLIPRDR channel can
reach chansrv.

## Offline regression

`xrdp-private-build-only-stage-unit` exercises inert file copies with
mocked upstream executable/library bytes and CMake script-mode checks.
It does not build, load or run xrdp/FreeRDP or touch X11/Firefox/clipboard.

The runtime A/B/C screenshot experiment remains NO-GO until the private
session/channel, authenticated displays, verified requestor identity and
browser File receipt are demonstrated under fresh explicit authorization.
