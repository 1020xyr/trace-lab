/*
 * ======================================================================
 * Step 6: engines/sync.c — 最简单的 I/O 引擎
 * ======================================================================
 *
 * 原始路径：src/fio/engines/sync.c (519 行)
 *
 * sync.c 是 fio 中最简单、最"原始"的 I/O 引擎。
 * 它直接调用 read()/write()/pread()/pwrite() 系统调用，
 * 每次 queue() 都阻塞等待 I/O 完成，然后返回 FIO_Q_COMPLETED。
 *
 * 为什么先学 sync 引擎？
 *   1. 代码最短（~400 行），容易理解 queue() 的含义
 *   2. 它是理解异步引擎（libaio/io_uring）的基础
 *   3. 它注册了 5 个引擎变体，展示了引擎家族的注册模式
 *
 * 阅读要点：
 *   1. fio_syncio_queue() — 最基本的 queue()：read()/write()
 *   2. fio_psyncio_queue() — psync 变体：pread()/pwrite()
 *   3. fio_syncio_prep() — lseek 优化（避免不必要的 seek）
 *   4. 引擎注册：5 个 ioengine_ops 结构体
 *   5. sync vs psync 的区别：lseek+read vs pread
 *
 * 预计阅读时间：10 分钟
 * ======================================================================
 */


/* ======================================================================
 * sync 引擎家族一览
 * ======================================================================
 *
 * sync.c 注册了 5 个引擎，每个使用不同的系统调用：
 *
 *   引擎名   │ 系统调用            │ 特点
 *   ─────────┼────────────────────┼────────────────────────────────
 *   sync     │ read()/write()     │ 需要 lseek 定位，最简单的引擎
 *   psync    │ pread()/pwrite()   │ 不需要 lseek，原子定位+读写
 *   vsync    │ readv()/writev()   │ 批量 I/O（scatter-gather）
 *   pvsync   │ preadv()/pwritev() │ 批量 + 原子定位
 *   pvsync2  │ preadv2/pwritev2   │ 最新接口，支持 RWF_HIPRI 等
 *
 * ★ 对于性能测试，psync 比 sync 更好：
 *   sync  需要两次系统调用（lseek + read/write）
 *   psync 只需要一次系统调用（pread/pwrite）
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/uio.h>
#include <errno.h>

#include "../fio.h"
#include "../optgroup.h"
#include "../lib/rand.h"

/* sync 引擎用 engine_data 存储上次 I/O 的结束位置 */
#define LAST_POS(f)  ((f)->engine_pos)

/* sync 引擎的私有数据 */
struct syncio_data {
	struct iovec *iovecs;           /* vsync 模式的 scatter-gather 数组 */
	struct io_u **io_us;            /* vsync 模式的 io_u 数组 */
	unsigned int queued;            /* 当前已 queue 但未 commit 的数量 */
	unsigned int events;            /* 上次 commit 后的完成事件数 */
	unsigned long queued_bytes;     /* 已 queue 的字节数 */

	unsigned long long last_offset; /* 上次 I/O 的结束偏移 */
	struct fio_file *last_file;     /* 上次 I/O 的目标文件 */
	enum fio_ddir last_ddir;        /* 上次 I/O 的方向 */

	struct frand_state rand_state;  /* 随机数状态（pvsync2 的 hipri 百分比） */
};


/* ======================================================================
 * ★ 核心函数 #1: fio_syncio_prep() — lseek 优化
 * ======================================================================
 *
 * sync 引擎使用 read()/write()，它们依赖文件描述符的当前位置。
 * prep() 负责将 fd 的位置 lseek 到 io_u->offset。
 *
 * ★ 优化：如果上次 I/O 的结束位置 == 本次 offset，跳过 lseek
 *   这对于顺序 I/O 非常有效（连续 I/O 不需要 seek）
 */
static int fio_syncio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;

	if (!ddir_rw(io_u->ddir))
		return 0;

	/* ★ 优化：位置连续时不需要 lseek */
	if (LAST_POS(f) != -1ULL && LAST_POS(f) == io_u->offset)
		return 0;

	if (lseek(f->fd, io_u->offset, SEEK_SET) == -1) {
		td_verror(td, errno, "lseek");
		return 1;
	}

	return 0;
}


