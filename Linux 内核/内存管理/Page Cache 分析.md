---
title: Linux Page Cache 实现分析
aliases:
  - pagecache
  - page-cache
  - 页缓存分析
tags:
  - linux/kernel
  - linux/mm
  - linux/page-cache
source: /Users/cheney/work/code/ub/kernel @ 8003dfcde32f (openEuler OLK-6.6 / kernel 6.6.0)
created: 2026-10-30
updated: 2026-10-31
status: 已验证（静态阅读，未编译未运行）
---
# Linux Page Cache 实现分析

> 本文所有结论均来自本机源码树的实际阅读，行号与代码片段对应该树当前状态。
> 未读到的内容不写；标注「未验证」处为推测或未与上游逐行比对。

## 0. 源码定位与版本

| 项 | 值 |
| --- | --- |
| 源码根目录 | `/Users/cheney/work/code/ub/kernel` |
| 版本 | 6.6.0（`Makefile:2-5`：`VERSION = 6` / `PATCHLEVEL = 6` / `SUBLEVEL = 0`） |
| 代号 | `NAME = Hurr durr I'ma ninja sloth`（`Makefile:6`） |
| 发行分支 | openEuler OLK-6.6（`git describe`：`6.6.0-180.0.0-267-g8003dfcde32f`） |
| HEAD | `8003dfcde32f  !29996 [CVEAgent@OLK-6.6] Fix CVE-2026-98813` |
| mm 目录特征文件 | `filemap_local.c`、`gmem.c`、`etmem.c`、`dynamic_pool.c` 等，均为 openEuler 增量 |

```make
# kernel/Makefile:2-6
VERSION = 6
PATCHLEVEL = 6
SUBLEVEL = 0
EXTRAVERSION =
NAME = Hurr durr I'ma ninja sloth
```

规模参考：`mm/filemap.c` 4487 行、`mm/vmscan.c` 7730 行、`mm/readahead.c` 900 行、`mm/truncate.c` 888 行、`mm/page-writeback.c` 约 3300 行、`lib/xarray.c` 2474 行。

下文行号形如 `mm/filemap.c:1878`。

---

## 1. 核心设计

### 1.1 page cache 的定位

page cache 是**以页为单位的文件内容内存缓存**，索引是「文件内页偏移」`pgoff_t index`（单位为 `PAGE_SIZE`）。它同时承担四件事：

1. **读加速**：命中即直接从内存拷贝到用户态，免去块设备 I/O。
2. **写缓冲**：write 只改内存页并置脏（dirty），由回写线程异步落盘。
3. **mmap 的数据来源**：`mmap` 缺页时从 page cache 取页并映射进用户页表，与 `read/write` 共享同一份页。
4. **内存回收的对象**：这些页是**可回收的（reclaimable）**文件页，内存紧张时不需要写回即可直接丢弃（clean 页），或先写回再丢弃（dirty 页）。

它是 VFS/文件系统子系统与 MM 子系统的接合面：文件系统提供 `address_space_operations`（`a_ops`）定义「怎么从存储读/写/失效」，MM 负责「页放在哪、什么时候回收」。

### 1.2 address_space 与 inode 的关系

每个 inode 内嵌一个 `address_space`，它就是该文件 page cache 的宿主容器。

```c
/* include/linux/fs.h:500-519 */
struct address_space {
	struct inode		*host;          /* 反向指针：宿主 inode */
	struct xarray		i_pages;        /* 核心索引：页号 -> folio / shadow / swap 项 */
	struct rw_semaphore	invalidate_lock;/* 与 truncate/punch hole 互斥 */
	gfp_t			gfp_mask;       /* 该 mapping 的分配掩码 */
	...
	struct rb_root_cached	i_mmap;         /* 映射该文件的 VMA 区间树 */
	unsigned long		nrpages;
	pgoff_t			writeback_index;
	const struct address_space_operations *a_ops;
	unsigned long		flags;
	errseq_t		wb_err;
	...
} __attribute__((aligned(sizeof(long)))) __randomize_layout;
```

inode 侧：

```c
/* include/linux/fs.h:691 */
	struct address_space	*i_mapping;
/* include/linux/fs.h:762 */
	struct address_space	i_data;
```

两者在 inode 初始化时绑定——`i_mapping` 默认指向自己的 `i_data`：

```c
/* fs/inode.c:156,211,221 */
int inode_init_always(struct super_block *sb, struct inode *inode)
{
	struct address_space *const mapping = &inode->i_data;
	...
	mapping->a_ops = &empty_aops;
	mapping->host = inode;
	...
	mapping_set_gfp_mask(mapping, GFP_HIGHUSER_MOVABLE);
	...
	inode->i_mapping = mapping;
}
```

要点：

- 普通文件 `i_mapping == &inode->i_data`。少数场景会覆盖 `i_mapping` 以共享别的 mapping（例如某些伪文件系统/块设备把多个 inode 指向同一个 mapping）。
- 因此**同一个 inode 的所有缓存页有唯一 `address_space`**，`mapping->host` 指回 inode，形成 `inode ⇄ address_space` 双向绑定。
- 同一文件被多次 open/mmap，走的都是这一个 mapping，这正是页缓存能跨进程共享的基础。
- 反查：`folio_mapping()` 从 folio 找回它所属的 mapping，并过滤匿名页：

```c
/* mm/util.c:775-791 */
struct address_space *folio_mapping(struct folio *folio)
{
	if (unlikely(folio_test_slab(folio)))
		return NULL;
	if (unlikely(folio_test_swapcache(folio)))
		return swap_address_space(folio->swap);

	mapping = folio->mapping;
	if ((unsigned long)mapping & PAGE_MAPPING_FLAGS)   /* 低 2 位被复用于 anon/KSM 标记 */
		return NULL;
	return mapping;
}
```

`folio->mapping` 的低位被复用：`PAGE_MAPPING_ANON 0x1`、`PAGE_MAPPING_MOVABLE 0x2`（`include/linux/page-flags.h:678-681`），所以 page cache 页的 `mapping` 指针天然对齐、低两位为 0；这也是能拿它和 XArray 的 value 条目（最低位为 1）区分开的前提。

### 1.3 XArray（原 radix tree）索引结构

`i_pages` 是 `struct xarray`：

```c
/* include/linux/xarray.h */
struct xarray {
	spinlock_t	xa_lock;    /* 保护整棵树的锁，page cache 的 i_pages 锁就是它 */
	gfp_t		xa_flags;
	void __rcu *	xa_head;    /* 单条目或指向根 xa_node */
};
```

节点的分支因子固定为 64（`CONFIG_BASE_SMALL` 下为 16）：

```c
/* include/linux/xarray.h:1144-1150 */
#ifndef XA_CHUNK_SHIFT
#define XA_CHUNK_SHIFT		(CONFIG_BASE_SMALL ? 4 : 6)
#endif
#define XA_CHUNK_SIZE		(1UL << XA_CHUNK_SHIFT)
#define XA_CHUNK_MASK		(XA_CHUNK_SIZE - 1)
#define XA_MARK_LONGS		DIV_ROUND_UP(XA_CHUNK_SIZE, BITS_PER_LONG)
```

```c
/* include/linux/xarray.h — struct xa_node */
struct xa_node {
	unsigned char	shift;		/* 本层每个 slot 覆盖的位数 */
	unsigned char	offset;		/* 自己在父节点中的槽位 */
	unsigned char	count;
	unsigned char	nr_values;
	struct xa_node __rcu *parent;
	struct xarray	*array;
	...
	void __rcu	*slots[XA_CHUNK_SIZE];
	union {
		unsigned long	tags[XA_MAX_MARKS][XA_MARK_LONGS];
		unsigned long	marks[XA_MAX_MARKS][XA_MARK_LONGS];
	};
};
```

三个关键设计：

**(a) 一个槽位要么是对象指针，要么是「value」条目。** value 的最低位恒为 1：

```c
/* include/linux/xarray.h:55-59 */
static inline void *xa_mk_value(unsigned long v)
{
	WARN_ON((long)v < 0);
	return (void *)((v << 1) | 1);
}
/* include/linux/xarray.h:80-83 */
static inline bool xa_is_value(const void *entry)
{
	return (unsigned long)entry & 1;
}
```

这直接支撑了 page cache 里「同一个页号槽位可以放 folio 指针，也可以放 shadow 或 swap 项」——`xa_is_value()` 为真即表示「不是 page cache 页」。

**(b) 三种全局标记（mark）**，用来在不遍历树的情况下判断「有没有脏页/回写页」：

```c
/* include/linux/fs.h:535-538 */
#define PAGECACHE_TAG_DIRTY		XA_MARK_0
#define PAGECACHE_TAG_WRITEBACK		XA_MARK_1
#define PAGECACHE_TAG_TOWRITE		XA_MARK_2
```

`mapping_tagged(mapping, tag)` 只需读根节点（`include/linux/fs.h:543-546` → `xa_marked()`），是 O(1)。

**(c) 查找/插入靠 `xa_state` 游标。** 查找逐层下降：

```c
/* lib/xarray.c:236-250 */
void *xas_load(struct xa_state *xas)
{
	void *entry = xas_start(xas);
	while (xa_is_node(entry)) {
		struct xa_node *node = xa_to_node(entry);
		if (xas->xa_shift > node->shift)
			break;
		entry = xas_descend(xas, node);
		if (node->shift == 0)
			break;
	}
	return entry;
}
```

插入/替换/删除统一走 `xas_store()`（`lib/xarray.c:777`），它按 `xas->xa_shift` 决定是直接写 `xa_head` 还是写节点槽位，并维护 `count`/`nr_values` 与 mark 传播。打标记不能在任意上下文直接改数组，必须先定位条目：

```c
/* lib/xarray.c:2069-2077 */
void __xa_set_mark(struct xarray *xa, unsigned long index, xa_mark_t mark)
{
	XA_STATE(xas, xa, index);
	void *entry = xas_load(&xas);
	if (entry)
		xas_set_mark(&xas, mark);
}
```

这就是 `__folio_mark_dirty()` 必须持 `i_pages` 锁再打 DIRTY 标记的原因（见 §4.3）。

> 背景（非本树代码）：radix tree 在 4.20 前后被 XArray 取代，`i_pages` 的类型从 `struct radix_tree_root` 变为 `struct xarray`，`page->index` 语义不变，所以索引逻辑与旧代码同构。

### 1.4 folio 抽象

`folio` 是「一个物理连续、整体管理的内存块」的头结构，与 `struct page` 共享内存布局（用 `FOLIO_MATCH` 静态断言保证）：

```c
/* include/linux/mm_types.h:323-360（节选） */
struct folio {
	union {
		struct {
			unsigned long flags;
			union {
				struct list_head lru;          /* LRU 链节点 / 延迟拆分链 */
				struct { void *__filler; unsigned int mlock_count; };
			};
			struct address_space *mapping;   /* 所属 mapping（低 2 位复用标记） */
			pgoff_t index;                   /* 文件内页偏移（首个页） */
			union {
				void *private;               /* 文件系统私有数据（buffer_heads 等） */
				swp_entry_t swap;            /* swap cache 用 */
			};
			atomic_t _mapcount;
			atomic_t _refcount;              /* 唯一的引用计数 */
#ifdef CONFIG_MEMCG
			unsigned long memcg_data;
#endif
		};
		struct page page;
	};
	union {  /* 第二个 page 的头信息 */
		struct {
			unsigned long _flags_1, _head_1, _folio_avail;
			atomic_t _entire_mapcount;
			atomic_t _nr_pages_mapped;
			atomic_t _pincount;
			unsigned int _folio_nr_pages;    /* 阶数信息 */
		};
		struct page __page_1;
	};
	...
};
```

```c
/* include/linux/mm_types.h:401-431 */
FOLIO_MATCH(flags, flags);
FOLIO_MATCH(lru, lru);
FOLIO_MATCH(mapping, mapping);
FOLIO_MATCH(index, index);
FOLIO_MATCH(private, private);
FOLIO_MATCH(_mapcount, _mapcount);
FOLIO_MATCH(_refcount, _refcount);
```

设计收益（对 page cache 尤其重要）：

- **一个 folio 一个锁、一个引用计数、一次 I/O**：order-N 的 folio 用一次 `read_folio` 搬 `N * PAGE_SIZE`，减少块层往返。
- `index` 是首页偏移，`folio_nr_pages(folio)` 给出页数，`folio_contains()` 判断某 index 是否落在该 folio 内——查找时不必假设 order=0。
- 布局与 `struct page` 兼容，使旧代码可以 `&folio->page` 继续工作（过渡期设计）。

page cache 对 large folio 的支持是**按文件系统显式开启**的：

```c
/* include/linux/pagemap.h:429-432 */
static inline void mapping_set_large_folios(struct address_space *mapping)
{
	mapping_set_folio_order_range(mapping, 0, MAX_PAGECACHE_ORDER);
}
/* include/linux/pagemap.h:374 */
#define MAX_PAGECACHE_ORDER	min(MAX_XAS_ORDER, PREFERRED_MAX_PAGECACHE_ORDER)
```

未开启时 `__filemap_get_folio()` 会强制 `order = 0`（`mm/filemap.c:1935-1936`）。

### 1.5 页的状态与标志位

标志位定义在 `include/linux/page-flags.h`：

```c
/* include/linux/page-flags.h:100-124（节选） */
enum pageflags {
	PG_locked,		/* Page is locked. Don't touch. */
	PG_writeback,		/* Page is under writeback */
	PG_referenced,
	PG_uptodate,
	PG_dirty,
	PG_lru,
	PG_head,		/* Must be in bit 6 */
	PG_waiters,		/* Page has waiters, check its waitqueue. Must be bit #7 ... */
	PG_active,
	PG_workingset,
	PG_error,
	PG_slab,
	PG_owner_priv_1,	/* 文件系统可用 */
	PG_arch_1,
	PG_reserved,
	PG_private,		/* If pagecache, has fs-private data */
	PG_private_2,		/* If pagecache, has fs aux data */
	PG_mappedtodisk,
	PG_reclaim,		/* To be reclaimed asap */
	PG_swapbacked,		/* Page is backed by RAM/swap */
	PG_unevictable,
	...
};
/* include/linux/page-flags.h:147 */
	PG_readahead = PG_reclaim,
```

与 page cache 直接相关的语义：

| 标志 | 含义 | 谁设置 / 谁等待 |
| --- | --- | --- |
| `PG_locked` | folio 互斥锁（不可重入）。保护内容与状态，I/O 期间持有 | `folio_lock()`/`folio_trylock()`；`truncate`、`filemap_remove_folio` 都要求先持有 |
| `PG_uptodate` | 内容有效（读 I/O 成功完成或整页写完） | `filemap_read_folio` 成功后由 `a_ops->read_folio` 置位；`filemap_read` 用它决定能否直接拷贝 |
| `PG_dirty` | 有未回写修改 | `folio_mark_dirty()`；与 `PAGECACHE_TAG_DIRTY` 标记配套 |
| `PG_writeback` | 正在回写 | `folio_start_writeback()`，`folio_end_writeback()` 清除 |
| `PG_lru` | 在 LRU 链上 | `folio_add_lru()` 后由 per-cpu 批处理落地 |
| `PG_active` / `PG_referenced` | 二次机会 LRU 的活跃度 | `folio_mark_accessed()` |
| `PG_workingset` | 被 refault 判定为工作集 | `workingset_refault()` |
| `PG_reclaim` | 尽快回收；**与 `PG_readahead` 共用同一位** | 回收路径 / 预读 |

`PG_locked` 的语义在源码注释里写得很明确：

```c
/* include/linux/page-flags.h:56-60 */
 * During initiation of disk I/O, PG_locked is set. This bit is set before I/O
 * and cleared when writeback _starts_ or when read _completes_. PG_writeback
 * is set before writeback _starts_ and cleared when
 * writeback _completes_.
 * PG_locked also pins a page in pagecache, and blocks truncation of the file
```

即：**读 I/O 靠 `PG_locked` 表示「正在读取」，写 I/O 靠 `PG_writeback` 表示「正在回写」**。这也决定了后面所有「等页可用」的代码都在等 `PG_locked`，而「等落盘」的代码都在等 `PG_writeback`。

### 1.6 与 LRU / 回收子系统的关系

page cache 页进入系统后必须挂到某条 LRU 链上，才能被回收器找到。链的集合在 `lruvec` 里：

```c
/* include/linux/mmzone.h — struct lruvec */
struct lruvec {
	struct list_head		lists[NR_LRU_LISTS];
	spinlock_t			lru_lock;
	unsigned long			anon_cost;
	unsigned long			file_cost;
	atomic_long_t			nonresident_age;   /* shadow 时间戳的基准 */
	unsigned long			refaults[ANON_AND_FILE];
	unsigned long			flags;
#ifdef CONFIG_LRU_GEN
	struct lru_gen_folio		lrugen;
#endif
	...
};
```

```c
/* include/linux/mmzone.h:275-282 */
enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE,
	LRU_UNEVICTABLE,
	NR_LRU_LISTS
};
```

页该上哪条链由 `folio_lru_list()` 决定（file 页固定进 `LRU_*_FILE`）：

```c
/* include/linux/mm_inline.h:87-101 */
static __always_inline enum lru_list folio_lru_list(struct folio *folio)
{
	if (folio_test_unevictable(folio))
		return LRU_UNEVICTABLE;

	lru = folio_is_file_lru(folio) ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	if (folio_test_active(folio))
		lru += LRU_ACTIVE;
	return lru;
}
```

关系链可以概括为：

```
read/write -> page cache folio -> folio_add_lru() -> lruvec->lists[LRU_INACTIVE_FILE]
                                                        |
内存压力 -> shrink_folio_list() <- 从链尾摘页 <- folio_isolate/lru_to_folio
                |
                +-> clean: 直接 __remove_mapping + free
                +-> dirty: pageout() -> ->writepage -> 置 PG_writeback -> 下次再回收
```

**关键点：page cache 是「文件页（file LRU）」的唯一主要来源**，而回收器对 file 页和 anon 页采取不同策略（file 页可直接丢弃，anon 页必须先换出），这一区别贯穿 `shrink_folio_list()` 与 `get_scan_count()`（`mm/vmscan.c:2399`）。

---

## 2. 内存申请路径

### 2.1 read 的完整调用链

```
read(2)
 └─ ksys_read / vfs_read                      (fs/read_write.c)
     └─ file->f_op->read_iter                 (常规文件 = generic_file_read_iter)
         └─ generic_file_read_iter()           mm/filemap.c:2832
             ├─ IOCB_DIRECT: ->direct_IO()     直接 I/O 绕过 page cache
             └─ filemap_read()                 mm/filemap.c:2647   ← 缓冲读主循环
                 └─ filemap_get_pages()        mm/filemap.c:2565
                     ├─ filemap_get_read_batch()          mm/filemap.c:2359  ← 无锁批量查缓存
                     ├─ page_cache_sync_readahead()       include/linux/pagemap.h:1404
                     │    └─ page_cache_sync_ra()          mm/readahead.c:719
                     │        └─ ondemand_readahead()      mm/readahead.c:567
                     │            └─ page_cache_ra_order() mm/readahead.c:486（large folio）
                     │            └─ do_page_cache_ra()    mm/readahead.c:281
                     │                └─ page_cache_ra_unbounded()  mm/readahead.c:201
                     │                └─ read_pages()      mm/readahead.c:150
                     │                    └─ a_ops->readahead() / a_ops->read_folio()
                     └─ filemap_create_folio()            mm/filemap.c:2508  ← 未命中，分配新页
                         ├─ filemap_alloc_folio()         mm/filemap.c:990
                         │   └─ folio_alloc()             include/linux/gfp.h:314
                         │       └─ __folio_alloc_node() → __alloc_pages()  mm/page_alloc.c:4998
                         ├─ filemap_add_folio()           mm/filemap.c:956
                         │   └─ __filemap_add_folio()     mm/filemap.c:851   ← 插入 XArray
                         │   └─ folio_add_lru()           mm/swap.c:500      ← 挂 LRU
                         └─ filemap_read_folio()          mm/filemap.c:2395  ← 真正发起读 I/O
                             └─ a_ops->read_folio() → folio_wait_locked_killable()
```

`filemap_read()` 主循环本身只做三件事：取批、做 i_size 与 uptodate 校验、`copy_folio_to_iter()` 拷贝：

