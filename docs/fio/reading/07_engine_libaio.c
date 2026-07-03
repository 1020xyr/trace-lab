/*
 * ======================================================================
 * Step 7: engines/libaio.c — Linux 原生异步 I/O 引擎
 * ======================================================================
 *
 * 原始路径：src/fio/engines/libaio.c (489 行)
 *
 * libaio 是 Linux 上最常用的异步 I/O 引擎。
 * 它使用内核提供的 io_submit()/io_getevents() 接口，
 * 可以同时提交多个 I/O 请求，真正实现并发 I/O。
 *
 * 与 sync 引擎的对比：
 *   sync:  queue → read() → 阻塞 → 完成 → 返回
 *   libaio: queue → 入队 → 返回 QUEUED
 *           commit → io_submit() 批量提交
 *           getevents → io_getevents() 批量收割
 *
 * 阅读要点：
 *   1. libaio_data 的环形缓冲区（head/tail 管理）
 *   2. fio_libaio_queue() — 入队（不提交，只放入环缓冲）
 *   3. fio_libaio_commit() — io_submit() 批量提交
 *   4. fio_libaio_getevents() — io_getevents() 批量收割
 *   5. 异步 I/O 的三步曲：queue → commit → getevents
 *
 * 预计阅读时间：15 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ libaio 异步 I/O 三步曲
 * ======================================================================
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │  do_io() 主循环                                              │
 *   │                                                              │
 *   │  Step 1: queue()  ─ 将 io_u 放入环形缓冲区                   │
 *   │            ld->iocbs[head] = &io_u->iocb                    │
 *   │            head++                                            │
 *   │            return FIO_Q_QUEUED （不提交，只是入队）           │
 *   │                                                              │
 *   │  Step 2: commit() ─ 调用 io_submit() 批量提交                │
 *   │            io_submit(aio_ctx, queued, iocbs)                 │
 *   │            tail += submitted                                 │
 *   │            记录 issue_time                                   │
 *   │                                                              │
 *   │  Step 3: getevents() ─ 调用 io_getevents() 收割完成事件      │
 *   │            io_getevents(aio_ctx, min, max, events)           │
 *   │            对每个事件调用 event() 获取 io_u                  │
 *   │                                                              │
 *   │  环形缓冲区状态：                                             │
 *   │    ┌────┬────┬────┬────┬────┬────┬────┬────┐                │
 *   │    │    │    │ io │ io │ io │    │    │    │                │
 *   │    └────┴────┴────┴────┴────┴────┴────┴────┘                │
 *   │              ↑              ↑                                │
 *   │            tail           head                               │
 *   │         (commit 推进)   (queue 推进)                         │
 *   │         ←─ queued ──→                                        │
 *   └──────────────────────────────────────────────────────────────┘
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <libaio.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "../fio.h"
/* ... 其他头文件省略 ... */


/* ======================================================================
 * 第一部分：libaio_data — 引擎私有数据
 * ======================================================================
 *
 * 每个使用 libaio 引擎的 job 都有一个 libaio_data 实例，
 * 存储在 td->io_ops_data 中。
 */
struct libaio_data {
	io_context_t aio_ctx;           /* ★ libaio 上下文（io_queue_init 创建） */
	struct io_event *aio_events;    /* ★ 完成事件数组（io_getevents 填充） */
	struct iocb **iocbs;            /* iocb 指针数组（提交给 io_submit） */
	struct io_u **io_us;            /* 对应的 io_u 指针数组 */

	struct io_u **io_u_index;
	struct iovec *iovecs;           /* 向量 I/O 使用 */

	/*
	 * ★ 环形缓冲区管理：
	 *
	 *   entries = iodepth（环的大小）
	 *   head    = queue() 的写入位置（入队推进）
	 *   tail    = commit() 的读取位置（提交推进）
	 *   queued  = head - tail（已入队但未提交的数量）
	 *   is_pow2 = entries 是否为 2 的幂（优化取模为 AND）
	 */
	int is_pow2;
	unsigned int entries;           /* 环大小 = iodepth */
	unsigned int queued;            /* 当前已入队未提交数 */
	unsigned int head;              /* 入队位置 */
	unsigned int tail;              /* 提交位置 */

