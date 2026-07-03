# strace 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐行解读。
> 环境：CentOS Stream 9 / x86_64，strace 6.12，内核 5.14。

---

## 场景 1：基础追踪 — `strace ls /tmp`

### 命令

```bash
strace ls /tmp 2>&1 | head -20
```

### 实际输出（前 20 行）

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffe90ca3a58 /* 55 vars */) = 0
brk(NULL)                               = 0x55e4a800d000
arch_prctl(0x3001 /* ARCH_??? */, 0x7ffc227bb920) = -1 EINVAL (Invalid argument)
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
mmap(NULL, 17947, PROT_READ, MAP_PRIVATE, 3, 0) = 0x7f12996d0000
close(3)                                = 0
openat(AT_FDCWD, "/lib64/libselinux.so.1", O_RDONLY|O_CLOEXEC) = 3
read(3, "\177ELF\2\1\1\0\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0\0\0\0\0\0\0\0\0"..., 832) = 832
newfstatat(3, "", {st_mode=S_IFREG|0755, st_size=175744, ...}, AT_EMPTY_PATH) = 0
mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f12996ce000
mmap(NULL, 181928, PROT_READ, MAP_PRIVATE|MAP_DENYWRITE, 3, 0) = 0x7f12996a1000
mmap(0x7f12996a7000, 110592, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x6000) = 0x7f12996a7000
mmap(0x7f12996c2000, 32768, PROT_READ, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x21000) = 0x7f12996c2000
mmap(0x7f12996ca000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x28000) = 0x7f12996ca000
mmap(0x7f12996cc000, 5800, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) = 0x7f12996cc000
close(3)                                = 0
openat(AT_FDCWD, "/lib64/libcap.so.2", O_RDONLY|O_CLOEXEC) = 3
read(3, "\177ELF\2\1\1\0\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0p{\0\0\0\0\0\0"..., 832) = 832
```

### 逐行注解

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffe90ca3a58 /* 55 vars */) = 0
│       │            │                │                                │
│       │            │                │                                └─ ★ 返回值 0 = 成功
│       │            │                └─ 环境变量指针数组（55 个环境变量）
│       │            └─ argv 参数数组
│            └─ 被执行的程序路径
└─ ★ execve：进程启动的第一个系统调用，加载可执行文件
```

```
brk(NULL) = 0x55e4a800d000
│    │      │
│    │      └─ ★ 返回当前 program break 地址（堆的起始位置）
│    └─ NULL = 查询当前 break 位置（不修改）
└─ brk：管理堆内存，第一次调用通常是查询
```

```
arch_prctl(0x3001 /* ARCH_??? */, 0x7ffc227bb920) = -1 EINVAL (Invalid argument)
│           │                                     │
│           │                                     └─ ★ 返回 -1 = 失败，EINVAL = 参数无效
│           └─ 0x3001 = 未知的 arch_prctl 操作码（可能是新内核特性，旧 glibc 不认识）
└─ arch_prctl：设置 x86_64 架构相关的线程状态，这里尝试一个新操作码但失败（正常）
```

```
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
│       │                        │        │
│       │                        │        └─ ★ ENOENT = 文件不存在（正常，大多数系统没有这个文件）
│       │                        └─ R_OK = 检查读权限
│       └─ 动态链接器检查预加载库列表
└─ access：检查进程是否有权限访问文件
```

```
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
│       │          │                  │                     │
│       │          │                  │                     └─ ★ 返回 fd=3（第一个打开的文件描述符）
│       │          │                  └─ O_CLOEXEC = exec 时自动关闭此 fd
│       │          └─ 动态链接器缓存（记录共享库路径）
│       └─ AT_FDCWD = 相对于当前工作目录
└─ openat：★ 现代 Linux 统一用 openat 代替 open（避免 TOCTOU 竞态）
```

```
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
│           │  │   │                                        │
│           │  │   │                                        └─ AT_EMPTY_PATH = 对 fd 本身 stat
│           │  │   └─ st_size=17947 字节 = 17.5 KB
│           │  └─ 空字符串 + AT_EMPTY_PATH = 等价于 fstat(fd)
│           └─ fd=3（刚才打开的 ld.so.cache）
└─ newfstatat：★ 替代了 fstat/lstat/stat，获取文件元数据
```

```
mmap(NULL, 17947, PROT_READ, MAP_PRIVATE, 3, 0) = 0x7f12996d0000
│      │     │      │          │             │  │    │
│      │     │      │          │             │  │    └─ ★ 返回映射后的虚拟地址
│      │     │      │          │             │  └─ offset=0（从文件头开始映射）
│      │     │      │          │             └─ fd=3
│      │     │      │          └─ MAP_PRIVATE = 私有映射（写时复制）
│      │     │      └─ PROT_READ = 只读
│      │     └─ 17947 字节（整个 ld.so.cache）
│      └─ NULL = 让内核选择映射地址
└─ mmap：★ 将文件映射到内存，避免多次 read（共享库加载的标准方式）
```

### 后半段关键系统调用