```c
/* mm/filemap.c:2669-2704（节选） */
	do {
		cond_resched();
		if (unlikely(iocb->ki_pos >= i_size_read(inode)))
			break;
		error = filemap_get_pages(iocb, iter->count, &fbatch, false);
		if (error < 0)
			break;
		isize = i_size_read(inode);
		if (unlikely(iocb->ki_pos >= isize))
			goto put_folios;
		end_offset = min_t(loff_t, isize, iocb->ki_pos + iter->count);
```

注意 `i_size` 是在确认 folio uptodate 之后才读的，注释解释了原因（`mm/filemap.c:2684-2691`）：避免把截断后新扩展出来的空洞内容读给用户，同时保证计数一致。

### 2.2 未命中时的分配：`filemap_create_folio()`

```c
/* mm/filemap.c:2508-2546 */
static int filemap_create_folio(struct file *file,
		struct address_space *mapping, pgoff_t index,
		struct folio_batch *fbatch)
{
	folio = filemap_alloc_folio(mapping_gfp_mask(mapping), 0);
	if (!folio)
		return -ENOMEM;

	/*
	 * Protect against truncate / hole punch. Grabbing invalidate_lock
	 * here assures we cannot instantiate and bring uptodate new
	 * pagecache folios after evicting page cache during truncate
	 * and before actually freeing blocks.
	 */
	filemap_invalidate_lock_shared(mapping);
	error = filemap_add_folio(mapping, folio, index,
			mapping_gfp_constraint(mapping, GFP_KERNEL));
	if (error == -EEXIST)
		error = AOP_TRUNCATED_PAGE;
	if (error)
		goto error;

	error = filemap_read_folio(file, mapping->a_ops->read_folio, folio);
	...
	filemap_invalidate_unlock_shared(mapping);
	folio_batch_add(fbatch, folio);
	return 0;
error:
	filemap_invalidate_unlock_shared(mapping);
	folio_put(folio);
	return error;
}
```

三个要点：

1. **order 固定为 0**：这条「未命中补页」的路径只分配单页；large folio 由预读路径负责（`page_cache_ra_order`）。
2. **`invalidate_lock` 读锁跨越整个分配→插入→读 I/O 过程**：与 `truncate`/`punch hole` 的写锁互斥，防止「truncate 已把页踢掉、块已释放」之后又插进来一个新页。源码注释明确说明这是为了保持锁规则简单（`mm/filemap.c:2519-2532`）。
3. **`-EEXIST` 转成 `AOP_TRUNCATED_PAGE`**：说明竞争中别的线程已抢先插入，`filemap_get_pages()` 收到后 `goto retry` 重来。

`filemap_read_folio()` 负责发起 I/O 并等待：

```c
/* mm/filemap.c:2395-2420 */
static int filemap_read_folio(struct file *file, filler_t filler,
		struct folio *folio)
{
	bool workingset = folio_test_workingset(folio);
	unsigned long pflags = 0;
	int error;

	folio_clear_error(folio);

	/* Start the actual read. The read will unlock the page. */
	if (unlikely(workingset))
		psi_memstall_enter(&pflags);
	error = filler(file, folio);
	...
	error = folio_wait_locked_killable(folio);
	if (error)
		return error;
	if (folio_test_uptodate(folio))
		return 0;
	if (file)
		shrink_readahead_size_eio(&file->f_ra);   /* 读失败：缩小预读窗口 */
	return -EIO;
}
```

注释「The read will unlock the page」是理解锁模型的关键：`->read_folio()` 负责在 I/O 完成回调里 `folio_unlock()`；若文件系统同步完成，它在返回前就解锁了。所以这里的 `folio_wait_locked_killable()` 通常立即返回。

### 2.3 `__filemap_get_folio()` 与 FGP 标志语义

这是 page cache 通用入口，`FGP_*` 决定行为。定义：

```c
/* include/linux/pagemap.h:698-708 */
#define FGP_ACCESSED		((__force fgf_t)0x00000001)
#define FGP_LOCK		((__force fgf_t)0x00000002)
#define FGP_CREAT		((__force fgf_t)0x00000004)
#define FGP_WRITE		((__force fgf_t)0x00000008)
#define FGP_NOFS		((__force fgf_t)0x00000010)
#define FGP_NOWAIT		((__force fgf_t)0x00000020)
#define FGP_FOR_MMAP		((__force fgf_t)0x00000040)
#define FGP_STABLE		((__force fgf_t)0x00000080)
#define FGF_GET_ORDER(fgf)	(((__force unsigned)fgf) >> 26)	/* top 6 bits */

#define FGP_WRITEBEGIN		(FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE)
```

| 标志 | 语义 | 代码位置 |
| --- | --- | --- |
| `FGP_ACCESSED` | 命中后置 `PG_referenced`/激活，用于 LRU 二次机会 | `mm/filemap.c:1909-1910` |
| `FGP_LOCK` | 返回时 folio 已加锁；`FGP_NOWAIT` 时用 `folio_trylock` 失败返回 `-EAGAIN` | `mm/filemap.c:1893-1906` |
| `FGP_CREAT` | 未命中就分配+插入+挂 LRU，返回已加锁 folio | `mm/filemap.c:1920` 起 |
| `FGP_WRITE` | 调用方要写；给 gfp 加 `__GFP_WRITE`，并清 idle 位 | `mm/filemap.c:1923-1929` |
| `FGP_NOFS` | 清 `__GFP_FS`，避免递归回文件系统 | `mm/filemap.c:1924` |
| `FGP_NOWAIT` | 不阻塞：gfp 换 `GFP_NOWAIT|__GFP_NOWARN`，`-ENOMEM` 转 `-EAGAIN` | `mm/filemap.c:1925-1928, 1979-1980` |
| `FGP_FOR_MMAP` | 允许调用方自己做锁 dance，分配后立即解锁 | `mm/filemap.c:1987-1988` |
| `FGP_STABLE` | 等 `PG_writeback` 结束（`folio_wait_stable`），保证写前页稳定 | `mm/filemap.c:1909-1918` |

命中路径（`mm/filemap.c:1878-1918`）：

```c
repeat:
	folio = filemap_get_entry(mapping, index);
	if (xa_is_value(folio))       /* shadow / swap 项当作未命中 */
		folio = NULL;
	if (!folio)
		goto no_page;

	if (fgp_flags & FGP_LOCK) {
		if (fgp_flags & FGP_NOWAIT) {
			if (!folio_trylock(folio)) { folio_put(folio); return ERR_PTR(-EAGAIN); }
		} else {
			folio_lock(folio);
		}
		/* Has the page been truncated? */
		if (unlikely(folio->mapping != mapping)) {
			folio_unlock(folio); folio_put(folio); goto repeat;
		}
		VM_BUG_ON_FOLIO(!folio_contains(folio, index), folio);
	}
```

**「加锁后必须复核」** 是这里最重要的代码模式：拿到引用后，页可能已被 truncate 从该 mapping 摘走并重新分配进别的文件，所以要用 `folio->mapping != mapping` 检测，必要时重试。

分配路径的两个细节（`mm/filemap.c:1935-1988`）：

```c
		if (!mapping_large_folio_support(mapping))
			order = 0;
		...
		if (order > MAX_PAGECACHE_ORDER)
			order = MAX_PAGECACHE_ORDER;
		/* If we're not aligned, allocate a smaller folio */
		if (index & ((1UL << order) - 1))
			order = __ffs(index);

		do {
			gfp_t alloc_gfp = gfp;
			err = -ENOMEM;
			if (order > 0)
				alloc_gfp |= __GFP_NORETRY | __GFP_NOWARN;
			folio = filemap_alloc_folio(alloc_gfp, order);
			if (!folio)
				continue;
			if (fgp_flags & FGP_ACCESSED)
				__folio_set_referenced(folio);
			err = filemap_add_folio(mapping, folio, index, gfp);
			if (!err)
				break;
			folio_put(folio);
			folio = NULL;
		} while (order-- > 0);
```

- **对齐降阶**：`index & (2^order - 1)` 非零时用 `__ffs(index)` 退到能覆盖该 index 的最大阶，保证 folio 不跨边界。
- **分配失败逐级降阶**：`while (order-- > 0)` 保证大 folio 拿不到时最终退到 order 0，而不是直接失败。
- `order > 0` 的分配强制 `__GFP_NORETRY | __GFP_NOWARN`，避免为「可选优化」触发代价高昂的直接回收。

### 2.4 插入 XArray 与内存统计：`__filemap_add_folio()`

```c
/* mm/filemap.c:851-870（节选） */
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	XA_STATE_ORDER(xas, &mapping->i_pages, index, folio_order(folio));
	bool huge;
	long nr;
	unsigned int forder = folio_order(folio);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio);
	mapping_set_update(&xas, mapping);

	VM_BUG_ON_FOLIO(index & (folio_nr_pages(folio) - 1), folio);
	huge = folio_test_hugetlb(folio);
	nr = folio_nr_pages(folio);

	gfp &= GFP_RECLAIM_MASK;
	folio_ref_add(folio, nr);
	folio->mapping = mapping;
	folio->index = xas.xa_index;
```

循环体处理「槽位冲突」：

```c
/* mm/filemap.c:870-916（节选） */
	for (;;) {
		int order = -1;
		void *entry, *old = NULL;

		xas_lock_irq(&xas);
		xas_for_each_conflict(&xas, entry) {
			old = entry;
			if (!xa_is_value(entry)) {
				xas_set_err(&xas, -EEXIST);   /* 已有 folio：让调用方重试 */
				goto unlock;
			}
			if (order == -1)
				order = xas_get_order(&xas);
		}
		if (old) {
			if (order > 0 && order > forder) {
				... /* 拆分大的 swap/shadow 条目，腾出空间 */
			}
			if (shadowp)
				*shadowp = old;               /* 把 shadow 交给上层的 refault 判定 */
		}
		xas_store(&xas, folio);
		if (xas_error(&xas))
			goto unlock;
		mapping->nrpages += nr;
		if (!huge) {
			__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, nr);
			if (folio_test_pmd_mappable(folio))
				__lruvec_stat_mod_folio(folio, NR_FILE_THPS, nr);
		}
unlock:
		xas_unlock_irq(&xas);
		if (!xas_nomem(&xas, gfp))
			break;
		xas_set_order(&xas, index, forder);   /* 用预分配的 node 内存重试 */
	}
```

值得注意的实现细节：

- `gfp &= GFP_RECLAIM_MASK`（`:867`）：这里收窄 gfp 只为 `xas_nomem()` 预分配 XArray 节点服务，避免把 `__GFP_MOVABLE` 之类传给 slab 分配。
- `folio_ref_add(folio, nr)`（`:868`）：**在插入前就把引用计数加到 `nr`**，对应「每个基页一个 page cache 引用」；失败时 `error` 分支用 `folio_put_refs(folio, nr)` 精确回滚（`:951`）。
- `xas_nomem()` 返回真表示「已释放锁并预分配了节点内存」，于是跳出 `xas_lock_irq` 后重新进入循环，这是 XArray 在原子上下文插入的标准失败重试模式。
- 冲突处理区分两类：`!xa_is_value(entry)` 说明真有 folio 占位 → `-EEXIST`；是 value（shadow/swap）→ 记录 order，必要时拆分，并把 shadow 交给 `shadowp` 供之后判断 refault。

上层的 `filemap_add_folio()` 负责 memcg 计费、上锁与 LRU 接入：

```c
/* mm/filemap.c:956-988 */
int filemap_add_folio(struct address_space *mapping, struct folio *folio,
				pgoff_t index, gfp_t gfp)
{
	void *shadow = NULL;
	int ret;

	ret = mem_cgroup_charge(folio, NULL, gfp);
	if (ret)
		return ret;

	__folio_set_locked(folio);
	ret = __filemap_add_folio(mapping, folio, index, gfp, &shadow);
	if (unlikely(ret)) {
		mem_cgroup_uncharge(folio);
		__folio_clear_locked(folio);
	} else {
		WARN_ON_ONCE(folio_test_active(folio));
		if (!(gfp & __GFP_WRITE) && shadow)
			workingset_refault(folio, shadow);   /* 命中影子 -> 可能是工作集 */
		folio_add_lru(folio);
	}
	return ret;
}
```

`!(gfp & __GFP_WRITE)` 这个条件很有讲究：如果是「马上要被覆盖写」的页（`FGP_WRITE`），把它当作工作集来激活是浪费——注释原文见 `mm/filemap.c:973-982`。

### 2.5 预读（readahead）机制

预读的目标是「用一次大 I/O 覆盖后续若干次读」。窗口状态在 `struct file_ra_state`（`ra->start/size/async_size`）。

同步预读入口是内联包装：

```c
/* include/linux/pagemap.h:1404-1410 */
static inline
void page_cache_sync_readahead(struct address_space *mapping,
		struct file_ra_state *ra, struct file *file, pgoff_t index,
		unsigned long req_count)
{
	DEFINE_READAHEAD(ractl, file, ra, mapping, index);
	page_cache_sync_ra(&ractl, req_count);
}
```

（在 6.6 里函数名带 `_ra` 后缀；早期版本的 `page_cache_ra()` / `page_cache_readahead()` 已被拆成 `_sync_` / `_async_` / `_order` / `_unbounded` 几个入口。）

窗口增长策略：

```c
/* mm/readahead.c:343-357 */
static unsigned long get_init_ra_size(unsigned long size, unsigned long max)
{
	unsigned long newsize = roundup_pow_of_two(size);
	if (newsize <= max / 32)      newsize = newsize * 4;
	else if (newsize <= max / 4)  newsize = newsize * 2;
	else                          newsize = max;
	return newsize;
}
/* mm/readahead.c:361-370 */
static unsigned long get_next_ra_size(struct file_ra_state *ra,
				      unsigned long max)
{
	unsigned long cur = ra->size;
	if (cur < max / 16)  return 4 * cur;
	if (cur <= max / 2)  return 2 * cur;
	return max;
}
```

`ondemand_readahead()`（`mm/readahead.c:567`）根据「是否是预期页」「是否顺序」决定：首次预读、顺序增长、还是退回单页。判定顺序访问的核心是比较 `index` 与 `expected = round_down(ra->start + ra->size - ra->async_size, ...)`：

```c
/* mm/readahead.c:595-605 */
	expected = round_down(ra->start + ra->size - ra->async_size,
			1UL << order);
	if (folio && index == expected) {
		ra->start += ra->size;
		ra->size = max(ra->size, get_next_ra_size(ra, max_pages));
		ra->async_size = ra->size;
		goto readit;
	}
```

**预读与 page cache 的接口就是「顺手把 folio 提前插进 `i_pages` 并置 `PG_readahead`」**：

```c
/* mm/readahead.c:258-259 */
		if (i == nr_to_read - lookahead_size)
			folio_set_readahead(folio);
		ractl->_workingset |= folio_test_workingset(folio);
		ractl->_nr_pages++;
```

之后 `filemap_get_read_batch()` 遇到 `PG_readahead` 就停止取批（`mm/filemap.c:2383-2384`），把控制权交回 `filemap_get_pages()`，由它调 `filemap_readahead()` → `page_cache_async_ra()` 推进窗口。这就是「pipelining」：不必等应用读完整个窗口才发下一批 I/O。

批量提交 I/O 在 `read_pages()`：

```c
/* mm/readahead.c:150-179 */
static void read_pages(struct readahead_control *rac)
{
	const struct address_space_operations *aops = rac->mapping->a_ops;

	if (!readahead_count(rac))
		return;
	if (unlikely(rac->_workingset)) { ... psi_memstall_enter(&rac->_pflags); }
	blk_start_plug(&plug);

	if (aops->readahead) {
		aops->readahead(rac);
		/* Clean up the remaining folios. */
		while ((folio = readahead_folio(rac)) != NULL) {
			folio_get(folio);
			filemap_remove_folio(folio);
			folio_unlock(folio);
			folio_put(folio);
		}
	} else {
		while ((folio = readahead_folio(rac)) != NULL)
			aops->read_folio(rac->file, folio);
	}
	blk_finish_plug(&plug);
```

`blk_start_plug()`/`blk_finish_plug()` 把这一批 folio 的 I/O 合并进同一个请求队列批次，这是预读吞吐的关键。若文件系统没实现 `->readahead`，就退回逐页 `->read_folio`；已提交但未被消费的页要 `filemap_remove_folio()` 清理掉。

large folio 预读走独立路径 `page_cache_ra_order()`（`mm/readahead.c:486`），会按对齐逐步升阶，并显式处理「不要分配越过 EOF 的页」：

```c
/* mm/readahead.c:523-534 */
	while (index <= limit) {
		unsigned int order = new_order;
		/* Align with smaller pages if needed */
		if (index & ((1UL << order) - 1))
			order = __ffs(index);
		/* Don't allocate pages past EOF */
		while (index + (1UL << order) - 1 > limit)
			order--;
		err = ra_alloc_folio(ractl, index, mark, order, gfp);
		if (err)
			break;
		index += 1UL << order;
	}
```

注意 `while (... > limit) order--`：如果降到 order 0 仍越界，`order` 会下溢成 `UINT_MAX`，此时 `ra_alloc_folio` 里的 `index & ((1UL << order) - 1)` 结果取决于移位宽度——属于**需要有 EOF 边界保证才能安全的前置条件**（`limit` 由 `i_size` 计算，见 `:492`）；如果调用方在 i_size 之外触发预读，这里的行为值得复查。*（此点未构造用例验证，标注为待验证风险。）*

### 2.6 内存分配的实际入口与 GFP 语义

`filemap_alloc_folio()` 是本树中带 NUMA 本地化策略的版本：

```c
/* mm/filemap.c:989-1014 */
#ifdef CONFIG_NUMA
struct folio *filemap_alloc_folio(gfp_t gfp, unsigned int order)
{
	int n;
	struct folio *folio;

	filemap_prepare_alloc(&gfp);

	if (filemap_alloc_local_enabled())
		return filemap_try_alloc_local(gfp, order);

	if (cpuset_do_page_mem_spread()) {
		unsigned int cpuset_mems_cookie;
		do {
			cpuset_mems_cookie = read_mems_allowed_begin();
			n = cpuset_mem_spread_node();
			folio = __folio_alloc_node(gfp, order, n);
		} while (!folio && read_mems_allowed_retry(cpuset_mems_cookie));
		return folio;
	}
	return folio_alloc(gfp, order);
}
EXPORT_SYMBOL(filemap_alloc_folio);
#endif
```

非 NUMA 配置下改用内联版本，直接透传：

```c
/* include/linux/pagemap.h:641-649 */
#ifdef CONFIG_NUMA
struct folio *filemap_alloc_folio(gfp_t gfp, unsigned int order);
#else
static inline struct folio *filemap_alloc_folio(gfp_t gfp, unsigned int order)
{
	return folio_alloc(gfp, order);
}
#endif
```

继续往下就是通用伙伴系统入口：

```c
/* include/linux/gfp.h:314-317 */
static inline struct folio *folio_alloc(gfp_t gfp, unsigned int order)
{
	return __folio_alloc_node(gfp, order, numa_node_id());
}
```

```c
/* mm/page_alloc.c:4998-5004 */
struct page *__alloc_pages(gfp_t gfp, unsigned int order, int preferred_nid,
							nodemask_t *nodemask)
{
	struct page *page;
	unsigned int alloc_flags = ALLOC_WMARK_LOW;
	gfp_t alloc_gfp; /* The gfp_t that was actually used for allocation */
	struct alloc_context ac = { };
```

page cache 页的默认 gfp 在 inode 初始化时定下：

```c
/* fs/inode.c:211 */
	mapping_set_gfp_mask(mapping, GFP_HIGHUSER_MOVABLE);
```

```c
/* include/linux/gfp_types.h:336,344-345 */
#define GFP_KERNEL	(__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_HIGHUSER	(GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE	(GFP_HIGHUSER | __GFP_MOVABLE | __GFP_SKIP_KASAN)
```

语义解读：

- `__GFP_HIGHMEM`：允许分配在高端内存（32 位系统必要；64 位无实质影响）。
- `__GFP_MOVABLE`：**可迁移**——这些页内容不涉及内核直接解引用（DMA 除外），压缩/迁移时可以搬家，从而能与不可迁移页分开，减少碎片。
- `__GFP_RECLAIM`：允许直接回收。
- **没有 `__GFP_IO`/`__GFP_FS`**：注意 `GFP_HIGHUSER_MOVABLE` 展开里不含 `__GFP_FS`，而 `filemap_create_folio()` 又用 `mapping_gfp_constraint(mapping, GFP_KERNEL)` 取交集，得到的结果仍然不含 `__GFP_FS`。这保证了「分配 page cache 页时不会递归进入文件系统回收路径」——正是 `page_cache_ra_unbounded()` 里用 `memalloc_nofs_save()` 显式强调的那件事（`mm/readahead.c:210-219`）。

