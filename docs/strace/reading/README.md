# strace 源码阅读指南

> 本目录包含 strace 核心机制的源码阅读材料，每个文件都带有详细中文注释。
> 按 Step 1 → 5 的顺序阅读。

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 |
|------|------|------|------|--------|
| **1** | [01_ptrace_mechanism.md](./01_ptrace_mechanism.md) | 内核 `ptrace.c` + strace `strace.c` | ~350 | ★ ptrace 机制详解：TRACEME/ATTACH/SYSCALL/GETREGS，性能开销分析 |
| **2** | [02_strace_main.c](./02_strace_main.c) | strace `strace.c` | ~350 | ★ main()、init()、fork/attach、next_event/dispatch 主循环 |
| **3** | [03_syscall_decode.md](./03_syscall_decode.md) | strace `syscall.c` | ~450 | ★ 解码流水线、PTRACE_GETREGSET、sysent 表、寄存器映射 |
| **4** | [04_command_reference.md](./04_command_reference.md) | strace 全部源码 | ~450 | ★ 完整命令参考：参数与源码位置对照、过滤器分类、10+ 场景命令 |
| **5** | [05_command_output_demo.md](./05_command_output_demo.md) | 实际执行 | ~550 | ★ 每个命令实际执行 + 真实输出 + 逐行注解 |
| **6** | [06_performance_analysis.md](./06_performance_analysis.md) | 性能分析实战 | ~350 | ★ strace -c 统计、系统调用热点诊断、strace vs perf trace |

## 与其他文件的关系

```
learning-roadmap.md     → 完整学习路线（6 个 Step + 5 个实验 + 源码索引）
QA.md                   → 问答集（持续追加）
reading/README.md       → 本文件（阅读入口）
reading/01_*.md         → ptrace 核心机制
reading/02_*.c          → main()、初始化、主循环
reading/03_*.md         → syscall 解码流水线
reading/04_*.md         → 命令参考手册
reading/05_*.md         → 实战输出解析
reading/06_*.md         → ★ 性能分析应用（syscall 热点诊断）
```

## 阅读建议

1. **先读 learning-roadmap.md**：了解整体架构和 6 个 Step 的学习路线
2. **精读 01_ptrace_mechanism.md**：ptrace 是 strace 的基石，理解 tracer-tracee 模型
3. **对照 reading-roadmap.md Step 2-5**：阅读 strace.c 和 syscall.c 的关键函数
4. **查阅 04_command_reference.md**：需要某个参数用法时快速参考
5. **运行 05_command_output_demo.md 中的命令**：边看输出边理解每一列的含义

## 文件说明

- 每个文件顶部标注原始路径，方便在 `src/strace/src/` 中查找完整源码
- ★ 标记核心重点
- 表格对照源码与行为
- ASCII 流程图展示调用链和数据流
- 短文件（< 200 行）完整拷贝 + 逐行注释
- 长文件仅截取关键函数 + 详细注释
