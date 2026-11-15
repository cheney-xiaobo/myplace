---
title: zram 机制剖析：开启、管控与写回
aliases:
  - zram 管控逻辑
  - zram writeback 与 recompress
  - zram 开启与判据
tags:
  - linux-kernel
  - memory
  - zram
  - swap
  - zsmalloc
source: /Users/cheney/work/code/ub/kernel
created: 2026-11-15
updated: 2026-11-15
status: verified-by-reading
---

# zram 机制剖析：开启、管控与写回

按《内存管理机制逐个剖析》的四段式写：**开启方式 → 管控/决策逻辑 → 生效判据 → 机制间互斥与坑**。代码引用基于 `/Users/cheney/work/code/ub/kernel`（6.6，HEAD `8003dfcd`）；本机未挂 zram 实测，性能数字均为读码推断，标「未验证」。

框架性介绍（zsmalloc 原理、位域布局）见 [[zram 框架介绍]]，本文只讲「怎么开、开了之后按什么规则决策、怎么判断真生效、会踩什么」。

## 0. 结论先行

1. **本树 zram 是 `=m`，默认不生成任何设备**。`num_devices` 默认 1（`drivers/block/zram/zram_drv.c:45`），`modprobe zram` 只建 1 个；要用更多走 `/sys/class/zram-control/hot_add`。
2. **四个配置决定能力边界**：`ZRAM=m`、默认算法 `lzo-rle`、`ZRAM_WRITEBACK=y`、`ZRAM_MULTI_COMP=y`，但 **`ZRAM_MEMORY_TRACKING` 未设**（`arch/x86/configs/openeuler_defconfig:2412-2420`）。所以 **idle 只能 `idle=all`，不能按页龄；debugfs 的 per-slot 时间戳也不存在**。
3. **所有「改变设备形态」的写入必须发生在 `disksize` 之前**：`comp_algorithm`、`recomp_algorithm`、`backing_dev` 都查 `init_done()`，初始化后写一律 `-EBUSY`（`drivers/block/zram/zram_drv.c:1025-1030`、`drivers/block/zram/zram_drv.c:479-483`）。**顺序错了只能 `reset` 重来**。
4. **写路径的三条出口**：同字节页走 `ZRAM_SAME`（**不占 zsmalloc 内存**，只记 element）；压缩后 `>= huge_class_size` 视为不可压缩，**原样存整页**并打 `ZRAM_HUGE`；其余进 zsmalloc。`mem_limit` 超限时分配直接 `-ENOMEM`，上层 swap 写失败。
5. **writeback 是「逐页同步 bio」**：每个页 `REQ_SYNC` + `submit_bio_wait()`，无批量合并（`drivers/block/zram/zram_drv.c:718-731`），所以大批量回写慢、且不可中断。它靠 `ZRAM_UNDER_WB` + `ZRAM_IDLE` 与 `zram_free_page` 关竞态（`drivers/block/zram/zram_drv.c:700-712`、`drivers/block/zram/zram_drv.c:747-757`）。
6. **zram 压缩过的页仍在 guest 物理内存里**（zsmalloc pool 记账在 `mem_used`），所以 **FPR 报不出去**——这是两者最关键的边界（`mm/page_reporting.c` 只认 buddy 空闲页）。

## 1. 开启方式

### 1.1 配置核对表（本树）

| 项 | 本树值 | 位置 | 决定什么 |
| --- | --- | --- | --- |
| `CONFIG_ZRAM` | `=m` | `arch/x86/configs/openeuler_defconfig:2412` | 需手动 modprobe，不自启 |
| 默认算法 | `lzo-rle` | 同上 `drivers/block/zram/zram_drv.c:2413`、`drivers/block/zram/zram_drv.c:2417` | `comp_algorithm` 初始值 |
| `CONFIG_ZRAM_WRITEBACK` | `=y` | 同上 `drivers/block/zram/zram_drv.c:2418` | `backing_dev`/`writeback` 属性存在 |
| `CONFIG_ZRAM_MEMORY_TRACKING` | **未设** | 同上 `drivers/block/zram/zram_drv.c:2419` | 无页龄，idle 只能 `all` |
| `CONFIG_ZRAM_MULTI_COMP` | `=y` | 同上 `drivers/block/zram/zram_drv.c:2420` | 4 档算法优先级、`recompress` |
| `CONFIG_ZPOOL` / `CONFIG_ZSMALLOC` | `=y` / `=y` | 同上 `drivers/block/zram/zram_drv.c:1108`、`drivers/block/zram/zram_drv.c:1126` | 后端分配器 |
| `CONFIG_ZSMALLOC_STAT` | `=y` | 同上 `drivers/block/zram/zram_drv.c:1127` | `/sys/kernel/debug/zsmalloc/` |

