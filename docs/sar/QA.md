# sar / sysstat 问答集

> 持续更新。每个问题包含：问题背景、解答、相关文件位置。

---

## 目录

- [Q1: sar 发现 %system 很高但吞吐低怎么排查？](#q1-sar-发现-system-很高但吞吐低怎么排查)
- [Q2: 如何用 sar 判断网络软中断 CPU 不均衡？](#q2-如何用-sar-判断网络软中断-cpu-不均衡)
- [Q3: sar 数据和 vmstat/top 数据不一致，以哪个为准？](#q3-sar-数据和-vmstaptop-数据不一致以哪个为准)

---

## Q1: sar 发现 %system 很高但吞吐低怎么排查？

**日期：** 2026-07-04
**场景：** AMD 服务器，应用报告 CPU 高占用低吞吐
**相关文件：** `reading/02_command_reference.md`, `reading/04_diagnostic_workflow.md`

### 回答

`%system` 高说明内核态占用大量 CPU 时间。需要逐步缩小范围。

**Step 1：区分 %system 的构成**

```bash
# 使用 -u ALL 获取 irq 和 soft 的单独列
sar -u ALL 1 10
```

```
-u:     %system 包含 sys + irq + soft（合并显示）
-u ALL: %sys, %irq, %soft 分开显示

如果 %irq + %soft 占了大部分 → 中断/软中断问题
如果 %sys 本身高 → 锁竞争/系统调用风暴
```

**Step 2：看上下文切换和进程创建**

```bash
sar -w 1 10
```

| 指标 | 正常（4 核） | 异常 |
|------|-------------|------|
| cswch/s | < 20000 | > 50000 → 过度调度/锁竞争 |
| proc/s | < 50 | > 100 → 频繁 fork |

**Step 3：看各 CPU 分布**

```bash
sar -P ALL 1 10
```

| 模式 | 含义 |
|------|------|
| 所有 CPU %system 均匀高 | 全局锁竞争 |
| 某个 CPU %system 特别高 | 中断/锁绑定到该 CPU |
| 某个 CPU %soft 特别高 | 网络中断不均衡 |

**Step 4：看运行队列**

```bash
sar -q 1 10
```

- `runq-sz > CPU 核数` → 进程排队等待
- `blocked > 3` → I/O 阻塞

**Step 5：用专业工具深入**

```
%sys 高 + cswch/s 高 → perf lock record → 锁竞争分析
%sys 高 + 某函数 12%  → perf top → 内核热点函数
%soft 高 + CPU 不均衡 → 启用 irqbalance / 配置 RSS
```

**★ 快速总结：** sar 告诉你"哪个维度异常"，但定位根因需要 perf/bpftrace 深入。

---

## Q2: 如何用 sar 判断网络软中断 CPU 不均衡？

**日期：** 2026-07-04
**场景：** 网络吞吐上不去，怀疑软中断不均衡
**相关文件：** `reading/02_command_reference.md`, `reading/04_diagnostic_workflow.md`

### 回答

需要组合三个 sar 命令来判断。

**Step 1：确认软中断存在**

```bash
sar -u ALL 1 5
# 关注 %soft 列
# %soft > 5% → 软中断占比高，需要关注
```

**Step 2：看各 CPU 的 %soft 分布**

```bash
sar -P ALL -u ALL 1 5
# 对比各 CPU 的 %soft 列
```

```
正常情况（均衡）：
  CPU 0: %soft = 3%
  CPU 1: %soft = 3%
  CPU 2: %soft = 2%
  CPU 3: %soft = 3%

异常情况（不均衡）：
  CPU 0: %soft = 25%   ★ 承担了大量软中断
  CPU 1: %soft = 1%
  CPU 2: %soft = 1%
  CPU 3: %soft = 1%
```

**Step 3：看网络包速率**

```bash
sar -n DEV 1 5
# 关注 rxpck/s（每秒接收包数）
# 小包密集（rxpck/s 高但 rxkB/s 低）最容易暴露不均衡问题
```

**Step 4：看网络错误**

```bash
sar -n EDEV 1 5
# 关注 rxdrop/s（接收丢弃数）
# rxdrop/s > 0 → CPU 处理不过来，包被丢弃
```

**Step 5：看中断分布**

```bash
sar -I XALL 1 5
# 关注网络相关中断线在各 CPU 上的分布
# 如果集中在一个 CPU → 中断亲和性问题
```

**★ 判断标准：**

```
正常:
  - 各 CPU %soft 差异 < 3%
  - rxdrop/s = 0
  - 中断分布均匀

不均衡:
  - 某个 CPU %soft 是其他 CPU 的 5x 以上
  - rxdrop/s > 0
  - 中断集中在一个 CPU

解决方案:
  1. systemctl start irqbalance（自动均衡中断）
  2. 配置网卡多队列 + RSS:
     ethtool -L eth0 combined 4
  3. 启用 RPS（Receive Packet Steering）:
     echo f > /sys/class/net/eth0/queues/rx-0/rps_cpus
```

---

## Q3: sar 数据和 vmstat/top 数据不一致，以哪个为准？

**日期：** 2026-07-04
**场景：** 同一时间段内，sar -u 显示 %user=30%，但 top 显示某个进程 CPU 占用 80%，产生疑惑
**相关文件：** `reading/01_sar_architecture.md`

### 回答

**三个工具的底层数据源相同（都读 /proc），但因为采样间隔和计算方式不同，导致数值看起来不一致。** 不存在"谁更准"的问题，而是它们在不同时间粒度上都是正确的。

#### ★ 数据源对比表

| 指标 | sar 数据源 | vmstat 数据源 | top 数据源 | 是否相同 |
|------|-----------|-------------|-----------|---------|
| CPU 时间 | `/proc/stat` | `/proc/stat` | `/proc/stat` | ★ 完全相同 |
| 内存 | `/proc/meminfo` | `/proc/meminfo` | `/proc/meminfo` | ★ 完全相同 |
| 换页 | `/proc/vmstat` | `/proc/vmstat` | — | ★ 完全相同 |
| 磁盘 | `/proc/diskstats` | — | — | — |
| 上下文切换 | `/proc/stat` | `/proc/stat` | — | ★ 完全相同 |

**源码依据（sar 的数据采集函数）：**

```
sar 的 sadc 每次采集时调用以下函数：
  read_stat_cpu()    → /proc/stat         → CPU 时间    ← vmstat/top 也读这里
  read_stat_memory() → /proc/meminfo       → 内存使用
  read_stat_disk()   → /proc/diskstats     → 磁盘 I/O
  read_stat_paging() → /proc/vmstat        → 页面换入换出 ← vmstat 也读这里
  read_stat_queue()  → /proc/loadavg       → 负载/队列

（来源：reading/01_sar_architecture.md 第 284-291 行）
```

#### ★ 不一致的三个根因

**根因 1：采样间隔不同（最常见）**

```
时间线：
  0s ──────── 3s ──────── 60s ──────── 600s
  │           │           │             │
  top(3s)     top(3s)     top(3s)       sar(600s)
  CPU=100%    CPU=0%      CPU=0%        CPU=0.5%

top 看到：0-3 秒内 CPU 100%（短时尖峰）
sar 看到：0-600 秒内平均 CPU 0.5%（★ 尖峰被平均掉了）

★ 结论：sar 的默认 10 分钟粒度会掩盖短时尖峰
```

**根因 2：计算方式不同**

```
top 的 %CPU：
  %CPU = (进程 CPU 时间增量 / 采样间隔) × 100%
  ★ 多核系统上可以 > 100%（例如 4 核上最高 400%）
  ★ top 默认显示单核百分比

sar 的 %user：
  %user = (user_jiffies_diff / total_jiffies_diff) × 100%
  ★ 是所有 CPU 的平均值
  ★ 永远不会 > 100%

示例（8 核机器）：
  1 个进程占满 1 个核：
    top:   %CPU = 100%（单核视角）
    sar:   %user = 12.5%（8 核平均 = 100/8）
    sar -P ALL: CPU 3 = 100%（★ 用 -P ALL 看单核就一致了）
```

**根因 3：vmstat 的特殊输出格式**

```
vmstat 1:
  procs ──── memory ──── swap ── io ── system ── cpu ──
   r  b   swpd  free  buff  cache  si so  bi bo  in cs  us sy id wa st
   2  0      0 12345  6789 123456   0  0   0  0 100 200 30 10 60  0  0

  us = user%, sy = system%, id = idle%
  ★ vmstat 和 sar -u 都读 /proc/stat，数值应该一致
  ★ 但如果 vmstat 采样间隔 = 1s，sar 采样间隔 = 600s，数值会不同
```

#### ★ 对照表：什么时候用谁

```
需求                             推荐工具              理由
─────────────────────────────    ──────────────────    ──────────────────
实时看某个进程的 CPU              top / pidstat         ★ top 按进程拆分
实时看全局 CPU 分布               sar -u 1 5            1 秒粒度，准确
查看历史 CPU 趋势（昨天）          sar -u -f /var/log/sa/saXX  ★ sar 独有
快速看一眼全貌                    vmstat 1              单行输出，直观
CPU + 内存 + IO + 网络综合        sar -A 1 5            一次看所有维度
确认 sar 和 top 是否一致          sar -u -P ALL 1 3     ★ 必须用 1 秒粒度 + 单核
```

#### 验证方法：让 sar 和 top 输出一致

```bash
# 方法：两者都用 1 秒粒度，同时运行
# 终端 1：
sar -u -P ALL 1 5

# 终端 2：
top -bn1 -d1 | head -5

# 对比：
# sar 的 %user（所有 CPU 平均）≈ top 的 %us（所有 CPU 平均）
# sar -P ALL 的单核 %user ≈ top 按 1 切换后单核的 %us
```

#### ★ 关键结论

```
                    数据源         采样粒度          视角
  sar (默认)        /proc/stat     10 分钟           全局平均
  sar -u 1          /proc/stat     1 秒              全局平均
  sar -P ALL 1      /proc/stat     1 秒              单核
  vmstat 1          /proc/stat     1 秒              全局平均
  top               /proc/stat     3 秒              按进程 + 全局

★ 数据源完全相同，差异来自采样间隔和计算方式
★ 要对齐：统一用 1 秒粒度 + 相同视角（全局 or 单核）
★ sar 的独有优势：历史数据查询（-f /var/log/sa/saXX）
```

**一句话总结：** sar、vmstat、top 都读 `/proc/stat`，数据源完全相同。数值不一致是因为采样间隔不同（sar 默认 10 分钟 vs top 默认 3 秒）和计算方式不同（全局平均 vs 单核百分比）。统一用 1 秒粒度即可对齐。

---

*新的问题将追加到此文件。每个问题记录日期、场景、相关文件。*
