/*
 * ======================================================================
 * Step 2: ioengines.h — I/O 引擎插件接口定义
 * ======================================================================
 *
 * 原始路径：src/fio/ioengines.h (159 行)
 *
 * fio 的强大之处在于其插件式的 I/O 引擎架构——同一个 fio 框架，
 * 可以通过切换引擎来测试完全不同的 I/O 路径：
 *   - sync:    普通的 read()/write()
 *   - libaio:  Linux 原生异步 I/O（io_submit/io_getevents）
 *   - io_uring: 最新的高性能异步 I/O
 *   - spdk:    用户态 NVMe 驱动（绕过内核）
 *
 * 本文件定义了所有引擎必须实现的"接口合约"：struct ioengine_ops。
 * 类比面向对象中的"抽象基类"或 Java 的 Interface。
 *
 * 阅读要点：
 *   1. fio_q_status 枚举 — queue() 的三种返回值
 *   2. struct ioengine_ops — 引擎必须实现的函数指针
 *   3. 引擎标志位（FIO_SYNCIO / FIO_RAWIO 等）
 *   4. 引擎注册/加载的外部函数声明
 *
 * 预计阅读时间：10 分钟
 * ======================================================================
 */

#ifndef FIO_IOENGINE_H
#define FIO_IOENGINE_H

/* ... 头文件省略 ... */

#define FIO_IOOPS_VERSION   39   /* 接口版本号，引擎必须匹配 */

/* ======================================================================
 * 第一部分：★ queue() 返回值枚举 fio_q_status
 * ======================================================================
 *
 * 这是引擎的 queue() 函数提交 I/O 后的三种可能结果：
 *
 *   FIO_Q_COMPLETED ─┐  I/O 已经完成了（同步引擎）
 *   FIO_Q_QUEUED    ─┤  I/O 已入队，将来异步完成（异步引擎）
 *   FIO_Q_BUSY      ─┘  队列满了，请先 reap 一些完成事件
 *
 * ┌───────────────────────────────────────────────────────────────┐
 * │                    do_io() 主循环                             │
 * │                                                               │
 * │   ret = engine->queue(td, io_u);                              │
 * │                                                               │
 * │   if ret == FIO_Q_COMPLETED:                                  │
 * │       直接记录完成统计（无需 getevents）                        │
 * │                                                               │
 * │   if ret == FIO_Q_QUEUED:                                     │
 * │       继续循环提交，直到 queue_full()                          │
 * │       然后调用 getevents() 等待一批完成                         │
 * │                                                               │
 * │   if ret == FIO_Q_BUSY:                                       │
 * │       先调用 getevents() reap 一些事件                         │
 * │       然后重试 queue()                                         │
 * └───────────────────────────────────────────────────────────────┘
 */
enum fio_q_status {
	FIO_Q_COMPLETED = 0,       /* ★ I/O 同步完成（sync 引擎） */
	FIO_Q_QUEUED    = 1,       /* ★ I/O 已入队，异步完成（libaio/io_uring） */
	FIO_Q_BUSY      = 2,       /* ★ 队列满，请先 commit() + getevents() */
};


/* ======================================================================
 * 第二部分：★ 核心结构体 ioengine_ops
 * ======================================================================
 *
 * 每个 I/O 引擎都要填充一个 ioengine_ops 结构体，
 * 注册到 fio 的全局引擎链表中。
 *
 * 引擎可以只实现部分函数指针——未实现的留 NULL。
 * fio 框架会在调用前检查是否为 NULL。
 *
 * 引擎生命周期：
 *
 *   setup()         ← 初始化引擎（分配引擎私有数据）
 *     │
 *     ▼
 *   init()          ← 运行时初始化（打开设备、创建 io_context 等）
 *     │
 *     ▼
 *   post_init()     ← 后初始化（libaio 用它来调用 io_queue_init）
 *     │
 *     ▼
 *   ┌──────────────────────────────────────────────┐
 *   │              I/O 主循环                       │
 *   │                                               │
 *   │  prep()   ← 准备单个 io_u（填充 iocb 等）     │
 *   │  queue()  ← ★ 提交单个 io_u                   │
 *   │  commit() ← 批量提交（异步引擎）               │
 *   │  getevents() ← 等待一批完成                    │
 *   │  event()  ← 取出第 N 个完成事件               │
 *   └──────────────────────────────────────────────┘
 *     │
 *     ▼
 *   cleanup()       ← 清理引擎资源
 *
 * 文件操作（可选，默认用 generic_open_file 等）：
 *   open_file() / close_file() / get_file_size()
 */
