# Top-Down Microarchitecture Analysis — 问答集

> 持续追加的 Q&A，记录学习和实践中的关键问题

---

## 目录

- [Q1: 为什么 perf stat --topdown 在虚拟机上无法使用？](#q1-为什么-perf-stat---topdown-在虚拟机上无法使用)
- [Q2: TMAM 的 Pipeline Width（流水线宽度）为什么因 CPU 代际不同？](#q2-tmam-的-pipeline-width流水线宽度为什么因-cpu-代际不同)
- [Q3: AMD 没有 Top-Down PMU 事件怎么办？](#q3-amd-没有-top-down-pmu-事件怎么办)
- [Q4: CPU 高占用低吞吐如何用 Top-Down 方法诊断？](#q4-cpu-高占用低吞吐如何用-top-down-方法诊断)
- [Q5: Backend Bound 中 Memory Bound 和 Core Bound 如何区分？](#q5-backend-bound-中-memory-bound-和-core-bound-如何区分)

---

<!-- 格式说明：
## Q{N}: {问题标题}

**日期：** YYYY-MM-DD
**场景：** 在哪里遇到的问题
**相关文件：** 对应的 reading 文件
**源码位置：** 具体文件和行号

### 回答
（源码级解答 + 代码引用 + 表格/图示）
-->

---

## Q1: 为什么 perf stat --topdown 在虚拟机上无法使用？

**日期：** 2026-07-03
**场景：** 在 KVM 虚拟机上运行 perf stat --topdown 时报 PMU 事件不可用
**相关文件：** reading/02_command_reference.md

### 回答

**根因：** PMU（Performance Monitoring Unit）是 CPU 的硬件计数器，需要通过虚拟化透传才能使用。

| 虚拟化方式 | PMU 可用性 | 说明 |
|-----------|-----------|------|
| 物理机 | ★ 完全可用 | 直接访问 PMU 寄存器 |
| KVM + PMU passthrough | 部分可用 | 需要 `-cpu host` 且主机支持 |
| KVM 默认 | 不可用 | 虚拟 CPU 不模拟 PMU |
| VMware/VirtualBox | 不可用 | 不透传 PMU |

**验证方法：**
```bash
# 检查 PMU 是否可用
perf stat -e cycles,instructions -- sleep 1
# 如果显示 <not supported>，说明 PMU 不可用

# 检查 dmesg 中的 PMU 信息
dmesg | grep -i pmu
# 正常应看到：Performance Events: PEBS fmt3+, 32-deep x86_64 PMU
```

**解决方案：**
1. 使用物理机进行测试
2. KVM 启动时添加 `-cpu host` 参数透传 PMU
3. 使用 toplev.py 的 `--emulate` 模式（近似结果）

---

## Q2: TMAM 的 Pipeline Width（流水线宽度）为什么因 CPU 代际不同？

**日期：** 2026-07-03
**场景：** 不同 CPU 上 topdown 百分比计算基数不同
**相关文件：** reading/01_pipeline_slots.md

### 回答

Pipeline Width = 每周期最大退休 μop 数量。不同微架构的宽度不同：

| CPU 微架构 | Pipeline Width | 代号 |
|-----------|---------------|------|
| Sandy Bridge / Ivy Bridge | 4 | SNB/IVB |
| Haswell / Broadwell | 4 | HSW/BDW |
| Skylake / Kaby Lake / Coffee Lake | 4 | SKL/KBL/CFL |
| Ice Lake / Tiger Lake | 4 | ICL/TGL |
| Alder Lake (P-Core) | 4 | ADL-P |
| Alder Lake (E-Core) | 4 | ADL-E（但执行端口更少） |
| Sapphire Rapids / Emerald Rapids | 4 | SPR/EMR |

**★ 虽然宽度都是 4，但内部执行资源（端口数、ROB 大小、缓存层次）差异很大。** 这导致同一程序在不同 CPU 上的 topdown 分布不同。

**公式：**
```
Total Slots = Pipeline_Width × CPU_CLK_UNHALTED.THREAD
Retiring%   = INST_RETIRED.ANY / Total Slots
```

---

## Q3: AMD 没有 Top-Down PMU 事件怎么办？

**日期：** 2026-07-04
**场景：** 在 AMD EPYC 服务器上运行 `perf stat --topdown` 报 "not supported"，需要替代方案进行微架构瓶颈分析
**相关文件：** reading/04_amd_topdown.md

### 回答

**根因：** Intel TMAM 依赖 Intel 特有的 PMU 事件（如 `IDQ_UOPS_NOT_DELIVERED`、`UOPS_ISSUED.ANY` 等），AMD 的 PMU 事件集不同，没有这些事件。

**★ 四种替代方案：**

**方案 1: `perf stat -e stalled-cycles-*`（最推荐）**

```bash
perf stat -e \
  cycles,instructions,\
  stalled-cycles-frontend,\
  stalled-cycles-backend \
  -- ./program

# 输出近似 Top-Down Level 1:
# Frontend Bound ≈ stalled-cycles-frontend / cycles
# Backend Bound  ≈ stalled-cycles-backend / cycles
# Retiring       ≈ 1 - (frontend + backend) / cycles
```

与 Intel TMAM 的对应关系：

| Intel TMAM Level 1 | AMD 等效方法 | 精度 |
|--------------------|-------------|------|
| Retiring | `1 - (stalled_fe + stalled_be)/cycles` | 近似 |
| Frontend Bound | `stalled-cycles-frontend / cycles` | 近似 |
| Backend Bound | `stalled-cycles-backend / cycles` | 近似 |
| Bad Speculation | 无法直接区分 | 缺失 |

**方案 2: `perf stat -d` + cache/branch 指标**

```bash
perf stat -d -- ./program

# 从 IPC + cache miss 率 + branch miss 率推断瓶颈类型:
# IPC < 1 + cache-miss > 30% → Memory Bound
# IPC < 1 + branch-miss > 5% → Bad Speculation
# IPC < 1 + cache-miss < 10% → Core Bound
```

**方案 3: AMD IBS（Instruction Based Sampling）**

```bash
# IBS 是 AMD 独有的精确采样，比 Intel PEBS 信息更丰富
perf record -e ibs_op// -g -- ./program
perf report

# IBS Op 提供每条指令的:
# - DC miss latency（★ 直接看到每次 cache miss 的实际延迟）
# - TLB 命中/缺失状态
# - 分支结果 + 是否误预测
# - 数据访问地址
```

**方案 4: AMD μProf（AMD 官方分析工具）**

```bash
# 下载: https://www.amd.com/en/developer/uprof.html
# 提供类似 Intel VTune 的图形化 Top-Down 分析
AMDuProfCLI collect --config muarch_topdown -- ./program
```

**★ 推荐诊断流程：**
1. 先用 `perf stat -e stalled-cycles-*` 确定 Frontend/Backend 方向
2. 用 `perf stat -e L1-*,LLC-*,branch-*` 细分 Memory/Branch 问题
3. 用 IBS 深入到指令级定位热点

---

## Q4: CPU 高占用低吞吐如何用 Top-Down 方法诊断？

**日期：** 2026-07-04
**场景：** 服务器 CPU 利用率 90%+，但应用吞吐只有预期的 1/3，需要系统化定位瓶颈
**相关文件：** reading/05_high_cpu_low_throughput.md

### 回答

**★ 5 步诊断法：**

**Step 1: 看 IPC — 确认是否存在大量 stall**

```bash
perf stat -e cycles,instructions -- ./program

# IPC = instructions / cycles
# IPC < 1.0 → 严重 stall（> 75% 的流水线 Slot 被浪费）
# IPC 0.5-1.0 → 中等 stall
# IPC > 2.0 → CPU 效率高，问题可能在 I/O 或锁
```

**Step 2: 看 Frontend Bound vs Backend Bound — 确定瓶颈方向**

```bash
# Intel:
perf stat --topdown --td-level 1 -- ./program

# AMD:
perf stat -e stalled-cycles-frontend,stalled-cycles-backend -- ./program

# Backend Bound > 50% → 后端是主要瓶颈（内存/执行单元）
# Frontend Bound > 30% → 前端瓶颈（取指/解码）
# Bad Speculation > 25% → 分支预测问题
```

**Step 3: 看 Memory Bound vs Core Bound — 定位后端瓶颈类型**

```bash
# Intel:
perf stat --topdown --td-level 2 -- ./program

# AMD: 结合 L3 miss 率推断
perf stat -e stalled-cycles-backend,LLC-loads,LLC-load-misses -- ./program

# Memory Bound 占 Backend 的 70%+ → 内存瓶颈
# Core Bound 占 Backend 的 70%+ → 执行单元/依赖链瓶颈
```

**Step 4: 看 L1/L2/L3 miss 率 — 定位哪级缓存是瓶颈**

```bash
perf stat -e \
  L1-dcache-loads,L1-dcache-load-misses,\
  LLC-loads,LLC-load-misses,\
  dTLB-loads,dTLB-load-misses \
  -- ./program

# L3 miss > 30% → DRAM 访问是瓶颈
# L1 miss 高但 L3 miss 低 → L1 压力但 L3 兜底
# TLB miss > 5% → 需要大页
```

**Step 5: 看 NUMA 跨节点访问比例 — 排除远端内存访问**

```bash
perf mem record -- ./program
perf mem report --sort=mem

# 关注 "Remote RAM" 和 "Remote L3" 的比例
# Remote > 20% → NUMA 本地性问题 → numactl 绑定
```

**三种典型瓶颈模式速查：**

| 模式 | IPC | Backend | L3 Miss | 根因 | 优化 |
|------|-----|---------|---------|------|------|
| 大内存扫描 | 0.3 | 89% | 55% | 工作集 >> L3 | 流式预取、压缩 |
| 随机访问 | 0.15 | 93% | 70% | 指针追踪 | 数据结构优化 |
| 锁竞争 | 0.6 | 55% | 20% | 自旋锁等待 | per-CPU 数据、RCU |

**AMD 特别注意事项：**
- 8 核共享 32MB L3（CCD 内）→ 工作集 > 32MB 时 L3 miss 必然高
- 跨 CCD 访问延迟翻倍（~100 vs ~35 周期）→ 用 `taskset` 绑定协作线程到同一 CCD
- 检查 NPS 配置 → `numactl --hardware` 确认 NUMA 节点数

---

## Q5: Backend Bound 中 Memory Bound 和 Core Bound 如何区分？

**日期：** 2026-07-04
**场景：** Top-Down Level 1 分析显示 Backend Bound 占比很高，需要进一步定位是内存瓶颈还是执行单元瓶颈
**相关文件：** `reading/05_high_cpu_low_throughput.md`、`reading/02_command_reference.md`

### 回答

**★ Memory Bound 和 Core Bound 的区分是 TMAM Level 2 的核心任务。** 两者使用不同的 PMU 事件来判断 — Memory Bound 通过内存子系统停顿事件（`CYCLE_ACTIVITY.STALLS_MEM_ANY`）直接测量，Core Bound 通过排除法（`Backend Bound - Memory Bound`）得到。

#### TMAM Level 2 分类逻辑

```
                    All Pipeline Slots
                    ┌───────────────────┐
                    │                   │
              ┌─────┴─────┐       ┌────┴─────┐
              │ Retiring  │       │ Not      │
              │ (有用的)   │       │ Retiring │
              └───────────┘       └────┬─────┘
                                       │
                    ┌──────────────────┼──────────────────┐
                    │                  │                  │
              ┌─────┴──────┐  ┌───────┴──────┐  ┌───────┴──────┐
              │ Frontend   │  │ Bad          │  │ Backend      │
              │ Bound      │  │ Speculation  │  │ Bound        │
              └────────────┘  └──────────────┘  └──────┬───────┘
                                                       │
                                        ┌──────────────┴──────────────┐
                                        │                             │
                                 ┌──────┴──────┐              ┌───────┴──────┐
                                 │ Memory      │              │ Core         │
                                 │ Bound       │              │ Bound        │
                                 └─────────────┘              └──────────────┘
                                     ★ Level 2 ★
```

#### ★ 核心 PMU 事件对照表

| 分类 | PMU 事件 | 含义 | 源码位置 |
|------|---------|------|---------|
| **Memory Bound** | `CYCLE_ACTIVITY.STALLS_MEM_ANY` | 因内存子系统有 outstanding load 导致的执行停顿 | `skylake/pipeline.json:453` |
| **Core Bound** | 无直接事件 | Backend Bound 减去 Memory Bound 的差值 | 计算公式 |
| Backend Bound 总计 | `CYCLE_ACTIVITY.STALLS_TOTAL` | 所有类型的执行停顿 | `skylake/pipeline.json:403` |
| 补充事件 | `EXE_ACTIVITY.EXE_BOUND_0PORTS` | 无端口可用的执行停顿（Core Bound 特征） | `skylake/pipeline.json:463` |

**源码证据（Skylake 事件定义）：**

```json
// 源码：src/linux-5.10/tools/perf/pmu-events/arch/x86/skylake/pipeline.json:453
{
    "EventCode": "0xA3",
    "UMask": "0x14",
    "EventName": "CYCLE_ACTIVITY.STALLS_MEM_ANY",
    "BriefDescription": "Execution stalls while memory subsystem
                         has an outstanding load."
}

// 源码：src/linux-5.10/tools/perf/pmu-events/arch/x86/skylake/pipeline.json:403
{
    "EventCode": "0xA3",
    "UMask": "0x04",
    "EventName": "CYCLE_ACTIVITY.STALLS_TOTAL",
    "BriefDescription": "Total execution stalls."
}
```

#### ★ 计算公式

```
Level 2 Backend Bound 分解（来源：reading/02_command_reference.md 第 325-329 行）：

Memory Bound = CYCLE_ACTIVITY.STALLS_MEM_ANY / CPU_CLK_UNHALTED.THREAD

Core Bound   = Backend Bound - Memory Bound
             = (CYCLE_ACTIVITY.STALLS_TOTAL - CYCLE_ACTIVITY.STALLS_MEM_ANY)
               / CPU_CLK_UNHALTED.THREAD

★ 关键理解：
  - Memory Bound 是直接测量的（有专用 PMU 事件）
  - Core Bound 是排除法得到的（总停顿 - 内存停顿 = 执行单元停顿）
  - 这意味着 Core Bound 包含了"不在等待内存但也没在干活"的周期
```

#### Memory Bound vs Core Bound 的特征差异

| 特征 | Memory Bound | Core Bound |
|------|-------------|-----------|
| **根本原因** | 等待数据从缓存/内存到达 | 执行单元忙或指令依赖链 |
| **PMU 特征** | `STALLS_MEM_ANY` 高 | `STALLS_MEM_ANY` 低但 `STALLS_TOTAL` 高 |
| **L3 miss 率** | ★ 高（> 30%） | 低（< 10%） |
| **IPC** | 低 | 低 |
| **典型场景** | 大数据集扫描、随机访问、cache 污染 | 除法运算、长依赖链、端口竞争 |
| **优化方向** | 预取、数据局部性、大页 | 算法优化、减少依赖链 |

（来源：reading/05_high_cpu_low_throughput.md 第 194-195 行）

#### ★ 实战诊断流程

**Step 1：Level 2 Top-Down 分析（Intel 平台）**

```bash
# 一步获取 Level 2 全部指标
perf stat --topdown --td-level 2 -- ./program

# 输出示例：
# Topdown accuracy: 99.8%
#   Retiring:           28.5%
#   Bad Speculation:     2.5%
#   Frontend Bound:      5.2%
#   Backend Bound:      63.8%
#     Memory Bound:     52.3%    ← ★ 占 Backend 的 82%
#     Core Bound:       11.5%
```

**Step 2：用原始 PMU 事件验证**

```bash
# 采集 Memory Bound 和 Core Bound 相关事件
perf stat -e \
  CYCLE_ACTIVITY.STALLS_MEM_ANY,\
  CYCLE_ACTIVITY.STALLS_TOTAL,\
  CYCLE_ACTIVITY.CYCLES_MEM_ANY,\
  EXE_ACTIVITY.EXE_BOUND_0PORTS,\
  MEM_LOAD_RETIRED.L3_MISS,\
  MEM_LOAD_RETIRED.L3_HIT \
  -- ./program

# 判断逻辑：
# STALLS_MEM_ANY / STALLS_TOTAL > 70% → Memory Bound 为主
# STALLS_MEM_ANY / STALLS_TOTAL < 30% → Core Bound 为主
```

**Step 3：AMD 平台的替代方法**

```bash
# AMD 没有 TMAM 的专用事件，用以下方式近似：

# 方法 1：stalled-cycles + cache miss 推断
perf stat -e \
  stalled-cycles-backend,\
  LLC-loads,LLC-load-misses,\
  L1-dcache-loads,L1-dcache-load-misses \
  -- ./program

# 判断逻辑：
# LLC miss 率高 + stalled-cycles-backend 高 → Memory Bound
# LLC miss 率低 + stalled-cycles-backend 高 → Core Bound

# 方法 2：perf stat -d（通用指标）
perf stat -d -- ./program
# IPC < 1 + cache-miss > 30% → Memory Bound
# IPC < 1 + cache-miss < 10% → Core Bound
```

#### Level 3 进一步细分（Intel）

```
Memory Bound 细分（Level 3）：
┌──────────────────────┬─────────────────────────────────────────┐
│ L1 Bound             │ L1 缓存命中但调度器满                    │
│ L2 Bound             │ L2 缓存未命中导致停顿                    │
│ L3 Bound             │ L3 缓存未命中导致停顿                    │
│ DRAM Bound           │ 主存访问导致长延迟停顿                   │
│ Store Bound          │ Store Buffer 满                          │
└──────────────────────┴─────────────────────────────────────────┘

Core Bound 细分（Level 3）：
┌──────────────────────┬─────────────────────────────────────────┐
│ Divider              │ 除法单元繁忙                             │
│ Port 0/1/5           │ 特定执行端口竞争                         │
│ Port 2/3/4           │ 加载/存储端口竞争                        │
└──────────────────────┴─────────────────────────────────────────┘

（来源：reading/02_command_reference.md 第 167-192 行）
```

#### 三种典型场景对比

```
场景 A：大内存扫描（Memory Bound）
  perf stat --topdown --td-level 2:
    Memory Bound: 82%  ← ★ L3 miss 高，DRAM 访问多
    Core Bound:    7%
  优化：流式预取、数据压缩、减少工作集

场景 B：密集浮点运算（Core Bound）
  perf stat --topdown --td-level 2:
    Memory Bound:  8%  ← ★ 数据在 L1 中，但计算单元忙
    Core Bound:   65%
  优化：SIMD 向量化、减少依赖链、循环展开

场景 C：指针追踪/链表遍历（Memory Bound）
  perf stat --topdown --td-level 2:
    Memory Bound: 88%  ← ★ 随机访问导致 cache miss 级联
    Core Bound:    5%
  优化：数据结构改为数组、cache-friendly 布局
```

**一句话总结：** Memory Bound 通过 `CYCLE_ACTIVITY.STALLS_MEM_ANY` 直接测量（等数据的周期），Core Bound 通过排除法（`Backend Bound - Memory Bound`）得到（有数据但没活干的周期）。`perf stat --topdown --td-level 2` 一步获取两者的占比，L3 miss 率是辅助判断的关键指标。
