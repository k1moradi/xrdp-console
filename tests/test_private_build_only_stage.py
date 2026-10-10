#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline CMake source gates and inert private module stage tests."""
from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parent.parent
TOP = (SOURCE / "CMakeLists.txt").read_text()
TEST_CMAKE = (SOURCE / "tests/CMakeLists.txt").read_text()
SRC_CMAKE = (SOURCE / "src/CMakeLists.txt").read_text()
STAGE = SOURCE / "cmake/stage_private_xrdp_module.cmake"


class PrivateBuildOnlyTests(unittest.TestCase):
    def test_private_mode_is_opt_in_and_requires_xrdp_and_testing(self):
        self.assertIn('option(XRDP_CONSOLE_PRIVATE_BUILD_ONLY', TOP)
        self.assertIn('if(NOT XRDP_CONSOLE_BUILD_XRDP OR NOT XRDP_CONSOLE_PRIVATE_XRDP_BUILD)', TOP)
        self.assertIn('if(NOT BUILD_TESTING)', TOP)
        self.assertIn('XRDP_CONSOLE_PRIVATE_BUILD_ONLY_CONFIGURED', TOP)
        self.assertIn('flag changed in existing CMake cache', TOP)
        self.assertIn('option(XRDP_CONSOLE_RUN_XRDP_TESTS', TOP)
        self.assertIn('add_test(NAME xrdp-upstream-unit', TOP)
        self.assertIn('add_subdirectory(tests)', TOP)

    def test_verified_h264_freerdp_remains_mandatory_in_normal_mode(self):
        self.assertIn('if(XRDP_CONSOLE_PRIVATE_BUILD_ONLY)', TEST_CMAKE)
        self.assertIn('endif() # strict FreeRDP preflight in normal configurations only',
                      TEST_CMAKE)
        self.assertIn('RDP loader CTests require the project-built, H.264-capable',
                      TEST_CMAKE)
        self.assertIn('COMMAND "${XRDP_CONSOLE_FREERDP_EXECUTABLE}" /buildconfig',
                      TEST_CMAKE)
        self.assertIn('WITH_GFX_H264=ON', TEST_CMAKE)
        self.assertIn('WITH_(OPENH264|FFMPEG|VIDEO_FFMPEG)=ON', TEST_CMAKE)
        self.assertIn('add_test(NAME xrdp-loader-smoke', TEST_CMAKE)
        self.assertIn('if(XRDP_CONSOLE_FREERDP_EXECUTABLE AND', TEST_CMAKE)
        self.assertIn('cannot adopt a FreeRDP executable from the cache',
                      TEST_CMAKE)

    def test_no_global_test_bypass_or_fake_freerdp_executable(self):
        self.assertNotIn('BUILD_TESTING OFF', TOP)
        self.assertNotIn('BUILD_TESTING=OFF', TEST_CMAKE)
        self.assertNotIn('touch xfreerdp', TEST_CMAKE)
        self.assertNotIn('find_program(XRDP_CONSOLE_FREERDP_EXECUTABLE', TEST_CMAKE)
        self.assertIn('Private build-only: omit FreeRDP-dependent RDP loader tests',
                      TEST_CMAKE)
        self.assertIn('xrdp-private-build-paths-unit', TEST_CMAKE)
        self.assertIn('xrdp-private-build-provenance-unit', TEST_CMAKE)

    def test_private_staging_is_explicit_and_does_not_change_normal_install(self):
        self.assertIn('if(XRDP_CONSOLE_PRIVATE_XRDP_BUILD)', SRC_CMAKE)
        self.assertIn('add_custom_target(xrdp-console-private-stage', SRC_CMAKE)
        self.assertIn('DEPENDS xrdp-console xrdp_upstream', SRC_CMAKE)
        self.assertIn('-P "${CMAKE_SOURCE_DIR}/cmake/stage_private_xrdp_module.cmake"',
                      SRC_CMAKE)
        self.assertIn('LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/xrdp-console"',
                      SRC_CMAKE)
        self.assertNotIn('install(TARGETS xrdp-console\\n'
                         '        LIBRARY DESTINATION "${XRDP_CONSOLE_XRDP_INSTALL_DIR}',
                         SRC_CMAKE)


class PrivateModuleStageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / ".release/xrdp-console"
        self.root.mkdir(parents=True)
        self.build = self.root / "build/abc-private"
        self.build.mkdir(parents=True)
        self.install = self.root / ("build/abc-private/_deps/xrdp-install-" + "a" * 16)
        self.install.mkdir(parents=True)
        self.lib = self.install / "lib/xrdp"
        self.lib.mkdir(parents=True)
        (self.install / "sbin").mkdir()
        for name in ("libxrdp.so", "libcommon.so"):
            (self.lib / name).write_bytes(b"inert upstream dependency " + name.encode())
        (self.install / "sbin/xrdp").write_bytes(b"inert upstream server")
        self.module = self.build / "bin/libxrdp_console.so"
        self.module.parent.mkdir()
        self.module.write_bytes(b"inert first party module: no ELF and no runtime")

    def stage(self, *, release=None, install=None, build=None, module=None):
        return subprocess.run(
            ["cmake",
             "-DPRIVATE_RELEASE_ROOT=" + str(release or self.root),
             "-DPRIVATE_INSTALL_ROOT=" + str(install or self.install),
             "-DPRIVATE_BUILD_ROOT=" + str(build or self.build),
             "-DMODULE_FILE=" + str(module or self.module),
             "-P", str(STAGE)],
            capture_output=True, text=True, check=False, timeout=10,
            env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})

    def test_mocked_file_stages_once_and_is_hash_idempotent(self):
        result = self.stage()
        self.assertEqual(result.returncode, 0, result.stderr)
        dest = self.lib / self.module.name
        self.assertEqual(dest.read_bytes(), self.module.read_bytes())
        self.assertIn('PRIVATE_MODULE_STATIC_STAGE_OK', result.stdout + result.stderr)
        self.assertEqual(self.stage().returncode, 0)
        self.assertEqual(hashlib.sha256(dest.read_bytes()).hexdigest(),
                         hashlib.sha256(self.module.read_bytes()).hexdigest())
        self.assertFalse((self.root / "socket-root").exists())

    def test_reject_existing_different_module_or_symlink(self):
        dest = self.lib / self.module.name
        dest.write_bytes(b"unreviewed previous module")
        self.assertNotEqual(self.stage().returncode, 0)
        dest.unlink()
        outside = Path(self.tmp.name) / "outside.so"
        outside.write_bytes(b"protected")
        dest.symlink_to(outside)
        self.assertNotEqual(self.stage().returncode, 0)
        self.assertEqual(outside.read_bytes(), b"protected")

    def test_private_libtool_soname_symlinks_are_allowed_only_inside_libdir(self):
        for name in ("libxrdp.so", "libcommon.so"):
            original = self.lib / name
            versioned = self.lib / (name + ".0.10.6")
            original.rename(versioned)
            original.symlink_to(versioned.name)
        passed = self.stage()
        self.assertEqual(passed.returncode, 0, passed.stderr)
        self.assertTrue((self.lib / self.module.name).is_file())

    def test_private_libtool_symlink_may_not_resolve_outside_loader_directory(self):
        internal = self.lib / "libcommon.so"
        internal.unlink()
        outside = Path(self.tmp.name) / "libcommon.so.0"
        outside.write_bytes(b"external bogus libcommon")
        internal.symlink_to(outside)
        failed = self.stage()
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("escapes matched loader directory",
                      failed.stdout + failed.stderr)
        self.assertFalse((self.lib / self.module.name).exists())
        self.assertEqual(outside.read_bytes(), b"external bogus libcommon")

    def test_no_external_install_or_source(self):
        for overrides in (
                {"install": self.root.parent / "external"},
                {"install": Path("/usr/local/lib/xrdp")},
                {"release": Path("/run/xrdp")},
                {"build": Path(self.tmp.name)},
                {"module": self.lib / "libxrdp.so"}):
            with self.subTest(overrides=overrides):
                self.assertNotEqual(self.stage(**overrides).returncode, 0)

    def test_reject_unmatched_module_basename_or_version_prefix(self):
        wrong = self.module.parent / "libxrdp.so"
        wrong.write_bytes(b"not the first-party module")
        self.assertNotEqual(self.stage(module=wrong).returncode, 0)
        old_install = self.install
        wrong_install = self.root / "build/abc-private/_deps/xrdp-install-old"
        old_install.rename(wrong_install)
        self.assertNotEqual(self.stage(install=wrong_install).returncode, 0)

    def test_reject_missing_private_deps_without_creating_files(self):
        (self.lib / "libcommon.so").unlink()
        self.assertNotEqual(self.stage().returncode, 0)
        self.assertFalse((self.lib / self.module.name).exists())

    def test_reject_symlink_redirect_of_private_libdir(self):
        original = self.lib
        original.rename(original.parent / "old-xrdp")
        original.symlink_to(original.parent / "old-xrdp", target_is_directory=True)
        self.assertNotEqual(self.stage().returncode, 0)

    def test_reject_traversal_or_alias_build_paths(self):
        result = self.stage(build=self.root / "build/abc-private/../abc-private")
        self.assertNotEqual(result.returncode, 0)
        alias = self.root / "staging-alias"
        alias.symlink_to(self.build, target_is_directory=True)
        self.assertNotEqual(self.stage(build=alias).returncode, 0)


if __name__ == "__main__":
    unittest.main()
