# perf mem 与 AMD 特定事件

> ======================================================================
> 阅读要点：
>   1. perf mem 内存访问延迟分析
>   2. AMD 与 Intel 的 perf 事件差异
>   3. AMD IBS（Instruction Based Sampling）的使用
>   4. AMD 特有的性能分析问题
>
> 相关源码：
>   - src/linux-5.10/tools/perf/builtin-mem.c       — perf mem 实现
>   - src/linux-5.10/arch/x86/events/amd/core.c     — AMD PMU 驱动
>   - src/linux-5.10/arch/x86/events/amd/ibs.c      — AMD IBS 实现
>   - src/linux-5.10/arch/x86/events/amd/uncore.c   — AMD uncore（L3/DF）PMU
>   - src/linux-5.10/arch/x86/include/asm/perf_event.h — IBS 常量定义
>
> 预计阅读时间：25 分钟
> ======================================================================

---

## 一、perf mem — 内存访问延迟分析

### 1.1 基本原理

```
perf mem 使用 ★ PEBS（Intel）或 IBS（AMD）的内存采样功能：
  → 记录每次 load/store 的：
    - 数据地址（虚拟 + 物理）
    - ★ 延迟（从发起访问到完成的周期数）
    - ★ 数据来源（L1/L2/L3/Local DRAM/Remote DRAM）
    - 访问类型（load/store）

源码：src/linux-5.10/tools/perf/builtin-mem.c
```

### 1.2 基本用法

```bash
# 采集（load + store）
perf mem record -t load ./app          # 只追踪 load
perf mem record -t store ./app         # 只追踪 store
perf mem record -t ldst ./app          # load + store（默认）
perf mem record -a sleep 10            # 全系统 10 秒

# 分析
perf mem report                        # 交互式 TUI
perf mem report --stdio                # 文本输出
```

### 1.3 输出字段解读

```
perf mem report 输出列：

字段              │ 含义                          │ 诊断要点
─────────────────┼──────────────────────────────┼──────────────────
Symbol           │ 函数名                        │ ★ 热点函数
DSO              │ 所属共享库                    │ —
TLB              │ TLB 命中情况                  │ ★ miss → 大页优化
Cache            │ 缓存命中层级                  │ ★ L1/L2/L3/DRAM
Mem              │ 内存类型                      │ —
Snoop            │ 是否需要 cache 一致性交互     │ ★ HITM → false sharing
Cycles           │ 访问延迟（CPU 周期）          │ ★ 越高越慢

★ 数据来源层级：
  L1 hit:     ~4 周期
  L2 hit:     ~12 周期
  L3 hit:     ~40 周期
  Local DRAM: ~200 周期
  Remote DRAM: ~400 周期（NUMA 远程节点）
  Remote Cache: ~100 周期（远程 socket 的 L3）
```

### 1.4 延迟分布分析

```bash
# 查看内存访问延迟的直方图分布
perf mem report --sort=mem,snoop --stdio
```

```
典型输出：

  Overhead  Memory access     Snoop
  ........  ................  ...........
    65.3%   L1 hit            HitM
    15.2%   L2 hit            HitM
     8.5%   L3 hit            HitM
     6.2%   Local DRAM        HitM
     3.1%   Remote DRAM       HitM
     1.7%   LFB hit           HitM

诊断：
  ★ L1/L2 占比高 → 内存访问模式良好
  ★ DRAM 占比高 → 工作集大或访问不连续
  ★ Remote DRAM → NUMA 不友好
  ★ HitM 频繁 → false sharing（配合 perf c2c 确认）
```

---

## 二、AMD 与 Intel 的 perf 事件差异

### 2.1 通用事件对比

```
perf 事件名           │ Intel 映射                    │ AMD 映射
─────────────────────┼──────────────────────────────┼──────────────────────
★ cycles                │ CPU_CLK_UNHALTED.THREAD_P    │ cpu cycles unhalted
★ instructions          │ INST_RETIRED.ANY             │ retired instructions
★ cache-references      │ LONGEST_LAT_CACHE.REFERENCE  │ L3 cache access
★ cache-misses          │ LONGEST_LAT_CACHE.MISS       │ L3 cache miss
★ branches              │ BR_INST_RETIRED.ALL_BRANCHES │ retired branch instr
★ branch-misses         │ BR_MISP_RETIRED.ALL_BRANCHES │ retired mispredict branch

★ 通用事件由内核自动映射，用户使用方式完全相同
★ 差异在内核 PMU 驱动中处理：
  Intel: src/linux-5.10/arch/x86/events/intel/core.c
  AMD:   src/linux-5.10/arch/x86/events/amd/core.c
```

**源码佐证 — AMD 通用事件的硬件编码映射表：**

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/core.c:233-266 */

/* AMD K7 ~ Family 16h 的通用事件映射 */
static const u64 amd_perfmon_event_map[PERF_COUNT_HW_MAX] =
{
    /* ★ cycles 和 instructions 的编码跨代不变 */
    [PERF_COUNT_HW_CPU_CYCLES]          = 0x0076,
    [PERF_COUNT_HW_INSTRUCTIONS]        = 0x00c0,
    [PERF_COUNT_HW_CACHE_REFERENCES]    = 0x077d,   /* L2 cache access       */
    [PERF_COUNT_HW_CACHE_MISSES]        = 0x077e,   /* L2 cache miss         */
    [PERF_COUNT_HW_BRANCH_INSTRUCTIONS] = 0x00c2,
    [PERF_COUNT_HW_BRANCH_MISSES]       = 0x00c3,
    [PERF_COUNT_HW_STALLED_CYCLES_FRONTEND] = 0x00d0, /* "Decoder empty"     */
    [PERF_COUNT_HW_STALLED_CYCLES_BACKEND]  = 0x00d1, /* "Dispatch stalls"   */
};

