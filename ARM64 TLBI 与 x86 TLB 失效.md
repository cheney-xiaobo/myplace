---
title: ARM64 TLBI 与 x86 TLB 失效
aliases:
  - TLBI
  - TLB 失效
  - tlbi
  - tlbflush
tags:
  - linux/kernel
  - linux/mm
  - arch/arm64
  - arch/x86
source: /Users/cheney/work/code/ub/kernel @ 8003dfcde32f (openEuler OLK-6.6 / kernel 6.6.0)
created: 2026-11-01
updated: 2026-11-01
status: 已验证（静态阅读 + git 历史，未在硬件上实测）
---

# ARM64 TLBI 与 x86 TLB 失效

四个问题：TLBI 是什么、x86 为什么没有对应机制、一般怎么优化、社区是怎么一步步引进的。

## 1. TLBI 是什么

TLBI = **TLB Invalidate**，ARMv8-A 里一组通过系统指令发起的 TLB 维护操作。它和 x86 的 `invlpg` 属于同一类东西（让翻译缓存里的旧表项失效），但粒度设计和广播模型完全不同。

**操作数编码**（`arch/arm64/include/asm/tlbflush.h:59-65`）：

```c
#define __TLBI_VADDR(addr, asid)				\
	({							\
		unsigned long __ta = (addr) >> 12;		\
		__ta &= GENMASK_ULL(43, 0);			\
		__ta |= (unsigned long)(asid) << 48;		\
		__ta;						\
	})
```

也就是 VA 放低 44 位、ASID 放高 16 位（`[63:48]`）。这决定了 TLBI 的粒度天然有四种：

| 粒度 | 典型指令 | 说明 |
| --- | --- | --- |
| 全部 | `vmalle1` / `vmalle1is` | 整个 TLB |
| 按 ASID | `aside1` / `aside1is` | 一个地址空间的全部表项 |
| 按 VA + ASID | `vae1` / `vale1` / `vae1is` | 单个页面 |
| **按区间** | `vae1is` + NUM/SCALE | ARMv8.4-TLBIRANGE，一次失效一大段 |

关键概念：

- **后缀 `is` = inner shareable，即“广播”**。`vmalle1` 只影响本核，`vmalle1is` 会打到同一 inner-shareable domain 里的所有 PE。这是 ARM 与 x86 最大的结构差异：**ARM 的失效可以由硬件广播，x86 必须靠 IPI 软件通知**。
- **屏障配对**：发之前 `dsb(ishst)`，发之后 `dsb(ish)` + `isb()`（`arch/arm64/include/asm/tlbflush.h:301-307` 的 `flush_tlb_all()` 就是最小样例）。
- **TTL 层级提示**（ARMv8.4-TTL）：操作数 `[47:44]` 可以告诉硬件“这个失效发生在第几级页表”，带 hint 时可以少走 walk cache。掩码在 `:104`，编码逻辑在 `__tlbi_level()`（`:106-118`），TTL 为 0 表示不带提示。
- **区间编码**（ARMv8.4-TLBIRANGE）：`__TLBI_VADDR_RANGE()`（`:138-148`）把 ASID/TG/SCALE/NUM/TTL/BADDR 打进一个操作数，覆盖范围是
  `[BADDR, BADDR + (NUM+1) × 2^(5×SCALE+1) × PAGE_SIZE)`（注释 `:129-136`），单条指令最多覆盖 `MAX_TLBI_RANGE_PAGES = (31+1) × 2^16` 页（`:151-153`）。

**新增的 TLBID / TLBIP**（本树有，见 §4）：TLBID 让失效**定向到一个 domain**而不是全广播；TLBIP 是 128 位形式，把 VA 放在高寄存器、ASID/TTL/TLBID 放在低寄存器。

## 2. 为什么 x86 不用这个

不是“不用”，是**指令集里根本没有对应粒度**。三条硬约束：

**（1）没有区间失效，只能逐页**

