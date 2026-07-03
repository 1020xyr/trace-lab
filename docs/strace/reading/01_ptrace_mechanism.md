/*
 * ======================================================================
 * Step 1: ptrace 机制详解 — strace 的基石
 * ======================================================================
 *
 * 原始路径：
 *   内核 ptrace 实现：src/linux-5.10/kernel/ptrace.c
 *   内核 ptrace 头文件：src/linux-5.10/include/uapi/linux/ptrace.h
 *   strace ptrace 封装：src/strace/src/ptrace.c
 *   strace 使用 ptrace：src/strace/src/strace.c
 *
 * 本文件深入讲解 strace 赖以生存的 ptrace 基础设施：
 *   1. ptrace 的"追踪者-被追踪者"模型
 *   2. 所有核心 PTRACE_* 请求的用途和调用时机
 *   3. strace 如何利用 ptrace 拦截系统调用
 *   4. 性能开销分析（每次系统调用 2 次上下文切换）
 *   5. 源码级解读：从 strace.c 到内核 ptrace.c 的调用链
 *
 * 预计阅读时间：25 分钟
 * ======================================================================
 */


/* ======================================================================
 * 第一部分：ptrace 概述 — "追踪者-被追踪者"模型
 * ======================================================================
 *
 * ptrace（process trace）是 Linux 内核提供的进程追踪机制。
 * 它允许一个进程（tracer）观察和控制另一个进程（tracee）的执行。
 *
 * ★ 核心模型：tracer 通过 ptrace 请求"暂停" tracee，读取其状态，然后"恢复"。
 *
 *   ┌──────────────┐          ┌──────────────┐
 *   │   tracer     │          │   tracee     │
 *   │   (strace)   │          │   (ls/进程)   │
 *   │              │          │              │
 *   │  wait4()  ◄──┼──────────┼─ 系统调用入口  │
 *   │              │          │  ptrace_stop  │
 *   │  分析停止原因  │          │  暂停执行     │
 *   │  读取寄存器    │          │              │
 *   │  打印参数     │          │              │
 *   │              │          │              │
 *   │  PTRACE_     │          │              │
 *   │  SYSCALL ────┼──────────┼─► 继续执行    │
 *   │              │          │              │
 *   │  wait4()  ◄──┼──────────┼─ 系统调用出口  │
 *   │              │          │  ptrace_stop  │
 *   │  读取返回值    │          │  暂停执行     │
 *   │  打印结果     │          │              │
 *   │              │          │              │
 *   │  PTRACE_     │          │              │
 *   │  SYSCALL ────┼──────────┼─► 继续执行    │
 *   └──────────────┘          └──────────────┘
 *
 * ★ 关键理解：
 *   - tracee 在每次 syscall 入口和出口都会暂停（ptrace_stop）
 *   - tracer 通过 wait4() 等待暂停事件
 *   - tracer 用 PTRACE_SYSCALL 恢复 tracee
 *   - 这意味着每个系统调用需要 2 次 ptrace 停止
 */


/* ======================================================================
 * 第二部分：核心 PTRACE_* 请求详解
 * ======================================================================
 *
 * 以下是 strace 使用的所有 ptrace 请求，按使用顺序排列。
 */

/* ----------------------------------------------------------------------
 * 2.1 PTRACE_TRACEME — "我要被追踪"
 * ----------------------------------------------------------------------
 *
 * 调用者：子进程（tracee）
 * 时机：fork 之后、exec 之前
 * 效果：标记当前进程"允许被父进程追踪"
 *
 * ★ 源码位置：strace.c startup_child() → 子进程分支
 *   原始代码 (line 1688):
 *     if (!use_seize && ptrace(PTRACE_TRACEME, 0L, 0L, 0L) < 0)
 *         perror_msg_and_die("ptrace(PTRACE_TRACEME, ...)");
 *
 * 调用后的效果：
 *   1. 子进程的 task_struct 中设置 PT_PTRACED 标志
 *   2. 后续 execve() 会触发 ptrace_stop（让 tracer 看到第一个 syscall）
 *   3. 子进程收到 SIGSTOP 时暂停（kill(getpid(), SIGSTOP)）
 *
 * 为什么 exec 前要 kill(SIGSTOP)?
 *   → 让 tracer 有机会调用 PTRACE_SETOPTIONS 设置选项
 *   → tracer 设置完毕后用 PTRACE_SYSCALL 恢复
 *   → 这样 execve 的入口和出口都能被捕获
 */

