# Maintainer workflow

Use a clean out-of-tree build and keep generated files out of commits:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DXRDP_CONSOLE_NATIVE=ON \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel 1
ctest --test-dir build --output-on-failure
cmake --install build
cpack --config build/CPackConfig.cmake
```

`XRDP_CONSOLE_NATIVE` defaults to `ON` for this host and enables `-O3`,
`-march=native`, and `-mtune=native` for all first-party targets. Set it to
`OFF` when producing binaries for another machine or for portable CI.

Before publishing a change:

1. run `ctest --test-dir build --output-on-failure`;
2. run `cmake --install build --prefix "$PWD/_dist"` and inspect the file list;
3. run `cpack --config build/CPackConfig.cmake`;
4. run `python3 -B tools/benchmark/xrdp_console_bench.py --help`;
5. run the network self-test only when a cached sudo ticket is available.

The shared-console daemon is a separate, host-specific dependency build. Fetch
the pinned archive, apply `patches/xrdp/series`, build, test, and install it
under `build/_deps/` with:

```sh
scripts/build-optimized-xrdp.sh
```

The script is intentionally serial and runs the xrdp unit suite. It installs
only below `build/_deps/xrdp-install`; activation requires the explicit
privileged helper and therefore never happens as a side effect of a normal
CMake install. `docs/xrdp-dependency.md` and `patches/xrdp/README.md` are the
source of truth for the upstream pin and retained changes.

Do not commit Xauthority files, VNC password files, systemd backups, raw
isolated-run logs, private xrdp binaries, or dependency archives. Keep one
canonical benchmark (`tools/benchmark/xrdp_console_bench.py`) and add a mode or a
focused helper instead of creating another top-level benchmark variant.
Do not edit generated xrdp files under `build/_deps/` or add first-party runtime
code there. First-party code belongs under `src/`, benchmark and diagnostic
code belongs under `tools/`, and upstream deviations belong under
`patches/xrdp/`.
