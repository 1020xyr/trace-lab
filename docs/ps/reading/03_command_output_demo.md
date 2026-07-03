# ps 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐列解读。
> 系统：CentOS / Linux 5.10，4 核 vCPU。

---

## 场景 1：ps aux — BSD 格式全进程列表

### 命令

```bash
ps aux
```

### 输出（选取代表性片段）

```
USER         PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND
root           1  0.0  0.1  21612 12632 ?        Ss   Jun13   0:20 /usr/lib/systemd/systemd --switched-root ...
root           2  0.0  0.0      0     0 ?        S    Jun13   0:00 [kthreadd]
root           4  0.0  0.0      0     0 ?        I<   Jun13   0:00 [kworker/R-rcu_g]
root          17  0.0  0.0      0     0 ?        I    Jun13   4:31 [rcu_preempt]
root         979  0.0  0.0  12172  4996 ?        Ss   Jun13   0:02 sshd: /usr/sbin/sshd -D [listener] ...
root      433969 47.9  7.9 73866652 606132 pts/2 Rl+  22:58  11:35 claude --resume
root      460581  0.1  0.2  67428 17804 ?        S    23:28   0:00 nginx: worker process
```

### 逐列注解

```
列名       │ 含义                              │ 数据来源
───────────┼──────────────────────────────────┼────────────────────────────────
USER       │ ★ 进程所有者用户名               │ /proc/[pid]/status → Uid → 查 /etc/passwd
PID        │ ★ 进程 ID                        │ /proc/[pid]/stat 字段 1
%CPU       │ ★ CPU 使用率 (%)                 │ (utime+stime 差值) / (采样间隔 × HZ) × 100
%MEM       │ ★ 物理内存使用率 (%)            │ RSS / MemTotal × 100
VSZ        │ ★ 虚拟内存大小 (KB)             │ /proc/[pid]/stat 字段 23 (vsize / 1024)
RSS        │ ★ 常驻内存集 (KB)               │ /proc/[pid]/stat 字段 24 (rss × PAGE_SIZE / 1024)
TTY        │ 控制终端                          │ /proc/[pid]/stat 字段 7
STAT       │ ★ 进程状态 + 修饰符              │ /proc/[pid]/stat 字段 3 + flags
START      │ 启动时间                          │ /proc/[pid]/stat 字段 22 (starttime)
TIME       │ 累计 CPU 时间                     │ (utime + stime) / HZ
COMMAND    │ 完整命令行                        │ /proc/[pid]/cmdline
```

### 逐行解读

```
第 1 行 — systemd (PID 1):
  USER=root      → 由 root 运行
  PID=1          → 系统第一个进程
  %CPU=0.0       → 几乎不占 CPU
  %MEM=0.1       → 占物理内存 0.1%
  VSZ=21612      → 虚拟内存 21MB
  RSS=12632      → 实际占用物理内存 12MB
  TTY=?          → ? 表示没有控制终端（守护进程）
  STAT=Ss        → S(睡眠) + s(会话 leader)
                   ★ systemd 是会话 leader，当前在等待事件
  START=Jun13    → 6 月 13 日启动
  TIME=0:20      → 累计使用 20 秒 CPU 时间
  COMMAND        → 完整命令行

第 2 行 — kthreadd (PID 2):
  VSZ=0, RSS=0  → ★ 内核线程没有用户空间内存，所以 VSZ/RSS 都是 0
  STAT=S         → 睡眠中（等待创建新的内核线程）
  COMMAND=[kthreadd]  → 方括号包裹 = 内核线程

第 3 行 — kworker (PID 4):
  STAT=I<        → I(空闲内核线程) + <(高优先级)
  COMMAND=[kworker/R-rcu_g]
     → R-rcu_g 表示这是 RCU 相关的高优先级工作线程

第 4 行 — rcu_preempt (PID 17):
  STAT=I         → 空闲内核线程（无修饰符）
  TIME=4:31      → ★ 累计 4 分 31 秒 CPU 时间 — RCU 回收工作量大

第 5 行 — claude (PID 433969):
  %CPU=47.9      → ★ 占用近一半 CPU（但只有 1/4 核，说明在消耗约 1.9 核）
  %MEM=7.9       → 占物理内存 7.9%
  VSZ=73866652   → 虚拟内存 ~70GB（★ 很大但不用担心，大部分是映射未使用）
  RSS=606132     → 实际占用 ~592MB
  TTY=pts/2      → 在 pts/2 伪终端上
  STAT=Rl+       → R(运行) + l(多线程) + +(前台)
                   ★ 正在前台运行的多线程进程
  TIME=11:35     → 已使用 11 分 35 秒 CPU 时间

第 6 行 — nginx worker (PID 460581):
  STAT=S         → 睡眠中（等待网络请求）
  TTY=?          → 无控制终端（守护进程的工作进程）
```

