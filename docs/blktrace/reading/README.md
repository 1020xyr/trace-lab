# blktrace 源码阅读指南

> 本目录包含 blktrace 全链路的源码阅读材料，每个文件都带有详细中文注释。
> 按 Step 1 → 7 的顺序阅读。

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 |
|------|------|------|------|--------|
| **1** | [01_blktrace_api.h](./01_blktrace_api.h) | 内核 `uapi/linux/` + 用户态 | 146 | 数据结构 `blk_io_trace`、action 标志位、ioctl 定义 |
| **2** | [02_block_tracepoints.h](./02_block_tracepoints.h) | 内核 `trace/events/block.h` | 656 | 所有 block tracepoint 的定义（Q/D/C 的 hook 点） |
| **3** | [03_kernel_blktrace.c](./03_kernel_blktrace.c) | 内核 `kernel/trace/blktrace.c` | ~300 | `__blk_add_trace()` 核心实现 + relay 初始化 + 回调注册 |
| **4** | [04_userspace_blktrace.c](./04_userspace_blktrace.c) | 用户态 `blktrace/blktrace.c` | ~200 | `main()`、ioctl 调用、relay 数据读取主循环 |
| **5** | [05_blkparse.c](./05_blkparse.c) | 用户态 `blktrace/blkparse.c` | ~80 | `dump_trace()` 事件解析与格式化输出 |
| **6** | [06_btt_trace_handlers.c](./06_btt_trace_handlers.c) | 用户态 `blktrace/btt/` | ~260 | Q/D/C 事件配对、Q2D/D2C/Q2C 延迟计算 |
| **7** | [07_blk_mq_tracepoints.c](./07_blk_mq_tracepoints.c) | 内核 `block/blk-mq.c` | ~100 | tracepoint 在 blk-mq I/O 路径中的调用位置 |

## 阅读建议

1. **先通读 Step 1**：理解 `blk_io_trace` 结构体 — 这是所有工具的数据基础
2. **对照 Step 2 + 7**：理解 tracepoint 定义（block.h）和调用位置（blk-mq.c）的关系
3. **Step 3 是核心**：`__blk_add_trace()` 是内核中所有事件的记录入口
4. **Step 4 理解用户态**：blktrace 工具如何通过 ioctl + relay 与内核交互
5. **Step 5-6 理解解析**：blkparse 如何解析二进制，btt 如何计算延迟

## 文件说明

- 短文件（< 200 行）：完整拷贝，逐行注释
- 长文件（> 200 行）：仅截取关键函数，跳过无关代码
- 每个文件顶部标注原始路径，方便在 `src/` 中查找完整版
