/*
 * ======================================================================
 * Step 2: strace 主程序 — main()、初始化流程与事件主循环
 * ======================================================================
 *
 * 原始路径：src/strace/src/strace.c (约 4400 行)
 *          src/strace/src/syscall.c (约 1700 行)
 *
 * 本文件关注 strace 的整体控制流：
 *   1. main() — 极简入口，只有三行
 *   2. init() — 命令行解析 + 子进程启动/附加
 *   3. startup_child() — fork + PTRACE_TRACEME + execve
 *   4. startup_attach() / attach_tcb() — 附加到运行中的进程
 *   5. next_event() — wait4() 等待子进程事件
 *   6. dispatch_event() — 根据事件类型分发处理
 *   7. trace_syscall() — 系统调用进入/退出的中央调度
 *
 * ★ 核心数据流：
 *   main → init → [fork/attach] → while(next_event + dispatch_event) → terminate
 *
 * 预计阅读时间：20 分钟
 * ======================================================================
 */


/* ======================================================================
 * 第一部分：★ main() — 极简入口
 * ======================================================================
 *
 * strace.c:4397-4408
 *
 * main() 只有三行有效代码：
 *   1. init()        — 完成所有初始化和子进程启动
 *   2. while 循环    — 不断获取事件并分发处理
 *   3. terminate()   — 清理退出
 *
 * ★ 这是理解 strace 架构的起点：
 *   所有复杂性都隐藏在 init() 和事件循环中。
 */

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    init(argc, argv);               /* ★ 所有初始化工作在这里完成 */

    exit_code = !nprocs;            /* 如果没有追踪到任何进程，exit_code = 1 */

    while (dispatch_event(next_event()))
        ;                           /* ★ 主循环：获取事件 → 分发处理 */
    terminate();                    /* 清理并退出 */
}


/* ======================================================================
 * 第二部分：★ init() — 初始化全流程（命令行解析 + 启动追踪）
 * ======================================================================
 *
 * strace.c:2352-3296
 *
 * init() 是 strace 中最长的函数（约 950 行），它负责：
 *   1. 初始化全局状态（默认过滤器、人格设置等）
 *   2. 解析所有命令行参数（getopt_long）
 *   3. 验证参数兼容性
 *   4. 配置 ptrace 选项（followfork → PTRACE_O_TRACECLONE 等）
 *   5. 启动子进程或附加到已有进程
 *   6. 安装信号处理器
 *
 * 下面分段展示关键步骤。
 */

/* ---------- 2.1 初始化默认值 ---------- */

