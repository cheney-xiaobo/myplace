# 设计文档：Pause 保留 rootfs ublk 设备 + 后台自包含化 + Resume 快路径（一期）

状态：提案（Phase 1）
范围：单节点 pause/resume 生命周期的 rootfs 存储路径优化；目标是降低 pause 同步成本、
消除每个 pause/resume 周期的层搬迁，并为 resume 提供设备复用快路径。核心动机：**pause 后更快
释放 guest 内存（FC 进程退出），同时 pause/resume 本身更薄**。

---

## 1. 目标与非目标

### 目标

1. pause 热路径不再对 rootfs 做 restack-to-artifact（seal + rename + 跨 FS copy +
   runtime-owned lowers 搬迁 + 多次 fsync 串行链）。
2. pause 后 rootfs 的 ublk 设备**保留存活**，注册到 `UblkDeviceManager` 的 retained 表；
   resume 时若设备仍活，直接复用（symlink 已有机制），跳过 `create_overlaybd_runtime_device`。
3. 自包含 artifact 布局（sealed 层栈 + config）改由**后台任务**异步产出，record 升级采用
   原子 CAS；完成前 crash 一律回退到 runtime 目录恢复，无数据丢失。
4. 快速 pause/resume 折返（pause 后立即 resume）在 rootfs 侧**零搬迁、零重建**。

### 非目标（一期明确不做）

- extra drives / volume 的 restack 行为不变（`snapshot_extra_drives` 保持现状）。
- 内存快照路径不变（脏页 -> overlaybd raw 层 -> artifact root，仍然在 pause 热路径同步完成）。
- UFFD 惰性内存恢复（独立议题）。
- balloon 空闲页过滤、捕获写路径 O_DIRECT（独立议题）。
- 卷的 seal 生命周期（`uploading`/`failed` 状态机）不动。
- 跨节点迁移目标端的 `start_paused`（migrate-paused）沿用现有 Immediate 模式。
- fork / template capture（`capture_to_dir`）/ pack recording 沙箱：一律走 Immediate 模式。

---

## 2. 现状回顾（关键锚点）

| 环节 | 位置 | 现行为 |
| --- | --- | --- |
| orchestrator pause | `src/orchestrator/service.rs:1448` (`pause_sandbox_impl`) | 分配 artifact root -> detach -> `sandbox.pause()` -> `persist_paused` -> `stop()` |
| 后端 pause | `src/sandbox/firecracker/sandbox.rs:1246` (`pause_to_dir`) | volume sync -> FC pause -> `snapshot_to_dir` |
| rootfs 捕获 | `sandbox.rs:1335` -> `overlaybd_snapshot.rs:636` (`restack_snapshot_overlaybd_rootfs`) | daemon `create_snapshot_and_restack`：seal upper -> **rename 出 runtime 目录** -> 开新 upper；随后 `rewrite_lowers_with_runtime_roots`（`overlaybd_snapshot.rs:416`）把 runtime-owned lowers link/copy 进 artifact；跨 FS 时 `copy_file_atomically` 全量复制 |
| daemon restack | `storage/ublk-daemon/src/server.rs:1373` -> `storage/overlaybd/src/image/image_file.rs:268` (`create_snapshot_and_restack`) | `close_seal_and_reopen`（seal 本体只收尾 index，**数据不搬**）+ rename + fresh upper |
| stop 释放 | `sandbox.rs:1501`（rootfs_runtime）、`:1558`（mem device） | ublk 设备全部释放；work_dir（`TempDir`，`sandbox.rs:176`）Drop 时整目录删除 |
| persisted record | `src/orchestrator/persistence/file_backed.rs:19-37` | `PersistedPausedRecord { version:1, lifecycle: Paused|Resuming, metadata, artifact_root, state }`；`state` = `FirecrackerSnapshotConfig` JSON |
| resume | `sandbox.rs:2033` (`start_resume`) -> `:2103` ublk 重建 -> `:2356` `load_snapshot_file` | 从 snapshot config 重建 rootfs 设备、新 upper、FC 全量读回内存 |
| Paused 态 capture/publish | `service.rs:2074` (`capture_snapshot_from_paused_state`) -> `sandbox.rs:269` (`create_captured_snapshot`) | 直接以 record 里的 config 拼 manifest，发布既有 artifact |

