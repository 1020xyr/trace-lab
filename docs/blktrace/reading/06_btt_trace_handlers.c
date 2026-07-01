/*
 * ======================================================================
 * Step 6: btt — 延迟计算（Q2D / D2C / Q2C）
 * ======================================================================
 *
 * 原始路径：src/blktrace/btt/ 目录
 *
 * btt（Block Trace Timeline）通过分析 blktrace 事件流，
 * 将同一 I/O 的 Q/D/C 事件配对，计算各阶段延迟。
 *
 * 本文件包含：
 *   1. trace_queue.c — Q 事件处理
 *   2. trace_issue.c — D 事件处理 + Q2D 计算
 *   3. trace_complete.c — C 事件处理 + D2C/Q2C 计算
 *   4. latency.c — 延迟数据输出
 *   5. q2d.c — Q2D 延迟直方图
 *
 * 预计阅读时间：20 分钟
 * ======================================================================
 */


/* ======================================================================
 * 6.1 trace_queue.c — Q 事件处理
 * ======================================================================
 *
 * 当 btt 读取到一个 Q（Queue）事件时：
 *   1. 创建一个 io 结构来跟踪这个 I/O
 *   2. 记录 Q 时间戳
 *   3. 更新统计信息
 *
 * 这个 io 结构会一直保留，直到对应的 C（Complete）事件被处理。
 * 配对方式：通过 device + sector + bytes 来匹配同一 I/O 的不同事件。
 */

/* Q 事件的内部处理 */
static void handle_queue(struct io *q_iop)
{
	/* 更新 seek 分析（顺序/随机 I/O 检测） */
	seeki_add(q_iop->dip->q2q_handle, q_iop);

	/* 更新 Q 区域统计（用于时间线输出） */
	update_qregion(&all_regions, q_iop->t.time);

	/* 更新设备级别的 Q 统计 */
	dip_update_q(q_iop->dip, q_iop);

	/* 更新进程级别的 Q 统计 */
	pip_update_q(q_iop);

	/* 区分 DM 映射设备和普通设备 */
	if (remapper_dev(q_iop->t.device))
		update_lq(&last_q, &all_avgs.q2q_dm, q_iop->t.time);
	else {
		/* ★ 更新 Q 大小的直方图 */
		update_q_histo(q_iop->t.bytes);
		update_lq(&last_q, &all_avgs.q2q, q_iop->t.time);
	}

	/*
	 * ★ 初始化所有阶段的时间戳为 -1（未发生）
	 * 后续 D/C 事件会更新 d_time / c_time
	 */
	q_iop->i_time = q_iop->g_time = q_iop->c_time = q_iop->m_time =
						q_iop->d_time = (__u64)-1;

	/* 递增 Q 计数 */
	q_iop->dip->n_qs++;

	/* 更新活跃队列深度（用于计算平均队列深度） */
	q_iop->dip->t_act_q += q_iop->dip->n_act_q;
	q_iop->dip->n_act_q++;
}

/* Q 事件的入口函数 */
void trace_queue(struct io *q_iop)
{
	if (q_iop->t.bytes == 0)
		return;

	/* io_setup 将 io 结构加入追踪表，返回 true 表示成功 */
	if (io_setup(q_iop, IOP_Q))
		handle_queue(q_iop);
	else
		io_release(q_iop);
}


/* ======================================================================
 * 6.2 trace_issue.c — D 事件处理 + ★ Q2D 计算
 * ======================================================================
 *
 * 当 btt 读取到一个 D（Issue/Dispatch）事件时：
 *   1. 通过 device + sector 找到之前记录的 Q 事件
 *   2. ★ 计算 Q2D 延迟 = D 时间 - Q 时间
 *   3. 如果之前有 I（Insert）事件，也计算 I2D
 */

