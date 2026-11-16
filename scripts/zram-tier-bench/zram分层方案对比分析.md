# 沙箱暂停池：多 zram priority 分层 vs zram + writeback

> 场景前提：整机 swap 只服务沙箱暂停内存，暂停期页确定不被访问，冷热由业务语义给定而非内核推断；换出时机可控、resume 延迟是 SLA 指标。

**结论：两者不是替代关系，最优是"B 管时机、A 管兜底"的组合。** 单看一个：B 更贴合沙箱语义（下沉时机可控），A 更扛吞吐（换出快、实现简单）。真正的分野在于你要的是"精细调度"还是"容量弹性"。

---

## 一、机制本质差异

| 项 | A：多 zram + priority | B：zram + writeback |
|---|---|---|
| 分层载体 | 多个 swap 设备 | 单个设备的"内存态/远端态"两种状态 |
| 决策者 | 内核 swap 分配器（prio + 簇轮转） | 编排层显式触发 sysfs |
| 决策信号 | 内存压力（何时换出） | pause 时长 / 恢复概率（何时下沉） |
| 页的归属 | 一次分配即固定，不回迁 | 可下沉、可读回，状态可逆 |
| 分配粒度 | 簇（默认 256 页） | 单页（逐 slot 遍历） |

A 是"多个抽屉，按优先级往里放"；B 是"一个抽屉 + 一个仓库，你可以决定什么时候搬"。

---

## 二、图 1：多 zram 按 priority 分层

```html type="renderer"
<html style="margin:0;padding:0;">
<div style="background-color:transparent;box-sizing:border-box;font-family:'Roboto','PingFang SC','Segoe UI',Arial,sans-serif;color:#1A1B1C;padding:4px 0;">

  <div style="font-size:15px;font-weight:600;">多 zram 按 priority 分层的实现结构</div>
  <div style="font-size:12px;color:#6B7280;margin-top:3px;">机制示意：prio 只决定"新页去哪"，已分配的页不回迁</div>

  <div style="margin-top:14px;padding:10px 14px;border-radius:12px;background:rgba(139,200,234,0.14);border:0.5px solid rgba(0,0,0,0.08);">
    <div style="font-size:13px;font-weight:600;">① 页需要离开物理内存</div>
    <div style="font-size:12px;color:#4B5563;margin-top:3px;">kswapd 回收｜直接回收｜编排层主动 MADV_PAGEOUT / cgroup memory.reclaim</div>
  </div>

  <div style="text-align:center;color:#6B7280;font-size:13px;line-height:20px;">▼</div>

  <div style="padding:12px 14px;border-radius:12px;background:#fff;border:0.5px solid rgba(0,0,0,0.08);">
    <div style="font-size:13px;font-weight:600;">② swap 分配器：按 priority 分桶取设备</div>
    <div style="display:flex;flex-wrap:wrap;gap:8px;margin-top:8px;">
      <div style="flex:1 1 200px;min-width:0;padding:8px 10px;border-radius:8px;background:rgba(0,0,0,0.02);font-size:11.5px;color:#4B5563;line-height:1.55;">
        可用设备挂在 swap_avail_heads 上，永远先取最高 prio 的桶
      </div>
      <div style="flex:1 1 200px;min-width:0;padding:8px 10px;border-radius:8px;background:rgba(0,0,0,0.02);font-size:11.5px;color:#4B5563;line-height:1.55;">
        同 prio 多设备 → 桶内簇轮转（默认 256 页/簇）
      </div>
      <div style="flex:1 1 200px;min-width:0;padding:8px 10px;border-radius:8px;background:rgba(0,0,0,0.02);font-size:11.5px;color:#4B5563;line-height:1.55;">
        高 prio 写满 / mem_limit 到顶 → 标记不可用 → 降到下一档
      </div>
    </div>
  </div>

  <div style="text-align:center;color:#6B7280;font-size:13px;line-height:20px;">▼</div>

  <div style="display:flex;flex-wrap:wrap;gap:8px;">
    <div style="flex:1 1 190px;min-width:0;padding:10px 12px;border-radius:10px;background:rgba(139,200,234,0.22);border:0.5px solid rgba(0,0,0,0.08);">
      <div style="font-size:12px;font-weight:600;">prio 100 · zram0</div>
      <div style="font-size:11.5px;color:#4B5563;margin-top:4px;line-height:1.5;">本地 RAM，zstd<br>热页第一落点<br>换入即释放</div>
    </div>
    <div style="flex:1 1 190px;min-width:0;padding:10px 12px;border-radius:10px;background:rgba(139,200,234,0.14);border:0.5px solid rgba(0,0,0,0.08);">
      <div style="font-size:12px;font-weight:600;">prio 50 · zram1</div>
      <div style="font-size:11.5px;color:#4B5563;margin-top:4px;line-height:1.5;">本地 RAM，lz4<br>次级缓冲<br>填满后长期驻留</div>
    </div>
    <div style="flex:1 1 190px;min-width:0;padding:10px 12px;border-radius:10px;background:rgba(250,173,20,0.16);border:0.5px solid rgba(0,0,0,0.08);">
      <div style="font-size:12px;font-weight:600;">prio 10 · 远端块设备</div>
      <div style="font-size:11.5px;color:#4B5563;margin-top:4px;line-height:1.5;">NVMe-oF / pmem 块设备<br>不压缩，1:1 传输<br>冷页兜底</div>
    </div>
  </div>

  <div style="text-align:center;color:#6B7280;font-size:13px;line-height:20px;">▼ / ▲ 读回</div>

  <div style="padding:10px 14px;border-radius:12px;border:1px dashed rgba(0,0,0,0.18);">
    <div style="font-size:12.5px;font-weight:600;">③ 页表 swap entry = type（设备序号）+ offset（簇内槽位）</div>
    <div style="font-size:11.5px;color:#6B7280;margin-top:3px;">读回时按 type 直达对应设备，不需要再做选择；页一旦落下就不会因上层腾空而迁移</div>
  </div>

</div>
</html>
```

