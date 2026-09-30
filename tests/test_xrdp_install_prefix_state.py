# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


class XrdpInstallPrefixStateTests(unittest.TestCase):
    def test_changing_install_prefix_selects_fresh_generated_tree(self) -> None:
        source_dir = Path(sys.argv[1]).resolve()
        cmake = sys.argv[2]

        with tempfile.TemporaryDirectory(prefix="xrdp-prefix-state-") as temp:
            root = Path(temp)
            build_dir = root / "build"

            def configure(prefix: Path) -> dict[str, str]:
                subprocess.run(
                    [
                        cmake,
                        "-S",
                        str(source_dir),
                        "-B",
                        str(build_dir),
                        "-G",
                        "Ninja",
                        "-DBUILD_TESTING=OFF",
                        "-DXRDP_CONSOLE_BUILD_XRDP=ON",
                        "-DXRDP_CONSOLE_RUN_XRDP_TESTS=OFF",
                        "-DXRDP_CONSOLE_BUILD_TOOLS=OFF",
                        f"-DXRDP_CONSOLE_XRDP_INSTALL_DIR={prefix}",
                    ],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    timeout=90,
                )
                cache = (build_dir / "CMakeCache.txt").read_text(
                    encoding="utf-8"
                )

                def value(name: str) -> str:
                    match = re.search(
                        rf"^{re.escape(name)}:[^=]+=([^\n]*)$",
                        cache,
                        flags=re.MULTILINE,
                    )
                    if match is None:
                        self.fail(f"CMake cache is missing {name}")
                    return match.group(1)

                return {
                    "install": value("XRDP_CONSOLE_XRDP_INSTALL_DIR"),
                    "state": value("XRDP_CONSOLE_XRDP_STATE_HASH"),
                    "build": value("XRDP_CONSOLE_XRDP_BUILD_DIR"),
                }

            first = configure(root / "prefix-one")
            second = configure(root / "prefix-two")

            self.assertEqual(first["install"], str(root / "prefix-one"))
            self.assertEqual(second["install"], str(root / "prefix-two"))
            self.assertNotEqual(first["state"], second["state"])
            self.assertNotEqual(first["build"], second["build"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
