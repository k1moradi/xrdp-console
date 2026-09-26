# xrdp-console

`xrdp-console` is a first-party GPLv3 xrdp module for sharing the already
logged-in physical X11 desktop over RDP. The local monitor and remote client
see and control the same X11 session; the module does not start a second
desktop.

The production path is now direct X11 rather than an RFB/VNC bridge:

```text
physical X11 -> XCB/XDamage + persistent XShm capture -> bounded presentation -> negotiated RDP graphics
RDP keyboard/mouse -> xrdp-console -> XTest -> the same physical X11 session
RDP text clipboard <-> xrdp-console CLIPRDR <-> X11 CLIPBOARD selection
```

The module uses bounded damage snapshots and capture/presentation work,
client-input-first servicing, XFixes cursor updates, XTest input, inverse mouse
mapping, aspect-fit presentation, and client-requested presentation resizing
without changing the physical Xorg mode. Its text clipboard bridge uses the
RDP `cliprdr` channel and the X11 `CLIPBOARD` selection; it does not implement
file or image clipboard formats.

Graphics output is capability-gated, not forced on the client: negotiated
RDPGFX H.264/AVC420 is used when the required server encoder and surface state
are available; standard RemoteFX, xrdp GFX Planar, or classic bitmap paths are
used where appropriate as compatible alternatives. The server records the
negotiated capabilities and selected path. RDPGFX scaled-output eligibility is
diagnostic only; client-side scaling is not enabled. Presentation scaling
currently remains a bounded CPU path. The optional OpenGL 3.3 and Vulkan
targets are probes/benchmarks, not a production GPU capture or encoding path.

The repository retains the x11vnc/libvnc backend as a legacy comparison and
rollback path. `--backend vnc` measures that path; `--backend direct-x11`
measures the first-party module. Both benchmark modes use isolated private
services and leave production ports alone. A hash-pinned, patched xrdp 0.10.6.1
build is generated under `build/_deps/`; building it and activating the direct
module are explicit operations. A normal CMake install never replaces or
reconfigures the system daemon.

Known limitation: rapid page scrolling can still look uneven or tear while
frames are arriving. Scroll-motion analysis is diagnostic; it does not yet
perform production surface-copy acceleration. Validate this behavior with the
Microsoft Windows and macOS clients before treating scrolling as complete.

The end-to-end benchmark is a developer tool, `xrdp_console_bench.py`. It supports
graphics latency, input round trips, controlled compositor churn, classic RFX
or GFX negotiation, and a Linux network namespace with symmetric `tc netem`
delay, jitter, loss, and rate limits. Small C helpers provide X11 pixel and
input probes, GL workloads, scroll stimuli, and Vulkan reporting.

## Build

On Ubuntu 26.04, install the build, module, and test dependencies with:

```sh
sudo apt install \
  cmake ninja-build build-essential pkg-config python3 \
  libxcb1-dev libxcb-damage0-dev libxcb-shm0-dev libxcb-xfixes0-dev \
  libxcb-xinput-dev libxcb-xtest0-dev \
  libxkbfile-dev libx264-dev libjpeg-dev libfreetype-dev libssl-dev \
  libfuse3-dev libx11-dev libxtst-dev \
  xauth x11-utils xrdp x11vnc freerdp-x11 tigervnc-viewer xvfb
```

The FreeRDP X11 client package name depends on the Ubuntu release. Ubuntu
26.04 provides `freerdp-x11`; Ubuntu 24.04 uses `freerdp2-x11`. If following
these instructions on another Debian/Ubuntu release, check its package index
with `apt-cache search '^freerdp(-|[23]-)x11$'` and install the available X11
client package. `freerdp3-x11` may be present as a transitional package.

OpenGL and Vulkan development packages are optional: install `libgl-dev` for
the GLX helpers or `libvulkan-dev` for the Vulkan probe. To build the optional
GL capability probe, configure with
`-DXRDP_CONSOLE_ENABLE_GL_CAPABILITY_PROBE=ON`. To build the CPU pixel-pipeline
benchmark, use `-DXRDP_CONSOLE_ENABLE_PIXEL_PIPELINE_BENCH=ON` (requires x264
development files). These tools do not enable a GPU runtime path.

The ordinary out-of-tree Release build compiles the tools and unprivileged
tests. It does not fetch/build xrdp or produce the production module because
`XRDP_CONSOLE_BUILD_XRDP` defaults to `OFF`. `XRDP_CONSOLE_NATIVE` defaults to
`ON`; this optimizes for the build host and does not produce a portable binary.
Set it to `OFF` when portability is more important than host-specific tuning:

