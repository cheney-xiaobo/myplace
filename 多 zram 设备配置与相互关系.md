---
title: 多 zram 设备配置与相互关系
aliases:
  - zram 多设备
  - 多个 zram 怎么配
tags:
  - zram
  - swap
  - 内核
source: /Users/cheney/work/code/ub/kernel
created: 2026-11-16
updated: 2026-11-16
status: 已核对源码 + 配置序列
---

# 多 zram 设备配置与相互关系

## 结论

- **设备之间没有任何共享状态**：每个 zram 设备有自己的 `table`（每页一项）、自己的 zsmalloc 池、自己的压缩流、自己的 `mem_limit` 和统计，生命周期也各自独立。
- **它们之间只有两条"语义关系"**：① 都当 swap 用时，由 swap 层的**优先级**决定新页写到哪个设备（旧页不会因此搬家）；② 主动配置 `backing_dev` 时，形成"A 的后备设备是 B"的依赖。
- **没有全局配额**：N 个设备的实际内存占用是各自池的**求和**，每个设备的 `mem_limit` 只约束自己。

## 1. 一个 zram 设备是什么

每个 `zramN` 都是独立的压缩块设备。`disksize` 写下去时才真正建池（`drivers/block/zram/zram_drv.c:2045-2055` 检查 `init_done`，然后 `zram_meta_alloc()`）：

```c
zram->table = vzalloc(array_size(num_pages, sizeof(*zram->table)));  /* :1256 */
zram->mem_pool = zs_create_pool(zram->disk->disk_name);             /* :1261 */
```

也就是说：**表 + 池都是 per-device 的**，这是"设备之间不共享"的根因。压缩流也是 per-device、且 per-CPU（`drivers/block/zram/zcomp.c:180` 的 `alloc_percpu`），每套流占 2 页 buffer（`drivers/block/zram/zcomp.c:49-58`）。

没写 `disksize` 的设备只是"存在但不可用"：`/sys/block/zramN/initstate` 是 0。

## 2. 怎么建多个设备

两条路，二选一或混用：

### 2.1 加载模块时预建

```bash
modprobe zram num_devices=4
```

`num_devices` 是模块参数，默认 1，只能在加载时指定（`drivers/block/zram/zram_drv.c:2484-2485`；初始化时循环建出这么多个，见 `:2460-2466`）。

### 2.2 运行期热加

```bash
cat /sys/class/zram-control/hot_add      # 读一次 = 建一个新设备，返回它的编号
```

`hot_add` 是个"读操作带副作用"的属性，读完返回新设备 id（`drivers/block/zram/zram_drv.c:2351-2369`）。

对称地删设备：

```bash
echo 2 > /sys/class/zram-control/hot_remove
```

要求该设备**没有被打开**（`disk_openers()`，`:2300-2306`），即先 `swapoff`/`umount` 再删。

## 3. 每个设备的初始化顺序（不能反）

```bash
echo lz4  > /sys/block/zram0/comp_algorithm    # 1. 算法：必须在 disksize 之前
echo 4G   > /sys/block/zram0/disksize          # 2. 容量：触发建表 + 建池
echo 512M > /sys/block/zram0/mem_limit         # 3. 可选：内存硬上限
```

- **算法必须在前**：`__comp_algorithm_store()` 里明确拒绝已初始化设备（`"Can't change algorithm for initialized device"`，`:1028`）。
- **`disksize` 只能设一次**：改容量必须 `reset` 重来（`:2052-2056`）。
- **`mem_limit` 可以运行期调整**，但它不是"软目标"：池页数一旦超过它，写入直接 `-ENOMEM`（`:1513-1519`），不是退化存储。

`max_comp_streams` 是历史遗留属性、返回值不可信（`:955-975` 注释说早就改成 per-CPU 流了）。

## 4. 投入使用：两条互斥路线

### 4.1 当 swap（可以多个设备分层）

```bash
mkswap /dev/zram0 && swapon -p 100 /dev/zram0
mkswap /dev/zram1 && swapon -p 50  /dev/zram1
cat /proc/swaps
```

优先级数字越大越优先。分配时 swap 层按优先级从可用设备里取槽位（`mm/swapfile.c:2530-2536` 注释明确 `swap_avail_head` 是按优先级排序、`folio_alloc_swap()` 从最高可用优先级取；取用循环见 `:1173`）。

**关键点：优先级只影响新页。** 已有页留在原设备，不会因为优先级变化而迁移——要搬只能 `swapoff`/`swapon` 或走上一轮讨论的 writeback 路线。

### 4.2 当普通块设备

```bash
mkfs.ext4 /dev/zram1 && mount /dev/zram1 /mnt/zram1
```

和 swap 用法无冲突，但同一设备不能同时当 swap 和普通块设备（内容会互相覆盖）。

## 5. 设备间关系一览

