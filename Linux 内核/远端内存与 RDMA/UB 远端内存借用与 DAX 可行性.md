---
title: UB 远端内存借用与 DAX 设备可行性
aliases:
  - ub.mem 能否做成 dax
  - OBMM 形态与 DAX
tags:
  - ub
  - obmm
  - dax
  - 内存池化
source: /Users/cheney/work/code/ub/kernel, /Users/cheney/work/code/ub/ubs-mem
created: 2026-11-12
updated: 2026-11-12
status: 分析完成（未实测）
---

> 本文引用路径以 `/Users/cheney/work/code/ub` 为根：`kernel/...` 是 6.6 内核（含 UB 补丁），`ubs-mem/...` 是池化内存 SDK。

# 结论

分两种形态回答，两者结论相反：

| 形态 | 能否做成 DAX 设备 | 原因 |
| --- | --- | --- |
| **numa 形态**（`ubs_mem_numa_create`） | **不能**，且不可配置绕过 | 该区间已经被 `add_memory_remote()` 上线成 `System RAM (Remote)`，进入 System RAM 资源树与 buddy；DAX 的内核入口明确拒绝 System RAM 区间 |
| **fd 形态**（`ubs_mem_fd_create` → `/dev/obmm_shmdev<memid>`） | **原理上可以，当前不成立** | 区间在 iomem 里是 `IORESOURCE_MEM`（不在 System RAM 集合），本机也没有 struct page，符合 device-dax 对"设备内存"的要求；但没有任何 dax 代码，且 MEMID 叶资源是 BUSY、OBMM 明令外部不得改资源树 |

一句话：**能不能做取决于"这块内存有没有被上线成内存"，而不是远端借用本身。** 只要走了 numa 形态，DAX 这条路就永久关闭；只有不走 numa 的 fd 形态才留有可能，且必须改 OBMM 本身，不是"加个模块/prtkl 参数"就能上线。

## 1. 借用内存的两条呈现路径

接口层只有这两种形态，二选一：

| 形态 | 创建接口 | 用户态怎么用 |
| --- | --- | --- |
| numa | `ubs_mem_numa_create()`（`ubs-mem/3rdparty/ubse/include/ubs_engine_mem.h:290`） | 变成 NUMA 节点上的内存，进程/VM 按 NUMA 分配即可，无文件接口 |
| fd | `ubs_mem_fd_create()`（`ubs-mem/3rdparty/ubse/include/ubs_engine_mem.h:141`） | `/dev/obmm_shmdev<memid>`（前缀 `ubs-mem/3rdparty/ubse/include/ubs_engine_mem.h:110`），`open` + `mmap` |

另有 `UBS_MEM_FLAG_CACHEABLE`（`ubs-mem/3rdparty/ubse/include/ubs_engine_mem.h:33`）决定该区间是否可缓存映射——这条对 CPU 直接 load/store 的带宽影响很大，也是后面 DAX 是否现实的前提之一。

### 资源树怎么长的

OBMM 的注释直接画出来了（`kernel/drivers/ub/obmm/obmm_resource.c:29-38`）：

```
(iomem_resource)
   PREIMPORT_UBMEM
     System RAM (Remote)      <- numa 形态多出的一层
       MEMID_1
       MEMID_2
   DIRECT_IMPORT_UBMEM
     System RAM (Remote)      <- 直接导入但选 numa 形态时也有
       MEMID_3
   DIRECT_IMPORT_UBMEM
     MEMID_4                  <- 纯 fd 形态：没有 System RAM 层
```

两层资源各有分工：

- UBMEM 资源（`PREIMPORT_UBMEM` / `DIRECT_IMPORT_UBMEM`）是 `iomem_resource` 的直接子节点，flags 只有 `IORESOURCE_MEM`（`kernel/drivers/ub/obmm/obmm_resource.c:87`）。
- MEMID 资源是叶子，flags 是 `IORESOURCE_MEM | IORESOURCE_BUSY`（`kernel/drivers/ub/obmm/obmm_resource.c:198`）。
- 走 numa 形态时中间夹一层 `System RAM (Remote)`，这一层才是"把远端内存当成内存用"的关键。

## 2. numa 形态为什么不能变成 DAX

