# perf mem 与 AMD 特定事件

> ======================================================================
> 阅读要点：
>   1. perf mem 内存访问延迟分析
>   2. AMD 与 Intel 的 perf 事件差异
>   3. AMD IBS（Instruction Based Sampling）的使用
>   4. AMD 特有的性能分析问题
>
> 相关源码：
>   - src/linux-5.10/tools/perf/builtin-mem.c       — perf mem 实现
>   - src/linux-5.10/arch/x86/events/amd/core.c     — AMD PMU 驱动
>   - src/linux-5.10/arch/x86/events/amd/ibs.c      — AMD IBS 实现
>
> 预计阅读时间：20 分钟
> ======================================================================

---

## 一、perf mem — 内存访问延迟分析

### 1.1 基本原理

```
perf mem 使用 ★ PEBS（Intel）或 IBS（AMD）的内存采样功能：
  → 记录每次 load/store 的：
    - 数据地址（虚拟 + 物理）
    - ★ 延迟（从发起访问到完成的周期数）
    - ★ 数据来源（L1/L2/L3/Local DRAM/Remote DRAM）
    - 访问类型（load/store）

源码：src/linux-5.10/tools/perf/builtin-mem.c
```

### 1.2 基本用法

```bash
# 采集（load + store）
perf mem record -t load ./app          # 只追踪 load
perf mem record -t store ./app         # 只追踪 store
perf mem record -t ldst ./app          # load + store（默认）
perf mem record -a sleep 10            # 全系统 10 秒

# 分析
perf mem report                        # 交互式 TUI
perf mem report --stdio                # 文本输出
```

### 1.3 输出字段解读

```
perf mem report 输出列：

字段              │ 含义                          │ 诊断要点
─────────────────┼──────────────────────────────┼──────────────────
Symbol           │ 函数名                        │ ★ 热点函数
DSO              │ 所属共享库                    │ —
TLB              │ TLB 命中情况                  │ ★ miss → 大页优化
Cache            │ 缓存命中层级                  │ ★ L1/L2/L3/DRAM
Mem              │ 内存类型                      │ —
Snoop            │ 是否需要 cache 一致性交互     │ ★ HITM → false sharing
Cycles           │ 访问延迟（CPU 周期）          │ ★ 越高越慢

★ 数据来源层级：
  L1 hit:     ~4 周期
  L2 hit:     ~12 周期
  L3 hit:     ~40 周期
  Local DRAM: ~200 周期
  Remote DRAM: ~400 周期（NUMA 远程节点）
  Remote Cache: ~100 周期（远程 socket 的 L3）
```

### 1.4 延迟分布分析

```bash
# 查看内存访问延迟的直方图分布
perf mem report --sort=mem,snoop --stdio
```

```
典型输出：

  Overhead  Memory access     Snoop
  ........  ................  ...........
    65.3%   L1 hit            HitM
    15.2%   L2 hit            HitM
     8.5%   L3 hit            HitM
     6.2%   Local DRAM        HitM
     3.1%   Remote DRAM       HitM
     1.7%   LFB hit           HitM

诊断：
  ★ L1/L2 占比高 → 内存访问模式良好
  ★ DRAM 占比高 → 工作集大或访问不连续
  ★ Remote DRAM → NUMA 不友好
  ★ HitM 频繁 → false sharing（配合 perf c2c 确认）
```

---

## 二、AMD 与 Intel 的 perf 事件差异

### 2.1 通用事件对比

```
perf 事件名           │ Intel 映射                    │ AMD 映射
─────────────────────┼──────────────────────────────┼──────────────────────
★ cycles                │ CPU_CLK_UNHALTED.THREAD_P    │ cpu cycles unhalted
★ instructions          │ INST_RETIRED.ANY             │ retired instructions
★ cache-references      │ LONGEST_LAT_CACHE.REFERENCE  │ L3 cache access
★ cache-misses          │ LONGEST_LAT_CACHE.MISS       │ L3 cache miss
★ branches              │ BR_INST_RETIRED.ALL_BRANCHES │ retired branch instr
★ branch-misses         │ BR_MISP_RETIRED.ALL_BRANCHES │ retired mispredict branch

★ 通用事件由内核自动映射，用户使用方式完全相同
★ 差异在内核 PMU 驱动中处理：
  Intel: src/linux-5.10/arch/x86/events/intel/core.c
  AMD:   src/linux-5.10/arch/x86/events/amd/core.c
```

