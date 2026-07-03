# 进程状态详解 — Linux 进程状态机与 /proc 数据源

> 阅读要点：
>   1. 7 种进程状态的内核常量、触发条件和转换路径
>   2. /proc/[pid]/stat 的 52 个字段逐一解读
>   3. STAT 字段的多字符编码规则
>   4. D 状态的排查（为什么 kill -9 杀不掉）
>
> 预计阅读时间：20 分钟

---

## 一、进程状态总览

### 1.1 状态速查表

```
字母 │ 内核常量              │ 全称                         │ 可被 kill │ 常见场景
─────┼──────────────────────┼──────────────────────────────┼───────────┼─────────────────────
 R   │ TASK_RUNNING          │ Running / Runnable           │ ✅ 可杀   │ 正在 CPU 上或等待调度
 S   │ TASK_INTERRUPTIBLE    │ Sleeping (interruptible)     │ ✅ 可杀   │ 等待事件或信号（最常见）
 D   │ TASK_UNINTERRUPTIBLE  │ Disk sleep (uninterruptible) │ ❌ 不可杀 │ 等待 I/O、锁、NFS 等
 Z   │ EXIT_ZOMBIE           │ Zombie                       │ ❌ 不可杀 │ 已退出，父进程未回收
 T   │ TASK_STOPPED          │ Stopped                      │ ✅ 可杀   │ SIGSTOP / Ctrl+Z
 t   │ TASK_TRACED           │ Tracing stop                 │ ✅ 可杀   │ ptrace 调试器暂停
 X   │ EXIT_DEAD             │ Dead                         │ —        │ 瞬态，几乎看不到
 I   │ TASK_IDLE             │ Idle kernel thread           │ ❌ 不可杀 │ 内核线程空闲等待
```

★ **ps 的 STAT 列**不仅包含状态字母，还附带修饰字符（如 Ss、S+、Ss+），详见第三节。

### 1.2 状态转换全景图

```
                          fork() / clone()
                               │
                               ▼
                     ┌──────────────────┐
                     │     新建 (N)      │
                     │    TASK_RUNNING   │  ← 加入调度器就绪队列
                     └────────┬─────────┘
                              │ 调度器选中（schedule()）
                              ▼
                     ┌──────────────────┐
                ┌───→│    运行中 (R)     │←──────────────────┐
                │    │   TASK_RUNNING    │                    │
                │    └──┬──────────┬────┘                    │
                │       │          │                         │
                │  时间片用完    等待资源                     │
                │  或被抢占      (sleep)                     │
                │       │          │                         │
                │       │     ┌────┴─────┐                   │
                │       │     │          │                   │
                │       │     ▼          ▼                   │
                │  ┌─────────┐   ┌───────────┐              │
                │  │ 睡眠(S)  │   │ 深睡眠(D)  │              │
                │  │可中断    │   │ 不可中断    │              │
                │  │等信号/   │   │ 等 I/O/锁  │              │
                │  │等事件    │   │            │              │
                │  └────┬────┘   └─────┬─────┘              │
                │       │              │                     │
                │  收到信号或      等待条件满足               │
                │  事件发生       (I/O完成/锁释放)           │
                │       │              │                     │
                │       └──────┬───────┘                     │
                │              └─────────────────────────────┘
                │                     重新进入就绪队列
                │
                │  收到 SIGSTOP / SIGTSTP / 调试器暂停
                │
                ▼
       ┌──────────────────┐
       │    停止 (T)       │
       │  TASK_STOPPED     │  ← 进程暂停执行
       └────────┬─────────┘
                │ 收到 SIGCONT
                └──────────→ 回到就绪队列 (R)

                收到 SIGTRAP（ptrace）
                │
                ▼
       ┌──────────────────┐
       │  追踪停止 (t)     │
       │  TASK_TRACED      │  ← 调试器暂停
       └────────┬─────────┘
                │ 调试器继续
                └──────────→ 回到就绪队列 (R)

                进程调用 exit()
                │
                ▼
       ┌──────────────────┐
       │   僵尸 (Z)        │
       │  EXIT_ZOMBIE      │  ← ★ 进程已死，但 task_struct 还在
       │                   │    等待父进程 wait() 回收
       └────────┬─────────┘
                │ 父进程调用 wait() / waitpid()
                ▼
             彻底消亡（EXIT_DEAD → 释放 task_struct）
```

---

## 二、各状态详解

### 2.1 R — Running / Runnable（运行 / 就绪）