`ZRAM_MAX_COMPS`：multi-comp 下是 4（primary=0、secondary=1..3），否则 1（`drivers/block/zram/zram_drv.h:96-104`）。优先级用两个 flag bit 存在 `table[index].flags` 里（`drivers/block/zram/zram_drv.c:158-175`），**只有 2 bit，所以最多 4 档**。

### 1.2 加载与建设备

```bash
modprobe zram num_devices=4        # 默认 1；参数定义在 drivers/block/zram/zram_drv.c:2484-2486
ls /sys/block | grep zram          # zram0..zram3
cat /sys/class/zram-control/hot_add # 动态加一个，返回新 id
```

`zram_init()` 只做三件事：注册 zcomp CPU 热插状态、`register_blkdev(0,"zram")` 拿主设备号、按 `num_devices` 逐个 `zram_add()`（`drivers/block/zram/zram_drv.c:2433-2470`）。`zram_add()` 里把队列设成 nonrot、logical block 4K、physical block = PAGE_SIZE、`QUEUE_FLAG_STABLE_WRITES`，容量先给 0（`drivers/block/zram/zram_drv.c:2214-2284`）。

### 1.3 顺序敏感的初始化（关键）

`disksize` 是「初始化完成」的唯一标志（`init_done()` 就是 `zram->disksize != 0`，`drivers/block/zram/zram_drv.c:88-91`）。**写 `disksize` 会**：对齐到页、`vzalloc` 槽表、`zs_create_pool()`、算 `huge_class_size`、按已有 `comp_algs[]` 建 zcomp、`set_capacity`（`drivers/block/zram/zram_drv.c:2045-2098`、`drivers/block/zram/zram_drv.c:1252-1275`）。

因此正确顺序是：

```bash
dev=/sys/block/zram0
echo lzo-rle    > $dev/comp_algorithm          # 必须在 disksize 前，否则 -EBUSY
echo "zstd"     > $dev/recomp_algorithm        # 可选：只设 secondary 档，multi-comp 才有
echo /dev/nvme0n1p9 > $dev/backing_dev         # 可选：回写后端，必须先于 disksize
echo 2G         > $dev/disksize                # 此时才真正建 pool、设容量
```

依据：`__comp_algorithm_store()` 的 `init_done` → `-EBUSY`（`drivers/block/zram/zram_drv.c:1024-1030`）；`backing_dev_store()` 同理（`drivers/block/zram/zram_drv.c:478-483`）；`recomp_algorithm_store()` 走同一函数族（`drivers/block/zram/zram_drv.c:1078-1120`）。

### 1.4 当 swap 用

```bash
mkswap /dev/zram0 && swapon -p 100 /dev/zram0   # 优先级高于磁盘 swap，先在 zram 里换出
```

swap 路径的联动点：`zram_devops.swap_slot_free_notify`（`drivers/block/zram/zram_drv.c:2150-2156` 注册）→ `zram_slot_free_notify()`（`drivers/block/zram/zram_drv.c:1986-1999`）在 swap 槽被释放时先 `notify_free++`，再 `zram_slot_trylock()` 抢槽；抢不到就 `miss_free++` 放弃，抢到才 `zram_free_page()`。**`notify_free` 是通知次数，不是释放页数**，两者差值要看 `miss_free`。

## 2. 管控 / 决策逻辑

### 2.1 槽状态机

每个 4K 槽一个 `struct zram_table_entry`（`drivers/block/zram/zram_drv.h:106-135`），`flags` 里塞了锁位、对象大小、以及这些状态位（`drivers/block/zram/zram_drv.h:48-60`）：

