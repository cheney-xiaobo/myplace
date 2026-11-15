# 内核内存调度模式全景与 Roadmap（OLK-6.6 基线）

> 目的：把「内核当前针对内存的所有调度模式」一次梳理清楚，并给出**可直接执行**的开启/验证方式与未来 roadmap。
>
> - 源码树：`/Users/cheney/work/code/ub/kernel`，HEAD `8003dfcd`（openEuler OLK-6.6）
> - 配置基线：`arch/x86/configs/openeuler_defconfig`（下称 x86-dc）、`arch/arm64/configs/openeuler_defconfig`（arm64-dc）
> - 主线基线：kernel.org 2026-11-15 页面显示 stable **7.3.4**、mainline **7.4-rc2**（2026-11-09）、6.6 longterm **6.6.162**
> - 核对日期：2026-11-15

## 0. 一句话结论

内存调度不是「一个开关」，而是 **5 类、20+ 个机制**的组合；本树真正**默认生效**的只有少数（NUMA balancing=NORMAL、经典 LRU 回收、swap、THP=always、compaction、memcg 记账），而 **MGLRU / zswap / PSI / demotion / DAMON / etmem / zram_reclaim 全部属于「编译进去但默认不工作」**。做性能分析或方案设计时，第一步必须是「默认状态核对」，否则会把未开启的机制当成已生效。

### 0.1 速览表

| 类别 | 代表机制 | 本树默认状态 | 作用 |
|---|---|---|---|
| 页放置 | mempolicy、NUMA balancing、cpuset | NORMAL 模式，多节点才启用 | 决定「页放哪」 |
| 回收 | 经典 LRU、MGLRU、memory.reclaim、PSI | 经典 LRU 生效；MGLRU/PSI 关 | 决定「先收谁」 |
| 换出/压缩 | swap、zram、zswap、MADV_PAGEOUT | swap 生效；zram 是模块；zswap 关 | 决定「压到哪」 |
| 迁移/分层 | demotion、memory tiers、compaction、THP/mTHP | demotion 关；THP always | 决定「冷页迁哪层」 |
| 容器 QoS | memcg max/high、OOM 优先级、swap QoS | 记账生效；QoS 开关默认 0 | 决定「谁让路」 |

## 1. 判定方法：配置 → 运行时开关 → 计数器 → 行为

| 级别 | 看什么 | 命令 |
|---|---|---|
| L1 编译配置 | Kconfig 是否编入 | `zcat /proc/config.gz \| grep -E 'CONFIG_(LRU_GEN\|ZSWAP\|ZRAM\|DAMON\|NUMA_BALANCING\|PSI\|ETMEM)'` |
| L2 运行时开关 | sysfs/sysctl 是否打开 | `cat /proc/sys/kernel/numa_balancing`、`cat /sys/kernel/mm/lru_gen/enabled`、`cat /sys/module/zswap/parameters/enabled` |
| L3 计数器 | 是否真的在做事 | `grep -E 'pgscan\|pgsteal\|pswpin\|pswpout\|thp_\|compact_\|numa_' /proc/vmstat` |
| L4 行为 | 目标进程/页是否变化 | `cat /proc/<pid>/status`（VmRSS/VmSwap）、`numastat -p <pid>`、`zramctl` |

**判定原则**：只有 L2 + L3 同时成立，才可以说「该机制在生效」。只看 Kconfig 一律不算。

## 2. 页放置（Placement）

### 2.1 静态 NUMA 策略（mempolicy）

- 模式：`MPOL_DEFAULT`、`MPOL_PREFERRED`、`MPOL_BIND`、`MPOL_INTERLEAVE`、`MPOL_WEIGHTED_INTERLEAVE`（`include/uapi/linux/mempolicy.h:20-27`）
- `WEIGHTED_INTERLEAVE` 按节点权重交错，适合 CXL/PMEM 这类带宽不对称的层级（`mm/mempolicy.c:562`、`mm/mempolicy.c:2101-2119`）
- 接口：`set_mempolicy(2)`、`mbind(2)`、`set_mempolicy_home_node(2)`

