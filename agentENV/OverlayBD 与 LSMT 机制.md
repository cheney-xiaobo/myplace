---
title: OverlayBD 与 LSMT 机制
aliases:
  - overlaybd-mechanisms
  - LSMT 机制
tags:
  - agentenv/storage
source: /Users/cheney/work/code/AgentENV
created: 2026-10-31
updated: 2026-10-31
status: verified-read
---

# OverlayBD 与 LSMT 机制

pause/resume 优化涉及的底层原语速查（流程见 [[Pause-Resume 流程与代码地图]]）。

## 1. seal 语义：加 trailer，不拷数据

- `close_seal`：`storage/overlaybd/src/image/image_file.rs:225`（`close_seal_and_reopen` `:302`）
- 测试佐证：`storage/overlaybd/src/image/image_file.rs:1992` `test_close_seal_turns_the_upper_into_a_layer_without_copying`——58GiB upper seal 成本是一次 rename；seal 后 runtime 路径置空防止下次 `prepare_runtime_upper` 截断层
- 二次 seal 拒绝（`already been sealed`）：`storage/overlaybd/src/image/image_file.rs:2051`
- **seal ≠ 合并**：被覆盖块不回收，index trailer 追加，层数 +1

## 2. restack 与写锁

- daemon 原语：`storage/ublk-daemon/src/server.rs:1373` `handle_restack_snapshot` → `create_snapshot_and_restack`
- restack 内部：`storage/overlaybd/src/image/image_file.rs:301` 一带——seal（重开 lower 栈 `:347`）→ 开新 upper（`:341`）→ `stack_files`（`:354`）→ config.lowers.push（`:368`）
- **seal-in-place 技巧**：把 `output_layer_path` 指到 runtime 目录内部即得（同 FS rename、零拷贝、写锁语义复用）——一期方案不需要新原语
- 层序：`lowers` 向量为 bottom→top（restack push 尾部），append 顺序即层序

## 3. RW 布局与 Hybrid in-place

- 三种 `RwLayout`：Sparse / LogStructured / HybridLogStructured（`storage/overlaybd/src/lsmt/file/readwrite.rs:67` 一带；生产 upper 用 Hybrid）
- **Hybrid 给每个唯一逻辑块分配物理槽位，重写走 in-place 覆写，新块才 append**：`storage/overlaybd/src/lsmt/file/readwrite.rs:417`（注释）、`:616` 起（Append/ReuseZero/in-place 三种写 fragment）
- 建文件：`create_file_rw`（`storage/overlaybd/src/lsmt/file/stack.rs:15`）；测试构造 helper：`storage/overlaybd/src/image/image_file.rs:1116` `create_initialized_upper_with_mode`
- 影响：churn 负载下持久 upper 磁盘占用有界（见 [[FlushUpper 二期方案与基准]]）

## 4. merge 原语（唯一"归还"机制）

- `merge_files_ro`：`storage/overlaybd/src/lsmt/file/stack.rs:222`
- `compact_to`：`storage/overlaybd/src/lsmt/file/helper.rs:1421`
- 读旧写新 → 删旧文件 → truncate 立即释放其 cache 页；被覆盖块在此刻才回收
- agent 侧缺的只是编排（后台 merge → CAS image.json → 删旧层），形态同一期后台 placement 任务

## 5. cache backend 与 checkpoint

- `checkpoint`：`storage/overlaybd/src/backend/cache/full_file_cache/cache_entry.rs:613`——**先 fsync data file（`:635`）再原子写 meta.bin（tmp→fsync→rename）**，保证 bitmap 永不声称磁盘上不存在的 block；反向（数据在 bitmap 无）无害，crash 后按 miss 重新拉取
- 数据写入路径**无 fsync**：buffered write + dirty 标记（`storage/overlaybd/src/backend/cache/full_file_cache/cache_entry.rs:661`）
- 触发点：① 300s 周期任务扫 dirty entries（`storage/overlaybd/src/backend/cache/full_file_cache/cache_pool.rs:391`，周期常量 `storage/overlaybd/src/backend/cache/mod.rs:44`）；② 显式 `CachedFile::sync()`（`storage/overlaybd/src/backend/cache/full_file_cache/cache_store.rs:1547`，seal/commit/close 时）；③ entry 初建
- **pause 期间跑几次**：≈ 0（本地 rootfs lowers 不走 cache backend）～ 被显式 sync 的 cache-backed 层数 + 撞上的周期 tick；每 entry 一次 fsync，是 5 分钟窗口的摊销点，**不是每条数据一次**
- 注意：`checkpoint()` 无 dirty 早退，clean entry 也全量重写 meta.bin（bitmap ∝ 缓存块数，大层 MB 级）；优化前需先确认 eviction 路径与 bitmap 落盘的顺序耦合，是正确性问题
- rootfs 写路径 fsync 便宜的原因正在于此：周期 checkpoint 已摊销大部分脏数据，pause/seal 显式 sync 只刷最近窗口残余（闲置沙箱 ~10–50ms）

## 6. image.json 与消费方

- runtime 侧 image.json：一期设计中 pause 同步路径原地重写（seal 的 durable 锚点）
- artifact 侧 image.json：后台任务新建（temp+fsync+原子 rename），无人引用期间写入安全
- `ImageService` 可 Clone（`storage/overlaybd/src/image/image_service.rs:106`），基准中每周期重开 image 无需重建服务
