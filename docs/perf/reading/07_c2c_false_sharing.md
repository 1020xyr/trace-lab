# perf c2c — Cache-to-Cache 共享与 False Sharing 检测

> ======================================================================
> 阅读要点：
>   1. Cache line 共享与 false sharing 的本质
>   2. perf c2c record 的采集原理
>   3. perf c2c report 输出解读
>   4. false sharing 的检测与修复
>
> 相关源码：
>   - src/linux-5.10/tools/perf/builtin-c2c.c — perf c2c 实现
>   - src/linux-5.10/tools/perf/builtin-mem.c — perf mem 实现（c2c 依赖）
>
> 预计阅读时间：20 分钟
> ======================================================================

---

## 一、Cache Line 共享与 False Sharing

### 1.1 Cache Line 基础

```
Cache Line（缓存行）是 CPU 缓存的最小传输单元：
  Intel: 64 字节
  AMD:   64 字节

当 CPU 访问内存中的某个字节时：
  → 整个 64 字节的 cache line 被加载到 L1/L2/L3

┌─── Cache Line (64 bytes) ──────────────────────────┐
│ byte[0] byte[1] ... byte[63]                       │
│ ┌──────┐┌──────┐┌──────┐┌──────┐                   │
│ │ var_A││ var_B││ var_C││ var_D│  ...               │
│ │CPU 0 ││CPU 1 ││CPU 0 ││CPU 1 │                    │
│ │写    ││写    ││读    ││写    │                    │
│ └──────┘└──────┘└──────┘└──────┘                   │
└────────────────────────────────────────────────────┘

★ 如果 var_A（CPU 0 写）和 var_B（CPU 1 写）在同一个 cache line 中，
  即使它们逻辑上无关，也会导致缓存行在两个 CPU 之间反复迁移！
```

### 1.2 False Sharing 的产生

```
┌─ CPU 0 ──────────────┐    ┌─ CPU 1 ──────────────┐
│ counter_0++           │    │ counter_1++           │
│ (地址: 0x1000)        │    │ (地址: 0x1008)        │
└───────────┬───────────┘    └───────────┬───────────┘
            │                            │
            └────────┐  ┌────────────────┘
                     │  │
                     ▼  ▼
              同一个 Cache Line (64 bytes)
              [counter_0 | counter_1 | ...]

MESI 协议行为：
  1. CPU 0 写 counter_0 →  invalidated CPU 1 的 cache line
  2. CPU 1 写 counter_1 →  invalidated CPU 0 的 cache line
  3. CPU 0 再写 → 又从 CPU 1 拿回 cache line（Cache-to-Cache transfer）
  4. 反复 ping-pong → ★ 性能严重下降！

★ False Sharing = 两个 CPU 写同一个 cache line 的不同变量
  → 逻辑上无共享，但物理上共享 cache line
  → 导致大量 Cache-to-Cache (c2c) 传输
```

### 1.3 False Sharing 的性能影响

```
无 false sharing:
  CPU 0 写 counter_0 → L1 hit → ~1 周期
  CPU 1 写 counter_1 → L1 hit → ~1 周期

有 false sharing:
  CPU 0 写 counter_0 → invalidate CPU 1 → ~100 周期
  CPU 1 写 counter_1 → invalidate CPU 0 → ~100 周期
  → 每次写操作慢 100 倍！

★ 典型症状：
  - 多线程程序，增加线程数反而性能下降
  - CPU 利用率高但吞吐低
  - perf 看到大量 cache-misses 但代码看起来无共享
```

---

## 二、perf c2c 采集原理

### 2.1 工作原理

```
perf c2c record 的本质：
  → 使用 PEBS（Intel）或 IBS（AMD）的内存采样功能
  → 记录每次内存访问的：
    - 虚拟地址
    - 物理地址（用于计算 cache line 地址）
    - 延迟（从发起 load/store 到完成）
    - 数据来源（L1/L2/L3/Remote/DRAM）
    - 是否是 HITM（Hit Modified，即 cache line 被其他 CPU 修改过）

源码位置：src/linux-5.10/tools/perf/builtin-c2c.c
```

### 2.2 基本用法

```bash
# 采集（使用 mem-loads 和 mem-stores 事件）
perf c2c record -a sleep 10           # 全系统 10 秒
perf c2c record ./app                  # 特定程序

# 底层等价于：
perf record -e mem-loads,mem-stores -d ./app
# -d 表示记录数据地址（data address sampling）

# 分析
perf c2c report                        # 交互式 TUI
perf c2c report --stdio                # 文本输出
```

---

## 三、perf c2c report 输出解读

### 3.1 共享数据缓存行表（Shared Data Cache Line Table）

```
perf c2c report 最重要的输出：

=====================================================
         Shared Data Cache Line Table
=====================================================
#         ------- Cacheline ----------    Total     Tot  ----- LLC Load Hitm -----
# Index      Address  Node  PA cnt  records   Pct       Hitm   Local   Remote
# .....  ...............  ....  ......  .......  ......  .......  ......  .......
    0      0x7f3a1c000     0      1     8542    32.1%     4521   3200     1321
    1      0x7f3a1c040     0      1     3210    12.1%      890    700      190
    2      0x7f3a1c080     0      2     1200     4.5%       50     45        5

逐列解读：
  Index         → 排名（按 HITM 次数排序）
  Address       → ★ cache line 的物理地址
  Node          → NUMA 节点号
  PA cnt        → 映射到该 cache line 的不同物理地址数量
  records       → 采样到的内存访问记录数
  Tot Pct       → 占总采样的百分比
  LLC Hitm      → ★★ LLC Hit Modified 总次数
  Local         → 本地 HITM（同 socket 的其他核心）
  Remote        → ★ 远程 HITM（跨 socket，更慢）

★ HITM（Hit Modified）= false sharing 的直接证据！
  → 表示本 CPU 加载的 cache line 被其他 CPU 修改过
  → 需要从其他 CPU 的缓存中获取最新数据
  → HITM 次数越多 → false sharing 越严重
```

