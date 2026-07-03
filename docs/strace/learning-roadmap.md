# strace 学习路线

> 从 ptrace 机制到系统调用解码，从进程追踪到性能分析

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 源码阅读路线图](#2-源码阅读路线图)
  - [Step 1: ptrace 系统调用机制](#step-1-ptrace-系统调用机制)
  - [Step 2: strace main() 与初始化流程](#step-2-strace-main-与初始化流程)
  - [Step 3: 主循环 — wait 与 dispatch](#step-3-主循环--wait-与-dispatch)
  - [Step 4: 系统调用解码 — 从寄存器到可读输出](#step-4-系统调用解码--从寄存器到可读输出)
  - [Step 5: sysent 表与 syscall 分发](#step-5-sysent-表与-syscall-分发)
  - [Step 6: 过滤器与高级功能](#step-6-过滤器与高级功能)
- [3. 核心概念：ptrace 工作原理](#3-核心概念ptrace-工作原理)
- [4. 动手实验清单](#4-动手实验清单)
- [5. 关键源码文件索引](#5-关键源码文件索引)

---

## 1. 整体架构

```
┌──────────────────────────────────────────────────────────┐
│                    strace 用户态工具                       │
│  main() → init() → startup_child()/startup_attach()      │
│  → 主循环: next_event() → dispatch_event()               │
│           → trace_syscall() → 解码 → 输出                  │
├──────────────────────────────────────────────────────────┤
│                    ptrace 接口层                           │
│  PTRACE_TRACEME / PTRACE_SEIZE / PTRACE_ATTACH           │
│  PTRACE_SYSCALL（进入/退出 系统调用停止）                    │
│  PTRACE_GET_SYSCALL_INFO（新式，内核 5.3+）                 │
│  PTRACE_GETREGS / PTRACE_GETREGSET（读取寄存器）            │
│  PTRACE_GETEVENTMSG（获取事件消息）                         │
│  PTRACE_SETOPTIONS（设置追踪选项）                          │
├──────────────────────────────────────────────────────────┤
│                    Linux 内核                              │
│  系统调用入口/出口 → ptrace_stop()                         │
│  SIGTRAP | 0x80 信号通知 tracer                           │
│  wait4() 返回被追踪进程的状态变化                            │
└──────────────────────────────────────────────────────────┘
```

### 数据流全路径

```
 被追踪进程（tracee）发起系统调用
     │
     ▼
 内核系统调用入口
     │
     ▼
 ┌── ptrace_stop() ──────────────────────────────┐
 │  暂停 tracee，向 tracer 发送 SIGTRAP | 0x80    │  ← syscall-enter-stop
 └────────────────────────────────────────────────┘
     │
     ▼
 tracer 的 wait4() 返回
     │
     ▼
 ┌── next_event() ───────────────────────────────┐
 │  分析 wait status                              │
 │  判断是 syscall-enter 还是 syscall-exit        │
 └────────────────────────────────────────────────┘
     │
     ▼
 ┌── trace_syscall() ────────────────────────────┐
 │  进入时：get_scno() → get_syscall_args()       │
 │         → syscall_entering_trace()             │
 │  退出时：get_error() → syscall_exiting_trace()  │
 └────────────────────────────────────────────────┘
     │
     ▼
 ┌── 系统调用解码 ────────────────────────────────┐
 │  sysent[scno].sys_func(tcp)                    │
 │  → 各 syscall 专属解码函数（如 sys_read、sys_openat）│
 │  → 读取 tracee 内存中的参数                      │
 │  → 格式化输出                                    │
 └────────────────────────────────────────────────┘
     │
     ▼
 ptrace(PTRACE_SYSCALL) → 继续追踪，等待下一次停止
```

---

## 2. 源码阅读路线图

### Step 1: ptrace 系统调用机制

**目标：** 理解 strace 赖以生存的 ptrace 基础设施

**文件位置：**
- 内核 ptrace 实现：`src/linux-5.10/kernel/ptrace.c`
- strace ptrace 封装：`src/strace/src/ptrace.c`
- strace ptrace 选项：`src/strace/src/strace.c` (搜索 `PTRACE_O_`)

**阅读要点：**

ptrace 是 Linux 内核提供的进程追踪机制，strace 完全建立在它之上：

| ptrace 请求 | 用途 | strace 使用场景 |
|-------------|------|----------------|
| `PTRACE_TRACEME` | 子进程声明"我要被追踪" | `exec_or_die()` 中子进程调用 |
| `PTRACE_ATTACH` | 附加到已运行的进程 | `startup_attach()` 中 `-p` 模式 |
| `PTRACE_SEIZE` | 新式附加（不发送 SIGSTOP） | 替代 ATTACH，更优雅 |
| `PTRACE_SYSCALL` | 恢复执行，在 syscall 入口/出口停止 | 主循环每次迭代都调用 |
| `PTRACE_GETREGS` | 读取寄存器 | 获取 syscall 号和参数 |
| `PTRACE_GETREGSET` | 读取寄存器（新式，更灵活） | 替代 GETREGS |
| `PTRACE_GET_SYSCALL_INFO` | 直接获取 syscall 信息 | 内核 5.3+，最高效 |
| `PTRACE_GETEVENTMSG` | 获取事件消息（如 clone 的子 PID） | 追踪 fork/clone 时 |
| `PTRACE_SETOPTIONS` | 设置追踪选项 | 启用 TRACESYSGOOD 等 |
| `PTRACE_CONT` | 恢复执行 | 非 syscall 停止时 |
| `PTRACE_DETACH` | 解除追踪 | strace 退出时 |

**关键标志位：**

```c
// strace.c 中的 ptrace_setoptions（初始值）
PTRACE_O_TRACESYSGOOD  // 在 syscall 停止时，信号 = SIGTRAP | 0x80
PTRACE_O_TRACEEXEC     // exec 时产生 PTRACE_EVENT_EXEC
PTRACE_O_TRACEEXIT     // exit 时产生 PTRACE_EVENT_EXIT

// -f 选项追加：
PTRACE_O_TRACECLONE    // clone 时产生 PTRACE_EVENT_CLONE
PTRACE_O_TRACEFORK     // fork 时产生 PTRACE_EVENT_FORK
PTRACE_O_TRACEVFORK    // vfork 时产生 PTRACE_EVENT_VFORK
```

**思考题：**
1. 为什么 `SIGTRAP | 0x80` 而不是纯 `SIGTRAP`？（答：区分 syscall 停止和断点/单步等其他 TRAP 类型）
2. `PTRACE_SEIZE` 比 `PTRACE_ATTACH` 好在哪里？（答：不发送 SIGSTOP，不改变被追踪进程的信号状态）
3. 为什么 strace 对每个 syscall 都要两次停止（enter + exit）？（答：enter 时读参数，exit 时读返回值）

---

### Step 2: strace main() 与初始化流程

**目标：** 理解 strace 从启动到开始追踪的完整初始化路径

**文件位置：** `src/strace/src/strace.c`

**main() 执行流程：**

```
main() (line 4398)
 │
 ├── setlocale(LC_ALL, "")
 │
 ├── init(argc, argv)                    ← 核心初始化
 │    ├── 解析命令行参数（getopt_long）
 │    │    -e trace/expr/status/...
 │    │    -p PID, -f, -c, -T, -t, -o, -S
 │    │    -v, -y, -r, -s, -x, ...
 │    ├── 初始化过滤器（qualify_*）
 │    ├── 打开输出文件（-o）
 │    ├── 设置信号处理器
 │    │
 │    ├── startup_child(argv, env)         ← 启动子进程模式
 │    │    ├── fork()
 │    │    │    ├── 子进程: ptrace(PTRACE_TRACEME)
 │    │    │    │           exec(pathname)
 │    │    │    └── 父进程: ptrace_attach_or_seize(pid)
 │    │    │               创建 tcb 结构
 │    │    │
 │    │    或
 │    │
 │    ├── startup_attach()                 ← 附加模式 (-p)
 │    │    ├── 遍历 tcbtab[]
 │    │    └── 对每个 pid 调用 attach_tcb()
 │    │         ├── ptrace(PTRACE_ATTACH/SEIZE)
 │    │         ├── waitpid() 等待停止
 │    │         └── ptrace(PTRACE_SETOPTIONS)
 │    │
 │    └── 设置 print_pid_pfx 等输出控制
 │
 ├── 主循环
 │    while (dispatch_event(next_event()))
 │        ;
 │
 └── terminate()
```

**思考题：**
1. `startup_child` 和 `startup_attach` 的区别是什么？（答：前者 fork + exec 新进程，后者附加到已有进程）
2. 为什么子进程要在 exec 之前调用 `PTRACE_TRACEME`？（答：确保 tracer 能捕获到 execve 这个 syscall）

---

### Step 3: 主循环 — wait 与 dispatch

**目标：** 理解 strace 如何同时追踪多个进程

**文件位置：** `src/strace/src/strace.c`

**核心循环：**

```c
// main() 中的核心循环（line 4405）
while (dispatch_event(next_event()))
    ;
```

**next_event() 做了什么（line 3757）：**

```
next_event()
 │
 ├── 检查 pending_tcps 队列（已排队的 tcb）
 │
 ├── 调用 wait4(-1, &status, ...)
 │    等待任意被追踪进程的状态变化
 │    ├── 返回值 > 0 → 有进程停止了
 │    ├── 返回值 == 0 → 无事件（非阻塞模式）
 │    └── 返回值 < 0 → ECHILD（所有进程已退出）
 │
 ├── 分析 wait status
 │    ├── WIFSTOPPED → 进程收到信号停止
 │    ├── WIFEXITED  → 进程退出
 │    ├── WIFSIGNALED → 进程被信号杀死
 │    └── status >> 16 → ptrace 事件（CLONE/FORK/EXEC/EXIT）
 │
 ├── pid2tcb(pid) → 找到对应的 tcb
 │
 └── 返回 struct tcb_wait_data（包含 te 和 status）
```

**dispatch_event() 做了什么（line 4074）：**

```
dispatch_event(wd)
 │
 ├── 分析 trace_event 类型
 │    ├── TE_BREAK → 退出循环
 │    ├── TE_NEXT  → 继续循环
 │    └── TE_RESTART → 处理 syscall
 │
 ├── trace_syscall(current_tcp, &restart_sig)
 │
 └── ptrace(PTRACE_SYSCALL, pid, 0, restart_sig)
      恢复 tracee，等待下一次 syscall 停止
```

**思考题：**
1. `wait4(-1, ...)` 中的 `-1` 是什么意思？（答：等待任意子进程，这样才能同时追踪多个进程）
2. 如果同时追踪 10 个进程，next_event() 如何处理？（答：每次 wait4 返回一个进程的事件，循环处理）

---

### Step 4: 系统调用解码 — 从寄存器到可读输出

**目标：** 理解 strace 如何把二进制寄存器值转换为人类可读的格式

**文件位置：** `src/strace/src/syscall.c`

**trace_syscall() 核心逻辑（line 4049）：**

```c
trace_syscall(struct tcb *tcp, unsigned int *sig)
{
    if (entering(tcp)) {
        // ★ 系统调用进入阶段
        int res = syscall_entering_decode(tcp);  // 获取 scno + 参数
        switch (res) {
        case 0: return 0;
        case 1: res = syscall_entering_trace(tcp, sig);  // 打印入口
        }
        syscall_entering_finish(tcp, res);
        return res;
    } else {
        // ★ 系统调用退出阶段
        struct timespec ts = {};
        int res = syscall_exiting_decode(tcp, &ts);
        if (res != 0)
            res = syscall_exiting_trace(tcp, &ts, res);  // 打印出口
        syscall_exiting_finish(tcp);
        return res;
    }
}
```

**解码流水线：**

```
1. get_scno()                    ← 获取系统调用号
   └── arch_get_scno()           ← 从 orig_rax 读取（x86_64）
       或 PTRACE_GET_SYSCALL_INFO

2. get_syscall_args()            ← 获取系统调用参数
   └── arch_get_syscall_args()   ← 从 rdi/rsi/rdx/r10/r8/r9 读取
       tcp->u_arg[0..5] = 寄存器值

3. sysent[scno].sys_func(tcp)   ← 调用专属解码函数
   例如 read → sys_read(tcp)
        → printargs(tcp)
        → 打印 "read(3, "...", 832)"

4. syscall_exiting_trace()       ← 打印返回值和耗时
   → get_error() 从 rax 获取返回值/errno
   → 打印 "= 832" 或 "= -1 ENOENT (No such file...)"
```

**x86_64 寄存器到系统调用参数的映射：**

| 寄存器 | 用途 | strace 中的访问 |
|--------|------|----------------|
| `orig_rax` | 系统调用号 | `x86_64_regs.orig_rax` |
| `rdi` | 参数 1 (fd / pathname / ...) | `tcp->u_arg[0]` |
| `rsi` | 参数 2 (buf / flags / ...) | `tcp->u_arg[1]` |
| `rdx` | 参数 3 (count / mode / ...) | `tcp->u_arg[2]` |
| `r10` | 参数 4 (offset / ...) | `tcp->u_arg[3]` |
| `r8`  | 参数 5 | `tcp->u_arg[4]` |
| `r9`  | 参数 6 | `tcp->u_arg[5]` |
| `rax` | 返回值（退出时） | `tcp->u_rval` / `tcp->u_error` |

> **注意：** x86_64 系统调用使用 `r10` 而非 `rcx` 作为第 4 个参数（`rcx` 被内核用来保存用户态 `rip`）。

**思考题：**
1. 为什么需要 `orig_rax` 而不是 `rax`？（答：`rax` 在系统调用返回时被覆盖为返回值，`orig_rax` 保存了原始的调用号）
2. 为什么 `PTRACE_GET_SYSCALL_INFO` 比 `PTRACE_GETREGS` 更好？（答：一次调用获取所有信息，减少 ptrace 调用次数，且不受架构差异影响）

---

### Step 5: sysent 表与 syscall 分发

**目标：** 理解 strace 如何为每个系统调用匹配解码函数

**文件位置：**
- `src/strace/src/sysent.h` — 结构体定义
- `src/strace/src/syscall.c` — sysent 数组
- `src/strace/src/linux/x86_64/syscallent.h` — x86_64 系统调用表

**sysent 结构体：**

```c
// sysent.h
typedef struct sysent {
    unsigned nargs;              // 参数个数
    int sys_flags;               // 标志位（TRACE_FILE、TRACE_NETWORK 等）
    int sen;                     // syscall enum（用于 switch 分发）
    int (*sys_func)(struct tcb *); // ★ 解码函数指针
    const char *sys_name;        // 系统调用名（"read"、"write" 等）
} struct_sysent;
```

**sys_flags 分类标志：**

```c
#define TRACE_FILE    000000001  // 文件相关（openat, read, write, ...）
#define TRACE_IPC     000000002  // IPC 相关（msgget, semop, ...）
#define TRACE_NETWORK 000000004  // 网络相关（socket, connect, send, ...）
#define TRACE_PROCESS 000000010  // 进程相关（fork, clone, exec, ...）
#define TRACE_SIGNAL  000000020  // 信号相关（kill, sigaction, ...）
#define TRACE_DESC    000000040  // 文件描述符相关（read, write, close, ...）
#define TRACE_MEMORY  000000100  // 内存映射相关（mmap, mprotect, ...）
```

> **`-e trace=file` 就是过滤 `sys_flags & TRACE_FILE` 的系统调用！**

**思考题：**
1. `sen` 字段的作用是什么？（答：用于 `switch(sen)` 分发，比如 socketcall 的子调用解码）
2. 如果某个系统调用号没有对应的 sysent 条目怎么办？（答：strace 生成 `syscall_0xNN` 的 stub 名称）

---

### Step 6: 过滤器与高级功能

**目标：** 理解 strace 的各种过滤和高级选项的实现原理

**文件位置：**
- 过滤器：`src/strace/src/filter_qualify.c`
- seccomp 过滤：`src/strace/src/filter_seccomp.c`
- 路径追踪：`src/strace/src/pathtrace.c`
- 统计计数：`src/strace/src/count.c`

**过滤器实现层次：**

```
-e trace=read,write
    │
    ▼
qualify_syscall() → 设置 qual_flags[scno]
    │
    ▼
syscall_entering_trace() 检查 traced(tcp)
    │
    ├── 匹配 → 正常解码输出
    └── 不匹配 → 设置 TCB_FILTERED，跳过输出
                  但仍用 PTRACE_SYSCALL 继续追踪

-e trace=/path/to/file
    │
    ▼
pathtrace_match(tcp) → 检查 syscall 参数中的路径
    │
    ├── 匹配 → 输出
    └── 不匹配 → 跳过

-c (统计模式)
    │
    ▼
cflag == CFLAG_ONLY_STATS → 只计数不输出
syscall_exiting_trace() 中累加 count[]、time[]
退出时打印统计表

-seccomp (seccomp 过滤)
    │
    ▼
filter_seccomp.c → 在内核侧过滤
不匹配的 syscall 完全不触发 ptrace 停止
→ 大幅降低开销！
```

---

## 3. 核心概念：ptrace 工作原理

### ptrace 的"追踪者-被追踪者"模型

```
┌──────────────┐          ┌──────────────┐
│   tracer     │          │   tracee     │
│   (strace)   │          │   (ls/进程)   │
│              │          │              │
│  wait4()  ◄──┼──────────┼─ 系统调用入口  │
│              │          │  ptrace_stop  │
│  分析停止原因  │          │  暂停执行     │
│  读取寄存器    │          │              │
│  打印参数     │          │              │
│              │          │              │
│  PTRACE_     │          │              │
│  SYSCALL ────┼──────────┼─► 继续执行    │
│              │          │              │
│  wait4()  ◄──┼──────────┼─ 系统调用出口  │
│              │          │  ptrace_stop  │
│  读取返回值    │          │  暂停执行     │
│  打印结果     │          │              │
│              │          │              │
│  PTRACE_     │          │              │
│  SYSCALL ────┼──────────┼─► 继续执行    │
└──────────────┘          └──────────────┘
```

### 两种启动模式

**模式 1：启动新进程（`strace command`）**

```
strace                          子进程
  │                               │
  ├─ fork() ─────────────────────►│
  │                               ├─ ptrace(PTRACE_TRACEME)
  │                               ├─ exec("ls")
  │                               │   ↑ 此时内核知道有 tracer
  │                               │     execve 会触发 syscall-enter-stop
  ├─ waitpid(child) ◄─────────────┤
  ├─ ptrace(SETOPTIONS)           │
  ├─ ptrace(SYSCALL) ─────────────┼─► 继续执行
  │                               │
  │  ← 主循环开始 →                │
```

**模式 2：附加到已有进程（`strace -p PID`）**

```
strace                          目标进程
  │                               │
  ├─ ptrace(PTRACE_ATTACH, pid)   │  ← 内核发送 SIGSTOP
  ├─ waitpid(pid) ◄───────────────┤  ← 进程停止
  ├─ ptrace(SETOPTIONS)           │
  ├─ ptrace(SYSCALL) ─────────────┼─► 继续执行
  │                               │
  │  ← 主循环开始 →                │
```

### 性能影响

ptrace 的开销来自：
1. **每次 syscall 两次上下文切换**（enter-stop + exit-stop）
2. **tracer 的寄存器读取**（PTRACE_GETREGS/GETREGSET）
3. **tracer 的内存读取**（PTRACE_PEEKDATA / process_vm_readv）
4. **调度依赖**（tracee 必须等 tracer 处理完才能继续）

> **优化：** seccomp 过滤（`--seccomp`）可以在内核侧直接跳过不感兴趣的 syscall，
> 避免 ptrace 停止，大幅降低开销。

---

## 4. 动手实验清单

### 实验 1：基础追踪

```bash
# 追踪 ls 的所有系统调用
strace ls /tmp

# 只追踪文件相关系统调用
strace -e trace=file ls /tmp

# 显示每次调用的耗时
strace -T ls /tmp

# 显示绝对时间戳
strace -t ls /tmp

# 统计模式：汇总所有 syscall 的调用次数和耗时
strace -c ls /tmp
```

### 实验 2：附加到运行中的进程

```bash
# 启动一个后台进程
sleep 3600 &
SLEEP_PID=$!

# 附加到它
strace -p $SLEEP_PID -e trace=all -T

# 另开终端 kill 它
kill $SLEEP_PID
```

### 实验 3：追踪子进程

```bash
# 追踪 bash 执行命令的所有子进程
strace -f -e trace=process bash -c 'echo hello; ls /tmp'
```

### 实验 4：过滤特定文件

```bash
# 只追踪访问 /etc/passwd 的系统调用
strace -e trace=all -P /etc/passwd cat /etc/passwd
```

### 实验 5：性能分析

```bash
# 统计某个命令的 syscall 耗时分布
strace -c -S time find /usr -name "*.h" 2>&1 | head -20
```

---

## 5. 关键源码文件索引

### 核心文件

| 文件 | 关注点 |
|------|--------|
| `src/strace.c` | `main()`、`init()`、`startup_child()`、`startup_attach()`、`next_event()`、`dispatch_event()`、`trace_syscall()` |
| `src/syscall.c` | `get_scno()`、`get_syscall_args()`、`get_error()`、`syscall_entering_*()`、`syscall_exiting_*()`、`sysent[]` 表 |
| `src/defs.h` | `struct tcb` 定义、全局变量、所有核心函数声明 |
| `src/sysent.h` | `struct_sysent` 定义、`TRACE_*` 标志位 |
| `src/ptrace.c` | `ptrace(PTRACE_*)` 解码输出（当被追踪程序本身调用 ptrace 时） |

### 架构相关文件

| 文件 | 关注点 |
|------|--------|
| `src/linux/x86_64/get_scno.c` | x86_64 系统调用号获取（orig_rax） |
| `src/linux/x86_64/get_syscall_args.c` | x86_64 参数获取（rdi/rsi/rdx/r10/r8/r9） |
| `src/linux/x86_64/arch_regs.c` | x86_64 寄存器定义和读取 |
| `src/linux/x86_64/syscallent.h` | x86_64 系统调用表 |

### 功能模块

| 文件 | 关注点 |
|------|--------|
| `src/filter_qualify.c` | `-e` 过滤器实现 |
| `src/filter_seccomp.c` | seccomp 内核侧过滤 |
| `src/pathtrace.c` | `-P` 路径追踪 |
| `src/count.c` | `-c` 统计计数 |
| `src/ptrace_syscall_info.c` | `PTRACE_GET_SYSCALL_INFO` 支持 |
| `src/execve.c` | execve 系统调用解码 |
| `src/open.c` | open/openat 系统调用解码 |
| `src/net.c` | socket/connect/send/recv 解码 |

### 推荐阅读顺序

```
1. sysent.h            → 理解 syscall 表结构和 TRACE_* 标志
2. defs.h (struct tcb) → 理解核心数据结构
3. strace.c: main()    → 理解整体流程
4. strace.c: startup_child() → 理解 fork/exec/PTRACE_TRACEME
5. strace.c: next_event() + dispatch_event() → 理解主循环
6. syscall.c: get_scno() + get_syscall_args() → 理解 syscall 解码
7. syscall.c: syscall_entering_trace() + syscall_exiting_trace() → 理解输出
8. linux/x86_64/get_syscall_args.c → 理解寄存器到参数的映射
9. filter_qualify.c   → 理解 -e 过滤的实现
```

---

## 参考资源

- [strace GitHub](https://github.com/strace/strace)
- [strace 官方文档](https://strace.io/)
- [strace(1) man page](https://man7.org/linux/man-pages/man1/strace.1.html)
- [ptrace(2) man page](https://man7.org/linux/man-pages/man2/ptrace.2.html)
- [Playing with ptrace (blog)](https://blog.tartanllama.xyz/writing-a-linux-debugger-setup/)
- [How strace works (LWN)](https://lwn.net/Articles/365835/)
- [PTRACE_GET_SYSCALL_INFO (kernel commit)](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=201766a20e30)
