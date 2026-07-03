# BPF/eBPF 源码阅读指南

> 本目录包含 BPF 子系统和三大工具（bcc/bpftrace/libbpf）的源码阅读材料，每个文件都带有详细中文注释。
> 按 Step 1 → 8 的顺序阅读。Step 1-5 为概念和用法，Step 6-8 为内核源码深入阅读。

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 |
|------|------|------|------|--------|
| **1** | [01_bpf_architecture.md](./01_bpf_architecture.md) | 内核 `bpf.h` + libbpf | 740+ | BPF 程序类型、verifier 原理、Map 类型、helper functions、JIT |
| **2** | [02_bpftrace_basics.md](./02_bpftrace_basics.md) | bpftrace 源码 | 730+ | probe 类型、内置变量、map 操作、聚合函数、编译流程 |
| **3** | [03_command_reference.md](./03_command_reference.md) | bpftrace + bpftool | 620+ | bpftrace 语法速查、常用 one-liners 30+、bpftool 命令 |
| **4** | [04_command_output_demo.md](./04_command_output_demo.md) | 实际执行输出 | 1040+ | bpftrace one-liners 真实输出 + 逐行注解 + 与 blktrace 对比 |
| **5** | [05_performance_diagnostics.md](./05_performance_diagnostics.md) | ★ 综合文档 | 350+ | 性能诊断 one-liners：自旋锁追踪、软中断分析、调度延迟、cache miss |
| **6** | [06_bpf_syscall.h](./06_bpf_syscall.h) | ★ 内核源码摘录 | 490+ | bpf() 系统调用接口：cmd 枚举、union bpf_attr、程序类型、Map 类型 |
| **7** | [07_bpftrace_probe.cpp](./07_bpftrace_probe.cpp) | ★ bpftrace 源码摘录 | 630+ | probe 附着全流程：编译流水线、kprobe/tracepoint/uprobe/profile 附着 |
| **8** | [08_bpf_verifier.c](./08_bpf_verifier.c) | ★ 内核源码摘录 | 780+ | BPF 验证器：bpf_check()、do_check()、寄存器类型系统、常见拒绝原因 |

## 阅读建议

1. **先读 Step 1**：理解 BPF 虚拟机、程序类型、Map 类型 — 这是所有 BPF 工具的基础
2. **Step 2 学 bpftrace**：从最简单的 one-liner 开始，理解 probe 类型和 DSL 语法
3. **Step 3 当速查表**：语法参考 + 30+ 实用 one-liners + bpftool 命令
4. **Step 4 看真实输出**：每个命令实际执行，对照输出理解每个字段的含义
5. **Step 5 性能诊断**：★ 针对 CPU 高占用低吞吐、软中断高、锁竞争的实战诊断脚本
6. **Step 6-8 深入内核**：★ 从源码层面理解 bpf() 系统调用、probe 附着、verifier 验证
7. **结合 blktrace 学习**：Step 4 中有 bpftrace 与 blktrace 追踪同一事件的对比

## 文件说明

- Step 1-5 使用 Markdown 格式（概念 + 源码索引），适合快速理解 BPF 全貌
- **Step 6-8 使用源码摘录格式**（`.h`/`.cpp`/`.c`），从实际源码中拷贝关键代码 + 详细中文注释，适合深入理解实现细节
- 每个文件顶部标注相关源码位置，方便在 `src/` 中查找
- 使用 ★ 标记核心重点，表格对照概念，ASCII 图展示数据流
- 代码块标注语言类型（C / bpftrace / bash）
- **参考模板：** Step 6-8 遵循 `docs/blktrace/reading/` 的注释风格

## 源码目录映射

| 源码目录 | 说明 |
|---------|------|
| `src/linux-5.10/kernel/bpf/` | ★ 内核 BPF 实现（verifier、syscall、map 等） |
| `src/linux-5.10/include/uapi/linux/bpf.h` | ★ BPF 用户空间 API（系统调用接口） |
| `src/linux-5.10/include/linux/bpf.h` | 内核 BPF 内部定义（寄存器类型等） |
| `src/linux-5.10/include/linux/bpf_verifier.h` | 验证器内部数据结构 |
| `src/libbpf/` | libbpf — BPF 程序加载库（C） |
| `src/bpftrace/` | bpftrace — 高级追踪 DSL |
| `src/bpftrace/src/attached_probe.cpp` | ★ bpftrace probe 附着实现 |
| `src/bpftrace/src/ast/passes/` | bpftrace 编译流水线（AST passes） |
| `src/bcc/` | bcc — BPF Compiler Collection（Python/C） |
