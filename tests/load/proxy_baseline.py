"""HTTP/1.1 프록시 경로를 wrk로 측정하고 모든 완료 응답의 본문을 검사한다."""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import re
import select
import shutil
import socket
import statistics
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
binary = str(args.binary.resolve())
lua = Path(__file__).with_name("verify_proxy.lua").resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
wrk = shutil.which("wrk")
if wrk is None:
    raise SystemExit("wrk is required")
payload = b"x" * 1024
runs = []
metadata = {
    "claim": "Local HTTP/1.1 proxy baseline; not a comparison with the ab direct-static run",
    "utc": datetime.now(timezone.utc).isoformat(), "platform": platform.platform(),
    "logical_cpus": os.cpu_count(), "binary": binary,
    "binary_sha256": hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
    "lua_sha256": hashlib.sha256(lua.read_bytes()).hexdigest(),
    "wrk": wrk, "threads": 1, "duration_seconds": 3, "repeats": 5,
    "concurrencies": [1, 4, 8], "payload_bytes": 1024, "http_version": "1.1",
    "connection_policy": "close on both legs; one backend; no reuse",
    "build_requirement": "CONCURRENCY=threadpool BUILD_MODE=release (-O2 -DNDEBUG)",
    "uptime_before": subprocess.check_output(["uptime"], text=True).strip(),
}
version = subprocess.run([wrk, "-v"], capture_output=True, text=True)
metadata["wrk_version"] = version.stdout + version.stderr


@contextlib.contextmanager
def server(root, name, options=()):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen([binary, str(port), str(root), *options],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        if not select.select([process.stdout], [], [], 3)[0]:
            raise RuntimeError("server startup timeout")
        if not process.stdout.readline().startswith("HTTP server listening"):
            raise RuntimeError("server startup failed")
        yield port
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            _, errors = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        (output / f"{name}.server.stderr.txt").write_text(errors)
        if process.returncode != 0:
            raise RuntimeError((name, process.returncode, errors))


def verify(port):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
    try:
        connection.request("GET", "/api/fixture.txt")
        response = connection.getresponse()
        if (response.status != 200 or response.read() != payload or
                response.getheader("Content-Length") != "1024" or
                response.getheader("Connection") != "close"):
            raise RuntimeError("response validation failed")
    finally:
        connection.close()


try:
    with tempfile.TemporaryDirectory(prefix="jungle-proxy-bench-") as directory:
        root = Path(directory)
        backend_root = root / "backend"
        proxy_root = root / "proxy"
        (backend_root / "api").mkdir(parents=True)
        proxy_root.mkdir()  # 같은 파일을 두지 않아 로컬 처리라면 검증에 실패하게 한다.
        (backend_root / "api/fixture.txt").write_bytes(payload)
        for repetition in range(5):
            conditions = (1, 4, 8)
            order = conditions[repetition % 3:] + conditions[:repetition % 3]
            for concurrency in order:
                name = f"run-{repetition + 1}-c{concurrency}"
                with server(backend_root, name + "-backend") as backend:
                    with server(proxy_root, name + "-proxy", ["--route", "/api", "127.0.0.1", str(backend)]) as port:
                        for _ in range(100):
                            verify(port)
                        command = [wrk, "-t1", f"-c{concurrency}", "-d3s", "--timeout", "5s",
                                   "-s", str(lua), f"http://127.0.0.1:{port}/api/fixture.txt"]
                        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
                        (output / f"{name}.txt").write_text(result.stdout)
                        (output / f"{name}.stderr.txt").write_text(result.stderr)
                        match = re.search(r"^CHECK_JSON (.+)$", result.stdout, re.MULTILINE)
                        if result.returncode != 0 or match is None:
                            raise RuntimeError((name, result.returncode, result.stderr))
                        row = json.loads(match.group(1))
                        row.update(repetition=repetition + 1, concurrency=concurrency, command=command,
                                   backend_port=backend, proxy_port=port)
                        row["requests_per_s"] = row["completed"] * 1000000 / row["duration_us"]
                        row["passed"] = (row["completed"] > 0 and row["completed"] == row["observed"] == row["valid"] and
                                         all(row[key] == 0 for key in ("invalid", "non_200", "connect_errors",
                                             "read_errors", "write_errors", "timeouts", "status_errors")))
                        runs.append(row)
                        print(f"run={repetition + 1} c={concurrency} rps={row['requests_per_s']:.2f} pass={row['passed']}", flush=True)
                        verify(port)
    summary = []
    for concurrency in (1, 4, 8):
        rows = [row for row in runs if row["concurrency"] == concurrency]
        rates = [row["requests_per_s"] for row in rows]
        summary.append({"concurrency": concurrency, "runs": len(rows),
                        "rps_median": statistics.median(rates), "rps_min": min(rates), "rps_max": max(rates),
                        "completed": sum(row["completed"] for row in rows),
                        "invalid": sum(row["invalid"] for row in rows),
                        "non_200": sum(row["non_200"] for row in rows),
                        "socket_errors": sum(row[key] for row in rows for key in
                                             ("connect_errors", "read_errors", "write_errors", "timeouts")),
                        "passed": all(row["passed"] for row in rows)})
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    if not all(row["passed"] for row in runs):
        raise SystemExit("Some runs failed validation; inspect retained raw results")
finally:
    metadata["uptime_after"] = subprocess.check_output(["uptime"], text=True).strip()
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (output / "runs.json").write_text(json.dumps(runs, indent=2) + "\n")