**痛点**：每个 pause/resume 周期 rootfs 层栈 +1、一次 seal+placement 串行链；work_dir 下
runtime 目录随 stop 消失，设备无法跨 pause 存活。

---

## 3. 核心设计决策

### D1：seal（数据落定）与 placement（自包含布局搬迁）分离

seal 本体（`close_seal_and_reopen`）只做 index 收尾 + 开新 upper，数据文件不搬；今天 pause
路径的真正成本在 placement：sealed 层 rename 进 artifact、runtime-owned lowers link/copy
（跨 FS 时退化为全量 copy）、artifact config 重写、`sync_all` 链。

一期方案：pause 热路径只做 **seal-in-place**（新 daemon 原语，见 D3），
placement 整体后移到后台任务。

### D2：runtime 目录从 launch 起就放在持久位置（而非 pause 时移交）

现 runtime 目录在 work_dir（TempDir）下，stop 即删。若 pause 时再"移交所有权"，
存在 crash 泄漏窗口、且 rename 跨 FS 不可行。改为：**launch 时就把 overlaybd runtime 目录
放在 per-sandbox 持久路径**（建议 `<AENV_HOME>/sandbox-runtime/<sandbox_id>/overlaybd`），
work_dir 只保留 socket/symlink/日志等 transient 内容。

收益：

- stop 不再销毁 upper/层文件，设备跨 pause 保留成为可能；
- crash 泄漏治理统一：该目录归 persister 的 per-sandbox artifact 语义管（启动孤儿清理 +
  kill 清理均按 sandbox_id 可枚举）；
- fallback 重建（daemon/节点重启后）的 image.json 路径稳定。

**此决策是方案承重墙**：没有 D2，retained 设备与 crash 恢复都不成立。

### D3：durability barrier 用 seal-in-place（复用既有语义），flush 原语留二期

pause 返回前必须保证 upper 数据 durable。两个候选：

- **A. FlushUpper**（只 fsync log-structured data/index）：零层增长、最便宜，但裸 fsync 后
  crash-replay 的正确性需要 overlaybd LSMT flush 语义逐项验证（`flush_group_commit` 是内部
  group-commit 路径），风险面大。
- **B. SealInPlace**（`close_seal_and_reopen` 去掉 rename/搬迁）：seal 语义已被
  restack 流程充分验证，daemon 改动是"减法"（跳过 rename 与 placement），层栈每周期 +1
  （与今天相同，不再恶化）。

**一期选 B**。层增长交给现有自适应 compaction 预算在后台 placement 时顺带评估。
A 作为二期优化项（独立开关，需 LSMT flush 语义论证 + crash 注入测试）。

### D4：持久化兼容策略 —— lifecycle 枚举不动，新增全部 additive

`PersistedPausedLifecycle` 增加新变体会让**老版本 daemon 反序列化失败**，而
`load_paused` 对坏记录的处置是 `cleanup_invalid_record`（删除）——滚动升级窗口内会丢
Paused 沙箱。因此：

- `lifecycle` 保持 `Paused | Resuming` 不变；
- retained 信息全部放进 `state`（`FirecrackerSnapshotConfig`）的 **serde(default) 新字段**；
  老版本读到会忽略未知字段，其 `validate_persisted` 走到的路径均真实存在（见 5.3），
  老 resume 会自然落到"慢路径重建"——语义正确，只是不快；
- 唯一泄漏：老版本删除沙箱时不认识 runtime 目录，留下孤儿目录。**泄漏优于丢失**，
  由新版本启动孤儿清理兜底。
- `RECORD_VERSION` 不 bump（记录结构未变）。

---

## 4. 总体流程

### 4.1 Pause（retained 模式）

