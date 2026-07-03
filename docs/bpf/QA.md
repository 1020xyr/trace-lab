# BPF/eBPF (bcc + bpftrace + libbpf) 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: 如何用 bpftrace 追踪自旋锁竞争？](#q1-如何用-bpftrace-追踪自旋锁竞争)
- [Q2: 如何用 bpftrace 分析软中断 CPU 占用？](#q2-如何用-bpftrace-分析软中断-cpu-占用)

---

## Q1: 如何用 bpftrace 追踪自旋锁竞争？

**日期：** 2026-07-04
**场景：** 系统 %system 高但吞吐低，怀疑自旋锁竞争
**相关文件：** `reading/05_performance_diagnostics.md`

### 回答

通过 hook `_raw_spin_lock` / `_raw_spin_unlock` 来追踪自旋锁的等待时间和调用栈。

**方法 1：自旋锁等待时间分布**

```bash
bpftrace -e '
  kprobe:_raw_spin_lock { @lock_start[tid] = nsecs; }
  kretprobe:_raw_spin_lock /@lock_start[tid]/ {
    @wait_us = hist((nsecs - @lock_start[tid]) / 1000);
    delete(@lock_start[tid]);
  }'
```

**原理：**
- `kprobe:_raw_spin_lock` — 进入自旋锁时记录时间
- `kretprobe:_raw_spin_lock` — 获取到锁后计算等待时间
- 如果锁未被持有，等待时间 ≈ 0（直接获取）
- 如果锁被其他 CPU 持有，等待时间 = 自旋等待的时间

**方法 2：找出哪些调用栈最频繁竞争**

```bash
bpftrace -e '
  kprobe:_raw_spin_lock { @[kstack] = count(); }'
```

输出中排名靠前的调用栈就是自旋锁热点。

**方法 3：自旋锁持有时间（找出长持有者）**

```bash
bpftrace -e '
  kprobe:_raw_spin_lock { @acq[tid] = nsecs; }
  kprobe:_raw_spin_unlock /@acq[tid]/ {
    @hold_us = hist((nsecs - @acq[tid]) / 1000);
    delete(@acq[tid]);
  }'
```

**解读：**
- `@wait_us` 峰值在 [0] → 大部分获取无竞争（正常）
- `@wait_us` 有 [128, 256) 或更高桶 → ★ 存在严重的锁争用
- `@hold_us` 峰值 > 100μs → 临界区太长，考虑拆分

**★ 注意事项：**
- kprobe 不能 hook inline 函数。如果 `_raw_spin_lock` 被内联，需要
  hook 到 `do_raw_spin_lock` 或 `native_queued_spin_lock_slowpath`
- `native_queued_spin_lock_slowpath` 只在锁竞争时才被调用，
  所以 `perf top` 中看到它排名靠前就是竞争的直接证据

---

## Q2: 如何用 bpftrace 分析软中断 CPU 占用？

**日期：** 2026-07-04
**场景：** sar -u ALL 显示 %soft 高，需要定位是哪个 CPU 和哪种软中断
**相关文件：** `reading/05_performance_diagnostics.md`

### 回答

通过 hook `__do_softirq` 内核函数来追踪软中断的处理时间和 CPU 分布。

**方法 1：软中断处理时间分布**

```bash
bpftrace -e '
  kprobe:__do_softirq { @start = nsecs; }
  kretprobe:__do_softirq /@start/ {
    @softirq_us = hist((nsecs - @start) / 1000);
    @start = 0;
  }'
```

**方法 2：按 CPU 统计软中断频率和时间**

```bash
bpftrace -e '
  kprobe:__do_softirq { @start[cpu] = nsecs; }
  kretprobe:__do_softirq /@start[cpu]/ {
    @total_us[cpu] = sum((nsecs - @start[cpu]) / 1000);
    @max_us[cpu] = max((nsecs - @start[cpu]) / 1000);
    @count[cpu] = count();
    delete(@start[cpu]);
  }'
```

**方法 3：网络软中断专项追踪**

```bash
bpftrace -e '
  kprobe:net_rx_action { @start = nsecs; }
  kretprobe:net_rx_action /@start/ {
    @net_rx_us = hist((nsecs - @start) / 1000);
    @start = 0;
  }'
```

**解读：**

| 指标 | 正常 | 异常 |
|------|------|------|
| `@softirq_us` 峰值 | [1] ~ [4] μs | > [64] μs |
| `@count` 各 CPU 分布 | 均匀 | 集中在某 CPU |
| `@net_rx_us` | < 10μs | > 100μs |

**★ 常见问题模式：**
- 某 CPU 软中断远高于其他 → 中断亲和性问题 → 启用 irqbalance
- `@softirq_us` 双峰分布 → 突发流量导致偶尔长时间处理
- `net_rx_action` 处理时间 > 1ms → 网络包风暴或驱动问题

---

*新的问题将追加到此文件。每个问题记录日期、场景、相关文件。*
