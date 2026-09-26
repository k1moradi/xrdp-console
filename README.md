# xrdp-console

`xrdp-console` shares an already-running Linux X11 desktop through xrdp. A
person at the physical monitor and an RDP user see and control the same
desktop; connecting does not create a second graphical session.

The product is a first-party xrdp module, not a VNC proxy or merely a benchmark
suite. Its live path is:

```text
X11 desktop ── XCB / XDamage / XShm ── presentation ── RDP graphics ── client
RDP keyboard and pointer ── xrdp-console / XTest ── same X11 desktop
RDP text clipboard ── CLIPRDR / X11 CLIPBOARD selection ── same desktop
```

## What it provides

- Direct capture of the physical X11 desktop with XCB, XDamage, and a persistent
  MIT-SHM capture buffer.
- Bounded damage, capture, and presentation work so input and disconnect
  handling continue to get service during graphics updates.
- Keyboard and mouse injection through XTest, XFixes cursor updates, cleanup of
  held keys/buttons on disconnect, and inverse pointer mapping.
- Presentation resizing without changing the physical Xorg mode. The current
  server-side aspect-fit path scales the source and leaves letterbox regions
  where the aspect ratios differ.
- A first-party text clipboard bridge for the RDP `cliprdr` channel and the
  X11 `CLIPBOARD` selection.
- Capability-based graphics selection. Depending on what the client and server
  actually negotiate and initialize, output can use RDPGFX H.264/AVC420,
  standard RemoteFX, GFX Planar, or classic bitmap. The module does not force a
  codec the client did not negotiate.

RDPGFX scaled-output capability is currently observed for diagnostics only;
the client-side scaling path is not enabled. Scaling is CPU-based, and the
optional OpenGL/Vulkan probes do not add GPU processing to the runtime.

## Current limitations

- Fast page scrolling can still appear uneven or tear while updates are in
  flight. Scroll-motion analysis is diagnostic; production surface-copy scroll
  acceleration is not enabled yet.
- The module's clipboard bridge is text-focused; it does not implement image
  clipboard formats or file transfer.
- Windows and macOS Microsoft RDP clients have been exercised against this
  backend. Revalidate codec negotiation, resize, clipboard, and reconnect after
  transport changes; smooth, tear-free scrolling remains an open client-side
  acceptance issue.
- The host must expose an X11 display that the xrdp service can access. The
  deployment on this machine uses `DISPLAY=:0` and an explicit Xauthority file.

## Install prerequisites

For Ubuntu 26.04, the following packages cover the pinned xrdp/module build,
unit/integration tests, and local RDP smoke client:

```sh
sudo apt install \
  cmake ninja-build build-essential pkg-config python3 \
  libxcb1-dev libxcb-damage0-dev libxcb-shm0-dev libxcb-xfixes0-dev \
  libxcb-xinput-dev libxcb-xtest0-dev \
  libpam0g-dev libxkbfile-dev libxfixes-dev libxrandr-dev \
  libx264-dev libjpeg-dev libfreetype-dev libssl-dev \
  libfuse3-dev libx11-dev libxtst-dev \
  xauth x11-utils xvfb xrdp freerdp-x11
```

FreeRDP's X11 package name varies by release: Ubuntu 26.04 provides
`freerdp-x11`; Ubuntu 24.04 uses `freerdp2-x11`. On another Debian/Ubuntu
release, check with `apt-cache search '^freerdp(-|[23]-)x11$'`. FreeRDP is used
for local smoke tests; Microsoft clients connect to the server directly.

OpenGL and Vulkan development packages are optional and only needed for
diagnostic/profiling tools. For example, install `libgl-dev` and configure with
`-DXRDP_CONSOLE_ENABLE_GL_CAPABILITY_PROBE=ON` to build the OpenGL 3.3 capability
probe. This is not a production GPU requirement.

## Build and test

