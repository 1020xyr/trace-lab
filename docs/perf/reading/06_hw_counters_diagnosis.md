# perf 硬件计数器详解与性能诊断

> ======================================================================
> 阅读要点：
>   1. 硬件计数器（PMC）的物理本质与内核映射
>   2. IPC（Instructions Per Cycle）的含义与诊断阈值
>   3. cache-miss 率的正常/异常范围与诊断
>   4. branch-miss 率与分支预测失败的代价
>   5. 用 perf stat 诊断 CPU 高占用低吞吐的完整流程
>
> 相关源码：
>   - src/linux-5.10/include/uapi/linux/perf_event.h — enum perf_hw_id 定义
>   - src/linux-5.10/arch/x86/events/core.c          — x86 PMU 驱动
>   - src/linux-5.10/arch/x86/events/intel/core.c    — Intel PMU 配置
>   - src/linux-5.10/arch/x86/events/amd/core.c      — AMD PMU 配置
>   - src/linux-5.10/tools/perf/builtin-stat.c       — perf stat 实现
>
> 预计阅读时间：25 分钟
> ======================================================================

---

## 一、硬件计数器（PMC）的物理本质

### 1.1 PMC 是什么

PMC（Performance Monitoring Counter）是 CPU 内部的**专用硬件寄存器**，
由 PMU（Performance Monitoring Unit）驱动，能够在不干扰程序执行的情况下
精确记录微架构事件。

```
┌──────────────────────────────────────────────────────────┐
│                      CPU Core                             │
│                                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
│  │ ALU/FPU  │  │ L1 Cache │  │ 分支预测器│  │ Load/Store│ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬────┘ │
│       │              │             │              │       │
│  ┌────▼──────────────▼─────────────▼──────────────▼──┐  │
│  │              PMU (Performance Monitor Unit)         │  │
│  │                                                    │  │
│  │  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐          │  │
│  │  │ PMC0 │  │ PMC1 │  │ PMC2 │  │ PMC3 │  ...     │  │
│  │  │cycles│  │instr │  │cache │  │branch│          │  │
│  │  └──────┘  └──────┘  └──────┘  └──────┘          │  │
│  │                                                    │  │
│  │  ★ 每个 PMC 是一个 48~64 位的硬件计数器            │  │
│  │  ★ 每个时钟周期自动递增（零开销！）                │  │
│  │  ★ 可配置为计数不同的微架构事件                    │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

### 1.2 内核如何映射 PMC

内核通过 `enum perf_hw_id`（`perf_event.h:45`）将通用事件名映射到
架构特定的 PMC 配置：

```c
/* src/linux-5.10/include/uapi/linux/perf_event.h */
enum perf_hw_id {
    PERF_COUNT_HW_CPU_CYCLES           = 0,  /* CPU 周期 */
    PERF_COUNT_HW_INSTRUCTIONS         = 1,  /* 退休指令数 */
    PERF_COUNT_HW_CACHE_REFERENCES     = 2,  /* LLC 访问 */
    PERF_COUNT_HW_CACHE_MISSES         = 3,  /* LLC 未命中 */
    PERF_COUNT_HW_BRANCH_INSTRUCTIONS  = 4,  /* 分支指令 */
    PERF_COUNT_HW_BRANCH_MISSES        = 5,  /* 分支预测失败 */
    PERF_COUNT_HW_BUS_CYCLES           = 6,  /* 总线周期 */
    PERF_COUNT_HW_STALLED_CYCLES_FRONTEND = 7, /* 前端停顿 */
    PERF_COUNT_HW_STALLED_CYCLES_BACKEND  = 8, /* 后端停顿 */
    PERF_COUNT_HW_REF_CPU_CYCLES       = 9,  /* 参考周期 */
};
```

**★ 关键认知：** 这些是**通用化事件**（generic events）。内核将
`cycles` 翻译为架构特定的 MSR 配置值。在 Intel 上 `cycles` 映射到
`CPU_CLK_UNHALTED.THREAD_P`，在 AMD 上映射到 `cpu cycles unhalted`。

### 1.3 PMC 数量限制

```
CPU 架构               │ 通用 PMC 数量  │ 固定功能计数器
──────────────────────┼──────────────┼──────────────────
Intel Core (Skylake+) │ 4 个通用     │ 3 个固定 (cycles/instr/ref)
Intel Xeon (Ice Lake) │ 4~8 个通用   │ 3~4 个固定
AMD Zen 2/3/4         │ 4~6 个通用   │ 无固定功能计数器
AMD Zen 5             │ 6~12 个通用  │ 无固定功能计数器