	struct cmdprio cmdprio;         /* 优先级管理 */
};

/* 引擎私有选项 */
struct libaio_options {
	struct thread_data *td;
	unsigned int userspace_reap;    /* 用户态收割（绕过内核 io_getevents） */
	struct cmdprio_options cmdprio_options;
	unsigned int nowait;            /* RWF_NOWAIT 标志 */
	unsigned int vectored;          /* 使用 preadv/pwritev */
};


/* ======================================================================
 * ★ 核心函数 #1: fio_libaio_queue() — 入队（不提交）
 * ======================================================================
 *
 * queue() 只是将 io_u 的 iocb 放入环形缓冲区，
 * 并不调用 io_submit()。真正的提交在 commit() 中完成。
 *
 * ★ 与 sync 引擎的关键区别：
 *   sync:  queue() 直接调用 read()/write()，阻塞等待完成
 *   libaio: queue() 只是入队，立即返回 FIO_Q_QUEUED
 */
static enum fio_q_status fio_libaio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops_data;

	fio_ro_check(td, io_u);

	/* ★ 队列满 → 返回 BUSY，上层会调用 commit() 先提交 */
	if (ld->queued == td->o.iodepth)
		return FIO_Q_BUSY;

	/*
	 * TRIM 和 SYNCFS 不支持异步提交，必须同步执行。
	 * 如果队列中还有待提交的 I/O，先返回 BUSY 让上层排空。
	 */
	if (io_u->ddir == DDIR_TRIM || io_u->ddir == DDIR_SYNCFS) {
		if (ld->queued)
			return FIO_Q_BUSY;

		if (io_u->ddir == DDIR_TRIM)
			do_io_u_trim(td, io_u);
		else
			do_io_u_sync(td, io_u);
		io_u_mark_submit(td, 1);
		io_u_mark_complete(td, 1);
		return FIO_Q_COMPLETED;
	}

	/* 设置 IO 优先级（cmdprio 功能） */
	if (ld->cmdprio.mode != CMDPRIO_MODE_NONE)
		fio_libaio_cmdprio_prep(td, io_u);

	/* ★ 放入环形缓冲区 */
	ld->iocbs[ld->head] = &io_u->iocb;
	ld->io_us[ld->head] = io_u;
	ring_inc(ld, &ld->head, 1);     /* head = (head + 1) % entries */
	ld->queued++;

	return FIO_Q_QUEUED;            /* ★ 异步：返回 QUEUED */
}


/* ======================================================================
 * ★ 核心函数 #2: fio_libaio_commit() — 批量提交
 * ======================================================================
 *
 * commit() 将环形缓冲区中已入队的 iocb 批量提交给内核。
 * 使用 io_submit() 系统调用，一次可以提交多个 I/O。
 *
 * ★ 这是异步引擎的核心价值：
 *   一次 io_submit() 提交多个 I/O，内核并行处理，
 *   比逐个提交（sync 模式）的吞吐量大得多。
 */
