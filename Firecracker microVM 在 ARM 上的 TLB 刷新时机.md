---
title: Firecracker microVM 在 ARM 上的 TLB 刷新时机
aliases:
  - Firecracker TLB
  - microVM TLB
  - VMID 回卷
  - KVM stage-2 失效
tags:
  - linux/kernel
  - linux/mm
  - arch/arm64
  - kvm
  - firecracker
  - microvm
source: /Users/cheney/work/code/ub/kernel @ 8003dfcde32f (openEuler OLK-6.6 / kernel 6.6.0)；Firecracker main 分支官方文档
created: 2026-11-01
updated: 2026-11-01
status: 内核侧已静态验证；Firecracker 侧依据官方文档；未在 ARM 硬件实测
---

# Firecracker microVM 在 ARM 上的 TLB 刷新时机

场景：ARM 主机上用 Firecracker 起 microVM，沙箱启动、运行任务，中途 pause / resume。

**一句话结论**：`pause` 和 `resume` 本身**不刷 TLB**（只是让 vCPU 线程退出/重入 KVM_RUN）。真正大量刷新的地方是四件事：**VMID 回卷**、**vCPU 落到"上次跑的是别的 vCPU"的物理核**、**stage-2 页表被整块改动（dirty log / memslot / mmu_notifier）**、**宿主 mm 里 guest RAM 被 unmap**。

## 1. 先分清四层失效域

ARM 上一台 microVM 同时涉及四套 TLB 命名空间，混在一起谈就会得出错误结论：

| 层 | 谁维护 | 典型失效指令 | 谁触发 |
| --- | --- | --- | --- |
| guest stage-1 | guest 内核（自己的 ASID 分配器） | guest 自己的 `tlbi vae1…` | guest 内部 unmap、guest 内进程切换 |
| **stage-2（VMID）** | KVM hyp | `ipas2e1is`（单页/区间）、`vmalls12e1is`（整个 VMID）、`alle1is`（全部） | memslot 变更、dirty log、mmu_notifier、VMID 回卷、vCPU 跨核 |
| host 进程 mm（ASID） | host 内核 | 区间 TLBI / 整 mm | `munmap` guest RAM、进程退出、swap |
| host 内核 | host 内核 | `flush_tlb_kernel_range` | vmalloc / 模块加载 |

两条要点：

- **guest 的 TLB 失效不会被 trap**。本树的 `HCR_TTLB` 只出现在嵌套虚拟化路径（`arch/arm64/kvm/emulate-nested.c`），普通 KVM 不设该位；KVM 的 vTLBID 支持只 trap `TLBIDIDR_EL1` 的读（`set_tlbididr_trap()`，`arch/arm64/kvm/arm.c:970-980`）。所以 guest 内部怎么刷，对 host 侧开销没有直接关系，也不进入 host 的计数器。
- **stage-2 的失效是"按 VMID 寻址"的**：`__kvm_tlb_flush_vmid()` 只发一条 `vmalls12e1is`（`arch/arm64/kvm/hyp/vhe/tlb.c:174-188`），不需要给其它 CPU 发 IPI——硬件在 inner-shareable 域内广播。

### 1.1 五类 TLBI，只有一类与 vCPU 绑定

| # | 触发 | 指令 | 失效范围 | 发起方 | 与"哪个 vCPU 进场"绑定吗 |
| --- | --- | --- | --- | --- | --- |
| ① | vCPU 装载到一个 pCPU | `vmalle1` + `ic iallu` | **本核**上该 VMID 的全部 stage-1 表项 + 本核 I-cache | 本核，本地（**不带** `is`） | **是**，唯一一类 |
| ② | stage-2 页表改动 | `vmalls12e1is` / `ipas2e1is` | 整个 VMID（或区间）的 stage-2 | 任意线程发起，硬件域内广播 | 否，VM 级 |
| ③ | VMID 回卷 | `alle1is` | 全系统所有 EL1 的 stage-1 + stage-2 | `flush_context()` | 否，全局 |
| ④ | host 进程 mm 变化 | 区间 TLBI / 整 mm | host 地址空间 | host 内核 | 否，进程级 |
| ⑤ | guest 内部失效 | guest 自己的 `tlbi` | guest stage-1/2 | guest 内核 | 否，且 host 不可见（见本节第一条） |