预读则在此基础上再加「不许重试」：

```c
/* include/linux/pagemap.h:660-663 */
static inline gfp_t readahead_gfp_mask(struct address_space *x)
{
	return mapping_gfp_mask(x) | __GFP_NORETRY | __GFP_NOWARN;
}
```

`__GFP_NORETRY` 让预读页分配失败时**只尝试轻量回收、不触发直接回收 stall**，避免「为了预读把进程卡死」——这是预读最容易被误解的性能设计点。

---

## 3. 释放路径

释放有三条独立入口，最后都汇到 `free_unref_page()`：

1. **truncate / invalidate**：文件被截断或显式失效，页必须丢弃（可能先写回）。
2. **回收（reclaim）**：内存压力下 `kswapd`/直接回收走 `shrink_folio_list()`。
3. **引用归零**：正常 `folio_put()`，当 `_refcount` 归零时真正释放。

### 3.1 truncate / invalidate

主函数是 `truncate_inode_pages_range()`，采用「批量查找 + 批量删除」两阶段：

```c
/* mm/truncate.c:347-391（节选） */
void truncate_inode_pages_range(struct address_space *mapping,
				loff_t lstart, loff_t lend)
{
	...
	start = (lstart + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (lend == -1)
		end = -1;
	else
		end = (lend + 1) >> PAGE_SHIFT;

	folio_batch_init(&fbatch);
	index = start;
	while (index < end && find_lock_entries(mapping, &index, end - 1,
			&fbatch, indices)) {
		truncate_folio_batch_exceptionals(mapping, &fbatch, indices);
		for (i = 0; i < folio_batch_count(&fbatch); i++)
			truncate_cleanup_folio(fbatch.folios[i]);
		delete_from_page_cache_batch(mapping, &fbatch);
		for (i = 0; i < folio_batch_count(&fbatch); i++)
			folio_unlock(fbatch.folios[i]);
		folio_batch_release(&fbatch);
		cond_resched();
	}
```

- 先 `find_lock_entries()`（`mm/filemap.c:2101`）批量取并**加锁**——加锁是必要的，否则无法安全摘除。
- `truncate_cleanup_folio()` 负责解除用户页表映射、通知文件系统失效：

```c
/* mm/truncate.c:172-187 */
static void truncate_cleanup_folio(struct folio *folio)
{
	if (folio_mapped(folio))
		unmap_mapping_folio(folio);
	if (folio_needs_release(folio))
		folio_invalidate(folio, 0, folio_size(folio));
	folio_cancel_dirty(folio);
	folio_clear_mappedtodisk(folio);
}
```

- 然后 `delete_from_page_cache_batch()`（`mm/filemap.c:323`）一次性摘除并把 `filemap_free_folio()` 延后到锁外做（见 §3.4）。
- 部分页（`lstart`/`lend` 不整除）走 `truncate_inode_partial_folio()`（`mm/truncate.c:235`），对 large folio 可能先拆分再失效。
- 循环结束后还有一个「再扫一遍确认全清空」的阶段（`mm/truncate.c:415-460`），因为上面的批处理过程中可能有新页插入。

单页版本：

```c
/* mm/truncate.c:189-198 */
int truncate_inode_folio(struct address_space *mapping, struct folio *folio)
{
	if (folio->mapping != mapping)
		return -EIO;
	truncate_cleanup_folio(folio);
	filemap_remove_folio(folio);
	return 0;
}
```

`filemap_remove_folio()` 是「从缓存摘页 + 释放引用」的标准实现：

```c
/* mm/filemap.c:253-268 */
void filemap_remove_folio(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;

	BUG_ON(!folio_test_locked(folio));
	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	__filemap_remove_folio(folio, NULL);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_add_lru(mapping->host);
	spin_unlock(&mapping->host->i_lock);

	filemap_free_folio(mapping, folio);
}
```

锁顺序值得记：`host->i_lock` → `i_pages.xa_lock`。文件级注释 `mm/filemap.c:81-129` 给出了完整的锁序约束（`i_rwsem` → `invalidate_lock` → page lock → `i_pages` → `i_lock`/`private_lock` …），这是分析死锁时的第一手依据。

真正的摘除：

```c
/* mm/filemap.c:131-149 */
static void page_cache_delete(struct address_space *mapping,
				   struct folio *folio, void *shadow)
{
	XA_STATE(xas, &mapping->i_pages, folio->index);
	long nr = 1;

	mapping_set_update(&xas, mapping);
	xas_set_order(&xas, folio->index, folio_order(folio));
	nr = folio_nr_pages(folio);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	xas_store(&xas, shadow);
	xas_init_marks(&xas);

	folio->mapping = NULL;
	/* Leave page->index set: truncation lookup relies upon it */
	mapping->nrpages -= nr;
}
```

两个细节：

- `xas_store(&xas, shadow)`：**删除时不是简单存 NULL，而是可以存 shadow**（回收路径用它记录「这里曾有页」）。
- `xas_init_marks(&xas)`：清掉该条目的所有 mark（DIRTY/WRITEBACK/TOWRITE）。注意 `page_cache_delete_batch()` 里存 NULL 后**没有**调 `xas_init_marks()`（`mm/filemap.c:315-318`）——批量删除依赖 `delete_from_page_cache_batch()` 前对每个 folio 调 `filemap_unaccount_folio()` 时清理脏页记账。两者是否等价**未逐行验证**。
- `folio->index` 故意保留不清空，因为后续截断查找会用到。

`invalidate_inode_pages2_range()`（`mm/truncate.c:636`）是主动失效（如 NFS 缓存一致性），与 truncate 的区别是要**先写回**（`folio_launder()`，`mm/truncate.c:616`），写回失败则放弃失效并返回错误。

### 3.2 页回收：`shrink_folio_list()`

回收器从 LRU 链尾取页，逐页尝试。函数主体在 `mm/vmscan.c:1035`，骨架：

```c
/* mm/vmscan.c:1035-1070（节选） */
static unsigned int shrink_folio_list(struct list_head *folio_list,
		struct pglist_data *pgdat, struct scan_control *sc,
		struct reclaim_stat *stat, bool ignore_references)
{
	struct folio_batch free_folios;
	...
retry:
	while (!list_empty(folio_list)) {
		...
		folio = lru_to_folio(folio_list);
		list_del(&folio->lru);

		if (!folio_trylock(folio))
			goto keep;                      /* 锁不到：放回，别死等 */
		...
		nr_pages = folio_nr_pages(folio);
		sc->nr_scanned += nr_pages;

		if (unlikely(!folio_evictable(folio)))
			goto activate_locked;
		if (!sc->may_unmap && folio_mapped(folio))
			goto keep_locked;
		folio_check_dirty_writeback(folio, &dirty, &writeback);
```

`folio_trylock` 而非 `folio_lock` 是回收器不阻塞的核心设计：**锁不住说明有人正在操作这页，直接跳过比等待更划算**，可避免回收线程被某个慢 I/O 拖死。

脏/回写状态的判定：

```c
/* mm/vmscan.c:911-941 */
static void folio_check_dirty_writeback(struct folio *folio,
				       bool *dirty, bool *writeback)
{
	/* 匿名页不走 flusher，不因它们阻塞回收 */
	if (!folio_is_file_lru(folio) ||
	    (folio_test_anon(folio) && !folio_test_swapbacked(folio))) {
		*dirty = false;
		*writeback = false;
		return;
	}
	*dirty = folio_test_dirty(folio);
	*writeback = folio_test_writeback(folio);

	/* 有文件系统私有数据时，允许文件系统修正判断 */
	if (!folio_test_private(folio))
		return;
	mapping = folio_mapping(folio);
	if (mapping && mapping->a_ops->is_dirty_writeback)
		mapping->a_ops->is_dirty_writeback(folio, dirty, writeback);
}
```

对 4K 页来说标志位通常准确；有 `private`（buffer_heads）时必须问文件系统，因为块级脏状态可能与页级不一致。

真正的「摘页换空间」在 `__remove_mapping()`：

```c
/* mm/vmscan.c:708-752（节选） */
static int __remove_mapping(struct address_space *mapping, struct folio *folio,
			    bool reclaimed, struct mem_cgroup *target_memcg)
{
	int refcount;
	void *shadow = NULL;

	BUG_ON(!folio_test_locked(folio));
	BUG_ON(mapping != folio_mapping(folio));

	if (!folio_test_swapcache(folio))
		spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	...
	refcount = 1 + folio_nr_pages(folio);
	if (!folio_ref_freeze(folio, refcount))
		goto cannot_free;
	/* note: atomic_cmpxchg in folio_ref_freeze provides the smp_rmb */
	if (unlikely(folio_test_dirty(folio))) {
		folio_ref_unfreeze(folio, refcount);
		goto cannot_free;
	}
```

这里有两个必须理解的点：

- `folio_ref_freeze()` 用 `atomic_cmpxchg` 把计数**冻结成 0**：只有「除了 LRU 隔离引用之外没人引用」才成功。这是回收能安全释放页的前提。
- **先冻结计数、后检查 dirty**，顺序不能反。注释（`mm/vmscan.c:720-744`）给了完整反例：若先查 dirty 再查计数，`get_user_pages` → 用户写 → 释放引用 的并发序列会让「刚写过但还没置脏」的数据被静默丢弃。`folio_ref_freeze` 的 `cmpxchg` 顺带提供了 `smp_rmb` 语义，保证 dirty 标志的读取不会早于计数读取。

只读页（clean）直接删；dirty 页由上层 `pageout()` 先回写：

```c
/* mm/vmscan.c:754-796（节选） */
	if (folio_test_swapcache(folio)) {
		swp_entry_t swap = folio->swap;
		if (reclaimed && !mapping_exiting(mapping))
			shadow = workingset_eviction(folio, target_memcg);
		__delete_from_swap_cache(folio, swap, shadow);
		mem_cgroup_swapout(folio, swap);
		xa_unlock_irq(&mapping->i_pages);
		put_swap_folio(folio, swap);
	} else {
		void (*free_folio)(struct folio *);
		free_folio = mapping->a_ops->free_folio;
		/*
		 * Remember a shadow entry for reclaimed file cache in
		 * order to detect refaults, thus thrashing, later on.
		 */
		if (reclaimed && folio_is_file_lru(folio) &&
		    !mapping_exiting(mapping) && !dax_mapping(mapping))
			shadow = workingset_eviction(folio, target_memcg);
		__filemap_remove_folio(folio, shadow);
		xa_unlock_irq(&mapping->i_pages);
		if (mapping_shrinkable(mapping))
			inode_add_lru(mapping->host);
		spin_unlock(&mapping->host->i_lock);

		if (free_folio)
			free_folio(folio);
	}
```

`shadow` 只在四个条件同时成立时生成：**是回收（不是 truncate）、是 file LRU、mapping 还没销毁、不是 DAX**。这正是 workingset（§4.4）的数据来源。

dirty 页的处理在 `pageout()`：

```c
/* mm/vmscan.c:626-700（节选） */
static pageout_t pageout(struct folio *folio, struct address_space *mapping,
			 struct swap_iocb **plug, struct list_head *folio_list)
{
	if (!is_page_cache_freeable(folio))
		return PAGE_KEEP;
	if (!mapping) {
		if (folio_test_private(folio)) {
			if (try_to_free_buffers(folio)) {
				folio_clear_dirty(folio);
				pr_info("%s: orphaned folio\n", __func__);
				return PAGE_CLEAN;
			}
		}
		return PAGE_KEEP;
	}
	if (mapping->a_ops->writepage == NULL)
		return PAGE_ACTIVATE;

	if (folio_clear_dirty_for_io(folio)) {
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_NONE,
			.nr_to_write = SWAP_CLUSTER_MAX,
			.range_start = 0,
			.range_end = LLONG_MAX,
			.for_reclaim = 1,
			.swap_plug = plug,
		};
```

`WB_SYNC_NONE` + `for_reclaim = 1` 表示「尽力而为、不等待」——回收器不会为了一页去同步落盘，写不成就下轮再说。

### 3.3 `folio_batch` 批量操作

批量是为了摊薄锁与函数调用开销。结构在 `include/linux/pagevec.h`：

```c
/* include/linux/pagevec.h:15,31 */
#define PAGEVEC_SIZE	31
struct folio_batch {
	unsigned char		nr;
	unsigned char		i;
	bool			percpu;
	struct folio		*folios[PAGEVEC_SIZE];
};
```

```c
/* include/linux/pagevec.h:71-76 */
static inline unsigned folio_batch_add(struct folio_batch *fbatch,
		struct folio *folio)
{
	fbatch->folios[fbatch->nr++] = folio;
	return folio_batch_space(fbatch);
}
```

`folio_batch_add()` **返回剩余槽位**，为 0 时调用方立即冲刷——这是 `shrink_folio_list()` 里「攒满一批就批量释放」的写法：

```c
/* mm/vmscan.c:1475-1489 */
free_it:
		nr_reclaimed += nr_pages;
		folio_unqueue_deferred_split(folio);
		if (folio_batch_add(&free_folios, folio) == 0) {
			mem_cgroup_uncharge_folios(&free_folios);
			try_to_unmap_flush();
			free_unref_folios(&free_folios);
		}
		continue;
```

批量删除则分两阶段（这是为缩短临界区）：锁内只做「摘树 + 记账」，锁外才真正释放：

```c
/* mm/filemap.c:323-347 */
void delete_from_page_cache_batch(struct address_space *mapping,
				  struct folio_batch *fbatch)
{
	if (!folio_batch_count(fbatch))
		return;

	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	for (i = 0; i < folio_batch_count(fbatch); i++) {
		struct folio *folio = fbatch->folios[i];
		trace_mm_filemap_delete_from_page_cache(folio);
		filemap_unaccount_folio(mapping, folio);
	}
	page_cache_delete_batch(mapping, fbatch);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_add_lru(mapping->host);
	spin_unlock(&mapping->host->i_lock);

	for (i = 0; i < folio_batch_count(fbatch); i++)
		filemap_free_folio(mapping, fbatch->folios[i]);
}
```

`page_cache_delete_batch()`（`mm/filemap.c:282`）用一次 `xas_for_each()` 遍历整批，并在遇到「index 比预期大」时跳过——说明该 folio 已被别人摘走（`VM_BUG_ON_FOLIO(folio->index > fbatch->folios[i]->index, ...)`，`:306-307`）。

### 3.4 引用计数与 `free_unref_page()` 的下放

page cache 的引用计数是 `folio->_refcount`，语义为「page cache 自身 1 个引用 + 每个基页 1 个引用」（`__remove_mapping` 里 `refcount = 1 + folio_nr_pages(folio)` 印证）。归零路径：

```c
/* mm/swap.c:91-99 */
static void page_cache_release(struct folio *folio)
{
	struct lruvec *lruvec = NULL;
	unsigned long flags;

	__page_cache_release(folio, &lruvec, &flags);
	if (lruvec)
		unlock_page_lruvec_irqrestore(lruvec, flags);
}
/* mm/swap.c:77-85 */
static void __page_cache_release(struct folio *folio, struct lruvec **lruvecp,
		unsigned long *flagsp)
{
	if (folio_test_lru(folio)) {
		folio_lruvec_relock_irqsave(folio, lruvecp, flagsp);
		lruvec_del_folio(*lruvecp, folio);
		__folio_clear_lru_flags(folio);
	}
}
```

```c
/* mm/swap.c:101-120 */
void __folio_put(struct folio *folio)
{
	if (unlikely(folio_is_zone_device(folio))) {
		free_zone_device_folio(folio); return;
	} else if (folio_test_hugetlb(folio)) {
		free_huge_folio(folio); return;
	}
	if (unlikely(folio_test_fcma(folio))) {   /* openEuler CMA folio 扩展 */
		folio_put_cma(folio); return;
	}
	page_cache_release(folio);
	folio_unqueue_deferred_split(folio);
	mem_cgroup_uncharge(folio);
	free_unref_page(&folio->page, folio_order(folio));
}
```

`free_unref_page()` 优先归还 per-CPU 页框缓存（PCP），避免每次都动 zone 锁：

```c
/* mm/page_alloc.c:2695-2740（节选） */
void free_unref_page(struct page *page, unsigned int order)
{
	unsigned long __maybe_unused UP_flags;
	struct per_cpu_pages *pcp;
	struct zone *zone;
	unsigned long pfn = page_to_pfn(page);
	int migratetype;

	if (page_from_dynamic_pool(page)) {       /* openEuler 动态内存池 */
		dynamic_pool_free_page(page);
		return;
	}
	if (!pcp_allowed_order(order)) {
		__free_pages_ok(page, order, FPI_NONE);
		return;
	}
	if (!free_pages_prepare(page, order))
		return;
	...
	zone = page_zone(page);
	pcp_trylock_prepare(UP_flags);
	pcp = pcp_spin_trylock(zone->per_cpu_pageset);
	if (pcp) {
		free_unref_page_commit(zone, pcp, page, migratetype, order);
		pcp_spin_unlock(pcp);
	} else {
		free_one_page(zone, page, pfn, order, FPI_NONE);
	}
	pcp_trylock_finish(UP_flags);
}
```

- `pcp_spin_trylock` **用 trylock 而非 lock**：拿不到就直接走慢路径 `free_one_page()`（进伙伴系统），避免在 CPU 间争抢 PCP 锁时自旋。
- `free_pages_prepare()` 负责校验与清理（含 `PG_` 标志复位、debug 检查），任何「释放时状态不对」的问题都在这里报 `Bad page state`。
- `free_unref_page_commit()`（`mm/page_alloc.c:2644`）把页挂到 `pcp->lists[pindex]`，超过 watermark 时 `free_pcppages_bulk()` 批量归还伙伴系统。

批量版本 `free_unref_folios()`（`mm/page_alloc.c:2747`）在回收与批处理路径用，能按 zone 归组、一次加锁释放多个 folio。

完整释放链：

```
folio_put() / folio_put_refs()
 └─ folio_put_testzero()            引用归零
     └─ __folio_put()               mm/swap.c:101
         ├─ page_cache_release()    摘 LRU（若在链上）
         ├─ mem_cgroup_uncharge()
         └─ free_unref_page()       mm/page_alloc.c:2695
             ├─ pcp 快路径
             └─ __free_pages_ok()/free_one_page()  慢路径
```

---

## 4. 管理流程

### 4.1 lookup：无锁查找协议

`mm/filemap.c:1799-1815` 的注释把协议写得很清楚：

```c
/*
 * Lockless page cache protocol:
 * On the lookup side:
 * 1. Load the folio from i_pages
 * 2. Increment the refcount if it's not zero
 * 3. If the folio is not found by xas_reload(), put the refcount and retry
 *
 * On the removal side:
 * A. Freeze the page (by zeroing the refcount if nobody else has a reference)
 * B. Remove the page from i_pages
 * C. Return the page to the page allocator
 */
```

实现：

```c
/* mm/filemap.c:1831-1874 */
void *filemap_get_entry(struct address_space *mapping, pgoff_t index)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;

	rcu_read_lock();
repeat:
	xas_reset(&xas);
	folio = xas_load(&xas);
	if (xas_retry(&xas, folio))
		goto repeat;
	/*
	 * A shadow entry of a recently evicted page, or a swap entry from
	 * shmem/tmpfs.  Return it without attempting to raise page count.
	 */
	if (!folio || xa_is_value(folio))
		goto out;
	if (!folio_try_get(folio))
		goto repeat;
	if (unlikely(folio != xas_reload(&xas))) {
		folio_put(folio);
		goto repeat;
	}
out:
	rcu_read_unlock();
	return folio;
}
```

三处防御：

- `xas_retry()`：XArray 节点正在被 RCU 释放/重排时返回真，必须重试。
- `xa_is_value()`：shadow/swap 项不是 folio，**不能**加引用，原样返回给上层判断。
- `folio != xas_reload()`：拿引用期间条目可能被替换（同一 index 换了新 folio），必须复核；这是「加引用后重新验证」的经典 ABA 防御。
- `folio_try_get()` 失败说明引用已归零、页正在被释放，重试即可。

上面的 `filemap_get_entry()` 只负责「取到并加引用」。批量版 `filemap_get_read_batch()`（`mm/filemap.c:2359`）在同一个 RCU 临界区里连续取多个相邻 folio，遇到这几种情况就停：超过 `max`、是 value、是 sibling（large folio 的尾页指针）、非 uptodate、带 `PG_readahead`。后者是预读的异步触发点。

