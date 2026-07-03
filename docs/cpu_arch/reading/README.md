# CPU 架构与内核 CPU 子系统 源码阅读指南

> 本目录包含 CPU 微架构、内核调度器、cpufreq/cpuidle 子系统的阅读材料。
> 按 Step 1 → 4 的顺序阅读，每个文件都带有详细中文注释。

## 阅读路线

| Step | 文件 | 来源 | 关注点 |
|------|------|------|--------|
| **1** | 01_cpu_microarchitecture.md | 综合整理 | 流水线 6 阶段、缓存层次、分支预测、乱序执行 ROB、SMT、内存序 |
| **2** | 02_kernel_cpu_scheduler.md | 内核 `kernel/sched/` + `include/linux/sched.h` | task_struct、sched_entity、rq、cfs_rq、CFS 红黑树、vruntime |
| **3** | 03_command_reference.md | 系统文档 + man pages | lscpu / taskset / numactl / cpupower / /proc/cpuinfo 解读 |
| **4** | 04_command_output_demo.md | 实际命令执行输出 | 真实输出 + 逐行注解，包含 lscpu、schedstat、perf stat 等 |

## 阅读建议

1. **先通读 Step 1**：理解 CPU 硬件基础 — 流水线、缓存、乱序执行是一切性能分析的硬件根基
2. **Step 2 是核心**：内核调度器决定任务在哪个 CPU 上何时运行，直接影响程序性能
3. **Step 3 掌握工具**：lscpu、taskset、numactl 是日常调优必备命令
4. **Step 4 对照实践**：真实命令输出 + 逐行注解，加深理解

## 文件说明

- 概念文件（Step 1）：综合多源整理，用 ASCII 图表 + 表格展示
- 源码文件（Step 2）：从 `src/linux-5.10/` 提取关键结构体和函数，逐字段注释
- 参考文件（Step 3）：命令用法速查手册
- 实战文件（Step 4）：真实执行输出 + 逐行注解
- 每个文件顶部标注关键阅读要点和预计阅读时间
- 使用 `★` 标记核心重点，表格对照源码与含义