struct ioengine_ops {
	/* ─── 引擎元信息 ─── */
	struct flist_head list;         /* 全局引擎链表节点 */
	const char *name;               /* ★ 引擎名称（如 "libaio"、"sync"） */
	int version;                    /* 接口版本（必须 == FIO_IOOPS_VERSION） */
	int flags;                      /* ★ 引擎能力标志（见下面的 FIO_* 枚举） */
	void *dlhandle;                 /* dlopen 句柄（动态加载引擎时使用） */

	/* ─── ★ 引擎生命周期函数 ─── */
	int (*setup)(struct thread_data *);       /* 初始化引擎（配置阶段） */
	int (*init)(struct thread_data *);        /* 运行时初始化（运行阶段） */
	int (*post_init)(struct thread_data *);   /* 后初始化（io_queue_init 等） */

	/* ─── ★ I/O 提交/完成函数（核心） ─── */
	int (*prep)(struct thread_data *, struct io_u *);
	/*
	 * prep(): 准备单个 io_u
	 *   - libaio: 调用 io_prep_pread/io_prep_pwrite 填充 iocb
	 *   - sync:   通常不需要（返回 0 即可）
	 */

	enum fio_q_status (*queue)(struct thread_data *, struct io_u *);
	/*
	 * ★ queue(): 提交单个 I/O — 引擎最核心的函数
	 *   - sync:   调用 read()/write()，阻塞等待完成，返回 FIO_Q_COMPLETED
	 *   - libaio: 将 iocb 放入队列，返回 FIO_Q_QUEUED（不等待完成）
	 *   - 队列满时返回 FIO_Q_BUSY
	 */

	int (*commit)(struct thread_data *);
	/*
	 * commit(): 批量提交已 queue 的 I/O
	 *   - sync:   不需要（queue 已直接完成）
	 *   - libaio: 调用 io_submit() 批量提交所有已 queue 的 iocb
	 */

	int (*getevents)(struct thread_data *, unsigned int, unsigned int,
			 const struct timespec *);
	/*
	 * ★ getevents(): 等待 I/O 完成
	 *   - sync:   不需要（queue 已直接完成）
	 *   - libaio: 调用 io_getevents() 等待 min~max 个完成事件
	 *   参数: min=最少等待数, max=最多等待数, timespec=超时
	 *   返回: 实际完成的事件数
	 */

	struct io_u *(*event)(struct thread_data *, int);
	/*
	 * event(): 获取第 N 个完成事件对应的 io_u
	 *   - getevents() 返回后，框架调用 event(0), event(1), ...
	 *     来逐个获取完成的 io_u
	 */

	/* ─── 错误处理 ─── */
	char *(*errdetails)(struct thread_data *, struct io_u *);
	/* 返回错误的详细描述字符串（可选） */

	/* ─── 清理 ─── */
	void (*cleanup)(struct thread_data *);    /* 释放引擎资源 */

	/* ─── 文件操作（可选，一般用 generic_* 默认实现） ─── */
	int (*open_file)(struct thread_data *, struct fio_file *);
	int (*close_file)(struct thread_data *, struct fio_file *);
	int (*invalidate)(struct thread_data *, struct fio_file *);
	int (*unlink_file)(struct thread_data *, struct fio_file *);
	int (*get_file_size)(struct thread_data *, struct fio_file *);
	int (*prepopulate_file)(struct thread_data *, struct fio_file *);

