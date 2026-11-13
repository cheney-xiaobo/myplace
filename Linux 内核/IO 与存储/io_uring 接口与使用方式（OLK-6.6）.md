---
title: io_uring 接口与使用方式（OLK-6.6）
aliases:
  - io_uring
  - io_uring 使用
  - liburing 接口
tags:
  - linux/kernel
  - linux/io
  - io-uring
  - liburing
source: /Users/cheney/work/code/ub/kernel @ 8003dfcde32fff47a74060d8ecbe8d246703d334；liburing 官方源码与示例
created: 2026-11-11
updated: 2026-11-11
status: 内核源码静态核对；示例已在 Linux 7.0.14 ARM64 容器验证，未在 OLK-6.6 运行
---

# io_uring 接口与使用方式（OLK-6.6）

## 0. 一句话与适用边界

**io_uring 是用户态与内核共享提交队列（SQ）和完成队列（CQ）的异步请求接口。** 一个 SQE 描述读、写、网络、超时等操作，一个 CQE 返回该操作的结果；批量提交、完成等待、轮询和固定资源注册都围绕这三条系统调用展开：`io_uring_setup`、`io_uring_enter`、`io_uring_register`。

实践上优先使用 **liburing**，不要在普通应用中重复实现 mmap、内存屏障和 SQ/CQ 索引管理。内核源码头注释专门说明共享环的 acquire/release 顺序，并直接指向 liburing 示例（`io_uring/io_uring.c:1-37`）。

它不代表“所有 I/O 都异步”“完全无系统调用”或“必然零拷贝”：普通请求可以先在提交路径直接尝试，无法立即完成时再进入异步路径（`io_uring/io_uring.c:1965-1977`）。单次、同步、很小的文件读未必比 `read()` 更快；主要收益通常来自**批量、真正并发、网络事件、较少资源查找开销和特定轮询场景**。

## 1. 核心对象与执行路径

```text
用户态                                      内核
  │                                           │
  │ 1. io_uring_get_sqe + prep_*              │
  ▼                                           ▼
SQE: opcode/fd/off/addr/len/user_data  →  解析并尝试执行
  │                                           ├─ 立即完成
  │ 2. io_uring_submit                        ├─ poll / 异步重试
  │    非 SQPOLL 通常进入内核                  ├─ io-wq
  │    SQPOLL 由内核线程取 SQE                 └─ SQPOLL / IOPOLL
  │                                           │
  │ 4. wait/peek + cqe_seen                   ▼
  ◀────────────  CQ: user_data/res/flags  ←  发布完成
```

- **SQE**：`opcode`、`flags`、`fd`、偏移、地址、长度、操作特有 flags、`user_data`、固定缓冲区索引或 buffer group 等（`include/uapi/linux/io_uring.h:27-98`）。
- **CQE**：`user_data` 原样返回，`res` 是字节数、fd、0 或负 errno，`flags` 表示更多完成、提供的 buffer ID、zero-copy notification 等（`include/uapi/linux/io_uring.h:386-417`）。
- SQ/CQ 元数据和 SQEs 通过共享内存交换；默认 CQ 大小是 SQ 的两倍，因为 SQE 只在提交阶段占用，在飞请求可以多于 SQ 项数（`io_uring/io_uring.c:3889-3916`）。

## 2. 三条系统调用与 mmap ABI

ARM64 使用 asm-generic 编号：setup=425、enter=426、register=427（`include/uapi/asm-generic/unistd.h:759-764`）。

| 接口 | 作用 | 关键参数/返回 |
| --- | --- | --- |
| `io_uring_setup(entries, params)` | 创建 ring，返回 ring fd，并回填队列大小、feature 和 mmap offsets | `flags` 选择 SQPOLL/IOPOLL 等；本树还会检查 `kernel.io_uring_disabled`（`io_uring/io_uring.c:4081-4145`） |
| `io_uring_enter(fd, to_submit, min_complete, flags, argp, argsz)` | 提交 SQE，并可等待完成、唤醒 SQPOLL 或等待 SQ 空位 | `IORING_ENTER_GETEVENTS` 请求等待；EXT_ARG 可携带 timespec/sigmask（`io_uring/io_uring.c:3592-3639`、`io_uring/io_uring.c:3637-3768`） |
| `io_uring_register(fd, opcode, arg, nr_args)` | 注册/更新固定文件、固定缓冲区、eventfd、probe、restrictions、buffer ring 等 | opcode 表见 `include/uapi/linux/io_uring.h:521-570`，分发入口见 `io_uring/io_uring.c:4603-4648` |