三条硬证据，都是内核自身代码，不是设计约定：

### 2.1 它已经是 driver-managed System RAM

远端节点上线的资源名被硬编码为 `System RAM (Remote)`（`kernel/mm/memory_hotplug.c:1541`），而只要资源名不是裸的 `"System RAM"`，就追加 `IORESOURCE_SYSRAM_DRIVER_MANAGED`（`kernel/mm/memory_hotplug.c:259-262`）。同时 flags 里带 `IORESOURCE_BUSY`。

`add_memory_remote()` 就是这条路上的执行者（`kernel/drivers/base/numa_remote.c:436`，其注释在 `:425` 说明资源名必须是 `System RAM (Remote)` 格式），调用方是 OBMM 的 numa 创建路径（`kernel/drivers/ub/obmm/obmm_import.c:133`、`:149`，预导入路径见 `kernel/drivers/ub/obmm/obmm_preimport.c:112`）。

### 2.2 devdax 抢不到这块区间

`dev_dax_probe()` 对每个 range 先 `devm_request_mem_region()`（`kernel/drivers/dax/device.c:436-442`）。区间已经是 BUSY 的 System RAM 资源，申请直接 `-EBUSY`。

### 2.3 就算抢到了，memremap 也会拒绝

`devm_create_dev_dax()` 最终走 `devm_memremap_pages()`，后者在 `kernel/mm/memremap.c:195-202` 做检查：

```c
is_ram = region_intersects(range->start, range_len(range),
        IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE);
if (is_ram != REGION_DISJOINT) {
    WARN_ONCE(1, "attempted on %s region ...");
    return -ENXIO;
}
```

**落在 System RAM 上的区间一律 `-ENXIO`。** 这是 DAX 子系统的定义性约束：device-dax 管理的是"设备内存"，不是"系统内存"。两者是互斥状态，没法让一块区间同时被两个语义owner。

### 2.4 反向桥梁印证互斥

`dax/kmem` 做的恰好是反方向：从 devdax 拿区间 → `request_mem_region` → 改 flags 成 `IORESOURCE_SYSTEM_RAM` → `add_memory_driver_managed()` 上线成 System RAM（`kernel/drivers/dax/kmem.c:137-168`）。

所以内核里这条路的单向性是明确的：**iomem 设备区间 →（devdax）→ System RAM**。numa 形态已经站在终点，倒不回去。

## 3. fd 形态为什么"原理可行、当前不成立"

### 3.1 原理可行

- 纯 fd 形态的区间在资源树里只有 `DIRECT_IMPORT_UBMEM` → `MEMID_*`，**没有 System RAM 层**，因此 `region_intersects(SYSTEM_RAM)` 是 disjoint，2.3 那条检查放行。
- 本机没有这些页的 struct page：导入区间在 `obmm_vma_access()` 里走 `ioremap_prot()`（`kernel/drivers/ub/obmm/obmm_shm_dev.c:226`），而不是 `pfn_to_page()`+`vmap()`。这正是 device-dax 期待的"无页结构、按设备内存访问"的形态。
- 可缓存映射是可选能力（`UBS_MEM_FLAG_CACHEABLE`），fd 形态还有显式的可缓存/不可缓存一致性检查（`kernel/drivers/ub/obmm/obmm_shm_dev.c:285-290`），CPU load/store 的前提在软件侧是具备的。

### 3.2 当前不成立

1. **没有任何 dax 代码。** `drivers/ub/` 下无 `dax_region` / `devm_create_dev_dax` 调用；ubs-engine、ubs-mem 也不产生 dax 设备。
2. **MEMID 叶资源是 BUSY**（`kernel/drivers/ub/obmm/obmm_resource.c:198`）。外部 provider 想在这段上 `request_mem_region` 一样会 `-EBUSY`，除非 OBMM 在注册 MEMID 时就不置 BUSY。
3. **OBMM 明令外部不得改这棵子树**：`kernel/drivers/ub/obmm/obmm_resource.c:48-52` 写明外部访问者只读，唯一例外是 OBMM 自己触发的内存热插拔/NUMA 建立流程。

结论：**要做只能由 OBMM 自己长出一个 dax provider**，绕不过去。