x86 的单页失效是 `invlpg`，一次一个地址（`arch/x86/mm/tlb.c:1149`）。要失效一段就得循环：

```c
/* arch/x86/mm/tlb.c:1061-1063 */
	/* flush range by one by one 'invlpg' */
	for (addr = f->start; addr < f->end; addr += PAGE_SIZE)
		flush_tlb_one_kernel(addr);
```

**（2）INVPCID 只有 4 种粒度，没有 range，也没有 domain**

```c
/* arch/x86/include/asm/invpcid.h:24-27 */
#define INVPCID_TYPE_INDIV_ADDR		0
#define INVPCID_TYPE_SINGLE_CTXT	1
#define INVPCID_TYPE_ALL_INCL_GLOBAL	2
#define INVPCID_TYPE_ALL_NON_GLOBAL	3
```

单地址 / 单 PCID / 全部含 global / 全部非 global —— 到此为止。所以 x86 面对“失效 1000 个连续页”只有两个选择：循环 `invlpg`，或者整 TLB 刷。内核用阈值在两者之间切（`tlb_single_page_flush_ceiling`，默认 33 页，`arch/x86/mm/tlb.c:957`、判断在 `:1012-1013`）。

**（3）没有广播失效，必须 IPI**

ARM 一条 `*is` 指令就能让所有核失效；x86 必须由发起核逐个通知（`native_flush_tlb_multi()`，`arch/x86/mm/tlb.c:909`），接收核在自己的上下文里执行失效。**这也是 x86 侧 TLB 优化的主战场：想办法少发 IPI。**

**（4）ASID 的对应物是 PCID，但解决的问题不同**

PCID 让“切地址空间”不必整 TLB 刷（类似 ASID），但它是**按上下文标号**，不提供“定向广播到某组核”的能力。ARM 的 TLBID 则是后者——把“发给谁”这件事也交给硬件。

## 3. 一般怎么优化

### arm64 侧（本树代码里能看到的手段）

| 手段 | 做法 | 位置 |
| --- | --- | --- |
| 区间合并 | 把 N 次单页失效压成一条 range TLBI，按 SCALE/NUM 分段 | `__flush_tlb_range_op()` 循环 `arch/arm64/include/asm/tlbflush.h:427-437` |
| 层级提示 | 能确定页表层级时带 TTL，跳过无用 walk | `__tlbi_level()` `:106-118` |
| 只本地就非共享 | mm 只在本核活动时用 `nsh` 屏障 + 无 `is` 指令 | `flush_tlb_begin()` `:188-205` |
| 定向 domain | 用 `mm_cpumask` 选出覆盖它的最小 domain，只发给这些核 | `flush_tlb_mm()` `:309-332`、`pick_best_domain()` `arch/arm64/mm/tlbid.c:217-243` |
| 128 位 TLBIP | VA 与（ASID/TTL/TLBID）打包成一条 | `flush_tlb_page()` 路径 |
| 批处理延迟 | 大量单页失效先攒着，最后合并 | `arch_tlbbatch_should_defer()` `:334` 起 |
| 省屏障 | 拆分函数省掉不需要的尾部 DSB | `__flush_tlb_range` 系列 |
| 避免 ASID 回卷 | ASID rollover 时做正确的失效 | `arch/arm64/mm/context.c` 相关 |

一个细节：`flush_tlb_begin()` 的选择逻辑本身就是优化——**先看这个 mm 有没有在别的核上跑过**，没有就直接走非共享路径（`:196-203`），省掉广播；有才考虑 domain 或广播。

### x86 侧

- **能少刷就少刷**：范围小于阈值就逐个 `invlpg`，否则整 TLB（`:1011-1016`）。
- **PCID**：切 mm 时不刷 TLB，靠 context id 区分（`invpcid_flush_single_context()`）。
- **IPI 合并**：一次失效通知尽量覆盖多核、多次失效合并成一次 shootdown。
- **不失效就别失效**：`freed_tables` 之类标志让某些场景跳过。

