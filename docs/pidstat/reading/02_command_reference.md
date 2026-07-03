# pidstat 完整命令参考

> 所有参数详解、字段速查表、10+ 场景命令组合。
> 基于 sysstat 12.7.6，内核 6.6.102。

---

## 一、基本用法

```bash
pidstat [选项] [间隔秒数 [采样次数]] [-e <程序> <参数>]
```

**两种工作模式：**

```
模式 1：无间隔参数（默认）
  pidstat -u              → 报告从系统启动到当前的累积平均值

模式 2：指定间隔
  pidstat -u 2 5          → 每 2 秒采样一次，共 5 次
                            输出每次采样的"区间速率" + 最终 Average: 行
```

---

## 二、核心参数详解

### 统计维度选择

| 参数 | 含义 | 输出列 | /proc 数据来源 |
|------|------|--------|---------------|
| `-u` | ★ CPU 使用率 | %usr/%system/%guest/%wait/%CPU/CPU | `/proc/[pid]/stat` 字段 14,15,42 + `schedstat` |
| `-d` | ★ I/O 统计 | kB_rd/s/kB_wr/s/kB_ccwr/s/iodelay | `/proc/[pid]/io` 的 read_bytes/write_bytes |
| `-r` | ★ 内存与页错误 | minflt/s/majflt/s/VSZ/RSS/%MEM | `/proc/[pid]/stat` 字段 10,12 + `status` 的 Vm* |
| `-w` | ★ 上下文切换 | cswch/s/nvcswch/s | `/proc/[pid]/status` 的 voluntary/nonvoluntary_ctxt_switches |
| `-s` | 栈大小 | StkSize/StkRef | `/proc/[pid]/status` 的 VmStk |
| `-v` | 内核表信息 | threads/fd-nr | `/proc/[pid]/status` 的 Threads + `/proc/[pid]/fd/` 计数 |
| `-R` | 实时优先级 | prio/policy | `sched_getparam()` 系统调用 |

**多个维度可组合：** `pidstat -u -d -r -w` 一次性输出 CPU + I/O + 内存 + 上下文切换。

### 进程过滤

| 参数 | 含义 | 示例 |
|------|------|------|
| `-p <pid>` | 只追踪指定 PID | `pidstat -u -p 1234` |
| `-p ALL` | 显示所有进程（含不活跃的） | `pidstat -u -p ALL` |
| `-p SELF` | 只追踪 pidstat 自身 | `pidstat -u -p SELF` |
| `-C <pattern>` | ★ 按命令名正则过滤（ERE） | `pidstat -C "fio" -u` |
| `-G <name>` | 按进程组名过滤 | `pidstat -G systemd -u` |
| `-U [username]` | 按用户名过滤（显示 USER 而非 UID） | `pidstat -U root -u` |

### 显示控制

| 参数 | 含义 | 效果 |
|------|------|------|
| `-t` | ★ 显示线程级（TID） | 输出 TGID + TID 两列，每个线程单独一行 |
| `-T TASK` | 只显示线程（同 -t） | 等价于 -t |
| `-T CHILD` | 显示子进程累积 | 包含 cutime/cstime |
| `-T ALL` | 线程 + 子进程 | 同时显示 TGID/TID 和子进程时间 |
| `-l` | ★ 显示完整命令行 | Command 列显示完整 `/proc/[pid]/cmdline` |
| `-h` | 不显示头部信息 | 输出无标题行（适合脚本解析） |
| `-H` | 每个采样周期都显示头部 | 每次采样重复输出列标题 |
| `-I` | ★ 按 CPU 数分割 %CPU | %CPU = 原始值 / CPU 数量（SMP 系统上更直观） |
| `--human` | 人类可读单位 | 自动缩放大数值（K/M/G） |
| `--dec={0\|1\|2}` | 小数位数 | 控制输出精度（默认 2 位） |

### 特殊参数

| 参数 | 含义 | 示例 |
|------|------|------|
| `-e <cmd>` | 执行命令并追踪其生命周期 | `pidstat -u -e sleep 5` |

---

## 三、输出字段速查表

### -u CPU 字段

