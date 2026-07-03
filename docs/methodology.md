# Linux 系统性能分析方法论

> 从"不知道 CPU 在干嘛"到"精确定位瓶颈根因"的系统化方法

---

## 核心思想：逐层排除法

性能分析不是"猜哪里有问题然后去验证"，而是**从宏观到微观、逐层排除**：

```
第一层：宏观指标（哪里有问题？）
  sar / vmstat / top / mpstat
  → 确定瓶颈维度：CPU? 内存? I/O? 网络?

第二层：维度深入（为什么有问题？）
  CPU → perf / pidstat / strace
  内存 → /proc/vmstat / slabinfo / perf kmem
  I/O → iostat / blktrace / btt
  网络 → netstat / ss / sar -n / tcpdump

第三层：内核机制（根因是什么？）
  锁竞争 → perf lock / bpftrace spinlock 追踪
  内存分配器 → /proc/slabinfo / bpftrace kmem
  调度器 → perf sched / bpftrace sched 追踪
  网络协议栈 → bpftrace / perf trace / tcpdump

第四层：源码级理解（能不能优化？）
  内核源码 → 理解瓶颈机制 → 提出优化方案
```

---

## 第一章：方法论框架

### 1.1 USE 方法（Brendan Gregg）

每个资源检查三个维度：

| 维度 | 含义 | 工具 |
|------|------|------|
| **U**tilization | 资源使用率（忙碌时间百分比） | top, mpstat, iostat, sar |
| **S**aturation | 资源饱和度（排队等待量） | vmstat r, iostat avgqu-sz, sar -q |
| **E**rrors | 错误计数 | dmesg, sar -n EDEV, iostat |

### 1.2 分析流程（决策树）

```
系统性能不佳
    │
    ├── sar -u 1：CPU 分布是什么？
    │   │
    │   ├── %user 高 → 应用层计算密集
    │   │   └── perf record → 找热点函数
    │   │
    │   ├── %system 高 → 内核态开销大 ★ 你的常见问题
    │   │   ├── perf record -e cycles:k → 内核热点函数
    │   │   ├── perf lock record → 锁竞争？
    │   │   ├── bpftrace kmem → 内存分配热点？
    │   │   └── bpftrace softirq → 软中断热点？
    │   │
    │   ├── %iowait 高 → I/O 等待
    │   │   └── iostat -x → blktrace → btt
    │   │
    │   ├── %steal 高 → 虚拟化争抢
    │   │   └── 宿主机问题，联系云平台
    │   │
    │   └── %idle 高但吞吐低 → 应用层问题
    │       └── 单线程瓶颈？锁等待？外部依赖？
    │
    ├── sar -q 1：负载队列如何？
    │   ├── runq-sz > CPU 核数 → 过载
    │   └── blocked > 0 → I/O 瓶颈
    │
    └── vmstat 1：内存/交换如何？
        ├── si/so > 0 → swap 活跃 → 内存不足
        └── free 持续下降 → 内存泄漏
```

### 1.3 "CPU 高但吞吐低"的专项分析

这是你最常遇到的问题。核心思路：**CPU 时间在花在哪里？**

```
Step 1: 确定 CPU 时间分布
  perf stat -e cycles,instructions,cache-misses,cache-references,\
            branches,branch-misses -p PID sleep 10
  
  关键指标：
  - IPC < 1 → 大量 stall（等待内存/锁/分支预测）
  - cache-miss rate > 5% → 内存访问是瓶颈
  - branch-miss rate > 5% → 分支预测失败多

Step 2: 确定 stall 类型
  perf stat --topdown-l1 -p PID sleep 10
  （AMD 上用 perf stat -d 替代）
  
  - Backend Bound + Memory Bound → cache miss / 内存延迟
  - Backend Bound + Core Bound → 计算单元不够 / 依赖链
  - Frontend Bound → 取指瓶颈（ICache miss / iTLB miss）
  - Bad Speculation → 分支预测失败

Step 3: 如果怀疑锁竞争
  perf lock record -a sleep 10
  perf lock report
  
  看 Wait Time Total 最高的锁 → 找到锁的内核函数

Step 4: 如果怀疑内存分配
  bpftrace -e '
    kprobe:kmem_cache_alloc { @alloc[kstack(3)] = count(); }
    kprobe:kfree { @free[kstack(3)] = count(); }
    interval:s:5 { exit(); }
  '
  
  看哪个内核路径分配/释放最频繁

Step 5: 如果怀疑调度问题
  perf sched record sleep 10
  perf sched latency
  
  看哪些线程等待调度时间最长
```

