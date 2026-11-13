---
title: zone 的作用与内存回收观测
aliases:
  - zone
  - 内存回收观测
  - reclaim-observability
tags:
  - linux/kernel
  - linux/mm
  - linux/reclaim
source: /Users/cheney/work/code/ub/kernel @ 8003dfcde32f (openEuler OLK-6.6 / kernel 6.6.0)
created: 2026-10-31
updated: 2026-10-31
status: 已验证（静态阅读，未编译未运行）
---

# zone 的作用与内存回收观测

承接 [[内存管理总览]] 的 §2（物理内存表示）、§4（分配器）、§9（回收）。回答两个问题：zone 到底解决什么问题；以及怎么判断“回收发生了、是谁触发的、是不是某个 zone 缺水”。

## 1. zone 是什么，为什么要有

zone 是 **NUMA 节点内部按“可寻址范围 / 可移动性”切的第二级分区**。为什么不能只有 node：同一个节点里的物理内存，对设备和内核并不等价。

| 时期 | 动因 | 产生的 zone |
| --- | --- | --- |
| 32 位时代 | ISA DMA 只能访问低 16 MB；内核线性映射区有限，高端内存无法常驻 | `ZONE_DMA`、`ZONE_HIGHMEM` |
| 64 位时代 | 部分设备只有 32 位 DMA 掩码 | `ZONE_DMA32` |
| 现代 | 内存热插拔、高阶分配需要“只放可搬迁页”的区域 | `ZONE_MOVABLE` |

`ZONE_MOVABLE` 的定位在内核注释里写得很清楚（`include/linux/mmzone.h:804-852`）：它主要为了提高内存下线成功率、并把不可搬迁分配挡在外面以增加 THP 供给；注释里列了 7 类例外（长期 pin 的页、memblock 分配、内存空洞、hwpoison、`PG_offline`、`ZERO_PAGE(0)`、memmap_on_memory）。

## 2. zone 承担的六项职责

| 职责 | 说明 | 代码 |
| --- | --- | --- |
| **分配边界** | gfp 里的 zone 位决定从哪类 zone 要页 | `gfp_zone()` `include/linux/gfp.h:155`，掩码 `GFP_ZONEMASK` `include/linux/gfp_types.h:80` |
| **水位管理** | 每个 zone 独立维护 min/low/high，是“要不要回收”的唯一判据 | `_watermark[]` `include/linux/mmzone.h:869`、`watermark_boost` `:870`、计算在 `__setup_per_zone_wmarks()` `mm/page_alloc.c:6321` |
| **回收单位** | zone 低水位触发唤醒 kswapd；回收按 zone 逐个扫 | `__zone_watermark_ok()` `mm/page_alloc.c:3196`、`zone_watermark_ok()` `:3274` |
| **锁粒度** | `zone->lock` 保护该 zone 的 buddy 空闲链表 | `spinlock_t lock` `include/linux/mmzone.h:995`、`free_area[]` `:984` |
| **统计口径** | `/proc/zoneinfo` 的一切数字都是按 zone 出的 | `mm/vmstat.c:1675` |
| **PCP 归属** | 每 CPU 页缓存是“每 zone 一份”，不是全局 | `per_cpu_pageset` `include/linux/mmzone.h:889` |

三个页数要分清（`include/linux/mmzone.h:910-957`）：`spanned_pages`（含空洞）⊇ `present_pages`（真实存在）⊇ `managed_pages`（buddy 管的）。**水位是按 `managed_pages` 算的**（`__setup_per_zone_wmarks()`，`mm/page_alloc.c:6321-6379`），这是它和 `MemTotal` 对不上的原因之一。

水位的实际算法（`mm/page_alloc.c:6323-6379`）：

```
pages_min   = min_free_kbytes >> (PAGE_SHIFT - 10)
lowmem_pages = Σ managed_pages（不含 highmem / MOVABLE / EXTMEM）
WMARK_MIN[zone] = pages_min × managed_pages[zone] / lowmem_pages
              （highmem / MOVABLE / EXTMEM 例外：clamp(managed/1024, SWAP_CLUSTER_MAX, 128)）
gap = max(WMARK_MIN[zone]/4, managed_pages × watermark_scale_factor / 10000)
WMARK_LOW  = MIN + gap
WMARK_HIGH = LOW + gap
WMARK_PROMO= HIGH + gap
```

