#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure safety contracts for the isolated cropped-edge xrdp loader case.

These functions never open an X11 connection, start an RDP listener, write to
the pinned xrdp install prefix, or inspect private clipboard data.
"""
from __future__ import annotations

import os
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
