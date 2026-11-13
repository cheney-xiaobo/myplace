---
title: ARM64 Linux 6.6 TLBI 失效场景与数据清理边界
aliases:
  - TLBI 数据清理
  - ARM64 TLB 刷新场景
  - Linux 6.6 TLBI 场景
tags:
  - linux/kernel
  - linux/mm
  - arch/arm64
  - tlbi
  - cache-coherency
source: /Users/cheney/work/code/ub/kernel @ 8003dfcde32fff47a74060d8ecbe8d246703d334（openEuler OLK-6.6 / kernel 6.6.0）
created: 2026-11-09
updated: 2026-11-09
status: 静态源码核对；未在 ARM64 硬件上实测
---

# ARM64 Linux 6.6 TLBI 失效场景与数据清理边界

## 0. 先纠正“用 TLBI 清理数据”的说法

**TLBI 清理的是旧地址翻译及相关的页表遍历缓存，不是页内数据。** 它解决的是：页表已经改变，但某个 CPU、vCPU 或设备翻译单元仍可能沿用旧的 VA/IPA/IOVA → PA 映射、权限或内存属性。

因此，下面三件事必须分开：

| 目标 | 正确机制 | TLBI 是否负责 |
| --- | --- | --- |
| 阻止继续通过旧映射访问物理页 | TLB / walk-cache 失效 | **是** |
| 让 CPU 与设备看到正确的内存数据 | D-cache clean / invalidate、DMA 同步 | 否 |
| 让新指令被正确取指 | D-cache → PoU、I-cache 失效、上下文同步 | 否，单独 TLBI 不够 |
| 把内存内容清零或安全擦除 | 显式清零、带清零语义的分配/释放路径 | 否 |

ARM64 的核心 API 注释明确把 `flush_tlb_range()` 定义为使用户地址范围及其 walk-cache 失效，把 `flush_tlb_kernel_range()` 定义为对应的内核映射版本；它们不是内存内容擦除接口（`arch/arm64/include/asm/tlbflush.h:240-291`）。

> 本文说“需要刷 TLBI”，准确含义是**在允许旧物理页被释放、复用、写回或改变用途之前，必须保证旧翻译已经失效并完成必要的同步**。这不等于每次页表操作都立即执行一条固定 TLBI：内核可能按范围合并、延迟等待，或在确认不存在旧翻译时跳过。

## 1. 判断条件：不是“有没有写页表”，而是“旧翻译还能不能被使用”

满足下列任一条件时，应重点检查失效路径：

1. **有效映射被删除**：旧 VA/IPA/IOVA 仍可能命中 TLB，而物理页即将释放或复用。
2. **有效映射的权限或属性被修改**：旧条目可能仍允许写入、执行，或沿用旧 cacheability/shareability。
3. **物理页即将开始 I/O 或改变用途**：旧的“可写翻译”若仍存在，CPU 可能在 I/O 开始后继续改写该页。
4. **翻译上下文编号即将复用**：ASID/VMID 回卷后，旧代条目不能冒充新地址空间的翻译。
5. **另一个翻译域的页表被修改**：CPU stage-1、KVM stage-2、SMMU stage-1/2、设备 ATC 各自需要对应的失效，不可相互替代。

反之，如果旧条目本来就不可能进入 TLB，就不能机械地补一次 TLBI。本树对“仅从 present-invalid 改为 valid、其他位不变”的内核映射明确跳过失效，因为 non-valid entry 不能被 TLB 缓存（`arch/arm64/mm/pageattr.c:132-147`）。

## 2. 需要失效的典型场景

### 2.1 用户地址空间：unmap、权限变化、回收与复用