传统 mmap ABI 的偏移为（`include/uapi/linux/io_uring.h:419-427`）：

| 区域 | offset | 内容 |
| --- | --- | --- |
| SQ ring | `IORING_OFF_SQ_RING = 0` | SQ head/tail/mask/flags/array |
| CQ ring | `IORING_OFF_CQ_RING = 0x8000000` | CQ head/tail/mask/CQEs |
| SQEs | `IORING_OFF_SQES = 0x10000000` | SQE 数组 |

若 `params.features` 有 `IORING_FEAT_SINGLE_MMAP`，SQ/CQ 可以合并映射；feature 位定义在 `include/uapi/linux/io_uring.h:502-518`。ring fd 的 mmap 分发位于 `io_uring/io_uring.c:3441-3520`。

**不要只根据本树 UAPI 判断上游 6.6 或目标机器一定支持某 flag。** 本 OLK 树包含厂商扩展，例如 `IORING_SETUP_EXT_PARAM`、`io_uring_params_ext` 和 `IORING_SETUP_SQ_THREAD_FORCE_IDLE`（`include/uapi/linux/io_uring.h:193-201`、`include/uapi/linux/io_uring.h:495-500`）。本地 git 核对也确认 `NO_MMAP`、`NO_SQARRAY` 已在上游 v6.6，不能误报为后 6.6 回合。运行内核、liburing 头文件和应用程序可能是三个不同版本，必须做运行时探测和错误处理。

## 3. 推荐的 liburing 调用顺序

| 步骤 | 接口 | 必须检查什么 |
| --- | --- | --- |
| 初始化 | `io_uring_queue_init(depth, &ring, 0)` 或 `io_uring_queue_init_params()` | 返回 0 或负 errno；不是 ring fd |
| 取 SQE | `io_uring_get_sqe(&ring)` | 返回 `NULL` 表示当前 SQ 已满 |
| 填请求 | `io_uring_prep_read/write/recv/...()` | fd、偏移、长度及指针生命周期 |
| 关联上下文 | `io_uring_sqe_set_data64()` / `io_uring_sqe_set_data()` | 每个 SQE 都显式设置；内核只保存 64 位值，不管理其指向对象 |
| 提交 | `io_uring_submit()` 或 `io_uring_submit_and_wait()` | 非 SQPOLL 返回提交数量或负 errno，不是 I/O 结果；SQPOLL 计数可能高估实际提交数 |
| 等完成 | `io_uring_wait_cqe()` / `io_uring_peek_cqe()` / 批量 `io_uring_peek_batch_cqe()` | 返回 0 仅表示取到 CQE；仍需检查 `cqe->res` |
| 释放 CQE | `io_uring_cqe_seen(&ring, cqe)` | 消费完字段后必须调用；批量处理则用 `io_uring_cq_advance()` |
| 销毁 | `io_uring_queue_exit(&ring)` | 在请求依赖资源释放前完成 ring 清理 |

本笔记配套两个完整示例：

- [io_uring_read.c](/Users/cheney/.dsh/bot/workspace/io_uring_read.c)：从文件 offset 0 最多读 4096 字节，检查 submit 短提交、CQE 负 errno、short read 和 `user_data`。
- [io_uring_probe.c](/Users/cheney/.dsh/bot/workspace/io_uring_probe.c)：输出 ring features，并探测常见 opcode 是否由**当前运行内核**支持。

最小主流程如下；完整错误清理见上面的源码文件：

