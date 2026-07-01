/*
 * ======================================================================
 * Step 1: blktrace_api.h — 数据结构与 API 定义
 * ======================================================================
 *
 * 原始路径：
 *   内核版：src/linux-5.10/include/uapi/linux/blktrace_api.h
 *   用户态：src/blktrace/blktrace_api.h
 *
 * 本文件是 blktrace 全链路的"宪法"——所有事件的结构体、标志位、ioctl 都在这里定义。
 * 内核和用户态各有一份副本，内容几乎一致。
 *
 * 阅读要点：
 *   1. blk_io_trace 结构体 — 每个 I/O 事件的记录格式
 *   2. action 标志位（BLK_TC_*）— 位掩码，不是枚举！
 *   3. action 组合宏（BLK_TA_*）— 将类别和操作编码到 32 位中
 *   4. ioctl 定义 — 用户态与内核通信的 4 个命令
 *
 * 预计阅读时间：10 分钟
 * ======================================================================
 */

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPIBLKTRACE_H
#define _UAPIBLKTRACE_H

#include <linux/types.h>

/* ======================================================================
 * 第一部分：Trace 类别标志（低 16 位）
 * ======================================================================
 *
 * 这些标志用位掩码表示 I/O 事件的"属性"。
 * 一个事件可以同时具有多个属性，例如：
 *   一个 FUA 写请求 = BLK_TC_WRITE | BLK_TC_FUA | BLK_TC_ISSUE
 *
 * ★ 重要：这是位掩码，检测时用 & 运算，不能用 ==
 */
enum blktrace_cat {
	BLK_TC_READ     = 1 << 0,   /* 读操作 */
	BLK_TC_WRITE    = 1 << 1,   /* 写操作 */
	BLK_TC_FLUSH    = 1 << 2,   /* flush 操作（刷缓存到介质） */
	BLK_TC_SYNC     = 1 << 3,   /* 同步 I/O（O_SYNC 打开的文件） */
	BLK_TC_SYNCIO   = BLK_TC_SYNC,
	BLK_TC_QUEUE    = 1 << 4,   /* ★ Q：入队/合并阶段 */
	BLK_TC_REQUEUE  = 1 << 5,   /* 重新入队（设备拒绝了请求） */
	BLK_TC_ISSUE    = 1 << 6,   /* ★ D：下发到设备驱动 */
	BLK_TC_COMPLETE = 1 << 7,   /* ★ C：I/O 完成 */
	BLK_TC_FS       = 1 << 8,   /* 文件系统请求（非 SCSI 直通） */
	BLK_TC_PC       = 1 << 9,   /* PC 请求（SCSI passthrough 命令） */
	BLK_TC_NOTIFY   = 1 << 10,  /* 特殊通知消息（进程名映射等） */
	BLK_TC_AHEAD    = 1 << 11,  /* 预读（readahead） */
	BLK_TC_META     = 1 << 12,  /* 元数据（非用户数据） */
	BLK_TC_DISCARD  = 1 << 13,  /* TRIM/discard 请求 */
	BLK_TC_DRV_DATA = 1 << 14,  /* 驱动私有二进制数据 */
	BLK_TC_FUA      = 1 << 15,  /* ★ FUA：强制直接写介质，绕过缓存 */

	BLK_TC_END      = 1 << 15,  /* 位用完了！16 位到此为止 */
};

/*
 * action 字段的编码方式：
 *
 *   高 16 位 = 类别标志（BLK_TC_*）左移 16 位
 *   低 8 位  = 操作类型（__BLK_TA_*）
 *
 *   ┌────────────────┬────────┬────────────┐
 *   │ 类别标志(16bit) │ 保留   │ 操作(8bit)  │
 *   │ BLK_TC_ISSUE   │        │ __BLK_TA_* │
 *   └────────────────┴────────┴────────────┘
 *   ← 高 16 位 →              ← 低 8 位 →
 *
 *   例如 BLK_TA_ISSUE = __BLK_TA_ISSUE(7) | BLK_TC_ACT(BLK_TC_ISSUE(6))
 *   = 7 | (6 << 16) = 0x00060007
 */
#define BLK_TC_SHIFT        (16)
#define BLK_TC_ACT(act)     ((act) << BLK_TC_SHIFT)


/* ======================================================================
 * 第二部分：操作类型（低 8 位）
 * ======================================================================
 *
 * 这些值表示 I/O 在 block layer 中经历的"具体动作"。
 * 它们会和上面的类别标志组合成完整的 action 值。
 */
