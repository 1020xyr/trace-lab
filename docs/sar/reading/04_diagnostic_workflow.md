# sar 性能诊断工作流

> ======================================================================
> 阅读要点：
>   1. sar 综合诊断流程 — 发现异常到定位根因
>   2. CPU 高占用低吞吐的 sar 排查
>   3. 网络软中断 CPU 不均衡排查
>   4. sar 与其他工具的联动策略
>
> 适用场景：AMD 服务器，CPU 高占用低吞吐、软中断高、锁竞争
> 预计阅读时间：20 分钟
> ======================================================================

---

## 一、综合诊断流程

### 1.1 总体思路

```
sar 是系统级"雷达"——发现异常维度，然后用专业工具深入
                    ★ sar 不是最终答案，而是起点

┌──────────────────────────────────────────────────────────────┐
│                     sar 综合诊断流程                          │
│                                                              │
│  Step 1: sar -A 1 5 → 全维度扫描                             │
│          │                                                    │
│          ├── %user 高 → CPU 计算瓶颈 → perf                   │
│          ├── %system 高 → 内核开销大 → perf lock              │
│          ├── %soft 高 → 软中断高 → bpftrace                   │
│          ├── %iowait 高 → I/O 瓶颈 → iostat/blktrace         │
│          ├── runq-sz > CPU → 过载 → pidstat                   │
│          ├── cswch/s 高 → 过度调度 → bpftrace                 │
│          ├── majflt/s > 0 → 内存不足 → sar -B                 │
│          ├── rxdrop/s > 0 → 网络丢包 → ethtool                │
│          └── await 高 → 磁盘延迟 → iostat -xz                │
│                                                              │
│  Step 2: 确定异常维度后，选择专业工具深入                      │
│          │                                                    │
│          ├── CPU → perf stat/record/top/lock/c2c             │
│          ├── 内存 → sar -B -W + pidstat -r                   │
│          ├── I/O → iostat -xz + blktrace + bpftrace          │
│          ├── 网络 → sar -n EDEV + ss + bpftrace              │
│          └── 锁 → perf lock + bpftrace                       │
│                                                              │
│  Step 3: 修复 → 验证 → 再次 sar 确认                         │
└──────────────────────────────────────────────────────────────┘
```

---

## 二、CPU 高占用低吞吐的 sar 排查

### 2.1 第一步：区分 CPU 时间分布

```bash
# ★ 使用 -u ALL 获取完整的 CPU 时间分布
sar -u ALL 1 10
```

```
场景分析：

  %usr 高 + 吞吐低
  │
  ├── IPC 低 → 内存/cache 瓶颈
  │   └── perf stat -d → cache-miss 率, IPC
  │
  ├── 热点函数效率低
  │   └── perf record -g → 热点分析
  │
  └── 算法复杂度问题
      └── perf record -g → 看哪个函数占用最多

  %sys 高 + 吞吐低
  │
  ├── 系统调用风暴
  │   ├── sar -w → cswch/s, proc/s 异常高
  │   └── bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm]=count(); }'
  │
  ├── 锁竞争
  │   ├── perf top → native_queued_spin_lock_slowpath 高
  │   └── perf lock record → 锁竞争分析
  │
  └── 内存管理开销
      └── sar -B → pgscank/pgscand > 0 → 内存压力

  %soft 高 + 吞吐低
  │
  ├── 网络包处理瓶颈
  │   ├── sar -n DEV → rxpck/s 高
  │   ├── sar -n EDEV → rxdrop/s > 0
  │   └── sar -P ALL → 某个 CPU 的 %soft 特别高 → 中断亲和性
  │
  └── RCU 回调风暴
      └── perf top → rcu_process_callbacks 高

  %iowait 高 + 吞吐低
  │
  ├── 磁盘瓶颈
  │   └── sar -d -p → await, %util 高
  │
  └── 文件系统瓶颈
      └── sar -F → %fsused, %Iused 高
```

### 2.2 第二步：深入分析 %system 高

```bash
# 1. 看上下文切换和进程创建
sar -w 1 10

# 异常指标：
#   cswch/s > 50000 → 过度调度（线程过多或锁竞争）
#   proc/s > 100   → 频繁 fork（shell 脚本密集执行）
```

```bash
# 2. 看各 CPU 核心的负载分布
sar -P ALL 1 10

# 异常指标：
#   某个 CPU %system 远高于其他 → 中断/锁绑定到该 CPU
#   所有 CPU %system 均匀高 → 全局锁竞争
```

