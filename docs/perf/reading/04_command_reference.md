# perf 完整命令参考与参数速查

> 每个常用子命令的参数详解，附源码位置和实际用法。
> 安装方式：`yum install -y perf`（或 `apt install linux-tools-common`）

---

## 一、perf 子命令总览

```
perf 子命令        │ 功能                          │ 使用频率
──────────────────┼──────────────────────────────┼──────────
perf stat         │ 统计性能计数器                │ ★★★★★
perf record       │ 采样并记录到 perf.data        │ ★★★★★
perf report       │ 分析 perf.data 的热点函数     │ ★★★★★
perf top          │ 实时显示系统热点函数          │ ★★★★
perf script       │ 输出每个采样点的详细信息      │ ★★★★
perf trace        │ 追踪系统调用（类似 strace）   │ ★★★★
perf list         │ 列出所有可用事件              │ ★★★★
perf mem          │ 内存访问分析                  │ ★★★
perf c2c          │ 多核缓存共享分析              │ ★★
perf sched        │ 调度器分析                    │ ★★
perf lock         │ 锁竞争分析                    │ ★★
perf kvm          │ KVM 虚拟化分析               │ ★★
perf annotate     │ 反汇编 + 源码标注             │ ★★★
perf evlist       │ 列出 perf.data 中的事件       │ ★★
perf diff         │ 对比两次 perf.data            │ ★★
```

---

## 二、perf stat — 性能计数器统计

### 基本用法

```bash
perf stat [选项] <命令>              # 对命令进行统计
perf stat -p <PID> [选项]            # 对正在运行的进程统计
perf stat -a [选项] sleep 10         # 全系统统计（持续 10 秒）
```

### 参数对照表

| 参数 | 含义 | 典型用法 |
|------|------|---------|
| `-e <event>` | ★ 指定要统计的事件 | `perf stat -e cycles,instructions ./app` |
| `-d` | 显示缓存相关默认事件 | `perf stat -d ./app` |
| `-d -d` | 显示更多缓存/TLB 事件 | `perf stat -d -d ./app` |
| `-a` | 全系统模式（所有 CPU） | `perf stat -a sleep 5` |
| `-p <PID>` | 附加到指定进程 | `perf stat -p 1234` |
| `-t <TID>` | 附加到指定线程 | `perf stat -t 5678` |
| `-c <cpu>` | 只统计指定 CPU | `perf stat -c 0,1 ./app` |
| `-r <N>` | 重复运行 N 次取均值 | `perf stat -r 5 ./app` |
| `-o <file>` | 输出到文件 | `perf stat -o result.txt ./app` |
| `--append` | 追加到输出文件 | `perf stat --append -o result.txt ./app` |
| `-x <sep>` | CSV 分隔符 | `perf stat -x, ./app` |
| `-v` | 详细模式（显示事件编号） | `perf stat -v ./app` |

### `-e` 参数的事件语法

```bash
# 单个事件
perf stat -e cycles ./app

# 多个事件用逗号分隔
perf stat -e cycles,instructions,cache-misses ./app

# 事件修饰符
perf stat -e cycles:k          # 只统计内核态
perf stat -e cycles:u          # 只统计用户态
perf stat -e cycles:uk         # 内核态 + 用户态（默认）
perf stat -e cycles:pp         # 精确定位（PEBS）

# 事件组
perf stat -e '{cycles,instructions}' ./app

# PMU 特定事件
perf stat -e cpu/event=0x3c,umask=0x0/ ./app      # 编码硬件事件
perf stat -e sched:sched_switch ./app               # tracepoint 事件

# 通配符
perf stat -e 'cache-*' ./app
```

### `-d` 默认事件组