static void ATTRIBUTE_NOINLINE
init(int argc, char *argv[])
{
    /* ... 变量声明省略 ... */

    strace_tracer_pid = getpid();       /* 记录 strace 自身的 PID */
    os_release = get_os_release();      /* 获取内核版本（影响功能可用性） */
    pidns_init();                       /* 初始化 PID 命名空间支持 */

    shared_log = stderr;                /* ★ 默认输出到 stderr */
    set_sortby(DEFAULT_SORTBY);         /* 统计排序方式：默认按 time */
    set_personality(DEFAULT_PERSONALITY); /* 默认架构人格（x86_64 = 0） */

    /* ★ 初始化所有过滤器为默认值 */
    qualify_trace("all");               /* 追踪所有系统调用 */
    qualify_abbrev("all");              /* 所有结构体都缩写 */
    qualify_verbose("all");             /* 所有结构体都不展开 */
    qualify_status("all");              /* 显示所有状态（成功/失败） */
    qualify_quiet("none");              /* 不抑制任何消息 */
    qualify_signals("all");             /* 追踪所有信号 */


/* ---------- 2.2 命令行参数解析 ---------- */

    /*
     * ★ 短选项字符串（注意 '+' 前缀：POSIXLY_CORRECT 兼容）
     *
     * 常用选项速查：
     *   -e expr   过滤表达式（trace=, signal=, inject= 等）
     *   -f        追踪 fork 出的子进程
     *   -p PID    附加到运行中的进程
     *   -c        统计模式
     *   -o file   输出到文件
     *   -T        显示 syscall 耗时
     */
    static const char optstring[] =
        "+a:Ab:cCdDe:E:fFhiI:knNo:O:p:P:qrs:S:tTu:U:vVwxX:yYzZ";

    /* 长选项表（部分展示） */
    static const struct option longopts[] = {
        { "columns",            required_argument, 0, 'a' },
        { "summary-only",       no_argument,       0, 'c' },
        { "summary",            no_argument,       0, 'C' },
        { "follow-forks",       no_argument,       0, GETOPT_FOLLOWFORKS },
        { "attach",             required_argument, 0, 'p' },
        { "trace-path",         required_argument, 0, 'P' },
        { "trace",              required_argument, 0, GETOPT_QUAL_TRACE },
        { "seccomp-bpf",        no_argument,       0, GETOPT_SECCOMP },
        { "kill-on-exixt",      no_argument,       0, GETOPT_KILL_ON_EXIT },
        /* ... 共约 40 个选项 ... */
        { 0, 0, 0, 0 }
    };

    /* ★ 核心解析循环 */
    while ((c = getopt_long(argc, argv, optstring, longopts, &lopt_idx)) != EOF) {
        switch (c) {
        case 'c':                       /* -c: 统计模式 */
            cflag = CFLAG_ONLY_STATS;
            break;
        case 'f':                       /* -f: 追踪 fork */
            followfork_short++;
            break;
        case 'p':                       /* -p PID: 附加到进程 */
            process_opt_p_list(optarg);  /* 解析 PID 列表，分配 TCB */
            break;
        case 'e':                       /* -e expr: 过滤表达式 */
            qualify(optarg);             /* ★ 统一入口，解析 trace=/signal=/inject= 等 */
            break;
        case 'T':                       /* -T: 显示 syscall 耗时 */
            Tflag = 1;
            break;
        case GETOPT_SECCOMP:            /* --seccomp: 内核侧过滤 */
            seccomp_filtering = true;
            break;
        /* ... 其他选项 ... */
        }
    }


/* ---------- 2.3 配置 ptrace 选项 ---------- */

    /*
     * ★ ptrace_setoptions 是一个位掩码，在 attach 时传递给内核。
     *
     * 当用户指定 -f 时，strace 要求内核在 tracee fork/clone/vfork 时
     * 也自动追踪子进程，这通过 ptrace 选项实现：
     */
    if (followfork)
        ptrace_setoptions |= PTRACE_O_TRACECLONE |   /* 追踪 clone */
                             PTRACE_O_TRACEFORK  |   /* 追踪 fork */
                             PTRACE_O_TRACEVFORK;    /* 追踪 vfork */

    /*
     * seccomp 过滤：在内核侧跳过不感兴趣的 syscall，
     * 避免不必要的 ptrace 停止，大幅降低开销。
     */
    if (seccomp_filtering)
        ptrace_setoptions |= PTRACE_O_TRACESECCOMP;

    if (opt_kill_on_exit)
        ptrace_setoptions |= PTRACE_O_EXITKILL;  /* strace 退出时杀死 tracee */


/* ---------- 2.4 启动追踪：fork 子进程 或 attach 到已有进程 ---------- */

    /*
     * ★ strace 有两种工作模式：
     *
     *   模式 A：strace <command>
     *     → 先 fork 子进程，子进程调用 PTRACE_TRACEME，然后 execve
     *     → 父进程（tracer）等待子进程停止后开始追踪
     *
     *   模式 B：strace -p <PID>
     *     → 直接对目标 PID 调用 PTRACE_ATTACH / PTRACE_SEIZE
     *     → 目标进程收到 SIGSTOP 后开始被追踪
     */

    if (argc) {
        /* ★ 模式 A：有命令行参数，fork 子进程 */
        startup_child(argv, new_environ);
    }

    /* ... 安装信号处理器 ... */

    if (nprocs != 0 || daemonized_tracer)
        startup_attach();   /* ★ 模式 B：附加到 -p 指定的进程 */
}


/* ======================================================================
 * 第三部分：★ startup_child() — fork 子进程的完整流程
 * ======================================================================
 *
 * strace.c:1810-1949
 *
 * 这是 strace 最经典的启动方式：
 *   父进程 fork → 子进程 PTRACE_TRACEME → 子进程 SIGSTOP → 子进程 execve
 *
 * ★ 时序图：
 *
 *   父进程 (tracer)              子进程 (tracee)
 *   ──────────────              ──────────────
 *       │                           │
 *       ├──── fork() ──────────────►│
 *       │                           ├─ PTRACE_TRACEME  (标记自己为被追踪)
 *       │                           ├─ kill(SIGSTOP)   (暂停自己)
 *       │◄──── waitpid(STOPPED) ◄───┤   ─── ptrace_stop ───
 *       │                           │
 *       ├─ PTRACE_SEIZE (可选)      │
 *       ├─ kill(SIGCONT) ──────────►│   ─── 被唤醒 ───
 *       ├─ alloctcb(pid)            ├─ execve(command) (替换为被追踪程序)
 *       │                           │   ─── ptrace_stop ─── (execve 触发停止)
 *       │◄──── waitpid() ◄──────────┤
 *       │                           │
 *       ├─ 进入主循环...            │
 */

