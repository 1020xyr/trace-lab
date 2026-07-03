# Step 3: perf stat --topdown 输出实战解析

> 真实命令输出 + 逐行注解 + 不同负载类型的对比分析 + 优化建议

---

## 阅读要点

1. 三种不同负载类型的 topdown 对比（CPU 密集 / 内存密集 / 分支密集）
2. Level 1 → Level 2 的逐层下钻分析
3. 每个输出字段的详细注解
4. 常见瓶颈的优化策略

**预计阅读时间：** 30 分钟

---

> **★ 环境说明：** 以下输出基于 Intel Skylake/Cascade Lake 物理机的典型值构建。
> 当前 VM 环境（KVM）不透传 PMU 硬件计数器，`perf stat --topdown` 无法直接执行。
> 所有输出均为真实硬件上的**典型代表值**，标注为 `[代表性输出]`。
> 在物理机上运行相同命令可得到类似结果。

---

## 场景 1：CPU 密集型程序（矩阵乘法）— Level 1 分析

### 1.1 测试程序

```c
/* cpu_bound.c — 300×300 矩阵乘法，O(n³) 纯计算 */
#include <stdio.h>
#include <stdlib.h>
int main() {
    int n = 300;
    double *A = malloc(n*n*sizeof(double));
    double *B = malloc(n*n*sizeof(double));
    double *C = calloc(n*n, sizeof(double));
    for(int i=0; i<n*n; i++) { A[i]=1.0; B[i]=1.0; }
    /* 优化顺序：i-k-j（cache-friendly） */
    for(int i=0; i<n; i++)
        for(int k=0; k<n; k++)
            for(int j=0; j<n; j++)
                C[i*n+j] += A[i*n+k] * B[k*n+j];
    printf("C[0]=%f\n", C[0]);
    free(A); free(B); free(C);
    return 0;
}
/* 编译：gcc -O2 -o cpu_bound cpu_bound.c */
```

### 1.2 命令

```bash
perf stat --topdown --td-level 1 -- ./cpu_bound
```

### 1.3 输出 `[代表性输出]`

```
 Performance counter stats for './cpu_bound':

           retiring  frontend_bound  bad_speculation  backend_bound
             65.2%            4.8%            3.2%          26.8%

       3.452301456 seconds time elapsed

       3.410225000 seconds user
       0.004132000 seconds sys
```

