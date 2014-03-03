/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_PAGE_HEADER --
 *	Blocks have a common header, a WT_PAGE_HEADER structure followed by a
 * block-manager specific structure.
 */
struct __wt_page_header {
	/*
	 * The record number of the first record of the page is stored on disk
	 * so we can figure out where the column-store leaf page fits into the
	 * key space during salvage.
	 */
	uint64_t recno;			/* 00-07: column-store starting recno */

	/*
	 * We maintain page write-generations in the non-transactional case
	 * as that's how salvage can determine the most recent page between
	 * pages overlapping the same key range.
	 */
	uint64_t write_gen;		/* 08-15: write generation */

	/*
	 * The page's in-memory size isn't rounded or aligned, it's the actual
	 * number of bytes the disk-image consumes when instantiated in memory.
	 */
	uint32_t mem_size;		/* 16-19: in-memory page size */

	union {
		uint32_t entries;	/* 20-23: number of cells on page */
		uint32_t datalen;	/* 20-23: overflow data length */
	} u;

	uint8_t type;			/* 24: page type */

#define	WT_PAGE_COMPRESSED	0x01	/* Page is compressed on disk */
#define	WT_PAGE_EMPTY_V_ALL	0x02	/* Page has all zero-length values */
#define	WT_PAGE_EMPTY_V_NONE	0x04	/* Page has no zero-length values */
	uint8_t flags;			/* 25: flags */

	/*
	 * End the structure with 2 bytes of padding: it wastes space, but it
	 * leaves the structure 32-bit aligned and having a few bytes to play
	 * with in the future can't hurt.
	 */
	uint8_t unused[2];		/* 26-27: unused padding */
};
/*
 * WT_PAGE_HEADER_SIZE is the number of bytes we allocate for the structure: if
 * the compiler inserts padding it will break the world.
 */
#define	WT_PAGE_HEADER_SIZE		28

/*
 * The block-manager specific information immediately follows the WT_PAGE_HEADER
 * structure.
 */
#define	WT_BLOCK_HEADER_REF(dsk)					\
	((void *)((uint8_t *)(dsk) + WT_PAGE_HEADER_SIZE))

/*
 * WT_PAGE_HEADER_BYTE --
 * WT_PAGE_HEADER_BYTE_SIZE --
 *	The first usable data byte on the block (past the combined headers).
 */
#define	WT_PAGE_HEADER_BYTE_SIZE(btree)					\
	((u_int)(WT_PAGE_HEADER_SIZE + (btree)->block_header))
#define	WT_PAGE_HEADER_BYTE(btree, dsk)					\
	((void *)((uint8_t *)(dsk) + WT_PAGE_HEADER_BYTE_SIZE(btree)))

/*
 * WT_ADDR --
 *	An in-memory structure to hold a block's location.
 */
struct __wt_addr {
	uint8_t *addr;			/* Block-manager's cookie */
	uint8_t  size;			/* Block-manager's cookie length */

#define	WT_ADDR_INT	1		/* Internal page */
#define	WT_ADDR_LEAF	2		/* Leaf page */
#define	WT_ADDR_LEAF_NO	3		/* Leaf page, no overflow */
	uint8_t  type;
};

/*
 * Overflow tracking of on-page key/value items: As pages are reconciled,
 * overflow key/value records referenced from the original page are discarded
 * as they are updated or removed.  We track such overflow items to ensure we
 * never discard the underlying blocks more than once.
 */
struct __wt_ovfl_onpage {
	uint8_t addr_offset;		/* Overflow addr offset */
	uint8_t addr_size;		/* Overflow addr size */

	/*
	 * On each page reconciliation, set the just-added flag for each newly
	 * added skiplist entry.  If reconciliation succeeds, the underlying
	 * blocks are then discarded, if reconciliation fails for any reason,
	 * the added records are discarded.
	 */
#define	WT_OVFL_ONPAGE_JUST_ADDED	0x01
	uint8_t flags;

	/*
	 * The untyped address immediately follows the WT_OVFL_ONPAGE structure.
	 */
#define	WT_OVFL_ONPAGE_ADDR(p)						\
	((void *)((uint8_t *)(p) + (p)->addr_offset))

	WT_OVFL_ONPAGE *next[0];	/* Forward-linked skip list */
};

/*
 * Overflow tracking for reuse: When a page is reconciled, we write new K/V
 * overflow items.  If pages are reconciled multiple times, we need to know
 * if we've already written a particular overflow record (so we don't write
 * it again), as well as if we've modified an overflow record previously
 * written (in which case we want to write a new record and discard blocks
 * used by the previously written record).  Track overflow records written
 * for the page, storing the values in a skiplist with the record's value as
 * the "key".
 */
struct __wt_ovfl_reuse {
	uint32_t value_offset;		/* Overflow value offset */
	uint32_t value_size;		/* Overflow value size */
	uint8_t  addr_offset;		/* Overflow addr offset */
	uint8_t  addr_size;		/* Overflow addr size */