/* ----------------------------------------------------------------------
 * 2.2 PTRACE_ATTACH / PTRACE_SEIZE — "附加到已有进程"
 * ----------------------------------------------------------------------
 *
 * 调用者：tracer
 * 时机：strace -p PID 启动时
 * 效果：让指定进程成为 tracee
 *
 * ★ 源码位置：strace.c ptrace_attach_or_seize() (line 575-587)
 *
 *   static int
 *   ptrace_attach_or_seize(int pid, const char **ptrace_attach_cmd)
 *   {
 *       int r;
 *       if (!use_seize)
 *           return *ptrace_attach_cmd = "PTRACE_ATTACH",
 *                  ptrace(PTRACE_ATTACH, pid, 0L, 0L);
 *       r = ptrace(PTRACE_SEIZE, pid, 0L, (unsigned long) ptrace_setoptions);
 *       if (r)
 *           return *ptrace_attach_cmd = "PTRACE_SEIZE", r;
 *       r = ptrace(PTRACE_INTERRUPT, pid, 0L, 0L);
 *       return *ptrace_attach_cmd = "PTRACE_INTERRUPT", r;
 *   }
 *
 * PTRACE_ATTACH vs PTRACE_SEIZE 对比：
 *
 *   特性              PTRACE_ATTACH        PTRACE_SEIZE
 *   ─────────────     ─────────────────    ─────────────────
 *   发送 SIGSTOP      ★ 是                 ★ 否
 *   改变信号状态      是（注入 SIGSTOP）    否（不干扰进程）
 *   传递 options      需要后续 SETOPTIONS   在 SEIZE 时直接传递
 *   内核版本          所有版本              3.4+
 *   strace 偏好       旧内核回退           ★ 默认首选
 *
 * ★ SEIZE 之后需要 PTRACE_INTERRUPT 来让进程暂停，
 *    因为 SEIZE 本身不会让进程停止（这是它的优势）。
 */

/* ----------------------------------------------------------------------
 * 2.3 PTRACE_SETOPTIONS — "设置追踪选项"
 * ----------------------------------------------------------------------
 *
 * 调用者：tracer
 * 时机：tracee 首次停止后
 * 效果：启用各种追踪特性
 *
 * ★ strace 设置的选项（strace.c line 75, 3138-3148）：
 *
 *   // 基础选项（始终设置）
 *   ptrace_setoptions = PTRACE_O_TRACESYSGOOD
 *                     | PTRACE_O_TRACEEXEC
 *                     | PTRACE_O_TRACEEXIT;
 *
 *   // -f 选项追加
 *   if (followfork)
 *       ptrace_setoptions |= PTRACE_O_TRACECLONE
 *                         |  PTRACE_O_TRACEFORK
 *                         |  PTRACE_O_TRACEVFORK;
 *
 *   // --seccomp 选项追加
 *   if (seccomp_filtering)
 *       ptrace_setoptions |= PTRACE_O_TRACESECCOMP
 *                         |  PTRACE_O_EXITKILL;
 *
 * 各选项含义对照表：
 *
 *   选项                   │ 效果
 *   ──────────────────────┼──────────────────────────────────────────
 *   PTRACE_O_TRACESYSGOOD │ ★ syscall 停止时信号 = SIGTRAP|0x80（而非纯 SIGTRAP）
 *   PTRACE_O_TRACEEXEC    │ execve 时产生 PTRACE_EVENT_EXEC 事件
 *   PTRACE_O_TRACEEXIT    │ exit 时产生 PTRACE_EVENT_EXIT 事件
 *   PTRACE_O_TRACECLONE   │ clone 时产生 PTRACE_EVENT_CLONE 事件
 *   PTRACE_O_TRACEFORK    │ fork 时产生 PTRACE_EVENT_FORK 事件
 *   PTRACE_O_TRACEVFORK   │ vfork 时产生 PTRACE_EVENT_VFORK 事件
 *   PTRACE_O_TRACESECCOMP │ seccomp 过滤命中时产生事件
 *   PTRACE_O_EXITKILL     │ tracer 退出时自动杀死所有 tracee
 *
 * ★ 为什么 SIGTRAP|0x80？
 *   普通 SIGTRAP 可能来自断点（int3）、单步（TF 位）等。
 *   TRACESYSGOOD 让 syscall 停止用 0x80 位标记，便于区分。
 *   strace 通过 (status >> 8) & 0xff == (SIGTRAP|0x80) 来判断。
 */

