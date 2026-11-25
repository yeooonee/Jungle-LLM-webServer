"""직접 정적 파일 경로의 ab 기준값을 측정하고 원본 결과를 보관한다."""
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
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
ab = shutil.which("ab")
if ab is None:
    raise SystemExit("ab is required")
payload = b"x" * 1024
requests = 60000
repeats = 5
concurrencies = (1, 4, 8)
runs = []
metadata = {
    "claim": "Local direct-static baseline only; no capacity or bottleneck claim",
    "utc": datetime.now(timezone.utc).isoformat(),
    "platform": platform.platform(), "logical_cpus": os.cpu_count(),
    "binary": binary, "binary_sha256": hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
    "ab": ab, "body_bytes": len(payload),
    "requests_per_run": requests, "repeats": repeats,
    "concurrencies": concurrencies, "keep_alive": False,
    "build_requirement": "CONCURRENCY=threadpool BUILD_MODE=release (-O2 -DNDEBUG)",
    "uptime_before": subprocess.check_output(["uptime"], text=True).strip(),
    "ab_version": subprocess.check_output([ab, "-V"], text=True),
}


@contextlib.contextmanager
def server(root):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen([binary, str(port), str(root)], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
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
        if process.returncode != 0:
            raise RuntimeError((process.returncode, errors))


def verify(port):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        connection.request("GET", "/fixture.txt")
        response = connection.getresponse()
        body = response.read()
        if (response.status != 200 or body != payload or
                response.getheader("Content-Length") != str(len(payload)) or
                response.getheader("Connection") != "close"):
            raise RuntimeError("response validation failed")
    finally:
        connection.close()


def run_ab(port, concurrency, count, name):
    command = [ab, "-q", "-s", "5", "-n", str(count), "-c", str(concurrency),
               f"http://127.0.0.1:{port}/fixture.txt"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    (output / f"{name}.txt").write_text(result.stdout)
    (output / f"{name}.stderr.txt").write_text(result.stderr)
    if result.returncode != 0:
        raise RuntimeError((name, result.returncode, result.stderr))

    def number(label, required=True):
        match = re.search(r"^" + re.escape(label) + r":\s+([0-9.]+)", result.stdout, re.MULTILINE)
        if match is None and required:
            raise RuntimeError(f"missing ab field: {label}")
        return float(match.group(1)) if match else 0

    row = {"concurrency": concurrency, "command": command,
           "completed": int(number("Complete requests")),
           "failed": int(number("Failed requests")),
           "non_2xx": int(number("Non-2xx responses", False)),
           "body_bytes": int(number("Document Length")),
           "html_transferred": int(number("HTML transferred")),
           "elapsed_s": number("Time taken for tests"),
           "requests_per_s": number("Requests per second")}
    if (row["completed"] != count or row["failed"] != 0 or row["non_2xx"] != 0 or
            row["body_bytes"] != len(payload) or row["html_transferred"] != count * len(payload)):
        raise RuntimeError((name, "failed or incomplete work", row))
    row["mean_request_ms"] = row["elapsed_s"] * 1000 * concurrency / count
    return row


try:
    # 조건·반복마다 새 목적지 포트를 써 연결 종료 후 TIME_WAIT의 포트 재사용 영향을 줄인다.
    with tempfile.TemporaryDirectory(prefix="jungle-static-bench-") as root:
        (Path(root) / "fixture.txt").write_bytes(payload)
        for repetition in range(repeats):
            # 항상 같은 조건이 처음·마지막에 오지 않도록 순서를 회전한다.
            order = concurrencies[repetition % 3:] + concurrencies[:repetition % 3]
            for concurrency in order:
                with server(root) as port:
                    verify(port)
                    run_ab(port, 1, 200, f"warmup-{repetition + 1}-c{concurrency}")
                    row = run_ab(port, concurrency, requests,
                                 f"run-{repetition + 1}-c{concurrency}")
                    row["repetition"] = repetition + 1
                    runs.append(row)
                    print(f"run={repetition + 1} c={concurrency} rps={row['requests_per_s']:.2f}", flush=True)
                    verify(port)
    summary = []
    for concurrency in concurrencies:
        rows = [row for row in runs if row["concurrency"] == concurrency]
        rates = [row["requests_per_s"] for row in rows]
        summary.append({"concurrency": concurrency, "runs": len(rows),
                        "rps_median": statistics.median(rates), "rps_min": min(rates),
                        "rps_max": max(rates), "completed": sum(row["completed"] for row in rows),
                        "failed": sum(row["failed"] for row in rows),
                        "non_2xx": sum(row["non_2xx"] for row in rows)})
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
finally:
    metadata["uptime_after"] = subprocess.check_output(["uptime"], text=True).strip()
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (output / "runs.json").write_text(json.dumps(runs, indent=2) + "\n")
