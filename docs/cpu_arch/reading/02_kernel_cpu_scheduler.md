<!--
======================================================================
  Step 2: 内核 CPU 调度器核心数据结构与算法
======================================================================
  来源: src/linux-5.10/
    - include/linux/sched.h (task_struct, sched_entity)
    - kernel/sched/sched.h (rq, cfs_rq)
    - kernel/sched/fair.c (CFS 核心算法)
    - kernel/sched/core.c (schedule() 主函数)
    - include/linux/sched/topology.h (sched_domain)
  阅读要点:
    - struct task_struct 中调度相关的关键字段
    - struct sched_entity: vruntime、红黑树节点、负载权重
    - struct rq: 每 CPU 运行队列
    - struct cfs_rq: CFS 运行队列，红黑树和 min_vruntime
    - CFS 核心函数: update_curr(), pick_next_entity(), __enqueue_entity()
    - __schedule() 调度主路径
    - 调度域 (sched_domain) 与负载均衡
  预计时间: 50-70 分钟
======================================================================
-->

# 内核 CPU 调度器核心数据结构与算法

> Linux 调度器是内核中最复杂的子系统之一。
> 本文从关键数据结构入手，逐层展开到 CFS 核心算法和调度主路径。

---

## 目录

- [1. 调度器架构总览](#1-调度器架构总览)
- [2. struct task_struct — 进程描述符](#2-struct-task_struct--进程描述符)
- [3. struct sched_entity — CFS 调度实体](#3-struct-sched_entity--cfs-调度实体)
- [4. struct rq — 运行队列](#4-struct-rq--运行队列)
- [5. struct cfs_rq — CFS 运行队列](#5-struct-cfs_rq--cfs-运行队列)
- [6. CFS 核心算法](#6-cfs-核心算法)
- [7. __schedule() 调度主路径](#7-__schedule-调度主路径)
- [8. 调度域与负载均衡](#8-调度域与负载均衡)
- [9. CPU 亲和性与 NUMA 感知](#9-cpu-亲和性与-numa-感知)
- [10. 调度参数速查表](#10-调度参数速查表)

---

## 1. 调度器架构总览

```
  ┌─────────────────────────────────────────────────────────────┐
  │                    用户态系统调用                              │
  │  schedule()  sched_yield()  sched_setaffinity()  nice()     │
  │  setpriority()  sched_setscheduler()  cgroups/cpu            │
  ├─────────────────────────────────────────────────────────────┤
  │                  schedule() 核心函数                          │
  │  kernel/sched/core.c:4410                                    │
  │                                                              │
  │  1. rq_lock() — 获取运行队列锁                               │
  │  2. deactivate_task() — 如需要，将当前任务出队               │
  │  3. pick_next_task() — 选择下一个任务                        │
  │  4. context_switch() — 切换页表、寄存器                      │
  │  5. rq_unlock() — 释放锁                                     │
  ├─────────────────────────────────────────────────────────────┤
  │                  调度类 (scheduling class)                     │
  │                                                              │
  │  ┌───────┐   ┌────────┐   ┌───────┐   ┌───────┐            │
  │  │  DL   │ > │   RT   │ > │  CFS  │ > │ Idle  │            │
  │  │deadline│   │FIFO/RR│   │NORMAL │   │       │            │
  │  └───────┘   └────────┘   └───────┘   └───────┘            │
  │                                                              │
  │  pick_next_task() 按 DL→RT→CFS→Idle 优先级遍历              │
  ├─────────────────────────────────────────────────────────────┤
  │              每 CPU 运行队列 (struct rq)                       │
  │  DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);        │
  │  每个 CPU 有独立的 rq，包含 cfs_rq / rt_rq / dl_rq           │
  └─────────────────────────────────────────────────────────────┘
```

---

## 2. struct task_struct — 进程描述符

> 来源: `src/linux-5.10/include/linux/sched.h:640`
> ★ 这是内核中最重要的数据结构之一，包含进程的所有状态信息。

```c
/* ====== 第 1 部分：任务状态 ====== */
struct task_struct {
    /*
     * ★ 任务状态位:
     *   TASK_RUNNING (0x0000)        — 可运行（在运行队列中或正在执行）
     *   TASK_INTERRUPTIBLE (0x0001)  — 可中断睡眠（等待事件/信号）
     *   TASK_UNINTERRUPTIBLE (0x0002)— 不可中断睡眠（等待 I/O）
     *   __TASK_STOPPED (0x0004)      — 被信号停止
     *   __TASK_TRACED (0x0008)       — 被 ptrace 跟踪
     *   TASK_DEAD (0x0080)           — 进程已死
     *   TASK_WAKEKILL (0x0100)       — 可被致命信号唤醒
     *   TASK_WAKING (0x0200)         — 正在被唤醒
     */
    volatile long           state;

    /* ====== 第 2 部分：SMP 相关 ====== */
#ifdef CONFIG_SMP
    int                     on_cpu;         /* 是否正在 CPU 上运行 */
    struct __call_single_node wake_entry;   /* IPI 唤醒入口 */
    unsigned int            cpu;            /* ★ 当前所在的 CPU 编号 */
    unsigned int            wakee_flips;    /* 唤醒者翻转计数（负载均衡用） */
    unsigned long           wakee_flip_decay_ts;
    struct task_struct      *last_wakee;    /* 上次唤醒的任务 */
    int                     recent_used_cpu;/* 最近使用的 CPU（唤醒亲和用） */
    int                     wake_cpu;       /* 被唤醒时所在的 CPU */
#endif
    int                     on_rq;          /* ★ 是否在运行队列中
                                             * TASK_ON_RQ_QUEUED (1)
                                             * TASK_ON_RQ_MIGRATING (2) */

    /* ====== 第 3 部分：★ 调度优先级 ====== */
    int                     prio;           /* 动态优先级 (0-139) */
    int                     static_prio;    /* 静态优先级 (nice 映射) */
    int                     normal_prio;    /* 不考虑 RT boost 的优先级 */
    unsigned int            rt_priority;    /* 实时优先级 (1-99) */

    /* ====== 第 4 部分：★ 调度类与调度实体 ====== */
    const struct sched_class *sched_class;  /* ★ 指向当前调度类
                                             * (&dl_sched_class,
                                             *  &rt_sched_class,
                                             *  &fair_sched_class,
                                             *  &idle_sched_class) */
    struct sched_entity     se;             /* ★ CFS 调度实体 */
    struct sched_rt_entity  rt;             /* RT 调度实体 */
    struct sched_dl_entity  dl;             /* DL 调度实体 */

    /* ====== 第 5 部分：调度策略与 CPU 亲和性 ====== */
    unsigned int            policy;         /* ★ 调度策略:
                                             * SCHED_NORMAL (0) — CFS
                                             * SCHED_FIFO (1) — RT FIFO
                                             * SCHED_RR (2) — RT 轮转
                                             * SCHED_BATCH (3) — 批处理
                                             * SCHED_IDLE (5) — 空闲
                                             * SCHED_DEADLINE (6) — 截止期限 */
    int                     nr_cpus_allowed;/* 允许运行的 CPU 数量 */
    const cpumask_t         *cpus_ptr;      /* ★ CPU 亲和性掩码指针 */
    cpumask_t               cpus_mask;      /* ★ CPU 亲和性掩码
                                             * taskset/chrt 修改此字段 */

    /* ====== 第 6 部分：内存管理 ====== */
    struct mm_struct        *mm;            /* 用户空间地址空间描述符
                                             * 内核线程为 NULL */
    struct mm_struct        *active_mm;     /* 当前激活的地址空间
                                             * 内核线程借用前一个任务的 mm */

    /* ... 后续字段省略（文件描述符、信号、命名空间等） ... */
};
```

### task_struct 调度相关字段关系图

```
  task_struct
  ├── state: TASK_RUNNING / TASK_INTERRUPTIBLE / ...
  │
  ├── on_cpu: 是否正在 CPU 上执行
  ├── on_rq: 是否在运行队列中
  ├── cpu: 当前所在 CPU 编号
  │
  ├── prio / static_prio / normal_prio / rt_priority
  │   └── 决定任务在调度器中的优先级
  │
  ├── sched_class ──────────→ 决定使用哪种调度算法
  │   ├── dl_sched_class (SCHED_DEADLINE)
  │   ├── rt_sched_class (SCHED_FIFO / SCHED_RR)
  │   ├── fair_sched_class (SCHED_NORMAL / SCHED_BATCH / SCHED_IDLE)
  │   └── idle_sched_class (idle 线程)
  │
  ├── se (sched_entity) ────→ CFS 调度使用的核心结构
  │   ├── vruntime           → 虚拟运行时间
  │   ├── run_node           → 红黑树节点
  │   └── load               → 负载权重
  │
  └── cpus_mask ────────────→ CPU 亲和性 (taskset 修改此字段)
```

---

## 3. struct sched_entity — CFS 调度实体

> 来源: `src/linux-5.10/include/linux/sched.h:451`
> ★ 这是 CFS 调度的核心数据结构，每个可运行任务都有一个 sched_entity。

```c
/* ====== sched_entity: CFS 调度的核心 ====== */
struct sched_entity {
    /* ====== 第 1 部分：★ 负载与红黑树 ====== */
    struct load_weight      load;       /* ★ 负载权重
                                         * nice 0 → weight = 1024
                                         * nice -20 → weight = 88761
                                         * nice +19 → weight = 15
                                         * 用于 vruntime 计算 */
    struct rb_node          run_node;   /* ★ 红黑树节点
                                         * key = vruntime
                                         * CFS 运行队列中所有任务
                                         * 按 vruntime 组织为红黑树 */
    struct list_head        group_node; /* 组调度链表节点 */
    unsigned int            on_rq;      /* 是否在运行队列中 */

    /* ====== 第 2 部分：★ 时间与 vruntime ====== */
    u64                     exec_start;        /* 本次执行的开始时间 */
    u64                     sum_exec_runtime;  /* 累计实际执行时间 (ns) */
    u64                     vruntime;          /* ★★ 虚拟运行时间
                                                * CFS 的核心指标
                                                * = sum_exec_runtime * 1024 / weight
                                                * vruntime 最小的任务优先运行 */
    u64                     prev_sum_exec_runtime; /* 上次统计点的累计时间 */

    /* ====== 第 3 部分：迁移统计 ====== */
    u64                     nr_migrations;     /* ★ 被迁移到其他 CPU 的次数 */

    /* ====== 第 4 部分：调度统计 ====== */
    struct sched_statistics statistics;        /* 等待时间、睡眠时间等统计 */

    /* ====== 第 5 部分：组调度 ====== */
#ifdef CONFIG_FAIR_GROUP_SCHED
    int                     depth;             /* 在调度组层次中的深度 */
    struct sched_entity     *parent;           /* 父调度实体 */
    struct cfs_rq           *cfs_rq;           /* ★ 此实体所在的 CFS 队列 */
    struct cfs_rq           *my_q;             /* 此实体"拥有"的子队列 */
    unsigned long           runnable_weight;   /* 缓存的子队列任务数 */
#endif

    /* ====== 第 6 部分：PELT 负载追踪 ====== */
#ifdef CONFIG_SMP
    struct sched_avg        avg;               /* ★ Per-Entity Load Tracking
                                                * 追踪每个实体的 CPU 利用率
                                                * 用于负载均衡决策 */
#endif
};
```

### ★ vruntime 计算原理

```
  vruntime = 实际执行时间 × (NICE_0_LOAD / weight)

  ┌──────────────┬────────┬─────────────────────────────────────┐
  │ nice 值       │ weight │ vruntime 增长率 (相对于实际时间)     │
  ├──────────────┼────────┼─────────────────────────────────────┤
  │ -20 (最高优先) │ 88761  │ × (1024/88761) ≈ 0.012 → 极慢增长  │
  │  0 (默认)     │ 1024   │ × (1024/1024) = 1.0 → 正常增长      │
  │ +10           │ 110    │ × (1024/110) ≈ 9.3 → 快速增长       │
  │ +19 (最低优先) │ 15     │ × (1024/15) ≈ 68.3 → 极快速增长    │
  └──────────────┴────────┴─────────────────────────────────────┘

  ★ 核心规则:
  - vruntime 增长越慢 → 越早被调度（获得更多 CPU 时间）
  - vruntime 增长越快 → 越晚被调度（获得更少 CPU 时间）
  - nice -20 的任务 vruntime 增长速度是 nice +19 的 1/5900
```

---

## 4. struct rq — 运行队列

> 来源: `src/linux-5.10/kernel/sched/sched.h:895`
> ★ 每个 CPU 有独立的 rq，包含该 CPU 上所有调度类的运行队列。

```c
/* ====== 每 CPU 运行队列 ====== */
struct rq {
    /* ====== 第 1 部分：★ 锁与基本计数 ====== */
    raw_spinlock_t      lock;           /* ★ 运行队列自旋锁
                                         * 所有对该 rq 的操作必须持有此锁 */

    unsigned int        nr_running;     /* ★ 当前可运行的任务总数
                                         * 包括所有调度类 */
#ifdef CONFIG_NUMA_BALANCING
    unsigned int        nr_numa_running;     /* NUMA 相关任务数 */
    unsigned int        nr_preferred_running;/* 在首选 NUMA 节点上的任务数 */
    unsigned int        numa_migrate_on;     /* NUMA 迁移是否进行中 */
#endif

    /* ====== 第 2 部分：★ 调度统计 ====== */
    u64                 nr_switches;    /* ★ 上下文切换总次数
                                         * /proc/schedstat 可见 */

    /* ====== 第 3 部分：★ 三个调度类的运行队列 ====== */
    struct cfs_rq       cfs;            /* ★ CFS (公平调度) 运行队列 */
    struct rt_rq        rt;             /* RT (实时调度) 运行队列 */
    struct dl_rq        dl;             /* DL (截止期限调度) 运行队列 */

    /* ====== 第 4 部分：★ 当前任务 ====== */
    struct task_struct __rcu *curr;     /* ★ 当前正在此 CPU 上运行的任务 */
    struct task_struct  *idle;          /* idle 线程 (swapper/N) */
    struct task_struct  *stop;          /* stop_machine 使用的特殊线程 */
    unsigned long       next_balance;   /* 下次负载均衡检查时间 */
    struct mm_struct    *prev_mm;       /* 上次任务的 mm (切换用) */

    /* ====== 第 5 部分：时钟 ====== */
    unsigned int        clock_update_flags;
    u64                 clock;          /* rq 时钟 (基于 sched_clock) */
    u64                 clock_task;     /* 扣除 IRQ/steal 时间的任务时钟 */
    u64                 clock_pelt;     /* PELT 使用的时钟 */

    /* ====== 第 6 部分：SMP 调度域 ====== */
#ifdef CONFIG_SMP
    struct root_domain  *rd;            /* root domain (RT 负载均衡) */
    struct sched_domain __rcu *sd;      /* ★ 调度域 (CPU 拓扑层次) */
    unsigned long       cpu_capacity;   /* CPU 容量 (考虑频率和负载) */
    unsigned long       cpu_capacity_orig;/* CPU 原始容量 (不考虑频率) */

    struct callback_head *balance_callback;/* 负载均衡回调链表 */

    unsigned char       idle_balance;   /* 是否触发了 idle 负载均衡 */

    int                 active_balance; /* 主动负载均衡标志 */
    int                 push_cpu;       /* 迁移目标 CPU */

    int                 cpu;            /* ★ 此 rq 对应的 CPU 编号 */
    int                 online;         /* CPU 是否在线 */

    struct list_head    cfs_tasks;      /* CFS 任务链表 (用于负载均衡遍历) */

    struct sched_avg    avg_rt;         /* RT 类平均利用率 */
    struct sched_avg    avg_dl;         /* DL 类平均利用率 */

    u64                 idle_stamp;     /* 进入空闲的时间戳 */
    u64                 avg_idle;       /* 平均空闲时长 */
#endif

    /* ====== 第 7 部分：调度统计 (schedstat) ====== */
#ifdef CONFIG_SCHEDSTATS
    struct sched_info   rq_sched_info;  /* 延迟统计 */
    unsigned long long  rq_cpu_time;    /* CPU 时间 */
    unsigned int        yld_count;      /* sched_yield() 调用次数 */
    unsigned int        sched_count;    /* schedule() 调用次数 */
    unsigned int        sched_goidle;   /* 调度到 idle 的次数 */
    unsigned int        ttwu_count;     /* try_to_wake_up() 次数 */
    unsigned int        ttwu_local;     /* 本地唤醒次数 */
#endif
};
```

### rq 与 task_struct 的关系

```
  CPU 0                    CPU 1                    CPU 2
  ┌──────────┐            ┌──────────┐            ┌──────────┐
  │  rq[0]   │            │  rq[1]   │            │  rq[2]   │
  │ nr_run=3 │            │ nr_run=2 │            │ nr_run=1 │
  │          │            │          │            │          │
  │ curr ────┼──→ task A  │ curr ────┼──→ task D  │ curr ────┼──→ task F
  │ idle ────┼──→ swapper │ idle ────┼──→ swapper │ idle ────┼──→ swapper
  │          │            │          │            │          │
  │ cfs:     │            │ cfs:     │            │ cfs:     │
  │  ├─B     │            │  ├─E     │            │  ├─G     │
  │  └─C     │            │  └─(空)  │            │  └─(空)  │
  │          │            │          │            │          │
  │ sd ─→ NUMA domain     │ sd ─→ NUMA domain     │ sd ─→ NUMA domain  │
  │    └→ MC domain       │    └→ MC domain       │    └→ MC domain    │
  │         └→ SMT domain │         └→ SMT domain │         └→ (无 SMT) │
  └──────────┘            └──────────┘            └──────────┘
```

---

## 5. struct cfs_rq — CFS 运行队列

> 来源: `src/linux-5.10/kernel/sched/sched.h:519`
> ★ 管理 CFS 类任务的运行队列，核心是红黑树和 min_vruntime。

```c
/* ====== CFS 运行队列 ====== */
struct cfs_rq {
    /* ====== 第 1 部分：★ 负载与计数 ====== */
    struct load_weight  load;              /* 队列中所有任务的总负载 */
    unsigned int        nr_running;        /* ★ 当前可运行的 CFS 任务数 */
    unsigned int        h_nr_running;      /* 层次化计数 (含子组) */
    unsigned int        idle_h_nr_running; /* SCHED_IDLE 策略的任务数 */

    /* ====== 第 2 部分：★ 时钟与 min_vruntime ====== */
    u64                 exec_clock;        /* CFS 队列的总执行时间 */
    u64                 min_vruntime;      /* ★★ CFS 的核心变量
                                            * 红黑树中最小的 vruntime
                                            * 新任务/唤醒任务的 vruntime
                                            * 不能小于 min_vruntime
                                            * 防止新任务获得不公平的优势 */
#ifndef CONFIG_64BIT
    u64                 min_vruntime_copy; /* 32 位下的无锁读取副本 */
#endif

    /* ====== 第 3 部分：★ 红黑树 ====== */
    struct rb_root_cached tasks_timeline;  /* ★★ CFS 红黑树根节点
                                            * 所有可运行的 sched_entity
                                            * 按 vruntime 排序
                                            * cached 版本缓存最左节点
                                            * (vruntime 最小的任务) */

    /* ====== 第 4 部分：当前/下一个/跳过 ====== */
    struct sched_entity *curr;             /* ★ 当前正在运行的实体 */
    struct sched_entity *next;             /* 下一个想运行的实体 (yield 设置) */
    struct sched_entity *last;             /* 上一个运行的实体 */
    struct sched_entity *skip;             /* 跳过的实体 (sched_yield) */

    /* ====== 第 5 部分：PELT 负载追踪 ====== */
#ifdef CONFIG_SMP
    struct sched_avg    avg;               /* 队列级平均利用率 */

    struct {
        raw_spinlock_t  lock;
        int             nr;                /* 待移除的任务数 */
        unsigned long   load_avg;          /* 待移除的负载 */
        unsigned long   util_avg;          /* 待移除的利用率 */
        unsigned long   runnable_avg;      /* 待移除的可运行时间 */
    } removed;                             /* 迁移到其他 CPU 的任务
                                            * 需要延迟移除其负载贡献 */
#endif

    /* ====== 第 6 部分：组调度 ====== */
#ifdef CONFIG_FAIR_GROUP_SCHED
    struct rq           *rq;               /* 所属的 CPU rq */
    struct list_head    leaf_cfs_rq_list;  /* 叶子 cfs_rq 链表 */
    struct task_group   *tg;               /* 所属的任务组 (cgroup) */

#ifdef CONFIG_CFS_BANDWIDTH
    int                 runtime_enabled;   /* CFS 带宽控制是否启用 */
    s64                 runtime_remaining; /* 剩余运行时间配额 */
    /* ... cgroup CPU 限流相关 ... */
#endif
#endif
};
```

### ★ CFS 红黑树结构图

```
  cfs_rq->tasks_timeline (红黑树, key = vruntime)

              task_C (vr=12.5ms, 红色)
             /                        \
      task_A (vr=8.3ms, 黑色)     task_E (vr=18.7ms, 黑色)
      /              \                /              \
  (NULL)     task_B (vr=10.1ms)  task_D (vr=15.2ms)  task_F (vr=20.0ms)
                 红色                红色                红色

  ★ 最左节点 (cached):
  tasks_timeline.rb_leftmost → task_A (vr=8.3ms)
  → pick_next_entity() 通常选择最左节点

  ★ min_vruntime = 8.3ms (最左节点的 vruntime)
  → 新唤醒任务的 vruntime = max(原值, min_vruntime - 阈值)
  → 防止睡眠很久的任务回来后 vruntime 远小于其他任务
```

---

## 6. CFS 核心算法

### 6.1 ★ update_curr() — 更新 vruntime

> 来源: `src/linux-5.10/kernel/sched/fair.c:842`

```c
/* ====== update_curr: 更新当前任务的 vruntime ====== */
static void update_curr(struct cfs_rq *cfs_rq)
{
    struct sched_entity *curr = cfs_rq->curr;  /* 当前运行的实体 */
    u64 now = rq_clock_task(rq_of(cfs_rq));    /* 当前时间 (ns) */
    u64 delta_exec;

    if (unlikely(!curr))
        return;

    /* 计算本次执行的时间差 */
    delta_exec = now - curr->exec_start;       /* ★ 实际执行时间 */
    if (unlikely((s64)delta_exec <= 0))
        return;

    curr->exec_start = now;                    /* 更新执行起始时间 */

    /* 更新统计 */
    curr->sum_exec_runtime += delta_exec;      /* 累加实际执行时间 */
    schedstat_add(cfs_rq->exec_clock, delta_exec);

    /* ★★ 核心: 计算 vruntime 增量 */
    curr->vruntime += calc_delta_fair(delta_exec, curr);
    /*
     * calc_delta_fair(delta_exec, curr) 等价于:
     *   delta_exec * NICE_0_LOAD / curr->load.weight
     *
     * nice 0 (weight=1024): vruntime += delta_exec × 1.0
     * nice -5 (weight=3121): vruntime += delta_exec × 0.33
     * nice +10 (weight=110):  vruntime += delta_exec × 9.31
     */

    /* 更新队列的 min_vruntime */
    update_min_vruntime(cfs_rq);

    /* 发送 tracepoint 事件 (perf sched 可追踪) */
    if (entity_is_task(curr)) {
        struct task_struct *curtask = task_of(curr);
        trace_sched_stat_runtime(curtask, delta_exec, curr->vruntime);
    }
}
```

### 6.2 ★ __enqueue_entity() — 插入红黑树

> 来源: `src/linux-5.10/kernel/sched/fair.c:575`

```c
/* ====== __enqueue_entity: 将 sched_entity 插入 CFS 红黑树 ====== */
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    struct rb_node **link = &cfs_rq->tasks_timeline.rb_root.rb_node;
    struct rb_node *parent = NULL;
    struct sched_entity *entry;
    bool leftmost = true;    /* ★ 是否是最左节点 (vruntime 最小) */

    /*
     * 在红黑树中找到正确位置:
     * key = vruntime
     */
    while (*link) {
        parent = *link;
        entry = rb_entry(parent, struct sched_entity, run_node);

        if (entity_before(se, entry)) {
            /*
             * se->vruntime < entry->vruntime
             * → 插入到左子树
             * ★ 保持 leftmost = true
             */
            link = &parent->rb_left;
        } else {
            /*
             * se->vruntime >= entry->vruntime
             * → 插入到右子树
             */
            link = &parent->rb_right;
            leftmost = false;  /* ★ 不是最左节点 */
        }
    }

    /* 插入节点 */
    rb_link_node(&se->run_node, parent, link);
    rb_insert_color_cached(&se->run_node,
                           &cfs_rq->tasks_timeline, leftmost);
    /*
     * rb_insert_color_cached: 红黑树平衡操作
     * 如果 leftmost=true，同时更新缓存的最左指针
     * → 后续 __pick_first_entity() 可 O(1) 获取最左节点
     */
}
```

### 6.3 ★ pick_next_entity() — 选择下一个任务

> 来源: `src/linux-5.10/kernel/sched/fair.c:4435`

```c
/* ====== pick_next_entity: 选择 vruntime 最小的任务 ====== */
static struct sched_entity *
pick_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
    /* ★ 获取红黑树最左节点 (vruntime 最小) */
    struct sched_entity *left = __pick_first_entity(cfs_rq);
    struct sched_entity *se;

    /*
     * 如果 curr 的 vruntime 比最左节点还小
     * → curr 应该继续运行
     */
    if (!left || (curr && entity_before(curr, left)))
        left = curr;

    se = left; /* 理想情况下运行最左节点 */

    /*
     * 处理 skip buddy (sched_yield 标记的任务):
     * 如果被 skip 的任务恰好是最左节点，尝试选第二个
     */
    if (cfs_rq->skip == se) {
        struct sched_entity *second;
        if (se == curr) {
            second = __pick_first_entity(cfs_rq);
        } else {
            second = __pick_next_entity(se);
            if (!second || (curr && entity_before(curr, second)))
                second = curr;
        }
        if (second && wakeup_preempt_entity(second, left) < 1)
            se = second;
    }

    /*
     * 处理 next buddy (sched_yield 指定的下一个任务):
     * 如果 next 的 vruntime 差距不大，优先运行它
     */
    if (cfs_rq->next && wakeup_preempt_entity(cfs_rq->next, left) < 1) {
        se = cfs_rq->next;
    } else if (cfs_rq->last && wakeup_preempt_entity(cfs_rq->last, left) < 1) {
        se = cfs_rq->last;
    }

    return se;
}
```

### 6.4 ★ place_entity() — 计算新任务的初始 vruntime

> 来源: `src/linux-5.10/kernel/sched/fair.c:4108`

```c
/* ====== place_entity: 设置新任务/唤醒任务的 vruntime ====== */
static void
place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int initial)
{
    u64 vruntime = cfs_rq->min_vruntime;  /* ★ 基准: 队列最小 vruntime */

    /*
     * 新创建的任务 (initial=1):
     * 当前周期已经承诺给现有任务，新任务的额外权重会稍微
     * 拖慢其他任务，所以给新任务一个惩罚:
     * vruntime = min_vruntime + 一个调度周期的份额
     */
    if (initial && sched_feat(START_DEBIT))
        vruntime += sched_vslice(cfs_rq, se);

    /*
     * 被唤醒的任务 (initial=0):
     * 睡眠的任务不应该被惩罚太多
     * 给一个"奖励": vruntime = min_vruntime - 半个调度周期
     *
     * ★ GENTLE_FAIR_SLEEPERS: 只给一半的奖励
     * 防止交互式任务通过频繁睡眠获得过多优势
     */
    if (!initial) {
        unsigned long thresh = sysctl_sched_latency;  /* 默认 6ms */
        if (sched_feat(GENTLE_FAIR_SLEEPERS))
            thresh >>= 1;  /* 减半: 3ms */
        vruntime -= thresh;
    }

    /* ★ 确保 vruntime 不会倒退 */
    se->vruntime = max_vruntime(se->vruntime, vruntime);
}
```

### 6.5 CFS 调度周期计算

```c
/*
 * 调度周期 (sched_latency):
 * 在这个时间窗口内，所有可运行任务至少运行一次
 *
 * 默认: 6ms × (1 + ilog2(ncpus))
 */

/* fair.c:682 */
static u64 __sched_period(unsigned int nr_running)
{
    /*
     * 如果任务数 ≤ sched_nr_latency (默认 8):
     *   周期 = sysctl_sched_latency (6ms)
     *
     * 如果任务数 > sched_nr_latency:
     *   周期 = nr_running × sysctl_sched_min_granularity
     *   (每个任务至少获得 min_granularity = 0.75ms)
     */
    if (unlikely(nr_running > sched_nr_latency))
        return nr_running * sysctl_sched_min_granularity;
    return sysctl_sched_latency;
}
```

```
  调度周期示意:

  3 个任务, sched_latency = 6ms:

  ┌────────────────────────── 6ms ──────────────────────────┐
  │  task A (2ms)  │  task B (2ms)  │  task C (2ms)         │
  │  nice 0        │  nice 0        │  nice 0               │
  └─────────────────────────────────────────────────────────┘

  3 个任务 (A: nice 0, B: nice 0, C: nice +10):

  ┌────────────────────────── 6ms ──────────────────────────┐
  │  task A (~2.6ms)  │  task B (~2.6ms)  │ task C (~0.8ms) │
  │  weight=1024      │  weight=1024      │ weight=110      │
  └─────────────────────────────────────────────────────────┘

  12 个任务 (> sched_nr_latency=8):

  ┌──────── 12 × 0.75ms = 9ms ─────────────────────────────┐
  │ 0.75 │ 0.75 │ 0.75 │ 0.75 │ 0.75 │ ... × 12            │
  └─────────────────────────────────────────────────────────┘
```

---

## 7. __schedule() 调度主路径

> 来源: `src/linux-5.10/kernel/sched/core.c:4410`

```c
/* ====== __schedule: 调度核心函数 ====== */
static void __sched notrace __schedule(bool preempt)
{
    struct task_struct *prev, *next;
    unsigned long *switch_count;
    unsigned long prev_state;
    struct rq_flags rf;
    struct rq *rq;
    int cpu;

    cpu = smp_processor_id();          /* 获取当前 CPU 编号 */
    rq = cpu_rq(cpu);                  /* ★ 获取当前 CPU 的 rq */
    prev = rq->curr;                   /* ★ 当前正在运行的任务 */

    schedule_debug(prev, preempt);     /* 调试检查 */

    local_irq_disable();               /* ★ 关闭本地中断 */
    rcu_note_context_switch(preempt);  /* 通知 RCU 上下文切换 */

    rq_lock(rq, &rf);                  /* ★ 获取 rq 自旋锁 */
    smp_mb__after_spinlock();          /* 内存屏障 */

    update_rq_clock(rq);               /* 更新 rq 时钟 */

    switch_count = &prev->nivcsw;      /* 默认为非自愿切换计数 */

    prev_state = prev->state;          /* 读取当前任务状态 */

    /* ====== 非抢占式调度: 检查是否需要睡眠 ====== */
    if (!preempt && prev_state) {
        if (signal_pending_state(prev_state, prev)) {
            prev->state = TASK_RUNNING; /* 有信号 → 继续运行 */
        } else {
            /* ★ 任务主动睡眠: 从运行队列中移除 */
            deactivate_task(rq, prev, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);

            if (prev->in_iowait) {
                atomic_inc(&rq->nr_iowait); /* 增加 I/O 等待计数 */
            }
        }
        switch_count = &prev->nvcsw;   /* 自愿切换计数 */
    }

    /* ★★ 选择下一个任务 */
    next = pick_next_task(rq, prev, &rf);
    /*
     * pick_next_task() 遍历:
     * DL → RT → CFS → Idle
     * 返回优先级最高的可运行任务
     */

    clear_tsk_need_resched(prev);      /* 清除重调度标志 */

    /* ====== 如果选中了不同的任务 → 执行上下文切换 ====== */
    if (likely(prev != next)) {
        rq->nr_switches++;             /* ★ 增加切换计数 */
        RCU_INIT_POINTER(rq->curr, next);

        /*
         * context_switch():
         * 1. switch_mm() — 切换页表 (如果 mm 不同)
         * 2. switch_to() — 切换寄存器 (栈指针、指令指针等)
         * 3. finish_arch_post_lock_switch() — 架构特定清理
         */
        rq = context_switch(rq, prev, next, &rf);
    }

    /* balance_callback 处理 */
    ...
}
```

### 调度触发路径

```
  ┌──────────────────────────────────────────────────────────┐
  │                 调度触发的三种方式                          │
  │                                                            │
  │  1. 主动调度 (preempt=false):                              │
  │     schedule() ← 任务主动调用                              │
  │     ├── 系统调用返回时检查 TIF_NEED_RESCHED                 │
  │     ├── mutex_lock() 等阻塞操作                            │
  │     ├── wait_event() / sleep                               │
  │     └── cond_resched() — 显式让出 CPU                      │
  │                                                            │
  │  2. 抢占调度 (preempt=true):                               │
  │     __schedule(true) ← 被更高优先级任务抢占                │
  │     ├── 时钟中断 → scheduler_tick() → set_tsk_need_resched│
  │     ├── 唤醒高优先级任务 → check_preempt_curr()            │
  │     └── 内核抢占配置 (CONFIG_PREEMPT=y)                    │
  │                                                            │
  │  3. idle 调度:                                             │
  │     没有其他可运行任务 → 运行 idle 线程                    │
  │     idle 线程中循环调用 schedule_idle()                    │
  └──────────────────────────────────────────────────────────┘
```

---

## 8. 调度域与负载均衡

### 8.1 ★ struct sched_domain — 调度域

> 来源: `src/linux-5.10/include/linux/sched/topology.h:79`

```c
/* ====== sched_domain: CPU 拓扑层次 ====== */
struct sched_domain {
    struct sched_domain __rcu *parent;   /* ★ 父域 (更高层次) */
    struct sched_domain __rcu *child;    /* ★ 子域 (更低层次) */
    struct sched_group *groups;          /* 调度组 (负载均衡的基本单位) */
    unsigned long min_interval;          /* 最小负载均衡间隔 (ms) */
    unsigned long max_interval;          /* 最大负载均衡间隔 (ms) */
    unsigned int busy_factor;            /* 忙碌时减少负载均衡频率 */
    unsigned int imbalance_pct;          /* 负载不均衡阈值 (%) */
    unsigned int cache_nice_tries;       /* 缓存热任务保留尝试次数 */

    int nohz_idle;                       /* NOHZ idle 状态 */
    int flags;                           /* ★ SD_* 标志位 */
    int level;                           /* ★ 域层级编号 */

    /* 运行时字段 */
    unsigned long last_balance;          /* 上次负载均衡时间 */
    unsigned int balance_interval;       /* 当前均衡间隔 */
    unsigned int nr_balance_failed;      /* 连续失败次数 */

    /* idle 负载均衡统计 */
    u64 max_newidle_lb_cost;             /* newidle 最大成本 */

    /* CPU span: 此域覆盖的所有 CPU */
    unsigned int span_weight;            /* CPU 数量 */
    unsigned long span[];                /* ★ CPU 位图 (变长) */
};
```

### 8.2 调度域层次

```
  典型 NUMA 系统的调度域层次:

  ┌─────────────────────────────────────────────────┐
  │  NUMA Domain (level=3)                           │
  │  span: 0-15 (所有 CPU)                           │
  │  负载均衡跨 NUMA 节点                             │
  │  间隔: 100-600ms                                 │
  ├─────────────────────────────────────────────────┤
  │  DIE Domain (level=2)                            │
  │  span: 0-7 (CPU package 0)    span: 8-15 (pkg 1)│
  │  负载均衡在同一 package 内                        │
  │  间隔: 20-100ms                                  │
  ├─────────────────────────────────────────────────┤
  │  MC (Multi-Core) Domain (level=1)               │
  │  span: 0-3  span: 4-7  span: 8-11  span: 12-15  │
  │  负载均衡在同一 cluster 内                        │
  │  间隔: 5-20ms                                    │
  ├─────────────────────────────────────────────────┤
  │  SMT Domain (level=0)                            │
  │  span: 0-1  span: 2-3  span: 4-5  ...           │
  │  负载均衡在超线程之间                             │
  │  间隔: 1-5ms                                     │
  └─────────────────────────────────────────────────┘
```

### 8.3 负载均衡触发时机

```
  ┌────────────────────────────────────────────────────────┐
  │              负载均衡的三种触发方式                       │
  │                                                          │
  │  1. newidle 负载均衡:                                    │
  │     CPU 变空闲时立即触发                                 │
  │     idle_balance() → 从其他 CPU 迁移任务过来            │
  │     条件: rq->nr_running == 0                           │
  │                                                          │
  │  2. 周期性负载均衡:                                      │
  │     scheduler_tick() 中检查                             │
  │     间隔由 sched_domain->balance_interval 控制          │
  │     忙碌 CPU 间隔更长 (busy_factor)                     │
  │                                                          │
  │  3. 唤醒时负载均衡:                                      │
  │     try_to_wake_up() → select_task_rq_fair()            │
  │     选择唤醒任务应该放在哪个 CPU                         │
  │     考虑: 唤醒者亲和、缓存热、NUMA 本地                 │
  └────────────────────────────────────────────────────────┘
```

---

## 9. CPU 亲和性与 NUMA 感知

### 9.1 CPU 亲和性

```c
/*
 * sched_setaffinity() 系统调用:
 * 修改进程的 cpus_mask，限制其可运行的 CPU 集合
 *
 * taskset 命令底层调用此系统调用
 */

/* task_struct 中的相关字段: */
int              nr_cpus_allowed;  /* 允许运行的 CPU 数量 */
const cpumask_t  *cpus_ptr;       /* 指向 cpus_mask 或 cpu_possible_mask */
cpumask_t        cpus_mask;       /* ★ CPU 亲和性位图 */

/*
 * 示例: taskset -c 0,2 my_program
 * → cpus_mask = 0b0101 = 0x5
 * → nr_cpus_allowed = 2
 * → 任务只能在 CPU 0 和 CPU 2 上运行
 */
```

### 9.2 NUMA 感知调度

```
  NUMA Balancing 机制:

  ┌──────────────────────────────────────────────────────┐
  │  1. 任务在 NUMA node 0 上运行                         │
  │     但其大部分内存页面在 NUMA node 1 上               │
  │                                                        │
  │  2. 内核定期将页面标记为 PROT_NONE                    │
  │     任务访问这些页面时触发 page fault                  │
  │                                                        │
  │  3. 内核记录 page fault 发生的 NUMA 节点              │
  │     统计每个任务在各节点的访问频率                     │
  │                                                        │
  │  4. 决策:                                              │
  │     a) 迁移任务到内存所在节点 (任务迁移)              │
  │     b) 迁移内存到任务所在节点 (页面迁移)              │
  │     c) 两者都尝试                                      │
  └──────────────────────────────────────────────────────┘

  相关内核参数:
  /proc/sys/kernel/numa_balancing = 0 (禁用) / 1 (启用)

  相关 rq 字段:
  rq->nr_numa_running       — NUMA 相关任务数
  rq->nr_preferred_running  — 在首选节点上运行的任务数
  rq->numa_migrate_on       — 是否正在进行 NUMA 迁移
```

---

## 10. 调度参数速查表

### 内核调度参数 (/proc/sys/kernel/)

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `sched_latency_ns` | 6000000 (6ms) | CFS 调度周期 |
| `sched_min_granularity_ns` | 750000 (0.75ms) | CFS 最小抢占粒度 |
| `sched_wakeup_granularity_ns` | 1000000 (1ms) | 唤醒抢占阈值 |
| `sched_migration_cost_ns` | 500000 (0.5ms) | 任务迁移成本阈值 |
| `sched_nr_migrate` | 32 | 单次负载均衡最大迁移任务数 |
| `sched_rt_period_us` | 1000000 (1s) | RT 带宽控制周期 |
| `sched_rt_runtime_us` | 950000 (0.95s) | RT 带宽配额 |
| `numa_balancing` | 1 | NUMA 自动均衡开关 |

### 调度策略速查

| 策略 | 值 | 类 | 说明 |
|------|---|---|------|
| SCHED_NORMAL | 0 | CFS | 默认策略，普通进程 |
| SCHED_FIFO | 1 | RT | 实时 FIFO，无时间片 |
| SCHED_RR | 2 | RT | 实时轮转，有时间片 |
| SCHED_BATCH | 3 | CFS | 批处理，不优化交互性 |
| SCHED_IDLE | 5 | CFS | 极低优先级后台任务 |
| SCHED_DEADLINE | 6 | DL | 截止期限调度 |