## 4. 真要做，施工清单

1. **资源模型**：fd 形态下 MEMID 注册时增加一种"dax 模式"，不置 `IORESOURCE_BUSY`（或给 dax 单独一层父资源），让出区间给 dax bus。
2. **provider**：`alloc_dax_region()` + `devm_create_dev_dax()`，按 UB 的拆分粒度设置 `align`（建议 ≥2M，可显著减少 vmemmap 开销）。
3. **生命周期**：这是最大障碍。dax_region 是**静态注册**、随设备销毁；而借用关系是动态的——create/delete、对账回收、超时归还、借出节点故障都会让远端内存凭空消失（ubturbo 的 RMRS 侧另有 `MemBorrowRollback` 一类回滚逻辑，见 `plugins/rmrs/src/ubturbo_plugin/intranode_strategy/rmrs_rollback_module.h`）。DAX 没有撤销模型：区间没了，已 mmap 的进程只能吃 SIGBUS/MCE。要做得先设计"借用关系失效 → dax 下线 → 通知/阻塞访问者"的链路。
4. **权限语义**：fd 形态有 owner(uid/gid/pid)/mode/permission（`ubs-mem/3rdparty/ubse/include/ubs_engine_mem.h:141` 起的接口族），devdax 只有文件权限 + 单 pgmap，per-client 语义要自己补。
5. **共享**：同一 range 多个 MEMID 给多个进程用时，dax 侧要怎么映射（一个 region 多 dev_dax）需要定模型。

工作量量级：不是"参数"级，是一个新的子驱动 + 资源模型改造 + 撤销链路设计。

## 5. 值不值得

| 维度 | 现状 fd 形态（`/dev/obmm_shmdev`） | 改造成 devdax |
| --- | --- | --- |
| 用户态零拷贝 mmap | 已有 | 同样有 |
| 可缓存/不可缓存控制 | 显式标志位 + 一致性校验 | 需塞进 pgmap 语义 |
| PMD 粒度大页映射 | 已有（`OBMM_MMAP_GRANU_PMD`） | dax 有 align，但语义不同 |
| 属主/权限/共享计数 | 有（owner/mode/mmap_count） | 无，要自建 |
| 动态回收 | 有完整回收/对账链路 | 无撤销模型，风险大 |
| 文件系统直挂（fsdax） | 无 | DAX 唯一实质增量 |
| 通用生态（VFIO/gmem、daxctl） | 无 | 有 |

**判断**：如果目标只是"进程/容器零拷贝访问远端内存"，fd 形态已经够用而且控制更细，改 DAX 是负收益。如果目标变成"在远端内存上直接跑文件系统（fsdax）"或"喂给只认 dax 的上层（部分 VM/加速卡栈）"，才有做的价值——但那时候要连带做块设备/pmem 那一套，成本远高于只做 devdax。

另外一个事实：DAX 也无法解决"远端内存掉线"这个根本问题，它没有失效通知机制；numa 形态同样不能（掉线表现为 MCE）。两种形态在可靠性上都依赖上层（SMAP 的迁回、RMRS 的归还）。

## 6. 未验证

- 未在 UB 硬件上实测任何 DAX 行为；本文全部结论来自读码，没有跑过 dax 创建。
- UB 远端物理区间能否被 `devm_memremap_pages()` 接受（ZONE_DEVICE + aarch64 架构侧是否允许该 PA 段建 vmemmap），未验证。
- OBMM 的 MEMID 拆分粒度（`unit_size`）与 dax `align` 的匹配关系未验证。
- `IORESOURCE_SYSRAM_DRIVER_MANAGED` 在本内核中被 SMAP/ubdma 插件用作"远端内存"的识别条件（`ubturbo` 的 `plugins/smap/src/kernel/comm/iomem.c`、`plugins/ubdma/src/ub_dma_iomem.c`，均在 `update_resource()` 中过滤该 flag）——这是读取源码确认的，但"是否还有别的模块依赖该形态"未做全仓排查。

## 相关

- [[UBTurbo SMAP 架构与实现原理]]
- [[UBTurbo SMAP 冷热判断与交换速率]]
- [[RDMA 使用方式与性能调优]]
