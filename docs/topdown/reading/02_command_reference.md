# Step 2: perf stat --topdown 命令参考

> 全面掌握 TMAM 的工具链：perf stat 参数、metric group、pmu-tools、事件组定义

---

## 阅读要点

1. `perf stat --topdown --td-level N` 参数详解
2. `-M TopdownL1/L2/L3` metric group 方式
3. toplev.py（pmu-tools）增强功能
4. Level 1-4 每级的事件组和
5. 10+ 个分析场景的命令命令

**预计阅读时间：** 20 分钟

---

## 一、perf stat --topdown 参数详解

### 1.1 基本语法

```bash
perf stat --topdown --td-level <N> [选项] -- <命令>
```

### 1.2 核心参数对照表

| 参数 | 含义 | 默认值 | 说明 |
|------|------|--------|------|
| `--topdown` | 启用 topdown 分析 | 关闭 | ★ 必须指定的开关 |
| `--td-level <N>` | 分析层级（0=max） | 1 | 1=四大类，2=八子类 |
| `-M TopdownL1` | Metric group 方式 | — | 等价于 --topdown --td-level 1 |
| `-M TopdownL2` | Metric group L2 | — | 等价于 --topdown --td-level 2 |
| `--metric-only` | 仅显示计算后的指标 | 关闭 | 隐藏原始事件计数 |
| `--metric-no-group` | 不组合事件组 | 关闭 | 减少 multiplexing |
| `-j` / `--json-output` | JSON 输出 | 关闭 | 方便脚本解析 |
| `-x,` | CSV 分隔符 | — | 导出到表格分析 |
| `-I <ms>` | 间隔打印 | — | 每 N 毫秒输出一次 |
| `-r <N>` | 重复运行 | 1 | 取平均值和标准差 |
| `-C <cpu>` | 指定 CPU | 全部 | 绑定到特定 CPU |
| `-p <pid>` | 监控进程 | — | 附加到已运行进程 |
| `-t <tid>` | 监控线程 | — | 附加到特定线程 |

### 1.3 --topdown vs -M TopdownL1

两种方式功能等价，但实现机制略有不同：

```
方式 1：--topdown --td-level N
┌─────────────────────────────────────────────────┐
│ perf stat --topdown --td-level 1 -- ./program   │
│                                                 │
│ 内部行为：                                       │
│ - perf 自动计算需要的事件列表                      │
│ - 根据 td-level 确定展开到第几级                   │
│ - 自动处理事件分组和 multiplexing                  │
│ ★ 推荐方式（Linux 5.13+）                        │
└─────────────────────────────────────────────────┘

方式 2：-M TopdownL1
┌─────────────────────────────────────────────────┐
│ perf stat -M TopdownL1 -- ./program             │
│                                                 │
│ 内部行为：                                       │
│ - 使用 JSON 定义的 metric group                  │
│ - 从 /sys/devices/cpu/events/ 或下的 定义            │
│ - 兼容性更好（旧版 perf 也可 │
│ ★ 兼容性方式方式                                     │
└─────────────────────────────────────────────────┘
```

### 1.4 常用命令示例

```bash
# 方式 1：Level 1 四大类分析
perf stat --topdown --td-level 1 -- ./program

# 方式 2：metric group（兼容性更好）
perf stat -M TopdownL1 -- ./program

# 方式 3：Level 2 八大子类分析
perf stat --topdown --td-level 2 -- ./program

# 方式 4：仅显示指标值（隐藏原始事件计数）
perf stat --topdown --td-level 1 --metric-only -- ./program

# 方式 4：JSON 输出（便于脚本解析）
perf stat --topdown --td-level 1 -j -- ./program

# 方式 5：间隔打印（每 1000ms 输出一次）
perf stat --topdown --td-level 1 -I 1000 -- ./program

# 方式 6：附加到已运行的进程
perf stat --topdown --td-level 1 -p <PID>

# 方式 7：仅监控特定 CPU
perf stat --topdown --td-level 1 -C 0 -- ./program

# 方式 8：重复运行取平均
perf stat --topdown --td-level 1 -r 3 -- ./program

# 方式 9：CSV 输出（方便导入表格）
perf stat --topdown --td-level 1 -x, -- ./program

# 方式 10：同时收集其他指标
perf stat --topdown --td-level 1 -e cache-misses,cache-references -- ./program
```

---

## 二、Level 1-4 每级的事件组

