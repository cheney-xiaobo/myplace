# vPause + zram 压缩 + CXL 动态借用内存（L2）整套设计

> 状态：设计提案（基于 vpause-v2 分支现状分析）
> 范围：AgentENV 沙箱内存压缩分层、CXL 借用内存后端、扩缩容机制、触发设计、性能模型、成本决策与落地路线

---

## 1. 背景与目标

AgentENV vpause-v2 的 vPause 机制在显式 Pause 时停止 vCPU，并把 guest RAM 经
`process_madvise(MADV_PAGEOUT)` 推入宿主 swap 子系统，实现"沙箱暂停态的内存压缩驻留"。
当前方案要求运维预先提供 swap（脚本搭建 zram 主层 + swapfile 溢出层）。

本设计在此之上引入第三层：**用 CXL 借用内存（远端内存池）做 zram 的 writeback 后端（L2）**，
并包含借用内存的动态扩缩容（槽位化）。目标：

- 暂停态沙箱的 DRAM 占用进一步压缩（冷页沉到 L2，L1 只留活跃工作集）
- resume 热路径不受损（L1 本地解压兜底）
- 借用内存可扩可缩，按需从 CXL 池借还
- 故障半径收敛到单沙箱（SIGBUS 走既有 terminal teardown），整机不受威胁
- **内核零改动、AgentENV Rust 核心零改动**——全部基于 mainline 现成机制

---

## 2. 总体架构

```text
AgentENV vPause (pageout.rs 零改动)                编排层（新增）                内核（零改动）
┌────────────────────────┐          ┌──────────────────────────┐
│ v_pause → MADV_PAGEOUT ┼──→ swap ─┤ L1 zram 槽位池            │  zram/zsmalloc 现成
│ v_resume ← 缺页换回      │          │  (本地DRAM, prio100)     │  writeback 现成
│ (内核透明 demand paging) │          │   ↓ idle+writeback       │
└────────────────────────┘          │ L2 backing 槽位池         │  CXL fsdax 现成
                                    │  (每槽独占 CXL 区间)      │  swapoff 迁移 现成
   CXL 借用内存池 ── 区间分配 ──→     │ L3 磁盘 swapfile (兜底)   │
   (per-host 借用记账)               └──────────────────────────┘
```

三层职责：

| 层 | 介质 | 优先级 | 承载内容 | 单页换回成本（估算） |
| --- | --- | --- | --- | --- |
| L1 | 本地 DRAM（zram 压缩） | 100 | 活跃轮次工作集 + 上轮冷页 | 1-3 µs（纯解压） |
| L2 | CXL fsdax（zram writeback） | L1 的 backing | 多轮未访的最冷页 | 4-10 µs（CXL 读 + 解压） |
| L3 | 磁盘 swapfile | 最低 | 兜底/归档 | 50-100+ µs |

---

## 3. 设计原则与关键决策

### 3.1 为什么 L2 选"zram writeback → CXL 块设备"，而不是其他形态

| 路径 | 实现层 | 内核改动 | 故障域 | 结论 |
| --- | --- | --- | --- | --- |
| A. zram 压缩数据本体放 CXL（node-pinned zsmalloc） | 内核 patch | 大 | **host panic 级**（不可迁移内核页 + MCE on Kernel data） | 否决 |
| **B. zram writeback → CXL 块设备** | 用户态脚本 | **零** | 沙箱级（swap-in EIO → SIGBUS） | **采用** |
| C. CXL swapfile 独立溢出层 | 用户态脚本 | 零 | 沙箱级 | B 的退化版（无压缩落盘） |

关键论据：

1. **zram 的数据落位不可控**：zsmalloc 分配跟随换出上下文（pageout worker / kswapd）
   的 NUMA policy，不是设备属性；要落 CXL 节点需把 CXL 在线为 ZONE_NORMAL 并控制
   kswapd 的 mempolicy——不可工程化。若只在线为 ZONE_MOVABLE，内核分配也不会用它。
2. **A 路径故障域**：zsmalloc 页是不可迁移内核页，落在 CXL 节点后节点无法优雅
   offline/hot-remove（池子无法回收容量）；坏页是 `memory_failure`(Kernel data) → panic 级。
3. **B 路径故障链**：CXL 块设备读 EIO → swap-in 失败 → FC 进程访问该 guest 页
   SIGBUS → 仅该沙箱死 → 走既有 terminal-error teardown。宿主机无恙，池可 drain。
