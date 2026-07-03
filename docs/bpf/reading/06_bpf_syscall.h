/*
 * ======================================================================
 * Step 6: 内核 bpf.h — BPF 系统调用接口（用户态与内核的唯一通道）
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/include/uapi/linux/bpf.h (1500+ 行)
 *           src/linux-5.10/include/linux/bpf.h (内部定义)
 *
 * 本文件是 BPF 子系统的用户空间 API 定义。所有 BPF 操作——创建 Map、
 * 加载程序、读写 Map 元素——都通过 bpf() 系统调用完成。
 *
 * 我们只关注四个核心部分：
 *   1. enum bpf_cmd    — 系统调用的命令号（做什么操作）
 *   2. union bpf_attr   — 系统调用的参数（传递什么数据）
 *   3. enum bpf_prog_type — BPF 程序类型（挂载到哪里）
 *   4. enum bpf_map_type  — Map 类型（用什么数据结构）
 *
 * 预计阅读时间：20 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ 核心 #1: enum bpf_cmd — bpf() 系统调用的命令号
 * ======================================================================
 *
 * bpf() 是 BPF 子系统的唯一入口，类似于 ioctl()。
 * 第一个参数 cmd 决定执行什么操作，第二个参数 attr 传递具体参数。
 *
 * 用户态调用方式：
 *   int bpf(int cmd, union bpf_attr *attr, unsigned int size);
 *   // 实际通过 syscall(__NR_bpf, cmd, &attr, sizeof(attr)) 调用
 *
 * ★ 重要：bpf() 系统调用需要 CAP_BPF 或 CAP_SYS_ADMIN 能力。
 */
enum bpf_cmd {
    /* ---- Map 操作（5 个基本命令）---- */
    BPF_MAP_CREATE,             /* 创建一个 Map，返回 fd
                                 * attr 使用 map_create 字段
                                 * 类比：open() 创建文件 */
    BPF_MAP_LOOKUP_ELEM,        /* 根据 key 查找 Map 中的 value
                                 * attr 使用 map_elem 字段（map_fd, key, value）
                                 * 类比：hash_map[key] */
    BPF_MAP_UPDATE_ELEM,        /* 创建或更新 Map 中的元素
                                 * flags: BPF_ANY(创建或更新) /
                                 *        BPF_NOEXIST(仅创建) /
                                 *        BPF_EXIST(仅更新) */
    BPF_MAP_DELETE_ELEM,        /* 根据 key 删除 Map 中的元素 */
    BPF_MAP_GET_NEXT_KEY,       /* 获取指定 key 的下一个 key（用于遍历）
                                 * ★ 遍历 Map 的标准方式：
                                 *   key = NULL → 获取第一个 key
                                 *   然后循环 get_next_key 直到返回错误 */

    /* ---- 程序操作 ---- */
    BPF_PROG_LOAD,              /* ★ 加载 BPF 程序到内核
                                 * 这是最复杂的命令，会触发 verifier 验证
                                 * attr 使用 prog_load 字段：
                                 *   prog_type  — 程序类型（kprobe/tracepoint/...）
                                 *   insns      — BPF 指令数组
                                 *   insn_cnt   — 指令数量
                                 *   license    — 许可证字符串（"GPL" 等）
                                 *   log_buf    — verifier 日志缓冲区
                                 *   log_level  — verifier 详细程度
                                 * 成功返回 fd，失败可通过 log_buf 查看原因 */

    /* ---- BPF 对象文件系统操作 ---- */
    BPF_OBJ_PIN,                /* 将 BPF 对象 pin 到 bpffs 文件系统
                                 * 例如 /sys/fs/bpf/my_map
                                 * 即使创建进程退出，对象仍然存在 */
    BPF_OBJ_GET,                /* 从 bpffs 路径获取已 pin 的对象 fd */

    /* ---- 程序附着操作 ---- */
    BPF_PROG_ATTACH,            /* ★ 将 BPF 程序附着到 cgroup/网络钩子
                                 * target_fd    — 目标 cgroup fd
                                 * attach_bpf_fd — BPF 程序 fd
                                 * attach_type   — 附着类型 */
    BPF_PROG_DETACH,            /* 从目标分离 BPF 程序 */