**容易混的一点**：① 的**触发**是 per-vCPU 的（"这次上来的是哪个 vCPU"），但**失效范围是 per-VM 的**（整个 VMID 在本核的所有表项），并不"只清这个 vCPU 的"。所以"vCPU 的刷新"这个说法只在触发维度成立。

**硬件不会自动刷**。TLB 是每个 PE 私有的，条目带 ASID/VMID 标签；切 vCPU 或换物理核时，硬件只是换标签去查，不清表。观察到"vCPU 换到新核后往往刷一次"，是因为新核的 `last_vcpu_ran` 初值是 -1，`-1 != vcpu_id` 必然进入刷新分支（`arch/arm64/kvm/arm.c:1152`）——这是 KVM 的保守选择（注释所谓 over-invalidation），不是硬件行为。

### 1.2 两阶段翻译：谁建表、谁刷表

```
guest VA ──stage-1──▶ IPA ──stage-2──▶ PA
          TTBR0/1_EL1        VTTBR_EL2
          TCR_EL1            VTCR_EL2
```

| | stage-1 | stage-2 |
| --- | --- | --- |
| 输入 → 输出 | VA → IPA | IPA → PA |
| 页表所有者 | **guest 内核** | **KVM** |
| 根指针 | `TTBR0_EL1` / `TTBR1_EL1`（guest 每个进程一套） | `VTTBR_EL2` = stage-2 PGD 物理地址 + VMID + CNP（`kvm_get_vttbr()`，`arch/arm64/include/asm/kvm_mmu.h:287-297`） |
| 配置 | `TCR_EL1` / `SCTLR_EL1` | `VTCR_EL2`（`kvm_get_vtcr()`，`arch/arm64/kvm/hyp/pgtable.c:626-664`）：物理地址宽度、IPA 位数、起始层级 SL0、硬件 AF、VMID 位宽 |
| 谁改表 | guest 自己，**host 完全不碰** | `user_mem_abort()` → `kvm_pgtable_stage2_map()`（`arch/arm64/kvm/mmu.c:1713`） |
| 谁发失效 | guest 自己，host 看不到 | KVM |
| 失效作用域 | 当前 VMID 内 | 整个 VMID 或某个 IPA |

**host 对 stage-1 的唯一动作是保存/恢复寄存器**，不解析、不改表：`ctxt_sys_reg(ctxt, TTBR0_EL1) = read_sysreg_el1(SYS_TTBR0)`（`arch/arm64/kvm/hyp/include/hyp/sysreg-sr.h:77-78`），恢复在 `:162-163`。

**VTCR 和 VTTBR 每次进 guest 都重写**：`__load_stage2()` 两行 `write_sysreg`（`arch/arm64/kvm/hyp/vhe/switch.c:362`、`arch/arm64/kvm/hyp/vhe/tlb.c:56`，实现见 `arch/arm64/include/asm/kvm_mmu.h:303-311`）。所以换 VM 不额外付寄存器钱。

**stage-2 是按需建的**：guest 首次访问 → stage-2 fault → `kvm_handle_guest_abort()`（`arch/arm64/kvm/mmu.c:1763`）→ 查 memslot → `user_mem_abort()` 建映射。这是 Firecracker "Demand fault paging enabled by default" 的落脚点。

**关键：stage-2 失效一定连带整个 stage-1。**

```c
	/*
	 * We could do so much better if we had the VA as well.
	 * Instead, we invalidate Stage-2 for this IPA, and the
	 * whole of Stage-1. Weep...
	 */
	ipa >>= 12;
	__tlbi_level(ipas2e1is, ipa, level);
	...
	dsb(ish);
	__tlbi(vmalle1is);
	dsb(ish);
```

（`arch/arm64/kvm/hyp/vhe/tlb.c:92-109`）

原因在后一段注释里：S1 和 S2 是合并缓存的，如果先清 stage-1，别的 CPU 可能用**旧 stage-2** 重新填出一条完整的 S1+S2 项。所以顺序必须是**先 S2、后 S1**，中间要 `dsb(ish)`。

代价：清一条 IPA 的 stage-2，等于**在域内广播把该 VMID 的整个 stage-1 清掉**。guest 热的 stage-1 表项会被 host 每一次 stage-2 单页失效连坐。

