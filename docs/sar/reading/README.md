# sar 源码阅读指南

> 本目录包含 sar (System Activity Reporter) 的架构分析和命令参考材料。
> 按 Step 1 → 3 的顺序阅读。

## 阅读路线

| Step | 文件 | 内容 | 行数 | 关注点 |
|------|------|------|------|--------|
| **1** | [01_sar_architecture.md](./01_sar_architecture.md) | 架构详解 | ~350 | sadc/sa1/sa2/sar 关系、数据文件格式、采集配置 |
| **2** | [02_command_reference.md](./02_command_reference.md) | 命令参考手册 | ~450 | 全部参数分类、10+ 场景用法 |
| **3** | [03_command_output_demo.md](./03_command_output_demo.md) | 命令输出实战 | ~550 | 真实输出 + 逐列注解 + 诊断要点 |

## 阅读建议

1. **先通读 Step 1**：理解 sadc → saXX → sar 的完整数据流 — 这是使用 sar 的基础
2. **Step 2 按需查阅**：按"CPU → 内存 → I/O → 网络 → 队列"的顺序，每组记住核心参数
3. **Step 3 是实战核心**：每个输出都逐列注解，标注正常范围和异常信号

## 文件说明

- 所有文件均为 markdown 格式（非源码注释），因为 sar 的核心逻辑在 C 二进制中
- 重点分析 sadc 的采集机制和 sa 文件的二进制格式
- 每个命令都实际执行，输出真实数据并逐列注解
- 用 ★ 标记核心概念和关键指标
