# ps 与 /proc 数据源 — 每列数据的来龙去脉

> ======================================================================
> 本文件深入分析 ps 各输出列与 /proc 文件字段的精确映射关系。
> 01 文件已讲解进程状态和 /proc/[pid]/stat 字段，本文件聚焦于：
>
> 阅读要点：
>   1. ps 从 /proc 读取哪些文件、如何使用
>   2. /proc/[pid]/stat vs /proc/[pid]/status 的区别
>   3. ps 格式字段与 /proc 字段的完整映射表
>   4. WCHAN 的含义 — 进程在内核中睡眠的函数名
>   5. 实际 cat /proc/1/stat 并与 ps 输出逐项对照
>
> 预计阅读时间：15 分钟
> ======================================================================

---

## 目录

- [一、ps 读取的 /proc 文件清单](#一ps-读取的-proc-文件清单)
- [二、/proc/[pid]/stat vs /proc/[pid]/status 对比](#二procpidstat-vs-procpidstatus-对比)
- [三、ps 格式字段与 /proc 字段完整映射表](#三ps-格式字段与-proc-字段完整映射表)
- [四、WCHAN 深度解析 — 进程在内核中等待什么](#四wchan-深度解析--进程在内核中等待什么)
- [五、实测对照：cat /proc/1/stat vs ps 输出](#五实测对照cat-proc1stat-vs-ps-输出)
- [六、特殊字段解析](#六特殊字段解析)
- [七、ps 的两种输出格式对比](#七ps-的两种输出格式对比)

---

## 一、ps 读取的 /proc 文件清单

### ★ ps 的数据来源

ps 从以下 /proc 文件中读取数据，每个文件提供不同的信息维度：

```
文件                       │ ps 使用的场景          │ 提供的主要数据
──────────────────────────┼──────────────────────┼───────────────────────────
/proc/[pid]/stat          │ ★ 几乎所有字段        │ PID、状态、CPU 时间、内存
                           │                       │ 优先级、线程数、启动时间
/proc/[pid]/status        │ USER、内存补充        │ UID→用户名、VmSize、VmRSS
/proc/[pid]/cmdline       │ CMD（完整命令行）      │ exec() 时的参数列表
/proc/[pid]/wchan         │ WCHAN 列              │ 进程睡眠的内核函数名
/proc/[pid]/stack         │ —（ps 不直接使用）     │ 内核调用栈（需 root）
/proc/[pid]/schedstat     │ —（ps 不直接使用）     │ 调度等待时间
/proc/[pid]/exe           │ —（readlink 可用）     │ 可执行文件的完整路径
/proc/[pid]/limits        │ —                     │ 资源限制
/proc/[pid]/fd/           │ —（ls -la 可用）       │ 打开的文件描述符
```

### ★ ps 读取流程

```
┌──────────────────────────────────────────────────────────────────┐
│                    ps 的数据读取流程                                │
│                                                                    │
│  1. 打开 /proc/ 目录                                              │
│  2. 遍历所有数字子目录（每个数字 = PID）                           │
│  3. 对每个 PID：                                                   │
│     ├── 读取 /proc/[pid]/stat → 获取 52 个字段                   │
│     │   ★ 这是主要数据源，一个文件提供 90% 的输出列              │
│     │                                                             │
│     ├── 读取 /proc/[pid]/status → 获取 UID、补充内存             │
│     │   → UID 查 /etc/passwd 得到用户名                          │
│     │                                                             │
│     ├── 读取 /proc/[pid]/cmdline → 获取完整命令行                │
│     │   → 用 NUL 分隔的字符串                                    │
│     │                                                             │
│     └── 读取 /proc/[pid]/wchan → 获取 WCHAN 字符串               │
│         → 进程睡眠时等待的内核函数名                              │
│                                                                    │
│  4. 根据 -o 选项选择要显示的列                                    │
│  5. 格式化输出表格                                                │
│                                                                    │
│  ★ 版本信息：                                                      │
│  $ ps --version                                                    │
│  ps from procps-ng 4.0.4                                          │
└──────────────────────────────────────────────────────────────────┘
```

---

## 二、/proc/[pid]/stat vs /proc/[pid]/status 对比

### ★ 核心差异

```
维度          │ /proc/[pid]/stat              │ /proc/[pid]/status
─────────────┼──────────────────────────────┼──────────────────────────────
格式          │ 单行，空格分隔               │ 多行，key: value 格式
可读性        │ 差（需按位置编号解析）       │ ★ 好（字段名直接可见）
字段数        │ 52 个                        │ ~50 个（不同内核版本不同）
CPU 时间      │ ★ 有（utime/stime）          │ ✗ 无
内存信息      │ vsize(字节) + rss(页数)      │ ★ VmSize(KB) + VmRSS(KB) 等
上下文切换    │ ✗ 无                         │ ★ 有（voluntary/nonvoluntary）
UID/GID       │ ✗ 无                         │ ★ 有（real/eff/saved/fs）
信号信息      │ 已废弃（字段31-34=0）        │ ★ 有（SigPnd/SigBlk/SigCgt）
进程名        │ 短名（16 字节限制）          │ 短名（同样 16 字节限制）
```

### ★ 实际对照

```bash
$ cat /proc/1/stat
1 (systemd) S 0 1 1 0 -1 4194560 136191 100801645 100 164051 \
  998 1154 967617 237500 20 0 1 0 16 22130688 3092 ...
```

```bash
$ cat /proc/1/status
Name:	systemd
State:	S (sleeping)
Tgid:	1
Pid:	1
PPid:	0
Uid:	0	0	0	0
VmPeak:	   30080 kB
VmSize:	   21612 kB
VmRSS:	   12368 kB
Threads:	1
voluntary_ctxt_switches:	25709
nonvoluntary_ctxt_switches:	5595
```

### ★ 两者都有的字段对比

```
信息        │ stat 字段                │ status 字段           │ 一致性
───────────┼─────────────────────────┼──────────────────────┼──────────
进程名      │ 字段2: (systemd)        │ Name: systemd        │ ✓ 一致
状态        │ 字段3: S                │ State: S (sleeping)  │ ✓ 一致（status 多了描述）
PID         │ 字段1: 1                │ Pid: 1               │ ✓ 一致
PPID        │ 字段4: 0                │ PPid: 0              │ ✓ 一致
线程数      │ 字段20: 1               │ Threads: 1           │ ✓ 一致
虚拟内存    │ 字段23: 22130688(字节)  │ VmSize: 21612 kB    │ ✓ 22130688/1024=21612
RSS         │ 字段24: 3092(页)        │ VmRSS: 12368 kB     │ ✓ 3092×4=12368

★ 结论：两者的内存数据完全一致，只是单位不同
  stat 用原始单位（字节/页数），status 统一用 KB
```

### ★ 只在 stat 中有的字段

```
字段             │ ps 用途          │ 为什么 status 没有
────────────────┼─────────────────┼──────────────────────────
utime/stime     │ %CPU, TIME      │ status 不暴露 CPU 时间
minflt/majflt   │ —               │ status 不暴露页错误
priority/nice   │ PRI, NI         │ status 不暴露优先级
num_threads     │ NLWP            │ status 有 Threads（等价）
starttime       │ START           │ status 不暴露启动时间
processor       │ PSR             │ status 不暴露 CPU 号
policy          │ CLS             │ status 不暴露调度策略
wchan(地址)     │ WCHAN           │ status 无（但有 /proc/[pid]/wchan）
```

### ★ 只在 status 中有的字段

```
字段                       │ ps 用途          │ 为什么 stat 没有
──────────────────────────┼─────────────────┼──────────────────────────
Uid (4 列)                │ USER            │ stat 不暴露 UID
VmPeak/VmSwap             │ —               │ stat 只有当前 VmSize/VmRSS
voluntary_ctxt_switches   │ —               │ stat 设计较早，不含切换统计
SigPnd/SigBlk/SigCgt      │ —               │ stat 的信号字段已废弃
CapInh/CapPrm/CapEff      │ —               │ 权限信息，stat 不暴露
```

---

## 三、ps 格式字段与 /proc 字段完整映射表

### ★ 完整映射表

```bash
# 查看 ps 支持的所有格式字段
$ ps -L    # 或 man ps 查看 STANDARD FORMAT SPECIFIERS
```

```
ps 字段   │ 显示名    │ /proc 来源          │ 字段位置       │ 计算方式
─────────┼──────────┼────────────────────┼───────────────┼──────────────────
pid      │ PID      │ stat               │ 字段 1         │ 直接读取
ppid     │ PPID     │ stat               │ 字段 4         │ 直接读取
pgrp     │ PGID     │ stat               │ 字段 5         │ 直接读取
sid      │ SID      │ stat               │ 字段 6         │ 直接读取
user     │ USER     │ status             │ Uid → passwd   │ UID 查 /etc/passwd
uid      │ UID      │ status             │ Uid 第一列     │ 直接读取
stat     │ STAT     │ stat               │ 字段 3 + 修饰  │ 状态 + flags 推导
%cpu     │ %CPU     │ stat               │ 字段 14+15     │ (utime+stime)/存活时间
%mem     │ %MEM     │ status             │ VmRSS          │ RSS/物理内存 × 100
vsz      │ VSZ      │ stat/status        │ 字段23/VmSize  │ vsize/1024 或 VmSize
rss      │ RSS      │ stat/status        │ 字段24/VmRSS   │ rss×4 或 VmRSS
ni       │ NI       │ stat               │ 字段 19        │ nice 值
pri      │ PRI      │ stat               │ 字段 18        │ priority（内核值）
nlwp     │ NLWP     │ stat/status        │ 字段20/Threads │ 线程数
tty      │ TTY      │ stat               │ 字段 7         │ tty_nr 解码
time     │ TIME     │ stat               │ 字段 14+15     │ (utime+stime)/HZ
start    │ START    │ stat               │ 字段 22        │ starttime 转日期
wchan    │ WCHAN    │ wchan              │ —              │ 函数名（截断到 6 字符）
comm     │ COMMAND  │ stat               │ 字段 2         │ 短名（去括号）
cmd/args │ CMD      │ cmdline            │ —              │ 完整命令行
psr      │ PSR      │ stat               │ 字段 39        │ 最近运行的 CPU
cls      │ CLS      │ stat               │ 字段 41        │ 调度策略编号
lwp      │ LWP      │ —                  │ —              │ 线程 ID
etime    │ ELAPSED  │ stat               │ 字段 22        │ 当前时间 - starttime
```

### ★ STAT 字段的编码规则

```
ps 的 STAT 列 = 状态字母 + 修饰字符

状态字母来源：/proc/[pid]/stat 字段 3
  R/S/D/Z/T/t/I/X → 直接读取

修饰字符来源：/proc/[pid]/stat 的多个字段推导
  s → session leader（字段6 session == 字段1 pid）
  l → multi-threaded（字段20 num_threads > 1）
  < → high priority（字段19 nice < 0）
  N → low priority（字段19 nice > 0）
  + → foreground（字段8 tpgid == 字段5 pgrp）
  L → locked pages（mlock 标记）
```

### ★ 实例解码

```bash
$ ps -p 1 -o pid,stat,comm --no-headers
      1 Ss   systemd
```

```
STAT = "Ss" 解码：
  S → /proc/1/stat 字段3 = 'S'（可中断睡眠）
  s → /proc/1/stat 字段6(session) == 字段1(pid) == 1
      → systemd 是会话 leader（init 进程天然是）
```

---

## 四、WCHAN 深度解析 — 进程在内核中等待什么

### ★ WCHAN 是什么

```
WCHAN = Wait Channel（等待通道）

含义：进程当前正在内核中等待的函数名。
      当进程进入睡眠（S 或 D 状态），内核记录它在哪个函数中等待。

★ WCHAN 告诉你：
  "这个进程卡住了——它卡在内核的哪个位置？"
```

### ★ 实际读取

```bash
$ cat /proc/1/wchan
ep_poll
```

```bash
$ ps -p 1 -o pid,stat,wchan,comm --no-headers
      1 Ss   ep_pol systemd
```

**★ 注意**：ps 的 WCHAN 列将函数名截断到 6 个字符！
- `ep_poll` → 显示为 `ep_pol`（截掉最后一个字母）
- 完整函数名需要读 `/proc/[pid]/wchan`

### ★ WCHAN 常见值及含义

```
WCHAN 值          │ 含义                              │ 典型进程
─────────────────┼──────────────────────────────────┼──────────────────
ep_poll           │ ★ epoll_wait() 等待事件          │ systemd, nginx
poll_schedule     │ poll()/select() 等待              │ 等待输入的进程
do_wait           │ wait() 等待子进程                 │ shell (bash)
kthreadd          │ 内核线程管理器空闲等待            │ kthreadd (PID 2)
worker_thread     │ kworker 等待工作                  │ kworker
smpboot           │ 内核线程启动等待                  │ migration, cpuhp
rcu_gp            │ RCU grace period 等待             │ rcu_preempt
io_schedule       │ ★ 等待 I/O 完成                  │ D 状态进程
futex_wait        │ futex 锁等待                      │ 多线程同步
pipe_wait         │ 管道等待                          │ 管道读写
nfs_wait_bit      │ NFS 等待响应                      │ NFS 客户端
hrtimer_nanny     │ 高精度定时器等待                  │ sleep/nanosleep
do_nanosleep      │ nanosleep 等待                    │ sleep 命令
sk_wait_data      │ socket 等待数据到达               │ 等待 recv 的进程
```

### ★ 实际查看所有睡眠进程的 WCHAN

```bash
$ ps -eo pid,stat,wchan,comm | grep -E ' S| I' | head -15
```

```
    PID STAT WCHAN  COMMAND
      1 Ss   ep_pol systemd         ← epoll_wait 等待事件
      2 S    kthrea kthreadd        ← 内核线程管理器
      3 S    kthrea pool_workqueue_  ← 工作队列
      4 I<   rescue kworker/R-rcu_g  ← RCU 高优先级工作线程
     16 S    smpboo ksoftirqd/0     ← 软中断处理线程
     18 S    smpboo migration/0     ← CPU 迁移线程
     19 S    smpboo idle_inject/0   ← 空闲注入线程
```

**逐行解读：**

```
PID 1 (systemd) → WCHAN = ep_poll
  → systemd 在 epoll_wait() 中等待
  → 等待子进程事件（SIGCHLD）或其他 fd 事件
  → ★ 这是事件驱动型守护进程的标准等待方式

PID 2 (kthreadd) → WCHAN = kthreadd
  → 内核线程管理器在等待新的线程创建请求
  → ★ 所有内核线程都是 kthreadd 的子进程

PID 16 (ksoftirqd/0) → WCHAN = smpboot
  → 软中断线程在 CPU 启动框架中等待
  → 当有大量网络/磁盘中断时被唤醒

kworker 线程 → WCHAN = rescue 或 worker
  → 工作线程在等待新的工作任务
  → "rescue" = worker_pool 的救援线程
```

### ★ WCHAN 与 /proc/[pid]/stack 的关系

```bash
$ cat /proc/1/stack
[<0>] ep_poll+0x334/0x380
[<0>] do_epoll_wait+0xb8/0xe0
[<0>] __x64_sys_epoll_wait+0x5d/0xf0
[<0>] do_syscall_64+0x34/0x80
[<0>] entry_SYSCALL_64_after_hwframe+0x78/0xe2
```

```
/proc/[pid]/wchan 只告诉你"在哪个函数等"（ep_poll）
/proc/[pid]/stack 告诉你完整的调用链：

  entry_SYSCALL_64_after_hwframe    ← 系统调用入口
    → do_syscall_64                 ← 64 位系统调用分发
      → __x64_sys_epoll_wait       ← epoll_wait 系统调用
        → do_epoll_wait            ← epoll 实现
          → ep_poll                ← ★ 实际等待位置

★ WCHAN = 调用栈的最内层函数（进程实际阻塞的位置）
```

### ★ WCHAN 在诊断 D 状态时的关键作用

```bash
# 找出所有 D 状态进程及其等待位置
$ ps -eo pid,stat,wchan,cmd | awk '$2 ~ /^D/'
```

```
D 状态 WCHAN 常见值：
  io_schedule    → 等待磁盘 I/O（最常见）
  nfs_wait_bit   → 等待 NFS 服务器响应
  futex_wait     → 等待内核锁
  get_request    → 等待 block layer 分配 request

★ WCHAN + D 状态 = 精确定位进程卡在哪一步
  是排查 "kill -9 杀不掉的进程" 的第一步
```

---

## 五、实测对照：cat /proc/1/stat vs ps 输出

### 第一步：读取 /proc/1/stat 并编号

```bash
$ cat /proc/1/stat | tr ' ' '\n' | nl
```

```
     1	1                    ← PID
     2	(systemd)            ← 进程名
     3	S                    ← 状态
     4	0                    ← PPID
     5	1                    ← 进程组 ID
     6	1                    ← 会话 ID
     7	0                    ← 终端号
     8	-1                   ← 前台进程组
     9	4194560              ← flags
    10	136191               ← 次缺页
    11	100801645            ← 子进程次缺页
    12	100                  ← 主缺页
    13	164051               ← 子进程主缺页
    14	998                  ← ★ utime (clock_t)
    15	1154                 ← ★ stime (clock_t)
    16	967617               ← 子进程 utime
    17	237500               ← 子进程 stime
    18	20                   ← priority
    19	0                    ← nice
    20	1                    ← 线程数
    21	0                    ← (废弃)
    22	16                   ← 启动时间(jiffies)
    23	22130688             ← vsize(字节)
    24	3092                 ← rss(页数)
    25~52 ...               ← 其余字段
```

### 第二步：运行 ps 获取相同进程

```bash
$ ps -p 1 -o pid,ppid,user,stat,%cpu,%mem,vsz,rss,tty,wchan,comm --no-headers
      1       0 root     Ss    0.0  0.1  21612 12368 ?        ep_pol systemd
```

### ★ 逐项对照

```
ps 列    │ ps 值    │ /proc/1/stat 字段   │ 计算过程                    │ 验证
────────┼─────────┼────────────────────┼─────────────────────────────┼──────
PID      │ 1       │ 字段1 = 1          │ 直接读取                     │ ✓
PPID     │ 0       │ 字段4 = 0          │ 直接读取                     │ ✓
USER     │ root    │ status: Uid=0      │ 0 → /etc/passwd → root      │ ✓
STAT     │ Ss      │ 字段3=S + 推导s    │ S=睡眠, s=session leader    │ ✓
%CPU     │ 0.0     │ 字段14+15          │ (998+1154)/(存活时间×100)    │ ✓
         │         │                    │ ≈ 0.0%（21天累计，极低）     │
%MEM     │ 0.1     │ 字段24=3092页      │ 3092×4KB / 物理内存 × 100    │ ✓
         │         │                    │ = 12368KB / ~8GB ≈ 0.1%      │
VSZ      │ 21612   │ 字段23=22130688    │ 22130688 / 1024 = 21612 KB   │ ✓
RSS      │ 12368   │ 字段24=3092        │ 3092 × 4 = 12368 KB          │ ✓
TTY      │ ?       │ 字段7=0            │ 0 = 无终端 → 显示 ?          │ ✓
WCHAN    │ ep_pol  │ /proc/1/wchan      │ ep_poll 截断为 6 字符        │ ✓
COMMAND  │ systemd │ 字段2=(systemd)    │ 去掉括号                     │ ✓
```

### ★ %CPU 计算详解

```
ps 的 %CPU = (utime + stime) / (进程存活时间) × 100

utime + stime = 998 + 1154 = 2152 clock_t
             = 2152 / 100 = 21.52 秒

进程存活时间 = 当前时间 - starttime
  starttime = 16 jiffies（开机后 16/100 秒启动）
  开机至今 ≈ 21 天 = 21 × 86400 = 1,814,400 秒

%CPU = 21.52 / 1,814,400 × 100 ≈ 0.001% → 显示为 0.0

★ 注意：ps 的 %CPU 是"生命周期平均值"
  与 pidstat 的 %CPU（采样间隔内速率）完全不同！
  pidstat 看到的是"最近 1 秒的 CPU 使用率"
  ps 看到的是"从启动到现在的平均 CPU 使用率"
```

---

## 六、特殊字段解析

### ★ TTY 字段解码

```bash
$ ps -eo pid,tty,comm | head -10
```

```
    PID TT       COMMAND
      1 ?        systemd      ← 无终端（?）
      2 ?        kthreadd     ← 内核线程无终端
    999 pts/0    bash         ← 伪终端 pts/0
```

```
tty_nr（stat 字段 7）的编码方式：
  值 = (major << 8) | minor
  
  major = 0 → 无终端 → 显示 ?
  major = 136 → pts/N 伪终端
  major = 4 → tty/N 虚拟控制台
  
  例：tty_nr = 34816
    major = 34816 >> 8 = 136 → pts 终端
    minor = 34816 & 0xFF = 0 → pts/0
```

### ★ TIME 字段

```
ps 的 TIME 列 = 累计 CPU 时间（不是墙钟时间！）

格式：[分钟:]秒.百分秒

例：ps 显示 TIME = 0:21
  = (utime + stime) / CLK_TCK
  = (998 + 1154) / 100
  = 21.52 秒 → 显示为 0:21

★ TIME 不包含：
  - 进程睡眠的时间
  - 等待 I/O 的时间
  - 子进程的 CPU 时间（除非使用 -S 选项）
```

### ★ ELAPSED（墙钟时间）

```bash
$ ps -p 1 -o pid,etime,comm --no-headers
      1  21-03:45:12 systemd
```

```
ELAPSED = 当前时间 - starttime
         = 21 天 3 小时 45 分 12 秒

★ ELAPSED vs TIME 的区别：
  ELAPSED = 进程存在了多久（包含睡眠）
  TIME = 进程实际消耗了多少 CPU 时间
```

### ★ 完整命令行 vs 短名

```bash
# 短名（stat 字段 2，16 字节限制）
$ ps -p 1 -o comm --no-headers
systemd

# 完整命令行（cmdline 文件）
$ ps -p 1 -o cmd --no-headers
/usr/lib/systemd/systemd --switched-root --system --deserialize=43 rhgb vring_force_dma_api noibrs
```

```bash
# cmdline 的原始格式（NUL 分隔）
$ cat /proc/1/cmdline | tr '\0' ' '; echo
/usr/lib/systemd/systemd --switched-root --system --deserialize=43 rhgb vring_force_dma_api noibrs
```

**★ 区别：**
```
comm（短名）：
  - 来自 /proc/[pid]/stat 字段 2
  - 最大 16 字节（TASK_COMM_LEN = 16）
  - 可能被截断（如 "systemd-journal" → "systemd-journa"）
  - exec() 后变为新程序名

cmd（完整命令行）：
  - 来自 /proc/[pid]/cmdline
  - NUL 分隔的参数列表
  - 可能为空（内核线程用方括号表示）
  - exec() 后变为新程序的参数
```

### ★ 内核线程的特殊标记

```bash
$ ps aux | grep '\[' | head -5
```

```
root  2  0.0  0.0  0  0 ?  S  Jun13  0:00 [kthreadd]
root  4  0.0  0.0  0  0 ?  I< Jun13  0:00 [kworker/R-rcu_g]
```

```
内核线程特征：
  - VSZ = 0, RSS = 0（不映射用户空间内存）
  - cmdline 为空（没有 exec() 过）
  - ps 用方括号包裹名称：[kthreadd]
  - PPID = 2（kthreadd 的子进程）或 PPID = 0
```

---

## 七、ps 的两种输出格式对比

### ★ ps aux vs ps -ef

```bash
$ ps aux | head -3
USER    PID %CPU %MEM    VSZ   RSS TTY  STAT START   TIME COMMAND
root      1  0.0  0.1  21612 12368 ?    Ss   Jun13   0:21 /usr/lib/systemd/...
```

```bash
$ ps -ef | head -3
UID     PID  PPID  C STIME TTY     TIME CMD
root      1     0  0 Jun13 ?   00:00:21 /usr/lib/systemd/...
```

### ★ 字段差异对照

```
ps aux 独有          │ ps -ef 独有         │ 说明
────────────────────┼────────────────────┼──────────────────────
%CPU                │ C                  │ aux 精确到小数，-ef 整数
%MEM                │ —                  │ aux 独有内存百分比
VSZ                 │ —                  │ aux 独有虚拟内存
RSS                 │ —                  │ aux 独有物理内存
STAT                │ —                  │ aux 独有状态码
—                   │ PPID               │ -ef 独有父进程 ID
—                   │ STIME              │ -ef 独有启动时间（aux 用 START）
```

### ★ 共同字段

```
字段    │ ps aux 列名 │ ps -ef 列名 │ /proc 来源
────────┼────────────┼────────────┼──────────────
用户    │ USER        │ UID        │ status: Uid
PID     │ PID         │ PID        │ stat: 字段1
TTY     │ TTY         │ TTY        │ stat: 字段7
CPU时间 │ TIME        │ TIME       │ stat: 字段14+15
命令    │ COMMAND     │ CMD        │ cmdline
```

### ★ 推荐用法

```
场景                    │ 推荐命令
───────────────────────┼────────────────────────────────
快速查看所有进程        │ ps aux
查看父子关系           │ ps -ef 或 ps -eH（树形）
自定义列               │ ps -eo pid,stat,%cpu,wchan,comm
找特定进程             │ ps -C nginx 或 ps aux | grep nginx
查看线程               │ ps -eLf 或 ps -T -p <PID>
按 CPU 排序            │ ps aux --sort=-%cpu | head
按内存排序             │ ps aux --sort=-%mem | head
```

---

## 总结

### ★ ps 数据流全景

```
┌──────────────────────────────────────────────────────────────────┐
│                    ps 数据读取全景图                                │
│                                                                    │
│  /proc/[pid]/stat ──────────────────┐                             │
│    字段 1 (pid)        → PID        │                             │
│    字段 2 (comm)       → COMMAND    │                             │
│    字段 3 (state)      → STAT       │                             │
│    字段 4 (ppid)       → PPID       │  ★ 主要数据源               │
│    字段 7 (tty_nr)     → TTY        │  提供 90% 的输出列          │
│    字段 14+15 (CPU)    → %CPU, TIME │                             │
│    字段 18 (priority)  → PRI        │                             │
│    字段 19 (nice)      → NI         │                             │
│    字段 22 (starttime) → START      │                             │
│    字段 23 (vsize)     → VSZ        │                             │
│    字段 24 (rss)       → RSS        │                             │
│    字段 39 (processor) → PSR        │                             │
│    字段 41 (policy)    → CLS        │                             │
│                          ───────────┘                             │
│                                                                    │
│  /proc/[pid]/status ──────────────┐                               │
│    Uid            → USER          │                               │
│    VmSize         → VSZ (补充)    │  ★ 辅助数据源                 │
│    VmRSS          → RSS (补充)    │  提供 USER 和内存补充         │
│                       ────────────┘                               │
│                                                                    │
│  /proc/[pid]/cmdline ─────→ CMD（完整命令行）                     │
│  /proc/[pid]/wchan ───────→ WCHAN（睡眠函数名）                   │
│                                                                    │
│  ps 组合以上数据 → 格式化输出表格                                   │
└──────────────────────────────────────────────────────────────────┘
```

### ★ 核心映射速查表

```
想看什么         │ ps 命令                              │ /proc 数据源
────────────────┼─────────────────────────────────────┼────────────────
进程状态         │ ps -eo pid,stat,comm                │ stat 字段3
CPU 使用率       │ ps -eo pid,%cpu,comm --sort=-%cpu   │ stat 字段14+15
内存占用         │ ps -eo pid,rss,%mem,comm            │ stat 字段24/status
父子关系         │ ps -ef 或 ps -eH                    │ stat 字段4
等待什么         │ ps -eo pid,stat,wchan,comm          │ /proc/[pid]/wchan
线程信息         │ ps -eLf                             │ stat 字段20
内核线程         │ ps aux | grep '\['                  │ VSZ=0, PPID=2
```