/* ★ Family 17h（Zen 系列）使用不同的事件编码 */
static const u64 amd_f17h_perfmon_event_map[PERF_COUNT_HW_MAX] =
{
    [PERF_COUNT_HW_CPU_CYCLES]          = 0x0076,   /* ★ 与旧版相同 */
    [PERF_COUNT_HW_INSTRUCTIONS]        = 0x00c0,   /* ★ 与旧版相同 */
    [PERF_COUNT_HW_CACHE_REFERENCES]    = 0xff60,   /* ★ 编码变了！ */
    [PERF_COUNT_HW_CACHE_MISSES]        = 0x0964,   /* ★ 编码变了！ */
    [PERF_COUNT_HW_BRANCH_INSTRUCTIONS] = 0x00c2,
    [PERF_COUNT_HW_BRANCH_MISSES]       = 0x00c3,
    [PERF_COUNT_HW_STALLED_CYCLES_FRONTEND] = 0x0287,
    [PERF_COUNT_HW_STALLED_CYCLES_BACKEND]  = 0x0187,
};

/* ★ 运行时根据 CPU Family 选择映射表 */
static u64 amd_pmu_event_map(int hw_event)
{
    if (boot_cpu_data.x86 >= 0x17)   /* ★ Family 17h = Zen 1 及以上 */
        return amd_f17h_perfmon_event_map[hw_event];
    return amd_perfmon_event_map[hw_event];
}

/*
 * 要点：
 *   1. 用户输入 perf stat -e cycles 时，内核自动查表得到 raw 编码
 *   2. F17h 的 cache-references 编码 (0xff60) 与旧版 (0x077d) 不同
 *      → 因为 Zen 重新设计了缓存层级，L2 不再是"最后一级缓存"
 *   3. cycles (0x0076) 和 instructions (0x00c0) 跨代不变
 */
```

### 2.2 AMD 缓存事件编码（hw_cache_event_ids）

通用事件之外，`perf stat -e cache-misses` 等缓存事件也需要映射到硬件编码。
AMD 在 `core.c` 中维护了两张缓存事件表：

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/core.c:21-124（旧版）/ 126-228（F17h+） */

/* 仅展示关键差异：L1D 和 DTLB 的编码在 F17h 发生了变化 */

/* ── 旧版（K7 ~ Fam16h）的 L1D 缓存事件 ── */
[ C(L1D) ] = {
    [ C(OP_READ) ] = {
        [ C(RESULT_ACCESS) ] = 0x0040, /* Data Cache Accesses        */
        [ C(RESULT_MISS)   ] = 0x0141, /* Data Cache Misses          */
    },
},

/* ── F17h（Zen）的 L1D 缓存事件 ── */
[ C(L1D) ] = {
    [ C(OP_READ) ] = {
        [ C(RESULT_ACCESS) ] = 0x0040, /* Data Cache Accesses（不变） */
        [ C(RESULT_MISS)   ] = 0xc860, /* ★ L2$ access from DC Miss  */
                                        /*   含义变了！不再是 L1 miss  */
                                        /*   而是"因 L1 miss 导致的    */
                                        /*    L2 访问"                */
    },
},

/* ── F17h 的 DTLB 事件 ── */
[ C(DTLB) ] = {
    [ C(OP_READ) ] = {
        [ C(RESULT_ACCESS) ] = 0xff45, /* ★ All L2 DTLB accesses     */
        [ C(RESULT_MISS)   ] = 0xf045, /* ★ L2 DTLB misses (PT walks)*/
    },
},

/* ── F17h 的 LL（Last Level）缓存事件全部为 0 ── */
[ C(LL) ] = {
    [ C(OP_READ) ] = {
        [ C(RESULT_ACCESS) ] = 0,      /* ★ 不支持！*/
        [ C(RESULT_MISS)   ] = 0,      /* ★ 因为 AMD 的 L3 是 per-CCX */
    },                                  /*   需要走 uncore PMU 访问     */
},

/*
 * 要点：
 *   1. F17h 的 L1D miss 含义变为"L2 访问因 DC miss"——更贴近实际延迟影响
 *   2. F17h 的 LL (Last Level Cache) 事件全部为 0
 *      → AMD 的 L3 不在核心 PMU 中，而在 uncore PMU (amd_l3) 中
 *      → 这是与 Intel 的关键差异：Intel 的 LLC 事件直接映射到核心 PMU
 */
```

### 2.3 查看 AMD 可用事件

```bash
# ★ 在 AMD 服务器上使用 perf list 查看可用事件
perf list                              # 所有事件
perf list hw                           # 硬件事件
perf list cache                        # 缓存事件
perf list pmu                          # PMU 特定事件

# AMD 特有的 PMU 事件（通过 raw 编码访问）
perf list --desc --details             # 带描述的详细列表
```

### 2.4 AMD 原始事件编码

```bash
# AMD 使用 raw 事件编码（rNNN）访问架构特定事件
# 事件编码参考 AMD Processor Programming Reference (PPR)

# 示例：AMD Zen 3/4 的 L3 缓存事件
perf stat -e r0400000000431004 ./app     # L3 访问（AMD 特定编码）
perf stat -e r0400000000431008 ./app     # L3 miss

# 示例：AMD Data Fabric 事件（跨 NUMA 流量）
perf stat -e amd_df/event=0x7,umask=0x38/ ./app  # DRAM 读取

★ 注意：raw 事件编码因 AMD 微架构版本而异
  Zen 2、Zen 3、Zen 4 的事件编码不同
  参考文档：AMD PPR for Family 19h（Zen 3/4）或 Family 1Ah（Zen 5）
```

**源码佐证 — AMD uncore PMU 的事件格式属性：**

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/uncore.c:288-314 */

