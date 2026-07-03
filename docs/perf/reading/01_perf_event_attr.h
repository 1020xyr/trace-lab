/*
 * ======================================================================
 * Step 1: perf_event_attr — perf 事件的核心配置结构体
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/include/uapi/linux/perf_event.h (第 317-439 行)
 *
 * 本文件是 perf 子系统的用户态 ABI 头文件中最核心的部分：
 *   struct perf_event_attr — 描述"要采集什么事件、如何采样、采样哪些附加信息"
 *
 * 阅读要点：
 *   1. type + config 的组合如何确定一个具体事件
 *   2. sample_type 位掩码控制采样时携带哪些附加数据
 *   3. sample_period vs sample_freq 的区别
 *   4. 标志位（disabled / inherit / exclude_* / precise_ip 等）的含义
 *
 * 预计阅读时间：15 分钟
 *
 * ★ 调用关系：
 *   perf record -e cycles
 *       → parse_events() 解析 "-e cycles"
 *       → 填充 perf_event_attr { type=PERF_TYPE_HARDWARE, config=PERF_COUNT_HW_CPU_CYCLES }
 *       → sys_perf_event_open(&attr, pid, cpu, ...)
 *       → 内核 perf_event_open() 根据 attr 创建 perf_event
 * ======================================================================
 */


/* ======================================================================
 * ★ 事件类型枚举（type 字段）
 * ======================================================================
 *
 * perf_event_attr.type 指定事件的大类。
 * 每个大类下，由 config 字段进一步指定具体事件。
 */
enum perf_type_id {
	PERF_TYPE_HARDWARE			= 0,   /* ★ 硬件 PMU 事件（CPU 计数器） */
	PERF_TYPE_SOFTWARE			= 1,   /* ★ 软件事件（内核提供） */
	PERF_TYPE_TRACEPOINT			= 2,   /* ★ tracepoint 事件（ftrace 静态探针） */
	PERF_TYPE_HW_CACHE			= 3,   /* ★ 硬件缓存事件（三级组合编码） */
	PERF_TYPE_RAW				= 4,   /* ★ 原始 PMU 事件（直接写 MSR 编码） */
	PERF_TYPE_BREAKPOINT			= 5,   /* 硬件断点（数据/指令断点） */

	PERF_TYPE_MAX,				 /* non-ABI */
};

/*
 * ★ 事件类型速查表
 *
 * +------------------------+-----------------------------------+-------------------------------+
 * | type                   | 含义                              | 使用场景                       |
 * +------------------------+-----------------------------------+-------------------------------+
 * | PERF_TYPE_HARDWARE     | CPU 硬件计数器                    | perf record -e cycles          |
 * | PERF_TYPE_SOFTWARE     | 内核软件计数器                    | perf record -e cpu-clock       |
 * | PERF_TYPE_TRACEPOINT   | ftrace tracepoint                 | perf record -e sched:sched_switch |
 * | PERF_TYPE_HW_CACHE     | 缓存事件（L1/LLC/DTLB 等）       | perf record -e L1-dcache-load-misses |
 * | PERF_TYPE_RAW          | 原始 PMU 编码                    | perf record -e r003c（Intel 事件 0x3c） |
 * | PERF_TYPE_BREAKPOINT   | 硬件断点                          | perf record -e mem:0xADDR:rw  |
 * +------------------------+-----------------------------------+-------------------------------+
 */


/* ======================================================================
 * ★ 硬件事件编码（config 字段，当 type = PERF_TYPE_HARDWARE 时）
 * ======================================================================
 *
 * perf_event_attr.config 在 type=PERF_TYPE_HARDWARE 时，
 * 取值来自此枚举，对应 CPU 的通用性能计数器。
 */
enum perf_hw_id {
	PERF_COUNT_HW_CPU_CYCLES		= 0,   /* ★ CPU 时钟周期 */
	PERF_COUNT_HW_INSTRUCTIONS		= 1,   /* ★ 退休指令数 */
	PERF_COUNT_HW_CACHE_REFERENCES		= 2,   /* 缓存引用（通常是 LLC 访问） */
	PERF_COUNT_HW_CACHE_MISSES		= 3,   /* ★ 缓存未命中（通常是 LLC miss） */
	PERF_COUNT_HW_BRANCH_INSTRUCTIONS	= 4,   /* 分支指令数 */
	PERF_COUNT_HW_BRANCH_MISSES		= 5,   /* ★ 分支预测失败数 */
	PERF_COUNT_HW_BUS_CYCLES		= 6,   /* 总线周期 */
	PERF_COUNT_HW_STALLED_CYCLES_FRONTEND	= 7,   /* 前端停顿周期 */
	PERF_COUNT_HW_STALLED_CYCLES_BACKEND	= 8,   /* 后端停顿周期 */
	PERF_COUNT_HW_REF_CPU_CYCLES		= 9,   /* 参考 CPU 周期（不受频率调节影响） */

