# xrdp-console

`xrdp-console` shares an already-running Linux X11 desktop through xrdp. A
person at the physical monitor and an RDP user see and control the same
desktop; connecting does not create a second graphical session. The product is
a first-party direct-X11 xrdp module. Its live path is:

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
- Presentation resizing without changing the physical Xorg mode. By default,
  server-side aspect-fit scaling keeps the full source visible; eligible
  clients may opt into protocol-gated client-side scaled output.
- A first-party text clipboard bridge for the RDP `cliprdr` channel and the
  X11 `CLIPBOARD` selection.
- Capability-based graphics selection. Depending on what the client and server
  actually negotiate and initialize, output can use RDPGFX H.264/AVC420,
  standard RemoteFX, GFX Planar, or classic bitmap. The module does not force a
  codec the client did not negotiate.
- Opt-in RDPGFX client-offload paths for scaled-output mapping, verified
  SurfaceToSurface scroll reuse, bitmap-cache observation, and a bounded
  verified bitmap cache. They refine scheduler-selected H.264 updates and keep
  H.264 fallback; each is disabled unless its service environment gate is
  exactly `1`.

Run a baseline with all four gates unset, then test one gate at a time:

| Gate | Behavior |
| --- | --- |
| `XRDP_CONSOLE_CLIENT_SCALE=1` | Use client-side scaled-output mapping only when negotiated capabilities are eligible; otherwise retain server-side scaling. |
| `XRDP_CONSOLE_CLIENT_SCROLL=1` | Allow conservative same-surface scroll copies only after scheduler selection, high-confidence motion matching, and exact pixel verification. |
| `XRDP_CONSOLE_CLIENT_CACHE_OBSERVE=1` | Observe bitmap-cache reuse without changing rendering. |
| `XRDP_CONSOLE_CLIENT_CACHE=1` | Enable the bounded 16-slot verified bitmap cache, with H.264 fallback on uncertainty or failure. |

These gates are configured in the xrdp service environment, not in the RDP
client, so Microsoft clients continue using the normal server address and
**port 3389**. A prior Microsoft macOS session advertised RDPGFX 10.7 flags
`0x82`, which is not eligible for scaled-output mapping; the server-side
presentation fallback is expected for that capability set. OpenGL/Vulkan
probes remain diagnostic only and do not accelerate the runtime.

To test a gate, disconnect RDP clients, add exactly one `Environment=` line to
the xrdp service drop-in (for example,
`Environment=XRDP_CONSOLE_CLIENT_SCROLL=1`), reload systemd, and restart xrdp.
Remove that line and restart for the all-gates-off baseline. Do not change the
service's RDP port; clients use port `3389` throughout.

## Current limitations

- The gated scroll-copy path and bitmap cache still need careful live
  validation with Microsoft Windows/macOS clients. Leave the gates off if a
  client shows tearing, stale content, or worse input responsiveness.
- The module's clipboard bridge is text-focused; it does not implement image
  clipboard formats or file transfer.
- The baseline has been exercised with Microsoft Windows/macOS clients, but
  this newly gated offload stack still needs per-gate native-client validation.
  Smooth, tear-free scrolling remains an acceptance requirement.
- The host must expose an X11 display that the xrdp service can access. The
  deployment on this machine uses `DISPLAY=:0` and an explicit Xauthority file.

## Install prerequisites

For Ubuntu 26.04, install the build and deployment prerequisites:

```sh
sudo apt install \
  cmake ninja-build build-essential pkg-config python3 \
  libxcb1-dev libxcb-damage0-dev libxcb-shm0-dev libxcb-xfixes0-dev \
  libxcb-xinput-dev libxcb-xtest0-dev \
  libpam0g-dev libxkbfile-dev libxfixes-dev libxrandr-dev \
  libx264-dev libjpeg-dev libfreetype-dev libssl-dev \
  libfuse3-dev libx11-dev libxtst-dev \
  xauth x11-utils xvfb xrdp
```

The server and its Xvfb tests do not require a FreeRDP client. To run the
optional local RDP smoke test on Ubuntu 26.04, install:

```sh
sudo apt install freerdp3-x11
```

Ubuntu 26.04 provides the FreeRDP 3 X11 client; `freerdp2-x11` is not the
available package name. Package names vary between Debian/Ubuntu releases. To
find the client package available on another release, run:

```sh
apt-cache search '^freerdp.*x11$'
```

If no client package is available, you can still build and run the server-side
tests; use a Windows or macOS Microsoft RDP client for manual connection tests.
Ubuntu's packaged FreeRDP may be built without an H.264 GFX decoder. In that
case, the capability-gated `xrdp-loader-gfx-h264-odd-scaled-smoke` test is
reported as skipped; installing FFmpeg alone cannot add a decoder to an already
compiled FreeRDP binary. The ordinary CTest suite and other loader modes still
run with the packaged client.