### 图 1 要点

- `swap_avail_heads` 按 prio 分桶，永远从最高优先级可用桶取；
- 同 prio 多设备在桶内簇轮转，粒度是簇（4K 页下 256 页 ≈ 1MB）；
- 高优先级设备写满或 `mem_limit` 到顶返回 ENOSPC → 标记不可用 → 自动降级；
- **prio 是静态配置，不感知 pause 时长**，无法表达"暂停超过 30 分钟才允许沉远端"；
- 已落在低优先级设备上的页不会因为高优先级腾出空间而迁回。

---

## 三、图 2：zram + writeback

```html type="renderer"
<html style="margin:0;padding:0;">
<div style="background-color:transparent;box-sizing:border-box;font-family:'Roboto','PingFang SC','Segoe UI',Arial,sans-serif;color:#1A1B1C;padding:4px 0;">

  <div style="font-size:15px;font-weight:600;">zram + writeback 的两级存储实现</div>
  <div style="font-size:12px;color:#6B7280;margin-top:3px;">机制示意：zram 是缓存层，backing_dev 是下沉层，写回由编排层显式触发</div>

  <div style="display:flex;flex-wrap:wrap;gap:12px;margin-top:14px;">

    <div style="flex:1 1 300px;min-width:0;">

      <div style="padding:10px 12px;border-radius:12px;background:rgba(162,221,170,0.22);border:0.5px solid rgba(0,0,0,0.08);">
        <div style="font-size:12.5px;font-weight:600;">① 换出页写入 zram0（内存态）</div>
        <div style="font-size:11.5px;color:#4B5563;margin-top:3px;line-height:1.55;">压缩后交给 zsmalloc，优先当前 CPU 的 per-CPU 池<br>槽位状态：内存态（占 RAM，可极快读回）</div>
      </div>

      <div style="text-align:center;color:#6B7280;font-size:12px;line-height:22px;">▼ 显式触发（pause 超阈值）</div>

      <div style="padding:10px 12px;border-radius:12px;background:rgba(139,200,234,0.18);border:0.5px solid rgba(0,0,0,0.08);">
        <div style="font-size:12.5px;font-weight:600;">② 写回 backing_dev（远端块设备）</div>
        <div style="font-size:11.5px;color:#4B5563;margin-top:3px;line-height:1.55;">写出的是压缩后数据；zsmalloc 立即释放 → 本地内存真正下降<br>槽位状态：WB 态（数据在远端，本地只剩元数据）</div>
      </div>

      <div style="text-align:center;color:#6B7280;font-size:12px;line-height:22px;">▲ 缺页读回</div>

      <div style="padding:10px 12px;border-radius:12px;background:rgba(250,173,20,0.16);border:0.5px solid rgba(0,0,0,0.08);">
        <div style="font-size:12.5px;font-weight:600;">③ 读回：WB slot → 远端读压缩块 → 解压</div>
        <div style="font-size:11.5px;color:#4B5563;margin-top:3px;line-height:1.55;">逐页同步提交，发生在缺页路径<br>这里就是 resume 延迟的来源</div>
      </div>

    </div>

    <div style="flex:1 1 220px;min-width:0;padding:12px 14px;border-radius:12px;background:rgba(0,0,0,0.025);border:0.5px solid rgba(0,0,0,0.08);">
      <div style="font-size:12.5px;font-weight:600;">控制面（sysfs）</div>
      <div style="font-size:11.5px;color:#4B5563;margin-top:6px;line-height:1.9;">
        backing_dev 指定下沉目标<br>
        idle 标记哪些页算冷<br>
        writeback 触发下沉动作<br>
        writeback_limit 下沉总量上限
      </div>
      <div style="margin-top:10px;padding-top:8px;border-top:0.5px solid rgba(0,0,0,0.08);font-size:11px;color:#6B7280;line-height:1.6;">
        注意：zram 元数据重启即丢，<br>backing 需配启动清理流程
      </div>
    </div>

  </div>

</div>
</html>
```