```
# ---- 目录打开与读取 ----

statx(AT_FDCWD, "/tmp", AT_STATX_SYNC_AS_STAT|AT_NO_AUTOMOUNT, STATX_MODE,
      {stx_mode=S_IFDIR|S_ISVTX|0777, stx_size=1880, ...}) = 0
│     │           │      │                                          │
│     │           │      │                                          └─ ★ S_ISVTX = sticky bit（/tmp 的标准权限）
│     │           │      └─ STATX_MODE = 只请求 mode 字段
│     │           └─ /tmp 目录
│     └─ AT_STATX_SYNC_AS_STAT = 默认同步级别
└─ statx：★ 最新一代 stat 系统调用，替代 stat/fstat/lstat

openat(AT_FDCWD, "/tmp", O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY) = 3
│                              │
│                              └─ ★ O_DIRECTORY = 强制要求打开的是目录
└─ 打开 /tmp 目录准备读取

getdents64(3, 0x55b7383a8d20 /* 94 entries */, 32768) = 4072
│           │  │                      │              │    │
│           │  │                      │              │    └─ ★ 返回 4072 字节（实际读取量）
│           │  │                      │              └─ 32768 = 缓冲区大小（32KB）
│           │  │                      └─ ★ 94 entries = 读到 94 个目录项
│           │  └─ 用户空间缓冲区地址
│           └─ fd=3（/tmp 目录）
└─ getdents64：★ 读取目录项（现代 Linux 用 getdents64 替代 readdir）

getdents64(3, 0x55b7383a8d20 /* 0 entries */, 32768) = 0
│                                                        │
│                                                        └─ ★ 返回 0 = 目录已读完
└─ 第二次调用返回 0，表示没有更多目录项

# ---- 输出结果 ----

write(1, "{7112B6A8-865C-46A0-9DF4-624EF48"..., 1970) = 1970
│      │  │                                        │     │
│      │  │                                        │     └─ ★ 成功写入 1970 字节
│      │  │                                        └─ 请求写入 1970 字节
│      │  └─ 输出内容（ls 的格式化结果）
│      └─ fd=1（stdout）
└─ write：★ 将 ls 结果写到标准输出

close(1) = 0          ← 关闭 stdout
close(2) = 0          ← 关闭 stderr
exit_group(0) = ?     ← ★ 进程退出，exit code = 0
+++ exited with 0 +++ ← strace 提示进程已退出
```

### 一个 `ls` 的完整系统调用生命周期

```
┌──────────────────────────────────────────────────────────────────┐
│                    ls /tmp 的系统调用流程                         │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. execve()          ← 加载 /usr/bin/ls                        │
│  2. brk(NULL)         ← 查询堆起始位置                           │
│  3. access()          ← 检查 ld.so.preload（预加载库）            │
│  4. openat(ld.so.cache) → newfstatat → mmap → close             │
│     └─ 加载动态链接缓存                                          │
│  5. openat(libselinux.so) → read(ELF头) → newfstatat            │
│     → mmap(代码段/数据段/BSS) → close                            │
│     └─ ★ 加载每个 .so 都是这个模式                                │
│  6. openat(libcap.so) → ... → close                             │
│  7. openat(libc.so) → ... → close                               │
│  8. mprotect() × 6    ← 设置内存保护属性                         │
│  9. brk() × 2         ← 扩展堆空间                              │
│  10. getrandom()       ← 获取随机数（ASLR/安全相关）              │
│  11. statx("/tmp")     ← 检查 /tmp 是目录                        │
│  12. openat("/tmp", O_DIRECTORY) → getdents64 × 2 → close       │
│      └─ ★ 打开目录 → 读目录项 → 读完                             │
│  13. write(stdout)     ← 输出目录列表                            │
│  14. close(1), close(2) ← 关闭输出                               │
│  15. exit_group(0)     ← 退出                                    │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 输出格式总结

```
每行格式：syscall_name(arg1, arg2, ...) = retval

★ 特殊标记：
  -1 ENOENT (No such file or directory)  → 失败：errno 名 + 描述
  0x7f12996d0000                         → 返回值（指针以十六进制显示）
  "\177ELF\2\1\1\0..."                   → 字符串参数（转义显示）
  /* 55 vars */                          → strace 添加的注释
  AT_FDCWD                               → 常量名翻译（-100 → AT_FDCWD）
  O_RDONLY|O_CLOEXEC                     → 位标志展开（多个标志用 | 分隔）
```

---

## 场景 2：统计报告 — `strace -c ls /tmp`

### 命令

```bash
strace -c -o /tmp/strace_c_demo.txt ls /tmp
cat /tmp/strace_c_demo.txt
```

### 实际输出

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 39.53    0.000017           1        10           close
 18.60    0.000008           8         1           write
 16.28    0.000007           0         9           newfstatat
 13.95    0.000006           3         2           getdents64
 11.63    0.000005           2         2         2 ioctl
  0.00    0.000000           0         6           read
  0.00    0.000000           0        20           mmap
  0.00    0.000000           0         6           mprotect
  0.00    0.000000           0         1           munmap
  0.00    0.000000           0         3           brk
  0.00    0.000000           0         2         1 access
  0.00    0.000000           0         1           execve
  0.00    0.000000           0         2         2 statfs
  0.00    0.000000           0         6         4 prctl
  0.00    0.000000           0         2         1 arch_prctl
  0.00    0.000000           0         1           set_tid_address
  0.00    0.000000           0         8           openat
  0.00    0.000000           0         1           set_robust_list
  0.00    0.000000           0         1           prlimit64
  0.00    0.000000           0         1           getrandom
  0.00    0.000000           0         1           statx
  0.00    0.000000           0         1           rseq
------ ----------- ----------- --------- --------- ----------------
100.00    0.000043           0        87        10 total
```

### 逐列注解

```
列名         含义                                          示例解读
─────────   ──────────────────────────────────────────   ──────────────────────────
% time      ★ 该 syscall 占总时间的百分比                  close 占 39.53%
seconds     该 syscall 累计耗时（秒）                      close 共 17μs
usecs/call  每次调用平均耗时（微秒）                        close 每次 ~1μs
calls       ★ 调用次数                                    close 被调用了 10 次
errors      返回错误的次数                                 ioctl 2 次调用都失败了
syscall     系统调用名称                                   —
```

### 关键解读