```
内核常量：TASK_RUNNING（值 0）

含义：进程正在 CPU 上执行，或者在运行队列（runqueue）中等待被调度。

★ 注意：Linux 把"运行中"和"就绪"合并为同一个状态。
  ps 中看到 R 的进程，可能是：
  - 真的正在 CPU 上执行（此刻在消耗 CPU 周期）
  - 在就绪队列中等待（其他进程占着 CPU）

触发条件：
  - fork() 后加入调度器
  - 从 S/D/T 状态被唤醒
  - 时间片用完后重新排队

ps 中看到 R 的含义：
  - 如果大部分进程是 R → 系统繁忙，CPU 可能不够
  - 如果只有一个 R（就是 ps 自己）→ 系统空闲
```

### 2.2 S — Sleeping（可中断睡眠）

```
内核常量：TASK_INTERRUPTIBLE（值 1）

含义：进程正在等待某个事件发生，可以被信号唤醒。

最常见的等待场景：
  - 等待用户输入（stdin read）
  - 等待网络数据（socket recv）
  - 等待定时器到期（sleep/nanosleep）
  - 等待锁释放（futex/mutex）
  - 等待子进程退出（waitpid）

★ 大多数进程大部分时间都处于 S 状态 — 这是正常的！
  一个空闲系统中 95%+ 的进程都是 S 状态。

内核代码入口：
  schedule() → 当前进程从运行队列移除 → 切换到其他进程

ps 中看到 S 的含义：
  - 进程在等待某个事件
  - 可以通过 WCHAN 列看到等待的具体内核函数
  - 收到信号后会转为 R 状态（被唤醒）
```

### 2.3 D — Uninterruptible Sleep（不可中断睡眠）

```
内核常量：TASK_UNINTERRUPTIBLE（值 2）

含义：进程正在等待某个条件，且不能被信号打断。

★ 这是最"危险"的状态 — kill -9 也杀不掉！

常见触发场景：
  - 等待磁盘 I/O 完成（NFS、坏盘、慢盘）
  - 等待内核锁（mutex、semaphore）
  - 等待设备驱动响应
  - 等待文件系统操作（sync、fsync）

内核代码入口：
  io_schedule()  — 等待 I/O 完成
  mutex_lock()   — 等待互斥锁

★ 为什么 D 状态不能被 kill？
  ┌──────────────────────────────────────────────────┐
  │  进程处于 D 状态时，内核代码假设进程不会被中断。   │
  │  如果在内核代码的关键路径上中断进程，可能导致：     │
  │  - 数据结构不一致（写了一半的状态）                 │
  │  - 死锁（持有某个锁时被杀死）                      │
  │  - 文件系统损坏                                    │
  │                                                   │
  │  所以内核设计为：D 状态不响应信号。                 │
  │  只有等待的条件满足后，进程才会回到 R 状态，        │
  │  此时才能处理 pending 的信号。                      │
  └──────────────────────────────────────────────────┘

排查 D 状态：
  1. ps -eo pid,stat,wchan,cmd | grep " D"
     → 看 WCHAN 列，知道卡在内核哪个函数

  2. cat /proc/<PID>/stack
     → 看内核调用栈，定位具体卡在哪一步

  3. dmesg | tail -50
     → 看是否有 I/O 错误、超时信息

  4. iostat -x 1
     → 看磁盘 I/O 是否正常
```

### 2.4 Z — Zombie（僵尸进程）

```
内核常量：EXIT_ZOMBIE（值 32）

含义：进程已经退出（exit），但父进程还没有调用 wait()/waitpid() 读取退出状态。

★ 僵尸进程不消耗 CPU 和内存，但占用 PID 和 task_struct。

生命周期：
  进程调用 exit()
       │
       ├── 释放内存映射（mm_struct）
       ├── 释放文件描述符
       ├── 释放信号量等 IPC 资源
       │
       └── 但保留 task_struct（存储退出码和统计信息）
           → 进入 Z 状态
           → 等待父进程 wait()

  父进程调用 wait()
       │
       └── 读取子进程的退出码
           → 释放 task_struct
           → 进程彻底消亡

★ 为什么杀不掉僵尸进程？
  僵尸进程已经"死了"——它不运行任何代码，不占 CPU/内存。
  kill 发送信号给进程，但僵尸进程已无法接收信号。
  只有父进程 wait() 才能回收它。

排查僵尸进程：
  ps -eo pid,ppid,stat,cmd | awk '$3 ~ /Z/'
  → 找到 PPID，看父进程是谁
  → 如果父进程是 init(1)，说明原父进程已死，init 会负责回收

消除方法：
  1. 杀死父进程 → 僵尸被 init 收养并自动回收
  2. 通知父进程回收 → kill -SIGCHLD <parent_pid>
```

