/*
 * ======================================================================
 * Step 3: 系统调用解码流水线 — 从寄存器到可读输出
 * ======================================================================
 *
 * 原始路径：
 *   src/strace/src/syscall.c       — 解码流水线主逻辑
 *   src/strace/src/linux/x86_64/get_scno.c         — x86_64 获取 syscall 号
 *   src/strace/src/linux/x86_64/get_syscall_args.c — x86_64 获取参数
 *   src/strace/src/linux/x86_64/get_error.c        — x86_64 获取返回值
 *   src/strace/src/linux/x86_64/arch_regs.h        — x86_64 寄存器偏移定义
 *   src/strace/src/sysent.h        — sysent 结构体定义
 *   src/strace/src/linux/64/syscallent.h — 系统调用表（通用 64 位）
 *   src/strace/src/ucopy.c         — umovestr() 从 tracee 读字符串
 *   src/strace/src/upeek.c         — upeek() PTRACE_PEEKUSER 封装
 *
 * 本文件深入讲解 strace 如何将原始寄存器值转换为可读的 syscall 输出：
 *   1. 寄存器读取：PTRACE_GETREGSET / PTRACE_GETREGS
 *   2. 系统调用号解码：get_scno() → sysent[] 表查找
 *   3. 参数解码：get_syscall_args() → 寄存器 → u_arg[]
 *   4. 入口解码流水线：syscall_entering_decode/trace/finish
 *   5. 出口解码流水线：syscall_exiting_decode/trace/finish
 *   6. sysent 表结构：如何将 syscall 号映射到解码函数
 *   7. 用户空间内存读取：umovestr()、upeek()
 *
 * 预计阅读时间：25 分钟
 * ======================================================================
 */


/* ======================================================================
 * 第一部分：★ 完整解码流水线总览
 * ======================================================================
 *
 * strace 对每个系统调用的处理分为"入口"和"出口"两个阶段。
 * 每个阶段都有一条三步流水线：decode → trace → finish
 *
 * ┌────────────────────────────────────────────────────────────────────┐
 * │                     SYSCALL 入口阶段                               │
 * │                                                                    │
 * │  ptrace_stop                                                       │
 * │      │                                                             │
 * │      ▼                                                             │
 * │  syscall_entering_decode(tcp)                                      │
 * │      ├─ get_scno(tcp)          → 从寄存器读取 syscall 号           │
 * │      │    ├─ get_syscall_regs() → PTRACE_GETREGSET 读取全部寄存器  │
 * │      │    ├─ arch_get_scno()    → 从 orig_rax 提取 syscall 号      │
 * │      │    └─ tcp->s_ent = &sysent[scno] → 查表获取 sysent 条目     │
 * │      └─ get_syscall_args(tcp)  → 从寄存器提取参数                  │
 * │           └─ tcp->u_arg[0..5] = {rdi, rsi, rdx, r10, r8, r9}      │
 * │      │                                                             │
 * │      ▼                                                             │
 * │  syscall_entering_trace(tcp, sig)                                  │
 * │      ├─ traced(tcp)?           → 检查过滤器                       │
 * │      ├─ pathtrace_match(tcp)?  → 检查路径过滤                     │
 * │      ├─ printleader(tcp)       → 打印 PID 前缀                    │
 * │      ├─ tprints(sys_name)      → 打印 syscall 名称                │
 * │      └─ tcp_sysent(tcp)->sys_func(tcp) → ★ 调用解码函数打印参数    │
 * │      │                                                             │
 * │      ▼                                                             │
 * │  syscall_entering_finish(tcp, res)                                 │
 * │      ├─ tcp->flags |= TCB_INSYSCALL   → 标记为"在 syscall 内部"   │
 * │      └─ clock_gettime(CLOCK_MONOTONIC) → 记录入口时间             │
 * │                                                                    │
 * │  PTRACE_SYSCALL → tracee 执行系统调用                               │
 * └────────────────────────────────────────────────────────────────────┘
 *
 * ┌────────────────────────────────────────────────────────────────────┐
 * │                     SYSCALL 出口阶段                               │
 * │                                                                    │
 * │  ptrace_stop                                                       │
 * │      │                                                             │
 * │      ▼                                                             │
 * │  syscall_exiting_decode(tcp, pts)                                  │
 * │      ├─ clock_gettime(CLOCK_MONOTONIC) → 记录出口时间             │
 * │      └─ get_syscall_result(tcp)                                   │
 * │           ├─ get_syscall_result_regs() → 再次读取寄存器            │
 * │           └─ get_error(tcp)                                       │
 * │                ├─ rax 值 → tcp->u_rval (返回值)                   │
 * │                └─ 检查是否为负数 → tcp->u_error (errno)           │
 * │      │                                                             │
 * │      ▼                                                             │
 * │  syscall_exiting_trace(tcp, ts, res)                               │
 * │      ├─ print_syscall_resume(tcp) → 打印 "... syscall resumed>"   │
 * │      ├─ sys_func(tcp)         → ★ 再次调用解码函数（打印输出参数） │
 * │      ├─ 打印返回值             → "= 0" 或 "= -1 ENOENT (...)"     │
 * │      └─ Tflag? 打印耗时        → "<0.000123>"                     │
 * │      │                                                             │
 * │      ▼                                                             │
 * │  syscall_exiting_finish(tcp)                                       │
 * │      └─ tcp->flags &= ~TCB_INSYSCALL  → 清除"在 syscall 内部"标志 │
 * └────────────────────────────────────────────────────────────────────┘
 */