enum blktrace_act {
	__BLK_TA_QUEUE = 1,         /* 入队（bio 进入 block layer）→ 对应 Q */
	__BLK_TA_BACKMERGE,         /* 后向合并（新 bio 追加到已有 request 尾部） */
	__BLK_TA_FRONTMERGE,        /* 前向合并（新 bio 插入到已有 request 头部） */
	__BLK_TA_GETRQ,             /* 分配 request 结构 */
	__BLK_TA_SLEEPRQ,           /* 等待 request 分配（队列满了） */
	__BLK_TA_REQUEUE,           /* 重新入队（设备返回错误，需要重试） */
	__BLK_TA_ISSUE,             /* ★ 下发到设备驱动 → 对应 D */
	__BLK_TA_COMPLETE,          /* ★ 设备驱动完成 → 对应 C */
	__BLK_TA_PLUG,              /* 插队（暂不下发，等待更多请求合并） */
	__BLK_TA_UNPLUG_IO,         /* 拔塞（I/O 触发，开始下发累积的请求） */
	__BLK_TA_UNPLUG_TIMER,      /* 定时器触发拔塞 */
	__BLK_TA_INSERT,            /* 插入 I/O 调度器队列 */
	__BLK_TA_SPLIT,             /* bio 被拆分（跨设备边界等） */
	__BLK_TA_BOUNCE,            /* 使用 bounce buffer（DMA 限制） */
	__BLK_TA_REMAP,             /* bio 被重映射（DM/LVM/分区） */
	__BLK_TA_ABORT,             /* request 被中止 */
	__BLK_TA_DRV_DATA,          /* 驱动私有数据 */
	__BLK_TA_CGROUP = 1 << 8,   /* 来自 cgroup 的请求 */
};


/* ======================================================================
 * 第三部分：通知事件
 * ======================================================================
 */
enum blktrace_notify {
	__BLK_TN_PROCESS = 0,       /* 建立 PID → 进程名映射 */
	__BLK_TN_TIMESTAMP,         /* 系统时钟时间戳 */
	__BLK_TN_MESSAGE,           /* 字符消息 */
	__BLK_TN_CGROUP = __BLK_TA_CGROUP,
};


/* ======================================================================
 * 第四部分：组合宏 — 将操作类型和类别标志编码为完整 action
 * ======================================================================
 *
 * 使用方式：内核代码中调用 __blk_add_trace(bt, ..., BLK_TA_ISSUE, ...)
 * 此时 what = BLK_TA_ISSUE = __BLK_TA_ISSUE | BLK_TC_ACT(BLK_TC_ISSUE)
 *           = 7 | (6 << 16) = 0x00060007
 *
 * blkparse 解析时会分别提取高 16 位和低 8 位来显示事件类型。
 */
#define BLK_TA_QUEUE        (__BLK_TA_QUEUE | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_BACKMERGE    (__BLK_TA_BACKMERGE | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_FRONTMERGE   (__BLK_TA_FRONTMERGE | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_GETRQ        (__BLK_TA_GETRQ | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_SLEEPRQ      (__BLK_TA_SLEEPRQ | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_REQUEUE      (__BLK_TA_REQUEUE | BLK_TC_ACT(BLK_TC_REQUEUE))
#define BLK_TA_ISSUE        (__BLK_TA_ISSUE | BLK_TC_ACT(BLK_TC_ISSUE))     /* ★ D */
#define BLK_TA_COMPLETE     (__BLK_TA_COMPLETE | BLK_TC_ACT(BLK_TC_COMPLETE)) /* ★ C */
#define BLK_TA_PLUG         (__BLK_TA_PLUG | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_UNPLUG_IO    (__BLK_TA_UNPLUG_IO | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_UNPLUG_TIMER (__BLK_TA_UNPLUG_TIMER | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_INSERT       (__BLK_TA_INSERT | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_SPLIT        (__BLK_TA_SPLIT)
#define BLK_TA_BOUNCE       (__BLK_TA_BOUNCE)
#define BLK_TA_REMAP        (__BLK_TA_REMAP | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_ABORT        (__BLK_TA_ABORT | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_DRV_DATA     (__BLK_TA_DRV_DATA | BLK_TC_ACT(BLK_TC_DRV_DATA))

#define BLK_TN_PROCESS      (__BLK_TN_PROCESS | BLK_TC_ACT(BLK_TC_NOTIFY))
#define BLK_TN_TIMESTAMP    (__BLK_TN_TIMESTAMP | BLK_TC_ACT(BLK_TC_NOTIFY))
#define BLK_TN_MESSAGE      (__BLK_TN_MESSAGE | BLK_TC_ACT(BLK_TC_NOTIFY))


/* ======================================================================
 * 第五部分：Magic 和 Version
 * ======================================================================
 *
 * 用于验证二进制文件的合法性。
 * 0x656174 = ASCII "eat" → "eat my shorts"（Jens Axboe 的幽默）
 */
#define BLK_IO_TRACE_MAGIC  0x65617400
#define BLK_IO_TRACE_VERSION    0x07