```
orchestrator.pause_sandbox_impl
  ├─ allocate_artifact_root                     (不变)
  ├─ sandbox.pause(root, mode=RetainedRootfs)
  │    ├─ volume sync + FC pause + vm_state.bin  (不变)
  │    ├─ mem dirty 层 -> artifact root           (不变)
  │    ├─ rootfs: daemon SealInPlace             (新：seal + fresh upper，零搬迁)
  │    │    └─ snapshot config 的 rootfs_image_config 指向持久 runtime image.json
  │    ├─ extra drives: 现有 restack-to-dir      (不变，一期范围外)
  │    └─ 返回 FirecrackerSnapshotConfig(retained 标记)
  ├─ persist_paused(record: lifecycle=Paused, state 含 retained 字段)   (不变接口)
  ├─ UblkDeviceManager.retain_rootfs(sandbox_id, {device, image_config_path,
  │    virtual_size, runtime_dir})               (新)
  ├─ sandbox.stop()  —— rootfs_runtime 移交 retained 表而非 release；FC 退出，
  │    guest RSS 在此释放                          (内存收益不变)
  └─ spawn 后台 placement 任务（best-effort，失败仅告警）
       ├─ 硬链接/复制 runtime 层文件 -> artifact root 布局   (link 优先，copy 兜底)
       ├─ 写自包含 image.json（lowers only，无 upper）+ fsync
       └─ CAS put_record：state.rootfs_image_config 切到 artifact 路径（sealed 完成）
```

### 4.2 Resume

```
orchestrator.resume_sandbox_inner -> launch_sandbox(for_resume)
  ├─ UblkDeviceManager.take_retained_rootfs(sandbox_id)
  │    ├─ Some(entry) 且 daemon 探活通过 → 快路径：
  │    │    rootfs_runtime 直接采纳 entry（symlink 设备路径沿用现有驱动绑定机制），
  │    │    取消未完成的后台 placement，跳过 create_overlaybd_runtime_device
  │    └─ None（daemon 重启过 / 已被 kill / 探活失败）→ 回退：
  │         按现有路径以 state.rootfs_image_config 重建设备
  │         （sealed 完成 → artifact 自包含 config；未完成 → 持久 runtime image.json，
  │           D2 保证文件均在）
  └─ 其余步骤不变（mem 设备、网络、hooks、load_snapshot_file、resume、envd）
```

### 4.3 状态机流转

内存 store 的 `SandboxState` 机器不变：`Running -> Pausing -> Paused -> Resuming -> Running`。
变化在 record 语义层新增一个**正交维度** `sealed`（由 state JSON 中 rootfs config 指向的位置
隐式表达）：

```
                 pause(RetainedRootfs)
  Running ───────────────────────────────► Paused(unsealed, 设备 retained)
     ▲                                          │      │
     │  resume(快路径: 设备活)                    │      │ 后台 placement 完成
     │  resume(回退: 设备死)                     │      ▼
     └────────────────────────────────────  Paused(sealed)
              （sealed/unsealed 均可 resume/kill/capture，
                capture on unsealed 先同步补 placement，见 §6.3）
```

不变式：

- I1：`Paused(unsealed)` 记录的 rootfs config 指向持久 runtime 目录，该目录文件**必须**存在
  （seal-in-place 已在 pause 返回前完成）。
- I2：`Paused(sealed)` 记录的 rootfs config 指向 artifact root 自包含布局；
  runtime 目录文件仍被 retained 设备引用（placement 只硬链接/复制，**绝不移动**），直到
  沙箱删除或 resume 放弃设备。
- I3：lifecycle 只在 `Paused <-> Resuming` 间由 CAS 驱动；sealed 维度只由后台任务
  单向推进（unsealed -> sealed），resume/kill 不回退该维度。
- I4：retained 设备注册表是 retained 运行时状态的所有者；`stop()` 不再释放 rootfs 设备，
  释放点收敛到：resume 采纳 / kill 删除 / 启动清扫。

---

## 5. 详细改动点

### 5.1 overlaybd / ublk-daemon（新原语）

**实现简化（一期可不加新协议原语）**：现有 `restack_snapshot` 将 `output_layer_path`
指到 runtime 目录内部（如 `<runtime>/layers/<uuid>.commit`）即是 seal-in-place——
同 FS rename 原子且零拷贝，daemon 端写锁语义原样复用，agent 侧仅省掉 artifact
placement 段。下表 `SealUpper` 作为更干净的原语留二期；一期 §5.3 调用点改为传
runtime 内路径。

