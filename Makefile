CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L

.PHONY: all clean

all: tcp_server tcp_client

tcp_server: tcp_server.c http.c http.h file_pool.c file_pool.h cgi.c cgi.h | cgi_sum
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

tcp_server_forward: tcp_server_forward.c proxy_cache.c proxy_cache.h proxy_response.c proxy_response.h http.c http.h file_pool.c file_pool.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

tcp_server_reverse: tcp_server_reverse.c proxy_response.c proxy_response.h http.c http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

cgi_sum: cgi_sum.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

tcp_client: tcp_client.c http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f tcp_server tcp_client cgi_sum tcp_server_forward tcp_server_reverse
