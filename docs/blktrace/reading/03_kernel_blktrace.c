/*
 * ======================================================================
 * Step 3: 内核 blktrace.c — __blk_add_trace + relay 初始化 + 回调注册
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/kernel/trace/blktrace.c (2002 行)
 *
 * 本文件是 blktrace 的内核实现。我们只关注三个核心部分：
 *   1. __blk_add_trace() — 所有事件的记录入口（最核心的函数）
 *   2. do_blk_trace_setup() — relay buffer 的初始化
 *   3. blk_add_trace_rq_*() — tracepoint 回调函数（将 tracepoint 事件转为 blk_io_trace）
 *
 * 预计阅读时间：20 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ 核心函数 #1: __blk_add_trace — 所有事件的记录入口
 * ======================================================================
 *
 * 每当 block 层发生 tracepoint 事件时，最终都会调用这个函数。
 * 它的职责：
 *   1. 编码 action 标志（添加读写方向、SYNC/FUA 等属性）
 *   2. 在 relay buffer 的 per-CPU subbuffer 中预留空间
 *   3. 填充 blk_io_trace 结构
 *   4. 拷贝 PDU 附加数据（如果有）
 *
 * 参数说明：
 *   bt       — blk_trace 实例（包含 relay channel、设备号等）
 *   sector   — I/O 起始扇区
 *   bytes    — I/O 大小（字节）
 *   op       — 操作类型（READ/WRITE/DISCARD/FLUSH）
 *   op_flags — 操作标志（SYNC/FUA/META/RAHEAD）
 *   what     — 事件类型（BLK_TA_QUEUE / BLK_TA_ISSUE / BLK_TA_COMPLETE 等）
 *   error    — 错误码
 *   pdu_len  — 附加数据长度
 *   pdu_data — 附加数据指针
 *   cgid     — cgroup ID（0 = 无 cgroup）
 */