### 2.5 T — Stopped（停止）

```
内核常量：TASK_STOPPED（值 4）

含义：进程被暂停执行，不消耗 CPU。

触发条件：
  - 收到 SIGSTOP 信号（不可捕获）
  - 收到 SIGTSTP 信号（Ctrl+Z，可捕获）
  - job control 暂停（shell 的 bg/fg 机制）
  - 进程自己调用 raise(SIGSTOP)

恢复：
  - 收到 SIGCONT 信号
  - shell 命令：fg（前台继续）或 bg（后台继续）

ps 中看到 T：
  - 通常是用户手动 Ctrl+Z 暂停的
  - 在后台作业列表中常见
```

### 2.6 t — Tracing Stop（追踪停止）

```
内核常量：TASK_TRACED（值 8）

含义：进程被 ptrace 调试器暂停。

触发条件：
  - strace 附加到进程
  - gdb 调试时设置断点
  - 任何 ptrace(PTRACE_ATTACH) 操作

ps 中看到 t：
  - 通常说明有调试器正在跟踪这个进程
  - strace 附加期间，进程状态显示为 t
```

### 2.7 I — Idle Kernel Thread（空闲内核线程）

```
内核常量：TASK_IDLE（值 1024，Linux 4.14+）

含义：空闲的内核线程，不会增加系统 load average。

★ 这是 Linux 4.14 引入的新状态。
  早期版本中，空闲内核线程用 D 状态表示，但会导致 load average 虚高。
  引入 I 状态后，idle 内核线程不计入 load。

ps 中看到 I：
  - 通常是内核线程（进程名用方括号包裹，如 [kworker/0:0H-events]）
  - 带有 < 修饰符 → 高优先级内核线程
```

---

## 三、STAT 字段的多字符编码

ps 的 STAT 列不仅包含状态字母，还可能带有修饰字符：

### 3.1 修饰字符速查表

```
修饰符 │ 含义                          │ 来源
───────┼───────────────────────────────┼────────────────────────────
  s    │ Session leader（会话首进程）   │ 进程是会话的 leader
  l    │ 多线程（multi-threaded）       │ 进程包含多个线程（NLWP > 1）
  <    │ 高优先级（high priority）      │ nice 值 < 0
  N    │ 低优先级（low nice）           │ nice 值 > 0
  +    │ 前台进程组（foreground）       │ 进程在前台进程组中
  L    │ 有锁页（pages locked）         │ 有内存页被 mlock 锁定
  R    │ (此处不是状态)                 │ 不会出现在修饰符中
```

### 3.2 常见 STAT 组合解读

```
STAT  │ 解读
──────┼──────────────────────────────────────────────────────────
 Ss   │ S(睡眠) + s(会话 leader) → systemd, sshd 等守护进程
 S+   │ S(睡眠) + +(前台) → 前台 shell 中等待输入的进程
 Ss+  │ S(睡眠) + s(会话 leader) + +(前台) → 前台 shell 本身
 Ssl  │ S(睡眠) + s(会话 leader) + l(多线程) → 多线程守护进程
 Sl   │ S(睡眠) + l(多线程) → 多线程工作进程
 R+   │ R(运行) + +(前台) → 正在前台运行的进程
 Rl+  │ R(运行) + l(多线程) + +(前台) → 前台运行的多线程进程
 I<   │ I(空闲) + <(高优先级) → 高优先级内核线程（如 kworker）
 I    │ I(空闲) → 普通内核线程
 D+   │ D(不可中断睡眠) + +(前台) → 前台进程卡在 I/O
 D    │ D(不可中断睡眠) → 后台进程卡在 I/O
 Z+   │ Z(僵尸) + +(前台) → 前台进程的僵尸子进程
 SN   │ S(睡眠) + N(低优先级) → nice 值 > 0 的睡眠进程
```

### 3.3 解码练习

```
看到 STAT = "Ssl"，逐位解读：
  S → 进程在可中断睡眠中
  s → 是会话 leader（通常是守护进程）
  l → 是多线程的

看到 STAT = "R+"，逐位解读：
  R → 进程正在运行或在就绪队列中
  + → 在前台进程组

看到 STAT = "I<"，逐位解读：
  I → 空闲内核线程
  < → 高优先级（不会被普通进程的 nice 影响）
```