| 文件 | 改动 |
| --- | --- |
| `storage/overlaybd/src/image/image_file.rs` | 新增 `seal_upper_in_place(&self)`：直接调用既有 `close_seal_and_reopen()`（`image_file.rs:302` 同源），**不 rename、不搬 lowers、不动 image.json 路径**。返回 sealed 层 descriptor |
| `storage/ublk-daemon/src/lib.rs` / `client.rs` | `DaemonRequest::SealUpper { dev_id }` / `DaemonResponse::SealUpperCreated { descriptor, data_stat }`；client 侧 `seal_upper()`，超时与错误分类对齐 `restack_snapshot`（`client.rs:551`） |
| `storage/ublk-daemon/src/server.rs` | `handle_seal_upper`（结构对照 `handle_restack_snapshot`，`server.rs:1373`）：取 image -> `image_lock` 写锁 -> `seal_upper_in_place` -> 回 descriptor。pool 路径与独占路径同 restack 处理 |

### 5.2 runtime 目录持久化（D2）

| 文件 | 改动 |
| --- | --- |
| `src/sandbox/firecracker/sandbox.rs` | `start_fresh`/`start_resume` 中 runtime 目录来源由 `fc_cwd.join("overlaybd")` 改为 `sandbox_runtime_dir(&self.id)`（新函数，落 `<AENV_HOME>/sandbox-runtime/<id>/overlaybd`；launch 时 `create_dir_all`，创建失败即 fail launch）。`rootfs_runtime`/`extra_drive_runtimes` 的 `image_config_path` 相应落位 |
| `src/sandbox/firecracker/sandbox.rs` (`stop`) | `stop()` 前段不变；**不再删除** runtime 目录（目录不在 work_dir 下后，work_dir TempDir Drop 自然只清 transient 内容） |
| GC 登记 | `sandbox-runtime/<id>` 目录登记进现有"live writable upper 不可回收"清单（`local_store`/缓存 GC 侧的目录排除表），满足 AGENTS.md 存储约束 |

注：extra drives 的 runtime 目录一期留在 work_dir 下不动（它们每个周期仍 restack-to-dir），
避免扩大爆炸半径。

### 5.3 snapshot config 与 paused state（D4）

| 文件 | 改动 |
| --- | --- |
| `src/sandbox/firecracker/config.rs` (`FirecrackerSnapshotConfig` / `FirecrackerCommonConfig`) | 新增 serde(default) 字段：`rootfs_retained: bool`（默认 false）、`rootfs_runtime_dir: Option<PathBuf>`（默认 None）。retained pause 时：`rootfs_image_config.image_config_path = <持久 runtime>/image.json`，两字段置位 |
| `validate_persisted` | 增加 retained 分支：校验 runtime image.json 及其引用的层文件/upper 存在；老字段路径不变。**注意**：老版本 daemon 也会执行此函数，新字段被忽略后其校验的就是 runtime image.json 路径（真实存在）——兼容行为正确 |
| `sandbox.rs:1274` (`snapshot_to_dir`) | 签名加 `mode: RootfsCaptureMode::{Immediate, RetainedRootfs}`。`RetainedRootfs` 分支：daemon `seal_upper` 替代 `restack_snapshot_overlaybd_rootfs`；`rewrite_lowers_*`/link-copy 段跳过；manifest 的 rootfs 路径指向 runtime image.json |
| `sandbox.rs:415` (trait `pause`) | trait `pause(artifact_root, mode)`；orchestrator 按配置传 mode。`start_paused`（migrate 目标端）、`capture_to_dir`（template）、fork/pack-recording 调用点固定传 `Immediate` |

### 5.4 retained 设备注册表

| 文件 | 改动 |
| --- | --- |
| `src/sandbox/ublk/device.rs` (`UblkDeviceManager`) | 新增 `retained_rootfs: DashMap<SandboxId, RetainedRootfs>`，`RetainedRootfs { device: UblkDevice, image_config_path: PathBuf, actual_virtual_size: u64, runtime_dir: PathBuf, artifact_root: PathBuf }`。API：`retain_rootfs` / `take_retained_rootfs`（原子 remove-and-return）/ `drop_retained_rootfs`（release_device + 删 runtime 目录）/ `list_retained`。启动清扫：daemon 重启后所有设备失效，启动时清空该表（daemon probe 已有机制，reusable 探活失败即回收） |
| `sandbox.rs:1501` (`stop`) | `rootfs_runtime` 的释放改为：pause-retained 语义成立时 `retain_rootfs()` 移交；其余情形（failover stop、terminal pause、kill）维持现状 `release_device` |

