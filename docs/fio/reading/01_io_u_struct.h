/*
 * ======================================================================
 * Step 1: io_u.h — fio 的 I/O 单元结构体
 * ======================================================================
 *
 * 原始路径：src/fio/io_u.h (213 行)
 *
 * io_u 是 fio 中最核心的数据结构——每一个 I/O 请求都用一个 io_u 来描述。
 * 类比 blktrace 中的 blk_io_trace：blk_io_trace 是"记录"一个 I/O 事件，
 * 而 io_u 是"发起"一个 I/O 事件。
 *
 * fio 的工作流程就是：
 *   分配 io_u → 填充 offset/buflen/ddir → 交给引擎提交 → 等待完成 → 回收 io_u
 *
 * 阅读要点：
 *   1. 时间戳字段 start_time / issue_time — 用于计算 slat/clat/lat
 *   2. offset/buflen — I/O 的位置和大小
 *   3. ddir — I/O 方向（读/写/trim/sync）
 *   4. flags — io_u 的状态机（FREE → FLIGHT → 回收）
 *   5. 底部 union — 引擎私有数据（iocb/aiocb/sg_io_hdr 等）
 *
 * 预计阅读时间：10 分钟
 * ======================================================================
 */

#ifndef FIO_IO_U
#define FIO_IO_U

/* ... 头文件省略 ... */

/* ======================================================================
 * 第一部分：io_u 标志位（状态机）
 * ======================================================================
 *
 * io_u 的生命周期：
 *
 *   ┌──────────┐    get_io_u()    ┌──────────┐    queue()    ┌──────────┐
 *   │  FREE     │ ───────────────→ │ IN_CUR   │ ────────────→ │ FLIGHT   │
 *   │ (空闲池)  │                  │  _DEPTH  │               │ (在飞)    │
 *   └──────────┘                  └──────────┘               └──────────┘
 *        ↑                                                       │
 *        │                     put_io_u()                        │
 *        └───────────────────────────────────────────────────────┘
 *
 * ★ 重要：IO_U_F_FREE 和 IO_U_F_FLIGHT 互斥
 *   - FREE = 在空闲池中，可以使用
 *   - FLIGHT = 已提交给引擎，等待完成
 */
enum {
	IO_U_F_FREE        = 1 << 0,   /* io_u 在空闲池中 */
	IO_U_F_FLIGHT      = 1 << 1,   /* ★ io_u 已提交，等待完成 */
	IO_U_F_NO_FILE_PUT = 1 << 2,   /* 不自动释放文件引用 */
	IO_U_F_IN_CUR_DEPTH= 1 << 3,   /* ★ 计入当前队列深度 */
	IO_U_F_BUSY_OK     = 1 << 4,   /* 允许引擎返回 BUSY */
	IO_U_F_TRIMMED     = 1 << 5,   /* 已完成 TRIM */
	IO_U_F_BARRIER     = 1 << 6,   /* barrier I/O */
	IO_U_F_VER_LIST    = 1 << 7,   /* 来自验证列表 */
	IO_U_F_PATTERN_DONE= 1 << 8,   /* 数据模式已填充 */
	IO_U_F_DEVICE_ERROR= 1 << 9,   /* 设备级错误 */
	IO_U_F_VER_IN_DEV  = 1 << 10,  /* 验证数据在设备中 */
	IO_U_F_ZEROED      = 1 << 11,  /* 零填充数据 */
	IO_U_F_ERRORED     = 1 << 12,  /* 已出错的 offset */
};