The production module is built together with the project's hash-pinned,
patched xrdp 0.10.6.1 dependency. The upstream build is intentionally serial
to limit memory use:

```sh
cmake -S . -B build-xrdp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXRDP_CONSOLE_NATIVE=ON \
  -DXRDP_CONSOLE_BUILD_XRDP=ON
cmake --build build-xrdp --parallel 1
ctest --test-dir build-xrdp --output-on-failure
```

`XRDP_CONSOLE_NATIVE=ON` tunes binaries for the build host and is not suitable
for distributing one binary across different CPUs; set it to `OFF` for a
portable build. `XRDP_CONSOLE_BUILD_XRDP` defaults to `OFF`; without it CMake
builds the standalone tools/tests but not the xrdp module.

The module is produced at `build-xrdp/src/libxrdp_console.so`; the matching
private xrdp install is under `build-xrdp/_deps/xrdp-install`. CMake install
and CPack create ordinary files/packages only—they do not modify the running
xrdp service. A configured CMake build directory records the checkout's
absolute path, so use a fresh build directory if the repository is moved or
renamed.

The test suite includes native unit tests, authenticated-Xvfb X11 integration
tests, module lifecycle/reconnect checks, and (when the pinned xrdp build is
enabled) upstream xrdp tests and a FreeRDP loader/pixel smoke test. See
[`docs/testing.md`](docs/testing.md) for test coverage and
[`docs/xrdp-dependency.md`](docs/xrdp-dependency.md) for the pinned dependency
and retained patch rationale.

## Activate the direct Console module

Activation is explicit and requires administrator privileges. Build and test
the production configuration above first, disconnect any current RDP session,
then run:

```sh
sudo env XRDP_CONSOLE_BUILD_DIR="$PWD/build-xrdp" \
  scripts/activate-direct-console.sh
```

The activation script validates the candidate daemon, module, Xauthority,
service state, and listener before changing the host. It switches the
`[Console]` profile to module code `21`, enables text clipboard and dynamic
presentation resizing, installs the matching module/chansrv artifacts, and
restarts xrdp. The configured listener remains on **port 3389**. It refuses to
restart while a client is connected and prints a root-only rollback backup
directory under `/var/backups/xrdp-x11vnc/`.

To roll back, use the exact backup path printed by activation:

```sh
sudo scripts/activate-direct-console.sh --rollback \
  /var/backups/xrdp-x11vnc/direct-console-TIMESTAMP
```

Connect from the Windows or macOS Microsoft RDP client to the host's existing
address and port `3389`; no client-side codec switch is required. The server
logs the negotiated graphics capabilities and the output path selected for
that session. For live server diagnostics, use:

```sh
sudo journalctl -u xrdp -f
```

## Development and diagnostics

The primary product is the direct-X11 Console module. The repository also
contains benchmark and diagnostic tools; they are for validating latency,
resource use, input responsiveness, and client-visible pixels, not a separate
runtime requirement. Start with
[`docs/measurement-model.md`](docs/measurement-model.md) and
[`docs/network-latency.md`](docs/network-latency.md). The older VNC/RFB
comparison helpers remain in the tree as historical tooling; the production
Console path does not use them.

Source areas:

```text
src/module/       xrdp module ABI and lifecycle
src/x11/          XCB connection, capture, damage, cursor, and input
src/core/         geometry, damage state, and bounded presentation scaling
src/rdp/          graphics transports, scheduling, and RDP adapters
src/clipboard/    text clipboard protocol and X11 selection bridge
tools/             benchmarks and diagnostics
scripts/           build, activation, rollback, and support utilities
patches/xrdp/      explicit patches against the pinned xrdp dependency
docs/              design, test, deployment, and measurement notes
```

## License

Copyright (C) 2026 Keivan Moradi. Licensed under the GNU General Public
License version 3 or later. See [`LICENSE`](LICENSE) and
[`THIRD_PARTY.md`](THIRD_PARTY.md).