默认 `watermark_scale_factor=10` 时，gap 约为 zone 可管理内存的 **0.1%**，所以 `high − low` 很小；这也是为什么内存曲线会在接近水位时频繁抖动。

另外 `lowmem_reserve[]`（`include/linux/mmzone.h:883`）是 zone 之间的保护线：低端 zone 给高端 zone 的分配预留空间，避免“低端 zone 被高端请求吃干净”。

## 3. 本树实际有哪几个 zone

`arch/x86/configs/openeuler_defconfig`：

- `CONFIG_ZONE_DMA=y`（`:1212`）、`CONFIG_ZONE_DMA32=y`（`:1213`）、`CONFIG_ZONE_DEVICE=y`（`:1214`）
- 无 `CONFIG_HIGHMEM`（x86_64 不需要）
- 无 `CONFIG_ZONE_EXTMEM`（源码里有 `ZONE_EXTMEM` 分支，`include/linux/mmzone.h:801-803`，但本配置未开启）
- `CONFIG_MEMORY_RELIABLE` 依赖 ARM64 且默认关（`mm/Kconfig:1451`），x86_64 上不适用

所以运行期看到的是 **DMA / DMA32 / Normal / Movable** 四个（`ZONE_DEVICE` 只用于设备内存，不参与常规分配）。

`ZONE_DMA32` 在 64 位机器上通常不大，但**它先于 Normal 被尝试**（zonelist 顺序），所以小内存机或大量 32 位 DMA 设备时，`/proc/zoneinfo` 里最先见底的是它。

## 4. 一次回收是怎么被触发的

```
分配请求 __alloc_pages()                    mm/page_alloc.c:4998
  └─ 快路径 get_page_from_freelist()        :3409
       └─ 逐 zone 试（zonelist + highest_zoneidx）
            └─ zone_watermark_fast()        :3281 → __zone_watermark_ok() :3196
                 水位不够 → 不用这个 zone，继续下一个
  └─ 慢路径 __alloc_pages_slowpath()        :4367
       ├─ wake_all_kswapds()                :4069 → wakeup_kswapd() mm/vmscan.c:7279
       │     → kswapd 醒来 → balance_pgdat() mm/vmscan.c:6864（按 zone 循环）
       │     → shrink_node() / shrink_zones()
       └─ 还是拿不到 → __alloc_pages_direct_reclaim()  :4030
             → try_to_free_pages()          mm/vmscan.c:6494
             → do_try_to_free_pages()       mm/vmscan.c:6276
             → shrink_zones()               mm/vmscan.c:6153
```

三个关键点：

1. **判断单位是 zone**：`zone_watermark_fast()` 用的是该 zone 自己的 `mark`（`mm/page_alloc.c:3501-3503`），不是全局内存。
2. **kswapd 是每 node 一个**（`pgdat->kswapd`，`include/linux/mmzone.h:1382`），但由**某个 zone** 的水位不足触发（`wakeup_kswapd(zone, ...)`，`mm/vmscan.c:7279`），并用 `highest_zoneidx` 限制它能扫到哪些 zone。
3. **直接回收是兜底**：只有唤醒 kswapd 并重试之后仍失败，才让分配者自己下场回收（`mm/page_alloc.c:4030`）。所以**看到 direct reclaim 计数增长，说明 kswapd 已经跟不上**。

另外有个降噪机制：zone 一旦掉到 high 水位以下就打 `ZONE_BELOW_HIGH` 标记，并降低该 zone 的 PCP 缓存上限、回收 PCP 页，避免“差一点就触发回收”（`mm/page_alloc.c:3492-3503`；PCP 回调唤醒在 `:3159-3164`）。

## 5. 怎么查“有没有触发回收”

### 5.1 第一层：`/proc/vmstat` 计数器

`/proc/vmstat` 由 `mm/vmstat.c:2144` 注册，计数器定义在 `include/linux/vm_event_item.h:32-51`，文本名在 `mm/vmstat.c:1280-1290` 一带。

