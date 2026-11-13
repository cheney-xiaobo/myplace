---
title: zram 框架介绍
aliases:
  - zram 原理
  - 压缩内存块设备
  - zsmalloc
tags:
  - linux-kernel
  - memory
  - zram
  - swap
  - compression
source: /Users/cheney/work/code/ub/kernel
created: 2026-11-12
updated: 2026-11-12
status: verified-by-reading
---

# zram 框架介绍

代码引用基于 `/Users/cheney/work/code/ub/kernel`（6.6）的 `drivers/block/zram/` 与 `mm/zsmalloc.c`。**代码能证实的部分给出文件:行号；性能判断与调优建议属经验，未在本机实测。**

## 0. 三句话结论

1. **zram 是一个"压缩内存块设备"**：对外是块设备（常拿来做 swap），对内把每个页压缩后塞进内存，用 zsmalloc 而不是 kmalloc 存放，换来接近 2~3 倍的"扩容"效果。
2. **它快在三个针对性优化**：整页同字节不存数据、不可压缩页原样存放、压缩流按 CPU 各备一份避免锁竞争。
3. **复杂度集中在两处**：元数据被压进一个 `unsigned long` 的位域里，以及压缩后大小不定逼出了一个专门的内存分配器 zsmalloc。

## 1. zram 是什么

zram 把一块内存模拟成块设备：写进来的页先压缩再存内存，读出去时再解压。因为占的是内存不是磁盘，它没有机械/闪存延迟，代价是压缩解压的 CPU 开销。

典型用法是**当 swap 设备**：内存吃紧时，把不常访问的页换出到 zram，相当于用 CPU 换内存容量。也常用于嵌入式、手机（Android 的 swap 默认就走 zram）、容器内存超卖。

内核里的角色定位（`drivers/block/zram/zram_drv.c:2152-2158`）：

```c
static const struct block_device_operations zram_devops = {
	.open = zram_open,
	.submit_bio = zram_submit_bio,
	.swap_slot_free_notify = zram_slot_free_notify,
	.owner = THIS_MODULE
};
```

一个 `.submit_bio` 收所有 I/O，一个 `.swap_slot_free_notify` 响应 swap 槽位释放——**这两个回调基本就是 zram 的全部对外接口**。

## 2. 架构图

```
                        应用 / swap 子系统
                               │ bio
                               ▼
                    zram_submit_bio()        ← 块层入口，:1965
                               │
              ┌────────────────┼────────────────┐
              ▼                ▼                ▼
        读 zram_bio_read   写 zram_bio_write   discard
              │                │
              ▼                ▼
      zram_read_page     zram_write_page
      (:1379)            (:1434)
              │                │
   ┌──────────┤         ┌──────┴──────┬──────────────┐
   │          │         ▼             ▼              ▼
   │          │    page_same     zcomp 压缩      不可压缩页
   │          │    _filled       (percpu)        原样存(HUGE)
   │          │    (:196)        zcomp.c           │
   │          │         │            │             │
   ▼          ▼         ▼            ▼             ▼
 SAME页    WB页      只存element   crypto API     zsmalloc
 填充      读后备     不占内存     lzo/lz4/zstd    存放
 element   设备
                        │
                        ▼
              table[index] = {handle, flags}
                        │
                        ▼
              zsmalloc (zs_pool)
              zspage / size class / fullness
                        │
                        ▼
                     物理页（可跨页）
```

三层各司其职：

- **zram_drv**：块设备外壳 + 元数据（table）+ 读写照度，决定"这一页怎么处理"。
- **zcomp**：压缩后端，封装 crypto API，管理 percpu 压缩流。
- **zsmalloc**：真正的内存分配器，负责把不定长的压缩对象高效塞进物理页。

`writeback` 是可选的第四条路：把冷页写到真实块设备，腾出内存（下面单独讲）。

## 3. 核心数据结构

### 3.1 struct zram（设备级）

`drivers/block/zram/zram_drv.h:106-142`，关键成员：

