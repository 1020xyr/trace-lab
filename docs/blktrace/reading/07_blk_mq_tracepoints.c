/*
 * ======================================================================
 * Step 7: block/blk-mq.c — tracepoint 在 I/O 路径中的调用位置
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/block/blk-mq.c (3925 行)
 *
 * blk-mq（Multi-Queue Block I/O）是现代 Linux 内核的块设备 I/O 框架。
 * 本文件只关注 tracepoint 被调用的位置，理解 I/O 生命周期中每个事件
 * 在内核代码中的精确位置。
 *
 * 预计阅读时间：10 分钟
 * ======================================================================
 */


/*
 * ★ blk-mq I/O 路径概览
 *
 * 用户进程 write(fd, buf, len)
 *   │
 *   ▼
 * VFS → 文件系统（ext4/xfs/btrfs）
 *   │  生成 bio 结构
 *   ▼
 * submit_bio(bio)
 *   │
 *   ▼
 * ┌─── submit_bio_noacct() ──────────────────────────────────────┐
 * │                                                               │
 * │  trace_block_bio_queue(q, bio);          ← ★ Q 事件          │
 * │    │                                                         │
 * │    └→ blk_add_trace_bio() → __blk_add_trace(BLK_TA_QUEUE)   │
 * │                                                               │
 * └──────────────────────────────────────────────────────────────┘
 *   │
 *   ▼
 * blk_mq_submit_bio(bio)
 *   │
 *   ├── 分配 request 结构
 *   │     trace_block_getrq(q, bio, rw);     ← G 事件
 *   │
 *   ├── 如果有 I/O 调度器：
 *   │     blk_mq_sched_insert_request(rq)
 *   │       trace_block_rq_insert(q, rq);    ← ★ I 事件
 *   │
 *   ├── 合并检查：
 *   │     blk_mq_sched_try_merge()
 *   │       trace_block_bio_backmerge()      ← M 事件
 *   │
 *   └── 下发到设备驱动：
 *       blk_mq_dispatch_request()
 *         trace_block_rq_issue(q, rq);       ← ★ D 事件
 *
 *   │
 *   ▼
 * 设备驱动处理（NVMe/SCSI/virtio-blk/...）
 *   │
 *   ▼
 * 硬件完成 → 中断 → 软中断处理
 *   │
 *   ▼
 * blk_mq_complete_request(rq)
 *   │
 *   └→ trace_block_rq_complete(rq, error, nr_bytes); ← ★ C 事件
 *
 *
 * ★ Plug/Unplug 机制（P/U 事件）
 *
 * blk-mq 使用 plug 机制来延迟 request 的下发，以便合并更多 I/O：
 *
 * blk_start_plug(&plug);          ← 开始累积 request
 *   │
 *   ├── submit_bio(bio1)          ← Q 事件（但不立即下发）
 *   ├── submit_bio(bio2)          ← Q 事件（可能和 bio1 合并）
 *   └── submit_bio(bio3)          ← Q 事件
 *
 * blk_finish_plug(&plug);         ← 拔塞，一次性下发所有累积的 request
 *   │
 *   └── blk_mq_plug_issue()
 *       ├── trace_block_rq_issue(rq1)  ← D 事件
 *       ├── trace_block_rq_issue(rq2)  ← D 事件
 *       └── trace_block_rq_issue(rq3)  ← D 事件
 *
 * 对应的内核函数：
 *   blk_start_plug()    → trace_block_plug(q)       ← P 事件
 *   blk_finish_plug()   → trace_block_unplug(q, ...) ← U 事件
 *   blk_schedule_flush_plug() → trace_block_unplug() ← U 事件（调度器触发）
 */


/*
 * ★ blk-mq 中的关键函数与 tracepoint 对照
 *
 * 函数                              │ tracepoint              │ 事件
 * ──────────────────────────────────┼─────────────────────────┼──────
 * submit_bio_noacct()               │ block_bio_queue         │ Q
 * blk_mq_get_request()              │ block_getrq             │ G
 * blk_mq_get_request() (等待)       │ block_sleeprq           │ S
 * blk_mq_sched_insert_request()     │ block_rq_insert         │ I
 * blk_mq_sched_try_merge()          │ block_bio_backmerge     │ M
 *                                   │ block_bio_frontmerge    │ M
 * blk_mq_dispatch_request()         │ block_rq_issue          │ D
 * blk_mq_complete_request()         │ block_rq_complete       │ C
 * blk_mq_requeue_request()          │ block_rq_requeue        │ R
 * blk_start_plug()                  │ block_plug              │ P
 * blk_finish_plug()                 │ block_unplug            │ U
 * blk_account_io_start()            │ block_bio_remap         │ m
 *                                   │ block_rq_remap          │ m
 */