| 场景 | 为什么需要 | 本树实现证据 |
| --- | --- | --- |
| `munmap`、`MADV_DONTNEED` 等 zap/unmap；进程地址空间拆除 | 页表项已清，但其他 CPU 可能仍保留旧翻译；必须先完成失效，再释放页表和物理页资源 | `zap_page_range_single()` 以 `tlb_gather_mmu()` 开始、`tlb_finish_mmu()` 收尾（`mm/memory.c:1974-1981`）；`tlb_flush_mmu()` 先执行 TLB-only，再执行资源释放（`mm/mmu_gather.c:362-374`），真正是否 flush 由已清页表/表项标志决定（`include/asm-generic/tlb.h:458-470`） |
| 页回收、反向映射解除，尤其 dirty 页写回 | 旧的可写 TLB 项若在 I/O 开始后仍可用，会造成丢失写；释放前失效也用于防止旧映射继续访问而造成数据泄漏 | `try_to_unmap_flush()` 的注释同时说明 I/O 与 free 两个约束（`mm/rmap.c:610-636`）；dirty folio 在 `pageout()` 前调用 `try_to_unmap_flush_dirty()`（`mm/vmscan.c:1364-1370`） |
| `mprotect`、userfaultfd write-protect、NUMA balancing 等 `change_protection` 类修改 | 旧权限或属性仍可能缓存；是否失效由新旧 PTE 决定，并累积到范围刷新 | 修改后由 `pte_needs_flush(oldpte, ptent)` 判定，再调用 `tlb_flush_pte_range()`（`mm/mprotect.c:167-195`） |

这里有两个容易误述的例外：

- **不是每次普通进程切换都刷。** ASID 相同代的快路径直接切上下文；只有 ASID 耗尽并回卷后，各 CPU 才根据 `tlb_flush_pending` 在后续切换中执行 `local_flush_tlb_all()`（`arch/arm64/mm/context.c:190-215`、`arch/arm64/mm/context.c:243-258`；`switch_mm()` 入口在 `arch/arm64/include/asm/mmu_context.h:260-274`）。
- **不是进程退出就固定刷整个 mm。** `fullmm` 拆除时，ASID 分配器保证编号复用前完成全失效；当前路径可能只处理 walk-cache，未释放页表时甚至直接返回（`arch/arm64/include/asm/tlb.h:53-73`）。

### 2.2 内核映射：vmalloc/vmap、set_memory、percpu 与内存热移除

| 场景 | 失效要求与证据 |
| --- | --- |
| 活跃内核映射的权限/属性更新 | `update_mapping_prot()` 修改 live kernel mappings 后调用 `flush_tlb_kernel_range()`（`arch/arm64/mm/mmu.c:922-936`）；`set_memory_*` 的公共路径也按修改类型决定是否刷新（`arch/arm64/mm/pageattr.c:132-149`） |
| `vfree`/`vunmap`、vmap alias 拆除 | vmalloc 会先 no-flush unmap，再通过 lazy purge 合并刷新（`mm/vmalloc.c:2420-2427`、`mm/vmalloc.c:2280-2323`）。**延迟不是取消**：`vm_unmap_aliases()` 返回后，调用者持有的页不应再有 vmap 层的旧 alias（`mm/vmalloc.c:2891-2954`） |
| percpu 虚拟映射解除 | `pcpu_post_unmap_tlb_flush()` 对解除映射的 kernel range 调用 `flush_tlb_kernel_range()`；注释说明若区域交还 vmalloc，可由 vmalloc lazy flush 处理（`mm/percpu-vm.c:172-190`） |
| 内存热插拔移除映射 | PTE/PMD/PUD 路径都是先清映射并刷 kernel range，再按参数释放热插拔页（`arch/arm64/mm/mmu.c:1329-1408`） |

### 2.3 ASID 回卷：上下文编号复用

正常切换靠 ASID 隔离旧条目；ASID 耗尽时必须建立新一代上下文，并清除可能与旧编号匹配的翻译。本树在 `new_context()` 找不到空闲 ASID 时增加 generation 并执行 `flush_context()`，各 CPU 随后在上下文切换路径检查 pending 位并本地全刷（`arch/arm64/mm/context.c:190-215`、`arch/arm64/mm/context.c:249-260`）。