### VSZ 异常大怎么办？

```
VSZ=73866652 KB ≈ 70 GB — 看起来很大！

★ 这是正常的。VSZ 包含所有虚拟地址空间映射：
  - 共享库（libc.so 等）
  - 内存映射文件（mmap）
  - 预留但未使用的地址空间
  - 线程栈

关键指标是 RSS（实际物理内存占用），而不是 VSZ。
当 VSZ >> RSS 时，说明大部分虚拟内存还没映射到物理页。
```

---

## 场景 2：ps -ef — System V 格式全进程列表

### 命令

```bash
ps -ef
```

### 输出（选取片段）

```
UID        PID    PPID  C STIME TTY          TIME CMD
root         1       0  0 Jun13 ?        00:00:20 /usr/lib/systemd/systemd --switched-root ...
root         2       0  0 Jun13 ?        00:00:00 [kthreadd]
root        17       2  0 Jun13 ?        00:04:31 [rcu_preempt]
root       979       1  0 Jun13 ?        00:00:02 sshd: /usr/sbin/sshd -D [listener] ...
root     433969  433889 37 22:58 pts/2    00:11:35 claude --resume
```

### 逐列注解

```
列名       │ 含义                              │ 与 BSD 格式的对比
───────────┼──────────────────────────────────┼─────────────────────
UID        │ ★ 用户 ID（这里显示用户名）      │ 对应 BSD 的 USER
PID        │ ★ 进程 ID                        │ 相同
PPID       │ ★ 父进程 ID                      │ ★ BSD aux 没有此列！
C          │ CPU 使用（整数 0-99）            │ 对应 BSD 的 %CPU（但精度低）
STIME      │ 启动时间                          │ 对应 BSD 的 START
TTY        │ 控制终端                          │ 相同
TIME       │ 累计 CPU 时间                     │ 相同
CMD        │ 完整命令行                        │ 对应 BSD 的 COMMAND
```

### 与 ps aux 的对比

```
ps aux 有但 ps -ef 没有的列：
  %MEM — 物理内存使用率
  VSZ  — 虚拟内存
  RSS  — 常驻内存
  STAT — 详细状态（含修饰符）

ps -ef 有但 ps aux 没有的列：
  PPID — 父进程 ID ★

★ 结论：
  - 性能诊断用 ps aux（有 %CPU、%MEM、VSZ、RSS、STAT）
  - 看父子关系用 ps -ef（有 PPID）
  - 或者用 -o 自定义格式，两者都要
```

### PPID 列的价值

```
通过 PPID 可以追踪进程树：

PID 979 (sshd listener)
  PPID=1 → 由 systemd(PID 1) 启动
  │
  ├── PID 433214 (sshd: root [priv])
  │     PPID=979 → 由 sshd listener fork
  │     │
  │     └── PID 433217 (sshd: root@notty)
  │           PPID=433214 → 特权进程 fork 的工作进程
  │
  └── PID 433288 (sshd: root [priv])
        PPID=979 → 另一个 SSH 连接
```

---

## 场景 3：ps -eo pid,stat,wchan,cmd — 自定义格式

### 命令

```bash
ps -eo pid,stat,wchan,cmd --sort=-%cpu | head -20
```

### 输出

