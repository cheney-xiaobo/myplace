---
title: DAMON region 合并逻辑
aliases:
  - damon region merge
  - kdamond_merge_regions
tags:
  - damon
  - 内核
  - 内存采样
source: /Users/cheney/work/code/ub/kernel
created: 2026-11-15
updated: 2026-11-15
status: 已核对源码（6.6 + stable 回补）
---

# DAMON region 合并逻辑

一句话：**每个 aggregation 周期末尾，把"物理相邻 + 访问计数差不多 + 合并后不超过 sz_limit"的相邻 region 贪心合并成一个，计数按大小加权平均；如果 region 数还超过 `max_nr_regions`，就把"差不多"的阈值翻倍再合，直到数量达标或阈值顶到上限。**

## 1. 什么时候合并

`kdamond_fn()` 主循环里，每到一个 aggregation 周期（`passed_sample_intervals >= next_aggregation_sis`）就调用一次（`mm/damon/core.c:1561-1564`）：

```c
if (ctx->passed_sample_intervals >= next_aggregation_sis) {
        kdamond_merge_regions(ctx, max_nr_accesses / 10, sz_limit);
```

位置很关键：**在 `after_aggregation` 回调之前、在 `kdamond_apply_schemes()` 之前**。代码注释说明了动机——先合并再跑 scheme 可以降低开销（`mm/damon/core.c:1567-1569`）。

初始阈值来自 `max_nr_accesses`，即本周期里观测到的最大访问计数（`mm/damon/core.c:1558`），对 vaddr/paddr 来说由采样节拍决定（`include/linux/damon.h:678-686` 的 `damon_max_nr_accesses()`，等于 `aggr_interval / sample_interval`）。所以**初始阈值 = 理论上限的 1/10**。

## 2. 判定条件：三个，必须同时满足

`damon_merge_regions_of()`（`mm/damon/core.c:1228-1248`）：

```c
if (prev && prev->ar.end == r->ar.start &&               /* 1. 物理相邻 */
    abs(prev->nr_accesses - r->nr_accesses) <= thres &&  /* 2. 计数接近 */
    damon_sz_region(prev) + damon_sz_region(r) <= sz_limit) /* 3. 不超上限 */
        damon_merge_two_regions(t, prev, r);
else
    prev = r;
```

三点细节：

- **相邻靠 `ar.end == ar.start` 判断**，不做真正的区间重叠检测（DAMON 的 region 列表本身是覆盖目标区间、互不重叠的）。
- **贪心、从左到右链式合并**：合并成功后 `prev` 指针不前进，所以第三个 region 还能接着并进已经变大的那个 region——不是"两两配对"。
- **阈值比的是差值绝对值**，不是比值。`thres` 直接是访问计数的容差（0..max_nr_accesses），乘上区域大小后才是"面积差"。

## 3. 合并后的数值怎么算

`damon_merge_two_regions()`（`mm/damon/core.c:1209-1219`）：

```c
l->nr_accesses = (l->nr_accesses * sz_l + r->nr_accesses * sz_r) / (sz_l + sz_r);
l->age         = (l->age         * sz_l + r->age         * sz_r) / (sz_l + sz_r);
l->ar.end = r->ar.end;
damon_destroy_region(r, t);
```

即 **按大小加权平均**，区间右端点扩展，右侧 region 销毁。

一个容易漏掉的点：**`last_nr_accesses` 没有被合并**（对比 `mm/damon/core.c:858` 处每个周期末的 `last_nr_accesses = nr_accesses` 赋值）。合并后的 region 保留的是左区间上一周期那份值，不是加权结果。它只用于下一次 age 计算（差值超过阈值就让 age 归零），所以影响限于 age 的判定精度，不影响 `nr_accesses` 本身。

## 4. 阈值怎么演化：不达标就翻倍

`kdamond_merge_regions()`（`mm/damon/core.c:1267-1294`）：

```c
max_thres = c->attrs.aggr_interval / (c->attrs.sample_interval ?: 1);
while (true) {
        nr_regions = 0;
        damon_for_each_target(t, c)
                damon_merge_regions_of(t, threshold, sz_limit, count_age);
        ...
        if (nr_regions <= c->attrs.max_nr_regions || max_thres <= threshold)
                break;
        if (threshold < max_thres / 2)
                threshold = max(1, threshold * 2);
        else
                threshold = max_thres;
}
```

要点：

- 循环条件是**两个出口**：region 数已降到 `max_nr_regions` 以下，或者阈值已经顶到理论上限 `max_thres`（= aggr/sample）。后者是必须的兜底——用户可能设了大量互不相邻的 region，再怎么放宽阈值也合不到一起。
- 每轮把阈值翻倍（1 → 2 → 4 ...），上限硬夹到 `max_thres`。
- `count_age` 只在**第一轮**为 true：多轮"激进合并"不会反复给 age 加一。这条来自上游修复 `8ec24d1e1e17 mm/damon/core: skip aging from repeated aggressive merging`，否则反复合并会让 age 被重复累加，污染 monitoring 结果。
- 阈值翻倍时的溢出/死循环也修过：`7cf91d5e9c3d mm/damon/core: avoid infinite kdamond_merge_regions() internal loop`。老代码先翻倍再比较，`max_thres > UINT_MAX/2` 时翻倍溢出会绕过退出条件而无限循环；现在改成先判断再赋值，并且显式夹到 `max_thres`。