### 2.2 查看 AMD 可用事件

```bash
# ★ 在 AMD 服务器上使用 perf list 查看可用事件
perf list                              # 所有事件
perf list hw                           # 硬件事件
perf list cache                        # 缓存事件
perf list pmu                          # PMU 特定事件

# AMD 特有的 PMU 事件（通过 raw 编码访问）
perf list --desc --details             # 带描述的详细列表
```

### 2.3 AMD 原始事件编码

```bash
# AMD 使用 raw 事件编码（rNNN）访问架构特定事件
# 事件编码参考 AMD Processor Programming Reference (PPR)

# 示例：AMD Zen 3/4 的 L3 缓存事件
perf stat -e r0400000000431004 ./app     # L3 访问（AMD 特定编码）
perf stat -e r0400000000431008 ./app     # L3 miss

# 示例：AMD Data Fabric 事件（跨 NUMA 流量）
perf stat -e amd_df/event=0x7,umask=0x38/ ./app  # DRAM 读取

★ 注意：raw 事件编码因 AMD 微架构版本而异
  Zen 2、Zen 3、Zen 4 的事件编码不同
  参考文档：AMD PPR for Family 19h（Zen 3/4）或 Family 1Ah（Zen 5）
```

### 2.4 AMD 与 Intel 的微架构差异

```
特性                    │ Intel (Xeon)         │ AMD (EPYC)
───────────────────────┼─────────────────────┼─────────────────────
L3 结构                │ ★ 统一 LLC             │ ★ 每 CCX 独立 L3
                       │ 所有核心共享          │ 核心只能访问本 CCX 的 L3
CCX / CCD             │ ★ 不适用                │ ★ 4~8 核一个 CCX
                       │                       │ 2~12 个 CCX 一个 CCD
NUMA 拓扑             │ ★ 每 socket 1 NUMA     │ ★ 每 CCD 可能 1 NUMA
                       │                       │ （NPS 配置决定）
PMC 数量               │ 4 通用 + 3 固定      │ 4~6 通用（无固定）
精确采样               │ ★ PEBS                 │ ★ IBS（更强大）
L3 miss 事件          │ 全局 LLC miss        │ ★ 按 CCX 分别统计
```

---

## 三、AMD IBS（Instruction Based Sampling）

### 3.1 IBS 是什么

```
IBS（Instruction Based Sampling）是 AMD 独有的精确采样技术：
  → ★ 比 Intel PEBS 功能更丰富
  → 可以同时获取：
    - 指令的精确地址
    - 指令的延迟（周期数）
    - 数据来源（缓存层级）
    - 分支预测结果
    - TLB 命中情况

★ IBS 有两种模式（源码 ibs.c:191-192）：
  perf_ibs_fetch → 指令获取采样（Instruction Fetch）
  perf_ibs_op    → 操作采样（Micro-Op）
```

### 3.2 IBS 使用方法

```bash
# IBS Fetch（指令获取采样）
perf record -e ibs_fetch// ./app
perf report

# IBS Op（操作采样）
perf record -e ibs_op// ./app
perf report

# IBS 提供精确的 per-instruction 分析
# 比 Intel PEBS 更细粒度（可以看到每条指令的延迟）

# 查看 IBS 是否可用
perf list | grep ibs
# 输出：
#   ibs_fetch//  [Kernel PMU event]
#   ibs_op//     [Kernel PMU event]
```

### 3.3 IBS vs Intel PEBS