---

## 第二章：CPU 性能分析

### 2.1 CPU 时间去哪了？

```
CPU 时间分解：
┌─────────────────────────────────────────────┐
│ 用户态 (%user)                                │
│   ├── 应用计算（正常）                        │
│   ├── 用户态锁竞争（pthread_mutex/futex）    │
│   └── 系统调用开销（syscall entry/exit）      │
├─────────────────────────────────────────────┤
│ 内核态 (%system)                              │
│   ├── 内存分配/释放 (kmalloc/kfree)           │
│   ├── 锁竞争 (spinlock/rwlock/mutex)          │
│   ├── 系统调用处理 (vfs/block/network)        │
│   ├── 中断处理 (硬中断 + 软中断)              │
│   ├── 调度器开销 (context switch)             │
│   └── 页表管理 (TLB shootdown)                │
├─────────────────────────────────────────────┤
│ I/O 等待 (%iowait)                            │
│   └── 等待 I/O 完成（磁盘/网络 DMA）          │
├─────────────────────────────────────────────┤
│ 空闲 (%idle)                                  │
│   ├── 真正空闲（HLT 指令）                    │
│   └── 等待锁（spin_lock 空转 → 表现为 %system）│
└─────────────────────────────────────────────┘
```

### 2.2 诊断工具链

| 问题 | 工具 | 命令 |
|------|------|------|
| CPU 各核利用率不均 | mpstat | `mpstat -P ALL 1` |
| 哪个进程占 CPU | pidstat | `pidstat -u 1` |
| 进程内哪个线程占 CPU | pidstat | `pidstat -t -p PID 1` |
| 内核态热点函数 | perf | `perf record -e cycles:k -a` |
| 用户态热点函数 | perf | `perf record -e cycles:u -p PID` |
| 调用栈分析 | perf | `perf report -g` |
| 实时热点 | perf | `perf top` |
| 系统调用分布 | strace | `strace -c -p PID` |
| 系统调用耗时 | strace | `strace -T -p PID` |
| 锁竞争 | perf lock | `perf lock record + report` |
| 调度延迟 | perf sched | `perf sched latency` |

### 2.3 常见模式与诊断

| 模式 | 症状 | 诊断 | 常见根因 |
|------|------|------|---------|
| **计算密集** | %user 高, IPC > 1 | perf record 看热点函数 | 算法复杂度、数据量大 |
| **锁竞争** | %system 高, 吞吐低 | perf lock, bpftrace spinlock | 全局锁、热锁、false sharing |
| **内存访问瓶颈** | IPC < 0.5, cache-miss 高 | perf stat -d, perf c2c | 数据结构不 cache 友好、NUMA |
| **系统调用开销** | %system 高, syscall 频繁 | strace -c, perf trace | 小 I/O、频繁 alloc/free |
| **中断风暴** | %system 高, si 高 | /proc/interrupts, bpftrace | 网络包过多、单队列网卡 |
| **调度开销** | %system 高, ctxsw 高 | vmstat, perf sched | 线程过多、CPU 亲和性差 |
| **NUMA 效应** | 部分核慢, remote access 高 | numastat, perf stat numa | 跨 NUMA 内存访问 |

---

## 第三章：锁竞争分析

### 3.1 Linux 内核中的锁类型

| 锁类型 | 源码 | 特点 | 适用场景 |
|--------|------|------|---------|
| **spinlock** | `spin_lock()` | 忙等待，不睡眠 | 短临界区，中断上下文 |
| **rwlock** | `read_lock()` | 读共享，写互斥 | 读多写少 |
| **mutex** | `mutex_lock()` | 可睡眠，不可在中断中用 | 长临界区 |
| **rw_semaphore** | `down_read()` | 可睡眠读写锁 | 长临界区，读多写少 |
| **RCU** | `rcu_read_lock()` | 无锁读，延迟释放 | 极高频读，极低频写 |
| **atomic** | `atomic_inc()` | 无锁原子操作 | 计数器、标志位 |

