"""Check simultaneous TCP connections and optionally send concurrent HTTP requests."""

import argparse
import asyncio
from collections import Counter, deque
from contextlib import ExitStack, suppress
from http.client import HTTPException, HTTPResponse
import re
import socket


HOST = "127.0.0.1"
TIMEOUT = 3
CONNECT_BATCH_SIZE = 32
KEEP_ALIVE_INTERVAL = 3
AB_REQUESTS = 10_000


def check_capacity(label, port, authority_port, count, backend_ports=()):
    authority = f"{HOST}:{authority_port}"
    target = "/" if port == authority_port else f"http://{authority}/"
    request = (
        f"GET {target} HTTP/1.1\r\nHost: {authority}\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n"
    ).encode("ascii")

    with ExitStack() as sockets:
        clients = []
        for _ in range(count):
            try:
                client = socket.create_connection((HOST, port), timeout=TIMEOUT)
            except OSError as error:
                print(f"[실패] {label} :{port} · TCP 연결 {len(clients)}/{count}개 · {error}")
                return False
            clients.append(sockets.enter_context(client))

        # Send requests after every socket has connected. Each socket is used only once.
        passed = 0
        first_error = None
        for number, client in enumerate(clients, start=1):
            try:
                client.sendall(request)
                with HTTPResponse(client) as response:
                    response.begin()
                    body = response.read()
                    backend = response.getheader("X-Backend-Port")
                    if response.status != 200 or body != b"hello":
                        raise ValueError(f"HTTP {response.status}, 본문 {body[:40]!r}")
                    if backend_ports and backend not in backend_ports:
                        raise ValueError(f"예상과 다른 X-Backend-Port {backend!r}")
            except (OSError, HTTPException, ValueError) as error:
                if first_error is None:
                    first_error = f"연결 {number}번 · {error}"
            else:
                passed += 1
            finally:
                client.close()

    result = "통과" if passed == count else "실패"
    print(f"[{result}] {label} :{port} · 동시 TCP 연결 {count}개 중 응답 검증 {passed}/{count}")
    if first_error is not None:
        print(f"  첫 오류 · {first_error}")
    return passed == count


async def run_ab(port, authority_port, concurrency, show_status):
    print(f"ab 시작 · 동시성 {concurrency} · 총 {AB_REQUESTS}건 · Keep-Alive 끔", flush=True)
    verbosity = ("-v", "2") if show_status else ()
    process = await asyncio.create_subprocess_exec(
        "/usr/sbin/ab", *verbosity, "-n", str(AB_REQUESTS), "-c", str(concurrency), "-s", "10",
        "-X", f"{HOST}:{port}", f"http://{HOST}:{authority_port}/",
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
    )
    try:
        output, _ = await process.communicate()
    except asyncio.CancelledError:
        with suppress(ProcessLookupError):
            process.kill()
        await process.communicate()
        raise

    report = output.decode(errors="replace")
    stats = dict(re.findall(
        r"^(Complete requests|Failed requests|Non-2xx responses):[ \t]*(\d+)", report, re.MULTILINE
    ))
    if show_status:
        error_codes = Counter(re.findall(
            r"^WARNING: Response code not 2xx \((\d{3})\)", report, re.MULTILINE
        ))
        for status, count in sorted(error_codes.items()):
            print(f"ab HTTP {status} · {count}건", flush=True)
        _, separator, summary = report.partition("\nServer Software:")
        report = separator.lstrip() + summary if separator else "\n".join(report.splitlines()[-12:]) + "\n"
    print(report, end="", flush=True)
    if (process.returncode != 0 or stats.get("Complete requests") != str(AB_REQUESTS)
            or stats.get("Failed requests") != "0" or stats.get("Non-2xx responses", "0") != "0"):
        raise ValueError("ab 요청 완료 수, 실패 수 또는 HTTP 상태 검사에 실패했습니다")