★ 当请求的事件数 > PMC 数量时，内核自动进行**多路复用**（multiplexing）
  → perf stat 输出会显示 "<not counted>" 或缩放后的估计值
  → 解决方案：减少同时统计的事件数，或用事件组 {}
```

---

## 二、perf stat 硬件计数器详解

### 2.1 核心事件组合

```bash
# ★ 性能诊断第一步：获取核心硬件指标
perf stat -e cache-misses,cache-references,L1-dcache-load-misses,\
instructions,cycles,branches,branch-misses ./app
```

### 2.2 各事件含义与映射

```
perf 事件名              │ perf_hw_id │ 映射到的微架构事件
────────────────────────┼──────────┼──────────────────────────────
cycles                   │ 0        │ CPU_CLK_UNHALTED (核心时钟)
instructions             │ 1        │ INST_RETIRED (退休指令)
cache-references         │ 2        │ LLC (最后一级缓存) 访问
cache-misses             │ 3        │ LLC 未命中（→ 主存访问）
branches                 │ 4        │ 分支指令退休
branch-misses            │ 5        │ 分支预测错误
L1-dcache-loads          │ HW_CACHE │ L1 数据缓存加载
L1-dcache-load-misses    │ HW_CACHE │ L1 数据缓存未命中
```

---

## 三、IPC（Instructions Per Cycle）

### 3.1 IPC 的含义

```
IPC = instructions / cycles

IPC 衡量 CPU 每个时钟周期能执行多少条指令。
★ IPC 是判断 CPU 瓶颈类型的最重要单一指标。
```

### 3.2 IPC 诊断阈值

```
IPC 值       │ 性能评级  │ 瓶颈类型              │ 优化方向
────────────┼──────────┼──────────────────────┼──────────────────
> 2.0       │ ★ 优秀    │ 计算密集型             │ 已充分利用流水线
1.5 ~ 2.0   │ 良好     │ 轻度内存等待           │ 可进一步优化
1.0 ~ 1.5   │ 一般     │ 混合负载               │ 检查缓存命中率
0.5 ~ 1.0   │ ★ 较差   │ 内存瓶颈 / 长延迟操作  │ 优化数据局部性
< 0.5       │ ★★ 很差  │ 严重停顿               │ 检查 cache/TLB/分支

★ 理论最大值取决于 CPU 微架构：
  Intel Skylake:  4-wide → 理论 IPC 上限 4.0
  AMD Zen 3/4:   4-wide → 理论 IPC 上限 4.0
  实际程序中 IPC > 2.0 已经非常好
```

### 3.3 IPC 与瓶颈类型诊断

```
┌──────────────────────────────────────────────────────┐
│              IPC 诊断决策树                            │
│                                                      │
│  IPC < 0.5?                                          │
│  ├── 是 → 严重停顿，检查：                           │
│  │     ├── cache-miss 率高? → L3 cache 瓶颈         │
│  │     ├── branch-miss 率高? → 分支预测失败         │
│  │     ├── TLB miss 多? → 工作集太大                 │
│  │     └── stalled-cycles-backend 高? → 内存带宽瓶颈│
│  │                                                   │
│  └── 否 → IPC 0.5~1.5?                               │
│        ├── 是 → 中度停顿，检查：                     │
│        │     ├── cache-miss 率                       │
│        │     └── 是否 I/O 密集                       │
│        │                                             │
│        └── 否 → IPC > 1.5                            │
│              └── 计算密集型，CPU 利用率高（正常）     │
└──────────────────────────────────────────────────────┘
```

### 3.4 stalled-cycles 前端 vs 后端

```bash
perf stat -e stalled-cycles-frontend,stalled-cycles-backend,cycles,instructions ./app
```

```
stalled-cycles-frontend（前端停顿）：
  → 指令获取/解码跟不上
  → 原因：I-cache miss、分支预测失败、指令 TLB miss
  → 前端停顿 / cycles > 30% → 指令供给不足

stalled-cycles-backend（后端停顿）：
  → 执行单元/内存子系统跟不上
  → 原因：D-cache miss、TLB miss、执行端口争用、内存带宽
  → 后端停顿 / cycles > 40% → 数据访问瓶颈

★ 典型模式：
  计算密集: frontend 低, backend 低, IPC 高
  内存密集: frontend 低, backend 高, IPC 低
  分支密集: frontend 高, backend 低, IPC 中
