# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目性质

这是一个**源码学习仓库**（trace-lab · 探针手记），目标是深入理解 Linux 性能工具的采集机制、内核 hook 点、数据格式与解析逻辑。不是传统的软件开发项目——我们的产出是**带注释的源码阅读材料、实验报告、QA 问答集**，而非可运行的应用程序。

## 语言

所有交流、文档、注释、commit message 使用**简体中文**。代码标识符（变量名、函数名、文件名）保持英文。

## 仓库结构

```
trace-lab/
├── src/           # 第三方源码（blktrace/fio/strace/bcc/bpftrace/libbpf/linux-5.10）
├── docs/          # ★ 我们的工作在这里
│   └── {tool}/
│       ├── reading/    # 带中文注释的源码阅读材料（按 Step 编号）
│       ├── lab/        # 实战实验报告（真实设备上的命令输出）
│       └── QA.md       # 问答集（持续追加）
└── experiments/   # 实验脚本（.sh）
```

## 关键约定

### `src/` 目录

`src/` 包含第三方源码（已从 git 中移除 `.git` 目录），作为阅读参考材料。这些文件**不是我们的代码**，不应修改。IDE 对 `docs/*/reading/` 中的 `.c`/`.h` 文件报"identifier undefined"是**预期行为**——这些文件是注释性摘录，不是可编译代码。

### `docs/*/reading/` 文件

- 短文件（< 200 行）：完整拷贝 + 逐行注释
- 长文件：仅截取关键函数 + 详细注释
- 每个文件顶部标注原始路径、阅读要点、预计时间
- 编号规则：`01_*.h` → `02_*.h` → ... 按学习顺序排列

### QA 文件

每个工具的 `QA.md` 采用固定格式追加：
```markdown
## Q{N}: {问题标题}

**日期：** YYYY-MM-DD
**场景：** 在哪里遇到的问题
**相关文件：** 对应的 reading 文件

### 回答
（源码级解答 + 代码引用 + 表格/图示）
```

## 编译 blktrace（从 src/ 源码）

blktrace 通常不在发行版默认仓库中，需从源码编译：

```bash
cd src/blktrace
make -j$(nproc)
# btreplay 可能因缺少 libaio.h 编译失败，手动链接核心工具：
gcc -o blktrace blktrace.o rbtree.o act_mask.o -lpthread
gcc -o blkparse blkparse.o blkparse_fmt.o rbtree.o act_mask.o
# btt 在子目录：btt/btt（make -C btt 即可）
```

fio 通过包管理器安装：`yum install -y fio`

## 实验运行

- 实验在 `/tmp/` 下运行（避免在仓库中产生二进制文件）
- blktrace 使用**相对路径**输出（绝对路径会有 `./` 前缀 bug）
- 实验前先 `cd /tmp/blktrace_lab` 再执行

```bash
cd /tmp/blktrace_lab
/root/code/tool_learn/src/blktrace/blktrace -d /dev/vdb -o exp1 -w 8 &
sleep 1
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=6 --time_based
wait
```

## Commit 格式

```
类型: 简短描述

（详细说明）
```

常用类型：`新增`、`更新`、`修复`、`文档`

## 源码快速定位

| 要找什么 | 路径 |
|---------|------|
| blk_io_trace 结构体 | `src/linux-5.10/include/uapi/linux/blktrace_api.h` |
| block tracepoint 定义 | `src/linux-5.10/include/trace/events/block.h` |
| 内核 __blk_add_trace() | `src/linux-5.10/kernel/trace/blktrace.c` |
| 用户态 blktrace main() | `src/blktrace/blktrace.c` |
| blk_fill_rwbs() | `src/linux-5.10/kernel/trace/blktrace.c:1959` |
| blk-flush 序列逻辑 | `src/linux-5.10/block/blk-flush.c` |
| btt 延迟计算 | `src/blktrace/btt/trace_complete.c` |
| fio I/O 引擎 | `src/fio/ioengines/` |
| perf 源码 | `src/linux-5.10/tools/perf/` |
