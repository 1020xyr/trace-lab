# perf 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: 如何用 perf 判断是 L3 cache miss 还是 CPU 计算瓶颈？](#q1-如何用-perf-判断是-l3-cache-miss-还是-cpu-计算瓶颈)
- [Q2: perf c2c 如何检测 false sharing？](#q2-perf-c2c-如何检测-false-sharing)
- [Q3: perf stat 显示 IPC < 1 但 cache-miss 率正常，可能是什么原因？](#q3-perf-stat-显示-ipc--1-但-cache-miss-率正常可能是什么原因)
- [Q4: perf record 报 "perf_event_paranoid" 错误怎么解决？](#q4-perf-record-报-perf_event_paranoid-错误怎么解决)

---

## Q1: 如何用 perf 判断是 L3 cache miss 还是 CPU 计算瓶颈？

**日期：** 2026-07-04
**场景：** AMD 服务器 CPU 高占用低吞吐，需要区分是 cache miss 导致还是计算效率低
**相关文件：** `reading/06_hw_counters_diagnosis.md`

### 回答

核心方法是用 `perf stat -d` 获取 **IPC** 和 **cache-miss 率**，通过两者组合判断：

```bash
perf stat -d -r 3 ./app
```

**诊断矩阵：**

| IPC | cache-miss 率 | 结论 |
|-----|-------------|------|
| > 1.5 | < 5% | ★ CPU 计算密集（正常） |
| > 1.5 | > 15% | 混合型：计算多且有 cache 问题 |
| < 1.0 | < 5% | 其他停顿（分支预测/TLB/前端） |
| < 1.0 | > 15% | ★ **L3 cache miss 是瓶颈** |
| < 0.5 | > 30% | ★★ **严重 L3 cache miss** |

**关键指标计算：**

```bash
# IPC = instructions / cycles
# cache-miss 率 = cache-misses / cache-references × 100%

# 进一步区分 L1 miss 和 L3 miss
perf stat -e L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses,instructions,cycles ./app

# L1 miss 率 = L1-dcache-load-misses / L1-dcache-loads
# L3 miss 率 = LLC-load-misses / LLC-loads

# ★ L3 miss 的代价远大于 L1 miss：
#   L1 miss → L2/L3 命中 → ~15 周期
#   L3 miss → 主存 → ~300 周期
#   → L3 miss 率 > 10% 就已经很严重
```

**前端/后端停顿辅助判断：**

```bash
perf stat -e stalled-cycles-frontend,stalled-cycles-backend,cycles ./app
```

| 停顿类型 | 含义 | 与 cache miss 的关系 |
|---------|------|---------------------|
| stalled-frontend 高 | 指令获取跟不上 | I-cache miss / 分支预测失败 |
| stalled-backend 高 | 数据访问跟不上 | ★ D-cache miss / L3 miss / 内存带宽 |

**★ 结论：**
- IPC 低 + cache-miss 率高 + stalled-backend 高 → **L3 cache miss 瓶颈**
- IPC 高 + cache-miss 率低 → **CPU 计算瓶颈**（优化算法）
- IPC 低 + cache-miss 率低 → 其他原因（检查分支预测、TLB、锁竞争）

---

## Q2: perf c2c 如何检测 false sharing？

**日期：** 2026-07-04
**场景：** 多线程程序增加线程数后性能反而下降，怀疑 false sharing
**相关文件：** `reading/07_c2c_false_sharing.md`

### 回答

`perf c2c` 通过 **HITM（Hit Modified）** 指标检测 false sharing。

**使用方法：**

```bash
# 采集
perf c2c record ./app

# 分析
perf c2c report --stdio
```

**原理：**

```
perf c2c record 使用 PEBS（Intel）或 IBS（AMD）的内存采样功能，
记录每次内存访问的：
  - 物理地址 → 计算 cache line 地址（64 字节对齐）
  - 访问类型（load/store）
  - 数据来源（L1/L2/L3/DRAM）
  - 是否是 HITM（该 cache line 是否被其他 CPU 修改过）

★ HITM（Hit Modified）= 本 CPU 加载一个 cache line 时，
  发现它被其他 CPU 修改过，需要从其他 CPU 的缓存中获取最新数据。
  这就是 Cache-to-Cache (c2c) 传输。
```

**判断 false sharing 的关键信号：**

```
perf c2c report 输出中：

  Shared Data Cache Line Table:
    某个 cache line 地址 → HITM 次数高 → ★ false sharing

  特征：
    1. 多个 CPU 同时 store 到同一个 cache line
    2. 大量 LLC Load HITM
    3. store 的变量逻辑上无关，但物理上在同一 cache line
```

**典型 false sharing 代码模式：**

```c
// false sharing：两个线程写相邻变量
struct { long counter_a; long counter_b; } shared;

// CPU 0: shared.counter_a++  → cache line @0x1000
// CPU 1: shared.counter_b++  → 同一个 cache line @0x1000！
// → 每次写都会 invalidate 对方的缓存 → 性能下降 100x

// 修复：cache line 对齐
struct __attribute__((aligned(64))) {
    long counter;
    char padding[56];
} per_cpu_counters[NR_CPUS];
```

**★ 注意：** `perf c2c` 需要物理机（PEBS/IBS 支持），在 KVM 虚拟机中可能不可用。

---

## Q3: perf stat 显示 IPC < 1 但 cache-miss 率正常，可能是什么原因？

**日期：** 2026-07-04
**场景：** `perf stat` 发现 IPC 只有 0.3~0.8，但 cache-miss 率只有 2~5%，不是 cache 的问题，那到底是什么拖慢了 CPU？
**相关文件：** `reading/06_hw_counters_diagnosis.md`

### 回答

IPC < 1 意味着 CPU 每个周期平均退休不到 1 条指令。cache miss 只是众多原因之一。perf 源码中的 **Topdown 方法**将所有停顿分为 4 大类，这是 Intel 提出的系统化分析框架。

#### 1. Topdown 四大分类

perf 通过 `--topdown` 选项支持 Topdown Level 1 分析。事件定义在源码中：

```c
// tools/perf/builtin-stat.c:131-137
static const char *topdown_metric_attrs[] = {
    "slots",
    "topdown-retiring",      // 有效退休（好事）
    "topdown-bad-spec",      // 错误推测（分支预测失败等）
    "topdown-fe-bound",      // 前端瓶颈（指令获取跟不上）
    "topdown-be-bound",      // 后端瓶颈（执行资源不足）
    NULL,
};
```

对应 Skylake 架构的指标公式（`tools/perf/pmu-events/arch/x86/skylake/skl-metrics.json`）：

| Topdown 分类 | 公式（简化版） | 含义 |
|-------------|-------------|------|
| **Retiring** | `UOPS_RETIRED.RETIRE_SLOTS / (4 * cycles)` | 有效工作占比 |
| **Bad_Speculation** | `(UOPS_ISSUED - UOPS_RETIRED + 4 * INT_MISC.RECOVERY_CYCLES) / (4 * cycles)` | 因错误推测浪费的 slot |
| **Frontend_Bound** | `IDQ_UOPS_NOT_DELIVERED.CORE / (4 * cycles)` | 前端无法喂饱后端 |
| **Backend_Bound** | `1 - (FE + BadSpec + Retiring)` | 后端资源不足 |

> ★ **关键理解：** Skylake 每周期最多发出 4 个 uop，所以总 slot = 4 × cycles。IPC < 1 说明大量 slot 被浪费。

#### 2. IPC < 1 但 cache-miss 正常的常见原因

```
┌─────────────────────────────────────────────────┐
│              CPU Pipeline Slot 去向              │
│  4 slots/cycle = Retiring + Bad_Spec            │
│                  + Frontend_Bound + Backend_Bound│
│                                                 │
│  IPC = Retiring × 4 (近似)                      │
│                                                 │
│  IPC < 1 的 5 大原因（cache-miss 只是其中一个）： │
│                                                 │
│  ① 分支预测失败 (Bad_Spec)                       │
│  ② TLB miss (Backend_Bound → Memory)            │
│  ③ 指令缓存 miss (Frontend_Bound)               │
│  ④ 执行端口争用 (Backend_Bound → Core)           │
│  ⑤ 数据依赖链 (Backend_Bound → Core)             │
└─────────────────────────────────────────────────┘
```

**详细分析：**

| 原因 | Topdown 分类 | cache-miss 表现 | 诊断事件 |
|------|-------------|----------------|---------|
| **分支预测失败** | Bad_Speculation | 正常 | `branch-misses`、`INT_MISC.RECOVERY_CYCLES` |
| **dTLB/iTLB miss** | Backend_Bound (Memory) | 正常 | `dTLB-load-misses`、`iTLB-load-misses` |
| **指令缓存 miss** | Frontend_Bound | 正常 | `L1-icache-load-misses`、`stalled-cycles-frontend` |
| **执行端口争用** | Backend_Bound (Core) | 正常 | `CYCLE_ACTIVITY.STALLS_TOTAL`、`UOPS_ISSUED.ANY` |
| **长依赖链** | Backend_Bound (Core) | 正常 | `CYCLE_ACTIVITY.STALLS_TOTAL` |
| **L3 cache miss** | Backend_Bound (Memory) | ★ 高 | `LLC-load-misses`、`CYCLE_ACTIVITY.STALLS_L3_MISS` |

#### 3. 源码验证：stalled-cycles 前后端分离

内核将停顿事件分为前端和后端两类，定义在硬件事件枚举中：

```c
// include/uapi/linux/perf_event.h:56-57
PERF_COUNT_HW_STALLED_CYCLES_FRONTEND = 7,  // 前端停顿周期
PERF_COUNT_HW_STALLED_CYCLES_BACKEND  = 8,  // 后端停顿周期
```

perf stat 输出时的颜色阈值设定（`tools/perf/util/stat-shadow.c:304-306`）：

```c
// 不同停顿类型的颜色告警阈值（百分比）
static const double grc_table[GRC_MAX_NR][3] = {
    [GRC_STALLED_CYCLES_FE] = { 50.0, 30.0, 10.0 },  // >50% 红色
    [GRC_STALLED_CYCLES_BE] = { 75.0, 50.0, 20.0 },  // >75% 红色
    [GRC_CACHE_MISSES]      = { 20.0, 10.0, 5.0  },   // >20% 红色
};
```

> ★ **注意：** 后端停顿阈值（75% 才红色）远高于前端（50%），说明后端停顿更"常见"——因为 cache miss、内存访问都算后端停顿。

#### 4. 分支预测失败的代价

分支预测失败属于 **Bad_Speculation**，perf 用 `INT_MISC.RECOVERY_CYCLES` 事件追踪恢复周期：

```json
// tools/perf/pmu-events/arch/x86/skylake/pipeline.json:73-76
{
    "EventName": "INT_MISC.RECOVERY_CYCLES",
    "BriefDescription": "Core cycles the allocator was stalled due to recovery
                         from earlier clear event for this thread
                         (e.g. misprediction or memory nuke)"
}
```

每次分支预测失败，CPU 需要 **15~20 个周期**清空流水线并重新从正确路径取指。如果程序有大量不可预测的分支（如哈希表查找、随机数据），即使 cache 完全命中，IPC 也会很低。

#### 5. 实战诊断命令

```bash
# Step 1: 基本 stat + stalled-cycles 分析
perf stat -e cycles,instructions,cache-misses,cache-references,\
    stalled-cycles-frontend,stalled-cycles-backend,\
    branch-misses,branch-instructions,\
    dTLB-load-misses,dTLB-loads,\
    iTLB-load-misses,iTLB-loads,\
    L1-icache-load-misses ./app

# Step 2: Topdown 分析（Intel CPU 需要 perf ≥ 4.10）
perf stat --topdown -a -- sleep 5

# Step 3: 如果 stalled-frontend 高，深入检查指令缓存
perf stat -e L1-icache-loads,L1-icache-load-misses,\
    cycles:u,cycles:k ./app

# Step 4: 如果 stalled-backend 高但 cache-miss 正常，检查 TLB 和依赖链
perf stat -e dTLB-load-misses,dTLB-loads,\
    iTLB-load-misses,iTLB-loads,\
    L1-dcache-loads,L1-dcache-load-misses,\
    LLC-loads,LLC-load-misses ./app
```

#### 6. 诊断决策树

```
perf stat 显示 IPC < 1
│
├── cache-miss 率 > 15%?
│   └── YES → L3 cache miss 是瓶颈（见 Q1）
│
├── stalled-cycles-frontend > 30%?
│   └── YES → 前端瓶颈
│       ├── L1-icache-load-misses 高? → 指令缓存 miss（代码太大/太分散）
│       └── iTLB-load-misses 高? → iTLB miss（代码页太多）
│
├── branch-miss 率 > 5%?
│   └── YES → 分支预测失败（Bad_Speculation）
│       └── 优化：减少不可预测分支，用 cmov、查表等
│
├── dTLB-load-miss 率高?
│   └── YES → TLB miss（大内存随机访问）
│       └── 优化：使用大页（hugepage）、减少工作集
│
├── stalled-cycles-backend > 50% 但 cache/TLB 正常?
│   └── YES → 执行端口争用或数据依赖链
│       └── 优化：减少依赖链深度，提高 ILP
│
└── 以上都正常?
    └── 检查 --topdown 输出，可能有微码 assist 或锁竞争
```

**★ 一句话总结：** IPC < 1 + cache-miss 正常 = 问题不在 cache，用 `perf stat --topdown` 定位是分支预测失败（Bad_Speculation）、前端饥饿（Frontend_Bound）、还是后端执行瓶颈（Backend_Bound/Core），再针对性优化。

---

## Q4: perf record 报 "perf_event_paranoid" 错误怎么解决？

**日期：** 2026-07-04
**场景：** 普通用户执行 `perf record` 时报 `Error: Permission denied. Hint: Check /proc/sys/kernel/perf_event_paranoid setting`，不清楚原因和解决方案
**相关文件：** `reading/05_perf_record_flow.md`

### 回答

`perf_event_paranoid` 是内核的安全旋钮，控制非特权用户能使用哪些 perf 功能。默认值是 **2**（限制最严格之一），不同级别限制不同功能。

#### 1. 源码验证：默认值和各级别含义

内核在 `kernel/events/core.c:405-411` 定义了 paranoid 注释和默认值：

```c
// kernel/events/core.c:405-411
/*
 * perf event paranoia level:
 *  -1 - not paranoid at all
 *   0 - disallow raw tracepoint access for unpriv
 *   1 - disallow cpu events for unpriv
 *   2 - disallow kernel profiling for unpriv
 */
int sysctl_perf_event_paranoid __read_mostly = 2;  // ★ 默认值是 2
```

内核通过三个 inline 函数实现分级权限检查（`include/linux/perf_event.h:1315-1337`）：

```c
// include/linux/perf_event.h:1315-1337
static inline int perf_allow_kernel(struct perf_event_attr *attr)
{
    if (sysctl_perf_event_paranoid > 1 && !perfmon_capable())
        return -EACCES;       // paranoid ≥ 2：禁止内核态采样
    return security_perf_event_open(attr, PERF_SECURITY_KERNEL);
}

static inline int perf_allow_cpu(struct perf_event_attr *attr)
{
    if (sysctl_perf_event_paranoid > 0 && !perfmon_capable())
        return -EACCES;       // paranoid ≥ 1：禁止 CPU 级事件
    return security_perf_event_open(attr, PERF_SECURITY_CPU);
}

static inline int perf_allow_tracepoint(struct perf_event_attr *attr)
{
    if (sysctl_perf_event_paranoid > -1 && !perfmon_capable())
        return -EPERM;        // paranoid ≥ 0：禁止 tracepoint
    return security_perf_event_open(attr, PERF_SECURITY_TRACEPOINT);
}
```

#### 2. 权限检查的核心：perfmon_capable()

`perfmon_capable()` 是权限判断的关键函数（`include/linux/capability.h:254-257`）：

```c
// include/linux/capability.h:254-257
static inline bool perfmon_capable(void)
{
    return capable(CAP_PERFMON) || capable(CAP_SYS_ADMIN);
    // ★ 拥有 CAP_PERFMON 或 CAP_SYS_ADMIN 其中之一即可绕过 paranoid 限制
}
```

> ★ **Linux 5.8+ 引入了 `CAP_PERFMON`（capability 38）**，这是专门为性能监控设计的权限，比 `CAP_SYS_ADMIN` 更精细。`root` 用户天然拥有 `CAP_SYS_ADMIN`，所以 `sudo perf record` 总是成功。

#### 3. perf_event_open 系统调用中的权限检查流程

`perf_event_open` 系统调用（`kernel/events/core.c:11618`）的权限检查顺序：

```
SYSCALL_DEFINE5(perf_event_open, ...)          // core.c:11618
│
├── 1. security_perf_event_open(attr, PERF_SECURITY_OPEN)   // core.c:11641
│   └── SELinux/AppArmor 等 LSM 检查（是否允许打开 perf 事件）
│
├── 2. perf_copy_attr() → 拷贝用户空间参数
│
├── 3. if (!attr.exclude_kernel)               // core.c:11649
│   └── perf_allow_kernel(&attr)               // ★ paranoid > 1 且无权限 → -EACCES
│       └── 这就是为什么默认值 2 禁止非 root 做内核态采样
│
├── 4. if (attr.namespaces)
│   └── perfmon_capable() 检查                  // core.c:11656
│
└── 后续：task 级 ptrace 权限检查               // core.c:11737
    └── 监控其他进程也需要权限
```

#### 4. 各级别权限对照表

| paranoid 值 | 用户态采样 | 内核态采样 | CPU 级事件 | tracepoint | 系统范围 | 典型用户 |
|-------------|-----------|-----------|-----------|------------|---------|---------|
| **-1** | ✅ | ✅ | ✅ | ✅ | ✅ | 开发/测试环境 |
| **0** | ✅ | ✅ | ✅ | ❌ | 部分 | 允许大部分功能 |
| **1** | ✅ | ✅ | ❌ | ❌ | ❌ | 仅监控自己的进程 |
| **2**（默认） | ✅ | ❌ | ❌ | ❌ | ❌ | ★ 多数发行版默认值 |
| **3**（Ubuntu/Debian） | ❌ | ❌ | ❌ | ❌ | ❌ | 最严格，完全禁用 |

> ★ **注意：** paranoid=3 不是上游内核标准，而是 Ubuntu/Debian 等发行版的补丁。上游内核注释只到 2。

#### 5. perf 用户态的 fallback 机制

perf 工具本身在遇到权限错误时有自动降级逻辑（`tools/perf/util/evsel.c:2522-2549`）：

```c
// tools/perf/util/evsel.c:2522-2549
} else if (err == EACCES && !evsel->core.attr.exclude_kernel &&
           (paranoid = perf_event_paranoid()) > 1) {
    // ★ 当 paranoid > 1 且遇到 EACCES 时，自动添加 exclude_kernel
    //   即从内核+用户态采样 降级为 仅用户态采样

    scnprintf(msg, msgsize, "kernel.perf_event_paranoid=%d, trying "
              "to fall back to excluding kernel and hypervisor "
              " samples", paranoid);
    evsel->core.attr.exclude_kernel = 1;
    evsel->core.attr.exclude_hv     = 1;
    return true;
}
```

perf 还会给出友好的错误提示（`tools/perf/util/evlist.c:1478-1497`）：

```c
// tools/perf/util/evlist.c:1478-1497
case EACCES:
case EPERM:
    "Error:\t%s.\n"
    "Hint:\tCheck /proc/sys/kernel/perf_event_paranoid setting."
    // 如果 value >= 2：提示 "For your workloads it needs to be <= 1"
    // 系统范围追踪："For system wide tracing it needs to be set to -1"
    // 修复命令：'sudo sh -c "echo -1 > /proc/sys/kernel/perf_event_paranoid"'
```

用户态也通过 `perf_event_paranoid_check()` 做预检查（`tools/perf/util/util.c:290-294`）：

```c
// tools/perf/util/util.c:290-294
bool perf_event_paranoid_check(int max_level)
{
    return perf_cap__capable(CAP_SYS_ADMIN) ||
           perf_cap__capable(CAP_PERFMON) ||
           perf_event_paranoid() <= max_level;
    // ★ 三重检查：root 权限 || CAP_PERFMON 权限 || paranoid 值足够低
}
```

#### 6. 解决方案

**方案 A：临时降低 paranoid 值（推荐用于开发/测试）**

```bash
# 查看当前值
cat /proc/sys/kernel/perf_event_paranoid

# 允许内核态采样（paranoid ≤ 1）
sudo sh -c 'echo 1 > /proc/sys/kernel/perf_event_paranoid'

# 允许系统范围采样（paranoid = -1）
sudo sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid'

# 仅监控自己的进程（不需要修改，paranoid=2 也允许）
perf record ./my_app
```

**方案 B：永久修改（写入 sysctl 配置）**

```bash
# 写入 sysctl 配置
echo 'kernel.perf_event_paranoid=1' | sudo tee /etc/sysctl.d/99-perf.conf
sudo sysctl --system
```

**方案 C：使用 CAP_PERFMON 权限（Linux 5.8+，最安全）**

```bash
# 给 perf 二进制文件添加 CAP_PERFMON capability
sudo setcap cap_perfmon,cap_sys_admin+ep $(which perf)

# 或者只给 CAP_SYS_ADMIN（旧内核）
sudo setcap cap_sys_admin+ep $(which perf)

# 验证
getcap $(which perf)
# 输出：/usr/bin/perf cap_sys_admin,cap_perfmon=ep
```

**方案 D：sudo 运行（最简单但不推荐生产）**

```bash
sudo perf record -g ./my_app
```

#### 7. 不同 paranoid 值下的 perf 命令可用性

```bash
# paranoid=2（默认）：
perf record ./app              # ✅ 用户态采样（自动 fallback）
perf record -a ./app           # ❌ 系统范围采样被禁止
perf record -e cycles:k ./app  # ❌ 内核态采样被禁止

# paranoid=1：
perf record ./app              # ✅
perf record -e cycles:k ./app  # ✅
perf record -a ./app           # ❌ 系统范围仍需 root

# paranoid=-1：
perf record -a -g -- sleep 10  # ✅ 全部功能开放
```

#### 8. 安全建议

| 场景 | 推荐 paranoid 值 | 说明 |
|------|-----------------|------|
| 生产服务器 | 2（默认） | 仅允许用户态自监控 |
| 开发测试机 | 1 | 允许内核态采样 |
| CI/性能测试 | -1 | 完全开放 |
| 容器环境 | 2 + `--cap-add=SYS_ADMIN` | 容器内需要额外 capability |
| 多用户服务器 | 2 + CAP_PERFMON | 精细授权特定用户 |

**★ 一句话总结：** `perf_event_paranoid` 默认值 2 禁止非 root 做内核态/系统范围采样，开发环境 `echo -1 > /proc/sys/kernel/perf_event_paranoid` 即可解决；生产环境推荐用 `CAP_PERFMON` 精细授权。

---

*新的问题将追加到此文件。每个问题记录日期、场景、相关文件。*
