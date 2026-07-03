# bpftrace 性能诊断 One-liners

> ======================================================================
> 阅读要点：
>   1. 自旋锁追踪 — 定位锁竞争热点
>   2. 软中断追踪 — 分析 ksoftirqd CPU 占用
>   3. 调度延迟追踪 — 进程排队等待时间
>   4. cache miss 关联 — 用 perf_event_open 在 bpftrace 中读取 PMC
>   5. 综合性能诊断脚本
>
> 相关源码：
>   - src/linux-5.10/kernel/locking/spinlock.c  — 自旋锁实现
>   - src/linux-5.10/kernel/softirq.c            — 软中断实现
>   - src/linux-5.10/kernel/sched/core.c         — 调度器核心
>
> 预计阅读时间：25 分钟
> ======================================================================

---

## 一、自旋锁追踪

### 1.1 原理

自旋锁竞争是"CPU 高占用低吞吐"的典型原因之一。
bpftrace 可以精确追踪锁的获取/等待/释放，定位竞争热点。

```
自旋锁竞争的数据流：

  线程 A → _raw_spin_lock() → 获取成功 → 临界区 → _raw_spin_unlock()
  线程 B → _raw_spin_lock() → ★ 自旋等待 → ... → 获取成功
                                  ↑
                              bpftrace 在这里记录：
                              等待时间 + 调用栈 + 进程信息
```

### 1.2 自旋锁热点 one-liners

```bash
# ★ 1. 统计自旋锁的等待时间分布（按调用栈）
bpftrace -e '
  kprobe:_raw_spin_lock { @lock_start[tid] = nsecs; }
  kretprobe:_raw_spin_lock /@lock_start[tid]/ {
    @wait_us = hist((nsecs - @lock_start[tid]) / 1000);
    delete(@lock_start[tid]);
  }'

# ★ 2. 找出哪些调用栈最频繁获取自旋锁
bpftrace -e '
  kprobe:_raw_spin_lock { @[kstack] = count(); }'

# ★ 3. 自旋锁持有时间分布（找出长持有者）
bpftrace -e '
  kprobe:_raw_spin_lock { @lock_acq[tid] = nsecs; }
  kprobe:_raw_spin_unlock /@lock_acq[tid]/ {
    @hold_us = hist((nsecs - @lock_acq[tid]) / 1000);
    delete(@lock_acq[tid]);
  }'

# 4. 按进程统计自旋锁等待
bpftrace -e '
  kprobe:_raw_spin_lock { @lock_start[tid] = nsecs; }
  kretprobe:_raw_spin_lock /@lock_start[tid]/ {
    @wait_us[comm] = hist((nsecs - @lock_start[tid]) / 1000);
    delete(@lock_start[tid]);
  }'

# ★ 5. 追踪所有类型的锁（spinlock + mutex + rwlock）
bpftrace -e '
  kprobe:_raw_spin_lock { @spin[tid] = nsecs; }
  kretprobe:_raw_spin_lock /@spin[tid]/ {
    @spinlock_us = hist((nsecs - @spin[tid]) / 1000);
    delete(@spin[tid]);
  }
  kprobe:mutex_lock { @mtx[tid] = nsecs; }
  kretprobe:mutex_lock /@mtx[tid]/ {
    @mutex_us = hist((nsecs - @mtx[tid]) / 1000);
    delete(@mtx[tid]);
  }'
```

### 1.3 输出解读

```
@wait_us:
[0]                 1200 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[1]                  350 |@@@@@@@@@@@@@@                                      |
[2, 4)               180 |@@@@@@@                                             |
[4, 8)                45 |@                                                   |
[8, 16)               12 |                                                    |
[16, 32)               3 |                                                    |
[32, 64)               1 |                                                    |
[64, 128)              0 |                                                    |
[128, 256)             1 |                                                    |

★ 解读：
  [0] = 1200 次 → 1200 次获取锁时几乎无等待（锁未被持有）
  [128, 256) = 1 次 → 有一次等待了 128~256μs → ★ 严重的锁争用
  大多数 < 1μs → 锁竞争不严重
```

