#!/usr/bin/env python3
"""Exercise the first-party module's C ABI and ownership boundary."""

from __future__ import annotations

import ctypes
import os
import pathlib
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import time


MOD_START = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int
)
MOD_CONNECT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
MOD_EVENT = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_int,
    ctypes.c_long,
    ctypes.c_long,
    ctypes.c_long,
    ctypes.c_long,
)
MOD_SIGNAL = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
MOD_END = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
MOD_SET_PARAM = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p
)
MOD_SESSION_CHANGE = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int
)
MOD_GET_WAIT_OBJS = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_ssize_t),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_ssize_t),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
)
MOD_CHECK_WAIT_OBJS = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
MOD_FRAME_ACK = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int
)
MOD_SUPPRESS_OUTPUT = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
)


class MonitorInfo(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_int),
        ("top", ctypes.c_int),
        ("right", ctypes.c_int),
        ("bottom", ctypes.c_int),
        ("flags", ctypes.c_int),
        ("physical_width", ctypes.c_uint),
        ("physical_height", ctypes.c_uint),
        ("orientation", ctypes.c_uint),
        ("desktop_scale_factor", ctypes.c_uint),
        ("device_scale_factor", ctypes.c_uint),
        ("is_primary", ctypes.c_uint),
    ]


MOD_SERVER_MONITOR_RESIZE = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.POINTER(MonitorInfo),
    ctypes.POINTER(ctypes.c_int),
)
MOD_SERVER_MONITOR_FULL_INVALIDATE = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int
)
MOD_SERVER_VERSION_MESSAGE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)


class ModulePrefix(ctypes.Structure):
    """The module callback prefix needed by the lifecycle test."""

    _fields_ = [
        ("size", ctypes.c_int),
        ("version", ctypes.c_int),
        ("mod_start", MOD_START),
        ("mod_connect", MOD_CONNECT),
        ("mod_event", MOD_EVENT),
        ("mod_signal", MOD_SIGNAL),
        ("mod_end", MOD_END),
        ("mod_set_param", MOD_SET_PARAM),
        ("mod_session_change", MOD_SESSION_CHANGE),
        ("mod_get_wait_objs", MOD_GET_WAIT_OBJS),
        ("mod_check_wait_objs", MOD_CHECK_WAIT_OBJS),
        ("mod_frame_ack", MOD_FRAME_ACK),
        ("mod_suppress_output", MOD_SUPPRESS_OUTPUT),
        ("mod_server_monitor_resize", MOD_SERVER_MONITOR_RESIZE),
        ("mod_server_monitor_full_invalidate",
         MOD_SERVER_MONITOR_FULL_INVALIDATE),
        ("mod_server_version_message", MOD_SERVER_VERSION_MESSAGE),
    ]


def open_x11_pointer_query(display: str):
    """Return an Xlib-backed root-pointer query for the private Xvfb."""
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XQueryPointer.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_uint),
    ]
    x11.XQueryPointer.restype = ctypes.c_int
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    x11.XCloseDisplay.restype = ctypes.c_int

    connection = x11.XOpenDisplay(display.encode())
    if not connection:
        raise AssertionError(f"could not open X display {display}")
    root = x11.XDefaultRootWindow(connection)

    def query() -> tuple[int, int]:
        root_return = ctypes.c_ulong()
        child_return = ctypes.c_ulong()
        root_x = ctypes.c_int()
        root_y = ctypes.c_int()
        window_x = ctypes.c_int()
        window_y = ctypes.c_int()
        mask = ctypes.c_uint()
        if not x11.XQueryPointer(
            connection,
            root,
            ctypes.byref(root_return),
            ctypes.byref(child_return),
            ctypes.byref(root_x),
            ctypes.byref(root_y),
            ctypes.byref(window_x),
            ctypes.byref(window_y),
            ctypes.byref(mask),
        ):
            raise AssertionError("XQueryPointer failed")
        return root_x.value, root_y.value

    return x11, connection, query


def assert_pointer_event(module, handle, display: str, x: int, y: int,
                         expected: tuple[int, int]) -> None:
    # WM_MOUSEMOVE in xrdp_constants.h.  The module must inverse-map the RDP
    # presentation coordinate before sending XTest input to the physical root.
    assert module.mod_event(handle, 100, x, y, 0, 0) == 0
    time.sleep(0.05)
    x11, connection, query = open_x11_pointer_query(display)
    try:
        assert query() == expected
    finally:
        x11.XCloseDisplay(connection)


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


def choose_display_number() -> int:
    socket_directory = pathlib.Path("/tmp/.X11-unix")
    for display_number in range(90, 200):
        if (
            not pathlib.Path(f"/tmp/.X{display_number}-lock").exists()
            and not (socket_directory / f"X{display_number}").exists()
        ):
            return display_number
    raise AssertionError("could not find a free X display number")


