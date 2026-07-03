# strace 完整命令参考与源码实现

> 每个常用命令选项的实现源码位置，以及完整的过滤器分类速查表。
> 所有参数均标注了在 strace 源码中的对应位置。

---

## 一、基本用法

```bash
# 追踪命令执行
strace [选项] <command> [args...]

# 附加到运行中的进程
strace [选项] -p <PID>
```

---

## 二、参数与源码实现对照

### 核心参数

| 参数 | 含义 | 源码位置 | 默认值 |
|------|------|---------|--------|
| ★ `-e trace=xxx` | 过滤要追踪的系统调用 | `filter_qualify.c: qualify_syscall()` | `trace=all` |
| ★ `-p PID` | 附加到运行中的进程 | `strace.c: startup_attach()` | — |
| ★ `-f` | 追踪 fork/clone 出的子进程 | `strace.c: followfork` + `PTRACE_O_TRACECLONE` | 不追踪 |
| ★ `-c` | 统计模式（只计数不输出） | `count.c: count_syscall()` + `cflag` | 关闭 |
| ★ `-T` | 显示每次 syscall 耗时 | `syscall.c: Tflag` + `syscall_exiting_trace()` | 关闭 |
| `-t` | 显示时间戳（HH:MM:SS） | `syscall.c: tflag` | 关闭 |
| `-tt` | 显示微秒级时间戳 | `syscall.c: tflag == 2` | 关闭 |
| `-ttt` | 显示 epoch 微秒时间戳 | `syscall.c: tflag == 3` | 关闭 |
| `-o file` | 输出到文件 | `strace.c: outf` + `open_file()` | stderr |
| `-S sort` | 统计报告排序方式 | `count.c: summary_sortby` | `time` |
| `-v` | 详细输出（不缩写） | `filter_qualify.c: verbose_set` | 缩写 |
| `-s strsize` | 字符串最大显示长度 | `strace.c: max_strlen` | ★ 32 |
| `-y` | 显示 fd 对应的路径 | `defs.h: show_fd_path` | 关闭 |
| `-yy` | 显示 fd 的完整协议信息 | `defs.h: show_fd_path == 2` | 关闭 |
| `-r` | 显示相对时间戳 | `syscall.c: rflag` | 关闭 |
| `-x` | 十六进制显示非 ASCII 字符串 | `strace.c: xflag` | 关闭 |
| `-xx` | 十六进制显示所有字符串 | `strace.c: xflag == 2` | 关闭 |
| ★ `-P path` | 只追踪访问指定路径的 syscall | `pathtrace.c: pathtrace_match()` | — |
| ★ `--seccomp` | 启用 seccomp 内核侧过滤 | `filter_seccomp.c` | 关闭 |

### 进阶参数

| 参数 | 含义 | 源码位置 |
|------|------|---------|
| ★ `-e inject=xxx` | 故障注入（篡改 syscall 返回值） | `syscall.c: tamper_with_syscall_entering()` |
| `-e signal=xxx` | 追踪指定信号 | `filter_qualify.c: signal_set` |
| `-e status=xxx` | 按返回状态过滤输出 | `filter_qualify.c: status_set` |
| `-e quiet=xxx` | 抑制特定信息 | `filter_qualify.c: quiet_set` |
| `-e read=fd` | 显示从指定 fd 读取的数据 | `util.c: dumpio` |
| `-e write=fd` | 显示写入指定 fd 的数据 | `util.c: dumpio` |
| `-D` | 在独立子进程中运行 tracer | `strace.c: daemonized_tracer` |
| `-I ms` | 中断延迟（用于调试） | `strace.c: interrupt_timeout` |
| ★ `--kill-on-exit` | strace 退出时杀死所有 tracee | `strace.c: PTRACE_O_EXITKILL` |
| `--syscall-limit=N` | 追踪 N 个 syscall 后分离 | `strace.c: syscall_limit` |

---

## 三、`-e trace=` 过滤器完整分类

### ★ 内置分类关键字

`-e trace=xxx` 支持以下预定义分类，每个分类对应 sysent.h 中的 TRACE_* 标志位：

