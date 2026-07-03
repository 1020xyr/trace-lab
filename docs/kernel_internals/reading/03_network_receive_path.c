/*
 * ======================================================================
 * Step 3: 网络收包路径 — 从网卡中断到协议栈
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/net/core/dev.c (11000+ 行)
 *
 * 本文件关注网络收包的核心路径：
 *   1. ____napi_schedule() — NAPI 调度入口
 *   2. net_rx_action() — NET_RX_SOFTIRQ 的处理函数
 *   3. napi_poll() — 从网卡 ring buffer 取包
 *   4. process_backlog() — backlog NAPI 的处理
 *   5. __netif_receive_skb() — 协议栈处理入口
 *   6. napi_schedule_prep() — NAPI 状态管理
 *
 * 预计阅读时间：25 分钟
 * ======================================================================
 *
 * ★ 网络收包完整路径（★ 核心！）
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │  网卡硬件                                                      │
 *   │    ↓ DMA 写入数据到 ring buffer                                │
 *   │    ↓ 触发硬件中断 (IRQ)                                        │
 *   │                                                              │
 *   │  硬中断处理 (top half)                                         │
 *   │    driver_irq_handler()                                        │
 *   │      → napi_schedule_prep() — 检查/设置 SCHED 标志             │
 *   │      → __napi_schedule_irqoff()                               │
 *   │          → ____napi_schedule()                                │
 *   │              → list_add_tail(napi, sd->poll_list)              │
 *   │              → __raise_softirq_irqoff(NET_RX_SOFTIRQ)          │
 *   │      → 屏蔽中断（不再处理同 queue 的包）                        │
 *   │                                                              │
 *   │  软中断处理 (bottom half) — net_rx_action()                    │
 *   │    遍历 sd->poll_list:                                         │
 *   │      → napi_poll(napi, budget)                                 │
 *   │          → napi->poll(napi, weight)                            │
 *   │              = driver_poll() 或 process_backlog()              │
 *   │          → 处理收到的 sk_buff                                  │
 *   │      → 预算耗尽或时间超限 → 退出，重新触发软中断                 │
 *   │                                                              │
 *   │  协议栈处理                                                     │
 *   │    __netif_receive_skb(skb)                                    │
 *   │      → XDP（如果配置）                                          │
 *   │      → tc ingress                                             │
 *   │      → ip_rcv() / ipv6_rcv()                                  │
 *   │      → tcp_v4_rcv() / udp_rcv()                               │
 *   │      → socket 接收队列                                         │
 *   │                                                              │
 *   └──────────────────────────────────────────────────────────────┘
 *
 *  ★ 软中断不均衡的原因：
 *    - 网卡的多队列（RSS）通常将不同 flow 映射到不同 CPU
 *    - 但 RSS hash 不保证均匀 — 少数大 flow 集中在某个 CPU
 *    - 软中断只能由触发它的 CPU 处理（除非配置了 RPS）
 *    - 结果：某个 CPU 的 ksoftirqd 占用 100%，而其他 CPU 空闲
 *    - 解决方案：RPS（Receive Packet Steering）或调整 RSS 配置
 */


/* ======================================================================
 * ★ 核心函数 #1: ____napi_schedule — NAPI 调度入口
 * ======================================================================
 *
 * 当网卡驱动收到中断时，调用此函数将 NAPI 实例加入 per-CPU 的
 * poll_list，并触发 NET_RX_SOFTIRQ 软中断。
 *
 * ★ 关键设计：NAPI 是"中断合并"机制
 *   - 第一个包触发硬中断
 *   - 硬中断只负责将 NAPI 加入 poll_list 并触发软中断
 *   - 后续到达的包由软中断的 poll 函数批量处理
 *   - 避免了每个包都触发一次中断的开销
 *
 * ★ 必须在关中断状态下调用
 */
