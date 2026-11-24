# Jungle-LLM-webServer

외부 HTTP/이벤트 루프 라이브러리 없이 **C11**과 **POSIX 소켓 API**만으로 구현하는 웹 서버 및 프록시 프로젝트입니다.

> 현재 HTTP 요청 파싱·응답 송신, GET 정적 파일 서빙, POST 덧셈과 선택적인 CGI 실행을 제공합니다. 빌드에서 순차 처리 또는 스레드 풀을 선택하며, 기본값은 스레드 풀입니다. 프록시는 이후 구현 범위입니다.

## 프로젝트 구조

각 기능 모듈의 헤더(`.h`)와 소스(`.c`)는 `src/<모듈>/` 아래에 함께 배치(코로케이션)합니다.

```text
.
├── Makefile
├── README.md
├── src/
│   ├── main.c          # 진입점, 서버 생명주기 및 시그널 조율
│   ├── config/         # 설정 파일 및 CLI 인자 처리
│   ├── net/            # 소켓, 연결 관리, 동시성 계층
│   ├── http/           # HTTP 요청 파싱, 응답 생성, 프로토콜 구조체
│   ├── dynamic/        # 서버 내부 덧셈 처리
│   ├── cgi/            # CGI 실행, 출력 파싱, HTTP 송신
│   ├── router/         # 요청 라우팅 (정적 / 동적 / 프록시 분기)
│   ├── static/         # 정적 파일 서빙, MIME 매핑
│   ├── proxy/          # 프록시 중계
│   └── util/           # 동적 버퍼, 로깅 등 공통 유틸리티
├── tests/
│   ├── unit/           # C 단위 테스트
│   ├── integration/    # 통합 및 엣지케이스 테스트
│   └── load/           # 부하 테스트 스크립트
├── config/             # 서버 설정 파일 예시
├── www/                # 정적 파일 루트
├── samples/            # 프록시·동적 요청 테스트용 샘플 코드
└── docs/               # 설계 문서 및 ADR
```

> config·router·proxy·util 등 일부 디렉터리는 이후 구현을 위한 구조입니다.

## 빌드 및 실행

```sh
# 기본 디버그 빌드 (build/threadpool/webserver)
make

# 릴리스 빌드 (build/threadpool/webserver-release)
make release

# AddressSanitizer + UndefinedBehaviorSanitizer 빌드 (build/threadpool/webserver-asan)
make asan

# 실행
./build/threadpool/webserver 8080 ./www

# 다른 터미널에서 정적 파일 응답 확인
curl -i http://127.0.0.1:8080/

# 단위 테스트 실행
make test

# ASan + UBSan으로 단위 테스트 실행
make BUILD_MODE=asan test

# 실제 HTTP 정적 파일 통합 테스트 (Python 3 필요)
make test-integration

# sanitizer 빌드로 통합 테스트
make asan
python3 tests/integration/test_static.py build/threadpool/webserver-asan

# 빌드 산출물 정리
make clean
```

실행 인자는 `[포트] [정적 루트] [CGI 실행 파일]`이며 포트·루트를 생략하면 `8080`, `www`를 사용합니다. 마지막 인자를 지정하면 `POST /cgi/add`를 해당 프로그램에 연결합니다. `0.0.0.0`에 바인딩하므로 다른 컴퓨터에서는 서버의 실제 IP로 접속합니다. 서버는 Ctrl+C 또는 SIGTERM으로 종료합니다.

연결당 요청 하나에 응답하고 소켓을 닫으며, 연결 소켓은 블로킹 송수신을 사용합니다. 250ms 대기 제한으로 종료 요청을 주기적으로 확인하며, 대기 시간 만료만으로 연결을 끊지는 않습니다. 순차 모드에서는 유휴 연결 하나가 다음 연결 처리를 막고, 스레드 풀에서는 작업자 하나를 점유합니다. 종료 요청 시 대기 연결을 닫고 처리 중인 연결을 중단합니다.

학습 내용과 코드의 대응은 [TCP 소켓 서버 학습 문서](docs/learning/tcp/01-tcp-socket-server.md), 현재 학습 상태는 [진행 기록](docs/learning/tcp/progress.md)에 정리했습니다.

## 빌드 시 동시성 선택

```sh
# 기본: 작업자 4개, 대기 큐 8개
make CONCURRENCY=threadpool
./build/threadpool/webserver 8080 ./www

# 기존 순차 처리와 비교
make CONCURRENCY=sequential
./build/sequential/webserver 8080 ./www

# 컴파일 모드와 독립적으로 조합 가능
make CONCURRENCY=threadpool BUILD_MODE=asan
make CONCURRENCY=sequential BUILD_MODE=release

# 각 방식의 HTTP 기능 및 동시성 검증
make CONCURRENCY=threadpool test-integration
make CONCURRENCY=sequential test-integration
```

`CONCURRENCY`는 `sequential`, `threadpool`만 지원하며 다른 값은 빌드 오류입니다. fork·이벤트 루프는 아직 구현하지 않았습니다. 방식별 소스 하나만 링크하며 바이너리와 오브젝트는 `build/<방식>/`에 분리하므로 clean 없이 전환할 수 있습니다.

두 방식 모두 같은 `tcp_handle_client()`로 요청을 처리합니다. 순차 모드는 수락 스레드가 직접 처리하고, 스레드 풀 모드는 큐에 연결 fd를 넣어 작업자에게 소유권을 넘깁니다. 큐는 뮤텍스·조건 변수로 보호하며 실제 HTTP 처리 중에는 큐 잠금을 잡지 않습니다.

스레드 풀의 큐가 가득 차면 새로 수락한 연결을 즉시 닫습니다. HTTP 503을 보내는 정책은 아니므로 클라이언트는 연결 종료·리셋 등으로 관찰할 수 있습니다. 작업 중 4개와 큐 대기 8개 외에 OS의 수락 대기열은 별도로 존재합니다.

종료 시 새 연결 수락을 멈추고 큐의 연결을 닫습니다. 처리 중인 소켓은 `shutdown()`으로 수신·송신 대기를 깨우고, 작업자가 자신의 fd를 닫고 끝날 때까지 join한 다음 큐와 루트 fd를 정리합니다. 응답을 모두 완료시키는 graceful shutdown은 아닙니다. 작업자는 SIGINT·SIGTERM을 차단하여 수락 스레드만 종료 시그널을 처리합니다.

## HTTP 요청 파서

`src/http/request.h`의 `http_request_parse(data, length, &request)`는 누적 수신 버퍼에서 요청 하나를 해석합니다. 입력은 NUL 종료 문자열일 필요가 없으며, 파서는 메모리를 할당하거나 입력을 변경하지 않습니다.

