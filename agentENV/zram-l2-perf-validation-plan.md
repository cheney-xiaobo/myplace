# zram / CXL-L2 沙箱内存三态性能验证方案（Phase 0）

> 目标：在**同一负载、同一换出页集合、同一判据**下，比较三种沙箱内存状态：
> A 普通内存（全驻留） / B 压缩到 zram（L1） / C 被 zram writeback 淘汰到 CXL backing（L2）。
>
> 基线：`vpause-zram-cxl-l2-design.md` §7.4 的分层消融法。本方案补齐三件事：
> **状态怎么确定性地造出来、结果怎么归因、哪些前提会直接推翻结论**。
>
> 环境：openEuler OLK-6.6（内核树 `/Users/cheney/work/code/ub/kernel`）+ AgentENV vpause-v2。
> 本文所有"运行时数字"均待 Linux 测试机实测；代码事实已按行号核对（2026-11-12）。

---

## 0. 六个代码事实：直接改变实验设计

| # | 事实 | 证据 | 对方案的影响 |
| --- | --- | --- | --- |
| 1 | `CONFIG_ZRAM_MEMORY_TRACKING` 未开启 | `arch/x86/configs/openeuler_defconfig:2419`、`arch/arm64/configs/openeuler_defconfig:2477` | **按年龄标记 idle 不可用**（`echo 3600 > idle` 返回 `EINVAL`），只有 `echo all > idle`。设计 §4.3 的"每轮 pause 只淘汰冷页"在默认内核做不到 |
| 2 | `backing_dev` 必须在 `disksize` 之前写入，初始化后再写返回 `EBUSY` | `drivers/block/zram/zram_drv.c:460-490` | 设计 §12 验证脚本（`vpause-zram-cxl-l2-design.md:515-517`）先 `disksize` 后 `backing_dev`，会假失败；槽位构造顺序必须反过来 |
| 3 | `bd_stat` 单位是 **4K bytes**，不是 KiB | `drivers/block/zram/zram_drv.c:1191-1203`、`Documentation/admin-guide/blockdev/zram.rst:279-283` | 设计 §9.1 的 `$1/1024` 按 MiB 报数，**低估 4 倍**。正确换算：字节 = 值 × 4096，MiB = 值 / 256 |
| 4 | writeback 是**每页一个 4K bio、`submit_bio_wait` 同步、单线程串行** | `drivers/block/zram/zram_drv.c:601-742`（`submit_bio_wait` 在 `:724`） | 吞吐上限 = 4K 同步写延迟的倒数。设计 §7.4 的"写回吞吐 ≥ NVMe 顺序写带宽"是错的 KPI，应对标 fio 4K QD1 同步写 |
| 5 | 回写后 `pages_stored` 会重新 +1，`mm_stat` 第一列**包含已在 backing 的页** | `drivers/block/zram/zram_drv.c:765`、`:1171`、`:1323` | L1 驻留页 ≠ `orig_data_size`；必须减去 backing 页。状态判据见 §1.3 |
| 6 | 首次 pause 的 dirty window 等于 union，**没有可 advise 的页**；`track_dirty_pages=false` 时才走全量 region | `src/sandbox/firecracker/pageout.rs:472-500`、`src/sandbox/firecracker/sandbox.rs:788-900` | 新沙箱第一次 pause 可能"零换出"。必须用**双 pause 协议**或实验期关脏页跟踪，否则 B/C 态根本造不出来 |

> 第 1、4、6 条不是实现细节，是会"让实验测出漂亮但错误结论"的前提：
> - 若默认内核上跑 C 组，writeback 只能全量，得到的是"**全冷 L2**"上界，不是"冷页分层"的真实形态；
> - 若按设计文档的 KPI 测 writeback，会拿顺序带宽去否定一个天生 4K 同步的路径；
> - 若第一次 pause 就测，看到 `VmSwap≈0` 会误判 zram 没生效。

---

## 1. 三组定义、构造与判据

### 1.1 组定义