```
★ 调用次数 TOP 5：
  mmap × 20       ← 加载共享库的代码段/数据段/BSS
  close × 10      ← 关闭各种 fd
  newfstatat × 9  ← 查询文件属性（每次 openat 后都 fstat）
  openat × 8      ← 打开共享库 + /tmp 目录
  read × 6        ← 读取 ELF 头信息

★ 错误次数分析：
  prctl × 4 errors  ← 检查 capability 边界，EINVAL = 不支持的能力号（正常）
  ioctl × 2 errors  ← TCGETS 失败，因为 stdout 不是终端（管道/重定向）
  statfs × 2 errors ← 检查 SELinux 挂载点，不存在（正常）
  arch_prctl × 1    ← 新操作码不被支持
  access × 1        ← ld.so.preload 不存在

★ 耗时分析：
  总耗时 43μs → ls 是极快的命令
  close 占 39.53%  → 主要是 mmap 映射后的 close，耗时很少
  write 占 18.60%  → 单次 write 8μs，写 1970 字节到 stdout

★ 性能优化启示：
  如果一个命令调用了 87 次 syscall 但总耗时 43μs，
  说明系统调用本身开销极小（每次约 0.5μs）。
  真正慢的命令瓶颈在 I/O 或计算，不在 syscall 切换。

★ 注意 % time 列的误导：
  大部分 syscall 耗时太短被测量精度淹没（显示 0.00%）。
  只有 close/write/newfstatat/getdents64/ioctl 有可测量的时间。
  这不代表 mmap 不耗时——而是 ptrace 的时间分辨率有限。
```

---

## 场景 3：文件类系统调用 — `strace -e trace=file cat /etc/hostname`

### 命令

```bash
strace -e trace=file cat /etc/hostname
```

### 实际输出

```
execve("/usr/bin/cat", ["cat", "/etc/hostname"], 0x7fffaed8f828 /* 55 vars */) = 0
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
openat(AT_FDCWD, "/lib64/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0755, st_size=2819640, ...}, AT_EMPTY_PATH) = 0
openat(AT_FDCWD, "/usr/lib/locale/locale-archive", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=224328464, ...}, AT_EMPTY_PATH) = 0
newfstatat(1, "", {st_mode=S_IFREG|0644, st_size=627, ...}, AT_EMPTY_PATH) = 0
openat(AT_FDCWD, "/etc/hostname", O_RDONLY) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=24, ...}, AT_EMPTY_PATH) = 0
iZbp11r90r4g931c0wfjt5Z
+++ exited with 0 +++
```

### 逐行注解

```
execve("/usr/bin/cat", ...) = 0
└─ ★ cat 程序启动

access("/etc/ld.so.preload", R_OK) = -1 ENOENT
└─ 检查预加载库（不存在，正常）

openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
└─ ★ 打开动态链接缓存（查找 libc.so.6 的路径）

newfstatat(3, "", ..., AT_EMPTY_PATH) = 0
└─ 获取 ld.so.cache 文件大小（17947 字节）

openat(AT_FDCWD, "/lib64/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
└─ ★ cat 只依赖 libc（不像 ls 还依赖 libselinux/libcap）

newfstatat(3, "", ..., AT_EMPTY_PATH) = 0
└─ libc.so.6 = 2819640 字节 ≈ 2.7 MB

openat(AT_FDCWD, "/usr/lib/locale/locale-archive", ...) = 3
└─ ★ 打开 locale 归档（cat 需要处理字符编码/locale）

newfstatat(3, "", ..., AT_EMPTY_PATH) = 0
└─ locale-archive = 224328464 字节 ≈ 214 MB（★ 巨大的文件！但只 mmap 需要的部分）

newfstatat(1, "", ..., AT_EMPTY_PATH) = 0
└─ ★ 检查 stdout（fd=1）的属性

openat(AT_FDCWD, "/etc/hostname", O_RDONLY) = 3
└─ ★★★ 终于打开目标文件！这是 cat 真正要读的文件

newfstatat(3, "", ..., AT_EMPTY_PATH) = 0
└─ hostname 文件 = 24 字节

iZbp11r90r4g931c0wfjt5Z
└─ 文件内容被写到 stdout（这就是本机的 hostname）
```

### `-e trace=file` 过滤器说明

```
★ trace=file 只追踪"文件类"系统调用，包括：
  - execve              进程执行
  - open/openat         打开文件
  - access              权限检查
  - stat/fstat/newfstatat/statx  文件属性查询
  - chmod/chown         权限修改
  - link/rename         文件操作
  - mkdir/rmdir         目录操作

★ 被过滤掉的调用（对比完整追踪）：
  完整追踪 cat /etc/hostname: ~40 行（包含 mmap/mprotect/brk/close/read 等）
  -e trace=file 过滤后:       11 行（★ 只有文件相关 syscall）

  被过滤掉的具体调用：
  - mmap/mprotect/brk    （内存类）
  - close                （fd 管理类）
  - read/write           （I/O 类）
  - ioctl                （设备控制类）
  - getrandom            （随机数）
  - set_tid_address      （线程类）

→ 用 -e trace=file 可以聚焦"程序访问了哪些文件"，
  排查"找不到配置文件"等问题时非常有用。
```

### 对比：ls 和 cat 的依赖差异

```
ls /tmp 加载的共享库（5 个）：
  libselinux.so.1   ← SELinux 标签显示
  libcap.so.2       ← capability 检查
  libpcre2-8.so.0   ← 正则表达式（--color 匹配用）
  libc.so.6         ← C 标准库
  ld.so.cache       ← 动态链接缓存

cat /etc/hostname 加载的共享库（1 个）：
  libc.so.6         ← C 标准库
  ld.so.cache       ← 动态链接缓存

→ ★ ls 功能复杂（颜色、SELinux、正则），所以依赖更多 .so
  cat 功能简单，只依赖 libc
```

---

