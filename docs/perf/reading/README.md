# perf 源码阅读指南

> 本目录包含 perf 全链路的源码阅读材料，每个文件都带有详细中文注释。
> 按 Step 1 → 6 的顺序阅读。

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 |
|------|------|------|------|--------|
| **1** | [01_perf_event.h](./01_perf_event.h) | 内核 `uapi/linux/perf_event.h` | ~440 | ★ 核心结构体 `perf_event_attr`、事件类型枚举、采样格式标志 |
| **2** | [02_perf_record.c](./02_perf_record.c) | 用户态 `builtin-record.c` | ~200 | `__cmd_record()` 核心流程：open → mmap → 主循环 → 写文件 |
| **3** | [03_perf_report.c](./03_perf_report.c) | 用户态 `builtin-report.c` | ~150 | `__cmd_report()` 解析流程：读 perf.data → 构建直方图 → 排序输出 |
| **4** | [04_pmu_events.md](./04_pmu_events.md) | 综合文档 | ~250 | PMU 硬件事件详解：cycles/instructions/cache-miss/branch-miss |
| **5** | [05_command_reference.md](./05_command_reference.md) | 综合文档 | 400+ | perf 完整命令参考：record/report/stat/top/list/script/trace 等 |
| **6** | [06_command_output_demo.md](./06_command_output_demo.md) | 实战输出 | 500+ | 真实命令输出逐行解读：stat/record+report/script/trace/火焰图 |

## 阅读建议

1. **先通读 Step 1**：理解 `perf_event_attr` 结构体 — 这是所有 perf 子命令的配置基础
2. **Step 2 + 3 对照阅读**：理解 record 写入和 report 读取是镜像关系
3. **Step 4 理解硬件层**：了解 PMU 计数器的物理本质，理解为什么虚拟机可能不支持硬件事件
4. **Step 5 按需查阅**：命令参考是速查手册，用到哪个子命令就查哪个
5. **Step 6 动手实验**：在自己的系统上执行命令，对照输出理解每个字段的含义

## 文件说明

- 短文件（< 200 行）：完整拷贝关键函数，逐行注释
- 长文件（> 200 行）：仅截取关键函数，跳过参数解析等辅助代码
- 每个文件顶部标注原始路径，方便在 `src/` 中查找完整版
- `.md` 文件为综合文档，结合源码分析和概念解释
