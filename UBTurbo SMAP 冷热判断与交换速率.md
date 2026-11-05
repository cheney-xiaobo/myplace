---
title: UBTurbo SMAP 冷热判断与交换速率
aliases:
  - SMAP 冷热策略
  - ubturbo 冷热判断
tags:
  - linux-kernel
  - memory
  - numa
  - ubturbo
  - smap
source: /Users/cheney/work/code/ubturbo
created: 2026-11-05
updated: 2026-11-05
status: verified-by-reading
---

# UBTurbo SMAP 冷热判断与交换速率

分析对象：`/Users/cheney/work/code/ubturbo` 的 SMAP 组件（`plugins/smap`），聚焦三个问题——冷热判断逻辑、冷热交换速率、扫描频率。所有行号基于当前工作副本实测。

## 0. 三句话结论

1. **冷热分数 = 累计热度 acc + 当前窗口 freq**，落进 0–255 的频次桶；acc 每 4 轮扫描减半，freq 每轮读一次就清零。
2. **交换量 = min(远端驻留页 × ratio/100, 本地可迁页, 远端热页数, maxMigrate)**；grouped 模式默认 `ratio=1`，即每个迁移周期换掉远端页的 1%。
3. **扫描周期默认 200 ms，迁移周期默认 2000 ms**，一个迁移周期内含约 10 轮扫描；两者都按 pid 可覆盖，且强制 `scan <= migrate`。
4. **硬件判热走的是内存侧 MMIO 采样单元（UPA SMAP），不是 CPU 的 PMU/MSR**：配置 `BA_CTRL` 指定一段地址区间、按 STS 粒度（4K 或 2M）统计访问次数，读回结果区后 `floor(sqrt())` 压缩进同一个 `freq` 字段；硬件自带的阈值与中断被屏蔽，全程靠"下发—睡眠—读回"同步轮询。

## 1. 冷热判断逻辑

### 1.1 两个计数器

| 计数器 | 位宽 | 存储位置 | 语义 | 清零方式 |
| --- | --- | --- | --- | --- |
| `freq` | 8 位 | `page->flags`（`SMAP_FREQ_VAL_SHIFT/WIDTH`） | 当前扫描窗口内的访问次数，饱和 255 | **每轮扫描读取后置哨兵**，下次读为 0 |
| `acc`（即 `prior`） | 5 位，最大 30 | `LAST_CPUPID` 的 bit[9..13] | 跨轮累计热度 | **每 4 轮扫描减半**，且只对"本轮未被访问"的页减半 |

- `freq` 的读清逻辑：`smap_page_freq_read_clear()`（`plugins/smap/src/kernel/scan/smap_page_flags.h:109-128`）原子读出计数值并把哨兵位置 1；哨兵已在时直接返回 0。
- `freq` 的加一逻辑：`smap_page_freq_inc()`（`plugins/smap/src/kernel/scan/smap_page_flags.h:133`），哨兵被置位时的首次访问把值重置为 1。
- `acc` 的衰减：`SMAP_ACC_DECAY_CYCLE` = 4（`plugins/smap/src/kernel/scan/smap_page_flags.h:202`），实现 `smap_page_acc_decay()` 做 `N -> N>>1`（`plugins/smap/src/kernel/scan/smap_page_flags.h:288-305`），调用点在冷页遍历分支（`plugins/smap/src/kernel/scan/accessed_bit.c:1503`）。
- 设计意图写得很明确：不减半的话，一次热过的页会永久占住本地槽位、永远无法降级（`plugins/smap/src/kernel/scan/smap_page_flags.h:279-286`）。

### 1.2 决策分数

用户态把两个计数器**直接相加**：

```c
int merged = (int)actc[i].prior + (int)actc[i].freq;
actc[i].freq = (actc_t)MIN(merged, FREQ_BUCKETS_SIZE - 1);
```

`MergeAccPriorToFreq()`（`plugins/smap/src/user/manage/manage_scan.c:139-146`），`FREQ_BUCKETS_SIZE` = 256（`plugins/smap/src/user/manage/manage.h:75`）。

**只有非 VM 进程做合成**（`plugins/smap/src/user/manage/manage_scan.c:169`）；VM 的 `prior` 来自前端采样数组，语义不同，不参与相加。

调用顺序是 `DistributeActcData()`（`plugins/smap/src/user/manage/manage_scan.c:157`）先合成，`CalcActcStats()`（`plugins/smap/src/user/manage/manage_scan.c:248`）后统计。统计时要还原窗口频次，会再减掉 `prior`（`plugins/smap/src/user/manage/manage_scan.c:70-79`）——这是一个隐式契约，顺序反了 `freqSum` 就会错。

### 1.3 记忆时长

