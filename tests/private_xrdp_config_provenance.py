#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only CMake/source provenance gate for a *private* xrdp build.

The host operator runs this against the exact reviewed Git worktree and its
configured CMakeCache. No xrdp binaries, loader, clipboard, X11 or network
processes are executed. Git is invoked only in read-only inspection modes.
This is configuration provenance, not compiler-output reproducibility.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
from typing import Any

CORRECTED_CHANSRV_ANCESTOR = "447447ff68fe34cf2391ca9c908c4bd993ed4ef2"
EXPECTED_VERSION = "0.10.6.1"
EXPECTED_ARCHIVE = "2f7beb5a3b2529c8d72dc0df9b8cdca31ab0e0c14d1e3421210f5e6ec0ab3b75"
EXPECTED_URL = ("https://github.com/neutrinolabs/xrdp/releases/download/"
                "v0.10.6.1/xrdp-0.10.6.1.tar.gz")
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
TAG = re.compile(r"[0-9a-f]{16}\Z")
COMMIT = re.compile(r"[0-9a-f]{40}\Z")

# Exact project() metadata from the reviewed top-level CMakeLists.txt:
# project(xrdp_console VERSION 0.1.0
#   DESCRIPTION "Direct-X11 shared physical-console module for xrdp"
#   LANGUAGES C CXX)
#
# CMake 3.31/4.2 write these generated bookkeeping values as :STATIC in
# CMakeCache.txt. None are authoritative source, compiler or install inputs.
# Do not broadly permit arbitrary STATIC fields.
PROJECT_STATIC_METADATA: dict[str, str] = {
    "CMAKE_PROJECT_NAME": "xrdp_console",
    "CMAKE_PROJECT_DESCRIPTION": "Direct-X11 shared physical-console module for xrdp",
    "CMAKE_PROJECT_VERSION": "0.1.0",
    "CMAKE_PROJECT_VERSION_MAJOR": "0",
    "CMAKE_PROJECT_VERSION_MINOR": "1",
    "CMAKE_PROJECT_VERSION_PATCH": "0",
    "CMAKE_PROJECT_VERSION_TWEAK": "",
    "CMAKE_PROJECT_HOMEPAGE_URL": "",
    "CMAKE_PROJECT_COMPAT_VERSION": "",
    "CMAKE_PROJECT_SPDX_LICENSE": "",
    "xrdp_console_IS_TOP_LEVEL": "ON",
}
# Dynamic STATIC paths must independently match the already attested source
# and build; do not allow aliases, traversal, or ancestor symlinks.
PROJECT_STATIC_PATH_KEYS = frozenset({
    "CMAKE_FIND_PACKAGE_REDIRECTS_DIR",
    "xrdp_console_BINARY_DIR",
    "xrdp_console_SOURCE_DIR",
})



class ProvenanceError(RuntimeError):
    pass


def sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def within(value: Path, root: Path) -> bool:
    return value == root or root in value.parents


