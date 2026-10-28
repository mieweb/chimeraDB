#!/usr/bin/env python3
"""Exercise the compiled health helper and real CLI with disposable TCP peers.

Usage: test-health.py /path/to/chimeradb-health
Only Python's standard library is needed; no database, driver or host service.
"""
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


require(len(sys.argv) == 2, __doc__)
probe = Path(sys.argv[1]).resolve()
require(probe.is_file(), f"missing probe: {probe}")
cli_source = Path(__file__).resolve().parent.parent / "cli" / "chimeradb"
checks = 0


def document(value=1.0):
    body = b"\x01ok\0" + struct.pack("<d", value) + b"\0"
    return struct.pack("<I", len(body) + 4) + body


def packet(request_id, body=None, opcode=2013, flags=0, kind=0):
    payload = struct.pack("<I", flags) + bytes([kind]) + (document() if body is None else body)
    return struct.pack("<iiii", len(payload) + 16, 123, request_id, opcode) + payload


def receive_exact(peer, length):
    data = b""
    while len(data) < length:
        chunk = peer.recv(length - len(data))
        require(chunk, "health client closed before sending a ping")
        data += chunk
    return data


with tempfile.TemporaryDirectory(prefix="chimera-health-test-") as directory:
    work = Path(directory)
    (work / "bin").mkdir()
    (work / "libexec").mkdir()
    shutil.copy2(cli_source, work / "bin" / "chimeradb")
    (work / "libexec" / "chimeradb-health").symlink_to(probe)
    sql = work / "bin" / "mariadb"
    sql.write_text("""#!/usr/bin/env bash
set -eu
case "${!#}" in
  'SELECT VERSION()') echo 11.8.9 ;;
  "SELECT plugin_status FROM information_schema.plugins WHERE plugin_name = 'chimera_mongo'") echo ACTIVE ;;
  'SELECT @@chimera_mongo_port') echo "$FIXTURE_PORT" ;;
  'SELECT @@chimera_mongo_bind') echo 127.0.0.1 ;;
  "SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name = 'chimera_meta'") echo 1 ;;
  "SELECT COUNT(*) FROM mysql.func WHERE name = 'mongo'") echo 1 ;;
  '\\s') printf 'Connection:\\t\\tLocalhost via UNIX socket\\n' ;;
  *) exit 2 ;;
esac
""")
    sql.chmod(0o755)
    environment = dict(os.environ, PATH=f"{work / 'bin'}:{os.environ['PATH']}")
    for key in ("CHIMERA_DEFAULTS_FILE", "CHIMERA_MONGO_HOST", "CHIMERA_MONGO_PORT"):
        environment.pop(key, None)

    def execute(port, cli=False):
        command = [str(work / "bin" / "chimeradb"), "status"] if cli else [str(probe), "127.0.0.1", str(port)]
        started = time.monotonic()
        result = subprocess.run(command, env=dict(environment, FIXTURE_PORT=str(port)),
                                capture_output=True, text=True, timeout=4)
        require(time.monotonic() - started < 3.5, "health check exceeded its whole-operation deadline")
        return result

    def check(label, expected, reply=packet, mode="normal", cli=False):
        global checks
        stop = threading.Event()
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
            if mode != "refused":
                listener.listen(1)
                listener.settimeout(4)

                def serve():
                    try:
                        with listener.accept()[0] as peer:
                            peer.settimeout(3)
                            header = receive_exact(peer, 16)
                            length, request_id, _, opcode = struct.unpack("<iiii", header)
                            require(21 < length < 256 and opcode == 2013, "client did not send a bounded OP_MSG")
                            request = receive_exact(peer, length - 16)
                            require(request[:5] == b"\0" * 5 and b"\x10ping\0\x01\0\0\0" in request,
                                    "client did not send the ping command")
                            require(b"\x02$db\0\x06\0\0\0admin\0" in request, "ping did not target admin")
                            if mode == "stall":
                                stop.wait(4)
                            elif mode != "eof":
                                response = reply(request_id)
                                if mode in ("fragmented", "trickle"):
                                    for byte in response:
                                        if stop.wait(0.12 if mode == "trickle" else 0.001):
                                            break
                                        peer.sendall(bytes([byte]))
                                else:
                                    peer.sendall(response)
                    except Exception as error:
                        errors.append(error)

                thread = threading.Thread(target=serve, daemon=True)
                thread.start()
            try:
                result = execute(port, cli)
            finally:
                stop.set()
                if mode != "refused":
                    thread.join(timeout=4)
                    require(not thread.is_alive(), "fixture did not stop")
            require(not errors, f"fixture failed: {errors}")
            require(result.returncode == expected,
                    f"{label}: expected {expected}, got {result.returncode}: {result.stdout}{result.stderr}")
            checks += 1
            print(f"PASS: {label}")

    check("successful Mongo ping", 0)
    check("fragmented response remains valid", 0, mode="fragmented")
    check("refused listener", 1, mode="refused")
    check("TCP accept without a reply times out", 1, mode="stall")
    check("partial progress cannot reset the deadline", 1, mode="trickle")
    check("EOF is not healthy", 1, mode="eof")
    check("Mongo error reply", 1, lambda rid: packet(rid, document(0)))
    check("mismatched responseTo", 1, lambda rid: packet(rid + 1))
    check("wrong reply opcode", 1, lambda rid: packet(rid, opcode=1))
    check("invalid BSON terminator", 1, lambda rid: packet(rid, document()[:-1] + b"x"))
    check("invalid BSON length", 1, lambda rid: packet(rid, b"\x05\0\0\0" + document()[4:]))
    check("unexpected OP_MSG flags", 1, lambda rid: packet(rid, flags=2))
    check("unexpected OP_MSG section kind", 1, lambda rid: packet(rid, kind=1))
    check("oversized reply rejected before allocation", 1,
          lambda rid: struct.pack("<iiii", 64 * 1024 * 1024, 1, rid, 2013))
    check("CLI ACTIVE plugin with a responding Mongo listener", 0, cli=True)
    check("CLI ACTIVE plugin with a dead listener is not ready", 1, mode="refused", cli=True)
    check("CLI ACTIVE plugin with a stuck listener is not ready", 1, mode="stall", cli=True)
print(f"PASS: {checks} compiled Mongo health/CLI checks")