| 指令 | stage-2 | stage-1 | 位置 |
| --- | --- | --- | --- |
| `ipas2e1is` + `vmalle1is` | 该 IPA | **整个 VMID，广播** | `arch/arm64/kvm/hyp/vhe/tlb.c:98-107` |
| `vmalls12e1is` | 整个 VMID | **整个 VMID** | `:183` |
| `vmalle1`（无 `is`） | 不涉及 | **整个 VMID，仅本核** | `:197` |

**批量失效的退化**：`kvm_pgtable_stage2_unmap()` 只在 `system_supports_tlb_range() && stage2_has_fwb(pgt)` 时才延后成一次 `kvm_tlb_flush_vmid_range()`（`arch/arm64/kvm/hyp/pgtable.c:865-867`、`:1163-1166`）；否则**每个被清掉的叶子项各发一次** `__kvm_tlb_flush_vmid_ipa()`（`:878-881`）——每清一个叶子就广播一次整个 stage-1。dirty log 会把大页拆成 4K，这里就按 4K 计数。

## 2. 时间线：每一步刷不刷

| 阶段 | 刷吗 | 刷什么 | 依据 |
| --- | --- | --- | --- |
| 建 memslot（不带 dirty log） | 基本不刷 | 此时 stage-2 还没有表项 | `kvm_arch_commit_memory_region()` 只在 dirty log 分支动作（`arch/arm64/kvm/mmu.c:2114-2149`） |
| 建 memslot（带 `track_dirty_pages`） | **刷整块 RAM** | 整 memslot 写保护 + range 失效 | `kvm_mmu_wp_memory_region()`（`:1179-1198`）→ `kvm_flush_remote_tlbs_memslot()`（`:1194`） |
| 首次进入 guest | 只分配 VMID，通常不刷 | 位图满时**全局广播 `alle1is`** | `new_vmid()`（`arch/arm64/kvm/vmid.c:99-147`）、`flush_context()`（`:49-77`） |
| 同 vCPU 继续在同一物理核跑 | **不刷** | — | `last_ran` 命中（`arch/arm64/kvm/arm.c:1150-1153`） |
| vCPU 落到"上次跑的是别的 vCPU"的核 | **刷本地** | `vmalle1` + `ic iallu` | `:1153` → `arch/arm64/kvm/hyp/vhe/tlb.c:190-203` |
| guest 内部失效 | host 不参与 | guest 自己的 TLBI | `HCR_TTLB` 未设（见 §1） |
| balloon / `MADV_DONTNEED` / host 回收 guest RAM | **刷整个 VMID** | `vmalls12e1is` | mmu_notifier → `.flush_on_ret` → `kvm_flush_remote_tlbs()`（`virt/kvm/kvm_main.c:679-680`）→ `kvm_arch_flush_remote_tlbs()`（`arch/arm64/kvm/mmu.c:171-175`） |
| **pause** | **不刷** | 只清本 CPU 的 active_vmid | `kvm_arch_vcpu_put()` → `kvm_arm_vmid_clear_active()`（`arch/arm64/kvm/arm.c:1224`） |
| **resume（同核、无回卷）** | **不刷** | — | `kvm_arm_vmid_update()` 快路径直接返回（`arch/arm64/kvm/vmid.c:172-176`） |
| resume（pause 期间发生过回卷） | 不额外刷 | 换一个新 VMID | 回卷时已全局刷过（`arch/arm64/kvm/vmid.c:129-136`）；旧表项的清理由那次广播完成 |
| resume（落到新物理核） | **刷本地一次** | `vmalle1` + `ic iallu` | 该核 `last_vcpu_ran` 初值是 -1（`arch/arm64/kvm/mmu.c:943`） |
| 销毁 microVM | 基本不刷 | stage-2 页表直接释放 | `kvm_free_stage2_pgd()` 不失效 TLB（`arch/arm64/kvm/mmu.c` 内），靠 VMID 不复用兜底 |
| Firecracker 进程退出 | **刷 host mm 大范围** | guest RAM 全部 unmap | `exit_mmap` → `tlb_finish_mmu()`（`mm/mmu_gather.c:437`） |

## 3. "大量刷新"的四个真实来源（按影响面排序）

### 3.1 VMID 回卷：沙箱创建速率 = VMID 消耗速率