async def check_concurrent_requests(port, authority_port, count, rounds, backend_ports,
                                   hold_seconds, ab_concurrency, ab_show_status):
    authority = f"{HOST}:{authority_port}"
    target = "/" if port == authority_port else f"http://{authority}/"
    request = (
        f"GET {target} HTTP/1.1\r\nHost: {authority}\r\n"
        "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n"
    ).encode("ascii")

    async def exchange(reader, writer):
        writer.write(request)
        await writer.drain()
        head = await reader.readuntil(b"\r\n\r\n")
        status_line, *fields = head[:-4].split(b"\r\n")
        headers = {name.lower(): value.strip()
                   for name, value in (line.split(b":", 1) for line in fields)}
        if status_line.split()[1:2] != [b"200"]:
            raise ValueError(f"HTTP 응답 {status_line.decode('ascii', errors='replace')}")
        if headers.get(b"connection", b"").lower() != b"keep-alive":
            raise ValueError("서버가 연결을 유지하지 않았습니다")
        backend = headers.get(b"x-backend-port", b"").decode("ascii")
        if backend_ports and backend not in backend_ports:
            raise ValueError(f"예상과 다른 X-Backend-Port {backend!r}")
        if int(headers.get(b"content-length", b"-1")) != len(b"hello"):
            raise ValueError("예상과 다른 Content-Length")
        if await reader.readexactly(len(b"hello")) != b"hello":
            raise ValueError("예상과 다른 응답 본문")

    clients = []
    loop = asyncio.get_running_loop()
    batches = deque()
    label = "전체 프록시 경로" if backend_ports else "원 서버"

    async def open_client():
        client = await asyncio.wait_for(asyncio.open_connection(HOST, port), TIMEOUT)
        clients.append(client)
        await asyncio.wait_for(exchange(*client), TIMEOUT)
        return client

    async def run_batch(operations):
        pending = asyncio.gather(*operations, return_exceptions=True)
        results = await asyncio.wait_for(pending, TIMEOUT)
        for result in results:
            if isinstance(result, Exception):
                raise result
        return results

    async def refresh_batch():
        _, batch = batches.popleft()
        refresh_at = loop.time() + KEEP_ALIVE_INTERVAL
        await run_batch(exchange(*client) for client in batch)
        batches.append((refresh_at, batch))
        return len(batch)

    async def hold_connections():
        print(f"{label} :{port} · 연결 {count}개 준비 중", flush=True)
        for offset in range(0, count, CONNECT_BATCH_SIZE):
            if batches and batches[0][0] <= loop.time():
                await refresh_batch()
            size = min(CONNECT_BATCH_SIZE, count - offset)
            refresh_at = loop.time() + KEEP_ALIVE_INTERVAL
            batch = await run_batch(open_client() for _ in range(size))
            batches.append((refresh_at, batch))

        print(f"연결 {len(clients)}/{count}개 준비 완료 · {hold_seconds}초 유지 시작", flush=True)
        deadline = loop.time() + hold_seconds
        ab_task = None
        if ab_concurrency:
            ab_task = asyncio.create_task(run_ab(port, authority_port, ab_concurrency, ab_show_status))
        ab_timed_out = False
        responses = 0
        try:
            while loop.time() < deadline:
                await asyncio.sleep(min(deadline, batches[0][0]) - loop.time())
                if loop.time() >= deadline:
                    break
                responses += await refresh_batch()

            ab_timed_out = ab_task is not None and not ab_task.done()
            if ab_timed_out:
                ab_task.cancel()

            # Verify every original connection after the hold period without reconnecting.
            for _ in range(len(batches)):
                responses += await refresh_batch()
            print(f"[통과] {label} · TCP 연결 {count}개를 {hold_seconds}초 유지 · 응답 검증 {responses}건")

            if ab_task is not None:
                if ab_timed_out:
                    raise ValueError("ab가 연결 유지 시간 안에 끝나지 않았습니다")
                ab_task.result()
                print(f"[통과] ab · {AB_REQUESTS}건 완료 · 실패 0건")
        finally:
            if ab_task is not None:
                if not ab_timed_out:
                    ab_task.cancel()
                await asyncio.gather(ab_task, return_exceptions=True)

    try:
        if hold_seconds:
            await hold_connections()
            return True

        for _ in range(count):
            await open_client()

        print(f"{label} :{port} · 연결 {count}개 준비 완료")
        for number in range(1, rounds + 1):
            exchanges = asyncio.gather(*(exchange(*client) for client in clients), return_exceptions=True)
            results = await asyncio.wait_for(exchanges, TIMEOUT)
            failures = [result for result in results if isinstance(result, Exception)]
            print(f"{number}/{rounds}회 · 응답 검증 {count - len(failures)}/{count}")
            if failures:
                raise failures[0]
    except (OSError, ValueError, EOFError, asyncio.LimitOverrunError, asyncio.TimeoutError) as error:
        print(f"[실패] {label} · 열린 연결 {len(clients)}/{count}개 · {type(error).__name__} {error}")
        return False
    finally:
        for _, writer in clients:
            writer.close()
        await asyncio.gather(*(writer.wait_closed() for _, writer in clients), return_exceptions=True)

    print(f"[통과] {label} · 동시 요청 {count}개씩 총 {count * rounds}건")
    return True