	/*
	 * On each page reconciliation, we clear the entry's in-use flag, and
	 * reset it as the overflow record is re-used.  After reconciliation
	 * completes, unused skiplist entries are discarded, along with their
	 * underlying blocks.
	 *
	 * On each page reconciliation, set the just-added flag for each new
	 * skiplist entry; if reconciliation fails for any reason, discard the
	 * newly added skiplist entries, along with their underlying blocks.
	 */
#define	WT_OVFL_REUSE_INUSE		0x01
#define	WT_OVFL_REUSE_JUST_ADDED	0x02
	uint8_t	 flags;

	/*
	 * The untyped address immediately follows the WT_OVFL_REUSE structure,
	 * the untyped value immediately follows the address.
	 */
#define	WT_OVFL_REUSE_ADDR(p)						\
	((void *)((uint8_t *)(p) + (p)->addr_offset))
#define	WT_OVFL_REUSE_VALUE(p)						\
	((void *)((uint8_t *)(p) + (p)->value_offset))

	WT_OVFL_REUSE *next[0];		/* Forward-linked skip list */
};

/*
 * Overflow tracking for cached values: When a page is reconciled, we write new
 * K/V overflow items, and discard previous underlying blocks.  If there's a
 * transaction in the system that needs to read the previous value, we have to
 * cache the old value until no running transaction needs it.
 */
struct __wt_ovfl_txnc {
	uint64_t current;		/* Maximum transaction ID at store */

	uint32_t value_offset;		/* Overflow value offset */
	uint32_t value_size;		/* Overflow value size */
	uint8_t  addr_offset;		/* Overflow addr offset */
	uint8_t  addr_size;		/* Overflow addr size */

	/*
	 * The untyped address immediately follows the WT_OVFL_TXNC
	 * structure, the untyped value immediately follows the address.
	 */
#define	WT_OVFL_TXNC_ADDR(p)						\
	((void *)((uint8_t *)(p) + (p)->addr_offset))
#define	WT_OVFL_TXNC_VALUE(p)						\
	((void *)((uint8_t *)(p) + (p)->value_offset))

	WT_OVFL_TXNC *next[0];		/* Forward-linked skip list */
};

/*
 * WT_UPD_SKIPPED --
 *	When a page is reconciled, there may be updates that cannot be written.
 * Those updates are copied and then restored when the page is re-instantiated.
 */
struct __wt_upd_skipped {
	WT_UPDATE	*upd;		/* Skipped update */

	/*
	 * Skipped updates have to be moved to another page, so they come with
	 * either a pointer to the WT_INSERT list, or a pointer to a row-store
	 * leaf page update list.
	 */
	void		*head;		/* (WT_UPDATE **) or (WT_INSERT *) */
	uint8_t	is_insert;	/* (WT_INSERT *) */
};

/*
 * WT_PAGE_MODIFY --
 *	When a page is modified, there's additional information to maintain.
 */
struct __wt_page_modify {
	/*
	 * Track the highest transaction ID at which the page was written to
	 * disk.  This can be used to avoid trying to write the page multiple
	 * times if a snapshot is keeping old versions pinned (e.g., in a
	 * checkpoint).
	 */
	uint64_t disk_snap_min;

	/* The largest transaction ID seen on the page by reconciliation. */
	uint64_t rec_max_txn;

	/* The largest update transaction ID (approximate). */
	uint64_t update_txn;

	/*
	 * When pages are reconciled, the result is one or more replacement
	 * blocks.  In the case of multiple replacement blocks, some blocks
	 * may be identical to blocks written after previous reconciliations
	 * of the page, so add supporting infrastructure to detect when an
	 * existing block is sufficient and we can skip a write.
	 */
	WT_ADDR	 replace;		/* Single replacement block */
	struct __wt_multi {		/* Multiple replacement blocks */
		union {
			uint64_t recno;	/* Column-store: starting recno */
			WT_IKEY *ikey;	/* Row-store: variable-length key */
		} key;

		/*
		 * XXXKEITH
		 * These two sets of fields should be a union, only one gets
		 * filled in, it's either an address or a skipped update.
		 */
		WT_UPD_SKIPPED *skip;	/* Skipped updates */
		uint32_t skip_entries;
		void *skip_dsk;		/* Page's disk image */

		WT_ADDR	 addr;		/* Address */
		uint32_t size;		/* Size */
		uint32_t cksum;		/* Checksum */
		uint8_t	 reuse;		/* Being reused */

	} *multi;
	uint32_t multi_entries;		/* Multi-block element count */
	size_t	 multi_size;		/* Multi-block memory footprint */