static void handle_issue(struct io *d_iop)
{
	LIST_HEAD(head);
	struct list_head *p, *q;

	/* 递减活跃队列计数（request 从队列中移走了） */
	if (d_iop->dip->n_act_q != 0)
		d_iop->dip->n_act_q--;

	/* 更新 seek 分析 */
	seeki_add(d_iop->dip->seek_handle, d_iop);
	/* 更新块号 dump（用于 I/O 热点图） */
	bno_dump_add(d_iop->dip->bno_dump_handle, d_iop);
	/* 更新 I/O 统计 */
	iostat_issue(d_iop);
	d_iop->dip->n_ds++;
	if (!remapper_dev(d_iop->t.device))
		update_d_histo(d_iop->t.bytes);
	/* 更新平均队列深度 */
	aqd_issue(d_iop->dip->aqd_handle, BIT_TIME(d_iop->t.time));

	/*
	 * ★ 核心：找到与此 D 事件对应的 Q 事件，计算 Q2D
	 *
	 * dip_foreach_list 通过 device + sector 在追踪表中查找
	 * 之前记录的 Q 事件（可能有多个，因为合并的 I/O 共享同一 D 事件）
	 */
	dip_foreach_list(d_iop, IOP_Q, &head);
	list_for_each_safe(p, q, &head) {
		struct io *q_iop = list_entry(p, struct io, f_head);

		/* ★ 计算中间延迟 */
		if (q_iop->i_time != (__u64)-1)
			/* I2D = D 时间 - I 时间（在调度器中等待的时间） */
			update_i2d(q_iop, tdelta(q_iop->i_time, d_iop->t.time));
		else if (q_iop->m_time != (__u64)-1)
			/* M2D = D 时间 - M 时间（合并后到下发的时间） */
			update_m2d(q_iop, tdelta(q_iop->m_time, d_iop->t.time));

		d_iop->bytes_left -= q_iop->t.bytes;
		list_del(&q_iop->f_head);

		/* 记录 D 事件的时间和扇区（供后续 C 事件使用） */
		q_iop->d_time = d_iop->t.time;
		q_iop->d_sec = d_iop->t.sector;
		q_iop->d_nsec = t_sec(&d_iop->t);

		/* ★ 计算并记录 Q2D 延迟 */
		if (output_all_data)
			q2d_histo_add(q_iop->dip->q2d_priv,
						d_iop->t.time - q_iop->t.time);
		/* latency_q2d 将延迟写入 q2d 输出文件和平台统计 */
		latency_q2d(q_iop->dip, d_iop->t.time,
						d_iop->t.time - q_iop->t.time);
	}
}

void trace_issue(struct io *d_iop)
{
	if (d_iop->t.bytes == 0)
		return;

	if (io_setup(d_iop, IOP_D))
		handle_issue(d_iop);

	io_release(d_iop);
}


/* ======================================================================
 * 6.3 trace_complete.c — C 事件处理 + ★ D2C / Q2C 计算
 * ======================================================================
 *
 * 当 btt 读取到一个 C（Complete）事件时：
 *   1. 通过 device + sector 找到之前记录的 Q 和 D 事件
 *   2. ★ 计算 Q2C 延迟 = C 时间 - Q 时间
 *   3. ★ 计算 D2C 延迟 = C 时间 - D 时间
 */

static void handle_complete(struct io *c_iop)
{
	LIST_HEAD(head);
	struct list_head *p, *q;
	__u64 d_time = (__u64)-1;
	FILE *pit_fp = c_iop->dip->pit_fp;
	double cur = BIT_TIME(c_iop->t.time);

	update_blks(c_iop);
	update_cregion(&all_regions, c_iop->t.time);
	update_cregion(&c_iop->dip->regions, c_iop->t.time);
	if (c_iop->pip)
		update_cregion(&c_iop->pip->regions, c_iop->t.time);
	aqd_complete(c_iop->dip->aqd_handle, cur);
	rstat_add(c_iop->dip->rstat_handle, cur, c_iop->t.bytes >> 9);

	/*
	 * ★ 核心：找到与此 C 事件对应的 Q 事件，计算 Q2C 和 D2C
	 */
	dip_foreach_list(c_iop, IOP_Q, &head);
	list_for_each_safe(p, q, &head) {
		struct io *q_iop = list_entry(p, struct io, f_head);

		/* ★ Q2C = C 时间 - Q 时间（端到端总延迟） */
		__u64 q2c = tdelta(q_iop->t.time, c_iop->t.time);

		c_iop->bytes_left -= q_iop->t.bytes;

		/* 更新 Q2C 统计 */
		update_q2c(q_iop, q2c);
		latency_q2c(q_iop->dip, q_iop->t.time, q2c);

		/* 如果有 D 时间（request 确实被下发了），计算 D2C */
		if (q_iop->d_time != (__u64)-1) {
			/* ★ D2C = C 时间 - D 时间（设备处理延迟） */
			__u64 d2c = tdelta(q_iop->d_time, c_iop->t.time);

			p_live_add(q_iop->dip, q_iop->d_time, c_iop->t.time);
			/* 更新 D2C 统计 */
			update_d2c(q_iop, d2c);
			latency_d2c(q_iop->dip, c_iop->t.time, d2c);
			iostat_complete(q_iop, c_iop);

			d_time = q_iop->d_time;
		}

		/* 如果启用了 per-IO 输出（-p 参数），显示完整的 I/O 时间线 */
		if (per_io_ofp) {
			q_iop->c_time = c_iop->t.time;
			q_iop->c_sec = c_iop->t.sector;
			q_iop->c_nsec = t_sec(&c_iop->t);
			display_io_track(per_io_ofp, q_iop);
		}

		list_del(&q_iop->f_head);
		io_release(q_iop);   /* 释放 io 结构 */
	}
}

