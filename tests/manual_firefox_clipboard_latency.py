#!/usr/bin/env python3
"""Run one bounded Firefox clipboard paste diagnostic.

Synthetic mode starts a private Xvfb and staged X11 PNG owner. Consumer-only
mode launches a fresh geckodriver-managed Firefox on the existing DISPLAY and
uses the current X11 clipboard without changing its owner. Neither mode
attaches to the user's existing Firefox profile.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import select
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, BinaryIO

CURRENT_CLIPBOARD_OBSERVATION_SECONDS = 30.0


class WebDriver:
    def __init__(self, base_url: str) -> None:
        self.base_url = base_url

    def command(self, method: str, path: str,
                payload: dict[str, Any] | None = None) -> dict[str, Any]:
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            self.base_url + path,
            data=data,
            headers={"Content-Type": "application/json"},
            method=method)
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                body = response.read()
        except urllib.error.HTTPError as error:
            body = error.read()
        parsed = json.loads(body.decode("utf-8"))
        value = parsed.get("value", {})
        if isinstance(value, dict) and value.get("error"):
            raise RuntimeError(
                f"WebDriver {method} {path} failed: {value}")
        return parsed


def read_line(stream: BinaryIO, timeout: float) -> str:
    ready, _, _ = select.select([stream], [], [], timeout)
    if not ready:
        raise TimeoutError("timed out waiting for Xvfb display number")
    line = stream.readline()
    if not line:
        raise RuntimeError("Xvfb exited before reporting a display")
    return line.decode("ascii", errors="replace").strip()


def wait_for_marker(stream: BinaryIO, marker: bytes,
                    timeout: float) -> list[str]:
    deadline = time.monotonic() + timeout
    lines: list[str] = []
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        ready, _, _ = select.select([stream], [], [], remaining)
        if not ready:
            break
        line = stream.readline()
        if not line:
            break
        decoded = line.decode("utf-8", errors="replace").rstrip()
        lines.append(decoded)
        if marker in line:
            return lines
    raise TimeoutError(
        f"timed out waiting for owner marker {marker!r}; lines={lines!r}")


def choose_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def find_firefox_binary() -> str | None:
    candidates = (
        "/usr/lib/firefox/firefox",
        "/snap/firefox/current/usr/lib/firefox/firefox",
        shutil.which("firefox"),
    )
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def wait_for_webdriver(driver: WebDriver, process: subprocess.Popen[bytes],
                       timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("geckodriver exited before becoming ready")
        try:
            driver.command("GET", "/status")
            return
        except (OSError, TimeoutError, json.JSONDecodeError):
            time.sleep(0.05)
    raise TimeoutError("geckodriver did not become ready")


def event_has_valid_png_file(event: dict[str, Any]) -> bool:
    if event.get("trusted") is not True:
        return False
    items = event.get("items", [])
    if not isinstance(items, list):
        return False
    for item in items:
        if not isinstance(item, dict) or item.get("kind") != "file" or \
                item.get("type") != "image/png":
            continue
        file_info = item.get("file")
        if not isinstance(file_info, dict):
            continue
        readback = file_info.get("readback")
        if (isinstance(readback, dict) and
                isinstance(readback.get("bytes"), int) and
                readback["bytes"] > 0 and
                readback.get("pngSignature") == "89504e470d0a1a0a"):
            return True
    return False


def paste_events_have_valid_png(events: object) -> bool:
    return isinstance(events, list) and any(
        isinstance(event, dict) and event_has_valid_png_file(event)
        for event in events)


def paste_events_have_pending_file_delivery(events: object) -> bool:
    """Return true while a paste advertises Files without terminal file data."""
    if not isinstance(events, list):
        return False
    for event in events:
        if not isinstance(event, dict):
            continue
        types = event.get("types", [])
        items = event.get("items", [])
        if not isinstance(types, list):
            types = []
        if not isinstance(items, list):
            items = []
        file_items = [
            item for item in items
            if isinstance(item, dict) and item.get("kind") == "file"
        ]
        if "Files" in types and not file_items:
            return True
        for item in file_items:
            file_info = item.get("file")
            if not isinstance(file_info, dict):
                return True
            if file_info.get("readback") == "pending":
                return True
    return False


def observation_window_seconds(consume_current: bool, delay_ms: int) -> float:
    minimum = (CURRENT_CLIPBOARD_OBSERVATION_SECONDS
               if consume_current else 10.0)
    return max(minimum, delay_ms / 1000.0 + 5.0)


def browser_page_url(page: Path, consume_current: bool) -> str:
    """Return an in-memory probe URL reachable by Snap-confined Firefox.

    Both consumer and synthetic modes embed the same static local probe
    document in a data: URL. This keeps page loading independent of the host
    path used by a detached worktree and avoids a file:// namespace confound.
    """
    del consume_current
    document = page.read_text(encoding="utf-8")
    return "data:text/html;charset=utf-8," + urllib.parse.quote(
        document, safe="")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("helper", type=Path, nargs="?")
    parser.add_argument("png", type=Path, nargs="?")
    parser.add_argument(
        "--consume-current", action="store_true",
        help="use the current DISPLAY clipboard without starting an X11 owner")
    parser.add_argument(
        "--expect-png", action="store_true",
        help="exit nonzero unless Firefox exposes a readable image/png file")
    parser.add_argument("--stage", choices=("before-notify", "after-notify"))
    parser.add_argument("--delivery", choices=("incr", "direct"),
                        default="incr")
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--press-count", type=int, default=1,
                        help="number of distinct Ctrl+V key presses (default: 1)")
    parser.add_argument("--press-interval-ms", type=int, default=250,
                        help="gap between presses, 0..10000 (default: 250)")
    parser.add_argument("--xvfb", type=Path, default=shutil.which("Xvfb"))
    parser.add_argument("--geckodriver", type=Path,
                        default=shutil.which("geckodriver"))
    parser.add_argument("--firefox", type=Path, default=find_firefox_binary())
    parser.add_argument("--page", type=Path,
                        default=Path(__file__).with_name(
                            "clipboard-paste-event-probe.html"))
    args = parser.parse_args()
    if not 0 <= args.delay_ms <= 60000:
        parser.error("--delay-ms must be in 0..60000")
    if not 1 <= args.press_count <= 8:
        parser.error("--press-count must be in 1..8")
    if not 0 <= args.press_interval_ms <= 10000:
        parser.error("--press-interval-ms must be in 0..10000")

    if args.consume_current:
        if args.helper is not None or args.png is not None:
            parser.error(
                "--consume-current does not accept the synthetic helper/PNG "
                "positional arguments")
        if args.stage is not None:
            parser.error("--consume-current does not use --stage")
        if not os.environ.get("DISPLAY"):
            parser.error("--consume-current requires DISPLAY in the environment")
    else:
        if args.helper is None or args.png is None or args.stage is None:
            parser.error(
                "synthetic mode requires HELPER PNG --stage and --delay-ms")
        if args.delivery == "direct" and args.stage != "before-notify":
            parser.error(
                "direct delivery is available only with before-notify delay")

    required_programs = ("geckodriver", "firefox") if args.consume_current else (
        "xvfb", "geckodriver", "firefox")
    for name in required_programs:
        if getattr(args, name) is None:
            parser.error(f"could not find {name}; pass its path explicitly")

    required_files = [args.page]
    if not args.consume_current:
        assert args.helper is not None
        assert args.png is not None
        required_files.extend((args.helper, args.png))
    for file_path in required_files:
        if not file_path.is_file():
            parser.error(f"file does not exist: {file_path}")

    xvfb: subprocess.Popen[bytes] | None = None
    owner: subprocess.Popen[bytes] | None = None
    geckodriver: subprocess.Popen[bytes] | None = None
    driver: WebDriver | None = None
    session_id: str | None = None
    owner_prefix: list[str] = []

    try:
        environment = os.environ.copy()
        environment["MOZ_ENABLE_WAYLAND"] = "0"

        if not args.consume_current:
            assert args.xvfb is not None
            assert args.helper is not None
            assert args.png is not None
            assert args.stage is not None
            xvfb = subprocess.Popen(
                [str(args.xvfb), "-displayfd", "1", "-screen", "0",
                 "1024x768x24", "-nolisten", "tcp", "-ac"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
            assert xvfb.stdout is not None
            display_number = read_line(xvfb.stdout, 5.0)
            environment["DISPLAY"] = f":{display_number}"

            if args.delivery == "direct":
                owner_mode = (
                    "owner-png-file-direct-xrdp-targets-prenotify-delay")
            elif args.stage == "before-notify":
                owner_mode = (
                    "owner-png-file-incr-xrdp-targets-prenotify-delay")
            else:
                owner_mode = "owner-png-file-incr-xrdp-targets-delay"
            owner = subprocess.Popen(
                [str(args.helper), owner_mode, str(args.png),
                 str(args.delay_ms)],
                env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, bufsize=0)
            assert owner.stdout is not None
            owner_prefix = wait_for_marker(
                owner.stdout, b"PNG_FILE_OWNER_READY", 10.0)
            if args.delivery == "direct" and not any(
                    "delivery=direct" in line for line in owner_prefix):
                raise RuntimeError(
                    "X server does not support direct delivery of this PNG; "
                    f"owner output={owner_prefix!r}")

        port = choose_port()
        geckodriver = subprocess.Popen(
            [str(args.geckodriver), "--host", "127.0.0.1",
             "--port", str(port), "--log", "fatal"],
            env=environment, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        driver = WebDriver(f"http://127.0.0.1:{port}")
        wait_for_webdriver(driver, geckodriver)
        created = driver.command("POST", "/session", {
            "capabilities": {
                "alwaysMatch": {
                    "browserName": "firefox",
                    "moz:firefoxOptions": {
                        "binary": str(args.firefox),
                    },
                },
            },
        })
        session = created.get("value", {})
        session_id = created.get("sessionId") or session.get("sessionId")
        if not session_id:
            raise RuntimeError(f"geckodriver returned no session ID: {created}")

        start = time.monotonic()
        driver.command("POST", f"/session/{session_id}/url", {
            "url": browser_page_url(args.page, args.consume_current),
        })
        element = driver.command("POST", f"/session/{session_id}/element", {
            "using": "css selector",
            "value": "#paste-target",
        }).get("value", {}).get("element-6066-11e4-a52e-4f735466cecf")
        if not element:
            raise RuntimeError("could not locate paste target in probe page")
        driver.command("POST", f"/session/{session_id}/element/{element}/click", {})
        time.sleep(0.2)
        start = time.monotonic()
        key_actions: list[dict[str, Any]] = []
        for press_index in range(args.press_count):
            key_actions.extend((
                {"type": "keyDown", "value": "\ue009"},
                {"type": "keyDown", "value": "v"},
                {"type": "keyUp", "value": "v"},
                {"type": "keyUp", "value": "\ue009"},
            ))
            if press_index + 1 < args.press_count:
                key_actions.append({
                    "type": "pause",
                    "duration": args.press_interval_ms,
                })
        driver.command("POST", f"/session/{session_id}/actions", {
            "actions": [{
                "type": "key",
                "id": "clipboard-test-keyboard",
                "actions": key_actions,
            }],
        })

        paste_log = "No paste events observed."
        last_observation: str | None = None
        last_change = time.monotonic()
        quiet_seconds = max(1.5, args.delay_ms / 1000.0 + 0.5)
        event_deadline = time.monotonic() + observation_window_seconds(
            args.consume_current, args.delay_ms)
        while time.monotonic() < event_deadline:
            result = driver.command(
                "POST", f"/session/{session_id}/execute/sync", {
                    "script": "return JSON.stringify(window.__clipboardPasteProbeEvents)",
                    "args": [],
                })
            paste_log = result.get("value", "[]")
            if paste_log != last_observation:
                last_observation = paste_log
                last_change = time.monotonic()
            if paste_log != "[]":
                try:
                    event_data = json.loads(paste_log)
                    pending_file_delivery = (
                        paste_events_have_pending_file_delivery(event_data))
                    if (not pending_file_delivery and
                            time.monotonic() - last_change >= quiet_seconds):
                        break
                except json.JSONDecodeError:
                    break
            time.sleep(0.05)

        elapsed = time.monotonic() - start
        stage_label = "current-clipboard" if args.consume_current else args.stage
        print(f"stage={stage_label} delay_ms={args.delay_ms} "
              f"delivery={args.delivery} press_count={args.press_count} "
              f"press_interval_ms={args.press_interval_ms} "
              f"paste_observation_s={elapsed:.3f}")

        payload_sha256: str | None = None
        payload_size: int | None = None
        if args.png is not None:
            payload_sha256 = hashlib.sha256(args.png.read_bytes()).hexdigest()
            payload_size = args.png.stat().st_size

        valid_png = False
        if paste_log == "[]":
            print("RESULT no DOM paste event observed")
        else:
            try:
                event_data = json.loads(paste_log)
                valid_png = paste_events_have_valid_png(event_data)
                print(f"RESULT dom_paste_event_count={len(event_data)} "
                      f"requested_keypress_count={args.press_count} "
                      f"valid_png_file={valid_png}")
                for event_index, event in enumerate(event_data, start=1):
                    print(f"RESULT event={event_index} "
                          f"paste_monotonic_ms={event.get('monotonicMs')} "
                          f"paste_wall_time={event.get('wallTime')} "
                          f"trusted={event.get('trusted')} "
                          f"types={event.get('types')!r} "
                          f"items={event.get('items')!r} "
                          f"files={event.get('files')!r}")
                    for item_index, item in enumerate(event.get("items", [])):
                        file_item = item.get("file")
                        if not file_item:
                            continue
                        readback = file_item.get("readback")
                        if payload_sha256 is not None and payload_size is not None:
                            exact = (
                                isinstance(readback, dict) and
                                readback.get("bytes") == payload_size and
                                readback.get("pngSignature") ==
                                    "89504e470d0a1a0a" and
                                readback.get("sha256") == payload_sha256
                            )
                            print(
                                f"RESULT event={event_index} item={item_index} "
                                f"exact_png={exact} "
                                f"source_sha256={payload_sha256} "
                                f"browser_readback={readback!r}")
                        else:
                            print(
                                f"RESULT event={event_index} item={item_index} "
                                f"browser_readback={readback!r}")
            except json.JSONDecodeError:
                print("RESULT paste event logged but could not parse event JSON")

        if args.expect_png and not valid_png:
            return 1
        return 0
    finally:
        if driver is not None and session_id is not None:
            try:
                driver.command("DELETE", f"/session/{session_id}")
            except Exception:
                pass
        for process in (geckodriver, owner, xvfb):
            if process is None or process.poll() is not None:
                continue
            process.terminate()
            try:
                process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3.0)
        if owner is not None and owner.stdout is not None:
            try:
                for line in owner.stdout:
                    owner_prefix.append(
                        line.decode("utf-8", errors="replace").rstrip())
            except OSError:
                pass
            important_markers = (
                "PNG_FILE_OWNER_REQUEST target=image/png",
                "PNG_FILE_OWNER_PRE_NOTIFY_DELAY_STARTED",
                "PNG_FILE_OWNER_INCR_FIRST_CHUNK_DELAY_STARTED",
                "PNG_FILE_OWNER_INCR_FIRST_CHUNK_DELAY_DONE",
                "PNG_FILE_OWNER_REQUEST_WAITER_QUEUED",
                "PNG_FILE_OWNER_SELECTION_NOTIFY",
                "PNG_FILE_OWNER_INCR_FIRST_CHUNK\n",
                "PNG_FILE_OWNER_INCR_DONE",
                "BadWindow",
            )
            important = [
                line for line in owner_prefix
                if any(marker in line for marker in important_markers)
            ]
            print("x11_owner_milestones:\n" + "\n".join(important))


if __name__ == "__main__":
    raise SystemExit(main())
