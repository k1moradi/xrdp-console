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
from typing import Any, Callable, Protocol, Sequence

from firefox_chansrv_consumer import classify_receipt

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
    try:
        canonical = path.resolve(strict=must_exist)
    except OSError as exc:
        raise UnsafePlan(f"{label}: required private path unavailable") from exc
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


def _valid_binary_role(name: str, spec: dict[str, Any], root: Path,
                       *, executable: bool = True) -> dict[str, str]:
    if not isinstance(spec, dict):
        raise UnsafePlan(f"{name}: missing binary specification")
    path = _private_path(_manifest_value(spec, "path", str), root,
                         name, must_exist=True)
    if not path.is_file() or (executable and not os.access(path, os.X_OK)):
        raise UnsafePlan(f"{name}: executable/artifact file unavailable")
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


def _validate_private_rdp_endpoint(
        endpoint: dict[str, Any], release: Path,
        run_root: Path, source_display: str
        ) -> dict[str, Any]:
    """Offline description only; no bind, connect, spawn or listener attestation."""
    if not isinstance(endpoint, dict):
        raise UnsafePlan("Missing private C-leg RDP endpoint contract")
    if _manifest_value(endpoint, "bind_address", str) != "127.0.0.1":
        raise UnsafePlan("C-leg requires an explicit private loopback listener")
    port = _manifest_value(endpoint, "port", int)
    if not 1025 <= port <= 65535:
        raise UnsafePlan("C-leg listener port must be nonprivileged")
    if _manifest_value(endpoint, "session_route", str) != "external-chansrv":
        raise UnsafePlan("C-leg must not invent a sesman session")
    client_display = _manifest_value(endpoint, "client_display", str)
    if (not PRIVATE_DISPLAY.fullmatch(client_display) or
            client_display == source_display):
        raise UnsafePlan("C-leg client X11 display must be distinct and private")
    client_authority = _private_path(
        _manifest_value(endpoint, "client_xauthority", str),
        run_root, "client_xauthority")
    if (not client_authority.is_file() or
            client_authority.stat().st_mode & 0o077):
        raise UnsafePlan("C-leg client Xauthority is missing or not private")
    config_path = _private_path(
        _manifest_value(endpoint, "config_path", str),
        run_root, "xrdp_config")
    config_sha = _manifest_value(endpoint, "config_sha256", str)
    if (not config_path.is_file() or
            SHA256.fullmatch(config_sha) is None or
            _sha256(config_path) != config_sha):
        raise UnsafePlan("Private xrdp configuration hash mismatch")
    bins = _manifest_value(endpoint, "artifacts", dict)
    if set(bins) != {"xrdp", "module", "peer", "rdp_client"}:
        raise UnsafePlan("Missing private xrdp/module/peer/client artifact identity")
    artifacts = {
        role: _valid_binary_role(role, record, release,
                                 executable=(role != "module"))
        for role, record in bins.items()
    }
    if len({item["path"] for item in artifacts.values()}) != len(artifacts):
        raise UnsafePlan("C-leg artifacts cannot share executable identities")
    if artifacts["xrdp"]["source_commit"] != CHANSRV_SOURCE_REF:
        raise UnsafePlan("Private xrdp must share the corrected chansrv source stack")
    display_number = int(source_display[1:])
    expected_route = f"DISPLAY({display_number},{os.getuid()})"
    if _manifest_value(endpoint, "chansrvport", str) != expected_route:
        raise UnsafePlan("C-leg chansrvport must match private owner display and UID")
    return {
        "bind_address_requested": "127.0.0.1",
        "port_requested": port,
        "listener_verified": False,
        "config_sha256": config_sha,
        "session_route": "external-chansrv",
        "chansrvport": expected_route,
        "client_display": client_display,
        "client_auth_private": True,
        "artifacts": artifacts,
        "runtime_authorized": False,
    }


