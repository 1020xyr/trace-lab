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
>   - src/linux-5.10/kernel/locking/           — 内核锁实现
>
> 预计阅读时间：15 分钟
> ======================================================================

---

## 一、内核锁的类型

### 1.1 锁类型与性能特征

```
锁类型              │ 等待行为             │ 适用场景          │ 性能影响
───────────────────┼─────────────────────┼─────────────────┼──────────────
spinlock           │ ★ 忙等（自旋）       │ 短临界区          │ 高 CPU 消耗
                   │ while(locked);       │ （< 几十微秒）    │ 不释放 CPU
rwlock             │ 读不互斥，写互斥     │ 读多写少          │ 中等
mutex              │ 睡眠等待             │ 长临界区          │ 低 CPU 消耗
                   │ schedule()           │ （可睡眠上下文）  │ 但可能阻塞
semaphore          │ 计数型睡眠等待       │ 资源池            │ 中等
rcu                │ 无锁读，延迟释放     │ 高频读            │ ★ 读端零开销
```

### 1.2 自旋锁的性能问题

```
自旋锁等待时：
  CPU 0 持有锁 → CPU 1 自旋等待（while 循环）→ CPU 1 100% 占用但无产出
  CPU 0 持有锁 → CPU 2 自旋等待 → CPU 2 100% 占用但无产出

  ★ 症状：
    - CPU 使用率高（%system 高）
    - 但吞吐低（线程在忙等，不是在做有用的工作）
    - perf top 看到 native_queued_spin_lock_slowpath 占用高

  ★ 这就是"CPU 高占用低吞吐"的经典场景之一！
```

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
    > 5% → 有明显竞争
    > 20% → 严重竞争
  - total wait 大 → 大量 CPU 时间在等锁
  - max wait 大 → 可能有锁持有时间过长的情况
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
# > 5% → 有显著的锁竞争

# 方法 3：perf lock 精确分析
perf lock record -a sleep 10
perf lock report --stdio
# 查看 contended 最高的锁
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
减小临界区              │ 通用                    │ 低
读写锁替代互斥锁        │ 读多写少                │ 低
per-CPU 变量            │ 聚合计数/统计           │ 中
RCU 替代读写锁          │ 高频读、低频更新        │ 高
无锁数据结构            │ 特定场景（队列/计数器） │ 高
细粒度锁                │ 大锁拆分为多个小锁      │ 中
```

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
  native_queued_spin_lock_slowpath  → 自旋锁慢路径（争用标志）
  _raw_spin_lock                    → 自旋锁入口
  mutex_lock                        → 互斥锁
  rwsem_down_read_slowpath          → 读写信号量慢路径
```

### 源码位置

```
perf lock:     src/linux-5.10/tools/perf/builtin-lock.c
内核锁实现:    src/linux-5.10/kernel/locking/spinlock.c
               src/linux-5.10/kernel/locking/mutex.c
               src/linux-5.10/kernel/locking/rwsem.c
锁 tracepoint: src/linux-5.10/include/trace/events/lock.h
```
