// 정적 파일 서빙: URL 경로 → 루트 안의 파일 → 응답
#ifndef STATIC_H
#define STATIC_H

void serve_file(int fd, const char *root, const char *url_path);

#endif