	/* ─── 控制 ─── */
	void (*terminate)(struct thread_data *);   /* 终止正在运行的 I/O */

	/* ─── 内存管理（可选，用于特殊内存分配需求） ─── */
	int (*iomem_alloc)(struct thread_data *, size_t);
	void (*iomem_free)(struct thread_data *);

	/* ─── io_u 级别的初始化/清理（可选） ─── */
	int (*io_u_init)(struct thread_data *, struct io_u *);
	void (*io_u_free)(struct thread_data *, struct io_u *);

	/* ─── ZBD（Zoned Block Device）相关（可选） ─── */
	int (*get_zoned_model)(...);
	int (*report_zones)(...);
	int (*reset_wp)(...);
	int (*move_zone_wp)(...);
	int (*get_max_open_zones)(...);
	int (*get_max_active_zones)(...);
	int (*finish_zone)(...);

	/* ─── FDP（Flexible Data Placement）相关（可选） ─── */
	int (*fdp_fetch_ruhs)(...);

	/* ─── 引擎私有选项 ─── */
	int option_struct_size;           /* 引擎选项结构体的大小 */
	struct fio_option *options;       /* 引擎特有的配置选项 */
};


/* ======================================================================
 * 第三部分：引擎标志位
 * ======================================================================
 *
 * 这些标志告诉 fio 框架引擎的"能力"和"限制"，
 * 框架据此决定是否需要做额外处理。
 *
 * 例如：
 *   FIO_SYNCIO — queue() 是同步的，不需要 getevents()
 *   FIO_RAWIO  — 直接 I/O，不走文件系统
 *   FIO_FAKEIO — 假 I/O（null 引擎，用于测试框架本身）
 */
enum {
	__FIO_SYNCIO = 0,       /* ★ queue() 是同步的（read/write 阻塞完成） */
	__FIO_RAWIO,            /* 直接 I/O（raw device） */
	__FIO_DISKLESSIO,       /* 无磁盘参与（网络引擎等） */
	__FIO_NOEXTEND,         /* 引擎不能扩展文件 */
	__FIO_NODISKUTIL,       /* diskutil 无法处理（特殊设备） */
	__FIO_UNIDIR,           /* 单向引擎（只读或只写） */
	__FIO_NOIO,             /* 不实际做 I/O（伪 I/O） */
	__FIO_PIPEIO,           /* 管道 I/O（不可 seek） */
	__FIO_BARRIER,          /* 支持 barrier */
	__FIO_MEMALIGN,         /* 需要对齐内存 */
	__FIO_BIT_BASED,        /* 以 bit 为单位（非 byte） */
	__FIO_FAKEIO,           /* ★ 假装做 I/O（null 引擎） */
	__FIO_NOSTATS,          /* 不做统计 */
	__FIO_NOFILEHASH,       /* 不哈希文件用于查找 */
	__FIO_ASYNCIO_SYNC_TRIM, /* 异步引擎但 trim 是同步的 */
	__FIO_ASYNCIO_SYNC_SYNCFS, /* 异步引擎但 syncfs 是同步的 */
	__FIO_NO_OFFLOAD,       /* 无异步卸载 */
	__FIO_ASYNCIO_SETS_ISSUE_TIME, /* 异步引擎在 commit 中设置 issue_time */
	__FIO_SKIPPABLE_IOMEM_ALLOC,   /* 可跳过 iomem 分配 */
	__FIO_RO_NEEDS_RW_OPEN,        /* 只读 job 也要 RW 打开 */
	__FIO_MULTI_RANGE_TRIM,        /* 支持多范围 trim */
	__FIO_ATOMICWRITES,            /* 支持原子写 */
	__FIO_SYNCFS,                  /* 支持 syncfs */
	__FIO_IOENGINE_F_LAST,
};

/* 标志位的位掩码版本（1 << __FIO_xxx） */
enum fio_ioengine_flags {
	FIO_SYNCIO          = 1 << __FIO_SYNCIO,
	FIO_RAWIO           = 1 << __FIO_RAWIO,
	/* ... 其余类似 ... */
};