- 요청 라인의 메서드·요청 대상·HTTP/1.0 또는 HTTP/1.1 버전을 추출합니다. 메서드 실행 가능 여부와 URI의 세부 해석은 이후 처리 계층의 역할입니다.
- 헤더를 첫 콜론에서 나누고 값 앞뒤 공백·탭을 제거합니다. 이름은 원문을 보존하며, 내부의 `Host`·`Content-Length` 비교는 대소문자를 구분하지 않습니다.
- HTTP/1.1에는 비어 있지 않은 `Host` 하나를 요구합니다. 호스트 주소 문법 전체를 검증하거나 호스트와 포트를 분리하지는 않습니다.
- `Content-Length`는 십진수 숫자만 허용하며 오버플로를 검사합니다. 값이 같아도 중복 헤더와 쉼표로 나열한 길이는 거부합니다.
- `Content-Length`와 `Transfer-Encoding`이 모두 없으면 요청 본문 길이는 0입니다. `Content-Length`가 있으면 정확히 그 바이트만 본문으로 취급합니다.
- `Transfer-Encoding`은 지원하지 않습니다. `Content-Length`와 함께 있으면 잘못된 요청으로 거부합니다.
- 기본 한도는 요청 라인을 포함한 헤더 영역 8192바이트, 헤더 64개, 본문 1 MiB입니다.

| 반환값 | 의미 |
|---|---|
| `HTTP_PARSE_COMPLETE` | 요청 하나 완성. 결과 필드를 사용할 수 있음 |
| `HTTP_PARSE_INCOMPLETE` | 누적 버퍼를 보존하고 추가 수신 필요 |
| `HTTP_PARSE_INVALID` | 문법 또는 프레이밍 오류 |
| `HTTP_PARSE_TOO_LARGE` | 헤더·본문 한도 초과 |
| `HTTP_PARSE_UNSUPPORTED` | 지원하지 않는 Transfer-Encoding |

완성된 결과의 각 `http_slice`는 원본 버퍼 안의 주소와 길이를 가리킵니다. NUL 종료를 보장하지 않으므로 `%s`나 `strlen()`으로 읽으면 안 됩니다. 결과를 사용하는 동안 버퍼를 해제·재할당·덮어쓰거나 이동하지 않아야 합니다. `consumed` 뒤의 바이트는 다음 요청 처리를 위해 보관할 수 있습니다.

파서는 수신·시간 측정·EOF를 처리하지 않습니다. `INCOMPLETE` 상태에서 상대 송신이 종료되면 호출자가 불완전한 요청으로 처리해야 합니다. chunked 해독, 연결 유지 정책, 수신 시간 제한은 아직 구현하지 않았습니다.

## HTTP 응답 생성·송신

`src/http/response.h`의 `http_response_send()`는 상태 라인, `Content-Type`, `Content-Length`, `Connection: close`, 빈 줄을 생성하고 본문을 송신합니다. 상태 코드는 현재 200·400·403·404·413·500·501·502·504를 지원합니다.

```c
http_response response = {
    .status = 200,
    .content_type = "text/plain; charset=utf-8",
    .body = "hello",
    .body_length = 5,
};
int result = http_response_send(client_fd, &response, &stopping);
```

성공하면 `0`, 실패하면 `-1`을 반환하며 실패 원인은 `errno`로 확인합니다. 본문은 문자열이 아닌 바이트 길이로 보내므로 NUL이 있는 바이너리도 가능합니다. 응답 헤더 전체는 1024바이트 버퍼 안에 들어가야 하며, 헤더 삽입을 막기 위해 Content-Type의 제어문자를 거부합니다. 잘못된 입력은 송신 전에 거부합니다.

송신 중에는 본문 메모리를 유지해야 합니다. `stopping`은 유효한 종료 플래그의 주소여야 하고, 호출자는 `SIGPIPE`를 무시하도록 설정해야 합니다. 함수는 연결 소켓의 `SO_SNDTIMEO`를 250ms로 설정하고, 대기 만료 시 종료 요청을 확인하며 재시도합니다. 이 값은 전체 응답의 송신 기한이 아니므로 종료 요청이 없다면 읽지 않는 상대를 계속 기다릴 수 있습니다.

소켓은 함수가 닫지 않습니다. 호출자가 성공·실패 모두에서 소켓을 닫아야 하며, 실패 시 일부 바이트가 이미 전송되었을 수 있으므로 처음부터 재전송하면 안 됩니다. 현재는 연결당 응답 하나를 보내는 정책입니다. 정적 파일은 `http_response_send_headers()`로 헤더를 먼저 보낸 뒤 고정 크기 버퍼로 본문을 전송합니다.

## 정적 파일 서빙

`./build/threadpool/webserver 8080 ./www`로 지정한 루트에서 파일을 제공합니다. 서버는 시작할 때 루트 디렉터리를 열고, 요청마다 해당 fd를 기준으로 파일을 찾습니다. 루트를 열 수 없으면 시작에 실패합니다.

- 정적 파일은 GET으로 제공합니다. POST /add는 아래 동적 처리로 분기하며, 나머지 미지원 메서드·POST 경로는 501을 반환합니다.
- `/`와 디렉터리 경로는 해당 디렉터리의 `index.html`을 제공합니다. 목록 출력과 리다이렉트는 하지 않습니다.
- 쿼리 문자열은 파일 경로에서 제외하고 `%xx`로 인코딩한 경로는 디코딩합니다.
- 디코딩 후 `..` 경로 요소, 역슬래시, 모든 심볼릭 링크는 403으로 거부합니다. 정상 파일을 가리키는 루트 내부 링크도 허용하지 않습니다.
- 없는 파일·중간 디렉터리·index 파일은 404, 접근 권한 부족과 일반 파일이 아닌 대상은 403, 기타 파일 열기·정보 조회 실패는 500입니다.
- HTML·CSS·JS·PNG·JPG/JPEG·GIF·SVG·TXT·JSON 확장자를 대소문자 구분 없이 매핑합니다. 나머지는 `application/octet-stream`입니다.
- 파일 크기로 Content-Length를 정하고 16 KiB씩 읽어 송신합니다. 파일 전체 크기에 비례한 메모리를 할당하지 않습니다.
- 헤더 송신 이후 읽기 실패나 파일 축소가 발생하면 두 번째 오류 응답을 보내지 않고 연결을 종료합니다. 파일 증가는 처음 확인한 길이까지만 전송합니다. 전송 중 파일 변경의 일관성을 보장하지 않습니다.

요청 수신에는 파서 한도인 헤더 8 KiB·본문 1 MiB를 적용합니다. 잘못된 요청과 불완전한 EOF는 400, 크기 초과는 413, 지원하지 않는 Transfer-Encoding은 501로 응답합니다. 전체 요청 수신 기한은 아직 없으므로 느린 클라이언트가 순차 모드를 막거나 스레드 풀 작업자를 계속 점유할 수 있습니다.


## 동적 요청: POST /add

```sh
curl -i http://127.0.0.1:8080/add --data-raw 'a=2&b=3'
```

서버는 Content-Length만큼 본문을 받은 뒤 `dynamic_add_parse()`로 입력을 읽고 `dynamic_add()`로 계산합니다. `dynamic_add_send()`가 결과를 JSON으로 만들어 기존 HTTP 응답 모듈로 보냅니다. 위 요청은 200과 `{"result":5}`를 반환합니다.

