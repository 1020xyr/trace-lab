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
 *   1. 主循环的 5 种退出条件（terminate/runtime/flow/bytes/NULL io_u）
 *   2. get_io_u → queue → getevents 的三步曲
 *   3. 异步引擎如何处理 BUSY（队列满时先 reap）
 *   4. 验证（verify）工作负载的特殊处理
 *   5. rate limiting 机制（usec_for_io + rate_next_io_time）
 *   6. thinktime 机制（handle_thinktime + thinkcycles）
 *   7. reap 机制详解（wait_for_completions 的批量收割）
 *
 * 预计阅读时间：30 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ do_io() 主循环流程全景图
 * ======================================================================
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                    do_io() 入口                             │
 *   │  初始化：bytes_done, total_bytes, 运行状态                   │
 *   │  计算 total_bytes（考虑 5 种退出条件的字节量）                │
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
 * ★ 退出条件详解：5 种终止循环的方式
 * ======================================================================
 *
 * do_io() 的 while 循环有 5 种退出路径：
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 退出条件     │ 触发时机               │ 源码位置                  │
 * ├──────────────────────────────────────────────────────────────────┤
 * │ 1. terminate │ td->terminate 被设置   │ 信号/SIGTERM/外部控制     │
 * │    /done     │ td->done = true        │ 另一个 job 触发 exitall   │
 * ├──────────────────────────────────────────────────────────────────┤
 * │ 2. runtime   │ runtime_exceeded()     │ --runtime=60 到期         │
 * │   exceeded   │ 返回 true              │ 设置 fio_mark_td_terminate│
 * ├──────────────────────────────────────────────────────────────────┤
 * │ 3. bytes     │ bytes_issued >= total  │ --size 或 --io_size 达标  │
 * │   issued     │ 且非 iolog/time_based  │ time_based+verify 例外    │
 * ├──────────────────────────────────────────────────────────────────┤
 * │ 4. io_u      │ get_io_u() 返回 NULL   │ 文件读完/随机map耗尽      │
 * │   is NULL    │ 且非 BUSY              │ number_ios 达标           │
 * ├──────────────────────────────────────────────────────────────────┤
 * │ 5. error     │ ret < 0 (I/O 错误)     │ 设备报错/ENOSPC           │
 * │              │ check_min_rate 失败    │ 速率低于 ratemin          │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * total_bytes 的计算考虑了多种情况：
 *   - 普通情况：td->o.size（--size 参数）
 *   - 随机写 + norandommap：max(size, io_size)
 *   - 顺序写：max(size, io_size)
 *   - verify_backlog：total_bytes += td->o.size（需要双倍字节）
 *   - trimwrite：total_bytes += total_io_size（trim + write 双倍）
 */


/* ======================================================================
 * ★ reap 机制详解：异步引擎如何收割完成事件
 * ======================================================================
 *
 * 对于异步引擎（libaio / io_uring），I/O 提交后不会立即完成。
 * 需要定期调用 engine->getevents() 来"收割"已完成的事件。
 *
 * 收割时机：
 *
 *   ┌─────────────────────────────────────────────────┐
 *   │ 何时触发 reap？                                  │
 *   │                                                  │
 *   │  1. queue_full(td) = true                       │
 *   │     → 在飞 I/O 数达到 iodepth，必须先收割        │
 *   │                                                  │
 *   │  2. ret == FIO_Q_BUSY && cur_depth > 0           │
 *   │     → 引擎返回 BUSY（资源不足），先收割再试      │
 *   │                                                  │
 *   │  3. io_in_polling(td) = true                     │
 *   │     → 启用了 I/O polling 模式，持续轮询          │
 *   └─────────────────────────────────────────────────┘
 *
 * reap 的内部流程（wait_for_completions）：
 *
 *   wait_for_completions(td, &comp_time)
 *     │
 *     ├─ 计算 min_evts（最少等待事件数）
 *     │    ├─ queue_full 时：min_evts = max(1, iodepth_batch_complete_min)
 *     │    └─ 非满时：min_evts = iodepth_batch_complete_min
 *     │
 *     ├─ 循环：do {
 *     │    io_u_queued_complete(td, min_evts)
 *     │      → td_io_getevents(td, min_evts)
 *     │        → engine->getevents(td, min_evts)
 *     │          → libaio: io_getevents() 系统调用
 *     │          → io_uring: io_uring_wait_cqe()
 *     │
 *     │    对每个完成的 io_u：
 *     │      → io_u->end_io() 回调（更新统计 / 验证数据）
 *     │      → put_io_u() 归还到空闲池
 *     │
 *     │  } while (full && cur_depth > iodepth_low);
 *     │    ★ 队列满时持续收割直到低于低水位线
 *     └─
 *
 * iodepth 水位线示意：
 *
 *   iodepth=32           ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
 *                        ↑ 到达此行 → 必须 reap
 *
 *   iodepth_low=4        ■■■■
 *                        ↑ 收割到此行以下才停止
 *
 *   cur_depth=20         ■■■■■■■■■■■■■■■■■■■■
 *                        ↑ 当前在飞 20 个 I/O
 *
 *   流程：
 *   1. 提交 I/O 直到 cur_depth == iodepth (32)
 *   2. queue_full → reap：收割完成事件
 *   3. 持续 reap 直到 cur_depth <= iodepth_low (4)
 *   4. 继续提交 I/O...
 */