### 5.5 后台 placement 任务（orchestrator）

| 文件 | 改动 |
| --- | --- |
| `src/orchestrator/service.rs` | pause 成功、`stop()` 完成后 spawn `spawn_rootfs_placement(sandbox_id, config)`（`tokio::spawn`，持 `Arc<Self>`）。JoinHandle 记入 `self.placements: Mutex<HashMap<SandboxId, JoinHandle>>`。步骤：① 读 runtime image.json；② 逐层 `link_or_copy` 到 `<artifact_root>/rootfs/`（复用 `overlaybd_snapshot.rs:184` 既有原语，**禁止 rename/move**，满足 I2）；③ 写自包含 image.json（lowers only）+ `sync_all`（复用 `write_bytes_atomically`）；④ CAS `put_record`：仅当 lifecycle 仍为 Paused 时切换 rootfs config 指向并清 `rootfs_retained` 标记。任一步失败：告警 + 保留 unsealed（I1 仍成立，可重试，可被 fallback 消化） |
| 取消 | resume 快路径 `take_retained_rootfs` 成功后 abort 该任务；kill 流程 abort 后**必须 await JoinHandle 结束**再删 record。④ 必须是条件写（仅当记录存在且 lifecycle==Paused 时落盘），与 await 构成双保险：单靠条件写挡不住"get 后、put 前记录被删"的交错——那会在 records.db 里复活一个已删除的 Paused 记录，重启后成为幽灵沙箱（`mark_resuming` 等现有调用有内存 store 状态机仲裁，后台任务没有）。写了一半的 artifact 文件位于该沙箱 artifact root 内，随删除清理 |

### 5.6 resume 快路径

| 文件 | 改动 |
| --- | --- |
| `sandbox.rs:2033` (`start_resume`) | 进入 ublk 分支前先 `take_retained_rootfs(sandbox_id)`：命中且 daemon 探活通过 → `self.rootfs_runtime = Some(handle)`、`rootfs_image_config_path`/`current_rootfs_virtual_size` 直接采纳，跳过 `create_overlaybd_runtime_device`（`:2107`）；symlink 逻辑复用（`:2104`）。未命中 → 现有重建路径（config 指向 sealed 布局或 runtime 目录，均能工作） |
| 失败回退 | 快路径采纳后若后续任一步失败（网络、hooks、load）：`cleanup_failed_launch` 中对采纳的 retained 设备执行 `release_device` + 删 runtime 目录（该沙箱记录按失败语义回滚，属终局路径） |

### 5.7 kill / 删除 / 启动恢复

| 文件 | 改动 |
| --- | --- |
| `service.rs:1345` (`remove_deleted_sandbox`) | 新增编排助手 `cleanup_sandbox_runtime(sandbox_id)`，**任何状态、任何终局路径**的删除都要调用：① abort+await 该沙箱的 placement 任务（§5.5）；② `drop_retained_rootfs`（设备在表则 release）；③ 删除 `<AENV_HOME>/sandbox-runtime/<id>` —— 注意 **Running 态 kill 同样需要**：D2 之后 `stop()` 不再删该目录，只走 Paused 分支会漏删。调用点：`remove_deleted_sandbox`（kill）、`cleanup_failed_launch`（启动失败回滚）、pause/capture 终局失败后的 teardown（如 `pause_sandbox_impl` 的 terminal 分支）。顺序：先 release 设备（daemon 持有目录内文件的 fd，unlink-while-open 在 Linux 安全），再删 runtime 目录，最后 `delete_record_and_artifacts`（artifact root 内含 placement 半成品，一并清除） |
| `file_backed.rs`（load_paused） | 不改结构；`decode_paused_state` 得到的 retained 字段随 state 走。启动时 `UblkDeviceManager` retained 表为空 → 所有 Paused 沙箱 resume 走回退重建（daemon 重启设备必死，快路径天然不命中，无需额外处理） |
| 孤儿清理 | `cleanup_orphan_artifacts` 扩展：清扫 `sandbox-runtime/` 下不属于任何 record 的目录 |

