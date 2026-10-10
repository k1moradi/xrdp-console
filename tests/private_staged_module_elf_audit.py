#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static, read-only validation of the first-party staged xrdp console module.

Do not invoke ldd or dlopen the module: readelf inspects bytes without using
the dynamic linker. This verifies only the on-disk static ELF lookup topology,
NOT provenance of compiler output, actual dlopen, IPC or runtime acceptance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

HASH = re.compile(r"[0-9a-f]{64}\Z")
PRIVATE_SONAMES = ("libxrdp.so", "libcommon.so", "libsesman.so", "libipm.so")
DYNAMIC_FIELD = re.compile(
    r"\((NEEDED|SONAME|RUNPATH|RPATH)\)[^\n]*?\[([^\]]*)\]")


class UnsafeModule(RuntimeError):
    """A private staged module is not proven statically isolated."""


def _within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _canonical(path: str, label: str, *, directory: bool = False) -> Path:
    raw = Path(path)
    if not raw.is_absolute() or ".." in raw.parts:
        raise UnsafeModule(f"{label} must be an absolute non-traversing path")
    for ancestor in (raw, *raw.parents):
        if ancestor.is_symlink():
            raise UnsafeModule(f"{label} contains a symlinked ancestor")
    resolved = raw.resolve(strict=True)
    if resolved != raw or (not resolved.is_dir() if directory
                           else not resolved.is_file()):
        raise UnsafeModule(f"{label} must be an existing canonical artifact")
    if resolved.stat().st_uid != os.geteuid():
        raise UnsafeModule(f"{label} is not owned by the invoking user")
    return resolved


def _sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_dynamic(text: str) -> tuple[tuple[str, ...], str | None, str | None]:
    fields: dict[str, list[str]] = {}
    for tag, value in DYNAMIC_FIELD.findall(text):
        fields.setdefault(tag, []).append(value)
    for tag in ("SONAME", "RUNPATH", "RPATH"):
        if len(fields.get(tag, [])) > 1:
            raise UnsafeModule(f"Ambiguous duplicate ELF {tag}")
    if fields.get("RUNPATH") and fields.get("RPATH"):
        raise UnsafeModule("Ambiguous simultaneous ELF RPATH and RUNPATH")
    needed = tuple(fields.get("NEEDED", ()))
    if (not needed or len(set(needed)) != len(needed)
            or any(not name or "/" in name or "\\" in name for name in needed)):
        raise UnsafeModule("Invalid or missing DT_NEEDED entries")
    soname = (fields.get("SONAME") or [None])[0]
    runpath = (fields.get("RUNPATH") or fields.get("RPATH") or [None])[0]
    return needed, soname, runpath