/* ======================================================================
 * ★ rate limiting 机制详解
 * ======================================================================
 *
 * fio 支持两种速率控制：
 *   - --rate / --rate_iops：限制 I/O 速率上限
 *   - --ratemin / --rate_iops_min：保证 I/O 速率下限
 *
 * 速率上限的工作原理：
 *
 *   1. 每次 I/O 提交后，计算下一个 I/O 的预期时间：
 *      td->rate_next_io_time[ddir] = usec_for_io(td, ddir)
 *
 *   2. usec_for_io() 的计算逻辑（backend.c:856-900）：
 *
 *      ┌─ RATE_PROCESS_POISSON（泊松分布）─────────────────────┐
 *      │  iops = bps / min_bs                                   │
 *      │  interval = (1000000 / iops) * -log(random)            │
 *      │  ★ 模拟真实 I/O 到达的随机性                           │
 *      └────────────────────────────────────────────────────────┘
 *
 *      ┌─ RATE_PROCESS_LINEAR（线性/固定间隔）─────────────────┐
 *      │  if rate_iops + bssplit:                               │
 *      │    interval = 1000000 / iops                           │
 *      │  else:                                                  │
 *      │    secs = issued_bytes / bps                           │
 *      │    interval = secs * 1000000 + remainder * 1000000/bps│
 *      │  ★ 基于已发送字节量计算理想时间                        │
 *      └────────────────────────────────────────────────────────┘
 *
 *   3. 如果当前时间 < rate_next_io_time，fio 会等待
 *      （通过 io_queue_event 内部的速率检查）
 *
 * 速率下限的检查（check_min_rate）：
 *
 *   每次循环迭代后，检查是否满足最低速率要求：
 *
 *   __check_min_rate(td, now, ddir):
 *     1. 前 2 秒不检查（热身期）
 *     2. 检查间隔不低于 ratecycle（默认 500ms）
 *     3. 计算实际速率 = (current_bytes - last_bytes) * 1000 / elapsed
 *     4. 如果 actual_rate < ratemin → 报错并终止
 */