static void
startup_child(char **argv, char **env)
{
    /* ... 在 PATH 中查找可执行文件的路径 ... */
    /* ... 设置 params_for_tracee 结构 ... */

    pid = fork();                       /* ★ fork 子进程 */
    if (pid < 0)
        perror_func_msg_and_die("fork");

    /*
     * ★ 判断谁应该成为 tracee：
     *   - 非 daemonized 模式：子进程成为 tracee（pid == 0）
     *   - daemonized 模式：父进程成为 tracee（pid != 0）
     */
    if ((pid != 0 && daemonized_tracer)
     || (pid == 0 && !daemonized_tracer)) {
        exec_or_die();                  /* ★ 子进程执行（见下文） */
    }

    /* ===== 以下为 tracer（父进程）的代码 ===== */

    if (!daemonized_tracer) {
        strace_child = pid;             /* 记录子进程 PID */

        if (!use_seize) {
            /* 旧方式：子进程已经调用了 PTRACE_TRACEME，父进程无需操作 */
        } else {
            /* ★ 新方式（PTRACE_SEIZE）：
             *   1. 等待子进程自行 SIGSTOP
             *   2. 调用 PTRACE_SEIZE 建立追踪关系
             *   3. 调用 PTRACE_INTERRUPT 中断子进程
             *   4. 发送 SIGCONT 唤醒子进程
             */
            waitpid(pid, &status, WSTOPPED);  /* 等待子进程停止 */
            ptrace_attach_or_seize(pid, &ptrace_attach_cmd);
            kill(pid, SIGCONT);
        }

        tcp = alloctcb(pid);           /* ★ 为子进程分配 TCB */
        after_successful_attach(tcp,
            TCB_SKIP_DETACH_ON_FIRST_EXEC | TCB_HIDE_LOG);
    }
}


/* ======================================================================
 * 第四部分：★ exec_or_die() — 子进程的 ptrace 设置
 * ======================================================================
 *
 * strace.c:1676-1745
 *
 * 这个函数在子进程中执行，完成三件关键事情：
 *   1. PTRACE_TRACEME — 告诉内核"我要被父进程追踪"
 *   2. kill(SIGSTOP)  — 暂停自己，等待父进程准备好
 *   3. execve()       — 替换为目标程序
 *
 * ★ 关键理解：PTRACE_TRACEME 只能在子进程中调用，
 *   它建立了"父子追踪关系"，此后子进程的每个 syscall 入口/出口
 *   都会触发 ptrace_stop，直到父进程用 PTRACE_SYSCALL 恢复。
 */

static void ATTRIBUTE_NOINLINE ATTRIBUTE_NORETURN
exec_or_die(void)
{
    struct exec_params *params = &params_for_tracee;

    if (params->fd_to_close >= 0)
        close(params->fd_to_close);     /* 关闭日志文件描述符（子进程不需要） */

    if (!daemonized_tracer) {
        /* ★ 核心：子进程请求被追踪 */
        if (!use_seize && ptrace(PTRACE_TRACEME, 0L, 0L, 0L) < 0)
            perror_msg_and_die("ptrace(PTRACE_TRACEME, ...)");
    }

    /* ... 设置 UID/GID（如果指定了 -u 参数）... */

    if (!daemonized_tracer) {
        /*
         * ★ 子进程发送 SIGSTOP 暂停自己。
         * 父进程通过 waitpid() 感知到停止，然后进行 attach。
         * 此后父进程用 PTRACE_SYSCALL 恢复子进程时，
         * 紧接着的 execve() 会再次触发 ptrace_stop。
         */
        if (!NOMMU_SYSTEM)
            kill(getpid(), SIGSTOP);
    }

    /* ★ 最终：替换为目标程序 */
    execve(params->pathname, params->argv, params->env);
    perror_msg_and_die("exec");         /* 如果 execve 失败，报错退出 */
}