/* ======================================================================
 * 第二部分：★ 寄存器读取 — PTRACE_GETREGSET
 * ======================================================================
 *
 * syscall.c:1136-1283
 *
 * strace 通过 ptrace 请求读取 tracee 的 CPU 寄存器。
 * 有两种方式（取决于架构和内核版本）：
 *
 *   方式 1（推荐）：PTRACE_GETREGSET + NT_PRSTATUS
 *     → 通过 iovec 结构读取，支持变长寄存器集
 *     → x86_64 上可区分 64 位/32 位/x32 模式
 *
 *   方式 2（旧式）：PTRACE_GETREGS
 *     → 固定结构体，不支持变长
 *     → SPARC 架构使用（参数顺序不同）
 *
 * ★ strace 优先使用 GETREGSET，不支持时回退到 GETREGS。
 */

/* 方式 1：PTRACE_GETREGSET（推荐） */

# define ptrace_getregset_or_getregs ptrace_getregset
static long
ptrace_getregset(pid_t pid)
{
    /*
     * ★ iovec 结构：
     *   iov_base = 寄存器缓冲区地址
     *   iov_len  = 期望读取的字节数
     *
     * 内核会根据实际寄存器集大小填写 iov_len，
     * strace 通过检查 iov_len 来判断 tracee 是 32 位还是 64 位！
     */
    static struct iovec io = {
        .iov_base = &ARCH_REGS_FOR_GETREGSET,  /* 架构特定的寄存器结构 */
        .iov_len = sizeof(ARCH_REGS_FOR_GETREGSET)
    };
    return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &io);
    /*                                     ^^^^^^^^^^^^
     * NT_PRSTATUS = 1，表示读取"进程状态"寄存器集
     * （包含通用寄存器、指令指针、栈指针等）
     */
}


/* 方式 2：PTRACE_GETREGS（旧式） */

# define ptrace_getregset_or_getregs ptrace_getregs
static long
ptrace_getregs(pid_t pid)
{
    return ptrace(PTRACE_GETREGS, pid, NULL, &ARCH_REGS_FOR_GETREGS);
}


/*
 * ★ get_regs() — 统一的寄存器读取入口
 *
 * syscall.c:1216-1283
 *
 * 这个函数封装了上述两种方式，并处理缓存：
 * 每次 ptrace_stop 后只读取一次寄存器，后续调用直接返回缓存值。
 */

static long
get_regs(struct tcb *const tcp)
{
    if (get_regs_error != -1)           /* 已缓存，直接返回 */
        return get_regs_error;

    /* 尝试 PTRACE_GETREGSET/PTRACE_GETREGS */
    get_regs_error = ptrace_getregset_or_getregs(tcp->pid);

    /* 对于多架构支持（x86_64 + i386 + x32），
     * 根据 iov_len 判断当前 personality */
    return get_regs_error;
}


/* ======================================================================
 * 第三部分：★ 系统调用号获取 — get_scno()
 * ======================================================================
 *
 * syscall.c:1444-1497 + linux/x86_64/get_scno.c
 *
 * get_scno() 的任务：从寄存器中提取系统调用号，并查找 sysent[] 表。
 *
 * ★ 在 x86_64 上，系统调用号存放在 orig_rax 寄存器中。
 *   （注意不是 rax！rax 存放的是返回值）
 *
 * ★ PTRACE_GET_SYSCALL_INFO：
 *   Linux 5.3+ 引入的新接口，可以直接获取 syscall 信息，
 *   不需要手动读取寄存器。strace 优先使用此接口。
 */

/*
 * ★ 通用入口（syscall.c:1444-1497）
 */
int
get_scno(struct tcb *tcp)
{
    tcp->scno = -1;
    tcp->s_ent = NULL;
    tcp->qual_flg = QUAL_RAW | DEFAULT_QUAL_FLAGS;

    /* 第一步：读取寄存器（或 PTRACE_GET_SYSCALL_INFO） */
    if (get_syscall_regs(tcp) < 0)
        return -1;

    /* ★ 优先使用 PTRACE_GET_SYSCALL_INFO（Linux 5.3+） */
    if (ptrace_syscall_info_is_entry()) {
        tcp->scno = ptrace_sci.entry.nr;  /* 直接从内核获取 */
    } else {
        /* 回退：架构特定的寄存器读取 */
        int rc = arch_get_scno(tcp);       /* 见下方 x86_64 实现 */
        if (rc != 1) return rc;
    }

    tcp->true_scno = tcp->scno;
    tcp->scno = shuffle_scno(tcp->scno);   /* 处理 subcall 映射 */

    /* ★ 查找 sysent[] 表 */
    if (scno_is_valid(tcp->scno)) {
        tcp->s_ent = &sysent[tcp->scno];         /* 有效的 syscall */
        tcp->qual_flg = qual_flags(tcp->scno);    /* 获取过滤器标志 */
    } else {
        /* 无效的 syscall 号：创建临时 stub */
        /* sys_name = "syscall_0xNNN" */
        tcp->s_ent = &stub_sysent;
    }

    return 1;
}