---

## 二、软中断追踪

### 2.1 原理

软中断（softirq）是内核处理延迟敏感任务（网络包、定时器、RCU 等）
的机制。当软中断处理时间过长，会导致：
- ksoftirqd 线程 CPU 占用高
- 用户态进程被抢占
- 网络包延迟增加

```
软中断处理流程：

  硬中断（网卡收到包）
      │
      ├── raise_softirq(NET_RX_SOFTIRQ)
      │     → 标记本 CPU 有软中断待处理
      │
      ├── 硬中断返回时检查
      │     ├── 在进程上下文？→ __do_softirq() 立即处理
      │     └── 在中断上下文？→ 唤醒 ksoftirqd 线程处理
      │
      ▼
  __do_softirq()
      │
      ├── net_rx_action()      → 处理网络包
      ├── timer_softirq()      → 处理定时器
      ├── rcu_process_callbacks() → RCU 回调
      └── ...

  ★ ksoftirqd CPU 占用高 → 说明软中断处理时间过长
    → 通常是网络包处理瓶颈
```

### 2.2 软中断 one-liners

```bash
# ★ 1. 软中断处理时间分布
bpftrace -e '
  kprobe:__do_softirq { @start = nsecs; }
  kretprobe:__do_softirq /@start/ {
    @softirq_us = hist((nsecs - @start) / 1000);
    @start = 0;
  }'

# ★ 2. 按 CPU 统计软中断频率
bpftrace -e '
  kprobe:__do_softirq { @[cpu] = count(); }'

# ★ 3. 按 CPU 统计软中断处理时间
bpftrace -e '
  kprobe:__do_softirq { @start[cpu] = nsecs; }
  kretprobe:__do_softirq /@start[cpu]/ {
    @total_us[cpu] = sum((nsecs - @start[cpu]) / 1000);
    @max_us[cpu] = max((nsecs - @start[cpu]) / 1000);
    @count[cpu] = count();
    delete(@start[cpu]);
  }'

# 4. 追踪 ksoftirqd 的 CPU 占用
bpftrace -e '
  tracepoint:sched:sched_stat_runtime {
    if (str(args->comm) == "ksoftirqd") {
      @runtime_ns[args->cpu] = sum(args->runtime);
    }
  }'

# 5. 网络软中断（NET_RX）详细追踪
bpftrace -e '
  kprobe:net_rx_action { @start = nsecs; }
  kretprobe:net_rx_action /@start/ {
    @net_rx_us = hist((nsecs - @start) / 1000);
    @start = 0;
  }'
```

### 2.3 输出解读

```
@softirq_us:
[1]                  800 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[2, 4)               300 |@@@@@@@@@@@@@@@                                     |
[4, 8)               100 |@@@@@                                               |
[8, 16)               30 |@                                                   |
[16, 32)               5 |                                                    |
[32, 64)               2 |                                                    |
[64, 128)              1 |                                                    |

★ 解读：
  大多数软中断在 1~4μs 内完成（正常）
  有少量 64~128μs 的 → 可能是网络包突发导致的
  如果 > 1ms 频繁出现 → 严重问题，需要优化网络栈
```

---

## 三、调度延迟追踪

### 3.1 原理

调度延迟（scheduling latency）是进程从"可运行"到"真正运行"
之间的等待时间。高调度延迟意味着进程在运行队列中排队太久。

```
进程调度延迟：

  进程变为可运行（sched_wakeup / sched_switch 选中）
      │
      ├── 加入运行队列（runqueue）
      │     → 等待时间开始
      │
      ▼
  调度器选择该进程（finish_task_switch / sched_switch）
      │
      └── 等待时间结束
          → 调度延迟 = 选择时间 - 加入队列时间

★ 调度延迟高 = runq-sz 大 = 进程排队
  → sar -q 可以看到 runq-sz
  → bpftrace 可以看到精确的每进程调度延迟
```

