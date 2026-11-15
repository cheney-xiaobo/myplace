# DAMON 全景：使能方式、监控参数与策略逻辑

> 把 DAMON 的「怎么开、参数在哪、监控怎么算、策略怎么决策」一次讲完，包含 sysfs 全量目录、模块参数、配额/水位/过滤器状态机与执行链。
>
> - 源码树：`/Users/cheney/work/code/ub/kernel`，HEAD `8003dfcd`（openEuler OLK-6.6）
> - 配置基线：`arch/x86/configs/openeuler_defconfig`
> - 核对日期：2026-11-15；行号已用 `check_linerefs.py` 校验

---

## 0. 架构一句话

DAMON = **采样器** + **区域自适应** + **策略引擎（DAMOS）**：

- 采样器按 `sample_us` 读访问位，按 `aggr_us` 聚合出每个 region 的 `nr_accesses`/`age`；
- region 数在 `nr_regions/min..max` 间自适应分裂/合并，控制开销与精度；
- 策略按 `access_pattern`（size / nr_accesses / age）命中 region，再受 **quota（限速）**、**watermark（水位开关）**、**filter（过滤）** 约束，最后执行 action。

## 1. 前置与使能方式

### 1.1 配置前提（x86-dc 已全部满足）

```
CONFIG_DAMON=y
CONFIG_DAMON_VADDR=y        # 虚拟地址空间（进程）
CONFIG_DAMON_PADDR=y        # 物理地址空间
CONFIG_DAMON_SYSFS=y        # sysfs 控制面（推荐）
CONFIG_DAMON_RECLAIM=y      # 预置回收模块
CONFIG_DAMON_LRU_SORT=y     # 预置 LRU 排序模块
# CONFIG_DAMON_DBGFS is not set   # debugfs 已关（废弃）
```
见 `arch/x86/configs/openeuler_defconfig:1253-1259`。

ARM64 还可开 `CONFIG_DAMON_MEM_SAMPLING`（硬件采样，`mm/damon/Kconfig:35-39`），本树 arm64 的 `CONFIG_MEM_SAMPLING=y`（`arch/arm64/configs/openeuler_defconfig:1287`）。

### 1.2 sysfs（主路径）

```bash
ls /sys/kernel/mm/damon/admin
# kdamonds/
```

完整层级（`Documentation/admin-guide/mm/damon/usage.rst:64-95`）：

```
/sys/kernel/mm/damon/admin
│ kdamonds/nr_kdamonds
│ │ 0/state,pid
│ │ │ contexts/nr_contexts
│ │ │ │ 0/avail_operations,operations
│ │ │ │ │ monitoring_attrs/
│ │ │ │ │ │ intervals/sample_us,aggr_us,update_us
│ │ │ │ │ │ nr_regions/min,max
│ │ │ │ │ targets/nr_targets
│ │ │ │ │ │ 0/pid_target
│ │ │ │ │ │ │ regions/nr_regions
│ │ │ │ │ │ │ │ 0/start,end
│ │ │ │ │ │ schemes/nr_schemes
│ │ │ │ │ │ │ 0/action
│ │ │ │ │ │ │ access_pattern/sz/{min,max}
│ │ │ │ │ │ │                   nr_accesses/{min,max}
│ │ │ │ │ │ │                   age/{min,max}
│ │ │ │ │ │ │ quotas/ms,bytes,reset_interval_ms
│ │ │ │ │ │ │ weights/sz_permil,nr_accesses_permil,age_permil
│ │ │ │ │ │ │ watermarks/metric,interval_us,high,mid,low
│ │ │ │ │ │ │ filters/nr_filters
│ │ │ │ │ │ │ 0/type,matching,memcg_path,addr_start,addr_end,target_idx
│ │ │ │ │ │ │ stats/nr_tried,sz_tried,nr_applied,sz_applied,qt_exceeds
│ │ │ │ │ │ │ tried_regions/total_bytes
│ │ │ │ │ │ │ │ 0/start,end,nr_accesses,age
```