```
分类关键字     │ 对应标志位         │ 包含的系统调用
──────────────┼───────────────────┼────────────────────────────────────────────
★ file           │ TRACE_FILE        │ openat, read, write, close, stat, chmod,
               │  (000000001)      │ chown, link, symlink, rename, mkdir, ...
               │                   │ （所有涉及文件名/路径的 syscall）
★ network        │ TRACE_NETWORK     │ socket, connect, bind, listen, accept,
               │  (000000004)      │ sendto, recvfrom, sendmsg, recvmsg,
               │                   │ setsockopt, getsockopt, ...
★ process        │ TRACE_PROCESS     │ clone, fork, vfork, execve, exit,
               │  (000000010)      │ wait4, kill, getpid, getppid, ...
signal         │ TRACE_SIGNAL      │ kill, tkill, tgkill, sigaction,
               │  (000000020)      │ sigprocmask, signalfd, rt_sigreturn, ...
★ desc           │ TRACE_DESC        │ read, write, pread, pwrite, select,
               │  (000000040)      │ poll, epoll, close, dup, fcntl, ...
ipc            │ TRACE_IPC         │ msgget, msgsnd, msgrcv, semget,
               │  (000000002)      │ semop, shmget, shmat, shmdt, ...
memory         │ TRACE_MEMORY      │ mmap, munmap, mprotect, mremap,
               │  (000000100)      │ brk, madvise, mlock, ...
```

### 过滤语法

```bash
# 单个分类
strace -e trace=file ls /tmp

# 多个分类（逗号分隔）
strace -e trace=file,network curl http://example.com

# 单个系统调用名
strace -e trace=openat,read,write cat /etc/hosts

# ★ 否定（排除某些 syscall）
strace -e trace='!mmap,mprotect,munmap' ls /tmp

# 全部（默认值）
strace -e trace=all ls /tmp

# 混合分类和系统调用名
strace -e trace=file,mmap,getpid ls /tmp
```

### 源码实现流程

```
-e trace=file,read,write
    │
    ▼
init() 中调用 qualify_trace("file,read,write")
    │
    ▼
filter_qualify.c: qualify_syscall()
    │
    ├── "file" → 遍历 sysent[]，对所有 sys_flags & TRACE_FILE 的 syscall
    │             设置 qual_flags[scno] = QUAL_TRACE
    ├── "read"  → 找到 scno=0，设置 qual_flags[0] = QUAL_TRACE
    └── "write" → 找到 scno=1，设置 qual_flags[1] = QUAL_TRACE
    │
    ▼
syscall_entering_trace() 检查 traced(tcp)
    │
    ├── 匹配（qual_flags[scno] & QUAL_TRACE）→ 正常解码输出
    └── 不匹配 → 设置 TCB_FILTERED，跳过输出
                  ★ 但仍用 PTRACE_SYSCALL 继续追踪（不停止追踪！）
```

> ★ **重要理解：** 过滤只影响"输出"，不影响"追踪"。
> 所有 syscall 仍然触发 ptrace 停止，只是不匹配的不会打印。
> 这是 strace 性能开销大的根本原因之一。
> `--seccomp` 选项可以在内核侧跳过停止，大幅降低开销。

---

## 四、`-e` 系列其他过滤器

### `-e status=` — 按返回状态过滤

```bash
# 只显示失败的 syscall
strace -e status=failed ls /tmp

# 只显示成功的 syscall
strace -e status=successful ls /tmp

# 未完成的 syscall
strace -e status=unfinished ls /tmp
```

可选值：`successful`、`failed`、`unfinished`、`unavailable`、`detached`、`all`

### `-e signal=` — 追踪指定信号

```bash
# 只显示 SIGTERM 和 SIGCHLD
strace -e signal=SIGTERM,SIGCHLD bash -c "sleep 1; exit"
```

### `-e quiet=` — 抑制特定信息

```bash
# 抑制 attach/detach 消息
strace -e quiet=attach -p 1234

# 抑制进程退出消息
strace -e quiet=exit ls /tmp

# 抑制路径解析错误
strace -e quiet=path-resolution cat /etc/hosts
```