### 3.2 spinlock 为什么导致 "CPU 高但吞吐低"

```
CPU 0: spin_lock(&lock)     ← 获取锁成功，进入临界区
CPU 1: spin_lock(&lock)     ← 获取失败，空转等待（消耗 CPU 但不做有用功！）
CPU 2: spin_lock(&lock)     ← 获取失败，空转等待
CPU 3: spin_lock(&lock)     ← 获取失败，空转等待

结果：
  sar -u 显示：4 个 CPU 都是 100% 利用率
  实际吞吐：只有 CPU 0 在做有用功
  → CPU 利用率 400%，但吞吐 ≈ 1 个 CPU

在 perf record 中：
  _raw_spin_lock 占比 60%+ → 确认是锁竞争
  实际业务函数占比 < 20% → 有用功少
```

### 3.3 锁竞争诊断流程

```
Step 1: perf lock record -a -- sleep 10
        perf lock report
        
        看 Wait Time Total 最高的锁
        → 确认是哪把锁

Step 2: perf record -e cycles:k -a -- sleep 10
        perf report
        
        看 _raw_spin_lock* 函数的占比
        → > 20% 说明锁竞争严重

Step 3: bpftrace 追踪锁的调用栈
        bpftrace -e '
          kprobe:_raw_spin_lock_irqsave {
            @[kstack(5)] = count();
          }
          interval:s:5 { exit(); }
        '
        → 找到是哪个内核路径在频繁获取锁

Step 4: 根据调用栈定位具体子系统
        - mm/ 相关 → 内存分配器锁（zone->lock, slab lock）
        - net/ 相关 → 网络协议栈锁（sk_lock, qdisc lock）
        - fs/ 相关 → 文件系统锁（inode->i_lock, dcache lock）
        - block/ 相关 → 块设备锁（queue lock）
```

### 3.4 false sharing（伪共享）

```
问题：两个 CPU 修改不同变量，但变量在同一 cache line (64 bytes) 中

Cache Line (64 bytes):
┌──────────────┬──────────────┬──────────┐
│ counter_cpu0 │ counter_cpu1 │ padding  │
│  (CPU 0 写)  │  (CPU 1 写)  │          │
└──────────────┴──────────────┴──────────┘

CPU 0 写 counter_cpu0 → cache line 失效 → CPU 1 需要重新从 L3 加载
CPU 1 写 counter_cpu1 → cache line 失效 → CPU 0 需要重新从 L3 加载
→ cache line 在两个 CPU 之间反复传递（"乒乓效应"）

检测：perf c2c record -a -- sleep 10
      perf c2c report
      → 看 HITM (Hit Modified) 最高的内存地址
```

---

## 第四章：内存子系统分析

### 4.1 内存层次结构与延迟

```
          容量        延迟         带宽
L1 Cache  32-64KB    ~1ns         ~2TB/s/core
L2 Cache  256-512KB  ~3-5ns       ~1TB/s/core
L3 Cache  32MB(CCD)  ~10-15ns     ~500GB/s/socket
DRAM      128-512GB  ~60-100ns    ~50-100GB/s/socket
NVMe SSD  TB 级      ~10-100μs    ~5-7GB/s/device
HDD       TB 级      ~5-10ms      ~100-200MB/s

关键认知：
  L3 → DRAM 延迟跳 6-10 倍
  DRAM → SSD 延迟跳 100-1000 倍
  → 每减少一层 cache miss，性能提升显著
```

### 4.2 内存性能指标

```bash
# 1. Cache miss 率
perf stat -e cache-misses,cache-references -p PID sleep 10
# cache-miss rate = cache-misses / cache-references
# < 1% 优秀, 1-5% 正常, > 5% 需要优化, > 10% 严重

# 2. TLB miss
perf stat -e dTLB-load-misses,dTLB-loads,iTLB-load-misses -p PID sleep 10

# 3. NUMA 远程访问
perf stat -e numa_mem_loads_local,numa_mem_loads_remote -p PID sleep 10
# remote/local > 0.2 说明 NUMA 配置有问题

# 4. 内存分配速率
grep pgalloc /proc/vmstat    # 每秒调用 watch -n1 看增量
# pgpgin/pgpgout → 页分配/释放速率

# 5. slab 使用
cat /proc/slabinfo | sort -k3 -rn | head -20
# 看哪些内核对象占用内存最多
```

