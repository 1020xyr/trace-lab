# pidstat 工作原理

> pidstat 不采集数据——它只是 `/proc/[pid]/` 文件的格式化视图。
> 理解这一点，是掌握 pidstat 的关键。

---

## 一、整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     pidstat 用户态                           │
│  1. 遍历 /proc/[pid]/ 目录                                   │
│  2. 读取 stat/status/io/schedstat 文件                      │
│  3. 解析字段、计算差值（间隔模式）                            │
│  4. 格式化输出表格                                           │
├─────────────────────────────────────────────────────────────┤
│                     /proc 文件系统（内核虚拟文件系统）         │
│  /proc/[pid]/stat      → CPU 时间、页错误                    │
│  /proc/[pid]/status    → 内存、上下文切换                    │
│  /proc/[pid]/io        → I/O 字节数                          │
│  /proc/[pid]/schedstat → 调度等待时间                        │
├─────────────────────────────────────────────────────────────┤
│                     内核 task_struct                         │
│  utime/stime/min_flt/maj_flt/nvcsw/nivcsw                  │
│  每次系统调用、中断、调度时由内核更新                          │
└─────────────────────────────────────────────────────────────┘
```

### /proc/[pid]/ 目录注册机制（内核源码）

```c
/* 源码位置：src/linux-5.10/fs/proc/base.c:3159-3266 */
/* ★ /proc/[pid]/ 下的每个虚拟文件都在这里注册
 * 宏含义：ONE = 单次读取（调用 show 函数输出全部内容）
 *        REG = 常规文件（有完整的 file_operations）
 *        DIR = 子目录  LNK = 符号链接 */
static const struct pid_entry tgid_base_stuff[] = {
    DIR("task",       S_IRUGO|S_IXUGO, proc_task_inode_operations, ...),
    DIR("fd",         S_IRUSR|S_IXUSR, proc_fd_inode_operations, ...),
    REG("environ",    S_IRUSR, proc_environ_operations),
    ONE("status",     S_IRUGO, proc_pid_status),       // ★ → proc_pid_status()
    ONE("stat",       S_IRUGO, proc_tgid_stat),         // ★ → do_task_stat(whole=1)
    ONE("statm",      S_IRUGO, proc_pid_statm),         // ★ → proc_pid_statm()
    REG("maps",       S_IRUGO, proc_pid_maps_operations),
    ONE("io",        S_IRUSR, proc_tgid_io_accounting), // ★ → do_io_accounting(whole=1)
    ONE("schedstat",  S_IRUGO, proc_pid_schedstat),     // ★ → proc_pid_schedstat()
    LNK("cwd",        proc_cwd_link),
    LNK("root",       proc_root_link),
    LNK("exe",        proc_exe_link),
    // ... 更多文件注册 ...
};
```

> **注意：** `proc_tgid_stat` 调用 `do_task_stat(m, ns, pid, task, 1)`，`whole=1` 表示聚合所有线程；
> 线程级 `proc_tid_stat` 调用 `do_task_stat(m, ns, pid, task, 0)`，`whole=0` 表示单线程。

### 与 blktrace 的核心差异

| 维度 | blktrace | pidstat |
|------|----------|---------|
| 数据来源 | 内核 tracepoint + relay buffer | /proc 文件系统 |
| 采集方式 | 事件驱动（每个 I/O 产生事件） | 轮询驱动（周期性读取） |
| 精度 | 纳秒级时间戳 | 秒级采样间隔 |
| 开销 | 需内核 hook + 大量 buffer | 仅读文件，几乎无开销 |
| 数据粒度 | 每个 I/O 请求 | 每个进程的聚合速率 |
| 工具链 | blktrace → blkparse → btt | pidstat 一体化 |

---

## 二、数据来源详解

### 2.1 /proc/[pid]/stat — CPU 和页错误

```bash
$ cat /proc/1/stat
1 (systemd) S 0 1 1 0 -1 4194560 127241 79618744 100 158274
  905 1071 771861 184018 20 0 1 0 16 22130688 3158 ...
  ↑       ↑  ↑  ↑                                  ↑    ↑
  PID    comm状态 父PID                            utime stime
                                                      ↑     ↑
                                              用户态时间  内核态时间
                                              (clock_t)  (clock_t)
