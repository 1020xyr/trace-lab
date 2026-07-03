/*
 * ======================================================================
 * Step 2: 伙伴系统（Buddy Allocator）— 物理页分配的最后一站
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/mm/page_alloc.c (8000+ 行)
 *
 * 本文件关注伙伴系统的**分配路径**：
 *   1. __alloc_pages_nodemask() — 分配物理页的入口
 *   2. get_page_from_freelist() — 遍历 zonelist 查找可用 zone
 *   3. rmqueue() — 从 zone 分配页面
 *   4. __rmqueue_pcplist / rmqueue_bulk — per-CPU pageset 机制
 *   5. __rmqueue_smallest — 从 buddy freelist 分配（核心算法）
 *   6. free_pcppages_bulk — per-CPU pageset 的回收
 *   7. zone_watermark_fast — 水位线检查
 *
 * 预计阅读时间：25 分钟
 * ======================================================================
 *
 * ★ 伙伴系统架构（★ 核心！）
 *
 *   ┌─────────────────────────────────────────────────────────┐
 *   │              页面分配路径（从快到慢）                       │
 *   ├─────────────────────────────────────────────────────────┤
 *   │                                                         │
 *   │  Layer 1: per-CPU pageset（最快，仅需关中断）              │
 *   │    per_cpu_pages.lists[migratetype] → page1 → page2     │
 *   │    ★ 仅限 order-0 分配                                   │
 *   │    ★ 不需要 zone->lock                                   │
 *   │                                                         │
 *   │  Layer 2: zone buddy freelist（需要 zone->lock）          │
 *   │    zone->free_area[order][migratetype].free_list         │
 *   │    ★ 高位阶分配必须走这条路径                              │
 *   │    ★ zone->lock 是全局竞争热点                            │
 *   │                                                         │
 *   │  Layer 3: 内存回收（最慢）                                │
 *   │    __alloc_pages_slowpath → 直接回收 / kswapd             │
 *   │    ★ 涉及页面换出、slab shrink 等                         │
 *   │                                                         │
 *   └─────────────────────────────────────────────────────────┘
 *
 *   伙伴系统 freelist 结构：
 *
 *     zone->free_area[order]
 *       ┌─────────────────────────────────────────┐
 *       │ order 0: MIGRATE_UNMOVABLE → page → page│
 *       │          MIGRATE_MOVABLE   → page → page│
 *       │          MIGRATE_RECLAIMABLE → page     │
 *       ├─────────────────────────────────────────┤
 *       │ order 1: MIGRATE_UNMOVABLE → page_pair  │
 *       │          MIGRATE_MOVABLE   → page_pair  │
 *       ├─────────────────────────────────────────┤
 *       │ order 2: ...                            │
 *       │ ...                                     │
 *       │ order 10: (MAX_ORDER-1)                 │
 *       └─────────────────────────────────────────┘
 *
 *   ★ 关键性能瓶颈：
 *   - zone->lock 保护整个 zone 的 buddy freelist
 *   - 高并发分配时，多 CPU 同时竞争 zone->lock
 *   - per-CPU pageset 的作用就是减少对 zone->lock 的访问
 *     （批量预取 + 批量归还）
 */


/* ======================================================================
 * ★ 核心函数 #1: __alloc_pages_nodemask — 物理页分配入口
 * ======================================================================
 *
 * 这是所有物理页分配的统一入口（alloc_pages、__get_free_pages 等最终都到这里）。
 *
 * 两步策略：
 *   1. 快速路径：get_page_from_freelist() — 从 freelist 直接分配
 *   2. 慢速路径：__alloc_pages_slowpath() — 内存回收、压缩、OOM 等
 */