4. **与 vPause 生命周期自洽**：vPause 本来就 not crash-recoverable，FC 进程死亡与
   "FC SIGBUS" 走同一条 teardown 路，无需新增恢复逻辑。

### 3.2 CXL 形态决策

```text
CXL type-3 内存 → cxl region → fsdax → /dev/pmem_cxlN（块设备形态）
⚠ 绝不用 kmem 在线为 ZONE_NORMAL NUMA 节点——杜绝内核分配误落 CXL，保住"可回收"属性
```

writeback 走 bio → pmem 驱动路径（非 DAX 直映射），延迟特征即块设备语义下的
CXL.mem 访问，性能验证方法按块设备 profile 做。

### 3.3 核心抽象：槽位（Slot）

zram 的 `disksize`/`backing_dev` 初始化后静态（改 = reset = 丢数据）。因此**扩缩容的
正确单位是"槽位"而非"设备"**：

```text
一个 L2 槽位 = 1 台 zram 设备 + 1 段独占 CXL 区间（dm-linear 子窗口）
             + 固定容量（如 8GB）+ 生命周期状态机

槽位状态机:
Empty → Provisioning → Active ⇄ Draining → Retired → Empty
(建zram+挂backing  (承载swap,    (swapoff排空,    (reset,
 +swapon)          周期writeback) 页迁往他槽)    归还CXL区间)
```

与 WarmPool 的 Fill/Drain 槽位模型同构，爆炸半径以槽为单位。与 AgentENV 现有
per-node 多 zram 设备的脚本设计自然衔接（每 zram 配对独立 CXL 区间）。

**强制 per-slot 独占 backing**：`backing_dev_store` 用 `filp_open` 挂载、无独占
claim（基于源码级分析，待运行时坐实，见 §12）；两台 zram 共享同一 backing 会各自
维护覆盖整个设备的独立 offset 位图 → 静默互相踩踏。设计上禁止共享。

---

## 4. 关键机制：各层如何工作

### 4.1 vPause 换出（现状，零改动）

```text
v_pause (sandbox.rs)
 ├─ stop_vcpus=true → PATCH /vm {"state":"Paused"}（FC 进程活着，只停 vCPU）
 ├─ pageout_pidfd() + pageout_targets()
 │    ├─ 脏页驱动路径: GET /vm/dirty-memory-ranges
 │    │    victims = 累计脏页 union − balloon 丢弃掩码 − 当前脏窗口
 │    │    （窗口排除：resume 秒级重访，换出只会 swap ping-pong）
 │    └─ 兜底路径: 全量 guest-memory-regions
 └─ PageoutControl::spawn_job → FIFO 队列 + 8 worker
      → process_madvise(pidfd, iovec, MADV_PAGEOUT) 按 64MiB chunk round-robin
```

- 需要 `CAP_SYS_NICE` + `CAP_SYS_PTRACE`（scoped capabilities）
- best-effort：失败不阻塞 Pause；resume/teardown 只 bump generation 在 chunk 边界取消
- zram 成为主 swap 层后被内核 swap 子系统选中完成压缩（用户态不碰 zram 接口）

### 4.2 L1：zram 压缩层（脚本搭建，vpause-v2 现状）

```bash
modprobe zram num_devices=N          # 或 /sys/class/zram-control/hot_add
echo <bytes> > /sys/block/zramN/disksize
echo lz4 > /sys/block/zramN/comp_algorithm
swapon -p 100 /dev/zramN
```

- per-NUMA 多设备同优先级 → 消除单设备 swap-slot/锁串行化
- **必须禁用 zswap**（6.6 内核会给 zram 再压一遍，双重压缩）
- zram 惰性分配：`disksize` 是容量上限不是预分配，不用不占内存

### 4.3 L2：writeback 淘汰策略与触发

mainline zram writeback 机制（无自动 LRU，是"年龄标记 + 类型筛选 + 外部触发"）：

```bash
echo /dev/mapper/l2slotN > /sys/block/zramN/backing_dev   # 挂 CXL 区间
echo 3600 > /sys/block/zramN/idle        # 标记 3600s 未访问的 slot 为 idle（aged LRU）
echo idle > /sys/block/zramN/writeback   # 写回 idle 页（另有 huge/incompr 模式）
```

行为细节（影响设计）：