扫描周期 200 ms，4 轮减半一次 → **半衰期 800 ms**；注释给出值跨度约 `2 × DECAY_CYCLE` = 8 轮 ≈ **1.6 s**（`plugins/smap/src/kernel/scan/smap_page_flags.h:206-215`）。也就是说这套策略只看最近 1–2 秒的访问历史，长周期热点和刚变热的页会被同等对待。

### 1.4 冷热两端的选取

| 侧 | 排序 | 位置 |
| --- | --- | --- |
| 本地页 | 按 `freq` **升序**（最冷在前） | `plugins/smap/src/user/strategy/grouped_strategy.c:527`，比较函数 `:81` |
| 远端页 | 按 `freq` **降序**（最热在前） | `plugins/smap/src/user/strategy/grouped_strategy.c:620`，比较函数 `:97` |

### 1.5 阈值与收益判据

成对交换是否值得（`IsSwapBeneficial()`，`plugins/smap/src/user/strategy/grouped_strategy.c:459-473`）：

```
slowThred = slowThred(默认2) × freqWt × 2        // → 默认 4
收益成立 ⇔
    remote.freq >= min.remote.freq(默认 0)
  ∧ remote.freq > local.freq + min.freq.gain(默认 0)
  ∧ ( (local.freq == 0 ∧ remote.freq > 0)
      ∨ local.freq + slowThred < remote.freq )
```

即**默认要求远端页频次比本地页高出至少 4**，否则不交换——这是抑制"频率相近、净收益为负"的抖动保护。

其他闸门：

- **白名单页不参与**：非 VM 的文件/共享页在扫描时打 `ACTC_WHITE_LIST_BIT`（`plugins/smap/src/kernel/scan/access_pid.c:526-527`）；VM 因 guest RAM 是 memfd/hugetlbfs 而豁免此规则。
- **远端共享文件页直接给最大频次**：`is_shared_file_page(page) && nid >= nr_local_numa` 时 `freq = U8_MAX`（`plugins/smap/src/kernel/scan/access_pid.c:529-531`），等于强制判定为最热、优先迁回本地。
- **VM 的频率标定**：`freqWt = l2FreqMax / l1FreqMax`，远端最大值取 99 分位（`smap.remote.freq.percentile`，默认 99，`plugins/smap/src/user/strategy/separate_strategy.c:119-146`）。
- **本地水位**：本地驻留页必须达到 `localReservePages × 95%`，否则本轮不交换（`IsGroupLocalSteady()`，`plugins/smap/src/user/strategy/grouped_strategy.c:212-224`，默认 95 见 `plugins/smap/src/user/strategy/strategy_config.c:56`）。

### 1.6 硬件判热（HIST）：寄存器级机制

前面 1.1–1.5 讲的是"软件 AF 扫描"。硬件判热走的是完全不同的通路：**不在 CPU 侧读 PMU/MSR，而是通过 MMIO 配置内存侧采样单元（华为 UPA SMAP），再从一块结果区把"窗口计数"读回来。**

#### 寄存器地图

寄存器定义在 `plugins/smap/src/kernel/scan/upa_smap.h:9-32`（偏移）与 `:34-57`（初值）。

| 偏移 | 寄存器 | 关键字段 |
| --- | --- | --- |
| 0x00 | `BA_CTRL` | `sts_base_addr[20:0]`（采样起始地址）、`page_size`、`sts_rd_en`、`sts_wr_en`、`sts_enable`、`ram_init_done` |
| 0x08 | `BA_THRESH` | ECC 相关 |
| 0x0C | `SMAP_TH1` | `smap_cnt_prd_th`（32 位周期计数阈值） |
| 0x10 | `SMAP_TH2` | `smap_page_view_th`（16 位） |
| 0x14 | `SMAP_TH3` | `smap_page_num_th`（16 位） |
| 0x18 | `SMAP_TH4` | `smap_total_cnt_th`（32 位） |
| 0x1C–0x2C | `SMAP_CNT0..4` | `cnt_prd` / `cnt_page` / `cnt_total` / `cnt_all` / `cnt_drop` |
| 0x30–0x38 | `CNT_CLR0..2` | 三类计数器清零位 |
| 0x3C | `SMAP_EN` | `cnt_prd_en` / `cnt_page_en` / `cnt_total_en` |
| 0x40 | `INT_MASK` | 三类中断屏蔽位，**初值 0x7 = 全屏蔽** |
| 0x44–0x4C | `CNT_INT_CLR0..2` | 中断清除 |
| 0x50 / 0x54 | `RPT0` / `RPT1` | 中断上报 / 中断状态 |

结果区不在同一块偏移里，而是独立的物理窗口：N6 机型在 `0x4000`、N7 在 `0x1000`（`plugins/smap/src/kernel/scan/ub_hist.c:25-26`、`:155-160`），条目数 N6 = 8192、N7 = 32768，每条 2 字节（`plugins/smap/src/kernel/scan/ub_hist.h:14-21`）。

