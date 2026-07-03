/*
 * ======================================================================
 * Step 1: SLUB 分配器 — 从 kmem_cache_alloc() 到伙伴系统
 * ======================================================================
 *
 * 原始路径：
 *   - src/linux-5.10/mm/slub.c (5000+ 行)
 *   - src/linux-5.10/include/linux/slub_def.h
 *
 * 本文件关注 SLUB 分配器的**分配路径**（kmalloc 底层实现）：
 *   1. kmem_cache_cpu / kmem_cache — 关键数据结构
 *   2. kmem_cache_alloc() → slab_alloc_node() — 快路径
 *   3. get_freelist() — 从 slab page 获取 freelist
 *   4. ___slab_alloc() — 慢路径（per-CPU 耗尽后的 fallback）
 *   5. get_partial() / get_partial_node() — 从 node partial list 取 slab
 *   6. deactivate_slab() / put_cpu_partial() — slab 的归还
 *
 * 预计阅读时间：30 分钟
 * ======================================================================
 *
 * ★ SLUB 三层架构（★ 核心！）
 *
 *   ┌─────────────────────────────────────────────────────────┐
 *   │              SLUB 分配路径（从快到慢）                      │
 *   ├─────────────────────────────────────────────────────────┤
 *   │                                                         │
 *   │  Layer 1: per-CPU freelist （最快，无锁 cmpxchg_double）  │
 *   │    kmem_cache_cpu.freelist → obj1 → obj2 → obj3 → NULL  │
 *   │    ★ 90%+ 的分配在这里完成                                │
 *   │                                                         │
 *   │  Layer 2: per-CPU partial list （次快，cmpxchg 单指针）   │
 *   │    kmem_cache_cpu.partial → page1 → page2 → ...          │
 *   │    ★ 每个 page 是 frozen 状态，有剩余对象                   │
 *   │                                                         │
 *   │  Layer 3: per-node partial list （需要 spinlock）         │
 *   │    kmem_cache_node.partial → page1 → page2 → ...          │
 *   │    ★ 需要获取 n->list_lock                                │
 *   │                                                         │
 *   │  Layer 4: 伙伴系统（最慢，需要 zone->lock）                │
 *   │    new_slab() → allocate_slab() → __alloc_pages()         │
 *   │    ★ 从 buddy allocator 分配新的物理页                     │
 *   │                                                         │
 *   └─────────────────────────────────────────────────────────┘
 *
 *   性能瓶颈分析：
 *   - Layer 1 完全无锁，每 CPU 独立 → 无竞争
 *   - Layer 2 只需 this_cpu_cmpxchg → 无竞争（单 CPU 操作）
 *   - Layer 3 需要 n->list_lock → 多 CPU 分配同 cache 时竞争
 *   - Layer 4 需要 zone->lock → 全局热点，高并发时严重竞争
 */


/* ======================================================================
 * ★ 关键数据结构
 * ======================================================================
 *
 * 理解 SLUB 的前提：kmem_cache_cpu（每 CPU）和 kmem_cache（全局）
 */

/*
 * per-CPU 数据（slub_def.h:42）
 *
 * 每个 kmem_cache（如 kmalloc-64）在每个 CPU 上有一份 kmem_cache_cpu。
 * 这是快路径的核心数据结构——分配对象时先从这里取。
 */
struct kmem_cache_cpu {
    void **freelist;        /* ★ 指向下一个可用对象的指针 */
    unsigned long tid;      /* ★ 全局唯一事务 ID（用于 cmpxchg_double 的 ABA 保护） */
    struct page *page;      /* ★ 当前正在分配的 slab page（frozen 状态） */
#ifdef CONFIG_SLUB_CPU_PARTIAL
    struct page *partial;   /* ★ per-CPU partial slab 链表（frozen 状态的 slab 缓存） */
#endif
#ifdef CONFIG_SLUB_STATS
    unsigned stat[NR_SLUB_STAT_ITEMS];
#endif
};

/*
 * kmem_cache 结构（slub_def.h:83）— 全局共享
 *
 * 每种大小类别（如 kmalloc-64、kmalloc-128）对应一个 kmem_cache。
 * 它管理全局配置和 per-node 的 partial list。
 */
