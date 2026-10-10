#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Link real *synthetic* ELF modules with the production private RPATH policy.

Never builds, loads or executes xrdp, X11, a clipboard owner or the test
module. Only cmake, the system C compiler, and readelf inspect inert code.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent
RPATH_HELPER = SOURCE / "cmake/private_module_rpath.cmake"


class PrivateModuleLinkerRegression(unittest.TestCase):
    def test_private_module_is_linked_with_final_safe_origin_runpath(self):
        for executable in ("cmake", "cc", "readelf"):
            self.assertIsNotNone(shutil.which(executable),
                                 f"Missing offline linker test tool: {executable}")
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            src = base / "src"
            src.mkdir()
            build = base / "build"
            (src / "core.c").write_text("int synthetic_private_core(void) {return 7;}\n")
            (src / "module.c").write_text(
                "extern int synthetic_private_core(void);\n"
                "int synthetic_console_entry(void) {return synthetic_private_core();}\n")
            cmake = (
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(PrivateModuleRpathFixture LANGUAGES C)\n"
                "add_library(privatecore SHARED core.c)\n"
                "add_library(synthetic_console MODULE module.c)\n"
                "target_link_libraries(synthetic_console PRIVATE privatecore)\n"
                f'include("{RPATH_HELPER.as_posix()}")\n'
                "xrdp_console_set_private_module_rpath(synthetic_console)\n"
            )
            (src / "CMakeLists.txt").write_text(cmake)
            environment = {
                "PATH": "/usr/bin:/bin", "LC_ALL": "C",
                "HOME": str(base),
            }
            for cmd in (
                ["cmake", "-S", str(src), "-B", str(build)],
                ["cmake", "--build", str(build)],
            ):
                completed = subprocess.run(
                    cmd, env=environment, capture_output=True, text=True,
                    timeout=30, check=False)
                self.assertEqual(completed.returncode, 0,
                                 completed.stdout + completed.stderr)
            module = build / "libsynthetic_console.so"
            self.assertTrue(module.is_file())
            details = subprocess.run(
                ["readelf", "-W", "-d", str(module)],
                env=environment, capture_output=True, text=True,
                timeout=10, check=False)
            self.assertEqual(details.returncode, 0, details.stderr)
            runpaths = re.findall(
                r"\((?:RUNPATH|RPATH)\)[^\n]*\[([^\]]*)\]",
                details.stdout)
            self.assertEqual(runpaths, ["$ORIGIN"], details.stdout)
            self.assertFalse(any(not component for component in
                                 runpaths[0].split(":")))
            self.assertNotIn(str(base), runpaths[0])
            self.assertNotIn("/usr/local", runpaths[0])
            self.assertRegex(details.stdout,
                             r"\(NEEDED\).*?\[libprivatecore\.so\]")
            # We never dlopen or run libsynthetic_console.so.

    def test_private_rpath_policy_is_separate_from_normal_module(self):
        module = (SOURCE / "src/CMakeLists.txt").read_text()
        helper = RPATH_HELPER.read_text()
        self.assertIn("if(XRDP_CONSOLE_PRIVATE_XRDP_BUILD)", module)
        self.assertIn("xrdp_console_set_private_module_rpath(xrdp-console)",
                      module)
        self.assertIn("BUILD_WITH_INSTALL_RPATH TRUE", helper)
        self.assertIn('INSTALL_RPATH "$ORIGIN"', helper)
        self.assertIn("INSTALL_RPATH_USE_LINK_PATH FALSE", helper)
        self.assertIn('BUILD_RPATH ""', helper)
        self.assertIn("BUILD_RPATH\n", module)
        self.assertIn('INSTALL_RPATH "$ORIGIN/../xrdp"', module)
        self.assertIn("else()\n        # Preserve the existing normal", module)
        self.assertNotIn("patchelf", helper)
        self.assertNotIn("execute_process", helper)
        self.assertNotIn("file(WRITE", helper)


if __name__ == "__main__":
    unittest.main()