    /* ---- 测试与查询 ---- */
    BPF_PROG_TEST_RUN,          /* 在测试模式下运行 BPF 程序（不需要附着）
                                 * 常用于单元测试和性能基准测试 */
    BPF_PROG_GET_NEXT_ID,       /* 枚举系统中所有 BPF 程序（通过 ID） */
    BPF_MAP_GET_NEXT_ID,        /* 枚举系统中所有 Map（通过 ID） */
    BPF_PROG_GET_FD_BY_ID,      /* 通过程序 ID 获取 fd */
    BPF_MAP_GET_FD_BY_ID,       /* 通过 Map ID 获取 fd */
    BPF_OBJ_GET_INFO_BY_FD,     /* 通过 fd 获取对象信息（程序名、Map 类型等） */
    BPF_PROG_QUERY,             /* 查询 cgroup 上附着的 BPF 程序列表 */

    /* ---- 高级功能 ---- */
    BPF_RAW_TRACEPOINT_OPEN,    /* 打开 raw tracepoint 并附着 BPF 程序
                                 * ★ 这是 bpftrace 附着 kprobe/tracepoint
                                 *   的底层机制之一 */
    BPF_BTF_LOAD,               /* 加载 BTF（BPF Type Format）数据 */
    BPF_BTF_GET_FD_BY_ID,       /* 通过 BTF ID 获取 fd */
    BPF_TASK_FD_QUERY,          /* 查询进程打开的 BPF 相关文件 */

    /* ---- 批量操作（提高性能）---- */
    BPF_MAP_LOOKUP_AND_DELETE_ELEM, /* 原子地查找并删除元素（队列/栈） */
    BPF_MAP_FREEZE,             /* 冻结 Map（禁止用户态写入） */
    BPF_BTF_GET_NEXT_ID,        /* 枚举 BTF 对象 */
    BPF_MAP_LOOKUP_BATCH,       /* ★ 批量查找（一次多个 key/value） */
    BPF_MAP_LOOKUP_AND_DELETE_BATCH,
    BPF_MAP_UPDATE_BATCH,       /* 批量更新 */
    BPF_MAP_DELETE_BATCH,       /* 批量删除 */

    /* ---- BPF Link（新一代附着机制）---- */
    BPF_LINK_CREATE,            /* ★ 创建 BPF link（将程序附着到钩子）
                                 * link 是 5.7+ 内核引入的新附着方式
                                 * 比 prog_attach 更灵活，支持自动清理 */
    BPF_LINK_UPDATE,            /* 更新 link 中的 BPF 程序（原子替换） */
    BPF_LINK_GET_FD_BY_ID,      /* 通过 link ID 获取 fd */
    BPF_LINK_GET_NEXT_ID,       /* 枚举所有 link */

    /* ---- 统计与迭代器 ---- */
    BPF_ENABLE_STATS,           /* 启用 BPF 运行时统计（执行时间/次数） */
    BPF_ITER_CREATE,            /* 创建 BPF 迭代器（遍历内核数据结构） */
    BPF_LINK_DETACH,            /* 分离 link */
    BPF_PROG_BIND_MAP,          /* 将 Map 绑定到程序 */
};


/* ======================================================================
 * ★ 核心 #2: enum bpf_map_type — Map 类型枚举
 * ======================================================================
 *
 * BPF Map 是 BPF 程序与用户空间通信的主要方式，也是 BPF 程序
 * 之间共享数据的手段。不同类型的 Map 适用于不同场景。
 *
 * 类比：Map 就像一个内核态的"数据库"，BPF 程序在里面读写数据，
 *       用户态程序通过 bpf() 系统调用读取结果。
 *
 * +---------------------------+--------------------------------------------+
 * | Map 类型                  | 用途和特性                                  |
 * +---------------------------+--------------------------------------------+
 * | HASH                      | 通用哈希表，最常用                          |
 * | ARRAY                     | 固定大小数组，按索引访问，零拷贝             |
 * | PERCPU_HASH / PERCPU_ARRAY| 每个 CPU 独立副本，避免锁竞争               |
 * | LRU_HASH                  | 带 LRU 淘汰的哈希表，适合有界数据           |
 * | LPM_TRIE                  | 最长前缀匹配树，适合 IP 路由/CIDR 匹配      |
 * | PROG_ARRAY                | 程序数组，用于 BPF 程序间 tail call         |
 * | PERF_EVENT_ARRAY          | ★ perf 事件输出，BPF → 用户态的主要通道     |
 * | RINGBUF                   | ★ 新一代环形缓冲区，替代 perf_event_array   |
 * | STACK_TRACE               | 存储调用栈（函数返回地址列表）               |
 * | SOCKMAP / SOCKHASH        | 存储 socket 引用，用于网络策略              |
 * | ARRAY_OF_MAPS / HASH_OF_MAPS | Map 嵌套（Map 中的值是另一个 Map）       |
 * | QUEUE / STACK             | FIFO 队列 / LIFO 栈（无 key，只有 value）   |
 * +---------------------------+--------------------------------------------+
 */
