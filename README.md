# xrdp-console

`xrdp-console` is the first-party GPLv3 shared-console project. The current
migration slice contains the measured VNC bridge and its developer tooling;
the direct-X11 runtime is intentionally reserved under `src/` for the next
implementation step.

The current measured path is:

```text
physical X11 display -> x11vnc -> xrdp libvnc.so -> RDP client
```

It measures the path that matters for a workstation where an RDP connection
must show the same LXQt session as the physical monitor. The benchmark uses
the versions installed by the host distribution and keeps all benchmark
services on private ports. The workspace also carries a reproducible,
host-native xrdp candidate with the measured VNC optimizations and the
upstream resize-state fix. It is built and activated explicitly; a normal
CMake install never replaces the system daemon.

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
  libx11-dev libxtst-dev libgl-dev libvulkan-dev \
  xrdp x11vnc freerdp3-x11 tigervnc-viewer xvfb
```

Configure an out-of-tree Release build. `-march=native` is optional and
should only be used for a benchmark build that will run on the same machine:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
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

For a local native helper build:

```sh
cmake -S . -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DXRDP_VNC_NATIVE=ON
cmake --build build-native
```

## Run the benchmark

The physical X server's Xauthority cookie must be readable by the invoking
user. Pass it explicitly when SDDM keeps the cookie root-only:

```sh
AUTH=/run/user/$(id -u)/xrdp-console.xauth
export XRDP_VNC_BENCH_HELPER_DIR="$PWD/build/bin"
export XRDP_VNC_RESULTS="$PWD/results"
python3 -B tools/benchmark/xrdp_console_bench.py \
  --auth "$AUTH" --mode input-roundtrip \
  --pipeline rfx --disable-gfx-for-vnc \
  --input-churn-fps 15 --duration 20 --repetitions 2 \
  --only lan --network-mode localhost
```

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

The normal `--transport rdp` report measures the complete private x11vnc ->
xrdp -> FreeRDP path. Graphics reports include p50/p95/p99/max, misses, GL
render time, process CPU/RSS, and best-effort TCP wire bytes/sec,
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

For server-side attribution, enable the opt-in profile on the private xrdp
process:

```sh
python3 -B tools/benchmark/xrdp_console_bench.py \
  --transport rdp --only baseline --duration 20 --fps 15 --repetitions 3 \
  --xrdp "$PWD/build/prefix/xrdp-optimized-resize/sbin/xrdp" \
  --console-lib libvnc.so \
  --xrdp-env XRDP_VNC_PROFILE=1
```

The candidate emits one `VNC_PERF` aggregate per VNC framebuffer update and
per RDP painter update. The fields cover VNC parser time, framebuffer copy,
bitmap handling, encoding, send time, PDU count, and bitmap stream bytes.
`wire_bytes` in the server log means bytes in the RDP bitmap PDU stream; it
does not include TCP/TLS framing. The environment variable is false by
default and does not change the production daemon unless its service
environment is explicitly configured. It also emits `VNC_SCHED` lines with
per-update request wait, parser/process, server flush, and next-request-gap
times (and, for the request-ahead experiment, request lead), plus rectangle
and raw-byte counts. Profiling is observational and
adds logging work, so use `XRDP_VNC_PROFILE=0` for clean latency A/B tests and
use the profile timings for attribution rather than as a latency baseline.

To correlate a physical marker with the VNC-side paint and classic RDP
flush, add a point coordinate to the same private-daemon invocation:

```sh
python3 -B tools/benchmark/xrdp_console_bench.py \
  --transport rdp --mode input-roundtrip --only lan \
  --xrdp-env XRDP_VNC_PROFILE=1 \
  --xrdp-env XRDP_VNC_PROFILE_POINT_X=1260 \
  --xrdp-env XRDP_VNC_PROFILE_POINT_Y=70