/*
 * ★ x86_64 特定实现（linux/x86_64/get_scno.c）
 *
 * x86_64 支持三种"人格"（personality）：
 *   personality 0: x86_64（64 位原生）
 *   personality 1: i386（32 位兼容）
 *   personality 2: x32（32 位指针 + 64 位长整型）
 *
 * ★ 区分方式：通过 GETREGSET 返回的 iov_len 和 __X32_SYSCALL_BIT
 */

static int
arch_get_scno(struct tcb *tcp)
{
    kernel_ulong_t scno = 0;
    unsigned int currpers;

    if (x86_io.iov_len == sizeof(i386_regs)) {
        /* ★ iov_len = i386 寄存器大小 → 32 位进程 */
        scno = i386_regs.orig_eax;      /* i386: syscall 号在 orig_eax */
        currpers = 1;                    /* personality 1: i386 */
    } else {
        /* ★ iov_len = x86_64 寄存器大小 → 64 位进程 */
        scno = x86_64_regs.orig_rax;    /* x86_64: syscall 号在 orig_rax */
        currpers = 0;                    /* personality 0: x86_64 */

        if (scno & __X32_SYSCALL_BIT) {
            /* ★ __X32_SYSCALL_BIT = 0x40000000
             * 如果设置了此位，说明是 x32 ABI */
            currpers = 2;                /* personality 2: x32 */
        }
    }

    update_personality(tcp, currpers);   /* 切换到正确的 sysent 表 */
    tcp->scno = scno;
    return 1;
}


/* ======================================================================
 * 第四部分：★ 参数提取 — get_syscall_args()
 * ======================================================================
 *
 * syscall.c:1499-1524 + linux/x86_64/get_syscall_args.c
 *
 * 系统调用参数在入口时存放在特定寄存器中。
 * 不同架构使用不同的寄存器传参约定。
 *
 * ★ x86_64 调用约定（System V AMD64 ABI）：
 *
 *   参数位置  │ 寄存器    │ tcp->u_arg 索引
 *   ─────────┼──────────┼──────────────
 *   第 1 个   │ rdi       │ u_arg[0]
 *   第 2 个   │ rsi       │ u_arg[1]
 *   第 3 个   │ rdx       │ u_arg[2]
 *   第 4 个   │ r10       │ u_arg[3]   ← 注意：用户态用 rcx，syscall 用 r10
 *   第 5 个   │ r8        │ u_arg[4]
 *   第 6 个   │ r9        │ u_arg[5]
 *   系统调用号 │ orig_rax  │ tcp->scno
 *   返回值     │ rax       │ tcp->u_rval
 *
 * ★ 为什么第 4 个参数用 r10 而不是 rcx？
 *   syscall 指令会破坏 rcx（用来保存返回地址），
 *   所以内核约定用 r10 代替 rcx 传递第 4 个参数。
 */

/*
 * ★ x86_64 特定实现（linux/x86_64/get_syscall_args.c）
 */
static int
arch_get_syscall_args(struct tcb *tcp)
{
    if (x86_io.iov_len != sizeof(i386_regs)) {
        /* ★ x86-64 或 x32 ABI */
        tcp->u_arg[0] = x86_64_regs.rdi;    /* 第 1 个参数 */
        tcp->u_arg[1] = x86_64_regs.rsi;    /* 第 2 个参数 */
        tcp->u_arg[2] = x86_64_regs.rdx;    /* 第 3 个参数 */
        tcp->u_arg[3] = x86_64_regs.r10;    /* 第 4 个参数（不是 rcx！） */
        tcp->u_arg[4] = x86_64_regs.r8;     /* 第 5 个参数 */
        tcp->u_arg[5] = x86_64_regs.r9;     /* 第 6 个参数 */
    } else {
        /* ★ i386 ABI（32 位兼容模式） */
        tcp->u_arg[0] = (uint32_t) i386_regs.ebx;
        tcp->u_arg[1] = (uint32_t) i386_regs.ecx;
        tcp->u_arg[2] = (uint32_t) i386_regs.edx;
        tcp->u_arg[3] = (uint32_t) i386_regs.esi;
        tcp->u_arg[4] = (uint32_t) i386_regs.edi;
        tcp->u_arg[5] = (uint32_t) i386_regs.ebp;
    }
    return 1;
}


/*
 * ★ 通用入口（syscall.c:1499-1524）
 *
 * 优先使用 PTRACE_GET_SYSCALL_INFO 提供的参数，
 * 回退到架构特定的寄存器读取。
 */