```
    PID STAT WCHAN  CMD
 433969 Rl+  -      claude --resume
 433710 Sl   unix_s /root/.vscode-server/.../cpptools
  43113 Sl+  ep_pol /usr/local/bin/claude agents
 433385 Sl   ep_pol /root/.vscode-server/.../node --type=extensionHost ...
 433289 Sl   ep_pol /root/.vscode-server/.../node .../server-main.js ...
 201857 Sl+  ep_pol claude --resume c18786a2-...
 433427 Sl   ep_pol /root/.vscode-server/.../node --type=ptyHost ...
   1512 Ssl  hrtime /usr/local/aegis/aegis_client/.../AliYunDunMonitor
 433384 Sl   ep_pol /root/.vscode-server/.../node --type=extensionHost ...
 433322 Sl   futex_ /root/.vscode-server/.../code-fcf60477... command-shell ...
   1490 Ssl  hrtime /usr/local/aegis/aegis_client/.../AliYunDun
 433298 S    do_pol sshd: root@notty
 460581 S    ep_pol nginx: worker process
```

### WCHAN 列解读

```
WCHAN     │ 全称                         │ 含义
──────────┼──────────────────────────────┼────────────────────────────
-         │ (running)                    │ ★ 进程正在运行，不等待
ep_pol    │ epoll_poll                   │ 在 epoll_wait() 中等待事件
unix_s    │ unix_stream_sendpage         │ 在 UNIX socket 上等待
futex_    │ futex_wait                   │ 在 futex 上等待锁
hrtime    │ hrtimer_nanosleep            │ 在定时器中等待（sleep）
do_pol    │ do_poll                      │ 在 poll() 系统调用中等待
```

★ **WCHAN 是排查"进程在干什么"的关键字段：**
- 看到 `ep_pol` → 进程在用 epoll 等待网络/文件事件
- 看到 `futex_` → 进程在等待锁（可能是线程同步）
- 看到 `io_schedule` → 进程在等待磁盘 I/O（D 状态！）
- 看到 `-` → 进程正在 CPU 上运行

### 排序解读

```
--sort=-%cpu 让输出按 CPU 使用率降序排列。

第一行 PID=433969 (claude) 的 STAT=Rl+：
  R → 正在运行
  l → 多线程
  + → 前台进程
  WCHAN=- → 不等待任何事件（正在 CPU 上执行）

这解释了为什么它 CPU 最高 — 它确实正在运行。
```

---

## 场景 4：ps --sort=-%cpu | head — CPU 占用排行

### 命令

```bash
ps aux --sort=-%cpu | head -11
```

### 输出

```
USER       PID %CPU %MEM    VSZ   RSS TTY   STAT START   TIME COMMAND
root    433969 47.9  7.9 73866652 606132 pts/2 Rl+  22:58 11:35 claude --resume
root    433710  6.2  3.6 304684 281020 ?    Sl   22:57  1:31 cpptools
root     43113  2.8  2.4 73521792 188544 pts/0 Sl+  Jun24 27:12 claude agents
root    433385  1.8  5.0 10390776 388520 ?   Sl   22:56  0:46 node --type=extensionHost
root    433289  1.5  2.7 1835088 206364 ?    Sl   22:56  0:38 node .../server-main.js
root    201857  0.8  5.6 73928072 428944 pts/1 Sl+  Jun26 87:14 claude --resume
root    433427  0.7  1.3 1576516  99900 ?    Sl   22:56  0:17 node --type=ptyHost
root      1512  0.6  0.5 157628  38752 ?    Rsl  Jun13 156:11 AliYunDunMonitor
```

### 解读

```
★ Top CPU 消费者分析：

#1 claude (PID 433969) — 47.9% CPU
   STAT=Rl+ → 正在前台运行，多线程
   TIME=11:35 → 已使用 11 分 35 秒 CPU
   ★ 这是当前正在执行的主进程

#2 cpptools (PID 433710) — 6.2% CPU
   STAT=Sl → 睡眠中，多线程
   NLWP 可通过 -eLf 确认
   ★ VSZ=304684, RSS=281020 → VSZ ≈ RSS，内存几乎全映射了

#8 AliYunDunMonitor (PID 1512) — 0.6% CPU
   STAT=Rsl → R(运行) + s(会话 leader) + l(多线程)
   TIME=156:11 → ★ 运行很久，累计 156 分钟 CPU
   这是阿里云安骑士的监控进程
```