```

**pidstat 关心的字段（按位置编号）：**

| 字段号 | 名称 | pidstat 用途 |
|--------|------|-------------|
| 1 | pid | PID 列 |
| 2 | comm | Command 列 |
| 10 | minflt | minflt/s（次页错误率） |
| 12 | majflt | majflt/s（主页错误率） |
| 14 | utime | %usr（用户态 CPU 时间） |
| 15 | stime | %system（内核态 CPU 时间） |
| 16 | cutime | 子进程用户态时间（-T ALL 时使用） |
| 17 | cstime | 子进程内核态时间 |
| 39 | processor | CPU 列（最后运行在哪个 CPU） |
| 42 | guest_time | %guest（虚拟机客户时间） |

**时间单位：** clock_t（通常 100Hz，即 1 clock_t = 10ms）。可通过 `getconf CLK_TCK` 验证。

**进程状态字符映射（内核源码）**

```c
/* 源码位置：src/linux-5.10/include/linux/sched.h:79-112 */
/* ★ 进程状态常量定义 —— 对应 /proc/[pid]/stat 字段 3 的状态字符 */
#define TASK_RUNNING            0x0000   // → 'R' 可运行/正在运行
#define TASK_INTERRUPTIBLE      0x0001   // → 'S' 可中断睡眠（等待事件/信号）
#define TASK_UNINTERRUPTIBLE    0x0002   // → 'D' 不可中断睡眠（通常等待 I/O）
#define __TASK_STOPPED          0x0004   // → 'T' 停止（收到 SIGSTOP）
#define __TASK_TRACED           0x0008   // → 't' 被调试器跟踪停止
#define EXIT_DEAD               0x0010   // → 'X' 已退出（最终状态）
#define EXIT_ZOMBIE             0x0020   // → 'Z' 僵尸进程（父进程未回收）
#define TASK_PARKED             0x0040   // → 'P' 停放（内核线程专用）
#define TASK_DEAD               0x0080   // → 'X' 死亡
#define TASK_IDLE   (TASK_UNINTERRUPTIBLE | TASK_NOLOAD) // → 'I' 空闲（不计入负载）

/* 便捷组合宏 */
#define TASK_KILLABLE  (TASK_WAKEKILL | TASK_UNINTERRUPTIBLE) // 可被致命信号唤醒的 D 状态
#define TASK_STOPPED   (TASK_WAKEKILL | __TASK_STOPPED)
#define TASK_TRACED    (TASK_WAKEKILL | __TASK_TRACED)
```

```c
/* 源码位置：src/linux-5.10/fs/proc/array.c:129-149 */
/* ★ 状态字符数组 —— task_state_index() 返回索引，查表得到状态字符串 */
static const char * const task_state_array[] = {
    "R (running)",       /* 0x00 */
    "S (sleeping)",      /* 0x01 */
    "D (disk sleep)",    /* 0x02 */
    "T (stopped)",       /* 0x04 */
    "t (tracing stop)",  /* 0x08 */
    "X (dead)",          /* 0x10 */
    "Z (zombie)",        /* 0x20 */
    "P (parked)",        /* 0x40 */
    "I (idle)",          /* 0x80 */
};

static inline const char *get_task_state(struct task_struct *tsk)
{
    BUILD_BUG_ON(1 + ilog2(TASK_REPORT_MAX) != ARRAY_SIZE(task_state_array));
    return task_state_array[task_state_index(tsk)];
    /* ★ task_state_index() 用 fls() 找到 state 中最高置位 bit，
     *    作为数组索引。例如 state=0x0001 → fls(1)=1 → "S (sleeping)" */
}
```

```c
/* 源码位置：src/linux-5.10/include/linux/sched.h:1476-1490 */
/* ★ 状态索引计算：将 task->state 映射到 task_state_array 下标 */
static inline unsigned int task_state_index(struct task_struct *tsk)
{
    unsigned int tsk_state = READ_ONCE(tsk->state);
    unsigned int state = (tsk_state | tsk->exit_state) & TASK_REPORT;

    BUILD_BUG_ON_NOT_POWER_OF_2(TASK_REPORT_MAX);

    if (tsk_state == TASK_IDLE)
        state = TASK_REPORT_IDLE;     // ★ TASK_IDLE 特殊处理，映射到 'I'

    return fls(state);   // ★ fls = find last set，返回最高位位置作为数组索引
}
```

**%CPU 计算公式：**
```
%CPU = (utime_new - utime_old + stime_new - stime_old) / (interval × CLK_TCK) × 100
```

**内核源码：`do_task_stat()` 输出逻辑**

```c
/* 源码位置：src/linux-5.10/fs/proc/array.c:431-622 */
/* ★ 这是 /proc/[pid]/stat 文件的核心输出函数
 * whole=1 → 进程级（聚合所有线程）  whole=0 → 线程级 */