	PERF_COUNT_HW_MAX,
};

/*
 * ★ perf record 命令行与 config 的映射关系
 *
 * 命令行用法                  → type                 → config
 * -e cycles                  → PERF_TYPE_HARDWARE   → PERF_COUNT_HW_CPU_CYCLES (0)
 * -e instructions            → PERF_TYPE_HARDWARE   → PERF_COUNT_HW_INSTRUCTIONS (1)
 * -e cache-misses            → PERF_TYPE_HARDWARE   → PERF_COUNT_HW_CACHE_MISSES (3)
 * -e branch-misses           → PERF_TYPE_HARDWARE   → PERF_COUNT_HW_BRANCH_MISSES (5)
 * -e cpu-clock               → PERF_TYPE_SOFTWARE   → PERF_COUNT_SW_CPU_CLOCK (0)
 * -e task-clock              → PERF_TYPE_SOFTWARE   → PERF_COUNT_SW_TASK_CLOCK (1)
 * -e page-faults             → PERF_TYPE_SOFTWARE   → PERF_COUNT_SW_PAGE_FAULTS (2)
 * -e context-switches        → PERF_TYPE_SOFTWARE   → PERF_COUNT_SW_CONTEXT_SWITCHES (3)
 * -e cpu-migrations          → PERF_TYPE_SOFTWARE   → PERF_COUNT_SW_CPU_MIGRATIONS (4)
 */


/* ======================================================================
 * ★ 缓存事件三级编码（type = PERF_TYPE_HW_CACHE 时的 config 编码规则）
 * ======================================================================
 *
 * config = (cache_result) << 16 | (cache_op) << 8 | cache_id
 *
 * 例如 L1-dcache-load-misses:
 *   cache_id     = PERF_COUNT_HW_CACHE_L1D (0)
 *   cache_op     = PERF_COUNT_HW_CACHE_OP_READ (0)
 *   cache_result = PERF_COUNT_HW_CACHE_RESULT_MISS (1)
 *   config = (1 << 16) | (0 << 8) | 0 = 0x10000
 */
enum perf_hw_cache_id {
	PERF_COUNT_HW_CACHE_L1D			= 0,   /* L1 数据缓存 */
	PERF_COUNT_HW_CACHE_L1I			= 1,   /* L1 指令缓存 */
	PERF_COUNT_HW_CACHE_LL			= 2,   /* Last Level Cache */
	PERF_COUNT_HW_CACHE_DTLB		= 3,   /* 数据 TLB */
	PERF_COUNT_HW_CACHE_ITLB		= 4,   /* 指令 TLB */
	PERF_COUNT_HW_CACHE_BPU			= 5,   /* 分支预测单元 */
	PERF_COUNT_HW_CACHE_NODE		= 6,   /* 本地内存节点 */
};

enum perf_hw_cache_op_id {
	PERF_COUNT_HW_CACHE_OP_READ		= 0,   /* 读操作 */
	PERF_COUNT_HW_CACHE_OP_WRITE		= 1,   /* 写操作 */
	PERF_COUNT_HW_CACHE_OP_PREFETCH		= 2,   /* 预取 */
};

enum perf_hw_cache_op_result_id {
	PERF_COUNT_HW_CACHE_RESULT_ACCESS	= 0,   /* 访问次数 */
	PERF_COUNT_HW_CACHE_RESULT_MISS		= 1,   /* 未命中次数 */
};


/* ======================================================================
 * ★ 软件事件编码（config 字段，当 type = PERF_TYPE_SOFTWARE 时）
 * ======================================================================
 */
