---
title: 笔记库说明
aliases:
  - README
  - 总索引
tags:
  - meta/index
updated: 2026-11-14
---

# 笔记库说明

个人技术笔记，按主题分目录保存。内核笔记的入口是 [[Linux 内核知识库]]。

| 目录 | 内容 | 入口 |
| --- | --- | --- |
| `Linux 内核/` | 内核源码静态分析 | [[Linux 内核知识库]] |
| `Linux 内核/内存管理/` | 内存管理总览、Page Cache、回收观测与 zone、zram | [[内存管理总览]] |
| `Linux 内核/TLB 失效/` | ARM64 TLBI 能力与场景、KVM / Firecracker 的 TLB 刷新 | [[ARM64 TLBI 与 x86 TLB 失效]] |
| `Linux 内核/IO 与存储/` | io_uring 接口、SQ/CQ 与使用方式 | [[io_uring 接口与使用方式（OLK-6.6）]] |
| `Linux 内核/远端内存与 RDMA/` | RDMA verbs 与调优、UB 远端内存借用与 DAX | [[RDMA 使用方式与性能调优]] |
| `UBTurbo/` | ubturbo SMAP 架构、冷热判断与交换速率 | [[UBTurbo SMAP 架构与实现原理]] |
| `agentENV/` | AgentENV 沙箱、Pause-Resume、并发启动与设计文档 | [[AgentENV 知识库]] |
| `scripts/zram-tier-bench/` | zram → brd 三态微基准：C 源码、预检、配置与多轮运行脚本 | 见该目录 `README.md` |

约定：内核结论带 `文件路径:行号`，未实测的结论标「未验证」；写作与行号校验流程见 [[内存专题记忆]] §8。
