# CPU 架构与内核 CPU 子系统 — 问答集

> 持续积累的问题与解答，涵盖 CPU 微架构、内核调度器、调优实践。
> 每个问题包含：日期、场景、相关文件、源码级解答。

---

<!-- 模板: 按以下格式追加新问答 -->

<!--
## Q{N}: {问题标题}

**日期：** YYYY-MM-DD
**场景：** 在哪里遇到的问题
**相关文件：** 对应的 reading 文件

### 回答
（源码级解答 + 代码引用 + 表格/图示）
-->

---

## Q1: AMD CCD 内 8 核共享 L3 Cache 对性能有什么影响？

**日期：** 2026-07-04
**场景：** 在 AMD EPYC 服务器上优化多线程应用，发现同一程序在不同线程分配策略下性能差异巨大
**相关文件：** reading/05_amd_microarchitecture.md

### 回答

**核心事实：** AMD Zen 3/4 的每个 CCD（Core Complex Die）包含 8 个核心和一个统一的 32MB L3 Cache。这个"8 核共享 32MB"有以下几层含义：

**1. 同 CCD 内：低延迟通信**

```
  CCD 内部（8 核 + 32MB L3）:

  C0 ←→ L3 ←→ C1     延迟: ~35 周期
  C0 ←→ L3 ←→ C7     延迟: ~40 周期（远端 slice）
  C0 ←→ L3 ←→ 同CCD任意核  延迟: 35-40 周期
```

CCD 内的 L3 在物理上分为多个 slice（每核约 4MB），但逻辑上对 CCD 内所有核统一可见。同 CCD 内任意两核通过 L3 通信只需 ~35 周期。

**2. 跨 CCD：高延迟通信**

```
  跨 CCD 通信路径:

  C0(CCD0) → GMI → I/O Die → GMI → C8(CCD1)
  延迟: ~80-120 周期（约 3-4 倍于同 CCD）

  ★ 关键点: 不同 CCD 的 L3 不互通
    CCD0 的 L3 对 CCD1 不可见
    跨 CCD 的数据共享必须经过 Infinity Fabric
    → 等效于一次 L3 miss + IF 往返
```

**3. 32MB 容量的实际含义**

```
  计算: 8 核共享 32MB = 每核约 4MB L3 配额

  场景分析:
  ┌──────────────────────┬─────────────┬──────────────────────┐
  │ 场景                  │ 总工作集    │ L3 miss 预期          │
  ├──────────────────────┼─────────────┼──────────────────────┤
  │ 8 核各 2MB 工作集    │ 16MB        │ 低（16 < 32）        │
  │ 8 核各 5MB 工作集    │ 40MB        │ 高（40 > 32）        │
  │ 8 核共享 20MB 数据   │ 20MB        │ 中（共享但 < 32）    │
  │ 8 核各 10MB + 共享   │ 80MB+       │ 极高                │
  └──────────────────────┴─────────────┴──────────────────────┘
```

**4. 性能影响量化**

| 通信场景 | 延迟 | 带宽 | 优化建议 |
|---------|------|------|---------|
| 同核 SMT 线程 | ~0 额外 | 共享 L1/L2 | 适合紧密耦合线程 |
| 同 CCD 内核间 | ~35 周期 | 共享 L3 | ★ 协作线程应绑定到同 CCD |
| 跨 CCD（同 socket） | ~100 周期 | 受 IF 带宽限制 | 避免跨 CCD 共享数据 |
| 跨 socket | ~180 周期 | 受 xGMI 带宽限制 | 避免跨 socket 共享 |

**5. 实际优化建议**

```bash
# 查看 CCD 拓扑（确定哪些核在同一个 CCD）
lscpu -e
# 或
cat /sys/devices/system/cpu/cpu*/topology/core_siblings_list

# 将协作的 8 个线程绑定到同一 CCD
# 以 EPYC 7763 为例，CCD0 = CPU 0-7，CCD1 = CPU 8-15...
taskset -c 0-7 ./my_8thread_program

# 验证绑定效果
perf stat -e LLC-load-misses -- taskset -c 0-7 ./program  # 同 CCD
perf stat -e LLC-load-misses -- taskset -c 0,8,16,24,32,40,48,56 ./program  # 跨 CCD
# 预期: 跨 CCD 的 LLC-load-misses 显著更高
```

---

## Q2: 如何区分 L3 cache miss 导致 CPU 高占用 vs CPU 抢占导致 cache miss？

**日期：** 2026-07-04
**场景：** 服务器 CPU 利用率 90%+，L3 miss 率 35%+，不确定是数据访问模式导致 miss，还是频繁上下文切换污染了缓存
**相关文件：** reading/06_l3_cache_miss_analysis.md

### 回答

这是两个截然不同的因果链，但表现相似。核心诊断方法如下：

**因果链 A：数据问题 → L3 miss → CPU 等待 → 高占用低吞吐**
```
  工作集过大/随机访问 → L3 miss 率高 → DRAM 延迟 → 流水线 stall → CPU 忙但无产出
```

**因果链 B：调度问题 → 上下文切换 → cache 污染 → L3 miss 升高**
```
  过度订阅/频繁迁移 → context_switch → TLB flush + L1/L2 cold → L3 miss 升高
```

**★ 核心区分方法（4 步）：**

**Step 1: 检查上下文切换率**