static inline void ____napi_schedule(struct softnet_data *sd,
                     struct napi_struct *napi)
{
    /* ★ 将 NAPI 实例加入当前 CPU 的 poll 链表 */
    list_add_tail(&napi->poll_list, &sd->poll_list);

    /* ★ 触发 NET_RX_SOFTIRQ 软中断 */
    __raise_softirq_irqoff(NET_RX_SOFTIRQ);
}

/*
 * __napi_schedule — 带中断保护的版本
 *
 * 如果调用时中断是开启的，使用此函数。
 */
void __napi_schedule(struct napi_struct *n)
{
    unsigned long flags;

    local_irq_save(flags);
    ____napi_schedule(this_cpu_ptr(&softnet_data), n);
    local_irq_restore(flags);
}
EXPORT_SYMBOL(__napi_schedule);

/*
 * __napi_schedule_irqoff — 中断已关闭时的版本
 *
 * 网卡中断处理中通常中断已经关闭，可以直接调用。
 */
void __napi_schedule_irqoff(struct napi_struct *n)
{
    ____napi_schedule(this_cpu_ptr(&softnet_data), n);
}
EXPORT_SYMBOL(__napi_schedule_irqoff);


/* ======================================================================
 * ★ 核心函数 #2: napi_schedule_prep — NAPI 状态管理
 * ======================================================================
 *
 * 在将 NAPI 加入 poll_list 之前，必须先调用此函数检查状态。
 *
 * 状态标志：
 *   NAPIF_STATE_SCHED   — NAPI 已被调度（正在 poll_list 中）
 *   NAPIF_STATE_DISABLE — NAPI 被禁用（正在 napi_disable）
 *   NAPIF_STATE_MISSED  — 调度请求被错过（已调度但又有新数据到达）
 *
 * ★ 保证同一 NAPI 实例不会被重复加入 poll_list
 */
bool napi_schedule_prep(struct napi_struct *n)
{
    unsigned long val, new;

    do {
        val = READ_ONCE(n->state);

        /* 如果 NAPI 被禁用，不允许调度 */
        if (unlikely(val & NAPIF_STATE_DISABLE))
            return false;

        /* 设置 SCHED 标志 */
        new = val | NAPIF_STATE_SCHED;

        /*
         * ★ 如果 SCHED 已经设置，额外设置 MISSED 标志
         *
         * 这表示在 poll 过程中又有新数据到达。
         * poll 完成后会检查 MISSED 标志，决定是否重新调度。
         */
        new |= (val & NAPIF_STATE_SCHED) / NAPIF_STATE_SCHED *
                           NAPIF_STATE_MISSED;
    } while (cmpxchg(&n->state, val, new) != val);

    /* 返回 true 表示此次成功设置了 SCHED（之前未在调度中） */
    return !(val & NAPIF_STATE_SCHED);
}
EXPORT_SYMBOL(napi_schedule_prep);


/* ======================================================================
 * ★ 核心函数 #3: net_rx_action — NET_RX_SOFTIRQ 的处理函数
 * ======================================================================
 *
 * 这是网络收包软中断的核心处理函数。
 * 在 ksoftirqd 线程或中断返回路径中被调用。
 *
 * 处理逻辑：
 *   1. 将 per-CPU poll_list 转移到本地链表
 *   2. 遍历 poll_list，依次调用 napi_poll
 *   3. 直到预算耗尽或时间超限
 *   4. 未完成的 NAPI 实例重新放回 poll_list，再次触发软中断
 *
 * ★ 两个退出条件（防止软中断霸占 CPU）：
 *   - budget ≤ 0：处理的包数超过 netdev_budget（默认 300）
 *   - 时间超过 netdev_budget_usecs（默认 20000 微秒 = 20ms）
 */
