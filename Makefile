CC = cc
CFLAGS = -Wall -Wextra

all: build/server build/server_copy

SRCS = src/main.c src/http.c src/static.c

build/server: $(SRCS) src/http.h src/static.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS)

# SO_REUSEADDR 끈 실험용 (포트 8070)
build/server_copy: experiments/server_copy.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf build

.PHONY: all clean