| 维度 | 是否共享 | 证据 |
| --- | --- | --- |
| 元数据 table | 不共享，每设备 `vzalloc` 一份 | `drivers/block/zram/zram_drv.c:1256` |
| zsmalloc 池 | 不共享，每设备 `zs_create_pool` | `drivers/block/zram/zram_drv.c:1261` |
| 压缩流 buffer | 不共享；且每设备 × 每 CPU 一套，每套 2 页 | `drivers/block/zram/zcomp.c:180`、`:49-58` |
| `mem_limit` | 独立，无全局配额 | `:1513-1519` |
| 统计 `mm_stat`/`io_stat` | 独立，总量要自己累加 | `:2179-2205` |
| 生命周期 | 独立（`disksize`/`reset`/`hot_remove` 都只作用于单设备） | `:2045`、`:2100`、`:2300` |
| 物理内存 | **共享**——所有设备都从同一块 RAM 里要页 | 池最终落到 page allocator |
| swap 槽位空间 | 独立（每个 swap 设备一个 swp type，各有自己的槽位空间） | `include/linux/swapops.h:27-28` |
| 后备设备 | 可选依赖（A 的 `backing_dev` 指向 B） | `drivers/block/zram/zram_drv.c:460-520` |

## 6. 三种实际"关系"形态

1. **互不相干**：一个当 swap、一个当裸块设备、一个闲置。彼此不影响。
2. **同一 swap 池的分层**：多个设备按优先级做"快算法 / 强压缩"两层。新页按优先级落，旧页不动，所以是"自然分层"而不是"迁移分层"。
3. **后备依赖**：A 的 `backing_dev` 指向 B，A 的页可以 writeback 进 B 的池（上一轮已分析：写回存的是解压后的原始页，B 会重新压缩；B 不能再当 swap 用）。

## 7. 坑与限制

1. **`comp_algorithm` 写晚了就改不了**，只能 `reset` 重来（reset 会销毁池里所有数据）。
2. **`reset`/`hot_remove` 都要求设备没被打开**：swap 还 on 着就是 `-EBUSY`（`:2100-2121`、`:2300-2306`）。正确顺序是 `swapoff` → `reset` → `hot_remove`。
3. **`mem_limit` 超限是写失败**，不是降级。swap 场景下表现为换出失败，页留在内存里，压力更大时会看到 OOM 而不是 zram 报错。
4. **设备越多，静态开销越大**：每设备 × 每 CPU 一套 2 页 buffer（`drivers/block/zram/zcomp.c:49-58`）。8 设备 × 64 CPU ≈ 4 MB 常驻，这是"多建设备"的隐性成本（按实际 CPU 数自己算，属推算）。
5. **没有全局上限**：想限制总占用，只能给每个设备分别设 `mem_limit`，自己保证总和 ≤ 预算。
6. **`zramctl --find` 会新建设备**而不是复用空闲设备，脚本里连调两次容易建出多余设备。

## 8. 完整示例：两层压缩 swap + 一个备用块设备

```bash
# 建设备（4 个）
modprobe zram num_devices=4

# 第 1 层：快算法、小容量、高优先级
echo lz4  > /sys/block/zram0/comp_algorithm
echo 2G   > /sys/block/zram0/disksize
echo 512M > /sys/block/zram0/mem_limit
mkswap /dev/zram0
swapon -p 100 /dev/zram0

# 第 2 层：强压缩、大容量、低优先级
echo zstd > /sys/block/zram1/comp_algorithm
echo 8G   > /sys/block/zram1/disksize
echo 2G   > /sys/block/zram1/mem_limit
mkswap /dev/zram1
swapon -p 50 /dev/zram1

# 第 3 个当临时文件系统
echo lz4  > /sys/block/zram2/comp_algorithm
echo 1G   > /sys/block/zram2/disksize
mkfs.ext4 -q /dev/zram2 && mount /dev/zram2 /mnt/zram2

# 观察
zramctl
cat /proc/swaps
for d in /sys/block/zram*; do echo "$d $(cat $d/mm_stat 2>/dev/null | cut -d' ' -f1-3)"; done
```

`mm_stat` 前三个字段是原始大小、压缩后大小、实际占用（字节），跨设备要逐个读再累加。

## 9. 未验证

- 上面的配置序列是按源码路径推导 + 常见发行版用法写的，**没有在目标机器上逐条执行过**；`swapon -p` 与 `mem_limit` 组合的实际表现（换出失败后的内存压力）需要实测。
- 每设备 per-CPU buffer 的具体开销是按 CPU 数推算，不是实测值。
- 各发行版的 `zram-generator`/`zramctl` 封装行为不同（尤其 `--find` 是否会复用），未逐一确认。

## 相关

- [[zram 框架介绍]]
- [[zram 多设备之间能否迁移内存]]