enum perf_sw_ids {
	PERF_COUNT_SW_CPU_CLOCK			= 0,   /* ★ CPU 时钟（基于 hrtimer 采样） */
	PERF_COUNT_SW_TASK_CLOCK		= 1,   /* 任务时钟（仅统计任务运行时间） */
	PERF_COUNT_SW_PAGE_FAULTS		= 2,   /* 缺页中断 */
	PERF_COUNT_SW_CONTEXT_SWITCHES		= 3,   /* ★ 上下文切换 */
	PERF_COUNT_SW_CPU_MIGRATIONS		= 4,   /* CPU 迁移 */
	PERF_COUNT_SW_PAGE_FAULTS_MIN		= 5,   /* 次缺页 */
	PERF_COUNT_SW_PAGE_FAULTS_MAJ		= 6,   /* 主缺页 */
	PERF_COUNT_SW_ALIGNMENT_FAULTS		= 7,   /* 对齐错误 */
	PERF_COUNT_SW_EMULATION_FAULTS		= 8,   /* 仿真错误 */
	PERF_COUNT_SW_DUMMY			= 9,   /* ★ 虚拟事件（用于追踪 mmap/comm 等元数据） */
	PERF_COUNT_SW_BPF_OUTPUT		= 10,  /* BPF 输出事件 */
};


/* ======================================================================
 * ★ 采样类型位掩码（sample_type 字段）
 * ======================================================================
 *
 * sample_type 是一个位掩码，指定每个样本中包含哪些附加信息。
 * 例如 perf record -g 会设置 PERF_SAMPLE_CALLCHAIN。
 * perf report 根据 sample_type 来解析每个样本的格式。
 */
enum perf_event_sample_format {
	PERF_SAMPLE_IP				= 1U << 0,   /* ★ 指令指针（RIP/EIP） */
	PERF_SAMPLE_TID				= 1U << 1,   /* ★ 线程 ID（pid + tid） */
	PERF_SAMPLE_TIME			= 1U << 2,   /* ★ 时间戳（纳秒） */
	PERF_SAMPLE_ADDR			= 1U << 3,   /* 内存地址（用于 mem 事件） */
	PERF_SAMPLE_READ			= 1U << 4,   /* 计数器读值 */
	PERF_SAMPLE_CALLCHAIN			= 1U << 5,   /* ★★ 调用栈（-g 参数启用） */
	PERF_SAMPLE_ID				= 1U << 6,   /* 事件 ID */
	PERF_SAMPLE_CPU				= 1U << 7,   /* ★ CPU 编号 */
	PERF_SAMPLE_PERIOD			= 1U << 8,   /* 采样周期 */
	PERF_SAMPLE_STREAM_ID			= 1U << 9,   /* 流 ID */
	PERF_SAMPLE_RAW				= 1U << 10,  /* ★ 原始数据（tracepoint 参数） */
	PERF_SAMPLE_BRANCH_STACK		= 1U << 11,  /* ★ 分支栈（-j 参数，LBR 硬件） */
	PERF_SAMPLE_REGS_USER			= 1U << 12,  /* 用户态寄存器 */
	PERF_SAMPLE_STACK_USER			= 1U << 13,  /* 用户栈转储 */
	PERF_SAMPLE_WEIGHT			= 1U << 14,  /* 权重（用于延迟采样） */
	PERF_SAMPLE_DATA_SRC			= 1U << 15,  /* 数据来源（PEBS） */
	PERF_SAMPLE_IDENTIFIER			= 1U << 16,  /* 事件标识符 */
	PERF_SAMPLE_TRANSACTION			= 1U << 17,  /* 事务信息 */
	PERF_SAMPLE_REGS_INTR			= 1U << 18,  /* 中断时寄存器 */
	PERF_SAMPLE_PHYS_ADDR			= 1U << 19,  /* 物理地址 */
	PERF_SAMPLE_AUX				= 1U << 20,  /* AUX 区域数据 */
	PERF_SAMPLE_CGROUP			= 1U << 21,  /* cgroup ID */
};

/*
 * ★ perf record 命令行参数与 sample_type 的映射
 *
 * 命令行参数        → 设置的 sample_type 位
 * (默认)            → IP | TID | TIME | PERIOD
 * -g / --call-graph → + CALLCHAIN
 * --cpu             → + CPU
 * -j / --branch-any → + BRANCH_STACK
 * -d / --data       → + ADDR | DATA_SRC
 * --weight          → + WEIGHT
 * --raw-samples     → + RAW
 * --call-graph dwarf → + CALLCHAIN + REGS_USER + STACK_USER
 */