### 1.3 模块方式（预置策略，最简单）

```bash
modprobe damon_reclaim enabled=Y
modprobe damon_lru_sort enabled=Y
# 运行中改参数
echo Y > /sys/module/damon_reclaim/parameters/enabled
echo 60000000 > /sys/module/damon_reclaim/parameters/min_age
```
- 模块的 `enabled` 默认 **N**（`mm/damon/reclaim.c:242-245`；`mm/damon/lru_sort.c:288-291`）
- `commit_inputs=Y` 等价于「重新读参数并重新应用」，用于运行中改完参数后生效（`mm/damon/reclaim.c:42`）
- 模块以 **exclusive** 模式启动（`damon_start(&ctx, 1, true)`，`mm/damon/reclaim.c:210`），同一时间只能有一个 exclusive ctx

### 1.4 debugfs（废弃，不建议）

`Documentation/admin-guide/mm/damon/usage.rst:498-514` 明确标记 DEPRECATED；本树 `CONFIG_DAMON_DBGFS` 未开。

## 2. 最小可跑序列（sysfs，监控一个进程并 pageout）

```bash
cd /sys/kernel/mm/damon/admin
echo 1 > kdamonds/nr_kdamonds
echo 1 > kdamonds/0/contexts/nr_contexts
echo vaddr > kdamonds/0/contexts/0/operations
echo 1 > kdamonds/0/contexts/0/targets/nr_targets
echo $$ > kdamonds/0/contexts/0/targets/0/pid_target
echo 1 > kdamonds/0/contexts/0/schemes/nr_schemes

cd kdamonds/0/contexts/0
# 采样/聚合/更新间隔与 region 数
echo 5000     > monitoring_attrs/intervals/sample_us
echo 100000   > monitoring_attrs/intervals/aggr_us
echo 1000000  > monitoring_attrs/intervals/update_us
echo 10       > monitoring_attrs/nr_regions/min
echo 1000     > monitoring_attrs/nr_regions/max

# 方案：冷页（age ≥ 1 个聚合周期、无访问）pageout
cd schemes/0
echo pageout > action
echo 0 > access_pattern/nr_accesses/min
echo 0 > access_pattern/nr_accesses/max
echo 1 > access_pattern/age/min
echo 4294967295 > access_pattern/age/max
echo 10 > quotas/ms
echo $((128*1024*1024)) > quotas/bytes
echo 1000 > quotas/reset_interval_ms
echo 1 > quotas/weights/age_permil

cd /sys/kernel/mm/damon/admin
echo on > kdamonds/0/state
cat kdamonds/0/state          # on
cat kdamonds/0/pid
```

> 手工写 sysfs 容易错，生产建议用 `damo`（`Documentation/admin-guide/mm/damon/usage.rst:494-496`）。

## 3. 监控参数（monitoring_attrs）

### 3.1 三个间隔

| 参数 | 默认（sysfs 新建 context） | 语义 |
|---|---|---|
| `sample_us` | 5000（5ms） | 每次采样读访问位；决定精度与 CPU 开销 |
| `aggr_us` | 100000（100ms） | 聚合窗口；`nr_accesses` 按窗口计数，`age` 以窗口为单位 |
| `update_us` | 60000000（60s） | 更新监控目标（vaddr 重新取 vma、paddr 重新取区域） |

默认值来自 `mm/damon/sysfs.c:570`（5000/100000/60000000）。
约束（`mm/damon/core.c:615-627`）：`min_nr_regions >= 3`、`min <= max`、`sample_us <= aggr_us`；不满足直接 `-EINVAL`。

### 3.2 region 数与自适应

- 默认 10–1000（`mm/damon/sysfs.c:581`）
- **分裂**：聚合后若 region 数不足或某 region 过大，按 `damon_split_regions_of`（`mm/damon/core.c:1320`）拆分；
- **合并**：相邻且访问频率相近的 region 合并（`damon_merge_two_regions`，`mm/damon/core.c:1209-1219`）；
- **上限**：region 总大小受 `damon_region_sz_limit` 约束，避免监控开销失控。