### 2.1 Level 1：四大类

```
Level 1 使用以下核心 PMU 事件：
┌────────────────────────────┬──────────────────────────────┐
│ PMU 事件                    │ 用途                          │
├────────────────────────────┼──────────────────────────────┤
│ INST_RETIRED.ANY           │ ★ 退休的 μop 总数            │
│ CPU_CLK_UNHALTED.THREAD    │ ★ 未停止的 CPU 周期数        │
│ IDQ_UOPS_NOT_DELIVERED.CORE│ 前端未交付的 μop（空闲 Slot） │
│ UOPS_ISSUED.ANY            │ 发送到后端的 μop 总数         │
│ INT_MISC.RECOVERY_CYCLES   │ 流水线恢复周期                │
│ BR_MISP_RETIRED.ALL_BRANCHES│ 错误预测的分支              │
│ MACHINE_CLEARS.COUNT       │ 流水线完全冲刷次数            │
└────────────────────────────┴──────────────────────────────┘
```

**★ Level 1 计算公式（简化版）：**

```
Total_Slots       = 4 × CPU_CLK_UNHALTED.THREAD
Retiring          = INST_RETIRED.ANY / Total_Slots
Frontend_Bound    = IDQ_UOPS_NOT_DELIVERED.CORE / Total_Slots
Bad_Speculation   = (Total_Slots - Retiring - Frontend_Bound - Backend_Bound)
                  = Pipeline_Flush_Slots + Non_Retiring_Slots
Backend_Bound     = (CPU_CLK - UOPS_ISSUED_Slots) / Total_Slots

★ 实际计算比这复杂得多，perf 内部有修正公式
```

### 2.2 Level 2：八大子类

```
Level 2 在 Level 1 基础上增加以下事件：
┌─────────────────────────────────┬────────────────────────────────┐
│ PMU 事件                         │ 用途                            │
├─────────────────────────────────┼────────────────────────────────┤
│ IDQ_UOPS_NOT_DELIVERED.CORE     │ Fetch Latency / Bandwidth 区分 │
│ ICACHE_16B_IFDATA_STALL         │ I-Cache 停顿                   │
│ IDQ.MS_SWITCHES                 │ μop Cache ↔ MS 切换            │
│ BR_MISP_RETIRED.ALL_BRANCHES    │ Branch Mispredict 计数         │
│ MACHINE_CLEARS.COUNT            │ Machine Clears 计数            │
│ CYCLE_ACTIVITY.STALLS_MEM_ANY  │ Memory Bound 周期              │
│ CYCLE_ACTIVITY.STALLS_TOTAL    │ 总停顿周期                      │
│ EXE_ACTIVITY.EXE_BOUND_0PORTS  │ Core Bound（无端口可用）        │
└─────────────────────────────────┴────────────────────────────────┘
```

### 2.3 Level 3：细分事件

```
Level 3 进一步区分：

Memory Bound 细分：
┌─────────────────────────────────┬────────────────────────────────┐
│ PMU 事件                         │ 用途                            │
├─────────────────────────────────┼────────────────────────────────┤
│ L1D_PEND_MISS.PENDING           │ L1 未命中挂起周期              │
│ MEM_LOAD_RETIRED.L1_MISS        │ L1 数据缓存未命中              │
│ MEM_LOAD_RETIRED.L2_MISS        │ L2 缓存未命中                  │
│ MEM_LOAD_RETIRED.L3_MISS        │ L3 缓存未命中                  │
│ MEM_LOAD_RETIRED.L3_HIT         │ L3 缓存命中                    │
│ OFFCORE_REQUESTS_OUTSTANDING    │ 在 outstanding 请求数 请求   │
│ L2_RQSTS.MISS                   │ L2 请求未命中                  │
└─────────────────────────────────┴────────────────────────────────┘

Core Bound 细分：
┌─────────────────────────────────┬────────────────────────────────┐
│ PMU 事件                         │ 用途                            │
├─────────────────────────────────┼────────────────────────────────┤
│ EXE_ACTIVITY.1_PORTS_UTIL      │ 仅 1 个端口活跃                 │
│ EXE_ACTIVITY.2_PORTS_UTIL      │ 2 个端口活跃                    │
│ ARITH.DIVIDER_UOPS             │ 除法单元活跃                    │
└─────────────────────────────────┴────────────────────────────────┘

Fetch Latency 细分：
┌─────────────────────────────────┬────────────────────────────────┐
│ PMU 事件                         │ 用途                            │
├─────────────────────────────────┼────────────────────────────────┤
│ ICACHE_16B_IFDATA_STALL         │ I-Cache 数据停顿                │
│ ITLB_MISSES.WALK_COMPLETED       │ iTLB 未命中                   │
│ INT_MISC.CLEARS_COUNT           │ 分支重定向次数（重新取指）      │
│ DSB2MITE_SWITCHES.PENALTY_CYCLES │ DSB ↔ MITE 切换惩罚           │
└─────────────────────────────────┴────────────────────────────────┘
```