static int
get_syscall_args(struct tcb *tcp)
{
    if (ptrace_syscall_info_is_entry()) {
        /* ★ 直接从内核接口获取参数（Linux 5.3+） */
        for (unsigned int i = 0; i < n; ++i)
            tcp->u_arg[i] = ptrace_sci.entry.args[i];
        return 1;
    }
    return arch_get_syscall_args(tcp);    /* 回退到寄存器读取 */
}


/* ======================================================================
 * 第五部分：★ 返回值和错误码获取 — get_syscall_result / get_error
 * ======================================================================
 *
 * syscall.c:1537-1564 + linux/x86_64/get_error.c
 *
 * syscall 出口时，返回值在 rax（x86_64）或 eax（i386）中。
 * ★ Linux 的约定：如果 rax 是负数且在 [-4095, -1] 范围内，
 *   表示系统调用失败，其绝对值是 errno。
 */

/*
 * ★ x86_64 特定实现（linux/x86_64/get_error.c）
 */
static void
arch_get_error(struct tcb *tcp, const bool check_errno)
{
    long long rax;

    if (x86_io.iov_len == sizeof(i386_regs)) {
        rax = (int32_t) i386_regs.eax;     /* i386: 32 位符号扩展 */
    } else {
        rax = x86_64_regs.rax;             /* x86_64: 64 位 */
    }

    /*
     * ★ 核心判断：rax 是否是负数 errno？
     *
     * is_negated_errno() 检查 rax 是否在 [-4095, -1] 范围内。
     * 如果是：
     *   u_rval = -1（表示失败）
     *   u_error = -rax（实际 errno 值）
     * 如果否：
     *   u_rval = rax（成功的返回值）
     *   u_error = 0（没有错误）
     */
    if (check_errno && is_negated_errno(rax)) {
        tcp->u_rval = -1;
        tcp->u_error = -rax;
    } else {
        tcp->u_rval = rax;
        tcp->u_error = 0;
    }
}


/* ======================================================================
 * 第六部分：★ sysent 结构体和系统调用表
 * ======================================================================
 *
 * sysent.h:13-19 + linux/64/syscallent.h
 *
 * sysent 是 strace 的核心查找表：将系统调用号映射到解码函数。
 * 每个系统调用都有一个 sysent 条目。
 */

/*
 * ★ sysent 结构体定义（sysent.h:13-19）
 */
typedef struct sysent {
    unsigned nargs;                    /* 参数个数（0-6） */
    int sys_flags;                     /* ★ 标志位：TRACE_FILE, TRACE_NETWORK 等 */
    int sen;                           /* 系统调用枚举号（SEN_read, SEN_write 等） */
    int (*sys_func)(struct tcb *);     /* ★★ 解码函数指针：核心！ */
    const char *sys_name;              /* 系统调用名称字符串 */
} struct_sysent;

/*
 * ★ sys_flags 标志位说明（sysent.h:21-43）
 *
 * 标志位                 │ 八进制值    │ 含义
 * ──────────────────────┼────────────┼──────────────────────
 * TRACE_FILE            │ 000000001  │ 文件相关（openat, stat, chmod...）
 * TRACE_IPC             │ 000000002  │ IPC 相关（msgget, semop, shmget...）
 * TRACE_NETWORK         │ 000000004  │ 网络相关（socket, connect, sendto...）
 * TRACE_PROCESS         │ 000000010  │ 进程相关（clone, execve, exit...）
 * TRACE_SIGNAL          │ 000000020  │ 信号相关（kill, sigaction...）
 * TRACE_DESC            │ 000000040  │ 描述符相关（read, write, poll...）
 * TRACE_MEMORY          │ 000000100  │ 内存相关（mmap, mprotect...）
 * SYSCALL_NEVER_FAILS   │ 000000200  │ 永不失败的 syscall（getpid 等）
 * TRACE_STAT            │ 000004000  │ stat 系列
 * TRACE_FSTAT           │ 000200000  │ fstat 系列
 * TRACE_SECCOMP_DEFAULT │ 002000000  │ seccomp 默认追踪
 */


/*
 * ★ 系统调用表示例（linux/64/syscallent.h，前 80 条）
 *
 * 格式：[编号] = { nargs, sys_flags, SEN(xxx), "syscall_name" }
 *
 * 注意：这是通用 64 位表，x86_64 实际使用的表在
 * linux/x86_64/syscallent.h 中（会 #include 此文件）。
 */