| flag | 含义 | 谁置位 | 谁清除 |
| --- | --- | --- | --- |
| `ZRAM_SAME` | 整页同字节，element 存那个字节，**不占 zsmalloc** | 写路径 `page_same_filled()`（`drivers/block/zram/zram_drv.c:1445-1451`） | `zram_free_page()`（`drivers/block/zram/zram_drv.c:1305-1313`） |
| `ZRAM_HUGE` | 压缩后 ≥ `huge_class_size`，按**整页原样**存 | `drivers/block/zram/zram_drv.c:1532-1536` | `drivers/block/zram/zram_drv.c:1283-1287` |
| `ZRAM_INCOMPRESSIBLE` | 多算法都压不动（recompress 场景） | `drivers/block/zram/zram_drv.c:1693` | `drivers/block/zram/zram_drv.c:1293-1294` |
| `ZRAM_WB` | 已回写到 `backing_dev`，element 是块号 | `drivers/block/zram/zram_drv.c:762-764` | `drivers/block/zram/zram_drv.c:1301-1303` |
| `ZRAM_UNDER_WB` | 正在回写中（关竞态用） | `drivers/block/zram/zram_drv.c:703` | 调用方负责，`:710/:728/:755/:761` |
| `ZRAM_IDLE` | 自上次 idle 标记后没被访问 | `mark_idle()`（`drivers/block/zram/zram_drv.c:282-305`） | 读路径 `zram_accessed()`（`drivers/block/zram/zram_drv.c:862-866`） |

槽互斥用 `flags` 的第 0 位做 `bit_spin_lock`（`drivers/block/zram/zram_drv.c:63-84`）；读路径全程持槽锁调 `zram_read_from_zspool()`，只有 WB 页先解锁再走块设备（`drivers/block/zram/zram_drv.c:1379-1405`）。

### 2.2 写路径（`zram_write_page()`，`drivers/block/zram/zram_drv.c:1434-1556`）

1. **同字节页短路**：`page_same_filled(mem,&element)` 命中 → `ZRAM_SAME`、`same_pages++`、直接 `goto out`。**不压缩、不进 zsmalloc**，只存 element（`drivers/block/zram/zram_drv.c:1443-1452`）。
2. **压缩**：取 per-CPU 流 `zcomp_stream_get(comps[ZRAM_PRIMARY_COMP])`（`drivers/block/zram/zram_drv.c:1456`），压缩到 2 页缓冲（`drivers/block/zram/zcomp.c:117-140` 的 `*dst_len = PAGE_SIZE*2`）。
3. **不可压缩判定**：`comp_len >= huge_class_size` → 强制 `comp_len = PAGE_SIZE`（`drivers/block/zram/zram_drv.c:1466-1470`），后面按整页原样 `memcpy` 进 zsmalloc，并打 `ZRAM_HUGE`（`drivers/block/zram/zram_drv.c:1532-1536`）。
4. **zsmalloc 分配双路径**：先 `zs_malloc(GFP_KSWAPD_RECLAIM|NOWARN|HIGHMEM|MOVABLE)` 快路径（预占禁止睡眠，用 per-CPU 流）；失败 → `writestall++`、放掉流、换 `GFP_NOIO` 慢路径重来，压缩结果非整页时**重压一次**（`drivers/block/zram/zram_drv.c:1476-1509`）。
5. **mem_limit 闸门**：`alloced_pages = zs_get_total_pages()`，超过 `limit_pages` 就 `zs_free()` 后返回 `-ENOMEM`（`drivers/block/zram/zram_drv.c:1510-1517`）。上层 `zram_bvec_write` 失败 → `bio->bi_status = BLK_STS_IOERR`、`failed_writes++`（`drivers/block/zram/zram_drv.c:1945-1950`）。**swap 写失败会回落到其他 swap 或触发 OOM，不会静默成功**。
6. **落槽**：持槽锁 → 先 `zram_free_page()` 释放旧内容 → 写 flags/handle/obj_size → `pages_stored++`（`drivers/block/zram/zram_drv.c:1518-1556`）。

部分写（partial IO）会先读全页再改（`zram_bvec_write_partial()`，`drivers/block/zram/zram_drv.c:1562-1578`）。

### 2.3 释放路径（`zram_free_page()`，`drivers/block/zram/zram_drv.c:1278-1328`）

- 清 `IDLE`/`HUGE`/`INCOMPRESSIBLE`，优先级归 0；
- `ZRAM_WB` → `free_block_bdev()`（清 bitmap、`bd_count--`）→ `goto out`；
- `ZRAM_SAME` → `same_pages--` → `goto out`；
- 否则 `zs_free()` + `compr_data_size -= obj_size`；
- 统一出口：`pages_stored--`、handle/size 清 0、`WARN_ON` 检查遗留 flag。

`reset_store()` 会先 `sync_blockdev()` 再 `zram_reset_device()`，后者遍历槽表调 `zram_meta_free()` 逐个释放（`drivers/block/zram/zram_drv.c:2100-2138`、`drivers/block/zram/zram_drv.c:1235-1250`）。