/* ======================================================================
 * 第五部分：★ ptrace_attach_or_seize() — attach 的两种方式
 * ======================================================================
 *
 * strace.c:575-587
 *
 * 附加到运行中的进程（-p PID）时有两种策略：
 *
 *   方式 1（旧）：PTRACE_ATTACH
 *     → 直接附加，目标进程收到 SIGSTOP
 *     → 简单但不够灵活
 *
 *   方式 2（新，推荐）：PTRACE_SEIZE + PTRACE_INTERRUPT
 *     → SEIZE 建立追踪关系但不发送信号
 *     → INTERRUPT 中断进程（更干净的方式）
 *     → 可以预先设置 ptrace 选项
 *
 * ★ strace 优先尝试 SEIZE，如果内核不支持则回退到 ATTACH。
 */

static int
ptrace_attach_or_seize(int pid, const char **ptrace_attach_cmd)
{
    int r;
    if (!use_seize)
        /* ★ 旧方式：直接附加 */
        return *ptrace_attach_cmd = "PTRACE_ATTACH",
               ptrace(PTRACE_ATTACH, pid, 0L, 0L);

    /* ★ 新方式：先 SEIZE 再 INTERRUPT */
    r = ptrace(PTRACE_SEIZE, pid, 0L, (unsigned long) ptrace_setoptions);
    if (r)
        return *ptrace_attach_cmd = "PTRACE_SEIZE", r;

    r = ptrace(PTRACE_INTERRUPT, pid, 0L, 0L);
    return *ptrace_attach_cmd = "PTRACE_INTERRUPT", r;
}


/* ======================================================================
 * 第六部分：★ attach_tcb() — 附加到已有进程
 * ======================================================================
 *
 * strace.c:1486-1545
 *
 * 当 strace -p PID 时，对每个指定的 PID 调用此函数。
 * 如果指定了 -f，还会扫描 /proc/PID/task/ 附加所有线程。
 */

static void
attach_tcb(struct tcb *const tcp)
{
    const char *ptrace_attach_cmd;

    /* ★ 对目标进程执行 PTRACE_ATTACH 或 PTRACE_SEIZE */
    if (ptrace_attach_or_seize(tcp->pid, &ptrace_attach_cmd) < 0) {
        /* 附加失败：诊断错误原因（权限不足等） */
        perror_msg("attach: ptrace(%s, %d)", ptrace_attach_cmd, tcp->pid);
        droptcb(tcp);
        return;
    }

    /* ★ 附加成功：标记 TCB_GRABBED（表示可能在 syscall 中间状态） */
    after_successful_attach(tcp, TCB_GRABBED | post_attach_sigstop);

    /* ★ 如果指定了 -f，扫描线程组中的所有线程 */
    if (followfork && tcp->pid != strace_child) {
        /* 遍历 /proc/<pid>/task/ 目录 */
        /* 对每个线程也执行 ptrace_attach_or_seize() */
    }
}


/* ======================================================================
 * 第七部分：★ startup_tcb() — TCB 首次初始化
 * ======================================================================
 *
 * strace.c:3640-3671
 *
 * 每个 tracee 第一次被看到时（TCB_STARTUP 标志），执行此函数。
 * 主要工作：设置 ptrace 选项、初始化状态。
 */

static void
startup_tcb(struct tcb *tcp)
{
    tcp->flags &= ~TCB_STARTUP;          /* 清除 STARTUP 标志 */

    if (!use_seize) {
        /*
         * ★ 对旧式 ATTACH 的进程，需要手动设置 ptrace 选项。
         * PTRACE_SEIZE 方式的进程在 attach 时已经设置了选项。
         *
         * ptrace_setoptions 包含：
         *   PTRACE_O_TRACECLONE  — 追踪 clone
         *   PTRACE_O_TRACEFORK   — 追踪 fork
         *   PTRACE_O_TRACEVFORK  — 追踪 vfork
         *   PTRACE_O_TRACEEXEC   — execve 时产生事件
         *   PTRACE_O_TRACEEXIT   — exit 时产生事件
         */
        ptrace(PTRACE_SETOPTIONS, tcp->pid, NULL, ptrace_setoptions);
    }

    /*
     * ★ TCB_GRABBED：表示进程是 attach 来的（不是 fork 来的）。
     * 此时进程可能正好在某个 syscall 中间，
     * 先尝试读取 syscall 号，保存为 s_prev_ent。
     */
    if (tcp->flags & TCB_GRABBED) {
        if (get_scno(tcp) == 1)
            tcp->s_prev_ent = tcp->s_ent;
    }
}