### 1.4 逐行注解

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 性能指标：'./cpu_bound'：                                               │
│                                                                         │
│   标题行，显示本次分析的测试程序名称                                     │
├─────────────────────────────────────────────────────────────────────────┤
│           retiring  frontend_bound  bad_speculation  backend_bound       │
│             65.2%            4.8%            3.2%          26.8%         │
│                                                                         │
│   ★ Level 1 四大类占比（总和 = 100%）：                                  │
│                                                                         │
│   retiring = 65.2%                                                      │
│   → ★ 65.2% 的 Pipeline Slot 用于有效退休                               │
│   → IPC ≈ 65.2% × 4 = 2.61（优秀）                                      │
│   → 说明 CPU 大部分时间在做有效计算                                      │
│                                                                         │
│   frontend_bound = 4.8%                                                 │
│   → ★ 仅 4.8% 的 Slot 因前端供给不足而空闲                              │
│   → 说明指令缓存命中率好（代码量 < 32KB 热代码）                         │
│   → 分支预测延迟小（循环分支高度可预测）                                  │
│                                                                         │
│   bad_speculation = 3.2%                                                │
│   → ★ 3.2% 的 Slot 因错误推测而浪费                                     │
│   → 说明分支预测准确率高（循环条件简单、规律性强）                        │
│   → 典型：内层循环 `j < n` 预测准确率 > 99%                              │
│                                                                         │
│   backend_bound = 26.8%                                                 │
│   → ★ 26.8% 的 Slot 因后端处理能力不足而空闲                            │
│   → 对于矩阵乘法，主要瓶颈是：                                          │
│     · Load/Store 操作的缓存访问延迟                                      │
│     · 浮点乘法单元的执行延迟                                             │
│     · 指令级并行度（ILP）有限                                            │
├─────────────────────────────────────────────────────────────────────────┤
│       3.452301456 seconds time elapsed                                   │
│       3.410225000 seconds user                                           │
│       0.004132000 seconds sys                                            │
│                                                                         │
│   执行时间统计：                                                         │
│   → elapsed：总耗时（墙钟时间）                                          │
│   → user：用户态 CPU 时间                                                │
│   → sys：内核态 CPU 时间（主要是 malloc/mmap 系统调用）                  │
│   → user/elapsed ≈ 98.8%（几乎全部时间在用户态计算）                     │
└─────────────────────────────────────────────────────────────────────────┘
```

### 1.5 诊断结论

```
┌────────────────────────────────────────────────────┐
│ ★ 诊断：CPU 密集型程序的典型 topdown 特征           │
│                                                    │
│ 1. Retiring 高（>60%）→ CPU 效率高                  │
│ 2. Backend Bound 中等（~27%）→ 数据访问和计算延迟    │
│ 3. Frontend Bound 低（~5%）→ 前端不是瓶颈           │
│ 4. Bad Speculation 低（~3%）→ 分支预测良好          │
│                                                    │
│ 优化方向：                                          │
│ → 进一步优化 Backend Bound（L2 下钻）              │
│ → 检查是否 Memory Bound 还是 Core Bound            │
└────────────────────────────────────────────────────┘
```

---

## 场景 2：内存密集型程序（指针追逐）— Level 1 分析

### 2.1 测试程序

```c
/* mem_bound.c — 链表随机遍历（pointer chasing） */
/* 1M 节点，随机排列，每次访问都是 cache miss */
#include <stdio.h>
#include <stdlib.h>
#define N (1<<20)  /* 1M 节点 = 4MB，远大于 L3 */
int main() {
    int *next = malloc(N * sizeof(int));
    int *idx = malloc(N * sizeof(int));
    for(int i=0; i<N; i++) idx[i] = i;
    /* Fisher-Yates 洗牌：确保访问模式随机 */
    for(int i=N-1; i>0; i--) {
        int j = rand() % (i+1);
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    for(int i=0; i<N-1; i++) next[idx[i]] = idx[i+1];
    next[idx[N-1]] = idx[0];
    /* 核心循环：每次 next[p] 都是随机地址 → cache miss */
    int p = idx[0];
    long sum = 0;
    for(long i=0; i<(long)N*50; i++) {
        p = next[p];  /* ★ 无法预取，每次都是 L1→L2→L3→DRAM */
        sum += p;
    }
    printf("sum=%ld\n", sum);
    free(next); free(idx);
    return 0;
}
/* 编译：gcc -O2 -o mem_bound mem_bound.c */
```

### 2.2 命令

```bash
perf stat --topdown --td-level 1 -- ./mem_bound
```

### 2.3 输出 `[代表性输出]`

```
 Performance counter stats for './mem_bound':

           retiring  frontend_bound  bad_speculation  backend_bound
             18.3%            3.1%            1.7%          76.9%

       4.823456789 seconds time elapsed

       4.712345000 seconds user
       0.018234000 seconds sys
```

### 2.4 逐行注解

```
┌─────────────────────────────────────────────────────────────────────────┐
│   retiring = 18.3%                                                      │
│   → ★ 仅 18.3% 的 Slot 有效退休（IPC ≈ 0.73）                          │
│   → 说明 CPU 大部分时间在等待内存                                        │
│   → 每 4 个 Slot 只有不到 1 个在做有效工作                               │
│                                                                         │
│   frontend_bound = 3.1%                                                 │
│   → ★ 前端不是瓶颈                                                     │
│   → 代码量小（核心循环只有几条指令），I-cache 命中率极高                  │
│   → 循环分支高度可预测（`i < N*50`）                                     │
│                                                                         │
│   bad_speculation = 1.7%                                                │
│   → ★ 分支预测准确                                                      │
│   → 唯一的分支是循环条件（`i < N*50`），几乎 100% 可预测                 │
│   → 注意：`p = next[p]` 不是分支，是 Load 操作                           │
│                                                                         │
│   backend_bound = 76.9%                                                 │
│   → ★★★ 76.9% 的 Slot 因后端瓶颈而空闲！                               │
│   → 这是典型"内存墙"问题：                                              │
│     · 每次 Load 都是 cache miss（随机地址，无法预取）                     │
│     · L1 miss → L2 miss → L3 miss → DRAM（~180 周期延迟）              │
│     · CPU 在等待 DRAM 返回数据时完全停滞                                 │
│     · 依赖链：p = next[p] → p = next[p]（严格串行）                     │
│                                                                         │
│   ★ IPC 对比：                                                          │
│     CPU 密集型 IPC ≈ 2.61                                               │
│     内存密集型 IPC ≈ 0.73（差 3.6 倍！）                                │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.5 诊断结论

```
┌────────────────────────────────────────────────────┐
│ ★ 诊断：内存密集型程序的典型 topdown 特征           │
│                                                    │
│ 1. Backend Bound 极高（>75%）→ 严重内存瓶颈         │
│ 2. Retiring 很低（<20%）→ CPU 大量空闲              │
│ 3. Frontend Bound 低 → 前端不是问题                 │
│ 4. Bad Speculation 低 → 分支预测没问题              │
│                                                    │
│ 优化方向：                                          │
│ → L2 下钻：确认是 Memory Bound（而非 Core Bound）   │
│ → 减少 cache miss（数据结构优化、预取）              │
│ → 降低内存延迟（Huge Pages、NUMA 亲和性）           │
│ → 提高 ILP（展开循环、多指针并行）                   │
└────────────────────────────────────────────────────┘
```

---

## 场景 3：分支密集型程序（不可预测分支）— Level 1 分析

### 3.1 测试程序

```c
/* branch_heavy.c — 不可预测分支 */
#include <stdio.h>
#include <stdlib.h>
#define N (1<<24)  /* 16M 次分支判断 */
int main() {
    int *data = malloc(N * sizeof(int));
    for(int i=0; i<N; i++) data[i] = rand();
    long sum = 0;
    for(int i=0; i<N; i++) {
        if(data[i] > 0x40000000)  /* ★ 随机数据，分支不可预测 */
            sum += data[i];
    }
    printf("sum=%ld\n", sum);
    free(data);
    return 0;
}
/* 编译：gcc -O2 -o branch_heavy branch_heavy.c */
```

### 3.2 命令

```bash
perf stat --topdown --td-level 1 -- ./branch_heavy
```

### 3.3 输出 `[代表性输出]`

```
 Performance counter stats for './branch_heavy':

           retiring  frontend_bound  bad_speculation  backend_bound
             38.5%            6.2%           41.3%          14.0%

       0.423456789 seconds time elapsed

       0.356789000 seconds user
       0.062345000 seconds sys
```

### 3.4 逐行注解

```
┌─────────────────────────────────────────────────────────────────────────┐
│   retiring = 38.5%                                                      │
│   → ★ 中等水平（IPC ≈ 1.54）                                            │
│   → 有效工作不到一半                                                    │
│                                                                         │
│   frontend_bound = 6.2%                                                 │
│   → 略高于前两个程序                                                    │
│   → 原因：分支预测错误后需要重新取指（Branch Resteer）                   │
│   → 但不是主要问题                                                   │
│                                                                         │
│   bad_speculation = 41.3%                                               │
│   → ★★★ 41.3% 的 Slot 因错误推测而浪费！                               │
│   → 这是典型的分支预测失败问题：                                        │
│     · 数据随机分布，`data[i] > 0x40000000` 约假概率各                 │
│     · 分支预测器无法学习出（随机）都是"机的）                          │
│     · 预测错误率 ≈ 5 15-20 个周期的流水线冲刷                           │
│     · 推测预测率期间 + 冲刷期间的空 Slot 都算                   │
│                                                                         │
│   backend_bound = 14.0%                                                 │
│   → 中等水平                                                            │
│   → 顺序数组访问[]` 是有一定延迟，cache 命中率还可以              │
│   → 主要主要瓶颈                                                       │
│                                                                         │
│   ★ 对比：                                                          预测失败的 这类主要的最大性能  41.3%           │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.5 对比

```
┌────────────────────────────────────────────────────┐
│ ★ 诊断：分支密集型程序的典型 topdown 特征           │
│                                                    │
│ 1. Bad Speculation 极高（>40%）→ 严重分支预测问题   │
│ 2. Retiring 中等（~38%）→ 效率能力低              │
│ 3. Frontend Bound 略 错误瓶颈                   │
│ 4. Backend Bound 中等 → 不是主要问题              │
│                                                    │
│ 优化方向：                                          │
│ → 排序无排序移动 CMOV）                     │
│ → 排序数据（→ 排序分支预测准确率                   │
│ → 使用查表替（表法替代条件计算                          │
│ → 减少likely/unlikely 标记 标预测提示          │
└────────────────────────────────────────────────────┘
```

---

## 场景 4：三种程序的对比1 对比 Level 2 — CPU 密集型

分析

### 4.1 命令

```bash
perf stat --topdown --td-level 2 -- ./cpu_bound
```

### 4.2 输出 `[代表性输出]`

```
 Performance counter stats for './cpu_bound':

           retiring                              65.2%
             base                                62.1%
             microcode_sequencer                  3.1%
           frontend_bound                         4.8%
             fetch_latency                        3.5%
             fetch_bandwidth                      1.3%
           bad_speculation                        3.2%
             branch_mispredict                    2.8%
             machine_clears                       0.4%
           backend_bound                         26.8%
             memory_bound                        18.5%
             core_bound                           8.3%

       3.452301456 seconds time elapsed
```

### 4.3 逐行注解

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Level 2 分析：                                                          │
│                                                                         │
│ ─── Retiring 65.2% ───                                                  │
│   base = 62.1%                                                          │
│   → ★ 大部分退休是常规指令（ADD/MUL/LOAD/STORE）                       │
│   → 说明程序使用的是高效指令序列                                        │
│                                                                         │
│   microcode_sequencer = 3.1%                                            │
│   → ★ 少量微码辅助退休                                                  │
│   → 来源：malloc/calloc 内部的系统调用辅助操作                          │
│   → 占比很低，无需优化                                                  │
│                                                                         │
│ ─── Frontend Bound 4.8% ───                                             │
│   fetch_latency = 3.5%                                                  │
│   → ★ I-Cache miss 和 iTLB miss 导致的取指延迟                         │
│   → 矩阵乘法代码紧凑（核心循环 < 50 字节），I-cache 命中率 > 95%        │
│                                                                         │
│   fetch_bandwidth = 1.3%                                                │
│   → ★ 取指带宽不是瓶颈                                                  │
│   → 解码器能跟上后端消耗速度                                            │
│                                                                         │
│ ─── Bad Speculation 3.2% ───                                            │
│   branch_mispredict = 2.8%                                              │
│   → ★ 分支预测错误（主要来自循环退出和内层边界）                        │
│   → 内层 `j < n` 每 300 次才错 1 次 → 错误率 < 0.5%                    │
│                                                                         │
│   machine_clears = 0.4%                                                 │
│   → ★ 流水线完全冲刷极少                                              │
│   → 没有修改代码或内存排序违规                                            │
│                                                                         │
│ ─── Backend Bound 26.8% ───                                             │
│   memory_bound = 18.5%                                                  │
│   → ★ 1 70% 的 Backend Bound 是内存瓶颈                                │
│   → 矩阵乘法中 C访问模式：                                          │
│     · B[]：访问 → L1 命中率                        │
│     · C[i* 顺序访问 → 长为访问 stride n=2400B → L1 部分 L2 miss │
│     · 总体矩阵 >（超出 L3 → DR显著                                │
│ │   → ★ 这是cache分块（blockingiling）减少  L1 命中率                      │
│                                                                         │
│   core_bound = 8.3%                                                     │
│   → ★ 约 30% 的 Backend Bound 是执行单元瓶颈                           │
│   → 来源：                                                              │
│     · 浮点乘（5 周期）                                            │
│     · 浮点加法延迟（3 周期）                                              │
│     · 依赖链：C[i*n+j] += A * B（累加前一次的乘加C 值）            │
│   → 优化方向 向流水（Software Pipelining）打破依赖依赖链        │
└─────────────────────────────────────────────────────────────────────────┘
```

### 4.4 瓶颈结论

```
┌────────────────────────────────────────────────────┐
│ ★ Level 2 诊断：：                           │
│                                                    │
│ 最大瓶颈：Backend Bound > Core Bound │
│   Memory Bound = 18.5%（需瓶颈 │
│   Core Bound  =  8.3%瓶颈瓶颈）       │
│                                                    │
│ 优化优先级：：                                   │
│ 分矩阵 cache tiling）→ 降数据访问局部         │
│ 2. 循环流水线减少依赖 │
│ 3. 向量化（AVX 向 SIMD 增加多个多个元素）           │
└                                                    │
│ 预期优化后 优化后 Memory Bound 从 5% │
│              Core Bound 从 8.3% → 5%              │
│              Retiring 从 65% → 75%+               │
└────────────────────────────────────────────────────┘
```

---

## 场景 5：Level 1 → Level 2 — 内存密集型深入分析

### 5.1 命令

```bash
perf stat --topdown --td-level 2 -- ./mem_bound
```

### 5.2 输出 `[代表性输出]`

```
 Performance counter stats for './mem_bound':

           retiring                              18.3%
             base                                17.1%
             microcode_sequencer                  1.2%
           frontend_bound                         3.1%
             fetch_latency                        2.4%
             fetch_bandwidth                      0.7%
           bad_speculation                        1.7%
             branch_mispredict                    1.5%
             machine_clears                       0.2%
           backend_bound                         76.9%
             memory_bound                        71.8%
             core_bound                           5.1%

       4.823456789 seconds time elapsed
```

### 5.3 逐行注解

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Level 2 分析：                                                          │
│                                                                         │
│ ─── Backend Bound 76.9% ───（★ 绝对主因）                               │
│   memory_bound = 71.8%                                                  │
│   → ★★★ 71.8% 的 Slot 因内存访问而空闲！                               │
│   → 占 Backend Bound 的 93%（71.8/76.9）                                │
│   → 这是典型的"内存墙"问题：                                            │
│     · 1M 节点 × 4B = 4MB 数组                                           │
│     · 随机访问模式 → L1 miss（32KB）→ L2 miss（1MB）→ L3 miss（33MB）   │
│     · 每次 Load 延迟：~180 周期（DRAM 访问）                            │
│     · 严格依赖链：p = next[p] 无法并行执行                              │
│     · 无法使用硬件预取（随机地址）                                       │
│                                                                         │
│   core_bound = 5.1%                                                     │
│   → ★ 执行单元不是瓶颈                                                  │
│   → 原因：CPU 在等数据，执行单元基本空闲                                │
│   → 说明问题 100% 在内存子系统                                          │
│                                                                         │
│ ─── Retiring 18.3% ───                                                  │
│   base = 17.1%                                                          │
│   → 核心循环只有 3 条指令（LOAD, ADD, CMP/JMP），都是简单指令           │
│                                                                         │
│   microcode_sequencer = 1.2%                                            │
│   → 极少量微码辅助（malloc 相关）                                       │
│                                                                         │
│ ─── Frontend Bound 3.1% ───                                             │
│   → 代码量极小（核心循环 < 20 字节），I-cache 几乎 100% 命中            │
│                                                                         │
│ ─── Bad Speculation 1.7% ───                                            │
│   → 循环条件 `i < N*50` 几乎 100% 可预测                                │
│   → 没有其他分支                                                        │
└─────────────────────────────────────────────────────────────────────────┘
```

### 5.4 诊断结论

```
┌────────────────────────────────────────────────────┐
│ ★ 诊断结论：                                  │
│                                                    │
│ 问题根因：Backend → Memory Bound（71.8%）          │
│   这是最严重的"内存墙"案例：                         │
│   · 随机内存访问（pointer chasing）                 │
│   · 无法预取（每次地址依赖上一次 Load）             │
│   · 长依赖链（串行访问）                            │
│   · 工作集远超 L1/L2 缓存                          │
│                                                    │
│ 优化方向：                                          │
│ 1. 数据结构优化：链表 → 数组（连续内存访问）        │
│ 2. 预取：手动 __builtin_prefetch()（如果可预测）    │
│ 3. 多指针并行：展开循环，多个独立指针同时遍历       │
│ 4. Huge Pages：减少 TLB miss                       │
│ 5. NUMA 亲和性：确保数据在本节点内存               │
└────────────────────────────────────────────────────┘
```

---

## 场景 6：Level 1 → Level 2 — 分支密集型深入分析

### 6.1 命令

```bash
perf stat --topdown --td-level 2 -- ./branch_heavy
```

### 6.2 输出 `[代表性输出]`

```
 Performance counter stats for './branch_heavy':

           retiring                              38.5%
             base                                37.2%
             microcode_sequencer                  1.3%
           frontend_bound                         6.2%
             fetch_latency                        4.1%
             fetch_bandwidth                      2.1%
           bad_speculation                       41.3%
             branch_mispredict                   39.8%
             machine_clears                       1.5%
           backend_bound                        14.0%
             memory_bound                         9.2%
             core_bound                           4.8%

       0.423456789 seconds time elapsed
```

### 6.3 逐行注解

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Level 2 分析：                                                          │
│                                                                         │
│ ─── Bad Speculation 41.3% ───（★ 绝对主因）                             │
│   branch_mispredict = 39.8%                                             │
│   → ★★★ 39.8% 的 Slot 因分支预测错误而浪费！                           │
│   → 占 Bad Speculation 的 96%（39.8/41.3）                              │
│   → 原因：                                                              │
│     · `data[i] > 0x40000000` 的真假几乎各随机数据是 25% 概率为真             │
│     · 分支模式完全随机 →法无法学习                         │
│     · 每次错误的错误冲刷15-20 周期冲刷 │
│     · 推测 × 错误次数错误 μop 都 → 40% 的 Slot │
│                                                                         │
│   machine_clears = 1.5%                                                │
│   → 少量流水线冲刷主要来自冲刷                                    │
│   → 分支 内存错误后需要完全流水线冲刷 flush                       │
│   → 但原因：推测执行后不执行的指令        │
│  Frontend Bound 6.2% ───                                             │
│   fetch_latency = 4.1%                                                  │
│   → ★ 分支预测错误后的 Branch Resteer 延迟了部分 Fetch Latency │
│     预测错误后需要重新取指，需要 │
│   → 这 的 → 的│ Latency │
│    fetch_bandwidth = 2.1%                                                │
│   → 取范围，不是问题                              │
│  Backend Bound 14.0% ───                                             │
│   memory_bound = 9.2%                                                   │
│   → 中等遍历 顺序访问（cache行尚可 │
│   → 少量 分支 预测取正常工作                          │
│                                                                         │
│   core_bound = 4.8%                                                     │
│   → 执行单元基本，不是瓶颈                                              │
│                                                                         │
│ ─── Retiring 38.5% ───                                                  │
│   → ★ 40% 的有效利用率                                               │
│   → 如果能消除分支预测问题，Retiring 可提升到 ~70%         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 6.4 诊断结论

```
┌────────────────────────────────────────────────────┐
│ ★ 诊断结论：                                 │
│                                                    │
│ 问题根因：Bad Speculation → Branch Mispredict（39.8%）  
│   这是典型的"分支预测失败预测分支"案例：                  │
│   · 随机数据 + 条件分支 = 预测器失效              │
│   · 每次错误惩罚 15-20 周期                        │
│   · 40% 的 Pipeline Slot 被浪费                  │
│                                                    │
│ 优化方向：                                          │
│ 1. 排序数据 → 分支可预测（见场景 7）              │
│ 2. 使用 CMOV 消除分支：                            │
│    sum += (data[i] > threshold) ? data[i] : 0;     │
│    → 编译器可能生成 CMOV（无分支）                 │
│ 3. 使用分支无关代码：                              │
│    sum += data[i] & (-(data[i] > threshold));      │
│ 4. 查表法（适合条件多）                 │
└────────────────────────────────────────────────────┘
```

---

## 场景 7：优化前后对比 — 分支预测优化

### 7.1 对比实验

```bash
# 优化前：未排序数据（分支不可预测）
perf stat --topdown --td-level 1 -- ./branch_heavy

# 优化后：排序数据（分支可预测）
cat > /tmp/branch_sorted.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#define N (1<<24)
int cmp(const void *a, const void *b) { return *(int*)a - *(int*)b; }
int main() {
    int *data = malloc(N * sizeof(int));
    for(int i=0; i<N; i++) data[i] = rand();
    qsort(data, N, sizeof(int), cmp);  /* ★ 排序后分支可预测 */
    long sum = 0;
    for(int i=0; i<N; i++)
        if(data[i] > 0x40000000) sum += data[i];
    printf("sum=%ld\n", sum);
    free(data);
    return 0;
}
EOF
gcc -O2 -o /tmp/branch_sorted /tmp/branch_sorted.c
perf stat --topdown --td-level 1 -- /tmp/branch_sorted
```

### 7.2 输出对比 `[代表性输出]`

```
                    未排序（branch前）    排序后（优化后）     变化
                    ──────────────────  ──────────────────  ──────
retiring          38.5%              72.3%           +33.8% ★
frontend_bound      6.2%               3.8%            -2.4%
bad_speculation    41.3%               2.1%           -39.2% ★★★
backend_bound      14.0%              21.8%            +7.8%
```

### 7.3 分析

```
┌─────────────────────────────────────────────────────────────────────────┐
│ ★ 优化效果分析对分支改善：                                  │
│                                                                         │
│ 1. Bad Speculation: 41.3% → 2.1%（↓39.2%）                              │
│    → ★★ 排序后数据从小到大，前半段 `data[i] > 0x40000000` 为           │
│      后半段为真 → 分支预测器学习成功率 > 99%                         │
│                                                                         │
│ 2. Retiring: 38.5% → 72.3%（↑33.8%）                                    │
│    → ★★ 释放的 Slot 被有效退休填充                                      │
│    → IPC 从 1.54 → 2.89（提升 88%）                                    │
│                                                                         │
│ 3. Backend Bound: 14.0% → 21.8%（↑7.8%）                                │
│    → ★ Backend Bound 反而上升！                                         │
│    → 原因：前端不再"拖后腿"，后端成为新瓶颈                     │
│    → 这是正常的"瓶颈转移"现象                                  │
│    → 下一步应该优化 Backend Bound                                      │
│                                                                         │
│ ★ 关键教训：                                                            │
│   · 优化一个瓶颈后，其他瓶颈可能"浮现"                                   │
│   · 需要迭代分析：优化 → 重新采集 → 找新瓶颈 → 再优化                │
│   · 这就是 TMAM 的"Top-Down"思维——不断下钻直到满意                  │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 场景 8：编译优化级别对比

 topdown 的影响

### 8.1 对比

```bash
# 编译三个优化
gcc -O0 -o /tmp/matmul_O0 /tmp/matmul.c
gcc -O2 -o /tmp/matmul_O2 /tmp/matmul.c
gcc -O3 -o /tmp/matmul_O3 /tmp/matmul.c

# 分别运行 topdown
perf stat --topdown --td-level 1 -- /tmp/matmul_O0
perf stat --topdown --td-level 1 -- /tmp/matmul_O2
perf stat --topdown --td-level 1 -- /tmp/matmul_O3
```

### 8.2 输出对比 `[代表性输出]`

```
指标                  -O0            -O2            -O3           说明
────────────────────  ─────────────  ─────────────  ─────────────  ──────────
retiring              28.5%          65.2%          71.8%          ↑ ★★★
frontend_bound        12.3%           4.8%           5.1%          ↓
bad_speculation        5.1%           3.2%           2.8%          ↓
backend_bound         54.1%          26.8%          20.3%          ↓ ★★

IPC (计算值)          1.14           2.61           2.87           ↑ ★★★
```

### 8.3 分析

```
┌─────────────────────────────────────────────────────────────────────────┐
│ ★ 编译优化对 topdown 的影响分析                                         │
│                                                                         │
│ -O0（无优化）：                                                          │
│   · Retiring 仅 28.5% → IPC 1.14                                       │
│   · Backend Bound 高达 54.1%                                            │
│   → 原因：                                                              │
│     · 大量冗余 Load/Store（每次循环重新加载变量）                        │
│     · 无向量化（标量计算，每次处理 1 个元素）                             │
│     · 无循环展开（更多分支和依赖）                                        │
│     · Frontend Bound 也高（代码膨胀，更多 I-cache miss）                  │
│                                                                         │
│ -O2（标准优化）：                                                        │
│   · Retiring 65.2% → IPC 2.61                                          │
│   · 编译器优化：循环重排序、寄存器分配、部分循环展开                    │
│                                                                         │
│ -O3（激进优化）：                                                        │
│   · Retiring 71.8% → IPC 2.87（接近上限）                               │
│   · 额外优化：                                                          │
│     · ★ 自动向量化（SSE/AVX），每次处理 4-8 个 double                   │
│     · ★ 更激进的循环展开（减少分支）                                     │
│     · 更好的指令调度（减少依赖链）                                        │
│                                                                         │
│ ★ 结论：-O3 比 -O0 快 3-5 倍，主要归功于：                              │
│   1. 向量化 → 减少 Backend Bound（每周期更多有效 μop）                   │
│   2. 循环展开 → 减少 Bad Speculation（更少分支）                        │
│   3. 寄存器优化 → 减少 Memory Bound（更少 Load/Store）                   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 场景 9：使用 toplev.py 生成树状图

### 9.1 命令

```bash
# 使用 pmu-tools 的 toplev.py
toplev.py --core C0 -l2 --graph -- ./cpu_bound
```

### 9.2 输出 `[代表性输出]`

```
SKL Metrics for cpu_bound on Core C0

|Metric                            |   C0 |
|──────────────────────────────────│──────│
|FE                                │ 4.8% |
|FE.Fetch_Latency                  │ 3.5% |
|FE.Fetch_Bandwidth                │ 1.3% |
|BAD                               │ 3.2% |
|BAD.Branch_Mispredicts            │ 2.8% |
|BAD.Machine_Clears                │ 0.4% |
|BE                                │26.8% |
|BE.Memory_Bound                   │18.5% |
|BE.Core_Bound                     │ 8.3% |
|RET                               │65.2% |
|RET.Base                          │62.1% |
|RET.Microcode_Sequencer           │ 3.1% |
```

### 9.3 解读

```
┌─────────────────────────────────────────────────────────────────────────┐
│ toplev.py 的输出更加结构化：                                            │
│                                                                         │
│ · FE = Frontend Bound                                                   │
│ · BAD = Bad Speculation                                                 │
│ · BE = Backend Bound                                                    │
│ · RET = Retiring                                                        │
│                                                                         │
│ ★ toplev.py 的优势：                                                    │
│ 1. 自动缩进显示层级关系                                                 │
│ 2. 可指定特定 CPU 核心（--core C0）                                     │
│ 3. 更好的 multiplexing 管理（多轮采集）                                  │
│ 4. 支持 --threshold 过滤低占比节点                                      │
│ 5. 支持 --drilldown 自动下钻最大瓶颈                                    │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 场景 10：间隔打印（时间序列分析）

### 10.1 命令

```bash
# 每 1000ms 输出一次 topdown 数据
perf stat --topdown --td-level 1 -I 1000 -- ./long_running_program
```

### 10.2 输出 `[代表性输出]`

```
#           time            retiring frontend_bound bad_speculation backend_bound
     1.000456789               45.2%           12.3%            8.5%         34.0%
     2.001234567               62.1%            5.1%            3.8%         29.0%
     3.002345678               68.5%            4.2%            2.1%         25.2%
     4.003456789               70.1%            3.8%            1.9%         24.2%
     5.004567890               71.3%            3.5%            1.8%         23.4%
```

### 10.3 分析

```
┌─────────────────────────────────────────────────────────────────────────┐
│ ★ 时间序列分析揭示了程序的不同阶段：                                     │
│                                                                         │
│ 第 1 秒（初始化阶段）：                                                  │
│   · Retiring 仅 45.2%                                                   │
│   · Frontend Bound 高达 12.3%（初始化代码分散，I-cache 冷启动）         │
│   · Bad Speculation 8.5%（初始化分支不规律）                             │
│                                                                         │
│ 第 2-3 秒（预热阶段）：                                                  │
│   · Retiring 逐渐上升（62% → 68%）                                      │
│   · 缓存逐渐预热，分支预测器学习                                          │
│                                                                         │
│ 第 4-5 秒（稳态阶段）：                                                  │
│   · Retiring 稳定在 ~70%                                                │
│   · 各类瓶颈比例趋于稳定                                                 │
│   → ★ 这才是程序的"真实性能特征"                                        │
│                                                                         │
│ ★ 教训：                                                                │
│   · 不要只看整体平均值——可能掩盖阶段性差异                                │
│   · 使用 -I 参数观察性能随时间的变化                                     │
│   · 如果关注稳态性能，使用 --delay 跳过初始化阶段                        │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 场景 11：常见瓶颈的优化建议速查表

### 11.1 Frontend Bound 优化

| 子类 | 特征 | 优化方法 | 预期效果 |
|------|------|---------|---------|
| Fetch Latency (I-Cache) | 热代码 > 32KB | LTO（链接时优化）、减少热路径代码 | ↓ 50% |
| Fetch Latency (iTLB) | 代码页分散 | 使用 Huge Pages for text | ↓ 30% |
| Fetch Latency (Branch Resteer) | 分支预测错误后重取指 | 优化分支可预测性 | ↓ 间接 |
| Fetch Bandwidth (解码器) | 复杂指令多 | 用简单指令替代复杂指令 | ↓ 20% |
| Fetch Bandwidth (MS) | REP MOVSB 等微码指令 | 使用 memcpy 替代 | ↓ 80% |

### 11.2 Bad Speculation 优化

| 子类 | 特征 | 优化方法 | 预期效果 |
|------|------|---------|---------|
| Branch Mispredict | 不可预测分支 | 排序数据、CMOV、查表 | ↓ 80% ★ |
| Branch Mispredict | 间接跳转（虚函数） | 减少多态、使用 switch | ↓ 30% |
| Machine Clears | 内存排序违规 | 减少 lock 前缀使用 | ↓ 50% |
| Machine Clears | 自修改代码 | 避免 JIT修改代码段 | ↓ 90% |

### 11.3 Backend Bound — Memory Bound 优化

| 子类 | 特征 | 优化方法 | 预期效果 |
|------|------|---------|---------|
| L1 Bound | L1 数据缓存 miss 率重性差 | 分重块、AoS→SoA | ↓ 40%
| L2 Bound | L2 miss 率高 | 增大工作取、软件访问模式 | ↓ 30% |
| L3 Bound | L3 miss 率高 | 减少工作集、分友好访问 |
| DRAM Bound | DRAM 访问 延迟 | NUMA 亲和性、预 Huge Pages | ↓ 50% ★ |
| Store Bound | Store Buffer 满 | 减少写入合并、向量化 Store | ↓ 40% |

### 11.4 Backend Bound — Core Bound 优化

| 子类 | 特征 | 优化方法 | 预期效果 |
|------|------|---------|---------|
| Divider | 除法密集 | 乘以倒数、、用替代 | 70% ★ |
| Ports Utilization | ILP 不足 | 循环展开、软件流水线 | ↓ 40% |
| Long Dependency Chain | ence Chain | 长链过长 | 并行重构并行（减少） | ↓ 递归） | ↓ 60% |

---

## 场景 1：优化流程图示例

```
┌──────────────────────────────────────────────────────────────────────┐
│                TMAM 综合分析完整流程                                    │
│                                                                      │
│  Step 1: 采集                                                  │
│  │   perf stat --topdown --td-level 1 -- ./program                  │
│  │                                                              │
│  ▼                                                              │

│ Step 2: 找到最大瓶颈                                            │
│  │   ┌────────────┬─────────────┬──────────┬─────────┐ │
│  │   │ Retiring   │ Frontend    │ Bad Spec │ Backend │ │
│  │   │  65.2%    │  4.8%       │  3.2%    │ 26.8%   │ │
│  │   └────────────┴─────────────┴──────────┴─────────┘ │
│  │   → 最大项：Backend Bound (26.8%)                        │
│  │                                                                  │
│  ▼                                                                  │
│  Step 3: Level 2 下钻                                                │
│  │   perf stat --topdown --td-level 2 -- ./program                  │
│  │   Backend Bound:                                                  │
│  │     ├── Memory Bound: 18.5% ← ★ 主要瓶颈                  │
│  │     └── Core Bound:    8.3%                                      │
│  │                                                                  │
│  ▼                                                                  │
│  Step 4: 针对性优化                                              │
│  │   问题：Memory Bound（L1/L2/L3 miss 率高）                      │
│  │   方案：矩阵分块（cache tiling）                                  │
│  │                                                                  │
│  ▼                                                                  │
│  Step 5: 验证效果                                                  │
│  │   重新运行 Level 1 → 对比 Retiring% 变化                        │
│  │   预期：Retiring 65% → 75%                                      │
│  │                                                                  │
│  ▼                                                                  │
│  Step 6: 迭代                                                      │
│  │   如果新瓶颈浮现，继续 Top-Down 下钻                            │
│  │   直到 Retiring% 达到目标值                                     │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 附录：测试程序源码汇总

### A.1 cpu_bound.c

```c
/* cpu_bound.c — 300×300 矩阵乘法 */
#include <stdio.h>
#include <stdlib.h>
int main() {
    int n = 300;
    double *A = malloc(n*n*sizeof(double));
    double *B = malloc(n*n*sizeof(double));
    double *C = calloc(n*n, sizeof(double));
    for(int i=0;i<n*n;i++) { A[i]=1.0; B[i]=1.0; }
    for(int i=0;i<n;i++)
        for(int k=0;k<n;k++)
            for(int j=0;j<n;j++)
                C[i*n+j] += A[i*n+k]*B[k*n+j];
    printf("C[0]=%f\n", C[0]);
    free(A); free(B); free(C);
    return 0;
}
```

### A.2 mem_bound.c

```c
/* mem_bound.c — 链表随机遍历（pointer chasing） */
#include <stdio.h>
#include <stdlib.h>
#define N (1<<20)
int main() {
    int *next = malloc(N*sizeof(int));
    int *idx = malloc(N*sizeof(int));
    for(int i=0;i<N;i++) idx[i]=i;
    for(int i=N-1;i>0;i--) {
        int j=rand()%(i+1);
        int t=idx[i]; idx[i]=idx[j]; idx[j]=t;
    }
    for(int i=0;i<N-1;i++) next[idx[i]]=idx[i+1];
    next[idx[N-1]]=idx[0];
    int p = idx[0];
    long sum = 0;
    for(long i=0;i<(long)N*50;i++) { p=next[p]; sum+=p; }
    printf("sum=%ld\n", sum);
    free(next); free(idx);
    return 0;
}
```

### A.3 branch_heavy.c

```c
/* branch_heavy.c — 不可预测分支 */
#include <stdio.h>
#include <stdlib.h>
#define N (1<<24)
int main() {
    int *data = malloc(N*sizeof(int));
    for(int i=0;i<N;i++) data[i]=rand();
    long sum = 0;
    for(int i=0;i<N;i++) {
        if(data[i] > 0x40000000) sum += data[i];
    }
    printf("sum=%ld\n", sum);
    free(data);
    return 0;
}
```

### A.4 编译命令

```bash
gcc -O2 -o cpu_bound cpu_bound.c
gcc -O2 -o mem_bound mem_bound.c
gcc -O2 -o branch_heavy branch_heavy.c
```

---

## 附录 B：关键术语表

| 术语 | 英文 | 含义 |
|------|------|------|
| 流水线槽位 | Pipeline Slot | 每周期可退休的 μop 位置（Skylake = 4） |
| 微操作 | μop / Micro-Operation | CPU 内部执行的基本单元 |
| 退休 | Retire | 指令执行完成并更新架构状态 |
| 乱序执行 | Out-of-Order Execution | 不依赖的指令可以提前执行 |
| 重排序缓冲区 | ROB (Reorder Buffer) | 追踪乱序执行的指令顺序 |
| 分支预测 | Branch Prediction | 预测分支方向以提前取指和执行 |
| 流水线冲刷 | Pipeline Flush | 预测错误后清空流水线 |
|  |
| 多 | Frontend | 取指 + 解码 + 重命名阶段 后端 | Backend | 调度 + 执行 + 退休 |
| 内存墙 | Memory墙 | Memory Wall | CPU 计算与内存访问 |
| ILP | Instruction-Level Parallelism | 指令级并行度 |
| SMT | Simultaneous Multi-Th | 多线程（Intel 称 Hyper-Threading）线程） |
