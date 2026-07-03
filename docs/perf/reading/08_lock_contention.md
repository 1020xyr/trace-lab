# perf lock — 锁竞争分析与自旋锁热点定位

> ======================================================================
> 阅读要点：
>   1. 内核锁的类型与性能影响
>   2. perf lock record + report 的使用和输出解读
>   3. 自旋锁热点定位
>   4. 锁竞争的优化策略
>
> 相关源码：
>   - src/linux-5.10/tools/perf/builtin-lock.c — perf lock 实现
>   - src/linux-5.10/kernel/locking/qspinlock.c — queued spinlock 慢路径
>   - src/linux-5.10/include/asm-generic/qspinlock.h — queued spinlock 快速路径
>   - src/linux-5.10/include/asm-generic/qspinlock_types.h — qspinlock 数据结构
>   - src/linux-5.10/kernel/locking/mcs_spinlock.h — MCS 队列节点
>   - src/linux-5.10/include/asm-generic/qrwlock.h — 队列读写锁
>   - src/linux-5.10/kernel/locking/spinlock.c — 自旋锁入口
>
> 预计阅读时间：25 分钟
> ======================================================================

---

## 一、内核锁的类型

### 1.1 锁类型与性能特征

```
锁类型              │ 等待行为             │ 适用场景          │ 性能影响
───────────────────┼─────────────────────┼─────────────────┼──────────────
spinlock           │ ★ 忙等（自旋）       │ 短临界区          │ ★ 高 CPU 消耗
                   │ while(locked);       │ （< 几十微秒）    │ ★ 不释放 CPU
rwlock             │ ★ 读不互斥，写互斥     │ ★ 读多写少          │ 中等
mutex              │ ★ 睡眠等待             │ 长临界区          │ 低 CPU 消耗
                   │ schedule()           │ （可睡眠上下文）  │ ★ 但可能阻塞
semaphore          │ ★ 计数型睡眠等待       │ ★ 资源池            │ 中等
rcu                │ ★ 无锁读，延迟释放     │ ★ 高频读            │ ★ 读端零开销
```

#### ★ 源码：qspinlock 数据结构与位域布局

```c
/* 源码位置：src/linux-5.10/include/asm-generic/qspinlock_types.h:14-44 */

typedef struct qspinlock {
    union {
        atomic_t val;          /* ★ 整个锁只有 4 字节（一个 u32） */

#ifdef __LITTLE_ENDIAN
        struct {
            u8  locked;        /* 字节 0：锁定标志（0 或 1） */
            u8  pending;       /* 字节 1：pending 位（有人正在等锁交接） */
        };
        struct {
            u16 locked_pending;/* 低 16 位：locked + pending 合并访问 */
            u16 tail;          /* 高 16 位：队列尾部编码（cpu + 嵌套层级） */
        };
#endif
    };
} arch_spinlock_t;

/*
 * ★ 位域分布（NR_CPUS < 16K 时）：
 *
 *  bit 31                              bit 0
 *  ┌──────────────┬──────┬─────────┬────────┐
 *  │ tail cpu(+1) │ idx  │ pending │ locked │
 *  │  14 bits     │2 bits│ 8 bits  │ 8 bits │
 *  └──────────────┴──────┴─────────┴────────┘
 *
 *  - locked:  锁是否被持有（0/1）
 *  - pending: 有等待者正在"乐观自旋"等待交接
 *  - tail:    MCS 队列尾部的 CPU 编号 + 嵌套层级
 *
 * ★ 设计精髓：把 tail pointer + next->locked 压缩进一个 u32，
 *   使得 spinlock_t 保持 4 字节大小，兼容已有 API。
 */
```