	/*
	 * When pages which have split into multiple blocks are evicted, the
	 * multiple blocks are converted into a WT_REF array and inserted in
	 * the parent's child index.  Those arrays live here, appearing only
	 * in internal pages with children that have split and subsequently
	 * been evicted.
	 */
	struct __wt_split_list {
		WT_REF	*refs;		/* Split child WT_REF arrays */
		uint32_t entries;	/* Array element count */
	} *splits;
	uint32_t splits_entries;	/* Split child WT_REFs element count */

	/*
	 * When a root page splits, we create a fake page and write it; that
	 * fake page can also split and so on, and we continue this process
	 * until we write a single replacement root block.  We use the root
	 * split field of this structure to track that list of fake pages, so
	 * they are discarded when they're no longer needed.
	 */
	WT_PAGE *root_split;		/* Linked list of root split pages */

	/*
	 * Appended items to column-stores: there is only a single one of these
	 * per column-store tree.
	 */
	WT_INSERT_HEAD **append;	/* Appended items */

	/*
	 * Updated items in column-stores: variable-length RLE entries can
	 * expand to multiple entries which requires some kind of list we can
	 * expand on demand.  Updated items in fixed-length files could be done
	 * based on an WT_UPDATE array as in row-stores, but there can be a
	 * very large number of bits on a single page, and the cost of the
	 * WT_UPDATE array would be huge.
	 */
	WT_INSERT_HEAD **update;	/* Updated items */

	/* Overflow record tracking. */
	struct __wt_ovfl_track {
		WT_OVFL_ONPAGE	*ovfl_onpage[WT_SKIP_MAXDEPTH];
		WT_OVFL_REUSE	*ovfl_reuse[WT_SKIP_MAXDEPTH];
		WT_OVFL_TXNC	*ovfl_txnc[WT_SKIP_MAXDEPTH];
	} *ovfl_track;

	uint64_t bytes_dirty;		/* Dirty bytes added to cache. */

	/*
	 * The write generation is incremented when a page is modified, a page
	 * is clean if the write generation is 0.
	 *
	 * !!!
	 * 4B values are probably larger than required, but I'm more confident
	 * 4B types will always be backed by atomic writes to memory.
	 */
	uint32_t write_gen;

#define	WT_PAGE_LOCK(s, p)						\
	__wt_spin_lock((s), &S2C(s)->page_lock[(p)->modify->page_lock])
#define	WT_PAGE_TRYLOCK(s, p, idp)					\
	__wt_spin_trylock((s), &S2C(s)->page_lock[(p)->modify->page_lock], idp)
#define	WT_PAGE_UNLOCK(s, p)						\
	__wt_spin_unlock((s), &S2C(s)->page_lock[(p)->modify->page_lock])
	uint8_t page_lock;    /* Page's spinlock */

#define	WT_PM_REC_EMPTY		0x01	/* Reconciliation: page empty */
#define	WT_PM_REC_REPLACE	0x02	/* Reconciliation: page replaced */
#define	WT_PM_REC_SPLIT		0x04	/* Reconciliation: page split */
#define	WT_PM_REC_MASK							\
	(WT_PM_REC_EMPTY | WT_PM_REC_REPLACE | WT_PM_REC_SPLIT)
	uint8_t flags;			/* Page flags */
};

/*
 * WT_PAGE --
 * The WT_PAGE structure describes the in-memory page information.
 */
struct __wt_page {
	/* Per page-type information. */
	union {
		/*
		 * Internal pages (both column- and row-store).
		 *
		 * The page record number is only used by column-store, but it
		 * makes some things simpler and it doesn't cost us any memory,
		 * other structures in this union are still larger.
		 *
		 * In-memory internal pages have an array of pointers to child
		 * structures, maintained in collated order.  When a page is
		 * read into memory, the initial list of children is stored in
		 * the "orig_index" field, and it and the collated order are
		 * the same.  After a page splits, the collated order and the
		 * original order will differ.
		 *
		 * Multiple threads of control may be searching the in-memory
		 * internal page and a child page of the internal page may
		 * cause a split at any time.  When a page splits, a new array
		 * is allocated and atomically swapped into place.  Threads in
		 * the old array continue without interruption (the old array is
		 * still valid), but have to avoid racing.  No barrier is needed
		 * because the array reference is updated atomically, but code
		 * reading the fields multiple times would be a very bad idea.
		 * Specifically, do not do this:
		 *	WT_REF **refp = page->pg_intl_index->index;
		 *	uint32_t entries = page->pg_intl_index->entries;
		 *
		 * The field is declared volatile (so the compiler knows not to
		 * read it multiple times).
		 */
		struct {
			uint64_t recno;		/* Starting recno */

			struct __wt_page_index {
				uint32_t entries;
				WT_REF	**index;
			} * volatile index;	/* Collated children */