- 경로는 정확히 `/add`, 메서드는 POST입니다. GET은 기존 정적 파일 처리로 갑니다.
- 본문은 `a=<정수>&b=<정수>` 순서만 허용합니다. 음수와 선행 0은 가능하며 숫자는 C의 long 범위여야 합니다.
- 공백, `+` 부호, URL 인코딩, 필드 순서 변경, 추가·중복 필드는 지원하지 않습니다. 일반적인 폼 파서가 아닙니다.
- 본문 형식 오류, 숫자 범위 초과, 합계 오버플로는 400과 JSON 오류 본문으로 응답합니다. HTTP 파싱 자체의 오류는 기존 공통 오류 처리로 갑니다.
- 전역 계산 상태는 없으며 각 요청의 숫자·합계·응답 버퍼는 해당 호출에서만 유지합니다. CGI는 별도의 POST /cgi/add 경로로 연결하며, 동시 처리 방식은 빌드 옵션으로 선택합니다.


## CGI 학습 1단계: 덧셈 프로그램 단독 실행

```sh
make cgi
printf '%s' 'a=2&b=3' | REQUEST_METHOD=POST CONTENT_LENGTH=7 ./build/threadpool/cgi-add
make test-cgi
make BUILD_MODE=asan test-cgi
```

`samples/cgi-add.c`는 `REQUEST_METHOD=POST`와 유효한 `CONTENT_LENGTH`를 확인하고, 표준입력에서 지정한 바이트 수만큼 읽습니다. 기존 `dynamic_add_parse()`와 `dynamic_add()`로 입력 해석·계산을 재사용합니다. HTTP 송신 함수는 `src/dynamic/add_response.c`로 옮겨 계산 코드만 CGI에 링크할 수 있게 했습니다.

정상 출력은 `Status: 200 OK`, `Content-Type: application/json`, CRLF 빈 줄, `{"result":5}`입니다. HTTP 상태 라인·Content-Length·Connection 헤더는 출력하지 않습니다. 잘못된 입력이나 부족한 본문은 CGI 형식의 400, 내부 메모리·입력 처리 실패는 500으로 출력합니다. 입력 길이는 기존 서버와 같은 1 MiB 이내의 십진수만 허용합니다.

처리한 오류 응답을 포함해 출력에 성공하면 프로세스 종료 코드는 0입니다. 출력 실패는 비정상 종료 코드로 알리며 진단은 stderr에만 씁니다. stdin의 EOF를 기다리지 않고 지정한 길이만 읽으므로, 다음 단계에서 서버가 길이만큼 본문을 전달해야 합니다. 서버가 본문 전송을 멈추고 파이프를 열어두면 이 프로그램은 기다립니다. 실행 시간 제한은 다음 서버 연동 단계의 책임입니다.

실행 파일은 정적 파일 루트 밖의 빌드 디렉터리에 생성합니다. 1단계 당시 CGI는 HTTP 경로에 연결하지 않았습니다. 현재도 기존 POST /add는 서버 내부 함수 방식으로 동작합니다.


## CGI 학습 2단계: 자식 프로세스 실행과 회수

```sh
make cgi
printf '%s' 'a=2&b=3' | ./build/threadpool/cgi-launch ./build/threadpool/cgi-add 7
make test-cgi
```

`samples/cgi-launch.c`는 서버에 연결하기 전 프로세스 실행만 확인하는 학습용 실행기입니다. 실행 파일 경로와 본문 길이를 인자로 받습니다. 길이를 십진수·1 MiB 이내로 검증하고 자식에게 전달할 `REQUEST_METHOD=POST`, `CONTENT_LENGTH=7` 환경변수 배열을 구성합니다. 진단 도구 등에서 사용할 기본 `PATH=/usr/bin:/bin`도 전달합니다. 부모의 환경변수를 변경하는 `setenv()`는 사용하지 않습니다.

흐름은 `인자 검증 → child_env 구성 → posix_spawn() → waitpid() → 종료 결과 확인`입니다. `child_argv`는 자식의 명령행 인자, `child_env`는 자식의 환경변수이며 두 배열 모두 NULL로 끝납니다. `child_pid`는 실행한 자식을 식별합니다. `posix_spawn()`의 반환값 0은 실행 요청의 성공이며 CGI의 계산 성공이나 종료를 의미하지 않습니다. 실패 시 반환된 오류 번호를 `strerror()`로 해석합니다.

`waitpid(child_pid, &status, 0)`는 해당 자식이 끝날 때까지 기다리고 종료 상태를 회수합니다. 시그널에 의해 대기가 중단된 EINTR은 다시 기다립니다. `status`는 종료 코드 자체가 아니므로 `WIFEXITED()` 확인 후 `WEXITSTATUS()`로 읽습니다. 시그널 종료는 `WIFSIGNALED()`와 `WTERMSIG()`로 확인합니다. CGI가 출력한 HTTP 400과 프로세스의 비정상 종료는 서로 다릅니다.

2단계에서는 파일 동작과 시그널 속성을 지정하지 않아 자식이 부모의 stdin·stdout·stderr와 기본 상속 규칙을 사용했습니다. 위 명령의 본문은 CGI의 stdin으로, CGI 출력은 터미널로 전달됩니다. 실행기가 응답을 읽거나 HTTP로 보내지는 않습니다. 다음 단계에서 별도의 파이프로 입출력을 연결합니다.

2단계 당시 실행기는 종료 기한이 없어 자식이 멈추면 계속 기다렸습니다. 서버에서 사용하기 전에 파이프 처리·시간 제한·불필요한 fd 상속 방지·시그널 속성 설정이 필요하며, 현재 웹 서버에서는 호출하지 않습니다.


## CGI 학습 3단계: 입력 파이프 연결

명령은 2단계와 같습니다. 3단계에서는 `cgi-launch`가 자신의 stdin에서 읽은 본문을 새 파이프로 CGI에 전달하도록 변경했습니다. 이 단계까지는 CGI의 stdout·stderr를 그대로 상속했습니다.

```text
셸의 본문 → 부모 stdin → forward_body() → input_pipe[1]
                                            ↓
                                       input_pipe[0] → 자식 stdin
```

`pipe(input_pipe)`는 읽기 끝 `[0]`과 쓰기 끝 `[1]`을 만듭니다. `posix_spawn_file_actions_*()`는 부모의 fd를 즉시 변경하는 함수가 아니라, 자식을 실행할 때 적용할 파일 작업을 등록합니다. 자식에서는 쓰기 끝을 닫고 읽기 끝을 `dup2()`로 표준 입력 fd 0에 연결합니다. 원래 읽기 fd가 0과 다르면 중복 fd도 닫습니다. 부모는 실행 직후 읽기 끝을 닫습니다. 파일 작업 목록은 실행 후 destroy하며, 실행 실패 시에도 파이프 양쪽을 정리합니다.

`forward_body()`는 최대 4096바이트씩 읽어 선언한 본문 길이까지만 전달합니다. `remaining`은 더 읽을 길이, `received`는 이번에 읽은 길이, `sent`는 그중 파이프에 쓴 길이입니다. 부분 쓰기는 남은 위치부터 반복하고 EINTR은 재시도합니다. 길이를 모두 전달하거나 부모 stdin에서 EOF를 만나면 쓰기 끝을 닫습니다. 본문이 부족하면 CGI가 EOF를 보고 기존 400 응답을 생성합니다.

