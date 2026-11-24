"""CGI 실행기의 환경변수 전달, 종료 코드, 실행 실패 처리를 확인한다."""
from pathlib import Path
import os
import select
import subprocess
import sys
import tempfile
import time

launcher, cgi = (str(Path(path).resolve()) for path in sys.argv[1:])


def run(path, length, body=b""):
    return subprocess.run([launcher, path, length], input=body,
                          capture_output=True, timeout=3,
                          env={**os.environ, "REQUEST_METHOD": "GET",
                               "CONTENT_LENGTH": "999"})


result = run(cgi, "7", b"a=2&b=3")
assert result.returncode == 0 and result.stderr == b"", result
assert result.stdout == (b"Status: 200 OK\r\nContent-Type: application/json"
                         b"\r\n\r\n{\"result\":5}"), result.stdout

result = run(cgi, "7", b"a=2")
assert result.returncode == 0 and result.stderr == b"", result
assert result.stdout.startswith(b"Status: 400 Bad Request\r\n"), result.stdout

for length in ("", "-1", "7x", "1048577", "999999999999999999999"):
    result = run(cgi, length)
    assert result.returncode != 0 and result.stdout == b"", result
    assert b"Invalid body length" in result.stderr, result.stderr

with tempfile.TemporaryDirectory() as directory:
    result = run(str(Path(directory) / "missing-cgi"), "0")
    assert result.returncode != 0 and b"posix_spawn:" in result.stderr, result

    program = Path(directory) / "child"
    program.write_text("#!/bin/sh\nexit 42\n")
    program.chmod(0o700)
    result = run(str(program), "0")
    assert result.returncode == 42, result

    program.write_text("#!/bin/sh\nkill -TERM $$\n")
    result = run(str(program), "0")
    assert result.returncode != 0 and b"terminated by signal" in result.stderr, result

    # 자식은 EOF를 기다린다. 양쪽에 불필요한 쓰기 fd가 남으면 끝나지 않는다.
    program.write_text("#!/bin/sh\ncat >/dev/null\nexit 42\n")
    result = run(str(program), "7", b"a=2&b=3")
    assert result.returncode == 42 and result.stderr == b"", result

    # 읽는 쪽이 먼저 닫혀도 부모가 SIGPIPE로 죽지 않고 EPIPE를 처리한다.
    program.write_text("#!/bin/sh\nexec 0<&-\nexit 0\n")
    result = run(str(program), "1048576", b"x" * 1048576)
    assert result.returncode > 0 and b"write CGI input:" in result.stderr, result

    # 파이프 용량보다 큰 출력을 먼저 쓰는 CGI도 입력 전달과 교착하지 않는다.
    header = b"Content-Type: application/octet-stream\r\n\r\n"
    header_command = "printf 'Content-Type: application/octet-stream\\r\\n\\r\\n'\n"
    program.write_text("#!/bin/sh\n" + header_command +
                       "dd if=/dev/zero bs=4096 count=64 2>/dev/null\ncat\n")
    body = b"x\x00y" * 200000
    result = run(str(program), str(len(body)), body)
    assert result.returncode == 0 and result.stderr == b"", result.stderr
    assert result.stdout == header + b"\x00" * (4096 * 64) + body

    # CGI가 먼저 출력해도 전체 수집·검증 전에는 외부로 내보내지 않는다.
    program.write_text("#!/bin/sh\n" + header_command + "printf ready\ncat\n")
    process = subprocess.Popen([launcher, str(program), "7"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    try:
        assert not select.select([process.stdout], [], [], 0.2)[0], "Premature output"
        output, errors = process.communicate(b"a=2&b=3", timeout=3)
        assert process.returncode == 0 and errors == b"", errors
        assert output == header + b"readya=2&b=3", output
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()

    # stdout의 EOF를 받아도 남은 입력을 읽는 자식에게는 계속 전달한다.
    program.write_text("#!/bin/sh\nexec 1>&-\ncat >/dev/null\nexit 42\n")
    result = run(str(program), "1048576", b"x" * 1048576)
    assert result.returncode == 42 and result.stdout == b"", result
    assert result.stderr == b"", result.stderr

    # 입출력 대기와 출력 EOF 이후의 종료 대기에 동일한 실행 기한을 적용한다.
    for child_code in (
        "while True: time.sleep(1)",
        "os.close(1)\nwhile True: time.sleep(1)",
        "while True:\n os.write(1, b'x')\n time.sleep(0.2)",
    ):
        pid_file = Path(directory) / "child.pid"
        program.write_text(
            f"#!{sys.executable}\nimport os, time\n"
            f"open({str(pid_file)!r}, 'w').write(str(os.getpid()))\n"
            + child_code + "\n")
        started = time.monotonic()
        result = subprocess.run([launcher, str(program), "0"],
                                input=b"", capture_output=True, timeout=7)
        elapsed = time.monotonic() - started
        assert result.returncode > 0 and b"CGI timeout" in result.stderr, result
        assert result.stdout == b"", result.stdout
        assert 2.5 <= elapsed < 6, elapsed
        pid = int(pid_file.read_text())
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f"CGI process {pid} was not reaped")


    for output in (b"not CGI", b"Content-Type: text/plain\r\nBroken header\r\n\r\nsecret"):
        program.write_text(f"#!{sys.executable}\nimport sys\nsys.stdout.buffer.write({output!r})\n")
        result = run(str(program), "0")
        assert result.returncode > 0 and result.stdout == b"", result
        assert b"Invalid CGI response" in result.stderr, result.stderr

    # 유효한 출력이어도 프로세스가 실패하면 내보내지 않는다.
    program.write_text("#!/bin/sh\n" + header_command + "printf secret\nexit 42\n")
    result = run(str(program), "0")
    assert result.returncode == 42 and result.stdout == b"", result

    for size in (1048576, 1048577):
        program.write_text(f"#!{sys.executable}\nimport sys\n"
                           f"sys.stdout.buffer.write({header!r} + b'x' * {size})\n")
        result = run(str(program), "0")
        if size == 1048576:
            assert result.returncode == 0 and result.stderr == b"", result.stderr
            assert result.stdout == header + b"x" * size
        else:
            assert result.returncode > 0 and result.stdout == b"", result
            assert b"Invalid CGI response" in result.stderr, result.stderr

    # 무한 출력은 수집 한도에서 중단하고, 자식도 회수한다.
    program.write_text(f"#!{sys.executable}\nimport os\n"
                       f"open({str(pid_file)!r}, 'w').write(str(os.getpid()))\n"
                       "while True: os.write(1, b'x' * 4096)\n")
    result = run(str(program), "0")
    assert result.returncode > 0 and result.stdout == b"", result
    assert b"CGI output too large" in result.stderr, result.stderr
    try:
        os.kill(int(pid_file.read_text()), 0)
    except ProcessLookupError:
        pass
    else:
        raise AssertionError("Oversized CGI process was not reaped")

payload = b"a=" + b"0" * (1048576 - 6) + b"&b=0"
result = run(cgi, str(len(payload)), payload)
assert result.returncode == 0 and result.stderr == b"", result
assert result.stdout.endswith(b'{"result":0}'), result.stdout

# 선언한 길이 뒤의 데이터는 전달하지 않는다.
result = run(cgi, "7", b"a=2&b=3ignored")
assert result.returncode == 0 and result.stderr == b"", result
assert result.stdout.endswith(b'{"result":5}'), result.stdout

result = subprocess.run([launcher], capture_output=True, timeout=3)
assert result.returncode != 0 and b"Usage:" in result.stderr, result
print("CGI launcher checks passed: 27")