To run the H.264 GFX loader test, keep the distro FreeRDP package installed and
build a separate test client against Ubuntu's FFmpeg development packages:

```sh
sudo apt install \
  git cmake ninja-build build-essential pkg-config libssl-dev \
  libavcodec-dev libavutil-dev libswscale-dev libusb-1.0-0-dev \
  libx11-dev libxext-dev libxinerama-dev libxcursor-dev libxdamage-dev \
  libxfixes-dev libxi-dev libxkbfile-dev libxrandr-dev libxrender-dev
scripts/build-test-freerdp.sh
```

The build script verifies the resulting client's H.264 GFX and FFmpeg build
flags and installs it only under `build-test-freerdp/`; it does not uninstall
or overwrite Ubuntu's FreeRDP packages. It disables unrelated Kerberos, CUPS,
and PC/SC integrations while retaining RDPGFX and client channels. It prints
the exact executable path to use in the subsequent
`scripts/build-direct-console.sh` command. The client path can also be passed
directly to CMake using
`-DXRDP_CONSOLE_FREERDP_EXECUTABLE=/absolute/path/to/xfreerdp3`. See
[`docs/testing.md`](docs/testing.md) for the capability-gated test behavior.

The Ubuntu `xrdp` package supplies the host service/configuration framework
used by the activation script. The active daemon is then switched to this
project's pinned, locally built xrdp; the distribution daemon is not the
direct-X11 module.

OpenGL and Vulkan development packages are optional and only needed for
diagnostic/profiling tools. For example, install `libgl-dev` and configure with
`-DXRDP_CONSOLE_ENABLE_GL_CAPABILITY_PROBE=ON` to build the OpenGL 3.3 capability
probe. This is not a production GPU requirement.

## Build and test

Run the supported native build script. It configures and compiles the
first-party module together with the hash-pinned, patched xrdp 0.10.6.1
dependency, serially, then runs CTest:

```sh
scripts/build-direct-console.sh
```

The script defaults to `build-direct-console/`; override that with
`XRDP_CONSOLE_BUILD_DIR`. It enables `XRDP_CONSOLE_NATIVE=ON` (`-march=native`),
which is appropriate for this host but not for distributing one binary across
different CPUs. For a portable manual build, set `XRDP_CONSOLE_NATIVE=OFF`.
`XRDP_CONSOLE_BUILD_XRDP` defaults to `OFF` in ordinary CMake configurations;
the supported direct-console build script enables it.

The build, activation, and benchmark-matrix scripts are source-checkout
workflows because they use the pinned dependency/build tree. They are not
installed as if they could operate from a binary package; the installed
diagnostic and restart helpers are independent of the checkout.

The module is produced at `build-direct-console/src/libxrdp_console.so`; the
matching private xrdp install is under
`build-direct-console/_deps/xrdp-install`. CMake install and CPack create
ordinary files/packages only—they do not modify the running xrdp service. A
configured CMake build directory records the checkout's absolute path, so use
a fresh build directory if the repository is moved or renamed.

The test suite includes native unit tests for the offload paths,
authenticated-Xvfb X11 integration tests, module lifecycle/reconnect checks,
and (when the pinned xrdp build is enabled) upstream xrdp tests and FreeRDP
loader/pixel smokes. Use an H.264-capable test client to avoid skipping the
H.264 smoke. See
[`docs/testing.md`](docs/testing.md) for test coverage and
[`docs/xrdp-dependency.md`](docs/xrdp-dependency.md) for the pinned dependency
and retained patch rationale.

## Activate the direct Console module

Activation is explicit and requires administrator privileges. Build and test
the production configuration above first, disconnect any current RDP session,
then run:

```sh
sudo env XRDP_CONSOLE_BUILD_DIR="$PWD/build-direct-console" \
  scripts/activate-direct-console.sh
```

The activation script validates the candidate daemon, module, Xauthority,
service state, and listener before changing the host. It switches the
`[Console]` profile to module code `21`, enables text clipboard and dynamic
presentation resizing, installs the matching module/chansrv artifacts, and
restarts xrdp. The configured listener remains on **port 3389**. Activation
does not enable the four client-offload gates; leave them unset for baseline,
then enable only one at a time in the xrdp service environment. It refuses to
restart while a client is connected and prints a root-only rollback backup
directory under `/var/backups/xrdp-console/`.

To roll back, use the exact backup path printed by activation:

```sh
sudo scripts/activate-direct-console.sh --rollback \
  /var/backups/xrdp-console/direct-console-TIMESTAMP
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
contains benchmark and diagnostic tools for validating latency, resource use,
input responsiveness, client-visible pixels, and opt-in RDPGFX offload paths.
Start with
[`docs/measurement-model.md`](docs/measurement-model.md) and
[`docs/network-latency.md`](docs/network-latency.md). The benchmark defaults
to direct-X11; these tools do not change the production transport.

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