```

The xrdp log then contains `VNC_POINT` records with monotonic paint,
flush-start, send-return, result, and decoded red/blue `marker_state` values.
The configured coordinate must be inside the benchmark marker; the default
input-roundtrip marker is `(1260,70)`. The benchmark correlates only a
successful point record with the matching marker state, rather than pairing
same-coordinate updates by time alone. `VNC_SCHED` records are also parsed
and summarized by the benchmark; they report request wait, parser/process,
server flush, and the gap before the next RFB request. Point profiling is
opt-in and should be used for attribution runs, not clean latency A/B
measurements.

The opt-in progressive-visibility experiment closes and reopens the classic
bitmap painter while an incremental RAW VNC rectangle is still being received.
It keeps `server_paint_rect()` as the backing-store update and does not issue
another RFB update request:

```sh
python3 -B tools/benchmark/xrdp_console_bench.py \
  --transport rdp --mode input-roundtrip --only lan \
  --xrdp-env XRDP_VNC_INCREMENTAL_FB=1 \
  --xrdp-env XRDP_VNC_PROGRESSIVE_FLUSH=1 \
  --xrdp-env XRDP_VNC_PROGRESSIVE_FLUSH_BYTES=262144
```

The switch is disabled by default and is accepted only for incremental RAW,
unsuppressed, non-GFX, direct-bitmap output. CopyRect, cursor, resize, the
blocking parser, and the normal next-request boundary are unchanged. Its
`VNC_SCHED` additions are `first_flush_us`, `progressive_flushes`,
`bytes_before_first_flush`, and `logical_update_us`; keep profiling disabled
when using the latency result as an A/B comparison.

The bounded request-ahead experiment sends at most one incremental RFB request
while a single nonempty RAW rectangle in the current incremental update is
still being processed. It is disabled by default and is restricted to the
same incremental, unsuppressed, non-GFX, direct-bitmap path:

```sh
python3 -B tools/benchmark/xrdp_console_bench.py \
  --transport rdp --mode input-roundtrip --only lan \
  --pipeline rfx --disable-gfx-for-vnc \
  --xrdp-env XRDP_VNC_INCREMENTAL_FB=1 \
  --xrdp-env XRDP_VNC_REQUEST_AHEAD=1
```

The invariant is at most one outstanding incremental request. The scheduler
profile reports `next_request_lead_us` when the next request was written before
the current RDP flush completed; the normal `next_request_gap_us` remains the
post-flush gap. Keep this as an experiment until repeated fixed-variable A/B
runs show a durable latency improvement.

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

The optimized daemon source is kept under
`third_party/xrdp-0.10.6.1-optimized`. Build it without root privileges with:

```sh
scripts/build-optimized-xrdp.sh
```

When x264 is installed by the distribution under `/usr`, set the sysroot to
`/` (the value must contain the `usr/` subtree):

```sh
XRDP_X264_SYSROOT=/ scripts/build-optimized-xrdp.sh
```

After reviewing the candidate and closing the RDP connection, switch only the
xrdp service to it with:

```sh
sudo scripts/use-matched-xrdp-console-daemon.sh
```

The script backs up the systemd drop-in and restores it automatically if the
new daemon fails to stay active. It preserves the existing x11vnc profile,
clipboard channel, and VNC scheduling/encoding optimizations.

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
third_party/            transitional optimized xrdp dependency
deps/                   transitional build sysroots
```

Generated builds, raw run logs, credentials, Xauthority files, production
backups, and private runtime binaries are deliberately excluded by `.gitignore`.
The optimized xrdp source and its small dependency sysroots remain temporarily
under `third_party/` and `deps/` so the current candidate can be rebuilt after
a reboot or package upgrade. The next dependency-migration step will replace
these checked-in inputs with a pinned build-directory checkout and a small
patch series. The checked-in `libxkbfile` sysroot is an x86_64 Linux build
input; other architectures should use the distribution's development package
or provide an equivalent sysroot through the build script's existing paths.

## License

Copyright (C) 2026 Keivan Moradi. This project is licensed under the GNU
General Public License version 3 or later. See [`LICENSE`](LICENSE) and
[`THIRD_PARTY.md`](THIRD_PARTY.md).