static int fio_libaio_commit(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;
	struct iocb **iocbs;
	struct io_u **io_us;
	struct timespec ts;
	int ret, wait_start = 0;

	if (!ld->queued)
		return 0;

	do {
		long nr = ld->queued;

		/* 计算本次提交的 iocb 范围 */
		nr = min((unsigned int) nr, ld->entries - ld->tail);
		io_us = ld->io_us + ld->tail;
		iocbs = ld->iocbs + ld->tail;

		/* ★ 核心系统调用：io_submit() */
		ret = io_submit(ld->aio_ctx, nr, iocbs);

		if (ret > 0) {
			/* 成功提交了 ret 个 I/O */
			fio_libaio_queued(td, io_us, ret);   /* 记录 issue_time */
			io_u_mark_submit(td, ret);            /* 更新提交统计 */

			ld->queued -= ret;
			ring_inc(ld, &ld->tail, ret);         /* tail 推进 */
			ret = 0;
			wait_start = 0;
		} else if (ret == -EINTR || !ret) {
			/* 被信号中断或提交 0 个：重试 */
			if (!ret)
				io_u_mark_submit(td, ret);
			wait_start = 0;
			continue;
		} else if (ret == -EAGAIN) {
			/*
			 * 内核 AIO 队列满了。
			 * 如果还有已入队的 I/O，先返回让上层 reap 一些事件。
			 * 如果队列为空还 EAGAIN，最多等 30 秒。
			 */
			if (ld->queued) {
				ret = 0;
				break;
			}
			if (!wait_start) {
				fio_gettime(&ts, NULL);
				wait_start = 1;
			} else if (mtime_since_now(&ts) > 30000) {
				log_err("fio: aio appears to be stalled\n");
				break;
			}
			usleep(1);
			continue;
		} else if (ret == -ENOMEM) {
			/* iocb 分配失败：如果已有 queued 则不报错 */
			if (ld->queued)
				ret = 0;
			break;
		} else
			break;
	} while (ld->queued);

	return ret;
}


/* ======================================================================
 * ★ 核心函数 #3: fio_libaio_getevents() — 批量收割完成事件
 * ======================================================================
 *
 * getevents() 等待内核完成 I/O 并返回结果。
 * 使用 io_getevents() 系统调用，一次可以收割多个完成事件。
 *
 * ★ 参数说明：
 *   min — 至少等待 min 个事件（0 = 不阻塞，立即返回已有的）
 *   max — 最多返回 max 个事件
 *   t   — 超时时间（NULL = 无限等待）
 *
 * ★ 两种收割模式：
 *   1. 内核态（默认）：io_getevents() 系统调用
 *   2. 用户态（userspace_reap=1）：直接读 aio_ring 共享内存
 */
static int fio_libaio_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct libaio_data *ld = td->io_ops_data;
	struct libaio_options *o = td->eo;
	unsigned actual_min = td->o.iodepth_batch_complete_min == 0 ? 0 : min;
	struct timespec __lt, *lt = NULL;
	int r, events = 0;

	if (t) {
		__lt = *t;
		lt = &__lt;
	}

	do {
		if (o->userspace_reap == 1
		    && actual_min == 0
		    && ((struct aio_ring *)(ld->aio_ctx))->magic
				== AIO_RING_MAGIC) {
			/*
			 * ★ 用户态快速收割：
			 * 直接读取 aio_ring 共享内存中的完成事件。
			 * aio_ring 是内核和用户态共享的环形缓冲区，
			 * 内核在完成 I/O 后直接写入 events 数组。
			 * 用户态通过比较 head/tail 来判断是否有新事件。
			 *
			 * 优点：不需要系统调用开销
			 * 缺点：需要自己处理并发（atomic_store_release）
			 */
			r = user_io_getevents(ld->aio_ctx, max - events,
				ld->aio_events + events);
		} else {
			/*
			 * ★ 内核态收割（默认路径）：
			 * io_getevents() 阻塞等待至少 actual_min 个完成事件，
			 * 最多返回 max - events 个。
			 */
			r = io_getevents(ld->aio_ctx, actual_min,
				max - events, ld->aio_events + events, lt);
		}

		if (r > 0) {
			events += r;
			actual_min -= min((unsigned int)events, actual_min);
		} else if ((min && r == 0) || r == -EAGAIN) {
			/* 没有足够的事件：尝试提交更多 I/O，然后重试 */
			fio_libaio_commit(td);
			if (actual_min)
				usleep(10);
		} else if (r != -EINTR)
			break;
	} while (events < min);

	return r < 0 ? r : events;
}


/* ======================================================================
 * 第四部分：fio_libaio_event() — 解析单个完成事件
 * ======================================================================
 *
 * 在 getevents() 返回后，框架逐个调用 event(0), event(1), ...
 * 来获取每个完成事件对应的 io_u。
 */
static struct io_u *fio_libaio_event(struct thread_data *td, int event)
{
	struct libaio_data *ld = td->io_ops_data;
	struct io_event *ev;
	struct io_u *io_u;