#### 一次采样任务的时序

驱动**不使用中断**（`INT_MASK` 初值 0x7 全屏蔽），而是"下发区间 → 睡眠 → 关闭 → 读回"的同步模型：

1. **下发**：`submit_ba_task()` 改写 `BA_CTRL`——把待采样区间起点写进 `sts_base_addr`，置 `sts_enable`、`sts_rd_en`，并按是否 CC 内存决定 `sts_wr_en`（`plugins/smap/src/kernel/scan/hist_ops.c:805-828`）。
2. **等待**：`wait_ba_task(scan_time)` 直接 `usleep_range()`（`plugins/smap/src/kernel/scan/hist_ops.c:830-836`）。
3. **关闭**：`disable_ba_task()` 清 `sts_enable`（`plugins/smap/src/kernel/scan/hist_ops.c:838-849`）。
4. **读回**：`ub_hist_get_statistic_result()` 从结果区读 `freq_register_cnt` 个 u16（`plugins/smap/src/kernel/scan/ub_hist.c:294-313`）。
5. **换算**：每个 u16 = **该地址槽在这段时间内的访问次数**，不是页号。

#### 从"槽计数"到"页频次"的三次变换

| 步骤 | 做什么 | 位置 |
| --- | --- | --- |
| ① 取最大 | 4K 模式下每个扫描窗口内对 2M 子槽取 `max`，得到候选热窗口 | `calc_4k_scan_hot_wins()`，`plugins/smap/src/kernel/scan/hist_ops.c:426-461` |
| ② 求和 | 把落在同一个 kernel page 上的多个子计数器相加（4K 硬件粒度 + 64K 内核页时必须求和，否则同一页会被重复计） | `do_actc_update()`，`plugins/smap/src/kernel/scan/hist_ops.c:676-703` |
| ③ 开平方压缩 | `compress_freq(u16) = floor(sqrt(raw))`，逐位试凑，天然把 0–65535 压到 0–255 | `plugins/smap/src/kernel/scan/drv_common.h:25-36` |

最后一步落到与软件扫描**完全相同的字段**：

```c
smap_page_freq_add(page, compress_freq((u16)raw_sum));
```

（`plugins/smap/src/kernel/scan/hist_ops.c:656`）——即硬件结果直接累加进 `page->flags` 的 8 位 `freq` 计数器（`plugins/smap/src/kernel/scan/smap_page_flags.h:161-186`，哨兵置位时覆盖、否则饱和累加）。这就是"软硬结合模式的计数尺度不同、策略侧必须用 `freqWt` 归一"的根本原因：**两边最后都压进同一个 8 位桶，但原始量纲不一样。**

#### 覆盖范围：一条计数有多大

硬件不是逐页计数的，一条 u16 计数的覆盖范围由 STS 粒度决定：

| 机型 | 结果区条数 | 4K 模式：一条 | 4K 模式：一个窗口（= 整个结果区） | 2M 模式：一条 | 2M 模式：一个窗口 |
| --- | --- | --- | --- | --- | --- |
| N6 | 8192 | 4 KB | 32 MB | 2 MB | 16 GB |
| N7 | 32768 | 4 KB | 128 MB | 2 MB | 64 GB |

- 窗口大小 = `freq_register_cnt × page_size`（`get_hist_scan_win_size()`，`plugins/smap/src/kernel/scan/hist_ops.c:241-249`），`page_size` 取 4K 或 2M。
- **一个窗口恰好用满整个结果区**（32 MB / 4 KB = 8192，128 MB / 4 KB = 32768，16 GB / 2 MB = 8192），所以每次任务都会把整块结果区读回：`ub_hist_get_statistic_result()` 按 `BA_STS_WORD_COUNT(ba_sts_value_count)` 读（`plugins/smap/src/kernel/scan/ub_hist.c:294-313`，条数见 `:162-167`）。
- 上层再用窗口内偏移 `(inter_start - seg->start) >> shift` 取本窗口覆盖的部分，`shift` = 2M 模式 `HIST_ADDR_SHIFT_2M`（21）/ 4K 模式 `HIST_ADDR_SHIFT_4K`（12）（`update_actc_direct()`，`plugins/smap/src/kernel/scan/hist_ops.c:730-731`、`:752-753`；常量见 `plugins/smap/src/kernel/scan/hist_ops.h:20-21`）。
- 代码注释也印证窗口就是 "32MB or 128MB long tracking window"（`plugins/smap/src/kernel/scan/hist_ops.c:614`）。
- 2M 模式的窗口达 16 GB / 64 GB，等于把整段远端内存当作一个窗口，因此**不做热窗口筛选**；`calc_4k_scan_hot_wins()` 只在 4K 多粒度模式下用，且它的输入是"先做一遍 2M 粗扫"得到的临时缓冲区，按 2M 索引、在 32 MB/128 MB 尺度上取 `max`（`plugins/smap/src/kernel/scan/hist_ops.c:431-437`）。