| 计数器 | 含义 | 说明 |
| --- | --- | --- |
| `pgscan_kswapd` | kswapd 扫描的页数 | 后台回收强度 |
| `pgsteal_kswapd` | kswapd 实际回收到的页数 | 扫描/回收比值反映 LRU 质量 |
| `pgscan_direct` | 直接回收扫描页数 | **出现就说明分配者被迫自己回收** |
| `pgsteal_direct` | 直接回收拿到的页数 | 同上 |
| `pgscan_direct_throttle` | 直接回收被限流次数 | `mm/vmscan.c:6469`，说明多任务在抢回收 |
| `allocstall_dma` / `_dma32` / `_normal` / `_movable` | **每个 zone** 上进入直接回收的次数 | 按 zone 展开，`include/linux/vm_event_item.h:29-30` |
| `pgscan_anon` / `pgscan_file` | 按类型统计 | 判断回收是在扫匿名还是文件页 |
| `pgsteal_anon` / `pgsteal_file` | 按类型统计 | 匿名页被回收说明已经在写 swap |
| `compact_stall` | 为高阶分配触发整理 | `mm/page_alloc.c:3786` |
| `oom_kill` | OOM 击杀次数 | 最坏情况 |

`allocstall_*` 是最直接的回答：**它按 zone 记账，且只在非 cgroup 回收路径累加**（`mm/vmscan.c:6287`）。

### 5.2 第二层：区分 kswapd 与直接回收

```
pgscan_kswapd 增长，pgscan_direct 不动  → 后台回收够用，健康
pgscan_direct 增长，pgscan_kswapd 也增长 → kswapd 跟不上，分配路径开始变慢
allocstall_normal 增长，pgscan_direct 不增长 → 该 zone 反复进入直接回收但扫不到东西（可能是不可回收页占住）
```

### 5.3 第三层：定位到 zone

`/proc/zoneinfo` 的输出由 `zoneinfo_show_print()`（`mm/vmstat.c:1675`）生成，每个 zone 一段：

```
Node 0, zone   Normal
  pages free     12345
        boost    0
        min      1024
        low      1280
        high     1536
        spanned  262144
        present  262144
        managed  250000
        cma      0
        protection: (0, 0, 0, 0)
```

字段对应 `mm/vmstat.c:1691-1716`。判读方式：**`pages free` 贴近或低于 `low`，就是该 zone 在推动回收；低于 `min` 说明已经进入直接回收甚至失败**。`boost` 非 0 说明碎片整理抬高了水位（`watermark_boost`，`include/linux/mmzone.h:870`）。

### 5.4 第四层：抓现场

tracepoint 定义在 `include/trace/events/vmscan.h`，调用点在 `mm/vmscan.c`：

| 事件 | 定义 | 触发点 | 用途 |
| --- | --- | --- | --- |
| `mm_vmscan_kswapd_wake` | `:66` | `mm/vmscan.c:7259` | kswapd 被唤醒，带 `highest_zoneidx` 和 order |
| `mm_vmscan_kswapd_sleep` | `:49` | `mm/vmscan.c:7155` | kswapd 认为平衡了，回去睡 |
| `mm_vmscan_direct_reclaim_begin` | `:136` | `mm/vmscan.c:6541` | 某次分配进入直接回收 |
| `mm_vmscan_direct_reclaim_end` | `:176` | `mm/vmscan.c:6545` | 本次回收回来多少页 |
| `mm_vmscan_lru_isolate` | `:281` | `mm/vmscan.c:1766`、`:4494` | 从 LRU 上摘了多少页、哪条链 |

cgroup 回收另有独立事件（`mm_vmscan_memcg_reclaim_begin`，`include/trace/events/vmscan.h:144`），排查容器时不要只看全局事件。

```bash
# 看谁在回收、回收多少
perf record -e 'vmscan:*' -a -- sleep 10
perf script | head -50

# 或者用 tracefs
echo 1 > /sys/kernel/debug/tracing/events/vmscan/enable
cat /sys/kernel/debug/tracing/trace_pipe
```

### 5.5 第五层：压力信号 PSI

