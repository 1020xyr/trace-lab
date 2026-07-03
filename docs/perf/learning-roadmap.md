# perf 学习路线

> 从 PMU 硬件计数器到 perf.data，从采样到热点分析

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 源码阅读路线图](#2-源码阅读路线图)
  - [Step 1: 数据结构 — perf_event_attr](#step-1-数据结构--perf_event_attr)
  - [Step 2: perf record — 数据采集核心流程](#step-2-perf-record--数据采集核心流程)
  - [Step 3: perf report — 解析与热点分析](#step-3-perf-report--解析与热点分析)
  - [Step 4: PMU 硬件事件 — cycles/instructions/cache-miss](#step-4-pmu-硬件事件--cyclesinstructionscache-miss)
  - [Step 5: perf script/trace — 事件流与系统调用追踪](#step-5-perf-scripttrace--事件流与系统调用追踪)
- [3. 核心概念](#3-核心概念)
- [4. 动手实验清单](#4-动手实验清单)
- [5. 关键源码文件索引](#5-关键源码文件索引)
- [6. 参考资源](#6-参考资源)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     用户态工具                               │
│  perf record(采集) → perf report(分析) → perf script(导出)   │
│  perf stat(计数)   → perf top(实时)    → perf trace(strace)  │
├─────────────────────────────────────────────────────────────┤
│                     系统调用接口                              │
│  perf_event_open()  +  mmap()  +  ioctl(ENABLE/DISABLE)     │
├─────────────────────────────────────────────────────────────┤
│                     内核 perf 子系统                         │
│  PMU 硬件计数器 / 软件事件 / tracepoint                      │
│  perf_event → ring buffer (per-CPU) → perf.data             │
└─────────────────────────────────────────────────────────────┘
```

### 数据流全路径

```
 用户程序执行（如 dd / fio / 你的应用）
     │
     ▼
 perf record 解析命令行参数
     │
     ▼
 构造 struct perf_event_attr
     │
     ├── type = PERF_TYPE_HARDWARE / SOFTWARE / TRACEPOINT
     ├── config = 具体事件（cycles / task-clock / sched:sched_switch）
     └── sample_type = IP | TID | TIME | CALLCHAIN
     │
     ▼
 ┌── perf_event_open() ──────────────────────────┐
 │  内核分配 perf_event 结构                       │
 │  绑定到指定 CPU / 线程                          │
 │  配置 PMU 或软件计数器                          │
 └───────────────────────────────────────────────┘
     │
     ▼
 ┌── mmap() 映射 ring buffer ────────────────────┐
 │  per-CPU 环形缓冲区（默认 128 页 = 512KB）      │
 │  内核写入采样数据 → 用户态读取                    │
 └───────────────────────────────────────────────┘
     │
     ▼
 计数溢出 → 内核记录 IP/TID/时间戳/调用栈
     │
     ▼
 perf record 主循环：poll() + mmap 读取
     │
     ▼
 写入 perf.data（二进制文件）
     │
     ├── 文件头：环境信息（CPU、OS、命令行）
     ├── 合成事件：MMAP / COMM / FORK / EXEC
     └── 采样事件：PERF_RECORD_SAMPLE
     │
     ▼
 perf report 读取 perf.data
     │
     ▼
 ┌── perf_session__process_events() ─────────────┐
 │  解析每个事件 → 构建直方图（hist_entry）          │
 │  按 symbol/dso/comm 聚合 → 排序 → 输出           │
 └───────────────────────────────────────────────┘
     │
     ▼
 热点报告：Overhead% / Symbol / Callchain
```

---

## 2. 源码阅读路线图

### Step 1: 数据结构 — perf_event_attr

**目标：** 理解 perf 事件配置的"宪法"——所有采样行为都由此结构体决定

**文件位置：**
- 内核定义：`src/linux-5.10/include/uapi/linux/perf_event.h`

**阅读要点：**

```c
struct perf_event_attr {
    __u32 type;          // 事件大类：硬件/软件/tracepoint/缓存/原始
    __u32 size;          // 结构体大小（用于前后兼容）
    __u64 config;        // 具体事件编号
    union {
        __u64 sample_period;  // 采样周期（每 N 个事件采一次）
        __u64 sample_freq;    // 采样频率（每秒采 N 次）
    };
    __u64 sample_type;   // 采样时记录哪些信息（IP/TID/时间/调用栈）
    // ★ 位域标志：disabled / inherit / exclude_user / exclude_kernel / ...
};
```

**思考题：**
1. `sample_period` 和 `sample_freq` 有什么区别？（period 是固定计数，freq 由内核动态调整）
2. `exclude_kernel = 1` 后 perf report 还能看到内核函数吗？（不能，除非 perf_event_paranoid ≤ 1）
3. `PERF_TYPE_HW_CACHE` 的 config 编码方式是什么？（cache_id | cache_op << 8 | cache_result << 16）

**实操：**
```bash
perf record -e task-clock -c 100000 -- dd if=/dev/zero of=/dev/null bs=1M count=100
perf report --header --stdio  # 查看 attr 的实际配置
```

---

### Step 2: perf record — 数据采集核心流程

**目标：** 理解从命令行到 perf.data 文件的完整流程

**文件位置：**
- 入口：`src/linux-5.10/tools/perf/builtin-record.c` — `cmd_record()` / `__cmd_record()`

**阅读要点：**

```
cmd_record()
  │
  ├── 1. 解析命令行参数 → struct record + record_opts
  │
  ├── 2. record__open()
  │     ├── 配置 perf_event_attr（evlist__config）
  │     ├── evsel__open() → perf_event_open() 系统调用
  │     └── record__mmap() → mmap() 映射 ring buffer
  │
  ├── 3. record__synthesize() → 写入合成事件（MMAP/COMM/FORK）
  │
  ├── 4. 主循环：
  │     ├── record__mmap_read_all() → 从 ring buffer 读取采样
  │     ├── record__write() → 写入 perf.data
  │     └── evlist__poll() → 等待新数据
  │
  └── 5. 停止 → 写文件尾 → 关闭 session
```

**思考题：**
1. 为什么需要 dummy event？（系统宽模式需要跟踪 MMAP 事件以解析符号）
2. `record__mmap_read_all()` 为什么用 per-CPU 循环？（每个 CPU 有独立 ring buffer）

---

### Step 3: perf report — 解析与热点分析

**目标：** 理解 perf.data 如何变成热点报告

**文件位置：**
- 入口：`src/linux-5.10/tools/perf/builtin-report.c` — `cmd_report()` / `__cmd_report()`

**阅读要点：**

```
cmd_report()
  │
  ├── 1. 打开 perf.data → perf_session__new()
  │
  ├── 2. __cmd_report()
  │     ├── report__setup_sample_type() → 确认采样类型
  │     ├── perf_session__process_events() → 遍历所有事件
  │     │     └── 对每个 SAMPLE 事件 → 构建 hist_entry
  │     ├── report__collapse_hists() → 合并相同符号的计数
  │     └── report__output_resort() → 按 overhead% 排序
  │
  └── 3. report__browse_hists() → 输出结果
```

**思考题：**
1. `hist_entry` 包含哪些关键字段？（IP、symbol、dso、comm、period）
2. `--no-children` 和默认的 children 模式有什么区别？（children 累加子函数的开销）

---

### Step 4: PMU 硬件事件 — cycles/instructions/cache-miss

**目标：** 理解硬件性能计数器如何工作

**文件位置：**
- 事件定义：`src/linux-5.10/include/uapi/linux/perf_event.h` — `enum perf_hw_id`
- PMU 驱动：`src/linux-5.10/arch/x86/events/` （Intel/AMD 架构相关）

**阅读要点：**

```
PMU（Performance Monitoring Unit）是 CPU 内部的专用硬件：

┌─────────────────────────────────────────────┐
│              CPU Core                        │
│  ┌─────────┐  ┌──────────┐  ┌────────────┐ │
│  │  ALU    │  │ L1 Cache │  │ 分支预测器  │ │
│  └────┬────┘  └────┬─────┘  └─────┬──────┘ │
│       │            │              │         │
│  ┌────▼────────────▼──────────────▼───────┐ │
│  │        PMU (Performance Monitor Unit)   │ │
│  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  │ │
│  │  │ PMC0 │ │ PMC1 │ │ PMC2 │ │ PMC3 │  │ │ ← 通常 4-8 个通用计数器
│  │  └──────┘ └──────┘ └──────┘ └──────┘  │ │
│  └────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

**思考题：**
1. 为什么硬件 PMU 计数器数量有限？（硅片面积、功耗、多路复用）
2. 虚拟化环境下 PMU 不可用时用什么替代？（软件事件：task-clock / page-faults）

---

### Step 5: perf script/trace — 事件流与系统调用追踪

**目标：** 理解 perf 的原始事件导出和系统调用追踪能力

**文件位置：**
- `src/linux-5.10/tools/perf/builtin-script.c`
- `src/linux-5.10/tools/perf/builtin-trace.c`

**阅读要点：**
- `perf script`：逐条输出采样事件（IP、时间戳、调用栈）
- `perf trace`：类似 strace，追踪系统调用（基于 tracepoint `raw_syscalls:*`）

---

## 3. 核心概念

### 3.1 采样 vs 计数

```
采样模式（perf record）：
  每隔 N 个事件记录一次 IP/调用栈 → 生成热点分布图
  优点：知道"热点在哪里"
  缺点：有采样误差（统计估计）

计数模式（perf stat）：
  直接读取计数器总值 → 生成精确统计
  优点：精确数值
  缺点：不知道"热点在哪里"
```

### 3.2 事件类型

```
类型                    type 值      config 含义
────────────────────    ────────    ──────────────────────────
PERF_TYPE_HARDWARE      0           CPU cycles, instructions, cache-miss ...
PERF_TYPE_SOFTWARE      1           task-clock, page-faults, context-switches ...
PERF_TYPE_TRACEPOINT    2           内核 tracepoint ID（如 sched:sched_switch）
PERF_TYPE_HW_CACHE      3           L1-D/L1-I/LLC 缓存事件（编码格式）
PERF_TYPE_RAW           4           原始 PMU 事件编码（架构相关）
PERF_TYPE_BREAKPOINT    5           硬件断点（内存地址读/写/执行）
```

### 3.3 perf_event_open 系统调用

```c
int fd = perf_event_open(&attr, pid, cpu, group_fd, flags);
// pid = -1, cpu = N  → 监控系统宽指定 CPU
// pid = P, cpu = -1  → 监控指定进程（任意 CPU）
// pid = 0, cpu = -1  → 监控当前进程
```

### 3.4 Ring Buffer 机制

```
┌─── CPU 0 ────────────────────────────┐
│  ring buffer（默认 128 页 = 512KB）   │
│  ┌─────────────────────────────────┐ │
│  │ [采样1][采样2][采样3] ... [采样N] │ │
│  │     ↑ write          ↑ read    │ │
│  └─────────────────────────────────┘ │
└──────────────────────────────────────┘
┌─── CPU 1 ────────────────────────────┐
│  ring buffer（独立，无锁写入）         │
└──────────────────────────────────────┘
```

---

## 4. 动手实验清单

### 实验 1：perf stat 基础计数

```bash
# 用软件事件统计 dd 的性能特征
perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
    -- dd if=/dev/zero of=/dev/null bs=1M count=1000

# 多次运行取平均
perf stat -r 5 -e task-clock,context-switches,page-faults \
    -- dd if=/dev/zero of=/dev/null bs=1M count=200
```

### 实验 2：perf record + report 热点分析

```bash
# 采集（使用软件事件 task-clock）
perf record -e task-clock -c 100000 -g -- \
    dd if=/dev/zero of=/dev/null bs=1M count=500

# 分析报告
perf report --stdio --no-children
perf report --stdio --sort=dso     # 按共享库分组
```

### 实验 3：perf trace 系统调用追踪

```bash
# 追踪指定系统调用
perf trace -e read,write,openat,close -- \
    dd if=/dev/zero of=/dev/null bs=4K count=100
```

### 实验 4：perf script 事件流

```bash
# 先 record
perf record -e task-clock -c 100000 -g -- \
    dd if=/dev/zero of=/dev/null bs=1M count=200

# 逐条查看采样
perf script | head -60
```

### 实验 5：火焰图生成（需 FlameGraph 工具）

```bash
# 安装 FlameGraph
git clone https://github.com/brendangregg/FlameGraph

# 采集
perf record -e task-clock -g -F 99 -- dd if=/dev/zero of=/dev/null bs=1M count=200

# 生成折叠格式 → SVG
perf script | FlameGraph/stackcollapse-perf.pl | FlameGraph/flamegraph.pl > flame.svg
```

### 实验 6：全系统监控

```bash
# 监控整个系统 5 秒
perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
    -a -- sleep 5
```

---

## 4.5 进阶专题（性能诊断深入）

完成基础实验后，按以下顺序学习进阶专题：

### 专题 1：硬件计数器与性能诊断

**阅读材料：** `reading/06_hw_counters_diagnosis.md`

**核心技能：**
- 用 `perf stat -d` 获取 IPC、cache-miss 率、branch-miss 率
- 根据 IPC 和 cache-miss 率判断瓶颈类型
- 诊断 CPU 高占用低吞吐的完整流程

```bash
# 关键命令
perf stat -d -r 3 ./app                    # IPC + cache-miss + branch-miss
perf stat -e stalled-cycles-frontend,stalled-cycles-backend,cycles ./app
```

### 专题 2：perf c2c — False Sharing 检测

**阅读材料：** `reading/07_c2c_false_sharing.md`

**核心技能：**
- 理解 cache line 共享与 false sharing 的产生机制
- 用 `perf c2c record` + `perf c2c report` 检测 HITM
- 修复 false sharing（cache line 对齐）

### 专题 3：perf lock — 锁竞争分析

**阅读材料：** `reading/08_lock_contention.md`

**核心技能：**
- 用 `perf lock record` + `perf lock report` 分析锁争用
- 定位自旋锁热点（`native_queued_spin_lock_slowpath`）
- 锁竞争优化策略（减小临界区 / per-CPU / RCU）

### 专题 4：perf mem + AMD 特定事件

**阅读材料：** `reading/09_mem_and_amd.md`

**核心技能：**
- 用 `perf mem record` 分析内存访问延迟
- AMD IBS（Instruction Based Sampling）的使用
- AMD CCX/NUMA 拓扑对性能分析的影响

---

## 5. 关键源码文件索引

### 内核侧

| 文件 | 说明 | 推荐阅读顺序 |
|------|------|:-----------:|
| `include/uapi/linux/perf_event.h` | ★ perf_event_attr 结构体、事件枚举、ioctl 定义 | 1 |
| `kernel/events/core.c` | perf_event_open() 实现、ring buffer、事件调度 | 进阶 |
| `kernel/events/callchain.c` | 调用栈采集 | 进阶 |
| `arch/x86/events/core.c` | x86 PMU 驱动 | 进阶 |
| `arch/x86/events/intel/core.c` | Intel 特定 PMU 配置 | 进阶 |
| `include/linux/perf_event.h` | 内核内部 perf 接口 | 进阶 |

### 用户态

| 文件 | 说明 | 推荐阅读顺序 |
|------|------|:-----------:|
| `tools/perf/builtin-record.c` | ★ perf record 核心流程 | 2 |
| `tools/perf/builtin-report.c` | ★ perf report 解析流程 | 3 |
| `tools/perf/builtin-stat.c` | perf stat 计数模式 | 4 |
| `tools/perf/builtin-script.c` | perf script 事件导出 | 5 |
| `tools/perf/builtin-trace.c` | perf trace 系统调用追踪 | 5 |
| `tools/perf/builtin-top.c` | perf top 实时热点 | 进阶 |
| `tools/perf/util/evsel.c` | 事件选择（evsel）操作 | 进阶 |
| `tools/perf/util/evlist.c` | 事件列表（evlist）管理 | 进阶 |
| `tools/perf/util/mmap.c` | ring buffer 读取 | 进阶 |
| `tools/perf/util/session.c` | perf.data 会话管理 | 进阶 |
| `tools/perf/util/header.c` | perf.data 文件头读写 | 进阶 |
| `tools/perf/util/pmu.c` | PMU 事件解析 | 进阶 |

---

## 6. 参考资源

- Brendan Gregg: [Linux perf Profiling](https://www.brendangregg.com/linuxperf.html)
- perf wiki: https://perf.wiki.kernel.org/
- man perf_event_open: 系统调用详细文档
- Intel SDM Volume 3B Chapter 18: Performance Monitoring
