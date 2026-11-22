# Jungle-LLM-webServer

외부 HTTP/이벤트 루프 라이브러리 없이 **C11**과 **POSIX 소켓 API**만으로 구현하는 웹 서버 및 프록시 프로젝트입니다.

> 현재 IPv4 TCP 에코 서버와 독립 HTTP 요청 파서를 구현한 단계입니다. 실행 서버는 여전히 에코 방식이며, 파서의 소켓 수신 연결·HTTP 응답·정적 파일·프록시·동시 요청 처리는 이후 구현 범위입니다.

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
# 기본 디버그 빌드 (build/webserver)
make

# 릴리스 빌드 (build/webserver-release)
make release

# AddressSanitizer + UndefinedBehaviorSanitizer 빌드 (build/webserver-asan)
make asan

# 실행
./build/webserver 8080

# 다른 터미널에서 접속 후 입력한 내용이 돌아오는지 확인
nc 127.0.0.1 8080

# 단위 테스트 실행
make test

# ASan + UBSan으로 단위 테스트 실행
make BUILD_MODE=asan test

# 실제 TCP 통합 테스트 (Python 3 필요)
make test-integration

# sanitizer 빌드로 통합 테스트
make asan
python3 tests/integration/test_tcp.py build/webserver-asan

# 빌드 산출물 정리
make clean
```

포트를 생략하면 `8080`을 사용합니다. `0.0.0.0`에 바인딩하므로 다른 컴퓨터에서는 서버의 실제 IP로 접속합니다. 서버는 Ctrl+C 또는 SIGTERM으로 종료합니다.

연결 소켓은 블로킹 송수신을 사용합니다. 250ms 대기 제한으로 종료 요청을 주기적으로 확인하며, 대기 시간 만료만으로 연결을 끊지는 않습니다. 유휴 연결 하나가 다음 연결 처리를 막는 순차 서버입니다. 종료 요청 시 현재 연결도 닫습니다.

학습 내용과 코드의 대응은 [TCP 소켓 서버 학습 문서](docs/learning/tcp/01-tcp-socket-server.md), 현재 학습 상태는 [진행 기록](docs/learning/tcp/progress.md)에 정리했습니다.

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