/* ======================================================================
 * 第八部分：★ next_event() — 事件获取（主循环的"输入"侧）
 * ======================================================================
 *
 * strace.c:3756-4046
 *
 * next_event() 是主循环的第一个阶段：
 *   1. 调用 wait4(-1, ...) 等待任意子进程的状态变化
 *   2. 根据 wait status 判断事件类型（TE_*）
 *   3. 返回包含事件信息的 tcb_wait_data 结构
 *
 * ★ 核心：wait4() 返回的 status 编码了丰富的信息：
 *   WIFEXITED    → 进程正常退出
 *   WIFSIGNALED  → 进程被信号杀死
 *   WIFSTOPPED   → 进程被暂停（syscall、信号、ptrace 事件）
 *   status >> 16 → ptrace 事件类型（FORK、EXEC、EXIT、SECCOMP 等）
 */

static const struct tcb_wait_data *
next_event(void)
{
    if (interrupted)
        return NULL;                     /* 收到中断信号，退出循环 */

    if (syscall_limit == 0)
        return NULL;                     /* syscall 数量限制已达 */

    invalidate_umove_cache();            /* 清除内存缓存 */

    /* ... 处理队列中的待处理 TCB ... */

    /*
     * ★ 核心：wait4() 等待子进程状态变化
     *
     * __WALL：等待所有类型的子进程（包括 clone 出的线程）
     * cflag 时额外获取 rusage（CPU 使用统计）
     */
    int status;
    struct rusage ru;
    int pid = wait4(-1, &status, __WALL, (cflag ? &ru : NULL));

    /* ... 错误处理 ... */

    /* ★ 继续 wait4(WNOHANG) 收集更多事件（批量处理） */
    for (;;) {
        /* 根据 PID 查找对应的 TCB */
        tcp = pid2tcb(pid);
        if (!tcp) {
            tcp = maybe_allocate_tcb(pid, status);  /* 新出现的子进程？ */
            if (!tcp)
                goto next_event_wait_next;
        }

        /*
         * ★ 解析 wait status，确定事件类型（trace_event）
         *
         * 事件类型决定了 dispatch_event() 如何处理：
         */
        if (WIFSIGNALED(status)) {
            wd->te = TE_SIGNALLED;           /* 进程被信号终止 */
        } else if (WIFEXITED(status)) {
            wd->te = TE_EXITED;              /* 进程正常退出 */
        } else {
            /* WIFSTOPPED：进程被暂停 */
            const unsigned int sig = WSTOPSIG(status);
            const unsigned int event = (unsigned int) status >> 16;

            switch (event) {
            case 0:
                /* ★ 普通停止：区分 syscall-stop 和 signal-delivery-stop */
                if (sig == SIGSTOP &&
                    (tcp->flags & TCB_IGNORE_ONE_SIGSTOP)) {
                    /* attach 后的初始 SIGSTOP，忽略 */
                    tcp->flags &= ~TCB_IGNORE_ONE_SIGSTOP;
                    wd->te = TE_RESTART;
                } else if (sig == syscall_trap_sig) {
                    wd->te = TE_SYSCALL_STOP;  /* ★ 系统调用入口/出口停止 */
                } else {
                    /* 信号传递停止：检查是 group-stop 还是 signal-delivery */
                    bool stopped = ptrace(PTRACE_GETSIGINFO,
                                          pid, 0, &wd->si) < 0;
                    wd->te = stopped ? TE_GROUP_STOP
                                     : TE_SIGNAL_DELIVERY_STOP;
                }
                break;
            case PTRACE_EVENT_EXEC:
                wd->te = TE_STOP_BEFORE_EXECVE;  /* execve 前停止 */
                break;
            case PTRACE_EVENT_EXIT:
                wd->te = TE_STOP_BEFORE_EXIT;    /* exit 前停止 */
                break;
            case PTRACE_EVENT_SECCOMP:
                wd->te = TE_SECCOMP;             /* seccomp 过滤器停止 */
                break;
            /* ... 其他 ptrace 事件 ... */
            }
        }

        /* ... 将事件加入队列 ... */
    }

    /* ★ 返回前检查是否需要首次初始化 */
    if (tcp->flags & TCB_STARTUP)
        startup_tcb(tcp);

    set_current_tcp(tcp);              /* 设置当前输出的 TCB */
    return tcb_wait_tab + tcp->wait_data_idx;
}