/* syscall 号 │ nargs │ flags │ SEN 枚举          │ syscall 名称 */
[  0] = { 2,  TM,             SEN(io_setup),              "io_setup"              },
[  1] = { 1,  TM,             SEN(io_destroy),            "io_destroy"            },
[  2] = { 3,  0,              SEN(io_submit),             "io_submit"             },
[  3] = { 3,  0,              SEN(io_cancel),             "io_cancel"             },
[  5] = { 5,  TF,             SEN(setxattr),              "setxattr"              },
[ 17] = { 2,  TF,             SEN(getcwd),                "getcwd"                },
[ 23] = { 1,  TD,             SEN(dup),                   "dup"                   },
[ 25] = { 3,  TD,             SEN(fcntl),                 "fcntl"                 },
[ 29] = { 3,  TD,             SEN(ioctl),                 "ioctl"                 },
[ 33] = { 4,  TD|TF,          SEN(mknodat),               "mknodat"               },
[ 40] = { 5,  TF,             SEN(mount),                 "mount"                 },
[ 49] = { 1,  TF,             SEN(chdir),                 "chdir"                 },
[ 56] = { 4,  TD|TF,          SEN(openat),                "openat"                },
[ 57] = { 1,  TD,             SEN(close),                 "close"                 },
[ 59] = { 2,  TD,             SEN(pipe2),                 "pipe2"                 },
[ 61] = { 3,  TD,             SEN(getdents64),            "getdents64"            },
[ 62] = { 3,  TD,             SEN(lseek),                 "lseek"                 },
[ 63] = { 3,  TD,             SEN(read),                  "read"                  },
[ 64] = { 3,  TD,             SEN(write),                 "write"                 },
[ 65] = { 3,  TD,             SEN(readv),                 "readv"                 },
[ 66] = { 3,  TD,             SEN(writev),                "writev"                },

/*
 * ★ 以 read (scno=63) 为例解读：
 *
 *   [ 63] = { 3, TD, SEN(read), "read" }
 *           │  │  │    │         │
 *           │  │  │    │         └─ 打印名称："read"
 *           │  │  │    └─ SEN 枚举：SEN_read（用于 switch 判断）
 *           │  │  └─ sys_flags: TD = TRACE_DESC（文件描述符相关）
 *           │  └─ nargs: 3（read 有 3 个参数：fd, buf, count）
 *           └─ syscall 编号: 63
 *
 * ★ sys_func 字段在此表中没有显示，它通过 SEN 枚举在编译时
 *   映射到具体的解码函数。例如：
 *   SEN(read) → sys_read()   → 打印 read(fd, "content", count)
 *   SEN(openat) → sys_openat() → 打印 openat(dirfd, "/path", flags, mode)
 */


/* ======================================================================
 * 第七部分：★ 入口解码流水线 — syscall_entering_decode/trace/finish
 * ======================================================================
 *
 * syscall.c:581-699
 *
 * 这三步流水线在每次 syscall 入口时被调用。
 */

/*
 * ★ Step 1: decode — 解码 syscall 号和参数
 *
 * 返回值：
 *   0  → 忽略此次停止
 *   1  → 正常，继续 trace
 *   其他 → 错误
 */
int
syscall_entering_decode(struct tcb *tcp)
{
    int res = get_scno(tcp);            /* ★ 读取 syscall 号 */
    if (res == 0) return res;

    if (res != 1 || (res = get_syscall_args(tcp)) != 1) {
        /* 寄存器读取失败，打印 syscall 名称 + " <unavailable>" */
        printleader(tcp);
        tprints_arg_begin(tcp_sysent(tcp)->sys_name);
        return res;
    }

    /* 处理特殊的间接 syscall（ipc、socketcall） */
    /* ... */

    return 1;
}


/*
 * ★ Step 2: trace — 过滤检查 + 打印 syscall 入口
 *
 * 这是产生 strace 输出行的核心函数。
 */
int
syscall_entering_trace(struct tcb *tcp, unsigned int *sig)
{
    /* 过滤检查：是否隐藏日志？是否被 trace= 过滤？ */
    if (hide_log(tcp) || !traced(tcp)
        || ((tracing_paths || tracing_fds) && !pathtrace_match(tcp))) {
        tcp->flags |= TCB_FILTERED;
        return 0;
    }

    tcp->flags &= ~TCB_FILTERED;

    /* 故障注入（-e inject=） */
    if (inject(tcp))
        tamper_with_syscall_entering(tcp, sig);

    /* 纯统计模式（-c）：不输出，直接返回 */
    if (cflag == CFLAG_ONLY_STATS)
        return 0;

    /* ★ 打印输出行 */
    printleader(tcp);                           /* 打印 PID 前缀 */
    tprints_arg_begin(tcp_sysent(tcp)->sys_name); /* 打印 syscall 名称 */

    /*
     * ★★ 核心：调用 sysent 表中注册的解码函数
     *
     * raw(tcp) 为 true（-e raw= 选项）时，
     *   使用通用的 printargs() 按原始十六进制打印参数。
     * 否则调用 sys_func()，例如：
     *   sys_read() → 打印 "read(3, "hello\n", 1024)"
     *   sys_openat() → 打印 "openat(AT_FDCWD, "/etc/hosts", O_RDONLY)"
     *   sys_mmap() → 打印 "mmap(NULL, 4096, PROT_READ|PROT_WRITE, ...)"
     */
    int res = raw(tcp) ? printargs(tcp) : tcp_sysent(tcp)->sys_func(tcp);

    fflush(tcp->outf);
    return res;
}


/*
 * ★ Step 3: finish — 标记状态转换
 */
void
syscall_entering_finish(struct tcb *tcp, int res)
{
    tcp->flags |= TCB_INSYSCALL;          /* ★ 标记为"在 syscall 内部" */
    tcp->sys_func_rval = res;             /* 保存 sys_func 的返回值 */

    /* 记录入口时间（用于 -T 计算耗时） */
    if ((Tflag || cflag) && !filtered(tcp))
        clock_gettime(CLOCK_MONOTONIC, &tcp->etime);
}