两边的哲学差异：**ARM 把“范围”和“发给谁”做进了指令；x86 只有“单页/整个”和“软件通知”，所以优化空间更多落在软件调度上。**

## 4. 本树的 TLBID / TLBIP 实现

这已经不是纯主线代码了，是 openEuler 带进来的特性。

```c
/* arch/arm64/Kconfig:2306-2314 */
config ARM64_TLBID
	bool "Enable support for TLBID (TLBI Domains)"
	default y
	depends on AS_HAS_TLBIP
	help
	  TLBI broadcasting all PEs introduces performance noise. By
	  combining hardware and software, TLBID (TLBI Domain) limit TLBI
	  to an appropriate scope, avoiding the performance overhead caused
	  by broadcasting.

	  Arm Architecture defines the FEAT_TLBID extension since Armv9.3,
```

要点：

- **FEAT_TLBID 从 Armv9.3 开始**，domain 可以嵌套（一个 domain 可以属于多个父 domain）。
- **拓扑来自固件**：ACPI 新增 TLBI 表，列出 PE 与 SMMU 各属于哪些 domain（`include/acpi/actbl2.h:368-369` 的 `ACPI_TLBI_TYPE_PROCESSOR` / `_SMMU`，驱动在 `drivers/acpi/arm64/tlbid.c`）。
- **运行时三重门禁**：`CONFIG_ARM64_TLBID` + `SYSINSTR128` + `ARM64_HAS_TLBID`（`system_supports_tlbid()`，`arch/arm64/include/asm/cpufeature.h:937-942`）。缺任何一项就退回广播。
- **domain 选择**：`pick_best_domain()` 从 `cpu_domain[nr_cpu-1][...]` 开始，向上找第一个**完全覆盖** `mm_cpumask` 的 domain（`arch/arm64/mm/tlbid.c:217-243`）；找不到就返回 domain 0（即退化为广播）。注意它要求 `cpumask_equal(&tmp, active_cpus)`，是**精确覆盖**而不是"包含"。
- **失效路径改造**：`flush_tlb_mm()` 三选一（LOCAL → `aside1`，BROADCAST → `aside1is`，DOMAIN → `pick_best_domain()` + `__TLBI_DOMAIN(ASID, domain)` 再 `aside1is`），`arch/arm64/include/asm/tlbflush.h:309-332`。
- **KVM 侧还有 vTLBID**：给 guest 虚拟出 domain（`arch/arm64/include/asm/kvm_host.h:227-256`、`arch/arm64/kvm/arm.c:327`），并有 `KVM_REQ_RELOAD_VTLBID`。
- **有 KUnit 测试**：`arch/arm64/mm/test_tlbid.c`。

本树 arm64 defconfig 已打开：`CONFIG_ARM64_TLBID=y`（`arch/arm64/configs/openeuler_defconfig:577`）。

## 5. 社区引进的时间线

用 `git log` / `git describe --contains` 在本树查到的记录（日期为提交日期）：

| 进入版本 | 提交 | 作者 | 内容 |
| --- | --- | --- | --- |
| **v5.9-rc1** | `e735b98a5fe0`、`c4ab2cbc1d87` | Zhenyu Ye | ARMv8.4-TTL：`tlbi_user_level()` 助手、`flush_tlb_range` 带 TTL 字段 |
| **v5.9-rc1** | `d1d3aa98b1d4` | Zhenyu Ye | 用上 **TLBI RANGE**（ARMv8.4-TLBIRANGE），一次失效一整段 |
| v6.6 | `43b3dfdd0455` | Barry Song | 页回收/迁移时**批量延迟** TLB shootdown |
| v6.6 | `6bbd42e2df8f` | Alistair Popple | 失效 TLB 时调用 `mmu_notifier` 的 `invalidate_range()` |
| 本树 6.6.0-16 | `a630a671456d` | Ryan Roberts | 拆分 `__flush_tlb_range()` 省掉尾部 DSB |
| 本树 6.6.0-24 | `b4fa372c60f1` | Gavin Shan | 修正 TLBI RANGE 操作数 |
| 本树 6.6.0-61 | `1fe29f6d875f` | Gavin Shan | `MAX_TLBI_RANGE_PAGES` 也允许区间操作 |
| — | `8fa5625eaa5e` | Ryan Roberts | range-based tlbi 改为**递减 scale** |
| — | `96e4d6a563a7` | Oliver Upton | `MAX_TLBI_OPS` 改名 `MAX_TLBI_RANGE_PAGES` |
| — | `378959bdc9be`（2026-06） | Mark Rutland | 允许 XZR 作为 TLBI 参数 |
| **仅 openEuler 6.6.0-179+** | `5468798756f2`、`e28c4375a213`、`a43893f89baa` | Jinjiang Tu、Zeng Heng | **TLBID / TLBIP**：`flush_tlb_mm()`、`flush_tlb_range()`、`flush_tlb_page()` 三级优化 |