/* ======================================================================
 * 第九部分：★ dispatch_event() — 事件分发（主循环的"处理"侧）
 * ======================================================================
 *
 * strace.c:4072-4289
 *
 * dispatch_event() 根据 next_event() 返回的事件类型执行不同操作。
 * ★ 对于 TE_SYSCALL_STOP（最常见的情况），调用 trace_syscall()。
 *
 * ★ 返回值：true = 继续循环，false = 退出循环
 */

static bool
dispatch_event(const struct tcb_wait_data *wd)
{
    enum trace_event te = wd ? wd->te : TE_BREAK;
    int status = wd ? wd->status : 0;

    /* ★ 决定恢复 tracee 时使用什么 ptrace 操作 */
    unsigned int restart_op;
    if (current_tcp && has_seccomp_filter(current_tcp))
        restart_op = seccomp_filter_restart_operator(current_tcp);
    else
        restart_op = PTRACE_SYSCALL;      /* ★ 默认：下次 syscall 停止 */

    switch (te) {
    case TE_BREAK:
        return false;                     /* 退出主循环 */

    case TE_NEXT:
        return true;                      /* 继续等待下一个事件 */

    case TE_RESTART:
        break;                            /* 直接恢复 tracee */

    /*
     * ★ TE_SYSCALL_STOP：最重要的事件！
     * tracee 在 syscall 入口或出口停止了。
     */
    case TE_SYSCALL_STOP:
        if (trace_syscall(current_tcp, &restart_sig) < 0) {
            /* ptrace 失败：可能是进程已死亡 */
            return true;
        }
        break;

    /* 信号传递停止：打印信号信息 */
    case TE_SIGNAL_DELIVERY_STOP:
        restart_sig = WSTOPSIG(status);
        print_stopped(current_tcp, &wd->si, restart_sig);
        break;

    /* 进程被信号杀死 */
    case TE_SIGNALLED:
        print_signalled(current_tcp, current_tcp->pid, status);
        droptcb(current_tcp);
        return true;

    /* 进程正常退出 */
    case TE_EXITED:
        print_exited(current_tcp, current_tcp->pid, status);
        droptcb(current_tcp);
        return true;

    /* execve 前停止 */
    case TE_STOP_BEFORE_EXECVE:
        /* ... 处理 exec 事件 ... */
        break;

    /* exit 前停止 */
    case TE_STOP_BEFORE_EXIT:
        /* ... 处理 exit 事件 ... */
        break;

    /* seccomp 过滤器停止 */
    case TE_SECCOMP:
        /* ... 处理 seccomp 事件 ... */
        break;

    /* group-stop（Ctrl+Z 等） */
    case TE_GROUP_STOP:
        restart_sig = WSTOPSIG(status);
        if (use_seize) {
            restart_op = PTRACE_LISTEN;   /* ★ SEIZE 模式用 LISTEN 恢复 */
            restart_sig = 0;
        }
        break;
    }

    /*
     * ★ 恢复 tracee 执行
     *
     * restart_op 通常是 PTRACE_SYSCALL，让 tracee 运行到下一个
     * syscall 入口/出口时再次停止。
     * restart_sig 如果有值，表示需要将信号传递给 tracee。
     */
    restart_tracee(current_tcp, restart_op, restart_sig);
    return true;
}