```c
/* 源码位置：src/linux-5.10/kernel/locking/mcs_spinlock.h:18-22 */

/* MCS 队列节点 —— queued spinlock 的基础数据结构 */
struct mcs_spinlock {
    struct mcs_spinlock *next;  /* 指向队列中下一个等待者 */
    int locked;                 /* ★ 1 = 获得锁，0 = 等待中 */
    int count;                  /* 嵌套计数（task/softirq/hardirq/nmi） */
};

/*
 * ★ MCS 锁的核心思想：
 *   每个等待者自旋在【自己的本地变量】node->locked 上，
 *   而非反复读取【共享的】锁变量 → 避免 cache bouncing。
 *
 *   CPU 0 持锁 → node0.locked = 1
 *   CPU 1 等待 → 自旋在 node1.locked（本地缓存行）
 *   CPU 2 等待 → 自旋在 node2.locked（本地缓存行）
 *
 *   解锁时：CPU 0 设置 node1.locked = 1 → CPU 1 获得锁
 *   CPU 1 解锁时：设置 node2.locked = 1 → CPU 2 获得锁
 */
```

### 1.2 自旋锁的性能问题

```
自旋锁等待时：
  CPU 0 持有锁 → CPU 1 自旋等待（while 循环）→ CPU 1 100% 占用但无产出
  CPU 0 持有锁 → CPU 2 自旋等待 → CPU 2 100% 占用但无产出

  ★ 症状：
    - ★ CPU 使用率高（%system 高）
    - ★ 但吞吐低（线程在忙等，不是在做有用的工作）
    - ★ perf top 看到 native_queued_spin_lock_slowpath 占用高

  ★ 这就是"CPU 高占用低吞吐"的经典场景之一！
```

#### ★ 源码：queued spinlock 快速路径（无争用）

```c
/* 源码位置：src/linux-5.10/include/asm-generic/qspinlock.h:78-87 */

static __always_inline void queued_spin_lock(struct qspinlock *lock)
{
    u32 val = 0;

    /* ★ 快速路径：用 CAS 尝试把 0（未锁定）改为 1（锁定）
     *   如果成功 → 直接返回，无任何等待
     *   如果失败 → 说明有争用，进入慢路径 */
    if (likely(atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL)))
        return;

    /* ★ 慢路径：有竞争者，需要排队等待 */
    queued_spin_lock_slowpath(lock, val);
}
```

> **关键理解**：`perf top` 中看到的 `native_queued_spin_lock_slowpath` 就是从这里调用的——
> 快速路径（CAS 成功）不会出现在采样中，只有进入慢路径才会被统计到。

---

## 二、perf lock 使用方法

### 2.1 采集

```bash
# 采集锁事件（全系统 10 秒）
perf lock record -a sleep 10

# 采集特定程序
perf lock record ./app

# 采集特定 PID
perf lock record -p <PID> sleep 10

# 底层使用 lock:* tracepoint
perf record -e 'lock:*' -a sleep 10
```

### 2.2 分析

```bash
# 默认报告（按等待时间排序）
perf lock report

# 按锁地址排序
perf lock report -s address

# 按调用者排序
perf lock report -s caller

# 按锁名排序
perf lock report -s name

# 纯文本输出
perf lock report --stdio
```

---

## 三、perf lock report 输出解读

### 3.1 锁竞争概览

```
perf lock report 典型输出：

=== output for thread 12345 ===

                acquired    contended   avg wait(ns)   total wait(ns)   max wait(ns)
rcu_read_lock     150000       0          0               0                0
&dev->lock         8500      230       1520           349600            45200
&q->lock           3200      890       8900          7921000           125000
&cpu_hotplug       1200       45       3200           144000            28000

逐列解读：
  acquired        → ★ 获取锁的总次数
  contended       → ★★ 争用次数（获取时需要等待的次数）
  avg wait(ns)    → ★ 平均等待时间（纳秒）
  total wait(ns)  → ★ 总等待时间（纳秒）→ 浪费的 CPU 时间
  max wait(ns)    → 最大单次等待时间

★ 关键指标：
  - contended / acquired = 争用率
    ★ > 5% → 有明显竞争
    ★ > 20% → 严重竞争
  - ★ total wait 大 → 大量 CPU 时间在等锁
  - ★ max wait 大 → 可能有锁持有时间过长的情况
```

