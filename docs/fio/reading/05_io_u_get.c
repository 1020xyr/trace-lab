/*
 * ======================================================================
 * Step 5: io_u.c — get_io_u() 与 offset 计算逻辑
 * ======================================================================
 *
 * 原始路径：src/fio/io_u.c
 *   - __get_io_u():     第 1757-1815 行
 *   - get_io_u():       第 1924-2022 行
 *   - fill_io_u():      第 1078-1140 行
 *   - get_next_offset(): 第 553-600 行
 *   - get_next_block():  第 477-546 行
 *   - get_next_seq_offset(): 第 401-475 行
 *
 * get_io_u() 是 do_io() 每次循环的第一步——获取一个"装备齐全"的 io_u。
 * 所谓"装备齐全"，是指 io_u 已经知道了：
 *   - 要访问哪个文件（file）
 *   - 要访问文件的哪个位置（offset）
 *   - 要传输多少数据（buflen）
 *   - 是读还是写（ddir）
 *
 * 其中 offset 的计算是最复杂的部分——它决定了 I/O 是顺序还是随机。
 *
 * 阅读要点：
 *   1. __get_io_u() — 从空闲池弹出 io_u（纯分配，不填充内容）
 *   2. get_io_u() — 完整流程：分配 + 填充
 *   3. 顺序 I/O 的 offset：last_pos + ddir_seq_add
 *   4. 随机 I/O 的 offset：随机数生成 + randommap
 *   5. get_next_block() 中顺序/随机的分支逻辑
 *
 * 预计阅读时间：15 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ offset 计算的决策树
 * ======================================================================
 *
 *   get_io_u()
 *     │
 *     ├── __get_io_u()           ← 从 freelist/requeue 弹出空闲 io_u
 *     │
 *     ├── check_get_verify()     ← 是否有 backlog 验证任务？
 *     ├── check_get_trim()       ← 是否有 backlog trim 任务？
 *     │
 *     ├── set_io_u_file()        ← ★ 选择目标文件 + 填充 offset/buflen/ddir
 *     │     │
 *     │     └── fill_io_u()      ← 真正填充 io_u 的内容
 *     │           │
 *     │           ├── set_rw_ddir()        ← 决定读还是写
 *     │           │
 *     │           ├── get_next_offset()    ← ★ 计算 offset
 *     │           │     │
 *     │           │     └── get_next_block()
 *     │           │           │
 *     │           │           ├── 顺序 I/O:
 *     │           │           │   get_next_seq_offset()
 *     │           │           │     offset = f->last_pos[ddir] - f->file_offset
 *     │           │           │     然后加上 ddir_seq_add（步进值）
 *     │           │           │
 *     │           │           └── 随机 I/O:
 *     │           │               get_next_rand_block()
 *     │           │                 生成随机块号 b
 *     │           │                 offset = b * td->o.ba[ddir]
 *     │           │                 (ba = block align，块对齐大小)
 *     │           │
 *     │           ├── get_next_buflen()    ← 计算传输大小
 *     │           │
 *     │           └── mark_random_map()    ← randommap 标记已用块
 *     │
 *     └── td_io_prep()           ← 调用引擎的 prep()
 */


/* ======================================================================
 * ★ 核心函数 #1: __get_io_u() — 从空闲池获取 io_u
 * ======================================================================
 *
 * 这是最底层的 io_u 分配函数。它只负责"弹出一个空闲 io_u"，
 * 不填充任何 offset/buflen/ddir 信息。
 *
 * io_u 的来源有两个（按优先级）：
 *   1. requeue 列表 — 之前因为 BUSY 而被退回的 io_u（优先重试）
 *   2. freelist — 全新的空闲 io_u
 */
struct io_u *__get_io_u(struct thread_data *td)
{
	const bool needs_lock = td_async_processing(td);
	struct io_u *io_u = NULL;

	if (td->stop_io)
		return NULL;

	if (needs_lock)
		__td_io_u_lock(td);

again:
	/* 优先从 requeue 列表取（之前需要重试的 IO） */
	if (!io_u_rempty(&td->io_u_requeues)) {
		io_u = io_u_rpop(&td->io_u_requeues);
		io_u->resid = 0;
		/* fsync 期间，requeue 的 io_u 可能指向已关闭的文件 */
		if (io_u->file && td->runstate == TD_FSYNCING) {
			put_file_log(td, io_u->file);
			io_u->file = NULL;
		}
	/* 否则从 freelist 取新的 */
	} else if (!queue_full(td)) {
		io_u = io_u_qpop(&td->io_u_freelist);

		io_u->file = NULL;
		io_u->buflen = 0;
		io_u->resid = 0;
		io_u->end_io = NULL;
	}