```
-d（第一级）：
  task-clock              CPU 时间
  context-switches        上下文切换
  cpu-migrations          CPU 迁移
  page-faults             缺页异常
  cycles                  CPU 周期数
  instructions            指令数（→ IPC = instructions/cycles）
  branches                分支指令数
  branch-misses           分支预测失败数

-d -d（第二级）：
  L1-dcache-loads         L1 数据缓存加载
  L1-dcache-load-misses   L1 数据缓存未命中
  LLC-loads               LLC（最后一级缓存）加载
  LLC-load-misses         LLC 加载未命中

-d -d -d（第三级）：
  L1-icache-loads         L1 指令缓存加载
  L1-icache-load-misses   L1 指令缓存未命中
  dTLB-loads              数据 TLB 加载
  dTLB-load-misses        数据 TLB 未命中
  iTLB-loads              指令 TLB 加载
  iTLB-load-misses        指令 TLB 未命中
```

---

## 三、perf record — 采样记录

### 基本用法

```bash
perf record [选项] <命令>             # 采样命令执行过程
perf record -p <PID> [选项]           # 采样正在运行的进程
perf record -a [选项] sleep 10        # 全系统采样
```

### 参数对照表

| 参数 | 含义 | 典型用法 |
|------|------|---------|
| `-e <event>` | ★ 指定采样事件 | `perf record -e cycles ./app` |
| `-g` | ★ 记录调用栈（默认 fp 方式） | `perf record -g ./app` |
| `--call-graph <type>` | ★ 调用栈方式选择 | `perf record --call-graph dwarf ./app` |
| `-F <freq>` | ★ 采样频率（Hz） | `perf record -F 999 ./app` |
| `-p <PID>` | 附加到进程 | `perf record -p 1234` |
| `-t <TID>` | 附加到线程 | `perf record -t 5678` |
| `-a` | 全系统采样 | `perf record -a sleep 5` |
| `-c <count>` | 每 N 个事件采样一次 | `perf record -c 10000 ./app` |
| `-o <file>` | 输出文件（默认 perf.data） | `perf record -o my.data ./app` |
| `-C <cpu>` | 只采样指定 CPU | `perf record -C 0,1 sleep 5` |
| `--cpu <cpu>` | 限制采样到指定 CPU | `perf record --cpu 2 sleep 5` |
| `-j <filter>` | 记录分支信息 | `perf record -j any ./app` |
| `-m <pages>` | mmap buffer 大小 | `perf record -m 64 ./app` |
| `--per-thread` | 每线程一个 buffer | `perf record --per-thread ./app` |
| `-W` | 带权重采样 | `perf record -W ./app` |
| `-D <delay>` | 延迟 N 毫秒后开始 | `perf record -D 1000 ./app` |
| `--no-buildid` | 不记录 build-id | `perf record --no-buildid ./app` |
| `--switch-output` | 滚动输出文件 | `perf record --switch-output=100M` |

### `--call-graph` 调用栈方式对比

| 方式 | 参数 | 精度 | 开销 | 适用场景 |
|------|------|------|------|---------|
| `fp` | `--call-graph fp` | 中 | 低 | 默认方式，编译时保留帧指针 |
| `dwarf` | `--call-graph dwarf` | ★ 高 | 高 | 无帧指针、内联函数多 |
| `lbr` | `--call-graph lbr` | 中 | 低 | 硬件 LBR 支持（Intel CPU） |

```bash
# dwarf 方式生成完整调用链（推荐）
perf record --call-graph dwarf,65528 -F 99 ./app

# fp 方式（快速但可能不完整）
perf record -g ./app

# lbr 方式（需要硬件支持）
perf record --call-graph lbr ./app
```

### 默认采样事件

```
未指定 -e 时，默认使用 cycles:ppp（如果硬件支持）
如果 cycles 不可用（如 VM 无 PMU），自动降级为 cpu-clock:ppp

ppp 后缀含义：
  p  → precise_ip（精确指令定位）
  pp → 更精确（使用 PEBS / IBS）
  ppp → 最精确级别
```

---

## 四、perf report — 热点函数分析

### 基本用法

```bash
perf report [选项]                    # 分析默认 perf.data
perf report -i <file> [选项]          # 分析指定文件
```

### 参数对照表

