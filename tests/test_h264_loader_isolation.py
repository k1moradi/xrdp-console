#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline safety tests for the private cropped-edge H.264 RDP loader."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from h264_loader_isolation import (
    isolated_loader_module_name,
    private_client_display_is_safe,
    require_loopback_tcp_listener,
)


class LoaderIsolationTests(unittest.TestCase):
    def setUp(self):
        scratch = Path.cwd() / "test-artifacts" / "h264-loader-isolation-unit"
        scratch.mkdir(parents=True, exist_ok=True)
        self.dir = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.dir.cleanup)
        self.root = Path(self.dir.name)

    def test_module_link_resolves_to_private_workspace_without_prefix_writes(self):
        prefix = self.root / "pinned"
        pinned_modules = prefix / "lib" / "xrdp"
        pinned_modules.mkdir(parents=True)
        marker = pinned_modules / "keep-read-only.txt"
        marker.write_text("untouched", encoding="utf-8")
        private = self.root / "worktree" / "private-modules"
        private.mkdir(parents=True)
        module = private / "libxrdp_console_loader_test.so"
        module.write_bytes(b"synthetic module fixture, not a native ELF")
        spec = isolated_loader_module_name(prefix, module)
        self.assertTrue(spec.startswith("../"))
        self.assertEqual((pinned_modules / spec).resolve(), module.resolve())
        self.assertEqual(sorted(p.name for p in pinned_modules.iterdir()),
                         ["keep-read-only.txt"])
        self.assertEqual(marker.read_text(encoding="utf-8"), "untouched")

    def test_module_link_inside_pinned_prefix_is_rejected(self):
        prefix = self.root / "pinned"
        module_dir = prefix / "lib" / "xrdp"
        module_dir.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, "inside pinned"):
            isolated_loader_module_name(prefix, module_dir / "bad.so")
        alt = prefix / "other-modules"
        alt.mkdir()
        with self.assertRaisesRegex(ValueError, "inside pinned"):
            isolated_loader_module_name(prefix, alt / "bad.so")

    def test_oversized_module_path_is_rejected(self):
        prefix = self.root / "pinned"
        (prefix / "lib" / "xrdp").mkdir(parents=True)
        private = self.root / ("x" * 120) / ("y" * 120)
        private.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, "loader buffer"):
            isolated_loader_module_name(prefix, private / "module.so")

    def test_physical_or_parent_x11_display_is_rejected(self):
        auth = self.root / "private.xauthority"
        auth.touch()
        self.assertFalse(private_client_display_is_safe(":0", ":0", str(auth)))
        self.assertFalse(private_client_display_is_safe(":0.1", ":0", str(auth)))
        self.assertFalse(private_client_display_is_safe(":44.1", ":44", str(auth)))
        self.assertFalse(private_client_display_is_safe("", ":0", str(auth)))
        self.assertFalse(private_client_display_is_safe("localhost:44", ":0", str(auth)))
        self.assertFalse(private_client_display_is_safe(":94", ":0", None))
        self.assertTrue(private_client_display_is_safe(":94", ":0", str(auth)))

    def _write_proc_tables(self, addresses: list[tuple[str, str]]):
        proc_net = self.root / "net"
        proc_net.mkdir(exist_ok=True)
        header = "  sl  local_address rem_address st tx_queue"
        for family in ("tcp", "tcp6"):
            rows = [header]
            for index, (record_family, address) in enumerate(addresses):
                if record_family == family:
                    rows.append(
                        f"  {index}: {address}:A873 00000000:0000 0A 0:0")
            (proc_net / family).write_text("\n".join(rows) + "\n",
                                            encoding="ascii")
        return proc_net

    def test_exactly_one_loopback_listener_is_accepted(self):
        proc_net = self._write_proc_tables([("tcp", "0100007F")])
        require_loopback_tcp_listener(43123, proc_net)

    def test_wildcard_and_other_ipv4_interfaces_are_rejected(self):
        for address in ("00000000", "0201A8C0"):
            proc_net = self._write_proc_tables([("tcp", address)])
            with self.assertRaisesRegex(AssertionError, "exclusively"):
                require_loopback_tcp_listener(43123, proc_net)

    def test_dual_stack_or_extra_listeners_are_rejected(self):
        proc_net = self._write_proc_tables([
            ("tcp", "0100007F"),
            ("tcp6", "00000000000000000000000000000000"),
        ])
        with self.assertRaisesRegex(AssertionError, "exclusively"):
            require_loopback_tcp_listener(43123, proc_net)

    def test_absent_listener_or_wrong_port_is_rejected(self):
        proc_net = self._write_proc_tables([("tcp", "0100007F")])
        with self.assertRaisesRegex(AssertionError, "exclusively"):
            require_loopback_tcp_listener(43124, proc_net)
        with self.assertRaises(ValueError):
            require_loopback_tcp_listener(0, proc_net)


if __name__ == "__main__":
    unittest.main()
