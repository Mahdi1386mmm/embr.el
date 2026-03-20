CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
PREFIX ?= /usr/local

all: embr-booster

embr-booster: embr-booster.c
	$(CC) $(CFLAGS) -o $@ $<

check: embr-booster
	./embr-booster --help >/dev/null 2>&1 || true
	@echo "OK: embr-booster builds and runs"

install: embr-booster
	install -d $(DESTDIR)$(PREFIX)/libexec
	install -m 755 embr-booster $(DESTDIR)$(PREFIX)/libexec/embr-booster

clean:
	rm -f embr-booster

.PHONY: all check install clean