static __latent_entropy void net_rx_action(struct softirq_action *h)
{
    struct softnet_data *sd = this_cpu_ptr(&softnet_data);

    /* ★ 时间限制：最多运行 netdev_budget_usecs 微秒 */
    unsigned long time_limit = jiffies +
        usecs_to_jiffies(netdev_budget_usecs);

    /* ★ 包数预算：最多处理 netdev_budget 个包 */
    int budget = netdev_budget;

    LIST_HEAD(list);
    LIST_HEAD(repoll);

    /* 关中断，将 per-CPU poll_list 转移到本地链表 */
    local_irq_disable();
    list_splice_init(&sd->poll_list, &list);
    local_irq_enable();

    for (;;) {
        struct napi_struct *n;

        if (list_empty(&list)) {
            /* 本地链表为空，检查是否有新加入的或需要 repoll 的 */
            if (!sd_has_rps_ipi_waiting(sd) && list_empty(&repoll))
                goto out;
            break;
        }

        /* 取出链表头的 NAPI 实例，调用 poll */
        n = list_first_entry(&list, struct napi_struct, poll_list);

        /* ★ napi_poll 返回实际处理的包数，从预算中扣除 */
        budget -= napi_poll(n, &repoll);

        /*
         * ★ 检查退出条件
         *
         * 预算耗尽或时间超限 → 退出软中断
         * 将剩余的 NAPI 实例放回 poll_list，重新触发软中断
         * （由 ksoftirqd 线程在下一个调度点继续处理）
         */
        if (unlikely(budget <= 0 ||
                 time_after_eq(jiffies, time_limit))) {
            sd->time_squeeze++;   /* 统计软中断被"挤压"的次数 */
            break;
        }
    }

    /*
     * 将未处理完的 NAPI 实例放回 per-CPU poll_list
     * 并重新触发 NET_RX_SOFTIRQ
     */
    local_irq_disable();

    list_splice_tail_init(&sd->poll_list, &list);
    list_splice_tail(&repoll, &list);
    list_splice(&list, &sd->poll_list);
    if (!list_empty(&sd->poll_list))
        __raise_softirq_irqoff(NET_RX_SOFTIRQ);  /* ★ 重新触发软中断 */

    net_rps_action_and_irq_enable(sd);
out:
    __kfree_skb_flush();
}


/* ======================================================================
 * ★ 核心函数 #4: napi_poll — 调用驱动的 poll 函数
 * ======================================================================
 *
 * 从 NAPI 实例中调用驱动注册的 poll 函数来收取数据包。
 *
 * 处理逻辑：
 *   1. 从 poll_list 中移除 NAPI 实例
 *   2. 调用 napi->poll()（驱动实现）
 *   3. 根据返回值决定：
 *      - work < weight：此 NAPI 的数据已收完，退出
 *      - work == weight：预算用完，放入 repoll 列表稍后继续
 */
static int napi_poll(struct napi_struct *n, struct list_head *repoll)
{
    void *have;
    int work, weight;

    /* 从 poll_list 中移除 */
    list_del_init(&n->poll_list);

    /* netpoll 锁保护 */
    have = netpoll_poll_lock(n);

    weight = n->weight;   /* ★ weight 是每次 poll 最多处理的包数（默认 64） */

    /* 检查 NAPI 是否仍在调度状态（避免与 netpoll 竞争） */
    work = 0;
    if (test_bit(NAPI_STATE_SCHED, &n->state)) {
        /* ★ 调用驱动的 poll 函数 */
        work = n->poll(n, weight);
        trace_napi_poll(n, work, weight);
    }

    /* 驱动不应返回超过 weight 的值 */
    if (unlikely(work > weight))
        pr_err_once("NAPI poll function %pS returned %d, "
                    "exceeding its budget of %d.\n",
                    n->poll, work, weight);

    if (likely(work < weight))
        goto out_unlock;   /* ★ 数据已收完 */

    /*
     * work == weight：预算用完，还有更多数据
     *
     * 检查是否有 napi_disable 请求
     */
    if (unlikely(napi_disable_pending(n))) {
        napi_complete(n);
        goto out_unlock;
    }

    /* flush GRO 中过期的聚合包 */
    if (n->gro_bitmask) {
        napi_gro_flush(n, HZ >= 1000);
    }

    /* 将 GRO 中正常积累的包送到协议栈 */
    gro_normal_list(n);

    /* 放入 repoll 列表，稍后继续 poll */
    if (unlikely(!list_empty(&n->poll_list))) {
        pr_warn_once("%s: Budget exhausted after napi rescheduled\n",
                 n->dev ? n->dev->name : "backlog");
        goto out_unlock;
    }

    /* ★ 将 NAPI 加入 repoll 列表（在 net_rx_action 中稍后继续） */
    list_add_tail(&n->poll_list, repoll);

out_unlock:
    netpoll_poll_unlock(have);

    return work;
}