| 组 | 状态 | 构造要点 | 用途 |
| --- | --- | --- | --- |
| **A0** | 正常内存、不暂停 | 沙箱稳定运行，重放工作集 | 噪声地板：无缺页时每页首触成本 |
| **A** | 暂停/恢复，但无 swap | `swapoff -a`，`v_pause` 开启；页面全驻留 | 隔离"vCPU 停/启 + 客户端重连"开销，得到纯 A 基线 |
| **B** | 压缩到 zram（L1） | zram swap，pageout，**不回写** | 每次首触 = L1 解压 |
| **C** | 被 zram 淘汰到 L2 | 在 B 状态后，冻结 vCPU，`echo all > idle` + `writeback idle` | 每次首触 = L2 读 + 解压 |
| **C-brd** | C 的介质消融 | backing=`/dev/ram0`（本地 DRAM 伪块设备） | 分离"块设备/swap 路径"与"CXL 链路惩罚" |
| **C-nvme** | C 的介质消融 | backing=独立 NVMe 分区或 dm-linear 窗口（勿用有数据的分区） | 给 L2 一个现实对照 |
| **C-cold** | 真·冷页分层（可选） | 需重编内核开 `CONFIG_ZRAM_MEMORY_TRACKING=y`，`echo 3600 > idle` | 验证设计本意的"只淘汰冷页"；Phase 1 再做 |

### 1.2 状态构造手册（Linux，root）

**前置（对 A/B/C 全部适用）：** 测试 server 必须开 `[v_pause].enabled = true`、`stop_vcpus = true`
（`config/default.toml:368-382`；默认 `enabled=false`，此时 `aenv pause` 走 heavy snapshot 而不是 vPause）。
用隔离的 `AENV_TEST_STATE_DIR` 起 server，避免影响线上状态；`memory_snapshot.track_dirty_pages`
按 §6 第 7 条在整组内固定。

**A：**
```bash
sudo bash scripts/zram-swap-teardown.sh --remove   # 或 swapoff -a
swapon --show                                      # 期望为空
grep -E 'VmRSS|VmSwap' /proc/<fc_pid>/status       # VmSwap == 0
```

**B：**
```bash
sudo bash scripts/zram-swap-setup.sh --per-node 1 --size-mib 2048
swapon --show
cat /sys/block/zram0/backing_dev 2>/dev/null || echo "no backing"   # 必须为空
```

**C（当前 setup 脚本不支持 `--backing-dev`，见 `scripts/zram-swap-setup.sh:21-28`；手工或按设计 §11.1 扩展）：**
```bash
# 0) 保证 zram0 空闲：先 teardown/reset；模块未加载则 modprobe zram num_devices=1
#    （模块已加载但没有设备时用 /sys/class/zram-control/hot_add）
# 1) backing 设备：dm-linear 窗口（CXL/pmem），或 /dev/ram0、独立 NVMe 分区
sudo dmsetup create l2slot0 --table "0 $((2048*1024*2)) linear /dev/pmem_cxl0 0"
# 2) 关 zswap（手工路径不能依赖 setup 脚本；模块未加载时可忽略）
[ -w /sys/module/zswap/parameters/enabled ] && echo 0 > /sys/module/zswap/parameters/enabled
# 3) 顺序不能反：backing_dev / comp_algorithm 都必须在 disksize 之前（否则 EBUSY）
echo /dev/mapper/l2slot0 > /sys/block/zram0/backing_dev
echo lz4                 > /sys/block/zram0/comp_algorithm
echo 2048M               > /sys/block/zram0/disksize
mkswap /dev/zram0 && swapon -p 100 /dev/zram0
# 4) v_pause + pageout 完成、vCPU 已停后：
echo all > /sys/block/zram0/idle
sync
/usr/bin/time -f 'writeback_s=%e' sh -c 'echo idle > /sys/block/zram0/writeback'
cat /sys/block/zram0/bd_stat
```
实践约束：backing 应 ≥ `disksize`，否则回写可能中途 `ENOSPC`（内核按 backing 的 `nr_pages` 分配 offset，`zram_drv.c:490-505`）；两台 zram 共享同一 backing 会互相踩 offset，**必须一槽一 backing**（设计 §3.3 已指出）。