---

## 四、D 状态深度排查

### 4.1 为什么 kill -9 杀不掉？

```
进程收到 SIGKILL 后的处理流程：

普通进程（S/R 状态）：
  SIGKILL 到达
      │
      ▼
  内核标记进程需要终止
      │
      ▼
  进程回到 R 状态 → 调度器运行 → do_exit() → 消亡

D 状态进程：
  SIGKILL 到达
      │
      ▼
  内核标记进程需要终止（pending signal）
      │
      ▼
  ★ 但进程在 TASK_UNINTERRUPTIBLE 中
     不检查信号，只等待 I/O 条件满足
      │
      ▼
  如果 I/O 永远不完成（NFS 断连、坏盘）
      │
      ▼
  进程永远卡在 D 状态 → kill -9 无效
```

### 4.2 排查步骤

```bash
# 第一步：找出所有 D 状态进程
ps -eo pid,ppid,stat,wchan,cmd | awk '$3 ~ /^D/'

# 第二步：查看等待的内核函数
cat /proc/<PID>/wchan
# 常见结果：
#   io_schedule       → 等待磁盘 I/O
#   nfs_wait_bit      → 等待 NFS 服务器
#   futex_wait        → 等待锁
#   pipe_wait         → 等待管道

# 第三步：查看内核调用栈
cat /proc/<PID>/stack
# 示例输出：
#   [<0>] io_schedule+0x12/0x40
#   [<0>] submit_bio_wait+0x61/0xb0
#   [<0>] blkdev_issue_flush+0x101/0x140
#   → 说明在等待磁盘 flush 完成

# 第四步：查看系统日志
dmesg | grep -i "blocked\|hung\|i/o error" | tail -20
# 示例：
#   INFO: task fio:12345 blocked for more than 120 seconds.
#   → 内核检测到进程卡住超过 120 秒

# 第五步：检查磁盘状态
iostat -x 1 3
# 看 await（I/O 等待时间）和 %util（磁盘利用率）
```

### 4.3 D 状态的常见原因和解决

```
原因                 │ 表现                    │ 解决方案
─────────────────────┼────────────────────────┼─────────────────────────
NFS 服务器断连        │ wchan=nfs_wait_bit      │ 恢复 NFS 连接或 umount -f
磁盘硬件故障          │ wchan=io_schedule       │ 更换磁盘，检查 dmesg
磁盘队列拥塞          │ wchan=get_request_wait  │ 减小 I/O 负载
文件系统 sync 卡住    │ wchan=io_schedule       │ 检查磁盘状态
内核死锁 (bug)       │ 多个进程互相等待        │ 重启（唯一办法）
```

---

## 五、/proc/[pid]/stat 完整字段说明

### 5.1 真实数据

```bash
$ cat /proc/1/stat
```

```
1 (systemd) S 0 1 1 0 -1 4194560 131842 96194324 100 163707 ...
```

### 5.2 字段逐一解读