#### 采样节拍

扫描线程 `hist-scan-thread` 是**连续轮转**的：主循环直接调 `hist_scan_sliding(&dev->info, dev->period, dev->pgsize)`，两轮之间不 sleep（只有空闲或出错才 `msleep(1)` / `msleep(THREAD_SLEEP)`，`plugins/smap/src/kernel/scan/hist_ops.c:1331-1379`）。

所以 `dev->period` 不是"睡多久"，而是**一轮扫描的时间预算**：

| 参数 | 值 | 含义 |
| --- | --- | --- |
| `HIST_THREAD_PERIOD` | 512 ms | 一轮的时间预算（`plugins/smap/src/kernel/scan/hist_ops.h:34`；`plugins/smap/src/kernel/scan/hist_ops.c:1582` 赋给 `dev->period`） |
| `hist_scan_duration_per_win` | 64 ms | 单窗口硬件统计时长，取值 (0, 512]（`plugins/smap/src/kernel/scan/hist_ops.c:107-110`、`:134`） |
| 顺扫模式每窗口 | 固定 64 ms | `scan_time = hist_scan_duration_per_win`（`:1071-1072`） |
| 多粒度模式每窗口 | `512 / scan_wins_num_per_ba` | 同上三元表达式 else 分支（`:1073`） |

两种 4K 扫描模式的实质差别（`plugins/smap/src/kernel/scan/hist_ops.c:1131-1151`）：

- `hist_4k_scan_mode_param = 1`（**默认**，顺扫）：不做热区筛选，直接顺序扫**全部** 4K 窗口，每窗口固定 64 ms，扫完所有窗口才进下一轮。一轮耗时 = 窗口数 × 64 ms，与 512 ms 无关。
- `hist_4k_scan_mode_param = 0`（多粒度）：先做一次 2M 粗扫把结果写进 tmpbuffer，用 `calc_4k_scan_hot_wins()` 按窗口取 `max` 筛出热区，再只对热区做 4K 细扫；每个 BA 每轮最多保留 `512/64 - n` = `8 - n` 个热窗口（`:600-602`）。

**推算（未实测）**：顺扫模式下，N7 一个窗口 128 MB、耗时 64 ms，折合约 2 GB/s，扫一遍 64 GB 需要 512 个窗口 × 64 ms ≈ **33 秒**；N6 是 32 MB/64 ms = 512 MB/s，扫 16 GB 同样约 33 秒。两者窗口数都是 512（64 GB ÷ 128 MB、16 GB ÷ 32 MB），因为窗口大小与结果区条数成正比。

#### 模块参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `hist_4k_scan_mode_param` | 1 | 0 = 多粒度（先 2M 粗扫筛热区再 4K 细扫），1 = 顺扫（`plugins/smap/src/kernel/scan/hist_ops.c:91-94`，枚举见 `plugins/smap/src/kernel/scan/hist_ops.h:40-41`） |
| `hist_scan_duration_per_win` | 64 | 单窗口统计时长 ms（`plugins/smap/src/kernel/scan/hist_ops.c:107-110`） |
| `ub_watch_perf_prd_ms` | 200 | 带宽采样周期，须 ≤ `ub_watch_sample_interval_ms`（`:149`、`:185`） |
| `ub_watch_sample_interval_ms` | 1000 | 带宽采样间隔，须 ≥ `ub_watch_perf_prd_ms`（`:150`、`:225`） |

参数名就是 `module_param_cb()` 的第一个实参，即 **`hist_4k_scan_mode_param`**（不是 `hist_4k_scan_mode`）。对应路径 `/sys/module/smap/parameters/hist_4k_scan_mode_param`。

#### 带宽旁路：MAR 计数器

同一模块还用固定物理地址的 MAR 寄存器采集读写流量（`plugins/smap/src/kernel/scan/ub_hist.c:327-331`）：

| 偏移 | 含义 |
| --- | --- |
| 0x0 | `mar_perf_prd` / `mar_perf_mode` / `mar_perf_en`（周期 = `ms × 1e6 >> 4`） |
| 0x4 | `flux_wr` 写流量 |
| 0x8 | `flux_rd` 读流量 |

8 条路径的地址写死在 `ub_hist_mar_cfg_pa[]`（`plugins/smap/src/kernel/scan/ub_hist.c:331-335`）。这份数据就是用户态 `ubBwMonitor` 的来源，对应 §2.5 的"带宽超阈停交换"。

#### 三个值得注意的点