| 参数 | 含义 | 典型用法 |
|------|------|---------|
| `-g` | ★ 显示调用图（callee 视角） | `perf report -g` |
| `--call-graph <type>` | ★ 调用图显示方式 | `perf report --call-graph graph,0.5,caller` |
| `--stdio` | 纯文本输出（非 TUI） | `perf report --stdio` |
| `-n` | 显示采样数 | `perf report -n --stdio` |
| `--sort <key>` | ★ 排序方式 | `perf report --sort symbol,dso` |
| `--no-children` | 不显示子函数累计 | `perf report --no-children` |
| `--percentage` | 显示百分比 | `perf report --percentage absolute` |
| `-s <key>` | `--sort` 的简写 | `perf report -s comm,dso,symbol` |
| `--dsos <list>` | 只显示指定 DSO | `perf report --dsos libc.so` |
| `--comms <list>` | 只显示指定命令 | `perf report --comms nginx` |
| `--symbols <list>` | 只显示指定函数 | `perf report --symbols main` |
| `-w <width>` | 列宽设置 | `perf report -w 30,20,50` |
| `--header` | 显示 perf.data 头信息 | `perf report --header` |
| `--header-only` | 只显示头信息 | `perf report --header-only` |
| `--hierarchy` | 层次化显示 | `perf report --hierarchy` |
| `--total-period` | 显示总周期数 | `perf report --total-period` |

### `--call-graph` 显示选项

```bash
# 基本形式
perf report --call-graph flat          # 扁平调用链
perf report --call-graph graph         # 树形调用图（默认）
perf report --call-graph fractal       # 分形模式（相对百分比）
perf report --call-graph none          # 不显示调用图

# 详细参数
perf report --call-graph graph,<min_percent>,<print_type>,<order>
#   min_percent：最小显示百分比（如 0.5）
#   print_type：graph / flat / fractal / folded
#   order：caller（谁调用的）/ callee（调用了谁）
```

### `--sort` 排序键

| 键 | 含义 | 示例 |
|---|------|------|
| `comm` | 进程名 | `--sort comm` |
| `pid` | 进程 ID | `--sort pid` |
| `dso` | 共享库名 | `--sort dso` |
| `symbol` | 函数名 | `--sort symbol` |
| `srcline` | 源码行号 | `--sort srcline` |
| `parent` | 父函数 | `--sort parent` |
| `cpu` | CPU 号 | `--sort cpu` |
| `overhead` | 开销占比 | `--sort overhead` |
| `sample` | 采样数 | `--sort sample` |

---

## 五、perf top — 实时热点分析

### 基本用法

```bash
perf top [选项]                       # 实时显示系统热点
perf top -p <PID> [选项]              # 只监视指定进程
```

### 参数对照表

| 参数 | 含义 | 典型用法 |
|------|------|---------|
| `-e <event>` | 采样事件 | `perf top -e cycles` |
| `-a` | 全系统（默认） | `perf top -a` |
| `-p <PID>` | 只监视指定进程 | `perf top -p 1234` |
| `-C <cpu>` | 只监视指定 CPU | `perf top -C 0` |
| `-F <freq>` | 采样频率 | `perf top -F 999` |
| `-g` | 显示调用栈 | `perf top -g` |
| `--stdio` | 文本输出（非 TUI） | `perf top --stdio` |
| `-d <sec>` | 刷新间隔 | `perf top -d 5` |
| `-K` | 隐藏内核符号 | `perf top -K` |
| `-U` | 隐藏用户符号 | `perf top -U` |
| `--sort <key>` | 排序方式 | `perf top --sort symbol` |
| `-z` | 每次刷新清零 | `perf top -z` |

### TUI 交互快捷键

```
[d]  刷新延迟     [e]  显示行数     [f]  过滤（按采样数）
[F]  标注过滤     [s]  标注符号     [S]  停止标注
[K]  隐藏内核     [U]  隐藏用户     [z]  清零采样
[q]  退出
```

---

## 六、perf script — 详细采样流

### 基本用法

```bash
perf script [选项]                    # 输出默认 perf.data
perf script -i <file> [选项]          # 输出指定文件
```

### 参数对照表