struct kmem_cache {
    struct kmem_cache_cpu __percpu *cpu_slab;  /* ★ per-CPU 数据区 */
    slab_flags_t flags;
    unsigned long min_partial;      /* 每个 node 至少保留的 partial slab 数量 */
    unsigned int size;              /* 对象总大小（含元数据） */
    unsigned int object_size;       /* 对象实际大小（不含元数据） */
    unsigned int offset;            /* freelist 指针在对象内的偏移 */
#ifdef CONFIG_SLUB_CPU_PARTIAL
    unsigned int cpu_partial;       /* per-CPU partial 中最多保留的对象数 */
#endif
    struct kmem_cache_order_objects oo;   /* 最优 order 和对应的对象数 */
    gfp_t allocflags;
    const char *name;               /* cache 名称，如 "kmalloc-64" */
    struct list_head list;          /* 全局 slab_caches 链表中的节点 */
    /* ... 省略调试相关字段 ... */
};


/* ======================================================================
 * ★ 核心函数 #1: kmem_cache_alloc — kmalloc 的底层入口
 * ======================================================================
 *
 * 用户调用 kmalloc(64, GFP_KERNEL) 时，内核根据大小选择 kmalloc-64
 * 对应的 kmem_cache，然后调用 kmem_cache_alloc()。
 *
 * 调用链：
 *   kmalloc(64, GFP_KERNEL)
 *     → kmalloc_slab(64) → 找到 kmalloc-64 的 kmem_cache
 *     → kmem_cache_alloc(s, GFP_KERNEL)
 *       → slab_alloc(s, gfpflags, _RET_IP_)
 *         → slab_alloc_node(s, gfpflags, NUMA_NO_NODE, addr)
 *           → ★ 快路径：this_cpu_cmpxchg_double 原子操作
 *           → ★ 慢路径：__slab_alloc → ___slab_alloc
 */
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t gfpflags)
{
    void *ret = slab_alloc(s, gfpflags, _RET_IP_);

    trace_kmem_cache_alloc(_RET_IP_, ret, s->object_size,
                s->size, gfpflags);

    return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc);

/* slab_alloc 是 slab_alloc_node 的封装，不指定 NUMA node */
static __always_inline void *slab_alloc(struct kmem_cache *s,
        gfp_t gfpflags, unsigned long addr)
{
    return slab_alloc_node(s, gfpflags, NUMA_NO_NODE, addr);
}


/* ======================================================================
 * ★ 核心函数 #2: slab_alloc_node — 快路径 + 慢路径入口
 * ======================================================================
 *
 * 这是 SLUB 分配的核心函数，包含快路径和慢路径的分叉点。
 *
 * 快路径（热路径）：
 *   1. 读取当前 CPU 的 kmem_cache_cpu
 *   2. 从 c->freelist 取第一个对象
 *   3. 用 this_cpu_cmpxchg_double 原子更新 freelist + tid
 *   → 如果成功，分配完成（★ 无锁！）
 *
 * 慢路径：
 *   → 调用 __slab_alloc（关中断）→ ___slab_alloc
 *
 * ★ tid（事务 ID）机制：
 *   - 每个 CPU 上的每次分配/释放操作都会使 tid 递增
 *   - cmpxchg_double 同时比较 freelist 和 tid
 *   - 防止 ABA 问题：即使 freelist 碰巧相同，tid 也不同
 *   - 同时保证操作在同一 CPU 上完成（防止抢占后在错误 CPU 上操作）
 */