### 2.4 Level 4：最细粒度事件

```
Level 4 在 Level 3 基础上进一步区分：

L1 Bound 细分：
┌─────────────────────────────────┬────────────────────────────────┐
│ PMU 事件                         │ 用途                            │
├─────────────────────────────────┼────────────────────────────────┤
│ LD_BLOCKS.NO_SR                 │ Store 转发失败                  │
│ LD_BLOCKS_PARTIAL.ADDRESS_ALIAS │ 4KB 地址别名冲突                │
│ LD_HEAD.L1_MISS                 │ L1 Miss + Lock/TLB              │
│ DTLB_LOAD_MISSES.WALK_COMPLETED │ DTLB 加载未命中                 │
│ LD_BLOCKS.DTLB_MISS             │ DTLB 未命中导致的加载阻塞       │
└─────────────────────────────────┴────────────────────────────────┘

L3 Bound 细分：
┌─────────────────────────────────┬────────────────────────────────┐
│ PMU 事件                         │ 用途                            │
├─────────────────────────────────┼────────────────────────────────┤
│ OFFCORE_REQUESTS_OUTSTANDING.L3_MISS │ Outstanding L3 Miss 请求   │
│ MEM_LOAD_L3_MISS_RETIRED.REMOTE_DRAM │ 远端 DRAM 访问             │
│ SQ_MISC.SPLIT_LOCK              │ 跨缓存行锁                      │
│ OCR.DEMAND_DATA_RD.L3_MISS      │ L3 未命中的数据读取             │
└─────────────────────────────────┴────────────────────────────────┘
```

---

## 三、toplev.py（pmu-tools）使用

### 3.1 安装

```bash
# 方式 1：pip 安装
pip3 install pmu-tools

# 方式 2：从 GitHub 
git clone https://github.com/andikleen/pmu-tools.git
cd pmu-tools/

# 方式 3：perf（CentOS/RHEL）
yum install -y pmu-tools  # 如果 EPEL 可用
```

### 3.2 基本语法

```bash
toplev.py [选项] -- <命令>
```

### 3.3 核心参数

| 参数 | 含义 | 默认值 | 说明 |
|------|------|--------|------|
| `-l <N>` | 分析层级 | 1 | 1-6 级 |
| `--core <C>` | 指定 CPU 核心 | 全部 | 格式：`C0`、`C0-C3` |
| `--no-desc` | 不显示指标描述 | 显示 | 减少输出 |
| `--graph` | 生成 ASCII 图表 | 关闭 | 树状展示 |
| `--nodes <nodes>` | 仅显示指定节点 | 全部 | 如 `+Frontend_Bound` |
| `--threshold <N>` | 显示阈值 | 5% | 低于 N% 的节点不显示 |
| `--run-sample` | 采样模式 | 关闭 | 结合 perf record |
| `--drilldown` | 交互式下钻 | 关闭 | 逐步展开 |
| `-v` | 详细模式 | 关闭 | 显示原始计数 |
| `-x <sep>` | CSV 分隔符 | — | 导出到表格 |
| `-I <ms>` | 间隔打印 | — | 每 N ms 输出 |
| `-r <N>` | 重复运行 | 1 | 取平均值 |

### 3.4 与 perf stat --topdown 的对比

| 特性 | perf stat --topdown | toplev.py |
|------|--------------------|-----------|
| 安装 | 内核自带 | pip3 install |
| 最高级别 | 取决于 CPU 支持 | ★ 最高 6 级 |
| 事件管理 | 自动 | ★ 精细控制 multiplexing |
| 图表输出 | 无 | ★ ASCII 树状图 |
| 多核支持 | 基本 | ★ 按核/按线程分析 |
| 输出格式 | 文本/JSON/CSV | 文本/CSV/HTML |
| 描述信息 | 无 | ★ 每个指标有详细解释 |