			WT_REF	*orig_index;	/* Original children */
			uint32_t orig_entries;	/* Original children count */
		} intl;
#undef	pg_intl_recno
#define	pg_intl_recno		u.intl.recno
#undef	pg_intl_orig_index
#define	pg_intl_orig_index	u.intl.orig_index
#undef	pg_intl_orig_entries
#define	pg_intl_orig_entries	u.intl.orig_entries
#undef	pg_intl_index
#define	pg_intl_index		u.intl.index
#define	WT_INTL_FOREACH_BEGIN(page, ref) do {				\
	WT_PAGE_INDEX *__pindex;					\
	WT_REF **__refp;						\
	uint32_t __entries;						\
	for (__pindex = (page)->pg_intl_index,				\
	    __refp = __pindex->index,					\
	    __entries = __pindex->entries; __entries > 0; --__entries) {\
		(ref) = *__refp++;
#define	WT_INTL_FOREACH_END	} } while (0)

		/* Row-store leaf page. */
		struct {
			WT_ROW *d;		/* Key/value pairs */

			/*
			 * The column-store leaf page modification structures
			 * live in the WT_PAGE_MODIFY structure to keep the
			 * WT_PAGE structure as small as possible for read-only
			 * pages.  For consistency, we could move the row-store
			 * modification structures into WT_PAGE_MODIFY too, but
			 * that doesn't shrink WT_PAGE any further and it would
			 * require really ugly naming inside of WT_PAGE_MODIFY
			 * to avoid growing that structure.
			 */
			WT_INSERT_HEAD	**ins;	/* Inserts */
			WT_UPDATE	**upd;	/* Updates */

			uint32_t entries;	/* Entries */
		} row;
#undef	pg_row_d
#define	pg_row_d	u.row.d
#undef	pg_row_ins
#define	pg_row_ins	u.row.ins
#undef	pg_row_upd
#define	pg_row_upd	u.row.upd
#define	pg_row_entries	u.row.entries
#define	pg_row_entries	u.row.entries

		/* Fixed-length column-store leaf page. */
		struct {
			uint64_t recno;		/* Starting recno */

			uint8_t	*bitf;		/* Values */
			uint32_t entries;	/* Entries */
		} col_fix;
#undef	pg_fix_recno
#define	pg_fix_recno	u.col_fix.recno
#undef	pg_fix_bitf
#define	pg_fix_bitf	u.col_fix.bitf
#undef	pg_fix_entries
#define	pg_fix_entries	u.col_fix.entries

		/* Variable-length column-store leaf page. */
		struct {
			uint64_t recno;		/* Starting recno */

			WT_COL *d;		/* Values */

			/*
			 * Variable-length column-store files maintain a list of
			 * RLE entries on the page so it's unnecessary to walk
			 * the page counting records to find a specific entry.
			 */
			WT_COL_RLE *repeats;	/* RLE array for lookups */
			uint32_t    nrepeats;	/* Number of repeat slots */

			uint32_t    entries;	/* Entries */
		} col_var;
#undef	pg_var_recno
#define	pg_var_recno	u.col_var.recno
#undef	pg_var_d
#define	pg_var_d	u.col_var.d
#undef	pg_var_repeats
#define	pg_var_repeats	u.col_var.repeats
#undef	pg_var_nrepeats
#define	pg_var_nrepeats	u.col_var.nrepeats
#undef	pg_var_entries
#define	pg_var_entries	u.col_var.entries
	} u;

	/* Page's on-disk representation: NULL for pages created in memory. */
	WT_PAGE_HEADER *dsk;

	/* If/when the page is modified, we need lots more information. */
	WT_PAGE_MODIFY *modify;

	/*
	 * The page's read generation acts as an LRU value for each page in the
	 * tree; it is used by the eviction server thread to select pages to be
	 * discarded from the in-memory tree.
	 *
	 * The read generation is a 64-bit value, if incremented frequently, a
	 * 32-bit value could overflow.
	 *
	 * The read generation is a piece of shared memory potentially read
	 * by many threads.  We don't want to update page read generations for
	 * in-cache workloads and suffer the cache misses, so we don't simply
	 * increment the read generation value on every access.  Instead, the
	 * read generation is incremented by the eviction server each time it
	 * becomes active.  To avoid incrementing a page's read generation too
	 * frequently, it is set to a future point.
	 */
#define	WT_READ_GEN_NOTSET	0
#define	WT_READ_GEN_OLDEST	1
#define	WT_READ_GEN_STEP	100
	uint64_t read_gen;

	uint64_t memory_footprint;	/* Memory attached to the page */