这是最容易被忽略、也最容易在 Firecracker 场景下变成常态的一条。

`vmid_map` 里的位**只在回卷时整体清零，VM 销毁不释放自己的 VMID 位**——`vmid.c` 里唯一的清位操作是 pinned 路径的 `__clear_bit()`（`arch/arm64/kvm/vmid.c:243`），其余全靠 `flush_context()` 的 `bitmap_zero()`（`arch/arm64/kvm/vmid.c:57`）。回卷触发条件只有一个：`find_next_zero_bit()` 走不到空位（`arch/arm64/kvm/vmid.c:129-136`）。

于是：

| VMID 宽度 | 来源 | 回卷周期 |
| --- | --- | --- |
| 8 位（255 个可用，VMID 0 保留） | `get_vmid_bits()` 默认值（`arch/arm64/include/asm/cpufeature.h:1027-1040`） | 每累计 255 个 VM |
| 16 位（65535 个） | `ID_AA64MMFR1_EL1.VMIDBITS == 16` | 每累计 65535 个 VM |

回卷的代价是**全系统广播**，与 ASID 那套"延迟到下次切换"完全不同（`arch/arm64/kvm/vmid.c:69-76` 的注释明确说明了这个设计差异）：

```c
	kvm_call_hyp(__kvm_flush_vm_context);   /* arch/arm64/kvm/vmid.c:76 */
```

```c
void __kvm_flush_vm_context(void)
{
	dsb(ish);
	__tlbi(alle1is);      /* arch/arm64/kvm/hyp/vhe/tlb.c:205-210，nvhe 同名 :196-203 */
	dsb(ish);
}
```

`alle1is` 是**所有 EL1 的 stage-1+stage-2** 全刷，且是 inner-shareable 广播。对 Firecracker 的意义：**一个沙箱一个 VM，销毁不还 VMID**，所以按 10 个沙箱/秒的创建速率，8 位 VMID 大约每 25 秒就要来一次全核 `alle1is`；16 位则约 1.8 小时一次。

两个附带的坑：

- `kvm_arm_vmid_alloc_init()` 里有 `WARN_ON(NUM_USER_VMIDS - 1 <= num_possible_cpus())`（`arch/arm64/kvm/vmid.c:261`）——**8 位 VMID 在 ≥255 核的机器上不成立**，回卷后可能分配失败。
- `flush_context()` 的注释说"broadcast TLB flush + I-cache invalidation"，但代码里只有 `alle1is`、没有 `ic` 指令。以代码为准，这条注释偏旧（观察，不是 bug）。

### 3.2 dirty log：一次性把整块 guest RAM 写保护

Firecracker 的 diff snapshot 在 `track_dirty_pages: true` 时会开 KVM dirty log。开了就立刻发生（`arch/arm64/kvm/mmu.c:2144`）：

```c
		kvm_mmu_wp_memory_region(kvm, new->id);      /* 整 memslot 写保护 */
		kvm_mmu_split_memory_region(kvm, new->id);   /* 大页拆成 4K */
```

`kvm_mmu_wp_memory_region()` 走完 `stage2_wp_range()` 后 `kvm_flush_remote_tlbs_memslot()`（`:1194`），也就是**整块 guest RAM 的 stage-2 失效**。范围失效还是整 VMID 失效，取决于两个条件（`arch/arm64/kvm/hyp/pgtable.c:675-685`）：

```c
	if (!system_supports_tlb_range() || pages > MAX_TLBI_RANGE_PAGES) {
		kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);    /* 退化：整个 VMID */
		return;
	}
```

- `MAX_TLBI_RANGE_PAGES` = `__TLBI_RANGE_PAGES(31, 3)` = `32 << 16` = 2097152 页 = **8 GiB**（4K 页，`arch/arm64/include/asm/tlbflush.h:151-153`）。**guest RAM 超过 8 GiB 就退化成整 VMID 刷**。
- 不支持 ARMv8.4-TLBIRANGE 的 CPU（例如 Neoverse N1 这类 v8.2 核，**按架构推断，未在目标硬件验证**）则**永远是整 VMID 刷**。
- 另外，`kvm_mmu_split_memory_region()` 会把 2M 大页拆成 4K，之后 stage-2 覆盖率变差——这是"翻页变多"而不是"刷新变多"，但排查时容易和刷新混淆。Firecracker 文档也写了 dirty tracking 会 negate 大部分 huge page 收益。

