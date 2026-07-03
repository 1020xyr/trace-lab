# pidstat 问答集

> 学习 pidstat 过程中遇到的问题与源码级解答。

---

## Q1: pidstat 的 cswch/s 和 nvcswch/s 分别说明什么？

**日期：** 2026-07-04
**场景：** 用 `pidstat -w` 排查进程调度行为时，不清楚两种上下文切换的含义和诊断价值
**相关文件：** reading/01_pidstat_architecture.md（上下文切换数据来源）

### 回答

#### 两种上下文切换的本质区别

```
cswch/s（voluntary context switch，自愿切换）
─────────────────────────────────────────────
  进程主动调用调度器让出 CPU。

  触发条件（源码级）：
  - 进程调用 schedule() 进入 TASK_INTERRUPTIBLE/TASK_UNINTERRUPTIBLE
  - 常见场景：
    ├─ 等待 I/O 完成（read/write 阻塞在磁盘或网络）
    ├─ 等待锁/信号量（mutex_lock、down_interruptible）
    ├─ sleep/nanosleep（主动休眠）
    ├─ 等待子进程（waitpid）
    └─ 等待 epoll/poll/select 事件

  数据来源：/proc/[pid]/status → voluntary_ctxt_switches
  内核更新位置：kernel/sched/core.c → context_switch()
               当 prev->state != TASK_RUNNING 时 +1

nvcswch/s（involuntary context switch，非自愿切换）
─────────────────────────────────────────────────────
  进程被调度器强制换下 CPU（进程本身不想让出）。

  触发条件（源码级）：
  - 进程处于 TASK_RUNNING 但被调度器选中替换
  - 常见场景：
    ├─ 时间片用完（CFS 调度器的 vruntime 比较）
    ├─ 高优先级进程抢占（SCHED_FIFO/RR 实时进程唤醒）
    ├─ cgroup CPU 配额耗尽（CFS bandwidth control）
    └─ 中断返回时触发重新调度

  数据来源：/proc/[pid]/status → nonvoluntary_ctxt_switches
  内核更新位置：kernel/sched/core.c → context_switch()
               当 prev->state == TASK_RUNNING 时 +1
```

#### 诊断价值对照表

```
模式                     │ cswch/s │ nvcswch/s │ 含义              │ 行动
─────────────────────────┼─────────┼───────────┼───────────────────┼──────────────────
I/O 密集型               │ 高      │ 低        │ 进程频繁等 I/O     │ 优化 I/O 或缓存
CPU 计算 + 竞争激烈       │ 低      │ 高        │ 进程被抢占         │ 增加 CPU 或减少并发
混合型（数据库典型）      │ 高      │ 高        │ 既有 I/O 又有竞争  │ 综合优化
空闲/轻量                 │ 低      │ 低        │ 进程不活跃         │ 无需处理
锁等待严重                │ 极高    │ 低        │ 频繁等锁再被唤醒   │ 减少锁竞争
CPU 过载                  │ 中      │ 高        │ 时间片频繁被抢     │ 扩容或降负载
```

#### 实际诊断示例

```bash
# 1. 查看进程的上下文切换
pidstat -w -p <PID> 1 5

# 2. 典型输出分析
# cswch/s=5000, nvcswch/s=2
# → I/O 密集型（每秒 5000 次主动等待）
# → 结合 pidstat -d 看 kB_rd/s 和 kB_wr/s

# cswch/s=10, nvcswch/s=500
# → CPU 竞争激烈（每秒 500 次被抢占）
# → 用 mpstat -P ALL 1 看各 CPU 负载

# cswch/s=10000, nvcswch/s=0
# → 极高频自愿切换 = 频繁 sleep/wake 循环
# → 可能是 busy-wait with sleep 或锁竞争（mutex 反复尝试）
```

#### ★ 结论

- **cswch/s 高** → 进程在**等待**什么东西（I/O、锁、信号量），用 `pidstat -d` 和 `strace -p` 进一步确认
- **nvcswch/s 高** → 进程想运行但**被抢走**了 CPU，说明系统 CPU 资源不足或竞争太激烈
- **两者结合** → 可以区分 I/O 密集型、CPU 密集型、锁竞争型进程

