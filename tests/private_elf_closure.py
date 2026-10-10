#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline-only ELF closure evidence for a future private chansrv build.

Reads ELF metadata via readelf(1), never executes the candidate or calls ldd.
A green report is *not* runtime authorization or proof of sesman IPC safety.
"""
from __future__ import annotations

from dataclasses import dataclass
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from typing import Any

CHANSRV_SOURCE_REF = "447447ff68fe34cf2391ca9c908c4bd993ed4ef2"
PRIVATE_FAMILIES = ("libcommon.so", "libsesman.so", "libipm.so", "libxrdp.so")
REQUIRED_FAMILIES = ("libcommon.so", "libsesman.so", "libipm.so")
SHA256 = re.compile(r"[a-f0-9]{64}\Z")
DYNAMIC_FIELD = re.compile(r"\((NEEDED|SONAME|RUNPATH|RPATH)\)[^\n]*?\[([^\]]*)\]")


class UnsafeELF(RuntimeError):
    """Cannot prove a private static ELF dependency closure."""


@dataclass(frozen=True)
class ELFMetadata:
    needed: tuple[str, ...]
    soname: str | None
    runpath: str | None
    rpath: str | None


def parse_dynamic_section(output: str) -> ELFMetadata:
    fields: dict[str, list[str]] = {}
    for tag, value in DYNAMIC_FIELD.findall(output):
        fields.setdefault(tag, []).append(value)
    for unique in ("SONAME", "RUNPATH", "RPATH"):
        if len(fields.get(unique, [])) > 1:
            raise UnsafeELF(f"Ambiguous duplicate ELF {unique}")
    if "RUNPATH" in fields and "RPATH" in fields:
        raise UnsafeELF("Ambiguous simultaneous RPATH and RUNPATH")
    needed = fields.get("NEEDED", [])
    if not needed or any(not name or "/" in name or "\\" in name
                         for name in needed):
        raise UnsafeELF("Missing or unsafe ELF DT_NEEDED entries")
    return ELFMetadata(tuple(needed),
                       (fields.get("SONAME") or [None])[0],
                       (fields.get("RUNPATH") or [None])[0],
                       (fields.get("RPATH") or [None])[0])


def _within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _release(path: Any) -> Path:
    if type(path) is not str:
        raise UnsafeELF("Missing private release root")
    candidate = Path(path)
    if not candidate.is_absolute() or ".." in candidate.parts:
        raise UnsafeELF("Release root must be absolute without traversal")
    root = candidate.resolve(strict=True)
    if (root.name != "xrdp-console" or root.parent.name != ".release"
            or root.stat().st_uid != os.geteuid() or not root.is_dir()):
        raise UnsafeELF("Expected owned .release/xrdp-console workspace")
    return root


def _artifact(spec: Any, root: Path, name: str) -> Path:
    if not isinstance(spec, dict) or type(spec.get("path")) is not str:
        raise UnsafeELF(f"{name}: missing path")
    candidate = Path(spec["path"])
    if (not candidate.is_absolute() or ".." in candidate.parts or
            not _within(candidate, root)):
        raise UnsafeELF(f"{name}: outside the protected private workspace")
    path = candidate.resolve(strict=True)
    if (not _within(path, root) or not path.is_file()
            or path.stat().st_uid != os.geteuid()):
        raise UnsafeELF(f"{name}: symlink escapes private workspace")
    sha = spec.get("sha256")
    if type(sha) is not str or SHA256.fullmatch(sha) is None:
        raise UnsafeELF(f"{name}: missing SHA-256")
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    if h.hexdigest() != sha:
        raise UnsafeELF(f"{name}: SHA-256 mismatch")
    return path


def _readelf(path: Path) -> ELFMetadata:
    utility = shutil.which("readelf")
    if not utility:
        raise UnsafeELF("readelf unavailable; never substitute ldd")
    result = subprocess.run(
        [utility, "-W", "-d", str(path)], capture_output=True,
        timeout=10, check=False, text=True,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
    if result.returncode != 0:
        raise UnsafeELF(f"ELF dynamic section unavailable: {path.name}")
    return parse_dynamic_section(result.stdout)


def _search_dirs(path: Path, elf: ELFMetadata, root: Path) -> tuple[Path, ...]:
    raw_path = elf.runpath if elf.runpath is not None else elf.rpath
    if raw_path is None:
        return ()
    dirs = []
    for item in raw_path.split(":"):
        if not item:
            raise UnsafeELF("Empty RUNPATH component could load cwd")
        if item.startswith("$ORIGIN"):
            tail = item[len("$ORIGIN"):]
            if tail and not tail.startswith("/"):
                raise UnsafeELF("Unexpected RUNPATH variable expansion")
            candidate = path.parent / tail.lstrip("/")
        elif item.startswith(chr(36) + "{ORIGIN}"):
            tail = item[len(chr(36) + "{ORIGIN}"):]
            if tail and not tail.startswith("/"):
                raise UnsafeELF("Unexpected RUNPATH variable expansion")
            candidate = path.parent / tail.lstrip("/")
        elif item.startswith("/") and "$" not in item:
            candidate = Path(item)
        else:
            raise UnsafeELF(f"Untrusted RUNPATH component: {item}")
        # Reject lexical external paths before even trying to inspect them.
        if not _within(candidate, root):
            raise UnsafeELF("RUNPATH references protected/external directory")
        try:
            resolved = candidate.resolve(strict=True)
        except OSError as exc:
            raise UnsafeELF("RUNPATH directory unavailable") from exc
        if not resolved.is_dir() or not _within(resolved, root):
            raise UnsafeELF("RUNPATH resolves into protected/external directory")
        dirs.append(resolved)
    return tuple(dirs)


def audit_elf_closure(spec: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(spec, dict) or spec.get("schema") != 1:
        raise UnsafeELF("Unrecognized ELF audit schema")
    if spec.get("source_commit") != CHANSRV_SOURCE_REF:
        raise UnsafeELF("Candidate is not pinned to corrected PR #32 source")
    root = _release(spec.get("release_root"))
    chansrv = _artifact(spec.get("chansrv"), root, "chansrv")
    declarations = spec.get("private_libraries")
    if not isinstance(declarations, dict) or not declarations:
        raise UnsafeELF("Missing explicit private xrdp library manifest")
    if any(type(name) is not str or "/" in name or "\\" in name
           for name in declarations):
        raise UnsafeELF("Invalid private SONAME key")
    libraries = {name: _artifact(record, root, name)
                 for name, record in declarations.items()}
    if chansrv in libraries.values() or len(set(libraries.values())) != len(libraries):
        raise UnsafeELF("Duplicate ELF artifact identity")
    metadata = {chansrv: _readelf(chansrv)}
    metadata.update({path: _readelf(path) for path in libraries.values()})
    for soname, path in libraries.items():
        if not soname.startswith(PRIVATE_FAMILIES):
            raise UnsafeELF(f"Unknown xrdp library family: {soname}")
        if metadata[path].soname != soname:
            raise UnsafeELF(f"{soname}: declared SONAME differs from ELF")
    verified: set[str] = set()
    system: set[str] = set()
    for path, dynamic in metadata.items():
        lookup_dirs = _search_dirs(path, dynamic, root)
        for needed in dynamic.needed:
            if needed.startswith(PRIVATE_FAMILIES):
                library = libraries.get(needed)
                if library is None:
                    raise UnsafeELF(f"Undeclared private xrdp DT_NEEDED: {needed}")
                candidates = [
                    (directory / needed).resolve(strict=True)
                    for directory in lookup_dirs if (directory / needed).exists()]
                if candidates != [library]:
                    raise UnsafeELF(
                        f"{path.name}: {needed} not uniquely resolvable in private RUNPATH")
                verified.add(needed)
            else:
                system.add(needed)
    if any(not any(name.startswith(family) for name in verified)
           for family in REQUIRED_FAMILIES):
        raise UnsafeELF("Private libcommon, libsesman and libipm not all linked")
    return {
        "mode": "STATIC_ELF_AUDIT_ONLY", "runtime_authorized": False,
        "source_commit": CHANSRV_SOURCE_REF,
        "chansrv_sha256": spec["chansrv"]["sha256"],
        "private_dependencies_verified": sorted(verified),
        "system_dependencies_not_resolved": sorted(system),
        "remaining_gates": [
            "Actual dynamic-loader resolution, transitive system libs and dlopen unverified",
            "Private runstate/chansrv/sesman/CLIPRDR IPC handshake unverified",
            "Private Xvfb and Firefox X11 process identities unverified",
            "Runtime still needs separate explicit authorization",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Read-only chansrv ELF metadata audit")
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        report = audit_elf_closure(
            json.loads(args.manifest.read_text(encoding="utf-8")))
        print(json.dumps(report, indent=2, sort_keys=True))
    except (UnsafeELF, OSError, ValueError, TypeError,
            subprocess.SubprocessError, json.JSONDecodeError) as exc:
        print(json.dumps({"mode": "STATIC_ELF_AUDIT_ONLY",
                          "runtime_authorized": False, "error": str(exc)}))
    return 2  # Even a passing static audit never authorizes execution.


if __name__ == "__main__":
    raise SystemExit(main())