	if (io_u) {
		/* 清除状态标志，准备新一轮使用 */
		assert(io_u->flags & IO_U_F_FREE);
		io_u_clear(td, io_u, IO_U_F_FREE | IO_U_F_NO_FILE_PUT |
				     IO_U_F_TRIMMED | IO_U_F_BARRIER |
				     IO_U_F_VER_LIST);

		io_u->error = 0;
		io_u->acct_ddir = -1;
		td->cur_depth++;           /* ★ 当前队列深度 +1 */
		assert(!(td->flags & TD_F_CHILD));
		io_u_set(td, io_u, IO_U_F_IN_CUR_DEPTH);
		io_u->ipo = NULL;
	} else if (td_async_processing(td)) {
		/*
		 * 异步模式且无空闲 io_u：等待验证线程归还
		 * （使用 pthread_cond_wait 阻塞等待）
		 */
		int ret;
		assert(!(td->flags & TD_F_CHILD));
		ret = pthread_cond_wait(&td->free_cond, &td->io_u_lock);
		if (fio_unlikely(ret != 0)) {
			td->error = errno;
		} else if (!td->error)
			goto again;        /* 被唤醒后重试 */
	}

	if (needs_lock)
		__td_io_u_unlock(td);

	return io_u;
}


/* ======================================================================
 * ★ 核心函数 #2: fill_io_u() — 填充 io_u 的 ddir/offset/buflen
 * ======================================================================
 *
 * fill_io_u() 是 io_u 内容的真正填充者。
 * 它由 set_io_u_file() 调用，在文件选定后填充：
 *   1. ddir — 读/写/trim/sync
 *   2. offset — 文件内偏移
 *   3. buflen — 传输大小
 */
static int fill_io_u(struct thread_data *td, struct io_u *io_u)
{
	bool is_random;
	uint64_t offset;
	enum io_u_action ret;

	if (td_ioengine_flagged(td, FIO_NOIO))
		goto out;

	/* ① 确定 I/O 方向（读/写/trim/sync） */
	set_rw_ddir(td, io_u);

	if (io_u->ddir == DDIR_INVAL || io_u->ddir == DDIR_TIMEOUT) {
		dprint(FD_IO, "invalid direction received ddir = %d", io_u->ddir);
		return 1;
	}

	/* fsync/fdatasync 等不需要 offset/buflen */
	if (!ddir_rw(io_u->ddir))
		goto out;

	/* ZBD（Zoned Block Device）特殊处理 */
	if (td->o.zone_mode == ZONE_MODE_STRIDED)
		setup_strided_zone_mode(td, io_u);
	else if (td->o.zone_mode == ZONE_MODE_ZBD)
		setup_zbd_zone_mode(td, io_u);

	/* ② 计算 offset 和 buflen */
	if (multi_range_trim(td, io_u)) {
		if (fill_multi_range_io_u(td, io_u))
			return 1;
	} else {
		/*
		 * ★ 正常路径：
		 *   get_next_offset() — 计算 offset
		 *   get_next_buflen() — 计算 buflen
		 */
		if (get_next_offset(td, io_u, &is_random)) {
			dprint(FD_IO, "io_u %p, failed getting offset\n", io_u);
			return 1;
		}

		io_u->buflen = get_next_buflen(td, io_u, is_random);
		if (!io_u->buflen) {
			dprint(FD_IO, "io_u %p, failed getting buflen\n", io_u);
			return 1;
		}
	}
	offset = io_u->offset;

	/* ... ZBD 调整、文件边界检查等省略 ... */

out:
	/* 记录随机种子（验证用） */
	io_u->rand_seed = __rand(&td->random_state);
	/* ... */
	return 0;
}


/* ======================================================================
 * ★ 核心函数 #3: get_next_offset() — offset 计算入口
 * ======================================================================
 *
 * 这是 offset 计算的总入口。它通过 get_next_block() 获取
 * 原始偏移，然后加上文件起始偏移（file_offset），做边界检查。
 */