### `-e read=` / `-e write=` — 数据转储

```bash
# 显示从 fd 3 读取的数据内容
strace -e read=3 cat /etc/hosts

# 显示写入 fd 1 的数据内容
strace -e write=1 echo "hello world"
```

---

## 五、统计模式（`-c`）详解

### 基本用法

```bash
strace -c ls /tmp                    # 统计所有 syscall
strace -c -S time ls /tmp            # 按耗时排序
strace -c -S calls ls /tmp           # 按调用次数排序
strace -c -S name ls /tmp            # 按名称排序
strace -c -e trace=file ls /tmp      # 只统计文件相关 syscall
```

### 输出列含义

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
```

```
列名        │ 含义                          │ 源码位置
───────────┼──────────────────────────────┼───────────────────────
% time     │ 该 syscall 占总耗时的百分比   │ count.c: CSC_TIME_100S
seconds    │ 该 syscall 的总耗时（秒）     │ count.c: CSC_TIME_TOTAL
usecs/call │ 平均每次调用耗时（微秒）      │ count.c: CSC_TIME_AVG
calls      │ 调用次数                      │ count.c: CSC_CALLS
errors     │ 返回错误的次数                │ count.c: CSC_ERRORS
syscall    │ 系统调用名称                  │ count.c: CSC_SC_NAME
```

### 源码实现

```c
// count.c 中的统计结构体
struct call_counts {
    struct timespec time;         // 累计耗时
    struct timespec time_min;     // 最小耗时
    struct timespec time_max;     // 最大耗时
    struct timespec time_avg;     // 平均耗时
    uint64_t calls, errors;       // 调用次数和错误次数
};
```

统计在 `syscall_exiting_trace()` 中累加，退出时调用 `printsummary()` 打印表格。

---

## 六、时间戳选项对比

```
选项    │ 输出格式                      │ 示例
───────┼──────────────────────────────┼────────────────────────
-t      │ HH:MM:SS                     │ 14:30:25 execve(...)
-tt     │ HH:MM:SS.microseconds        │ 14:30:25.123456 execve(...)
-ttt    │ epoch_seconds.microseconds   │ 1719993025.123456 execve(...)
-r      │ 相对上一行的时间间隔          │ 0.000123 execve(...)
-T      │ 每次 syscall 耗时（追加）     │ execve(...) = 0 <0.000608>
```

> `-t`/`-tt`/`-ttt` 和 `-r` 可以组合使用，但 `-T` 是独立追加的列。

---

## 七、常用场景命令集

### 场景 1：追踪程序启动过程

```bash
# 看 ls 启动时加载了哪些共享库
strace -e trace=openat,mmap ls /tmp

# 看 cat 读取了什么文件
strace -e trace=file cat /etc/hosts
```

### 场景 2：诊断文件找不到

```bash
# 查看所有 ENOENT 错误
strace -e status=failed ls /tmp
# 或者只看文件相关失败
strace -e trace=file -e status=failed ./my_program
```

### 场景 3：追踪网络请求

```bash
# 追踪 curl 的所有网络操作
strace -e trace=network curl -s http://example.com

# 追踪某个服务的网络连接
strace -e trace=network -p $(pidof nginx) -f
```

### 场景 4：性能分析

```bash
# 找出最耗时的系统调用
strace -c -S time find /usr -name "*.h"

# 显示每个 syscall 的耗时
strace -T ls /tmp | sort -t'<' -k2 -rn | head -10
```

### 场景 5：追踪子进程树

```bash
# 追踪 make 构建过程的所有子进程
strace -f -e trace=process make

# 追踪 bash 脚本的所有 fork/exec
strace -f -e trace=process,clone,fork,execve bash -c "echo hi; ls"
```

### 场景 6：追踪特定文件

```bash
# 只追踪访问 /etc/passwd 的操作
strace -P /etc/passwd cat /etc/passwd

