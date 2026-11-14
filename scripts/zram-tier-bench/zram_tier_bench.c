// SPDX-License-Identifier: Apache-2.0
/*
 * zram_tier_bench.c - 100 MB 工作集的三态内存性能对照
 *
 *   状态 A：全部驻留 DRAM。
 *   状态 B：process_madvise(pidfd, iovec, MADV_PAGEOUT) 把工作集换出到 zram。
 *   状态 C：zram 的 backing_dev 指向内存盘（brd / RAM 块设备），
 *           echo all > idle 后 echo idle > writeback，把 zram 内容淘汰到后备盘。
 *
 * 每个状态测两类指标：
 *   1) 冷访问：按随机页序首次触碰每一页，统计单页换入延迟分布与换入吞吐；
 *   2) 稳态：全部页换回后，随机 8B 读/写吞吐（对照组，说明三态差别在换入路径）。
 *
 * 状态判据不是“以为换出了”，而是每步读：
 *   /proc/self/status 的 VmRSS / VmSwap
 *   /sys/block/zram0/mm_stat（字节）
 *   /sys/block/zram0/bd_stat（4K 单位，见 zram_drv.c:1191 的 FOUR_K）
 *
 * 依赖：
 *   Linux >= 5.10（process_madvise，需要 CAP_SYS_NICE，mm/madvise.c:1868）
 *   zram 模块 + 已 swapon 的 zram 交换区
 *   状态 C 额外需要 backing_dev 指向一个块设备（见 setup-zram-l2.sh）
 *
 * 编译：
 *   gcc -O2 -Wall -Wextra -o zram-tier-bench zram_tier_bench.c
 *
 * 运行（必须 root，否则没有 CAP_SYS_NICE，也写不了 writeback）：
 *   sudo ./zram-tier-bench --size-mb 100 --dev zram0
 *
 * 注意：
 *   - 只在独占测试机上跑。swap/page-cluster/writeback 都是宿主级、破坏性操作。
 *   - swap 里已有别的进程的页、swap readahead 未关、THP、CPU 频率/绑核
 *     都会污染单页延迟。setup 脚本会关 page-cluster，其余在 README 里列。
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_process_madvise
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_process_madvise 440
#else
#error "unknown syscall number for process_madvise; define SYS_process_madvise"
#endif
#endif

#ifndef SYS_pidfd_open
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_pidfd_open 434
#else
#error "unknown syscall number for pidfd_open; define SYS_pidfd_open"
#endif
#endif

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

#define PAGE_SZ 4096u
#define MIB (1ull << 20)
#define PAGEOUT_CHUNK (32ull * MIB)	/* 单次 process_madvise 的建议范围 */
#define NO_PIDFD (-1)

enum cold_mode { COLD_READ = 0, COLD_WRITE = 1, COLD_BOTH = 2 };

static uint64_t rng_state = 0x243f6a8885a308d3ull;
static volatile uint64_t g_sink;	/* 防止读操作被优化掉 */
static FILE *g_csv;

static inline uint64_t rng_next(void)
{
	uint64_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

/* n < 2^32，用乘法取模，避免上亿次除法污染 fill/bench 的时间 */
static inline size_t rng_below(uint64_t n)
{
	uint64_t r = rng_next() >> 32;

	return (size_t)((r * n) >> 32);
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void dief(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void warnf(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "warn:  ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void csv_row(const char *state, const char *metric, double value,
		    const char *unit)
{
	if (!g_csv)
		return;
	fprintf(g_csv, "%s,%s,%.3f,%s\n", state, metric, value, unit);
}

/* ---------- 读文件 ---------- */

static unsigned long long read_u64_file(const char *path)
{
	FILE *f = fopen(path, "r");
	unsigned long long v = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%llu", &v) != 1)
		v = 0;
	fclose(f);
	return v;
}

static char read_str_file(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	buf[0] = '\0';
	if (!f)
		return 0;
	if (!fgets(buf, (int)len, f)) {
		buf[0] = '\0';
		fclose(f);
		return 0;
	}
	fclose(f);
	n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return 1;
}

/* /proc/self/status 的 VmRSS:/VmSwap:，单位 kB */
static unsigned long long status_kb(const char *key)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	unsigned long long kb = 0;
	size_t klen = strlen(key);

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, klen) == 0) {
			if (sscanf(line + klen, "%llu", &kb) != 1)
				kb = 0;
			break;
		}
	}
	fclose(f);
	return kb;
}

static unsigned long long status_swap_kb(void) { return status_kb("VmSwap:"); }
static unsigned long long status_rss_kb(void) { return status_kb("VmRSS:"); }