/* ======================================================================
 * ★ 读取格式（read_format 字段）
 * ======================================================================
 *
 * 控制 read(fd) 返回的数据格式，主要用于 perf stat 场景。
 */
enum perf_event_read_format {
	PERF_FORMAT_TOTAL_TIME_ENABLED		= 1U << 0,  /* 事件启用总时间 */
	PERF_FORMAT_TOTAL_TIME_RUNNING		= 1U << 1,  /* 事件实际运行时间 */
	PERF_FORMAT_ID				= 1U << 2,  /* 事件 ID */
	PERF_FORMAT_GROUP			= 1U << 3,  /* 组读取模式 */
};


/* ======================================================================
 * ★★★ 核心结构体：struct perf_event_attr ★★★
 * ======================================================================
 *
 * 这是 perf 子系统最重要的数据结构。
 * 用户态通过 sys_perf_event_open(&attr, pid, cpu, group_fd, flags)
 * 将此结构传递给内核，内核据此创建和配置 perf_event。
 *
 * ★ 数据流：
 *   用户态 perf record → 填充 attr → syscall → 内核 perf_event_open()
 *                                                  ↓
 *                                            分配 PMU 计数器
 *                                            配置采样参数
 *                                            关联到目标进程/CPU
 */
struct perf_event_attr {

	/*
	 * ★ type: 事件大类
	 *
	 * 取值来自 enum perf_type_id:
	 *   0 = HARDWARE (CPU PMU)
	 *   1 = SOFTWARE (内核软件)
	 *   2 = TRACEPOINT (ftrace)
	 *   3 = HW_CACHE (缓存事件)
	 *   4 = RAW (原始编码)
	 *   5 = BREAKPOINT (断点)
	 */
	__u32			type;

	/*
	 * size: attr 结构体大小
	 *
	 * 用于前向/后向兼容。内核通过 size 判断用户态传入的 attr 版本。
	 * 例如 VER0 = 64, VER5 = 112, 当前内核 = 120 字节。
	 */
	__u32			size;

	/*
	 * ★ config: 事件具体编码
	 *
	 * 含义取决于 type:
	 *   type=HARDWARE   → enum perf_hw_id (cycles=0, instructions=1, ...)
	 *   type=SOFTWARE   → enum perf_sw_ids (cpu-clock=0, task-clock=1, ...)
	 *   type=TRACEPOINT → tracepoint 的 ID (从 /sys/kernel/debug/tracing/events/ 获取)
	 *   type=HW_CACHE   → 三级编码: (result<<16) | (op<<8) | cache_id
	 *   type=RAW        → CPU 原始 PMU 编码 (如 Intel event 0x3c umask 0x08 → 0x083c)
	 */
	__u64			config;

	/*
	 * ★ sample_period / sample_freq: 采样频率控制
	 *
	 * 这两个字段是 union，共用同一块内存：
	 *
	 *   sample_period = N：每 N 个事件触发一次采样
	 *     例如 cycles, period=100000 → 每 100000 个 CPU 周期采样一次
	 *
	 *   sample_freq = F：每秒采样 F 次（内核自动调整 period）
	 *     例如 freq=4000 → 内核动态调节 period 使采样率约 4000Hz
	 *     ★ 这是 perf record 的默认模式（-F 参数）
	 *
	 * 由 freq 标志位决定使用哪个字段：
	 *   freq=0 → 使用 sample_period
	 *   freq=1 → 使用 sample_freq（默认 4000 Hz）
	 */
	union {
		__u64		sample_period;
		__u64		sample_freq;
	};

	/*
	 * ★ sample_type: 每个样本中包含哪些附加信息
	 *
	 * 位掩码，取值来自 enum perf_event_sample_format。
	 * 可以组合多个标志：IP | TID | TIME | CALLCHAIN 等。
	 * perf record -g 会加上 PERF_SAMPLE_CALLCHAIN。
	 */
	__u64			sample_type;

	/*
	 * read_format: read() 返回数据的格式
	 *
	 * 控制通过 read(fd) 读取计数器值时返回哪些信息。
	 * 主要用于 perf stat 场景，perf record 通常不关心。
	 */
	__u64			read_format;

	/* ======================================================================
	 * ★ 标志位区域（bit field）
	 * ======================================================================
	 *
	 * 以下 30+ 个标志位控制 perf_event 的各种行为。
	 * 每个标志位只有 1 bit，通过位域定义。
	 */
	__u64			disabled       :  1, /* 初始禁用（创建后需 ioctl 启用）
							   ★ perf record fork 模式默认设置 */

