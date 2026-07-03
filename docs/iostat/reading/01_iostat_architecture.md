# Step 1: iostat 工作原理 — 从 /proc/diskstats 到诊断指标

> ======================================================================
> 本文件讲解 iostat 的数据来源、计算逻辑和与 blktrace 的关系。
> 
> 阅读要点：
>   1. /proc/diskstats 的 20 个字段 — iostat 所有数据的唯一来源
>   2. 间隔采样原理 — iostat 如何从累计值算出"速率"
>   3. 核心指标的计算公式 — %util / await / avgqu-sz 等
>   4. 与 blktrace 的关系 — 宏观统计 vs 微观事件
>
> 预计阅读时间：15 分钟
> ======================================================================

---

## 目录

- [一、iostat 的数据来源](#一iostat-的数据来源)
- [二、/proc/diskstats 字段详解](#二procdiskstats-字段详解)
- [三、间隔采样原理](#三间隔采样原理)
- [四、核心指标计算公式](#四核心指标计算公式)
- [五、CPU 统计 — /proc/stat](#五cpu统计--procstat)
- [六、与 blktrace 的关系](#六与-blktrace-的关系)
- [七、诊断思维链](#七诊断思维链)

---

## 一、iostat 的数据来源

iostat **不采集任何新数据**，它只是读取两个内核文件并做减法运算：

```
┌──────────────────────────────────────────────────────────┐
│                       iostat                             │
│                                                          │
│   读取 ──→ /proc/stat        → CPU 时间（jiffies）      │
│   读取 ──→ /proc/diskstats   → 设备 I/O 计数器          │
│                                                          │
│   计算：(第二次读取 - 第一次读取) / 间隔 = 速率          │
├──────────────────────────────────────────────────────────┤
│                      内核层                              │
│                                                          │
│   每次 I/O 完成 ──→ 更新 struct disk_stats 计数器       │
│   每次时钟中断 ──→ 更新 cpu 时间计数器                    │
│                                                          │
│   /proc/stat 和 /proc/diskstats 只是把这些计数器暴露出来  │
└──────────────────────────────────────────────────────────┘
```

**★ 核心认知：** iostat 是一个**纯读取工具**，不开启任何内核追踪，不调用任何 tracepoint。这意味着：
- 零额外性能开销（只读两个 proc 文件）
- 看不到单个 I/O 事件（只有聚合统计）
- 不会遗漏任何 I/O（计数器在 block layer 内部更新，100% 准确）

---

## 二、/proc/diskstats 字段详解

### 实际输出

```bash
cat /proc/diskstats | grep vdb
```

```
 253      16 vdb 211572 27 13505762 2568153 5405125 32395 50894896 8233186 0 635086 10804328 0 0 0 0 50181 2988 0 0 0
```

### ★ 完整字段对照表

这是 iostat 所有数据的**唯一来源**。理解这 20 个字段就理解了 iostat 的一切。

```
字段 │ 名称                │ 单位     │ 含义                              │ iostat 使用
─────┼────────────────────┼─────────┼───────────────────────────────────┼────────────────
  1  │ major              │ -       │ 主设备号（253 = virtio-blk）      │ 识别设备
  2  │ minor              │ -       │ 次设备号                          │ 识别设备
  3  │ dev_name           │ -       │ 设备名（vdb）                     │ 显示名称
  4  │ reads_completed    │ 次      │ ★ 读操作完成数                    │ r/s 的分子
  5  │ reads_merged       │ 次      │ 读请求合并数                      │ rrqm/s 的分子
  6  │ sectors_read       │ 扇区    │ ★ 读扇区数（1扇区=512字节）       │ rkB/s 的分子
  7  │ read_time          │ ms      │ 读操作总耗时                      │ r_await 的分子
  8  │ writes_completed   │ 次      │ ★ 写操作完成数                    │ w/s 的分子
  9  │ writes_merged      │ 次      │ 写请求合并数                      │ wrqm/s 的分子
 10  │ sectors_written    │ 扇区    │ ★ 写扇区数                        │ wkB/s 的分子
 11  │ write_time         │ ms      │ 写操作总耗时                      │ w_await 的分子
 12  │ ios_in_progress    │ 个      │ ★ 当前正在处理的 I/O 数           │ —（瞬时值）
 13  │ io_time            │ ms      │ ★ I/O 操作总耗时（有 I/O 在忙）   │ %util 的分子
 14  │ weighted_io_time   │ ms      │ ★ 加权 I/O 等待时间               │ avgqu-sz 的分子
 15  │ discards_completed │ 次      │ discard 完成数                    │ d/s 的分子
 16  │ discards_merged    │ 次      │ discard 合并数                    │ drqm/s 的分子
 17  │ sectors_discarded  │ 扇区    │ discard 扇区数                    │ dkB/s 的分子
 18  │ discard_time       │ ms      │ discard 总耗时                    │ d_await 的分子
 19  │ flush_completed    │ 次      │ flush 完成数                      │ f/s 的分子
 20  │ flush_time         │ ms      │ flush 总耗时                      │ f_await 的分子
```

### ★ 重点字段深入解析

**字段 4 — reads_completed（读完成数）**
```
这是"完成的读请求次数"，不是"发起的读请求次数"。
如果 3 个读请求被合并（merge）成 1 个，reads_completed 只加 1。
```

**字段 6 — sectors_read（读扇区数）**
```
1 扇区 = 512 字节（固定！不管设备实际扇区大小）
rkB/s = Δsectors_read × 512 / 1024 / Δt = Δsectors_read / 2 / Δt
```

**字段 12 — ios_in_progress（进行中的 I/O）**
```
★ 唯一的"瞬时值"（不是累计值）
= 当前此刻设备正在处理的 I/O 请求数
其他字段都是从开机至今的累计值

当 iostat 采样时，这个值反映的是采样瞬间的快照。
```

**字段 13 — io_time（I/O 忙时间）**
```
★ %util 的分子！

每当设备有至少 1 个 I/O 在处理时，内核就以 jiffies（通常 1ms）为粒度
递增这个计数器。

例如：1 秒内设备有 800ms 在处理 I/O → io_time 增加 800
      %util = 800 / 1000 × 100% = 80%
```

**字段 14 — weighted_io_time（加权等待时间）**
```
★ avgqu-sz 的分子！

每次 I/O 完成时：weighted_io_time += (当前 in_flight 数) × (该 I/O 的耗时)

例：1 秒内完成了 100 个 I/O，每个 I/O 在队列中等待时，
    in_flight 分别为 1, 2, 3, ...
    weighted_io_time = Σ(in_flight_i × duration_i)

avgqu-sz = Δweighted_io_time / Δio_time
         = 加权等待时间 / 总时间 = 平均队列深度
```

### 数据在内核中的更新位置

```
┌──────────────────────────────────────────────────────────┐
│                   Block Layer I/O 路径                    │
│                                                          │
│  bio 提交                                                │
│    │                                                     │
│    ▼                                                     │
│  __submit_bio() ──→ disk_stats.reads/writes++           │
│    │                 disk_stats.sectors += nr_sectors     │
│    │                 disk_stats.in_flight++               │
│    ▼                                                     │
│  I/O 调度器 ──→ 可能合并：merges++                       │
│    │                                                     │
│    ▼                                                     │
│  驱动处理 ──→ io_ticks++ （每个 jiffy，只要有 I/O 在忙）  │
│    │           weighted_io_time += in_flight              │
│    ▼                                                     │
│  I/O 完成 ──→ disk_stats.in_flight--                     │
│               io_time += elapsed                          │
│               read_time/write_time += elapsed             │
│               reads_completed++ / writes_completed++      │
└──────────────────────────────────────────────────────────┘

内核源码位置：
  - linux-5.10/block/blk-core.c: blk_account_io_start()
  - linux-5.10/block/blk-core.c: blk_account_io_done()
  - linux-5.10/include/linux/genhd.h: struct disk_stats
```

---

## 三、间隔采样原理

### ★ 为什么第一次输出和后续输出不同？

```
┌─────────────────────────────────────────────────────────────────┐
│                    iostat 1 3                                    │
│                    │    │    │    │                              │
│                    t0   t1   t2   t3                            │
│                    ▼    ▼    ▼    ▼                              │
│                  read0 read1 read2 read3                         │
│                                                                  │
│  首次输出 = (read0 - boot_value) / (t0 - boot_time)             │
│             ↑                                                     │
│             开机以来的平均值（可能跨越数天！）                      │
│                                                                  │
│  第 2 次 = (read1 - read0) / 1s    ← 真正的 1 秒内速率           │
│  第 3 次 = (read2 - read1) / 1s    ← 真正的 1 秒内速率           │
│                                                                  │
│  ★ -y 参数跳过首次输出，直接从第 2 次开始                         │
│     iostat -y 1 3 → 只有 3 次间隔采样输出                        │
└─────────────────────────────────────────────────────────────────┘
```

### 计算示例

```
t=0s 读取 /proc/diskstats:
  vdb: reads=211572  writes=5405125  sectors_read=13505762  io_time=635086

t=1s 读取 /proc/diskstats:
  vdb: reads=211627  writes=5421125  sectors_read=13506814  io_time=635986

计算：
  r/s       = (211627 - 211572) / 1 = 55
  w/s       = (5421125 - 5405125) / 1 = 16000
  rkB/s     = (13506814 - 13505762) / 2 / 1 = 526 KB/s
  %util     = (635986 - 635086) / 1000 × 100% = 90%
```

---

## 四、核心指标计算公式

### ★ 完整公式表

```
指标           │ 公式                                          │ 含义
──────────────┼───────────────────────────────────────────────┼──────────────────────
r/s            │ Δreads_completed / Δt                        │ 每秒读 I/O 数
w/s            │ Δwrites_completed / Δt                       │ 每秒写 I/O 数
rkB/s          │ Δsectors_read × 512 / 1024 / Δt             │ 每秒读吞吐量(KB)
wkB/s          │ Δsectors_written × 512 / 1024 / Δt          │ 每秒写吞吐量(KB)
rrqm/s         │ Δreads_merged / Δt                           │ 每秒读合并数
wrqm/s         │ Δwrites_merged / Δt                          │ 每秒写合并数
%rrqm          │ rrqm/s / (r/s + rrqm/s) × 100%              │ 读合并百分比
%wrqm          │ wrqm/s / (w/s + wrqm/s) × 100%              │ 写合并百分比
r_await        │ Δread_time / Δreads_completed                │ 读平均延迟(ms)
w_await        │ Δwrite_time / Δwrites_completed              │ 写平均延迟(ms)
await          │ (Δread_time+Δwrite_time)/(Δreads+Δwrites)    │ ★ 总平均延迟(ms)
rareq-sz       │ Δsectors_read × 512 / Δreads / 1024          │ 读平均请求大小(KB)
wareq-sz       │ Δsectors_written × 512 / Δwrites / 1024      │ 写平均请求大小(KB)
aqu-sz         │ Δweighted_io_time / 1000 / Δt                │ ★ 平均队列深度
%util          │ Δio_time / (Δt × 1000) × 100%               │ ★ 设备利用率
tps            │ (Δreads + Δwrites + Δdiscards) / Δt          │ 每秒传输数
```

### ★ 三大核心指标深入解析

#### %util — 设备忙碌度

```
%util = Δio_time / (Δt × 1000) × 100%

      设备有I/O在忙的时间
    = ───────────────────── × 100%
          总采样时间

例：1 秒采样期内，设备有 960ms 在处理 I/O
    %util = 960 / 1000 × 100% = 96%

★ 注意：
  - %util = 100% 不代表"设备已到极限"（多队列 SSD 可能还有余量）
  - %util = 100% 只代表"整个采样期间设备一直有 I/O 在处理"
  - 对于单队列设备（HDD、virtio-blk），100% ≈ 设备饱和
  - 对于多队列 NVMe SSD，100% 只说明一个队列满了
```

#### await — 端到端延迟

```
await = (Δread_time + Δwrite_time) / (Δreads + Δwrites)

这是所有 I/O 从提交到完成的平均等待时间（毫秒）。
★ 等价于 blktrace/btt 中的 Q2C（端到端延迟）

await = 排队等待 + 设备处理
      ≈ Q2D      + D2C

但 iostat 无法区分排队延迟和设备处理延迟！
→ 需要 blktrace 来看 Q2D 和 D2C 分别是多少
```

#### avgqu-sz (aqu-sz) — 平均队列深度

```
avgqu-sz = Δweighted_io_time / (1000 × Δt)

weighted_io_time 的累加方式：
  每次 I/O 完成时，weighted_io_time += in_flight × io_duration

简化理解：
  avgqu-sz ≈ 平均同时有几个 I/O 在处理

例：1 秒内，平均同时有 25 个 I/O 在队列中
    avgqu-sz = 25

★ 注意：
  - 这个值可能大于设备队列深度（因为包含等待调度的 I/O）
  - 值越高说明排队越严重
  - 对于 libaio + iodepth=32，正常值在 25~32 之间
```

### await 与 blktrace 延迟的对应

```
iostat await ≈ blktrace Q2C
            = Q2D + D2C

           ┌──────────┐  ┌──────────┐
           │   Q2D    │  │   D2C    │
I/O:   Q ──→ 排队等待  ──→ D 下发 ──→ 设备处理 ──→ C 完成
           │          │  │          │
           │ 软件层    │  │ 硬件层    │
           └──────────┘  └──────────┘

iostat 只能看到整体 await（Q 到 C 的总时间）
blktrace 可以分开看 Q2D（软件延迟）和 D2C（硬件延迟）
```

---

## 五、CPU 统计 — /proc/stat

### /proc/stat 中的 CPU 行

```bash
head -1 /proc/stat
```

```
cpu  2239477 21586 980103 69404232 60863 0 2638 0 0 0
     ─────── ───── ────── ──────── ───── ─ ────
       │      │     │       │       │   │   │
       │      │     │       │       │   │   └─ steal: 虚拟化偷时间
       │      │     │       │       │   └───── irq: 硬中断处理
       │      │     │       │       └───────── softirq: 软中断处理
       │      │     │       └───────────────── iowait: 等待 I/O 完成
       │      │     └───────────────────────── system: 内核态时间
       │      └─────────────────────────────── nice: 低优先级用户态
       └────────────────────────────────────── user: 用户态时间

所有值的单位：jiffies（通常 = 10ms，由 USER_HZ 决定）
```

### iostat CPU 字段的计算

```
total = user + nice + system + iowait + steal + irq + softirq + idle

%user   = user / total × 100%
%nice   = nice / total × 100%
%system = (system + irq + softirq) / total × 100%
%iowait = iowait / total × 100%
%steal  = steal / total × 100%
%idle   = idle / total × 100%
```

### ★ %iowait 的正确理解

```
%iowait 高 ≠ I/O 是瓶颈！

%iowait 的真实含义：
  "CPU 本来可以干活，但恰好没有进程可运行，且至少有一个 I/O 在等待"

两种情况都会导致 %iowait 高：
  1. I/O 密集：大量 I/O 阻塞了所有 CPU 进程 → 确实是 I/O 瓶颈
  2. CPU 空闲：只有一个进程在做 I/O，其他 CPU 都闲着 → 不是瓶颈

★ 正确的诊断方式：
  %iowait 高 + %util 高 → I/O 是瓶颈
  %iowait 高 + %util 低 → 只是 CPU 空闲，不一定有问题
```

---

## 六、与 blktrace 的关系

### 对比总览

```
维度          │ iostat                  │ blktrace
─────────────┼────────────────────────┼────────────────────────
视角          │ ★ 宏观统计             │ ★ 微观事件
粒度          │ 设备级聚合             │ 单个 I/O 级
数据源        │ /proc/diskstats        │ kernel tracepoint
采集方式      │ 读取累计计数器         │ relay buffer 实时记录
开销          │ 几乎为零               │ 有一定开销（高 IOPS 时 2~5%）
信息量        │ 速率、利用率、平均延迟 │ 每个 I/O 的完整生命周期
适合场景      │ "有没有问题？"         │ "问题出在哪？"
输出频率      │ 按采样间隔             │ 每个 I/O 事件都记录
```

### ★ 指标对应关系

```
iostat 指标    │ blktrace/btt 对应      │ 差异说明
──────────────┼───────────────────────┼──────────────────────────
r/s            │ IOPS (read)           │ 相同概念
w/s            │ IOPS (write)          │ 相同概念
await          │ Q2C avg               │ ★ 几乎相同（端到端延迟）
r_await        │ 读请求 Q2C            │ 只统计读操作
w_await        │ 写请求 Q2C            │ 只统计写操作
avgqu-sz       │ btt Average Queue Depth│ 计算方式略有不同
%util          │ 无直接对应            │ blktrace 不算利用率
—              │ Q2D avg               │ ★ iostat 不单独追踪排队延迟
—              │ D2C avg               │ ★ iostat 不单独追踪设备延迟
```

### 诊断工作流：先用 iostat，再用 blktrace

```
步骤 1：iostat -x 1 → 快速判断
┌──────────────────────────────────────────────────────────┐
│ 观察 %util、await、avgqu-sz                              │
│                                                          │
│ %util ≈ 100%？  ──→ 设备忙                                │
│ await > 20ms？  ──→ 延迟高                                │
│ avgqu-sz > 32？ ──→ 队列堆积                              │
│                                                          │
│ 如果一切正常 → 不需要 blktrace                            │
│ 如果发现异常 → 进入步骤 2                                 │
└──────────────────────────────────────────────────────────┘
                         ↓
步骤 2：blktrace + blkparse + btt → 微观定位
┌──────────────────────────────────────────────────────────┐
│ blktrace -d /dev/vdb -o trace -w 30                     │
│ blkparse -i trace -d trace.bin                          │
│ btt -i trace.bin -o summary                             │
│                                                          │
│ 看 Q2D 和 D2C：                                          │
│   Q2D 高 → 软件层瓶颈（I/O 调度器、队列深度）             │
│   D2C 高 → 硬件层瓶颈（设备性能）                         │
│                                                          │
│ 看 Seek 信息：                                            │
│   MEAN seek = 0 → 顺序 I/O                               │
│   MEAN seek 很大 → 随机 I/O                               │
└──────────────────────────────────────────────────────────┘
```

### 类比

```
iostat  ≈ 交通摄像头
          每分钟统计：经过多少车、平均速度、道路占用率
          看不到：每辆车从哪来、到哪去、在哪等了多久

blktrace ≈ GPS 追踪器
          每辆车：出发时间、等红灯时间、行驶时间、到达时间
          能精确找到：哪条路堵了、哪个红绿灯等太久
```

---

## 七、诊断思维链

### 从指标到结论的完整决策树

```
iostat -x 1 观察
    │
    ├── %util ≈ 100%
    │   │
    │   ├── await 高（> 20ms）
    │   │   → ★ 设备饱和 + 延迟大 → 应用明显受影响
    │   │   │
    │   │   ├── r_await >> w_await → 读延迟是主要问题
    │   │   │   → 用 blktrace 看 Q2D 和 D2C
    │   │   │
    │   │   └── w_await >> r_await → 写延迟是主要问题
    │   │       → 检查 w/s 是否过高，或 wareq-sz 是否太小
    │   │
    │   └── await 低（< 5ms）
    │       → 设备忙但响应快，可能不是瓶颈
    │       → 多队列 SSD 可能还有余量
    │
    ├── await 高但 %util 低
    │   → 少量慢 I/O（如大文件顺序读写）
    │   → 设备没被充分利用，但单个 I/O 耗时长
    │   → rareq-sz / wareq-sz 可能很大（128KB+）
    │
    ├── avgqu-sz 很大（> 32）
    │   → ★ 队列堆积，I/O 调度器压力大
    │   → 用 blktrace 看 Q2D 延迟
    │   → Q2D 高 = 软件层瓶颈
    │
    ├── rrqm/s 或 wrqm/s 很高
    │   → I/O 合并活跃（通常是顺序 I/O）
    │   → %rrqm/%wrqm 高 = 合并效率高，不一定是坏事
    │
    └── %iowait 高
        │
        ├── %util 也高 → CPU 被 I/O 阻塞，确实是 I/O 瓶颈
        └── %util 低 → CPU 空闲，只是碰巧有 I/O 在等待
```

### 快速参考：常见场景的 iostat 特征

```
场景                  │ %util │ await  │ r/s    │ w/s    │ avgqu-sz │ 典型原因
─────────────────────┼───────┼────────┼────────┼────────┼──────────┼──────────────
4K 随机写（iodepth=32）│ ~96%  │ 1~2ms  │ ~0     │ ~20000 │ ~25      │ 高 IOPS 写
128K 顺序读（iodepth=16）│~100% │ 8~11ms │ ~2800  │ ~0     │ ~31      │ 大吞吐读
128K 顺序写（iodepth=16）│~100% │ 9~10ms │ ~0     │ ~2900  │ ~28      │ 大吞吐写
空闲                  │ <1%   │ —      │ ~0     │ ~0     │ ~0       │ 无 I/O
数据库 OLTP           │ 80%+  │ 5~20ms │ 高     │ 高     │ 16~64    │ 混合读写
写回风暴              │ 100%  │ >50ms  │ 低     │ 极高   │ >100     │ 脏页刷盘
```

---

## 总结

| 要点 | 内容 |
|------|------|
| **数据源** | /proc/diskstats（20 个字段）+ /proc/stat（CPU 时间） |
| **计算方式** | 两次读取的差值 / 间隔 = 速率 |
| **首次输出** | 开机以来的平均值（用 `-y` 跳过） |
| **%util** | 设备有 I/O 在忙的时间比（不等于容量使用率） |
| **await** | 端到端延迟 ≈ blktrace Q2C |
| **avgqu-sz** | 平均队列深度（排队严重程度） |
| **与 blktrace** | iostat 看宏观 → blktrace 看微观 |
| **诊断流程** | iostat 发现异常 → blktrace 定位根因 |