Configured CMake build trees store the checkout's absolute path. If the
repository was moved or renamed, configure a fresh build directory rather than
trying to reuse the old cache.

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXRDP_CONSOLE_NATIVE=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To install the developer tools to a user prefix and/or create packages:

```sh
cmake --install build --prefix "$HOME/.local"
cpack --config build/CPackConfig.cmake
```

To build the first-party module and the pinned patched xrdp candidate, enable
the production dependency explicitly. The upstream xrdp build is intentionally
serial to limit memory use:

```sh
cmake -S . -B build-xrdp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXRDP_CONSOLE_NATIVE=ON \
  -DXRDP_CONSOLE_BUILD_XRDP=ON
cmake --build build-xrdp --parallel 1
ctest --test-dir build-xrdp --output-on-failure
```

That build produces `build-xrdp/src/libxrdp_console.so` and the matching
private xrdp installation under `build-xrdp/_deps/xrdp-install`. Neither
`cmake --install` nor CPack activates it as the system server. The install
places benchmark helpers under `libexec/xrdp-console/benchmark/helpers` and
developer tools under the data directory. CPack produces a relocatable
`.tar.gz` and, on Debian systems, a `.deb`. The optional offline RemoteFX
batch benchmark uses the pinned codec when building xrdp, or system `rfxcodec`
development files otherwise.

For an explicit host-native build without installing:

```sh
cmake -S . -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DXRDP_CONSOLE_NATIVE=ON
cmake --build build-native
```

## Run the benchmark

The physical X server's Xauthority cookie must be readable by the invoking
user. Pass it explicitly when SDDM keeps the cookie root-only:

```sh
AUTH=/run/user/$(id -u)/xrdp-console.xauth
export XRDP_CONSOLE_HELPER_DIR="$PWD/build/bin"
export XRDP_CONSOLE_RESULTS="$PWD/results"
python3 -B tools/benchmark/xrdp_console_bench.py \
  --auth "$AUTH" --mode input-roundtrip \
  --pipeline rfx --disable-gfx-for-vnc \
  --input-churn-fps 15 --duration 20 --repetitions 2 \
  --only lan --network-mode localhost
```

For the first-party direct-X11 graphics path, use the private xrdp build and
the module produced by the native build:

```sh
python3 -B tools/benchmark/xrdp_console_bench.py \
  --backend direct-x11 --transport rdp --mode graphics \
  --auth "$AUTH" \
  --xrdp "$PWD/build/_deps/xrdp-install/sbin/xrdp" \
  --direct-module "$PWD/build/src/libxrdp_console.so" \
  --duration 20 --fps 15 --repetitions 3 \
  --network-mode localhost
```

This mode skips x11vnc, the IPv6-to-IPv4 RFB proxy, and chansrv; stages the
module into the private xrdp installation; and exercises its direct text-only
`cliprdr` channel under module code `21`. The harness's default direct graphics
request is standard RemoteFX (`--direct-graphics-transport rfx`); it can also
request classic bitmap or GFX Planar. This controlled benchmark does not
exercise the production H.264 path. By default it sets the client geometry to
the physical X11 geometry; use `--direct-allow-scaled-presentation` to test a
different initial presentation size, or `--direct-dynamic-resizing` to test
client monitor-resize requests. Graphics mode measures draw-completion to
FreeRDP-framebuffer visibility. Input-roundtrip also exercises XTest keyboard
delivery and reports the end-to-end marker stages. The classic pointer canvas
is 32x32; unsupported larger X cursors retain the previous/default RDP cursor.
Held XTest keys and buttons are released when the module session ends.

The namespace transport needs a cached sudo ticket for short-lived network
setup and cleanup commands. The benchmark itself remains a normal-user
process:

```sh
sudo -v
python3 -B tools/benchmark/xrdp_console_bench.py \
  --network-self-test --network-delay-ms 2.5 --network-jitter-ms 1
```

The `scripts/run-network-latency-matrix.sh` wrapper runs the validated 0,
2.5, and 5 ms one-way matrix at 0, 15, and 30 fps. Zero-delay controls use
loopback and no jitter; impaired cases use a temporary veth namespace. See
[`docs/network-latency.md`](docs/network-latency.md).
The timestamp definitions and percentile convention are documented in
[`docs/measurement-model.md`](docs/measurement-model.md).
The focused test plan is in [`docs/testing.md`](docs/testing.md).

Product-level workspace, result, helper, and executable overrides use the
`XRDP_CONSOLE_*` prefix. The former `XRDP_*` workspace/result/helper and
executable names remain accepted as compatibility fallbacks. Deployment-specific VNC
authentication settings retain the `XRDP_VNC_*` prefix.