```c
struct io_uring ring;
struct io_uring_sqe *sqe;
struct io_uring_cqe *cqe;
char buf[4096];
int ret;

ret = io_uring_queue_init(8, &ring, 0);
if (ret < 0)
        /* strerror(-ret) */;

sqe = io_uring_get_sqe(&ring);
if (!sqe)
        /* 先提交/回收已有请求，或稍后再试 */;

io_uring_prep_read(sqe, fd, buf, sizeof(buf), 0);
io_uring_sqe_set_data64(sqe, 0x1234);

ret = io_uring_submit(&ring);
if (ret != 1)
        /* 处理负错误或短提交 */;

ret = io_uring_wait_cqe(&ring, &cqe);
if (ret == 0) {
        if (cqe->res < 0)
                /* I/O 错误：strerror(-cqe->res) */;
        else
                /* 成功，但 0 <= cqe->res < 请求长度是合法短读 */;
        io_uring_cqe_seen(&ring, cqe);
}
io_uring_queue_exit(&ring);
```

## 4. 常用 opcode 与 liburing prep 接口

本树 opcode 总表在 `include/uapi/linux/io_uring.h:203-256`，各操作到内核 prep/issue handler 的映射在 `io_uring/opdef.c:49-220` 及该文件后续表项。

| 类别 | 常用 opcode | liburing 接口示例 |
| --- | --- | --- |
| 文件读写 | `READ`/`WRITE`、`READV`/`WRITEV` | `io_uring_prep_read()`、`io_uring_prep_write()`、`io_uring_prep_readv()`、`io_uring_prep_writev()` |
| 固定缓冲区读写 | `READ_FIXED`/`WRITE_FIXED` | `io_uring_prep_read_fixed()`、`io_uring_prep_write_fixed()` |
| 同步与空间 | `FSYNC`、`SYNC_FILE_RANGE`、`FALLOCATE` | `io_uring_prep_fsync()`、`io_uring_prep_sync_file_range()`、`io_uring_prep_fallocate()` |
| 文件与元数据 | `OPENAT`/`OPENAT2`、`CLOSE`、`STATX` | `io_uring_prep_openat()`、`io_uring_prep_openat2()`、`io_uring_prep_close()`、`io_uring_prep_statx()` |
| socket | `ACCEPT`、`CONNECT`、`SEND`/`RECV`、`SENDMSG`/`RECVMSG`、`SHUTDOWN` | 对应的 `io_uring_prep_accept/connect/send/recv/sendmsg/recvmsg/shutdown()` |
| 零拷贝发送 | `SEND_ZC`、`SENDMSG_ZC` | `io_uring_prep_send_zc()`、`io_uring_prep_sendmsg_zc()`；必须额外理解 notification CQE |
| readiness | `POLL_ADD`、`POLL_REMOVE` | `io_uring_prep_poll_add()`、`io_uring_prep_poll_multishot()`、`io_uring_prep_poll_remove()` |
| 时间与取消 | `TIMEOUT`、`TIMEOUT_REMOVE`、`LINK_TIMEOUT`、`ASYNC_CANCEL` | `io_uring_prep_timeout()`、`io_uring_prep_timeout_remove()`、`io_uring_prep_link_timeout()`、`io_uring_prep_cancel()` |
| 零拷贝管道转发 | `SPLICE`、`TEE` | `io_uring_prep_splice()`、`io_uring_prep_tee()` |
| 控制与测试 | `NOP`、`MSG_RING`、`EPOLL_CTL` | `io_uring_prep_nop()`、`io_uring_prep_msg_ring()`、`io_uring_prep_epoll_ctl()` |

**`epoll` 与 io_uring 不是同一层接口**：epoll 主要通知 fd readiness；io_uring 的 SQE 可以直接描述 read/write/accept/timeout 等操作，也可以通过 `POLL_ADD` 做 readiness，并可注册 eventfd 后纳入现有 epoll 循环。

## 5. setup flags：默认、SQPOLL、IOPOLL 与 taskrun

flags 定义见 `include/uapi/linux/io_uring.h:137-193`。