/* ======================================================================
 * 第八部分：★ 出口解码流水线 — syscall_exiting_decode/trace/finish
 * ======================================================================
 *
 * syscall.c:713-1049
 */

/*
 * ★ Step 1: decode — 获取返回值
 */
int
syscall_exiting_decode(struct tcb *tcp, struct timespec *pts)
{
    /* 尽早记录出口时间 */
    if ((Tflag || cflag) && !filtered(tcp))
        clock_gettime(CLOCK_MONOTONIC, pts);

    /* 内存映射变化通知（mmap/munmap 等） */
    if (tcp_sysent(tcp)->sys_flags & MEMORY_MAPPING_CHANGE)
        mmap_notify_report(tcp);

    if (filtered(tcp)) return 0;

    /* ★ 读取返回值和错误码 */
    return get_syscall_result(tcp);
    /* 内部调用 get_error()，设置 tcp->u_rval 和 tcp->u_error */
}


/*
 * ★ Step 2: trace — 打印 syscall 出口信息
 *
 * 这是 strace 输出行中 "= N" 部分的核心。
 */
int
syscall_exiting_trace(struct tcb *tcp, struct timespec *ts, int res)
{
    /* 打印 "<... syscall_name resumed>"（如果是被中断的 syscall） */
    if (cflag != CFLAG_ONLY_STATS)
        print_syscall_resume(tcp);

    if (res == 1) {
        /*
         * ★ 再次调用 sys_func() 解码输出参数
         *
         * 许多 syscall 的解码函数在入口和出口做不同的事：
         *   入口：打印输入参数（fd, flags 等）
         *   出口：打印输出参数（struct stat *buf 的内容等）
         *
         * 例如 stat() 在出口时读取用户空间的 struct stat 并打印。
         */
        if (!raw(tcp)) {
            if (!(tcp->sys_func_rval & RVAL_DECODED))
                sys_res = tcp_sysent(tcp)->sys_func(tcp);
        }
    }

    /* ★ 打印返回值 */
    if (tcp->u_error) {
        /* 有错误：打印 "= -1 ENOENT (No such file or directory)" */
        switch (tcp->u_error) {
        case ERESTARTSYS:
            /* 信号中断，将被重启 */
            print_erestart("ERESTARTSYS",
                "To be restarted if SA_RESTART is set");
            break;
        case ERESTARTNOINTR:
            /* 无条件重启 */
            print_erestart("ERESTARTNOINTR", "To be restarted");
            break;
        /* ... 其他 ERESTART* ... */
        default:
            /* ★ 普通错误："-1 ENOENT (No such file or directory)" */
            print_err_ret(tcp->u_rval, tcp->u_error);
            break;
        }
    } else {
        /* ★ 成功：根据 sys_res 的格式标志打印 */
        switch (sys_res & RVAL_MASK) {
        case RVAL_HEX:    PRINT_VAL_X(tcp->u_rval);  break; /* 十六进制 */
        case RVAL_OCTAL:  print_numeric_ll_umode_t(mode); break; /* 八进制（mode） */
        case RVAL_UDECIMAL: PRINT_VAL_U(tcp->u_rval); break; /* 无符号十进制 */
        case RVAL_FD:     printfd(tcp, tcp->u_rval);  break; /* 文件描述符 */
        default:          /* 有符号十进制 */; break;
        }
    }

    /* 打印辅助信息字符串（auxstr） */
    if ((sys_res & RVAL_STR) && tcp->auxstr)
        tprints_sysret_string("retstr", tcp->auxstr);

    /* ★ 打印 syscall 耗时（-T 选项） */
    if (Tflag) {
        ts_sub(ts, ts, &tcp->etime);    /* 耗时 = 出口时间 - 入口时间 */
        /* 输出格式：<秒.微秒> */
    }

    return 0;
}


/*
 * ★ Step 3: finish — 清除状态
 */
void
syscall_exiting_finish(struct tcb *tcp)
{
    tcp->flags &= ~(TCB_INSYSCALL | TCB_TAMPERED | ...);
    tcp->sys_func_rval = 0;
}


/* ======================================================================
 * 第九部分：★ 从 tracee 内存读取数据 — umovestr() 和 upeek()
 * ======================================================================
 *
 * 系统调用的参数经常包含指针（如文件名、缓冲区），
 * strace 需要从 tracee 的用户空间内存中读取这些指针指向的数据。
 */

/*
 * ★ upeek() — 读取 tracee 的 USER 区域（寄存器）
 *
 * upeek.c:18-33
 *
 * PTRACE_PEEKUSER 每次读取一个 word（8 字节）。
 * 用于在不读取全部寄存器的情况下获取单个寄存器值。
 */
int
upeek(struct tcb *tcp, unsigned long off, kernel_ulong_t *res)
{
    long val;

    errno = 0;
    /* ★ PTRACE_PEEKUSER：从 tracee 的 USER 区域读取一个 word
     * off 是寄存器偏移（如 8 * RAX = 80） */
    val = ptrace(PTRACE_PEEKUSER, (pid_t) tcp->pid, (void *) off, 0);
    if (val == -1 && errno) {
        return -1;
    }
    *res = (unsigned long) val;
    return 0;
}


