"""동시성 테스트 — 유저 시나리오: 여러 이용자가 동시에 다양한 기능을 써도 딜레이가 없어야 한다.

실행: make test   (서버·CGI 빌드 후 이 파일 실행)
- 시나리오마다 빈 포트에 서버를 새로 띄우고 끝나면 끔 (앞 시나리오가 서버를 멈춰도 뒤에 영향 없음)
- 동시 인원 N = 1 → 10 → 100, 단계별 시간을 출력
- 합격 기준: 다른 손님 B의 응답 ≤ 100ms / 느린 CGI는 ⌈N/WORKERS⌉ × 1초 + 100ms
- 요청당 타임아웃: max(5초, 기준 + 1초). 넘으면 '응답 없음'으로 실패
"""
import math
import os
import socket
import subprocess
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURES = REPO / "tests" / "fixtures"
WWW = FIXTURES / "www"
BIG = WWW / "big.bin"            # 테스트가 매번 생성 (.gitignore)

STEPS = (1, 10, 100)
FAST_MS = 100                    # '딜레이 없음' 기준
WORKERS = 8                      # 서버가 최소 8명을 동시에 처리해야 한다는 요구사항
CGI_SLEEP = 1.0                  # cgi-src/sleep.c가 자는 시간
BASE_TIMEOUT = 5.0


def timeout_for(limit_s):
    return max(BASE_TIMEOUT, limit_s + 1.0)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get(port, path, timeout):
    """GET 하나 보내고 끝(EOF)까지 받기. → (상태 코드, 본문, 걸린 초). 타임아웃·연결 실패면 상태 0"""
    t0 = time.monotonic()
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
            s.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
            data = b""
            while chunk := s.recv(65536):
                data += chunk
                if time.monotonic() - t0 > timeout:
                    raise socket.timeout
    except OSError:              # socket.timeout 포함
        return 0, b"", time.monotonic() - t0
    head, _, body = data.partition(b"\r\n\r\n")
    parts = head.split(b" ")
    status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
    return status, body, time.monotonic() - t0


def report(name, n, elapsed, limit, ok):
    shown = "응답 없음" if elapsed is None else f"{elapsed * 1000:8.1f}ms"
    print(f"\n  [{name}] N={n:<3} {shown}  (기준 {limit * 1000:.0f}ms)  {'PASS' if ok else 'FAIL'}", end="")


class ServerTest(unittest.TestCase):
    """시나리오마다 새 서버. 붙잡아 둔 손님 소켓은 self.held에 모아 끝에 닫음"""

    @classmethod
    def setUpClass(cls):
        if not BIG.exists():
            BIG.write_bytes(os.urandom(3 * 1024 * 1024))

    def start_server(self):
        self.held = []
        self.port = free_port()
        self.proc = subprocess.Popen(
            [str(REPO / "build" / "server"), "-c", str(FIXTURES / "test.conf"), "-p", str(self.port)],
            cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:   # 포트가 열릴 때까지 대기
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                time.sleep(0.05)
                return
            except OSError:
                time.sleep(0.02)
        self.fail("서버가 2초 안에 시작되지 않음 (make 했는지 확인)")

    def stop_server(self):
        for s in self.held:
            s.close()
        self.proc.kill()
        self.proc.wait()

    def hold(self, raw, rcvbuf=None):
        """손님 하나 연결 → raw만 보내고 붙잡아 둠 (읽지도 닫지도 않음)"""
        s = socket.socket()
        if rcvbuf:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)   # 작게 → 서버 write가 빨리 막힘
        # 연결은 0.5초만 기다림: 대기열(BACKLOG)이 차면 connect가 막히는데, 5초씩 기다리면 N=100에서 수백 초 걸림
        s.settimeout(0.5)
        try:
            s.connect(("127.0.0.1", self.port))
            s.settimeout(BASE_TIMEOUT)
            s.sendall(raw)
        except OSError:
            pass                 # 대기열이 차서 연결 실패해도 시나리오는 계속 (B의 결과로 판정)
        self.held.append(s)

    def check_other_guest(self, name, make_slow_guest, steps):
        """N명이 서버를 붙잡은 동안 다른 손님 B의 GET /가 FAST_MS 안에 와야 함"""
        limit = FAST_MS / 1000
        for n in steps:
            with self.subTest(N=n):
                self.start_server()
                try:
                    for _ in range(n):
                        make_slow_guest()
                    time.sleep(0.2)          # 서버가 느린 손님을 붙잡을 시간
                    status, _, elapsed = get(self.port, "/", timeout_for(limit))
                    ok = status == 200 and elapsed <= limit
                    report(name, n, elapsed if status else None, limit, ok)
                    self.assertEqual(status, 200, "B가 응답을 못 받음")
                    self.assertLessEqual(elapsed, limit, "B가 기준보다 늦게 받음")
                finally:
                    self.stop_server()


