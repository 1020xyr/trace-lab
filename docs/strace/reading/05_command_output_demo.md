# strace 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐行解读。
> 系统：x86_64 Linux（Alibaba Cloud Linux），strace 6.12。

---

## 场景 1：基础追踪 — `strace ls /tmp`

### 命令

```bash
strace ls /tmp
```

### 输出片段（前 20 行）

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffcb63012c8 /* 55 vars */) = 0
brk(NULL)                               = 0x5575b39fe000
arch_prctl(0x3001 /* ARCH_??? */, 0x7ffd43a66f20) = -1 EINVAL (Invalid argument)
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
mmap(NULL, 17947, PROT_READ, MAP_PRIVATE, 3, 0) = 0x7ff178076000
close(3)                                = 0
openat(AT_FDCWD, "/lib64/libselinux.so.1", O_RDONLY|O_CLOEXEC) = 3
read(3, "\177ELF\2\1\1\0\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0\0\0\0\0\0\0\0\0"..., 832) = 832
newfstatat(3, "", {st_mode=S_IFREG|0755, st_size=175744, ...}, AT_EMPTY_PATH) = 0
mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7ff178074000
mmap(NULL, 181928, PROT_READ, MAP_PRIVATE|MAP_DENYWRITE, 3, 0) = 0x7ff178047000
close(3)                                = 0
openat(AT_FDCWD, "/lib64/libcap.so.2", O_RDONLY|O_CLOEXEC) = 3
read(3, "\177ELF\2\1\1\0\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0p{\0\0\0\0\0\0"..., 832) = 832
close(3)                                = 0
arch_prctl(ARCH_SET_FS, 0x7ff178039c40) = 0
set_tid_address(0x7ff178039f10)         = 452613
set_robust_list(0x7ff178039f20, 24)     = 0
```

### 逐行注解

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffcb63012c8 /* 55 vars */) = 0
│       │              │             │                              │
│       │              │             │                              └─ ★ 返回值 0 = 成功
│       │              │             └─ envp 指针 + 环境变量数量（55 vars）
│       │              └─ argv 数组
│       └─ ★ 可执行文件路径
└─ syscall 名称（execve 总是第一个！）

brk(NULL) = 0x5575b39fe000
│    │       │
│    │       └─ 返回当前堆顶地址（用于计算需要的内存大小）
│    └─ 参数 NULL = 查询当前 program break，不修改
└─ 堆管理 syscall，glibc 用来初始化堆

arch_prctl(0x3001 /* ARCH_??? */, 0x7ffd43a66f20) = -1 EINVAL
│           │                                    │
│           │                                    └─ ★ -1 = 失败，EINVAL = 无效参数
│           └─ strace 无法识别的子命令号，显示为 ARCH_???
└─ 架构特定的线程寄存器设置

access("/etc/ld.so.preload", R_OK) = -1 ENOENT (No such file or directory)
│       │                        │    │
│       │                        │    └─ ★ ENOENT = 文件不存在（预期行为）
│       │                        └─ R_OK = 检查读权限
│       └─ 预加载库列表（通常不存在）
└─ 动态链接器检查是否有预加载库

openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
│       │         │                   │                    │
│       │         │                   │                    └─ ★ 返回 fd=3
│       │         │                   └─ 只读 + 子进程执行时自动关闭
│       │         └─ 动态链接缓存（包含 .so 文件路径索引）
│       └─ AT_FDCWD = 相对于当前工作目录
└─ 现代 Linux 用 openat 替代 open（更安全，避免 TOCTOU 攻击）

newfstatat(3, "", {...st_size=17947...}, AT_EMPTY_PATH) = 0
│           │  │    │                    │                │
│           │  │    │                    │                └─ 0 = 成功
│           │  │    │                    └─ AT_EMPTY_PATH = 对 fd 本身 stat
│           │  │    └─ stat 结构体（缩写显示，-v 可看全部）
│           │  └─ 空字符串 = 不追加路径
│           └─ fd=3（刚打开的 ld.so.cache）
└─ 获取文件大小（17947 字节），为后续 mmap 做准备

mmap(NULL, 17947, PROT_READ, MAP_PRIVATE, 3, 0) = 0x7ff178076000
│       │      │      │          │           │  │   │
│       │      │      │          │           │  │   └─ ★ 返回映射的虚拟地址
│       │      │      │          │           │  └─ offset=0（从文件头开始映射）
│       │      │      │          │           └─ fd=3
│       │      │      │          └─ MAP_PRIVATE = 写时复制的私有映射
│       │      │      └─ PROT_READ = 只读
│       │      └─ 映射大小 = 文件大小
│       └─ NULL = 让内核选择映射地址
└─ 将 ld.so.cache 映射到内存（比 read 更高效）

read(3, "\177ELF\2\1\1\0..."..., 832) = 832
│     │  │                            │   │
│     │  │                            │   └─ ★ 实际读取了 832 字节
│     │  │                            └─ 请求读取 832 字节
│     │  └─ 读取内容：ELF 魔数 \177ELF + 64位标识 \2
│     └─ fd=3（libselinux.so.1）
└─ 读取共享库的 ELF 头部，验证文件格式

close(3) = 0
└─ 关闭 fd，0 = 成功
```