enum bpf_map_type {
    BPF_MAP_TYPE_UNSPEC,            /* 未指定（错误值） */
    BPF_MAP_TYPE_HASH,              /* ★ 哈希表 — 最通用的 Map 类型 */
    BPF_MAP_TYPE_ARRAY,             /* 数组 — 固定大小，按索引 O(1) 访问 */
    BPF_MAP_TYPE_PROG_ARRAY,        /* 程序数组 — 用于 tail call 跳转 */
    BPF_MAP_TYPE_PERF_EVENT_ARRAY,  /* ★ perf 事件数组 — BPF 输出到用户态 */
    BPF_MAP_TYPE_PERCPU_HASH,       /* per-CPU 哈希表 — 每个 CPU 独立副本 */
    BPF_MAP_TYPE_PERCPU_ARRAY,      /* per-CPU 数组 */
    BPF_MAP_TYPE_STACK_TRACE,       /* 调用栈存储 — 保存内核/用户态栈帧 */
    BPF_MAP_TYPE_CGROUP_ARRAY,      /* cgroup 数组 */
    BPF_MAP_TYPE_LRU_HASH,          /* ★ LRU 哈希表 — 自动淘汰最久未访问 */
    BPF_MAP_TYPE_LRU_PERCPU_HASH,   /* per-CPU LRU 哈希表 */
    BPF_MAP_TYPE_LPM_TRIE,          /* 最长前缀匹配树 — IP 路由查找 */
    BPF_MAP_TYPE_ARRAY_OF_MAPS,     /* 数组嵌套 Map */
    BPF_MAP_TYPE_HASH_OF_MAPS,      /* 哈希表嵌套 Map */
    BPF_MAP_TYPE_DEVMAP,            /* 网络设备映射 — XDP 重定向用 */
    BPF_MAP_TYPE_SOCKMAP,           /* socket 映射 — 网络策略 */
    BPF_MAP_TYPE_CPUMAP,            /* CPU 映射 — XDP 负载均衡 */
    BPF_MAP_TYPE_XSKMAP,            /* AF_XDP socket 映射 */
    BPF_MAP_TYPE_SOCKHASH,          /* socket 哈希表 */
    BPF_MAP_TYPE_CGROUP_STORAGE,    /* cgroup 本地存储 */
    BPF_MAP_TYPE_REUSEPORT_SOCKARRAY, /* reuseport socket 数组 */
    BPF_MAP_TYPE_PERCPU_CGROUP_STORAGE,
    BPF_MAP_TYPE_QUEUE,             /* FIFO 队列 */
    BPF_MAP_TYPE_STACK,             /* LIFO 栈 */
    BPF_MAP_TYPE_SK_STORAGE,        /* socket 本地存储 */
    BPF_MAP_TYPE_DEVMAP_HASH,       /* 网络设备哈希映射 */
    BPF_MAP_TYPE_STRUCT_OPS,        /* 结构体操作（替换内核回调） */
    BPF_MAP_TYPE_RINGBUF,           /* ★ 环形缓冲区 — 5.8+，替代 perf_event */
    BPF_MAP_TYPE_INODE_STORAGE,     /* inode 本地存储 */
};