带标记的批量查找用于回写（只找脏页）：`filemap_get_folios_tag()`（`mm/filemap.c:2292`）。`find_get_entries`/`find_lock_entries` 用于 truncate 与回收（`mm/filemap.c:2052`/`2101`）。

### 4.2 插入与删除的锁

| 操作 | 锁 | 位置 |
| --- | --- | --- |
| 查树（无锁读） | RCU + refcount 校验 | `mm/filemap.c:1831` |
| 插入 folio | `xas_lock_irq()`（即 `i_pages.xa_lock`） | `mm/filemap.c:876` |
| 删除 folio | `xa_lock_irq()` + `host->i_lock` | `mm/filemap.c:257-258` |
| 打 DIRTY 标记 | `xa_lock_irqsave()` | `mm/page-writeback.c:2687` |
| 打/清 WRITEBACK 标记 | `xas_lock_irqsave()` | `mm/page-writeback.c:3024` |
| 防 truncate 竞争 | `invalidate_lock`（rwsem） | `mm/filemap.c:2532`, `mm/truncate.c` |
| inode 生命期 | `host->i_lock` | `mm/filemap.c:257` |

`i_pages` 的锁就是内嵌在 `struct xarray` 里的 `xa_lock`（`include/linux/xarray.h`），所以源码里 `xa_lock_irq(&mapping->i_pages)` 与 `xas_lock_irq(&xas)` 操作的是同一把锁，只是前者直接、后者通过 state 间接。

`host->i_lock` 的存在理由是：`inode_add_lru()` 可能在 mapping 被销毁（inode 回收）时与删除并发，需要额外保护。

**`invalidate_lock` 与 page lock 的分工**最容易混淆，源码注释给了明确理由（`mm/filemap.c:2519-2532`）：page lock 只能保护「已经插入的页」，不能阻止「truncate 清空缓存 → 释放块 → 又有新页插进来并把它读成 uptodate」这个窗口，所以 `filemap_create_folio()` 必须在整个过程中持 `invalidate_lock` 读锁。

### 4.3 write 路径的 dirty / writeback 标记

**写入内存页**：

```c
/* mm/filemap.c:4070-4122（节选） */
ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
	...
	do {
		bytes = iov_iter_count(i);
retry:
		offset = pos & (chunk - 1);
		bytes = min(chunk - offset, bytes);
		balance_dirty_pages_ratelimited(mapping);   /* 脏页限流 */

		status = a_ops->write_begin(file, mapping, pos, bytes,
						&page, &fsdata);
		...
		folio = page_folio(page);
		copied = copy_folio_from_iter_atomic(folio, offset, bytes, i);
		flush_dcache_folio(folio);
		status = a_ops->write_end(file, mapping, pos, bytes, copied,
						page, fsdata);
```

- `write_begin()` 内部会通过 `__filemap_get_folio(FGP_WRITEBEGIN)` 拿到已锁定的页（`FGP_LOCK|FGP_WRITE|FGP_CREAT|FGP_STABLE`），这就是 §2.3 里 `FGP_STABLE` 的用处：**写之前必须等上一轮回写结束**，否则会写出撕裂的数据。
- `balance_dirty_pages_ratelimited()`（`mm/page-writeback.c:2084`）在超过 dirty 阈值时主动睡眠限流，把「写得太快」的进程压到回写速度，这是 page cache 不会吃光内存的关键机制之一。

**置脏**：

```c
/* mm/page-writeback.c:2785-2807 */
bool folio_mark_dirty(struct folio *folio)
{
	struct address_space *mapping = folio_mapping(folio);

	if (likely(mapping)) {
		/* ... 处理 PG_reclaim 与 folio_end_writeback 的竞争 ... */
		if (folio_test_reclaim(folio))
			folio_clear_reclaim(folio);
		return mapping->a_ops->dirty_folio(mapping, folio);
	}
	return noop_dirty_folio(mapping, folio);
}
```

常规文件系统 `dirty_folio == filemap_dirty_folio`：

```c
/* mm/page-writeback.c:2717-2743 */
bool filemap_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	folio_memcg_lock(folio);
	if (folio_test_set_dirty(folio)) {       /* 已经脏：直接返回，省掉锁 */
		folio_memcg_unlock(folio);
		return false;
	}
	__folio_mark_dirty(folio, mapping, !folio_test_private(folio));
	folio_memcg_unlock(folio);

	if (mapping->host) {
		/* !PageAnon && !swapper_space */
		__mark_inode_dirty(mapping->host, I_DIRTY_PAGES);
	}
	return true;
}
```

```c
/* mm/page-writeback.c:2683-2695 */
void __folio_mark_dirty(struct folio *folio, struct address_space *mapping,
			     int warn)
{
	unsigned long flags;

	xa_lock_irqsave(&mapping->i_pages, flags);
	if (folio->mapping) {	/* Race with truncate? */
		WARN_ON_ONCE(warn && !folio_test_uptodate(folio));
		folio_account_dirtied(folio, mapping);
		__xa_set_mark(&mapping->i_pages, folio_index(folio),
				PAGECACHE_TAG_DIRTY);
	}
	xa_unlock_irqrestore(&mapping->i_pages, flags);
}
```

三个关键设计：

1. **`PG_dirty` 位与 `PAGECACHE_TAG_DIRTY` 标记是两个东西，需要同步维护**。位在 folio，标记在 XArray 节点。`__xa_set_mark` 必须在 `i_pages` 锁内做，因为它要沿途修改节点的 marks 数组。
2. `if (folio->mapping)` 是对 truncate 的防御：`mapping` 已被清空说明页已不在缓存里，此时不应再记账/打标。
3. `folio_account_dirtied()` 把脏页记到 inode 对应的 `bdi_writeback` 上，这是 `/proc/meminfo` 的 `Dirty` 与 cgroup writeback 限流的记账来源。

**开始回写**：

```c
/* include/linux/page-flags.h:808-811 */
#define folio_start_writeback(folio)			\
	__folio_start_writeback(folio, false)
```

```c
/* mm/page-writeback.c:3010-3056（节选） */
bool __folio_start_writeback(struct folio *folio, bool keep_write)
{
	long nr = folio_nr_pages(folio);
	struct address_space *mapping = folio_mapping(folio);
	...
	if (mapping && mapping_use_writeback_tags(mapping)) {
		XA_STATE(xas, &mapping->i_pages, folio_index(folio));
		...
		xas_lock_irqsave(&xas, flags);
		xas_load(&xas);
		ret = folio_test_set_writeback(folio);
		if (!ret) {
			bool on_wblist;
			on_wblist = mapping_tagged(mapping, PAGECACHE_TAG_WRITEBACK);
			xas_set_mark(&xas, PAGECACHE_TAG_WRITEBACK);
			if (bdi->capabilities & BDI_CAP_WRITEBACK_ACCT) {
				...
				wb_stat_mod(wb, WB_WRITEBACK, nr);
				if (!on_wblist)
					wb_inode_writeback_start(wb);
			}
			if (mapping->host && !on_wblist)
				sb_mark_inode_writeback(mapping->host);
		}
		if (!folio_test_dirty(folio))
			xas_clear_mark(&xas, PAGECACHE_TAG_DIRTY);
		if (!keep_write)
			xas_clear_mark(&xas, PAGECACHE_TAG_TOWRITE);
		xas_unlock_irqrestore(&xas, flags);
	} else {
		ret = folio_test_set_writeback(folio);
	}
	if (!ret) {
		lruvec_stat_mod_folio(folio, NR_WRITEBACK, nr);
		zone_stat_mod_folio(folio, NR_ZONE_WRITE_PENDING, nr);
	}
```

`if (!folio_test_dirty(folio)) xas_clear_mark(...DIRTY)` 这一行对应一个微妙的不一致窗口：`folio_clear_dirty_for_io()` 清位但保留标记（注释见 `mm/page-writeback.c:2867-2879`），目的是让并发的 `sync` 还能发现这页；直到 `start_writeback` 才把标记清掉，此时「位与标记」重新一致。

**完成回写**：

```c
/* mm/filemap.c:1607-1637 */
void folio_end_writeback(struct folio *folio)
{
	/* folio_test_clear_reclaim() 会引入不必要的原子操作 */
	if (folio_test_reclaim(folio)) {
		folio_clear_reclaim(folio);
		folio_rotate_reclaimable(folio);     /* 放回 LRU 尾部附近 */
	}
	/*
	 * Writeback does not hold a folio reference of its own, relying
	 * on truncation to wait for the clearing of PG_writeback.
	 * But here we must make sure that the folio is not freed and
	 * reused before the folio_wake().
	 */
	folio_get(folio);
	if (!__folio_end_writeback(folio))
		BUG();
	smp_mb__after_atomic();
	folio_wake(folio, PG_writeback);
	acct_reclaim_writeback(folio);
	folio_put(folio);
}
```

`folio_get()` / `folio_put()` 包住 `folio_wake()`：回写本身不持引用（依赖截断去等 `PG_writeback` 清除），所以唤醒等待者之前必须临时抓一个引用，防止页在 `folio_wake` 执行前被释放并复用。

清理标签在 `__folio_end_writeback()`：

```c
/* mm/page-writeback.c:2965-3008（节选） */
		xa_lock_irqsave(&mapping->i_pages, flags);
		ret = folio_test_clear_writeback(folio);
		if (ret) {
			__xa_clear_mark(&mapping->i_pages, folio_index(folio),
						PAGECACHE_TAG_WRITEBACK);
			if (bdi->capabilities & BDI_CAP_WRITEBACK_ACCT) {
				...
				wb_stat_mod(wb, WB_WRITEBACK, -nr);
				__wb_writeout_add(wb, nr);
				if (!mapping_tagged(mapping, PAGECACHE_TAG_WRITEBACK))
					wb_inode_writeback_end(wb);
			}
		}
		if (mapping->host && !mapping_tagged(mapping, PAGECACHE_TAG_WRITEBACK))
			sb_clear_inode_writeback(mapping->host);
		xa_unlock_irqrestore(&mapping->i_pages, flags);
```

注意 `mapping_tagged(mapping, WRITEBACK)` 在循环里被反复查询：**根节点 marks 的 O(1) 查询使得「该 inode 是否还有页在回写」不必遍历树**，这是 sb/inode writeback 状态标记成本低的原因。

**异步回写由 flusher 内核线程驱动**：

```
wb_workfn()                 fs/fs-writeback.c:2349
 └─ wb_do_writeback()       fs/fs-writeback.c:2318
     ├─ wb_writeback()      fs/fs-writeback.c:2122
     │   └─ do_writepages() mm/page-writeback.c:2564
     │       └─ a_ops->writepages()  或  write_cache_pages() -> ->writepage
     │           └─ folio_clear_dirty_for_io() → folio_start_writeback() → 提交 I/O
     └─ wb_check_old_data_flush() / wb_check_background_flush()  周期性回写
```

`do_writepages()` 的分派：

```c
/* mm/page-writeback.c:2564-2586 */
int do_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	if (wbc->nr_to_write <= 0)
		return 0;
	wb = inode_to_wb_wbc(mapping->host, wbc);
	wb_bandwidth_estimate_start(wb);
	while (1) {
		if (mapping->a_ops->writepages) {
			ret = mapping->a_ops->writepages(mapping, wbc);
		} else if (mapping->a_ops->writepage) {
			struct blk_plug plug;
			blk_start_plug(&plug);
			ret = write_cache_pages(mapping, wbc, writepage_cb, mapping);
			blk_finish_plug(&plug);
		} else {
			ret = 0;    /* 字符设备等无回写需求 */
		}
```

### 4.4 LRU 链表与 workingset（shadow entry）

**上链**是延迟批量的：

```c
/* mm/swap.c:500-521 */
void folio_add_lru(struct folio *folio)
{
	struct folio_batch *fbatch;

	VM_BUG_ON_FOLIO(folio_test_active(folio) &&
			folio_test_unevictable(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	/* see the comment in lru_gen_add_folio() */
	if (lru_gen_enabled() && !folio_test_unevictable(folio) &&
	    lru_gen_in_fault() && !(current->flags & PF_MEMALLOC))
		folio_set_active(folio);

	folio_get(folio);
	local_lock(&cpu_fbatches.lock);
	fbatch = this_cpu_ptr(&cpu_fbatches.lru_add);
	folio_batch_add_and_move(fbatch, folio, lru_add_fn);
	local_unlock(&cpu_fbatches.lock);
}
```

注释点出了设计目的：**「上哪条链」的决定被推迟到批处理真正落地时**，这样调用方有机会在中间调 `folio_mark_accessed()` 把页直接放上 active 链，省掉一次「先上 inactive 再提升」。这就是 `__folio_get_folio(FGP_ACCESSED)` 在分配时就直接 `__folio_set_referenced()` 的原因（`mm/filemap.c:1957-1958`）。

**访问提升**（二次机会）：

```c
/* mm/swap.c:457-496（节选） */
void folio_mark_accessed(struct folio *folio)
{
	if (lru_gen_enabled()) {
		folio_inc_refs(folio);       /* 代际 LRU 用引用计数表示热度 */
		return;
	}
	if (!folio_test_referenced(folio)) {
		folio_set_referenced(folio);     /* 第一次访问：只打标记 */
	} else if (folio_test_unevictable(folio)) {
		/* unevictable 链不轮转，提升无意义 */
	} else if (!folio_test_active(folio)) {
		if (folio_test_lru(folio))
			folio_activate(folio);       /* 第二次访问：升到 active */
		else
			__lru_cache_activate_folio(folio);
		folio_clear_referenced(folio);
		workingset_activation(folio);
	}
	if (folio_test_idle(folio))
		folio_clear_idle(folio);
}
```

**workingset / shadow entry** 是「用极小的空间记住被淘汰的页」的机制，用来识别**抖动（thrashing）**。

淘汰时打包一个影子条目：

```c
/* mm/workingset.c:197-208 */
static void *pack_shadow(int memcgid, pg_data_t *pgdat, unsigned long eviction,
			 bool workingset)
{
	eviction &= EVICTION_MASK;
	eviction = (eviction << MEM_CGROUP_ID_SHIFT) | memcgid;
	eviction = (eviction << NODES_SHIFT) | pgdat->node_id;
	eviction = (eviction << WORKINGSET_SHIFT) | workingset;

	return xa_mk_value(eviction);
}
```

```c
/* mm/workingset.c:383-405（节选） */
void *workingset_eviction(struct folio *folio, struct mem_cgroup *target_memcg)
{
	struct pglist_data *pgdat = folio_pgdat(folio);
	...
	if (lru_gen_enabled())
		return lru_gen_eviction(folio);

	lruvec = mem_cgroup_lruvec(target_memcg, pgdat);
	memcgid = mem_cgroup_id(lruvec_memcg(lruvec));
	eviction = atomic_long_read(&lruvec->nonresident_age);
	eviction >>= bucket_order;            /* 时间戳按桶粗化 */
	workingset_age_nonresident(lruvec, folio_nr_pages(folio));
	return pack_shadow(memcgid, pgdat, eviction,
				folio_test_workingset(folio));
}
```

一个 `unsigned long` 打包了：淘汰时间戳（`nonresident_age` 桶化）、memcg id、NUMA 节点 id、以及「当时是不是工作集」这一位。然后用 `xa_mk_value()` 变成最低位为 1 的 value 条目，**占用原来 folio 的槽位**——这就是它能「几乎零成本」记录淘汰历史的原因。

再次读入时（也就是 §2.4 里 `filemap_add_folio()` 把 `shadow` 传下去的地方）：

```c
/* mm/workingset.c:510-560（节选） */
void workingset_refault(struct folio *folio, void *shadow)
{
	bool file = folio_is_file_lru(folio);
	...
	if (lru_gen_enabled()) { lru_gen_refault(folio, shadow); return; }
	mem_cgroup_flush_stats_ratelimited();
	rcu_read_lock();
	nr = folio_nr_pages(folio);
	memcg = folio_memcg(folio);
	pgdat = folio_pgdat(folio);
	lruvec = mem_cgroup_lruvec(memcg, pgdat);

	mod_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + file, nr);
	...
	if (!workingset_test_recent(shadow, file, &workingset))
		goto out;

	folio_set_active(folio);
	workingset_age_nonresident(lruvec, nr);
	mod_lruvec_state(lruvec, WORKINGSET_ACTIVATE_BASE + file, nr);

	/* Folio was active prior to eviction */
	if (workingset) {
		folio_set_workingset(folio);
		lru_note_cost_refault(folio);
		mod_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + file, nr);
```

逻辑是：**「这页被淘汰后，同一个 memcg/node 里又发生了多少次淘汰」= refault distance**。距离小于可用内存说明「本来放得下」，即**因内存不足而抖动**；此时把页直接放上 active 链并计入 `WORKINGSET_RESTORE`，同时在 `lruvec->refaults[]` 记账，影响后续 file/anon 的回收配比。

**shadow 节点也需要回收**：影子条目本身占 XArray 节点，所以只有「可能产生影子」的 mapping 才被跟踪：

```c
/* mm/internal.h:1510-1516 */
/* Only track the nodes of mappings with shadow entries */
void workingset_update_node(struct xa_node *node);
extern struct list_lru shadow_nodes;
#define mapping_set_update(xas, mapping) do {			\
	if (!dax_mapping(mapping) && !shmem_mapping(mapping)) {	\
		xas_set_update(xas, workingset_update_node);	\
		xas_set_lru(xas, &shadow_nodes);		\
	}							\
} while (0)
```

`__filemap_add_folio()`、`page_cache_delete()`、`page_cache_delete_batch()` 都调了 `mapping_set_update()`（`mm/filemap.c:861`、`137`、`290`），把 XArray 节点的创建/销毁挂到 `shadow_nodes` list_lru 上。DAX 与 shmem 例外（前者只有零页不产生影子，后者影子玩法不同）。

### 4.5 reclaim 与 writeback 的交互

这是整个子系统里最容易出稳定性问题的地方。`shrink_folio_list()` 对 dirty/writeback 页分三种情况（注释 `mm/vmscan.c:1119-1205` 有完整描述）：

| 情形 | 页状态 | 处理 |
| --- | --- | --- |
| 1 | 回写中，且 `PG_reclaim` 已置（第二轮扫到） | 置 `stat->nr_congested`，`activate_locked` 放回；**说明回写跟不上回收** |
| 2 | 回写中，memcg 回收且达到阈值 | 置 `PG_reclaim` 后放回（下一轮会等），避免直接触发 OOM |
| 3 | 回写中，普通情况 | `folio_unlock()` → `folio_wait_writeback()` → 重新放回链表**再试一次** |

```c
/* mm/vmscan.c:1187-1200（节选） */
				folio_set_reclaim(folio);
				stat->nr_writeback += nr_pages;
				goto activate_locked;
			/* Case 3 above */
			} else {
				folio_unlock(folio);
				folio_wait_writeback(folio);
				/* then go back and try same folio again */
				list_add_tail(&folio->lru, folio_list);
				continue;
			}
```

依据是：
- **`PG_reclaim`（与 `PG_readahead` 同一位）在这里当「第二次遇到」的计数器用**。第一轮置位放回，第二轮看到已置位就说明「这页在回写队列里躺了一整圈 LRU」，此时若还无限等待就可能因为存储挂死而无进展，所以改为计入 `nr_congested` 并走节流（`vmscan_throttle`）。
- 情形 3 的 `list_add_tail` + `continue` 是「原地重试」：不解锁直接等，等完再走一遍完整判断。这是同步回收里少见的「阻塞点」，也是直接回收延迟的主要来源之一。

脏页真正落盘走 `pageout()`（§3.2）；回收侧只负责「提交回写请求」和「下次再来」。二者的节流耦合在：

- `balance_dirty_pages_ratelimited()`（`mm/page-writeback.c:2084`）限制**生产者**（写进程）。
- `stat->nr_congested` / `nr_unqueued_dirty` 累积后由 `shrink_folio_list()` 的调用者决定是否唤醒 flusher 或直接 stall（`mm/vmscan.c:1553` 附近的收尾处理）。
- `folio_end_writeback()` 里 `folio_rotate_reclaimable()`（`mm/filemap.c:1615-1617`）把「带 reclaim 标记的完成页」放回 LRU 尾部附近，让回收器尽快看到它，而不是再等一整圈。

### 4.6 回收策略：clean 与 dirty 各自按什么"排序"

先给结论，再给证据：