				inherit	       :  1, /* 子进程继承事件
							   ★ 追踪 fork 出的子进程时自动设置 */

				pinned	       :  1, /* 事件必须始终在 PMU 上运行
							   如果 PMU 计数器不够，该事件不会被轮换 */

				exclusive      :  1, /* 独占 PMU（同组事件独占硬件计数器） */

				exclude_user   :  1, /* 不统计用户态事件
							   ★ --exclude-user 参数 */

				exclude_kernel :  1, /* 不统计内核态事件
							   ★ --exclude-kernel 参数 */

				exclude_hv     :  1, /* 不统计 hypervisor 事件 */

				exclude_idle   :  1, /* 不统计 idle 时的事件 */

				mmap           :  1, /* 记录 mmap 事件（内存映射创建）
							   ★ 用于后续地址到 DSO 的映射解析 */

				comm	       :  1, /* 记录 comm 事件（进程名变更）
							   ★ 用于 pid → 进程名的映射 */

				freq           :  1, /* ★ 使用 sample_freq 而非 sample_period
							   perf record -F 4000 → freq=1, sample_freq=4000 */

				inherit_stat   :  1, /* 子进程独立统计（每个子任务有独立计数） */

				enable_on_exec :  1, /* exec 时自动启用
							   ★ perf record 追踪命令时设置此位
							   避免 exec 前的噪音 */

				task           :  1, /* 追踪 fork/exit 事件 */

				watermark      :  1, /* 使用水位线唤醒（而非事件计数）
							   wakeup_watermark 字段生效 */

				/*
				 * ★ precise_ip: 指令精度级别
				 *
				 *   0 - IP 可以有任意偏差（skid）
				 *   1 - IP 必须有恒定偏差
				 *   2 - IP 要求零偏差（需要 PEBS/IBS 硬件支持）
				 *   3 - IP 必须零偏差
				 *
				 * perf record -e cycles:p 中的 ':p' 后缀设置 precise_ip=2
				 * perf record -e cycles:pp → precise_ip=2（同上）
				 * 零偏差需要 Intel PEBS 或 AMD IBS 硬件支持
				 */
				precise_ip     :  2,

				mmap_data      :  1, /* 记录非执行 mmap 事件
							   （数据映射：malloc、mmap 等） */

				sample_id_all  :  1, /* 所有事件类型都附加 sample_type 信息
							   ★ 确保非采样事件也能被正确关联 */

				exclude_host   :  1, /* 不统计 host（非虚拟机）事件 */
				exclude_guest  :  1, /* 不统计 guest（虚拟机）事件 */

				exclude_callchain_kernel : 1, /* 调用栈中排除内核帧 */
				exclude_callchain_user   : 1, /* 调用栈中排除用户帧 */

				mmap2          :  1, /* ★ 使用 MMAP2 格式（含 inode 信息）
							   比 MMAP 多了 maj/min/dev/ino 字段
							   能精确识别文件的身份 */

				comm_exec      :  1, /* 标记由 exec 产生的 comm 事件 */
				use_clockid    :  1, /* 使用 clockid 字段指定的时钟源 */
				context_switch :  1, /* 记录上下文切换事件 */
				write_backward :  1, /* 从后往前写环形缓冲区（覆盖模式） */
				namespaces     :  1, /* 记录 namespace 信息 */
				ksymbol        :  1, /* 记录内核符号（kallsyms 加载） */
				bpf_event      :  1, /* 记录 BPF 程序加载/卸载 */
				aux_output     :  1, /* 生成 AUX 记录 */
				cgroup         :  1, /* 记录 cgroup 信息 */
				text_poke      :  1, /* 记录代码修改（live patch） */
				__reserved_1   : 30;

	/*
	 * ★ 唤醒控制
	 *
	 * wakeup_events = N: 每 N 个事件唤醒一次用户态（配合 poll）
	 * wakeup_watermark = B: 环形缓冲区中积累 B 字节后唤醒（需 watermark=1）
	 *
	 * perf record 默认 wakeup_events=1，即每个事件都唤醒（高频采样时由 poll 节流）
	 */
	union {
		__u32		wakeup_events;
		__u32		wakeup_watermark;
	};