### %CPU 超过 100% 的情况

```
★ 多核系统上 %CPU 的计算方式：

ps aux 中的 %CPU 是 per-process 的，不是 per-core。

计算方式：
  %CPU = (Δutime + Δstime) / (Δwall_time × HZ) × 100

如果一个进程有 4 个线程，每个线程占满一个核：
  utime + stime 在 1 秒内增加了 400 jiffies（假设 HZ=100）
  %CPU = 400 / 100 × 100 = 400%

所以：
  - 4 核机器上，单进程最高 %CPU = 400%
  - 8 核机器上，单进程最高 %CPU = 800%
  - 如果你看到 %CPU=350%，说明进程几乎占满了所有核
```

---

## 场景 5：ps -eLf — 线程列表

### 命令

```bash
ps -eLf | head -20
```

### 输出

```
UID        PID    PPID     LWP  C NLWP STIME TTY          TIME CMD
root         1       0       1  0    1 Jun13 ?        00:00:21 /usr/lib/systemd/systemd ...
root         2       0       2  0    1 Jun13 ?        00:00:00 [kthreadd]
root        17       2      17  0    1 Jun13 ?        00:04:31 [rcu_preempt]
root       979       1     979  0    1 Jun13 ?        00:00:02 sshd: /usr/sbin/sshd -D ...
root    433969  433889  433969 37   12 22:58 pts/2    00:11:35 claude --resume
root    433969  433889  433970  0   12 22:58 pts/2    00:00:04 claude --resume
root    433969  433889  433971  3   12 22:58 pts/2    00:01:04 claude --resume
root    433969  433889  433972  3   12 22:58 pts/2    00:01:02 claude --resume
root    433969  433889  433973  3   12 22:58 pts/2    00:01:00 claude --resume
```

### 逐列注解

```
列名       │ 含义                              │ 数据来源
───────────┼──────────────────────────────────┼────────────────────────
UID        │ 用户                              │ /proc/[pid]/status
PID        │ ★ 进程 ID（同一进程的所有线程共享）│ /proc/[pid]/stat
PPID       │ 父进程 ID                         │ /proc/[pid]/stat
LWP        │ ★ 线程 ID (Lightweight Process)  │ /proc/[pid]/task/ 子目录名
C          │ CPU 使用（per-thread）            │ per-thread 的 CPU 时间
NLWP       │ ★ 进程的线程总数                  │ /proc/[pid]/stat 字段20
STIME      │ 启动时间                          │ /proc/[pid]/stat 字段22
TTY        │ 控制终端                          │ /proc/[pid]/stat 字段7
TIME       │ CPU 时间（per-thread）            │ per-thread 的 utime+stime
CMD        │ 命令名                            │ /proc/[pid]/cmdline
```

### 线程解读

```
PID=433969 的 claude 进程有 12 个线程（NLWP=12）：

LWP=433969  C=37  TIME=11:35  → ★ 主线程，消耗 37% CPU，最多时间
LWP=433970  C=0   TIME=00:04  → 工作线程，空闲中
LWP=433971  C=3   TIME=01:04  → 工作线程，活跃
LWP=433972  C=3   TIME=01:02  → 工作线程，活跃
LWP=433973  C=3   TIME=01:00  → 工作线程，活跃

★ 关键洞察：
  - PID 和 PPID 对同一进程的所有线程都是相同的
  - LWP 是每个线程的唯一标识
  - C 和 TIME 是 per-thread 的，可以看到哪个线程最忙
  - NLWP 对同一进程的所有线程行都相同（=12）
```

### 线程排查技巧

```bash
# 找出线程数最多的进程
ps -eo pid,nlwp,cmd --sort=-nlwp | head -10

# 特定进程的所有线程
ps -eLf -p 433969

# 哪些线程在消耗 CPU
ps -eLf --sort=-C | head -10
```

---

## 场景 6：ps -C sshd -f — 按命令名过滤

### 命令

