/*
 * ======================================================================
 * Step 4: backend.c — do_io() 主循环（fio 的心脏）
 * ======================================================================
 *
 * 原始路径：src/fio/backend.c (第 1151-1438 行)
 *
 * do_io() 是 fio 中最核心的函数——每个 job 的 I/O 主循环都在这里执行。
 * 它不断重复 "取 io_u → 提交 → 等待完成 → 记录统计" 的过程，
 * 直到满足退出条件（时间到期、数据量达标、或出错）。
 *
 * 如果把 fio 比作一个工厂，do_io() 就是流水线本身。
 *
 * 阅读要点：
 *   1. 主循环的退出条件（时间/字节量/IO数/错误）
 *   2. get_io_u → queue → getevents 的三步曲
 *   3. 异步引擎如何处理 BUSY（队列满时先 reap）
 *   4. 验证（verify）工作负载的特殊处理
 *   5. 速率控制（rate limiting）和 thinktime
 *
 * 预计阅读时间：20 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ do_io() 主循环流程全景图
 * ======================================================================
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                    do_io() 入口                             │
 *   │  初始化：bytes_done, total_bytes, 运行状态                   │
 *   └────────────────────────┬────────────────────────────────────┘
 *                            │
 *                            ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │              while 主循环条件检查                             │
 *   │  iolog 未读完 || trim_list 非空 || 字节未超 || time_based    │
 *   └────────────────────────┬────────────────────────────────────┘
 *                            │
 *              ┌─────────────┼─────────────────────┐
 *              │ 终止检查     │ 时间检查              │ 字节量检查
 *              │ terminate?  │ runtime_exceeded?    │ bytes >= total?
 *              │ → break     │ → terminate + break  │ → break
 *              └─────────────┼─────────────────────┘
 *                            │ 通过
 *                            ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  ★ Step 1: get_io_u(td)                                    │
 *   │  从池中取出一个 io_u，填充好 offset/buflen/ddir              │
 *   │                                                             │
 *   │  如果返回 EBUSY → goto reap（先回收一些完成事件）             │
 *   │  如果返回 NULL  → break（退出循环）                          │
 *   └────────────────────────┬────────────────────────────────────┘
 *                            │
 *                            ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  Step 2: 后处理 io_u（验证/日志）                            │
 *   │                                                             │
 *   │  - 写操作 + verify：填充验证数据（populate_verify_io_u）      │
 *   │  - 读操作 + verify：设置 end_io 回调（verify_io_u）          │
 *   │  - log_io_piece()：记录 IO 片段供后续验证                    │
 *   └────────────────────────┬────────────────────────────────────┘
 *                            │
 *                            ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  ★ Step 3: 提交 I/O                                        │
 *   │                                                             │
 *   │  [IO_MODE_OFFLOAD]：workqueue_enqueue()（卸载到工作线程）     │
 *   │  [普通模式]：                                                │
 *   │    io_u_submit(td, io_u)                                    │
 *   │      → td_io_queue(td, io_u)                                │
 *   │        → engine->queue(td, io_u)                            │
 *   │          ├── sync:  read()/write() → FIO_Q_COMPLETED        │
 *   │          └── libaio: 入队 → FIO_Q_QUEUED                    │
 *   │                                                             │
 *   │    io_queue_event() 处理同步完成结果                         │
 *   └────────────────────────┬────────────────────────────────────┘
 *                            │
 *                            ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  ★ Step 4: reap — 等待异步完成                              │
 *   │                                                             │
 *   │  reap:                                                      │
 *   │    full = queue_full(td) || (BUSY && cur_depth > 0)         │
 *   │    if (full || io_in_polling(td)):                          │
 *   │      wait_for_completions(td, &comp_time)                   │
 *   │        → td_io_getevents() → engine->getevents()            │
 *   │        → 对每个完成的 io_u 执行 end_io 回调                  │
 *   └────────────────────────┬────────────────────────────────────┘
 *                            │
 *                            ▼
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  Step 5: 辅助处理                                           │
 *   │                                                             │
 *   │  - thinkcycles：CPU 空转模拟计算开销                         │
 *   │  - thinktime：sleep 模拟思考时间                             │
 *   │  - check_min_rate：速率下限检查                              │
 *   │  - lat_target_check：延迟目标检查                            │
 *   │                                                             │
 *   │  → 回到 while 循环顶部                                      │
 *   └─────────────────────────────────────────────────────────────┘
 */


