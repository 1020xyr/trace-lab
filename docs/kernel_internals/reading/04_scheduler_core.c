/*
 * ======================================================================
 * Step 4: CFS 调度器核心 — 从 __schedule() 到上下文切换
 * ======================================================================
 *
 * 原始路径：
 *   - src/linux-5.10/kernel/sched/core.c (7000+ 行)
 *   - src/linux-5.10/kernel/sched/fair.c (11000+ 行)
 *
 * 本文件关注 CFS 调度器的核心路径：
 *   1. __schedule() — 调度主函数
 *   2. pick_next_task() — 选择下一个任务
 *   3. context_switch() — 上下文切换
 *   4. update_curr() — 更新 vruntime
 *   5. 红黑树调度示意
 *
 * 预计阅读时间：25 分钟
 * ======================================================================
 *
 * ★ CFS（Completely Fair Scheduler）核心思想
 *
 *   目标：让每个 runnable 任务获得"完全公平"的 CPU 时间。
 *
 *   实现手段：
 *     1. 用 vruntime（虚拟运行时间）衡量每个任务的"已消耗时间"
 *     2. vruntime 最小的任务优先运行
 *     3. 用红黑树维护所有 runnable 任务，按 vruntime 排序
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │  CFS 红黑树示意                                               │
 *   │                                                              │
 *   │              [vruntime=50]                                    │
 *   │             /            \                                    │
 *   │      [vr=30]            [vr=80]                               │
 *   │       /    \            /     \                               │
 *   │   [vr=20] [vr=40]  [vr=70] [vr=100]                          │
 *   │     /                                                         │
 *   │  [vr=10] ← ★ 最左节点，vruntime 最小                         │
 *   │                                                              │
 *   │  ★ 调度器总是选择最左节点（__pick_first_entity）              │
 *   │  ★ 新唤醒的任务插入红黑树后，如果其 vruntime 更小，            │
 *   │    触发抢占（设置 TIF_NEED_RESCHED）                          │
 *   │                                                              │
 *   │  vruntime 的计算：                                            │
 *   │    vruntime += delta_exec * NICE_0_LOAD / task->load.weight   │
 *   │                                                              │
 *   │  → nice 值越低（优先级越高）的进程，vruntime 增长越慢         │
 *   │  → 因此获得更多的 CPU 时间                                    │
 *   └──────────────────────────────────────────────────────────────┘
 *
 *  ★ 调度时机：
 *    1. 主动让出 CPU：mutex_lock、sleep、wait_event 等
 *    2. 抢占：timer interrupt → scheduler_tick() → TIF_NEED_RESCHED
 *    3. 唤醒：wake_up_process() 设置了 TIF_NEED_RESCHED
 *    4. 系统调用/异常返回用户态时检查 TIF_NEED_RESCHED
 */


/* ======================================================================
 * ★ 核心函数 #1: __schedule — 调度主函数
 * ======================================================================
 *
 * 这是 Linux 调度器的核心入口。每次需要选择下一个任务运行时，
 * 都会调用此函数（直接或间接通过 schedule()）。
 *
 * 入口方式：
 *   1. 显式阻塞：mutex、semaphore、waitqueue 等
 *   2. TIF_NEED_RESCHED 标志检查（中断返回、系统调用返回等路径）
 *   3. 抢占（CONFIG_PREEMPTION=y 时，preempt_enable() 中检查）
 */
