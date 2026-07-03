# ps 命令学习路线

> 从 /proc 文件系统到进程状态机，从 STAT 字段到性能诊断

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 数据来源 — ps 从哪里获取信息](#2-数据来源--ps-从哪里获取信息)
- [3. 进程状态机 — R/S/D/Z/T 的转换](#3-进程状态机--rsdzt-的转换)
- [4. 学习路线图](#4-学习路线图)
  - [Step 1: 进程状态详解](#step-1-进程状态详解)
  - [Step 2: 命令参考](#step-2-命令参考)
  - [Step 3: 输出实战](#step-3-输出实战)
- [5. 与 top/htop/pidstat 的区别和互补](#5-与-tophtoppidstat-的区别和互补)
- [6. 动手实验清单](#6-动手实验清单)
- [7. 关键数据源索引](#7-关键数据源索引)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                      用户态工具                               │
│  ps(快照)  ←→  top(实时)  ←→  pidstat(采样统计)               │
├─────────────────────────────────────────────────────────────┤
│                      接口层                                   │
│  /proc/[pid]/stat  /proc/[pid]/status  /proc/[pid]/cmdline  │
│  /proc/[pid]/task/  /proc/[pid]/wchan  /proc/[pid]/sched    │
├─────────────────────────────────────────────────────────────┤
│                      内核进程管理                              │
│  task_struct → 调度器(CFS/RT/DL) → 进程状态机 → 信号处理     │
└─────────────────────────────────────────────────────────────┘
```

### ps 的数据流全路径

```
用户执行 ps
    │
    ▼
ps 遍历 /proc/ 目录
    │
    ├── 读取 /proc/[pid]/stat     → PID、状态、CPU 时间、内存等
    ├── 读取 /proc/[pid]/status   → 进程名、UID、内存详情
    ├── 读取 /proc/[pid]/cmdline  → 完整命令行（\0 分隔）
    ├── 读取 /proc/[pid]/task/    → 线程列表（每个子目录是一个 LWP）
    ├── 读取 /proc/[pid]/wchan    → 内核等待函数名
    ├── 读取 /proc/[pid]/statm    → 内存页统计
    └── 读取 /proc/[pid]/sched    → 调度统计（可选）
    │
    ▼
ps 计算 %CPU（两次采样的 CPU 时间差 / 时间间隔）
ps 计算 %MEM（RSS / 物理内存总量 × 100）
    │
    ▼
格式化输出（BSD/System V/自定义格式）
```

### ps vs /proc 的关系

```
ps 输出字段              /proc 数据来源
───────────────────────  ─────────────────────────────────
PID                      /proc/[pid]/stat 字段 1
STAT                     /proc/[pid]/stat 字段 3（state 字符）
%CPU                     /proc/[pid]/stat 字段 14+15（utime+stime）
                         ★ ps 做两次采样计算差值
%MEM                     /proc/[pid]/statm 字段 2（resident pages）
VSZ                      /proc/[pid]/stat 字段 23（vsize，字节）
RSS                      /proc/[pid]/stat 字段 24（rss，页数）
WCHAN                    /proc/[pid]/wchan
CMD                      /proc/[pid]/cmdline（完整）或 /proc/[pid]/stat 字段 2（短名）
NLWP                     /proc/[pid]/status → Threads 行
USER                     /proc/[pid]/status → Uid 行 → 查 /etc/passwd
```

---

## 2. 数据来源 — ps 从哪里获取信息

### 2.1 /proc/[pid]/stat — 核心数据源

这是 ps 最重要的数据源，包含 52 个字段（Linux 5.10）。

```
$ cat /proc/1/stat
1 (systemd) S 0 1 1 0 -1 4194560 131842 96194324 ...
```

**字段速查表：**

| 序号 | 字段 | 含义 | ps 对应列 |
|------|------|------|-----------|
| 1 | pid | 进程 ID | PID |
| 2 | comm | 进程名（括号内） | CMD（短名） |
| 3 | state | 进程状态字符 | STAT（首字符） |
| 4 | ppid | 父进程 ID | PPID |
| 5 | pgrp | 进程组 ID | PGID |
| 6 | session | 会话 ID | SID |
| 7 | tty_nr | 控制终端号 | TTY |
| 8 | tpgid | 前台进程组 ID | — |
| 9 | flags | 进程标志 | — |
| 10 | minflt | 次缺页次数（无需磁盘 I/O） | — |
| 13 | utime | 用户态 CPU 时间（jiffies） | TIME（的一部分） |
| 14 | stime | 内核态 CPU 时间（jiffies） | TIME（的一部分） |
| 18 | priority | 优先级 | PRI |
| 19 | nice | nice 值 | NI |
| 20 | num_threads | 线程数 | NLWP |
| 22 | starttime | 启动时间（jiffies） | START |
| 23 | vsize | 虚拟内存大小（字节） | VSZ |
| 24 | rss | 常驻内存大小（页数） | RSS |
| 39 | processor | 最近使用的 CPU | PSR |
| 41 | policy | 调度策略 | CLS |

★ **完整字段说明见 `reading/01_process_states.md`**

### 2.2 /proc/[pid]/status — 人类可读版本

```
$ cat /proc/1/status
Name:   systemd          ← 进程名
Umask:  0000
State:  S (sleeping)     ← ★ 状态 + 人类可读描述
Tgid:   1                ← 线程组 ID = 主线程 PID
Pid:    1
PPid:   0
Uid:    0  0  0  0       ← real, effective, saved, fs
Gid:    0  0  0  0       ← real, effective, saved, fs
Threads: 1               ← 线程数
VmPeak:    30080 kB      ← 虚拟内存峰值
VmSize:    21612 kB      ← 虚拟内存当前 = VSZ
VmRSS:     12632 kB      ← 常驻内存 = RSS
VmSwap:        0 kB      ← swap 使用量
```

### 2.3 /proc/[pid]/cmdline — 完整命令行

```
$ cat /proc/1/cmdline | tr '\0' ' '
/usr/lib/systemd/systemd --switched-root --system --deserialize=43 rhgb ...
```

★ **注意：** cmdline 中的参数用 `\0`（空字符）分隔，不是空格。如果进程修改了自己的命令行（如 nginx worker），看到的可能是修改后的版本。

### 2.4 /proc/[pid]/task/ — 线程信息

```
$ ls /proc/1/task/
1
```

每个子目录名是一个线程 ID（LWP）。ps -eLf 会遍历每个进程的 task/ 目录。

### 2.5 /proc/[pid]/wchan — 等待位置

```
$ cat /proc/1/wchan
ep_poll
```

★ 显示进程在内核中等待的函数名。如果进程正在运行，显示 `-`。

---

## 3. 进程状态机 — R/S/D/Z/T 的转换

### 状态转换全景图

```
                    fork()
                       │
                       ▼
              ┌──────────────┐
              │   新建(N)     │
              │ TASK_RUNNING  │ ← 虽然叫 RUNNING 但包含"就绪"
              └──────┬───────┘
                     │ 被调度器选中
                     ▼
              ┌──────────────┐
         ┌───→│  运行中(R)    │←──┐
         │    │ TASK_RUNNING  │   │ 被抢占 / 时间片用完
         │    └──────┬───────┘   │
         │           │           │
         │    ┌──────┴──────┐    │
         │    │             │    │
         │    ▼             ▼    │
   被唤醒  ┌────────┐  ┌────────┐│
         ││ 可中断   │  │ 不可中断││
         ││ 睡眠(S)  │  │ 睡眠(D)││
         ││TASK_INT  │  │TASK_UN ││
         │└────┬───┘  └──┬─────┘│
         │     │         │      │
         │  等待事件    等待I/O   │
         │  (信号可唤醒) (信号不可唤醒)
         │     │         │      │
         │     └────┬────┘      │
         │          │ 事件发生   │
         │          └───────────┘
         │
         │  收到 SIGSTOP/SIGTSTP
         ▼
  ┌──────────────┐
  │  停止(T)      │
  │ TASK_STOPPED  │ ← 调试断点、Ctrl+Z
  └──────┬───────┘
         │ 收到 SIGCONT
         └──────→ 回到 R

         进程退出
         │
         ▼
  ┌──────────────┐
  │  僵尸(Z)      │
  │ TASK_ZOMBIE   │ ← 进程已死，但父进程还没 wait()
  │               │   ★ 无法被 kill，只能等父进程回收
  └──────┬───────┘
         │ 父进程调用 wait()
         ▼
       彻底消亡
```

### 各状态的要点

```
状态  │ 字母 │ 全称                    │ 可被 kill │ 常见场景
──────┼──────┼─────────────────────────┼───────────┼─────────────────────
运行  │ R    │ Running / Runnable      │ ✅ 可杀   │ 正在 CPU 上执行或等待调度
睡眠  │ S    │ Sleeping (interruptible) │ ✅ 可杀   │ 等待事件/信号（最常见）
深睡  │ D    │ Disk sleep (uninterrupt.)│ ❌ 不可杀 │ 等待 I/O（NFS 卡死等）
僵尸  │ Z    │ Zombie                   │ ❌ 不可杀 │ 已退出但父进程未回收
停止  │ T    │ Stopped                  │ ✅ 可杀   │ SIGSTOP / 调试器暂停
追踪  │ t    │ Tracing stop             │ ✅ 可杀   │ ptrace 调试中
死亡  │ X    │ Dead                     │ —        │ 瞬态，几乎看不到
空闲  │ I    │ Idle kernel thread       │ ❌ 不可杀 │ 内核线程空闲
```

★ **D 状态为什么杀不掉？** 详见 `reading/01_process_states.md` 第四节。

---

## 4. 学习路线图

### Step 1: 进程状态详解

**目标：** 理解每种进程状态的内核实现、状态转换条件、/proc/[pid]/stat 的 40+ 字段

**文件位置：** `reading/01_process_states.md`

**阅读要点：**
1. 7 种进程状态的内核常量和转换条件
2. /proc/[pid]/stat 每个字段的含义和计算方式
3. D 状态的排查方法（为什么 kill -9 杀不掉）
4. STAT 字段的多字符编码（如 Ss、S+、Ss+、R+、I< 等）

**思考题：**
1. 为什么内核把"就绪"和"运行中"都用 TASK_RUNNING 表示？
2. 僵尸进程的 PID 能被复用吗？在什么条件下？
3. 一个进程 STAT=Ss+ 中的三个字符分别代表什么？

---

### Step 2: 命令参考

**目标：** 掌握 ps 的参数分类（选择/格式/排序）和常用字段

**文件位置：** `reading/02_command_reference.md`

**阅读要点：**
1. 选择参数：-e/-a/-u/-p/-C/-G 如何过滤进程
2. 格式参数：-o/-f/-F/-l/-j 如何控制输出列
3. 格式字段：PID/PPID/USER/STAT/%CPU/%MEM/VSZ/RSS 等的含义
4. 排序：--sort=+/- 字段名
5. 10+ 个场景命令

**思考题：**
1. ps aux 和 ps -ef 的区别是什么？（答：BSD vs System V 格式，列排列不同）
2. %CPU 的值为什么可能超过 100%？（答：多核累计）
3. VSZ 和 RSS 有什么区别？（答：虚拟 vs 物理内存）

---

### Step 3: 输出实战

**目标：** 实际执行命令，理解每一列的含义，能解读 STAT 字段

**文件位置：** `reading/03_command_output_demo.md`

**阅读要点：**
1. ps aux 逐列注解
2. ps -ef System V 格式逐列注解
3. 自定义格式和排序
4. 线程列表（-eLf）
5. STAT 字段每个字符的含义注解

---

## 5. 与 top/htop/pidstat 的区别和互补

### 工具对比总览

```
工具      │ 类型     │ 数据源           │ 刷新  │ 适用场景
──────────┼──────────┼──────────────────┼───────┼──────────────────────
ps        │ 快照     │ /proc/[pid]/*    │ 一次性 │ 进程列表、状态查询、过滤
top       │ 实时     │ /proc/[pid]/*    │ 持续   │ 实时监控、资源排行
htop      │ 实时     │ /proc/[pid]/*    │ 持续   │ 交互式监控、树形视图
pidstat   │ 采样统计 │ /proc/[pid]/stat │ 定期   │ CPU/内存/IO 采样报告
```

### 各工具的独有能力

| 工具 | 独有能力 | 典型用法 |
|------|---------|---------|
| **ps** | 灵活的过滤和格式化、一次性快照、脚本友好 | `ps -C nginx -f`、`ps -eo pid,stat,cmd --sort=-%cpu` |
| **top** | 实时刷新、全局概览、load average | 日常监控、快速定位 CPU/内存消耗者 |
| **htop** | 交互式界面、树形视图、F1-F10 快捷键 | 交互式排查、进程树可视化 |
| **pidstat** | 定期采样、per-thread、I/O 统计 | `pidstat -u 1 10`（每秒采样 CPU，共 10 次） |

### 互补使用场景

```
场景：服务器 CPU 飙高

第一步：top                      → 看全局：哪个进程 CPU 最高？
第二步：ps -p <PID> -o pid,stat,%cpu,%mem,wchan,cmd  → 看详情：什么状态？等在哪？
第三步：ps -eLf -p <PID>         → 看线程：哪个线程在消耗 CPU？
第四步：pidstat -t -p <PID> 1    → 看趋势：每秒的 CPU 变化
```

```
场景：进程 D 状态排查

第一步：ps -eo pid,stat,wchan,cmd | grep " D "  → 找出所有 D 状态进程
第二步：cat /proc/<PID>/wchan                    → 看卡在内核哪个函数
第三步：cat /proc/<PID>/stack                    → 看内核调用栈
第四步：dmesg | tail                             → 看是否有 I/O 错误
```

### 数据精度对比

```
       ps                    top                    pidstat
       │                     │                      │
       ▼                     ▼                      ▼
  读取 /proc 一次        每 3 秒刷新一次         用户指定采样间隔
  %CPU =                 %CPU =                   %CPU =
  (Δcpu_time) /          同 ps，但刷新间隔内       精确的采样间隔内
  (Δwall_time)           的平均值                  平均值

  ★ 局限：               ★ 优势：                 ★ 优势：
  一次快照看不到趋势      实时看到变化趋势           精确的定时采样
  %CPU 可能不准          CPU 高的进程更明显          适合生成报告
```

---

## 6. 动手实验清单

### 实验 1：基础进程列表

```bash
#!/bin/bash
# 查看所有进程，理解每一列
ps aux | head -30

# System V 格式
ps -ef | head -30

# 自定义格式 — 关注进程状态和等待位置
ps -eo pid,ppid,user,stat,wchan,cmd | head -30
```

### 实验 2：进程状态观察

```bash
#!/bin/bash
# 创建一个 D 状态进程（通过不可中断的 I/O）
# 方法：使用 dd 写入一个 NFS 挂载点（如果可用）

# 创建一个 S 状态进程
sleep 3600 &
SLEEP_PID=$!
ps -p $SLEEP_PID -o pid,stat,wchan,cmd
# 预期 STAT=S+，WCHAN=hrtimer_nanostream 或类似

# 创建一个 T 状态进程
kill -STOP $SLEEP_PID
ps -p $SLEEP_PID -o pid,stat,cmd
# 预期 STAT=T+
kill -CONT $SLEEP_PID
ps -p $SLEEP_PID -o pid,stat,cmd
# 预期 STAT=S+

# 创建一个 Z 状态进程
bash -c '(sleep 0.1 &); sleep 2' &
sleep 0.5
ps -eo pid,ppid,stat,cmd | grep defunct
# 预期看到 Z+ 状态的 defunct 进程

# 清理
kill $SLEEP_PID 2>/dev/null
```

### 实验 3：CPU 和内存排序

```bash
#!/bin/bash
# CPU 占用最高的 10 个进程
ps aux --sort=-%cpu | head -11

# 内存占用最高的 10 个进程
ps aux --sort=-%mem | head -11

# VSZ vs RSS 对比
ps -eo pid,vsz,rss,cmd --sort=-rss | head -15
```

### 实验 4：线程级分析

```bash
#!/bin/bash
# 查看所有线程
ps -eLf | head -30

# 特定进程的线程详情
ps -eLf -p $(pgrep -f "claude" | head -1)

# 统计各进程的线程数
ps -eo pid,nlwp,cmd --sort=-nlwp | head -15
```

### 实验 5：进程树与父子关系

```bash
#!/bin/bash
# 显示进程树
ps -ejH | head -40

# 或用 forest 格式
ps -eo pid,ppid,cmd --forest | head -40

# 查找特定命令名的所有进程
ps -C sshd -f
```

---

## 7. 关键数据源索引

### /proc 文件系统

| 路径 | 内容 | ps 对应字段 |
|------|------|------------|
| `/proc/[pid]/stat` | 进程状态（52 字段） | PID, STAT, PPID, %CPU, VSZ, RSS, NI, PRI, NLWP, START, TIME |
| `/proc/[pid]/status` | 人类可读状态 | Name, UID, Threads, VmSize, VmRSS |
| `/proc/[pid]/cmdline` | 完整命令行（\0 分隔） | CMD |
| `/proc/[pid]/statm` | 内存页统计 | VSZ, RSS |
| `/proc/[pid]/wchan` | 内核等待函数名 | WCHAN |
| `/proc/[pid]/task/` | 线程目录 | LWP, NLWP |
| `/proc/[pid]/sched` | 调度统计 | PSR, CLS |
| `/proc/[pid]/io` | I/O 统计 | — |
| `/proc/[pid]/stack` | 内核调用栈 | — |
| `/proc/[pid]/fd/` | 打开的文件描述符 | — |

### 内核源码（src/linux-5.10/）

| 文件 | 关注点 |
|------|--------|
| `include/linux/sched.h` | `task_struct` 定义（进程控制块） |
| `kernel/sched/core.c` | 调度器核心、状态转换 |
| `kernel/sched/fair.c` | CFS 调度器（SCHED_NORMAL） |
| `kernel/sched/rt.c` | 实时调度器（SCHED_FIFO/SCHED_RR） |
| `kernel/sched/deadline.c` | 截止期调度器（SCHED_DEADLINE） |
| `kernel/exit.c` | 进程退出、僵尸状态 |
| `kernel/signal.c` | 信号处理、状态转换（D→Z 的条件） |
| `fs/proc/array.c` | /proc/[pid]/stat 的生成逻辑 |
| `fs/proc/base.c` | /proc/[pid]/ 目录的实现 |

### 推荐阅读顺序

```
1. 01_process_states.md   → 理解进程状态机和 /proc 数据源
2. 02_command_reference.md → 掌握 ps 的参数和格式字段
3. 03_command_output_demo.md → 实际执行命令，理解每列含义
```

---

## 参考资源

- [ps(1) man page](https://man7.org/linux/man-pages/man1/ps.1.html)
- [proc(5) man page](https://man7.org/linux/man-pages/man5/proc.5.html)
- [Linux 内核 sched.h](https://github.com/torvalds/linux/blob/master/include/linux/sched.h)
- [Understanding the Linux Kernel — Chapter 3: Processes](https://www.oreilly.com/library/view/understanding-the-linux/0596005652/)
- [Linux Performance — Process States](https://www.brendangregg.com/linuxperf.html)
- [IBM: Linux process states](https://developer.ibm.com/articles/au-linux-process-states/)