struct page *
__alloc_pages_nodemask(gfp_t gfp_mask, unsigned int order, int preferred_nid,
                            nodemask_t *nodemask)
{
    struct page *page;
    unsigned int alloc_flags = ALLOC_WMARK_LOW;  /* ★ 默认使用 low watermark */
    gfp_t alloc_mask;
    struct alloc_context ac = { };

    /* 边界检查：order 不能超过 MAX_ORDER-1（通常是 11） */
    if (unlikely(order >= MAX_ORDER)) {
        WARN_ON_ONCE(!(gfp_mask & __GFP_NOWARN));
        return NULL;
    }

    /* 过滤掉不合法的 gfp 标志 */
    gfp_mask &= gfp_allowed_mask;
    alloc_mask = gfp_mask;

    /* 准备分配上下文：计算 zonelist、migratetype 等 */
    if (!prepare_alloc_pages(gfp_mask, order, preferred_nid, nodemask,
                             &ac, &alloc_mask, &alloc_flags))
        return NULL;

    /* 第一次尝试：避免跨 node 碎片化 */
    alloc_flags |= alloc_flags_nofragment(ac.preferred_zoneref->zone, gfp_mask);

    /* ★ 快速路径：从 freelist 分配 */
    page = get_page_from_freelist(alloc_mask, order, alloc_flags, &ac);
    if (likely(page))
        goto out;

    /*
     * ★ 慢速路径：快速路径失败
     * → 尝试内存回收（直接回收 / 唤醒 kswapd）
     * → 尝试内存压缩（compaction）
     * → 尝试 OOM killer
     */
    alloc_mask = current_gfp_context(gfp_mask);
    ac.spread_dirty_pages = false;
    ac.nodemask = nodemask;

    page = __alloc_pages_slowpath(alloc_mask, order, &ac);

out:
    /* cgroup 内存计费 */
    if (memcg_kmem_enabled() && (gfp_mask & __GFP_ACCOUNT) && page &&
        unlikely(__memcg_kmem_charge_page(page, gfp_mask, order) != 0)) {
        __free_pages(page, order);
        page = NULL;
    }

    trace_mm_page_alloc(page, order, alloc_mask, ac.migratetype);

    return page;
}
EXPORT_SYMBOL(__alloc_pages_nodemask);


/* ======================================================================
 * ★ 核心函数 #2: get_page_from_freelist — 遍历 zonelist 分配
 * ======================================================================
 *
 * 遍历 zonelist 中的每个 zone，检查水位线，找到第一个满足条件的 zone
 * 后从中分配页面。
 *
 * 关键检查：
 *   1. cpuset 约束（进程允许使用的 node 集合）
 *   2. dirty limit（避免单个 node 的脏页过多）
 *   3. 碎片化避免（ALLOC_NOFRAGMENT）
 *   4. ★ 水位线检查（zone_watermark_fast）
 *   5. 实际分配（rmqueue）
 */
static struct page *
get_page_from_freelist(gfp_t gfp_mask, unsigned int order, int alloc_flags,
                        const struct alloc_context *ac)
{
    struct zoneref *z;
    struct zone *zone;
    struct pglist_data *last_pgdat_dirty_limit = NULL;
    bool no_fallback;

retry:
    no_fallback = alloc_flags & ALLOC_NOFRAGMENT;
    z = ac->preferred_zoneref;

    /* ★ 遍历 zonelist 中的每个 zone */
    for_next_zone_zonelist_nodemask(zone, z, ac->highest_zoneidx,
                    ac->nodemask) {
        struct page *page;
        unsigned long mark;

        /* cpuset 检查：进程是否允许在此 node 分配 */
        if (cpusets_enabled() &&
            (alloc_flags & ALLOC_CPUSET) &&
            !__cpuset_zone_allowed(zone, gfp_mask))
                continue;

        /* 脏页均衡：避免在脏页过多的 node 上继续分配 */
        if (ac->spread_dirty_pages) {
            if (last_pgdat_dirty_limit == zone->zone_pgdat)
                continue;
            if (!node_dirty_ok(zone->zone_pgdat)) {
                last_pgdat_dirty_limit = zone->zone_pgdat;
                continue;
            }
        }

        /* 碎片化避免：如果跨 node 了，放弃碎片化避免策略重试 */
        if (no_fallback && nr_online_nodes > 1 &&
            zone != ac->preferred_zoneref->zone) {
            int local_nid = zone_to_nid(ac->preferred_zoneref->zone);
            if (zone_to_nid(zone) != local_nid) {
                alloc_flags &= ~ALLOC_NOFRAGMENT;
                goto retry;
            }
        }

        /*
         * ★ 水位线检查
         *
         * 根据 alloc_flags 决定使用哪个水位线：
         *   - ALLOC_WMARK_MIN  → min watermark（紧急分配）
         *   - ALLOC_WMARK_LOW  → low watermark（默认，kswapd 的目标）
         *   - ALLOC_WMARK_HIGH → high watermark（kswapd 唤醒阈值）
         */
        mark = wmark_pages(zone, alloc_flags & ALLOC_WMARK_MASK);

        if (!zone_watermark_fast(zone, order, mark,
                       ac->highest_zoneidx, alloc_flags,
                       gfp_mask)) {
            int ret;

            /* 水位线不满足 → 尝试 node_reclaim 或跳过 */
            if (alloc_flags & ALLOC_NO_WATERMARKS)
                goto try_this_zone;   /* 紧急分配：忽略水位线 */

            if (node_reclaim_mode == 0 ||
                !zone_allows_reclaim(ac->preferred_zoneref->zone, zone))
                continue;   /* 跳过此 zone */

            ret = node_reclaim(zone->zone_pgdat, gfp_mask, order);
            switch (ret) {
            case NODE_RECLAIM_NOSCAN:
                continue;
            case NODE_RECLAIM_FULL:
                continue;
            default:
                if (zone_watermark_ok(zone, order, mark,
                    ac->highest_zoneidx, alloc_flags))
                    goto try_this_zone;
                continue;
            }
        }

try_this_zone:
        /* ★ 水位线满足 → 从 zone 中分配页面 */
        page = rmqueue(ac->preferred_zoneref->zone, zone, order,
                gfp_mask, alloc_flags, ac->migratetype);
        if (page) {
            prep_new_page(page, order, gfp_mask, alloc_flags);
            return page;
        }
    }

    /* 所有 zone 都失败 → 如果有碎片化避免标志，去掉后重试 */
    if (no_fallback) {
        alloc_flags &= ~ALLOC_NOFRAGMENT;
        goto retry;
    }

    return NULL;
}


