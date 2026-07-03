# CPU 架构与内核 CPU 子系统 源码阅读指南

> ★ **cpu_arch 是理解性能数据硬件根因的知识基础** — 从 CPU 微架构到内核调度器，解释"为什么 IPC 低、cache-miss 高"。
> 在性能分析方法论中，cpu_arch 位于"工具数据 → 硬件理解"的最后一环，将 perf 数字转化为微架构洞察。

本目录包含 CPU 微架构、内核调度器、cpufreq/cpuidle 子系统的阅读材料。
按 Step 1 → 7 的顺序阅读，每个文件都带有详细中文注释。

**预计总阅读时间：** 5-7 小时（快速入门 40 分钟，深入理解 4 小时）

---

## ★ cpu_arch 在性能分析中的定位

```
┌─────────────────────────────────────────────────────────────────┐
│                    CPU 性能分析完整链路                           │
│                                                                  │
│  第 1 层：宏观发现                                                │
│    sar -u / mpstat → CPU 高？%user 高？%system 高？             │
│                                                                  │
│  第 2 层：进程定位                                                │
│    pidstat -u → 哪个进程占 CPU                                  │
│                                                                  │
│  第 3 层：工具采集                                                │
│    perf stat → IPC、cache-miss 率、branch-miss                  │
│    perf record → 函数级热点                                      │
│                                                                  │
│  第 4 层：硬件理解 ← ★ cpu_arch 在这里                          │
│    理解 IPC 低 → 流水线阻塞原因（前端/后端/预测错误）            │
│    理解 cache-miss 高 → 缓存层次、NUMA 拓扑、CCX 共享           │
│    理解 锁竞争 → 自旋锁实现、CPU 调度器行为                      │
│                                                                  │
│  第 5 层：方法论归类                                              │
│    topdown → Frontend/Backend/Bad Speculation/Retiring          │
└─────────────────────────────────────────────────────────────────┘
```

---

## 阅读路线

| Step | 文件 | 来源 | 关注点 | 预计时间 |
|------|------|------|--------|---------|
| **1** | [01_cpu_microarchitecture.md](./01_cpu_microarchitecture.md) | 综合整理 | ★ 流水线 6 阶段、缓存层次、分支预测、乱序执行 ROB、SMT、内存序 | 40 分钟 |
| **2** | [02_kernel_cpu_scheduler.md](./02_kernel_cpu_scheduler.md) | 内核 `kernel/sched/` + `include/linux/sched.h` | ★ task_struct、sched_entity、rq、cfs_rq、CFS 红黑树、vruntime | 40 分钟 |
| **3** | [03_command_reference.md](./03_command_reference.md) | 系统文档 + man pages | lscpu / taskset / numactl / cpupower / /proc/cpuinfo 解读 | 25 分钟 |
| **4** | [04_command_output_demo.md](./04_command_output_demo.md) | 实际命令执行输出 | ★ 真实输出 + 逐行注解，包含 lscpu、schedstat、perf stat 等 | 30 分钟 |
| **5** | [05_amd_microarchitecture.md](./05_amd_microarchitecture.md) | AMD PPR + 优化指南 | ★★ AMD CCD/CCX 拓扑、8核共享L3、NPS配置、AMD vs Intel | 35 分钟 |
| **6** | [06_l3_cache_miss_analysis.md](./06_l3_cache_miss_analysis.md) | perf 文档 + 实践整理 | ★★ L3 miss 测量、两种因果链、perf c2c/mem、诊断流程 | 35 分钟 |
| **7** | [07_spinlock_analysis.md](./07_spinlock_analysis.md) | 内核源码 + perf/bpftrace | ★ ticket vs queued spinlock、perf lock、bpftrace 追踪 | 30 分钟 |

---

## 阅读建议

1. **先通读 Step 1**：理解 CPU 硬件基础 — 流水线、缓存、乱序执行是一切性能分析的硬件根基
2. **★ Step 2 是核心**：内核调度器决定任务在哪个 CPU 上何时运行，直接影响程序性能
3. **Step 3 掌握工具**：lscpu、taskset、numactl 是日常调优必备命令
4. **Step 4 对照实践**：真实命令输出 + 逐行注解，加深理解
5. **★★ Step 5 AMD 专题**：如果使用 AMD 服务器，必读 — CCD 拓扑和 L3 共享对性能影响巨大
6. **★★ Step 6 L3 Miss 专题**：性能工程师最常遇到的瓶颈类型，两种因果链的区分方法
7. **★ Step 7 自旋锁专题**：高竞争场景下的内核锁问题定位