### 图 2 要点

- 同一页可在"内存态（zsmalloc）"与"WB 态（backing 上）"间切换，切换权在编排层；
- 写回的是压缩后数据，带宽省约 60%，但**仍按页逐次同步提交**，I/O 次数不减；
- 代价集中在第 ③ 步：读回发生在缺页路径，远端 RTT 直接变成 resume 延迟；
- zram 元数据重启清空，backing 上的旧数据会变成无法回收的脏数据。

---

## 四、十维对比

| # | 维度 | A | B | 判定 |
|---|---|---|---|---|
| 1 | 下沉时机可控 | 被动 | 显式 | **B** |
| 2 | 按沙箱选页 | 否 | 否（只能 idle/huge） | 平 |
| 3 | 下沉吞吐 | 高（直写、簇级） | 低（逐页同步） | **A** |
| 4 | resume 延迟 | 本地层极低；远端层 = RTT | 本地=解压；远端=RTT+解压 | 平 |
| 5 | 带宽消耗 | 1:1 | 约 1/2.5 | **B** |
| 6 | 内存释放确定性 | slot free 即时 | 写回后立即释放 | 平 |
| 7 | 故障域 | 远端设备挂 → 其上所有沙箱受损 | 同左，范围限于已下沉部分 | 平 |
| 8 | 可观测性 | 设备级 | 设备级 + 写回量/失败计数 | B 略优 |
| 9 | 实现复杂度 | 低（zramctl + swapon） | 中（backing 分区、清理、限流） | **A** |
| 10 | 重启语义 | 天然易失、无残留 | backing 脏数据需清理 | **A** |

---

## 五、定量账（RDMA 远端假设）