static void __blk_add_trace(struct blk_trace *bt, sector_t sector, int bytes,
		     int op, int op_flags, u32 what, int error, int pdu_len,
		     void *pdu_data, u64 cgid)
{
	struct task_struct *tsk = current;
	struct ring_buffer_event *event = NULL;
	struct trace_buffer *buffer = NULL;
	struct blk_io_trace *t;
	unsigned long flags = 0;
	unsigned long *sequence;
	pid_t pid;
	int cpu, pc = 0;
	bool blk_tracer = blk_tracer_enabled;
	ssize_t cgid_len = cgid ? sizeof(cgid) : 0;

	/* 检查追踪是否在运行 */
	if (unlikely(bt->trace_state != Blktrace_running && !blk_tracer))
		return;

	/*
	 * ★ 编码 action 标志
	 *
	 * what 参数此时只包含低 8 位的操作类型（如 BLK_TA_ISSUE）。
	 * 这里通过位运算添加高 16 位的类别标志：
	 *   - 读写方向（READ 或 WRITE）
	 *   - SYNC（同步 I/O）
	 *   - RAHEAD（预读）
	 *   - META（元数据）
	 *   - PREFLUSH（flush 前缀）
	 *   - ★ FUA（强制单元访问）← 这就是你之前忽略的标志！
	 *   - DISCARD
	 *   - FLUSH
	 *
	 * MASK_TC_BIT(op_flags, FUA) 的实现：
	 *   #define MASK_TC_BIT(rw, bit) (BIT(BLK_TC_##bit + BLK_TC_SHIFT) \
	 *       * !!(op_flags & REQ_##bit))
	 *   即：如果 op_flags 中有 REQ_FUA 标志，就在 action 的高位设置 BLK_TC_FUA
	 *
	 * ★ 重要：这就是为什么 blkparse 输出中 FUA 请求会显示 'F' 标记
	 */
	what |= ddir_act[op_is_write(op) ? WRITE : READ];
	what |= MASK_TC_BIT(op_flags, SYNC);
	what |= MASK_TC_BIT(op_flags, RAHEAD);
	what |= MASK_TC_BIT(op_flags, META);
	what |= MASK_TC_BIT(op_flags, PREFLUSH);
	what |= MASK_TC_BIT(op_flags, FUA);    /* ★ FUA 标志在这里被编码！ */
	if (op == REQ_OP_DISCARD || op == REQ_OP_SECURE_ERASE)
		what |= BLK_TC_ACT(BLK_TC_DISCARD);
	if (op == REQ_OP_FLUSH)
		what |= BLK_TC_ACT(BLK_TC_FLUSH);
	if (cgid)
		what |= __BLK_TA_CGROUP;

	/* 获取当前进程的 PID */
	pid = tsk->pid;

	/* 检查是否应该记录此事件（act_mask、LBA 范围、PID 过滤） */
	if (act_log_check(bt, what, sector, pid))
		return;

	cpu = raw_smp_processor_id();

	/*
	 * 两种输出路径：
	 * 1. ftrace 模式（blk_tracer = true）：写入 ftrace 的 ring_buffer
	 * 2. relay 模式（默认）：写入 relay 的 per-CPU subbuffer
	 *
	 * blktrace 命令行工具默认使用 relay 模式。
	 * "echo 1 > /sys/kernel/debug/tracing/events/block/enable" 使用 ftrace 模式。
	 */
	if (blk_tracer) {
		/* ftrace 模式：使用 ring_buffer */
		tracing_record_cmdline(current);
		buffer = blk_tr->array_buffer.buffer;
		pc = preempt_count();
		event = trace_buffer_lock_reserve(buffer, TRACE_BLK,
						  sizeof(*t) + pdu_len + cgid_len,
						  0, pc);
		if (!event)
			return;
		t = ring_buffer_event_data(event);
		goto record_it;
	}

	/* 检查进程名是否需要记录（PID→进程名映射） */
	if (unlikely(tsk->btrace_seq != blktrace_seq))
		trace_note_tsk(tsk);

	/*
	 * ★ relay 模式的核心：在 per-CPU subbuffer 中预留空间
	 *
	 * 为什么要关中断？
	 *   防止中断处理程序中的 I/O 操作也调用 __blk_add_trace()，
	 *   导致同一 CPU 上的 relay_reserve 被干扰（relay_reserve 不是完全无锁的）。
	 */
	local_irq_save(flags);

	/* 从 relay channel 中预留 sizeof(blk_io_trace) + pdu 的空间 */
	t = relay_reserve(bt->rchan, sizeof(*t) + pdu_len + cgid_len);
	if (t) {
		/* per-CPU 序列号（每个 CPU 独立递增） */
		sequence = per_cpu_ptr(bt->sequence, cpu);

		/* ★ 填充 blk_io_trace 结构 */
		t->magic = BLK_IO_TRACE_MAGIC | BLK_IO_TRACE_VERSION;
		t->sequence = ++(*sequence);
		t->time = ktime_to_ns(ktime_get());   /* ★ 纳秒时间戳 */

record_it:
		/* 这些字段在 ftrace 和 relay 两种路径中共用 */
		t->cpu = cpu;
		t->pid = pid;

		t->sector = sector;
		t->bytes = bytes;
		t->action = what;     /* ★ 编码后的完整 action（含 FUA 等标志） */
		t->device = bt->dev;
		t->error = error;
		t->pdu_len = pdu_len + cgid_len;

		/* 拷贝 cgroup ID 和 PDU 附加数据 */
		if (cgid_len)
			memcpy((void *)t + sizeof(*t), &cgid, cgid_len);
		if (pdu_len)
			memcpy((void *)t + sizeof(*t) + cgid_len, pdu_data, pdu_len);

		if (blk_tracer) {
			trace_buffer_unlock_commit(blk_tr, buffer, event, 0, pc);
			return;
		}
	}

	local_irq_restore(flags);
}


/* ======================================================================
 * ★ 核心函数 #2: do_blk_trace_setup — 初始化 relay buffer
 * ======================================================================
 *
 * 当用户态 blktrace 工具调用 ioctl(fd, BLKTRACESETUP, &buts) 时，
 * 内核最终调用此函数来初始化追踪环境。
 *
 * 主要工作：
 *   1. 分配 blk_trace 结构
 *   2. 创建 debugfs 目录
 *   3. ★ 打开 relay channel（per-CPU buffer）
 *   4. 设置过滤参数
 */