```

---

## 四、Cache-Miss 率分析

### 4.1 三级缓存延迟

```
层级    │ 容量（典型） │ 延迟（周期） │ 延迟（ns @3GHz）
───────┼────────────┼────────────┼──────────────────
L1-D    │ 32~48 KB   │ 4~5        │ ~1.5 ns
L1-I    │ 32~64 KB   │ 4~5        │ ~1.5 ns
L2      │ 256~1024 KB│ 10~14      │ ~4 ns
L3 (LLC)│ 16~96 MB   │ 35~70      │ ~15 ns
主存    │ GB~TB      │ 200~400    │ ~100 ns

★ 每次 L3 miss → 访问主存 → 约 100ns → 约 300 个 CPU 周期
★ 这就是为什么 cache-miss 率对性能影响巨大
```

### 4.2 cache-miss 率 = cache-misses / cache-references

```bash
perf stat -e cache-misses,cache-references ./app
```

```
cache-miss 率           │ 评级      │ 含义
───────────────────────┼──────────┼─────────────────────────
< 1%                   │ ★ 优秀    │ 工作集完全在 LLC 内
1% ~ 5%                │ 良好      │ 偶尔 miss，正常
5% ~ 15%               │ ★ 一般    │ 工作集偏大或访问模式差
15% ~ 30%              │ ★★ 较差   │ 严重 cache 不友好
> 30%                  │ ★★★ 很差  │ 内存带宽瓶颈

★ cache-references 映射到 LLC（最后一级缓存）访问
  即 L3 在 Intel 上，L3 在 AMD 上
★ cache-misses 映射到 LLC miss
  即需要访问主存的次数
```

### 4.3 L1 vs LLC miss 的区分

```bash
# 同时看 L1 和 LLC
perf stat -e L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses ./app
```

```
诊断矩阵：
L1 miss 率    │ LLC miss 率   │ 瓶颈判断
─────────────┼──────────────┼──────────────────────────
低 (<5%)      │ 低 (<5%)     │ 缓存友好，无问题
高 (>10%)     │ 低 (<5%)     │ L1 miss 多但 L2/LLC 兜住
低 (<5%)      │ 高 (>15%)    │ ★ 少见，可能 LLC 太小
高 (>10%)     │ 高 (>15%)    │ ★★ 严重！工作集远超缓存

★ L1 miss 但 LLC hit：延迟 ~15 周期（L2/L3 访问）
★ LLC miss → 主存：延迟 ~300 周期
  → LLC miss 的代价远大于 L1 miss
```

---

## 五、Branch-Miss 率分析

### 5.1 分支预测失败的代价

```
分支预测失败（branch misprediction）的代价：

  正确预测: 0 周期惩罚（流水线不中断）
  预测失败: 15~20 周期惩罚（刷新流水线）

  ★ 现代 CPU 流水线深度 14~19 级
  ★ 每次 mispredict = 浪费一整条流水线的工作

  在 3GHz CPU 上：
    15 周期 = 5ns
    每秒 100M 次 mispredict = 500ms 浪费
    → 半秒的 CPU 时间白白丢失！
```

### 5.2 branch-miss 率诊断

```bash
perf stat -e branches,branch-misses ./app
```

```
branch-miss 率                │ 评级      │ 含义
─────────────────────────────┼──────────┼─────────────────────
branch-misses/branches < 1%  │ ★ 优秀    │ 分支预测几乎全对
1% ~ 5%                      │ 良好      │ 正常水平
5% ~ 10%                     │ ★ 一般    │ 有优化空间
> 10%                        │ ★★ 较差   │ 分支预测严重失败

★ 高 branch-miss 率的常见原因：
  - 大量 if/else 且条件随机
  - 虚函数调用（间接分支）
  - 数据依赖的条件判断
  - 搜索树/哈希表的随机跳转

★ 优化方法：
  - __builtin_expect() 提示编译器
  - 减少不可预测的分支（用 CMOV/位运算替代）
  - 排序数据使分支更可预测
  - PGO（Profile Guided Optimization）编译
```

---

## 六、CPU 高占用低吞吐诊断流程

### 6.1 完整诊断流程

```
CPU 高占用低吞吐 — 这意味着 CPU 很忙但产出少

Step 1: 确认问题
  sar -u 1 5
  → %user 或 %system 很高，但业务吞吐低

Step 2: perf stat 获取硬件指标
  perf stat -d -r 3 -p <PID>
  → 获取 IPC、cache-miss 率、branch-miss 率