/* ======================================================================
 * ★ thinktime 机制详解
 * ======================================================================
 *
 * --thinktime=N 参数：在每次（或每 N 次）I/O 后插入延迟，
 * 模拟应用层的"思考"时间，使 I/O 模式更接近真实场景。
 *
 * handle_thinktime() 的工作流程（backend.c:912-1007）：
 *
 *   1. 判断是否需要 think：
 *      ├─ thinktime_iotime：基于时间间隔触发
 *      │   └─ 距上次 thinktime 超过 thinktime_iotime μs
 *      └─ thinktime_blocks：基于 I/O 次数触发
 *          └─ 已完成的 I/O 数达到 thinktime_blocks
 *
 *   2. 执行 thinktime：
 *      ├─ io_u_quiesce(td)：等待所有在飞 I/O 完成
 *      │   ★ 确保 thinktime 期间没有 I/O 在飞
 *      ├─ thinktime_spin：前 N μs 用 CPU 空转（高精度）
 *      │   └─ usec_spin(left)：忙等
 *      └─ thinktime（剩余）：用 sleep（让出 CPU）
 *          └─ usec_sleep(td, left)：nanosleep()
 *
 *   3. 速率补偿（rate_ign_think = 0 时）：
 *      如果设置了 --rate，thinktime 期间会"错过"一些 I/O。
 *      fio 会计算错过的字节数并补偿到 rate_io_issue_bytes，
 *      使得长期平均速率仍然符合 --rate 设定。
 *
 * --thinkcycles=N 参数：
 *   每次 I/O 后用 CPU 空转 N 个周期。
 *   cycles_spin(N) 执行 N 次空循环，模拟计算开销。
 *   与 thinktime 不同，thinkcycles 不会让出 CPU。
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

	/* ─── 初始化：记录各方向的已完成字节数（从上次断点继续） ─── */
	/*
	 * DDIR_RWDIR_CNT = 3，分别对应：
	 *   DDIR_READ  (0) — 读
	 *   DDIR_WRITE (1) — 写
	 *   DDIR_TRIM  (2) — trim
	 * bytes_done[i] 用于统计各方向的完成量
	 */
	for (i = 0; i < DDIR_RWDIR_CNT; i++)
		bytes_done[i] = td->bytes_done[i];

	/* ─── 设置运行状态 ─── */
	/*
	 * in_ramp_period(td)：检查是否在热身期（--ramp_time）
	 * 热身期的 I/O 不计入统计，用于让系统达到稳态
	 */
	if (in_ramp_period(td))
		td_set_runstate(td, TD_RAMP);
	else
		td_set_runstate(td, TD_RUNNING);

	/* 初始化延迟目标检查（--latency_target） */
	lat_target_init(td);

	/* ==================================================================
	 * ★ 计算总目标字节量 total_bytes
	 * ==================================================================
	 *
	 * total_bytes 的确定需要综合考虑多种退出条件：
	 *
	 * 情况 1：普通模式
	 *   total_bytes = td->o.size（--size 参数指定的大小）
	 *
	 * 情况 2：随机写 + norandommap
	 *   norandommap 允许重复写同一位置，所以需要写更多才能覆盖
	 *   total_bytes = max(size, io_size)
	 *
	 * 情况 3：顺序写 + io_size > size
	 *   允许写到 io_size（--io_size 参数）
	 *   total_bytes = max(size, io_size)
	 *
	 * 情况 4：verify_backlog
	 *   边写边验证，需要写入 + 验证读 = 双倍字节
	 *   total_bytes += td->o.size
	 *
	 * 情况 5：trimwrite
	 *   先 trim 再写，每个位置两次操作
	 *   total_bytes += total_io_size
	 *   number_ios *= 2
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
	 * while 循环的继续条件（满足任一即可继续）：
	 *
	 *   1. td->o.read_iolog_file && !flist_empty(&td->io_log_list)
	 *      → 正在回放 iolog（blktrace 文件或自定义日志）
	 *      → 且日志列表中还有待回放的 I/O
	 *
	 *   2. !flist_empty(&td->trim_list)
	 *      → 有未处理的 trim 操作
	 *
	 *   3. !io_issue_bytes_exceeded(td)
	 *      → 已发送的字节量未超过上限
	 *      → 对应退出条件 3（bytes_issued >= total_bytes）
	 *
	 *   4. td->o.time_based
	 *      → 基于时间的运行模式（--time_based）
	 *      → 即使数据写完也继续运行直到 runtime 到期
	 */
	while ((td->o.read_iolog_file && !flist_empty(&td->io_log_list)) ||
		(!flist_empty(&td->trim_list)) || !io_issue_bytes_exceeded(td) ||
		td->o.time_based) {

		struct timespec comp_time;
		struct io_u *io_u;
		int full;
		enum fio_ddir ddir;

		/* 更新资源使用统计（CPU 时间、内存等） */
		check_update_rusage(td);

		/* ─── 退出条件 #1：外部终止 ─── */
		/*
		 * td->terminate：外部信号（SIGTERM/SIGINT）或 exitall 触发
		 * td->done：当前 job 已完成（例如另一个 job 的 exitall_error）
		 */
		if (td->terminate || td->done)
			break;

		/* 更新时间戳缓存（避免频繁调用 gettimeofday） */
		update_ts_cache(td);

		/* ─── 退出条件 #2：运行时间超限 ─── */
		/*
		 * runtime_exceeded() 检查 td->o.timeout（--runtime 参数）
		 *
		 * 为什么检查两次？
		 *   第一次用缓存的时间戳（快速但不精确）
		 *   如果第一次通过，再用最新时间戳确认（精确）
		 *   这是性能优化：避免每次迭代都调用 gettimeofday()
		 */
		if (runtime_exceeded(td, &td->ts_cache)) {
			__update_ts_cache(td);
			if (runtime_exceeded(td, &td->ts_cache)) {
				fio_mark_td_terminate(td);
				break;
			}
		}

		/* ─── 退出条件 #3：流控 ─── */
		/*
		 * flow_threshold_exceeded()：用于多 job 之间的流控
		 * 如果设置了 --flow 参数，当 flow counter 超过阈值时跳过本次
		 * （不是 break，而是 continue → 不提交 I/O 但不退出循环）
		 */
		if (flow_threshold_exceeded(td))
			continue;

		/* ─── 退出条件 #4：字节量达标 ─── */
		/*
		 * bytes_issued >= total_bytes 时的退出逻辑：
		 *
		 *   如果 read_iolog_file → 不退出（iolog 有自己的终止条件）
		 *   如果 time_based 且无 verify → 不退出（继续运行直到 runtime）
		 *   如果 time_based 且有 verify → 退出（写入完成，开始验证阶段）
		 *   其他情况 → 退出
		 */
		if (bytes_issued >= total_bytes &&
		    !td->o.read_iolog_file &&
		    (!td->o.time_based ||
		     (td->o.time_based && td->o.verify != VERIFY_NONE)))
			break;

		/* ==============================================================
		 * ★ 核心步骤 1: 获取 io_u
		 * ==============================================================
		 *
		 * get_io_u() 是 I/O 的"生产者"，完成以下工作：
		 *   - 从空闲池（io_u_freelist）弹出一个 io_u
		 *   - 选择目标文件（多文件时轮转）
		 *   - 计算 offset（顺序递增 or 随机生成）
		 *   - 计算 buflen（固定 or 随机范围 bsrange）
		 *   - 设置 ddir（读/写/trim/sync）
		 *   - 填充缓冲区内容（写入时）
		 *
		 * 返回值处理：
		 *   - 正常返回 io_u 指针 → 继续处理
		 *   - ERR_PTR(-EBUSY) → 队列满或资源不足，goto reap
		 *   - NULL → 没有更多 I/O 可做，退出循环
		 *   - ERR_PTR(other) → 错误，退出循环
		 */
		io_u = get_io_u(td);
		if (IS_ERR_OR_NULL(io_u)) {
			int err = PTR_ERR(io_u);

			io_u = NULL;
			ddir = DDIR_INVAL;
			if (err == -EBUSY) {
				/*
				 * ★ 引擎返回 BUSY：
				 * 可能原因：
				 *   1. 异步引擎的在飞 I/O 数达到上限
				 *   2. 系统资源不足（如 aio_max_nr 限制）
				 *   3. 设备队列满
				 *
				 * 处理：goto reap → 先收割一些完成事件释放资源
				 */
				ret = FIO_Q_BUSY;
				goto reap;
			}
			/*
			 * latency_target 模式下，即使 io_u 为 NULL 也要去 reap
			 * 因为可能需要等待在飞 I/O 完成来更新延迟统计
			 */
			if (td->o.latency_target)
				goto reap;
			/* ★ 退出条件 #5：io_u 为 NULL，没有更多 I/O */
			break;
		}

		/* ==============================================================
		 * Step 2: 验证模式预处理
		 * ==============================================================
		 */

		/* ─── 写操作 + 验证模式：填充校验数据 ─── */
		/*
		 * 当 --verify 被启用时，写入的数据需要包含校验和。
		 * populate_verify_io_u() 在 buffer 中填充：
		 *   - 校验头（verify_header）
		 *   - 校验数据（CRC32C / MD5 / SHA 等）
		 *   - 随机种子（用于验证时重新生成期望数据）
		 *
		 * IO_U_F_PATTERN_DONE 标志防止重复填充（io_u 被复用时）
		 * log_inflight() 记录在飞的 I/O，用于 fsync 安全性检查
		 */
		if (io_u->ddir == DDIR_WRITE && td->flags & TD_F_DO_VERIFY) {
			if (!(io_u->flags & IO_U_F_PATTERN_DONE)) {
				io_u_set(td, io_u, IO_U_F_PATTERN_DONE);
				io_u->numberio = td->io_issues[io_u->ddir];
				populate_verify_io_u(td, io_u);
				log_inflight(td, io_u);
			}
		} else if (ddir_sync(io_u->ddir)) {
			/* fsync/fdatasync 操作：记录提交时间 */
			on_fsync_submitted(td, io_u);
		}

		ddir = io_u->ddir;

		/* ─── 读操作 + 验证模式：设置完成回调 ─── */
		/*
		 * 验证模式下的读操作需要在完成后检查数据正确性。
		 * 通过设置 io_u->end_io 回调来实现：
		 *
		 *   verify_io_u:       同步验证（直接在完成路径中检查）
		 *                      读取数据 → 重新计算校验和 → 对比
		 *                      不匹配 → td_verror() 报错
		 *
		 *   verify_io_u_async: 异步验证（提交到验证线程）
		 *                      读取数据 → 放入验证队列
		 *                      验证线程异步处理
		 *
		 * 额外处理：
		 *   - 纯读工作负载 + verify：需要生成随机种子
		 *   - verify_state_load：加载之前的验证状态
		 *   - verify_state_should_skip/stop：状态同步
		 */
		if (td->o.verify != VERIFY_NONE && io_u->ddir == DDIR_READ &&
		    ((io_u->flags & IO_U_F_VER_LIST) || !td_rw(td))) {

			/* 纯读工作负载：生成随机种子用于后续验证 */
			if (!td_write(td) && !td->o.verify_pattern_bytes) {
				io_u->rand_seed = __rand(&td->verify_state);
				if (sizeof(int) != sizeof(long *))
					io_u->rand_seed *= __rand(&td->verify_state);
			}

			/* 纯读工作负载：分配 numberio（用于验证状态匹配） */
			if (!td_rw(td) && !(io_u->flags & IO_U_F_VER_LIST))
				io_u->numberio = td->io_issues[io_u->ddir];

			/* 验证状态同步：跳过或停止 */
			if (verify_state_should_skip(td, io_u->numberio)) {
				td->io_issues[io_u->ddir]++;
				put_io_u(td, io_u);
				continue;
			}
			if (verify_state_should_stop(td, io_u->numberio)) {
				put_io_u(td, io_u);
				break;
			}

			/* ★ 设置完成回调：I/O 完成后自动验证 */
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
		/*
		 * log_io_piece() 将当前 I/O 的信息（offset/buflen/numberio）
		 * 记录到 io_hist 链表中。验证阶段会根据这个记录来：
		 *   1. 知道每个位置应该有什么数据
		 *   2. 按正确顺序回放验证
		 *
		 * 条件：写操作 + (do_verify || trim_verify) + 非实验性验证
		 */
		if (td_write(td) && io_u->ddir == DDIR_WRITE &&
		    (td->o.do_verify || td->trim_verify) &&
		    td->o.verify != VERIFY_NONE &&
		    !td->o.experimental_verify)
			log_io_piece(td, io_u);

		/* ==============================================================
		 * ★ 核心步骤 2 & 3: 提交 I/O 并处理结果
		 * ==============================================================
		 *
		 * 两种提交模式：
		 *   1. IO_MODE_OFFLOAD：卸载到工作线程
		 *   2. 普通模式：直接在当前线程提交
		 */
		if (td->o.io_submit_mode == IO_MODE_OFFLOAD) {
			/*
			 * ─── 卸载模式 ───
			 * 将 io_u 放入工作队列，由另一个线程执行 I/O。
			 * 用于模拟 I/O 提交和计算在不同线程的场景。
			 *
			 * workqueue_enqueue() 将 io_u->work 加入队列，
			 * 工作线程调用 td_io_queue() 执行实际 I/O。
			 */
			const unsigned long long blen = io_u->xfer_buflen;
			const enum fio_ddir __ddir = acct_ddir(io_u);

			if (td->error)
				break;

			workqueue_enqueue(&td->io_wq, &io_u->work);
			ret = FIO_Q_QUEUED;

			/* 更新统计计数器 */
			if (ddir_rw(__ddir)) {
				td->io_issues[__ddir]++;
				td->io_issue_bytes[__ddir] += blen;
				td->rate_io_issue_bytes[__ddir] += blen;
			}

			/* 速率控制：计算下一个 I/O 的预期时间 */
			if (ddir_rw(__ddir) && should_check_rate(td)) {
				td->rate_next_io_time[__ddir] = usec_for_io(td, __ddir);
				fio_gettime(&comp_time, NULL);
			}

		} else {
			/* ==============================================================
			 * ─── 普通模式（最常见） ───
			 * ==============================================================
			 *
			 * 调用链：
			 *   io_u_submit(td, io_u)
			 *     → td_io_queue(td, io_u)
			 *       → engine->queue(td, io_u)
			 *         ├── sync 引擎: read()/write()
			 *         │   → 阻塞直到完成
			 *         │   → 返回 FIO_Q_COMPLETED
			 *         ├── libaio 引擎: io_submit()
			 *         │   → 入队到内核 AIO 上下文
			 *         │   → 返回 FIO_Q_QUEUED（异步）
			 *         └── io_uring 引擎: io_uring_submit()
			 *             → 提交 SQE 到 io_uring
			 *             → 返回 FIO_Q_QUEUED（异步）
			 */
			ret = io_u_submit(td, io_u);

			/* ─── 速率控制：计算下一个 I/O 的预期时间 ─── */
			/*
			 * should_check_rate(td) 检查是否启用了速率限制
			 * （--rate / --rate_iops 参数）
			 *
			 * usec_for_io() 根据当前已发送的字节量和目标速率，
			 * 计算下一个 I/O 应该在什么时间点发出。
			 *
			 * rate_next_io_time 在 io_queue_event 中被用于决定是否等待
			 */
			if (ddir_rw(ddir) && should_check_rate(td))
				td->rate_next_io_time[ddir] = usec_for_io(td, ddir);

			/* ─── 处理同步完成的结果 ─── */
			/*
			 * io_queue_event() 处理引擎返回的状态：
			 *
			 *   FIO_Q_COMPLETED（同步完成）：
			 *     → 检查 io_u->error
			 *     → 处理 short I/O（io_u->resid > 0）
			 *     → 调用 io_u_sync_complete() 更新统计
			 *     → 如果是验证模式，执行验证
			 *
			 *   FIO_Q_QUEUED（异步入队）：
			 *     → 不做额外处理，等待 reap
			 *
			 *   FIO_Q_BUSY（资源忙）：
			 *     → requeue_io_u()：将 io_u 放回队列
			 *     → 跳到 reap 标签
			 *
			 * 如果 io_queue_event() 返回 true，说明遇到致命错误，break
			 */
			if (io_queue_event(td, io_u, &ret, ddir, &bytes_issued, 0, &comp_time))
				break;

			/* ==============================================================
			 * ★ 核心步骤 4: reap — 收割完成事件
			 * ==============================================================
			 *
			 * reap 标签：当队列满或引擎 BUSY 时跳到这里
			 *
			 * 对于异步引擎，I/O 提交后不会立即完成。
			 * 需要调用 wait_for_completions() 来收割已完成的事件。
			 *
			 * 触发 reap 的条件：
			 *   1. queue_full(td)：在飞 I/O 数 == iodepth
			 *   2. ret == FIO_Q_BUSY && cur_depth > 0：引擎忙且有在飞 I/O
			 *   3. io_in_polling(td)：I/O polling 模式（持续轮询）
			 *
			 * wait_for_completions() 内部：
			 *   → 计算 min_evts（最少等待数）
			 *   → io_u_queued_complete(td, min_evts)
			 *     → td_io_getevents()
			 *       → engine->getevents()
			 *         → libaio: io_getevents(ctx, min_evts, ...)
			 *         → io_uring: io_uring_wait_cqe()
			 *   → 对每个完成的 io_u：
			 *     → io_u->end_io() 回调
			 *       → 统计延迟、带宽、IOPS
			 *       → 验证模式下检查数据完整性
			 *     → put_io_u() 归还到空闲池
			 *   → 循环收割直到 cur_depth <= iodepth_low
			 */
reap:
			full = queue_full(td) ||
				(ret == FIO_Q_BUSY && td->cur_depth);
			if (full || io_in_polling(td))
				ret = wait_for_completions(td, &comp_time);
		}

		/* ─── 错误检查 ─── */
		if (ret < 0)
			break;

		/* ==============================================================
		 * Step 5: 辅助处理 — thinktime 和 thinkcycles
		 * ==============================================================
		 */

		/* ─── thinkcycles：CPU 空转模拟计算开销 ─── */
		/*
		 * --thinkcycles=N：每次 I/O 后 CPU 空转 N 个周期
		 * cycles_spin(N) 执行 N 次空循环
		 *
		 * 与 thinktime 的区别：
		 *   - thinkcycles 不 sleep，占用 CPU
		 *   - thinktime 可以 sleep，让出 CPU
		 *
		 * 用途：模拟应用层在两次 I/O 之间的 CPU 计算时间
		 * 例如：数据库在两次 I/O 之间执行 SQL 查询
		 */
		if (ddir_rw(ddir) && td->o.thinkcycles)
			cycles_spin(td->o.thinkcycles);

		/* ─── thinktime：模拟"思考"延迟 ─── */
		/*
		 * --thinktime=N：每 thinktime_blocks 次 I/O 后 sleep N μs
		 *
		 * handle_thinktime() 的完整流程：
		 *   1. 检查是否达到触发条件（thinktime_blocks 次 I/O 或 thinktime_iotime）
		 *   2. io_u_quiesce()：等待所有在飞 I/O 完成
		 *   3. thinktime_spin μs：CPU 空转（高精度等待）
		 *   4. 剩余时间：nanosleep()（让出 CPU）
		 *   5. 速率补偿：如果 rate_ign_think=0，补偿 thinktime 期间错过的 I/O
		 *
		 * 典型用法：
		 *   --thinktime=1000 --thinktime_blocks=4
		 *   → 每 4 次 I/O 后等待 1ms
		 *   → 模拟应用每处理 4 个 I/O 后做一次计算
		 */
		if (ddir_rw(ddir) && td->o.thinktime)
			handle_thinktime(td, ddir, &comp_time);

		/* 如果没有完成的 I/O 且引擎不是 FIO_NOIO，继续循环 */
		if (!ddir_rw_sum(td->bytes_done) &&
		    !td_ioengine_flagged(td, FIO_NOIO))
			continue;

		/* ==============================================================
		 * Step 6: 速率下限检查
		 * ==============================================================
		 *
		 * --ratemin / --rate_iops_min：保证最低 I/O 速率
		 *
		 * check_min_rate() 内部（backend.c:219-229）：
		 *   遍历每个方向（read/write/trim）：
		 *     → __check_min_rate(td, now, ddir)
		 *       → 前 2 秒不检查（热身期）
		 *       → 检查间隔不低于 ratecycle
		 *       → 计算 actual_rate = (bytes - last_bytes) * 1000 / elapsed
		 *       → 如果 actual_rate < ratemin：
		 *         → log_err("rate_min=%lluB/s not met, got %lluB/s")
		 *         → 返回 true → 终止 job
		 *
		 * 用途：确保存储设备满足最低性能 SLA
		 */
		if (!in_ramp_period(td) && should_check_rate(td)) {
			if (check_min_rate(td, &comp_time)) {
				if (exitall_on_terminate || td->o.exitall_error)
					fio_terminate_threads(td->groupid, td->o.exit_what);
				td_verror(td, EIO, "check_min_rate");
				break;
			}
		}

		/* ─── 延迟目标检查 ─── */
		/*
		 * --latency_target：自适应调节 iodepth 以达到目标延迟
		 * lat_target_check() 比较实际延迟与目标延迟，
		 * 如果实际延迟 > 目标延迟，减少在飞 I/O 数
		 */
		if (!in_ramp_period(td) && td->o.latency_target)
			lat_target_check(td);
	}
	/* ==================================================================
	 * ★ 主循环结束
	 * ================================================================== */

	check_update_rusage(td);

	/* 检查是否有泄漏的 trim 条目 */
	if (td->trim_entries)
		log_err("fio: %lu trim entries leaked?\n", td->trim_entries);

	/* ==================================================================
	 * ★ 收尾工作
	 * ================================================================== */

	/* fill_device 模式：忽略 ENOSPC/EDQUOT 错误 */
	if (td->o.fill_device && (td->error == ENOSPC || td->error == EDQUOT)) {
		td->error = 0;
		fio_mark_td_terminate(td);
	}

	if (!td->error) {
		struct fio_file *f;

		/* ─── 等待所有在飞 I/O 完成 ─── */
		/*
		 * IO_MODE_OFFLOAD：刷新工作队列
		 * 普通模式：等待 cur_depth 个在飞 I/O
		 *
		 * io_u_queued_complete(td, i)
		 *   → engine->getevents(td, i, i, ...)
		 *   → 确保所有已提交的 I/O 都完成
		 */
		if (td->o.io_submit_mode == IO_MODE_OFFLOAD) {
			workqueue_flush(&td->io_wq);
			i = 0;
		} else
			i = td->cur_depth;

		if (i) {
			ret = io_u_queued_complete(td, i);
			/* fill_device 模式下忽略完成阶段的 ENOSPC */
			if (td->o.fill_device &&
			    (td->error == ENOSPC || td->error == EDQUOT))
				td->error = 0;
		}

		/* ─── 最终 fsync（--end_fsync=1） ─── */
		/*
		 * 测试结束后执行 fsync 确保数据持久化。
		 * 三种触发方式：
		 *   --end_fsync=1：对每个文件执行 fsync()
		 *   --end_syncfs=1：对整个文件系统执行 syncfs()
		 *   --fsync_on_close=1：关闭文件前 fsync
		 */
		if (should_fsync(td) &&
		    (td->o.end_fsync || td->o.end_syncfs ||
		     td->o.fsync_on_close)) {
			td_set_runstate(td, TD_FSYNCING);

			if (td->o.end_syncfs) {
				fio_syncfs(td);
			} else {
				for_each_file(td, f, i) {
					if (!fio_file_fsync(td, f))
						continue;

					log_err("fio: end_fsync failed for file %s\n",
						f->file_name);
				}
			}
		}
	}
	/* ... 错误处理（省略） ... */
}