static __always_inline void *slab_alloc_node(struct kmem_cache *s,
        gfp_t gfpflags, int node, unsigned long addr)
{
    void *object;
    struct kmem_cache_cpu *c;
    struct page *page;
    unsigned long tid;
    struct obj_cgroup *objcg = NULL;

    /* 分配前的 hook（cgroup 内存计费、kasan 等） */
    s = slab_pre_alloc_hook(s, &objcg, 1, gfpflags);
    if (!s)
        return NULL;
redo:
    /*
     * ★ 读取 per-CPU 数据
     *
     * 关键：先读 tid，再读 c 指针，保证一致性。
     * 如果发生 CPU 迁移（CONFIG_PREEMPTION），tid 和 c 可能不匹配，
     * 此时重新读取直到匹配。
     */
    do {
        tid = this_cpu_read(s->cpu_slab->tid);
        c = raw_cpu_ptr(s->cpu_slab);
    } while (IS_ENABLED(CONFIG_PREEMPTION) &&
         unlikely(tid != READ_ONCE(c->tid)));

    /*
     * barrier() 保证 tid 在 c->freelist / c->page 之前读取。
     * 如果没有 barrier，编译器可能重排序，导致读到下一个 tid 的数据。
     */
    barrier();

    /*
     * ★ 快路径核心：
     *   1. 读取当前 freelist 头指针和 page
     *   2. 如果 freelist 非空且 page 匹配目标 node → 尝试原子更新
     *   3. 否则 → 慢路径
     */
    object = c->freelist;
    page = c->page;
    if (unlikely(!object || !page || !node_match(page, node))) {
        /* ★ 慢路径：freelist 为空 或 page 不在目标 node */
        object = __slab_alloc(s, gfpflags, node, addr, c);
    } else {
        /*
         * 快路径：从 freelist 取第一个对象
         *
         * get_freepointer_safe() 从当前对象中读取下一个对象的指针
         * （freelist 是链表结构，每个空闲对象的头部存着下一个空闲对象的地址）
         */
        void *next_object = get_freepointer_safe(s, object);

        /*
         * ★ 原子操作：this_cpu_cmpxchg_double
         *
         * 同时比较并更新两个值：
         *   旧值: (freelist = object, tid = tid)
         *   新值: (freelist = next_object, tid = next_tid(tid))
         *
         * 如果在此期间中断/抢占修改了 freelist 或 tid，操作会失败，
         * 跳转到 redo 重新来过。
         *
         * ★ 这就是 SLUB "无锁" 的秘诀——不用 spinlock，
         *    而是用 CAS（Compare-And-Swap）原子操作。
         */
        if (unlikely(!this_cpu_cmpxchg_double(
                s->cpu_slab->freelist, s->cpu_slab->tid,
                object, tid,
                next_object, next_tid(tid)))) {

            note_cmpxchg_failure("slab_alloc", s, tid);
            goto redo;
        }
        prefetch_freepointer(s, next_object);  /* 预取下一个对象，加速连续分配 */
        stat(s, ALLOC_FASTPATH);
    }

    /* 如果需要，擦除对象中的 freelist 指针（安全加固） */
    maybe_wipe_obj_freeptr(s, object);

    /* __GFP_ZERO 时清零对象 */
    if (unlikely(slab_want_init_on_alloc(gfpflags, s)) && object)
        memset(object, 0, s->object_size);

    slab_post_alloc_hook(s, objcg, gfpflags, 1, &object);

    return object;
}


/* ======================================================================
 * ★ 核心函数 #3: ___slab_alloc — 慢路径（per-CPU 耗尽后的 fallback）
 * ======================================================================
 *
 * 当快路径失败时（freelist 为空、page 不匹配 node），进入此函数。
 *
 * 此函数在中断关闭的状态下运行（__slab_alloc 负责关中断）。
 *
 * 查找顺序：
 *   1. 尝试从当前 page 的 regular freelist 获取（get_freelist）
 *   2. 尝试从 per-CPU partial list 获取
 *   3. 尝试从 per-node partial list 获取（new_slab_objects）
 *   4. 最后向伙伴系统申请新的物理页（new_slab → __alloc_pages）
 */