### 3.3 vCPU 跨物理核 / 多 vCPU 共享物理核

**触发条件**：`kvm_arch_vcpu_load()` 把 vCPU 装到物理核时，检查这块核上"上次跑的是哪个 vCPU"：

```c
	last_ran = this_cpu_ptr(mmu->last_vcpu_ran);       /* arch/arm64/kvm/arm.c:1140 */
	...
	if (*last_ran != vcpu->vcpu_id) {                  /* :1152 */
		kvm_call_hyp(__kvm_flush_cpu_context, mmu);    /* :1153 */
		*last_ran = vcpu->vcpu_id;
	}
```

`last_vcpu_ran` 是每个 VM 一份的 per-CPU `int`，创建时全部初始化成 -1（`arch/arm64/kvm/mmu.c:936-943`，声明见 `arch/arm64/include/asm/kvm_host.h:183`）。**每个物理核只记住一个 vCPU id**（最后一次跑的那个），不是集合或位图——所以命中条件只是"这次要装的和上次那个是同一个"。

**刷的是什么**：

```c
	__tlb_switch_to_guest(mmu, &cxt);   /* 把 VTTBR 切到该 VMID */
	__tlbi(vmalle1);                    /* 该 VMID 的全部 stage-1 项 */
	asm volatile("ic iallu");           /* 整个 I-cache */
	dsb(nsh); isb();                    /* arch/arm64/kvm/hyp/vhe/tlb.c:190-203 */
```

比 stage-2 单页失效重得多：一条 `vmalle1` 清掉该 VMID 的所有表项，外加 `ic iallu`。只在本地核、`nsh` 域内完成，**不发 IPI**。

**为什么必须刷**：KVM 自己的理由写在注释里（`arch/arm64/kvm/arm.c:1143-1151`）——"We guarantee that both TLBs and I-cache are private to each vcpu"。guest 的 ASID 分配器默认每个 CPU 的 TLB 是私有的，而一个物理核可能先后跑同一 VM 的多个 vCPU，TLB / I-cache 内容会串。注释同时承认"over-invalidation 可接受"：可能在 vCPU 真正进 guest 之前就被抢占，白刷也算。

**三种触发与量级**：

| 形态 | 刷新次数 | 说明 |
| --- | --- | --- |
| 1 vCPU 的 VM 迁移物理核 | ≤ 走过的 pCPU 数 | 每个核第一次必刷（初值 -1），之后 `last_ran` 记住该 vCPU 就不再刷 |
| 多 vCPU 共享同一物理核 | **每次 vCPU 切换 1 次** | 频率 = vCPU 上下文切换频率 |
| pause/resume 后落到新核 | 1 次/新核 | 绑核可完全消除 |

**推论：1 vCPU 的 VM 结构上免疫这一类刷。** 每个 VM 只有一个 vcpu_id（0），`last_vcpu_ran` 一旦写成 0 就永远命中，之后无论被迁到哪个核（每核首次除外）都不再刷。反过来说，2 个 vCPU 的 VM 会不会刷，取决于这两颗 vCPU 是否会在同一物理核上交替——默认不绑核时由调度器放置决定，不可控。这是"每核多台单 vCPU VM"与"每核一台多 vCPU VM"在 TLB 成本上的关键分歧。

### 3.3.1 一次切换的成本拆解

**共同项**（任何 vCPU 切换都要付）：`vcpu_put` / `vcpu_load` 的一组状态搬运——timer、vgic、FP、sysregs（VHE）、PMU、以及 `kvm_arm_vmid_clear_active()` 与 `kvm_arm_vmid_update()` 的 cmpxchg（`arch/arm64/kvm/arm.c:1135`、`:1208`、`:1224`、`:1762`）。另外，**每次进 guest 都会写 VTTBR**（`arch/arm64/kvm/hyp/vhe/switch.c:362` 的 `__load_stage2()`），所以"换 VM"本身不额外付 VTTBR 的钱。

**独有项**（仅 `last_ran` 不匹配时）：`__kvm_flush_cpu_context()`（`arch/arm64/kvm/hyp/vhe/tlb.c:190-203`）里的一整段 VHE 序列：