	/*
	 * 断点相关字段（type = PERF_TYPE_BREAKPOINT 时使用）
	 * 对于其他类型的事件，这些字段作为 config1/config2 的扩展
	 */
	__u32			bp_type;            /* 断点类型（读/写/执行） */
	union {
		__u64		bp_addr;            /* 断点地址 */
		__u64		kprobe_func;        /* kprobe 函数名指针 */
		__u64		uprobe_path;        /* uprobe 文件路径指针 */
		__u64		config1;            /* config 扩展字段 1 */
	};
	union {
		__u64		bp_len;             /* 断点长度 */
		__u64		kprobe_addr;        /* kprobe 地址 */
		__u64		probe_offset;       /* probe 偏移量 */
		__u64		config2;            /* config 扩展字段 2 */
	};

	/*
	 * branch_sample_type: 分支采样过滤
	 *
	 * 当 sample_type 包含 PERF_SAMPLE_BRANCH_STACK 时，
	 * 此字段控制记录哪些类型的分支。
	 * 例如：PERF_SAMPLE_BRANCH_ANY（所有分支）
	 *       PERF_SAMPLE_BRANCH_ANY_CALL（仅函数调用）
	 *       PERF_SAMPLE_BRANCH_ANY_RETURN（仅函数返回）
	 * 对应 perf record -j any_call 等参数
	 */
	__u64	branch_sample_type;

	/*
	 * sample_regs_user: 用户态寄存器转储掩码
	 *
	 * 指定要转储的用户态寄存器集合（位掩码）。
	 * perf record --call-graph dwarf 需要此字段，
	 * 用于 DWARF CFI 回溯展开调用栈。
	 */
	__u64	sample_regs_user;

	/*
	 * sample_stack_user: 用户栈转储大小
	 *
	 * 每次采样时转储多少字节的用户栈。
	 * perf record --call-graph dwarf,8192 → sample_stack_user = 8192
	 */
	__u32	sample_stack_user;

	__s32	clockid;                /* 时钟源（当 use_clockid=1 时） */

	__u64	sample_regs_intr;       /* 中断时寄存器转储掩码 */
	__u32	aux_watermark;          /* AUX 区域水位线 */
	__u16	sample_max_stack;       /* 调用栈最大深度（限制帧数） */
	__u16	__reserved_2;
	__u32	aux_sample_size;        /* AUX 采样大小 */
	__u32	__reserved_3;
};


/* ======================================================================
 * ★ perf_event_mmap_page — 环形缓冲区的共享元数据页
 * ======================================================================
 *
 * perf_event_open() 返回的 fd 通过 mmap() 映射后，
 * 第一页是 struct perf_event_mmap_page（元数据），
 * 后续页是环形缓冲区数据区。
 *
 * ★ 布局：
 *   +--------------------------+
 *   |  perf_event_mmap_page    |  ← 第 0 页（元数据）
 *   |  (version, lock, index,  |
 *   |   offset, time_enabled,  |
 *   |   time_running, ...)     |
 *   +--------------------------+
 *   |  环形缓冲区数据区         |  ← 第 1~N 页
 *   |  (perf_event_header +    |
 *   |   sample data)           |
 *   +--------------------------+
 *
 * 用户态通过以下流程读取计数器值（无需 syscall）：
 *   1. 读取 pc->lock（seqlock）
 *   2. 读取 pc->index（PMU 计数器编号）
 *   3. 如果 cap_user_rdpmc，使用 rdpmc 指令直接读 PMU
 *   4. 读取 pc->offset，与 PMU 值相加得到最终计数
 *   5. 再次读取 pc->lock，如果变化则重试
 */
struct perf_event_mmap_page {
	__u32	version;
	__u32	compat_version;

	__u32	lock;			/* seqlock 同步锁 */
	__u32	index;			/* 硬件事件标识符 */
	__s64	offset;			/* 加到硬件计数器值上 */
	__u64	time_enabled;		/* 事件激活总时间 */
	__u64	time_running;		/* 事件在 CPU 上运行的时间 */

	union {
		__u64	capabilities;
		struct {
			__u64	cap_bit0		: 1,
				cap_bit0_is_deprecated	: 1,
				cap_user_rdpmc		: 1, /* ★ 可用 rdpmc 指令读取 */
				cap_user_time		: 1,
				cap_user_time_zero	: 1,
				cap_user_time_short	: 1,
				cap_____res		: 58;
		};
	};