## 5. 合并后的大小上限从哪来

`sz_limit = damon_region_sz_limit(ctx)`（`mm/damon/core.c:678-695`）：

```c
damon_for_each_target(t, ctx)
        damon_for_each_region(r, t)
                sz += damon_sz_region(r);
if (ctx->attrs.min_nr_regions)
        sz /= ctx->attrs.min_nr_regions;
```

即 **被监控总大小 / `min_nr_regions`**，下限是 `DAMON_MIN_REGION`（= `PAGE_SIZE`，`include/linux/damon.h:19`；仅在 KUNIT 测试里被改成 1，见 `mm/damon/core.c:20-23`）。

所以 `min_nr_regions` 的真实语义是"**分辨率下限**"：它同时决定单 region 能长多大，而 `max_nr_regions` 决定合并多激进。在 `kdamond_fn` 里 `sz_limit` 只在启动和 ops update 时重算（`mm/damon/core.c:1531`、`:1594`），所以目标是动态变化时，这个上限会有滞后。

## 6. 与 split 对偶：滞回区间

合并完并不是结束。同一周期内先 `kdamond_reset_aggregated()`（`nr_accesses` 归零、`last_nr_accesses` 存旧值，`mm/damon/core.c:853-861`），接着 `kdamond_split_regions()`（`mm/damon/core.c:1357-1380`）：

- 只有 region 数 **≤ max_nr_regions / 2** 时才拆（多了不拆，少了才拆，形成滞回）。
- 每个 region 随机拆成 2 份（若上次数量相同且 < max/3 则 3 份），拆分点取原大小的 10%~90%，按 `DAMON_MIN_REGION` 对齐（`mm/damon/core.c:1320-1345`、`:1301-1315`）。
- 拆分是**随机**的、不是按访问热度的——目的只是恢复分辨率，给下个周期重新观测的机会。

合并与拆分的注释互相点名："合错了后面 split 会改回来"（`mm/damon/core.c:1256-1259`）、"拆多了后面 merge 会改回来"（`mm/damon/core.c:1350-1354`）。稳定状态下 region 数在两个阈值之间来回，而不是收敛到某个定值。

另外 scheme 应用阶段还会再拆 region：quota 用不完一个 region 时按剩余配额切（`mm/damon/core.c:1030-1039`），以及 charged-region 跳过时切（`mm/damon/core.c:943`）。所以"合并后的 region 形态"在 apply 之后可能又被细分。

## 7. 哪些参数能影响合并

| 参数 | 位置 | 影响 |
| --- | --- | --- |
| `attrs/nr_regions/max` | sysfs（默认 1000，`mm/damon/sysfs.c:581-589`） | 合并目标数量；达不到就翻倍阈值 |
| `attrs/nr_regions/min` | 同上（默认 10，须 ≥3 且 ≤ max，`mm/damon/core.c:621-623`） | 决定 `sz_limit` = 总大小/min |
| `attrs/sample_us`、`aggr_us` | sysfs | 决定 `max_nr_accesses` 与 `max_thres`，进而决定初始阈值 = 上限/10 |
| 目标区间划分 | `damon_set_regions()` | 互不相邻的区间永远合不到一起（这棵树的 `654bec3f660d` 还禁止了输入区间重叠） |

sysfs 的 `min`/`max` 是标准 ul_range 两个属性文件（`mm/damon/sysfs-common.c:95-99`）。

## 8. 实际影响与坑

1. **合并会稀释热点**：region 计数是面积加权平均，一个热子区间并进冷 region 后计数被拉低；但差值被 `thres` 卡住，所以稀释幅度有界（≤ thres）。多次翻倍阈值那一轮，稀释会更明显。
2. **合并发生在 scheme 之前**：DAMOS 看到的是合并后的 region，pattern 的 `min_sz_region` / `nr_accesses` 判定都在合并后的粒度上，太激进的合并会让过滤条件误判。
3. **`max_nr_regions` 调小是唯一会触发多轮激进合并的路径**（运行时 via `damon_set_attrs`），也最容易触发上面两个已被修掉的老 bug 版本。
4. **age 与 region 粒度耦合**：合并时 age 加权平均，拆分时新 region 继承 age（`mm/damon/core.c:1312-1314`），所以 age 是"这块地址的既往热度"的近似，不严格对应某个具体页。
5. **阈值语义是"本周期计数差"而非"热度差"**：`nr_accesses` 在每个 aggregation 周期被清零重算，所以阈值比较的是同一周期内的计数，周期长度（aggr_us）变化会直接改变阈值的物理含义。

## 9. 说明

- 本文代码位置基于这棵 6.6 树（含 stable-v6.6.157/158 回补的两个 DAMON 修复），与纯 6.6 主线在 `count_age`、阈值夹逼两点上有差异，已按本树代码描述。
- 未做实验：不同 `max_nr_regions`/`aggr_us` 下合并轮数与监控精度的影响没有实测数据，上面谈的是代码可推导的行为。

## 相关

- [[UBTurbo SMAP 冷热判断与交换速率]]
- [[UBTurbo SMAP 架构与实现原理]]