/* 定义 Data Fabric 和 L3 的事件格式（sysfs 展示给用户） */
DEFINE_UNCORE_FORMAT_ATTR(event12, event, "config:0-7,32-35");
DEFINE_UNCORE_FORMAT_ATTR(event14, event, "config:0-7,32-35,59-60"); /* F17h+ DF */
DEFINE_UNCORE_FORMAT_ATTR(event8,  event, "config:0-7");              /* F17h+ L3 */
DEFINE_UNCORE_FORMAT_ATTR(umask,   umask, "config:8-15");
DEFINE_UNCORE_FORMAT_ATTR(coreid,  coreid, "config:42-44");           /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(slicemask, slicemask, "config:48-51");      /* F17h L3 */
DEFINE_UNCORE_FORMAT_ATTR(threadmask8, threadmask, "config:56-63");   /* F17h L3 */
DEFINE_UNCORE_FORMAT_ATTR(enallslices, enallslices, "config:46");     /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(enallcores,  enallcores, "config:47");     /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(sliceid, sliceid, "config:48-50");          /* F19h L3 */

/*
 * 要点：
 *   1. F17h 的 L3 事件：8-bit event + 8-bit umask + slicemask + threadmask
 *   2. F19h（Zen 3）增加了 coreid / enallslices / enallcores
 *      → 可以按核心或 L3 slice 精细过滤
 *   3. Data Fabric 事件：12~14-bit event + umask
 *      → 用于监控跨 NUMA 的内存访问流量
 */
```

### 2.5 AMD 与 Intel 的微架构差异

```
特性                    │ Intel (Xeon)         │ AMD (EPYC)
───────────────────────┼─────────────────────┼─────────────────────
L3 结构                │ 统一 LLC             │ ★ 每 CCX 独立 L3
                       │ 所有核心共享          │ 核心只能访问本 CCX 的 L3
CCX / CCD             │ 不适用                │ ★ 4~8 核一个 CCX
                       │                       │ 2~12 个 CCX 一个 CCD
NUMA 拓扑             │ 每 socket 1 NUMA     │ ★ 每 CCD 可能 1 NUMA
                       │                       │ （NPS 配置决定）
PMC 数量               │ 4 通用 + 3 固定      │ 4~6 通用（无固定）
精确采样               │ PEBS                 │ ★ IBS（更强大）
L3 miss 事件          │ 全局 LLC miss        │ 按 CCX 分别统计
```

**源码佐证 — AMD PMU 结构体与初始化：**

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/core.c:908-939 */

static __initconst const struct x86_pmu amd_pmu = {
    .name           = "AMD",
    .handle_irq     = amd_pmu_handle_irq,
    .disable_all    = amd_pmu_disable_all,
    .enable_all     = x86_pmu_enable_all,
    .enable         = x86_pmu_enable_event,
    .disable        = amd_pmu_disable_event,
    .hw_config      = amd_pmu_hw_config,
    .schedule_events = x86_schedule_events,
    .eventsel       = MSR_K7_EVNTSEL0,    /* ★ 事件选择 MSR 基址 */
    .perfctr        = MSR_K7_PERFCTR0,    /* ★ 计数器 MSR 基址 */
    .addr_offset    = amd_pmu_addr_offset,
    .event_map      = amd_pmu_event_map,   /* ★ 通用事件映射函数 */
    .max_events     = ARRAY_SIZE(amd_perfmon_event_map),
    .num_counters   = AMD64_NUM_COUNTERS,  /* ★ 4 个通用计数器（Intel 有 4+3） */
    .cntval_bits    = 48,                  /* ★ 48-bit 计数器宽度 */
    .cntval_mask    = (1ULL << 48) - 1,
    .apic           = 1,
    .max_period     = (1ULL << 47) - 1,    /* ★ 用最高位检测溢出 */
    .get_event_constraints = amd_get_event_constraints,
    .put_event_constraints = amd_put_event_constraints,
    .amd_nb_constraints = 1,  /* ★ 默认启用 NorthBridge 约束 */
};

/* 源码位置：core.c:941-993 — 核心 PMU 扩展初始化 */
static int __init amd_core_pmu_init(void)
{
    if (!boot_cpu_has(X86_FEATURE_PERFCTR_CORE))
        return 0;

    /* ★ 如果有 core perf counter extensions，切换到 F15h MSR */
    x86_pmu.eventsel  = MSR_F15H_PERF_CTL;   /* ★ 0xc0010200 */
    x86_pmu.perfctr   = MSR_F15H_PERF_CTR;   /* ★ 0xc0010201 */
    x86_pmu.num_counters = AMD64_NUM_COUNTERS_CORE; /* ★ 6 个计数器 */
    x86_pmu.amd_nb_constraints = 0; /* NB 事件由 uncore PMU 单独处理 */

    if (boot_cpu_data.x86 >= 0x17) {         /* ★ Family 17h (Zen) */
        /* Large Increment per Cycle 事件必须分配在偶数编号的计数器上
           且需要相邻的奇数计数器配合 */
        x86_pmu.get_event_constraints = amd_get_event_constraints_f17h;
        x86_pmu.put_event_constraints = amd_put_event_constraints_f17h;
        x86_pmu.perf_ctr_pair_en = AMD_MERGE_EVENT_ENABLE;
        x86_pmu.flags |= PMU_FL_PAIR;
    }
    return 0;
}

/* 源码位置：core.c:996-1024 — AMD PMU 总入口 */
__init int amd_pmu_init(void)
{
    /* Performance-monitoring supported from K7 and later */
    if (boot_cpu_data.x86 < 6)
        return -ENODEV;

    x86_pmu = amd_pmu;              /* ★ 设置全局 PMU 为 AMD 版本 */
    ret = amd_core_pmu_init();       /* ★ 初始化核心计数器扩展 */

    /* ★ 根据 Family 选择缓存事件表 */
    if (boot_cpu_data.x86 >= 0x17)
        memcpy(hw_cache_event_ids, amd_hw_cache_event_ids_f17h, ...);
    else
        memcpy(hw_cache_event_ids, amd_hw_cache_event_ids, ...);

    return 0;
}

/*
 * 要点：
 *   1. num_counters: 旧版 4 个，F15h+ 扩展到 6 个（Intel 有 4 通用 + 3 固定）
 *   2. MSR 基址从 K7 的 MSR_K7_EVNTSEL0 切换到 F15h 的 MSR_F15H_PERF_CTL
 *   3. F17h 引入"大增量事件"约束——某些高频事件必须成对使用相邻计数器
 */
```