def main():
    parser = argparse.ArgumentParser(description="실행 중인 원 서버와 프록시의 동시 연결 한도를 확인합니다.")
    parser.add_argument("--connections", type=int, default=256)
    parser.add_argument("--origin-ports", type=int, nargs=2, default=(9000, 9001))
    parser.add_argument("--reverse-port", type=int, default=9200)
    parser.add_argument("--forward-port", type=int, default=9100)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--concurrent-rounds", type=int, default=0,
                       help="연결을 준비한 뒤 동시에 요청할 횟수")
    modes.add_argument("--hold-seconds", type=int, default=0,
                       help="같은 연결을 열어 두고 응답을 확인할 시간(초)")
    parser.add_argument("--origin-only", action="store_true",
                        help="첫 번째 원 서버에 직접 접속해 지속 연결을 검사")
    parser.add_argument("--ab-concurrency", type=int, default=0,
                        help="연결 유지 중 전체 경로로 실행할 ApacheBench 동시 요청 수")
    parser.add_argument("--ab-show-status", action="store_true",
                        help="ApacheBench의 HTTP 오류 상태 코드별 건수를 출력")
    args = parser.parse_args()
    if args.connections < 1:
        parser.error("--connections에는 1 이상의 정수를 입력하세요")
    if args.concurrent_rounds < 0:
        parser.error("--concurrent-rounds에는 0 이상의 정수를 입력하세요")
    if args.hold_seconds < 0:
        parser.error("--hold-seconds에는 0 이상의 정수를 입력하세요")
    if args.origin_only and not (args.concurrent_rounds or args.hold_seconds):
        parser.error("--origin-only에는 --hold-seconds 또는 --concurrent-rounds가 필요합니다")
    if not 0 <= args.ab_concurrency <= AB_REQUESTS:
        parser.error(f"--ab-concurrency에는 0~{AB_REQUESTS} 범위의 정수를 입력하세요")
    if args.ab_concurrency and (not args.hold_seconds or args.origin_only):
        parser.error("--ab-concurrency에는 전체 프록시 경로의 --hold-seconds가 필요합니다")
    if args.ab_show_status and not args.ab_concurrency:
        parser.error("--ab-show-status에는 --ab-concurrency가 필요합니다")

    origins = tuple(str(port) for port in args.origin_ports)
    origin_1, origin_2 = args.origin_ports
    if args.concurrent_rounds or args.hold_seconds:
        port = origin_1 if args.origin_only else args.forward_port
        authority_port = origin_1 if args.origin_only else args.reverse_port
        backends = () if args.origin_only else origins
        passed = asyncio.run(check_concurrent_requests(
            port, authority_port, args.connections, args.concurrent_rounds,
            backends, args.hold_seconds, args.ab_concurrency, args.ab_show_status,
        ))
        return 0 if passed else 1

    routes = (
        ("원 서버 1", origin_1, origin_1, ()),
        ("원 서버 2", origin_2, origin_2, ()),
        ("리버스 → 원 서버", args.reverse_port, args.reverse_port, origins),
        ("포워드 → 리버스 → 원 서버", args.forward_port, args.reverse_port, origins),
    )
    results = [
        check_capacity(label, port, authority_port, args.connections, backend_ports)
        for label, port, authority_port, backend_ports in routes
    ]
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