/* ======================================================================
 * ★ 核心函数: do_io() — I/O 主循环
 * ======================================================================
 *
 * 参数：
 *   td         — 当前 job 的 thread_data（包含所有配置和状态）
 *   bytes_done — 输出参数，记录各方向已完成的字节数
 *
 * 返回值：无（通过 td->error 和 bytes_done 报告结果）
 */
static void do_io(struct thread_data *td, uint64_t *bytes_done)
{
	unsigned int i;
	int ret = 0;
	uint64_t total_bytes, bytes_issued = 0;

	/* 记录各方向的已完成字节数（从上次断点继续） */
	for (i = 0; i < DDIR_RWDIR_CNT; i++)
		bytes_done[i] = td->bytes_done[i];

	/* 设置运行状态：ramp 期（热身）或 running 期 */
	if (in_ramp_period(td))
		td_set_runstate(td, TD_RAMP);
	else
		td_set_runstate(td, TD_RUNNING);

	lat_target_init(td);

	/* ─── 计算总目标字节量 ─── */
	/*
	 * total_bytes 的确定有多重考虑：
	 *   1. 普通情况：td->o.size（--size 参数）
	 *   2. 随机写 + norandommap：可能写到 io_size（覆盖整个区域）
	 *   3. 顺序写 + io_size > size：允许写到 io_size
	 *   4. verify_backlog：验证模式可能需要双倍字节
	 *   5. trimwrite：先 trim 再写，双倍字节
	 */
	total_bytes = td->o.size;

	if (td_write(td) && td_random(td) && td->o.norandommap)
		total_bytes = max(total_bytes, (uint64_t) td->o.io_size);

	if (td_write(td) && !td_random(td))
		total_bytes = max(total_bytes, (uint64_t)td->o.io_size);

	if (td->o.verify != VERIFY_NONE &&
	   (td_write(td) && td->o.verify_backlog))
		total_bytes += td->o.size;

	if (td_trimwrite(td)) {
		total_bytes += td->total_io_size;
		td->o.number_ios *= 2;
	}

	/* ==================================================================
	 * ★ 主循环开始
	 * ==================================================================
	 *
	 * 循环继续的条件（满足任一即可继续）：
	 *   1. 正在读 iolog（回放模式），且 iolog 列表非空
	 *   2. trim_list 非空（有待处理的 trim）
	 *   3. 已发出的字节量未超过上限
	 *   4. time_based 模式（基于时间，不关心字节量）
	 */
	while ((td->o.read_iolog_file && !flist_empty(&td->io_log_list)) ||
		(!flist_empty(&td->trim_list)) || !io_issue_bytes_exceeded(td) ||
		td->o.time_based) {

		struct timespec comp_time;
		struct io_u *io_u;
		int full;
		enum fio_ddir ddir;

		check_update_rusage(td);

		/* ─── 退出条件检查 #1：外部终止 ─── */
		if (td->terminate || td->done)
			break;

		update_ts_cache(td);

		/* ─── 退出条件检查 #2：运行时间超限 ─── */
		if (runtime_exceeded(td, &td->ts_cache)) {
			__update_ts_cache(td);
			if (runtime_exceeded(td, &td->ts_cache)) {
				fio_mark_td_terminate(td);
				break;
			}
		}

		/* ─── 退出条件检查 #3：流控 ─── */
		if (flow_threshold_exceeded(td))
			continue;

		/* ─── 退出条件检查 #4：字节量达标 ─── */
		if (bytes_issued >= total_bytes &&
		    !td->o.read_iolog_file &&
		    (!td->o.time_based ||
		     (td->o.time_based && td->o.verify != VERIFY_NONE)))
			break;

		/* ==============================================================
		 * ★ 核心步骤 1: 获取 io_u
		 * ==============================================================
		 *
		 * get_io_u() 完成以下工作：
		 *   - 从空闲池弹出一个 io_u
		 *   - 选择目标文件
		 *   - 计算 offset（顺序递增 or 随机生成）
		 *   - 计算 buflen（固定 or 随机范围）
		 *   - 设置 ddir（读/写/trim/sync）
		 *   - 填充缓冲区内容（写入时）
		 */
		io_u = get_io_u(td);
		if (IS_ERR_OR_NULL(io_u)) {
			int err = PTR_ERR(io_u);

			io_u = NULL;
			ddir = DDIR_INVAL;
			if (err == -EBUSY) {
				/* ★ 引擎返回 BUSY：队列满了，先去 reap 一些完成事件 */
				ret = FIO_Q_BUSY;
				goto reap;
			}
			if (td->o.latency_target)
				goto reap;
			break;
		}

		/* ─── 写操作 + 验证模式：填充校验数据 ─── */
		if (io_u->ddir == DDIR_WRITE && td->flags & TD_F_DO_VERIFY) {
			if (!(io_u->flags & IO_U_F_PATTERN_DONE)) {
				io_u_set(td, io_u, IO_U_F_PATTERN_DONE);
				io_u->numberio = td->io_issues[io_u->ddir];
				populate_verify_io_u(td, io_u);
				log_inflight(td, io_u);
			}
		} else if (ddir_sync(io_u->ddir)) {
			on_fsync_submitted(td, io_u);
		}

		ddir = io_u->ddir;

		/* ─── 读操作 + 验证模式：设置完成回调 ─── */
		/*
		 * 当 --verify 被启用时，读操作完成后需要检查数据正确性。
		 * 通过设置 io_u->end_io 回调来实现：
		 *   - verify_io_u:       同步验证（直接在完成路径中检查）
		 *   - verify_io_u_async: 异步验证（提交到验证线程）
		 */
		if (td->o.verify != VERIFY_NONE && io_u->ddir == DDIR_READ &&
		    ((io_u->flags & IO_U_F_VER_LIST) || !td_rw(td))) {
			/* ... 验证相关的 numberio/seed 设置省略 ... */

			if (td->o.verify_async)
				io_u->end_io = verify_io_u_async;
			else
				io_u->end_io = verify_io_u;
			td_set_runstate(td, TD_VERIFYING);
		} else if (in_ramp_period(td))
			td_set_runstate(td, TD_RAMP);
		else
			td_set_runstate(td, TD_RUNNING);

		/* ─── 记录 IO 片段（供后续验证使用） ─── */
		if (td_write(td) && io_u->ddir == DDIR_WRITE &&
		    (td->o.do_verify || td->trim_verify) &&
		    td->o.verify != VERIFY_NONE &&
		    !td->o.experimental_verify)
			log_io_piece(td, io_u);

		/* ==============================================================
		 * ★ 核心步骤 2 & 3: 提交 I/O 并处理结果
		 * ==============================================================
		 */
		if (td->o.io_submit_mode == IO_MODE_OFFLOAD) {
			/*
			 * 卸载模式：将 io_u 放入工作队列，由另一个线程执行
			 * 用于模拟 I/O 提交和计算在不同线程的场景
			 */
			workqueue_enqueue(&td->io_wq, &io_u->work);
			ret = FIO_Q_QUEUED;
			/* ... 统计更新 ... */
		} else {
			/*
			 * ★ 普通模式（最常见）：
			 *   io_u_submit() → td_io_queue() → engine->queue()
			 */
			ret = io_u_submit(td, io_u);

			/* 速率控制：计算下一个 IO 的预期时间 */
			if (ddir_rw(ddir) && should_check_rate(td))
				td->rate_next_io_time[ddir] = usec_for_io(td, ddir);

			/* 处理同步完成的结果 */
			if (io_queue_event(td, io_u, &ret, ddir, &bytes_issued, 0, &comp_time))
				break;

			/* ==============================================================
			 * ★ 核心步骤 4: reap — 收割完成事件
			 * ==============================================================
			 *
			 * 对于异步引擎（libaio/io_uring），queue() 只是入队，
			 * 需要定期调用 getevents() 来收割已完成的 I/O。
			 *
			 * 何时 reap：
			 *   - 队列满了（queue_full）
			 *   - 引擎返回 BUSY（资源不足）
			 *   - polling 模式（持续轮询完成）
			 */
reap:
			full = queue_full(td) ||
				(ret == FIO_Q_BUSY && td->cur_depth);
			if (full || io_in_polling(td))
				ret = wait_for_completions(td, &comp_time);
		}
		if (ret < 0)
			break;

		/* ─── thinktime：模拟"思考"延迟 ─── */
		/*
		 * --thinktime=N 参数：每次 IO 后 sleep N 微秒
		 * 用于模拟应用层的处理时间，使 IO 模式更接近真实场景
		 */
		if (ddir_rw(ddir) && td->o.thinkcycles)
			cycles_spin(td->o.thinkcycles);

		if (ddir_rw(ddir) && td->o.thinktime)
			handle_thinktime(td, ddir, &comp_time);

		if (!ddir_rw_sum(td->bytes_done) &&
		    !td_ioengine_flagged(td, FIO_NOIO))
			continue;

		/* ─── 速率下限检查 ─── */
		if (!in_ramp_period(td) && should_check_rate(td)) {
			if (check_min_rate(td, &comp_time)) {
				if (exitall_on_terminate || td->o.exitall_error)
					fio_terminate_threads(td->groupid, td->o.exit_what);
				td_verror(td, EIO, "check_min_rate");
				break;
			}
		}

		/* ─── 延迟目标检查 ─── */
		if (!in_ramp_period(td) && td->o.latency_target)
			lat_target_check(td);
	}
	/* ==================================================================
	 * ★ 主循环结束
	 * ================================================================== */

	check_update_rusage(td);

	if (td->trim_entries)
		log_err("fio: %lu trim entries leaked?\n", td->trim_entries);

	/* ─── 收尾工作 ─── */
	if (td->o.fill_device && (td->error == ENOSPC || td->error == EDQUOT)) {
		td->error = 0;
		fio_mark_td_terminate(td);
	}
	if (!td->error) {
		struct fio_file *f;

		/* 等待所有在飞的 IO 完成 */
		if (td->o.io_submit_mode == IO_MODE_OFFLOAD) {
			workqueue_flush(&td->io_wq);
			i = 0;
		} else
			i = td->cur_depth;

		if (i) {
			ret = io_u_queued_complete(td, i);
			/* ... */
		}

		/* 最终 fsync（--end_fsync=1） */
		if (should_fsync(td) &&
		    (td->o.end_fsync || td->o.end_syncfs ||
		     td->o.fsync_on_close)) {
			td_set_runstate(td, TD_FSYNCING);
			/* ... 执行 fsync/syncfs ... */
		}
	}
	/* ... 错误处理 ... */
}