### 3.2 调度延迟 one-liners

```bash
# ★ 1. 调度延迟直方图（基于 sched tracepoint）
bpftrace -e '
  tracepoint:sched:sched_wakeup { @queuetime[args->pid] = nsecs; }
  tracepoint:sched:sched_switch /@queuetime[args->next_pid]/ {
    @sched_lat_us = hist((nsecs - @queuetime[args->next_pid]) / 1000);
    delete(@queuetime[args->next_pid]);
  }'

# ★ 2. 按进程统计调度延迟
bpftrace -e '
  tracepoint:sched:sched_wakeup { @queuetime[args->pid] = nsecs; }
  tracepoint:sched:sched_switch /@queuetime[args->next_pid]/ {
    $lat = (nsecs - @queuetime[args->next_pid]) / 1000;
    @lat_us[args->next_comm] = hist($lat);
    @max_us[args->next_comm] = max($lat);
    delete(@queuetime[args->next_pid]);
  }'

# 3. 各 CPU 运行队列长度实时统计
bpftrace -e '
  tracepoint:sched:sched_switch {
    if (args->prev_state == 1) {
      @runnable[cpu]++;
    }
    if (args->next_pid != 0) {
      @runnable[cpu]--;
    }
  }'

# 4. 进程 off-CPU 时间（被调度出去的总时间）
bpftrace -e '
  tracepoint:sched:sched_switch /args->prev_pid > 0/ {
    @offcpu[args->prev_pid] = nsecs;
  }
  tracepoint:sched:sched_wakeup /@offcpu[args->pid]/ {
    @offcpu_us[args->comm] = hist((nsecs - @offcpu[args->pid]) / 1000);
    delete(@offcpu[args->pid]);
  }'
```

---

## 四、Cache Miss 关联分析

### 4.1 原理

bpftrace 本身不能直接读取 PMC（硬件性能计数器），但可以通过
`hardware` probe 类型利用 perf_event_open 来采样硬件事件。

```bash
# ★ 使用 hardware probe 在 bpftrace 中读取 PMC
# 需要物理机（VM 中 PMC 不可用）

# 1. cache-miss 采样（每 10000 次 cache miss 触发一次）
bpftrace -e '
  hardware:cache-misses:10000 {
    @[comm, kstack(5)] = count();
  }'

# 2. 按 CPU 统计 cache-miss 频率
bpftrace -e '
  hardware:cache-misses:10000 {
    @[cpu] = count();
  }'

# 3. branch-miss 采样
bpftrace -e '
  hardware:branch-misses:1000 {
    @[comm, kstack(3)] = count();
  }'

# 4. 同时追踪 cache-miss 和调度事件
bpftrace -e '
  hardware:cache-misses:10000 {
    @cache_miss[comm] = count();
  }
  tracepoint:sched:sched_switch {
    @ctx_switch[comm] = count();
  }'
```

### 4.2 限制说明

```
★ hardware probe 的限制：
  - 需要物理机（VM 中 PMC 通常不可用）
  - 采样频率受 PMC 多路复用影响
  - 不如 perf stat 精确（perf stat 用计数模式，hardware probe 用采样模式）
  - 适合定位"哪个函数/cache line 产生 miss"，不适合精确统计 miss 率

★ 推荐组合：
  perf stat → 精确统计 cache-miss 率
  perf c2c → false sharing 检测
  bpftrace hardware probe → 按进程/调用栈关联 cache-miss
```

---

## 五、综合性能诊断脚本

### 5.1 一键性能诊断模板