| 参数 | 含义 | 典型用法 |
|------|------|---------|
| `-i <file>` | 输入文件 | `perf script -i perf.data` |
| `-F <fields>` | ★ 输出字段选择 | `perf script -F comm,pid,tid,time,event,ip,sym` |
| `-s <script>` | 运行自定义脚本 | `perf script -s report.py` |
| `-g` | 显示调用栈 | `perf script -g` |
| `--comms <list>` | 过滤进程 | `perf script --comms nginx` |
| `--symbols <list>` | 过滤函数 | `perf script --symbols main` |
| `--dsos <list>` | 过滤 DSO | `perf script --dsos libc.so` |
| `-G` | 不显示调用栈（默认显示） | `perf script -G` |

### `-F` 输出字段

| 字段 | 含义 | 示例输出 |
|------|------|---------|
| `comm` | 进程名 | `test_cpu` |
| `pid` | 进程 ID | `435603` |
| `tid` | 线程 ID | `435603` |
| `time` | 时间戳 | `1782226.262565` |
| `event` | 事件名 | `cpu-clock:ppp` |
| `ip` | 指令地址 | `401172` |
| `sym` | 函数名 | `is_prime` |
| `dso` | 共享库名 | `test_cpu` |
| `period` | 采样周期 | `250000` |
| `callindent` | 调用栈缩进 | （用于 FlameGraph） |

---

## 七、perf trace — 系统调用追踪

### 基本用法

```bash
perf trace [选项] <命令>              # 追踪命令执行
perf trace -p <PID> [选项]            # 追踪运行中的进程
```

### 参数对照表

| 参数 | 含义 | 典型用法 |
|------|------|---------|
| `-e <syscall>` | ★ 过滤系统调用 | `perf trace -e read,write,open ./app` |
| `-p <PID>` | 追踪指定进程 | `perf trace -p 1234` |
| `-a` | 全系统 | `perf trace -a` |
| `-s` | 显示调用栈 | `perf trace -s -e read ./app` |
| `-o <file>` | 输出到文件 | `perf trace -o trace.txt ./app` |
| `--duration <ms>` | 只显示超过指定时长的调用 | `perf trace --duration 1 ./app` |
| `-S <syscall>` | 排除指定系统调用 | `perf trace -S write ./app` |
| `--summary` | 结束时输出统计摘要 | `perf trace --summary ./app` |
| `--summary-only` | 只输出摘要 | `perf trace --summary-only ./app` |
| `--no-inherit` | 不追踪子进程 | `perf trace --no-inherit ./app` |

### 输出格式解读

```
时间(ms)   耗时(ms)   进程/PID        系统调用(参数...)              返回值
────────  ──────────  ──────────────  ──────────────────────────  ──────────
0.570     (0.019 ms)  test_io/436703  openat(dfd:CWD, filename:..., flags:...) = 3
0.591     (0.014 ms)  test_io/436703  write(fd:3<testfile.dat>, buf:..., count:4096) = 4096
```

```
字段含义：
  0.570            → 程序开始后的时间（毫秒）
  (0.019 ms)       → ★ 该系统调用耗时 19μs
  test_io/436703   → 进程名/PID
  openat(...)      → 系统调用名 + 参数（自动解析）
  dfd: CWD         → 参数的符号化显示（CWD = 当前目录）
  = 3              → 返回值（fd=3）
```

---

## 八、perf list — 事件列表

### 基本用法

```bash
perf list                            # 列出所有事件
perf list hw                         # 只列硬件事件
perf list sw                         # 只列软件事件
perf list cache                      # 缓存事件
perf list tracepoint                 # tracepoint 事件
perf list pmu                        # PMU 事件
perf list sched                      # sched tracepoint
perf list block                      # block tracepoint
perf list --desc                     # 显示事件描述
perf list --details                  # 显示详细信息
```

### 事件类型分类