```
字段      含义                                数据来源
────────  ──────────────────────────────────  ──────────────────────
UID       进程所属用户 ID                     /proc/[pid]/status (Uid)
PID       进程 ID                             /proc/[pid]/stat 字段 1
%usr      用户态 CPU 时间占比（不含 nice）     (utime_diff) / (interval × CLK_TCK) × 100
%system   内核态 CPU 时间占比                  (stime_diff) / (interval × CLK_TCK) × 100
%guest    虚拟机客户操作系统占用 CPU 时间       (guest_time_diff) / (interval × CLK_TCK) × 100
%wait     ★ 等待 CPU 调度的时间占比            schedstat 的 wait_sum 差值
%CPU      %usr + %system + %guest 的总和       计算值
CPU       进程最后运行在哪个 CPU 上             /proc/[pid]/stat 字段 39 (processor)
Command   进程名（/proc/[pid]/comm）           /proc/[pid]/stat 字段 2
```

### -d I/O 字段

```
字段       含义                                数据来源
─────────  ──────────────────────────────────  ──────────────────────
kB_rd/s    每秒读取速率 (KB/s)                 (read_bytes_diff) / interval / 1024
kB_wr/s    每秒写入速率 (KB/s)                 (write_bytes_diff) / interval / 1024
kB_ccwr/s  取消的写入速率 (KB/s)               (cancelled_write_bytes_diff) / interval / 1024
iodelay    I/O 延迟 (ms)                       /proc/[pid]/schedstat 中的阻塞时间
Command    进程名                              /proc/[pid]/stat 字段 2
```

**注意：** kB_rd/s 和 kB_wr/s 只统计**实际到达磁盘的 I/O**（经过 block layer 的字节数），
不包含 page cache 命中的读写。这意味着：
- 读取命中 page cache → read_bytes 不增加 → kB_rd/s 为 0
- 写入停留在 page cache（未刷盘） → write_bytes 不增加 → kB_wr/s 为 0

### -r 内存字段

```
字段      含义                                数据来源
────────  ──────────────────────────────────  ──────────────────────
minflt/s  次页错误率（次/秒）                  (minflt_diff) / interval
          从内存分配页面，无磁盘 I/O
majflt/s  主页错误率（次/秒）                  (majflt_diff) / interval
          需要从磁盘加载页面（有磁盘 I/O）
VSZ       虚拟内存大小 (KB)                    /proc/[pid]/status → VmSize
RSS       常驻内存大小 (KB)                    /proc/[pid]/status → VmRSS
%MEM      RSS 占总物理内存的百分比             RSS / 总物理内存 × 100
Command   进程名                              /proc/[pid]/stat 字段 2
```

### -w 上下文切换字段

```
字段       含义                                数据来源
─────────  ──────────────────────────────────  ──────────────────────
cswch/s    自愿上下文切换率（次/秒）            voluntary_ctxt_switches 差值 / interval
           进程主动让出 CPU：
           - I/O 阻塞等待
           - sleep/usleep/nanosleep
           - 等待锁/信号量
           - 等待子进程
nvcswch/s  非自愿上下文切换率（次/秒）          nonvoluntary_ctxt_switches 差值 / interval
           进程被调度器强制换下：
           - 时间片用完
           - 高优先级进程抢占
Command    进程名                              /proc/[pid]/stat 字段 2
```

### -t 线程字段

```
字段      含义
────────  ────────────────────────
TGID      线程组 ID（= 主线程的 PID）
TID       线程 ID（每个线程唯一）
```

输出中 TGID 行（TID 为 `-`）是进程级汇总，TID 行（TGID 为 `-`）是线程级明细。

### -R 实时优先级字段

```
字段      含义
────────  ────────────────────────
prio      实时优先级（0-99，越大越高）
policy    调度策略（FIFO/RR/IDLE/OTHER）
```

### -s 栈大小字段

```
字段       含义
─────────  ────────────────────────────
StkSize    栈分配大小 (KB)，来自 VmStk
StkRef     栈实际引用大小 (KB)
```

### -v 内核表字段

```
字段       含义
─────────  ────────────────────────────
threads    线程数（/proc/[pid]/status → Threads）
fd-nr      打开的文件描述符数（/proc/[pid]/fd/ 目录条目计数）
```

---

## 四、场景命令组合

### 场景 1：查看所有活跃进程的 CPU 使用

