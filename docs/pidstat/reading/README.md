# pidstat 阅读指南

> ★ **pidstat 是 Linux 进程级性能监控的核心工具** — 从 /proc 文件系统拆解每个进程的 CPU/I/O/内存/上下文切换。
> 在性能分析方法论中，pidstat 位于"系统级发现 → 进程级定位"的关键转折点。

本目录包含 pidstat 的工作原理、命令参考和实战输出解析。
按 Step 1 → 3 的顺序阅读。

**预计总阅读时间：** 2-3 小时（快速入门 20 分钟，深入理解 1.5 小时）

---

## ★ pidstat 在性能分析中的定位

```
┌─────────────────────────────────────────────────────────────────┐
│                    性能诊断工具链                                 │
│                                                                  │
│  第 1 层：系统级发现（有异常）                                    │
│    sar -u → CPU 高    iostat → I/O 高    free → 内存紧          │
│         │                    │                   │               │
│         ▼                    ▼                   ▼               │
│  第 2 层：进程级定位 ← ★ pidstat 在这里                         │
│    pidstat -u → 哪个进程占 CPU                                  │
│    pidstat -d → 哪个进程在做 I/O                                │
│    pidstat -r → 哪个进程在吃内存                                │
│    pidstat -w → 哪个进程在等锁/等 CPU                           │
│         │                                                        │
│         ▼                                                        │
│  第 3 层：深入分析（为什么）                                      │
│    perf → 函数级热点    blktrace → I/O 详情    strace → 系统调用│
└─────────────────────────────────────────────────────────────────┘
```

---

## 阅读路线

| Step | 文件 | 内容 | 行数 | 关注点 | 预计时间 |
|------|------|------|------|--------|---------|
| **1** | [01_pidstat_architecture.md](./01_pidstat_architecture.md) | pidstat 工作原理 | ~180 | ★ /proc 数据来源、采样差值机制、各指标的内核来源 | 20 分钟 |
| **2** | [02_command_reference.md](./02_command_reference.md) | 完整命令参考 | ~440 | ★ 所有参数详解、12 场景命令组合、字段速查表、5 个常见陷阱 | 25 分钟 |
| **3** | [03_command_output_demo.md](./03_command_output_demo.md) | 输出实战解析 | ~750 | ★★ 真实命令输出、逐行注解、9 个场景、与 blktrace 关系 | 40 分钟 |

---

## 阅读建议

1. **先通读 Step 1**：理解 pidstat 的数据来源 — 它不采集数据，只是 /proc 的格式化视图
2. **Step 2 当手册用**：遇到不熟悉的参数时查阅
3. **★ Step 3 是核心**：结合真实输出理解每个字段的含义（9 个场景），这是最有价值的部分

---

## 阅读路径推荐

### 快速入门（20 分钟）

```
01_pidstat_architecture → 理解数据来源和 /proc 映射
03_command_output_demo  → 场景 1（CPU）+ 场景 2（I/O）
```

### 深入理解（1.5 小时）

```
01 → 02（速查）→ 03（全部 9 个场景）
```

---

## ★ 核心指标速查

| 维度 | 选项 | 关键指标 | 诊断价值 |
|------|------|---------|---------|
| CPU | `-u` | %usr, %system, ★%wait | %wait 高 = 进程排队等 CPU（pidstat 独有！） |
| I/O | `-d` | kB_rd/s, kB_wr/s, iodelay | 找到 I/O 最密集的进程 |
| 内存 | `-r` | minflt/s, majflt/s, RSS | majflt/s > 0 = 磁盘缺页（内存压力） |
| 切换 | `-w` | cswch/s, nvcswch/s | nvcswch/s 高 = CPU 竞争激烈 |

---

## ★ 与其他工具的交叉引用

### pidstat + perf — CPU 诊断完整链路

```
★ 这是 CPU 高占用诊断的标准流程

1. pidstat -u 1 → 找到 %CPU 最高的进程
   │
   ├── %usr 高 → 用户态计算密集
   ├── %system 高 → 内核态开销大
   └── %wait 高 → CPU 排队严重
   │
   ▼
2. pidstat -w → 看上下文切换
   ├── cswch/s 高 → I/O 等待或锁等待
   └── nvcswch/s 高 → CPU 时间片竞争
   │
   ▼
3. perf record -p <PID> -g -- sleep 10 → 函数级热点分析
   → 火焰图定位 CPU 消耗在哪个函数
```

### pidstat + iostat — I/O 进程定位

```
1. iostat -x 1 → 发现设备级 await 高或 %util 高
2. pidstat -d 1 → 找到 kB_wr/s 或 kB_rd/s 最高的进程
3. 两者对照 → 确认哪个进程导致了设备级 I/O 压力
```

### pidstat + blktrace — I/O 深入分析

```
1. pidstat -d → 找到 I/O 最密集的进程
2. blktrace -d /dev/vdb → 追踪该进程的每个 I/O 事件
3. btt 分析 → Q2D 高（软件瓶颈）还是 D2C 高（硬件瓶颈）
```

### pidstat + sar — 趋势 + 定位

```
1. sar -u → 发现历史上 CPU 异常时段（宏观趋势）
2. pidstat -u → 当前时刻哪个进程占 CPU（实时定位）
3. sar -u -f /var/log/sa/saXX → 事后回溯分析
```

---

## 文件说明

- 本目录不包含源码摘录（pidstat 的数据来源是 /proc 文件系统，不是复杂的内核代码路径）
- 所有输出均来自真实执行，逐行注解每个字段
- 重点关注"间隔采样"模式下的差值计算逻辑
- ★ 标记核心重点，表格用于字段对照和参数速查

---

## 相关资源

- [learning-roadmap.md](../learning-roadmap.md) — 完整学习路线和实验清单
- [QA.md](../QA.md) — 常见问题问答集
- [perf reading](../perf/reading/) — ★ 深入分析必读：从进程级定位到函数级热点
- [iostat reading](../iostat/reading/) — 设备级 I/O 观测：与 pidstat -d 配合
- [blktrace reading](../blktrace/reading/) — I/O 微观追踪：每个 I/O 的延迟拆解
- [sar reading](../sar/reading/) — 宏观趋势发现：从历史数据中发现异常
