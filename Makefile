CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L

.PHONY: all clean

all: tcp_server tcp_client

tcp_server: tcp_server.c http.c http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

tcp_client: tcp_client.c http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f tcp_server tcp_client