| 问题 | 答案 |
| --- | --- |
| clean 页回收用什么算法排序？ | 5 条 FIFO 链（active/inactive × anon/file + unevictable）+ **二次机会（second chance）**；排序信号是页表 Accessed 位与 `PG_referenced`，不是时间戳或访问频率 |
| dirty 页怎么排序？ | **LRU 上与 clean 页同链同序**；真正决定落盘先后的是 flusher：inode 按 `dirtied_when` FIFO，页按文件内 index 从 `writeback_index` 轮转；回收器对脏页只"催"不"排" |
| 两者混在一起谁先走？ | 只看 LRU 位置（最久未访问优先），但同样年龄下 clean 页一步即可释放，dirty 页倾向多活一圈 |

#### 4.6.1 前提：本树默认是经典双链 LRU，不是 MGLRU

代码里两套实现都在（大量 `lru_gen_enabled()` 分叉），默认走哪套由配置决定：

```c
/* mm/Kconfig:1337-1350 */
config LRU_GEN
	bool "Multi-Gen LRU"
...
config LRU_GEN_ENABLED
	bool "Enable by default"
	depends on LRU_GEN
```

openEuler defconfig 的实际取值：

```
arch/x86/configs/openeuler_defconfig:1235-1236
CONFIG_LRU_GEN=y
# CONFIG_LRU_GEN_ENABLED is not set

arch/arm64/configs/openeuler_defconfig:1268-1269
CONFIG_LRU_GEN=y
# CONFIG_LRU_GEN_ENABLED is not set
```

`CONFIG_LRU_GEN_ENABLED` 未设时 `lru_gen_enabled()` 是 `static_branch_unlikely`（`include/linux/mm_inline.h` 中两套定义），默认 false，因此**本树出厂配置走 §1.6 的 active/inactive 双链 LRU**。MGLRU 可运行时切换：

```c
/* mm/vmscan.c:5220 enabled_store —— 接受 y/n 或位图 */
	for (i = 0; i < NR_LRU_GEN_CAPS; i++) {
		bool enabled = caps & BIT(i);
		if (i == LRU_GEN_CORE)
			lru_gen_change_state(enabled);
		...
```

即 `echo y > /sys/kernel/mm/lru_gen/enabled`（属性在 `mm/vmscan.c:5247` 注册）。下面 4.6.2–4.6.4 按经典 LRU 描述。

#### 4.6.2 clean 页：5 条 FIFO 链 + 二次机会

"队列"就是那 5 条链表（`include/linux/mmzone.h:275-282`），回收永远从链尾取：

```c
/* include/linux/mm.h:235-238 */
static inline struct folio *lru_to_folio(struct list_head *head)
{
	return list_entry((head)->prev, struct folio, lru);
}
```

```c
/* mm/vmscan.c:1652-1673（节选） */
static unsigned long isolate_lru_folios(unsigned long nr_to_scan, ...)
{
	struct list_head *src = &lruvec->lists[lru];
	...
	while (scan < nr_to_scan && !list_empty(src)) {
		folio = lru_to_folio(src);      /* 链尾 = 最久未被引用 */
```

所以**基础排序是「最近有没有被访问」的 FIFO**，没有时间戳、没有频率计数。精度靠二次机会补，判定在 `folio_check_references()`：

```c
/* mm/vmscan.c:846-851 */
enum folio_references {
	FOLIOREF_RECLAIM,
	FOLIOREF_RECLAIM_CLEAN,
	FOLIOREF_KEEP,
	FOLIOREF_ACTIVATE,
};
```

```c
/* mm/vmscan.c:853-909（节选） */
	referenced_ptes = folio_referenced(folio, 1, sc->target_mem_cgroup, &vm_flags);
	referenced_folio = folio_test_clear_referenced(folio);
	...
	if (referenced_ptes == -1)          /* rmap 锁竞争：本轮先不动 */
		return FOLIOREF_KEEP;
	if (referenced_ptes) {
		folio_set_referenced(folio);    /* 标记，下一圈再看 */
		if (referenced_folio || referenced_ptes > 1)
			return FOLIOREF_ACTIVATE;
		if ((vm_flags & VM_EXEC) && folio_is_file_lru(folio))
			return FOLIOREF_ACTIVATE;
		return FOLIOREF_KEEP;           /* 第一次访问：留在 inactive 再给一圈 */
	}
	if (referenced_folio && folio_is_file_lru(folio))
		return FOLIOREF_RECLAIM_CLEAN;
	return FOLIOREF_RECLAIM;
```

判定表：

| 页表引用（PTE Accessed 汇总） | `PG_referenced` 历史 | 结果 | 效果 |
| --- | --- | --- | --- |
| 0 | 0 | `RECLAIM` | 直接回收 |
| 0 | 1（file 页） | `RECLAIM_CLEAN` | 可回收，同时用于 file/anon 配比统计 |
| ≥1 | 0 且仅 1 个 PTE | `KEEP` | 留在 inactive，**再给一圈** |
| ≥1 | 已置位，或 PTE > 1 | `ACTIVATE` | 升到 active 链 |
| ≥1 | 且 VMA 可执行（file 页） | `ACTIVATE` | 可执行代码优先保留 |

源码注释解释了为什么「第一次访问不算」（`mm/vmscan.c:875-884`）：所有被映射的页在缺页瞬间都天然带一次页表引用，必须看到第二次访问才可信。

**active 链何时参与**——默认只扫 inactive：

```c
/* mm/vmscan.c:2218-2231 */
static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan, ...)
{
	if (is_active_lru(lru)) {
		if (sc->may_deactivate & (1 << is_file_lru(lru)))
			shrink_active_list(nr_to_scan, lruvec, sc, lru);
		else
			sc->skipped_deactivate = 1;
		return 0;
	}
	return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}
```

`may_deactivate` 取决于 inactive 是否「太少」，而「太少」的目标比例随内存规模变化：

```c
/* mm/vmscan.c:2260-2277 */
static bool inactive_is_low(struct lruvec *lruvec, enum lru_list inactive_lru)
{
	...
	gb = (inactive + active) >> (30 - PAGE_SHIFT);
	if (gb)
		inactive_ratio = int_sqrt(10 * gb);
	else
		inactive_ratio = 1;
	return inactive * inactive_ratio < active;
}
```

即目标比例约 `inactive : active = 1 : sqrt(10 × GB)`——小内存接近 1:1，16GB 约 1:4，64GB 约 1:8。只有当 inactive 低于这个比例，才去降级 active 链上的页。

`shrink_active_list()` 的降级也有偏向（`mm/vmscan.c:2113-2126`）：被引用的 active 页里，只有**file 且 VMA 可执行**的才 rotate 回 active，其余一律降到 inactive。这就是「可执行代码在中度内存压力下更抗淘汰」的实现。

clean 页的最后一步不涉及写回：`__remove_mapping` 冻结引用后直接 `__filemap_remove_folio` + 批量 `free_unref_folios`（§3.2）。

#### 4.6.3 dirty 页：LRU 顺序与 clean 完全相同，落盘顺序由 flusher 决定

三个容易误解的点：

**(1) LRU 上脏页和干净页同一个链、同一套排序。** `folio_lru_list()` 只判断 anon/file 与 active，不看脏位（`include/linux/mm_inline.h:87-101`）。脏页的区别只在「回收时不能直接丢」。

**(2) 决定「哪些脏页先落盘」的是 flusher 的两级队列。**

inode 级按 `dirtied_when` FIFO：

```c
/* fs/fs-writeback.c:118-131（节选） */
static bool inode_io_list_move_locked(struct inode *inode,
				      struct bdi_writeback *wb,
				      struct list_head *head)
{
	list_move(&inode->i_io_list, head);   /* 新变脏的 inode 插到 b_dirty 头部 */
```

```c
/* fs/fs-writeback.c:1463-1481（节选） */
static int move_expired_inodes(struct list_head *delaying_queue,
			       struct list_head *dispatch_queue,
			       unsigned long dirtied_before)
{
	while (!list_empty(delaying_queue)) {
		inode = wb_inode(delaying_queue->prev);      /* 尾部 = 最久未回写 */
		if (inode_dirtied_after(inode, dirtied_before))
			break;                                   /* 比截止时间新就停 */
```

`b_dirty` 头新尾旧，`queue_io()`（`fs/fs-writeback.c:1525`）从尾部取，于是**最老的脏 inode 先写**。截止时间两档：

- 周期性 kupdate：`dirtied_before = jiffies - msecs_to_jiffies(dirty_expire_interval * 10)`（`fs/fs-writeback.c:2168-2172`）。`dirty_expire_interval = 30 * 100`（`mm/page-writeback.c:109`，单位厘秒 → **30 秒**；代码里的 `* 10` 是厘秒→毫秒的单位换算，不是放大 10 倍）。
- 后台回写：`dirtied_before = jiffies`，所有脏 inode 都算，但只在超过 background 阈值时才被调度（`wb_over_bg_thresh()`）。

页级是**文件内 index 升序 + 轮转**，与「脏了多久」无关：

```c
/* mm/page-writeback.c:2432-2441（节选） */
	if (wbc->range_cyclic) {
		index = mapping->writeback_index; /* prev offset */
		end = -1;
	} else {
		index = wbc->range_start >> PAGE_SHIFT;
		end = wbc->range_end >> PAGE_SHIFT;
		...
	}
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages) {
		tag_pages_for_writeback(mapping, index, end);
		tag = PAGECACHE_TAG_TOWRITE;
	} else
		tag = PAGECACHE_TAG_DIRTY;
```

- 从 `mapping->writeback_index` 开始按 index 升序扫 `DIRTY` 标记，到文件末尾回绕，下一轮从上次停下的位置继续（`range_cyclic`）——避免每次从文件头开始，也避免大文件尾部饿死。
- `WB_SYNC_ALL`（fsync/sync）先用 `tag_pages_for_writeback()`（`mm/page-writeback.c:2363`）给「当前已脏」的页打 `TOWRITE` 快照，之后只写快照内的页。这是防 livelock 的关键：否则持续写新页的进程会让回写永远无法结束。

**(3) 回收路径遇到脏页：就地问一句「能不能写」，不能就放回——不排序、不排队。**

走 `pageout()`（`WB_SYNC_NONE` + `for_reclaim=1`，见 §3.2）。写不动就 `PAGE_KEEP` / `PAGE_ACTIVATE` 放回。对正在回写的页，用 `PG_reclaim` 记「这是第几圈遇到它」：

```c
/* mm/vmscan.c:1187-1200 */
				folio_set_reclaim(folio);
				stat->nr_writeback += nr_pages;
				goto activate_locked;
			/* Case 3 above */
			} else {
				folio_unlock(folio);
				folio_wait_writeback(folio);
				list_add_tail(&folio->lru, folio_list);
				continue;
			}
```

而 `shrink_inactive_list()` 在发现「扫到的全是未入队脏页」时直接叫醒 flusher：

```c
/* mm/vmscan.c:2017-2018 */
	if (stat.nr_unqueued_dirty == nr_taken) {
		wakeup_flusher_threads(WB_REASON_VMSCAN);
```

所以**回收器对脏页是「催」而不是「排」**：它只负责判断能否立刻释放，落盘调度权在 flusher。

#### 4.6.4 两者混排时谁先走

- 候选顺序只看 LRU 位置：inactive file 链尾最久未被访问的优先，与脏不脏无关。
- 同样是链尾，clean 页一步释放；dirty 页要先发起写回或放回，占用 I/O 且容易进第二轮。**相同引用年龄下 clean 页实际先走，dirty 页倾向多活一圈**。
- clean 页扫完才 deactivate active file；file 页也不够时，才按 `get_scan_count()` 的配比转向 anon。

file/anon 的配比不是固定比例，而是按「回收代价」反向加权：

```c
/* mm/vmscan.c:2480-2505（节选） */
	total_cost = sc->anon_cost + sc->file_cost;
	anon_cost = total_cost + sc->anon_cost;
	file_cost = total_cost + sc->file_cost;
	total_cost = anon_cost + file_cost;

	ap = swappiness * (total_cost + 1);
	ap /= anon_cost + 1;

	fp = (200 - swappiness) * (total_cost + 1);
	fp /= file_cost + 1;
	fraction[0] = ap;
	fraction[1] = fp;
```

`anon_cost` / `file_cost` 由 `lru_note_cost()` 按实际扫描与 pageout 量累积（`mm/vmscan.c:2004`）；某条链越「贵」就越少扫。`swappiness` 只作为 anon 相对 file 的 I/O 代价权重，不是「用不用 swap」的开关。

#### 4.6.5 排序信号一览

| 层次 | 排的是什么 | 依据 | 代码 |
| --- | --- | --- | --- |
| LRU 链 | 谁先被回收 | 链尾优先 + PTE/`PG_referenced` 二次机会 | `mm/vmscan.c:1652`、`:853` |
| active/inactive | 谁被降级 | inactive 低于 `1:sqrt(10×GB)` 才降级 | `mm/vmscan.c:2260` |
| file/anon | 各扫多少 | `anon_cost`/`file_cost` 加权 + swappiness | `mm/vmscan.c:2480` |
| inode（脏） | 哪个文件先写 | `b_dirty` FIFO，最老 `dirtied_when` 优先 | `fs/fs-writeback.c:1463` |
| 页（脏） | 文件内哪段先写 | index 升序，从 `writeback_index` 轮转 + TOWRITE 快照 | `mm/page-writeback.c:2416`、`:2363` |
| 回收中的脏页 | 不排序 | 就地 `pageout`，失败放回，催 flusher | `mm/vmscan.c:626`、`:2018` |


### 4.7 脏页速率的测量与限流（`balance_dirty_pages` 控制环）

先给结论，避免误解：**内核并不去"测量"应用写得多快，而是每 200 ms 采一次「脏页增量」和「回写完成量」，用位置误差（脏页处在阈值带的哪个位置）乘上一个自校正的基准速率，算出每个写进程该睡多久。** 所谓"脏页速率"只在 200 ms 窗口里算出来一次，用来校正基准速率，本身不是限流开关。

#### 4.7.1 计数：速率的两端

产生端，每个 folio 变脏时记一次（`bdi_writeback` 是「块设备 × memcg」粒度）：

```c
/* mm/page-writeback.c:2628-2653（节选）folio_account_dirtied() */
__node_stat_mod_folio(folio, NR_DIRTIED, nr);
wb_stat_mod(wb, WB_RECLAIMABLE, nr);
wb_stat_mod(wb, WB_DIRTIED, nr);
current->nr_dirtied += nr;
__this_cpu_add(bdp_ratelimits, nr);
```

完成端，回写结束时记一次：

```c
/* mm/page-writeback.c:2982-2990（节选）__folio_end_writeback() */
if (bdi->capabilities & BDI_CAP_WRITEBACK_ACCT) {
	struct bdi_writeback *wb = inode_to_wb(inode);
	wb_stat_mod(wb, WB_WRITEBACK, -nr);
	__wb_writeout_add(wb, nr);        /* WB_WRITTEN += nr + bdi 份额 */
```

`__wb_writeout_add()`（`:601-612`）除了累加 `WB_WRITTEN`（`:605`），还会用 `fprop` 记这一份写完成量，用于后面算单设备在全局中的份额（§8.3.1 第 2 点）。

#### 4.7.2 采样：200 ms 一次，两个量

```c
/* mm/page-writeback.c:59,61 */
#define BANDWIDTH_INTERVAL	max(HZ/5, 1)      /* 200 ms */
#define RATELIMIT_CALC_SHIFT	10
```

`__wb_update_bandwidth()`（`:1464-1500`）在每个周期里做两件事，都由 `bw_time_stamp`/`dirtied_stamp`/`written_stamp` 三个游标（`include/linux/backing-dev-defs.h:134-138`）算出增量：

1. **脏页产生速率**——整条链路里唯一的直接"速率测量"：

```c
/* mm/page-writeback.c:1328 */
dirty_rate = (dirtied - wb->dirtied_stamp) * HZ / elapsed;
```

注释（`:1323-1327`）说明了它的局限：长期看它应等于回写速率，但用户态 truncate 丢弃脏页、文件系统重脏化都会让它失真，所以后面还要做滤波。

2. **回写带宽**——`wb_update_write_bandwidth()`（`:1202-1250`），先做 3 秒窗口的加权平均（`:1206-1229`），再叠一层 1/8 平滑滤尖峰（`:1234-1238`），存进 `avg_write_bandwidth`（恒 > 0，`:1242`）。

#### 4.7.3 位置误差：`pos_ratio` 是个三次多项式

```c
/* mm/page-writeback.c:919-929 的设计目标 */
f(dirty) = 1 + ((setpoint - dirty) / (limit - setpoint))^3
(1) f(freerun)  = 2.0   => 低位快速提速
(2) f(setpoint) = 1.0   => 平衡点
(3) f(limit)    = 0     => 硬限
(4) df/dx <= 0          => 负反馈
(5) 越靠近 setpoint 斜率越平 => 大误差快响应、平衡点附近不振荡
```

实现是 `pos_ratio_polynom()`（`:931-946`），`setpoint = (freerun + hard_limit) / 2`，结果 clamp 到 `[0, 2]`（`:945`）。所以脏页在平衡点以下时**允许跑到 2 倍速率**，在硬限处速率归零。

`wb_position_ratio()`（`:1023-1200`）在此之上再叠一层**单设备**控制线：斜率约 `1/(8×write_bw)`（`:1128-1175` 的推导注释），把 `pos_ratio` 按 `wb_dirty` 相对 `wb_setpoint` 的位置缩放，低于 `x_intercept - span/4` 时压到 `pos_ratio/4`（`:1179-1183`）；如果 `wb_dirty` 掉到 `wb_thresh/2` 以下还有一段"reserve 区"反向放大最多 8 倍（`:1190-1197`），防止设备饿死空转。

#### 4.7.4 基准速率自校正：`dirty_ratelimit`

```c
/* mm/page-writeback.c:1367-1373（节选） */
balanced_dirty_ratelimit = div_u64((u64)task_ratelimit * write_bw,
				   dirty_rate | 1);
if (unlikely(balanced_dirty_ratelimit > write_bw))
	balanced_dirty_ratelimit = write_bw;
```

推导见 `:1337-1366` 的注释：若系统里有 N 个写进程、每个被限到 `task_ratelimit`，则 `dirty_rate ≈ N × task_ratelimit`，于是 `balanced ≈ write_bw / N`——即"每个进程应该分到的回写带宽"。注释还解释了为什么不能直接用 `rate × write_bw/dirty_rate`：若同时乘 `pos_ratio`，会卡在任意 `pos_ratio` 上自洽不动（`:1349-1362`）。

拿到目标后**不是直接赋值**，而是按误差方向小步逼近（`:1440-1457`）：`shift = dirty_ratelimit/(2×step+1)`、再 `>>3`，越接近目标步长越小，避免 `dirty_rate` 的 200 ms 采样噪声造成抖动（`:1375-1438` 有完整说明）。

#### 4.7.5 换算成睡眠时间：`pause`

```c
/* mm/page-writeback.c:1875-1891（节选） */
dirty_ratelimit = READ_ONCE(wb->dirty_ratelimit);
task_ratelimit = ((u64)dirty_ratelimit * sdtc->pos_ratio) >> RATELIMIT_CALC_SHIFT;
...
period = HZ * pages_dirtied / task_ratelimit;   /* 这批页本该花的时间 */
pause  = period;
if (current->dirty_paused_when)
	pause -= now - current->dirty_paused_when;  /* 扣掉已睡/已耗时 */
```

`pause` 再被 `min_pause`/`max_pause` 夹住（`:1896-1915`），然后：

```c
/* mm/page-writeback.c:1941-1947（节选） */
if (flags & BDP_ASYNC) { ret = -EAGAIN; break; }
__set_current_state(TASK_KILLABLE);
bdi->last_bdp_sleep = jiffies;
io_schedule_timeout(pause);
```

两个上界值得记住：`MAX_PAUSE = max(HZ/5, 1)`（`:48`，即 200 ms），而 `wb_max_pause()`（`:1549-1566`）会按当前 `wb_dirty` 和回写带宽再收一次，目标是"留下的脏页正好够磁盘忙约 1/8 秒"，避免小内存机器睡太久把磁盘饿空。`wb_min_pause()`（`:1568-1643`）同时反推下一次的 `nr_dirtied_pause`（攒多少页再来一次），下限是 `DIRTY_POLL_THRESH`（`:54`，4 KB 页时 32 页）。

#### 4.7.6 到底限不限流：五个状态

