# Linux 内核子系统 — 性能分析视角

> 从观测工具到内核源码，理解性能瓶颈的根因

---

## 为什么要学内核子系统？

当你用 perf/bpftrace 发现某个内核函数占比很高时，如果不懂它所在的子系统，就只能停留在"知道热点函数名"的层面，无法：
- 理解**为什么**这个函数被频繁调用
- 判断**是否正常**（有些函数本就高频）
- 提出**优化方案**（调参数？改架构？换锁？）

本目录覆盖 4 个与性能分析最相关的内核子系统：

| 子系统 | 对应性能问题 | 核心源码 |
|--------|------------|---------|
| **内存分配器** | kmalloc/kfree 热点、zone lock 竞争 | mm/slub.c, mm/page_alloc.c |
| **网络协议栈** | 软中断高、skb 分配热点、TCP 重传 | net/ipv4/, net/core/ |
| **调度器** | 上下文切换多、调度延迟、CPU 亲和性 | kernel/sched/ |
| **锁机制** | spinlock 竞争、false sharing | kernel/locking/ |

---

## 学习路线

### 模块 A：内存分配器（最优先）

**目标：** 理解 kmalloc → SLUB → buddy allocator 的完整路径，定位分配/释放热点

```
Step 1: 观测工具
  - /proc/slabinfo — slab 缓存统计
  - /proc/vmstat — 页分配/释放计数
  - /sys/kernel/slab/ — SLUB 调优接口
  - perf kmem — 内存分配事件追踪
  - bpftrace kprobe:kmem_cache_alloc — 分配调用栈

Step 2: 核心源码阅读
  - mm/slub.c — SLUB 分配器（重点：per-CPU freelist, partial list）
  - mm/page_alloc.c — 伙伴系统 + per-CPU pageset
  - include/linux/mmzone.h — zone/node 结构和锁定义

Step 3: 锁竞争分析
  - zone->lock — 全局页分配锁
  - node->lru_lock — LRU 列表锁
  - slab 的 per-CPU 锁 vs node 锁
  - AMD CCD 拓扑下的锁竞争放大效应
```

### 模块 B：网络协议栈

**目标：** 理解数据包从网卡到应用的完整路径，定位软中断/协议处理瓶颈

```
Step 1: 观测工具
  - /proc/net/ — 网络统计
  - /proc/interrupts — 中断分布
  - /proc/softirqs — 软中断分布
  - ss -ti — TCP 连接详情
  - perf stat net 事件
  - bpftrace tcp/skb 追踪

Step 2: 核心源码阅读
  - net/core/dev.c — NAPI 收包框架
  - net/ipv4/tcp_input.c — TCP 收包处理
  - net/ipv4/tcp_output.c — TCP 发包处理
  - include/linux/skbuff.h — sk_buff 结构
  - net/core/skbuff.c — skb 分配/释放

Step 3: 性能瓶颈分析
  - NAPI 与中断合并
  - GRO/GSO/TSO 优化
  - RPS/RFS 软中断负载均衡
  - qdisc 锁竞争
```

### 模块 C：调度器

**目标：** 理解 CFS 调度决策，定位上下文切换和调度延迟问题

```
Step 1: 观测工具
  - vmstat (r/b 列) — 运行队列和阻塞队列
  - /proc/schedstat — 调度统计
  - /proc/sched_debug — 调度器调试信息
  - perf sched — 调度事件分析
  - bpftrace 调度追踪

Step 2: 核心源码阅读
  - kernel/sched/core.c — __schedule() 主函数
  - kernel/sched/fair.c — CFS 公平调度
  - kernel/sched/rt.c — 实时调度
  - include/linux/sched.h — task_struct 调度相关字段
```

### 模块 D：锁机制

**目标：** 理解各类内核锁的实现和性能特征，定位锁竞争根因

```
Step 1: 观测工具
  - perf lock — 锁等待统计
  - perf c2c — cache line 争用（false sharing）
  - bpftrace spinlock/mutex 追踪
  - /proc/locks — 文件锁

Step 2: 核心源码阅读
  - kernel/locking/spinlock.c — 自旋锁实现
  - kernel/locking/qspinlock.c — 队列自旋锁（queued spinlock）
  - kernel/locking/mutex.c — 互斥锁实现
  - kernel/locking/rwsem.c — 读写信号量
  - include/linux/spinlock.h — 锁 API

Step 3: 高级话题
  - ticket spinlock vs queued spinlock
  - RCU（Read-Copy-Update）原理
  - 无锁数据结构在内核中的应用
  - AMD CCD 拓扑对锁性能的影响
```

---

## 与已有文档的关系

```
docs/
├── methodology.md          ← 性能分析方法论（入口）
├── kernel_internals/       ← 本目录（内核子系统深入）
│   ├── learning-roadmap.md
│   └── reading/
├── perf/                   ← perf 工具使用
├── bpf/                    ← bpftrace 工具使用
├── topdown/                ← CPU 微架构瓶颈分析
├── cpu_arch/               ← CPU 架构知识
├── sar/                    ← 系统宏观指标
└── ...                     ← 其他工具
```

**阅读顺序建议：**

1. 先读 `methodology.md` — 掌握分析框架
2. 遇到具体瓶颈 → 用工具定位到内核子系统
3. 回到本目录 → 深入理解该子系统的源码
4. 结合 `topdown/` + `cpu_arch/` — 理解硬件层面的瓶颈

---

## 实验环境建议

```bash
# 1. 编译内核调试信息（如果发行版没有）
yum install -y kernel-debuginfo

# 2. 启用 perf 内核事件
echo 0 > /proc/sys/kernel/perf_event_paranoid

# 3. 安装 bpftrace
yum install -y bpftrace

# 4. 启用锁统计（如果需要）
# 内核配置 CONFIG_LOCK_STAT=y

# 5. 查看内核版本（影响源码路径）
uname -r
```