```
序号  │ 字段名          │ 类型     │ 含义                          │ ps 对应列
──────┼────────────────┼──────────┼──────────────────────────────┼───────────
  1   │ pid            │ %d       │ 进程 ID                       │ PID
  2   │ comm           │ %s       │ 进程名（括号包裹）             │ CMD（短名）
  3   │ state          │ %c       │ ★ 进程状态字符 (R/S/D/Z/T/I) │ STAT
  4   │ ppid           │ %d       │ 父进程 ID                     │ PPID
  5   │ pgrp           │ %d       │ 进程组 ID                     │ PGID
  6   │ session        │ %d       │ 会话 ID                       │ SID
  7   │ tty_nr         │ %d       │ 控制终端号                    │ TTY
  8   │ tpgid          │ %d       │ 前台进程组 ID                 │ —
  9   │ flags          │ %u       │ 进程标志位                    │ —
 10   │ minflt         │ %lu      │ 次缺页次数（无需磁盘 I/O）    │ MINFLT
 11   │ cminflt        │ %lu      │ 子进程的累计次缺页            │ —
 12   │ majflt         │ %lu      │ 主缺页次数（需要磁盘 I/O）    │ MAJFLT
 13   │ cmajflt        │ %lu      │ 子进程的累计主缺页            │ —
 14   │ utime          │ %lu      │ ★ 用户态 CPU 时间 (jiffies)  │ TIME
 15   │ stime          │ %lu      │ ★ 内核态 CPU 时间 (jiffies)  │ TIME
 16   │ cutime         │ %ld      │ 子进程的用户态时间            │ —
 17   │ cstime         │ %ld      │ 子进程的内核态时间            │ —
 18   │ priority       │ %ld      │ 优先级                        │ PRI
 19   │ nice           │ %ld      │ nice 值 (-20~19)             │ NI
 20   │ num_threads    │ %ld      │ ★ 线程数                     │ NLWP
 21   │ itrealvalue    │ %ld      │ (已废弃)                      │ —
 22   │ starttime      │ %llu     │ ★ 启动时间 (jiffies)         │ START
 23   │ vsize          │ %lu      │ ★ 虚拟内存 (字节)            │ VSZ
 24   │ rss            │ %ld      │ ★ 常驻内存 (页数)            │ RSS
 25   │ rsslim         │ %lu      │ RSS 软限制                    │ —
 26   │ startcode      │ %lu      │ 代码段起始地址                │ —
 27   │ endcode        │ %lu      │ 代码段结束地址                │ —
 28   │ startstack     │ %lu      │ 栈起始地址                    │ —
 29   │ kstkesp        │ %lu      │ 内核栈指针                    │ —
 30   │ kstkeip        │ %lu      │ 内核指令指针                  │ —
 31   │ signal         │ %lu      │ (已废弃)                      │ —
 32   │ blocked        │ %lu      │ (已废弃)                      │ —
 33   │ sigignore      │ %lu      │ (已废弃)                      │ —
 34   │ sigcatch       │ %lu      │ (已废弃)                      │ —
 35   │ wchan          │ %lu      │ 等待通道（地址）              │ WCHAN（函数名）
 36   │ nswap          │ %lu      │ (已废弃)                      │ —
 37   │ cnswap         │ %lu      │ (已废弃)                      │ —
 38   │ exit_signal    │ %d       │ 退出时发送给父进程的信号      │ —
 39   │ processor      │ %d       │ ★ 最近使用的 CPU 号          │ PSR
 40   │ rt_priority    │ %u       │ 实时优先级                    │ RTPRIO
 41   │ policy         │ %u       │ ★ 调度策略                   │ CLS
 42   │ delayacct_blks │ %llu     │ 等待块设备 I/O 的时间         │ —
 43   │ guest_time     │ %lu      │ 虚拟机客户态时间              │ —
 44   │ cguest_time    │ %ld      │ 子进程的客户态时间            │ —
 45   │ start_data     │ %lu      │ 数据段起始地址                │ —
 46   │ end_data       │ %lu      │ 数据段结束地址                │ —
 47   │ start_brk      │ %lu      │ 堆起始地址                    │ —
 48   │ arg_start      │ %lu      │ 参数起始地址                  │ —
 49   │ arg_end        │ %lu      │ 参数结束地址                  │ —
 50   │ env_start      │ %lu      │ 环境变量起始地址              │ —
 51   │ env_end        │ %lu      │ 环境变量结束地址              │ —
 52   │ exit_code      │ %d       │ 退出码                        │ —
```

### 5.3 关键字段深入解读

#### state（字段 3）— 进程状态

```
字符 │ 内核常量              │ 含义
─────┼──────────────────────┼─────────────────────────
 R   │ TASK_RUNNING          │ 运行中 / 就绪
 S   │ TASK_INTERRUPTIBLE    │ 可中断睡眠
 D   │ TASK_UNINTERRUPTIBLE  │ 不可中断睡眠
 Z   │ EXIT_ZOMBIE           │ 僵尸
 T   │ TASK_STOPPED          │ 停止
 t   │ TASK_TRACED           │ 追踪停止
 X/x │ EXIT_DEAD             │ 死亡
 I   │ TASK_IDLE             │ 空闲内核线程
```

#### utime + stime（字段 14+15）— CPU 时间

```
jiffies = 内核时钟中断计数（通常 1 jiffy = 1/100 秒，即 HZ=100）

utime = 进程在用户态执行的时间（jiffies）
stime = 进程在内核态执行的时间（jiffies）

★ ps 的 %CPU 计算方式：
  %CPU = (Δ(utime + stime)) / (Δwall_time × HZ) × 100

  例：两次采样间隔 1 秒（100 jiffies）
  utime 增加了 50，stime 增加了 30
  %CPU = (50 + 30) / 100 × 100 = 80%

★ 多核系统上 %CPU 可以超过 100%：
  一个 4 线程的进程，每个线程占满一个核
  %CPU = 4 × 100 = 400%
```