读法：

1. **2020 年那两条是真正的分水岭**——ARMv8.4 的 TTL 与 RANGE 让 ARM64 从“逐页失效”升级到“区间 + 层级提示”，这是后续所有优化的基础。
2. **2023 年的 `43b3dfdd0455` 与 mm 直接相关**：回收/迁移触发的 TLB 失效可以攒批延迟，避免每次 unmap 都广播一次。
3. **2024–2025 年的一堆提交都是打磨**：操作数修正、scale 递减、上限重命名、省 DSB——说明这条路径是“用起来之后才发现细节问题”的典型。
4. **TLBID/TLBIP 三个提交标着 `hulk inclusion`**，`git describe --contains` 只到 openEuler `6.6.0-179.0.0`，**不在上游 v6.6 里**。提交信息里还带了 `bugzilla: atomgit.com/openeuler/kernel/issues/9795`，是标准的 OLK 回合格式。

## 6. 进程上下文切换要刷 TLB 吗

**结论：正常情况下不刷。** 现代 x86/arm64 换进程只是换“命名空间”（PCID / ASID），不是刷 TLB。真正产生大量刷新的地方是**地址空间被改动**（unmap / mprotect / 回收 / 迁移），不是调度本身。

### 6.1 什么情况下刷，什么情况下不刷

| 场景 | 是否刷 TLB | 依据 |
| --- | --- | --- |
| 同一进程的两个线程切换 | **不刷** | x86：`real_prev == next` 且非 lazy 直接 return（`arch/x86/mm/tlb.c:563`、`:585-587`）；arm64：`prev == next` 时不调 `__switch_mm()`（`arch/arm64/include/asm/mmu_context.h:261-265`） |
| 换进程，x86 且该 mm 已占本 CPU 的 PCID 槽 | **不刷** | `choose_new_asid()` 命中槽且 `tlb_gen` 未过期 → `need_flush = false`（`arch/x86/mm/tlb.c:234-242`）→ `build_cr3_noflush()`（`:173-183`） |
| 换进程，x86 但槽位被轮转复用 | 刷**该 PCID** | `next_asid` 轮转 + `need_flush = true`（`:246-253`）→ 写 CR3 不带 NOFLUSH（`build_cr3()` `:159-171`） |
| 换进程，x86 无 PCID | **整个 TLB** | `!static_cpu_has(X86_FEATURE_PCID)` → `*new_asid = 0; *need_flush = true`（`:224-228`） |
| 换进程，arm64 | **不刷** | 新 mm 用自己的 ASID，写 TTBR0_EL1 即切换命名空间（`cpu_switch_mm()` `arch/arm64/include/asm/mmu_context.h:57-60`） |
| arm64 ASID 回卷 | **整个 TLB**（延迟到该 CPU 下次切换） | `flush_context()` 里 `cpumask_setall(&tlb_flush_pending)`（`arch/arm64/mm/context.c:131`），`check_and_switch_context()` 里 `local_flush_tlb_all()`（`:257-258`） |
| 切换到内核线程 | 不刷用户 TLB | arm64 用保留 TTBR0（`arch/arm64/include/asm/mmu_context.h:252-255`）；x86 `leave_mm()` 切到 ASID 0（`arch/x86/mm/tlb.c:721-722`） |

