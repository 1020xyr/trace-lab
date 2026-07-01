# trace-lab · 探针手记

> 从源码出发，深入理解 Linux 性能工具

## 项目简介

这是一个系统学习 Linux 性能/调试工具的实践仓库。通过**下载源码 → 对照阅读 → 动手实验**的方式，
深入理解每个工具的采集机制、内核 hook 点、数据格式与解析逻辑。

## 目录结构

```
trace-lab/
├── README.md                  # 本文件
├── src/                       # 源码（.gitignore 排除，仅本地使用）
│   ├── blktrace/              # blktrace 用户态工具（blktrace、blkparse、btt）
│   ├── linux-5.10/            # Linux 内核 5.10 源码
│   │   ├── kernel/trace/      #   ← 内核 blktrace.c 在这里
│   │   ├── block/             #   ← 块设备驱动层
│   │   ├── include/trace/events/block.h  # ← block tracepoint 定义
│   │   └── tools/perf/        #   ← perf 工具源码
│   ├── fio/                   # fio 灵活 I/O 测试工具
│   ├── strace/                # strace 系统调用追踪
│   ├── bcc/                   # BPF Compiler Collection
│   ├── bpftrace/              # bpftrace DTrace 风格脚本工具
│   └── libbpf/                # libbpf BPF 加载库
│
├── docs/                      # 学习笔记与分析文档
│   ├── blktrace/              # blktrace + blkparse + btt 学习路线
│   │   └── learning-roadmap.md
│   ├── fio/
│   ├── strace/
│   ├── perf/
│   └── bpf/
│
└── experiments/               # 实验脚本
    ├── blktrace/
    └── ...
```

## 学习路线

| 阶段 | 工具 | 核心概念 | 预计时间 |
|------|------|---------|---------|
| **1** | blktrace + blkparse + btt | tracepoint、relay buffer、blk_io_trace | 1-2 周 |
| **2** | fio | I/O 引擎、queue depth、direct I/O | 3-5 天 |
| **3** | strace | ptrace、系统调用、信号 | 3-5 天 |
| **4** | perf | PMU、perf_event、采样 | 1-2 周 |
| **5** | BPF (bcc + bpftrace) | BPF 虚拟机、kprobe、tracepoint | 2-4 周 |

## 快速开始

```bash
# 下载所有源码
git clone git@github.com:1020xyr/trace-lab.git
cd trace-lab
# 源码在 src/ 目录中（需要通过 .gitignore 中的说明自行克隆）

# 查看 blktrace 学习路线
cat docs/blktrace/learning-roadmap.md
```

## 博客

配套博客：**探针手记 (Probe Notes)**

---

*始于一次 FUA 标志位的遗漏，终于对每个工具的源码级理解。*