/* ======================================================================
 * 第六部分：★ 核心结构体 blk_io_trace
 * ======================================================================
 *
 * 这是 blktrace 记录的每一个 I/O 事件的二进制格式。
 * 内核 __blk_add_trace() 填充它，用户态 blkparse/btt 解析它。
 *
 * 在磁盘上的二进制文件中，每个事件就是一个连续的 blk_io_trace 结构，
 * 后面可能跟随 pdu_len 字节的附加数据（如 SCSI CDB 命令）。
 *
 * 结构体大小：固定 48 字节（不含 pdu）
 *
 *   ┌─────────────────────────────────────────────────────┐
 *   │ magic(4) sequence(4) time(8) sector(8) bytes(4)     │
 *   │ action(4) pid(4) device(4) cpu(4) error(2) pdu(2)   │
 *   └─────────────────────────────────────────────────────┘
 *   │← pdu_len 字节的附加数据 →│
 */
struct blk_io_trace {
	__u32 magic;        /* 魔数 + 版本号：BLK_IO_TRACE_MAGIC | version */
	__u32 sequence;     /* 事件序号（per-CPU 递增） */
	__u64 time;         /* ★ 时间戳（纳秒，ktime_get()） */
	__u64 sector;       /* ★ 起始扇区号（512 字节扇区） */
	__u32 bytes;        /* ★ I/O 大小（字节） */
	__u32 action;       /* ★ 事件类型（位掩码，见上面的 BLK_TA_* 和 BLK_TC_*） */
	__u32 pid;          /* 发起 I/O 的进程 PID */
	__u32 device;       /* 设备号（major << 20 | minor） */
	__u32 cpu;          /* 事件发生在哪个 CPU */
	__u16 error;        /* 完成错误码（0 = 成功） */
	__u16 pdu_len;      /* 附加数据长度（紧跟在结构体后面） */
	/* 如果有 cgroup，cgroup id 存储在 pdu 区域的前 8 字节 */
};


/* ======================================================================
 * 第七部分：Remap 事件附加数据
 * ======================================================================
 *
 * 当 bio 被 DM/LVM 重映射时，附加在 blk_io_trace 后面的数据。
 * 记录了重映射前后的设备和扇区。
 */
struct blk_io_trace_remap {
	__be32 device_from;     /* 原始设备号 */
	__be32 device_to;       /* 目标设备号 */
	__be64 sector_from;     /* 原始扇区号 */
};


/* ======================================================================
 * 第八部分：追踪状态
 * ======================================================================
 */
enum {
	Blktrace_setup = 1,     /* 已配置，等待启动 */
	Blktrace_running,       /* 正在追踪 */
	Blktrace_stopped,       /* 已停止 */
};


#define BLKTRACE_BDEV_SIZE  32


/* ======================================================================
 * 第九部分：★ 用户态配置结构体
 * ======================================================================
 *
 * 用户态 blktrace 工具通过 BLKTRACESETUP ioctl 将此结构传给内核。
 * 内核在 do_blk_trace_setup() 中读取这些参数来初始化追踪。
 */
struct blk_user_trace_setup {
	char name[BLKTRACE_BDEV_SIZE];  /* 输出：内核填充的设备名 */
	__u16 act_mask;                 /* 输入：要追踪的事件类别（位掩码） */
                                        /*   例如 0xFFFF = 追踪所有事件 */
                                        /*   例如 BLK_TC_ISSUE | BLK_TC_COMPLETE = 只追踪 D 和 C */
	__u32 buf_size;                 /* 输入：每个 relay subbuffer 大小（字节） */
                                        /*   默认 512KB */
	__u32 buf_nr;                   /* 输入：每个 CPU 的 subbuffer 数量 */
                                        /*   默认 4 */
	__u64 start_lba;                /* 输入：起始 LBA 过滤（0 = 不过滤） */
	__u64 end_lba;                  /* 输入：结束 LBA 过滤（0 = 不过滤） */
	__u32 pid;                      /* 输入：只追踪指定 PID（0 = 所有进程） */
};


/* ======================================================================
 * 第十部分：★ ioctl 命令定义（仅用户态版本）
 * ======================================================================
 *
 * 用户态 blktrace 工具通过这 4 个 ioctl 与内核通信：
 *
 *   BLKTRACESETUP   → 配置追踪参数（buffer 大小、事件过滤等）
 *   BLKTRACESTART   → 开始追踪
 *   BLKTRACESTOP    → 停止追踪
 *   BLKTRACETEARDOWN → 清理资源（释放 relay buffer、debugfs 文件）
 *
 * 调用顺序：SETUP → START → ... 运行中 ... → STOP → TEARDOWN
 */
#define BLKTRACESETUP   _IOWR(0x12, 115, struct blk_user_trace_setup)
#define BLKTRACESTART   _IO(0x12, 116)
#define BLKTRACESTOP    _IO(0x12, 117)
#define BLKTRACETEARDOWN _IO(0x12, 118)

#endif /* _UAPIBLKTRACE_H */