### 6.2 x86：每 CPU 只有 6 个 PCID 槽

```c
/* arch/x86/include/asm/tlbflush.h:65 */
#define TLB_NR_DYN_ASIDS	6
```

`choose_new_asid()`（`arch/x86/mm/tlb.c:220-255`）只有三种结局：

```c
	if (!static_cpu_has(X86_FEATURE_PCID)) {      /* :224 */
		*new_asid = 0;
		*need_flush = true;
		return;
	}
	...
		*need_flush = (this_cpu_read(cpu_tlbstate.ctxs[asid].tlb_gen) <
			       next_tlb_gen);              /* :240 */
	...
	*new_asid = this_cpu_add_return(cpu_tlbstate.next_asid, 1) - 1;  /* :246 */
	*need_flush = true;                                              /* :254 */
```

关键在 `need_flush` 的语义（`load_new_mm_cr3()` `:283-300`）：

```c
	if (need_flush) {
		invalidate_user_asid(new_asid);
		new_mm_cr3 = build_cr3(pgdir, new_asid, lam);          /* 不带 NOFLUSH */
	} else {
		new_mm_cr3 = build_cr3_noflush(pgdir, new_asid, lam);  /* | CR3_NOFLUSH */
	}
	write_cr3(new_mm_cr3);
```

- `need_flush = true` → 写 CR3 **不带** `CR3_NOFLUSH` 位，硬件失效 CR3 里那个 PCID 的表项（按 CR3 语义 global 项不受影响）。所以这是 **per-ASID 代价**，不需要遍历页表、也不发 IPI。
- `need_flush = false` → 带 `CR3_NOFLUSH`，一条 CR3 写入完成切换，**零失效**。
- 因此“换进程要刷 TLB”在 x86 上只对两种情况成立：**没有 PCID 的老 CPU**，或**每 CPU 活跃 mm 超过 6 个**（槽被轮转复用）。
- tracepoint `trace_tlb_flush(TLB_FLUSH_ON_TASK_SWITCH, ...)`（`:645`、`:650`）的 `pages` 字段**不是页数**：`need_flush=true` 分支传 `TLB_FLUSH_ALL`，`false` 分支传 `0`。所以它能区分“这次切换刷没刷”，但给不出刷了多少页。

### 6.3 arm64：ASID 位分配出去就不回收

`switch_mm()`（`arch/arm64/include/asm/mmu_context.h:261-274`）只在 `prev != next` 时调 `__switch_mm()`（`:264-265`），后者对普通 mm 走 `check_and_switch_context()`（`arch/arm64/mm/context.c:218`）。快路径是一次 cmpxchg：

```c
	old_active_asid = atomic64_read(this_cpu_ptr(&active_asids));   /* :243 */
	if (old_active_asid && asid_gen_match(asid) &&
	    atomic64_cmpxchg_relaxed(this_cpu_ptr(&active_asids),
				     old_active_asid, asid))
		goto switch_mm_fastpath;                                 /* :247 */
```

只有 `!asid_gen_match(asid)` 才需要 `new_context()` 重新分配 ASID；`tlb_flush_pending` 置位时才 `local_flush_tlb_all()`（`:257-258`）。

**这里有个容易忽略的点**：`asid_map` 里的 bit 只在 `flush_context()` 里被整体重置（`set_reserved_asid_bits()` `:90-98`），**mm 退出时不释放自己的 ASID 位**（`context.c` 里只有 `pinned_asid_map` 有 `__clear_bit()`，`:335`）。所以回卷不是由“同时存活的 mm 数”决定，而是由**这一代累计分配过 ASID 的 mm 数**决定：