### 1.3 三态判据（统一口径）

```bash
z=/sys/block/zram0
ps=$(getconf PAGE_SIZE)
orig=$(awk '{print $1}' "$z/mm_stat")   # 字节，含已回写页
bd=$(awk   '{print $1}' "$z/bd_stat")   # 4K 块数（不是 KiB）
same=$(awk '{print $6}' "$z/mm_stat")   # 同内容页的“页数”，不是字节
bd_bytes=$((bd*4096))
same_bytes=$((same*ps))
l1_bytes=$((orig-bd_bytes))             # L1 口径（含 SAME 页的原始大小）
l1_real=$((orig-bd_bytes-same_bytes))   # 真正占 zsmalloc 的 L1
echo "total=${orig}B  L2=${bd_bytes}B  L1=${l1_bytes}B (same=${same_bytes}B)"
```

| 组 | 必须同时满足 |
| --- | --- |
| A | `/proc/<fc>/status VmSwap == 0`；`swapon --show` 无 zram；guest 重放 `minor faults ≈ 0` |
| B | `VmSwap ≈ orig`；`bd_count == 0`；`bd_reads == 0`；`mm_stat.mem_used_total > 0` |
| C | `bd_count ≈ (orig - same_bytes)/4096`（容差 ±2%）；`bd_reads == 0`（回写前）；`l1_real ≈ 0`；`bd_writes ≈ bd_count` |
| C（resume 后） | `bd_reads` 增量 ≈ 首次触碰页数 × (PAGE_SIZE/4096)（随机模式）；顺序模式还会叠加 readahead 预取页，需按缺页/预取拆分 |

> 不能写成"恒有 `orig/4096 == bd_count`"：回写会跳过 `ZRAM_SAME` 页（`zram_drv.c:683-686`），
> 这类零页留在 zram 但永远不进 backing，所以等式必须扣掉 `same_pages`。
> 也不要拿 `bd_count` 判断"当前还在 L2"——页被读回后 backing 副本保留、计数不回落（设计 §4.3 的描述正确）。
> 实验用非零数据时 `same_pages` 应接近 0；若明显非零，先查初始化时是否留下了零页。

---

## 2. 先决校准（L0，半天）

目的：拿到"介质裸能力"基线，C 组结果才能归因到 CXL，而不是 swap 路径。

```bash
# 同一台机、同一容量：CXL fsdax 窗口 / brd / NVMe
for dev in /dev/mapper/l2slot0 /dev/ram0 /dev/nvme0n1pX; do
  fio --name=4k-randread --filename=$dev --direct=1 --rw=randread --bs=4k \
      --iodepth=1 --numjobs=1 --runtime=60 --time_based --group_reporting
  fio --name=4k-randread-qd32 --filename=$dev --direct=1 --rw=randread --bs=4k \
      --iodepth=32 --numjobs=4 --runtime=60 --time_based --group_reporting
  fio --name=4k-randwrite-qd1 --filename=$dev --direct=1 --rw=randwrite --bs=4k \
      --iodepth=1 --numjobs=1 --runtime=60 --time_based --group_reporting
  fio --name=1m-seqwrite --filename=$dev --direct=1 --rw=write --bs=1M \
      --iodepth=16 --numjobs=1 --runtime=60 --time_based --group_reporting
done
```

- `CXL − brd` = 纯链路惩罚；
- 4K QD1 随机写 = **writeback 的现实上限**（zram 回写是逐页同步）；
- 1M 顺序写只作参照，不作为 C 组 KPI。

---

## 3. 主实验矩阵

### 3.1 单沙箱（Phase 1，先跑）