자식에게 파이프 쓰기 끝을 남기면 부모가 닫아도 쓰기 fd가 여전히 존재하므로 자식이 EOF를 받지 못할 수 있습니다. 양쪽에서 사용하지 않는 끝을 닫는 이유입니다. 부모는 본문 전달이 끝난 다음 `waitpid()`로 자식을 회수합니다. 먼저 기다리면 입력을 기다리는 자식과 서로 대기할 수 있습니다.

부모는 자식 실행 후 SIGPIPE를 무시하도록 설정합니다. 자식이 읽기 끝을 먼저 닫으면 부모의 write가 EPIPE로 실패하며, 부모는 쓰기 끝을 닫고 자식을 회수한 뒤 실패로 종료합니다. 이 설정은 이미 실행한 자식의 시그널 처리에는 영향을 주지 않습니다. 부모의 입력 오류·전달 오류와 CGI가 생성한 HTTP 400도 구분합니다.

`make test-cgi`는 기존 검사와 함께 1 MiB 본문 전달, 조기 EOF, 쓰기 끝 정리에 따른 EOF, 자식의 조기 입력 종료, 선언 길이 이후 데이터 제외를 확인합니다. 3단계에서는 CGI 출력을 부모가 읽는 파이프까지는 연결하지 않았습니다. 3단계 당시 실행기는 종료 기한이 없는 학습용이었습니다.


## CGI 학습 4단계: 출력 파이프와 입출력 중계

실행 명령은 동일합니다. 출력 파이프 `output_pipe`를 추가하고 자식의 쓰기 끝을 stdout(fd 1)에 연결했습니다. 부모는 출력의 쓰기 끝을, 자식은 출력의 읽기 끝과 중복 fd를 닫습니다. stderr는 진단용으로 그대로 상속합니다.

```text
부모 → input_pipe[1]  → input_pipe[0]  → CGI stdin
부모 ← output_pipe[0] ← output_pipe[1] ← CGI stdout
```

입력을 모두 쓰고 출력을 읽는 순서는 일반적인 CGI에서 교착할 수 있습니다. CGI가 입력을 다 읽기 전에 많은 출력을 쓰면 출력 파이프가 가득 차 CGI가 멈추고, 부모도 입력 파이프가 가득 차 멈출 수 있기 때문입니다. 이전 `forward_body()`를 `relay_cgi()`로 교체해 두 방향을 함께 처리합니다. HTTP 서버의 동시 처리 방식은 바꾸지 않습니다.

`poll()`이 감시하는 대상은 세 개입니다. 부모 stdin은 입력 버퍼가 비어 있고 더 읽을 본문이 있을 때만 감시합니다. CGI 입력 파이프는 보낼 데이터가 있을 때 POLLOUT을 감시합니다. CGI 출력 파이프는 EOF까지 POLLIN을 감시합니다. 하나의 버퍼를 다 보내기 전에는 다음 입력을 읽지 않으므로 보관 메모리는 입력·출력 각각 4096바이트로 고정됩니다.

입력 파이프의 부모 쓰기 끝만 O_NONBLOCK으로 설정합니다. 쓰기 가능 알림 뒤에도 버퍼 전체를 쓸 수 있다는 보장은 없으므로 부분 쓰기와 EAGAIN을 처리합니다. `remaining`은 앞으로 stdin에서 읽을 길이, `pending`은 현재 버퍼에서 아직 쓰지 못한 길이, `sent`는 현재 버퍼에서 다음에 쓸 위치입니다.

POLLHUP은 읽지 않은 데이터가 남아 있어도 발생할 수 있습니다. 출력 파이프를 즉시 닫지 않고 read를 계속하며, read가 0을 반환할 때 EOF로 처리합니다. 출력 EOF와 자식 종료는 서로 다르므로 남은 입력 전달과 waitpid 회수도 각각 수행합니다. 중계 실패 시 부모 쪽 파이프를 정리하고 자식을 SIGKILL로 종료시킨 뒤 회수합니다.

부모는 CGI 출력을 해석하지 않고 fwrite·fflush로 자신의 stdout에 그대로 중계합니다. CGI 헤더 파싱과 HTTP 응답 생성은 아직 연결하지 않았습니다. 출력 전체를 저장하지 않으므로 총 출력 크기 제한도 없으며, 부모 stdout으로의 쓰기는 blocking입니다. 4단계에서는 실행 기한과 느린 출력 소비자에 대한 제한을 구현하지 않았습니다.

`make test-cgi`와 `make BUILD_MODE=asan test-cgi`에서 CGI 자체 28개, 실행기 18개를 검사합니다. 입력 전 대량 출력과 바이너리 왕복, 부모 입력 대기 중 출력 전달, 출력 EOF 이후 입력 전달을 포함합니다.


## CGI 학습 5단계: 대기 기한과 시간 초과 정리

`CGI_TIMEOUT_SECONDS`는 현재 학습용 실행기의 기한인 3초입니다. `CONTENT_LENGTH`와는 관계없는 실행 정책입니다. `clock_gettime(CLOCK_MONOTONIC, &deadline)`으로 현재 시간을 얻고 3초를 더해 절대 기한을 정합니다. 시스템 날짜가 바뀌어도 경과 시간 측정에 영향을 받지 않는 시계를 사용합니다.

`time_left_ms()`는 현재 시각과 기한의 차이를 계산해 남은 밀리초를 반환합니다. poll의 대기 시간 단위에 맞춰 올림하며, 기한이 지나면 `CGI timeout`을 stderr에 출력합니다. relay_cgi는 무기한 대기 대신 이 남은 시간을 poll에 전달합니다. 데이터가 계속 도착하거나 EINTR로 대기가 중단되어도 기한을 다시 3초 뒤로 미루지 않습니다. 따라서 이는 무응답 시간 제한이 아니라 같은 절대 기한을 공유하는 대기 제한입니다.

출력 EOF를 받은 뒤에도 CGI가 살아 있을 수 있습니다. 종료 대기에서도 같은 기한을 확인하고 `waitpid(..., WNOHANG)`으로 종료 여부를 확인합니다. 반환값이 자식 PID이면 회수가 끝났고, 0이면 아직 실행 중입니다. 실행 중에는 최대 10ms 쉬고 다시 확인해 CPU를 계속 소모하는 반복을 피합니다.

시간 초과나 중계 오류가 나면 부모의 파이프를 닫고 직접 실행한 자식에게 SIGKILL을 보낸 뒤 waitpid로 회수하며 실행기는 실패로 종료합니다. HTTP 서버에 연결하지 않았으므로 HTTP 오류 응답을 생성하지는 않습니다. 5단계에서는 받은 출력을 즉시 터미널로 전달했으므로 실패 전에 내보낸 출력을 되돌리지 못했습니다.

새 검사는 아무것도 출력하지 않는 CGI, stdout만 닫고 종료하지 않는 CGI, 조금씩 계속 출력하는 CGI가 모두 약 3초 뒤 종료되고 직접 자식이 회수되는지 확인합니다. 일반 빌드와 ASan/UBSan에서 CGI 28개·실행기 21개를 검사합니다.

한계: 부모 stdout의 fwrite·fflush는 여전히 blocking이라 느린 출력 소비자 때문에 그 호출 안에서 멈춘 시간은 이 기한으로 중단하지 못합니다. posix_spawn 호출 자체나 SIGKILL 이후 OS의 자식 회수 시간까지 엄밀하게 제한하는 구현도 아닙니다. 자식이 추가로 만든 후손 프로세스의 종료는 관리하지 않습니다. CGI 헤더 파싱·HTTP 연결·출력량 제한·서버용 fd와 시그널 정책은 다음 범위입니다.


