---
title: Pause-Resume 流程与代码地图
aliases:
  - pause-resume-flow
  - pause resume 代码地图
tags:
  - agentenv/pause-resume
source: /Users/cheney/work/code/AgentENV
created: 2026-10-31
updated: 2026-10-31
status: verified-read
---

# Pause-Resume 流程与代码地图

AgentENV 现状 pause/resume 全流程的代码走读地图。优化设计见 [[Rootfs Retention 一期设计]] 与 [[FlushUpper 二期方案与基准]]，机制细节见 [[OverlayBD 与 LSMT 机制]]。

## 1. Pause 编排层七段（`pause_sandbox_impl`）

实现：`src/orchestrator/service.rs:1448`（主体 1449–1633）。

```text
① pin image refs → ② 分配 artifact root → ③ detach handle+route
→ ④ backend.pause（重活） → ⑤ persist_paused（唯一 durable 点）
→ ⑥ store.update → ⑦ stop()（释放资源）→ 事件发布
```

| 段 | 位置 | 要点 |
| --- | --- | --- |
| ① protect_image_refs | `src/orchestrator/service.rs:1449` | 先立 GC 保护再摘 handle，防层被回收 |
| ② allocate_artifact_root | `src/orchestrator/service.rs:1472` | 分配不 mutate backend，失败零成本回滚 |
| ③ detach handle+route | `src/orchestrator/service.rs:1495` | 此刻起沙箱对外不可达（"厚重感"来源之一），`removed_proxy_route` 留作回滚 |
| ④ sandbox.pause | `src/orchestrator/service.rs:1511` | 见 §2 |
| ⑤ persist_paused | `src/orchestrator/service.rs:1564` | 写 records.db；失败先试原地 resume（`src/sandbox/firecracker/sandbox.rs:594` → `src/sandbox/firecracker/instance.rs:456`，FC 进程还在） |
| ⑥ store.update | `src/orchestrator/service.rs:1602` | API 可见 Paused |
| ⑦ stop | `src/orchestrator/service.rs:1604` | 释放 FC 进程（guest RSS 在此归还）+ ublk 设备；**失败只 warn 不回滚**（记录已 durable） |

## 2. Pause 后端四段串行（`snapshot_to_dir`）

入口 `src/sandbox/firecracker/sandbox.rs:1246` `pause_to_dir`：卷 sync（经 envd）→ FC `create_state_only_snapshot`（内存不经 FC 快照通道，只写 vm_state.bin + 返回 dirty ranges）→ `src/sandbox/firecracker/sandbox.rs:1274` 四段：

| 段 | 调用点 | 实现 | 成本 |
| --- | --- | --- | --- |
| ① mem 脏页→overlaybd raw 层 | `src/sandbox/firecracker/sandbox.rs:1440` | `src/sandbox/firecracker/overlaybd_snapshot.rs:775`（process_vm_readv 读 FC 进程 + 32 并发写） | **∝ 脏字节，首次 pause ≈ 全量** |
| ② rootfs restack | `src/sandbox/firecracker/sandbox.rs:1335` | `src/sandbox/firecracker/overlaybd_snapshot.rs:637`（seal upper + rename 出 runtime + 开新 upper，**层栈 +1**） | ∝ delta + 拷贝 |
| ③ 层搬迁 | 同上 | lowers link/copy：`src/sandbox/firecracker/overlaybd_snapshot.rs:184`；跨 FS copy：`:282`；**sealed delta 全量拷入 artifact：`:529`** | **2× delta IO；跨 FS = GB 级秒级** |
| ④ extra drives | `src/sandbox/firecracker/sandbox.rs:2688` | 同 ②③ 模式 | 叠加 |

## 3. Resume 流程

入口 `src/orchestrator/service.rs:1642` `resume_sandbox` → `src/orchestrator/service.rs:1659`（CAS：`src/orchestrator/persistence/file_backed.rs:374` mark_resuming；回滚 `:381`）→ `src/sandbox/firecracker/sandbox.rs:2033` `start_resume`：

1. warm pool 取 FC 实例（对照冷启动路径 `src/sandbox/firecracker/sandbox.rs:1851`）
2. rootfs 设备重建 `create_overlaybd_runtime_device`（`src/sandbox/ublk/device.rs:456`，~5–50ms）
3. mem 设备 + symlink；网络槽 + FC spawn
4. custom extension `start_resume` hook（`src/sandbox/custom_extension/client.rs:144`，**同步阻塞**，失败即 fail 操作）
5. `load_snapshot_file`（`src/sandbox/firecracker/sandbox.rs:2357` 调用）——**resume 大头，FC 全量读回，4GB ≈ 1.5–4s**；UFFD 惰性恢复可破（`src/sandbox/firecracker/instance.rs:491`，当前 dead code，`storage/uffd-core/` reference-only）
6. FC resume + envd `wait_for_ready`/`init`（`src/sandbox/firecracker/sandbox.rs:982`，每轮 resume 一次 guest 往返）

## 4. 错误阶梯（编排层精华）

| 失败点 | 物理状态 | 恢复策略 |
| --- | --- | --- |
| ①/② | 完好运行 | 纯元数据 CAS 回 Running，成本≈0 |
| ④ 非终局 | FC paused 可继续 | **重新插 handle + 恢复路由** + 状态回 Running；终局失败**不回插**（runtime 可能已污染）→ stop + 移除记录 |
| ⑤ persist 失败 | FC paused 未 stop | 原地 resume（无需重新 launch）；再失败升级终局拆除 |
| ⑦ stop 失败 | 已 durable | warn 照常返回，泄漏由重启清扫兜底 |

原则：**失败点越靠后，恢复手段越重**——元数据回滚 → handle 回插 → 原地 resume → 终局拆除。所有状态恢复走 `update_state_if_state` CAS，防并发改写。

## 5. 周边与持久化

- graceful shutdown 把 Running 持久化为 Paused：`src/orchestrator/service.rs:1378`
- Paused 态再 capture/publish：`src/orchestrator/service.rs:2074` → `src/sandbox/firecracker/sandbox.rs:269`
- persistence trait：`src/orchestrator/persistence/mod.rs:79`；实现 `src/orchestrator/persistence/file_backed.rs:336`（put_record `:150`，record 结构 `:19`）；测试 `:658`
- pause 释放点：`src/sandbox/firecracker/sandbox.rs:1472` stop（rootfs release `:1501`、mem `:1558`；release_device `src/sandbox/ublk/device.rs:315`）；work_dir TempDir `src/sandbox/firecracker/sandbox.rs:176`
- 旁路入口（timeout auto-pause/resume）：`src/api/impls/sandbox.rs:783`、`src/api/impls/sandbox.rs:1373`

## 6. 建议走读顺序

`snapshot_to_dir` 四段（对照本文 §2）→ rootfs restack 实现与层搬迁（成本与层增长都在这）→ resume 顺走 §3。