这仍然不是“清理进程数据”，而是防止**不同生命周期、但编号相同的地址空间混用翻译**。

### 2.4 KVM：stage-2、vCPU 跨核与 VMID 回卷

KVM 不能只做宿主用户 mm 的 `flush_tlb_*`，还要处理 VMID/stage-2：

| 场景 | 需要的动作 | 证据 |
| --- | --- | --- |
| stage-2 IPA 映射被删除或修改 | 单 IPA/range 失效先处理 stage-2，再失效该 VMID 的 stage-1；顺序不能反，否则另一 CPU 可能用旧 stage-2 重新填入组合翻译 | `arch/arm64/kvm/hyp/vhe/tlb.c:82-112`、`arch/arm64/kvm/hyp/vhe/tlb.c:146-188` |
| vCPU 装载到上次运行其他 vCPU 的 pCPU | KVM 为保持各 vCPU 的 TLB/I-cache 私有性，执行本核 `vmalle1` 与 `ic iallu`；这是上下文隔离，不是清 guest RAM | 判定在 `arch/arm64/kvm/arm.c:1135-1155`，指令序列在 `arch/arm64/kvm/hyp/vhe/tlb.c:190-203` |
| VMID 耗尽并回卷 | 广播 `alle1is`，使旧 VMID 代的所有相关 EL1 stage-1/stage-2 翻译失效 | 回卷路径在 `arch/arm64/kvm/vmid.c:49-77`、`arch/arm64/kvm/vmid.c:129-146`，指令在 `arch/arm64/kvm/hyp/vhe/tlb.c:205-210` |

memslot、dirty logging、宿主 mmu_notifier、pause/resume 等更细的触发时间线见 [[Firecracker microVM 在 ARM 上的 TLB 刷新时机]]。

### 2.5 SMMU/IOMMU：IOVA 与设备 ATC 是独立失效域

设备经过 SMMU 做 DMA 翻译时，**CPU 的 `flush_tlb_kernel_range()` 不能代替 SMMU TLB 或设备 ATC 失效**。

ARM SMMUv3 的 unmap 收集范围后由 `.iotlb_sync` 进入 `arm_smmu_tlb_inv_range_domain()`（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:3507-3539`）。该驱动按 stage-1 ASID 或 stage-2 VMID 发 SMMU command-queue TLBI，并继续调用 ATC 失效；嵌套 stage-2 改变时还需处理相关 stage-1（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:2385-2515`）。

所以“IOVA 已 unmap”与“CPU TLB 已刷”不是同一个完成条件。驱动应使用 DMA/IOMMU 子系统接口，不应自行补一条 CPU TLBI 来代替 `.iotlb_sync`。

## 3. 这些场景不需要、或不能只靠 TLBI

### 3.1 普通读写页内容

只修改页面里的普通数据，没有改变地址翻译、权限或属性，**不会因为“写了数据”而需要 TLBI**。是否需要 cache 维护取决于访问者是否非一致性地共享该内存。

### 3.2 DMA 数据一致性

本树 ARM64 DMA 同步路径在设备访问前执行 `dcache_clean_poc()`；CPU 读取设备写入的数据时，按方向执行 `dcache_inval_poc()`（`arch/arm64/mm/dma-mapping.c:20-44`）。正确入口是 DMA API，例如 `dma_map_single()`/`dma_unmap_single()` 或 `dma_sync_single_for_device()`/`dma_sync_single_for_cpu()`，由 DMA ops 调用对应架构与 IOMMU 路径。

- **clean**：把脏 cache line 写回到指定一致点，不是清零。
- **invalidate**：使缓存副本失效，也不等于擦除物理内存；错误地对含未写回数据的范围做 invalidate 还可能丢数据。
- PoC/PoU/PoP 的接口语义分别列在 `arch/arm64/include/asm/cacheflush.h:37-79`，对应 `civac`、`ivac`、`cvac`、`cvap` 汇编实现位于 `arch/arm64/mm/cache.S:102-197`。

