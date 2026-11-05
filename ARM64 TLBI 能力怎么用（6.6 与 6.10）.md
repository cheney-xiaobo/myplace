---
title: ARM64 TLBI 能力怎么用（6.6 与 6.10）
aliases:
  - tlbi 用法
  - TLB range 用法
tags:
  - linux/kernel
  - arch/arm64
  - linux/mm
source: /Users/cheney/work/code/ub/kernel @ 8003dfcde32f (openEuler OLK-6.6)，另比对 v6.6 / v6.12.33 上游标签
created: 2026-11-01
updated: 2026-11-01
status: 部分验证（v6.6 与 v6.12.33 有标签可核实；v6.10 无标签，按其间提交推断）
---

# ARM64 TLBI 能力怎么用（6.6 与 6.10）

## 0. 一句话

**TLBI 没有开关、没有 sysfs、没有启动参数。** 内核启动时检测硬件能力，之后所有 `flush_tlb_*` 自动挑最优指令；你能“使用”的只是**内核 API**，外加确认它是否真的生效。6.6 与 6.10 的差别不在“怎么开”，而在**门控条件**和**range 操作的健壮性**。

## 1. 能力从哪里来

两个独立能力，各自一条检测规则（本树 `arch/arm64/kernel/cpufeature.c`）：

| 能力 | 检测字段 | 代码 |
| --- | --- | --- |
| `ARM64_HAS_TLB_RANGE`（区间失效） | `ID_AA64ISAR0_EL1.TLB == RANGE` | `:2987-2993` |
| `ARM64_HAS_ARMv8_4_TTL`（层级提示） | `ID_AA64MMFR2_EL1.TTL == IMP` | `:2980-2986` |

两者都是 `ARM64_CPUCAP_SYSTEM_FEATURE`，**启动时自动检测，没有 Kconfig 能直接“打开硬件能力”**。

## 2. 6.6 与 6.10 的关键差异：Kconfig 还管不管运行时

这是唯一一处会让你“在 6.6 上发现能力没用上”的地方。

**v6.6（标签核实）**：

```c
/* arch/arm64/include/asm/cpufeature.h（v6.6） */
static inline bool system_supports_tlb_range(void)
{
	return IS_ENABLED(CONFIG_ARM64_TLB_RANGE) &&
		cpus_have_const_cap(ARM64_HAS_TLB_RANGE);
}
```

**v6.12.33（标签核实）**：

```c
static inline bool system_supports_tlb_range(void)
{
	return alternative_has_cap_unlikely(ARM64_HAS_TLB_RANGE);
}
```

改动来自 `b54b52576443`（Mark Rutland，2023-10-16，*arm64: Avoid cpus_have_const_cap() for ARM64_HAS_TLB_RANGE*）。

含义：

| 版本 | 要同时满足什么才用上 range |
| --- | --- |
| **6.6** | 硬件支持 + `CONFIG_ARM64_TLB_RANGE=y`（依赖 `AS_HAS_ARMV8_4`，即汇编器 ≥ binutils 2.30） |
| **6.10 及以后** | 只要硬件支持；Kconfig 退化成“汇编器是否认识这些指令”的编译期条件 |

也就是说：**6.6 上如果有人把 `CONFIG_ARM64_TLB_RANGE` 关了（或汇编器太老导致默认关），即使 CPU 支持，range 路径也不会走，全部退化成逐页失效。** 6.10 起没这个坑。

Kconfig 本身在两边都在（`arch/arm64/Kconfig`，`default y`）：
- 本树 `arch/arm64/Kconfig:2295-2304`，`openeuler_defconfig:576` 为 `=y`
- `TLBI_TTL` 没有对应 Kconfig，完全靠硬件能力

## 3. 怎么确认已经生效

启动日志里由 `setup_system_capabilities()` 打印（`arch/arm64/kernel/cpufeature.c:4118-4148`，其中 `:4147` 是 `pr_info("detected: %s\n", caps->desc)`）：

```bash
dmesg | grep -i 'TLB range'
# 期望看到：CPU features: detected: TLB range maintenance instructions
```

注意它只在 `cpucap_match_all_early_cpus(caps) && cpus_have_cap(caps)` 成立时打印（`:4145-4147`），所以**没打印不等于不支持**，只是不满足打印条件。要更确定，只能看 `/proc/cpuinfo` 的 Features 或直接查 ID 寄存器（未在本文验证后者）。

## 4. 代码里怎么“用”

**原则：不要自己发 TLBI 指令。** 用内核 API，让 arch 代码去挑指令。

| 场景 | API | 备注 |
| --- | --- | --- |
| 内核映射区间 | `flush_tlb_kernel_range(start, end)` | 见下方版本差异 |
| 用户映射区间 | `flush_tlb_range(vma, start, end)` | 走 `__flush_tlb_range()`，含 range 优化 |
| munmap / 进程退出 | `tlb_flush_mmu()`、`mmu_gather` | 批量 + 延迟，最后统一发 |
| THP | `flush_pmd_tlb_range()` | `__HAVE_ARCH_FLUSH_PMD_TLB_RANGE`（`arch/arm64/include/asm/pgtable.h:39`） |
| 页回收 / 迁移 | `arch_tlbbatch_add_pending()` → `arch_tlbbatch_flush()` | v6.6 已有（`arch/arm64/include/asm/tlbflush.h:300`、`:327`） |
| KVM stage-2 | `__flush_s2_tlb_range_op()` | v6.6 `:400-401` |