| 因子 | 取值 |
| --- | --- |
| 状态 | A0 / A / B / C / C-brd / C-nvme |
| 重放模式 | 随机全量（固定种子排列）/ 顺序全量 / 混合 70/30 |
| 数据内容 | 可压缩结构化非零数据（主）/ `urandom` 不可压缩（敏感性） |
| 重复 | 5 次，组序随机化，每次新沙箱 |
| 工作集 | guest RAM 的 ~80%（例如 1 GiB guest → 800 MiB） |

最小闭环：`A / B / C × 随机 / 顺序 × 5 次 = 30 run`（每 run 2–5 min）。
`C-brd / C-nvme` 各 5 次补归因；`A0` 5 次定噪声地板。

### 3.2 并发（Phase 1b）

只在 B / C 上跑 `N ∈ {1,4,8,16}`，每档 3–5 次；记录聚合 `bd_reads/s`、p99、PSI、CXL 链路是否饱和（`perf` uncore 或 `dmesg` AER/降速）。

### 3.3 必测的"负对照"

- 工作集 < 实际换出量（例如只触碰 100 MiB，pageout 却全量）：三组差异应≈0，用来证明"差异只来自被换出的页"；
- 回写后不 resume（只测 writeback 吞吐与 pause 延长），避免把暂停成本算进换回延迟。

---

## 4. 驱动与采集

### 4.1 端到端流程（每个 run）

```text
1. 组环境就绪（§1.2；[v_pause].enabled=true），启动沙箱，等待 envd ready
2. aenv exec <id> -- bash -c 'nohup /root/bench ... >/root/bench.log 2>&1 &'
   （bench：初始化工作集 → 写 READY → sleep 10s → 重放）
3. 轮询 guest READY 标记（bench 已初始化并 fdatasync）
4. 状态构造，整组固定一种协议：
   a) 隔离模式（track_dirty_pages=false）：aenv pause → 首次 pause 即全量 pageout
   b) 生产模式（track_dirty_pages=true）：aenv pause（只清窗口，victim 为空）
      → aenv resume → 静置 ≥2s（guest 不写）→ aenv pause（第二次，真正 pageout）
5. host 轮询：pageout queue_depth==0 且 VmSwap 连续 3 次采样稳定；
   实际换出比例 <90% 的 run 记录比例、不并入主结论（§6 第 8 条）
6. C 组：vCPU 停住时执行 idle+writeback，计时；读 bd_stat/mm_stat
7. aenv resume <id>；bench 的 sleep 到期（或已在暂停中到期）后开始重放
8. guest 重放：每页只碰一次，记录每页首触 ns；结束写 CSV + 校验和
9. 拉回 /root/bench.csv、host 侧 zram/vmstat/PSI/eBPF 数据
10. teardown/重置，下一 run
```

**关键点：**
- bench 必须 `nohup` 分离，因为 pause 会摘路由，前台 exec 会话可能断；
- 重放起点由 guest 自己记（`t_replay_start`），warm-up 总量与 host 的 `VmSwap` 曲线互为校验；
- **sleep 只负责给 host 编排留窗口，不承担"脏窗口老化"**：首查询的 window 恒等于 union（事实 6），
  多睡一会儿不会让第一次 pause 有 victim；要靠"隔离模式"或"两次 pause"解决；
- 隔离模式与生产模式的数据**不能混在一组**：两者的 victim 集（全量 vs 累积脏页）不同；
- 每次 run 用**新沙箱**，避免 zram 槽位、guest 页状态跨 run 复用。

### 4.2 guest bench 规格（交给 DevCI 实现）

- 匿名 mmap 工作集，逐页写入**非零、有差异**的 pattern（避免 `ZRAM_SAME` 零页/同页被回写跳过）；
- 记录 CRC/SHA 校验和（A/B/C 必须一致）；
- 随机重放用固定种子 LCG 排列；顺序重放按地址升序；
- 每页：`t0=clock_gettime(MONOTONIC)` → 读 1 字节（`volatile`/`asm` 防优化）→ `t1`，写 CSV；
- 只统计**首触**；同一页不重复触碰；
- READY 前 `fdatasync` 结果文件；READY 后 sleep 10s（给 host 编排；若在暂停中到期，resume 后立即重放）；
- 结束时输出 `warmup_total_ns`、`pages`、`checksum`；
- 额外输出 `getrusage` 的 minor faults，用于与 host `do_swap_page` 次数交叉验证。