### 4.3 内核内存分配器架构

```
kmalloc(size)
    │
    ├── size ≤ 8KB → SLUB 分配器
    │   │
    │   ├── per-CPU freelist (无锁，最快)
    │   │   └── 预分配的对象列表，每个 CPU 独立
    │   │
    │   ├── per-node partial list (node 锁)
    │   │   └── 部分使用的 slab 列表
    │   │
    │   └── 全局 slab 分配 (zone 锁，最慢)
    │       └── 从 buddy allocator 分配新页
    │
    └── size > 8KB → 伙伴系统（Buddy Allocator）
        │
        └── zone->lock (全局锁!)
            └── 每个 zone (DMA/NORMAL/HIGHMEM) 一把锁

瓶颈点：
  高频 kmalloc/kfree → per-CPU freelist 频繁耗尽
  → fallback 到 node partial（需要 node 锁）
  → 或 fallback 到 buddy allocator（需要 zone 锁）
  → 多核竞争全局锁 → CPU 高但吞吐低
```

---

## 第五章：网络协议栈分析

### 5.1 数据包处理流程

```
收包路径（从网卡到应用）：

网卡收到数据包
    │
    ▼
硬中断 → napi_schedule() → 关闭中断，进入软中断
    │
    ▼
软中断 NET_RX_SOFTIRQ
    │
    ├── 网卡驱动 napi_poll()  ← 从 ring buffer 取包
    ├── skb 分配               ← 分配 sk_buff 结构体
    ├── 协议栈处理              ← IP → TCP/UDP
    ├── netfilter/iptables     ← 防火墙规则
    └── 放入 socket 接收队列    ← 应用可读取

发包路径（从应用到网卡）：

应用 send()/write()
    │
    ├── 系统调用开销
    ├── TCP 协议处理           ← 拥塞控制、分段
    ├── skb 分配               ← 分配 sk_buff
    ├── netfilter/iptables
    ├── qdisc 排队              ← 队列调度
    └── 网卡驱动发送            ← 放入 ring buffer

每个环节都可能成为瓶颈！
```

### 5.2 网络性能诊断流程

```
Step 1: 宏观指标
  sar -n DEV 1        → 各网卡 pps 和带宽
  sar -n EDEV 1       → 错误/丢包率
  ss -s               → socket 统计摘要

Step 2: 如果吞吐正常但 CPU 高
  cat /proc/interrupts | grep eth    → 中断在各 CPU 的分布
  cat /proc/softirqs                 → 软中断在各 CPU 的分布
  
  → 集中在 1-2 个 CPU → RPS/多队列配置问题
  → 均匀分布但总量高 → 包速率太高，考虑 busy polling

Step 3: 如果软中断 CPU 高
  perf record -e cycles:k -a -- sleep 10
  perf report | grep -E "napi|net_rx|net_tx|ksoftirqd"
  
  → napi_poll 占比高 → 网卡驱动效率
  → __netif_receive_skb 占比高 → 协议栈处理开销
  → iptable 相关 → 防火墙规则过多

Step 4: 深入 skb 分配
  bpftrace -e '
    kprobe:__alloc_skb { @skb_alloc[kstack(3)] = count(); }
    kprobe:kfree_skb { @skb_free[kstack(3)] = count(); }
    interval:s:5 { exit(); }
  '
  → 看哪个路径分配/释放 skb 最多

Step 5: TCP 协议栈分析
  bpftrace -e '
    kprobe:tcp_sendmsg { @send_bytes = hist(arg2); }
    kprobe:tcp_recvmsg { @recv_bytes = hist(arg2); }
    kprobe:tcp_retransmit_skb { @retrans = count(); }
    interval:s:5 { exit(); }
  '
```

### 5.3 常见网络性能问题

| 问题 | 症状 | 诊断 | 优化 |
|------|------|------|------|
| 软中断不均衡 | 1-2 个 CPU 100%，其他空闲 | /proc/interrupts | RPS 或 多队列网卡 |
| 包速率瓶颈 | pps 上不去但带宽有余 | sar -n DEV | 增大 MTU（jumbo frame） |
| TCP 重传多 | 吞吐波动大 | ss -ti, sar -n ETCP | 检查网络质量、调 TCP 参数 |
| skb 分配热点 | %system 高 | bpftrace skb | GRO/GSO 优化 |
| 连接数过多 | TIME_WAIT 多 | ss -s | tcp_tw_reuse, 连接池 |
| 锁竞争 | qdisc lock | perf lock | 多队列 qdisc (mq/fq) |