1. **硬件阈值和中断机制实际没启用**。`SMAP_TH1..TH4` 只在 `ub_hist_ba_init()` 里被写成 `INIT` 常量（`BA_SMAP_TH1_INIT` = 0xFFFFFFFF 等，`plugins/smap/src/kernel/scan/upa_smap.h:38-41`），之后全代码再没有写过；`INT_MASK` 初值 0x7 全屏蔽（`plugins/smap/src/kernel/scan/upa_smap.h:51`）。也就是说"哪个页算热"完全由软件拿窗口计数后自己比较，硬件的阈值/中断通路是闲置的。
2. **`sts_base_addr` 的移位常量前后不一致**：`submit_ba_task()` 里对齐用的是 `align_addr_by_sts_size(start_addr, sts_size)`（按实际页型），但写字段时用的是 `shift = get_hist_addr_shift(STS_SIZE_4K)`（**恒定按 4K**，`plugins/smap/src/kernel/scan/hist_ops.c:815-816`）。2M 模式下这两者差 9 位。是否属于有意设计（硬件按 4K 窗口解释该字段）需要对照硬件手册确认，**未验证**。
3. **空间分辨率取决于 STS 粒度，对 64K 内核页仍偏粗**：一条硬件记录 = 4 KB（4K 模式）或 2 MB（2M 模式），但一次任务统计的范围是一整个窗口（4K 模式 32 MB / 128 MB，2M 模式 16 GB / 64 GB，见前文覆盖范围）。当硬件粒度小于内核页时（4K STS + 64 KB 内核页），驱动把落在同一页上的多个子计数器求和后再压缩一次（`do_actc_update()`，`plugins/smap/src/kernel/scan/hist_ops.c:676`、`:693-694`），避免同一页被重复计入而提前饱和。所以硬件判热是"窗口内计数摊到页"，与软件 AF 扫描"逐 PTE 精确"是两种精度。

## 2. 冷热交换速率

### 2.1 grouped 模式（swap 路径）

```c
return (remoteUsed * ratio + GROUP_SWAP_PERCENT_BASE - 1) / GROUP_SWAP_PERCENT_BASE;
```

`CalcGroupSwapBaseCap()`（`plugins/smap/src/user/strategy/grouped_strategy.c:437-444`），`GROUP_SWAP_PERCENT_BASE` = 100（`:24`），`ratio` 取 `smap.group.swap.ratio`，默认 1、范围 0–10（`plugins/smap/src/user/strategy/strategy_config.c:44`、`:401-413`）。

**exchange_cap = ⌈remoteUsed × ratio / 100⌉**

之后再取四重最小值（`plugins/smap/src/user/strategy/grouped_strategy.c:1060-1063`）：

```
swapCap = MIN( exchange_cap,
               maxMigrate,
               nrLocalPages,      // 本地可作冷候选的页数
               nrRemotePages )    // 远端可作热候选的页数
```

默认 `ratio=1` → **每个迁移周期换远端驻留页的 1%**；`ratio=0` 直接关闭交换。

交换阶段的门控（`RunSwapStage()`，`plugins/smap/src/user/strategy/grouped_strategy.c:1110-1153`）：

1. `enableSwap` 为假 → 跳过（`:1112`）
2. 上一轮失衡导致 `groupSwapFrozen` → 冻结（`:1115-1118`）
3. 进程总页数需连续 `GROUP_SWAP_READY_ROUNDS` = 2 轮稳定（`:23`、`:245-258`、`:1120`）
4. 本地未达水位、无远端页、或是共享 target → 该组跳过（`:1130`）
5. 每个组还要连续 2 轮成为候选（`:1134-1141`）

### 2.2 separate 模式 —— 普通进程

`CalcSwapNum()` 的 `PROCESS_TYPE` 分支（`plugins/smap/src/user/strategy/separate_strategy.c:232-265`）：

```c
/* The number of pages with swap should not exceed 1% */
uint64_t maxByTotal = process->walkPage.nrPage / HUNDRED;
```

随后连取 7 个最小值（`:250-263`）：

```
migrateNum = MIN( localLen, maxMigrate )
           = MIN( ..., numaFreePage[localNid] )
           = MIN( ..., lastZeroNum )   // 本地 freq==0 的页数（最冷）
           = MIN( ..., lastFreqNum )   // 远端 freq>0 的页数（有热度）
           = MIN( ..., localFree )     // 本地非白名单页
           = MIN( ..., remoteFree )    // 远端非白名单页
           = MIN( ..., maxByTotal )    // ≤ 总页数 1%
```

即：**只换"本地 0 频次页"与"远端有频次页"，且每周期不超过总页数的 1%**。

### 2.3 separate 模式 —— VM

用频次桶 + 二分求最大 k（`CalLowMigrateNumByBuckets()`，`plugins/smap/src/user/strategy/separate_strategy.c:152-172`）：

