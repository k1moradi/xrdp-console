#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Disable RDP monitor resizing for the shared physical x11vnc console.
# The physical Xorg display has a fixed mode; a Mac RDP client can otherwise
# request its window size and receive a black/unfinished framebuffer.

set -eu

config=/etc/xrdp/xrdp.ini
if [ "$(id -u)" -ne 0 ]; then
    echo "Run this script with sudo: sudo $0" >&2
    exit 1
fi
if [ ! -f "$config" ]; then
    echo "Missing $config" >&2
    exit 1
fi

if python3 - "$config" <<'PY'
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
start = next((i for i, line in enumerate(lines) if line.strip() == "[Console]"), None)
if start is None:
    raise SystemExit(1)
end = next((i for i in range(start + 1, len(lines))
           if lines[i].strip().startswith("[") and lines[i].strip().endswith("]")),
           len(lines))
entries = [line.split("=", 1)[1].strip().lower()
           for line in lines[start + 1:end]
           if "=" in line and not line.lstrip().startswith(("#", ";"))
           and line.split("=", 1)[0].strip().lower() == "enable_dynamic_resizing"]
raise SystemExit(0 if entries == ["false"] else 1)
PY
then
    echo "Console dynamic resizing is already disabled; no restart or backup needed."
    systemctl is-active --quiet xrdp || systemctl restart xrdp
    exit 0
fi

stamp=$(date +%Y%m%d-%H%M%S)
backup="$config.xrdp-console-before-console-dynamic-resize-$stamp"
cp -a -- "$config" "$backup"

CONFIG="$config" python3 - <<'PY'
import os
import re
import stat
import tempfile
from pathlib import Path

path = Path(os.environ["CONFIG"])
text = path.read_text(encoding="utf-8")
lines = text.splitlines(keepends=True)

section_start = None
section_end = len(lines)
for index, line in enumerate(lines):
    if line.strip() == "[Console]":
        section_start = index
        break
if section_start is None:
    raise SystemExit("xrdp.ini has no [Console] section")
for index in range(section_start + 1, len(lines)):
    stripped = lines[index].strip()
    if stripped.startswith("[") and stripped.endswith("]"):
        section_end = index
        break

key_re = re.compile(r"^\s*enable_dynamic_resizing\s*=", re.IGNORECASE)
console = lines[section_start + 1:section_end]
console = [line for line in console if not key_re.match(line)]
console.append("enable_dynamic_resizing=false\n")
new_lines = lines[:section_start + 1] + console + lines[section_end:]
new_text = "".join(new_lines)

mode = stat.S_IMODE(path.stat().st_mode)
uid = path.stat().st_uid
gid = path.stat().st_gid
fd, temporary = tempfile.mkstemp(prefix=".xrdp.ini.", dir=str(path.parent), text=True)
try:
    with os.fdopen(fd, "w", encoding="utf-8", newline="") as stream:
        stream.write(new_text)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, mode)
    os.chown(temporary, uid, gid)
    os.replace(temporary, path)
except Exception:
    try:
        os.unlink(temporary)
    except FileNotFoundError:
        pass
    raise

# Validate the result in the same section parser used for the edit.
result = path.read_text(encoding="utf-8").splitlines()
start = next(i for i, line in enumerate(result) if line.strip() == "[Console]")
end = next((i for i in range(start + 1, len(result))
            if result[i].strip().startswith("[") and result[i].strip().endswith("]")),
           len(result))
entries = [line.split("=", 1)[1].strip().lower()
           for line in result[start + 1:end]
           if "=" in line and not line.lstrip().startswith(("#", ";"))
           and line.split("=", 1)[0].strip().lower() == "enable_dynamic_resizing"]
if entries != ["false"]:
    raise SystemExit(f"unexpected Console enable_dynamic_resizing entries: {entries!r}")
PY

if ! systemctl restart xrdp || ! systemctl is-active --quiet xrdp; then
    echo "xrdp did not remain active; restoring $backup" >&2
    cp -a -- "$backup" "$config"
    systemctl restart xrdp || true
    exit 1
fi

echo "Disabled dynamic resizing for the physical x11vnc console."
echo "Backup: $backup"
echo "Active Console setting:"
awk '
    /^\[Console\]$/ { in_console=1; print; next }
    in_console && /^\[/ { exit }
    in_console && /^(enable_dynamic_resizing|lib|ip|port|disable_gfx|chansrvport|channel\.)/ { print }
' "$config"
systemctl --no-pager --full status xrdp | sed -n '1,16p'