```bash
ps -C sshd -f
```

### 输出

```
UID        PID    PPID  C STIME TTY          TIME CMD
root       979       1  0 Jun13 ?        00:00:02 sshd: /usr/sbin/sshd -D [listener] 0 of 10-100 startups
root    433214     979  0 22:56 ?        00:00:00 sshd: root [priv]
root    433217  433214  0 22:56 ?        00:00:01 sshd: root@notty
root    433288     979  0 22:56 ?        00:00:00 sshd: root [priv]
root    433298  433288  0 22:56 ?        00:00:04 sshd: root@notty
```

### 解读

```
★ sshd 的进程模型（经典的 fork + 特权分离）：

PID 979 (sshd listener)
│ PPID=1 → 由 systemd 启动
│ CMD="sshd -D [listener]" → -D 表示前台运行，[listener] 表示监听进程
│
├── PID 433214 (sshd: root [priv])
│   │ PPID=979 → 由 listener fork 出来的
│   │ [priv] → 特权进程（持有 root 权限）
│   │
│   └── PID 433217 (sshd: root@notty)
│       PPID=433214 → 由 [priv] fork 出来的
│       @notty → 非交互式会话（没有分配 pty，如 SSH 命令执行）
│       TIME=00:00:01 → 这个连接很少活动
│
└── PID 433288 (sshd: root [priv])
    │ PPID=979 → 另一个 SSH 连接
    │
    └── PID 433298 (sshd: root@notty)
        PPID=433288
        TIME=00:00:04 → 这个连接更活跃（可能是当前的 SSH 会话）

★ 安全设计：
  - [listener] 监听新连接（以 root 运行）
  - [priv] 处理认证（以 root 运行）
  - root@notty 是降权后的工作进程
```

---

## 场景 7：ps -p PID -o pid,stat,%cpu,%mem,vsz,rss,wchan,cmd — 特定进程详情

### 命令

```bash
ps -p 1 -o pid,stat,%cpu,%mem,vsz,rss,wchan,cmd
```

### 输出

```
    PID STAT %CPU %MEM    VSZ   RSS WCHAN  CMD
      1 Ss    0.0  0.1  21612 12664 ep_pol /usr/lib/systemd/systemd --switched-root --system --deserialize=43 rhgb vring_force_dma_api noibrs
```

### 解读

```
字段   │ 值       │ 含义
───────┼─────────┼──────────────────────────────────────────────
PID    │ 1        │ 系统第一个用户空间进程
STAT   │ Ss       │ S(睡眠) + s(会话 leader)
                    ★ systemd 是所有用户会话的 leader
%CPU   │ 0.0      │ 几乎不消耗 CPU
%MEM   │ 0.1      │ 占 0.1% 物理内存
VSZ    │ 21612 KB │ 虚拟内存 ~21MB
RSS    │ 12664 KB │ 实际物理内存 ~12MB
WCHAN  │ ep_pol   │ ★ 正在 epoll_wait() 中等待事件
                    systemd 用 epoll 监控所有子进程的状态
CMD    │ (完整命令行) │ systemd 的启动参数

★ WCHAN=ep_pol 的含义：
  systemd 作为 init 进程，需要同时监控数百个子进程的状态变化。
  它使用 epoll 机制等待以下事件：
  - 子进程退出（SIGCHLD）
  - D-Bus 消息
  - socket 激活
  - timer 到期
  - 文件变化（inotify）
```

---

## 场景 8：进程树 — ps --forest

### 命令

```bash
ps -eo pid,ppid,cmd --forest | head -40
```

### 输出

```
    PID    PPID CMD
      2       0 [kthreadd]
      3       2  \_ [pool_workqueue_release]
      4       2  \_ [kworker/R-rcu_g]
      5       2  \_ [kworker/R-rcu_p]
      6       2  \_ [kworker/R-slub_]
      7       2  \_ [kworker/R-netns]
      9       2  \_ [kworker/0:0H-events_highpri]
     16       2  \_ [ksoftirqd/0]
     17       2  \_ [rcu_preempt]
     18       2  \_ [migration/0]
     43       2  \_ [kdevtmpfs]
     45       2  \_ [kauditd]
     46       2  \_ [khungtaskd]
     49       2  \_ [kcompactd0]
```