/* ----------------------------------------------------------------------
 * 2.4 PTRACE_SYSCALL — "继续执行，在下次 syscall 入口/出口停止"
 * ----------------------------------------------------------------------
 *
 * 调用者：tracer
 * 时机：主循环每次处理完一个事件后
 * 效果：恢复 tracee 执行，但在下一次 syscall 入口或出口暂停
 *
 * ★ 源码位置：strace.c ptrace_restart() (line 608)
 *
 *   static int
 *   ptrace_restart(const unsigned int op, struct tcb *const tcp, unsigned int sig)
 *   {
 *       int err;
 *       errno = 0;
 *       ptrace(op, tcp->pid, 0L, (unsigned long) sig);
 *       err = errno;
 *       if (!err || err == ESRCH)
 *           return 0;
 *       // ...
 *   }
 *
 *   调用方式：ptrace_restart(PTRACE_SYSCALL, tcp, restart_sig);
 *
 * ★ 关键理解：PTRACE_SYSCALL 让 tracee 在两个点暂停：
 *   1. syscall-enter-stop（系统调用进入内核时）
 *   2. syscall-exit-stop（系统调用返回用户态时）
 *
 *   每次 ptrace_stop 后，tracer 的 wait4() 都会返回。
 *   strace 通过 tcb->flags 中的 TCB_INSYSCALL 位来区分进入和退出。
 *
 *   时间线：
 *   ──────────────────────────────────────────────────────────────────►
 *   execve  enter-stop ─ exit-stop ─ read  enter-stop ─ exit-stop ─ ...
 *           ↑            ↑           ↑             ↑
 *           strace 读参数  strace 读返回值  strace 读参数   strace 读返回值
 *           打印入口行     补全输出        打印入口行      补全输出
 */

/* ----------------------------------------------------------------------
 * 2.5 PTRACE_GETREGS / PTRACE_GETREGSET — "读取寄存器"
 * ----------------------------------------------------------------------
 *
 * 调用者：tracer
 * 时机：syscall-enter-stop 或 syscall-exit-stop 时
 * 效果：读取 tracee 的 CPU 寄存器
 *
 * ★ 用途：获取系统调用号和参数
 *
 *   x86_64 寄存器 → 系统调用参数映射：
 *
 *   寄存器     │ 用途                    │ strace 访问
 *   ──────────┼────────────────────────┼────────────────────
 *   orig_rax  │ ★ 系统调用号            │ x86_64_regs.orig_rax
 *   rdi       │ 参数 1 (fd/pathname)   │ tcp->u_arg[0]
 *   rsi       │ 参数 2 (buf/flags)     │ tcp->u_arg[1]
 *   rdx       │ 参数 3 (count/mode)    │ tcp->u_arg[2]
 *   r10       │ 参数 4 (offset)        │ tcp->u_arg[3]
 *   r8        │ 参数 5                 │ tcp->u_arg[4]
 *   r9        │ 参数 6                 │ tcp->u_arg[5]
 *   rax       │ 返回值（exit-stop 时）  │ tcp->u_rval / tcp->u_error
 *
 *   ★ 注意：第 4 个参数用 r10 而非 rcx！
 *   原因：内核用 SYSCALL 指令进入系统调用时，rcx 被用来保存用户态 rip。
 *
 * GETREGS vs GETREGSET 对比：
 *
 *   特性             GETREGS             GETREGSET
 *   ────────────     ────────────────    ────────────────
 *   参数             pid, 0, &regs       pid, NT_PRSTATUS, &iovec
 *   返回数据         struct user_regs    由 iovec 指定大小
 *   跨架构支持       差（固定结构）       ★ 好（灵活的 iovec）
 *   strace 偏好      回退方案            ★ 默认首选
 */

