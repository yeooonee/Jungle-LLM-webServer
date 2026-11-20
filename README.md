# Jungle-LLM-webServer

외부 HTTP/이벤트 루프 라이브러리 없이 **C11**과 **POSIX 소켓 API**만으로 구현하는 웹 서버 및 프록시 프로젝트입니다.

> 현재 IPv4 TCP 에코 서버를 구현한 단계입니다. 클라이언트 하나씩 순차 처리하며, HTTP·정적 파일·프록시·동시 요청 처리는 이후 학습과 구현 범위입니다.

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