### 3.3 目标与初始 region

- `vaddr`：目标是进程（`pid_target`），DAMON **自动覆盖整个进程映射**，也可用 `regions/` 指定初始范围（`usage.rst:226-253`）
- `fvaddr`：固定虚拟地址范围，不随进程 vma 变化
- `paddr`：物理地址空间，**不会自动建 region**，必须手写 `regions/`；模块默认取最大 System RAM（`mm/damon/reclaim.c:88-89`、`mm/damon/reclaim.c:120`）

## 4. DAMOS 策略全解

### 4.1 access_pattern（命中条件）

`schemes/<N>/access_pattern/` 下三组 `min/max`（闭区间，`usage.rst:300-311`）：

- `sz`：region 字节数
- `nr_accesses`：每个聚合窗口的访问次数
- `age`：距离上次访问经过的聚合窗口数

三者同时满足才命中。例：`age ≥ 120s/aggr` 且 `nr_accesses == 0` = 「120 秒没被访问的冷区」。

### 4.2 action 与支持矩阵

| action | 语义 | vaddr/fvaddr | paddr |
|---|---|---|---|
| `willneed` | `madvise(MADV_WILLNEED)` 预读 | ✅ | ❌ |
| `cold` | `madvise(MADV_COLD)` 降优先级 | ✅ | ❌ |
| `pageout` | `madvise(MADV_PAGEOUT)` 换出/回收 | ✅ | ✅ |
| `hugepage` | `madvise(MADV_HUGEPAGE)` | ✅ | ❌ |
| `nohugepage` | `madvise(MADV_NOHUGEPAGE)` | ✅ | ❌ |
| `lru_prio` | 提升 LRU 优先级 | ❌ | ✅ |
| `lru_deprio` | 降低 LRU 优先级 | ❌ | ✅ |
| `demotion` | 迁移冷区到指定节点（`remote_node`） | ✅（`CONFIG_MIGRATION`） | ❌ |
| `stat` | 只统计不动作 | ✅ | ✅ |

- 定义见 `include/linux/damon.h:86-117`；支持矩阵同文件 `:100-104`
- vaddr 实现：`mm/damon/vaddr.c:749-810`（含 `DAMOS_DEMOTION` 走 `damon_migrate_pages`，`mm/damon/vaddr.c:483-500`）
- paddr 实现：`mm/damon/paddr.c:215-240`（`pageout` 是 `folio_isolate_lru` + `reclaim_pages`，**同步**）；`lru_prio/deprio` 在 `mm/damon/paddr.c:245-292`

> 注意：`demotion` 的 `remote_node` 在 sysfs 里**没有暴露**，只能用内核 API/模块设置。

### 4.3 quota（限速）

`schemes/<N>/quotas/`：

| 文件 | 含义 |
|---|---|
| `ms` | 每个 `reset_interval_ms` 内最多占用的动作时间（毫秒） |
| `bytes` | 每个窗口最多作用的内存字节数 |
| `reset_interval_ms` | 窗口长度 |
| `weights/*_permil` | 优先权重：size / 访问频率 / age，千分比 |

逻辑：
1. `damos_set_effective_quota`（`mm/damon/core.c:1093-1114`）：`esz = 实测吞吐 × ms`（不超过 `bytes`），`ms=0` 时 `esz=bytes`；
2. `damos_adjust_quota`（`mm/damon/core.c:1116-1166`）：每个窗口用评分直方图算 `min_score`，只动作高分区域；
3. 窗口切换时累计 `total_charged_sz`、清零 `charged_sz`、超限记 `qt_exceeds`（`mm/damon/core.c:1132-1141`）；
4. 动作时若单 region 超出剩余配额，先 `damon_split_region_at` 再动作（`mm/damon/core.c:1033-1039`）。