---

## 四、Level 1-4 各级的事件组定义

### 4.1 Level 1：四大类指标

```
Level 1 指标定义（Skylake 为例）：

┌──────────────────────┬─────────────────────────────────────────┐
│ 指标名称              │ 计算公式（简化）                         │
├──────────────────────┼─────────────────────────────────────────┤
│ Retiring             │ INST_RETIRED.ANY / (4 × CPU_CLK)       │
│ Frontend Bound       │ IDQ_UOPS_NOT_DELIVERED.CORE / (4 × CLK)│
│ Bad Speculation      │ 1 - Retiring - Frontend - Backend       │
│ Backend Bound        │ (CLK - UOPS_ISSUED_any / 4) / CLK      │
└──────────────────────┴─────────────────────────────────────────┘

★ 注意：实际公式更复杂，包含修正项
  perf 源码中使用 JSON 定义的 metric 表达式
```

### 4.2 Level 2：八大子类指标

```
Frontend Bound 分解：
┌──────────────────────┬─────────────────────────────────────────┐
│ Fetch Latency        │ (IF_STALL + iTLB_STALL + ...) / Total   │
│ Fetch Bandwidth      │ Frontend Bound - Fetch Latency          │
└──────────────────────┴─────────────────────────────────────────┘

Bad Speculation 分解：
┌──────────────────────┬─────────────────────────────────────────┐
│ Branch Mispredict    │ BR_MISP_RETIRED × Recovery / Total      │
│ Machine Clears       │ MACHINE_CLEARS × Recovery / Total       │
└──────────────────────┴─────────────────────────────────────────┘

Backend Bound 分解：
┌──────────────────────┬─────────────────────────────────────────┐
│ Memory Bound         │ CYCLE_ACTIVITY.STALLS_MEM_ANY / CLK    │
│ Core Bound           │ Backend Bound - Memory Bound            │
└──────────────────────┴─────────────────────────────────────────┘

Retiring 分解：
┌──────────────────────┬─────────────────────────────────────────┐
│ Base                 │ 简单指令退休比例                          │
│ Microcode Sequencer  │ 复杂指令（MS 辅助）退休比例              │
└──────────────────────┴─────────────────────────────────────────┘
```

### 4.3 perf 源码中的 metric 定义

```
perf 的 topdown metric 定义在 JSON 文件中：

路径（以核源码为例）：
  src/linux-5.10/tools/perf/pmu-events/arch/x86/skylake/sk-metrics.json

每个中 示例（：
{
  "MetricName": "tma_frontend_bound",
  "MetricExpr": "IDQ_UOPS_NOT_DELIVERED.CORE / (4 * CPU_CLK_UNHALTED.THREAD)",
  "MetricGroup": "MetricGroup": "TopDownL1",
  "Brief": "topdown",
  "Frontend Bound",
  "Metric": "This category represents fraction of slots where the processor's Frontend undersupplies its Backend"
}

★ 不同 CPU 代际的 metric 表达式不同
  perf 根据 CPUID 自动加载对应的 JSON 文件
```

---

## 五、10+ 个分析场景实用命令

### 场景 1：快速诊断程序瓶颈

```bash
# 运行 Level 1 分析，仅显示百分比
perf stat --topdown --td-level 1 --metric-only -- ./program

# ★ 输出示例：
#      retiring  frontend_bound  bad_speculation  backend_bound
#         65.2%            5.1%            3.8%          25.9%
```

### 场景 2：下钻到 Level 2

```bash
perf stat --topdown --td-level 2 --metric-only -- ./program

# ★ 输出示例（Level 2 展开 Backend八 ）：
#      retiring          65.2%
#        base                  62.1%
#        microcode_sequencer    3.1%
#      frontend_bound    5.1%
#        fetch_latency          3.8%
#        fetch_bandwidth        1.3%
#      bad_speculation   3.8%
#        branch_mispredict      3.2%
#        machine_clears         0.6%
#      backend_bound    25.9%
#        memory_bound          20.1%
#        core_bound             5.8%
```

### 场景 3：对比优化前后

```bash
# 优化前（朴素实现）
perf stat --topdown --td-level 2 -- ./program_naive 2>&1 | tee before.txt

# 优化后（改进实现）
perf stat --topdown --td-level 2 -- ./program_optimized 2>&1 | tee after.txt

# ★ 对比 Retiring% 变化：越高说明优化越有效
```

