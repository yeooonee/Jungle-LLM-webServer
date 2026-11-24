CC = cc
CFLAGS = -Wall -Wextra

CGI_PROGS = $(patsubst cgi-src/%.c,build/cgi-bin/%,$(wildcard cgi-src/*.c))

all: build/server build/server_copy $(CGI_PROGS)

SRCS = src/main.c src/http.c src/static.c src/cgi.c src/pool.c

build/server: $(SRCS) src/http.h src/static.h src/cgi.h src/pool.h
	@mkdir -p build
	$(CC) $(CFLAGS) -pthread -o $@ $(SRCS)

# CGI 프로그램: cgi-src/이름.c → build/cgi-bin/이름 (server.conf의 cgi_root)
build/cgi-bin/%: cgi-src/%.c
	@mkdir -p build/cgi-bin
	$(CC) $(CFLAGS) -o $@ $<

# SO_REUSEADDR 끈 실험용 (포트 8070)
build/server_copy: experiments/server_copy.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

test: all
	python3 tests/test_concurrency.py

clean:
	rm -rf build

.PHONY: all clean test