```bash
numactl --hardware
numactl --interleave=all ./workload        # 交错
numactl --prefer=0 ./workload              # 优先节点
numastat -p $$ ; cat /proc/$$/numa_maps     # 验证实际落点
```

### 2.2 自动 NUMA balancing（默认生效）

- 编译开关：`CONFIG_NUMA_BALANCING=y`、`CONFIG_NUMA_BALANCING_DEFAULT_ENABLED=y`（`arch/x86/configs/openeuler_defconfig:185-186`）
- 只在**多 NUMA 节点**时自动启用：`mm/mempolicy.c:3414-3428`
- 默认模式 = `NUMA_BALANCING_NORMAL`（`kernel/sched/core.c:4452`、`kernel/sched/core.c:4462-4468`）
- 模式位定义：`include/linux/sched/sysctl.h:22-27`（`NORMAL=0x1`、`MEMORY_TIERING=0x2`）
- sysctl 取值 0-4，handler 在 `kernel/sched/core.c:4483-4503`，入口在 `kernel/sched/core.c:4644-4651`

```bash
cat /proc/sys/kernel/numa_balancing          # 1=普通, 2=内存分层, 3=两者
echo 2 > /proc/sys/kernel/numa_balancing     # 切到 memory tiering 模式
grep -E 'numa_pte_updates|numa_hint_faults' /proc/vmstat
```

### 2.3 memory tiering 模式

- 只有 sysctl 的 bit1（`0x2`）才是 tiering；被 `mm/migrate.c:685`、`mm/vmscan.c:4793` 等路径使用
- 单独打开 tiering 不会凭空产生「层」：tier 拓扑要先由 HMAT/CXL 建立（见 §5.2）
- 判据：`cat /sys/kernel/mm/numa/demotion_enabled` + `/sys/devices/system/node/` 的 tier 信息

### 2.4 cpuset 绑定

- cgroup v2 `cpuset.mems` 限定可分配节点；常与 memcg 一起做租户隔离

```bash
cat /sys/fs/cgroup/<cg>/cpuset.mems
echo 0 > /sys/fs/cgroup/<cg>/cpuset.mems
```

### 2.5 分配侧

- 默认策略是 first-touch（谁先碰谁分配）
- 大页/巨页见 §5.3、§5.4；CMA 见 §5.5

## 3. 回收（Reclaim）

### 3.1 经典 LRU（默认生效）

- kswapd + direct reclaim + watermark；`vm.swappiness=60`（`mm/vmscan.c:203`）
- 可调：`vm.watermark_scale_factor`、`vm.min_free_kbytes`、`vm.swappiness`
- 判据：`/proc/vmstat` 的 `pgscan_*` / `pgsteal_*`、`allocstall_*`

```bash
grep -E 'pgscan_direct|pgscan_kswapd|pgsteal' /proc/vmstat
```

### 3.2 MGLRU（编译进去，默认关闭）

- `CONFIG_LRU_GEN=y`，但 `CONFIG_LRU_GEN_ENABLED` 未设置（`arch/x86/configs/openeuler_defconfig:1235-1236`；`arch/arm64/configs/openeuler_defconfig:1269`）
- 默认静态键为 false（`mm/vmscan.c:2631-2636`）；运行时可整体开关（`mm/vmscan.c:5247-5251`）
- 文档：`Documentation/admin-guide/mm/multigen_lru.rst:27-31`、`Documentation/admin-guide/mm/multigen_lru.rst:57-61`

```bash
cat /sys/kernel/mm/lru_gen/enabled      # 默认 0x0000
echo y > /sys/kernel/mm/lru_gen/enabled
cat /sys/kernel/mm/lru_gen/enabled      # 0x0007
```

**注意**：MGLRU 生效时，zram_reclaim 不介入回收决策（`Documentation/admin-guide/mm/zram_reclaim.rst:25-27`）。

### 3.3 主动回收（memcg）

- `memory.reclaim`：cgroup v1 在 `mm/memcontrol.c:6132`，v2 在 `mm/memcontrol.c:8238`
- `memory.high` 超限会触发节流式回收

```bash
echo "100M" > /sys/fs/cgroup/<cg>/memory.reclaim
cat /sys/fs/cgroup/<cg>/memory.current
grep -E 'pgscan|pgsteal' /sys/fs/cgroup/<cg>/memory.stat
```