- 写回后 zsmalloc 内存立刻释放——省 DRAM 的时点
- 回读后 backing 副本保留（重复 swap-in 不重写，再次 idle 也无需重写回）
- slot 被重写 → backing 副本失效，新压缩数据回 L1
- slot 释放 → backing offset 归还空闲位图，可选 TRIM
- **vPause 的 pause 轮次是天然 writeback 驱动点**：上轮冷页在下一轮 pause 间隙
  `idle + writeback`，与"L1 只保留活跃工作集"的设计对齐

### 4.4 换回：内核统一处理，AgentENV 零参与

```text
v_resume: cancel pageout + resume vCPU；大部分 guest 页仍驻留 L1/L2
guest 触碰被换出页 → KVM 建立 host PTE → host 缺页（FC 进程上下文）
→ do_swap_page → L1 命中（本地解压）或 L2 命中（CXL 读 + 解压）→ 映射 → vCPU 继续
```

逐页、惰性、按需 demand paging；换入延迟直接体现在 guest 执行耗时里。
AgentENV 对换回侧不记录任何指标（只能从 vmstat si / VmSwap / bd_stat 反推）。
可选扩展（见 §11）：resume 主动 prefetch（MADV_WILLNEED + victim 账本）。

---

## 5. 借用内存扩缩容设计

### 5.1 扩容流程（从池借用）

```text
触发见 §6。前置：池侧区间分配器批准新区间。

Provisioning（分钟级，零干扰）:
1. dm-linear 在 CXL bdev 上切一段新区间 → /dev/mapper/l2slotN
2. zram 起一台新设备 → 挂 backing_dev → swapon -p 100
   （与存量设备同优先级，swap 子系统自动轮转分摊新写）
3. 槽位进 Active 池
✓ 存量槽、运行中沙箱、vPaused 沙箱全部无感
```

### 5.2 缩容流程（归还池）

关键事实：**mainline `swapoff` 把页迁移到其他活跃 swap 设备**（不是强制回 DRAM），
压缩驻留态得以保持：

```text
Draining（编排器选 pause 间隙/低负载窗口）:
1. 选目标槽（优先 bd_count 低的）
2. swapoff /dev/zramN:
   ├─ vPaused 沙箱驻留页 → 迁移其他 Active 槽（读出+重写入，DRAM 尖峰有界）
   └─ 已 writeback 的页 → 经 backing 回读后重写入目标槽
3. Retired: reset zram → 解绑 dm 区间 → 池侧记账扣减

安全护栏:
- 并发 = 1（同时只 drain 一台，避免迁移风暴叠加）
- DRAM 余量 > 安全线才执行（防雪崩）
- drain 中新 swap-out 自然流向其他槽（同优先级轮转）
```

### 5.3 池侧记账与区间管理

```text
池管理器（per-host agent + 集中账本）:
- 全局空闲区间表：CXL bdev 空间按区间软分区（当前用 dm-linear；
  CXL 3.1 Dynamic Capacity extent 内核支持落地后换原生 extent 供给）
- per-host 借用量 = Σ Active 槽容量，扩缩容即记账增减
- 池紧张 → 向 host 下发归还请求 → 编排器排期缩容
- RAS 信号（poison 增长/链路降速）→ 定向缩容受影响槽位
```

### 5.4 容量口径澄清

- **实际占用**：动态，自动随 writeback/slot 释放伸缩（`bd_count`），由 idle 年龄参数控制——运营上真正想调的"L2 大小"
- **容量**：静态预留（zram 约束 backing ≥ disksize），正解是**前置超配 + 用量监控**，不要在线改 zram；容量调整 = 槽位增减

---

## 6. 扩缩容触发设计

不对称原则：**扩容便宜（分钟级、零干扰、可回退）所以激进；缩容昂贵（swapoff 迁移 +
DRAM 尖峰）所以保守**。

### 6.1 信号源

```text
编排器（30s 评估周期）订阅三类源:
├─ 本机信号: zram sysfs (bd_count) + AgentENV metrics (pageout target/advised, vPaused 计数)
├─ 池侧信号: 池管理器 RPC（归还请求、配额、空闲水位）
└─ 硬件信号: CXL RAS（poison 计数、AER 降速、温度）
```

### 6.2 扩容触发（满足其一即动）