### 5.8 graceful shutdown

shutdown 时对 Running 沙箱照常逐个 pause（retained 模式同样适用）；对已是 Paused 沙箱，
drain 策略：等待 placement 任务完成（有界超时，如 `shutdown_grace`），超时则 abort——
unsealed 记录完全合法（I1），重启后回退路径可恢复。**不做** shutdown 前同步 seal 全量
（那是今天的行为，正是要省掉的）。

### 5.9 删除清理矩阵

沙箱删除（任何状态、任何终局路径）后必须全部清除的资产与责任点：

| 资产 | 清理点 |
| --- | --- |
| FC 进程 / guest 内存 | `stop()`（现有） |
| work_dir（transient：socket、symlink、日志） | TempDir Drop（现有） |
| rootfs / extra-drive ublk 设备 | Running kill：`stop()` release；Paused kill：`drop_retained_rootfs` |
| retained 注册表项 | `take` / `drop`（DashMap remove 原子） |
| 后台 placement 任务 | abort + await（§5.5） |
| `<AENV_HOME>/sandbox-runtime/<id>` | `cleanup_sandbox_runtime`（§5.7，全部终局路径） |
| artifact root（vm_state、mem 层、placement 半成品） | `delete_record_and_artifacts`（现有） |
| records.db 条目 | 同上（现有）；复活防护见 §5.5 条件写 + await |
| image refs 保护 | `release_image_refs`（现有） |
| 卷 backing | VolumeManager 现有释放路径（现有） |

补充：硬链接 placement 下 sealed 层由 runtime 目录与 artifact root 共享 inode，
两侧目录均删除后引用归零、空间自然回收。已知例外：滚动升级窗口内老版本 daemon
删除沙箱会遗留 `sandbox-runtime/` 孤儿目录（D4，泄漏优于丢失），新版本启动清扫兜底。

---

## 6. Crash 窗口分析

前提：daemon 与 FC 同生命周期（ublk 设备随 daemon 存活）；`put_record` 是 record 唯一权威
写入（LocalKvStore，WAL 持久化等级沿用现状）。

| # | 窗口 | crash 后状态 | 恢复行为 | 数据丢失 |
| --- | --- | --- | --- | --- |
| W1 | pause：seal-in-place 完成后、`persist_paused` 前 | record 不存在（store 仍 Pausing） | store 该沙箱不存在（未持久化）→ 启动视为从未 pause；artifact root 与 runtime 目录成孤儿 | 无（沙箱本就未确认 pause；孤儿由清理扫除） |
| W2 | pause：record 落盘后、`stop()` 前 | record = Paused(unsealed)；FC/daemon 死 | 启动 load → resume 走**回退**：以 runtime image.json 重建设备（D2 文件在） | 无 |
| W3 | pause：`stop()` 后、placement spawn/执行中 | record = Paused(unsealed)；retained 设备死 | 同 W2 回退；placement 半成品文件在 artifact root 内，无引用、随清理 | 无 |
| W4 | placement：层文件就位、CAS 前 | record 仍 unsealed；artifact 有未引用副本 | 回退仍用 runtime 目录；placement 重试幂等（硬链接覆盖） | 无 |
| W5 | placement：CAS 后 | record = sealed；runtime 目录仍在（被引用） | 回退以 artifact 自包含 config 重建；retained 表已无该设备（设备死） | 无 |
| W6 | resume：Resuming 中（mark_resuming 后、load 前） | record lifecycle = Resuming | 现有逻辑已覆盖：启动时 Resuming 记录回滚为 Paused（`rollback_resuming`）→ 再次 resume 回退 | 无 |
| W7 | resume：快路径采纳后、FC load 失败 | store = Resuming；retained 设备被采纳（进程内） | 进程退出 → 设备随 daemon 死；W6 同路恢复 | 无 |
| W8 | daemon 单独重启（宿主不重启） | 所有 retained 设备失效；Paused record 完好 | resume 全部走回退；FC 若还活着会因设备消失 I/O error —— **与现状一致**（今天设备同样不跨 daemon 重启） | 无（受影响的是当时 Running 沙箱，非本设计引入） |
| W9 | 节点掉电 | sealed upper 已 seal-in-place（W2 前置完成）+ record WAL | 回退重建；seal 的 durable 语义与今天 restack 相同（同一 `close_seal` 本体） | 无 |
| W10 | kill 与 placement 并发 | kill 先取 retained 所有权 → abort+**await** 任务退出 → 删目录/设备/record；若 placement 先完成 CAS，kill 删除 sealed record 同样成立 | `take`/`drop` 的互斥由 DashMap remove 原子性保证；await 防止任务在 record 删除后复活 records.db 条目 | 无 |

