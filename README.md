# xrdp-console

`xrdp-console` is a first-party xrdp module for sharing an already-running
Linux X11 desktop. A user at the physical monitor and an RDP client see and
control the same X11 session; connecting does **not** create a second desktop.

The current runtime path is:

```text
physical X11 desktop
    │
    ├─ XCB + XDamage + MIT-SHM capture
    │
    ├─ xrdp-console presentation / graphics scheduling
    │
    └─ patched xrdp 0.10.6.1 ── RDP ── client

RDP keyboard / pointer ── XTest ── same X11 desktop
RDP clipboard ── patched xrdp-chansrv ── X11 CLIPBOARD
```

## Project status

This project is under active performance and correctness development. The
current source contains the optimized direct-X11 path, the pinned xrdp patch
series, H.264/AVC420 output, RemoteFX and Planar fallbacks, interaction
prioritization, resize handling, clipboard support, and capability-gated
client offloads.

It is **not yet production-complete**. A Microsoft macOS live test showed
major screen tearing while scrolling, especially under heavy CPU load, and
the display later froze before the client was disconnected. The current H.264
path now derives dirty tiles from one immutable X11 source snapshot per
presentation batch, and the new temporal-coherence test checks 2,000 complete
FreeRDP client frames per run. All five default/contention/offload A/B runs
currently pass with zero incoherent frames. This is strong automated evidence,
but native Windows/macOS retesting is still required; frame-stall recovery is
also not yet validated end-to-end.

For live testing, keep a rollback path available and use the feature gates
documented below to isolate client offloads.

## What the current implementation provides

- Direct capture of the physical X11 desktop with XCB, XDamage, and MIT-SHM.
- Bounded capture and presentation work so input and disconnect processing
  continue to receive service during graphics updates.
- Keyboard and pointer injection through XTest, XFixes cursor updates, physical
  pointer reflection, and cleanup of held input state on disconnect.
- Dynamic presentation resizing without changing the physical Xorg mode.
- RDPGFX H.264/AVC420 when negotiated and usable, with standard RemoteFX,
  GFX Planar, and classic bitmap fallback paths.
- An asynchronous xrdp H.264 encoder path for Console sessions, plus adaptive
  ACK/queue-depth pacing in the patched xrdp runtime.
- Capability-gated client-side scaled-surface mapping.
- Exact-verified same-surface scroll reuse.
- A bounded 16-slot verified RDPGFX bitmap cache.
- Automatic SSSE3 AVC420 conversion on supported x86 CPUs, with a scalar
  fallback on unsupported CPUs/platforms.
- Clipboard through the pinned xrdp-chansrv path. The current xrdp patchset
  supports Unicode text and image clipboard transfers using PNG and
  CF_DIB/CF_DIBV5-compatible paths. The Console profile does not enable file
  transfer.

The repository pins xrdp **0.10.6.1** and applies the production patch series
listed in [`patches/xrdp/series`](patches/xrdp/series). See
[`patches/xrdp/README.md`](patches/xrdp/README.md) and
[`docs/xrdp-dependency.md`](docs/xrdp-dependency.md) for the retained patch
rationale.

## Supported host assumptions

The current deployment scripts target the existing development/test host
configuration. They are **not yet a complete clean-machine installer**.

`activate-direct-console.sh` expects all of the following to already exist:

- an active `xrdp.service`;
- an active `xrdp-console-chansrv.service`;
- an X11 physical desktop reachable as `DISPLAY=:0`;
- a readable `XAUTHORITY` path in the xrdp service environment;
- xrdp already listening on TCP port `3389`;
- no active RDP client when activation or restart is requested.

If `xrdp-console-chansrv.service` is not already provisioned on a new host,
stop there: the current repository does not yet provide a clean-host installer
for that service.

The tested package instructions below are for Ubuntu 26.04. Package names may
differ on other distributions.

## Install build prerequisites

Install the dependencies used by the native module, pinned xrdp build, X11
integration tests, and the isolated H.264-capable FreeRDP test client:

```sh
sudo apt update

sudo apt install -y \
  git cmake ninja-build build-essential pkg-config python3 \
  xrdp iproute2 xauth xvfb x11-utils \
  libpam0g-dev libfuse3-dev libssl-dev \
  libx264-dev libjpeg-dev libfreetype-dev \
  libavcodec-dev libavutil-dev libswscale-dev libusb-1.0-0-dev \
  libx11-dev libxext-dev libxinerama-dev libxcursor-dev \
  libxdamage-dev libxfixes-dev libxi-dev libxkbfile-dev \
  libxrandr-dev libxrender-dev libxtst-dev \
  libxcb1-dev libxcb-damage0-dev libxcb-shm0-dev \
  libxcb-xfixes0-dev libxcb-xinput-dev libxcb-xtest0-dev
```