### 4.3 host 侧采集

**状态/计数（warm-up 窗口 100 ms 采样 VmSwap/PSI；其余 5 s 采样）：**
```bash
grep -E 'VmRSS|VmSwap' /proc/<fc_pid>/status
cat /sys/block/zram*/mm_stat          # orig/compr/mem_used/same/huge
cat /sys/block/zram*/bd_stat          # 4K 块
cat /proc/vmstat | grep -E 'pswpin|pswpout'
cat /proc/pressure/memory             # some/full
```
**per-fault 延迟（eBPF，机制层）：**
```bash
# 先确认符号可见：grep do_swap_page /sys/kernel/debug/tracing/available_filter_functions
# 用 pid 过滤（FC 的 vCPU 线程名可能不是 firecracker，按 comm 会漏采）
bpftrace -e '
kprobe:do_swap_page /pid == $1/ { @s[tid] = nsecs; }
kretprobe:do_swap_page /@s[tid]/ {
  $d = nsecs - @s[tid]; delete(@s[tid]);
  @us = hist($d / 1000);
}' <fc_pid>
```
注意：`do_swap_page` 也会处理 swapcache 命中（readahead 预取过的页），所以顺序模式下直方图会有低延迟峰；这正是要观察的"readahead 摊销"，不要当成噪声剔除。

**介质层：** `biolatency -d /dev/mapper/l2slot0 60`（默认 µs，别加 `-m`）、`biosnoop`（4K 读的 p50/p99）；zram 解压可用 `funclatency zram_read_from_zspool`（**先验证符号未被内联**，否则退到 `zram_read_page`）。

**CPU/开销：** `perf stat -p <fc_pid> -e cycles,instructions,cache-misses` 跑 warm-up 窗口；`/usr/bin/time` 量 writeback；`agentenv_v_pause_pageout_*` 指标看 target/advised/job_duration/queue_depth（`src/sandbox/firecracker/pageout.rs:126-160`）。

### 4.4 归因规则（避免"看起来像"）

| 观测 | 归因 |
| --- | --- |
| `do_swap_page` p50 与 `biolatency` 4K 读 p50 接近 | L2 命中，CXL 链路是主项 |
| `do_swap_page` p50 ≈ `zram_read_from_zspool` p50 + 固定缺页成本 | L1 命中，解压是主项 |
| `bd_reads` 增量 ≈ 0 但 guest 有缺页 | 走的是 swapcache 命中（readahead 预取），不是 L1/L2 |
| `bd_reads` 增量 < 触碰页数 | C 组回写不完整（SAME 页/ENOSPC/写失败），报告实际 L2 占比 |
| writeback 期间 FC 进程 `VmSwap` 不变、`bd_writes` 增长 | 正常：回写不改变 swap 记账，只搬介质 |

---

## 5. 判定门槛（写在跑之前，防止事后找理由）

| KPI | 门槛 | 依据 |
| --- | --- | --- |
| L1 每页首触 p50 / p99 | ≤ 5 µs / ≤ 15 µs | 设计估计 1–3 µs + 缺页固定成本 |
| L2 每页首触 p50 / p99 | ≤ 12 µs / ≤ 40 µs | 设计估计 4–10 µs + 余量 |
| L2 / L1 p50 比值 | ≤ 5× | 设计"2–4×"，5× 为红线 |
| L2 vs NVMe backing p50 | ≥ 3× 更优 | 设计"好 ~7 倍"，3× 为最低可接受 |
| writeback 吞吐 | ≥ 同介质 fio 4K QD1 同步写的 80% | 路径同为逐页 bio；**不拿顺序带宽做标准** |
| pause 延长（writeback） | ≤ 1.5 s / GiB | 4K 同步 2–6 µs/页 → 800 MiB 约 0.4–1.2 s；超过说明逐页同步不可接受 |
| resume-to-ready | 不劣于 B 组 +5% | 设计"L1 兜住热路径" |
| warm-up 总量 | C ≤ 3.5× B，且 ≤ NVMe 方案的 1/5 | 摊销模型 7 µs vs 2 µs |
| 稳态吞吐（warm-up 后） | 与 A 差 ≤5% | 负对照：差异只应出现在首触 |
| 数据完整性 | 校验和 100% 一致；无 SIGBUS/OOM | 淘汰路径不能静默损坏 |
| 并发 p99（N=8） | ≤ 2× N=1 | CXL 链路不饱和的判据 |

