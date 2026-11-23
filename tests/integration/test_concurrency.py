"""실행 방식별 대기, 병렬 요청, 큐 포화 거절과 종료를 검증한다."""
import concurrent.futures
import contextlib
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
mode = sys.argv[2]
assert mode in ("sequential", "threadpool")


@contextlib.contextmanager
def server(root):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen([binary, str(port), str(root)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert process.stdout.readline().startswith("HTTP server listening")
        yield process, port
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


def connect(port, complete=False):
    client = socket.create_connection(("127.0.0.1", port), timeout=3)
    client.sendall(b"GET / HTTP/1.1\r\n" + (b"Host: x\r\n\r\n" if complete else b""))
    return client


def read_response(client):
    response = http.client.HTTPResponse(client)
    response.begin()
    assert response.status == 200
    assert response.read() == b"hello"


def expect_closed(client):
    client.settimeout(2)
    try:
        assert client.recv(1) == b"", "unexpected response from rejected/closed connection"
    except ConnectionResetError:
        pass


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    (root / "index.html").write_bytes(b"hello")
    with server(root) as (process, port):
        with connect(port) as slow, connect(port, complete=True) as fast:
            if mode == "sequential":
                fast.settimeout(0.2)
                try:
                    fast.recv(1, socket.MSG_PEEK)
                    raise AssertionError("sequential server handled another connection")
                except socket.timeout:
                    pass
                slow.close()
                fast.settimeout(3)
            read_response(fast)

        def add(number):
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                connection.request("POST", "/add", body=f"a={number}&b=1")
                response = connection.getresponse()
                assert response.status == 200
                assert json.loads(response.read()) == {"result": number + 1}
            finally:
                connection.close()

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(add, range(40)))

    if mode == "threadpool":
        with server(root) as (process, port):
            clients = []
            try:
                # 4개 작업자를 불완전한 요청으로 점유한다.
                clients.extend(connect(port) for _ in range(4))
                queued = connect(port, complete=True)
                clients.append(queued)
                queued.settimeout(0.2)
                try:
                    queued.recv(1, socket.MSG_PEEK)
                    raise AssertionError("more than four workers handled requests")
                except socket.timeout:
                    pass
                # 이미 대기 중인 1개 + 아래 7개로 큐 8칸을 채운다.
                clients.extend(connect(port) for _ in range(7))
                with connect(port, complete=True) as rejected:
                    expect_closed(rejected)
                process.terminate()
                assert process.wait(timeout=3) == 0
                for client in clients:
                    expect_closed(client)
            finally:
                for client in clients:
                    client.close()

print(f"Concurrency checks passed: {mode}")