def _readelf(path: Path, option: str) -> str:
    utility = shutil.which("readelf", path="/usr/bin:/bin")
    if utility is None:
        raise UnsafeModule("System readelf unavailable; never substitute ldd")
    result = subprocess.run(
        [utility, "-W", option, str(path)],
        capture_output=True, text=True, timeout=12, check=False,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
    if result.returncode:
        raise UnsafeModule("readelf could not inspect staged module")
    return result.stdout


def _lookup_dirs(runpath: str | None, staged: Path,
                 install: Path) -> tuple[Path, ...]:
    if runpath is None:
        return ()
    result = []
    for entry in runpath.split(":"):
        if not entry:
            raise UnsafeModule("Empty RPATH/RUNPATH component risks cwd lookup")
        if entry.startswith("$ORIGIN"):
            suffix = entry[len("$ORIGIN"):]
            if suffix and not suffix.startswith("/"):
                raise UnsafeModule("Unsupported ELF origin expression")
            candidate = staged.parent / suffix.lstrip("/")
        elif entry.startswith("$" + "{ORIGIN}"):
            suffix = entry[len("$" + "{ORIGIN}"):]
            if suffix and not suffix.startswith("/"):
                raise UnsafeModule("Unsupported ELF origin expression")
            candidate = staged.parent / suffix.lstrip("/")
        elif entry.startswith("/") and "$" not in entry:
            candidate = Path(entry)
        else:
            raise UnsafeModule(f"External/unsupported ELF RUNPATH entry: {entry}")
        # Do not resolve external paths just to discover whether they exist.
        if not _within(candidate, install):
            # Canonical comparison below also handles inside paths with .. .
            normalized = Path(os.path.normpath(candidate))
            if not _within(normalized, install):
                raise UnsafeModule("ELF RUNPATH references external/protected prefix")
        try:
            resolved = candidate.resolve(strict=True)
        except OSError as exc:
            raise UnsafeModule("ELF RUNPATH directory is missing") from exc
        if not resolved.is_dir() or not _within(resolved, install):
            raise UnsafeModule("ELF RUNPATH resolves outside private installation")
        if resolved in result:
            raise UnsafeModule("Duplicate ELF RUNPATH search directory")
        result.append(resolved)
    return tuple(result)


def audit_module(*, release_root: str, install_prefix: str,
                 built_module: str, staged_module: str,
                 expected_sha256: str) -> dict:
    """Statically check exact module bytes, DT_NEEDED and private RUNPATH.

    Never executes the module or other private xrdp binaries.
    """
    root = _canonical(release_root, "release root", directory=True)
    if root.name != "xrdp-console" or root.parent.name != ".release":
        raise UnsafeModule("Private root must be .release/xrdp-console")
    install = _canonical(install_prefix, "private install", directory=True)
    if not _within(install, root):
        raise UnsafeModule("Private install escapes release workspace")
    if not re.fullmatch(r"xrdp-install-[0-9a-f]{16}", install.name):
        raise UnsafeModule("Private install must be hash-keyed")
    built = _canonical(built_module, "built module")
    staged = _canonical(staged_module, "staged module")
    if not _within(built, root) or _within(built, install):
        raise UnsafeModule("Built module must be in a separate private build tree")
    expected_path = install / "lib" / "xrdp" / "libxrdp_console.so"
    if staged != expected_path:
        raise UnsafeModule("Staged file is not the first-party console module")
    if HASH.fullmatch(expected_sha256) is None:
        raise UnsafeModule("Invalid expected module hash")
    build_hash = _sha(built)
    stage_hash = _sha(staged)
    if build_hash != expected_sha256 or stage_hash != expected_sha256:
        raise UnsafeModule("Built/staged module SHA-256 identity mismatch")
    header = _readelf(staged, "-h")
    if not re.search(r"^\s*Type:\s+DYN\b", header, flags=re.MULTILINE):
        raise UnsafeModule("Staged module is not a shared-object ELF")
    needed, soname, runpath = parse_dynamic(_readelf(staged, "-d"))
    if soname not in (None, "libxrdp_console.so"):
        raise UnsafeModule("Unexpected first-party module SONAME")
    private = [name for name in needed if name.startswith(PRIVATE_SONAMES)]
    if not any(name.startswith("libxrdp.so") for name in private):
        raise UnsafeModule("First-party module is not linked against private libxrdp")
    if not any(name.startswith("libcommon.so") for name in private):
        raise UnsafeModule("First-party module is not linked against private libcommon")
    dirs = _lookup_dirs(runpath, staged, install)
    library_root = install / "lib" / "xrdp"
    if not library_root.is_dir() or library_root.is_symlink():
        raise UnsafeModule("Private xrdp loader directory unavailable")
    resolved_libs = {}
    for name in private:
        matches = []
        for folder in dirs:
            candidate = folder / name
            if candidate.exists():
                path = candidate.resolve(strict=True)
                if (not path.is_file() or not _within(path, library_root)):
                    raise UnsafeModule(f"Private {name} resolves outside matched lib/xrdp")
                matches.append(path)
        if len(set(matches)) != 1:
            raise UnsafeModule(f"Private {name} not uniquely found in reviewed RUNPATH")
        resolved_libs[name] = str(matches[0])
    return {
        "mode": "READ_ONLY_STAGED_MODULE_ELF_AUDIT",
        "runtime_authorized": False,
        "built_sha256": build_hash,
        "staged_sha256": stage_hash,
        "staged_module": str(staged),
        "private_needed_resolved": resolved_libs,
        "system_needed_unresolved": sorted(
            name for name in needed if name not in private),
        "runpath": runpath,
        "remaining_gates": [
            "Build transcript and actual Git source-to-ELF provenance",
            "Dynamic loader precedence, transitive dependencies and dlopen",
            "Actual private module/CLIPRDR session and chansrv IPC",
            "Private X11/Firefox screenshot File acceptance and explicit authorization",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Offline readelf inspection of private console stage")
    parser.add_argument("--release", required=True)
    parser.add_argument("--install", required=True)
    parser.add_argument("--built", required=True)
    parser.add_argument("--staged", required=True)
    parser.add_argument("--sha256", required=True)
    args = parser.parse_args(argv)
    try:
        result = audit_module(
            release_root=args.release, install_prefix=args.install,
            built_module=args.built, staged_module=args.staged,
            expected_sha256=args.sha256)
        print(json.dumps(result, indent=2, sort_keys=True))
    except (UnsafeModule, OSError, ValueError, subprocess.SubprocessError) as exc:
        print(json.dumps({"mode": "READ_ONLY_STAGED_MODULE_ELF_AUDIT",
                          "runtime_authorized": False, "error": str(exc)}))
    return 2  # Static metadata is never permission to start a session.


if __name__ == "__main__":
    raise SystemExit(main())
