---
title: AgentENV 知识库
aliases:
  - agentenv-notes
  - AENV知识库
tags:
  - agentenv
  - meta/moc
source: /Users/cheney/work/code/AgentENV
created: 2026-10-31
updated: 2026-11-07
status: active
---

# AgentENV 知识库

AgentENV（AENV）是基于 Firecracker microVM 的 Agentic RL 执行环境平台（E2B 兼容 API，snapshot-capable，overlaybd+ublk 存储）。本主题记录 **沙箱拉起、ublk 设备管理、pause/resume 生命周期优化与单机并发启动**的设计分析和代码走读。代码结论带 `文件路径:行号`；静态阅读、本机基准及 Linux 专属路径的验证范围以各篇说明为准。

## 笔记

| 笔记 | 内容 | 用途 |
| --- | --- | --- |
| [[沙箱拉起全流程]] | [T]/[C]/[F]/[R] 四入口变体、launch 14 步编排、start_fresh/start_resume、fork 专路径、warm pool、失败回滚 | 拉起链路走读入口 |
| [[Ublk 设备管理]] | agent/daemon/内核三层架构、设备创建流、FC 挂接、引用计数与 cache_fd、restack、释放清理顺序、恢复（负面结论） | 设备层机制速查 |
| [[Pause-Resume 流程与代码地图]] | 现状流程、编排错误阶梯、代码导航 | 走读代码的入口地图 |
| [[Rootfs Retention 一期设计]] | Phase-1 设计：seal-in-place、后台 placement、设备保留 | 改动清单与 crash 窗口分析 |
| [[FlushUpper 二期方案与基准]] | Phase-2 设计：持久 upper 续写 + 基准数据 | churn 场景收益量化 |
| [[OverlayBD 与 LSMT 机制]] | seal 语义、Hybrid in-place、cache checkpoint | 底层原语速查 |
| [[Pause-Resume 内存账本]] | 内存收益归因、页缓存演化、开销量化 | 回答"到底省了哪块内存" |
| [[Roadmap 方向分析]] | 成熟度阶梯、方向线、UFFD 等潜伏项 | 投入方向对齐 |
| [[单机并发启动配置与架构分析]] | 100–500 并发启动链路、warmup 起始配置、单/多镜像、x86/ARM、容量与测试矩阵 | 并发压测配置入口 |
| [[并发效率观测与 envd 等待分析]] | 并发效率公式、原始数据、PromQL、envd 前后调用链、诊断矩阵与插桩建议 | 分析启动尾延迟和 envd 等待 |
| [[并发启动控制点分析]] | admission、singleflight、互斥、三类 warm pool、ublk/OverlayBD/DNS 限流及控制缺口 | 判断哪些参数真正能限制并发 |

## 阅读路径

**想先看懂现状代码**：[[沙箱拉起全流程]]（拉起四变体）→ [[Ublk 设备管理]]（设备层）→ [[Pause-Resume 流程与代码地图]]（`pause_sandbox_impl` → `snapshot_to_dir` 四段串行 → `start_resume`）。

**想理解优化方案**：[[Rootfs Retention 一期设计]]（已产出设计文档）→ [[FlushUpper 二期方案与基准]]（已有基准数据）→ [[Pause-Resume 内存账本]]（收益归因）。

**想查底层机制**：[[OverlayBD 与 LSMT 机制]]。

**想做 100–500 并发启动分析**：[[单机并发启动配置与架构分析]]（配置和测试矩阵）→ [[并发效率观测与 envd 等待分析]]（采集和定位）→ [[并发启动控制点分析]]（限流边界和控制缺口）。这三篇基于主线 `34cdc8098096726646853a18cec7ae143995dcef` 的静态源码分析，未做 Linux/KVM 实测。

## 核心结论速览

1. pause 热路径的大头 = 内存脏页落盘（首次 ≈ 全量，之后增量）；rootfs restack 的 sealed delta 全量拷贝是第二大头（`src/sandbox/firecracker/overlaybd_snapshot.rs:529`）
2. resume 大头 = FC `load_snapshot_file` 全量读回内存镜像，成本 ∝ 内存大小；设备创建只占 ~5–50ms。UFFD 惰性恢复是唯一结构性解法，代码已埋（`src/sandbox/firecracker/instance.rs:491`，dead code）
3. 现状每 pause 新建一层 → 层数/fd/磁盘无界累积；HybridLogStructured upper 支持 in-place 覆写（`storage/overlaybd/src/lsmt/file/readwrite.rs:417`），FlushUpper 方案下 churn 负载天然有界（基准实测磁盘 12× 差距）
4. guest RSS 的释放发生在 stop() 时 FC 进程退出，与设备是否保留无关

## 基线

- 仓库根：`/Users/cheney/work/code/AgentENV`（Rust workspace + `services/` Go 模块）
- 设计文档：`docs/design/pause-rootfs-retention-phase1.md`（一期完整设计，crash 窗口 W1–W10）
- 基准：`storage/overlaybd/src/image/image_file.rs:2092` `bench_pause_churn_new_layer_vs_flush_upper`（`#[ignore]` 门控）
- 行号校验：`python3 /Users/cheney/.dsh/bot/workspace/check_linerefs.py "<笔记>" --root /Users/cheney/work/code/AgentENV`

## 维护方式

按 [[知识库约定]] 直接在本 vault 写作；代码行号变更后需重新校验。
