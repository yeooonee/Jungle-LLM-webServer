# Jungle-LLM-webServer

외부 HTTP/이벤트 루프 라이브러리 없이 **C11**과 **POSIX 소켓 API**만으로 구현하는 웹 서버 및 프록시 프로젝트입니다.

> 현재 HTTP 요청 파싱·응답 송신, GET 정적 파일 서빙, POST 덧셈을 제공합니다. 빌드에서 순차 처리 또는 스레드 풀을 선택하며, 기본값은 스레드 풀입니다. 프록시는 이후 구현 범위입니다.

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

> `dynamic/` 모듈은 동적 요청 처리 방식이 정해진 뒤 필요 시 추가합니다.

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

실행 인자는 `[포트] [정적 루트]`이며 생략하면 `8080`, `www`를 사용합니다. `0.0.0.0`에 바인딩하므로 다른 컴퓨터에서는 서버의 실제 IP로 접속합니다. 서버는 Ctrl+C 또는 SIGTERM으로 종료합니다.

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

`src/http/response.h`의 `http_response_send()`는 상태 라인, `Content-Type`, `Content-Length`, `Connection: close`, 빈 줄을 생성하고 본문을 송신합니다. 상태 코드는 현재 200·400·403·404·413·500·501을 지원합니다.

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
- 전역 계산 상태는 없으며 각 요청의 숫자·합계·응답 버퍼는 해당 호출에서만 유지합니다. CGI는 아직 연결하지 않았으며, 동시 처리 방식은 빌드 옵션으로 선택합니다.

