"""Exercise the real TCP server using only Python's standard library."""
import contextlib
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
from pathlib import Path

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/webserver").resolve())


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


@contextlib.contextmanager
def server(port):
    process = subprocess.Popen(
        [binary, str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    try:
        assert process.stdout.readline().startswith("TCP echo server listening"), "startup failed"
        yield process
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            _, errors = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        assert process.returncode == 0, (process.returncode, errors)
        assert "Sanitizer" not in errors and "runtime error:" not in errors, errors


def connect(port):
    return socket.create_connection(("127.0.0.1", port), timeout=3)


def exchange(port, payload, fragmented=False):
    with connect(port) as client:
        failures = []

        def write():
            try:
                if fragmented:
                    for byte in payload:
                        client.sendall(bytes([byte]))
                else:
                    client.sendall(payload)
                client.shutdown(socket.SHUT_WR)
            except Exception as error:
                failures.append(error)

        writer = threading.Thread(target=write, daemon=True)
        writer.start()
        received = bytearray()
        while True:
            chunk = client.recv(8192)
            if not chunk:
                break
            received.extend(chunk)
        writer.join(timeout=3)
        assert not writer.is_alive(), "sender stalled"
        assert not failures, failures
        assert received == payload, (len(received), len(payload))


for invalid in ["0", "65536", "-1", "abc", "8080x", "", "999999999999999999999999"]:
    result = subprocess.run([binary, invalid], capture_output=True, timeout=3)
    assert result.returncode != 0, invalid

port = free_port()
with server(port) as process:
    result = subprocess.run([binary, str(port)], capture_output=True, timeout=3)
    assert result.returncode != 0 and b"bind" in result.stderr
    exchange(port, b"hello\nworld\n", fragmented=True)
    exchange(port, b"")
    exchange(port, bytes(range(256)) * 8192)
    with connect(port) as client:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        client.sendall(b"disconnect abruptly" * 256)
    exchange(port, b"alive after reset")
    with connect(port):
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=3) == 0

# Immediate restart and shutdown while no client is connected.
with server(port) as process:
    process.send_signal(signal.SIGINT)
    assert process.wait(timeout=3) == 0

# A peer that never reads must not prevent shutdown during a blocked send.
with server(free_port()) as process:
    # Obtain the actual port from the process arguments.
    blocked_port = int(process.args[1])
    with connect(blocked_port) as client:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        client.settimeout(0.2)
        payload = os.urandom(65536)
        for _ in range(256):
            try:
                client.sendall(payload)
            except socket.timeout:
                break
        process.terminate()
        assert process.wait(timeout=3) == 0

print("TCP integration checks passed")
