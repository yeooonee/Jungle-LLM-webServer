#ifndef PROXY_ROUTE_H
#define PROXY_ROUTE_H

#include "proxy/forward.h"

/* shortcut: 경로당 백엔드는 최대 8개이며 더 필요하면 저장 방식을 확장한다. */
#define PROXY_MAX_BACKENDS 8

typedef struct {
  const char *prefix;
  proxy_backend backends[PROXY_MAX_BACKENDS];
  size_t backend_count;
  size_t next_backend;
} proxy_route;

/* shortcut: 규칙은 최대 16개이며 더 큰 설정이 필요하면 저장 방식을 확장한다. */
#define PROXY_MAX_ROUTES 16

typedef struct {
  proxy_route items[PROXY_MAX_ROUTES];
  size_t count;
} proxy_routes;

/* routes는 0으로 초기화하고 이 함수로만 등록한다. 문자열은 빌려 쓴다. 성공 0, 실패 -1과 errno이며 실패 시 설정은 유지한다.
 * 같은 경로이면 목록에 추가한다. 동일 주소·포트 중복 EEXIST,
 * 경로 한도 ENOSPC, 백엔드 한도 E2BIG, 잘못된 입력 EINVAL. */
int proxy_routes_add(proxy_routes *routes, const char *prefix, proxy_backend backend);

/* 일치하는 가장 긴 경로의 규칙을 빌려 반환한다. 없으면 NULL이다. */
proxy_route *proxy_route_select(proxy_routes *routes, http_slice target);

/* 시작 후 설정은 바꾸지 않는다. 선택·순번 갱신은 내부 뮤텍스로 보호한다.
 * 성공 시 목록의 백엔드를 빌려 반환하고, 실패 시 NULL과 errno를 반환한다. */
const proxy_backend *proxy_backend_next(proxy_route *route);

/* 경로는 /로 시작하고, / 자체를 제외하면 /로 끝나지 않는다.
 * 쿼리·fragment·공백·제어문자·비ASCII·역슬래시는 허용하지 않는다. */
int proxy_route_prefix_valid(const char *prefix);

/* 검증된 prefix와 하위 경로를 선택한다. 디코딩·경로 재작성은 하지 않는다. */
int proxy_route_matches(const char *prefix, http_slice target);

#endif
