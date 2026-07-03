# sar / sysstat 源码阅读指南

> ★ **sar 是 Linux 系统级性能监控的"雷达"** — 持续记录 CPU/内存/I/O/网络全维度数据，是事后回溯和趋势分析的首选工具。
> 在性能分析方法论中，sar 位于"长期监控 → 异常发现 → 工具联动"链路的核心位置。

本目录包含 sar（sysstat）数据采集与查询的学习材料，每个文件都带有详细中文注释。
按 Step 1 → 4 的顺序阅读。

**预计总阅读时间：** 3.5-5 小时（快速入门 30 分钟，深入理解 3 小时）

---

## ★ sar 在性能分析中的定位

```
┌─────────────────────────────────────────────────────────────────┐
│                    系统级性能监控全景                             │
│                                                                  │
│  ★ sar 是系统级"雷达" — 发现异常维度                            │
│    sar -u → CPU 异常？     → 用 perf/pidstat 深入               │
│    sar -r → 内存紧张？     → 用 pidstat -r 定位进程             │
│    sar -d → I/O 瓶颈？     → 用 iostat/blktrace 深入           │
│    sar -n DEV → 网络异常？ → 用 ss/netstat/mpstat 深入          │
│    sar -q → 队列堆积？     → 用 pidstat -w 分析调度             │
│                                                                  │
│  核心价值：                                                      │
│    1. 持续记录 → 事后回溯（"昨天凌晨 3 点发生了什么？"）         │
│    2. 全维度覆盖 → 不遗漏任何异常维度                            │
│    3. 低开销 → sadc 二进制采集，几乎无性能影响                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 | 预计时间 |
|------|------|------|------|--------|---------|
| **1** | [01_sar_architecture.md](./01_sar_architecture.md) | sysstat 源码 + 系统配置 | 450+ | ★ sadc/sa1/sa2/sar 的关系、sa 二进制文件格式、systemd 定时采集 | 30 分钟 |
| **2** | [02_command_reference.md](./02_command_reference.md) | sar 命令手册 | 710+ | ★ 按维度分类的参数速查：CPU/内存/I/O/网络/队列/中断 | 35 分钟 |
| **3** | [03_command_output_demo.md](./03_command_output_demo.md) | 实际执行输出 | 880+ | ★★ 19 个场景的真实 sar 输出逐列注解 | 50 分钟 |
| **4** | [04_diagnostic_workflow.md](./04_diagnostic_workflow.md) | ★ 综合文档 | 300+ | ★★ 性能诊断工作流：CPU 高占用低吞吐排查、网络软中断不均衡、工具联动 | 35 分钟 |

---

## 阅读建议

1. **先读 Step 1**：理解 sadc → sa 文件 → sar/sadf 的完整数据流
2. **Step 2 当速查表**：按维度（CPU/内存/I/O/网络）分类记忆参数
3. **★ Step 3 看真实输出**：每个命令实际执行，理解每列的含义和正常/异常范围
4. **★★ Step 4 实战诊断**：从 sar 发现异常到用专业工具深入定位的完整流程

---

## 阅读路径推荐

### 快速入门（30 分钟）

```
01_sar_architecture     → 理解 sadc/sa 文件/sar 的数据流
03_command_output_demo  → 场景 1（sar -u CPU）+ 场景 3（sar -d I/O）
```

### 深入理解（3 小时）

```
01 → 02（速查）→ 03（重点场景）→ 04（诊断工作流）
```

### 完整学习（半天）

```
01 → 02 → 03（全部 19 场景）→ 04
+ learning-roadmap.md 中的实验动手操作
```

---

## ★ 核心指标异常阈值

| 指标 | 命令 | 正常值 | 异常值 | 深入工具 |
|------|------|--------|--------|---------|
| %iowait | `sar -u` | < 5% | > 10% | → iostat/blktrace |
| %system | `sar -u` | < 15% | > 30% | → perf/pidstat |
| %soft | `sar -u ALL` | < 5% | > 10% | → mpstat/proc/interrupts |
| %memused | `sar -r` | < 80% | > 90% | → pidstat -r |
| majflt/s | `sar -B` | 0 | > 0 | → pidstat -r |
| await | `sar -d -p` | < 10ms | > 20ms | → iostat/blktrace |
| %util | `sar -d -p` | < 80% | > 90% | → iostat/blktrace |
| runq-sz | `sar -q` | < CPU 核数 | > CPU 核数 | → pidstat -w |
| rxdrop/s | `sar -n EDEV` | 0 | > 0 | → ss/mpstat |

---

## ★ 与其他工具的交叉引用

### sar → iostat/blktrace — I/O 异常深入

```
★ sar -d 发现 I/O 异常后的标准诊断流程

1. sar -d → 发现 await 高或 %util 高（历史趋势）
   │
   ▼
2. iostat -x 1 → 实时确认当前 I/O 状态
   │
   ├── %util ≈ 100% → 设备饱和
   ├── await > 20ms → I/O 延迟大
   └── avgqu-sz > 32 → 队列堆积
   │
   ▼
3. blktrace + btt → 微观分析 Q2D/D2C
   ├── Q2D 高 → 软件层瓶颈
   └── D2C 高 → 硬件层瓶颈
```

### sar → pidstat — 进程级定位

```
1. sar -u → 发现 %user 或 %system 持续高
2. pidstat -u 1 → 找到 %CPU 最高的进程
3. pidstat -w → 看上下文切换（cswch/s、nvcswch/s）
4. perf record → 函数级热点分析
```

### sar → perf — CPU 微架构分析

```
1. sar -u → 发现 %user 高但业务吞吐低（CPU 高占用低吞吐）
2. perf stat → 测量 IPC、cache-miss 率
3. perf record → 火焰图定位热点函数
4. perf c2c → 检测 false sharing
```

### sar → ss/mpstat — 网络异常深入

```
1. sar -n DEV → 发现网络流量异常
2. sar -n EDEV → 检查丢包/错误
3. ss -tna → 检查 TCP 连接状态
4. mpstat -P ALL → 检查软中断分布
```

---

## 文件说明

- 所有文件均为 markdown 格式（非源码注释），因为 sar 的核心逻辑在 C 二进制中
- 重点分析 sadc 的采集机制和 sa 文件的二进制格式
- 每个命令都实际执行，输出真实数据并逐列注解
- 用 ★ 标记核心概念和关键指标
- sar 是系统级"雷达"——发现异常维度，然后用 perf/bpftrace/iostat 等深入

---

## 相关资源

- [learning-roadmap.md](../learning-roadmap.md) — 完整学习路线和实验清单
- [QA.md](../QA.md) — 常见问题问答集
- [iostat reading](../iostat/reading/) — ★ I/O 深入分析：sar -d 发现异常后用 iostat 实时确认
- [pidstat reading](../pidstat/reading/) — ★ 进程定位：sar 发现异常后用 pidstat 找进程
- [perf reading](../perf/reading/) — CPU 微架构分析：sar -u 发现高 CPU 后用 perf 定位根因
- [blktrace reading](../blktrace/reading/) — I/O 微观追踪：blktrace Q2D/D2C 延迟拆解
- [netstat reading](../netstat/reading/) — 网络诊断：sar -n 发现异常后用 ss 检查连接