# 追踪访问 /dev 目录下的任何文件
strace -P /dev/ ls /dev
```

### 场景 7：输出到文件 + 带时间戳

```bash
# 输出到文件，带微秒时间戳
strace -tt -o trace.log ls /tmp

# 多进程输出到不同文件
strace -f -ff -o trace ls /tmp
# 生成 trace.PID1, trace.PID2, ... 多个文件
```

### 场景 8：故障注入（测试错误处理）

```bash
# 让 openat 总是返回 ENOENT
strace -e inject=openat:error=ENOENT cat /etc/hosts

# 让 50% 的 read 返回 EIO
strace -e inject=read:error=EIO:when=2+2 cat /etc/hosts

# 让 write 延迟 100ms
strace -e inject=write:delay_enter=100ms echo "hello"
```

### 场景 9：附加到已运行的进程

```bash
# 追踪 nginx worker
strace -p $(pgrep -f "nginx: worker") -T -e trace=network

# 追踪并显示 fd 路径
strace -p 1234 -y -e trace=desc

# 追踪多线程进程的所有线程
strace -p $(pidof myapp) -f
```

### 场景 10：seccomp 优化（减少开销）

```bash
# 使用 seccomp 过滤减少 ptrace 开销
strace --seccomp -e trace=file ls /tmp

# 对比：普通模式 vs seccomp 模式
time strace -c ls /tmp              # 普通模式
time strace --seccomp -c ls /tmp    # seccomp 模式（更快）
```

### 场景 11：查看环境变量

```bash
# -v 显示完整环境变量（execve 的第 3 个参数）
strace -v -e trace=execve env
# 对比不加 -v 时的缩写输出
strace -e trace=execve env
```

### 场景 12：查看完整 stat 结构

```bash
# 默认：stat 输出用 "..." 缩写
strace -e trace=newfstatat stat /etc/hosts

# -v：显示 stat 结构体的所有字段
strace -v -e trace=newfstatat stat /etc/hosts
```

---

## 八、源码文件快速索引

```bash
# 找 sysent 表结构定义
grep -n "struct sysent" src/strace/src/sysent.h

# 找 TRACE_* 标志位定义
grep -n "TRACE_FILE\|TRACE_NETWORK\|TRACE_PROCESS" src/strace/src/sysent.h

# 找 tcb 结构定义
grep -n "struct tcb" src/strace/src/defs.h

# 找过滤器实现
grep -n "qualify_syscall\|qual_flags" src/strace/src/filter_qualify.c

# 找统计计数实现
grep -n "call_counts\|printsummary\|count_syscall" src/strace/src/count.c

# 找 seccomp 过滤实现
grep -n "seccomp" src/strace/src/filter_seccomp.c

# 找路径追踪实现
grep -n "pathtrace_match" src/strace/src/pathtrace.c

# 找 ptrace 附加逻辑
grep -n "ptrace_attach_or_seize\|startup_attach" src/strace/src/strace.c

# 找 syscall 解码流水线
grep -n "syscall_entering\|syscall_exiting\|trace_syscall" src/strace/src/syscall.c

# 找 x86_64 系统调用表
grep -n "sysent0\|SEN_" src/strace/src/linux/x86_64/syscallent.h | head -20
```

---

## 九、常用命令组合速查

```
目的                    │ 命令
───────────────────────┼──────────────────────────────────────────────
快速诊断启动问题       │ strace -e trace=file,openat <command>
找出 ENOENT 错误       │ strace -e status=failed <command>
分析耗时分布           │ strace -c -S time <command>
追踪网络行为           │ strace -e trace=network <command>
追踪子进程树           │ strace -f -e trace=process <command>
追踪特定文件           │ strace -P /path/to/file <command>
带时间戳输出到文件     │ strace -tt -o trace.log <command>
每个 syscall 耗时       │ strace -T <command>
减少追踪开销           │ strace --seccomp -e trace=file <command>
附加到进程追踪网络     │ strace -f -e trace=network -p <PID>
故障注入测试           │ strace -e inject=openat:error=ENOENT <command>
显示 fd 对应路径       │ strace -y -e trace=desc <command>
```
