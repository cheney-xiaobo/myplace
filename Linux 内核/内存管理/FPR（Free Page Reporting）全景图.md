# FPR（Free Page Reporting）全景图

> **FPR = Free Page Reporting**：guest 把「已经释放、回到 buddy 的空闲页」通过 virtio-balloon 报告给 VMM；VMM 对宿主对应内存做 punch hole / `MADV_DONTNEED`，降低宿主 RSS、提高单机内存密度。
>
> 本图把 **guest 内核 → guest virtio 驱动 → virtio 协议 → VMM（CubeSandbox/Cloud Hypervisor fork）→ 宿主 OS** 五段串起来，并给出使能条件、参数、判据与坑位。
>
> - guest 内核：`/Users/cheney/work/code/ub/kernel`（OLK-6.6，HEAD `8003dfcd`）
> - VMM：`/Users/cheney/work/code/CubeSandbox`（Cloud Hypervisor fork）
> - 核对日期：2026-11-15；kernel 行号已用 `check_linerefs.py` 校验

---

## 0. 一张全景图

```
┌─ Guest 内核 ───────────────────────────────────────────────────────────────┐
│ free_unref_page() / __free_pages_ok()                                       │
│   └─ page_reporting_notify_free(order)          mm/page_alloc.c:899         │
│        │  （order ≥ page_reporting_order 且 static key 已开）              │
│        └─ queue_delayed_work(2s)                 mm/page_reporting.c:50/84  │
│             └─ page_reporting_process_zone()     mm/page_reporting.c:261    │
│                  ├─ 按 zone / order / migratetype 扫空闲链表                │
│                  ├─ 每 32 段为一组（PAGE_REPORTING_CAPACITY）               │
│                  ├─ prdev->report()  ───────────────┐                       │
│                  └─ page_reporting_drain()           │ 放回 buddy +          │
│                     （SetPageReported）              │ 标记「已报告」        │
└──────────────────────────────┬───────────────────────┴───────────────────────┘
                               │ report(sg, nents)
┌─ Guest virtio-balloon 驱动 ───▼─────────────────────────────────────────────┐
│ virtballoon_free_page_report()   drivers/virtio/virtio_balloon.c:222        │
│   ├─ virtqueue_add_inbuf(reporting_vq, sg, nents)                           │
│   ├─ virtqueue_kick()                                                       │
│   └─ wait_event(vb->acked)  ← 等 VMM 消费完                                  │
└──────────────────────────────┬─────────────────────────────────────────────┘
                               │ VIRTIO_BALLOON_F_REPORTING（bit 5）
┌─ virtio 协议 ─────────────────▼─────────────────────────────────────────────┐
│ reporting virtqueue：REPORTING_QUEUE_SIZE = 32                              │
│ 每个 scatterlist 段 = 一段 guest 物理页/块（PFN 范围）                      │
└──────────────────────────────┬─────────────────────────────────────────────┘
┌─ VMM（CubeSandbox / Cloud Hypervisor fork）─▼──────────────────────────────┐
│ process_reporting_queue()        hypervisor/virtio-devices/src/balloon.rs   │
│   └─ release_memory_range()      （同一文件，见 §4）                        │
│        ├─ MAP_SHARED 文件后端：fallocate(PUNCH_HOLE|KEEP_SIZE)             │
│        └─ 其余：madvise(MADV_DONTNEED)                                      │
└──────────────────────────────┬─────────────────────────────────────────────┘
                               │
┌─ 宿主 OS ────────────────────▼─────────────────────────────────────────────┐
│ 页框真正归还 → VMM 进程 RSS 下降 → 同一台机器可塞更多 sandbox              │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## 1. 三侧职责

| 侧 | 负责什么 | 关键点 |
|---|---|---|
| guest 内核 | 判定「哪些空闲块值得报告」、批量、节流、防重复 | 只报 **order 足够大**的空闲块；2s 延迟 |
| guest 驱动 | 把块打包成 scatterlist 发给 VMM，等 ack | reporting vq 容量必须 ≥ 32 |
| VMM | 校验 GPA 范围、punch hole / madvise | 只对 guest 内存区域做；MAP_SHARED 走 punch hole |
| 宿主 OS | 真正回收页框 | VMM RSS 下降是唯一有效判据 |

---

## 2. guest 内核侧：什么时候报、报什么、报多少

### 2.1 触发点

- 每次把页放回 buddy 时调 `page_reporting_notify_free(order)`（`mm/page_alloc.c:899`）
- 只有 `order >= page_reporting_order` 且 `page_reporting_enabled` 静态键已开才会真正排队（`mm/page_reporting.c:60-90`）

### 2.2 节流与批量

| 常量 | 值 | 位置 | 含义 |
|---|---|---|---|
| `PAGE_REPORTING_DELAY` | 2 秒 | `mm/page_reporting.c:50` | 攒够一批再报，避免频繁唤醒 |
| `PAGE_REPORTING_CAPACITY` | 32 | `include/linux/page_reporting.h:8` | 每次 report 最多 32 段 |
| `page_reporting_order` | -1 → 驱动值 → `pageblock_order` | `mm/page_reporting.c:360-375` | 最小报告 order |
| watermark 保护 | low + 32 × 2^order | `mm/page_reporting.c:261-271` | 空闲不足时跳过，保证前进 |

- 扫描顺序：zone → order（从 `page_reporting_order` 到最高）→ migratetype；跳过 isolate
- 报告失败：`page_reporting_drain` 会把页放回原 free list，不标记（`mm/page_reporting.c:106-130`）
- 成功：`__SetPageReported(page)`；页被重新分配时 flag 清除，之后可再次报告
- 已报告且未被重新分配的页不会重复上报

### 2.3 order 的选择（决定「报得出去吗」）

- 默认 `pageblock_order`：4K 页时为 9（2 MiB）
- ARM64 64K 页时 `pageblock_order` 可能是 13（512 MiB），virtio-balloon 显式设 `order=5`（2 MiB），避免频繁拆 THP（`drivers/virtio/virtio_balloon.c:1137-1160` 的注释与赋值）
- 可用内核参数 `page_reporting_order=N` 覆盖；太小会碎片化、太大会等不到合并

### 2.4 注册与开启

- `page_reporting_register(prdev)`（`mm/page_reporting.c:352`）：
  - 校验只有一个 prdev（`-EBUSY`）
  - 决定 order、初始化 delayed work、先跑一次全 zone flush
  - 打开 `page_reporting_enabled` 静态键，打印 `Free page reporting enabled`
- 注销：`page_reporting_unregister`，必须在驱动卸载前调用

---

## 3. 使能条件（逐项核对）

| 条件 | 怎么查 | 本树/项目现状 |
|---|---|---|
| guest 内核 `CONFIG_PAGE_REPORTING=y` | `zcat /proc/config.gz \| grep PAGE_REPORTING` | x86-dc:1169 `y` |
| guest 有 virtio-balloon 驱动 | `CONFIG_VIRTIO_BALLOON` | x86-dc:7276 `m`（需加载） |
| guest 能看到 balloon 设备 | `ls /sys/bus/virtio/devices/*/features` 或 dmesg | 由 VMM 决定 |
| VMM 提供 feature | `--balloon size=0,free_page_reporting=on` | Cloud Hypervisor 语法；`CubeShim` 默认 `free_page_reporting: true`（`CubeShim/shim/src/hypervisor/config.rs` 第 176 行） |
| reporting vq 容量 ≥ 32 | VMM 侧 `REPORTING_QUEUE_SIZE=32` | CubeSandbox `balloon.rs` 第 40 行 |
| 设备拓扑在快照里保留 | 恢复后 `dmesg` 看 balloon/reporting | 旧模板快照可能没有 balloon，恢复后不生效（E2E README 明确说明，需要 redo 模板） |

启动后 guest 侧确认：
```bash
dmesg | grep -i "Free page reporting"     # 期望出现 enabled
cat /sys/module/page_reporting/parameters/page_reporting_order  # 若编译为模块
cat /proc/buddyinfo                        # 看是否有高阶空闲块
```

---

## 4. VMM 侧：收到报告后做什么

CubeSandbox（Cloud Hypervisor fork）处理链：

1. `process_reporting_queue()` 从 reporting vq 弹 descriptor chain，逐段取 GPA 和长度（`hypervisor/virtio-devices/src/balloon.rs` 第 286 行起）
2. `release_memory_range()`：
   - `find_region()` 校验 GPA 落在 guest 内存区域内，并按区域边界 clamp 长度
   - 若区域是 `MAP_SHARED` 且带 file offset：先 `fallocate(PUNCH_HOLE|KEEP_SIZE)` 打洞（绝不 punch MAP_PRIVATE 的镜像/共享 base）
   - 然后 `madvise(MADV_DONTNEED)` 丢弃该映射的匿名页（`balloon.rs` 第 135 行起）
   - punch 失败只 warn，回退 madvise
3. 每段处理完 `add_used` 并触发中断，guest 的 `wait_event(acked)` 被唤醒

> 关键语义：VMM 只是「被告知这些页 guest 不用了」，真正释放靠宿主 `madvise`/`punch hole`；guest 侧内存早已 free，`MemFree` 不变。

---

## 5. 生命周期（一次完整循环）

```
分配/触碰 1536 MiB        → VMM RSS 上升（峰值）
进程退出 / free           → 页回 buddy，但可能碎片化
drop_caches + compact ×3  → 合并成 ≥2 MiB 块（否则永远报不出去）
notify → 2s 延迟          → 扫描、打包 32 段、上报
VMM punch/madvise         → 宿主页框释放、RSS 回落
页被再次分配              → PageReported 清除 → 下轮可再报（可循环）
```

CubeSandbox E2E 就是这么测的：`stress --vm 1536M --vm-keep` → `drop_caches` + `compact_memory` ×3 → 90s 内等 RSS 回落（`tests/e2e/memory_reclaim/`）。

---

## 6. 观测与判据

| 层 | 看什么 | 说明 |
|---|---|---|
| guest | `dmesg` 的 `Free page reporting enabled` | 注册成功 |
| guest | `/proc/buddyinfo`、`/proc/pagetypeinfo` | 是否有高阶空闲块（碎了就报不出去） |
| guest | `page_reporting_order` | 报告门槛 |
| VMM | `/proc/<pid>/status` 的 `VmRSS` | 唯一有效判据：峰值后回落 |
| VMM | `smaps` 的 `Rss`/`Pss` | 定位哪些区域被丢弃 |
| CubeSandbox E2E | baseline ≤ 512 MiB、峰值增量 ≥ 1024 MiB、回落 ≤ baseline+384 MiB | `hypervisor/tests/integration.rs` 第 1928-1930 行 |

**判定模板**：先记录 baseline RSS → 让 guest 触碰 N MiB → 记录峰值 → guest 释放并 compact → 记录回落量。回落量接近释放量才算 FPR 生效；若 guest 侧 `MemFree` 上升但 VMM RSS 不降，问题在 VMM 侧（feature 未协商/队列未处理/区域校验失败）。

---

## 7. 与其他机制的关系

| 机制 | 关系 |
|---|---|
| virtio-balloon inflate/deflate | 独立队列；inflate 是 guest 主动交页给 host，FPR 是 guest 主动报告空闲页，二者互不影响 |
| THP | FPR 只报 order ≥ pageblock 的块；ARM64 64K 页时 balloon 设 order=5 以避免拆 2MB THP |
| page cache | **FPR 不回收 page cache**；它只报「已 free 的页」。要压 page cache 需要另一条路径（见下） |
| zram/zswap | 被压缩的页仍在 guest 占用内存，不会被报告；只有进程真正 free 后才可能 |
| MEM_FREE_RECLAIM（memop） | 本树 ARM64 有 host 主动请求的扩展（`CONFIG_MM_FREE_RECLAIM=y`，arm64-dc:1307）：`RECLAIM_PAGE_CACHE` / `COMPACT_MEMORY` / `QUERY_FRAGMENTATION`（`drivers/virtio/virtio_balloon.c:438-506`）。CubeSandbox 目前未实现这条 host 侧，FPR 仍是主路径 |
| 快照/恢复 | balloon 是设备；设备拓扑写进快照。旧模板恢复后可能根本没有 balloon → FPR 不生效 |

---

## 8. 限制与坑

1. **只报大块**：order < page_reporting_order 的空闲页永远报不出去；碎片化负载必须先 compact。
2. **2 秒节流**：不适合毫秒级回收；E2E 用 90s 窗口等。
3. **报告 ≠ 免费**：VMM 收到后做 `MADV_DONTNEED`，该页下次访问会重新分配并触发缺页（guest 侧无感，因为 guest 已 free）。
4. **MAP_PRIVATE 不能 punch**：VMM 对共享文件走后端打洞，对私有映射只 madvise，防止破坏快照/共享 base。
5. **已报告页不重复**：页未被重新分配前不会再报，所以「报告后 RSS 不动」可能是正常的。
6. **模板/快照拓扑**：恢复后没有 balloon 设备时，guest 压根不会注册 FPR。
7. **seccomp**：VMM 需要允许 `madvise`/`fallocate`，否则 release 失败只 warn、RSS 不降。
8. **不是回收策略**：FPR 是被动「告知空闲」，不做 LRU 判断；真正的内存压力回收还是 guest 内核的 kswapd/MGLRU/DAMON 那一套。

---

## 9. 一页纸速查

| 层 | 组件/符号 | 位置 | 作用 |
|---|---|---|---|
| guest 内核 | `page_reporting_notify_free` | `mm/page_alloc.c:899` | free 路径触发 |
| guest 内核 | `page_reporting_process_zone` | `mm/page_reporting.c:261` | 扫空闲链表、分组 |
| guest 内核 | `page_reporting_drain` | `mm/page_reporting.c:106` | 放回 buddy + 标记 |
| guest 内核 | `PAGE_REPORTING_DELAY`(2s)/`CAPACITY`(32) | `mm/page_reporting.c:50` / `include/linux/page_reporting.h:8` | 节流/批量 |
| guest 内核 | `page_reporting_order` | `mm/page_reporting.c:15` | 最小报告 order |
| guest 驱动 | `virtballoon_free_page_report` | `drivers/virtio/virtio_balloon.c:222` | 打包上报、等 ack |
| virtio | `VIRTIO_BALLOON_F_REPORTING`(5) | `include/uapi/linux/virtio_balloon.h:39` | 特性位 |
| VMM | `process_reporting_queue` | `CubeSandbox/hypervisor/virtio-devices/src/balloon.rs`（第 286 行） | 消费报告 |
| VMM | `release_memory_range` | 同上（第 135 行） | punch hole / madvise |
| VMM CLI | `--balloon ...free_page_reporting=on` | Cloud Hypervisor 语法 | 打开特性 |
| 默认配置 | `free_page_reporting: true` | `CubeSandbox/CubeShim/shim/src/hypervisor/config.rs`（第 176 行） | sandbox 默认开 |
| 测试 | E2E 阈值 | `CubeSandbox/hypervisor/tests/integration.rs`（第 1928-1930 行） | 判据 |

---

## 10. 参考

- `Documentation/mm/free_page_reporting.rst`（guest 内核 API）
- 本树：`mm/page_reporting.c`、`include/linux/page_reporting.h`、`mm/page_alloc.c:899`、`drivers/virtio/virtio_balloon.c:222`
- CubeSandbox：`hypervisor/virtio-devices/src/balloon.rs`、`CubeShim/shim/src/hypervisor/config.rs`、`hypervisor/tests/integration.rs`、`tests/e2e/memory_reclaim/`、`hypervisor/release-notes.md`（Free Page Reporting 一节）

## 11. 更新日志

- 2026-11-15：初版；按「guest 内核 → virtio → VMM → 宿主」四段给出全景图、使能条件、参数、判据与坑位。