/* ======================================================================
 * 第二部分：★ 核心结构体 io_u
 * ======================================================================
 *
 * io_u 是 fio 发起的每一个 I/O 的"身份证"。
 * 它携带了 I/O 的全部信息：方向、位置、大小、缓冲区、时间戳、引擎私有数据等。
 *
 * 在 fio 内部，有一个 io_u 池（freelist），
 * do_io() 主循环从池中取出 io_u，填充后交给引擎，完成后归还。
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ 时间戳: start_time → issue_time → (引擎完成)             │
 *   │           │             │             │                   │
 *   │           └── slat ─────┤             │                   │
 *   │                         ├── clat ─────┤                   │
 *   │           └──────── lat ──────────────┘                   │
 *   │                                                           │
 *   │ I/O 描述: file + offset + buflen + ddir                   │
 *   │ 缓冲区:   buf → xfer_buf → xfer_buflen                   │
 *   │ 引擎数据: union { iocb / aiocb / sg_io_hdr / ... }       │
 *   └──────────────────────────────────────────────────────────┘
 */
struct io_u {
	/* ─── 时间戳字段 ─── */
	/*
	 * ★ start_time: io_u 被 get_io_u() 取出的时刻
	 * ★ issue_time: io_u 被引擎真正提交（io_submit 等）的时刻
	 *
	 * 延迟计算：
	 *   slat = issue_time - start_time     （提交延迟，对应 blktrace Q→D）
	 *   clat = complete_time - issue_time   （完成延迟，对应 blktrace D→C）
	 *   lat  = complete_time - start_time   （总延迟，对应 blktrace Q→C）
	 */
	struct timespec start_time;
	struct timespec issue_time;

	/* ─── I/O 目标 ─── */
	struct fio_file *file;          /* 目标文件/设备 */
	unsigned int flags;             /* 状态标志（见上面的 IO_U_F_*） */
	enum fio_ddir ddir;             /* ★ I/O 方向：DDIR_READ / DDIR_WRITE / DDIR_TRIM / DDIR_SYNC */

	/*
	 * 回放工作负载时，可能要以不同的 IO 类型来统计
	 * 例如：回放时实际是写，但统计时按读来算
	 */
	enum fio_ddir acct_ddir;

	/* ─── 写操作序号 ─── */
	uint64_t numberio;              /* 第几次写操作（用于验证） */

	/* ─── IO 优先级 ─── */
	unsigned short ioprio;          /* IO 优先级（ioprio_set 设置的值） */
	unsigned short clat_prio_index; /* clat 统计的优先级索引 */

	/* ─── TRIM 相关 ─── */
	unsigned int number_trim;       /* 本次 IO 包含的 trim 范围数 */

	/* ─── ★ I/O 位置和大小 ─── */
	/*
	 * buflen:  I/O 传输大小（字节）
	 * offset:  I/O 在文件中的偏移（字节），即 xfer_offset
	 * verify_offset: 验证用的偏移（和 offset 可能不同）
	 * buf:     数据缓冲区指针
	 *
	 * 对于顺序 I/O：offset = last_pos（上一次结束位置）
	 * 对于随机 I/O：offset = 随机数生成器产生的位置
	 */
	unsigned long long buflen;
	unsigned long long offset;
	unsigned long long verify_offset;
	void *buf;

	/* 随机数种子（用于生成缓冲区内容） */
	uint64_t rand_seed;

	/* ─── 实际传输缓冲区 ─── */
	/*
	 * 当引擎返回部分传输（partial transfer）时，
	 * xfer_buf / xfer_buflen 会指向剩余未传输的部分
	 * 初始值：xfer_buf = buf, xfer_buflen = buflen
	 */
	void *xfer_buf;
	unsigned long long xfer_buflen;

	/* 预填充缓冲区已填充的长度（处理可变块大小） */
	unsigned long long buf_filled_len;

	/* 验证相关 */
	struct io_piece *ipo;

	/* 剩余未传输字节数 + 错误码 */
	unsigned long long resid;
	unsigned int error;

	int inflight_idx;

	/* ─── 引擎私有状态 ─── */
	union {
		unsigned int index;     /* io_u 在池中的索引 */
		unsigned int seen;      /* 已使用次数 */
	};
	void *engine_data;              /* 引擎私有数据指针 */

	/* 验证列表 or 工作队列（互斥使用） */
	union {
		struct flist_head verify_list;
		struct workqueue_work work;
	};