| 模式 | 使用方式 | 代价与限制 |
| --- | --- | --- |
| 默认 | `queue_init(..., 0)`；submit 时进入内核，CQ 可主动 peek 或通过 enter GETEVENTS 等待 | 仍有提交/等待系统调用，但一次可批量处理多个 SQE |
| `IORING_SETUP_SQPOLL` | 内核 SQ 线程轮询提交；应用更新 SQ tail 后检查 `IORING_SQ_NEED_WAKEUP`，必要时用 `IORING_ENTER_SQ_WAKEUP` 唤醒 | 消耗轮询 CPU；idle 后仍需唤醒；完成仍要 reap CQ。`sq_thread_idle` 缺省为 1 秒（`io_uring/sqpoll.c:412-439`） |
| `IORING_SETUP_IOPOLL` | 轮询设备 I/O 完成，适合支持轮询的低延迟块设备路径 | 不是通用 socket/ buffered-file 开关；读写要求 `IOCB_DIRECT` 且文件实现 `iopoll`，否则 `-EOPNOTSUPP`（`io_uring/rw.c:681-724`） |
| `IORING_SETUP_COOP_TASKRUN` | 尽量在任务本来就要进出内核时运行 task work，减少 IPI 打断 | 不改变请求语义；与 SQPOLL 不兼容（`io_uring/io_uring.c:3952-3971`） |
| `IORING_SETUP_TASKRUN_FLAG` | 有需要运行的 task work 时设置 `IORING_SQ_TASKRUN`，提示应用进入内核 | 本树要求与 DEFER_TASKRUN 配合，不是独立性能开关（`io_uring/io_uring.c:3964-3970`） |
| `IORING_SETUP_DEFER_TASKRUN` + `IORING_SETUP_SINGLE_ISSUER` | 将 task work 延迟到应用获取事件等时点，适合单提交者事件循环 | DEFER 强制 SINGLE_ISSUER，且与 SQPOLL 不兼容；应用必须按计划 enter/GETEVENTS，不能只读共享 CQ 内存（`io_uring/io_uring.c:3922-3928`、`io_uring/io_uring.c:3973-3981`） |

SQPOLL 与 IOPOLL 同时使用时，SQ 线程也可承担完成轮询；没有 SQPOLL 时，IOPOLL 通过 syscall 路径检查完成（`io_uring/io_uring.c:3937-3945`）。这些都属于性能模式，不应在功能未验证前作为默认配置。

## 6. 固定文件、固定缓冲区与 provided buffers 不要混淆

| 机制 | 注册接口 | SQE 使用方式 | 解决什么 |
| --- | --- | --- | --- |
| fixed files | `io_uring_register_files()` / `register_files_update()` | `sqe->fd` 填固定表索引，并设置 `IOSQE_FIXED_FILE` | 避免普通请求路径反复 `fget/fput`；普通 fd 与 fixed-file 获取分支见 `io_uring/io_uring.c:1750-1761` |
| fixed buffers | `io_uring_register_buffers()` | `io_uring_prep_read_fixed/write_fixed()`，`buf_index` 指向已注册 iovec | 重复使用已注册、已 pin 的内存；不是 fixed file |
| provided buffers | `io_uring_prep_provide_buffers()`，或 `io_uring_register_buf_ring()` + `io_uring_buf_ring_add/advance()` | 设置 `IOSQE_BUFFER_SELECT` 和 buffer group；内核为 recv/read 选择 buffer | 特别适合多 shot 接收，避免每次请求预先固定一个应用 buffer |

固定缓冲区注册涉及内存 pin/accounting，并受 `RLIMIT_MEMLOCK` 限制（`io_uring/rsrc.c:41-58`）。注册返回后，注册用的 iovec 数组可以释放，但底层数据内存必须继续保留到注销且无相关在飞 I/O。大 ring、大 buffer 注册失败时，除了 fd 限制，还应检查 memlock 和容器资源策略。`io_uring_sqe_set_flags()` 是覆盖式赋值，组合 flags 应一次传入，或在 prep 后用 `sqe->flags |= ...` 明确保留已有位。

provided buffer 完成时，若 `cqe->flags & IORING_CQE_F_BUFFER`，buffer ID 在 `cqe->flags >> IORING_CQE_BUFFER_SHIFT`；定义见 `include/uapi/linux/io_uring.h:401-417`。应用处理完后要按对应 buffer-ring 协议归还 buffer，不能只调用 `cqe_seen` 就认为 buffer 也已自动归还。

## 7. 链接、超时、multishot 与 eventfd