### 2.4 读路径

- WB 页：解锁后 `read_from_bdev()`（`bd_reads++`；有 parent bio 时 `bio_chain + submit_bio` 异步，无 parent 时同步读，`drivers/block/zram/zram_drv.c:825-830`、`drivers/block/zram/zram_drv.c:582-596`、`drivers/block/zram/zram_drv.c:808-820`）。
- 否则：`zram_read_from_zspool()`（`drivers/block/zram/zram_drv.c:1334-1375`）：`ZRAM_SAME` 直接把 element 填满整页；`size == PAGE_SIZE` 直接 `memcpy`（跳过解压）；其余按 `table[index]` 里记录的**优先级**取对应 zcomp 解压。
- 读成功后 `zram_accessed()` 清 `ZRAM_IDLE`（MEMORY_TRACKING 时还记 `ac_time`）（`drivers/block/zram/zram_drv.c:862-866`）。

### 2.5 多算法与重压缩

- `recomp_algorithm` 只写 secondary 档（`"priority=N algo=..."`，1..3），`comp_algorithm` 只写 primary（`drivers/block/zram/zram_drv.c:1046-1056`、`drivers/block/zram/zram_drv.c:1078-1120`）。
- `recompress_store()` 支持 `type=idle|huge|huge_idle` + `threshold=N` + 可选 `algo=`；`threshold >= huge_class_size` 直接 `-EINVAL`（`drivers/block/zram/zram_drv.c:1738-1790`）。
- 逐槽跳过 `WB/UNDER_WB/SAME/INCOMPRESSIBLE`，只对 idle/huge 命中者重压（`drivers/block/zram/zram_drv.c:1830-1845`）；`zram_recompress()` 内部按 `prio..prio_max` 依次尝试，压缩后仍 ≥ threshold 则保留旧对象（`drivers/block/zram/zram_drv.c:1597-1740`）。所以**重压缩收益取决于阈值设得是否低于当前对象大小**。

### 2.6 idle 标记（`idle_store()`，`drivers/block/zram/zram_drv.c:306-340`）

- `echo all > idle`：`cutoff_time=0`，**把所有已分配槽都标 idle**。
- `echo <秒> > idle`：**只有 `CONFIG_ZRAM_MEMORY_TRACKING` 才解析**（`drivers/block/zram/zram_drv.c:315-322`），本树未设 → 数字输入直接 `-EINVAL`。
- 未初始化时 `-EINVAL`（`drivers/block/zram/zram_drv.c:327-329`）。
- `mark_idle()` 跳过 `ZRAM_UNDER_WB` 的槽，避免与 writeback 竞态（`drivers/block/zram/zram_drv.c:289-300`）。

### 2.7 回写（`writeback_store()`，`drivers/block/zram/zram_drv.c:601-775`）

模式（`drivers/block/zram/zram_drv.c:611-630`）：`idle` / `huge` / `huge_idle` / `incompressible` / `page_index=<n>`。

前置闸门（按顺序返回）：

| 条件 | 返回 |
| --- | --- |
| 未初始化 | `-EINVAL`（`drivers/block/zram/zram_drv.c:632-636`） |
| 没设 `backing_dev` | `-ENODEV`（`drivers/block/zram/zram_drv.c:638-641`） |
| `page_index` 越界 | `-EINVAL`（`drivers/block/zram/zram_drv.c:650-656`） |
| `writeback_limit_enable=1` 且额度为 0 | `-EIO`（`drivers/block/zram/zram_drv.c:664-670`） |
| 后端 bitmap 无空闲块 | `-ENOSPC`（`drivers/block/zram/zram_drv.c:672-677`） |

逐页循环：跳过 `WB`/`SAME`/`UNDER_WB`（`drivers/block/zram/zram_drv.c:683-687`），再按模式过 `IDLE`/`HUGE`/`INCOMPRESSIBLE` 过滤（`drivers/block/zram/zram_drv.c:689-696`）；置 `UNDER_WB` + `IDLE` 后解锁 → 读页 → `REQ_SYNC` bio 单页 `submit_bio_wait()`（`drivers/block/zram/zram_drv.c:711-731`）；成功则 `bd_writes++`，重新持锁校验「槽还在且仍 IDLE」，否则放弃本次（`drivers/block/zram/zram_drv.c:747-757`）；确认后 `zram_free_page()` 释放压缩内存、置 `ZRAM_WB` + 块号、`pages_stored++`、按 4K 扣额度（`drivers/block/zram/zram_drv.c:759-770`）。

