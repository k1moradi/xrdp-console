#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Forward loopback TCP listeners to an IPv4 TCP endpoint.

The xrdp VNC module has used both IPv6 and IPv4 loopback resolution across
builds. x11vnc's threaded listener remains IPv4-only in the benchmark, so
this tiny user-owned proxy accepts both loopback families and forwards them to
the IPv4 endpoint. It is deliberately limited to loopback and has no
authentication or network-facing mode.
"""

from __future__ import annotations

import argparse
import selectors
import socket
import sys


MAX_BUFFER_BYTES = 16 * 1024 * 1024


def close_socket(selector: selectors.BaseSelector, sock: socket.socket) -> None:
    """Unregister and close one relay socket, tolerating prior cleanup."""
    try:
        selector.unregister(sock)
    except (KeyError, ValueError):
        pass
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def close_pair(selector: selectors.BaseSelector,
               sockets: tuple[socket.socket, socket.socket]) -> None:
    for sock in sockets:
        close_socket(selector, sock)


def update_interest(selector: selectors.BaseSelector, sock: socket.socket,
                    pending: bytearray) -> None:
    """Read continuously; wait for writable notification when buffered."""
    events = selectors.EVENT_READ
    if pending:
        events |= selectors.EVENT_WRITE
    try:
        # ``modify()`` defaults data to None.  Preserve the peer socket so a
        # readable event can still find its destination after the first
        # buffered write enables EVENT_WRITE.
        peer = selector.get_key(sock).data
        selector.modify(sock, events, peer)
    except (KeyError, ValueError):
        pass


def relay_connection(client: socket.socket, backend: socket.socket) -> None:
    """Relay one connection without spinning when either output blocks.

    Each destination owns a bounded output buffer.  A nonblocking ``send``
    that returns ``EAGAIN`` leaves the socket registered for ``EVENT_WRITE``;
    the selector wakes us only when the kernel can accept more bytes.
    """
    sockets = (client, backend)
    for sock in sockets:
        sock.setblocking(False)
    selector = selectors.DefaultSelector()
    pending = {client: bytearray(), backend: bytearray()}
    selector.register(client, selectors.EVENT_READ, backend)
    selector.register(backend, selectors.EVENT_READ, client)
    try:
        while selector.get_map():
            events = selector.select(timeout=10.0)
            for key, mask in events:
                source = key.fileobj
                destination = key.data
                if mask & selectors.EVENT_READ:
                    try:
                        data = source.recv(65536)
                    except (BlockingIOError, InterruptedError):
                        data = None
                    except OSError:
                        return
                    if data is None:
                        pass
                    elif not data:
                        return
                    else:
                        output = pending[destination]
                        if len(output) + len(data) > MAX_BUFFER_BYTES:
                            # A stalled peer must not turn into unbounded
                            # memory growth. Closing both ends is preferable to
                            # silently delaying an RFB session indefinitely.
                            return
                        output.extend(data)

                if mask & selectors.EVENT_WRITE:
                    output = pending[source]
                    if output:
                        try:
                            sent = source.send(output)
                        except (BlockingIOError, InterruptedError):
                            sent = 0
                        except OSError:
                            return
                        if sent <= 0:
                            return
                        del output[:sent]

                for sock in sockets:
                    update_interest(selector, sock, pending[sock])
    finally:
        close_pair(selector, sockets)
        selector.close()


def relay(listeners: list[socket.socket], backend_host: str,
          backend_port: int) -> None:
    accept_sel = selectors.DefaultSelector()
    for listener in listeners:
        accept_sel.register(listener, selectors.EVENT_READ)
    while True:
        events = accept_sel.select(timeout=1.0)
        if not events:
            continue
        for key, _ in events:
            try:
                client, _ = key.fileobj.accept()
            except OSError:
                continue
            print("ACCEPT", flush=True)
            try:
                backend = socket.create_connection(
                    (backend_host, backend_port), timeout=3.0
                )
            except OSError:
                print("BACKEND_FAIL", flush=True)
                client.close()
                continue
            print("BACKEND_CONNECTED", flush=True)
            relay_connection(client, backend)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("listen_port", type=int)
    parser.add_argument("backend_port", type=int)
    args = parser.parse_args()
    listeners: list[socket.socket] = []
    for family, address in (
            (socket.AF_INET6, ("::1", args.listen_port)),
            (socket.AF_INET, ("127.0.0.1", args.listen_port))):
        listener = socket.socket(family, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if family == socket.AF_INET6:
            listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        listener.bind(address)
        listener.listen(8)
        listeners.append(listener)
    print(f"READY loopback:{args.listen_port} -> 127.0.0.1:{args.backend_port}",
          flush=True)
    try:
        relay(listeners, "127.0.0.1", args.backend_port)
    except KeyboardInterrupt:
        pass
    finally:
        for listener in listeners:
            listener.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
