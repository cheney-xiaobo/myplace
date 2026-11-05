---
title: Linux 内核知识库
aliases:
  - kernel-notes
tags:
  - linux/kernel
  - meta/moc
---

# Linux 内核知识库

基于本机源码的静态阅读笔记。**所有结论都带 `文件路径:行号`，没有编译或运行验证。**

## 笔记

| 笔记 | 结构 | 用途 |
| --- | --- | --- |
| [[内存管理总览]] | §0–§19 | 内存管理整体：分层全景、node/zone/page、buddy+PCP、slub/vmalloc、地址空间与缺页、回收、swap、NUMA、memcg/OOM、THP/KSM、观测调优 |
| [[Page Cache 分析]] | §0–§10 | page cache 专文：核心设计、申请/释放、脏页限流（§4.7）、双阈值与 dirtyable memory（§8.3）、工程实践、ext4 vs XFS（§10 附录） |
| [[内存回收观测与zone]] | §1–§8 | zone 的作用与回收观测：vmstat 计数器、zoneinfo 判读、tracepoint、PSI、memcg 视角、判定表 |
| [[ARM64 TLBI 与 x86 TLB 失效]] | §1–§6 | TLBI 的粒度与编码、x86 为何没有对应机制、两边优化手段、本树 TLBID/TLBIP 实现、社区引进时间线 |
| [[ARM64 TLBI 能力怎么用（6.6 与 6.10）]] | §0–§8 | 能力如何被检测与门控、怎么确认生效、该调哪些 API、退化阈值、6.6→6.12 的变化 |
| [[Firecracker microVM 在 ARM 上的 TLB 刷新时机]] | §1–§7 | 四层失效域、启动/运行/pause/resume/snapshot/销毁各阶段是否刷、VMID 回卷与 dirty log 的放大效应、观测与优化 |
| [[内存专题记忆]] | 索引型 | 文档地图、本树配置事实卡、主题索引、自纠错清单、更新日志 |

## 代号约定

正文里出现的两种简写，分别指向这两篇笔记：

- `overview §X` → [[内存管理总览]] 第 X 节
- `pagecache §X` → [[Page Cache 分析]] 第 X 节

## 阅读路径

**想建立整体认识**：[[内存管理总览]] §1 全景 → §2 表示 → §4 分配器 → §7 地址空间 → §8 缺页 → §9 回收。

**想查“某个默认行为是什么”**：[[内存专题记忆]] §3 配置事实卡。

**想搞清 page cache / 写回 / 脏页**：[[Page Cache 分析]] §1 核心设计 → §4.7 脏页限流 → §8.3 双阈值。

**想比较 ext4 与 XFS**：[[Page Cache 分析]] §10 附录。

## 基线

- 源码根：`/Users/cheney/work/code/ub/kernel`
- 版本：6.6.0，openEuler OLK-6.6，HEAD `8003dfcde32f`
- 配置依据：`arch/x86/configs/openeuler_defconfig`

三条最容易记错的默认值：THP 是 `always`、NUMA balancing 开机就开、MGLRU 编译进去但**默认未启用**。

## 维护方式

**新笔记直接写在本 vault**，规则见 [[知识库约定]]。下面这三篇是例外：它们在 workspace 里写作与校验，再同步过来。

```bash
cd /Users/cheney/.dsh/bot/workspace
python3 check_linerefs.py linux-mm-overview.md        # 行号边界校验
python3 check_linerefs.py linux-pagecache-analysis.md
python3 sync_to_obsidian.py                            # 同步到 vault
```

**vault 里的副本是只读产物**：直接在这里改，下次同步会被覆盖。要改内容请改 workspace 里的源文件。