/* ======================================================================
 * ★ 核心 #3: enum bpf_prog_type — BPF 程序类型
 * ======================================================================
 *
 * 程序类型决定了 BPF 程序可以挂载到内核的哪个位置，以及它能访问
 * 什么样的上下文（context）。
 *
 * +----------------------------+----------------------+------------------------+
 * | 程序类型                    | 挂载点                | 上下文                 |
 * +----------------------------+----------------------+------------------------+
 * | SOCKET_FILTER              | socket 收包路径       | struct __sk_buff       |
 * | KPROBE                     | ★ 内核函数入口/返回   | struct pt_regs         |
 * | TRACEPOINT                 | ★ 内核 tracepoint     | tracepoint 参数结构     |
 * | RAW_TRACEPOINT             | raw tracepoint        | 原始参数（无格式化）     |
 * | PERF_EVENT                 | ★ perf 事件           | struct bpf_perf_event  |
 * | XDP                        | 网卡驱动收包最早点     | struct xdp_md          |
 * | SCHED_CLS / SCHED_ACT      | TC 流量分类/动作      | struct __sk_buff       |
 * | CGROUP_SKB                 | cgroup 网络过滤       | struct __sk_buff       |
 * | CGROUP_SOCK                | cgroup socket 操作    | struct bpf_sock        |
 * | TRACING                    | ★ fentry/fexit/iter   | 函数参数（BTF）         |
 * | LSM                        | Linux 安全模块钩子     | 取决于 LSM hook        |
 * | STRUCT_OPS                 | 替换内核结构体回调     | 原函数参数             |
 * +----------------------------+----------------------+------------------------+
 *
 * ★ 重要说明：
 * 追踪类程序（KPROBE/TRACEPOINT/PERF_EVENT/RAW_TRACEPOINT）
 * 不受稳定 API 保证，因为内核数据结构会随版本变化。
 * 这就是为什么 BPF 追踪程序需要与特定内核版本匹配。
 */
enum bpf_prog_type {
    BPF_PROG_TYPE_UNSPEC,
    BPF_PROG_TYPE_SOCKET_FILTER,      /* socket 过滤器（最早的 BPF 程序类型） */
    BPF_PROG_TYPE_KPROBE,             /* ★ kprobe/kretprobe — 追踪内核函数 */
    BPF_PROG_TYPE_SCHED_CLS,          /* TC 分类器 */
    BPF_PROG_TYPE_SCHED_ACT,          /* TC 动作 */
    BPF_PROG_TYPE_TRACEPOINT,         /* ★ tracepoint — 追踪内核静态追踪点 */
    BPF_PROG_TYPE_XDP,                /* XDP — 网卡驱动层包处理 */
    BPF_PROG_TYPE_PERF_EVENT,         /* ★ perf_event — 采样型追踪 */
    BPF_PROG_TYPE_CGROUP_SKB,         /* cgroup skb 过滤 */
    BPF_PROG_TYPE_CGROUP_SOCK,        /* cgroup socket 操作 */
    BPF_PROG_TYPE_LWT_IN,             /* 轻量隧道入口 */
    BPF_PROG_TYPE_LWT_OUT,            /* 轻量隧道出口 */
    BPF_PROG_TYPE_LWT_XMIT,           /* 轻量隧道转发 */
    BPF_PROG_TYPE_SOCK_OPS,           /* socket 操作回调 */
    BPF_PROG_TYPE_SK_SKB,             /* socket skb 策略 */
    BPF_PROG_TYPE_CGROUP_DEVICE,      /* cgroup 设备访问控制 */
    BPF_PROG_TYPE_SK_MSG,             /* socket 消息策略 */
    BPF_PROG_TYPE_RAW_TRACEPOINT,     /* ★ raw tracepoint — 无格式化的追踪点 */
    BPF_PROG_TYPE_CGROUP_SOCK_ADDR,   /* cgroup socket 地址操作 */
    BPF_PROG_TYPE_LWT_SEG6LOCAL,      /* SRv6 轻量隧道 */
    BPF_PROG_TYPE_LIRC_MODE2,         /* 红外遥控 */
    BPF_PROG_TYPE_SK_REUSEPORT,       /* SO_REUSEPORT 选择 */
    BPF_PROG_TYPE_FLOW_DISSECTOR,     /* 流解析器 */
    BPF_PROG_TYPE_CGROUP_SYSCTL,      /* cgroup sysctl 控制 */
    BPF_PROG_TYPE_RAW_TRACEPOINT_WRITABLE, /* 可写 raw tracepoint */
    BPF_PROG_TYPE_CGROUP_SOCKOPT,     /* cgroup socket 选项 */
    BPF_PROG_TYPE_TRACING,            /* ★ fentry/fexit — 基于 BTF 的新一代追踪 */
    BPF_PROG_TYPE_STRUCT_OPS,         /* 替换内核结构体操作 */
    BPF_PROG_TYPE_EXT,                /* 程序扩展（替换已有 BPF 程序） */
    BPF_PROG_TYPE_LSM,                /* Linux 安全模块 */
    BPF_PROG_TYPE_SK_LOOKUP,          /* socket 查找 */
};