| ASID 宽度 | 来源 | 回卷周期 |
| --- | --- | --- |
| 16 位（`NUM_USER_ASIDS` = 65536，`:37`） | `ID_AA64MMFR0_EL1.ASIDBITS == 16`（`:42-62`） | 每累计 65536 个新 mm |
| 8 位（256） | 固件报 8 位，或字段未知时“假设 8 位”（`:44-47` 的 `pr_warn`） | **每 256 个新 mm** |

还有两个会进一步缩短周期的因素：

- **KPTI**（内核页表隔离）把 ASID 按奇偶成对分配，偶数位整体保留给内核侧，**可用数量只剩一半**：`set_kpti_asid_bits()` 用 `memset(map, 0xaa, len)` 把偶数位标为 reserved（`arch/arm64/mm/context.c:79-89`，调用点 `:96`、`:424`）。16 位 → 32768，8 位 → **128**。
- **KVM 固定 ASID**（`pinned_asid_map`）再占一部分：`max_pinned_asids = num_available_asids - num_possible_cpus() - 2`（`:401`），余量不够时 `arm64_mm_context_get()` 直接返回 0（`:292-296`）。

回卷的代价是系统级的：`flush_context()` 置 `tlb_flush_pending = all`（`:131`），**每个 CPU 在下一次上下文切换时**做一次 `local_flush_tlb_all()`（`:257-258`，实现见 `arch/arm64/include/asm/tlbflush.h:293-299`：`dsb(nshst)` + `vmalle1` + `dsb(nsh)` + `isb`）。

这解释了容器/构建机上常见的现象：**大量 fork/exec 或短命 mm 会把 ASID 快速耗尽，从而周期性触发全系统 TLB 刷新**——刷新不在 fork 那一刻发生，而是摊到各 CPU 的下一次切换上。

### 6.4 大量 TLB 刷新真正来自哪里

按影响面排序：

**1) 地址空间拆除/改动**（最常见）

`munmap()`、`exit_mmap()`、`MADV_DONTNEED`、`mprotect()`、`mremap()`、`MAP_FIXED` 覆盖，都走 zap 路径：

- `mm/memory.c:1959`、`:1981` → `tlb_finish_mmu()`
- `mm/mmu_gather.c:437` → `tlb_flush_mmu()`（`:370`）

超过阈值就放弃区间、改成整 mm 刷：

```c
/* arch/x86/mm/tlb.c:1011-1015 */
	if ((end == TLB_FLUSH_ALL) ||
	    ((end - start) >> stride_shift) > tlb_single_page_flush_ceiling) {
		start = 0;
		end = TLB_FLUSH_ALL;
	}
```

`tlb_single_page_flush_ceiling = 33`（`arch/x86/mm/tlb.c:957`，注释按“每次约 100ns”估的）。arm64 对应的是 `__flush_tlb_range_limit_excess()`（`arch/arm64/include/asm/tlbflush.h:588-602`），上限是 `MAX_TLBI_RANGE_PAGES`。

**2) 页回收 / 迁移 / compaction / KSM / THP**

这类场景一次要 unmap 成百上千个 PTE，走批量延迟：

- 攒批：`arch_tlbbatch_add_pending()`（`mm/rmap.c:658`）
- 提前刷的阈值：`TLB_FLUSH_BATCH_PENDING_LARGE`（`mm/rmap.c:645`，判断在 `:668`）
- 收尾：`arch_tlbbatch_flush()`（`mm/rmap.c:624`）
- 触发点：`mm/vmscan.c:1369`（`try_to_unmap_flush_dirty`）、`:1485`、`:1552`；`mm/migrate.c:1878`；`mm/khugepaged.c:2028`；`mm/huge_memory.c:3254`

**3) 内核地址空间**

`vmalloc`/`vfree`/`ioremap`/模块加载/`percpu` 扩展都要 `flush_tlb_kernel_range()`：`mm/vmalloc.c:506`、`:2322`、`:2425`、`:2874`、`:2939`；`mm/percpu-vm.c:188`；`mm/hugetlb_vmemmap.c:68`。x86 侧实现（`arch/x86/mm/tlb.c:1066-1078`）：