### 3.2 按调用栈分析

```bash
# 查看哪个调用路径导致最多的锁竞争
perf lock report -s caller --stdio
```

```
=== acquired at:

  native_queued_spin_lock_slowpath
  _raw_spin_lock_irqsave
  tcp_sendmsg
  inet_sendmsg
  sock_sendmsg
  __sys_sendto

    acquired: 5200   contended: 890   avg wait: 4500ns   total: 4005000ns

★ 这条调用栈说明：
  - TCP 发送路径中有锁竞争
  - tcp_sendmsg → _raw_spin_lock_irqsave 是争用热点
  - 5200 次获取中 890 次有争用（17% 争用率）
```

---

## 四、自旋锁热点定位

### 4.1 快速定位自旋锁热点

```bash
# 方法 1：perf top 实时查看
perf top -a
# 如果看到以下函数排名靠前 → 自旋锁竞争严重：
#   native_queued_spin_lock_slowpath
#   _raw_spin_lock
#   _raw_spin_lock_irqsave
#   do_raw_spin_lock

# 方法 2：perf record 采样
perf record -a -g -F 99 sleep 5
perf report --stdio --no-children
# 查找 [k] native_queued_spin_lock_slowpath 的占比
# ★ > 5% → 有显著的锁竞争

# 方法 3：perf lock 精确分析
perf lock record -a sleep 10
perf lock report --stdio
# 查看 contended 最高的锁
```

#### ★ 源码：queued_spin_lock_slowpath() —— 自旋锁慢路径全貌

这就是 `perf top` 中频繁出现的 `native_queued_spin_lock_slowpath` 的实现。
理解这段代码才能理解为什么自旋锁竞争会导致高 CPU 消耗。