### 7.1 SQE links 与 linked timeout

- `IOSQE_IO_LINK`：把下一个 SQE 链接到当前请求；普通 link 在前序失败后会取消后续链，short read 等“非预期结果”也可能导致断链。
- `IOSQE_IO_HARDLINK`：更强的链接，不因前序失败而按普通 link 方式断链。
- `IOSQE_IO_DRAIN`：等待此前在飞请求后再执行。
- `IOSQE_ASYNC`：强制走异步路径，不代表更快。

flag 定义见 `include/uapi/linux/io_uring.h:109-135`，链接提交逻辑见 `io_uring/io_uring.c:2188-2238`。

linked timeout 是两个 SQE：业务请求带 `IOSQE_IO_LINK`，后接 `io_uring_prep_link_timeout()`。**它会生成业务请求和 timeout 两个 CQE**。业务先完成时，timeout 常以 `-ECANCELED` 结束；timeout 先到期时，timeout 返回 `-ETIME`，业务请求可能被取消。内核取消后续链接及 timeout 的路径见 `io_uring/timeout.c:157-213`、`io_uring/timeout.c:233-254`。不要把 timeout 的 `-ECANCELED` 当成业务失败，也不要只等一个 CQE。

### 7.2 multishot accept/recv/poll

一个 SQE 可以产生多个 CQE：

- accept 使用 `IORING_ACCEPT_MULTISHOT`（`include/uapi/linux/io_uring.h:363-366`）。
- recv 使用 `IORING_RECV_MULTISHOT`（`include/uapi/linux/io_uring.h:326-351`）。
- poll 使用 `IORING_POLL_ADD_MULTI`（`include/uapi/linux/io_uring.h:290-306`）。

处理规则：

1. `cqe->flags & IORING_CQE_F_MORE` 表示同一父 SQE 以后还会有 CQE；没有 MORE 表示该 multishot 已终止。
2. 多个 CQE 可携带相同 `user_data`，请求上下文必须保留到**没有 MORE 的最终完成**，不是第一个 CQE 就释放。
3. multishot recv 在本树对应的成熟用法是与 `IOSQE_BUFFER_SELECT` 配合；兼容 6.6 的保守写法使用 `io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0)` 并设置 buffer group，不要照搬更新版本关于正 len 的新语义。
4. 每个 accept CQE 都可能返回一个新 fd；每个 recv CQE 都要独立检查 `res`、MORE 和 buffer ID。provided buffers 耗尽时可能得到 `-ENOBUFS` 并终止 multishot，需要补充 buffer 后按语义 rearm。

### 7.3 接入现有 epoll/event-loop

```c
int efd = eventfd(0, EFD_CLOEXEC);
int ret = io_uring_register_eventfd(&ring, efd);
/* 成功后把 efd 加入 epoll；可读时 read uint64_t，然后批量 reap CQ。 */
```

注册路径见 `io_uring/io_uring.c:2841-2877`。注意 eventfd 计数与 CQE 数量**不是一一对应关系**，源码明确说明这一点（`io_uring/io_uring.c:585-605`）。正确做法是收到通知后 drain CQ，而不是按 eventfd 数值假定 CQE 个数。`io_uring_register_eventfd_async()` 只通知异步完成路径，不能当作所有完成都通知的版本（`io_uring/io_uring.c:548-579`）。

## 8. 完成语义与生命周期：最容易出错的地方