	/* ─── ZBD（Zoned Block Device）回调 ─── */
	void (*zbd_queue_io)(struct thread_data *td, struct io_u *, int *q);
	void (*zbd_put_io)(struct thread_data *td, const struct io_u *);

	/* ─── I/O 完成回调 ─── */
	/*
	 * end_io 在 I/O 完成后被调用：
	 *   - 普通 I/O：默认回调（记录统计）
	 *   - 验证 I/O：verify_io_u（检查数据正确性）
	 *   - 异步验证：verify_io_u_async
	 */
	int (*end_io)(struct thread_data *, struct io_u **);

	/* 数据类型/规格（用于 NVMe ZNS 等） */
	uint32_t dtype;
	uint32_t dspec;

	/* ─── ★ 引擎私有数据（union，按编译选项选择） ─── */
	/*
	 * 每个引擎需要自己的数据结构来跟踪 I/O：
	 *   - libaio:    struct iocb（Linux AIO 控制块）
	 *   - posixaio:  os_aiocb_t（POSIX AIO 控制块）
	 *   - sg:        struct sg_io_hdr（SCSI generic 头）
	 *   - solarisaio: aio_result_t
	 *   - rdma:      struct ibv_mr（内存注册）
	 *   - mmap:      void *mmap_data
	 *
	 * ★ 使用 union 意味着同一时刻只能用一个引擎——这是编译期决定的
	 */
	union {
#ifdef CONFIG_LIBAIO
		struct iocb iocb;               /* libaio 引擎：io_prep_pread/io_prep_pwrite */
#endif
#ifdef CONFIG_POSIXAIO
		os_aiocb_t aiocb;               /* POSIX AIO 引擎 */
#endif
#ifdef FIO_HAVE_SGIO
		struct sg_io_hdr hdr;           /* SCSI generic 引擎 */
#endif
#ifdef CONFIG_SOLARISAIO
		aio_result_t resultp;           /* Solaris AIO 引擎 */
#endif
#ifdef CONFIG_RDMA
		struct ibv_mr *mr;              /* RDMA 内存注册 */
#endif
		void *mmap_data;                /* mmap 引擎 */
	};
	void *pi_attr;                          /* 保护信息属性 */
};


/* ======================================================================
 * 第三部分：io_u 操作函数声明
 * ======================================================================
 *
 * 这些函数构成了 io_u 的完整生命周期管理：
 *
 *   分配 → 填充 → 提交 → 完成 → 回收
 *
 *   __get_io_u()     从空闲池弹出一个 io_u（不填充 offset/ddir）
 *   get_io_u()       ★ 完整流程：弹出 + 填充 offset/buflen/ddir
 *   put_io_u()       归还 io_u 到空闲池
 *   clear_io_u()     清除 io_u 的状态
 *   requeue_io_u()   将 io_u 放回重传队列（引擎返回 BUSY 时）
 *
 *   io_u_queued()    io_u 被提交后调用（记录 slat）
 *   io_u_queued_complete()  等待队列中的 io_u 完成
 *   io_u_sync_complete()    同步 I/O 的完成处理
 */
extern struct io_u *__get_io_u(struct thread_data *);
extern struct io_u *get_io_u(struct thread_data *);
extern void put_io_u(struct thread_data *, struct io_u *);
extern void clear_io_u(struct thread_data *, struct io_u *);
extern void requeue_io_u(struct thread_data *, struct io_u **);
extern int __must_check io_u_sync_complete(struct thread_data *, struct io_u *);
extern int __must_check io_u_queued_complete(struct thread_data *, int);
extern void io_u_queued(struct thread_data *, struct io_u *);
extern int io_u_quiesce(struct thread_data *);
extern void io_u_log_error(struct thread_data *, struct io_u *);
extern void io_u_mark_depth(struct thread_data *, unsigned int);
extern void fill_io_buffer(struct thread_data *, void *, unsigned long long, unsigned long long);
extern void io_u_fill_buffer(struct thread_data *td, struct io_u *, unsigned long long, unsigned long long);
void io_u_mark_complete(struct thread_data *, unsigned int);
void io_u_mark_submit(struct thread_data *, unsigned int);
bool queue_full(const struct thread_data *);

