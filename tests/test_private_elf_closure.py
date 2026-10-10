#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Purely offline ELF policy regressions using inert byte stubs."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import private_elf_closure as elf


def section(needed, *, soname=None, runpath="$ORIGIN", rpath=None):
    return elf.ELFMetadata(tuple(needed), soname, runpath, rpath)


class ClosureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / ".release" / "xrdp-console"
        self.root.mkdir(parents=True)
        self.libdir = self.root / "lib"
        self.bindir = self.root / "bin"
        self.libdir.mkdir()
        self.bindir.mkdir()
        self.chansrv = self.bindir / "chansrv"
        self.sonames = ("libcommon.so.0", "libsesman.so.0", "libipm.so.0")
        self.paths = {"chansrv": self.chansrv}
        self.paths.update({name: self.libdir / name for name in self.sonames})
        for name, path in self.paths.items():
            path.write_bytes(b"\x7fELF inert: " + name.encode())
        self.spec = {
            "schema": 1,
            "release_root": str(self.root),
            "source_commit": elf.CHANSRV_SOURCE_REF,
            "chansrv": self.record(self.chansrv),
            "private_libraries": {
                name: self.record(self.paths[name]) for name in self.sonames},
        }
        self.sections = {
            self.chansrv: section((*self.sonames, "libc.so.6"),
                                  runpath="$ORIGIN/../lib"),
            **{self.paths[name]: section(
                ("libc.so.6",), soname=name, runpath="$ORIGIN")
               for name in self.sonames},
        }

    @staticmethod
    def record(path):
        return {"path": str(path),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    def audit(self):
        with mock.patch.object(elf, "_readelf",
                               side_effect=self.sections.__getitem__):
            return elf.audit_elf_closure(self.spec)

    def test_clean_private_static_graph_remains_runtime_blocked(self):
        with mock.patch.dict(os.environ, {
                "LD_LIBRARY_PATH": "/protected/xrdp", "DISPLAY": ":0"}):
            report = self.audit()
        self.assertFalse(report["runtime_authorized"])
        self.assertEqual(report["mode"], "STATIC_ELF_AUDIT_ONLY")
        self.assertEqual(report["private_dependencies_verified"],
                         sorted(self.sonames))
        self.assertIn("libc.so.6", report["system_dependencies_not_resolved"])

    def test_gnu_dynamic_section_parser(self):
        text = (
            " 0x0001 (NEEDED) Shared library: [libcommon.so.0]\n"
            " 0x0001 (NEEDED) Shared library: [libc.so.6]\n"
            " 0x000e (SONAME) Library soname: [libsesman.so.0]\n"
            " 0x001d (RUNPATH) Library runpath: [$ORIGIN/../lib]\n")
        parsed = elf.parse_dynamic_section(text)
        self.assertEqual(parsed.needed, ("libcommon.so.0", "libc.so.6"))
        self.assertEqual(parsed.soname, "libsesman.so.0")
        self.assertEqual(parsed.runpath, "$ORIGIN/../lib")

    def test_readelf_cannot_execute_target_or_use_inherited_environment(self):
        done = mock.Mock(returncode=0, stdout=(
            "0x1 (NEEDED) Shared library: [libc.so.6]\n"))
        with (mock.patch.object(elf.shutil, "which", return_value="/usr/bin/readelf"),
              mock.patch.object(elf.subprocess, "run", return_value=done) as call):
            self.assertEqual(elf._readelf(self.chansrv).needed, ("libc.so.6",))
        args, kwargs = call.call_args
        self.assertEqual(args[0],
                         ["/usr/bin/readelf", "-W", "-d", str(self.chansrv)])
        self.assertEqual(kwargs["env"], {"PATH": "/usr/bin:/bin", "LC_ALL": "C"})

    def test_no_protected_or_external_private_library_paths(self):
        for value in (
                "/home/keivan/xrdp-console/build-direct-console/_deps/"
                "xrdp-install/lib/libcommon.so.0",
                "/var/tmp/third-party-build/libcommon.so.0"):
            self.spec["private_libraries"][self.sonames[0]]["path"] = value
            with self.subTest(value=value), self.assertRaises(elf.UnsafeELF):
                self.audit()

    def test_protected_absolute_runpath_rejected(self):
        self.sections[self.chansrv] = section(
            self.sonames, runpath="/home/keivan/xrdp-console/"
            "build-direct-console/_deps/xrdp-install/lib")
        with self.assertRaises(elf.UnsafeELF):
            self.audit()

    def test_origin_escape_rejected(self):
        self.sections[self.chansrv] = section(
            self.sonames, runpath="$ORIGIN/../../../../")
        with self.assertRaises(elf.UnsafeELF):
            self.audit()

    def test_empty_relative_and_variable_runpaths_rejected(self):
        for value in ("", ":$ORIGIN/../lib", ".", "lib",
                      "$LIB", "$ORIGINish", chr(36) + "{FOO}/lib"):
            self.sections[self.chansrv] = section(self.sonames, runpath=value)
            with self.subTest(value=value), self.assertRaises(elf.UnsafeELF):
                self.audit()

    def test_braced_origin_resolves_inside_release(self):
        self.sections[self.chansrv] = section(
            self.sonames, runpath=chr(36) + "{ORIGIN}/../lib")
        self.assertEqual(len(self.audit()["private_dependencies_verified"]), 3)

    def test_missing_or_undeclared_private_soname_rejected(self):
        self.sections[self.chansrv] = section(
            (*self.sonames, "libxrdp.so.0"), runpath="$ORIGIN/../lib")
        with self.assertRaisesRegex(elf.UnsafeELF, "Undeclared"):
            self.audit()

    def test_wrong_soname_rejected(self):
        path = self.paths[self.sonames[0]]
        self.sections[path] = section(
            ("libc.so.6",), soname="libcommon.so.99")
        with self.assertRaisesRegex(elf.UnsafeELF, "SONAME"):
            self.audit()

    def test_missing_or_duplicate_search_path_rejected(self):
        for value in ("$ORIGIN", "$ORIGIN/../lib:$ORIGIN/../lib"):
            self.sections[self.chansrv] = section(self.sonames, runpath=value)
            with self.subTest(value=value), self.assertRaises(elf.UnsafeELF):
                self.audit()

    def test_source_and_sha256_must_match(self):
        self.spec["source_commit"] = "f" * 40
        with self.assertRaises(elf.UnsafeELF):
            self.audit()
        self.spec["source_commit"] = elf.CHANSRV_SOURCE_REF
        self.spec["chansrv"]["sha256"] = "a" * 64
        with self.assertRaises(elf.UnsafeELF):
            self.audit()

    def test_transitive_private_deps_are_not_ignored(self):
        self.sections[self.chansrv] = section(
            ("libcommon.so.0", "libsesman.so.0"), runpath="$ORIGIN/../lib")
        self.sections[self.paths["libsesman.so.0"]] = section(
            ("libipm.so.0", "libc.so.6"), soname="libsesman.so.0")
        self.assertEqual(self.audit()["private_dependencies_verified"],
                         sorted(self.sonames))

    def test_branded_internal_soname_symlink_within_private_tree(self):
        name = self.sonames[0]
        actual = self.libdir / (name + ".versioned")
        self.paths[name].rename(actual)
        (self.libdir / name).symlink_to(actual.name)
        self.spec["private_libraries"][name] = self.record(actual)
        self.sections[actual] = self.sections.pop(self.paths[name])
        self.assertEqual(len(self.audit()["private_dependencies_verified"]), 3)

    def test_both_rpath_and_runpath_rejected(self):
        with self.assertRaisesRegex(elf.UnsafeELF, "simultaneous"):
            elf.parse_dynamic_section(
                "0x1 (NEEDED) Shared library: [libc.so.6]\n"
                "0x1d (RUNPATH) Library runpath: [$ORIGIN]\n"
                "0xf (RPATH) Library rpath: [$ORIGIN]\n")

    def test_absolute_needed_path_rejected(self):
        with self.assertRaises(elf.UnsafeELF):
            elf.parse_dynamic_section(
                "0x1 (NEEDED) Shared library: [/protected/libcommon.so.0]")

    def test_cli_has_no_execution_and_always_returns_blocked(self):
        path = self.root / "audit.json"
        path.write_text(json.dumps(self.spec))
        with mock.patch.object(elf, "_readelf",
                               side_effect=self.sections.__getitem__):
            self.assertEqual(elf.main(["--manifest", str(path)]), 2)
        with self.assertRaises(SystemExit) as err:
            elf.main(["--manifest", str(path), "--execute"])
        self.assertEqual(err.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
