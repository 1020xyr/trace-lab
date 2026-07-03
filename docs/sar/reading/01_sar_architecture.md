# sar 架构详解 — sadc / sa1 / sa2 / sar 的关系

> Step 1 · 理解 sysstat 数据采集与查询的完整架构
> 来源：sysstat-12.7.6 源码 + 系统配置文件
> 阅读要点：四个工具的分工、sa 二进制文件格式、systemd 定时采集
> 预计时间：20 分钟

---

## 1. 四个工具的分工

```
┌─────────────────────────────────────────────────────────────────┐
│                     sysstat 工具链                               │
│                                                                 │
│  ┌──────┐     ┌──────┐     ┌─────────────────────┐             │
│  │ sadc │────→│ saXX │────→│       sar           │             │
│  │采集器│     │二进制│     │ 实时查询 / 历史读取  │             │
│  │(C)   │     │文件  │     │  (C)                │             │
│  └──┬───┘     └──────┘     └─────────────────────┘             │
│     │           ↑    ↓                                          │
│     │           │    └──→ sadf (格式转换: CSV/JSON/SVG)         │
│     │           │                                               │
│     │     ┌─────┴───┐     ┌──────┐                              │
│     └─────│ sa1     │     │ sa2  │──→ sarXX (文本报告)          │
│           │(shell)  │     │(shell)│                             │
│           │调用 sadc │     │调用sar│                             │
│           └─────────┘     └──────┘                              │
└─────────────────────────────────────────────────────────────────┘
```

### 1.1 各工具职责

| 工具 | 类型 | 路径 | ★ 核心职责 |
|------|------|------|-----------|
| **sadc** | C 二进制 | `/usr/lib64/sa/sadc` | 读取 /proc + /sys 计数器，写入 sa 二进制文件 |
| **sa1** | shell 脚本 | `/usr/lib64/sa/sa1` | 读取配置、处理路径，然后 `exec sadc` |
| **sa2** | shell 脚本 | `/usr/lib64/sa/sa2` | 调用 `sar -A -f saXX > sarXX` 生成文本报告 |
| **sar** | C 二进制 | `/usr/bin/sar` | 读取 sa 文件或直接读 /proc，格式化输出 |
| **sadf** | C 二进制 | `/usr/bin/sadf` | 将 sa 文件转换为 CSV/JSON/SVG |

### 1.2 调用关系

```
systemd timer (每 10 分钟)
  │
  ▼
sysstat-collect.service
  │
  ├── ExecStart: /usr/lib64/sa/sa1 1 1
  │                 │
  │                 ├── 读取 /etc/sysconfig/sysstat
  │                 │   HISTORY=28, SADC_OPTIONS="-S DISK"
  │                 │
  │                 └── exec sadc -F -L -S DISK 1 1 /var/log/sa
  │                       │
  │                       ├── 打开 /var/log/sa/saXX (今天的文件)
  │                       ├── 读取所有 /proc + /sys 计数器
  │                       ├── 写入一条二进制记录
  │                       └── 退出
  │
systemd timer (每天 00:07)
  │
  ▼
sysstat-summary.service
  │
  └── ExecStart: /usr/lib64/sa/sa2 -A
                    │
                    ├── sar -A -f /var/log/sa/saXX > /var/log/sa/sarXX
                    └── 清理过期文件 + 压缩旧文件
```

### 1.3 sa1 脚本详解

```bash
#!/usr/bin/sh
# /usr/lib64/sa/sa1 — sadc 的 shell 包装

# 读取配置文件
[ -r ${SYSCONFIG_DIR}/${SYSCONFIG_FILE} ] && . ${SYSCONFIG_DIR}/${SYSCONFIG_FILE}

# ★ HISTORY > 28 天 → 使用长文件名 (saYYYYMMDD) + sadc -D 选项
if [ ${HISTORY} -gt 28 ]; then
    SADC_OPTIONS="${SADC_OPTIONS} -D"
    LONG_NAME=y
fi

# ★ 默认模式（无参数时）：采集 1 次，间隔 1 秒
if [ $# = 0 ] && [ "${BOOT}" = "n" ]; then
    exec ${ENDIR}/sadc -F -L ${SADC_OPTIONS} 1 1 ${SA_DIR}
else
    exec ${ENDIR}/sadc -F -L ${SADC_OPTIONS} $* ${SA_DIR}
fi
```