int do_io_u_sync(const struct thread_data *, struct io_u *);
int do_io_u_trim(struct thread_data *, struct io_u *);


/* ======================================================================
 * 第四部分：调试辅助
 * ======================================================================
 *
 * dprint_io_u() 在启用 FIO_INC_DEBUG 时打印 io_u 的关键信息：
 *   偏移、长度、方向、文件名
 *
 * 用法示例：dprint(FD_IO, "处理 io_u", io_u);
 * 输出：io_u 0x7f...: off=0x1000,len=0x1000,ddir=0,file=/dev/vdb
 */
#ifdef FIO_INC_DEBUG
static inline void dprint_io_u(struct io_u *io_u, const char *p)
{
	struct fio_file *f = io_u->file;

	if (f)
		dprint(FD_IO, "%s: io_u %p: off=0x%llx,len=0x%llx,ddir=%d,file=%s\n",
				p, io_u,
				(unsigned long long) io_u->offset,
				io_u->buflen, io_u->ddir,
				f->file_name);
	else
		dprint(FD_IO, "%s: io_u %p: off=0x%llx,len=0x%llx,ddir=%d\n",
				p, io_u,
				(unsigned long long) io_u->offset,
				io_u->buflen, io_u->ddir);
}
#else
#define dprint_io_u(io_u, p)
#endif


/* ======================================================================
 * 第五部分：acct_ddir 辅助函数
 * ======================================================================
 *
 * 用于回放场景：如果设置了 acct_ddir，用它代替实际 ddir 来统计。
 * 例如回放一个写操作的 trace 时，可能想按读来统计延迟。
 */
static inline enum fio_ddir acct_ddir(struct io_u *io_u)
{
	if (io_u->acct_ddir != -1)
		return io_u->acct_ddir;
	return io_u->ddir;
}

/* 标志位操作的便捷宏 */
#define io_u_clear(td, io_u, val) \
	td_flags_clear((td), &(io_u->flags), (val))
#define io_u_set(td, io_u, val) \
	td_flags_set((td), &(io_u)->flags, (val))

#endif


/*
 * ======================================================================
 * 总结：io_u 结构体与 blk_io_trace 的对照
 * ======================================================================
 *
 * ┌────────────────────┬────────────────────┬──────────────────────────────┐
 * │ io_u 字段           │ blk_io_trace 字段  │ 说明                         │
 * ├────────────────────┼────────────────────┼──────────────────────────────┤
 * │ offset             │ sector             │ I/O 位置（fio 用字节，blktrace 用扇区）│
 * │ buflen             │ bytes              │ I/O 大小（都用字节）          │
 * │ ddir               │ action (R/W)       │ I/O 方向                     │
 * │ start_time         │ —                  │ fio 独有：get_io_u 的时间     │
 * │ issue_time         │ time (Q 事件)      │ 提交时间                      │
 * │ —                  │ time (C 事件)      │ blktrace 的完成时间           │
 * │ error              │ error              │ 错误码                       │
 * │ iocb (union)       │ —                  │ 引擎私有数据（fio 独有）      │
 * └────────────────────┴────────────────────┴──────────────────────────────┘
 *
 * 调用链（io_u 的一生）：
 *
 *   do_io()
 *     → get_io_u()                // 从池中取出 + 填充 offset/buflen/ddir
 *       → __get_io_u()            // 从 freelist/requeue 弹出
 *       → set_io_u_file()         // 选择目标文件
 *       → fill_io_u()             // 设置 ddir + offset + buflen
 *         → set_rw_ddir()         // 决定读还是写
 *         → get_next_offset()     // ★ 计算 offset（顺序 or 随机）
 *         → get_next_buflen()     // 计算 buflen
 *     → td_io_queue()             // 调用引擎的 queue()
 *     → wait_for_completions()    // 等待完成
 *     → put_io_u()                // 归还到池中
 */
