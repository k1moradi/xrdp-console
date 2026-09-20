#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Probe VS Code/Electron hardware rendering on the physical X11 display.

The probe uses an isolated temporary profile and no extensions, so it does not
change the user's running Code instance. It asks Chromium's GPU process for
SystemInfo and reports the active rendering path. An optional device regular
expression can turn a host-specific expectation into a checked condition. This
is a capability check, not a claim that x11vnc itself can capture through
Vulkan or GL.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
from typing import Any


def http_json(url: str, timeout: float = 1.0) -> Any:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read())


def ws_connect(url: str, timeout: float = 20.0) -> socket.socket:
    if not url.startswith("ws://"):
        raise RuntimeError(f"only ws:// CDP endpoints are supported: {url}")
    address = url[5:]
    host_port, path = address.split("/", 1)
    if ":" in host_port:
        host, port_text = host_port.rsplit(":", 1)
        port = int(port_text)
    else:
        host, port = host_port, 80
    sock = socket.create_connection((host, port), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET /{path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode("ascii")
    sock.sendall(request)
    header = bytearray()
    while b"\r\n\r\n" not in header:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("CDP WebSocket closed during handshake")
        header.extend(chunk)
        if len(header) > 65536:
            raise RuntimeError("CDP WebSocket handshake is too large")
    first_line = bytes(header).split(b"\r\n", 1)[0]
    if not first_line.startswith(b"HTTP/1.1 101"):
        raise RuntimeError(f"CDP WebSocket handshake failed: {first_line!r}")
    expected = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
    ).decode("ascii")
    headers = {}
    for line in bytes(header).decode("latin1").split("\r\n")[1:]:
        if ":" in line:
            name, value = line.split(":", 1)
            headers[name.lower()] = value.strip()
    if headers.get("sec-websocket-accept") != expected:
        raise RuntimeError("CDP WebSocket accept key mismatch")
    sock.settimeout(timeout)
    return sock


def ws_send(sock: socket.socket, payload: bytes) -> None:
    mask = os.urandom(4)
    size = len(payload)
    if size < 126:
        header = bytes((0x81, 0x80 | size))
    elif size <= 0xFFFF:
        header = bytes((0x81, 0x80 | 126)) + struct.pack(">H", size)
    else:
        header = bytes((0x81, 0x80 | 127)) + struct.pack(">Q", size)
    sock.sendall(header + mask + bytes(value ^ mask[index % 4] for index, value in enumerate(payload)))


def ws_recv(sock: socket.socket) -> tuple[int, bytes]:
    header = sock.recv(2)
    if len(header) != 2:
        raise RuntimeError("CDP WebSocket closed")
    first, second = header
    opcode = first & 0x0F
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    masked = bool(second & 0x80)
    mask = recv_exact(sock, 4) if masked else b""
    payload = bytearray(recv_exact(sock, length))
    if masked:
        for index in range(length):
            payload[index] ^= mask[index % 4]
    return opcode, bytes(payload)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("CDP WebSocket closed")
        data.extend(chunk)
    return bytes(data)


def cdp_system_info(sock: socket.socket) -> dict[str, Any]:
    ws_send(sock, json.dumps({"id": 1, "method": "SystemInfo.getInfo"}).encode())
    while True:
        opcode, payload = ws_recv(sock)
        if opcode == 0x9:  # ping; reply with pong
            sock.sendall(bytes((0x8A, len(payload))) + payload)
            continue
        if opcode == 0x8:
            raise RuntimeError("CDP WebSocket closed before SystemInfo response")
        if opcode not in (0x1, 0x2):
            continue
        message = json.loads(payload.decode("utf-8"))
        if message.get("id") == 1:
            if "error" in message:
                raise RuntimeError(str(message["error"]))
            return message["result"]


def wait_debug_endpoint(port: int, deadline: float) -> dict[str, Any]:
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            version = http_json(f"http://127.0.0.1:{port}/json/version")
            if version.get("webSocketDebuggerUrl"):
                return version
        except Exception as exc:  # startup races are expected
            last_error = exc
        time.sleep(0.1)
    raise RuntimeError(f"Electron debugging endpoint did not open: {last_error}")


def gpu_process_cpu(pid: int, seconds: float = 1.0) -> float | None:
    stat_path = Path(f"/proc/{pid}/stat")
    try:
        before = stat_path.read_text().split()
        ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
        start = (int(before[13]) + int(before[14])) / ticks
        wall = time.monotonic()
        time.sleep(seconds)
        after = stat_path.read_text().split()
        end = (int(after[13]) + int(after[14])) / ticks
        elapsed = time.monotonic() - wall
        return max(0.0, (end - start) / elapsed * 100.0)
    except (FileNotFoundError, IndexError, ValueError, OSError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", default=os.getcwd(),
                        help="workspace opened in the isolated Code profile")
    parser.add_argument("--code", default="/usr/share/code/code")
    parser.add_argument("--display", default=os.environ.get("DISPLAY", ":0"))
    parser.add_argument("--dump-json", action="store_true",
                        help="print Chromium SystemInfo JSON for troubleshooting")
    parser.add_argument("--expect-device-regex",
                        help="fail unless the reported GPU device matches this regex")
    args = parser.parse_args()
    if args.expect_device_regex is not None:
        try:
            re.compile(args.expect_device_regex)
        except re.error as exc:
            raise SystemExit(f"invalid --expect-device-regex: {exc}") from exc
    if not Path(args.code).is_file():
        raise SystemExit(f"Code binary not found: {args.code}")
    if not Path(args.workspace).exists():
        raise SystemExit(f"Workspace not found: {args.workspace}")

    with tempfile.TemporaryDirectory(prefix="xrdp-vnc-vscode-gpu-") as root:
        portable = Path(root) / "portable"
        user_data = Path(root) / "user-data"
        extensions = Path(root) / "extensions"
        portable.mkdir()
        user_data.mkdir()
        extensions.mkdir()
        port = 39000 + (os.getpid() % 2000)
        env = os.environ.copy()
        for key in (
            "ELECTRON_RUN_AS_NODE", "VSCODE_DEV", "VSCODE_PORTABLE", "VSCODE_IPC_HOOK_CLI",
            "VSCODE_PID", "VSCODE_CLI", "VSCODE_NLS_CONFIG", "ELECTRON_NO_ATTACH_CONSOLE",
        ):
            env.pop(key, None)
        env["VSCODE_PORTABLE"] = str(portable)
        env["DISPLAY"] = args.display
        command = [
            args.code, f"--user-data-dir={user_data}", f"--extensions-dir={extensions}",
            "--disable-extensions", "--no-sandbox", "--disable-gpu-sandbox",
            f"--remote-debugging-port={port}", "--new-window", args.workspace,
        ]
        process = subprocess.Popen(
            command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            endpoint = wait_debug_endpoint(port, time.monotonic() + 20.0)
            with ws_connect(endpoint["webSocketDebuggerUrl"]) as sock:
                info = cdp_system_info(sock)
            if args.dump_json:
                print(json.dumps(info, indent=2, sort_keys=True))
            gpu = info.get("gpu", {})
            devices = gpu.get("devices", [])
            device = devices[0] if devices else {}
            aux = gpu.get("auxAttributes", {})
            features = gpu.get("featureStatus", {})
            print(f"gpu.vendor={device.get('vendorString', '')}")
            print(f"gpu.device={device.get('deviceString', '')}")
            print(f"gpu.display_type={aux.get('displayType', '')}")
            print(f"gpu.gl_implementation={aux.get('glImplementationParts', '')}")
            print(f"gpu.gl_renderer={aux.get('glRenderer', '')}")
            print(f"gpu.hardware_supports_vulkan={aux.get('hardwareSupportsVulkan', '')}")
            print(f"gpu.process_crash_count={aux.get('processCrashCount', '')}")
            for name in ("gpu_compositing", "opengl", "rasterization", "webgl", "webgpu"):
                print(f"gpu.feature.{name}={features.get(name, '')}")
            gpu_pid = None
            ps = subprocess.run(
                ["pgrep", "-P", str(process.pid), "-f", "--", "--type=gpu-process"],
                capture_output=True, text=True, check=False,
            )
            if ps.stdout.strip():
                gpu_pid = int(ps.stdout.strip().splitlines()[0])
            if gpu_pid is not None:
                usage = gpu_process_cpu(gpu_pid)
                if usage is not None:
                    print(f"gpu.process_cpu_1s={usage:.1f}%")
            device_text = " ".join(
                str(device.get(key, "")) for key in ("vendorString", "deviceString"))
            accelerated = (
                aux.get("displayType") == "ANGLE_OPENGL"
                and features.get("gpu_compositing") in {"enabled", "enabled_on"}
                and features.get("opengl") in {"enabled", "enabled_on"}
                and features.get("rasterization") in {"enabled", "enabled_on"}
            )
            expected_matches = (
                args.expect_device_regex is None or
                re.search(args.expect_device_regex, device_text) is not None
            )
            if not accelerated or not expected_matches:
                reason = "hardware ANGLE/OpenGL path was not active"
                if accelerated and not expected_matches:
                    reason = f"GPU device did not match {args.expect_device_regex!r}"
                print(f"RESULT=FAIL: {reason}", file=sys.stderr)
                return 1
            print("RESULT=PASS: Electron is using hardware ANGLE/OpenGL")
            return 0
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
