# zram-tier-bench 构建。C 源码由 CodeLens 维护；运行脚本/README 由 DevCI 维护。
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

all: zram-tier-bench

zram-tier-bench: zram_tier_bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f zram-tier-bench

.PHONY: all clean