### 3.4 zswap（前端压缩，默认关闭）

- `CONFIG_ZSWAP=y`，但 `CONFIG_ZSWAP_DEFAULT_ON` 未设置（`arch/x86/configs/openeuler_defconfig:1110-1111`）
- 默认压缩器 lzo（`:1114-1119`），zpool zsmalloc（`:1122`）
- 开启：`zswap.enabled=1` 或 `echo 1 > /sys/module/zswap/parameters/enabled`（`Documentation/admin-guide/mm/zswap.rst:30-38`）

```bash
cat /sys/module/zswap/parameters/enabled
echo 1 > /sys/module/zswap/parameters/enabled
grep zswap /proc/vmstat
```

### 3.5 zram_reclaim（openEuler 扩展，ARM64 限定）

- `CONFIG_ZRAM_RECLAIM`（`mm/Kconfig:64-80`）；arm64 默认开（`arch/arm64/configs/openeuler_defconfig:1152`）
- 依赖 `ARM64 && MEMCG && SWAP`，且**只在特定 HiSilicon CPU** 注册接口（`Documentation/admin-guide/mm/zram_reclaim.rst:42-55`）
- 接口：`memory.zram_reclaim "<enable> <ratio>"`，默认 ratio 30、范围 1-60，关闭写 `0 0`
- 生效前提：MGLRU 未生效、anon 页不少于 file 页一半、有 swap 使用（`Documentation/admin-guide/mm/zram_reclaim.rst:31-40`）

```bash
echo "1 30" > /sys/fs/cgroup/<cg>/memory.zram_reclaim
cat /sys/fs/cgroup/<cg>/memory.zram_reclaim
```

### 3.6 DAMON

- `CONFIG_DAMON=y` + `VADDR` / `PADDR` / `SYSFS` / `RECLAIM` / `LRU_SORT`（`arch/x86/configs/openeuler_defconfig:1253-1259`）
- **不是开了就工作**：需用户态通过 sysfs 配置 kdamond + scheme；`DAMON_RECLAIM`、`DAMON_LRU_SORT` 是预置 scheme
- ARM64 可用硬件采样：`CONFIG_MEM_SAMPLING=y`（`arch/arm64/configs/openeuler_defconfig:1287`；`mm/Kconfig:1519-1527`）
- 文档：`Documentation/admin-guide/mm/damon/`
- 判据：`/sys/kernel/mm/damon/admin/` 下的 `state`、`/proc/vmstat` 回收计数

### 3.7 etmem（openEuler 分层）

- `CONFIG_ETMEM=y` + `etmem_scan` / `etmem_swap` 模块（`arch/x86/configs/openeuler_defconfig:1244-1246`；`mm/Kconfig:1498-1504`）
- 需要用户态 etmemd：周期扫描目标 vma → 冷页加入 swapcache → 换出到高速介质（`mm/etmem.c`）
- 判据：etmemd 日志 + 目标进程 RSS/Swap + swap 使用量

### 3.8 PSI（压力反馈，默认关闭）

- `CONFIG_PSI=y`，但 `CONFIG_PSI_DEFAULT_DISABLED=y`（`arch/x86/configs/openeuler_defconfig:141-142`）
- 默认静态键 disabled（`kernel/sched/psi.c:144-148`）；启动参数 `psi=1`（`Documentation/admin-guide/kernel-parameters.txt:4831`）

```bash
cat /proc/pressure/memory
```

PSI 的价值不在「回收」，而在给回收/迁移策略提供反馈信号（见 §9）。

## 4. 换出与压缩

### 4.1 swap 基础（默认生效）

- `CONFIG_SWAP=y`（`arch/x86/configs/openeuler_defconfig:1109`）
- 关键参数：`vm.swappiness`、`vm.page-cluster`（0 = 关 swap readahead）
- 判据：`/proc/vmstat` 的 `pswpin`/`pswpout`、`/proc/meminfo` 的 `SwapCached`

### 4.2 zram（压缩内存盘，模块）