/* ======================================================================
 * ★ 核心 #4: union bpf_attr — bpf() 系统调用的参数结构
 * ======================================================================
 *
 * bpf() 系统调用使用 union 来复用同一个参数结构，不同 cmd 使用不同字段。
 * 这是典型的 C 语言 "tagged union" 设计。
 *
 * ★ 为什么用 union？
 * 因为 bpf() 只有一个 attr 参数，但不同命令需要不同的参数。
 * union 让内核只需要一个 syscall 入口，而不是为每个操作定义独立的 syscall。
 */
union bpf_attr {

    /* ---- 用于 BPF_MAP_CREATE 命令 ---- */
    struct {
        __u32   map_type;       /* Map 类型（enum bpf_map_type） */
        __u32   key_size;       /* key 的大小（字节）
                                 * 例如：int key → key_size = 4 */
        __u32   value_size;     /* value 的大小（字节） */
        __u32   max_entries;    /* Map 最大元素数量
                                 * ★ 创建时固定，不可动态扩展 */
        __u32   map_flags;      /* 创建标志（见下方 BPF_F_* 定义） */
        __u32   inner_map_fd;   /* 内部 Map 的 fd（用于嵌套 Map） */
        __u32   numa_node;      /* NUMA 节点（需设置 BPF_F_NUMA_NODE） */
        char    map_name[BPF_OBJ_NAME_LEN]; /* Map 名称（16 字节，调试用） */
        __u32   map_ifindex;    /* 网卡接口索引（offload 用） */
        __u32   btf_fd;         /* BTF 数据 fd（类型信息） */
        __u32   btf_key_type_id;    /* key 的 BTF 类型 ID */
        __u32   btf_value_type_id;  /* value 的 BTF 类型 ID */
        __u32   btf_vmlinux_value_type_id; /* 内核结构体的 BTF 类型 ID */
    };

    /* ---- 用于 BPF_MAP_{LOOKUP,UPDATE,DELETE}_ELEM 命令 ---- */
    struct {
        __u32           map_fd;     /* Map 的文件描述符 */
        __aligned_u64   key;        /* ★ 指向 key 的用户态指针 */
        union {
            __aligned_u64 value;    /* ★ 指向 value 的用户态指针
                                     * （LOOKUP/UPDATE 使用） */
            __aligned_u64 next_key; /* 指向下一个 key 的指针
                                     * （GET_NEXT_KEY 使用） */
        };
        __u64           flags;      /* 操作标志（BPF_ANY/NOEXIST/EXIST） */
    };

    /* ---- 用于 BPF_MAP_*_BATCH 批量命令 ---- */
    struct {
        __aligned_u64   in_batch;   /* 输入：起始批量位置（NULL=从头开始） */
        __aligned_u64   out_batch;  /* 输出：下一批的起始位置 */
        __aligned_u64   keys;       /* key 数组指针 */
        __aligned_u64   values;     /* value 数组指针 */
        __u32           count;      /* 输入：元素数量 / 输出：实际填充数量 */
        __u32           map_fd;     /* Map fd */
        __u64           elem_flags;
        __u64           flags;
    } batch;

    /* ---- 用于 BPF_PROG_LOAD 命令（★ 最复杂的部分）---- */
    struct {
        __u32           prog_type;  /* ★ 程序类型（enum bpf_prog_type）
                                     * 决定 verifier 的验证规则和可挂载位置 */
        __u32           insn_cnt;   /* ★ BPF 指令数量 */
        __aligned_u64   insns;      /* ★ BPF 指令数组指针
                                     * 每条指令 8 字节（struct bpf_insn） */
        __aligned_u64   license;    /* 许可证字符串指针（"GPL"/"Dual BSD/GPL"）
                                     * ★ GPL 程序可以使用更多 helper */
        __u32           log_level;  /* verifier 日志级别：
                                     * 0 = 不输出日志
                                     * 1 = 输出验证过程
                                     * 2 = 输出每条指令的状态 */
        __u32           log_size;   /* 用户态日志缓冲区大小 */
        __aligned_u64   log_buf;    /* ★ 用户态日志缓冲区指针
                                     * verifier 拒绝程序时，这里包含拒绝原因 */
        __u32           kern_version; /* 内核版本（已不使用） */
        __u32           prog_flags; /* 程序标志（BPF_F_SLEEPABLE 等） */
        char            prog_name[BPF_OBJ_NAME_LEN]; /* 程序名称（16 字节） */
        __u32           prog_ifindex;  /* 网卡接口索引（offload 用） */
        __u32           expected_attach_type; /* 预期的附着类型
                                     * ★ 某些程序类型在加载时就需要知道
                                     *   将要附着到哪里，以便 verifier 做
                                     *   针对性的验证（context 访问检查等） */
        __u32           prog_btf_fd;  /* BTF 类型数据 fd */
        __u32           func_info_rec_size;
        __aligned_u64   func_info;
        __u32           func_info_cnt;
        __u32           line_info_rec_size;
        __aligned_u64   line_info;    /* 行号信息（调试用） */
        __u32           line_info_cnt;
        __u32           attach_btf_id;  /* 要附着的内核 BTF 类型 ID */
        __u32           attach_prog_fd; /* 0 = 附着到 vmlinux */
    };