/*
 * ★ umovestr() — 从 tracee 内存读取字符串
 *
 * ucopy.c:403-469
 *
 * 这是 strace 中最常用的数据读取函数之一。
 * 用于读取文件路径、环境变量等字符串参数。
 *
 * ★ 两种读取策略：
 *   1. process_vm_readv()（推荐）：批量读取，一次可以读多个字节
 *   2. PTRACE_PEEKDATA（回退）：每次读取一个 word（8 字节）
 */
int
umovestr(struct tcb *const tcp, kernel_ulong_t addr, unsigned int len,
         char *laddr)
{
    if (tracee_addr_is_invalid(addr))
        return -1;

    const int pid = tcp->pid;

    /* ★ 策略 1：process_vm_readv()（Linux 3.2+） */
    if (!process_vm_readv_not_supported) {
        while (len) {
            /* 按页边界分块读取，避免跨页 EFAULT */
            int r = vm_read_mem(pid, laddr, addr, chunk_len);
            if (r > 0) {
                /* 检查是否读到 NUL 终止符 */
                char *nul_addr = memchr(laddr, '\0', r);
                if (nul_addr)
                    return nread + (nul_addr - laddr) + 1;
                /* 继续读取下一块 */
            }
        }
    }

    /* ★ 策略 2：PTRACE_PEEKDATA（回退方案） */
    return umovestr_peekdata(pid, addr, len, laddr);
    /* 每次用 PTRACE_PEEKDATA 读取一个 word */
}


/* ======================================================================
 * 第十部分：★ 寄存器偏移定义（x86_64）
 * ======================================================================
 *
 * linux/x86_64/arch_regs.h
 *
 * 这些定义对应 struct user_regs_struct 中的偏移量。
 * PTRACE_PEEKUSER 使用这些偏移来读取特定寄存器。
 * 每个偏移 × 8 = 在 USER 区域中的字节偏移。
 */

#define R15         0
#define R14         1
#define R13         2
#define R12         3
#define RBP         4
#define RBX         5
#define R11         6
#define R10         7
#define R9          8
#define R8          9
#define RAX         10     /* ★ 返回值 / 错误码 */
#define RCX         11
#define RDX         12     /* 第 3 个参数 */
#define RSI         13     /* 第 2 个参数 */
#define RDI         14     /* 第 1 个参数 */
#define ORIG_RAX    15     /* ★★ 系统调用号（原始 rax） */
#define RIP         16     /* 指令指针 */
#define RSP         19     /* 栈指针 */

/*
 * ★ 用法示例：
 *
 *   upeek(tcp, 8 * RAX, &val);    → 读取 rax（返回值）
 *   upeek(tcp, 8 * ORIG_RAX, &val); → 读取 orig_rax（syscall 号）
 *   upeek(tcp, 8 * RDI, &val);    → 读取 rdi（第 1 个参数）
 *
 *   偏移 8 * RAX = 80，即 USER 区域第 80 字节处是 rax 的值。
 */