1. **三层返回值不要混用**：`queue_init` 的负 errno；`submit/wait_cqe` 的调用结果；`cqe->res` 的操作结果。`wait_cqe=0` 不代表 read/write 成功。
2. **短读短写合法**：正数小于请求长度不是库错误；read 返回 0 通常表示 EOF。需要完整消息时由应用循环并调整 offset/buffer。
3. **完成顺序不等于提交顺序**：并发请求必须用 `user_data` 找回上下文，不能按数组提交顺序解释 CQE；如果按 `user_data` cancel，在飞请求应使用可区分的键。
4. **并发文件读写优先显式 offset**：`offset == -1` 使用并更新共享文件位置，多个请求并发时不能按提交顺序推导各自读写位置。
5. **必须 `cqe_seen`**：不及时推进 CQ head 会耗尽 CQ 空间并进入 overflow 路径；内核 CQE 发布及 overflow 处理见 `io_uring/io_uring.c:889-945`。
6. **数据 buffer 至少保留到对应完成消费完**：不能在 submit 返回后立即 free/reuse。非 SQPOLL 的 `IORING_FEAT_SUBMIT_STABLE` 可放宽 iovec/timespec 等提交元数据生命周期，但不等于 read/write 数据区可提前释放；SQPOLL 还有更严格要求。跨模式最稳妥的规则是把 SQE 引用的元数据和数据区都保留到完成。
7. **multishot 要保留到最终 CQE**：看到 MORE 就继续等；没有 MORE 后才释放请求上下文。
8. **send_zc 不是普通 send**：还有 notification CQE，`IORING_CQE_F_NOTIF`、`IORING_CQE_F_MORE` 及 copied usage 位见 `include/uapi/linux/io_uring.h:327-361`、`include/uapi/linux/io_uring.h:401-413`。不能在普通发送完成时就假定 zero-copy 缓冲区已可复用。
9. **fd 编号和固定槽位要按协议复用**：请求在飞时不要让 fd close/reopen 或 fixed slot update 把旧编号指向另一个对象。
10. **liburing ring 不是天然多线程无锁对象**：多线程提交/收割需要应用同步，或使用独立 ring；`SINGLE_ISSUER` 更明确限定单提交任务。
11. **不要只看 liburing 版本或 opcode probe**：新版本库可以运行在旧内核，但新 opcode/flag 仍可能返回 `-EINVAL` 或 `-EOPNOTSUPP`。probe 只证明 opcode 表项受支持，不单独证明 multishot flag、特定文件、IOPOLL 或设备能力可用，还需真实请求 smoke test。
12. **正常退出先收敛请求**：完成或取消并回收 outstanding 请求，再 `io_uring_queue_exit()`；后者负责销毁 ring，不是“等待所有 I/O 成功”的接口。

## 9. 环境准备、编译、探测与排错

### 9.1 内核与容器前置条件

本树 `CONFIG_IO_URING` 默认 y（`init/Kconfig:1963-1970`），ARM64 openEuler defconfig 也设置为 y（`arch/arm64/configs/openeuler_defconfig:267`）。目标 Linux 可检查：

```bash
uname -a
grep -E '^CONFIG_IO_URING=' /boot/config-$(uname -r) 2>/dev/null || \
  zgrep -E '^CONFIG_IO_URING=' /proc/config.gz 2>/dev/null || true
cat /proc/sys/kernel/io_uring_disabled
cat /proc/sys/kernel/io_uring_group
ulimit -l
ulimit -n
```

`kernel.io_uring_disabled` 在本树中的含义：0 允许；1 只允许 `io_uring_group` 指定组或具有 `CAP_SYS_ADMIN` 的任务；2 全部禁止，失败返回 `-EPERM`（`io_uring/io_uring.c:4120-4145`）。容器还要检查 seccomp/LSM：**本次实测默认 Docker seccomp 下即使该 sysctl 为 0，setup 仍返回 `EPERM`。**

如果需要限制一个 ring 可执行的 SQE/register 操作，可用 `IORING_SETUP_R_DISABLED` 创建，随后调用 `io_uring_register_restrictions()` 配置 opcode/flags 白名单，最后 `io_uring_enable_rings()`。restrictions 只能在 disabled 状态注册一次，启用后未许可操作会被拒绝（`io_uring/io_uring.c:4204-4295`、`io_uring/io_uring.c:4454-4458`）。

### 9.2 安装 liburing 并编译

Debian/Ubuntu：

```bash
sudo apt-get update
sudo apt-get install build-essential pkg-config liburing-dev
cc -std=c11 -Wall -Wextra -Werror -O2 io_uring_read.c \
  -o io_uring_read $(pkg-config --cflags --libs liburing)
cc -std=c11 -Wall -Wextra -Werror -O2 io_uring_probe.c \
  -o io_uring_probe $(pkg-config --cflags --libs liburing)
```