	__u16	pmc_width;		/* PMU 计数器位宽 */
	__u16	time_shift;
	__u32	time_mult;
	__u64	time_offset;
	__u64	time_zero;
	__u32	size;
	/* ... 后续字段省略 ... */
};


/* ======================================================================
 * ★ ioctl 控制命令
 * ======================================================================
 *
 * 通过 ioctl(fd, cmd) 控制已打开的 perf_event。
 */
#define PERF_EVENT_IOC_ENABLE			_IO ('$', 0)   /* 启用事件 */
#define PERF_EVENT_IOC_DISABLE			_IO ('$', 1)   /* 禁用事件 */
#define PERF_EVENT_IOC_REFRESH			_IO ('$', 2)   /* 刷新计数器 */
#define PERF_EVENT_IOC_RESET			_IO ('$', 3)   /* 重置计数器 */
#define PERF_EVENT_IOC_PERIOD			_IOW('$', 4, __u64)  /* 修改采样周期 */
#define PERF_EVENT_IOC_SET_OUTPUT		_IO ('$', 5)   /* 共享输出缓冲区 */
#define PERF_EVENT_IOC_SET_FILTER		_IOW('$', 6, char *) /* 设置 tracepoint 过滤器 */
#define PERF_EVENT_IOC_ID			_IOR('$', 7, __u64 *) /* 获取事件 ID */
#define PERF_EVENT_IOC_SET_BPF			_IOW('$', 8, __u32)  /* 附加 BPF 程序 */


/*
 * ★ 总结：perf_event_attr 的完整字段映射表
 *
 * +------------------+---------------------------------------+---------------------------------+
 * | 字段             | 含义                                  | perf record 对应参数             |
 * +------------------+---------------------------------------+---------------------------------+
 * | type             | 事件大类                              | -e cycles (HARDWARE)             |
 * | size             | 结构体大小（版本兼容）                 | （自动填充）                      |
 * | config           | 事件编码                              | -e instructions (config=1)        |
 * | sample_period    | 每 N 个事件采样一次                    | -c 100000                         |
 * | sample_freq      | 每秒采样 N 次                          | -F 4000（默认）                   |
 * | sample_type      | 样本包含的附加信息                     | -g (CALLCHAIN), -d (DATA_SRC)     |
 * | read_format      | read() 返回格式                        | （perf stat 使用）                |
 * | disabled         | 初始禁用                              | fork 模式自动设置                  |
 * | inherit          | 子进程继承                             | 默认启用                          |
 * | exclude_user     | 排除用户态                             | --exclude-user                    |
 * | exclude_kernel   | 排除内核态                             | --exclude-kernel                  |
 * | freq             | 使用频率模式                           | -F (默认启用)                      |
 * | enable_on_exec   | exec 时自动启用                        | 追踪命令时默认设置                  |
 * | mmap / mmap2     | 记录内存映射事件                       | 默认启用                          |
 * | comm             | 记录进程名变更                         | 默认启用                          |
 * | precise_ip       | 指令精度（0-3）                        | :p / :pp 后缀                     |
 * | branch_sample_type | 分支采样过滤                         | -j any_call                       |
 * | sample_regs_user | 用户态寄存器                           | --call-graph dwarf                |
 * | sample_stack_user| 用户栈转储大小                         | --call-graph dwarf,8192           |
 * | wakeup_events    | 唤醒事件数                             | --wakeup-events                   |
 * | watermark        | 水位线唤醒模式                         | --wakeup-watermark                |
 * +------------------+---------------------------------------+---------------------------------+
 *
 * ★ 调用链总结：
 *   perf record -e cycles -g -F 4000 -- ls
 *       ↓
 *   parse_events() 解析 "-e cycles"
 *       ↓
 *   填充 perf_event_attr:
 *     type = PERF_TYPE_HARDWARE (0)
 *     config = PERF_COUNT_HW_CPU_CYCLES (0)
 *     sample_type = IP|TID|TIME|CALLCHAIN
 *     sample_freq = 4000
 *     freq = 1
 *     mmap = 1, comm = 1
 *     disabled = 1, enable_on_exec = 1
 *       ↓
 *   sys_perf_event_open(&attr, pid=-1, cpu=0, group_fd=-1, 0)
 *       ↓
 *   内核 perf_event_open() → 分配 PMU 计数器 → 等待 exec 启用
 */