---

## 三、AMD IBS（Instruction Based Sampling）

### 3.1 IBS 是什么

```
IBS（Instruction Based Sampling）是 AMD 独有的精确采样技术：
  → ★ 比 Intel PEBS 功能更丰富
  → 可以同时获取：
    - 指令的精确地址
    - 指令的延迟（周期数）
    - 数据来源（缓存层级）
    - 分支预测结果
    - TLB 命中情况

★ IBS 有两种模式（源码 ibs.c:191-192）：
  perf_ibs_fetch → 指令获取采样（Instruction Fetch）
  perf_ibs_op    → 操作采样（Micro-Op）
```

**源码佐证 — IBS 能力检测（CPUID）：**

```c
/* 源码位置：src/linux-5.10/arch/x86/include/asm/perf_event.h:362-382 */

#define IBS_CPUID_FEATURES      0x8000001b     /* ★ CPUID 叶子节点 */

/* IBS 能力标志位（来自 CPUID Fn8000_001B_EAX） */
#define IBS_CAPS_AVAIL          (1U<<0)        /* IBS 可用 */
#define IBS_CAPS_FETCHSAM       (1U<<1)        /* ★ 指令获取采样 */
#define IBS_CAPS_OPSAM          (1U<<2)        /* ★ 操作采样 */
#define IBS_CAPS_RDWROPCNT      (1U<<3)        /* 读写操作计数 */
#define IBS_CAPS_OPCNT          (1U<<4)        /* ★ 操作计数（周期 vs μop） */
#define IBS_CAPS_BRNTRGT        (1U<<5)        /* ★ 分支目标地址 */
#define IBS_CAPS_OPCNTEXT       (1U<<6)        /* ★ 扩展操作计数 */
#define IBS_CAPS_RIPINVALIDCHK  (1U<<7)        /* RIP 无效检查 */
#define IBS_CAPS_OPBRNFUSE      (1U<<8)        /* 分支融合 */
#define IBS_CAPS_FETCHCTLEXTD   (1U<<9)        /* 扩展 Fetch 控制 */
#define IBS_CAPS_OPDATA4        (1U<<10)       /* ★ 第 4 个 Op 数据寄存器 */

/* 最低保证的能力（不查 CPUID 时的默认值） */
#define IBS_CAPS_DEFAULT        (IBS_CAPS_AVAIL | IBS_CAPS_FETCHSAM | IBS_CAPS_OPSAM)

/* 源码位置：src/linux-5.10/arch/x86/events/amd/ibs.c:799-818 */

static __init u32 __get_ibs_caps(void)
{
    if (!boot_cpu_has(X86_FEATURE_IBS))
        return 0;                              /* ★ CPU 不支持 IBS */

    max_level = cpuid_eax(0x80000000);
    if (max_level < IBS_CPUID_FEATURES)
        return IBS_CAPS_DEFAULT;               /* ★ CPUID 级别不够，用默认 */

    caps = cpuid_eax(IBS_CPUID_FEATURES);      /* ★ 读取 IBS 能力位 */
    if (!(caps & IBS_CAPS_AVAIL))
        return IBS_CAPS_DEFAULT;
    return caps;
}

/*
 * 要点：
 *   1. IBS 能力通过 CPUID 0x8000001b 检测
 *   2. 最基本的能力是 FETCHSAM + OPSAM（指令获取 + 操作采样）
 *   3. 高级能力（分支目标、扩展计数等）随 CPU 代际增加
 */
```

**源码佐证 — IBS PMU 结构体定义：**

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/ibs.c:538-586 */

/* ★ IBS Fetch PMU — 指令获取采样 */
static struct perf_ibs perf_ibs_fetch = {
    .pmu = {
        .task_ctx_nr  = perf_invalid_context,
        .event_init   = perf_ibs_init,
        .add          = perf_ibs_add,
        .del          = perf_ibs_del,
        .start        = perf_ibs_start,
        .stop         = perf_ibs_stop,
        .read         = perf_ibs_read,
        .capabilities = PERF_PMU_CAP_NO_EXCLUDE,
    },
    .msr          = MSR_AMD64_IBSFETCHCTL,     /* ★ 0xc0011030 */
    .config_mask  = IBS_FETCH_CONFIG_MASK,
    .cnt_mask     = IBS_FETCH_MAX_CNT,         /* 16-bit 最大计数 */
    .enable_mask  = IBS_FETCH_ENABLE,           /* bit 48 */
    .valid_mask   = IBS_FETCH_VAL,              /* bit 49 */
    .max_period   = IBS_FETCH_MAX_CNT << 4,
    .get_count    = get_ibs_fetch_count,
};

/* ★ IBS Op PMU — 微操作采样 */
static struct perf_ibs perf_ibs_op = {
    .pmu = {
        .task_ctx_nr  = perf_invalid_context,
        .event_init   = perf_ibs_init,
        /* ... 与 fetch 相同的回调 ... */
    },
    .msr          = MSR_AMD64_IBSOPCTL,        /* ★ 0xc0011033 */
    .config_mask  = IBS_OP_CONFIG_MASK,
    .cnt_mask     = IBS_OP_MAX_CNT | IBS_OP_CUR_CNT | IBS_OP_CUR_CNT_RAND,
    .enable_mask  = IBS_OP_ENABLE,              /* bit 17 */
    .valid_mask   = IBS_OP_VAL,                 /* bit 18 */
    .max_period   = IBS_OP_MAX_CNT << 4,
    .get_count    = get_ibs_op_count,
};

