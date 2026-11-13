---
title: Rootfs Retention 一期设计
aliases:
  - phase1-retention
  - pause rootfs 保留设计
tags:
  - agentenv/design
  - agentenv/pause-resume
source: /Users/cheney/work/code/AgentENV/docs/design/pause-rootfs-retention-phase1.md
created: 2026-10-31
updated: 2026-10-31
status: proposal
---

# Rootfs Retention 一期设计

> 完整设计文档：`/Users/cheney/work/code/AgentENV/docs/design/pause-rootfs-retention-phase1.md`（含 crash 窗口 W1–W10、§11 改动清单）。本笔记是决策摘要与改动地图。

目标：pause 热路径去掉 rootfs restack 的 sealed delta 全量拷贝与层搬迁串行段（现状见 [[Pause-Resume 流程与代码地图]] §2），层搬迁后台化，rootfs ublk 设备跨 pause/resume 保留复用。

## 1. 核心决策

| # | 决策 | 理由 |
| --- | --- | --- |
| D1 | **seal 与 placement 分离**：seal 本体（`close_seal`，只加 index trailer 不拷数据，`storage/overlaybd/src/image/image_file.rs:225`）留在 pause 同步路径；placement（rename 进 artifact、lowers link/copy、fsync 链）整体后台化 | 查证后发现 seal 本体近零成本，成本全在 placement |
| D2 | **runtime 目录持久化**：overlaybd runtime 目录（upper + image.json）放 `<AENV_HOME>/sandbox-runtime/<id>/`，launch 时即落位，work_dir 只留 transient | 否则设备保留与 crash 恢复都不成立；work_dir 随 stop 消失（`src/sandbox/firecracker/sandbox.rs:176`） |
| D3 | 一期用 **seal-in-place** 而非 flush barrier | seal 语义已被 restack 流程充分验证；RW torn-tail 重开语义验证推迟（Phase-2a 变硬前置，见 [[FlushUpper 二期方案与基准]]） |
| D4 | **lifecycle 枚举不动**，retained 信息全部进 state JSON 的 `serde(default)` 字段 | 新增枚举变体会让老版本 daemon 删除坏记录（滚动升级丢 Paused 沙箱）；老版本读到新字段自然走慢路径重建。**泄漏优于丢失** |

seal-in-place 实现技巧：现有 daemon `restack_snapshot` 把 `output_layer_path` 指到 runtime 目录内部即得（同 FS rename 零拷贝、写锁语义复用）——`storage/ublk-daemon/src/server.rs:1373` handle_restack_snapshot，一期**不需要新增 daemon 原语**。

## 2. 流程改造

### Pause 热路径（快）

```text
卷 sync → FC pause → vm_state + 脏页内存层（不变）
→ rootfs 只 seal-in-place（不 seal、不搬层、不碰设备）
→ record 写 #1（unsealed 形态，指针 → runtime 目录 image.json）
→ stop()：rootfs 设备移交 retained 注册表；FC 退出释放 guest RSS（收益不变）
```

### 后台任务（pause 返回后，best-effort、幂等）

```text
T1 层文件硬链接 runtime → artifact
T2 原子写 artifact image.json（新文件，temp+fsync+rename）
T3 CAS record 写 #2 → sealed 形态（指针翻转到 artifact，单向不可逆）
旧层文件保留（硬链接共享 inode，仍被 retained 设备引用）
```

### Resume 快路径

- retained 表命中且 daemon 探活成功 → 采纳设备与 image config，**跳过** `create_overlaybd_runtime_device`（`src/sandbox/ublk/device.rs:456`），abort+await 后台 placement
- 未命中（daemon 重启/已 kill）→ 回退现有重建路径：sealed 走 artifact 自包含 config；unsealed 走持久 runtime 目录（seal 已在 pause 返回前完成，文件必在）

## 3. Record 状态机（sealed 维度，与 lifecycle 正交）

```text
unsealed（指针→runtime image.json） --后台 placement CAS--> sealed（指针→artifact 自包含）  单向
```

两种形态在每个 crash 点独立自洽；record JSON 全值重写、KV 单次 put、WAL 原子。runtime 侧 image.json 在 pause 同步路径**原地重写**（seal 的 durable 锚点，不可后移）。

## 4. Crash 窗口与清理完备性

- W1–W10 覆盖 pause 各阶段、placement 各阶段、resume 中断、daemon 单独重启、掉电、kill 并发；所有中间态由"record 指针 + 持久 runtime 目录 + seal durable 语义"三元组重建，**无一窗口丢数据**
- kill 清理顺序：先 release 设备（daemon 持 fd，unlink-while-open 安全）→ 删 runtime 目录 → `delete_record_and_artifacts`（placement 半成品随 artifact root 清除）；硬链接层两侧目录都删后 inode 归零
- 两处曾遗漏、已补进设计文档：① Running 态 kill 的 runtime 目录泄漏（统一 `cleanup_sandbox_runtime(id)` 覆盖 kill/启动回滚/终局 teardown）；② kill 与后台 placement 并发时任务复活 records.db 条目（条件写 + kill 侧 abort 后 await，双保险）
- 例外残留：滚动升级窗口老版本 daemon 删沙箱会留 `sandbox-runtime/` 孤儿目录（D4 代价），新版本启动清扫兜底

## 5. 改动点地图

| 位置 | 改动 |
| --- | --- |
| `src/sandbox/firecracker/sandbox.rs:1246` `pause_to_dir` | 拆分，rootfs seal 参数化（Immediate/Deferred） |
| `src/sandbox/ublk/device.rs:315` 一带 | retained-device 注册表（按 sandbox_id 索引，stop 移交而非 release） |
| `src/orchestrator/persistence/file_backed.rs:336` | record state JSON 加 serde(default) retained 字段；load_paused 孤儿 runtime dir 恢复分支 |
| `src/sandbox/firecracker/sandbox.rs:2033` `start_resume` | live-device 快路径（symlink 复用驱动绑定） |
| 编排层 `src/orchestrator/service.rs:1448` | ④ seal-in-place；⑦ 设备移交；⑦ 后 spawn placement |
| `src/sandbox/firecracker/sandbox.rs:269` `create_captured_snapshot` | unsealed 记录先同步补 placement 再拼 manifest（publish/migrate-paused 路径，易漏） |

## 6. 边界与不做

- 卷设备不动（seal 带上传生命周期 `uploading`/`failed` 状态机）；extra drives 二期
- `pack_recording` 沙箱排除在 retention 外
- 配置默认 `pause_retain_rootfs_device = false`，灰度实测后再定默认值（收益量化见 [[Pause-Resume 内存账本]]：设备保留的 resume 收益 ~5–50ms，占 resume 总耗时 <2–3%，主要价值在高频折返场景）