#### vsize + rss（字段 23+24）— 内存

```
vsize = 虚拟内存大小（字节）
  → ps 的 VSZ 列（单位 KB）
  → 包含所有映射的内存，包括未实际使用的
  → 共享库、内存映射文件都算在内

rss = 常驻内存集大小（页数，通常 4KB/页）
  → ps 的 RSS 列（单位 KB）
  → 实际占用的物理内存
  → ★ RSS 是判断进程真实内存占用的关键指标

VSZ vs RSS 的区别：
  ┌─────────────────────────────────────┐
  │ 虚拟内存 (VSZ)                       │
  │  ┌────────────┐  ┌──────────────┐   │
  │  │ 已映射的    │  │ 已映射但未    │   │
  │  │ 物理页      │  │ 使用的页      │   │
  │  │ = RSS      │  │ (demand      │   │
  │  │            │  │  paging)     │   │
  │  └────────────┘  └──────────────┘   │
  └─────────────────────────────────────┘
```

#### policy（字段 41）— 调度策略

```
值 │ 内核常量         │ ps CLS 列 │ 含义
───┼─────────────────┼──────────┼─────────────────────────
 0 │ SCHED_NORMAL     │ TS       │ 普通分时调度（CFS）
 1 │ SCHED_FIFO       │ FF       │ 实时先进先出
 2 │ SCHED_RR         │ RR       │ 实时轮转
 3 │ SCHED_BATCH      │ B        │ 批处理（低优先级）
 5 │ SCHED_IDLE       │ #        │ 空闲时调度
 6 │ SCHED_DEADLINE   │ DL       │ 截止期调度
```

---

## 六、/proc/[pid]/status 关键字段

```
字段              │ 含义                            │ 与 ps 的关系
──────────────────┼─────────────────────────────────┼────────────────
Name              │ 进程名                          │ CMD
State             │ 状态字符 + 描述                 │ STAT
Tgid              │ 线程组 ID = 主线程 PID          │ PID
Pid               │ 进程 ID                         │ PID
PPid              │ 父进程 ID                       │ PPID
Uid               │ real/effective/saved/fs UID     │ USER
Gid               │ real/effective/saved/fs GID     │ GROUP
Threads           │ 线程数                          │ NLWP
VmPeak            │ 虚拟内存峰值                    │ —
VmSize            │ 虚拟内存当前值                  │ VSZ
VmRSS             │ 常驻内存集                      │ RSS
VmSwap            │ swap 使用量                     │ —
VmData            │ 数据段大小                      │ —
VmStk             │ 栈大小                          │ —
VmExe             │ 代码段大小                      │ —
VmLib             │ 共享库大小                      │ —
```

---

## 总结

### 进程状态调用链

```
用户执行 ps
    │
    ▼
遍历 /proc/[pid]/
    │
    ├── stat 字段 3 → state 字符（R/S/D/Z/T/I）→ STAT 首字符
    ├── stat 字段 9 → flags → 修饰字符（s/l/</N/+）
    ├── stat 字段 18 → priority → PRI
    ├── stat 字段 19 → nice → NI 列 / 修饰字符（</N）
    ├── stat 字段 20 → num_threads → NLWP / 修饰字符（l）
    ├── stat 字段 23 → vsize → VSZ
    ├── stat 字段 24 → rss → RSS
    ├── stat 字段 41 → policy → CLS
    ├── status → Uid → 查 /etc/passwd → USER
    ├── wchan → WCHAN
    └── cmdline → CMD
```

### 完整状态映射表

```
STAT 字段 │ 内核状态               │ 含义
──────────┼───────────────────────┼─────────────────────────
R         │ TASK_RUNNING          │ 运行中/就绪
S         │ TASK_INTERRUPTIBLE    │ 可中断睡眠
D         │ TASK_UNINTERRUPTIBLE  │ 不可中断睡眠（等 I/O）
Z         │ EXIT_ZOMBIE           │ 僵尸（已退出未回收）
T         │ TASK_STOPPED          │ 停止（SIGSTOP）
t         │ TASK_TRACED           │ 追踪停止（ptrace）
X         │ EXIT_DEAD             │ 死亡（瞬态）
I         │ TASK_IDLE             │ 空闲内核线程
```