/* ======================================================================
 * ★ 核心函数 #3: rmqueue — 从 zone 分配页面
 * ======================================================================
 *
 * 这是从指定 zone 分配页面的核心函数。
 *
 * 两条路径：
 *   - order-0：优先使用 per-CPU pageset（快速，无 zone->lock）
 *   - 高阶或 CMA 场景：直接操作 buddy freelist（需要 zone->lock）
 */
static inline
struct page *rmqueue(struct zone *preferred_zone,
            struct zone *zone, unsigned int order,
            gfp_t gfp_flags, unsigned int alloc_flags,
            int migratetype)
{
    unsigned long flags;
    struct page *page;

    if (likely(order == 0)) {
        /*
         * ★ order-0 快速路径：从 per-CPU pageset 分配
         *
         * per-CPU pageset 是每个 CPU 维护的小型页面缓存，
         * 避免每次分配 order-0 页面都需要获取 zone->lock。
         */
        if (!IS_ENABLED(CONFIG_CMA) || alloc_flags & ALLOC_CMA ||
                migratetype != MIGRATE_MOVABLE) {
            page = rmqueue_pcplist(preferred_zone, zone, gfp_flags,
                    migratetype, alloc_flags);
            goto out;
        }
    }

    /*
     * ★ 高阶分配路径（order > 0）或 CMA 限制场景
     * → 需要获取 zone->lock
     */
    WARN_ON_ONCE((gfp_flags & __GFP_NOFAIL) && (order > 1));
    spin_lock_irqsave(&zone->lock, flags);   /* ★ 获取 zone lock */

    do {
        page = NULL;
        /*
         * 高阶原子分配优先从 HIGHATOMIC 区域分配
         * （预留区域，避免被普通分配消耗）
         */
        if (order > 0 && alloc_flags & ALLOC_HARDER) {
            page = __rmqueue_smallest(zone, order, MIGRATE_HIGHATOMIC);
            if (page)
                trace_mm_page_alloc_zone_locked(page, order, migratetype);
        }
        /*
         * ★ 从 buddy freelist 中查找最小匹配
         * __rmqueue 内部会尝试 fallback 到其他 migratetype
         */
        if (!page)
            page = __rmqueue(zone, order, migratetype, alloc_flags);
    } while (page && check_new_pages(page, order));

    spin_unlock(&zone->lock);    /* ★ 释放 zone lock */

    if (!page)
        goto failed;
    __mod_zone_freepage_state(zone, -(1 << order),
                  get_pcppage_migratetype(page));

out:
    /* 更新统计信息 */
    __count_zid_vm_events(PGALLOC, page_zonenum(page), 1 << order);
    zone_statistics(preferred_zone, zone);
    local_irq_restore(flags);
    return page;

failed:
    local_irq_restore(flags);
    return NULL;
}


/* ======================================================================
 * ★ 核心函数 #4: __rmqueue_smallest — 伙伴系统的核心分配算法
 * ======================================================================
 *
 * 这是伙伴系统最核心的函数——从 freelist 中找到满足 order 的最小页面块。
 *
 * 算法：
 *   1. 从请求的 order 开始，逐层向上查找
 *   2. 找到第一个有可用页面的 order
 *   3. 将大块拆分成小块（expand），剩余部分放回对应 order 的 freelist
 *
 * ★ 这就是"伙伴"名称的由来：
 *   2^k 大小的块可以拆成两个 2^(k-1) 的"伙伴"块
 */
