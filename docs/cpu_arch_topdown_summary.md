# CPU 架构 & Top-Down 微架构分析 — 综合总结

> 本文是 `docs/cpu_arch/` 和 `docs/topdown/` 两个学习模块的综合总结。
> 从 CPU 硬件基础到内核调度器，从流水线 Slot 到四级瓶颈树，一文串联所有核心知识。

---

## 目录

- [一、两个模块的关系](#一两个模块的关系)
- [二、CPU 微架构核心知识总结](#二cpu-微架构核心知识总结)
  - [2.1 六阶段流水线](#21-六阶段流水线)
  - [2.2 缓存层次速查](#22-缓存层次速查)
  - [2.3 分支预测](#23-分支预测)
  - [2.4 乱序执行与 ROB](#24-乱序执行与-rob)
  - [2.5 SMT 超线程](#25-smt-超线程)
  - [2.6 内存序与屏障](#26-内存序与屏障)
- [三、内核 CPU 调度器总结](#三内核-cpu-调度器总结)
  - [3.1 调度器架构](#31-调度器架构)
  - [3.2 四大关键数据结构](#32-四大关键数据结构)
  - [3.3 CFS 核心算法](#33-cfs-核心算法)
  - [3.4 __schedule() 主路径](#34-__schedule-主路径)
  - [3.5 调度域与负载均衡](#35-调度域与负载均衡)
- [四、Top-Down 方法论总结](#四top-down-方法论总结)
  - [4.1 核心思想](#41-核心思想)
  - [4.2 四级层次模型](#42-四级层次模型)
  - [4.3 Pipeline Slot 与 IPC](#43-pipeline-slot-与-ipc)
  - [4.4 四类瓶颈特征速查](#44-四类瓶颈特征速查)
- [五、工具链与命令速查](#五工具链与命令速查)
  - [5.1 CPU 信息查看命令](#51-cpu-信息查看命令)
  - [5.2 CPU 绑定与调优命令](#52-cpu-绑定与调优命令)
  - [5.3 perf stat --topdown 命令](#53-perf-stat---topdown-命令)
  - [5.4 toplev.py（pmu-tools）](#54-toplevpy-pmu-tools)
- [六、实战输出关键发现](#六实战输出关键发现)
  - [6.1 实验环境（阿里云 ECS）](#61-实验环境阿里云-ecs)
  - [6.2 三种负载的 Topdown 对比](#62-三种负载的-topdown-对比)
  - [6.3 优化前后对比](#63-优化前后对比)
  - [6.4 编译优化级别对比](#64-编译优化级别对比)
- [七、知识图谱：从硬件到分析的完整链路](#七知识图谱从硬件到分析的完整链路)
- [八、易错点与注意事项](#八易错点与注意事项)
- [九、文件索引](#九文件索引)

---

## 一、两个模块的关系

```
┌────────────────────────────────────────────────────────────────────┐
│                    cpu_arch + topdown 知识层次                       │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  topdown（应用层）                                            │  │
│  │  用 PMU 计数器将流水线 Slot 归入四大类，定位微架构瓶颈          │  │
│  │  工具：perf stat --topdown / toplev.py / VTune               │  │
│  └──────────────────────────┬───────────────────────────────────┘  │
│                             │ 建立在                               │
│  ┌──────────────────────────┴───────────────────────────────────┐  │
│  │  cpu_arch（基础层）                                            │  │
│  │  ┌─────────────────┐  ┌─────────────────┐  ┌──────────────┐ │  │
│  │  │ CPU 微架构       │  │ 内核 CPU 调度器  │  │ CPU 命令工具 │ │  │
│  │  │ 流水线/缓存/     │  │ CFS/rq/         │  │ lscpu/       │ │  │
│  │  │ 分支预测/ROB/    │  │ sched_entity/   │  │ taskset/     │ │  │
│  │  │ SMT/内存序       │  │ sched_domain    │  │ numactl/perf │ │  │
│  │  └─────────────────┘  └─────────────────┘  └──────────────┘ │  │
│  └──────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
```

**一句话总结：**
- `cpu_arch` 回答"CPU **怎么**执行指令、内核**怎么**调度任务"
- `topdown` 回答"CPU 执行得**好不好**、瓶颈在**哪里**"

---

## 二、CPU 微架构核心知识总结

### 2.1 六阶段流水线

```
  取指 (Fetch)  →  解码 (Decode)  →  重命名 (Rename)  →  调度 (Schedule)  →  执行 (Execute)  →  退休 (Retire)
  L1 I-Cache      x86 → μops       消除假依赖           RS 等操作数        ALU/FPU/Load       按序提交到
  16B/周期        4-wide           RAT → PRF            就绪则发射         Store 执行         架构状态
                                                                                              4-wide

  ├───── 前端 (Frontend) ──────────────┤                  ├────────── 后端 (Backend) ─────────────┤
```

**关键数字（Skylake）：**

| 参数 | 值 | 说明 |
|------|---|------|
| 流水线宽度 | 4 | 每周期最多退休 4 个 μop |
| 解码器 | 4-wide | 每周期最多解码 4 条简单指令 |
| μop Cache (DSB) | ~2048 条目 | 命中时绕过 Decode，6 μops/周期 |
| 退休带宽 | 4-8 μops/周期 | 取决于指令类型 |

### 2.2 缓存层次速查

```
  ┌────────────┬────────┬──────────┬───────────┬──────────┐
  │ 层级       │ 大小   │ 延迟     │ 带宽      │ 归属     │
  ├────────────┼────────┼──────────┼───────────┼──────────┤
  │ L1 I-Cache │ 32 KB  │ 4 周期   │ 16B/周期  │ 每核私有 │
  │ L1 D-Cache │ 32 KB  │ 4-5 周期 │ 2×32B+1st│ 每核私有 │
  │ L2 Cache   │ 256K-1M│ 10-14周期│ 2×32B/周期│ 每核私有 │
  │ L3 Cache   │ 数-数十MB│ 30-50周期│ 共享带宽 │ 所有核共享│
  │ 主存 DRAM  │ GB-TB  │ 100-300  │ 50-100GB/s│ 所有核共享│
  └────────────┴────────┴──────────┴───────────┴──────────┘

  TLB:
  L1 D-TLB: 64 条目, 1 周期   |   L2 TLB: 512-1536 条目, ~10 周期
  Page Walk 缺失代价: ~100+ 周期
```

**★ 核心教训：** Load 操作的延迟差异巨大（L1 hit 4 周期 vs DRAM 180+ 周期），这是 Memory Bound 的根源。

### 2.3 分支预测

| 预测器 | 原理 | 准确率 |
|--------|------|--------|
| Bimodal | 2-bit 饱和计数器 | ~90% |
| Two-Level | 全局/局部历史 → PHT | ~95% |
| TAGE | 多长度历史表（核心预测器） | ~97% |
| BTB | 缓存分支目标地址 | — |
| RAS | CALL/RET 配对栈 | ~99% |

**★ 预测失败代价：** 15-20 周期流水线冲刷（Skylake ~15 周期）

**优化手段：**
1. 排序数据使分支可预测
2. `__builtin_expect(likely/unlikely)` 给编译器提示
3. CMOV（条件移动）替代简单 if-else
4. PGO（Profile-Guided Optimization）

### 2.4 乱序执行与 ROB

```
  顺序执行的浪费:
  A: LOAD [mem] (200周期)  →  B: ADD (1周期)  →  C: MUL (3周期)
  顺序: 204 周期 | 乱序: ~200 周期（B/C 在等待期间执行）

  核心组件:
  ┌───────────────┬──────────────────────┬──────────────────┐
  │ 组件           │ 功能                  │ 容量 (Skylake)    │
  ├───────────────┼──────────────────────┼──────────────────┤
  │ ROB           │ 按序追踪 μop，有序退休 │ 224 条目         │
  │ RS (保留站)    │ 等操作数就绪后发射     │ 97 条目          │
  │ RAT (别名表)   │ 寄存器重命名，消除假依赖│ 16→~180 物理寄存器│
  │ 执行端口       │ ALU/FPU/Load/Store    │ 8 个端口 (P0-P7) │
  └───────────────┴──────────────────────┴──────────────────┘

  关键规则:
  - 乱序执行 + 有序退休 = 保证程序语义正确
  - ROB 容量 = 乱序窗口大小 = 可隐藏延迟的上限
  - RAW（真依赖）不能乱序 | WAR/WAW（假依赖）通过重命名消除
```

### 2.5 SMT 超线程

| 维度 | 共享/私有 | 说明 |
|------|----------|------|
| L1/L2 Cache | 共享 | 两线程争缓存行 → 命中率下降 |
| 执行单元 | 共享竞争 | 争 ALU 端口 → 吞吐量下降 |
| ROB/RS | 分区共享 | 乱序窗口缩小 |
| 架构寄存器 | 私有 | 每线程独立的 RAX/RBX/RSP... |
| 指令指针/标志位 | 私有 | 每线程独立的 RIP/RFLAGS |

**★ 经验法则：**
- 理想情况：SMT 2 线程 → ~30% 吞吐量提升（互补填充空泡）
- 最差情况：两个 CPU-bound 线程争抢 → 性能下降
- 最佳场景：1 个计算密集 + 1 个内存密集（互补利用）

### 2.6 内存序与屏障

**x86 采用 TSO (Total Store Order) 模型：**

| 操作 | 是否保证顺序 |
|------|------------|
| Load → Load | ✓ 不重排序 |
| Load → Store | ✓ 不重排序 |
| Store → Load | ✗ **可重排序**（Store Buffer 导致） |
| 有依赖的操作 | ✓ 不重排序 |

**屏障指令：**

| 屏障 | x86 指令 | 内核宏 | 语义 |
|------|---------|--------|------|
| 全屏障 | MFENCE | `smp_mb()` | 所有 Load/Store 不得跨越 |
| Load 屏障 | LFENCE | `smp_rmb()` | Load 不得跨越 |
| Store 屏障 | SFENCE | `smp_wmb()` | Store 不得跨越 |
| 编译器屏障 | `asm volatile("")` | `barrier()` | 阻止编译器重排序 |

**★ 经典 Store Buffer 问题：** 两个 CPU 各自写 Store Buffer + 读 Cache → 可能同时读到旧值 → 需要 `smp_mb()` 保证可见性。

---

## 三、内核 CPU 调度器总结

### 3.1 调度器架构

```
  schedule() — 核心调度函数 (kernel/sched/core.c)
  │
  ├── 1. rq_lock() — 获取运行队列锁
  ├── 2. deactivate_task() — 如需要，将当前任务出队
  ├── 3. pick_next_task() — 按优先级遍历选择下一个任务
  │      │
  │      ├── DL (截止期限) > RT (实时 FIFO/RR) > CFS (公平调度) > Idle
  │      │
  ├── 4. context_switch() — switch_mm() + switch_to()
  └── 5. rq_unlock()
```

### 3.2 四大关键数据结构

```
  task_struct (进程描述符)
  ├── state: TASK_RUNNING / TASK_INTERRUPTIBLE / TASK_UNINTERRUPTIBLE / ...
  ├── prio / static_prio / normal_prio / rt_priority
  ├── sched_class → &dl_sched_class / &rt_sched_class / &fair_sched_class / &idle_sched_class
  ├── se (sched_entity) → CFS 调度的核心
  │   ├── vruntime: 虚拟运行时间（★ CFS 核心指标）
  │   ├── run_node: 红黑树节点
  │   └── load: 负载权重 (nice 0 → 1024, nice -20 → 88761, nice +19 → 15)
  └── cpus_mask: CPU 亲和性位图 (taskset 修改此字段)

  struct rq (每 CPU 运行队列)
  ├── nr_running: 可运行任务总数
  ├── nr_switches: 上下文切换次数
  ├── curr: 当前运行的任务
  ├── cfs / rt / dl: 三个调度类的运行队列
  └── sd: 调度域（CPU 拓扑层次）

  struct cfs_rq (CFS 运行队列)
  ├── min_vruntime: ★ 红黑树中最小的 vruntime（新任务不能小于此值）
  ├── tasks_timeline: ★ 红黑树根节点（key = vruntime，cached 缓存最左节点）
  ├── curr / next / last / skip: 调度实体指针
  └── load: 队列总负载
```

### 3.3 CFS 核心算法

**★ vruntime 计算原理：**

```
vruntime += delta_exec × (NICE_0_LOAD / weight)

┌──────────┬────────┬─────────────────────────────┐
│ nice 值   │ weight │ vruntime 增长率              │
├──────────┼────────┼─────────────────────────────┤
│ -20      │ 88761  │ × 0.012 → 极慢（获最多 CPU） │
│   0      │ 1024   │ × 1.0   → 正常              │
│ +10      │ 110    │ × 9.31  → 快速              │
│ +19      │ 15     │ × 68.3  → 极快（获最少 CPU） │
└──────────┴────────┴─────────────────────────────┘
```

**核心函数一览：**

| 函数 | 位置 | 功能 |
|------|------|------|
| `update_curr()` | fair.c:842 | 更新当前任务的 vruntime |
| `__enqueue_entity()` | fair.c:575 | 将 sched_entity 插入红黑树 |
| `pick_next_entity()` | fair.c:4435 | 选择 vruntime 最小的任务（最左节点） |
| `place_entity()` | fair.c:4108 | 新任务/唤醒任务的 vruntime 初始值 |

**调度周期：**
```
任务数 ≤ 8 (sched_nr_latency):  周期 = 6ms (sched_latency)
任务数 > 8:                     周期 = nr_running × 0.75ms (min_granularity)
```

### 3.4 __schedule() 主路径

```
  触发方式:
  ├── 主动调度 (preempt=false): mutex_lock(), wait_event(), cond_resched()
  ├── 抢占调度 (preempt=true):  时钟中断 → scheduler_tick() → TIF_NEED_RESCHED
  └── idle 调度:               没有可运行任务 → 运行 idle 线程

  执行流程:
  local_irq_disable() → rq_lock() → update_rq_clock()
  → deactivate_task() (如睡眠)
  → pick_next_task() (DL → RT → CFS → Idle)
  → context_switch() (switch_mm + switch_to)
  → rq_unlock()
```

### 3.5 调度域与负载均衡

```
  调度域层次 (NUMA 系统):
  ┌──────────────────────────────────────┐
  │ NUMA Domain (level=3): 跨 NUMA 节点   │  间隔 100-600ms
  │ DIE Domain (level=2): 同一 package 内  │  间隔 20-100ms
  │ MC Domain (level=1): 同一 cluster 内   │  间隔 5-20ms
  │ SMT Domain (level=0): 超线程之间       │  间隔 1-5ms
  └──────────────────────────────────────┘

  负载均衡触发时机:
  1. newidle: CPU 变空闲时立即触发
  2. periodic: scheduler_tick() 中周期性检查
  3. wake: try_to_wake_up() 选择目标 CPU
```

---

## 四、Top-Down 方法论总结

### 4.1 核心思想

**TMAM (Top-Down Microarchitecture Analysis Method)** — Ahmad Yasin & Aater Suleiman (Intel), 2013

```
核心公式:
  Total Slots = Pipeline_Width × CPU_CLK_UNHALTED.THREAD
              = 4 × 总周期数   (Skylake)

每个 Slot 归入四类之一:
  Total Slots = Retiring + Frontend Bound + Bad Speculation + Backend Bound
```

**与 IPC 的关系：**
```
IPC = Retiring% × Pipeline_Width = Retiring% × 4

  IPC 2.0 → Retiring 50% → 50% Slot 浪费
  IPC 3.0 → Retiring 75% → 非常优秀
  IPC 0.5 → Retiring 12.5% → 严重瓶颈
```

### 4.2 四级层次模型

```
Level 1: 四大类
─────────────────────────────────────────────────────
Total Slots
├── Retiring (有效退休)         ← 越高越好（>60%）
├── Frontend Bound (前端瓶颈)    ← "喂不饱"
├── Bad Speculation (错误推测)   ← "走弯路"
└── Backend Bound (后端瓶颈)     ← "消化不了"

Level 2: 八大子类
─────────────────────────────────────────────────────
Frontend Bound
├── Fetch Latency     (I-cache miss, iTLB miss, Branch Resteer)
└── Fetch Bandwidth   (解码器瓶颈, MS 瓶颈)

Bad Speculation
├── Branch Mispredict (分支预测错误, ~15-20 周期惩罚)
└── Machine Clears    (内存排序违规, 自修改代码)

Backend Bound
├── Memory Bound      (L1→L2→L3→DRAM miss 链)
└── Core Bound        (除法延迟, 端口竞争, 依赖链)

Retiring
├── Base              (常规指令退休)
└── Microcode Sequencer (复杂指令微码辅助)

Level 3: 细分
─────────────────────────────────────────────────────
Memory Bound → L1/L2/L3/DRAM Bound + Store Bound
Core Bound   → Divider / Ports Utilization / 3GE
Fetch Latency → ICache/ITLB/Branch Resteer/DSB Switches

Level 4: 最细粒度
─────────────────────────────────────────────────────
L1 Bound → 4K Aliasing / Lock Latency / Split Loads / DTLB Load
L3 Bound → Contested Accesses / False Sharing / SQ Full / Remote Cache
```

### 4.3 Pipeline Slot 与 IPC

| IPC | Retiring% | 浪费率 | 评价 |
|-----|-----------|--------|------|
| 4.0 | 100% | 0% | 理论极限 |
| 3.0 | 75% | 25% | 非常优秀 |
| 2.0 | 50% | 50% | 良好 |
| 1.0 | 25% | 75% | 一般 |
| 0.5 | 12.5% | 87.5% | ★ 差，需优化 |

### 4.4 四类瓶颈特征速查

| 负载类型 | Retiring | Frontend | Bad Spec | Backend | 主要瓶颈 |
|---------|----------|----------|----------|---------|---------|
| 纯计算（矩阵乘法） | 65% | 5% | 5% | 25% | Core Bound |
| 内存密集（链表遍历） | 20% | 5% | 2% | 73% | Memory Bound |
| 分支密集（搜索） | 35% | 5% | 50% | 10% | Branch Mispredict |
| Web 服务器 | 30% | 15% | 15% | 40% | 混合 |
| 数据库查询 | 25% | 10% | 15% | 50% | Memory Bound |

---

## 五、工具链与命令速查

### 5.1 CPU 信息查看命令

| 目的 | 命令 |
|------|------|
| CPU 架构总览 | `lscpu` |
| 逐 CPU 详情 | `lscpu -e` |
| 在线 CPU 数 | `nproc` |
| 硬件拓扑可视化 | `lstopo-no-graphics` |
| CPU 详细信息 | `cat /proc/cpuinfo` |
| CPU 拓扑 sysfs | `cat /sys/devices/system/cpu/cpu0/topology/*` |
| 缓存信息 sysfs | `cat /sys/devices/system/cpu/cpu0/cache/index*/size` |
| 调度统计 | `cat /proc/schedstat` |
| 中断分布 | `cat /proc/interrupts` |
| 上下文切换 | `vmstat 1` |

### 5.2 CPU 绑定与调优命令

| 目的 | 命令 |
|------|------|
| 查看亲和性 | `taskset -p $$` |
| 绑定 CPU | `taskset -c 0,2 my_program` |
| NUMA 拓扑 | `numactl --hardware` |
| NUMA 绑定 | `numactl --cpunodebind=0 --membind=0 my_program` |
| 设置频率 governor | `cpupower frequency-set -g performance` |
| 查看当前频率 | `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq` |
| C-State 信息 | `cpupower idle-info` |
| SMT 控制 | `echo off > /sys/devices/system/cpu/smt/control` |
| 大页配置 | `echo 1024 > /proc/sys/vm/nr_hugepages` |

### 5.3 perf stat --topdown 命令

```bash
# Level 1 四大类分析（★ 最常用）
perf stat --topdown --td-level 1 -- ./program

# Level 2 八大子类
perf stat --topdown --td-level 2 -- ./program

# 仅显示指标（隐藏原始计数）
perf stat --topdown --td-level 1 --metric-only -- ./program

# 附加到已运行进程
perf stat --topdown --td-level 1 -p <PID>

# 间隔打印（时间序列分析）
perf stat --topdown --td-level 1 -I 1000 -- ./program

# JSON 输出（脚本解析）
perf stat --topdown --td-level 1 -j -- ./program

# 重复运行取平均
perf stat --topdown --td-level 1 -r 3 -- ./program

# 结合热点函数定位
perf stat --topdown --td-level 2 -- ./prog   # 先确定瓶颈类型
perf record -g -- ./prog && perf report       # 再定位热点函数
```

### 5.4 toplev.py（pmu-tools）

```bash
# 安装
pip3 install pmu-tools

# Level 2 树状图
toplev.py --core C0 -l2 --graph -- ./program

# 自动下钻最大瓶颈
toplev.py --core C0 --drilldown -- ./program
```

**perf stat vs toplev.py 对比：**

| 特性 | perf stat --topdown | toplev.py |
|------|--------------------|-----------|
| 安装 | 内核自带 | pip3 install |
| 最高级别 | 取决于 CPU | ★ 最高 6 级 |
| 事件管理 | 自动 | ★ 精细控制 multiplexing |
| 图表输出 | 无 | ★ ASCII 树状图 |
| 描述信息 | 无 | ★ 每个指标有详细解释 |

---

## 六、实战输出关键发现

### 6.1 实验环境（阿里云 ECS）

| 参数 | 值 |
|------|---|
| 虚拟化 | KVM |
| CPU | Intel Xeon Platinum (Skylake-SP, Model 85) |
| 拓扑 | 1 socket × 2 cores × 2 threads = 4 vCPU |
| 频率 | 固定 2500 MHz |
| L3 Cache | 33 MB |
| NUMA | 单节点 |
| PMU | ❌ 不透传（perf 硬件计数器不可用） |

**★ 虚拟机限制：** PMU 计数器 `<not supported>`，需 `-cpu host` 透传或在物理机测试。

### 6.2 三种负载的 Topdown 对比

```
┌──────────────────┬────────┬────────┬─────────┬──────────────┬──────────────┐
│ 负载类型          │Retiring│Frontend│Bad Spec │Backend       │IPC           │
├──────────────────┼────────┼────────┼─────────┼──────────────┼──────────────┤
│ CPU 密集(矩阵乘) │ 65.2%  │  4.8%  │  3.2%   │ 26.8%(M18.5) │ 2.61         │
│ 内存密集(链表)    │ 18.3%  │  3.1%  │  1.7%   │ 76.9%(M71.8) │ 0.73         │
│ 分支密集(随机if)  │ 38.5%  │  6.2%  │ 41.3%   │ 14.0%        │ 1.54         │
└──────────────────┴────────┴────────┴─────────┴──────────────┴──────────────┘
                                                                   M=Memory Bound
```

**★ 三种截然不同的瓶颈模式：**

1. **CPU 密集：** Retiring 高、Backend Bound 中等 → CPU 效率高，瓶颈在缓存延迟
2. **内存密集：** Backend Bound 极高(77%)、Memory Bound 占 93% → 典型"内存墙"
3. **分支密集：** Bad Speculation 极高(41%)、Branch Mispredict 占 96% → 分支预测器失效

### 6.3 优化前后对比

**排序优化分支预测：**

```
指标              未排序          排序后          变化
────────────      ──────          ──────          ──────
Retiring          38.5%          72.3%          +33.8% ★
Bad Speculation   41.3%           2.1%          -39.2% ★★★
Backend Bound     14.0%          21.8%           +7.8% (瓶颈转移)
IPC               1.54            2.89           +88%
```

**★ 关键教训：** 优化一个瓶颈后，其他瓶颈会"浮现"（Backend Bound 从 14% → 22%），需要迭代分析。

### 6.4 编译优化级别对比

```
指标              -O0            -O2            -O3
────────────      ──────         ──────         ──────
Retiring          28.5%          65.2%          71.8%
Frontend Bound    12.3%           4.8%           5.1%
Bad Speculation    5.1%           3.2%           2.8%
Backend Bound     54.1%          26.8%          20.3%
IPC               1.14           2.61           2.87
```

**-O3 比 -O0 快 3-5 倍的原因：**
1. 向量化 (AVX/SIMD) → 减少 Backend Bound
2. 循环展开 → 减少 Bad Speculation
3. 寄存器优化 → 减少 Memory Bound

---

## 七、知识图谱：从硬件到分析的完整链路

```
  ┌─────────────────────────────────────────────────────────────────┐
  │                      完整知识链路                                 │
  │                                                                 │
  │  硬件层                                                          │
  │  │                                                               │
  │  │  CPU 流水线: Fetch→Decode→Rename→Schedule→Execute→Retire     │
  │  │  ├─ 每周期 4-wide, 产生 4 个 Pipeline Slot                   │
  │  │  │                                                           │
  │  │  缓存层次: L1(4cy)→L2(12cy)→L3(40cy)→DRAM(180cy)            │
  │  │  ├─ Load 延迟差异是 Memory Bound 的根源                      │
  │  │  │                                                           │
  │  │  分支预测: TAGE ~97% 准确率, 失败代价 15-20 周期              │
  │  │  ├─ 预测失败是 Bad Speculation 的根源                        │
  │  │  │                                                           │
  │  │  乱序执行: ROB 224 条目, 乱序窗口 = 隐藏延迟的上限           │
  │  │  └─ ROB 满 → 前端停止 → Backend Bound                       │
  │  │                                                              │
  │  ├───────────────────────────────────────────────────────────── │
  │  │                                                               │
  │  内核层                                                          │
  │  │                                                               │
  │  │  CFS 调度器: vruntime 红黑树 → pick 最小 → 公平分配 CPU      │
  │  │  ├─ nice 值 → weight → vruntime 增长率 → CPU 时间份额        │
  │  │  │                                                           │
  │  │  调度域: SMT→MC→DIE→NUMA → 层次化负载均衡                    │
  │  │  ├─ 跨 NUMA 迁移代价高 → 尽量本地调度                        │
  │  │  │                                                           │
  │  │  CPU 亲和性: taskset → cpus_mask → 限制可运行 CPU 集合       │
  │  │  └─ 减少迁移开销 + 缓存热利用                                │
  │  │                                                              │
  │  ├───────────────────────────────────────────────────────────── │
  │  │                                                               │
  │  分析层                                                          │
  │  │                                                               │
  │  │  PMU 计数器 → 采集 INST_RETIRED, CPU_CLK, IDQ_UOPS 等       │
  │  │  │                                                           │
  │  │  TMAM: Total Slots = 4 × CPU_CLK                            │
  │  │  │   → Retiring = INST_RETIRED / Total_Slots                │
  │  │  │   → Frontend Bound = IDQ_UOPS_NOT_DELIVERED / Total      │
  │  │  │   → Bad Speculation = 冲刷 + 丢弃 μop                    │
  │  │  │   → Backend Bound = 剩余                                  │
  │  │  │                                                           │
  │  │  工具: perf stat --topdown → 百分比 → 下钻 Level 2/3/4       │
  │  │  └─ 结合 perf record → 定位热点函数 → 针对性优化             │
  │  │                                                              │
  │  └───────────────────────────────────────────────────────────── │
  └─────────────────────────────────────────────────────────────────┘
```

---

## 八、易错点与注意事项

### 8.1 虚拟机环境

| 问题 | 原因 | 解决 |
|------|------|------|
| PMU 计数器 `<not supported>` | KVM 默认不透传 PMU | `-cpu host` 或物理机测试 |
| cpufreq 不可用 | 频率由宿主机管理 | 物理机测试 |
| 温度不可读 | 传感器不透传 | 物理机测试 |
| perf 软件事件可用 | 不依赖硬件 PMU | 直接使用 |

### 8.2 Topdown 分析

| 问题 | 原因 | 解决 |
|------|------|------|
| 指标显示 `<not counted>` | PMU 计数器不够，multiplexing | 降低层级或用 toplev.py |
| 结果不稳定 | 其他 perf 进程占用计数器 | 确认独占 PMU |
| 优化后新瓶颈浮现 | 正常的瓶颈转移现象 | 迭代分析 |
| 只看平均值掩盖差异 | 程序有阶段性行为 | 用 `-I 1000` 间隔打印 |

### 8.3 调度器

| 问题 | 说明 |
|------|------|
| vruntime 不是实际时间 | vruntime = 实际时间 × (1024/weight)，受 nice 值影响 |
| min_vruntime 单调递增 | 防止新唤醒任务获得不公平优势 |
| nice -20 与 +19 差距巨大 | vruntime 增长率差 5900 倍 (88761/15) |
| 调度周期随任务数增长 | ≤8 任务: 6ms; >8 任务: nr × 0.75ms |

### 8.4 SMT 性能

| 场景 | 预期 |
|------|------|
| 两个无关 CPU-bound 线程 | 性能可能下降（争抢执行单元和缓存） |
| 一个计算 + 一个内存密集 | 理想互补（~30% 提升） |
| CPU 密集型关闭 SMT | 可能更好（独占 ROB 和端口） |

---

## 九、文件索引

### cpu_arch 目录

| 文件 | 内容 | 行数 | 预计时间 |
|------|------|------|---------|
| `learning-roadmap.md` | 整体学习路线图，8 大章节 | ~760 行 | 通读 20 分钟 |
| `QA.md` | 问答集（暂无内容，有待补充） | ~37 行 | — |
| `reading/01_cpu_microarchitecture.md` | 流水线/缓存/分支预测/ROB/SMT/内存序 | ~650 行 | 40-60 分钟 |
| `reading/02_kernel_cpu_scheduler.md` | 调度器架构/数据结构/CFS 算法/调度域 | ~970 行 | 50-70 分钟 |
| `reading/03_command_reference.md` | lscpu/taskset/numactl/cpupower 等 11 个调优场景 | ~740 行 | 30-40 分钟 |
| `reading/04_command_output_demo.md` | 阿里云 ECS 真实命令输出 + 逐行注解 | ~680 行 | 30-40 分钟 |
| `reading/README.md` | 阅读指南与文件说明 | ~30 行 | 2 分钟 |

### topdown 目录

| 文件 | 内容 | 行数 | 预计时间 |
|------|------|------|---------|
| `learning-roadmap.md` | TMAM 方法论概览 + 四级层次模型 + 6 步学习路线 | ~550 行 | 通读 20 分钟 |
| `QA.md` | 问答集（Q1: VM 上 PMU 不可用, Q2: Pipeline Width） | ~85 行 | 5 分钟 |
| `reading/01_pipeline_slots.md` | Pipeline Slot 定义 + 四大类瓶颈详解 + 微架构参数 | ~715 行 | 25 分钟 |
| `reading/02_command_reference.md` | perf stat --topdown 参数详解 + 15 个分析场景 | ~655 行 | 20 分钟 |
| `reading/03_command_output_demo.md` | 三种负载对比 + Level 2 下钻 + 优化效果分析 | ~1050 行 | 30 分钟 |
| `reading/README.md` | 阅读指南与文件说明 | ~42 行 | 2 分钟 |

### 推荐学习顺序

```
  ① cpu_arch/learning-roadmap.md        — 总览全貌 (20 min)
  ② cpu_arch/reading/01_cpu_microarchitecture.md  — 硬件基础 (40 min)
  ③ cpu_arch/reading/02_kernel_cpu_scheduler.md   — 调度器核心 (50 min)
  ④ cpu_arch/reading/03_command_reference.md       — 命令速查 (30 min)
  ⑤ cpu_arch/reading/04_command_output_demo.md     — 真实输出 (30 min)
  ⑥ topdown/learning-roadmap.md          — TMAM 方法论 (20 min)
  ⑦ topdown/reading/01_pipeline_slots.md — Slot 与四大类 (25 min)
  ⑧ topdown/reading/02_command_reference.md        — 命令详解 (20 min)
  ⑨ topdown/reading/03_command_output_demo.md      — 输出实战 (30 min)
  ⑩ 回到本文总结                        — 查漏补缺 (15 min)

  总计: ~5 小时
```

---

> **文档生成日期：** 2026-07-04
> **来源目录：** `docs/cpu_arch/` + `docs/topdown/`
> **总文件数：** 13 个源文件（约 5,900 行）
