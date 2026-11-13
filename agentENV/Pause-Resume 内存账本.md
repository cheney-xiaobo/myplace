---
title: Pause-Resume 内存账本
aliases:
  - pause-resume-memory-ledger
  - 内存收益归因
tags:
  - agentenv/memory
  - agentenv/pause-resume
source: /Users/cheney/work/code/AgentENV
created: 2026-10-31
updated: 2026-10-31
status: analyzed
---

# Pause-Resume 内存账本

回答"pause/resume 到底省哪块内存、各项优化值多少"。流程见 [[Pause-Resume 流程与代码地图]]，方案见 [[Rootfs Retention 一期设计]]、[[FlushUpper 二期方案与基准]]。

## 1. 最大且唯一确定的内存收益：FC 进程退出

guest RSS 的释放在 stop() 时 Firecracker 进程退出（`src/sandbox/firecracker/sandbox.rs:1472` → rootfs/mem 设备 release `:1501`/`:1558`）。**与设备是否保留无关**，所有方案共享此收益。

## 2. 关键事实：释放设备 ≠ 释放页缓存

seal + fsync 后 rootfs 层在页缓存里全是 **clean 页**；且 close fd 不触发逐出，内核只在内存压力下回收（可零成本）。所以"不保留设备"几乎不省页缓存，"保留设备"也不多占——设备保留在内存维度中性，其价值是 resume 省一次设备创建 + 首 IO 热。

## 3. 设备保留 vs 不保留（retention 收益量化）

| 维度 | 保留 | 不保留 | 差异 |
| --- | --- | --- | --- |
| guest RSS 释放 | GB 级即时 | 相同 | 无差异 |
| rootfs 层页缓存 | clean 可回收 | 同样滞留 | ≈ 0 |
| daemon 每设备堆内存（队列缓冲+LSMT 索引） | 百 KB–数 MB/设备（**待实测**） | 立即释放 | 不保留的唯一真内存收益 = N_paused × 此值 |
| ublk 设备号/槽位 | 每个 paused 占一个 | 立即归还 | 百级 paused 时实质重量 |
| resume 设备创建 `src/sandbox/ublk/device.rs:456` | 跳过省 ~5–50ms | 照付 | 保留的核心收益（<resume 总耗时 2–3%） |

结论：retention 默认应关（`pause_retain_rootfs_device=false`），一期先落 seal-in-place+后台 placement（不依赖设备保留，pause 收益照拿），用指标实测后再定。

## 4. rootfs 快照写入开销（每 pause）

成本 ∝ 自上次 seal 的写入量 delta，不 ∝ rootfs 大小（guest 运行期写已 group-commit 增量落盘）：

| 环节 | 现状 | seal-in-place 后 |
| --- | --- | --- |
| seal 本体（index 收尾+fsync 链） | ~10–50ms | 相同 |
| **sealed delta 全量拷入 artifact**（`src/sandbox/firecracker/overlaybd_snapshot.rs:529`，2×delta IO） | delta=2GB ≈ **1–2s** | **移后台** |
| lowers placement（同 FS 硬链接 μs / **跨 FS 全量 copy 秒级**） | 付 | 移后台 |
| 合计 | 闲置 ~10–50ms；写活跃 1–3s+ | **只剩 ~10–50ms** |

跨 FS 是隐雷：runtime 与 artifact 不在同一 FS 时每次 pause 搬整个层栈。

## 5. 页缓存演化（两方案对比）

| 维度 | 现状（每周期新建层） | FlushUpper |
| --- | --- | --- |
| 每周期 cache 净增 | D_k+R_k clean 页滞留 | 相同 |
| 瞬时峰值 | +2×R_r（sealed delta 拷贝） | 少这笔 |
| 文件数/fd/daemon anon | 无界 | 恒定 2 个 |
| mem 历史 delta 页 | 每 resume 全量读 touch（`load_snapshot_file`） | 相同，merge 前谁也跑不掉 |
| cache 归还出口 | 隐式（work_dir 删除）+删沙箱 | 只剩 merge + fadvise(rootfs) +删沙箱 |

要点：**mem 侧 fadvise(DONTNEED) 无效**（每次 resume 全量读回 touch 全部历史层），mem 的唯一出口是 merge；rootfs 侧 fadvise 有效（旧 delta 不被读不 touch）。空闲沙箱 Σ 收敛于 touched-once 集合；churn 沙箱无 merge 必炸（磁盘/cache/resume 时长一起）。

## 6. 其他加速空间（按性价比）

1. **balloon 空闲页过滤脏页集**——pause 侧最大杠杆：free_page_reporting 已开，guest 空闲页即使被标脏也无需写内存层（读回给零页），首次 pause 写量可砍大截
2. **捕获期 page-cache 双重占用**：内存层落盘走 page cache，恰在最想省内存时多出一份脏页缓存——写路径 O_DIRECT 或写后 fadvise(DONTNEED)
3. **三段落盘并行化**：mem 转换/rootfs restack/extra drives 资源不相交，现状串行
4. **resume 侧大头**是 `load_snapshot_file` 全量读回（`src/sandbox/firecracker/sandbox.rs:2357`，4GB@NVMe ≈ 1.5–4s）——UFFD 惰性恢复是唯一结构性解法（`src/sandbox/firecracker/instance.rs:491` dead code）；`process_vm_readv` 同步阻塞 Tokio 的 TODO 在 `src/sandbox/firecracker/process_vm_reader.rs:20`
5. 小项：extension `start_resume` hook 移出关键路径；跳过重复 envd.init
