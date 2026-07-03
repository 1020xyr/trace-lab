# pidstat 学习路线

> 从 /proc 文件系统到逐进程性能画像，理解 Linux 进程级监控的核心工具

---

## 目录

- [1. pidstat 是什么](#1-pidstat-是什么)
- [2. 与 top / ps 的区别](#2-与-top--ps-的区别)
- [3. 核心概念](#3-核心概念)
- [4. 学习路线图](#4-学习路线图)
  - [Step 1: 理解数据来源 — /proc/[pid]/ 文件系统](#step-1-理解数据来源--procpid-文件系统)
  - [Step 2: CPU 统计 — pidstat -u](#step-2-cpu-统计--pidstat--u)
  - [Step 3: I/O 统计 — pidstat -d](#step-3-io-统计--pidstat--d)
  - [Step 4: 内存统计 — pidstat -r](#step-4-内存统计--pidstat--r)
  - [Step 5: 上下文切换 — pidstat -w](#step-5-上下文切换--pidstat--w)
  - [Step 6: 间隔采样与持续监控](#step-6-间隔采样与持续监控)
  - [Step 7: 线程级统计与过滤](#step-7-线程级统计与过滤)
  - [Step 8: 综合分析与实战场景](#step-8-综合分析与实战场景)
- [5. 动手实验清单](#5-动手实验清单)
- [6. 参考资源](#6-参考资源)

---

## 1. pidstat 是什么

**pidstat** 是 sysstat 工具包中的进程级性能监控工具，用于报告**每个进程（或线程）** 的 CPU、内存、I/O、上下文切换等统计信息。

```
┌─────────────────────────────────────────────────────────────┐
│                     用户态工具                               │
│  pidstat（读取 /proc 并计算差值，输出格式化报告）              │
├─────────────────────────────────────────────────────────────┤
│                     /proc 文件系统                           │
│  /proc/[pid]/stat     → CPU 时间、页错误、上下文切换          │
│  /proc/[pid]/status   → 内存用量（VmSize/VmRSS）            │
│  /proc/[pid]/io       → I/O 字节数（read_bytes/write_bytes） │
│  /proc/[pid]/smaps    → 详细内存映射（可选）                  │
├─────────────────────────────────────────────────────────────┤
│                     内核进程调度器                            │
│  task_struct → utime/stime/min_flt/maj_flt/nvcsw/nivcsw     │
└─────────────────────────────────────────────────────────────┘
```

### 数据流全路径

```
 进程运行（内核维护 task_struct）
     │
     ▼
 内核将统计写入 /proc/[pid]/ 下各文件
     │
     ├── /proc/[pid]/stat      ← CPU 时间（utime/stime）、页错误
     ├── /proc/[pid]/status    ← 内存（VmSize/VmRSS）
     ├── /proc/[pid]/io        ← I/O 字节数
     └── /proc/[pid]/schedstat ← 调度延迟（wait time）
     │
     ▼
 pidstat 周期性读取 /proc/[pid]/* 文件
     │
     ▼
 计算两次采样的差值 → 得到"每秒速率"
     │
     ▼
 格式化输出到终端
```

---

## 2. 与 top / ps 的区别

| 特性 | pidstat | top | ps |
|------|---------|-----|-----|
| **数据来源** | /proc/[pid]/ 文件 | /proc + 系统调用 | /proc 快照 |
| **输出模式** | 逐进程逐指标的表格 | 实时交互式界面 | 一次性快照 |
| **间隔采样** | ✅ 支持（`interval count`） | ✅ 内置刷新 | ❌ 仅快照 |
| **I/O 统计** | ✅ 逐进程 kB_rd/s、kB_wr/s | ❌ 无 | ❌ 无 |
| **上下文切换** | ✅ 逐进程 cswch/s、nvcswch/s | ❌ 无 | ❌ 无 |
| **内存页错误** | ✅ minflt/s、majflt/s | ❌ 无 | ❌ 无 |
| **线程级** | ✅ `-t` 显示 TID | 部分 | ✅ `-L` |
| **命令过滤** | ✅ `-C` 正则匹配 | 交互式搜索 | `grep` 管道 |
| **脚本友好** | ✅ 纯文本输出 | ❌ curses 界面 | ✅ 但无间隔 |
| **CPU 分维度** | ✅ %usr/%system/%guest/%wait | ✅ 但维度少 | ❌ 仅 %CPU |
| **累积报告** | ✅ `Average:` 行 | ❌ 无 | ❌ 无 |

**核心优势：** pidstat 是唯一一个同时提供 **I/O + 上下文切换 + 页错误 + CPU 分维度** 的命令行工具，且天然适合脚本和自动化。

---

## 3. 核心概念

### 3.1 采样差值 vs 累积值

pidstat 有两种工作模式：

```
模式 1：无间隔参数（默认）
  → 报告从系统启动到当前的累积平均值
  → 类似 "cat /proc/[pid]/stat" 的格式化版本

模式 2：指定间隔（pidstat 2 5）
  → 每隔 N 秒采样一次，计算差值
  → 输出的是"这段时间内的速率"
  → 最后输出 Average: 汇总行
```

### 3.2 CPU 维度

```
%usr    → 用户态 CPU 时间占比（不含 nice）
%system → 内核态 CPU 时间占比
%guest  → 虚拟机中客户操作系统占用的 CPU 时间
%wait   → 进程等待 CPU 的时间占比（★ pidstat 独有！）
%CPU    → %usr + %system + %guest 的总和
CPU     → 进程运行在哪个 CPU 上
```

**%wait 的价值：** 这个指标在 top/ps 中看不到。如果一个进程 `%wait` 很高，说明它经常被调度器排队等待——系统 CPU 可能过载。

### 3.3 I/O 维度

```
kB_rd/s   → 进程读取速率（KB/s），来自 /proc/[pid]/io 的 read_bytes
kB_wr/s   → 进程写入速率（KB/s），来自 /proc/[pid]/io 的 write_bytes
kB_ccwr/s → 取消的写入（KB/s），来自 cancelled_write_bytes
iodelay   → I/O 延迟（ms），进程等待 I/O 完成的时间
```

### 3.4 内存维度

```
minflt/s → 次页错误率（次/秒），从内存中分配页面（无磁盘 I/O）
majflt/s → 主页错误率（次/秒），需要从磁盘加载页面（有磁盘 I/O）
VSZ      → 虚拟内存大小（KB），包含所有映射（含共享库、mmap）
RSS      → 常驻内存大小（KB），实际占用的物理内存
%MEM     → RSS 占总物理内存的百分比
```

### 3.5 上下文切换维度

```
cswch/s  → 自愿上下文切换率（voluntary context switch）
           进程主动让出 CPU（如 sleep、wait、I/O 阻塞）
nvcswch/s → 非自愿上下文切换率（involuntary context switch）
           进程被调度器强制换下（时间片用完、高优先级抢占）
```

---

## 4. 学习路线图

### Step 1: 理解数据来源 — /proc/[pid]/ 文件系统

**目标：** 理解 pidstat 的每一个数据字段来自哪个 /proc 文件

**关键文件：**

```
/proc/[pid]/stat        → CPU 时间（utime, stime）、页错误（minflt, majflt）
                          上下文切换需从 /proc/[pid]/status 读取

/proc/[pid]/status      → VmSize, VmRSS, VmPeak（内存）
                          voluntary_ctxt_switches, nonvoluntary_ctxt_switches

/proc/[pid]/io          → read_bytes, write_bytes, cancelled_write_bytes
                          rchar, wchar, syscr, syscw

/proc/[pid]/schedstat   → 调度等待时间（pidstat 的 %wait 来源）
```

**实操：**
```bash
cat /proc/1/stat          # systemd 的 CPU 和页错误
cat /proc/1/io            # systemd 的 I/O 统计
cat /proc/1/status        # systemd 的内存和上下文切换
```

---

### Step 2: CPU 统计 — pidstat -u

**目标：** 理解每个进程的 CPU 使用模式

**阅读要点：** 见 `reading/03_command_output_demo.md` 场景 1

**关键认知：**
- `%usr + %system ≠ %CPU`（%CPU 不含 %guest，但包含 nice 值影响的时间）
- `%wait > 0` 说明进程在等待 CPU 资源
- 内核线程（kworker、ksoftirqd 等）只有 `%system`

---

### Step 3: I/O 统计 — pidstat -d

**目标：** 找出哪个进程在消耗磁盘 I/O

**阅读要点：** 见 `reading/03_command_output_demo.md` 场景 2

**关键认知：**
- kB_rd/s 和 kB_wr/s 来自 `read_bytes` 和 `write_bytes`（实际到磁盘的字节）
- 不等于 rchar/wchar（rchar/wchar 包含缓存命中，不一定触发磁盘 I/O）
- `iodelay` 来自 `/proc/[pid]/schedstat` 中的 I/O 等待时间
- 如果 kB_wr/s 高但 iodelay=0，说明写入了 page cache（延迟写入）

---

### Step 4: 内存统计 — pidstat -r

**目标：** 理解进程的内存使用和页错误模式

**阅读要点：** 见 `reading/03_command_output_demo.md` 场景 3

**关键认知：**
- VSZ（虚拟内存）可能远大于 RSS（物理内存），特别是 mmap 了大量文件的进程
- minflt/s 高是正常的（从内存分配页面），majflt/s 高才是问题（需要磁盘 I/O）
- RSS 增长 + minflt/s 高 = 进程在分配更多内存

---

### Step 5: 上下文切换 — pidstat -w

**目标：** 分析进程的调度行为

**阅读要点：** 见 `reading/03_command_output_demo.md` 场景 4

**关键认知：**
- cswch/s 高 → 进程频繁 I/O 等待或 sleep（正常行为）
- nvcswch/s 高 → 进程被抢占（可能是 CPU 竞争激烈）
- 两者的比例可以判断是 I/O 密集型还是 CPU 密集型

---

### Step 6: 间隔采样与持续监控

**目标：** 掌握 pidstat 的实时监控能力

**用法：** `pidstat [选项] [间隔秒数] [采样次数]`

**阅读要点：** 见 `reading/03_command_output_demo.md` 场景 6

---

### Step 7: 线程级统计与过滤

**目标：** 使用 `-t`（线程）和 `-C`（命令名过滤）精准定位

**阅读要点：** 见 `reading/02_command_reference.md`

---

### Step 8: 综合分析与实战场景

**目标：** 组合多个选项做综合性能分析

**阅读要点：** 见 `reading/03_command_output_demo.md` 场景 5（综合画像）、场景 9（与 blktrace 的关系）

---

## 5. 动手实验清单

### 实验 1: 基础 CPU 监控

```bash
# 查看所有活跃进程的 CPU 使用
pidstat -u

# 间隔采样（每 2 秒，共 5 次）
pidstat -u 2 5

# 只看 claude 相关进程
pidstat -C claude -u
```

### 实验 2: I/O 热点进程定位

```bash
# 启动 I/O 密集任务
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/tmp/testfile --size=100M --runtime=5 --time_based &
FIO_PID=$!

# 追踪 fio 的 I/O
pidstat -d -p $FIO_PID 1 5
wait
```

### 实验 3: 内存泄漏检测

```bash
# 每 5 秒采样一次，观察 RSS 是否持续增长
pidstat -r -p <PID> 5 60
```

### 实验 4: 上下文切换分析

```bash
# 找出上下文切换最频繁的进程
pidstat -w 2 3 | sort -k4 -rn | head -20
```

### 实验 5: 综合分析

```bash
# CPU + I/O + 内存 + 上下文切换一次性看全
pidstat -u -d -r -w 2 5
```

---

## 6. 参考资源

- [sysstat GitHub](https://github.com/sysstat/sysstat)
- [pidstat(1) man page](https://man7.org/linux/man-pages/man1/pidstat.1.html)
- [proc(5) man page — /proc/[pid]/stat 字段说明](https://man7.org/linux/man-pages/man5/proc.5.html)
- [Linux Performance — Brendan Gregg](https://www.brendangregg.com/linuxperf.html)
- [sysstat 官方文档](https://sysstat.github.io/)
