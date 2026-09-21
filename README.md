# xrdp-console

`xrdp-console` is the first-party GPLv3 shared-console project. The current
migration slice contains the measured VNC bridge, its developer tooling, and
the first direct-X11 C++ runtime vertical slice: XCB/XDamage/XShm capture into
xrdp's classic bitmap callbacks.

The current measured paths are:

```text
physical X11 display -> x11vnc -> xrdp libvnc.so -> RDP client
physical X11 display -> xrdp-console XCB/XDamage/XShm -> classic RDP bitmap -> RDP client
```

The default `--backend vnc` measures the existing workstation path where an
RDP connection must show the same LXQt session as the physical monitor.
`--backend direct-x11` measures the first-party XCB/XDamage/XShm module against
the same private xrdp and FreeRDP stages, without starting x11vnc or the RFB
proxy. Both modes keep benchmark services on private ports. The workspace
also carries a reproducible, host-native xrdp candidate with fixed-console
geometry, direct-bitmap/error propagation, and resize-state recovery. It is
built and activated explicitly; a normal CMake install never replaces the
system daemon.

The end-to-end benchmark is a developer tool, `xrdp_console_bench.py`. It supports
graphics latency, input round trips, controlled compositor churn, classic RFX
or GFX negotiation, and a Linux network namespace with symmetric `tc netem`
delay, jitter, loss, and rate limits. Small C helpers provide X11 pixel and
input probes, GL workloads, scroll stimuli, and Vulkan reporting.

## Build

On Debian or Ubuntu, install the development dependencies first:

```sh
sudo apt install \
  cmake ninja-build build-essential pkg-config python3 \
  libxcb1-dev libxcb-damage0-dev libxcb-xfixes0-dev \
  libx11-dev libxtst-dev libgl-dev libvulkan-dev \
  xauth x11-utils xrdp x11vnc freerdp3-x11 tigervnc-viewer xvfb
```

Configure an out-of-tree Release build. For this machine, use the native
option so every project target is optimized for the host CPU:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXRDP_CONSOLE_NATIVE=ON \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel 1
ctest --test-dir build --output-on-failure
cmake --install build
cpack --config build/CPackConfig.cmake
```

The build installs benchmark helper binaries under
`libexec/xrdp-console/benchmark/helpers` and developer tools under the data
directory. Run the benchmark directly from
`tools/benchmark/xrdp_console_bench.py` or its installed data path. CPack produces
a relocatable `.tar.gz` and, on Debian systems, a `.deb`. The optional offline
codec probe is built when both `rfxcodec` and
`x264` development files are available; its absence does not affect the
end-to-end benchmark. Vulkan development files are optional too; without them
the Vulkan capability helper is omitted while the rest of the toolkit remains
buildable.

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

This mode is intentionally graphics-only while XTest input is not implemented.
It skips x11vnc, the IPv6-to-IPv4 RFB proxy, chansrv, GFX/drdynvc, and dynamic
resizing; stages the module into the private xrdp installation; sets `code=0`
for the direct classic-bitmap path; and forces the FreeRDP geometry to the
physical X11 geometry. The reported marker latency is therefore the
draw-completion to FreeRDP-framebuffer-visible stage needed for the first
direct-backend comparison.

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

The toolkit documents the measured x11vnc profile for a local network, but it
does not silently rewrite `/etc/xrdp` or install a privileged systemd unit. A
deployment that wants the Windows-like shared-console behavior can use the
installed systemd template in `share/xrdp-console/systemd` as a starting
point and review every path and Xauthority policy for its display manager. See
[`docs/console-profile.md`](docs/console-profile.md).

The optimized daemon is generated from the pinned xrdp archive and the small
patch series under `patches/xrdp/`. Build it without root privileges with:

```sh
scripts/build-optimized-xrdp.sh
```

The build uses the host's development packages by default. Private dependency
locations can be supplied through the `XRDP_CONSOLE_XRDP_CPPFLAGS`,
`XRDP_CONSOLE_XRDP_LDFLAGS`, and `XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH` cache
variables; see [`docs/xrdp-dependency.md`](docs/xrdp-dependency.md).

After reviewing the candidate and closing the RDP connection, switch only the
xrdp service to it with:

```sh
sudo scripts/use-matched-xrdp-console-daemon.sh
```

The script backs up the systemd drop-in and restores it automatically if the
new daemon fails to stay active. It preserves the existing x11vnc profile,
clipboard channel, fixed-console geometry, direct-bitmap transport, and
resize/error-recovery behavior.

If this checkout should replace the distribution `xrdp` package for this
machine, run the explicit privileged deployment from the same terminal after
authenticating with `sudo -v`:

```sh
sudo scripts/install-console-xrdp.sh
```

That deployment keeps the existing `/etc/xrdp` Console configuration and
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
src/                    first-party runtime (direct-X11 backend in development)
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