    /* ---- 用于 BPF_PROG_ATTACH / BPF_PROG_DETACH 命令 ---- */
    struct {
        __u32   target_fd;      /* 目标对象的 fd（如 cgroup fd） */
        __u32   attach_bpf_fd;  /* 要附着的 BPF 程序 fd */
        __u32   attach_type;    /* 附着类型（enum bpf_attach_type） */
        __u32   attach_flags;   /* 附着标志（BPF_F_ALLOW_OVERRIDE/MULTI） */
        __u32   replace_bpf_fd; /* 要替换的旧程序 fd（BPF_F_REPLACE） */
    };

    /* ---- 用于 BPF_PROG_TEST_RUN 命令 ---- */
    struct {
        __u32           prog_fd;      /* BPF 程序 fd */
        __u32           retval;       /* 输出：程序返回值 */
        __u32           data_size_in; /* 输入数据大小 */
        __u32           data_size_out;/* 输出缓冲区大小 */
        __aligned_u64   data_in;      /* 输入数据指针 */
        __aligned_u64   data_out;     /* 输出数据指针 */
        __u32           repeat;       /* 重复执行次数 */
        __u32           duration;     /* 输出：总执行时间（ns） */
        __u32           ctx_size_in;
        __u32           ctx_size_out;
        __aligned_u64   ctx_in;
        __aligned_u64   ctx_out;
        __u32           flags;
        __u32           cpu;          /* 在指定 CPU 上执行 */
    } test;

    /* ... 其他字段省略（BPF_OBJ_*, BPF_*_GET_*_ID 等）... */
};


/* ======================================================================
 * BPF 指令格式（每条指令 8 字节）
 * ======================================================================
 *
 * struct bpf_insn 是 BPF 虚拟机的指令格式。
 * 所有 BPF 程序最终都编译成这种指令序列，然后由 verifier 验证。
 */
struct bpf_insn {
    __u8    code;       /* 操作码（opcode）
                         * 高 4 位：指令类别（ALU/LDX/STX/JMP 等）
                         * 低 4 位：具体操作 */
    __u8    dst_reg:4;  /* 目标寄存器（0-10） */
    __u8    src_reg:4;  /* 源寄存器（0-10） */
    __s16   off;        /* 有符号偏移量（用于内存访问和跳转） */
    __s32   imm;        /* 有符号立即数 */
};

/*
 * BPF 寄存器约定（11 个 64 位寄存器）：
 *
 * +------+-------------------+--------------------------------------+
 * | 寄存器 | 角色              | 说明                                 |
 * +------+-------------------+--------------------------------------+
 * | R0   | 返回值            | 函数返回值 / helper 返回值            |
 * | R1   | 参数 1            | 程序入口时 = 指向 bpf_context 的指针  |
 * | R2   | 参数 2            |                                       |
 * | R3   | 参数 3            |                                       |
 * | R4   | 参数 4            |                                       |
 * | R5   | 参数 5            |                                       |
 * | R6   | 被调用者保存       | 函数调用后值不变                      |
 * | R7   | 被调用者保存       |                                       |
 * | R8   | 被调用者保存       |                                       |
 * | R9   | 被调用者保存       |                                       |
 * | R10  | ★ 帧指针（只读）  | 指向栈底，只能用于计算栈地址          |
 * +------+-------------------+--------------------------------------+
 *
 * ★ BPF 栈大小为 512 字节，从 R10 向下增长。
 */