/* ----------------------------------------------------------------------
 * 2.6 PTRACE_GET_SYSCALL_INFO — "直接获取 syscall 信息"（内核 5.3+）
 * ----------------------------------------------------------------------
 *
 * 调用者：tracer
 * 时机：替代 GETREGS，一次调用获取所有信息
 * 效果：返回 ptrace_syscall_info 结构体
 *
 * ★ 这是最高效的方式：一次 ptrace 调用获取 scno + 参数 + 返回值
 *   而不需要：GETREGS + 多次 PEEKUSER
 *
 * strace.c 中的检测逻辑 (line 1994):
 *   if (ptrace(PTRACE_SEIZE, getpid(), 0, 0) == 0)
 *       → 检测内核是否支持 GET_SYSCALL_INFO
 *
 * ptrace_syscall_info 结构包含：
 *   - op: ENTRY 还是 EXIT
 *   - arch: 架构标识
 *   - instruction_pointer: 指令指针
 *   - stack_pointer: 栈指针
 *   - 对于 ENTRY: scno + args[0..5]
 *   - 对于 EXIT: rval + is_error + error
 */

/* ----------------------------------------------------------------------
 * 2.7 PTRACE_GETEVENTMSG — "获取事件消息"
 * ----------------------------------------------------------------------
 *
 * 调用者：tracer
 * 时机：PTRACE_EVENT_* 停止时
 * 效果：获取事件的附加信息
 *
 * 典型场景：
 *   PTRACE_EVENT_CLONE → msg = 新子进程的 PID
 *   PTRACE_EVENT_FORK  → msg = 新子进程的 PID
 *   PTRACE_EVENT_EXEC  → msg = 旧的 PID（如果 exec 发生在多线程中）
 *   PTRACE_EVENT_EXIT  → msg = 退出状态码
 *
 * ★ strace 用 msg 中的新 PID 创建新的 tcb 结构来追踪子进程。
 */

/* ----------------------------------------------------------------------
 * 2.8 PTRACE_CONT / PTRACE_DETACH — "恢复/解除追踪"
 * ----------------------------------------------------------------------
 *
 * PTRACE_CONT：
 *   - 恢复 tracee 执行，不在 syscall 处停止
 *   - 用于非 syscall 的信号停止（让进程继续处理信号）
 *   - 可以传递信号号让 tracee 收到该信号
 *
 * PTRACE_DETACH：
 *   - 解除追踪关系，tracee 恢复自由执行
 *   - strace 退出时对每个 tracee 调用
 *
 * ★ 源码位置：strace.c (line 1217)
 *   int error = ptrace(PTRACE_DETACH, tcp->pid, 0, 0);
 *   if (error < 0)
 *       perror_func_msg("ptrace(PTRACE_DETACH,%u)", tcp->pid);
 */


/* ======================================================================
 * 第三部分：strace 如何利用 ptrace 拦截系统调用
 * ======================================================================
 *
 * ★ 完整流程图：从 strace 启动到输出第一行
 */