def review_manifest(spec: dict[str, Any]) -> dict[str, Any]:
    """Read-only, strict preflight. Never authorizes runtime execution."""
    if not isinstance(spec, dict) or spec.get("schema") not in (1, 2):
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

    schema = spec["schema"]
    binding = _manifest_value(spec, "rdp_listener", str)
    if schema == 1:
        if binding != "disabled":
            raise UnsafePlan("Legacy offline controller permits no RDP listener")
        endpoint = None
    else:
        if binding != "private-loopback-unverified":
            raise UnsafePlan("C-leg needs a private loopback endpoint contract")
        endpoint = _validate_private_rdp_endpoint(
            _manifest_value(spec, "private_rdp_endpoint", dict),
            release, run_root, display)

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
        "rdp_listener": binding,
        "private_rdp_endpoint": endpoint,
        "host_gates": [
            "C-leg requires a separate private RDP virtual-channel endpoint; "
            "schema 1 has no C-leg route" if endpoint is None else
            "C-leg private loopback listener and chansrvport runtime checks absent",
            "Private RDP module, channel forwarding and synthetic CLIPRDR peer "
            "not started or attested",
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
    def is_running(self) -> bool: ...


@dataclass(frozen=True)
class CaseResult:
    leg: str
    generation: int
    status: str
    receipt: dict[str, Any] | None
    classification: str


def compare_file_acceptance(results: Sequence[CaseResult]) -> dict[str, Any]:
    """Compare only three *observed* ordered legs; return a hypothesis, not proof.

    Native X11/CLIPRDR cause attribution requires separate runtime evidence.
    This offline function never infers Firefox XIDs, timeouts, or byte paths.
    """
    if (len(results) != len(LEGS) or
            [r.leg for r in results] != list(LEGS) or
            any(type(r.generation) is not int for r in results) or
            any(a.generation >= b.generation
                for a, b in zip(results, results[1:]))):
        raise UnsafePlan("A/B/C comparison requires three ordered fresh generations")
    # Recompute from the actual retained metadata. A manually constructed
    # CaseResult with status="accepted" must never launder failed File evidence.
    for r in results:
        if (not isinstance(r.receipt, dict) or
                r.receipt.get("phase") != "complete" or
                r.receipt.get("trusted") is not True or
                r.receipt.get("source") != "paste"):
            raise UnsafePlan("A/B/C comparison requires a completed trusted paste")
        actual = classify_receipt(
            r.receipt, FIXTURE_SIZE, FIXTURE_SHA256, FIXTURE_DIMS,
            require_exact_png_encoding=(r.leg == "chansrv"))
        required = ("READABLE_PNG_FILE" if r.leg == "chansrv"
                    else "READABLE_PNG_FILE_VALIDATED_IMAGE")
        expected_status = ("image-file-accepted" if actual == required
                           else "image-file-rejected")
        if r.classification != actual or r.status != expected_status:
            raise UnsafePlan("Claimed image-file status disagrees with browser receipt")
    accepted = [r.status == "image-file-accepted" for r in results]
    labels = [r.classification for r in results]
    if not accepted[0]:
        return {"outcome": "REFERENCE_CONTROL_FAILED",
                "confidence": "inconclusive", "classifications": labels}
    if accepted == [True, True, False]:
        outcome = "REMOTE_OWNER_PATH_SUSPECT"
    elif accepted == [True, False, False]:
        outcome = "QT_PIXMAP_MIME_CONVERSION_SUSPECT"
    elif accepted == [True, True, True]:
        outcome = "SYNTHETIC_FILE_ACCEPTANCE_CONFIRMED"
    else:
        outcome = "MIXED_RESULTS_INCONCLUSIVE"
    return {"outcome": outcome, "confidence": "hypothesis",
            "classifications": labels}


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
        self._experiment_invalid = False

    def run_case(self, leg: str, generation: int,
                 owner_factory: Callable[[], OwnerPort],
                 browser_factory: Callable[[], BrowserPort]) -> CaseResult:
        if self._unsafe_cleanup or self._active:
            raise UnsafePlan("An owner is active or cleanup is unverified")
        if self._experiment_invalid:
            raise UnsafePlan("Browser event or reference control invalid; start a new trial")
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
                self._experiment_invalid = True
                raise UnsafePlan("Trusted browser paste receipt incomplete")
            # The screenshot paste success gate is the actual synchronous
            # DataTransferItem.getAsFile() -> readable decoded PNG File.
            # The Qt owners may re-encode; chansrv must preserve source bytes.
            classification = classify_receipt(
                receipt, FIXTURE_SIZE, FIXTURE_SHA256, FIXTURE_DIMS,
                require_exact_png_encoding=(leg == "chansrv"))
            if classification == "INVALID_UNTRUSTED_EVENT":
                self._experiment_invalid = True
                raise UnsafePlan("Missing trusted Firefox paste event")
            good = ("READABLE_PNG_FILE" if leg == "chansrv"
                    else "READABLE_PNG_FILE_VALIDATED_IMAGE")
            status = ("image-file-accepted" if classification == good
                      else "image-file-rejected")
            result = CaseResult(leg, generation, status, receipt, classification)
        except BaseException as exc:
            pending_error = exc
        finally:
            for port in (browser, owner):
                if port is not None:
                    try:
                        port.stop()
                        if port.is_running():
                            raise UnsafePlan("Child still alive after requested teardown")
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
        # A failing ScreenGrab reference invalidates the remaining
        # comparison; running more owners cannot isolate the root cause.
        if leg == "qt-pixmap" and result.status != "image-file-accepted":
            self._experiment_invalid = True
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