`/proc/pressure/memory` 由 `psi_proc_init()` 注册（`kernel/sched/psi.c:2004`，memory 那条在 `:2009`），资源类型 `PSI_MEM` 定义在 `include/linux/psi_types.h:44`。

```
some avg10=0.00 avg60=0.12 avg300=0.03 total=123456
full avg10=0.00 avg60=0.05 avg300=0.01 total=65432
```

`some` 表示至少有一个任务因内存停顿，`full` 表示所有任务都停。直接回收路径会显式标记这段停顿（`psi_memstall_enter()` `mm/page_alloc.c:4039`，`psi_memstall_leave()` `:4064`），**所以 PSI 的 memory 压力几乎可以等同于“回收造成的停顿”**。

### 5.6 memcg 视角：cgroup 的回收数在哪看

**先给结论**：cgroup 自己的回收量要看**该 cgroup 的 `memory.stat`**；全局 `/proc/vmstat` 能看到一部分，但**最顺手的 `pgscan_kswapd` / `pgscan_direct` 恰恰看不到 cgroup 触发的回收**。

计数点分两类，差别就在 `!cgroup_reclaim(sc)` 这个门控上：

| 计数器 | cgroup 触发的回收是否计入 | 代码 |
| --- | --- | --- |
| `pgscan_kswapd` / `pgscan_direct` | **不计入** | `mm/vmscan.c:1981-1982`（经典 LRU）、`:4487-4490`（MGLRU） |
| `pgsteal_kswapd` / `pgsteal_direct` | **不计入** | `mm/vmscan.c:1998-1999`、`:4668-4669` |
| `pgrefill` | **不计入** | `mm/vmscan.c:2088-2089` |
| `allocstall_*` | **不计入** | `mm/vmscan.c:6286-6287` |
| `pgscan_anon` / `pgscan_file` | **计入**（无条件累加） | `mm/vmscan.c:1984`、`:4493` |
| `pgsteal_anon` / `pgsteal_file` | **计入**（无条件累加） | `mm/vmscan.c:2001`、`:4671` |
| memcg 的 `pgscan` / `pgsteal` | **总是计入** | `mm/vmscan.c:1983`、`:2000`、`:4491`、`:4670` |

原因：`cgroup_reclaim(sc)` 的实现就是 `return sc->target_mem_cgroup;`（`mm/vmscan.c:208-210`）。只有**针对某个 memcg 的回收**（限额/接口触发，入口 `try_to_free_mem_cgroup_pages()` `mm/vmscan.c:6593`，在 `:6605` 设置 `.target_mem_cgroup`）才为真；kswapd 和分配者直接回收都是全局回收，`target_mem_cgroup` 为空，走 `count_vm_events` 那条全局分支。

**记到哪个 cgroup**：按 lruvec 归属，扫了谁的页就记谁头上——`__count_memcg_events(lruvec_memcg(lruvec), ...)` 是无条件执行的。而且全局回收本身也会遍历每个 memcg 的 lruvec：`shrink_node_memcgs()` 用 `mem_cgroup_iter(target_memcg, ...)` 迭代（`mm/vmscan.c:5902-5965`），而 `mem_cgroup_iter()` 在 `root` 为空时取 `root_mem_cgroup`（`mm/memcontrol.c:1172-1173`），也就是整棵树。

于是形成一个**不对称**结论：

| 回收类型 | 全局 `pgscan_kswapd/direct` | 该 cgroup 的 `memory.stat` pgscan |
| --- | --- | --- |
| 全局回收（kswapd / 分配者直接回收）扫到该 cgroup 的页 | **涨** | **涨** |
| 针对该 cgroup 的限额回收 | **不涨** | **涨** |

所以：**cgroup 的 `memory.stat` 能看到两种回收，全局计数器只能看到其中一种。** 想回答“我这个 cgroup 被回收了多少”，只能看它自己的 `memory.stat`。

**层级**：`memcg->vmstats->events[]` 在注释里写明是 “Aggregated (CPU and subtree)”（`mm/memcontrol.c:784-791`），`memcg_events()` 读的就是它（`:970-977`），rstat 向上传播（`__cgroup_rstat_updated`，`:651-670`）。**所以父 cgroup 的数已经包含子 cgroup。**