## CGI 학습 6단계: 출력 헤더와 본문 해석

`src/cgi/response.h`의 `cgi_response_parse(data, length, &response)`는 EOF까지 받은 CGI 출력 전체를 해석합니다. 6단계에서는 실행기나 HTTP 송신에 연결하지 않고 파서와 단위 테스트만 추가했습니다. 당시 실행기는 원문을 터미널로 즉시 중계했습니다.

```text
Status: 400 Bad Request\r\n
Content-Type: application/json\r\n
\r\n
{"error":"bad input"}
```

위 출력에서 `status=400`, `content_type=application/json`, `body={"error":"bad input"}`를 얻습니다. Status가 없으면 200을 사용합니다. `cgi_response`의 Content-Type과 body는 HTTP 파서의 `http_slice`처럼 원본 주소와 길이를 빌려 씁니다. NUL 종료 문자열이 아니며 원본 버퍼가 살아 있어야 합니다. 본문 내부의 NUL·CRLF는 그대로 보존합니다. 메모리를 할당·해제하거나 입력을 변경하지 않습니다.

파서는 CRLF로 끝나는 헤더 줄을 읽고 첫 빈 줄 뒤를 모두 본문으로 취급합니다. 각 헤더는 첫 콜론에서 나누고 값 양끝의 공백·탭을 제거합니다. 헤더 이름은 대소문자를 구분하지 않으며 올바른 토큰이어야 합니다. 헤더의 NUL·제어문자·비 ASCII 값과 줄 접기는 거부합니다. 헤더 이후의 본문에는 이 문자 제한을 적용하지 않습니다.

이번 프로젝트의 제한된 CGI 출력 계약은 다음과 같습니다. Content-Type은 비어 있지 않은 하나만 허용하며 값 내부 탭은 거부합니다. Status는 선택이며 중복은 거부하고 `200 OK`처럼 200~599의 세 자리 코드, 공백, 비어 있지 않은 사유 문구를 요구합니다. 파서는 상태 코드 숫자만 보관합니다. HTTP 송신 모듈의 현재 지원 코드 집합은 이보다 작으므로 이후 연결 시 매핑·검증이 필요합니다.

다른 헤더는 문법만 검사하고 결과에 보관하지 않습니다. Content-Length도 사용하지 않으며 EOF까지 받은 실제 본문 길이를 사용합니다. 추가 헤더 전달, LF만 사용하는 출력, Location 기반 CGI 리다이렉트와 NPH 응답은 지원하지 않습니다. 일반적인 CGI 전체 구현을 의미하지 않습니다.

헤더 영역은 빈 줄까지 8192바이트, 헤더 수는 64개, 본문은 1 MiB까지로 기존 HTTP 한도를 재사용합니다. 성공하면 1, 불완전한 출력·문법 오류·한도 초과는 0을 반환하고 실패 시 결과 구조체는 변경하지 않습니다. 이미 EOF를 받은 전체 출력용 파서이므로 데이터 부족을 별도 상태로 구분하지 않습니다. 호출자가 출력 수집 중에도 크기 한도를 적용해야 메모리 사용을 제한할 수 있습니다.

`make test`와 `make BUILD_MODE=asan test`로 검증합니다. 정상·기본 상태 코드, 헤더 순서·대소문자·첫 콜론 분리, 빈 본문·바이너리 본문, 잘못된 헤더·중복·제어문자, 헤더와 본문의 크기 경계, 헤더 수 경계를 포함합니다. 다음 연결 단계에서는 CGI 출력을 제한된 버퍼에 모은 뒤 이 파서에 전달해야 합니다.


## CGI 학습 7단계: 출력 수집과 파서 연결

실행 명령은 같습니다. 이제 실행기는 CGI 출력을 바로 터미널로 보내지 않고, 자식의 출력 EOF와 성공 종료를 확인한 다음 CGI 파서로 검증합니다. 모두 통과한 경우에만 기존 CGI 원문을 stdout에 출력합니다. 아직 HTTP 응답으로 변환하거나 서버 경로에 연결하지 않습니다.

```text
CGI stdout → read() → captured에 누적
                    → 출력 EOF와 자식 종료 확인
                    → cgi_response_parse()
                    → 성공한 원문만 stdout에 출력
```

`captured`는 malloc으로 할당한 출력 보관 버퍼입니다. `captured_length`는 실제로 받은 바이트 수입니다. `CGI_MAX_OUTPUT_BYTES`는 헤더 8 KiB와 본문 1 MiB를 더한 수집 상한입니다. 매번 읽은 길이가 남은 용량보다 크면 복사하지 않고 실패 처리합니다. NUL 종료는 하지 않으며, 복사와 출력 모두 명시적인 길이를 사용합니다. 입력·출력 중계용 작은 버퍼와 별도로 출력 수집용 약 1 MiB를 한 실행에 할당합니다.

수집 한도는 메모리 사용을 제한하고, 파서는 헤더·본문 각각의 한도를 따로 검증합니다. 짧은 헤더와 1 MiB를 넘는 본문은 총 수집 용량 안에 들어오더라도 파서가 거부합니다. 반대로 수집 상한을 넘는 출력은 EOF까지 기다리지 않고 중단합니다. 중계 중 stdout으로 쓰지 않으므로 느린 출력 소비자가 CGI 수집을 막는 문제도 이 구간에서는 사라집니다.

프로세스 종료 코드가 0인 경우에만 파서를 실행합니다. CGI 형식의 400 응답을 출력하고 정상 종료했다면 유효한 응답으로 내보냅니다. 출력 문법이 올바르더라도 종료 코드가 42인 경우에는 내보내지 않습니다. 파싱 실패, 시간 초과, 과도한 출력에서도 stdout은 비어 있습니다. 오류 진단은 stderr에 출력합니다.

할당 실패는 파이프를 닫고 자식을 종료·회수합니다. 중계 실패와 시간 초과도 자식을 종료·회수한 후 출력 버퍼를 해제합니다. 자식이 이미 종료된 경우에는 파싱·최종 출력의 성공 여부와 관계없이 버퍼를 해제합니다. 파서의 결과는 버퍼를 빌려 쓰므로 free 이후에는 사용할 수 없습니다.

`make test-cgi`와 `make BUILD_MODE=asan test-cgi`에서 CGI 28개·실행기 27개를 검사합니다. 검증 전 출력 보류, 대량 출력과 입력 동시 처리, 바이너리 보존, 잘못된 CGI 출력, 비정상 종료 후 출력 폐기, 본문 한도 경계, 무한 출력의 조기 중단과 자식 회수를 포함합니다.

한계: 검증 후 원문을 stdout으로 내보내는 최종 fwrite·fflush에는 아직 blocking 쓰기를 사용하며, 이 단계는 CGI 수집 기한 밖입니다. 후손 프로세스 관리는 추가하지 않았습니다. 다음 HTTP 연결 단계에서는 CGI 원문을 그대로 보내지 않고 파서의 상태 코드·Content-Type·본문으로 HTTP 응답을 생성해야 합니다.


