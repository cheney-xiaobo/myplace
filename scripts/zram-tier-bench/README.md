# zRAM → brd L2 三态微基准

比较同一工作集的三种状态：

- **A**：匿名页驻留普通 RAM。
- **B**：通过 `process_madvise(pidfd, ..., MADV_PAGEOUT)` 换入 zRAM。
- **C**：先把页换入 zRAM，再通过 `idle=all`、`writeback=idle` 淘汰到 brd（RAM 块设备）。

> brd 仍然使用 RAM。本实验测的是 zRAM 解压与额外块设备回写/读回路径，**不是 SSD/UFS 时延，也不能据此计算整机节省了多少物理内存**。

## 文件

| 文件 | 用途 |
| --- | --- |
| `zram_tier_bench.c` | 工作集初始化、三态构造、首次读写、稳态读写、校验与统计 |
| `check-zram-env.sh` | 只读内核/工具/后备设备预检 |
| `setup-zram-l2.sh` | 配置专用 zRAM、brd、swap 和 `vm.page-cluster=0`，支持清理 |
| `run-zram-tier-bench.sh` | 多轮运行，保存原始日志、CSV、环境快照及有效状态 |

## 环境要求

- 独占的 Linux 测试机；不要在生产机或共享性能 CI 上运行。
- root：`process_madvise` 需要 `CAP_SYS_NICE`，设备配置还需要 `CAP_SYS_ADMIN`。
- Linux 5.10+，建议核对目标内核源码和配置：

```text
CONFIG_SWAP=y
CONFIG_ZSMALLOC=y
CONFIG_ZRAM=y|m
CONFIG_ZRAM_WRITEBACK=y
CONFIG_BLK_DEV_RAM=y|m
CONFIG_ZRAM_MEMORY_TRACKING=y   # 可选；idle=all 本身不依赖它
```

- GCC、Make、util-linux（`zramctl`、`swapon`、`blockdev`、`findmnt`）、`mkswap`。
- 默认需要至少 256 MiB 的未使用 brd/backing 块设备；zRAM disksize 默认 512 MiB，工作集默认 100 MiB。
- 如果 brd 是内核内置且默认只有 4 MiB，脚本会拒绝使用；请提供更大的空闲块设备，或在独立测试 VM 启动时调整 brd 参数。脚本不会修改启动配置。

## 构建与预检

```bash
cd scripts/zram-tier-bench
make
./check-zram-env.sh
```

已有后备块设备时，可同时检查容量、挂载和 swap 冲突：

```bash
./check-zram-env.sh /dev/ram0
```

预检失败时不得记录性能结果。特别是 `CONFIG_ZRAM=n` 时，即使 `process_madvise` 返回成功，也只能说明系统调用可用，页面可能进入其他文件 swap，不能标成 zRAM 结果。

## 配置专用 zRAM + brd

```bash
sudo ./setup-zram-l2.sh --yes
```

默认参数：

```text
zram0: lz4，disksize=512 MiB，swap priority=100
backing: /dev/ram0，至少 256 MiB
vm.page-cluster: 0（关闭 swap readahead）
```

可覆盖参数：

```bash
sudo ./setup-zram-l2.sh \
  --dev zram0 --brd /dev/ram0 \
  --zram-mb 512 --l2-mb 256 --algorithm lz4 --yes
```

如果 `/dev/ram0` 不存在，脚本以 `rd_nr=1 rd_size=<l2-mb*1024> max_part=0` 加载 brd。若设备已经存在但容量不足，脚本会失败，不会重置或格式化它。`backing_dev` 和 `comp_algorithm` 都在 `disksize` 之前设置。

配置会保留到显式清理，避免压测结束后丢失状态证据。清理命令：

```bash
sudo ./setup-zram-l2.sh --dev zram0 --cleanup --yes
```

清理只处理本脚本记录的 zRAM：关闭其 swap、reset zRAM、恢复原 `page-cluster`，并只卸载由本脚本加载的模块。不会关闭其他 swap。

## 运行

```bash
# 快速冒烟：5 轮，100 MiB，唯一 salt 的可压缩文本页，首次读+首次写
sudo ./run-zram-tier-bench.sh --runs 5

# 正式采样：建议至少 20 轮，并固定 CPU/频率、后台负载和设备温度
sudo ./run-zram-tier-bench.sh --runs 20 --pattern text --cold-mode both

# 只测 A/B
sudo ./run-zram-tier-bench.sh --runs 5 --no-l2
```

输出默认在 `results/<timestamp>-<pid>/`，也可用 `--output <新目录>` 指定。每轮包含：

- `run-N.log`：状态证据、缺页、CPU、数据校验及完整 stdout/stderr。
- `run-N.csv`：`state,metric,value,unit` 四列指标。
- `environment.txt`：内核、参数、swap、zRAM 初始统计。
- `STATUS`：`RUNNING`、`COMPLETE` 或 `INVALID:<原因>`。任何程序错误、pidfd 回退、关键状态证据缺失或校验失败都会判废，不会将部分 CSV 当作有效结果。

## 指标与 CSV state

| state | 含义 | 主要 metric |
| --- | --- | --- |
| `A` | 普通 RAM 初始化及稳态 | `fill_mbs`、`read_mops`、`write_mops` |
| `A-read` / `A-write` | 驻留页首次逐页读/写基线 | `fault_p50/p95/p99_us`、`fault_throughput_mbs` |
| `B-read` / `B-write` | zRAM 首次换入读/写 | 同上 |
| `C-read` / `C-write` | brd 首次读回后读/写 | 同上 |
| `B-evict` / `C-evict` | 首次 pageout | `pageout_mbs`、`vmswap_mb` |
| `B-re-evict` / `C-re-evict` | `both` 中为首次写重新 pageout | 同上 |
| `C-wb` | 首次 zRAM→brd writeback | `writeback_s` |
| `C-re-wb` | 为首次写重新 writeback | `writeback_s` |

`both` 先测首次读；此时页面已经回到 RAM，所以必须重新 pageout（C 还要重新 writeback）才能测首次写。**汇总淘汰成本时使用 `C-evict`/`C-wb`，不要把 `C-re-evict`/`C-re-wb` 混入首轮样本。**

A-write 不含缺页，只表示 store 和计时开销基线；B/C 的首次写包含换入及写缺页路径，报告时应说明这个差异。稳态读写发生在页面回驻后，不是 zRAM/brd 的持续随机访问吞吐。

## 状态有效性与数据口径

- A：`VmSwap≈0`，工作集驻留 RAM。
- B：pageout 后 `VmSwap≈工作集`，`mm_stat.orig_data_size≈工作集`，首轮不应有 backing 写回增量。
- C：writeback 后 `VmSwap≈工作集`，`bd_count`/`bd_writes` 增长且 `mem_used_total` 下降；读回后检查 `bd_reads`。
- 主测使用 `-p text`：每页有唯一 salt，避免相同页进入 `ZRAM_SAME` 而被 writeback 跳过。`-p random` 可作为不可压缩数据对照。
- 每个冷访问阶段每页只访问一次。A-read 为保证 fill 后首次访问，不做前置全量校验；A-write 和 B/C 写路径校验每页首个 8B 以外的数据，B/C 读路径做全量校验。校验失败退出码为 3。
- P50/P95/P99 来自单页计时，包含 `clock_gettime` 开销；吞吐、wall/CPU、minor/major fault 和 zRAM/brd 证据共同解释，不能只看一个时延数字。
- 多轮报告建议给中位数、离散程度和置信区间；测试期间不要并发运行其他内存压力任务。
