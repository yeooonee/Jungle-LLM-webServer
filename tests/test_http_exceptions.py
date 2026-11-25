"""Integration tests for HTTP errors on isolated loopback ports."""

from contextlib import closing
from http.client import HTTPConnection, HTTPResponse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOST = "127.0.0.1"
BODY_LIMIT = 64 * 1024
HEADER_LIMIT = 8 * 1024
IO_TIMEOUT = 8


def stop_server(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def send_request(port, target, method="GET", body=None, headers=None):
    with closing(HTTPConnection(HOST, port, timeout=IO_TIMEOUT)) as connection:
        connection.request(method, target, body=body, headers={"Connection": "close", **(headers or {})})
        with connection.getresponse() as response:
            response.read()
            return response.status, response.getheader("X-Backend-Port")


def read_status(sock):
    with HTTPResponse(sock) as response:
        response.begin()
        response.read()
        return response.status


class HTTPExceptionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        workspace = tempfile.TemporaryDirectory(prefix="jungle-http-tests-")
        cls.addClassCleanup(workspace.cleanup)
        cls.directory = Path(workspace.name)
        cls.build_dir = cls.directory / "build"
        subprocess.run(
            ["make", f"BUILD_DIR={cls.build_dir}", "tcp_server", "tcp_server_forward", "tcp_server_reverse"],
            cwd=ROOT,
            check=True,
        )

    def start_server(self, name, *args):
        with closing(socket.socket()) as reservation:
            reservation.bind((HOST, 0))
            port = reservation.getsockname()[1]

        log_path = self.directory / f"{name}-{port}.log"
        env = os.environ.copy()
        env.pop("JUNGLE_ORIGIN_TOKEN", None)
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [self.build_dir / name, "-p", str(port), *args],
                cwd=ROOT,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
        self.addCleanup(stop_server, process)

        deadline = time.monotonic() + 3
        while process.poll() is None and time.monotonic() < deadline:
            try:
                with socket.create_connection((HOST, port), timeout=0.2):
                    return port, process
            except ConnectionRefusedError:
                time.sleep(0.02)
        self.fail(f"{name} did not start on port {port}:\n{log_path.read_text()}")

    def start_chain(self):
        origin, _ = self.start_server("tcp_server")
        reverse, _ = self.start_server("tcp_server_reverse", "-b", str(origin))
        forward, _ = self.start_server("tcp_server_forward", "-o", str(reverse))
        return forward, reverse, origin

    def wait_for_status(self, port, expected):
        deadline = time.monotonic() + 4
        while True:
            status, _ = send_request(port, "/health")
            if status == expected:
                return
            if time.monotonic() >= deadline:
                self.fail(f"Expected {expected} from /health, last response was {status}")
            time.sleep(0.05)

    def test_body_at_64_kib_is_forwarded(self):
        forward, reverse, origin = self.start_chain()
        status, backend = send_request(
            forward, f"http://{HOST}:{reverse}/submit", "POST", b"a" * BODY_LIMIT
        )
        self.assertEqual(status, 200)
        self.assertEqual(backend, str(origin))

    def test_body_over_64_kib_is_rejected(self):
        forward, reverse, _ = self.start_chain()
        # With Expect, the client waits for permission before sending the declared body.
        request = (
            f"POST http://{HOST}:{reverse}/submit HTTP/1.1\r\nHost: {HOST}:{reverse}\r\n"
            f"Content-Length: {BODY_LIMIT + 1}\r\nExpect: 100-continue\r\n"
            "Connection: close\r\n\r\n"
        ).encode("ascii")
        with socket.create_connection((HOST, forward), timeout=IO_TIMEOUT) as client:
            client.sendall(request)
            self.assertEqual(read_status(client), 413)

    def test_headers_over_8_kib_are_rejected(self):
        origin, _ = self.start_server("tcp_server")
        prefix = f"GET / HTTP/1.1\r\nHost: {HOST}:{origin}\r\nX-Test: ".encode("ascii")
        suffix = b"\r\n\r\n"
        padding = b"a" * (HEADER_LIMIT + 1 - len(prefix) - len(suffix))
        with socket.create_connection((HOST, origin), timeout=IO_TIMEOUT) as client:
            client.sendall(prefix + padding + suffix)
            self.assertEqual(read_status(client), 431)

    def test_long_uri_exceeds_8_kib_header_limit(self):
        origin, _ = self.start_server("tcp_server")
        prefix = b"GET /"
        suffix = f" HTTP/1.1\r\nHost: {HOST}:{origin}\r\n\r\n".encode("ascii")
        path = b"a" * (HEADER_LIMIT + 1 - len(prefix) - len(suffix))
        with socket.create_connection((HOST, origin), timeout=IO_TIMEOUT) as client:
            client.sendall(prefix + path + suffix)
            self.assertEqual(read_status(client), 431)

    def test_incomplete_headers_are_rejected_on_eof(self):
        origin, _ = self.start_server("tcp_server")
        request = f"GET / HTTP/1.1\r\nHost: {HOST}:{origin}\r\n".encode("ascii")
        with socket.create_connection((HOST, origin), timeout=IO_TIMEOUT) as client:
            client.sendall(request)
            client.shutdown(socket.SHUT_WR)
            self.assertEqual(read_status(client), 400)
            self.assertEqual(client.recv(1), b"")

    def test_missing_host_is_rejected(self):
        origin, _ = self.start_server("tcp_server")
        with socket.create_connection((HOST, origin), timeout=IO_TIMEOUT) as client:
            client.sendall(b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n")
            self.assertEqual(read_status(client), 400)

    def test_invalid_and_unsupported_http_versions_are_rejected(self):
        origin, _ = self.start_server("tcp_server")
        for version, expected in (("HTTP/1.x", 400), ("HTTP/1.2", 505)):
            with self.subTest(version=version):
                request = f"GET / {version}\r\nHost: {HOST}:{origin}\r\n\r\n".encode("ascii")
                with socket.create_connection((HOST, origin), timeout=IO_TIMEOUT) as client:
                    client.sendall(request)
                    self.assertEqual(read_status(client), expected)

    def test_unsupported_method_is_rejected(self):
        forward, reverse, _ = self.start_chain()
        status, _ = send_request(forward, f"http://{HOST}:{reverse}/submit", "PUT")
        self.assertEqual(status, 405)

    def test_backend_connection_failure_returns_502(self):
        origin, process = self.start_server("tcp_server")
        reverse, _ = self.start_server("tcp_server_reverse", "-b", str(origin))
        self.assertEqual(send_request(reverse, "/health")[0], 200)
        stop_server(process)
        self.assertEqual(send_request(reverse, "/health")[0], 502)

    def test_failed_health_check_returns_503(self):
        origin, process = self.start_server("tcp_server")
        reverse, _ = self.start_server("tcp_server_reverse", "-b", str(origin), "-H", "1")
        self.wait_for_status(reverse, 200)
        stop_server(process)
        self.wait_for_status(reverse, 503)

    def test_backend_response_timeout_returns_504(self):
        with closing(socket.socket()) as backend:
            backend.bind((HOST, 0))
            backend.listen(1)
            backend.settimeout(3)
            reverse, _ = self.start_server("tcp_server_reverse", "-b", str(backend.getsockname()[1]))

            with closing(HTTPConnection(HOST, reverse, timeout=IO_TIMEOUT)) as client:
                start = time.monotonic()
                client.request("GET", "/health", headers={"Connection": "close"})
                upstream, _ = backend.accept()
                with upstream:
                    upstream.settimeout(3)
                    with upstream.makefile("rb") as request:
                        self.assertEqual(request.readline(), b"GET /health HTTP/1.1\r\n")
                    # Keep the accepted TCP connection open without a response.
                    with client.getresponse() as response:
                        self.assertEqual(response.status, 504)
                        self.assertGreaterEqual(time.monotonic() - start, 4.5)
                        response.read()

    def test_partial_post_disconnect_closes_connection(self):
        forward, reverse, origin = self.start_chain()
        for port, target in (
            (origin, "/submit"),
            (reverse, "/submit"),
            (forward, f"http://{HOST}:{reverse}/submit"),
        ):
            with self.subTest(port=port):
                request = (
                    f"POST {target} HTTP/1.1\r\nHost: {HOST}:{reverse}\r\n"
                    "Content-Length: 5\r\n\r\nhe"
                ).encode("ascii")
                with socket.create_connection((HOST, port), timeout=IO_TIMEOUT) as client:
                    client.sendall(request)
                    client.shutdown(socket.SHUT_WR)
                    self.assertEqual(read_status(client), 400)
                    self.assertEqual(client.recv(1), b"")

                status, backend = send_request(forward, f"http://{HOST}:{reverse}/submit", "POST", b"hello")
                self.assertEqual(status, 200)
                self.assertEqual(backend, str(origin))

    def test_partial_post_tcp_reset_leaves_servers_responsive(self):
        forward, reverse, origin = self.start_chain()
        proxy_target = f"http://{HOST}:{reverse}/submit"
        for port, target in ((origin, "/submit"), (reverse, "/submit"), (forward, proxy_target)):
            with self.subTest(port=port):
                request = (
                    f"POST {target} HTTP/1.1\r\nHost: {HOST}:{reverse}\r\n"
                    "Content-Length: 5\r\nExpect: 100-continue\r\n\r\nhe"
                ).encode("ascii")
                with socket.create_connection((HOST, port), timeout=IO_TIMEOUT) as client:
                    client.sendall(request)
                    with client.makefile("rb") as response:
                        self.assertEqual(response.readline(), b"HTTP/1.1 100 Continue\r\n")
                        self.assertEqual(response.readline(), b"\r\n")
                    # Closing with zero linger sends a TCP reset instead of an EOF.
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))

                status, backend = send_request(forward, proxy_target, "POST", b"hello")
                self.assertEqual(status, 200)
                self.assertEqual(backend, str(origin))


if __name__ == "__main__":
    unittest.main(verbosity=2)
