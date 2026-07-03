# trace-lab · 探针手记

> 从源码出发，深入理解 Linux 性能工具

## 项目简介

这是一个系统学习 Linux 性能/调试工具的实践仓库。通过**下载源码 → 对照阅读 → 动手实验**的方式，
深入理解每个工具的采集机制、内核 hook 点、数据格式与解析逻辑。

## 目录结构

```
trace-lab/
├── README.md                      # 本文件
├── src/                           # 第三方源码（仅本地使用）
│   ├── blktrace/                  # blktrace 用户态工具（blktrace、blkparse、btt）
│   ├── linux-5.10/                # Linux 内核 5.10 源码
│   │   ├── kernel/trace/          #   ← 内核 blktrace.c / BPF 子系统
│   │   ├── block/                 #   ← 块设备驱动层 / blk-flush
│   │   ├── include/trace/events/  #   ← tracepoint 定义（block.h 等）
│   │   └── tools/perf/            #   ← perf 工具源码
│   ├── fio/                       # fio 灵活 I/O 测试工具
│   ├── strace/                    # strace 系统调用追踪
│   ├── bcc/                       # BPF Compiler Collection
│   ├── bpftrace/                  # bpftrace DTrace 风格脚本工具
│   └── libbpf/                    # libbpf BPF 加载库
│
├── docs/                          # ★ 学习笔记与分析文档
│   │
│   ├── ★ methodology.md           # 性能分析方法论（逐层排除法、USE 方法）
│   ├── ★ tool_selection_guide.md  # 工具选型指南（根据问题选工具组合）
│   ├── ★ cross_tool_diagnosis.md  # 跨工具诊断工作流（5 大实战场景）
│   │
│   ├── blktrace/                  # 各工具学习目录（17 个）
│   │   ├── learning-roadmap.md    #   学习路线图
│   │   ├── reading/               #   带中文注释的源码阅读材料
│   │   ├── QA.md                  #   问答集（持续追加）
│   │   └── lab/                   #   实战实验报告
│   ├── fio/
│   ├── vmstat/
│   ├── sar/
│   ├── iostat/
│   ├── pidstat/
│   ├── ps/
│   ├── ss/
│   ├── netstat/
│   ├── strace/
│   ├── perf/
│   ├── topdown/
│   ├── cpu_arch/
│   ├── bpf/                       #   bcc + bpftrace
│   ├── nginx/
│   ├── fuse/
│   └── kernel_internals/          #   SLUB / page_alloc / scheduler / network
│
└── experiments/                   # 实验脚本
    └── blktrace/
```

## 学习路线

按推荐顺序排列。阶段 0 的方法论是全局导航图，建议最先阅读。

| 阶段 | 工具/领域 | 核心概念 | 预计时间 |
|------|----------|---------|---------|
| **0** | [methodology.md](docs/methodology.md) | 逐层排除法、USE 方法、瓶颈分类 | 1 天 |
| **1** | [blktrace + blkparse + btt](docs/blktrace/) | tracepoint、relay buffer、blk_io_trace | 1-2 周 |
| **2** | [fio](docs/fio/) | I/O 引擎、queue depth、direct I/O | 3-5 天 |
| **3** | [vmstat](docs/vmstat/) | 宏观系统概览、瓶颈方向判断 | 1 天 |
| **4** | [sar](docs/sar/) | 系统活动报告、历史数据分析 | 2 天 |
| **5** | [iostat](docs/iostat/) | 磁盘 I/O 统计、%util/await 诊断 | 2 天 |
| **6** | [pidstat](docs/pidstat/) | 进程级资源监控 | 2 天 |
| **7** | [ps](docs/ps/) | 进程状态机、D/Z 状态排查 | 1 天 |
| **8** | [ss / netstat](docs/ss/) | 网络连接分析、TCP 状态机 | 2 天 |
| **9** | [strace](docs/strace/) | ptrace 机制、系统调用追踪 | 3-5 天 |
| **10** | [perf](docs/perf/) | PMU、硬件计数器、热点分析 | 1-2 周 |
| **11** | [topdown](docs/topdown/) | CPU 微架构瓶颈分类 | 3-5 天 |
| **12** | [cpu_arch](docs/cpu_arch/) | CPU 架构、AMD CCD、缓存层次 | 1 周 |
| **13** | [bpf (bcc + bpftrace)](docs/bpf/) | BPF 虚拟机、kprobe、tracepoint | 2-4 周 |
| **14** | [nginx](docs/nginx/) | 事件驱动、worker 模型、性能调优 | 3 天 |
| **15** | [fuse](docs/fuse/) | 用户态文件系统、/dev/fuse 协议 | 3 天 |
| **16** | [kernel_internals](docs/kernel_internals/) | SLUB / page_alloc / scheduler / network | 2-4 周 |

> **跨工具参考：** [cross_tool_diagnosis.md](docs/cross_tool_diagnosis.md)（5 大场景诊断工作流） · [tool_selection_guide.md](docs/tool_selection_guide.md)（根据问题选工具）

## 快速开始

```bash
# 1. 克隆仓库
git clone git@github.com:1020xyr/trace-lab.git
cd trace-lab

# 2. 先读方法论——建立全局分析框架
cat docs/methodology.md

# 3. 选一个工具开始深入（推荐从 blktrace 起步）
cat docs/blktrace/learning-roadmap.md

# 4. 遇到问题？查工具选型指南
cat docs/tool_selection_guide.md
```

## 仓库统计

| 指标 | 数量 |
|------|------|
| 工具/领域覆盖 | **17** 个 |
| 源码阅读材料（reading/） | **113** 篇 |
| 问答条目（QA.md） | **56** 条 |
| 学习路线图 | **17** 份 |
| 实战实验（lab/） | **5** 份 |
| 跨工具文档 | **3** 篇（方法论 + 诊断工作流 + 选型指南） |
| 文档总文件数 | **154** |

## 博客

配套博客：**探针手记 (Probe Notes)**

---

*始于一次 FUA 标志位的遗漏，终于对每个工具的源码级理解。*
