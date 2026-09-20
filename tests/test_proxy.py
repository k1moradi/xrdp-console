#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for the private IPv6-to-IPv4 RFB relay."""

from __future__ import annotations

import socket
import subprocess
import sys
import threading
import time
import unittest
import os
from pathlib import Path


PROXY = Path(__file__).parents[1] / "tools/benchmark/rfb_v6_v4_proxy.py"


def process_cpu_seconds(pid: int) -> float:
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        ticks = int(fields[11]) + int(fields[12])
        return ticks / float(os.sysconf("SC_CLK_TCK"))
    except (FileNotFoundError, IndexError, ValueError):
        return 0.0


def wait_for_line(stream, marker: bytes, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        chunk = stream.readline()
        if chunk:
            data.extend(chunk)
            if marker in data:
                return
    raise AssertionError(f"proxy did not announce {marker!r}: {bytes(data)!r}")


class RfbProxyTests(unittest.TestCase):
    def test_forwards_data_in_both_directions(self) -> None:
        backend_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        backend_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        backend_listener.bind(("127.0.0.1", 0))
        backend_listener.listen(1)
        backend_port = backend_listener.getsockname()[1]

        proxy_listener_probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        proxy_listener_probe.bind(("127.0.0.1", 0))
        proxy_port = proxy_listener_probe.getsockname()[1]
        proxy_listener_probe.close()

        proxy = subprocess.Popen(
            [sys.executable, str(PROXY), str(proxy_port), str(backend_port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        client: socket.socket | None = None
        backend: socket.socket | None = None
        try:
            assert proxy.stdout is not None
            wait_for_line(proxy.stdout, b"READY ", 3.0)
            client = socket.create_connection(("127.0.0.1", proxy_port), timeout=3)
            backend, _ = backend_listener.accept()
            client.settimeout(3.0)
            backend.settimeout(3.0)

            client.sendall(b"client-to-backend")
            self.assertEqual(backend.recv(64), b"client-to-backend")
            backend.sendall(b"backend-to-client")
            self.assertEqual(client.recv(64), b"backend-to-client")
        finally:
            for sock in (client, backend, backend_listener):
                if sock is not None:
                    try:
                        sock.close()
                    except OSError:
                        pass
            proxy.terminate()
            try:
                proxy.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                proxy.kill()
                proxy.wait(timeout=3.0)
            if proxy.stdout is not None:
                proxy.stdout.close()
            if proxy.stderr is not None:
                proxy.stderr.close()

    def test_blocked_backend_does_not_busy_spin(self) -> None:
        backend_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        backend_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        backend_listener.bind(("127.0.0.1", 0))
        backend_listener.listen(1)
        backend_port = backend_listener.getsockname()[1]

        proxy_listener_probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        proxy_listener_probe.bind(("127.0.0.1", 0))
        proxy_port = proxy_listener_probe.getsockname()[1]
        proxy_listener_probe.close()

        proxy = subprocess.Popen(
            [sys.executable, str(PROXY), str(proxy_port), str(backend_port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        client: socket.socket | None = None
        backend: socket.socket | None = None
        writer: threading.Thread | None = None
        try:
            assert proxy.stdout is not None
            wait_for_line(proxy.stdout, b"READY ", 3.0)
            client = socket.create_connection(("127.0.0.1", proxy_port), timeout=3)
            backend, _ = backend_listener.accept()
            # Make the blocked-write state deterministic.  The backend is not
            # read during the CPU observation interval.
            backend.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            client.settimeout(3.0)
            payload = b"x" * (8 * 1024 * 1024)

            def write_payload() -> None:
                assert client is not None
                try:
                    client.sendall(payload)
                except (OSError, TimeoutError):
                    pass

            writer = threading.Thread(target=write_payload, daemon=True)
            writer.start()
            time.sleep(0.25)
            started = process_cpu_seconds(proxy.pid)
            observed = time.monotonic()
            time.sleep(0.75)
            elapsed = time.monotonic() - observed
            consumed = process_cpu_seconds(proxy.pid) - started
            self.assertLess(
                consumed / max(elapsed, 1e-3),
                0.50,
                "relay consumed too much CPU while backend was blocked",
            )
        finally:
            for sock in (client, backend, backend_listener):
                if sock is not None:
                    try:
                        sock.close()
                    except OSError:
                        pass
            if writer is not None:
                writer.join(timeout=1.0)
            proxy.terminate()
            try:
                proxy.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                proxy.kill()
                proxy.wait(timeout=3.0)
            if proxy.stdout is not None:
                proxy.stdout.close()
            if proxy.stderr is not None:
                proxy.stderr.close()


if __name__ == "__main__":
    unittest.main()