**总结：没有任何窗口依赖"record 写入前的内存态"；所有中间态都可由
（record 指针 + 持久 runtime 目录 + seal durable 语义）三元组重建。**

---

## 7. 并发交互矩阵

| 操作对 | 交互 | 保证机制 |
| --- | --- | --- |
| resume vs placement | resume `take_retained` 原子取走 → placement abort/放弃 CAS | DashMap remove + CAS lifecycle |
| kill vs retained 设备 | kill 先 take，设备随 kill 释放 | 同上 |
| capture/publish vs unsealed Paused | `capture_snapshot_from_paused_state` 检出 `rootfs_retained` → 先**同步**补 placement（同一任务体，等待完成）再拼 manifest；失败则 capture 失败，沙箱保持 Paused | manifest 路径必须自包含（publish 语义不变） |
| 二次 pause vs 上一轮 placement | 新 pause 分配新 artifact root；旧任务对旧 root 操作，CAS 时发现 lifecycle 非 Paused 而放弃；旧 root 由沙箱级清理回收 | record 单写者 |
| shutdown vs placement | 有界 drain + abort（见 5.8） | — |

---

## 8. 配置与观测

```toml
[sandbox]
# 一期默认关闭，灰度开关
pause_retain_rootfs_device = false
# retained pause 后启动后台自包含化（placement）。seal 本身始终同步（crash 恢复
# 锚点，不可后移）；此开关只控制层搬迁 + record 指针翻转是否后台执行。
# 关闭则 record 永远 unsealed：resume/capture 走回退/同步补齐，语义仍正确。
pause_rootfs_background_placement = true
```

### record 两次写语义（`pause_rootfs_background_placement` 开启时）

`records.db` 的 record（整个 `FirecrackerSnapshotConfig` JSON）被写两次，全值重写：

- **#1（pause 同步落盘）**：`rootfs_image_config.image_config_path = <runtime>/image.json`，
  `rootfs_retained = true`（unsealed 形态，自洽可恢复）
- **#2（后台 placement 完成时）**：先硬链接层文件（幂等）、原子写 artifact image.json
  （新文件），最后单次 put 翻转指针（sealed 形态，自洽可恢复）

T1–T3 任意点 crash：T3 前 = unsealed（runtime 目录完好且已 seal），T3 后 = sealed
（artifact 自包含）。两种形态独立可恢复（W4/W5）。全值重写而非增量：KV 接口无字段级
更新，record 仅 KB 级；指针翻转必须单次原子切换。第二次写防护：kill abort+await +
任务侧每 await 点取消自检 + put 前条件校验（记录存在且 lifecycle==Paused）。

指标（`src/observability` 现有维度上追加）：

- `pause_duration{phase=mem_convert|rootfs_seal|drives|persist|stop}` —— 验证热路径收益的核心；
- `retained_rootfs_devices`（gauge）、`rootfs_placement{result=ok|aborted|failed}`、
  `rootfs_placement_duration`；
- `resume_rootfs_path{path=retained_fast|rebuild_sealed|rebuild_runtime}` —— 快路径命中率。

tracing：placement 各步骤 `debug!`，CAS 放弃/采纳 `info!`，失败 `warn!`（对齐现有
pause/resume 生命周期日志风格）。

---

## 9. 测试计划

