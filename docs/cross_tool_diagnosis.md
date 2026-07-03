# 跨工具性能诊断工作流

> 从"系统变慢了"到"精确定位根因"的实战诊断流程。  
> 每个流程包含：实际命令、预期输出解读、判断条件、下一步决策。

---

## 目录

- [诊断哲学：为什么需要跨工具组合](#诊断哲学为什么需要跨工具组合)
- [问题 1：CPU 高但吞吐低](#问题-1cpu-高但吞吐低)
- [问题 2：I/O 延迟高](#问题-2io-延迟高)
- [问题 3：网络吞吐正常但 CPU 高](#问题-3网络吞吐正常但-cpu-高)
- [问题 4：内存压力大](#问题-4内存压力大)
- [问题 5：系统整体变慢（无明确瓶颈）](#问题-5系统整体变慢无明确瓶颈)
- [工具组合速查表](#工具组合速查表)

---

## 诊断哲学：为什么需要跨工具组合

单个工具只能看到一个切面。性能诊断的核心是**多工具交叉验证**：

```
                    ┌─────────────┐
                    │  问题现象    │
                    │ "系统变慢"   │
                    └──────┬──────┘
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ vmstat 1 │ │ sar -u 1 │ │ iostat -x│   ← 第一层：宏观扫描
        └────┬─────┘ └────┬─────┘ └────┬─────┘
             │            │            │
             ▼            ▼            ▼
        ┌──────────────────────────────────────┐
        │         确定瓶颈维度                   │   ← 判断方向
        │   CPU? 内存? I/O? 网络? 调度?          │
        └──────────────┬───────────────────────┘
                       │
          ┌────────────┼────────────┐
          ▼            ▼            ▼
    ┌──────────┐ ┌──────────┐ ┌──────────┐
    │  perf    │ │ blktrace │ │ pidstat  │   ← 第二层：维度深入
    │  record  │ │ + btt    │ │ -u/-d    │
    └────┬─────┘ └────┬─────┘ └────┬─────┘
         │            │            │
         ▼            ▼            ▼
    ┌──────────────────────────────────────┐
    │         定位热点函数/进程              │   ← 锁定目标
    └──────────────┬───────────────────────┘
                   │
          ┌────────┼────────┐
          ▼        ▼        ▼
    ┌────────┐ ┌────────┐ ┌────────┐
    │bpftrace│ │perf    │ │ strace │   ← 第三层：机制追踪
    │        │ │ lock   │ │        │
    └───┬────┘ └───┬────┘ └───┬────┘
        │          │          │
        ▼          ▼          ▼
    ┌──────────────────────────────────────┐
    │         根因确认 + 修复方案            │
    └──────────────────────────────────────┘
```

**关键原则：**

| 原则 | 说明 |
|------|------|
| ★ **先宏观后微观** | 先用 vmstat/sar 确定维度，再用 perf/bpftrace 深入 |
| ★ **先无侵入后侵入** | 先用 /proc 文件系统和低开销工具，再考虑 strace/bpftrace |
| ★ **交叉验证** | 一个工具的结论要用另一个工具验证（例如 perf 说锁竞争 → 用 bpftrace 确认） |
| ★ **先排除再深入** | 排除法比猜测法高效 10 倍 |

---

## 问题 1：CPU 高但吞吐低

> 这是最常见的性能问题。现象：CPU 利用率接近 100%，但业务吞吐量远低于预期。

### 诊断流程图

```
CPU 高但吞吐低
    │
    ├── Step 1: vmstat 1 → 看 r/cs/in 确认是 CPU 问题还是调度问题
    │
    ├── Step 2: sar -u ALL 1 → 区分 %user / %system / %iowait / %steal
    │   │
    │   ├── %user 高 → 应用计算密集 → Step 3A
    │   ├── %system 高 → 内核态开销 → Step 3B
    │   ├── %iowait 高 → I/O 等待 → 转"问题 2：I/O 延迟高"
    │   └── %steal 高 → 虚拟化争抢 → 宿主机问题，非本诊断范围
    │
    ├── Step 3A (应用层): pidstat -u -t 1 → perf record -g -p PID
    │   │
    │   └── 找到热点函数 → 算法优化 / 数据结构优化
    │
    ├── Step 3B (内核态): perf record -e cycles:k -a
    │   │
    │   ├── _raw_spin_lock* 占比高 → Step 4: perf lock（锁竞争）
    │   ├── kmem_cache_alloc 占比高 → Step 5: bpftrace kmem（内存分配）
    │   ├── __softirqentry_text_start 占比高 → 转"问题 3"
    │   └── __schedule / pick_next 占比高 → 调度开销
    │
    ├── Step 4: perf lock record → perf lock report
    │   └── 找到热点锁 → bpftrace 追踪锁的调用栈 → 源码分析
    │
    ├── Step 5: bpftrace kmem → 找到分配/释放热点
    │   └── slab 热点 → /proc/slabinfo 验证 → 内存池化
    │
    └── Step 6: perf stat --topdown-l1 → 硬件层面瓶颈
        ├── Backend Bound → cache miss / 内存延迟
        ├── Frontend Bound → 取指瓶颈
        └── Bad Speculation → 分支预测失败
```

### Step 1：宏观确认 — vmstat 1

```bash
vmstat 1
```

**预期输出与解读：**

```
procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----
 r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st
12  0      0 819200 256000 1280000   0    0     4    20  45000 89000 45 52  1  2  0
 ^                                              ^     ^    ^     ^    ^   ^
 │                                              │     │    │     │    │   │
 │                                              │     │    │     │    │   └── %system 高 → 内核态开销
 │                                              │     │    │     │    │       ★ 关键指标！
 │                                              │     │    │     │    └── %user
 │                                              │     │    │     └── 上下文切换数（高 → 线程过多/锁竞争）
 │                                              │     │    └── 中断数
 │                                              │     └── 写出 blocks/s
 │                                              └── 读入 blocks/s
 └── 运行队列（高 → CPU 饱和，多进程争抢）
```

**判断条件：**

| 指标 | 正常 | 异常 | 含义 |
|------|------|------|------|
| `r` | < CPU 核数 | > CPU 核数 × 2 | CPU 过载 |
| `cs` | < 10000/s | > 50000/s | 上下文切换频繁 |
| `in` | < 10000/s | > 50000/s | 中断风暴 |
| `wa` | < 5% | > 20% | I/O 等待严重 → 转"问题 2" |

### Step 2：CPU 时间分布 — sar -u ALL 1

```bash
sar -u ALL 1 5
```

**关注列及决策：**

| 列 | 含义 | 如果高 → 下一步 |
|----|------|----------------|
| `%user` | 用户态 CPU | Step 3A：应用层热点分析 |
| `%system` | 内核态 CPU | Step 3B：内核热点分析 ★ |
| `%iowait` | I/O 等待 | 转"问题 2：I/O 延迟高" |
| `%steal` | 虚拟化争抢 | 联系宿主机/云平台 |
| `%nice` | 低优先级任务 | 检查 nice 设置 |

```
预期输出示例（%system 高的场景）：
Average:     CPU    %usr   %nice    %sys %iowait   %irq   %soft  %steal  %guest  %gnice   %idle
Average:     all   15.20    0.00   52.80    2.10   0.00    3.50    0.00    0.00    0.00   26.40
                                    ^^^^                                      ^^^^
                                    内核态 52.8% → 异常高！                     软中断 3.5%
                                    → 进入 Step 3B                              → 检查网络
```

### Step 3A：应用层热点 — pidstat + perf record

```bash
# 找到高 CPU 的进程和线程
pidstat -u -t 1 5 | grep -v "Average" | sort -k7 -rn | head -20

# 对目标进程采样
perf record -g -p <PID> -- sleep 10

# 查看热点函数
perf report -g --stdio | head -50
```

**预期输出解读：**

```
# perf report 示例
Overhead  Command  Shared Object       Symbol
  45.20%  worker   app                 [.] compute_hash()      ← 应用函数热点
  22.10%  worker   libc.so.6           [.] __GI___strncasecmp  ← 字符串比较开销
  12.30%  worker   app                 [.] serialize_request() ← 序列化开销
   5.40%  worker   [kernel.kallsyms]   [k] copy_user_enhanced_fast_string ← syscall 开销
```

**决策：**
- 单个函数 > 50% → 优化该函数（算法/数据结构）
- 多个函数均匀分布 → 考虑整体架构优化
- `[kernel.kallsyms]` 占比高 → 系统调用开销 → 批量处理/减少 syscall

### Step 3B：内核态热点 — perf record -e cycles:k

```bash
perf record -e cycles:k -a -- sleep 10
perf report --stdio | head -50
```

**常见热点函数与诊断方向：**

| 热点函数 | 含义 | 下一步 |
|---------|------|--------|
| `_raw_spin_lock*` | 自旋锁竞争 | Step 4：perf lock |
| `kmem_cache_alloc` / `kfree` | 内存分配/释放 | Step 5：bpftrace kmem |
| `__schedule` / `pick_next_task_fair` | 调度开销 | perf sched |
| `__softirqentry_text_start` | 软中断处理 | 转"问题 3" |
| `page_fault` / `handle_mm_fault` | 缺页中断 | 检查内存映射 |
| `flush_tlb_func` / `native_flush_tlb` | TLB flush | 检查大页 |
| `native_queued_spin_lock_slowpath` | 队列锁等待 | perf lock |

### Step 4：锁竞争确认 — perf lock

```bash
perf lock record -a -- sleep 10
perf lock report
```

**预期输出：**

```
=== output for lock events ===

                Holder          ─────── Wait Time Total ────────
                                  Count       Time(ns)    Average(ns)

                kworker/u8:0        15200     892000000        58684
                worker/1234         12800     765000000        59765
                nginx/5678          11200     680000000        60714

=== Lock statistics ===

                Lock Name                  Address       Hold          Wait
                                                    Count  Time(ns)  Count  Time(ns)
                &(&zone->lock)->rlock      ffff88...   45200  ...    28900  1820000000  ★
                sk_lock-AF_INET             ffff88...   32100  ...    18500   920000000
```

**判断条件：**
- `Wait Time Total` 最高的锁 → 就是瓶颈
- 根据锁名确定子系统 → bpftrace 追踪调用栈 → 源码分析修复

```bash
# bpftrace 追踪锁的调用栈
bpftrace -e '
kprobe:_raw_spin_lock_irqsave {
    @[kstack(5)] = count();
}
interval:s:5 { exit(); }
'
```

### Step 5：内存分配热点 — bpftrace kmem

```bash
bpftrace -e '
kprobe:kmem_cache_alloc {
    @alloc[kstack(3)] = count();
}
kprobe:kfree {
    @free[kstack(3)] = count();
}
interval:s:5 { exit(); }
'
```

**预期输出与验证：**

```bash
# 同时用 /proc/slabinfo 验证
cat /proc/slabinfo | sort -k3 -rn | head -10
```

| 发现 | 含义 | 修复方向 |
|------|------|---------|
| kmalloc-256 最频繁 | 256 字节对象频繁分配 | 对象池/内存池 |
| radix_tree_node 最多 | page cache 基数树节点 | 内存压力大 |
| skbuff_head_alloc 最多 | skb 分配频繁 | 转"问题 3" |

### Step 6：硬件层面瓶颈 — perf topdown

```bash
perf stat --topdown-l1 -p <PID> -- sleep 10
```

**预期输出与解读：**

```
Topdown accuracy may decrease with increasing multiplexing.

         retiring  bad speculatn  frontend bound  backend bound       # CPU
           15.2%          8.5%          12.3%          64.0%       S0-C0   ★

# Backend Bound 64% → CPU 流水线后端停顿严重
# 进一步拆分：
perf stat --topdown-l2 -p <PID> -- sleep 10
# → Memory Bound vs Core Bound
```

| L1 指标 | 含义 | 修复方向 |
|---------|------|---------|
| Backend Bound > 40% | 数据/指令供给不足 | 优化内存访问模式 |
| Memory Bound > 30% | cache miss / 内存延迟 | perf c2c / 大页 / 预取 |
| Core Bound > 30% | 计算单元不足 / 依赖链 | 指令级并行优化 |
| Frontend Bound > 20% | 取指瓶颈 | ICache 优化 / iTLB 大页 |
| Bad Speculation > 15% | 分支预测失败 | 分支提示 / 数据排序 |

---

## 问题 2：I/O 延迟高

> 现象：应用响应慢，日志中出现 I/O 超时，用户感知到卡顿。

### 诊断流程图

```
I/O 延迟高
    │
    ├── Step 1: vmstat 1 → 看 wa/bi/bo
    │   ├── wa > 20% → I/O 瓶颈确认
    │   └── wa < 5% → 可能不是 I/O 问题 → 检查其他维度
    │
    ├── Step 2: iostat -xz 1 → 看 %util/await/avgqu-sz/r_await/w_await
    │   │
    │   ├── %util ≈ 100% → 设备饱和
    │   ├── await 高但 svctm 低 → 队列等待长（并发 I/O 多）
    │   ├── r_await >> w_await → 读延迟异常
    │   └── w_await >> r_await → 写延迟异常
    │
    ├── Step 3: pidstat -d 1 → 找到高 I/O 的进程
    │   └── 确定是哪个进程在做 I/O
    │
    ├── Step 4: blktrace + blkparse + btt → I/O 生命周期分析 ★
    │   │
    │   ├── Q2D 高 → 请求在队列中等待久 → I/O 调度/并发问题
    │   ├── D2C 高 → 设备处理慢 → 硬件瓶颈
    │   └── Q2C = Q2D + D2C → 总延迟
    │
    └── Step 5: perf lock → 是否有 I/O 路径的锁竞争
        └── qdisc lock / queue lock → 块设备/文件系统锁
```

### Step 1：I/O 等待确认 — vmstat 1

```bash
vmstat 1
```

**关注指标：**

| 指标 | 含义 | 异常阈值 |
|------|------|---------|
| `wa` | I/O 等待 CPU 百分比 | > 20% 严重 |
| `bi` | 读入 blocks/s | 持续增长 → 大量读 I/O |
| `bo` | 写出 blocks/s | 持续增长 → 大量写 I/O |
| `b` | 不可中断睡眠的进程数 | > 0 说明有进程在等 I/O |

```
procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----
 r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st
 2  8      0 512000 128000 2560000   0    0  128000    0 12000 8000  5 15  0 80  0
    ^                              ^^^^^^                              ^^^^^^^^
    8 个进程在等 I/O               读入 128K blocks/s                    I/O 等待 80%！
    → 严重 I/O 瓶颈               → 大量顺序读                          → 确认是 I/O 问题
```

### Step 2：设备级分析 — iostat -xz 1

```bash
iostat -xz 1
```

**关键输出列解读：**

```
Device   r/s     w/s   rkB/s   wkB/s  rrqm/s  wrqm/s  %rrqm  %wrqm r_await w_await
         aq-sz rareq-sz wareq-sz  svctm  %util
vdb     5200      0  208000       0       0       0    0.0    0.0    8.50    0.00
         44.20    40.00     0.00   0.15  78.00
         ^^^^    ^^^^                                      ^^^^
         │       │                                          │
         │       │                                          └── 读平均延迟 8.5ms → 高！
         │       └── 每秒 5200 读请求 → 高 IOPS
         └── 队列深度 44 → 大量请求在排队

         svctm 0.15ms → 设备实际处理很快
         await 8.5ms >> svctm 0.15ms → 等待时间占总延迟 98%！
         → 问题在排队等待，不在设备本身
```

**决策矩阵：**

| %util | await | svctm | 诊断 | 方向 |
|-------|-------|-------|------|------|
| ≈ 100% | 高 | 高 | 设备慢（硬件瓶颈） | 升级存储 / 换 SSD |
| ≈ 100% | 高 | 低 | 队列等待（并发过高） | 减少并发 / 调 I/O 调度器 |
| < 100% | 高 | 低 | I/O 调度延迟 | 检查 I/O 调度器 / 优先级 |
| 低 | 低 | 低 | 正常 | 不是 I/O 问题 |

### Step 3：找到 I/O 进程 — pidstat -d 1

```bash
pidstat -d 1 5 | sort -k5 -rn | head -10
```

**预期输出：**

```
Linux 5.10.0    hostname    07/04/2026     _x86_64_    (8 CPU)

   UID       PID    kB_rd/s   kB_wr/s kB_ccwr/s iodelay  Command
     0      1234   102400.00     0.00     0.00       0  fio              ← 这个进程
     0      5678     1024.00   512.00     0.00       0  jbd2/vdb-8       ← 日志写入
```

### Step 4：I/O 生命周期分析 — blktrace + btt ★

这是 I/O 诊断的核心工具链：

```bash
# 1. 启动 blktrace（在 /tmp 下运行，避免仓库产生二进制）
cd /tmp/blktrace_lab
/root/code/tool_learn/src/blktrace/blktrace -d /dev/vdb -o exp1 -w 10 &
sleep 1
# 2. 触发 I/O 负载
fio --name=test --ioengine=libaio --direct=1 --rw=randread --bs=4k \
    --filename=/dev/vdb --runtime=8 --time_based
wait

# 3. 解析 trace 数据
/root/code/tool_learn/src/blktrace/blkparse -i exp1.blktrace.0 -d exp1.disk_stats

# 4. btt 分析延迟
/root/code/tool_learn/src/blktrace/btt/btt -i exp1.blktrace.0
```

**btt 输出解读：**

```
==================== Device Information ====================
Devices analyzed: /dev/vdb

==================== Device /dev/vdb ====================

    Device          |      Q2D      |      D2C      |      Q2C
    ─────────────────────────────────────────────────────────────
    Reads           |    1.20 ms    |    0.15 ms    |    1.35 ms
    Writes          |    0.00 ms    |    0.00 ms    |    0.00 ms
    ─────────────────────────────────────────────────────────────

    Q2D = Queue to Dispatch（请求排队等待时间）
    D2C = Dispatch to Complete（设备实际处理时间）
    Q2C = Queue to Complete（总延迟 = Q2D + D2C）
```

**判断逻辑：**

| Q2D | D2C | 诊断 |
|-----|-----|------|
| 高 | 低 | ★ 队列等待 → I/O 调度器/并发度问题 |
| 低 | 高 | 设备慢 → 硬件瓶颈/存储介质问题 |
| 高 | 高 | 设备饱和 → 需要升级存储 |
| 低 | 低 | I/O 不是瓶颈 → 检查其他维度 |

```bash
# 进一步分析：I/O 合并情况
/root/code/tool_learn/src/blktrace/btt/btt -i exp1.blktrace.0 -l
# -l 选项显示 I/O 合并统计 → 合并率低说明 I/O 已经是随机的
```

### Step 5：I/O 路径锁竞争

```bash
# 如果 %system 也高，检查 I/O 路径是否有锁竞争
perf lock record -a -- sleep 10
perf lock report | grep -E "blk|queue|inode|dentry"
```

**常见 I/O 路径锁：**

| 锁 | 位置 | 场景 |
|----|------|------|
| `queue_lock` | 块设备层 | 高并发 I/O |
| `inode->i_lock` | VFS 层 | 大量文件操作 |
| `dcache_lock` | 目录缓存 | 目录遍历 |
| `jbd2_journal_lock` | ext4 日志 | 频繁 fsync |

---

## 问题 3：网络吞吐正常但 CPU 高

> 现象：网络带宽和 pps 看起来正常，但 CPU（尤其 %system 和 %soft）消耗很高。

### 诊断流程图

```
网络吞吐正常但 CPU 高
    │
    ├── Step 1: sar -n DEV 1 → 确认网络吞吐数据
    │   └── 确认 pps 和带宽在合理范围
    │
    ├── Step 2: cat /proc/softirqs → 软中断在各 CPU 的分布
    │   │
    │   ├── 集中在 1-2 个 CPU → RPS/多队列未配置
    │   └── 均匀但总量高 → 包处理效率问题
    │
    ├── Step 3: ss -s → TCP 连接状态分布
    │   ├── TIME_WAIT 过多 → 连接管理问题
    │   └── CLOSE_WAIT 过多 → 应用未关闭连接
    │
    ├── Step 4: bpftrace → 追踪 softirq / skb 分配热点
    │   └── 找到消耗 CPU 最多的内核路径
    │
    └── Step 5: perf record -e cycles:k → 网络协议栈热点函数
        ├── napi_poll 高 → 驱动效率
        ├── __netif_receive_skb 高 → 协议栈开销
        └── nf_hook 高 → 防火墙规则过多
```

### Step 1：网络吞吐确认 — sar -n DEV 1

```bash
sar -n DEV 1 5
```

**预期输出：**

```
IFACE   rxpck/s   txpck/s    rxkB/s    txkB/s   rxcmp/s   txcmp/s  rxmcst/s   %ifutil
  eth0   85000     42000    102400     51200       0         0         0        45.2
        ^^^^^      ^^^^^    ^^^^^^     ^^^^^
        收包 85K   发包 42K  收 100MB   发 50MB
        pps         pps      带宽        带宽

# pps 85K 看似不高，但如果每个包都要软中断处理
# → 85K × 每包处理开销 = CPU 消耗
```

**关键认知：**
- 带宽高但 pps 低 → 大包传输，效率高
- 带宽低但 pps 高 → 小包传输，CPU 开销大 ★

### Step 2：软中断分布 — /proc/softirqs

```bash
# 先看绝对值
cat /proc/softirqs

# 再看增量（每秒变化量）
watch -n 1 "cat /proc/softirqs | grep NET_RX"
```

**预期输出与判断：**

```
          CPU0       CPU1       CPU2       CPU3
NET_RX:  2800000    120000     115000     110000     ← 不均衡！CPU0 承担了 87%
           ^^^^^^^    ^^^^^^     ^^^^^^     ^^^^^^

NET_TX:   120000     115000     110000     105000     ← 均匀
```

| 分布情况 | 诊断 | 修复 |
|---------|------|------|
| 集中在 1 个 CPU | 单队列网卡 / RPS 未配置 | 启用 RPS / 多队列网卡 |
| 均匀但增量很高 | 包速率太高 | GRO/GSO / busy polling |
| 某个 CPU 明显低 | 中断亲和性配置 | 检查 smp_affinity |

```bash
# 配置 RPS（如果支持）
echo ffffffff > /sys/class/net/eth0/queues/rx-0/rps_cpus
# 让所有 CPU 参与软中断处理
```

### Step 3：TCP 连接状态 — ss -s

```bash
ss -s
```

**预期输出：**

```
Total: 15234
TCP:   12800 (estab 8200, closed 2400, orphaned 120, timewait 1800)

Transport Total     IP        IPv6
RAW       0         0         0
UDP       12        8         4
TCP       10400     8200      2200        ← 连接总数
INET      10412     8208      2204
FRAG      0         0         0
```

**判断条件：**

| 状态 | 正常范围 | 异常 | 含义 |
|------|---------|------|------|
| TIME_WAIT | < 5000 | > 20000 | 短连接过多 → 端口耗尽风险 |
| CLOSE_WAIT | < 100 | > 1000 | 应用未关闭连接 → bug |
| ESTABLISHED | 视业务而定 | 突增 | 连接风暴 |
| SYN_RECV | < 100 | > 1000 | SYN 攻击 / backlog 不足 |

```bash
# 更详细的连接分析
ss -ti state time-wait    # 查看 TIME_WAIT 连接的详情
ss -ti state established dst :443    # 查看到 443 端口的连接
```

### Step 4：内核网络路径追踪 — bpftrace

```bash
# 追踪 skb 分配和释放
bpftrace -e '
kprobe:__alloc_skb {
    @skb_alloc[kstack(3)] = count();
}
kprobe:kfree_skb {
    @skb_free[kstack(3)] = count();
    @drop_reason[arg1] = count();
}
kprobe:napi_complete_done {
    @napi_complete = count();
}
interval:s:5 { exit(); }
'
```

```bash
# 追踪软中断处理时间
bpftrace -e '
kprobe:__do_softirq {
    @start[tid] = nsecs;
}
kretprobe:__do_softirq /@start[tid]/ {
    @softirq_time = hist(nsecs - @start[tid]);
    delete(@start[tid]);
}
interval:s:5 { exit(); }
'
```

### Step 5：网络协议栈热点 — perf

```bash
perf record -e cycles:k -a -- sleep 10
perf report --stdio | grep -E "net|skb|napi|tcp|ip_|softirq"
```

**常见热点函数与优化方向：**

| 热点函数 | 含义 | 优化方向 |
|---------|------|---------|
| `napi_poll` / 驱动 poll 函数 | 网卡驱动收包 | 调整 ring buffer / coalesce |
| `__netif_receive_skb_core` | 协议栈收包路径 | RPS / GRO |
| `ip_rcv` / `ip_local_deliver` | IP 层处理 | 减少 netfilter hook |
| `nf_hook_slow` | netfilter 规则匹配 | 精简 iptables 规则 |
| `tcp_v4_rcv` | TCP 收包 | 调整 TCP 参数 |
| `__alloc_skb` / `__kmalloc_reserve` | skb 分配 | GRO 合并减少包数 |
| `kfree_skb` | skb 释放（含丢包） | 检查丢包原因 |

---

## 问题 4：内存压力大

> 现象：应用变慢，系统出现 swap 活动，OOM killer 触发，或频繁缺页中断。

### 诊断流程图

```
内存压力大
    │
    ├── Step 1: vmstat 1 → 看 si/so/free/buff/cache
    │   ├── si/so > 0 → 正在 swap → 物理内存不足
    │   └── free 持续下降 → 内存泄漏
    │
    ├── Step 2: sar -r 1 → 详细内存使用
    │   ├── %memused > 95% → 内存耗尽
    │   └── %commit > 100% → 超额承诺（可能被 OOM kill）
    │
    ├── Step 3: sar -B 1 → 页表活动
    │   ├── pgpgin/pgpgout 高 → 页分配/回收频繁
    │   ├── majflt/s 高 → 缺页中断（需从磁盘读）
    │   └── pgfree/s 高 → 频繁释放页面
    │
    ├── Step 4: /proc/slabinfo → 内核内存使用
    │   └── 找到占用最多的 slab 缓存
    │
    └── Step 5: bpftrace → 追踪 kmalloc/kfree 热点
        └── 找到分配/释放最频繁的内核路径
```

### Step 1：swap 活动确认 — vmstat 1

```bash
vmstat 1
```

**关注指标：**

```
procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----
 r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st
 4  2 1024000 64000 32000 512000  2048  1024  4096  2048 25000 15000 20 30  5 45  0
             ^^^^            ^^   ^^    ^^
             │               │    │     │
             │               │    │     └── 每秒 swap out 1024KB → 严重！
             │               │    └── 每秒 swap in 2048KB → 更严重！应用在等 swap 读回
             │               └── 可用内存仅 64MB → 物理内存耗尽
             └── 已使用 swap 1GB → 大量数据被换出
```

| 指标 | 正常 | 警告 | 严重 |
|------|------|------|------|
| `si` (swap in) | 0 | > 0 | > 1000 KB/s |
| `so` (swap out) | 0 | > 0 | > 500 KB/s |
| `free` | > 总内存 10% | < 总内存 5% | < 100MB |

### Step 2：详细内存分析 — sar -r

```bash
sar -r 1 5
```

**预期输出：**

```
kbmemfree   kbavail kbmemused  %memused  kbbuffers  kbcached  kbcommit   %commit  kbactive kbinact   kbdirty
   64000     256000  16384000     98.4      32000    512000   18000000    108.0   12800000  2560000     64000
   ^^^^^              ^^^^^^^^     ^^^^              ^^^^^^   ^^^^^^^^     ^^^^^
   │                  │            │                  │       │            │
   │                  │            │                  │       │            └── 超额承诺！可能触发 OOM
   │                  │            │                  │       └── 需要 18GB 但物理只有 16GB
   │                  │            │                  └── page cache 512MB
   │                  │            └── 内存使用 98.4%
   │                  └── 可用 256MB（含 buffer/cache 可回收部分）
   └── 真正空闲仅 64MB
```

### Step 3：页表活动 — sar -B

```bash
sar -B 1 5
```

**关键指标：**

```
pgpgin/s pgpgout/s   fault/s  majflt/s  pgfree/s pgscank/s pgscand/s pgsteal/s    %vmeff
  4096      2048     125000      2500    120000      8000         0      6000       75.0
  ^^^^      ^^^^     ^^^^^^      ^^^^    ^^^^^^      ^^^^                 ^^^^       ^^^^
  │         │        │           │       │           │                    │          │
  │         │        │           │       │           │                    │          └── 回收效率 75%
  │         │        │           │       │           └── kswapd 扫描页数（内存回收压力）
  │         │        │           │       └── 每秒释放 120K 页 → 频繁换入换出
  │         │        │           └── 缺页中断 2500/s → 需要从磁盘读！
  │         │        └── 总缺页 125K/s（含 minor fault）
  │         └── 写出页
  └── 读入页 → swap in 活动
```

| 指标 | 含义 | 异常阈值 |
|------|------|---------|
| `majflt/s` | 主要缺页（从磁盘读） | > 100/s 严重 |
| `pgscank/s` | kswapd 扫描页数 | > 10000/s 说明回收压力大 |
| `%vmeff` | 回收效率 | < 30% 说明回收困难 |
| `pgsteal/s` | 实际回收页数 | 与 pgscank 对比看效率 |

### Step 4：内核内存使用 — /proc/slabinfo

```bash
# 按 active_objs 排序，看哪些内核对象最多
cat /proc/slabinfo | sort -k3 -rn | head -20

# 按内存占用估算排序（active_objs × objsize）
awk 'NR>2 {printf "%s: %d objects × %d bytes = %d MB\n", $1, $3, $4, ($3*$4)/1048576}' \
    /proc/slabinfo | sort -t= -k2 -rn | head -20
```

**常见高占用 slab 与含义：**

| slab 缓存 | 含义 | 可能原因 |
|-----------|------|---------|
| `dentry` | 目录项缓存 | 大量文件/目录访问 |
| `inode_cache` | inode 缓存 | 文件数量多 |
| `buffer_head` | buffer cache 元数据 | 块设备 I/O 多 |
| `radix_tree_node` | page cache 基数树 | 大文件缓存 |
| `kmalloc-*` | 通用内核分配 | 需 bpftrace 深入 |
| `skbuff_head` | 网络包缓冲 | 网络流量大 |
| `task_struct` | 进程描述符 | 进程/线程数多 |

### Step 5：内存分配热点 — bpftrace

```bash
# 追踪内核内存分配
bpftrace -e '
kprobe:kmem_cache_alloc {
    @alloc_slab[kstack(5)] = count();
}
kprobe:__kmalloc {
    @alloc_kmalloc[kstack(5)] = count();
}
kprobe:kfree {
    @free[kstack(5)] = count();
}
interval:s:10 { exit(); }
'
```

```bash
# 追踪缺页中断路径
bpftrace -e '
kprobe:handle_mm_fault {
    @fault[kstack(5)] = count();
}
interval:s:10 { exit(); }
'
```

**补充：快速定位内存泄漏**

```bash
# 进程级内存追踪
pidstat -r 1 60 | sort -k5 -rn | head -10

# 查看进程的内存映射
cat /proc/<PID>/smaps_rollup

# 内核内存泄漏检测（如果有 kmemleak）
cat /sys/kernel/debug/kmemleak
```

---

## 问题 5：系统整体变慢（无明确瓶颈）

> 现象：用户反馈"系统慢了"，但 CPU/内存/IO/网络 都看不出明显异常。这是最棘手的情况。

### 诊断流程图

```
系统整体变慢（无明确瓶颈）
    │
    ├── Step 1: 全局扫描 — vmstat 1 + sar -q
    │   ├── 运行队列 r → CPU 饱和？
    │   ├── blocked 进程数 b → I/O 等待？
    │   └── 都正常 → 继续
    │
    ├── Step 2: 进程概览 — top / ps
    │   ├── 是否有 D 状态进程 → 内核阻塞
    │   ├── 是否有 zombie 进程 → 资源泄漏
    │   └── 进程数是否异常增长 → fork bomb / 连接泄漏
    │
    ├── Step 3: 内核日志 — dmesg | tail
    │   ├── OOM killer → 内存不足
    │   ├── hardware error → 硬件故障
    │   ├── soft lockup → 内核死锁
    │   └── rcu stall → RCU 回调堆积
    │
    ├── Step 4: 多维度同时看 — sar -u/-r/-d/-n
    │   └── 对比历史基线，找到偏离最大的指标
    │
    └── Step 5: 硬件计数器 — perf stat -a
        ├── IPC 异常低 → CPU 效率问题
        ├── cache miss 异常高 → 内存访问模式变化
        └── context switch 异常高 → 调度开销
```

### Step 1：全局扫描

```bash
# 同时运行多个采集，获取全面视图
vmstat 1 &
sar -q 1 &
wait
```

**vmstat 1 解读：**

| 关注点 | 正常 | 异常 | 含义 |
|--------|------|------|------|
| `r` (运行队列) | < CPU 核数 | > 核数 × 2 | CPU 过载 |
| `b` (阻塞进程) | 0 | > 0 | I/O 等待 |
| `cs` (上下文切换) | < 10000 | > 50000 | 线程过多/锁竞争 |
| `in` (中断) | < 10000 | > 50000 | 中断风暴 |

**sar -q 解读：**

```bash
sar -q 1 5
```

```
runq-sz  plist-sz   ldavg-1   ldavg-5  ldavg-15   blocked
    12       850      8.50      6.20      4.80         3
    ^^       ^^^      ^^^^                ^^^^         ^
    │        │        │                   │            │
    │        │        │                   │            └── 3 个进程在等 I/O
    │        │        │                   └── 15 分钟负载 → 趋势（上升中）
    │        │        └── 1 分钟负载 → 当前
    │        └── 进程总数
    └── 运行队列长度
```

### Step 2：进程概览

```bash
# 按 CPU 排序
ps aux --sort=-%cpu | head -20

# 按内存排序
ps aux --sort=-%mem | head -20

# 检查 D 状态（不可中断睡眠）进程
ps aux | awk '$8 ~ /D/ {print}'

# 检查 zombie 进程
ps aux | awk '$8 ~ /Z/ {print}'

# 进程/线程总数
ps -eLf | wc -l
```

**D 状态进程的含义：**

```
D 状态 = 不可中断睡眠（Uninterruptible Sleep）
  → 通常是在等 I/O（NFS 挂载卡死、磁盘故障、内核 bug）
  → kill -9 无法杀死 D 状态进程！
  → 大量 D 状态进程 → 内核级 I/O 阻塞
```

### Step 3：内核日志

```bash
dmesg | tail -100 | grep -iE "error|warn|fail|kill|lockup|stall|oom|bug"
```

**关键内核消息：**

| 消息 | 含义 | 严重程度 | 动作 |
|------|------|---------|------|
| `Out of memory: Kill process` | OOM killer 触发 | ★★★★★ | 增加内存/修复泄漏 |
| `BUG: soft lockup` | 软锁（CPU 被内核占住 > 20s） | ★★★★★ | 内核 bug/驱动问题 |
| `rcu_sched self-detected stall` | RCU 回调堆积 | ★★★★ | CPU 被长时间占用 |
| `INFO: task blocked for > 120s` | 任务阻塞超时 | ★★★★ | I/O 设备故障 |
| `MCE: Hardware Error` | 硬件错误（ECC/CPU） | ★★★★★ | 更换硬件 |
| `nf_conntrack: table full` | 连接跟踪表满 | ★★★ | 调大 nf_conntrack_max |

### Step 4：多维度对比

```bash
# 同时采集多维度数据
sar -u 1 10 > /tmp/sar_cpu.txt &
sar -r 1 10 > /tmp/sar_mem.txt &
sar -d 1 10 > /tmp/sar_disk.txt &
sar -n DEV 1 10 > /tmp/sar_net.txt &
wait

# 逐一检查，与历史基线对比
# 关注"变化率"而非"绝对值"
```

**对比思维：**

```
正常基线（上周同一时段）          当前值              偏差
─────────────────────────────────────────────────────────
CPU %user: 30%                   32%               +6% ✓ 正常
CPU %system: 5%                  18%              +260% ★ 异常！
Mem %used: 60%                   62%               +3% ✓ 正常
Disk %util: 20%                  22%              +10% ✓ 正常
Net rxpck: 50K                   48K               -4% ✓ 正常
cs/s: 5000                       45000            +800% ★ 异常！
```

### Step 5：全局硬件计数器

```bash
perf stat -a -e cycles,instructions,cache-misses,cache-references,\
            context-switches,cpu-migrations,page-faults -- sleep 10
```

**预期输出与解读：**

```
Performance counter stats for 'system wide':

    48,000,000,000      cycles                    # 4.8 GHz × 10s × 8 cores
    12,000,000,000      instructions              # 0.25 IPC  ← 很低！
         850,000,000      cache-misses             # 7.08% of cache-references
      12,000,000,000      cache-references
          450,000,000      context-switches         # 45M/s ← 很高！
            2,500,000      cpu-migrations           # 250K/s ← 进程在 CPU 间迁移
           15,000,000      page-faults              # 1.5M/s
```

**诊断逻辑：**

| 指标 | 正常 | 异常 | 含义 |
|------|------|------|------|
| IPC | > 1.0 | < 0.5 | CPU 效率低，大量 stall |
| cache-miss rate | < 1% | > 5% | 内存访问模式问题 |
| context-switches/s | < 10K | > 50K | 线程/锁问题 |
| cpu-migrations/s | < 1K | > 10K | CPU 亲和性差 |

```bash
# 如果 IPC 低但 cache-miss 不高 → 可能是锁竞争
perf lock record -a -- sleep 10
perf lock report

# 如果 cpu-migrations 高 → 检查 CPU 亲和性
perf record -e migrations -a -- sleep 10
perf report
```

---

## 工具组合速查表

### 按瓶颈维度的工具链

| 瓶颈维度 | 第一层（宏观扫描） | 第二层（维度深入） | 第三层（根因定位） |
|---------|-------------------|-------------------|-------------------|
| **CPU 计算** | vmstat, sar -u | pidstat -u, perf top | perf record -g, perf topdown |
| **CPU 锁竞争** | sar -u (%system 高) | perf lock record | bpftrace spinlock, perf c2c |
| **内存不足** | vmstat (si/so) | sar -r, sar -B | bpftrace kmem, /proc/slabinfo |
| **内存泄漏** | vmstat (free 下降) | pidstat -r | valgrind, kmemleak |
| **I/O 延迟** | vmstat (wa), iostat | blktrace + blkparse | btt (Q2D/D2C 分析) |
| **I/O 饱和** | iostat (%util) | blktrace | btt -l (合并分析) |
| **网络吞吐** | sar -n DEV | ss -ti | bpftrace tcp |
| **网络 CPU** | sar -n + /proc/softirqs | perf record :k | bpftrace softirq/skb |
| **调度延迟** | vmstat (r/cs) | perf sched record | bpftrace sched |
| **全局慢** | sar -q, dmesg | perf stat -a | 多维度对比基线 |

### 工具开销对照（决定能否在生产环境使用）

| 工具 | 开销 | 生产环境 | 说明 |
|------|------|---------|------|
| vmstat / sar / iostat | < 0.1% | ★ 安全 | 读取 /proc 文件 |
| pidstat | < 0.1% | ★ 安全 | 读取 /proc 文件 |
| perf stat | < 0.5% | ★ 安全 | 硬件计数器，几乎零开销 |
| perf record (默认频率) | 1-3% | ⚠ 注意 | 采样频率 ~4KHz，有开销 |
| perf record (低频) | < 1% | ★ 较安全 | `-F 99` 降低频率 |
| perf lock | 2-5% | ⚠ 谨慎 | tracepoint 开销较大 |
| blktrace | 1-5% | ⚠ 谨慎 | relay buffer 写入开销 |
| strace | **10-100x** | ✘ 禁用 | ptrace 极大拖慢进程 |
| bpftrace | < 1% | ★ 安全 | eBPF JIT，开销极低 |
| perf trace | 1-3% | ⚠ 注意 | 比 strace 好但仍有开销 |
| tcpdump | 1-10% | ⚠ 取决于流量 | 高 pps 时开销大 |

### 快速诊断决策树（10 秒版）

```
系统慢了 → 运行：
  vmstat 1

  r > 核数？ ──→ CPU 饱和 → sar -u → perf record
  b > 0？  ──→ I/O 等待  → iostat -x → blktrace
  si/so > 0？──→ 内存不足 → sar -r → /proc/slabinfo
  cs > 50K？──→ 调度/锁   → perf lock → bpftrace
  都正常？ ──→ dmesg → perf stat -a → 对比基线
```

---

> **总结：** 性能诊断不是靠直觉猜，而是靠**系统化的排除法** + **多工具交叉验证**。  
> 先用低成本工具（vmstat/sar）确定方向，再用精确工具（perf/bpftrace）深入定位。  
> 永远不要跳过第一层的宏观扫描——它是你选择正确诊断路径的指南针。