/* ======================================================================
 * Map 创建标志
 * ====================================================================== */
enum {
    BPF_F_NO_PREALLOC   = (1U << 0),  /* 不预分配元素（节省内存） */
    BPF_F_NO_COMMON_LRU = (1U << 1),  /* per-CPU LRU 列表（提高扩展性） */
    BPF_F_NUMA_NODE     = (1U << 2),  /* 指定 NUMA 节点 */
    BPF_F_RDONLY        = (1U << 3),  /* 用户态只读 */
    BPF_F_WRONLY        = (1U << 4),  /* 用户态只写 */
    BPF_F_STACK_BUILD_ID= (1U << 5),  /* 栈 Map 存储 build_id 而非地址 */
    BPF_F_ZERO_SEED     = (1U << 6),  /* 哈希种子置零（仅测试用） */
    BPF_F_RDONLY_PROG   = (1U << 7),  /* BPF 程序侧只读 */
    BPF_F_WRONLY_PROG   = (1U << 8),  /* BPF 程序侧只写 */
    BPF_F_CLONE         = (1U << 9),  /* 从监听 socket 克隆到新连接 */
    BPF_F_MMAPABLE      = (1U << 10), /* ★ 支持 mmap() 映射到用户态 */
    BPF_F_PRESERVE_ELEMS= (1U << 11), /* 进程退出后保留元素 */
    BPF_F_INNER_MAP     = (1U << 12), /* 标记为可嵌套的内部 Map */
};

/* Map 元素操作标志 */
enum {
    BPF_ANY     = 0,    /* 创建新元素或更新已有 */
    BPF_NOEXIST = 1,    /* 仅创建（如果已存在则失败） */
    BPF_EXIST   = 2,    /* 仅更新（如果不存在则失败） */
    BPF_F_LOCK  = 4,    /* 使用 spin_lock 保护操作 */
};


/*
 * ★ 总结：BPF 系统调用的完整数据流
 *
 *  用户态                          内核
 *  =======                         ====
 *
 *  1. 创建 Map
 *     bpf(BPF_MAP_CREATE,         →  bpf_map_create()
 *         &attr)                     → 分配 Map 内存
 *     ← 返回 map_fd                  → 返回 fd
 *
 *  2. 加载 BPF 程序
 *     bpf(BPF_PROG_LOAD,          →  bpf_prog_load()
 *         &attr)                     → bpf_check() ← ★ verifier 验证
 *     ← 返回 prog_fd                 → JIT 编译
 *                                     → 返回 fd
 *
 *  3. 附着程序到钩子
 *     bpf(BPF_LINK_CREATE,        →  bpf_link_create()
 *         &attr)                     → 将 BPF 程序连接到内核钩子点
 *     ← 返回 link_fd                 → 注册 tracepoint/kprobe 回调
 *
 *  4. 读取 Map 数据
 *     bpf(BPF_MAP_LOOKUP_ELEM,    →  bpf_map_lookup_elem()
 *         &attr)                     → 在 Map 中查找 key
 *     ← value 写入 attr.value        → 返回 value
 *
 *  ┌─────────────────────────────────────────────────────┐
 *  │              BPF 程序生命周期                         │
 *  │                                                      │
 *  │  .bt 文件 / C 源码                                   │
 *  │      │                                               │
 *  │      ▼                                               │
 *  │  LLVM 编译器（clang / bpftrace 编译器）               │
 *  │      │                                               │
 *  │      ▼                                               │
 *  │  BPF 字节码（ELF 格式）                              │
 *  │      │                                               │
 *  │      ▼                                               │
 *  │  bpf(BPF_PROG_LOAD) ──→ verifier 静态分析            │
 *  │      │                      │                        │
 *  │      │                   拒绝 → 返回错误+日志         │
 *  │      ▼                      │                        │
 *  │  JIT 编译为本地机器码       │                        │
 *  │      │                                               │
 *  │      ▼                                               │
 *  │  bpf(BPF_LINK_CREATE) ──→ 附着到内核钩子             │
 *  │      │                                               │
 *  │      ▼                                               │
 *  │  钩子触发 → 执行 BPF 程序 → 写入 Map/perf buffer    │
 *  │      │                                               │
 *  │      ▼                                               │
 *  │  用户态：bpf(MAP_LOOKUP) / perf buffer poll          │
 *  └─────────────────────────────────────────────────────┘
 */
