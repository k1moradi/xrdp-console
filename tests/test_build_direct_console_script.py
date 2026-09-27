# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import sys
import unittest


class BuildDirectConsoleScriptTests(unittest.TestCase):
    def test_activation_command_is_copy_paste_safe_single_line(self) -> None:
        script = Path(sys.argv[1]).read_text(encoding="utf-8")

        expected = (
            "\"sudo env XRDP_CONSOLE_BUILD_DIR='$build_root' "
            "'$workspace_root/scripts/activate-direct-console.sh'\""
        )
        self.assertIn(expected, script)

        activation_section = script.split(
            '"Activate only after reviewing the test results:"', 1
        )[1]
        self.assertNotIn(r"\n  '", activation_section)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