/*
 * strace ls /tmp
 *
 *  ┌── strace main() ──────────────────────────────────────────────────┐
 *  │                                                                   │
 *  │  1. fork()                                                        │
 *  │     ├── 子进程: ptrace(TRACEME) → kill(SIGSTOP) → execve("ls")   │
 *  │     └── 父进程: waitpid() 等待子进程停止                           │
 *  │                                                                   │
 *  │  2. 子进程 SIGSTOP 停止                                           │
 *  │     → ptrace(SETOPTIONS, TRACESYSGOOD|TRACEEXEC|TRACEEXIT)       │
 *  │     → ptrace(SYSCALL, pid, 0, 0) → 恢复子进程                     │
 *  │                                                                   │
 *  │  3. 子进程 execve("ls") → 内核 ptrace_stop()                      │
 *  │     → 内核发送 SIGTRAP|0x80 给父进程                              │
 *  │                                                                   │
 *  │  4. 父进程 wait4() 返回                                           │
 *  │     → next_event() 分析 status: syscall-enter-stop                │
 *  │     → dispatch_event() → trace_syscall()                          │
 *  │       ├── get_scno() → orig_rax = 59 (execve)                    │
 *  │       ├── get_syscall_args() → rdi = "/usr/bin/ls"                │
 *  │       └── 打印 "execve(\"/usr/bin/ls\", ..."                     │
 *  │                                                                   │
 *  │  5. ptrace(SYSCALL) → 恢复子进程                                   │
 *  │     → 子进程 execve 完成 → syscall-exit-stop                      │
 *  │     → wait4() 返回 → trace_syscall() 退出阶段                     │
 *  │       └── get_error() → rax = 0 (成功)                           │
 *  │       └── 打印 ") = 0\n"                                         │
 *  │                                                                   │
 *  │  6. ptrace(SYSCALL) → 恢复 → 子进程执行 brk(NULL)                 │
 *  │     → 又一轮 enter-stop + exit-stop                              │
 *  │     → 打印 "brk(NULL) = 0x5575b39fe000\n"                       │
 *  │                                                                   │
 *  │  ... 循环直到子进程 exit_group() ...                               │
 *  │                                                                   │
 *  │  N. 子进程 PTRACE_EVENT_EXIT                                      │
 *  │     → 打印 "+++ exited with 0 +++"                               │
 *  │     → ptrace(DETACH) 或 wait4 返回 WIFEXITED                     │
 *  └───────────────────────────────────────────────────────────────────┘
 */


/* ======================================================================
 * 第四部分：两种启动模式的源码解读
 * ======================================================================
 */

/* ----------------------------------------------------------------------
 * 4.1 模式 1：启动新进程（strace command）
 * ----------------------------------------------------------------------
 *
 * strace                          子进程
 *   │                               │
 *   ├─ fork() ─────────────────────►│
 *   │                               ├─ ptrace(PTRACE_TRACEME)
 *   │                               ├─ kill(getpid(), SIGSTOP)
 *   │                               │   ↑ 暂停，等 tracer 设置选项
 *   ├─ waitpid(child) ◄─────────────┤
 *   ├─ ptrace(SETOPTIONS)           │
 *   ├─ ptrace(SYSCALL) ─────────────┼─► 继续执行
 *   │                               ├─ exec("ls")
 *   │                               │   ↑ exec 触发 syscall-enter-stop
 *   ├─ wait4() ◄────────────────────┤
 *   │                               │
 *   │  ← 主循环开始 →                │
 */

/* ----------------------------------------------------------------------
 * 4.2 模式 2：附加到已有进程（strace -p PID）
 * ----------------------------------------------------------------------
 *
 * strace                          目标进程
 *   │                               │
 *   ├─ ptrace(PTRACE_SEIZE, pid)    │  ← 不发送 SIGSTOP
 *   ├─ ptrace(PTRACE_INTERRUPT, pid)│  ← 请求中断
 *   ├─ waitpid(pid) ◄───────────────┤  ← 进程暂停
 *   ├─ ptrace(SETOPTIONS)           │
 *   ├─ ptrace(SYSCALL) ─────────────┼─► 继续执行
 *   │                               │
 *   │  ← 主循环开始 →                │
 */