---

## 阅读路径推荐

### 快速入门（40 分钟）

```
01_cpu_microarchitecture → 理解流水线和缓存基础
04_command_output_demo   → 看真实 lscpu/perf stat 输出
```

### 深入理解（4 小时）

```
01 → 02（重点）→ 03（速查）→ 04 → 05（AMD 必读）→ 06（重点）→ 07
```

### 完整学习（半天+）

```
01 → 02 → 03 → 04 → 05 → 06 → 07
+ perf/reading/ 中的硬件计数器和 topdown 文档
```

---

## ★ 微架构瓶颈速查

| 瓶颈类型 | perf 指标 | 硬件根因 | 详见 |
|---------|----------|---------|------|
| 前端阻塞 | Frontend Bound 高 | 指令缓存 miss、分支预测 | Step 1 |
| 后端阻塞 | Backend Bound 高 | L1/L2/L3 cache miss、TLB miss | Step 6 |
| 预测错误 | Bad Speculation 高 | 分支预测失败 | Step 1 |
| L3 miss | LLC-loads-miss 高 | 两种因果链（容量 vs 共享） | Step 6 |
| false sharing | HITM 高 | 多核写同一 cache line | perf/07_c2c |
| 锁竞争 | 自旋锁等待长 | ticket/queued spinlock | Step 7 |

---

## ★ 与其他工具的交叉引用

### cpu_arch + topdown — 微架构瓶颈归类

```
★ 这是 CPU 性能分析的方法论框架

1. perf stat → 采集硬件计数器
   │
   ├── IPC < 1.0 → 流水线利用率低
   ├── cache-miss > 5% → 内存访问瓶颈
   └── branch-miss > 3% → 分支预测失败
   │
   ▼
2. topdown 方法论 → 4 大类瓶颈归类
   ├── Frontend Bound → 取指瓶颈
   │   └── 参见 cpu_arch/01 Step 1（流水线前端）
   ├── Backend Bound → 数据瓶颈
   │   └── 参见 cpu_arch/06 Step 6（L3 miss 分析）
   ├── Bad Speculation → 预测错误
   │   └── 参见 cpu_arch/01 Step 1（分支预测）
   └── Retiring → 有效工作
       └── 越高越好
```

### cpu_arch + perf — 从数字到根因

```
1. perf stat 报告 cache-miss 率 = 15%
   → cpu_arch 知识解释：L3 cache miss 的两种因果链
      a) 容量型：工作集 > L3 大小
      b) 共享型：多核 false sharing
   → 指导下一步：perf c2c 区分两种情况
```

### cpu_arch + pidstat — 调度行为分析

```
1. pidstat -w → cswch/s 和 nvcswch/s
   → cpu_arch 知识解释：
      cswch/s 高 = 进程主动让出（I/O 等待、sleep）
      nvcswch/s 高 = 被调度器抢占（时间片用完、高优先级）
   → 结合 CFS 调度器理解（Step 2）
```

### cpu_arch + nginx — AMD CCD 绑核优化

```
1. lscpu → 确认 CCD/CCX 拓扑
2. cpu_arch 知识 → 同一 CCX 内共享 L3，跨 CCX 访问延迟高
3. nginx worker_processes + taskset → 绑定 worker 到同一 CCX
4. 效果：减少跨 CCX 的 L3 miss，降低延迟
   → 参见 nginx/04_performance_tuning.md
```

---

## 文件说明

- 概念文件（Step 1、5、6、7）：综合多源整理，用 ASCII 图表 + 表格展示
- 源码文件（Step 2）：从 `src/linux-5.10/` 提取关键结构体和函数，逐字段注释
- 参考文件（Step 3）：命令用法速查手册
- 实战文件（Step 4）：真实执行输出 + 逐行注解
- 每个文件顶部标注关键阅读要点和预计阅读时间
- 使用 `★` 标记核心重点，表格对照源码与含义

---

## 相关资源

- [learning-roadmap.md](../learning-roadmap.md) — 完整学习路线和实验清单
- [QA.md](../QA.md) — 常见问题问答集
- [perf reading](../perf/reading/) — ★ 工具层必读：硬件计数器采集和 topdown 分析
- [pidstat reading](../pidstat/reading/) — 进程级 CPU 分析：调度行为与上下文切换
- [nginx reading](../nginx/reading/) — 应用层调优：AMD CCD 绑核、高 CPU 排查
- [sar reading](../sar/reading/) — 宏观趋势发现：从 sar -u 到 cpu_arch 深入分析