| 状态 | 判据 | `pos_ratio` | 实际行为 | 代码 |
| --- | --- | --- | --- | --- |
| 自由跑 | `dirty ≤ (bg_thresh + thresh)/2` | 不参与 | **完全不休眠**，只设下次检查间隔 | `:1790-1806`、`:1540-1546` |
| 温和限流 | 高于 freerun、低于 `thresh` | 0 ~ 1 | 按 `pause` 睡，脏页越多睡越久 | `:1875-1891` |
| 到硬限 | `dirty ≥ limit`（`thresh` 的上界） | 0 | `task_ratelimit = 0` → 直接睡 `max_pause`，循环重试 | `:1037-1040`、`:1883-1886` |
| 濒临硬限放行 | `wb_dirty ≤ wb_stat_error()` | 0 | 退出循环放行（避免完全卡死） | `:1970-1971` |
| memcg / 慢盘 | 两者取更小 `pos_ratio` | min | 容器或 fuse 类设备被更早限住 | `:1862-1864`、`:1040-1105` |

"到硬限"这一档最容易误解：**不是返回 `-ENOSPC` 也不是把进程挂死**，而是每次最多睡 `max_pause`（≤200 ms）再重试，同时 `io_schedule_timeout()` 让出 CPU 顺便触发 I/O 调度。真正会造成"卡住"观感的是 `dirty_ratelimit` 太小（慢盘）而脏页又一直补不上。

触发频率本身也有第二层限流，否则 1000 个进程同时写就能一起冲过阈值：

- `balance_dirty_pages_ratelimited()`（`:2015-2070`）只在 `current->nr_dirtied ≥ ratelimit` 时才进 `balance_dirty_pages()`；
- 一旦 `wb->dirty_exceeded` 为真，`ratelimit` 被压到 `32 >> (PAGE_SHIFT-10)`（4 KB 页时 **8 页**，`:2032-2034`），检查频率提高一个数量级；
- 每 CPU 的 `bdp_ratelimits` 计数防止单 CPU 累积过多（`:2043-2052`），跨任务的 `ratelimit_pages = dirty_thresh / (num_online_cpus() × 32)`（下限 16，`:2227-2235`）保证总超调不超过约 3%（注释 `:2216-2220`）；
- 退出进程未及记账的脏页通过 `dirty_throttle_leaks` 转嫁给后续任务（`:2055-2059`、`:1988-1995`），否则短命进程（如编译器的 cc1）可以无限逃避限流。

同时，只要越过 `bg_thresh`，这里就会主动唤醒回写线程（`:1808-1810`），让"生产-回写"闭环尽早启动，而不是等到达硬限。

#### 4.7.7 几个坑

1. **`pos_ratio = 0` 不等于脏页立刻下降**：已经在回写队列里的页不受限流影响，限的是"新产生"。
2. **阈值本身会爬**：`dom->dirty_limit` 由 `update_dirty_limit()`（`:1252-1279`）维护——向上一步跟到 `thresh`，向下每次只按 `(limit - thresh) >> 5` 缓慢回落（`:1270-1277`）。所以超载后立刻调小 `dirty_ratio` 不会马上生效，会看到一段振荡。
3. **限流是 per-bdi 的**：`avg_write_bandwidth` 和 `dirty_ratelimit` 都挂在 `bdi_writeback` 上，慢盘不会拖慢快盘，快盘也不会把额度借给慢盘（份额按 `fprop` 写完成比例分，见 §8.3.1）。
4. **观测手段**：`/sys/kernel/debug/bdi/<name>/stats` 直接打印 `BdiDirtied`/`BdiWritten`/`BdiWriteBandwidth`/`BdiDirtyThresh`/`DirtyThresh`/`BackgroundThresh`（`mm/backing-dev.c:78-101`）；想看实时的 `dirty_ratelimit`/`task_ratelimit`/`pause` 就开 `balance_dirty_pages` tracepoint（`:1900`、`:1929`）。注意 debugfs 里的 `BdiWriteBandwidth` 是瞬时值 `write_bandwidth`，不是平滑后的 `avg_write_bandwidth`（`:97` 用的是前者）。
5. **`PF_LOCAL_THROTTLE` 例外**（`:1821-1829`）：cgroup v2 的 writeback 内核线程只按自己 wb 的 freerun 判，不受全局脏页影响。

---

## 5. 本树观察到的重要差异（openEuler OLK-6.6）

以下是读代码时发现的、与本树之外的上游 6.6 可能不同的地方。**对「本树是什么样」的陈述均已读码验证；「与上游不同」的判断未做逐行比对，且本机没有第二份内核源码可对照，因此按差异提示对待。**

### 5.1 `filemap_alloc_folio()` 被 `CONFIG_NUMA` 条件编译

`mm/filemap.c:989-1010` 把整个函数包在 `#ifdef CONFIG_NUMA` 里，非 NUMA 时由 `include/linux/pagemap.h:641-649` 提供 `folio_alloc()` 的直通内联版本。这使 openEuler 的「page cache 本地分配」策略只在 NUMA 配置下生效。

配套的自研文件是 `mm/filemap_local.c`（文件头注明 Huawei，2026），新增了 `sysctl_filemap_alloc_local` 开关与 `filemap_prepare_alloc()` / `filemap_try_alloc_local()`：

```c
/* mm/filemap_local.c:19 */
static int sysctl_filemap_alloc_local __read_mostly;
```

并在 `filemap_alloc_folio()` 里优先于 cpuset spread 与默认分配（`mm/filemap.c:995-998`）。它的语义是「优先在本地、非 cpuless 的 NUMA 节点分配 page cache」，避免远端内存访问。

**影响**：分析 page cache 分配行为时，NUMA 机器上的实际落点由这个 sysctl 决定，不能只看 `numa_node_id()`。

### 5.2 `PAGEVEC_SIZE` 为 31

```c
/* include/linux/pagevec.h:15 */
#define PAGEVEC_SIZE	31
```

批量查找/删除/释放的批大小直接由它决定，会影响 `find_lock_entries()`、`truncate_inode_pages_range()`、`free_unref_folios()` 的批处理粒度与单次持锁时长。**这个 31 是否为 openEuler 相对上游的调整，本机无从比对，未验证**；但它是本树实测值，读代码时按 31 理解批量行为即可。

### 5.3 其他本树特有扩展（影响面较小，供排查参考）

- `__filemap_add_folio()` 内有 large swap/shadow 条目的拆分逻辑（`mm/filemap.c:891-913`），并在 `shmem_mapping()` 上 `BUG_ON`——说明该分支只面向非 shmem 的大型 value 条目。
- `filemap_free_folio()` 会先调 `a_ops->free_folio` 再 `folio_put_refs`（`mm/filemap.c:231-244`），且对 large folio 用 `refs = folio_nr_pages(folio)`，与「每基页一个引用」的计数模型一致。
- `__folio_put()` 增加了 `folio_test_fcma()` 分支（`mm/swap.c:111-114`），对应 `CONFIG_CMA_FOLIO`。
- `free_unref_page()` 增加了 `page_from_dynamic_pool()` 分支（`mm/page_alloc.c:2703-2706`），对应 openEuler 动态内存池（`mm/dynamic_pool.c`）。
- `page-flags.h` 末尾增加了 `PG_pool`、`PG_fcma`、`PG_pswiotlb` 等位（`include/linux/page-flags.h`）。

---

## 6. 核心数据结构清单

| 结构 | 位置 | 作用 |
| --- | --- | --- |
| `struct address_space` | `include/linux/fs.h:500` | 一个 inode 的 page cache 容器；持 `i_pages`、`a_ops`、`gfp_mask`、`invalidate_lock`、`nrpages` |
| `struct inode`（相关字段） | `include/linux/fs.h:691`(`i_mapping`)、`:762`(`i_data`) | `i_mapping` 默认指向 `i_data`，把 inode 与其缓存绑定 |
| `struct xarray` | `include/linux/xarray.h` | 页号→folio/value 的索引；内嵌 `xa_lock` |
| `struct xa_node` | `include/linux/xarray.h` | XArray 树节点，64 槽 + 3 组 mark 位图 |
| `struct folio` | `include/linux/mm_types.h:323` | 物理连续页的头部；持 `flags`/`lru`/`mapping`/`index`/`_refcount` |
| `enum pageflags` | `include/linux/page-flags.h:100` | `PG_locked/uptodate/dirty/writeback/lru/active/workingset/...` |
| `struct lruvec` | `include/linux/mmzone.h` | 每 memcg×node 的 LRU 链集合 + `lru_lock` + `nonresident_age` + `refaults[]` |
| `enum lru_list` | `include/linux/mmzone.h:275` | inactive/active × anon/file + unevictable，共 5 条 |
| `struct folio_batch` | `include/linux/pagevec.h:31` | 批量操作载体（`PAGEVEC_SIZE`=31） |
| `struct address_space_operations` | `include/linux/fs.h` | `read_folio`/`readahead`/`writepage`/`writepages`/`dirty_folio`/`invalidate_folio`/`free_folio` 等回调 |
| `struct file_ra_state` | `include/linux/fs.h` | 预读窗口：`start`/`size`/`async_size`/`prev_pos` |
| `struct readahead_control` | `include/linux/pagemap.h` | 一次预读的上下文（mapping/ra/index/nr_pages/workingset） |
| `struct writeback_control` | `include/linux/writeback.h` | 一次回写的控制参数（`sync_mode`/`nr_to_write`/`for_reclaim`） |
| `struct bdi_writeback` | `include/linux/backing-dev-defs.h` | 每个 BDI/memcg 的回写状态与统计 |

---

## 7. 关键函数索引表

### 7.1 查找与插入

| 函数 | 位置 | 作用 |
| --- | --- | --- |
| `filemap_get_entry` | `mm/filemap.c:1831` | RCU 无锁查找单条目，返回 folio 或 value（shadow/swap） |
| `__filemap_get_folio` | `mm/filemap.c:1878` | 通用入口，按 `FGP_*` 决定加锁/创建/等待稳定 |
| `filemap_get_read_batch` | `mm/filemap.c:2359` | 读路径批量取相邻 folio，遇 readahead 页即停 |
| `find_get_entries` | `mm/filemap.c:2052` | 按范围取条目（含 value），带 refcount |
| `find_lock_entries` | `mm/filemap.c:2101` | 按范围取并加锁，truncate 用 |
| `filemap_get_folios_tag` | `mm/filemap.c:2292` | 按 XArray 标记取（如只取脏页） |
| `filemap_add_folio` | `mm/filemap.c:956` | memcg 计费 + 上锁 + 插入 + 接 LRU |
| `__filemap_add_folio` | `mm/filemap.c:851` | 真正插入 XArray，处理冲突/shadow/统计 |
| `filemap_alloc_folio` | `mm/filemap.c:990` | 分配 folio（NUMA 本地化策略） |
| `page_cache_next_miss` / `prev_miss` | `mm/filemap.c:1746` / `1782` | 找缓存空洞，预读用 |

### 7.2 读与预读

| 函数 | 位置 | 作用 |
| --- | --- | --- |
| `generic_file_read_iter` | `mm/filemap.c:2832` | 缓冲读入口，DIO 直接下传 |
| `filemap_read` | `mm/filemap.c:2647` | 缓冲读主循环（取批→校验→拷贝） |
| `filemap_get_pages` | `mm/filemap.c:2565` | 一个 folio 批的获取编排（命中/预读/新建） |
| `filemap_create_folio` | `mm/filemap.c:2508` | 未命中：分配+插入+读 I/O |
| `filemap_read_folio` | `mm/filemap.c:2395` | 调 `->read_folio` 并等待 uptodate |
| `filemap_update_page` | `mm/filemap.c:2452` | 处理部分 uptodate 的页（补读） |
| `filemap_range_uptodate` | `mm/filemap.c:2428` | 判断部分页范围是否已有效 |
| `page_cache_sync_readahead` | `include/linux/pagemap.h:1404` | 同步预读入口（包装 `page_cache_sync_ra`） |
| `page_cache_sync_ra` | `mm/readahead.c:719` | 同步预读主逻辑（强制预读/按需预读分派） |
| `page_cache_async_ra` | `mm/readahead.c:755` | 异步预读（遇到 `PG_readahead` 页时推进窗口） |
| `ondemand_readahead` | `mm/readahead.c:567` | 按访问模式决定窗口大小与起点 |
| `page_cache_ra_order` | `mm/readahead.c:486` | large folio 预读，按对齐升阶 |
| `page_cache_ra_unbounded` | `mm/readahead.c:201` | 无边界预读（文件系统自用） |
| `do_page_cache_ra` | `mm/readahead.c:281` | 按窗口调用 unbounded |
| `read_pages` | `mm/readahead.c:150` | 批量提交 I/O（blk plug + `->readahead`/`->read_folio`） |
| `get_init_ra_size` / `get_next_ra_size` | `mm/readahead.c:343` / `361` | 预读窗口初始/增长策略 |

### 7.3 写、脏页与回写

| 函数 | 位置 | 作用 |
| --- | --- | --- |
| `generic_perform_write` | `mm/filemap.c:4070` | 缓冲写主循环（write_begin→copy→write_end） |
| `balance_dirty_pages_ratelimited` | `mm/page-writeback.c:2084` | 脏页比例限流（写进程睡眠） |
| `balance_dirty_pages` | `mm/page-writeback.c:1691` | 限流控制环主体：算 `pos_ratio`、`task_ratelimit`、`pause` 并睡眠 |
| `folio_account_dirtied` | `mm/page-writeback.c:2628` | 脏页记账起点（`WB_DIRTIED`/`NR_DIRTIED`/`nr_dirtied`） |
| `__wb_update_bandwidth` | `mm/page-writeback.c:1464` | 每 200 ms 采样脏页增量与写完成量，驱动速率估计 |
| `wb_update_write_bandwidth` | `mm/page-writeback.c:1202` | 回写带宽的 3 秒加权平均 + 1/8 平滑 → `avg_write_bandwidth` |
| `wb_update_dirty_ratelimit` | `mm/page-writeback.c:1306` | 由 `write_bw/dirty_rate` 推出并小步逼近 `dirty_ratelimit` |
| `wb_position_ratio` | `mm/page-writeback.c:1023` | 位置误差 → `pos_ratio`，含单设备控制线与 reserve 区 |
| `pos_ratio_polynom` | `mm/page-writeback.c:931` | 三次多项式：freerun 给 2×、setpoint 给 1×、硬限给 0 |
| `wb_max_pause` / `wb_min_pause` | `mm/page-writeback.c:1549` / `1568` | 睡眠上下界与下次检查间隔（`nr_dirtied_pause`） |
| `dirty_poll_interval` | `mm/page-writeback.c:1540` | 未达阈值时按剩余余量 sqrt 缩放检查间隔 |
| `update_dirty_limit` | `mm/page-writeback.c:1252` | 硬限 `dirty_limit` 的爬升 / 1/32 缓慢回落，抑制振荡 |
| `writeback_set_ratelimit` | `mm/page-writeback.c:2224` | 设跨任务 `ratelimit_pages`（约 3% 超调上限） |
| `folio_mark_dirty` | `mm/page-writeback.c:2785` | 置脏入口，转 `a_ops->dirty_folio` |
| `filemap_dirty_folio` | `mm/page-writeback.c:2717` | 常规文件系统的 `dirty_folio` 实现 |
| `__folio_mark_dirty` | `mm/page-writeback.c:2683` | 在 `i_pages` 锁内打 `DIRTY` 标记并记账 |
| `do_writepages` | `mm/page-writeback.c:2564` | 回写分派（`writepages` 或 `write_cache_pages`） |
| `folio_clear_dirty_for_io` | `mm/page-writeback.c:2881` | 清 `PG_dirty`（保留标记）并记账，准备写回 |
| `__folio_start_writeback` | `mm/page-writeback.c:3010` | 置 `PG_writeback` + `WRITEBACK` 标记，清 `DIRTY`/`TOWRITE` |
| `folio_end_writeback` | `mm/filemap.c:1607` | 回写完成：rotating、清标记、唤醒等待者 |
| `__folio_end_writeback` | `mm/page-writeback.c:2965` | 清 `PG_writeback` 与标记，更新 wb/sb 状态 |
| `wb_workfn` / `wb_do_writeback` / `wb_writeback` | `fs/fs-writeback.c:2349` / `2318` / `2122` | flusher 线程回写任务链 |
| `write_cache_pages` | `mm/page-writeback.c:2416` | 按 index 从 `writeback_index` 轮转扫脏页（`range_cyclic`） |
| `tag_pages_for_writeback` | `mm/page-writeback.c:2363` | 给「当前已脏」的页打 `TOWRITE` 快照，防回写 livelock |
| `move_expired_inodes` / `queue_io` | `fs/fs-writeback.c:1463` / `1525` | 按 `dirtied_when` 从 `b_dirty` 尾部取过期 inode（FIFO 最老优先） |
| `wb_check_old_data_flush` | `fs/fs-writeback.c:2257` | 周期性回写（`dirty_writeback_interval` 每 5 秒检查，30 秒过期） |

### 7.4 删除、截断与回收

| 函数 | 位置 | 作用 |
| --- | --- | --- |
| `filemap_remove_folio` | `mm/filemap.c:253` | 单页摘除 + 释放引用 |
| `__filemap_remove_folio` | `mm/filemap.c:222` | 记账 + 摘树（可存 shadow） |
| `filemap_unaccount_folio` | `mm/filemap.c:152` | 反记账（NR_FILE_PAGES 等）、脏页告警 |
| `page_cache_delete` | `mm/filemap.c:131` | `xas_store(shadow)` + `xas_init_marks` + `nrpages--` |
| `delete_from_page_cache_batch` | `mm/filemap.c:323` | 批量摘除，锁内记账、锁外释放 |
| `page_cache_delete_batch` | `mm/filemap.c:282` | 批量 `xas_store(NULL)` |
| `filemap_free_folio` | `mm/filemap.c:231` | 调 `->free_folio` 后按页数 `folio_put_refs` |
| `truncate_inode_pages_range` | `mm/truncate.c:347` | 按字节范围截断（批量 + 二次确认） |
| `truncate_inode_folio` | `mm/truncate.c:189` | 单 folio 截断 |
| `truncate_inode_partial_folio` | `mm/truncate.c:235` | 部分截断（可能先拆分 large folio） |
| `truncate_cleanup_folio` | `mm/truncate.c:172` | 解映射 + `->invalidate_folio` + 取消脏位 |
| `folio_invalidate` | `mm/truncate.c:153` | 调 `a_ops->invalidate_folio` |
| `invalidate_inode_pages2_range` | `mm/truncate.c:636` | 主动失效（先写回 `folio_launder`） |
| `shrink_folio_list` | `mm/vmscan.c:1035` | 回收主循环（隔离页逐页判定） |
| `folio_check_dirty_writeback` | `mm/vmscan.c:911` | 判定脏/回写状态（可问文件系统） |
| `pageout` | `mm/vmscan.c:626` | 脏页回收：`clear_dirty_for_io` + `->writepage` |
| `__remove_mapping` | `mm/vmscan.c:708` | 冻结引用 + 摘树 + 生成 shadow |
| `folio_isolate_lru` | `mm/vmscan.c:1795` | 从 LRU 摘出单 folio（带引用） |
| `get_scan_count` | `mm/vmscan.c:2399` | 计算 file/anon 各自的扫描量（回收配比） |
| `shrink_list` | `mm/vmscan.c:2218` | 分派：active 链走降级，inactive 链走回收 |
| `shrink_inactive_list` | `mm/vmscan.c:1945` | 隔离 inactive 页 → `shrink_folio_list` → 放回/计数 |
| `shrink_active_list` | `mm/vmscan.c:2063` | active 链降级：被引用页仅 VM_EXEC file 才 rotate 回去 |
| `isolate_lru_folios` | `mm/vmscan.c:1652` | 从 LRU 链尾批量隔离候选页 |
| `folio_check_references` | `mm/vmscan.c:853` | 二次机会判定：`KEEP`/`ACTIVATE`/`RECLAIM(_CLEAN)` |
| `inactive_is_low` | `mm/vmscan.c:2260` | inactive:active 目标比例 `1:sqrt(10×GB)`，决定是否降级 active |

### 7.5 LRU、释放与 workingset

