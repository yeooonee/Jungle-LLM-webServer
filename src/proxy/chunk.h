#ifndef PROXY_CHUNK_H
#define PROXY_CHUNK_H

#include "proxy/body.h"

/* proxy_body_consume에서 사용하는 내부 chunked 처리 함수다. */
http_parse_result proxy_chunk_consume(proxy_body *body, http_slice input, int eof,
                                        http_slice *forward);

#endif
