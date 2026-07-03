# CPU 架构与内核 CPU 子系统学习路线

> 从微架构流水线到内核调度器，从硬件计数器到性能分析工具

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. CPU 微架构概览](#2-cpu-微架构概览)
  - [2.1 指令流水线](#21-指令流水线)
  - [2.2 缓存层次](#22-缓存层次)
  - [2.3 乱序执行与分支预测](#23-乱序执行与分支预测)
  - [2.4 SIMD 与向量化](#24-simd-与向量化)
- [3. x86_64 架构关键概念](#3-x86_64-架构关键概念)
  - [3.1 特权级 Ring 0-3](#31-特权级-ring-0-3)
  - [3.2 MSR 与 CPUID](#32-msr-与-cpuid)
  - [3.3 中断与异常](#33-中断与异常)
- [4. 内核 CPU 子系统](#4-内核-cpu-子系统)
  - [4.1 调度器架构](#41-调度器架构)
  - [4.2 CFS 完全公平调度](#42-cfs-完全公平调度)
  - [4.3 cpufreq 频率调节](#43-cpufreq-频率调节)
  - [4.4 cpuidle 空闲状态](#44-cpuidle-空闲状态)
  - [4.5 NUMA 架构与调度](#45-numa-架构与调度)
- [5. 源码阅读路线图](#5-源码阅读路线图)
  - [Step 1: CPU 微架构基础](#step-1-cpu-微架构基础)
  - [Step 2: 内核调度器核心数据结构](#step-2-内核调度器核心数据结构)
  - [Step 3: CFS 调度算法](#step-3-cfs-调度算法)
  - [Step 4: CPU 拓扑与负载均衡](#step-4-cpu-拓扑与负载均衡)
  - [Step 5: cpufreq / cpuidle 子系统](#step-5-cpufreq--cpuidle-子系统)
  - [Step 6: CPU 相关命令与调优](#step-6-cpu-相关命令与调优)
- [6. 与性能工具的关系](#6-与性能工具的关系)
- [7. 动手实验清单](#7-动手实验清单)
- [8. 关键源码文件索引](#8-关键源码文件索引)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        用户态工具                                 │
│  perf stat/top   lscpu   taskset   numactl   cpupower   turbostat│
├─────────────────────────────────────────────────────────────────┤
│                        系统调用接口                               │
│  sched_setaffinity()  setpriority()  perf_event_open()           │
│  /proc/cpuinfo  /sys/devices/system/cpu/  /proc/schedstat        │
├─────────────────────────────────────────────────────────────────┤
│                      内核 CPU 子系统                              │
│  scheduler (CFS/RT/DL)  cpufreq  cpuidle  NUMA balancing         │
│  topology   load-balancing   PELT (Per-Entity Load Tracking)     │
├─────────────────────────────────────────────────────────────────┤
│                        硬件抽象层                                 │
│  ACPI   CPU microcode   MSR access   PMU (Performance Monitor)   │
├─────────────────────────────────────────────────────────────────┤
│                        CPU 硬件                                   │
│  流水线 → 乱序执行 → 缓存层次 → 分支预测 → SMT → PMU 计数器      │
└─────────────────────────────────────────────────────────────────┘
```

### 数据流：从硬件事件到性能分析

```
  CPU 硬件 PMU（Performance Monitoring Unit）
     │
     ▼
  硬件事件计数器（cycles, instructions, cache-miss, branch-miss ...）
     │
     ▼
  ┌── perf_event 子系统 ──────────────────────────────────┐
  │  perf_event_open() → 配置计数器                        │
  │  NMI 中断 → 采样 → ring buffer                         │
  └──────────────────────────────────────────────────────┘
     │
     ▼
  perf 用户态工具（perf stat / perf record / perf report）
     │
     ▼
  性能分析报告（热点函数、缓存命中率、IPC、分支预测准确率 ...）
```

---

## 2. CPU 微架构概览

### 2.1 指令流水线

现代 CPU 将指令执行分解为多个阶段，实现指令级并行：

```
                    x86_64 指令流水线（简化模型）

  ┌───────┐  ┌───────┐  ┌────────┐  ┌──────────┐  ┌─────────┐  ┌────────┐
  │ Fetch │→ │Decode │→ │ Rename │→ │ Schedule │→ │ Execute │→ │ Retire │
  │ 取指  │  │ 解码  │  │ 重命名 │  │ 调度/派遣│  │  执行   │  │  退休  │
  └───────┘  └───────┘  └────────┘  └──────────┘  └─────────┘  └────────┘
     │          │          │            │              │            │
     │          │          │            │              │            │
  L1 I-Cache  x86→μops  消除假依赖  从 Reservation  ALU/FPU/   按程序序
  16 字节/周期            (WAW/WAR)   Station 选    Load/Store   提交结果
                                    就绪的 μops    单元执行     到架构状态
```

| 阶段 | 英文 | 功能 | 典型延迟 |
|------|------|------|----------|
| 取指 | Fetch | 从 L1 I-Cache 读取指令字节 | 1 周期 |
| 解码 | Decode | x86 指令 → 微操作（μops） | 1-4 周期 |
| 重命名 | Rename | 将架构寄存器映射到物理寄存器，消除假依赖 | 1 周期 |
| 调度 | Schedule | 从 Reservation Station 中选择操作数就绪的 μops | 0-1 周期 |
| 执行 | Execute | 在功能单元（ALU/FPU/Load-Store）中执行 | 1-30+ 周期 |
| 退休 | Retire | 按程序顺序将结果提交到架构状态 | 1 周期 |

### 2.2 缓存层次

```
  ┌─────────────────────────────────────────────┐
  │                  CPU Core                     │
  │                                               │
  │  ┌──────────┐    ┌──────────┐                 │
  │  │ L1 I-Cache│    │ L1 D-Cache│    ← 32-64 KB │
  │  │  4 周期   │    │  4-5 周期│                  │
  │  └──────────┘    └──────────┘                 │
  │        │               │                       │
  │        ▼               ▼                       │
  │  ┌──────────────────────────┐                  │
  │  │     L2 Cache (统一)       │    ← 256KB-1MB  │
  │  │       10-14 周期          │                  │
  │  └──────────────────────────┘                  │
  │              │                                  │
  └──────────────┼──────────────────────────────────┘
                 │
                 ▼
  ┌──────────────────────────────────┐
  │      L3 Cache (LLC, 共享)        │    ← 数 MB - 数十 MB
  │          30-50 周期               │
  └──────────────────────────────────┘
                 │
                 ▼
  ┌──────────────────────────────────┐
  │          主存 (DRAM)              │    ← 100-300 周期
  └──────────────────────────────────┘
```

**TLB（Translation Lookaside Buffer）：**

| TLB 层级 | 典型条目数 | 命中延迟 | 缺失代价 |
|----------|-----------|---------|---------|
| L1 D-TLB | 64-72 条目 | 1 周期 | ~10 周期（查 L2 TLB） |
| L1 I-TLB | 128-144 条目 | 1 周期 | ~10 周期 |
| L2 TLB | 512-1536 条目 | ~10 周期 | ~100+ 周期（Page Walk） |

### 2.3 乱序执行与分支预测

**乱序执行（Out-of-Order Execution）：**

```
                  乱序执行引擎核心组件

  ┌───────────────┐     ┌─────────────────────┐
  │  μop 队列      │     │  ROB (Reorder Buffer) │  ← ★ 核心
  │  (Rename 输出) │────→│  按程序序跟踪 μops    │
  └───────────────┘     │  容量: ~200-500 条目   │
                         └────────┬──────────────┘
                                  │ 派遣
                                  ▼
                         ┌─────────────────────┐
                         │  Reservation Station │
                         │  (RS) 等待操作数就绪  │
                         │  容量: ~50-100 条目   │
                         └────────┬──────────────┘
                                  │ 执行
                                  ▼
                         ┌─────────────────────┐
                         │  功能单元 (FU)         │
                         │  ALU × 4, FPU, Load,  │
                         │  Store, Branch         │
                         └────────┬──────────────┘
                                  │ 完成
                                  ▼
                         ROB 按序退休 → 架构状态更新
```

**分支预测（Branch Prediction）：**

| 预测器类型 | 原理 | 准确率 |
|-----------|------|--------|
| Bimodal | 2-bit 饱和计数器（强不选/弱不选/弱选/强选） | ~90% |
| Two-Level | 全局历史 + 局部历史 → PHT 查表 | ~95% |
| TAGE | Tagged Geometric History Length，多长度历史表 | ~97% |
| BTB (Branch Target Buffer) | 缓存分支目标地址 | — |
| RAS (Return Address Stack) | 专门预测 ret 指令 | ~99% |

分支预测失败的代价：~15-20 周期流水线冲刷（pipeline flush）

### 2.4 SIMD 与向量化

| 指令集 | 向量宽度 | 典型用途 |
|--------|---------|---------|
| SSE/SSE2 | 128-bit (XMM) | 浮点运算、整数并行 |
| AVX/AVX2 | 256-bit (YMM) | 科学计算、图像处理 |
| AVX-512 | 512-bit (ZMM) | HPC、机器学习推理 |

---

## 3. x86_64 架构关键概念

### 3.1 特权级 Ring 0-3

```
  ┌─────────────────────────────────────┐
  │         Ring 0 (内核态)              │  ← 最高特权
  │  操作系统内核、设备驱动               │
  │  可执行所有指令（含特权指令）          │
  ├─────────────────────────────────────┤
  │         Ring 1-2 (极少使用)          │
  │  历史上用于 OS 服务层                │
  │  现代 OS 基本不使用                   │
  ├─────────────────────────────────────┤
  │         Ring 3 (用户态)              │  ← 最低特权
  │  应用程序                             │
  │  不可执行特权指令（如 hlt, in, out）  │
  └─────────────────────────────────────┘

  模式切换路径:
  Ring 3 → Ring 0: 系统调用 (syscall/sysenter) 或 中断/异常
  Ring 0 → Ring 3: sysret/sysexit 或 iret
```

### 3.2 MSR 与 CPUID

**MSR（Model-Specific Register）：** 用于配置和监控 CPU 行为的 64-bit 寄存器

| MSR 名称 | 地址 | 用途 |
|----------|------|------|
| IA32_PERF_STATUS | 0x198 | 当前频率/电压状态 |
| IA32_PERF_CTL | 0x199 | 频率控制请求 |
| IA32_THERM_STATUS | 0x1B1 | 温度状态 |
| IA32_MISC_ENABLE | 0x1A0 | 各种 CPU 功能开关 |
| IA32_PMC0 | 0xC1 | 硬件性能计数器 |
| IA32_FIXED_CTR0 | 0x309 | 固定功能计数器（instructions） |

**CPUID：** 查询 CPU 特性和参数的指令

```
  输入 EAX=0:  厂商 ID 字符串（GenuineIntel / AuthenticAMD）
  输入 EAX=1:  型号信息、特性标志（SSE/AVX/AES...）
  输入 EAX=4:  缓存参数（大小、关联度、行大小）
  输入 EAX=0xB: 拓扑信息（核心数、线程数）
```

### 3.3 中断与异常

| 类型 | 来源 | 示例 |
|------|------|------|
| 异常 (Exception) | CPU 内部，指令执行触发 | 除零(#DE)、页错误(#PF)、GP(#GP) |
| 中断 (Interrupt) | 外部设备 | 定时器、键盘、网卡 |
| 软中断 (Softirq) | 内核内部 | TIMER_SOFTIRQ、RCU_SOFTIRQ |

```
  中断处理流程:

  硬件中断
     │
     ▼
  CPU 完成当前指令
     │
     ▼
  IDT (Interrupt Descriptor Table) 查表
     │
     ▼
  保存上下文（寄存器、标志位）
     │
     ▼
  执行中断处理程序 (ISR)
     │
     ▼
  ┌─── 是否可延迟？ ─────────────────┐
  │  否 → 立即处理完毕，iret 返回      │
  │  是 → 标记 softirq，返回后处理     │
  └──────────────────────────────────┘
     │
     ▼
  ksoftirqd / do_softirq() 处理软中断
```

---

## 4. 内核 CPU 子系统

### 4.1 调度器架构

Linux 内核调度器采用模块化设计，每个调度类（scheduling class）独立实现：

```
  ┌─────────────────────────────────────────────────────┐
  │              schedule() — 核心调度函数                 │
  │                                                       │
  │  1. 获取 rq 锁                                        │
  │  2. 调用 pick_next_task() 选择下一个任务               │
  │  3. context_switch() 切换上下文                        │
  └───────────────────┬─────────────────────────────────┘
                      │
          pick_next_task() 按优先级遍历:
                      │
     ┌────────────────┼────────────────────────┐
     ▼                ▼                ▼       ▼
  ┌──────┐      ┌──────────┐     ┌──────┐  ┌───────┐
  │ DL   │      │   RT     │     │ CFS  │  │ Idle  │
  │ 截止 │      │ 实时     │     │ 公平 │  │ 空闲  │
  │ 期限 │      │ FIFO/RR  │     │ 调度 │  │ 循环  │
  └──────┘      └──────────┘     └──────┘  └───────┘
  优先级:  DL > RT > CFS > Idle
```

每个 CPU 有独立的运行队列（runqueue）：

```c
/* kernel/sched/core.c */
DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

/* kernel/sched/sched.h */
struct rq {
    raw_spinlock_t      lock;        /* 运行队列自旋锁 */
    unsigned int        nr_running;  /* 当前可运行任务数 */
    u64                 nr_switches; /* 上下文切换次数 */
    struct cfs_rq       cfs;         /* CFS 运行队列 */
    struct rt_rq        rt;          /* RT 运行队列 */
    struct dl_rq        dl;          /* DL 运行队列 */
    struct task_struct  *curr;       /* 当前正在运行的任务 */
    struct task_struct  *idle;       /* idle 线程 */
    struct task_struct  *stop;       /* stop_machine 线程 */
    struct sched_domain *sd;         /* 调度域（拓扑） */
    unsigned long       cpu_capacity;/* CPU 容量 */
    int                 cpu;         /* 此 rq 对应的 CPU 编号 */
    ...
};
```

### 4.2 CFS 完全公平调度

**★ 核心思想：** 每个任务获得等比例的 CPU 时间。使用虚拟运行时间（vruntime）跟踪每个任务的"公平份额"。

```
  CFS 调度核心流程:

  任务 A (weight=1024, nice=0)     任务 B (weight=512, nice=5)
       │                                │
       ▼                                ▼
  vruntime += delta_exec * 1024/weight   vruntime += delta_exec * 1024/weight
  = delta_exec * 1.0                    = delta_exec * 2.0
       │                                │
       │  vruntime 增长慢               │  vruntime 增长快
       │  (获得较多 CPU)                │  (获得较少 CPU)
       ▼                                ▼
  ┌─────────────────────────────────────────────────┐
  │  红黑树 (tasks_timeline)                          │
  │                                                   │
  │  key = vruntime                                   │
  │                                                   │
  │       A(vr=5.2)                                   │
  │      / \                                           │
  │    B(7.1)  C(8.3)     ← 最左节点 vruntime 最小    │
  │                           → pick_next_entity()    │
  │                              选择最左节点           │
  └─────────────────────────────────────────────────┘
```

**关键调度参数（fair.c）：**

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `sysctl_sched_latency` | 6ms × (1 + ilog(ncpus)) | 调度周期：所有任务至少运行一次的时间窗口 |
| `sysctl_sched_min_granularity` | 0.75ms × (1 + ilog(ncpus)) | 最小抢占粒度：任务至少运行的时间 |
| `sysctl_sched_wakeup_granularity` | 1ms × (1 + ilog(ncpus)) | 唤醒抢占粒度：新唤醒任务需领先多少才能抢占 |
| `sched_nr_latency` | 8 | 调度周期内可容纳的任务数阈值 |

### 4.3 cpufreq 频率调节

```
  ┌──────────────────────────────────────────┐
  │            cpufreq 框架                    │
  │                                             │
  │  Governor（策略层）                          │
  │  ┌─────────┐ ┌──────────┐ ┌────────────┐  │
  │  │performance│ │powersave │ │schedutil   │  │
  │  │  最高频率  │ │ 最低频率 │ │ 基于调度器 │  │
  │  └─────────┘ └──────────┘ │ 负载调频   │  │
  │  ┌──────────┐ ┌──────────┐└────────────┘  │
  │  │ondemand  │ │conservative│               │
  │  │ 按需调频 │ │ 保守调频   │               │
  │  └──────────┘ └──────────┘               │
  │                                             │
  │  Driver（硬件驱动层）                        │
  │  ┌────────────┐ ┌────────┐ ┌────────────┐ │
  │  │acpi-cpufreq│ │intel_  │ │cppc_       │ │
  │  │            │ │pstate  │ │cpufreq     │ │
  │  └────────────┘ └────────┘ └────────────┘ │
  └──────────────────────────────────────────┘
```

| Governor | 策略 | 适用场景 |
|----------|------|---------|
| performance | 始终最高频率 | 延迟敏感型负载 |
| powersave | 始终最低频率 | 节能优先 |
| ondemand | 根据 CPU 利用率动态调频 | 通用工作负载 |
| conservative | 类似 ondemand 但更平滑 | 电池供电设备 |
| schedutil | ★ 基于调度器 PELT 数据 | 现代内核推荐 |

### 4.4 cpuidle 空闲状态

| C-State | 名称 | 退出延迟 | 功耗节省 |
|---------|------|---------|---------|
| C0 | 活跃 | — | — |
| C1 (HLT) | 暂停 | ~1μs | 低 |
| C1E | 增强暂停 | ~1μs | 中 |
| C6 | 断电核心 | ~50-100μs | 高 |
| C7/C8 | 深度断电 | ~100-500μs | 极高 |

```
  cpuidle 决策流程:

  CPU 空闲
     │
     ▼
  cpuidle_select() — 选择最优 C-State
     │
     ├── 考虑因素:
     │   - 预测空闲时长
     │   - 各 C-State 的退出延迟
     │   - QoS 延迟约束 (latency constraint)
     │   - 功耗节省
     │
     ▼
  进入选定的 C-State (cpuidle_enter())
     │
     ▼
  中断唤醒 → 退出 C-State → 恢复执行
```

### 4.5 NUMA 架构与调度

```
  NUMA (Non-Uniform Memory Access) 拓扑:

  ┌─────────────────────┐    ┌─────────────────────┐
  │    NUMA Node 0       │    │    NUMA Node 1       │
  │                       │    │                       │
  │  ┌────┐ ┌────┐       │    │       ┌────┐ ┌────┐ │
  │  │CPU0│ │CPU1│       │    │       │CPU2│ │CPU3│ │
  │  └────┘ └────┘       │    │       └────┘ └────┘ │
  │       │               │    │        │              │
  │  ┌────┴────┐         │    │   ┌────┴────┐        │
  │  │ L3 Cache │         │    │   │ L3 Cache │        │
  │  │ (共享)   │         │    │   │ (共享)   │        │
  │  └────┬────┘         │    │   └────┬────┘        │
  │       │               │    │        │              │
  │  ┌────┴────┐         │    │   ┌────┴────┐        │
  │  │ 本地内存 │ ←── QPI/UPI ──→ │ 本地内存 │        │
  │  │ (低延迟) │         │    │   │ (低延迟) │        │
  │  └─────────┘         │    │   └─────────┘        │
  └─────────────────────┘    └─────────────────────┘

  远程内存访问延迟 ≈ 本地延迟 × 1.5 ~ 2.0
```

**NUMA 感知调度策略：**
- 尽量将任务调度到其内存分配所在的 NUMA 节点
- `numa_balancing`：内核自动将页面迁移到任务所在节点
- 调度域层次：SMT → MC (Multi-Core) → NUMA → DIE

---

## 5. 源码阅读路线图

### Step 1: CPU 微架构基础

**目标：** 理解 CPU 流水线、缓存、乱序执行的硬件基础

**文件位置：**
- `reading/01_cpu_microarchitecture.md`

**阅读要点：**
- 流水线 6 阶段：Fetch → Decode → Rename → Schedule → Execute → Retire
- 缓存层次 L1/L2/L3/TLB 的容量与延迟
- ROB（Reorder Buffer）如何实现乱序执行 + 有序退休
- SMT（超线程）的物理核心资源共享模型
- 内存序模型（Memory Ordering）与屏障指令

### Step 2: 内核调度器核心数据结构

**目标：** 掌握调度器的关键数据结构及其关系

**文件位置：**
- `reading/02_kernel_cpu_scheduler.md`
- 内核源码：`src/linux-5.10/include/linux/sched.h`（task_struct、sched_entity）
- 内核源码：`src/linux-5.10/kernel/sched/sched.h`（rq、cfs_rq）

**阅读要点：**
- `struct task_struct`：进程描述符，包含调度相关的所有字段
- `struct sched_entity`：CFS 调度实体，包含 vruntime、红黑树节点
- `struct rq`：每 CPU 运行队列
- `struct cfs_rq`：CFS 运行队列，包含红黑树和 min_vruntime
- `struct sched_domain`：调度域，描述 CPU 拓扑

### Step 3: CFS 调度算法

**目标：** 理解 CFS 的核心调度逻辑

**文件位置：**
- `reading/02_kernel_cpu_scheduler.md`（CFS 部分）
- 内核源码：`src/linux-5.10/kernel/sched/fair.c`

**阅读要点：**
- `update_curr()`：更新当前任务的 vruntime
- `__enqueue_entity()` / `__dequeue_entity()`：红黑树插入/删除
- `pick_next_entity()`：选择下一个运行的任务
- `place_entity()`：新任务/唤醒任务的 vruntime 初始值计算
- `__schedule()` → `pick_next_task()` → `context_switch()`：调度主路径

### Step 4: CPU 拓扑与负载均衡

**目标：** 理解调度域层次和负载均衡机制

**文件位置：**
- 内核源码：`src/linux-5.10/kernel/sched/topology.c`
- 内核源码：`src/linux-5.10/include/linux/sched/topology.h`

**阅读要点：**
- `struct sched_domain`：调度域定义
- 调度域层次：SMT → MC → DIE → NUMA
- 负载均衡触发时机：newidle、periodic、wake
- PELT（Per-Entity Load Tracking）负载追踪

### Step 5: cpufreq / cpuidle 子系统

**目标：** 理解频率调节和空闲状态管理的框架

**文件位置：**
- 内核源码：`src/linux-5.10/drivers/cpufreq/cpufreq.c`
- 内核源码：`src/linux-5.10/drivers/cpuidle/cpuidle.c`
- 内核源码：`src/linux-5.10/kernel/sched/cpufreq_schedutil.c`

**阅读要点：**
- cpufreq governor 注册与调频决策
- schedutil：基于 PELT 利用率数据的调频
- cpuidle governor：menu / ladder 策略
- C-State 选择算法

### Step 6: CPU 相关命令与调优

**目标：** 掌握实际工作中的 CPU 信息查看和调优命令

**文件位置：**
- `reading/03_command_reference.md`
- `reading/04_command_output_demo.md`

**阅读要点：**
- lscpu / numactl --hardware / lstopo 查看 CPU 拓扑
- taskset / numactl 绑定 CPU 和 NUMA 节点
- cpupower 调整频率策略
- perf stat 收集硬件计数器数据
- /proc/cpuinfo / /proc/schedstat 解读

---

## 6. 与性能工具的关系

### perf 硬件计数器

```
  ┌──────────────────────────────────────────────────┐
  │              PMU (Performance Monitor Unit)         │
  │                                                      │
  │  可编程计数器 (GP Counter):                          │
  │  - IA32_PMC0..PMC3 (地址 0xC1-0xC4)                │
  │  - 通过 IA32_PERFEVTSEL 配置监控事件                │
  │                                                      │
  │  固定功能计数器 (Fixed Counter):                     │
  │  - IA32_FIXED_CTR0: Instructions Retired            │
  │  - IA32_FIXED_CTR1: CPU Cycles ( unhalted)          │
  │  - IA32_FIXED_CTR2: Reference Cycles                │
  └──────────────────────┬───────────────────────────┘
                         │
                         ▼
  ┌──────────────────────────────────────────────────┐
  │              perf_event 子系统                      │
  │                                                      │
  │  perf_event_open()                                   │
  │  ├── type: PERF_TYPE_HARDWARE / PERF_TYPE_HW_CACHE  │
  │  ├── config: PERF_COUNT_HW_CPU_CYCLES 等            │
  │  ├── sample_period / sample_freq                     │
  │  └── read_format: PERF_FORMAT_TOTAL_TIME_ENABLED    │
  │                                                      │
  │  采样: NMI 中断 → perf_event_output() → ring buffer │
  │  计数: read(fd) 获取累计值                           │
  └──────────────────────┬───────────────────────────┘
                         │
                         ▼
  perf stat / perf record / perf report / perf top
```

### Top-Down 微架构分析

```
  Intel Top-Down 方法将流水线槽位分为 4 大类:

  Pipeline Slots (每周期可用)
  │
  ├── Retiring (有用工作)
  │   ├── Base (普通指令退休)
  │   └── Microcode Sequencer (微码序列)
  │
  ├── Bad Speculation (分支预测错误浪费)
  │   ├── Branch Mispredict
  │   └── Machine Clears
  │
  ├── Frontend Bound (前端瓶颈)
  │   ├── Fetch Latency (I-Cache miss, iTLB miss)
  │   └── Fetch Bandwidth (解码带宽不足)
  │
  └── Backend Bound (后端瓶颈)
      ├── Core Bound (执行单元不足/延迟)
      └── Memory Bound (内存带宽/延迟)
```

### CPU 迁移监控

```
  关注指标:
  ├── /proc/schedstat → 每个 CPU 的调度统计
  │   ├── sched_count: schedule() 调用次数
  │   ├── ttwu_count: try_to_wake_up() 次数
  │   └── ttwu_local: 本地唤醒次数（vs 远程唤醒）
  │
  ├── /proc/interrupts → 中断分布
  │   └── 观察 IPI (Inter-Processor Interrupt) 频率
  │
  └── perf sched → 调度事件分析
      ├── perf sched record → 记录调度事件
      └── perf sched map → 可视化任务在 CPU 间的迁移
```

---

## 7. 动手实验清单

### 实验 1：CPU 拓扑探测

```bash
# 查看 CPU 拓扑
lscpu
lscpu -e
cat /proc/cpuinfo | grep -E "processor|core id|physical id"
numactl --hardware

# 可视化拓扑（如果安装了 hwloc）
lstopo
lstopo-no-graphics
```

### 实验 2：CPU 亲和性验证

```bash
# 绑定进程到特定 CPU
taskset -c 0,1 stress --cpu 2 &
taskset -p $$

# 验证绑定效果
cat /proc/$!/stat | awk '{print $39}'  # 最近使用的 CPU

# NUMA 绑定
numactl --cpunodebind=0 --membind=0 stress --cpu 4 --vm 2
```

### 实验 3：perf 硬件计数器

```bash
# 基本计数器
perf stat -e cycles,instructions,cache-misses,branch-misses -- sleep 1

# Top-Down Level 1
perf stat --topdown -a -- sleep 5

# 缓存分析
perf stat -e L1-dcache-loads,L1-dcache-load-misses,\
  LLC-loads,LLC-load-misses -- dd if=/dev/zero bs=1M count=100
```

### 实验 4：调度器行为观察

```bash
# 查看调度统计
cat /proc/schedstat | head -20

# 查看每个任务的调度信息
cat /proc/$$/sched

# 观察上下文切换
vmstat 1 10

# 调整 nice 值观察 vruntime 变化
nice -n 19 stress --cpu 1 &
stress --cpu 1 &
cat /proc/*/sched | grep -E "se.vruntime|se.nr_migrations"
```

### 实验 5：cpufreq 调频观察

```bash
# 查看当前频率
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 临时切换 governor
cpupower frequency-set -g performance
cpupower frequency-set -g powersave

# 观察频率变化
watch -n 0.1 cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
```

### 实验 6：NUMA 性能差异

```bash
# 本地 NUMA 节点内存访问
numactl --cpunodebind=0 --membind=0 stream_benchmark

# 远程 NUMA 节点内存访问
numactl --cpunodebind=0 --membind=1 stream_benchmark

# 对比延迟差异
perf stat -e cache-misses,numa-interleave ...
```

---

## 8. 关键源码文件索引

| 要找什么 | 路径 |
|---------|------|
| `struct task_struct` | `src/linux-5.10/include/linux/sched.h:640` |
| `struct sched_entity` | `src/linux-5.10/include/linux/sched.h:451` |
| `struct rq` (运行队列) | `src/linux-5.10/kernel/sched/sched.h:895` |
| `struct cfs_rq` (CFS 队列) | `src/linux-5.10/kernel/sched/sched.h:519` |
| `struct sched_domain` | `src/linux-5.10/include/linux/sched/topology.h:79` |
| `__schedule()` 核心函数 | `src/linux-5.10/kernel/sched/core.c:4410` |
| `pick_next_task()` | `src/linux-5.10/kernel/sched/core.c` |
| CFS 调度参数 | `src/linux-5.10/kernel/sched/fair.c:38-82` |
| `update_curr()` vruntime 更新 | `src/linux-5.10/kernel/sched/fair.c:842` |
| `__enqueue_entity()` 红黑树插入 | `src/linux-5.10/kernel/sched/fair.c:575` |
| `pick_next_entity()` 选择下一任务 | `src/linux-5.10/kernel/sched/fair.c:4435` |
| `place_entity()` 初始 vruntime | `src/linux-5.10/kernel/sched/fair.c:4108` |
| `set_cpus_allowed_ptr()` | `src/linux-5.10/kernel/sched/core.c` |
| cpufreq 框架 | `src/linux-5.10/drivers/cpufreq/cpufreq.c` |
| schedutil governor | `src/linux-5.10/kernel/sched/cpufreq_schedutil.c` |
| cpuidle 框架 | `src/linux-5.10/drivers/cpuidle/cpuidle.c` |
| 调度域构建 | `src/linux-5.10/kernel/sched/topology.c` |
| PELT 负载追踪 | `src/linux-5.10/kernel/sched/pelt.c` |
| perf_event 子系统 | `src/linux-5.10/kernel/events/core.c` |
| CPU 拓扑 sysfs | `src/linux-5.10/drivers/base/topology.c` |