static void __sched notrace __schedule(bool preempt)
{
    struct task_struct *prev, *next;
    unsigned long *switch_count;
    unsigned long prev_state;
    struct rq_flags rf;
    struct rq *rq;
    int cpu;

    /* 获取当前 CPU 的 runqueue */
    cpu = smp_processor_id();
    rq = cpu_rq(cpu);
    prev = rq->curr;     /* ★ prev 是当前正在运行的任务 */

    schedule_debug(prev, preempt);

    if (sched_feat(HRTICK))
        hrtick_clear(rq);

    /* ★ 关中断 — 调度过程中不允许被中断 */
    local_irq_disable();
    rcu_note_context_switch(preempt);

    /*
     * ★ 获取 runqueue 锁
     *
     * rq_lock 内部会执行 smp_mb__after_spinlock()，
     * 确保之前对 prev->state 的写入对 wake_up 可见。
     *
     * ★ rq->lock 是 per-CPU 的，不同 CPU 之间不竞争
     */
    rq_lock(rq, &rf);
    smp_mb__after_spinlock();

    /* 更新 rq 时钟 */
    rq->clock_update_flags <<= 1;
    update_rq_clock(rq);

    switch_count = &prev->nivcsw;   /* 默认为非自愿切换计数 */

    /*
     * ★ 处理 prev 的状态
     *
     * prev_state 是 prev 在调用 schedule() 前设置的状态：
     *   - TASK_RUNNING (0)：自愿让出 CPU（如抢占）
     *   - TASK_INTERRUPTIBLE：等待信号/事件
     *   - TASK_UNINTERRUPTIBLE：等待 I/O 等
     */
    prev_state = prev->state;

    if (!preempt && prev_state) {
        /* 检查是否有待处理信号（TASK_INTERRUPTIBLE 状态） */
        if (signal_pending_state(prev_state, prev)) {
            prev->state = TASK_RUNNING;
        } else {
            /*
             * ★ 将 prev 从 runqueue 中移除
             *
             * prev 要进入睡眠状态，不再参与调度。
             * 等待被 wake_up_process() 重新加入 runqueue。
             */
            prev->sched_contributes_to_load =
                (prev_state & TASK_UNINTERRUPTIBLE) &&
                !(prev_state & TASK_NOLOAD) &&
                !(prev->flags & PF_FROZEN);

            if (prev->sched_contributes_to_load)
                rq->nr_uninterruptible++;

            deactivate_task(rq, prev, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);

            /* I/O 等待计数 */
            if (prev->in_iowait) {
                atomic_inc(&rq->nr_iowait);
                delayacct_blkio_start();
            }
        }
        switch_count = &prev->nvcsw;   /* 自愿切换计数 */
    }

    /*
     * ★ 选择下一个任务
     *
     * pick_next_task 遍历所有调度类（stop/dl/rt/fair/idle），
     * 选择优先级最高的可运行任务。
     */
    next = pick_next_task(rq, prev, &rf);

    /* 清除需要重新调度的标志 */
    clear_tsk_need_resched(prev);
    clear_preempt_need_resched();

    if (likely(prev != next)) {
        /* ★ 需要切换任务 */
        rq->nr_switches++;
        RCU_INIT_POINTER(rq->curr, next);
        ++*switch_count;

        psi_sched_switch(prev, next, !task_on_rq_queued(prev));
        trace_sched_switch(preempt, prev, next);

        /*
         * ★ 执行上下文切换
         *
         * context_switch 内部调用 switch_to()，
         * 切换到 next 的栈和寄存器状态。
         *
         * ★ 注意：此函数在 prev 的上下文中调用，
         *    但返回时已经在 next 的上下文中了。
         *    prev 被"冻结"在此处，直到被重新调度。
         */
        rq = context_switch(rq, prev, next, &rf);
    } else {
        /* prev == next：没有更好的任务，继续运行 prev */
        rq->clock_update_flags &= ~(RQCF_ACT_SKIP|RQCF_REQ_SKIP);
        rq_unlock_irq(rq, &rf);
    }

    /* 执行负载均衡回调 */
    balance_callback(rq);
}


/* ======================================================================
 * ★ 核心函数 #2: pick_next_task — 选择下一个任务
 * ======================================================================
 *
 * 遍历所有调度类，选择优先级最高的可运行任务。
 *
 * Linux 的调度类（优先级从高到低）：
 *   1. stop_sched_class — stop_machine 用
 *   2. dl_sched_class   — SCHED_DEADLINE（EDF 实时调度）
 *   3. rt_sched_class   — SCHED_FIFO / SCHED_RR（实时调度）
 *   4. fair_sched_class — SCHED_NORMAL / SCHED_BATCH（CFS）
 *   5. idle_sched_class — 空闲任务
 *
 * ★ 快速路径优化：
 *   如果当前 CPU 上只有 CFS 任务（最常见情况），
 *   直接调用 pick_next_task_fair()，避免遍历所有调度类。
 */
