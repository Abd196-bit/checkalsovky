CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

.PHONY: all clean engines

all: checkalsovky

checkalsovky: src/checkalsovky.c
	$(CC) $(CFLAGS) -o $@ $<

engines:
	$(MAKE) checkalsovky
	$(MAKE) -C stockfish/src -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2) build
	cd reckless && cargo rustc --release --no-default-features --bin reckless -- -C target-cpu=native --emit link=reckless

clean:
	rm -f checkalsovky
