# Top-Down Microarchitecture Analysis — 阅读指南

> 从流水线 Slot 出发，系统掌握 CPU 微架构瓶颈分析方法

---

## 阅读路线

| Step | 文件 | 类型 | 行数 | 关注点 |
|------|------|------|------|--------|
| 1 | `01_pipeline_slots.md` | 概念讲解 | ~450 | CPU 流水线模型、Pipeline Slot 定义、四大类瓶颈图解 |
| 2 | `02_command_reference.md` | 命令参考 | ~450 | perf stat --topdown 参数详解、事件组、分析场景 |
| 3 | `03_command_output_demo.md` | 输出解析 | ~550 | 真实命令输出 + 逐行注解 + 优化建议 |
| 4 | `04_amd_topdown.md` | AMD 专题 | ~400 | ★ AMD 替代方案：stalled-cycles、IBS、μProf |
| 5 | `05_high_cpu_low_throughput.md` | 实战诊断 | ~450 | ★ 5 步诊断法、三种典型案例、AMD 适配 |

---

## 阅读建议

1. **先读 `learning-roadmap.md`**：了解整体方法论和四级层次模型
2. **按 Step 顺序阅读**：每篇 reading 文件都有"预计阅读时间"标注
3. **边读边做**：每步都有"思考题"和"实操任务"，建议在物理机上验证
4. **对照输出**：`03_command_output_demo.md` 中的真实输出是最好的理解验证
5. **AMD 用户**：Step 4 必读，了解如何在 AMD 上做等效的 Top-Down 分析
6. **实战诊断**：Step 5 是端到端的诊断流程，适用于"CPU 高占用低吞吐"等常见问题

---

## 文件说明

- 本目录的文件均为 `.md` 格式的概念讲解和命令参考（非源码摘录）
- TMAM 的核心是**方法论 + 工具使用**，不涉及特定源码文件
- 如需了解 perf 内部实现，请参考 `src/linux-5.10/tools/perf/` 源码
- AMD 相关内容基于 AMD PPR（Processor Programming Reference）和 AMD μProf 文档

---

## 与 blktrace 的差异

| 维度 | blktrace | TMAM |
|------|---------|------|
| 分析对象 | Block I/O 子系统 | CPU 微架构流水线 |
| 数据来源 | 内核 tracepoint | 硬件 PMU 计数器 |
| 工具类型 | 用户态采集工具 | perf 内置 metric |
| reading 内容 | 源码摘录 + 注释 | 概念讲解 + 命令参考 |
| 核心数据 | blk_io_trace 结构 | Pipeline Slot 占比 |