```c
	if (end == TLB_FLUSH_ALL ||
	    (end - start) > tlb_single_page_flush_ceiling << PAGE_SHIFT)
		on_each_cpu(do_flush_tlb_all, NULL, 1);
	else
		on_each_cpu(do_kernel_range_flush, info, 1);
```

超过 33 页就 `on_each_cpu(do_flush_tlb_all)`——**整 TLB 刷 + 全核 IPI**。所以频繁的大 `vmalloc`/`vfree`、模块热加载、大 `percpu` 分配，是内核侧刷新的主要来源。

**4) 结构性因素**

- x86 每 CPU 6 个 PCID 槽（`arch/x86/include/asm/tlbflush.h:65`）：一个 CPU 上轮转的进程越多，槽位复用越频繁
- arm64 ASID 回卷（见 6.3），8 位 ASID 时尤其敏感
- KVM stage-2 / EPT 失效：guest 页表改动也会触发嵌套失效（本树路径未展开验证）
- 安全缓解（`cond_mitigation()` `arch/x86/mm/tlb.c:384` 的 IBPB、L1D flush）**不是 TLB 刷新**，但同在切换路径上，profile 时容易混在一起

### 6.5 怎么观测

| 手段 | 用法 | 能看出什么 |
| --- | --- | --- |
| `tlb_flush` tracepoint | `echo 1 > /sys/kernel/debug/tracing/events/tlb/tlb_flush/enable` | `reason` 字段区分 5 种来源（`include/trace/events/tlb.h:12-16`，事件定义 `:38`）：task switch / remote shootdown / local shootdown / local mm shootdown / remote ipi send |
| `/proc/interrupts` 的 `TLB:` 行 | x86 | IPI shootdown 次数（`arch/x86/kernel/irq.c:107`） |
| `/proc/vmstat` | — | 只能间接看（`pgscan_*`、`thp_*`），**没有 TLB 计数** |

判断口径：

- `reason=flush on task switch` 占比高 → 看 6.2/6.3：x86 是否无 PCID、活跃 mm 是否超过 6 个；arm64 是否在频繁回卷
- `reason=remote shootdown` 占比高 → 是地址空间变更（6.4 第 1、2 类），去看 unmap/回收/迁移，而不是调度
- `reason=remote ipi send` 与 `remote shootdown` 之比 → 一次 IPI 扇出了多少核

## 7. 未验证项

- **上游主线是否已合入 TLBID/TLBIP 未验证**。本树证据只能说明它不在 openEuler 所基于的 v6.6 里，且提交信息标为 hulk inclusion；对 2026 年的上游状态没有比对依据。
- **FEAT_TLBID 从 Armv9.3 起**这一句引自本树 Kconfig 注释（`arch/arm64/Kconfig:2314`），未对照 Arm ARM 原文。
- 所有性能结论（domain 比广播省多少）都来自提交信息的主张，**本文没有实测数据**。
- x86 侧的 `INVPCID` 是否在新硬件上有区间形式，本树代码中未见。
- **6.2 里“写 CR3 只失效该 PCID、不动 global 项”是 x86 CR3 的架构语义**，本树代码只体现“带/不带 `CR3_NOFLUSH` 位”的用法（`arch/x86/mm/tlb.c:159-183`），未在 Intel/AMD 手册原文逐条核对。
- **6.3 里“ASID 位不随 mm 释放”是从代码推断的**：`asid_map` 只在 `flush_context()` 重置（`arch/arm64/mm/context.c:90-98`），未见 mm 退出路径清位；未用运行时实验验证回卷周期。
- 6.4 中 KVM stage-2 失效路径未展开读代码，仅标注为可能来源。

---

*基线：`8003dfcde32f` / kernel 6.6.0 / openEuler OLK-6.6。本文未在硬件上实测，性能相关表述均来自代码与提交信息。*