/* IBS 相关的 MSR 定义 */
/* 源码位置：src/linux-5.10/arch/x86/include/asm/msr-index.h:454-472 */
/*
 * MSR_AMD64_IBSFETCHCTL   = 0xc0011030  — Fetch 控制寄存器
 * MSR_AMD64_IBSFETCHLINAD = 0xc0011031  — Fetch 线性地址
 * MSR_AMD64_IBSFETCHPHYSAD= 0xc0011032  — Fetch 物理地址
 * MSR_AMD64_IBSOPCTL      = 0xc0011033  — Op 控制寄存器
 * MSR_AMD64_IBSOPRIP      = 0xc0011034  — Op 指令指针（RIP）
 * MSR_AMD64_IBSOPDATA     = 0xc0011035  — Op 数据 1
 * MSR_AMD64_IBSOPDATA2    = 0xc0011036  — Op 数据 2
 * MSR_AMD64_IBSOPDATA3    = 0xc0011037  — Op 数据 3（含 DC miss 信息）
 * MSR_AMD64_IBSDCLINAD    = 0xc0011038  — DC 线性地址
 * MSR_AMD64_IBSDCPHYSAD   = 0xc0011039  — DC 物理地址
 * MSR_AMD64_IBSCTL        = 0xc001103a  — IBS 总控制
 * MSR_AMD64_IBSBRTARGET   = 0xc001103b  — 分支目标地址（★ IBS_CAPS_BRNTRGT）
 * MSR_AMD64_IBSOPDATA4    = 0xc001103d  — Op 数据 4（★ IBS_CAPS_OPDATA4）
 *
 * ★ Op 有 7~8 个 MSR 寄存器，提供远比 Fetch 更丰富的信息
 *   这也是 IBS Op 比 PEBS 更强大的硬件基础
 */
```

### 3.2 IBS 使用方法

```bash
# IBS Fetch（指令获取采样）
perf record -e ibs_fetch// ./app
perf report

# IBS Op（操作采样）
perf record -e ibs_op// ./app
perf report

# IBS 提供精确的 per-instruction 分析
# 比 Intel PEBS 更细粒度（可以看到每条指令的延迟）

# 查看 IBS 是否可用
perf list | grep ibs
# 输出：
#   ibs_fetch//  [Kernel PMU event]
#   ibs_op//     [Kernel PMU event]
```

**源码佐证 — IBS 事件初始化与精确事件透明替代：**

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/ibs.c:257-315 */

static int perf_ibs_init(struct perf_event *event)
{
    struct perf_ibs *perf_ibs;

    /* ★ 先判断是否为直接 IBS 事件（ibs_fetch 或 ibs_op） */
    perf_ibs = get_ibs_pmu(event->attr.type);
    if (perf_ibs) {
        config = event->attr.config;
    } else {
        /* ★ 否则尝试将通用事件透明转换为 IBS Op */
        perf_ibs = &perf_ibs_op;
        ret = perf_ibs_precise_event(event, &config);
        if (ret)
            return ret;
    }

    /* ★ 采样周期对齐：IBS 要求低 4 位为 0 */
    if (hwc->sample_period) {
        hwc->sample_period &= ~0x0FULL;  /* ★ 清除低 4 位 */
        if (!hwc->sample_period)
            hwc->sample_period = 0x10;   /* ★ 最小周期 16 */
    }

    hwc->config_base = perf_ibs->msr;    /* ★ 设置 MSR 基址 */
    hwc->config = config;
    return 0;
}

/* 源码位置：ibs.c:220-244 — 精确事件的透明替代 */
/*
 * Use IBS for precise event sampling:
 *   perf record -a -e cpu-cycles:p ...    # use ibs op counting cycles
 *   perf record -a -e r076:p ...          # same as -e cpu-cycles:p
 *   perf record -a -e r0C1:p ...          # use ibs op counting micro-ops
 */
static int perf_ibs_precise_event(struct perf_event *event, u64 *config)
{
    switch (event->attr.precise_ip) {
    case 0:
        return -ENOENT;      /* 非精确事件，不走 IBS */
    case 1:
    case 2:
        break;               /* ★ 精确级别 1~2 支持 */
    default:
        return -EOPNOTSUPP;  /* 精确级别 > 2 不支持 */
    }

    switch (event->attr.type) {
    case PERF_TYPE_HARDWARE:
        switch (event->attr.config) {
        case PERF_COUNT_HW_CPU_CYCLES:
            *config = 0;     /* ★ cycles → IBS Op（周期计数模式） */
            return 0;
        }
        break;
    case PERF_TYPE_RAW:
        switch (event->attr.config) {
        case 0x0076:
            *config = 0;     /* ★ raw cycles → IBS Op */
            return 0;
        }
        break;
    }
    return -ENOENT;
}

/*
 * 要点：
 *   1. perf record -e cpu-cycles:p 在 AMD 上自动使用 IBS Op
 *      → 用户无需显式指定 ibs_op，内核透明替代
 *   2. IBS 采样周期低 4 位必须为 0（硬件限制）
 *   3. IBS 支持精确级别 1~2（precise_ip），不支持级别 3
 */
```

### 3.3 IBS vs Intel PEBS

```
特性              │ Intel PEBS              │ AMD IBS
─────────────────┼────────────────────────┼────────────────────────
精确性           │ 指令级                  │ ★ 微操作级
延迟信息         │ load 延迟               │ ★ 指令延迟 + load 延迟
分支信息         │ 有限                    │ ★ 完整的分支预测结果
TLB 信息         │ 无                      │ ★ TLB 命中/miss
数据来源         │ 缓存层级                │ ★ 缓存层级 + NUMA 节点
采样开销         │ 低                      │ ★ 低
可用性           │ Intel Core 及以上       │ ★ AMD Family 10h 及以上

★ 在 AMD 上，IBS 是最强大的性能分析工具
  → 可以精确定位到每条指令的延迟来源
  → 支持 perf c2c（false sharing 检测）
  → 支持 perf mem（内存延迟分析）
```

**源码佐证 — IBS NMI 中断处理（采样数据收集的核心流程）：**

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/ibs.c:712-728 */