### 输出格式总结

```
每行格式：syscall_name(arg1, arg2, ...) = retval

★ 特殊标记：
  -1 ENOENT (No such file or directory)  → 失败：errno 名 + 描述
  0x7ff178076000                         → 返回值（指针以十六进制显示）
  "\177ELF\2\1\1\0..."                   → 字符串参数（转义显示）
  /* 55 vars */                          → strace 添加的注释
  AT_FDCWD                               → 常量名翻译（-2147483546 → AT_FDCWD）
  O_RDONLY|O_CLOEXEC                     → 位标志展开（多个标志用 | 分隔）
```

### 执行阶段分析

```
86 个 syscall 的执行阶段：

阶段 1：execve（1 次）
  └── 加载 /usr/bin/ls 可执行文件

阶段 2：动态链接（~30 次）
  ├── openat → read → fstatat → mmap → close
  │   对每个 .so 重复：ld.so.cache, libselinux.so.1,
  │   libcap.so.2, libc.so.6, libpcre2-8.so.0
  └── 共加载 5 个共享库

阶段 3：运行时初始化（~10 次）
  ├── brk, arch_prctl, set_tid_address, set_robust_list
  ├── mprotect（设置代码段为只读/可执行）
  └── rseq, prctl, getrandom

阶段 4：实际工作（~30 次）
  ├── ioctl（获取终端宽度）
  ├── statfs（检查 SELinux）
  ├── openat / getdents64（读取 /tmp 目录）
  └── write（输出目录列表到 stdout）

阶段 5：退出（1 次）
  └── exit_group(0) → +++ exited with 0 +++
```

---

## 场景 2：统计模式 — `strace -c ls /tmp`

### 命令

```bash
strace -c ls /tmp
```

### 输出（完整）

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 52.54    0.000475         475         1           execve
 22.12    0.000200          10        20           mmap
  5.97    0.000054           9         6           mprotect
  5.20    0.000047           5         8           openat
  2.10    0.000019          19         1           write
  1.99    0.000018           9         2         1 arch_prctl
  1.88    0.000017           2         6           read
  1.77    0.000016          16         1           munmap
  1.55    0.000014           1        10           close
  1.55    0.000014           1         9           newfstatat
  1.44    0.000013           4         3           brk
  0.77    0.000007           3         2         1 access
  0.33    0.000003           3         1           prlimit64
  0.33    0.000003           3         1           rseq
  0.22    0.000002           2         1           set_tid_address
  0.22    0.000002           2         1           set_robust_list
  0.00    0.000000           0         1         1 ioctl
  0.00    0.000000           0         2         2 statfs
  0.00    0.000000           0         6         4 prctl
  0.00    0.000000           0         2           getdents64
  0.00    0.000000           0         1           getrandom
  0.00    0.000000           0         1           statx
------ ----------- ----------- --------- --------- ----------------
100.00    0.000904          10        86         9 total
```

### 逐列注解

```
列名        │ 含义                                │ 示例值解读
───────────┼────────────────────────────────────┼───────────────────────────
% time     │ 该 syscall 占总耗时的百分比          │ execve 占 52.54%（一半以上！）
seconds    │ 该 syscall 的总耗时（秒）            │ execve 共 0.475ms
usecs/call │ 平均每次调用耗时（微秒）             │ execve 平均 475μs
calls      │ 调用次数                             │ mmap 调用了 20 次
errors     │ 返回 -1 的次数（空=0 次错误）        │ arch_prctl 有 1 次 EINVAL
syscall    │ 系统调用名称                         │ total = 汇总行
```

### 关键解读

```
★ 耗时 Top 3:
  execve   52.54%  → 加载可执行文件最慢（475μs，因为要解析 ELF）
  mmap     22.12%  → 内存映射 20 次（每个 .so 需要多次 mmap）
  mprotect  5.97%  → 设置内存保护属性