/*
 * ======================================================================
 * ★ 总结：do_io() 的数据流
 * ======================================================================
 *
 *   每次循环迭代产生一个 I/O：
 *
 *   get_io_u()                    获取 io_u + 计算 offset/buflen/ddir
 *     │
 *     ▼
 *   io_u_submit()                 提交 I/O
 *     │
 *     ├── td_io_queue()
 *     │     └── engine->queue()
 *     │           ├── sync:  read()/write() → FIO_Q_COMPLETED
 *     │           └── libaio: 入队 → FIO_Q_QUEUED
 *     │
 *     └── io_queue_event()        处理同步完成
 *           │
 *           ▼
 *   wait_for_completions()         等待异步完成（如果需要）
 *     └── td_io_getevents()
 *           └── engine->getevents()
 *                 └── engine->event(N)  获取第 N 个完成的 io_u
 *                       │
 *                       ▼
 *                 io_u->end_io()   完成回调（统计/验证）
 *                       │
 *                       ▼
 *                 put_io_u()       归还到空闲池
 *
 *   关键洞察：
 *     do_io() 是一个典型的"生产者-消费者"循环：
 *       生产者：get_io_u() 生产 I/O 请求
 *       消费者：引擎执行 I/O，wait_for_completions() 消费结果
 *       队列深度（iodepth）控制同时在飞的 I/O 数量
 */