/* ======================================================================
 * 第四部分：引擎入口函数声明
 * ======================================================================
 *
 * 这些是 fio 框架调用引擎的"包装函数"：
 *   td_io_queue()    → 调用 engine->queue()，同时记录统计
 *   td_io_getevents() → 调用 engine->getevents()
 *   td_io_commit()   → 调用 engine->commit()
 *
 * 它们的作用是在引擎调用前后添加：
 *   - 统计记录（延迟、吞吐等）
 *   - 错误处理
 *   - 调试日志
 */
extern int __must_check td_io_init(struct thread_data *);
extern int __must_check td_io_prep(struct thread_data *, struct io_u *);
extern enum fio_q_status __must_check td_io_queue(struct thread_data *, struct io_u *);
extern int __must_check td_io_getevents(struct thread_data *, unsigned int,
					unsigned int, const struct timespec *);
extern void td_io_commit(struct thread_data *);
extern int __must_check td_io_open_file(struct thread_data *, struct fio_file *);
extern int td_io_close_file(struct thread_data *, struct fio_file *);
extern int td_io_unlink_file(struct thread_data *, struct fio_file *);
extern int __must_check td_io_get_file_size(struct thread_data *, struct fio_file *);

/* 引擎加载/注册/卸载 */
extern struct ioengine_ops *load_ioengine(struct thread_data *);
extern void register_ioengine(struct ioengine_ops *);
extern void unregister_ioengine(struct ioengine_ops *);
extern void free_ioengine(struct thread_data *);
extern void close_ioengine(struct thread_data *);

/* 外部引擎的加载方式：dlopen 后查找 get_ioengine 符号 */
typedef void (*get_ioengine_t)(struct ioengine_ops **);

extern int fio_show_ioengine_help(const char *engine);

#endif


/*
 * ======================================================================
 * 总结：ioengine_ops 函数指针与引擎实现的映射
 * ======================================================================
 *
 * ┌───────────────┬──────────────────────┬────────────────────────────┐
 * │ 函数指针       │ sync 引擎             │ libaio 引擎                │
 * ├───────────────┼──────────────────────┼────────────────────────────┤
 * │ setup         │ (NULL)               │ (NULL)                     │
 * │ init          │ (NULL)               │ fio_libaio_init()          │
 * │ post_init     │ (NULL)               │ fio_libaio_post_init()     │
 * │ prep          │ fio_syncio_prep()    │ fio_libaio_prep()          │
 * │ queue         │ fio_syncio_queue()   │ fio_libaio_queue()         │
 * │ commit        │ (NULL)               │ fio_libaio_commit()        │
 * │ getevents     │ (NULL)               │ fio_libaio_getevents()     │
 * │ event         │ (NULL)               │ fio_libaio_event()         │
 * │ cleanup       │ (NULL)               │ fio_libaio_cleanup()       │
 * │ open_file     │ generic_open_file    │ generic_open_file          │
 * │ close_file    │ generic_close_file   │ generic_close_file         │
 * │ flags         │ FIO_SYNCIO           │ FIO_ASYNCIO_SYNC_TRIM      │
 * ├───────────────┼──────────────────────┼────────────────────────────┤
 * │ I/O 模型      │ 同步阻塞              │ 异步批量提交+批量收割       │
 * │ iodepth       │ 始终为 1              │ 可配置（如 32）            │
 * └───────────────┴──────────────────────┴────────────────────────────┘
 *
 * 调用链：
 *
 *   do_io()
 *     → get_io_u()
 *     → td_io_queue()
 *       → engine->queue()           ← ★ 引擎实现
 *         ├── sync:  read()/write() → 返回 FIO_Q_COMPLETED
 *         └── libaio: io_prep + 入队 → 返回 FIO_Q_QUEUED
 *     → td_io_commit()
 *       → engine->commit()          ← libaio: io_submit()
 *     → td_io_getevents()
 *       → engine->getevents()       ← libaio: io_getevents()
 *     → engine->event(N)            ← 取出第 N 个完成的 io_u
 */