★ 调用次数 Top 3:
  mmap     20 次   → 每个共享库需要 3-5 次 mmap（代码段+数据段+BSS）
  close    10 次   → 关闭打开的 fd
  newfstatat 9 次  → 获取文件属性

★ 错误率分析:
  9 个错误 / 86 个调用 = 10.5% 错误率
  prctl 4 错误、statfs 2 错误 → 探测系统特性（不是真正的错误）
  access 1 ENOENT → ld.so.preload 不存在（预期行为）

★ 总耗时 0.904ms（904μs），平均每个 syscall 约 10μs
  这包含了 ptrace 的开销（正常 ls 执行约 0.1ms）
```

### 排序对比（`-c -S calls`）

```bash
strace -c -S calls ls /tmp
```

```
按调用次数排序时：
  mmap          20 次  → 内存映射最频繁
  close         10 次
  newfstatat     9 次
  openat         8 次
  read           6 次
  mprotect       6 次
  prctl          6 次
  ...
```

---

## 场景 3：文件操作追踪 — `strace -e trace=file cat /etc/hosts`

### 命令

```bash
strace -e trace=file cat /etc/hosts
```

### 输出（完整）

```
execve("/usr/bin/cat", ["cat", "/etc/hosts"], 0x7ffd76a7c448 /* 55 vars */) = 0
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
openat(AT_FDCWD, "/lib64/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0755, st_size=2819640, ...}, AT_EMPTY_PATH) = 0
openat(AT_FDCWD, "/usr/lib/locale/locale-archive", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=224328464, ...}, AT_EMPTY_PATH) = 0
newfstatat(1, "", {st_mode=S_IFREG|0644, st_size=0, ...}, AT_EMPTY_PATH) = 0
openat(AT_FDCWD, "/etc/hosts", O_RDONLY) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=384, ...}, AT_EMPTY_PATH) = 0
+++ exited with 0 +++
```

### 逐行注解

```
★ execve — 加载 cat 可执行文件
execve("/usr/bin/cat", ["cat", "/etc/hosts"], ...) = 0

★ access — 检查预加载库（不存在 → ENOENT）
access("/etc/ld.so.preload", R_OK) = -1 ENOENT