### 4.4 watermarks（水位开关）

`schemes/<N>/watermarks/`：`metric`（`none` / `free_mem_rate` 千分比）、`interval_us`、`high/mid/low`。

状态机（`mm/damon/core.c:1425-1450`）：

- 当前指标 `metric > high` 或 `metric < low` → **停用**（activated=false），等待 `interval_us`；
- `high >= metric >= mid` 且未激活 → **保持停用**（等指标继续下降）；
- `metric < mid` 且未激活 → **激活**；
- 激活后只在越过 `high`/`low` 时才会被关掉（滞回，避免抖动）。

### 4.5 filters（过滤器）

`schemes/<N>/filters/nr_filters`，每个 filter 有 `type`、`matching`、`memcg_path`、`addr_start/end`、`target_idx`（`usage.rst:355-401`）。

- 类型：`anon`（匿名页）、`memcg`（指定 cgroup 路径）、`addr`（地址区间）、`target`（监控目标索引）（`include/linux/damon.h:232-252`）
- `matching=Y`：过滤掉匹配的页/区；`N`：只保留匹配的
- **实现层差异**：
  - `anon`/`memcg` 由 ops 层处理（`mm/damon/paddr.c:182-186`），**只有 paddr 支持**；
  - `addr`/`target` 由 core 层处理（`mm/damon/core.c:963-1020`）
- **统计差异**：`addr`/`target` 过滤掉的区域不计入 `tried`；`anon`/`memcg` 过滤的计入 `tried`（`usage.rst:397-401`）

### 4.6 stats 与 tried_regions

- `stats/`：`nr_tried`、`sz_tried`、`nr_applied`、`sz_applied`、`qt_exceeds`（`usage.rst:405-418`）
  - 不实时刷新；要写 `update_schemes_stats` 到 `kdamonds/<N>/state` 才更新
- `tried_regions/`：写 `update_schemes_tried_regions` 后生成每个 region 的明细（`start/end/nr_accesses/age`），用于验证「策略到底打到了哪些页」（`usage.rst:420-443`）

## 5. 执行链（kdamond 主循环）

`kdamond_fn`（`mm/damon/core.c:1513-1600`）：

```
while (!stop):
    prepare_access_checks()            # 清访问位/准备采样
    usleep(sample_interval)
    passed_sample_intervals++
    max_nr_accesses = check_accesses() # 读访问位，累计 nr_accesses
    if 到达聚合点:
        merge_regions(max_nr_accesses/10, sz_limit)
        after_aggregation()
    if 有 scheme:
        kdamond_apply_schemes()        # 配额/水位/过滤/评分/动作
    if 到达聚合点:
        重置聚合计数、split_regions、ops.reset_aggregated
    if 到达 update 点:
        ops.update()                   # vaddr 重新取 vma / paddr 重取区域
```

- 每个 scheme 有自己的 `next_apply_sis`：默认每个聚合窗口都可尝试（`mm/damon/core.c:1197-1203`）；
- `damon_do_apply_schemes`（`mm/damon/core.c:1063-1090`）先查 `activated` 与配额，再 `damos_apply_scheme`；
- `damos_apply_scheme`（`mm/damon/core.c:1023-1061`）负责：超配额截断 → `damos_filter_out` → 调 ops `apply_scheme` → 计时收费 → 更新统计；
- 动作后把 `r->age = 0`（`mm/damon/core.c:1056-1057`），避免同一区域被反复命中。

## 6. 两个预置策略模块

### 6.1 damon_reclaim（冷页回收）

**默认参数**（`mm/damon/reclaim.c:50-108`）：

