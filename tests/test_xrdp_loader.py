#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Load and exercise the first-party module through the generated xrdp."""

from __future__ import annotations

import os
import re
import select
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
                 timeout: float, diagnostics: Path | None = None) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if marker in read_text(log):
            return
        if process.poll() is not None:
            break
        time.sleep(0.05)
    details = read_text(log)
    if diagnostics is not None:
        details += f"\n[xrdp stdout]\n{read_text(diagnostics)}"
    raise AssertionError(f"xrdp did not report {marker!r}:\n{details}")


def read_line(stream, timeout: float) -> bytes:
    ready, _, _ = select.select([stream], [], [], timeout)
    return stream.readline() if ready else b""


def start_source_xvfb(log_path: Path) -> tuple[subprocess.Popen[bytes], str]:
    executable = shutil.which("Xvfb")
    if executable is None:
        raise AssertionError("xrdp loader smoke test needs Xvfb")

    with log_path.open("w", encoding="utf-8") as log_file:
        process = subprocess.Popen(
            [executable, "-displayfd", "1", "-screen", "0", "1024x768x24",
             "-nolisten", "tcp", "-noreset"],
            stdout=subprocess.PIPE,
            stderr=log_file,
            start_new_session=True,
        )
    if process.stdout is None:
        stop_process(process)
        raise AssertionError("source Xvfb display-number pipe was not created")
    display_number = read_line(process.stdout, 5.0).strip()
    if not display_number.isdigit():
        details = read_text(log_path)
        stop_process(process)
        raise AssertionError(
            f"source Xvfb did not allocate a display: {details}")
    display = ":" + display_number.decode("ascii")

    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["xdpyinfo", "-display", display],
            capture_output=True,
            check=False,
            timeout=2.0,
        )
        if result.returncode == 0:
            return process, display
        if process.poll() is not None:
            break
        time.sleep(0.05)

    details = read_text(log_path)
    stop_process(process)
    raise AssertionError(f"source Xvfb display {display} did not become ready:\n{details}")


def find_window(display: str, title: str, timeout: float) -> str:
    pattern = re.compile(
        r"^\s*(0x[0-9a-fA-F]+) \"" + re.escape(title) + r"\"",
        re.MULTILINE,
    )
    deadline = time.monotonic() + timeout
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
        time.sleep(0.05)
    raise AssertionError(
        f"FreeRDP window {title!r} did not appear:\n{last_tree}"
    )


def start_stimulus(stimulus_path: Path, display: str,
                   environment: dict[str, str]) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(
        [str(stimulus_path), display], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment,
        bufsize=0, start_new_session=True,
    )
    if process.stdin is None or process.stdout is None:
        stop_process(process)
        raise AssertionError("source stimulus pipes were not created")
    ready = read_line(process.stdout, 5.0)
    if ready != b"READY 160 100\n":
        stop_process(process)
        details = process.stderr.read().decode(errors="replace") if process.stderr else ""
        raise AssertionError(f"source stimulus did not become ready: {ready!r} {details}")
    return process