static void *___slab_alloc(struct kmem_cache *s, gfp_t gfpflags, int node,
              unsigned long addr, struct kmem_cache_cpu *c)
{
    void *freelist;
    struct page *page;

    stat(s, ALLOC_SLOWPATH);

    page = c->page;
    if (!page) {
        /*
         * 情况 A：当前 CPU 没有关联的 slab page
         * → 直接跳到 new_slab 去获取新的 slab
         */
        if (unlikely(node != NUMA_NO_NODE &&
                 !node_state(node, N_NORMAL_MEMORY)))
            node = NUMA_NO_NODE;
        goto new_slab;
    }
redo:

    /*
     * 情况 B：当前 page 不在目标 NUMA node 上
     * → deactivate 当前 page，然后跳到 new_slab
     */
    if (unlikely(!node_match(page, node))) {
        if (!node_state(node, N_NORMAL_MEMORY)) {
            node = NUMA_NO_NODE;
            goto redo;
        } else {
            stat(s, ALLOC_NODE_MISMATCH);
            deactivate_slab(s, page, c->freelist, c);
            goto new_slab;
        }
    }

    /* pfmemalloc 检查：内存回收上下文特殊处理 */
    if (unlikely(!pfmemalloc_match(page, gfpflags))) {
        deactivate_slab(s, page, c->freelist, c);
        goto new_slab;
    }

    /*
     * 情况 C：再次检查 c->freelist
     *
     * 为什么？因为关中断前可能有 CPU 迁移，
     * 或者中断处理程序可能已经补充了 freelist。
     */
    freelist = c->freelist;
    if (freelist)
        goto load_freelist;

    /*
     * ★ 情况 D：per-CPU freelist 为空，但 slab page 还有对象
     * → 从 page 的 regular freelist 获取
     *
     * get_freelist() 将 page->freelist 中的所有对象转移到 per-CPU freelist
     * 通过 cmpxchg_double 原子操作（不需要 node lock）
     */
    freelist = get_freelist(s, page);

    if (!freelist) {
        /* page 上也没有可用对象了 → 放弃这个 page */
        c->page = NULL;
        stat(s, DEACTIVATE_BYPASS);
        goto new_slab;
    }

    stat(s, ALLOC_REFILL);

load_freelist:
    /*
     * ★ 成功获取到 freelist
     * → 设置 per-CPU 的 freelist 为第二个对象（第一个被返回给调用者）
     * → 递增 tid
     */
    VM_BUG_ON(!c->page->frozen);
    c->freelist = get_freepointer(s, freelist);   /* freelist 头指针指向第二个对象 */
    c->tid = next_tid(c->tid);                    /* 递增事务 ID */
    return freelist;                              /* 返回第一个对象给调用者 */

new_slab:
    /*
     * ★ 情况 E：需要获取新的 slab page
     *
     * 先尝试 per-CPU partial list（无需 lock）
     */
    if (slub_percpu_partial(c)) {
        page = c->page = slub_percpu_partial(c);
        slub_set_percpu_partial(c, page);   /* 链表头后移 */
        stat(s, CPU_PARTIAL_ALLOC);
        goto redo;
    }

    /*
     * 最后手段：从 per-node partial list 获取，或向伙伴系统申请
     *
     * new_slab_objects() 内部调用：
     *   → get_partial()     — 从 node partial list 取 slab
     *   → new_slab()        — 向伙伴系统申请新页
     */
    freelist = new_slab_objects(s, gfpflags, node, &c);

    if (unlikely(!freelist)) {
        slab_out_of_memory(s, gfpflags, node);
        return NULL;
    }

    page = c->page;
    if (likely(!kmem_cache_debug(s) && pfmemalloc_match(page, gfpflags)))
        goto load_freelist;

    /* 调试模式或 pfmemalloc 不匹配时的处理 */
    if (kmem_cache_debug(s) &&
            !alloc_debug_processing(s, page, freelist, addr))
        goto new_slab;    /* Slab failed checks. Next slab needed */

    deactivate_slab(s, page, get_freepointer(s, freelist), c);
    return freelist;
}


/* ======================================================================
 * ★ 核心函数 #4: get_freelist — 从 slab page 的 freelist 获取对象
 * ======================================================================
 *
 * 当 per-CPU freelist 耗尽但 slab page 上还有对象时调用。
 * 此函数将 page 的 regular freelist 一次性转移到 per-CPU freelist。
 *
 * ★ 关键：使用 cmpxchg_double 原子操作，不需要 spinlock。
 *   - 比较 page->freelist 和 page->counters
 *   - 如果匹配，将 freelist 设为 NULL（全部取走），更新 counters
 *   - frozen=1 保证只有拥有此 page 的 CPU 才会调用此函数
 *
 * 返回值：
 *   - 非 NULL：成功获取的 freelist 头指针
 *   - NULL：page 上没有可用对象（被其他 CPU 远程 free 抢走了）
 */
static inline void *get_freelist(struct kmem_cache *s, struct page *page)
{
    struct page new;
    unsigned long counters;
    void *freelist;

    do {
        /* 读取当前 page 的 freelist 和 counters（原子对） */
        freelist = page->freelist;
        counters = page->counters;

        /* 构造新值 */
        new.counters = counters;
        VM_BUG_ON(!new.frozen);   /* ★ page 必须是 frozen 状态 */

        new.inuse = page->objects;  /* 标记所有对象都在使用中 */
        new.frozen = freelist != NULL;  /* 如果还有对象，保持 frozen */

    } while (!__cmpxchg_double_slab(s, page,
        freelist, counters,          /* 期望的旧值 */
        NULL, new.counters,          /* 设置的新值：freelist=NULL（全部取走） */
        "get_freelist"));

    return freelist;
}