	/*
	 * The page's parent: a link to the physical parent page, and a hint
	 * offset for the matching parent page's WT_REF structure.
	 */
#define	WT_PAGE_IS_ROOT(page)						\
	((page)->parent == NULL)
	WT_PAGE	*parent;		/* Page's parent */
	uint32_t ref_hint;		/* Page's WT_REF hint */

#define	WT_PAGE_INVALID		0	/* Invalid page */
#define	WT_PAGE_BLOCK_MANAGER	1	/* Block-manager page */
#define	WT_PAGE_COL_FIX		2	/* Col-store fixed-len leaf */
#define	WT_PAGE_COL_INT		3	/* Col-store internal page */
#define	WT_PAGE_COL_VAR		4	/* Col-store var-length leaf page */
#define	WT_PAGE_OVFL		5	/* Overflow page */
#define	WT_PAGE_ROW_INT		6	/* Row-store internal page */
#define	WT_PAGE_ROW_LEAF	7	/* Row-store leaf page */
	uint8_t type;			/* Page type */

#define	WT_PAGE_BUILD_KEYS	0x01	/* Keys have been built in memory */
#define	WT_PAGE_DISK_ALLOC	0x02	/* Disk image in allocated memory */
#define	WT_PAGE_DISK_MAPPED	0x04	/* Disk image in mapped memory */
#define	WT_PAGE_EVICT_LRU	0x08	/* Page is on the LRU queue */
#define	WT_PAGE_EVICT_FORCE	0x10	/* Page being forcibly evicted */
	uint8_t flags_atomic;		/* Atomic flags, use F_*_ATOMIC */
};

/*
 * WT_PAGE_DISK_OFFSET, WT_PAGE_REF_OFFSET --
 *	Return the offset/pointer of a pointer/offset in a page disk image.
 */
#define	WT_PAGE_DISK_OFFSET(page, p)					\
	WT_PTRDIFF32(p, (page)->dsk)
#define	WT_PAGE_REF_OFFSET(page, o)					\
	((void *)((uint8_t *)((page)->dsk) + (o)))

/*
 * Page state.
 *
 * Synchronization is based on the WT_REF->state field, which has a number of
 * possible states:
 *
 * WT_REF_DISK:
 *	The initial setting before a page is brought into memory, and set as a
 *	result of page eviction; the page is on disk, and must be read into
 *	memory before use.  WT_REF_DISK has a value of 0 (the default state
 *	after allocating cleared memory).
 *
 * WT_REF_DELETED:
 *	The page is on disk, but has been deleted from the tree; we can delete
 *	row-store leaf pages without reading them if they don't reference
 *	overflow items.
 *
 * WT_REF_EVICT_WALK:
 *	The next page to be walked for LRU eviction.  This page is available
 *	for reads but not eviction.
 *
 * WT_REF_LOCKED:
 *	Locked for exclusive access.  In eviction, this page or a parent has
 *	been selected for eviction; once hazard pointers are checked, the page
 *	will be evicted.  When reading a page that was previously deleted, it
 *	is locked until the page is in memory with records marked deleted.  The
 *	thread that set the page to WT_REF_LOCKED has exclusive access, no
 *	other thread may use the WT_REF until the state is changed.
 *
 * WT_REF_MEM:
 *	Set by a reading thread once the page has been read from disk; the page
 *	is in the cache and the page reference is OK.
 *
 * WT_REF_READING:
 *	Set by a reading thread before reading an ordinary page from disk;
 *	other readers of the page wait until the read completes.  Sync can
 *	safely skip over such pages: they are clean by definition.
 *
 * WT_REF_SPLIT:
 *	Set when the page is split; the WT_REF is dead and can no longer be
 *	used.
 *
 * The life cycle of a typical page goes like this: pages are read into memory
 * from disk and their state set to WT_REF_MEM.  When the page is selected for
 * eviction, the page state is set to WT_REF_LOCKED.  In all cases, evicting
 * threads reset the page's state when finished with the page: if eviction was
 * successful (a clean page was discarded, and a dirty page was written to disk
 * and then discarded), the page state is set to WT_REF_DISK; if eviction failed
 * because the page was busy, page state is reset to WT_REF_MEM.
 *
 * Readers check the state field and if it's WT_REF_MEM, they set a hazard
 * pointer to the page, flush memory and re-confirm the page state.  If the
 * page state is unchanged, the reader has a valid reference and can proceed.
 *
 * When an evicting thread wants to discard a page from the tree, it sets the
 * WT_REF_LOCKED state, flushes memory, then checks hazard pointers.  If a
 * hazard pointer is found, state is reset to WT_REF_MEM, restoring the page
 * to the readers.  If the evicting thread does not find a hazard pointer,
 * the page is evicted.
 */
enum __wt_page_state {
	WT_REF_DISK=0,			/* Page is on disk */
	WT_REF_DELETED,			/* Page is on disk, but deleted */
	WT_REF_EVICT_WALK,		/* Next page for LRU eviction */
	WT_REF_LOCKED,			/* Page locked for exclusive access */
	WT_REF_MEM,			/* Page is in cache and valid */
	WT_REF_READING,			/* Page being read */
	WT_REF_SPLIT			/* Page was split */
};

/*
 * WT_REF --
 *	A single in-memory page and the state information used to determine if
 * it's OK to dereference the pointer to the page.
 */
struct __wt_ref {
	WT_PAGE *page;			/* In-memory page */