```c
/* 源码位置：src/linux-5.10/kernel/locking/qspinlock.c:315-561 */

/*
 * ★ 状态转换图（lock 的 32-bit 值中各字段的变化）：
 *   格式：(tail, pending, locked)
 *
 *   无争用：(0,0,0) → (0,0,1)  快速路径直接获取
 *   pending：(0,1,1) → (0,1,0) → (0,0,1)  乐观等待交接
 *   排队：  (n,x,y) → (n,0,0) → (*,0,1)   MCS 队列等待
 */
void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
    struct mcs_spinlock *prev, *next, *node;
    u32 old, tail;
    int idx;

    /* ── 阶段 1：等待 pending → locked 的交接 ── */
    if (val == _Q_PENDING_VAL) {
        /* ★ 有人在把 pending 转成 locked，短暂自旋等待交接完成
         *   _Q_PENDING_LOOPS 默认为 1，避免无限自旋 */
        int cnt = _Q_PENDING_LOOPS;
        val = atomic_cond_read_relaxed(&lock->val,
                       (VAL != _Q_PENDING_VAL) || !cnt--);
    }

    /* ── 阶段 2：检测到已有争用 → 直接入队 ── */
    if (val & ~_Q_LOCKED_MASK)
        goto queue;   /* ★ 不止 locked 位被设置 → 已有排队者 */

    /* ── 阶段 3：乐观尝试 —— 设置 pending 位 ── */
    /* 0,0,* → 0,1,*  成为 "pending" 等待者（不入队，只自旋等交接） */
    val = queued_fetch_set_pending_acquire(lock);

    if (unlikely(val & ~_Q_LOCKED_MASK)) {
        /* ★ 设置 pending 后发现已有其他竞争者 → 撤销 pending，入队 */
        if (!(val & _Q_PENDING_MASK))
            clear_pending(lock);
        goto queue;
    }

    /* ── 阶段 4：作为 pending 等待者，等锁释放 ── */
    if (val & _Q_LOCKED_MASK)
        /* ★ 自旋等待当前持有者释放锁（load-acquire 语义） */
        atomic_cond_read_acquire(&lock->val, !(VAL & _Q_LOCKED_MASK));

    /* 拿到锁，清除 pending，设置 locked */
    clear_pending_set_locked(lock);
    return;

    /* ══════════════════════════════════════════════════════════
     * ── 阶段 5：MCS 排队（真正的争用路径） ──
     * ★ 这是 CPU 消耗最大的部分，也是 perf 采样命中最多的位置
     * ══════════════════════════════════════════════════════════ */
queue:
    /* ★ 获取当前 CPU 的 per-CPU 队列节点 */
    node = this_cpu_ptr(&qnodes[0].mcs);
    idx = node->count++;
    tail = encode_tail(smp_processor_id(), idx);

    /* 嵌套超过 4 层（task/softirq/hardirq/nmi）→ 退化为简单自旋 */
    if (unlikely(idx >= MAX_NODES)) {
        while (!queued_spin_trylock(lock))
            cpu_relax();        /* ★ 最坏情况：纯粹的忙等 */
        goto release;
    }

    node = grab_mcs_node(node, idx);
    barrier();
    node->locked = 0;     /* ★ 初始化为"未获得锁" */
    node->next = NULL;

    /* 再试一次 trylock（也许锁刚好释放了） */
    if (queued_spin_trylock(lock))
        goto release;

    smp_wmb();  /* 确保节点初始化在发布 tail 之前完成 */

    /* ★ 把本 CPU 的节点发布到队列尾部（原子交换） */
    old = xchg_tail(lock, tail);
    next = NULL;

    if (old & _Q_TAIL_MASK) {
        /* ★ 队列中有前驱 → 把自己链接到前驱后面，然后自旋等待 */
        prev = decode_tail(old);
        WRITE_ONCE(prev->next, node);    /* 链接到前驱节点 */

        /* ★★ 自旋在自己的 node->locked 上（本地缓存行，无 cache bouncing）
         *   展开为 smp_cond_load_acquire(&node->locked, VAL)
         *   即：while (node->locked == 0) cpu_relax();
         *   这就是 perf 采样中看到的 CPU 消耗来源 */
        arch_mcs_spin_lock_contended(&node->locked);

        next = READ_ONCE(node->next);
        if (next)
            prefetchw(next);
    }

    /* ── 阶段 6：到达队头，等待锁真正移交 ── */
    /* ★ 等待锁变为 unlocked 且无 pending */
    val = atomic_cond_read_acquire(&lock->val,
                   !(VAL & _Q_LOCKED_PENDING_MASK));

    /* ★ 获取锁！ */
    if ((val & _Q_TAIL_MASK) == tail) {
        /* 队列中只有我一个 → 直接清除 tail 并获取锁（无争用快速释放） */
        if (atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCKED_VAL))
            goto release;
    }

    /* 队列中还有后继者 → 设置 locked，然后唤醒后继 */
    set_locked(lock);

    if (!next)
        next = smp_cond_load_relaxed(&node->next, (VAL));

    /* ★ 把锁传递给队列中的下一个等待者 */
    arch_mcs_spin_unlock_contended(&next->locked);
    /* ↑ 展开为 smp_store_release(&next->locked, 1)
     *   下一个 CPU 的 arch_mcs_spin_lock_contended() 循环将退出 */

release:
    __this_cpu_dec(qnodes[0].mcs.count);
}
```

> **性能关键路径总结**：
>
> | 阶段 | 行为 | CPU 开销 | perf 可见度 |
> |------|------|---------|------------|
> | 快速路径 | CAS 成功直接返回 | 极低 | 不可见 |
> | pending 等待 | 自旋等 locked 位清除 | 中等 | 较低 |
> | MCS 排队 | 自旋在本地 `node->locked` | ★ 高 | ★★ 高（采样热点） |
> | 嵌套超限退化 | 纯 `cpu_relax()` 忙等 | ★★ 最高 | ★★ 高 |