void trace_complete(struct io *c_iop)
{
	if (c_iop->t.bytes == 0)
		return;

	if (io_setup(c_iop, IOP_C))
		handle_complete(c_iop);

	io_release(c_iop);
}


/* ======================================================================
 * 6.4 latency.c — 延迟数据输出
 * ======================================================================
 *
 * btt 可以将每个 I/O 的延迟数据输出到 .dat 文件，
 * 用于后续画图分析（如 gnuplot）。
 *
 * 文件格式：时间戳（秒） 延迟（秒）
 * 例如：
 *   0.001234 0.000045   ← 第 1.234ms 时的延迟是 45μs
 */

/* 输出一条延迟记录 */
static inline void latency_out(FILE *ofp, __u64 tstamp, __u64 latency)
{
	if (ofp)
		fprintf(ofp, "%lf %lf\n", TO_SEC(tstamp), TO_SEC(latency));
}

/* 为每个设备分配延迟输出文件 */
void latency_alloc(struct d_info *dip)
{
	dip->q2d_ofp = latency_open(dip, q2d_name, "q2d");  /* *_q2d.dat */
	dip->d2c_ofp = latency_open(dip, d2c_name, "d2c");  /* *_d2c.dat */
	dip->q2c_ofp = latency_open(dip, q2c_name, "q2c");  /* *_q2c.dat */
}

/* 在 trace_issue() 中调用：记录 Q2D 延迟 */
void latency_q2d(struct d_info *dip, __u64 tstamp, __u64 latency)
{
	plat_x2c(dip->q2d_plat_handle, tstamp, latency);
	latency_out(dip->q2d_ofp, tstamp, latency);
}

/* 在 trace_complete() 中调用：记录 D2C 延迟 */
void latency_d2c(struct d_info *dip, __u64 tstamp, __u64 latency)
{
	plat_x2c(dip->d2c_plat_handle, tstamp, latency);
	latency_out(dip->d2c_ofp, tstamp, latency);
}

/* 在 trace_complete() 中调用：记录 Q2C 延迟 */
void latency_q2c(struct d_info *dip, __u64 tstamp, __u64 latency)
{
	plat_x2c(dip->q2c_plat_handle, tstamp, latency);
	latency_out(dip->q2c_ofp, tstamp, latency);
}


/* ======================================================================
 * 6.5 q2d.c — Q2D 延迟直方图
 * ======================================================================
 *
 * 将 Q2D 延迟按区间统计百分比，用于快速判断调度器压力。
 *
 * 区间划分（毫秒）：
 *   <5ms, <10ms, <25ms, <50ms, <75ms, <100ms, <250ms, <500ms, <1s, ≥1s
 */

void q2d_histo_add(void *priv, __u64 q2d_in)
{
	int index;
	struct q2d_info *q2dp = priv;
	double q2d = BIT_TIME(q2d_in);
	long msec = (long)(q2d / 0.001);   /* 转换为毫秒 */

	switch (msec) {
	default:          index = 9; break;    /* ≥1000ms */
	case 500 ... 999: index = 8; break;
	case 250 ... 499: index = 7; break;
	case 100 ... 249: index = 6; break;
	case  75 ...  99: index = 5; break;
	case  50 ...  74: index = 4; break;
	case  25 ...  49: index = 3; break;
	case  10 ...  24: index = 2; break;
	case   5 ...   9: index = 1; break;
	case   0 ...   4: index = 0; break;    /* <5ms */
	}

	q2dp->histos[index]++;
	q2dp->nhistos++;
}


/*
 * ★ 总结：btt 的延迟计算流程
 *
 *   事件流：... Q(sector=100, time=T1) ... D(sector=100, time=T2) ... C(sector=100, time=T3) ...
 *
 *   btt 处理 Q 事件：
 *     创建 io 结构，记录 q_iop->t.time = T1
 *
 *   btt 处理 D 事件：
 *     找到 Q 事件的 io 结构
 *     Q2D = T2 - T1
 *     记录 q_iop->d_time = T2
 *
 *   btt 处理 C 事件：
 *     找到 Q 事件的 io 结构
 *     Q2C = T3 - T1
 *     D2C = T3 - T2  （因为 q_iop->d_time = T2）
 *
 *   配对依据：device + sector + bytes（不是 PID！）
 *   因为 C 事件在中断上下文中执行，PID 通常是 0。
 */