| 信号 | 判据 |
| --- | --- |
| L2 水位 + 趋势 | EWMA 水位 > 80% 且斜率向上，预测 T 分钟内打满（水位是慢变量，必须配趋势外推） |
| 写回失败 | bd writeback 失败/拒绝计数出现（backing 不足的直接证据） |
| DRAM 压力 | PSI memory > 阈值（压缩层承接不住冷页） |
| 池宽松降槛 | 池空闲充裕时门槛从 80% 降到 60% |

### 6.3 缩容触发（必须同时满足 + 动机层其一）

```text
信号层: 水位 < 30% 持续 ≥ 30 分钟（迟滞 + 时间双保险，防振荡）
护栏层: DRAM 余量 > 安全线
互斥层: 无进行中的 drain（并发 = 1）
动机层（其一）:
  ① 池下发归还请求（池容量紧张）← 最常见驱动
  ② 借用计费/配额策略到期
  ③ 定期碎片整合
```

**关键认知**：缩容的常见驱动不是"我水位低了"，而是"池要容量了"——本机低水位只是
"允许缩"的前置条件，不是"应该缩"的理由。

### 6.4 RAS 触发（最高优先级，绕过保守条件）

```text
poison 增长 / AER 降速 / 链路重试 → 立即 drain 受影响槽
  不看水位、不看护栏：风险规避，不是容量管理
  配套：受影响 vPaused 沙箱提前 resume 或转 persistent heavy-pause（现成逃生通道）
```

### 6.5 规则引擎设计点

- 不对称迟滞：扩 80% / 缩 30%，中间 50 个百分点死区，防边界振荡
- 慢信号用趋势（EWMA+斜率），快信号用边沿（RAS/写回失败出现即触发）
- 缩容排期参考 AgentENV scheduler 心跳/负载画像：低负载节点优先、pause 间隙执行
- drain 进行中不扩容（交接期行为可推理）；RAS 可抢占一切

---

## 7. 性能模型

### 7.1 单页换回成本（每 4K 页首次触碰）

```text
正常驻留 DRAM: ~100 ns（EPT 已缓存）
L1 命中(zram):  1-3 µs（缺页处理 + 本地解压）        比正常慢 ~10-30x
L2 命中(CXL):   4-10 µs（+ CXL.mem 读 + bio 路径）    比正常慢 ~40-100x，比 L1 慢 2-4x
L3 磁盘:        50-100+ µs                            比 L2 再慢 10-20x
```

L2 相对 L1 是**增量惩罚**（CXL 链路 + bio 各占 1-3µs），大头是缺页处理的固定内核成本。

### 7.2 摊销总账：每页一次性成本

换回后页重新驻留 DRAM，后续访问回基线。Resume 后 warm-up 代价：

```text
warm-up ≈ Σ (各层页数 × 各层每页成本)
页数分布由 vPause 受害者选择决定:
  活跃窗口页 → 驻留 DRAM（0 成本）；上轮冷页 → L1；更早冷页 → L2

算例: 512MB 工作集，50% 驻留 / 35% L1 / 15% L2:
  L1: 128MB ÷ 4K × 2µs ≈ 64 ms
  L2: 77MB ÷ 4K × 7µs  ≈ 133 ms → 合计 ≈ 0.2s 摊到 resume 后几十秒
  同场景 NVMe 做 L2: 19K 页 × 80µs ≈ 1.5s——CXL 层好 ~7 倍
```

### 7.3 负载画像体感

| 画像 | L2 影响 |
| --- | --- |
| 工作集稳定（代码代理典型 churn） | 接近零——dirty 窗口排除已把活跃集留 DRAM，多数 resume 不碰 L2 |
| 冷启动重放/全量扫描 | warm-up 多付 0.1-1s 量级；顺序模式吃到 swap readahead 批量预取 |
| 疯狂随机全量重访 | 最坏：µs 级缺页风暴 + 多沙箱并发挤 CXL 链路带宽（Gen5 x16 ~40-50GB/s） |
| L2 页永不再触碰 | 永远零差异 |

三个内置减震器：受害者分级（最冷才进 L2）、L1 热路径缓冲、writeback 回读副本保留。

### 7.4 性能验证方法（分层消融法）

