---
title: RDMA 使用方式、复杂度来源与性能调优
aliases:
  - RDMA 使用方式
  - RDMA 调优
  - verbs 编程模型
tags:
  - rdma
  - infiniband
  - network
  - performance
  - linux-kernel
source: /Users/cheney/work/code/ub/kernel
created: 2026-11-12
updated: 2026-11-12
status: verified-by-reading
---

# RDMA 使用方式、复杂度来源与性能调优

内核引用基于 `/Users/cheney/work/code/ub/kernel`（6.6）的 `drivers/infiniband`。**代码能证实的部分给出文件:行号；性能数字与调优收益属工程经验，未在本机实测（无 RDMA 网卡）。**

## 0. 三句话结论

1. **用法 = 建一串对象 → 把 QP 状态机推到 RTS → 异步投递 + 收割完成**。不是 socket 的 `connect/write/read`，没有"调用即完成"。
2. **复杂的根源**：RDMA 把"可靠传输 + 内存语义 + 零拷贝"同时暴露给应用，而且把硬件状态机、内存注册、完成队列都交给你管。它优化的是**数据面**，复杂度全堆在**控制面**。
3. **调优顺序**：先测准，再按「CPU/NUMA 亲和 → 完成模型 → 队列与内联 → 内存注册 → 网络无损配置」逐层动，**一次只改一个变量**。

## 1. 使用方式

### 1.1 最小流程

以 libibverbs（用户态）为例，八步：

| 步 | 调用 | 干什么 |
| --- | --- | --- |
| 1 | `ibv_open_device` | 打开 RDMA 设备 |
| 2 | `ibv_alloc_pd` | 建保护域 PD，后续对象都挂在它下面 |
| 3 | `ibv_create_cq` | 建完成队列 CQ |
| 4 | `ibv_reg_mr` | 注册内存：pin 住 + 建 IOMMU 映射 + 拿到 lkey/rkey |
| 5 | `ibv_create_qp` | 建队列对 QP（发送 SQ + 接收 RQ） |
| 6 | `ibv_modify_qp` ×3 | RESET → INIT → RTR → RTS，每步必填参数不同 |
| 7 | 交换 QPN/GID/PSN | 连接管理，**协议没规定怎么做，自己解决** |
| 8 | `ibv_post_send/recv` + `ibv_poll_cq` | 投递工作请求，轮询完成 |

内核侧对应实现（用户态 verbs 最终落到这些函数）：

| 对象 | 内核函数 | 位置 |
| --- | --- | --- |
| PD | `__ib_alloc_pd()` | `drivers/infiniband/core/verbs.c:259` |
| QP | `ib_create_qp_user()` / `_kernel()` | `drivers/infiniband/core/verbs.c:1281` / `:1336` |
| CQ | `__ib_create_cq()` | `drivers/infiniband/core/verbs.c:2132` |
| MR 映射 | `ib_map_mr_sg()` | `drivers/infiniband/core/verbs.c:2714` |
| 改 QP 状态 | `ib_modify_qp()` | `drivers/infiniband/core/verbs.c:2013` |
| 投递/收割 | `ib_post_send()` / `ib_poll_cq()` | `include/rdma/ib_verbs.h:3862` / `:3993` |

注意最后两个是**内联壳**，直接转给硬件驱动：

```c
static inline int ib_post_send(...)
{
	return qp->device->ops.post_send(qp, send_wr, ...);
}
static inline int ib_poll_cq(...)
{
	return cq->device->ops.poll_cq(cq, num_entries, wc);
}
```

（`include/rdma/ib_verbs.h:3862-3869`、`:3993-3997`）——**"用 RDMA"实际上是"用你的网卡驱动"**，这是厂商差异会渗进应用的结构性原因。

### 1.2 连接语义先想清楚

| 类型 | 语义 | 适用 |
| --- | --- | --- |
| RC | 可靠连接，有序、重传 | 绝大多数场景，默认选它 |
| UC | 不可靠连接 | 能容忍丢包、要自己补可靠性 |
| UD | 不可靠数据报，无连接，要地址句柄 AH | 一对多、组播 |
| XRC / SRD | 共享接收 / 可靠数据报 | 大规模扇出省 QP |