| 参数 | 默认 | 含义 |
|---|---|---|
| `min_age` | 120000000（120s） | 冷区阈值 |
| `sample_interval` | 5000 | 5ms |
| `aggr_interval` | 100000 | 100ms |
| `min_nr_regions` / `max_nr_regions` | 10 / 1000 | region 范围 |
| `quota_ms` | 10 | 每窗口 10ms 动作时间 |
| `quota_sz` | 128 MiB | 每窗口最多回收 128 MiB |
| `quota_reset_interval_ms` | 1000 | 1s 窗口 |
| `weight_age` | 1 | 优先回收更老的区域 |
| `wmarks_high/mid/low` | 500/400/200 | 空闲率 50%/40%/20% |
| `wmarks_interval` | 5000000 | 5s 检查一次 |
| `monitor_region_start/end` | 0/0 | 0 表示自动取最大 System RAM |
| `skip_anon` | N | Y 时过滤匿名页 |
| `enabled` | N | 默认不启动 |

**scheme**：`sz ≥ PAGE_SIZE`、`nr_accesses == 0`、`age ≥ min_age/aggr` → `DAMOS_PAGEOUT`（`mm/damon/reclaim.c:120-143`）。

**管控逻辑**：每聚合窗口尝试一次；先过水位（空闲率高于 high 或低于 low 就停，低于 mid 才启动）；再过 quota（10ms/128MiB 每秒）；`skip_anon=Y` 时加 anon 过滤器（`mm/damon/reclaim.c:186-196`）。

### 6.2 damon_lru_sort（冷热 LRU 排序）

**默认参数**（`mm/damon/lru_sort.c:53-103`）：

| 参数 | 默认 | 含义 |
|---|---|---|
| `hot_thres_access_freq` | 500（千分比） | 访问频率 ≥ 50% 视为热区 |
| `cold_min_age` | 120000000（120s） | 冷区阈值 |
| `quota_ms` | 10 | 每个 scheme 分一半（5ms） |
| `weight_nr_accesses` | 1 | 优先处理更热的区域 |
| `wmarks_high/mid/low` | 200/150/50 | 空闲率 20%/15%/5% |
| `sample/aggr` | 5000/100000 | 同 reclaim |

**两个 scheme**（`mm/damon/lru_sort.c:130-200`）：
- hot：`nr_accesses ≥ hot_thres` → `DAMOS_LRU_PRIO`（标记 accessed，抗回收）
- cold：`nr_accesses == 0` 且 `age ≥ cold_thres` → `DAMOS_LRU_DEPRIO`（标记 unaccessed，优先回收）

**管控逻辑**：hot 阈值按 `max_nr_accesses × hot_thres_access_freq / 1000` 计算；两 scheme 各占一半 quota；watermark 与 reclaim 同一状态机。

## 7. 参数总表

### 7.1 sysfs

| 路径 | 默认/范围 | 说明 |
|---|---|---|
| `kdamonds/<N>/state` | on/off/commit/update_schemes_stats/update_schemes_tried_regions/clear_schemes_tried_regions | 控制与统计刷新 |
| `contexts/<N>/operations` | vaddr/fvaddr/paddr | 监控地址空间类型 |
| `intervals/sample_us` | 5000 | 采样间隔 |
| `intervals/aggr_us` | 100000 | 聚合间隔 |
| `intervals/update_us` | 60000000 | 目标更新间隔 |
| `nr_regions/min,max` | 10,1000；min≥3 | region 上下限 |
| `targets/<N>/pid_target` | pid | vaddr 目标进程 |
| `schemes/<N>/action` | 见 §4.2 | 动作 |
| `access_pattern/{sz,nr_accesses,age}/{min,max}` | 闭区间 | 命中条件 |
| `quotas/ms,bytes,reset_interval_ms` | 0 表示不限 | 限速 |
| `quotas/weights/*_permil` | 0-1000 | 优先权重 |
| `watermarks/metric,interval_us,high,mid,low` | none/free_mem_rate | 水位开关 |
| `filters/<N>/type,matching,...` | anon/memcg/addr/target | 过滤 |

### 7.2 模块参数

