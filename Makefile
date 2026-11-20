CC = cc
CFLAGS = -Wall -Wextra

all: build/server build/server_copy

build/server: src/server.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

# SO_REUSEADDR 끈 실험용 (포트 8070)
build/server_copy: experiments/server_copy.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf build

.PHONY: all clean