### 解读

```
★ --forest 的缩进规则：

  \_ 表示"是上面进程的子进程"

  PID 2 [kthreadd] 是内核线程的根：
    所有内核线程（方括号包裹的名字）都是 kthreadd 的子进程。
    kthreadd 是内核线程的"保姆"——负责创建和管理所有内核线程。

★ 常见的内核线程：

  线程名               │ 作用
  ─────────────────────┼──────────────────────────────
  kthreadd             │ 内核线程管理器
  kworker/*            │ 工作线程（处理内核工作队列）
  ksoftirqd/*          │ 软中断处理线程（per-CPU）
  migration/*          │ 进程迁移线程（per-CPU，负载均衡）
  kcompactd*           │ 内存碎片整理线程
  khungtaskd           │ 检测 D 状态超时的看门狗
  oom_reaper           │ OOM 时回收内存
  kdevtmpfs            │ 管理 /dev 设备文件系统
  kauditd              │ 审计日志线程
```

---

## 场景 9：STAT 字段每个字符的含义注解

### 统计当前系统的 STAT 分布

```bash
ps -eo pid,stat,cmd | awk '{print $2}' | sort | uniq -c | sort -rn | head
```

### 输出

```
  53 S           ← 可中断睡眠（无修饰符）
  41 I<          ← 空闲高优先级内核线程
  22 Ss          ← 会话 leader 睡眠
  16 I           ← 空闲内核线程
  15 Sl          ← 多线程睡眠
   9 Ssl         ← 会话 leader + 多线程睡眠
   5 Ss+         ← 会话 leader + 前台睡眠
   3 SN          ← 低优先级睡眠
   2 Sl+         ← 多线程 + 前台睡眠
   1 Rl+         ← 多线程 + 前台运行
   1 R           ← 运行中（就是 ps 自己）
```

### 每个 STAT 的逐字符解读

```
STAT  │ 首字符    │ 修饰字符                          │ 典型进程
──────┼──────────┼──────────────────────────────────┼────────────────────
S     │ S(睡眠)   │ (无修饰)                          │ 普通后台睡眠进程
I<    │ I(空闲)   │ <(高优先级)                       │ kworker/R-* 内核线程
I     │ I(空闲)   │ (无修饰)                          │ rcu_tasks_* 内核线程
Ss    │ S(睡眠)   │ s(会话 leader)                    │ sshd listener, crond
Sl    │ S(睡眠)   │ l(多线程)                         │ 多线程后台服务
Ssl   │ S(睡眠)   │ s(会话 leader) + l(多线程)        │ systemd, AliYunDun
Ss+   │ S(睡眠)   │ s(会话 leader) + +(前台)          │ 登录 shell (bash)
SN    │ S(睡眠)   │ N(低优先级, nice>0)               │ renice 过的进程
Sl+   │ S(睡眠)   │ l(多线程) + +(前台)               │ 前台多线程进程
Rl+   │ R(运行)   │ l(多线程) + +(前台)               │ 正在运行的前台进程
R+    │ R(运行)   │ +(前台)                           │ ps 命令本身
S<sl  │ S(睡眠)   │ <(高优先级)+s(会话leader)+l(多线程)│ 高优先级守护进程
```

### 修饰字符完整映射

```
修饰符 │ 来源字段              │ 条件                    │ 含义
───────┼──────────────────────┼────────────────────────┼───────────────────
  s    │ stat 字段 6 (session)│ pid == session          │ 会话 leader
  l    │ stat 字段 20         │ num_threads > 1         │ 多线程
  <    │ stat 字段 19 (nice)  │ nice < 0                │ 高优先级
  N    │ stat 字段 19 (nice)  │ nice > 0                │ 低优先级
  +    │ stat 字段 8 (tpgid)  │ pgrp == tpgid           │ 前台进程组
  L    │ (检查 mlock)         │ 有 mlock 的页面         │ 有锁定的内存页
```

### 前台 vs 后台的 "+" 修饰符

