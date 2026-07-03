# Top-Down Microarchitecture Analysis Method (TMAM) 学习路线图

> 从流水线 Slot 到四级瓶颈树：系统性定位 CPU 微架构性能瓶颈

---

## 目录

- [1. 方法论概览](#1-方法论概览)
- [2. 四级层次模型](#2-四级层次模型)
- [3. 核心概念](#3-核心概念)
- [4. 学习路线（8 步）](#4-学习路线8-步)
- [5. 工具链](#5-工具链)
- [6. 实战实验清单](#6-实战实验清单)
- [7. 关键文件索引](#7-关键文件索引)
- [参考资源](#参考资源)

---

## 1. 方法论概览

### 1.1 什么是 TMAM

Top-Down Microarchitecture Analysis Method（TMAM）是由 **Ahmad Yasin** 和 **Aater Suleiman**（Intel）于 2013 年提出的系统化微架构性能分析方法。

**核心思想：** 将 CPU 流水线中每一个"Pipeline Slot"（流水线槽位）归类到四个互斥的大类中，通过逐级下钻（Top-Down）定位具体瓶颈原因。

### 1.2 为什么需要 TMAM

传统的 IPC（Instructions Per Cycle）分析只能告诉你"CPU 效率不高"，但无法回答"为什么不高"。TMAM 解决了三个关键问题：

| 问题 | 传统方法 | TMAM |
|------|---------|------|
| CPU 利用率低的原因？ | 逐个猜测（缓存？分支？） | 系统化分类，百分比量化 |
| 优化方向在哪？ | 靠经验 | 数据驱动，精准定位 |
| 不同程序瓶颈差异？ | 难以比较 | 统一框架，直接对比 |

### 1.3 核心公式

```
Pipeline Slot = 流水线宽度 × CPU 周期数
```

以 Skylake 为例，流水线宽度 = **4**（每周期最多退休 4 个 μop），因此：

```
Total Slots = 4 × CPU_CLK_UNHALTED.THREAD
```

每个 Slot 被归入以下四类之一：

```
Total Slots = Frontend Bound + Bad Speculation + Backend Bound + Retiring
```

### 1.4 整体架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Pipeline Slots (每周期 × 4)                       │
├──────────────┬──────────────┬──────────────┬────────────────────────┤
│              │              │              │                        │
│   Retiring   │  Bad Specu-  │   Frontend   │     Backend            │
│   (有效退休)  │  lation      │   Bound      │     Bound              │
│              │  (错误推测)   │  (前端瓶颈)   │    (后端瓶颈)           │
│  ┌────────┐  │  ┌────────┐  │  ┌────────┐  │  ┌────────┐           │
│  │ Base   │  │  │Branch  │  │  │Fetch   │  │  │Memory  │           │
│  │Operate │  │  │Mispre- │  │  │Latency │  │  │Bound   │           │
│  ├────────┤  │  │dict    │  │  ├────────┤  │  ├────────┤           │
│  │Micro-  │  │  ├────────┤  │  │Fetch   │  │  │Core    │           │
│  │sequencer│  │  │Machine │  │  │Band-   │  │  │Bound   │           │
│  │        │  │  │Clears  │  │  │width   │  │  │        │           │
│  └────────┘  │  └────────┘  │  └────────┘  │  └────────┘           │
└──────────────┴──────────────┴──────────────┴────────────────────────┘
```

---

## 2. 四级层次模型

### Level 1：四大类

```
                    Total Pipeline Slots
                    ┌────────┴────────┐
              ┌─────┴─────┐     ┌─────┴─────┐
              │ Frontend  │     │ Backend   │
              │   Bound   │     │   Bound   │
              └───────────┘     └───────────┘
              ┌───────────┐     ┌───────────┐
              │   Bad     │     │ Retiring  │
              │Speculation│     │(有效工作)  │
              └───────────┘     └───────────┘
```

| 类别 | 含义 | 典型原因 | 理想占比 |
|------|------|---------|---------|
| **Retiring** | 成功退休的 μop | 程序正常执行 | 越高越好（>60%） |
| **Frontend Bound** | 前端无法供给足够的 μop | I-cache miss, iTLB miss, 分支预测延迟 | 越低越好 |
| **Bad Speculation** | 因错误推测而浪费的 Slot | 分支预测失败、流水线冲刷 | 越低越好 |
| **Backend Bound** | 后端无法及时消耗 μop | 缓存未命中、执行端口不足 | 越低越好 |

### Level 2：八大子类

```
Frontend Bound
├── Fetch Latency      ← 取指延迟（I-cache miss, iTLB miss）
└── Fetch Bandwidth    ← 取指带宽不足（解码器瓶颈）

Bad Speculation
├── Branch Mispredict  ← 分支预测错误
└── Machine Clears     ← 流水线完全冲刷（内存排序违规等）

Backend Bound
├── Memory Bound       ← 内存子系统瓶颈（L1/L2/L3/DRAM miss）
└── Core Bound         ← 执行单元瓶颈（ALU 不足、指令延迟）

Retiring
├── Base               ← 常规退休
└── Microcode Sequencer← 微码辅助退休（复杂指令如 REP MOVSB）
```

### Level 3：细分子类（部分展示）

```
Fetch Latency
├── ICache Misses        ← 指令缓存未命中
├── ITLB Misses          ← 指令 TLB 未命中
├── Branch Resteers      ← 分支重定向后的重新取指
└── DSB Switches         ← 微操作缓存与遗留解码器切换

Memory Bound
├── L1 Bound             ← L1 数据缓存瓶颈
├── L2 Bound             ← L2 缓存瓶颈
├── L3 Bound             ← L3 缓存瓶颈
├── DRAM Bound           ← 主存带宽/延迟瓶颈
└── Store Bound          ← 存储缓冲区瓶颈

Core Bound
├── Divider              ← 除法单元瓶颈
├── Ports Utilization    ← 执行端口利用率不足
└── 3GE (3+ μop)        ← 长延迟依赖链
```

### Level 4：最细粒度（部分展示）

```
L1 Bound
├── 4K Aliasing          ← 4KB 地址别名冲突
├── Lock Latency         ← 锁操作延迟
├── Split Loads          ← 跨缓存行的加载
└── DTLB Load            ← 数据 TLB 加载未命中

L3 Bound
├── Contested Accesses   ← 多核竞争访问
├── Data Sharing         ← 伪共享（False Sharing）
├── SQ Full              ← Super Queue 满
└── Remote Cache         ← 远端缓存访问（NUMA）
```

---

## 3. 核心概念

### 3.1 Pipeline Slot（流水线槽位）

Pipeline Slot 是 TMAM 的基本度量单位。一个 Slot 代表"一个 μop 在一个时钟周期内占据的一个流水线位置"。

```
        Cycle 1    Cycle 2    Cycle 3    Cycle 4
Slot 0  [μop-A]   [μop-D]   [------]   [μop-H]
Slot 1  [μop-B]   [μop-E]   [------]   [μop-I]
Slot 2  [μop-C]   [------]   [------]   [μop-J]
Slot 3  [------]   [------]   [------]   [------]
         ▲ 3/4      ▲ 2/4      ▲ 0/4      ▲ 3/4
         Retiring   Retiring   Stalled    Retiring
```

**★ 关键：** Skylake 每周期最多退休 4 个 μop，因此每周期有 4 个 Slot。空白的 Slot 就是"浪费"。

### 3.2 μop（Micro-Operation）

μop 是 CPU 内部实际执行的操作单元。一条 x86 指令可能被拆分为多个 μop：

| x86 指令 | μop 数量 | 说明 |
|---------|---------|------|
| `ADD r64, r64` | 1 | 简单指令，1 个 μop |
| `ADD [mem], r64` | 2 | load + add（融合后） |
| `REP MOVSB` | 100+ | 复杂指令，微码展开 |
| `CPUID` | 50+ | 序列化指令，微码辅助 |

### 3.3 IPC（Instructions Per Cycle）

```
IPC = Instructions_Retired / CPU_CLK_UNHALTED.THREAD
```

| IPC 范围 | 评价 | TMAM 分析方向 |
|---------|------|-------------|
| > 2.0 | 优秀 | Retiring 占比高 |
| 1.0 - 2.0 | 良好 | 检查 Backend Bound |
| 0.5 - 1.0 | 一般 | 检查 Frontend/Backend |
| < 0.5 | 差 | 严重瓶颈，需深入分析 |

**★ IPC 与 TMAM 的关系：** IPC = Retiring% × Pipeline_Width。IPC = 2.0 意味着 50% 的 Slot 用于有效退休。

### 3.4 Stall（停顿）

当流水线中没有足够的 μop 可退休时，就发生了 Stall：

```
正常流水线：
  取指 → 解码 → 重命名 → 发射 → 执行 → 退休
  [F]    [D]     [R]      [A]    [E]    [R]    ← 每级都有 μop

前端停顿（Frontend Bound）：
  取指 → [等待 I-cache]
         ─────────────────────── 后端饥饿

后端停顿（Backend Bound）：
  取指 → 解码 → 重命名 → 发射 → [等待 L3 miss]
                                  ─────────── 执行延迟
```

---

## 4. 学习路线（8 步）

### Step 1：CPU 流水线基础

**目标：** 理解现代 CPU 流水线的基本模型

**文件位置：** `reading/01_pipeline_slots.md`

**阅读要点：**

```
1. 六阶段流水线：取指 → 解码 → 重命名 → 调度 → 执行 → 退休
2. 超标量架构：每周期可处理多个 μop
3. 乱序执行（Out-of-Order）：ROB（Reorder Buffer）的角色
4. Pipeline Slot 的数学定义
```

**思考题：**

1. 为什么流水线宽度是 4 而不是更大？
   > 答：功耗和面积的权衡。更宽的流水线需要更多的执行端口和更大的 ROB，面积和功耗呈非线性增长。
2. 如果一条指令需要 100 个周期（如除法），其他指令能继续执行吗？
   > 答：可以。乱序执行允许后续无依赖的指令先行执行，这就是为什么我们需要 ROBs 来追踪指令顺序。

**实操任务：** 运行 `perf stat -e cycles,instructions,cache-misses,branch-misses ./your_program` 获取基本指标。

---

### Step 2：perf stat --topdown 命令使用

**目标：** 掌握 TMAM 的 perf 工具链

**文件位置：** `reading/02_command_reference.md`

**阅读要点：**

```
1. perf stat --topdown --td-level N 的参数含义
2. Level 1-4 各级的输出格式
3. -M TopdownL1/L2/L3 的 metric group 方式
4. pmu-tools/toplev.py 的增强功能
```

**思考题：**

1. 为什么 --topdown 需要独占所有 PMU 计数器？
   > 答：TMAM 需要同时采集大量硬件事件（10-40 个），而 CPU 通常只有 4-8 个通用 PMU 计数器，需要多路复用（multiplexing）或使用固定计数器。

**实操任务：** 在支持 PMU 的物理机上运行 `perf stat --topdown --td-level 1 ./program`。

---

### Step 3：Level 1 四大类分析

**目标：** 理解四类瓶颈的含义和判定方法

**文件位置：** `reading/01_pipeline_slots.md`（四大类图解部分）

**阅读要点：**

```
1. Retiring = 有效工作的比例
2. Frontend Bound = "喂不饱"后端
3. Backend Bound = "消化不了"前端
4. Bad Speculation = "走弯路"的浪费
```

**思考题：**

1. 一个纯计算的程序（无内存访问），Frontend Bound 会高吗？
   > 答：通常不会。纯计算程序的指令序列紧凑、分支少，前端压力小。但如果代码量很大（代码段跨越多个缓存行），I-cache miss 可能导致 Frontend Bound 升高。

---

### Step 4：Level 2-3 细分子类

**目标：** 深入理解每类瓶颈的子分类

**文件位置：** `reading/02_command_reference.md`（事件组部分）

**阅读要点：**

```
1. Fetch Latency vs Fetch Bandwidth：延迟问题 vs 带宽问题
2. Branch Mispredict vs Machine Clears：分支错误 vs 完全冲刷
3. Memory Bound vs Core Bound：内存延迟 vs 执行单元不足
4. Base vs Microcode Sequencer：正常退休 vs 微码辅助
```

**思考题：**

1. 如何区分 Memory Bound 和 Core Bound？
   > 答：Memory Bound 的特征是高 LLC miss 率和高内存访问延迟；Core Bound 的特征是执行端口利用率低但内存访问正常。

---

### Step 5：实战分析——命令输出解读

**目标：** 学会阅读和解读 perf topdown 输出

**文件位置：** `reading/03_command_output_demo.md`

**阅读要点：**

```
1. 不同负载类型的 topdown 特征
2. 百分比含义和优化阈值
3. 从 Level 1 到 Level 2 的下钻路径
4. 常见瓶颈的优化策略
```

---

### Step 6：综合实验与优化实践

**目标：** 在真实程序上完成端到端的瓶颈分析

**实操任务：**

```bash
# 实验 1：对比 CPU 密集 vs 内存密集程序
perf stat --topdown --td-level 2 -- ./cpu_bound
perf stat --topdown --td-level 2 -- ./mem_bound

# 实验 2：对比优化前后的 topdown 变化
# 优化前：朴素矩阵乘法
# 优化后：分块矩阵乘法（cache-friendly）
gcc -O2 -o matmul_naive matmul_naive.c
gcc -O2 -o matmul_blocked matmul_blocked.c
perf stat --topdown --td-level 2 -- ./matmul_naive
perf stat --topdown --td-level 2 -- ./matmul_blocked
```

---

### Step 7：AMD 上的 Top-Down 分析

**目标：** 掌握在 AMD 平台上进行等效的 Top-Down 瓶颈分析

**文件位置：** `reading/04_amd_topdown.md`

**阅读要点：**

```
1. 为什么 AMD 没有 perf stat --topdown（Intel 特有 PMU 事件）
2. AMD 替代方案: stalled-cycles-frontend/backend 近似 Level 1
3. 用 perf stat -d 的 cache/branch 指标推断瓶颈类型
4. AMD IBS（Instruction Based Sampling）精确采样
5. AMD μProf 官方分析工具
```

**思考题：**

1. AMD 的 `stalled-cycles-backend` 和 Intel 的 `Backend Bound` 有何差异？
   > 答：Intel Backend Bound 用精确的 PMU 事件（UOPS_ISSUED.ANY）计算，区分 Memory Bound 和 Core Bound。AMD 的 stalled-cycles-backend 是一个笼统的"后端停顿周期"指标，不能直接区分 Memory vs Core Bound，需要结合 L3 miss 率来推断。

2. AMD IBS 比 Intel PEBS 多提供什么信息？
   > 答：IBS Op 模式提供每条采样指令的 DC miss latency（缓存缺失的实际延迟周期数），这在 Intel PEBS 中不是每条指令都有的。可以直接看到哪些内存访问导致了长时间等待。

**实操任务：** 在 AMD 服务器上运行 `perf stat -e stalled-cycles-frontend,stalled-cycles-backend,LLC-loads,LLC-load-misses -- ./program` 并进行瓶颈推断。

---

### Step 8：实战 — CPU 高占用低吞吐的 Top-Down 诊断

**目标：** 掌握端到端的 Top-Down 诊断流程（5 步法）

**文件位置：** `reading/05_high_cpu_low_throughput.md`

**阅读要点：**

```
1. Step 1: perf stat 看 IPC → IPC < 1.0 说明有大量 stall
2. Step 2: 看 Frontend Bound vs Backend Bound
3. Step 3: Backend Bound 看 Memory Bound vs Core Bound
4. Step 4: Memory Bound 看 L1/L2/L3 miss 率
5. Step 5: 结合 numactl 看跨 NUMA 访问比例
```

**思考题：**

1. 如果 IPC = 0.3 但 L3 miss 率只有 5%，瓶颈最可能在哪？
   > 答：Core Bound — 执行单元瓶颈或长依赖链。可能的原因包括：大量除法运算、串行依赖链（如链表遍历的 pointer chasing）、或自旋锁等待。

2. 在 AMD 上如果 Backend Bound 高，如何区分 Memory Bound 和 Core Bound？
   > 答：结合 L3 miss 率判断。如果 L3 miss 率 > 30%，说明大量 DRAM 访问 → Memory Bound。如果 L3 miss 率 < 10% 但 Backend stall 仍然高 → Core Bound。更精确的方法是使用 AMD IBS 查看每条指令的 DC miss latency。

**实操任务：** 对一个已知瓶颈的程序执行完整的 5 步诊断流程，记录每步的命令、指标和判断结论。

---

## 5. 工具链

### 5.1 perf stat（Linux 内核自带）

```bash
# Level 1 分析
perf stat --topdown --td-level 1 -- ./program

# Level 2 分析
perf stat --topdown --td-level 2 -- ./program

# 使用 metric group（兼容性更好）
perf stat -M TopdownL1 -- ./program
perf stat -M TopdownL2 -- ./program
```

### 5.2 pmu-tools / toplev.py

```bash
# 安装
pip3 install pmu-tools

# 运行（自动处理 multiplexing）
toplev.py --core C0 -l1 -- ./program
toplev.py --core C0 -l2 -- ./program

# 生成图表
toplev.py --core C0 -l2 --graph -- ./program
```

### 5.3 Intel VTune Profiler

```bash
# Microarchitecture Exploration 分析
vtune -collect microarchitectural-exploration -- ./program

# 提供 Level 1-5 的完整 topdown 树
# 图形化展示瓶颈路径（红色高亮）
```

---

## 6. 实战实验清单

### 实验 1：CPU 密集型 vs 内存密集型对比

**目标：** 观察两种极端负载的 topdown 差异

```bash
# CPU 密集型：矩阵乘法
cat > /tmp/matmul.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
int main() {
    int n = 400;
    double *A = malloc(n*n*sizeof(double));
    double *B = malloc(n*n*sizeof(double));
    double *C = calloc(n*n, sizeof(double));
    for(int i=0;i<n*n;i++) { A[i]=1.0; B[i]=1.0; }
    for(int i=0;i<n;i++)
        for(int k=0;k<n;k++)
            for(int j=0;j<n;j++)
                C[i*n+j] += A[i*n+k]*B[k*n+j];
    printf("C[0]=%f\n", C[0]);
    free(A); free(B); free(C);
    return 0;
}
EOF
gcc -O2 -o /tmp/matmul /tmp/matmul.c

# 内存密集型：链表遍历（pointer chasing）
cat > /tmp/chase.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#define N (1<<20)
int main() {
    int *next = malloc(N*sizeof(int));
    int *idx = malloc(N*sizeof(int));
    for(int i=0;i<N;i++) idx[i]=i;
    for(int i=N-1;i>0;i--) {
        int j=rand()%(i+1);
        int t=idx[i]; idx[i]=idx[j]; idx[j]=t;
    }
    for(int i=0;i<N-1;i++) next[idx[i]]=idx[i+1];
    next[idx[N-1]]=idx[0];
    int p = idx[0];
    long sum = 0;
    for(long i=0;i<(long)N*50;i++) { p=next[p]; sum+=p; }
    printf("sum=%ld\n", sum);
    free(next); free(idx);
    return 0;
}
EOF
gcc -O2 -o /tmp/chase /tmp/chase.c

# 对比运行
perf stat --topdown --td-level 2 -- /tmp/matmul
perf stat --topdown --td-level 2 -- /tmp/chase
```

**预期结果：**

| 指标 | CPU 密集型（matmul） | 内存密集型（chase） |
|------|-------------------|-------------------|
| Retiring | ~65% | ~20% |
| Frontend Bound | ~5% | ~5% |
| Bad Speculation | ~5% | ~2% |
| Backend Bound | ~25% | ~73% |
| Backend → Memory Bound | 低 | 高 |
| Backend → Core Bound | 较高 | 低 |

### 实验 2：分支预测影响

```bash
# 可预测分支 vs 不可预测分支
cat > /tmp/sorted.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#define N (1<<24)
int cmp(const void *a, const void *b) { return *(int*)a - *(int*)b; }
int main() {
    int *data = malloc(N*sizeof(int));
    for(int i=0;i<N;i++) data[i]=rand();
    qsort(data, N, sizeof(int), cmp);  // 排序后分支更可预测
    long sum = 0;
    for(int i=0;i<N;i++) if(data[i] > 0x40000000) sum += data[i];
    printf("sum=%ld\n", sum);
    free(data);
    return 0;
}
EOF

cat > /tmp/unsorted.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#define N (1<<24)
int main() {
    int *data = malloc(N*sizeof(int));
    for(int i=0;i<N;i++) data[i]=rand();
    // 不排序：分支不可预测
    long sum = 0;
    for(int i=0;i<N;i++) if(data[i] > 0x40000000) sum += data[i];
    printf("sum=%ld\n", sum);
    free(data);
    return 0;
}
EOF

gcc -O2 -o /tmp/sorted /tmp/sorted.c
gcc -O2 -o /tmp/unsorted /tmp/unsorted.c

perf stat --topdown --td-level 2 -- /tmp/sorted
perf stat --topdown --td-level 2 -- /tmp/unsorted
```

**预期：** unsorted 版本的 Bad Speculation 显著高于 sorted 版本。

### 实验 3：编译优化对 topdown 的影响

```bash
gcc -O0 -o /tmp/matmul_O0 /tmp/matmul.c
gcc -O2 -o /tmp/matmul_O2 /tmp/matmul.c
gcc -O3 -o /tmp/matmul_O3 /tmp/matmul.c

perf stat --topdown --td-level 2 -- /tmp/matmul_O0
perf stat --topdown --td-level 2 -- /tmp/matmul_O2
perf stat --topdown --td-level 2 -- /tmp/matmul_O3
```

**预期：** -O0 版本 Retiring 最低（大量冗余指令），-O3 版本 Retiring 最高（向量化）。

---

## 7. 关键文件索引

| 文件 | 说明 | 阅读顺序 |
|------|------|---------|
| `reading/01_pipeline_slots.md` | CPU 流水线模型与 Pipeline Slot 概念 | ★ Step 1 |
| `reading/02_command_reference.md` | perf stat --topdown 参数详解 | ★ Step 2 |
| `reading/03_command_output_demo.md` | 真实输出实战解析 | ★ Step 3 |
| `reading/04_amd_topdown.md` | AMD 上的 Top-Down 替代分析方法 | ★ Step 7 |
| `reading/05_high_cpu_low_throughput.md` | CPU 高占用低吞吐的 5 步诊断法 | ★ Step 8 |
| `QA.md` | 问答集（持续追加） | 按需查阅 |

---

## 参考资源

- **原始论文：** Ahmad Yasin, "A Top-Down Method for Performance Analysis and Counters Architecture", ISPASS 2014
- **Intel 官方文档：** [Top-Down Microarchitecture Analysis Method](https://www.intel.com/content/www/us/en/docs/vtune-profiler/user-guide/2024-0/top-down-microarchitecture-analysis-method.html)
- **AMD μProf：** [AMD Microarchitecture Profiler](https://www.amd.com/en/developer/uprof.html)
- **AMD PPR：** [Processor Programming Reference](https://www.amd.com/en/search/documentation/hub.html) — AMD PMU 事件定义
- **pmu-tools：** [github.com/andikleen/pmu-tools](https://github.com/andikleen/pmu-tools)
- **Brendan Gregg：** [CPU Flame Graphs](https://www.brendangregg.com/FlameGraphs/cpuflamegraphs.html)
- **Brendan Gregg：** Systems Performance, 2nd Edition — Chapter 6 (CPUs)
- **Intel 64 and IA-32 Architectures Optimization Reference Manual** — Chapter 2 (Microarchitecture)
- **Agner Fog：** [Microarchitecture of AMD and Intel x86 CPUs](https://agner.org/optimize/microarchitecture.pdf)
- **perf 源码：** `src/linux-5.10/tools/perf/` — 包含 topdown metric 定义