/*
 * ★ 深入理解：blk_mq_dispatch_request() 中的 D 事件
 *
 * 这是 I/O 从软件层进入设备驱动的分界点。
 * trace_block_rq_issue() 在这里被调用，产生 D 事件。
 *
 * 简化后的代码流程：
 *
 * static void blk_mq_dispatch_request(struct request_queue *q, ...)
 * {
 *     struct request *rq = ...;  // 从调度器队列中取出 request
 *
 *     // ★ 在调用设备驱动的 queue_rq 之前，触发 tracepoint
 *     trace_block_rq_issue(q, rq);
 *
 *     // 调用设备驱动的 queue_rq 回调
 *     ret = q->mq_ops->queue_rq(hctx, &bd);
 *
 *     // 如果设备返回 BLK_STS_RESOURCE（资源不足），需要 requeue
 *     if (ret == BLK_STS_RESOURCE) {
 *         trace_block_rq_requeue(q, rq);
 *         blk_mq_requeue_request(rq, ...);
 *     }
 * }
 *
 * 关键点：
 *   - D 事件发生在设备驱动处理之前
 *   - D2C 延迟 = 设备驱动处理时间 + 硬件执行时间 + 中断处理时间
 *   - 如果设备返回 RESOURCE，request 会被 requeue（R 事件）
 */


/*
 * ★ 深入理解：blk_mq_complete_request() 中的 C 事件
 *
 * 设备驱动完成 I/O 后调用此函数。
 * 通常在软中断（block softirq）或 workqueue 中执行。
 *
 * 简化后的代码流程：
 *
 * void blk_mq_complete_request(struct request *rq)
 * {
 *     // 调用设备驱动的 complete 回调（如果有）
 *     if (rq->q->mq_ops->complete)
 *         rq->q->mq_ops->complete(rq);
 *
 *     // ★ 触发 C 事件的 tracepoint
 *     trace_block_rq_complete(rq, error, nr_bytes);
 *
 *     // 完成 bio（通知上层：I/O 已完成）
 *     blk_mq_end_request(rq, error);
 * }
 *
 * 关键点：
 *   - C 事件在中断/软中断上下文中执行
 *   - current->pid 通常是 0（ksoftirqd 线程）
 *   - btt 不通过 PID 配对 Q 和 C，而是通过 device + sector
 */


/*
 * ★ Requeue 事件（R）详解
 *
 * 当设备驱动无法处理请求时（资源不足、临时错误等），
 * request 会被放回队列等待重试。
 *
 * 触发路径：
 *   blk_mq_dispatch_request()
 *     → q->mq_ops->queue_rq() 返回 BLK_STS_RESOURCE
 *     → blk_mq_requeue_request(rq)
 *     → trace_block_rq_requeue(q, rq)   ← R 事件
 *
 * R 事件对性能的影响：
 *   - 每次 requeue 都会增加 Q2C 延迟
 *   - 频繁的 requeue 说明设备资源紧张
 *   - 在 btt 输出中，如果 requeue 很多，需要检查设备状态
 *
 * 常见 requeue 原因：
 *   - NVMe: 控制器队列满
 *   - SCSI: 设备繁忙（QUEUE_FULL）
 *   - DM: 底层设备暂时不可用
 */


/*
 * ★ Plug/Unplug 机制（P/U 事件）详解
 *
 * Plug 机制的目的是延迟 request 的下发，给合并创造机会。
 *
 * 两种 unplug 方式：
 *   1. 显式 unplug（blk_finish_plug）
 *      进程主动调用，通常在文件系统批量提交 I/O 后
 *      trace_block_unplug(q, depth, explicit=true)
 *
 *   2. 隐式 unplug（blk_schedule_flush_plug）
 *      进程调度（schedule()）时自动触发
 *      防止 plug 持有的 request 永远不下发
 *      trace_block_unplug(q, depth, explicit=false)
 *
 * 对性能的影响：
 *   - P→U 之间的时间 = 请求在 plug 队列中等待的时间
 *   - 较长的 plug 时间有利于合并（提高吞吐），但增加延迟
 *   - blk-mq 中 plug 机制已被简化，很多场景不再使用
 */


/*
 * ★ 总结：完整的 I/O 事件生命周期与内核函数映射
 *
 * 时间线（从左到右）：
 *
 * Q ──────→ G ──→ I ──→ M ──→ P ──→ U ──→ D ──────→ C
 * │         │     │     │     │     │     │           │
 * │         │     │     │     │     │     │           └─ blk_mq_complete_request()
 * │         │     │     │     │     │     └─ blk_finish_plug()
 * │         │     │     │     │     └─ blk_schedule_flush_plug()
 * │         │     │     │     └─ blk_start_plug()
 * │         │     │     └─ blk_mq_sched_try_merge()
 * │         │     └─ blk_mq_sched_insert_request()
 * │         └─ blk_mq_get_request()
 * └─ submit_bio_noacct()
 *
 * 并非所有事件都会出现：
 *   - 无调度器（none scheduler）：没有 I 事件
 *   - 无法合并：没有 M 事件
 *   - 无 plug 机制：没有 P/U 事件
 *   - 设备正常：没有 R（requeue）事件
 *
 * 最常见的序列：Q → D → C
 */