**要点**：一页一次同步 IO，无 bio 合并；`writeback_limit` 单位是 4K（`(1UL << (PAGE_SHIFT-12))`，`drivers/block/zram/zram_drv.c:767-768`）；额度耗尽后同一循环里直接 `-EIO` 退出，已回写的保留。

### 2.8 内存上限（`mem_limit`，`drivers/block/zram/zram_drv.c:239-255`）

`limit_pages = PAGE_ALIGN(limit) >> PAGE_SHIFT`，**不需要未初始化也能改**（没有 init 检查）。它只在写路径判断 `alloced_pages > limit_pages` 时生效，所以是「软上限」：已存数据不受影响，只会让**新写**失败。`mem_used_max` 是只写 0 的重置（`drivers/block/zram/zram_drv.c:257-273`），用于把 `max_used` 对齐到当前值。

## 3. 生效判据

### 3.1 统计字段（`mm_stat`，`drivers/block/zram/zram_drv.c:1154-1185`）

| 列 | 含义 | 判据用途 |
| --- | --- | --- |
| 1 orig | `pages_stored << PAGE_SHIFT`（**当前实际存放的原始字节**，非容量） | 和 disksize 比对，判断「压了多少」 |
| 2 compr | `compr_data_size`，压缩后字节和 | `orig/compr` ≈ 压缩比（含 SAME 页为 0） |
| 3 mem_used | `zs_get_total_pages() << PAGE_SHIFT`，zsmalloc 实占 | **真实内存成本**，比 compr 大（有碎片/元数据） |
| 4 limit | `limit_pages << PAGE_SHIFT` | 确认 mem_limit 生效 |
| 5 max_used | 历史峰值 | 判断是否曾顶到 limit |
| 6 same | 同字节页数 | `same` 高说明负载零页多，压缩比会虚高 |
| 7 pages_compacted | `zs_compact()` 回收的页数 | 碎片整理是否有效 |
| 8 huge | 当前整页原样存放数 | 高说明数据不可压缩，zram 几乎没收益 |
| 9 huge_since | 建池以来累计 huge | 与 8 差值看是否在回落 |

### 3.2 其它统计

- `io_stat`（`drivers/block/zram/zram_drv.c:1137-1152`）：`failed_reads`、`failed_writes`、`notify_free`。**`failed_writes` 增长就是 mem_limit/内存不足真打到了写路径**。
- `bd_stat`（`drivers/block/zram/zram_drv.c:1192-1203`）：`bd_count`、`bd_reads`、`bd_writes`，**单位是 4K bytes**（`FOUR_K()`，`drivers/block/zram/zram_drv.c:1190`），不是 KiB，别当页数看。
- `initstate`（`drivers/block/zram/zram_drv.c:218-229`）：0/1，判断 `disksize` 是否已写。
- 未初始化就 `cat mm_stat` 全 0，容易误判「没数据」。

### 3.3 判据组合（怎么确认活干成了）

| 想验证 | 看什么 |
| --- | --- |
| 真的在压缩 | `orig/compr > 1` 且 `huge` 不涨 |
| 内存确实省了 | `mem_used < orig`，且 `mem_used` 与 `compr` 同向 |
| 回写真落盘 | `backing_dev` 非 none；`bd_writes` 增；`mm_stat` 的 `orig` 下降；对应 `ZRAM_WB` 页不再计入 zsmalloc |
| idle 标记真生效 | 只有 `all` 可用；标记后 `debug_stat` 的 `i` 位（`drivers/block/zram/zram_drv.c:900-910`）出现；`writeback idle` 才有可回写对象 |
| 超限真的拦住了 | `mem_limit` 写小值后持续写盘 → `failed_writes` 涨、`max_used` 贴 limit |

## 4. 互斥、依赖与坑