static int get_next_offset(struct thread_data *td, struct io_u *io_u,
			   bool *is_random)
{
	struct fio_file *f = io_u->file;
	enum fio_ddir ddir = io_u->ddir;
	int rw_seq_hit = 0;

	assert(ddir_rw(ddir));

	/*
	 * ddir_seq_nr：每 N 个 IO 后切换一次"顺序/随机"模式
	 * 用于 --rw=randrw:10 这样的混合模式
	 */
	if (td->o.ddir_seq_nr && !--td->ddir_seq_nr) {
		rw_seq_hit = 1;
		td->ddir_seq_nr = td->o.ddir_seq_nr;
	}

	/* ★ 核心：获取下一个块位置 */
	if (get_next_block(td, io_u, ddir, rw_seq_hit, is_random))
		return 1;

	/* 边界检查：offset 不能超过 io_size */
	if (io_u->offset >= f->io_size) {
		dprint(FD_IO, "get_next_offset: offset %llu >= io_size %llu\n",
					(unsigned long long) io_u->offset,
					(unsigned long long) f->io_size);
		return 1;
	}

	/* ★ 加上文件的起始偏移（file 可能不是从 0 开始的） */
	io_u->offset += f->file_offset;

	/* 边界检查：offset 不能超过文件大小 */
	if (io_u->offset >= f->real_file_size) {
		dprint(FD_IO, "get_next_offset: offset %llu >= size %llu\n",
					(unsigned long long) io_u->offset,
					(unsigned long long) f->real_file_size);
		return 1;
	}

	return 0;
}


/* ======================================================================
 * ★ 核心函数 #4: get_next_block() — 顺序 vs 随机的分支
 * ======================================================================
 *
 * 这是 offset 计算的核心决策点：
 *   - 顺序 I/O → get_next_seq_offset()
 *   - 随机 I/O → get_next_rand_block()
 *   - 混合模式 → 按概率选择
 */
static int get_next_block(struct thread_data *td, struct io_u *io_u,
			  enum fio_ddir ddir, int rw_seq,
			  bool *is_random)
{
	struct fio_file *f = io_u->file;
	uint64_t b, offset;
	int ret;

	assert(ddir_rw(ddir));

	b = offset = -1ULL;

	if (td_randtrimwrite(td) && ddir == DDIR_WRITE) {
		/* trimwrite 模式：写操作跟随 trim 的 offset */
		io_u_set(td, io_u, IO_U_F_BUSY_OK);
		offset = f->last_start[DDIR_TRIM] - f->file_offset;
		*is_random = true;
		ret = 0;
	} else if (rw_seq) {
		/* ★ 顺序命中：优先尝试顺序 offset */
		if (td_random(td)) {
			/*
			 * 混合模式（如 --rw=randrw）：
			 * 按 --rwmixread 百分比决定是随机还是顺序
			 */
			if (should_do_random(td, ddir)) {
				ret = get_next_rand_block(td, f, ddir, &b);
				*is_random = true;
			} else {
				*is_random = false;
				io_u_set(td, io_u, IO_U_F_BUSY_OK);
				ret = get_next_seq_offset(td, f, ddir, &offset);
				if (ret)
					ret = get_next_rand_block(td, f, ddir, &b);
			}
		} else {
			/* ★ 纯顺序模式 */
			*is_random = false;
			ret = get_next_seq_offset(td, f, ddir, &offset);
		}
	} else {
		/* 非顺序序列（rw_seq 未命中） */
		io_u_set(td, io_u, IO_U_F_BUSY_OK);
		*is_random = false;

		if (td->o.rw_seq == RW_SEQ_SEQ) {
			ret = get_next_seq_offset(td, f, ddir, &offset);
			if (ret) {
				ret = get_next_rand_block(td, f, ddir, &b);
				*is_random = false;
			}
		} else if (td->o.rw_seq == RW_SEQ_IDENT) {
			/* IDENT 模式：每个方向使用相同的 offset */
			if (f->last_start[ddir] != -1ULL)
				offset = f->last_start[ddir] - f->file_offset;
			else
				offset = 0;
			ret = 0;
		} else {
			log_err("fio: unknown rw_seq=%d\n", td->o.rw_seq);
			ret = 1;
		}
	}

	if (!ret) {
		/* ★ 最终赋值：offset 或 块号 × 对齐大小 */
		if (offset != -1ULL)
			io_u->offset = offset;            /* 顺序：直接偏移 */
		else if (b != -1ULL)
			io_u->offset = b * td->o.ba[ddir]; /* 随机：块号 × ba */
		else {
			log_err("fio: bug in offset generation\n");
			ret = 1;
		}
		io_u->verify_offset = io_u->offset;
	}

	return ret;
}


