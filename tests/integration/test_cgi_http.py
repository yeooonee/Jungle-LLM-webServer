"""실제 HTTP 요청을 CGI 실행·파싱·HTTP 송신까지 연결해 확인한다."""
import concurrent.futures
import contextlib
import http.client
import json
import os
import signal
import time
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

binary, cgi = (str(Path(path).resolve()) for path in sys.argv[1:3])
mode = sys.argv[3] if len(sys.argv) > 3 else "threadpool"


@contextlib.contextmanager
def server(root, program):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    command = [binary, str(port), str(root)]
    if program is not None:
        command.append(str(program))
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        assert process.stdout.readline().startswith("HTTP server listening")
        yield port, process
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            _, errors = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        assert process.returncode == 0, (process.returncode, errors)
        assert "Sanitizer" not in errors and "runtime error:" not in errors, errors


def request(port, path, payload=None, status=200, method="POST"):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request(method, path, body=payload)
        response = connection.getresponse()
        body = response.read()
        assert response.status == status, (response.status, body)
        assert response.getheader("Connection") == "close"
        assert int(response.getheader("Content-Length")) == len(body)
        assert response.getheader("Status") is None
        return response.getheader("Content-Type"), body
    finally:
        connection.close()


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "www"
    root.mkdir()
    (root / "index.html").write_bytes(b"static still works")
    with server(root, cgi) as (port, process):
        for payload, answer in ((b"a=2&b=3", 5), (b"a=-12&b=3", -9), (b"a=0&b=0", 0)):
            mime, body = request(port, "/cgi/add", payload)
            assert mime == "application/json" and json.loads(body) == {"result": answer}
        for payload in (b"", b"a=2&b=", b"b=3&a=2", b"a=2&b=3\0extra",
                        b"a=999999999999999999999999&b=0"):
            mime, body = request(port, "/cgi/add", payload, status=400)
            assert mime == "application/json" and "error" in json.loads(body)
        _, body = request(port, "/add", b"a=2&b=3")
        assert json.loads(body) == {"result": 5}
        _, body = request(port, "/", method="GET")
        assert body == b"static still works"
        request(port, "/cgi/add-more", b"a=2&b=3", status=501)
        request(port, "/cgi/add?other=1", b"a=2&b=3", status=501)
        request(port, "/cgi/add", method="GET", status=404)
        with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
            client.sendall(b"POST /cgi/add HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\na=2")
            client.sendall(b"&b=3")
            client.shutdown(socket.SHUT_WR)
            response = http.client.HTTPResponse(client)
            response.begin()
            assert response.status == 200 and json.loads(response.read()) == {"result": 5}

        def add(number):
            _, body = request(port, "/cgi/add", f"a={number}&b=1".encode())
            assert json.loads(body) == {"result": number + 1}

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(add, range(16)))

    with server(root, None) as (port, process):
        request(port, "/cgi/add", b"a=2&b=3", status=501)
        _, body = request(port, "/add", b"a=2&b=3")
        assert json.loads(body) == {"result": 5}

    with server(root, Path(directory) / "missing-program") as (port, process):
        _, body = request(port, "/cgi/add", b"a=2&b=3", status=500)
        assert body == b"Internal Server Error\n"
        request(port, "/", method="GET")

    program = Path(directory) / "fixture"
    program.touch()
    program.chmod(0o700)

    def fixture(code):
        program.write_text(f"#!{sys.executable}\nimport os, sys, time, signal\n"
                           "sys.stdin.buffer.read(int(os.environ['CONTENT_LENGTH']))\n" + code + "\n")

    for code in (
        "sys.stdout.write('not CGI')",
        "sys.stdout.write('Content-Type: text/plain\\r\\n\\r\\nsecret'); sys.exit(42)",
        "os.kill(os.getpid(), signal.SIGTERM)",
        "sys.stdout.write('Status: 201 Created\\r\\nContent-Type: text/plain\\r\\n\\r\\nsecret')",
        "sys.stdout.write('Content-Type: ' + 'a' * 1024 + '\\r\\n\\r\\nsecret')",
        "sys.stdout.write('Content-Type: text/plain\\r\\n\\r\\n' + 'x' * 1048577)",
        "sys.stdout.buffer.write(b'x' * (1048576 + 8193))",
    ):
        fixture(code)
        with server(root, program) as (port, process):
            _, body = request(port, "/cgi/add", b"a=2&b=3", status=502)
            assert body == b"Bad Gateway\n", body
            request(port, "/", method="GET")

    for code in ("time.sleep(30)", "os.close(1); time.sleep(30)"):
        fixture(code)
        with server(root, program) as (port, process):
            started = time.monotonic()
            _, body = request(port, "/cgi/add", b"a=2&b=3", status=504)
            assert body == b"Gateway Timeout\n", body
            assert 2.5 <= time.monotonic() - started < 5
            request(port, "/", method="GET")

    # 서버 종료가 CGI의 3초 기한까지 기다리지 않는지, 자식도 회수하는지 확인한다.
    for stop_signal, close_output in ((signal.SIGINT, False), (signal.SIGTERM, False),
                                      (signal.SIGINT, True), (signal.SIGTERM, True)):
        pid_file = Path(directory) / "active-pids"
        pid_file.unlink(missing_ok=True)
        fixture(("os.close(1)\n" if close_output else "") +
                f"with open({str(pid_file)!r}, 'a') as f: f.write(str(os.getpid()) + '\\n')\n"
                "time.sleep(30)")
        active = 4 if mode == "threadpool" else 1
        clients = []
        with server(root, program) as (port, process):
            try:
                for _ in range(active):
                    client = socket.create_connection(("127.0.0.1", port), timeout=5)
                    clients.append(client)
                    client.sendall(b"POST /cgi/add HTTP/1.1\r\nHost: x\r\n"
                                   b"Content-Length: 7\r\n\r\na=2&b=3")
                deadline = time.monotonic() + 2
                while True:
                    pids = pid_file.read_text().splitlines() if pid_file.exists() else []
                    if len(pids) == active:
                        break
                    assert time.monotonic() < deadline, ("CGI did not start", pids)
                    time.sleep(0.01)
                started = time.monotonic()
                process.send_signal(stop_signal)
                assert process.wait(timeout=1.5) == 0
                assert time.monotonic() - started < 1.5
                for pid in pids:
                    try:
                        os.kill(int(pid), 0)
                    except ProcessLookupError:
                        pass
                    else:
                        raise AssertionError(f"CGI process {pid} was not reaped")
                for client in clients:
                    try:
                        assert client.recv(1) == b"", "unexpected response during shutdown"
                    except ConnectionResetError:
                        pass
            finally:
                for client in clients:
                    client.close()

    result = subprocess.run([binary, "8080", str(root), ""],
                            capture_output=True, timeout=3)
    assert result.returncode != 0 and b"CGI program path" in result.stderr

print("CGI HTTP routing checks passed")