```text
层级0  原始块设备画像: fio 同参数跑 CXL fsdax / brd(本地DRAM伪块设备) / NVMe
       → brd 与 CXL 的差值 = 纯 CXL.mem 链路惩罚（无此对照组数据无法归因）
       profile: 单QD 4K随机读（swap-in 形态）/ 1M顺序写（writeback 形态）/ 高并发
层级1  zram writeback 路径:
       填 L1 → echo all > idle → time writeback → bd_stat 算吞吐
       bpftrace 量 do_swap_page 延迟直方图，L1/L2 命中分桶
层级2  端到端 vPause（隔离环境 AENV_TEST_STATE_DIR）:
       场景A 单沙箱冷 resume: warm-up penalty、resume-to-ready
       场景B N 沙箱并发 pause: PSI、邻居干扰、CXL 链路饱和
```

KPI 门槛：

| 指标 | 判定 |
| --- | --- |
| L2 写回吞吐 | ≥ NVMe 顺序写带宽，不拖慢同批 pageout job_duration |
| L2 换回 p99 | 显著优于 NVMe 4K 随机读（数十 µs 内） |
| resume-to-ready | 与无 writeback 基线持平（±5%）——L1 必须兜住热路径 |
| warm-up penalty | 优于 NVMe 方案 |
| 并发风暴 | PSI 平稳、无队列串行化毛刺 |

---

## 8. 成本决策：CXL vs 磁盘

### 8.1 账目

| | L1 本地 DRAM | L2 CXL | L2' NVMe |
| --- | --- | --- | --- |
| $/GB（量级估算） | ~3-5 | ~4-7（含控制器/交换机溢价） | ~0.05-0.15（TLC） |
| 容量成本比 | 1 | ≈1.2-1.5x DRAM | ≈ CXL 的 1/30-1/80 |
| 换回延迟 | 1-3 µs | 4-10 µs | 50-100+ µs |

### 8.2 决策判据：回读率

```text
bd_reads / bd_writes = 累计回读率（写出去的东西有多少被要回来）
  < 1-2%  → L2 纯停车场，NVMe 划算
  5-15%   → resume SLA 直接吃回读延迟，CXL 划算
  中间    → 看 resume SLA 值多少钱
```

vPause 的业务形态（沙箱 pause 后预期会回来）使回读率天然高于归档场景——这是
CXL 在此场景有机会的根因。

### 8.3 更准确的经济框架

CXL 的对标物不是磁盘，而是**每主机 stranded DRAM + 过配**：池化把典型 50-60%
的内存利用率提到 80-90%，利用率差吃掉大半单价溢价。vPause + 压缩本身就是
"DRAM 需求整流器"，CXL L2 是同一经济逻辑向下延伸；磁盘是兜底，不在同一坐标系。

### 8.4 分阶段切换（决策后置到有数据时）

```text
Phase 0: L2 先用 NVMe backing（最成熟玩法，零新硬件），持续采 bd_reads/bd_writes
Phase 1: 实测回读率高 → backing 换 CXL 槽位（走槽位状态机，运维操作非架构改动）
         实测低 → 停在 NVMe
Phase 2: L2=CXL（中冷层）+ L3=NVMe swapfile（归档冷层）并存
```

已知粗糙点：writeback 单一 backing，"中冷进 CXL、老冷进磁盘"的再分层 mainline
不直接支持（需双 zram 叠加），等回读率数据证明值得再做。

---

## 9. 观测与指标

### 9.1 数据源与采集命令

```bash
# L2 状态（挂 backing 后才有此文件；单位 KiB；字段: bd_count bd_reads bd_writes）
cat /sys/block/zram0/bd_stat
# 例: "    2048     512    2048" → 驻留2MiB 累计回读512KiB 累计写回2MiB

# 一步算回读率
cat /sys/block/zram0/bd_stat | awk '{printf "驻留 %.1f MiB | 回读/写回 %.2f%% | 回读/驻留 %.2f%%\n", $1/1024, $3>0?$2/$3*100:0, $1>0?$2/$1*100:0}'

# 全槽位汇总
cat /sys/block/zram*/bd_stat | awk '{c+=$1; r+=$2; w+=$3}
  END {printf "驻留 %.2f GiB, 回读 %.2f GiB, 写回 %.2f GiB, 回读率 %.2f%%\n", c/1048576, r/1048576, w/1048576, w>0?r/w*100:0}'

# 时序采集（30s 周期；bd_reads/writes 是累计值，看趋势必须做差分）
while true; do
  printf '%s ' "$(date +%s)"
  cat /sys/block/zram*/bd_stat | awk '{c+=$1; r+=$2; w+=$3} END {print c, r, w}'
  sleep 30
done | tee /var/log/zram_bdstat.log

# 窗口增量分析
awk 'NR>1 { dr=$3-prev_r; dw=$4-prev_w; dt=$1-prev_t
        printf "%s 窗口%ds: 回读 %d KiB (%.2f MiB/min), 写回 %d KiB, 窗口回读率 %.2f%%\n",
               strftime("%H:%M:%S",$1), dt, dr, dr/dt*60/1024, dw, dw>0?dr/dw*100:0 }
     { prev_t=$1; prev_r=$3; prev_w=$4 }' /var/log/zram_bdstat.log
```

