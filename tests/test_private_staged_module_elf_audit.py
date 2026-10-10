#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inert staged-module ELF regression tests; never execute the module."""
from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import private_staged_module_elf_audit as elf

DYNAMIC = (
    "0x0000001 (NEEDED) Shared library: [libxrdp.so.0]\n"
    "0x0000001 (NEEDED) Shared library: [libcommon.so.0]\n"
    "0x0000001 (NEEDED) Shared library: [libc.so.6]\n"
    "0x000001d (RUNPATH) Library runpath: [$ORIGIN]\n"
)
HEADER = "ELF Header:\n  Type: DYN (Shared object file)\n"


class StageELFTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.release = Path(tmp.name) / ".release/xrdp-console"
        self.release.mkdir(parents=True)
        self.install = self.release / ("build/_deps/xrdp-install-" + "a" * 16)
        self.lib = self.install / "lib/xrdp"
        self.lib.mkdir(parents=True)
        self.build = self.release / "build-module/bin"
        self.build.mkdir(parents=True)
        self.built = self.build / "libxrdp_console.so"
        self.staged = self.lib / "libxrdp_console.so"
        self.built.write_bytes(b"inert first-party module, NOT executable ELF")
        self.staged.write_bytes(self.built.read_bytes())
        for soname in ("libxrdp.so.0", "libcommon.so.0"):
            (self.lib / (soname + ".10.6")).write_bytes(
                ("inert private native library " + soname).encode())
            (self.lib / soname).symlink_to(soname + ".10.6")
        self.digest = hashlib.sha256(self.built.read_bytes()).hexdigest()
        self.dynamic = DYNAMIC
        self.header = HEADER

    def inspect(self, **overrides):
        inputs = {
            "release_root": str(self.release),
            "install_prefix": str(self.install),
            "built_module": str(self.built),
            "staged_module": str(self.staged),
            "expected_sha256": self.digest,
        }
        inputs.update(overrides)
        with mock.patch.object(
                elf, "_readelf",
                side_effect=lambda _p, flag: self.header if flag == "-h"
                else self.dynamic) as tool:
            result = elf.audit_module(**inputs)
        self.assertEqual(tool.call_count, 2)
        return result

    def test_internal_libtool_symlinks_are_supported_without_runtime(self):
        report = self.inspect()
        self.assertEqual(report["mode"], "READ_ONLY_STAGED_MODULE_ELF_AUDIT")
        self.assertFalse(report["runtime_authorized"])
        self.assertEqual(report["built_sha256"], self.digest)
        self.assertEqual(report["staged_sha256"], self.digest)
        self.assertEqual(set(report["private_needed_resolved"]),
                         {"libxrdp.so.0", "libcommon.so.0"})
        self.assertIn("libc.so.6", report["system_needed_unresolved"])
        for path in report["private_needed_resolved"].values():
            self.assertTrue(Path(path).is_relative_to(self.lib))

    def test_module_sha256_must_match_both_built_and_staged_bytes(self):
        with self.assertRaisesRegex(elf.UnsafeModule, "SHA-256"):
            self.inspect(expected_sha256="b" * 64)
        self.staged.write_bytes(b"altered staged bytes")
        with self.assertRaisesRegex(elf.UnsafeModule, "SHA-256"):
            self.inspect()

    def test_module_stage_must_be_exact_first_party_artifact(self):
        substitute = self.lib / "libxrdp.so"
        substitute.write_bytes(self.built.read_bytes())
        with self.assertRaisesRegex(elf.UnsafeModule, "first-party"):
            self.inspect(staged_module=str(substitute))
        self.staged.unlink()
        wrong = self.install / "lib/libxrdp_console.so"
        wrong.write_bytes(self.built.read_bytes())
        with self.assertRaisesRegex(elf.UnsafeModule, "first-party"):
            self.inspect(staged_module=str(wrong))

    def test_private_lib_symlink_escape_to_external_prefix_is_rejected(self):
        target = self.release.parent.parent / "protected-libcommon.so"
        target.write_bytes(b"outside libcommon")
        (self.lib / "libcommon.so.0").unlink()
        (self.lib / "libcommon.so.0").symlink_to(target)
        with self.assertRaisesRegex(elf.UnsafeModule, "outside matched"):
            self.inspect()
        self.assertEqual(target.read_bytes(), b"outside libcommon")

    def test_untrusted_rpath_and_runpath_entries_are_rejected(self):
        for value in (
            "/usr/local/lib/xrdp", "/run/xrdp/sockdir",
            str(self.release / "external-build"),
            "$ORIGIN/../../../../../../", "$ORIGINish",
            "$"+"{OTHER}/lib", "", ":$ORIGIN", "$ORIGIN:$ORIGIN",
        ):
            with self.subTest(runpath=value):
                self.dynamic = DYNAMIC.replace("[$ORIGIN]", "[" + value + "]")
                with self.assertRaises(elf.UnsafeModule):
                    self.inspect()
        self.dynamic = DYNAMIC

    def test_braced_origin_within_matched_install_is_valid(self):
        self.dynamic = DYNAMIC.replace("[$ORIGIN]", "[$"+"{ORIGIN}/../xrdp]")
        self.assertEqual(len(self.inspect()["private_needed_resolved"]), 2)

    def test_missing_private_needed_library_or_necessary_link_is_rejected(self):
        (self.lib / "libcommon.so.0").unlink()
        with self.assertRaisesRegex(elf.UnsafeModule, "not uniquely"):
            self.inspect()
        (self.lib / "libcommon.so.0").symlink_to("libcommon.so.0.10.6")
        self.dynamic = DYNAMIC.replace(
            "0x0000001 (NEEDED) Shared library: [libcommon.so.0]\n", "")
        with self.assertRaisesRegex(elf.UnsafeModule, "libcommon"):
            self.inspect()

    def test_non_shared_elf_header_or_wrong_soname_is_rejected(self):
        self.header = HEADER.replace("Type: DYN", "Type: EXEC")
        with self.assertRaisesRegex(elf.UnsafeModule, "shared-object"):
            self.inspect()
        self.header = HEADER
        self.dynamic = DYNAMIC + "0xe (SONAME) Library soname: [libxrdp.so.0]\n"
        with self.assertRaisesRegex(elf.UnsafeModule, "SONAME"):
            self.inspect()

    def test_duplicate_or_untrusted_dynamic_tags_fail_closed(self):
        with self.assertRaisesRegex(elf.UnsafeModule, "simultaneous"):
            elf.parse_dynamic(
                DYNAMIC + "0xf (RPATH) Library rpath: [$ORIGIN]\n")
        with self.assertRaisesRegex(elf.UnsafeModule, "Invalid"):
            elf.parse_dynamic(
                "0x1 (NEEDED) Shared library: [/protected/libcommon.so.0]\n")
        with self.assertRaisesRegex(elf.UnsafeModule, "duplicate"):
            elf.parse_dynamic(
                DYNAMIC + "0x1d (RUNPATH) Library runpath: [$ORIGIN]\n")

    def test_module_file_symlink_cannot_alias_unrelated_binary(self):
        target = self.release / "other-module.so"
        target.write_bytes(self.built.read_bytes())
        self.built.unlink()
        self.built.symlink_to(target)
        with self.assertRaisesRegex(elf.UnsafeModule, "symlinked"):
            self.inspect()

    def test_readelf_uses_only_system_inspection_with_sterile_environment(self):
        with (
            mock.patch.object(elf.shutil, "which",
                              return_value="/usr/bin/readelf") as which,
            mock.patch.object(elf.subprocess, "run",
                              return_value=mock.Mock(
                                  returncode=0, stdout=HEADER)) as invoke,
        ):
            self.assertEqual(elf._readelf(self.staged, "-h"), HEADER)
        which.assert_called_once_with("readelf", path="/usr/bin:/bin")
        argv, kw = invoke.call_args
        self.assertEqual(argv[0], [
            "/usr/bin/readelf", "-W", "-h", str(self.staged)])
        self.assertEqual(kw["env"], {"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
        self.assertFalse(kw["check"])
        self.assertTrue(kw["capture_output"])

    def test_static_report_cli_never_authorizes_runtime(self):
        args = [
            "--release", str(self.release), "--install", str(self.install),
            "--built", str(self.built), "--staged", str(self.staged),
            "--sha256", self.digest,
        ]
        with mock.patch.object(
                elf, "_readelf", side_effect=lambda _p, flag:
                HEADER if flag == "-h" else DYNAMIC):
            self.assertEqual(elf.main(args), 2)
        with self.assertRaises(SystemExit) as error:
            elf.main(args + ["--execute"])
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