The isolated x11vnc profiles are `baseline`, `lan`, `noxdamage`, and
`lan-noxdamage`. The last profile changes XDamage while retaining the LAN
speed hint, so it is the orthogonal comparison for the normal `lan` profile.

For a same-stimulus direct-VNC baseline, use the canonical benchmark's RAW-RFB
transport. It starts only a private loopback `x11vnc -nopw` and reports
wire-visible marker latency; it does not weaken the production listener's
authentication:

```sh
python3 -B tools/benchmark/xrdp_console_bench.py \
  --transport rfb --only baseline --duration 20 --fps 15 --repetitions 3
```

For the same-stimulus direct-RFB input lower bound, use the private client's
RFB KeyEvent messages. This sends the same F9 key pulse that the RDP input
mode uses and reports the same input, local-draw, returned-graphics, and total
stages while omitting xrdp and FreeRDP:

```sh
python3 -B tools/benchmark/xrdp_console_bench.py \
  --transport rfb --mode input-roundtrip --only baseline \
  --duration 20 --input-hz 5 --input-churn-fps 15 --repetitions 3
```

The normal `--backend vnc --transport rdp` report measures the complete
private x11vnc -> xrdp -> FreeRDP path. The direct backend uses the same
`--transport rdp` measurement but omits the VNC bridge. Graphics reports
include p50/p95/p99/max, misses, GL render time, process CPU/RSS, and
best-effort TCP wire bytes/sec,
retransmissions, send queue, and RTT from `ss`; input-roundtrip reports use
the same fields plus the four explicit latency stages. Direct-RFB input is a
lower-bound comparison, not a production authentication or desktop-present
measurement.

For a real VNC desktop-present comparison, use the installed TigerVNC viewer:

```sh
python3 -B tools/benchmark/xrdp_console_bench.py \
  --transport vnc-viewer --mode input-roundtrip --only baseline \
  --duration 20 --fps 15 --input-hz 5 --input-churn-fps 15 --repetitions 3
```

This starts `xtigervncviewer` on a private Xvfb display and measures the
marker after the viewer has presented it. `--transport rfb` is the direct
RFB-wire baseline; `--transport vnc-viewer` includes a real VNC client's
decode and X11 presentation, while `--transport rdp` remains the complete
x11vnc -> xrdp -> FreeRDP path. The viewer transport is a comparison tool
and does not connect to the production VNC listener.

The generated xrdp dependency intentionally does not include the old
`XRDP_VNC_PROFILE` or `VNC_SCHED` server profiling hooks. The benchmark
still accepts and summarizes those records for historical result files, but
the current native daemon does not emit them. New server-side attribution
should be added only as a small, separately justified diagnostic patch.

The `--console-lib` value is a module filename resolved from the optimized
xrdp installation's compiled module directory; do not pass the full module
path.

The benchmark never connects to the production ports unless the caller
explicitly overrides the executable/configuration inputs. Each run creates a
private x11vnc listener, xrdp configuration, RDP client display, and log
directory. Interrupting a run cleans up child processes and network
namespaces.

For a live, read-only comparison of VS Code and Firefox, use the episode
sampler. The `--rdp-port` option must match the local xrdp listener; it is not
hard-coded to 3389:

```sh
python3 -B tools/diagnostics/xrdp_console_episode_sampler.py \
  --duration 600 --interval 1 --rdp-port 3389 \
  --output results/vscode-episode.csv
```

The input-roundtrip report separates RDP input delivery, local X11 draw
completion, returned graphics, and the full T0-to-T2 round trip. The relay
used by the private VNC path is bounded and event-driven, so a blocked peer
does not consume a full CPU while a run is waiting for network backpressure.
The optional `vscode_gpu_probe.py --expect-device-regex` argument is the only
host-specific GPU assertion; without it the probe reports capabilities without
requiring a particular adapter model.

## Console profile

The first-party direct-X11 backend can be activated as the host's production
Console module after building and testing the pinned daemon and module:

```sh
SUDO_ASKPASS=/usr/bin/ssh-askpass SSH_ASKPASS_REQUIRE=force \
  sudo -A env XRDP_CONSOLE_BUILD_DIR="$PWD/build-xrdp" \
  scripts/activate-direct-console.sh
```

Activation keeps the configured RDP listener at **port 3389**, installs the
tested `libxrdp_console.so` beside the matching xrdp 0.10.6.1 daemon, and
switches `[Console]` to module code 21. It enables text clipboard and
dynamic-resize channels. Graphics choice remains capability-gated: the module
uses a negotiated and successfully initialized H.264/AVC420 path where
available, with compatible RFX/GFX Planar/classic output paths otherwise. It
does not force a codec the client did not negotiate. Unsupported
device/audio/remote-app channels are disabled for this Console session. The
operation refuses to restart while an RDP client is connected and keeps a
root-only backup under `/var/backups/xrdp-x11vnc/`.

