#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail-closed offline readiness and lifecycle core for Qt/chansrv Firefox A/B/C.

IMPORTANT: This executable NEVER spawns or connects to Xvfb, Firefox, Qt,
chansrv, xrdp, or CLIPRDR.  It has no live execution backend or --execute
escape hatch. Its in-memory case orchestration accepts *injected* test doubles.
Codex's host-specific private dependency and socket contracts must be proven
before a future separate implementation can attach a real backend.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
from typing import Any, Callable, Protocol

# Product and test identity are deliberately not conflated. The source used
# for the staged native chansrv must include the corrected committed 0041.
CHANSRV_SOURCE_REF = "447447ff68fe34cf2391ca9c908c4bd993ed4ef2"
FIXTURE_SHA256 = "d9b7864e95e934ee999ee333ce9bf86adcf823aaca271634bafb8d8b9d3f6c22"
FIXTURE_SIZE = 2_401_598
FIXTURE_DIMS = (1000, 800)
LEGS = ("qt-pixmap", "qt-png-only", "chansrv")
SHA256 = re.compile(r"[a-f0-9]{64}\Z")
PRIVATE_DISPLAY = re.compile(r":(19[1-9]|2[0-4][0-9])\Z")
SHARED_RESOURCES = ("/run/xrdp", "/var/run/xrdp", "/tmp/.X11-unix/X0")
ENVIRONMENT_FORBIDDEN = (
    "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR",
    "SESSION_MANAGER", "SSH_AUTH_SOCK", "LD_PRELOAD", "LD_LIBRARY_PATH",
    "XRDP_SOCKET_PATH", "XRDP_SOCKET_DIR", "XRDP_SESMAN_SOCKET_DIR",
)


class UnsafePlan(RuntimeError):
    """A safety gate failed: do not start any test process."""


