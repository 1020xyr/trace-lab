# sar (System Activity Reporter) 学习路线

> 从 sadc 后台采集到 sa 二进制文件，从 sar 查询到全维度系统画像

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 数据采集架构](#2-数据采集架构)
- [3. 与 sysstat 家族其他工具的关系](#3-与-sysstat-家族其他工具的关系)
- [4. 学习路线](#4-学习路线)
- [5. 动手实验清单](#5-动手实验清单)
- [6. 关键文件索引](#6-关键文件索引)
- [7. 参考资源](#7-参考资源)

---

## 1. 整体架构

```
┌───────────────────────────────────────────────────────────────────┐
│                        用户查询层                                  │
│  sar -u 1 5 (实时)    sar -u -f /var/log/sa/sa01 (历史)           │
│  读取 sa 二进制文件 → 解析 → 格式化输出                             │
├───────────────────────────────────────────────────────────────────┤
│                        数据存储层                                  │
│  /var/log/sa/saXX (每日二进制文件，XX = 日期)                      │
│  /var/log/sa/sarXX (每日文本报告，由 sa2 生成)                     │
├───────────────────────────────────────────────────────────────────┤
│                        数据采集层                                  │
│  systemd timer (每 10 分钟)                                       │
│    → sysstat-collect.service → sa1 脚本 → sadc 二进制             │
│  sadc: 读取 /proc + /sys 计数器 → 写入 sa 二进制文件               │
├───────────────────────────────────────────────────────────────────┤
│                        内核数据源                                  │
│  /proc/stat /proc/meminfo /proc/net/dev /proc/interrupts          │
│  /proc/diskstats /proc/vmstat /proc/loadavg /sys/block/*/stat    │
└───────────────────────────────────────────────────────────────────┘
```

### 数据流全路径

```
 内核 (/proc + /sys)
     │
     │  读取计数器快照
     ▼
 sadc (System Activity Data Collector)
     │  /usr/lib64/sa/sadc
     │  - 每次读取所有计数器
     │  - 计算差值（增量）
     │  - 写入二进制格式
     │
     ▼
 /var/log/sa/saXX  (XX = 当月日期，如 sa03)
     │  二进制格式，每天一个文件
     │  包含多个时间戳记录
     │
     ├──→ sar (实时查询)
     │    sar -u 1 5 → 实时采集并显示
     │    sar -r -f /var/log/sa/sa03 → 读历史
     │
     ├──→ sadf (数据格式转换)
     │    sadf -d → CSV 格式
     │    sadf -x → XML 格式
     │    sadf -g → SVG 图表
     │
     └──→ sa2 (每日报告生成)
          sa2 -A → /var/log/sa/sar03 (文本报告)
          每天 00:07 由 systemd timer 触发
```

### sar 的一次查询做了什么

```
sar -u 1 5
 │
 ├── 1. 不指定 -f → 实时模式
 │      调用 sadc 作为子进程
 │      sadc 读 /proc/stat → 间隔 1 秒 → 再读一次
 │      计算差值 → 输出给 sar
 │
 ├── 2. 解析 sadc 输出的二进制数据
 │
 ├── 3. 格式化为表格输出
 │      ┌────────────────────────────────────────────────────────┐
 │      │ 11:24:15 PM  CPU  %user  %nice  %system  %iowait  ... │
 │      │ 11:24:16 PM  all  20.20   0.00    4.04    0.00    ... │
 │      │ 11:24:17 PM  all  17.79   0.00    4.76    0.00    ... │
 │      │ ...                                                    │
 │      │ Average:     all  18.10   0.00    4.14    0.05    ... │
 │      └────────────────────────────────────────────────────────┘
 │
 └── 4. 输出 Average 行（所有采样的算术平均）
```

---

## 2. 数据采集架构

### 2.1 三层工具链

| 工具 | 路径 | 角色 | 触发方式 |
|------|------|------|---------|
| **sadc** | `/usr/lib64/sa/sadc` | ★ 核心采集器（C 语言二进制） | 被 sa1 调用 |
| **sa1** | `/usr/lib64/sa/sa1` | shell 包装脚本，设置参数后 exec sadc | systemd timer |
| **sa2** | `/usr/lib64/sa/sa2` | shell 脚本，调用 sar 生成每日文本报告 | systemd timer |
| **sar** | `/usr/bin/sar` | 查询/显示工具（C 语言二进制） | 用户手动执行 |

### 2.2 sadc 采集原理

```
sadc 1 5 /tmp/sa_test
 │
 │  interval = 1 秒, count = 5 次, 输出文件 = /tmp/sa_test
 │
 ├── 读取 /proc/stat 快照 T0 (CPU 时间计数器)
 │   读取 /proc/meminfo 快照 T0 (内存使用)
 │   读取 /proc/net/dev 快照 T0 (网络接口计数器)
 │   读取 /proc/diskstats 快照 T0 (磁盘 I/O 计数器)
 │   读取 /proc/vmstat 快照 T0 (虚拟内存统计)
 │   读取 /proc/interrupts 快照 T0 (中断计数)
 │   读取 /proc/loadavg 快照 T0 (负载均值)
 │   ...
 │
 ├── 写入文件头（系统信息、采集参数）
 │
 ├── 第 1 次采集 (T1):
 │   读取所有计数器 T1
 │   计算 T1 - T0 的差值（增量）
 │   写入一条二进制记录
 │   sleep 1
 │
 ├── 第 2 次采集 (T2): T2 - T1
 ├── 第 3 次采集 (T3): T3 - T2
 ├── 第 4 次采集 (T4): T4 - T3
 └── 第 5 次采集 (T5): T5 - T4
```

### 2.3 systemd 定时采集

```
systemctl enable sysstat
 │
 ├── sysstat.service (开机时)
 │     ExecStart: sa1 --boot
 │     → 在当前 sa 文件写入 "LINUX RESTART" 标记
 │
 ├── sysstat-collect.timer (每 10 分钟)
 │     OnCalendar=*:00/10
 │     → 触发 sysstat-collect.service
 │       → ExecStart: sa1 1 1
 │         → sadc -F -L -S DISK 1 1 /var/log/sa
 │
 ├── sysstat-summary.timer (每天 00:07)
 │     OnCalendar=00:07:00
 │     → 触发 sysstat-summary.service
 │       → ExecStart: sa2 -A
 │         → sar -A -f saXX > sarXX (文本报告)
 │
 └── sysstat-rotate.timer (每月 1 日)
       → 归档旧的 sa 文件
```

### 2.4 数据文件

```
/var/log/sa/
├── sa01    ← 每月 1 号的二进制数据（sadc 写入）
├── sa02    ← 每月 2 号的二进制数据
├── ...
├── sa03    ← 当天数据（正在写入）
├── sar01   ← 每月 1 号的文本报告（sa2 生成）
├── sar02   ← 每月 2 号的文本报告
└── ...

保留策略：
  HISTORY=28         → 保留 28 天
  COMPRESSAFTER=31   → 超过 31 天的压缩
  HISTORY > 28       → 使用长文件名 (saYYYYMMDD)
```

---

## 3. 与 sysstat 家族其他工具的关系

### 3.1 同一数据源，不同视角

所有 sysstat 工具读取的内核数据源**完全相同**，只是展示维度不同：

```
┌─────────────────────────────────────────────────────────────┐
│                    /proc + /sys 数据源                       │
│  stat  meminfo  vmstat  net/dev  diskstats  interrupts  ... │
└──────┬──────────┬──────────┬──────────┬──────────┬──────────┘
       │          │          │          │          │
       ▼          ▼          ▼          ▼          ▼
   ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐
   │ sar  │  │iostat│  │pidsta│  │mpsta │  │cifsiu│
   │      │  │      │  │  t   │  │  t   │  │  til │
   └──────┘  └──────┘  └──────┘  └──────┘  └──────┘
   全维度    磁盘I/O   进程级    CPU级    CIFS/NFS
   历史+实时  实时     实时      实时     实时
```

### 3.2 工具对比表

| 工具 | 数据源 | 采集方式 | 历史查询 | 核心用途 |
|------|--------|---------|---------|---------|
| **sar** | /proc 全部 | sadc 后台 + 实时 | ✅ `-f saXX` | 全维度系统画像 |
| **iostat** | /proc/diskstat + /proc/stat | 实时采集 | ❌ | 磁盘 I/O 专项 |
| **pidstat** | /proc/[pid]/stat | 实时采集 | ❌ | 进程级监控 |
| **mpstat** | /proc/stat | 实时采集 | ❌ | CPU 核心级监控 |
| **vmstat** | /proc/vmstat + /proc/stat | 实时采集 | ❌ | 虚拟内存 + 系统 |
| **tapestat** | /sys/class/scsi_tape | 实时采集 | ❌ | 磁带设备 |
| **cifsiostat** | /proc/fs/cifs | 实时采集 | ❌ | CIFS 文件系统 I/O |

### 3.3 sar vs iostat 输出对比

```
# iostat -x 1 1 的输出
Device     r/s     w/s    rkB/s    wkB/s  rrqm/s  wrqm/s  %rrqm  %wrqm  r_await  w_await  aqu-sz  rareq-sz  wareq-sz  svctm  %util
vda        0.00    4.00     0.00    68.00    0.00    0.00   0.00   0.00     0.00     0.67    0.00      0.00     17.00   0.67    0.27

# sar -d -p 1 1 的输出（相同数据，不同列名）
       tps     rkB/s     wkB/s     dkB/s   areq-sz    aqu-sz     await     %util DEV
      3.00      0.00     68.00      0.00     22.67      0.00      0.67      0.20 vda
```

**关键差异：**
- iostat 有更多细节（rrqm/wrqm、r_await/w_await 分离）
- sar 更简洁，但支持历史查询（iostat 不支持）
- sar -d 的 `await` 是读写合并值，iostat 的 `r_await`/`w_await` 是分开的

---

## 4. 学习路线

### Step 1: 理解架构 — sadc/sa1/sa2/sar 的关系

**目标：** 搞清楚四个工具各自的职责和调用关系

**阅读材料：** `reading/01_sar_architecture.md`

**阅读要点：**
1. sadc 是唯一的数据采集器（C 二进制）
2. sa1 是 sadc 的 shell 包装（配置参数 + 路径处理）
3. sa2 调用 sar 生成每日文本报告
4. sar 既支持实时查询，也支持历史文件读取

**实操：**
```bash
# 查看 sa1 脚本内容
cat /usr/lib64/sa/sa1

# 查看 systemd timer 配置
systemctl cat sysstat-collect.timer
systemctl cat sysstat-collect.service

# 手动用 sadc 采集
/usr/lib64/sa/sadc 1 5 /tmp/sa_test
sar -u -f /tmp/sa_test
```

### Step 2: 掌握命令参数 — 按维度分类记忆

**目标：** 熟悉 sar 的所有参数类别，能根据需求快速选择

**阅读材料：** `reading/02_command_reference.md`

**记忆方法：按维度分组**
```
CPU:     -u / -u ALL / -P ALL
内存:    -r / -r ALL / -B / -W / -S / -H
I/O:     -b / -d / -p
网络:    -n DEV / -n EDEV / -n SOCK / -n TCP / -n ETCP
队列:    -q / -q ALL
中断:    -I SUM / -I XALL
进程:    -w
```

### Step 3: 实战输出解读 — 理解每列含义

**目标：** 看到 sar 输出时，能逐列解释含义并判断是否异常

**阅读材料：** `reading/03_command_output_demo.md`

**核心指标速查：**

| 指标 | 命令 | 异常阈值 | 含义 |
|------|------|---------|------|
| %iowait | `sar -u` | > 10% | CPU 等 I/O |
| %system | `sar -u` | > 30% | 内核开销大 |
| %memused | `sar -r` | > 90% | 内存不足 |
| majflt/s | `sar -B` | > 0 | 磁盘换页（严重） |
| await | `sar -d -p` | > 20ms | 磁盘延迟高 |
| %util | `sar -d -p` | > 80% | 磁盘饱和 |
| runq-sz | `sar -q` | > CPU 核数 | 进程排队 |
| ldavg-1 | `sar -q` | > CPU 核数 | 负载过高 |

### Step 4: 历史数据分析 — 故障回溯

**目标：** 利用 sa 历史文件进行故障回溯和趋势分析

**实操：**
```bash
# 查看今天的 CPU 使用趋势
sar -u -f /var/log/sa/sa$(date +%d)

# 查看某天下午 2 点到 4 点的内存
sar -r -f /var/log/sa/sa15 -s 14:00:00 -e 16:00:00

# 导出为 CSV 供 Excel 分析
sadf -d /var/log/sa/sa01 -- -u > cpu_data.csv

# 生成 SVG 图表
sadf -g /var/log/sa/sa01 -- -u > cpu_chart.svg
```

### Step 5: 与其他工具联动 — 深度排查

**目标：** sar 发现异常后，用 iostat/pidstat 等工具深入定位

```
sar 发现 %iowait > 20%
  │
  ├──→ iostat -xz 1    → 哪块磁盘 await 高
  │
  ├──→ pidstat -d 1     → 哪个进程在做 I/O
  │
  └──→ blktrace        → I/O 路径中哪个阶段慢

sar 发现 %system > 50%
  │
  ├──→ mpstat -P ALL 1  → 是所有 CPU 还是个别 CPU
  │
  ├──→ pidstat -u 1     → 哪个进程 %sys 高
  │
  └──→ perf top         → 内核函数热点

sar 发现 runq-sz > 8
  │
  ├──→ pidstat 1        → 哪些进程在 R/D 状态
  │
  └──→ ps aux | sort -k3 -rn → CPU 消耗大户
```

---

### Step 6: 性能诊断工作流 — 从 sar 到专业工具

**目标：** 掌握从 sar 发现异常到用专业工具深入定位的完整流程

**阅读材料：** `reading/04_diagnostic_workflow.md`

**核心技能：**
- sar 综合诊断流程（全维度扫描 → 定位异常维度 → 专业工具深入）
- CPU 高占用低吞吐的 sar 排查（%user/%system/%soft/%iowait 分类）
- 网络软中断 CPU 不均衡排查（sar -P ALL + sar -n EDEV）
- sar 与 perf/bpftrace/iostat 的联动策略

**诊断矩阵：**

```
sar 异常指标                    │ 深入工具
───────────────────────────────┼──────────────────────
%usr 高 + IPC 低              │ perf stat -d
%sys 高 + cswch/s 高          │ perf lock + bpftrace
%soft 高 + CPU 不均衡         │ irqbalance + RSS
%iowait 高 + await 高         │ iostat -xz + blktrace
runq-sz > CPU 核数            │ pidstat + ps
```

---

## 5. 动手实验清单

### 实验 1: 基础采集与查询

```bash
#!/bin/bash
# 实验 1: 手动采集 + 查询

# 1. 确保 sysstat 服务启动
systemctl start sysstat
systemctl enable sysstat

# 2. 手动用 sadc 采集 10 秒
/usr/lib64/sa/sadc 1 10 /tmp/sa_lab1

# 3. 用 sar 读取
sar -u -f /tmp/sa_lab1          # CPU
sar -r -f /tmp/sa_lab1          # 内存
sar -d -f /tmp/sa_lab1          # 磁盘
sar -n DEV -f /tmp/sa_lab1      # 网络

# 4. 对比实时查询
sar -u 1 5
```

### 实验 2: 负载生成 + sar 检测

```bash
#!/bin/bash
# 实验 2: 生成负载，用 sar 检测

# 终端 1: 启动 CPU 压力
stress-ng --cpu 2 --timeout 10 &

# 终端 2: 启动 I/O 压力
stress-ng --io 2 --timeout 10 &

# 终端 3: sar 实时监测
sar -u -r -d -p 1 15
```

**预期观察：**
- `stress-ng --cpu` → %user 飙升
- `stress-ng --io` → %iowait 飙升 + 磁盘 %util 上升

### 实验 3: 历史回溯

```bash
#!/bin/bash
# 实验 3: 利用历史数据回溯

# 1. 等待 sysstat 采集一些数据（至少 30 分钟）
sleep 1800

# 2. 查看今天的 CPU 趋势
TODAY=$(date +%d)
sar -u -f /var/log/sa/sa${TODAY}

# 3. 只看下午 2 点到 3 点的数据
sar -u -f /var/log/sa/sa${TODAY} -s 14:00:00 -e 15:00:00

# 4. 导出 CSV
sadf -d /var/log/sa/sa${TODAY} -- -u > /tmp/cpu_trend.csv
head -5 /tmp/cpu_trend.csv
```

### 实验 4: sar + iostat + pidstat 联动排查

```bash
#!/bin/bash
# 实验 4: 多工具联动

# 1. 后台启动 fio 产生 I/O
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/vdb --runtime=15 --time_based &

# 2. sar 检测 I/O 维度
sar -d -p 1 10 &

# 3. iostat 深入看磁盘
iostat -xz 1 10 &

# 4. pidstat 看哪个进程在做 I/O
pidstat -d 1 10 &

wait
```

### 实验 5: 数据格式转换与可视化

```bash
#!/bin/bash
# 实验 5: sadf 数据格式转换

# 1. 采集数据
/usr/lib64/sa/sadc 1 30 /tmp/sa_lab5

# 2. 导出 CSV
sadf -d /tmp/sa_lab5 -- -u > /tmp/cpu.csv
sadf -d /tmp/sa_lab5 -- -r > /tmp/mem.csv

# 3. 导出 JSON
sadf -j /tmp/sa_lab5 -- -u > /tmp/cpu.json

# 4. 生成 SVG 图表
sadf -g /tmp/sa_lab5 -- -u > /tmp/cpu.svg
sadf -g /tmp/sa_lab5 -- -r > /tmp/mem.svg
```

---

## 6. 关键文件索引

### 系统配置文件

| 文件 | 作用 |
|------|------|
| `/etc/sysconfig/sysstat` | ★ 主配置：保留天数、采集选项、压缩策略 |
| `/usr/lib/systemd/system/sysstat.service` | 开机标记 LINUX RESTART |
| `/usr/lib/systemd/system/sysstat-collect.timer` | ★ 每 10 分钟触发采集 |
| `/usr/lib/systemd/system/sysstat-collect.service` | 执行 `sa1 1 1` |
| `/usr/lib/systemd/system/sysstat-summary.timer` | 每天 00:07 触发报告 |
| `/usr/lib/systemd/system/sysstat-summary.service` | 执行 `sa2 -A` |

### 工具二进制与脚本

| 文件 | 类型 | 作用 |
|------|------|------|
| `/usr/lib64/sa/sadc` | C 二进制 | ★ 核心采集器 |
| `/usr/lib64/sa/sa1` | shell 脚本 | sadc 的包装（配置 + 路径） |
| `/usr/lib64/sa/sa2` | shell 脚本 | 调用 sar 生成每日报告 |
| `/usr/bin/sar` | C 二进制 | ★ 查询/显示工具 |
| `/usr/bin/sadf` | C 二进制 | 数据格式转换（CSV/JSON/SVG） |

### 数据文件

| 路径 | 格式 | 说明 |
|------|------|------|
| `/var/log/sa/saXX` | 二进制 | 每日数据文件（sadc 写入） |
| `/var/log/sa/sarXX` | 文本 | 每日报告（sa2 生成） |

### 内核数据源

| 文件 | sar 参数 | 采集的数据 |
|------|---------|-----------|
| `/proc/stat` | `-u` `-P ALL` `-w` | CPU 时间、进程创建、上下文切换 |
| `/proc/meminfo` | `-r` `-r ALL` | 内存使用详情 |
| `/proc/vmstat` | `-B` `-W` | 页面换入换出、交换活动 |
| `/proc/diskstats` | `-d` `-b` | 磁盘 I/O 统计 |
| `/proc/net/dev` | `-n DEV` `-n EDEV` | 网络接口收发统计 |
| `/proc/net/sockstat` | `-n SOCK` | 套接字使用统计 |
| `/proc/net/tcp` | `-n TCP` `-n ETCP` | TCP 连接统计 |
| `/proc/interrupts` | `-I SUM` `-I XALL` | 中断计数 |
| `/proc/loadavg` | `-q` | 负载均值、运行队列 |
| `/sys/block/*/stat` | `-d` | 块设备统计（更精确） |

---

## 7. 参考资源

- [sysstat 官方 GitHub](https://github.com/sysstat/sysstat)
- [sysstat 官方文档](https://sysstat.sourceforge.io/)
- [sar(1) man page](https://man7.org/linux/man-pages/man1/sar.1.html)
- [sadc(8) man page](https://man7.org/linux/man-pages/man8/sadc.8.html)
- [sadf(1) man page](https://man7.org/linux/man-pages/man1/sadf.1.html)
- [sysstat(5) 配置文件 man page](https://man7.org/linux/man-pages/man5/sysstat.5.html)
- [Red Hat: Using sar to Monitor System Performance](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/8/html/managing_monitoring_and_updating_the_kernel/monitoring-system-performance-with-sar)
- [Linux Performance: sar 命令详解](https://linuxperf.com/)
- [sysstat 作者 Sebastien Godard 的技术文章](https://sysstat.sourceforge.io/articles.html)