### 9.2 其他观测面

```bash
cat /sys/block/zramN/mm_stat    # orig/compr → L1 压缩比；same/huge pages
vmstat 1 的 si/so               # 全局换入换出
grep VmSwap /proc/<fc_pid>/status # 单进程换出存量（pageout.rs 测试 fixture 已含此行，
                                 #  现有代码只解析 VmRSS——补 parse 即可，见 §11）
```

### 9.3 AgentENV 现成指标（pageout.rs）

```text
agentenv_v_pause_pageout_target_bytes_total{path}     目标页字节
agentenv_v_pause_pageout_advised_bytes_total          实际 advise 成功字节
agentenv_v_pause_pageout_rss_reclaimed_bytes_total    RSS 前后差（近似回收量）
agentenv_v_pause_pageout_job_duration_seconds 等
注意: 换回侧 AgentENV 零记录；压缩比只在 zram sysfs，需 node_exporter textfile 补齐
```

### 9.4 Prometheus textfile 方案

```bash
#!/bin/bash
# /etc/node_exporter/textfile_collector/zram_bd.sh
out="${1:-/var/lib/node_exporter/zram_bd.prom}"
tmp=$(mktemp)
{
  echo "# HELP zram_bd_kib zram writeback backing stats in KiB"
  echo "# TYPE zram_bd_kib gauge"
  cat /sys/block/zram*/bd_stat | awk -v OFS='' '{c+=$1; r+=$2; w+=$3}
    END {print "zram_bd_kib{field=\"count\"} ",c;
         print "zram_bd_kib{field=\"reads\"} ",r;
         print "zram_bd_kib{field=\"writes\"} ",w}'
} > "$tmp" && mv "$tmp" "$out"
```

```sql
# PromQL 滑动 1h 回读率
(sum_over_time(zram_bd_kib{field="reads"}[1h]) - sum_over_time(zram_bd_kib{field="reads"}[1h] offset 1h))
/
(sum_over_time(zram_bd_kib{field="writes"}[1h]) - sum_over_time(zram_bd_kib{field="writes"}[1h] offset 1h))
```

### 9.5 健康形态解读

- 回读应聚集在 v_resume 事件后几分钟（与 AgentENV stage metrics 对齐验证）
- 持续滴流 = idle 年龄参数太激进，热页被误写回 → 调大 idle 年龄
- `bd_reads` 对同一驻留周期同一页只计一次 → 天然就是 distinct 重访页数

---

## 10. 故障语义与 RAS 联动

```text
槽位故障（CXL 区间坏/链路断）:
  writeback 读回 EIO → swap-in 失败 → FC 进程 SIGBUS → 仅挂该槽的沙箱死
  → 既有 terminal-error teardown；其他槽、宿主机无恙
swap 写失败（内核容忍）: 页留 DRAM 重试，只有换入读失败才致命且半径是进程
池需要回收容量: 槽位 Draining/Retired 干净归还（writeback 页可释放，与 A 路径
              的不可迁移内核页对比，这是块设备语义的本质优势；DC extent 时代才玩得转）
RAS 早期预警: poison 增长/AER 降速 → 主动 drain + 受影响沙箱转 persistent 快照
```

---

## 11. 改动清单与可选扩展

### 11.1 必要改动（Rust 核心与内核零改动）