def _inside(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _private_path(value: str, root: Path, label: str, *,
                  must_exist: bool = True) -> Path:
    """Reject symlinks, parent traversals and paths outside the owned release."""
    path = Path(value)
    if not path.is_absolute() or ".." in path.parts:
        raise UnsafePlan(f"{label}: absolute non-traversing path required")
    # An existing symlink at any component is prohibited, not merely the
    # final canonical path, including symlinked directories.
    for candidate in (path, *path.parents):
        if candidate.is_symlink():
            raise UnsafePlan(f"{label}: symlinked component")
    canonical = path.resolve(strict=must_exist)
    if not _inside(canonical, root):
        raise UnsafePlan(f"{label}: path escapes private release root")
    if must_exist and canonical.stat().st_uid != os.geteuid():
        raise UnsafePlan(f"{label}: unexpected file owner")
    return canonical


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _manifest_value(spec: dict[str, Any], key: str, expected_type: type) -> Any:
    value = spec.get(key)
    if type(value) is not expected_type:
        raise UnsafePlan(f"{key}: missing or invalid value")
    return value


def scrub_child_environment(private_root: Path, display: str,
                            authority: Path, home: Path) -> dict[str, str]:
    """Never copy os.environ. Always supply a minimal owned child environment."""
    if not PRIVATE_DISPLAY.fullmatch(display):
        raise UnsafePlan("Private display must be :191 through :249")
    for item, name in ((authority, "XAUTHORITY"), (home, "HOME")):
        if not _inside(item, private_root):
            raise UnsafePlan(f"{name} escapes private release root")
    if not authority.is_file() or authority.stat().st_uid != os.geteuid():
        raise UnsafePlan("Private Xauthority absent or wrong owner")
    if authority.stat().st_mode & 0o077:
        raise UnsafePlan("Xauthority permissions must exclude group and others")
    if not home.is_dir() or home.stat().st_uid != os.geteuid():
        raise UnsafePlan("Private HOME absent or wrong owner")
    return {
        "PATH": "/usr/bin:/bin",
        "LANG": "C.UTF-8",
        "HOME": str(home),
        "DISPLAY": display,
        "XAUTHORITY": str(authority),
        "QT_QPA_PLATFORM": "xcb",
        "XRDP_CONSOLE_RELEASE_ROOT": str(private_root),
    }


def _validate_fixed_fixture(fixture: Path) -> None:
    if not fixture.is_file() or fixture.stat().st_size != FIXTURE_SIZE:
        raise UnsafePlan("Approved synthetic PNG size mismatch")
    if _sha256(fixture) != FIXTURE_SHA256:
        raise UnsafePlan("Approved synthetic PNG SHA-256 mismatch")
    with fixture.open("rb") as stream:
        header = stream.read(24)
    if header[:8] != b"\x89PNG\r\n\x1a\n":
        raise UnsafePlan("Approved synthetic PNG signature invalid")
    if (int.from_bytes(header[16:20], "big"),
            int.from_bytes(header[20:24], "big")) != FIXTURE_DIMS:
        raise UnsafePlan("Approved synthetic PNG dimensions mismatch")


def _valid_binary_role(name: str, spec: dict[str, Any], root: Path) -> dict[str, str]:
    if not isinstance(spec, dict):
        raise UnsafePlan(f"{name}: missing binary specification")
    path = _private_path(_manifest_value(spec, "path", str), root,
                         name, must_exist=True)
    if not path.is_file() or not os.access(path, os.X_OK):
        raise UnsafePlan(f"{name}: executable file unavailable")
    expected_sha = _manifest_value(spec, "sha256", str)
    if SHA256.fullmatch(expected_sha) is None or _sha256(path) != expected_sha:
        raise UnsafePlan(f"{name}: executable checksum mismatch")
    source = _manifest_value(spec, "source_commit", str)
    if re.fullmatch(r"[0-9a-f]{40}", source) is None:
        raise UnsafePlan(f"{name}: invalid pinned commit")
    if name == "chansrv" and source != CHANSRV_SOURCE_REF:
        raise UnsafePlan("chansrv must be built from committed corrected patch stack")
    # Host ELF runtime closure is not proven by a binary hash or readelf alone.
    # Existing protected-prefix build libraries may not be used for runtime.
    return {"path": str(path), "sha256": expected_sha, "source_commit": source}


def review_manifest(spec: dict[str, Any]) -> dict[str, Any]:
    """Read-only, strict preflight. Never authorizes runtime execution."""
    if not isinstance(spec, dict) or spec.get("schema") != 1:
        raise UnsafePlan("Unrecognized controller manifest schema")
    release = Path(_manifest_value(spec, "release_root", str))
    if not release.is_absolute() or ".." in release.parts or release.is_symlink():
        raise UnsafePlan("Invalid release root")
    release = release.resolve(strict=True)
    if release.name != ".release" or release.stat().st_uid != os.geteuid():
        raise UnsafePlan("Release root must be caller-owned .release")
    if not release.is_dir():
        raise UnsafePlan("Release root must be a directory")

    run_root = _private_path(
        _manifest_value(spec, "run_root", str), release, "run_root")
    if run_root == release or not run_root.is_dir():
        raise UnsafePlan("A dedicated existing private run directory is required")
    if run_root.stat().st_mode & 0o077:
        raise UnsafePlan("Private run directory must be mode 0700")
    authority = _private_path(
        _manifest_value(spec, "xauthority", str), run_root, "xauthority")
    home = _private_path(
        _manifest_value(spec, "home", str), run_root, "home")
    sockets = _private_path(
        _manifest_value(spec, "socket_dir", str), run_root, "socket_dir")
    if not sockets.is_dir() or sockets.stat().st_mode & 0o077:
        raise UnsafePlan("Sockets must be in a caller-owned mode-0700 directory")
    if run_root in (authority, home, sockets):
        raise UnsafePlan("Distinct private authority/home/socket paths required")

    display = _manifest_value(spec, "display", str)
    env = scrub_child_environment(release, display, authority, home)

    fixture = _private_path(
        _manifest_value(spec, "fixture", str), run_root, "fixture")
    _validate_fixed_fixture(fixture)

    bins = _manifest_value(spec, "private_binaries", dict)
    if set(bins) != {"qt_owner", "chansrv"}:
        raise UnsafePlan("Require exact Qt and chansrv private binary roles")
    artifacts = {
        role: _valid_binary_role(
            "chansrv" if role == "chansrv" else "qt_owner",
            data, release)
        for role, data in bins.items()
    }
    if artifacts["qt_owner"]["path"] == artifacts["chansrv"]["path"]:
        raise UnsafePlan("Owner binaries must be distinct")

    binding = _manifest_value(spec, "rdp_listener", str)
    if binding != "disabled":
        raise UnsafePlan("RDP is not required/authorized for this controller")

    # Explicit report of evidence the host operator must supply. A manifest
    # cannot self-attest a process PID, an ELF library closure, or X11 identity.
    # These are hard blockers even when local file preflight succeeds.
    return {
        "mode": "OFFLINE_ONLY",
        "runtime_authorized": False,
        "source_patch_commit": CHANSRV_SOURCE_REF,
        "display": display,
        "fixture": {"sha256": FIXTURE_SHA256, "bytes": FIXTURE_SIZE,
                    "width": FIXTURE_DIMS[0], "height": FIXTURE_DIMS[1]},
        "cases": list(LEGS),
        "private_binaries": artifacts,
        "child_environment_keys": sorted(env),
        "rdp_listener": "disabled",
        "host_gates": [
            "Xvfb process PID, arguments, socket and cookie must be attested",
            "Private chansrv sockets and full ELF dependency closure unverified",
            "Chansrv peer/session handshake and isolated startup unverified",
            "Geckodriver/Firefox startup and requestor PID evidence unverified",
            "Independent Qt and chansrv TARGETS/owner observation unverified",
            "Explicit authorization for private synthetic runtime not granted",
        ],
    }


class OwnerPort(Protocol):
    """A future host backend must implement this *injection-only* port."""
    def wait_ready(self, timeout_seconds: int) -> bool: ...
    def stop(self) -> None: ...
    def is_running(self) -> bool: ...


class BrowserPort(Protocol):
    def wait_ready(self, timeout_seconds: int) -> bool: ...
    def trusted_paste(self, timeout_seconds: int) -> dict[str, Any]: ...
    def stop(self) -> None: ...


@dataclass(frozen=True)
class CaseResult:
    leg: str
    generation: int
    status: str
    receipt: dict[str, Any] | None


class CaseCoordinator:
    """Mock-exercised case state; no OS handles, sockets or process launch.

    The real process backend does not exist until Codex closes the host
    blockers and the user separately authorizes a private test.
    """
    def __init__(self) -> None:
        self._next_leg = 0
        self._last_generation = -1
        self._active = False
        self._unsafe_cleanup = False

    def run_case(self, leg: str, generation: int,
                 owner_factory: Callable[[], OwnerPort],
                 browser_factory: Callable[[], BrowserPort]) -> CaseResult:
        if self._unsafe_cleanup or self._active:
            raise UnsafePlan("An owner is active or cleanup is unverified")
        if self._next_leg >= len(LEGS) or leg != LEGS[self._next_leg]:
            raise UnsafePlan("Owners must run once, in pixmap/PNG-only/chansrv order")
        if type(generation) is not int or generation <= self._last_generation:
            raise UnsafePlan("Each leg requires a fresh, increasing generation")

        owner: OwnerPort | None = None
        browser: BrowserPort | None = None
        self._active = True
        result: CaseResult | None = None
        pending_error: BaseException | None = None
        try:
            owner = owner_factory()
            if not owner.wait_ready(10) or not owner.is_running():
                raise UnsafePlan("Owner exited or was never ready")
            browser = browser_factory()
            if not browser.wait_ready(20):
                raise UnsafePlan("Browser not ready while owner was live")
            if not owner.is_running():
                raise UnsafePlan("Clipboard owner exited before paste")
            receipt = browser.trusted_paste(20)
            if not owner.is_running():
                raise UnsafePlan("Clipboard owner exited before receipt")
            if not isinstance(receipt, dict) or receipt.get("phase") != "complete":
                raise UnsafePlan("Trusted browser paste receipt incomplete")
            # File acceptance is deliberately NOT inferred from just phase.
            status = ("trusted-paste-observed"
                      if receipt.get("trusted") is True else
                      "untrusted-paste-inconclusive")
            result = CaseResult(leg, generation, status, receipt)
        except BaseException as exc:
            pending_error = exc
        finally:
            for port in (browser, owner):
                if port is not None:
                    try:
                        port.stop()
                    except BaseException as exc:
                        self._unsafe_cleanup = True
                        if pending_error is None:
                            pending_error = UnsafePlan(
                                "Unable to verify teardown of child owner/browser")
                            pending_error.__cause__ = exc
            self._active = False
        if pending_error is not None:
            raise pending_error
        assert result is not None
        self._last_generation = generation
        self._next_leg += 1
        return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Read-only private Firefox clipboard A/B/C readiness review")
    parser.add_argument("--manifest", type=Path, required=True,
                        help="Existing, owned local JSON manifest (never modified)")
    args = parser.parse_args(argv)
    try:
        report = review_manifest(json.loads(args.manifest.read_text(encoding="utf-8")))
        print(json.dumps(report, indent=2, sort_keys=True))
        return 2  # Blocked by design: the runtime backend does not exist.
    except (UnsafePlan, OSError, ValueError, json.JSONDecodeError) as exc:
        print(json.dumps({"mode": "OFFLINE_ONLY", "runtime_authorized": False,
                          "error": str(exc)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