def canonical_directory(value: str | Path, *, label: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or ".." in path.parts:
        raise ProvenanceError(f"{label}: require absolute non-traversing path")
    # Existing ancestor symlinks may rebase paths onto installed resources.
    for parent in (path, *path.parents):
        if parent.is_symlink():
            raise ProvenanceError(f"{label}: symlinked ancestor not allowed")
    try:
        resolved = path.resolve(strict=True)
    except OSError as exc:
        raise ProvenanceError(
            f"{label}: required canonical directory unavailable") from exc
    if not resolved.is_dir() or resolved != path:
        raise ProvenanceError(f"{label}: existing canonical directory required")
    return resolved


def read_cache(text: str) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in text.splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        left, value = line.split("=", 1)
        if ":" not in left:
            continue
        key, typ = left.split(":", 1)
        if typ == "STATIC":
            # Allow only generated project() metadata with source-defined
            # immutable values, plus separately validated source/build paths.
            if key in PROJECT_STATIC_METADATA:
                if value != PROJECT_STATIC_METADATA[key]:
                    raise ProvenanceError(
                        f"Unexpected CMake project metadata value: {key}")
            elif key not in PROJECT_STATIC_PATH_KEYS:
                raise ProvenanceError(f"Unexpected STATIC CMake cache key: {key}")
        elif typ not in ("BOOL", "STRING", "PATH", "FILEPATH", "INTERNAL",
                         "UNINITIALIZED"):
            raise ProvenanceError(f"Unexpected CMake cache type: {key}")
        if ((key in PROJECT_STATIC_METADATA or
             key in PROJECT_STATIC_PATH_KEYS) and typ != "STATIC"):
            raise ProvenanceError(
                f"CMake-generated {key} cache type must be STATIC")
        if key in entries:
            raise ProvenanceError(f"Duplicate CMake cache value: {key}")
        entries[key] = value
    return entries


def require(cache: dict[str, str], key: str, expected: str | None = None) -> str:
    if key not in cache:
        raise ProvenanceError(f"Missing private CMake cache key: {key}")
    value = cache[key]
    if expected is not None and value != expected:
        raise ProvenanceError(f"Unexpected {key}; private configuration is not matched")
    return value


def patchset_hash(source: Path) -> str:
    directory = source / "patches/xrdp"
    series = directory / "series"
    if not series.is_file() or series.is_symlink():
        raise ProvenanceError("Missing or linked patch series")
    members = [series]
    for raw in series.read_text(encoding="utf-8").splitlines():
        name = raw.strip()
        if not name or name.startswith("#"):
            continue
        relative = Path(name)
        if (relative.is_absolute() or ".." in relative.parts or
                "\\" in name or relative == Path(".")):
            raise ProvenanceError("Unsafe ordered patch series member")
        member = directory / relative
        if (not member.is_file() or
                any(p.is_symlink() for p in (member, *member.parents)
                    if within(p, directory)) or
                not within(member.resolve(strict=True), directory)):
            raise ProvenanceError(f"Missing, linked or external xrdp patch: {name}")
        members.append(member)
    if len(members) != 53:
        raise ProvenanceError("Expected the committed, ordered 52-patch stack")
    material = "".join(
        f"{p.relative_to(source).as_posix()}\n{sha(p.read_bytes())}\n"
        for p in members)
    return sha(material.encode("utf-8"))


def expected_state_hash(cache: dict[str, str], release: Path,
                        patch_hash: str, patch_script_hash: str) -> str:
    cflags = require(cache, "XRDP_CONSOLE_XRDP_CFLAGS")
    cppflags = require(cache, "XRDP_CONSOLE_XRDP_CPPFLAGS", "")
    ldflags = require(cache, "XRDP_CONSOLE_XRDP_LDFLAGS", "")
    pkgpath = require(cache, "XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH", "")
    options = [
        "<SOURCE_DIR>/configure",
        "--prefix=<INSTALL_DIR>",
        "--sysconfdir=<INSTALL_DIR>/etc",
        "--localstatedir=<INSTALL_DIR>/var",
        f"--runstatedir={release}/runstate",
        f"--with-socketdir={release}/socket-root",
        "--enable-strict-locations",
        "--enable-rfxcodec",
        "--enable-x264",
        "--enable-jpeg",
        "--enable-fuse",
        "--enable-ipv6",
        "--disable-vsock",
        "--disable-utmp",
        "--with-freetype2=yes",
        "--disable-neutrinordp",
    ]
    # Match xrdp_dependency.cmake's exact private-mode state-material order.
    configure_material = "\n".join(options)
    build_material = (
        f"CFLAGS={cflags}\nCPPFLAGS={cppflags}\nLDFLAGS={ldflags}\n"
        f"PKG_CONFIG_PATH={pkgpath}\ninstall-prefix=private-hash-keyed\n"
        f"configure-args={configure_material}\n"
        "profile=private-offline-only\n"
    )
    material = (
        f"version={EXPECTED_VERSION}\nurl={EXPECTED_URL}\n"
        f"archive={EXPECTED_ARCHIVE}\npatchset={patch_hash}\n"
        f"patch-script={patch_script_hash}\n{build_material}"
    )
    return sha(material.encode("utf-8"))


def verify_private_compiler_flags(cache: dict[str, str]) -> None:
    """Validate even an internally self-consistent CMake cache.

    The hash calculation must remain pure: an attacker can recompute the
    hash over unsafe -I/-L/rpath inputs. A matching hash is not a policy pass.
    """
    cflags = require(cache, "XRDP_CONSOLE_XRDP_CFLAGS")
    try:
        tokens = shlex.split(cflags)
    except ValueError as exc:
        raise ProvenanceError("Unparseable private compiler flags") from exc
    if not tokens or any(re.fullmatch(
            r"(-O[0-3sgz]|-g[0-9]?|-march=[A-Za-z0-9._+-]+|"
            r"-mtune=[A-Za-z0-9._+-]+|-fno-omit-frame-pointer|-fPIC|"
            r"-fPIE|-W[a-zA-Z0-9-]+)", token) is None for token in tokens):
        raise ProvenanceError("Private CFLAGS contain unreviewed compiler inputs")


def inspect_config(source: Path, build: Path, release: Path,
                   cache: dict[str, str]) -> dict[str, Any]:
    if release.name != "xrdp-console" or release.parent.name != ".release":
        raise ProvenanceError("Release root must be .release/xrdp-console")
    if not within(source, release) or not within(build, release):
        raise ProvenanceError("Private source/build must be inside release root")
    require(cache, "CMAKE_HOME_DIRECTORY", str(source))
    require(cache, "CMAKE_CACHEFILE_DIR", str(build))
    # Cache dictionaries constructed without read_cache() must obey the
    # same exact generated-project metadata constraints. Older CMake versions
    # can legitimately omit generated entries.
    for key, expected in PROJECT_STATIC_METADATA.items():
        if key in cache and cache[key] != expected:
            raise ProvenanceError(
                f"Unexpected top-level CMake project metadata: {key}")
    project_paths = {
        "xrdp_console_BINARY_DIR": build,
        "xrdp_console_SOURCE_DIR": source,
    }
    for key, expected in project_paths.items():
        if key in cache:
            # Comparing raw absolute paths first prevents reading an external
            # path. canonical_directory then disallows symlink ancestors.
            if Path(cache[key]) != expected:
                raise ProvenanceError(f"CMake project path escapes build: {key}")
            canonical_directory(cache[key], label=key)
    # Accept only the exact CMake-generated redirects directory for
    # this private build, and reject external, traversing or symlink paths.
    # Older CMake versions may omit this metadata entirely.
    redirects_value = cache.get("CMAKE_FIND_PACKAGE_REDIRECTS_DIR")
    if redirects_value is not None:
        expected_redirects = build / "CMakeFiles" / "pkgRedirects"
        # Reject external paths before probing them for existence.
        if Path(redirects_value) != expected_redirects:
            raise ProvenanceError(
                "CMake package redirects escape the current private build")
        canonical_directory(
            redirects_value, label="CMake package redirects directory")
    require(cache, "XRDP_CONSOLE_BUILD_XRDP", "ON")
    require(cache, "XRDP_CONSOLE_PRIVATE_XRDP_BUILD", "ON")
    require(cache, "XRDP_CONSOLE_PRIVATE_RELEASE_ROOT", str(release))
    require(cache, "XRDP_CONSOLE_XRDP_CONFIGURED_PROFILE", "private-offline-only")
    require(cache, "XRDP_CONSOLE_XRDP_VERSION", EXPECTED_VERSION)
    require(cache, "XRDP_CONSOLE_XRDP_SOURCE_URL", EXPECTED_URL)
    require(cache, "XRDP_CONSOLE_XRDP_SOURCE_SHA256", EXPECTED_ARCHIVE)
    deps = canonical_directory(
        require(cache, "XRDP_CONSOLE_XRDP_DEPS_ROOT"), label="dependency root")
    if not within(deps, release) or deps == release or deps == build:
        raise ProvenanceError("Private dependencies escape or collide with build")
    patch_hash = patchset_hash(source)
    require(cache, "XRDP_CONSOLE_XRDP_PATCHSET_HASH", patch_hash)
    patch_script = source / "cmake/apply_xrdp_patches.cmake"
    if not patch_script.is_file() or patch_script.is_symlink():
        raise ProvenanceError("Missing or linked patch replay script")
    full_state = expected_state_hash(
        cache, release, patch_hash, sha(patch_script.read_bytes()))
    require(cache, "XRDP_CONSOLE_XRDP_STATE_HASH", full_state)
    verify_private_compiler_flags(cache)
    tag = full_state[:16]
    install = Path(require(cache, "XRDP_CONSOLE_XRDP_INSTALL_DIR"))
    source_dir = Path(require(cache, "XRDP_CONSOLE_XRDP_SOURCE_DIR"))
    xrdp_build_dir = Path(require(cache, "XRDP_CONSOLE_XRDP_BUILD_DIR"))
    for label, path, expected in (
            ("install", install, deps / f"xrdp-install-{tag}"),
            ("patched source", source_dir, deps / f"xrdp-src-{tag}"),
            ("dependency build", xrdp_build_dir, deps / f"xrdp-build-{tag}")):
        if path != expected:
            raise ProvenanceError(f"Hash-keyed {label} directory mismatch")
        for parent in (path, *path.parents):
            if parent.is_symlink():
                raise ProvenanceError(f"Hash-keyed {label} path contains symlink")
    return {
        "private_build_config_verified": True,
        "patchset_hash": patch_hash,
        "state_hash": full_state,
        "install_prefix": str(install),
        "private_runstate": str(release / "runstate"),
        "private_socket_root": str(release / "socket-root"),
        "source_dir": str(source_dir),
        "dependency_build_dir": str(xrdp_build_dir),
        "upstream_archive_sha256": EXPECTED_ARCHIVE,
    }


def git_check(source: Path) -> str:
    # No fetch/checkout/status locks or git hooks. The dedicated private
    # worktree must have neither modified *nor untracked* build inputs.
    env = {
        "PATH": "/usr/bin:/bin", "LC_ALL": "C",
        "HOME": "/nonexistent",
        "GIT_OPTIONAL_LOCKS": "0",
        "GIT_NO_REPLACE_OBJECTS": "1",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": "/dev/null",
    }
    prefix = ["git", "-c", "core.fsmonitor=false",
              "-c", "core.hooksPath=/dev/null", "-C", str(source)]
    def git(*args: str, good=(0,)) -> str:
        result = subprocess.run(
            [*prefix, *args], capture_output=True, text=True,
            timeout=15, check=False, env=env)
        if result.returncode not in good:
            raise ProvenanceError(f"Git inspection failed: {args[0]}")
        return result.stdout.strip()
    if Path(git("rev-parse", "--show-toplevel")) != source:
        raise ProvenanceError("Git checkout root differs from attested source")
    commit = git("rev-parse", "HEAD")
    if COMMIT.fullmatch(commit) is None:
        raise ProvenanceError("Git HEAD is not a full source commit")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise ProvenanceError("Source has tracked or untracked changes; no receipt")
    git("merge-base", "--is-ancestor", CORRECTED_CHANSRV_ANCESTOR, "HEAD")
    return commit


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Read-only Git/patch/CMake private xrdp build provenance")
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--release", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        release = canonical_directory(args.release, label="release")
        source = canonical_directory(args.source, label="source")
        build = canonical_directory(args.build, label="build")
        cachefile = build / "CMakeCache.txt"
        if not cachefile.is_file() or cachefile.is_symlink():
            raise ProvenanceError("Missing canonical configured CMakeCache.txt")
        verified = inspect_config(
            source, build, release,
            read_cache(cachefile.read_text(encoding="utf-8")))
        verified.update({
            "source_commit_from_git": git_check(source),
            "corrected_chansrv_ancestor": CORRECTED_CHANSRV_ANCESTOR,
            "mode": "READ_ONLY_CONFIGURATION_PROVENANCE",
            "runtime_authorized": False,
            "binary_build_or_elf_verified": False,
            "remaining_gates": [
                "Native compile/link transcript and actual installed ELF hashes",
                "Applied zero-fuzz patch replay transcript",
                "Private dynamic-loader, modules and runstate/IPC closure",
                "Private Xvfb auth, RDP endpoint and Firefox acceptance",
                "Explicit authorization required for any future runtime",
            ],
        })
        print(json.dumps(verified, sort_keys=True, indent=2))
    except (ProvenanceError, OSError, ValueError, UnicodeError,
            subprocess.SubprocessError) as exc:
        print(json.dumps({
            "mode": "READ_ONLY_CONFIGURATION_PROVENANCE",
            "runtime_authorized": False, "error": str(exc)}, sort_keys=True))
    return 2  # Strictly a non-authorizing read-only evidence gate.


if __name__ == "__main__":
    raise SystemExit(main())