```
找最大 k ∈ [0, min(migrateNum, l1Count, l2Count)]，满足
    freqWt × kth_asc(l1, k) + slowThred < kth_desc(l2, k)
```

也就是从两端各取 k 页后仍保持足够频率差的最大 k。

### 2.4 换算成速率

```
每秒交换页数 = swapCap × 1000 / migrate.period(ms)
```

默认 `migrate.period = 2000 ms`、`ratio = 1`：远端驻留 100 GB 的 VM，每周期换掉约 1%（约 2.6 万页 ≈ 100 MB），速率约 **50 MB/s 上下**（按 4K 页粗算）。这是量级换算，不是实测值。

### 2.5 带宽闸门

远端内存带宽超过阈值即**按 NUMA 停止该节点的交换**（`ApplyUbBwStop()`，`plugins/smap/src/user/strategy/migration.c:1493-1515`）：

```c
uint32_t totalBw = readMb + writeMb;
if (totalBw >= threshold)
    current->strategyAttr.ubBwRestrict[nid] = UB_BW_SWAP_STOP;
```

`threshold` 来自 `smap.ub.bw.threshold`（默认 0 = 不开启，`plugins/smap/src/user/strategy/strategy_config.c:64`）。这是**硬停**，不是按比例降速；`UB_BW_SWAP_STOP` 的消费点在 `plugins/smap/src/user/strategy/separate_strategy.c:497`。

## 3. 扫描频率

### 3.1 两个周期

| 参数 | 默认 | 范围 | 约束 |
| --- | --- | --- | --- |
| `smap.scan.period` | 200 ms | 50–60000 ms | — |
| `smap.migrate.period` | 2000 ms | 500–300000 ms | 必须 ≥ scan.period |

常量见 `plugins/smap/src/user/strategy/strategy_config.h:21-27`；`scan <= migrate` 的校验在 `plugins/smap/src/user/strategy/strategy_config.c:778-780`。

### 3.2 驱动方式

- **迁移侧**：守护线程循环 `EnvMsleep(manager->migPeriod)` 后调 `ManagerDaemonWork()`（`plugins/smap/src/user/manage/thread.c:211-219`）——每个迁移周期跑一次策略。
- **扫描侧**：按 pid 的 `scan_time` 驱动，用户态通过 `SmapSetPidPeriod()` 下发（`plugins/smap/src/user/smap_inner_interface.c:128-134`，同时校验 50–60000 / 500–300000 与 `scan <= migrate`）。
- 一个迁移周期内的扫描轮数：`win_nr = ceil(duration × 1000 / scan_time)`（`plugins/smap/src/kernel/scan/access_pid.c:953`）。默认 2000/200 = **约 10 轮**。

### 3.3 内核给出的经验公式

4K 页场景下，内核会打印建议的最小扫描周期（`plugins/smap/src/kernel/scan/access_pid.c:1109-1113`）：

```
suggested_scan_time = total_size_GB × 50 ms
```

`DEFAULT_PERIOD_MS` = 50（`plugins/smap/src/kernel/scan/access_pid.c:32`）。即 **1 GB 目标内存对应 50 ms 扫描周期**——100 GB 的 VM 建议不低于 5 s 扫一轮。这是"打印建议"，不是自动套用。

### 3.4 硬件路径的节拍（与软件路径无关）

走 HIST 硬件扫描时，扫描节奏由内核线程自己决定，**不受 `smap.scan.period` 控制**：

| 量 | 值 | 出处 |
| --- | --- | --- |
| 线程轮次的时间预算 | `HIST_THREAD_PERIOD` = 512 ms | `plugins/smap/src/kernel/scan/hist_ops.h:34` |
| 单窗口统计时长 | `hist_scan_duration_per_win` = 64 ms | `plugins/smap/src/kernel/scan/hist_ops.c:107-110` |
| 顺扫一轮 | 全部窗口 × 64 ms（与 512 无关） | `plugins/smap/src/kernel/scan/hist_ops.c:1071-1072` |
| 多粒度每窗口 | 512 / `scan_wins_num_per_ba` | `plugins/smap/src/kernel/scan/hist_ops.c:1073` |

线程连续轮转、轮间不 sleep（`plugins/smap/src/kernel/scan/hist_ops.c:1331-1379`）。细节与覆盖范围见 §1.6。

## 4. 使用方式

### 4.1 前置条件：必须先关掉两个竞争机制

1. **关 NUMA Balance**：`sudo sysctl -w kernel.numa_balancing=0`。否则内核会把 SMAP 已迁到远端的页再迁回本地，迁移比例永远稳不住。
2. **停 DAMON**：两者都在采集访问特征，同时扫描同一进程会互相干扰周期和冷热判断。检查方式见 `docs/ubturbo_installation.md` 的「关闭冲突的内存管理机制」一节。