### 场景 4：附加到已运行进程

```bash
# 附加到 PID，采集 30 秒 topdown 数据
perf stat --topdown --td-level 1 -p <PID> sleep 30
```

### 场景 5：同时收集补充事件

```bash
# topdown + 缓存和分支事件
perf stat --topdown --td-level 1 -e cache-misses,branch-misses,L1-dcache-load-misses -- ./program
```

### 场景 6：多核对比

```bash
# 分别在每个核心上运行
for cpu in 0 1 2 3; do
  perf stat --topdown --td-level 1 -C $cpu -- ./program &
done
wait
```

### 场景 7：时间序列分析

```bash
# 每 1000ms 输出一次 topdown
perf stat --topdown --td-level 1 -I 1000 -- ./long_running_program

# ★ 可以观察性能随时间变化（如启动时 Frontend Bound 为主，稳态后 Backend Bound 为主）
```

### 场景 8：生成 toplev.py 树状图

```bash
# 使用 toplev.py 生成 ASCII 树状图
toplev.py --core C0 -l2 --graph -- ./program

# ★ 输出示例：
#     FE  │── 2.3%  Frontend Bound
#     FE  │ ├── 1.5%  Fetch Latency
#     FE  │  └── 0.8%  Fetch Bandwidth
#     BE  │── 25.9%  Backend Bound
#     BE  │ ├── 20.1%  Memory Bound
#     BE  │  └── 5.8%  Core Bound
#     BAD │── 3.8%  Bad Speculation
#     RET │── 68.0%  Retiring
```

### 场景 9：Level 3 深度分析

```bash
# Level 3 分析（需要更多事件，multiplexing 增加）
toplev.py --core C0 -l3 -- ./memory_bound_program

# ★ Level 3 可以看到：
#   Memory Bound → L1 Bound / L2 Bound / L3 Bound / DRAM Bound
#   Core Bound → Divider / Ports Utilization
```

### 场景 10：结合 perf record 定位热点函数

```bash
# 第一步：topdown 确定瓶颈类型
perf stat --topdown --td-level 2 -- ./program

# 第二步：perf record 找到热点函数
perf record -g -- ./program
perf report

# 第三步：perf annotate 查看热点指令
perf annotate --stdio

# ★ 结合：topdown 告诉你"为什么慢"，perf record 告诉你"在哪慢"
```

### 场景 11：多程序批量对比

```bash
#!/bin/bash
# batch_topdown.sh：批量对比多个程序
PROGRAMS="matmul chase sorted unsorted"

echo "程序,Retiring,Frontend,Backend,BadSpec"
for prog in $PROGRAMS; do
  result=$(perf stat --topdown --td-level 1 -x, -- ./$prog 2>&1 | grep -E "^tma_")
  echo "$prog,$result"
done
```

### 场景 12：JSON 输出

```bash
# JSON 格式输出，便于脚本自动化分析
perf stat --topdown --td-level 1 -j -- ./program 2>&1 | python3 -m json.tool
```

### 场景 13：使用 toplev.py 的 --drilldown 模式

```bash
# 交互式下钻：自动识别最大瓶颈并继续下钻
toplev.py --core C0 --drilldown -- ./program

# ★ toplev.py 会自动：
#   1. 运行 Level 1
#   2. 找到最大瓶颈项
#   3. 继续下钻到更深层级
#   4. 直到找到根本原因
```

### 场景 14：NUMA 感知的 topdown 分析

```bash
# 分别在不同 NUMA 节点上运行
perf stat --topdown --td-level 1 -C 0-3 -- numactl --cpunodebind=0 -- ./program
perf stat --topdown --td-level 1 -C 4-7 -- numactl --cpunodebind=1 -- ./program

# ★ 对比两个 NUMA 节点的 topdown 分布
#   远端内存访问会导致 Memory Bound 升高，NUMA 不平衡
```

### 场景 15：SMT 线程对比

```bash
# 单线程模式
taskset -c 0 perf stat --topdown --td-level 1 -- ./program

# 双线程模式（两个逻辑核）
taskset -c 0,1 perf stat --topdown --td-level 1 -- ./program_mt

# ★ 对比 SMT 开/关时的 topdown 差异
#   SMT 开启时 Backend Bound 通常更低（两个线程互补填充空闲 Slot）
```

---

## 六、输出格式说明

### 6.1 perf stat --topdown 输出格式