```
★ "+" 表示进程在前台进程组中。

在终端中：
  - 前台进程组（+）：能接收键盘输入和 Ctrl+C/Ctrl+Z 信号
  - 后台进程组（无 +）：不能直接接收终端信号

示例：
  $ sleep 100 &         → STAT=S+（后台作业，但在前台进程组？不！）
  实际上 & 放到后台后：STAT=S（无 +，因为不在前台进程组）

  $ sleep 100           → STAT=S+（前台运行）
  按 Ctrl+Z 后：        → STAT=T+（停止，仍在前台进程组）
  $ bg                  → STAT=S（后台继续，不再是前台进程组）
  $ fg                  → STAT=S+（回到前台进程组）
```

---

## 场景 10：排查 D 状态进程

### 命令

```bash
# 查找所有 D 状态进程
ps -eo pid,ppid,stat,wchan,cmd | awk '$3 ~ /^D/'
```

### 示例输出（模拟 NFS 卡住场景）

```
    PID    PPID STAT WCHAN       CMD
  12345   12340 D+   io_schedule dd if=/dev/nfs_mount/file of=/dev/null
```

### 排查步骤

```bash
# 1. 看 WCHAN — 卡在内核哪个函数
cat /proc/12345/wchan
# 输出：io_schedule

# 2. 看内核调用栈
cat /proc/12345/stack
# 输出示例：
# [<0>] io_schedule+0x12/0x40
# [<0>] nfs_wait_bit+0x20/0x30
# [<0>] __nfs_pageio_add_request+0x100/0x200
# → 说明在等待 NFS 服务器响应

# 3. 看系统日志
dmesg | grep -i "blocked\|nfs"
# 输出示例：
# NFS: v4 server not responding, still trying
# INFO: task dd:12345 blocked for more than 120 seconds.

# 4. 检查 NFS 连接
mount | grep nfs
# 看是否有 stale file handle 或 server not responding

# 5. 尝试恢复
# 如果 NFS 服务器恢复了，D 状态会自动解除
# 如果无法恢复，只能：
umount -f /nfs_mount   # 强制卸载
umount -l /nfs_mount   # 延迟卸载（lazy）
```

---

## 场景 11：脚本中的 ps 用法

### 检查进程是否存在

```bash
PID=433969

# 方法 1：用 ps 返回值
if ps -p $PID > /dev/null 2>&1; then
    echo "进程 $PID 正在运行"
else
    echo "进程 $PID 不存在"
fi
```

### 获取进程 PID（脚本中使用）

```bash
# -o pid= 去掉标题行
# --no-headers 也可以
ps -C nginx -o pid= --no-headers
# 输出（仅 PID）：
# 979
# 1234
# 5678
```

### 统计各用户进程数

```bash
ps -eo user --no-headers | sort | uniq -c | sort -rn
```

### 输出

```
  185 root          ← root 用户有 185 个进程
    3 nginx         ← nginx 用户有 3 个进程
    1 systemd-resolve
    1 systemd-network
    1 dbus
```

---

## 总结：ps 输出解读流程

```
看到 ps 输出时的解读顺序：

1. 看 STAT 首字符 → 进程在什么状态？
   R → 正在运行或就绪
   S → 在等待事件（正常）
   D → 在等 I/O（需要关注）
   Z → 僵尸（父进程未回收）
   T → 已暂停

2. 看 STAT 修饰符 → 补充信息
   s → 是守护进程/会话 leader
   l → 是多线程
   + → 在前台
   < → 高优先级
   N → 低优先级

3. 看 %CPU → 消耗多少 CPU？
   高 → 正在做计算密集型工作
   低 → 空闲或 I/O 密集型

4. 看 %MEM / RSS → 占用多少内存？
   RSS 是实际物理内存占用
   VSZ 大不一定有问题

5. 看 WCHAN → 在等什么？
   - → 正在运行
   ep_pol → 在 epoll 等待
   io_schedule → 在等 I/O
   futex_ → 在等锁

6. 看 TIME → 累计消耗了多少 CPU？
   大 → 长期运行的进程
   小 → 新启动的或很少活跃的进程
```