OpenGL and Vulkan development packages are optional. They are used only by
diagnostic/profiling tools; there is currently no production GPU rendering or
GPU conversion backend.

## Update an existing checkout before building

Do not assume a long-lived checkout is current. Older branches used different
project names, paths, build targets, and test coverage.

Start by inspecting the checkout without modifying it:

```sh
cd ~/xrdp-console

git status --short
git branch --show-current
git rev-parse HEAD

git fetch origin
git rev-parse origin/main
```

If `git status --short` is empty and you intend to build current `main`:

```sh
git switch main
git pull --ff-only
```

Verify the checkout:

```sh
git rev-parse HEAD
grep '^project' CMakeLists.txt
head -n 12 tools/benchmark/helpers/xrdp_codec_bench.c
```

The current tree uses `project(xrdp_console ...)`, and
`tools/benchmark/helpers/xrdp_codec_bench.c` defines
`_POSIX_C_SOURCE 200809L` before `<time.h>`.

If build output mentions the old package name `xrdp-vnc-bench` or the old path
`src/c/xrdp_codec_bench.c`, you are building an obsolete branch or stale build
tree.

Do not reuse a CMake build directory that was configured from a different
checkout or an old project layout.

## Production build and test: use the canonical script

For the actual direct-console runtime, use:

```sh
cd ~/xrdp-console
scripts/build-direct-console.sh
```

This is the supported production-candidate build workflow. It:

1. uses `build-direct-console/` by default;
2. builds an isolated FreeRDP 3.31.0 client under `build-test-freerdp/` when a
   suitable client is not already present;
3. verifies that the selected FreeRDP client reports RDPGFX H.264 plus an
   H.264 decoder backend;
4. configures `XRDP_CONSOLE_BUILD_XRDP=ON`;
5. downloads and hash-verifies xrdp 0.10.6.1;
6. applies the repository's ordered xrdp patch series;
7. builds the first-party module and pinned xrdp with host-native Release
   tuning;
8. runs the pinned upstream xrdp tests and the project CTest suite;
9. stops immediately if any build or test step fails.

The default build uses host-native optimization:

```text
-O3 -march=native -mtune=native
```

so the resulting runtime is intended for the machine that built it, not as a
portable binary for arbitrary CPUs.

The main output locations are:

```text
build-direct-console/src/libxrdp_console.so
build-direct-console/_deps/xrdp-install/
build-test-freerdp/install/
```

The build script does **not** activate the module, restart xrdp, or alter the
live desktop.

### Never continue after a failed build

A successful `ctest` invocation after a failed build does not mean the project
passed validation: CTest can run whatever subset of test executables happened
to exist already.

Do not run install, packaging, activation, or deployment after a failed build
or failed test.

If the canonical build succeeds and you want to rerun CTest explicitly:

```sh
ctest --test-dir build-direct-console --output-on-failure
```

## FreeRDP integration-test client

The project does not use an arbitrary `xfreerdp` found in `PATH` for the loader
tests. The canonical build uses the project's pinned isolated client.

To build or refresh it explicitly:

```sh
scripts/build-test-freerdp.sh
```

The client is built under `build-test-freerdp/` and does not replace Ubuntu's
system FreeRDP packages.

A custom test client may be selected with:

```sh
XRDP_CONSOLE_FREERDP_EXECUTABLE=/absolute/path/to/xfreerdp3 \
  scripts/build-direct-console.sh
```

The custom executable must pass the same `/buildconfig` checks. A missing or
incapable H.264 client is a configuration/build failure; the current canonical
workflow is not supposed to silently skip the H.264 loader requirement.

## Before activation

Activation modifies the live xrdp configuration and restarts services. Build
and test first.

Check the required host state:

```sh
systemctl is-active xrdp.service
systemctl is-active xrdp-console-chansrv.service

systemctl show xrdp.service -p Environment --value

ss -ltn 'sport = :3389'
ss -tn state established 'sport = :3389'
```

Confirm that:

- both services report `active`;
- the xrdp service environment contains `DISPLAY=:0`;
- its `XAUTHORITY` file exists and is readable;
- port `3389` is listening;
- there is no established RDP client.

The activation script performs these checks again and refuses to proceed when
they are not satisfied.

## Activate the tested build

With all RDP clients disconnected:

```sh
cd ~/xrdp-console

sudo env XRDP_CONSOLE_BUILD_DIR="$PWD/build-direct-console" \
  scripts/activate-direct-console.sh
```

