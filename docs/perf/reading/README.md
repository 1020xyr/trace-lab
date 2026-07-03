# perf 源码阅读指南

> ★ **perf 是 Linux 性能分析的瑞士军刀** — 从硬件计数器到函数级热点，从 CPU 瓶颈到锁竞争。
> 在性能分析方法论中，perf 覆盖"CPU 微架构分析 → 函数热点定位 → 锁/内存诊断"全链路。

本目录包含 perf 全链路的源码阅读材料，每个文件都带有详细中文注释。
按 Step 1 → 9 的顺序阅读。

**预计总阅读时间：** 5-7 小时（快速入门 40 分钟，深入理解 3 小时）

---

## ★ perf 在性能分析中的定位

```
┌─────────────────────────────────────────────────────────────────┐
│                    CPU 性能分析工具链                              │
│                                                                  │
│  第 1 层：宏观发现                                                │
│    sar -u / mpstat / top → CPU 高？%user 高？%system 高？       │
│                                                                  │
│  第 2 层：进程定位                                                │
│    pidstat -u → 哪个进程占 CPU？                                │
│                                                                  │
│  第 3 层：深入分析 ← ★ perf 在这里                               │
│    perf stat → IPC、cache-miss 率、branch-miss（微架构指标）    │
│    perf record + report → 函数级热点（火焰图）                  │
│    perf c2c → false sharing 检测                                │
│    perf lock → 锁竞争分析                                       │
│    perf mem → 内存延迟分析                                      │
│                                                                  │
│  第 4 层：微架构理解                                              │
│    cpu_arch/topdown → 理解 IPC 低/miss 高的硬件根因             │
└─────────────────────────────────────────────────────────────────┘
```

---

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 | 预计时间 |
|------|------|------|------|--------|---------|
| **1** | [01_perf_event.h](./01_perf_event.h) | 内核 `uapi/linux/perf_event.h` | ~440 | ★ 核心结构体 `perf_event_attr`、事件类型枚举、采样格式标志 | 30 分钟 |
| **2** | [02_perf_record.c](./02_perf_record.c) | 用户态 `builtin-record.c` | ~200 | `__cmd_record()` 核心流程：open → mmap → 主循环 → 写文件 | 20 分钟 |
| **3** | [03_perf_report.c](./03_perf_report.c) | 用户态 `builtin-report.c` | ~150 | `__cmd_report()` 解析流程：读 perf.data → 构建直方图 → 排序输出 | 15 分钟 |
| **4** | [04_command_reference.md](./04_command_reference.md) | 综合文档 | 650+ | perf 完整命令参考：record/report/stat/top/list/script/trace 等 | 30 分钟 |
| **5** | [05_command_output_demo.md](./05_command_output_demo.md) | 实战输出 | 1000+ | 真实命令输出逐行解读：stat/record+report/script/trace/火焰图 | 40 分钟 |
| **6** | [06_hw_counters_diagnosis.md](./06_hw_counters_diagnosis.md) | ★ 综合文档 | 300+ | ★ 硬件计数器详解：IPC、cache-miss 率、branch-miss、CPU 高占用低吞吐诊断 | 30 分钟 |
| **7** | [07_c2c_false_sharing.md](./07_c2c_false_sharing.md) | ★ 综合文档 | 250+ | ★ perf c2c 检测 false sharing：HITM 分析、cache line 争用 | 25 分钟 |
| **8** | [08_lock_contention.md](./08_lock_contention.md) | ★ 综合文档 | 200+ | ★ perf lock 锁竞争分析：自旋锁热点定位、争用率 | 20 分钟 |
| **9** | [09_mem_and_amd.md](./09_mem_and_amd.md) | ★ 综合文档 | 250+ | ★ perf mem 内存延迟分析 + AMD 特定事件 + IBS | 25 分钟 |

---

## 阅读建议