/* ======================================================================
 * 第五部分：主循环 — 同时追踪多个进程
 * ======================================================================
 *
 * strace 通过 wait4(-1, ...) 等待任意被追踪进程的事件。
 * -f 选项可以同时追踪父子进程。
 *
 * ★ 核心循环 (strace.c):
 *
 *   while (dispatch_event(next_event()))
 *       ;
 *
 * next_event() 做了什么：
 *   1. 调用 wait4(-1, &status, __WALL) 等待任意子进程
 *   2. pid2tcb(pid) 找到对应的 tcb（Trace Control Block）
 *   3. 分析 wait status 判断停止原因
 *   4. 返回 struct tcb_wait_data
 *
 * dispatch_event() 做了什么：
 *   1. 根据 trace_event 类型分发
 *   2. TE_RESTART → 调用 trace_syscall()
 *   3. 最后调用 ptrace_restart(PTRACE_SYSCALL, tcp, sig)
 *
 * ┌─────────────────────────────────────────────────────┐
 * │                    主循环                            │
 * │                                                     │
 * │   next_event()                                      │
 * │     │                                               │
 * │     ├── wait4(-1, &status, __WALL)                  │
 * │     │    等待任意被追踪进程                           │
 * │     │                                               │
 * │     ├── pid2tcb(pid)                                │
 * │     │    根据 PID 找到 tcb                           │
 * │     │                                               │
 * │     └── 分析 status                                 │
 * │          ├── WIFSTOPPED → 信号停止                   │
 * │          │    └── SIGTRAP|0x80 → syscall 停止       │
 * │          │    └── SIGTRAP → ptrace event            │
 * │          │    └── 其他信号 → 打印信号信息             │
 * │          ├── WIFEXITED → 进程退出                   │
 * │          └── status >> 16 → ptrace event            │
 * │               ├── CLONE → 创建新 tcb                │
 * │               ├── EXEC → 更新进程名                  │
 * │               └── EXIT → 清理 tcb                   │
 * │                                                     │
 * │   dispatch_event()                                  │
 * │     │                                               │
 * │     ├── trace_syscall(tcp)                          │
 * │     │    ├── entering(tcp) → 打印入口                │
 * │     │    └── !entering(tcp) → 打印出口               │
 * │     │                                               │
 * │     └── ptrace_restart(PTRACE_SYSCALL, tcp, sig)    │
 * │          恢复 tracee，等下一次停止                    │
 * └─────────────────────────────────────────────────────┘
 */


/* ======================================================================
 * 第六部分：性能开销分析
 * ======================================================================
 *
 * ★ ptrace 的开销来自 4 个方面：
 *
 * 1. 上下文切换开销（最大开销源）
 *    ──────────────────────────────
 *    每个系统调用需要 2 次 ptrace 停止：
 *      syscall-enter-stop: tracee → tracer（上下文切换 1）
 *      syscall-exit-stop:  tracee → tracer（上下文切换 2）
 *
 *    每次切换包括：
 *      - CPU 寄存器保存/恢复
 *      - 调度器介入（tracer 和 tracee 互相等待）
 *      - 可能的 TLB/cache 失效
 *
 *    ★ 一个 ls 命令 ≈ 86 个 syscall × 2 = 172 次上下文切换
 *
 * 2. 寄存器读取开销
 *    ────────────────
 *    每次停止后，strace 需要读取 tracee 的寄存器：
 *      - PTRACE_GETREGS：1 次 ptrace 调用
 *      - PTRACE_GETREGSET：1 次 ptrace 调用 + iovec
 *      - PTRACE_GET_SYSCALL_INFO：1 次调用获取所有信息（最高效）
 *
 * 3. 内存读取开销
 *    ─────────────
 *    某些 syscall 参数是指针（如 openat 的 pathname），
 *    strace 需要读取 tracee 的内存空间：
 *      - PTRACE_PEEKDATA：每次 8 字节（极慢！）
 *      - process_vm_readv()：批量读取（更快）
 *
 *    一个 openat("/etc/ld.so.cache") 需要读取 ~20 字节路径
 *    → 至少 3 次 PEEKDATA 或 1 次 process_vm_readv
 *
 * 4. 调度依赖开销
 *    ─────────────
 *    tracee 在 ptrace_stop 中暂停，必须等待 tracer 处理完毕
 *    并用 PTRACE_SYSCALL 恢复后才能继续。
 *    如果 tracer 正在处理另一个 tracee 的事件，当前 tracee 只能等待。
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │              性能开销时间线                               │
 * │                                                         │
 * │  无 strace:                                             │
 * │  ──|syscall|──|syscall|──|syscall|──                    │
 * │    ~1μs      ~1μs      ~1μs                            │
 * │                                                         │
 * │  有 strace:                                             │
 * │  ──|syscall|─stop─┤strace├─stop─┤strace├──             │
 * │    ~1μs    ~5μs  ~20μs  ~5μs  ~20μs                    │
 * │                                                         │
 * │  每个 syscall: 原始耗时 ~1μs → 追踪后 ~50μs              │
 * │  ★ 减速比：约 10x-100x                                  │
 * └─────────────────────────────────────────────────────────┘
 *
 * 典型减速数据（参考值）：
 *
 *   场景                │ 无 strace  │ strace    │ strace -c │ 减速比
 *   ───────────────────┼───────────┼──────────┼──────────┼───────
 *   ls /tmp (86 sysc)  │ ~1ms      │ ~10ms    │ ~5ms     │ 10x
 *   find /usr (1000+)  │ ~50ms     │ ~500ms   │ ~200ms   │ 10x
 *   dd 4K I/O (10000)  │ ~50ms     │ ~5s      │ ~2s      │ 100x
 *   nginx 高并发       │ 基准      │ 无法使用  │ 无法使用  │ >100x
 *
 * ★ 优化方案：seccomp 过滤（--seccomp 选项）
 *
 *   普通模式：所有 syscall 都触发 ptrace 停止
 *   seccomp 模式：在内核侧用 BPF 过滤，不匹配的 syscall 直接通过
 *
 *   ┌───────────────────────────────────────────────────────┐
 *   │  普通模式:                                             │
 *   │  syscall → ptrace_stop → tracer 判断 → 跳过/打印      │
 *   │            ★ 每次都停！                                │
 *   │                                                       │
 *   │  seccomp 模式:                                        │
 *   │  syscall → BPF 判断 → 匹配 → ptrace_stop → 打印      │
 *   │                   → 不匹配 → 直接通过（不停止！）      │
 *   │                  ★ 大幅减少停止次数                     │
 *   └───────────────────────────────────────────────────────┘
 */