这两条不是可选项，文档里用的是"必须"。

### 4.2 安装

```bash
# 源码构建
git clone https://gitcode.com/openeuler/ubturbo.git && cd ubturbo
git submodule update --init --recursive
dos2unix build.sh && ./build.sh          # 产物在 dist/release/{bin,lib,conf}

# 或 RPM
./build.sh ubturbo && ./build.sh ubturbo-rmrs
sudo dnf install -y ubturbo ubturbo-rmrs

# SMAP（必装，二选一）
sudo dnf install -y ubturbo-smap ubturbo-smap-dkms    # 推荐，内核升级自动重编
sudo dnf install -y ubturbo-smap ubturbo-smap-kmod    # 预编译，须与运行内核严格一致
```

未装 SMAP 或未加载驱动时，UBTurbo 服务会直接启动失败，日志是 `Start module failed, name:Smap`。

### 4.3 加载内核模块

```bash
sudo modprobe smap        # 安装 udev 规则后按名称解析即可，不用进 ko 目录
lsmod | grep '^smap'
```

对外提供 `/dev/smap_scan_dev` 和 `/dev/smap_migrate_dev`，属主 `ubturbo:ubturbo`、0600。

### 4.4 启用插件

`/opt/ubturbo/conf/ubturbo_plugin_admission.conf`，格式是 `pluginName=moduleCode`，**模块码必须 > 200**（小于等于 200 是框架内部保留）：

```ini
rmrs=777
turbo_ucache=778
```

出厂默认全部被注释，即**不加载任何插件**——这是很多人"装好了但什么都没发生"的原因。改完必须重启服务，当前不承诺热加载。

### 4.5 启动服务

```bash
sudo systemctl start ubturbo
systemctl status ubturbo --no-pager
journalctl -u ubturbo --since today
```

源码构建场景可直接 `cd dist/release/bin && ./ub_turbo_exec`，但必须保持 `bin/`、`lib/`、`conf/` 的相对布局。

### 4.6 调策略参数

`/opt/ubturbo/conf/smap/smap_strategy.conf`（SMAP **只读取、不回写**）。最常动的几项：

| 配置项 | 默认 | 作用 |
| --- | --- | --- |
| `smap.scan.period` | 200 | 扫描周期 ms，50–60000 |
| `smap.migrate.period` | 2000 | 迁移策略周期 ms，500–300000，须 ≥ scan |
| `smap.group.swap.ratio` | 1 | 每周期的交换量 = 远端页 × ratio/100，0 关闭 |
| `smap.group.swap.local.watermark.ratio` | 95 | 本地水位，未达则本轮不换 |
| `smap.remote.freq.percentile` | 99 | 远端热页取多少分位 |
| `smap.slow.threshold` | 2 | 频率差门槛系数，最终门槛 = 2×freqWt×2 |
| `smap.ub.bw.threshold` | 0 | 远端带宽阈值 MB/s，0 = 不启用；超限按 NUMA 硬停交换 |
| `smap.remote.scan.mode` | 1 | 0 纯软 / 1 软硬结合 / 2 纯硬，硬件只有鲲鹏 950 支持 |

### 4.7 典型调用流程

先确认 socket 就绪，再按顺序下发：

```bash
test -S /opt/ubturbo/ubturbo_ipc

# 1) 启动 SMAP：0=4K（容器），1=2M（虚机），同一实例不得混用
python3 tools/ubturbo_ipc/ubturbo_ipc.py start 0

# 2) 设置远端 NUMA 迁移配额（srcNid destNid sizeMiB）
python3 tools/ubturbo_ipc/ubturbo_ipc.py remote_numa_info 0 2 65536

# 3) 把 PID 加入冷热扫描：entries = pid:scanTime:duration
python3 tools/ubturbo_ipc/ubturbo_ipc.py process_tracking_add 0 12345:200:2000

# 4) 打开该 PID 的页面迁移
python3 tools/ubturbo_ipc/ubturbo_ipc.py process_migrate_enable 1 0 12345

# 5) 观察：查询页面冷热频次 / 是否在运行
python3 tools/ubturbo_ipc/ubturbo_ipc.py freq_query 12345 <length> 0
python3 tools/ubturbo_ipc/ubturbo_ipc.py is_running

# 6) 也可以手工按比例迁出：pid destNid ratio pidType
python3 tools/ubturbo_ipc/ubturbo_ipc.py migrate 12345 2 30 0

# 7) 收尾：移出扫描、停止
python3 tools/ubturbo_ipc/ubturbo_ipc.py process_tracking_remove 0 12345
python3 tools/ubturbo_ipc/ubturbo_ipc.py stop
```

