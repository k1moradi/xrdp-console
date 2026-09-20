# Maintainer workflow

Use a clean out-of-tree build and keep generated files out of commits:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel 1
ctest --test-dir build --output-on-failure
cmake --install build
cpack --config build/CPackConfig.cmake
```

The default build is portable. `-DXRDP_CONSOLE_NATIVE=ON` enables `-march=native`
for local helper microbenchmarks and must not be used for a binary intended for
other machines.

Before publishing a change:

1. run `ctest --test-dir build --output-on-failure`;
2. run `cmake --install build --prefix "$PWD/_dist"` and inspect the file list;
3. run `cpack --config build/CPackConfig.cmake`;
4. run `python3 -B tools/benchmark/xrdp_console_bench.py --help`;
5. run the network self-test only when a cached sudo ticket is available.

The shared-console daemon is a separate, host-specific build. Rebuild it from
the persistent patched source with:

```sh
scripts/build-optimized-xrdp.sh
```

The script is intentionally serial and runs the xrdp unit suite. It installs
only below `build/prefix/`; activation requires the explicit privileged helper
and therefore never happens as a side effect of a normal CMake install.

Do not commit Xauthority files, VNC password files, systemd backups, raw
isolated-run logs, private xrdp binaries, or dependency archives. Keep one
canonical benchmark (`tools/benchmark/xrdp_console_bench.py`) and add a mode or a
focused helper instead of creating another top-level benchmark variant. The
transitional xrdp source is currently the deliberate exception: preserve its
upstream notices, and update the persistent build script when its patch
sequence changes. Do not add new runtime code below `third_party/`; first-party
code belongs under `src/`, while benchmark and diagnostic code belongs under
`tools/`.