**关键参数解释：**

| sadc 参数 | 含义 |
|----------|------|
| `-F` | ★ 强制写入（即使数据未变化） |
| `-L` | 文件锁（防止多个 sadc 同时写入） |
| `-S DISK` | 采集磁盘统计（来自 SADC_OPTIONS 配置） |
| `1 1` | 间隔 1 秒，采集 1 次 |
| `/var/log/sa` | 输出目录（sadc 自动根据日期选择文件） |

---

## 2. sa 二进制文件格式

### 2.1 文件结构

```
sa 文件 (/var/log/sa/saXX) 结构
┌──────────────────────────────────────────────┐
│  文件头 (File Header)                         │
│  ├── magic number (0xdabb)                    │
│  ├── 版本号 (sysstat version)                 │
│  ├── 系统信息 (hostname, machine, cpu count)  │
│  ├── 操作系统名称 (Linux)                     │
│  ├── 内核版本                                 │
│  └── 采集配置 (哪些统计被采集)                 │
├──────────────────────────────────────────────┤
│  记录 1 (Record 1) — 时间戳 T1               │
│  ├── 时间戳 (struct tm)                       │
│  ├── 特殊标记 (R_NONE / R_RESTART / ...)      │
│  ├── CPU 统计 (struct stats_cpu)              │
│  ├── 内存统计 (struct stats_memory)           │
│  ├── 磁盘统计 (struct stats_disk[])           │
│  ├── 网络统计 (struct stats_net_dev[])         │
│  ├── 中断统计 (struct stats_irq[])             │
│  ├── 分页统计 (struct stats_paging)           │
│  ├── 交换统计 (struct stats_swap)             │
│  ├── 队列统计 (struct stats_queue)            │
│  └── ... (其他统计结构)                       │
├──────────────────────────────────────────────┤
│  记录 2 (Record 2) — 时间戳 T2               │
│  ├── ...                                      │
├──────────────────────────────────────────────┤
│  ...                                          │
├──────────────────────────────────────────────┤
│  记录 N (Record N) — 时间戳 TN               │
│  └── ...                                      │
└──────────────────────────────────────────────┘
```

### 2.2 文件头关键字段

```
字段                    含义                        示例值
──────────────────────  ──────────────────────────  ──────────────────
sa_magic                魔数                         0xdabb
sa_sysstat_version      sysstat 版本                 12.7.6
sa_nrcpus               CPU 核心数                   4
sa_machine              架构                         x86_64
sa_sysname              操作系统                     Linux
sa_nodename             主机名                       iZbp11r90r4g931c...
sa_release              内核版本                     6.6.102-5.3.1.alnx4...
sa_act_types            采集的统计类型位掩码          INT|DISK|...
```

### 2.3 LINUX RESTART 标记

当系统重启后，sysstat.service 调用 `sa1 --boot`，在 sa 文件中写入一条特殊记录：

```
11:23:24 PM  LINUX RESTART	(4 CPU)
```

这条记录告诉 sar：**之前的计数器数据来自上一轮启动**，不能与新启动后的数据混合计算差值。

### 2.4 验证 sa 文件格式

```bash
# 用 sadf 查看文件头
sadf -H /var/log/sa/sa03

# 输出：
# File header:
# sysstat version: 12.7.6
# file format: 0x2172
# ...

# 用 file 命令
file /var/log/sa/sa03
# 输出: sysstat activity data (version 12.7.6, little-endian)
```

---

## 3. 采集间隔配置

### 3.1 默认配置（systemd timer）

```
sysstat-collect.timer
  OnCalendar=*:00/10    ← ★ 每 10 分钟采集一次
  (即 :00, :10, :20, :30, :40, :50)

sysstat-summary.timer
  OnCalendar=00:07:00   ← ★ 每天 00:07 生成报告
```

**为什么是 10 分钟？**
- 平衡数据精度与磁盘开销
- 10 分钟间隔 × 144 条/天 × ~2KB/条 ≈ 288KB/天/文件
- 28 天 ≈ 8MB，可接受