static int do_task_stat(struct seq_file *m, struct pid_namespace *ns,
                        struct pid *pid, struct task_struct *task, int whole)
{
    // ... 省略前置逻辑 ...

    /* ★ 获取进程状态字符：'R', 'S', 'D', 'T' 等 */
    state = *get_task_state(task);              // 行 451

    /* 获取内存描述符 */
    mm = get_task_mm(task);                     // 行 454

    if (whole) {
        /* ★ whole=1：遍历所有线程，累加 min_flt, maj_flt, utime, stime 等 */
        struct task_struct *t = task;
        min_flt = task->min_flt;
        maj_flt = task->maj_flt;
        // ... 累加所有线程的统计值 ...
        while_each_thread(task, t) {
            min_flt += t->min_flt;
            maj_flt += t->maj_flt;
            // ... gtime, utime, stime 也类似累加 ...
        }                                       // 行 501-513
    } else {
        /* whole=0：直接取当前线程的值 */
        min_flt = task->min_flt;
        maj_flt = task->maj_flt;               // 行 524-528
    }

    /* ★ CPU 时间转换：纳秒 → clock_t（100Hz） */
    task_cputime_adjusted(task, &utime, &stime);
    /* task_cputime_adjusted 内部调用 nsec_to_clock_t()
     * 将 task->utime / task->stime（纳秒）转换为 clock_t 单位 */

    /* ★★★★★ 以下为 /proc/[pid]/stat 的 52 个字段输出 ★★★★★ */
    seq_put_decimal_ull(m, "", pid_nr_ns(pid, ns));      // 字段 1:  pid
    seq_puts(m, " (");
    proc_task_name(m, task, false);                        // 字段 2:  comm（进程名，括号包裹）
    seq_puts(m, ") ");
    seq_putc(m, state);                                    // 字段 3:  state（R/S/D/T...）
    seq_put_decimal_ll(m, " ", ppid);                      // 字段 4:  ppid（父进程 PID）
    seq_put_decimal_ll(m, " ", pgid);                      // 字段 5:  pgid
    seq_put_decimal_ll(m, " ", sid);                       // 字段 6:  sid
    seq_put_decimal_ll(m, " ", tty_nr);                    // 字段 7:  tty_nr
    seq_put_decimal_ll(m, " ", tty_pgrp);                  // 字段 8:  tty_pgrp
    seq_put_decimal_ull(m, " ", task->flags);              // 字段 9:  flags
    seq_put_decimal_ull(m, " ", min_flt);                  // 字段 10: ★ minflt（次页错误）
    seq_put_decimal_ull(m, " ", cmin_flt);                 // 字段 11: cminflt
    seq_put_decimal_ull(m, " ", maj_flt);                  // 字段 12: ★ majflt（主缺页）
    seq_put_decimal_ull(m, " ", cmaj_flt);                 // 字段 13: cmajflt
    seq_put_decimal_ull(m, " ", nsec_to_clock_t(utime));   // 字段 14: ★ utime
    seq_put_decimal_ull(m, " ", nsec_to_clock_t(stime));   // 字段 15: ★ stime
    seq_put_decimal_ll(m, " ", nsec_to_clock_t(cutime));   // 字段 16: cutime
    seq_put_decimal_ll(m, " ", nsec_to_clock_t(cstime));   // 字段 17: cstime
    seq_put_decimal_ll(m, " ", priority);                  // 字段 18: priority
    seq_put_decimal_ll(m, " ", nice);                      // 字段 19: nice
    seq_put_decimal_ll(m, " ", num_threads);               // 字段 20: num_threads
    seq_put_decimal_ull(m, " ", 0);                        // 字段 21: it_real_value（已废弃）
    seq_put_decimal_ull(m, " ", start_time);               // 字段 22: starttime
    seq_put_decimal_ull(m, " ", vsize);                    // 字段 23: vsize（虚拟内存字节数）
    seq_put_decimal_ull(m, " ", mm ? get_mm_rss(mm) : 0);  // 字段 24: rss（常驻集页数）
    // ... 字段 25-38: 内存地址、信号、wchan 等（pidstat 不关心）...
    seq_put_decimal_ll(m, " ", task_cpu(task));             // 字段 39: ★ processor（最后运行的 CPU）
    seq_put_decimal_ull(m, " ", task->rt_priority);        // 字段 40: rt_priority
    seq_put_decimal_ull(m, " ", task->policy);             // 字段 41: policy
    seq_put_decimal_ull(m, " ", delayacct_blkio_ticks(task)); // 字段 42: ★ delayacct（I/O 延迟）
    seq_put_decimal_ull(m, " ", nsec_to_clock_t(gtime));   // 字段 43: ★ guest_time
    // ... 字段 44-52 ...
}
```

> **源码验证：** 字段 14/15 的 utime/stime 在输出前经过 `nsec_to_clock_t()` 转换，
> 证实了"clock_t 单位"的结论。内核内部存储为纳秒（`task->utime` 类型为 `u64`），
> 输出时除以 `NSEC_PER_SEC / USER_HZ` 得到 clock_t。

**内核源码：task_struct 中对应的字段定义**

```c
/* 源码位置：src/linux-5.10/include/linux/sched.h */
struct task_struct {
    /* -1 unrunnable, 0 runnable, >0 stopped: */
    volatile long           state;          // ★ 行 649：进程状态