```bash
# 3. 看运行队列
sar -q 1 10

# 异常指标：
#   runq-sz > CPU 核数 → 进程排队等待
#   blocked > 3 → I/O 阻塞进程多
#   ldavg-1 > CPU 核数 → 系统过载
```

### 2.3 诊断矩阵

```
sar 指标组合                          │ 诊断结论            │ 下一步工具
─────────────────────────────────────┼───────────────────┼────────────────
%usr 高 + IPC 低                     │ 内存/cache 瓶颈   │ perf stat -d
%usr 高 + 某函数 90%                 │ 热点函数          │ perf record -g
%sys 高 + cswch/s > 50000           │ 锁竞争/线程过多   │ perf lock
%sys 高 + 某 CPU %sys 特别高         │ 中断/锁不均衡     │ mpstat + perf top
%soft 高 + rxpck/s 高               │ 网络包处理瓶颈    │ sar -n DEV/EDEV
%soft 高 + 某 CPU %soft 特别高       │ 中断亲和性问题    │ sar -P ALL
%iowait 高 + await > 20ms            │ 磁盘延迟高        │ iostat -xz
%iowait 高 + %util > 90%             │ 磁盘饱和          │ iostat -xz
runq-sz > CPU 核数                   │ 系统过载          │ pidstat
blocked > 3 + %iowait 高             │ I/O 阻塞          │ iostat + pidstat -d
majflt/s > 0                         │ 内存严重不足      │ sar -W -B
```

---

## 三、网络软中断排查

### 3.1 问题表现

```
网络软中断 CPU 不均衡的典型症状：
  - sar -u ALL 显示 %soft 高
  - sar -P ALL 显示某个 CPU 的 %soft 远高于其他
  - sar -n EDEV 显示 rxdrop/s > 0（包被丢弃）
  - 网络吞吐上不去

原因：
  网卡默认将所有中断发送到 CPU 0
  → CPU 0 忙于处理所有网络包（softirq）
  → 其他 CPU 空闲
  → 网络处理能力受限于单个 CPU
```

### 3.2 排查流程

```bash
# Step 1: 确认软中断存在
sar -u ALL 1 5
# 关注 %soft 列

# Step 2: 看各 CPU 的软中断分布
sar -P ALL -u ALL 1 5
# 关注哪个 CPU 的 %soft 最高

# Step 3: 看网络流量
sar -n DEV 1 5
# 关注 rxpck/s（每秒接收包数）

# Step 4: 看网络错误
sar -n EDEV 1 5
# 关注 rxdrop/s（接收丢弃数）

# Step 5: 看中断分布
sar -I SUM 1 5
sar -I XALL 1 5
# 关注网络相关中断线在各 CPU 上的分布
```

### 3.3 判断标准

```
指标                          │ 正常范围          │ 异常信号
─────────────────────────────┼──────────────────┼──────────────────
%soft (全系统)                │ < 5%              │ > 10% → 软中断高
某 CPU %soft                  │ 与其他 CPU 接近   │ 远高 → 不均衡
rxdrop/s                      │ 0                 │ > 0 → 包丢弃
rxpck/s / CPU 数              │ 均匀分布          │ 集中一个 CPU → 不均衡
中断分布 (sar -I XALL)        │ 均匀分布          │ 集中一个 CPU → 不均衡

★ 不均衡的解决方案：
  1. 开启 IRQ 亲和性（irqbalance 或手动绑定）
     echo 2 > /proc/irq/<N>/smp_affinity  # 绑定到 CPU 1
  2. 开启 RPS（Receive Packet Steering）
     echo f > /sys/class/net/eth0/queues/rx-0/rps_cpus
  3. 开启多队列网卡 + RSS
     ethtool -l eth0 combined 4  # 4 个队列
```

### 3.4 完整的网络软中断诊断示例

```
诊断过程：

$ sar -u ALL 1 5
  → %soft = 12% → 软中断占比高！

$ sar -P ALL 1 5
  → CPU 0: %soft = 45%
  → CPU 1: %soft = 2%
  → CPU 2: %soft = 1%
  → CPU 3: %soft = 1%
  → ★ CPU 0 承担了大部分软中断！

$ sar -n DEV 1 5
  → eth0: rxpck/s = 150000 → 15 万包/秒

$ sar -n EDEV 1 5
  → eth0: rxdrop/s = 500 → ★ 每秒丢弃 500 包！

结论：
  网卡中断全部发送到 CPU 0
  → CPU 0 处理不过来（单 CPU 极限约 10~20 万包/秒）
  → 导致包丢弃和吞吐下降

解决：
  1. 启用 irqbalance 服务
  2. 配置网卡多队列 + RSS
  3. 启用 RPS/XPS
```

