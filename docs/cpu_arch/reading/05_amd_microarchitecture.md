<!--
======================================================================
  Step 5: AMD 微架构专题（Zen 3 / Zen 4 / Zen 5）
======================================================================
  来源: AMD PPR (Processor Programming Reference)、AMD Software
        Optimization Guide、Linux 内核文档、Agner Fog 微架构手册
  阅读要点:
    - CCD（Core Complex Die）和 CCX（Core Complex Chiplet）拓扑
    - 8 核共享 32MB L3 Cache 的含义与性能影响
    - NUMA node per socket vs NPS（Node Per Socket）配置
    - AMD vs Intel 的关键架构差异
    - Infinity Fabric 互连与跨 CCD 延迟
  预计时间: 30-40 分钟
======================================================================
-->

# AMD 微架构专题（Zen 3 / Zen 4 / Zen 5）

> AMD EPYC 服务器在数据中心越来越普遍。理解 AMD 的独特拓扑
> 对性能调优至关重要——CCD/CCX 结构、L3 分区、NPS 配置都
> 与 Intel 平台有本质差异。

---

## 目录

- [1. AMD Zen 架构演进](#1-amd-zen-架构演进)
- [2. CCD 与 CCX 拓扑](#2-ccd-与-ccx-拓扑)
- [3. ★ 8 核共享 32MB L3 Cache](#3--8-核共享-32mb-l3-cache)
- [4. NUMA 与 NPS 配置](#4-numa-与-nps-配置)
- [5. AMD vs Intel 关键差异](#5-amd-vs-intel-关键差异)
- [6. Infinity Fabric 互连](#6-infinity-fabric-互连)
- [7. AMD 上的性能分析工具](#7-amd-上的性能分析工具)
- [8. 关键术语速查](#8-关键术语速查)

---

## 1. AMD Zen 架构演进

### 1.1 三代 Zen 架构对比

| 参数 | Zen 3 (Milan/5000) | Zen 4 (Genoa/7000) | Zen 5 (Turin/9000) |
|------|---------------------|---------------------|---------------------|
| 制程 | TSMC 7nm | TSMC 5nm | TSMC 4nm/3nm |
| 每 CCX 核心数 | ★ 8 核 | ★ 8 核 | ★ 8 核（最多 16 核 CCD） |
| 每 CCD L3 | 32 MB | 32 MB | 32 MB（可选 64 MB 3D V-Cache） |
| 每核 L2 | 512 KB | ★ 1 MB | 1 MB |
| 每核 L1 I/D | 32 KB / 32 KB | 32 KB / 32 KB | 32 KB / 32 KB（双周期访问） |
| ROB 大小 | 256 条目 | 320 条目 | 448 条目 |
| 调度器宽度 | 4-wide | 4-wide | ★ 6-wide（前端）/ 8-wide（后端） |
| 最大 EPYC 核心数 | 64 核 | 96 核 | 128/192 核 |
| 内存通道 | 8 × DDR4 | 12 × DDR5 | 12 × DDR5 |
| 最大 CCD 数/socket | 8 | 12 | 16 |

### 1.2 ★ Zen 3 的关键变革

```
  Zen 2 (Rome):                      Zen 3 (Milan):
  ┌──────────────────────┐           ┌──────────────────────┐
  │       CCD             │           │       CCD             │
  │  ┌────────┐ ┌──────┐ │           │  ┌──────────────────┐│
  │  │ CCX(4核)│ │ L3   │ │           │  │ CCX (8 核统一)   ││
  │  │ + L3   │ │ 16MB │ │           │  │ + 32MB L3 统一   ││
  │  │ 16MB   │ └──────┘ │           │  └──────────────────┘│
  │  ├────────┤ ┌──────┐ │           └──────────────────────┘
  │  │ CCX(4核)│ │ L3   │ │
  │  │ + L3   │ │ 16MB │ │           ★ Zen 3 将两个 4 核 CCX
  │  │ 16MB   │ └──────┘ │             合并为一个 8 核 CCX
  │  └────────┘ └──────┘ │             L3 也统一为 32MB
  └──────────────────────┘             → 核间通信不再跨 CCX
```

**★ Zen 3 性能飞跃的核心原因：**
- Zen 2：同一 CCD 内的 4+4 核访问彼此的 L3 需要跨 CCX → 额外 ~30 周期延迟
- Zen 3：8 核共享统一的 32MB L3 → 同 CCD 内任意两核通信延迟一致

---

## 2. CCD 与 CCX 拓扑

### 2.1 ★ AMD EPYC 的 Chiplet 架构

```
  AMD EPYC 处理器（以 Zen 3 Milan 为例）:

  ┌─────────────────────────────────────────────────────────┐
  │                    Package (Socket)                       │
  │                                                           │
  │  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐                    │
  │  │CCD 0│  │CCD 1│  │CCD 2│  │CCD 3│   ← 每个 CCD 包含  │
  │  │ 8核 │  │ 8核 │  │ 8核 │  │ 8核 │     1 个 CCX       │
  │  │ 32MB│  │ 32MB│  │ 32MB│  │ 32MB│     (Zen 3/4)      │
  │  └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘                    │
  │     │        │        │        │                         │
  │  ┌──┴──┐  ┌──┴──┐  ┌──┴──┐  ┌──┴──┐                    │
  │  │CCD 4│  │CCD 5│  │CCD 6│  │CCD 7│   8 × CCD = 64 核  │
  │  │ 8核 │  │ 8核 │  │ 8核 │  │ 8核 │                    │
  │  │ 32MB│  │ 32MB│  │ 32MB│  │ 32MB│                    │
  │  └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘                    │
  │     │        │        │        │                         │
  │     └────────┴────┬───┴────────┘                         │
  │                   │                                       │
  │           ┌───────┴───────┐                               │
  │           │    I/O Die    │  ← ★ 集中式 I/O Die (cIOD)   │
  │           │  (Infinity    │    - 内存控制器 (8 通道)       │
  │           │   Fabric)     │    - PCIe 控制器 (128 lanes)  │
  │           │               │    - Infinity Fabric 路由      │
  │           └───────────────┘                               │
  └─────────────────────────────────────────────────────────┘
```

### 2.2 CCD 内部结构（Zen 3/4）

```
  CCD (Core Complex Die) 内部:

  ┌──────────────────────────────────────────┐
  │              CCD = 1 × CCX                │
  │                                           │
  │  ┌───┐ ┌───┐ ┌───┐ ┌───┐               │
  │  │ C0│ │ C1│ │ C2│ │ C3│  ← 8 个核心     │
  │  │L2 │ │L2 │ │L2 │ │L2 │    每核私有 L2  │
  │  └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘               │
  │    │     │     │     │                   │
  │  ┌───┐ ┌───┐ ┌───┐ ┌───┐               │
  │  │ C4│ │ C5│ │ C6│ │ C7│               │
  │  │L2 │ │L2 │ │L2 │ │L2 │               │
  │  └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘               │
  │    │     │     │     │                   │
  │    └─────┴──┬──┴─────┘                   │
  │             │                             │
  │  ┌──────────┴──────────────┐              │
  │  │   L3 Cache (32 MB)      │  ← ★ 统一   │
  │  │   16-way set associative │    8 核共享 │
  │  │   64B cache line         │             │
  │  └──────────┬──────────────┘              │
  │             │                             │
  │  ┌──────────┴──────────────┐              │
  │  │  GMI (Global Memory     │  ← 连接到    │
  │  │  Interface) → I/O Die   │    Infinity  │
  │  └─────────────────────────┘    Fabric    │
  └──────────────────────────────────────────┘
```

### 2.3 ★ Linux 内核如何检测 AMD CCD/CCX 拓扑（源码佐证）

内核在 `init_amd()` 初始化流程中依次调用拓扑检测函数：

```c
/* 源码位置：arch/x86/kernel/cpu/amd.c:974-981 */
/* init_amd() 中的调用链 —— ★ AMD 拓扑检测的入口 */

	cpu_detect_cache_sizes(c);

	amd_detect_cmp(c);       /* ① 通过 APIC ID 位域检测 core/socket */
	amd_get_topology(c);     /* ② CPUID 0x8000001e 解析节点/CCD 拓扑 */
	srat_detect_node(c);     /* ③ SRAT 表关联 NUMA node */
	amd_detect_ppin(c);      /* ④ PPIN（Protected Processor ID） */

	init_amd_cacheinfo(c);   /* ⑤ 缓存层级初始化 */
```

---

**① `amd_detect_cmp()` —— 基于 APIC ID 的基础拓扑检测：**

```c
/* 源码位置：arch/x86/kernel/cpu/amd.c:385-397 */
/*
 * On a AMD dual core setup the lower bits of the APIC id
 * distinguish the cores. Assumes number of cores is a power of two.
 */
static void amd_detect_cmp(struct cpuinfo_x86 *c)
{
	unsigned bits;
	int cpu = smp_processor_id();

	bits = c->x86_coreid_bits;
	/* ★ APIC ID 低位 = core_id（核心在 socket 内的编号） */
	c->cpu_core_id = c->initial_apicid & ((1 << bits) - 1);
	/* ★ APIC ID 高位 = socket ID（物理处理器编号） */
	c->phys_proc_id = c->initial_apicid >> bits;
	/* socket ID 也用作 LLC（末级缓存）标识 */
	per_cpu(cpu_llc_id, cpu) = c->phys_proc_id;
}
/*
 * ★ 原理：APIC ID 的二进制编码包含了拓扑信息
 *   例如 64 核 EPYC（每 CCD 8 核，8 CCD）:
 *   APIC ID = [socket_id | ccd_id | core_in_ccd | smt_id]
 *   x86_coreid_bits 决定了核心编号占用的位数
 */
```

---

**② `amd_get_topology()` —— 通过 CPUID 0x8000001E 解析 CCD/节点拓扑：**

```c
/* 源码位置：arch/x86/kernel/cpu/amd.c:331-379 */
/*
 * Fixup core topology information for
 * (1) AMD multi-node processors
 * (2) AMD processors supporting compute units
 */
static void amd_get_topology(struct cpuinfo_x86 *c)
{
	u8 node_id;
	int cpu = smp_processor_id();

	/* ★ 检查 CPU 是否支持 Topology Extensions（CPUID 0x8000001E） */
	if (boot_cpu_has(X86_FEATURE_TOPOEXT)) {
		int err;
		u32 eax, ebx, ecx, edx;

		cpuid(0x8000001e, &eax, &ebx, &ecx, &edx);

		/* ECX[7:0] = 内部节点 ID（对应 NUMA node） */
		node_id = ecx & 0xff;

		/* Fam15h (Bulldozer 系列): EBX[7:0] = Compute Unit ID */
		if (c->x86 == 0x15)
			c->cu_id = ebx & 0xff;

		/* ★ Fam17h+ (Zen 系列): EBX[7:0] = Core ID */
		if (c->x86 >= 0x17) {
			c->cpu_core_id = ebx & 0xff;
			/* 如果启用了 SMT，需要除以每核线程数 */
			if (smp_num_siblings > 1)
				c->x86_max_cores /= smp_num_siblings;
		}

		/* 尝试使用 CPUID leaf 0xb/0x1f 获取更精细的拓扑层级 */
		err = detect_extended_topology(c);
		if (!err)
			c->x86_coreid_bits =
				get_count_order(c->x86_max_cores);

		/* ★ 设置 LLC ID —— 决定哪些核共享同一 L3 Cache */
		cacheinfo_amd_init_llc_id(c, cpu, node_id);

	/* 旧处理器回退路径：从 MSR 读取节点 ID */
	} else if (cpu_has(c, X86_FEATURE_NODEID_MSR)) {
		u64 value;
		rdmsrl(MSR_FAM10H_NODE_ID, value);
		node_id = value & 7;
		per_cpu(cpu_llc_id, cpu) = node_id;
	}

	/* ★ 多节点处理器标记（NPS > 1 时生效） */
	if (nodes_per_socket > 1) {
		set_cpu_cap(c, X86_FEATURE_AMD_DCM);
		legacy_fixup_core_id(c);
	}
}
```

---

**③ SMT 线程数检测与 TOPOEXT 特性恢复：**

```c
/* 源码位置：arch/x86/kernel/cpu/amd.c:734-749 */
/* early_init_amd() 中的 TOPOEXT 处理 */

	/* 如果 BIOS 关闭了 TopologyExtensions，尝试重新启用 */
	if (c->x86 == 0x15 &&
	    (c->x86_model >= 0x10 && c->x86_model <= 0x6f) &&
	    !cpu_has(c, X86_FEATURE_TOPOEXT)) {
		if (msr_set_bit(0xc0011005, 54) > 0) {
			rdmsrl(0xc0011005, value);
			if (value & BIT_64(54)) {
				set_cpu_cap(c, X86_FEATURE_TOPOEXT);
				pr_info_once("CPU: Re-enabling disabled "
				  "Topology Extensions Support.\n");
			}
		}
	}

	/* ★ CPUID 0x8000001E EBX[15:8] = ThreadsPerCore - 1 */
	/* 这就是内核判断每核有几个 SMT 线程的地方 */
	if (cpu_has(c, X86_FEATURE_TOPOEXT))
		smp_num_siblings =
			((cpuid_ebx(0x8000001e) >> 8) & 0xff) + 1;
```

---

**④ `nodes_per_socket` —— 每 socket 的 NUMA 节点数（NPS 配置的硬件基础）：**

```c
/* 源码位置：arch/x86/kernel/cpu/amd.c:36-41, 569-578 */

/* 全局变量：每 socket 的节点数，默认 1（即 NPS1） */
/*
 * nodes_per_socket: Stores the number of nodes per socket.
 * Refer to Fam15h Models 00-0fh BKDG - CPUID Fn8000_001E_ECX
 * Node Identifiers[10:8]
 */
static u32 nodes_per_socket = 1;

/* early_init_amd() 中解析： */
	if (boot_cpu_has(X86_FEATURE_TOPOEXT)) {
		u32 ecx;
		/* ★ CPUID 0x8000001E ECX[10:8] = NodesPerSocket - 1 */
		ecx = cpuid_ecx(0x8000001e);
		nodes_per_socket = ((ecx >> 8) & 7) + 1;
	} else if (boot_cpu_has(X86_FEATURE_NODEID_MSR)) {
		u64 value;
		/* 旧处理器：从 MSR_FAM10H_NODE_ID 读取 */
		rdmsrl(MSR_FAM10H_NODE_ID, value);
		nodes_per_socket = ((value >> 3) & 7) + 1;
	}
```

---

**⑤ `X86_FEATURE_TOPOEXT` 特性位定义：**

```c
/* 源码位置：arch/x86/include/asm/cpufeatures.h:180 */
#define X86_FEATURE_TOPOEXT  (6*32+22) /* Topology extensions CPUID leafs */
/*
 * ★ 该特性位表示 CPU 支持 CPUID 0x8000001E leaf
 *   这是 AMD 处理器拓扑枚举的"开关"：
 *   - 有此特性 → 可用 CPUID 0x8000001E 获取 core_id、node_id、线程数
 *   - 无此特性 → 回退到 MSR_FAM10H_NODE_ID 等旧方法
 *   AMD Family 15h (Bulldozer) 开始引入，Zen 系列全面支持
 */
```

---

**⑥ `detect_extended_topology()` —— 通用 x86 拓扑层级解析（CPUID leaf 0xb/0x1f）：**

```c
/* 源码位置：arch/x86/kernel/cpu/topology.c:92-155 */
int detect_extended_topology(struct cpuinfo_x86 *c)
{
	unsigned int eax, ebx, ecx, edx, sub_index;
	unsigned int ht_mask_width, core_plus_mask_width,
		     die_plus_mask_width;
	unsigned int core_select_mask, core_level_siblings;
	unsigned int die_select_mask, die_level_siblings;
	int leaf;

	/* ★ 优先使用 leaf 0x1f（V2 拓扑），回退到 leaf 0xb */
	leaf = detect_extended_topology_leaf(c);
	if (leaf < 0)
		return -1;

	/* sub-leaf 0: SMT 层级 */
	cpuid_count(leaf, SMT_LEVEL, &eax, &ebx, &ecx, &edx);
	c->initial_apicid = edx;
	core_level_siblings =
		smp_num_siblings = LEVEL_MAX_SIBLINGS(ebx);
	core_plus_mask_width =
		ht_mask_width = BITS_SHIFT_NEXT_LEVEL(eax);

	/* 遍历后续 sub-leaf，找 Core 和 Die 层级 */
	sub_index = 1;
	do {
		cpuid_count(leaf, sub_index, &eax, &ebx, &ecx, &edx);

		if (LEAFB_SUBTYPE(ecx) == CORE_TYPE) {
			core_level_siblings =
				LEVEL_MAX_SIBLINGS(ebx);
			core_plus_mask_width =
				BITS_SHIFT_NEXT_LEVEL(eax);
		}
		/* ★ leaf 0x1f 新增 DIE_TYPE，区分 die 和 package */
		if (LEAFB_SUBTYPE(ecx) == DIE_TYPE) {
			die_level_siblings =
				LEVEL_MAX_SIBLINGS(ebx);
			die_plus_mask_width =
				BITS_SHIFT_NEXT_LEVEL(eax);
		}
		sub_index++;
	} while (LEAFB_SUBTYPE(ecx) != INVALID_TYPE);

	/* ★ 通过位掩码从 APIC ID 中提取 core_id 和 die_id */
	core_select_mask =
		(~(-1 << core_plus_mask_width)) >> ht_mask_width;
	die_select_mask =
		(~(-1 << die_plus_mask_width)) >> core_plus_mask_width;

	c->cpu_core_id = apic->phys_pkg_id(c->initial_apicid,
				ht_mask_width) & core_select_mask;
	c->cpu_die_id = apic->phys_pkg_id(c->initial_apicid,
				core_plus_mask_width) & die_select_mask;
	c->phys_proc_id = apic->phys_pkg_id(c->initial_apicid,
				die_plus_mask_width);

	/* ★ 最终计算: 每 package 核心数 = 线程总数 / SMT 线程数 */
	c->x86_max_cores =
		(core_level_siblings / smp_num_siblings);
	__max_die_per_package =
		(die_level_siblings / core_level_siblings);

	return 0;
}
/*
 * ★ CPUID leaf 0xb/0x1f 层级类型定义:
 *   SMT_TYPE  = 1   → SMT 线程层级
 *   CORE_TYPE = 2   → 物理核心层级
 *   DIE_TYPE  = 5   → Die 层级（仅 leaf 0x1f 支持）
 *
 *   AMD Zen 处理器的 APIC ID 编码:
 *   ┌──────────┬────────┬───────────┬─────────┐
 *   │ Socket ID│ Die ID │ Core ID   │ SMT ID  │
 *   └──────────┴────────┴───────────┴─────────┘
 *   detect_extended_topology() 就是按层级解析这个编码
 */
```

---

```
  ★ CPUID 0x8000001E 寄存器解码总结（AMD PPR 规范）:

  EAX[31:0]  = Extended APIC ID（扩展 APIC 标识符）
  EBX[7:0]   = CoreId（核心 ID，Zen 系列使用）
  EBX[15:8]  = ThreadsPerCore - 1（每核线程数 - 1）
  ECX[7:0]   = NodeId（内部节点 ID，对应 NUMA node）
  ECX[10:8]  = NodesPerSocket - 1（每 socket 节点数 - 1）

  ★ 完整调用链:
  early_init_amd()
  ├─ 读取 CPUID 0x8000001E → smp_num_siblings（SMT 线程数）
  └─ 读取 CPUID 0x8000001E → nodes_per_socket（NPS 配置）
  init_amd()
  ├─ amd_detect_cmp()        → cpu_core_id, phys_proc_id（APIC ID 位域）
  ├─ amd_get_topology()      → core_id, node_id（CPUID 0x8000001E）
  │   ├─ detect_extended_topology() → die_id, core_id（CPUID 0xb/0x1f）
  │   └─ cacheinfo_amd_init_llc_id() → cpu_llc_id（LLC 共享关系）
  └─ init_amd_cacheinfo()    → 缓存层级枚举（CPUID 0x8000001D）
```

### 2.4 ★ 核心要点：CCD 内的核 vs 跨 CCD 的核

| 通信场景 | 路径 | 近似延迟 | 说明 |
|---------|------|---------|------|
| 同核 SMT 线程 | 共享 L1/L2 | ~0 额外 | 共享同一核心资源 |
| 同 CCD 内核间 | 共享 L3 | ~30-40 周期 | 通过 L3 slice 互连 |
| 跨 CCD（同 socket） | Infinity Fabric | ~80-120 周期 | 经 I/O Die 路由 |
| 跨 socket | Infinity Fabric (xGMI) | ~150-200 周期 | 经外部链路 |

**★ 这就是为什么 8 核共享 L3 重要：** 如果你的 8 个协作线程被调度到同一个 CCD 内，它们通过 L3 通信只需 ~35 周期；如果被分散到不同 CCD，延迟翻倍到 ~100 周期。

---

## 3. ★ 8 核共享 32MB L3 Cache

### 3.1 AMD L3 Cache 的独特结构

```
  AMD Zen 3/4 L3 Cache 结构:

  ┌────────────────────────────────────────────────┐
  │            32 MB L3 Cache                        │
  │                                                  │
  │  物理上分为多个 L3 slice（每核约 4MB）           │
  │  但逻辑上对 CCD 内所有核统一可见                  │
  │                                                  │
  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐           │
  │  │Slice0│ │Slice1│ │Slice2│ │Slice3│  ...       │
  │  │ ~4MB │ │ ~4MB │ │ ~4MB │ │ ~4MB │           │
  │  │(靠近C0)│ │(靠近C1)│ │(靠近C2)│ │(靠近C3)│           │
  │  └──────┘ └──────┘ └──────┘ └──────┘           │
  │                                                  │
  │  ★ 与 Intel 的 Slice-based L3 类似               │
  │    但 AMD 的 slice 都在同一 CCD 内               │
  │    而 Intel 的 slice 分布在整个 die 上            │
  │                                                  │
  │  ★ AMD 的 L3 是"非均匀延迟"的                    │
  │    访问靠近自己核心的 slice: ~30 周期             │
  │    访问远离自己核心的 slice: ~40 周期             │
  └────────────────────────────────────────────────┘
```

### 3.2 ★ 内核如何确定 LLC 共享关系（源码佐证）

内核通过 `cacheinfo_amd_init_llc_id()` 确定哪些核心共享同一 L3 Cache，
这是调度器将线程聚集到同一 CCD 的关键依据：

```c
/* 源码位置：arch/x86/kernel/cpu/cacheinfo.c:649-685 */
void cacheinfo_amd_init_llc_id(struct cpuinfo_x86 *c,
				int cpu, u8 node_id)
{
	/* 检查是否有 L3 Cache（CPUID 0x80000006 EDX != 0） */
	if (!cpuid_edx(0x80000006))
		return;

	if (c->x86 < 0x17) {
		/*
		 * ★ Fam15h/16h（Bulldozer/Jaguar 时代）:
		 *   LLC 在 NUMA node 级别 —— 同一 node 共享 L3
		 */
		per_cpu(cpu_llc_id, cpu) = node_id;

	} else if (c->x86 == 0x17 && c->x86_model <= 0x1F) {
		/*
		 * ★ Fam17h 早期（Zen 1/Zen 2，Naples/Rome）:
		 *   LLC 在 Core Complex 级别
		 *   Core Complex ID = APIC ID 右移 3 位
		 *   → 每 8 个 APIC ID 编号为一组（对应 1 个 CCX/CCD）
		 */
		per_cpu(cpu_llc_id, cpu) = c->apicid >> 3;

	} else {
		/*
		 * ★ Fam19h+（Zen 3/Zen 4，Milan/Genoa）:
		 *   从 CPUID 0x8000001D 读取共享该缓存的线程数
		 *   然后用 APIC ID 右移计算 LLC ID
		 */
		u32 eax, ebx, ecx, edx;
		u32 num_sharing_cache = 0;
		u32 llc_index = find_num_cache_leaves(c) - 1;

		cpuid_count(0x8000001d, llc_index,
			    &eax, &ebx, &ecx, &edx);
		if (eax)
			/* EAX[25:14] = NumSharingCache - 1 */
			num_sharing_cache =
				((eax >> 14) & 0xfff) + 1;

		if (num_sharing_cache) {
			int bits = get_count_order(
					num_sharing_cache);
			/* ★ APIC ID >> bits = LLC ID */
			/* 例如 8 线程共享 → bits=3 → APIC[7:3] */
			per_cpu(cpu_llc_id, cpu) =
				c->apicid >> bits;
		}
	}
}
/*
 * ★ 三代 Zen 的 LLC ID 计算方式演进:
 *   Zen 1/2: 硬编码 apicid >> 3（假设每 CCD 8 线程）
 *   Zen 3+:  从 CPUID 动态获取共享线程数，更通用
 *   → 这样即使未来 CCD 核心数变化（如 Zen 5 的 16 核 CCD），
 *     内核也能正确识别 LLC 边界
 */
```

### 3.3 32MB L3 的实际含义

| 场景 | 32MB L3 是否足够 | 说明 |
|------|-----------------|------|
| 8 核 × 单线程计算密集 | 通常足够 | 工作集 < 32MB 时全部命中 L3 |
| 8 核 × 大内存数据库分片 | 可能不足 | 如果每核工作集 > 4MB，L3 miss 率高 |
| 8 核 × 共享数据结构 | 取决于大小 | 共享的热点数据如果 < 32MB 则 OK |
| 频繁上下文切换 | L3 被污染 | 切换后新任务的 L3 内容无用 → cold miss |

**★ 关键计算：**
```
  假设 8 核各跑 1 个线程，每线程工作集 8MB:
    总工作集 = 8 × 8MB = 64MB
    L3 容量 = 32MB
    → 至少 50% 的数据无法驻留 L3 → 高 L3 miss 率 → 频繁访问 DRAM

  如果将线程绑定到同一 CCD 并限制工作集:
    每线程工作集 3MB:
    总工作集 = 8 × 3MB = 24MB < 32MB
    → 大部分数据驻留 L3 → 低 miss 率
```

### 3.4 AMD L3 Cache Partitioning（L3 Way Partitioning）

AMD EPYC 支持通过 **Platform QoS (PQoS)** 对 L3 进行分区：

```
  L3 Way Partitioning (CAT - Cache Allocation Technology):

  L3 Cache 16-way:
  ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
  │00│01│02│03│04│05│06│07│08│09│10│11│12│13│14│15│
  └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘

  分配方案示例:
  Class 0 (高优先级线程): Way 0-11 (75% L3)
  Class 1 (低优先级线程): Way 0-3  (25% L3)
  ★ 注意: 分区是位掩码，可以有重叠

  使用方式 (Linux resctrl):
  # 挂载 resctrl 文件系统
  mount -t resctrl resctrl /sys/fs/resctrl

  # 创建资源组
  mkdir /sys/fs/resctrl/high_prio

  # 设置 L3 缓存位掩码 (CBM)
  echo "L3:0=fff" > /sys/fs/resctrl/high_prio/schemata
  # fff = 0b111111111111 = Way 0-11

  # 将线程加入资源组
  echo <PID> > /sys/fs/resctrl/high_prio/tasks
```

**★ 使用场景：** 防止低优先级线程（如批处理任务）污染高优先级线程（如在线服务）的 L3 缓存。

---

## 4. NUMA 与 NPS 配置

### 4.1 ★ NPS（Node Per Socket）配置

AMD EPYC 支持多种 NPS 配置，决定了 NUMA 节点的划分方式：

```
  以 EPYC 7763 (Milan, 64 核, 8 CCD) 为例:

  ┌─────────────────────────────────────────────────────────┐
  │  NPS1 (1 NUMA node per socket) — 默认                     │
  │                                                           │
  │  ┌─────────────────────────────────────────────────────┐ │
  │  │                NUMA Node 0 (64 核)                    │ │
  │  │  CCD0 CCD1 CCD2 CCD3 CCD4 CCD5 CCD6 CCD7            │ │
  │  │  ───────────── 统一内存空间 ─────────────            │ │
  │  │  所有 8 通道内存统一编址                               │ │
  │  └─────────────────────────────────────────────────────┘ │
  │  ★ 优点: 编程简单，OS 自动负载均衡                        │
  │  ★ 缺点: 跨 CCD 远端内存访问延迟不透明                    │
  └─────────────────────────────────────────────────────────┘

  ┌─────────────────────────────────────────────────────────┐
  │  NPS2 (2 NUMA nodes per socket)                          │
  │                                                           │
  │  ┌────────────────────┐  ┌────────────────────┐          │
  │  │  NUMA Node 0        │  │  NUMA Node 1        │         │
  │  │  CCD0 CCD1 CCD2 CCD3│  │  CCD4 CCD5 CCD6 CCD7│         │
  │  │  内存通道 0-3       │  │  内存通道 4-7       │         │
  │  └────────────────────┘  └────────────────────┘          │
  │  ★ 优点: 更明确的本地/远端区分                            │
  │  ★ 缺点: 需要 NUMA 感知编程                               │
  └─────────────────────────────────────────────────────────┘

  ┌─────────────────────────────────────────────────────────┐
  │  NPS4 (4 NUMA nodes per socket)                          │
  │                                                           │
  │  ┌──────────┐┌──────────┐┌──────────┐┌──────────┐      │
  │  │Node 0    ││Node 1    ││Node 2    ││Node 3    │      │
  │  │CCD0 CCD1 ││CCD2 CCD3 ││CCD4 CCD5 ││CCD6 CCD7 │      │
  │  │通道 0-1  ││通道 2-3  ││通道 4-5  ││通道 6-7  │      │
  │  └──────────┘└──────────┘└──────────┘└──────────┘      │
  │  ★ 优点: 最强 NUMA 本地性，延迟最可预测                   │
  │  ★ 缺点: 编程复杂，需要精细的 NUMA 绑定                   │
  └─────────────────────────────────────────────────────────┘
```

### 4.2 NPS 配置对性能的影响

| 指标 | NPS1 | NPS2 | NPS4 |
|------|------|------|------|
| NUMA 节点数/socket | 1 | 2 | 4 |
| 每节点核心数 (64核) | 64 | 32 | 16 |
| 每节点内存通道 | 8 | 4 | 2 |
| 每节点内存带宽 | 最高 | 中 | 最低 |
| 远端内存访问延迟 | 不透明 | 明确 | 最明确 |
| OS 调度复杂度 | 低 | 中 | 高 |
| 适用场景 | 通用 | 数据库/HPC | 延迟敏感型 |

**★ 查看当前 NPS 配置：**
```bash
# 查看 NUMA 拓扑
numactl --hardware

# NPS1: 只看到 1 个 node per socket
# NPS2: 看到 2 个 node per socket
# NPS4: 看到 4 个 node per socket

# 查看 CCD 到 NUMA 的映射
lscpu | grep -i "NUMA\|node\|core"
```

### 4.3 ★ NPS 配置建议（性能工程师视角）

```
  决策树:

  你的应用是否 NUMA 感知？
  ├── 否 → 使用 NPS1（默认），让 OS 处理
  └── 是 → 你是否需要精细控制内存延迟？
      ├── 否 → NPS2 是个好的折中
      └── 是 → NPS4 + 手动 NUMA 绑定
                配合 numactl --cpunodebind=N --membind=N
```

---

## 5. AMD vs Intel 关键差异

### 5.1 ★ 架构对比总表

| 维度 | AMD (Zen 3/4 EPYC) | Intel (Xeon Scalable) |
|------|---------------------|----------------------|
| **封装方式** | Chiplet（CCD + I/O Die） | Monolithic（单片 die） |
| **L3 结构** | 每 CCD 32MB 独立 L3 | 统一 LLC（slice-based，所有核共享） |
| **核间互连** | Infinity Fabric | Mesh/Ring Interconnect |
| **内存控制器** | 集中在 I/O Die | 分布在 die 边缘 |
| **L2 大小** | 512KB-1MB/核 | 1.25-2MB/核（Golden Cove+） |
| **ROB 大小** | 256-448 条目 | 224-512 条目 |
| **NUMA 粒度** | NPS 可配置（1/2/4） | 固定（SNC 可选 1/2/4） |
| **SMT** | 2-way SMT | 2-way SMT（E-core 无 SMT） |
| **PMU 事件** | AMD Core PMC + IBS | Intel PEBS + Top-Down metrics |

### 5.2 ★ L3 Cache 结构差异（对性能影响最大）

```
  AMD Zen 3/4:                          Intel Xeon (Ice Lake/Sapphire Rapids):

  ┌─────┐ ┌─────┐ ┌─────┐             ┌─────────────────────────────┐
  │CCD 0│ │CCD 1│ │CCD 2│             │          Unified LLC          │
  │ L3  │ │ L3  │ │ L3  │             │  ┌────┐┌────┐┌────┐┌────┐   │
  │ 32MB│ │ 32MB│ │ 32MB│             │  │Sli ││Sli ││Sli ││Sli │   │
  └──┬──┘ └──┬──┘ └──┬──┘             │  │ce0 ││ce1 ││ce2 ││ceN │   │
     │       │       │                 │  └────┘└────┘└────┘└────┘   │
     │       │       │                 │  所有 slice 构成统一 L3      │
  ───┴───────┴───────┴───              │  总容量 = 1.875MB × 核数     │
    Infinity Fabric                    │  (如 32 核 → 60MB LLC)      │
                                       └─────────────────────────────┘
  ★ AMD: 每 CCD 独立 L3
    同 CCD 内: 低延迟 (~35 cy)          ★ Intel: 全局统一 L3
    跨 CCD: 高延迟 (~100 cy)              所有核等距访问
    不同 CCD 的 L3 不互通                 但 slice 物理分布导致
    → 跨 CCD 必须走 IF → DRAM            近端 slice 快、远端 slice 慢

  ★ 性能影响:
  AMD: 线程必须在同 CCD 才能高效共享 L3
  Intel: 任何核都能访问任何 L3 slice（但延迟不均匀）
```

### 5.3 内存控制器位置差异

```
  AMD:                              Intel:
  ┌─────────────────┐              ┌─────────────────────┐
  │  CCD  CCD  CCD  │              │  MC    Core  Core  MC│
  │    ┌────────┐   │              │  ─┬─    ────  ──── ─┬─│
  │    │ I/O Die│   │              │   │    Mesh/Ring  │  │
  │    │  MC×8  │   │              │  ─┴─    ────  ──── ─┴─│
  │    └────────┘   │              │  MC    Core  Core  MC│
  │  CCD  CCD  CCD  │              └─────────────────────┘
  └─────────────────┘
  ★ MC 集中在 I/O Die               ★ MC 分布在 die 边缘
    所有 CCD 到 MC 距离相等           靠近 MC 的核心访问更快
    → 内存延迟更均匀                  → 内存延迟更不均匀
```

---

## 6. Infinity Fabric 互连

### 6.1 Infinity Fabric 拓扑

```
  Infinity Fabric (IF) 互连结构:

  同 socket 内: GMI (Global Memory Interface)
  ┌──────────┐         ┌──────────┐
  │   CCD    │───GMI──→│  I/O Die │───GMI──→ 其他 CCD
  └──────────┘         │          │
                       │  路由层  │
                       └──────────┘

  跨 socket: xGMI (external GMI)
  ┌──────────────┐    xGMI Link    ┌──────────────┐
  │  Socket 0    │◄═══════════════►│  Socket 1    │
  │  I/O Die     │   (外部链路)    │  I/O Die     │
  └──────────────┘                 └──────────────┘

  ★ 延迟层次:
  同 CCD 内: ~35 周期（直接通过 L3）
  跨 CCD (同 socket): ~80-120 周期（经 GMI → I/O Die → GMI）
  跨 socket: ~150-200 周期（经 xGMI 外部链路）
```

### 6.2 Infinity Fabric 频率

| 参数 | 典型值 | 说明 |
|------|--------|------|
| IF 频率 (fCLK) | 1600-1800 MHz | 与内存频率耦合（MEMCLK = UCLK = FCLK 1:1:1） |
| IF 带宽 (每链路) | ~25-30 GB/s | 双向聚合 |
| 跨 socket 链路数 | 2-4 条 xGMI | EPYC 双路系统 |

**★ "甜点"频率：** IF 频率与内存频率保持 1:1 关系时延迟最低。如果内存频率超过 IF 能支持的最大频率，IF 会降频（1:2 模式），导致延迟增加 ~10ns。

---

## 7. AMD 上的性能分析工具

### 7.1 perf 在 AMD 上的支持

```bash
# AMD 支持的常见 perf 事件
perf stat -e \
  cycles,instructions,\
  cache-misses,cache-references,\
  L1-dcache-load-misses,\
  L2-cache-misses:u,\
  branch-misses \
  -- ./program

# ★ AMD 没有 Intel 的 --topdown 原生支持
# 但可以用 perf stat -d 获取类似信息
perf stat -d -- ./program
```

### 7.2 AMD IBS（Instruction Based Sampling）

```
  IBS 是 AMD 独有的精确采样机制（类似 Intel PEBS 但更强大）:

  IBS 两种模式:
  ┌─────────────────────────────────────────────┐
  │  IBS Fetch (取指采样):                        │
  │  - 采样取指操作                                │
  │  - 记录 I-cache miss、ITLB miss 信息          │
  │  - 识别前端瓶颈                               │
  ├─────────────────────────────────────────────┤
  │  IBS Op (操作采样):                            │
  │  - 采样执行操作                                │
  │  - 记录 D-cache miss、TLB miss、分支结果      │
  │  - 记录 DC miss latency（★ 关键：可以直接      │
  │    看到每次 cache miss 的实际延迟！）           │
  │  - 记录数据访问的物理地址                       │
  └─────────────────────────────────────────────┘

  使用方式:
  # IBS 通过 perf 的 AMD PMU 事件使用
  perf record -e ibs_op// -- ./program
  perf record -e ibs_fetch// -- ./program

  # 带延迟信息
  perf record -e ibs_op// -d -- ./program
  perf report --sort=dso,symbol,dcache_miss
```

### 7.3 AMD 上的替代 topdown 分析

```bash
# 方法 1: perf stat -d（默认指标组）
perf stat -d -- ./program
# 输出包含: cache-misses, cache-references, branch-misses, IPC 等

# 方法 2: 手动构造 topdown-like 指标
perf stat -e \
  cycles,instructions,\
  stalled-cycles-frontend,stalled-cycles-backend,\
  L1-dcache-load-misses,LLC-load-misses,\
  branch-misses \
  -- ./program

# 方法 3: AMD μProf（AMD 官方性能分析工具）
# 下载: https://www.amd.com/en/developer/uprof.html
# 功能类似 Intel VTune，支持 AMD 特定的 topdown 分析
```

---

## 8. 关键术语速查

| 术语 | 全称 | 含义 |
|------|------|------|
| CCD | Core Complex Die | 包含 8 核 + 32MB L3 的 chiplet |
| CCX | Core Complex (Chiplet) | CCD 内的核心复合体（Zen 3/4 中 1 CCD = 1 CCX） |
| IF | Infinity Fabric | AMD 的芯片间互连总线 |
| GMI | Global Memory Interface | CCD 到 I/O Die 的内部 IF 链路 |
| xGMI | External GMI | 跨 socket 的外部 IF 链路 |
| IOD / cIOD | I/O Die (central) | 集中式 I/O 芯片，包含 MC 和路由 |
| NPS | Node Per Socket | BIOS 中配置的 NUMA 节点数/socket |
| IBS | Instruction Based Sampling | AMD 独有的精确采样机制 |
| PQoS | Platform QoS | AMD EPYC 的 L3/MBW 分区功能 |
| CAT | Cache Allocation Technology | L3 Way Partitioning（AMD/Intel 通用术语） |
| MBM | Memory Bandwidth Monitoring | 内存带宽监控 |
| SNC | Sub-NUMA Clustering | Intel 的 NPS 等效功能 |
| V-Cache | 3D V-Cache | AMD 的堆叠 L3 技术（额外 64MB L3） |

---

## 总结

```
  ★ AMD 服务器性能分析的 5 个关键点:

  1. CCD 拓扑意识:
     → 8 核共享 32MB L3，跨 CCD 延迟翻倍
     → 用 taskset 将协作线程绑定到同一 CCD

  2. L3 容量规划:
     → 32MB / 8 核 = 每核约 4MB L3 配额
     → 工作集超过 32MB 时 L3 miss 率飙升

  3. NPS 配置选择:
     → NPS1 通用 | NPS2 折中 | NPS4 延迟敏感
     → 用 numactl --hardware 确认当前配置

  4. 工具链差异:
     → AMD 没有 Intel --topdown，用 IBS + perf stat -d 替代
     → AMD μProf 提供图形化分析

  5. Infinity Fabric 延迟:
     → 内存频率与 IF 频率 1:1:1 是甜点
     → 跨 socket 访问延迟 ~200 周期
```
