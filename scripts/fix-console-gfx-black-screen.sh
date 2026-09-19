#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Disable xrdp GFX for the shared physical x11vnc console.
# GFX is carried over drdynvc.  The stock/custom xrdp 0.10.6.1 binary used by
# this host does not implement a disable_gfx setting.  GFX capabilities are
# negotiated before the selected session profile is fully active, so drdynvc
# must be disabled both globally and for [Console].  cliprdr remains on.

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
def entries(section_name, key_name):
    start = next((i for i, line in enumerate(lines)
                  if line.strip() == f"[{section_name}]"), None)
    if start is None:
        return []
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].strip().startswith("[") and
                lines[i].strip().endswith("]")), len(lines))
    return [line.split("=", 1)[1].strip().lower()
            for line in lines[start + 1:end]
            if "=" in line and not line.lstrip().startswith(("#", ";"))
            and line.split("=", 1)[0].strip().lower() == key_name]

raise SystemExit(0 if (
    entries("Channels", "drdynvc") == ["false"] and
    entries("Console", "channel.drdynvc") == ["false"] and
    not entries("Console", "disable_gfx")
) else 1)
PY
then
    echo "Global and Console drdynvc are already disabled; no restart or backup needed."
    systemctl is-active --quiet xrdp || systemctl restart xrdp
    exit 0
fi

stamp=$(date +%Y%m%d-%H%M%S)
backup="$config.xrdp-vnc-bench-before-console-gfx-disable-$stamp"
cp -a -- "$config" "$backup"

CONFIG="$config" python3 - <<'PY'
import os
import re
import stat
import tempfile
from pathlib import Path

path = Path(os.environ["CONFIG"])
lines = path.read_text(encoding="utf-8").splitlines(keepends=True)

def replace_key(section_name, key_name):
    global lines
    section_start = next((i for i, line in enumerate(lines)
                          if line.strip() == f"[{section_name}]"), None)
    if section_start is None:
        raise SystemExit(f"xrdp.ini has no [{section_name}] section")
    section_end = next((i for i in range(section_start + 1, len(lines))
                        if lines[i].strip().startswith("[") and
                        lines[i].strip().endswith("]")), len(lines))
    key_re = re.compile(r"^\s*" + re.escape(key_name) + r"\s*=", re.IGNORECASE)
    section = [line for line in lines[section_start + 1:section_end]
               if not key_re.match(line) or line.lstrip().startswith(("#", ";"))]
    section.append(f"{key_name}=false\n")
    lines = lines[:section_start + 1] + section + lines[section_end:]

def remove_key(section_name, key_name):
    global lines
    section_start = next((i for i, line in enumerate(lines)
                          if line.strip() == f"[{section_name}]"), None)
    if section_start is None:
        raise SystemExit(f"xrdp.ini has no [{section_name}] section")
    section_end = next((i for i in range(section_start + 1, len(lines))
                        if lines[i].strip().startswith("[") and
                        lines[i].strip().endswith("]")), len(lines))
    key_re = re.compile(r"^\s*" + re.escape(key_name) + r"\s*=", re.IGNORECASE)
    section = [line for line in lines[section_start + 1:section_end]
               if not key_re.match(line) or line.lstrip().startswith(("#", ";"))]
    lines = lines[:section_start + 1] + section + lines[section_end:]

replace_key("Channels", "drdynvc")
replace_key("Console", "channel.drdynvc")
remove_key("Console", "disable_gfx")
new_text = "".join(lines)

metadata = path.stat()
fd, temporary = tempfile.mkstemp(prefix=".xrdp.ini.", dir=str(path.parent), text=True)
try:
    with os.fdopen(fd, "w", encoding="utf-8", newline="") as stream:
        stream.write(new_text)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, stat.S_IMODE(metadata.st_mode))
    os.chown(temporary, metadata.st_uid, metadata.st_gid)
    os.replace(temporary, path)
except Exception:
    try:
        os.unlink(temporary)
    except FileNotFoundError:
        pass
    raise

result = path.read_text(encoding="utf-8").splitlines()
def check(section_name, key_name):
    start = next(i for i, line in enumerate(result)
                 if line.strip() == f"[{section_name}]")
    end = next((i for i in range(start + 1, len(result))
                if result[i].strip().startswith("[") and
                result[i].strip().endswith("]")), len(result))
    return [line.split("=", 1)[1].strip().lower()
            for line in result[start + 1:end]
            if "=" in line and not line.lstrip().startswith(("#", ";"))
            and line.split("=", 1)[0].strip().lower() == key_name]

if check("Channels", "drdynvc") != ["false"]:
    raise SystemExit("unexpected global drdynvc entries")
if check("Console", "channel.drdynvc") != ["false"]:
    raise SystemExit("unexpected Console channel.drdynvc entries")
PY

if ! systemctl restart xrdp || ! systemctl is-active --quiet xrdp; then
    echo "xrdp did not remain active; restoring $backup" >&2
    cp -a -- "$backup" "$config"
    systemctl restart xrdp || true
    exit 1
fi

echo "Disabled xrdp GFX before capability negotiation via global drdynvc=false."
echo "Console override channel.drdynvc=false is also present."
echo "Clipboard channel remains enabled."
echo "Backup: $backup"
awk '
    /^\[Channels\]$/ { in_channels=1; print; next }
    in_channels && /^\[/ { exit }
    in_channels && /^drdynvc=/ { print }
' "$config"
awk '
    /^\[Console\]$/ { in_console=1; print; next }
    in_console && /^\[/ { exit }
    in_console && /^(channel\.cliprdr|channel\.drdynvc|disable_gfx|enable_dynamic_resizing)/ { print }
' "$config"
systemctl --no-pager --full status xrdp | sed -n '1,16p'