/* ======================================================================
 * ★ 核心函数 #5: get_partial — 从 node partial list 获取 slab
 * ======================================================================
 *
 * 当 per-CPU 层（freelist + partial list）都没有可用 slab 时，
 * 需要从 per-node partial list 中获取。
 *
 * ★ 需要获取 n->list_lock（spinlock）→ 竞争点！
 *
 * 查找策略：
 *   1. 先从本 node 的 partial list 找
 *   2. 如果本 node 没有，且允许跨 node，尝试其他 node（get_any_partial）
 */
static void *get_partial(struct kmem_cache *s, gfp_t flags, int node,
        struct kmem_cache_cpu *c)
{
    void *object;
    int searchnode = node;

    /* 如果未指定 node，使用当前 CPU 所在的 node */
    if (node == NUMA_NO_NODE)
        searchnode = numa_mem_id();

    /* ★ 从指定 node 的 partial list 获取 */
    object = get_partial_node(s, get_node(s, searchnode), c, flags);
    if (object || node != NUMA_NO_NODE)
        return object;

    /* 本 node 没有可用的 partial slab → 尝试其他 node */
    return get_any_partial(s, flags, c);
}

/*
 * get_partial_node — 从指定 node 的 partial list 获取 slab
 *
 * ★ 这是 per-node partial list 操作的核心函数
 * ★ 需要持有 n->list_lock
 *
 * 不仅获取一个 slab，还会"预热"per-CPU partial list：
 * 多取几个 slab 放到 c->partial 中，减少下次再来的概率。
 */
static void *get_partial_node(struct kmem_cache *s, struct kmem_cache_node *n,
                struct kmem_cache_cpu *c, gfp_t flags)
{
    struct page *page, *page2;
    void *object = NULL;
    unsigned int available = 0;
    int objects;

    /* 无锁检查：如果 partial list 为空，直接返回 */
    if (!n || !n->nr_partial)
        return NULL;

    /* ★ 获取 node lock — 竞争点！ */
    spin_lock(&n->list_lock);

    /* 遍历 partial list 中的每个 slab */
    list_for_each_entry_safe(page, page2, &n->partial, slab_list) {
        void *t;

        if (!pfmemalloc_match(page, flags))
            continue;

        /*
         * acquire_slab：将 slab 从 partial list 中取出并"冻结"
         * - 第一个 slab：object!=NULL 时设为 false，表示只冻结、不作为当前 slab
         * - 第一个 slab：object==NULL 时设为 true，作为当前 slab
         */
        t = acquire_slab(s, n, page, object == NULL, &objects);
        if (!t)
            break;

        available += objects;
        if (!object) {
            /* ★ 第一个 slab 作为当前 slab 使用 */
            c->page = page;
            stat(s, ALLOC_FROM_PARTIAL);
            object = t;
        } else {
            /* ★ 后续的 slab 放到 per-CPU partial list 中（预热） */
            put_cpu_partial(s, page, 0);
            stat(s, CPU_PARTIAL_NODE);
        }
        /*
         * 预热策略：获取到 cpu_partial/2 个对象后就停止
         * 避免一次取太多 slab 导致其他 CPU 分配时也要去 node 层
         */
        if (!kmem_cache_has_cpu_partial(s)
            || available > slub_cpu_partial(s) / 2)
            break;
    }
    spin_unlock(&n->list_lock);
    return object;
}


/* ======================================================================
 * ★ 核心函数 #6: new_slab — 向伙伴系统申请新的 slab
 * ======================================================================
 *
 * 当 partial list 也没有可用 slab 时，需要向伙伴系统申请新的物理页。
 * 这是最慢的路径，涉及 zone->lock 竞争。
 *
 * 调用链：
 *   new_slab() → allocate_slab() → __alloc_pages()
 *
 * allocate_slab 决定 order（页数）：
 *   - 优先使用 oo（optimal order）：平衡碎片和效率
 *   - 如果内存紧张，降级到 min（最小 order）
 */
static struct page *new_slab(struct kmem_cache *s, gfp_t flags, int node)
{
    if (unlikely(flags & GFP_SLAB_BUG_MASK))
        flags = kmalloc_fix_flags(flags);

    return allocate_slab(s,
        flags & (GFP_RECLAIM_MASK | GFP_CONSTRAINT_MASK), node);
}