驱动里如果只是映射/释放自己的缓冲区，走 `vmalloc` / `dma_alloc` / `vm_map_ram` 这些路径即可，它们内部已经调好了对应的 flush。

## 5. 什么时候会“退化”，这是能不能用上的关键

**v6.6 的 `__flush_tlb_range()`（标签核实）**：

```c
	/* 硬件不支持 range 且范围过大，或超过单条 range 指令上限 → 整 mm 刷 */
	if ((!system_supports_tlb_range() &&
	     (end - start) >= (MAX_TLBI_OPS * stride)) ||
	    pages >= MAX_TLBI_RANGE_PAGES) {
		flush_tlb_mm(vma->vm_mm);
		return;
	}
```

- `MAX_TLBI_OPS = PTRS_PER_PTE`（4K 页时 512）
- `MAX_TLBI_RANGE_PAGES = (31+1) × 2^16`
- 超过任一上限 → **整地址空间刷（`flush_tlb_mm`）**，代价最大

**内核区间在 v6.6 是逐页循环**（标签核实）：

```c
	if ((end - start) > (MAX_TLBI_OPS * PAGE_SIZE)) {
		flush_tlb_all();
		return;
	}
	...
	for (addr = start; addr < end; addr += 1 << (PAGE_SHIFT - 12))
		__tlbi(vaale1is, addr);
```

**上游 v6.12.33 仍然是逐页循环**，只是常量改名为 `MAX_DVM_OPS`。**而本树（OLK 6.6）已经回合了“内核区间走 range”的优化**（`67561efee231`，Kefeng Wang，2024-11-30）：

```c
	if (__flush_tlb_range_limit_excess(start, end, pages, stride)) {
		flush_tlb_all();
		return;
	}
	dsb(ishst);
	__flush_tlb_range_op(vaale1is, start, pages, stride, 0, 0, false);
```

这条**不在** v6.6、也不在 v6.12.33，属于本树额外带的优化（是否为更晚上游版本未验证）。

## 6. 6.6 → 6.10 其它实质变化

（依据：`git log v6.6..v6.12.33 -- arch/arm64/include/asm/tlbflush.h`，日期为提交日期；**6.10 无标签，无法逐条归位**）

| 变化 | 提交 | 日期 |
| --- | --- | --- |
| 去掉 range 的 Kconfig 运行时门控 | `b54b52576443` | 2023-10-16 |
| `MAX_TLBI_OPS` → `MAX_DVM_OPS` | `ec1c3b9ff160` | 2023-09-20 |
| range 改为**递减 scale**（更贴合大区间） | `e2768b798a19` | 2023-11-27 |
| range 支持 **FEAT_LPA2**（52 位，需 64K 对齐） | `c910f2b65518` | 2023-11-27 |
| 拆分 `__flush_tlb_range()`，省掉尾部 DSB | `d9d8dc2bd3fb` | 2024-02-15 |
| 修 TLBI RANGE 操作数 / 改进 `__TLBI_VADDR_RANGE` | `e3ba51ab24fd`、`e07255d69702` | 2024-04-05 |
| `MAX_TLBI_RANGE_PAGES` 也允许 range 操作 | `73301e464a72` | 2024-04-05 |

按日期推断，2024-02 与 2024-04 那批（DSB 消除、operand 修正）应该已在 6.9/6.10 里，**但本树没有 v6.10 标签，未验证**。

## 7. 实用建议

1. **6.6 上排查“range 没生效”**：先看 `CONFIG_ARM64_TLB_RANGE=y`，再看 `dmesg | grep -i 'TLB range'`，再看汇编器版本（`AS_HAS_ARMV8_4`）。三者缺一都会退化成逐页。
2. **6.10 上**：不用管 Kconfig，硬件支持就生效。
3. **想让内核区间也吃到 range**：上游 6.10 还没这功能（6.12.33 仍是逐页），需要类似本树 `67561efee231` 的补丁。
4. **别在驱动里手写 `tlbi` 指令**：屏障配对（`dsb ishst` / `dsb ish` / `isb`）和 ASID 编码错了很难查，用 API。

## 8. 未验证项

- **v6.10 无本地标签**，本文关于 6.10 的结论来自 v6.6 与 v6.12.33 两个标签的对比 + 提交日期推断。
- `dmesg` 打印那条只在特定条件出现（见 §3），不是可靠的“支持/不支持”判据。
- `67561efee231` 是否为更晚上游提交的回合，未验证（本树 `git tag --contains` 对它无输出，因为它是 OLK 分支上的独立提交对象）。

---

*基线：本树 `8003dfcde32f`（OLK-6.6）；上游比对使用本地标签 `v6.6`、`v6.12.33`。未在 arm64 硬件上实测。*