```c
	local_irq_save(...);                          /* :24 */
	__load_stage2(mmu, mmu->arch);                /* :56  写 VTTBR + isb */
	val &= ~HCR_TGE;                              /* :58  让 TLBI 命中 guest */
	...
	__tlbi(vmalle1);                              /* 当前 VMID 的全部表项 */
	asm volatile("ic iallu");                     /* 整个 I-cache 到 PoU */
	dsb(nsh); isb();
	write_sysreg(0, vttbr_el2);                   /* :69 */
	write_sysreg(HCR_HOST_VHE_FLAGS, hcr_el2);    /* :70 */
	local_irq_restore(...);                       /* :79 */
```

三条要点：

- **换 VM 不额外付钱**：判定用的是每个 VM 自己的 `last_vcpu_ran`，且 VTTBR 每次进 guest 都写。所以"不同 VM 的 vCPU 交替"在 TLB 上成本为零。
- **`vmalle1` 与 `ic iallu` 都是本地的**（非 `is` 变体），只损失本核；兄弟 vCPU 在其它核上的热状态不受影响。
- **`dsb(nsh)` 是等待**：要等 inner-shareable 域内此前操作完成，多核繁忙时是真实延迟。

**成本模型**：

```
overhead(核) = f × p × (C_fixed + C_refill)
    f = 每核 vCPU 切换率
    p = 切换中 last_ran 不匹配的比例
    C_fixed = 上面的固定序列（4 次 sysreg 写 + 3~4 个 ISB + 1 次 TLBI + 1 次 IC + dsb）
    C_refill = TLB 重填（stage-1 + stage-2 walk）× miss 数 + I-cache 填充
```

`f` 的量级：EEVDF 基片 `sysctl_sched_base_slice` = 0.7 ms（`kernel/sched/fair.c:100`），两个 50% 的 vCPU 占满一核时 f ≈ 1.4×10³ 次/秒。

| `C_fixed + C_refill` | 占一核比例（f=1.4×10³/s，p=1） |
| --- | --- |
| 5 µs | 0.7% |
| 20 µs | 2.8% |
| 50 µs | 7% |
| 100 µs | 14% |

**C 没有实测**，上表只是敏感度；这部分开销会表现为**guest 变慢**（TLB / I-cache 冷启动后的重填），而不是 host 的 sys time，所以在 host 上 `perf stat` 看不出来，得数 PMU 事件或直接比吞吐。

多 vCPU 抢核这条最容易爆炸，而 Firecracker README 明确写了 **CPU oversubscription 默认开启**。附带影响：`ic iallu` 清整个 I-cache，对解释器/JIT 类负载（serverless 里常见）可能比 TLB 更疼——**这一条是推断，未实测**。

### 3.4 宿主内存压力与沙箱销毁

guest RAM 是普通用户页：新建的 VM 是匿名内存，从快照恢复的是 memfile 的 `MAP_PRIVATE` + 写时 CoW。所以它同样会被宿主回收/换出。

**回收一个 folio 时的完整链路**（这里有个容易低估的地方）：

1. `try_to_unmap_one()` 先调 `mmu_notifier_invalidate_range_start(&range)`（`mm/rmap.c:1682`）。
2. KVM 的 notifier → `kvm_mmu_unmap_gfn_range()`（`virt/kvm/kvm_main.c:812-816`）→ arm64 `kvm_unmap_gfn_range()`（`arch/arm64/kvm/mmu.c:1912`）拆掉对应 GFN 的 stage-2。
3. 因为 `.flush_on_ret = true`，返回后执行 `kvm_flush_remote_tlbs()`（`virt/kvm/kvm_main.c:679-680`）——**整个 VMID 的 `vmalls12e1is`**，不是按 range 失效（`arch/arm64/kvm/mmu.c:171-175`）。
4. 同时 host PTE 被清：`set_tlb_ubc_flush_pending()`（`mm/rmap.c:658`）走 arm64 的批量机制。

**关键点**：notifier 是**按 folio 触发**的，所以"回收多少页 ≈ 多少次整 VMID 刷"。这与 dirty log 那种一次性 range 失效是完全不同的形态。

**host 侧"批量"省的是 dsb，不是 TLBI**：