/* ======================================================================
 * ★ 核心函数 #7: deactivate_slab — 归还 per-CPU slab
 * ======================================================================
 *
 * 当 CPU 需要切换到新的 slab 时（node 不匹配、pfmemalloc 等），
 * 需要将当前 slab 归还。
 *
 * 归还的目标状态取决于 slab 的使用情况：
 *   - 全部对象空闲 + node partial 够多 → M_FREE（释放整个 slab）
 *   - 还有空闲对象 → M_PARTIAL（放回 node partial list）
 *   - 所有对象都在使用 → M_FULL（放入 full list，仅调试用）
 *
 * ★ 归还操作需要 n->list_lock（对于 M_PARTIAL 和 M_FULL）
 *
 * 两阶段操作：
 *   Stage 1：在 frozen 状态下，将 per-CPU freelist 中的对象归还到 page->freelist
 *   Stage 2：解冻 page 并将其放到正确的链表上
 */
static void deactivate_slab(struct kmem_cache *s, struct page *page,
                void *freelist, struct kmem_cache_cpu *c)
{
    enum slab_modes { M_NONE, M_PARTIAL, M_FULL, M_FREE };
    struct kmem_cache_node *n = get_node(s, page_to_nid(page));
    int lock = 0;
    enum slab_modes l = M_NONE, m = M_NONE;
    void *nextfree;
    int tail = DEACTIVATE_TO_HEAD;
    struct page new;
    struct page old;

    /* 如果 page->freelist 非空，说明有远程 free 发生 → 放到链表尾部 */
    if (page->freelist) {
        stat(s, DEACTIVATE_REMOTE_FREES);
        tail = DEACTIVATE_TO_TAIL;
    }

    /*
     * Stage 1: 将 per-CPU freelist 的对象还回到 page->freelist
     *
     * 因为 page 仍然是 frozen 状态，只有本 CPU 在操作，
     * 所以 cmpxchg_double 不需要 node lock。
     */
    while (freelist && (nextfree = get_freepointer(s, freelist))) {
        void *prior;
        unsigned long counters;

        if (freelist_corrupted(s, page, &freelist, nextfree))
            break;

        do {
            prior = page->freelist;
            counters = page->counters;
            set_freepointer(s, freelist, prior);  /* 将当前对象链接到 freelist */
            new.counters = counters;
            new.inuse--;                          /* 减少使用中的对象计数 */
            VM_BUG_ON(!new.frozen);

        } while (!__cmpxchg_double_slab(s, page,
            prior, counters,
            freelist, new.counters,
            "drain percpu freelist"));

        freelist = nextfree;
    }

    /*
     * Stage 2: 解冻 page，放到正确的链表上
     *
     * 使用 cmpxchg_double 原子地修改 freelist + counters，
     * 同时设置 frozen=0。
     *
     * 如果有并发（远程 free 修改了 page），cmpxchg 会失败，
     * 需要 redo。
     */
redo:
    old.freelist = page->freelist;
    old.counters = page->counters;
    VM_BUG_ON(!old.frozen);

    new.counters = old.counters;
    if (freelist) {
        new.inuse--;
        set_freepointer(s, freelist, old.freelist);
        new.freelist = freelist;
    } else
        new.freelist = old.freelist;

    new.frozen = 0;   /* ★ 解冻 */

    /* 判断目标状态 */
    if (!new.inuse && n->nr_partial >= s->min_partial)
        m = M_FREE;       /* 所有对象都空闲 → 释放 slab */
    else if (new.freelist) {
        m = M_PARTIAL;    /* 还有空闲对象 → 放入 partial list */
        if (!lock) {
            lock = 1;
            /* ★ 获取 node lock */
            spin_lock(&n->list_lock);
        }
    } else {
        m = M_FULL;       /* 全部在使用 → 放入 full list */
    }

    /* 更新链表成员关系 */
    if (l != m) {
        if (l == M_PARTIAL)
            remove_partial(n, page);
        else if (l == M_FULL)
            remove_full(s, n, page);

        if (m == M_PARTIAL)
            add_partial(n, page, tail);
        else if (m == M_FULL)
            add_full(s, n, page);
    }

    l = m;
    /* ★ 原子更新：如果远程 free 发生了，这里会失败并 redo */
    if (!__cmpxchg_double_slab(s, page,
                old.freelist, old.counters,
                new.freelist, new.counters,
                "unfreezing slab"))
        goto redo;

    if (lock)
        spin_unlock(&n->list_lock);

    if (m == M_PARTIAL)
        stat(s, tail);
    else if (m == M_FULL)
        stat(s, DEACTIVATE_FULL);
    else if (m == M_FREE) {
        stat(s, DEACTIVATE_EMPTY);
        discard_slab(s, page);    /* 释放整个 slab → __free_pages() */
        stat(s, FREE_SLAB);
    }

    c->page = NULL;
    c->freelist = NULL;
}