static __always_inline
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
                        int migratetype)
{
    unsigned int current_order;
    struct free_area *area;
    struct page *page;

    /* ★ 从请求的 order 开始，向上查找直到 MAX_ORDER */
    for (current_order = order; current_order < MAX_ORDER; ++current_order) {
        area = &(zone->free_area[current_order]);

        /* 尝试从 migratetype 对应的 freelist 获取 */
        page = get_page_from_free_area(area, migratetype);
        if (!page)
            continue;   /* 此 order 没有可用页面，继续向上找 */

        /* ★ 从 freelist 中删除此页面 */
        del_page_from_free_list(page, zone, current_order);

        /*
         * ★ expand：将大块拆分成小块
         *
         * 例如：请求 order=1，找到 order=3 的块
         *   → 拆成 order=1（返回）+ order=1 + order=2
         *   → order=1 和 order=2 的剩余块放回到对应 freelist
         */
        expand(zone, page, order, current_order, migratetype);

        set_pcppage_migratetype(page, migratetype);
        return page;
    }

    return NULL;   /* 所有 order 都没有可用页面 */
}


/* ======================================================================
 * ★ 核心函数 #5: per-CPU pageset 机制
 * ======================================================================
 *
 * per-CPU pageset 是 order-0 页面的本地缓存，目的是避免每次都获取 zone->lock。
 *
 * 分配流程：
 *   rmqueue_pcplist → __rmqueue_pcplist
 *     → per-CPU list 为空时，调用 rmqueue_bulk 批量填充
 *
 * 释放流程：
 *   free_unref_page → free_pcppages_bulk
 *     → per-CPU list 满时，批量归还到 zone buddy freelist
 */

/*
 * __rmqueue_pcplist — 从 per-CPU pageset 分配
 *
 * ★ 仅需关中断，不需要 zone->lock
 */
static struct page *__rmqueue_pcplist(struct zone *zone, int migratetype,
            unsigned int alloc_flags,
            struct per_cpu_pages *pcp,
            struct list_head *list)
{
    struct page *page;

    do {
        if (list_empty(list)) {
            /*
             * ★ per-CPU list 为空 → 从 buddy freelist 批量填充
             *
             * rmqueue_bulk 会获取 zone->lock，
             * 一次性取 pcp->batch 个页面到 per-CPU list。
             *
             * ★ 这是 per-CPU pageset 的关键设计：
             *   批量操作，分摊 zone->lock 的竞争开销
             */
            pcp->count += rmqueue_bulk(zone, 0,
                    pcp->batch, list,
                    migratetype, alloc_flags);
            if (unlikely(list_empty(list)))
                return NULL;
        }

        /* 从 per-CPU list 取第一个页面 */
        page = list_first_entry(list, struct page, lru);
        list_del(&page->lru);
        pcp->count--;
    } while (check_new_pcp(page));

    return page;
}

/*
 * rmqueue_pcplist — 关中断保护下调用 __rmqueue_pcplist
 */
static struct page *rmqueue_pcplist(struct zone *preferred_zone,
            struct zone *zone, gfp_t gfp_flags,
            int migratetype, unsigned int alloc_flags)
{
    struct per_cpu_pages *pcp;
    struct list_head *list;
    struct page *page;
    unsigned long flags;

    local_irq_save(flags);     /* ★ 关中断保护 per-CPU 数据 */
    pcp = &this_cpu_ptr(zone->pageset)->pcp;
    list = &pcp->lists[migratetype];
    page = __rmqueue_pcplist(zone, migratetype, alloc_flags, pcp, list);
    if (page) {
        __count_zid_vm_events(PGALLOC, page_zonenum(page), 1);
        zone_statistics(preferred_zone, zone);
    }
    local_irq_restore(flags);
    return page;
}


/* ======================================================================
 * ★ 核心函数 #6: rmqueue_bulk — 批量从 buddy freelist 填充 per-CPU pageset
 * ======================================================================
 *
 * 当 per-CPU pageset 耗尽时调用。
 * 一次性从 buddy freelist 取 pcp->batch 个页面。
 *
 * ★ 需要 zone->lock
 * ★ 批量操作是分摊锁竞争的关键
 */
