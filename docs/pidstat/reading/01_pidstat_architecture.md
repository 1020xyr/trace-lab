# pidstat 工作原理

> pidstat 不采集数据——它只是 `/proc/[pid]/` 文件的格式化视图。
> 理解这一点，是掌握 pidstat 的关键。

---

## 一、整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     pidstat 用户态                           │
│  1. 遍历 /proc/[pid]/ 目录                                   │
│  2. 读取 stat/status/io/schedstat 文件                      │
│  3. 解析字段、计算差值（间隔模式）                            │
│  4. 格式化输出表格                                           │
├─────────────────────────────────────────────────────────────┤
│                     /proc 文件系统（内核虚拟文件系统）         │
│  /proc/[pid]/stat      → CPU 时间、页错误                    │
│  /proc/[pid]/status    → 内存、上下文切换                    │
│  /proc/[pid]/io        → I/O 字节数                          │
│  /proc/[pid]/schedstat → 调度等待时间                        │
├─────────────────────────────────────────────────────────────┤
│                     内核 task_struct                         │
│  utime/stime/min_flt/maj_flt/nvcsw/nivcsw                  │
│  每次系统调用、中断、调度时由内核更新                          │
└─────────────────────────────────────────────────────────────┘
```

### 与 blktrace 的核心差异

| 维度 | blktrace | pidstat |
|------|----------|---------|
| 数据来源 | 内核 tracepoint + relay buffer | /proc 文件系统 |
| 采集方式 | 事件驱动（每个 I/O 产生事件） | 轮询驱动（周期性读取） |
| 精度 | 纳秒级时间戳 | 秒级采样间隔 |
| 开销 | 需内核 hook + 大量 buffer | 仅读文件，几乎无开销 |
| 数据粒度 | 每个 I/O 请求 | 每个进程的聚合速率 |
| 工具链 | blktrace → blkparse → btt | pidstat 一体化 |

---

## 二、数据来源详解

### 2.1 /proc/[pid]/stat — CPU 和页错误

```bash
$ cat /proc/1/stat
1 (systemd) S 0 1 1 0 -1 4194560 127241 79618744 100 158274
  905 1071 771861 184018 20 0 1 0 16 22130688 3158 ...
  ↑       ↑  ↑  ↑                                  ↑    ↑
  PID    comm状态 父PID                            utime stime
                                                      ↑     ↑
                                              用户态时间  内核态时间
                                              (clock_t)  (clock_t)
```

**pidstat 关心的字段（按位置编号）：**

| 字段号 | 名称 | pidstat 用途 |
|--------|------|-------------|
| 1 | pid | PID 列 |
| 2 | comm | Command 列 |
| 10 | minflt | minflt/s（次页错误率） |
| 12 | majflt | majflt/s（主页错误率） |
| 14 | utime | %usr（用户态 CPU 时间） |
| 15 | stime | %system（内核态 CPU 时间） |
| 16 | cutime | 子进程用户态时间（-T ALL 时使用） |
| 17 | cstime | 子进程内核态时间 |
| 39 | processor | CPU 列（最后运行在哪个 CPU） |
| 42 | guest_time | %guest（虚拟机客户时间） |

**时间单位：** clock_t（通常 100Hz，即 1 clock_t = 10ms）。可通过 `getconf CLK_TCK` 验证。

**%CPU 计算公式：**
```
%CPU = (utime_new - utime_old + stime_new - stime_old) / (interval × CLK_TCK) × 100
```

### 2.2 /proc/[pid]/io — I/O 字节数

```bash
$ cat /proc/1/io
rchar: 157209793551           ← 读字符数（含缓存）
wchar: 114101323407           ← 写字符数（含缓存）
syscr: 35795360               ← read 系统调用次数
syscw: 26783505               ← write 系统调用次数
read_bytes: 12862489600       ← ★ 实际从磁盘读取的字节数
write_bytes: 32842510336      ← ★ 实际写入磁盘的字节数
cancelled_write_bytes: 276742144  ← 被截断取消的写入
```

**pidstat 关注的字段：**

| 字段 | pidstat 列 | 说明 |
|------|-----------|------|
| read_bytes | kB_rd/s | 实际从磁盘读（不含 page cache 命中） |
| write_bytes | kB_wr/s | 实际写到磁盘（不含 page cache 缓冲） |
| cancelled_write_bytes | kB_ccwr/s | 文件截断导致的取消写入 |

**kB_rd/s 计算公式：**
```
kB_rd/s = (read_bytes_new - read_bytes_old) / interval / 1024
```

**iodelay 来源：** 来自 `/proc/[pid]/schedstat` 或 `/proc/[pid]/sched` 中的 I/O 等待时间累积。

### 2.3 /proc/[pid]/status — 内存和上下文切换

```bash
$ grep -E "Vm|ctxt" /proc/1/status
VmPeak:    22130 kB     ← 虚拟内存峰值
VmSize:    21612 kB     ← 当前虚拟内存大小（VSZ）
VmRSS:     12632 kB     ← 常驻集大小（RSS）
VmSwap:        0 kB     ← 交换到 swap 的大小
voluntary_ctxt_switches:    158274   ← ★ 自愿上下文切换累积次数
nonvoluntary_ctxt_switches: 184018   ← ★ 非自愿上下文切换累积次数
```

**pidstat 关注的字段：**

| 字段 | pidstat 列 | 说明 |
|------|-----------|------|
| VmSize | VSZ | 虚拟内存大小（KB） |
| VmRSS | RSS | 物理内存大小（KB） |
| voluntary_ctxt_switches | cswch/s | 自愿切换（I/O 等待、sleep） |
| nonvoluntary_ctxt_switches | nvcswch/s | 非自愿切换（时间片耗尽、抢占） |

### 2.4 /proc/[pid]/schedstat — 调度等待时间

```bash
$ cat /proc/1/schedstat
5983621349 125344604 406038
  ↑         ↑        ↑
  运行时间  等待时间  运行次数
  (ns)      (ns)     (次)