    // ... 中间省略数百行 ...

    struct mm_struct        *mm;            // ★ 行 757：内存描述符指针
    struct mm_struct        *active_mm;

    u64                     utime;          // ★ 行 884：用户态 CPU 时间（纳秒）
    u64                     stime;          // ★ 行 885：内核态 CPU 时间（纳秒）
    u64                     gtime;          // ★ 行 890：虚拟机 guest 时间（纳秒）
    struct prev_cputime     prev_cputime;   // 行 891：上次快照（用于调整精度）

    /* Context switch counts: */
    unsigned long           nvcsw;          // ★ 行 900：自愿上下文切换次数
    unsigned long           nivcsw;         // ★ 行 901：非自愿上下文切换次数

    /* MM fault and swap info: */
    unsigned long           min_flt;        // ★ 行 910：次缺页（minor fault）累积计数
    unsigned long           maj_flt;        // ★ 行 911：主缺页（major fault）累积计数

    /*
     * executable name, excluding path.
     * - normally initialized setup_new_exec()
     * - access it with [gs]et_task_comm()
     * - lock it with task_lock()
     */
    char                    comm[TASK_COMM_LEN]; // ★ 行 943：进程名（16字节）

    struct task_io_accounting   ioac;       // ★ 行 1067：I/O 统计结构体
};
```

### 2.2 /proc/[pid]/io — I/O 字节数

```bash
$ cat /proc/1/io
rchar: 157209793551           ← 读字符数（含缓存）
wchar: 114101323407           ← 写字符数（含缓存）
syscr: 35795360               ← read 系统调用次数
syscw: 26783505               ← write 系统调用次数
read_bytes: 12862489600       ← ★ 实际从磁盘读取的字节数
write_bytes: 32842510336      ← ★ 实际写入磁盘的字节数
cancelled_write_bytes: 276742144  ← 被截断取消的写入
```

**pidstat 关注的字段：**

| 字段 | pidstat 列 | 说明 |
|------|-----------|------|
| read_bytes | kB_rd/s | 实际从磁盘读（不含 page cache 命中） |
| write_bytes | kB_wr/s | 实际写到磁盘（不含 page cache 缓冲） |
| cancelled_write_bytes | kB_ccwr/s | 文件截断导致的取消写入 |

**kB_rd/s 计算公式：**
```
kB_rd/s = (read_bytes_new - read_bytes_old) / interval / 1024
```

**iodelay 来源：** 来自 `/proc/[pid]/schedstat` 或 `/proc/[pid]/sched` 中的 I/O 等待时间累积。

**内核源码：`do_io_accounting()` 输出逻辑**

```c
/* 源码位置：src/linux-5.10/fs/proc/base.c:2927-2970 */
/* ★ /proc/[pid]/io 文件的核心输出函数 */
static int do_io_accounting(struct task_struct *task, struct seq_file *m, int whole)
{
    struct task_io_accounting acct = task->ioac;  // ★ 拷贝当前线程的 I/O 统计
    unsigned long flags;
    int result;

    result = mutex_lock_killable(&task->signal->exec_update_mutex);
    if (result)
        return result;

    if (!ptrace_may_access(task, PTRACE_MODE_READ_FSCREDS)) {
        result = -EACCES;   // ★ 权限检查：需要 ptrace 权限才能读取
        goto out_unlock;
    }

    if (whole && lock_task_sighand(task, &flags)) {
        struct task_struct *t = task;
        /* ★ whole=1：聚合进程组所有线程的 ioac */
        task_io_accounting_add(&acct, &task->signal->ioac);
        while_each_thread(task, t)
            task_io_accounting_add(&acct, &t->ioac);
        unlock_task_sighand(task, &flags);
    }

    /* ★ 输出 7 个字段 */
    seq_printf(m,
           "rchar: %llu\n"
           "wchar: %llu\n"
           "syscr: %llu\n"
           "syscw: %llu\n"
           "read_bytes: %llu\n"          // ★ 实际从磁盘读取
           "write_bytes: %llu\n"         // ★ 实际写入磁盘
           "cancelled_write_bytes: %llu\n",
           (unsigned long long)acct.rchar,
           (unsigned long long)acct.wchar,
           (unsigned long long)acct.syscr,
           (unsigned long long)acct.syscw,
           (unsigned long long)acct.read_bytes,
           (unsigned long long)acct.write_bytes,
           (unsigned long long)acct.cancelled_write_bytes);
    // ...
}
```

```c
/* 源码位置：src/linux-5.10/include/linux/task_io_accounting.h:12-46 */
/* ★ task_io_accounting 结构体 —— 嵌入在 task_struct.ioac 中 */
struct task_io_accounting {
#ifdef CONFIG_TASK_XACCT
    u64 rchar;                  // 读字符数（含 page cache 命中）
    u64 wchar;                  // 写字符数（含 page cache 缓冲）
    u64 syscr;                  // read() 系统调用次数
    u64 syscw;                  // write() 系统调用次数
#endif
#ifdef CONFIG_TASK_IO_ACCOUNTING
    u64 read_bytes;             // ★ 实际从存储设备读取的字节数
    u64 write_bytes;            // ★ 实际写入存储设备的字节数
    u64 cancelled_write_bytes;  // ★ 因 truncate 等原因取消的写入字节数
#endif
};
```

> **源码验证：** `rchar`/`wchar` 统计的是 read/write **系统调用**的字节数（含 page cache），
> 而 `read_bytes`/`write_bytes` 统计的是**实际到达/离开磁盘**的字节数。
> 这就是为什么 pidstat 的 kB_rd/kB_wr 用的是后者。

### 2.3 /proc/[pid]/status — 内存和上下文切换

```bash
$ grep -E "Vm|ctxt" /proc/1/status
VmPeak:    22130 kB     ← 虚拟内存峰值
VmSize:    21612 kB     ← 当前虚拟内存大小（VSZ）
VmRSS:     12632 kB     ← 常驻集大小（RSS）
VmSwap:        0 kB     ← 交换到 swap 的大小
voluntary_ctxt_switches:    158274   ← ★ 自愿上下文切换累积次数
nonvoluntary_ctxt_switches: 184018   ← ★ 非自愿上下文切换累积次数
```

**pidstat 关注的字段：**

| 字段 | pidstat 列 | 说明 |
|------|-----------|------|
| VmSize | VSZ | 虚拟内存大小（KB） |
| VmRSS | RSS | 物理内存大小（KB） |
| voluntary_ctxt_switches | cswch/s | 自愿切换（I/O 等待、sleep） |
| nonvoluntary_ctxt_switches | nvcswch/s | 非自愿切换（时间片耗尽、抢占） |

### 2.4 /proc/[pid]/schedstat — 调度等待时间

```bash
$ cat /proc/1/schedstat
5983621349 125344604 406038
  ↑         ↑        ↑
  运行时间  等待时间  运行次数
  (ns)      (ns)     (次)
