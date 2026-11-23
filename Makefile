CC       ?= cc
UNAME_S  := $(shell uname -s)

BUILD_DIR := build
SRC_DIR   := src

CONCURRENCY ?= threadpool
ifeq ($(CONCURRENCY),sequential)
  DISPATCH_SRC := $(SRC_DIR)/net/dispatch_sequential.c
else ifeq ($(CONCURRENCY),threadpool)
  DISPATCH_SRC := $(SRC_DIR)/net/dispatch_threadpool.c
else
  $(error Unsupported CONCURRENCY '$(CONCURRENCY)'; use sequential or threadpool)
endif

ALL_SRCS := $(sort $(wildcard $(SRC_DIR)/*.c $(SRC_DIR)/*/*.c $(SRC_DIR)/*/*/*.c))
SRCS := $(filter-out $(SRC_DIR)/net/dispatch_%.c,$(ALL_SRCS)) $(DISPATCH_SRC)
OUTPUT_DIR := $(BUILD_DIR)/$(CONCURRENCY)

BASE_CPPFLAGS := -I$(SRC_DIR)
ifeq ($(UNAME_S),Darwin)
  PLATFORM_CPPFLAGS := -D_DARWIN_C_SOURCE
else
  PLATFORM_CPPFLAGS := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
endif

BASE_CFLAGS  := -std=c11 -Wall -Wextra -Werror -Wpedantic
BASE_LDFLAGS := -pthread

BUILD_MODE ?= debug

ifeq ($(BUILD_MODE),release)
  MODE_CFLAGS  := -O2 -DNDEBUG
  MODE_LDFLAGS :=
  TARGET       := $(OUTPUT_DIR)/webserver-release
else ifeq ($(BUILD_MODE),asan)
  MODE_CFLAGS  := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
  MODE_LDFLAGS := -fsanitize=address,undefined
  TARGET       := $(OUTPUT_DIR)/webserver-asan
else
  MODE_CFLAGS  := -O0 -g
  MODE_LDFLAGS :=
  TARGET       := $(OUTPUT_DIR)/webserver
endif

OBJ_DIR  := $(OUTPUT_DIR)/obj-$(BUILD_MODE)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

CPPFLAGS := $(BASE_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(EXTRA_CPPFLAGS)
CFLAGS   := $(BASE_CFLAGS) $(MODE_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS  := $(BASE_LDFLAGS) $(MODE_LDFLAGS) $(EXTRA_LDFLAGS)

.PHONY: all debug release asan test test-integration clean help

all: $(TARGET)

debug:
	@$(MAKE) --no-print-directory BUILD_MODE=debug all

release:
	@$(MAKE) --no-print-directory BUILD_MODE=release all

asan:
	@$(MAKE) --no-print-directory BUILD_MODE=asan all

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

UNIT_SRCS := $(sort $(wildcard tests/unit/test_*.c))

test:
	@if [ -z "$(UNIT_SRCS)" ]; then \
		echo "No unit tests found in tests/unit/ yet."; \
	else \
		echo "Running unit tests..."; \
		for t in $(UNIT_SRCS); do \
			bin="$(OUTPUT_DIR)/tests-$(BUILD_MODE)/$$(basename $$t .c)"; \
			mkdir -p "$(OUTPUT_DIR)/tests-$(BUILD_MODE)"; \
			$(CC) $(CPPFLAGS) $(filter-out -DNDEBUG,$(CFLAGS)) "$$t" $(filter-out $(SRC_DIR)/main.c,$(SRCS)) -o "$$bin" $(LDFLAGS) && "$$bin" || exit 1; \
		done; \
	fi

clean:
	rm -rf $(BUILD_DIR)

test-integration: $(TARGET)
	python3 tests/integration/test_static.py $(TARGET)
	python3 tests/integration/test_concurrency.py $(TARGET) $(CONCURRENCY)

help:
	@echo "CONCURRENCY=threadpool (default) or sequential"
	@echo "Targets:"
	@echo "  make          Build default debug binary ($(OUTPUT_DIR)/webserver)"
	@echo "  make release  Build optimized binary ($(OUTPUT_DIR)/webserver-release)"
	@echo "  make asan     Build with ASan + UBSan ($(OUTPUT_DIR)/webserver-asan)"
	@echo "  make test     Build and run unit tests in tests/unit/"
	@echo "  make test-integration  Run static HTTP checks (Python 3 required)"
	@echo "  make clean    Remove build artifacts"

-include $(DEPS)