static inline struct task_struct *
pick_next_task(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    const struct sched_class *class;
    struct task_struct *p;

    /*
     * ★ 快速路径：所有任务都是 CFS 类
     *
     * 这是最常见的情况（大部分进程都是 SCHED_NORMAL）。
     * 直接调用 CFS 的选择函数，避免遍历。
     */
    if (likely(prev->sched_class <= &fair_sched_class &&
           rq->nr_running == rq->cfs.h_nr_running)) {

        p = pick_next_task_fair(rq, prev, rf);
        if (unlikely(p == RETRY_TASK))
            goto restart;

        /*
         * CFS 没有可选任务 → 选择 idle 任务
         * （idle 调度类的 pick_next_task 永远返回 idle task）
         */
        if (!p) {
            put_prev_task(rq, prev);
            p = pick_next_task_idle(rq);
        }

        return p;
    }

restart:
    /*
     * ★ 慢速路径：遍历所有调度类
     *
     * put_prev_task_balance：处理 prev 任务的归还和负载均衡
     */
    put_prev_task_balance(rq, prev, rf);

    /* ★ 按优先级从高到低遍历每个调度类 */
    for_each_class(class) {
        p = class->pick_next_task(rq);
        if (p)
            return p;
    }

    /* idle 类应该总能返回一个任务 */
    BUG();
}


/* ======================================================================
 * ★ 核心函数 #3: context_switch — 上下文切换
 * ======================================================================
 *
 * 从 prev 切换到 next 的完整过程：
 *   1. 切换地址空间（MM）
 *   2. 切换寄存器状态和栈（switch_to）
 *   3. 善后处理（finish_task_switch）
 *
 * ★ 上下文切换的开销来源：
 *   - switch_mm：TLB 刷新（最大开销）
 *     - 同进程的内核线程切换不需要 switch_mm（lazy TLB）
 *     - PCID/ASID 可以减少 TLB 刷新开销
 *   - switch_to：保存/恢复寄存器 + 栈切换
 *     - x86 上需要保存/恢复 ~15 个寄存器
 *     - 包括 FPU 状态（如果使用了浮点运算）
 *   - cache pollution：切换后 CPU cache 命中率下降
 *     - prev 的 working set 被 next 的数据替换
 *     - 这是最"隐性"但影响最大的开销
 */
static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
           struct task_struct *next, struct rq_flags *rf)
{
    prepare_task_switch(rq, prev, next);

    /* 半虚拟化钩子 */
    arch_start_context_switch(prev);

    /*
     * ★ 地址空间切换
     *
     * 四种情况的处理：
     *   kernel → kernel: lazy TLB（不切换 MM，共享 active_mm）
     *   user → kernel:   lazy TLB + mmgrab（增加引用计数）
     *   kernel → user:   switch_mm + mmdrop（切换 MM + 减少引用计数）
     *   user → user:     switch_mm（直接切换）
     */
    if (!next->mm) {                                // to kernel
        /* next 是内核线程 — 使用 lazy TLB */
        enter_lazy_tlb(prev->active_mm, next);

        next->active_mm = prev->active_mm;
        if (prev->mm)                           // from user
            mmgrab(prev->active_mm);
        else
            prev->active_mm = NULL;
    } else {                                        // to user
        membarrier_switch_mm(rq, prev->active_mm, next->mm);

        /*
         * ★ switch_mm_irqs_off — 切换页表
         *
         * 这是上下文切换中最昂贵的操作之一。
         * 涉及：
         *   - 加载新的 CR3（页表基地址）
         *   - TLB 刷新（或 PCID 切换）
         *   - 更新 per-CPU 的 mm 相关状态
         */
        switch_mm_irqs_off(prev->active_mm, next->mm, next);

        if (!prev->mm) {                        // from kernel
            rq->prev_mm = prev->active_mm;
            prev->active_mm = NULL;
        }
    }

    rq->clock_update_flags &= ~(RQCF_ACT_SKIP|RQCF_REQ_SKIP);

    prepare_lock_switch(rq, next, rf);

    /*
     * ★ switch_to — 切换寄存器和栈
     *
     * 这是架构相关的汇编代码。在 x86 上大致做以下事情：
     *   1. 保存 prev 的 callee-saved 寄存器到 prev 的栈
     *   2. 切换栈指针（RSP）到 next 的栈
     *   3. 恢复 next 的 callee-saved 寄存器
     *   4. 跳转到 next 的返回地址
     *
     * ★ 宏展开后大致是：
     *   __switch_to(prev, next, prev);
     *   // 此时已经在 next 的上下文中运行了
     *
     * ★ 关键：switch_to 之后的代码是在 next 的上下文中执行的！
     *   prev 被"冻结"在此处，直到被重新调度后才会继续执行。
     */
    switch_to(prev, next, prev);
    barrier();