---

## 四、sar 与专业工具联动策略

### 4.1 联动决策表

```
sar 发现的异常              │ 深入工具          │ 具体命令
───────────────────────────┼──────────────────┼────────────────────────────
%usr 高 + IPC 低           │ perf stat         │ perf stat -d -r 3 -p PID
                           │ perf c2c          │ perf c2c record -p PID
%sys 高                    │ perf lock         │ perf lock record -a sleep 10
                           │ perf record       │ perf record -g -a sleep 5
                           │ bpftrace          │ kprobe:_raw_spin_lock
%soft 高                   │ bpftrace          │ kprobe:__do_softirq
                           │ sar -P ALL        │ 确认 per-CPU 分布
%iowait 高                 │ iostat -xz        │ iostat -xz 1 10
                           │ pidstat -d        │ pidstat -d 1 10
                           │ blktrace          │ blktrace -d /dev/sdX
runq-sz 高                 │ pidstat           │ pidstat -u 1
                           │ ps aux            │ ps aux --sort=-%cpu
await 高                   │ iostat -xz        │ iostat -xz 1
                           │ blktrace + btt    │ 分析 D2C 延迟
rxdrop/s > 0               │ ethtool           │ ethtool -S eth0
                           │ ss -s             │ ss -s（socket 统计）
majflt/s > 0               │ sar -W -B         │ 确认换页活动
                           │ pidstat -r        │ 哪个进程缺页
cswch/s 极高               │ bpftrace          │ 调度延迟追踪
                           │ perf sched        │ perf sched record
```

### 4.2 实战工作流示例

```
场景：AMD 服务器，用户报告 CPU 高占用低吞吐

Step 1: sar 全维度扫描
  $ sar -A 1 5
  → 发现：%system = 35%，%soft = 8%，cswch/s = 45000
  → 异常：system 高 + 上下文切换高

Step 2: per-CPU 分析
  $ sar -P ALL -u ALL 1 10
  → 发现：CPU 0 的 %soft = 30%（其他 CPU < 2%）
  → 发现：所有 CPU 的 %system 均匀高
  → 结论：软中断不均衡 + 全局锁竞争

Step 3: 锁竞争深入
  $ perf top -a
  → native_queued_spin_lock_slowpath 占 12%
  $ perf lock record -a sleep 10
  $ perf lock report
  → 某个 rwlock 争用严重

Step 4: 软中断深入
  $ bpftrace -e 'kprobe:__do_softirq { @start=nsecs; }
    kretprobe:__do_softirq /@start/ {
      @us=hist((nsecs-@start)/1000); @start=0; }'
  → 软中断处理时间双峰：1~4μs 和 50~200μs
  $ sar -I XALL 1 5
  → 网络中断集中在 CPU 0

Step 5: 修复
  → 启用 irqbalance（解决软中断不均衡）
  → 减小锁临界区（解决锁竞争）

Step 6: 验证
  $ sar -A 1 5
  → %system 降至 8%，%soft 均匀分布，cswch/s 降至 15000
  → 吞吐提升 3x
```

---

## 总结

### sar 诊断速查

```
第一步: sar -A 1 5           → 全维度扫描，发现异常
第二步: 确定异常维度           → 对照诊断矩阵
第三步: 选择专业工具深入       → 对照联动决策表
第四步: 修复 + 验证           → 再次 sar 确认
```

### 关键指标异常阈值

```
指标              │ 命令           │ 正常值        │ 异常值
─────────────────┼───────────────┼──────────────┼──────────────
%user             │ sar -u        │ < 70%         │ > 90%
%system           │ sar -u        │ < 15%         │ > 30%
%soft             │ sar -u ALL    │ < 5%          │ > 10%
%iowait           │ sar -u        │ < 5%          │ > 10%
%iowait           │ sar -u        │ < 5%          │ > 10%
cswch/s           │ sar -w        │ < 20000       │ > 50000
runq-sz           │ sar -q        │ < CPU 核数    │ > CPU 核数
ldavg-1           │ sar -q        │ < CPU 核数    │ > CPU 核数
await             │ sar -d -p     │ < 10ms        │ > 20ms
%util             │ sar -d -p     │ < 80%         │ > 90%
majflt/s          │ sar -B        │ 0             │ > 0
rxdrop/s          │ sar -n EDEV   │ 0             │ > 0
retrans/s         │ sar -n ETCP   │ < 1% of oseg  │ > 1%
```