/* ★ NMI 入口：同时处理 fetch 和 op 两种 IBS */
static int perf_ibs_nmi_handler(unsigned int cmd, struct pt_regs *regs)
{
    u64 stamp = sched_clock();
    int handled = 0;

    handled += perf_ibs_handle_irq(&perf_ibs_fetch, regs); /* ★ 处理 fetch */
    handled += perf_ibs_handle_irq(&perf_ibs_op, regs);    /* ★ 处理 op */

    if (handled)
        inc_irq_stat(apic_perf_irqs);

    perf_sample_event_took(sched_clock() - stamp);
    return handled;
}

/* 源码位置：ibs.c:588-710 — IBS 中断处理的详细逻辑 */
static int perf_ibs_handle_irq(struct perf_ibs *perf_ibs, struct pt_regs *iregs)
{
    struct cpu_perf_ibs *pcpu = this_cpu_ptr(perf_ibs->pcpu);
    struct perf_event *event = pcpu->event;

    /* ★ 检查 IBS 是否处于 STARTED 状态 */
    if (!test_bit(IBS_STARTED, pcpu->state)) {
        /* 捕获停止后的迟到 NMI */
        if (test_and_clear_bit(IBS_STOPPED, pcpu->state))
            return 1;
        return 0;
    }

    /* ★ 读取 IBS 控制 MSR，检查 Valid 位 */
    msr = hwc->config_base;
    buf = ibs_data.regs;
    rdmsrl(msr, *buf);                    /* ★ 读 MSR（如 0xc0011030） */
    if (!(*buf++ & perf_ibs->valid_mask))  /* ★ 检查 VAL 位 */
        goto fail;

    /* ★ 逐个读取 IBS 数据寄存器 */
    offset = 1;
    do {
        rdmsrl(msr + offset, *buf++);     /* ★ 读后续的 MSR 寄存器 */
        size++;
        offset = find_next_bit(perf_ibs->offset_mask,
                               perf_ibs->offset_max, offset + 1);
    } while (offset < offset_max);

    /* ★ 额外读取：分支目标、Op 数据 4 等（按能力位决定） */
    if (event->attr.sample_type & PERF_SAMPLE_RAW) {
        if (perf_ibs == &perf_ibs_op) {
            if (ibs_caps & IBS_CAPS_BRNTRGT) {
                rdmsrl(MSR_AMD64_IBSBRTARGET, *buf++); /* ★ 分支目标地址 */
                size++;
            }
            if (ibs_caps & IBS_CAPS_OPDATA4) {
                rdmsrl(MSR_AMD64_IBSOPDATA4, *buf++);  /* ★ 第 4 个数据寄存器 */
                size++;
            }
        }
    }

    /* ★ 设置精确 IP：IBS 的 RIP 是精确的（skid = 0） */
    regs = *iregs;
    if (check_rip && (ibs_data.regs[2] & IBS_RIP_INVALID)) {
        regs.flags &= ~PERF_EFLAGS_EXACT;  /* RIP 无效，回退到 pt_regs */
    } else {
        set_linear_ip(&regs, ibs_data.regs[1]); /* ★ 使用 IBS 记录的精确 RIP */
        regs.flags |= PERF_EFLAGS_EXACT;
    }

    /* ★ 提交采样数据并处理溢出限流 */
    throttle = perf_event_overflow(event, &data, &regs);

    if (!throttle)
        perf_ibs_enable_event(perf_ibs, hwc, new_config); /* 重新启用采样 */

    return 1;
}

/*
 * IBS NMI 处理流程：
 *
 *   硬件触发 NMI
 *       │
 *       ▼
 *   perf_ibs_nmi_handler()        ← 注册在 NMI_LOCAL
 *       │
 *       ├── perf_ibs_handle_irq(fetch)   ← 读 Fetch MSR (0xc0011030~32)
 *       │       │
 *       │       ├── 检查 IBS_FETCH_VAL (bit 49)
 *       │       ├── 读线性地址、物理地址
 *       │       └── 提交 perf 采样
 *       │
 *       └── perf_ibs_handle_irq(op)      ← 读 Op MSR (0xc0011033~39,3b,3d)
 *               │
 *               ├── 检查 IBS_OP_VAL (bit 18)
 *               ├── 读 RIP、OpData1~4、DC 地址
 *               ├── 读分支目标（如果 IBS_CAPS_BRNTRGT）
 *               ├── 设置 PERF_EFLAGS_EXACT（精确 IP）
 *               └── 提交 perf 采样
 *
 * 要点：
 *   1. IBS 通过 NMI（非屏蔽中断）触发，保证不被其他中断打断
 *   2. Op 采样的 MSR 多达 7~8 个，包含完整的执行上下文
 *   3. IBS_RIP_INVALID 标志处理——极少数情况下 RIP 可能无效
 *   4. 限流机制：如果采样过于频繁，perf_event_overflow 返回 throttle
 */
```

---

## 四、AMD 服务器性能分析注意事项

### 4.1 NUMA 拓扑分析

```
AMD EPYC 的 NUMA 拓扑比 Intel 更复杂：

NPS（Nodes Per Socket）配置：
  ★ NPS1: 1 个 NUMA 节点/socket（所有 CCD 共享）
  ★ NPS2: 2 个 NUMA 节点/socket
  NPS4: 4 个 NUMA 节点/socket（★ 每个 CCD 一个 NUMA）

★ NPS4 性能最好但 NUMA 敏感性最高
  → ★ 跨 NUMA 访问延迟增加 50%~100%
  → 必须使用 ★ numactl 绑定进程到正确的 NUMA 节点

检查 NUMA 配置：
  numactl --hardware
  lscpu | grep NUMA
```

### 4.2 CCX 内 vs CCX 间的 L3 访问

```
AMD 的 L3 是 per-CCX 的（非全局共享）：

  CCD 0                    CCD 1
  ┌─────────────┐         ┌─────────────┐
  │ Core 0~7    │         │ Core 8~15   │
  │ L3 (32MB)   │         │ L3 (32MB)   │
  └─────────────┘         └─────────────┘
       │                        │
       └──── 内存控制器 ────────┘