/* ======================================================================
 * ★ 核心函数 #2: fio_io_end() — I/O 完成后的统一处理
 * ======================================================================
 *
 * 无论使用哪个系统调用，I/O 完成后都需要：
 *   1. 更新 LAST_POS（记录结束位置）
 *   2. 检查是否完整传输（resid = 剩余未传输字节）
 *   3. 处理错误
 *   4. 返回 FIO_Q_COMPLETED
 */
static int fio_io_end(struct thread_data *td, struct io_u *io_u, int ret)
{
	/* 更新文件位置记录（为下次 lseek 优化做准备） */
	if (io_u->file && ret >= 0 && ddir_rw(io_u->ddir))
		LAST_POS(io_u->file) = io_u->offset + ret;

	/* 检查传输完整性 */
	if (ret != (int) io_u->xfer_buflen) {
		if (ret >= 0) {
			/* 部分传输：记录剩余字节 */
			io_u->resid = io_u->xfer_buflen - ret;
			io_u->error = 0;
			return FIO_Q_COMPLETED;
		} else
			/* 系统调用返回 -1：记录 errno */
			io_u->error = errno;
	}

	if (io_u->error) {
		io_u_log_error(td, io_u);
		td_verror(td, io_u->error, "xfer");
	}

	/* ★ 同步引擎始终返回 FIO_Q_COMPLETED */
	return FIO_Q_COMPLETED;
}


/* ======================================================================
 * ★ 核心函数 #3: fio_syncio_queue() — sync 引擎的 queue() 实现
 * ======================================================================
 *
 * 这是最简单的 queue() 实现：
 *   读 → read(fd, buf, len)
 *   写 → write(fd, buf, len)
 *
 * ★ 关键：系统调用阻塞等待完成，然后直接返回 FIO_Q_COMPLETED
 *   不需要 commit()、不需要 getevents()
 */
static enum fio_q_status fio_syncio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret;

	fio_ro_check(td, io_u);    /* 读写权限检查（防止写只读文件） */

	if (io_u->ddir == DDIR_READ)
		/*
		 * ★ 读操作：read()
		 * 文件位置由 prep() 中的 lseek 设置
		 */
		ret = read(f->fd, io_u->xfer_buf, io_u->xfer_buflen);
	else if (io_u->ddir == DDIR_WRITE)
		/*
		 * ★ 写操作：write()
		 * 数据来自 io_u->xfer_buf（可能是随机数据、验证数据、或零）
		 */
		ret = write(f->fd, io_u->xfer_buf, io_u->xfer_buflen);
	else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		return FIO_Q_COMPLETED;
	} else
		/* fsync / fdatasync */
		ret = do_io_u_sync(td, io_u);

	return fio_io_end(td, io_u, ret);
}


/* ======================================================================
 * ★ 核心函数 #4: fio_psyncio_queue() — psync 引擎的 queue() 实现
 * ======================================================================
 *
 * psync 使用 pread()/pwrite()，不需要 lseek。
 * 优点：原子操作，线程安全，少一次系统调用。
 *
 * ★ psync vs sync 的系统调用对比：
 *
 *   sync 模式：
 *     lseek(fd, offset, SEEK_SET)     ← 第一次系统调用
 *     read(fd, buf, len)              ← 第二次系统调用
 *     总计：2 次系统调用
 *
 *   psync 模式：
 *     pread(fd, buf, len, offset)     ← 一次系统调用搞定
 *     总计：1 次系统调用
 */
static enum fio_q_status fio_psyncio_queue(struct thread_data *td,
					   struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret;

	fio_ro_check(td, io_u);

	if (io_u->ddir == DDIR_READ)
		ret = pread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
	else if (io_u->ddir == DDIR_WRITE)
		ret = pwrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
	else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		return FIO_Q_COMPLETED;
	} else
		ret = do_io_u_sync(td, io_u);

	return fio_io_end(td, io_u, ret);
}


/* ======================================================================
 * 引擎注册：5 个 ioengine_ops 结构体
 * ======================================================================
 *
 * 每个引擎变体用一个 ioengine_ops 结构体描述。
 * fio_init 修饰的注册函数在模块加载时自动调用。
 */