/* ======================================================================
 * ★ 核心函数 #5: process_backlog — backlog NAPI 的处理
 * ======================================================================
 *
 * backlog 是每个 CPU 上的"软件"NAPI 实例，用于处理：
 *   1. RPS（Receive Packet Steering）转发的包
 *   2. loopback 设备的包
 *   3. 没有硬件 NAPI 的老式网卡
 *
 * 它从 per-CPU 的 input_pkt_queue 取包，送入协议栈。
 */
static int process_backlog(struct napi_struct *napi, int quota)
{
    struct softnet_data *sd = container_of(napi, struct softnet_data, backlog);
    bool again = true;
    int work = 0;

    /* 检查是否有等待处理的 RPS IPI */
    if (sd_has_rps_ipi_waiting(sd)) {
        local_irq_disable();
        net_rps_action_and_irq_enable(sd);
    }

    napi->weight = dev_rx_weight;

    while (again) {
        struct sk_buff *skb;

        /*
         * ★ 从 process_queue 取包，送入协议栈
         *
         * process_queue 是从 input_pkt_queue 拼接过来的，
         * 避免在取包过程中需要持有锁。
         */
        while ((skb = __skb_dequeue(&sd->process_queue))) {
            rcu_read_lock();
            __netif_receive_skb(skb);   /* ★ 进入协议栈 */
            rcu_read_unlock();
            input_queue_head_incr(sd);
            if (++work >= quota)
                return work;
        }

        /*
         * process_queue 空了，检查 input_pkt_queue
         *
         * 需要关中断 + rps_lock 保护 input_pkt_queue
         * （因为硬中断/RPS IPI 可能并发写入 input_pkt_queue）
         */
        local_irq_disable();
        rps_lock(sd);
        if (skb_queue_empty(&sd->input_pkt_queue)) {
            /*
             * 两个队列都空了 → 标记 NAPI 完成
             *
             * 这里用 plain write 代替 clear_bit()，
             * 因为 backlog NAPI 只被当前 CPU 操作。
             */
            napi->state = 0;
            again = false;
        } else {
            /*
             * input_pkt_queue 有数据 → 拼接到 process_queue
             * 然后继续处理
             */
            skb_queue_splice_tail_init(&sd->input_pkt_queue,
                           &sd->process_queue);
        }
        rps_unlock(sd);
        local_irq_enable();
    }

    return work;
}


/* ======================================================================
 * ★ 核心函数 #6: __netif_receive_skb — 协议栈处理入口
 * ======================================================================
 *
 * 这是每个收包进入协议栈的入口点。
 * 在 NAPI poll 完成后（或通过 GRO 聚合后）被调用。
 *
 * 主要职责：
 *   1. 处理 PF_MEMALLOC 标记的包（内存回收上下文）
 *   2. 将包交给 __netif_receive_skb_one_core 处理
 *
 * __netif_receive_skb_one_core 内部依次经过：
 *   → XDP 程序（如果配置了 generic XDP）
 *   → tc ingress（流量控制入方向）
 *   → ptype_base 协议分发
 *     → ip_rcv()     (IPv4)
 *     → ipv6_rcv()   (IPv6)
 *     → arp_rcv()    (ARP)
 */