★ Core 0 访问 CCD 0 的 L3: ~40 周期（快）
★ Core 0 访问 CCD 1 的 L3: 不存在！（只能访问本 CCX 的 L3）
★ Core 0 访问远程 DRAM: ~200 周期（经过内存控制器）

→ perf stat -e cache-misses 在 AMD 上显示的是**本 CCX 的 L3 miss**
  不像 Intel 是全局 LLC miss
→ 这意味着 AMD 上 cache-miss 的含义不同！
```

**源码佐证 — AMD Uncore PMU 初始化（L3 和 Data Fabric）：**

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/uncore.c:557-670 */

static int __init amd_uncore_init(void)
{
    /* ★ 仅 AMD 和 Hygon（海光）CPU 进入 */
    if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD &&
        boot_cpu_data.x86_vendor != X86_VENDOR_HYGON)
        return -ENODEV;

    if (!boot_cpu_has(X86_FEATURE_TOPOEXT))
        return -ENODEV;

    num_counters_nb  = NUM_COUNTERS_NB;     /* ★ 4 个 NB/DF 计数器 */
    num_counters_llc = NUM_COUNTERS_L2;     /* 默认 4 个 L2 计数器 */

    if (boot_cpu_data.x86 >= 0x17) {
        /* ★ F17h+ 重命名 PMU：NB → amd_df，L2 → amd_l3 */
        num_counters_llc = NUM_COUNTERS_L3; /* ★ F17h+ 有 6 个 L3 计数器 */
        amd_nb_pmu.name  = "amd_df";        /* ★ Data Fabric PMU */
        amd_llc_pmu.name = "amd_l3";        /* ★ L3 Cache PMU */
        l3_mask          = true;
    }

    /* ★ 注册 NorthBridge / Data Fabric PMU */
    if (boot_cpu_has(X86_FEATURE_PERFCTR_NB)) {
        amd_uncore_nb = alloc_percpu(struct amd_uncore *);
        ret = perf_pmu_register(&amd_nb_pmu, amd_nb_pmu.name, -1);
        /* → 用户空间可见为 "amd_df" 或 "amd_nb" */
    }

    /* ★ 注册 LLC (L3 Cache) PMU */
    if (boot_cpu_has(X86_FEATURE_PERFCTR_LLC)) {
        /* F19h（Zen 3）增加更多 L3 格式属性 */
        if (boot_cpu_data.x86 >= 0x19) {
            *l3_attr++ = &format_attr_event8.attr;
            *l3_attr++ = &format_attr_umask.attr;
            *l3_attr++ = &format_attr_coreid.attr;      /* ★ 按核心过滤 */
            *l3_attr++ = &format_attr_enallslices.attr;  /* ★ 所有 slice */
            *l3_attr++ = &format_attr_enallcores.attr;   /* ★ 所有核心 */
            *l3_attr++ = &format_attr_sliceid.attr;      /* ★ 按 slice 过滤 */
            *l3_attr++ = &format_attr_threadmask2.attr;
        }
        amd_uncore_llc = alloc_percpu(struct amd_uncore *);
        ret = perf_pmu_register(&amd_llc_pmu, amd_llc_pmu.name, -1);
        /* → 用户空间可见为 "amd_l3" */
    }

    return 0;
}
device_initcall(amd_uncore_init);  /* ★ 内核启动时自动调用 */

/*
 * 要点：
 *   1. amd_l3 PMU 是 per-CCX 的——每个 CCD 有自己的 L3 计数器
 *      → 这就是为什么 perf stat -e cache-misses 只统计本 CCX
 *   2. F17h+ 将 NB PMU 重命名为 amd_df（Data Fabric）
 *      → 用于监控跨 NUMA 的内存流量
 *   3. F19h（Zen 3）的 L3 PMU 支持按 coreid / sliceid 精细过滤
 *      → perf stat -e amd_l3/event=X,sliceid=2/ 可看特定 L3 slice
 *   4. L3 有 6 个计数器（NUM_COUNTERS_L3），NB/DF 有 4 个
 */
```

**源码佐证 — Uncore PMU 结构体和 L3 计数器配置：**

```c
/* 源码位置：src/linux-5.10/arch/x86/events/amd/uncore.c:21-51 */

#define NUM_COUNTERS_NB     4       /* ★ NorthBridge/Data Fabric: 4 个计数器 */
#define NUM_COUNTERS_L2     4       /* ★ L2 缓存: 4 个计数器（旧版） */
#define NUM_COUNTERS_L3     6       /* ★ L3 缓存: 6 个计数器（F17h+） */
#define MAX_COUNTERS        6

#define RDPMC_BASE_NB       6       /* NB/DF 的 RDPMC 基址 */
#define RDPMC_BASE_LLC      10      /* L3 的 RDPMC 基址 */

struct amd_uncore {
    int id;
    int refcnt;
    int cpu;
    int num_counters;               /* ★ 计数器数量 */
    int rdpmc_base;                 /* ★ RDPMC 基址 */
    u32 msr_base;                   /* ★ MSR 基址（写入配置/读取计数） */
    cpumask_t *active_mask;
    struct pmu *pmu;
    struct perf_event *events[MAX_COUNTERS]; /* ★ 每个计数器的当前事件 */
    struct hlist_node node;
};

/* 源码位置：uncore.c:352-363 — L3 PMU 结构体 */
static struct pmu amd_llc_pmu = {
    .task_ctx_nr  = perf_invalid_context,
    .attr_groups  = amd_uncore_l3_attr_groups,
    .name         = "amd_l2",         /* ★ F17h+ 会被改名为 "amd_l3" */
    .event_init   = amd_uncore_event_init,
    .add          = amd_uncore_add,
    .del          = amd_uncore_del,
    .start        = amd_uncore_start,
    .stop         = amd_uncore_stop,
    .read         = amd_uncore_read,
    .capabilities = PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
    /* ★ NO_INTERRUPT: uncore 计数器不产生中断（没有溢出中断） */
    /* ★ NO_EXCLUDE: 不支持 per-process 过滤（per-CCX 全局计数） */
};
```

