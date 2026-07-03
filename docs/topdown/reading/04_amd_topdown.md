<!--
======================================================================
  Step 4: AMD 上的 Top-Down 分析
======================================================================
  来源: AMD PPR、AMD μProf 文档、Linux perf 源码、Brendan Gregg
  阅读要点:
    - AMD Zen 3/4 没有 Intel 的 Top-Down PMU 事件
    - AMD IBS（Instruction Based Sampling）的使用方法
    - perf stat 的替代方案
    - 用 cache/branch 指标推断瓶颈类型
  预计时间: 20-30 分钟
======================================================================
-->

# AMD 上的 Top-Down 分析

> Intel TMAM 依赖 Intel 特定的 PMU 事件（如 IDQ_UOPS_NOT_DELIVERED、
> UOPS_ISSUED.ANY 等）。AMD 的 PMU 事件集不同，没有 `--topdown`
> 原生支持。但 AMD 有自己的分析工具和方法可以达到类似效果。

---

## 目录

- [1. 为什么 AMD 没有 --topdown](#1-为什么-amd-没有---topdown)
- [2. AMD 的替代方案总览](#2-amd-的替代方案总览)
- [3. ★ 方法 1: perf stat -d 推断瓶颈](#3--方法-1perf-stat--d-推断瓶颈)
- [4. ★ 方法 2: stalled-cycles 事件](#4--方法-2stalled-cycles-事件)
- [5. ★ 方法 3: AMD IBS 精确采样](#5--方法-3amd-ibs-精确采样)
- [6. ★ 方法 4: AMD μProf](#6--方法-4amd-μprof)
- [7. 手动构造 Top-Down-like 分析](#7-手动构造-top-down-like-分析)
- [8. AMD vs Intel 指标对照表](#8-amd-vs-intel-指标对照表)

---

## 1. 为什么 AMD 没有 --topdown

### 1.1 Intel TMAM 的硬件依赖

```
  Intel Top-Down 需要的关键 PMU 事件:

  ┌─────────────────────────────────────────────────────────────┐
  │  事件名                       │ 用途                          │
  ├───────────────────────────────┼─────────────────────────────┤
  │  IDQ_UOPS_NOT_DELIVERED.CORE  │ 前端未供给的 Slot 数         │
  │                               │ → 计算 Frontend Bound        │
  │  UOPS_ISSUED.ANY              │ 发射到后端的 μop 数          │
  │                               │ → 计算 Backend Bound         │
  │  UOPS_RETIRED.RETIRE_SLOTS    │ 成功退休的 Slot 数           │
  │                               │ → 计算 Retiring              │
  │  INT_MISC.RECOVERY_CYCLES     │ 分支预测错误恢复周期         │
  │                               │ → 计算 Bad Speculation       │
  │  CPU_CLK_UNHALTED.THREAD      │ 总周期数                      │
  └─────────────────────────────────────────────────────────────┘

  ★ 这些事件名和编码是 Intel 特有的
    AMD 的 PMU 有不同的事件定义和编码
    → perf stat --topdown 在 AMD 上直接报 "not supported"
```

### 1.2 AMD PMU 的特点

```
  AMD Core PMC (Performance Monitor Counters):

  ┌────────────────────────────────────────────────────────────┐
  │  AMD Zen 3/4 PMU:                                           │
  │                                                              │
  │  通用计数器: 4-6 个（取决于微架构）                         │
  │  ├── 可配置为任何 AMD Core PMC 事件                         │
  │  ├── 事件通过 EventSelect + UnitMask 配置                   │
  │  └── 事件定义参见 AMD PPR (Processor Programming Reference) │
  │                                                              │
  │  固定计数器: 无（Intel 有 3 个固定计数器）                  │
  │  → AMD 的 cycles/instructions 也用通用计数器               │
  │                                                              │
  │  ★ IBS (Instruction Based Sampling):                        │
  │  ├── AMD 独有的精确采样机制                                  │
  │  ├── 提供比 Intel PEBS 更丰富的每条指令信息                  │
  │  └── 是 AMD 性能分析的核心工具                               │
  └────────────────────────────────────────────────────────────┘
```

---

## 2. AMD 的替代方案总览

```
  AMD 上的瓶颈分析方法:

  ┌────────────────────────────────────────────────────────────┐
  │  方法 1: perf stat -d                                      │
  │  ├── 使用通用事件（cache/branch/instructions/cycles）      │
  │  ├── 推断瓶颈类型（非精确 topdown）                        │
  │  └── ★ 最简单，推荐首先使用                                │
  ├────────────────────────────────────────────────────────────┤
  │  方法 2: stalled-cycles-frontend/backend                   │
  │  ├── 直接测量前端/后端停顿周期                              │
  │  ├── 类似 topdown Level 1 的 Frontend/Backend Bound       │
  │  └── ★ AMD Zen 3+ 支持                                    │
  ├────────────────────────────────────────────────────────────┤
  │  方法 3: AMD IBS (Instruction Based Sampling)              │
  │  ├── 精确采样每条指令的延迟来源                             │
  │  ├── 可以替代 topdown Level 2-4 的细分                     │
  │  └── ★ 最强大，但分析更复杂                                │
  ├────────────────────────────────────────────────────────────┤
  │  方法 4: AMD μProf                                         │
  │  ├── AMD 官方性能分析工具                                   │
  │  ├── 类似 Intel VTune，提供图形化 topdown                  │
  │  └── ★ 最接近 Intel TMAM 体验                              │
  └────────────────────────────────────────────────────────────┘
```

---

## 3. ★ 方法 1：perf stat -d 推断瓶颈

### 3.1 使用方法

```bash
# perf stat -d 是"默认指标组"，包含基本的性能指标
perf stat -d -- ./program

# 等效于手动指定这些事件:
perf stat -e \
  cycles,instructions,\
  cache-references,cache-misses,\
  branch-instructions,branch-misses,\
  bus-cycles \
  -- ./program
```

### 3.2 输出解读

```bash
# perf stat -d 典型输出:
#
#  Performance counter stats for './program':
#
#        2,345,678,901      cycles                    #  2.345 GHz
#        3,456,789,012      instructions              #  1.47 IPC
#          123,456,789      cache-references          #  123.456 M/sec
#           45,678,901      cache-misses              #  37.0% of all cache refs  ★
#          567,890,123      branch-instructions       #  567.890 M/sec
#           12,345,678      branch-misses             #  2.2% of all branches
#
#        1.000123456 seconds time elapsed

# ★ 关键指标:
# IPC = instructions / cycles = 3.46G / 2.35G = 1.47
# cache-misses% = 45.7M / 123.5M = 37.0% → L3 miss 率
# branch-misses% = 12.3M / 567.9M = 2.2% → 分支预测准确率
```

### 3.3 ★ 从 perf stat -d 推断瓶颈类型

```
  决策树（从通用指标推断瓶颈类型）:

  IPC < 1.0 ?
  ├── 是 → 有严重瓶颈，继续分析
  │   │
  │   ├── cache-misses% > 30% ?
  │   │   └── 是 → ★ Memory Bound (类似 Intel Backend → Memory Bound)
  │   │
  │   ├── branch-misses% > 5% ?
  │   │   └── 是 → ★ Bad Speculation (分支预测问题)
  │   │
  │   ├── cache-misses% < 10% && branch-misses% < 2% ?
  │   │   └── 是 → ★ Core Bound (执行单元瓶颈或依赖链)
  │   │
  │   └── 都不是特别高?
  │       └── → 混合瓶颈，需要更细的分析
  │
  └── 否 → IPC 尚可，检查是否有前端问题
      │
      └── 用 stalled-cycles-frontend 进一步确认
```

### 3.4 完整的缓存/分支分析

```bash
# 更详细的分析（AMD 兼容）
perf stat -e \
  cycles,instructions,\
  L1-dcache-loads,L1-dcache-load-misses,\
  LLC-loads,LLC-load-misses,\
  dTLB-loads,dTLB-load-misses,\
  branch-instructions,branch-misses \
  -- ./program

# 从各级 miss 率定位瓶颈层级:
# L1 miss 率 = L1-dcache-load-misses / L1-dcache-loads
# L3 miss 率 = LLC-load-misses / LLC-loads
# TLB miss 率 = dTLB-load-misses / dTLB-loads
```

| 瓶颈类型 | 特征指标 | 阈值 |
|---------|---------|------|
| L1 Bound | L1 miss 率高，L3 miss 率低 | L1 miss > 10%, L3 miss < 10% |
| L3 Bound | L3 miss 率高 | L3 miss > 20% |
| DRAM Bound | L3 miss 率极高 + IPC 极低 | L3 miss > 40%, IPC < 0.5 |
| TLB Bound | dTLB miss 率高 | dTLB miss > 5% |
| Branch Mispredict | branch-misses 高 | branch-miss > 5% |

---

## 4. ★ 方法 2：stalled-cycles 事件

### 4.1 使用方法

```bash
# stalled-cycles-frontend: 前端停顿周期（无法供给 μop）
# stalled-cycles-backend:  后端停顿周期（无法消化 μop）

perf stat -e \
  cycles,instructions,\
  stalled-cycles-frontend,\
  stalled-cycles-backend \
  -- ./program
```

### 4.2 输出解读

```bash
# 输出示例:
#
#        2,000,000,000      cycles
#        1,500,000,000      instructions              #  0.75 IPC
#          400,000,000      stalled-cycles-frontend    #  20.0% of cycles  ★
#        1,200,000,000      stalled-cycles-backend     #  60.0% of cycles  ★★
#
# ★ 解读:
# 60% 的周期后端在停顿 → Backend Bound 严重
# 20% 的周期前端在停顿 → Frontend Bound 中等
# 剩余 20% 在做有效工作 → Retiring ≈ 20%
#
# 这与 Intel Top-Down Level 1 的四大类对应:
# Retiring ≈ 1 - (frontend_stall + backend_stall) / cycles
# Frontend Bound ≈ stalled-cycles-frontend / cycles
# Backend Bound ≈ stalled-cycles-backend / cycles
```

### 4.3 ★ 与 Intel Top-Down 的对应关系

```
  Intel Top-Down Level 1:         AMD stalled-cycles 近似:

  ┌──────────────────────┐       ┌──────────────────────────────┐
  │ Retiring             │       │ 1 - (frontend + backend)     │
  │ Frontend Bound       │  ←→   │ stalled-cycles-frontend      │
  │ Bad Speculation      │       │   / cycles                   │
  │ Backend Bound        │  ←→   │ stalled-cycles-backend       │
  │                      │       │   / cycles                   │
  └──────────────────────┘       └──────────────────────────────┘

  ★ 注意:
  - stalled-cycles 不能区分 Bad Speculation
    (Intel TMAM 用专门的冲刷事件来区分)
  - stalled-cycles-frontend 和 backend 可能有重叠
    (同时前后端都在停顿)
  - 但作为 Level 1 的近似已经足够指导优化方向
```

### 4.4 AMD 上的 stalled-cycles 支持情况

| AMD 微架构 | stalled-cycles-frontend | stalled-cycles-backend | 说明 |
|-----------|------------------------|----------------------|------|
| Zen 2 (Rome) | ★ 支持 | ★ 支持 | 通过 PMC 事件映射 |
| Zen 3 (Milan) | ★ 支持 | ★ 支持 | 同上 |
| Zen 4 (Genoa) | ★ 支持 | ★ 支持 | 同上 |
| Zen 5 (Turin) | ★ 支持 | ★ 支持 | 同上 |

```bash
# 验证 AMD 上是否支持
perf list | grep stalled
# 应看到:
#   stalled-cycles-frontend [Hardware event]
#   stalled-cycles-backend  [Hardware event]
```

---

## 5. ★ 方法 3：AMD IBS 精确采样

### 5.1 IBS 是什么

```
  IBS (Instruction Based Sampling) — AMD 独有的精确采样

  与 Intel PEBS 的对比:

  ┌─────────────────────────────────────────────────────────────┐
  │  维度          │ Intel PEBS         │ AMD IBS               │
  ├───────────────┼────────────────────┼───────────────────────┤
  │  采样方式      │ 基于事件计数       │ ★ 基于指令完成        │
  │  采样精度      │ 精确（无 skid）    │ 精确（无 skid）       │
  │  提供的信息    │ 基本地址/寄存器    │ ★ 每条指令的完整元数据│
  │  DC miss 延迟  │ 部分支持           │ ★ 完整的 DC miss 延迟 │
  │  TLB 信息      │ 部分               │ ★ L1/L2 TLB 命中/缺失│
  │  分支结果      │ 支持               │ ★ 支持 + 误预测标记   │
  │  数据物理地址  │ 部分支持           │ ★ 支持                │
  │  取指信息      │ 不支持             │ ★ IBS Fetch 独立模式  │
  └─────────────────────────────────────────────────────────────┘
```

### 5.2 IBS 两种模式

```
  IBS Fetch（取指采样）:
  ┌─────────────────────────────────────────┐
  │  采样每条取指操作:                        │
  │  - 取指地址 (虚拟/物理)                   │
  │  - I-Cache 命中/缺失                     │
  │  - I-TLB 命中/缺失                       │
  │  - 取指延迟（周期数）                     │
  │  - 取指来源（L1/L2/L3/DRAM）            │
  │                                           │
  │  ★ 用途: 识别前端瓶颈                     │
  │  I-Cache miss 高 → 代码段过大             │
  │  I-TLB miss 高 → 需要代码大页             │
  └─────────────────────────────────────────┘

  IBS Op（操作采样）:
  ┌─────────────────────────────────────────┐
  │  采样每条执行操作:                        │
  │  - 执行延迟（从派遣到完成）               │
  │  - D-Cache 命中/缺失                     │
  │  - D-TLB 命中/缺失                       │
  │  - ★ DC miss latency（缓存缺失的实际延迟）│
  │  - 数据访问地址（虚拟/物理）              │
  │  - 分支结果 + 是否误预测                  │
  │  - 是否为 Load/Store                      │
  │                                           │
  │  ★ 用途: 识别后端瓶颈                     │
  │  DC miss latency 高 → Memory Bound        │
  │  分支误预测多 → Bad Speculation           │
  └─────────────────────────────────────────┘
```

### 5.3 使用方法

```bash
# IBS Op 采样（后端分析）
perf record -e ibs_op// -- ./program

# IBS Fetch 采样（前端分析）
perf record -e ibs_fetch// -- ./program

# 带调用栈
perf record -e ibs_op// -g --call-graph dwarf -- ./program

# 查看 IBS 采样结果
perf report

# ★ IBS 特有的输出字段:
# - Data Cache Miss: 是/否
# - DC Miss Latency: N 周期（★ 直接看到每次 miss 的延迟！）
# - TLB Miss: 是/否
# - Branch Mispredict: 是/否
# - Data Address: 0x...（访问的内存地址）
```

### 5.4 IBS 用于 Top-Down-like 分析

```bash
# 用 IBS Op 推断瓶颈类型:

# Step 1: 采集 IBS Op 数据
perf record -e ibs_op// -c 100000 -- ./program

# Step 2: 分析 miss 延迟分布
perf report --sort=dcach_miss_lat --stdio

# Step 3: 分析瓶颈来源
# 如果大量样本的 DC Miss Latency > 100 周期:
#   → Memory Bound（DRAM 访问）
# 如果大量样本的 DC Miss Latency < 40 周期:
#   → L3 Bound 或 L2 Bound
# 如果大部分样本 DC Miss = No:
#   → Core Bound（计算延迟而非内存延迟）
```

---

## 6. ★ 方法 4：AMD μProf

### 6.1 简介

```
  AMD μProf (Microarchitecture Profiler):

  ★ AMD 官方出品的性能分析工具
  ★ 功能对标 Intel VTune Profiler
  ★ 提供图形化的 Top-Down 分析

  官网: https://www.amd.com/en/developer/uprof.html

  支持的分析类型:
  ├── μArch Counters  — 类似 perf stat（但更精细）
  ├── μArch Top-Down  — ★ AMD 版本的 topdown 分析
  ├── Memory Access   — 内存访问延迟和热点分析
  ├── Power Analysis  — 功耗分析
  └── System Analysis — 系统级分析（中断、上下文切换）
```

### 6.2 使用方法

```bash
# 安装（从 AMD 官网下载）
# 支持 Linux 和 Windows

# 命令行模式
AMDuProfCLI collect --config muarch_topdown -- ./program
AMDuProfCLI report --input-dir <output-dir>

# 图形模式
AMDuProf &
# → 在 GUI 中选择分析类型和目标程序
```

---

## 7. 手动构造 Top-Down-like 分析

### 7.1 ★ AMD 版 Top-Down Level 1 脚本

```bash
#!/bin/bash
# amd_topdown_l1.sh — 在 AMD 上模拟 Top-Down Level 1

PROGRAM="$@"

perf stat -e \
  cycles,instructions,\
  stalled-cycles-frontend,\
  stalled-cycles-backend,\
  cache-references,cache-misses,\
  branch-instructions,branch-misses \
  -- $PROGRAM 2>&1 | awk '
/cycles/              { cycles = $1 }
/instructions/        { ins = $1; ipc = ins/cycles }
/stalled.*frontend/   { scf = $1; fe_pct = scf/cycles*100 }
/stalled.*backend/    { scb = $1; be_pct = scb/cycles*100 }
/cache-misses/        { cm = $1 }
/cache-references/    { cr = $1; miss_rate = cm/cr*100 }
/branch-misses/       { bm = $1 }
/branch-instructions/ { bi = $1; bm_rate = bm/bi*100 }
END {
  retiring = 100 - fe_pct - be_pct
  if (retiring < 0) retiring = 0
  printf "\n★ AMD Top-Down Level 1 (近似)\n"
  printf "  IPC:              %.2f\n", ipc
  printf "  Retiring:         %.1f%%\n", retiring
  printf "  Frontend Bound:   %.1f%%\n", fe_pct
  printf "  Backend Bound:    %.1f%%\n", be_pct
  printf "  (Bad Speculation 无法区分，包含在 Frontend/Backend 中)\n"
  printf "\n  L3 Miss Rate:     %.1f%%\n", miss_rate
  printf "  Branch Miss Rate: %.1f%%\n", bm_rate
  printf "\n★ 诊断:\n"
  if (ipc < 1.0) printf "  ⚠ IPC < 1.0 — 严重瓶颈\n"
  if (be_pct > 50) printf "  ⚠ Backend Bound > 50%% — 后端是主要瓶颈\n"
  if (miss_rate > 30) printf "  ⚠ L3 Miss > 30%% — 内存瓶颈\n"
  if (bm_rate > 5) printf "  ⚠ Branch Miss > 5%% — 分支预测问题\n"
}'
```

### 7.2 AMD 版 Top-Down Level 2 分析

```bash
# 在 Level 1 确定瓶颈方向后，用 Level 2 深入:

# 如果 Backend Bound 高:
perf stat -e \
  cycles,instructions,\
  L1-dcache-loads,L1-dcache-load-misses,\
  l2_rqst:all,l2_rqst:miss_all_no_l2_miss,\
  LLC-loads,LLC-load-misses,\
  dTLB-loads,dTLB-load-misses \
  -- ./program

# 计算各级 miss 率:
# L1 Bound: L1-dcache-load-misses / L1-dcache-loads
# L2 Bound: L2 misses / L2 requests
# L3 Bound: LLC-load-misses / LLC-loads
# TLB Bound: dTLB-load-misses / dTLB-loads

# 如果 Frontend Bound 高:
perf stat -e \
  cycles,instructions,\
  L1-icache-load-misses,\
  iTLB-load-misses \
  -- ./program
```

---

## 8. AMD vs Intel 指标对照表

### 8.1 Top-Down 等效指标

| Intel TMAM 指标 | AMD 等效方法 | 说明 |
|-----------------|-------------|------|
| Retiring | `1 - (stalled_fe + stalled_be)/cycles` | 近似值 |
| Frontend Bound | `stalled-cycles-frontend / cycles` | 近似值 |
| Backend Bound | `stalled-cycles-backend / cycles` | 近似值 |
| Bad Speculation | 无法直接区分 | 可通过 IBS branch mispredict 推断 |
| Memory Bound | `LLC-load-misses / LLC-loads` | L3 miss 率 |
| Core Bound | Backend Bound - Memory Bound | 间接推算 |
| Branch Mispredict | `branch-misses / branch-instructions` | 直接对应 |
| L1 Bound | `L1-dcache-load-misses / L1-dcache-loads` | 直接对应 |
| L3 Bound | `LLC-load-misses / LLC-loads` | 直接对应 |
| DRAM Bound | IBS DC Miss Latency > 100 cycles | 需 IBS |

### 8.2 工具对照

| 功能 | Intel | AMD |
|------|-------|-----|
| Top-Down Level 1 | `perf stat --topdown --td-level 1` | `perf stat -e stalled-cycles-*` |
| Top-Down Level 2+ | `perf stat --topdown --td-level 2` | `perf stat -e L1-*,LLC-*` + IBS |
| 精确采样 | PEBS (`perf record -e mem-loads`) | ★ IBS (`perf record -e ibs_op//`) |
| 图形化分析 | VTune Profiler | AMD μProf |
| 缓存竞争分析 | `perf c2c` | `perf c2c`（通用） |
| 内存访问分析 | `perf mem` | `perf mem`（通用） |
| 锁竞争分析 | `perf lock` | `perf lock`（通用） |

---

## 总结

```
  ★ AMD Top-Down 分析速查:

  Level 1 (四大类):
  └── perf stat -e stalled-cycles-frontend,stalled-cycles-backend
      → 近似 Retiring / Frontend Bound / Backend Bound

  Level 2 (八大类):
  └── perf stat -e L1-*,LLC-*,branch-*
      → L1/L2/L3/TLB miss 率 + 分支 miss 率

  Level 3+ (细分):
  └── AMD IBS (ibs_op // ibs_fetch)
      → 每条指令的延迟来源、缓存命中/缺失、TLB 状态

  图形化:
  └── AMD μProf
      → 类似 VTune 的 topdown 树状图
```
