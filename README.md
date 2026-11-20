# Jungle-LLM-webServer

외부 HTTP/이벤트 루프 라이브러리 없이 **C11**과 **POSIX 소켓 API**만으로 구현하는 웹 서버 및 프록시 프로젝트입니다.

> 현재 프로젝트 디렉터리 구조와 빌드 부트스트랩 단계이며, 동시성 모델·프록시 세부 방식·프로토콜 세부 구현은 추후 설계 단계에서 결정합니다.

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
./build/webserver

# 단위 테스트 실행
make test

# 빌드 산출물 정리
make clean
```
