/*
 * ======================================================================
 * Step 4: 用户态 blktrace.c — 采集工具的核心逻辑
 * ======================================================================
 *
 * 原始路径：src/blktrace/blktrace.c (2715 行)
 *
 * 本文件是 blktrace 命令行工具的实现。我们关注：
 *   1. 常量和数据结构
 *   2. setup_buts() — 通过 ioctl 配置内核追踪
 *   3. start_buts() — 启动追踪
 *   4. thread_main() — 每个 CPU 一个线程，从 relay buffer 读取数据
 *   5. handle_pfds_entries() — 读取事件并写入磁盘
 *   6. main() — 主函数流程
 *
 * 预计阅读时间：15 分钟
 * ======================================================================
 */


/* ======================================================================
 * 第一部分：关键常量
 * ====================================================================== */

/*
 * relay buffer 的默认参数
 *
 * BUF_SIZE: 每个 subbuffer 的大小 = 512KB
 * BUF_NR:   每个 CPU 的 subbuffer 数量 = 4
 *
 * 每个 CPU 的总缓冲 = 512KB × 4 = 2MB
 * 8 核系统总缓冲 = 2MB × 8 = 16MB
 *
 * 如果 I/O 量很大导致 relay buffer 溢出，blktrace 会报告 "dropped" 事件。
 * 此时可通过 -b (buf_size) 和 -n (buf_nr) 参数增大缓冲。
 */
#define BUF_SIZE    (512 * 1024)    /* 512KB per subbuffer */
#define BUF_NR      (4)             /* 4 subbuffers per CPU */

/* 输出文件的写缓冲大小 */
#define FILE_VBUF_SIZE  (128 * 1024)

/* debugfs 的 magic number（用于验证挂载） */
#define DEBUGFS_TYPE    (0x64626720)

/* 网络模式的默认端口 */
#define TRACE_NET_PORT  (8462)


/* ======================================================================
 * 第二部分：全局变量
 * ====================================================================== */

static unsigned long buf_size = BUF_SIZE;   /* 可通过 -b 参数修改 */
static unsigned long buf_nr = BUF_NR;       /* 可通过 -n 参数修改 */
static unsigned short act_mask = 0;         /* 事件过滤掩码，可通过 -a 参数设置 */
                                             /* 0 = 追踪所有事件 */

/*
 * handle_pfds 是一个函数指针，根据输出模式选择不同的读取策略：
 *   - handle_pfds_file()     — 写入本地文件（默认模式）
 *   - handle_pfds_netclient() — 发送到远程客户端
 *   - handle_pfds_entries()  — 直接处理（管道输出等）
 */
static int (*handle_pfds)(struct tracer *, int, int);


/* ======================================================================
 * 第三部分：★ setup_buts() — 配置内核追踪
 * ======================================================================
 *
 * 对每个要追踪的设备，通过 ioctl 调用 BLKTRACESETUP。
 * 这会在内核中：
 *   1. 分配 blk_trace 结构
 *   2. 创建 relay channel（per-CPU buffer）
 *   3. 创建 debugfs 文件
 */
static void setup_buts(void)
{
	struct list_head *p;

	/* 遍历所有要追踪的设备 */
	__list_for_each(p, &devpaths) {
		struct blk_user_trace_setup buts;
		struct devpath *dpp = list_entry(p, struct devpath, head);

		memset(&buts, 0, sizeof(buts));

		/* ★ 填充用户态配置结构 */
		buts.buf_size = buf_size;   /* relay subbuffer 大小（默认 512KB） */
		buts.buf_nr = buf_nr;       /* relay subbuffer 数量（默认 4） */
		buts.act_mask = act_mask;   /* 事件过滤掩码 */
		                             /* 对应命令行 -a 参数 */
		                             /* 例如 -a issue -a complete = 只追踪 D 和 C */

		/* ★ 通过 ioctl 将配置传给内核 */
		if (ioctl(dpp->fd, BLKTRACESETUP, &buts) >= 0) {
			dpp->ncpus = ncpus;
			dpp->buts_name = strdup(buts.name);
			/* 分配 per-CPU 统计信息 */
			dpp->stats = calloc(dpp->ncpus, sizeof(*dpp->stats));
		} else
			fprintf(stderr, "BLKTRACESETUP(2) %s failed: %d/%s\n",
				dpp->path, errno, strerror(errno));
	}
}