```c
static inline void arch_tlbbatch_add_pending(...)      /* arch/arm64/include/asm/tlbflush.h:767-771 */
{
	__flush_tlb_range_nosync(mm, start, end, PAGE_SIZE, true, 3);   /* 立即发 TLBI */
}
static inline void arch_tlbbatch_flush(...)            /* :370-373 */
{
	dsb(ish);                                          /* 只补一个 dsb */
}
```

还有个坑：`arch_flush_tlb_batched_pending()` 直接退化成 `flush_tlb_mm(mm)`（`arch/arm64/include/asm/tlbflush.h:353-356`）——批量刷新期间碰上 mprotect/munmap 就是整 mm 刷。Firecracker 的 balloon（对 guest RAM 做 madvise）正好落在这类路径上。

**换回来**：缺页重建 host PTE + guest 重新 fault 让 KVM 重建 stage-2。不是 flush，但同样是成本。

**沙箱销毁 / 进程退出**：

```c
void exit_mmap(struct mm_struct *mm)                 /* mm/mmap.c:3433 */
{
	mmu_notifier_release(mm);                       /* :3442 → KVM kvm_flush_shadow_all() */
	...
	tlb_gather_mmu_fullmm(&tlb, mm);                /* :3456 */
	unmap_vmas(...);
	tlb_finish_mmu(&tlb);                           /* :3472 */
```

- `mmu_notifier_release()` → `kvm_flush_shadow_all()`（`virt/kvm/kvm_main.c:969-979`）→ arm64 `kvm_uninit_stage2_mmu()` → `kvm_free_stage2_pgd()`：**释放页表但不发 TLB 失效**，靠 VMID 不复用兜底。
- 随后 `unmap_vmas()` 把整个 guest RAM 从 host 页表摘掉，`tlb_finish_mmu()` 统一失效。成本 ∝ RAM 大小；单段超过 8 GiB 会退化成整 mm / 全刷。
- Firecracker 一沙箱一进程，所以这是**每个沙箱都要付一次**的固定成本。

**为什么 ARM 上这一步更值得注意**：同一页被回收时，host mm 的 TLB 和 KVM 的 stage-2 要各失效一次，两套语义叠加；而且 stage-2 侧是整 VMID，粒度比实际回收范围粗得多。

**缓解**（按性价比）：

1. 关 swap、不让宿主回收 guest RAM（cgroup v2 `memory.min`，或干脆不超卖）。
2. 关掉或调小 balloon，避免在 guest RAM 上做 madvise。
3. 控制 guest RAM 大小与沙箱存活时间。
4. Firecracker 是否提供 mlock guest RAM 的选项，**未核实**。

## 4. pause / resume 专项结论

把上面串起来，pause/resume 的 TLB 行为可以精确表述为：

1. **pause 零 TLB**。`kvm_arch_vcpu_put()` 只做 `kvm_arm_vmid_clear_active()`（`arch/arm64/kvm/arm.c:1224`），把本 CPU 的 `active_vmids` 置为 `VMID_ACTIVE_INVALID`。副作用是**这个 VMID 不再被视为"保留"**，回卷时可以优先把它分给别人——这也是为什么旧 VM 的表项最终一定要靠回卷时那次全局 `alle1is` 清掉。
2. **resume 通常也零 TLB**。`kvm_arm_vmid_update()` 的 cmpxchg 快路径直接返回（`arch/arm64/kvm/vmid.c:172-176`）；VMID 没变、物理核没变，TLB 里的 stage-2 表项依然有效，可以直接接着用。
3. **resume 会刷的三种情况**：
   - pause 期间发生过 VMID 回卷 → 该 VM 需要换 VMID（`arch/arm64/kvm/vmid.c:155-187`）。回卷那一刻已经广播刷过，resume 本身不再额外刷。
   - vCPU 落到"上次跑的是别的 vCPU"的核 → 本地 `vmalle1` + `ic iallu`（`arch/arm64/kvm/arm.c:1153`）。
   - resume 前后 memslot 变了（内存热插拔、boot 时补 slot）→ 该 slot 范围失效（`kvm_arch_flush_shadow_memslot()`，`arch/arm64/kvm/mmu.c:2231-2240`）。

**实践含义**：如果负载是"频繁 pause 再 resume 跑下一批任务"，而 vCPU 没有绑核，那么每次 resume 都可能付一次本地全刷；绑核能直接消掉这一类。