/* ======================================================================
 * ★ 核心函数 #5: get_next_seq_offset() — 顺序 I/O 的 offset 计算
 * ======================================================================
 *
 * 顺序 I/O 的逻辑很简单：
 *   offset = 上一次 I/O 结束的位置（last_pos）
 *
 * 如果到达文件末尾：
 *   - time_based 模式：回绕到文件开头
 *   - 否则：返回 1（表示已到达末尾）
 */
static int get_next_seq_offset(struct thread_data *td, struct fio_file *f,
			       enum fio_ddir ddir, uint64_t *offset)
{
	struct thread_options *o = &td->o;

	assert(ddir_rw(ddir));

	/* time_based 模式：到达末尾后回绕 */
	if (f->last_pos[ddir] >= f->io_size + get_start_offset(td, f) &&
	    o->time_based && o->nr_files == 1) {
		f->last_pos[ddir] = f->file_offset;
		loop_cache_invalidate(td, f);
	}

	/* io_size > size 的顺序写：也允许回绕 */
	if (td_write(td) && o->io_size > o->size) {
		if (f->last_pos[ddir] >= f->io_size + get_start_offset(td, f)) {
			f->last_pos[ddir] = f->file_offset;
			loop_cache_invalidate(td, f);
		}
	}

	if (f->last_pos[ddir] < f->real_file_size) {
		uint64_t pos;

		/* 反向 I/O（ddir_seq_add < 0）的回绕处理 */
		if (f->last_pos[ddir] == f->file_offset &&
		    f->file_offset && o->ddir_seq_add < 0) {
			if (f->real_file_size > f->io_size)
				f->last_pos[ddir] = f->io_size;
			else
				f->last_pos[ddir] = f->real_file_size;
		}

		/* ★ 核心：offset = last_pos - file_offset */
		pos = f->last_pos[ddir] - f->file_offset;

		/* ddir_seq_add：步进值（默认 0，用于跳过某些区域） */
		if (pos && o->ddir_seq_add) {
			pos += o->ddir_seq_add;

			/* 超出文件范围则回绕 */
			if (pos >= f->real_file_size) {
				if (o->ddir_seq_add > 0)
					pos = f->file_offset;
				else {
					if (f->real_file_size > f->io_size)
						pos = f->io_size;
					else
						pos = f->real_file_size;
					pos += o->ddir_seq_add;
				}
			}
		}

		*offset = pos;
		return 0;
	}

	return 1;  /* 到达文件末尾 */
}


/* ======================================================================
 * ★ 核心函数 #6: get_io_u() — 完整的 io_u 获取流程
 * ======================================================================
 *
 * 这是 do_io() 调用的最外层接口，整合了上述所有子函数。
 */
struct io_u *get_io_u(struct thread_data *td)
{
	struct fio_file *f;
	struct io_u *io_u;
	int do_scramble = 0;
	long ret = 0;

	/* ① 从空闲池获取 io_u */
	io_u = __get_io_u(td);
	if (!io_u) {
		dprint(FD_IO, "__get_io_u failed\n");
		return NULL;
	}

	/* ② 检查 backlog 验证/trim 任务 */
	if (check_get_verify(td, io_u))
		goto out;
	if (check_get_trim(td, io_u))
		goto out;

	/* ③ requeue 的 io_u 已经有 file，跳过填充 */
	if (io_u->file)
		goto out;

	/* ④ 选择文件 + 填充 offset/buflen/ddir */
	if (td->flags & TD_F_READ_IOLOG) {
		/* iolog 回放模式：从日志中读取 offset/ddir */
		if (read_iolog_get(td, io_u))
			goto err_put;
	} else if (set_io_u_file(td, io_u)) {
		/*
		 * ★ 正常路径：
		 *   set_io_u_file()
		 *     → get_next_file()  选择目标文件
		 *     → fill_io_u()      填充 ddir/offset/buflen
		 */
		ret = -EBUSY;
		dprint(FD_IO, "io_u %p, setting file failed\n", io_u);
		goto err_put;
	}