static int rmqueue_bulk(struct zone *zone, unsigned int order,
            unsigned long count, struct list_head *list,
            int migratetype, unsigned int alloc_flags)
{
    int i, alloced = 0;

    spin_lock(&zone->lock);    /* ★ 获取 zone lock */

    for (i = 0; i < count; ++i) {
        /* 从 buddy freelist 分配单个页面 */
        struct page *page = __rmqueue(zone, order, migratetype,
                        alloc_flags);
        if (unlikely(page == NULL))
            break;

        if (unlikely(check_pcp_refill(page)))
            continue;

        /*
         * 页面按物理地址顺序添加到链表尾部。
         * 这对 I/O 设备有利——连续物理页可以合并 I/O 请求。
         */
        list_add_tail(&page->lru, list);
        alloced++;

        if (is_migrate_cma(get_pcppage_migratetype(page)))
            __mod_zone_page_state(zone, NR_FREE_CMA_PAGES,
                          -(1 << order));
    }

    /* 更新全局空闲页计数 */
    __mod_zone_page_state(zone, NR_FREE_PAGES, -(i << order));

    spin_unlock(&zone->lock);  /* ★ 释放 zone lock */
    return alloced;
}


/* ======================================================================
 * ★ 核心函数 #7: free_pcppages_bulk — per-CPU pageset 的批量归还
 * ======================================================================
 *
 * 当 per-CPU pageset 中的页面数超过阈值时，
 * 批量归还到 zone 的 buddy freelist。
 *
 * 过程：
 *   1. 先从 per-CPU list 中取出页面（无锁）
 *   2. 再获取 zone->lock，批量归还到 buddy freelist
 *
 * ★ 先无锁收集，再有锁归还 — 减少 zone->lock 持有时间
 */
static void free_pcppages_bulk(struct zone *zone, int count,
                    struct per_cpu_pages *pcp)
{
    int migratetype = 0;
    int batch_free = 0;
    int prefetch_nr = 0;
    bool isolated_pageblocks;
    struct page *page, *tmp;
    LIST_HEAD(head);

    count = min(pcp->count, count);

    /*
     * ★ 第一阶段：从 per-CPU list 收集页面（无锁）
     *
     * 采用轮转方式从不同 migratetype 的 list 中取页面，
     * 避免只清空某一类。
     */
    while (count) {
        struct list_head *list;

        do {
            batch_free++;
            if (++migratetype == MIGRATE_PCPCYPES)
                migratetype = 0;
            list = &pcp->lists[migratetype];
        } while (list_empty(list));

        /* 如果只有一个非空 list，全部取出 */
        if (batch_free == MIGRATE_PCPCYPES)
            batch_free = count;

        do {
            page = list_last_entry(list, struct page, lru);
            list_del(&page->lru);
            pcp->count--;

            if (bulkfree_pcp_prepare(page))
                continue;

            list_add_tail(&page->lru, &head);

            /*
             * ★ 预取 buddy 页面
             *
             * 归还页面时需要检查 buddy 是否可以合并，
             * 预取 buddy 的 struct page 到 cache，
             * 减少后续在 zone->lock 下的 cache miss。
             */
            if (prefetch_nr++ < pcp->batch)
                prefetch_buddy(page);
        } while (--count && --batch_free && !list_empty(list));
    }

    /*
     * ★ 第二阶段：获取 zone->lock，批量归还
     */
    spin_lock(&zone->lock);
    isolated_pageblocks = has_isolate_pageblock(zone);

    list_for_each_entry_safe(page, tmp, &head, lru) {
        int mt = get_pcppage_migratetype(page);
        VM_BUG_ON_PAGE(is_migrate_isolate(mt), page);
        if (unlikely(isolated_pageblocks))
            mt = get_pageblock_migratetype(page);

        /* __free_one_page：执行 buddy 合并 */
        __free_one_page(page, page_to_pfn(page), zone, 0, mt, FPI_NONE);
        trace_mm_page_pcpu_drain(page, 0, mt);
    }
    spin_unlock(&zone->lock);
}


/* ======================================================================
 * ★ 核心函数 #8: zone_watermark_fast — 水位线快速检查
 * ======================================================================
 *
 * 判断 zone 的空闲页面是否满足水位线要求。
 *
 * 三个水位线（从低到高）：
 *   min  → 紧急分配的底线（低于此值 → OOM 风险）
 *   low  → kswapd 的目标（低于此值 → 唤醒 kswapd 回收）
 *   high → kswapd 停止的阈值（高于此值 → kswapd 可以休息）
 *
 * ★ 快速检查仅针对 order-0，避免复杂的计算
 */