/* ======================================================================
 * ★ wait_for_completions() — 异步引擎的收割函数
 * ======================================================================
 *
 * 源码位置：backend.c:424-450
 *
 * 参数：
 *   td   — 当前 job 的 thread_data
 *   time — 输出参数，记录完成时间（用于速率控制）
 *
 * 工作流程：
 */
static int wait_for_completions(struct thread_data *td, struct timespec *time)
{
	/* 检查队列是否已满 */
	const int full = queue_full(td);
	int min_evts = 0;
	int ret;

	/* 日志重新增长检查（罕见功能） */
	if (td->flags & TD_F_REGROW_LOGS)
		return io_u_quiesce(td);

	/*
	 * ─── 计算最少等待事件数 ───
	 *
	 * iodepth_batch_complete_min：最少一次性收割的事件数
	 *   - 队列满时：至少收割 1 个（不能卡住）
	 *   - 队列未满：按用户配置
	 *   - 如果配置为 0：至少收割 1 个
	 */
	min_evts = min(td->o.iodepth_batch_complete_min, td->cur_depth);
	if ((full && !min_evts) || !td->o.iodepth_batch_complete_min)
		min_evts = 1;

	/* 记录时间（用于速率控制） */
	if (time && should_check_rate(td))
		fio_gettime(time, NULL);

	/*
	 * ─── 收割循环 ───
	 *
	 * 核心调用链：
	 *   io_u_queued_complete(td, min_evts)
	 *     → td_io_getevents(td, min_evts, td->cur_depth)
	 *       → engine->getevents(td, min_evts, td->cur_depth, timeout)
	 *         → libaio: io_getevents(ctx, min_evts, cur_depth, &ts)
	 *         → io_uring: io_uring_wait_cqe_nr(ring, &cqe, cur_depth)
	 *
	 *   对每个完成的 io_u：
	 *     → io_u->end_io(td, io_u)
	 *       → __io_u_log_latency()：记录延迟到统计结构
	 *       → io_u_sync_complete()：更新 bw/iops/lat 统计
	 *       → verify_io_u()：验证模式下的数据校验
	 *     → put_io_u(td, io_u)：归还 io_u 到空闲池
	 *
	 * 循环条件：队列满时持续收割直到低于低水位线
	 *   cur_depth > iodepth_low → 继续收割
	 *   cur_depth <= iodepth_low → 停止，回去提交新 I/O
	 */
	do {
		ret = io_u_queued_complete(td, min_evts);
		if (ret < 0)
			break;
	} while (full && (td->cur_depth > td->o.iodepth_low));

	return ret;
}