★ openat + newfstatat — 加载 libc.so.6（cat 只依赖 libc）
openat(AT_FDCWD, "/lib64/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_size=2819640, ...}, AT_EMPTY_PATH) = 0
                       ★ 2.8MB 的 libc

★ openat + newfstatat — 加载 locale 数据库
openat(AT_FDCWD, "/usr/lib/locale/locale-archive", ...) = 3
newfstatat(3, "", {st_size=224328464, ...}, ...) = 0
                       ★ 224MB 的 locale 文件！（但只 mmap 需要的部分）

★ newfstatat — 检查 stdout（fd=1）的属性
newfstatat(1, "", {st_mode=S_IFREG|0644, st_size=0}, AT_EMPTY_PATH) = 0
             ★ fd=1 = stdout，st_size=0 → 输出到空文件（重定向）

★★★ 目标文件 ★★★
openat(AT_FDCWD, "/etc/hosts", O_RDONLY) = 3
newfstatat(3, "", {st_size=384, ...}, AT_EMPTY_PATH) = 0
                       ★ 384 字节

★ 对比：cat 只加载了 1 个 .so（libc），而 ls 加载了 5 个
```

### 过滤效果对比

```
strace cat /etc/hosts           → 约 50 行输出（包含 mmap/read/close 等）
strace -e trace=file cat ...    → 12 行输出（★ 只有文件相关 syscall）

被过滤掉的 syscall：
  mmap, mprotect, brk, close, read, arch_prctl,
  set_tid_address, set_robust_list, rseq, exit_group, ...
```

---

## 场景 4：耗时标注 — `strace -T ls /tmp`

### 命令

```bash
strace -T ls /tmp
```

### 输出片段

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffce9a04850 /* 55 vars */) = 0 <0.000608>
brk(NULL)                               = 0x55d211abd000 <0.000075>
arch_prctl(0x3001 /* ARCH_??? */, ...)  = -1 EINVAL (Invalid argument) <0.000022>
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (...) <0.000019>
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3 <0.000080>
newfstatat(3, "", {...}, AT_EMPTY_PATH) = 0 <0.000540>
mmap(NULL, 17947, PROT_READ, MAP_PRIVATE, 3, 0) = 0x7f5e0c71c000 <0.000031>
close(3)                                = 0 <0.000023>
```

### 逐行注解

```
<T.NNNNNN>  ← ★ 该 syscall 的耗时（秒），从 enter-stop 到 exit-stop

execve(...) = 0 <0.000608>
              ★ 608μs — 最慢！execve 需要加载 ELF 并启动新进程

newfstatat(3, ...) = 0 <0.000540>
                     ★ 540μs — 首次 fstat 可能触发 dentry cache 加载

brk(NULL) = 0x... <0.000075>
              ★ 75μs — 查询堆地址

openat(...) = 3 <0.000080>
              ★ 80μs — 打开文件

mmap(...) = 0x... <0.000031>
              ★ 31μs — 内存映射

close(3) = 0 <0.000023>
           ★ 23μs — 关闭文件
```

### 耗时分布分析

```bash
# 提取耗时列并排序
strace -T ls /tmp 2>&1 | grep -oP '<\K[0-9.]+' | sort -rn | head -5
```

```
耗时分布（典型值）：
  608μs  execve        ← ★ 加载可执行文件
  540μs  newfstatat    ← 首次 stat（cache miss）
  108μs  mmap          ← 固定地址映射
   80μs  openat        ← 打开文件
   75μs  brk           ← 堆管理
   22-43μs  大部分 syscall
   <10μs  close/read    ← 快速操作

★ 规律：
  - execve 总是最慢的（需要解析 ELF、加载所有 .so）
  - 首次访问的文件操作比后续慢（cache miss）
  - mmap 普通映射很快，MAP_FIXED 略慢（需要检查地址冲突）
```

---

## 场景 5：绝对时间戳 — `strace -tt ls /tmp`

### 命令

```bash
strace -tt ls /tmp
```

### 输出片段

```
23:15:04.778611 execve("/usr/bin/ls", ["ls", "/tmp"], ...) = 0
23:15:04.779291 brk(NULL)               = 0x55fed7766000
23:15:04.779522 arch_prctl(0x3001, ...)  = -1 EINVAL (Invalid argument)
23:15:04.780037 access("/etc/ld.so.preload", R_OK) = -1 ENOENT (...)
23:15:04.780229 openat(AT_FDCWD, "/etc/ld.so.cache", ...) = 3
23:15:04.780378 newfstatat(3, "", {...}, AT_EMPTY_PATH) = 0
23:15:04.780515 mmap(NULL, 17947, PROT_READ, ...) = 0x7f08df11c000
23:15:04.780789 close(3)                = 0
```

### 注解

```
HH:MM:SS.μs  ← ★ 微秒级绝对时间戳（由 gettimeofday 获取）

23:15:04.778611  execve 开始
23:15:04.779291  brk 开始（与上一行间隔 680μs ≈ execve 耗时）
23:15:04.779522  arch_prctl 开始（间隔 231μs）
23:15:04.780037  access 开始（间隔 515μs）

★ 与 -T 的区别：
  -T 显示的是 syscall 自身耗时（exit - enter）
  -tt 显示的是 enter 时刻的墙钟时间
  两者互补：用相邻行的 -tt 差值可以算出包含 strace 处理开销的总时间
```

---

## 场景 6：追踪子进程 — `strace -f`

### 命令

```bash
strace -f -e trace=process,clone,fork,execve bash -c "echo hi; ls /tmp | head -3"
```

### 输出（完整）

```
execve("/usr/bin/bash", ["bash", "-c", "echo hi; ls /tmp | head -3"], ...) = 0
clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD
strace: Process 453463 attached
, child_tidptr=0x7f89b5ef9a10) = 453463
[pid 453462] clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD
strace: Process 453464 attached
, child_tidptr=0x7f89b5ef9a10) = 453464
[pid 453462] wait4(-1,  <unfinished ...>
[pid 453463] execve("/usr/bin/ls", ["ls", "/tmp"], ...) = 0
[pid 453464] execve("/usr/bin/head", ["head", "-3"], ...) = 0
[pid 453463] exit_group(0)              = ?
[pid 453463] +++ exited with 0 +++
[pid 453462] <... wait4 resumed>[{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL) = 453463
[pid 453462] wait4(-1,  <unfinished ...>
[pid 453464] exit_group(0)              = ?
[pid 453464] +++ exited with 0 +++
<... wait4 resumed>[{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL) = 453464
--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=453463, si_uid=0, si_status=0, si_utime=0, si_stime=0} ---
wait4(-1, 0x7ffee00fa410, WNOHANG, NULL) = -1 ECHILD (No child processes)
exit_group(0)                           = ?
+++ exited with 0 +++
```

### 逐行注解

```
★★★ 没有 PID 前缀的行属于主进程（bash, PID=453462）★★★

execve("/usr/bin/bash", ...) = 0
└── ★ bash 启动

clone(..., SIGCHLD, ...) = 453463
│                         │
│                         └─ ★ 返回子进程 PID = 453463
└── bash 用 clone 创建子进程（而非 fork，因为 clone 更灵活）

"strace: Process 453463 attached"
└── ★ strace 检测到新进程，自动开始追踪（-f 的效果）
    内核发送 PTRACE_EVENT_CLONE，strace 用 GETEVENTMSG 获取新 PID

[pid 453462] clone(...) = 453464
│             └─ bash 创建第二个子进程（head -3）
│
[pid 453462] wait4(-1, <unfinished ...>
│             │     │
│             │     └─ ★ <unfinished ...> 表示 syscall 尚未返回
│             │        （bash 在等待子进程，但其他进程的事件先被打印）
│             └─ wait4(-1) = 等待任意子进程
│
[pid 453463] execve("/usr/bin/ls", ...) = 0
└── ★ 第一个子进程执行 ls

[pid 453464] execve("/usr/bin/head", ...) = 0
└── ★ 第二个子进程执行 head

[pid 453463] exit_group(0) = ?
└── ★ ls 退出（exit_group 不返回，所以是 "?"）

[pid 453462] <... wait4 resumed>[{WIFEXITED(s) && WEXITSTATUS(s) == 0}] = 453463
│             │
│             └─ ★ wait4 恢复并返回：子进程 453463 正常退出
│                WEXITSTATUS = 0 → ls 成功

--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=453463} ---
└── ★ 子进程退出时内核发送的信号

exit_group(0) = ?
+++ exited with 0 +++
└── ★ bash 主进程退出
```

### 进程关系图

```
bash (PID 453462)
 ├── clone() → ls (PID 453463)  → execve("ls") → exit(0)
 ├── clone() → head (PID 453464) → execve("head") → exit(0)
 └── wait4() × 2 → exit(0)

★ 管道 ls /tmp | head -3 的实现：
  bash 创建两个子进程，用 pipe 连接它们的 stdout/stdin
  -f 让我们能同时看到 bash 和所有子进程的行为
```

### `-f` 与不加 `-f` 的对比

```bash
# 不加 -f：只看到 bash 的 clone 和 wait4
strace -e trace=process bash -c "echo hi"
# 输出 3 行：execve, clone, exit_group

# 加 -f：看到 bash + ls + head 的所有 process syscall
strace -f -e trace=process bash -c "echo hi; ls | head -3"
# 输出 16 行：包括子进程的 execve、exit_group、SIGCHLD 等
```

---

## 场景 7：网络追踪 — `strace -e trace=network curl`

### 命令

```bash
strace -e trace=network curl -s http://example.com
```

### 输出片段（关键部分）

```
socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 5
setsockopt(5, SOL_TCP, TCP_NODELAY, [1], 4) = 0
setsockopt(5, SOL_SOCKET, SO_KEEPALIVE, [1], 4) = 0
setsockopt(5, SOL_TCP, TCP_KEEPIDLE, [60], 4) = 0
setsockopt(5, SOL_TCP, TCP_KEEPINTVL, [60], 4) = 0
connect(5, {sa_family=AF_INET, sin_port=htons(80), sin_addr=inet_addr("172.66.147.243")}, 16) = -1 EINPROGRESS (Operation now in progress)
getsockopt(5, SOL_SOCKET, SO_ERROR, [0], [4]) = 0
getsockname(5, {sa_family=AF_INET, sin_port=htons(50832), sin_addr=inet_addr("172.27.141.144")}, [128 => 16]) = 0
getpeername(5, {sa_family=AF_INET, sin_port=htons(80), sin_addr=inet_addr("172.66.147.243")}, [128 => 16]) = 0
sendto(5, "GET / HTTP/1.1\r\nHost: example.co"..., 74, MSG_NOSIGNAL, NULL, 0) = 74
recvfrom(5, "HTTP/1.1 200 OK\r\nDate: Fri, 03 J"..., 102400, 0, NULL, NULL) = 870
```

### 逐行注解

```
socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 5
│       │         │            │            │
│       │         │            │            └─ ★ 返回 fd=5
│       │         │            └─ TCP 协议
│       │         └─ 流式套接字
│       └─ IPv4
└── 创建 TCP 套接字

setsockopt(5, SOL_TCP, TCP_NODELAY, [1], 4) = 0
│           │  │         │            │  │
│           │  │         │            │  └─ 选项长度 4 字节
│           │  │         │            └─ [1] = 启用（禁用 Nagle 算法）
│           │  │         └─ ★ TCP_NODELAY：禁用 Nagle 算法
│           │  └─ TCP 层选项
│           └─ fd=5
└── 禁用 Nagle 以减少延迟（HTTP 客户端常用）

setsockopt(5, SOL_SOCKET, SO_KEEPALIVE, [1], 4) = 0
└── ★ 启用 TCP Keep-Alive（检测断开的连接）

connect(5, {sa_family=AF_INET, sin_port=htons(80),
            sin_addr=inet_addr("172.66.147.243")}, 16) = -1 EINPROGRESS
│        │  │                          │                │
│        │  │                          │                └─ ★ EINPROGRESS = 非阻塞连接进行中
│        │  │                          └─ 目标 IP（Cloudflare CDN）
│        │  └─ 目标端口 80
│        └─ IPv4
└── 非阻塞 connect：立即返回 EINPROGRESS，后台进行三次握手

getsockopt(5, SOL_SOCKET, SO_ERROR, [0], [4]) = 0
│                                    │
│                                    └─ ★ SO_ERROR=0 → 连接成功！
└── 检查连接结果（非阻塞 connect 的标准做法）

sendto(5, "GET / HTTP/1.1\r\nHost: example.co"..., 74, MSG_NOSIGNAL, NULL, 0) = 74
│       │  │                                      │   │               │   │
│       │  │                                      │   │               │   └─ 返回发送了 74 字节
│       │  │                                      │   │               └─ MSG_NOSIGNAL：忽略 SIGPIPE
│       │  │                                      │   └─ 无目标地址（已连接）
│       │  │                                      └─ 发送 74 字节
│       │  └─ ★ HTTP GET 请求（被截断显示，strace 默认显示 32 字节字符串）
│       └─ fd=5
└── 发送 HTTP 请求

recvfrom(5, "HTTP/1.1 200 OK\r\nDate: Fri, 03 J"..., 102400, 0, NULL, NULL) = 870
│         │  │                                      │       │               │
│         │  │                                      │       │               └─ ★ 收到 870 字节
│         │  │                                      │       └─ 无特殊标志
│         │  │                                      └─ 接收缓冲区 102400 字节（100KB）
│         │  └─ ★ HTTP 响应（200 OK）
│         └─ fd=5
└── 接收 HTTP 响应
```

### IPv6 连接尝试（失败回退）

```
socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP) = 6
connect(6, {sa_family=AF_INET6, sin6_port=htons(80),
            inet_pton(AF_INET6, "2606:4700:10::ac42:93f3", &sin6_addr)}, 28)
    = -1 ENETUNREACH (Network is unreachable)
```

```
★ curl 的 Happy Eyeballs 策略：
  1. 先尝试 IPv4 连接（connect → EINPROGRESS）
  2. 同时尝试 IPv6 连接（connect → ENETUNREACH，本机无 IPv6）
  3. IPv4 连接成功（getsockopt SO_ERROR = 0）
  4. 使用 IPv4 连接发送请求
```

---

## 场景 8：附加到运行中进程 — `strace -p PID`

### 命令

```bash
# 启动后台进程
sleep 3600 &
SLEEP_PID=$!

# 附加到它
timeout 3 strace -p $SLEEP_PID -e trace=all -T

# 发送信号终止
kill -TERM $SLEEP_PID
```

### 输出（完整）

```
strace: Process 453217 attached
restart_syscall(<... resuming interrupted clock_nanosleep ...>) = ? ERESTART_RESTARTBLOCK (Interrupted by signal) <0.994028>
--- SIGTERM {si_signo=SIGTERM, si_code=SI_USER, si_pid=453194, si_uid=0} ---
+++ killed by SIGTERM +++
```

### 逐行注解

```
strace: Process 453217 attached
└── ★ strace 成功附加到进程（PTRACE_SEIZE + PTRACE_INTERRUPT）

restart_syscall(<... resuming interrupted clock_nanosleep ...>) = ? ERESTART_RESTARTBLOCK
│              │                                                │ │
│              │                                                │ └─ ★ <0.994028> = 等待了 ~1 秒
│              │                                                └─ ★ 被信号中断，需要重启
│              └─ sleep 内部用 clock_nanosleep 实现
└── ★ sleep 3600 实际是通过 clock_nanosleep 等待 3600 秒
    被 strace attach 中断后，内核返回 ERESTART_RESTARTBLOCK
    告诉进程"重新执行这个 syscall，用新的重启块"

--- SIGTERM {si_signo=SIGTERM, si_code=SI_USER, si_pid=453194, si_uid=0} ---
│             │                 │              │              │
│             │                 │              │              └─ 发送者 UID = 0 (root)
│             │                 │              └─ 发送者 PID = 453194
│             │                 └─ SI_USER = 用户空间发送的（kill 命令）
│             └─ SIGTERM 信号
└── ★ 信号到达！strace 打印信号详情

+++ killed by SIGTERM +++
└── ★ 进程被 SIGTERM 杀死（sleep 没有捕获 SIGTERM 的处理器）
```

### 信号输出格式

```
--- SIGNAME {si_signo=XXX, si_code=XXX, si_pid=XXX, si_uid=XXX, ...} ---

字段         │ 含义
───────────┼────────────────────────────────────────
si_signo    │ 信号编号（SIGTERM = 15）
si_code     │ 信号来源：SI_USER / SI_KERNEL / SI_TKILL / CLD_*
si_pid      │ 发送信号的进程 PID
si_uid      │ 发送信号的用户 UID
si_status   │ 子进程退出状态（仅 SIGCHLD）
si_utime    │ 子进程用户态 CPU 时间（仅 SIGCHLD）
si_stime    │ 子进程内核态 CPU 时间（仅 SIGCHLD）
```

---

## 场景 9：详细输出 — `strace -v`

### 命令

```bash
strace -v -e trace=file cat /etc/hosts
```

### 输出片段（对比 newfstatat）

**不加 -v（默认，缩写）：**
```
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
                                    ↑
                              省略大部分字段
```

**加 -v（完整）：**
```
newfstatat(3, "", {st_dev=makedev(0xfd, 0x3), st_ino=132387,
    st_mode=S_IFREG|0644, st_nlink=1, st_uid=0, st_gid=0,
    st_blksize=4096, st_blocks=40, st_size=17947,
    st_atime=1783039624 /* 2026-07-03T08:47:04.859336072+0800 */,
    st_atime_nsec=859336072,
    st_mtime=1783039624 /* 2026-07-03T08:47:04.840335159+0800 */,
    st_mtime_nsec=840335159,
    st_ctime=1783039624 /* 2026-07-03T08:47:04.843335303+0800 */,
    st_ctime_nsec=843335303}, AT_EMPTY_PATH) = 0
```

### 注解

```
-v 模式下 newfstatat 展示所有字段：

字段          │ 含义                              │ 示例值
────────────┼──────────────────────────────────┼─────────────────
st_dev       │ 设备号                            │ makedev(0xfd, 0x3)
st_ino       │ inode 号                          │ 132387
st_mode      │ 文件类型 + 权限                    │ S_IFREG|0644
st_nlink     │ 硬链接数                           │ 1
st_uid       │ 文件所有者 UID                     │ 0 (root)
st_gid       │ 文件所属组 GID                     │ 0 (root)
st_blksize   │ I/O 块大小                        │ 4096
st_blocks    │ 磁盘块数（512 字节单位）            │ 40
st_size      │ 文件大小（字节）                   │ 17947
st_atime     │ 最后访问时间                       │ 2026-07-03 08:47:04
st_mtime     │ 最后修改时间                       │ 2026-07-03 08:47:04
st_ctime     │ 最后状态改变时间                   │ 2026-07-03 08:47:04

★ -v 也影响 execve 的环境变量显示：
  不加 -v: execve(..., 0x7ffcb63012c8 /* 55 vars */) = 0
  加 -v:   execve(..., ["IS_SANDBOX=1", "SHELL=/bin/bash", ...全部 55 个...]) = 0
```

---

## 场景 10：排序统计 — `strace -c -S time`

### 命令

```bash
strace -c -S time find /usr -maxdepth 1 -name "*.h"
```

### 输出

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 18.46    0.000486          28        17           mmap
 16.29    0.000429          26        16         6 openat
 13.90    0.000366         366         1           execve
 10.94    0.000288          13        21           newfstatat
  9.72    0.000256          10        24           close
  7.03    0.000185          37         5           mprotect
  5.58    0.000147          10        14           fcntl
  4.14    0.000109          15         7           read
  2.35    0.000062          62         1           munmap
  2.05    0.000054          27         2           getdents64
  1.56    0.000041          13         3           brk
  1.41    0.000037          18         2         1 access
  1.37    0.000036          18         2         2 statfs
  1.03    0.000027          13         2         1 arch_prctl
  0.80    0.000021          10         2         2 ioctl
  0.72    0.000019          19         1           set_robust_list
  0.46    0.000012          12         1           prlimit64
  0.42    0.000011          11         1           fchdir
  0.38    0.000010          10         1           getrandom
  0.38    0.000010          10         1           rseq
  0.34    0.000009           9         1           uname
  0.34    0.000009           9         1           futex
  0.34    0.000009           9         1           set_tid_address
------ ----------- ----------- --------- --------- ----------------
100.00    0.002633          20       127        12 total
```

### 注解

```
★ -S time 按 seconds 列降序排列（默认行为）

★ 对比 ls 和 find 的差异：
  ls /tmp:    86 syscalls, 0.9ms  total, execve 最耗时
  find /usr: 127 syscalls, 2.6ms total, mmap 最耗时

★ find 特有的 syscall：
  fcntl (14次)    → find 用 fcntl 设置目录 fd 的 O_CLOEXEC
  fchdir (1次)    → find 用 fchdir 切换目录（比 chdir 更安全）
  getdents64 (2次) → 读取 /usr 目录条目
  uname (1次)     → 获取系统信息
  futex (1次)     → 线程同步原语（glibc 内部使用）

★ 错误分析：
  12 个错误 / 127 个调用 = 9.4% 错误率
  openat 6 错误 → 尝试打开不存在的目录/文件
  statfs 2 错误 → 检查 SELinux 文件系统不存在
  ioctl 2 错误 → 终端 ioctl 在管道/文件上失败
```

---

## 场景 11：输出到文件 — `strace -o`

### 命令

```bash
strace -o /tmp/strace_output_file.txt -e trace=file ls /tmp
```

### 效果

```
终端输出：正常的 ls 目录列表
文件内容：strace 的追踪输出（与不加 -o 时的 stderr 输出相同）
```

```bash
head -5 /tmp/strace_output_file.txt
```

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffdfee7b638 /* 55 vars */) = 0
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (...)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
openat(AT_FDCWD, "/lib64/libselinux.so.1", O_RDONLY|O_CLOEXEC) = 3
```

### `-ff` 多进程输出

```bash
strace -f -ff -o /tmp/trace ls /tmp
# 生成：
#   /tmp/trace.<PID1>  ← 主进程的输出
#   /tmp/trace.<PID2>  ← 子进程 1 的输出
#   /tmp/trace.<PID3>  ← 子进程 2 的输出
```

---

## 快速诊断流程

```
1. 基本追踪：strace <command>
   → 看所有 syscall，了解程序行为

2. 文件问题：strace -e trace=file -e status=failed <command>
   → 只看失败的文件操作，定位 ENOENT/EACCES

3. 耗时分析：strace -c -S time <command>
   → 找出最耗时的 syscall 类别

4. 逐行耗时：strace -T <command>
   → 找出单次最慢的 syscall

5. 网络问题：strace -e trace=network <command>
   → 看 connect/sendto/recvfrom 的参数和返回值

6. 子进程追踪：strace -f -e trace=process <command>
   → 看 fork/clone/exec 的完整调用树

7. 附加进程：strace -p <PID> -T -e trace=network
   → 实时观察运行中进程的网络行为

8. 详细输出：strace -v -s 256 <command>
   → 不缩写结构体，显示完整字符串
```