### 3.2 修改采集间隔

**方法 1: 修改 systemd timer（推荐）**

```bash
# 编辑 timer 文件
systemctl edit sysstat-collect.timer

# 改为每 1 分钟采集：
[Timer]
OnCalendar=*:00/1
```

**方法 2: 使用 cron（传统方式）**

```bash
# /etc/cron.d/sysstat（某些发行版使用）
# 每 10 分钟运行一次
*/10 * * * * root /usr/lib64/sa/sa1 1 1

# 每天 23:53 生成报告
53 23 * * * root /usr/lib64/sa/sa2 -A
```

### 3.3 /etc/sysconfig/sysstat 配置详解

```bash
# /etc/sysconfig/sysstat — sysstat 主配置文件

# ★ 数据保留天数（默认 28 天）
HISTORY=28

# ★ 超过多少天压缩（默认 31 天）
COMPRESSAFTER=31

# ★ sadc 采集选项
# -S DISK:  采集磁盘统计
# -S INT:   采集中断统计
# -S ALL:   采集所有统计
SADC_OPTIONS=" -S DISK"

# 数据文件目录
SA_DIR=/var/log/sa

# 压缩工具
ZIP="xz"

# 文件权限掩码
UMASK=0022
```

**SADC_OPTIONS 常用值：**

| 选项 | 含义 | 建议 |
|------|------|------|
| `-S DISK` | 采集磁盘统计 | ★ 默认，必选 |
| `-S INT` | 采集中断统计 | 需要排查中断时开启 |
| `-S IPV6` | 采集 IPv6 统计 | 使用 IPv6 时开启 |
| `-S POWER` | 采集功耗统计 | 笔记本/服务器 |
| `-S SNMP` | 采集 SNMP 统计 | 网络设备监控 |
| `-S XDISK` | 采集扩展磁盘统计 | 需要分区级别数据 |
| `-S ALL` | 采集所有统计 | ★ 全面采集（推荐） |

---

## 4. sadc 的采集机制

### 4.1 采集流程

```
sadc 的采集循环
─────────────────────────────────────────────────────────────

对于每次采集 (sample):
  │
  ├── 1. 读取计数器快照 (Snapshot A)
  │      read_stat_cpu()    → /proc/stat         → CPU 时间
  │      read_stat_memory() → /proc/meminfo       → 内存使用
  │      read_stat_disk()   → /proc/diskstats     → 磁盘 I/O
  │      read_stat_net()    → /proc/net/dev       → 网络收发
  │      read_stat_irq()    → /proc/interrupts    → 中断计数
  │      read_stat_paging() → /proc/vmstat        → 页面换入换出
  │      read_stat_swap()   → /proc/vmstat        → 交换活动
  │      read_stat_queue()  → /proc/loadavg       → 负载/队列
  │      ...
  │
  ├── 2. sleep(interval)
  │
  ├── 3. 读取计数器快照 (Snapshot B)
  │
  ├── 4. 计算差值 (B - A)
  │      ★ 差值 = 本采集间隔内的增量
  │      ★ sar 输出的是"每秒速率"（差值 / interval）
  │
  └── 5. 写入 sa 文件（二进制结构体数组）
```

### 4.2 差值计算的陷阱

```
计数器类型          处理方式              示例
────────────────  ────────────────────  ──────────────────
累计型 (counter)   B - A = 增量          /proc/stat 中的 CPU 时间
                    转为每秒速率          user 时间 (jiffies)

瞬时型 (gauge)     直接记录 B 的值       /proc/meminfo 中的内存
                    不做差值              kbmemfree, kbcached

比率型 (ratio)     从增量计算百分比       %user = user_diff / total_diff * 100
                    分母为总 jiffies      %memused = used / total * 100
```

**关键：sar 输出的 "%user" 不是绝对百分比，而是采样间隔内的比率。**

### 4.3 为什么 sar 和 top 的 CPU 数值不同

```
top:  默认 3 秒刷新，实时计算
sar:  默认取 sadc 的 10 分钟间隔差值

示例：
  0-3 秒:   CPU 100% (top 看到)
  3-600 秒: CPU 0%
  sar (10 分钟间隔): (100*3 + 0*597) / 600 = 0.5%

★ 结论：sar 的 10 分钟粒度可能掩盖短时尖峰
  → 需要细粒度时：sar -u 1 5 (实时 1 秒采样)
```