```
类型               │ 说明                          │ 可用性
──────────────────┼──────────────────────────────┼──────────────
Hardware event     │ CPU 硬件计数器（PMU）         │ 物理机 ✓ VM ×
Software event     │ 内核软件计数器                │ ★ 全环境可用
Tracepoint event   │ 内核 tracepoint               │ 全环境可用
Hardware cache     │ 缓存相关硬件事件              │ 物理机 ✓ VM ×
Raw hardware       │ 原始编码事件（rNNN）          │ 物理机 ✓ VM ×
Hardware breakpoint│ 硬件断点                      │ 通常可用
Kernel PMU         │ PMU 特定事件                  │ 取决于硬件
```

> ★ **注意：** 在 KVM 等虚拟机中，硬件 PMU 计数器（cycles、instructions 等）
> 通常不可用，显示 `<not supported>`。此时 perf 会自动降级为软件事件（cpu-clock）。
> 软件事件和 tracepoint 在 VM 中完全可用。

---

## 九、perf mem — 内存访问分析

### 基本用法

```bash
perf mem record [选项] <命令>         # 记录内存访问事件
perf mem report [选项]                # 分析内存访问模式
```

### 参数对照表

| 参数 | 含义 |
|------|------|
| `-t load` | 只追踪 load（读取） |
| `-t store` | 只追踪 store（写入） |
| `-t ldst` | 追踪 load + store |

### 输出字段

```
字段            │ 含义
───────────────┼────────────────────
Local DRAM     │ 本地 DRAM 访问
Remote DRAM    │ 远程 NUMA 节点 DRAM
Remote Cache   │ 远程缓存命中
L1/L2/L3 hit   │ 各级缓存命中
LFB hit        │ Line Fill Buffer 命中
Uncached       │ 不可缓存的 MMIO
```

---

## 十、perf c2c — 缓存共享分析

### 基本用法

```bash
perf c2c record [选项] <命令>         # 记录（需要 -e mem-loads,mem-stores）
perf c2c report [选项]                # 分析缓存行共享情况
```

### 用途

```
c2c 分析可以检测：
- 缓存行伪共享（false sharing）
- 多核之间缓存行频繁迁移
- 内存访问的 NUMA 本地性

典型场景：多线程性能调优、NUMA 优化
```

---

## 十一、10 个常用场景命令

### 场景 1：快速检查程序性能概况

```bash
# 一键获取 IPC、缓存命中率、分支预测率等关键指标
perf stat -d -r 3 ./my_app
# -d    → 缓存/分支默认事件组
# -r 3  → 重复 3 次取平均值
```

### 场景 2：找出 CPU 热点函数

```bash
# 采样（带调用栈）→ 生成报告
perf record -g --call-graph dwarf -F 99 ./my_app
perf report -g --stdio --no-children
# -g               → 记录调用栈
# --call-graph dwarf → DWARF 方式（最完整）
# -F 99            → 99Hz 采样（低开销）
# --no-children    → 只看自身开销
```

### 场景 3：实时监控生产系统热点

```bash
# 实时查看哪个函数/CPU 最忙
perf top -a --stdio
# -a      → 全系统
# --stdio → 文本输出（方便管道处理）
```

### 场景 4：追踪系统调用（strace 替代）

```bash
# 追踪所有系统调用 + 耗时
perf trace -e read,write,open,openat,close ./my_app
# 比 strace 开销更低（基于 perf_event_open）

# 只看慢调用（>1ms）
perf trace --duration 1 ./my_app
```

### 场景 5：分析缓存/TLB 性能

```bash
# 详细缓存统计
perf stat -d -d -d ./my_app
# 三级 -d 显示所有缓存和 TLB 事件

# 关注指标：
# L1-dcache-load-misses / L1-dcache-loads → L1 miss 率
# LLC-load-misses / LLC-loads             → LLC miss 率
# dTLB-load-misses / dTLB-loads           → TLB miss 率
```

### 场景 6：对比两次性能差异

```bash
# 生成旧版和新版的 perf.data
perf record -g -o old.data ./old_app
perf record -g -o new.data ./new_app

# 对比
perf diff old.data new.data
# 正数 → new 版本在该函数上变慢
# 负数 → new 版本在该函数上变快
```