static int __netif_receive_skb(struct sk_buff *skb)
{
    int ret;

    if (sk_memalloc_socks() && skb_pfmemalloc(skb)) {
        unsigned int noreclaim_flag;

        /*
         * PF_MEMALLOC 包的特殊处理：
         * - 只能投递给 SOCK_MEMALLOC socket
         * - 不能送到用户态
         * - 内存使用受限
         *
         * 设置 PF_MEMALLOC 标志，避免在协议栈处理中触发内存回收
         */
        noreclaim_flag = memalloc_noreclaim_save();
        ret = __netif_receive_skb_one_core(skb, true);
        memalloc_noreclaim_restore(noreclaim_flag);
    } else
        ret = __netif_receive_skb_one_core(skb, false);

    return ret;
}


/*
 * ★ 总结：网络收包的完整数据流
 *
 *  ┌────────────────────────────────────────────────────────────┐
 *  │                    硬中断（IRQ）                              │
 *  │  driver_irq_handler()                                        │
 *  │    → napi_schedule_prep(napi) — CAS 设置 SCHED              │
 *  │    → __napi_schedule_irqoff(napi)                            │
 *  │        → ____napi_schedule(sd, napi)                         │
 *  │            → list_add_tail(napi, sd->poll_list)              │
 *  │            → __raise_softirq_irqoff(NET_RX_SOFTIRQ)          │
 *  │    → 屏蔽网卡中断                                            │
 *  ├────────────────────────────────────────────────────────────┤
 *  │                    软中断（NET_RX_SOFTIRQ）                   │
 *  │  net_rx_action()                                             │
 *  │    │                                                         │
 *  │    ├─ napi_poll(napi, budget)                                │
 *  │    │   └─ napi->poll(napi, weight)                           │
 *  │    │       = driver_poll() — 从 ring buffer 取包              │
 *  │    │           → dma_sync_single_for_cpu()                    │
 *  │    │           → napi_gro_receive()                          │
 *  │    │               → GRO 聚合                                │
 *  │    │               → __netif_receive_skb(skb)                │
 *  │    │                   → ip_rcv() → tcp_v4_rcv()              │
 *  │    │                                                         │
 *  │    │   或 = process_backlog() — 处理 RPS/loopback 包          │
 *  │    │       └─ __skb_dequeue(process_queue)                    │
 *  │    │           → __netif_receive_skb(skb)                     │
 *  │    │                                                         │
 *  │    ├─ budget 耗尽？ → break, 重新触发软中断                   │
 *  │    └─ 时间超限？   → break, 重新触发软中断                   │
 *  │                                                              │
 *  │  剩余 NAPI 放回 poll_list → __raise_softirq_irqoff           │
 *  └────────────────────────────────────────────────────────────┘
 *
 *  ★ 软中断不均衡的根因分析：
 *
 *  | 原因                  | 说明                                  |
 *  |----------------------|---------------------------------------|
 *  | RSS hash 不均匀       | 大 flow 集中在单个 CPU                 |
 *  | 软中断 CPU 绑定       | NET_RX_SOFTIRQ 只能在触发它的 CPU 上执行 |
 *  | 单队列网卡            | 所有包走同一个 IRQ → 同一个 CPU         |
 *  | ksoftirqd 优先级低    | 被其他任务抢占导致处理延迟               |
 *
 *  ★ 解决方案：
 *    1. RPS（Receive Packet Steering）：软件层面将包分发到其他 CPU
 *    2. 调整 RSS hash 策略：使用更均匀的 hash 函数
 *    3. 增加网卡队列数：匹配 CPU 数量
 *    4. 调整 IRQ affinity：将网卡中断绑定到多个 CPU
 *    5. 调整 NAPI weight / budget：增大每次 poll 的预算
 */