openEuler/Fedora 系通常可使用 `gcc`、`pkgconf-pkg-config`、`liburing-devel` 包；若仓库版本不合适，可按官方 [liburing README](https://raw.githubusercontent.com/axboe/liburing/master/README) 从源码执行 `./configure && make && sudo make install`。

运行：

```bash
./io_uring_read ./io_uring_read.c > /tmp/read.out
cmp ./io_uring_read.c /tmp/read.out
./io_uring_probe

# 需要观察系统调用时；容器还需允许 strace/ptrace：
strace -f -e trace=io_uring_setup,io_uring_enter,io_uring_register \
  ./io_uring_read ./io_uring_read.c >/dev/null
```

不要为生产容器简单复制 `--security-opt seccomp=unconfined`。该选项只在本次一次性本地验证容器中用于区分 seccomp 阻断与程序错误；生产应使用最小权限的自定义 seccomp/运行时策略。

### 9.3 常见错误对照

| 错误/现象 | 优先检查 |
| --- | --- |
| setup `EPERM` | `io_uring_disabled`、容器 seccomp、LSM、用户组与 capability |
| setup `EINVAL` | ring 是否已启用、flags 组合、保留字段是否清零、运行内核是否支持该 flag |
| CQE `-EINVAL` / `-EOPNOTSUPP` | opcode、文件类型、flag、固定资源索引及 IOPOLL 前置条件；先运行 probe |
| register buffers `ENOMEM` | `RLIMIT_MEMLOCK`、cgroup/容器限制、注册总页数 |
| submit 小于准备数量 | 短提交处理，继续提交剩余 SQE；不要把它当成已完成数量 |
| CQ overflow / 不再前进 | 是否漏掉 `cqe_seen`/批量 advance，CQ 是否过小，multishot 是否耗尽 CQ 或 buffers |
| 程序偶发数据破坏 | buffer/iovec/user_data 生命周期、线程并发、fd/fixed slot 复用、MORE/NOTIF 是否处理完整 |

## 10. 本次验证、推断与资料

### 10.1 已实际验证

- 两个配套示例在 **Ubuntu 24.04 ARM64 容器、Linux 7.0.14-linuxkit、liburing 2.5** 下以 `-Wall -Wextra -Werror` 编译通过。
- 默认 Docker seccomp 下 `io_uring_queue_init_params()` 返回 `EPERM`；一次性容器设置 `seccomp=unconfined` 后，probe 成功输出 SQ/CQ/features，列出的 16 个常见 opcode 均返回 supported。
- `io_uring_read` 读取自身源码，输出与原文件 `cmp` 一致。
- **这些运行结果不属于 OLK-6.6 实测**；OLK-6.6 的结论仅来自静态源码。不同机器 probe 结果不能照搬。

### 10.2 工程建议属于推断

“默认先用普通模式、批量和正确生命周期管理，再按 profiling 引入 fixed resources、SQPOLL/IOPOLL”是基于上述执行路径的性能建议，不是内核保证。SQPOLL/IOPOLL 是否更快取决于 CPU 预算、设备、队列深度、批量大小和完成等待方式。

### 10.3 官方 liburing 资料

- [liburing README：构建、内核版本关系与 memlock](https://raw.githubusercontent.com/axboe/liburing/master/README)
- [官方 io_uring-test.c：批量 readv 示例](https://raw.githubusercontent.com/axboe/liburing/master/examples/io_uring-test.c)
- [liburing.h：公共接口与内联实现](https://raw.githubusercontent.com/axboe/liburing/master/src/include/liburing.h)
- [io_uring(7)：SQE pointer lifetimes 与 data stability](https://man7.org/linux/man-pages/man7/io_uring.7.html)
- [linked requests：链接、失败与 timeout 语义](https://man7.org/linux/man-pages/man7/io_uring_linked_requests.7.html)
- [provided buffers：buffer group、选择与归还](https://man7.org/linux/man-pages/man7/io_uring_provided_buffers.7.html)

## 相关笔记

- [[Linux 内核知识库]]
- [[ARM64 Linux 6.6 TLBI 失效场景与数据清理边界]]：io_uring registered buffers、DMA 与 cache/TLB 是不同层次，不能相互替代。