```bash
#!/usr/bin/bpftrace
/*
 * perf_diag.bt — 综合性能诊断
 * 同时追踪：锁竞争、软中断、调度延迟、CPU 采样
 *
 * 用法：bpftrace perf_diag.bt
 *       bpftrace perf_diag.bt -p <PID>
 */

BEGIN
{
    printf("=== 综合性能诊断开始 (Ctrl+C 结束) ===\n\n");
}

/* 1. CPU 采样 — 谁在用 CPU */
profile:hz:99
{
    @cpu_usage[comm] = count();
}

/* 2. 自旋锁等待 */
kprobe:_raw_spin_lock { @spin_start[tid] = nsecs; }
kretprobe:_raw_spin_lock /@spin_start[tid]/ {
    $lat = (nsecs - @spin_start[tid]) / 1000;
    @spinlock_us = hist($lat);
    if ($lat > 10) {
        @spinlock_hot[kstack(3)] = count();
    }
    delete(@spin_start[tid]);
}

/* 3. 软中断处理时间 */
kprobe:__do_softirq { @si_start[cpu] = nsecs; }
kretprobe:__do_softirq /@si_start[cpu]/ {
    @softirq_us[cpu] = hist((nsecs - @si_start[cpu]) / 1000);
    delete(@si_start[cpu]);
}

/* 4. 调度延迟 */
tracepoint:sched:sched_wakeup { @qtime[args->pid] = nsecs; }
tracepoint:sched:sched_switch /@qtime[args->next_pid]/ {
    @sched_lat_us = hist((nsecs - @qtime[args->next_pid]) / 1000);
    delete(@qtime[args->next_pid]);
}

/* 5. 上下文切换统计 */
tracepoint:sched:sched_switch {
    @ctx_switch[comm] = count();
}

END
{
    printf("\n=== CPU 使用 Top 10 ===\n");
    print(@cpu_usage, 10);

    printf("\n=== 自旋锁等待分布 ===\n");
    print(@spinlock_us);

    printf("\n=== 自旋锁热点 (等待 > 10μs) ===\n");
    print(@spinlock_hot, 10);

    printf("\n=== 软中断处理时间 (per CPU) ===\n");
    print(@softirq_us);

    printf("\n=== 调度延迟分布 ===\n");
    print(@sched_lat_us);

    printf("\n=== 上下文切换 Top 10 ===\n");
    print(@ctx_switch, 10);
}
```

---

## 六、诊断决策速查

```
CPU 高占用低吞吐
    │
    ├── 自旋锁竞争？
    │   bpftrace -e 'kprobe:_raw_spin_lock { @[kstack] = count(); }'
    │   → 看 native_queued_spin_lock_slowpath 是否在 top
    │
    ├── 软中断占用高？
    │   sar -u ALL → %soft 高
    │   bpftrace -e 'kprobe:__do_softirq { @start=nsecs; }
    │     kretprobe:__do_softirq /@start/ { @us=hist((nsecs-@start)/1000); }'
    │   → 看处理时间分布
    │
    ├── 调度延迟高？
    │   sar -q → runq-sz > CPU 核数
    │   bpftrace -e 'tracepoint:sched:sched_wakeup {@q[pid]=nsecs;}
    │     tracepoint:sched:sched_switch /@q[args->next_pid]/ {
    │       @us=hist((nsecs-@q[args->next_pid])/1000); }'
    │   → 看调度延迟分布
    │
    └── cache miss 高？
        perf stat -d → cache-miss 率
        bpftrace -e 'hardware:cache-misses:10000 { @[comm,kstack]=count(); }'
        → 关联 cache miss 到调用栈
```

---

## 总结

### 性能诊断 One-liner 速查

```
诊断目标          │ bpftrace 命令
─────────────────┼────────────────────────────────────────────────────
自旋锁热点        │ kprobe:_raw_spin_lock { @[kstack] = count(); }
自旋锁等待分布    │ kprobe/kretprobe:_raw_spin_lock + hist()
软中断处理时间    │ kprobe/kretprobe:__do_softirq + hist()
软中断 per-CPU    │ kprobe:__do_softirq { @[cpu] = count(); }
调度延迟分布      │ sched_wakeup + sched_switch + hist()
调度延迟 per 进程 │ sched_wakeup + sched_switch + hist by comm
cache-miss 关联   │ hardware:cache-misses:10000 { @[comm] = count(); }
综合诊断          │ 使用 perf_diag.bt 脚本模板
```
