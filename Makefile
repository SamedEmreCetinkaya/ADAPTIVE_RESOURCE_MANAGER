CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wformat=2 -Wshadow
# Exploit mitigations for a daemon that runs as root. Sanitizer builds leave them out.
HARDENING_CFLAGS := -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE
HARDENING_CFLAGS += $(shell $(CC) -fcf-protection -E -x c /dev/null > /dev/null 2>&1 && echo -fcf-protection)
HARDENING_LDFLAGS := -pie -Wl,-z,relro,-z,now,-z,noexecstack
SOURCES := $(wildcard SOURCE/*.c)
HEADERS := $(wildcard SOURCE/*.h)
BUILD_DIRECTORY := BUILD
BINARY := $(BUILD_DIRECTORY)/adaptive-resource-managerd

MEMORY_HOLDER := $(BUILD_DIRECTORY)/memory-holder

.PHONY: all sanitize analyze clean test

all: $(BINARY) $(MEMORY_HOLDER)

$(MEMORY_HOLDER): TESTS/memory_holder.c
	mkdir -p $(BUILD_DIRECTORY)
	$(CC) $(CFLAGS) $(HARDENING_CFLAGS) -o $@ $< $(HARDENING_LDFLAGS)

$(BINARY): $(SOURCES) $(HEADERS)
	mkdir -p $(BUILD_DIRECTORY)
	$(CC) $(CFLAGS) $(HARDENING_CFLAGS) -o $@ $(SOURCES) -lm $(HARDENING_LDFLAGS)

sanitize: $(SOURCES) $(HEADERS)
	mkdir -p $(BUILD_DIRECTORY)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -o $(BINARY)-sanitized $(SOURCES) -lm

# Static analysis of memory and file handling. Prints nothing when it finds nothing.
analyze:
	$(CC) $(CFLAGS) -fanalyzer -o /dev/null $(SOURCES) -lm 2>&1 | grep -E 'warning:|error:' || true
	$(CC) $(CFLAGS) -fanalyzer -o /dev/null TESTS/memory_holder.c 2>&1 | grep -E 'warning:|error:' || true

test: all sanitize
	sh TESTS/RUN_UNPRIVILEGED_TESTS.sh

clean:
	rm -rf $(BUILD_DIRECTORY)
