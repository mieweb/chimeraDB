#!/usr/bin/env python3
"""Exercise an already-running TLS listener; no database writes or dependencies.

CHIMERA_AUTH_TLS_CERT=... CHIMERA_AUTH_PORT=... python3 tls_handshake_deadline.py

Runs the slow-client cases in parallel with an established connection, taking
about 16 seconds. The trickle case must fail against the former per-I/O timeout.
"""

import argparse
import concurrent.futures
import os
import select
import socket
import ssl
import struct
import time


HANDSHAKE_SECONDS = 15
SCHEDULING_TOLERANCE = 3


def client_hello(context, server_name):
    outgoing = ssl.MemoryBIO()
    client = context.wrap_bio(ssl.MemoryBIO(), outgoing, server_hostname=server_name)
    try:
        client.do_handshake()
    except ssl.SSLWantReadError:
        pass
    hello = outgoing.read()
    assert len(hello) > 100, "expected a complete TLS ClientHello"
    return hello


def incomplete_handshake(address, hello, trickle):
    label = "trickled ClientHello" if trickle else "idle handshake"
    with socket.create_connection(address, timeout=3) as sock:
        started = time.monotonic()
        expires = started + HANDSHAKE_SECONDS + SCHEDULING_TOLERANCE
        # Send a genuine TLS record one byte at a time, keeping every individual
        # read below the old 15-second socket timeout without finishing it.
        offset = 0
        next_byte = started
        while True:
            now = time.monotonic()
            assert now < expires, f"{label} survived the overall handshake deadline"
            if trickle and now >= next_byte:
                assert offset < len(hello), "trickle unexpectedly completed ClientHello"
                try:
                    sock.sendall(hello[offset : offset + 1])
                except (BrokenPipeError, ConnectionResetError):
                    break
                offset += 1
                next_byte = now + 0.2
            wait = min(0.2, expires - now)
            if trickle:
                wait = min(wait, max(0, next_byte - now))
            if select.select([sock], [], [], wait)[0]:
                try:
                    data = sock.recv(4096)
                except ConnectionResetError:
                    data = b""
                assert not data, f"{label}: server replied before receiving ClientHello"
                break
        elapsed = time.monotonic() - started
        assert elapsed >= HANDSHAKE_SECONDS - 1, (
            f"{label} closed prematurely after {elapsed:.2f}s; deadline was not exercised"
        )
        assert elapsed < HANDSHAKE_SECONDS + SCHEDULING_TOLERANCE, (
            f"{label} closed too late after {elapsed:.2f}s"
        )
        return f"{label} closed after {elapsed:.2f}s"


def receive_exact(sock, length):
    result = bytearray()
    while len(result) < length:
        chunk = sock.recv(length - len(result))
        assert chunk, "TLS connection closed while reading Mongo hello reply"
        result.extend(chunk)
    return bytes(result)


def mongo_hello(sock, request_id):
    fields = b"\x10hello\x00" + struct.pack("<i", 1)
    fields += b"\x02$db\x00" + struct.pack("<i", 6) + b"admin\x00"
    document = struct.pack("<i", len(fields) + 5) + fields + b"\x00"
    body = struct.pack("<i", 0) + b"\x00" + document
    sock.sendall(struct.pack("<iiii", len(body) + 16, request_id, 0, 2013) + body)
    header = receive_exact(sock, 16)
    length, _, response_to, opcode = struct.unpack("<iiii", header)
    assert response_to == request_id and opcode == 2013, "invalid Mongo hello reply header"
    assert 21 < length < 1024 * 1024, "invalid Mongo hello reply size"
    reply = receive_exact(sock, length - 16)
    # The listener emits `ok` as a BSON double. Also require the handshake field
    # so an error document cannot pass merely by having a readable TLS reply.
    assert b"\x01ok\x00" + struct.pack("<d", 1) in reply, "Mongo hello failed"
    assert b"isWritablePrimary\x00" in reply, "missing Mongo hello handshake fields"


def established_connection(address, context, server_name):
    with socket.create_connection(address, timeout=3) as raw:
        with context.wrap_socket(raw, server_hostname=server_name) as tls:
            mongo_hello(tls, 1)
            # Both restored blocking mode and the absence of a leftover socket
            # timeout matter: a completed TLS session must remain usable later.
            time.sleep(HANDSHAKE_SECONDS + 1)
            mongo_hello(tls, 2)
    return "verified TLS hello succeeds before and after idle past the handshake deadline"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=int(os.getenv("CHIMERA_AUTH_PORT", "27017")))
    parser.add_argument("--ca-file", default=os.getenv("CHIMERA_AUTH_TLS_CERT"))
    parser.add_argument("--server-name", default=os.getenv("CHIMERA_MONGO_TLS_SERVER_NAME"))
    args = parser.parse_args()
    if not args.ca_file:
        parser.error("--ca-file or CHIMERA_AUTH_TLS_CERT is required")
    context = ssl.create_default_context(cafile=args.ca_file)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    address = (args.host, args.port)
    server_name = args.server_name or args.host
    hello = client_hello(context, server_name)
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        checks = [
            pool.submit(incomplete_handshake, address, hello, False),
            pool.submit(incomplete_handshake, address, hello, True),
            pool.submit(established_connection, address, context, server_name),
        ]
        for check in checks:
            print(f"  ok  {check.result()}", flush=True)


if __name__ == "__main__":
    main()