## 5. 怎么观测

| 想看的 | 手段 | 说明 |
| --- | --- | --- |
| host mm 侧的 unmap 失效 | `tlb_flush` tracepoint | `reason=remote shootdown`（`include/trace/events/tlb.h:12-16`，事件定义 `:38`） |
| stage-2 失效 | **本树没有 tracepoint** | `arch/arm64/kvm/trace.h` 只 include 了 `trace_arm.h` / `trace_handle_exit.h`，没有 TLB 事件；需用 kprobe 挂 `kvm_arch_flush_remote_tlbs`、`kvm_arch_flush_remote_tlbs_range`（`arch/arm64/kvm/mmu.c:171-184`） |
| VMID 回卷次数 | kprobe `new_vmid`；或用 VM 创建数推 | 回卷每 `NUM_USER_VMIDS` 个 VM 一次（8 位 = 256） |
| `kvm_stat` | **别指望** | arm64 的 `kvm_arch_flush_remote_tlbs()` 返回 0，不走 `KVM_REQ_TLB_FLUSH` 的 kick，`remote_tlb_flush` 计数不会体现 |
| VMID 位宽 | 读 `ID_AA64MMFR1_EL1.VMIDBITS` | `get_vmid_bits()`（`arch/arm64/include/asm/cpufeature.h:1027-1040`）；8 位默认值 |

## 6. 优化建议

1. **绑核**（cpuset / `--cpuset` 之外用 cgroup 或 `taskset`）：直接消掉 §3.3 的 `vmalle1 + ic iallu`。多 vCPU VM 尤其要保证 vCPU 数与物理核数对齐，避免同核轮转。
2. **非必要不开 `track_dirty_pages`**：它会让整块 RAM 写保护、把大页拆成 4K，并且这份开销是**常驻**的，不是只在 snapshot 那一刻。
3. **guest RAM 控制在 8 GiB 以内**（4K 页）：让 memslot 失效能走 range 指令而不是整 VMID；前提是硬件支持 TLBIRANGE。
4. **降低沙箱 churn 或复用 VM**：VMID 不随 VM 销毁归还，创建速率就是回卷速率。沙箱池化（预热复用）比"每任务新建"少一个数量级的 VMID 消耗。
5. **关 swap / 避免宿主机回收 guest RAM**：避免 host TLB 与 stage-2 同时被打。
6. **快照恢复注意**：从 snapshot load 时若带 `track_dirty_pages`，load 阶段就会做一次整块写保护 + 失效。

## 7. 未验证项

- **Firecracker 源码级细节未核对**：本机没有 Firecracker 源码。pause 的"vCPU 线程 park"机制、`PATCH /vm` 到内核 syscall 的具体序列，依据的是官方文档（`docs/snapshotting/snapshot-support.md`、README），不是代码。
- **aarch64 上 pause 的可用性**：官方 snapshot 文档说 snapshot 支持 README 中列出的所有 CPU 架构（含 Graviton），且 pause 是其前置条件；README 里 `[x86_64 only]` 标注的是 **Stop（关机）**，不是 pause。两者不要混。
- **目标硬件是否支持 TLBIRANGE / 16 位 VMID 未核实**：Graviton2 对应 Neoverse N1（ARMv8.2），按架构推断不支持 TLBIRANGE，但未在硬件上验证；16 位 VMID 属 ARMv8.1-VMID16，同样未验证。
- **Firecracker 是否提供 mlock guest RAM 的选项未核实**；宿主机侧可行的替代是 cgroup v2 的 `memory.min`。
- **§3.4 中"每个被回收的 folio 触发一次整 VMID 刷"是从 notifier 的调用粒度推出来的**，未用 kprobe 实测确认调用次数。
- **`ic iallu` 相对 TLB 刷的实际开销未测量**，文中"对 JIT 负载更疼"属推断。
- **本文所有"大量刷新造成多少开销"的量级判断都来自代码路径，没有实测数据**。
- guest 内部（stage-1）的失效量取决于 guest 内核与 workload，本文不涉及。

---

*基线：`8003dfcde32f` / kernel 6.6.0 / openEuler OLK-6.6；Firecracker main 分支官方文档（2026-11 读取）。未在 ARM 硬件实测。*