| 函数 | 位置 | 作用 |
| --- | --- | --- |
| `folio_add_lru` | `mm/swap.c:500` | 延迟批量上 LRU |
| `folio_mark_accessed` | `mm/swap.c:457` | 二次机会/代际热度提升 |
| `folio_lru_list` | `include/linux/mm_inline.h:87` | 决定 folio 该上哪条链 |
| `__folio_put` | `mm/swap.c:101` | 引用归零后的统一释放入口 |
| `page_cache_release` | `mm/swap.c:91` | 释放前从 LRU 摘除 |
| `free_unref_page` | `mm/page_alloc.c:2695` | 归还 PCP 或伙伴系统 |
| `free_unref_page_commit` | `mm/page_alloc.c:2644` | 挂入 PCP 链表，超阈值批量归还 |
| `free_unref_folios` | `mm/page_alloc.c:2747` | 批量释放（回收/批处理路径） |
| `folio_batch_add` / `folio_batch_release` | `include/linux/pagevec.h:71` / `80` | 批量装填/释放 |
| `workingset_eviction` | `mm/workingset.c:383` | 淘汰时生成 shadow 条目 |
| `pack_shadow` / `unpack_shadow` | `mm/workingset.c:202` / `211` | shadow 的位打包/解包 |
| `workingset_refault` | `mm/workingset.c:510` | refault 检测：决定是否激活 + 记抖动 |
| `workingset_activation` | `mm/workingset.c:570` | 页被重新激活时的 workingset 记账 |
| `mapping_set_update` | `mm/internal.h:1511` | 把 XArray 节点挂到 `shadow_nodes` 以便回收 |

### 7.6 XArray 底层

| 函数 | 位置 | 作用 |
| --- | --- | --- |
| `xas_load` | `lib/xarray.c:236` | 从游标位置向下查找到叶子条目 |
| `xas_store` | `lib/xarray.c:777` | 插入/替换/删除条目，维护计数与 mark |
| `__xa_set_mark` | `lib/xarray.c:2069` | 先定位再加标记（page cache 打 DIRTY 用） |
| `__xa_clear_mark` | `lib/xarray.c:2079` | 先定位再清标记（清 WRITEBACK 用） |
| `xa_mk_value` / `xa_is_value` | `include/linux/xarray.h:55` / `80` | value 条目编码（最低位 1），区分 shadow/swap 与 folio |

---

## 8. 工程实践：怎样用满 page cache 的价值

> 本节是「怎么用」的建议，前面各节的机制是依据。凡是涉及数字的默认值都来自本树代码；调参建议属于经验判断，已标注。

### 8.1 先确认 page cache 到底有没有在起作用

页码缓存是否命中，看这几个计数器就够（都是本树实现的统计）：

| 指标 | 位置 | 含义 |
| --- | --- | --- |
| `Cached` / `Dirty` / `Writeback` | `/proc/meminfo` | 文件页总量、待回写、正在回写 |
| `workingset_refault_file` / `workingset_activate_file` | `/proc/vmstat` | **refault 是抖动的最强信号**：页被淘汰后又被读回，说明缓存不够用 |
| `pgscan_kswapd` / `pgscan_direct` / `pgsteal_*` | `/proc/vmstat` | 回收压力；`pgscan_direct` 增长说明已进入同步回收（应用会卡） |
| `pgmajfault` | `/proc/vmstat` | 主要缺页，文件页为主时通常是 page cache 未命中 |
| `pgpgin` / `pgpgout` | `/proc/vmstat` | 实际 I/O 量；与业务量对比可判断命中率 |

判读方法：**业务量没涨而 `workingset_refault_file` 持续增长**，说明工作集大于可用内存，此时任何调参收益都有限，真正要做的是减少工作集或加内存；**`pgscan_direct` 增长**说明分配压力已经到了要应用自己回收的地步。

### 8.2 读：让预读窗口匹配访问模式

默认预读窗口是 128 KB，只有 32 个页：

```c
/* include/linux/pagemap.h:1380 */
#define VM_READAHEAD_PAGES	(SZ_128K / PAGE_SIZE)
/* mm/backing-dev.c:997 */
	bdi->ra_pages = VM_READAHEAD_PAGES;
```

对今天的顺序读吞吐来说 128 KB 明显偏小，`fadvise` 能按文件调整：

```c
/* mm/fadvise.c:88-107（节选） */
	case POSIX_FADV_SEQUENTIAL:
		file->f_ra.ra_pages = bdi->ra_pages * 2;   /* 翻倍 */
		file->f_mode &= ~FMODE_RANDOM;
		break;
	case POSIX_FADV_WILLNEED:
		...
		force_page_cache_readahead(mapping, file, start_index, nrpages);
```

```c
/* mm/fadvise.c:85-89 */
	case POSIX_FADV_RANDOM:
		file->f_mode |= FMODE_RANDOM;   /* 预读退化为单页 */
```

实践要点：

1. **顺序读大文件**：打开后立刻 `posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL)`，让 `ra_pages` 翻倍；如果这是主要 IO 类型，再考虑抬高该设备的 `/sys/class/bdi/<dev>/read_ahead_kb`（`mm/backing-dev.c:145` 建立 `ra_pages` 与它的换算）。预读窗口加大后，窗口增长是倍增式的（`get_next_ra_size()` 在 `mm/readahead.c:361`：小于 1/16 上限时 4 倍、小于 1/2 时 2 倍）。
2. **随机读**：一定用 `POSIX_FADV_RANDOM`。否则每次未命中都会预读一整窗，既浪费 I/O 又把无关页塞进缓存（`FMODE_RANDOM` 在 `page_cache_sync_ra()` 里被处理成「只读请求范围」，`mm/readahead.c:722-733`）。
3. **提前预取**：`POSIX_FADV_WILLNEED` 走 `force_page_cache_readahead()`，是真正的「异步拉进来」，适合「已知接下来要读哪些段」的场景（数据加载、启动预热）。
4. **一次性大扫描（备份、杀毒、批量 grep）**：这类访问会把工作集冲掉。推荐「`WILLNEED` 预取 + 读完 `DONTNEED` 丢弃」：

```c
/* mm/fadvise.c:113-121（节选） */
	case POSIX_FADV_DONTNEED:
		__filemap_fdatawrite_range(mapping, offset, endbyte, WB_SYNC_NONE);
		...
```

   注意 `DONTNEED` 会**先发起写回**（`WB_SYNC_NONE`，不等待完成），再 `invalidate_mapping_pages()`。对只读文件没有额外代价；对刚写过的文件，它会触发一次回写风暴——不要在大批量写之后立刻全文件 `DONTNEED`。
5. **大 folio**：本节代码树支持 file mTHP（`file_mthp_enabled()`，`include/linux/huge_mm.h:337`，由 `transparent_hugepage_flags` 的一个位控制），顺序读大文件时把页做大能显著减少 I/O 次数与页级开销。这是 openEuler 的增量特性（对照项：上游 6.6 的 file mTHP 覆盖度更低），开启前建议先测。

### 8.3 写：把 dirty 的抖动压到后台

默认的脏页参数（`mm/page-writeback.c`）：

```c
/* mm/page-writeback.c:74,91,102,109 */
static int dirty_background_ratio = 10;      /* 后台回写启动线：10% */
static int vm_dirty_ratio = 20;              /* 写进程硬限：20% */
unsigned int dirty_writeback_interval = 5 * 100;   /* 5 秒唤醒一次 flusher */
unsigned int dirty_expire_interval = 30 * 100;     /* 30 秒后算「过期」必须回写 */
```

**这两个百分比乘的基数不是总内存，也不是「剩余可用内存」，而是 `global_dirtyable_memory()` 算出的 dirtyable memory：**

```c
/* mm/page-writeback.c:345-364（节选） */
static unsigned long global_dirtyable_memory(void)
{
	x = global_zone_page_state(NR_FREE_PAGES);
	x -= min(x, totalreserve_pages);          /* 扣掉内核/低水位保留页 */
	x += global_node_page_state(NR_INACTIVE_FILE);
	x += global_node_page_state(NR_ACTIVE_FILE);
	...
	return x + 1;                             /* 保证不为 0 */
}
```

即 **基数 = 空闲页 − `totalreserve_pages` + 活跃文件页 + 非活跃文件页**，也就是「现在能直接分配的空闲内存 + 能回收出来的 page cache」。逐条看（每一项的完整推导与算例见 §8.3.1）：

- 不包含匿名页、slab（含可回收 slab）、页表；`vm_highmem_is_dirtyable` 关闭时还扣掉 highmem（`mm/page-writeback.c:360-361`）。
- `totalreserve_pages` 是各 zone `lowmem_reserve` 与水位保留的汇总（`mm/page_alloc.c:6251-6283`），所以这里的「free」不是字面上的 `MemFree`。
- 与 `/proc/meminfo` 的 `MemAvailable` 不等价：后者还估可回收 slab 与 swap，这里只认 file LRU。
- **基数是动态的**：每次进 `balance_dirty_pages()` 循环重算（`mm/page-writeback.c:1721-1725`），`wb_over_bg_thresh()` 也重算（`:2113-2115`）。

因此同一份 `dirty_ratio = 20` 在不同内存构成下对应的绝对量差别很大。示意（非实测）：16 GB 机器上 anon 8 GB、file cache 7 GB、free 0.5 GB，基数约 7.5 GB，硬限约 1.5 GB（≈ 总内存 9%），不是 3.2 GB；反之几乎纯读缓存的服务器基数可接近总内存，硬限也接近 20%。基数里含 file 页还带来一个自反特性：写缓存让 file 页变多，允许的脏页也随之变多；一旦内存吃紧、回收开始扫 file LRU，基数又缩回去。

另有三点容易忽略：

1. **字节参数优先于比例**：`vm.dirty_bytes` / `vm.dirty_background_bytes` 非 0 时直接按字节算，ratio 被忽略（`mm/page-writeback.c:381-383` 先把比例换成 per-PAGE_SIZE 精度，`:408-416` 再按 bytes 优先）。「改 ratio 没反应」通常就是这两个 bytes 被设过。
2. **阈值是三层嵌套的**：全局域 → memcg 域 → 单设备 wb。memcg 域的 avail 是「该 cgroup 的 file 页 + 至多全局剩余 clean 的 headroom」（`mdtc_calc_avail()`，`mm/page-writeback.c:851-859`）；设备份额按历史写完成量比例再乘 `bdi` 的 min/max_ratio（`__wb_calc_thresh()`，`:883-908`），只有带 `BDI_CAP_STRICTLIMIT` 的设备才真正被设备级阈值硬卡。
3. **实际开始限流的点是 `(thresh + bg_thresh) / 2`**（`dirty_freerun_ceiling()`，`mm/page-writeback.c:835-838`），不是 `bg_thresh`；越过 `bg_thresh` 只是唤醒后台回写线程，越过 `thresh` 才是硬睡。脏页计数本身也是全系统的 `NR_FILE_DIRTY + NR_WRITEBACK`（`:1721-1723`），别的进程写脏一样会拖慢你。

写进程到达硬限后不是报错，而是**自己睡**（`balance_dirty_pages_ratelimited()`，`mm/page-writeback.c:2084`，在 `generic_perform_write()` 的每轮循环里被调用，见 §4.3）。所以「写变慢」很多时候不是磁盘慢，而是被 dirty 限流了。

实践要点：

1. **写突发要留余量**：`dirty_background_ratio` 太小会让 flusher 来不及追上写入速率，脏页冲到 20% 硬限后写进程被强制节流。对「短时高速写、随后空闲」的负载，适当提高 background 线比提高硬限更有效（提高硬限只是把节流推迟到更难受的时刻）。
2. **用字节而非比例**（`dirty_background_bytes` / `dirty_bytes`）：内存很大的机器上 20% 是几十 GB，一次 `sync` 可能要写很久；按字节设上限更可控。
3. **`fsync` 的代价随脏页量线性增长**。`fsync` 走 `WB_SYNC_ALL`，会把该 inode 的脏页全部同步写完并等待（§4.3），还要先打 `TOWRITE` 快照。小步写 + 每步 `fsync` 是最差的模式；应批量写、合并 `fsync` 次数，日志类数据用 `fdatasync`（少一次元数据同步）。
4. **`O_SYNC`/`O_DSYNC` 会绕过 page cache 的写合并收益**，每次写都等落盘。除非有强持久化要求，否则用「write + 定期 fsync」更划算。

### 8.3.1 `dirtyable memory` 公式逐项拆解

把 highmem 分支去掉后，公式就是四件事的加减（`mm/page-writeback.c:345-364`）：

```c
/* mm/page-writeback.c:345-364，去掉 highmem 分支 */
avail = NR_FREE_PAGES - min(NR_FREE_PAGES, totalreserve_pages)
      + NR_INACTIVE_FILE + NR_ACTIVE_FILE
      + 1;
```

#### (1) `NR_FREE_PAGES`：真正可分配的空闲页

- 由 `account_freepages()` 维护（`mm/page_alloc.c:666-681`）：页进出 buddy 空闲链表时加减，`is_migrate_isolate()` 的页**直接不计**（被隔离的页既不在空闲链上，也不能拿来做 page cache）。
- 包含 PCP（per-CPU pageset）里的页：从 buddy 批量取到 pcp 仍然算 free，只有真正分配出去才减。
- 包含 CMA 的空闲页，只是另外多记一份 `NR_FREE_CMA_PAGES`（`mm/page_alloc.c:676-677`）——所以不要把「不可用于 page cache」想成全部 CMA。
- **统计是近似的**：全局计数器由每 CPU 的 `vm_stat_diff` 冲量定期回写，超过 `stat_threshold` 才同步（`refresh_zone_stat_thresholds()`，`mm/vmstat.c:272-315`）。`zone_page_state()` 读批量值、`zone_page_state_snapshot()` 才逐个 CPU 补差（`include/linux/vmstat.h:226-257`），限流用的是便宜的前者。误差量级是每 zone 几百页，但脏页计数用的是同一套机制，比较时误差部分抵消。

#### (2) `totalreserve_pages`：为什么不能把空闲全算进去

- `calculate_totalreserve_pages()`（`mm/page_alloc.c:6251-6283`）：对每个 node、每个 zone 取 `max(该 zone 对更高 zone 的 lowmem_reserve) + high watermark`，截到 `managed_pages` 后求和，结果写到全局 `totalreserve_pages`（`:6283`）。
- high watermark 怎么来的：`min_free_kbytes` 按 `managed_pages` 比例分到各 zone 得到 `WMARK_MIN`，然后 `WMARK_LOW = MIN + tmp`、`WMARK_HIGH = LOW + tmp`，其中 `tmp = max(MIN/4, managed × watermark_scale_factor/10000)`（`:6373-6380`，`watermark_scale_factor` 默认 10，`:309`）。单 zone 上典型结果是 `HIGH ≈ 1.5 × MIN`。
- `min_free_kbytes` 不是一个常数：默认 `sqrt(lowmem_kbytes × 16)`，再 clamp 到 128 KB ~ 256 MB（`calculate_min_free_kbytes()`，`:6437-6451`）；16 GB 机器约 16 MB。
- `lowmem_reserve` 的默认比例：DMA/DMA32 = 256、NORMAL = 32、HIGHMEM/MOVABLE = 0（`mm/page_alloc.c:256-269`），即 NORMAL 要给更高 zone 留 1/32（没有更高 zone 时就是 0）。
- 设计意图写在注释里（`mm/page-writeback.c:247-256`）：内核保留的页不能算作「可脏」，否则限流放行的脏页会把 zone 压到高水位以下，回收必须先回写才能满足一次分配——那就成了「为了允许脏页而必须写回」的死结。

#### (3) `NR_INACTIVE_FILE + NR_ACTIVE_FILE`：可回收的 page cache

- 就是 file LRU 两条链的页数，与 `enum lru_list` 前几项同序（`include/linux/mmzone.h:162-170`），folio 换链时按 `NR_LRU_BASE + lru` 记账（`__update_lru_size()`，`include/linux/mm_inline.h:38-50`；读出来用的例子见 `mm/vmscan.c:2267-2268`）。
- **脏页和回写中的页都在这两项里面**（回写完之前它们一直挂在 file LRU 上）——这是下面代数推论的依据。
- **不含** `LRU_UNEVICTABLE`（mlock、ramfs 等被钉住的页），它们记在 `NR_UNEVICTABLE`。
- **不含** 匿名页、可回收 slab、页表；也不含本树 `mm/dynamic_pool.c` / `gmem` 那类特殊内存。

#### (4) `+1` 与 highmem 分支

- `+1` 只是保证函数不返回 0（`:363` 注释 `Ensure that we never return 0`）。
- 32 位 + `CONFIG_HIGHMEM` 且 `vm_highmem_is_dirtyable == 0`（默认）时，再扣掉 highmem 的「free + file」（`highmem_dirtyable_memory()`，`:299-336`；调用点 `:360-361`）。64 位上是死代码。

#### (5) 一个反直觉的推论：20% 实际等于 25%

设 C = 干净 file 页、D = 脏页、W = 回写中页，则 `NR_ACTIVE_FILE + NR_INACTIVE_FILE = C + D + W`，于是

```
avail = (F − R) + C + D + W           // F = 空闲页，R = totalreserve
硬限条件：  D + W ≤ r · avail
=>  (D + W) · (1 − r) ≤ r · (F − R + C)
=>  D + W ≤ r/(1−r) · (F − R + C)
```

`r = 0.2` 时系数是 **0.25**，`bg` 线 `r = 0.1` 时是 **0.111**。两个含义：

1. 观察到的脏页上限相对「空闲 + 干净缓存」其实是 25%，比「总内存 20%」小得多（因为 anon 页不进基数）。
2. 分母里含 D + W，所以脏页在抬高自己的上限——正反馈，但有界（解出来就是上面那个闭式）。这也是脏页量看着「爬到 20% 还在爬」的原因。

#### (6) 算例（示意，不是本机实测）

16 GB、单 node、4 KB 页，`F ≈ 131072` 页、`R ≈ 24 MB ≈ 6144` 页：

| 场景 | anon | file（含脏） | avail | 硬限 20% | bg 10% |
| --- | --- | --- | --- | --- | --- |
| 通用服务器 | 8 GB | 7 GB | ≈ 7.5 GB | ≈ 1.5 GB | ≈ 0.75 GB |
| 纯缓存服务器 | 0.5 GB | 15 GB | ≈ 15.5 GB | ≈ 3.1 GB | ≈ 1.55 GB |

同一份 `vm.dirty_ratio = 20`，两种机器的绝对上限差一倍——这就是「比例参数在不同机器上表现不一致」的根源，也是文档建议大内存机器直接用 `dirty_bytes` 的原因。

#### (7) 在自己的机器上算一遍

```sh
# 当前 sysctl 实际值（注意 bytes 非 0 时 ratio 不生效）
sysctl vm.dirty_ratio vm.dirty_background_ratio \
        vm.dirty_bytes vm.dirty_background_bytes

# 近似计算 avail：R 用各 zone 的 high 水位之和代替（不含 lowmem_reserve）
read -r F A I < <(awk '
  /^nr_free_pages /{f=$2} /^nr_active_file /{a=$2} /^nr_inactive_file /{i=$2}
  END{print f, a, i}' /proc/vmstat)
R=$(awk '/^ *high +[0-9]+$/{h+=$2} END{print h+0}' /proc/zoneinfo)
echo "avail ≈ $((F - (F>R?R:F) + A + I)) 页 = $(( (F - (F>R?R:F) + A + I) * 4096 / 1048576 )) MB"

# 实际脏页量（分子）
awk '/^nr_file_dirty /||/^nr_writeback /' /proc/vmstat
```

两个坑：`totalreserve_pages` 没有对外出口，只能用水位近似，所以算出来会略偏大；`/proc/vmstat` 与内核 `global_zone_page_state()` 读的是同一份批量统计，采样瞬间的误差是正常的。

### 8.4 别让 page cache 被别人悄悄收走

本树有两个容易踩坑的 openEuler 特性：

**(a) 周期性 page cache 限额回收**（`CONFIG_PAGE_CACHE_LIMIT=y`，x86/arm64 defconfig 均开启）：

```c
/* mm/page_cache_limit.c:156-190（节选） */
static struct ctl_table page_cache_limit_table[] = {
	{ .procname = "cache_reclaim_s",      .data = &vm_cache_reclaim_s,      ... },
	{ .procname = "cache_reclaim_weight", .data = &vm_cache_reclaim_weight, ... },
	{ .procname = "cache_reclaim_enable", .data = &vm_cache_reclaim_enable, ... },
	{ .procname = "cache_limit_mbytes",   .data = &vm_cache_limit_mbytes,   ... },
};
```

