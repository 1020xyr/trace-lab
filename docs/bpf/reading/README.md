# BPF/eBPF 源码阅读指南

> 本目录包含 BPF 子系统和三大工具（bcc/bpftrace/libbpf）的源码阅读材料，每个文件都带有详细中文注释。
> 按 Step 1 → 4 的顺序阅读。

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 |
|------|------|------|------|--------|
| **1** | [01_bpf_architecture.md](./01_bpf_architecture.md) | 内核 `bpf.h` + libbpf | 350+ | BPF 程序类型、verifier 原理、Map 类型、helper functions |
| **2** | [02_bpftrace_basics.md](./02_bpftrace_basics.md) | bpftrace 源码 | 350+ | probe 类型、内置变量、map 操作、聚合函数、编译流程 |
| **3** | [03_command_reference.md](./03_command_reference.md) | bpftrace + bpftool | 400+ | bpftrace 语法速查、常用 one-liners 30+、bpftool 命令 |
| **4** | [04_command_output_demo.md](./04_command_output_demo.md) | 实际执行输出 | 400+ | bpftrace one-liners 真实输出 + 逐行注解 + 与 blktrace 对比 |

## 阅读建议

1. **先读 Step 1**：理解 BPF 虚拟机、程序类型、Map 类型 — 这是所有 BPF 工具的基础
2. **Step 2 学 bpftrace**：从最简单的 one-liner 开始，理解 probe 类型和 DSL 语法
3. **Step 3 当速查表**：语法参考 + 30+ 实用 one-liners + bpftool 命令
4. **Step 4 看真实输出**：每个命令实际执行，对照输出理解每个字段的含义
5. **结合 blktrace 学习**：Step 4 中有 bpftrace 与 blktrace 追踪同一事件的对比

## 文件说明

- 本目录使用 Markdown 格式（非源码摘录），因为 BPF 涉及多个项目，以概念 + 源码索引方式组织
- 每个文件顶部标注相关源码位置，方便在 `src/` 中查找
- 使用 ★ 标记核心重点，表格对照概念，ASCII 图展示数据流
- 代码块标注语言类型（C / bpftrace / bash）

## 源码目录映射

| 源码目录 | 说明 |
|---------|------|
| `src/libbpf/` | libbpf — BPF 程序加载库（C） |
| `src/bpftrace/` | bpftrace — 高级追踪 DSL |
| `src/bcc/` | bcc — BPF Compiler Collection（Python/C） |
| `/usr/include/linux/bpf.h` | 内核 BPF 接口定义 |
| `/usr/include/linux/btf.h` | BPF Type Format 定义 |
