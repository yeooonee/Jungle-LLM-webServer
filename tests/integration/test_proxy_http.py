"""CLI로 켠 서버를 통해 실제 TCP 백엔드와 HTTP 요청·응답을 주고받는다."""
import concurrent.futures
import contextlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading

binary, cgi = (str(Path(path).resolve()) for path in sys.argv[1:3])


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Backend(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.close_connection = True
        self.connection.settimeout(6)
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/api/wait":
            self.server.waiting.set()
            if self.connection.recv(1) == b"":
                self.server.disconnected.set()
            return
        replies = {
            "/api/chunked": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                            b"5\r\nhello\r\n0\r\nX-Checksum: abc\r\n\r\n",
            "/api/eof": b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nhello",
            "/api/short": b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello",
            "/api/malformed": b"not HTTP\r\n\r\n",
        }
        if self.path in replies:
            self.connection.sendall(replies[self.path])
            return
        result = json.dumps({
            "method": self.command, "path": self.path,
            "host": self.headers["Host"], "body": body.hex(),
            "connection": self.headers["Connection"],
            "secret": self.headers.get("X-Secret"),
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(result)))
        self.send_header("Connection", "X-Backend-Secret")
        self.send_header("X-Backend-Secret", "hidden")
        self.end_headers()
        self.wfile.write(result)

    do_POST = do_GET


@contextlib.contextmanager
def backend_server():
    backend = ThreadingHTTPServer(("127.0.0.1", 0), Backend)
    backend.waiting = threading.Event()
    backend.disconnected = threading.Event()
    thread = threading.Thread(target=backend.serve_forever)
    thread.start()
    try:
        yield backend
    finally:
        backend.shutdown()
        backend.server_close()
        thread.join()


@contextlib.contextmanager
def server(root, backend_port=None, program=None, extra_options=()):
    port = free_port()
    command = [binary, str(port), str(root)]
    if program is not None:
        command.append(program)
    if backend_port is not None:
        command.extend(["--proxy", "127.0.0.1", str(backend_port)])
    command.extend(extra_options)
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        assert select.select([process.stdout], [], [], 3)[0], "startup timed out"
        assert process.stdout.readline().startswith("HTTP server listening"), command
        yield port, process
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


def request(port, path, status=200, method="GET", body=None, headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        data = response.read()
        assert response.status == status, (response.status, data)
        assert response.getheader("Connection") == "close"
        assert response.getheader("X-Backend-Secret") is None
        if response.getheader("Content-Length") is not None:
            assert int(response.getheader("Content-Length")) == len(data)
        return response, data
    finally:
        connection.close()


for args, diagnostic in [
    (["--proxy"], b"Usage:"),
    (["--proxy-path"], b"Usage:"),
    (["--proxy-path", "/service"], b"requires --proxy"),
    (["--proxy-path", "/one", "--proxy-path", "/two"], b"Usage:"),
    (["--unknown"], b"Usage:"),
    (["--route", "/api", "127.0.0.1"], b"Usage:"),
    (["--route", "/bad/", "127.0.0.1", "8001"], b"Invalid proxy path"),
    (["--route", "/api", "localhost", "8001"], b"Invalid proxy IPv4"),
    (["--route", "/api", "127.0.0.1", "0"], b"Invalid proxy port"),
    (["--route", "/api", "127.0.0.1", "8001", "--route", "/api", "127.0.0.1", "8001"],
     b"Duplicate proxy backend"),
    (["--proxy", "127.0.0.1", "8001", "--route", "/other", "127.0.0.1", "8002"], b"Usage:"),
    (["--route", "/other", "127.0.0.1", "8002", "--proxy", "127.0.0.1", "8001"], b"Usage:"),
    (["--proxy-path", "/other", "--route", "/api", "127.0.0.1", "8001"], b"Usage:"),
    (["--route", "/api", "127.0.0.1", "8001", "--proxy-path", "/other"], b"Usage:"),
    (["--proxy", "127.0.0.1"], b"Usage:"),
    (["--proxy", "127.0.0.1", "8001", "extra"], b"Usage:"),
    (["--proxy", "127.0.0.1", "8001", "--proxy", "127.0.0.1", "8002"], b"Usage:"),
    (["8080", "www", "cgi", "extra", "--proxy", "127.0.0.1", "8001"], b"Usage:"),
]:
    result = subprocess.run([binary, *args], capture_output=True, timeout=3)
    assert result.returncode != 0 and diagnostic in result.stderr, (args, result.stderr)
for address in ("", "localhost", "::1", "999.0.0.1", "127.0.0.1:8001"):
    result = subprocess.run([binary, "--proxy", address, "8001"], capture_output=True, timeout=3)
    assert result.returncode != 0 and b"Invalid proxy IPv4" in result.stderr
for invalid in ("", "0", "65536", "-1", "+80", " 80", "80x", "999999999999999999999999"):
    for args, diagnostic in ((["--proxy", "127.0.0.1", invalid], b"Invalid proxy port"),
                              ([invalid], b"Invalid port")):
        result = subprocess.run([binary, *args], capture_output=True, timeout=3)
        assert result.returncode != 0 and diagnostic in result.stderr, (args, result.stderr)

for prefix in ("", "service", "/service/", "/a?b", "/a#b", "/a b", "/a\n",
               "/a\\b", "/한글", "/" + "a" * 8192):
    result = subprocess.run([binary, "--proxy", "127.0.0.1", "8001",
                             "--proxy-path", prefix], capture_output=True, timeout=3)
    assert result.returncode != 0 and b"Invalid proxy path" in result.stderr

too_many_backends = []
for number in range(9):
    too_many_backends.extend(["--route", "/pool", "127.0.0.1", str(10000 + number)])
result = subprocess.run([binary, *too_many_backends], capture_output=True, timeout=3)
assert result.returncode != 0 and b"Too many backends for proxy path" in result.stderr

overflow = []
for number in range(17):
    overflow.extend(["--route", f"/rule{number}", "127.0.0.1", "8001"])
result = subprocess.run([binary, *overflow], capture_output=True, timeout=3)
assert result.returncode != 0 and b"Too many proxy routes" in result.stderr

with tempfile.TemporaryDirectory() as directory, backend_server() as backend:
    root = Path(directory)
    (root / "index.html").write_bytes(b"static works")
    (root / "api").write_bytes(b"local api")
    (root / "apix").write_bytes(b"local apix")
    backend_port = backend.server_port
    with server(root, backend_port) as (port, process):
        for path in ("/api", "/api/", "/api/users?q=a%20b"):
            _, data = request(port, path)
            result = json.loads(data)
            assert result["path"] == path and result["method"] == "GET"
            assert result["host"] == f"127.0.0.1:{backend_port}"
            assert result["connection"] == "close"
        payload = bytes(range(256)) * 256
        _, data = request(port, "/api/echo?x=1", method="POST", body=payload,
                          headers={"Connection": "X-Secret", "X-Secret": "hidden"})
        result = json.loads(data)
        assert result["body"] == payload.hex() and result["secret"] is None
        assert result["method"] == "POST" and result["path"] == "/api/echo?x=1"
        _, data = request(port, "/")
        assert data == b"static works"
        _, data = request(port, "/apix")
        assert data == b"local apix"
        request(port, "/other?q=/api/", status=404)
        _, data = request(port, "/add", method="POST", body=b"a=2&b=3")
        assert json.loads(data) == {"result": 5}
        response, data = request(port, "/api/chunked")
        assert data == b"hello" and response.getheader("Transfer-Encoding") == "chunked"
        response, data = request(port, "/api/eof")
        assert data == b"hello" and response.getheader("Content-Length") is None
        request(port, "/api/malformed", status=502)
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        try:
            connection.request("GET", "/api/short")
            response = connection.getresponse()
            assert response.status == 200
            try:
                response.read()
            except http.client.IncompleteRead as error:
                assert error.partial == b"hello"  # 200 뒤에 502를 붙이지 않는다.
            else:
                raise AssertionError("short response was accepted")
        finally:
            connection.close()
        request(port, "/api/wait", status=504)
        assert backend.disconnected.wait(2), "timed-out backend socket was not closed"
        request(port, "/")

    with server(root, backend_port, extra_options=("--proxy-path", "/service")) as (port, process):
        for path in ("/service", "/service/", "/service/users?q=a%20b"):
            _, data = request(port, path)
            assert json.loads(data)["path"] == path
        _, data = request(port, "/service/echo", method="POST", body=b"a=1&b=2")
        assert json.loads(data)["body"] == b"a=1&b=2".hex()
        _, data = request(port, "/api")
        assert data == b"local api"
        request(port, "/servicex", status=404)
        request(port, "/other?q=/service/", status=404)
    with server(root, extra_options=("--proxy-path", "/service/v1", "--proxy",
                                     "127.0.0.1", str(backend_port))) as (port, process):
        _, data = request(port, "/service/v1/users")
        assert json.loads(data)["path"] == "/service/v1/users"
        request(port, "/service/v10/users", status=404)
    with server(root, backend_port, extra_options=("--proxy-path", "/")) as (port, process):
        _, data = request(port, "/")
        assert json.loads(data)["path"] == "/"  # 로컬 index.html보다 프록시 규칙이 우선한다.
        _, data = request(port, "/add", method="POST", body=b"a=1&b=2")
        assert json.loads(data)["path"] == "/add"

    with backend_server() as other:
        other_port = other.server_port
        for destinations in ((backend_port, other_port), (other_port, backend_port)):
            options = []
            for destination in destinations:
                options.extend(["--route", "/pool", "127.0.0.1", str(destination)])
            options.extend(["--route", "/pool/admin", "127.0.0.1", str(destinations[1])])
            with server(root, extra_options=options) as (port, process):
                for number in range(4):
                    _, data = request(port, "/pool/echo?x=1", method="POST", body=b"hello")
                    result = json.loads(data)
                    assert result["host"] == f"127.0.0.1:{destinations[number % 2]}"
                    assert result["path"] == "/pool/echo?x=1" and result["body"] == b"hello".hex()
                _, data = request(port, "/pool/admin/users")
                assert json.loads(data)["host"] == f"127.0.0.1:{destinations[1]}"
                _, data = request(port, "/pool/echo")
                assert json.loads(data)["host"] == f"127.0.0.1:{destinations[0]}"

                def load(number):
                    _, data = request(port, f"/pool/load?id={number}")
                    return json.loads(data)["host"]

                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
                    selected = list(executor.map(load, range(40)))
                assert selected.count(f"127.0.0.1:{destinations[0]}") == 20
                assert selected.count(f"127.0.0.1:{destinations[1]}") == 20
        with server(root, extra_options=("--route", "/pool", "127.0.0.1", str(free_port()),
                                         "--route", "/pool", "127.0.0.1", str(backend_port))) as (port, process):
            request(port, "/pool", status=502)  # 현재는 두 번째 백엔드로 자동 재전송하지 않는다.
            _, data = request(port, "/pool")
            assert json.loads(data)["host"] == f"127.0.0.1:{backend_port}"
            request(port, "/pool", status=502)
        eight = ["--route", "/pool", "127.0.0.1", str(backend_port)]
        for number in range(7):
            eight.extend(["--route", "/pool", "127.0.0.1", str(10000 + number)])
        with server(root, extra_options=eight) as (port, process):
            _, data = request(port, "/pool")
            assert json.loads(data)["host"] == f"127.0.0.1:{backend_port}"
        rules = [("/api", backend_port), ("/api/admin", other_port), ("/files", other_port)]
        for ordered in (rules, list(reversed(rules))):
            options = []
            for prefix, destination in ordered:
                options.extend(["--route", prefix, "127.0.0.1", str(destination)])
            with server(root, extra_options=options) as (port, process):
                for path, destination in (("/api", backend_port), ("/api/admin/users", other_port),
                                           ("/api/administrator", backend_port), ("/files?q=1", other_port),
                                           ("/api?next=/api/admin", backend_port)):
                    _, data = request(port, path)
                    result = json.loads(data)
                    assert result["path"] == path
                    assert result["host"] == f"127.0.0.1:{destination}"
                _, data = request(port, "/api/admin/echo", method="POST", body=b"hello")
                assert json.loads(data)["body"] == b"hello".hex()
                _, data = request(port, "/")
                assert data == b"static works"
                request(port, "/filesx", status=404)
        with server(root, extra_options=("--route", "/", "127.0.0.1", str(backend_port),
                                         "--route", "/files", "127.0.0.1", str(other_port))) as (port, process):
            _, data = request(port, "/files/a")
            assert json.loads(data)["host"] == f"127.0.0.1:{other_port}"
            _, data = request(port, "/other")
            assert json.loads(data)["host"] == f"127.0.0.1:{backend_port}"
        # 선택한 규칙이 실패해도 짧은 규칙으로 재전송하지 않는다.
        with server(root, extra_options=("--route", "/", "127.0.0.1", str(backend_port),
                                         "--route", "/files", "127.0.0.1", str(free_port()))) as (port, process):
            request(port, "/files/a", status=502)
            _, data = request(port, "/other")
            assert json.loads(data)["host"] == f"127.0.0.1:{backend_port}"
    maximum = []
    for number in range(16):
        maximum.extend(["--route", f"/rule{number}", "127.0.0.1", str(backend_port)])
    with server(root, extra_options=maximum) as (port, process):
        _, data = request(port, "/rule15")
        assert json.loads(data)["path"] == "/rule15"

    # 옵션을 생략하면 같은 /api 요청도 정적 파일 처리로 돌아간다.
    with server(root) as (port, process):
        _, data = request(port, "/api")
        assert data == b"local api"
    # 기존 CGI 위치 인자와 새 프록시 옵션을 함께 사용할 수 있다.
    with server(root, backend_port, cgi) as (port, process):
        _, data = request(port, "/cgi/add", method="POST", body=b"a=2&b=3")
        assert json.loads(data) == {"result": 5}
        _, data = request(port, "/api")
        assert json.loads(data)["path"] == "/api"
    with server(root, free_port()) as (port, process):
        request(port, "/api", status=502)
        request(port, "/")

    for stop_signal in (signal.SIGINT, signal.SIGTERM):
        backend.waiting.clear()
        backend.disconnected.clear()
        with server(root, backend_port) as (port, process):
            with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                client.sendall(b"GET /api/wait HTTP/1.1\r\nHost: x\r\n\r\n")
                assert backend.waiting.wait(2), "backend request did not arrive"
                process.send_signal(stop_signal)
                assert process.wait(timeout=1.5) == 0
                assert backend.disconnected.wait(2), "shutdown leaked backend socket"

print("Proxy CLI and HTTP integration checks passed")
