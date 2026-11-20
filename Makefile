CC = cc
CFLAGS = -Wall -Wextra

all: server server_copy

server: src/server.c
	$(CC) $(CFLAGS) -o $@ $<

# SO_REUSEADDR 끈 실험용 (포트 8070)
server_copy: experiments/server_copy.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f server server_copy

.PHONY: all clean