- `CONFIG_ZRAM=m`（`arch/x86/configs/openeuler_defconfig:2412`）；`CONFIG_ZRAM_WRITEBACK=y`，但 `CONFIG_ZRAM_MEMORY_TRACKING` 未设置（`:2418-2419`）
- **顺序要求**：`backing_dev`、`comp_algorithm` 必须先于 `disksize`，否则 `EBUSY`（`drivers/block/zram/zram_drv.c:460-490`）
- `bd_stat` 单位是 4K bytes（`drivers/block/zram/zram_drv.c:1191-1203`）
- `MEMORY_TRACKING=n` → 只能 `idle=all`，**不能按页龄只淘汰冷页**

```bash
modprobe zram num_devices=1
echo /dev/ram0 > /sys/block/zram0/backing_dev     # 必须在 disksize 前
echo lz4 > /sys/block/zram0/comp_algorithm
echo 512M > /sys/block/zram0/disksize
mkswap /dev/zram0 && swapon -p 100 /dev/zram0
echo all > /sys/block/zram0/idle
echo idle > /sys/block/zram0/writeback
cat /sys/block/zram0/mm_stat /sys/block/zram0/bd_stat
```

### 4.3 zswap

见 §3.4。

### 4.4 主动换出（madvise / process_madvise）

- `MADV_PAGEOUT` / `process_madvise`：`mm/madvise.c:1864-1868` 要求 `CAP_SYS_NICE`
- 这是 AgentENV vPause、沙箱内存压缩的入口（`process_madvise(MADV_PAGEOUT)`）
- 判据：`VmSwap` 增长 + `mm_stat` / `bd_stat` 变化

### 4.5 memcg swap 限制

- `memory.swap.max`；配合 `memory.zram_reclaim` 控制匿名页回收倾向

## 5. 迁移 / 分层

### 5.1 compaction 与 migration

- `CONFIG_COMPACTION=y`（`arch/x86/configs/openeuler_defconfig:1167`）；proactiveness 默认 20（`mm/compaction.c:1919`）
- `CONFIG_MIGRATION=y`（`:1170`）
- 手动触发：`echo 1 > /proc/sys/vm/compact_memory`
- 判据：`/proc/vmstat` 的 `compact_*`

### 5.2 memory tiers 与 demotion

- `mm/memory-tiers.c`；`numa_demotion_enabled` 默认 **false**（`mm/memory-tiers.c:937`），sysfs 属性在 `mm/memory-tiers.c:961-965`
- 前提：tier 拓扑已建立（HMAT/CXL/手动），且 NUMA balancing 打开 tiering 位（§2.3）

```bash
cat /sys/kernel/mm/numa/demotion_enabled
echo true > /sys/kernel/mm/numa/demotion_enabled
grep -E 'pgdemote_' /proc/vmstat
```

### 5.3 THP / mTHP

- `CONFIG_TRANSPARENT_HUGEPAGE_ALWAYS=y`（`arch/x86/configs/openeuler_defconfig:1186-1187`）
- mTHP：按 size 的 sysfs（`mm/huge_memory.c:900`；`Documentation/admin-guide/mm/transhuge.rst:48-51`、`Documentation/admin-guide/mm/transhuge.rst:115-117`）

```bash
cat /sys/kernel/mm/transparent_hugepage/enabled
ls /sys/kernel/mm/transparent_hugepage/hugepages-*/enabled
echo always > /sys/kernel/mm/transparent_hugepage/hugepages-64kB/enabled
grep -E 'thp_fault_alloc|thp_swpout|thp_split_page' /proc/vmstat
```

### 5.4 hugetlb

- `CONFIG_HUGETLB_PAGE=y`（`arch/x86/configs/openeuler_defconfig:8428-8431`）
- `vm.nr_hugepages` / `hugepages-<size>kB/nr_hugepages`

### 5.5 CMA / hotplug / ZONE_DEVICE / CXL

- `CONFIG_CMA=y`（`arch/x86/configs/openeuler_defconfig:1199-1203`）
- `CONFIG_MEMORY_HOTPLUG=y`（`:1158-1159`）、`CONFIG_MHP_MEMMAP_ON_MEMORY=y`（`:1161`）
- `CONFIG_ZONE_DEVICE=y`、`CONFIG_DEVICE_PRIVATE=y`（`:1214-1217`）
- CXL 为模块：`CONFIG_CXL_BUS=m` 等（`:2138-2148`）