Activation validates the built daemon/module, checks the embedded build
revision, preserves port `3389`, backs up the existing configuration and
runtime files, installs the tested module plus matching pinned chansrv, updates
the `[Console]` profile to module code `21`, and restarts the required services.

The script prints a root-only backup directory such as:

```text
/var/backups/xrdp-console/direct-console-YYYYMMDD-HHMMSS
```

Keep that exact path for rollback.

### Roll back

Use the exact backup directory printed by activation:

```sh
cd ~/xrdp-console

sudo scripts/activate-direct-console.sh --rollback \
  /var/backups/xrdp-console/direct-console-YYYYMMDD-HHMMSS
```

The rollback restores the previous xrdp configuration, module, chansrv binary,
service drop-in, service state, and port-3389 listener.

## Restart an already activated installation

Disconnect all RDP clients first, then run:

```sh
cd ~/xrdp-console
sudo scripts/restart-direct-console.sh
```

The restart helper refuses to restart xrdp while a client is connected and
verifies that the services return and port `3389` is listening.

## Diagnostics

For a read-only service/listener/log snapshot:

```sh
cd ~/xrdp-console
sudo scripts/diagnose-direct-console.sh
```

For live xrdp logs:

```sh
sudo journalctl -u xrdp.service -f
```

For graphics timing counters, enable runtime profiling in the xrdp service
environment:

```ini
[Service]
Environment=XRDP_CONSOLE_PROFILE=1
```

Then reload systemd and restart only after all clients have disconnected:

```sh
sudo systemctl daemon-reload
sudo scripts/restart-direct-console.sh
```

The profile reports capture, AVC420 conversion, H.264 submission, frame-ACK
wait, damage, presentation, and bitmap-cache counters.

## Graphics feature gates

Three RDPGFX performance paths are requested by default. For these gates, an
unset variable or exact `1` enables the request; any other explicit value
disables it. Actual use remains conditional on negotiated client capabilities
and runtime safety checks.

| Variable | Default | Purpose |
| --- | --- | --- |
| `XRDP_CONSOLE_CLIENT_SCALE` | requested | Use client-side scaled-surface mapping when the negotiated RDPGFX capability and geometry permit it; otherwise server-side scaling is used. |
| `XRDP_CONSOLE_CLIENT_SCROLL` | requested | Permit exact-verified `SurfaceToSurface` scroll reuse on eligible H.264 frames. |
| `XRDP_CONSOLE_CLIENT_CACHE` | requested | Permit the bounded, byte-verified 16-slot RDPGFX bitmap cache when capability, geometry, and ACK-residency requirements are satisfied. |
| `XRDP_CONSOLE_CLIENT_CACHE_OBSERVE` | off | Collect cache-observation diagnostics without enabling it implicitly. Exact `1` is required. |

These settings do not force client capabilities and do not change the RDP
listener port.

### Recommended isolation setup while tearing is under investigation

The current live issue is scrolling tearing under load. To establish an
all-off client-offload baseline, create a separate systemd drop-in rather than
editing the activation script's generated `upstream-local.conf`.

For example:

```sh
sudo mkdir -p /etc/systemd/system/xrdp.service.d

sudo tee /etc/systemd/system/xrdp.service.d/90-xrdp-console-features.conf \
  >/dev/null <<'EOF'
[Service]
Environment=XRDP_CONSOLE_CLIENT_SCALE=0
Environment=XRDP_CONSOLE_CLIENT_SCROLL=0
Environment=XRDP_CONSOLE_CLIENT_CACHE=0
Environment=XRDP_CONSOLE_PROFILE=1
EOF

sudo systemctl daemon-reload
```

After disconnecting every RDP client:

```sh
cd ~/xrdp-console
sudo scripts/restart-direct-console.sh
```

Then enable only one optimization at a time by changing its value to `1`,
reloading systemd, restarting with no connected client, and repeating the same
test workload.

Remove the separate drop-in to restore default policy:

```sh
sudo rm /etc/systemd/system/xrdp.service.d/90-xrdp-console-features.conf
sudo systemctl daemon-reload
cd ~/xrdp-console
sudo scripts/restart-direct-console.sh
```

## Testing scope and current gaps

The current test suite covers:

- first-party C/C++ unit tests;
- generation-safe H.264 capture/transmission state;
- AVC420 conversion and RDPGFX command layout;
- scroll-motion discovery and exact reuse classification;
- verified bitmap-cache behavior and ACK residency;
- presentation geometry/scaling;
- X11 damage, XShm capture, input, pointer, cursor, and clipboard behavior
  under authenticated Xvfb;
- module lifecycle and reconnect handling;
- pinned xrdp upstream tests including Console-specific patch regressions;
- FreeRDP loader/pixel integration, including H.264 GFX.

