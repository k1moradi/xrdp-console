# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import sys
import unittest


class BuildDirectConsoleScriptTests(unittest.TestCase):
    def test_preflight_and_activation_commands_are_copy_paste_safe(self) -> None:
        script = Path(sys.argv[1]).read_text(encoding="utf-8")

        self.assertIn('"Preflight (read-only):"', script)
        self.assertIn(
            "xrdp_install_root=${XRDP_CONSOLE_XRDP_INSTALL_DIR:-$build_root/_deps/xrdp-install}",
            script,
        )
        self.assertIn(
            '"-DXRDP_CONSOLE_XRDP_INSTALL_DIR=$xrdp_install_root"', script
        )
        self.assertIn(
            'printf "sudo env XRDP_CONSOLE_BUILD_DIR=\'%s\' '
            "'%s/scripts/activate-direct-console.sh' --preflight\\n\"",
            script,
        )
        self.assertIn(
            '"Activate only after reviewing the preflight result:"', script
        )
        self.assertIn(
            'printf "sudo env XRDP_CONSOLE_BUILD_DIR=\'%s\' '
            "'%s/scripts/activate-direct-console.sh'\\n\"",
            script,
        )
        self.assertNotIn(r"\n  '", script)


    def test_build_verifies_source_and_module_provenance(self) -> None:
        script = Path(sys.argv[1]).read_text(encoding="utf-8")

        self.assertIn("XRDP_CONSOLE_EXPECT_REVISION", script)
        self.assertIn("XRDP_CONSOLE_REQUIRE_CLEAN", script)
        self.assertIn("Source provenance changed during the build.", script)
        self.assertIn("generated/build_revision.h", script)
        self.assertIn("libxrdp_console.so", script)
        self.assertIn("strings \"$module_path\"", script)
        self.assertIn("Verified built module revision:", script)



if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