static inline bool zone_watermark_fast(struct zone *z, unsigned int order,
                unsigned long mark, int highest_zoneidx,
                unsigned int alloc_flags, gfp_t gfp_mask)
{
    long free_pages;

    free_pages = zone_page_state(z, NR_FREE_PAGES);

    /*
     * ★ order-0 快速检查
     *
     * 简单比较 free_pages 和 mark + lowmem_reserve，
     * 减去不可用的页面数（如 highatomic 预留）。
     *
     * 如果满足，直接返回 true — 避免调用慢速的 __zone_watermark_ok
     */
    if (!order) {
        long fast_free;

        fast_free = free_pages;
        fast_free -= __zone_watermark_unusable_free(z, 0, alloc_flags);
        if (fast_free > mark + z->lowmem_reserve[highest_zoneidx])
            return true;
    }

    /* 慢速检查：需要考虑高阶分配的连续性要求 */
    if (__zone_watermark_ok(z, order, mark, highest_zoneidx, alloc_flags,
                    free_pages))
        return true;

    /* GFP_ATOMIC order-0 分配忽略 watermark_boost */
    if (unlikely(!order && (gfp_mask & __GFP_ATOMIC) && z->watermark_boost
        && ((alloc_flags & ALLOC_WMARK_MASK) == WMARK_MIN))) {
        mark = z->_watermark[WMARK_MIN];
        return __zone_watermark_ok(z, order, mark, highest_zoneidx,
                    alloc_flags, free_pages);
    }

    return false;
}


/* ======================================================================
 * prep_new_page — 新分配页面的初始化
 * ======================================================================
 */
static void prep_new_page(struct page *page, unsigned int order, gfp_t gfp_flags,
                unsigned int alloc_flags)
{
    post_alloc_hook(page, order, gfp_flags);

    /* 如果需要初始化页面内容（__GFP_ZERO） */
    if (!free_pages_prezeroed() && want_init_on_alloc(gfp_flags))
        kernel_init_free_pages(page, 1 << order);

    /* 高阶分配：设置 compound page */
    if (order && (gfp_flags & __GFP_COMP))
        prep_compound_page(page, order);

    /* ALLOC_NO_WATERMARKS 分配的页面标记为 pfmemalloc */
    if (alloc_flags & ALLOC_NO_WATERMARKS)
        set_page_pfmemalloc(page);
    else
        clear_page_pfmemalloc(page);
}


/*
 * ★ 总结：物理页分配的完整调用链
 *
 *  alloc_pages(GFP_KERNEL, 0)
 *    → __alloc_pages_nodemask(gfp_mask, order, preferred_nid, nodemask)
 *      │
 *      ├─ [快速路径] get_page_from_freelist()
 *      │   │
 *      │   ├─ zone_watermark_fast() → 水位线检查
 *      │   │
 *      │   └─ rmqueue()
 *      │       │
 *      │       ├─ [order-0] rmqueue_pcplist()  ← per-CPU pageset
 *      │       │   └─ __rmqueue_pcplist()
 *      │       │       └─ [list empty] rmqueue_bulk()  ← zone->lock
 *      │       │           └─ __rmqueue() → __rmqueue_smallest()
 *      │       │
 *      │       └─ [order>0] spin_lock(zone->lock)  ← 直接需要 zone->lock
 *      │           └─ __rmqueue() → __rmqueue_smallest() + expand()
 *      │
 *      └─ [慢速路径] __alloc_pages_slowpath()
 *          ├─ 直接内存回收（shrink_page_list）
 *          ├─ 唤醒 kswapd
 *          ├─ 内存压缩（compaction）
 *          └─ OOM killer
 *
 *  ★ zone->lock 竞争分析：
 *
 *  | 分配类型       | 是否需要 zone->lock | 竞争程度 |
 *  |---------------|--------------------|---------|
 *  | order-0 分配   | 仅 pcp 耗尽时需要   | 低       |
 *  | 高阶分配       | 每次都需要          | 高       |
 *  | order-0 释放   | 仅 pcp 满时需要     | 低       |
 *  | 高阶释放       | 每次都需要          | 高       |
 *
 *  ★ 优化策略：
 *  - 增大 pcp->batch：减少 rmqueue_bulk 频率
 *  - 避免不必要的高阶分配：使用 order-0 + scatterlist
 *  - NUMA 感知：分配在本地 node 的 zone，避免跨 node 锁竞争
 */