/* ======================================================================
 * 第七部分：tcb — strace 的核心数据结构
 * ======================================================================
 *
 * tcb（Trace Control Block）是 strace 为每个被追踪进程维护的状态结构。
 *
 * ★ 源码位置：src/strace/src/defs.h
 *
 * 关键字段：
 *
 *   struct tcb {
 *       int pid;               // ★ 被追踪进程的 PID
 *       int flags;             // TCB_INSYSCALL / TCB_FILTERED / ...
 *       unsigned int scno;     // ★ 当前系统调用号
 *       kernel_ulong_t u_arg[6]; // ★ 6 个系统调用参数
 *       kernel_ulong_t u_rval; // 系统调用返回值
 *       unsigned int u_error;  // 系统调用错误码（errno）
 *       int qual_flg;          // ★ 过滤器标志（决定是否输出）
 *       FILE *outf;            // 输出文件
 *       struct timespec stime; // syscall 进入时间（用于 -T 计时）
 *       // ...
 *   };
 *
 * tcb 与 ptrace 请求的关系：
 *
 *   事件                │ 对 tcb 的操作
 *   ───────────────────┼──────────────────────────────────
 *   新进程             │ alloc_tcb() → 创建新 tcb
 *   syscall-enter-stop │ get_scno() → tcb->scno
 *                      │ get_syscall_args() → tcb->u_arg[]
 *   syscall-exit-stop  │ get_error() → tcb->u_rval / tcb->u_error
 *   PTRACE_EVENT_CLONE │ 用 GETEVENTMSG 获取子 PID → alloc_tcb()
 *   PTRACE_EVENT_EXIT  │ tcb 标记为 exiting
 *   进程退出           │ 释放 tcb
 */


/* ======================================================================
 * 第八部分：ptrace 请求完整速查表
 * ======================================================================
 */

/*
 * ┌───────────────────────┬────────────────────────────────────────────┐
 * │ ptrace 请求            │ strace 中的使用场景                        │
 * ├───────────────────────┼────────────────────────────────────────────┤
 * │ PTRACE_TRACEME        │ 子进程声明"我要被追踪"（startup_child）     │
 * │ PTRACE_ATTACH         │ 旧式附加到进程（-p 模式回退方案）           │
 * │ PTRACE_SEIZE          │ ★ 新式附加（不发送 SIGSTOP，默认首选）      │
 * │ PTRACE_INTERRUPT      │ SEIZE 后请求中断（替代 SIGSTOP）            │
 * │ PTRACE_SYSCALL        │ ★ 主循环每次迭代调用，等待 syscall 停止     │
 * │ PTRACE_CONT           │ 非 syscall 信号停止时恢复执行               │
 * │ PTRACE_DETACH         │ strace 退出时解除追踪                       │
 * │ PTRACE_SETOPTIONS     │ 设置 TRACESYSGOOD/TRACEEXEC/TRACEEXIT 等   │
 * │ PTRACE_GETREGS        │ 读取寄存器（旧式）                          │
 * │ PTRACE_GETREGSET      │ ★ 读取寄存器（新式，iovec 方式）            │
 * │ PTRACE_GET_SYSCALL_INFO │ ★ 一次获取 syscall 全部信息（5.3+）      │
 * │ PTRACE_GETEVENTMSG    │ 获取事件消息（clone 的子 PID 等）            │
 * │ PTRACE_GETSIGINFO     │ 获取信号详细信息（-e trace=signal 时）      │
 * │ PTRACE_PEEKDATA       │ 读取 tracee 内存（旧式，每次 8 字节）       │
 * └───────────────────────┴────────────────────────────────────────────┘
 */