选错类型，后面的状态机参数、地址交换方式全都不一样。

### 1.3 控制面有两条路

- **libibverbs 裸 verbs**：全部自己管，包括连接建立。
- **rdma_cm / rdma-core**：提供类似 socket 的地址解析和连接管理（内核侧 `drivers/infiniband/core/cma.c`、`iwcm.c`），适合先跑通再下沉优化。

传输类型在内核里就这几种（`include/rdma/ib_verbs.h:165-170`）：`RDMA_TRANSPORT_IB`、`IWARP`、`USNIC`、`USNIC_UDP`。**RoCE 不单独列**，它在用户态表现为 IB transport + Ethernet 链路层——这也是"明明是以太网，却按 InfiniBand 的方式编程"的困惑来源。

## 2. 为什么这么复杂

按"难在哪"拆七条，每条说清设计动机。

### 2.1 异步完成语义

`post_send` 返回只代表"请求进了发送队列"，**不代表数据发出、更不代表成功**。结果在 CQ 里，且可能由网卡后来报错。

为什么这样设计：零拷贝 + 内核旁路要求不阻塞，硬件 DMA 自己跑。代价是应用必须自己维护"已投递未收割"的簿记。

### 2.2 QP 状态机

QP 有 7 个状态（`include/rdma/ib_verbs.h:1270-1276`）：`RESET / INIT / RTR / RTS / SQD / SQE / ERR`。

合法迁移不是随便跳的，内核用一张 7×7 的表管着（`drivers/infiniband/core/verbs.c:1389`），而且**每种 QP 类型在每个迁移上必填/可选参数不同**：

```c
req_param = qp_state_table[cur_state][next_state].req_param[type];
opt_param = qp_state_table[cur_state][next_state].opt_param[type];
if ((mask & req_param) != req_param)
	return false;
```

（`ib_modify_qp_is_ok()`，`drivers/infiniband/core/verbs.c:1692-1716`）

这就是为什么 INIT→RTR 要填 pkey/GID/MTU，RTR→RTS 又要填超时/重试/PSN——**硬件寄存器就是按阶段配置的，API 把硬件状态机原样暴露了出来**。

### 2.3 内存注册

RDMA 要网卡直接 DMA 你的内存，所以必须 pin 住、建 IOMMU 映射、发 lkey/rkey。映射本身也是交给驱动的：

```c
mr->page_size = page_size;
return mr->device->ops.map_mr_sg(mr, sg, sg_nents, sg_offset);
```

（`ib_map_mr_sg()`，`drivers/infiniband/core/verbs.c:2714-2724`）

注册慢、有页对齐要求、还牵涉权限（rkey 给对端用），**这是普通网络编程完全没有的一摊事**。

### 2.4 连接管理没有标准

QPN、GID、PSN 这些信息谁来交换？verbs 不管。你可以用 socket 带外交换、用 rdma_cm、用集群自己的控制面。**协议栈故意留白**，因为部署形态太多。

### 2.5 数据面/控制面分离

控制面可以走 rdma_cm 甚至普通 TCP，但数据面必须自己写 post/poll。两套模型混在一个程序里，心智负担翻倍。

### 2.6 厂商差异渗进应用

既然 post/poll/map 都是 `device->ops.*`，那 WQE 大小、内联上限、CQ 合并能力自然因卡而异。调优参数没法写死。

### 2.7 可靠性要你自己收尾

RC 的重传、PSN 检查、QP 进 ERR 后的恢复，都在你这边。**它给你"可靠传输"的语义，但不给你"可靠传输的运维"**。

一句话总结：**RDMA 把网卡驱动里最原始的对象模型直接端给了应用，省掉了内核协议栈，也省掉了内核协议栈帮你扛的所有复杂度。**

## 3. 性能调优

### 3.1 先测准，再动手

- 用 perftest 打底：`ib_write_bw`（带宽）、`ib_send_lat`（时延）、`ib_read_bw`。
- 分清三个指标：**消息速率 msg/s、带宽 Gb/s、时延 us**。优化方向经常互相打架。
- 看硬件计数器：`/sys/class/infiniband/<dev>/ports/*/hw_counters`，别只信应用侧日志。

