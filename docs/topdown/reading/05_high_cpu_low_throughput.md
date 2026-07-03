<!--
======================================================================
  Step 5: 实战 — CPU 高占用低吞吐的 Top-Down 诊断
======================================================================
  来源: Brendan Gregg《Systems Performance》、Intel/AMD 优化指南、
        Linux perf 文档
  阅读要点:
    - 端到端的诊断流程（5 步法）
    - 每一步的命令、指标、判断标准
    - AMD 和 Intel 双平台适配
    - 三种典型案例分析
  预计时间: 25-35 分钟
======================================================================
-->

# 实战：CPU 高占用低吞吐的 Top-Down 诊断

> "CPU 利用率 100%，但吞吐只有预期的 1/3" — 这是性能工程师最常
> 遇到的问题之一。Top-Down 方法提供了系统化的诊断路径。

---

## 目录

- [1. 5 步诊断法概览](#1--5-步诊断法概览)
- [2. Step 1: perf stat 看 IPC](#2-step-1perf-stat-看-ipc)
- [3. Step 2: Frontend Bound vs Backend Bound](#3-step-2frontend-bound-vs-backend-bound)
- [4. Step 3: Memory Bound vs Core Bound](#4-step-3memory-bound-vs-core-bound)
- [5. Step 4: L1/L2/L3 miss 率](#5-step-4l1l2l3-miss-率)
- [6. Step 5: NUMA 跨节点访问比例](#6-step-5numa-跨节点访问比例)
- [7. 完整诊断流程图](#7--完整诊断流程图)
- [8. 三种典型案例分析](#8-三种典型案例分析)
- [9. AMD 适配](#9-amd-适配)

---

## 1. 5 步诊断法概览

```
  +------------------------------------------------------------+
  |                                                              |
  |  Step 1: perf stat -> IPC                                    |
  |  +-- IPC < 1.0 -> 有大量 stall，继续                         |
  |  +-- IPC > 2.0 -> CPU 效率高，问题可能在 I/O 或锁等待       |
  |                                                              |
  |  Step 2: Frontend Bound vs Backend Bound                    |
  |  +-- Frontend > 30% -> 前端瓶颈（取指/解码）                |
  |  +-- Backend > 50% -> 后端瓶颈（内存/执行单元），继续       |
  |                                                              |
  |  Step 3: Memory Bound vs Core Bound                         |
  |  +-- Memory > 70% of Backend -> 内存瓶颈，继续              |
  |  +-- Core > 70% of Backend -> 执行单元/依赖链瓶颈           |
  |                                                              |
  |  Step 4: L1/L2/L3 miss 率                                   |
  |  +-- L3 miss > 30% -> DRAM 访问是瓶颈                      |
  |  +-- L1 miss 高但 L3 miss 低 -> L1 压力但 L3 兜底          |
  |                                                              |
  |  Step 5: NUMA 跨节点访问                                    |
  |  +-- 远端访问 > 20% -> NUMA 本地性问题                      |
  |                                                              |
  +------------------------------------------------------------+
```

---

## 2. Step 1: perf stat 看 IPC

### 2.1 命令

```bash
# 基本 IPC 测量
perf stat -e cycles,instructions -- ./program

# 带时间序列（观察 IPC 是否稳定）
perf stat -e cycles,instructions -I 1000 -- ./program

# 附加到已运行进程
perf stat -e cycles,instructions -p <PID> sleep 10
```

### 2.2 IPC 判断标准

| IPC 范围 | 评价 | 含义 | 下一步 |
|---------|------|------|--------|
| > 3.0 | 极好 | CPU 高度利用，接近理论极限 | 问题可能不在 CPU |
| 2.0-3.0 | 良好 | 正常的高效执行 | 检查是否有 I/O 或锁等待 |
| 1.0-2.0 | 一般 | 有一定瓶颈 | 继续 Step 2 |
| 0.5-1.0 | 差 | 严重 stall | 继续 Step 2（高优先级） |
| < 0.5 | 极差 | CPU 几乎空转 | 继续 Step 2（紧急） |

### 2.3 IPC 与 Retiring 的关系

```
  IPC = Retiring% x Pipeline_Width

  对于 4-wide 流水线 (Intel Skylake+ / AMD Zen 3+):
    IPC 2.0 = Retiring 50%  = 50% 的 Slot 被有效利用
    IPC 1.0 = Retiring 25%  = 75% 的 Slot 被浪费
    IPC 0.5 = Retiring 12.5% = 87.5% 的 Slot 被浪费

  IPC < 1.0 意味着 > 75% 的 CPU 周期被浪费
  这就是"CPU 高占用低吞吐"的本质
```

---

## 3. Step 2: Frontend Bound vs Backend Bound

### 3.1 Intel 平台命令

```bash
# Level 1 Top-Down 分析
perf stat --topdown --td-level 1 -- ./program

# 或者使用 metric group
perf stat -M TopdownL1 -- ./program
```

### 3.2 AMD 平台替代命令

```bash
# 使用 stalled-cycles 事件
perf stat -e \
  cycles,instructions,\
  stalled-cycles-frontend,\
  stalled-cycles-backend \
  -- ./program
```

### 3.3 判断标准

| 指标 | 正常 | 警告 | 严重 | 含义 |
|------|------|------|------|------|
| Frontend Bound | < 15% | 15-30% | > 30% | 前端无法供给足够的 uop |
| Backend Bound | < 30% | 30-50% | > 50% | 后端无法及时消化 uop |
| Bad Speculation | < 10% | 10-25% | > 25% | 分支预测错误浪费 |
| Retiring | > 50% | 25-50% | < 25% | 有效工作的比例 |

### 3.4 Frontend Bound 的常见原因

```
  Frontend Bound 高 -> 检查:

  1. I-Cache miss (代码段过大)
     perf stat -e L1-icache-load-misses -- ./program

  2. I-TLB miss (代码页过多)
     perf stat -e iTLB-load-misses -- ./program

  3. 分支重定向延迟
     perf stat -e branch-misses -- ./program

  优化方向:
  - 减少代码大小（-Os 优化、LTO）
  - 使用大页映射代码段（-z max-page-size=2097152）
  - PGO（Profile-Guided Optimization）改善代码布局
```

---

## 4. Step 3: Memory Bound vs Core Bound

### 4.1 Intel 平台

```bash
# Level 2 Top-Down 分析（区分 Memory Bound 和 Core Bound）
perf stat --topdown --td-level 2 -- ./program

# 关注:
# Backend Bound -> Memory Bound 占比
# Backend Bound -> Core Bound 占比
```

### 4.2 AMD 平台替代

```bash
# 用缓存 miss 率和 stalled-cycles 推断
perf stat -e \
  stalled-cycles-backend,\
  LLC-loads,LLC-load-misses,\
  branch-misses \
  -- ./program

# 如果 Backend Bound 高 且 L3 miss 率高:
#   -> Memory Bound
# 如果 Backend Bound 高 但 L3 miss 率低:
#   -> Core Bound（执行单元瓶颈或长依赖链）
```

### 4.3 判断标准

| 瓶颈类型 | 特征 | 常见原因 |
|---------|------|---------|
| Memory Bound | L3 miss 率高、DRAM 带宽接近饱和 | 大数据集、随机访问、cache 污染 |
| Core Bound | L3 miss 率低、IPC 低、无大量 cache miss | 除法运算、长依赖链、端口竞争 |

---

## 5. Step 4: L1/L2/L3 miss 率

### 5.1 命令

```bash
# 全层次缓存 miss 分析
perf stat -e \
  L1-dcache-loads,L1-dcache-load-misses,\
  l2_rqst:all,l2_rqst:miss,\
  LLC-loads,LLC-load-misses \
  -- ./program

# AMD 兼容版本
perf stat -e \
  L1-dcache-loads,L1-dcache-load-misses,\
  LLC-loads,LLC-load-misses,\
  dTLB-loads,dTLB-load-misses \
  -- ./program
```

### 5.2 各级 miss 率的意义

```
  缓存 Miss 漏斗:

  L1 Loads:  1,000,000,000
  L1 Misses:   100,000,000  (10% miss)  -> 进入 L2
  L2 Misses:    30,000,000  (30% of L2)  -> 进入 L3
  L3 Misses:    10,000,000  (33% of L3)  -> 进入 DRAM

  每次 DRAM 访问代价: ~200 周期 (~80ns)
  10M 次 DRAM 访问 = 2,000,000,000 周期 = ~0.8 秒 @ 2.5GHz
```

| 层级 | 健康 miss 率 | 问题 miss 率 | 优化方向 |
|------|-------------|-------------|---------|
| L1 | < 5% | > 10% | 改善数据局部性、减小工作集 |
| L2 | < 15% | > 25% | 预取优化、数据布局调整 |
| L3 | < 20% | > 30% | 减小总工作集、NUMA 绑定 |
| TLB | < 1% | > 5% | 使用大页（2MB/1GB） |

### 5.3 Intel Level 3 Top-Down

```bash
# Intel 平台可以精确到每级缓存
perf stat --topdown --td-level 3 -- ./program

# 输出示例:
# L1 Bound:     8.5%   <- L1 缓存压力
# L2 Bound:     5.2%   <- L2 缓存压力
# L3 Bound:    15.3%   <- L3 缓存压力 (主要瓶颈)
# DRAM Bound:  22.1%   <- DRAM 访问压力 (主要瓶颈)
# Store Bound:  3.1%   <- 存储缓冲区压力
```

---

## 6. Step 5: NUMA 跨节点访问比例

### 6.1 检查 NUMA 拓扑

```bash
# 查看 NUMA 拓扑
numactl --hardware

# 查看各 NUMA 节点的内存分配
numastat -m

# 查看进程的 NUMA 内存分布
numastat -p <PID>
```

### 6.2 测量跨 NUMA 访问

```bash
# perf mem 分析（可以看到 Remote vs Local 访问）
perf mem record -- ./program
perf mem report --sort=mem

# 关注:
# "Remote RAM" 和 "Remote L3" 的比例
# 如果 Remote > 20% -> NUMA 本地性问题

# 或者用 numastat 观察趋势
numastat -p <PID> -c 1  # 每秒刷新
# 关注 other_node 列的增长速度
```

### 6.3 NUMA 优化

```bash
# 绑定到特定 NUMA 节点
numactl --cpunodebind=0 --membind=0 -- ./program

# 对比绑定前后的性能差异
# 如果绑定后性能提升 > 20%:
#   -> NUMA 本地性是重要因素

# 自动 NUMA balancing（内核默认开启）
cat /proc/sys/kernel/numa_balancing
# 1 = 开启（内核自动迁移页面到访问它的 CPU 所在节点）
```

---

## 7. 完整诊断流程图

```
  CPU 高占用低吞吐
         |
         v
  +-- Step 1: perf stat IPC --------------------------------+
  |                                                          |
  |  IPC < 1.0?                                             |
  |  +-- 否 -> CPU 效率尚可，检查 I/O 等待或锁竞争          |
  |  +-- 是 -> 确认有大量 stall，继续                        |
  +--------------------+-------------------------------------+
                       |
                       v
  +-- Step 2: Frontend vs Backend --------------------------+
  |                                                          |
  |  Intel: perf stat --topdown --td-level 1                |
  |  AMD:   perf stat -e stalled-cycles-frontend/backend    |
  |                                                          |
  |  主要瓶颈方向:                                          |
  |  +-- Frontend Bound > 30%                               |
  |  |   -> I-Cache miss? I-TLB miss? 分支重定向?          |
  |  |                                                      |
  |  +-- Backend Bound > 50%                                |
  |  |   -> 继续 Step 3                                     |
  |  |                                                      |
  |  +-- Bad Speculation > 25%                              |
  |      -> 分支预测问题 -> 排序数据 / PGO / CMOV          |
  +--------------------+-------------------------------------+
                       |
                       v
  +-- Step 3: Memory Bound vs Core Bound -------------------+
  |                                                          |
  |  Intel: perf stat --topdown --td-level 2                |
  |  AMD:   结合 stalled-cycles-backend + L3 miss 率       |
  |                                                          |
  |  +-- Memory Bound > Core Bound                          |
  |  |   -> 继续 Step 4（定位哪级缓存）                    |
  |  |                                                      |
  |  +-- Core Bound > Memory Bound                          |
  |      -> 检查除法运算、长依赖链、执行端口竞争            |
  |      -> perf record -e cycles:pp -g 找热点函数          |
  +--------------------+-------------------------------------+
                       |
                       v
  +-- Step 4: L1/L2/L3 miss 率 ----------------------------+
  |                                                          |
  |  perf stat -e L1-dcache-load-misses,LLC-load-misses    |
  |                                                          |
  |  +-- L3 miss > 30% -> DRAM 是瓶颈                      |
  |  |   -> 减小工作集 / 改善局部性 / 预取                 |
  |  |   -> 继续 Step 5（检查 NUMA）                       |
  |  |                                                      |
  |  +-- L1 miss 高但 L3 miss 低                            |
  |  |   -> L1 压力大但 L3 兜底                            |
  |  |   -> 改善数据布局、使用缓存友好的数据结构            |
  |  |                                                      |
  |  +-- TLB miss 高                                        |
  |      -> 使用大页 (2MB/1GB)                              |
  +--------------------+-------------------------------------+
                       |
                       v
  +-- Step 5: NUMA 跨节点访问 ------------------------------+
  |                                                          |
  |  perf mem report --sort=mem                             |
  |  numactl --hardware                                      |
  |                                                          |
  |  +-- Remote RAM > 20%                                   |
  |  |   -> NUMA 本地性问题                                 |
  |  |   -> numactl --cpunodebind=N --membind=N             |
  |  |   -> 检查 NPS 配置（AMD）                            |
  |  |                                                      |
  |  +-- Remote RAM < 5%                                    |
  |      -> NUMA 不是问题                                    |
  |      -> 回到 Step 4 重新审视                            |
  +----------------------------------------------------------+
```

---

## 8. 三种典型案例分析

### 8.1 案例 A：大内存扫描型负载

```
  现象: CPU 100%, 吞吐 500MB/s（预期 2GB/s）

  Step 1: perf stat
    IPC = 0.3  -> 严重 stall

  Step 2: Top-Down Level 1
    Retiring:       8%
    Frontend Bound: 2%
    Bad Speculation: 1%
    Backend Bound:  89%   -> 后端严重瓶颈

  Step 3: Level 2
    Memory Bound: 82% (占 Backend 的 92%)  -> 内存瓶颈
    Core Bound:    7%

  Step 4: 缓存 miss
    L1 miss rate: 15%
    L3 miss rate: 55%  -> 超过一半的 L3 访问 miss
    MPKI: 35           -> 极高

  Step 5: NUMA
    Remote RAM: 3%  -> NUMA 不是问题

  根因: 数据集 200GB >> L3 32MB
        顺序扫描但工作集太大 -> 持续的 DRAM 访问
  优化: 流式预取、压缩数据、增加内存带宽
```

### 8.2 案例 B：随机访问型负载（哈希表/链表）

```
  现象: CPU 100%, QPS 只有预期的 1/5

  Step 1: perf stat
    IPC = 0.15  -> 极差

  Step 2: Top-Down Level 1
    Retiring:       4%
    Backend Bound:  93%

  Step 3: Level 2
    Memory Bound: 88%  -> 内存瓶颈
    Core Bound:    5%

  Step 4: 缓存 miss
    L3 miss rate: 70%  -> 大量 DRAM 访问
    MPKI: 60           -> 极高

    perf mem report:
    80% 的访问是 "L3 miss" (DRAM)

  Step 5: NUMA
    Remote RAM: 30%  -> NUMA 问题也严重

  根因: 哈希表 64GB，随机访问 -> 每次查找都 miss
        + 数据分布在多个 NUMA 节点
  优化:
    1. numactl 绑定到本地 NUMA 节点
    2. 分片哈希表 -> 每线程本地分片
    3. 使用 cache-oblivious 数据结构（B-tree 替代哈希表）
```

### 8.3 案例 C：锁竞争导致的 CPU 高占用

```
  现象: CPU 100%, 请求延迟波动大

  Step 1: perf stat
    IPC = 0.6  -> 较差

  Step 2: Top-Down Level 1
    Retiring:       15%
    Backend Bound:  55%
    Frontend Bound: 5%
    Bad Speculation: 25%  -> 异常高

  Step 3: perf record
    热点函数:
    25% _raw_spin_lock_irqsave  -> 自旋锁竞争!
    15% queued_spin_lock_slowpath

  补充分析:
    vmstat: cs = 25000/s  -> 高上下文切换
    L3 miss rate: 20%  -> 中等（切换导致的 cache 污染）

  根因: 全局自旋锁保护共享数据结构
        多线程竞争 -> 自旋等待浪费 CPU
        每次获取锁后的 cache 无效化 -> 额外的 cache miss
  优化:
    1. 拆分全局锁为 per-CPU 锁
    2. 使用 RCU 替代读侧锁
    3. 减少临界区长度
```

---

## 9. AMD 适配

### 9.1 AMD 上的完整诊断命令

```bash
# Step 1: IPC
perf stat -e cycles,instructions -- ./program

# Step 2: Frontend vs Backend (替代 topdown)
perf stat -e \
  cycles,instructions,\
  stalled-cycles-frontend,\
  stalled-cycles-backend \
  -- ./program

# Step 3: Memory vs Core (通过 cache miss 推断)
perf stat -e \
  cycles,instructions,\
  LLC-loads,LLC-load-misses,\
  branch-misses,\
  L1-dcache-loads,L1-dcache-load-misses \
  -- ./program

# Step 4: 详细缓存分析
perf stat -e \
  L1-dcache-loads,L1-dcache-load-misses,\
  LLC-loads,LLC-load-misses,\
  dTLB-loads,dTLB-load-misses,\
  iTLB-load-misses \
  -- ./program

# Step 5: NUMA 分析
perf mem record -- ./program
perf mem report --sort=mem

# 补充: IBS 精确分析
perf record -e ibs_op// -g -- ./program
perf report
```

### 9.2 AMD 诊断速查表

| 诊断目标 | Intel 命令 | AMD 替代命令 |
|---------|-----------|-------------|
| IPC | `perf stat -e cycles,instructions` | 相同 |
| Level 1 四大类 | `perf stat --topdown --td-level 1` | `perf stat -e stalled-cycles-*` |
| Level 2 细分 | `perf stat --topdown --td-level 2` | `perf stat -e L1-*,LLC-*` |
| Level 3+ 细分 | `perf stat --topdown --td-level 3` | IBS (`perf record -e ibs_op//`) |
| 热点函数 | `perf record -g` | 相同 |
| 缓存竞争 | `perf c2c` | 相同 |
| 内存访问 | `perf mem` | 相同（IBS 增强） |
| 锁竞争 | `perf lock` | 相同 |

---

## 总结

```
  CPU 高占用低吞吐的 Top-Down 诊断:

  Step 1: IPC < 1.0 -> 确认有严重 stall
  Step 2: Backend Bound > 50% -> 后端是主要瓶颈
  Step 3: Memory Bound -> 内存子系统问题
  Step 4: L3 miss > 30% -> DRAM 访问是根因
  Step 5: Remote RAM > 20% -> NUMA 加剧问题

  每个 Step 都有明确的:
  - 命令（Intel + AMD 双平台）
  - 判断标准（阈值）
  - 优化方向（具体可操作）

  AMD 特别提示:
  - 没有 --topdown，用 stalled-cycles + cache miss 推断
  - IBS 是 AMD 独有的强大工具
  - 注意 CCD 拓扑（8 核共享 32MB L3）
  - 注意 NPS 配置（NUMA 节点数）
```
