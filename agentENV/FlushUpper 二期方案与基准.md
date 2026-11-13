---
title: FlushUpper 二期方案与基准
aliases:
  - flush-upper
  - 持久 upper 方案
tags:
  - agentenv/design
  - agentenv/pause-resume
  - agentenv/benchmark
source: /Users/cheney/work/code/AgentENV
created: 2026-10-31
updated: 2026-10-31
status: proposal-with-data
---

# FlushUpper 二期方案与基准

现状每 pause 新建层导致的累积问题，与"持久 RW upper 续写"方案（mem + rootfs 统一）及本机基准验证。

## 1. 问题：无界累积

LSMT append-only + 每 pause seal 新层的组合下（现状流程见 [[Pause-Resume 流程与代码地图]]）：

| 无界项 | 机制 |
| --- | --- |
| 磁盘 Σ | 被覆盖的块也不回收（`close_seal` 不合并，`storage/overlaybd/src/image/image_file.rs:225`），churn 下同批块反复写、每周期留永久增量 |
| 文件数 / daemon anon / fd | 每周期 mem+1、rootfs+1，pause/resume 路径无任何 merge |
| cache 污染 | 每周期 delta 的 clean 页滞留，挤占其他沙箱工作集 |
| mem 读放大 | resume `load_snapshot_file` 全量读 ΣD，随周期数线性变慢 |

## 2. 关键机制发现：HybridLogStructured 支持 in-place 覆写

这推翻了"upper 是纯 log、不 seal 照样涨"的直觉。代码证据：

- `storage/overlaybd/src/lsmt/file/readwrite.rs:417`：*"Sparse and Hybrid can rewrite existing data blocks in place"*
- `storage/overlaybd/src/lsmt/file/readwrite.rs:616` 起：hybrid 写路径分 `Append` / `ReuseZero` / in-place 三种 fragment；重写已有块走原物理槽位，只有新块 append

**推论：churn 负载（同工作集反复脏）下，持久 upper 的磁盘占用 = 唯一工作集大小，与周期数无关。** merge 降级为只服务"工作集持续扩张"型负载。

## 3. 方案设计（Phase-2a / 2b）

```text
布局: per-sandbox 持久 RW upper（sandbox-runtime/<id>/，base/lowers 不可变绝不写入）
pause k: mem 脏页追加写 mem.upper；rootfs 只 flush+fsync（不 seal 不搬层）
resume:  retained/重开 base+upper → mem ublk 摊平 → load_snapshot_file 不变（改动全在块层下，FC 无感知）
降级:    fork/capture/publish 需不可变层时，先 seal 当前 upper（复用一期 seal-in-place）
Phase-2b: merge 预算触发（层数/大小超限）→ merge_files_ro → CAS image.json → 删旧文件（cache 页唯一归还点）
```

正确性要点：版本序自然正确（log 中新版本排在旧版本后，摊平后者胜出）；vm_state 顺序不变（flush upper → vm_state → record）；**D3 升级硬前置**——agent 需新增"重开既有 RW upper"恢复路径，LSMT RW torn-tail 重开语义必须验证 + crash 注入测试；fallback 可用"恢复时先 close_seal 再开新层"。

## 4. 基准实测

用例：`storage/overlaybd/src/image/image_file.rs:2092` `bench_pause_churn_new_layer_vs_flush_upper`（`#[ignore]` 门控，不进 CI）

```bash
cargo test -p overlaybd --release --lib -- --ignored --nocapture bench_pause_churn
```

参数：1GiB 虚拟盘，12 周期 × 16MiB churn（同工作集每周期重写、per-cycle marker 校验最新版本胜出），macOS/APFS 单次运行。

| 指标 | A：每周期新层（现状） | B：持久 upper 续写 |
| --- | --- | --- |
| 每周期写 | 18–34ms（冷文件） | **7–8ms（暖文件，首周期 20ms）** |
| 每周期 seal | ~0.3ms | 无 |
| 每周期栈重建 open | 0.2→5.0ms **随层数线性涨**（≈0.4ms/层） | 无 |
| 12 周期后工作集读回 | 5.2ms | 4.1ms |
| **文件数 / 磁盘** | **39 个 / 192.9 MiB** | **3 个 / 15.9 MiB（≈唯一工作集）** |

换算生产量级：D=1GiB/周期、50 周期 churn → A ≈ 50 GiB 磁盘 + 重建线性涨；B ≈ 1 GiB 封顶、零重建。

**诚实标注**：绝对值在 Linux/NVMe 会不同，但 in-place vs append、文件数、重建线性涨三个结构性结论由代码决定；读回差距小是因为 churn 全命中顶层，非 churn（最新版本分散深层）未覆盖；on-disk 结论已与 `storage/overlaybd/src/lsmt/file/readwrite.rs:417` 交叉验证。

## 5. 页缓存结论（修正版）

- B 方案 cache 占用 = 唯一工作集（in-place 覆写在同批页上打转），**有界**；A 每周期新 clean 页滞留，cache 累积反而是 A 的问题
- 两案的 clean 页都可被内核零成本回收，不是泄漏；真正代价是挤占工作集
- **mem 与 rootfs 的 fadvise(DONTNEED) 有效性截然不同**：rootfs 旧 delta 不被读就不 touch，fadvise 有效；mem 每次 resume 全量读摊平，所有历史 delta 页每轮被 touch，**fadvise 无效，mem 唯一出口是 merge**（merge 同时修复 cache 与 resume 读放大，双份红利）
- app 方案比现状少一份瞬时双占（现状 sealed delta 拷贝瞬时 2×R cache + IO）

## 6. 落位

一期先行（[[Rootfs Retention 一期设计]]，seal-in-place 灰度）→ Phase-2a 统一 FlushUpper（D3 硬前置）→ **Phase-2b merge 必须紧随不能推迟**。rootfs 侧 fadvise 作为 merge 间隙补充手段。