	/*
	 * Address: on-page cell if read from backing block, off-page WT_ADDR
	 * if instantiated in-memory, or NULL if page created in-memory.
	 */
	void	*addr;

	/*
	 * The child page's key.  Do NOT change this union without reviewing
	 * __wt_ref_key.
	 */
	union {
		uint64_t recno;		/* Column-store: starting recno */
		void	*ikey;		/* Row-store: instantiated key */
		uint64_t pkey;		/* Row-store: on-page key */
	} key;

	uint64_t txnid;			/* Transaction ID */

	volatile WT_PAGE_STATE state;	/* Page state */

	uint32_t unused;
};
/*
 * WT_REF_SIZE is the expected structure size -- we verify the build to ensure
 * the compiler hasn't inserted padding which would break the world.
 */
#define	WT_REF_SIZE	40

/*
 * WT_LINK_PAGE --
 * Link a child page into a reference in its parent.
 */
#define	WT_LINK_PAGE(ppage, pref, cpage) do {				\
	(cpage)->parent = (ppage);					\
	(pref)->page = (cpage);						\
} while (0)

/*
 * WT_ROW --
 * Each in-memory page row-store leaf page has an array of WT_ROW structures:
 * this is created from on-page data when a page is read from the file.  It's
 * sorted by key, fixed in size, and references data on the page.
 *
 * Multiple threads of control may be searching the in-memory row-store pages,
 * and the key may be instantiated at any time.  Code must be able to handle
 * both when the key has not been instantiated (the key field points into the
 * page's disk image), and when the key has been instantiated (the key field
 * points outside the page's disk image).  We don't need barriers because the
 * key is updated atomically, but code that reads the key field multiple times
 * is a very, very bad idea.  Specifically, do not do this:
 *
 *	key = rip->key;
 *	if (key_is_on_page(key)) {
 *		cell = rip->key;
 *	}
 *
 * The field is declared volatile (so the compiler knows it shouldn't read it
 * multiple times), and we obscure the field name and use a copy macro in all
 * references to the field (so the code doesn't read it multiple times), all
 * to make sure we don't introduce this bug (again).
 *
 * Casting the read to a (void *) is safe as we are not taking the address of
 * the object.
 */
struct __wt_row {
	void * volatile __key;		/* On-page cell or off-page WT_IKEY */
};
#define	WT_ROW_KEY_COPY(rip)	((rip)->__key)
#define	WT_ROW_KEY_SET(rip, v)	((rip)->__key) = (v)

/*
 * WT_ROW_FOREACH --
 *	Walk the entries of an in-memory row-store leaf page.
 */
#define	WT_ROW_FOREACH(page, rip, i)					\
	for ((i) = (page)->pg_row_entries,				\
	    (rip) = (page)->pg_row_d; (i) > 0; ++(rip), --(i))
#define	WT_ROW_FOREACH_REVERSE(page, rip, i)				\
	for ((i) = (page)->pg_row_entries,				\
	    (rip) = (page)->pg_row_d + ((page)->pg_row_entries - 1);	\
	    (i) > 0; --(rip), --(i))

/*
 * WT_ROW_SLOT --
 *	Return the 0-based array offset based on a WT_ROW reference.
 */
#define	WT_ROW_SLOT(page, rip)						\
	((uint32_t)(((WT_ROW *)rip) - (page)->pg_row_d))

/*
 * WT_COL --
 * Each in-memory variable-length column-store leaf page has an array of WT_COL
 * structures: this is created from on-page data when a page is read from the
 * file.  It's fixed in size, and references data on the page.
 */
struct __wt_col {
	/*
	 * Variable-length column-store data references are page offsets, not
	 * pointers (we boldly re-invent short pointers).  The trade-off is 4B
	 * per K/V pair on a 64-bit machine vs. a single cycle for the addition
	 * of a base pointer.  The on-page data is a WT_CELL (same as row-store
	 * pages).
	 *
	 * If the value is 0, it's a single, deleted record.
	 *
	 * Obscure the field name, code shouldn't use WT_COL->value, the public
	 * interface is WT_COL_PTR.
	 */
	uint32_t __value;
};

/*
 * WT_COL_RLE --
 * In variable-length column store leaf pages, we build an array of entries
 * with RLE counts greater than 1 when reading the page.  We can do a binary
 * search in this array, then an offset calculation to find the cell.
 */
struct __wt_col_rle {
	uint64_t recno;			/* Record number of first repeat. */
	uint64_t rle;			/* Repeat count. */
	uint32_t indx;			/* Slot of entry in col_var.d */
} WT_GCC_ATTRIBUTE((packed));

/*
 * WT_COL_PTR --
 *	Return a pointer corresponding to the data offset -- if the item doesn't
 * exist on the page, return a NULL.
 */