## 场景 4：系统调用耗时 — `strace -T ls /tmp`

### 命令

```bash
strace -T ls /tmp 2>&1 | head -10
```

### 实际输出（前 10 行）

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffcc4aa0220 /* 55 vars */) = 0 <0.000349>
brk(NULL)                               = 0x55a091a03000 <0.000026>
arch_prctl(0x3001 /* ARCH_??? */, 0x7fffbd040d20) = -1 EINVAL (Invalid argument) <0.000037>
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory) <0.000030>
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3 <0.000033>
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0 <0.000035>
mmap(NULL, 17947, PROT_READ, MAP_PRIVATE, 3, 0) = 0x7f4017cef000 <0.000029>
close(3)                                = 0 <0.000022>
openat(AT_FDCWD, "/lib64/libselinux.so.1", O_RDONLY|O_CLOEXEC) = 3 <0.000028>
read(3, "\177ELF\2\1\1\0\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0\0\0\0\0\0\0\0\0"..., 832) = 832 <0.000023>
```

### 耗时注解

```
系统调用                                        耗时(秒)     耗时(μs)    解读
─────────────────────────────────────────────   ──────────  ──────────  ──────────────────
execve("/usr/bin/ls", ...)                      <0.000349>  ★ 349μs    最慢！加载 ELF + 解释器
brk(NULL)                                       <0.000026>    26μs     查询堆地址，很快
arch_prctl(0x3001, ...)                         <0.000037>    37μs     失败也花了 37μs
access("/etc/ld.so.preload", ...)               <0.000030>    30μs     文件不存在的查询
openat("/etc/ld.so.cache", ...)                 <0.000033>    33μs     打开缓存文件
newfstatat(3, "", ...)                          <0.000035>    35μs     获取文件属性
mmap(NULL, 17947, ...)                          <0.000029>    29μs     映射 ld.so.cache
close(3)                                        <0.000022>    22μs     关闭 fd（最快之一）
openat("libselinux.so.1", ...)                  <0.000028>    28μs     打开共享库
read(3, "\177ELF...", 832)                      <0.000023>    23μs     读 ELF 头 832 字节
```

### `-T` 参数说明

```
★ -T 在每个系统调用后追加 <秒数>，格式为 <0.xxxxxx>

耗时分布规律：
  execve:  349μs  ← ★★ 远高于其他调用（需要加载 ELF、映射解释器 ld-linux.so）
  普通调用: 22~37μs ← 大部分系统调用在 20~40μs 之间
  close:   22μs   ← ★ 通常是最快的系统调用

→ 如果看到某个调用耗时 > 1ms，说明该调用涉及了阻塞操作
  （如磁盘 I/O、网络等待、锁竞争等），是性能排查的关键线索。

对比示例：
  正常 openat:  <0.000028> = 28μs    ← 打开缓存文件
  网络 open:    <0.152300> = 152ms   ← ★ 如果访问 NFS 挂载点，可能很慢
  磁盘 read:    <2.341000> = 2341ms  ← ★ 磁盘故障时的 read 延迟
```

### 耗时提取单行命令

```bash
# 提取耗时列并排序，找最慢的 5 个调用
strace -T ls /tmp 2>&1 | grep -oP '<\K[0-9.]+' | sort -rn | head -5
```

```
典型输出：
  0.000349  ← execve（最慢）
  0.000037  ← arch_prctl
  0.000035  ← newfstatat
  0.000033  ← openat
  0.000030  ← access
```

---

## 场景 5：子进程追踪 — `strace -f`

### 命令

```bash
strace -f -e trace=%process bash -c "ls /dev/null; echo done"
```

> 使用 `ls /dev/null` 而非 `echo hello`，因为 `echo` 是 bash 内建命令不会 fork，
> 而 `ls` 是外部命令，bash 必须 fork 子进程来执行它。

### 实际输出

```
execve("/usr/bin/bash", ["bash", "-c", "ls /dev/null; echo done"], 0x7fff8ee17a88 /* 55 vars */) = 0
clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, child_tidptr=0x7f2926904a10) = 456770
strace: Process 456770 attached
[pid 456770] execve("/usr/bin/ls", ["ls", "/dev/null"], 0x55f1f8e05b50 /* 55 vars */) = 0
[pid 456769] wait4(-1, /dev/null
 <unfinished ...>
[pid 456770] exit_group(0)              = ?
[pid 456770] +++ exited with 0 +++
<... wait4 resumed>[{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL) = 456770
--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=456770, si_uid=0, si_status=0, si_utime=0, si_stime=0} ---
wait4(-1, 0x7ffcc33f8a10, WNOHANG, NULL) = -1 ECHILD (No child processes)
done
exit_group(0)                           = ?
+++ exited with 0 +++
```

### 逐行注解

```
execve("/usr/bin/bash", ...) = 0
└─ bash 进程启动（pid=456769）

clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD,
      child_tidptr=...) = 456770
│     │                │                                              │
│     │                │                                              └─ ★ 返回子进程 PID = 456770
│     │                └─ CLONE_CHILD_CLEARTID = 子进程退出时清除 thread ID
│     │                   CLONE_CHILD_SETTID  = 在子进程中设置 thread ID
│     │                   SIGCHLD = 子进程退出时发送 SIGCHLD 信号
│     └─ child_stack=NULL = 子进程共享父进程的栈空间（fork 语义）
└─ ★★★ clone：Linux 用 clone 统一实现 fork/vfork/clone
   bash 执行外部命令 "ls" 时需要 fork 一个子进程

strace: Process 456770 attached
└─ ★ strace 自动附加到子进程（-f 的效果）

[pid 456770] execve("/usr/bin/ls", ...) = 0
└─ ★ 子进程（456770）执行 ls 命令