#### ★ 源码：MCS 节点自旋等待的底层实现

```c
/* 源码位置：src/linux-5.10/kernel/locking/mcs_spinlock.h:24-46 */

/*
 * ★ arch_mcs_spin_lock_contended —— 等待者自旋的核心宏
 *
 * 默认实现使用 smp_cond_load_acquire()：
 *   - 语义：while (*l == 0) cpu_relax();  + acquire 内存屏障
 *   - ARM64 等平台会替换为 wfe（Wait For Event）以降低功耗
 */
#ifndef arch_mcs_spin_lock_contended
#define arch_mcs_spin_lock_contended(l)         \
do {                                            \
    smp_cond_load_acquire(l, VAL);             \
    /* ★ 循环直到 VAL（即 *l）变为非零 */       \
} while (0)
#endif

/*
 * ★ arch_mcs_spin_unlock_contended —— 持锁者释放锁的传递
 *
 * smp_store_release() 提供 release 语义，
 * 确保临界区内的所有操作在解锁之前完成。
 */
#ifndef arch_mcs_spin_unlock_contended
#define arch_mcs_spin_unlock_contended(l)       \
    smp_store_release((l), 1)
    /* ★ 将下一个等待者的 node->locked 设为 1 → 唤醒它 */
#endif
```

### 4.2 锁热点诊断流程

```
发现 %system 高 + 吞吐低
    │
    ├── perf top → 看到 spin_lock 函数排名靠前
    │   │
    │   └── perf lock record → perf lock report
    │       │
    │       ├── 哪个锁争用最严重？（contended 最高）
    │       │   │
    │       │   └── 哪个调用路径触发的？（-s caller）
    │       │       │
    │       │       └── 能否减小临界区？
    │       │           ├── 能 → 重构代码
    │       │           └── 不能 → 换锁类型（RCU/无锁）
    │       │
    │       └── 多个锁都有争用？
    │           └── 可能需要整体架构优化
    │               （减少共享状态、per-CPU 化）
    │
    └── perf top 没看到 spin_lock
        └── 锁竞争不是主要原因，检查其他方向
```

---

## 五、常见问题与优化

### 5.1 锁竞争优化策略

```
策略                    │ 适用场景                │ 复杂度
───────────────────────┼───────────────────────┼──────────
★ 减小临界区              │ 通用                    │ 低
★ 读写锁替代互斥锁        │ 读多写少                │ 低
★ per-CPU 变量            │ 聚合计数/统计           │ 中
★ RCU 替代读写锁          │ 高频读、低频更新        │ 高
★ 无锁数据结构            │ 特定场景（队列/计数器） │ 高
★ 细粒度锁                │ 大锁拆分为多个小锁      │ 中
```

#### ★ 源码：qrwlock 读写锁 —— 读不互斥的实现

```c
/* 源码位置：src/linux-5.10/include/asm-generic/qrwlock_types.h:13-27 */

/*
 * ★ 队列读写锁的数据结构
 *   由一个 32-bit 计数器 + 一个 qspinlock 组成
 */
typedef struct qrwlock {
    union {
        atomic_t cnts;          /* ★ 读写状态计数器 */
        struct {
#ifdef __LITTLE_ENDIAN
            u8 wlocked;         /* 字节 0：写锁标志（0 或 0xff） */
            u8 __lstate[3];     /* 字节 1-3：读锁计数 */
#endif
        };
    };
    arch_spinlock_t wait_lock;  /* 内部的自旋锁（写者排队用） */
} arch_rwlock_t;

/*
 * ★ 位域含义：
 *   _QW_LOCKED  = 0x0ff  → 写锁被持有
 *   _QW_WAITING = 0x100  → 有写者在等待
 *   _QR_SHIFT   = 9      → 读者计数从 bit 9 开始
 *   _QR_BIAS    = (1<<9) → 每个读者原子加 0x200
 *
 *   读锁快速路径：原子加 _QR_BIAS，检查无写者 → 直接返回
 *   写锁快速路径：CAS 把 0 改为 _QW_LOCKED → 直接返回
 */
```