---

## Q2: 如何用 pidstat 快速定位高 CPU 进程和线程？

**日期：** 2026-07-04
**场景：** 系统 CPU 使用率高，需要快速定位是哪个进程的哪个线程在消耗 CPU
**相关文件：** reading/02_command_reference.md（-u 和 -t 参数）

### 回答

#### 完整诊断流程

```
Step 1：找到 %CPU 最高的进程
──────────────────────────────
  pidstat -u 1 | sort -k7 -rn | head -10

  输出示例：
  11:00:01 PM   UID   PID  %usr %system %guest %wait  %CPU  CPU  Command
  11:00:01 PM  1000  1234  85.00   5.00   0.00  0.50  90.00    2  java
  11:00:01 PM     0  5678   2.00  15.00   0.00  0.00  17.00    0  fio
  
  关键字段：
  %usr=85    → java 用户态 CPU 很高（业务逻辑/计算）
  %system=5  → 内核态开销小
  %wait=0.5  → 轻微的 CPU 排队
  %CPU=90    → 总共占用约 0.9 个核

Step 2：看线程级 CPU 使用
───────────────────────────
  pidstat -u -t -p 1234 1

  输出示例：
  TGID    TID   %usr  %system  %CPU  Command
  1234      -   85.00    5.00  90.00  java           ← 进程汇总
     -   1235    0.00    0.00   0.00  |__java        ← GC 线程（空闲）
     -   1236   80.00    3.00  83.00  |__java        ← ★ 工作线程（热点！）
     -   1237    5.00    2.00   7.00  |__java        ← 辅助线程
  
  ★ TGID 行是汇总，TID 行是明细
  ★ TID=1236 占了 83% CPU → 这就是热点线程

Step 3：看该线程的上下文切换
──────────────────────────────
  pidstat -w -t -p 1234 1

  输出示例：
  TGID    TID   cswch/s  nvcswch/s  Command
  1234      -    200.00     50.00   java
     -   1236     10.00     45.00   |__java      ← 热点线程
     -   1237    180.00      5.00   |__java      ← 辅助线程
  
  TID=1236 的 nvcswch/s=45 → CPU 时间片竞争
  TID=1237 的 cswch/s=180 → 在频繁等待（I/O 或锁）

Step 4：看 I/O 排除
──────────────────────
  pidstat -d -p 1234 1
  
  如果 kB_rd/s 和 kB_wr/s 都很低 → 确认瓶颈在 CPU 不在 I/O
```

#### 快速定位命令清单

```bash
# 1. 找高 CPU 进程（实时排序）
pidstat -u 1 | sort -k7 -rn | head -10

# 2. 看特定进程的线程级 CPU
pidstat -u -t -p <PID> 1

# 3. 看线程级上下文切换
pidstat -w -t -p <PID> 1

# 4. 综合画像（一次看 CPU + I/O + 内存 + 上下文切换）
pidstat -u -d -r -w -p <PID> 1

# 5. 追踪命令执行全过程
pidstat -u -t -e <command> <args>

# 6. 按 CPU 数归一化（多核系统更直观）
pidstat -I -u 1 | sort -k7 -rn | head -10
```

#### 进阶：用 perf 做函数级分析

```bash
# pidstat 告诉你"哪个线程在忙"
# perf 告诉你"这个线程在忙什么函数"

# 对热点线程做 10 秒采样
perf record -t <TID> -g -- sleep 10

# 生成火焰图（需要 FlameGraph 工具）
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg

# 或直接用 perf report
perf report
```

#### ★ 总结

```
pidstat 定位高 CPU 的层次化方法：

系统级      pidstat -u 1 | sort -k7 -rn
  │         → 找到高 %CPU 的 PID
  ▼
进程级      pidstat -u -p <PID> 1
  │         → 看 %usr/%system/%wait 分布
  ▼
线程级      pidstat -u -t -p <PID> 1
  │         → 找到高 %CPU 的 TID
  ▼
调度级      pidstat -w -t -p <PID> 1
  │         → 看 cswch/s/nvcswch/s（等锁？竞争？）
  ▼
函数级      perf record -t <TID> -g
            → 火焰图定位热点函数
```
