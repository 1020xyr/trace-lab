# 工具选型指南：根据问题类型选择正确的工具组合

> 不是所有问题都需要 perf + bpftrace。选对工具，5 分钟定位问题；选错工具，5 小时还在猜。

---

## 目录

- [选型核心原则](#选型核心原则)
- [按问题类型的工具选择表](#按问题类型的工具选择表)
- [工具开销对比](#工具开销对比)
- [生产环境 vs 开发环境工具选择](#生产环境-vs-开发环境工具选择)
- [工具之间的数据流转图](#工具之间的数据流转图)
- [工具能力矩阵](#工具能力矩阵)

---

## 选型核心原则

```
                    ┌──────────────────────────────────┐
                    │        工具选型三原则              │
                    └──────────────────────────────────┘

    ┌────────────────┐  ┌────────────────┐  ┌────────────────┐
    │  ① 够用就好     │  │  ② 先轻后重     │  │  ③ 交叉验证     │
    │                │  │                │  │                │
    │  vmstat 能解决  │  │  先 /proc      │  │  perf 说锁竞争  │
    │  就不要上       │  │  再 perf       │  │  → bpftrace    │
    │  bpftrace      │  │  最后 bpftrace │  │    确认        │
    └────────────────┘  └────────────────┘  └────────────────┘
```

**原则解释：**

| 原则 | 说明 | 反面教材 |
|------|------|---------|
| ★ **够用就好** | 用最简单的工具解决问题 | 用 bpftrace 看 CPU 利用率（vmstat 就行） |
| ★ **先轻后重** | 先用低开销工具确认方向，再用重型工具深入 | 上来就 strace -p（10-100x 减速） |
| ★ **交叉验证** | 一个工具的结论要用另一个工具验证 | 只看 perf 就下结论（可能是采样偏差） |

---

## 按问题类型的工具选择表

### CPU 问题

| 具体问题 | 首选工具 | 深入工具 | 不推荐 |
|---------|---------|---------|--------|
| CPU 利用率高 | `top` / `vmstat 1` | `sar -u ALL 1` | strace |
| 哪个进程占 CPU | `pidstat -u 1` | `top -H -p PID` | strace |
| 哪个线程占 CPU | `pidstat -u -t 1` | `perf top -p PID` | strace |
| 用户态热点函数 | `perf record -e cycles:u` | `perf report -g` | strace -c |
| 内核态热点函数 | `perf record -e cycles:k` | `bpftrace kstack` | ftrace (开销大) |
| 锁竞争 | `perf lock record/report` | `bpftrace spinlock` | strace |
| false sharing | `perf c2c record/report` | — | — |
| 调度延迟 | `vmstat (cs/r)` | `perf sched latency` | strace |
| IPC 低（stall 多） | `perf stat -d` | `perf topdown-l1/l2` | — |
| 分支预测失败 | `perf stat branch-*` | `perf topdown` | — |
| cache miss 高 | `perf stat cache-*` | `perf mem` / `perf c2c` | — |

### 内存问题

| 具体问题 | 首选工具 | 深入工具 | 不推荐 |
|---------|---------|---------|--------|
| 内存不足 | `free -h` / `vmstat 1` | `sar -r` | — |
| swap 活动 | `vmstat 1 (si/so)` | `sar -r`, `sar -B` | — |
| 内存泄漏（用户态） | `pidstat -r 1` | `valgrind --tool=massif` | strace |
| 内存泄漏（内核态） | `/proc/slabinfo` | `kmemleak` / `bpftrace kmem` | — |
| 缺页中断多 | `sar -B (majflt)` | `bpftrace handle_mm_fault` | strace |
| slab 占用高 | `/proc/slabinfo` | `bpftrace kmem_cache_alloc` | — |
| NUMA 不均衡 | `numastat` | `perf stat numa-*` | — |
| TLB miss 多 | `perf stat dTLB/iTLB` | 大页 (hugepages) | — |
| OOM 分析 | `dmesg` | `/proc/<pid>/smaps` | — |

### I/O 问题

| 具体问题 | 首选工具 | 深入工具 | 不推荐 |
|---------|---------|---------|--------|
| I/O 延迟高 | `iostat -xz 1` | `blktrace + btt` | strace -T |
| 哪个进程 I/O 多 | `pidstat -d 1` | `iotop` | strace |
| I/O 队列分析 | `iostat (avgqu-sz)` | `blktrace + blkparse` | — |
| I/O 调度延迟 | `iostat (await/svctm)` | `btt (Q2D)` | — |
| 设备处理慢 | `iostat (svctm)` | `btt (D2C)` | — |
| I/O 合并情况 | `iostat (rrqm/wrqm)` | `btt -l` | — |
| 文件系统延迟 | `pidstat -d` | `bpftrace vfs_read/vfs_write` | strace |
| NFS 延迟 | `nfsstat` | `bpftrace nfs_*` | tcpdump |

### 网络问题

| 具体问题 | 首选工具 | 深入工具 | 不推荐 |
|---------|---------|---------|--------|
| 网络带宽 | `sar -n DEV 1` | `iftop` / `nload` | tcpdump |
| 丢包/错误 | `sar -n EDEV 1` | `ss -ti` | tcpdump |
| 连接状态 | `ss -s` | `ss -ti state *` | netstat (慢) |
| 软中断分布 | `/proc/softirqs` | `bpftrace softirq` | — |
| TCP 重传 | `ss -ti` / `sar -n ETCP` | `bpftrace tcp_retransmit_skb` | tcpdump |
| 协议栈热点 | `perf record :k` | `bpftrace skb/tcp` | strace |
| DNS 延迟 | `dig +stats` | `bpftrace udp_*` | strace |
| 包速率瓶颈 | `sar -n DEV (pps)` | `bpftrace napi_poll` | tcpdump |

### 综合问题

| 具体问题 | 首选工具 | 深入工具 | 不推荐 |
|---------|---------|---------|--------|
| 系统整体慢 | `vmstat 1` + `sar -q` | `dmesg` + `perf stat -a` | strace |
| 间歇性卡顿 | `sar -u 1 300` (长时间) | `bpftrace latency` | — |
| 启动慢 | `systemd-analyze` | `perf record -a` (启动期间) | strace |
| 负载突增 | `sar -q` (历史) | 多维度 sar 对比 | — |

---

## 工具开销对比

> 选择工具时，开销是核心考量。以下是实测级别的开销对比。

### 开销一览表

```
开销级别（从低到高）：

  ┌────────────────────────────────────────────────────────────────────────┐
  │  0%    1%    5%    10%   50%   100%  500%  1000%     10000%           │
  │  │     │     │     │     │     │     │     │          │               │
  │  ├─ vmstat / sar / iostat / pidstat  (< 0.1%)  ★ 生产安全             │
  │  │                                                                  │
  │  ├─ perf stat                 (< 0.5%)  ★ 生产安全                    │
  │  │                                                                  │
  │  ├─ perf record -F 99         (~1%)     ⚠ 生产可用（低频）            │
  │  │                                                                  │
  │  ├─ perf record (默认 4KHz)   (1-3%)    ⚠ 生产谨慎                    │
  │  │                                                                  │
  │  ├─ bpftrace                  (< 1%)    ★ 生产安全                    │
  │  │                                                                  │
  │  ├─ blktrace                  (1-5%)    ⚠ 生产谨慎                    │
  │  │                                                                  │
  │  ├─ perf lock                 (2-5%)    ⚠ 生产谨慎                    │
  │  │                                                                  │
  │  ├─ perf trace                (1-3%)    ⚠ 生产谨慎                    │
  │  │                                                                  │
  │  ├─ tcpdump                   (1-10%)   ⚠ 取决于流量                  │
  │  │                                                                  │
  │  └─ strace                    (10-100x)  ✘ 生产禁用！！！              │
  │                                                                      │
  └────────────────────────────────────────────────────────────────────────┘
```

### 各工具开销详解

| 工具 | 开销 | 原理 | 为什么这个开销 |
|------|------|------|---------------|
| **vmstat** | < 0.1% | 读取 /proc 文件 | 纯文件读取，无内核 hook |
| **sar** | < 0.1% | 定时采集 + 写入文件 | sadc 守护进程，开销极低 |
| **iostat** | < 0.1% | 读取 /proc/diskstats | 纯文件读取 |
| **pidstat** | < 0.1% | 读取 /proc/[pid]/stat | 纯文件读取 |
| **perf stat** | < 0.5% | 读取硬件计数器 | PMU 硬件采集，几乎零开销 |
| **perf record** | 1-3% | 采样 + 写入 perf.data | 默认 4KHz 采样，每次采样有中断开销 |
| **perf record -F 99** | < 1% | 低频采样 | 每秒 99 次采样，开销可忽略 |
| **bpftrace** | < 1% | eBPF JIT 编译执行 | BPF 程序在内核中 JIT 执行，无上下文切换 ★ |
| **blktrace** | 1-5% | relay buffer 写入 | 每个 I/O 事件都记录，I/O 密集时开销大 |
| **perf lock** | 2-5% | tracepoint hook | 每个锁操作都触发 tracepoint |
| **perf trace** | 1-3% | tracepoint hook | 每个系统调用触发 |
| **strace** | **10-100x** | ptrace 机制 | ★ 每次系统调用都暂停进程、上下文切换到 strace 进程 |
| **tcpdump** | 1-10% | 包拷贝 | 高 pps 时每个包都拷贝，开销线性增长 |

### strace 为什么这么慢？

```
strace 的工作机制（ptrace）：

目标进程                      strace 进程
─────────                    ──────────
执行系统调用
    │
    ▼
内核暂停目标进程 ────→ 唤醒 strace ────→ strace 读取参数
    │                                        │
    │                                        ▼
    │                                   打印信息
    │                                        │
    │            ←──── PTRACE_CONT ──────────┘
    ▼
继续执行
    │
    ▼
系统调用返回
    │
    ▼
内核再次暂停 ────→ 唤醒 strace ────→ strace 读取返回值
    │                                        │
    │            ←──── PTRACE_CONT ──────────┘
    ▼
继续执行

★ 每次系统调用 = 2 次进程切换（进入 + 返回）
★ 如果进程每秒 10 万次系统调用 → 20 万次进程切换 → 性能暴跌 10-100 倍
```

**替代方案：**

| 需求 | 不要用 strace | 用这个代替 |
|------|-------------|-----------|
| 看系统调用分布 | `strace -c -p PID` | `perf trace -p PID` (开销 < 3%) |
| 看系统调用耗时 | `strace -T -p PID` | `bpftrace syscall` (开销 < 1%) |
| 看某个系统调用参数 | `strace -e write -p PID` | `bpftrace` 指定追踪 |
| 完整系统调用日志 | `strace -f -p PID` | ✘ 只在开发环境用 |

---

## 生产环境 vs 开发环境工具选择

### 生产环境：安全优先

```
生产环境工具选择决策树：

问题出现
    │
    ├── 能重启/停机吗？
    │   │
    │   ├── 否 → 只能用"读取型"工具
    │   │   ├── vmstat / sar / iostat / pidstat  ★ 首选
    │   │   ├── /proc/* 文件系统                ★ 安全
    │   │   ├── perf stat                       ★ 安全
    │   │   ├── perf record -F 99 (低频)        ⚠ 评估后使用
    │   │   └── bpftrace (简单脚本)             ★ 通常安全
    │   │
    │   └── 是 → 可以在维护窗口用重型工具
    │       ├── perf record (完整采样)
    │       ├── blktrace (I/O 追踪)
    │       └── perf lock (锁分析)
    │
    └── 是否影响业务 SLA？
        │
        ├── 是 → 严格限制采集时间（< 30s）
        └── 否 → 正常采集（60s 内）
```

### 生产环境安全工具清单

| 工具 | 安全等级 | 限制条件 | 备注 |
|------|---------|---------|------|
| vmstat 1 | ★★★★★ 安全 | 无 | 纯读取 |
| sar -u/-r/-d/-n 1 | ★★★★★ 安全 | 无 | 纯读取 |
| iostat -xz 1 | ★★★★★ 安全 | 无 | 纯读取 |
| pidstat -u/-d/-r 1 | ★★★★★ 安全 | 无 | 纯读取 |
| perf stat | ★★★★☆ 安全 | 采集时间 < 60s | 硬件计数器 |
| perf record -F 99 | ★★★☆☆ 注意 | 采集时间 < 30s | 低频采样 |
| bpftrace (简单) | ★★★★☆ 安全 | 脚本复杂度低 | eBPF JIT |
| ss -s / ss -ti | ★★★★★ 安全 | 无 | 纯读取 |
| /proc/* | ★★★★★ 安全 | 无 | 纯读取 |
| dmesg | ★★★★★ 安全 | 无 | 纯读取 |

### 生产环境禁用工具

| 工具 | 原因 | 替代方案 |
|------|------|---------|
| **strace** | ptrace 10-100x 减速，可能导致超时 | perf trace / bpftrace |
| **tcpdump** (长时间) | 高流量时包拷贝开销大 | sar -n DEV / ss -ti |
| **perf record** (高频) | 默认 4KHz 中断开销 | perf record -F 99 |
| **blktrace** (长时间) | relay buffer 写入开销 | iostat (宏观) |
| **perf lock** (长时间) | tracepoint 每锁操作触发 | 短时间 perf lock |
| **ltrace** | ptrace 机制，同 strace | — |

### 开发环境：深入分析

开发/测试环境可以使用所有工具：

```bash
# ★ 完整诊断流程（仅开发环境）

# 1. strace 完整追踪（开发环境专用）
strace -f -T -tt -o /tmp/trace.log -p <PID>

# 2. perf 完整采样
perf record -g -e cycles:k,cycles:u -a -- sleep 30

# 3. blktrace 完整 I/O 追踪
blktrace -d /dev/vdb -o full -w 60

# 4. bpftrace 深度追踪
bpftrace -e '
kprobe:vfs_read { @start[tid] = nsecs; }
kretprobe:vfs_read /@start[tid]/ {
    @latency = hist(nsecs - @start[tid]);
    delete(@start[tid]);
}
'

# 5. 锁竞争完整分析
perf lock record -a -- sleep 30
perf lock report
```

---

## 工具之间的数据流转图

> 工具不是孤立的。一个工具的输出是另一个工具的输入线索。

### 完整诊断数据流

```
                    ┌─────────────────────────────────────────────┐
                    │            第一层：宏观扫描                    │
                    └─────────────────────────────────────────────┘
                    
    ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
    │ vmstat  │  │ sar -u  │  │ sar -r  │  │iostat -x│  │sar -n   │
    │  1      │  │ ALL 1   │  │ 1       │  │ 1       │  │ DEV 1   │
    └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘
         │            │            │             │            │
         │  r/cs/wa   │ %user/sys  │ %memused    │ %util      │ pps/带宽
         │  si/so     │ %iowait    │ si/so       │ await      │ pps
         │            │            │             │ avgqu-sz   │
         ▼            ▼            ▼             ▼            ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │                    确定瓶颈维度                                  │
    │                                                                 │
    │  CPU 计算 ──┐  内存压力 ──┐  I/O 延迟 ──┐  网络问题 ──┐        │
    │             │             │             │             │        │
    └─────────────┼─────────────┼─────────────┼─────────────┼────────┘
                  │             │             │             │
    ┌─────────────▼─────────────▼─────────────▼─────────────▼────────┐
    │            第二层：维度深入                                      │
    └─────────────────────────────────────────────────────────────────┘
    
    ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
    │ pidstat  │  │/proc/    │  │ blktrace │  │/proc/    │
    │ -u/-t    │  │slabinfo  │  │+blkparse │  │softirqs  │
    │          │  │          │  │          │  │          │
    └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘
         │             │             │              │
         │ 找到进程/   │ slab 占用   │ trace 文件   │ 中断分布
         │ 线程        │ 排名        │              │
         ▼             ▼             ▼              ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │              第三层：根因定位                                     │
    └─────────────────────────────────────────────────────────────────┘
    
    ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
    │ perf     │  │ bpftrace │  │   btt    │  │ perf     │
    │ record   │  │          │  │          │  │ lock     │
    │ -g       │  │          │  │          │  │          │
    └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘
         │             │             │              │
         │ 热点函数    │ 内核路径    │ Q2D/D2C      │ 锁等待
         │ + 调用栈   │ 追踪       │ 延迟分解     │ 时间
         ▼             ▼             ▼              ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │              根因确认 + 修复方案                                  │
    └─────────────────────────────────────────────────────────────────┘
```

### 工具间线索传递示例

```
示例 1：CPU 高但吞吐低

sar -u 1  →  %system 52%（内核态高）
    │
    │  线索：%system 高 → 需要看内核态热点
    ▼
perf record -e cycles:k -a -- sleep 10
perf report  →  _raw_spin_lock_irqsave 占 45%
    │
    │  线索：spinlock 热点 → 需要确认锁竞争
    ▼
perf lock record -a -- sleep 10
perf lock report  →  zone->lock 等待时间最长
    │
    │  线索：zone->lock → 内存分配器锁 → 确认是内存分配路径
    ▼
bpftrace -e 'kprobe:kmem_cache_alloc { @[kstack(5)] = count(); }'
    →  确认分配路径 → 提出优化方案（对象池/大页）
```

```
示例 2：I/O 延迟高

vmstat 1  →  wa 45%（I/O 等待高）
    │
    │  线索：wa 高 → 需要看哪个设备
    ▼
iostat -xz 1  →  vdb: %util=98%, await=12ms, svctm=0.2ms
    │
    │  线索：await >> svctm → 排队等待问题
    ▼
pidstat -d 1  →  PID 1234 (fio) 读取 200MB/s
    │
    │  线索：找到 I/O 进程 → 需要分析 I/O 生命周期
    ▼
blktrace -d /dev/vdb -o exp -w 10
blkparse -i exp.blktrace.0
btt -i exp.blktrace.0  →  Q2D=11ms, D2C=0.2ms
    │
    │  线索：Q2D 高 → I/O 调度/队列问题 → 调整 I/O 调度器
    ▼
cat /sys/block/vdb/queue/scheduler  →  当前 mq-deadline
    →  尝试 bfq 或 none（NVMe）
```

```
示例 3：网络 CPU 高

sar -n DEV 1  →  eth0: rxpck=85K/s, rxkB=102400 (100MB/s)
    │
    │  线索：pps 85K 不算高，但 CPU 高 → 检查软中断分布
    ▼
cat /proc/softirqs  →  NET_RX: CPU0=85%, 其他 CPU < 5%
    │
    │  线索：软中断集中在 CPU0 → 需要分散
    ▼
cat /sys/class/net/eth0/queues/rx-0/rps_cpus  →  00000001（只有 CPU0）
    │
    │  线索：RPS 未配置 → 配置 RPS 分散到所有 CPU
    ▼
echo ffffffff > /sys/class/net/eth0/queues/rx-0/rps_cpus
    →  验证：sar -u 1 → CPU 利用率均匀分布
```

---

## 工具能力矩阵

> 一张表看清每个工具能做什么、不能做什么。

### 按采集维度

| 维度 | vmstat | sar | iostat | pidstat | perf | bpftrace | strace | blktrace |
|------|--------|-----|--------|---------|------|----------|--------|----------|
| CPU 利用率 | ✓ | ✓ | ✘ | ✓ | ✓ | ✓ | ✘ | ✘ |
| CPU 时间分布 | ✘ | ✓ | ✘ | ✓ | ✓ | ✓ | ✘ | ✘ |
| 热点函数 | ✘ | ✘ | ✘ | ✘ | ✓ | ✓ | ✘ | ✘ |
| 调用栈 | ✘ | ✘ | ✘ | ✘ | ✓ | ✓ | ✘ | ✘ |
| 锁竞争 | ✘ | ✘ | ✘ | ✘ | ✓ | ✓ | ✘ | ✘ |
| 内存使用 | ✓ | ✓ | ✘ | ✓ | ✘ | ✓ | ✘ | ✘ |
| 内存分配追踪 | ✘ | ✘ | ✘ | ✘ | ✘ | ✓ | ✘ | ✘ |
| 缺页中断 | ✓ | ✓ | ✘ | ✘ | ✓ | ✓ | ✘ | ✘ |
| 磁盘利用率 | ✘ | ✓ | ✓ | ✘ | ✘ | ✘ | ✘ | ✓ |
| I/O 延迟分解 | ✘ | ✘ | ✓ | ✘ | ✘ | ✓ | ✘ | ✓ |
| I/O 生命周期 | ✘ | ✘ | ✘ | ✘ | ✘ | ✓ | ✘ | ✓ |
| 网络带宽 | ✘ | ✓ | ✘ | ✘ | ✘ | ✘ | ✘ | ✘ |
| 网络包分析 | ✘ | ✓ | ✘ | ✘ | ✘ | ✓ | ✘ | ✘ |
| TCP 连接 | ✘ | ✘ | ✘ | ✘ | ✘ | ✓ | ✘ | ✘ |
| 软中断 | ✘ | ✓ | ✘ | ✘ | ✓ | ✓ | ✘ | ✘ |
| 系统调用 | ✘ | ✘ | ✘ | ✘ | ✓ | ✓ | ✓ | ✘ |
| 调度延迟 | ✓ | ✘ | ✘ | ✘ | ✓ | ✓ | ✘ | ✘ |
| 硬件计数器 | ✘ | ✘ | ✘ | ✘ | ✓ | ✘ | ✘ | ✘ |

### 按工具特性

| 特性 | vmstat | sar | perf | bpftrace | strace | blktrace |
|------|--------|-----|------|----------|--------|----------|
| 历史数据 | ✘ | ✓ (sa 文件) | ✓ (perf.data) | ✘ | ✘ | ✓ (trace 文件) |
| 实时监控 | ✓ | ✓ | ✓ (perf top) | ✓ | ✓ | ✘ |
| 长期采集 | ✘ | ✓ (sadc) | ✘ | ✘ | ✘ | ✘ |
| 低开销 | ✓ | ✓ | ⚠ | ✓ | ✘ | ⚠ |
| 可编程 | ✘ | ✘ | ✘ | ✓ | ✘ | ✘ |
| 调用栈 | ✘ | ✘ | ✓ | ✓ | ✓ | ✘ |
| 聚合统计 | ✘ | ✓ | ✓ | ✓ | ✓ (-c) | ✓ (btt) |
| 安装难度 | 低 | 低 (sysstat) | 中 (perf) | 高 (bpf) | 低 | 高 (编译) |
| 学习曲线 | 低 | 低 | 中 | 高 | 低 | 高 |

### 按问题类型推荐组合

| 问题类型 | 推荐组合（由简到繁） |
|---------|---------------------|
| **CPU 热点** | `top` → `pidstat -u` → `perf record -g` → `perf report` |
| **锁竞争** | `sar -u` → `perf record :k` → `perf lock` → `bpftrace spinlock` |
| **内存不足** | `free` → `vmstat 1` → `sar -r` → `/proc/slabinfo` → `bpftrace kmem` |
| **I/O 延迟** | `iostat -x` → `pidstat -d` → `blktrace` → `btt` |
| **网络 CPU** | `sar -n DEV` → `/proc/softirqs` → `perf record :k` → `bpftrace softirq` |
| **全局慢** | `vmstat` + `sar -q` → `dmesg` → `perf stat -a` → 多维度对比 |
| **间歇性卡顿** | `sar` (长时间采集) → 找到异常时段 → 针对性深入 |

---

## 常用命令速查

```bash
# ═══════════════════════════════════════════════════════
#  万能开局五件套（任何性能问题先跑这五个）
# ═══════════════════════════════════════════════════════

vmstat 1              # CPU/内存/I/O 概览
sar -u ALL 1          # CPU 时间分布
iostat -xz 1          # 磁盘 I/O
sar -n DEV 1          # 网络流量
sar -q 1              # 负载队列

# ═══════════════════════════════════════════════════════
#  确定维度后的深入命令
# ═══════════════════════════════════════════════════════

# CPU 方向
pidstat -u -t 1       # 找到高 CPU 的线程
perf top -p PID       # 实时看热点函数
perf record -g -p PID # 采样调用栈

# 内存方向
sar -r 1              # 内存使用详情
sar -B 1              # 页表活动
cat /proc/slabinfo | sort -k3 -rn | head  # slab 使用

# I/O 方向
pidstat -d 1          # 高 I/O 进程
blktrace -d DEV -o out -w 10  # I/O 追踪
btt -i out.blktrace.0         # 延迟分解

# 网络方向
ss -s                 # 连接状态
cat /proc/softirqs    # 软中断分布
sar -n EDEV 1         # 网络错误

# 锁竞争方向
perf lock record -a -- sleep 10
perf lock report

# ═══════════════════════════════════════════════════════
#  生产环境安全命令（开销 < 1%）
# ═══════════════════════════════════════════════════════

vmstat 1
sar -u ALL 1 5
iostat -xz 1 5
pidstat -u 1 5
perf stat -a sleep 10
ss -s
dmesg | tail -50
```

---

> **总结：** 工具选型的核心是**问题驱动**——先确定瓶颈维度（CPU/内存/IO/网络），  
> 再用最轻量的工具确认方向，最后用精确工具定位根因。  
> 永远记住：**strace 是最后的手段，不是第一选择**。
