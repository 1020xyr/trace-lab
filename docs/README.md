# trace-lab · 探针手记 — Linux 性能工具知识库

> ★ 从"系统变慢了"到"精确定位根因"的源码级学习路径  
> ★ 17 个工具/领域 · 154 个文件 · 81,000+ 行 · 52 个 QA 问答

---

## 目录

- [1. 知识库概览](#1-知识库概览)
- [2. 按角色快速导航](#2-按角色快速导航)
- [3. 按问题类型导航](#3-按问题类型导航)
- [4. 完整工具目录](#4-完整工具目录)
- [5. 学习路线图](#5-学习路线图)
- [6. 实验脚本索引](#6-实验脚本索引)
- [7. 全局 QA 索引](#7-全局-qa-索引)

---

## 1. 知识库概览

### 规模

| 指标 | 数值 |
|------|------|
| 覆盖工具/领域 | **17** 个 |
| 总文件数 | **154** 个 |
| 总行数 | **81,400+** 行 |
| QA 问答 | **52** 个问题 |
| 学习路线图 | **17** 份（每个工具一份） |
| 命令参考 | **16** 份 |
| 命令输出实战 | **16** 份（含真实输出 + 逐行注解） |
| 实验脚本 | **4** 个 `.sh` + **1** 个实验报告 |
| 文件类型 | 123 个 `.md` · 20 个 `.c` · 7 个 `.h` · 4 个 `.sh` · 1 个 `.cpp` |

### 覆盖的 17 个工具/领域

```
┌─ 系统观测 ─────────────────────────────────────────────┐
│  vmstat · iostat · sar · ps · pidstat                  │
├─ 内核追踪 ─────────────────────────────────────────────┤
│  blktrace · strace · perf · bpf/bpftrace               │
├─ 专项分析 ─────────────────────────────────────────────┤
│  fio · ss · netstat · topdown · cpu_arch               │
├─ 应用与子系统 ─────────────────────────────────────────┤
│  nginx · fuse · kernel_internals                       │
├─ 方法论 ───────────────────────────────────────────────┤
│  methodology（方法论） · cross_tool_diagnosis（跨工具诊断）│
└────────────────────────────────────────────────────────┘
```

### 每个工具的统一结构

```
docs/{tool}/
├── reading/           # 带中文注释的源码阅读材料
│   ├── 01_*.h/c/md    #   按学习顺序编号
│   ├── 02_*.h/c/md
│   ├── ...
│   ├── README.md      #   阅读指南
│   ├── command_reference.md      # 命令速查表
│   ├── command_output_demo.md    # 真实输出 + 逐行注解
│   └── learning-roadmap.md       # 学习路线图
├── lab/               # 实战实验报告 / 实验脚本
│   ├── vdb-lab.sh     #   可运行的实验脚本
│   └── vdb-lab-report.md  # 实验报告
└── QA.md              # 问答集（持续追加）
```

### 使用方法

**按角色进入** → 见第 2 节，找到你的角色，按推荐顺序学习  
**按问题进入** → 见第 3 节，找到你遇到的问题，沿推荐路径排查  
**按深度进入** → 见第 5 节，选择新手 / 进阶 / 专项路径  
**按工具进入** → 见第 4 节，直接跳转到对应工具的目录

---

## 2. 按角色快速导航

### ★ 我是 SRE / 运维工程师

> 目标：快速定位线上问题，建立系统化的观测能力

**推荐学习顺序：**

```
vmstat → iostat → ps → sar → ss → pidstat → netstat → strace
```

| 优先级 | 工具 | 学什么 | 解决什么问题 |
|--------|------|--------|-------------|
| ★★★ | [vmstat](./vmstat/) | r/b/w 列、si/so、cs/in | 系统整体健康度一键判断 |
| ★★★ | [iostat](./iostat/) | await、%util、r_await/w_await | 磁盘是否是瓶颈 |
| ★★★ | [ps](./ps/) | 进程状态、%CPU、%MEM | 哪个进程在捣乱 |
| ★★☆ | [sar](./sar/) | 历史数据回放、%system/%user | 事后分析"昨晚发生了什么" |
| ★★☆ | [ss](./ss/) | 连接状态、Recv-Q/Send-Q | TCP 连接堆积排查 |
| ★★☆ | [pidstat](./pidstat/) | 每进程/线程的 CPU、I/O、上下文切换 | 精准定位到线程级 |
| ★☆☆ | [netstat](./netstat/) | TIME_WAIT、连接统计 | 网络连接数异常 |
| ★☆☆ | [strace](./strace/) | 系统调用追踪 | 进程在干什么"卡住了" |

---

### ★ 我是性能工程师

> 目标：深入理解瓶颈根因，从微架构层面分析

**推荐学习顺序：**

```
methodology → perf → topdown → cpu_arch → kernel_internals → bpf
```

| 优先级 | 工具 | 学什么 | 解决什么问题 |
|--------|------|--------|-------------|
| ★★★ | [methodology](./methodology.md) | 逐层排除法、USE 方法 | 建立系统化的分析思维 |
| ★★★ | [perf](./perf/) | HW 计数器、火焰图、c2c | 精确定位热点函数和缓存行为 |
| ★★★ | [topdown](./topdown/) | TMAM 四层分类 | CPU 流水线效率分析 |
| ★★☆ | [cpu_arch](./cpu_arch/) | 微架构、L3 Cache、CCD 拓扑 | 理解硬件层面的性能特征 |
| ★★☆ | [kernel_internals](./kernel_internals/) | 调度器、内存分配器 | 内核态瓶颈的根因 |
| ★★☆ | [bpf](./bpf/) | bpftrace 动态追踪 | 自定义探针、锁竞争分析 |

---

### ★ 我是存储工程师

> 目标：理解 I/O 全链路，从块设备到文件系统

**推荐学习顺序：**

```
blktrace → fio → iostat → fuse → kernel_internals
```

| 优先级 | 工具 | 学什么 | 解决什么问题 |
|--------|------|--------|-------------|
| ★★★ | [blktrace](./blktrace/) | 块设备 I/O 追踪、Q2C 延迟 | I/O 延迟到底花在哪一段 |
| ★★★ | [fio](./fio/) | io_u 结构、I/O 引擎、iodepth | 存储性能基准测试 |
| ★★★ | [iostat](./iostat/) | /proc/diskstats、设备级统计 | 磁盘整体负载概览 |
| ★★☆ | [fuse](./fuse/) | FUSE 协议、writeback_cache | 用户态文件系统性能优化 |
| ★☆☆ | [kernel_internals](./kernel_internals/) | 页分配器、SLUB | 内存分配对 I/O 的影响 |

---

### ★ 我是网络工程师

> 目标：诊断网络吞吐、延迟、连接问题

**推荐学习顺序：**

```
ss → netstat → bpf → kernel_internals
```

| 优先级 | 工具 | 学什么 | 解决什么问题 |
|--------|------|--------|-------------|
| ★★★ | [ss](./ss/) | TCP 状态、cwnd/rtt、socket buffer | 连接级诊断 |
| ★★★ | [netstat](./netstat/) | TCP 状态机、连接统计 | 网络全局概览 |
| ★★☆ | [bpf](./bpf/) | 软中断追踪、网络收发路径 | 网络 CPU 开销分析 |
| ★★☆ | [kernel_internals](./kernel_internals/) | 网络接收路径 | 内核网络栈瓶颈 |
| ★☆☆ | [sar](./sar/) | 网络设备统计 | 历史网络数据回放 |

---

### ★ 我是应用开发者

> 目标：理解应用层性能问题的底层原因

**推荐学习顺序：**

```
strace → perf → bpf → nginx → fuse
```

| 优先级 | 工具 | 学什么 | 解决什么问题 |
|--------|------|--------|-------------|
| ★★★ | [strace](./strace/) | 系统调用追踪、futex 分析 | 进程行为可视化 |
| ★★★ | [perf](./perf/) | 火焰图、函数级热点 | CPU 热点在哪行代码 |
| ★★☆ | [bpf](./bpf/) | 自定义探针、动态追踪 | 不重编译即可追踪函数 |
| ★★☆ | [nginx](./nginx/) | worker 模型、CPU 绑核 | Web 服务器调优 |
| ★☆☆ | [fuse](./fuse/) | 用户态文件系统 | 理解 FUSE 应用的性能开销 |

---

## 3. 按问题类型导航

| 遇到的问题 | 推荐诊断路径 | 关键命令 |
|-----------|-------------|---------|
| **CPU 高但吞吐低** | [methodology](./methodology.md) → [perf](./perf/) → [topdown](./topdown/) → [cpu_arch](./cpu_arch/) | `perf stat --topdown -a` |
| **I/O 延迟高** | [blktrace](./blktrace/) → [fio](./fio/) → [iostat](./iostat/) → [kernel_internals](./kernel_internals/) | `blktrace -d /dev/sdX` |
| **网络吞吐低** | [ss](./ss/) → [netstat](./netstat/) → [bpf](./bpf/) | `ss -ti` |
| **内存压力大** | [vmstat](./vmstat/) → [sar](./sar/) → [kernel_internals](./kernel_internals/) | `vmstat 1` + `sar -B` |
| **锁竞争 / false sharing** | [perf](./perf/) → [bpf](./bpf/) → [kernel_internals](./kernel_internals/) | `perf c2c record` |
| **系统整体变慢** | [methodology](./methodology.md) → [cross_tool_diagnosis](./cross_tool_diagnosis.md) | 多工具联合排查 |
| **进程卡住 / hang** | [strace](./strace/) → [ps](./ps/) → [perf](./perf/) | `strace -p $PID` |
| **上下文切换过多** | [vmstat](./vmstat/) → [pidstat](./pidstat/) → [perf](./perf/) | `pidstat -w 1` |
| **磁盘 %util 100%** | [iostat](./iostat/) → [blktrace](./blktrace/) → [fio](./fio/) | `iostat -xz 1` |
| **TCP 连接堆积** | [ss](./ss/) → [netstat](./netstat/) | `ss -s` + `ss state time-wait` |
| **软中断 CPU 不均衡** | [sar](./sar/) → [bpf](./bpf/) → [cpu_arch](./cpu_arch/) | `sar -n DEV 1` |
| **FUSE 文件系统慢** | [fuse](./fuse/) → [strace](./strace/) → [perf](./perf/) | `strace -c -p $PID` |
| **nginx QPS 低** | [nginx](./nginx/) → [cpu_arch](./cpu_arch/) → [perf](./perf/) | CPU 绑核 + 火焰图 |

---

## 4. 完整工具目录

### 方法论

| 文件 | 行数 | 描述 |
|------|------|------|
| [methodology.md](./methodology.md) | 520 | ★ Linux 性能分析方法论 — 逐层排除法、USE 方法 |
| [cross_tool_diagnosis.md](./cross_tool_diagnosis.md) | 1,082 | ★ 跨工具性能诊断工作流 — 实战排查流程 |

### 系统观测工具

| 工具 | 目录 | 文件数 | 行数 | 描述 |
|------|------|--------|------|------|
| vmstat | [docs/vmstat/](./vmstat/) | 6 | 2,142 | 虚拟内存统计 — 系统整体健康度的"体温计" |
| iostat | [docs/iostat/](./iostat/) | 7 | 3,485 | I/O 设备统计 — 磁盘是否是瓶颈的第一判断工具 |
| sar | [docs/sar/](./sar/) | 7 | 3,408 | 系统活动报告 — "飞行记录仪"，事后分析利器 |
| ps | [docs/ps/](./ps/) | 6 | 2,779 | 进程快照 — 进程状态、资源占用的基础工具 |
| pidstat | [docs/pidstat/](./pidstat/) | 7 | 3,507 | 每进程/线程统计 — 精准到线程级的 CPU、I/O、上下文切换 |

### 内核追踪工具

| 工具 | 目录 | 文件数 | 行数 | 描述 |
|------|------|--------|------|------|
| blktrace | [docs/blktrace/](./blktrace/) | 14 | 5,566 | 块设备 I/O 追踪 — 从 Q 到 C 的全链路延迟分析 |
| strace | [docs/strace/](./strace/) | 10 | 6,072 | 系统调用追踪 — 基于 ptrace 的进程行为可视化 |
| perf | [docs/perf/](./perf/) | 13 | 7,011 | 性能计数器 — HW PMU、火焰图、c2c false sharing 检测 |
| bpf | [docs/bpf/](./bpf/) | 12 | 6,393 | BPF/eBPF 动态追踪 — bpftrace 自定义探针 |

### 专项分析工具

| 工具 | 目录 | 文件数 | 行数 | 描述 |
|------|------|--------|------|------|
| fio | [docs/fio/](./fio/) | 16 | 7,796 | I/O 性能测试框架 — io_u 结构、引擎机制、统计输出 |
| ss | [docs/ss/](./ss/) | 6 | 3,566 | Socket Statistics — TCP 连接级诊断、cwnd/rtt 分析 |
| netstat | [docs/netstat/](./netstat/) | 7 | 3,824 | 网络统计 — TCP 状态机、连接统计与诊断 |
| topdown | [docs/topdown/](./topdown/) | 8 | 4,439 | Top-Down 微架构分析 — TMAM 四层分类法 |
| cpu_arch | [docs/cpu_arch/](./cpu_arch/) | 10 | 7,011 | CPU 架构 — 微架构、L3 Cache、CCD 拓扑、锁竞争 |

### 应用与子系统

| 工具 | 目录 | 文件数 | 行数 | 描述 |
|------|------|--------|------|------|
| nginx | [docs/nginx/](./nginx/) | 7 | 4,030 | nginx 性能调优 — worker 模型、CPU 绑核、QPS 优化 |
| fuse | [docs/fuse/](./fuse/) | 10 | 5,518 | FUSE 用户态文件系统 — 协议机制、性能优化 |
| kernel_internals | [docs/kernel_internals/](./kernel_internals/) | 5 | 2,717 | 内核内部机制 — SLUB、页分配器、网络接收、调度器 |

---

## 5. 学习路线图

### ★ 新手入门路径

> 适合：刚接触 Linux 性能分析，希望建立基本观测能力

```
 ┌─────────────┐     ┌────────┐     ┌─────────┐     ┌────┐     ┌────┐     ┌─────┐
 │ methodology │────▶│ vmstat │────▶│ iostat  │────▶│ ps │────▶│ ss │────▶│ sar │
 │  方法论基础  │     │ 体温计 │     │ 磁盘观测 │     │进程│     │网络│     │回放 │
 └─────────────┘     └────────┘     └─────────┘     └────┘     └────┘     └─────┘
```

**每站学什么：**

| 步骤 | 工具 | 核心收获 | 预计时间 |
|------|------|---------|---------|
| 1 | [methodology](./methodology.md) | 逐层排除法思维、USE 方法论 | 30 分钟 |
| 2 | [vmstat](./vmstat/) | r/b 列含义、si/so 判断、cs/in 解读 | 1 小时 |
| 3 | [iostat](./iostat/) | await/%util 含义、首次输出差异 | 1 小时 |
| 4 | [ps](./ps/) | 进程状态码、%CPU 计算方式 | 30 分钟 |
| 5 | [ss](./ss/) | TCP 状态、Recv-Q/Send-Q 含义 | 1 小时 |
| 6 | [sar](./sar/) | 历史数据采集与回放 | 1 小时 |

---

### ★ 进阶路径

> 适合：已有基本观测能力，希望深入理解瓶颈根因

```
 ┌──────┐     ┌─────────┐     ┌──────────┐     ┌────────────────────┐
 │ perf │────▶│ topdown │────▶│ cpu_arch │────▶│ kernel_internals   │
 │ PMU  │     │ TMAM    │     │ 微架构   │     │ 调度器/内存分配器   │
 └──────┘     └─────────┘     └──────────┘     └────────────────────┘
```

**每站学什么：**

| 步骤 | 工具 | 核心收获 | 预计时间 |
|------|------|---------|---------|
| 1 | [perf](./perf/) | HW 计数器、perf record/report、火焰图、c2c | 3-4 小时 |
| 2 | [topdown](./topdown/) | TMAM 四层分类、Pipeline Slots 分析 | 2 小时 |
| 3 | [cpu_arch](./cpu_arch/) | CPU 微架构、L3 Cache、CCD 拓扑 | 2-3 小时 |
| 4 | [kernel_internals](./kernel_internals/) | SLUB 分配器、页分配器、调度器核心 | 3-4 小时 |

---

### ★ 专项路径

#### 存储方向

```
 ┌──────────┐     ┌─────┐     ┌─────────┐     ┌──────┐
 │ blktrace │────▶│ fio │────▶│ iostat  │────▶│ fuse │
 │ 块设备   │     │ 基准│     │ 统计    │     │用户态│
 └──────────┘     └─────┘     └─────────┘     └──────┘
```

| 步骤 | 核心收获 |
|------|---------|
| [blktrace](./blktrace/) | blk_io_trace 结构体、Q→D→C 全链路、btt 分析 |
| [fio](./fio/) | io_u 结构体、I/O 引擎机制、iodepth 维持、统计输出 |
| [iostat](./iostat/) | /proc/diskstats 原理、await/%util 解读 |
| [fuse](./fuse/) | FUSE 协议、writeback_cache 优化 |

#### 网络方向

```
 ┌────┐     ┌─────────┐     ┌─────┐     ┌────────────────────┐
 │ ss │────▶│ netstat │────▶│ bpf │────▶│ kernel_internals   │
 │连接│     │ TCP 状态│     │追踪 │     │ 网络接收路径        │
 └────┘     └─────────┘     └─────┘     └────────────────────┘
```

| 步骤 | 核心收获 |
|------|---------|
| [ss](./ss/) | ss 内部实现、cwnd/rtt 调优 |
| [netstat](./netstat/) | TCP 状态机、TIME_WAIT 优化 |
| [bpf](./bpf/) | bpftrace 追踪软中断、网络收发路径 |
| [kernel_internals](./kernel_internals/) | 网络接收路径（NAPI、softirq） |

#### 应用方向

```
 ┌────────┐     ┌──────┐     ┌───────┐     ┌─────┐
 │ strace │────▶│ perf │────▶│ nginx │────▶│ bpf │
 │ 系统调用│     │ 热点 │     │ 调优  │     │追踪 │
 └────────┘     └──────┘     └───────┘     └─────┘
```

| 步骤 | 核心收获 |
|------|---------|
| [strace](./strace/) | ptrace 机制、系统调用解码、futex 分析 |
| [perf](./perf/) | 火焰图定位热点函数 |
| [nginx](./nginx/) | worker 模型、CPU 绑核策略 |
| [bpf](./bpf/) | 动态追踪应用函数 |

#### 内核方向

```
 ┌────────────────────┐     ┌──────────┐     ┌─────────┐     ┌─────┐
 │ kernel_internals   │────▶│ cpu_arch │────▶│ topdown │────▶│ bpf │
 │ 分配器/调度器       │     │ 微架构   │     │ TMAM    │     │探针 │
 └────────────────────┘     └──────────┘     └─────────┘     └─────┘
```

| 步骤 | 核心收获 |
|------|---------|
| [kernel_internals](./kernel_internals/) | SLUB、页分配器、调度器、网络栈 |
| [cpu_arch](./cpu_arch/) | 微架构细节、Cache 层级、锁竞争 |
| [topdown](./topdown/) | CPU 效率量化分析 |
| [bpf](./bpf/) | 用 bpftrace 验证内核行为假设 |

---

## 6. 实验脚本索引

所有实验脚本均在 `/dev/vdb` 上运行，输出在 `/tmp/` 下，避免污染仓库。

### 可运行的实验脚本

| 报告/脚本 | 行数 | 所属工具 | 内容摘要 |
|-----------|------|---------|---------|
| [blktrace/lab/vdb-lab-report.md](./blktrace/lab/vdb-lab-report.md) | — | blktrace | 块设备 I/O 追踪实验报告（blktrace + fio 联合实验） |
| [bpf/lab/vdb-lab.sh](./bpf/lab/vdb-lab.sh) | 537 | bpf | bpftrace 实战实验 — 自定义探针、锁追踪、软中断分析 |
| [fio/lab/vdb-lab.sh](./fio/lab/vdb-lab.sh) | 465 | fio | fio 实战实验 — 不同引擎/模式的 I/O 基准测试 |
| [perf/lab/vdb-lab.sh](./perf/lab/vdb-lab.sh) | 438 | perf | perf 实战实验 — stat/record/report/c2c 全流程 |
| [strace/lab/vdb-lab.sh](./strace/lab/vdb-lab.sh) | 417 | strace | strace 实战实验 — 系统调用追踪、统计、过滤 |

### 运行方式

```bash
# 通用模式（以 blktrace 为例）
cd /tmp/blktrace_lab
/root/code/tool_learn/src/blktrace/blktrace -d /dev/vdb -o exp1 -w 8 &
sleep 1
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=6 --time_based
wait

# 直接运行脚本
bash /root/code/tool_learn/docs/bpf/lab/vdb-lab.sh
```

> **注意：** 实验在 `/tmp/` 下运行，blktrace 使用相对路径输出（绝对路径会有 `./` 前缀 bug）。

---

## 7. 全局 QA 索引

共计 **52** 个问答，分布在 **16** 个工具的 QA.md 中。

### blktrace — [QA.md](./blktrace/QA.md)（9 问）

| # | 问题 |
|---|------|
| Q1 | BLK_TC_FS 中 TC 表示什么？BLK_TC_FS 的含义是什么？ |
| Q2 | 为什么 BLK_TC_QUEUE 和 \_\_BLK_TA_ISSUE 都是入队？ |
| Q3 | rwbs 为 N 时如何知道它具体在干嘛？ |
| Q4 | sync、fsync、flush、FUA 有什么差异？ |
| Q5 | 操作类型用 blk_fill_rwbs 解析，事件类型（D/C/Q）在哪解析？ |
| Q6 | setup_buts() 中 buts 是什么的缩写？ |
| Q7 | 为什么 per-CPU 设计？relay buffer 为什么还可能溢出？ |
| Q8 | btt 的 seek 分析如何工作？如何判断顺序/随机 I/O？btt 该看哪些指标？ |
| Q9 | blkiomon -I 5 -d /dev/vdb 为什么没有输出？ |

### fio — [QA.md](./fio/QA.md)（6 问）

| # | 问题 |
|---|------|
| Q1 | io_u 中 acct_ddir 的作用是什么？为什么和 ddir 分开？ |
| Q2 | verify_offset 和 offset 有什么区别？验证偏移的作用是什么？ |
| Q3 | fio 如何维持 iodepth？保证在途 I/O 数目的机制是什么？ |
| Q4 | rate_iops 限速机制如何工作？ |
| Q5 | fio 测延迟为什么必须用 iodepth=1？ |
| Q6 | fio 的 slat/clat 与 blktrace 的 Q2D/D2C 是什么关系？ |

### perf — [QA.md](./perf/QA.md)（4 问）

| # | 问题 |
|---|------|
| Q1 | 如何用 perf 判断是 L3 cache miss 还是 CPU 计算瓶颈？ |
| Q2 | perf c2c 如何检测 false sharing？ |
| Q3 | perf stat 显示 IPC < 1 但 cache-miss 率正常，可能是什么原因？ |
| Q4 | perf record 报 "perf_event_paranoid" 错误怎么解决？ |

### strace — [QA.md](./strace/QA.md)（4 问）

| # | 问题 |
|---|------|
| Q1 | strace -c 显示 futex 调用最多说明什么？ |
| Q2 | strace 的性能开销有多大？生产环境能用吗？ |
| Q3 | strace 输出中 +++ exited with 0 +++ 是什么意思？ |
| Q4 | strace -p 附加到进程后没有输出，可能的原因？ |

### topdown — [QA.md](./topdown/QA.md)（4 问）

| # | 问题 |
|---|------|
| Q1 | 为什么 perf stat --topdown 在虚拟机上无法使用？ |
| Q2 | TMAM 的 Pipeline Width（流水线宽度）为什么因 CPU 代际不同？ |
| Q3 | AMD 没有 Top-Down PMU 事件怎么办？ |
| Q4 | CPU 高占用低吞吐如何用 Top-Down 方法诊断？ |

### cpu_arch — [QA.md](./cpu_arch/QA.md)（3 问）

| # | 问题 |
|---|------|
| Q1 | AMD CCD 内 8 核共享 L3 Cache 对性能有什么影响？ |
| Q2 | 如何区分 L3 cache miss 导致 CPU 高占用 vs CPU 抢占导致 cache miss？ |
| Q3 | 如何定位自旋锁竞争？ |

### iostat — [QA.md](./iostat/QA.md)（3 问）

| # | 问题 |
|---|------|
| Q1 | iostat %util=100% 一定是瓶颈吗？ |
| Q2 | await 高但 %util 低怎么解释？ |
| Q3 | iostat 首次输出和后续输出为什么不同？ |

### nginx — [QA.md](./nginx/QA.md)（3 问）

| # | 问题 |
|---|------|
| Q1 | AMD CCD 拓扑下 nginx worker 如何绑定 CPU？ |
| Q2 | nginx worker CPU 高但 QPS 低怎么排查？ |
| Q3 | nginx 的 worker_processes 设多少合适？和 CPU 核数的关系？ |

### sar — [QA.md](./sar/QA.md)（3 问）

| # | 问题 |
|---|------|
| Q1 | sar 发现 %system 很高但吞吐低怎么排查？ |
| Q2 | 如何用 sar 判断网络软中断 CPU 不均衡？ |
| Q3 | sar 数据和 vmstat/top 数据不一致，以哪个为准？ |

### bpf — [QA.md](./bpf/QA.md)（2 问）

| # | 问题 |
|---|------|
| Q1 | 如何用 bpftrace 追踪自旋锁竞争？ |
| Q2 | 如何用 bpftrace 分析软中断 CPU 占用？ |

### fuse — [QA.md](./fuse/QA.md)（2 问）

| # | 问题 |
|---|------|
| Q1 | FUSE 文件系统性能瓶颈通常在哪里？ |
| Q2 | writeback_cache 为什么能提升 FUSE 写性能？ |

### netstat — [QA.md](./netstat/QA.md)（2 问）

| # | 问题 |
|---|------|
| Q1 | 网络吞吐正常但 CPU 高怎么排查？ |
| Q2 | TIME_WAIT 过多怎么优化？ |

### pidstat — [QA.md](./pidstat/QA.md)（2 问）

| # | 问题 |
|---|------|
| Q1 | pidstat 的 cswch/s 和 nvcswch/s 分别说明什么？ |
| Q2 | 如何用 pidstat 快速定位高 CPU 进程和线程？ |

### ss — [QA.md](./ss/QA.md)（2 问）

| # | 问题 |
|---|------|
| Q1 | ss 输出中 Recv-Q 和 Send-Q 在 LISTEN 状态下为什么含义不同？ |
| Q2 | ss -i 显示的 cwnd 和 rtt 是什么？如何利用这些信息调优 TCP？ |

### vmstat — [QA.md](./vmstat/QA.md)（2 问）

| # | 问题 |
|---|------|
| Q1 | vmstat 首次输出的数值为什么和后续行差异很大？ |
| Q2 | vmstat 的 cs 列（上下文切换）多少算正常？ |

### ps — [QA.md](./ps/QA.md)（1 问）

| # | 问题 |
|---|------|
| Q1 | ps 的 %CPU 超过 100% 是正常的吗？ |

---

## 附录：文件类型统计

| 类型 | 数量 | 说明 |
|------|------|------|
| `.md` | 123 | 文档、QA、学习路线图、命令参考、输出实战 |
| `.c` | 20 | 带注释的内核/工具源码摘录 |
| `.h` | 7 | 带注释的头文件（结构体定义） |
| `.sh` | 4 | 可运行的实验脚本 |
| `.cpp` | 1 | bpftrace 探针源码 |

---

> ★ **持续更新中** — 每次遇到新的性能问题并解决后，将问答追加到对应工具的 `QA.md`  
> ★ **结论需验证** — 所有技术结论必须通过源码验证，不凭记忆或推测  
> ★ **实战优先** — 每个工具都有 `command_output_demo.md` 展示真实输出
