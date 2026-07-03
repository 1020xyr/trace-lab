# perf 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: 如何用 perf 判断是 L3 cache miss 还是 CPU 计算瓶颈？](#q1-如何用-perf-判断是-l3-cache-miss-还是-cpu-计算瓶颈)
- [Q2: perf c2c 如何检测 false sharing？](#q2-perf-c2c-如何检测-false-sharing)

---

## Q1: 如何用 perf 判断是 L3 cache miss 还是 CPU 计算瓶颈？

**日期：** 2026-07-04
**场景：** AMD 服务器 CPU 高占用低吞吐，需要区分是 cache miss 导致还是计算效率低
**相关文件：** `reading/06_hw_counters_diagnosis.md`

### 回答

核心方法是用 `perf stat -d` 获取 **IPC** 和 **cache-miss 率**，通过两者组合判断：

```bash
perf stat -d -r 3 ./app
```

**诊断矩阵：**

| IPC | cache-miss 率 | 结论 |
|-----|-------------|------|
| > 1.5 | < 5% | ★ CPU 计算密集（正常） |
| > 1.5 | > 15% | 混合型：计算多且有 cache 问题 |
| < 1.0 | < 5% | 其他停顿（分支预测/TLB/前端） |
| < 1.0 | > 15% | ★ **L3 cache miss 是瓶颈** |
| < 0.5 | > 30% | ★★ **严重 L3 cache miss** |

**关键指标计算：**

```bash
# IPC = instructions / cycles
# cache-miss 率 = cache-misses / cache-references × 100%

# 进一步区分 L1 miss 和 L3 miss
perf stat -e L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses,instructions,cycles ./app

# L1 miss 率 = L1-dcache-load-misses / L1-dcache-loads
# L3 miss 率 = LLC-load-misses / LLC-loads

# ★ L3 miss 的代价远大于 L1 miss：
#   L1 miss → L2/L3 命中 → ~15 周期
#   L3 miss → 主存 → ~300 周期
#   → L3 miss 率 > 10% 就已经很严重
```

**前端/后端停顿辅助判断：**

```bash
perf stat -e stalled-cycles-frontend,stalled-cycles-backend,cycles ./app
```

| 停顿类型 | 含义 | 与 cache miss 的关系 |
|---------|------|---------------------|
| stalled-frontend 高 | 指令获取跟不上 | I-cache miss / 分支预测失败 |
| stalled-backend 高 | 数据访问跟不上 | ★ D-cache miss / L3 miss / 内存带宽 |

**★ 结论：**
- IPC 低 + cache-miss 率高 + stalled-backend 高 → **L3 cache miss 瓶颈**
- IPC 高 + cache-miss 率低 → **CPU 计算瓶颈**（优化算法）
- IPC 低 + cache-miss 率低 → 其他原因（检查分支预测、TLB、锁竞争）

---

## Q2: perf c2c 如何检测 false sharing？

**日期：** 2026-07-04
**场景：** 多线程程序增加线程数后性能反而下降，怀疑 false sharing
**相关文件：** `reading/07_c2c_false_sharing.md`

### 回答

`perf c2c` 通过 **HITM（Hit Modified）** 指标检测 false sharing。

**使用方法：**

```bash
# 采集
perf c2c record ./app

# 分析
perf c2c report --stdio
```

**原理：**

```
perf c2c record 使用 PEBS（Intel）或 IBS（AMD）的内存采样功能，
记录每次内存访问的：
  - 物理地址 → 计算 cache line 地址（64 字节对齐）
  - 访问类型（load/store）
  - 数据来源（L1/L2/L3/DRAM）
  - 是否是 HITM（该 cache line 是否被其他 CPU 修改过）

★ HITM（Hit Modified）= 本 CPU 加载一个 cache line 时，
  发现它被其他 CPU 修改过，需要从其他 CPU 的缓存中获取最新数据。
  这就是 Cache-to-Cache (c2c) 传输。
```

**判断 false sharing 的关键信号：**

```
perf c2c report 输出中：

  Shared Data Cache Line Table:
    某个 cache line 地址 → HITM 次数高 → ★ false sharing

  特征：
    1. 多个 CPU 同时 store 到同一个 cache line
    2. 大量 LLC Load HITM
    3. store 的变量逻辑上无关，但物理上在同一 cache line
```

**典型 false sharing 代码模式：**

```c
// false sharing：两个线程写相邻变量
struct { long counter_a; long counter_b; } shared;

// CPU 0: shared.counter_a++  → cache line @0x1000
// CPU 1: shared.counter_b++  → 同一个 cache line @0x1000！
// → 每次写都会 invalidate 对方的缓存 → 性能下降 100x

// 修复：cache line 对齐
struct __attribute__((aligned(64))) {
    long counter;
    char padding[56];
} per_cpu_counters[NR_CPUS];
```

**★ 注意：** `perf c2c` 需要物理机（PEBS/IBS 支持），在 KVM 虚拟机中可能不可用。

---

*新的问题将追加到此文件。每个问题记录日期、场景、相关文件。*