static int do_blk_trace_setup(struct request_queue *q, char *name, dev_t dev,
			      struct block_device *bdev,
			      struct blk_user_trace_setup *buts)
{
	struct blk_trace *bt = NULL;
	struct dentry *dir = NULL;
	int ret;

	/* 验证参数 */
	if (!buts->buf_size || !buts->buf_nr)
		return -EINVAL;

	/* 设备名处理：将 '/' 替换为 '_'（debugfs 路径不允许 '/'） */
	strncpy(buts->name, name, BLKTRACE_BDEV_SIZE);
	strreplace(buts->name, '/', '_');

	/* 检查是否已有追踪在运行（不允许并发追踪同一设备） */
	if (rcu_dereference_protected(q->blk_trace, ...)) {
		pr_warn("Concurrent blktraces are not allowed on %s\n", buts->name);
		return -EBUSY;
	}

	/* 分配 blk_trace 结构 */
	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	bt->sequence = alloc_percpu(unsigned long);   /* per-CPU 序列号 */
	bt->msg_data = __alloc_percpu(BLK_TN_MAX_MSG, __alignof__(char));

	/* 创建 debugfs 目录（/sys/kernel/debug/block/<device>/） */
	if (bdev && !bdev_is_partition(bdev))
		dir = q->debugfs_dir;   /* 整盘：复用已有的 debugfs 目录 */
	else
		bt->dir = dir = debugfs_create_dir(buts->name, blk_debugfs_root);

	bt->dev = dev;

	/* 创建 debugfs 文件 */
	bt->dropped_file = debugfs_create_file("dropped", 0444, dir, bt, ...);
	bt->msg_file = debugfs_create_file("msg", 0222, dir, bt, ...);

	/*
	 * ★ 核心：打开 relay channel
	 *
	 * relay_open("trace", dir, buf_size, buf_nr, callbacks, data)
	 *
	 * 这会创建：
	 *   /sys/kernel/debug/block/<dev>/trace0  (CPU 0 的 relay buffer)
	 *   /sys/kernel/debug/block/<dev>/trace1  (CPU 1 的 relay buffer)
	 *   ...
	 *
	 * 用户态 blktrace 工具通过 mmap 或 read 从这些文件中读取数据。
	 */
	bt->rchan = relay_open("trace", dir, buts->buf_size,
				buts->buf_nr, &blk_relay_callbacks, bt);

	/* 设置事件过滤器 */
	bt->act_mask = buts->act_mask;   /* 用户指定的事件类别掩码 */
	if (!bt->act_mask)
		bt->act_mask = (u16) -1; /* 0 = 追踪所有事件 */

	/* 设置 LBA 范围过滤 */
	blk_trace_setup_lba(bt, bdev);
	if (buts->start_lba) bt->start_lba = buts->start_lba;
	if (buts->end_lba)   bt->end_lba   = buts->end_lba;

	bt->pid = buts->pid;            /* PID 过滤（0 = 所有进程） */
	bt->trace_state = Blktrace_setup;

	/* 将 blk_trace 结构关联到 request queue */
	rcu_assign_pointer(q->blk_trace, bt);

	/* ... */
	return 0;
}


/* ======================================================================
 * ★ 核心函数 #3: tracepoint 回调 — 将 tracepoint 事件转为 blk_io_trace
 * ======================================================================
 *
 * 这些函数是注册到 block tracepoint 上的回调。
 * 当 tracepoint 被触发时，它们将 tracepoint 参数转换为 __blk_add_trace() 的调用。
 *
 * 关系图：
 *
 *   trace_block_rq_issue(q, rq)          ← tracepoint 被触发
 *       │
 *       ▼
 *   blk_add_trace_rq_issue(ignore, q, rq) ← 回调函数
 *       │
 *       ▼
 *   blk_add_trace_rq(rq, 0, bytes, BLK_TA_ISSUE, cgid)  ← 公共处理
 *       │
 *       ▼
 *   __blk_add_trace(bt, sector, nr_bytes, ..., BLK_TA_ISSUE, ...)  ← 记录事件
 */