/* /sys/block/<dev>/<attr> 里按空白分隔的整数数组，返回个数 */
static int read_u64_vec(const char *path, uint64_t *out, int max)
{
	FILE *f = fopen(path, "r");
	int n = 0;

	if (!f)
		return -1;
	while (n < max) {
		unsigned long long v;

		if (fscanf(f, "%llu", &v) != 1)
			break;
		out[n++] = (uint64_t)v;
	}
	fclose(f);
	return n;
}

static int write_attr(const char *dev, const char *attr, const char *val)
{
	char path[160];
	int fd;
	size_t len = strlen(val);

	snprintf(path, sizeof(path), "/sys/block/%s/%s", dev, attr);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (write(fd, val, len) != (ssize_t)len) {
		int err = -errno;

		close(fd);
		return err;
	}
	close(fd);
	return 0;
}

/* 本进程 user+sys CPU 时间，用来把“性能”拆成耗时与 CPU 两列 */
static uint64_t proc_cpu_ns(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ((uint64_t)ru.ru_utime.tv_sec + (uint64_t)ru.ru_stime.tv_sec) *
	       1000000000ull +
	       ((uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_usec) *
	       1000ull;
}

/* FNV-1a over 8B words：换出/换回前后各算一次，校验数据没有被写坏 */
static uint64_t checksum_region(const uint8_t *base, size_t size)
{
	const uint64_t *p = (const uint64_t *)base;
	size_t n = size / 8;
	uint64_t h = 1469598103934665603ull;

	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

static void report_checksum(const char *state, uint64_t ref, uint64_t now)
{
	if (ref == now) {
		printf("  数据校验(%s): OK (hash=%016" PRIx64 ")\n", state, ref);
		return;
	}
	/* 数据坏了就不该有“有效结果”：非零退出，让运行脚本把整轮判废 */
	fprintf(stderr, "error: 数据校验(%s) FAIL ref=%016" PRIx64
		" now=%016" PRIx64 "，zram/L2 路径写坏了数据；本轮作废\n",
		state, ref, now);
	exit(3);
}

/*
 * 只校验每页头 8B 以外的内容。cold-write 会改写每页第 0 个 8B 字，
 * 页面其余部分必须原样，用来在不保留写前副本的情况下判数据完整性。
 */
static uint64_t checksum_skip_head(const uint8_t *base, size_t npages)
{
	const uint64_t *p = (const uint64_t *)base;
	size_t wpp = PAGE_SZ / 8;
	uint64_t h = 1469598103934665603ull;

	for (size_t pg = 0; pg < npages; pg++) {
		for (size_t w = 1; w < wpp; w++) {
			h ^= p[pg * wpp + w];
			h *= 1099511628211ull;
		}
	}
	return h;
}

/* ---------- 统计 ---------- */

struct dist_summary {
	double mean_us;
	double p50_us;
	double p95_us;
	double p99_us;
	double max_us;
};

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static struct dist_summary summarize(const uint64_t *lat, size_t n)
{
	struct dist_summary s = { 0 };
	uint64_t *copy;
	double sum = 0;

	if (n == 0)
		return s;
	copy = malloc(n * sizeof(*copy));
	if (!copy)
		dief("out of memory for latency summary");
	memcpy(copy, lat, n * sizeof(*copy));
	qsort(copy, n, sizeof(*copy), cmp_u64);

	for (size_t i = 0; i < n; i++)
		sum += (double)lat[i];
	s.mean_us = sum / (double)n / 1000.0;
	s.p50_us = (double)copy[n / 2] / 1000.0;
	s.p95_us = (double)copy[(size_t)((double)n * 0.95)] / 1000.0;
	s.p99_us = (double)copy[(size_t)((double)n * 0.99)] / 1000.0;
	s.max_us = (double)copy[n - 1] / 1000.0;
	free(copy);
	return s;
}

static void shuffle_pages(uint32_t *a, size_t n)
{
	for (size_t i = n - 1; i > 0; i--) {
		size_t j = rng_below((uint64_t)i + 1);
		uint32_t t = a[i];

		a[i] = a[j];
		a[j] = t;
	}
}

/* ---------- 工作集操作 ---------- */

/* 随机 8B 写，把整块内存写满（13M 次写覆盖 25600 个页） */
static void fill_random(uint8_t *base, size_t size)
{
	size_t n = size / 8;

	for (size_t i = 0; i < n; i++) {
		size_t off = rng_below(n) * 8;

		*(volatile uint64_t *)(base + off) = rng_next();
	}
}

/*
 * 可压缩数据：整页 'A'，每页唯一 salt。
 * 唯一 salt 是必须的：完全相同的页会变成 zram 的 ZRAM_SAME，而
 * writeback_store 明确跳过 ZRAM_SAME（zram_drv.c:683-686），
 * 那样 C 组根本不会产生 L2 数据。
 */
static void fill_text(uint8_t *base, size_t npages, const uint32_t *perm)
{
	for (size_t i = 0; i < npages; i++) {
		uint8_t *p = base + (size_t)perm[i] * PAGE_SZ;
		uint64_t salt = 0x9e3779b97f4a7c15ull * (uint64_t)(perm[i] + 1);

		memset(p, 'A', PAGE_SZ);
		memcpy(p, &salt, sizeof(salt));
		memcpy(p + 8, "AgentENV zram-tier", 19);
	}
}

static double bench_random_read(uint8_t *base, size_t size, uint64_t ops)
{
	size_t n = size / 8;
	uint64_t t0 = now_ns();

	for (uint64_t i = 0; i < ops; i++) {
		size_t off = rng_below(n) * 8;

		g_sink += *(volatile uint64_t *)(base + off);
	}
	return (double)(now_ns() - t0) / 1e9;
}

static double bench_random_write(uint8_t *base, size_t size, uint64_t ops)
{
	size_t n = size / 8;
	uint64_t t0 = now_ns();

	for (uint64_t i = 0; i < ops; i++) {
		size_t off = rng_below(n) * 8;

		*(volatile uint64_t *)(base + off) = rng_next();
	}
	return (double)(now_ns() - t0) / 1e9;
}

/*
 * 冷换入：按随机页序每页只读一次，逐页计时。
 * 这一步会把所有页换回 DRAM，所以之后必须重新 pageout 才能进入下一态。
 */
static void fault_pass(uint8_t *base, size_t npages, const uint32_t *perm,
		       uint64_t *lat)
{
	struct rusage before;
	struct rusage after;
	uint64_t t0 = now_ns();
	uint64_t cpu0 = proc_cpu_ns();

	getrusage(RUSAGE_SELF, &before);
	for (size_t i = 0; i < npages; i++) {
		uint8_t *p = base + (size_t)perm[i] * PAGE_SZ;
		uint64_t s = now_ns();

		g_sink += *(volatile uint64_t *)p;
		lat[i] = now_ns() - s;
	}
	getrusage(RUSAGE_SELF, &after);

	printf("  缺页: minor +%ld, major +%ld\n",
	       (long)(after.ru_minflt - before.ru_minflt),
	       (long)(after.ru_majflt - before.ru_majflt));
	printf("  逐页访问 wall: %.3f s, CPU %.1f ms\n",
	       (double)(now_ns() - t0) / 1e9,
	       (double)(proc_cpu_ns() - cpu0) / 1e6);
}

static void report_fault(const char *state, const uint64_t *lat, size_t npages,
			 size_t size)
{
	struct dist_summary s = summarize(lat, npages);
	double sum_ns = 0;

	for (size_t i = 0; i < npages; i++)
		sum_ns += (double)lat[i];
	printf("  逐页首次访问(%s): P50=%.1f us P95=%.1f us P99=%.1f us max=%.1f us"
	       " mean=%.1f us, %.1f MB/s (%.0f 页/s)\n",
	       state, s.p50_us, s.p95_us, s.p99_us, s.max_us, s.mean_us,
	       (double)size / MIB / (sum_ns / 1e9),
	       (double)npages / (sum_ns / 1e9));
	csv_row(state, "fault_p50_us", s.p50_us, "us");
	csv_row(state, "fault_p95_us", s.p95_us, "us");
	csv_row(state, "fault_p99_us", s.p99_us, "us");
	csv_row(state, "fault_throughput_mbs", (double)size / MIB / (sum_ns / 1e9), "MB/s");
}

/* 首次写：直接写每页第 0 个 8B 字，触发 swap-in + 写缺页 */
static void cold_write_pass(uint8_t *base, size_t npages, const uint32_t *perm,
			    uint64_t *lat)
{
	struct rusage before;
	struct rusage after;
	uint64_t t0 = now_ns();
	uint64_t cpu0 = proc_cpu_ns();

	getrusage(RUSAGE_SELF, &before);
	for (size_t i = 0; i < npages; i++) {
		uint8_t *p = base + (size_t)perm[i] * PAGE_SZ;
		uint64_t v = rng_next();	/* RNG 开销放在计时区间之外 */
		uint64_t s = now_ns();

		*(volatile uint64_t *)p = v;
		lat[i] = now_ns() - s;
	}
	getrusage(RUSAGE_SELF, &after);

	printf("  缺页: minor +%ld, major +%ld\n",
	       (long)(after.ru_minflt - before.ru_minflt),
	       (long)(after.ru_majflt - before.ru_majflt));
	printf("  逐页访问 wall: %.3f s, CPU %.1f ms\n",
	       (double)(now_ns() - t0) / 1e9,
	       (double)(proc_cpu_ns() - cpu0) / 1e6);
}

/* ---------- 状态操作 ---------- */

static ssize_t advise_pageout(int pidfd, void *addr, size_t len)
{
	size_t done = 0;

	while (done < len) {
		size_t n = len - done;
		struct iovec iov;
		ssize_t ret;

		if (n > PAGEOUT_CHUNK)
			n = (size_t)PAGEOUT_CHUNK;
		/* base 必须页对齐：mmap 首地址与 32MB 步进都是 4K 对齐 */
		iov.iov_base = (char *)addr + done;
		iov.iov_len = n;
		ret = syscall(SYS_process_madvise, pidfd, &iov, 1,
			      (int)MADV_PAGEOUT, 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			break;	/* 没有可回收的页 */
		done += (size_t)ret;
	}
	return (ssize_t)done;
}

static ssize_t advise_pageout_fallback(void *addr, size_t len)
{
	if (madvise(addr, len, MADV_PAGEOUT) < 0)
		return -errno;
	return (ssize_t)len;
}

static double pageout(int pidfd, void *base, size_t size, const char *phase)
{
	uint64_t t0 = now_ns();
	uint64_t t1;
	uint64_t cpu0 = proc_cpu_ns();
	ssize_t advised;

	if (pidfd >= 0) {
		advised = advise_pageout(pidfd, base, size);
		if (advised < 0 && (advised == -EPERM || advised == -ENOSYS ||
				    advised == -EINVAL)) {
			warnf("process_madvise 失败(%s)，回退 madvise(MADV_PAGEOUT)；"
			      "这条数据没有走 pidfd 路径", strerror((int)-advised));
			advised = advise_pageout_fallback(base, size);
		}
	} else {
		warnf("pidfd_open 不可用，回退 madvise(MADV_PAGEOUT)");
		advised = advise_pageout_fallback(base, size);
	}
	if (advised < 0)
		dief("MADV_PAGEOUT 失败: %s", strerror((int)-advised));
	t1 = now_ns();

	printf("  pageout: %.1f MB in %.3f s (%.0f MB/s, CPU %.1f ms)\n",
	       (double)size / MIB, (double)(t1 - t0) / 1e9,
	       (double)size / MIB / ((double)(t1 - t0) / 1e9),
	       (double)(proc_cpu_ns() - cpu0) / 1e6);
	csv_row(phase, "pageout_mbs", (double)size / MIB / ((double)(t1 - t0) / 1e9), "MB/s");
	return (double)(t1 - t0) / 1e9;
}

static void zram_stats(const char *dev, const char *tag)
{
	char path[160];
	uint64_t mm[9] = { 0 };
	uint64_t bd[3] = { 0 };
	int n;

	snprintf(path, sizeof(path), "/sys/block/%s/mm_stat", dev);
	n = read_u64_vec(path, mm, 9);
	if (n >= 3)
		printf("  [%s] mm_stat: orig=%.1f MB comp=%.1f MB mem_used=%.1f MB"
		       " same_pages=%" PRIu64 " huge=%" PRIu64 "\n",
		       tag, (double)mm[0] / MIB, (double)mm[1] / MIB,
		       (double)mm[2] / MIB,
		       (uint64_t)(n >= 6 ? mm[5] : 0),
		       (uint64_t)(n >= 8 ? mm[7] : 0));
	else
		warnf("读不到 %s（zram 未初始化？）", path);

	snprintf(path, sizeof(path), "/sys/block/%s/bd_stat", dev);
	n = read_u64_vec(path, bd, 3);
	if (n >= 3)
		printf("  [%s] bd_stat (4K 单位): bd_count=%" PRIu64
		       " (=%.1f MB) bd_reads=%" PRIu64 " bd_writes=%" PRIu64 "\n",
		       tag, bd[0], (double)bd[0] * PAGE_SZ / MIB, bd[1], bd[2]);
}

static void expect_swapped(const char *tag, size_t size)
{
	unsigned long long rss = status_rss_kb();
	unsigned long long sw = status_swap_kb();
	unsigned long long want_kb = (unsigned long long)(size / 1024);

	printf("  [check] %s: VmRSS=%.1f MB VmSwap=%.1f MB (want VmSwap≈%.0f MB)\n",
	       tag, (double)rss / 1024.0, (double)sw / 1024.0,
	       (double)want_kb / 1024.0);
	if (sw < want_kb * 85 / 100)
		warnf("%s: VmSwap 只有 %.0f MB，换出没达到 %.0f MB（swap 空间不足、"
		      "页被跳过，或别的进程在抢 swap？）",
		      tag, (double)sw / 1024.0, (double)want_kb / 1024.0);
	else
		csv_row(tag, "vmswap_mb", (double)sw / 1024.0, "MB");
	if (rss > (unsigned long long)(size / 1024) / 10)
		printf("  [hint] RSS 仍较高（%.1f MB），可能有页没换出\n",
		       (double)rss / 1024.0);
}

/* ---------- 每态一轮 ---------- */

struct run_cfg {
	size_t size;
	uint64_t ops;
	uint64_t seed;
	const char *dev;
	int pattern_text;
	int cold_mode;
};

static void steady_passes(const char *state, uint8_t *base, size_t size,
			  uint64_t ops)
{
	double sec;
	double mops;
	uint64_t cpu0 = proc_cpu_ns();

	sec = bench_random_read(base, size, ops);
	mops = (double)ops / sec / 1e6;
	printf("  稳态随机读: %" PRIu64 " ops in %.3f s = %.2f Mops/s\n",
	       ops, sec, mops);
	csv_row(state, "read_mops", mops, "Mops/s");

	sec = bench_random_write(base, size, ops);
	mops = (double)ops / sec / 1e6;
	printf("  稳态随机写: %" PRIu64 " ops in %.3f s = %.2f Mops/s\n",
	       ops, sec, mops);
	printf("  稳态读写 CPU: %.1f ms\n",
	       (double)(proc_cpu_ns() - cpu0) / 1e6);
	csv_row(state, "write_mops", mops, "Mops/s");
}

static void evict_zram(int pidfd, uint8_t *base, size_t size, const char *tag)
{
	printf("  换出到 zram (%s)\n", tag);
	pageout(pidfd, base, size, tag);
	expect_swapped(tag, size);
}

/* pageout 之后必须重新标记 idle，再 writeback；否则新页不在 idle 集合里 */
static void evict_l2(const struct run_cfg *cfg, const char *tag)
{
	uint64_t bd_before[3] = { 0 };
	uint64_t bd_after[3] = { 0 };
	char path[160];
	uint64_t t0;
	double sec;
	int n;

	snprintf(path, sizeof(path), "/sys/block/%s/bd_stat", cfg->dev);
	read_u64_vec(path, bd_before, 3);

	t0 = now_ns();
	if (write_attr(cfg->dev, "idle", "all") < 0)
		dief("写 idle=all 失败（需要 root，且 zram 已初始化）");
	if (write_attr(cfg->dev, "writeback", "idle") < 0)
		dief("写 writeback=idle 失败（需要 CONFIG_ZRAM_WRITEBACK）");
	sec = (double)(now_ns() - t0) / 1e9;
	printf("  writeback(idle): %.3f s (%.0f MB/s)\n", sec,
	       (double)cfg->size / MIB / sec);
	csv_row(tag, "writeback_s", sec, "s");

	n = read_u64_vec(path, bd_after, 3);
	if (n >= 3) {
		uint64_t pages = bd_after[0] - bd_before[0];

		printf("  bd_count +%" PRIu64 " (=%.1f MB), bd_writes +%" PRIu64 "\n",
		       pages, (double)pages * PAGE_SZ / MIB,
		       bd_after[2] - bd_before[2]);
		if (bd_after[2] == bd_before[2])
			warnf("bd_writes 没有增长：writeback 没有真的发生（页不是 idle？）");
	}
	zram_stats(cfg->dev, tag);
}

static void refill(const struct run_cfg *cfg, uint8_t *base, size_t npages,
		   const uint32_t *perm)
{
	uint64_t t0 = now_ns();
	double sec;

	if (cfg->pattern_text)
		fill_text(base, npages, perm);
	else
		fill_random(base, cfg->size);
	sec = (double)(now_ns() - t0) / 1e9;
	printf("  refill: %.1f MB in %.3f s\n", (double)cfg->size / MIB, sec);
}

/*
 * 在“已淘汰”状态下先测首次读，再（可选）重新淘汰后测首次写。
 * cold-mode=both 时，读测完页已全部回到 DRAM，必须再 pageout 一次，
 * 否则所谓“首次写”量的其实是驻留页，结论会失真。
 */
static void measure_state(int pidfd, const char *tag, uint8_t *base,
			  const struct run_cfg *cfg, uint32_t *perm,
			  uint64_t *lat, size_t npages, int is_l2,
			  uint64_t ref_full, uint64_t ref_skip)
{
	int do_read = (cfg->cold_mode == COLD_READ || cfg->cold_mode == COLD_BOTH);
	int do_write = (cfg->cold_mode == COLD_WRITE || cfg->cold_mode == COLD_BOTH);
	char name[48];

	if (do_read) {
		fault_pass(base, npages, perm, lat);
		snprintf(name, sizeof(name), "%s-read", tag);
		report_fault(name, lat, npages, cfg->size);
		report_checksum(name, ref_full, checksum_region(base, cfg->size));
	}

	if (do_write) {
		if (do_read) {
			if (is_l2) {
				evict_zram(pidfd, base, cfg->size, "C-re-evict");
				evict_l2(cfg, "C-re-wb");
			} else {
				evict_zram(pidfd, base, cfg->size, "B-re-evict");
			}
		}
		cold_write_pass(base, npages, perm, lat);
		snprintf(name, sizeof(name), "%s-write", tag);
		report_fault(name, lat, npages, cfg->size);
		report_checksum(name, ref_skip,
				checksum_skip_head(base, npages));
	}

	steady_passes(tag, base, cfg->size, cfg->ops);
}

static void run_state_a(uint8_t *base, const struct run_cfg *cfg,
			uint32_t *perm, uint64_t *lat, size_t npages)
{
	uint64_t t0 = now_ns();
	double fill_s;

	printf("=== A. 普通内存：全部驻留 DRAM ===\n");
	if (cfg->pattern_text)
		fill_text(base, cfg->size / PAGE_SZ, perm);
	else
		fill_random(base, cfg->size);
	fill_s = (double)(now_ns() - t0) / 1e9;
	printf("  fill(%s): %.1f MB in %.3f s (%.0f MB/s)\n",
	       cfg->pattern_text ? "text" : "random",
	       (double)cfg->size / MIB, fill_s,
	       (double)cfg->size / MIB / fill_s);
	csv_row("A", "fill_mbs", (double)cfg->size / MIB / fill_s, "MB/s");
	printf("  [check] VmRSS=%.1f MB VmSwap=%.1f MB\n",
	       (double)status_rss_kb() / 1024.0,
	       (double)status_swap_kb() / 1024.0);

	/*
	 * A 的冷读对照：fill 之后立即逐页首次访问。这里不能先做全量
	 * checksum——那会把整段顺序读一遍，A-read 就变成“再次访问”，
	 * 与 B/C 的“淘汰后首次访问”口径不对称。A 没有淘汰路径，
	 * 数据完整性由 B（zram 读回）和 C（brd 读回）覆盖。
	 */
	shuffle_pages(perm, npages);
	fault_pass(base, npages, perm, lat);
	report_fault("A-read", lat, npages, cfg->size);

	/* A 的首次写基线：页驻留，量的是普通 store，不含换入 */
	if (cfg->cold_mode != COLD_READ) {
		uint64_t ref_skip = checksum_skip_head(base, npages);

		cold_write_pass(base, npages, perm, lat);
		report_fault("A-write", lat, npages, cfg->size);
		report_checksum("A-write", ref_skip,
				checksum_skip_head(base, npages));
	}

	steady_passes("A", base, cfg->size, cfg->ops);
}

static void run_state_b(int pidfd, uint8_t *base, const struct run_cfg *cfg,
			uint32_t *perm, uint64_t *lat, size_t npages)
{
	uint64_t ref_full;
	uint64_t ref_skip = 0;

	printf("=== B. zram：process_madvise(MADV_PAGEOUT) 换出到压缩内存 ===\n");
	shuffle_pages(perm, npages);
	refill(cfg, base, npages, perm);
	ref_full = checksum_region(base, cfg->size);
	if (cfg->cold_mode != COLD_READ)
		ref_skip = checksum_skip_head(base, npages);

	evict_zram(pidfd, base, cfg->size, "B-evict");
	zram_stats(cfg->dev, "B");

	measure_state(pidfd, "B", base, cfg, perm, lat, npages, 0,
		      ref_full, ref_skip);

	printf("  [check] 换回后 VmRSS=%.1f MB VmSwap=%.1f MB\n",
	       (double)status_rss_kb() / 1024.0,
	       (double)status_swap_kb() / 1024.0);
}

static void run_state_c(int pidfd, uint8_t *base, const struct run_cfg *cfg,
			uint32_t *perm, uint64_t *lat, size_t npages)
{
	char path[160];
	char backing[256] = { 0 };
	uint64_t ref_full;
	uint64_t ref_skip = 0;
	uint64_t bd[3] = { 0 };
	int n;

	printf("=== C. 后备内存盘：zram 内容 writeback 到 backing_dev ===\n");
	snprintf(path, sizeof(path), "/sys/block/%s/backing_dev", cfg->dev);
	if (!read_str_file(path, backing, sizeof(backing)))
		dief("读不到 %s", path);
	if (strcmp(backing, "none") == 0)
		dief("backing_dev 未设置；先跑 setup-zram-l2.sh（或用 --no-l2 跳过 C）");
	printf("  backing_dev=%s\n", backing);

	shuffle_pages(perm, npages);
	refill(cfg, base, npages, perm);
	ref_full = checksum_region(base, cfg->size);
	if (cfg->cold_mode != COLD_READ)
		ref_skip = checksum_skip_head(base, npages);

	evict_zram(pidfd, base, cfg->size, "C-evict");
	zram_stats(cfg->dev, "C-before-wb");
	evict_l2(cfg, "C-wb");

	{
		unsigned long long sw = status_swap_kb();

		printf("  [check] writeback 后 VmSwap=%.1f MB（页仍在 swap，L1 已变成 L2）\n",
		       (double)sw / 1024.0);
		if (sw < (unsigned long long)(cfg->size / 1024) * 85 / 100)
			warnf("writeback 后 VmSwap 掉到 %.0f MB，页可能被换回 DRAM",
			      (double)sw / 1024.0);
	}

	measure_state(pidfd, "C", base, cfg, perm, lat, npages, 1,
		      ref_full, ref_skip);

	snprintf(path, sizeof(path), "/sys/block/%s/bd_stat", cfg->dev);
	n = read_u64_vec(path, bd, 3);
	if (n >= 3)
		printf("  bd_reads 累计=%" PRIu64 " (4K 单位)\n", bd[1]);

	printf("  [check] 换回后 VmRSS=%.1f MB VmSwap=%.1f MB\n",
	       (double)status_rss_kb() / 1024.0,
	       (double)status_swap_kb() / 1024.0);
}

/* ---------- 命令行 ---------- */

static void usage(const char *prog)
{
	printf("用法: %s [选项]\n"
	       "  -s, --size-mb N   工作集大小，默认 100（MB）\n"
	       "  -o, --ops N       每次稳态随机读/写的操作数，默认 size/8\n"
	       "  -d, --dev NAME    zram 设备名，默认 zram0\n"
	       "  -z, --seed N      RNG 种子，默认 0x243f6a8885a308d3\n"
	       "  -p, --pattern P   random（默认）或 text（可压缩，观察 zram 压缩率）\n"
	       "  -m, --cold-mode M read(默认)|write|both：首次访问只测读 / 只测写 / 都测\n"
	       "  -L, --no-l2       只测 A/B，不做 backing_dev writeback\n"
	       "  -c, --csv FILE    把每个指标再写一份 CSV\n"
	       "  -h, --help\n"
	       "\n先跑 setup-zram-l2.sh 建好 zram + 内存盘，再 sudo 运行本程序。\n",
	       prog);
}

int main(int argc, char **argv)
{
	struct run_cfg cfg = {
		.size = 100ull * MIB,
		.ops = 0,
		.seed = 0x243f6a8885a308d3ull,
		.dev = "zram0",
		.pattern_text = 0,
		.cold_mode = COLD_READ,
	};
	const char *csv_path = NULL;
	int no_l2 = 0;
	int opt;
	uint32_t *perm = NULL;
	uint64_t *lat = NULL;
	uint8_t *base = NULL;
	int pidfd;
	size_t npages;

	static const struct option long_opts[] = {
		{ "size-mb", required_argument, NULL, 's' },
		{ "ops", required_argument, NULL, 'o' },
		{ "dev", required_argument, NULL, 'd' },
		{ "seed", required_argument, NULL, 'z' },
		{ "pattern", required_argument, NULL, 'p' },
		{ "cold-mode", required_argument, NULL, 'm' },
		{ "no-l2", no_argument, NULL, 'L' },
		{ "csv", required_argument, NULL, 'c' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	while ((opt = getopt_long(argc, argv, "s:o:d:z:p:m:Lc:h", long_opts,
				  NULL)) != -1) {
		switch (opt) {
		case 's':
			cfg.size = (size_t)strtoull(optarg, NULL, 0) * MIB;
			break;
		case 'o':
			cfg.ops = strtoull(optarg, NULL, 0);
			break;
		case 'd':
			cfg.dev = optarg;
			break;
		case 'z':
			cfg.seed = strtoull(optarg, NULL, 0);
			break;
		case 'p':
			if (strcmp(optarg, "text") == 0)
				cfg.pattern_text = 1;
			else if (strcmp(optarg, "random") == 0)
				cfg.pattern_text = 0;
			else
				dief("未知 pattern: %s", optarg);
			break;
		case 'm':
			if (strcmp(optarg, "read") == 0)
				cfg.cold_mode = COLD_READ;
			else if (strcmp(optarg, "write") == 0)
				cfg.cold_mode = COLD_WRITE;
			else if (strcmp(optarg, "both") == 0)
				cfg.cold_mode = COLD_BOTH;
			else
				dief("未知 cold-mode: %s", optarg);
			break;
		case 'L':
			no_l2 = 1;
			break;
		case 'c':
			csv_path = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (cfg.size == 0)
		dief("size 不能为 0");
	if (cfg.size % PAGE_SZ)
		cfg.size = (cfg.size + PAGE_SZ - 1) & ~(size_t)(PAGE_SZ - 1);
	npages = cfg.size / PAGE_SZ;
	if (npages > UINT32_MAX)
		dief("size 太大，页数超过 u32");
	if (cfg.ops == 0)
		cfg.ops = cfg.size / 8;
	if (cfg.ops > 1000000000ull)
		dief("ops 太大: %" PRIu64, cfg.ops);
	rng_state = (uint64_t)cfg.seed * 6364136223846793005ull +
		    1442695040888963407ull;

	if (csv_path) {
		g_csv = fopen(csv_path, "w");
		if (!g_csv)
			dief("打开 CSV 失败: %s", csv_path);
		fprintf(g_csv, "state,metric,value,unit\n");
	}

	if (geteuid() != 0)
		warnf("非 root：process_madvise 需要 CAP_SYS_NICE（mm/madvise.c:1868），"
		      "C 组也写不了 writeback");

	/* 宿主/设备前置检查 */
	{
		char path[160];
		char backing[256] = { 0 };
		unsigned long long disksize;
		unsigned long long page_cluster;
		unsigned long long baseline_swap;

		snprintf(path, sizeof(path), "/sys/block/%s/disksize", cfg.dev);
		disksize = read_u64_file(path);
		if (disksize == 0)
			dief("%s 未初始化（disksize=0）；先 sudo ./setup-zram-l2.sh",
			     path);
		if (disksize < cfg.size)
			warnf("zram disksize %.0f MB < 工作集 %.0f MB，换出会被截断",
			      (double)disksize / MIB, (double)cfg.size / MIB);
		printf("设备: %s, disksize=%.1f MB\n", cfg.dev,
		       (double)disksize / MIB);

		snprintf(path, sizeof(path), "/sys/block/%s/backing_dev", cfg.dev);
		if (read_str_file(path, backing, sizeof(backing)))
			printf("backing_dev: %s\n", backing);

		page_cluster = read_u64_file("/proc/sys/vm/page-cluster");
		if (page_cluster != 0)
			warnf("/proc/sys/vm/page-cluster=%llu，swap readahead 会一次"
			      "换入多页，单页延迟被放大；建议 "
			      "echo 0 > /proc/sys/vm/page-cluster",
			      page_cluster);

		baseline_swap = status_swap_kb();
		if (baseline_swap > 8ull * 1024)
			warnf("启动时本进程已有 VmSwap=%.1f MB，swap 上有别人的页或残留",
			      (double)baseline_swap / 1024.0);
	}

	base = mmap(NULL, cfg.size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		dief("mmap %.0f MB 失败: %s", (double)cfg.size / MIB,
		     strerror(errno));
	/* 固定 4K 粒度：否则 THP 会把 100MB 折成 50 个 2MB 页，换入统计失真 */
	if (madvise(base, cfg.size, MADV_NOHUGEPAGE) < 0)
		warnf("MADV_NOHUGEPAGE 失败: %s（THP 可能生效）", strerror(errno));

	perm = malloc(npages * sizeof(*perm));
	lat = malloc(npages * sizeof(*lat));
	if (!perm || !lat)
		dief("分配 perm/lat 失败");
	for (size_t i = 0; i < npages; i++)
		perm[i] = (uint32_t)i;

	pidfd = (int)syscall(SYS_pidfd_open, (pid_t)getpid(), 0);
	if (pidfd < 0) {
		warnf("pidfd_open(self) 失败(%s)，状态 B 会回退 madvise",
		      strerror(errno));
		pidfd = NO_PIDFD;
	}

	printf("工作集 %.1f MB, %zu 页(4K), 每轮 ops=%" PRIu64 ", pattern=%s\n\n",
	       (double)cfg.size / MIB, npages, cfg.ops,
	       cfg.pattern_text ? "text" : "random");

	shuffle_pages(perm, npages);
	run_state_a(base, &cfg, perm, lat, npages);

	run_state_b(pidfd, base, &cfg, perm, lat, npages);

	if (!no_l2)
		run_state_c(pidfd, base, &cfg, perm, lat, npages);

	munmap(base, cfg.size);
	if (pidfd != NO_PIDFD)
		close(pidfd);
	free(perm);
	free(lat);
	if (g_csv)
		fclose(g_csv);
	printf("\n完成（sink=%" PRIu64 "）。\n", (uint64_t)g_sink);
	return 0;
}