### 3.3 修改可执行代码

写入新指令后，旧问题可能同时存在于 D-cache、I-cache 和其他 CPU 的取指状态。`flush_icache_range()` 先执行 `caches_clean_inval_pou()`，再让其他在线 CPU 经历上下文同步事件以重新取指（`arch/arm64/include/asm/cacheflush.h:82-105`）；底层注释明确其用途是“代码已写入且即将执行”（`arch/arm64/mm/cache.S:45-58`）。

因此，**只刷 TLB 不足以发布自修改代码、JIT 代码或新加载指令**；若映射本身也改了，则 cache 维护和 TLB 失效可能都需要。

### 3.4 真正的内容清零或安全擦除

TLBI 不访问并改写页内 payload，D-cache invalidate 也不是安全擦除。若需求是防止下一任分配者读到旧内容，应使用对应分配器的清零保证或显式清零，并单独评估 cache 残留、设备 DMA、交换/持久化介质等问题；不能拿一次 TLBI 当作数据销毁证明。

### 3.5 已确定没有旧有效翻译

新建一个此前 invalid 的映射，通常不是为了“清旧数据”而刷。本树对纯 present-invalid → valid 的内核属性变化给出了直接跳过条件（`arch/arm64/mm/pageattr.c:139-146`）。普通进程切换同样不应默认等同于全 TLB 失效（`arch/arm64/mm/context.c:243-258`）。

## 4. 开发时应该怎么做

### 4.1 前置条件与启用方式

- 本文针对**内核内 ARM64 MM/KVM/IOMMU 路径**，不需要用户部署或加载一个“TLBI 工具”，也没有供普通用户手动清理数据的 `tlbi` 命令。
- 用户态执行 `munmap`、`mprotect`、`madvise` 等操作后，由内核自动选择是否以及何时失效；用户程序不应尝试自行发 TLBI。
- 内核源码头注释明确要求驱动及多数内核代码优先使用 TLB 管理例程，而非直接嵌入 `__tlbi()` 汇编宏（`arch/arm64/include/asm/tlbflush.h:21-31`）。

### 4.2 按调用层选择接口

| 你正在做什么 | 应使用的路径 | 不要做什么 |
| --- | --- | --- |
| 用户进程解除映射或修改权限 | `munmap`、`mprotect`、`madvise` 等系统调用 | 假设每次调用都产生一条固定 TLBI，或手动补 cache/TLB 汇编 |
| 实现通用 MM 的 unmap/zap | `mmu_gather` / `tlb_finish_mmu` 等既有协议，保持“失效完成后释放”的顺序 | 绕过 gather 直接释放已 unmap 的页 |
| 管理 vmalloc/vmap 映射 | `vmalloc`/`vfree`、`vmap`/`vunmap` 及 `vm_unmap_aliases()` 等现有接口 | 在 lazy flush 尚未保证完成时复用旧 alias 指向的页 |
| 修改内核直接映射属性 | `set_memory_*` 等公共接口；只有实现架构/MM 底层路径时才直接使用 `flush_tlb_kernel_range()` | 只改 PTE 而不同步 live kernel mapping |
| 修改 KVM guest 映射 | KVM stage-2 MMU、memslot、mmu_notifier 既有接口 | 用宿主用户 mm flush 代替 VMID/stage-2 失效 |
| 管理设备 IOVA | IOMMU core 的 map/unmap/iotlb sync 协议；普通驱动优先 DMA API | 用 CPU TLBI 代替 SMMU/ATC 失效 |
| 同步非一致性 DMA 缓冲区 | DMA map/unmap 与 `dma_sync_single_for_*` | 用 TLBI 代替 D-cache clean/invalidate |
| 发布新指令 | 对应架构的 `flush_icache_range()`/代码补丁框架 | 只做 TLBI，遗漏 I/D-cache 与取指同步 |
| 清除敏感数据 | 有明确清零语义的分配/释放路径或显式清零 | 把 TLBI 或 cache invalidate 当作内容擦除 |

