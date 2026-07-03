<!--
======================================================================
  Step 7: 自旋锁性能分析
======================================================================
  来源: Linux 内核源码 kernel/locking/、perf 文档、bpftrace 文档
  阅读要点:
    - spinlock 在高竞争下的表现
    - ticket spinlock vs queued spinlock
    - 如何识别 spinlock 热点
    - perf lock、bpftrace 追踪方法
  预计时间: 20-30 分钟
======================================================================
-->

# 自旋锁性能分析

> 自旋锁（spinlock）是内核中最基本的同步原语。在高竞争场景下，
> 自旋锁可能成为严重的性能瓶颈——CPU 在"自旋等待"中消耗大量
> 周期却不做任何有效工作。

---

## 目录

- [1. 自旋锁基础](#1-自旋锁基础)
- [2. ★ Ticket Spinlock vs Queued Spinlock](#2--ticket-spinlock-vs-queued-spinlock)
- [3. 自旋锁竞争的性能表现](#3-自旋锁竞争的性能表现)
- [4. ★ 如何识别自旋锁热点](#4--如何识别自旋锁热点)
- [5. perf lock：锁竞争分析](#5-perf-lock锁竞争分析)
- [6. bpftrace 追踪自旋锁](#6-bpftrace-追踪自旋锁)
- [7. 实战诊断流程](#7-实战诊断流程)

---

## 1. 自旋锁基础

### 1.1 自旋锁的工作原理

```
  spinlock 基本语义:

  ┌─────────────────────────────────────────────────┐
  │  spin_lock(&lock):                                │
  │    while (!atomic_try_lock(&lock))                │
  │      ; // "自旋"：忙等待，不断尝试获取锁         │
  │                                                    │
  │  spin_unlock(&lock):                              │
  │    atomic_unlock(&lock);                          │
  └─────────────────────────────────────────────────┘

  ★ 核心特征:
  - 获取锁失败时不睡眠 → CPU 持续空转
  - 适用于极短的临界区（< 几十μs）
  - 不适合长临界区 → 浪费 CPU 周期
```

### 1.2 内核中的自旋锁类型

| 锁类型 | 源码位置 | 使用场景 |
|--------|---------|---------|
| `raw_spinlock_t` | `include/linux/spinlock_types_raw.h` | 最底层，不可抢占 |
| `spinlock_t` | `include/linux/spinlock_types.h` | 通用自旋锁 |
| `rwlock_t` | `include/linux/rwlock_types.h` | 读写锁（读共享，写独占） |
| `seqlock_t` | `include/linux/seqlock.h` | 序列锁（读者无阻塞） |

```c
/* include/linux/spinlock_types_raw.h */
typedef struct raw_spinlock {
    arch_spinlock_t raw_lock;
    /* ... debug fields ... */
} raw_spinlock_t;

/*
 * ★ arch_spinlock_t 的实际实现取决于架构:
 * - x86: queued spinlock (qspinlock)
 * - ARM64: queued spinlock
 * - 旧架构: ticket spinlock 或 test-and-set
 */
```

---

## 2. ★ Ticket Spinlock vs Queued Spinlock

### 2.1 Ticket Spinlock（票号自旋锁）

```
  原理: 类似排队叫号

  ┌──────────────────────────────────────────────────────────┐
  │  结构体:                                                   │
  │  struct ticket_spinlock {                                  │
  │    u32 head;  // 当前服务的票号                             │
  │    u32 tail;  // 下一个分配的票号                           │
  │  };                                                        │
  │                                                            │
  │  获取锁:                                                   │
  │  1. my_ticket = atomic_fetch_add(&lock->tail, 1)           │
  │  2. while (my_ticket != lock->head)                        │
  │       cpu_relax();  // 忙等待                              │
  │                                                            │
  │  释放锁:                                                   │
  │  lock->head++;  // 服务下一个票号                          │
  │                                                            │
  │  队列示例:                                                  │
  │  tail=5                        head=2                      │
  │  ┌──────┬──────┬──────┐                                    │
  │  │ CPU0 │ CPU3 │ CPU7 │  ← 等待的 CPU                      │
  │  │ tk=2 │ tk=3 │ tk=4 │                                    │
  │  └──────┴──────┴──────┘                                    │
  │    ↑                                                       │
  │  正在服务 head=2 (CPU0)                                    │
  │  CPU0 释放 → head=3 → CPU3 获得锁                         │
  └──────────────────────────────────────────────────────────┘
```

**★ Ticket Spinlock 的问题：**
```
  缓存行竞争 (Cache Line Bouncing):

  所有等待的 CPU 都在轮询同一个 lock->head 变量
  → 每次 head 变化，所有 CPU 的 L1/L2 缓存中的该缓存行都失效
  → 产生大量 cache coherence traffic（MESI invalidate）

  CPU0 修改 head
  → invalidate CPU1-CPU7 缓存中的 head 副本
  → 所有 CPU 同时从 L3/DRAM 重新读取 head
  → 缓存行在核心间"弹跳"

  影响: N 个 CPU 竞争 → 每次 unlock 产生 N-1 次 cache miss
  → 竞争越激烈，cache miss 越多 → 恶性循环
```

### 2.2 ★ Queued Spinlock（队列自旋锁）— 现代 Linux 默认

```
  原理: 每个 CPU 自旋在自己的本地变量上

  ┌──────────────────────────────────────────────────────────┐
  │  每个 CPU 有自己的 MCS 节点:                                │
  │                                                            │
  │  struct mcs_spinlock {                                     │
  │    struct mcs_spinlock *next;  // 队列中的下一个节点       │
  │    u32 locked;                 // 本节点的锁状态           │
  │  };                                                        │
  │                                                            │
  │  队列结构:                                                  │
  │                                                            │
  │  lock → [CPU0.mcs] → [CPU3.mcs] → [CPU7.mcs] → NULL      │
  │           locked=0     locked=1     locked=1               │
  │           (持有锁)     (等待 CPU0)  (等待 CPU3)            │
  │                                                            │
  │  ★ 关键优势:                                                │
  │  CPU3 自旋在自己的 mcs.locked 上                            │
  │  CPU7 自旋在自己的 mcs.locked 上                            │
  │  → 各自轮询不同的缓存行 → 无 cache line bouncing!         │
  │                                                            │
  │  释放锁: CPU0 设置 CPU3.mcs.locked = 0                    │
  │  → 只有 CPU3 的缓存行失效 → 单次 cache miss               │
  └──────────────────────────────────────────────────────────┘
```

### 2.3 对比总结

| 维度 | Ticket Spinlock | Queued Spinlock |
|------|----------------|-----------------|
| 等待方式 | 所有 CPU 轮询同一变量 | 每 CPU 轮询自己的变量 |
| Cache 影响 | ★ 每次 unlock 产生 N-1 次 miss | ★ 每次 unlock 仅 1 次 miss |
| 公平性 | FIFO（先到先服务） | FIFO |
| 内存开销 | 8 字节 (head + tail) | 每 CPU 一个 MCS 节点 |
| 内核配置 | `CONFIG_TICKET_SPINLOCKS` | `CONFIG_QUEUED_SPINLOCKS` |
| 适用架构 | 旧架构 | ★ x86_64、ARM64 等现代架构 |
| 源码 | `kernel/locking/spinlock_ticket.c` | `kernel/locking/qspinlock.c` |

```
  ★ 性能差异（高竞争场景，如 8 核争同一把锁）:

  Ticket Spinlock:
  - 每次 unlock → 7 次 cache invalidation
  - 1000 次锁获取 → ~7000 次不必要的 cache miss
  - 大量时间浪费在 cache coherence traffic 上

  Queued Spinlock:
  - 每次 unlock → 1 次 cache invalidation
  - 1000 次锁获取 → ~1000 次 cache miss
  - 减少 ~85% 的 cache 流量
```

---

## 3. 自旋锁竞争的性能表现

### 3.1 ★ 宏观表现

```
  自旋锁高竞争时的系统表现:

  ┌──────────────────────────────────────────────────────┐
  │                                                        │
  │  top/htop:                                              │
  │  - CPU 利用率高 (90%+)                                 │
  │  - %sys 偏高（内核态时间）                              │
  │  - 但应用吞吐量低                                       │
  │                                                        │
  │  perf stat:                                             │
  │  - IPC 低 (< 1.0)                                      │
  │  - cycles 高但 instructions 低                         │
  │  - 大量时间花在自旋等待（无有效指令退休）               │
  │                                                        │
  │  perf record:                                           │
  │  - 热点函数中出现 _raw_spin_lock / queued_spin_lock    │
  │    _slow_path                                           │
  │                                                        │
  └──────────────────────────────────────────────────────┘
```

### 3.2 微观表现

```
  自旋等待时 CPU 在做什么:

  CPU 执行:
  ┌─────────────────────────────────────────────┐
  │  queued_spin_lock_slowpath():                 │
  │                                                │
  │  for (;;) {                                    │
  │    if (try_lock())                             │
  │      break;                                    │
  │    cpu_relax();  // PAUSE 指令                │
  │  }                                             │
  │                                                │
  │  cpu_relax() 在 x86 上是 PAUSE 指令:          │
  │  - 延迟 ~10-140 周期（取决于微架构）           │
  │  - 降低功耗（避免流水线满载空转）              │
  │  - 避免内存序违规（memory order violation）    │
  │                                                │
  │  ★ 每个 PAUSE 周期:                            │
  │  - CPU 消耗周期但不退休有效指令                │
  │  - IPC → 接近 0                                 │
  │  - 表现为"CPU 忙但无产出"                      │
  └─────────────────────────────────────────────┘
```

---

## 4. ★ 如何识别自旋锁热点

### 4.1 方法 1: perf record + 符号分析

```bash
# 录制（使用 cycles:pp 精确定位到指令级）
perf record -e cycles:pp -g -- ./program

# 查看热点函数
perf report --stdio --sort=symbol

# ★ 关注这些函数:
# queued_spin_lock_slowpath    ← 自旋锁慢路径（等待中）
# _raw_spin_lock               ← 自旋锁入口
# _raw_spin_lock_irqsave       ← 关中断 + 自旋锁
# _raw_spin_lock_bh            ← 关软中断 + 自旋锁
# native_queued_spin_lock_slowpath ← 架构特定慢路径

# 如果这些函数占比 > 5%，说明存在显著的锁竞争
```

### 4.2 方法 2: perf record + 调用链分析

```bash
# 录制带调用栈的数据
perf record -e cycles:pp -g --call-graph dwarf -- ./program

# 查看哪些代码路径触发了锁等待
perf report --stdio --sort=symbol,dso --call-graph fractal

# ★ 输出示例:
# - 15.30% _raw_spin_lock_irqsave
#    - 15.30% _raw_spin_lock_irqsave
#       - 12.10% __alloc_pages_nodemask    ← 页分配器锁
#       -  2.50% do_epoll_wait             ← epoll 锁
#       -  0.70% tcp_sendmsg               ← TCP 发送锁
#
# → 页分配器锁是最大竞争点 → 考虑 per-CPU page cache 或大页

```

### 4.3 方法 3: /proc/lock_stat

```bash
# 启用锁统计（需要内核 CONFIG_LOCK_STAT=y）
echo 1 > /proc/sys/kernel/lock_stat

# 运行工作负载
./program

# 查看锁统计
cat /proc/lock_stat

# 输出格式:
# lock-class name    con-bounces  contentions   waittime-min
#                    waittime-max waittime-total acq-bounces
#                    acquisitions holdtime-min   holdtime-max
#                    holdtime-total
#
# ★ 关键指标:
# contentions: 锁竞争次数（获取锁时发现已被持有）
# con-bounces: 竞争时的 cache bounce 次数
# waittime-total: 累计等待时间
# holdtime-total: 累计持有时间
```

---

## 5. perf lock：锁竞争分析

### 5.1 使用方法

```bash
# 记录锁事件
perf lock record -- ./program

# 分析锁竞争
perf lock report

# ★ 输出示例:
# ┌─────────────────────────────────────────────────────────┐
# │  === output for lock contention ===                       │
# │                                                            │
# │                Name   acquired  contended  avg wait (ns)  │
# │                                                            │
# │   &(&zone->lock)->rlock    50000       8000      12500   │
# │   &(&q->__queue_lock)->... 30000       5000       8000   │
# │   rcu_read_lock            80000          0          0   │
# │                                                            │
# │  ★ contended 列: 竞争次数                                 │
# │  ★ avg wait (ns): 平均等待时间                            │
# │  高 contended + 高 avg wait = 严重竞争                    │
# └─────────────────────────────────────────────────────────┘

# 按等待时间排序
perf lock report --sort=wait

# 按竞争次数排序
perf lock report --sort=acquired
```

### 5.2 perf lock 的事件类型

```bash
# 查看可用的锁事件
perf list lock

# 常见事件:
# lock:lock_acquire       — 尝试获取锁
# lock:lock_release       — 释放锁
# lock:lock_contended     — 获取锁时发现竞争（需等待）
# lock:lock_acquired      — 成功获取锁（可能经过了等待）
```

---

## 6. bpftrace 追踪自旋锁

### 6.1 追踪自旋锁等待时间

```bash
# 追踪 queued_spin_lock_slowpath 的耗时分布
bpftrace -e '
kprobe:queued_spin_lock_slowpath {
    @start[tid] = nsecs;
}

kretprobe:queued_spin_lock_slowpath /@start[tid]/ {
    $wait = nsecs - @start[tid];
    @wait_ns = hist($wait);
    @total_wait = sum($wait);
    delete(@start[tid]);
}

END {
    clear(@start);
}
'

# ★ 输出示例:
# @wait_ns:
# [100, 200)         1200 |@@@@@@@                                     |
# [200, 500)         8500 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
# [500, 1K)          3200 |@@@@@@@@@@@@@@@@@@                          |
# [1K, 2K)            800 |@@@@                                        |
# [2K, 5K)            120 |                                              |
#
# @total_wait: 4500000000  (4.5 秒累计等待)
#
# → 大部分等待在 200-500ns → 中等竞争
# → 如果出现 > 2μs 的等待 → 严重竞争
```

### 6.2 追踪锁竞争最严重的调用路径

```bash
# 找到等待时间最长的锁获取路径
bpftrace -e '
kprobe:queued_spin_lock_slowpath {
    @start[tid] = nsecs;
}

kretprobe:queued_spin_lock_slowpath /@start[tid]/ {
    $wait = nsecs - @start[tid];
    if ($wait > 1000) {  // 只记录 > 1μs 的等待
        @[kstack] = count();
    }
    delete(@start[tid]);
}
'

# ★ 输出会显示哪些内核调用路径导致了长时间锁等待
```

### 6.3 追踪每把锁的竞争情况

```bash
# 追踪特定锁地址的竞争次数
bpftrace -e '
kprobe:_raw_spin_lock {
    $lock = arg0;
    @locks[$lock] = count();
}
'

# 结合符号解析找到具体是哪把锁
# 用 ksym 查看锁地址属于哪个结构体
```

---

## 7. 实战诊断流程

### 7.1 ★ 自旋锁问题诊断决策树

```
  CPU 高利用率 + 低吞吐
       │
       ▼
  ┌─ Step 1: perf record 看热点 ────────────────────┐
  │  perf record -e cycles:pp -g -- ./program        │
  │  perf report --sort=symbol                        │
  │                                                    │
  │  _raw_spin_lock* 占比 > 5%?                       │
  │  ├── 是 → 存在锁竞争，继续 Step 2               │
  │  └── 否 → 不是锁竞争问题，转向其他分析            │
  └────────────────────────────────────────────────────┘
       │
       ▼
  ┌─ Step 2: perf lock 看竞争详情 ──────────────────┐
  │  perf lock record -- ./program                    │
  │  perf lock report --sort=wait                     │
  │                                                    │
  │  找到竞争最严重的锁:                               │
  │  - 哪把锁的 contended 最高？                       │
  │  - 平均等待时间是多少？                             │
  └────────────────────────────────────────────────────┘
       │
       ▼
  ┌─ Step 3: 分析调用链确定根因 ────────────────────┐
  │  perf report -g --sort=symbol,dso                │
  │                                                    │
  │  常见的锁竞争根因:                                 │
  │  ├── 页分配器锁 → 考虑大页 / per-CPU 缓存       │
  │  ├── 文件系统锁 → 考虑不同挂载点 / 异步 I/O     │
  │  ├── 网络栈锁   → 考虑 RPS/RFS / 多队列网卡     │
  │  ├── slab 分配器锁 → 考虑 per-CPU 缓存池        │
  │  └── 驱动锁     → 考虑驱动优化或替代方案         │
  └────────────────────────────────────────────────────┘
       │
       ▼
  ┌─ Step 4: bpftrace 深入分析 ─────────────────────┐
  │  用 bpftrace 追踪锁等待时间分布                   │
  │  用 bpftrace 追踪锁获取的调用路径                 │
  │  用 bpftrace 统计每把锁的竞争频率                 │
  └────────────────────────────────────────────────────┘
```

### 7.2 常见优化手段

| 问题 | 症状 | 优化方案 |
|------|------|---------|
| 页分配器锁竞争 | `__alloc_pages` 中的 `_raw_spin_lock` | 使用大页（减少分配次数）、per-CPU 页池 |
| slab 分配器竞争 | `kmem_cache_alloc` 中的锁 | per-CPU slab cache、批量分配 |
| 网络栈锁竞争 | `tcp_sendmsg` / `netif_receive_skb` | 多队列网卡 + RPS/RFS + XDP |
| 文件系统锁竞争 | `ext4/xfs` 内部锁 | 不同挂载点、异步 I/O、io_uring |
| RCU 回调锁 | `rcu_process_callbacks` | 调整 RCU 参数、减少 RCU 回调 |

---

## 总结

```
  ★ 自旋锁性能分析速查:

  检测:
  ├── perf record -e cycles:pp -g → 看 _raw_spin_lock 占比
  ├── perf lock record/report → 看锁竞争统计
  └── /proc/lock_stat → 看锁级别的竞争数据

  定位:
  ├── perf report -g → 看调用链，找到哪把锁
  ├── bpftrace → 追踪等待时间分布
  └── perf c2c → 看锁地址的 cache line bouncing

  优化:
  ├── 减少锁的粒度（拆分大锁）
  ├── 减少临界区长度
  ├── per-CPU 数据结构（避免共享）
  ├── 无锁数据结构（RCU、lock-free queue）
  └── 批量操作（减少锁获取次数）
```