SLOW_BODY = (b"POST /cgi-bin/adder HTTP/1.1\r\nHost: localhost\r\n"
             b"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 100\r\n\r\n"
             b"a=1&b=2&c=")   # 10바이트
SLOW_DOWNLOAD = b"GET /big.bin HTTP/1.1\r\nHost: localhost\r\n\r\n"


class TestConcurrency(ServerTest):

    def test_1_slow_request_body(self):
        """느린 손님: Content-Length: 100인데 본문 10바이트만 보내고 멈춤 → 그동안 B"""
        self.check_other_guest("느린 본문", lambda: self.hold(SLOW_BODY), (1,))

    def test_2_slow_cgi(self):
        """1초 걸리는 CGI를 N명이 동시에 → 전체가 ⌈N/WORKERS⌉ × 1초 + 100ms 안에"""
        self.check_slow_cgi((1, 10))

    def check_slow_cgi(self, steps):
        for n in steps:
            limit = math.ceil(n / WORKERS) * CGI_SLEEP + FAST_MS / 1000
            with self.subTest(N=n):
                self.start_server()
                try:
                    t0 = time.monotonic()
                    with ThreadPoolExecutor(max_workers=n) as pool:
                        results = list(pool.map(lambda _: get(self.port, "/cgi-bin/sleep", timeout_for(limit)), range(n)))
                    total = time.monotonic() - t0
                    ok_count = sum(1 for st, _, _ in results if st == 200)
                    ok = ok_count == n and total <= limit
                    report("느린 CGI", n, total, limit, ok)
                    self.assertEqual(ok_count, n, f"{n}명 중 {ok_count}명만 200")
                    self.assertLessEqual(total, limit, "전체 시간이 기준 초과")
                finally:
                    self.stop_server()

    def test_3_mixed_static_and_cgi(self):
        """정적+CGI를 N명이 섞어서 동시에 → 전부 200, 본문 일치 (시간은 안 봄)"""
        expected = {
            "/": (WWW / "index.html").read_bytes(),
            "/css/style.css": (WWW / "css" / "style.css").read_bytes(),
            "/cgi-bin/adder?a=1&b=2": b"1 + 2 = 3\n",
        }
        paths = list(expected)
        for n in STEPS:
            with self.subTest(N=n):
                self.start_server()
                try:
                    t0 = time.monotonic()
                    with ThreadPoolExecutor(max_workers=n) as pool:
                        results = list(pool.map(lambda i: (paths[i % 3], get(self.port, paths[i % 3], BASE_TIMEOUT)), range(n)))
                    total = time.monotonic() - t0
                    bad = [(p, st) for p, (st, body, _) in results if st != 200 or body != expected[p]]
                    report("정적+CGI 혼합", n, total, BASE_TIMEOUT, not bad)
                    self.assertFalse(bad, f"실패 {len(bad)}건: {bad[:5]}")
                finally:
                    self.stop_server()

    def test_4_slow_download(self):
        """느린 수신: 3MB 파일을 요청해 놓고 안 읽음 → 그동안 B"""
        self.check_other_guest("느린 수신", lambda: self.hold(SLOW_DOWNLOAD, rcvbuf=4096), (1,))


class TestKnownLimits(ServerTest):
    """알려진 한계: 스레드 풀(워커 8)이라 실패가 정상 → expectedFailure (실패하면 통과로 셈)
    주의: 실패 '이유'가 바뀌어도 모름 (예: 깨뜨려 보기 C에서 느린 CGI N=100이 80명 → 24명으로 줄어도 통과로 보임)
    이 클래스가 '예상 밖 성공'(unexpected success)이 되면 한계가 풀린 것 → 일반 테스트로 옮길 것"""

    @unittest.expectedFailure
    def test_1_slow_request_body_over_workers(self):
        """느린 손님이 워커 수 이상 → B는 read 타임아웃(5초)까지 대기"""
        self.check_other_guest("느린 본문", lambda: self.hold(SLOW_BODY), (10, 100))

    @unittest.expectedFailure
    def test_2_slow_cgi_100(self):
        """100명 → 워커 8 + 큐 16을 넘은 연결이 OS 대기열(BACKLOG 10)에서 밀려 재시도 (추정)"""
        TestConcurrency.check_slow_cgi(self, (100,))

    @unittest.expectedFailure
    def test_4_slow_download_over_workers(self):
        """안 읽는 손님이 워커 수 이상 → write 타임아웃이 없어 워커가 영원히 묶임"""
        self.check_other_guest("느린 수신", lambda: self.hold(SLOW_DOWNLOAD, rcvbuf=4096), (10, 100))


if __name__ == "__main__":
    unittest.main(verbosity=2)