[pid 456769] wait4(-1, ... <unfinished ...>
│             │
│             └─ -1 = 等待任意子进程
└─ 父进程（456769 = bash）调用 wait4 等待子进程完成
   <unfinished ...> = 该调用尚未返回（子进程还在运行）

[pid 456770] exit_group(0) = ?
└─ ★ 子进程（ls）退出，exit code = 0

[pid 456770] +++ exited with 0 +++
└─ strace 确认子进程已退出

<... wait4 resumed>[{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL) = 456770
│                  │                                                    │
│                  │                                                    └─ ★ 返回退出的子进程 PID
│                  └─ WIFEXITED = 正常退出，WEXITSTATUS = 退出码 0
└─ 父进程的 wait4 调用恢复并返回，获知子进程 456770 已正常退出

--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=456770, si_uid=0,
             si_status=0, si_utime=0, si_stime=0} ---
└─ ★★ 内核向父进程发送 SIGCHLD 信号
   si_code=CLD_EXITED  = 子进程正常退出（非被信号杀死）
   si_pid=456770       = 退出的子进程 PID
   si_uid=0            = 子进程的 UID（root）
   si_status=0         = 退出码

wait4(-1, ..., WNOHANG, NULL) = -1 ECHILD (No child processes)
└─ bash 再次 wait4（WNOHANG = 非阻塞），发现没有更多子进程

exit_group(0) = ?
+++ exited with 0 +++
└─ bash 主进程退出
```

### 父子进程交互时序图

```
  bash (456769)                          ls (456770)
      │                                      │
      │──── clone() ────────────────────────>│ 创建子进程
      │                                      │
      │──── wait4(-1) ──> 阻塞等待           │──── execve("ls")
      │     <unfinished>                     │     执行 ls /dev/null
      │                                      │
      │                                      │──── exit_group(0)
      │                                      │     退出
      │                                      │
      │<── wait4 返回 ──────────────────────  │  ← SIGCHLD
      │    子进程 456770 退出                 │
      │                                      │
      │──── exit_group(0)                    │
      │     bash 退出                        │
```

### `-f` vs 不加 `-f` 的区别

```
不加 -f：
  只追踪 bash 自身 → 看到 clone() 和 wait4()
  看不到子进程 ls 内部的系统调用

加 -f：
  追踪 bash + 所有子进程 → ★ 看到 ls 的 execve、exit_group 等
  strace 为每个 PID 加上 [pid XXXXX] 前缀
  <unfinished ...> 和 <... resumed> 标记被中断/恢复的调用

★ strace -ff -o /tmp/trace 更进一步：
  为每个进程生成独立的输出文件（/tmp/trace.456769, /tmp/trace.456770）
  方便分别分析各进程的行为
```

---

## 场景 6：网络追踪 — `strace -e trace=network curl`

### 命令

```bash
strace -e trace=network curl -s http://example.com > /dev/null
```

### 实际输出

```
socketpair(AF_UNIX, SOCK_STREAM, 0, [3, 4]) = 0
socket(AF_INET6, SOCK_DGRAM, IPPROTO_IP) = 5
socketpair(AF_UNIX, SOCK_STREAM, 0, [5, 6]) = 0
socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 5
setsockopt(5, SOL_TCP, TCP_NODELAY, [1], 4) = 0
setsockopt(5, SOL_SOCKET, SO_KEEPALIVE, [1], 4) = 0
setsockopt(5, SOL_TCP, TCP_KEEPIDLE, [60], 4) = 0
setsockopt(5, SOL_TCP, TCP_KEEPINTVL, [60], 4) = 0
connect(5, {sa_family=AF_INET, sin_port=htons(80), sin_addr=inet_addr("104.20.23.154")}, 16) = -1 EINPROGRESS (Operation now in progress)
socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP) = 6
setsockopt(6, SOL_TCP, TCP_NODELAY, [1], 4) = 0
setsockopt(6, SOL_SOCKET, SO_KEEPALIVE, [1], 4) = 0
setsockopt(6, SOL_TCP, TCP_KEEPIDLE, [60], 4) = 0
setsockopt(6, SOL_TCP, TCP_KEEPINTVL, [60], 4) = 0
connect(6, {sa_family=AF_INET6, sin6_port=htons(80), ...}, 28) = -1 ENETUNREACH (Network is unreachable)
getsockopt(5, SOL_SOCKET, SO_ERROR, [0], [4]) = 0
getsockname(5, {sa_family=AF_INET, sin_port=htons(59248), sin_addr=inet_addr("172.27.141.144")}, [128 => 16]) = 0
getpeername(5, {sa_family=AF_INET, sin_port=htons(80), sin_addr=inet_addr("104.20.23.154")}, [128 => 16]) = 0
sendto(5, "GET / HTTP/1.1\r\nHost: example.co"..., 74, MSG_NOSIGNAL, NULL, 0) = 74
recvfrom(5, "HTTP/1.1 200 OK\r\nDate: Fri, 03 J"..., 102400, 0, NULL, NULL) = 870
+++ exited with 0 +++
```

### 逐行注解

```
socketpair(AF_UNIX, SOCK_STREAM, 0, [3, 4]) = 0
│           │         │              │        │
│           │         │              │        └─ 返回两个 fd: 3 和 4（一对连接）
│           │         │              └─ [3, 4] = 输出的 fd 对
│           │         └─ SOCK_STREAM = 流式套接字
│           └─ AF_UNIX = Unix 域套接字（进程间通信）
└─ ★ socketpair：创建一对连接的 Unix 域套接字（curl 内部 IPC 用）

socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 5
│       │          │             │            │
│       │          │             │            └─ ★ 返回 fd=5
│       │          │             └─ IPPROTO_TCP = TCP 协议
│       │          └─ SOCK_STREAM = 流式（TCP）
│       └─ AF_INET = IPv4
└─ ★★ 创建 TCP socket — 这是真正的网络连接