| 层 | 内容 | 入口 |
| --- | --- | --- |
| 单元 | `FirecrackerSnapshotConfig` retained 字段 serde 往返 + 老 record（无新字段）decode 兼容 + `validate_persisted` 两分支 | `cargo test -p agentenv --lib` |
| 单元（daemon） | `seal_upper` 请求/响应、错误分类、pool/独占两路径 | `make -C services test` 不覆盖；Rust 侧 `storage/ublk-daemon` 单测 |
| 单元（mock backend） | pause mode 状态机矩阵（Immediate/Retained × 成功/非终局失败/终局失败）、retained 表 take/drop 互斥 | `src/orchestrator/tests.rs`、`src/sandbox/mock.rs` 扩展 |
| 集成（VM） | retained pause→resume 快路径断言（跳过 device create 日志）；pause→pause→pause 层增长符合预算；pause→kill -9 daemon→resume 回退；placement 中断后 record/CAS 幂等；kill 清理 runtime 目录（Running 与 Paused 两态均验证）；placement 执行中 kill：await 后 records.db 不得残留/复活该沙箱条目 | `make test-agent-integration` + `scripts/tests/`（需 KVM/ublk 环境） |
| crash 注入 | W1–W5 定点：用断电模拟（record 写入前后、CAS 前后 kill daemon + 清 page cache） | `make test-ublk` 环境手动 + 脚本化 |
| 回归 | 全量 pause/resume/fork/snapshot 套件在开关关闭时与主分支行为逐字节一致（mode=Immediate 全走旧路径） | `make test` |

验收标准：

1. 开关开启时，retained pause 的 `rootfs_seal` 段耗时显著低于原 restack+placement 总和
   （无跨 FS copy 命中时接近纯 index 收尾）；
2. pause→立即 resume 折返 N=10 次层栈增长 ≤ 现有自适应预算允许值，且 resume 全部命中
   `retained_fast`；
3. §6 全部窗口注入后 resume/kill 数据一致（rootfs 写入无丢失）；
4. 开关关闭时 CI 全绿且无行为 diff。

---

## 10. 风险与未决问题

| 风险 | 缓解 |
| --- | --- |
| 混部窗口：老版本 daemon 删除沙箱遗留 runtime 目录孤儿 | 接受泄漏；新版本启动清扫兜底；发布说明注明 |
| `sandbox-runtime` 持久目录进入缓存 GC 视野 | §5.2 登记；`persistence-artifact-inventory.md` 文档同步更新 |
| retained 设备占住 daemon 资源（fd/页缓存）上限 | retained 数量指标 + 可选 `release_after_seal`（一期不做，默认保页缓存热） |
| `seal_upper_in_place` 与 restack 并发（placement 任务不会触发，但 capture-on-unsealed 同步补 seal 时设备可能仍被 retained 引用） | capture 走同步 placement **不触碰设备**（只链接文件）；若需 seal（upper 非空，即 resume 后又 pause 前 capture）该路径本就走 Immediate 模式，无并发 |
| 老 `FirecrackerPausedState::create_captured_snapshot` 对 retained config 拼 manifest 会得到非自包含路径 | 已在 §7 交互矩阵覆盖：retained 检出 → 先同步补 placement |
| 二期方向：FlushUpper 原语（零层增长 barrier）、extra drives/volumes 纳入 retention、`release_after_seal` 降页缓存、retention 与 P2P 发布协同 | 本文档不展开 |

---

## 11. 改动文件一览

```
storage/overlaybd/src/image/image_file.rs      seal_upper_in_place
storage/ublk-daemon/src/{lib,client,server}.rs SealUpper 原语
src/sandbox/ublk/device.rs                     retained 注册表 + 启动清扫
src/sandbox/firecracker/config.rs              SnapshotConfig 新字段 + validate 分支 + runtime_dir 来源
src/sandbox/firecracker/sandbox.rs             snapshot_to_dir(mode)、stop 移交、start_resume 快路径
src/sandbox/firecracker/overlaybd_snapshot.rs  （复用 link_or_copy/write_bytes_atomically，无新增大块）
src/sandbox/backend.rs                          trait pause(mode)
src/orchestrator/service.rs                     pause_sandbox_impl spawn placement、kill 扩展、shutdown drain
src/orchestrator/persistence/file_backed.rs     （结构不变；孤儿清理扩展）
src/cfg.rs + config/default.toml                开关
docs/src/internals/persistence-artifact-inventory.md  runtime 目录归属说明
```

验证命令：`make fmt && make clippy && make test-unit`；集成见 §9。
