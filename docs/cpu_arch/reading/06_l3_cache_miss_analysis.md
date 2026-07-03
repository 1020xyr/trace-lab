<!--
======================================================================
  Step 6: L3 Cache Miss 专题分析
======================================================================
  来源: Linux perf 文档、Intel SDM、AMD PPR、Brendan Gregg 性能分析
  阅读要点:
    - 用 perf 测量 L3 cache miss 率
    - L3 miss → 内存带宽瓶颈 → CPU 等待 → 高 %system 低吞吐 的因果链
    - CPU 抢占 → 上下文切换 → TLB flush → cache pollution 的反向链
    - 如何区分两种因果关系
    - perf c2c、perf mem 的使用方法
  预计时间: 25-35 分钟
======================================================================
-->

# L3 Cache Miss 专题分析

> L3 Cache Miss 是服务器性能问题中最常见也最容易误判的根因之一。
> 本文从测量方法、因果链分析、诊断工具三个维度展开。

---

## 目录

- [1. ★ L3 Cache Miss 的测量方法](#1--l3-cache-miss-的测量方法)
- [2. 因果链 A：L3 Miss → CPU 高占用低吞吐](#2-因果链-al3-miss--cpu-高占用低吞吐)
- [3. 因果链 B：CPU 抢占 → Cache Miss 升高](#3-因果链-bcpu-抢占--cache-miss-升高)
- [4. ★ 如何区分两种因果关系](#4--如何区分两种因果关系)
- [5. perf c2c：Cache-to-Cache 分析](#5-perf-c2ccache-to-cache-分析)
- [6. perf mem：内存访问采样](#6-perf-mem内存访问采样)
- [7. 实战诊断流程](#7-实战诊断流程)

---

## 1. ★ L3 Cache Miss 的测量方法

### 1.1 基本测量：perf stat

```bash
# 方法 1: 通用硬件事件（Intel/AMD 通用）
perf stat -e cache-references,cache-misses -- ./program

# cache-references = LLC (Last Level Cache) 访问次数
# cache-misses     = LLC miss 次数
# miss 率 = cache-misses / cache-references

# 方法 2: 更精细的 LLC 事件
perf stat -e \
  LLC-loads,LLC-load-misses,\
  LLC-stores,LLC-store-misses \
  -- ./program

# ★ LLC-load-misses: 加载时 L3 miss → 需要访问 DRAM（高延迟惩罚）
# ★ LLC-store-misses: 存储时 L3 miss → 需要分配缓存行（相对影响小）
```

### 1.2 各级缓存 miss 率测量

```bash
# 完整的缓存层次分析
perf stat -e \
  L1-dcache-loads,L1-dcache-load-misses,\
  l2_rqst:all,l2_rqst:miss,\
  LLC-loads,LLC-load-misses \
  -- ./program

# AMD 上的等效命令（事件名可能不同）
perf stat -e \
  L1-dcache-loads,L1-dcache-load-misses,\
  l2_request:all,l2_cache_miss_from_l2_cache_miss,\
  LLC-loads,LLC-load-misses \
  -- ./program
```

### 1.3 ★ 关键指标与阈值

| 指标 | 健康范围 | 警告范围 | 严重问题 | 说明 |
|------|---------|---------|---------|------|
| L1 miss 率 | < 5% | 5-10% | > 10% | 工作集过大或访问模式差 |
| L2 miss 率 | < 10% | 10-20% | > 20% | 数据局部性差 |
| **L3 miss 率** | < 20% | 20-40% | **> 40%** | ★ 主要瓶颈指标 |
| MPKI (Misses Per Kilo Instructions) | < 5 | 5-20 | > 20 | 每千条指令的 L3 miss 数 |

```
  ★ MPKI 计算:
  MPKI = LLC-load-misses × 1000 / instructions

  示例:
  LLC-load-misses = 50,000,000
  instructions    = 10,000,000,000
  MPKI = 50,000,000 × 1000 / 10,000,000,000 = 5.0

  → MPKI < 5: 内存子系统压力小
  → MPKI > 20: 严重的内存瓶颈
```

### 1.4 时间序列分析

```bash
# 每秒打印一次缓存 miss 指标（观察 miss 率是否持续高）
perf stat -e cache-references,cache-misses -I 1000 -- ./program

# 输出示例:
#     1.001 sec   12,345,678  cache-references   1,234,567  cache-misses  (10.0%)
#     2.002 sec   45,678,901  cache-references  18,271,560  cache-misses  (40.0%) ★
#     3.003 sec   44,567,890  cache-references  17,827,156  cache-misses  (40.0%) ★
#
# → 第 1 秒 miss 率低（warm-up），后续持续 40% → 稳态瓶颈
```

---

## 2. 因果链 A：L3 Miss → CPU 高占用低吞吐

### 2.1 ★ 完整因果链

```
  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
  │ L3 Cache     │     │ 内存带宽     │     │ CPU 流水线    │
  │ Miss 率高    │────→│ 瓶颈         │────→│ Stall        │
  │ (> 30%)      │     │              │     │              │
  └──────────────┘     └──────┬───────┘     └──────┬───────┘
                              │                     │
                              ▼                     ▼
                     ┌──────────────┐     ┌──────────────┐
                     │ DRAM 访问    │     │ CPU 利用率高  │
                     │ 延迟 ~200 cy │     │ 但吞吐低     │
                     │              │     │ ★ 核心矛盾   │
                     └──────────────┘     └──────────────┘
```

**逐步解析：**

```
  Step 1: 程序访问的数据不在 L3 Cache 中
         → CPU 发出 DRAM 读取请求
         → 延迟: ~200 周期（约 70-80ns @ 2.5GHz）

  Step 2: 多个核心同时产生 L3 miss
         → DRAM 带宽饱和（典型服务器: ~50-100 GB/s）
         → 后续请求排队等待

  Step 3: CPU 流水线等待数据
         → Load 指令无法完成 → 依赖该数据的后续指令全部阻塞
         → ROB 填满 → 前端停止取指 → 流水线空转

  Step 4: CPU 占用率显示 100%
         → 但实际有效工作（Retiring）可能只有 15-25%
         → 75%+ 的时间在等待内存数据
         → 表现为"CPU 忙但吞吐低"
```

### 2.2 特征识别

| 指标 | 特征值 | 说明 |
|------|--------|------|
| CPU 利用率 | 高 (> 80%) | top/mpstat 显示 CPU 忙 |
| IPC | ★ 低 (< 0.8) | 每周期完成的指令少 |
| L3 miss 率 | 高 (> 30%) | 大量 DRAM 访问 |
| Backend Bound | 高 (> 50%) | 后端等待数据 |
| Memory Bound | 高 (占 Backend > 70%) | 瓶颈在内存子系统 |
| 内存带宽 | 接近饱和 | perf/sar 显示高带宽利用率 |

### 2.3 常见触发场景

```
  场景 1: 大内存数据库查询
  ─────────────────────
  查询扫描 100GB 数据集 → 远超 L3 容量(32MB)
  → 持续 L3 miss → 持续 DRAM 访问 → CPU 等待

  场景 2: 随机内存访问（链表/哈希表遍历）
  ─────────────────────────────────────
  指针追踪: p = p->next
  每次访问新地址 → cache miss → 200 周期延迟
  → 无法乱序（串行依赖）→ IPC 极低 (~0.2)

  场景 3: 多核竞争共享 L3
  ──────────────────────
  8 核各 8MB 工作集 → 总需求 64MB > L3 32MB
  → 缓存行频繁被替换 → 高 conflict miss
```

---

## 3. 因果链 B：CPU 抢占 → Cache Miss 升高

### 3.1 ★ 完整因果链（反向因果）

```
  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
  │ 频繁的 CPU   │     │ 上下文切换    │     │ TLB/Cache    │
  │ 抢占/迁移    │────→│ (context     │────→│ 污染         │
  │              │     │  switch)     │     │              │
  └──────────────┘     └──────────────┘     └──────┬───────┘
                                                    │
                     ┌──────────────┐     ┌────────┴───────┐
                     │ L3 Miss 率   │     │ Cache miss     │
                     │ 升高         │◄────│ 增加            │
                     │              │     │ (cold miss)    │
                     └──────────────┘     └────────────────┘
```

**逐步解析：**

```
  Step 1: 任务被调度器抢占或在 CPU 间迁移
         → 触发 context_switch()

  Step 2: 上下文切换的缓存影响
         ├── switch_mm(): 切换页表
         │   → CR3 寄存器更新 → TLB 全部失效（不用 PCID 时）
         │   → 后续内存访问需要 Page Walk → ~100+ 周期
         │
         ├── 新任务的工作集不在 L1/L2 中
         │   → cold miss（冷启动缺失）
         │   → 前几百次内存访问几乎全部 miss
         │
         └── 旧任务的数据占据 L3 空间
             → 新任务的工作集与旧任务不同
             → capacity miss（容量缺失）增加

  Step 3: 切换后一段时间内 miss 率升高
         → "warm-up penalty" — 缓存重新预热
         → 通常持续 10-100μs（取决于工作集大小）

  Step 4: 如果切换频繁（如 1000 次/秒）
         → 累计的 warm-up penalty 显著
         → 观察到持续的高 L3 miss 率
         → 但根因不是数据访问模式，而是调度行为
```

### 3.2 特征识别

| 指标 | 特征值 | 说明 |
|------|--------|------|
| 上下文切换率 | ★ 高 (> 10K/s) | vmstat 的 cs 列 |
| CPU 迁移次数 | 高 | /proc/schedstat 的 nr_migrations |
| L3 miss 率 | 中高 (15-40%) | 但不如场景 A 那么极端 |
| miss 率的时域特征 | ★ 尖峰模式 | 每次切换后出现 miss 尖峰 |
| 任务运行时间片段 | 短 | 任务频繁被抢占，运行时间短 |
| %system 时间 | 偏高 | 内核态时间（切换开销） |

### 3.3 常见触发场景

```
  场景 1: 过度订阅（over-subscription）
  ────────────────────────────────
  可运行任务数 >> CPU 核数
  → 调度器频繁抢占和切换
  → 每次切换 → cache 污染 → miss 升高

  场景 2: 锁竞争导致的阻塞-唤醒风暴
  ─────────────────────────────
  线程 A 等锁 → 阻塞 → 线程 B 获得锁 → 运行
  → 线程 A 被唤醒 → 抢占线程 B → 反复切换
  → 两个线程的 cache 互相驱逐

  场景 3: CPU 亲和性设置不当
  ──────────────────────
  没有绑定 CPU → 调度器在多个核之间迁移任务
  → 每次迁移 → 新核的 L1/L2 是冷的 → miss 升高
  → 特别是跨 CCD 迁移 → L3 也是冷的（AMD）
```

---

## 4. ★ 如何区分两种因果关系

### 4.1 决策树

```
  L3 Cache Miss 率高
       │
       ▼
  ┌─ Step 1: 检查上下文切换率 ──────────────────────┐
  │  vmstat 1 → 看 cs 列                             │
  │                                                    │
  │  cs < 5000/s → 切换到 Step 2（可能不是抢占问题） │
  │  cs > 10000/s → 抢占可能是根因，继续 Step 3      │
  └────────────────────────────────────────────────────┘
       │
       ▼
  ┌─ Step 2: 检查工作集大小 ────────────────────────┐
  │  估算: 线程数 × 每线程工作集 vs L3 容量           │
  │                                                    │
  │  工作集 >> L3 → 因果链 A（数据本身太大）          │
  │  工作集 ≤ L3 → 切换到 Step 3                      │
  └────────────────────────────────────────────────────┘
       │
       ▼
  ┌─ Step 3: 用 perf c2c 看 miss 的时域特征 ───────┐
  │  perf c2c record -- ./program                    │
  │  perf c2c report                                  │
  │                                                    │
  │  如果 miss 集中在少数地址 → 热点数据问题（链 A）  │
  │  如果 miss 分散在大量地址 → 可能是切换污染（链 B）│
  └────────────────────────────────────────────────────┘
       │
       ▼
  ┌─ Step 4: 绑定 CPU 对照实验 ────────────────────┐
  │  实验 A: taskset 绑定到固定 CPU                  │
  │  实验 B: 不绑定（允许迁移）                       │
  │                                                    │
  │  绑定后 miss 率显著下降 → 链 B（抢占/迁移导致）  │
  │  绑定后 miss 率不变 → 链 A（数据访问模式问题）   │
  └────────────────────────────────────────────────────┘
```

### 4.2 ★ 区分指标对照表

| 诊断维度 | 因果链 A（数据问题） | 因果链 B（抢占问题） |
|---------|---------------------|---------------------|
| vmstat cs (上下文切换/s) | 正常 (< 5K) | ★ 高 (> 10K) |
| /proc/schedstat nr_migrations | 正常 | ★ 高 |
| %system 时间 | 正常 (< 10%) | ★ 偏高 (> 15%) |
| 绑定 CPU 后 miss 变化 | ★ 不变 | ★ 显著下降 |
| perf c2c 热点分布 | 集中在少数地址 | 分散在大量地址 |
| LLC-load-miss vs LLC-store-miss | load-miss 为主 | 两者都高 |
| IPC | 低但稳定 | 低且波动大 |
| 时间序列 miss 率 | 持续高 | 尖峰模式（与切换同步） |

### 4.3 ★ 关键命令：对比实验

```bash
# 实验 A: 不绑定 CPU（基线）
perf stat -e cache-misses,cache-references,cs \
  -- ./program

# 实验 B: 绑定到同一 CCD 的 8 个核心
perf stat -e cache-misses,cache-references,cs \
  -- taskset -c 0-7 ./program

# 实验 C: 减少并发任务数（减少抢占）
perf stat -e cache-misses,cache-references,cs \
  -- taskset -c 0-3 ./program  # 只用 4 核

# ★ 如果 B vs A 的 miss 率显著下降:
#    → 跨 CCD 迁移是 miss 的主要原因（链 B）
#
# ★ 如果 C vs A 的 miss 率显著下降:
#    → 过度订阅（over-subscription）是 miss 的主要原因（链 B）
#
# ★ 如果 B、C 与 A 的 miss 率无显著差异:
#    → 数据访问模式本身是问题（链 A）
```

---

## 5. perf c2c：Cache-to-Cache 分析

### 5.1 ★ perf c2c 是什么

`perf c2c` 专门用于分析**多核之间的缓存行共享和竞争**，能识别：
- False Sharing（伪共享）
- Cache Line Bouncing（缓存行在核间反复迁移）
- 热点内存地址的访问模式

### 5.2 使用方法

```bash
# 记录（使用 PEBS/IBS 精确采样）
perf c2c record -- ./program
# 等效于:
# perf record -e mem-loads,mem-stores -W -d -- ./program

# 分析报告
perf c2c report

# 关键输出:
# ┌────────────────────────────────────────────────────┐
# │  Shared Data Cache Line Table                       │
# │                                                      │
# │  Cacheline         Hitm%  FBAcc  LdMiss  StMiss     │
# │  0x7f1234567000    45.2%  1234   5678    234        │ ← ★ 热点
# │  0x7f1234568000    23.1%  890    2345    123        │
# │  ...                                                 │
# │                                                      │
# │  ★ Hitm%: HIT Modified — 访问的缓存行被其他核修改   │
# │    Hitm% 高 → 伪共享 / 缓存行竞争                    │
# └────────────────────────────────────────────────────┘
```

### 5.3 关键指标解读

| 指标 | 含义 | 高值的含义 |
|------|------|-----------|
| Hitm% | 访问时缓存行被其他核修改的比例 | ★ 伪共享/真共享竞争 |
| LdMiss | Load miss 次数 | 数据不在本地缓存 |
| StMiss | Store miss 次数 | 需要独占缓存行 |
| FBAcc | Fast Block Access（快速连续访问） | 热点数据 |
| RMT Hitm | 远端 NUMA 节点的 Hit Modified | 跨 NUMA 伪共享 |

---

## 6. perf mem：内存访问采样

### 6.1 使用方法

```bash
# 记录内存访问的延迟信息
perf mem record -- ./program
# 等效于:
# perf record -e mem-loads,mem-stores -W -d -- ./program

# 报告（按延迟排序）
perf mem report --sort=mem,lat,dso,symbol

# 报告（按缓存层级分组）
perf mem report --sort=mem

# 输出中的 mem 列含义:
# L1 hit    → L1 缓存命中 (~4 周期)
# L2 hit    → L2 缓存命中 (~12 周期)
# L3 hit    → L3 缓存命中 (~40 周期)
# L3 miss   → 主存访问 (~200 周期) ★
# Remote L3 → 远端 NUMA 节点的 L3 命中 (~120 周期)
# Remote RAM→ 远端 NUMA 节点的主存访问 (~280 周期)
```

### 6.2 ★ 延迟分布分析

```bash
# 统计各级缓存的访问比例
perf mem report --sort=mem --stdio

# 输出示例:
#   Overhead  Memory access
#     45.23%  L3 miss        ← ★ 45% 的访问需要访问 DRAM
#     23.45%  L3 hit          ← 23% 在 L3 命中
#     18.67%  L2 hit
#     10.12%  L1 hit
#      2.53%  Remote RAM      ← 2.5% 跨 NUMA 访问

# ★ 诊断:
# L3 miss > 30% → 内存瓶颈（因果链 A）
# Remote RAM > 10% → NUMA 本地性问题
# L1 hit < 50% → 工作集过大或访问模式差
```

---

## 7. 实战诊断流程

### 7.1 ★ 端到端诊断步骤

```
  ┌─────────────────────────────────────────────────────────────┐
  │  Step 1: 初步判断 — 是否真的是 L3 miss 问题？              │
  │                                                               │
  │  perf stat -e cache-references,cache-misses,instructions \   │
  │    -I 1000 -- ./program                                      │
  │                                                               │
  │  计算: miss 率 = cache-misses / cache-references              │
  │  计算: MPKI = cache-misses × 1000 / instructions              │
  │                                                               │
  │  miss 率 > 20% 或 MPKI > 10 → 确认是 L3 miss 问题           │
  └──────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Step 2: 排除抢占因素                                        │
  │                                                               │
  │  vmstat 1     → 看 cs (上下文切换/s)                         │
  │  mpstat -P ALL 1  → 看各 CPU 利用率和 %soft/%sys             │
  │                                                               │
  │  cs > 10K/s → 需要同时分析抢占影响                           │
  │  cs < 5K/s  → 基本排除抢占，继续 Step 3                      │
  └──────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Step 3: 区分 LLC-load-miss vs LLC-store-miss               │
  │                                                               │
  │  perf stat -e LLC-loads,LLC-load-misses, \                   │
  │    LLC-stores,LLC-store-misses -- ./program                  │
  │                                                               │
  │  load-miss 为主 → 读密集型瓶颈（数据预取不够）               │
  │  store-miss 为主 → 写密集型瓶颈（写缓冲区满）                │
  │  两者都高 → 工作集过大 + 可能的缓存污染                      │
  └──────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Step 4: 定位热点函数和热点地址                              │
  │                                                               │
  │  perf mem record -- ./program                                │
  │  perf mem report --sort=symbol,lat,mem                       │
  │                                                               │
  │  找到 miss 最多的函数 + 延迟最高的内存访问模式               │
  └──────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Step 5: 检查多核竞争                                        │
  │                                                               │
  │  perf c2c record -- ./program                                │
  │  perf c2c report                                              │
  │                                                               │
  │  Hitm% > 20% → 伪共享/缓存行竞争                           │
  │  RMT Hitm > 5% → 跨 NUMA 伪共享                             │
  └──────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Step 6: 对照实验验证                                        │
  │                                                               │
  │  A) taskset 绑定 vs 不绑定 → 排除迁移影响                    │
  │  B) 减少线程数 vs 全量 → 排除过度订阅                       │
  │  C) numactl --membind → 排除 NUMA 远端访问                  │
  │  D) resctrl L3 分区 → 排除缓存污染                           │
  └─────────────────────────────────────────────────────────────┘
```

### 7.2 典型案例分析

```
  案例: "8 线程程序 CPU 100% 但吞吐只有预期的 1/3"

  诊断过程:
  ┌────────────────────────────────────────────────────────┐
  │ Step 1: perf stat                                      │
  │   miss 率 = 45%, MPKI = 25 → 确认 L3 miss 严重        │
  │                                                          │
  │ Step 2: vmstat                                         │
  │   cs = 3000/s → 不高，排除抢占因素                     │
  │                                                          │
  │ Step 3: LLC 细分                                       │
  │   LLC-load-misses 远大于 LLC-store-misses              │
  │   → 读密集型瓶颈                                       │
  │                                                          │
  │ Step 4: perf mem report                                │
  │   60% 访问 L3 miss, 热点函数: hash_table_lookup()     │
  │   → 哈希表的随机访问模式导致 cache miss                │
  │                                                          │
  │ Step 5: perf c2c report                                │
  │   Hitm% = 35% → 多个线程竞争同一哈希桶                │
  │                                                          │
  │ 根因: 哈希表过大(64MB) + 随机访问 + 多线程竞争         │
  │ 修复: 分片哈希表(per-thread) + 减小工作集              │
  └────────────────────────────────────────────────────────┘
```

---

## 总结

```
  ★ L3 Cache Miss 诊断速查:

  ┌──────────────────────────────────────────────────────────┐
  │                                                            │
  │  测量:                                                     │
  │  perf stat -e cache-references,cache-misses               │
  │  perf stat -e LLC-loads,LLC-load-misses                   │
  │                                                            │
  │  定位:                                                     │
  │  perf mem record/report → 热点函数 + 延迟分布            │
  │  perf c2c record/report → 多核竞争 + 伪共享              │
  │                                                            │
  │  区分因果:                                                 │
  │  vmstat cs → 高 = 抢占导致 miss（链 B）                   │
  │  taskset 绑定实验 → miss 下降 = 迁移导致（链 B）          │
  │  miss 率不变 = 数据模式问题（链 A）                       │
  │                                                            │
  └──────────────────────────────────────────────────────────┘
```
