#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure the local x11vnc -> xrdp -> RDP-client display path.

The benchmark starts only user-owned, loopback services:

    physical X :0 -> isolated x11vnc -> isolated xrdp -> FreeRDP on Xvfb

    An OpenGL workload toggles a solid red/blue marker on the physical display.
    The default RDP transport timestamps the completed GL swap and polls the
    corresponding pixel in the FreeRDP window.  ``--transport rfb`` instead
    starts a private no-password x11vnc and timestamps the same marker in RAW
    RFB bytes on the loopback socket.  ``--transport vnc-viewer`` puts an
    actual TigerVNC viewer between that private server and a private Xvfb
    display.  In input-roundtrip mode the RFB and viewer transports send the
    same F9 key stimulus as the RDP mode, so all paths share the physical X11
    marker and four-stage timing model.  No transport connects to production
    ports 5900/3389 or modifies system configuration.
"""

from __future__ import annotations

import argparse
import errno
import math
import os
import re
import secrets
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

# This tool is frequently run once as root for namespace diagnostics and then
# as the desktop user.  Avoid leaving root-owned __pycache__ entries in the
# shared benchmark workspace.
sys.dont_write_bytecode = True


def environment_value(canonical: str, legacy: str) -> str | None:
    """Read a canonical product setting with a legacy compatibility alias."""
    value = os.environ.get(canonical)
    if value is None:
        value = os.environ.get(legacy)
    return value


# Resolve paths from the installed/source tree, never from the caller's home.
# The benchmark is often invoked through sudo for namespace setup; relying on
# Path.home() in that case points at the administrator's home and loses the
# project's helpers.
SCRIPT_DIR = Path(__file__).resolve().parent
SCRIPT_WORKSPACE = SCRIPT_DIR.parents[1]
workspace_value = environment_value(
    "XRDP_CONSOLE_WORKSPACE", "XRDP_VNC_WORKSPACE")
WORKSPACE = Path(workspace_value or str(SCRIPT_WORKSPACE))
HOME = Path.home()
results_value = environment_value("XRDP_CONSOLE_RESULTS", "XRDP_VNC_RESULTS")
if results_value is not None:
    RESULTS = Path(results_value)
elif (WORKSPACE / "src").is_dir():
    RESULTS = WORKSPACE / "results"
else:
    # An installed launcher must not try to create /usr/results. Keep run
    # artifacts beside the caller unless an explicit result directory is set.
    RESULTS = Path.cwd() / "xrdp-console-results"
ISOLATED = RESULTS / "isolated-runs"


def first_path(*candidates: Path) -> Path:
    """Select the first existing executable, retaining a useful fallback."""
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return candidates[0]


def first_directory(*candidates: Path) -> Path:
    """Select the first existing directory, retaining a useful fallback."""
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return candidates[0]


installed_helper_dir = (
    WORKSPACE.parent / "libexec" / "xrdp-console" /
    "benchmark" / "helpers"
)
helper_value = environment_value(
    "XRDP_CONSOLE_HELPER_DIR", "XRDP_VNC_BENCH_HELPER_DIR")
HELPER_DIR = Path(
    helper_value or str(first_directory(WORKSPACE / "build/bin", installed_helper_dir))
)
GPU_STIMULUS = HELPER_DIR / "x11vnc-gpu-stimulus"
PIXEL_PROBE = HELPER_DIR / "x11-pixel-probe"
KEY_STIMULUS = HELPER_DIR / "x11vnc-latency-stimulus"
KEY_INJECTOR = HELPER_DIR / "x11-xtest-key"
V6_V4_PROXY = SCRIPT_DIR / "rfb_v6_v4_proxy.py"
PRIVATE_FREERDP = ISOLATED / "rdp-bench-root/usr/bin/xfreerdp"
freerdp_override = environment_value(
    "XRDP_CONSOLE_FREERDP", "XRDP_VNC_FREERDP")
FREERDP = first_path(
    Path(freerdp_override) if freerdp_override else PRIVATE_FREERDP,
    Path("/usr/bin/xfreerdp"),
)
viewer_override = environment_value(
    "XRDP_CONSOLE_VIEWER", "XRDP_VNC_VIEWER")
VNC_VIEWER = first_path(
    Path(viewer_override) if viewer_override else Path("/usr/bin/xtigervncviewer"),
    Path("/usr/bin/vncviewer"),
)
xrdp_override = environment_value("XRDP_CONSOLE_XRDP", "XRDP_VNC_XRDP")
XRDP = first_path(
    Path(xrdp_override) if xrdp_override else Path("/usr/local/sbin/xrdp"),
    Path("/usr/sbin/xrdp"),
)
x11vnc_override = environment_value(
    "XRDP_CONSOLE_X11VNC", "XRDP_VNC_X11VNC")
X11VNC = first_path(
    Path(x11vnc_override) if x11vnc_override else Path("/usr/bin/x11vnc"),
)
# Keep xrdp and chansrv from the same installation when a private prefix is
# explicitly selected. Mixing socket-root builds can otherwise make a run
# wait forever for a socket that the other binary never creates.
chansrv_override = environment_value(
    "XRDP_CONSOLE_CHANSRV", "XRDP_VNC_CHANSRV")
CHANSRV = first_path(
    Path(chansrv_override) if chansrv_override else XRDP.parent / "xrdp-chansrv",
    Path("/usr/local/sbin/xrdp-chansrv"),
    Path("/usr/sbin/xrdp-chansrv"),
    Path("/usr/lib/xrdp/xrdp-chansrv"),
)

VNC_PROFILES = {
    "baseline": (),
    "lan": ("-speeds", "lan"),
    "noxdamage": ("-noxdamage",),
    "lan-noxdamage": ("-speeds", "lan", "-noxdamage"),
}

# X11 keysym for F9, the key consumed by x11vnc-latency-stimulus.
RFB_KEY_F9 = 0xFFC6


class TcpSnapshot:
    """Best-effort counters for the private benchmark TCP connection."""

    def __init__(self, bytes_sent: int | None = None,
                 bytes_received: int | None = None,
                 retransmissions: int | None = None,
                 send_queue: int | None = None,
                 rtt_ms: float | None = None) -> None:
        self.bytes_sent = bytes_sent
        self.bytes_received = bytes_received
        self.retransmissions = retransmissions
        self.send_queue = send_queue
        self.rtt_ms = rtt_ms

    @property
    def wire_bytes(self) -> int | None:
        if self.bytes_sent is None or self.bytes_received is None:
            return None
        return self.bytes_sent + self.bytes_received


def tcp_snapshot(port: int) -> TcpSnapshot | None:
    """Read the established private connection from ``ss`` when available."""
    try:
        result = subprocess.run(
            ["ss", "-tin"], capture_output=True, text=True, check=False,
        )
    except OSError:
        return None
    lines = result.stdout.splitlines()
    for index, line in enumerate(lines):
        fields = line.split()
        if len(fields) < 5 or fields[0] not in {
                "ESTAB", "SYN-SENT", "SYN-RECV", "FIN-WAIT-1", "FIN-WAIT-2",
                "CLOSE-WAIT", "LAST-ACK", "CLOSING", "TIME-WAIT"}:
            continue
        if not any(re.search(rf":{port}(?:%|$)", endpoint)
                   for endpoint in fields[3:5]):
            continue
        detail = ""
        if index + 1 < len(lines) and lines[index + 1][:1].isspace():
            detail = lines[index + 1]
        values: dict[str, str] = {}
        for token in detail.split():
            key, separator, value = token.partition(":")
            if separator:
                values[key] = value
        retransmissions = None
        if "retrans" in values:
            try:
                retransmissions = int(values["retrans"].split("/", 1)[0])
            except ValueError:
                pass
        rtt_ms = None
        if "rtt" in values:
            try:
                rtt_ms = float(values["rtt"].split("/", 1)[0])
            except ValueError:
                pass
        bytes_sent = None
        bytes_received = None
        for key, target in (("bytes_sent", "bytes_sent"),
                            ("bytes_received", "bytes_received")):
            if key in values:
                try:
                    if target == "bytes_sent":
                        bytes_sent = int(values[key])
                    else:
                        bytes_received = int(values[key])
                except ValueError:
                    pass
        try:
            send_queue = int(fields[2])
        except ValueError:
            send_queue = None
        return TcpSnapshot(bytes_sent, bytes_received, retransmissions,
                           send_queue, rtt_ms)
    return None


class RfbClient:
    """Minimal RFB 3.x client for an isolated, RAW-only benchmark."""

    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.width = 0
        self.height = 0
        self.bytes_sent = 0
        self.bytes_received = 0

    def _recv_exact(self, size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            chunk = self.sock.recv(size - len(data))
            if not chunk:
                raise RuntimeError("RFB peer closed the connection")
            data.extend(chunk)
            self.bytes_received += len(chunk)
        return bytes(data)

    def _send(self, data: bytes) -> None:
        self.sock.sendall(data)
        self.bytes_sent += len(data)

    def connect(self) -> None:
        server_version = self._recv_exact(12)
        if not server_version.startswith(b"RFB "):
            raise RuntimeError(f"invalid RFB version {server_version!r}")
        self._send(b"RFB 003.008\n")

        if server_version[8:11] == b"003":
            security_type = struct.unpack(">I", self._recv_exact(4))[0]
            if security_type != 1:
                raise RuntimeError(
                    f"private RFB server requires unsupported security {security_type}"
                )
        else:
            security_types = self._recv_exact(1)[0]
            if security_types == 0:
                reason_length = struct.unpack(">I", self._recv_exact(4))[0]
                reason = self._recv_exact(reason_length).decode(errors="replace")
                raise RuntimeError(f"RFB security negotiation failed: {reason}")
            offered = self._recv_exact(security_types)
            if 1 not in offered:
                raise RuntimeError(
                    f"private RFB server does not offer security None: {offered!r}"
                )
            self._send(b"\x01")
        result = struct.unpack(">I", self._recv_exact(4))[0]
        if result != 0:
            raise RuntimeError(f"RFB security result was {result}")

        self._send(b"\x01")  # ClientInit: shared desktop.
        init = self._recv_exact(24)
        self.width, self.height = struct.unpack(">HH", init[:4])
        name_length = struct.unpack(">I", init[20:24])[0]
        self._recv_exact(name_length)
        if self.width <= 0 or self.height <= 0:
            raise RuntimeError(f"invalid RFB framebuffer {self.width}x{self.height}")

        # 32-bit little-endian true colour with RGB shifts.  This makes the
        # marker decode independent of x11vnc's native XImage format.
        pixel_format = struct.pack(
            ">BBBBHHHBBBBBB",
            32, 24, 0, 1, 255, 255, 255, 16, 8, 0, 0, 0, 0,
        )
        self._send(b"\x00\x00\x00\x00" + pixel_format)
        self._send(struct.pack(">BBH i", 2, 0, 1, 0))  # RAW only.
        self.request(incremental=False)

    def request(self, *, incremental: bool) -> None:
        self._send(struct.pack(
            ">BBHHHH", 3, int(incremental), 0, 0, self.width, self.height,
        ))

    def key_event(self, *, pressed: bool, keysym: int = RFB_KEY_F9) -> None:
        """Send one RFB KeyEvent using an X11 keysym."""
        self._send(struct.pack(">BBxxI", 4, int(pressed), keysym))

    def send_key_pulse(self, keysym: int = RFB_KEY_F9) -> int:
        """Send F9 down/up and return the pre-send monotonic timestamp."""
        event_ns = time.monotonic_ns()
        self.key_event(pressed=True, keysym=keysym)
        self.key_event(pressed=False, keysym=keysym)
        return event_ns

    @staticmethod
    def _pixel_rgb(pixel: bytes) -> tuple[int, int, int]:
        value = int.from_bytes(pixel, "little")
        return ((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff)

    def read_update(self, marker_x: int, marker_y: int,
                    deadline: float) -> tuple[int, tuple[int, int, int]] | None:
        self.sock.settimeout(max(0.1, deadline - time.monotonic()))
        message_type = self._recv_exact(1)[0]
        if message_type == 0:  # FramebufferUpdate
            self._recv_exact(1)
            rectangles = struct.unpack(">H", self._recv_exact(2))[0]
            found: tuple[int, tuple[int, int, int]] | None = None
            for _ in range(rectangles):
                x, y, width, height, encoding = struct.unpack(
                    ">HHHHi", self._recv_exact(12)
                )
                if encoding == 0:  # RAW
                    payload = self._recv_exact(width * height * 4)
                    if (found is None and x <= marker_x < x + width and
                            y <= marker_y < y + height):
                        offset = ((marker_y - y) * width + marker_x - x) * 4
                        found = (time.monotonic_ns(),
                                 self._pixel_rgb(payload[offset:offset + 4]))
                elif encoding == 1:  # CopyRect
                    self._recv_exact(4)
                elif encoding in (-224, -223, -239, -240, -232):
                    raise RuntimeError(
                        f"private RAW RFB server sent unsupported encoding {encoding}"
                    )
                else:
                    raise RuntimeError(f"private RFB server sent encoding {encoding}")
            return found
        if message_type == 2:  # Bell
            return None
        if message_type == 3:  # ServerCutText
            self._recv_exact(3)
            length = struct.unpack(">I", self._recv_exact(4))[0]
            self._recv_exact(length)
            return None
        raise RuntimeError(f"private RFB server sent message type {message_type}")

    def wait_for_marker(self, state: int, visible_ns: int, marker_x: int,
                        marker_y: int, timeout: float,
                        *, pending_update: bool = False) -> float | None:
        deadline = time.monotonic() + timeout
        pending = pending_update
        while time.monotonic() < deadline:
            if not pending:
                self.request(incremental=True)
            pending = False
            try:
                observed = self.read_update(marker_x, marker_y, deadline)
            except socket.timeout:
                return None
            if observed is not None:
                timestamp_ns, rgb = observed
                red, green, blue = rgb
                matches = ((red > 200 and green < 80 and blue < 80) if state
                           else (blue > 200 and red < 80 and green < 80))
                if matches:
                    return (timestamp_ns - visible_ns) / 1e6
        return None


def _interrupt_benchmark(signum, frame) -> None:
    """Turn termination into an exception so run_case() can clean up."""
    del signum, frame
    raise KeyboardInterrupt


signal.signal(signal.SIGINT, _interrupt_benchmark)
signal.signal(signal.SIGTERM, _interrupt_benchmark)


class LineReader:
    """Read complete lines from an unbuffered child pipe without data loss."""

    def __init__(self, stream) -> None:
        self._fd = stream.fileno()
        self._buffer = bytearray()

    def readline(self, timeout: float) -> bytes:
        import select

        deadline = time.monotonic() + timeout
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                line = bytes(self._buffer[:newline + 1])
                del self._buffer[:newline + 1]
                return line
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return b""
            ready, _, _ = select.select([self._fd], [], [], remaining)
            if not ready:
                return b""
            chunk = os.read(self._fd, 4096)
            if not chunk:
                return bytes(self._buffer)
            self._buffer.extend(chunk)


def process_cpu_seconds(proc: subprocess.Popen[bytes]) -> float:
    return process_cpu_seconds_pid(proc.pid)


def process_cpu_seconds_pid(pid: int) -> float:
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().split()
        ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
        return (int(fields[13]) + int(fields[14])) / ticks
    except (FileNotFoundError, IndexError, ValueError):
        return 0.0


def process_rss_bytes(proc: subprocess.Popen[bytes]) -> int:
    return process_rss_bytes_pid(proc.pid)


def process_rss_bytes_pid(pid: int) -> int:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    except (FileNotFoundError, IndexError, ValueError):
        pass
    return 0


def process_tree_pids(root_pid: int) -> set[int]:
    """Return a root process and its current descendants."""
    children: dict[int, list[int]] = {}
    for entry in Path("/proc").glob("[0-9]*"):
        try:
            fields = (entry / "stat").read_text().split()
            pid = int(fields[0])
            parent = int(fields[3])
        except (FileNotFoundError, IndexError, ValueError):
            continue
        children.setdefault(parent, []).append(pid)
    result = {root_pid}
    pending = [root_pid]
    while pending:
        parent = pending.pop()
        for child in children.get(parent, []):
            if child not in result:
                result.add(child)
                pending.append(child)
    return result


def process_cpu_tree(proc: subprocess.Popen[bytes]) -> float:
    return sum(process_cpu_seconds_pid(pid) for pid in process_tree_pids(proc.pid))


def process_rss_tree(proc: subprocess.Popen[bytes]) -> int:
    return sum(process_rss_bytes_pid(pid) for pid in process_tree_pids(proc.pid))


def kill_process(proc: subprocess.Popen[bytes] | None, *, privileged: bool = False) -> None:
    if proc is None or proc.poll() is not None:
        return
    try:
        process_group = os.getpgid(proc.pid)
    except ProcessLookupError:
        return

    def send(signal_number: int) -> None:
        try:
            os.killpg(process_group, signal_number)
        except PermissionError:
            if not privileged:
                raise
            # ``ip netns exec`` is launched through sudo, so its short-lived
            # supervisor may remain root-owned even after setpriv drops the
            # FreeRDP child back to the invoking user.  Use the already
            # authenticated sudo ticket only for that process-group signal.
            subprocess.run(
                ["sudo", "-n", "kill", f"-{signal_number}",
                 "--", f"-{process_group}"],
                check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
    try:
        send(signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            send(signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=3)


def start_process(command: list[str], env: dict[str, str], log_path: Path,
                  *, stdin=None, preserve_tty: bool = False) -> subprocess.Popen[bytes]:
    log = log_path.open("wb")
    try:
        options = {
            "stdin": stdin,
            "stdout": log,
            "stderr": subprocess.STDOUT,
            "env": env,
            "bufsize": 0,
            # The benchmark normally detaches every helper into its own
            # session so process-group cleanup is deterministic.  A namespace
            # FreeRDP client is launched through sudo -n, however.  Keeping
            # its controlling TTY while assigning a private process group
            # lets sudo use the timestamp established by `sudo -v` in the
            # caller's shell without allowing a prompt to block the run.
            "start_new_session": not preserve_tty,
        }
        if preserve_tty:
            options["preexec_fn"] = os.setpgrp
        return subprocess.Popen(command, **options)
    finally:
        log.close()


def wait_tcp_port(host: str, port: int, deadline: float) -> socket.socket:
    """Wait for a TCP listener on a specific address.

    The normal benchmark uses loopback.  Namespace mode binds xrdp to the
    host side of a private veth, so probing 127.0.0.1 would give a false
    negative even though xrdp is ready for the isolated FreeRDP client.
    """
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection((host, port), timeout=1)
            sock.settimeout(20)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"xrdp did not open {host}:{port}: {last_error}")


def wait_vnc_port(port: int, deadline: float) -> socket.socket:
    """Wait for the private x11vnc listener without touching production VNC."""
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            sock.settimeout(20)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"x11vnc did not open port {port}: {last_error}")


class SyntheticNetwork:
    """Optional private veth/netns transport for the FreeRDP client.

    The server and all X11/VNC helpers stay in the benchmark's host
    namespace.  Only FreeRDP enters the temporary client namespace.  Netem
    is installed on both veth ends, so ``--network-delay-ms`` is a one-way
    delay and the expected base RTT is twice that value.

    Namespace setup needs CAP_NET_ADMIN.  We intentionally use ``sudo -n``
    for each short-lived operation rather than running the benchmark as root,
    and fail with an actionable message when the caller has not authenticated
    with ``sudo -v`` first.  The FreeRDP command drops back to the invoking
    user's uid/gid before it starts, preserving Xvfb and file ownership.
    """

    def __init__(self, args: argparse.Namespace) -> None:
        self.mode = args.network_mode
        self.delay_ms = float(args.network_delay_ms)
        self.jitter_ms = float(args.network_jitter_ms)
        self.loss_percent = float(args.network_loss_percent)
        self.rate_mbps = (None if args.network_rate_mbps is None
                          else float(args.network_rate_mbps))
        self.namespace: str | None = None
        self.host_if: str | None = None
        self.client_if: str | None = None
        self.host_ip = "127.0.0.1"
        self.client_ip = "127.0.0.1"
        self.uid = os.getuid()
        self.gid = os.getgid()
        if self.enabled:
            token = f"{os.getpid() & 0xffff:04x}{secrets.token_hex(2)}"
            self.namespace = f"xrdpbench-{token}"
            # Linux interface names are limited to 15 bytes.
            self.host_if = f"xrbh{token}"
            self.client_if = f"xrbc{token}"
            octet = (os.getpid() % 250) or 1
            self.host_ip = f"10.254.{octet}.1"
            self.client_ip = f"10.254.{octet}.2"

    @property
    def enabled(self) -> bool:
        return self.mode == "namespace"

    @staticmethod
    def _format_ms(value: float) -> str:
        return f"{value:g}ms"

    @staticmethod
    def _format_rate(value: float) -> str:
        return f"{value:g}mbit"

    def _sudo(self, *command: str, check: bool = True) -> subprocess.CompletedProcess:
        result = subprocess.run(
            ["sudo", "-n", *command], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if check and result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
            raise RuntimeError(
                f"sudo command failed ({result.returncode}): "
                f"{' '.join(command)}: {detail}"
            )
        return result

    def _exec_ns(self, *command: str, check: bool = True) -> subprocess.CompletedProcess:
        if self.namespace is None:
            raise RuntimeError("network namespace is not initialized")
        return self._sudo("ip", "netns", "exec", self.namespace, *command,
                          check=check)

    def setup(self) -> None:
        if not self.enabled:
            return
        probe = subprocess.run(
            ["sudo", "-n", "true"], check=False,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        if probe.returncode != 0:
            raise RuntimeError(
                "network namespace mode needs cached sudo credentials; run "
                "sudo -v once, then rerun the benchmark"
            )

        try:
            self._sudo("ip", "netns", "add", self.namespace)
            self._sudo("ip", "link", "add", self.host_if, "type", "veth",
                       "peer", "name", self.client_if)
            self._sudo("ip", "link", "set", self.client_if, "netns",
                       self.namespace)
            self._sudo("ip", "addr", "add", f"{self.host_ip}/30",
                       "dev", self.host_if)
            self._sudo("ip", "link", "set", self.host_if, "up")
            self._exec_ns("ip", "addr", "add", f"{self.client_ip}/30",
                          "dev", self.client_if)
            self._exec_ns("ip", "link", "set", "lo", "up")
            self._exec_ns("ip", "link", "set", self.client_if, "up")
            self._install_netem(self.host_if, in_namespace=False)
            self._install_netem(self.client_if, in_namespace=True)
        except Exception:
            self.cleanup()
            raise

    def _install_netem(self, interface: str, *, in_namespace: bool) -> None:
        # With no impairment requested, leave the veth at its normal qdisc.
        # This keeps the namespace baseline as close as possible to the
        # existing localhost baseline while still exercising the real path.
        if (self.delay_ms == 0 and self.jitter_ms == 0 and
                self.loss_percent == 0 and self.rate_mbps is None):
            return
        command = ["tc", "qdisc", "replace", "dev", interface, "root",
                   "netem"]
        if self.delay_ms or self.jitter_ms:
            command += ["delay", self._format_ms(self.delay_ms)]
            if self.jitter_ms:
                command.append(self._format_ms(self.jitter_ms))
        if self.loss_percent:
            command += ["loss", "random", f"{self.loss_percent:g}%"]
        if self.rate_mbps is not None:
            command += ["rate", self._format_rate(self.rate_mbps)]
        if in_namespace:
            self._exec_ns(*command)
        else:
            self._sudo(*command)

    def wrap_client_command(self, command: list[str], env: dict[str, str]) -> list[str]:
        if not self.enabled:
            return command
        if self.namespace is None:
            raise RuntimeError("network namespace is not initialized")
        # ``setpriv`` runs after entering the netns and drops root before
        # FreeRDP starts.  Pass only the environment needed by X11/FreeRDP;
        # this avoids relying on sudo's distribution-specific env policy.
        env_keys = {
            "DISPLAY", "XAUTHORITY", "HOME", "PATH", "LANG", "LC_ALL",
            "LC_CTYPE", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME",
            "LD_LIBRARY_PATH", "PULSE_SERVER",
        }
        assignments = [f"{key}={env[key]}" for key in sorted(env_keys)
                       if key in env]
        return [
            "sudo", "-n", "ip", "netns", "exec", self.namespace,
            "/usr/bin/setpriv", f"--reuid={self.uid}", f"--regid={self.gid}",
            "--init-groups", "--", "/usr/bin/env", *assignments, *command,
        ]

    def header(self) -> str:
        if not self.enabled:
            return "network=localhost one_way_delay_ms=0 expected_rtt_ms=0"
        expected_rtt = 2.0 * self.delay_ms
        rate = "unlimited" if self.rate_mbps is None else f"{self.rate_mbps:g}"
        return (
            f"network=namespace host={self.host_ip} client={self.client_ip} "
            f"one_way_delay_ms={self.delay_ms:g} expected_rtt_ms={expected_rtt:g} "
            f"jitter_ms={self.jitter_ms:g} loss_percent={self.loss_percent:g} "
            f"rate_mbps={rate}"
        )

    def diagnostics(self) -> str:
        """Return the namespace addresses, qdiscs, and an ICMP RTT sample."""
        if not self.enabled or not self.namespace or not self.client_if or not self.host_if:
            raise RuntimeError("network diagnostics require namespace mode")
        host_qdisc = self._sudo("tc", "qdisc", "show", "dev", self.host_if).stdout.strip()
        client_qdisc = self._exec_ns("tc", "qdisc", "show", "dev",
                                     self.client_if).stdout.strip()
        ping = self._exec_ns("ping", "-n", "-c", "3", "-W", "1", self.host_ip).stdout.strip()
        return (
            f"host_qdisc={host_qdisc or 'none'}\n"
            f"client_qdisc={client_qdisc or 'none'}\n"
            f"ping_from_client_to_host:\n{ping}"
        )

    def cleanup(self) -> None:
        if not self.enabled:
            return
        # Deleting the namespace removes the peer interface.  Delete the host
        # side explicitly too, covering failures partway through setup.
        if self.namespace:
            self._sudo("ip", "netns", "delete", self.namespace, check=False)
        if self.host_if:
            self._sudo("ip", "link", "delete", self.host_if, check=False)
        self.namespace = None


def discover_auth(explicit: str | None) -> str:
    if explicit:
        path = Path(explicit)
        if path.is_file() and os.access(path, os.R_OK):
            return str(path)
        raise RuntimeError(f"Xauthority file is not readable: {path}")

    # SDDM keeps the live Xorg cookie in /run/sddm, whose directory is often
    # mode 0711 and therefore not listable by the desktop user.  The same
    # cookie is exposed by the console-sharing service in the user's runtime
    # directory, and interactive shells commonly export a temporary copy as
    # XAUTHORITY.  Prefer those direct paths before attempting the SDDM glob.
    candidates: list[Path] = []
    environment_auth = os.environ.get("XAUTHORITY")
    if environment_auth:
        candidates.append(Path(environment_auth))
    candidates.append(Path(f"/run/user/{os.getuid()}/xrdp-console.xauth"))
    try:
        candidates.extend(sorted(Path("/run/sddm").glob("xauth_*")))
    except PermissionError:
        pass
    seen: set[Path] = set()
    for candidate in candidates:
        if candidate in seen:
            continue
        seen.add(candidate)
        if candidate.is_file() and os.access(candidate, os.R_OK):
            return str(candidate)
    raise RuntimeError(
        "cannot find a readable Xauthority file (checked $XAUTHORITY, "
        "/run/user/<uid>/xrdp-console.xauth, and /run/sddm/xauth_*)"
    )


def rewrite_xrdp_config(source: Path, target: Path, port: int,
                        vnc_port: int, log_path: Path,
                        chansrv_path: Path, cert: Path, key: Path,
                        max_bpp: int, disable_gfx_for_vnc: bool,
                        enable_gfx_for_vnc: bool,
                        console_lib: str,
                        bitmap_compression: bool | None = None,
                        bulk_compression: bool | None = None,
                        disable_dynamic_resizing: bool = False,
                        bind_host: str = "127.0.0.1") -> None:
    """Copy the installed xrdp profile into a private test configuration."""
    if disable_gfx_for_vnc and enable_gfx_for_vnc:
        raise RuntimeError("GFX enable and disable options are mutually exclusive")
    lines = source.read_text().splitlines()
    section = ""
    output: list[str] = []
    seen: set[tuple[str, str]] = set()
    replacements = {
        ("Globals", "port"): f"port=tcp://{bind_host}:{port}",
        ("Globals", "fork"): "fork=true",
        ("Globals", "certificate"): f"certificate={cert}",
        ("Globals", "key_file"): f"key_file={key}",
        ("Globals", "autorun"): "autorun=Console",
        ("Globals", "allow_channels"): "allow_channels=true",
        ("Globals", "max_bpp"): f"max_bpp={max_bpp}",
        ("Logging", "LogFile"): f"LogFile={log_path}",
        ("Logging", "EnableSyslog"): "EnableSyslog=false",
        # GFX capability negotiation occurs before the selected session
        # profile is fully active, so the global channel must be disabled in
        # the GFX-off case as well as the Console override below.
        ("Channels", "drdynvc"): (
            "drdynvc=false" if disable_gfx_for_vnc else "drdynvc=true"
        ),
        ("Console", "lib"): f"lib={console_lib}",
        ("Console", "port"): f"port={vnc_port}",
        ("Console", "password"): "password=na",
        ("Console", "chansrvport"): f"chansrvport={chansrv_path}",
        ("Console", "channel.cliprdr"): "channel.cliprdr=true",
        # xrdp carries GFX over drdynvc.  disable_gfx=true is not understood
        # by the stock/custom 0.10.6.1 binary, so use the channel override for
        # a real GFX-disabled comparison.
        ("Console", "channel.drdynvc"): (
            "channel.drdynvc=false" if disable_gfx_for_vnc
            else "channel.drdynvc=true"
        ),
    }
    if bitmap_compression is not None:
        replacements[("Globals", "bitmap_compression")] = (
            f"bitmap_compression={'true' if bitmap_compression else 'false'}")
    if bulk_compression is not None:
        replacements[("Globals", "bulk_compression")] = (
            f"bulk_compression={'true' if bulk_compression else 'false'}")
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
        if "=" in line and not stripped.startswith(";") and not stripped.startswith("#"):
            key_name = line.split("=", 1)[0].strip()
            replacement = replacements.get((section, key_name))
            if replacement is not None:
                output.append(replacement)
                seen.add((section, key_name))
                continue
        output.append(line)
    for item, replacement in replacements.items():
        if item not in seen:
            raise RuntimeError(f"installed xrdp.ini lacks {item[0]} {item[1]}")
    console_start = next(
        (index for index, line in enumerate(output)
         if line.strip() == "[Console]"), None,
    )
    if console_start is None:
        raise RuntimeError("installed xrdp.ini lacks [Console] section")
    console_end = next(
        (index for index in range(console_start + 1, len(output))
         if output[index].strip().startswith("[") and
         output[index].strip().endswith("]")), len(output),
    )
    if enable_gfx_for_vnc or disable_gfx_for_vnc:
        output = [
            line for index, line in enumerate(output)
            if not (console_start < index < console_end and
                    "=" in line and
                    line.split("=", 1)[0].strip().lower() == "disable_gfx" and
                    not line.lstrip().startswith((";", "#")))
        ]
        console_end = next(
            (index for index in range(console_start + 1, len(output))
            if output[index].strip().startswith("[") and
             output[index].strip().endswith("]")), len(output),
        )
    if disable_dynamic_resizing:
        console_start = next(
            (index for index, line in enumerate(output)
             if line.strip() == "[Console]"), None,
        )
        if console_start is None:
            raise RuntimeError("installed xrdp.ini lacks [Console] section")
        console_end = next(
            (index for index in range(console_start + 1, len(output))
             if output[index].strip().startswith("[") and
             output[index].strip().endswith("]")), len(output),
        )
        has_dynamic_resize = any(
            line.split("=", 1)[0].strip().lower() == "enable_dynamic_resizing"
            for line in output[console_start + 1:console_end]
            if "=" in line and not line.lstrip().startswith((";", "#"))
        )
        if not has_dynamic_resize:
            output.insert(console_end, "enable_dynamic_resizing=false")
    target.write_text("\n".join(output) + "\n")
    target.chmod(0o600)


def remove_chansrv_from_config(path: Path) -> None:
    """Disable chansrv in a private benchmark config.

    The pixel-path benchmark does not exercise clipboard or drive channels.
    Removing the explicit Console socket avoids attaching a synthetic test
    client to the production chansrv daemon when the standalone helper is not
    available.
    """
    lines = path.read_text().splitlines()
    section = ""
    kept: list[str] = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
        if section == "Console" and "=" in line:
            key = line.split("=", 1)[0].strip().lower()
            if key in {"chansrvport", "channel.cliprdr", "channel.drdynvc"}:
                continue
        kept.append(line)
    path.write_text("\n".join(kept) + "\n")


def find_window(display: str, title: str, deadline: float) -> str:
    pattern = re.compile(
        r'^\s*(0x[0-9a-fA-F]+) "' + re.escape(title) + r'"', re.MULTILINE
    )
    last_tree = ""
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["xwininfo", "-root", "-tree", "-display", display],
            capture_output=True, text=True, check=False,
        )
        last_tree = result.stdout
        match = pattern.search(last_tree)
        if match:
            return match.group(1)
        time.sleep(0.1)
    raise RuntimeError(f"FreeRDP window {title!r} did not appear:\n{last_tree}")


def find_vnc_viewer_window(display: str, port: int, deadline: float) -> str:
    """Find the TigerVNC content window without assuming its title wording."""
    pattern = re.compile(r'^\s*(0x[0-9a-fA-F]+) "([^"]*)"')
    last_tree = ""
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["xwininfo", "-root", "-tree", "-display", display],
            capture_output=True, text=True, check=False,
        )
        last_tree = result.stdout
        for line in last_tree.splitlines():
            match = pattern.match(line)
            if match is None:
                continue
            title = match.group(2)
            if (str(port) in title or "TigerVNC" in title or
                    "VNC Viewer" in title):
                return match.group(1)
        time.sleep(0.1)
    raise RuntimeError(
        f"VNC viewer window for port {port} did not appear:\n{last_tree}"
    )


def wait_log_marker(path: Path, marker: str, deadline: float) -> None:
    """Wait for a short-lived helper to announce readiness in its log."""
    while time.monotonic() < deadline:
        if path.is_file() and marker in path.read_text(errors="replace"):
            return
        time.sleep(0.05)
    contents = path.read_text(errors="replace") if path.is_file() else ""
    raise RuntimeError(f"helper did not become ready; log={contents!r}")


def wait_path(path: Path, deadline: float) -> None:
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise RuntimeError(f"helper did not create {path}")


def remove_stale_unix_socket(path: Path) -> None:
    """Remove only an unowned stale chansrv socket.

    A benchmark must never unlink a live production chansrv endpoint owned by
    the same user.  An active Unix listener accepts the probe connection; a
    stale pathname normally returns ECONNREFUSED and can be removed safely.
    """
    if not path.exists():
        return
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        try:
            probe.connect(str(path))
        except ConnectionRefusedError:
            path.unlink(missing_ok=True)
        except FileNotFoundError:
            return
        except OSError as exc:
            if exc.errno in (errno.ENXIO, errno.ENOTCONN):
                path.unlink(missing_ok=True)
            else:
                raise RuntimeError(
                    f"refusing to remove non-stale Unix socket {path}: {exc}"
                ) from exc
        else:
            raise RuntimeError(
                f"refusing to disturb active Unix socket {path}"
            )
    finally:
        probe.close()


def assert_tcp_port_available(host: str, port: int, *, check_ipv6: bool = False) -> None:
    """Fail before a child starts if a private benchmark port is occupied."""
    addresses: list[tuple[int, tuple[str, int]]] = [
        (socket.AF_INET, (host, port)),
    ]
    if check_ipv6 and host == "127.0.0.1":
        addresses.append((socket.AF_INET6, ("::1", port)))
    for family, address in addresses:
        sock = socket.socket(family, socket.SOCK_STREAM)
        try:
            sock.bind(address)
        except OSError as exc:
            raise RuntimeError(
                f"private benchmark port {address[0]}:{port} is already in use"
            ) from exc
        finally:
            sock.close()


def sample_remote_pixels(display: str, window: str, points: list[tuple[int, int]],
                         env: dict[str, str]) -> list[str]:
    """Collect a few diagnostic pixels if the marker cannot be observed."""
    results: list[str] = []
    for index, (point_x, point_y) in enumerate(points):
        sample = subprocess.Popen(
            [str(PIXEL_PROBE), display, window, str(point_x), str(point_y)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, bufsize=0, start_new_session=True,
        )
        try:
            if sample.stdin is None or sample.stdout is None:
                raise RuntimeError("pixel probe pipes were not created")
            reader = LineReader(sample.stdout)
            ready = reader.readline(1)
            sample.stdin.write(b"sample\n")
            sample.stdin.flush()
            line = reader.readline(1)
            results.append(f"({point_x},{point_y}) {line.decode(errors='replace').strip()!r}")
            sample.stdin.close()
            sample.wait(timeout=1)
            del ready
        finally:
            kill_process(sample)
    return results


def marker_rgb_matches(parts: list[bytes], state: int) -> bool:
    if len(parts) < 4:
        return False
    try:
        red, green, blue = (int(value) for value in parts[1:4])
    except ValueError:
        return False
    if state:
        return red > 200 and green < 80 and blue < 80
    return blue > 200 and red < 80 and green < 80


def wait_marker(probe: LineReader, probe_stdin, state: int,
                visible_ns: int, timeout: float,
                poll_interval: float, *, diagnostic: bool = False,
                observed_ns_out: list[int] | None = None) -> float | None:
    deadline = time.monotonic() + timeout
    latest = b""
    while time.monotonic() < deadline:
        probe_stdin.write(b"sample\n")
        probe_stdin.flush()
        line = probe.readline(min(0.5, max(0.02, deadline - time.monotonic())))
        if line:
            latest = line
            fields = line.split()
            if marker_rgb_matches(fields, state):
                try:
                    observed_ns = int(fields[0])
                    if observed_ns_out is not None:
                        observed_ns_out.append(observed_ns)
                    return (observed_ns - visible_ns) / 1e6
                except (IndexError, ValueError):
                    pass
        time.sleep(poll_interval)
    if diagnostic:
        print(f"marker timeout state={state} last={latest.decode(errors='replace').strip()!r}")
    return None


def parse_physical_marker(line: bytes) -> tuple[int, int, int]:
    """Decode event, draw-complete, and marker state from the X11 helper.

    Older helper binaries emitted ``event state``.  Treat that format as if
    the draw completed at the event timestamp so a source-tree Python script
    remains usable with an older helper during an incremental rebuild.
    """
    fields = line.split()
    if len(fields) >= 3:
        return int(fields[0]), int(fields[1]), int(fields[2])
    if len(fields) >= 2:
        event_ns = int(fields[0])
        return event_ns, event_ns, int(fields[1])
    raise RuntimeError(f"invalid physical marker line: {line!r}")


def print_transport_summary(label: str, start: TcpSnapshot | None,
                            end: TcpSnapshot | None, elapsed: float) -> None:
    wire_bytes = None
    if start is not None and end is not None:
        start_bytes = start.wire_bytes
        end_bytes = end.wire_bytes
        if start_bytes is not None and end_bytes is not None:
            wire_bytes = max(0, end_bytes - start_bytes)
    rate = (wire_bytes / elapsed / 1024.0) if wire_bytes is not None else None
    retrans = None
    if end is not None and end.retransmissions is not None:
        retrans = end.retransmissions
        if start is not None and start.retransmissions is not None:
            retrans = max(0, retrans - start.retransmissions)
    send_queue = None if end is None else end.send_queue
    rtt_ms = None if end is None else end.rtt_ms
    print(
        f"{'':10} transport={label} "
        f"wire_bytes={('NA' if wire_bytes is None else wire_bytes)} "
        f"wire_kib_s={('NA' if rate is None else f'{rate:.1f}')} "
        f"retrans={('NA' if retrans is None else retrans)} "
        f"sendq={('NA' if send_queue is None else send_queue)} "
        f"rtt_ms={('NA' if rtt_ms is None else f'{rtt_ms:.3f}')}"
    )


def summarize(name: str, latencies: list[float], misses: int,
              samples: int, render_ns: list[int], processes: list,
              cpu_start: dict[int, float], wall_start: float,
              rss_start: dict[int, int], *,
              transport_label: str | None = None,
              transport_start: TcpSnapshot | None = None,
              transport_end: TcpSnapshot | None = None) -> None:
    ordered = sorted(latencies)
    if ordered:
        p50 = statistics.median(ordered)
        p95 = percentile(ordered, 0.95)
        p99 = percentile(ordered, 0.99)
        first_count = max(1, len(latencies) // 3)
        early = statistics.median(sorted(latencies[:first_count]))
        late = statistics.median(sorted(latencies[-first_count:]))
        drift = late / early if early else 0.0
        print(
            f"{name:10} e2e p50={p50:7.1f}ms p95={p95:7.1f}ms "
            f"p99={p99:7.1f}ms max={ordered[-1]:7.1f}ms "
            f"samples={len(ordered):4} "
            f"misses={misses:3} early/late={early:5.1f}/{late:5.1f} "
            f"drift={drift:4.2f}x"
        )
    else:
        print(f"{name:10} e2e no visible marker samples={samples} misses={misses}")
    if render_ns:
        render_ms = [value / 1e6 for value in render_ns]
        print(
            f"{'':10} GL render median={statistics.median(render_ms):5.2f}ms "
            f"p95={percentile(render_ms, .95):5.2f}ms"
        )
    elapsed = max(0.001, time.monotonic() - wall_start)
    metrics = []
    for proc in processes:
        before = cpu_start.get(proc.pid, 0.0)
        cpu = max(0.0, process_cpu_tree(proc) - before) / elapsed * 100.0
        start_rss = rss_start.get(proc.pid, 0)
        metrics.append(f"pid{proc.pid} cpu={cpu:.1f}% rss={start_rss / 1048576:.1f}->{process_rss_tree(proc) / 1048576:.1f}MiB")
    print(f"{'':10} " + "; ".join(metrics))
    if transport_label is not None:
        print_transport_summary(transport_label, transport_start,
                                transport_end, elapsed)



def percentile(values: list[float], fraction: float) -> float:
    """Return the nearest-rank percentile used by the published reports."""
    if not values:
        raise ValueError("percentile requires at least one value")
    ordered = sorted(values)
    rank = max(1, math.ceil(len(ordered) * fraction))
    return ordered[min(len(ordered) - 1, rank - 1)]


def summarize_input_latencies(label: str, values: list[float]) -> None:
    if not values:
        print(f"{label:18} no samples")
        return
    print(
        f"{label:18} p50={statistics.median(values):7.1f}ms "
        f"p95={percentile(values, 0.95):7.1f}ms "
        f"p99={percentile(values, 0.99):7.1f}ms "
        f"max={max(values):7.1f}ms"
    )


VNC_SCHED_FIELDS = (
    ("wait_us", "request_wait_us"),
    ("process_us", "process_us"),
    ("flush_us", "flush_us"),
    ("next_request_gap_us", "next_request_gap_us"),
    ("next_request_lead_us", "next_request_lead_us"),
    ("logical_update_us", "logical_update_us"),
)
VNC_SCHED_RE = re.compile(r"\bVNC_SCHED\s+(?P<fields>.*)$")
VNC_POINT_RE = re.compile(r"\bVNC_POINT\s+(?P<fields>.*)$")


def _parse_profile_fields(line: str, pattern: re.Pattern[str]) -> dict[str, int] | None:
    match = pattern.search(line)
    if match is None:
        return None
    fields: dict[str, int] = {}
    for key, value in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+)",
                                 match.group("fields")):
        fields[key] = int(value)
    return fields


def parse_vnc_sched_lines(text: str) -> list[dict[str, int]]:
    records = []
    for line in text.splitlines():
        fields = _parse_profile_fields(line, VNC_SCHED_RE)
        if fields:
            records.append(fields)
    return records


def parse_vnc_point_lines(text: str) -> list[dict[str, int]]:
    records = []
    for line in text.splitlines():
        fields = _parse_profile_fields(line, VNC_POINT_RE)
        if fields:
            records.append(fields)
    return records


def correlate_vnc_points(
        points: list[dict[str, int]], draw_ns: list[int],
        visible_ns: list[int], expected_states: list[int | None] | None = None,
        ) -> list[tuple[int, int, dict[str, int]]]:
    """Match marker-state-specific VNC paints to benchmark samples.

    Point records are ordered by their monotonic paint timestamp.  A record
    is eligible only when it is a successful, classified marker paint between
    the corresponding physical draw and client-visible timestamps.  A wrong
    color state is skipped, which prevents unrelated same-coordinate churn
    from being paired with the sample.
    """
    if len(draw_ns) != len(visible_ns):
        raise ValueError("draw and visible timestamp counts differ")
    if expected_states is None:
        expected_states = [None] * len(draw_ns)
    if len(expected_states) != len(draw_ns):
        raise ValueError("expected marker-state count differs from timestamps")

    ordered_points = sorted(
        points, key=lambda point: point.get("paint_ns", 0))
    pairs: list[tuple[int, int, dict[str, int]]] = []
    point_index = 0
    for draw, visible, expected_state in zip(
            draw_ns, visible_ns, expected_states):
        while point_index < len(ordered_points):
            point = ordered_points[point_index]
            paint = point.get("paint_ns")
            if paint is None or paint < draw:
                point_index += 1
                continue
            if paint > visible:
                break
            point_index += 1
            if point.get("result", 0) != 0:
                continue
            send = point.get("send_end_ns")
            if send is None or send <= 0:
                continue
            marker_state = point.get("marker_state")
            if expected_state is not None and marker_state != expected_state:
                continue
            pairs.append((draw, visible, point))
            break
    return pairs


def _format_profile_percentiles(values: list[float]) -> str:
    return (
        f"p50={statistics.median(values):7.1f}us "
        f"p95={percentile(values, .95):7.1f}us "
        f"p99={percentile(values, .99):7.1f}us"
    )


def _summarize_point_stage(label: str, values: list[float]) -> None:
    if values:
        print(f"{'':18} {label:26} {_format_profile_percentiles(values)} "
              f"n={len(values)}")


def summarize_xrdp_profile(log_path: Path, workload: str,
                           draw_ns: list[int] | None = None,
                           visible_ns: list[int] | None = None,
                           expected_states: list[int | None] | None = None,
                           ) -> None:
    """Print scheduler and optional marker-point percentiles from one run."""
    if not log_path.is_file():
        return
    text = log_path.read_text(errors="replace")
    sched = parse_vnc_sched_lines(text)
    points = parse_vnc_point_lines(text)
    if sched:
        print(f"{'':18} VNC_SCHED workload={workload} updates={len(sched)}")
        for field, label in VNC_SCHED_FIELDS:
            values = [float(record[field]) for record in sched if field in record]
            if values:
                print(f"{'':18} {label:26} {_format_profile_percentiles(values)}")

    if not points or not draw_ns or not visible_ns:
        return

    pairs = correlate_vnc_points(
        points, draw_ns, visible_ns, expected_states)

    print(f"{'':18} VNC_POINT workload={workload} records={len(points)} "
          f"matched={len(pairs)} unmatched={len(draw_ns) - len(pairs)}")
    stages: dict[str, list[float]] = {
        "draw -> VNC paint": [],
        "VNC paint -> flush begin": [],
        "flush begin -> RDP send": [],
        "VNC paint -> RDP send": [],
        "RDP send -> client visible": [],
        "draw -> client visible": [],
    }
    for draw, visible, point in pairs:
        paint = point["paint_ns"]
        flush_begin = point.get("flush_begin_ns", paint)
        send = point["send_end_ns"]
        stages["draw -> VNC paint"].append((paint - draw) / 1000.0)
        stages["VNC paint -> flush begin"].append((flush_begin - paint) / 1000.0)
        stages["flush begin -> RDP send"].append((send - flush_begin) / 1000.0)
        stages["VNC paint -> RDP send"].append((send - paint) / 1000.0)
        stages["RDP send -> client visible"].append((visible - send) / 1000.0)
        stages["draw -> client visible"].append((visible - draw) / 1000.0)
    for label, values in stages.items():
        _summarize_point_stage(label, values)


def run_direct_rfb_case(args: argparse.Namespace, name: str, profile: str,
                        auth: str, runtime: Path, base_port: int,
                        repetition: int) -> None:
    """Measure the physical X -> private x11vnc RAW-RFB wire path."""
    x, y = 20, 20
    vnc_port = base_port + 2575
    case_dir = runtime / f"{name}-{repetition}"
    case_dir.mkdir()
    env_source = os.environ.copy()
    env_source.update({"DISPLAY": args.display, "XAUTHORITY": auth})
    vnc_command = [
        str(X11VNC), "-display", args.display, "-auth", auth, "-localhost",
        "-listen", "127.0.0.1", "-no6", "-rfbport", str(vnc_port),
        "-nopw", "-forever", "-shared", "-xdamage", "-xd_mem", "0",
        "-threads", "-repeat", "-input_skip", "1", "-wait", "5",
        "-defer", "5", "-deferupdate", "5", "-wait_ui", "2",
        "-setdefer", "-1", "-scrollcopyrect", args.scrollcopyrect, "-quiet",
    ]
    profile_flags = list(VNC_PROFILES[profile])
    if "-noxdamage" in profile_flags:
        vnc_command[vnc_command.index("-xdamage")] = "-noxdamage"
        profile_flags.remove("-noxdamage")
    vnc_command += profile_flags
    vnc: subprocess.Popen[bytes] | None = None
    stimulus: subprocess.Popen[bytes] | None = None
    client: RfbClient | None = None
    try:
        assert_tcp_port_available("127.0.0.1", vnc_port, check_ipv6=True)
        print(f"{name}#{repetition}: transport=direct-rfb private-loopback")
        vnc = start_process(vnc_command, env_source, case_dir / "x11vnc.log")
        probe = wait_vnc_port(vnc_port, time.monotonic() + 8)
        probe.close()
        client = RfbClient(wait_vnc_port(vnc_port, time.monotonic() + 8))
        client.connect()
        if client.width < x + args.width or client.height < y + args.height:
            raise RuntimeError(
                f"RFB framebuffer {client.width}x{client.height} is smaller "
                f"than the {args.width}x{args.height} stimulus"
            )
        marker_x = x + args.width // 2
        marker_y = y + args.height // 2
        stimulus = subprocess.Popen(
            [str(GPU_STIMULUS), args.display, str(args.width), str(args.height)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env_source, bufsize=0, start_new_session=True,
        )
        if stimulus.stdin is None or stimulus.stdout is None:
            raise RuntimeError("GPU stimulus pipes were not created")
        stimulus_reader = LineReader(stimulus.stdout)
        first = stimulus_reader.readline(5)
        if first.startswith(b"SWAP_CONTROL "):
            print(f"{name}: {first.decode(errors='replace').strip()}")
            first = stimulus_reader.readline(5)
        if not first.startswith(b"READY "):
            raise RuntimeError(f"stimulus did not become ready: {first!r}")
        fields = first.split()
        if len(fields) < 2 or fields[1] != f"{args.width}x{args.height}".encode():
            raise RuntimeError(f"stimulus dimensions differ from request: {first!r}")

        # The initial non-incremental request sent by connect() is consumed by
        # the warm-up. Subsequent requests are incremental and RAW-only.
        stimulus.stdin.write(b"frame\n")
        stimulus.stdin.flush()
        warmup = stimulus_reader.readline(5)
        if not warmup:
            raise RuntimeError("stimulus warm-up failed")
        warmup_visible_ns, warmup_state, _ = parse_physical_marker(warmup)
        warmup_latency = client.wait_for_marker(
            warmup_state, warmup_visible_ns, marker_x, marker_y, args.timeout,
            pending_update=True,
        )
        if warmup_latency is None:
            raise RuntimeError("direct RFB marker did not arrive during warm-up")

        samples = max(1, int(args.duration * args.fps))
        latencies: list[float] = []
        render_ns: list[int] = []
        misses = 0
        process_list = [vnc]
        cpu_start = {proc.pid: process_cpu_tree(proc) for proc in process_list}
        rss_start = {proc.pid: process_rss_tree(proc) for proc in process_list}
        transport_start = tcp_snapshot(vnc_port)
        if transport_start is None:
            transport_start = TcpSnapshot(client.bytes_sent, client.bytes_received)
        wall_start = time.monotonic()
        next_tick = wall_start
        for _ in range(samples):
            next_tick += 1.0 / args.fps
            stimulus.stdin.write(b"frame\n")
            stimulus.stdin.flush()
            line = stimulus_reader.readline(5)
            try:
                visible_ns, state, render_duration_ns = parse_physical_marker(line)
            except (RuntimeError, ValueError):
                misses += 1
            else:
                render_ns.append(render_duration_ns)
                latency = client.wait_for_marker(
                    state, visible_ns, marker_x, marker_y, args.timeout,
                )
                if latency is None:
                    misses += 1
                else:
                    latencies.append(latency)
            delay = next_tick - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        transport_end = tcp_snapshot(vnc_port)
        if transport_end is None:
            transport_end = TcpSnapshot(client.bytes_sent, client.bytes_received)
        summarize(
            f"{name}#{repetition}", latencies, misses, samples, render_ns,
            process_list, cpu_start, wall_start, rss_start,
            transport_label="rfb-direct-wire-visible",
            transport_start=transport_start, transport_end=transport_end,
        )
    finally:
        if client is not None:
            client.sock.close()
        for proc in (stimulus, vnc):
            kill_process(proc)


def run_direct_rfb_input_case(args: argparse.Namespace, name: str,
                              profile: str, auth: str, runtime: Path,
                              base_port: int, repetition: int) -> None:
    """Measure client RFB KeyEvent -> physical X11 -> RAW-RFB pixels."""
    vnc_port = base_port + 2575
    case_dir = runtime / f"{name}-{repetition}"
    case_dir.mkdir()
    env_source = os.environ.copy()
    env_source.update({"DISPLAY": args.display, "XAUTHORITY": auth})
    vnc_command = [
        str(X11VNC), "-display", args.display, "-auth", auth, "-localhost",
        "-listen", "127.0.0.1", "-no6", "-rfbport", str(vnc_port),
        "-nopw", "-forever", "-shared", "-xdamage", "-xd_mem", "0",
        "-threads", "-repeat", "-input_skip", "1", "-wait", "5",
        "-defer", "5", "-deferupdate", "5", "-wait_ui", "2",
        "-setdefer", "-1", "-scrollcopyrect", args.scrollcopyrect, "-quiet",
    ]
    profile_flags = list(VNC_PROFILES[profile])
    if "-noxdamage" in profile_flags:
        vnc_command[vnc_command.index("-xdamage")] = "-noxdamage"
        profile_flags.remove("-noxdamage")
    vnc_command += profile_flags
    vnc: subprocess.Popen[bytes] | None = None
    key_stimulus: subprocess.Popen[bytes] | None = None
    churn: subprocess.Popen[bytes] | None = None
    churn_thread: threading.Thread | None = None
    churn_stop = threading.Event()
    client: RfbClient | None = None
    try:
        assert_tcp_port_available("127.0.0.1", vnc_port, check_ipv6=True)
        print(f"{name}#{repetition}: transport=direct-rfb input-roundtrip")
        vnc = start_process(vnc_command, env_source, case_dir / "x11vnc.log")
        probe = wait_vnc_port(vnc_port, time.monotonic() + 8)
        probe.close()
        client = RfbClient(wait_vnc_port(vnc_port, time.monotonic() + 8))
        client.connect()

        input_x = args.input_x
        input_y = args.input_y
        marker_x = input_x + 80
        marker_y = input_y + 50
        if client.width <= marker_x or client.height <= marker_y:
            raise RuntimeError(
                f"RFB framebuffer {client.width}x{client.height} does not "
                f"contain the key marker at {marker_x}x{marker_y}"
            )

        churn_fps = args.input_churn_fps
        if churn_fps is None:
            churn_fps = args.fps
        if churn_fps > 0:
            churn = subprocess.Popen(
                [str(GPU_STIMULUS), args.display, str(args.width), str(args.height)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=env_source, bufsize=0,
                start_new_session=True,
            )
            if churn.stdin is None or churn.stdout is None:
                raise RuntimeError("churn stimulus pipes were not created")
            churn_reader = LineReader(churn.stdout)
            first = churn_reader.readline(5)
            if first.startswith(b"SWAP_CONTROL "):
                print(f"{name}: {first.decode(errors='replace').strip()}")
                first = churn_reader.readline(5)
            if not first.startswith(b"READY "):
                raise RuntimeError(f"churn stimulus did not become ready: {first!r}")

            def churn_frames() -> None:
                next_tick = time.monotonic()
                while not churn_stop.is_set():
                    next_tick += 1.0 / churn_fps
                    try:
                        churn.stdin.write(b"frame\n")
                        churn.stdin.flush()
                        if not churn_reader.readline(2):
                            break
                    except (BrokenPipeError, OSError):
                        break
                    churn_stop.wait(max(0.0, next_tick - time.monotonic()))

            churn_thread = threading.Thread(
                target=churn_frames, name="xrdp-bench-churn", daemon=True)
            churn_thread.start()

        key_stimulus = subprocess.Popen(
            [str(KEY_STIMULUS), args.display, "--key",
             str(input_x), str(input_y)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env_source,
            bufsize=0, start_new_session=True,
        )
        if key_stimulus.stdout is None:
            raise RuntimeError("key stimulus output pipe was not created")
        key_reader = LineReader(key_stimulus.stdout)
        if not key_reader.readline(5).startswith(b"READY "):
            raise RuntimeError("physical key stimulus did not become ready")

        # The initial non-incremental request sent by connect() is consumed by
        # this first key pulse. Subsequent requests are incremental RAW-only.
        t0_ns = client.send_key_pulse()
        physical_line = key_reader.readline(args.timeout)
        if not physical_line:
            raise RuntimeError("direct RFB input warm-up did not reach X11")
        t1_event_ns, t1_draw_done_ns, physical_state = \
            parse_physical_marker(physical_line)
        warmup_latency = client.wait_for_marker(
            1 - physical_state, t1_draw_done_ns, marker_x, marker_y,
            args.timeout, pending_update=True,
        )
        if warmup_latency is None:
            raise RuntimeError("direct RFB input warm-up marker did not return")
        del t0_ns, t1_event_ns

        samples = max(1, int(args.duration * args.input_hz))
        input_delivery_ms: list[float] = []
        local_draw_ms: list[float] = []
        return_graphics_ms: list[float] = []
        roundtrip_ms: list[float] = []
        delivery_misses = 0
        return_misses = 0
        measured_processes = [vnc, key_stimulus]
        if churn is not None:
            measured_processes.append(churn)
        cpu_start = {proc.pid: process_cpu_tree(proc)
                     for proc in measured_processes}
        rss_start = {proc.pid: process_rss_tree(proc)
                     for proc in measured_processes}
        transport_start = tcp_snapshot(vnc_port)
        wall_start = time.monotonic()
        next_tick = wall_start

        for _ in range(samples):
            next_tick += 1.0 / args.input_hz
            t0_ns = client.send_key_pulse()
            physical_line = key_reader.readline(args.timeout)
            if not physical_line:
                delivery_misses += 1
            else:
                t1_event_ns, t1_draw_done_ns, physical_state = \
                    parse_physical_marker(physical_line)
                input_ms = (t1_event_ns - t0_ns) / 1e6
                local_ms = (t1_draw_done_ns - t1_event_ns) / 1e6
                input_delivery_ms.append(input_ms)
                local_draw_ms.append(local_ms)
                return_ms = client.wait_for_marker(
                    1 - physical_state, t1_draw_done_ns, marker_x, marker_y,
                    args.timeout,
                )
                if return_ms is None:
                    return_misses += 1
                else:
                    return_graphics_ms.append(return_ms)
                    roundtrip_ms.append(input_ms + local_ms + return_ms)
            delay = next_tick - time.monotonic()
            if delay > 0:
                time.sleep(delay)

        print(f"{name}#{repetition}: input-roundtrip churn={churn_fps:.1f}fps "
              f"input={args.input_hz:.1f}Hz samples={samples} "
              f"delivery_misses={delivery_misses} return_misses={return_misses}")
        summarize_input_latencies("input T0->T1_event", input_delivery_ms)
        summarize_input_latencies("local T1_event->draw", local_draw_ms)
        summarize_input_latencies("graphics draw->T2", return_graphics_ms)
        summarize_input_latencies("roundtrip T0->T2", roundtrip_ms)
        elapsed = max(0.001, time.monotonic() - wall_start)
        metrics = []
        for proc in measured_processes:
            before = cpu_start.get(proc.pid, 0.0)
            cpu = max(0.0, process_cpu_tree(proc) - before) / elapsed * 100.0
            metrics.append(
                f"pid{proc.pid} cpu={cpu:.1f}% "
                f"rss={rss_start.get(proc.pid, 0) / 1048576:.1f}->"
                f"{process_rss_tree(proc) / 1048576:.1f}MiB")
        print(f"{'':18} " + "; ".join(metrics))
        transport_end = tcp_snapshot(vnc_port)
        print_transport_summary("rfb-direct-input-wire", transport_start,
                                transport_end, elapsed)
    finally:
        churn_stop.set()
        if churn_thread is not None:
            churn_thread.join(timeout=2)
        if client is not None:
            client.sock.close()
        for proc in (key_stimulus, churn, vnc):
            kill_process(proc)


def run_input_roundtrip(args: argparse.Namespace, name: str, repetition: int,
                        window: str, client_display: str,
                        env_source: dict[str, str], env_client: dict[str, str],
                        case_dir: Path, xrdp_port: int,
                        processes: list[subprocess.Popen[bytes]],
                        xrdp_log: Path | None,
                        transport_label: str = "rdp-private-wire") -> None:
    """Measure client key injection -> physical X11 -> returned RDP pixels."""
    key_stimulus: subprocess.Popen[bytes] | None = None
    key_injector: subprocess.Popen[bytes] | None = None
    probe: subprocess.Popen[bytes] | None = None
    churn: subprocess.Popen[bytes] | None = None
    churn_thread: threading.Thread | None = None
    churn_stop = threading.Event()
    input_x = args.input_x
    input_y = args.input_y
    churn_fps = args.input_churn_fps
    if churn_fps is None:
        churn_fps = args.fps

    try:
        if churn_fps > 0:
            churn = subprocess.Popen(
                [str(GPU_STIMULUS), args.display, str(args.width), str(args.height)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=env_source, bufsize=0, start_new_session=True,
            )
            if churn.stdin is None or churn.stdout is None:
                raise RuntimeError("churn stimulus pipes were not created")
            churn_reader = LineReader(churn.stdout)
            first = churn_reader.readline(5)
            if first.startswith(b"SWAP_CONTROL "):
                print(f"{name}: {first.decode(errors='replace').strip()}")
                first = churn_reader.readline(5)
            if not first.startswith(b"READY "):
                raise RuntimeError(f"churn stimulus did not become ready: {first!r}")

            def churn_frames() -> None:
                next_tick = time.monotonic()
                while not churn_stop.is_set():
                    next_tick += 1.0 / churn_fps
                    try:
                        churn.stdin.write(b"frame\n")
                        churn.stdin.flush()
                        if not churn_reader.readline(2):
                            break
                    except (BrokenPipeError, OSError):
                        break
                    churn_stop.wait(max(0.0, next_tick - time.monotonic()))

            churn_thread = threading.Thread(target=churn_frames,
                                            name="xrdp-bench-churn", daemon=True)
            churn_thread.start()

        # Map and focus the key target after the churn window so background
        # drawing cannot take keyboard focus away from the physical :0 target.
        key_stimulus = subprocess.Popen(
            [str(KEY_STIMULUS), args.display, "--key", str(input_x), str(input_y)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env_source, bufsize=0, start_new_session=True,
        )
        if key_stimulus.stdout is None:
            raise RuntimeError("key stimulus output pipe was not created")
        key_reader = LineReader(key_stimulus.stdout)
        if not key_reader.readline(5).startswith(b"READY "):
            raise RuntimeError("physical key stimulus did not become ready")

        key_injector = subprocess.Popen(
            [str(KEY_INJECTOR), client_display, window],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env_client, bufsize=0, start_new_session=True,
        )
        if key_injector.stdin is None or key_injector.stdout is None:
            raise RuntimeError("key injector pipes were not created")
        injector_reader = LineReader(key_injector.stdout)
        if not injector_reader.readline(5).startswith(b"READY "):
            raise RuntimeError("client key injector did not become ready")

        marker_x = input_x + 80
        marker_y = input_y + 50
        probe = subprocess.Popen(
            [str(PIXEL_PROBE), client_display, window,
             str(marker_x), str(marker_y)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env_client, bufsize=0, start_new_session=True,
        )
        if probe.stdin is None or probe.stdout is None:
            raise RuntimeError("pixel probe pipes were not created")
        probe_reader = LineReader(probe.stdout)
        if not probe_reader.readline(5).startswith(b"READY "):
            raise RuntimeError("input pixel probe did not become ready")

        # Warm up the RDP input path and establish a known remote marker.
        key_injector.stdin.write(b"pulse\n")
        key_injector.stdin.flush()
        injected_line = injector_reader.readline(5)
        physical_line = key_reader.readline(5)
        if not injected_line or not physical_line:
            raise RuntimeError("input warm-up did not traverse the RDP/VNC path")
        t1_event_ns, t1_draw_done_ns, physical_state = parse_physical_marker(
            physical_line)
        # The Xlib stimulus's state 0 is red / state 1 is blue, the inverse of
        # the GL marker convention used by wait_marker().
        if wait_marker(probe_reader, probe.stdin, 1 - physical_state,
                       t1_draw_done_ns,
                       args.timeout, args.poll_ms / 1000.0,
                       diagnostic=True) is None:
            raise RuntimeError("input warm-up marker did not return over RDP")

        samples = max(1, int(args.duration * args.input_hz))
        input_delivery_ms: list[float] = []
        local_draw_ms: list[float] = []
        return_graphics_ms: list[float] = []
        roundtrip_ms: list[float] = []
        point_draw_ns: list[int] = []
        point_visible_ns: list[int] = []
        point_expected_states: list[int] = []
        delivery_misses = 0
        return_misses = 0
        measured_processes = list(processes)
        if churn is not None:
            measured_processes.append(churn)
        cpu_start = {proc.pid: process_cpu_tree(proc) for proc in measured_processes}
        rss_start = {proc.pid: process_rss_tree(proc) for proc in measured_processes}
        transport_start = tcp_snapshot(xrdp_port)
        wall_start = time.monotonic()
        next_tick = wall_start

        for _ in range(samples):
            next_tick += 1.0 / args.input_hz
            key_injector.stdin.write(b"pulse\n")
            key_injector.stdin.flush()
            injected_line = injector_reader.readline(args.timeout)
            if not injected_line:
                delivery_misses += 1
            else:
                t0_ns = int(injected_line.strip())
                physical_line = key_reader.readline(args.timeout)
                if not physical_line:
                    delivery_misses += 1
                else:
                    t1_event_ns, t1_draw_done_ns, physical_state = \
                        parse_physical_marker(physical_line)
                    input_ms = (t1_event_ns - t0_ns) / 1e6
                    local_ms = (t1_draw_done_ns - t1_event_ns) / 1e6
                    input_delivery_ms.append(input_ms)
                    local_draw_ms.append(local_ms)
                    observed_ns: list[int] = []
                    expected_marker_state = 1 - physical_state
                    return_ms = wait_marker(
                        probe_reader, probe.stdin, expected_marker_state,
                        t1_draw_done_ns,
                        args.timeout, args.poll_ms / 1000.0,
                        observed_ns_out=observed_ns,
                    )
                    if return_ms is None:
                        return_misses += 1
                    else:
                        return_graphics_ms.append(return_ms)
                        roundtrip_ms.append(input_ms + local_ms + return_ms)
                        point_draw_ns.append(t1_draw_done_ns)
                        point_visible_ns.append(observed_ns[0])
                        point_expected_states.append(expected_marker_state)
            delay = next_tick - time.monotonic()
            if delay > 0:
                time.sleep(delay)

        print(f"{name}#{repetition}: input-roundtrip churn={churn_fps:.1f}fps "
              f"input={args.input_hz:.1f}Hz samples={samples} "
              f"delivery_misses={delivery_misses} return_misses={return_misses}")
        summarize_input_latencies("input T0->T1_event", input_delivery_ms)
        summarize_input_latencies("local T1_event->draw", local_draw_ms)
        summarize_input_latencies("graphics draw->T2", return_graphics_ms)
        summarize_input_latencies("roundtrip T0->T2", roundtrip_ms)
        elapsed = max(0.001, time.monotonic() - wall_start)
        metrics = []
        for proc in measured_processes:
            before = cpu_start.get(proc.pid, 0.0)
            cpu = max(0.0, process_cpu_tree(proc) - before) / elapsed * 100.0
            metrics.append(
                f"pid{proc.pid} cpu={cpu:.1f}% "
                f"rss={rss_start.get(proc.pid, 0) / 1048576:.1f}->"
                f"{process_rss_tree(proc) / 1048576:.1f}MiB")
        print(f"{'':18} " + "; ".join(metrics))
        transport_end = tcp_snapshot(xrdp_port)
        print_transport_summary(transport_label, transport_start,
                                transport_end, elapsed)
        if xrdp_log is not None:
            summarize_xrdp_profile(
                xrdp_log, f"{name}#{repetition} fps={churn_fps:.1f}",
                point_draw_ns, point_visible_ns, point_expected_states)
    finally:
        churn_stop.set()
        if churn_thread is not None:
            churn_thread.join(timeout=2)
        for proc in (probe, key_injector, key_stimulus, churn):
            kill_process(proc)


def run_vnc_viewer_case(args: argparse.Namespace, name: str, profile: str,
                        auth: str, runtime: Path, base_port: int,
                        repetition: int) -> None:
    """Measure the physical X -> x11vnc -> real VNC viewer path."""
    x, y = 20, 20
    vnc_port = base_port + 2575
    client_display = f":{args.client_display}"
    case_dir = runtime / f"{name}-{repetition}"
    case_dir.mkdir()
    env_source = os.environ.copy()
    env_source.update({"DISPLAY": args.display, "XAUTHORITY": auth})
    env_client = os.environ.copy()
    env_client.update({"DISPLAY": client_display})
    vnc_command = [
        str(X11VNC), "-display", args.display, "-auth", auth, "-localhost",
        "-listen", "127.0.0.1", "-no6", "-rfbport", str(vnc_port),
        "-nopw", "-forever", "-shared", "-xdamage", "-xd_mem", "0",
        "-threads", "-repeat", "-input_skip", "1", "-wait", "5",
        "-defer", "5", "-deferupdate", "5", "-wait_ui", "2",
        "-setdefer", "-1", "-scrollcopyrect", args.scrollcopyrect, "-quiet",
    ]
    profile_flags = list(VNC_PROFILES[profile])
    if "-noxdamage" in profile_flags:
        vnc_command[vnc_command.index("-xdamage")] = "-noxdamage"
        profile_flags.remove("-noxdamage")
    vnc_command += profile_flags
    viewer_command = [
        str(VNC_VIEWER), "-display", client_display,
        "-SecurityTypes", "None", "-PreferredEncoding", "Raw",
        "-FullColor", "-Shared", "-RemoteResize=0", "-ViewOnly=0",
        "-SendClipboard=0", "-AcceptClipboard=0", "-AlertOnFatalError=0",
        "-ReconnectOnError=0", "-geometry",
        f"{args.client_width}x{args.client_height}+0+0",
        f"127.0.0.1::{vnc_port}",
    ]
    xvfb: subprocess.Popen[bytes] | None = None
    vnc: subprocess.Popen[bytes] | None = None
    viewer: subprocess.Popen[bytes] | None = None
    stimulus: subprocess.Popen[bytes] | None = None
    probe: subprocess.Popen[bytes] | None = None
    try:
        assert_tcp_port_available("127.0.0.1", vnc_port, check_ipv6=True)
        print(f"{name}#{repetition}: transport=vnc-viewer private-loopback")
        xvfb = start_process(
            ["/usr/bin/Xvfb", client_display, "-screen", "0",
             f"{args.client_width}x{args.client_height}x24", "-nolisten", "tcp"],
            env_client, case_dir / "xvfb.log")
        time.sleep(0.4)
        if xvfb.poll() is not None:
            raise RuntimeError(f"Xvfb exited; see {case_dir / 'xvfb.log'}")
        vnc = start_process(vnc_command, env_source, case_dir / "x11vnc.log")
        vnc_probe = wait_vnc_port(vnc_port, time.monotonic() + 8)
        vnc_probe.close()
        viewer = start_process(
            viewer_command, env_client, case_dir / "vncviewer.log")
        window = find_vnc_viewer_window(
            client_display, vnc_port, time.monotonic() + 15)
        measured_processes = [proc for proc in (xvfb, vnc, viewer)
                              if proc is not None]
        if args.mode == "input-roundtrip":
            run_input_roundtrip(
                args, name, repetition, window, client_display, env_source,
                env_client, case_dir, vnc_port, measured_processes, None,
                transport_label="vnc-viewer-wire")
            return

        marker_x = x + args.width // 2
        marker_y = y + args.height // 2
        stimulus = subprocess.Popen(
            [str(GPU_STIMULUS), args.display, str(args.width), str(args.height)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env_source, bufsize=0, start_new_session=True,
        )
        if stimulus.stdin is None or stimulus.stdout is None:
            raise RuntimeError("GPU stimulus pipes were not created")
        stimulus_reader = LineReader(stimulus.stdout)
        first = stimulus_reader.readline(5)
        if first.startswith(b"SWAP_CONTROL "):
            print(f"{name}: {first.decode(errors='replace').strip()}")
            first = stimulus_reader.readline(5)
        if not first.startswith(b"READY "):
            raise RuntimeError(f"stimulus did not become ready: {first!r}")
        fields = first.split()
        if len(fields) < 2 or fields[1] != f"{args.width}x{args.height}".encode():
            raise RuntimeError(f"stimulus dimensions differ from request: {first!r}")
        probe = subprocess.Popen(
            [str(PIXEL_PROBE), client_display, window,
             str(marker_x), str(marker_y)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env_client, bufsize=0, start_new_session=True,
        )
        if probe.stdin is None or probe.stdout is None:
            raise RuntimeError("pixel probe pipes were not created")
        probe_reader = LineReader(probe.stdout)
        if not probe_reader.readline(5).startswith(b"READY "):
            raise RuntimeError("VNC viewer pixel probe did not become ready")
        stimulus.stdin.write(b"frame\n")
        stimulus.stdin.flush()
        warmup = stimulus_reader.readline(5)
        if not warmup:
            raise RuntimeError("stimulus warm-up failed")
        warmup_fields = warmup.split()
        if wait_marker(
                probe_reader, probe.stdin, int(warmup_fields[1]),
                int(warmup_fields[0]), args.timeout, args.poll_ms / 1000.0,
                diagnostic=True) is None:
            raise RuntimeError("VNC viewer marker did not arrive during warm-up")
        samples = max(1, int(args.duration * args.fps))
        latencies: list[float] = []
        render_ns: list[int] = []
        misses = 0
        cpu_start = {proc.pid: process_cpu_tree(proc)
                     for proc in measured_processes}
        rss_start = {proc.pid: process_rss_tree(proc)
                     for proc in measured_processes}
        transport_start = tcp_snapshot(vnc_port)
        wall_start = time.monotonic()
        next_tick = wall_start
        for _ in range(samples):
            next_tick += 1.0 / args.fps
            stimulus.stdin.write(b"frame\n")
            stimulus.stdin.flush()
            line = stimulus_reader.readline(5)
            fields = line.split()
            if len(fields) < 3:
                misses += 1
            else:
                visible_ns = int(fields[0])
                state = int(fields[1])
                render_ns.append(int(fields[2]))
                latency = wait_marker(
                        probe_reader, probe.stdin, state, visible_ns,
                        args.timeout, args.poll_ms / 1000.0)
                if latency is None:
                    misses += 1
                else:
                    latencies.append(latency)
            delay = next_tick - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        transport_end = tcp_snapshot(vnc_port)
        summarize(f"{name}#{repetition}", latencies, misses, samples, render_ns,
                  measured_processes, cpu_start, wall_start, rss_start,
                  transport_label="vnc-viewer-wire",
                  transport_start=transport_start, transport_end=transport_end)
    finally:
        for proc in (probe, stimulus, viewer, vnc, xvfb):
            kill_process(proc)


def run_case(args: argparse.Namespace, name: str, profile: str,
             auth: str, runtime: Path, base_port: int,
             repetition: int) -> None:
    x, y = 20, 20
    # Keep the two private listeners in distinct ranges.  The high VNC port
    # also avoids colliding with any development RDP listener a user may have.
    vnc_port = base_port + 2575
    vnc_backend_port = vnc_port + 1
    xrdp_port = base_port
    client_display = f":{args.client_display}"
    network = SyntheticNetwork(args)
    case_dir = runtime / f"{name}-{repetition}"
    case_dir.mkdir()
    cert = case_dir / "cert.pem"
    key = case_dir / "key.pem"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-subj", "/CN=localhost", "-keyout", str(key), "-out", str(cert),
         "-days", "1"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        check=True,
    )
    key.chmod(0o600)
    xrdp_log = case_dir / "xrdp.log"
    config = case_dir / "xrdp.ini"
    chansrv_path = Path("/run/xrdp/sockdir") / str(os.getuid()) / \
        f"xrdp_chansrv_socket_{args.client_display}"
    chansrv_api_path = Path("/run/xrdp/sockdir") / str(os.getuid()) / \
        f"xrdpapi_{args.client_display}"
    if not args.no_chansrv:
        for stale_socket in (chansrv_path, chansrv_api_path):
            remove_stale_unix_socket(stale_socket)
    rewrite_xrdp_config(
        Path("/etc/xrdp/xrdp.ini"), config, xrdp_port, vnc_port,
        xrdp_log, chansrv_path, cert, key, args.max_bpp,
        args.disable_gfx_for_vnc, args.enable_gfx_for_vnc, args.console_lib,
        args.bitmap_compression, args.bulk_compression,
        args.disable_dynamic_resizing,
        network.host_ip,
    )
    if args.no_chansrv:
        remove_chansrv_from_config(config)
    env_source = os.environ.copy()
    env_source.update({"DISPLAY": args.display, "XAUTHORITY": auth})
    env_client = os.environ.copy()
    env_client.update({
        "DISPLAY": client_display,
        "LD_LIBRARY_PATH": str(ISOLATED / "rdp-bench-root/usr/lib/x86_64-linux-gnu") + ":/usr/local/lib:/usr/local/lib/xrdp",
    })
    vnc_command = [
        str(X11VNC), "-display", args.display, "-auth", auth, "-localhost",
        "-listen", "127.0.0.1", "-no6", "-rfbport", str(vnc_backend_port),
        # This x11vnc is a private, loopback-only backend.  Keeping its RFB
        # security type at None avoids a legacy xrdp VNC-module password
        # exchange that is not needed for this isolated harness; production
        # x11vnc continues to use its configured -rfbauth password.
        "-nopw", "-forever",
        "-shared", "-xdamage", "-xd_mem", "0", "-threads", "-repeat",
        "-input_skip", "1", "-wait", "5", "-defer", "5", "-deferupdate",
        "5", "-wait_ui", "2", "-setdefer", "-1", "-scrollcopyrect",
        args.scrollcopyrect, "-quiet",
    ]
    profile_flags = list(VNC_PROFILES[profile])
    if "-noxdamage" in profile_flags:
        vnc_command[vnc_command.index("-xdamage")] = "-noxdamage"
        profile_flags.remove("-noxdamage")
    vnc_command += profile_flags
    xvfb: subprocess.Popen[bytes] | None = None
    vnc: subprocess.Popen[bytes] | None = None
    proxy: subprocess.Popen[bytes] | None = None
    chansrv: subprocess.Popen[bytes] | None = None
    xrdp: subprocess.Popen[bytes] | None = None
    client: subprocess.Popen[bytes] | None = None
    stimulus: subprocess.Popen[bytes] | None = None
    probe: subprocess.Popen[bytes] | None = None
    try:
        network.setup()
        assert_tcp_port_available(network.host_ip, xrdp_port)
        assert_tcp_port_available("127.0.0.1", vnc_port, check_ipv6=True)
        assert_tcp_port_available("127.0.0.1", vnc_backend_port)
        print(f"{name}#{repetition}: {network.header()}")
        xvfb = start_process(
            ["/usr/bin/Xvfb", client_display, "-screen", "0",
             f"{args.client_width}x{args.client_height}x24", "-nolisten", "tcp"], env_client,
            case_dir / "xvfb.log",
        )
        time.sleep(0.4)
        if xvfb.poll() is not None:
            raise RuntimeError(f"Xvfb exited; see {case_dir / 'xvfb.log'}")
        chansrv_env = env_client.copy()
        chansrv_env.pop("XAUTHORITY", None)
        chansrv_env.update({
            "HOME": str(HOME),
            "XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}",
            "CHANSRV_LOG_PATH": str(case_dir),
        })
        if not args.no_chansrv:
            chansrv = start_process(
                [str(CHANSRV)], chansrv_env, case_dir / "chansrv-stderr.log",
            )
            wait_path(chansrv_path, time.monotonic() + 8)
        vnc = start_process(vnc_command, env_source, case_dir / "x11vnc.log")
        vnc_probe = wait_vnc_port(vnc_backend_port, time.monotonic() + 8)
        vnc_probe.close()
        # xrdp's VNC transport opens an IPv6 socket for a 127.0.0.1 backend
        # and tries ::1 first. Keep x11vnc IPv4-only (matching production),
        # while forwarding that local IPv6 hop to its IPv4 listener. This
        # helper is loopback-only and is part of the benchmark harness.
        proxy = start_process(
            [sys.executable, str(V6_V4_PROXY), str(vnc_port),
             str(vnc_backend_port)], env_source, case_dir / "vnc-proxy.log",
        )
        wait_log_marker(case_dir / "vnc-proxy.log", "READY ",
                        time.monotonic() + 8)
        xrdp_env = env_source.copy()
        xrdp_env.update(args.xrdp_env)
        xrdp = start_process(
            [str(XRDP), "-n", "-c", str(config)], xrdp_env,
            case_dir / "xrdp-stderr.log",
        )
        xrdp_probe = wait_tcp_port(network.host_ip, xrdp_port,
                                   time.monotonic() + 8)
        xrdp_probe.close()
        pipeline_option = f"/{args.pipeline}"
        client_command = network.wrap_client_command(
            [str(FREERDP), f"/v:{network.host_ip}:{xrdp_port}", "/u:na", "/p:na",
             "/cert:ignore",
             f"/size:{args.client_width}x{args.client_height}",
             pipeline_option, "/network:lan",
             "/t:xrdp-gpu-bench", "-decorations", "/window-position:0x0",
             "/log-level:WARN"], env_client,
        )
        client = start_process(
            client_command, env_client, case_dir / "xfreerdp.log",
            preserve_tty=network.enabled,
        )
        complete_deadline = time.monotonic() + 20
        while time.monotonic() < complete_deadline:
            if client.poll() is not None:
                raise RuntimeError(f"FreeRDP exited with {client.returncode}; see {case_dir / 'xfreerdp.log'}")
            xrdp_state = xrdp_log.read_text(errors="replace")
            # libvnc reports successful protocol negotiation through these
            # normal xrdp manager messages.  The older "VNC connection
            # complete" text is not emitted by current xrdp builds.
            if ("VNC User disabled EXTENDED_DESKTOP_SIZE" in xrdp_state or
                    "VNC: Clipboard (if available)" in xrdp_state):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError(f"xrdp did not complete VNC connection; see {xrdp_log}")
        negotiated = "unknown"
        log_text = xrdp_log.read_text(errors="replace")
        # The private xrdp instance can log an initial capability probe before
        # the real FreeRDP connection.  Use the last encoder-start message so
        # a per-profile GFX fallback is reported accurately.
        gfx_pos = log_text.rfind("starting gfx")
        rfx_pos = log_text.rfind("starting rfx codec session")
        policy_pos = max(
            log_text.rfind(
                "Disabling GFX for the selected VNC backend before capability negotiation"
            ),
            log_text.rfind("Disabling GFX as 'drdynvc' isn't available"),
        )
        # An RFX encoder start after the VNC connection is the stronger
        # observation: it proves that GFX was not selected.  Keep the policy
        # message as a fallback for builds which suppress both encoder
        # diagnostics and GFX at low colour depth.
        latest_encoder_pos = max(gfx_pos, rfx_pos)
        if policy_pos > latest_encoder_pos or (
                args.max_bpp < 32 and latest_encoder_pos < 0):
            negotiated = "GFX_DISABLED"
        elif latest_encoder_pos >= 0:
            negotiated = "GFX" if gfx_pos > rfx_pos else "RFX"
        else:
            raise RuntimeError(
                "xrdp did not report a negotiated graphics encoder"
            )
        if args.disable_gfx_for_vnc and negotiated == "GFX":
            raise RuntimeError("GFX remained active after the Console policy")
        print(f"{name}#{repetition}: requested={args.pipeline.upper()} negotiated={negotiated}")
        window = find_window(client_display, "xrdp-gpu-bench", time.monotonic() + 10)
        measured_processes = [proc for proc in
                              (xvfb, chansrv, proxy, vnc, xrdp, client)
                              if proc is not None]
        if args.mode == "input-roundtrip":
            run_input_roundtrip(
                args, name, repetition, window, client_display, env_source,
                env_client, case_dir, xrdp_port, measured_processes, xrdp_log)
            return
        marker_x = x + args.width // 2
        marker_y = y + args.height // 2
        stimulus = subprocess.Popen(
            [str(GPU_STIMULUS), args.display, str(args.width), str(args.height)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env_source, bufsize=0, start_new_session=True,
        )
        if stimulus.stdin is None or stimulus.stdout is None:
            raise RuntimeError("GPU stimulus pipes were not created")
        stimulus_reader = LineReader(stimulus.stdout)
        first = stimulus_reader.readline(5)
        if first.startswith(b"SWAP_CONTROL "):
            print(f"{name}: {first.decode(errors='replace').strip()}")
            first = stimulus_reader.readline(5)
        if not first.startswith(b"READY "):
            raise RuntimeError(f"stimulus did not become ready: {first!r}")
        fields = first.split()
        if len(fields) < 2 or fields[1] != f"{args.width}x{args.height}".encode():
            raise RuntimeError(f"stimulus dimensions differ from request: {first!r}")
        probe = subprocess.Popen(
            [str(PIXEL_PROBE), client_display, window, str(marker_x), str(marker_y)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env_client, bufsize=0, start_new_session=True,
        )
        if probe.stdin is None or probe.stdout is None:
            raise RuntimeError("pixel probe pipes were not created")
        probe_reader = LineReader(probe.stdout)
        if not probe_reader.readline(5).startswith(b"READY "):
            raise RuntimeError("pixel probe did not become ready")
        # Start with an unmeasured frame. This establishes a known remote color
        # so the first measured toggle cannot match a stale initial pixel.
        stimulus.stdin.write(b"frame\n")
        stimulus.stdin.flush()
        warmup = stimulus_reader.readline(5)
        if not warmup:
            raise RuntimeError("stimulus warm-up failed")
        warmup_fields = warmup.split()
        warmup_latency = wait_marker(
            probe_reader, probe.stdin, int(warmup_fields[1]), int(warmup_fields[0]),
            args.timeout, args.poll_ms / 1000.0, diagnostic=True,
        )
        if warmup_latency is None:
            print("remote diagnostic: " + "; ".join(sample_remote_pixels(
                client_display, window,
                [(0, 0), (marker_x, marker_y), (args.width // 2, args.height // 2)],
                env_client)))
            raise RuntimeError("remote marker did not arrive during warm-up")
        samples = max(1, int(args.duration * args.fps))
        latencies: list[float] = []
        render_ns: list[int] = []
        point_draw_ns: list[int] = []
        point_visible_ns: list[int] = []
        point_expected_states: list[int] = []
        misses = 0
        process_list = measured_processes
        cpu_start = {proc.pid: process_cpu_tree(proc) for proc in process_list}
        rss_start = {proc.pid: process_rss_tree(proc) for proc in process_list}
        transport_start = tcp_snapshot(xrdp_port)
        wall_start = time.monotonic()
        next_tick = wall_start
        for _ in range(samples):
            next_tick += 1.0 / args.fps
            stimulus.stdin.write(b"frame\n")
            stimulus.stdin.flush()
            line = stimulus_reader.readline(5)
            fields = line.split()
            if len(fields) < 3:
                misses += 1
            else:
                visible_ns = int(fields[0])
                state = int(fields[1])
                render_ns.append(int(fields[2]))
                observed_ns: list[int] = []
                latency = wait_marker(
                    probe_reader, probe.stdin, state, visible_ns, args.timeout,
                    args.poll_ms / 1000.0, observed_ns_out=observed_ns,
                )
                if latency is None:
                    misses += 1
                else:
                    latencies.append(latency)
                    point_draw_ns.append(visible_ns)
                    point_visible_ns.append(observed_ns[0])
                    point_expected_states.append(state)
            delay = next_tick - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        transport_end = tcp_snapshot(xrdp_port)
        summarize(f"{name}#{repetition}", latencies, misses, samples, render_ns,
                  process_list, cpu_start, wall_start, rss_start,
                  transport_label="rdp-private-wire",
                  transport_start=transport_start, transport_end=transport_end)
        summarize_xrdp_profile(
            xrdp_log, f"{name}#{repetition} fps={args.fps:.1f}",
            point_draw_ns, point_visible_ns, point_expected_states)
    finally:
        for proc in (probe, stimulus, client, xrdp, proxy, vnc, chansrv, xvfb):
            kill_process(proc, privileged=(proc is client and network.enabled))
        network.cleanup()


def main() -> int:
    global XRDP, X11VNC
    parser = argparse.ArgumentParser()
    parser.add_argument("--display", default=":0")
    parser.add_argument("--auth")
    parser.add_argument("--duration", type=float, default=20.0)
    parser.add_argument("--fps", type=float, default=15.0)
    parser.add_argument("--mode", choices=("graphics", "input-roundtrip"),
                        default="graphics",
                        help="measure graphics latency or full input round-trip")
    parser.add_argument(
        "--transport", choices=("rdp", "rfb", "vnc-viewer"), default="rdp",
        help=("client transport: complete private RDP path, direct RAW-RFB "
              "wire baseline, or a real VNC viewer (default: rdp)"))
    parser.add_argument("--input-hz", type=float, default=5.0,
                        help="key pulses per second in input-roundtrip mode")
    parser.add_argument("--input-churn-fps", type=float,
                        help="background GL churn rate; 0 gives an idle control")
    parser.add_argument("--input-x", type=int, default=1180,
                        help="physical key marker X coordinate")
    parser.add_argument("--input-y", type=int, default=20,
                        help="physical key marker Y coordinate")
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--poll-ms", type=float, default=3.0)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=640)
    parser.add_argument("--pipeline", choices=("gfx", "rfx"), default="gfx",
                        help="FreeRDP graphics path to request (default: gfx)")
    parser.add_argument("--max-bpp", type=int, choices=(16, 24, 32), default=32,
                        help="private xrdp color depth (default: 32)")
    parser.add_argument("--disable-gfx-for-vnc", action="store_true",
                        help=("set global drdynvc=false and Console "
                              "channel.drdynvc=false (the effective GFX "
                              "disable switch)"))
    parser.add_argument("--enable-gfx-for-vnc", action="store_true",
                        help=("set channel.drdynvc=true in the private Console "
                              "profile (allow GFX negotiation)"))
    parser.add_argument("--disable-dynamic-resizing", action="store_true",
                        help=("disable the private Console profile's dynamic "
                              "RDP monitor resize requests"))
    parser.add_argument("--client-display", type=int, default=99)
    parser.add_argument("--client-width", type=int, default=1366,
                        help="isolated RDP client width (default: 1366)")
    parser.add_argument("--client-height", type=int, default=768,
                        help="isolated RDP client height (default: 768)")
    parser.add_argument("--base-port", type=int, default=3390,
                        help="private xrdp port; x11vnc uses the next port")
    parser.add_argument("--only", choices=tuple(VNC_PROFILES),
                        help="run one x11vnc profile (default: baseline and lan)")
    parser.add_argument("--repetitions", type=int, default=1,
                        help="number of independent runs per profile (default: 1)")
    parser.add_argument("--xrdp", default=str(XRDP))
    parser.add_argument("--x11vnc", default=str(X11VNC),
                        help="x11vnc binary used by the isolated backend")
    parser.add_argument("--console-lib", default="libvnc.so",
                        help="VNC module filename for the private xrdp instance")
    parser.add_argument("--bitmap-compression", choices=("on", "off"),
                        help="override classic RDP bitmap compression in the private instance")
    parser.add_argument("--bulk-compression", choices=("on", "off"),
                        help="override RDP bulk compression in the private instance")
    parser.add_argument("--no-chansrv", action="store_true",
                        help="omit the private chansrv socket for pixel-only runs")
    parser.add_argument(
        "--scrollcopyrect", choices=("never", "mouse", "keys", "always"),
        default="never",
        help="x11vnc scrollcopyrect mode (default: never)")
    parser.add_argument(
        "--network-mode", choices=("auto", "localhost", "namespace"),
        default="auto",
        help=("transport for FreeRDP: auto selects a private veth/netns when "
              "impairment is requested (default: auto)"))
    parser.add_argument(
        "--network-delay-ms", type=float, default=0.0,
        help="symmetric-link one-way netem delay in milliseconds (default: 0)")
    parser.add_argument(
        "--network-jitter-ms", type=float, default=0.0,
        help="one-way netem delay jitter in milliseconds (default: 0)")
    parser.add_argument(
        "--network-loss-percent", type=float, default=0.0,
        help="random packet loss applied in each direction (default: 0)")
    parser.add_argument(
        "--network-rate-mbps", type=float,
        help="optional netem rate limit applied in each direction")
    parser.add_argument(
        "--network-self-test", action="store_true",
        help="create the veth/netns, print qdiscs, and measure ping RTT, then exit")
    parser.add_argument(
        "--xrdp-env", action="append", default=[], metavar="NAME=VALUE",
        help="environment override for the private xrdp process; repeatable")
    args = parser.parse_args()
    xrdp_env: dict[str, str] = {}
    for assignment in args.xrdp_env:
        name, separator, value = assignment.partition("=")
        if not separator or not name or not name.replace("_", "A").isalnum():
            parser.error(f"invalid --xrdp-env assignment: {assignment!r}")
        xrdp_env[name] = value
    args.xrdp_env = xrdp_env
    args.bitmap_compression = (None if args.bitmap_compression is None
                               else args.bitmap_compression == "on")
    args.bulk_compression = (None if args.bulk_compression is None
                             else args.bulk_compression == "on")
    XRDP = Path(args.xrdp)
    X11VNC = Path(args.x11vnc)
    if (args.width <= 0 or args.height <= 0 or
            args.client_width <= 0 or args.client_height <= 0 or
            args.duration <= 0 or args.fps <= 0 or args.repetitions <= 0 or
            args.input_hz <= 0 or
            (args.input_churn_fps is not None and args.input_churn_fps < 0)):
        parser.error("dimensions, rates, duration, and repetitions must be valid")
    if args.disable_gfx_for_vnc and args.enable_gfx_for_vnc:
        parser.error("--enable-gfx-for-vnc and --disable-gfx-for-vnc are mutually exclusive")
    if args.transport in {"rfb", "vnc-viewer"} and (
            args.network_mode == "namespace" or args.network_self_test or
            args.network_delay_ms != 0 or args.network_jitter_ms != 0 or
            args.network_loss_percent != 0 or args.network_rate_mbps is not None):
        parser.error(
            "direct RFB and the VNC-viewer baseline use private loopback; "
            "network impairment is RDP-only")
    if args.network_self_test and args.network_mode == "localhost":
        parser.error("--network-self-test requires --network-mode namespace or auto")
    if args.network_self_test:
        args.network_mode = "namespace"
    network_impairment = (
        args.network_delay_ms != 0 or args.network_jitter_ms != 0 or
        args.network_loss_percent != 0 or args.network_rate_mbps is not None
    )
    if args.network_mode == "auto":
        args.network_mode = "namespace" if network_impairment else "localhost"
    if args.network_mode == "localhost" and network_impairment:
        parser.error(
            "network impairment requires --network-mode namespace (or leave "
            "--network-mode auto)"
        )
    if (not all(math.isfinite(value) for value in (
            args.network_delay_ms, args.network_jitter_ms,
            args.network_loss_percent,
            (args.network_rate_mbps if args.network_rate_mbps is not None else 0.0))) or
            args.network_delay_ms < 0 or args.network_jitter_ms < 0 or
            args.network_loss_percent < 0 or args.network_loss_percent >= 100 or
            (args.network_rate_mbps is not None and args.network_rate_mbps <= 0)):
        parser.error(
            "network delay/jitter must be nonnegative, loss must be in [0,100), "
            "and rate must be positive"
        )
    if args.network_self_test:
        network = SyntheticNetwork(args)
        try:
            network.setup()
            print(network.header())
            print(network.diagnostics())
            return 0
        except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
            print(f"BENCHMARK ERROR: {exc}", file=sys.stderr)
            return 2
        finally:
            network.cleanup()
    required_executables = [X11VNC, GPU_STIMULUS]
    if args.transport == "rdp":
        required_executables += [
            PIXEL_PROBE, V6_V4_PROXY, FREERDP, XRDP,
            Path("/usr/bin/Xvfb"), Path("/usr/bin/xwininfo"),
        ]
    elif args.transport == "vnc-viewer":
        required_executables += [
            PIXEL_PROBE, VNC_VIEWER, Path("/usr/bin/Xvfb"),
            Path("/usr/bin/xwininfo"),
        ]
    if args.mode == "input-roundtrip":
        required_executables.append(KEY_STIMULUS)
        if args.transport in {"rdp", "vnc-viewer"}:
            required_executables.append(KEY_INJECTOR)
    for required in required_executables:
        if not required.is_file() and not shutil.which(str(required)):
            parser.error(f"required executable is missing: {required}")
    try:
        auth = discover_auth(args.auth)
        ISOLATED.mkdir(parents=True, exist_ok=True)
        runtime = Path(tempfile.mkdtemp(prefix="xrdp-vnc-gpu-", dir=ISOLATED))
        print("xrdp -> x11vnc end-to-end GPU/compositor benchmark")
        print("Private test services are started; production ports 5900/3389 are untouched.")
        print(f"source={args.display} client=:{args.client_display} "
                  f"client_size={args.client_width}x{args.client_height} "
                  f"workload={args.width}x{args.height} "
                  f"duration={args.duration:.1f}s fps={args.fps:.1f} "
                  f"mode={args.mode} transport={args.transport} "
                  f"rdp_pipeline={args.pipeline.upper()} "
                  f"max_bpp={args.max_bpp} "
                  f"disable_gfx_for_vnc={args.disable_gfx_for_vnc} "
                  f"enable_gfx_for_vnc={args.enable_gfx_for_vnc} "
                  f"disable_dynamic_resizing={args.disable_dynamic_resizing} "
                  f"scrollcopyrect={args.scrollcopyrect} "
                  f"network_mode={args.network_mode} "
                  f"network_delay_ms={args.network_delay_ms:g} "
                  f"network_jitter_ms={args.network_jitter_ms:g} "
                  f"network_loss_percent={args.network_loss_percent:g} "
                  f"network_rate_mbps={('unlimited' if args.network_rate_mbps is None else f'{args.network_rate_mbps:g}')} "
                  f"xrdp_env={args.xrdp_env or '{}'} "
                  f"repetitions={args.repetitions} x11vnc={X11VNC}")
        cases = [("baseline", "baseline"), ("lan", "lan")]
        if args.only:
            cases = [(args.only, args.only)]
        for index, (name, use_lan) in enumerate(cases):
            for repetition in range(1, args.repetitions + 1):
                port_offset = (index * args.repetitions + repetition - 1) * 10
                if args.transport == "rfb":
                    if args.mode == "input-roundtrip":
                        run_direct_rfb_input_case(
                            args, name, use_lan, auth, runtime,
                            args.base_port + port_offset, repetition)
                    else:
                        run_direct_rfb_case(
                            args, name, use_lan, auth, runtime,
                            args.base_port + port_offset, repetition)
                elif args.transport == "vnc-viewer":
                    run_vnc_viewer_case(
                        args, name, use_lan, auth, runtime,
                        args.base_port + port_offset, repetition)
                else:
                    run_case(args, name, use_lan, auth, runtime,
                             args.base_port + port_offset, repetition)
        print(f"runtime logs: {runtime}")
        return 0
    except KeyboardInterrupt:
        print("BENCHMARK interrupted; isolated child processes were cleaned up",
              file=sys.stderr)
        return 130
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"BENCHMARK ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
