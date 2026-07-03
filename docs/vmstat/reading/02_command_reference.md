# vmstat 命令参考手册

> ★ 本文件覆盖 vmstat (procps-ng 4.0.4) 的全部参数、使用场景和常见陷阱。
> 预计阅读时间：15 分钟

---

## 目录

- [一、参数分类总览](#一参数分类总览)
- [二、采样控制参数](#二采样控制参数)
- [三、输出模式参数](#三输出模式参数)
- [四、单位控制参数](#四单位控制参数)
- [五、输出列详解](#五输出列详解)
- [六、场景命令速查](#六场景命令速查)
- [七、vmstat vs top vs sar vs mpstat 对比](#七vmstat-vs-top-vs-sar-vs-mpstat-对比)
- [八、常见陷阱](#八常见陷阱)
- [九、组合使用技巧](#九组合使用技巧)

---

## 一、参数分类总览

```
vmstat [采样控制] [输出模式] [单位] [延迟 [次数]]
```

| 分类     | 参数                              | 作用                 |
|---------|----------------------------------|---------------------|
| 采样控制 | `delay count` / `-n` / `-a`      | 控制采样间隔、次数、表头  |
| 输出模式 | `-s` / `-d` / `-D` / `-p` / `-t` | 切换不同统计视图        |
| 宽输出   | `-w`                             | 列对齐，适合大屏/日志    |
| 单位控制 | `-S k/K/m/M`                     | 内存列的显示单位        |
| 统计模式 | `-a`                             | 用 active/inactive 替换 buff/cache |

---

## 二、采样控制参数

### 2.1 基本语法：`vmstat [delay [count]]`

| 形式                | 含义                                   |
|--------------------|---------------------------------------|
| `vmstat`           | 输出一次（自启动以来的平均值）              |
| `vmstat 1`         | 每秒采样，无限循环（Ctrl+C 终止）          |
| `vmstat 1 5`       | 每秒采样，共 5 次后自动退出                |
| `vmstat 5 10`      | 每 5 秒采样，共 10 次                     |

★ **delay** 以秒为单位，**count** 是采样次数。count 仅在指定 delay 时有效。

### 2.2 `-n`：表头只显示一次

| 形式              | 含义                                    |
|------------------|----------------------------------------|
| `vmstat -n 1`    | 每秒采样，表头只在第一次输出时打印           |
| `vmstat 1`       | 默认行为：每次窗口重绘时重复打印表头         |

★ `-n` 对长时间采集、重定向到文件非常有用——避免表头混在数据中间。

### 2.3 `-a`：显示 active/inactive 内存

默认模式下 `buff` 和 `cache` 列被替换为 `inact` 和 `active`：

| 列名     | 含义                                     |
|---------|-----------------------------------------|
| `inact` | 不活跃内存页数（长时间未访问，可回收）         |
| `active`| 活跃内存页数（近期访问，内核倾向保留）         |

---

## 三、输出模式参数

### 3.1 `-s`：系统摘要统计（一次输出）

显示自启动以来的累计统计值，包括内存、交换分区、I/O、CPU、中断、上下文切换等。
**不进入采样循环**，等价于 `cat /proc/stat /proc/meminfo /proc/vmstat` 的精选汇总。

```bash
vmstat -s
```

### 3.2 `-d`：磁盘统计

从 `/proc/diskstats` 读取每块磁盘的读/写/IO 统计：

| 列名       | 含义                                     |
|-----------|-----------------------------------------|
| `total`   | 读/写操作总次数                             |
| `merged`  | 合并的读/写请求数（内核将相邻请求合并）        |
| `sectors` | 读/写的扇区数（每扇区 512 字节）              |
| `ms`      | 读/写耗时（毫秒）                           |
| `cur`     | 当前正在进行的 I/O 数                       |
| `sec`     | I/O 花费的总时间（秒）                      |

### 3.3 `-D`：磁盘汇总统计

将所有磁盘的统计值**加总**为一行，适合快速查看整体 I/O 负载。

### 3.4 `-p <partition>`：分区统计

显示指定分区的读写统计，格式同 `-d` 但只针对单个设备。

```bash
vmstat -p /dev/vdb
vmstat -p sda1
```

### 3.5 `-t`：附加时间戳

在每行输出末尾追加当前时间戳（格式 `CST` 即系统时区时间）。
适合事后分析时定位异常发生的具体时间点。

```bash
vmstat -t 1 60    # 每分钟采样，带时间戳
```

### 3.6 `-w`：宽输出模式

加大列宽，使数字对齐更清晰。在列数多、数值大时特别有用。

```bash
vmstat -w 1 10
```

---

## 四、单位控制参数

`-S` 控制内存相关列（`swpd`/`free`/`buff`/`cache` 或 `inact`/`active`）的显示单位：

| 参数    | 单位           | 换算                          |
|--------|---------------|-------------------------------|
| `-S k` | KiB (1024)    | 原始值，默认行为                  |
| `-S K` | KB  (1000)    | 以 1000 为基数换算               |
| `-S m` | MiB (1048576) | 原始值 / 1024                  |
| `-S M` | MB  (1000000) | 原始值 / 1000000               |

★ **小写 = 二进制前缀（Ki/Mi），大写 = 十进制前缀（K/M）**。
这个区分在内存数值较大时影响明显。

**注意：** `-S` 不影响 swap I/O（`si`/`so`）和 block I/O（`bi`/`bo`）列，这两组始终以 block/s 为单位。

---

## 五、输出列详解

### 默认模式（`vmstat 1`）列说明

```
procs -----------memory---------- ---swap-- -----io---- -system-- -------cpu-------
 r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st gu
```

| 列名   | 全称                      | 单位         | 数据来源               |
|-------|--------------------------|-------------|----------------------|
| `r`   | runnable processes       | 进程数       | `/proc/stat`         |
| `b`   | blocked processes        | 进程数       | `/proc/stat`         |
| `swpd`| swap used                | KiB         | `/proc/meminfo`      |
| `free`| free memory              | KiB         | `/proc/meminfo`      |
| `buff`| buffer memory            | KiB         | `/proc/meminfo`      |
| `cache`| page cache              | KiB         | `/proc/meminfo`      |
| `si`  | swap in                  | KiB/s       | `/proc/vmstat`       |
| `so`  | swap out                 | KiB/s       | `/proc/vmstat`       |
| `bi`  | blocks in (from disk)    | blocks/s    | `/proc/vmstat`       |
| `bo`  | blocks out (to disk)     | blocks/s    | `/proc/vmstat`       |
| `in`  | interrupts               | 次/秒       | `/proc/stat`         |
| `cs`  | context switches         | 次/秒       | `/proc/stat`         |
| `us`  | user CPU                 | %           | `/proc/stat`         |
| `sy`  | system CPU               | %           | `/proc/stat`         |
| `id`  | idle CPU                 | %           | `/proc/stat`         |
| `wa`  | IO-wait CPU              | %           | `/proc/stat`         |
| `st`  | stolen CPU (虚拟化)       | %           | `/proc/stat`         |
| `gu`  | guest CPU (KVM 客户机)    | %           | `/proc/stat`         |

★ **r** = 可运行（在 run queue 中等待或正在执行），**b** = 不可中断睡眠（通常是等 I/O）。

---

## 六、场景命令速查

### 场景 1：快速看一眼系统状态

```bash
vmstat               # 一次性快照（启动以来平均值）
vmstat 1 1           # 1 秒内的真实瞬时值
```

### 场景 2：持续监控 CPU 和内存

```bash
vmstat 1 60          # 每分钟采集 60 次
vmstat -w 1          # 宽输出，持续监控
```

### 场景 3：检查是否发生 swap

```bash
vmstat 1 5           # 关注 si/so 列，非零说明有交换
vmstat -s | grep swap # 累计交换统计
```

### 场景 4：分析 I/O 瓶颈

```bash
vmstat -d            # 各磁盘读写统计
vmstat -D            # 磁盘汇总
vmstat 1 10          # 关注 wa% 和 bi/bo
```

### 场景 5：诊断高 CPU 但低吞吐

```bash
vmstat 1 30          # 关注 cs（上下文切换）和 in（中断）
```

★ cs > 50000/s 通常说明锁竞争或过多短任务。

### 场景 6：记录到文件并事后分析

```bash
vmstat -n -t 1 3600 > /tmp/vmstat_$(date +%Y%m%d_%H%M).log
# -n: 表头只一次  -t: 带时间戳  3600 次 = 1 小时
```

### 场景 7：大内存服务器可读输出

```bash
vmstat -S m 1 10     # 以 MiB 显示内存列
vmstat -S M 1 10     # 以 MB 显示内存列
```

### 场景 8：区分活跃/不活跃内存

```bash
vmstat -a 1 5        # active/inactive 替换 buff/cache
```

### 场景 9：定位特定分区 I/O

```bash
vmstat -p /dev/vdb   # 指定设备的读写统计
vmstat -p sda1
```

### 场景 10：配合 fio 压力测试

```bash
vmstat 1 &
fio --name=stress --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=10 --time_based --iodepth=32
wait
```

### 场景 11：排查虚拟化偷取时间

```bash
vmstat 1 10          # 关注 st 列，非零说明宿主机争抢 CPU
```

### 场景 12：长时间基线采集（自动化）

```bash
nohup vmstat -n -t 5 > /var/log/vmstat_baseline.log 2>&1 &
# 每 5 秒采样一次，后台运行
```

---

## 七、vmstat vs top vs sar vs mpstat 对比

| 特性              | vmstat         | top            | sar             | mpstat          |
|------------------|---------------|---------------|-----------------|-----------------|
| **所属包**        | procps-ng     | procps-ng     | sysstat         | sysstat         |
| **默认安装**      | ✅ 几乎所有发行版 | ✅ 几乎所有发行版 | ❌ 需安装        | ❌ 需安装        |
| **CPU 细分**      | us/sy/id/wa/st | us/sy/id/wa/st/hi/si | 最完整      | 每 CPU 核心分别   |
| **内存概览**      | swpd/free/buff/cache | RES/VIRT/SHR 进程级 | 系统级     | ❌ 不涉及        |
| **I/O 统计**      | bi/bo (block) | ❌ 仅 D 状态进程数 | 磁盘+网络      | ❌ 不涉及        |
| **上下文切换**     | cs 列          | ❌             | cswch/s         | ❌              |
| **中断数**        | in 列          | ❌             | intr/s          | ❌              |
| **磁盘详情**      | `-d` / `-D`    | ❌             | `-d` 完整磁盘    | ❌              |
| **进程级信息**     | ❌             | ✅ 核心功能     | ❌              | ❌              |
| **历史记录**       | ❌ 实时        | ❌ 实时        | ✅ 自动归档      | ✅ 自动归档      |
| **脚本友好**       | ✅ 纯文本       | 需 `-b` 批处理 | ✅ 纯文本        | ✅ 纯文本        |
| **采样精度**       | 秒级           | 秒级           | 秒级            | 秒级            |
| **适用场景**       | 系统全局快速诊断 | 进程级 CPU/内存 | 长期性能基线     | CPU 核心级分析   |

### 什么时候用哪个？

```
需要快速看系统整体状态？         → vmstat 1
需要找哪个进程在吃 CPU/内存？    → top
需要长期性能数据和历史回放？      → sar
需要看每个 CPU 核心的负载差异？   → mpstat -P ALL 1
需要分析 I/O 瓶颈？             → vmstat -d + iostat -x
需要看上下文切换是否过高？        → vmstat 1（cs 列）
```

---

## 八、常见陷阱

### ★ 陷阱 1：首次输出是启动以来的平均值

**现象：** `vmstat` 不带参数或 `vmstat 1 5` 的第一行数据与后续行差异巨大。

**原因：** 第一行输出的是**自系统启动以来的累计平均值**，不是瞬时值。

```
procs -----------memory---------- ---swap-- -----io---- -system-- -------cpu-------
 r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st gu
 0  0      0 1103476  50268 3771464    0    0    17   175 1200    5  1  0 98  0  0  0
                                     ↑    ↑    ↑    ↑    ↑    ↑ ← 这些是启动以来的平均！
 1  0      0 1102640  50268 3771480    0    0     0     0 5130 7086 19  4 77  0  0  0
                                     ↑ ← 第二行起才是真实瞬时值
```

**对策：**
- 分析时**跳过第一行**
- 或使用 `vmstat 1 2` 取第二行作为首个有效数据点
- 脚本中可用 `vmstat 1 2 | tail -1` 获取单次有效读数

### ★ 陷阱 2：`-n` 只显示表头一次

**现象：** 加 `-n` 后只有第一行有表头，后续行没有。

**这是预期行为，不是 bug。** `-n` 的全称是 "no header repeat"。

**对策：** 如果需要在终端反复查看表头，不加 `-n`；如果是重定向到文件后做文本处理，加 `-n`。

### ★ 陷阱 3：`bi`/`bo` 的单位是 block/s，不是 KB/s

**现象：** `bi` 列显示 64000，以为是 64000 KB/s，实际是 64000 blocks/s。

**说明：** 在大多数系统中 1 block = 1024 bytes（即 1 KB），所以数值上 `bi` ≈ KB/s。
但这**不是规范保证**——取决于内核版本和块设备配置。

**对策：** 如果需要精确的 KB/s，使用 `iostat -k` 或 `sar -d`。

### ★ 陷阱 4：`r` 列包含正在运行 + 等待运行的进程

**现象：** `r = 10` 但只有 4 个 CPU 核心。

**说明：** `r` 是 runqueue 长度，包括：
- 当前正在各 CPU 上执行的进程
- 在 run queue 中等待被调度的进程

★ `r > CPU 核心数` 说明 CPU 资源紧张，进程在排队。

### ★ 陷阱 5：`wa` 高不一定是磁盘慢

**现象：** `wa = 30%` 但磁盘 IOPS 不高。

**可能原因：**
- NFS 挂载点无响应（网络 I/O 也计入 wa）
- 虚拟磁盘（virtio-blk）等待宿主机 I/O
- swap 读写（si/so 非零时 wa 也会升高）

### ★ 陷阱 6：内存列的单位受 `-S` 影响，但 I/O 列不受

**现象：** `vmstat -S M` 时以为 `bi` 也是 MB/s。

**说明：** `-S` 只影响 `swpd`/`free`/`buff`/`cache`（或 `inact`/`active`）。
`si`/`so`/`bi`/`bo` 始终以 KiB/s 或 blocks/s 为单位。

### ★ 陷阱 7：不带参数时 `vmstat` 只输出一次就退出

**现象：** 输入 `vmstat` 后只看到一行数据就返回了。

**说明：** 不带 delay/count 参数时，vmstat 默认只输出一次系统平均值。

**对策：** 使用 `vmstat 1` 持续监控，或 `vmstat 1 10` 采集 10 次。

---

## 九、组合使用技巧

### 9.1 脚本化监控

```bash
# 每小时采集，带时间戳，后台运行
nohup vmstat -n -t 1 3600 > /tmp/vmstat_$(date +%H%M).log 2>&1 &
```

### 9.2 配合 awk 提取关键列

```bash
# 提取 wa 列（第 16 列），超过 10% 时告警
vmstat 1 60 | awk 'NR>3 && $16>10 {print "wa ALERT:", $0}'
```

### 9.3 配合 gnuplot 画图

```bash
vmstat -n 1 300 > /tmp/vmstat_data.txt
# 然后用 gnuplot 画 cs 列趋势图
gnuplot -e "plot '/tmp/vmstat_data.txt' skip 2 using 12 with lines title 'context switches'"
```

### 9.4 与 fio 联动做 I/O 基准测试

```bash
# 同时采集 vmstat 和 fio
vmstat -n -t 1 > /tmp/vmstat_during_fio.log &
fio --name=bench --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=30 --time_based --iodepth=64 --numjobs=4 \
    --group_reporting --output-format=json --output=/tmp/fio_result.json
wait
# 对比 vmstat 的 bo/wa/cs 与 fio 的 IOPS/latency
```

---

## 附录：参数速查表

| 参数               | 作用                                | 示例                    |
|-------------------|-------------------------------------|------------------------|
| `vmstat`          | 一次性系统平均值快照                    | `vmstat`               |
| `vmstat delay count` | 周期采样                          | `vmstat 1 10`          |
| `-n`              | 表头只显示一次                         | `vmstat -n 1`          |
| `-a`              | 显示 active/inactive 内存             | `vmstat -a 1 5`        |
| `-s`              | 系统摘要统计（累计值）                  | `vmstat -s`            |
| `-d`              | 磁盘统计                              | `vmstat -d`            |
| `-D`              | 磁盘汇总统计                          | `vmstat -D`            |
| `-p <dev>`        | 分区/设备统计                         | `vmstat -p /dev/vdb`   |
| `-t`              | 附加时间戳                            | `vmstat -t 1`          |
| `-w`              | 宽输出模式                            | `vmstat -w 1 10`       |
| `-S k/K/m/M`      | 内存列单位                           | `vmstat -S m 1`        |