	ev = ld->aio_events + event;

	/* ★ 通过 iocb 指针反查 io_u（container_of 技巧） */
	io_u = container_of(ev->obj, struct io_u, iocb);

	/* 检查传输完整性 */
	if (ev->res != io_u->xfer_buflen) {
		if (ev->res > io_u->xfer_buflen)
			io_u->error = -ev->res;          /* 内核错误码 */
		else
			io_u->resid = io_u->xfer_buflen - ev->res;  /* 部分传输 */
	} else
		io_u->error = 0;

	return io_u;
}


/* ======================================================================
 * 第五部分：辅助函数
 * ======================================================================
 */

/* 环形缓冲区递增（利用 2 的幂优化取模） */
static inline void ring_inc(struct libaio_data *ld, unsigned int *val,
			    unsigned int add)
{
	if (ld->is_pow2)
		*val = (*val + add) & (ld->entries - 1);   /* AND 代替 MOD */
	else
		*val = (*val + add) % ld->entries;
}

/* fio_libaio_prep() — 准备 iocb */
static int fio_libaio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct libaio_options *o = td->eo;
	struct fio_file *f = io_u->file;
	struct iocb *iocb = &io_u->iocb;
	struct libaio_data *ld = td->io_ops_data;

	if (io_u->ddir == DDIR_READ) {
		if (o->vectored) {
			struct iovec *iov = &ld->iovecs[io_u->index];
			iov->iov_base = io_u->xfer_buf;
			iov->iov_len = (size_t)io_u->xfer_buflen;
			io_prep_preadv(iocb, f->fd, iov, 1, io_u->offset);
		} else {
			/* ★ 准备异步读请求 */
			io_prep_pread(iocb, f->fd, io_u->xfer_buf,
				      io_u->xfer_buflen, io_u->offset);
		}
	} else if (io_u->ddir == DDIR_WRITE) {
		if (o->vectored) {
			/* ... */
		} else {
			/* ★ 准备异步写请求 */
			io_prep_pwrite(iocb, f->fd, io_u->xfer_buf,
				       io_u->xfer_buflen, io_u->offset);
		}
	} else if (ddir_sync(io_u->ddir))
		io_prep_fsync(iocb, f->fd);

	return 0;
}

/* 用户态收割：直接读 aio_ring 共享内存 */
struct aio_ring {
	unsigned id;
	unsigned nr;                    /* ring 中 events 的数量 */
	unsigned head;                  /* 用户态消费位置 */
	unsigned tail;                  /* 内核生产位置 */
	unsigned magic;                 /* AIO_RING_MAGIC = 0xa10a10a1 */
	unsigned compat_features;
	unsigned incompat_features;
	unsigned header_length;
	struct io_event events[0];      /* ★ 完成事件数组（共享内存） */
};

static int user_io_getevents(io_context_t aio_ctx, unsigned int max,
			     struct io_event *events)
{
	long i = 0;
	unsigned head;
	struct aio_ring *ring = (struct aio_ring*) aio_ctx;

	while (i < max) {
		head = ring->head;
		if (head == ring->tail)
			break;           /* 没有更多完成事件 */
		/* 从 ring 中复制事件 */
		events[i] = ring->events[head];
		atomic_store_release(&ring->head, (head + 1) % ring->nr);
		i++;
	}
	return i;
}


/* 引擎注册 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name           = "libaio",
	.version        = FIO_IOOPS_VERSION,
	.flags          = FIO_ASYNCIO_SYNC_TRIM |    /* trim 是同步的 */
			  FIO_ASYNCIO_SYNC_SYNCFS |   /* syncfs 是同步的 */
			  FIO_ASYNCIO_SETS_ISSUE_TIME | /* commit 设置 issue_time */
			  FIO_ATOMICWRITES,           /* 支持原子写 */
	.init           = fio_libaio_init,
	.post_init      = fio_libaio_post_init,       /* ★ io_queue_init() */
	.prep           = fio_libaio_prep,
	.queue          = fio_libaio_queue,
	.commit         = fio_libaio_commit,
	.getevents      = fio_libaio_getevents,
	.event          = fio_libaio_event,
	.cleanup        = fio_libaio_cleanup,
	.open_file      = generic_open_file,
	.close_file     = generic_close_file,
	.get_file_size  = generic_get_file_size,
	.options        = options,
	.option_struct_size = sizeof(struct libaio_options),
};


