/*
 * ======================================================================
 * Step 2: block tracepoint 定义 — 内核中 Q/D/C 的 hook 点
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/include/trace/events/block.h (656 行)
 *
 * 本文件定义了 Linux 内核 block 层的所有 tracepoint。
 * 当内核编译时开启 CONFIG_BLK_DEV_IO_TRACE=y，这些 tracepoint 会被激活。
 * blktrace 内核模块（kernel/trace/blktrace.c）会注册回调到这些 tracepoint 上。
 *
 * 阅读要点：
 *   1. TRACE_EVENT / DEFINE_EVENT 宏的含义
 *   2. 重点 tracepoint：block_bio_queue(Q)、block_rq_insert(I)、
 *      block_rq_issue(D)、block_rq_complete(C)
 *   3. TP_fast_assign 中的字段就是 blktrace 最终记录的数据
 *   4. blk_fill_rwbs() 生成 rwbs 字段（如 "WS" = Write+Sync）
 *
 * 预计阅读时间：15 分钟
 * ======================================================================
 */

/*
 * ★ TRACE_EVENT 宏说明（理解这个宏才能读懂本文件）：
 *
 * TRACE_EVENT(name,
 *     TP_PROTO(参数类型),     // 函数签名（tracepoint 被调用时的参数）
 *     TP_ARGS(参数名),         // 参数名列表
 *     TP_STRUCT__entry(        // 定义要记录的字段
 *         __field(类型, 名称)
 *         __array(类型, 名称, 大小)
 *         __dynamic_array(类型, 名称, 大小)
 *     ),
 *     TP_fast_assign(          // 在 tracepoint 触发时执行（赋值）
 *         __entry->字段 = 值;
 *     ),
 *     TP_printk(格式, ...)     // 用于 ftrace 的文本输出格式
 * );
 *
 * DECLARE_EVENT_CLASS = 定义一个模板类
 * DEFINE_EVENT = 基于模板创建具体事件（共享格式，减少代码体积）
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM block

/*
 * rwbs 字段长度：8 个字符
 * 例如 "R" = 读, "WS" = 写+同步, "WF" = 写+FUA, "D" = discard
 *
 * blk_fill_rwbs() 根据 bio->bi_opf / rq->cmd_flags 填充：
 *   R = READ, W = WRITE, D = DISCARD, F = FLUSH/FUA,
 *   S = SYNC, M = META, A = READAHEAD
 */
#define RWBS_LEN    8


/* ======================================================================
 * ★ 核心 tracepoint #1: block_bio_queue → 对应 Q 事件
 * ======================================================================
 *
 * 触发时机：bio 即将被放入 block 层队列（submit_bio_noacct 中调用）
 * 这是 I/O 进入 block layer 的第一个事件。
 *
 * 对应 blktrace 中的 Q（Queue）事件。
 *
 * 调用链：
 *   submit_bio() → submit_bio_noacct() → trace_block_bio_queue()
 */
TRACE_EVENT(block_bio_queue,

	TP_PROTO(struct request_queue *q, struct bio *bio),

	TP_ARGS(q, bio),

	TP_STRUCT__entry(
		__field( dev_t,        dev        )   /* 设备号 */
		__field( sector_t,     sector     )   /* 起始扇区 */
		__field( unsigned int, nr_sector  )   /* 扇区数 */
		__array( char,         rwbs, RWBS_LEN) /* R/W/S/F 标志 */
		__array( char,         comm, TASK_COMM_LEN) /* 进程名 */
	),

	TP_fast_assign(
		/* ★ 从 bio 结构中提取关键字段 */
		__entry->dev       = bio_dev(bio);
		__entry->sector    = bio->bi_iter.bi_sector;
		__entry->nr_sector = bio_sectors(bio);
		/* 将 bio 的操作标志转换为可读的 rwbs 字符串 */
		blk_fill_rwbs(__entry->rwbs, bio->bi_opf, bio->bi_iter.bi_size);
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
	),

	/* ftrace 输出格式：major,minor rwbs sector + nr_sector [comm] */
	TP_printk("%d,%d %s %llu + %u [%s]",
		  MAJOR(__entry->dev), MINOR(__entry->dev), __entry->rwbs,
		  (unsigned long long)__entry->sector,
		  __entry->nr_sector, __entry->comm)
);