```c
/* 源码位置：src/linux-5.10/include/asm-generic/qrwlock.h:72-82 */

/*
 * ★ 读锁获取 —— 快速路径（无写者时零开销）
 */
static inline void queued_read_lock(struct qrwlock *lock)
{
    u32 cnts;

    /* ★ 原子增加读者计数（_QR_BIAS = 1 << 9） */
    cnts = atomic_add_return_acquire(_QR_BIAS, &lock->cnts);
    if (likely(!(cnts & _QW_WMASK)))
        return;   /* ★ 无写者 → 直接获取读锁，多个读者可并行 */

    /* 有写者 → 进入慢路径（可能需要等待写者释放） */
    queued_read_lock_slowpath(lock);
}
```

```c
/* 源码位置：src/linux-5.10/include/asm-generic/qrwlock.h:88-96 */

/*
 * ★ 写锁获取 —— 必须等待所有读者释放
 */
static inline void queued_write_lock(struct qrwlock *lock)
{
    u32 cnts = 0;
    /* ★ 快速路径：CAS 把 0 改为 _QW_LOCKED
     *   成功条件：当前无读者且无写者 */
    if (likely(atomic_try_cmpxchg_acquire(&lock->cnts, &cnts, _QW_LOCKED)))
        return;

    /* 快速路径失败 → 慢路径排队等待 */
    queued_write_lock_slowpath(lock);
}
```

> **rwlock vs spinlock 的 perf 表现差异**：
> - rwlock 在读多写少场景下，读者走快速路径（原子加法），**不会进入 slowpath**
> - 因此 `perf top` 中 rwlock 相关函数出现频率远低于 spinlock
> - 但如果写者频繁，rwlock 的争用同样会出现在 `queued_write_lock_slowpath` 中

---

## 总结

### 锁竞争诊断速查

```
命令                                      │ 用途
─────────────────────────────────────────┼────────────────────
perf top -a                               │ 实时查看锁热点
perf record -g -F 99 -a sleep 5          │ 采样锁热点调用栈
perf lock record -a sleep 10              │ 精确锁事件采集
perf lock report --stdio                  │ 锁竞争统计报告
perf lock report -s caller --stdio        │ 按调用路径分析

关键内核函数：
  ★ native_queued_spin_lock_slowpath  → 自旋锁慢路径（争用标志）
  ★ _raw_spin_lock                    → 自旋锁入口
  ★ mutex_lock                        → 互斥锁
  ★ rwsem_down_read_slowpath          → 读写信号量慢路径
```

### 源码位置

```
perf lock:          src/linux-5.10/tools/perf/builtin-lock.c

内核锁实现:
  自旋锁入口:       src/linux-5.10/kernel/locking/spinlock.c
  queued spinlock:  src/linux-5.10/kernel/locking/qspinlock.c       ★ 慢路径实现
  MCS 队列节点:     src/linux-5.10/kernel/locking/mcs_spinlock.h    ★ 自旋等待核心
  读写锁:           src/linux-5.10/kernel/locking/qrwlock.c
  互斥锁:           src/linux-5.10/kernel/locking/mutex.c
  读写信号量:       src/linux-5.10/kernel/locking/rwsem.c

头文件（数据结构 + 快速路径）:
  qspinlock_t:      src/linux-5.10/include/asm-generic/qspinlock_types.h  ★ 4 字节位域布局
  qspinlock 快路径: src/linux-5.10/include/asm-generic/qspinlock.h        ★ CAS + slowpath 调用
  qrwlock_t:        src/linux-5.10/include/asm-generic/qrwlock_types.h    ★ 读写锁结构
  qrwlock 快路径:   src/linux-5.10/include/asm-generic/qrwlock.h          ★ 读/写锁获取

锁 tracepoint:      src/linux-5.10/include/trace/events/lock.h
```
