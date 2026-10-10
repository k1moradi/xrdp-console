#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline safety tests for the private cropped-edge H.264 RDP loader."""
from __future__ import annotations

import ast
import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import test_xrdp_loader as loader

from h264_loader_isolation import (
    create_private_source_xauthority,
    isolated_desktop_environment,
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

    def test_source_cookie_is_exclusive_private_and_distinct_from_client(self):
        source = self.root / "source-xvfb.xauthority"
        other = self.root / "second-source.xauthority"
        client = self.root / "client-xvfb.xauthority"
        client.write_bytes(b"synthetic distinct client Xauthority")
        create_private_source_xauthority(source)
        create_private_source_xauthority(other)
        self.assertEqual(source.stat().st_mode & 0o777, 0o600)
        first = source.read_bytes()
        second = other.read_bytes()
        self.assertNotEqual(first, second)
        self.assertNotEqual(first, client.read_bytes())
        self.assertEqual(struct.unpack("!H", first[:2])[0], 0xffff)
        offsets = 2
        fields = []
        for _ in range(4):
            size = struct.unpack("!H", first[offsets:offsets + 2])[0]
            offsets += 2
            fields.append(first[offsets:offsets + size])
            offsets += size
        self.assertEqual(fields[:3], [b"", b"", b"MIT-MAGIC-COOKIE-1"])
        self.assertEqual(len(fields[3]), 16)
        self.assertEqual(offsets, len(first))
        with self.assertRaises(FileExistsError):
            create_private_source_xauthority(source)
        self.assertEqual(source.read_bytes(), first)

    def test_source_xvfb_uses_cookie_in_server_and_probe(self):
        auth = self.root / "source.xauthority"
        log = self.root / "source.log"
        fake_process = mock.Mock()
        fake_process.stdout = object()
        with (mock.patch.object(loader.shutil, "which",
                                return_value="/usr/bin/Xvfb"),
              mock.patch.object(loader.subprocess, "Popen",
                                return_value=fake_process) as popen,
              mock.patch.object(loader, "read_line", return_value=b"94\n"),
              mock.patch.object(loader.subprocess, "run",
                                return_value=mock.Mock(returncode=0)) as run):
            process, display = loader.start_source_display(
                log, 1366, 768, auth_file=auth)
        self.assertIs(process, fake_process)
        self.assertEqual(display, ":94")
        args = popen.call_args.args[0]
        self.assertEqual(args[-2:], ["-auth", str(auth)])
        self.assertIn("-nolisten", args)
        self.assertTrue(auth.is_file())
        probe = run.call_args
        self.assertEqual(probe.kwargs["env"]["DISPLAY"], ":94")
        self.assertEqual(probe.kwargs["env"]["XAUTHORITY"], str(auth))

    def test_source_xvfb_rejects_display_zero_and_stops_process(self):
        auth = self.root / "source.xauthority"
        log = self.root / "source.log"
        fake_process = mock.Mock()
        fake_process.stdout = object()
        with (mock.patch.object(loader.shutil, "which",
                                return_value="/usr/bin/Xvfb"),
              mock.patch.object(loader.subprocess, "Popen",
                                return_value=fake_process),
              mock.patch.object(loader, "read_line", return_value=b"0\n"),
              mock.patch.object(loader, "stop_process") as stop):
            with self.assertRaisesRegex(AssertionError, "physical DISPLAY"):
                loader.start_source_display(log, 1366, 768, auth_file=auth)
        stop.assert_called_once_with(fake_process)

    def test_source_xvfb_stops_process_on_displayfd_timeout(self):
        auth = self.root / "source.xauthority"
        log = self.root / "source.log"
        fake_process = mock.Mock()
        fake_process.stdout = object()
        with (mock.patch.object(loader.shutil, "which",
                                return_value="/usr/bin/Xvfb"),
              mock.patch.object(loader.subprocess, "Popen",
                                return_value=fake_process),
              mock.patch.object(loader, "read_line", side_effect=TimeoutError),
              mock.patch.object(loader, "stop_process") as stop):
            with self.assertRaises(TimeoutError):
                loader.start_source_display(log, 1366, 768, auth_file=auth)
        stop.assert_called_once_with(fake_process)

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

    def test_loader_forces_new_xvfb_even_when_physical_display_is_usable(self):
        # Do not actually start Xvfb or inspect the real user's display.
        auth = self.root / "old-xauthority"
        auth.touch()
        scratch = self.root / "private-client"
        class ReplacedExec(Exception):
            pass
        with (mock.patch.dict(os.environ, {
                "DISPLAY": ":0", "XAUTHORITY": str(auth),
                "XRDP_CONSOLE_TEST_PRIVATE_CLIENT_XVFB": ""}),
              mock.patch.object(loader.shutil, "which",
                                return_value="/usr/bin/xvfb-run"),
              mock.patch.object(loader, "display_is_usable",
                                return_value=True),
              mock.patch.object(loader.os, "execvpe",
                                side_effect=ReplacedExec) as exec_call):
            with self.assertRaises(ReplacedExec):
                loader.ensure_test_display(
                    1364, 768, force_private_client=True, scratch_root=scratch)
        self.assertEqual(exec_call.call_count, 1)
        command, args, child_env = exec_call.call_args.args
        self.assertEqual(command, "/usr/bin/xvfb-run")
        self.assertEqual(args[0:2], ["/usr/bin/xvfb-run", "-a"])
        self.assertEqual(child_env["XRDP_CONSOLE_TEST_PARENT_DISPLAY"], ":0")
        self.assertEqual(child_env["XRDP_CONSOLE_TEST_PRIVATE_CLIENT_XVFB"], "1")
        self.assertEqual(child_env["XRDP_CONSOLE_TEST_XVFB_WRAPPER_PID"],
                         str(os.getpid()))
        self.assertEqual(child_env["TMPDIR"], str(scratch))
        self.assertNotIn("DISPLAY", child_env)
        self.assertNotIn("XAUTHORITY", child_env)
        self.assertTrue(scratch.is_dir())

    def test_loader_rejects_inherited_parent_display_in_child_mode(self):
        auth = self.root / "inherited-xauthority"
        auth.touch()
        with (mock.patch.dict(os.environ, {
                "DISPLAY": ":0", "XAUTHORITY": str(auth),
                "XRDP_CONSOLE_TEST_PARENT_DISPLAY": ":0",
                "XRDP_CONSOLE_TEST_PRIVATE_CLIENT_XVFB": "1"}),
              mock.patch.object(loader, "display_is_usable",
                                return_value=True)):
            with self.assertRaisesRegex(AssertionError, "private RDP client"):
                loader.ensure_test_display(
                    1364, 768, force_private_client=True,
                    scratch_root=self.root / "private-client")

    def test_generated_loader_ini_lines_are_not_indented(self):
        # The ini template is embedded in a source-level try block.
        # An indentation-only Python refactor once broke every directive.
        source = Path(loader.__file__).read_text(encoding="utf-8")
        syntax = ast.parse(source)
        generated = []
        for node in ast.walk(syntax):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            if (isinstance(func, ast.Attribute) and func.attr == "write_text" and
                    isinstance(func.value, ast.Name) and
                    func.value.id == "config_path" and node.args and
                    isinstance(node.args[0], ast.JoinedStr)):
                joined = node.args[0]
                generated.append("".join(
                    part.value if isinstance(part, ast.Constant)
                    else "<dynamic>"
                    for part in joined.values))
        self.assertEqual(len(generated), 1)
        template = generated[0]
        self.assertIn("[Globals]\nini_version=1\nfork=true\n", template)
        self.assertIn("port=tcp://127.0.0.1:<dynamic>", template)
        self.assertIn("[console]\nname=console\nlib=<dynamic>", template)
        for line in template.splitlines():
            if line.strip() and line != "<dynamic>":
                self.assertFalse(line[0].isspace(), repr(line))

    def test_private_client_marker_without_real_wrapper_parent_is_rejected(self):
        auth = self.root / "private-xauthority"
        auth.touch()
        with (mock.patch.dict(os.environ, {
                "DISPLAY": ":94", "XAUTHORITY": str(auth),
                "XRDP_CONSOLE_TEST_PARENT_DISPLAY": ":0",
                "XRDP_CONSOLE_TEST_PRIVATE_CLIENT_XVFB": "1",
                "XRDP_CONSOLE_TEST_XVFB_WRAPPER_PID": "1234"}),
              mock.patch.object(loader.os, "getppid", return_value=4567),
              mock.patch.object(loader, "display_is_usable",
                                return_value=True)):
            with self.assertRaisesRegex(AssertionError, "not owned by xvfb-run"):
                loader.ensure_test_display(1364, 768, force_private_client=True)

    def test_private_client_wrapper_parent_accepts_real_private_display(self):
        auth = self.root / "private-xauthority"
        auth.touch()
        with (mock.patch.dict(os.environ, {
                "DISPLAY": ":94", "XAUTHORITY": str(auth),
                "XRDP_CONSOLE_TEST_PARENT_DISPLAY": ":0",
                "XRDP_CONSOLE_TEST_PRIVATE_CLIENT_XVFB": "1",
                "XRDP_CONSOLE_TEST_XVFB_WRAPPER_PID": "1234"}),
              mock.patch.object(loader.os, "getppid", return_value=1234),
              mock.patch.object(loader, "display_is_usable",
                                return_value=True)):
            loader.ensure_test_display(1364, 768, force_private_client=True)

    def test_isolated_desktop_environment_drops_host_session_integrations(self):
        inherited = {
            "DISPLAY": ":0",
            "XAUTHORITY": str(self.root / "private-xauthority"),
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
            "XDG_RUNTIME_DIR": "/run/user/1000",
            "WAYLAND_DISPLAY": "wayland-0",
            "PULSE_SERVER": "unix:/run/user/1000/pulse/native",
            "PIPEWIRE_REMOTE": "pipewire-0",
            "SSH_AUTH_SOCK": "/run/user/1000/ssh-agent",
            "SESSION_MANAGER": "local/host:12345",
            "HOME": "/home/real-user",
            "KEEP_THIS": "still-here",
        }
        env = isolated_desktop_environment(inherited, self.root, ":94")
        self.assertEqual(env["DISPLAY"], ":94")
        self.assertEqual(env["XAUTHORITY"], inherited["XAUTHORITY"])
        self.assertEqual(env["KEEP_THIS"], "still-here")
        self.assertEqual(env["HOME"], str(self.root.resolve()))
        self.assertEqual(inherited["HOME"], "/home/real-user")
        for key in (
                "DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "PULSE_SERVER",
                "PIPEWIRE_REMOTE", "SSH_AUTH_SOCK", "SESSION_MANAGER"):
            self.assertNotIn(key, env)
        for key in (
                "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME",
                "XDG_CACHE_HOME", "XDG_DATA_HOME"):
            path = Path(env[key])
            self.assertTrue(path.is_relative_to(self.root))
            self.assertTrue(path.is_dir())
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)

    def test_isolated_desktop_environment_rejects_runtime_symlinks(self):
        outside = self.root / "outside"
        outside.mkdir()
        (self.root / "runtime").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            isolated_desktop_environment({}, self.root, ":95")

    def test_isolated_desktop_environment_rejects_symlinked_root(self):
        link = self.root / "aliased"
        link.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            isolated_desktop_environment({}, link, ":95")

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