    /* ★ 善后处理（在 next 的上下文中执行） */
    return finish_task_switch(prev);
}


/* ======================================================================
 * ★ 核心函数 #4: finish_task_switch — 切换后的善后
 * ======================================================================
 *
 * 在 switch_to 之后执行，此时已经在 next 的上下文中。
 * 负责清理 prev 留下的状态。
 */
static struct rq *finish_task_switch(struct task_struct *prev)
    __releases(rq->lock)
{
    struct rq *rq = this_rq();
    struct mm_struct *mm = rq->prev_mm;
    long prev_state;

    /*
     * prev 留给我们的 preempt_count 是 2：
     *   schedule()
     *     preempt_disable();          // +1
     *     __schedule()
     *       raw_spin_lock_irq();      // +1
     */
    if (WARN_ONCE(preempt_count() != 2*PREEMPT_DISABLE_OFFSET,
              "corrupted preempt_count: %s/%d/0x%x\n",
              current->comm, current->pid, preempt_count()))
        preempt_count_set(FORK_PREEMPT_COUNT);

    rq->prev_mm = NULL;

    /*
     * ★ 观察 prev 的状态
     *
     * 必须在 finish_task() 之前读取 prev->state，
     * 因为 finish_task 设置 on_cpu=false 后，
     * prev 可能在另一个 CPU 上被唤醒并运行。
     */
    prev_state = prev->state;

    vtime_task_switch(prev);
    perf_event_task_sched_in(prev, current);
    finish_task(prev);            /* 设置 prev->on_cpu = false */
    finish_lock_switch(rq);       /* ★ 释放 rq->lock */
    finish_arch_post_lock_switch();
    kcov_finish_switch(current);

    fire_sched_in_preempt_notifiers(current);

    /* membarrier 同步 */
    if (mm) {
        membarrier_mm_sync_core_before_usermode(mm);
        mmdrop(mm);    /* 释放 lazy TLB 持有的 mm 引用 */
    }

    /* prev 是否已经退出（TASK_DEAD） */
    if (unlikely(prev_state == TASK_DEAD)) {
        if (prev->sched_class->task_dead)
            prev->sched_class->task_dead(prev);

        kprobe_flush_task(prev);
        put_task_stack(prev);
        put_task_struct_rcu_user(prev);
    }

    tick_nohz_task_switch();
    return rq;
}


/* ======================================================================
 * ★ 核心函数 #5: update_curr — 更新当前任务的 vruntime
 * ======================================================================
 *
 * 这是 CFS 公平性的核心——定期更新当前运行任务的 vruntime。
 *
 * vruntime 的含义：
 *   "如果这个任务以 SCHED_NORMAL 的 nice=0 权重运行，
 *    它应该消耗了多少时间"
 *
 * 计算公式：
 *   vruntime += delta_exec * (NICE_0_LOAD / task_load_weight)
 *
 *   - nice=0（默认权重）：vruntime = 实际时间
 *   - nice=-20（最高权重）：vruntime 增长很慢 → 获得更多 CPU
 *   - nice=+19（最低权重）：vruntime 增长很快 → 获得较少 CPU
 *
 * ★ 在以下时机被调用：
 *   - 调度器 tick（scheduler_tick → task_tick_fair）
 *   - 任务被抢占时
 *   - 任务主动让出 CPU 时
 *   - 新任务加入 runqueue 时
 */