| 场景 | A（远端块设备做 swap） | B（压缩后写回） |
|---|---|---|
| 1GB 沙箱下沉，RTT 100µs | 26.2 万页 × 4K，传输 1GB，串行约 26s | 26.2 万次提交，传输约 400MB，串行约 26s |
| 1GB 沙箱下沉，RTT 500µs | 约 131s | 约 131s |
| resume 命中本地 | 解压 10–50µs/页 | 解压 10–50µs/页 |
| resume 命中远端 | RTT + 原始页传输 | RTT + 压缩块传输 + 解压 |

**关键反直觉点：B 省的是带宽，不是往返次数。** 在 RTT 主导的场景里两者下沉耗时几乎相同，压缩只降低远端存储与网络带宽需求；跨机架/跨可用区时 B 的收益才明显，同机架 IB 则可视为纯 CPU 浪费。

---

## 六、风险矩阵

| 风险 | A 概率/影响 | B 概率/影响 | 缓解 |
|---|---|---|---|
| 回收路径被远端 I/O 拖住 | 高 / 极高 | 中 / 高 | 主动换出；禁止远端层参与 kswapd |
| 热沙箱被误沉 | 中 / 高 | 低 | 分层阈值 ≥ resume SLA 的 3 倍 |
| 远端掉线致数据丢失 | 中 / 高 | 中 / 高 | 单沙箱隔离 + 快速销毁 + 错误处理 |
| backing 脏数据堆积 | — | 高 / 中 | 启动清零 / 分区轮换 |
| 出口线程卡死 | — | 高 / 高 | 异步队列 + 并发限流 + writeback_limit |

---

## 七、决策树

```
首要诉求是什么？
├─ 只是"内存不够时有个兜底"        → A：zram 高 prio + 远端块设备低 prio
├─ 要按 pause 时长精确控制下沉      → B：zram + backing_dev + 阈值触发
├─ 远端带宽贵 / 跨可用区            → B（压缩后传输省约 60%）
├─ 暂停沙箱多、单个 GB 级、要快      → A（吞吐高，不引入压缩环节）
└─ 要按沙箱整块迁移、跨节点搬        → A、B 都不够，必须框架层 snapshot
```

---

## 八、推荐架构：三层组合（A + B）

```
运行中沙箱        → 本地 RAM
短暂停（秒级）    → 本地 zram（prio 高），只做容量缓冲
长暂停（分钟+）   → writeback 到远端块设备（压缩后传输）
极端冷（小时+）   → 远端块设备作为最低 prio 的 swap 兜底
```

- 本地层用 A 的思路：简单、快、换入即释放；
- 下沉用 B 的机制：显式触发、带宽省约 60%；
- 最低层用 A 兜底：避免写回失败后无处可去。

---

## 九、配置骨架

### A：纯 swap 分层

```bash
zramctl --find --algorithm zstd --size 8G     # zram0
swapon -p 100 /dev/zram0                       # 高 prio 先用
swapon -p 10  /dev/nvme0n1                     # 远端块设备兜底
sysctl -w vm.swappiness=1                       # 别让远端层参与热路径
```

### B：写回时机由调度器控制

```bash
echo 8G > /sys/block/zram0/disksize
echo /dev/nvme0n1p4 > /sys/block/zram0/backing_dev    # 容量须 ≥ disksize
echo huge_idle > /sys/block/zram0/idle
echo 1 > /sys/block/zram0/writeback_limit_enable
echo 2048M > /sys/block/zram0/writeback_limit
# 由编排层在"pause 超阈值"后触发，绝不放在 pause 的同步路径里
echo huge_idle > /sys/block/zram0/writeback
```

---

## 十、必须写进评审会的两句前提

1. **A 的 prio 是静态配置，不感知 pause 时长**——把它当"分层决策器"会误沉热沙箱；
2. **B 的写回是逐页同步**——放在 pause 的同步路径里会拖住出口线程，必须异步 + 限流。

一句话收尾：如果只能选一个，选 B——沙箱场景最稀缺的能力是"什么时候动"，不是"动得多快"；如果能设计，用 A 做本地缓冲与兜底、B 做下沉，再给框架层 snapshot 留个后门。