### 5.6 迁移系统调用

- `migrate_pages(2)`、`move_pages(2)`；NUMA balancing 的 hint fault 迁移走 `mm/migrate.c`

## 6. 容器 / 租户 QoS

- memcg v2 基础：`memory.max`、`memory.high`、`memory.low`、`memory.min`、`memory.reclaim`、`memory.swap.max`、`memory.oom.group`
- openEuler 扩展：
  - `CONFIG_MEMCG_QOS`（`arch/x86/configs/openeuler_defconfig:191`）、`CONFIG_MEMCG_OOM_PRIORITY`（`:193`）；sysctl `kernel.memcg_qos_enable` 默认 **0**（`mm/memcontrol.c:4368-4369`）
  - `CONFIG_MEMCG_SWAP_QOS`（`:194`）；`memcg_swap_qos_enable` 默认 **0**（`mm/memcontrol.c:4550`）
  - `CONFIG_MEMCG_MEMFS_INFO`（`:192`）
  - `memory.zram_reclaim`（ARM64 限定，§3.5）
- dmem cgroup（设备内存隔离）：`CONFIG_CGROUP_DMEM=y`（`arch/x86/configs/openeuler_defconfig:215`；`Documentation/admin-guide/cgroup-v2.rst:2550-2581`）
- PSI（§3.8）+ cpuset（§2.4）

## 7. 本树扩展 vs 上游

| 扩展 | 位置 | 平台 | 需要用户态 | 性质 |
|---|---|---|---|---|
| etmem | `mm/etmem.c`、`mm/Kconfig:1498-1504` | x86/arm64 | 是（etmemd） | 分层内存（DRAM + 压缩/高速介质） |
| zram_reclaim | `mm/zram_reclaim.c`、`mm/Kconfig:64-80` | ARM64 + 特定 HiSilicon | 否（memcg 接口） | zram 场景的回收偏置 |
| memcg OOM 优先级 / swap QoS | `mm/memcontrol.c`、defconfig:191-194 | 通用 | 否（sysctl） | 容器 QoS |
| MEM_SAMPLING | `mm/Kconfig:1519-1527`、arm64-dc:1287 | ARM64 | 否 | 硬件采样给 NUMA/DAMON |
| BPF_READAHEAD | `mm/Kconfig`（`config BPF_READAHEAD`） | 通用 | 是（BPF 程序） | 可编程预读（非回收） |

**不要混淆**：`net/Kconfig:77-82` 的 `UB_UMS` 是 UB socket 加速，不是内存调度。

## 8. 「编译进去 ≠ 生效」核对表

| 机制 | Kconfig | 默认生效 | 开启方式 | 生效判据 |
|---|---|---|---|---|
| NUMA balancing | `NUMA_BALANCING_DEFAULT_ENABLED=y` | 是（多节点） | `kernel.numa_balancing` | `numa_pte_updates` 增长 |
| MGLRU | `LRU_GEN=y`、`LRU_GEN_ENABLED` 未设 | 否 | `echo y > /sys/kernel/mm/lru_gen/enabled` | `lru_gen` 计数/`enabled` 非 0 |
| zswap | `ZSWAP=y`、默认关 | 否 | `zswap.enabled=1` 或 sysfs | `zswap_*` 计数 |
| zram | `ZRAM=m` | 否（模块未加载） | `modprobe zram` | `/dev/zram0` 存在 |
| zram writeback | `ZRAM_WRITEBACK=y` | 依赖配置 | `backing_dev` + `writeback=idle` | `bd_stat` 非 0 |
| PSI | `PSI=y`、`PSI_DEFAULT_DISABLED=y` | 否 | `psi=1` | `/proc/pressure/memory` 存在 |
| demotion | 无独立开关 | 否 | `demotion_enabled=true` | `pgdemote_*` 增长 |
| DAMON | `DAMON*=y` | 否 | sysfs 配置 scheme | `kdamonds/*/state=on` |
| etmem | `ETMEM=y` | 否 | 加载模块 + etmemd | etmemd 日志/swap |
| zram_reclaim | `ZRAM_RECLAIM` | ARM64 部分机型 | `memory.zram_reclaim` | `memory.stat` 比例变化 |
| memcg QoS | `MEMCG_QOS/OOM_PRIORITY/SWAP_QOS=y` | 否（sysctl=0） | sysctl 置 1 | 对应行为/统计 |
| THP | `TRANSPARENT_HUGEPAGE_ALWAYS=y` | 是 | 已默认 | `thp_fault_alloc` 增长 |