```

**pidstat 的 %wait 来源：** 等待时间（第二个字段）的差值除以采样间隔。

```
%wait = (wait_new - wait_old) / (interval × 10^9) / nr_cpu × 100
```

---

## 三、采样差值机制

### 无间隔模式（默认）

```bash
pidstat -u    # 输出从启动到当前的累积平均值
```

```
进程启动时的 /proc 值
         ↓
当前的 /proc 值
         ↓
差值 / 进程存活时间 = 平均速率
```

### 间隔模式

```bash
pidstat -u 2 5    # 每 2 秒采样一次，共 5 次
```

```
T0: 读取 /proc/[pid]/stat → 记录 utime_0, stime_0
    │
    ↓ 等待 2 秒
    │
T1: 读取 /proc/[pid]/stat → 记录 utime_1, stime_1
    │
    ↓ 计算差值
    │
    %usr = (utime_1 - utime_0) / (2 × CLK_TCK) × 100
    %system = (stime_1 - stime_0) / (2 × CLK_TCK) × 100
    │
    ↓ 输出 T1 行
    │
T2: 读取 → 计算 T2-T1 差值 → 输出 T2 行
...
T5: 输出 Average: 行（所有采样的平均）
```

**重要：只有活跃的进程才会出现在输出中。** 如果某个进程在采样间隔内没有任何 CPU/IO 活动，pidstat 不会显示它（减少输出噪音）。

---

## 四、进程枚举机制

```
pidstat 如何找到所有进程？