| 成员 | 作用 |
| --- | --- |
| `table` | 每页一项的元数据数组 |
| `mem_pool` | zsmalloc 的 zs_pool |
| `comps[]` | 压缩算法（主 + 可选多个备，用于 recompress） |
| `disk` | gendisk，块层外壳 |
| `disksize` | 逻辑容量（未压缩口径，字节） |
| `limit_pages` | 内存上限（物理页数，mem_limit 设置） |
| `stats` | 统计计数器 |
| `backing_dev` / `bitmap` | writeback 的后备设备与位图 |

注意 `disksize` 和 `limit_pages` 是**两个口径**：前者是"你能往这台设备写多少未压缩数据"，后者是"这台设备最多占多少物理内存"。压缩率高的时候 disksize 可以远大于 limit_pages。

### 3.2 zram_table_entry（每页元数据）

`drivers/block/zram/zram_drv.h:66-75`：

```c
struct zram_table_entry {
	union {
		unsigned long handle;   /* zsmalloc 返回的对象句柄 */
		unsigned long element;  /* same-page 的那个重复值 */
	};
	unsigned long flags;          /* 低位存压缩后大小，高位存标志 */
};
```

**这是 zram 最巧的一处设计**：压缩后大小和页标志挤在同一个 `flags` 里。低 `PAGE_SHIFT+1` 位存对象大小（不含头），高位是 `zram_pageflags`（`drivers/block/zram/zram_drv.h:41` 的 `ZRAM_FLAG_SHIFT`）。标志位有（`drivers/block/zram/zram_drv.h:47-61`）：

| 标志 | 含义 |
| --- | --- |
| `ZRAM_LOCK` | 槽位锁（用 `bit_spin_lock` 直接锁在 flags 的这位上，`:65-73`） |
| `ZRAM_SAME` | 整页同一字节，只存 element 不占内存 |
| `ZRAM_WB` | 页已写到后备设备 |
| `ZRAM_UNDER_WB` | 正在写回 |
| `ZRAM_HUGE` | 不可压缩，原样存了一整页 |
| `ZRAM_IDLE` | 自上次 idle 标记起没被访问 |
| `ZRAM_INCOMPRESSIBLE` | 所有算法都压不动 |
| `ZRAM_COMP_PRIORITY_BIT1/2` | 用的哪个优先级算法（recompress 用） |

一句话：**元数据省到极致**——每页只花两个 `unsigned long`，连锁都复用 flags 里的一位。

### 3.3 struct zcomp / zcomp_strm（压缩后端）

`drivers/block/zram/zcomp.h:10-23`：

```c
struct zcomp_strm {
	local_lock_t lock;
	void *buffer;              /* 压缩/解压缓冲 */
	struct crypto_comp *tfm;   /* crypto API 的算法句柄 */
};
struct zcomp {
	struct zcomp_strm __percpu *stream;  /* 每 CPU 一份 */
	const char *name;
	struct hlist_node node;
};
```

支持的算法在 `drivers/block/zram/zcomp.c:17-34`：`lzo`、`lzo-rle`、`lz4`、`lz4hc`、`842`、`zstd`（按 CONFIG 编译）。每个 stream 配 **2 页** buffer（`drivers/block/zram/zcomp.c:56`，`__get_free_pages(..., 1)`），因为压缩后可能比原页还大（`drivers/block/zram/zcomp.c:120-133` 有详细注释，特别提到 `842` 会 pad 到 8 的倍数）。

## 4. 读写路径

### 4.1 写：zram_write_page（`drivers/block/zram/zram_drv.c:1434-1557`）

按顺序做四件事：

1. **same-page 检测**（`:1446`）：`page_same_filled()`（`:196-216`）按 `unsigned long` 逐字比较整页是否同一个值。是则只记 `ZRAM_SAME` + element，**一个字节都不占**（`:1449-1451`）。
2. **压缩**（`:1456-1458`）：拿本 CPU 的 stream 压缩。`zcomp_stream_get()` 用 `local_lock` 关抢占（`drivers/block/zram/zcomp.c:106-110`），所以压缩期间不能被调度走。
3. **不可压缩兜底**（`:1468-1469`）：`comp_len >= huge_class_size` 就当作压不动，`comp_len = PAGE_SIZE`，后面原样存并打 `ZRAM_HUGE`（`:1540`）。
4. **分配 + 落盘到内存**（`:1483-1529`）：`zs_malloc` 拿 handle，`zs_map_object` 映射，`memcpy` 压缩数据。分配失败会走可睡眠的慢路径并记 `writestall`（`:1491`），最后检查 `limit_pages` 上限（`:1513-1517`）。

