# First-party runtime

The first runtime milestone is the `xrdp-console` module ABI/lifecycle
boundary. With `XRDP_CONSOLE_BUILD_XRDP=ON`, CMake builds
`libxrdp_console.so` as a C++23 module that exports only the upstream
`mod_init()`/`mod_exit()` entry points and keeps the callback-table details in
`module/`.

It validates basic geometry and module parameters, constructs and destroys its
owned C++ context, and provides no-op lifecycle callbacks for xrdp. It does
not yet capture X11 or implement graphics, input, clipboard, resize, threads,
or performance policy.

The current checkout still uses the experimental x11vnc bridge for live
measurements. Its benchmark and diagnostic programs live under `tools/`; the
optimized xrdp dependency is generated under `build/_deps/` from the pin and
patch series documented in `docs/xrdp-dependency.md` while the direct-X11
backend is developed incrementally.