/* ======================================================================
 * 第四部分：★ start_buts() — 启动追踪
 * ======================================================================
 */
static void start_buts(void)
{
	struct list_head *p;

	__list_for_each(p, &devpaths) {
		struct devpath *dpp = list_entry(p, struct devpath, head);

		/* ★ 通过 ioctl 启动追踪 */
		if (ioctl(dpp->fd, BLKTRACESTART) < 0) {
			fprintf(stderr, "BLKTRACESTART %s failed: %d/%s\n",
				dpp->path, errno, strerror(errno));
		}
	}
}


/* ======================================================================
 * 第五部分：★ handle_pfds_entries() — 从 relay buffer 读取事件
 * ======================================================================
 *
 * 每个 CPU 有一个线程，循环调用 poll() + read() 从 relay buffer 读取数据。
 *
 * relay buffer 在 debugfs 中暴露为：
 *   /sys/kernel/debug/block/<dev>/trace0  (CPU 0)
 *   /sys/kernel/debug/block/<dev>/trace1  (CPU 1)
 *   ...
 *
 * 这些文件可以 mmap() 或 read()。blktrace 默认使用 read()。
 */
static int handle_pfds_entries(struct tracer *tp, int nevs, int force_read)
{
	int i, nentries = 0;
	struct trace_buf *tbp;
	struct pollfd *pfd = tp->pfds;       /* poll 文件描述符数组 */
	struct io_info *iop = tp->ios;       /* I/O 信息（fd、文件名等） */

	tbp = alloc_trace_buf(tp->cpu, buf_size);

	/* 遍历所有设备的 relay 文件 */
	for (i = 0; i < ndevs; i++, pfd++, iop++) {
		/* 有数据可读，或者强制读取（停止后的排空阶段） */
		if (pfd->revents & POLLIN || force_read) {

			/* ★ 从 relay buffer 读取数据 */
			tbp->len = read(iop->ifd, tbp->buf, buf_size);

			if (tbp->len > 0) {
				/* 更新统计信息 */
				pdc_dr_update(iop->dpp, tp->cpu, tbp->len);
				/* 将数据添加到输出队列 */
				add_trace_buf(iop->dpp, tp->cpu, &tbp);
				nentries++;
			} else if (tbp->len == 0) {
				/* 短读表示追踪已停止，不再等待 */
				if (tp->is_done)
					clear_events(pfd);
			} else {
				read_err(tp->cpu, iop->ifn);
				if (errno != EAGAIN || tp->is_done)
					clear_events(pfd);
			}
		}
	}
	free(tbp);

	if (nentries)
		incr_entries(nentries);

	return nentries;
}


/* ======================================================================
 * 第六部分：★ thread_main() — 每个 CPU 的工作线程
 * ======================================================================
 *
 * blktrace 为每个 CPU 启动一个线程，绑定到该 CPU 上运行。
 * 这样做的好处：
 *   1. 减少跨 CPU 的缓存失效
 *   2. 每个线程只读自己 CPU 的 relay buffer
 *   3. 避免锁竞争
 */
static void *thread_main(void *arg)
{
	int ret, ndone, to_val;
	struct tracer *tp = arg;

	/* 绑定到指定 CPU */
	ret = lock_on_cpu(tp->cpu);
	if (ret)
		goto err;

	/* 打开所有设备的 relay 文件 */
	ret = open_ios(tp);
	if (ret)
		goto err;

	/* poll 超时时间 */
	if (piped_output)
		to_val = 50;     /* 管道输出：频繁处理（50ms） */
	else
		to_val = 500;    /* 文件输出：半秒间隔（500ms） */

	tracer_signal_ready(tp, Th_running, 0);
	tracer_wait_unblock(tp);

	/* ★ 主循环：poll → read → write */
	while (!tp->is_done) {
		/* poll 等待 relay 文件可读 */
		ndone = poll(tp->pfds, ndevs, to_val);
		if (ndone || piped_output)
			/* 有数据可读：处理事件 */
			(void)handle_pfds(tp, ndone, piped_output);
		else if (ndone < 0 && errno != EINTR)
			fprintf(stderr, "Thread %d poll failed: %d/%s\n",
				tp->cpu, errno, strerror(errno));
	}

	/* 追踪停止后，排空剩余数据 */
	while (handle_pfds(tp, ndevs, 1) > 0)
		;

	close_ios(tp);
	tracer_signal_ready(tp, Th_leaving, 0);
	return NULL;
err:
	/* ... 错误处理 ... */
}


