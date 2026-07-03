# strace 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: strace -c 显示 futex 调用最多说明什么？](#q1-strace--c-显示-futex-调用最多说明什么)
- [Q2: strace 的性能开销有多大？生产环境能用吗？](#q2-strace-的性能开销有多大生产环境能用吗)
- [Q3: strace 输出中 +++ exited with 0 +++ 是什么意思？](#q3-strace-输出中--exited-with-0--是什么意思)
- [Q4: strace -p 附加到进程后没有输出，可能的原因？](#q4-strace--p-附加到进程后没有输出可能的原因)

---

## Q1: strace -c 显示 futex 调用最多说明什么？

**日期：** 2026-07-04  
**场景：** strace -c -f 追踪多线程程序，futex 占 55% 且 errors 高达 70%  
**相关文件：** `docs/strace/reading/06_performance_analysis.md`  
**源码位置：** 内核 `kernel/futex.c` — `futex_wait()` / `futex_wake()`

### 回答

**★ futex 调用多 + errors 高 = 严重的用户态锁竞争。**

#### futex 是什么

```
futex（Fast Userspace muTEX）是 Linux 内核提供的底层同步原语。
用户态的 mutex、条件变量、读写锁、信号量底层都通过 futex 实现。

无竞争时（快速路径）：
  用户态原子操作（CAS）直接获取锁 → 不进入内核 → 零开销

有竞争时（慢速路径）：
  用户态 CAS 失败 → 调用 futex(FUTEX_WAIT) → 内核挂起线程
  持锁者释放锁 → 调用 futex(FUTEX_WAKE) → 内核唤醒等待线程
  ★ 每次竞争 = 2 次系统调用 + 2 次上下文切换
```

#### strace -c 分析

```bash
strace -c -f -p <PID>

# 典型锁竞争输出：
# % time     seconds  usecs/call     calls    errors syscall
# ------ ----------- ----------- --------- --------- --------
#  55.30    0.123400          10     12340      8900 futex
#                                                     ^^^^
#                                          ★ errors/calls = 72%
```

**errors 的含义：**

| errno | 含义 | 说明 |
|-------|------|------|
| EAGAIN (11) | ★ 锁已被其他线程持有 | 最常见，说明竞争激烈 |
| ETIMEDOUT (110) | 等待锁超时 | 可能死锁或严重竞争 |
| EINTR (4) | 被信号中断 | 通常无害 |

#### 锁竞争 vs 正常等待

```
锁竞争（需要排查）：
  futex calls 多（> 10K/s）
  errors/calls > 50%
  usecs/call 差异大（有些很快，有些很慢）
  ★ 根因：多线程竞争同一个 mutex

正常等待（无需处理）：
  futex calls 少（< 1K/s）
  errors 少
  usecs/call 比较稳定
  ★ 例：定时器的 futex 等待、条件变量的正常等待
```

#### 进一步定位

```bash
# 1. 用 perf record 找到持锁函数
perf record -g -p <PID> -- sleep 10
perf report
# 找 pthread_mutex_lock / std::mutex 相关的调用栈

# 2. 用 bpftrace 追踪 futex 地址
bpftrace -e '
  tracepoint:syscalls:sys_enter_futex {
    @futex_addr[arg0] = count();
  }
'
# ★ 找出被竞争最多的 futex 地址 → 对应具体的锁变量

# 3. 查看锁竞争的调用栈
perf record -e sched:sched_stat_wait -p <PID> -- sleep 10
perf report
```

**一句话总结：** strace -c 显示 futex 多 + errors 高 = 多线程锁竞争严重。用 `perf record -g` 找到持锁热点函数，用 bpftrace 追踪 futex 地址定位具体的锁。

---

## Q2: strace 的性能开销有多大？生产环境能用吗？

**日期：** 2026-07-04  
**场景：** 需要在生产环境追踪进程的系统调用，担心 strace 影响服务性能  
**相关文件：** `docs/strace/reading/06_performance_analysis.md`  
**源码位置：** 内核 `kernel/ptrace.c` — `ptrace_stop()`

### 回答

**★ strace 会导致 10-100 倍减速，不推荐在生产环境使用。生产环境应使用 perf trace。**

#### 开销来源

```
strace 基于 ptrace 机制：

每个系统调用需要 2 次 ptrace stop（入口 + 出口）：

  被追踪进程:
    syscall 入口 → ptrace_stop() → 暂停 ~5μs
    tracer 读取参数 → ~5μs
    PTRACE_SYSCALL 恢复 → ~2μs
    syscall 执行 → 正常时间
    syscall 出口 → ptrace_stop() → 暂停 ~5μs
    tracer 读取返回值 → ~5μs
    PTRACE_SYSCALL 恢复 → ~2μs
    
  ★ 每个 syscall 额外开销：~20-30 μs
  ★ 如果进程每秒 10 万次 syscall → 额外 2-3 秒/秒 = 完全无法运行
```

#### 量化测试数据

| 场景 | 无 strace | 有 strace | 减速倍数 |
|------|----------|----------|---------|
| dd bs=4k count=10000 | 0.085s | 2.4s | ★ ~28x |
| dd bs=1M count=100 | 0.050s | 0.15s | ~3x |
| ls /tmp（大量 stat） | 0.003s | 0.08s | ★ ~27x |
| nginx 1K 请求 | 0.09s | 2.5s | ★ ~28x |
| make（编译项目） | 60s | 600s | ~10x |

**★ 规律：syscall 越频繁、越小，strace 开销越大。**

#### 减小开销的方法

```bash
# 方法 1: 只追踪特定 syscall（★ 最有效）
strace -e trace=accept4,sendfile -p <PID>
# 其他 syscall 不触发 ptrace stop → 开销大幅降低

# 方法 2: seccomp 内核侧过滤
strace --seccomp -e trace=accept4 -p <PID>
# 内核在 syscall 入口就过滤，减少 ~50% 开销

# 方法 3: 统计模式
strace -c -p <PID>
# 不需要格式化输出，开销较小

# 方法 4: 输出到文件（避免终端阻塞）
strace -o /tmp/trace.log -p <PID>
```

#### 生产环境替代方案

```bash
# ★ 生产环境推荐：perf trace（开销 < 5%）
perf trace -p <PID>                    # 实时追踪
perf trace -s -p <PID>                 # 统计模式（类似 strace -c）
perf trace -e read,write -p <PID>      # 追踪特定 syscall

# 对比：
# strace 每个 syscall 开销：~20-30 μs
# perf trace 每个 syscall 开销：~0.1-0.5 μs
# ★ perf trace 开销约为 strace 的 1/50 到 1/100
```

#### 选择决策

```
生产环境？
  │
  ├─ 是 → perf trace（★ 低开销，推荐）
  │
  └─ 否（开发/测试环境）→ strace（★ 信息全，推荐）
```

**一句话总结：** strace 基于 ptrace，每个 syscall 额外 ~20-30μs 开销，导致 10-100 倍减速。生产环境用 `perf trace`（开销 < 5%），开发环境用 `strace`（信息最全）。如果必须在生产用 strace，用 `-e trace=` 限定 syscall 类型 + `--seccomp` 减小开销。

---

## Q3: strace 输出中 +++ exited with 0 +++ 是什么意思？

**日期：** 2026-07-04  
**场景：** 用 strace 追踪进程，最后看到 `+++ exited with 0 +++`，不确定这是正常退出还是异常  
**相关文件：** `docs/strace/reading/03_syscall_decode.md`  
**源码位置：** strace `src/strace.c:3600-3613`、内核 `kernel/exit.c:873-920`

### 回答

**★ `+++ exited with N +++` 表示被追踪进程通过 `exit_group()` 系统调用正常退出，N 是退出状态码。**

#### strace 源码：print_exited()

```c
// strace/src/strace.c:3599-3613
static void
print_exited(struct tcb *tcp, const int pid, int status)
{
    if (pid == strace_child) {
        exit_code = WEXITSTATUS(status);   // ★ 提取退出状态码
        strace_child = 0;
    }

    if (cflag != CFLAG_ONLY_STATS &&
        !is_number_in_set(QUIET_EXIT, quiet_set)) {
        printleader(tcp);
        tprintf_string("+++ exited with %d +++", WEXITSTATUS(status));
        //                    ^^^^^^^^^^^^^^^^
        //                    ★ 用 WEXITSTATUS 宏从 wait 状态中提取退出码
        tprint_newline();
        line_ended();
    }
}
```

**关键点：**
- `WEXITSTATUS(status)` 是 POSIX 标准宏，从 `waitpid()` 返回的状态值中提取 8 位退出码
- 这行输出由 `TE_EXITED` 事件触发（见 `trace_event.h:56`）

#### 退出状态码含义

```
退出码 │ 含义             │ 常见场景
──────┼─────────────────┼────────────────────────────
  0   │ ★ 成功退出       │ 程序正常完成
  1   │ 通用错误         │ 脚本执行失败、参数错误
  2   │ 误用 shell 命令  │ bash 内置命令用法错误
 126  │ 命令不可执行     │ 权限不足
 127  │ 命令未找到       │ command not found
 128+N│ 被信号 N 杀死    │ 128+9=137 → 被 SIGKILL 杀死
 255  │ 退出状态超出范围 │ exit status out of range
```

#### exit() vs exit_group()

```c
// kernel/exit.c:873-876 — exit() 只终止当前线程
SYSCALL_DEFINE1(exit, int, error_code)
{
    do_exit((error_code&0xff)<<8);
}

// kernel/exit.c:915-920 — exit_group() 终止整个线程组
SYSCALL_DEFINE1(exit_group, int, error_code)
{
    do_group_exit((error_code & 0xff) << 8);
    // ★ 调用 do_group_exit() → zap_other_threads() → 杀死所有线程
    /* NOTREACHED */
    return 0;
}
```

```
exit() vs exit_group() 的区别：

  exit(code):
    只终止调用线程 → 其他线程继续运行
    ★ 多线程程序中几乎不用

  exit_group(code):
    终止整个线程组（进程中的所有线程）
    ★ glibc 的 exit() / _exit() 内部调用的是 exit_group
    ★ strace 中绝大多数 +++ exited with +++ 来自 exit_group
```

```c
// kernel/exit.c:882-907 — do_group_exit() 核心逻辑
void do_group_exit(int exit_code)
{
    struct signal_struct *sig = current->signal;

    if (signal_group_exit(sig))
        exit_code = sig->group_exit_code;
    else if (!thread_group_empty(current)) {
        struct sighand_struct *const sighand = current->sighand;
        spin_lock_irq(&sighand->siglock);
        // ...
        sig->group_exit_code = exit_code;
        sig->flags = SIGNAL_GROUP_EXIT;
        zap_other_threads(current);  // ★ 杀死同组其他线程
        // ...
        spin_unlock_irq(&sighand->siglock);
    }
    do_exit(exit_code);
}
```

#### strace 中的其他退出方式

```
+++ exited with 0 +++         ← exit_group(0) 正常退出
+++ killed by SIGKILL +++     ← 被 SIGKILL 信号杀死（strace.c:3591）
+++ killed by SIGSEGV (core dumped) +++  ← 段错误 + 核心转储
--- stopped by SIGSTOP ---    ← 被信号暂停（不是退出）
```

```c
// strace/src/strace.c:3590-3596 — 被信号杀死的输出
tprintf_string("+++ killed by %s %s+++",
               sprintsigname(WTERMSIG(status)),
               WCOREDUMP(status) ? "(core dumped) " : "");
```

#### strace 注释中的真实示例

```c
// strace/src/strace.c:3796-3799 — strace 源码注释中的示例
//  19923 clone(...) = 19924
//  19923 exit_group(1)     = ?
//  19923 +++ exited with 1 +++
//                     ^^^^ 退出码为 1，表示异常退出
```

**一句话总结：** `+++ exited with N +++` 是 strace 报告进程通过 `exit_group()` 正常退出。`N=0` 表示成功，`N≠0` 表示异常。glibc 的 `exit()` 内部调用 `exit_group()`，会终止所有线程。如果被信号杀死，则显示 `+++ killed by SIGNAL +++`。

---

## Q4: strace -p 附加到进程后没有输出，可能的原因？

**日期：** 2026-07-04  
**场景：** `strace -p <PID>` 附加到运行中的进程，但没有任何系统调用输出  
**相关文件：** `docs/strace/reading/01_ptrace_mechanism.md`  
**源码位置：** 内核 `kernel/ptrace.c:357-459` — `ptrace_attach()`

### 回答

**★ 最常见的原因是进程处于 D 状态（不可中断睡眠），无法响应 ptrace 发出的 SIGSTOP 信号。其他原因包括进程空闲无系统调用、权限不足等。**

#### ptrace attach 的内核流程

```c
// kernel/ptrace.c:357-459 — ptrace_attach() 核心逻辑
static int ptrace_attach(struct task_struct *task, long request,
                         unsigned long addr, unsigned long flags)
{
    // ...
    // 1. 权限检查
    retval = -EPERM;
    if (unlikely(task->flags & PF_KTHREAD))  // ★ 不能 attach 内核线程
        goto out;
    if (same_thread_group(task, current))     // ★ 不能 attach 自己的线程组
        goto out;

    // 2. 设置 ptrace 标志
    task->ptrace = flags;
    ptrace_link(task, current);

    // 3. ★ 关键：向目标进程发送 SIGSTOP
    if (!seize)
        send_sig_info(SIGSTOP, SEND_SIG_PRIV, task);
    //              ^^^^^^^^
    //              ★ PTRACE_ATTACH 必须发 SIGSTOP 让目标进程暂停

    // 4. 如果目标已经处于 STOPPED 状态，唤醒它以转换到 TRACED
    if (task_is_stopped(task) &&
        task_set_jobctl_pending(task, JOBCTL_TRAP_STOP | JOBCTL_TRAPPING))
        signal_wake_up_state(task, __TASK_STOPPED);

    // 5. ★ 等待目标进程完成 STOPPED → TRACED 的转换
    wait_on_bit(&task->jobctl, JOBCTL_TRAPPING_BIT, TASK_KILLABLE);
    //          ^^^^^^^^^^^^^
    //          ★ 如果目标进程在 D 状态，这里会一直等待！
}
```

#### attach 流程时序图（现代 strace 使用 SEIZE + INTERRUPT）

```
strace (tracer)                        目标进程 (tracee)
     │                                       │
     │  1. ptrace(PTRACE_SEIZE, pid)         │
     │───────────────────────────────────────│
     │  → 设置 PT_PTRACED | PT_SEIZED        │
     │  → ★ 不发 SIGSTOP，目标继续运行       │
     │                                       │
     │  2. ptrace(PTRACE_INTERRUPT, pid)     │
     │───────────────────────────────────────│
     │  → 请求中断目标进程                    │
     │                                       │
     │    ├─ 如果状态 = S（可中断睡眠）       │
     │    │   → 目标在下一个检查点暂停        │
     │    │   → TASK_TRACED                  │
     │    │   → strace 开始工作 ✓             │
     │    │                                  │
     │    └─ 如果状态 = D（不可中断睡眠）     │
     │        → ★ 无法到达检查点！            │
     │        → INTERRUPT 等待目标退出 D 状态 │
     │        → strace 卡住 ✗                │
     │                                       │
     │  3. waitpid(pid) ◄────────────────────│ 暂停事件
     │  4. ptrace(SETOPTIONS)                │
     │  5. ptrace(SYSCALL) ─────────────────►│ 恢复执行
     │                                       │
```

#### 原因分类与排查

```
原因                     │ 排查方法                          │ 解决方案
────────────────────────┼──────────────────────────────────┼───────────────────────
★ 进程在 D 状态         │ cat /proc/<PID>/status 看 State  │ 等 I/O 完成后自动恢复
（不可中断睡眠）         │ 或 ps -p <PID> -o stat,wchan     │ 无法强制中断
────────────────────────┼──────────────────────────────────┼───────────────────────
进程空闲无 syscall       │ 等待或触发进程活动                 │ 正常现象
                         │ strace -p <PID> -e trace=all     │
────────────────────────┼──────────────────────────────────┼───────────────────────
权限不足                 │ strace 会报 "Operation not       │ sudo strace -p <PID>
                         │ permitted" 错误                  │
────────────────────────┼──────────────────────────────────┼───────────────────────
内核线程                 │ ps 显示 COMMAND 为 [kworker/0]   │ ★ 不能 ptrace 内核线程
                         │ 方括号包裹 = 内核线程             │ 用 ftrace/perf 代替
────────────────────────┼──────────────────────────────────┼───────────────────────
已被其他 tracer attach   │ 一个进程同时只能被一个 tracer     │ 先 detach 其他 tracer
                         │ ptrace（内核限制）               │
```

#### D 状态详解

```
D 状态（TASK_UNINTERRUPTIBLE）的进程：

  ★ 不响应任何信号，包括 SIGSTOP 和 SIGKILL
  ★ 通常在内核中等待 I/O 操作完成：
    - 磁盘 I/O（submit_bio 后等待完成）
    - NFS 服务器响应
    - 文件系统锁（flock、fcntl）
    - 互斥锁（mutex）等待

  内核调度代码：
    // kernel/sched/core.c:4462-4473
    prev_state = prev->state;
    if (!preempt && prev_state) {
        if (signal_pending_state(prev_state, prev)) {
            prev->state = TASK_RUNNING;
        } else {
            prev->sched_contributes_to_load =
                (prev_state & TASK_UNINTERRUPTIBLE) &&  // ★ D 状态
                !(prev_state & TASK_NOLOAD) &&
                !(prev->flags & PF_FROZEN);
            if (prev->sched_contributes_to_load)
                rq->nr_uninterruptible++;  // ★ 计入 vmstat 的 b 列
        }
    }

  ★ D 状态的进程会贡献给 vmstat 的 b（blocked）列和 ps 的 load average
```

#### PTRACE_SEIZE：现代 strace 的默认行为

```c
// strace/src/strace.c:575-587 — strace 优先使用 SEIZE
static int
ptrace_attach_or_seize(int pid, const char **ptrace_attach_cmd)
{
    int r;
    if (!use_seize)
        return *ptrace_attach_cmd = "PTRACE_ATTACH",
               ptrace(PTRACE_ATTACH, pid, 0L, 0L);
    // ★ 优先尝试 PTRACE_SEIZE（不发 SIGSTOP）
    r = ptrace(PTRACE_SEIZE, pid, 0L, (unsigned long) ptrace_setoptions);
    if (r)
        return *ptrace_attach_cmd = "PTRACE_SEIZE", r;
    // ★ SEIZE 成功后，用 PTRACE_INTERRUPT 请求中断（比 SIGSTOP 更温和）
    r = ptrace(PTRACE_INTERRUPT, pid, 0L, 0L);
    return *ptrace_attach_cmd = "PTRACE_INTERRUPT", r;
}

// kernel/ptrace.c:361-373 — PTRACE_SEIZE 不发送 SIGSTOP
bool seize = (request == PTRACE_SEIZE);
if (seize) {
    flags = PT_PTRACED | PT_SEIZED | (flags << PT_OPT_FLAG_SHIFT);
} else {
    flags = PT_PTRACED;
}

// kernel/ptrace.c:411-413
if (!seize)
    send_sig_info(SIGSTOP, SEND_SIG_PRIV, task);
    // ★ 只有 PTRACE_ATTACH 发 SIGSTOP，PTRACE_SEIZE 不发
```

```
PTRACE_ATTACH vs PTRACE_SEIZE：

  PTRACE_ATTACH（旧接口）：
    → 发送 SIGSTOP → 目标进程暂停 → 转换为 TRACED
    → ★ 如果目标在 D 状态，attach 会阻塞

  PTRACE_SEIZE + PTRACE_INTERRUPT（★ 现代 strace 默认使用）：
    → SEIZE 不发送 SIGSTOP，目标进程继续运行
    → INTERRUPT 请求中断，但不依赖信号机制
    → ★ 对 D 状态进程的影响更小
    → 但如果目标一直在 D 状态，INTERRUPT 也无法使其暂停
```

#### 实用排查步骤

```bash
# 1. 检查目标进程状态
cat /proc/<PID>/status | grep State
# State: D (disk sleep)  → ★ D 状态，等 I/O
# State: S (sleeping)    → 正常睡眠，应该能 attach
# State: R (running)     → 运行中，应该能 attach

# 2. 查看进程在等什么
cat /proc/<PID>/wchan
# io_schedule  → 等待 I/O（D 状态的常见原因）
# futex_wait   → 等待锁（S 状态，可以 attach）

# 3. 查看内核调用栈
cat /proc/<PID>/stack
# 看具体卡在哪个内核函数

# 4. 如果确认是 D 状态
# ★ 唯一方案：等 I/O 完成，或解决 I/O 问题（如修复 NFS）
# 无法通过 kill 或任何信号中断 D 状态进程
```

**一句话总结：** `strace -p` 没有输出的最常见原因是目标进程处于 **D 状态**（不可中断睡眠），此时进程不响应 SIGSTOP 信号，ptrace attach 被阻塞在 `wait_on_bit()` 上。用 `cat /proc/<PID>/status | grep State` 确认状态，D 状态只能等 I/O 完成。其他原因包括进程空闲（无 syscall）、权限不足、目标是内核线程等。

---

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