def assert_client_pixel(client_display: str,
                        stimulus: subprocess.Popen[bytes],
                        window_title: str, pixel_probe: Path,
                        log_path: Path, stdout_path: Path,
                        probe_x: int, probe_y: int,
                        assert_sparse_planar_batch: bool = False) -> None:
    """Draw a known source color and require it in the FreeRDP framebuffer."""
    window = find_window(client_display, window_title, 8.0)
    probe: subprocess.Popen[object] | None = None
    try:
        probe = subprocess.Popen(
            [str(pixel_probe), client_display, window,
             str(probe_x), str(probe_y)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=os.environ.copy(), bufsize=0,
            start_new_session=True,
        )
        if (stimulus.stdin is None or stimulus.stdout is None or
                probe.stdin is None or probe.stdout is None):
            raise AssertionError("pixel assertion pipes were not created")
        if not read_line(probe.stdout, 5.0).startswith(b"READY "):
            raise AssertionError("pixel probe did not become ready")

        batch_lines = [
            line for line in read_text(log_path).splitlines()
            if "XRDP_CONSOLE_GFX_PLANAR_BATCH_V1 frame=" in line
        ]
        prior_batch_numbers = [
            int(value) for line in batch_lines
            if (match := re.search(r"\bframe=(\d+)", line)) is not None
            for value in [match.group(1)]
        ]
        prior_batch_number = max(prior_batch_numbers, default=0)

        stimulus.stdin.write(b"frame\n")
        stimulus.stdin.flush()
        source_line = read_line(stimulus.stdout, 5.0)
        fields = source_line.split()
        if len(fields) < 3:
            raise AssertionError(f"source stimulus did not draw a frame: {source_line!r}")
        state = int(fields[2])
        expected_red = state == 0

        def wait_for_pixel(expected_red: bool) -> bytes:
            deadline = time.monotonic() + 8.0
            last_pixel = b""
            while time.monotonic() < deadline:
                try:
                    probe.stdin.write(b"sample\n")
                    probe.stdin.flush()
                except BrokenPipeError as error:
                    raise AssertionError(
                        "pixel probe exited before the client-visible pixel "
                        f"assertion completed (status={probe.poll()}):\n"
                        f"{xrdp_log_excerpt(log_path)}"
                    ) from error
                line = read_line(
                    probe.stdout, min(0.5, deadline - time.monotonic()))
                if not line:
                    continue
                last_pixel = line
                pixel = line.split()
                if len(pixel) < 4:
                    continue
                red, green, blue = (int(value) for value in pixel[1:4])
                matches = (
                    red > 200 and green < 80 and blue < 80
                    if expected_red else
                    blue > 200 and red < 80 and green < 80
                )
                if matches:
                    return last_pixel
            raise AssertionError(
                "known source pixel did not reach the FreeRDP framebuffer: "
                f"last={last_pixel!r}\n{xrdp_log_excerpt(log_path)}"
            )

        wait_for_pixel(expected_red)
        if assert_sparse_planar_batch:
            # A client-visible pixel may precede the next xrdp GFX dirty
            # flush. Wait for the baseline draw's own completed Planar batch,
            # not merely an older pending=0 record, before drawing the sparse
            # pair.
            idle_deadline = time.monotonic() + 5.0
            last_idle_summary = ""
            idle_since = 0.0
            while time.monotonic() < idle_deadline:
                summaries = [
                    line for line in read_text(log_path).splitlines()
                    if "XRDP_CONSOLE_GFX_PLANAR_BATCH_V1 frame=" in line
                ]
                latest_summary = summaries[-1] if summaries else ""
                frame_match = re.search(r"\bframe=(\d+)", latest_summary)
                has_new_idle_frame = (
                    frame_match is not None and
                    int(frame_match.group(1)) > prior_batch_number and
                    latest_summary.endswith("pending=0")
                )
                if has_new_idle_frame:
                    if latest_summary != last_idle_summary:
                        last_idle_summary = latest_summary
                        idle_since = time.monotonic()
                    elif time.monotonic() - idle_since >= 0.10:
                        break
                else:
                    last_idle_summary = ""
                    idle_since = 0.0
                time.sleep(0.01)
            else:
                raise AssertionError(
                    "initial full-screen Planar damage did not drain before "
                    f"the sparse test:\n{xrdp_log_excerpt(log_path)}")

            try:
                stimulus.stdin.write(b"sparse\n")
                stimulus.stdin.flush()
            except BrokenPipeError as error:
                raise AssertionError("source stimulus exited before sparse draw") from error
            if read_line(stimulus.stdout, 5.0) != b"SPARSE_DONE\n":
                raise AssertionError("source stimulus did not complete sparse draw")
            wait_for_pixel(expected_red=False)

            deadline = time.monotonic() + 5.0
            batch_pattern = re.compile(
                r"XRDP_CONSOLE_GFX_PLANAR_BATCH_V1 .*starts=1 ends=1 "
                r"rects=2 tiles=2 pixels=800 pending=0")
            while time.monotonic() < deadline:
                if batch_pattern.search(read_text(log_path)) is not None:
                    break
                time.sleep(0.05)
            else:
                summaries = "\n".join(
                    line for line in read_text(log_path).splitlines()
                    if "XRDP_CONSOLE_GFX_PLANAR_BATCH_V1" in line)
                damage_debug = "\n".join(
                    line for line in read_text(stdout_path).splitlines()
                    if "DAMAGE_" in line)
                raise AssertionError(
                    "two sparse 20x20 updates were not emitted as one exact "
                    "Planar frame (expected starts=1 ends=1 rects=2 "
                    f"tiles=2 pixels=800 pending=0):\n{summaries}\n"
                    f"{damage_debug}\n"
                    f"{xrdp_log_excerpt(log_path)}")
    finally:
        stop_process(probe)


def presentation_probe_point(width: int, height: int) -> tuple[int, int]:
    """Map the stimulus pixel through the loader's aspect-fit transform."""
    source_width = 1024
    source_height = 768
    source_x = 30
    source_y = 30
    if width * source_height <= height * source_width:
        viewport_width = width
        viewport_height = max(1, width * source_height // source_width)
    else:
        viewport_height = height
        viewport_width = max(1, height * source_width // source_height)
    viewport_x = (width - viewport_width) // 2
    viewport_y = (height - viewport_height) // 2
    return (
        viewport_x + source_x * viewport_width // source_width,
        viewport_y + source_y * viewport_height // source_height,
    )


def xrdp_log_excerpt(path: Path) -> str:
    text = read_text(path)
    return text[-12000:]


def display_is_usable() -> bool:
    display = os.environ.get("DISPLAY")
    if not display:
        return False
    xdpyinfo = shutil.which("xdpyinfo")
    if xdpyinfo is None:
        return True
    try:
        result = subprocess.run(
            [xdpyinfo],
            capture_output=True,
            check=False,
            timeout=3.0,
            text=True,
        )
        if result.returncode != 0:
            return False
        return any(
            line.strip().startswith("dimensions:") and
            line.split()[1] == "1024x768"
            for line in result.stdout.splitlines()
            if len(line.split()) > 1
        )
    except (OSError, subprocess.TimeoutExpired):
        return False


def ensure_test_display() -> None:
    if display_is_usable():
        return

    xvfb_run = shutil.which("xvfb-run")
    if xvfb_run is None:
        raise AssertionError("xrdp loader smoke test needs DISPLAY or xvfb-run")

    environment = os.environ.copy()
    os.execvpe(
        xvfb_run,
        [
            xvfb_run,
            "-a",
            "-s",
            "-screen 0 1024x768x24",
            sys.executable,
            "-B",
            *sys.argv,
        ],
        environment,
    )


def main() -> int:
    arguments = list(sys.argv[1:])
    rfx_mode = False
    gfx_planar_mode = False
    mode_options = [option for option in ("--rfx", "--gfx-planar")
                    if option in arguments]
    if mode_options:
        if (len(mode_options) != 1 or arguments[-1] != mode_options[0] or
                arguments.count(mode_options[0]) != 1):
            raise SystemExit(
                "--rfx or --gfx-planar must be the final, sole loader-smoke option")
        arguments.pop()
        rfx_mode = mode_options[0] == "--rfx"
        gfx_planar_mode = mode_options[0] == "--gfx-planar"

    if len(arguments) not in (6, 8):
        raise SystemExit(
            f"usage: {sys.argv[0]} MODULE XRDP INSTALL_ROOT FREERDP "
            "PIXEL_PROBE STIMULUS [PRESENTATION_WIDTH PRESENTATION_HEIGHT] "
            "[--rfx|--gfx-planar]"
        )

    ensure_test_display()

    module_path = Path(arguments[0]).resolve()
    xrdp_path = Path(arguments[1]).resolve()
    install_root = Path(arguments[2]).resolve()
    freerdp_path = Path(arguments[3]).resolve()
    pixel_probe = Path(arguments[4]).resolve()
    stimulus_path = Path(arguments[5]).resolve()
    presentation_width = 1024
    presentation_height = 768
    if len(arguments) == 8:
        try:
            presentation_width = int(arguments[6])
            presentation_height = int(arguments[7])
        except ValueError as error:
            raise AssertionError("presentation geometry must be numeric") from error
        if presentation_width <= 0 or presentation_height <= 0:
            raise AssertionError("presentation geometry must be positive")
    probe_x, probe_y = presentation_probe_point(
        presentation_width, presentation_height)
    window_title = f"xrdp-console-loader-{os.getpid()}"
    for required in (
            module_path, xrdp_path, freerdp_path, pixel_probe, stimulus_path):
        if not required.is_file():
            raise AssertionError(f"missing smoke-test executable or module: {required}")

    with tempfile.TemporaryDirectory(prefix="xrdp-console-loader-") as temp:
        root = Path(temp)
        log_path = root / "xrdp.log"
        stdout_path = root / "xrdp-stdout.log"
        client_log_path = root / "freerdp.log"
        source_xvfb_log_path = root / "source-xvfb.log"
        config_path = root / "xrdp.ini"
        port = free_tcp_port()
        source_xvfb, source_display = start_source_xvfb(source_xvfb_log_path)

        module_dir = install_root / "lib" / "xrdp"
        module_dir.mkdir(parents=True, exist_ok=True)
        module_name = f"libxrdp_console_loader_{os.getpid()}.so"
        module_link = module_dir / module_name
        module_link.symlink_to(module_path)
        fastpath_option = "use_fastpath=both\n" if rfx_mode else ""
        drdynvc_enabled = "true" if gfx_planar_mode else "false"

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
allow_channels=true
max_bpp=32
{fastpath_option}autorun=console

[Logging]
LogFile={log_path}
LogLevel=DEBUG
EnableSyslog=false
EnableConsole=false

[Channels]
rdpdr=false
rdpsnd=false
drdynvc={drdynvc_enabled}
cliprdr=true
rail=false
xrdpvr=false

[console]
name=console
lib={module_name}
# First-party physical-console capability: complete pixels plus smooth scroll.
code=21
display={source_display}
username=smoke
password=smoke
""",
            encoding="utf-8",
        )

        server: subprocess.Popen[object] | None = None
        client: subprocess.Popen[object] | None = None
        stimulus: subprocess.Popen[bytes] | None = None
        try:
            # Create/map the source window before the module connects and
            # installs root XDamage. This excludes map/expose churn from the
            # sparse-rectangle acceptance assertion.
            stimulus = start_stimulus(
                stimulus_path, source_display, os.environ.copy())
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
                    f"/size:{presentation_width}x{presentation_height}",
                    f"/t:{window_title}",
                    "-compression",
                    "/network:lan",
                    "/timeout:5000",
                    "/log-level:WARN",
                    "-clipboard",
                ]
                if rfx_mode:
                    # RemoteFX bitmap codec with the modern graphics pipeline
                    # disabled. The module's capability gate requires gfx=0.
                    client_command.extend(["+rfx", "-gfx"])
                elif gfx_planar_mode:
                    client_command.append("/gfx")
                else:
                    client_command.append("-gfx")
                with client_log_path.open("w", encoding="utf-8") as client_log:
                    client = subprocess.Popen(
                        client_command,
                        cwd=root,
                        stdout=client_log,
                        stderr=subprocess.STDOUT,
                        start_new_session=True,
                    )
                    marker = f"loaded module '{module_name}' ok"
                    wait_for_log(server, log_path, marker, 12.0, stdout_path)
                    wait_for_log(
                        server,
                        log_path,
                        "xrdp-console: build revision=",
                        4.0,
                        stdout_path,
                    )
                    if re.search(
                            r"xrdp-console: build revision="
                            r"[0-9a-fA-F]{12}(?:-dirty)?\b",
                            read_text(log_path)) is None:
                        raise AssertionError(
                            "module startup did not report a concrete build revision\n"
                            f"{xrdp_log_excerpt(log_path)}")
                    wait_for_log(
                        server,
                        log_path,
                        "status from xrdp_mm_connect() : 0",
                        4.0,
                        stdout_path,
                    )
                    if rfx_mode:
                        wait_for_log(
                            server,
                            log_path,
                            "actual_output=standard-rfx",
                            4.0,
                            stdout_path,
                        )
                    if gfx_planar_mode:
                        wait_for_log(
                            server,
                            log_path,
                            "actual_output=gfx-planar",
                            4.0,
                            stdout_path,
                        )
                    assert_client_pixel(
                        os.environ["DISPLAY"], stimulus, window_title,
                        pixel_probe, log_path, stdout_path,
                        probe_x, probe_y,
                        assert_sparse_planar_batch=gfx_planar_mode)
        finally:
            stop_process(client)
            stop_process(server)
            stop_process(stimulus)
            stop_process(source_xvfb)
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