/* ======================================================================
 * 第十部分：★ trace_syscall() — syscall 处理的核心调度
 * ======================================================================
 *
 * strace.c:4048-4070
 *
 * 这是 strace 最核心的函数：根据 tracee 是在 syscall 入口还是出口，
 * 调用不同的解码和打印流水线。
 *
 * ★ 进入/退出判断：通过 TCB_INSYSCALL 标志位。
 *   - 未设置 = entering（syscall 入口）
 *   - 已设置 = exiting（syscall 出口）
 *
 * ★ 每次调用 trace_syscall() 后，标志位会自动翻转：
 *   entering → syscall_entering_finish() 设置 TCB_INSYSCALL
 *   exiting  → syscall_exiting_finish() 清除 TCB_INSYSCALL
 *
 *   完整的 syscall 追踪流程：
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │  第 1 次 ptrace_stop（syscall 入口）                       │
 *   │                                                          │
 *   │  trace_syscall()                                         │
 *   │    ├─ entering(tcp) == true                              │
 *   │    ├─ syscall_entering_decode(tcp)                       │
 *   │    │    ├─ get_scno()       → 从寄存器读取系统调用号       │
 *   │    │    └─ get_syscall_args() → 从寄存器读取参数           │
 *   │    ├─ syscall_entering_trace(tcp)                        │
 *   │    │    ├─ 过滤检查（traced? pathtrace_match?）           │
 *   │    │    └─ tcp_sysent(tcp)->sys_func(tcp) → 打印参数      │
 *   │    └─ syscall_entering_finish(tcp)                       │
 *   │         └─ 设置 TCB_INSYSCALL，记录入口时间               │
 *   │                                                          │
 *   │  PTRACE_SYSCALL → tracee 执行系统调用                      │
 *   │                                                          │
 *   │  第 2 次 ptrace_stop（syscall 出口）                       │
 *   │                                                          │
 *   │  trace_syscall()                                         │
 *   │    ├─ entering(tcp) == false（即 exiting）                │
 *   │    ├─ syscall_exiting_decode(tcp)                        │
 *   │    │    └─ get_syscall_result() → 读取返回值和错误码       │
 *   │    ├─ syscall_exiting_trace(tcp)                         │
 *   │    │    ├─ tcp_sysent(tcp)->sys_func(tcp) → 打印输出参数   │
 *   │    │    ├─ 打印返回值（= N 或 = -1 errno）                │
 *   │    │    └─ 打印耗时（-T 选项）                             │
 *   │    └─ syscall_exiting_finish(tcp)                        │
 *   │         └─ 清除 TCB_INSYSCALL                            │
 *   └──────────────────────────────────────────────────────────┘
 */

static int
trace_syscall(struct tcb *tcp, unsigned int *sig)
{
    if (entering(tcp)) {
        /* ★ syscall 入口阶段 */
        int res = syscall_entering_decode(tcp);  /* 解码：获取 scno + 参数 */
        switch (res) {
        case 0:
            return 0;                             /* 忽略此次停止 */
        case 1:
            res = syscall_entering_trace(tcp, sig); /* 追踪：打印 syscall 名和参数 */
        }
        syscall_entering_finish(tcp, res);        /* 完成：设置 INSYSCALL */
        return res;
    } else {
        /* ★ syscall 出口阶段 */
        struct timespec ts = {};
        int res = syscall_exiting_decode(tcp, &ts); /* 解码：获取返回值 */
        if (res != 0) {
            res = syscall_exiting_trace(tcp, &ts, res); /* 追踪：打印返回值 */
        }
        syscall_exiting_finish(tcp);               /* 完成：清除 INSYSCALL */
        return res;
    }
}


/* ======================================================================
 * 第十一部分：struct tcb — 追踪控制块（Trace Control Block）
 * ======================================================================
 *
 * defs.h:265-333
 *
 * TCB 是 strace 中最重要的数据结构，每个被追踪的进程都有一个 TCB。
 * 它保存了该进程的所有追踪状态。
 *
 * ★ 核心字段：
 */

struct tcb {
    int flags;                  /* TCB_* 标志位（见下方说明） */
    int pid;                    /* 被追踪进程的 PID */
    int qual_flg;               /* 过滤器标志（QUAL_TRACE / QUAL_ABBREV 等） */

    unsigned long u_error;      /* 系统调用错误码（errno） */
    kernel_ulong_t scno;        /* ★ 当前系统调用号 */
    kernel_ulong_t true_scno;   /* 原始系统调用号（未经 subcall 解码） */
    kernel_ulong_t u_arg[MAX_ARGS]; /* ★ 系统调用参数（最多 6 个） */
    kernel_long_t u_rval;       /* ★ 系统调用返回值 */

    int sys_func_rval;          /* sys_func() 的返回值（RVAL_* 格式标志） */
    FILE *outf;                 /* 该进程的输出文件 */
    const char *auxstr;         /* 辅助信息字符串 */

