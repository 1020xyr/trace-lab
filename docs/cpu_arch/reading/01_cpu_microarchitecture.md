<!--
======================================================================
  Step 1: CPU 微架构基础
======================================================================
  来源: 综合整理（Intel SDM、Agner Fog 微架构手册、Linux 内核文档）
  阅读要点:
    - 流水线 6 阶段的每个阶段做什么
    - 缓存层次 L1/L2/L3/TLB 的容量与延迟
    - 分支预测器的工作原理和失败代价
    - ROB（Reorder Buffer）如何实现乱序执行 + 有序退休
    - SMT（超线程）的资源共享模型
    - 内存序与内存屏障
  预计时间: 40-60 分钟
======================================================================
-->

# CPU 微架构基础

> 理解 CPU 硬件的执行机制是性能分析的根基。
> 本文从流水线、缓存、分支预测、乱序执行、SMT、内存序六个维度展开。

---

## 目录

- [1. 指令流水线](#1-指令流水线)
- [2. 缓存层次](#2-缓存层次)
- [3. 分支预测器](#3-分支预测器)
- [4. 乱序执行与 ROB](#4-乱序执行与-rob)
- [5. 超线程 SMT](#5-超线程-smt)
- [6. 内存序与内存屏障](#6-内存序与内存屏障)
- [7. 关键术语速查表](#7-关键术语速查表)

---

## 1. 指令流水线

### 1.1 ★ 六阶段流水线模型

现代 x86 处理器将指令执行分解为多个阶段，使多条指令同时在流水线的不同阶段执行：

```
  时钟周期:  T1    T2    T3    T4    T5    T6    T7    T8

  指令 A:  [Fetch][Decod][Renam][Sched][Execu][Retire]
  指令 B:        [Fetch][Decod][Renam][Sched][Execu][Retire]
  指令 C:              [Fetch][Decod][Renam][Sched][Execu][Retire]
  指令 D:                    [Fetch][Decod][Renam][Sched][Execu]
```

```
  ┌──────────┐
  │  Fetch   │  ← 从 L1 I-Cache 读取指令字节（通常 16 字节/周期）
  │  取指    │
  └────┬─────┘
       │
       ▼
  ┌──────────┐
  │  Decode  │  ← x86 变长指令 → μops（微操作）
  │  解码    │     简单指令: 1 μop (如 ADD reg, reg)
  └────┬─────┘     复杂指令: 2-4 μops (如 PUSH, CALL)
       │           超复杂指令: 微码序列 (MSROM, >4 μops)
       ▼
  ┌──────────┐
  │  Rename  │  ← ★ 寄存器重命名：消除假数据依赖
  │  重命名  │     架构寄存器（如 RAX）→ 物理寄存器（PRF 中 ~180 个）
  └────┬─────┘     消除 WAW (Write After Write) 和 WAR (Write After Read)
       │
       ▼
  ┌──────────┐
  │ Schedule │  ← 放入 Reservation Station (RS)
  │  调度    │     等待所有源操作数就绪
  └────┬─────┘     就绪后派遣（dispatch）到功能单元
       │
       ▼
  ┌──────────┐
  │ Execute  │  ← 在功能单元（FU）中执行运算
  │  执行    │     ALU: 整数运算（1 周期）
  └────┬─────┘     FPU: 浮点运算（3-20 周期）
       │           Load: 内存读取（4-200+ 周期，取决于缓存命中）
       │           Store: 内存写入（存储到 Store Buffer）
       │           Branch: 分支计算
       ▼
  ┌──────────┐
  │  Retire  │  ← ★ 按程序顺序提交结果到架构状态
  │  退休    │     ROB 保证即使乱序执行，退休仍按序
  └──────────┘     退休带宽: 通常 4-8 μops/周期
```

### 1.2 流水线关键概念

> **内核源码佐证：** 内核通过 CPUID 指令探测当前 CPU 支持的指令集特性，
> 这些特性直接影响流水线各阶段的行为。以下定义来自内核头文件：

```c
/* 源码位置：src/linux-5.10/arch/x86/include/asm/cpufeatures.h
 *
 * ★ 内核通过 CPUID 指令枚举 CPU 支持的硬件特性，
 *   每个特性用一个 bit 位表示，存储在 cpuinfo_x86.x86_capability[] 数组中。
 *   这些特性决定了流水线 Decode/Execute 阶段能处理哪些指令类型。
 */

/* === Decode 阶段相关：指令集扩展 === */
#define X86_FEATURE_CMOV        ( 0*32+15) /* CMOV 条件移动指令 → 消除分支，减少预测失败 */
#define X86_FEATURE_MMX         ( 0*32+23) /* MMX SIMD 指令集 */
#define X86_FEATURE_XMM         ( 0*32+25) /* "sse" SSE 指令集 */
#define X86_FEATURE_XMM2        ( 0*32+26) /* "sse2" SSE2 指令集 */
#define X86_FEATURE_AVX         ( 4*32+28) /* Advanced Vector Extensions → 256-bit 向量 */
#define X86_FEATURE_FMA         ( 4*32+12) /* Fused multiply-add → 融合乘加，单 μop 完成 */
#define X86_FEATURE_AES         ( 4*32+25) /* AES-NI 硬件加速指令 */

/* === Execute 阶段相关：执行能力 === */
#define X86_FEATURE_FPU         ( 0*32+ 0) /* 片上 FPU 浮点运算单元 */
#define X86_FEATURE_CLFLUSH     ( 0*32+19) /* CLFLUSH 缓存行刷新指令 */
#define X86_FEATURE_TSC         ( 0*32+ 4) /* Time Stamp Counter 时间戳计数器 */
#define X86_FEATURE_MSR         ( 0*32+ 5) /* Model-Specific Registers 模型特定寄存器 */

/* === 性能监控相关（perf 工具依赖） === */
#define X86_FEATURE_ARCH_PERFMON ( 3*32+11) /* Intel Architectural PerfMon 架构性能监控 */
#define X86_FEATURE_PEBS        ( 3*32+12) /* Precise-Event Based Sampling 精确事件采样 */
#define X86_FEATURE_BTS         ( 3*32+13) /* Branch Trace Store 分支追踪存储 */
```

| 概念 | 英文 | 说明 |
|------|------|------|
| μop | Micro-Operation | 流水线实际处理的最小执行单元，x86 指令在 Decode 阶段被拆分 |
| 宏融合 | Macro-Fusion | 将多条相关的简单 x86 指令合并为 1 个 μop（如 CMP + Jcc） |
| 微融合 | Micro-Fusion | 将 Load + ALU 操作融合为 1 个 μop（如 ADD [mem], reg） |
| MSROM | Microcode Sequencer ROM | 存储复杂指令的微码序列，如 DIV、SQRT |
| 流水线冲刷 | Pipeline Flush | 分支预测错误时清空流水线，代价 ~15-20 周期 |

### 1.3 Decode 阶段深入

```
  x86 指令长度: 1-15 字节（变长）

  Decode 带宽:
  ┌─────────────────────────────────────────────┐
  │  Intel Skylake+:                             │
  │  - 4-wide decode (4 条指令/周期)             │
  │  - 其中最多 1 条复杂指令走 MSROM             │
  │  - 简单指令: 1 μop/条                        │
  │                                               │
  │  μop Cache (DSB - Decoded Stream Buffer):     │
  │  - 缓存已解码的 μops（~2048 条目）           │
  │  - 命中时绕过 Decode，直接 6 μops/周期       │
  │  - 对热点代码性能至关重要                      │
  └─────────────────────────────────────────────┘
```

---

## 2. 缓存层次

### 2.1 ★ 缓存层次结构总览

```
  ┌────────────────────────────────────────────────────┐
  │                    CPU Core                          │
  │                                                      │
  │  ┌───────────┐        ┌───────────┐                  │
  │  │ L1 I-Cache│        │ L1 D-Cache│  ← ★ 最快       │
  │  │  32 KB    │        │  32 KB    │                  │
  │  │  8-way    │        │  8-way    │  延迟: 4-5 周期  │
  │  │  64B line │        │  64B line │  带宽: 2 load +  │
  │  └───────────┘        └───────────┘  1 store/周期    │
  │        │                    │                         │
  │        └────────┬───────────┘                         │
  │                 │                                     │
  │                 ▼                                     │
  │  ┌───────────────────────────┐                        │
  │  │      L2 Cache (统一)       │                        │
  │  │      256 KB - 1 MB        │  延迟: 10-14 周期      │
  │  │      4-16 way             │  带宽: 2 load/周期     │
  │  │      64B line             │                        │
  │  └─────────────┬─────────────┘                        │
  └────────────────┼──────────────────────────────────────┘
                   │
                   ▼
  ┌────────────────────────────────────┐
  │       L3 Cache (LLC, 最后一级缓存)  │
  │       数 MB - 数十 MB (共享)        │  延迟: 30-50 周期
  │       12-20 way                    │  Slice-based 架构
  │       64B line                     │  (每核一个 slice)
  └──────────────┬─────────────────────┘
                 │
                 ▼
  ┌────────────────────────────────────┐
  │           主存 (DRAM)               │  延迟: 100-300 周期
  │           通道 × 2/4/8              │  带宽: ~50-100 GB/s
  └────────────────────────────────────┘
```

### 2.1.1 内核如何探测缓存大小

> 内核在 CPU 初始化时通过 CPUID 指令探测各级缓存和 TLB 的大小：

```c
/* 源码位置：src/linux-5.10/arch/x86/kernel/cpu/common.c:690
 *
 * ★ cpu_detect_cache_sizes() — 通过 CPUID 叶节点探测缓存容量
 *   在 identify_cpu() 流程中被调用，为每个 CPU 填充 x86_cache_size 字段。
 */
void cpu_detect_cache_sizes(struct cpuinfo_x86 *c)
{
    unsigned int n, dummy, ebx, ecx, edx, l2size;

    n = c->extended_cpuid_level;

    /* CPUID 0x80000005: L1 缓存信息（AMD 定义） */
    if (n >= 0x80000005) {
        cpuid(0x80000005, &dummy, &ebx, &ecx, &edx);
        c->x86_cache_size = (ecx>>24) + (edx>>24);  /* ★ L1 D-Cache + L1 I-Cache (KB) */
    }

    if (n < 0x80000006)     /* 没有 L2 缓存？直接返回 */
        return;

    /* CPUID 0x80000006: L2/L3 缓存信息 */
    cpuid(0x80000006, &dummy, &ebx, &ecx, &edx);
    l2size = ecx >> 16;                              /* ★ L2 缓存大小 (KB) */

    c->x86_tlbsize += ((ebx >> 16) & 0xfff) + (ebx & 0xfff); /* ★ L2 TLB 条目数 */

    c->x86_cache_size = l2size;                      /* 最终写入 x86_cache_size */
}
```

```c
/* 源码位置：src/linux-5.10/arch/x86/kernel/cpu/common.c:737
 *
 * ★ cpu_detect_tlb() — 输出最后一级 TLB 的条目数
 *   这就是 dmesg 中 "Last level iTLB/dTLB entries" 的来源。
 */
static void cpu_detect_tlb(struct cpuinfo_x86 *c)
{
    if (this_cpu->c_detect_tlb)
        this_cpu->c_detect_tlb(c);   /* 调用厂商特定探测函数（Intel/AMD） */

    /* ★ 输出示例：
     * "Last level iTLB entries: 4KB 64, 2MB 8, 4MB 4"
     * "Last level dTLB entries: 4KB 72, 2MB 32, 4MB 16, 1GB 4"
     */
    pr_info("Last level iTLB entries: 4KB %d, 2MB %d, 4MB %d\n",
        tlb_lli_4k[ENTRIES], tlb_lli_2m[ENTRIES], tlb_lli_4m[ENTRIES]);

    pr_info("Last level dTLB entries: 4KB %d, 2MB %d, 4MB %d, 1GB %d\n",
        tlb_lld_4k[ENTRIES], tlb_lld_2m[ENTRIES],
        tlb_lld_4m[ENTRIES], tlb_lld_1g[ENTRIES]);
}
```

### 2.2 各级缓存详细参数

| 缓存层级 | 典型大小 | 关联度 | 行大小 | 访问延迟 | 每周期带宽 | 私有/共享 |
|----------|---------|--------|--------|---------|-----------|----------|
| L1 I-Cache | 32 KB | 8-way | 64 B | 4 周期 | 16 B 取指 | 每核私有 |
| L1 D-Cache | 32-48 KB | 8-way | 64 B | 4-5 周期 | 2×32B load + 1 store | 每核私有 |
| L2 Cache | 256K-1M | 4-16 way | 64 B | 10-14 周期 | 2×32B load | 每核私有 |
| L3 Cache | 1.5-4 MB/核 | 12-20 way | 64 B | 30-50 周期 | 共享带宽 | 所有核共享 |
| 主存 | GB-TB | — | 64 B | 100-300 周期 | 50-100 GB/s | 所有核共享 |

### 2.3 ★ TLB（地址转换缓存）

TLB 缓存虚拟地址 → 物理地址的映射，避免每次内存访问都执行 Page Walk：

```
  虚拟地址 (VA)
      │
      ├──→ L1 D-TLB (64-72 条目, 4-way)
      │    命中 → 1 周期得到物理页号
      │    缺失 ↓
      │
      ├──→ L2 TLB (512-1536 条目, 4-8 way)
      │    命中 → ~10 周期
      │    缺失 ↓
      │
      └──→ Page Walk (遍历页表, 4 级)
           PML4 → PDPT → PD → PT
           每级可能命中缓存 → 最坏 4 次内存访问
           代价: ~100+ 周期

  ★ 大页 (Huge Page) 优化:
  - 2MB 大页: 减少 1 级页表查找
  - 1GB 大页: 减少 2 级页表查找
  - 相同 TLB 条目覆盖更大地址空间
```

| TLB 层级 | 条目数 | 页大小 | 覆盖范围 | 命中延迟 |
|----------|--------|--------|---------|---------|
| L1 D-TLB | 64 | 4KB | 256 KB | 1 周期 |
| L1 D-TLB | 32 | 2MB | 64 MB | 1 周期 |
| L1 I-TLB | 128 | 4KB | 512 KB | 1 周期 |
| L2 TLB | 1536 | 4KB | 6 MB | ~10 周期 |

### 2.4 缓存关联与替换策略

```
  8-way 组相联缓存:

  地址 [63:11] → Tag        地址 [10:6] → Set Index    地址 [5:0] → Offset
  ┌─────────────────┐      ┌────────────┐              ┌──────────┐
  │    Tag (53 bit)  │      │ Index (5b) │              │ Off (6b) │
  └─────────────────┘      └────────────┘              └──────────┘

  Set 由 Index 选定 → 在 8 个 Way 中比较 Tag → 命中/缺失

  替换策略:
  - LRU (Least Recently Used): L1/L2 常用
  - Pseudo-LRU (PLRU): 近似 LRU，硬件开销更低
  - DRIP (Dynamic Re-reference Interval Prediction): L3 常用
```

### 2.5 缓存一致性协议（多核场景）

```
  MESI 协议状态:

  M (Modified)  → 本核心独占修改，与内存不一致
  E (Exclusive) → 本核心独占，与内存一致
  S (Shared)    → 多个核心共享只读，与内存一致
  I (Invalid)   → 无效，不在缓存中

  状态转换:
  ┌───┐ ReadMiss  ┌───┐  Write     ┌───┐
  │ I │ ────────→ │ E │ ────────→ │ M │
  └───┘           └───┘           └───┘
    ↑               ↓ Read(其他核)   │ Write(其他核)
    │               ▼               ▼
    │             ┌───┐           ┌───┐
    └──────────── │ S │ ←──────── │ I │  (Invalidate)
                  └───┘           └───┘

  ★ 对性能的影响:
  - False Sharing: 两个核心频繁写同一缓存行的不同字段 → 反复 invalidate
  - Cache Line Bouncing: 缓存行在核心间反复迁移 → 严重性能下降
  - 解决: 对齐关键数据到 64B 缓存行边界，使用 padding
```

---

## 3. 分支预测器

### 3.1 ★ 为什么需要分支预测

```
  流水线深度 ~14 级 → 分支结果计算出来前，后续指令已经取了 14 条

  如果不预测:
  ┌────────┐
  │ Branch │ → 等待结果 → 14 周期气泡 (bubble) → 后续指令
  └────────┘

  如果预测:
  ┌────────┐
  │ Branch │ → 预测 Taken → 继续取后续指令 → 如果预测正确: 0 周期损失
  └────────┘                                  如果预测错误: ~15-20 周期冲刷
```

### 3.2 分支预测器架构

```
  现代 CPU 分支预测器（以 Intel Skylake 为例）:

  ┌──────────────────────────────────────────────────┐
  │              Branch Predictor Unit                  │
  │                                                      │
  │  ┌──────────┐  ┌────────────┐  ┌───────────────┐  │
  │  │ Bimodal  │  │ Two-Level  │  │  TAGE-like    │  │
  │  │ 2-bit    │  │ 全局历史 + │  │ 多长度历史表  │  │
  │  │ 计数器   │  │ PHT        │  │ (Tagged       │  │
  │  │          │  │            │  │  Geometric)   │  │
  │  └──────────┘  └────────────┘  └───────────────┘  │
  │                                                      │
  │  ┌──────────┐  ┌────────────┐                      │
  │  │   BTB    │  │    RAS     │                      │
  │  │ Branch   │  │ Return     │                      │
  │  │ Target   │  │ Address    │                      │
  │  │ Buffer   │  │ Stack      │                      │
  │  │ (目标地址│  │ (返回地址  │                      │
  │  │  缓存)   │  │  预测栈)   │                      │
  │  └──────────┘  └────────────┘                      │
  └──────────────────────────────────────────────────┘
```

| 组件 | 功能 | 容量 |
|------|------|------|
| Bimodal | 基于 PC 地址的 2-bit 饱和计数器 | ~数千条目 |
| Two-Level | 全局/局部历史 → Pattern History Table | 历史长度 10-30 bit |
| TAGE | ★ 多长度历史表，用 Tag 匹配，几何级数递增的历史长度 | 核心预测器 |
| BTB | 缓存分支目标地址，避免重复计算 | ~数千条目 |
| RAS | 专门预测 CALL/RET 配对 | 16-32 深度 |

### 3.3 分支预测失败的影响

```
  分支预测失败 (Branch Misprediction):

  正确路径:  [A] [B] [C] [D] [E]
  错误预测:  [A] [X] [Y] [Z] [W] ← 这些 μops 全部丢弃
                                  冲刷流水线
                                  重新从正确路径取指

  代价: ~15-20 周期 (Skylake: ~15 周期)

  ★ 优化建议:
  1. 编译器: __builtin_expect (likely/unlikely) 提示编译器
  2. PGO (Profile-Guided Optimization): 用运行时数据优化布局
  3. 减少不可预测分支: 用条件移动 (CMOV) 替代简单 if-else
  4. 排序数据: 使分支更可预测（如排序数组的二分查找）
  5. perf 分析: perf stat -e branch-misses 定位热点
```

### 3.4 间接分支预测

```
  间接跳转 (如 switch 语句、虚函数调用、函数指针):

  jmp [rax]  ← 目标地址不确定，需要 BTB 预测

  Spectre-v2 漏洞:
  - 攻击者训练间接分支预测器指向恶意代码
  - 即使权限检查失败，推测执行的副作用（缓存状态）可被侧信道读取
  - 缓解措施:
    - Retpoline: 用 RET 指令替代间接跳转
    - IBRS (Indirect Branch Restricted Speculation)
    - eIBRS (Enhanced IBRS, 硬件级)
```

### 3.4.1 内核中的 Spectre-v2 缓解措施

> 内核在启动时根据 CPU 硬件能力选择分支预测漏洞的缓解策略：

```c
/* 源码位置：src/linux-5.10/arch/x86/include/asm/msr-index.h:48
 *
 * ★ SPEC_CTRL MSR（模型特定寄存器）— 控制 CPU 推测执行行为
 *   内核通过写此寄存器来启用/禁用各种推测执行缓解措施。
 */
#define MSR_IA32_SPEC_CTRL      0x00000048  /* Speculation Control */
#define SPEC_CTRL_IBRS          BIT(0)      /* ★ Indirect Branch Restricted Speculation
                                             *    限制间接分支的推测执行 */
#define SPEC_CTRL_STIBP         BIT(1)      /* ★ Single Thread Indirect Branch Predictor
                                             *    防止一个线程训练另一个线程的分支预测器 */
#define SPEC_CTRL_SSBD          BIT(2)      /* ★ Speculative Store Bypass Disable
                                             *    禁止推测性存储绕过 */

#define MSR_IA32_PRED_CMD       0x00000049  /* Prediction Command */
#define PRED_CMD_IBPB           BIT(0)      /* ★ Indirect Branch Prediction Barrier
                                             *    刷新间接分支预测器状态 */
```

```c
/* 源码位置：src/linux-5.10/arch/x86/include/asm/msr-index.h:91
 *
 * ★ ARCH_CAPABILITIES MSR — 硬件级漏洞免疫标志
 *   某些较新的 CPU 在硬件层面修复了漏洞，无需软件缓解。
 */
#define MSR_IA32_ARCH_CAPABILITIES    0x0000010a
#define ARCH_CAP_RDCL_NO              BIT(0)  /* 不受 Meltdown 影响 */
#define ARCH_CAP_IBRS_ALL             BIT(1)  /* ★ 增强型 IBRS（eIBRS），硬件级防护 */
#define ARCH_CAP_SSB_NO               BIT(4)  /* 不受 Speculative Store Bypass 影响 */
#define ARCH_CAP_MDS_NO               BIT(5)  /* 不受 MDS 微架构数据采样漏洞影响 */
```

```c
/* 源码位置：src/linux-5.10/arch/x86/kernel/cpu/common.c:857
 *
 * ★ init_speculation_control() — 初始化推测执行控制特性
 *   在 get_cpu_cap() 末尾调用，将厂商特定的特性标志统一映射到通用标志。
 */
static void init_speculation_control(struct cpuinfo_x86 *c)
{
    /* Intel CPU: SPEC_CTRL 特性位 → 同时意味着支持 IBRS 和 IBPB */
    if (cpu_has(c, X86_FEATURE_SPEC_CTRL)) {
        set_cpu_cap(c, X86_FEATURE_IBRS);          /* 间接分支受限推测 */
        set_cpu_cap(c, X86_FEATURE_IBPB);          /* 间接分支预测屏障 */
        set_cpu_cap(c, X86_FEATURE_MSR_SPEC_CTRL); /* MSR_SPEC_CTRL 可用 */
    }

    /* Intel STIBP（单线程间接分支预测器） */
    if (cpu_has(c, X86_FEATURE_INTEL_STIBP))
        set_cpu_cap(c, X86_FEATURE_STIBP);

    /* AMD CPU: 分别枚举各特性 */
    if (cpu_has(c, X86_FEATURE_AMD_IBRS)) {
        set_cpu_cap(c, X86_FEATURE_IBRS);
        set_cpu_cap(c, X86_FEATURE_MSR_SPEC_CTRL);
    }
    if (cpu_has(c, X86_FEATURE_AMD_IBPB))
        set_cpu_cap(c, X86_FEATURE_IBPB);
    /* ... AMD STIBP, SSBD 类似处理 ... */
}
```

```c
/* 源码位置：src/linux-5.10/arch/x86/kernel/cpu/bugs.c:79
 *
 * ★ check_bugs() — 内核启动时检测并缓解 CPU 硬件漏洞
 *   这是 Spectre/Meltdown/MDS 等所有缓解措施的入口函数。
 */
void __init check_bugs(void)
{
    identify_boot_cpu();
    cpu_smt_check_topology();   /* 初始化 SMT 拓扑信息 */

    /* ★ 读取当前 SPEC_CTRL MSR 值（保留未知位） */
    if (boot_cpu_has(X86_FEATURE_MSR_SPEC_CTRL))
        rdmsrl(MSR_IA32_SPEC_CTRL, x86_spec_ctrl_base);

    /* 如果 CPU 支持 STIBP，允许在掩码中设置该位 */
    if (boot_cpu_has(X86_FEATURE_STIBP))
        x86_spec_ctrl_mask |= SPEC_CTRL_STIBP;

    /* ★ 依次选择各漏洞的缓解策略 */
    spectre_v1_select_mitigation();   /* Spectre V1: 数组越界 */
    spectre_v2_select_mitigation();   /* Spectre V2: 间接分支注入 */
    ssb_select_mitigation();          /* Speculative Store Bypass */
    l1tf_select_mitigation();         /* L1 Terminal Fault */
    mds_select_mitigation();          /* Microarch Data Sampling */
    taa_select_mitigation();          /* TSX Async Abort */
    srbds_select_mitigation();        /* Special Register Buffer Data Sampling */

    arch_smt_update();                /* 根据 SMT 状态更新缓解措施 */
}
```

---

### 4.1 ★ 为什么需要乱序执行

```
  顺序执行的浪费:

  指令 A: LOAD [mem]    ← 需要 200 周期 (L3 miss)
  指令 B: ADD r1, r2    ← 与 A 无关，只需 1 周期
  指令 C: MUL r3, r4    ← 与 A 无关，只需 3 周期

  顺序执行: A(200) + B(1) + C(3) = 204 周期
  乱序执行: A(200), 同时 B 和 C 在等待期间执行 = ~200 周期

  ★ 核心思想: 当一条指令因等待数据而阻塞时，
              允许后续不依赖该数据的指令先执行
```

### 4.2 乱序执行引擎核心组件

```
  ┌─────────────────────────────────────────────────────────────┐
  │                   乱序执行引擎                                 │
  │                                                               │
  │  ┌─────────┐    ┌──────────────────────────────────────┐    │
  │  │  Rename  │───→│  ROB (Reorder Buffer)                 │    │
  │  │  重命名  │    │  ★ 乱序执行的核心                     │    │
  │  └─────────┘    │                                        │    │
  │                  │  - 按程序序分配条目                     │    │
  │                  │  - 记录每条 μop 的状态                  │    │
  │                  │  - μop 完成但前序未完成 → 等待          │    │
  │                  │  - 前序全部完成 → 退休（提交到架构状态） │    │
  │                  │                                        │    │
  │                  │  容量: ~224 条目 (Skylake)              │    │
  │                  │        ~352 条目 (Ice Lake)             │    │
  │                  │        ~512 条目 (Zen 3)                │    │
  │                  └──────────────────┬───────────────────┘    │
  │                                     │ 派遣                    │
  │                                     ▼                         │
  │                  ┌──────────────────────────────────────┐    │
  │                  │  Reservation Station (RS)              │    │
  │                  │  - 保存等待操作数就绪的 μops            │    │
  │                  │  - 操作数就绪 → 派遣到功能单元          │    │
  │                  │  - 容量: ~97 条目 (Skylake)             │    │
  │                  └──────────────────┬───────────────────┘    │
  │                                     │ 执行                    │
  │                                     ▼                         │
  │                  ┌──────────────────────────────────────┐    │
  │                  │  功能单元 (Execution Units)             │    │
  │                  │  Port 0: ALU, FPU, Branch             │    │
  │                  │  Port 1: ALU, FPU, AES                │    │
  │                  │  Port 2: Load + AGU                   │    │
  │                  │  Port 3: Store AGU                    │    │
  │                  │  Port 4: Store Data                   │    │
  │                  │  Port 5: ALU, Shuffle                 │    │
  │                  │  Port 6: ALU, Branch                  │    │
  │                  │  Port 7: Store AGU                    │    │
  │                  └──────────────────┬───────────────────┘    │
  │                                     │ 完成                    │
  │                                     ▼                         │
  │                  ROB 收到完成通知 → 按序退休                    │
  └─────────────────────────────────────────────────────────────┘
```

### 4.3 ROB 退休机制

```
  ROB 条目状态:

  ┌─────┬──────────┬──────────┬──────────┐
  │ 序号 │  μop A   │  μop B   │  μop C   │
  │  1   │ 完成 ✓   │ 执行中   │ 完成 ✓   │
  │  2   │ (已退休) │          │          │
  └─────┴──────────┴──────────┴──────────┘

  退休规则:
  1. 只能从 ROB 头部（最老的 μop）开始退休
  2. μop A 已完成 → 可以退休
  3. μop B 未完成 → 阻塞，μop C 即使完成也不能退休
  4. μop B 完成 → B 和 C 可以连续退休

  ★ 关键概念:
  - 乱序执行 + 有序退休 = 保证程序语义正确
  - ROB 容量 = 乱序窗口大小 = 可隐藏延迟的上限
  - ROB 满 → 前端停止取指（stall）
```

### 4.4 数据依赖与调度

> **内核源码佐证：** 内核通过 `get_cpu_cap()` 读取 CPUID 获取 CPU 的全部能力信息，
> 这些信息直接反映了乱序执行引擎能利用的指令级并行度（ILP）：

```c
/* 源码位置：src/linux-5.10/arch/x86/kernel/cpu/common.c:898
 *
 * ★ get_cpu_cap() — 通过 CPUID 指令枚举 CPU 特性
 *   这是内核了解 CPU 乱序执行能力（支持哪些指令、有多少功能单元）的入口。
 *   每个 CPUID 叶节点对应不同的特性类别。
 */
void get_cpu_cap(struct cpuinfo_x86 *c)
{
    u32 eax, ebx, ecx, edx;

    /* CPUID 0x01: 基础特性（FPU, SSE, SSE2, HT, ...） */
    if (c->cpuid_level >= 0x00000001) {
        cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
        c->x86_capability[CPUID_1_ECX] = ecx;  /* ★ SSE3, SSSE3, SSE4, AVX, AES... */
        c->x86_capability[CPUID_1_EDX] = edx;  /* ★ FPU, TSC, CMOV, MMX, HT... */
    }

    /* CPUID 0x07: 扩展特性（AVX2, AVX-512, BMI, ...） */
    if (c->cpuid_level >= 0x00000007) {
        cpuid_count(0x00000007, 0, &eax, &ebx, &ecx, &edx);
        c->x86_capability[CPUID_7_0_EBX] = ebx; /* ★ AVX2, BMI1/2, MPX... */
        c->x86_capability[CPUID_7_ECX] = ecx;
        c->x86_capability[CPUID_7_EDX] = edx;
    }

    /* AMD 扩展特性（0x80000001）: SYSCALL, NX, 大页... */
    if (eax >= 0x80000001) {
        cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
        c->x86_capability[CPUID_8000_0001_ECX] = ecx;
        c->x86_capability[CPUID_8000_0001_EDX] = edx; /* ★ SYSCALL, NX, LM(64-bit) */
    }

    /* ★ 推测执行控制特性（Spectre 缓解相关） */
    init_speculation_control(c);

    apply_forced_caps(c);  /* 应用内核命令行强制/禁用的特性 */
}
```

| 依赖类型 | 示例 | 能否乱序 | 处理方式 |
|---------|------|---------|---------|
| RAW (Read After Write) | A: R1=X, B: Y=R1 | 不能 | 真依赖，B 必须等 A 完成 |
| WAR (Write After Read) | A: Y=R1, B: R1=X | 可以 | 假依赖，寄存器重命名消除 |
| WAW (Write After Write) | A: R1=X, B: R1=Y | 可以 | 假依赖，寄存器重命名消除 |

```
  寄存器重命名消除假依赖:

  架构状态: RAX, RBX, RCX, ... (16 个通用寄存器)
  物理寄存器: PR0, PR1, PR2, ... (PRF, ~180 个物理寄存器)

  重命名表 (RAT - Register Alias Table):
  RAX → PR5     ← 当前 RAX 映射到物理寄存器 PR5
  RBX → PR12
  RCX → PR3

  指令 A: ADD RAX, 1   →  ADD PR5, 1     (RAT: RAX→PR5)
  指令 B: MOV RAX, RCX →  MOV PR7, PR3   (RAT: RAX→PR7, 新分配!)

  ★ 指令 A 和 B 对 RAX 的 WAW 依赖被消除:
     A 写 PR5, B 写 PR7, 互不干扰, 可以乱序执行
```

---

## 5. 超线程 SMT

### 5.1 ★ SMT（Simultaneous Multi-Threading）原理

```
  物理核心（1 个 Core）:

  ┌──────────────────────────────────────────────────┐
  │                 共享资源                            │
  │  ┌────────────┐  ┌──────────────────┐             │
  │  │ L1 I-Cache │  │ L1 D-Cache       │             │
  │  │ (共享)     │  │ (共享)           │             │
  │  └────────────┘  └──────────────────┘             │
  │  ┌────────────┐  ┌──────────────────┐             │
  │  │ L2 Cache   │  │ 执行单元 (ALU×4) │             │
  │  │ (共享)     │  │ (共享, 竞争)     │             │
  │  └────────────┘  └──────────────────┘             │
  │  ┌────────────┐  ┌──────────────────┐             │
  │  │ ROB        │  │ Reservation Sta. │             │
  │  │ (分区共享) │  │ (分区共享)       │             │
  │  └────────────┘  └──────────────────┘             │
  │                                                      │
  │  ┌──────────────────────────────────────────────┐  │
  │  │              私有资源（每线程独立）              │  │
  │  │  Thread 0:              Thread 1:             │  │
  │  │  - 架构寄存器 (RAX...)  - 架构寄存器           │  │
  │  │  - 指令指针 (RIP)       - 指令指针             │  │
  │  │  - 标志位 (RFLAGS)      - 标志位               │  │
  │  │  - MSRs (部分)          - MSRs (部分)          │  │
  │  │  - 栈指针 (RSP)         - 栈指针               │  │
  │  └──────────────────────────────────────────────┘  │
  └──────────────────────────────────────────────────┘
```

### 5.2 SMT 资源竞争与性能影响

| 共享资源 | 竞争后果 | 性能影响 |
|---------|---------|---------|
| L1 D-Cache | 两个线程争缓存行 | 缓存命中率下降 |
| 执行单元 | 两个线程争 ALU 端口 | 指令吞吐量下降 |
| ROB | ROB 空间被分摊 | 乱序窗口缩小，隐藏延迟能力下降 |
| TLB | TLB 条目被分摊 | TLB 缺失率上升 |
| Store Buffer | 存储操作竞争 | 写操作延迟增加 |

```
  ★ SMT 性能经验法则:

  理想情况:  2 线程 SMT → ~30% 吞吐量提升
             (两个线程的指令流填补对方的流水线空泡)

  最差情况:  2 线程 SMT → 性能下降
             (两个 CPU-bound 线程争抢执行单元和缓存)

  最佳场景:  1 个计算密集 + 1 个内存密集
             (互补利用流水线资源)

  内核调度:  sched_smt_weight 考虑 SMT 影响
             优先在同一核心的不同线程上调度不相关的任务
```

### 5.3 Linux 内核中的 SMT 感知

> **内核源码佐证：** 内核通过 CPUID 和 APIC 拓扑信息检测 SMT：

```c
/* 源码位置：src/linux-5.10/arch/x86/include/asm/cpufeatures.h:55
 *
 * ★ 超线程特性位 — CPUID 叶节点 1 的 EDX 第 28 位
 */
#define X86_FEATURE_HT          ( 0*32+28) /* Hyper-Threading */
```

```c
/* 源码位置：src/linux-5.10/arch/x86/include/asm/processor.h:81
 *
 * ★ cpuinfo_x86 结构体 — 内核为每个逻辑 CPU 维护一份
 *   包含了 CPU 家族/型号/缓存/拓扑等所有信息。
 */
struct cpuinfo_x86 {
    __u8            x86;            /* CPU family（如 6 = Core/Xeon） */
    __u8            x86_vendor;     /* CPU vendor（Intel/AMD/...） */
    __u8            x86_model;      /* CPU model（如 0x55 = Skylake-SP） */
    __u8            x86_stepping;   /* CPU stepping（修订版本） */
    int             x86_tlbsize;    /* DTLB/ITLB 总条目数 */
    __u8            x86_virt_bits;  /* 虚拟地址位数（通常 48） */
    __u8            x86_phys_bits;  /* 物理地址位数（通常 46） */
    __u32           x86_capability[NCAPINTS + NBUGINTS]; /* ★ CPU 特性位图 */
    char            x86_vendor_id[16];  /* "GenuineIntel" / "AuthenticAMD" */
    unsigned int    x86_cache_size;     /* ★ 缓存大小 (KB) */
    int             x86_cache_alignment;/* 缓存行对齐（通常 64 字节） */
    u16             x86_max_cores;      /* ★ CPUID 报告的最大核心数 */
    u16             apicid;             /* APIC ID */
    u16             phys_proc_id;       /* ★ 物理 CPU 插槽编号 */
    u16             cpu_core_id;        /* ★ 物理核心编号 */
    u16             cpu_die_id;         /* ★ Die 编号（多 Die 封装） */
    u32             microcode;          /* 微码版本号 */
} __randomize_layout;
```

```c
/* 源码位置：src/linux-5.10/arch/x86/kernel/cpu/common.c:774
 *
 * ★ detect_ht() — 检测超线程（SMT）拓扑
 *   通过 CPUID 叶节点 1 的 EBX[23:16] 获取每核心的逻辑线程数。
 */
void detect_ht(struct cpuinfo_x86 *c)
{
    int index_msb, core_bits;

    if (detect_ht_early(c) < 0)
        return;

    /* ★ smp_num_siblings = 每核心的逻辑处理器数量
     *   来自 CPUID(1).EBX[23:16]，值为 2 表示启用了 HT */
    index_msb = get_count_order(smp_num_siblings);
    c->phys_proc_id = apic->phys_pkg_id(c->initial_apicid, index_msb);

    /* 除以核心数得到每核心的线程数 */
    smp_num_siblings = smp_num_siblings / c->x86_max_cores;

    /* 从 APIC ID 中提取核心 ID */
    c->cpu_core_id = apic->phys_pkg_id(c->initial_apicid, index_msb) &
                     ((1 << core_bits) - 1);
}
```

```c
/* 源码位置：src/linux-5.10/arch/x86/kernel/smpboot.c:567
 *
 * ★ set_cpu_sibling_map() — 建立 CPU 拓扑关系映射
 *   在 SMP 启动时为每个 CPU 建立 sibling/core/package 关系。
 *   这是 /sys/devices/system/cpu/cpu*/topology/ 的基础。
 */
void set_cpu_sibling_map(int cpu)
{
    bool has_smt = smp_num_siblings > 1;       /* ★ 是否支持 SMT */
    bool has_mp = has_smt || boot_cpu_data.x86_max_cores > 1;

    /* 遍历所有已初始化的 CPU，建立拓扑关系 */
    for_each_cpu(i, cpu_sibling_setup_mask) {
        o = &cpu_data(i);

        /* ★ match_smt: 两个逻辑 CPU 是否共享同一物理核心？
         *   → 填充 topology_sibling_cpumask（/sys/.../thread_siblings_list） */
        if ((i == cpu) || (has_smt && match_smt(c, o)))
            link_mask(topology_sibling_cpumask, cpu, i);

        /* ★ match_llc: 是否共享最后一级缓存（LLC）？
         *   → 填充 cpu_llc_shared_mask（/sys/.../core_siblings_list） */
        if ((i == cpu) || (has_mp && match_llc(c, o)))
            link_mask(cpu_llc_shared_mask, cpu, i);

        /* ★ match_pkg: 是否在同一物理封装内？
         *   → 填充 topology_core_cpumask */
        if ((i == cpu) || (has_mp && match_pkg(c, o)))
            link_mask(topology_core_cpumask, cpu, i);
    }
}
```

```
  /sys/devices/system/cpu/smt/
  ├── active    ← 当前 SMT 是否启用 (1=启用)
  ├── control   ← SMT 控制策略:
  │              on      = 全部启用
  │              off     = 全部禁用
  │              forceoff = 强制禁用 (不可逆)
  │              notsupported = 硬件不支持
  └── present   ← SMT 是否可用

  /sys/devices/system/cpu/cpu0/topology/
  ├── thread_siblings_list  ← 同一核心的线程列表 (如 "0-1")
  ├── core_siblings_list    ← 同一 package 的核心列表
  ├── core_id               ← 物理核心编号
  └── physical_package_id   ← 物理 CPU 插槽编号
```

---

## 6. 内存序与内存屏障

### 6.1 ★ x86 内存序模型

```
  x86 采用 TSO (Total Store Order) 模型:

  允许的乱序:
  ✓ Store → Load 重排序 (Store 缓冲导致)
  ✓ CLFLUSH 重排序
  ✓ 不同地址的 Store → Store 不保证顺序（在某些场景）

  保证的顺序:
  ✗ Load → Load 不重排序
  ✗ Load → Store 不重排序
  ✗ Store → Store 同一地址不重排序
  ✗ 有依赖关系的操作不重排序

  对比其他架构:
  ┌──────────┬───────────────────────────────┐
  │ x86 TSO  │ 相对强一致，Store Buffer 是唯一 │
  │          │ 乱序源                           │
  ├──────────┼───────────────────────────────┤
  │ ARM WMO  │ 弱内存序，几乎所有操作都可能     │
  │          │ 重排序，需要显式屏障              │
  ├──────────┼───────────────────────────────┤
  │ RISC-V   │ RVWMO 弱内存序模型              │
  └──────────┴───────────────────────────────┘
```

### 6.2 内存屏障指令

| 屏障类型 | x86 指令 | 语义 | 内核宏 |
|---------|---------|------|--------|
| 全屏障 | `MFENCE` | 所有 Load/Store 不得跨越此屏障 | `smp_mb()` |
| Load 屏障 | `LFENCE` | 所有 Load 不得跨越此屏障 | `smp_rmb()` |
| Store 屏障 | `SFENCE` | 所有 Store 不得跨越此屏障 | `smp_wmb()` |
| 编译器屏障 | `""` (asm volatile) | 阻止编译器重排序 | `barrier()` |

### 6.2.1 内核中的内存屏障实现

> **内核源码佐证：** 以下是 x86 架构上内存屏障的实际实现：

```c
/* 源码位置：src/linux-5.10/arch/x86/include/asm/barrier.h
 *
 * ★ x86 内存屏障的实际实现
 *   x86 是 TSO（强序）模型，大多数场景只需编译器屏障。
 *   只有需要硬件屏障时才使用 MFENCE/LFENCE/SFENCE。
 */

/* === 基础屏障指令 === */
#define mb()    asm volatile("mfence":::"memory")   /* ★ 全屏障：序列化所有 Load/Store */
#define rmb()   asm volatile("lfence":::"memory")   /* ★ Load 屏障：序列化所有 Load */
#define wmb()   asm volatile("sfence" ::: "memory") /* ★ Store 屏障：序列化所有 Store */

/* === SMP 屏障（多核场景） === */
/*
 * ★ 注意：x86 的 __smp_rmb() 和 __smp_wmb() 退化为编译器屏障！
 *   因为 x86 TSO 模型保证：
 *   - Load 不会被其他 Load 重排序 → rmb 只需防编译器
 *   - Store 不会被其他 Store 重排序 → wmb 只需防编译器
 *   只有 smp_mb() 需要真正的硬件屏障（MFENCE 或 LOCK 指令）
 */
#define __smp_mb()    asm volatile("lock; addl $0,-4(%%rsp)" ::: "memory", "cc")
#define __smp_rmb()   dma_rmb()    /* 在 x86 上 = barrier()（编译器屏障） */
#define __smp_wmb()   barrier()    /* 编译器屏障即可 */

/* === Acquire/Release 语义 === */
/*
 * ★ x86 TSO 下，普通的 READ_ONCE/WRITE_ONCE 已经具有 Acquire/Release 语义
 *   因此只需编译器屏障来防止编译器重排序
 */
#define __smp_store_release(p, v)             \
do {                                          \
    compiletime_assert_atomic_type(*p);       \
    barrier();            /* 编译器屏障 */     \
    WRITE_ONCE(*p, v);    /* ★ Store 释放 */  \
} while (0)

#define __smp_load_acquire(p)                 \
({                                            \
    typeof(*p) ___p1 = READ_ONCE(*p);         \
    compiletime_assert_atomic_type(*p);       \
    barrier();            /* 编译器屏障 */     \
    ___p1;                /* ★ Load 获取 */   \
})

/*
 * ★ 原子操作在 x86 上自带序列化语义（LOCK 前缀），
 *   所以原子操作前后的屏障为空操作
 */
#define __smp_mb__before_atomic()  do { } while (0)
#define __smp_mb__after_atomic()   do { } while (0)

/* === 推测执行屏障 === */
/* ★ barrier_nospec: 阻止推测执行越过此点（Spectre V1 缓解） */
#define barrier_nospec() alternative("", "lfence", X86_FEATURE_LFENCE_RDTSC)
```

### 6.3 Store Buffer 与内存序问题

```

  初始状态: x = 0, y = 0

  CPU 0:              CPU 1:
  x = 1;              y = 1;
  r1 = y;             r2 = x;

  预期: r1==0 && r2==0 不可能同时为真
  实际: 在 x86 上可能出现 r1==0 && r2==0!

  原因:
  CPU 0:
    x = 1    → 写入 Store Buffer (尚未对其他 CPU 可见)
    r1 = y   → 从缓存读取 y = 0 → r1 = 0

  CPU 1:
    y = 1    → 写入 Store Buffer
    r2 = x   → 从缓存读取 x = 0 → r2 = 0

  解决: 在 Store 和 Load 之间加屏障
  CPU 0:  x = 1; smp_mb(); r1 = y;
  CPU 1:  y = 1; smp_mb(); r2 = x;
  → 屏障确保 Store Buffer 中的写入先刷入缓存，再做 Load
```

### 6.4 内核中的内存屏障使用场景

```
  常见使用场景:

  1. 生产者-消费者模式:
     生产者: data = value; smp_wmb(); ready = 1;
     消费者: while(!ready); smp_rmb(); use(data);

  2. RCU (Read-Copy-Update):
     rcu_assign_pointer() → 内部使用 smp_store_release()
     rcu_dereference()     → 内部使用 smp_load_acquire()

  3. 锁释放:
     spin_unlock() → smp_store_release() 确保临界区内
                     所有操作在锁释放前对其他 CPU 可见

  4. 原子操作:
     atomic_add_return() → 自带全屏障
     atomic_add()         → 不带屏障 (需要手动加)
```

---

## 7. 关键术语速查表

| 术语 | 英文全称 | 含义 |
|------|---------|------|
| μop | Micro-Operation | CPU 内部实际执行的最小操作单元 |
| ROB | Reorder Buffer | 重排序缓冲区，保证乱序执行后有序退休 |
| RS | Reservation Station | 保留站，等待操作数就绪后派遣到执行单元 |
| RAT | Register Alias Table | 寄存器别名表，实现寄存器重命名 |
| PRF | Physical Register File | 物理寄存器文件，~180 个物理寄存器 |
| BTB | Branch Target Buffer | 分支目标缓冲，缓存分支跳转目标地址 |
| RAS | Return Address Stack | 返回地址栈，预测 RET 指令 |
| PHT | Pattern History Table | 模式历史表，分支预测用 |
| MSROM | Microcode Sequencer ROM | 微码序列 ROM，存储复杂指令的微码 |
| DSB | Decoded Stream Buffer | 解码流缓冲（μop Cache） |
| MESI | Modified/Exclusive/Shared/Invalid | 缓存一致性协议四状态 |
| TSO | Total Store Order | x86 的内存序模型 |
| SMT | Simultaneous Multi-Threading | 同步多线程（Intel 称 Hyper-Threading） |
| TLB | Translation Lookaside Buffer | 地址转换缓存 |
| PMU | Performance Monitoring Unit | 性能监控单元，硬件计数器 |
| IPC | Instructions Per Cycle | 每周期指令数，衡量流水线效率 |
| ILP | Instruction-Level Parallelism | 指令级并行度 |
| MLP | Memory-Level Parallelism | 内存级并行度 |
| LLC | Last-Level Cache | 最后一级缓存（通常指 L3） |

---

## 8. 内核源码索引

> 以下是本文引用的所有内核源码文件及其与 CPU 微架构概念的对应关系：

| 微架构概念 | 内核源码路径 | 关键函数/定义 |
|-----------|-------------|-------------|
| 指令集特性（流水线 Decode/Execute） | `arch/x86/include/asm/cpufeatures.h` | `X86_FEATURE_*` 宏（~200 个特性位） |
| 推测执行控制（分支预测安全） | `arch/x86/include/asm/msr-index.h` | `MSR_IA32_SPEC_CTRL`, `SPEC_CTRL_IBRS/STIBP/SSBD` |
| 硬件漏洞免疫标志 | `arch/x86/include/asm/msr-index.h` | `MSR_IA32_ARCH_CAPABILITIES`, `ARCH_CAP_*` |
| CPU 信息结构体 | `arch/x86/include/asm/processor.h` | `struct cpuinfo_x86`（缓存/拓扑/特性） |
| CPUID 特性枚举 | `arch/x86/kernel/cpu/common.c:898` | `get_cpu_cap()` |
| 缓存大小探测 | `arch/x86/kernel/cpu/common.c:690` | `cpu_detect_cache_sizes()` |
| TLB 探测与输出 | `arch/x86/kernel/cpu/common.c:737` | `cpu_detect_tlb()` |
| 超线程检测 | `arch/x86/kernel/cpu/common.c:774` | `detect_ht()` |
| 推测执行控制初始化 | `arch/x86/kernel/cpu/common.c:857` | `init_speculation_control()` |
| CPU 识别总流程 | `arch/x86/kernel/cpu/common.c:1500` | `identify_cpu()` |
| 硬件漏洞缓解入口 | `arch/x86/kernel/cpu/bugs.c:79` | `check_bugs()` |
| SPEC_CTRL 虚拟化 | `arch/x86/kernel/cpu/bugs.c:156` | `x86_virt_spec_ctrl()` |
| SMP 拓扑映射 | `arch/x86/kernel/smpboot.c:567` | `set_cpu_sibling_map()` |
| 内存屏障（x86 TSO） | `arch/x86/include/asm/barrier.h` | `mb()`, `rmb()`, `wmb()`, `__smp_*` |
| 推测执行屏障 | `arch/x86/include/asm/barrier.h:52` | `barrier_nospec()` |

```
  ★ 内核 CPU 初始化总流程（与本文各章节的对应关系）:

  start_kernel()
    └── check_bugs()                          ← 【第3章】Spectre/Meltdown 缓解
         ├── identify_boot_cpu()
         │    └── identify_cpu()               ← 【第5章】CPU 识别总入口
         │         ├── generic_identify()
         │         │    ├── get_cpu_cap()      ← 【第1/4章】CPUID 枚举特性
         │         │    │    └── init_speculation_control() ← 【第3章】推测执行
         │         │    ├── cpu_detect_cache_sizes() ← 【第2章】缓存探测
         │         │    └── cpu_detect_tlb()   ← 【第2章】TLB 探测
         │         ├── this_cpu->c_identify()  ← 厂商特定识别（intel.c / amd.c）
         │         └── detect_ht()             ← 【第5章】超线程检测
         ├── rdmsrl(MSR_IA32_SPEC_CTRL)       ← 【第3章】读取推测控制 MSR
         ├── spectre_v2_select_mitigation()   ← 【第3章】选择缓解策略
         └── arch_smt_update()                ← 【第5章】根据 SMT 更新缓解

  SMP 启动（AP 核初始化）:
    └── set_cpu_sibling_map()                 ← 【第5章】建立拓扑关系
         ├── match_smt()  → topology_sibling_cpumask  (thread_siblings)
         ├── match_llc()  → cpu_llc_shared_mask       (core_siblings)
         └── match_pkg() → topology_core_cpumask      (package)
```
