<!--
======================================================================
  Step 3: CPU 相关命令参考手册
======================================================================
  来源: man pages + 实际使用经验
  阅读要点:
    - lscpu / nproc / lstopo 查看 CPU 拓扑
    - taskset / numactl 绑定 CPU 和 NUMA 节点
    - cpufreq-info / cpupower 频率调节
    - /proc/cpuinfo 和 /sys/devices/system/cpu/ 解读
    - perf stat 硬件计数器收集
    - 10+ 个 CPU 调优实战场景
  预计时间: 30-40 分钟
======================================================================
-->

# CPU 相关命令参考手册

> 本文汇总日常工作中最常用的 CPU 查看、绑定和调优命令。
> 每个命令附带用法说明、关键参数和典型使用场景。

---

## 目录

- [1. lscpu — CPU 架构信息](#1-lscpu--cpu-架构信息)
- [2. nproc — 在线 CPU 数量](#2-nproc--在线-cpu-数量)
- [3. lstopo — 硬件拓扑可视化](#3-lstopo--硬件拓扑可视化)
- [4. /proc/cpuinfo — CPU 详细信息](#4-proccpuinfo--cpu-详细信息)
- [5. /sys/devices/system/cpu/ — CPU sysfs 接口](#5-sysdevicessystemcpu--cpu-sysfs-接口)
- [6. taskset — CPU 亲和性绑定](#6-taskset--cpu-亲和性绑定)
- [7. numactl — NUMA 控制](#7-numactl--numa-控制)
- [8. cpupower — CPU 频率/电源管理](#8-cpupower--cpu-频率电源管理)
- [9. perf stat — 硬件性能计数器](#9-perf-stat--硬件性能计数器)
- [10. 其他常用命令](#10-其他常用命令)
- [11. CPU 调优实战场景](#11-cpu-调优实战场景)

---

## 1. lscpu — CPU 架构信息

### 基本用法

```bash
# 完整输出
lscpu

# 仅显示可执行 JSON 格式（脚本友好）
lscpu -J

# 扩展格式（显示每个 CPU 的详细信息）
lscpu -e

# 仅显示在线 CPU
lscpu -e --online

# 解析模式（键值对，适合脚本）
lscpu -p
```

### ★ 关键字段解读

| 字段 | 含义 | 性能意义 |
|------|------|---------|
| Architecture | CPU 架构 (x86_64/aarch64) | 决定指令集和内存模型 |
| CPU(s) | 总逻辑 CPU 数 (= sockets × cores × threads) | 总并行度 |
| Thread(s) per core | 每核超线程数 | >1 表示启用 SMT |
| Core(s) per socket | 每物理 CPU 核心数 | 物理核心数 |
| Socket(s) | 物理 CPU 插槽数 | 多路系统 > 1 |
| NUMA node(s) | NUMA 节点数 | >1 表示 NUMA 架构 |
| L1d/L1i cache | L1 数据/指令缓存大小 | 热点工作集大小参考 |
| L2/L3 cache | L2/L3 缓存大小 | LLC miss 代价分析 |
| Flags | CPU 特性标志 | 确认 AVX/SSE/AES 等支持 |
| Vulnerability | 安全漏洞缓解状态 | 了解 Spectre/Meltdown 补丁影响 |

### 扩展输出

```bash
# 每个 CPU 的详细信息
lscpu -e
# 输出列: CPU NODE SOCKET CORE L1d:L1i:L2:L3 ONLINE MAXMHZ MINMHZ
```

---

## 2. nproc — 在线 CPU 数量

```bash
# 输出可用 CPU 数量
nproc
# 等价于: grep -c ^processor /proc/cpuinfo

# 注意: nproc 返回的是当前在线的 CPU 数
# 可能小于物理核心数 (如果某些核心被离线)
# 也可能大于物理核心数 (如果启用了 SMT)
```

---

## 3. lstopo — 硬件拓扑可视化

```bash
# 安装: yum install hwloc / apt install hwloc

# 图形化显示拓扑 (X11)
lstopo

# 文本模式
lstopo-no-graphics

# 仅显示核心
lstopo-no-graphics --only core

# 仅显示 NUMA 节点
lstopo-no-graphics --only numa

# 输出为 PNG
lstopo topology.png

# 显示 CPU 绑定信息
lstopo-no-graphics --cpuset
```

### 输出示例（简化）

```
  Machine (31GB total)
    Package L#0
      NUMANode L#0 (P#0 31GB)
      L3 (33MB)
        L2 (1024KB) + L1d (32KB) + L1i (32KB) + Core L#0
          PU L#0 (P#0)    ← 逻辑 CPU 0
          PU L#1 (P#1)    ← 逻辑 CPU 1 (与 CPU 0 共享核心)
        L2 (1024KB) + L1d (32KB) + L1i (32KB) + Core L#1
          PU L#2 (P#2)    ← 逻辑 CPU 2
          PU L#3 (P#3)    ← 逻辑 CPU 3 (与 CPU 2 共享核心)
```

---

## 4. /proc/cpuinfo — CPU 详细信息

### ★ 关键字段

```bash
cat /proc/cpuinfo
```

| 字段 | 含义 | 示例 |
|------|------|------|
| processor | 逻辑 CPU 编号 | 0, 1, 2, 3 |
| vendor_id | CPU 厂商 | GenuineIntel / AuthenticAMD |
| cpu family | CPU 系列 | 6 (Intel Core/Xeon) |
| model | CPU 型号编号 | 85 (Skylake-SP) |
| model name | CPU 名称 | Intel(R) Xeon(R) Platinum |
| stepping | 修订版本号 | 4 |
| microcode | 微码版本 | 0x1 |
| cpu MHz | 当前频率 (MHz) | 2500.000 |
| cache size | L3 缓存大小 | 33792 KB |
| physical id | 物理 CPU 编号 | 0 |
| siblings | 同一物理 CPU 的逻辑 CPU 数 | 4 |
| core id | 物理核心编号 | 0 |
| cpu cores | 物理核心数 | 2 |
| apicid | APIC ID | 0 |
| flags | CPU 特性标志 | sse sse2 avx avx2 avx512f ... |
| bugs | 已知硬件 bug | spectre_v1 spectre_v2 meltdown |
| bogomips | BogoMIPS (内核启动时计算) | 5000.00 |

### 常用过滤命令

```bash
# 查看 CPU 型号（仅一行）
grep "model name" /proc/cpuinfo | head -1

# 查看每个 CPU 的核心归属
grep -E "processor|core id|physical id" /proc/cpuinfo

# 查看特定 CPU 特性
grep -o "avx512[a-z]*" /proc/cpuinfo | sort -u

# 统计物理核心数（去重）
grep "core id" /proc/cpuinfo | sort -u | wc -l
```

---

## 5. /sys/devices/system/cpu/ — CPU sysfs 接口

### ★ 目录结构

```bash
ls /sys/devices/system/cpu/
```

| 路径 | 含义 |
|------|------|
| `cpu0/`, `cpu1/`, ... | 每个 CPU 的配置目录 |
| `cpufreq/` | 全局 cpufreq 策略 |
| `cpuidle/` | 全局 cpuidle 状态 |
| `online` | 在线 CPU 列表 (如 "0-3") |
| `possible` | 所有可能的 CPU |
| `present` | 当前存在的 CPU |
| `isolated` | 隔离的 CPU (isolcpus= 内核参数) |
| `nohz_full` | NOHZ_FULL CPU (nohz_full= 内核参数) |
| `smt/` | SMT 控制目录 |
| `vulnerabilities/` | 安全漏洞缓解状态 |

### 每个 CPU 的子目录

```bash
ls /sys/devices/system/cpu/cpu0/
```

| 子目录/文件 | 含义 |
|------------|------|
| `topology/` | CPU 拓扑信息 |
| `cache/` | 各级缓存参数 |
| `cpufreq/` | 频率调节参数 |
| `cpuidle/` | 空闲状态信息 |
| `online` | 此 CPU 是否在线 |

### ★ topology 目录

```bash
# 物理 CPU 编号
cat /sys/devices/system/cpu/cpu0/topology/physical_package_id

# 物理核心编号
cat /sys/devices/system/cpu/cpu0/topology/core_id

# 超线程兄弟列表
cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
# 输出: "0-1" 表示 cpu0 和 cpu1 是同一核心的两个超线程

# 同 package 的核心列表
cat /sys/devices/system/cpu/cpu0/topology/core_siblings_list
```

### ★ cache 目录

```bash
# 各级缓存目录
ls /sys/devices/system/cpu/cpu0/cache/
# index0 = L1d, index1 = L1i, index2 = L2, index3 = L3

# 查看缓存大小
cat /sys/devices/system/cpu/cpu0/cache/index0/size   # L1d: 32K
cat /sys/devices/system/cpu/cpu0/cache/index1/size   # L1i: 32K
cat /sys/devices/system/cpu/cpu0/cache/index2/size   # L2: 1024K
cat /sys/devices/system/cpu/cpu0/cache/index3/size   # L3: 33792K

# 查看缓存关联度和行大小
cat /sys/devices/system/cpu/cpu0/cache/index0/ways_of_associativity
cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size
```

---

## 6. taskset — CPU 亲和性绑定

### ★ 基本用法

```bash
# 查看当前 shell 的 CPU 亲和性
taskset -p $$
# 输出: pid XXXX's current affinity mask: f
# f = 0b1111 = 所有 4 个 CPU

# 以十六进制掩码设置亲和性
taskset 0x3 my_program     # 只能在 CPU 0,1 上运行
# 0x3 = 0b0011

# 以 CPU 列表设置亲和性 (更易读)
taskset -c 0,2 my_program  # 只能在 CPU 0,2 上运行
taskset -c 0-3 my_program  # CPU 0-3

# 修改已运行进程的亲和性
taskset -p -c 0,1 <PID>    # 将 PID 绑定到 CPU 0,1
taskset -p 0x3 <PID>       # 等价写法
```

### 掩码计算

```
  CPU 0: bit 0 → 0x1
  CPU 1: bit 1 → 0x2
  CPU 2: bit 2 → 0x4
  CPU 3: bit 3 → 0x8

  组合:
  CPU 0+1 = 0x3
  CPU 0+2 = 0x5
  CPU 0+1+2+3 = 0xF
  CPU 2+3 = 0xC
```

### 验证绑定效果

```bash
# 启动绑定进程
taskset -c 0 stress --cpu 1 &
PID=$!

# 验证: 查看进程最近使用的 CPU
cat /proc/$PID/stat | awk '{print $39}'
# 字段 39 = processor (最近使用的 CPU 编号)
# 应该始终为 0

# 也可以用 taskset 查看
taskset -p $PID
# 输出: affinity mask: 1 (只有 CPU 0)
```

---

## 7. numactl — NUMA 控制

### ★ 基本用法

```bash
# 安装: yum install numactl

# 查看 NUMA 硬件拓扑
numactl --hardware
# 显示每个 NUMA 节点的 CPU、内存、距离矩阵

# 绑定到指定 NUMA 节点运行
numactl --cpunodebind=0 --membind=0 my_program

# 仅绑定内存（CPU 可跨节点调度）
numactl --membind=0 my_program

# 内存交叉分配
numactl --interleave=all my_program

# 优先在指定节点分配内存
numactl --preferred=0 my_program

# 查看进程的 NUMA 状态
numastat -p <PID>
```

### numactl --hardware 输出解读

```
  available: 2 nodes (0-1)        ← 2 个 NUMA 节点
  node 0 cpus: 0 1 2 3            ← Node 0 的 CPU
  node 0 size: 16384 MB           ← Node 0 的内存
  node 0 free: 12288 MB           ← Node 0 的可用内存
  node 1 cpus: 4 5 6 7            ← Node 1 的 CPU
  node 1 size: 16384 MB
  node 1 free: 10240 MB
  node distances:                  ← ★ NUMA 距离矩阵
  node   0   1
    0:  10  20                    ← Node 0 到自身=10, 到 Node 1=20
    1:  20  10                    ← 远程访问延迟约为本地的 2 倍
```

---

## 8. cpupower — CPU 频率/电源管理

### 基本用法

```bash
# 安装: yum install kernel-tools / apt install linux-tools-common

# 查看当前频率信息
cpupower frequency-info

# 查看所有 CPU 当前频率
cpupower frequency-info --human

# 设置 governor
cpupower frequency-set -g performance    # 最高频率
cpupower frequency-set -g powersave      # 最低频率
cpupower frequency-set -g ondemand       # 按需调节
cpupower frequency-set -g schedutil      # 基于调度器

# 设置固定频率
cpupower frequency-set -f 2500MHz

# 查看 idle 状态
cpupower idle-info

# 设置 C-State 最大延迟约束
cpupower idle-set -d 2    # 禁止进入 C3 及以上的 C-State
```

### /sys 接口直接操作

```bash
# 查看当前 governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# 查看可用 governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors

# 查看当前频率 (KHz)
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq

# 查看可用频率范围
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq

# 临时设置
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

---

## 9. perf stat — 硬件性能计数器

### ★ 基本用法

```bash
# 基础计数器
perf stat -e cycles,instructions my_program

# 缓存事件
perf stat -e cache-misses,cache-references,\
  L1-dcache-loads,L1-dcache-load-misses my_program

# 分支事件
perf stat -e branch-misses,branch-instructions my_program

# TLB 事件
perf stat -e dTLB-load-misses,iTLB-load-misses my_program

# Top-Down 分析 (Level 1)
perf stat --topdown -a -- sleep 5

# Top-Down 分析 (Level 2, 需要更多计数器)
perf stat --topdown --topdown-level=2 -a -- sleep 5

# 系统范围采样
perf stat -a -e cycles,instructions -- sleep 5

# 特定 CPU
perf stat -C 0,1 -e cycles -- sleep 5
```

### ★ 关键指标解读

| 指标 | 含义 | 健康范围 |
|------|------|---------|
| IPC (instructions/cycles) | 每周期指令数 | >1.0 良好, <0.5 需优化 |
| cache-miss ratio | 缓存缺失率 | <5% 良好, >20% 需优化 |
| branch-miss ratio | 分支预测失败率 | <3% 良好, >10% 需优化 |
| MPKI (misses per K instructions) | 每千条指令的缺失数 | 越小越好 |
| frontend bound | 前端瓶颈占比 | <20% 良好 |
| backend bound | 后端瓶颈占比 | <30% 良好 |

---

## 10. 其他常用命令

### 调度统计

```bash
# 调度统计信息
cat /proc/schedstat

# 每个任务的调度信息
cat /proc/<PID>/sched

# 中断分布
cat /proc/interrupts

# 上下文切换频率
vmstat 1
# cs 列 = 每秒上下文切换次数

# 软中断统计
cat /proc/softirqs

# 内核线程 CPU 使用
pidstat -u 1

# 每个 CPU 的使用率
mpstat -P ALL 1
```

### 进程 CPU 绑定查看

```bash
# 查看进程的 CPU 亲和性
taskset -p <PID>

# 查看进程最近使用的 CPU
cat /proc/<PID>/stat | awk '{print $39}'

# 查看进程的 NUMA 页面分布
numastat -p <PID>

# 查看进程在哪些 CPU 上运行过
perf sched record -- <command>
perf sched map
```

---

## 11. CPU 调优实战场景

### 场景 1: 数据库延迟优化

```bash
# 问题: MySQL 响应延迟高，抖动大
# 排查:

# 1. 检查 CPU 频率是否稳定
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
# 如果是 ondemand → 改为 performance
cpupower frequency-set -g performance

# 2. 检查是否有 CPU 迁移
perf stat -e cs,migrations -a -- sleep 10
# migrations 过高说明任务频繁在 CPU 间迁移

# 3. 绑定数据库线程到固定 CPU
taskset -c 0-7 mysqld ...

# 4. 隔离 CPU 给数据库使用 (需要重启)
# 内核参数: isolcpus=8-15
```

### 场景 2: 高并发 Web 服务

```bash
# 问题: Nginx worker 争抢 CPU，吞吐量不稳定

# 1. 查看当前 CPU 使用情况
mpstat -P ALL 1 5

# 2. 绑定每个 worker 到独立 CPU
# nginx.conf:
# worker_processes 4;
# worker_cpu_affinity 0001 0010 0100 1000;

# 3. 检查软中断是否均衡
cat /proc/interrupts | grep eth
# 如果不均衡 → 配置 IRQ 亲和性
echo 2 > /proc/irq/<IRQ>/smp_affinity  # 绑定到 CPU 1

# 4. 使用 RPS/RFS 均衡软中断
echo f > /sys/class/net/eth0/queues/rx-0/rps_cpus
```

### 场景 3: 科学计算 (CPU 密集型)

```bash
# 问题: 矩阵运算性能不理想

# 1. 确认 CPU 支持 AVX-512
grep avx512 /proc/cpuinfo

# 2. 确认编译器利用了向量化
perf stat -e cycles,instructions ./matrix_mul
# IPC 应该接近 2-4 (向量化后)

# 3. 绑定到同一 NUMA 节点避免远程内存访问
numactl --cpunodebind=0 --membind=0 ./matrix_mul

# 4. 关闭超线程避免资源争抢
echo off > /sys/devices/system/cpu/smt/control
```

### 场景 4: 虚拟化环境调优

```bash
# 问题: 虚拟机 CPU steal time 高

# 1. 检查 steal time
cat /proc/stat | head -1
# 第 8 个字段 = steal time (被宿主机抢占的时间)

# 2. 查看 /proc/cpuinfo 中的 hypervisor 标志
grep hypervisor /proc/cpuinfo

# 3. 检查宿主机是否配置了 CPU pin
# virsh vcpupin <domain> <vcpu> <pcpu>

# 4. 检查 NUMA 拓扑是否对齐
numactl --hardware
```

### 场景 5: C-State 导致延迟尖刺

```bash
# 问题: 低延迟应用偶发延迟尖刺

# 1. 检查当前 C-State
cpupower idle-info

# 2. 查看各 C-State 使用时间
cat /sys/devices/system/cpu/cpu0/cpuidle/state*/time

# 3. 禁止深度 C-State
# 方法 A: cpupower
cpupower idle-set -D 10   # 最大退出延迟 10μs

# 方法 B: 内核参数
# processor.max_cstate=1 intel_idle.max_cstate=0

# 方法 C: /dev/cpu_dma_latency
echo 0 > /dev/cpu_dma_latency  # 要求最低延迟，阻止深度 C-State
```

### 场景 6: 检查 Spectre/Meltdown 补丁影响

```bash
# 1. 查看漏洞状态
ls /sys/devices/system/cpu/vulnerabilities/
cat /sys/devices/system/cpu/vulnerabilities/meltdown
cat /sys/devices/system/cpu/vulnerabilities/spectre_v1
cat /sys/devices/system/cpu/vulnerabilities/spectre_v2

# 2. 查看是否启用 PTI (Page Table Isolation)
grep pti /proc/cmdline

# 3. 查看 Retpoline 状态
grep retpoline /proc/cpuinfo
# 或在 /sys/devices/system/cpu/vulnerabilities/spectre_v2 中查看

# 4. 性能对比 (补丁前后)
# PTI 对系统调用密集型应用影响 ~5-30%
# 可通过 nopti 内核参数临时禁用 (不推荐生产环境)
```

### 场景 7: 诊断 CPU 热节流

```bash
# 问题: CPU 高负载后性能突然下降

# 1. 读取温度
cat /sys/class/thermal/thermal_zone*/temp
# 值除以 1000 = 摄氏度

# 2. 读取 MSR (需要 msr 模块)
modprobe msr
rdmsr 0x1B1  # IA32_THERM_STATUS
# bit 0 = Thermal Status (1 = 正在节流)
# bit 16-22 = 温度与 TjMax 的差值

# 3. 检查 dmesg
dmesg | grep -i thermal

# 4. 监控频率变化
watch -n 0.5 cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
```

### 场景 8: 大页内存优化

```bash
# 问题: TLB miss 过多导致性能差

# 1. 检查 TLB miss
perf stat -e dTLB-load-misses,dTLB-store-misses,iTLB-load-misses \
  -- my_program

# 2. 配置大页
echo 1024 > /proc/sys/vm/nr_hugepages  # 分配 1024 × 2MB = 2GB

# 3. 使用大页运行
# 方法 A: mmap MAP_HUGETLB
# 方法 B: hugetlbfs 挂载
mount -t hugetlbfs hugetlbfs /dev/hugepages

# 4. 透明大页 (THP)
cat /sys/kernel/mm/transparent_hugepage/enabled
echo always > /sys/kernel/mm/transparent_hugepage/enabled
```

### 场景 9: 中断亲和性调优

```bash
# 问题: 网卡中断集中在 CPU 0，导致单核瓶颈

# 1. 查看中断分布
cat /proc/interrupts | grep eth

# 2. 手动分配 IRQ 亲和性
# 将所有网卡 IRQ 分散到不同 CPU
for irq in $(ls /proc/irq/*/eth0* 2>/dev/null | cut -d/ -f4 | sort -u); do
    echo $((1 << $i)) > /proc/irq/$irq/smp_affinity
    i=$((i+1))
done

# 3. 使用 irqbalance 自动均衡
systemctl start irqbalance

# 4. 或者配合 RPS/RFS
echo f > /sys/class/net/eth0/queues/rx-0/rps_cpus    # RPS 分散到所有 CPU
echo 1000 > /sys/class/net/eth0/queues/rx-0/rps_flow_cnt  # RFS flow 数
```

### 场景 10: perf Top-Down 分析

```bash
# 全面分析 CPU 流水线瓶颈

# Level 1: 四大类
perf stat --topdown -a -- sleep 10
# 输出:
#   retiring:        XX.X%  ← 有用工作
#   bad speculation: XX.X%  ← 分支预测错误浪费
#   frontend bound:  XX.X%  ← 取指/解码瓶颈
#   backend bound:   XX.X%  ← 执行/内存瓶颈

# 分析:
# - retiring > 60%: 良好
# - bad speculation > 20%: 需要优化分支
# - frontend bound > 20%: 检查 I-Cache / iTLB
# - backend bound > 30%: 检查内存带宽/延迟

# 进一步: 区分 memory bound vs core bound
perf stat --topdown --topdown-level=2 -a -- sleep 10
```

### 场景 11: 容器 CPU 限流排查

```bash
# 问题: 容器应用 CPU 使用率异常低

# 1. 查看 cgroup CPU 限制
cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us
cat /sys/fs/cgroup/cpu/cpu.cfs_period_us
# 配额 = quota / period

# 2. 检查是否被限流
cat /sys/fs/cgroup/cpu/cpu.stat
# nr_periods: 总周期数
# nr_throttled: 被限流次数
# throttled_time: 被限流总时间 (ns)

# 3. 如果 nr_throttled 接近 nr_periods:
# → 应用需要的 CPU 超过了 cgroup 配额
# → 增加 quota 或优化代码

# 4. 使用 cpu.shares 调整优先级
echo 2048 > /sys/fs/cgroup/cpu/cpu.shares  # 双倍权重
```