The loader/pixel smokes use the project's H.264-capable FreeRDP build, not a
FreeRDP client discovered from `PATH`. The H.264 visual-coherence matrix
scrolls a generation-marked tiled pattern and samples 2,000 complete client
frame snapshots per run. It compares default behavior with scroll reuse and
cache disabled, including controlled single-CPU-contention runs. A failing
run preserves its first incoherent client screenshot and the xrdp/FreeRDP logs
under `build-direct-console/test-artifacts/<test-name>/`. This stress test is
not a substitute for Microsoft Windows/macOS interoperability; native-client
scrolling and freeze recovery remain manual release gates.

The next correctness work is native-client retesting, followed by explicit
stuck-frame detection/recovery if real-client evidence still shows stalls.

See [`docs/testing.md`](docs/testing.md) for the detailed test inventory.

## Clipboard behavior

The production Console deployment uses the matching pinned xrdp-chansrv
binary. The current xrdp patchset supports:

- Unicode text clipboard;
- PNG image clipboard;
- incoming CF_DIB/CF_DIBV5 image data;
- serving Linux images as CF_DIB or PNG;
- bounded retry/failure behavior for X11 text-selection conversion.

The first-party module also contains a narrow text-only CLIPRDR bridge for
environments where chansrv is not in use; it deliberately disables itself when
xrdp reports that chansrv owns the channel.

The activation profile disables RDPDR, so file transfer is not part of the
current Console feature set.

## Presentation and client behavior

The physical X11 display mode is not changed by an RDP resize.

When client-side scaled mapping is negotiated and passes preflight, the module
can map a native H.264 surface into the requested presentation geometry.
Otherwise it performs server-side presentation scaling.

Clients should connect to the host's normal address on TCP port `3389`. No
client-side codec override is required; codec/capability selection is based on
normal negotiation.

The server logs the negotiated graphics mode and actual output path for each
session.

## Developer-only CMake builds

A plain CMake configuration is **not equivalent** to the production direct
Console build.

`XRDP_CONSOLE_BUILD_XRDP` defaults to `OFF`. With that default, the pinned
xrdp dependency and production `xrdp-console` module target are not built.

For a portable tooling/unit-test build that deliberately does not build the
pinned runtime:

```sh
cmake -S . -B build-tools -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXRDP_CONSOLE_NATIVE=OFF \
  -DXRDP_CONSOLE_BUILD_XRDP=OFF \
  -DXRDP_CONSOLE_ENABLE_CODEC_BENCH=OFF

cmake --build build-tools --parallel 1
ctest --test-dir build-tools --output-on-failure
```

Use a dedicated directory such as `build-tools/`; do not point this workflow at
`build-direct-console/`.

`cmake --install` and CPack create ordinary install/package artifacts. They do
**not** activate the direct Console module or restart the live xrdp service.
Do not use them as a substitute for `scripts/activate-direct-console.sh`.

Optional benchmark/probe targets can be enabled explicitly through the CMake
options in [`CMakeLists.txt`](CMakeLists.txt).

## Repository layout

```text
src/module/        xrdp module ABI, lifecycle, service loop
src/x11/           XCB/XDamage/XShm capture, cursor, pointer, input
src/core/          geometry, damage state, presentation/scaling primitives
src/rdp/           graphics transports, H.264 state, offloads, schedulers
src/clipboard/     fallback first-party text CLIPRDR bridge
patches/xrdp/      ordered production patch series for pinned xrdp 0.10.6.1
scripts/           canonical build, FreeRDP build, activation, restart, diagnostics
tests/             first-party unit/integration/loader tests
tools/benchmark/   measurement and stress tools
tools/diagnostics/ capability and runtime diagnostics
docs/              testing, dependency, validation, measurement, maintainer notes
```

## Further documentation

- [`docs/testing.md`](docs/testing.md) — current test matrix and limitations
- [`docs/xrdp-dependency.md`](docs/xrdp-dependency.md) — pinned xrdp build and
  patch rationale
- [`patches/xrdp/README.md`](patches/xrdp/README.md) — patch-by-patch production
  behavior
- [`docs/validation.md`](docs/validation.md) — validation notes
- [`docs/measurement-model.md`](docs/measurement-model.md) — benchmark and
  measurement model
- [`docs/network-latency.md`](docs/network-latency.md) — network-latency test
  setup
- [`docs/maintainers.md`](docs/maintainers.md) — maintainer workflow

## License

Copyright (C) 2026 Keivan Moradi.

Licensed under the GNU General Public License version 3 or later. See
[`LICENSE`](LICENSE), [`NOTICE`](NOTICE), and
[`THIRD_PARTY.md`](THIRD_PARTY.md).
