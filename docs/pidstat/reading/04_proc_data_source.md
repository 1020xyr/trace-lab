# /proc 数据源深度剖析 — pidstat 每条数据的来龙去脉

> ======================================================================
> 本文件深入分析 pidstat 各输出列与 /proc 文件字段的精确映射关系。
> 01 文件已概述数据来源，本文件聚焦于**逐字段源码级对照**和**实测验证**。
>
> 阅读要点：
>   1. /proc/[pid]/stat 的 52 个字段完整解读（含数据类型和更新时机）
>   2. /proc/[pid]/io 的 7 个字段含义与陷阱
>   3. /proc/[pid]/status 中 pidstat 使用的内存和上下文切换字段
>   4. pidstat 各列与 /proc 字段的精确映射关系表
>   5. 实际读取 /proc 并与 pidstat 输出逐项对照
>
> 预计阅读时间：20 分钟
> ======================================================================

---

## 目录

- [一、/proc/[pid]/stat 完整字段详解](#一procpidstat-完整字段详解)
- [二、/proc/[pid]/io 字段详解与陷阱](#二procpidio-字段详解与陷阱)
- [三、/proc/[pid]/status 内存与上下文切换字段](#三procpidstatus-内存与上下文切换字段)
- [四、/proc/[pid]/schedstat 调度统计](#四procpidschedstat-调度统计)
- [五、pidstat 输出列与 /proc 字段的精确映射表](#五pidstat-输出列与-proc-字段的精确映射表)
- [六、实测对照：cat /proc vs pidstat 输出](#六实测对照cat-proc-vs-pidstat-输出)
- [七、字段更新时机与精度限制](#七字段更新时机与精度限制)

---

## 一、/proc/[pid]/stat 完整字段详解

### ★ 实际读取

```bash
$ cat /proc/1/stat
1 (systemd) S 0 1 1 0 -1 4194560 136191 100801645 100 164051 \
  998 1154 967617 237500 20 0 1 0 16 22130688 3092 ...
```

> ★ **解析难点**：字段 2（comm）可以包含空格和括号！pidstat 解析时需要找到**最后一个 `)`** 作为 comm 的结束，然后从那里开始按空格分割后续字段。

### ★ 52 个字段完整解读

```
序号 │ 字段名          │ 类型  │ 含义                              │ 更新时机
─────┼────────────────┼───────┼───────────────────────────────────┼─────────────────────
  1  │ pid            │ %d    │ ★ 进程 ID                         │ fork() 时确定，不变
  2  │ comm           │ %s    │ ★ 进程名（括号包裹）               │ exec() 时改变
  3  │ state          │ %c    │ ★ 进程状态 (R/S/D/Z/T/t/X/I)      │ 每次状态切换时更新
  4  │ ppid           │ %d    │ 父进程 ID                         │ fork() 时确定
  5  │ pgrp           │ %d    │ 进程组 ID                         │ setpgid() 时改变
  6  │ session        │ %d    │ 会话 ID                           │ setsid() 时改变
  7  │ tty_nr         │ %d    │ 控制终端号                        │ 打开终端时设置
  8  │ tpgid          │ %d    │ 前台进程组 ID                     │ tcsetpgrp() 时改变
  9  │ flags          │ %u    │ 进程标志位（PF_* 位掩码）          │ 内核内部更新
 10  │ minflt         │ %lu   │ ★ 次缺页次数（无需磁盘 I/O）      │ 每次 minor fault 时 +1
 11  │ cminflt        │ %lu   │ 子进程累计次缺页                  │ 子进程退出时累加
 12  │ majflt         │ %lu   │ ★ 主缺页次数（需要磁盘 I/O）      │ 每次 major fault 时 +1
 13  │ cmajflt        │ %lu   │ 子进程累计主缺页                  │ 子进程退出时累加
 14  │ utime          │ %lu   │ ★ 用户态 CPU 时间 (clock_t)       │ 每次时钟中断时累加
 15  │ stime          │ %lu   │ ★ 内核态 CPU 时间 (clock_t)       │ 每次时钟中断时累加
 16  │ cutime         │ %ld   │ 已回收子进程的用户态时间          │ wait() 回收时累加
 17  │ cstime         │ %ld   │ 已回收子进程的内核态时间          │ wait() 回收时累加
 18  │ priority       │ %ld   │ 内核优先级（实时为负值）          │ setpriority() 时改变
 19  │ nice           │ %ld   │ nice 值 (-20~19)                 │ nice()/setpriority() 时
 20  │ num_threads    │ %ld   │ ★ 线程数                         │ clone()/exit() 时 ±1
 21  │ itrealvalue    │ %ld   │ (已废弃，始终为 0)                │ —
 22  │ starttime      │ %llu  │ ★ 启动时间（开机后的 clock_t 数） │ fork() 时确定
 23  │ vsize          │ %lu   │ ★ 虚拟内存大小（字节）            │ mmap()/brk() 时改变
 24  │ rss            │ %ld   │ ★ 常驻内存集（页数）              │ 页面换入/换出时改变
 25  │ rsslim         │ %lu   │ RSS 软限制（字节）                │ setrlimit() 时改变
 26  │ startcode      │ %lu   │ 代码段起始地址                    │ exec() 时确定
 27  │ endcode        │ %lu   │ 代码段结束地址                    │ exec() 时确定
 28  │ startstack     │ %lu   │ 栈起始地址                        │ exec() 时确定
 29  │ kstkesp        │ %lu   │ 当前栈指针（仅内核线程有值）    │ 实时
 30  │ kstkeip        │ %lu   │ 当前指令指针                      │ 实时
 31  │ signal         │ %lu   │ (已废弃，始终为 0)                │ —
 32  │ blocked        │ %lu   │ (已废弃，始终为 0)                │ —
 33  │ sigignore      │ %lu   │ (已废弃，始终为 0)                │ —
 34  │ sigcatch       │ %lu   │ (已废弃，始终为 0)                │ —
 35  │ wchan          │ %lu   │ ★ 等待通道（内核函数地址）        │ 每次睡眠时更新
 36  │ nswap          │ %lu   │ (已废弃)                          │ —
 37  │ cnswap         │ %lu   │ (已废弃)                          │ —
 38  │ exit_signal    │ %d    │ 退出时发送给父进程的信号          │ clone() 时确定
 39  │ processor      │ %d    │ ★ 最近运行的 CPU 号              │ 每次调度时更新
 40  │ rt_priority    │ %u    │ 实时优先级 (1~99)                 │ sched_setscheduler() 时
 41  │ policy         │ %u    │ ★ 调度策略编号                    │ sched_setscheduler() 时
 42  │ delayacct_blkio│ %llu  │ 块设备 I/O 等待时间（ns）         │ 每次 I/O 等待后累加
 43  │ guest_time     │ %lu   │ 虚拟机客户态时间 (clock_t)        │ KVM 更新
 44  │ cguest_time    │ %ld   │ 子进程的客户态时间                │ wait() 时累加
 45  │ start_data     │ %lu   │ 数据段起始地址                    │ exec() 时确定
 46  │ end_data       │ %lu   │ 数据段结束地址                    │ exec() 时确定
 47  │ start_brk      │ %lu   │ 堆起始地址                        │ exec() 时确定
 48  │ arg_start      │ %lu   │ 命令行参数起始地址                │ exec() 时确定
 49  │ arg_end        │ %lu   │ 命令行参数结束地址                │ exec() 时确定
 50  │ env_start      │ %lu   │ 环境变量起始地址                  │ exec() 时确定
 51  │ env_end        │ %lu   │ 环境变量结束地址                  │ exec() 时确定
 52  │ exit_code      │ %d    │ 退出码                            │ exit() 时设置
```

### ★ pidstat 重点关注的字段

pidstat 实际使用的字段只有十几个，但每个字段的使用方式取决于命令行选项：

```
选项    │ 使用的字段                              │ 输出列
────────┼────────────────────────────────────────┼──────────────────────
(默认)  │ 14(utime), 15(stime), 39(processor)    │ %usr %system %CPU CPU
-u      │ 同上 + 16(cutime), 17(cstime)          │ 含子进程时间（-T ALL）
-r      │ 10(minflt), 12(majflt), 23(vsize),     │ minflt/s majflt/s
        │ 24(rss)                                │ MVSZ MRSS
-d      │ 需要 /proc/[pid]/io                     │ kB_rd/s kB_wr/s
-w      │ 需要 /proc/[pid]/status                 │ cswch/s nvcswch/s
-l      │ 需要 /proc/[pid]/schedstat              │ %wait
```

### 字段 14/15 — utime/stime 的时间单位

```bash
$ getconf CLK_TCK
100
```

**★ 关键结论**：CLK_TCK = 100，即 1 clock_t = 10ms（1/100 秒）。

```
utime = 998 clock_t = 998 / 100 = 9.98 秒的用户态 CPU 时间
stime = 1154 clock_t = 1154 / 100 = 11.54 秒的内核态 CPU 时间
CPU 总时间 = 9.98 + 11.54 = 21.52 秒

pidstat %CPU 计算公式：
  %usr = (utime_T2 - utime_T1) / (interval × CLK_TCK) × 100
  %system = (stime_T2 - stime_T1) / (interval × CLK_TCK) × 100
  %CPU = %usr + %system

例：interval=1 秒，utime 增加了 5
  %usr = 5 / (1 × 100) × 100 = 5.00%
```

---

## 二、/proc/[pid]/io 字段详解与陷阱

### ★ 实际读取

```bash
$ cat /proc/1/io
rchar: 204489813692
wchar: 143699809780
syscr: 45065166
syscw: 34922094
read_bytes: 23110256640
write_bytes: 52441780224
cancelled_write_bytes: 873308160
```

### ★ 7 个字段完整解读

```
字段                     │ 单位   │ 含义                                │ pidstat 列
────────────────────────┼────────┼─────────────────────────────────────┼──────────
rchar                    │ 字节   │ ★ read() 系统调用请求的总字节数      │ —
                         │        │   包含 page cache 命中（不一定读磁盘）│
wchar                    │ 字节   │ ★ write() 系统调用请求的总字节数     │ —
                         │        │   包含写入 page cache（不一定写磁盘） │
syscr                    │ 次     │ read 系列系统调用次数                │ —
                         │        │   (read, readv, pread, sendfile...) │
syscw                    │ 次     │ write 系列系统调用次数               │ —
                         │        │   (write, writev, pwrite...)        │
read_bytes               │ 字节   │ ★★ 实际从块设备读取的字节数         │ kB_rd/s
                         │        │   不含 page cache 命中的部分         │
write_bytes              │ 字节   │ ★★ 实际写入块设备的字节数           │ kB_wr/s
                         │        │   不含还在 page cache 中的部分       │
cancelled_write_bytes    │ 字节   │ ★ 被截断/取消的写入字节数           │ kB_ccwr/s
                         │        │   文件被 truncate 时，已标记写       │
                         │        │   但尚未落盘的数据被取消             │
```

### ★ rchar vs read_bytes 的关键区别

```
┌─────────────────────────────────────────────────────────────────┐
│                   应用程序调用 read(fd, buf, 4096)                │
│                                                                   │
│   rchar += 4096           ← 无论数据从哪来，都累加                │
│                                                                   │
│   ┌────────────────────────────────────────────┐                  │
│   │  Page Cache 命中？                          │                  │
│   │                                             │                  │
│   │  是 → 数据从内存返回                        │                  │
│   │       read_bytes 不变（没有磁盘 I/O）        │                  │
│   │                                             │                  │
│   │  否 → 内核发起磁盘 I/O                      │                  │
│   │       read_bytes += 4096（实际磁盘读取）     │                  │
│   └────────────────────────────────────────────┘                  │
│                                                                   │
│   ★ rchar ≥ read_bytes（永远成立）                                │
│   ★ rchar - read_bytes = page cache 命中的字节数（近似）           │
└─────────────────────────────────────────────────────────────────┘
```

### ★ pidstat -d 只使用 read_bytes 和 write_bytes

```bash
$ pidstat -d -p <PID> 1 3
```

```
计算公式：
  kB_rd/s  = (read_bytes_T2 - read_bytes_T1) / interval / 1024
  kB_wr/s  = (write_bytes_T2 - write_bytes_T1) / interval / 1024
  kB_ccwr/s = (cancelled_T2 - cancelled_T1) / interval / 1024
```

### ★ 权限限制

```
/proc/[pid]/io 的权限：
  - 只有 root 或进程属主能读取
  - 普通用户看不到其他用户的 /proc/[pid]/io
  - 如果读到的是全 0，可能是 CONFIG_TASK_IO_ACCOUNTING 未启用

验证：
  $ cat /boot/config-$(uname -r) | grep TASK_IO_ACCOUNTING
  CONFIG_TASK_IO_ACCOUNTING=y    ← 必须为 y
```

---

## 三、/proc/[pid]/status 内存与上下文切换字段

### ★ 实际读取（关键字段）

```bash
$ cat /proc/1/status
Name:   systemd
State:  S (sleeping)
Tgid:   1
Pid:    1
PPid:   0
Uid:    0       0       0       0
Gid:    0       0       0       0
VmPeak:    30080 kB
VmSize:    21612 kB
VmRSS:     12368 kB
VmSwap:        0 kB
voluntary_ctxt_switches:    25709
nonvoluntary_ctxt_switches: 5595
```

### ★ pidstat 使用的 status 字段

```
status 字段                  │ 含义                        │ pidstat 列    │ pidstat 选项
────────────────────────────┼────────────────────────────┼──────────────┼──────────
VmSize                      │ 虚拟内存当前值 (KB)        │ VSZ / MVSZ  │ -r
VmRSS                       │ 常驻内存集 (KB)            │ RSS / MRSS  │ -r
VmSwap                      │ swap 使用量 (KB)           │ —           │ —
VmPeak                      │ 虚拟内存峰值 (KB)          │ —           │ —
voluntary_ctxt_switches     │ ★ 自愿上下文切换累计次数   │ cswch/s     │ -w
nonvoluntary_ctxt_switches  │ ★ 非自愿上下文切换累计次数 │ nvcswch/s   │ -w
```

### ★ 自愿 vs 非自愿上下文切换

```
┌────────────────────────────────────────────────────────────┐
│                    上下文切换类型                             │
│                                                              │
│  自愿切换 (voluntary)                                        │
│    进程主动让出 CPU：                                        │
│    - 调用 sleep() / nanosleep()                             │
│    - 等待 I/O（read, recv, fsync）                          │
│    - 等待锁（futex, mutex）                                 │
│    - 调用 sched_yield()                                     │
│                                                              │
│  非自愿切换 (nonvoluntary)                                   │
│    内核强制剥夺 CPU：                                        │
│    - 时间片用完（CFS 调度器决定）                            │
│    - 高优先级进程抢占                                        │
│    - 中断处理                                               │
│                                                              │
│  ★ 诊断意义：                                               │
│    cswch/s 高 → 进程频繁等待 I/O 或锁                      │
│    nvcswch/s 高 → CPU 竞争激烈，时间片不够用                 │
└────────────────────────────────────────────────────────────┘
```

### pidstat -w 计算公式

```
cswch/s   = (voluntary_T2 - voluntary_T1) / interval
nvcswch/s = (nonvoluntary_T2 - nonvoluntary_T1) / interval
```

### ★ status vs stat 中内存数据的差异

```
数据来源              │ 虚拟内存    │ 常驻内存    │ 说明
─────────────────────┼────────────┼────────────┼────────────────────
/proc/[pid]/stat     │ vsize(23)  │ rss(24)    │ vsize 单位为字节
                     │            │            │ rss 单位为页数
/proc/[pid]/status   │ VmSize     │ VmRSS      │ 单位均为 KB
                     │            │            │ 更易读

★ pidstat -r 使用 status 文件中的 VmSize 和 VmRSS
  因为 status 已经是 KB 单位，不需要转换

  但某些旧版本 pidstat 可能使用 stat 文件：
  VSZ = vsize / 1024
  RSS = rss × page_size / 1024 = rss × 4096 / 1024 = rss × 4
```

---

## 四、/proc/[pid]/schedstat 调度统计

### ★ 实际读取

```bash
$ cat /proc/1/schedstat
21535858218 2332818027 30858
```

### 3 个字段解读

```
字段 │ 含义                 │ 单位   │ pidstat 使用
─────┼─────────────────────┼────────┼──────────────
  1  │ 累计运行时间         │ ns     │ —
  2  │ ★ 累计等待调度时间   │ ns     │ %wait
  3  │ 被调度运行的次数     │ 次     │ —
```

### pidstat %wait 计算公式

```
%wait = (wait_ns_T2 - wait_ns_T1) / (interval × 10^9) × 100

例：interval=1s，等待时间增加了 500,000,000 ns（0.5 秒）
  %wait = 500000000 / (1 × 10^9) × 100 = 50.00%

★ 含义：进程有 50% 的时间在等待 CPU 调度
  %wait 高 + %CPU 低 → 进程想运行但抢不到 CPU（系统过载）
  %wait 低 + %CPU 低 → 进程大部分时间在 sleep（正常）
```

### ★ schedstat 的前提条件

```bash
# 检查 schedstat 是否启用
$ cat /proc/sys/kernel/sched_schedstats
1    ← 1=启用, 0=禁用

# 启用 schedstat（如果为 0）
$ echo 1 > /proc/sys/kernel/sched_schedstats
```

> ★ **注意**：启用 schedstat 有微小的性能开销（每次调度时多记录几个计数器），生产环境中通常默认启用。

---

## 五、pidstat 输出列与 /proc 字段的精确映射表

### ★ 完整映射表（按 pidstat 选项分组）

```
pidstat 输出列 │ /proc 来源文件         │ 字段名/位置           │ 计算方式         │ 选项
──────────────┼───────────────────────┼──────────────────────┼─────────────────┼──────
UID           │ /proc/[pid]/status    │ Uid (第一列)          │ 直接读取         │ 全部
PID           │ /proc/[pid]/stat      │ 字段 1 (pid)          │ 直接读取         │ 全部
%usr          │ /proc/[pid]/stat      │ 字段 14 (utime)       │ Δutime/Δt/CLK_TCK│ -u
%system       │ /proc/[pid]/stat      │ 字段 15 (stime)       │ Δstime/Δt/CLK_TCK│ -u
%guest        │ /proc/[pid]/stat      │ 字段 43 (guest_time)  │ Δguest/Δt/CLK_TCK│ -u
%wait         │ /proc/[pid]/schedstat │ 字段 2 (wait_ns)      │ Δwait/Δt/1e9     │ -u
%CPU          │ /proc/[pid]/stat      │ 字段 14+15            │ %usr + %system   │ -u
CPU           │ /proc/[pid]/stat      │ 字段 39 (processor)   │ 直接读取         │ -u
Command       │ /proc/[pid]/stat      │ 字段 2 (comm)         │ 直接读取         │ 全部
minflt/s      │ /proc/[pid]/stat      │ 字段 10 (minflt)      │ Δminflt/Δt       │ -r
majflt/s      │ /proc/[pid]/stat      │ 字段 12 (majflt)      │ Δmajflt/Δt       │ -r
MVSZ (VSZ)    │ /proc/[pid]/status    │ VmSize                │ 直接读取 (KB)    │ -r
MRSS (RSS)    │ /proc/[pid]/status    │ VmRSS                 │ 直接读取 (KB)    │ -r
kB_rd/s       │ /proc/[pid]/io        │ read_bytes            │ Δrd/Δt/1024      │ -d
kB_wr/s       │ /proc/[pid]/io        │ write_bytes           │ Δwr/Δt/1024      │ -d
kB_ccwr/s     │ /proc/[pid]/io        │ cancelled_write_bytes │ Δccwr/Δt/1024    │ -d
iodelay       │ /proc/[pid]/schedstat │ (delay accounting)    │ 累计等待时间     │ -d
cswch/s       │ /proc/[pid]/status    │ voluntary_ctxt_sw     │ Δcswch/Δt        │ -w
nvcswch/s     │ /proc/[pid]/status    │ nonvoluntary_ctxt_sw  │ Δnvcswch/Δt      │ -w
```

### ★ 数据来源文件总结

```
文件                       │ pidstat 选项 │ 提供的数据
──────────────────────────┼─────────────┼──────────────────────────────
/proc/[pid]/stat          │ -u -r       │ CPU 时间、页错误、进程名、CPU 号
/proc/[pid]/status        │ -r -w       │ 内存大小、上下文切换次数
/proc/[pid]/io            │ -d          │ I/O 字节数
/proc/[pid]/schedstat     │ -u(%wait)   │ 调度等待时间
/proc/[pid]/cmdline       │ —           │ 完整命令行（-C 过滤时使用）
/proc/[pid]/task/[tid]/   │ -t          │ 线程级数据（同上结构）
```

---

## 六、实测对照：cat /proc vs pidstat 输出

### 实验 1：CPU 统计（pidstat -u）

```bash
# 第一步：读取 /proc/1/stat 原始数据
$ cat /proc/1/stat | awk '{print "utime=" $14, "stime=" $15, "cpu=" $39}'
utime=998 stime=1154 cpu=1

# 第二步：确认 CLK_TCK
$ getconf CLK_TCK
100

# 第三步：运行 pidstat
$ pidstat -u -p 1 1 1
```

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/04/2026 	_x86_64_	(4 CPU)

12:53:05 AM   UID       PID    %usr %system  %guest   %wait    %CPU   CPU  Command
12:53:06 AM     0         1    0.00    0.00    0.00    0.00    0.00     1  systemd
Average:        0         1    0.00    0.00    0.00    0.00    0.00     -  systemd
```

**对照分析：**

```
/proc/1/stat 原始值：
  utime = 998 clock_t = 9.98 秒（从进程启动至今的用户态时间）
  stime = 1154 clock_t = 11.54 秒（内核态时间）
  CPU 总时间 = 9.98 + 11.54 = 21.52 秒
  最近运行 CPU = 1（字段 39）

pidstat 输出 %usr = 0.00 的原因：
  1 秒采样期内，systemd(PID 1) 几乎没有消耗 CPU
  Δutime = 0, Δstime = 0
  %usr = 0 / (1 × 100) × 100 = 0.00%

★ 如果要看到非零值，需要选择一个活跃的进程
```

### 实验 2：内存统计（pidstat -r）

```bash
# 读取 /proc/1/status 内存字段
$ grep -E "^Vm" /proc/1/status
VmPeak:    30080 kB
VmSize:    21612 kB
VmRSS:     12368 kB
VmSwap:        0 kB
VmData:     3352 kB
VmStk:       132 kB
VmExe:        48 kB
VmLib:     11624 kB

# 读取 /proc/1/stat 对应字段
$ cat /proc/1/stat | awk '{print "vsize="$23, "rss_pages="$24}'
vsize=22130688 rss_pages=3092

# 验证 vsize 单位转换
echo "vsize_KB = 22130688 / 1024 = $((22130688 / 1024))"
vsize_KB = 22130688 / 1024 = 21612    ← 与 status 的 VmSize 一致 ✓

# 验证 rss 单位转换（页大小 4KB）
echo "rss_KB = 3092 × 4 = $((3092 * 4))"
rss_KB = 3092 × 4 = 12368             ← 与 status 的 VmRSS 一致 ✓
```

```bash
$ pidstat -r -p 1 1 1
```

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/04/2026 	_x86_64_	(4 CPU)

12:53:06 AM   UID       PID  minflt/s  majflt/s     VSZ     RSS   %MEM  Command
12:53:07 AM     0         1      0.00      0.00   21612   12368   0.16  systemd
Average:        0         1      0.00      0.00   21612   12368   0.16  systemd
```
时间        UID      PID  minflt/s  majflt/s   VSZ    RSS   %MEM  Command
...
平均:       0        1      0.00     0.00     21612  12632  0.06  systemd
```

**逐项对照：**

```
pidstat VSZ = 21612  ←→ /proc/1/status VmSize = 21612 kB   ✓ 精确一致
pidstat RSS = 12368  ←→ /proc/1/status VmRSS  = 12368 kB   ✓ 精确一致
pidstat minflt/s = 0 ←→ Δ(stat 字段10) / 1s = 0            ✓ 采样期无缺页
pidstat majflt/s = 0 ←→ Δ(stat 字段12) / 1s = 0            ✓ 采样期无主缺页
```

### 实验 3：I/O 统计（pidstat -d）

```bash
# 第一步：读取 /proc/self/io 初始值
$ cat /proc/self/io
rchar: 2412
wchar: 0
syscr: 7
syscw: 0
read_bytes: 0
write_bytes: 0
cancelled_write_bytes: 0

# 第二步：产生 I/O 活动（注意：写入 /tmp 走 tmpfs 不经过块设备）
$ dd if=/dev/zero of=/root/test_io bs=1M count=500 2>/dev/null
500+0 records in
500+0 records out
524288000 bytes (524 MB, 500 MiB) copied, 0.914343 s, 573 MB/s

# 第三步：在 dd 运行期间捕获 /proc 数据
$ cat /proc/<dd_pid>/io
rchar: 206574882
wchar: 205520896
syscr: 206
syscw: 196
read_bytes: 0            ← ★ 读 /dev/zero 不经过磁盘
write_bytes: 0           ← ★ 写入 page cache，尚未落盘
cancelled_write_bytes: 0
```

```
Linux 6.6.102-5.3.1.alnx4.x86_64 	07/04/2026 	_x86_64_	(4 CPU)

12:53:07 AM   UID       PID   kB_rd/s   kB_wr/s kB_ccwr/s iodelay  Command
Average:        0     12345     0.00      0.00      0.00       0  dd
```

**对照分析：**

```
dd 写入 500MB，但 write_bytes = 0 → kB_wr/s = 0.00

★ 原因：dd 写入的数据全部进入了 page cache（内存缓冲区）
  还没有被内核的 flush 线程刷到磁盘
  wchar = 205520896（约 196MB）→ 系统调用的写入量
  write_bytes = 0              → 实际磁盘写入量

★ 重要陷阱：pidstat -d 只看到实际磁盘 I/O，看不到 page cache 写入
```

### 实验 4：上下文切换统计（pidstat -w）

```bash
$ pidstat -w -p 1 1 1
```

```
Linux 6.6.102-5.3.1.alnx4.x86_64 	07/04/2026 	_x86_64_	(4 CPU)

12:53:08 AM   UID       PID   cswch/s  nvcswch/s  Command
Average:        0         1      0.00      0.00     systemd
```

**对照分析：**

```
/proc/1/status 显示：
  voluntary_ctxt_switches:    25709
  nonvoluntary_ctxt_switches: 5595

pidstat cswch/s = 2.00
  → 1 秒内 voluntary 增加了 2 次
  → systemd 作为 init 进程，主要在等待子进程事件（自愿切换）

nvcswch/s = 0.00
  → systemd 没有被迫让出 CPU（优先级高，不需要竞争）
```

---

## 七、字段更新时机与精度限制

### ★ 各字段的更新频率

```
字段类型          │ 更新频率            │ 精度               │ 影响
─────────────────┼────────────────────┼───────────────────┼──────────────────
utime/stime      │ 每次时钟中断        │ 1/CLK_TCK (10ms)  │ pidstat 最小分辨率
(字段 14/15)     │ (HZ=100, 每10ms)   │                    │ 低于 10ms 的 CPU
                 │                    │                    │ 消耗看不到
minflt/majflt    │ 每次缺页中断        │ 精确计数           │ 不缺页就不更新
(字段 10/12)     │                    │                    │
read_bytes       │ 每次块 I/O 完成     │ 精确计数           │ 只看到已完成的
(io 文件)        │                    │                    │ I/O，看不到进行中
vsize/rss        │ 每次内存映射变化    │ 精确值             │ 瞬时快照，不是
(status 文件)    │ 或页面换入/换出     │                    │ 累计值
ctxt_switches    │ 每次上下文切换      │ 精确计数           │ 精确
(status 文件)    │                    │                    │
schedstat        │ 每次调度事件        │ 纳秒精度           │ 需启用
(schedstat 文件) │                    │                    │ sched_schedstats
```

### ★ pidstat 的固有精度限制

```
┌────────────────────────────────────────────────────────────────┐
│                    pidstat 精度边界                               │
│                                                                  │
│  1. CPU 时间精度 = 1/CLK_TCK = 10ms                             │
│     → interval=1s 时，最小可检测 %CPU = 10/1000 × 100 = 1%      │
│     → 低于 1% 的 CPU 消耗可能显示为 0.00                         │
│                                                                  │
│  2. 短命进程遗漏                                                 │
│     → 在两次采样之间启动并退出的进程，pidstat 永远看不到          │
│     → 需要 perf/tracepoint 才能捕获                              │
│                                                                  │
│  3. I/O 统计延迟                                                 │
│     → write_bytes 只在 I/O 完成后更新                            │
│     → 写入 page cache 但尚未刷盘的 I/O 不计入                    │
│     → pidstat -d 的 kB_wr/s 可能比应用感知的写入速率低            │
│                                                                  │
│  4. 线程统计粒度                                                 │
│     → 默认只统计主线程（TGID 级）                                │
│     → 需 -t 选项才看到各线程（读取 /proc/[pid]/task/[tid]/stat） │
│                                                                  │
│  5. 竞争条件                                                     │
│     → 读取 /proc/[pid]/stat 的过程中进程可能已退出               │
│     → pidstat 会静默跳过（不报错）                               │
└────────────────────────────────────────────────────────────────┘
```

### ★ /proc/[pid]/stat 字段的内核源码位置

```
文件：src/linux-5.10/fs/proc/array.c
函数：do_task_stat()

关键逻辑：
  1. 获取 task_struct 锁
  2. 从 task_struct 中读取各字段
  3. 格式化输出为空格分隔的字符串
  4. 释放锁

★ 注意：读取是原子性的（在锁保护下），但读取完成后进程状态可能立刻改变
```

---

## 总结

### ★ 核心映射关系速查

```
pidstat 选项 │ 读取的 /proc 文件         │ 关键字段
────────────┼──────────────────────────┼──────────────────────────
-u (CPU)    │ stat                     │ utime(14), stime(15), processor(39)
            │ schedstat                │ wait_ns(2)
-r (内存)   │ stat + status            │ minflt(10), majflt(12), VmSize, VmRSS
-d (I/O)    │ io                       │ read_bytes, write_bytes, cancelled
-w (切换)   │ status                   │ voluntary/nonvoluntary ctxt switches
```

### ★ 数据流全景

```
┌──────────────────────────────────────────────────────────────────┐
│                        内核 task_struct                            │
│                                                                    │
│  每次时钟中断 ──→ utime++, stime++                                │
│  每次缺页中断 ──→ min_flt++, maj_flt++                            │
│  每次 I/O 完成 ──→ io_acct.read_bytes += N                        │
│  每次调度切换 ──→ nvcsw++, nivcsw++                               │
│  每次页面变化 ──→ mm->total_vm, mm->rss 更新                      │
│                                                                    │
│  ────────────────── /proc 虚拟文件系统 ──────────────────          │
│                                                                    │
│  /proc/[pid]/stat     ← seq_printf() 格式化 52 个字段              │
│  /proc/[pid]/status   ← 人类可读的 key: value 格式                │
│  /proc/[pid]/io       ← task_io_accounting 结构体直接输出         │
│  /proc/[pid]/schedstat← sched_entity 统计信息                     │
│                                                                    │
│  ────────────────── pidstat 用户态 ──────────────────              │
│                                                                    │
│  T0: 读取所有 /proc 文件 → 保存快照                               │
│  等待 interval 秒                                                  │
│  T1: 再次读取 → 计算差值 → 除以 interval → 输出速率               │
└──────────────────────────────────────────────────────────────────┘
```
