CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
override CPPFLAGS += -Iinclude

BUILD_DIR := build
PROGRAMS := tcp_server tcp_client cgi_sum tcp_server_forward tcp_server_reverse

.PHONY: all clean $(PROGRAMS)

all: tcp_server tcp_client

$(PROGRAMS): %: $(BUILD_DIR)/%

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/tcp_server: src/tcp_server.c src/http.c src/file_pool.c src/cgi.c \
                        include/http.h include/file_pool.h include/cgi.h | $(BUILD_DIR)/cgi_sum
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(BUILD_DIR)/tcp_server_forward: src/tcp_server_forward.c src/proxy_cache.c src/proxy_response.c \
                                src/http.c include/proxy_cache.h include/proxy_response.h \
                                include/http.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(BUILD_DIR)/tcp_server_reverse: src/tcp_server_reverse.c src/proxy_response.c src/http.c \
                                src/file_pool.c include/proxy_response.h include/http.h \
                                include/file_pool.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

$(BUILD_DIR)/cgi_sum: src/cgi_sum.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILD_DIR)/tcp_client: src/tcp_client.c include/http.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(addprefix $(BUILD_DIR)/,$(PROGRAMS))