---

## 第六章：AMD 架构专项

### 6.1 AMD Zen 3/4 拓扑

```
Socket (如 EPYC 7763)
├── CCD 0 ─── 8 核共享 32MB L3
├── CCD 1 ─── 8 核共享 32MB L3
├── CCD 2 ─── 8 核共享 32MB L3
├── ...
├── CCD 7 ─── 8 核共享 32MB L3
├── Memory Controller (8 通道)
└── Infinity Fabric (CCD 间互联)

关键延迟：
  同 CCD 核间通信:     ~10ns (通过 L3)
  跨 CCD 核间通信:     ~80ns (通过 Infinity Fabric)
  → 跨 CCD 代价是同 CCD 的 8 倍！

性能影响：
  1. 锁竞争：如果锁的 cache line 在 CCD 间传递，延迟 ×8
  2. false sharing：跨 CCD 的 false sharing 代价更大
  3. NUMA：NPS 配置影响内存访问延迟
```

### 6.2 NPS（Node Per Socket）配置

| 模式 | NUMA 节点数 | 内存控制器/节点 | 特点 |
|------|------------|----------------|------|
| NPS1 | 1 | 8 通道共享 | 最大内存带宽，延迟均匀 |
| NPS2 | 2 | 4 通道/节点 | 平衡带宽和延迟 |
| NPS4 | 4 | 2 通道/节点 | 最低延迟，但带宽分散 |

### 6.3 AMD 性能调优建议

```bash
# 1. 查看 CCD 拓扑
lscpu | grep -E "Thread|Core|Socket|NUMA|L3"
lstopo

# 2. 绑定进程到同一 CCD（避免跨 CCD 通信）
taskset -c 0-7 ./program    # 只在 CCD 0 的 8 核上运行

# 3. NUMA 内存分配策略
numactl --cpunodebind=0 --membind=0 ./program  # CPU 和内存都在 node 0

# 4. AMD 特定的 perf 事件
perf list | grep -i amd
# AMD IBS (Instruction Based Sampling) — 精确采样
perf record -e ibs_op// -c 100000 -a -- sleep 10

# 5. 检查跨 NUMA 访问比例
perf stat -e numa_mem_loads_local,numa_mem_loads_remote -a sleep 10
# remote / (local + remote) > 20% → 需要优化 NUMA 绑定
```

---

## 第七章：工具箱速查

### 7.1 按场景选工具

| 场景 | 首选工具 | 深入工具 | 源码级工具 |
|------|---------|---------|-----------|
| **CPU 高但吞吐低** | perf stat -d | perf record + report | bpftrace kstack |
| **锁竞争** | perf lock | bpftrace spinlock | perf c2c (false sharing) |
| **内存分配热点** | /proc/slabinfo | bpftrace kmem | perf kmem |
| **Cache miss** | perf stat cache-* | perf c2c | perf mem |
| **软中断高** | /proc/softirqs | bpftrace softirq | perf record :k |
| **网络吞吐低** | sar -n DEV | ss -ti | bpftrace tcp |
| **I/O 延迟高** | iostat -x | blktrace + btt | bpftrace block |
| **NUMA 问题** | numastat | perf stat numa-* | numactl |
| **调度延迟** | vmstat r | perf sched | bpftrace sched |
| **系统调用开销** | strace -c | perf trace | bpftrace syscall |

### 7.2 万能诊断三步法

```
第一步：定位维度（5 分钟）
  sar -u 1; sar -q 1; vmstat 1; iostat -x 1; sar -n DEV 1
  → 确定是 CPU/内存/IO/网络 的哪个

第二步：定位组件（15 分钟）
  perf record -e cycles:k -a sleep 10; perf report
  → 找到占比最高的内核函数

第三步：定位根因（30 分钟+）
  根据函数名确定子系统 → bpftrace 深入追踪 → 源码分析
  → 找到根因 → 提出优化方案
```