任一不达标 → 先按 §6 排查混淆，再决定"设计问题"还是"环境问题"。

---

## 6. 混淆因素清单（必须固定/记录）

1. **`ZRAM_MEMORY_TRACKING` 的运行时状态**：`zcat /proc/config.gz | grep ZRAM` 或 `/boot/config-$(uname -r)`，不要只看 defconfig；默认关闭意味着 C 组只能全量回写。
2. **zswap 必须关**（脚本会关；手工 C 组要自己 `echo 0 > /sys/module/zswap/parameters/enabled`），否则双重压缩。
3. **zram 设备数与优先级在 B/C 必须一致**（脚本 `--per-node/--size-mib/--priority`；C 手工时对齐），否则 swap 轮转不同。
   另外，`scripts/zram-swap-setup.sh` 会把已有的低优先级 swapfile 保留为溢出层（脚本注释明确如此）；
   实验前应 `swapoff` 该磁盘 swap，否则 zram 满/回写失败时页会落到 L3，C 组混入磁盘层。
4. **backing 独占、容量 ≥ disksize**；共用 backing 会静默踩 offset。
5. **THP/KSM**：guest 与 host 的 THP 状态、`AnonHugePages`、KSM 开关全部记录并跨组一致；4K host 页（arm64 defconfig `ARM64_4K_PAGES=y`）下 guest 2M THP 会改变缺页/换出粒度。
6. **NUMA**：FC 进程与 CXL 窗口同节点；zsmalloc 跟随换出上下文分配，记录 `numastat`/mempolicy。
7. **首 pause 空转**（事实 6）：二选一，整组固定
   - **隔离模式**：`AGENTENV_MEMORY_SNAPSHOT_TRACK_DIRTY_PAGES=false` → 全量 region，第一次 pause 就 pageout；
   - **生产模式**：pause#1（只清窗口）→ resume → 静置 → pause#2（真正 pageout）。
   两种协议不要混在一组数据里。
8. **pageout 是 best-effort**：记录 `target_bytes` / `advised_bytes` / `VmSwap` 三者比例；实际换出比例 <90% 的 run 单列，不并入主结论。
9. **swap readahead**：`/proc/sys/vm/page-cluster` 固定并记录；顺序/随机模式必须分开报告，不能合并成一条曲线。
10. **`ZRAM_SAME` 零页**：回写会跳过（`zram_drv.c:683-686`），用非零数据；同时报告 `mm_stat.same_pages`。
11. **eBPF 开销**：instrumented / uninstrumented 各跑一次，确认 p50 偏移 <5%；否则用采样。
12. **频率/温度/邻居**：测试机独占、governor=performance、组序随机、每 run 前 `swapoff` 重置；并发测试记录 PSI。
13. **`bd_stat` 单位**：报告统一写"4K 块"和字节两个口径，避免把 2048 当 2 MiB（实际 8 MiB）。
14. **页大小**：host `getconf PAGE_SIZE`；若为 64K，`bd_stat`/`mm_stat`/guest 缺页的关系按 §1.3 换算，不能按 4K 页直接报"页数"。

---

## 7. 常见翻车场景（预演）