1. **顺序坑（最常见）**：`comp_algorithm` / `recomp_algorithm` / `backing_dev` 必须在 `disksize` 前写，否则 `-EBUSY`；只能 `reset` 后重来（`reset_store` 还要求设备没被打开、没被 claim，`drivers/block/zram/zram_drv.c:2118-2119`）。
2. **本树没有页龄**：`ZRAM_MEMORY_TRACKING` 未设 → `echo 3600 > idle` 报 `-EINVAL`；想按「冷却多久」回写只能自己在外层周期 `idle=all` + 立即 `writeback idle`。
3. **`page_index=` 回写**：只回写指定槽，`nr_pages` 被压成 1（`drivers/block/zram/zram_drv.c:652-656`），适合精确挑选，但不做模式过滤（仍跳过 WB/SAME/UNDER_WB）。
4. **writeback 与 reset 竞态**：`writeback_store` 全程持 `init_lock` 读锁，`reset_store` 取写锁并 `claim=true`，同时 `zram_open()` 拒绝新打开（`drivers/block/zram/zram_drv.c:2140-2148`）。所以 **`swapon` 着的 zram 不能 reset**（`disk_openers` 非 0）。
5. **回写是同步逐页**：大设备 `writeback idle` 会长时间占住 sysfs 写者与块设备队列；`writeback_limit` 是唯一刹车，但耗尽时返回 `-EIO` 而不是「部分成功」语义，脚本要按错误码分支。
6. **mem_limit 是软上限且可后改**：先超限写了数据再调小 limit，不会搬走已有数据，只会让后续写失败；`max_used` 不会自动回落，要 `echo 0 > mem_used_max`。
7. **不可压缩页的双倍成本**：`comp_len >= huge_class_size` 时整页原样进 zsmalloc，**外加池元数据**，极端情况下比不用 zram 更费内存；`mm_stat.huge` 持续增长就是信号。
8. **SAME 页是压缩比的假象**：零页/同字节页 `compr` 记 0，只看 `orig/compr` 会高估压缩收益，要同时看 `mem_used`。
9. **与 zswap 的关系**：两者都是「swap 前后端压缩」，本树 zswap 默认关（`CONFIG_ZSWAP_DEFAULT_ON` 未设），若同时开 zram+swap+zswap，zswap 在前端拦一层，zram 后端页可能再被压缩一次，压缩比与延迟都会被放大——**未在本机实测，标记未验证**。
10. **与 FPR 的边界**：zram 页是 guest 内核里**仍被占用**的 zsmalloc 对象，不是 buddy 空闲页，所以 `page_reporting_notify_free()` 不会看到它们，FPR 报不出去；只有 `zram_free_page()` 真正 `zs_free()` 后那部分内存才可能回 buddy。想要「先压再还宿主内存」，只能「回写后端 + 释放槽」这条弯路。
11. **`zram_reclaim` 是另一条路**：`mm/zram_reclaim.h` / 出现在的 `CONFIG_` 只对 ARM64+HiSilicon 特定 CPU 生效，本树 x86 不编译，**不是**本文讲的 zram 写回机制，别混。
12. **hot_remove 要求无 openers**：`rmmod zram` 也会因 openers 失败；`swapon` 期间先 `swapoff` 再删。

## 5. 最小可执行序列（核验用）

```bash
# 1) 建 1 个 2G 的 zram swap，带 2 档算法
modprobe zram num_devices=1
d=/sys/block/zram0
[ -n "$(cat $d/disksize)" ] && echo 1 > $d/reset
echo lzo-rle          > $d/comp_algorithm
echo "priority=1 algo=zstd" > $d/recomp_algorithm
echo 2G               > $d/disksize
cat $d/mm_stat        # 全 0：还没数据

# 2) 当高优先 swap
mkswap /dev/zram0 && swapon -p 100 /dev/zram0
# 压一波
stress --vm 512M --vm-keep & sleep 20; swapoff /dev/zram0
cat $d/mm_stat        # 看 orig/compr/mem_used/huge
cat $d/io_stat        # failed_*、notify_free

# 3) 回写后端（必须先设 backing_dev 再 disksize，所以上面要 reset 重来）
echo 1 > $d/reset
echo /dev/nvme0n1p9   > $d/backing_dev
echo 2G               > $d/disksize
swapon -p 100 /dev/zram0
echo all              > $d/idle
echo 1                > $d/writeback_limit_enable
echo 128M             > $d/writeback_limit     # 4K 单位额度：128M bytes
echo idle             > $d/writeback
cat $d/bd_stat        # 4K 单位
cat $d/mm_stat        # orig 应下降
```

`swapoff` 后数据会全部读回内存或写回后端，**这不是「释放」，只是换了位置**——想省宿主内存得靠 FPR 那一侧。

---

相关：[[zram 框架介绍]]（原理与 zsmalloc）、[[FPR（Free Page Reporting）全景图]]（guest→VMM 的整页回收）、[[内存管理机制逐个剖析：开启与管控逻辑]]（四段式模板）。