/* 公共的 request 追踪函数 */
static void blk_add_trace_rq(struct request *rq, int error,
			     unsigned int nr_bytes, u32 what, u64 cgid)
{
	struct blk_trace *bt;

	rcu_read_lock();
	/* 从 request queue 中获取 blk_trace 实例 */
	bt = rcu_dereference(rq->q->blk_trace);
	if (likely(!bt)) {    /* 大部分时候 blk_trace 是 NULL（未开启追踪） */
		rcu_read_unlock();
		return;
	}

	/* 标记请求类型：FS（文件系统）或 PC（SCSI passthrough） */
	if (blk_rq_is_passthrough(rq))
		what |= BLK_TC_ACT(BLK_TC_PC);
	else
		what |= BLK_TC_ACT(BLK_TC_FS);

	/* 调用核心函数记录事件 */
	__blk_add_trace(bt, blk_rq_trace_sector(rq), nr_bytes, req_op(rq),
			rq->cmd_flags, what, error, 0, NULL, cgid);
	rcu_read_unlock();
}

/* ★ D 事件回调：request 被下发到设备驱动 */
static void blk_add_trace_rq_issue(void *ignore,
				   struct request_queue *q, struct request *rq)
{
	blk_add_trace_rq(rq, 0, blk_rq_bytes(rq), BLK_TA_ISSUE,
			 blk_trace_request_get_cgid(q, rq));
}

/* ★ C 事件回调：request 被设备驱动完成 */
static void blk_add_trace_rq_complete(void *ignore, struct request *rq,
			int error, unsigned int nr_bytes)
{
	blk_add_trace_rq(rq, error, nr_bytes, BLK_TA_COMPLETE,
			 blk_trace_request_get_cgid(rq->q, rq));
}

/* I 事件回调：request 被插入调度器 */
static void blk_add_trace_rq_insert(void *ignore,
				    struct request_queue *q, struct request *rq)
{
	blk_add_trace_rq(rq, 0, blk_rq_bytes(rq), BLK_TA_INSERT,
			 blk_trace_request_get_cgid(q, rq));
}

/* 其他回调：requeue、merge 等... */
static void blk_add_trace_rq_requeue(void *ignore, ...);
static void blk_add_trace_rq_merge(void *ignore, ...);


/*
 * ★ bio 级别的追踪（Q 事件）
 */
static void blk_add_trace_bio(struct request_queue *q, struct bio *bio,
			      u32 what, int error)
{
	struct blk_trace *bt;

	rcu_read_lock();
	bt = rcu_dereference(q->blk_trace);
	if (likely(!bt)) {
		rcu_read_unlock();
		return;
	}

	/* 从 bio 中提取信息传给 __blk_add_trace */
	__blk_add_trace(bt, bio->bi_iter.bi_sector, bio->bi_iter.bi_size,
			bio_op(bio), bio->bi_opf, what, error, 0, NULL,
			blk_trace_bio_get_cgid(q, bio));
	rcu_read_unlock();
}


/*
 * ★ 总结：内核 blktrace.c 的数据流
 *
 *  用户态:  ioctl(BLKTRACESETUP)  →  do_blk_trace_setup()
 *           ioctl(BLKTRACESTART)  →  blk_trace_startstop()
 *
 *  内核 I/O 路径:
 *    submit_bio()
 *       → trace_block_bio_queue()
 *         → blk_add_trace_bio()
 *           → __blk_add_trace(action=BLK_TA_QUEUE)  ← Q 事件
 *             → relay_reserve()  →  per-CPU subbuffer
 *
 *    blk_mq_dispatch_request()
 *       → trace_block_rq_issue()
 *         → blk_add_trace_rq_issue()
 *           → __blk_add_trace(action=BLK_TA_ISSUE)  ← D 事件
 *
 *    blk_mq_complete_request()
 *       → trace_block_rq_complete()
 *         → blk_add_trace_rq_complete()
 *           → __blk_add_trace(action=BLK_TA_COMPLETE)  ← C 事件
 *
 *  用户态:  mmap /sys/kernel/debug/block/<dev>/traceN
 *           → 读取 blk_io_trace 结构
 *           → 写入 sda.blktrace.N 文件
 */