### 4.3 AMD 性能分析快速命令

```bash
# 1. 基本性能指标
perf stat -d -r 3 ./app

# 2. 使用 IBS 进行精确分析
perf record -e ibs_op// -g ./app
perf report --stdio

# 3. NUMA 分析
perf stat -e cache-misses -C 0 -- ./app   # 只看 CPU 0
perf stat -e cache-misses -C 8 -- ./app   # 对比 CPU 8（不同 CCD）

# 4. 内存访问分析
perf mem record -t load ./app
perf mem report --sort=mem,snoop --stdio

# 5. False sharing 检测
perf c2c record ./app
perf c2c report --stdio
```

---

## 总结

### AMD vs Intel 关键差异速查

```
项目              │ Intel                      │ AMD
─────────────────┼───────────────────────────┼──────────────────────────
L3 结构           │ 全局共享 LLC               │ per-CCX 独立 L3
精确采样          │ PEBS                       │ ★ IBS（更强大）
NUMA              │ 每 socket 1 NUMA           │ 每 CCD 可配 NUMA
cache-miss 含义   │ 全局 LLC miss              │ 本 CCX L3 miss
原始事件编码      │ Intel SDM Vol.3B           │ AMD PPR
PMC 数量          │ 4 通用 + 3 固定            │ 4~6 通用
查看事件          │ perf list                  │ perf list（相同命令）
L3 PMU            │ 集成在核心 PMU             │ ★ 独立 amd_l3 uncore PMU
DF PMU            │ 无                         │ ★ amd_df（监控跨 NUMA）
```

### 源码位置

```
perf mem:          src/linux-5.10/tools/perf/builtin-mem.c
AMD PMU 驱动:      src/linux-5.10/arch/x86/events/amd/core.c
  └ 事件映射表:       amd_perfmon_event_map (core.c:233)
  └ F17h 事件表:      amd_f17h_perfmon_event_map (core.c:248)
  └ 缓存事件表:       amd_hw_cache_event_ids (core.c:21)
  └ F17h 缓存表:      amd_hw_cache_event_ids_f17h (core.c:126)
  └ PMU 结构体:       amd_pmu (core.c:908)
  └ PMU 初始化:       amd_pmu_init() (core.c:996)
AMD IBS:           src/linux-5.10/arch/x86/events/amd/ibs.c
  └ IBS 能力位:       IBS_CAPS_* (perf_event.h:368-378)
  └ Fetch PMU:        perf_ibs_fetch (ibs.c:538)
  └ Op PMU:           perf_ibs_op (ibs.c:563)
  └ 精确事件替代:     perf_ibs_precise_event() (ibs.c:220)
  └ NMI 处理:         perf_ibs_handle_irq() (ibs.c:588)
  └ IBS 初始化:       perf_event_ibs_init() (ibs.c:761)
AMD Uncore:        src/linux-5.10/arch/x86/events/amd/uncore.c
  └ L3 PMU:           amd_llc_pmu (uncore.c:352)
  └ DF PMU:           amd_nb_pmu (uncore.c:338)
  └ Uncore 初始化:    amd_uncore_init() (uncore.c:557)
  └ 事件格式属性:     uncore.c:288-314
Intel PMU 驱动:    src/linux-5.10/arch/x86/events/intel/core.c
Intel PEBS:        src/linux-5.10/arch/x86/events/intel/ds.c
IBS 常量定义:      src/linux-5.10/arch/x86/include/asm/perf_event.h:362-411
MSR 定义:          src/linux-5.10/arch/x86/include/asm/msr-index.h:454-506
```

### AMD PMU 调用链总览

```
┌────────────────────────────────────────────────────────────────────┐
│ 用户空间                       │ 内核空间                          │
├────────────────────────────────┼───────────────────────────────────┤
│                                │                                   │
│ perf stat -e cycles            │                                   │
│      │                         │                                   │
│      ▼                         │                                   │
│ perf_event_open() ─────────────┼──► x86_pmu_event_init()           │
│                                │         │                         │
│                                │         ▼                         │
│                                │    amd_pmu_event_map()            │
│                                │    → 查 amd_f17h_perfmon_event_map│
│                                │    → 得到 0x0076 (cycles)         │
│                                │                                   │
│ perf record -e cpu-cycles:p    │                                   │
│      │                         │                                   │
│      ▼                         │                                   │
│ perf_event_open(:p) ───────────┼──► perf_ibs_precise_event()       │
│                                │    → 透明替代为 IBS Op            │
│                                │                                   │
│ perf record -e ibs_op//        │                                   │
│      │                         │                                   │
│      ▼                         │                                   │
│ perf_event_open(ibs_op) ───────┼──► perf_ibs_init()                │
│                                │    → 设置 MSR_AMD64_IBSOPCTL      │
│                                │                                   │
│                                │    NMI 触发时:                     │
│                                │    perf_ibs_nmi_handler()         │
│                                │    → perf_ibs_handle_irq(fetch)    │
│                                │    → perf_ibs_handle_irq(op)      │
│                                │    → rdmsrl() 读 IBS MSR 数据     │
│                                │    → perf_event_overflow()        │
│                                │                                   │
│ perf stat -e amd_l3/...        │                                   │
│      │                         │                                   │
│      ▼                         │                                   │
│ perf_event_open(amd_l3) ───────┼──► amd_uncore_event_init()        │
│                                │    → amd_uncore_add()             │
│                                │    → wrmsrl(MSR_F16H_L2I_PERF_CTL)│
│                                │    → rdpmcl() 读计数器             │
└────────────────────────────────────────────────────────────────────┘
```