```bash
# 累积平均（从系统启动至今）
pidstat -u

# 间隔采样（每 2 秒，共 5 次）
pidstat -u 2 5
```

### 场景 2：找出 I/O 最密集的进程

```bash
# 按 kB_wr/s 排序，取 Top 10
pidstat -d 2 3 | sort -k4 -rn | head -15

# 只追踪特定进程
pidstat -d -p $(pgrep fio) 1 10
```

### 场景 3：内存泄漏检测

```bash
# 每 5 秒采样一次，观察 RSS 是否持续增长
pidstat -r -p <PID> 5 60

# 观察 minflt/s — 如果持续很高说明在频繁分配内存
pidstat -r -C "java" 5 12
```

### 场景 4：上下文切换热点分析

```bash
# 找出上下文切换最频繁的进程
pidstat -w 2 3 | sort -k4 -rn | head -20

# 区分自愿 vs 非自愿
# cswch/s 高 → I/O 密集型（正常）
# nvcswch/s 高 → CPU 竞争激烈（需要优化）
pidstat -w -C "nginx" 2 5
```

### 场景 5：综合画像 — 一次看全部指标

```bash
# CPU + I/O + 内存 + 上下文切换
pidstat -u -d -r -w 2 5
```

### 场景 6：线程级分析

```bash
# 显示所有线程（多线程程序很有用）
pidstat -t -C "java" -u 2 5

# 线程 + 子进程
pidstat -t -T ALL -u -p <PID>
```

### 场景 7：命令过滤 + 完整路径

```bash
# 正则过滤进程名
pidstat -C "kworker" -u

# 显示完整命令行参数
pidstat -l -C "sshd" -u
```

### 场景 8：追踪命令执行全过程

```bash
# 执行命令并追踪其 CPU/IO/内存
pidstat -u -d -r -e fio --name=test --ioengine=sync --direct=1 \
    --rw=randwrite --bs=4k --filename=/tmp/testfile --size=100M --runtime=5
```

### 场景 9：按 CPU 数分割 %CPU

```bash
# 4 CPU 系统上，单核满载进程显示 %CPU=100% 而非 25%
# 使用 -I 后，%CPU = 原始值 / CPU 数量
pidstat -I -u 2 5

# 对比：
# 无 -I：%CPU=100.00（一个核满载）
# 有 -I：%CPU=25.00（= 100/4，直观看到占整体的比例）
```

### 场景 10：脚本友好的输出

```bash
# -h 不显示头部，适合 awk 直接处理
pidstat -h -u -C "fio" 2 5 | awk '{print $1, $3, $7}'

# 只输出特定列
pidstat -h -d 2 5 | awk '{printf "%s PID=%s rd=%.1f wr=%.1f\n", $1, $3, $4, $5}'
```

### 场景 11：按用户名过滤

```bash
# 只看 root 用户的进程
pidstat -U root -u 2 5

# 输出 USER 列（而非 UID 数字）
pidstat -U -u 2 5
```

### 场景 12：与 blktrace 互补分析

```bash
# pidstat 看"哪个进程在产生 I/O"
pidstat -d -C "fio" 2 5

# blktrace 看"块设备层面的 I/O 详情"
blktrace -d /dev/vdb -o trace -w 10
blkparse -i trace -o parsed.txt

# 两者结合：
# pidstat → 进程级 I/O 速率（kB_wr/s）
# blktrace → 设备级 I/O 事件（每个 request 的 D/C 事件、rwbs、扇区号）
```

---

## 五、参数完整列表

```
pidstat [ options ] [ <interval> [ <count> ] ] [ -e <program> <args> ]

统计维度:
  -u           CPU 使用率
  -d           I/O 统计
  -r           内存与页错误
  -w           上下文切换
  -s           栈大小
  -v           内核表信息（threads, fd-nr）
  -R           实时调度优先级

进程过滤:
  -p <pid>     指定 PID（支持逗号分隔多个 PID，或 ALL/SELF）
  -C <regex>   按命令名正则过滤（ERE）
  -G <name>    按进程组名过滤
  -U [user]    按用户名过滤

显示控制:
  -t           显示线程级（TGID + TID）
  -T {TASK|CHILD|ALL}  线程/子进程/全部
  -l           显示完整命令行
  -h           不显示头部
  -H           每次采样都显示头部
  -I           %CPU 除以 CPU 数量
  --human      人类可读单位
  --dec={0|1|2} 小数位数

特殊:
  -e <cmd>     执行并追踪指定命令
  -V           显示版本
```

