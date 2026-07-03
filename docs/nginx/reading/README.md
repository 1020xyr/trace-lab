# nginx 源码阅读指南

> 本目录包含 nginx 核心架构的源码阅读材料，每个文件都带有详细中文注释。
> 按 Step 1 → 3 的顺序阅读。

## 阅读路线

| Step | 文件 | 关注点 | 行数 | 预计时间 |
|------|------|--------|------|---------|
| **1** | [01_nginx_architecture.md](./01_nginx_architecture.md) | master-worker 模型、事件驱动、连接池/内存池、11 phase 流水线 | ~450 | 25 分钟 |
| **2** | [02_command_reference.md](./02_command_reference.md) | 命令行参数、信号控制、nginx.conf 关键指令、性能调优场景 | ~450 | 20 分钟 |
| **3** | [03_command_output_demo.md](./03_command_output_demo.md) | 实际命令执行、真实输出片段、逐行注解 | ~550 | 30 分钟 |

## 阅读建议

1. **先读 Step 1**：建立 nginx 架构的全局认知 — master-worker、epoll、phase handler 是三个核心支柱
2. **再读 Step 2**：掌握日常运维和性能调优所需的命令与配置
3. **最后读 Step 3**：通过真实命令输出验证前两步学到的知识，加深理解

## 文件说明

- 所有文件为 Markdown 格式，包含 ASCII 流程图、表格、★ 标记
- 每个文件顶部标注阅读要点和预计时间
- ★ 标记核心重点，帮助快速定位关键信息
- 每个文件底部有 ★ 总结段，概括全文件要点

## 前置知识

- 了解基本的 HTTP 协议（请求行、请求头、响应码）
- 了解 Linux 系统调用（epoll、sendfile、mmap）
- 了解进程模型（fork、信号、进程间通信）
