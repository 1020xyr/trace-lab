# sar / sysstat 源码阅读指南

> 本目录包含 sar（sysstat）数据采集与查询的学习材料，每个文件都带有详细中文注释。
> 按 Step 1 → 4 的顺序阅读。

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 |
|------|------|------|------|--------|
| **1** | [01_sar_architecture.md](./01_sar_architecture.md) | sysstat 源码 + 系统配置 | 450+ | sadc/sa1/sa2/sar 的关系、sa 二进制文件格式、systemd 定时采集 |
| **2** | [02_command_reference.md](./02_command_reference.md) | sar 命令手册 | 710+ | 按维度分类的参数速查：CPU/内存/I/O/网络/队列/中断 |
| **3** | [03_command_output_demo.md](./03_command_output_demo.md) | 实际执行输出 | 880+ | 19 个场景的真实 sar 输出逐列注解 |
| **4** | [04_diagnostic_workflow.md](./04_diagnostic_workflow.md) | ★ 综合文档 | 300+ | 性能诊断工作流：CPU 高占用低吞吐排查、网络软中断不均衡、工具联动 |

## 阅读建议

1. **先读 Step 1**：理解 sadc → sa 文件 → sar/sadf 的完整数据流
2. **Step 2 当速查表**：按维度（CPU/内存/I/O/网络）分类记忆参数
3. **Step 3 看真实输出**：每个命令实际执行，理解每列的含义和正常/异常范围
4. **Step 4 实战诊断**：★ 从 sar 发现异常到用专业工具深入定位的完整流程

## 核心指标异常阈值

| 指标 | 命令 | 正常值 | 异常值 |
|------|------|--------|--------|
| %iowait | `sar -u` | < 5% | > 10% |
| %system | `sar -u` | < 15% | > 30% |
| %soft | `sar -u ALL` | < 5% | > 10% |
| %memused | `sar -r` | < 80% | > 90% |
| majflt/s | `sar -B` | 0 | > 0 |
| await | `sar -d -p` | < 10ms | > 20ms |
| %util | `sar -d -p` | < 80% | > 90% |
| runq-sz | `sar -q` | < CPU 核数 | > CPU 核数 |
| rxdrop/s | `sar -n EDEV` | 0 | > 0 |

## 文件说明

- 所有文件均为 markdown 格式（非源码注释），因为 sar 的核心逻辑在 C 二进制中
- 重点分析 sadc 的采集机制和 sa 文件的二进制格式
- 每个命令都实际执行，输出真实数据并逐列注解
- 用 ★ 标记核心概念和关键指标
- sar 是系统级"雷达"——发现异常维度，然后用 perf/bpftrace/iostat 等深入
