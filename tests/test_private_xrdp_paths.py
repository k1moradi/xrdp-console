#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline CMake-only private path tests, never builds/runs xrdp."""
from __future__ import annotations

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CMAKE_CHECK = ROOT / "cmake/private_xrdp_paths.cmake"
CMAKE_DEP = ROOT / "cmake/xrdp_dependency.cmake"


class PrivateXrdpPathsTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.scratch = Path(temp.name)
        self.root = self.scratch / ".release/xrdp-console"
        self.root.mkdir(parents=True)
        self.build = self.root / "build-private"
        self.deps = self.root / "deps-private"
        self.script = self.scratch / "paths.cmake"
        self.script.write_text(
            'include("' + CMAKE_CHECK.as_posix() + '")\n'
            'xrdp_console_validate_private_paths("${ROOT}" "${BUILD}" "${DEPS}")\n'
            'message(STATUS "RUNTIME=${XRDP_CONSOLE_PRIVATE_RUNSTATE_DIR}")\n'
            'message(STATUS "SOCKET=${XRDP_CONSOLE_PRIVATE_SOCKET_DIR}")\n'
            'message(STATUS "RELEASE=${XRDP_CONSOLE_PRIVATE_CANONICAL_RELEASE_ROOT}")\n'
        )

    def probe(self, root=None, build=None, deps=None):
        args = (
            root if root is not None else self.root,
            build if build is not None else self.build,
            deps if deps is not None else self.deps)
        return subprocess.run(
            ["cmake", "-DROOT=" + str(args[0]),
             "-DBUILD=" + str(args[1]), "-DDEPS=" + str(args[2]),
             "-P", str(self.script)],
            capture_output=True, text=True, timeout=10, check=False,
            env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})

    def test_private_paths_derived_without_runtime_side_effects(self):
        result = self.probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"RUNTIME={self.root}/runstate", result.stdout)
        self.assertIn(f"SOCKET={self.root}/socket-root", result.stdout)
        self.assertIn(f"RELEASE={self.root}", result.stdout)
        self.assertFalse((self.root / "runstate").exists())
        self.assertFalse((self.root / "socket-root").exists())

    def test_external_build_and_dependency_roots_rejected(self):
        cases = [
            {"build": self.root.parent / "external-build"},
            {"deps": self.root.parent / "external-deps"},
            {"build": self.root}, {"deps": self.root},
            {"build": self.build, "deps": self.build},
            {"deps": "/run/xrdp/sockdir"}, {"build": "/tmp/unverified-build"},
        ]
        for kwargs in cases:
            with self.subTest(kwargs=kwargs):
                self.assertNotEqual(self.probe(**kwargs).returncode, 0)

    def test_parent_traversal_rejected_before_normalization(self):
        for kwargs in (
            {"build": self.root / "x/../build-private"},
            {"deps": self.root / "x/../deps-private"},
            {"root": self.root / "../xrdp-console"},
        ):
            with self.subTest(kwargs=kwargs):
                self.assertNotEqual(self.probe(**kwargs).returncode, 0)

    def test_release_root_must_exist_and_have_required_names(self):
        self.assertNotEqual(
            self.probe(root=self.root.parent / "missing").returncode, 0)
        self.assertNotEqual(
            self.probe(root=self.root.parent.parent).returncode, 0)
        alias = self.root.parent / "xrdp-console-alias"
        alias.symlink_to(self.root, target_is_directory=True)
        self.assertNotEqual(self.probe(root=alias).returncode, 0)

    def test_build_and_runtime_symlinks_rejected(self):
        symlink = self.build
        symlink.symlink_to(self.scratch, target_is_directory=True)
        self.assertNotEqual(self.probe().returncode, 0)
        symlink.unlink()
        (self.root / "socket-root").symlink_to(
            "/run/xrdp/sockdir", target_is_directory=True)
        self.assertNotEqual(self.probe().returncode, 0)

    def test_existing_runtime_non_directory_rejected(self):
        (self.root / "runstate").write_text("not a directory")
        self.assertNotEqual(self.probe().returncode, 0)

    def test_production_defaults_and_upstream_pins_are_unchanged(self):
        source = CMAKE_DEP.read_text()
        for required in (
                'option(XRDP_CONSOLE_PRIVATE_XRDP_BUILD',
                'set(_xrdp_runstate_arg "--runstatedir=/run")',
                'set(_xrdp_socketdir_arg "--with-socketdir=/run/xrdp/sockdir")',
                '"--localstatedir=<INSTALL_DIR>/var"',
                '"--prefix=<INSTALL_DIR>"',
                'xrdp_console_validate_private_paths(',
                'XRDP_CONSOLE_XRDP_SOURCE_SHA256',
                '_xrdp_patchset_hash'):
            with self.subTest(required=required):
                self.assertIn(required, source)

    def test_private_prefix_is_fingerprinted_and_hash_keyed(self):
        source = CMAKE_DEP.read_text()
        for required in (
                'set(_xrdp_install_fingerprint "private-hash-keyed")',
                '"profile=${_xrdp_profile}\\n"',
                'xrdp-install-${_xrdp_state_tag}',
                '"${_xrdp_runstate_arg}"',
                '"${_xrdp_socketdir_arg}"',
                'CACHE PATH "Hash-keyed isolated private xrdp installation prefix" FORCE',
                'if(XRDP_CONSOLE_PRIVATE_XRDP_BUILD)'):
            with self.subTest(required=required):
                self.assertIn(required, source)

    def test_cache_profile_cannot_switch_from_private_to_production(self):
        source = CMAKE_DEP.read_text()
        self.assertIn("XRDP_CONSOLE_XRDP_CONFIGURED_PROFILE", source)
        self.assertIn("choose a fresh isolated CMAKE_BINARY_DIR", source)
        self.assertIn('NOT XRDP_CONSOLE_XRDP_CONFIGURED_PROFILE STREQUAL _xrdp_profile',
                      source)

    def private_flags_probe(self, *, flags="-O3 -march=native -mtune=native",
                            cppflags="", ldflags="", pkgpath="",
                            environment=None):
        """Execute only the pure CMake checks, never project configure."""
        script = self.scratch / "flags.cmake"
        script.write_text(
            'include("' + CMAKE_CHECK.as_posix() + '")\n'
            'xrdp_console_reject_private_host_environment()\n'
            'xrdp_console_reject_private_build_flags('
            '"${CFLAGS}" "${CPPFLAGS}" "${LDFLAGS}" "${PKGPATH}")\n'
        )
        clean = {"PATH": "/usr/bin:/bin", "LC_ALL": "C"}
        clean.update(environment or {})
        return subprocess.run(
            ["cmake", "-DCFLAGS=" + flags, "-DCPPFLAGS=" + cppflags,
             "-DLDFLAGS=" + ldflags, "-DPKGPATH=" + pkgpath,
             "-P", str(script)],
            capture_output=True, text=True, timeout=10, check=False,
            env=clean)

    def test_private_default_native_flags_are_accepted_offline(self):
        self.assertEqual(self.private_flags_probe().returncode, 0)
        self.assertEqual(self.private_flags_probe(
            flags="-O2 -g -fPIC -Wall -Werror").returncode, 0)

    def test_protected_prefix_injection_via_flags_is_rejected(self):
        cases = (
            {"flags": "-O2 -I/opt/protected/xrdp/include"},
            {"flags": "-O2 -L/opt/protected/xrdp/lib"},
            {"flags": "-O2 -Wl,-rpath,/opt/protected/xrdp/lib"},
            {"flags": "-O2 -B/opt/protected/bin"},
            {"flags": "-O2 @/tmp/unreviewed.rsp"},
            {"flags": "-O2 -fplugin=/opt/unreviewed/plugin.so"},
            {"flags": "-O2 -specs=/tmp/custom.spec"},
            {"cppflags": "-I/opt/protected/include"},
            {"ldflags": "-L/opt/protected/lib"},
            {"pkgpath": "/opt/protected/xrdp/lib/pkgconfig"},
        )
        for entry in cases:
            with self.subTest(entry=entry):
                self.assertNotEqual(self.private_flags_probe(**entry).returncode, 0)

    def test_private_config_cannot_inherit_ambient_loader_or_pkg_config(self):
        keys = ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT",
                "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR",
                "PKG_CONFIG_SYSROOT_DIR", "LIBRARY_PATH", "CPATH",
                "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
                "CMAKE_PREFIX_PATH", "CMAKE_LIBRARY_PATH")
        for key in keys:
            with self.subTest(variable=key):
                result = self.private_flags_probe(
                    environment={key: "/protected/private-xrdp"})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(key, result.stderr)

    def test_private_host_guard_precedes_pkgconfig_discovery(self):
        source = CMAKE_DEP.read_text()
        self.assertLess(source.index(
            "xrdp_console_reject_private_host_environment()"),
            source.index("find_package(PkgConfig REQUIRED)"))
        self.assertIn('"--disable-utmp"', source)
        self.assertIn('"--disable-vsock"', source)
        self.assertIn('set(_xrdp_utmp_arg "--enable-utmp")', source)
        self.assertIn('set(_xrdp_vsock_arg "--enable-vsock")', source)
        self.assertIn("xrdp_console_reject_private_build_flags(", source)

    def test_configuration_check_never_writes_or_launches(self):
        source = CMAKE_CHECK.read_text()
        for forbidden in ("file(MAKE_DIRECTORY", "file(WRITE",
                          "file(REMOVE_RECURSE", "execute_process("):
            self.assertNotIn(forbidden, source)


if __name__ == "__main__":
    unittest.main()