1. 打开 /proc/ 目录
2. 遍历所有数字命名的子目录（每个数字 = PID）
3. 对每个 PID，尝试读取 /proc/[pid]/stat
4. 如果读取成功 → 进程存活，收集数据
5. 如果读取失败（ENOENT） → 进程已退出，跳过
6. 如果指定了 -C 过滤 → 检查 comm 是否匹配正则表达式
7. 如果指定了 -p → 只处理指定的 PID
```

**注意：** 短命进程可能被遗漏。如果进程在两次采样之间启动并退出，pidstat 永远不会看到它。这是轮询式监控的固有限制。

---

## 五、pidstat 与内核的关系图

```
┌────────────────────────────────────────────────────────────┐
│                    内核维护的计数器                          │
│                                                            │
│  task_struct {                                             │
│    utime ────────→ /proc/[pid]/stat 字段 14                │
│    stime ────────→ /proc/[pid]/stat 字段 15                │
│    min_flt ──────→ /proc/[pid]/stat 字段 10                │
│    maj_flt ──────→ /proc/[pid]/stat 字段 12                │
│    nvcsw ────────→ /proc/[pid]/status 的 voluntary_ctxt_switches   │
│    nivcsw ───────→ /proc/[pid]/status 的 nonvoluntary_ctxt_switches │
│    io_acct {                                               │
│      rchar ──────→ /proc/[pid]/io 的 rchar                 │
│      wchar ──────→ /proc/[pid]/io 的 wchar                 │
│      read_bytes ─→ /proc/[pid]/io 的 read_bytes            │
│      write_bytes → /proc/[pid]/io 的 write_bytes           │
│    }                                                       │
│  }                                                         │
│                                                            │
│  mm_struct {                                               │
│    total_vm ─────→ /proc/[pid]/status 的 VmSize            │
│    rss ──────────→ /proc/[pid]/status 的 VmRSS             │
│  }                                                         │
└────────────────────────────────────────────────────────────┘
         │
         ▼
    pidstat 读取 + 差值计算 → 输出表格
```

---

## 六、关键限制

| 限制 | 原因 | 影响 |
|------|------|------|
| **短命进程遗漏** | 轮询式采样，两次采样间退出的进程看不到 | 可能漏掉短任务 |
| **精度有限** | 秒级采样，无法捕捉毫秒级突发 | 看不到瞬时峰值 |
| **I/O 只看到磁盘层** | read_bytes/write_bytes 是实际磁盘 I/O | 看不到 page cache 命中 |
| **线程 vs 进程** | 默认只显示主线程（TGID） | 需 `-t` 看所有线程 |
| **权限限制** | 普通用户只能看自己的进程 | root 看全部 |
| **CPU 百分比上限** | %CPU 可以超过 100%（多线程进程） | 4 核机器上最大 400% |

**思考题：**
1. 为什么 pidstat 的 %CPU 有时候与 top 的 %CPU 不一致？
   （答：top 的 %CPU 是两次刷新间的差值，刷新间隔可能不是整秒；pidstat 基于精确的 interval 参数）
2. 为什么有些进程的 kB_wr/s 为 0 但确实在写文件？
   （答：写入了 page cache，还没刷到磁盘。write_bytes 只计算实际磁盘 I/O）
3. %wait 高但 %CPU 低意味着什么？
   （答：进程大部分时间在等待 CPU 调度——系统 CPU 过载）
