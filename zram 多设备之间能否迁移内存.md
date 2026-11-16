---
title: zram 多设备之间能否迁移内存
aliases:
  - zram device migration
  - zram 之间搬页
tags:
  - zram
  - 内核
  - swap
source: /Users/cheney/work/code/ub/kernel
created: 2026-11-16
updated: 2026-11-16
status: 已核对源码
---

# zram 多设备之间能否迁移内存

## 结论

**内核没有"把 zram A 里的页搬到 zram B"的原生机制**，也没有任何 sysfs 接口做这件事。可用的只有三条近似路径：

1. 把 zram B 设成 zram A 的 `backing_dev`，再用 `writeback` 把页"写出去"——因为写回存的是**解压后的原始页**，zram B 会重新压缩，效果上等于页被搬进了 B 的压缩池。属未公开支持用法。
2. 整设备迁移：`swapoff /dev/zramA` + `swapon /dev/zramB`——所有页先读回内存再写回，代价是 RAM 抖动。
3. 同一设备内 `recompress` 换更强算法——不改设备，只换压缩级别。

真正的"跨设备搬压缩对象"做不到，原因在三条硬约束上（见 §4）。

## 1. zram 的存储单元是"设备 + 块号"

每个 zram 设备各自持有一张 `table`（每页一项）和一个独立的 zsmalloc 池：

```c
zram->mem_pool = zs_create_pool(zram->disk->disk_name);   /* drivers/block/zram/zram_drv.c:1261 */
```

`zram_table_entry` 里存的是 `handle`（该池内对象的句柄）+ `flags`（低位存压缩后大小，高位存标志，见 `drivers/block/zram/zram_drv.h:66-75`）。**handle 只在所属 pool 内有意义**，换个设备就是无效值。所以"搬对象"必须先解压再压缩，不存在零拷贝的设备间搬运。

zram 对外暴露的全部控制面（`drivers/block/zram/zram_drv.c:2179-2205`）：

```
disksize initstate reset compact mem_limit mem_used_max idle
max_comp_streams comp_algorithm
backing_dev writeback writeback_limit writeback_limit_enable
io_stat mm_stat bd_stat debug_stat
recomp_algorithm recompress
```

里面**没有任何跨设备的迁移/转储接口**。`compact` 也只是调 `zs_compact()` 在**本池内**整理 zspage（`mm/zsmalloc.c:1556` 的 `migrate_zspage()` 是池内迁移，不是池间）。

## 2. 路径一：把另一个 zram 当 backing_dev（唯一能做"设备间搬页"的办法）

### 2.1 为什么技术上可行

`backing_dev_store()` 只要求目标是个块设备：

- `/* Support only block device in this moment */` + `S_ISBLK()`（`drivers/block/zram/zram_drv.c:501`）
- 拒绝零容量设备，注释明确说这同时防止自引用（`:516`）

zram 本身就是块设备，所以 `echo /dev/zramB > /sys/block/zramA/backing_dev` 能成功（前提是 A 还没 `disksize`，未初始化；B 已有非零容量）。代码里没有"backing 不能是 zram"的检查。

### 2.2 搬页的语义

写回时先 `zram_read_page()` 把页**解压**到临时页，然后用一个 bio 把这张**原始页**写到后备设备（`:715-720`），成功后释放本设备池里的对象、置 `ZRAM_WB` 并把块号存进 element（`:762-763`）。读回时看到 `ZRAM_WB` 就去后备设备读（`:1396`）。

所以整条链路是：**A 解压 → 写原始页给 B → B 重新压缩存进自己的池**。A 的池空间释放，B 的池空间增加，这就是"迁移"的实质。代价是每次搬页都付一次解压 + 一次压缩的 CPU，比直接把页写进 B 还贵。

触发是手动的（`echo idle|huge|incompressible|<index> > writeback`，`:601-620`），内核没有自动迁移策略。

### 2.3 坑（按严重程度）