---

## 5. sar 的两种工作模式

### 5.1 实时模式（不指定 -f）

```bash
sar -u 1 5
```

```
sar 进程
 │
 ├── fork() 子进程执行 sadc
 │     sadc 1 5 → 输出到 stdout (二进制流)
 │
 ├── sar 从 stdin 读取 sadc 的二进制输出
 │
 ├── 解析每条记录
 │
 └── 格式化输出到终端
```

### 5.2 历史模式（指定 -f）

```bash
sar -u -f /var/log/sa/sa03
```

```
sar 进程
 │
 ├── 打开 /var/log/sa/sa03 (二进制文件)
 │
 ├── 读取文件头 → 验证 magic number + 版本
 │
 ├── 逐条读取记录
 │
 ├── 如果指定 -s / -e → 过滤时间范围
 │
 └── 格式化输出到终端
```

### 5.3 时间范围过滤

```bash
# 只看 14:00 到 16:00 的数据
sar -u -f /var/log/sa/sa15 -s 14:00:00 -e 16:00:00

# 每 5 分钟显示一个采样点（跳过中间的）
sar -u -f /var/log/sa/sa15 -i 300
```

---

## 6. 与其他工具的数据源对比

### 6.1 完整数据源映射表

| sar 参数 | 内核数据源 | iostat 对应 | vmstat 对应 | mpstat 对应 |
|---------|-----------|------------|------------|------------|
| `-u` | `/proc/stat` | — | cpu 行 | mpstat -P ALL |
| `-u ALL` | `/proc/stat` | — | — | — |
| `-P ALL` | `/proc/stat` | — | — | ★ 完全等价 |
| `-r` | `/proc/meminfo` | — | — | — |
| `-B` | `/proc/vmstat` | — | ★ 等价 | — |
| `-W` | `/proc/vmstat` | — | si/so 列 | — |
| `-d` | `/proc/diskstats` | ★ 等价 | — | — |
| `-b` | `/proc/diskstats` | — | bi/bo 列 | — |
| `-n DEV` | `/proc/net/dev` | — | — | — |
| `-I SUM` | `/proc/interrupts` | — | in 列 | — |
| `-q` | `/proc/loadavg` + `/proc/stat` | — | r/b 列 | — |
| `-w` | `/proc/stat` | — | cs 列 | — |

### 6.2 选型指南

```
需要什么？                    用哪个工具？
────────────────────────────  ──────────────────
快速看一眼系统全貌            sar -A 1 5
查看历史数据（昨天/上周）      sar -u -f /var/log/sa/saXX
只看磁盘 I/O 细节              iostat -xz 1
看哪个进程在消耗 CPU           pidstat -u 1
看单个 CPU 核心的实时状态      mpstat -P ALL 1
看虚拟内存 + 综合              vmstat 1
一次性导出所有历史为 CSV       sadf -d saXX -- -u > csv
```

---

## 7. 总结

### 调用链

```
systemd timer (每 10 分钟)
    │
    ▼
sa1 (shell) ──→ sadc (C) ──→ /var/log/sa/saXX (二进制)
                                    │
                                    ├──→ sar (C) ──→ 终端输出
                                    ├──→ sadf (C) ──→ CSV/JSON/SVG
                                    └──→ sa2 (shell) ──→ /var/log/sa/sarXX (文本)

用户手动：
sar -u 1 5 ──→ fork sadc ──→ stdin ──→ 终端输出
```

### 核心认知

| 要点 | 说明 |
|------|------|
| ★ sadc 是唯一采集器 | sa1/sa2 只是 shell 包装 |
| ★ sa 文件是二进制格式 | 不能直接 cat/less，必须用 sar/sadf 读取 |
| ★ 默认 10 分钟粒度 | 可能掩盖短时尖峰，需要时用实时模式 |
| ★ sar 输出的是"每秒速率" | 差值 / 采样间隔 |
| ★ 历史查询是 sar 的杀手锏 | iostat/top/vmstat 都没有历史数据能力 |