| 位置 | 内容 | 量级 |
| --- | --- | --- |
| `scripts/zram-swap-setup.sh` | 支持 backing_dev 参数（槽位化） | 小 |
| `scripts/zram-swap-teardown.sh` | 排空 → reset → 摘 backing 顺序适配 | 小 |
| `scripts/l2-slot.sh`（新增） | 槽位状态机操作（provision/drain/retire） | 中 |
| `scripts/cxl-pool-agent`（新增） | 池侧区间分配 + 记账 + RAS 监听 | 中 |
| 编排器（AgentENV 或独立 daemon） | 水位决策 + 状态机推进 + pause 间隙调度 | 中 |
| `src/observability/` | zram mm_stat/bd_stat/CXL RAS 采集 | 中 |
| `src/sandbox/firecracker/pageout.rs` | **零改动**（swap 接口以下全透明） | — |
| 内核 | **零改动**（process_madvise/writeback/fsdax/swapoff 迁移全现成） | — |

### 11.2 可选扩展

| 项 | 说明 |
| --- | --- |
| resume 主动换回 | victim 账本（sandbox 层留存 spawn_job 的 regions）+ `process_madvise(MADV_WILLNEED)` 复用 worker 池；建议只 prefetch 确定重访集合（档位 off/working_set/full），防 RSS 回弹清零压缩收益；配套 VmSwap 打点（可同时补上现有 pageout 侧真实换出量盲区） |
| writeback 节奏自动化 | 从脚本 cron 挂进 pause 生命周期钩子（orchestrator 小改） |
| L2 容量水位循环 | 仅超配不足时需要；扩缩容已由槽位状态机覆盖 |

---

## 12. 未验证前提（Phase 0 必须坐实）

1. **backing 独占性**：源码级结论（`backing_dev_store` 无独占 claim，双 zram 共享
   同一 backing 会静默互相踩踏）需运行时验证。脚本（Linux 测试机）：

```bash
#!/usr/bin/env bash
# 双 zram 挂同一 backing 的独占性/数据完整性验证
set -euo pipefail
modprobe zram num_devices=2; modprobe brd rd_nr=1 rd_size=262144
echo 64M > /sys/block/zram0/disksize; echo 64M > /sys/block/zram1/disksize
echo /dev/ram0 > /sys/block/zram0/backing_dev
echo /dev/ram0 > /sys/block/zram1/backing_dev     # ← 观测点1: 是否被拒？
# 差异化 pattern 直接写 zram 块设备
dd if=/dev/urandom of=/dev/zram0 bs=1M count=32 2>/dev/null & p0=$!; wait $p0
dd if=/dev/zero   of=/dev/zram1 bs=1M count=32 2>/dev/null & p1=$!; wait $p1
sum0_before=$(sha256sum /dev/zram0 | cut -d' ' -f1)
echo all > /sys/block/zram0/idle; echo idle > /sys/block/zram0/writeback
echo all > /sys/block/zram1/idle; echo idle > /sys/block/zram1/writeback
sum0_after=$(sha256sum /dev/zram0 | cut -d' ' -f1)
[ "$sum0_before" = "$sum0_after" ] && echo "PASS: zram0 回读完整" \
  || echo "FAIL: zram0 数据被 zram1 写回踩踏 → 必须强制 per-slot 独占"
echo 1 > /sys/block/zram0/reset 2>/dev/null || true   # 观测点2: reset 顺序
```

2. **swapoff 迁移模型**：大存量（数 GB vPaused 驻留）下 swapoff 的迁移耗时与
   DRAM 尖峰需实测，产出缩容排期公式（迁移量 ≈ 该槽承载 swap 存量）。
3. **fio 消融画像**：CXL fsdax vs brd 的净链路惩罚（§7.4 层级0）。

---

## 13. 分阶段落地

```text
Phase 0  静态超配（NVMe backing 起步）
         ├─ 性能画像（分层消融法）
         ├─ backing 独占性验证（§12.1）
         └─ swapoff 迁移模型实测
Phase 1  槽位化改造（zram-swap-setup 参数化 + l2-slot.sh）
         手动 provision/drain；采集 bd_reads/bd_writes 回读率
Phase 2  回读率数据驱动的介质决策（§8.4）+ 水位自动化编排器（§6）
Phase 3  CXL 3.1 DC extent 原生化（内核支持落地后，dm 软分区退役）
```

---

## 附：一句话总结

**不要让 zram 的"内存"直接变成 CXL 内存，让 zram 的"写回目标"变成 CXL 块设备**——
扩缩容的正确单位是"槽"而非"设备"（加一台 zram 就是借一段 CXL，排空一台就是还一段
CXL），zram 本体永远静态，内核永远不动，故障半径永远收敛在单沙箱。