写路径的并发控制很关键：**压缩阶段持有 percpu stream（关抢占），但 `zs_malloc` 慢路径可能要睡眠**。所以代码在 `:1499` 处压缩重来一次（`goto compress_again`）——睡眠分配成功后必须重新压缩，因为慢路径中间 stream 被放掉了。

### 4.2 读：zram_read_from_zspool（`drivers/block/zram/zram_drv.c:1334-1377`）

按 table entry 分三种情况：

- **SAME 或空**：`zram_fill_page()` 用 element 把整页填回去（`:1349-1353`），不碰 zsmalloc。
- **不可压缩（size == PAGE_SIZE）**：`zs_map_object` 后直接 `memcpy`（`:1364-1368`）。
- **正常压缩页**：`zs_map_object` 后 `zcomp_decompress` 解压（`:1371`）。

如果页被标记了 `ZRAM_WB`（已写回后备设备），`zram_read_page` 改走 `read_from_bdev` 从后备设备读（`:1389-1397`），而且**先解锁再读**——因为读后备设备会睡眠，不能持有槽位锁。

### 4.3 释放：zram_free_page（`drivers/block/zram/zram_drv.c:1278`）

swap 槽位释放时由 `zram_slot_free_notify`（`:1986`）触发。按标志分别处理：WB 的 `free_block_bdev`，SAME 的只清标志，正常的 `zs_free`（`:1318`）。

注意 `miss_free` 统计（`:1995`）：`zram_slot_trylock` 拿不到锁就直接返回——**释放通知允许"错过"**，因为页正在被读写，后续会由正常路径释放。

## 5. 为什么用 zsmalloc 而不是 kmalloc

这是理解 zram 的另一半。压缩后对象大小不定，而且通常远小于一页。如果用 kmalloc：

- 按 slab 分配会取整到固定大小，内部碎片大；
- 一个对象很可能只用到页的一小部分，剩下的全浪费。

zsmalloc 的思路（`mm/zsmalloc.c`）：

| 概念 | 作用 |
| --- | --- |
| **zspage** | 一个或多个物理页组成，存放同一 size class 的多个对象（`:246-259`） |
| **size_class** | 按对象大小分桶，4K 页下有 255 个 class（`:144-145`），每个 class 记录 `objs_per_zspage`、`pages_per_zspage` |
| **fullness_group** | 按使用率分组（0/10/.../99/100%，`:157-165`），用于回收和紧凑 |
| **handle** | 对象句柄，间接寻址，允许对象跨页甚至迁移 |
| **mapmode** | 映射时区分 RW/RO/WO（`include/linux/zsmalloc.h:24-34`），跨页对象靠 `mapping_area` 的 `vm_buf` 拷贝 |

关键收益：**zspage 可以跨多个物理页**，所以一个 zspage 能塞进更多对象，内存利用率高。对象还能在物理页之间迁移做紧凑（`zs_compact`）。

代价：每次访问要 `zs_map_object`/`zs_unmap_object`，跨页对象还得拷贝。zram 用 mapmode 优化了这点——写用 `ZS_MM_WO`（不做 copy-in）、读用 `ZS_MM_RO`（不做 copy-out）。

## 6. 配置与使用

### 6.1 sysfs 接口

设备创建后（`zram_add`，`:2214`）容量是 0，要通过 sysfs 设置 `disksize` 才真正初始化（`disksize_store`，`:2045-2098`）。完整属性列表在 `:2179-2204`，常用的：