/* ======================================================================
 * 附录 A：★ strace 完整 syscall 拦截流程图
 * ======================================================================
 *
 *    ┌────────────────────────────────────────────────────────────────┐
 *    │                        用户空间                                 │
 *    │                                                                │
 *    │   strace (tracer)                    tracee (被追踪进程)        │
 *    │   ──────────────                     ──────────────────         │
 *    │        │                                   │                   │
 *    │        │              fork()               │                   │
 *    │        ├────────────────────────────────────┤                   │
 *    │        │                                   │                   │
 *    │        │                          PTRACE_TRACEME               │
 *    │        │                          kill(SIGSTOP)                │
 *    │        │◄──── wait4(STOPPED) ◄─────────────┤                   │
 *    │        │                                   │                   │
 *    │        │         PTRACE_SYSCALL            │                   │
 *    │        ├──────────────────────────────────►│                   │
 *    │        │                                   │                   │
 *    │        │                         ┌───── execve("/bin/ls") ─────┤
 *    │        │                         │         │                   │
 *    │        │                         │    ptrace_stop              │
 *    │        │                         │    (execve 入口)            │
 *    │        │◄──── wait4() ◄──────────┼─────────┤                   │
 *    │        │                         │         │                   │
 *    │        │  get_scno()             │         │                   │
 *    │        │  ├─ PTRACE_GETREGSET    │         │                   │
 *    │        │  │  → orig_rax = 59     │         │                   │
 *    │        │  └─ sysent[59] =        │         │                   │
 *    │        │     { "execve", 3, ... }│         │                   │
 *    │        │                         │         │                   │
 *    │        │  get_syscall_args()     │         │                   │
 *    │        │  ├─ rdi = "/bin/ls"     │         │                   │
 *    │        │  ├─ rsi = [argv]        │         │                   │
 *    │        │  └─ rdx = [envp]        │         │                   │
 *    │        │                         │         │                   │
 *    │        │  umovestr(rdi)          │         │                   │
 *    │        │  ├─ process_vm_readv()  │         │                   │
 *    │        │  └─ → "/bin/ls"         │         │                   │
 *    │        │                         │         │                   │
 *    │   打印:│  execve("/bin/ls", ...) │         │                   │
 *    │        │                         │         │                   │
 *    │        │  PTRACE_SYSCALL         │         │                   │
 *    │        ├──────────────────────────────────►│                   │
 *    │        │                         │         │                   │
 *    │        │                         └─── execve 在内核中执行 ────┤
 *    │        │                                   │                   │
 *    │        │                              ptrace_stop              │
 *    │        │                              (execve 出口)            │
 *    │        │◄──── wait4() ◄────────────────────┤                   │
 *    │        │                                   │                   │
 *    │        │  get_syscall_result()             │                   │
 *    │        │  ├─ PTRACE_GETREGSET              │                   │
 *    │        │  └─ rax = 0 (成功)                │                   │
 *    │        │                                   │                   │
 *    │   打印:│  ) = 0                            │                   │
 *    │        │                                   │                   │
 *    │        │  PTRACE_SYSCALL                   │                   │
 *    │        ├──────────────────────────────────►│                   │
 *    │        │                                   ├─ brk(NULL)        │
 *    │        │                              ptrace_stop              │
 *    │        │◄──── wait4() ◄────────────────────┤                   │
 *    │        │  ... (继续循环)                    │                   │
 *    │        │                                   │                   │
 *    └────────────────────────────────────────────────────────────────┘
 *
 *
 * ======================================================================
 * 附录 B：★ 数据流对照表 — 寄存器到输出的映射
 * ======================================================================
 *
 * 以 strace 追踪 read(3, buf, 1024) = 5 为例：
 *
 * ┌─────────────┬─────────────────┬──────────────────────────────┐
 * │ 阶段        │ 数据来源        │ strace 处理                   │
 * ├─────────────┼─────────────────┼──────────────────────────────┤
 * │ 入口 decode │ PTRACE_GETREGSET│ orig_rax = 0x00 → scno = 0   │
 * │             │                 │ → sysent[0] = { "read", ... } │
 * │             │                 │ rdi = 3 → u_arg[0] = 3 (fd)  │
 * │             │                 │ rsi = 0x7fff... → u_arg[1]   │
 * │             │                 │ rdx = 1024 → u_arg[2]        │
 * ├─────────────┼─────────────────┼──────────────────────────────┤
 * │ 入口 trace  │ sys_func(tcp)   │ sys_read() 打印：             │
 * │             │                 │ "read(3, "                    │
 * │             │                 │ （buf 内容在出口时打印）       │
 * ├─────────────┼─────────────────┼──────────────────────────────┤
 * │ 出口 decode │ PTRACE_GETREGSET│ rax = 5 → u_rval = 5         │
 * │             │                 │ is_negated_errno(5) = false   │
 * │             │                 │ → u_error = 0                 │
 * ├─────────────┼─────────────────┼──────────────────────────────┤
 * │ 出口 trace  │ sys_func(tcp)   │ sys_read() 打印 buf 内容：    │
 * │             │ umovestr(rsi, 5)│ "hello"                       │
 * │             │                 │ 然后打印 "= 5"               │
 * ├─────────────┼─────────────────┼──────────────────────────────┤
 * │ 完整输出    │                 │ read(3, "hello", 1024) = 5   │
 * └─────────────┴─────────────────┴──────────────────────────────┘
 *
 *
 * ======================================================================
 * 附录 C：★ sysent 表的查找过程
 * ======================================================================
 *
 *   用户代码: read(3, buf, 1024)
 *       │
 *       ▼
 *   syscall 指令（orig_rax = 63）
 *       │
 *       ▼
 *   ptrace_stop（入口）
 *       │
 *       ▼
 *   get_scno(tcp)
 *       │
 *       ├─ PTRACE_GETREGSET → 读取全部寄存器
 *       ├─ arch_get_scno()  → orig_rax = 63
 *       ├─ shuffle_scno(63) → 63（无映射变化）
 *       └─ tcp->s_ent = &sysent[63]
 *                        │
 *                        ▼
 *           sysent[63] = { 3, TD, SEN(read), "read" }
 *                              │
 *                              ▼
 *           nargs = 3 → 知道要读取 3 个参数
 *           sys_name = "read" → 打印 "read("
 *           SEN(read) → 调用 sys_read() 解码函数
 *                              │
 *                              ▼
 *   get_syscall_args(tcp)
 *       ├─ rdi = 3        → u_arg[0] (fd)
 *       ├─ rsi = 0x7f...  → u_arg[1] (buf 指针)
 *       └─ rdx = 1024     → u_arg[2] (count)
 *                              │
 *                              ▼
 *   sys_read(tcp) 被调用
 *       ├─ printfd(tcp, u_arg[0]) → 打印 "3"
 *       ├─ 入口：不打印 buf（等出口时再读）
 *       └─ tprintf("%lu", u_arg[2]) → 打印 "1024"
 */