/* ======================================================================
 * 第七部分：★ main() — 主函数
 * ====================================================================== */
int main(int argc, char *argv[])
{
	int ret = 0;

	setlocale(LC_NUMERIC, "en_US");
	pagesize = getpagesize();

	/* 获取 CPU 数量（决定启动多少个线程） */
	ncpus = sysconf(_SC_NPROCESSORS_CONF);
	if (ncpus < 0) {
		fprintf(stderr, "sysconf failed %d/%s\n", errno, strerror(errno));
		ret = 1;
		goto out;
	}

	/* ★ 解析命令行参数 */
	if (handle_args(argc, argv)) {
		ret = 1;
		goto out;
	}

	/*
	 * 注册信号处理器：
	 *   SIGINT  (Ctrl+C) → 停止追踪
	 *   SIGHUP           → 停止追踪
	 *   SIGTERM          → 停止追踪
	 *   SIGALRM          → 定时停止（-w 参数）
	 */
	signal(SIGINT, handle_sigint);
	signal(SIGHUP, handle_sigint);
	signal(SIGTERM, handle_sigint);
	signal(SIGALRM, handle_sigint);
	signal(SIGPIPE, SIG_IGN);

	if (kill_running_trace) {
		/* --kill 模式：停止已有的追踪 */
		struct devpath *dpp;
		struct list_head *p;
		__list_for_each(p, &devpaths) {
			dpp = list_entry(p, struct devpath, head);
			__stop_trace(dpp->fd);    /* ioctl(BLKTRACETEARDOWN) */
		}
	} else if (net_mode == Net_server) {
		/* 网络服务端模式 */
		ret = net_server();
	} else {
		/*
		 * ★ 默认模式：启动追踪
		 *
		 * run_tracers() 内部流程：
		 *   1. setup_buts()     — ioctl(BLKTRACESETUP)
		 *   2. start_buts()     — ioctl(BLKTRACESTART)
		 *   3. 设置 SIGALRM    — 如果指定了 -w 超时
		 *   4. 启动 per-CPU 线程 — 每个线程执行 thread_main()
		 *   5. 等待所有线程结束 — 直到信号或超时
		 *   6. 停止追踪        — ioctl(BLKTRACESTOP)
		 *   7. 清理            — ioctl(BLKTRACETEARDOWN)
		 */
		ret = run_tracers();
	}

out:
	if (pfp) fclose(pfp);
	rel_devpaths();
	return ret;
}


/*
 * ★ 总结：blktrace 用户态工具的执行流程
 *
 *   blktrace -d /dev/sda -o trace -w 10
 *
 *   1. main() → handle_args()
 *      解析参数：device=/dev/sda, output=trace, timeout=10s
 *
 *   2. run_tracers()
 *      a. setup_buts()
 *         ioctl(fd, BLKTRACESETUP, &buts)
 *         → 内核创建 relay channel + debugfs 文件
 *
 *      b. start_buts()
 *         ioctl(fd, BLKTRACESTART)
 *         → 内核开始记录 tracepoint 事件
 *
 *      c. alarm(10)  — 设置 10 秒定时器
 *
 *      d. 启动 ncpus 个线程
 *         每个线程绑定到一个 CPU
 *         打开 /sys/kernel/debug/block/sda/traceN
 *         循环：poll() → read() → write() 到 trace.blktrace.N
 *
 *      e. SIGALRM 触发 → handle_sigint() → 设置 is_done = 1
 *
 *      f. 线程退出循环，排空剩余数据
 *
 *      g. ioctl(fd, BLKTRACESTOP) — 停止内核追踪
 *         ioctl(fd, BLKTRACETEARDOWN) — 清理 relay buffer
 *
 *   3. 输出文件：
 *      trace.blktrace.0  (CPU 0 的事件)
 *      trace.blktrace.1  (CPU 1 的事件)
 *      ...
 *      trace.blktrace.N  (CPU N 的事件)
 */
