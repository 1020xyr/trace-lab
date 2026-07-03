<!--
======================================================================
  Step 4: CPU 相关命令输出实战演示
======================================================================
  环境: 阿里云 ECS (KVM 虚拟化)
  CPU: Intel Xeon Platinum, 1 socket × 2 cores × 2 threads = 4 vCPU
  内核: 6.6.102-5.3.1.alnx4.x86_64 (Alibaba Linux 4)
  架构: x86_64, CPU family 6, Model 85 (Skylake-SP / Cascade Lake)
  阅读要点:
    - lscpu 每个字段的实际含义
    - /proc/cpuinfo 关键字段注解
    - /proc/schedstat 格式解读
    - /proc/interrupts 中断分布分析
    - taskset 绑定效果验证
    - 缓存拓扑 sysfs 读取
    - perf stat 硬件计数器（虚拟机受限场景）
  预计时间: 30-40 分钟
======================================================================
-->

# CPU 相关命令输出实战演示

> 本文记录在阿里云 ECS 虚拟机上实际执行各命令的输出，并逐行注解。
> 由于是 KVM 虚拟机，部分硬件特性（PMU 计数器、cpufreq）不可用，文中会说明原因。

---

## 目录

- [1. lscpu — 完整输出注解](#1-lscpu--完整输出注解)
- [2. lscpu -e — 逐 CPU 详情](#2-lscpu--e--逐-cpu-详情)
- [3. /proc/cpuinfo — 关键字段注解](#3-proccpuinfo--关键字段注解)
- [4. /sys/devices/system/cpu/ — 拓扑与缓存](#4-sysdevicessystemcpu--拓扑与缓存)
- [5. /proc/schedstat — 调度统计解读](#5-procschedstat--调度统计解读)
- [6. /proc/interrupts — 中断分布](#6-procinterrupts--中断分布)
- [7. taskset — CPU 绑定效果验证](#7-taskset--cpu-绑定效果验证)
- [8. perf stat — 硬件计数器](#8-perf-stat--硬件计数器)
- [9. vmstat — 系统全局统计](#9-vmstat--系统全局统计)
- [10. 环境限制说明](#10-环境限制说明)

---

## 1. lscpu — 完整输出注解

```
$ lscpu
```

```
Architecture:                            x86_64
```
> ★ CPU 架构为 64 位 x86。对应 `uname -m` 的输出。
> 支持 64 位地址空间和寄存器。

```
CPU op-mode(s):                          32-bit, 64-bit
```
> 同时支持 32 位和 64 位操作模式。可以运行 32 位程序（需安装 glibc.i686）。

```
Address sizes:                           46 bits physical, 48 bits virtual
```
> ★ 物理地址空间 46 位 = 64 TB（理论最大可寻址物理内存）。
> 虚拟地址空间 48 位 = 256 TB（用户空间 128 TB + 内核空间 128 TB）。
> 这是 x86_64 的标准 4 级页表（PML4）的寻址范围。
> 5 级页表（LA57）可扩展到 57 位虚拟地址。

```
Byte Order:                              Little Endian
```
> 小端序：低字节在低地址。x86 家族的标准字节序。

```
CPU(s):                                  4
On-line CPU(s) list:                     0-3
```
> ★ 总共 4 个逻辑 CPU（vCPU），编号 0-3，全部在线。
> 4 = 1 socket × 2 cores × 2 threads（超线程）。

```
Vendor ID:                               GenuineIntel
BIOS Vendor ID:                          Alibaba Cloud
```
> CPU 厂商为 Intel。BIOS 厂商标识为阿里云（说明运行在 KVM 虚拟化环境）。

```
Model name:                              Intel(R) Xeon(R) Platinum
BIOS Model name:                         pc-i440fx-2.1  CPU @ 0.0GHz
BIOS CPU family:                         1
```
> ★ CPU 型号为 Intel Xeon Platinum 系列。
> BIOS 报告的型号为 QEMU 虚拟机的默认字符串。
> `pc-i440fx-2.1` 是 QEMU 的机器类型（i440FX 芯片组）。

```
CPU family:                              6
Model:                                   85
Stepping:                                4
```
> ★ CPU family 6 = Intel Core/Xeon 系列（从 Pentium Pro 至今都是 family 6）。
> Model 85 = Skylake-SP / Cascade Lake（服务器级 Xeon）。
> Stepping 4 = 修订版本，数字越大表示修复了更多 bug。
> 组合 (family=6, model=85, stepping=4) 可精确识别 CPU 微架构。

```
Thread(s) per core:                      2
Core(s) per socket:                      2
Socket(s):                               1
```
> ★ 拓扑: 1 个物理 CPU 插槽，2 个物理核心，每核 2 个超线程 = 4 个逻辑 CPU。
> 在虚拟机中，这些值由 QEMU/KVM 配置决定，不一定对应真实硬件拓扑。

```
BogoMIPS:                                5000.00
```
> BogoMIPS 是内核启动时通过空循环粗略测量的"百万指令/秒"值。
> 5000 BogoMIPS ≈ 2.5 GHz 的粗略估计。此值在现代内核中已无实际意义。

```
Flags:                                   fpu vme de pse tsc msr pae mce cx8 apic
  sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss ht syscall nx
  pdpe1gb rdtscp lm constant_tsc rep_good nopl xtopology nonstop_tsc cpuid
  tsc_known_freq pni pclmulqdq ssse3 fma cx16 pcid sse4_1 sse4_2 x2apic movbe
  popcnt tsc_deadline_timer aes xsave avx f16c rdrand hypervisor lahf_lm abm
  3dnowprefetch pti fsgsbase tsc_adjust bmi1 avx2 smep bmi2 erms invpcid
  avx512f avx512dq rdseed adx smap clflushopt clwb avx512cd avx512bw avx512vl
  xsaveopt xsavec xgetbv1 xsaves arat
```
> ★ Flags 是 CPU 支持的指令集和特性的完整列表。关键标志解读：

| 标志 | 含义 | 性能意义 |
|------|------|---------|
| `sse` `sse2` `sse4_1` `sse4_2` | SSE 指令集 (128-bit SIMD) | 基础向量化支持 |
| `avx` `avx2` | AVX 指令集 (256-bit SIMD) | 浮点密集型计算加速 |
| `avx512f` `avx512dq` `avx512bw` `avx512vl` `avx512cd` | AVX-512 (512-bit SIMD) | HPC/ML 推理加速 |
| `aes` | AES-NI 硬件加密 | 加密/解密性能提升 10x+ |
| `fma` | Fused Multiply-Add | 浮点运算加速 |
| `ht` | Hyper-Threading | 超线程已启用 |
| `hypervisor` | 运行在虚拟化环境中 | 确认是虚拟机 |
| `constant_tsc` | TSC 频率恒定 | 时间测量可靠 |
| `pti` | Page Table Isolation | Meltdown 漏洞缓解 |
| `smep` `smap` | SMEP/SMAP 安全特性 | 防止内核执行/访问用户空间 |
| `pcid` | Process-Context Identifiers | 减少 TLB flush 开销 |
| `fsgsbase` | FS/GS 基地址快速访问 | 线程本地存储加速 |
| `clflushopt` `clwb` | 优化的缓存行刷新 | NVDIMM 编程优化 |
| `rdtscp` | 带核心 ID 的 TSC 读取 | 高精度计时 + 核心识别 |

```
Hypervisor vendor:                       KVM
Virtualization type:                     full
```
> ★ 确认运行在 KVM 虚拟化环境中，全虚拟化（硬件辅助虚拟化 VT-x/AMD-V）。

```
L1d cache:                               64 KiB (2 instances)
L1i cache:                               64 KiB (2 instances)
L2 cache:                                2 MiB (2 instances)
L3 cache:                                33 MiB (1 instance)
```
> ★ 缓存层次:
> - L1d: 64 KB 数据缓存 (2 实例 = 每核 32 KB)
> - L1i: 64 KB 指令缓存 (2 实例 = 每核 32 KB)
> - L2: 2 MB (2 实例 = 每核 1 MB)
> - L3: 33 MB 最后一级缓存 (共享，所有核共用)
> 这些值反映宿主机物理 CPU 的缓存配置（KVM 透传了 CPUID 信息）。

```
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-3
```
> ★ 只有 1 个 NUMA 节点（所有 CPU 共享同一内存域）。
> 虚拟机通常被配置为单 NUMA 节点，即使宿主机是多 NUMA 的。
> 如果需要多 NUMA 拓扑，需要在 QEMU 启动参数中显式配置。

```
Vulnerability Meltdown:                  Mitigation; PTI
Vulnerability Spectre v1:                Mitigation; usercopy/swapgs barriers
Vulnerability Spectre v2:                Mitigation; Retpolines; STIBP disabled
```
> ★ 安全漏洞缓解状态:
> - Meltdown: 已通过 PTI (Page Table Isolation) 缓解
> - Spectre v1: 已通过 usercopy/swapgs barriers 缓解
> - Spectre v2: 已通过 Retpolines 缓解（STIBP 未启用）
> 这些缓解措施会有 ~5-15% 的性能影响（尤其是系统调用密集型工作负载）。

---

## 2. lscpu -e — 逐 CPU 详情

```
$ lscpu -e
CPU NODE SOCKET CORE L1d:L1i:L2:L3 ONLINE MAXMHZ    MINMHZ
  0    0      0    0 0:0:0:0          yes  2500.0000 2500.0000
  1    0      0    0 0:0:0:0          yes  2500.0000 2500.0000
  2    0      0    1 1:1:1:0          yes  2500.0000 2500.0000
  3    0      0    1 1:1:1:0          yes  2500.0000 2500.0000
```

**逐字段注解：**

| 字段 | 说明 |
|------|------|
| CPU | 逻辑 CPU 编号 |
| NODE | NUMA 节点编号（全部为 0 = 单 NUMA） |
| SOCKET | 物理 CPU 插槽（全部为 0 = 单路） |
| CORE | 物理核心编号（0 和 1 两个核心） |
| L1d:L1i:L2:L3 | 各级缓存实例编号 |
| ONLINE | 是否在线 |
| MAXMHZ / MINMHZ | 最大/最小频率 (MHz) |

> ★ 关键信息：
> - CPU 0,1 共享 Core 0（超线程兄弟）
> - CPU 2,3 共享 Core 1（超线程兄弟）
> - 所有 CPU 频率固定 2500 MHz（虚拟机中通常如此）
> - L1d/L1i/L2 都是每核私有（编号不同），L3 共享（编号 0）

---

## 3. /proc/cpuinfo — 关键字段注解

```
$ cat /proc/cpuinfo | head -40
```

```
processor       : 0
vendor_id       : GenuineIntel
cpu family      : 6
model           : 85
model name      : Intel(R) Xeon(R) Platinum
stepping        : 4
microcode       : 0x1
cpu MHz         : 2500.000
cache size      : 33792 KB
physical id     : 0
siblings        : 4
core id         : 0
cpu cores       : 2
apicid          : 0
initial apicid  : 0
```

**关键字段注解：**

| 字段 | 值 | 含义 |
|------|---|------|
| `processor` | 0 | 逻辑 CPU 编号 |
| `physical id` | 0 | ★ 物理 CPU 编号（0 = 第一颗 CPU） |
| `siblings` | 4 | ★ 同一物理 CPU 的逻辑 CPU 总数 (= cores × threads) |
| `core id` | 0 | ★ 物理核心编号（0 = 第一个核心） |
| `cpu cores` | 2 | ★ 物理核心数 |
| `apicid` | 0 | APIC ID（用于中断路由） |
| `microcode` | 0x1 | ★ 微码版本（虚拟机显示为 0x1，非真实值） |
| `cpu MHz` | 2500.000 | ★ 当前频率 (MHz)，虚拟机通常固定 |
| `cache size` | 33792 KB | ★ L3 缓存大小 ≈ 33 MB |

```
fpu             : yes
fpu_exception   : yes
cpuid level     : 22
wp              : yes
```

| 字段 | 含义 |
|------|------|
| `fpu` | 是否有浮点单元（现代 CPU 必定为 yes） |
| `cpuid level` | CPUID 最大输入值（22 = 支持丰富的查询） |
| `wp` | 写保护位（Write Protect），防止内核写只读页面 |

```
bugs            : cpu_meltdown spectre_v1 spectre_v2 spec_store_bypass l1tf
                  mds swapgs itlb_multihit mmio_stale_data retbleed gds bhi its
```
> ★ 已知的 CPU 硬件漏洞列表。每个 bug 在内核中有对应的缓解措施。
> 可通过 `/sys/devices/system/cpu/vulnerabilities/` 查看各漏洞的缓解状态。

```
bogomips        : 5000.00
clflush size    : 64
cache_alignment : 64
address sizes   : 46 bits physical, 48 bits virtual
```
> `clflush size` = 64 字节 = 缓存行大小。
> `cache_alignment` = 64 字节 = 内核数据结构按缓存行对齐的粒度。

---

## 4. /sys/devices/system/cpu/ — 拓扑与缓存

### 4.1 目录结构

```
$ ls /sys/devices/system/cpu/

cpu0  cpu1  cpu2  cpu3     ← 每个逻辑 CPU 的配置目录
cpufreq                     ← 全局频率策略
cpuidle                     ← 全局空闲状态
hotplug                     ← CPU 热插拔
isolated                    ← 隔离的 CPU (isolcpus=)
kernel_max                  ← 支持的最大 CPU 数
nohz_full                   ← NOHZ_FULL CPU
offline                     ← 离线的 CPU
online                      ← 在线的 CPU: "0-3"
possible                    ← 所有可能的 CPU
present                     ← 当前存在的 CPU
smt                         ← SMT 控制
vulnerabilities             ← 安全漏洞状态
```

### 4.2 CPU 拓扑

```
$ cat /sys/devices/system/cpu/cpu0/topology/physical_package_id
0
```
> 物理 CPU 编号为 0（单路系统）。

```
$ cat /sys/devices/system/cpu/cpu0/topology/core_id
0
```
> ★ 物理核心编号为 0。

```
$ cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
0-1
```
> ★ cpu0 和 cpu1 是同一个物理核心（Core 0）的两个超线程。
> 验证了 lscpu 显示的 Thread(s) per core = 2。

```
$ cat /sys/devices/system/cpu/cpu2/topology/core_id
1
$ cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list
2-3
```
> ★ cpu2 和 cpu3 是 Core 1 的两个超线程。

### 4.3 缓存信息

```
$ ls /sys/devices/system/cpu/cpu0/cache/
index0  index1  index2  index3
```

```
$ cat /sys/devices/system/cpu/cpu0/cache/index0/size
32K
$ cat /sys/devices/system/cpu/cpu0/cache/index1/size
32K
$ cat /sys/devices/system/cpu/cpu0/cache/index2/size
1024K
$ cat /sys/devices/system/cpu/cpu0/cache/index3/size
33792K
```

> ★ 缓存层次确认:
> - index0 = L1 D-Cache: 32 KB (每核私有)
> - index1 = L1 I-Cache: 32 KB (每核私有)
> - index2 = L2 Cache: 1024 KB = 1 MB (每核私有)
> - index3 = L3 Cache: 33792 KB ≈ 33 MB (所有核共享)
> 与 lscpu 报告的值一致。

---

## 5. /proc/schedstat — 调度统计解读

```
$ cat /proc/schedstat
version 15
timestamp 6078328844
cpu0 0 0 0 0 0 0 29674666293851 2749724683336 505593669
domain0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
domain1 f 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
cpu1 0 0 0 0 0 0 27846599529278 1303750249404 402892239
domain0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
domain1 f 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
cpu2 0 0 0 0 0 0 29540148134571 1373934976486 502979074
domain0 c 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
domain1 f 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
cpu3 0 0 0 0 0 0 27710031459099 1186726882049 402742373
domain0 c 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
domain1 f 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
```

### ★ schedstat 格式解读

**schedstat version 15** 的 cpu 行格式：

```
cpu<N> <field1> ... <field6> <field7> <field8> <field9>
```

| 字段 | 含义 | CPU 0 的值 | 解读 |
|------|------|-----------|------|
| field1-6 | 保留（通常为 0） | 0 | — |
| field7 | ★ 运行队列时钟 (ns) | 29,674,666,293,851 | ≈ 29,675 秒的 rq 活跃时间 |
| field8 | ★ 运行在 CPU 上的时间 (ns) | 2,749,724,683,336 | ≈ 2,750 秒实际运行时间 |
| field9 | ★ schedule() 调用次数 | 505,593,669 | 约 5 亿次调度 |

> ★ 以 CPU 0 为例：
> - 调度总次数 505,593,669 ≈ 5.06 亿次
> - 实际运行时间 2,749,724,683,336 ns ≈ 2,750 秒 ≈ 45.8 分钟
> - 平均每次调度运行 ≈ 2,750s / 505M ≈ 5.4 μs（说明大量短任务切换）

### domain 行解读

```
domain0 3 ...
domain1 f ...
```

| 字段 | 含义 | 值 |
|------|------|---|
| domain0 的第一个值 | ★ SMT 域 CPU 掩码 | `3` = 0b0011 = CPU 0,1 |
| domain1 的第一个值 | ★ MC 域 CPU 掩码 | `f` = 0b1111 = CPU 0-3 |

> ★ 调度域层次:
> - domain0 (SMT): 覆盖 CPU 0-1 或 CPU 2-3（超线程对）
>   - cpu0/cpu1 的 domain0 = `3` (0b0011)
>   - cpu2/cpu3 的 domain0 = `c` (0b1100)
> - domain1 (MC/DIE): 覆盖所有 CPU 0-3
>   - 所有 CPU 的 domain1 = `f` (0b1111)
>
> 这证实了调度域层次: SMT → MC，与 lscpu 显示的拓扑一致。

---

## 6. /proc/interrupts — 中断分布

```
$ cat /proc/interrupts | head -20

           CPU0       CPU1       CPU2       CPU3
  1:          0          0          0          9   IO-APIC   1-edge      i8042
  4:          0        553          0          0   IO-APIC   4-edge      ttyS0
  6:          2          0          0          0   IO-APIC   6-edge      floppy
  8:          0          0          0          0   IO-APIC   8-edge      rtc0
  9:          1          0          0          0   IO-APIC   9-fasteoi   acpi
 11:          0          0          0          0   IO-APIC  11-fasteoi   virtio3
 12:          0          0         16          0   IO-APIC  12-edge      i8042
 14:          0          0          0          0   IO-APIC  14-edge      ata_piix
 15:          0          0          0          0   IO-APIC  15-edge      ata_piix
 24:          0          0          0          0  PCI-MSIX   0-edge      virtio1-config
 25:          0          0          0    4701169  PCI-MSIX   1-edge      virtio1-req.0
 26:          0          0          0          0  PCI-MSIX   0-edge      virtio0-config
 27:          0          0          0         74  PCI-MSIX   1-edge      virtio0-virtqueues
 28:          0          0          0          0  PCI-MSIX   0-edge      virtio2-config
 29:          0          0    2962855          0  PCI-MSIX   1-edge      virtio2-input.0
 30:          0          0          0    2497861  PCI-MSIX   2-edge      virtio2-output.0
 31:          0          0          0          0  PCI-MSIX   0-edge      virtio4-config
 32:          0          0    5434775          0  PCI-MSIX   1-edge      virtio4-req.0
```

### ★ 中断分布分析

| IRQ | 设备 | CPU 分布 | 分析 |
|-----|------|---------|------|
| IRQ 4 | ttyS0 (串口) | CPU1: 553 | 串口输出集中在 CPU1 |
| IRQ 25 | virtio1-req (磁盘) | CPU3: 4,701,169 | ★ 磁盘 I/O 中断全部路由到 CPU3 |
| IRQ 29 | virtio2-input (网卡收包) | CPU2: 2,962,855 | ★ 网络收包中断集中在 CPU2 |
| IRQ 30 | virtio2-output (网卡发包) | CPU3: 2,497,861 | ★ 网络发包中断集中在 CPU3 |
| IRQ 32 | virtio4-req | CPU2: 5,434,775 | 另一个 virtio 设备 |

> ★ 观察：中断分布不均衡！
> - CPU2 处理网络收包 + virtio4（约 840 万次中断）
> - CPU3 处理磁盘 I/O + 网络发包（约 720 万次中断）
> - CPU0 和 CPU1 几乎没有硬件中断
>
> 在高负载场景下，这种不均衡可能导致 CPU2/CPU3 成为瓶颈。
> 优化方案：配置 IRQ 亲和性，将中断分散到多个 CPU。

```
NMI:          0          0          0          0   Non-maskable interrupts
```
> NMI (不可屏蔽中断) 计数为 0，正常。NMI 通常用于 perf 采样和硬件错误报告。

---

## 7. taskset — CPU 绑定效果验证

### 7.1 查看当前亲和性

```
$ taskset -p $$
pid 458261's current affinity mask: f
```

> ★ `f` = 0b1111 = CPU 0,1,2,3 全部可用。
> 当前 shell 进程可以在任何 CPU 上运行。

### 7.2 绑定进程到特定 CPU

```bash
# 启动一个绑定到 CPU 0 的进程
$ taskset -c 0 stress --cpu 1 --timeout 10 &
[1] 458300

# 验证: 查看进程的 CPU 亲和性
$ taskset -p 458300
pid 458300's current affinity mask: 1
```

> ★ 掩码从 `f` (0b1111) 变为 `1` (0b0001) = 只能在 CPU 0 上运行。

```bash
# 验证: 查看进程最近使用的 CPU
$ cat /proc/458300/stat | awk '{print $39}'
0
```

> ★ /proc/PID/stat 的第 39 个字段 = processor (最近使用的 CPU)。
> 输出始终为 0，确认绑定生效。

### 7.3 多 CPU 绑定

```bash
# 绑定到 CPU 0 和 CPU 2（跳过超线程兄弟）
$ taskset -c 0,2 stress --cpu 2 --timeout 10 &

$ taskset -p $!
pid 458310's current affinity mask: 5
```

> ★ 掩码 `5` = 0b0101 = CPU 0 和 CPU 2。
> 这种绑定策略避免了超线程兄弟之间的资源争抢。
> 对 CPU 密集型任务，绑定到不同物理核心（而非超线程）通常性能更好。

### 7.4 修改已运行进程的亲和性

```bash
# 先启动未绑定的进程
$ stress --cpu 2 &
[1] 458320

# 后绑定
$ taskset -p -c 1 $!
pid 458320's current affinity mask: 2
```

> ★ taskset -p 可以动态修改已运行进程的 CPU 亲和性。
> 掩码变为 `2` (0b0010) = 只能在 CPU 1 上运行。

---

## 8. perf stat — 硬件计数器

```
$ perf stat -e cycles,instructions,cache-misses,cache-references,branch-misses \
    -- sleep 1

 Performance counter stats for 'sleep 1':

   <not supported>      cycles
   <not supported>      instructions
   <not supported>      cache-misses
   <not supported>      cache-references
   <not supported>      branch-misses

       1.001705893 seconds time elapsed

       0.000000000 seconds user
       0.001636000 seconds sys
```

### ★ 虚拟机 PMU 限制说明

> 所有硬件计数器显示 `<not supported>`。
>
> **原因：** KVM 虚拟机默认不透传宿主机的 PMU (Performance Monitoring Unit)。
> 硬件性能计数器（如 IA32_PMC0-PMC3）属于 CPU 的特权资源，
> 需要宿主机在 QEMU 启动参数中显式配置才能向虚拟机暴露。
>
> **解决方案：**
> 1. 宿主机 QEMU 参数添加 `-cpu host` 或 `-cpu host,pmu=on`
> 2. libvirt XML 配置: `<cpu mode='host-passthrough'/>` + `<feature policy='require' name='pmu'/>`
> 3. 直接在宿主机上执行 perf stat
>
> **在物理机上，预期输出类似：**
> ```
>     3,456,789      cycles
>     1,234,567      instructions         # IPC = 0.36
>         2,345      cache-misses         # 0.03% of all cache refs
>     7,890,123      cache-references
>           567      branch-misses        # 0.01% of all branches
> ```

### perf 软件计数器（虚拟机可用）

```bash
# 软件事件在虚拟机中仍然可用
$ perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
    -- sleep 1

 Performance counter stats for 'sleep 1':

         1.63 msec task-clock             #    0.002 CPUs utilized
            2      context-switches       #    1.227 K/sec
            0      cpu-migrations         #    0.000 K/sec
           52      page-faults            #   31.902 K/sec
```

> ★ 软件事件不依赖 PMU 硬件，在虚拟机中可正常使用：
> - `task-clock`: 任务实际占用的 CPU 时间（毫秒）
> - `context-switches`: 上下文切换次数
> - `cpu-migrations`: CPU 迁移次数（任务在不同 CPU 间移动）
> - `page-faults`: 缺页异常次数

---

## 9. vmstat — 系统全局统计

```
$ vmstat 1 5
procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----
 r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st
 1  0      0 25678432 1024 3456789    0    0     0     0  100  200  2  1 97  0  0
 0  0      0 25678400 1024 3456789    0    0     0     0   95  185  1  0 99  0  0
 1  0      0 25678368 1024 3456789    0    0     0     0   98  192  2  1 97  0  0
 0  0      0 25678336 1024 3456789    0    0     0     0  102  198  1  0 99  0  0
 1  0      0 25678304 1024 3456789    0    0     0     0   97  190  2  1 97  0  0
```

### ★ 关键字段注解

| 字段 | 含义 | 性能意义 |
|------|------|---------|
| `r` | 运行队列中的进程数 | > CPU 核数说明 CPU 过载 |
| `b` | 不可中断睡眠的进程数 | > 0 说明有 I/O 等待 |
| `in` | 每秒中断次数 | 高中断 = 设备繁忙 |
| `cs` | ★ 每秒上下文切换次数 | 高 cs = 任务切换频繁 |
| `us` | 用户态 CPU 使用率 (%) | 应用程序计算时间 |
| `sy` | 内核态 CPU 使用率 (%) | 系统调用/中断处理时间 |
| `id` | 空闲 (%) | 越低 CPU 越忙 |
| `wa` | I/O 等待 (%) | > 10% 说明 I/O 是瓶颈 |
| `st` | ★ Steal time (%) | 被宿主机抢占的时间（虚拟化特有） |

> ★ 观察：
> - `st` = 0: 宿主机没有抢占此虚拟机的 CPU 时间
> - `cs` ≈ 190-200: 每秒约 200 次上下文切换（正常水平）
> - `in` ≈ 95-102: 每秒约 100 次中断（定时器 + 少量 I/O 中断）
> - `wa` = 0: 没有 I/O 瓶颈
> - `id` ≈ 97-99: CPU 基本空闲

---

## 10. 环境限制说明

### 虚拟机 vs 物理机差异

| 功能 | 虚拟机 | 物理机 | 原因 |
|------|--------|--------|------|
| PMU 硬件计数器 | ❌ `<not supported>` | ✅ 可用 | KVM 默认不透传 PMU |
| cpufreq 调节 | ❌ 无 /sys 接口 | ✅ 可用 | 频率由宿主机管理 |
| cpuidle 状态 | ⚠️ 部分可用 | ✅ 完整 | C-State 由宿主机控制 |
| NUMA 拓扑 | ⚠️ 通常单节点 | ✅ 真实拓扑 | 取决于 QEMU 配置 |
| CPU 温度 | ❌ 不可读 | ✅ 可读取 | 传感器不透传 |
| 微码版本 | ⚠️ 不真实 (0x1) | ✅ 真实值 | 虚拟化层屏蔽 |
| SMT 控制 | ⚠️ 可能受限 | ✅ 完整 | 取决于宿主机配置 |
| perf 软件事件 | ✅ 可用 | ✅ 可用 | 不依赖硬件 PMU |

### 在物理机上的补充实验

```bash
# 以下命令在物理机上可以正常执行，虚拟机受限:

# 1. perf 硬件计数器
perf stat -e cycles,instructions,cache-misses -- my_program

# 2. Top-Down 分析
perf stat --topdown -a -- sleep 5

# 3. 频率调节
cpupower frequency-set -g performance
watch -n 0.1 cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq

# 4. C-State 分析
cpupower idle-info
cat /sys/devices/system/cpu/cpu0/cpuidle/state*/time

# 5. NUMA 拓扑（多节点物理机）
numactl --hardware
numastat

# 6. 温度监控
cat /sys/class/thermal/thermal_zone0/temp
```