```
特性              │ Intel PEBS              │ AMD IBS
─────────────────┼────────────────────────┼────────────────────────
精确性           │ 指令级                  │ ★ 微操作级
延迟信息         │ load 延迟               │ ★ 指令延迟 + load 延迟
分支信息         │ 有限                    │ ★ 完整的分支预测结果
TLB 信息         │ 无                      │ ★ TLB 命中/miss
数据来源         │ 缓存层级                │ ★ 缓存层级 + NUMA 节点
采样开销         │ 低                      │ ★ 低
可用性           │ Intel Core 及以上       │ ★ AMD Family 10h 及以上

★ 在 AMD 上，IBS 是最强大的性能分析工具
  → 可以精确定位到每条指令的延迟来源
  → 支持 perf c2c（false sharing 检测）
  → 支持 perf mem（内存延迟分析）
```

---

## 四、AMD 服务器性能分析注意事项

### 4.1 NUMA 拓扑分析

```
AMD EPYC 的 NUMA 拓扑比 Intel 更复杂：

NPS（Nodes Per Socket）配置：
  ★ NPS1: 1 个 NUMA 节点/socket（所有 CCD 共享）
  ★ NPS2: 2 个 NUMA 节点/socket
  NPS4: 4 个 NUMA 节点/socket（★ 每个 CCD 一个 NUMA）

★ NPS4 性能最好但 NUMA 敏感性最高
  → ★ 跨 NUMA 访问延迟增加 50%~100%
  → 必须使用 ★ numactl 绑定进程到正确的 NUMA 节点

检查 NUMA 配置：
  numactl --hardware
  lscpu | grep NUMA
```

### 4.2 CCX 内 vs CCX 间的 L3 访问

```
AMD 的 L3 是 per-CCX 的（非全局共享）：

  CCD 0                    CCD 1
  ┌─────────────┐         ┌─────────────┐
  │ Core 0~7    │         │ Core 8~15   │
  │ L3 (32MB)   │         │ L3 (32MB)   │
  └─────────────┘         └─────────────┘
       │                        │
       └──── 内存控制器 ────────┘

★ Core 0 访问 CCD 0 的 L3: ~40 周期（快）
★ Core 0 访问 CCD 1 的 L3: 不存在！（只能访问本 CCX 的 L3）
★ Core 0 访问远程 DRAM: ~200 周期（经过内存控制器）

→ ★ perf stat -e cache-misses 在 AMD 上显示的是**本 CCX 的 L3 miss**
  不像 Intel 是全局 LLC miss
→ 这意味着 ★ AMD 上 cache-miss 的含义不同！
```

### 4.3 AMD 性能分析快速命令

```bash
# 1. 基本性能指标
perf stat -d -r 3 ./app

# 2. 使用 IBS 进行精确分析
perf record -e ibs_op// -g ./app
perf report --stdio

# 3. NUMA 分析
perf stat -e cache-misses -C 0 -- ./app   # 只看 CPU 0
perf stat -e cache-misses -C 8 -- ./app   # 对比 CPU 8（不同 CCD）

# 4. 内存访问分析
perf mem record -t load ./app
perf mem report --sort=mem,snoop --stdio

# 5. False sharing 检测
perf c2c record ./app
perf c2c report --stdio
```

---

## 总结

### AMD vs Intel 关键差异速查

```
项目              │ Intel                      │ AMD
─────────────────┼───────────────────────────┼──────────────────────────
L3 结构           │ ★ 全局共享 LLC               │ per-CCX 独立 L3
精确采样          │ PEBS                       │ ★ IBS（更强大）
NUMA              │ 每 socket 1 NUMA           │ ★ 每 CCD 可配 NUMA
cache-miss 含义   │ 全局 LLC miss              │ ★ 本 CCX L3 miss
原始事件编码      │ Intel SDM Vol.3B           │ AMD PPR
PMC 数量          │ 4 通用 + 3 固定            │ 4~6 通用
查看事件          │ perf list                  │ perf list（相同命令）
```

### 源码位置

```
perf mem:          src/linux-5.10/tools/perf/builtin-mem.c
AMD PMU 驱动:      src/linux-5.10/arch/x86/events/amd/core.c
AMD IBS:           src/linux-5.10/arch/x86/events/amd/ibs.c
Intel PMU 驱动:    src/linux-5.10/arch/x86/events/intel/core.c
Intel PEBS:        src/linux-5.10/arch/x86/events/intel/ds.c
```