## CGI 학습 8단계: CGI 결과를 HTTP로 송신

`src/cgi/send.h`의 `cgi_response_send(client_fd, &response, &stopping)`는 파싱한 CGI 결과를 기존 HTTP 응답 모듈에 연결합니다. 성공하면 0, 실패하면 -1을 반환하고 errno를 설정합니다. 이 단계에서는 함수와 소켓 테스트만 추가했으며 학습용 실행기와 웹 서버 라우팅은 아직 연결하지 않았습니다.

```text
cgi_response.status       → http_response.status
cgi_response.content_type → NUL 종료 임시 문자열
cgi_response.body         → HTTP 본문 주소·길이
                          → http_response_send()
```

CGI의 Content-Type은 원본 버퍼의 주소와 길이인 http_slice지만, 기존 HTTP 송신 함수는 NUL 종료 문자열을 받습니다. 따라서 Content-Type만 한도가 있는 지역 배열에 복사하고 끝에 NUL을 붙입니다. 본문은 복사하지 않고 기존 버퍼를 그대로 사용합니다. 호출 중 원본 CGI 버퍼가 유효해야 하며, 함수는 소켓이나 버퍼를 닫거나 해제하지 않습니다.

상태 라인과 표준 헤더는 HTTP 모듈이 새로 생성합니다. CGI의 `Status: 400 Custom reason`은 `HTTP/1.1 400 Bad Request`가 됩니다. Content-Length는 실제 body.length로 계산하고 Connection은 close로 설정합니다. CGI 원문에 있던 Content-Length·Connection·추가 헤더는 파싱 결과에 포함되지 않으므로 그대로 전달되지 않습니다.

HTTP 모듈을 재사용하므로 부분 송신·송신 오류·종료 플래그 처리도 기존 방식입니다. SIGPIPE 무시는 호출자의 책임이며, 전체 송신 기한은 아직 없습니다. 현재 지원 상태 코드는 기존 HTTP 모듈과 같은 200·400·403·404·413·500·501·502·504입니다. CGI 파서가 허용하는 다른 상태 코드(예: 201)는 송신 전에 EINVAL로 실패합니다. 이 함수가 자동으로 500 응답을 대신 보내지는 않습니다.

Content-Type의 잘못된 포인터·빈 값·NUL 삽입과 잘못된 본문 포인터는 EINVAL, 파서 한도보다 큰 값은 EMSGSIZE로 거부합니다. Content-Type의 제어문자 검사와 전체 HTTP 헤더 1024바이트 제한은 기존 HTTP 모듈에 위임합니다. 송신 이후 오류는 일부 바이트가 이미 전송된 상태일 수 있으므로 호출자가 새 오류 응답을 덧붙이면 안 됩니다.

`make test`와 `make BUILD_MODE=asan test`에 CGI HTTP 송신 테스트를 추가했습니다. 파서부터 socketpair 송신까지 연결해 CGI 헤더의 실제 HTTP 변환, 잘못된 CGI Content-Length의 제외, 바이너리·빈 본문, 오류 응답, 미지원 상태 코드와 잘못된 입력의 송신 전 거부를 확인합니다. 다음 단계에서는 실행·수집 과정을 서버에서 호출할 수 있도록 연결해야 합니다.


## CGI 학습 9단계: 실행 함수를 모듈로 분리

`src/cgi/run.h`의 `cgi_run(program, body, &output, NULL, -1)`으로 CGI 실행·입출력·수집·종료 회수·출력 파싱을 옮겼습니다. program은 실행 파일 경로, body는 이미 수신한 요청 본문의 주소와 길이입니다. 함수는 stdin에서 입력을 읽거나 stdout에 응답을 쓰지 않습니다. REQUEST_METHOD는 현재 POST로 고정하며 CONTENT_LENGTH는 body.length로 생성합니다.

```c
cgi_output output;
int result = cgi_run(configured_program, request.body, &output, NULL, -1);
if (result == 0) {
  int sent = cgi_response_send(client_fd, &output.response, &stopping);
  free(output.data);
  /* sent에 따라 호출자가 연결을 정리한다. */
}
```

위 코드는 호출 순서를 설명하는 예시이며 아직 서버 경로에는 연결하지 않았습니다. 성공 반환값은 0입니다. 직접 자식이 비정상 종료 코드로 끝나면 그 양수 코드를 반환하고, 실행·중계·시간 초과·파싱 오류는 -1입니다. 실패 시 output은 변경하지 않으며 내부 자원은 정리합니다. 9단계에서는 errno로 모든 실패 종류를 구분하지 않았습니다. 현재는 아래 서버 연결 2단계처럼 실패 원인을 errno로 전달하며 실행 오류 진단은 stderr에 출력합니다.

`cgi_output.data`는 함수가 할당한 원문 버퍼, length는 실제 길이, response는 파싱 결과입니다. response의 Content-Type과 본문은 data 안을 가리킵니다. 성공 시 버퍼 소유권을 호출자에게 넘기므로 송신 후 free(output.data)로 해제합니다. 실패 시에는 함수가 버퍼를 해제합니다. 입력 body는 빌려 쓰므로 호출이 끝날 때까지 유효해야 합니다.

입력은 메모리에 이미 있으므로 relay_cgi가 감시할 대상은 CGI 입력 쓰기·출력 읽기 두 개뿐입니다. sent는 본문에서 CGI에 전달한 바이트 수이며, 나머지 위치부터 부분 쓰기를 반복합니다. 기존 3초 기한·출력 수집 한도·실패 시 자식 종료와 회수는 유지했습니다. 실행 함수는 프로세스 전체의 SIGPIPE 처리나 환경변수를 변경하지 않습니다. SIGPIPE 무시는 호출자 책임입니다.

학습용 cgi-launch는 명령행 검증, stdin에서 본문 읽기, SIGPIPE 설정, cgi_run 호출, 원문 출력만 담당하도록 줄였습니다. 실행 명령은 그대로입니다. 요청한 길이보다 먼저 stdin EOF를 만나면 CLI가 기존 형식의 400을 생성합니다. 이제 CLI는 CGI 실행 전에 본문 전체를 읽으므로 이 stdin 대기는 CGI의 3초 기한에 포함되지 않습니다. 서버에서는 HTTP 파싱이 완료된 본문을 넘겨야 합니다.

`make test-cgi test`와 `make BUILD_MODE=asan test-cgi test`로 기존 CGI 실행 사례와 단위 테스트를 확인합니다. 새 테스트는 실행 함수의 잘못된 입력·실패 시 결과 보존을 확인하고, 실제 자식 실행 → 수집 → 파싱 → HTTP socketpair 송신까지 이어서 검사합니다.

다음 서버 연결 전에는 불필요한 소켓·파일 fd의 자식 상속과 작업자 시그널 마스크 상속 정책을 정리해야 합니다. 9단계 당시에는 호출자의 상속 규칙을 따랐으며 동시 CGI 실행의 fd 격리를 보장하지 않았습니다. 후손 프로세스 관리와 HTTP 전체 송신 기한도 아직 없습니다.


## CGI 학습 10단계: fd와 시그널 상속 격리