#define	WT_COL_PTR(page, cip)						\
	((cip)->__value == 0 ? NULL : WT_PAGE_REF_OFFSET(page, (cip)->__value))

/*
 * WT_COL_FOREACH --
 *	Walk the entries of variable-length column-store leaf page.
 */
#define	WT_COL_FOREACH(page, cip, i)					\
	for ((i) = (page)->pg_var_entries,				\
	    (cip) = (page)->pg_var_d; (i) > 0; ++(cip), --(i))

/*
 * WT_COL_SLOT --
 *	Return the 0-based array offset based on a WT_COL reference.
 */
#define	WT_COL_SLOT(page, cip)						\
	((uint32_t)(((WT_COL *)cip) - (page)->pg_var_d))

/*
 * WT_IKEY --
 * Instantiated key: row-store keys are usually prefix compressed and sometimes
 * Huffman encoded or overflow objects.  Normally, a row-store page in-memory
 * key points to the on-page WT_CELL, but in some cases, we instantiate the key
 * in memory, in which case the row-store page in-memory key points to a WT_IKEY
 * structure.
 */
struct __wt_ikey {
	uint32_t size;			/* Key length */

	/*
	 * If we no longer point to the key's on-page WT_CELL, we can't find its
	 * related value.  Save the offset of the key cell in the page.
	 *
	 * Row-store cell references are page offsets, not pointers (we boldly
	 * re-invent short pointers).  The trade-off is 4B per K/V pair on a
	 * 64-bit machine vs. a single cycle for the addition of a base pointer.
	 */
	uint32_t  cell_offset;

	/* The key bytes immediately follow the WT_IKEY structure. */
#define	WT_IKEY_DATA(ikey)						\
	((void *)((uint8_t *)(ikey) + sizeof(WT_IKEY)))
};

/*
 * WT_UPDATE --
 * Entries on leaf pages can be updated, either modified or deleted.  Updates
 * to entries referenced from the WT_ROW and WT_COL arrays are stored in the
 * page's WT_UPDATE array.  When the first element on a page is updated, the
 * WT_UPDATE array is allocated, with one slot for every existing element in
 * the page.  A slot points to a WT_UPDATE structure; if more than one update
 * is done for an entry, WT_UPDATE structures are formed into a forward-linked
 * list.
 */
struct __wt_update {
	uint64_t txnid;			/* update transaction */

	WT_UPDATE *next;		/* forward-linked list */

	/*
	 * We use the maximum size as an is-deleted flag, which means we can't
	 * store 4GB objects; I'd rather do that than increase the size of this
	 * structure for a flag bit.
	 */
#define	WT_UPDATE_DELETED_ISSET(upd)	((upd)->size == UINT32_MAX)
#define	WT_UPDATE_DELETED_SET(upd)	((upd)->size = UINT32_MAX)
	uint32_t size;			/* update length */

	/* The untyped value immediately follows the WT_UPDATE structure. */
#define	WT_UPDATE_DATA(upd)						\
	((void *)((uint8_t *)(upd) + sizeof(WT_UPDATE)))
} WT_GCC_ATTRIBUTE((packed));

/*
 * WT_INSERT --
 *
 * Row-store leaf pages support inserts of new K/V pairs.  When the first K/V
 * pair is inserted, the WT_INSERT_HEAD array is allocated, with one slot for
 * every existing element in the page, plus one additional slot.  A slot points
 * to a WT_INSERT_HEAD structure for the items which sort after the WT_ROW
 * element that references it and before the subsequent WT_ROW element; the
 * skiplist structure has a randomly chosen depth of next pointers in each
 * inserted node.
 *
 * The additional slot is because it's possible to insert items smaller than any
 * existing key on the page: for that reason, the first slot of the insert array
 * holds keys smaller than any other key on the page.
 *
 * In column-store variable-length run-length encoded pages, a single indx
 * entry may reference a large number of records, because there's a single
 * on-page entry representing many identical records.   (We don't expand those
 * entries when the page comes into memory, as that would require resources as
 * pages are moved to/from the cache, including read-only files.)  Instead, a
 * single indx entry represents all of the identical records originally found
 * on the page.
 *
 * Modifying (or deleting) run-length encoded column-store records is hard
 * because the page's entry no longer references a set of identical items.  We
 * handle this by "inserting" a new entry into the insert array, with its own
 * record number.  (This is the only case where it's possible to insert into a
 * column-store: only appends are allowed, as insert requires re-numbering
 * subsequent records.  Berkeley DB did support mutable records, but it won't
 * scale and it isn't useful enough to re-implement, IMNSHO.)
 */
struct __wt_insert {
	WT_UPDATE *upd;				/* value */

