#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Load and exercise the first-party module through the generated xrdp."""

from __future__ import annotations

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def stop_process(process: subprocess.Popen[object] | None) -> None:
    if process is None or process.poll() is not None:
        return

    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=3.0)


def port_is_listening(port: int) -> bool:
    wanted_port = f"{port:04X}"
    for proc_net in (Path("/proc/net/tcp"), Path("/proc/net/tcp6")):
        try:
            lines = proc_net.read_text(encoding="ascii").splitlines()[1:]
        except FileNotFoundError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) >= 4:
                local_port = fields[1].rsplit(":", 1)[-1]
                if local_port.upper() == wanted_port and fields[3] == "0A":
                    return True
    return False


def wait_for_listener(process: subprocess.Popen[object], port: int,
                      timeout: float, diagnostics: Path) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                "xrdp exited before listening "
                f"with status {process.returncode}:\n{read_text(diagnostics)}"
            )
        if port_is_listening(port):
            return
        time.sleep(0.05)
    raise AssertionError(
        f"xrdp did not listen on 127.0.0.1:{port}:\n{read_text(diagnostics)}"
    )


def wait_for_log(process: subprocess.Popen[object], log: Path, marker: str,
                 timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if marker in read_text(log):
            return
        if process.poll() is not None:
            break
        time.sleep(0.05)
    raise AssertionError(
        f"xrdp did not report {marker!r}:\n{read_text(log)}"
    )


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            f"usage: {sys.argv[0]} MODULE XRDP INSTALL_ROOT FREERDP"
        )

    module_path = Path(sys.argv[1]).resolve()
    xrdp_path = Path(sys.argv[2]).resolve()
    install_root = Path(sys.argv[3]).resolve()
    freerdp_path = Path(sys.argv[4]).resolve()
    for required in (module_path, xrdp_path, freerdp_path):
        if not required.is_file():
            raise AssertionError(f"missing smoke-test executable or module: {required}")

    xvfb_run = shutil.which("xvfb-run")
    if not os.environ.get("DISPLAY") and xvfb_run is None:
        raise AssertionError("FreeRDP smoke test needs DISPLAY or xvfb-run")

    with tempfile.TemporaryDirectory(prefix="xrdp-console-loader-") as temp:
        root = Path(temp)
        log_path = root / "xrdp.log"
        stdout_path = root / "xrdp-stdout.log"
        client_log_path = root / "freerdp.log"
        config_path = root / "xrdp.ini"
        port = free_tcp_port()

        module_dir = install_root / "lib" / "xrdp"
        module_dir.mkdir(parents=True, exist_ok=True)
        module_name = f"libxrdp_console_loader_{os.getpid()}.so"
        module_link = module_dir / module_name
        module_link.symlink_to(module_path)

        config_path.write_text(
            f"""[Globals]
ini_version=1
fork=true
port={port}
security_layer=negotiate
crypt_level=high
certificate={install_root / "etc" / "xrdp" / "cert.pem"}
key_file={install_root / "etc" / "xrdp" / "key.pem"}
bitmap_cache=false
bitmap_compression=false
bulk_compression=false
allow_channels=false
max_bpp=32
autorun=console

[Logging]
LogFile={log_path}
LogLevel=DEBUG
EnableSyslog=false
EnableConsole=false

[Channels]
rdpdr=false
rdpsnd=false
drdynvc=false
cliprdr=false
rail=false
xrdpvr=false

[console]
name=console
lib={module_name}
username=smoke
password=smoke
""",
            encoding="utf-8",
        )

        server: subprocess.Popen[object] | None = None
        client: subprocess.Popen[object] | None = None
        try:
            with stdout_path.open("w", encoding="utf-8") as server_stdout:
                server = subprocess.Popen(
                    [
                        str(xrdp_path),
                        "--nodaemon",
                        "--config",
                        str(config_path),
                    ],
                    cwd=root,
                    stdout=server_stdout,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                wait_for_listener(server, port, 8.0, stdout_path)

                client_command = [
                    str(freerdp_path),
                    f"/v:127.0.0.1:{port}",
                    "/u:smoke",
                    "/p:smoke",
                    "/cert:ignore",
                    "/size:1024x768",
                    "-gfx",
                    "-compression",
                    "/network:lan",
                    "/timeout:5000",
                    "/log-level:WARN",
                    "-clipboard",
                ]
                if not os.environ.get("DISPLAY"):
                    assert xvfb_run is not None
                    client_command = [
                        xvfb_run,
                        "-a",
                        "-s",
                        "-screen 0 1024x768x24",
                        *client_command,
                    ]
                with client_log_path.open("w", encoding="utf-8") as client_log:
                    client = subprocess.Popen(
                        client_command,
                        cwd=root,
                        stdout=client_log,
                        stderr=subprocess.STDOUT,
                        start_new_session=True,
                    )
                    marker = f"loaded module '{module_name}' ok"
                    wait_for_log(server, log_path, marker, 12.0)
                    wait_for_log(
                        server,
                        log_path,
                        "status from xrdp_mm_connect() : 0",
                        4.0,
                    )
                    time.sleep(0.25)
        finally:
            stop_process(client)
            stop_process(server)
            try:
                module_link.unlink()
            except FileNotFoundError:
                pass

        log = read_text(log_path)
        if "Failure setting up module" in log or "Error connecting to user session" in log:
            raise AssertionError(f"xrdp reported module setup failure:\n{log}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