### 3.2 CPU / NUMA / PCIe 亲和

- CQ 创建时带 `comp_vector`（`ib_cq_init_attr` 的字段，`include/rdma/ib_verbs.h:371-375`；老的 `__ib_alloc_cq()` 则直接作参数传入，`:3888-3890`），把完成中断绑到指定核。
- **内存分配和网卡同 NUMA**，跨 socket 的 PCIe 跳数是常见隐性瓶颈。
- 轮询线程绑核、隔离，避免被调度走。

### 3.3 完成模型：轮询 vs 中断

内核给的几种完成上下文（`include/rdma/ib_verbs.h:1586-1592`）：

| 模式 | 特点 |
| --- | --- |
| `IB_POLL_DIRECT` | 调用者上下文轮询，时延最低，烧 CPU |
| `IB_POLL_SOFTIRQ` | 软中断里收割，省 CPU，时延高 |
| `IB_POLL_WORKQUEUE` / `UNBOUND` | 工作队列，更省，时延更高 |

CQ 还能做**合并**（ moderation ），mlx5 的接口是：

```c
int mlx5_ib_modify_cq(struct ib_cq *cq, u16 cq_count, u16 cq_period)
```

（`drivers/infiniband/hw/mlx5/cq.c:1136-1149`，受 `cq_moderation` 能力位约束）——攒够 `cq_count` 个或等够 `cq_period` 微秒才报一次完成。

经验（非本仓库结论）：**小包/低时延走轮询，大包/高吞吐走中断 + 合并**。

### 3.4 队列与内联

- **内联 inline**：小包直接塞进 WQE，省一次 DMA 读。上限由 WQE 大小算出来：

```c
qp->max_inline_data = wqe_size - sq_overhead(attr) -
		      sizeof(struct mlx5_wqe_inline_seg);
```

（`drivers/infiniband/hw/mlx5/qp.c:608-610`）代价是 WQE 变大、每包占更多发送队列空间。
- 队列深度 SQ/RQ/CQ 要配平，CQ 太浅会丢完成。
- 用 post list **批量投递**，摊薄 doorbell 开销。

### 3.5 内存注册

- **复用 MR**（做 MR cache），绝对不要每包 `reg_mr`。
- 用大页减少页表项和 IOMMU 映射开销。
- 提前 pin 好，避免数据面触发 page fault。

### 3.6 网络侧（RoCE 专属，经验做法）

RoCE 跑在以太网上，**无损配置经常是性能差的真正原因**：

- PFC（按优先级流控）和 ECN 要配对开，DCQCN 参数要和交换机一致。
- MTU 要端到端一致。
- 拥塞时先看交换机丢包/暂停帧，再怀疑主机。

> 这一节是以太网/RoCE 部署的通行经验，**不是本仓库代码结论**，具体数值依交换机型号而定。

### 3.7 常见坑（按出现频率）

1. 每包注册内存 —— 直接把 RDMA 的优势吃光。
2. CQ 太浅或忘了收割 —— 完成堆积、发送卡死。
3. 跨 NUMA 分配内存/绑核 —— 带宽打对折。
4. 把 RDMA 当 socket 用，不管 QP 进 ERR 的恢复。
5. 只看带宽不看消息速率 —— 小包场景指标选错。

## 4. 和本仓库 SMAP/UB 的关系

[[UBTurbo SMAP 架构与实现原理]] 里那条 UB 总线 + DMA 通路**不是 RDMA verbs**，但概念同源：内存注册、完成队列、NUMA 亲和、doorbell 都是同一套硬件 DMA 思路。读懂 RDMA 的对象模型，回头看 SMAP 的迁移路径会顺很多。

## 5. 未验证

- 本机无 RDMA 网卡，**所有性能数字、调优收益未实测**。
- RoCE 的 PFC/ECN/DCQCN 具体参数依赖交换机与固件，本文只给方向不给数值。
- 用户态 libibverbs 的版本差异（尤其 inline 与 CQ 合并的默认值）未逐版本核对。

## 6. 相关

- [[UBTurbo SMAP 架构与实现原理]]
- [[UBTurbo SMAP 冷热判断与交换速率]]
- [[内存管理总览]]