/* ======================================================================
 * ★ 核心 tracepoint #2: block_rq_insert → 对应 I 事件
 * ======================================================================
 *
 * 触发时机：request 被插入 I/O 调度器队列
 *
 * 这是 I/O 调度阶段的标记点。在 blk-mq 中，
 * blk_mq_sched_insert_request() 会调用此 tracepoint。
 *
 * 使用 DECLARE_EVENT_CLASS(block_rq) 定义的模板，
 * 模板记录了 request 的关键字段。
 */
DEFINE_EVENT(block_rq, block_rq_insert,

	TP_PROTO(struct request_queue *q, struct request *rq),

	TP_ARGS(q, rq)
);


/* ======================================================================
 * block_rq 事件类模板（insert/issue/merge 共用）
 * ======================================================================
 *
 * 这个模板定义了 request 级别 tracepoint 要记录的所有字段。
 * 理解这个模板就能理解 insert/issue/merge 三个 tracepoint 的数据内容。
 */
DECLARE_EVENT_CLASS(block_rq,

	TP_PROTO(struct request_queue *q, struct request *rq),

	TP_ARGS(q, rq),

	TP_STRUCT__entry(
		__field(  dev_t,       dev        )   /* 设备号 */
		__field(  sector_t,    sector     )   /* 起始扇区 */
		__field(  unsigned int,nr_sector  )   /* 扇区数 */
		__field(  unsigned int,bytes      )   /* 字节数 */
		__array(  char,        rwbs, RWBS_LEN) /* R/W/S/F 标志 */
		__array(  char,        comm, TASK_COMM_LEN) /* 进程名 */
		__dynamic_array( char, cmd, 1      )   /* SCSI 命令（PC 请求用） */
	),

	TP_fast_assign(
		/* ★ 从 request 结构中提取关键字段 */
		__entry->dev       = rq->rq_disk ? disk_devt(rq->rq_disk) : 0;
		__entry->sector    = blk_rq_trace_sector(rq);
		__entry->nr_sector = blk_rq_trace_nr_sectors(rq);
		__entry->bytes     = blk_rq_bytes(rq);

		blk_fill_rwbs(__entry->rwbs, rq->cmd_flags, blk_rq_bytes(rq));
		__get_str(cmd)[0] = '\0';
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
	),

	TP_printk("%d,%d %s %u (%s) %llu + %u [%s]",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->rwbs, __entry->bytes, __get_str(cmd),
		  (unsigned long long)__entry->sector,
		  __entry->nr_sector, __entry->comm)
);


/* ======================================================================
 * ★ 核心 tracepoint #3: block_rq_issue → 对应 D 事件
 * ======================================================================
 *
 * 触发时机：request 从 I/O 调度器下发到设备驱动
 *
 * 在 blk-mq 中，blk_mq_dispatch_request() 调用此 tracepoint。
 * 这标志着 I/O 离开软件队列，进入设备驱动/硬件处理阶段。
 *
 * D2C 延迟的起点就是这个时间点。
 */
DEFINE_EVENT(block_rq, block_rq_issue,

	TP_PROTO(struct request_queue *q, struct request *rq),

	TP_ARGS(q, rq)
);


/* ======================================================================
 * ★ 核心 tracepoint #4: block_rq_complete → 对应 C 事件
 * ======================================================================
 *
 * 触发时机：设备驱动完成了 I/O 请求（全部或部分）
 *
 * 在 blk-mq 中，blk_mq_complete_request() 调用此 tracepoint。
 * 这标志着 I/O 在硬件层面已完成。
 *
 * ★ 注意：这个 tracepoint 通常在软中断（softirq）上下文中执行，
 *   所以 current->pid 通常是 0（不是原始发起进程的 PID）。
 *   btt 通过 device + sector 来配对 Q 和 C 事件，而不是 PID。
 */