```bash
vmstat 1
# 看 cs 列（上下文切换/秒）
# cs < 5000  → 基本排除抢占因素 → 倾向链 A
# cs > 10000 → 抢占可能是主因 → 倾向链 B
```

**Step 2: 对比实验——绑定 CPU**

```bash
# 实验 A: 不绑定（基线）
perf stat -e cache-misses,cache-references -- ./program

# 实验 B: 绑定到固定 CPU
perf stat -e cache-misses,cache-references -- taskset -c 0-7 ./program

# 判断:
# 绑定后 miss 率显著下降 (> 20%) → 链 B（迁移导致 miss）
# 绑定后 miss 率不变 → 链 A（数据模式本身的问题）
```

**Step 3: 观察 miss 率的时域特征**

```bash
perf stat -e cache-misses,cache-references -I 1000 -- ./program

# 链 A 特征: miss 率持续高（稳态）
#   1.0s: 35%  2.0s: 36%  3.0s: 35%  ...

# 链 B 特征: miss 率呈尖峰模式（与切换同步）
#   1.0s: 15%  2.0s: 45%  3.0s: 20%  4.0s: 50%  ...
```

**Step 4: perf c2c 看 miss 地址分布**

```bash
perf c2c record -- ./program
perf c2c report

# 链 A: miss 集中在少数热点地址（数据结构本身大）
# 链 B: miss 分散在大量地址（每次切换后不同数据的 cold miss）
```

**综合判断表：**

| 诊断维度 | 链 A（数据问题） | 链 B（抢占问题） |
|---------|-----------------|-----------------|
| vmstat cs | < 5K/s | > 10K/s |
| 绑定 CPU 后 miss | 不变 | 显著下降 |
| miss 率时域 | 持续高 | 尖峰波动 |
| perf c2c 热点 | 集中 | 分散 |
| %system 时间 | 正常 (< 10%) | 偏高 (> 15%) |
| IPC 稳定性 | 稳定低 | 波动大 |

---

## Q3: 如何定位自旋锁竞争？

**日期：** 2026-07-04
**场景：** 服务器 CPU 利用率高但吞吐低，怀疑存在内核自旋锁竞争
**相关文件：** reading/07_spinlock_analysis.md

### 回答

自旋锁竞争的典型表现是"CPU 忙但无产出"——多个 CPU 在 `_raw_spin_lock` 中自旋等待，消耗周期但不退休有效指令。

**★ 3 步定位法：**

**Step 1: perf record 看热点函数**

```bash
perf record -e cycles:pp -g -- ./program
perf report --stdio --sort=symbol

# ★ 关注这些函数:
# _raw_spin_lock_irqsave     — 自旋锁 + 关中断
# queued_spin_lock_slowpath  — 队列自旋锁慢路径（等待中）
# native_queued_spin_lock_slowpath — 架构特定慢路径
#
# 如果这些函数占比 > 5%，存在显著的锁竞争
# 如果占比 > 15%，锁竞争是主要瓶颈
```

**Step 2: perf lock 看锁统计**

```bash
# 记录锁事件
perf lock record -- ./program

# 分析竞争
perf lock report --sort=wait

# 关键输出:
#                Name      acquired  contended  avg wait(ns)
#  &(&zone->lock)->rlock    50000      8000      12500   ★ 页分配器锁
#  &(&q->__queue_lock)->..  30000      5000       8000   ★ 块设备队列锁
#
# contended 高 + avg wait 高 = 严重竞争
```

**Step 3: 调用链分析确定根因**

```bash
perf record -e cycles:pp -g --call-graph dwarf -- ./program
perf report --stdio --sort=symbol,dso --call-graph fractal

# 输出示例:
# - 15.30% _raw_spin_lock_irqsave
#    - 12.10% __alloc_pages_nodemask  ← 页分配器锁
#    -  2.50% do_epoll_wait            ← epoll 锁
#    -  0.70% tcp_sendmsg              ← TCP 发送锁
```

**补充：bpftrace 追踪锁等待时间分布**

```bash
bpftrace -e '
kprobe:queued_spin_lock_slowpath {
    @start[tid] = nsecs;
}
kretprobe:queued_spin_lock_slowpath /@start[tid]/ {
    $wait = nsecs - @start[tid];
    @wait_ns = hist($wait);
    delete(@start[tid]);
}'

# 如果出现 > 2μs 的等待 → 严重竞争
```

**常见锁竞争根因与优化：**

| 锁热点 | 根因 | 优化方案 |
|--------|------|---------|
| `__alloc_pages` | 页分配器竞争 | 使用大页（减少分配次数） |
| `kmem_cache_alloc` | slab 分配器竞争 | per-CPU 缓存池 |
| `tcp_sendmsg` / `netif_rx` | 网络栈锁 | 多队列网卡 + RPS/RFS |
| `ext4/xfs` 内部锁 | 文件系统锁 | 不同挂载点 / io_uring |

**★ Ticket Spinlock vs Queued Spinlock 的影响：**

现代 Linux（x86_64）默认使用 Queued Spinlock，相比旧的 Ticket Spinlock：
- Ticket: 所有等待 CPU 轮询同一变量 → 每次 unlock 产生 N-1 次 cache invalidation
- Queued: 每 CPU 轮询自己的变量 → 每次 unlock 仅 1 次 cache invalidation
- 高竞争场景下减少 ~85% 的 cache coherence traffic
