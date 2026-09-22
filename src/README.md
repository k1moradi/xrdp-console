# First-party runtime

The first runtime milestone is the `xrdp-console` module ABI/lifecycle
boundary. With `XRDP_CONSOLE_BUILD_XRDP=ON`, CMake builds
`libxrdp_console.so` as a C++23 module that exports only the upstream
`mod_init()`/`mod_exit()` entry points and keeps the callback-table details in
`module/`.

It validates basic geometry and module parameters, connects to the configured
X11 display through XCB, records the root window and separate
source/presentation geometry, and integrates the XCB socket with xrdp's
wait-object loop. It tracks raw XDamage rectangles into a bounded,
coalesced region through an explicit event sink, captures the supported 24-bit
X11/32-bit-storage layout through one persistent MIT-SHM arena, and sends
one classic bitmap update transaction through xrdp. It also provides a narrow,
bounded text-only CLIPBOARD bridge using the direct `cliprdr` channel and XCB
selection ownership. PRIMARY, files, images, INCR transfers, and richer
clipboard formats remain future work; the existing presentation scaling,
resize, and XTest input paths remain intentionally bounded and classic-bitmap
focused.

The current checkout still uses the experimental x11vnc bridge for live
measurements. Its benchmark and diagnostic programs live under `tools/`; the
optimized xrdp dependency is generated under `build/_deps/` from the pin and
patch series documented in `docs/xrdp-dependency.md` while the direct-X11
backend is developed incrementally.