1. **测出 B/C 完全一样**：先看 `bd_reads` 是否为 0。若 C 组 `bd_reads>0` 才说明真的从 L2 读；若 `bd_count==0`，是 writeback 没生效（多半是 `backing_dev` 没设或 `idle` 标记失败）。
2. **测出 C 比 NVMe 还慢**：先看 `biolatency`，若 4K 读 p50 已高于 fio L0 的 2–3 倍，多半是 dm-linear/CXL 窗口未对齐或 fsdax 走了非 DAX 路径。
3. **pause 时间异常长**：writeback 逐页同步 + 大工作集；用 `bd_writes/s` 与 `time` 对账，符合 §5 的 s/GiB 预算。
4. **resume 后很快又卡**：顺序重放 + readahead 把大量页提前换入，`VmSwap` 曲线出现尖峰；分开报告"缺页数"和"预取页数"。
5. **同一配置两次结果差 2×**：检查是否复用了沙箱（zram 槽位/guest 页状态未重置）或未做组序随机化。

---

## 8. 执行顺序与交付物

```text
Phase 0a（环境，1 天）
  ├─ 运行时 config 核对（ZRAM_WRITEBACK / MEMORY_TRACKING / page size）
  ├─ B/C 状态构造脚本与 §1.3 判据跑通
  ├─ fio L0（CXL / brd / NVMe）
  └─ 双 pause 协议 vs track_dirty_pages=false 的对比验证

Phase 0b（单沙箱三态，2 天）
  ├─ A0/A/B/C × 随机/顺序 × 5 次
  ├─ C-brd / C-nvme 各 5 次（归因）
  └─ 产出：每页首触 CDF、warm-up 曲线、writeback 吞吐

Phase 0c（并发与预算，1–2 天）
  ├─ B/C × N=1/4/8/16
  ├─ pause 延长 / 回写吞吐 / PSI
  └─ 产出：回读率 bd_reads/bd_writes、并发拐点

Phase 1（可选）
  └─ 开 CONFIG_ZRAM_MEMORY_TRACKING 重编内核，验证"只淘汰冷页"（C-cold）
```

**交付物：**
- `bench.csv` 字段：`run_id, arm, pattern, data, pages, swapped_bytes, checksum, p50_ns, p90_ns, p99_ns, warmup_total_ns, resume_ms, bd_count, bd_reads, bd_writes, mem_used, rss, vmswap, psi_some, psi_full, writeback_s`；
- 图 1：A/B/C 每页首触 CDF（随机与顺序分面）；
- 图 2：resume 后 `VmSwap` 归零曲线（A/B/C 叠加）；
- 图 3：并发 N vs 聚合 pages/s 与 p99；
- 结论表：§5 每项 KPI 的达标/不达标与置信区间；
- 回写设计文档 §7.1 的延迟表：用实测替换 1–3 / 4–10 µs 的估计，并标注 tracking 开/关两种形态。

---

## 9. 未验证项（Phase 0 必须先坐实）

1. 目标机运行时 `CONFIG_ZRAM_MEMORY_TRACKING` 是否确实关闭（defconfig ≠ 运行内核）。
2. `MADV_PAGEOUT` 在该沙箱 guest RAM 映射类型（anon / memfd）上的实际换出比例；按 §6 第 8 条量化。
3. CXL fsdax 窗口 + dm-linear 下的 `/dev/mapper/l2slot0` 是否符合"块设备语义"（fio L0 会给出答案）。
4. `do_swap_page` / `zram_read_from_zspool` 的 kprobe 符号是否可见（可能被内联）。
5. writeback 逐页同步在目标 CXL 介质上的真实 pages/s（决定 §5 的门槛是否要放宽，以及是否需要"批量回写"补丁）。
6. 64K 页平台（若存在）下 `bd_stat` 与 guest 缺页的换算是否按 §1.3 成立。

---

## 附：一句话

**先把"第一次 pause 不换出、回写只有全量、`bd_stat` 是 4K 块、writeback 是 4K 同步"四个事实钉死，再谈三态性能；否则测出来的不是 CXL L2 的能力，而是测量误差。**