	f = io_u->file;
	if (!f) {
		dprint(FD_IO, "io_u %p, setting file failed\n", io_u);
		goto err_put;
	}

	assert(fio_file_open(f));

	/* ⑤ 更新文件的 last_start / last_pos */
	if (ddir_rw(io_u->ddir) && !multi_range_trim(td, io_u)) {
		f->last_start[io_u->ddir] = io_u->offset;
		f->last_pos[io_u->ddir] = io_u->offset + io_u->buflen;

		/* ⑥ 写操作：填充缓冲区内容 */
		if (io_u->ddir == DDIR_WRITE) {
			if (td->flags & TD_F_REFILL_BUFFERS) {
				io_u_fill_buffer(td, io_u,
					td->o.min_bs[DDIR_WRITE],
					io_u->buflen);
			} else if ((td->flags & TD_F_SCRAMBLE_BUFFERS) &&
				   !(td->flags & TD_F_COMPRESS) &&
				   !(td->flags & TD_F_DO_VERIFY)) {
				do_scramble = 1;  /* 延迟混淆（减少延迟） */
			}
		} else if (io_u->ddir == DDIR_READ) {
			io_u->buf_filled_len = 0;
		}
	}

	/* ⑦ 设置传输指针 */
	io_u->xfer_buf = io_u->buf;
	io_u->xfer_buflen = io_u->buflen;
	io_u->ioprio = td->ioprio;
	io_u->clat_prio_index = 0;

out:
	assert(io_u->file);
	/* ⑧ 调用引擎的 prep() */
	if (!td_io_prep(td, io_u)) {
		/* ⑨ 记录 start_time（用于计算 slat） */
		if (!td->o.disable_lat)
			fio_gettime(&io_u->start_time, NULL);

		if (do_scramble)
			small_content_scramble(io_u);

		return io_u;
	}
err_put:
	dprint(FD_IO, "get_io_u failed\n");
	put_io_u(td, io_u);
	return ERR_PTR(ret);
}


/*
 * ======================================================================
 * ★ 总结：顺序 I/O vs 随机 I/O 的 offset 计算
 * ======================================================================
 *
 *   ┌────────────────────────────────────────────────────────────────────┐
 *   │                    顺序 I/O (rw=read/write)                       │
 *   │                                                                    │
 *   │   IO #1: offset = 0                    buflen = 4096              │
 *   │   IO #2: offset = 4096                 buflen = 4096              │
 *   │   IO #3: offset = 8192                 buflen = 4096              │
 *   │   ...                                                              │
 *   │   IO #N: offset = (N-1) * 4096         buflen = 4096              │
 *   │                                                                    │
 *   │   实现：get_next_seq_offset()                                      │
 *   │     offset = f->last_pos[ddir] - f->file_offset                   │
 *   │     然后 last_pos += buflen（下一次自动递增）                       │
 *   └────────────────────────────────────────────────────────────────────┘
 *
 *   ┌────────────────────────────────────────────────────────────────────┐
 *   │                    随机 I/O (rw=randread/randwrite)                │
 *   │                                                                    │
 *   │   IO #1: offset = 0x3a000              buflen = 4096              │
 *   │   IO #2: offset = 0x1c000              buflen = 4096              │
 *   │   IO #3: offset = 0x7e000              buflen = 4096              │
 *   │   ...                                                              │
 *   │   IO #N: offset = random() * ba[ddir]  buflen = 4096              │
 *   │                                                                    │
 *   │   实现：get_next_rand_block()                                      │
 *   │     b = 随机数生成器产生的块号                                      │
 *   │     offset = b * td->o.ba[ddir]                                   │
 *   │     然后 mark_random_map() 标记已使用（防重复）                     │
 *   │     (--norandommap 时不标记，允许重复访问)                          │
 *   └────────────────────────────────────────────────────────────────────┘
 *
 *   调用链：
 *
 *     do_io()
 *       → get_io_u()
 *         → set_io_u_file()
 *           → fill_io_u()
 *             → get_next_offset()
 *               → get_next_block()
 *                 ├── get_next_seq_offset()   ← 顺序
 *                 └── get_next_rand_block()   ← 随机
 *             → get_next_buflen()
 */