/* ======================================================================
 * ★ 核心函数 #8: put_cpu_partial — 放入 per-CPU partial list
 * ======================================================================
 *
 * 当 get_partial_node 获取了多个 slab 时，多余的 slab 通过此函数
 * 放入 per-CPU partial list，供后续快速使用。
 *
 * ★ 使用 this_cpu_cmpxchg 更新链表头（单指针 CAS，不需要 lock）
 *
 * 当 per-CPU partial 中的对象数超过阈值时，会触发 drain：
 * 将所有 per-CPU partial slab 归还到 node partial list（unfreeze_partials）
 */
static void put_cpu_partial(struct kmem_cache *s, struct page *page, int drain)
{
#ifdef CONFIG_SLUB_CPU_PARTIAL
    struct page *oldpage;
    int pages;
    int pobjects;

    preempt_disable();
    do {
        pages = 0;
        pobjects = 0;
        oldpage = this_cpu_read(s->cpu_slab->partial);

        if (oldpage) {
            pobjects = oldpage->pobjects;
            pages = oldpage->pages;
            if (drain && pobjects > slub_cpu_partial(s)) {
                unsigned long flags;
                /*
                 * ★ per-CPU partial 已满 → drain 到 node partial list
                 * 需要关中断 + 获取 node lock
                 */
                local_irq_save(flags);
                unfreeze_partials(s, this_cpu_ptr(s->cpu_slab));
                local_irq_restore(flags);
                oldpage = NULL;
                pobjects = 0;
                pages = 0;
                stat(s, CPU_PARTIAL_DRAIN);
            }
        }

        pages++;
        pobjects += page->objects - page->inuse;

        page->pages = pages;
        page->pobjects = pobjects;
        page->next = oldpage;   /* 链接到旧的链表头 */

        /* ★ 原子更新链表头 */
    } while (this_cpu_cmpxchg(s->cpu_slab->partial, oldpage, page)
                                    != oldpage);
    preempt_enable();
#endif  /* CONFIG_SLUB_CPU_PARTIAL */
}


/*
 * ★ 总结：SLUB 分配的完整调用链
 *
 *  kmem_cache_alloc(s, GFP_KERNEL)
 *    → slab_alloc(s, gfpflags, _RET_IP_)
 *      → slab_alloc_node(s, gfpflags, NUMA_NO_NODE, addr)
 *        │
 *        ├─ [快路径] this_cpu_cmpxchg_double(freelist, tid)
 *        │   → 成功：直接返回对象（无锁！）
 *        │
 *        └─ [慢路径] __slab_alloc → ___slab_alloc
 *            │
 *            ├─ [1] get_freelist(page)
 *            │   → cmpxchg_double_slab: 从 page->freelist 取对象
 *            │
 *            ├─ [2] slub_percpu_partial(c)
 *            │   → 从 per-CPU partial list 取 frozen slab
 *            │
 *            ├─ [3] get_partial() → get_partial_node()
 *            │   → spin_lock(n->list_lock)
 *            │   → acquire_slab: 从 node partial list 取 slab
 *            │
 *            └─ [4] new_slab() → allocate_slab() → __alloc_pages()
 *                → 向伙伴系统申请物理页（最慢路径）
 *
 *  ★ 锁竞争热点排序（从少到多）：
 *    1. per-CPU freelist  → 无锁（cmpxchg_double）
 *    2. per-CPU partial   → 无锁（this_cpu_cmpxchg）
 *    3. node partial list → n->list_lock（per-node 粒度）
 *    4. 伙伴系统          → zone->lock（per-zone 粒度，全局热点）
 *
 *  ★ 性能优化建议：
 *    - 高并发分配同一大小对象 → 考虑增大 cpu_partial 值
 *    - NUMA 系统 → 确保分配在本地 node（避免 get_any_partial）
 *    - 减少 slab 创建/销毁频率 → 使用 kmem_cache 而非频繁 kmalloc/kfree
 */
