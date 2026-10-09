# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import sys
import unittest


class BuildDirectConsoleScriptTests(unittest.TestCase):
    def test_parallel_build_applies_to_ninja_and_pinned_xrdp(self) -> None:
        script = Path(sys.argv[1]).read_text(encoding="utf-8")
        self.assertIn('build_jobs=${XRDP_CONSOLE_BUILD_JOBS:-2}', script)
        self.assertIn('xrdp_make_jobs=${XRDP_CONSOLE_XRDP_BUILD_JOBS:-$build_jobs}', script)
        self.assertIn('cmake --build "$build_root" --parallel "$build_jobs"', script)
        self.assertIn('"-DXRDP_CONSOLE_XRDP_BUILD_JOBS=$xrdp_make_jobs"', script)
        self.assertIn('XRDP_CONSOLE_FREERDP_BUILD_JOBS=', script)
        self.assertNotIn('cmake --build "$build_root" --parallel 1', script)

    def test_ctest_is_gated_by_read_only_socketdir_diagnostics(self) -> None:
        script = Path(sys.argv[1]).read_text(encoding="utf-8")
        diagnostic = 'tools/diagnostics/xrdp_clipboard_test_doctor.py'
        self.assertIn(diagnostic, script)
        self.assertLess(script.index(diagnostic),
                        script.index('ctest --test-dir "$build_root"'))
        self.assertIn('ctest --test-dir "$build_root" --output-on-failure --parallel 1', script)

    def test_scratch_files_remain_in_build_root(self) -> None:
        script = Path(sys.argv[1]).read_text(encoding="utf-8")
        self.assertIn('test_scratch_root=$build_root/test-artifacts/tmp', script)
        self.assertIn('export TMPDIR', script)

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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