/* ======================================================================
 * 第九部分：思考题与解答
 * ======================================================================
 *
 * Q1: 为什么 strace 对每个 syscall 都需要 2 次停止（enter + exit）？
 *
 * A1: enter-stop 时读取参数（因为 syscall 执行后参数可能被修改），
 *     exit-stop 时读取返回值。如果只捕获 enter，无法知道执行结果；
 *     如果只捕获 exit，参数可能已被内核修改（如 read 的 buf）。
 *
 * Q2: PTRACE_SEIZE 比 PTRACE_ATTACH 好在哪里？
 *
 * A2: ATTACH 会向 tracee 发送 SIGSTOP，改变了进程的信号状态。
 *     如果进程正在处理信号，ATTACH 可能导致意外行为。
 *     SEIZE 不发送 SIGSTOP，而是用 PTRACE_INTERRUPT 请求暂停，
 *     对 tracee 更加"透明"。
 *
 * Q3: 为什么 SIGTRAP|0x80 而不是纯 SIGTRAP？
 *
 * A3: 纯 SIGTRAP 可能来自多种原因：断点（int3）、单步（TF 位）、
 *     exec 事件等。TRACESYSGOOD 选项让 syscall 停止的信号变为
 *     SIGTRAP|0x80（即信号号 0x80 | 5 = 0x85 = 133），
 *     strace 用 (status >> 8) & 0xff 提取信号号来判断类型。
 *
 * Q4: 为什么第 4 个参数用 r10 而不是 rcx？
 *
 * A4: x86_64 的 syscall 指令会将用户态 rip 保存到 rcx，
 *     所以内核中 rcx 不再是原始参数值。
 *     系统调用约定将第 4 个参数从 rcx 移到 r10。
 *     这就是为什么 strace 读取 tcp->u_arg[3] 时用的是 r10。
 *
 * Q5: strace 能追踪自身吗？
 *
 * A5: 不能。PTRACE_SEIZE 自己会返回 EPERM。
 *     源码中 strace.c line 1994:
 *       if (ptrace(PTRACE_SEIZE, getpid(), 0, 0) == 0)
 *     这只是用来检测内核是否支持 SEIZE，不是真的追踪自己。
 */


/* ======================================================================
 * 总结：ptrace 调用链完整映射
 * ======================================================================
 *
 *   strace 用户态                    内核
 *   ──────────────────              ──────────────────────
 *   ptrace(TRACEME)        →       arch_ptrace() → ptrace_traceme()
 *   ptrace(SEIZE, pid)     →       ptrace_attach() → 设置 PT_PTRACED
 *   ptrace(INTERRUPT, pid) →       ptrace_interrupt() → 发送 SIGTRAP
 *   ptrace(SETOPTIONS)     →       ptrace_setoptions() → 设置各种标志
 *   ptrace(SYSCALL)        →       ptrace_resume() → wake_up_process()
 *   wait4()                →       调度到 tracee → 执行 syscall
 *                                  → ptrace_stop() → 暂停 + 通知 tracer
 *   ptrace(GETREGS)        →       ptrace_getregs() → 复制寄存器到用户态
 *   ptrace(GET_SYSCALL_INFO)→      ptrace_get_syscall_info()
 *   ptrace(DETACH)         →       ptrace_detach() → 清除 PT_PTRACED
 *
 * ★ 一句话总结：ptrace 是 strace 的"眼睛和手"——
 *    眼睛 = GETREGS/GET_SYSCALL_INFO（看 tracee 的状态），
 *    手 = SYSCALL/CONT（控制 tracee 的执行节奏）。
 */