```
# 默认输出（人类可读）
 Performance counter stats for './program':

       retiring  frontend_bound  bad_speculation  backend_bound
         65.2%            5.1%            3.8%          25.9%

       2.123456789 seconds time elapsed

# Level 2 输出
       retiring                              65.2%
         base                                62.1%
         microcode_sequencer                  3.1%
       frontend_bound                         5.1%
         fetch_latency                        3.8%
         fetch_bandwidth                      1.3%
       bad_speculation                        3.8%
         branch_mispredict                    3.2%
         machine_clears                       0.6%
       backend_bound                         25.9%
         memory_bound                        20.1%
         core_bound                           5.8%
```

### 6.2 toplev.py 输出格式

```
SKL Metrics for program on Core C0

|Metric                           |Core C0 |
|─────────────────────────────────│────────│
|Frontend_Bound                   │  5.1% │
|├── Fetch_Latency                │  3.8% │
│ └── Fetch_Bandwidth             │  1.3% │
│ Backend_Bound                   │ 25.9% │
│ ├── Memory_Bound                │ 20.1% │
│ └── Core_Bound                  │  5.8% │
│ Bad_Speculation                 │  3.8% │
│ ├── Branch_Mispredicts          │  3.2% │
│ └── Machine_Clears              │  0.6% │
│ Retiring                        │ 65.2% │
│ ├── Base                        │ 62.1% │
│ └── Microcode_Sequencer         │  3.1% │
```

### 6.3 输出字段含义速查

| 字段 | 含义 | 单位 |
|------|------|------|
| retiring | 有效退休比例 | % |
| frontend_bound | 前端瓶颈比例 | % |
| backend_bound | 后端瓶颈比例 | % |
| bad_speculation | 错误推测比例 | % |
| fetch_latency | 取指延迟比例 | % |
| fetch_bandwidth | 取指带宽比例 | % |
| branch_mispredict | 分支预测错误比例 | % |
| machine_clears | 流水线冲刷比例 | % |
| memory_bound | 内存瓶颈比例 | % |
| core_bound | 执行单元瓶颈比例 | % |
| base | 常规退休比例 | % |
| microcode_sequencer | 微码辅助退休比例 | % |

---

## 七、常见问题

### 7.1 为什么某些指标显示 `<not counted>`？

```
原因：PMU 计数器不够用，发生了 multiplexing（多路复用）
解决：
  1. 使用 --metric-no-group 减少事件组大小
  2. 使用 toplev.py（更好的 multiplexing 管理）
  3. 降低分析层级（一次只看 Level 1 或 Level 2）
```

### 7.2 为什么 topdown 结果不稳定？

```
原因：
  1. 其他 perf 进程占用 PMU 计数器
  2. 在 VM 中运行（无 PMU 透传）
  3. perf 版本不支持 --topdown 选项
解决：
  1. 确认没有其他 perf 进程在运行
  2. 在物理机上运行，或使用 toplev.py --emulate
  3. 升级 perf（推荐 5.13+）
```

### 7.3 topdown 结果是否可靠？

```
★ multiplexing 会引入误差
  如果 <not counted> 很多，结果的置信度下降
建议：
  1. 使用 -r 3 多次运行取平均
  2. 观察标准差（stddev），> 5% 的结果需谨慎
  3. 优先看百分比（%）而非绝对值
```

---

## 八、总结：命令速查表

| 目的 | 命令 |
|------|------|
| Level 1 快速诊断 | `perf stat --topdown --td-level 1 -- ./prog` |
| Level 2 下钻 | `perf stat --topdown --td-level 2 -- ./prog` |
| 仅显示指标 | `perf stat --topdown --td-level 1 --metric-only -- ./prog` |
| JSON 输出 | `perf stat --topdown --td-level 1 -j -- ./prog` |
| 附加到进程 | `perf stat --topdown --td-level 1 -p PID` |
| 间隔打印 | `perf stat --topdown --td-level 1 -I 1000 -- ./prog` |
| 重复取平均 | `perf stat --topdown --td-level 1 -r 3 -- ./prog` |
| toplev.py 树状图 | `toplev.py --core C0 -l2 --graph -- ./prog` |
| toplev.py 深度下钻 | `toplev.py --core C0 --drilldown -- ./prog` |
| 结合热点函数 | `perf record -g -- ./prog && perf report` |
| 同时收集缓存事件 | `perf stat --topdown --td-level 1 -e cache-misses -- ./prog` |