/*
 * ======================================================================
 * ★ 总结：libaio 引擎的异步 I/O 三步曲
 * ======================================================================
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │                     do_io() 主循环                           │
 *   │                                                              │
 *   │  ┌────────────────────────────────────────────────────────┐  │
 *   │  │  循环 iodepth 次 queue()                               │  │
 *   │  │                                                        │  │
 *   │  │  fio_libaio_queue():                                   │  │
 *   │  │    ld->iocbs[head] = &io_u->iocb   ← 放入环缓冲       │  │
 *   │  │    head++                                              │  │
 *   │  │    return FIO_Q_QUEUED             ← 不提交            │  │
 *   │  └──────────────────────────┬─────────────────────────────┘  │
 *   │                             │                                │
 *   │                             ▼                                │
 *   │  ┌────────────────────────────────────────────────────────┐  │
 *   │  │  commit() 批量提交                                     │  │
 *   │  │                                                        │  │
 *   │  │  fio_libaio_commit():                                  │  │
 *   │  │    io_submit(aio_ctx, queued, iocbs)                   │  │
 *   │  │    ← 内核开始并行处理多个 I/O                           │  │
 *   │  │    tail += submitted                                   │  │
 *   │  │    记录 issue_time（slat 的终点）                       │  │
 *   │  └──────────────────────────┬─────────────────────────────┘  │
 *   │                             │                                │
 *   │                             ▼                                │
 *   │  ┌────────────────────────────────────────────────────────┐  │
 *   │  │  getevents() 批量收割                                  │  │
 *   │  │                                                        │  │
 *   │  │  fio_libaio_getevents():                               │  │
 *   │  │    io_getevents(aio_ctx, min, max, events)             │  │
 *   │  │    ← 等待至少 min 个 I/O 完成                          │  │
 *   │  │                                                        │  │
 *   │  │  for (i = 0; i < events; i++)                          │  │
 *   │  │    io_u = fio_libaio_event(i)                          │  │
 *   │  │      ← 从 io_event 反查 io_u                          │  │
 *   │  │      ← 检查传输完整性                                  │  │
 *   │  │    io_u->end_io()  统计延迟/吞吐                        │  │
 *   │  │    put_io_u()      归还到空闲池                         │  │
 *   │  └────────────────────────────────────────────────────────┘  │
 *   └──────────────────────────────────────────────────────────────┘
 *
 *   ★ 与 sync 引擎的完整对比：
 *
 *   ┌───────────────────┬───────────────────┬───────────────────────────┐
 *   │                   │ sync 引擎          │ libaio 引擎               │
 *   ├───────────────────┼───────────────────┼───────────────────────────┤
 *   │ queue()           │ read()/write()    │ 放入环缓冲，返回 QUEUED   │
 *   │                   │ 返回 COMPLETED    │                           │
 *   ├───────────────────┼───────────────────┼───────────────────────────┤
 *   │ commit()          │ 不需要            │ io_submit() 批量提交      │
 *   ├───────────────────┼───────────────────┼───────────────────────────┤
 *   │ getevents()       │ 不需要            │ io_getevents() 批量收割   │
 *   ├───────────────────┼───────────────────┼───────────────────────────┤
 *   │ iodepth           │ 始终为 1          │ 可配置（如 32/64/128）    │
 *   ├───────────────────┼───────────────────┼───────────────────────────┤
 *   │ 并发 I/O          │ 不可能            │ 多个 I/O 同时在飞         │
 *   ├───────────────────┼───────────────────┼───────────────────────────┤
 *   │ 适用场景          │ 简单测试          │ 高性能存储测试             │
 *   │                   │ 单线程延迟测试     │ NVMe/SSD 性能评估         │
 *   └───────────────────┴───────────────────┴───────────────────────────┘
 */
