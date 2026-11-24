"""웹 서버 없이 CGI 실행 파일의 환경변수·표준입력·표준출력을 검증한다."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys

binary = str(Path(sys.argv[1]).resolve())
missing = object()
checks = 0


def check(payload, expected_status=200, expected_result=None, *,
          declared=missing, method="POST"):
    global checks
    env = os.environ.copy()
    for name in ("CONTENT_LENGTH", "REQUEST_METHOD"):
        env.pop(name, None)
    if declared is missing:
        env["CONTENT_LENGTH"] = str(len(payload))
    elif declared is not None:
        env["CONTENT_LENGTH"] = declared
    if method is not None:
        env["REQUEST_METHOD"] = method
    process = subprocess.run([binary], input=payload, capture_output=True,
                             env=env, timeout=3)
    assert process.returncode == 0, (process.returncode, process.stderr)
    assert process.stderr == b"", process.stderr
    headers, separator, body = process.stdout.partition(b"\r\n\r\n")
    assert separator, process.stdout
    lines = headers.decode("ascii").split("\r\n")
    assert lines[0].startswith(f"Status: {expected_status} "), process.stdout
    assert lines[1:] == ["Content-Type: application/json"], process.stdout
    data = json.loads(body)
    if expected_status == 200:
        assert data == {"result": expected_result}, data
    else:
        assert isinstance(data.get("error"), str), data
    checks += 1


check(b"a=2&b=3", expected_result=5)
check(b"a=-12&b=003", expected_result=-9)
check(b"a=0&b=0", expected_result=0)
bits = ctypes.sizeof(ctypes.c_long) * 8
maximum = 2 ** (bits - 1) - 1
minimum = -(2 ** (bits - 1))
check(f"a={maximum}&b={minimum}".encode(), expected_result=-1)
check(f"a={maximum}&b=0".encode(), expected_result=maximum)
check(f"a={minimum}&b=0".encode(), expected_result=minimum)
check(f"a={maximum}&b=1".encode(), 400)
check(f"a={minimum}&b=-1".encode(), 400)
check(f"a={maximum + 1}&b=0".encode(), 400)

for payload in (b"", b"a=2&b=", b"a=2x&b=3", b"b=3&a=2", b"a=2&b=3&c=4",
                b"a=+2&b=3", b"a=2&b=3\0extra"):
    check(payload, 400)
for value in (None, "", "-1", "+7", "7x", "1048577", "999999999999999999999999"):
    check(b"", 400, declared=value)
check(b"a=2", 400, declared="7")
check(b"a=2&b=3", 400, method="GET")
check(b"a=2&b=3", 400, method=None)
# 뒤에 다른 데이터가 있어도 CONTENT_LENGTH만큼만 해석한다.
check(b"a=2&b=3ignored", expected_result=5, declared="7")
check(b"a=" + b"0" * (1024 * 1024 - 6) + b"&b=0", expected_result=0)
print(f"Standalone CGI checks passed: {checks}")