### 场景 7：生成火焰图

```bash
# 1. 采样（带调用栈 + folded 格式输出）
perf record -g --call-graph dwarf -F 99 ./my_app

# 2. 转换为 folded 格式
perf script | stackcollapse-perf.pl > folded.txt

# 3. 生成 SVG 火焰图
flamegraph.pl folded.txt > flamegraph.svg

# 需要的工具：https://github.com/brendangregg/FlameGraph
```

### 场景 8：分析特定内核子系统

```bash
# 只采样 block tracepoint
perf record -e 'block:*' -a sleep 5
perf report --stdio

# 只采样调度器 tracepoint
perf record -e 'sched:*' -a sleep 5
perf report --stdio

# 只采样网络 tracepoint
perf record -e 'net:*' -a sleep 5
```

### 场景 9：分析多核负载均衡

```bash
# 全系统采样，按 CPU 排序
perf record -a -F 99 sleep 5
perf report --sort cpu,symbol --stdio

# 统计各 CPU 负载
perf stat -a -e task-clock sleep 5
# task-clock / 4 = 平均每 CPU 利用率（4 核系统）
```

### 场景 10：分析分支预测

```bash
# 统计分支预测率
perf stat -e branches,branch-misses ./my_app

# 关注指标：
# branch-misses / branches × 100 → 分支预测失败率
# < 5%  → 优秀
# 5-10% → 一般
# > 10% → 需要优化（考虑 __builtin_expect）
```

---

## 十二、环境注意事项

### VM / 容器中的 perf

```
事件类型          │ 物理机  │ VM (KVM)  │ 容器
─────────────────┼────────┼──────────┼──────
cycles           │ ✓      │ × 注1     │ ✓ 注2
instructions   │ ✓      │ × 注1     │ ✓ 注2
cache-*          │ ✓      │ × 注1     │ ✓ 注2
branches         │ ✓      │ × 注1     │ ✓ 注2
task-clock       │ ✓      │ ✓         │ ✓
context-switches │ ✓      │ ✓         │ ✓
page-faults      │ ✓      │ ✓         │ ✓
tracepoints      │ ✓      │ ✓         │ ✓ 注3
cpu-clock        │ ✓      │ ✓         │ ✓

注1：除非 VM 启用了 PMU 透传（KVM -cpu host,+pmu）
注2：容器共享宿主机内核，硬件计数器通常可用
注3：容器需要 --cap-add SYS_ADMIN 或使用 tracefs
```

### 常见错误

```bash
# 错误 1：<not supported>
# 原因：VM 中没有 PMU 硬件计数器
# 解决：改用软件事件
perf stat -e task-clock,context-switches,page-faults ./app

# 错误 2：Permission error
# 解决：
echo -1 > /proc/sys/kernel/perf_event_paranoid
# 或用 sudo

# 错误 3：mmap loss
# 解决：增大 buffer
perf record -m 256 ./app

# 错误 4：kernel symbols 显示为 [unknown]
# 解决：安装内核 debug 信息
# yum install kernel-debuginfo
# 或设置：echo 0 > /proc/sys/kernel/kptr_restrict
```

---

## 十三、源码文件快速索引

```bash
# perf 源码位于内核树中
# src/linux-5.10/tools/perf/

# perf stat 主函数
grep -n "cmd_stat" src/linux-5.10/tools/perf/builtin-stat.c

# perf record 主函数
grep -n "cmd_record" src/linux-5.10/tools/perf/builtin-record.c

# perf report 主函数
grep -n "cmd_report" src/linux-5.10/tools/perf/builtin-report.c

# 事件解析
grep -rn "perf_evsel" src/linux-5.10/tools/perf/util/evsel.c

# 采样引擎
grep -rn "perf_mmap" src/linux-5.10/tools/perf/util/mmap.c

# 调用栈解析
grep -rn "callchain" src/linux-5.10/tools/perf/util/callchain.c

# perf_event_open 系统调用
grep -rn "SYSCALL_DEFINE5(perf_event_open" src/linux-5.10/kernel/events/core.c
```