**`memory.stat` 里具体有哪几行**（`memory_stat_format()`，`mm/memcontrol.c:1700` 起）：

- `pgscan` / `pgsteal`：分别是 `KSWAPD + DIRECT + KHUGEPAGED` 三项之和（`:1674-1682`）
- 以及白名单里每个事件各占一行（`:1684-1692`），白名单在 `:716-742`，包含 `pgscan_kswapd`、`pgscan_direct`、`pgscan_khugepaged`、`pgsteal_*`、`pgrefill`、`pgactivate`、`pgdeactivate` 等

注意白名单里**没有** `PGSCAN_ANON/PGSCAN_FILE`，所以 **cgroup 侧看不到 anon/file 拆分**，只能看合计与 kswapd/direct 拆分。

**数字新鲜度**：读 `memory.stat` 会先 `mem_cgroup_flush_stats()`（`mm/memcontrol.c:1659`），但只有阈值超过 online CPU 数才真刷（`:693-696`）；另有每 `FLUSH_TIME = 2×HZ` 的后台 flusher（`:627`、`:706-714`）。所以数字最多滞后约 2 秒级别的批量窗口，不适合做精确到次的判定。

```bash
# 容器/进程所在 cgroup 的回收量
cat /sys/fs/cgroup/<path>/memory.stat | grep -E '^(pgscan|pgsteal)'

# 是不是 memcg 限额顶到导致的
cat /sys/fs/cgroup/<path>/memory.current /sys/fs/cgroup/<path>/memory.max
cat /sys/fs/cgroup/<path>/memory.events        # high / max 触发次数

# cgroup 级内存压力（PSI）
cat /sys/fs/cgroup/<path>/memory.pressure
```

### 5.7 最后：内核日志

分配彻底失败会走 `warn_alloc()`（`mm/page_alloc.c:3616`）打 `page allocation failure`，并附带当时的内存状态；再往后就是 OOM。**有这条日志说明已经掉到最后一级了。**

## 6. 判定表

| 现象 | 结论 |
| --- | --- |
| `pgscan_kswapd` 缓慢增长，PSI 接近 0 | 正常后台回收，无需处理 |
| `pgscan_direct` 增长，PSI memory `some` 上升 | 内存压力进入分配路径，延迟会抖动 |
| `pgscan_direct_throttle` 增长 | 多个任务在抢直接回收，已被限流 |
| `allocstall_<某个 zone>` 独涨 | 该 zone 缺水（常见于 DMA32 或 Movable） |
| `pgsteal_file` 远大于 `pgsteal_anon` | 在回收 page cache，应用缓存被压 |
| `pgsteal_anon` 增长 + `pswpin/pswpout` 增长 | 已经开始换出，延迟会明显变差 |
| `compact_stall` 增长但回收计数不动 | 问题在碎片，不在总量 |
| `oom_kill` 增长 | 已经无解，只能加内存或调限 |

## 7. 一条命令序列

```bash
# 1) 有没有回收，谁在回收
grep -E '^(pgscan|pgsteal|allocstall|compact_stall|oom_kill)' /proc/vmstat

# 2) 哪个 zone 缺水（free 对比 low/min）
grep -E 'Node|pages free|min|low|high|boost|managed' /proc/zoneinfo

# 3) 有多大停顿
cat /proc/pressure/memory

# 4) 容器内
cat /sys/fs/cgroup/memory.stat | grep -E '^(pgscan|pgsteal)'

# 5) 抓现场
perf record -e 'vmscan:*' -a -- sleep 10 && perf script | head
```

## 8. 未验证项

- 本文所有结论来自静态阅读，没有在真实负载下验证过计数器之间的比例关系。
- `ZONE_EXTMEM` 在本树的语义（`include/linux/mmzone.h:801-803` 只有分支，未见实现细节）未追。
- 不同内核版本 counter 名称有差异（尤其 `pgrefill` 已逐渐废弃），跨版本对比需重新确认。

---

*基线：`8003dfcde32f` / kernel 6.6.0 / openEuler OLK-6.6。承接 [[内存管理总览]]。*