/* ======================================================================
 * ★ io_queue_event() — 处理单次 I/O 的提交结果
 * ======================================================================
 *
 * 源码位置：backend.c:452-560
 *
 * 根据引擎返回的 ret 值处理三种情况：
 */
int io_queue_event(struct thread_data *td, struct io_u *io_u, int *ret,
		   enum fio_ddir ddir, uint64_t *bytes_issued, int from_verify,
		   struct timespec *comp_time)
{
	switch (*ret) {
	/*
	 * ─── FIO_Q_COMPLETED：同步完成 ───
	 *
	 * sync/psync 引擎的 read()/write() 阻塞返回。
	 * 此时 I/O 已经完成，直接更新统计。
	 */
	case FIO_Q_COMPLETED:
		if (io_u->error) {
			/* I/O 出错 */
			*ret = -io_u->error;
			invalidate_inflight(td, io_u);
			clear_io_u(td, io_u);
		} else if (io_u->resid) {
			/*
			 * Short I/O：实际传输的字节少于请求的字节
			 * 常见于：文件末尾、设备边界、信号中断
			 *
			 * 处理：调整 offset/buflen，重新入队
			 */
			long long bytes = io_u->xfer_buflen - io_u->resid;
			if (bytes_issued)
				*bytes_issued += bytes;
			/* ... 调整 io_u 参数 ... */
			requeue_io_u(td, &io_u);
		} else {
sync_done:
			/* 正常完成 */
			if (comp_time && should_check_rate(td))
				fio_gettime(comp_time, NULL);

			*ret = io_u_sync_complete(td, io_u);
			/*
			 * io_u_sync_complete() 内部：
			 *   1. 计算延迟 = now - io_u->issue_time
			 *   2. 更新 td->ts.slat/clat/lat 统计
			 *   3. 更新 td->ts.bw/iops 统计
			 *   4. 如果有 end_io 回调，执行它
			 *   5. put_io_u() 归还到空闲池
			 */
		}
		break;

	/*
	 * ─── FIO_Q_QUEUED：异步入队 ───
	 *
	 * libaio/io_uring 引擎将 I/O 提交到内核后返回。
	 * I/O 尚未完成，需要后续 reap。
	 */
	case FIO_Q_QUEUED:
		/* 统计更新（已提交但未完成） */
		td->io_issues[ddir]++;
		td->io_issue_bytes[ddir] += io_u->xfer_buflen;
		td->rate_io_issue_bytes[ddir] += io_u->xfer_buflen;
		if (bytes_issued)
			*bytes_issued += io_u->xfer_buflen;
		break;

	/*
	 * ─── FIO_Q_BUSY：引擎忙 ───
	 *
	 * 引擎无法接受新的 I/O（队列满或资源不足）。
	 * 将 io_u 放回队列，然后 goto reap 收割完成事件。
	 */
	case FIO_Q_BUSY:
		requeue_io_u(td, &io_u);
		/* 调用者会 goto reap */
		break;
	}

	return 0;
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
 *     │           ├── libaio: io_submit() → FIO_Q_QUEUED
 *     │           └── io_uring: io_uring_submit() → FIO_Q_QUEUED
 *     │
 *     └── io_queue_event()        处理同步完成 / 统计入队
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
 *       rate limiting 控制生产速度
 *       thinktime 模拟消费者的处理时间
 *
 *   退出条件的优先级：
 *     1. terminate/done（最高优先级：外部终止）
 *     2. runtime_exceeded（时间到了就走）
 *     3. bytes_issued >= total（数据量够了就走）
 *     4. get_io_u() == NULL（没活干了就走）
 *     5. ret < 0（出错了就走）
 */