/* sync 引擎：read()/write() + lseek */
static struct ioengine_ops ioengine_rw = {
	.name       = "sync",
	.version    = FIO_IOOPS_VERSION,
	.prep       = fio_syncio_prep,      /* ★ sync 需要 prep（lseek） */
	.queue      = fio_syncio_queue,
	.open_file  = generic_open_file,
	.close_file = generic_close_file,
	.get_file_size = generic_get_file_size,
	.flags      = FIO_SYNCIO | FIO_SYNCFS,  /* ★ 同步引擎标志 */
};

/* psync 引擎：pread()/pwrite()，不需要 prep */
static struct ioengine_ops ioengine_prw = {
	.name       = "psync",
	.version    = FIO_IOOPS_VERSION,
	/* ★ 注意：没有 .prep！因为 pread/pwrite 不需要 lseek */
	.queue      = fio_psyncio_queue,
	.open_file  = generic_open_file,
	.close_file = generic_close_file,
	.get_file_size = generic_get_file_size,
	.flags      = FIO_SYNCIO | FIO_SYNCFS,
};

/* vsync 引擎：readv()/writev()，支持批量提交 */
static struct ioengine_ops ioengine_vrw = {
	.name       = "vsync",
	.version    = FIO_IOOPS_VERSION,
	.init       = fio_vsyncio_init,
	.cleanup    = fio_vsyncio_cleanup,
	.queue      = fio_vsyncio_queue,
	.commit     = fio_vsyncio_commit,    /* ★ vsync 有 commit */
	.event      = fio_vsyncio_event,
	.getevents  = fio_vsyncio_getevents,
	.open_file  = generic_open_file,
	.close_file = generic_close_file,
	.get_file_size = generic_get_file_size,
	.flags      = FIO_SYNCIO | FIO_SYNCFS,
};

/* ... pvsync / pvsync2 类似，使用 preadv/pwritev 和 preadv2/pwritev2 ... */


/* 注册函数：在模块加载时自动调用 */
static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine_rw);     /* sync */
	register_ioengine(&ioengine_prw);    /* psync */
	register_ioengine(&ioengine_vrw);    /* vsync */
	/* ... pvsync, pvsync2 ... */
}

/* 注销函数：在模块卸载时自动调用 */
static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine_rw);
	unregister_ioengine(&ioengine_prw);
	unregister_ioengine(&ioengine_vrw);
	/* ... */
}


/*
 * ======================================================================
 * ★ 总结：sync 引擎的 I/O 路径
 * ======================================================================
 *
 *   do_io()
 *     │
 *     ├── get_io_u()                   ← 获取 io_u（offset/buflen/ddir 已填充）
 *     │
 *     ├── td_io_queue()
 *     │     └── fio_syncio_queue()
 *     │           │
 *     │           ├── [DDIR_READ]
 *     │           │     read(fd, buf, buflen)
 *     │           │     ← 阻塞等待完成
 *     │           │
 *     │           ├── [DDIR_WRITE]
 *     │           │     write(fd, buf, buflen)
 *     │           │     ← 阻塞等待完成
 *     │           │
 *     │           └── return FIO_Q_COMPLETED   ← ★ 同步完成
 *     │
 *     └── 无需 getevents()（同步引擎没有异步事件需要收割）
 *
 *   ★ sync 引擎 vs 异步引擎的关键区别：
 *
 *   ┌──────────────────┬──────────────────┬──────────────────────────┐
 *   │                  │ sync 引擎         │ 异步引擎（libaio 等）     │
 *   ├──────────────────┼──────────────────┼──────────────────────────┤
 *   │ queue() 行为     │ 阻塞等待完成      │ 入队后立即返回            │
 *   │ queue() 返回值   │ FIO_Q_COMPLETED  │ FIO_Q_QUEUED             │
 *   │ iodepth          │ 始终为 1          │ 可配置（如 32）           │
 *   │ commit()         │ 不需要            │ io_submit() 批量提交      │
 *   │ getevents()      │ 不需要            │ io_getevents() 收割       │
 *   │ 并发 I/O         │ 不可能            │ 多个 I/O 同时在飞          │
 *   └──────────────────┴──────────────────┴──────────────────────────┘
 */
