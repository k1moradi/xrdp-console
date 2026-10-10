#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure safety contracts for the isolated cropped-edge xrdp loader case.

These functions never open an X11 connection, start an RDP listener, write to
the pinned xrdp install prefix, or inspect private clipboard data.
"""
from __future__ import annotations

import os
import secrets
import struct
from pathlib import Path

LOOPBACK_IPV4_LISTENER = "0100007F"
XRDP_MODULE_LOAD_PATH_LIMIT = 255  # xrdp_mm_setup_mod1's C module-path buffer.


def isolated_loader_module_name(
        install_root: Path, private_module_link: Path) -> str:
    """Return an xrdp.ini lib path resolving into a private runtime directory.

    The pinned xrdp binary resolves lib from its compiled XRDP_MODULE_PATH,
    usually <install_root>/lib/xrdp. A relative lib path with '..' reaches a
    disposable symlink without writing inside that read-only install tree.
    The joined path must fit the fixed-size xrdp module-loader buffer.
    """
    root = install_root.resolve(strict=True)
    module_root = (root / "lib" / "xrdp").resolve(strict=True)
    private_parent = private_module_link.parent.resolve(strict=True)
    if private_parent == module_root or private_parent.is_relative_to(root):
        raise ValueError("private module link is inside pinned xrdp install root")
    relative = os.path.relpath(private_parent / private_module_link.name,
                               start=module_root)
    expanded = str(module_root / relative)
    if len(expanded.encode("utf-8")) >= XRDP_MODULE_LOAD_PATH_LIMIT:
        raise ValueError("private module path exceeds pinned xrdp loader buffer")
    if not relative.startswith("../"):
        raise ValueError("private module path does not leave the pinned module directory")
    return relative


def private_client_display_is_safe(
        display: str | None, parent_display: str | None,
        xauthority: str | None) -> bool:
    """Validate an xvfb-run child's display without connecting to X11."""
    if not display or not display.startswith(":"):
        return False
    if display.split(".", 1)[0] == ":0":
        return False
    if parent_display and display.split(".", 1)[0] == parent_display.split(".", 1)[0]:
        return False
    return bool(xauthority and Path(xauthority).is_file())


def require_loopback_tcp_listener(
        port: int, proc_net: Path = Path("/proc/net")) -> None:
    """Refuse an RDP listener visible on any non-loopback interface.

    Linux /proc/net/tcp encodes 127.0.0.1 as little-endian 0100007F.
    IPv6 is intentionally rejected: this test needs precisely one IPv4-only
    listener to avoid dual-stack/wildcard surprises.
    """
    if not 1 <= port <= 65535:
        raise ValueError("test TCP port is outside the valid range")
    matches = []
    for family in ("tcp", "tcp6"):
        lines = (proc_net / family).read_text(encoding="ascii").splitlines()[1:]
        for line in lines:
            fields = line.split()
            if len(fields) < 4 or fields[3] != "0A":
                continue
            address, separator, port_hex = fields[1].rpartition(":")
            if not separator:
                continue
            if int(port_hex, 16) != port:
                continue
            matches.append((family, address.upper()))
    if matches != [("tcp", LOOPBACK_IPV4_LISTENER)]:
        raise AssertionError(
            "xrdp test listener is not exclusively bound to IPv4 "
            f"127.0.0.1:{port}: {matches!r}")


# Session integrations (D-Bus, Wayland, PulseAudio, PipeWire and the agent)
# must never escape a test-only client or server into the physical desktop.
# Keep DISPLAY and XAUTHORITY unchanged: xvfb-run owns the private X11 auth.
_SESSION_ENVIRONMENT_KEYS = (
    "DBUS_SESSION_BUS_ADDRESS",
    "WAYLAND_DISPLAY",
    "PULSE_SERVER",
    "PIPEWIRE_REMOTE",
    "SSH_AUTH_SOCK",
    "GPG_AGENT_INFO",
    "SESSION_MANAGER",
    "DESKTOP_STARTUP_ID",
)


def isolated_desktop_environment(
        inherited: dict[str, str], scratch: Path,
        display: str) -> dict[str, str]:
    """Create private child-session paths without changing the parent env.

    The caller supplies a disposable, already-created test root. Fail closed
    for symlinks, so existing host XDG state cannot be reached by accident.
    """
    if scratch.is_symlink():
        raise ValueError("isolated desktop root may not be a symlink")
    root = scratch.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("isolated desktop root is not a directory")
    env = inherited.copy()
    for key in _SESSION_ENVIRONMENT_KEYS:
        env.pop(key, None)
    for name, relative in (
            ("XDG_RUNTIME_DIR", "runtime"),
            ("XDG_CACHE_HOME", "cache"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_DATA_HOME", "data")):
        child = root / relative
        if child.is_symlink():
            raise ValueError(f"isolated {name} path is a symlink")
        child.mkdir(mode=0o700, exist_ok=True)
        if child.resolve(strict=True).parent != root:
            raise ValueError(f"isolated {name} escapes its test root")
        child.chmod(0o700)
        env[name] = str(child)
    env["HOME"] = str(root)
    env["DISPLAY"] = display
    return env

# A FamilyWild MIT-MAGIC-COOKIE-1 record works with Xvfb -displayfd, where
# the server display number is allocated only after the -auth file is opened.
# The file is private to this synthetic source server, never shared with the
# FreeRDP client Xvfb or the physical user's Xauthority.
def create_private_source_xauthority(path: Path) -> None:
    """Create an exclusive 0600 Xauthority file before launching source Xvfb."""
    if path.parent.is_symlink() or not path.parent.is_dir():
        raise ValueError("source Xauthority parent is not a private directory")
    cookie = secrets.token_bytes(16)

    def field(value: bytes) -> bytes:
        return struct.pack("!H", len(value)) + value

    record = (
        struct.pack("!H", 0xFFFF) +  # Xauthority FamilyWild
        field(b"") +              # address: any local Xvfb display
        field(b"") +              # number: allocated by -displayfd
        field(b"MIT-MAGIC-COOKIE-1") +
        field(cookie)
    )
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                         getattr(os, "O_NOFOLLOW", 0), 0o600)
    with os.fdopen(descriptor, "wb") as file:
        file.write(record)


def require_unoccupied_pinned_xrdp_pidfile(install_root: Path) -> None:
    """Fail closed before private Xvfb if the pinned xrdp PID path exists.

    Pinned xrdp 0.10.6.1 defines XRDP_PID_PATH as localstatedir/run.
    The private loader dependency is configured with localstatedir =
    INSTALL_DIR/var. xrdp main() checks this file even under --nodaemon
    and will refuse startup if the referenced PID is active. Never remove
    or modify it: it could be owned by the deployed service.
    """
    root = install_root.resolve(strict=True)
    run_dir = root / "var" / "run"
    pid_file = run_dir / "xrdp.pid"
    if run_dir.is_symlink() or pid_file.is_symlink():
        raise AssertionError("pinned xrdp PID namespace uses a symlink")
    if pid_file.exists():
        raise AssertionError(
            "pinned xrdp PID file already exists; cannot prove an "
            "independent private listener from this binary. "
            "Do not delete the file or stop the live service.")
