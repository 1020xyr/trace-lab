# iostat 学习材料阅读指南

> 本目录包含 iostat 的工作原理、命令参考和实战输出解析。
> 按 Step 1 → 3 的顺序阅读。

## 阅读路线

| Step | 文件 | 行数 | 关注点 |
|------|------|------|--------|
| **1** | [01_iostat_architecture.md](./01_iostat_architecture.md) | ~520 | iostat 工作原理：/proc/diskstats 数据源、字段计算逻辑、与 blktrace 的差异 |
| **2** | [02_command_reference.md](./02_command_reference.md) | ~580 | 完整命令参考：所有参数、间隔采样模式、12 个场景组合 |
| **3** | [03_command_output_demo.md](./03_command_output_demo.md) | ~730 | 输出实战解析：8 个场景的真实输出 + 逐行注解 + 诊断表 |

## 阅读建议

1. **先读 Step 1**：理解 `/proc/diskstats` 的 20 个字段 — 这是 iostat 所有数据的来源
2. **Step 2 当手册用**：遇到不认识的参数时查阅
3. **Step 3 是核心**：每个场景都有真实命令输出 + 逐字段注解，对照自己的输出理解
4. **重点关注 `-x` 扩展输出**：这是日常诊断最常用的模式

## 文件说明

- 所有输出均来自实际执行（/dev/vdb，virtio-blk 40G）
- I/O 负载由 fio 生成
- 每个场景标注了"逐字段注解"的字段列表