公共（`mm/damon/modules-common.h:10-45`）：
`sample_interval`、`aggr_interval`、`min_nr_regions`、`max_nr_regions`、`quota_ms`、`quota_reset_interval_ms`、`quota_sz`、`wmarks_interval`、`wmarks_high`、`wmarks_mid`、`wmarks_low`，以及统计 `nr_<x>_tried_regions`/`bytes_<x>_tried_regions` 等只读项。

模块私有：
- `damon_reclaim`：`min_age`、`monitor_region_start`、`monitor_region_end`、`skip_anon`、`commit_inputs`、`enabled`、`kdamond_pid`
- `damon_lru_sort`：`hot_thres_access_freq`、`cold_min_age`、`monitor_region_start`、`monitor_region_end`、`commit_inputs`、`enabled`、`kdamond_pid`

## 8. 验证与调优

```bash
# 运行状态
cat /sys/kernel/mm/damon/admin/kdamonds/0/state
cat /sys/kernel/mm/damon/admin/kdamonds/0/pid

# 策略是否命中
cd /sys/kernel/mm/damon/admin/kdamonds/0
echo update_schemes_stats > state
cat contexts/0/schemes/0/stats/nr_tried contexts/0/schemes/0/stats/sz_applied
echo update_schemes_tried_regions > state
cat contexts/0/schemes/0/tried_regions/total_bytes

# 模块
cat /sys/module/damon_reclaim/parameters/enabled
cat /sys/module/damon_reclaim/parameters/reclaim_tried_regions
```

调优顺序建议：
1. 先 `action=stat` 跑一轮，看 `nr_accesses`/`age` 分布，确认模式参数；
2. 再开 `pageout`，从小 quota（`ms=10`、`bytes=128MiB`）起步；
3. 用水位（`free_mem_rate` 50/40/20）做自动化开关；
4. 最后按 `tried_regions` 核对命中区域是否符合预期。

## 9. 坑与限制

1. **exclusive ctx**：模块用 `damon_start(..., exclusive=true)`，同一时间只能有一个 exclusive 上下文；sysfs 多 kdamond 时注意互斥（`mm/damon/reclaim.c:210`）。
2. **filters 的平台差异**：`anon`/`memcg` 只有 paddr 支持，vaddr 上写这两个 type 会被忽略/拒绝（`mm/damon/paddr.c:182-186`）。
3. **`stats` 不实时**：不写 `update_schemes_stats` 读到的可能是上次快照。
4. **`update_us` 默认 60s**：vaddr 目标进程新 mmap 的区域最多 60s 后才被覆盖；需要更快反应就调小。
5. **pageout 在 paddr 是同步回收**（`reclaim_pages`），大批量时 kdamond 会被拖住；vaddr 的 `MADV_PAGEOUT` 是异步。
6. **`DAMOS_DEMOTION` 的 `remote_node` 没暴露到 sysfs**，只能模块/API 设置；且依赖 `CONFIG_MIGRATION`。
7. **采样开销**：`sample_us` 越小、`max_nr_regions` 越大，CPU 越高；paddr 用 `PAGE_IDLE_FLAG`，vaddr 走页表扫描。
8. **模块参数改完要 `commit_inputs=Y`**（或重新 modprobe）才重新应用。

## 10. 参考

- `Documentation/admin-guide/mm/damon/usage.rst`（sysfs 全量）
- `Documentation/admin-guide/mm/damon/reclaim.rst`、`Documentation/admin-guide/mm/damon/lru_sort.rst`
- `Documentation/admin-guide/mm/damon/start.rst`
- 源码：`mm/damon/core.c`、`mm/damon/sysfs.c`、`mm/damon/reclaim.c`、`mm/damon/lru_sort.c`、`mm/damon/vaddr.c`、`mm/damon/paddr.c`

## 11. 更新日志

- 2026-11-15：初版；覆盖使能方式、sysfs 全图、监控参数、DAMOS 策略（pattern/action/quota/watermark/filter/stats）、执行链、两个预置模块与参数总表。