def start_private_xvfb(
    auth_file: pathlib.Path,
) -> tuple[subprocess.Popen[object], str]:
    xvfb = shutil.which("Xvfb")
    if xvfb is None:
        raise AssertionError("module lifecycle test requires Xvfb")
    xauth = shutil.which("xauth")
    if xauth is None:
        raise AssertionError("module lifecycle test requires xauth")

    display_number = choose_display_number()
    display = f":{display_number}"
    cookie = secrets.token_hex(16)
    subprocess.run(
        [xauth, "-f", str(auth_file), "add", display, ".", cookie],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    auth_file.chmod(0o600)

    process = subprocess.Popen(
        [
            xvfb,
            display,
            "-auth",
            str(auth_file),
            "-screen",
            "0",
            "1024x768x24",
            "-nolisten",
            "tcp",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    xdpyinfo = shutil.which("xdpyinfo")
    if xdpyinfo is None:
        stop_process(process)
        raise AssertionError("module lifecycle test requires xdpyinfo")

    environment = os.environ.copy()
    environment["DISPLAY"] = display
    environment["XAUTHORITY"] = str(auth_file)
    deadline = time.monotonic() + 5.0
    authenticated = False
    while time.monotonic() < deadline:
        if process.poll() is not None:
            diagnostics = (
                process.stderr.read() if process.stderr is not None else ""
            )
            raise AssertionError(
                f"Xvfb exited with {process.returncode}: {diagnostics}"
            )
        if subprocess.run(
            [xdpyinfo],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode == 0:
            authenticated = True
            break
        time.sleep(0.05)

    if not authenticated:
        stop_process(process)
        raise AssertionError(f"Xvfb did not become ready on {display}")

    unauthenticated_environment = environment.copy()
    unauthenticated_environment["XAUTHORITY"] = str(
        auth_file.with_name("missing-xauthority")
    )
    if subprocess.run(
        [xdpyinfo],
        env=unauthenticated_environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode == 0:
        stop_process(process)
        raise AssertionError("Xvfb accepted a connection without Xauthority")

    return process, display


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} MODULE")

    auth_directory = tempfile.TemporaryDirectory(
        prefix="xrdp-console-xauthority-"
    )
    auth_file = pathlib.Path(auth_directory.name) / "xauthority"
    try:
        xvfb, display = start_private_xvfb(auth_file)
    except BaseException:
        auth_directory.cleanup()
        raise

    previous_display = os.environ.get("DISPLAY")
    previous_xauthority = os.environ.get("XAUTHORITY")
    os.environ["DISPLAY"] = display
    os.environ["XAUTHORITY"] = str(auth_file)
    handle = 0
    saved_stdin = os.dup(0)
    stdin_closed = False
    try:
        module_path = pathlib.Path(sys.argv[1])
        library = ctypes.CDLL(str(module_path))
        init = library.mod_init
        init.argtypes = []
        init.restype = ctypes.c_ssize_t
        exit_module = library.mod_exit
        exit_module.argtypes = [ctypes.c_ssize_t]
        exit_module.restype = ctypes.c_int

        # Force the first successful XCB connection to receive fd 0. The
        # connection layer must accept it and duplicate only the wait fd.
        os.close(0)
        stdin_closed = True
        handle = init()
        if handle == 0:
            raise AssertionError("mod_init returned a null handle")

        module = ctypes.cast(handle, ctypes.POINTER(ModulePrefix)).contents
        assert module.size >= ctypes.sizeof(ModulePrefix)
        assert module.version == 4

        assert module.mod_set_param(handle, b"hostname", b"lifecycle-test") == 0
        assert module.mod_set_param(handle, b"keylayout", b"0") == 0
        assert module.mod_set_param(handle, b"client_info", b"opaque") == 0
        assert module.mod_set_param(handle, b"display", b":65535") == 0
        assert module.mod_start(handle, 1024, 768, 32) == 0
        assert module.mod_connect(handle) == 1

        assert module.mod_set_param(handle, b"display", display.encode()) == 0
        assert module.mod_connect(handle) == 0
        assert os.readlink("/proc/self/fd/0").startswith("socket:")

        monitor = MonitorInfo(
            0, 0, 1511, 948, 0, 0, 0, 0, 100, 100, 1
        )
        in_progress = ctypes.c_int(99)
        assert (
            module.mod_server_monitor_resize(
                handle, 1512, 949, 1, ctypes.byref(monitor),
                ctypes.byref(in_progress),
            )
            == 0
        )
        assert in_progress.value == 0

        # The 1024x768 source fits into a 1512x949 presentation with a
        # centered 1265x949 viewport.  Verify that pointer input is mapped
        # through that viewport and never changes the physical X11 geometry.
        assert_pointer_event(handle=handle, module=module, display=display,
                             x=223, y=100, expected=(80, 80))
        assert module.mod_event(handle, 100, 0, 0, 0, 0) == 0
        time.sleep(0.05)
        x11, connection, query = open_x11_pointer_query(display)
        try:
            assert query() == (80, 80)
        finally:
            x11.XCloseDisplay(connection)

        assert (
            module.mod_server_monitor_full_invalidate(handle, 1512, 949)
            == 0
        )

        # An over-budget presentation is rejected without disturbing the
        # last valid transform/scaler configuration.
        in_progress.value = 99
        assert (
            module.mod_server_monitor_resize(
                handle, 8192, 8192, 1, ctypes.byref(monitor),
                ctypes.byref(in_progress),
            )
            == 1
        )
        assert in_progress.value == 0
        assert_pointer_event(handle=handle, module=module, display=display,
                             x=223, y=100, expected=(80, 80))

        # Resize storms are presentation-only changes.  They must not tear
        # down or reconnect the physical X11 transport, and every valid
        # layout must stay inside the scaler's bounded allocation policy.
        resize_sequence = ((1364, 768), (1728, 1117), (1512, 949),
                           (1366, 768))
        for _ in range(16):
            for width, height in resize_sequence:
                monitor.left = 0
                monitor.top = 0
                monitor.right = width - 1
                monitor.bottom = height - 1
                in_progress.value = 99
                assert (
                    module.mod_server_monitor_resize(
                        handle, width, height, 1, ctypes.byref(monitor),
                        ctypes.byref(in_progress),
                    )
                    == 0
                )
                assert in_progress.value == 0

        # The callback can be serviced without an RDP output callback table
        # installed; this exercises the full-invalidation state transition.
        assert module.mod_check_wait_objs(handle) == 0

        read_objs = (ctypes.c_ssize_t * 5)(11, 22, 33, 44, 0)
        write_objs = (ctypes.c_ssize_t * 3)(55, 66, 77)
        read_count = ctypes.c_int(3)
        write_count = ctypes.c_int(2)
        timeout = ctypes.c_int(17)
        assert (
            module.mod_get_wait_objs(
                handle,
                read_objs,
                ctypes.byref(read_count),
                write_objs,
                ctypes.byref(write_count),
                ctypes.byref(timeout),
            )
            == 0
        )
        assert read_count.value == 4
        assert write_count.value == 2
        assert timeout.value == 17
        assert list(read_objs[:3]) == [11, 22, 33]
        assert list(write_objs) == [55, 66, 77]
        assert read_objs[3] != 0
        assert module.mod_check_wait_objs(handle) == 0

        # Rebuilding the wait list over an already populated prefix must not
        # duplicate the X object.
        assert (
            module.mod_get_wait_objs(
                handle,
                read_objs,
                ctypes.byref(read_count),
                write_objs,
                ctypes.byref(write_count),
                ctypes.byref(timeout),
            )
            == 0
        )
        assert read_count.value == 4
        assert list(read_objs[:4]).count(read_objs[3]) == 1

        assert module.mod_end(handle) == 0

        # A second complete start/connect/end cycle must reuse no stale X11
        # state or wait object.
        assert module.mod_start(handle, 1024, 768, 32) == 0
        assert module.mod_connect(handle) == 0
        second_read_objs = (ctypes.c_ssize_t * 1)(0)
        second_read_count = ctypes.c_int(0)
        assert (
            module.mod_get_wait_objs(
                handle,
                second_read_objs,
                ctypes.byref(second_read_count),
                None,
                None,
                None,
            )
            == 0
        )
        assert second_read_count.value == 1
        assert second_read_objs[0] != 0
        assert module.mod_end(handle) == 0

        # The module must report a dead X connection to xrdp rather than
        # leaving the main loop permanently readable or hanging.
        assert module.mod_start(handle, 1024, 768, 32) == 0
        assert module.mod_connect(handle) == 0
        stop_process(xvfb)
        assert module.mod_check_wait_objs(handle) == 1
        assert module.mod_end(handle) == 0
    finally:
        try:
            if handle:
                assert exit_module(handle) == 0
        finally:
            if stdin_closed:
                os.dup2(saved_stdin, 0)
            os.close(saved_stdin)
            stop_process(xvfb)
            if previous_display is None:
                os.environ.pop("DISPLAY", None)
            else:
                os.environ["DISPLAY"] = previous_display
            if previous_xauthority is None:
                os.environ.pop("XAUTHORITY", None)
            else:
                os.environ["XAUTHORITY"] = previous_xauthority
            auth_directory.cleanup()

    assert exit_module(0) == 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