### 3.2 各 Cacheline 的详细访问模式

```
# 展开某个 cache line 的详细视图：

Cacheline 0x7f3a1c000:
  ------- Store Reference ------   ----- Load Reference -----
  Store       L1 Hit   L1 Miss    L1 Hit   LFB Hit   L1 Miss
  25420       20100      5320      18900    2100      1200

  ------- LLC Load Hitm --------
  Local    Remote    Load Dram
  3200     1321      800

  ---- Stores ----    ---- Loads ----
  CPU 0: 12000       CPU 0: 8900
  CPU 1: 13420       CPU 1: 10000
  CPU 2:    0        CPU 2:    0

  ★ 关键信号：
    - 多个 CPU 同时 store 到同一个 cache line → false sharing
    - 大量 LLC Load HITM → cache line 在 CPU 间频繁迁移
    - Local HITM >> Remote HITM → 同 socket 内的 false sharing
    - Remote HITM 显著 → 跨 socket false sharing（更严重）
```

### 3.3 HITM 分析函数视图

```
perf c2c report 还可以按函数查看 HITM 来源：

# 哪些函数产生了 HITM？
  ------- HITM ------
  Local   Remote   Symbol
  2100    800      update_counter     ← ★ 这个函数有 false sharing
   400    200      process_packet
   100     50      handle_request

★ update_counter 函数有大量 HITM → 检查其访问的数据结构
```

---

## 四、False Sharing 检测实战

### 4.1 经典 false sharing 示例

```c
// false_sharing.c — 经典的 false sharing 场景
#include <pthread.h>
#include <stdlib.h>

struct counter {
    long count;          // 8 字节
    // ★ 没有 padding → 多个 counter 在同一个 cache line 中
};

struct counter counters[8];  // 8 个计数器，前 8 个在同一 cache line

void *worker(void *arg) {
    int id = (long)arg;
    for (int i = 0; i < 100000000; i++) {
        counters[id].count++;   // ★ 每个线程写不同的 counter
                                 // 但它们在同一个 cache line 中！
    }
    return NULL;
}
```

### 4.2 检测 false sharing

```bash
# 编译
gcc -O2 -pthread -o false_sharing false_sharing.c

# 用 perf c2c 检测
perf c2c record ./false_sharing
perf c2c report --stdio

# 预期看到：
# - 同一个 cache line 地址有大量 HITM
# - 多个 CPU 同时 store
# - update_counter 或 worker 函数有大量 Local HITM
```

### 4.3 修复 false sharing

```c
// 修复方法 1：cache line 对齐
struct __attribute__((aligned(64))) counter {
    long count;
    char padding[56];     // ★ 填充到 64 字节
};

// 修复方法 2：使用 per-CPU 变量
__thread long my_count;   // 每个线程独立变量

// 修复方法 3：使用原子操作 + 独立变量
_Atomic long counters[8]; // 至少避免编译器优化到一起
```

---

## 五、诊断决策树

```
多线程程序性能不佳
    │
    ├── perf stat -d → cache-miss 率高
    │   │
    │   └── perf c2c record → perf c2c report
    │       │
    │       ├── HITM 次数高？
    │       │   ├── 是 → false sharing
    │       │   │   └── 对齐数据结构到 cache line
    │       │   │
    │       │   └── 否 → 真共享（true sharing）或工作集太大
    │       │       ├── 多线程读同一变量 → 真共享（用 per-CPU 变量）
    │       │       └── 工作集 > LLC → 优化数据局部性
    │       │
    │       └── 跨 socket HITM (Remote) 多？
    │           └── 是 → NUMA 不友好
    │               └── numactl --interleave 或绑 NUMA 节点
    │
    └── cache-miss 率不高 → 其他瓶颈
        ├── 锁竞争 → perf lock
        ├── 分支预测 → branch-miss
        └── 计算效率 → 优化算法
```

---

## 总结

### perf c2c 核心概念

```
概念              │ 说明                          │ 正常值
─────────────────┼──────────────────────────────┼──────────
HITM             │ Hit Modified（cache line 被    │ 越少越好
                 │ 其他 CPU 修改后又被加载）       │
Local HITM       │ 同 socket 内的 HITM            │ 比 Remote 好
Remote HITM      │ 跨 socket 的 HITM              │ ★ 更慢，应避免
c2c transfer     │ Cache-to-Cache 传输            │ 越少越好
False sharing    │ 不同变量在同一 cache line       │ ★ 必须修复
```

### 源码位置

```
perf c2c 实现:   src/linux-5.10/tools/perf/builtin-c2c.c
perf mem 实现:   src/linux-5.10/tools/perf/builtin-mem.c
PEBS 支持:       src/linux-5.10/arch/x86/events/intel/ds.c
AMD IBS 支持:    src/linux-5.10/arch/x86/events/amd/ibs.c
```