	union {
		uint64_t recno;			/* column-store record number */
		struct {
			uint32_t offset;	/* row-store key data start */
			uint32_t size;		/* row-store key data size */
		} key;
	} u;

#define	WT_INSERT_KEY_SIZE(ins) (((WT_INSERT *)ins)->u.key.size)
#define	WT_INSERT_KEY(ins)						\
	((void *)((uint8_t *)(ins) + ((WT_INSERT *)ins)->u.key.offset))
#define	WT_INSERT_RECNO(ins)	(((WT_INSERT *)ins)->u.recno)

	WT_INSERT *next[0];			/* forward-linked skip list */
};

/*
 * Skiplist helper macros.
 */
#define	WT_SKIP_FIRST(ins_head)						\
	(((ins_head) == NULL) ? NULL : (ins_head)->head[0])
#define	WT_SKIP_LAST(ins_head)						\
	(((ins_head) == NULL) ? NULL : (ins_head)->tail[0])
#define	WT_SKIP_NEXT(ins)  ((ins)->next[0])
#define	WT_SKIP_FOREACH(ins, ins_head)					\
	for ((ins) = WT_SKIP_FIRST(ins_head);				\
	    (ins) != NULL;						\
	    (ins) = WT_SKIP_NEXT(ins))

/*
 * Atomically allocate and swap a structure or array into place.
 */
#define	WT_PAGE_ALLOC_AND_SWAP(s, page, dest, v, count)	do {		\
	if (((v) = (dest)) == NULL) {					\
		WT_ERR(__wt_calloc_def(s, count, &(v)));		\
		if (WT_ATOMIC_CAS(dest, NULL, v))			\
			__wt_cache_page_inmem_incr(			\
			    s, page, (count) * sizeof(*(v)));		\
		else							\
			__wt_free(s, v);				\
	}								\
} while (0)

/*
 * WT_INSERT_HEAD --
 * 	The head of a skiplist of WT_INSERT items.
 */
struct __wt_insert_head {
	WT_INSERT *head[WT_SKIP_MAXDEPTH];	/* first item on skiplists */
	WT_INSERT *tail[WT_SKIP_MAXDEPTH];	/* last item on skiplists */
};

/*
 * The row-store leaf page insert lists are arrays of pointers to structures,
 * and may not exist.  The following macros return an array entry if the array
 * of pointers and the specific structure exist, else NULL.
 */
#define	WT_ROW_INSERT_SLOT(page, slot)					\
	((page)->pg_row_ins == NULL ? NULL : (page)->pg_row_ins[slot])
#define	WT_ROW_INSERT(page, ip)						\
	WT_ROW_INSERT_SLOT(page, WT_ROW_SLOT(page, ip))
#define	WT_ROW_UPDATE(page, ip)						\
	((page)->pg_row_upd == NULL ?					\
	    NULL : (page)->pg_row_upd[WT_ROW_SLOT(page, ip)])
/*
 * WT_ROW_INSERT_SMALLEST references an additional slot past the end of the
 * the "one per WT_ROW slot" insert array.  That's because the insert array
 * requires an extra slot to hold keys that sort before any key found on the
 * original page.
 */
#define	WT_ROW_INSERT_SMALLEST(page)					\
	((page)->pg_row_ins == NULL ?					\
	    NULL : (page)->pg_row_ins[(page)->pg_row_entries])

/*
 * The column-store leaf page update lists are arrays of pointers to structures,
 * and may not exist.  The following macros return an array entry if the array
 * of pointers and the specific structure exist, else NULL.
 */
#define	WT_COL_UPDATE_SLOT(page, slot)					\
	((page)->modify == NULL || (page)->modify->update == NULL ?	\
	    NULL : (page)->modify->update[slot])
#define	WT_COL_UPDATE(page, ip)						\
	WT_COL_UPDATE_SLOT(page, WT_COL_SLOT(page, ip))

/*
 * WT_COL_UPDATE_SINGLE is a single WT_INSERT list, used for any fixed-length
 * column-store updates for a page.
 */
#define	WT_COL_UPDATE_SINGLE(page)					\
	WT_COL_UPDATE_SLOT(page, 0)

/*
 * WT_COL_APPEND is an WT_INSERT list, used for fixed- and variable-length
 * appends.
 */
#define	WT_COL_APPEND(page)						\
	((page)->modify != NULL &&					\
	    (page)->modify->append != NULL ? (page)->modify->append[0] : NULL)

/* WT_FIX_FOREACH walks fixed-length bit-fields on a disk page. */
#define	WT_FIX_FOREACH(btree, dsk, v, i)				\
	for ((i) = 0,							\
	    (v) = (i) < (dsk)->u.entries ?				\
	    __bit_getv(							\
	    WT_PAGE_HEADER_BYTE(btree, dsk), 0, (btree)->bitcnt) : 0;	\
	    (i) < (dsk)->u.entries; ++(i),				\
	    (v) = __bit_getv(						\
	    WT_PAGE_HEADER_BYTE(btree, dsk), i, (btree)->bitcnt))