setsockopt(5, SOL_TCP, TCP_NODELAY, [1], 4) = 0
└─ ★ TCP_NODELAY = 禁用 Nagle 算法（减少小包延迟，HTTP 常用）

setsockopt(5, SOL_SOCKET, SO_KEEPALIVE, [1], 4) = 0
└─ SO_KEEPALIVE = 启用 TCP keepalive（检测断连）

setsockopt(5, SOL_TCP, TCP_KEEPIDLE, [60], 4) = 0
└─ TCP_KEEPIDLE = 60 秒无数据后开始发 keepalive 探测

setsockopt(5, SOL_TCP, TCP_KEEPINTVL, [60], 4) = 0
└─ TCP_KEEPINTVL = 探测间隔 60 秒

connect(5, {sa_family=AF_INET, sin_port=htons(80),
            sin_addr=inet_addr("104.20.23.154")}, 16) = -1 EINPROGRESS
│       │    │                                      │       │
│       │    │                                      │       └─ ★★ EINPROGRESS = 连接正在进行中
│       │    │                                      │          （非阻塞 connect 的正常返回值）
│       │    │                                      └─ addrlen=16（IPv4 地址长度）
│       │    └─ 目标: 104.20.23.154:80（example.com 的 IP）
│       └─ fd=5（TCP socket）
└─ ★★★ connect：发起 TCP 三次握手
   EINPROGRESS 不是错误！非阻塞 socket 的 connect 总是先返回 EINPROGRESS，
   之后通过 getsockopt(SO_ERROR) 检查连接是否成功。

connect(6, {sa_family=AF_INET6, sin6_port=htons(80), ...}, 28) = -1 ENETUNREACH
└─ ★ IPv6 连接失败（Network is unreachable）
   curl 同时尝试 IPv4 和 IPv6（Happy Eyeballs 算法），IPv6 不可用则回退到 IPv4

getsockopt(5, SOL_SOCKET, SO_ERROR, [0], [4]) = 0
│                                    │
│                                    └─ ★★ SO_ERROR=0 表示连接成功！
└─ 检查 connect 结果（非阻塞 connect 的标准做法）

getsockname(5, ..., sin_port=htons(59248), sin_addr=inet_addr("172.27.141.144"))
└─ 获取本地地址：172.27.141.144:59248（本机的 IP 和临时端口）

getpeername(5, ..., sin_port=htons(80), sin_addr=inet_addr("104.20.23.154"))
└─ 获取远端地址：104.20.23.154:80（确认连接对端）

sendto(5, "GET / HTTP/1.1\r\nHost: example.co"..., 74, MSG_NOSIGNAL, NULL, 0) = 74
│       │  │                                     │    │                    │
│       │  │                                     │    │                    └─ ★ 成功发送 74 字节
│       │  │                                     │    └─ MSG_NOSIGNAL = 不产生 SIGPIPE
│       │  │                                     └─ 74 字节
│       │  └─ HTTP GET 请求（可以看到请求内容！）
│       └─ fd=5
└─ ★★★ sendto：发送 HTTP 请求

recvfrom(5, "HTTP/1.1 200 OK\r\nDate: Fri, 03 J"..., 102400, 0, NULL, NULL) = 870
│         │  │                                     │                    │
│         │  │                                     │                    └─ ★ 收到 870 字节
│         │  │                                     └─ 102400 = 缓冲区大小（100KB）
│         │  └- HTTP 响应（200 OK）
│         └─ fd=5
└─ ★★★ recvfrom：接收 HTTP 响应
```

### curl 网络交互全流程

```
┌──────────────────────────────────────────────────────────────────┐
│                  curl → example.com 的网络流程                    │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. socketpair() × 2    ← 创建内部 IPC 通道                      │
│  2. socket(AF_INET6, DGRAM) ← DNS 查询（UDP）                    │
│  3. socket(AF_INET, STREAM) ← ★ 创建 TCP socket (fd=5)          │
│  4. setsockopt × 4       ← 配置 TCP 参数                         │
│  5. connect(5, IPv4:80)  ← ★ TCP 三次握手（返回 EINPROGRESS）     │
│  6. socket(AF_INET6, STREAM) ← ★ 同时尝试 IPv6                  │
│  7. connect(6, IPv6:80)  ← IPv6 失败（ENETUNREACH）             │
│  8. getsockopt(SO_ERROR) ← ★ 确认 IPv4 连接成功                  │
│  9. getsockname/getpeername ← 确认连接信息                       │
│  10. sendto("GET /...")  ← ★★ 发送 HTTP 请求                    │
│  11. recvfrom("HTTP/1.1 200 OK...") ← ★★ 接收 HTTP 响应         │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 场景 7：信号追踪 — `strace -p $$ -e trace=signal -c`

### 命令

```bash
# 追踪当前 shell 的信号，3 秒后发送 SIGINT 终止 strace
strace -p $$ -e trace=signal -c &
STRACE_PID=$!
sleep 3
kill -INT $STRACE_PID
wait $STRACE_PID 2>/dev/null
```

### 实际输出

```
strace: Process 456454 attached
strace: Process 456454 detached
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 45.45    0.000005           5         1           kill
 27.27    0.000003           3         1           rt_sigprocmask
 27.27    0.000003           3         1           rt_sigreturn
------ ----------- ----------- --------- --------- ----------------
100.00    0.000011           3         3           total
```

### 逐行注解