`run.c`의 `spawn_cgi()`가 자식 실행에 필요한 파일 작업과 속성을 구성합니다. 자식에게 남기는 것은 요청 본문 파이프에 연결한 stdin, 출력 수집 파이프에 연결한 stdout, 진단용으로 유지한 stderr입니다. 나머지 서버 소켓·파일·다른 요청의 파이프는 CGI 프로그램으로 상속하지 않습니다. 부모의 fd를 닫거나 FD_CLOEXEC 플래그를 변경하지는 않습니다. fd 0·1·2가 열려 있는 호출 환경을 요구하고 실행 전에 검사합니다.

부모에서 fd를 열고 나중에 CLOEXEC을 붙이는 사이에 다른 스레드가 자식을 실행할 수 있습니다. 이번 구현은 자식 생성 시점에 상속 정책을 적용하므로, 모든 fd 생성 지점에 공통 잠금을 추가하지 않고도 이 틈을 피합니다. 입력·출력 fd를 0·1에 연결한 뒤 원래 파이프 fd는 자식에서 닫습니다.

macOS에서는 `POSIX_SPAWN_CLOEXEC_DEFAULT`로 기본 fd 상속을 차단하고, 파일 작업으로 명시한 stdin·stdout·stderr만 유지합니다. stderr도 `adddup2(2, 2)`로 명시합니다. Linux에서는 연결 작업 뒤 `posix_spawn_file_actions_addclosefrom_np(..., 3)`로 3 이상의 모든 fd를 자식에서 닫습니다. 이 Linux 함수는 glibc 2.34부터 제공하므로 해당 버전 이상을 빌드 조건으로 명시했고, 지원하지 않는 플랫폼에서는 조용히 상속을 허용하지 않고 컴파일 오류로 알립니다. 순수 POSIX만으로 구현한 부분은 아니며 플랫폼 확장을 분기한 구현입니다. 근거는 [Apple의 spawn 플래그 정의](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/spawn.h)와 [glibc 2.34 릴리스 안내](https://lists.gnu.org/archive/html/info-gnu/2021-08/msg00001.html), [glibc closefrom 파일 작업 구현 설명](https://sourceware.org/pipermail/glibc-cvs/2021q3/073654.html)입니다.

`posix_spawnattr_t attributes`는 이번 자식에게 적용할 실행 속성입니다. 빈 mask를 `POSIX_SPAWN_SETSIGMASK`와 함께 적용해 작업자가 차단한 SIGINT·SIGTERM을 자식에게 그대로 넘기지 않습니다. `POSIX_SPAWN_SETSIGDEF`와 defaults 집합을 사용해 SIGPIPE·SIGINT·SIGTERM은 기본 처리로 시작합니다. 부모의 SIGPIPE 무시와 작업자의 시그널 차단은 유지합니다. 시그널 마스크와 처리 방식은 서로 다른 설정이므로 둘 다 명시합니다.

속성과 파일 작업은 성공·실패 모두에서 destroy합니다. 설정 오류가 있으면 자식을 실행하지 않고 기존 호출부가 파이프를 정리합니다. 실행 후의 입출력·3초 기한·출력 한도·자식 회수·반환값 계약은 유지합니다.

새 `test_cgi_inheritance.c`는 CLOEXEC이 없는 부모 소켓·파이프·디렉터리를 열어 둔 상태에서 4개 스레드가 CGI를 총 16번 실행합니다. 자식에서 테스트 범위 fd 3~255가 닫혔는지, 표준 fd가 열려 있는지, SIGPIPE·SIGINT·SIGTERM이 차단되지 않고 기본 처리인지 확인합니다. 각 요청의 본문 왕복과 부모의 fd·시그널 상태 보존도 확인합니다. 일반 빌드와 ASan/UBSan 검사에 포함했습니다.

아직 URL 라우팅에는 연결하지 않았으며 Linux 분기의 실제 빌드·실행은 확인하지 않았습니다. 후손 프로세스 종료, HTTP 전체 송신 기한, 서버 종료 중 CGI의 즉시 취소 정책도 다음 연결 시 고려할 범위입니다.


## CGI 서버 연결 1단계: 설정과 라우팅

```sh
make all cgi
./build/threadpool/webserver 8080 ./www ./build/threadpool/cgi-add
# 다른 터미널
curl -i http://127.0.0.1:8080/cgi/add --data-raw 'a=2&b=3'
```

응답은 HTTP 200, Content-Type application/json, 본문 `{"result":5}`입니다. 마지막 실행 인자를 생략하면 기존 실행 방식대로 CGI 경로는 활성화되지 않습니다. 순차 모드도 `make CONCURRENCY=sequential all cgi`로 빌드한 뒤 build/sequential의 실행 파일을 사용하면 됩니다.

main의 cgi_program은 CLI에서 받은 설정 경로를 가리킵니다. 서버 시작 → dispatch_start → 순차 처리 상태 또는 threadpool 상태 → tcp_handle_client 순서로 전달합니다. 작업자가 시작하기 전에 저장하며 실행 중에는 변경하지 않습니다. 문자열은 서버가 끝날 때까지 유효한 argv 메모리를 빌려 씁니다.

HTTP 파싱이 완료된 요청에서 메서드가 POST이고 대상이 정확히 `/cgi/add`이며 CGI 경로가 설정되어 있을 때만 cgi_program_send로 분기합니다. `/cgi/add?x=1` 등은 아직 지원하지 않습니다. GET은 기존 정적 파일 처리로 갑니다. 실행 파일 경로는 클라이언트의 URI에서 만들지 않으며 서버 실행 인자로만 지정합니다. 프로그램은 정적 파일 루트 밖에 둡니다.

```text
POST /cgi/add → HTTP 요청 파싱 → cgi_program_send()
              → cgi_run() → cgi_response_send() → free(output.data)
```

cgi_program_send는 실행 성공 시 파싱 결과를 HTTP로 송신한 뒤 원문 버퍼를 해제합니다. 송신 오류의 errno는 버퍼 정리 후에도 보존합니다. 서버 연결 1단계에서는 실행·수집·파싱 실패를 구분하지 않고 HTTP 500으로 응답했습니다. CGI 자체가 유효한 400을 생성하면 그 400을 그대로 HTTP로 변환합니다. 송신 함수가 실패한 뒤에는 일부 데이터가 이미 나갔을 수 있으므로 두 번째 오류 응답을 덧붙이지 않습니다.

빈 설정 경로는 시작 전에 거부합니다. 파일 존재·실행 권한 등은 CGI를 실제 실행할 때 확인하며, 잘못된 경로를 설정한 경우 해당 요청이 500을 받습니다. 설정하지 않은 상태의 POST /cgi/add는 기존 미지원 요청 정책에 따라 501입니다. 기존 POST /add와 GET 정적 파일 서빙은 유지합니다.

`make test-integration`은 이제 CGI 샘플도 빌드하고 기존 정적 파일·동시성 검사에 CGI HTTP 라우팅 검사를 추가합니다. 순차·스레드 풀 빌드와 스레드 풀 ASan/UBSan 빌드에서 정상·CGI 400·실행 실패 500·비활성화·정확한 경로 분기·본문 분할 수신·동시 CGI 요청 16개·기존 경로 유지를 확인했습니다. 일반 단위 테스트 8개도 통과했습니다.

서버 연결 1단계까지는 실패 종류별 HTTP 상태 처리와 서버 종료 시 CGI 취소가 남아 있었습니다. CGI 자체의 대기 기한만 3초로 적용했습니다. 미지원 CGI 상태 코드나 너무 긴 Content-Type은 송신 모듈에서 거부될 수 있으며 이를 HTTP 오류로 대체하는 정책도 다음 단계에서 정리합니다. 후손 프로세스 관리와 Linux 실행 검증은 아직 없습니다.


## CGI 서버 연결 2단계: 오류 분류와 종료 취소

CGI 실행 결과의 HTTP 응답 정책을 구분했습니다.

| 상황 | 처리 |
|---|---|
| 실행 파일 없음·권한 오류·서버 내부 자원 오류 | HTTP 500 |
| 자식의 실패 종료·시그널 종료·잘못된 출력·과도한 출력 | HTTP 502 |
| CGI의 3초 기한 초과 | HTTP 504 |
| CGI가 정상 생성한 400·500 등 지원 상태 | 해당 상태로 HTTP 응답 |
| 서버 종료에 의한 취소 | 응답하지 않고 자식 종료·회수 |

cgi_run의 인자는 이제 `(program, body, &output, stopping, cancel_fd)`입니다. stopping은 순차 처리에서 사용하는 시그널 종료 플래그이며 NULL이면 감시하지 않습니다. cancel_fd는 취소 전용 파이프의 읽기 끝이며 -1이면 감시하지 않습니다. 독립 실행기와 기존 실행 단위 테스트는 NULL, -1을 사용합니다. 취소는 ECANCELED, 기한 초과는 ETIMEDOUT, 잘못된 CGI 출력은 EPROTO 또는 EMSGSIZE로 전달합니다. 자식의 양수 실패 종료 코드도 유지합니다. 정리 중 close·kill·waitpid가 원래 오류를 덮지 않도록 보존합니다.

스레드 풀에는 cancel_pipe 한 쌍을 추가했습니다. 모든 작업자가 읽기 끝을 빌려 쓰며 dispatch_stop이 쓰기 끝을 닫으면 모든 CGI 대기가 EOF/HUP으로 깨어납니다. 데이터를 읽어 소비하지 않으므로 한 작업자가 다른 작업자의 취소 알림을 가져가는 문제가 없습니다. 각 작업자가 자식을 종료·회수하고 끝난 뒤 join하며, 그다음 읽기 끝을 닫습니다. 작업자 생성 실패와 시그널 마스크 설정 실패에서도 파이프를 정리합니다. CGI 자식은 이전 단계의 fd 격리 덕분에 이 파이프를 상속하지 않습니다.

relay_cgi는 입력 쓰기·출력 읽기와 함께 취소 fd의 POLLIN을 감시합니다. 현재 macOS에서는 events=0인 fd에 닫힘 알림도 오지 않는 것을 작은 재현으로 확인했으므로 POLLIN을 명시합니다. 이미 취소된 상태의 호출도 실행 전에 거부합니다. 출력 EOF 이후 자식 종료를 기다리는 루프에서도 같은 취소 조건을 확인합니다. 순차 모드는 시그널 처리로 설정한 stopping을 확인하며, 스레드 풀은 공유 volatile 값을 새로 읽지 않고 파이프를 사용합니다.

취소 시 CGI 쪽 파이프를 닫고 직접 자식에게 SIGKILL을 보낸 다음 waitpid로 회수합니다. HTTP 오류를 새로 보내지는 않습니다. 기존 소켓 shutdown은 recv·send를 깨우는 용도로 유지합니다. 이는 진행 중 응답을 완료하는 graceful shutdown 정책이 아닙니다.

HTTP 송신 전 검증도 분리했습니다. http_response_validate는 실제 송신과 같은 format_headers를 사용해 상태 코드·Content-Type·전체 헤더 크기를 검증합니다. CGI 결과를 build_response로 변환하는 단계에서 실패하면 아직 전송 전이므로 502로 대체할 수 있습니다. HTTP 송신을 시작한 다음 발생한 오류에는 두 번째 응답을 붙이지 않습니다. 따라서 미지원 CGI 상태 코드나 긴 Content-Type 때문에 연결만 끊어지던 경우도 502를 받습니다.

검사는 정상·CGI 400·실행 실패 500에 더해 자식 실패/시그널 종료, 잘못된 출력, 미지원 상태, 긴 Content-Type, 본문/수집 한도 초과의 502와 입출력/종료 대기의 504를 확인합니다. 클라이언트가 요청 송신만 종료한 경우에도 정상 응답하는지 확인합니다. SIGINT·SIGTERM 각각에 대해 CGI 출력 대기와 출력 EOF 후 종료 대기 중 서버를 중단하고, 1.5초 안에 서버가 종료되며 직접 자식이 모두 회수되는지 검사합니다. 스레드 풀은 CGI 4개, 순차 모드는 1개를 실행한 상태로 확인합니다.

검증 명령은 `make test-integration test`, `make CONCURRENCY=sequential test-integration`, `make BUILD_MODE=asan test-integration test`, 독립 CGI 회귀 검사는 `make test-cgi`와 `make BUILD_MODE=asan test-cgi`입니다. Linux 실제 실행, CGI 후손 프로세스 종료, 전체 HTTP 요청·응답의 기한은 아직 확인·구현하지 않았습니다.


## CGI 최종 확인: curl로 요청 흐름 재현

```sh
make all cgi
./build/threadpool/webserver 8080 ./www ./build/threadpool/cgi-add
```

다른 터미널에서 실행합니다.

```sh
curl -i http://127.0.0.1:8080/cgi/add --data-raw 'a=2&b=3'
curl -i http://127.0.0.1:8080/cgi/add --data-raw 'a=2&b='
curl -i http://127.0.0.1:8080/add --data-raw 'a=2&b=3'
curl -i http://127.0.0.1:8080/
```

첫 요청은 HTTP 200, Content-Type application/json, Content-Length 12, Connection close, 본문 `{"result":5}`를 받습니다. 두 번째는 CGI가 만든 400 오류 응답입니다. 나머지는 기존 서버 내부 덧셈과 정적 파일 처리를 확인합니다. 종료는 서버 터미널의 Ctrl+C입니다.

실제 curl로 위 네 요청과 서로 다른 입력의 동시 CGI 요청 4개를 확인했습니다. 본문과 Content-Length, Connection 헤더를 검사했고 검증용 서버도 종료했습니다. 이 확인은 처리량이나 부하 한계를 측정한 벤치마크는 아닙니다.

```text
accept → 작업자 배정 → recv로 요청 누적 → HTTP 파싱
       → POST /cgi/add 분기 → CGI 실행·입출력·수집·회수
       → CGI 출력 파싱 → HTTP 응답 검증·송신
       → CGI 출력 버퍼와 요청 버퍼 해제 → 연결 fd 닫기
```

여기까지가 현재 CGI 기반 동적 요청 처리의 기본 구현 범위입니다. 필수 과제 전체에서는 Proxy 또는 Reverse Proxy와 최종 부하·이식성 검증이 남아 있습니다. 추가 동시성 모델 체험은 현재 스레드 풀 구현과 별도의 학습 범위입니다.