1. **B 不能再同时当 swap 用。** A 在后备设备上分配块用的是自己的位图（`alloc_block_bdev()`，`:557`），与 B 的 swap slot 分配完全不协调，会互相覆盖块内容 → 数据损坏。B 只能作为纯后备存储。
2. **B 的 `mem_limit` 会传导成 A 的写回失败。** B 存不下时页会退化成不可压缩存储或返回 `-ENOSPC`，A 侧 `submit_bio_wait()` 拿到错误，`writeback` 返回该错误（`writeback_store` 的错误处理分支）。
3. **B 的容量决定 A 能搬多少。** A 只按 B 的 `i_size` 建位图、分配块，B 满即 `-ENOSPC`。
4. **只有部分页能搬。** 写回会跳过 `ZRAM_SAME`（整页同值的页）和已在 `ZRAM_WB`/`UNDER_WB` 的页；`idle`/`huge`/`incompressible` 模式还要求对应标志位。
5. **拆设备要讲顺序。** A 的 WB 页依赖 B，必须先 `reset` A（会 `free_block_bdev()` 归还块，`:1300`）或把页读回，再动 B。
6. **统计分裂。** A 的 `bd_stat` 记的是"写到后备设备"的次数/块数，B 的 `mm_stat` 记的是"自己池里存了多少"，两侧要合起来看才等于原占用。

## 3. 路径二和三：整设备迁移与设备内换算法

### 3.1 swapoff + swapon

如果 A、B 都是 swap 设备，想整体搬过去只能 `swapoff A` → `swapon B`。`swapoff` 会把 A 里所有页**读回内存**（需要 RAM 余量，接近 A 的原始数据量），再 `swapon B` 时这些页若被换出才重新压缩进 B。

要点：swap 层没有任何"迁移已有 swap entry"的接口，只有这条"读回来再换出"的路。另外 `swapon -p` 的优先级只影响**新分配的页**落到哪个设备，已有页不会因为优先级变化而搬家——所以"高优先级放快算法、低优先级放强压缩"的分层只能靠新页自然分层，不会自动迁移旧页。

### 3.2 设备内 recompress（不改设备，只改压缩级别）

`CONFIG_ZRAM_MULTI_COMP` 下有多算法能力：`recomp_algorithm` 设置主/次算法及优先级，`recompress` 按类型或算法把已存页用更高优先级的算法重压一遍（`zram_recompress()`，`:1596-1620`；入口 `recompress_store()`，`:1738`）。

它做的是"同一设备内换压缩算法"：读出来、用新算法压、更小就替换 handle 并更新优先级标记。**这不是跨设备迁移**，但如果目标只是"把旧页从快算法迁到强压缩算法"，这是唯一的内核支持路径，而且省掉了设备间跳转。

## 4. 为什么原生跨设备迁移做不到

1. **池是设备私有的**（`:1261`）。handle/zspage 的布局、size class 都是池内的，跨池必须先走"解压-再压缩"。
2. **页的落点被 swap entry 定死。** `swp_entry_t` = `(type, offset)`，type 是 swap 设备索引、offset 是设备内偏移（`include/linux/swapops.h:27-28`、`:86-90`）。一个页在 PTE/swap_map 里的位置是写死的，内核没有"改写 type 并保留内容"的 API；改 type 等于给这个页重新分配换出槽位，也就是再做一次换出。
3. **一致性成本高。** 迁移一个页要同时保住 PTE 引用计数、swap_map 计数、page cache（swapcache）里的副本，还要处理并发 fault。这类工作目前没有任何内核路径在做。

## 5. 实务建议

- 想省内存做"两级压缩池"（快算法池 + 强压缩池）：**不要用 backing_dev 互指**，用用户态的 idle 页扫描 + `writeback` 到真实块设备是受支持的做法；zram 之间互指只能算实验性用法，且必须保证目标设备不参与 swap。
- 想换算法：**在同一设备上 `recomp_algorithm` + `recompress`**，别换设备。
- 想整体换设备：`swapoff`/`swapon`，选内存压力低的时机做。
- 多 zram 的正确分工是**按优先级分层**（`swapon -p`），让新页按热度/优先级自然落到不同设备，而不是事后搬。

## 6. 未验证

- "zram 当另一个 zram 的 backing_dev"我只做了代码路径推导，**没有在实机跑过**，也没验证 `bdev_open_by_dev()` 在两个 zram 之间是否存在 holder 冲突；上生产前必须实测一遍写回/读回/删除顺序。
- 双压缩的实际 CPU 开销、以及 `writeback` 批量搬页对 swap 时延的影响都没有实测数据。
- 上游是否有计划支持跨设备迁移，未查。

## 相关

- [[zram 框架介绍]]
- [[UB 远端内存借用与 DAX 可行性]]
