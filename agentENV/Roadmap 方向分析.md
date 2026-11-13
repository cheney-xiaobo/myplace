---
title: Roadmap 方向分析
aliases:
  - agentenv-roadmap
tags:
  - agentenv/meta
source: /Users/cheney/work/code/AgentENV
created: 2026-10-31
updated: 2026-10-31
status: analyzed
---

# Roadmap 方向分析

仓库内**没有正式 roadmap 文档**（无 ROADMAP.md、GitHub milestones 不可见），本文从仓库信号（成熟度标注、TODO、dead code、docs 结构）+ 官方博客（kvcache.ai 2026-07-27 开源公告）**重建**的方向图。

## 1. 北极星指标（README 声明）

`README.md:29` 一带：boot/resume **<50ms**、pause **<100ms**、增量快照 **<100ms**（重盘写时）、fork 同节点 ≤16、内存超卖生产 9.6×、环境成本降 88.6–96.8%（官方博客）。一切方向服务于"闲置近乎免费、分支近乎免费、隔离不妥协"（Agentic RL 训练，Kimi K3 已在用）。

## 2. 成熟度阶梯

| 层级 | 内容 | 证据 |
| --- | --- | --- |
| 生产已验证 | Firecracker 沙箱、增量快照（OSS/posix）、模板、volumes、E2B 兼容、warm pool、ballooning | README；博客：70 节点 22.5 万沙箱 |
| 原型 | Gateway + Scheduler 多节点控制面 | `services/`（Go）；AGENTS.md 的 Redis 主/副本、artifact-node hints 约束在铺路 |
| 实验（默认关） | P2P（iroh 后端，生产未测）；PVM（x86_64、宿主自带 kvm_pvm、未进主线内核） | `docs/src/configuration/reference.md:372`、`docs/src/deployment/pvm.md:7` |
| 潜伏（代码已埋） | **UFFD 惰性恢复**（dead code + reference-only crate）；P2P 第二后端 urma | `src/sandbox/firecracker/instance.rs:491`、`src/p2p/urma/transport.rs:1007` |

## 3. 方向线

1. **多节点控制面成熟化**：原型→生产最大缺口。跨节点调度/迁移、P2P hints 与调度器联动、多租户隔离
2. **内存效率与生命周期成本**（本知识库主题所在）：ballooning/page cache 复用已做 → [[Rootfs Retention 一期设计]] → [[FlushUpper 二期方案与基准]] → **UFFD**（resume <50ms 目标从沙箱扩展到 GB 级内存的唯一路径，代码骨架已进仓库）
3. **分发与镜像生态**：overlaybd 按需加载已生产化；P2P 保守（OSS P2P-first、POSIX 禁止从 peer 修复）；冷启动带宽卸载
4. **API/生态面**：E2B 兼容是获客面；custom extension 已声明未来加更多能力（`src/custom_extension_api/generated/src/apis/mod.rs` 旁 README）
5. **虚拟化底座备选**：PVM 是唯一非 KVM 赌注，押远期内核主线

## 4. 可见空白（会被问到但 roadmap 无信号）

- **arm64**：无任何配置/测试标记，x86_64 唯一一等公民
- **GPU 沙箱**：无信号
- **配额/计费**：有认证/网络护栏，无 per-sandbox CPU/IO 配额 enforcement 与计量——多租户商用化前置项
- **滚动升级**：持久化 lifecycle 枚举不可扩（一期设计 D4 的根因），在线升级靠"泄漏优于丢失"保守策略

## 5. 与本主题工作的对齐

pause/resume 优化线（一期 → 2a → 2b → UFFD）正处方向线 2 的核心，UFFD 有仓库内 dead code 背书——优先落一期 + UFFD 预研与项目方向完全一致；P2P/urma 属可观望项。

一句话：**官方没写 roadmap，但代码替它写了——密度（内存线）→ 规模（控制面线）→ 带宽（分发线）三条主线清晰，UFFD 是埋得最深的种子。**