The physical X11 mode remains unchanged when the RDP presentation is resized;
the server currently uses its CPU aspect-fit scaler and inverse pointer mapping.
Negotiated RDPGFX scaled-output eligibility is logged for investigation but is
not yet used to move scaling to the Microsoft client. Native Windows/macOS
client validation is still required for codec negotiation, resize, scrolling,
and clipboard interoperability.

To restore the previous configuration, module, and daemon override, use the
backup path printed by activation:

```sh
SUDO_ASKPASS=/usr/bin/ssh-askpass SSH_ASKPASS_REQUIRE=force \
  sudo -A scripts/activate-direct-console.sh --rollback \
  /var/backups/xrdp-x11vnc/direct-console-TIMESTAMP
```

The graphical askpass command prompts for sudo authorization without putting
the password in shell history or process arguments. Keep the RDP client on
port 3389; activation does not rewrite that setting.

The toolkit retains the legacy x11vnc profile for comparison and rollback, but
does not silently rewrite `/etc/xrdp` or install a privileged systemd unit. A
deployment that wants the Windows-like shared-console behavior can use the
installed systemd template in `share/xrdp-console/systemd` as a starting
point and review every path and Xauthority policy for its display manager. See
[`docs/console-profile.md`](docs/console-profile.md).

For the legacy VNC deployment only, the optimized xrdp daemon can be generated
from the pinned archive and patch series under `patches/xrdp/`. Build that
dependency without root privileges with:

```sh
scripts/build-optimized-xrdp.sh
```

The build uses the host's development packages by default. Private dependency
locations can be supplied through the `XRDP_CONSOLE_XRDP_CPPFLAGS`,
`XRDP_CONSOLE_XRDP_LDFLAGS`, and `XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH` cache
variables; see [`docs/xrdp-dependency.md`](docs/xrdp-dependency.md).

For the legacy VNC Console profile only, after reviewing the candidate and
closing the RDP connection, switch the xrdp service to it with:

```sh
sudo scripts/use-matched-xrdp-console-daemon.sh
```

The script backs up the systemd drop-in and restores it automatically if the
new daemon fails to stay active. It preserves the existing x11vnc profile,
clipboard channel, fixed-console geometry, direct-bitmap transport, and
resize/error-recovery behavior. It is not the direct-X11 module activation
path; use `activate-direct-console.sh` for that.

For a host intentionally using the legacy x11vnc Console profile, if this
checkout should replace the distribution `xrdp` package, run the explicit
privileged deployment after authenticating with `sudo -v`:

```sh
sudo scripts/install-console-xrdp.sh
```

That legacy deployment keeps the existing `/etc/xrdp` Console configuration and
x11vnc service, retains `xrdp-sesman` only for the physical-console clipboard
socket, removes only the distribution `xrdp` package, and leaves `xorgxrdp`
installed but unused. It stores recoverable backups under
`/var/backups/xrdp-x11vnc/`.

After a transient remote-desktop failure, collect a read-only state report
with `scripts/diagnose-console-stack.sh`. If Xorg and the logged-in session
are still alive, the non-destructive recovery helper is:

```sh
sudo scripts/restart-console-stack.sh
```

It refreshes x11vnc, restarts sesman/xrdp in dependency order, refreshes the
optional physical-console chansrv service, and validates the live Xauthority,
Xorg VT, and ports 5900/3389. It does not restart SDDM. The optional
`--with-display-manager` mode is destructive and terminates the graphical
session; use it only as an explicit control when testing whether the display
manager itself is part of the failure.

## Source layout

```text
src/                    first-party direct-X11 runtime, RDP graphics and clipboard
tools/benchmark/        benchmark client, relay, and native helper sources
tools/diagnostics/      read-only diagnostic tools
tests/                  fast unprivileged Python tests
scripts/                deployment and experiment orchestration
docs/                   design, deployment, and validation records
packaging/              systemd templates for explicit console deployment
patches/xrdp/           small, explicit deviations from pinned upstream xrdp
```

Generated builds, raw run logs, credentials, Xauthority files, production
backups, and private runtime binaries are deliberately excluded by `.gitignore`.
The xrdp source archive, patched source tree, build tree, and private install
are generated below `build/_deps/` and are not first-party source. The archive
is hash-pinned and the retained deviations are listed in
`patches/xrdp/series`; no generated dependency binaries are committed.

## License

Copyright (C) 2026 Keivan Moradi. This project is licensed under the GNU
General Public License version 3 or later. See [`LICENSE`](LICENSE) and
[`THIRD_PARTY.md`](THIRD_PARTY.md).
