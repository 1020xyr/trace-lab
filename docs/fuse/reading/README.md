# FUSE 源码阅读指南

> 本目录包含 FUSE 全链路的源码阅读材料，每个文件都带有详细中文注释。
> 按 Step 1 → 4 的顺序阅读。

## 阅读路线

| Step | 文件 | 来源 | 关注点 |
|------|------|------|--------|
| **1** | [01_fuse_operations.h](./01_fuse_operations.h) | libfuse `include/fuse.h` | 核心结构体 `fuse_operations`——用户态文件系统的"虚函数表" |
| **2** | [02_dev_fuse_protocol.md](./02_dev_fuse_protocol.md) | 内核 `uapi/linux/fuse.h` + `fs/fuse/dev.c` | /dev/fuse 通信协议、请求/响应格式、opcode 定义 |
| **3** | [03_hello_fuse.c](./03_hello_fuse.c) | 自编示例（参考 libfuse example） | 一个完整可编译的最小 FUSE 文件系统，逐函数注释 |
| **4** | [04_fuse_performance.md](./04_fuse_performance.md) | 内核源码 + 性能分析 | FUSE 性能开销来源、优化手段、blktrace/strace 分析方法 |
| **5** | [05_command_reference.md](./05_command_reference.md) | 命令参考 | fusermount/mount 参数、libfuse API、8 个常用场景命令 |
| **6** | [06_command_output_demo.md](./06_command_output_demo.md) | 实际执行 | 8 个场景的真实输出 + 逐行注解 |
| **7** | [07_performance_optimization.md](./07_performance_optimization.md) | ★ 深度优化 | writeback_cache 原理、splice、io_uring、诊断流程 |

## 阅读建议

1. **先通读 Step 1**：理解 `fuse_operations` 结构体 — 这是用户态 FUSE 文件系统的核心接口
2. **Step 2 理解协议**：/dev/fuse 是内核与用户态的通信通道，理解请求/响应的二进制格式
3. **Step 3 动手实践**：编译运行 hello world，对照源码理解每个回调的触发时机
4. **Step 4 性能视角**：理解 FUSE 的开销来源，掌握性能分析方法

## 内核源码参考

本仓库 `src/linux-5.10/fs/fuse/` 包含完整的内核 FUSE 模块源码：

| 文件 | 职责 |
|------|------|
| `include/uapi/linux/fuse.h` | FUSE 协议定义（结构体、opcode、标志位） |
| `fs/fuse/dev.c` | /dev/fuse 设备实现（请求队列、读写处理） |
| `fs/fuse/file.c` | 文件操作 VFS 实现（read/write/open/release） |
| `fs/fuse/dir.c` | 目录操作 VFS 实现（lookup/readdir/mkdir） |
| `fs/fuse/inode.c` | inode 管理、mount 初始化 |
| `fs/fuse/fuse_i.h` | 内核 FUSE 内部数据结构 |

## 用户态库参考

libfuse 源码不在本仓库 `src/` 目录中。参考来源：

- **GitHub 仓库：** https://github.com/libfuse/libfuse
- **安装：** `yum install -y fuse-devel`（头文件安装在 `/usr/include/fuse/`）

## 文件说明

- `.h` 文件：从 libfuse 提取的关键结构体定义 + 逐字段中文注释
- `.md` 文件：协议解析 + ASCII 流程图 + 表格对照
- `.c` 文件：完整可编译示例 + 逐函数中文注释
- 每个文件顶部标注原始路径，方便在源码中查找完整版