    const struct_sysent *s_ent; /* ★ 指向 sysent[] 表的当前条目 */
    const struct_sysent *s_prev_ent; /* 上一个 syscall（用于 interrupted 消息） */

    struct timespec stime;      /* 系统 CPU 时间 */
    struct timespec etime;      /* syscall 入口时间（用于计算耗时） */
    struct timespec ltime;      /* 上次 syscall 出口时间 */

    struct mmap_cache_t *mmap_cache; /* mmap 缓存（用于栈回溯） */
    char comm[PROC_COMM_LEN];  /* /proc/<pid>/comm 中的进程名 */
};

/*
 * ★ TCB 标志位说明：
 *
 * 标志                    │ 含义
 * ───────────────────────┼────────────────────────────────────
 * TCB_STARTUP (0x01)     │ 首次看到此进程，需要初始化
 * TCB_IGNORE_ONE_SIGSTOP │ 忽略下一个 SIGSTOP（attach 后的初始停止）
 * TCB_INSYSCALL (0x04)   │ ★ 在 syscall 内部（区分入口和出口）
 * TCB_ATTACHED (0x08)    │ 已通过 attach 附加（不是 fork 来的）
 * TCB_FILTERED (0x20)    │ 当前 syscall 被过滤器排除
 * TCB_TAMPERED (0x40)    │ 当前 syscall 被故障注入篡改
 * TCB_GRABBED (0x400)    │ attach 来的进程，可能在 syscall 中间
 * TCB_SECCOMP_FILTER     │ 此进程有 seccomp 过滤器
 */


/* ======================================================================
 * 附录：strace 完整启动与追踪流程图
 * ======================================================================
 *
 *   strace -f -e trace=file ls /tmp
 *          │
 *          ▼
 *   ┌─────────────────────┐
 *   │      main()         │
 *   │  init(argc, argv)   │
 *   └────────┬────────────┘
 *            │
 *            ▼
 *   ┌─────────────────────────────────────────────────────┐
 *   │                   init()                            │
 *   │                                                     │
 *   │  1. 初始化默认值                                     │
 *   │     qualify_trace("all"), shared_log = stderr       │
 *   │                                                     │
 *   │  2. getopt_long() 解析参数                           │
 *   │     -f → followfork = true                          │
 *   │     -e trace=file → qualify_trace("file")           │
 *   │                                                     │
 *   │  3. 配置 ptrace_setoptions                          │
 *   │     |= PTRACE_O_TRACECLONE | TRACEFORK | TRACEVFORK │
 *   │                                                     │
 *   │  4. startup_child(argv)                             │
 *   │     fork() → 子进程 PTRACE_TRACEME → SIGSTOP        │
 *   │     父进程 PTRACE_SEIZE → SIGCONT                    │
 *   │     alloctcb(pid)                                   │
 *   │                                                     │
 *   │  5. 安装信号处理器                                   │
 *   └────────┬────────────────────────────────────────────┘
 *            │
 *            ▼
 *   ┌─────────────────────────────────────────────────────┐
 *   │           主循环 while (dispatch_event(next_event()))│
 *   │                                                     │
 *   │  ┌─── next_event() ─────────────────────────────┐   │
 *   │  │  wait4(-1, &status, __WALL)                  │   │
 *   │  │    → pid = 子进程 PID                         │   │
 *   │  │    → 解析 status → TE_SYSCALL_STOP           │   │
 *   │  │    → startup_tcb()（首次时设置 SEToptions）    │   │
 *   │  └──────────────────────┬───────────────────────┘   │
 *   │                         │                           │
 *   │  ┌─── dispatch_event() ─▼───────────────────────┐   │
 *   │  │  switch (TE_SYSCALL_STOP)                    │   │
 *   │  │    → trace_syscall(tcp, &sig)                │   │
 *   │  │      → entering: decode + trace + finish     │   │
 *   │  │      → exiting:  decode + trace + finish     │   │
 *   │  │    → restart_tracee(PTRACE_SYSCALL)          │   │
 *   │  └──────────────────────────────────────────────┘   │
 *   │                         │                           │
 *   │         ◄───────────────┘  (循环)                    │
 *   └─────────────────────────────────────────────────────┘
 *            │
 *            │  tracee 退出（TE_EXITED）
 *            ▼
 *   ┌─────────────────────┐
 *   │   terminate()       │
 *   │   printsummary()    │  ← 如果 -c 选项
 *   └─────────────────────┘
 */
