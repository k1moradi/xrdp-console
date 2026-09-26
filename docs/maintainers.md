# Maintainer workflow

The canonical product build is the native direct-X11 build. It builds the
first-party module and its matching pinned xrdp runtime, then runs CTest:

```sh
scripts/build-direct-console.sh
```

It defaults to `build-direct-console/`; set `XRDP_CONSOLE_BUILD_DIR` to use another
directory. The script does not activate or modify the host service. Run
`scripts/activate-direct-console.sh` separately, only after reviewing tests
and disconnecting RDP clients.

For portable source/unit-only CI, CMake can be used without building the
production module or pinned xrdp runtime:

```sh
cmake -S . -B build-portable -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DXRDP_CONSOLE_NATIVE=OFF -DXRDP_CONSOLE_BUILD_XRDP=OFF
cmake --build build-portable --parallel
ctest --test-dir build-portable --output-on-failure
```

This portable configuration is not the deployable direct-X11 product build.

Before publishing a change:

1. run the direct-X11 native build and its CTest suite;
2. if changing install rules, run `cmake --install` into a staging prefix and inspect the file list;
3. if changing packaging, run CPack from the same configured build and inspect the package contents;
4. run `python3 -B tools/benchmark/xrdp_console_bench.py --help`;
5. run the network self-test only when a cached sudo ticket is available.

The supported native direct-Console build fetches the pinned xrdp archive,
applies `patches/xrdp/series`, builds the first-party module and upstream
runtime, and runs CTest with:

```sh
scripts/build-direct-console.sh
```

The script is intentionally serial for this low-memory host. It builds only
inside the selected build directory; activation is a separate, explicit,
privileged step using `scripts/activate-direct-console.sh`. A normal CMake
build/install never changes the running service. `docs/xrdp-dependency.md` and
`patches/xrdp/README.md` are the source of truth for the upstream pin and
retained changes.

Do not commit Xauthority files, systemd backups, raw isolated-run logs, private
xrdp binaries, or dependency archives. Keep one
canonical benchmark (`tools/benchmark/xrdp_console_bench.py`) and add a mode or a
focused helper instead of creating another top-level benchmark variant.
Do not edit generated xrdp files under `build/_deps/` or add first-party runtime
code there. First-party code belongs under `src/`, benchmark and diagnostic
code belongs under `tools/`, and upstream deviations belong under
`patches/xrdp/`.