```
strace: Process 456454 attached
└─ ★ strace 附加到当前 shell 进程（$$ = 当前 PID）

strace: Process 456454 detached
└─ strace 收到 SIGINT 后从目标进程分离

% time  seconds  usecs/call  calls  errors  syscall
 45.45   0.000005    5          1            kill
│       │          │      │                 │
│       │          │      │                 └─ ★ kill：发送信号的调用
│       │          │      └─ 1 次调用
│       │          └─ 每次 5μs
│       └─ 共 5μs
└─ kill 占 45.45%：shell 在等待期间调用了 1 次 kill（可能是内部信号管理）

 27.27   0.000003    3          1            rt_sigprocmask
└─ ★ rt_sigprocmask：修改/查询信号掩码（1 次，3μs）
   shell 在处理信号前通常会修改信号掩码

 27.27   0.000003    3          1            rt_sigreturn
└─ ★★ rt_sigreturn：从信号处理函数返回
   当 strace 发送 SIGINT 给自己时，信号处理函数执行完毕后
   通过 rt_sigreturn 返回用户态

总计：3 次信号相关调用，共 11μs → shell 在空闲期间几乎不涉及信号操作
```

### 信号类系统调用说明

```
系统调用              用途                                    常见场景
──────────────      ────────────────────────────────      ──────────────────
kill                向进程/进程组发送信号                      kill -9 PID
rt_sigprocmask      修改/查询当前线程的信号掩码               阻塞/解除阻塞信号
rt_sigreturn        从信号处理函数返回到被中断的代码           ★ 内核自动调用
rt_sigaction        注册/查询信号处理函数                     signal() / sigaction()
rt_sigtimedwait     同步等待信号（带超时）                    信号等待
rt_sigqueueinfo     向指定线程发送信号（附带数据）             sigqueue()
signalfd4           创建信号文件描述符（用 poll/epoll 等）     异步信号处理
```

---

## 场景 8：输出到文件 — `strace -o output.txt ls`

### 命令

```bash
strace -o /tmp/strace_output_demo.txt ls /tmp > /dev/null
head -15 /tmp/strace_output_demo.txt
```

### 实际输出（文件内容）

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffe5b131d58 /* 55 vars */) = 0
brk(NULL)                               = 0x55fa7ea20000
arch_prctl(0x3001 /* ARCH_??? */, 0x7fffb6f5d650) = -1 EINVAL (Invalid argument)
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
mmap(NULL, 17947, PROT_READ, MAP_PRIVATE, 3, 0) = 0x7f74787d4000
close(3)                                = 0
openat(AT_FDCWD, "/lib64/libselinux.so.1", O_RDONLY|O_CLOEXEC) = 3
read(3, "\177ELF\2\1\1\0\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0\0\0\0\0\0\0\0\0"..., 832) = 832
newfstatat(3, "", {st_mode=S_IFREG|0755, st_size=175744, ...}, AT_EMPTY_PATH) = 0
mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f74787d2000
mmap(NULL, 181928, PROT_READ, MAP_PRIVATE|MAP_DENYWRITE, 3, 0) = 0x7f74787a5000
mmap(0x7f74787ab000, 110592, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x6000) = 0x7f74787ab000
mmap(0x7f74787c6000, 32768, PROT_READ, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x21000) = 0x7f74787c6000
```

### `-o` 参数说明

```
★ -o filename：将 strace 输出写入文件而非 stderr

使用场景：
  1. 输出量很大时，保存到文件方便后续分析
  2. 不想让 strace 输出和程序本身的输出混在一起
  3. 配合 grep/awk 做自动化分析

常见组合：
  strace -o out.txt -c ls          ← 统计报告写入 out.txt
  strace -o out.txt -T ls          ← 每行带耗时
  strace -o out.txt -f bash -c "x" ← 子进程追踪写入文件
  strace -o out.txt -s 200 cmd     ← 字符串截断长度 200（默认 32）

★ 注意：-o 写入的是 strace 追踪信息，程序自身的 stdout/stderr 不受影响
  所以 > /dev/null 只抑制了 ls 的输出，strace 输出仍写入文件
```

### `-ff` 多进程输出

```bash
strace -f -ff -o /tmp/trace bash -c "ls /dev/null"
# 生成：
#   /tmp/trace.456769  ← bash 主进程的输出
#   /tmp/trace.456770  ← ls 子进程的输出
```

```
★ -ff 与 -f -o 的区别：
  -f -o file.txt    → 所有进程的追踪信息混合写入一个文件
  -ff -o /tmp/trace → 每个进程独立文件（/tmp/trace.PID）

→ 分析多进程程序时，-ff 更方便（可以分别查看各进程的行为）
```

---

## 场景 9：与 perf trace 的对比

### 命令

```bash
# strace
strace ls /tmp 2>&1 | head -8

# perf trace
perf trace ls /tmp 2>&1 | head -8
```

### strace 输出

```
execve("/usr/bin/ls", ["ls", "/tmp"], 0x7ffe90ca3a58 /* 55 vars */) = 0
brk(NULL)                               = 0x55e4a800d000
arch_prctl(0x3001 /* ARCH_??? */, 0x7ffc227bb920) = -1 EINVAL (Invalid argument)
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
newfstatat(3, "", {st_mode=S_IFREG|0644, st_size=17947, ...}, AT_EMPTY_PATH) = 0
mmap(NULL, 17947, PROT_READ, MAP_PRIVATE, 3, 0) = 0x7f12996d0000
close(3)                                = 0
```

### perf trace 输出

```
         ? (         ): ls/456871  ... [continued]: execve())                   = 0
     0.091 ( 0.003 ms): ls/456871 brk()                                         = 0x56150d127000
     0.164 ( 0.002 ms): ls/456871 arch_prctl(option: 0x3001, arg2: 0x7ffdf4057370) = -1 EINVAL (Invalid argument)
     0.212 ( 0.004 ms): ls/456871 access(filename: 0xcc6e9410, mode: R)         = -1 ENOENT (No such file or directory)
     0.226 ( 0.021 ms): ls/456871 openat(dfd: CWD, filename: 0xcc6e8264, flags: RDONLY|CLOEXEC) = 4
     0.249 ( 0.005 ms): ls/456871 newfstatat(dfd: 4, filename: 0xcc6e8e35, statbuf: 0x7ffdf40565b0, flag: 4096) = 0
     0.256 ( 0.009 ms): ls/456871 mmap(len: 17947, prot: READ, flags: PRIVATE, fd: 4) = 0x7fcdcc6bc000
     0.266 ( 0.004 ms): ls/456871 close(fd: 4)                                  = 0
