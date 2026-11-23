"""실제 HTTP 클라이언트로 정적 파일과 경로 접근 제한을 확인한다."""
import contextlib
import http.client
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/webserver").resolve())


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


@contextlib.contextmanager
def server(port, root):
    process = subprocess.Popen([binary, str(port), str(root)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert process.stdout.readline().startswith("HTTP server listening"), "startup failed"
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


def check_response(response, expected, body=None, mime=None):
    actual = response.read()
    assert response.status == expected, (response.status, actual)
    assert int(response.getheader("Content-Length")) == len(actual)
    assert response.getheader("Connection") == "close"
    if body is not None:
        assert actual == body, (len(actual), len(body))
    if mime is not None:
        assert response.getheader("Content-Type") == mime


def request(port, path, expected=200, body=None, mime=None, method="GET"):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request(method, path)
        check_response(connection.getresponse(), expected, body, mime)
    finally:
        connection.close()


def raw_request(port, parts, expected):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        for part in parts:
            client.sendall(part)
        client.shutdown(socket.SHUT_WR)
        response = http.client.HTTPResponse(client)
        response.begin()
        check_response(response, expected)


for invalid in ["0", "65536", "-1", "abc", "8080x", "", "999999999999999999999999"]:
    result = subprocess.run([binary, invalid], capture_output=True, timeout=3)
    assert result.returncode != 0, invalid

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp) / "www"
    root.mkdir()
    outside = Path(temp) / "secret.txt"
    outside.write_bytes(b"must not leak")
    (root / "index.html").write_bytes(b"<h1>Hello</h1>\n")
    (root / "style.css").write_bytes(b"body { color: red; }")
    (root / "app.js").write_bytes(b"console.log('hello');")
    binary_body = bytes(range(256)) * 8192
    (root / "large.PNG").write_bytes(binary_body)
    (root / "photo.jpg").write_bytes(b"\xff\xd8\x00\xff")
    (root / "data.bin").write_bytes(b"\x00\xff\r\n")
    (root / "empty.txt").write_bytes(b"")
    (root / "space name.txt").write_bytes("한글".encode())
    (root / "sub").mkdir()
    (root / "sub/index.html").write_bytes(b"sub index")
    (root / "no-index").mkdir()
    (root / "blocked.txt").write_bytes(b"private")
    (root / "blocked.txt").chmod(0)
    (root / "escape.txt").symlink_to(outside)
    (root / "escape-dir").symlink_to(Path(temp), target_is_directory=True)
    os.mkfifo(root / "pipe")

    port = free_port()
    with server(port, root) as process:
        result = subprocess.run([binary, str(port), str(root)], capture_output=True, timeout=3)
        assert result.returncode != 0 and b"bind" in result.stderr
        request(port, "/", body=b"<h1>Hello</h1>\n", mime="text/html; charset=utf-8")
        request(port, "/index.html?version=1", body=b"<h1>Hello</h1>\n")
        request(port, "/style.css", mime="text/css; charset=utf-8")
        request(port, "/app.js", mime="text/javascript; charset=utf-8")
        request(port, "/large.PNG", body=binary_body, mime="image/png")
        request(port, "/photo.jpg", body=b"\xff\xd8\x00\xff", mime="image/jpeg")
        request(port, "/data.bin", body=b"\x00\xff\r\n", mime="application/octet-stream")
        request(port, "/empty.txt", body=b"")
        request(port, "/space%20name.txt", body="한글".encode())
        request(port, "/sub", body=b"sub index", mime="text/html; charset=utf-8")
        request(port, "/sub/", body=b"sub index")
        for path in ["/missing", "/no-index/", "/index.html/", "/index.html/child"]:
            request(port, path, 404)
        for path in ["/../secret.txt", "/%2e%2e/secret.txt", "/sub/%2e%2e/index.html",
                     "/..%2fsecret.txt", "/escape.txt", "/escape-dir/secret.txt", "/pipe",
                     "/%5c..%5csecret.txt"]:
            request(port, path, 403)
        for path in ["/bad%", "/bad%GG", "/bad%00", "/bad%0a", "/index.html#fragment"]:
            request(port, path, 400)
        if os.geteuid() != 0:
            request(port, "/blocked.txt", 403)
        request(port, "/", 501, method="POST")
        raw_request(port, [bytes([byte]) for byte in b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"], 200)
        raw_request(port, [b"GET / HTTP/1.1\r\n\r\n"], 400)
        raw_request(port, [b"GET / HTTP/1.1\r\nHost: x\r\n"], 400)
        raw_request(port, [b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhe"], 400)
        raw_request(port, [b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n"], 413)
        raw_request(port, [b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"], 501)
        with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            client.sendall(b"GET /large.PNG HTTP/1.1\r\nHost: x\r\n\r\n")
        request(port, "/", body=b"<h1>Hello</h1>\n")
        with socket.create_connection(("127.0.0.1", port), timeout=5):
            process.terminate()
            assert process.wait(timeout=3) == 0
    # 읽지 않는 클라이언트로 송신이 정체되어도 종료 요청에 응답해야 한다.
    with server(port, root) as process:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
            client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            client.sendall(b"GET /large.PNG HTTP/1.1\r\nHost: x\r\n\r\n")
            time.sleep(0.1)
            process.terminate()
            assert process.wait(timeout=3) == 0
    with server(port, root) as process:
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=3) == 0
    result = subprocess.run([binary, str(port), str(root / "missing")],
                            capture_output=True, timeout=3)
    assert result.returncode != 0 and b"static root" in result.stderr
    (root / "blocked.txt").chmod(0o600)

print("Static HTTP integration checks passed")