static void update_curr(struct cfs_rq *cfs_rq)
{
    struct sched_entity *curr = cfs_rq->curr;
    u64 now = rq_clock_task(rq_of(cfs_rq));
    u64 delta_exec;

    if (unlikely(!curr))
        return;

    /* ★ 计算自上次更新以来经过的实际时间 */
    delta_exec = now - curr->exec_start;
    if (unlikely((s64)delta_exec <= 0))
        return;

    curr->exec_start = now;   /* 更新起始时间 */

    /* 记录最大单次执行时间（用于调度延迟分析） */
    schedstat_set(curr->statistics.exec_max,
              max(delta_exec, curr->statistics.exec_max));

    /* ★ 累计实际运行时间 */
    curr->sum_exec_runtime += delta_exec;
    schedstat_add(cfs_rq->exec_clock, delta_exec);

    /*
     * ★ 核心：更新 vruntime
     *
     * calc_delta_fair() 将实际时间转换为公平时间：
     *   vruntime += delta_exec * NICE_0_LOAD / se->load.weight
     *
     * 这使得不同 nice 值的任务可以公平比较。
     */
    curr->vruntime += calc_delta_fair(delta_exec, curr);

    /* ★ 更新红黑树中的最小 vruntime */
    update_min_vruntime(cfs_rq);

    /* 更新任务和 cgroup 的统计信息 */
    if (entity_is_task(curr)) {
        struct task_struct *curtask = task_of(curr);

        trace_sched_stat_runtime(curtask, delta_exec, curr->vruntime);
        cgroup_account_cputime(curtask, delta_exec);
        account_group_exec_runtime(curtask, delta_exec);
    }

    /* CFS bandwidth：检查是否超出 cgroup 的 CPU 配额 */
    account_cfs_rq_runtime(cfs_rq, delta_exec);
}


/* ======================================================================
 *
 * ★ 总结：CFS 调度的完整流程
 *
 *  [触发调度]
 *    │
 *    ├─ 主动让出: mutex_lock / sleep / wait_event
 *    │   → schedule() → __schedule(false)
 *    │
 *    ├─ 抢占: timer interrupt → scheduler_tick()
 *    │   → set_tsk_need_resched()
 *    │   → 返回用户态/内核态时检查 → __schedule(true)
 *    │
 *    └─ 唤醒: wake_up_process() → set_tsk_need_resched()
 *        → 返回用户态/内核态时检查 → __schedule(true)
 *
 *  [__schedule()]
 *    │
 *    ├─ 1. 获取 rq->lock
 *    ├─ 2. update_rq_clock() — 更新 rq 时钟
 *    ├─ 3. 处理 prev 状态（deactivate 睡眠任务）
 *    ├─ 4. pick_next_task() — 选择下一个任务
 *    │       │
 *    │       ├─ [快速路径] pick_next_task_fair()
 *    │       │   → 从 CFS 红黑树中取最左节点
 *    │       │   → update_curr() 更新 vruntime
 *    │       │
 *    │       └─ [慢速路径] for_each_class → class->pick_next_task()
 *    │           → 按优先级遍历：stop → dl → rt → fair → idle
 *    │
 *    ├─ 5. context_switch()
 *    │       ├─ switch_mm() — 切换页表（最昂贵）
 *    │       ├─ switch_to() — 切换寄存器和栈
 *    │       └─ finish_task_switch() — 善后处理
 *    │
 *    └─ 6. balance_callback() — 负载均衡
 *
 *  ★ 上下文切换开销分析：
 *
 *  | 开销来源       | 典型延迟       | 说明                     |
 *  |---------------|---------------|--------------------------|
 *  | switch_mm     | 1-10 μs       | TLB 刷新是主要开销         |
 *  | switch_to     | 0.5-2 μs      | 寄存器保存/恢复            |
 *  | cache 污染     | 10-100 μs     | 切换后 cache 未命中的代价  |
 *  | rq->lock      | 可变           | 高并发调度时的竞争         |
 *
 *  ★ 性能优化建议：
 *    - 减少不必要的上下文切换：使用 epoll 而非阻塞 I/O
 *    - 避免频繁创建/销毁线程：使用线程池
 *    - 合理设置 nice 值：CPU 密集型任务适当降低优先级
 *    - 绑核（taskset/cgroups）：减少跨 CPU 迁移的 cache 污染
 *    - 使用 SCHED_BATCH：减少交互式调度的开销
 *
 *  ★ CFS 红黑树操作总结：
 *
 *  | 操作                 | 函数                    | 时间复杂度 |
 *  |---------------------|-------------------------|-----------|
 *  | 选择最小 vruntime    | __pick_first_entity()   | O(1)      |
 *  | 插入新任务           | enqueue_entity()        | O(log N)  |
 *  | 删除任务             | dequeue_entity()        | O(log N)  |
 *  | 更新 vruntime        | update_curr()           | O(1)      |
 *  | 唤醒时检查抢占       | check_preempt_wakeup()  | O(1)      |
 *
 *  ★ N（红黑树节点数）= 当前 CPU 上 runnable 的任务数
 *    对于典型的工作负载，N 很小（1-10），所以 O(log N) 接近 O(1)
 */