TRACE_EVENT(block_rq_complete,

	TP_PROTO(struct request *rq, int error, unsigned int nr_bytes),

	TP_ARGS(rq, error, nr_bytes),

	TP_STRUCT__entry(
		__field(  dev_t,       dev        )   /* 设备号 */
		__field(  sector_t,    sector     )   /* 起始扇区 */
		__field(  unsigned int,nr_sector  )   /* 完成的扇区数 */
		__field(  int,         error      )   /* ★ 错误码（0=成功） */
		__array(  char,        rwbs, RWBS_LEN)
		__dynamic_array( char, cmd, 1      )
	),

	TP_fast_assign(
		__entry->dev       = rq->rq_disk ? disk_devt(rq->rq_disk) : 0;
		__entry->sector    = blk_rq_pos(rq);
		__entry->nr_sector = nr_bytes >> 9;   /* 字节→扇区（÷512） */
		__entry->error     = error;

		blk_fill_rwbs(__entry->rwbs, rq->cmd_flags, nr_bytes);
		__get_str(cmd)[0] = '\0';
	),

	TP_printk("%d,%d %s (%s) %llu + %u [%d]",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->rwbs, __get_str(cmd),
		  (unsigned long long)__entry->sector,
		  __entry->nr_sector, __entry->error)
);


/* ======================================================================
 * 其他 tracepoint（快速浏览）
 * ======================================================================
 */

/* block_rq_requeue: request 被重新入队（设备拒绝了，需要重试） */
TRACE_EVENT(block_rq_requeue, ...);

/* block_rq_merge: request 在调度器中被合并 */
DEFINE_EVENT(block_rq, block_rq_merge, ...);

/* block_bio_backmerge / block_bio_frontmerge: bio 级别的合并 */
DEFINE_EVENT(block_bio_merge, block_bio_backmerge, ...);
DEFINE_EVENT(block_bio_merge, block_bio_frontmerge, ...);

/* block_bio_complete: bio 完成（比 rq_complete 更上层） */
TRACE_EVENT(block_bio_complete, ...);

/* block_getrq: 分配 request 结构 */
DEFINE_EVENT(block_get_rq, block_getrq, ...);

/* block_sleeprq: 等待 request 分配（队列满了） */
DEFINE_EVENT(block_get_rq, block_sleeprq, ...);

/* block_plug / block_unplug: 插队/拔塞机制 */
TRACE_EVENT(block_plug, ...);
DEFINE_EVENT(block_unplug, block_unplug, ...);

/* block_split: bio 被拆分 */
TRACE_EVENT(block_split, ...);

/* block_bio_remap / block_rq_remap: DM/LVM 重映射 */
TRACE_EVENT(block_bio_remap, ...);
TRACE_EVENT(block_rq_remap, ...);


/*
 * ★ 总结：blktrace 关心的核心 tracepoint 与事件映射
 *
 *   tracepoint              → blktrace 事件    → blkparse 字母
 *   ──────────────────────────────────────────────────────────
 *   block_bio_queue         → BLK_TA_QUEUE     → Q
 *   block_getrq             → BLK_TA_GETRQ     → G
 *   block_rq_insert         → BLK_TA_INSERT    → I
 *   block_rq_merge          → BLK_TA_BACKMERGE → M
 *   block_rq_issue          → BLK_TA_ISSUE     → D
 *   block_rq_complete       → BLK_TA_COMPLETE  → C
 *   block_rq_requeue        → BLK_TA_REQUEUE   → (requeue)
 *   block_plug              → BLK_TA_PLUG      → P
 *   block_unplug            → BLK_TA_UNPLUG_IO → U
 *
 *   I/O 生命周期：Q → G → I → M → D → C
 */

#endif /* _TRACE_BLOCK_H */