| 属性 | 读/写 | 作用 |
| --- | --- | --- |
| `disksize` | RW | 设容量并初始化（只能设一次） |
| `comp_algorithm` | RW | 选压缩算法 |
| `mem_limit` | WO | 物理内存上限 |
| `reset` | WO | 重置设备 |
| `mm_stat` | RO | 内存统计（下面详解） |
| `io_stat` | RO | I/O 统计 |
| `backing_dev` | RW | 设置 writeback 后备设备 |
| `writeback` | WO | 触发把 idle 页写回 |
| `recompress` | WO | 用更强算法重压已存页 |

### 6.2 典型用法

```bash
# 建一台 2G 的 zram，用 zstd
modprobe zram num_devices=1
echo zstd > /sys/block/zram0/comp_algorithm
echo 2G > /sys/block/zram0/disksize

# 当 swap 用
mkswap /dev/zram0
swapon /dev/zram0

# 看效果
cat /sys/block/zram0/mm_stat
```

### 6.3 mm_stat 的 9 个字段

`mm_stat_show`（`:1154-1188`）按序输出：

```
orig_data_size  compr_data_size  mem_used_total  mem_limit  mem_used_max
same_pages  pages_compacted  huge_pages  huge_pages_since
```

看压缩效果主要盯两个比值：`orig_data_size / mem_used_total`（实际扩容倍数），以及 `same_pages` 和 `huge_pages` 的占比——same 多是好事（白捡的），huge 多说明工作负载压不动，zram 帮不上忙。

## 7. 高级特性

### 7.1 writeback（内存不够时往真实磁盘倒）

配置 `backing_dev` 后，可以把 idle 页写到后备块设备，腾出 zram 占的内存。写过 writeback 的页打 `ZRAM_WB`，读的时候从后备设备读回（`:1389-1397`）。这让 zram 从"纯内存 swap"变成"内存 + 磁盘两级 swap"。

### 7.2 recompress（用更强的算法重压）

开启 `CONFIG_ZRAM_MULTI_COMP` 后可以配多个算法，先快后强：`recompress` 会把已存页用更高优先级的算法重新压一遍（`zram_recompress`，`:1596`），进一步省内存。table entry 里的 `COMP_PRIORITY` 位记录每页当前用的哪个算法。

### 7.3 idle / compact / mem_limit

- `idle`：把超过时间没访问的页标 `ZRAM_IDLE`，配合 writeback 决定倒谁。
- `compact`：触发 zsmalloc 紧凑，回收碎片。
- `mem_limit`：硬上限，超过就拒绝写（`:1513-1517` 返回 `-ENOMEM`），防止 zram 把内存吃光。

## 8. 性能与调优

**压缩算法选择**是最大杠杆：

| 算法 | 特点 |
| --- | --- |
| lzo / lzo-rle | 最快，压缩率一般，默认 |
| lz4 | 快，率略好于 lzo |
| lz4hc | 高压版 lz4，慢不少 |
| zstd | 压缩率最好，CPU 开销最大 |

经验（**未实测**）：内存压力敏感选 lzo/lz4，追求扩容倍数且 CPU 富余选 zstd。

其他调优点：

- **same-page 占比高**（大量零页/重复页）时 zram 几乎白赚，优先看 `same_pages`。
- **不可压缩页多**（huge_pages 高）说明工作负载不适合压缩，别硬上 zram。
- **压缩是 per-cpu 并行的**，CPU 核数直接影响吞吐；`writestall` 高说明内存分配跟不上，该降 disksize 或加 mem_limit。
- zram 自己也在内存里，**别把 disksize 设得比物理内存还激进**，否则压缩省下的又被它自己吃回去。

## 9. 未验证项

- 各算法的实际压缩率/吞吐对比未在本机实测，§8 的判断是经验结论。
- writeback 与 recompress 的完整触发路径只读了入口函数，未逐行核对全部边界条件。
- zsmalloc 的紧凑/迁移细节只读到数据结构层，未跟踪 `zs_compact` 的完整实现。

## 10. 相关

- [[内存管理总览]]
- [[内存回收观测与zone]]
- [[Page Cache 分析]]
- [[RDMA 使用方式与性能调优]]