```

**pidstat 的 %wait 来源：** 等待时间（第二个字段）的差值除以采样间隔。

```
%wait = (wait_new - wait_old) / (interval × 10^9) / nr_cpu × 100
```

---

## 三、采样差值机制

### 无间隔模式（默认）

```bash
pidstat -u    # 输出从启动到当前的累积平均值
```

```
进程启动时的 /proc 值
         ↓
当前的 /proc 值
         ↓
差值 / 进程存活时间 = 平均速率
```

### 间隔模式

```bash
pidstat -u 2 5    # 每 2 秒采样一次，共 5 次
```

```
T0: 读取 /proc/[pid]/stat → 记录 utime_0, stime_0
    │
    ↓ 等待 2 秒
    │
T1: 读取 /proc/[pid]/stat → 记录 utime_1, stime_1
    │
    ↓ 计算差值
    │
    %usr = (utime_1 - utime_0) / (2 × CLK_TCK) × 100
    %system = (stime_1 - stime_0) / (2 × CLK_TCK) × 100
    │
    ↓ 输出 T1 行
    │
T2: 读取 → 计算 T2-T1 差值 → 输出 T2 行
...
T5: 输出 Average: 行（所有采样的平均）
```

**重要：只有活跃的进程才会出现在输出中。** 如果某个进程在采样间隔内没有任何 CPU/IO 活动，pidstat 不会显示它（减少输出噪音）。

---

## 四、进程枚举机制

```
pidstat 如何找到所有进程？

1. 打开 /proc/ 目录
2. 遍历所有数字命名的子目录（每个数字 = PID）
3. 对每个 PID，尝试读取 /proc/[pid]/stat
4. 如果读取成功 → 进程存活，收集数据
5. 如果读取失败（ENOENT） → 进程已退出，跳过
6. 如果指定了 -C 过滤 → 检查 comm 是否匹配正则表达式
7. 如果指定了 -p → 只处理指定的 PID
```

**注意：** 短命进程可能被遗漏。如果进程在两次采样之间启动并退出，pidstat 永远不会看到它。这是轮询式监控的固有限制。

---

## 五、pidstat 与内核的关系图

```
┌────────────────────────────────────────────────────────────┐
│                    内核维护的计数器                          │
│                                                            │
│  task_struct {                                             │
│    utime ────────→ /proc/[pid]/stat 字段 14                │
│    stime ────────→ /proc/[pid]/stat 字段 15                │
│    min_flt ──────→ /proc/[pid]/stat 字段 10                │
│    maj_flt ──────→ /proc/[pid]/stat 字段 12                │
│    nvcsw ────────→ /proc/[pid]/status 的 voluntary_ctxt_switches   │
│    nivcsw ───────→ /proc/[pid]/status 的 nonvoluntary_ctxt_switches │
│    io_acct {                                               │
│      rchar ──────→ /proc/[pid]/io 的 rchar                 │
│      wchar ──────→ /proc/[pid]/io 的 wchar                 │
│      read_bytes ─→ /proc/[pid]/io 的 read_bytes            │
│      write_bytes → /proc/[pid]/io 的 write_bytes           │
│    }                                                       │
│  }                                                         │
│                                                            │
│  mm_struct {                                               │
│    total_vm ─────→ /proc/[pid]/status 的 VmSize            │
│    rss ──────────→ /proc/[pid]/status 的 VmRSS             │
│  }                                                         │
└────────────────────────────────────────────────────────────┘
         │
         ▼
    pidstat 读取 + 差值计算 → 输出表格
```

---

## 六、关键限制

| 限制 | 原因 | 影响 |
|------|------|------|
| **短命进程遗漏** | 轮询式采样，两次采样间退出的进程看不到 | 可能漏掉短任务 |
| **精度有限** | 秒级采样，无法捕捉毫秒级突发 | 看不到瞬时峰值 |
| **I/O 只看到磁盘层** | read_bytes/write_bytes 是实际磁盘 I/O | 看不到 page cache 命中 |
| **线程 vs 进程** | 默认只显示主线程（TGID） | 需 `-t` 看所有线程 |
| **权限限制** | 普通用户只能看自己的进程 | root 看全部 |
| **CPU 百分比上限** | %CPU 可以超过 100%（多线程进程） | 4 核机器上最大 400% |

**思考题：**
1. 为什么 pidstat 的 %CPU 有时候与 top 的 %CPU 不一致？
   （答：top 的 %CPU 是两次刷新间的差值，刷新间隔可能不是整秒；pidstat 基于精确的 interval 参数）
2. 为什么有些进程的 kB_wr/s 为 0 但确实在写文件？
   （答：写入了 page cache，还没刷到磁盘。write_bytes 只计算实际磁盘 I/O）
3. %wait 高但 %CPU 低意味着什么？
   （答：进程大部分时间在等待 CPU 调度——系统 CPU 过载）