1. **先通读 Step 1**：理解 `perf_event_attr` 结构体 — 这是所有 perf 子命令的配置基础
2. **Step 2 + 3 对照阅读**：理解 record 写入和 report 读取是镜像关系
3. **Step 4 按需查阅**：命令参考是速查手册，用到哪个子命令就查哪个
4. **Step 5 动手实验**：在自己的系统上执行命令，对照输出理解每个字段的含义
5. **Step 6 理解硬件层**：★ IPC、cache-miss 率、branch-miss 是性能诊断的核心指标
6. **Step 7~8 按需深入**：多线程问题看 c2c，锁问题看 lock
7. **Step 9 AMD 用户必读**：AMD IBS、NUMA 拓扑、CCX 架构对性能分析的影响

---

## 阅读路径推荐

### 快速入门（40 分钟）

```
01_perf_event.h        → 理解 perf_event_attr 和事件类型
05_command_output_demo → 看真实输出，建立直觉
```

### 深入理解（3 小时）

```
01 → 02 → 03 → 04（速查）→ 05（重点）→ 06（重点）
```

### 完整学习（半天+）

```
01 → 02 → 03 → 04 → 05 → 06 → 07 → 08 → 09
+ 结合 learning-roadmap.md 中的实验动手操作
+ 结合 cpu_arch/reading/ 理解微架构原理
```

---

## ★ 与其他工具的交叉引用

### perf + topdown — 微架构瓶颈分析

```
★ 这是 CPU 性能分析最强大的组合

1. perf stat → 采集硬件计数器（IPC、cache-miss、branch-miss）
   │
   ├── IPC < 1.0 → 流水线利用率低
   ├── cache-miss > 5% → 内存访问瓶颈
   └── branch-miss > 3% → 分支预测失败严重
   │
   ▼
2. topdown 方法论 → 将瓶颈归类到 4 大类
   ├── Frontend Bound → 取指瓶颈（指令缓存、分支预测）
   ├── Backend Bound → 数据瓶颈（cache miss、TLB miss）
   ├── Bad Speculation → 预测错误浪费
   └── Retiring → 有效工作比例
   │
   ▼
3. cpu_arch 知识 → 理解硬件层面的根因
   ├── L3 miss 高 → 参见 cpu_arch/06_l3_cache_miss_analysis.md
   ├── false sharing → 参见 perf/07_c2c_false_sharing.md
   └── 自旋锁争用 → 参见 cpu_arch/07_spinlock_analysis.md
```

### perf + pidstat — 进程级 → 函数级

```
1. pidstat -u → 找到高 CPU 进程
2. perf record -p <PID> -g -- sleep 10 → 采集该进程的函数级热点
3. perf report → 火焰图分析，定位到具体函数
```

### perf + fio — I/O 引擎 CPU 开销分析

```
1. fio 生成 I/O 负载
2. perf stat -e cycles,instructions,cache-misses -p <fio_PID>
   → 测量 fio 的 CPU 效率
3. perf record -p <fio_PID> -g → 分析 I/O 路径的 CPU 热点
```

### perf + sar — 宏观 → 微观

```
1. sar -u → 发现 %user 或 %system 持续高（宏观趋势）
2. perf stat → 确认 IPC、cache-miss 等微架构指标
3. perf record → 定位热点函数（微观根因）
```

---

## 文件说明

- 短文件（< 200 行）：完整拷贝关键函数，逐行注释
- 长文件（> 200 行）：仅截取关键函数，跳过参数解析等辅助代码
- 每个文件顶部标注原始路径，方便在 `src/` 中查找完整版
- `.md` 文件为综合文档，结合源码分析和概念解释
- `.c`/`.h` 文件是注释性摘录，不是可编译代码（IDE 报 "identifier undefined" 是预期行为）

---

## 相关资源

- [learning-roadmap.md](../learning-roadmap.md) — 完整学习路线和实验清单
- [QA.md](../QA.md) — 常见问题问答集
- [cpu_arch reading](../cpu_arch/reading/) — ★ 微架构原理必读：理解 IPC/cache-miss 的硬件根因
- [pidstat reading](../pidstat/reading/) — 进程级 CPU 定位：找到 perf 分析的目标进程
- [sar reading](../sar/reading/) — 宏观趋势发现：从 sar 异常到 perf 深入
- [fio reading](../fio/reading/) — I/O 性能测试：perf + fio 联合分析