---

## 六、常见陷阱

### 陷阱 1：kB_wr/s 为 0 但进程确实在写文件

**原因：** `write_bytes` 只统计**实际到达磁盘**的字节数。如果写入停留在 page cache（未触发 writeback），`write_bytes` 不增加。

```bash
# 验证：直接看 /proc
cat /proc/<PID>/io
# write_bytes: 0     ← 还没刷盘
# wchar: 1048576     ← 但确实写了 1MB（到 page cache）
```

**解决：** 使用 `fsync` 或 `O_DIRECT` 确保数据到达磁盘，或观察 `kB_ccwr/s`（取消写入）作为间接证据。

### 陷阱 2：短命进程被遗漏

**原因：** pidstat 是轮询式采样。如果进程在两次采样之间启动并退出，pidstat 永远不会看到它。

```bash
# 示例：短任务可能在采样间隙消失
pidstat -u 5 3    # 5 秒采样一次
# 一个运行 2 秒的进程可能正好在两次采样之间退出
```

**解决：** 缩短采样间隔（`pidstat -u 1`），或使用 `-e` 追踪命令执行全过程。

### 陷阱 3：%CPU 可以超过 100%

**原因：** 多线程进程可以在多个 CPU 上并行运行。4 核系统上，一个使用全部 4 核的进程 `%CPU = 400%`。

```bash
# 使用 -I 让 %CPU 表示"占总 CPU 资源的百分比"
pidstat -I -u    # 4 核系统上，满载进程 %CPU = 100%（= 400/4）
```

### 陷阱 4：Direct I/O 到块设备时 I/O 统计可能不准

**原因：** `read_bytes`/`write_bytes` 的计数与 block layer 的 I/O 会计机制有关。
对裸块设备的 O_DIRECT 写入可能不更新 `write_bytes` 计数器（因为绕过了文件系统层）。

```bash
# 示例：fio 直写 /dev/vdb
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=5
cat /proc/<FIO_PID>/io
# write_bytes: 0    ← ★ 直接写块设备时可能为 0

# 改为写文件
fio --name=test --ioengine=sync --rw=randwrite --bs=4k \
    --filename=/tmp/testfile --size=100M --runtime=5 --fsync=10
cat /proc/<FIO_PID>/io
# write_bytes: <非零>  ← 通过文件系统的 I/O 能正确统计
```

**解决：** 对块设备级 I/O 统计，使用 `blktrace` 而非 `pidstat -d`。两者互补（见场景 12）。

### 陷阱 5：iodelay 值经常为 0

**原因：** `iodelay` 来自 `/proc/[pid]/schedstat` 中的 I/O 阻塞等待时间累积。
如果进程使用了异步 I/O（libaio）或 I/O 很快完成（如 virtio-blk），iodelay 可能为 0。

```bash
# 验证
cat /proc/<PID>/schedstat
# 第二个字段 = 等待时间（纳秒）
# 如果这个值在两次采样间没有变化 → iodelay = 0
```

---

## 七、与 top/ps/vmstat 的功能对比

```
功能                 pidstat    top      ps       vmstat    iostat
───────────────────  ─────────  ───────  ───────  ────────  ────────
逐进程 CPU           ✅         ✅       ✅       ❌        ❌
逐进程 I/O           ✅ ★       ❌       ❌       ❌        ❌
逐进程 页错误        ✅ ★       ❌       ❌       ❌        ❌
逐进程 上下文切换    ✅ ★       ❌       ❌       ❌        ❌
%wait (CPU 等待)     ✅ ★       ❌       ❌       ❌        ❌
间隔采样             ✅         ✅       ❌       ✅        ✅
线程级               ✅         部分     ✅ -L    ❌        ❌
命令过滤             ✅         交互搜索 grep     ❌        ❌
脚本友好             ✅         ❌       ✅       ✅        ✅
系统级汇总           ❌         ✅       ❌       ✅        ✅
```

**pidstat 的独特价值：** 它是唯一能同时提供**逐进程 I/O + 上下文切换 + 页错误 + %wait**的命令行工具。