## 9. Roadmap

### 9.1 三个基线

| 基线 | 版本 | 说明 |
|---|---|---|
| 本树 | OLK-6.6（6.6 LTS + openEuler 回移） | 发行版内核，回移了 mTHP 等能力 |
| 主线 stable | 7.3.4（2026-11-10） | kernel.org |
| 主线 mainline | 7.4-rc2（2026-11-09） | 下一版本候选 |
| 6.6 longterm | 6.6.162（2026-11-06） | 与本树同代 |

### 9.2 H1（0-3 个月）：把本树「已具备但未启用」的能力用起来

1. **基线固化**：MGLRU 开/关、zswap 开/关、PSI 开启、demotion 按机型开启，形成四组 A/B 基线。
2. **观测统一**：PSI + `/proc/vmstat` + memcg `memory.stat` + zram `mm_stat`/`bd_stat` 一套指标，避免各说各话。
3. **场景化（AgentENV/沙箱）**：`process_madvise(MADV_PAGEOUT)` + zram + `backing_dev` 回写，按 §4.2 的顺序与 §4.4 的判据执行。
4. **风险**：MGLRU 与 zram_reclaim 互斥；zswap 与 zram 叠加要单独测 CPU 开销。

### 9.3 H2（3-12 个月）：对齐主线 7.x

- **已核实**：7.4-rc2 含 `bpf_proactive_reclaim()`（commit `48ea1ba704aa3a9975e917411ee05bbe67d65744`，2026-09-21），仅 `BPF_PROG_TYPE_SYSCALL` 可用，单次回收量上限 `MEMCG_CHARGE_BATCH`；意义是把「何时回收、回收多狠」从硬编码阈值变成 BPF 策略。
- **方向性（未逐条核对版本）**：mTHP 全链路（fault/swapout/khugepaged）、large folio swap、DAMON 增强（自动调优/统计）、memory tiering 完善（demotion 策略、CXL 动态容量）、swap 前端分层。
- **做法**：先做 capability matrix（7.3 vs 7.4 对照），再决定「回移」还是「升级内核」。

### 9.4 H3（12 个月+）：从开关到策略编排

- 闭环：PSI/refault 信号 → tier-aware reclaim → pageout/writeback → 迁移，形成反馈控制。
- 可编程：BPF kfunc（7.4-rc2 已起步）+ DAMON scheme。
- 虚拟化/沙箱：guest_memfd、per-VM tiering、`process_madvise` 批量接口。
- 可观测性：统一 mm_stat/bd_stat/PSI 的 per-cgroup 视图。

### 9.5 验证方法与风险

- 每条 roadmap 主张必须给：版本、提交/文档链接、配置、运行时证据；只写「方向」的必须标注「未核实」。
- 本报告已核实的只有：本树源码事实、7.4-rc2 的 `bpf_proactive_reclaim` commit、kernel.org 版本号。
- 未核实项：mTHP/large folio/DAMON 的具体合入版本与行为边界。

## 10. 参考

- kernel.org releases：https://www.kernel.org/
- `bpf_proactive_reclaim` commit：https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=48ea1ba704aa3a9975e917411ee05bbe67d65744
- 本树文档：`Documentation/admin-guide/mm/multigen_lru.rst`、`Documentation/admin-guide/mm/zswap.rst`、`Documentation/admin-guide/mm/zram_reclaim.rst`、`Documentation/admin-guide/mm/transhuge.rst`、`Documentation/admin-guide/cgroup-v2.rst`

## 11. 更新日志

- 2026-11-15：初版；基于 OLK-6.6 HEAD `8003dfcd` 与 kernel.org 7.4-rc2 基线。