Step 3: 根据指标判断瓶颈
  ┌────────────────────────────────────────────┐
  │                                            │
  │  IPC < 1.0 ?                               │
  │  ├── 是 → 内存/缓存瓶颈                    │
  │  │     │                                    │
  │  │     ├── cache-miss 率 > 15% ?            │
  │  │     │   └── 是 → L3 cache miss 严重      │
  │  │     │       perf c2c record → false sharing?│
  │  │     │       perf mem record → 访问延迟    │
  │  │     │                                    │
  │  │     ├── branch-miss 率 > 5% ?            │
  │  │     │   └── 是 → 分支预测失败多          │
  │  │     │       perf record -b → 分支热点    │
  │  │     │                                    │
  │  │     └── TLB miss 多 ?                    │
  │  │         └── 是 → 工作集太大              │
  │  │             考虑大页（Huge Pages）        │
  │  │                                          │
  │  └── 否 → IPC > 1.0 但吞吐低               │
  │        │                                     │
  │        ├── %system 高 ?                      │
  │        │   └── 是 → 锁竞争/系统调用密集      │
  │        │       perf lock record              │
  │        │       perf record -e syscalls:*     │
  │        │                                     │
  │        ├── context-switches 高 ?             │
  │        │   └── 是 → 过度调度                 │
  │        │       减少线程数/调整 CPU 亲和性    │
  │        │                                     │
  │        └── 计算本身低效                      │
  │            perf record -g → 热点函数         │
  │            优化算法复杂度                     │
  └────────────────────────────────────────────┘
```

### 6.2 物理机诊断命令序列

```bash
# ★ Step 1: 快速性能概览（3 次取平均）
perf stat -d -r 3 ./app

# ★ Step 2: 详细缓存和 TLB 统计
perf stat -d -d -d ./app

# ★ Step 3: 前端/后端停顿分析
perf stat -e stalled-cycles-frontend,stalled-cycles-backend,\
cycles,instructions ./app

# ★ Step 4: 热点函数 + 调用栈
perf record -g --call-graph dwarf -F 99 ./app
perf report --stdio --no-children

# ★ Step 5: 如果发现 cache-miss 高 → 深入分析
perf c2c record ./app        # false sharing 检测
perf mem record ./app        # 内存访问延迟

# ★ Step 6: 如果 %system 高 → 锁竞争分析
perf lock record ./app
perf lock report
```

---

## 七、TLB Miss 分析

### 7.1 TLB 的作用

```
TLB（Translation Lookaside Buffer）：
  → 缓存虚拟地址 → 物理地址的映射
  → 避免每次都查页表（page walk）

  TLB hit:  1~3 周期
  TLB miss: 10~100 周期（取决于页表深度，4 级页表需要 4 次内存访问）
```

### 7.2 TLB miss 诊断

```bash
perf stat -e dTLB-loads,dTLB-load-misses,iTLB-loads,iTLB-load-misses ./app
```

```
dTLB miss 率                    │ 评级      │ 含义
───────────────────────────────┼──────────┼──────────────────
dTLB-load-misses / dTLB < 0.1% │ ★ 优秀    │ TLB 命中率高
0.1% ~ 1%                      │ 良好      │ 偶尔 miss
1% ~ 5%                        │ ★ 注意    │ 工作集较大
> 5%                           │ ★★ 严重   │ 需要大页优化

★ 高 TLB miss 的解决方案：
  - 使用大页（Huge Pages）：2MB 或 1GB 页
    echo 1024 > /proc/sys/vm/nr_hugepages
    mmap + MAP_HUGETLB
  - 减少工作集（压缩数据结构）
  - 改善数据局部性（struct of arrays → array of structs）
```

---

## 总结

### 关键指标速查表

```
指标                          │ 计算方式                        │ 正常范围
─────────────────────────────┼────────────────────────────────┼──────────
IPC                           │ instructions / cycles          │ > 1.0
cache-miss 率                 │ cache-misses / cache-references│ < 5%
L1-dcache miss 率             │ L1-miss / L1-loads             │ < 5%
LLC miss 率                   │ LLC-miss / LLC-loads           │ < 10%
branch-miss 率                │ branch-misses / branches       │ < 5%
dTLB miss 率                  │ dTLB-miss / dTLB-loads         │ < 1%
前端停顿比                    │ stalled-frontend / cycles      │ < 30%
后端停顿比                    │ stalled-backend / cycles       │ < 40%
```

### 源码位置

```
事件定义:      src/linux-5.10/include/uapi/linux/perf_event.h:45 (enum perf_hw_id)
x86 PMU 驱动:  src/linux-5.10/arch/x86/events/core.c
Intel PMU:     src/linux-5.10/arch/x86/events/intel/core.c
AMD PMU:       src/linux-5.10/arch/x86/events/amd/core.c
perf stat:     src/linux-5.10/tools/perf/builtin-stat.c
```