组级迁移（多本地/多远端目标）用 `migrate_grouped <pid> <pageType> <local> <target>`，容量格式是 `0:1024,1:2048`（KB）。查远端 NUMA 上的进程配置用 `process_config_query`。

注意 `process_tracking_add` 的 `scanType` 和 `start` 的 `pageType` 是两个不同维度：前者是扫描方式，后者是页大小。

### 4.8 C 接口

对外的库是 `libubturbo_client.so`，SMAP 侧入口在 `plugins/smap/src/user/smap_interface.h:175-371`，共约 19 个函数。核心几个：

| 函数 | 位置 | 说明 |
| --- | --- | --- |
| `ubturbo_smap_start(pageType, extlog)` | `plugins/smap/src/user/smap_interface.h:218` | 启动，同时确定页型 |
| `ubturbo_smap_stop()` | `plugins/smap/src/user/smap_interface.h:225` | 停止 |
| `ubturbo_smap_process_tracking_add()` | `plugins/smap/src/user/smap_interface.h:302` | 加入扫描，内部校验 50–60000 / 500–300000 且 scan ≤ migrate |
| `ubturbo_smap_process_migrate_enable()` | `plugins/smap/src/user/smap_interface.h:323` | 开关某个 pid 的迁移 |
| `ubturbo_smap_migrate_out()` | `plugins/smap/src/user/smap_interface.h:175` | 按比例/容量迁出 |
| `ubturbo_smap_migrate_out_grouped()` | `plugins/smap/src/user/smap_interface.h:184` | 组级迁移 |
| `ubturbo_smap_freq_query()` | `plugins/smap/src/user/smap_interface.h:256` | 查频次 |

RMRS 插件就是通过 `dlsym(handle, "ubturbo_smap_start")` 从 `libsmap.so` 拿这些符号的（`plugins/rmrs/src/ubturbo_plugin/common/smap/rmrs_smap_module.cpp:53`）。

## 5. 注意事项与风险

1. **1% 的下限效应**：`walkPage.nrPage / 100` 在总页数 < 100 时等于 0，普通进程的交换直接归零（`plugins/smap/src/user/strategy/separate_strategy.c:246`）。小进程永远不交换，这是设计上的副作用还是有意为之，代码没有注释说明。
2. **`ratio` 是"远端页占比"而非"总页占比"**：`CalcGroupSwapBaseCap(remoteUsed)` 的入参是远端驻留页数，所以远端页越多、每周期换得越多，形成"远端占比越高、收回越快"的负反馈——但反过来，远端页很少时交换量会被压到 1 页级别，收敛会很慢。
3. **acc 5 位饱和在 30**：连续热页约 8 轮（`2 × DECAY_CYCLE`）后饱和，此后无法区分"很热"和"极热"（`plugins/smap/src/kernel/scan/smap_page_flags.h:190-215`）。
4. **`prior` 与 `freq` 的相加是"跨尺度拼接"**：两者量纲不同（一个是 4 轮衰减的累计值，一个是单轮读数），文档也承认软件扫描与 HIST 的计数尺度可能不同，要求先用权重归一（`docs/smap/performance_algorithm.md`）。`freqWt` 就是干这个的，默认 `smap.freq.wt = 0` 表示自动推导。
5. **`smap.adaptive.ratio.enable` 调整的不是交换倍率**：它对应 `SetAdaptMem()`（`plugins/smap/src/user/advanced-strategy/scene.c:406-409`），实际改的是 VM 本地内存配额 `nrL1Planed`（`plugins/smap/src/user/advanced-strategy/scene.c:426-460`）。名字容易误解，交换速率并不受它直接调节。

## 6. 未验证项

- HIST 硬件扫描路径的实际计数尺度与软件路径如何换算，代码里靠 `freqWt` 归一，未见成文公式。
- "一个窗口恰好用满整个结果区""4K 模式一条计数 = 4 KB"是根据代码算术反推（`freq_register_cnt × page_size` 与读回条数相互印证），**未查硬件手册确认**。
- 顺扫模式扫完一轮约 33 秒是按"窗口数 × 64 ms"推算，**未实测**；实际还受 BA 数量、并行度与远端内存规模影响。
- `sts_base_addr` 恒定按 `STS_SIZE_4K` 移位（`plugins/smap/src/kernel/scan/hist_ops.c:815-816`）在 2M 模式下是否合理，需硬件手册确认。
- 上述速率为按默认参数的换算，**未在目标硬件实测**交换吞吐与收益。
- `ManagerDaemonWork()` 内部对每个 pid 的处理顺序与并发模型未逐行核对。

## 7. 相关

- [[UBTurbo SMAP 架构与实现原理]]
- [[ARM64 TLBI 与 x86 TLB 失效]]
- [[Firecracker microVM 在 ARM 上的 TLB 刷新时机]]