```

### 对比表

```
特性              strace                              perf trace
─────────────    ────────────────────────────────    ────────────────────────────────
底层机制         ★ ptrace（每 syscall 停两次）        ★ perf_event + tracepoint
                                                     （内核侧采集，几乎零停顿）
性能开销         ★★ 高（10x~100x 减速）              ★ 低（< 5% 减速）
时间信息         -T 选项显示耗时                      ★ 内置两列时间：
                                                     绝对时间 (ms) + 调用耗时 (ms)
参数展示         ★★ 完整字符串                        部分参数显示为地址
                 "/etc/ld.so.cache"                  0xcc6e8264（字符串指针）
返回值           完整                                  完整
PID 显示         不加 -f 时不显示                     ★ 始终显示 进程名/PID
过滤             ★★ -e trace=file/network/...        --filter 更底层
子进程追踪       -f                                   --follow-forks
统计报告         ★★ -c 直接输出                       perf trace -s（summary）
attach 进程      ★★ -p PID                           -p PID
输出格式         单列文本                              带时间轴的双列表格
适用场景         调试 / 安全审计 / 详细参数            性能分析 / 生产环境追踪
```

### 时间格式差异

```
strace -T:
  openat(...) = 3 <0.000033>
                      └─ ★ 只有耗时（秒），无绝对时间

perf trace:
  0.226 ( 0.021 ms): ls/456871 openat(...)
  │       │                    │
  │       │                    └─ 进程名/PID
  │       └─ ★ 本次调用耗时 0.021 ms = 21 μs
  └─ ★ 绝对时间 0.226 ms（从程序启动算起）

→ perf trace 同时提供绝对时间和调用耗时，更适合分析延迟分布。
  strace -T 只提供调用耗时，但参数展示更完整。
```

### 参数展示差异

```
同一个 openat 调用：

strace:
  openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
                    │                    │
                    │                    └─ ★ 标志位解析为可读名称
                    └─ ★ 完整文件路径字符串

perf trace:
  openat(dfd: CWD, filename: 0xcc6e8264, flags: RDONLY|CLOEXEC) = 4
                │       │                         │
                │       │                         └─ 标志位也解析了
                │       └─ ★ 只显示指针地址（不解析字符串）
                └─ AT_FDCWD 解析为 CWD

→ strace 在参数可读性上胜出（显示字符串内容），
  perf trace 对字符串参数有时只显示地址。
```

### 何时用哪个？

```
用 strace：
  ★ 需要看完整参数（文件路径、字符串内容）
  ★ 安全审计（记录每个 syscall 的详细参数）
  ★ 调试"程序为什么失败"（看 ENOENT/EACCES 对应的路径）
  ★ 快速过滤（-e trace=file/network/process）

用 perf trace：
  ★ 生产环境性能分析（开销低）
  ★ 需要时间轴（绝对时间 + 调用耗时）
  ★ 与 perf record/report 配合使用
  ★ 需要内核 tracepoint 级别的细节
```

---

## 快速诊断流程

```
1. 基础追踪：看程序做了什么
   strace ls /tmp
   → 了解系统调用全貌（execve → 加载库 → 业务逻辑 → 退出）

2. 统计模式：找调用热点
   strace -c ls /tmp
   → 哪个 syscall 调用次数最多？哪个最耗时？

3. 耗时分析：找慢调用
   strace -T cmd
   → 哪一行 <0.xxxxx> 数值最大？就是性能瓶颈

4. 文件追踪：看程序访问了哪些文件
   strace -e trace=file cmd
   → 排查"找不到配置文件"、"读取了错误的路径"

5. 网络追踪：看程序的网络行为
   strace -e trace=network curl ...
   → 连接了哪个 IP？发了什么数据？连接是否成功？

6. 子进程追踪：看 fork 行为
   strace -f -e trace=%process bash -c "cmd"
   → 确认是否 fork、fork 了几个子进程、各自做了什么

7. 信号追踪：看信号流转
   strace -p PID -e trace=signal -c
   → 进程收到了哪些信号？频率如何？

8. 输出保存：方便后续分析
   strace -o out.txt cmd
   → grep "ENOENT" out.txt  ← 找所有"文件不存在"的错误
   → grep "openat" out.txt  ← 只看打开文件的调用
```

---

## 常用单行诊断命令

```bash
# 找程序尝试打开但不存在的文件
strace -e trace=openat cmd 2>&1 | grep ENOENT

# 统计程序打开了哪些文件（去重）
strace -e trace=openat cmd 2>&1 | grep -oP '"[^"]*"' | sort -u

# 找最慢的 10 个系统调用
strace -T cmd 2>&1 | grep -oP '<\K[0-9.]+' | sort -rn | head -10

# 追踪 TCP 连接
strace -e trace=connect,sendto,recvfrom cmd 2>&1 | grep -E "sin_addr|sin_port"

# 统计某个进程的信号
strace -p PID -e trace=signal -c
# 等待几秒后 Ctrl+C 查看统计

# 只看失败的 syscall
strace -e status=failed cmd

# 详细输出（不缩写结构体）
strace -v -s 256 cmd
```