默认值：`cache_reclaim_s = 0`（关闭周期回收）、`cache_limit_mbytes = 0`（不设上限）、`cache_reclaim_enable = 1`。由于前两者为 0，**默认不会主动收缩 page cache**（`should_reclaim_page_cache()` 两个条件都不满足，`mm/page_cache_limit.c:50-59`）。但如果运维设了 `vm.cache_limit_mbytes`，系统就会周期性把 file LRU 压到该上限以下（`shrink_shepherd()` → `shrink_memory()`，`mm/page_cache_limit.c:125-146`）。

**排查「缓存怎么老是上不去」时，先查这几个 sysctl 是否被设过。**

**(b) NUMA 本地分配**（§5.1）：`sysctl_filemap_alloc_local` 打开后，page cache 优先落在本地节点，跨节点访问延迟会明显改善；默认关闭。

### 8.5 常见反模式

| 反模式 | 为什么坏 | 对应机制 |
| --- | --- | --- |
| 顺序读大文件却不设 `SEQUENTIAL`，也不事后 `DONTNEED` | 默认 128 KB 窗口读不满带宽；扫描完把工作集冲掉，触发 refault | `mm/fadvise.c:88`、`:113` |
| 随机读却把 `read_ahead_kb` 调很大 | 每次未命中都预读整窗，纯浪费 IO 和内存 | `mm/readahead.c:722` |
| 对同一文件混用 `O_DIRECT` 与 buffered IO | 两套路径对同一区段各自缓存/绕过，语义与性能都难预测 | `mm/filemap.c:2840`（`IOCB_DIRECT` 直下 `->direct_IO`） |
| 小步写 + 每步 `fsync` | 每次 `WB_SYNC_ALL` 全量同步该 inode，代价随脏页线性涨 | `mm/page-writeback.c:2416` 起 |
| 大批量写后立刻全文件 `POSIX_FADV_DONTNEED` | `DONTNEED` 会先触发 `WB_SYNC_NONE` 回写 | `mm/fadvise.c:113-115` |
| 把 page cache 当持久化（写完不 `fsync` 就认为落盘） | 脏页只在内存，掉电丢失 | `PG_dirty` 语义，§1.5 |
| 以为「读过的文件一定还在缓存」 | clean 页在内存压力下可被无成本丢弃，这是设计目标而非缺陷 | §4.6.2 |

### 8.6 一句话总结

**page cache 的收益来自「顺序性」和「复用性」**：让它看到顺序访问（放大预读、合并写），让它看到复用（别用一次性扫描冲掉工作集），并保证脏页的产出速率低于回写速率（dirty 限流不要长期贴着硬限）。它**不提供**持久化保证，也**不保证**命中——命中率只由「工作集大小 vs 可用内存」决定，调参只能改变边缘效率。

---

## 9. 分析结论与阅读建议

### 9.1 三条贯穿全局的线索

1. **「引用计数 + 加锁后复核」是 page cache 无锁读的正确性基石。** `filemap_get_entry()` 的 `try_get` → `xas_reload` → 失败重试（`mm/filemap.c:1849-1855`）与 `__remove_mapping()` 的 `folio_ref_freeze` 必须成对理解：前者允许「猜着读」，后者保证「猜错时能被发现」。看任何 page cache 相关的并发 bug，先看这两处。

2. **`PG_dirty` 位、`PG_writeback` 位与 XArray 的 tag 是两套状态，同步点很集中。** 置位在 `__folio_mark_dirty` / `__folio_start_writeback`，清位在 `__folio_end_writeback`，且都持 `i_pages` 锁。中间存在「位已清、标记未清」的窗口（`folio_clear_dirty_for_io` 之后），是 sync/回写竞态的主要来源。

3. **回收与回写通过「标记 + LRU 轮转」松耦合。** 回收器不阻塞等 I/O（用 `trylock`、`WB_SYNC_NONE`、`PG_reclaim` 计数），回写器不关心 LRU 位置（只在 `folio_end_writeback` 里 `folio_rotate_reclaimable`）。两者之间只有脏页限流（`balance_dirty_pages_ratelimited`）与节流统计作为反馈，所以**存储变慢时的表现是「回收变慢 + 直接回收 stall」，而不是报错**。

### 9.2 值得进一步验证的点（本次未构造用例）

| 点 | 位置 | 为什么值得看 |
| --- | --- | --- |
| `page_cache_ra_order` 的 `while (... > limit) order--` 下溢 | `mm/readahead.c:526-527` | 若 `limit` 计算使得最小 order 仍越界，`order` 会变成 `UINT_MAX`；需确认调用方保证 `index <= limit`。**未验证** |
| `page_cache_delete_batch` 未调 `xas_init_marks` | `mm/filemap.c:315-318` | 与单页版 `page_cache_delete` 的 `xas_init_marks` 不对称；是否由 `xas_store(NULL)` 隐式清理，需按 XArray 语义确认。**未验证** |
| `__filemap_add_folio` 的 large value 拆分分支 | `mm/filemap.c:891-913` | 内嵌 `BUG_ON(shmem_mapping(mapping))`，说明该路径有前置假设；触发条件与 mTHP/swap 配置相关 |
| NUMA 本地分配策略的默认值与效果 | `mm/filemap_local.c:19` | `sysctl_filemap_alloc_local` 默认 0（关闭），行为与上游一致；开启后的落点策略需单独评估 |

### 9.3 想快速上手这块代码的阅读顺序

1. `include/linux/fs.h:500`（`address_space`）→ `include/linux/mm_types.h:323`（`folio`）→ `include/linux/page-flags.h:100`（标志位）。先建立「一个文件 → 一个 mapping → 树里一堆 folio」的心智模型。
2. `mm/filemap.c:1878`（`__filemap_get_folio`）：所有路径的公共入口，`FGP_*` 语义搞清楚就懂了一半。
3. `mm/filemap.c:2647`（`filemap_read`）→ `:2565`（`filemap_get_pages`）→ `:2508`（`filemap_create_folio`）：把「命中/预读/新建」三条分支走通。
4. `mm/readahead.c:567`（`ondemand_readahead`）：预读窗口怎么增长，`PG_readahead` 怎么触发下一次。
5. `mm/vmscan.c:1035`（`shrink_folio_list`）→ `:708`（`__remove_mapping`）→ `mm/workingset.c:202`（`pack_shadow`）：回收如何摘页、如何留痕。
6. `mm/page-writeback.c:2785`（`folio_mark_dirty`）→ `:3010`（`__folio_start_writeback`）→ `mm/filemap.c:1607`（`folio_end_writeback`）：一套脏/回写状态机完整闭环。

---

## 10. 附录：ext4 与 XFS 的差异（page cache / 回写视角）

> 本节只陈述在本树（`8003dfcde32f`）读到的实现，不引用发行版文档或厂商白皮书。两者都是日志文件系统，真正的分野不在"有没有日志"，而在**页缓存之上那层 IO 框架**：ext4 默认仍走 `buffer_head`，XFS 走 `iomap`。这一层决定了 folio 粒度、回写单元和延迟分配的实现形态。

### 10.1 根本差异：`buffer_head` vs `iomap`

| 维度 | ext4（默认路径） | XFS |
| --- | --- | --- |
| `a_ops` 注册 | `fs/ext4/inode.c:4454-4469`（`ext4_aops`）、`:4488-4502`（`ext4_da_aops`） | `fs/xfs/xfs_aops.c:545-557` |
| `read_folio` / `readahead` | `ext4_read_folio`（`:3307`）/ `ext4_readahead`（`:3323`），buffer_head 逐页 | `xfs_vm_read_folio` / `xfs_vm_readahead`（转为 iomap） |
| `writepages` | `ext4_writepages`（`:2934`） | `xfs_vm_writepages`（`:548`，走 iomap） |
| `write_begin` / `write_end` | 有：`ext4_write_begin`/`ext4_write_end`；delalloc 用 `ext4_da_write_begin`（`:3035`）/`ext4_da_write_end`（`:3194`） | **没有**，缓冲写整体交给 `iomap_file_buffered_write()`（`fs/xfs/xfs_file.c:781`） |
| `dirty_folio` | `ext4_dirty_folio`（`:4440-4445`）→ `block_dirty_folio()` | `iomap_dirty_folio`（`:549`） |
| `migrate_folio` | `buffer_migrate_folio` / `buffer_migrate_folio_norefs` | `filemap_migrate_folio`（`:553`） |
| 大 folio（file mTHP） | 默认不开启 | 每个 inode 无条件开启 |

大 folio 这一项是两边 `address_space` 配置的直接差别：

```c
/* fs/xfs/xfs_icache.c:91（xfs_inode_alloc() 内） */
mapping_set_large_folios(VFS_I(ip)->i_mapping);
```

而 iomap 的缓冲读正是用这个标志决定能否跨页建 folio：

```c
/* fs/iomap/buffered-io.c:886-887 */
if (!mapping_large_folio_support(iter->inode->i_mapping))
	len = min_t(size_t, len, PAGE_SIZE - offset_in_page(pos));
```

**本树有一个必须说明的点**：ext4 在这里已经有 iomap 版的 `a_ops`（`ext4_iomap_aops`，`fs/ext4/inode.c:4505-4520`），由一个 mount 选项切换：

```c
/* fs/ext4/super.c:1906-1907、2002-2005 */
fsparam_flag("buffered_iomap",   Opt_buffered_iomap),
fsparam_flag("nobuffered_iomap", Opt_nobuffered_iomap),
{Opt_buffered_iomap, EXT4_MOUNT2_BUFFERED_IOMAP, MOPT_SET | MOPT_2 | MOPT_EXT4_ONLY},
```

选择逻辑：

```c
/* fs/ext4/inode.c:4532-4548 ext4_set_aops() */
if (IS_DAX(inode))
	inode->i_mapping->a_ops = &ext4_dax_aops;
else if (ext4_test_inode_state(inode, EXT4_STATE_BUFFERED_IOMAP))
	inode->i_mapping->a_ops = &ext4_iomap_aops;
else if (test_opt(inode->i_sb, DELALLOC))
	inode->i_mapping->a_ops = &ext4_da_aops;
else
	inode->i_mapping->a_ops = &ext4_aops;
```

打开该选项后，普通文件才切到 iomap 并启用大 folio（`fs/ext4/inode.c:6083-6085`），且有排除条件：`inline_data` 特性、`verity` 特性、`data=journal`、非普通文件、DAX 都不走（`fs/ext4/inode.c:5785-5800+`）。所以**「ext4 不支持大 folio」在本树要加限定**：默认不支持，`-o buffered_iomap` 才支持，默认值仍是关闭。

### 10.2 从 IO 框架衍生的 page cache 行为差异

| 行为 | ext4 默认 | XFS |
| --- | --- | --- |
| 缓存页的 folio 粒度 | 恒为 order-0（一页一组 buffer_head） | 可达 order-N（受 mTHP 开关与文件对齐约束） |
| 每页附加元数据 | `struct buffer_head` 数组（`folio_buffers()`），脏/uptodate 按块维护 | 无 buffer_head，iomap 直接给区间 |
| 回写聚合单元 | `ext4_writepages` 按 extent 聚合成 bio，但每页仍参与 buffer_head 状态机 | iomap 构造 `iomap_ioend` 链（`fs/xfs/xfs_aops.c:23-27` 的 `xfs_writepage_ctx`），一次覆盖更长区间 |
| `i_size` 更新时机 | 写系统调用返回前，持 folio 锁更新（`ext4_da_do_write_end()`，`fs/ext4/inode.c:3147-3153` 注释） | **延迟到 IO 完成**（`xfs_setfilesize()`，`fs/xfs/xfs_aops.c:48` 起） |
| 延迟分配 | 有，`delalloc` 默认开启（`fs/ext4/super.c:4504-4508`：除非 `nodelalloc`） | 有，且分配/预分配由 iomap 层发起（`xfs_iomap_prealloc_size()`，`fs/xfs/xfs_iomap.c:400`，调用点 `:1103`） |
| 探测性预分配 | `mballoc` 的 prealloc（按块组） | 显式的 speculative preallocation（按文件增长速率动态调整，`fs/xfs/xfs_iomap.c:400-533`） |

一句话：**ext4 的 page cache 操作是「页 + buffer_head」粒度，XFS 是「folio + iomap 区间」粒度。** 顺序大 IO 上，XFS 天然能把多个页合成一个 folio、一次提交；ext4 默认只能逐页推进，这在 §8.2 讨论过的 file mTHP 上就是"用不上"的状态。

### 10.3 空间分配器

ext4 的核心是**块组（block group）+ mballoc 伙伴分配器**：

- 分配入口 `ext4_mb_new_blocks()`（`fs/ext4/mballoc.c:6325`）；
- buddy 信息挂在 `s_buddy_cache` 这个 inode 上，按「组 0 位图+伙伴 / 组 1 位图+伙伴 …」布局（`fs/ext4/mballoc.c:91-101` 的注释）；
- 文件映射用 extent 树（`ext4_ext_insert_extent()`，`fs/ext4/extents.c:2008`），树深在 `eh_depth`（`fs/ext4/ext4_extents.h:89`，读取见 `ext_depth()` `:192`）。

XFS 的核心是**分配组（AG）+ B+ 树**：

- 每个 AG 有自身的空闲空间 B+ 树（bnobt/cntbt，`fs/xfs/libxfs/xfs_alloc_btree.c:385`、`:407`）与 inode B+ 树（`fs/xfs/libxfs/xfs_ialloc_btree.c`）；
- inode 分配按 AG 走，且区分 finobt 的 near/newino 策略（`fs/xfs/libxfs/xfs_ialloc.c:1279`、`:1367`）；
- 文件映射是 bmap B+ 树（`fs/xfs/xfs_bmap.c`），区段记录数上限见 `XFS_MAX_EXTCNT_DATA_FORK_LARGE`（`fs/xfs/libxfs/xfs_format.h:940`）。

差异的工程含义：

- ext4 的元数据更新集中在少数块组描述符上，短事务、低开销，但在超大目录/超高并发下容易出现组内争用；
- XFS 把空间与 inode 摊到多个 AG，天然可并行，代价是元数据更分散、单次事务涉及更多 B+ 树操作；
- ext4 在线扩容靠 `resize_inode`（`fs/ext4/resize.c`）；XFS 只有 growfs（`fs/xfs/xfs_ioctl.c:2078-2092` 的 `XFS_IOC_GROWFS_DATA`/`_LOG`），**不支持在线缩容**。

### 10.4 日志与 fsync

| 维度 | ext4 | XFS |
| --- | --- | --- |
| 日志实现 | `jbd2`，事务提交 `jbd2_journal_commit_transaction()`（`fs/jbd2/commit.c:348`） | 环形 log + CIL 延迟日志，`xlog_cil_commit()`（`fs/xfs/xfs_log_cil.c:1734`）；授权用 grant head（`fs/xfs/xfs_log.c:134`） |
| 数据日志模式 | 支持 `data=ordered/journal/writeback`（`fs/ext4/super.c:4482-4488`） | **没有**数据日志模式（`fs/xfs/` 里搜不到 `data=journal` 之类选项） |
| fsync | `ext4_sync_file()`（`fs/ext4/fsync.c:141`）→ `jbd2_journal_force_commit()` | `xfs_file_fsync()`（`fs/xfs/xfs_file.c:132`）→ `xfs_log_force_seq()`（`:120`） |
| 快速提交 | 可选 fast commit（`fs/ext4/super.c:4475-4476` 打开 `JOURNAL_FAST_COMMIT`，实现见 `fs/ext4/fast_commit.c`） | 无对应机制，靠 CIL 批量 + log force |

对 page cache 的影响：**两者的回写都是"页缓存 → 块设备"的异步过程，日志只管元数据（XFS）或元数据+可选数据（ext4）。** 所以 `fsync` 的代价模型不同：ext4 需要等 jbd2 提交 + 数据回写（ordered 模式下还要保证数据先于元数据落盘）；XFS 等 log 到指定 LSN + 数据写回，靠 CIL 把多个事务合并成一次提交。

### 10.5 inode 号与目录结构

- **inode 号位宽**：ext4 的目录项里 inode 是 `__le32`（`struct ext4_dir_entry_2`，`fs/ext4/ext4.h:2361-2362`），即 32 位；XFS 是 `typedef unsigned long long xfs_ino_t`（`fs/xfs/xfs_linux.h:17`），64 位。这直接决定单文件系统能容纳的 inode 数量级。
- **目录**：ext4 小目录用线性目录块，大目录转 htree（`dx_probe()`，`fs/ext4/namei.c:304`；`ext4_dx_add_entry()`，`:2539`）；XFS 三级形态：shortform（`xfs_dir2_sf_addname()`，`fs/xfs/libxfs/xfs_dir2_sf.c:364`）→ block（`xfs_dir2_block.c`）→ leaf/node（`xfs_dir2_leaf.c`、`xfs_dir2_node.c`）。XFS 的 shortform 把小目录直接塞进 inode，省一次 IO；ext4 的小目录仍需单独的目录块。
- **inode 分配**：ext4 用 inode 位图 + inode 表（`fs/ext4/ialloc.c`）；XFS 用 inode chunk（64 个一组，`XFS_INODES_PER_CHUNK`，`fs/xfs/libxfs/xfs_format.h:1415`）+ inode B+ 树（`fs/xfs/libxfs/xfs_ialloc.c`）。

### 10.6 锁模型

- ext4：`i_rwsem` 保护写路径，`i_data_sem`（`fs/ext4/ext4.h:1109` 附近注释）序列化 truncate 与块分配，`mballoc` 另有按组锁。分配与截断在同一把读写信号量下排队。
- XFS：`xfs_ilock()` 提供 SHARED/EXCL 多级（`fs/xfs/xfs_inode.c:186`），配合 per-AG 锁与 CIL 的批量化提交，元数据并发度更高。

这解释了一个常见观感：**大量线程同时写同一目录/同一文件系统元数据时，XFS 的伸缩性通常更好**；而单线程顺序写大文件时，两者差距主要来自 IO 框架（iomap 的大 folio 与批量提交），不是锁。

### 10.7 上限与特性对照

| 项目 | ext4（本树，4 KB 块） | XFS |
| --- | --- | --- |
| `s_maxbytes` | `((1LL<<32)-1) << blkbits`，即 **16 TiB − 4 KB**（`ext4_max_size()`，`fs/ext4/super.c:3520-3551`；赋值 `:5324`） | `MAX_LFS_FILESIZE` = `LLONG_MAX` ≈ **8 EiB**（`fs/xfs/xfs_super.c:1703`、`include/linux/fs.h:1101`） |
| inode 号 | 32 位 | 64 位 |
| reflink / CoW | 无 | 有（`fs/xfs/xfs_reflink.c`） |
| 在线扩容 | 有（`fs/ext4/resize.c`） | 有（growfs） |
| 在线缩容 | 无 | 无 |
| 数据日志模式 | 有（ordered/writeback/journal） | 无（只记元数据） |
| 大 folio（缓冲 IO） | 默认无，`-o buffered_iomap` 才有 | 默认有 |

### 10.8 选型与调优建议

1. **默认用 ext4**：单机通用场景、小文件多、需要在线扩容、生态工具成熟（`e2fsck`/`resize2fs`/`debugfs`），且故障恢复路径被验证得最充分。
2. **选 XFS**：大文件、高并发元数据、海量目录项、需要 reflink（快照/克隆/去重）、单文件可能超过 16 TiB。
3. **从 page cache 角度**：XFS 默认就吃得到 iomap 的大 folio 与批量回写；ext4 默认是 4 KB 粒度，若本树 `-o buffered_iomap` 在你的内核/工具链上稳定，可作为实验选项评估（去掉 `verity`/`inline_data`/`data=journal` 才有机会生效）。这是**实验特性**，生产环境先做回归。
4. **fsync 密集负载**：ext4 开 fast commit（需文件系统特性支持，`fs/ext4/super.c:4475-4476`）能显著降低提交次数；XFS 侧靠 CIL 批量，调优空间主要在日志大小与 `logbsize`（未在本文验证具体参数效果）。
5. **不要用「哪个更快」一句话选型**：差异主要落在**并发元数据**与**大文件顺序 IO** 两个维度，其余场景两者都够用。

> **未验证项**：XFS 的 AG 数量/日志大小对具体负载的影响、ext4 fast commit 的实际收益幅度、`-o buffered_iomap` 的稳定性——这三项均需实测，本文只做代码层判断。

---

*文档生成时间：2026-10-29。源码状态：`8003dfcde32f`（openEuler OLK-6.6 / kernel 6.6.0）。本文未执行编译或运行测试，所有结论均来自静态阅读；标注「未验证」处为待确认项。*