### 4.3 为什么不能省略屏障或随意改成单条指令

ARM64 通用失效模板是：先 `DSB ISHST` 确保页表更新完成，再执行 TLBI，随后 `DSB ISH` 等待失效完成；若失效的是内核映射，还需要 `ISB` 丢弃按旧映射预取的指令（`arch/arm64/include/asm/tlbflush.h:231-238`）。本树 `flush_tlb_kernel_range()` 已封装完整序列及范围过大时的全刷退化（`arch/arm64/include/asm/tlbflush.h:733-751`），中间页表 walk-cache 失效另有专门接口（`arch/arm64/include/asm/tlbflush.h:753-765`）。

**不能把这个模板简化成“所有用户数据 TLBI 后都必须机械执行完全相同的 ISB”**，应按具体映射和调用路径使用现成 wrapper。

批量路径也不是“省掉正确性”：本树回收批处理先为各页发 TLBI，最后用一个 DSB 等待完成；若与 mprotect/munmap 等并发批刷发生冲突，则可能退化为整 mm flush（`arch/arm64/include/asm/tlbflush.h:349-373`）。

## 5. 一个可直接照用的审查顺序

审查一个“是否需要刷”的改动时，依次问：

1. **改了哪种翻译？** 用户 VA、内核 VA、guest IPA，还是设备 IOVA？
2. **旧条目是否可能仍有效？** 是纯 invalid → valid，还是有效映射的删除/权限/属性变化？
3. **危险窗口在哪里？** 物理页释放/复用、dirty 页 I/O、代码执行、设备 DMA、ASID/VMID 复用之前，旧翻译是否必须已经完成失效？
4. **失效域是否完整？** CPU stage-1、KVM stage-2 + stage-1、SMMU、ATC 是否分别覆盖？
5. **是否还缺数据 cache 或 I-cache 维护？** TLBI 不能替代 DMA 同步和 `flush_icache_range()`。
6. **需求是否其实是内容清零？** 若是，应另走清零/安全擦除设计。
7. **能否复用子系统 wrapper？** 优先让 MM、vmalloc、KVM、IOMMU、DMA 路径决定 range、广播、屏障和批处理，不在驱动里手写 TLBI。

## 6. 基线、推断与未验证项

- **源码事实**：以上文件与行号来自本机 openEuler OLK-6.6 树，HEAD 为 `8003dfcde32fff47a74060d8ecbe8d246703d334`，`Makefile:2-6` 标识 6.6.0。
- **发行版边界**：该树相对上游 `v6.6` 有大量回合修改；TLBID/TLBIP 等实现不能当作上游 6.6 的通用能力。相关粒度与优化见 [[ARM64 TLBI 与 x86 TLB 失效]]，能力检测和调用方式见 [[ARM64 TLBI 能力怎么用（6.6 与 6.10）]]。
- **分析性归纳**：§1 的判断条件与 §5 的审查顺序是依据上述源码路径整理的工程规则，不是某个单独源码函数的原文。
- **未验证**：未在 ARM64 硬件上构造并发 unmap、DMA、KVM 或 ASID/VMID 回卷测试，也未测量性能；本文不声称运行时验证通过。

## 7. 相关笔记

- [[ARM64 TLBI 与 x86 TLB 失效]]：TLBI 粒度、ASID、广播、range 与本树发行版特性。
- [[ARM64 TLBI 能力怎么用（6.6 与 6.10）]]：硬件能力检测、Kconfig 门控、API 与退化条件。
- [[Firecracker microVM 在 ARM 上的 TLB 刷新时机]]：KVM stage-2、VMID、vCPU 跨核和 microVM 生命周期。
- [[Linux 内核知识库]]：主题索引。
