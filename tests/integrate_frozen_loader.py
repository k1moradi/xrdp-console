#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate a review-only Firefox consumer integration for frozen loader.

Never modifies source in-place. The production xrdp C files are untouched.
Refuses to operate on a loader which differs from the frozen 0ee... tree.
"""
from __future__ import annotations
import argparse
import difflib
import hashlib
from pathlib import Path

FROZEN_LOADER_GIT_BLOB = '34ce4a5c0ea97634f253a687bfc6c461c3061b50'
FROZEN_COMMIT = '0ee616424b3290b50c2aec48491918eb1076916c'


def git_blob_id(source: bytes) -> str:
    return hashlib.sha1(f'blob {len(source)}\0'.encode('ascii')+source).hexdigest()


def change_once(src: str, original: str, replacement: str) -> str:
    count=src.count(original)
    if count != 1:
        raise ValueError(f'Frozen loader integration anchor count={count}; expected 1: {original[:75]!r}')
    return src.replace(original, replacement, 1)


def integrate(src: str) -> str:
    src = change_once(src,
        '        root = Path(temp)\n        named_png_fixture_path = root / "peer-named.png"\n',
        '''        root = Path(temp)
        firefox_delayed_png_consumer = (
            clipboard_delayed_png_response_mode and
            os.environ.get("XRDP_CONSOLE_TEST_FIREFOX_CONSUMER") == "1")
        named_png_fixture_path = root / "peer-named.png"
''')
    src = change_once(src,
        '''        source_display_process, source_display = start_source_display(
            source_display_log_path, source_width, source_height,
            randr_resize=randr_resize_mode)
''',
        '''        if firefox_delayed_png_consumer:
            from firefox_chansrv_consumer import start_authenticated_source_xvfb
            source_display_process, source_display = start_authenticated_source_xvfb(
                root, source_display_log_path, source_width, source_height)
        else:
            source_display_process, source_display = start_source_display(
                source_display_log_path, source_width, source_height,
                randr_resize=randr_resize_mode)
''')
    src = change_once(src,
        '''                        elif clipboard_delayed_png_response_mode:
                            if named_png_fixture_info is None:
                                raise AssertionError(
                                    "delayed PNG fixture metadata was not prepared")
                            assert_clipboard_delayed_png_response_session(
                                clipboard_helper, client, client_log_path,
                                chansrv_process, chansrv_logs_path,
                                chansrv_stdout_path, source_display,
                                *named_png_fixture_info)
''',
        '''                        elif clipboard_delayed_png_response_mode:
                            if named_png_fixture_info is None:
                                raise AssertionError(
                                    "delayed PNG fixture metadata was not prepared")
                            if firefox_delayed_png_consumer:
                                assert_clipboard_delayed_png_firefox_session(
                                    client, client_log_path, chansrv_process,
                                    chansrv_logs_path, chansrv_stdout_path,
                                    source_display, source_display_process,
                                    root, *named_png_fixture_info)
                            else:
                                assert_clipboard_delayed_png_response_session(
                                    clipboard_helper, client, client_log_path,
                                    chansrv_process, chansrv_logs_path,
                                    chansrv_stdout_path, source_display,
                                    *named_png_fixture_info)
''')
    src = change_once(src,
        '''def assert_clipboard_delayed_png_response_session(
''',
        '''def assert_clipboard_delayed_png_firefox_session(
        client: subprocess.Popen[object], client_log_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str,
        source_display_process: subprocess.Popen[bytes], root: Path,
        expected_png_bytes: int, expected_png_sha256: str) -> None:
    """Trusted Firefox paste over the existing synthetic CLIPRDR peer."""
    from firefox_chansrv_consumer import (
        TestInconclusive, correlate_metadata, run_firefox_chansrv_timing)
    delay_ms = int(os.environ["XRDP_CONSOLE_TEST_DELAY_PNG_RESPONSE_MS"])
    initial = wait_for_chansrv_pattern(
        chansrv_logs,
        rf"event=format-list[^\\n]*stored_formats=2 dib_format_id=8 "
        rf"png_format_id={NAMED_PNG_FORMAT_ID} generation=(\\d+)",
        15.0, chansrv_process, chansrv_stdout)
    match = re.search(r"generation=(\\d+)", initial)
    if match is None:
        raise AssertionError("No generation in synthetic Format List")
    generation = int(match.group(1))
    wait_for_peer_marker(client, client_log_path,
        f"PEER_INITIAL_FORMAT_LIST_SENT dib=8 png={NAMED_PNG_FORMAT_ID}",
        10.0)
    firefox = (os.environ.get("XRDP_CONSOLE_TEST_FIREFOX_BINARY") or
               shutil.which("firefox"))
    geckodriver = (os.environ.get("XRDP_CONSOLE_TEST_GECKODRIVER") or
                   shutil.which("geckodriver"))
    if firefox is None or geckodriver is None:
        raise TestSkipped("test-only Firefox/geckodriver executable unavailable")

    def wait_for_delivery(receipt: dict) -> None:
        # Firefox must remain alive until EVERY observed PNG INCR request is
        # accounted for, including repeated XIDs and properties. A first
        # terminator ack is insufficient evidence when Firefox retries.
        from firefox_x11_delivery_ledger import (
            DeliveryTraceError, wait_for_png_delivery)
        wait_for_peer_marker(
            client, client_log_path,
            f"PEER_PNG_DELAY_RESPONSE_SENT delay_ms={delay_ms} ",
            max(10.0, delay_ms / 1000.0 + 8.0))
        try:
            delivery = wait_for_png_delivery(
                lambda: chansrv_log_text(chansrv_logs),
                lambda: chansrv_process.poll() is None,
                generation=generation, expected_bytes=expected_png_bytes,
                timeout_s=20.0, quiescence_s=0.35)
        except DeliveryTraceError as exc:
            raise AssertionError(
                f"X11 PNG delivery evidence incomplete: {exc}") from exc
        receipt["x11_delivery"] = delivery

    try:
        report = run_firefox_chansrv_timing(
            source_display=source_display,
            xauthority=root / "firefox-Xauthority",
            xvfb_pid=source_display_process.pid,
            root=root, firefox=Path(firefox), geckodriver=Path(geckodriver),
            expected_size=expected_png_bytes,
            expected_sha256=expected_png_sha256,
            expected_generation=generation,
            after_receipt=wait_for_delivery)
    except TestInconclusive as exc:
        raise TestSkipped(str(exc)) from exc
    classification = report["classification"]
    metadata = correlate_metadata(
        chansrv_log_text(chansrv_logs), read_text(client_log_path),
        NAMED_PNG_FORMAT_ID, generation, classification)
    if metadata["x11_request_count"] < 1:
        raise AssertionError("No same-generation Firefox X11 image request")
    if metadata["incr_terminator_ack_count"] < 1:
        raise AssertionError("No completed Firefox X11 INCR transfer")
    if classification not in ("TRUSTED_PASTE_NULL_FILE", "READABLE_PNG_FILE"):
        raise AssertionError(f"Unexpected browser outcome: {classification} "
                             f"metadata={metadata} receipt={report}")
    print("FIREFOX_CLIPRDR_PASTE " + json.dumps({
        "delay_ms": delay_ms, "classification": classification,
        "trusted": report["trusted"], "getAsFileNull": report["getAsFileNull"],
        "fileSize": report.get("fileSize"), "sha256": report.get("sha256"),
        "x11": metadata, "firefox_version": report.get("firefox_version")},
        sort_keys=True), flush=True)


def assert_clipboard_delayed_png_response_session(
''')
    return src


def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--patch',type=Path,required=True)
    args=ap.parse_args()
    data=args.source.read_bytes()
    actual=git_blob_id(data)
    if actual != FROZEN_LOADER_GIT_BLOB:
        raise ValueError('Refusing nonfrozen loader; expected git blob '
                         f'{FROZEN_LOADER_GIT_BLOB}; got {actual}')
    original=data.decode('utf-8')
    updated=integrate(original)
    if args.output.resolve() == args.source.resolve():
        raise ValueError('Refusing in-place modification of source loader')
    if args.output.exists() or args.patch.exists():
        raise FileExistsError('Review-only output already exists')
    compile(updated,str(args.output),'exec')
    diff=''.join(difflib.unified_diff(original.splitlines(keepends=True),
                                   updated.splitlines(keepends=True),
                                   fromfile='a/tests/test_xrdp_loader.py',
                                   tofile='b/tests/test_xrdp_loader.py'))
    args.output.write_text(updated,encoding='utf-8')
    args.patch.write_text(diff,encoding='utf-8')
    print('Frozen loader integration generated; review-only, no production changes')
    print('patch lines:', len(diff.splitlines()))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
