# pidstat 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐字段解读。
> 环境：Linux 6.6.102 / 4 CPU / 7.5GB RAM / sysstat 12.7.6
> I/O 负载由 dd 和 fio 生成。

---

## 场景 1：CPU 使用率 — pidstat -u

### 命令

```bash
pidstat -u
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:00:51 PM   UID       PID    %usr %system  %guest   %wait    %CPU   CPU  Command
11:00:51 PM     0         1    0.00    0.00    0.00    0.00    0.00     2  systemd
11:00:51 PM     0        17    0.00    0.02    0.00    0.01    0.02     3  rcu_preempt
11:00:51 PM     0        26    0.00    0.00    0.00    0.00    0.00     2  kworker/2:0H-kblockd
11:00:51 PM     0        77    0.00    0.00    0.00    0.00    0.00     3  kworker/3:1H-kblockd
11:00:51 PM     0       393    0.00    0.00    0.00    0.00    0.00     3  systemd-journal
```

### 逐字段解读

```
11:00:51 PM   → 采样时间戳
UID           → 进程所属用户 ID（0 = root）
PID           → 进程 ID
%usr          → 用户态 CPU 时间占比（不含 nice 值影响的时间）
%system       → 内核态 CPU 时间占比（系统调用、中断处理）
%guest        → 虚拟机客户操作系统占用的 CPU（物理机上始终为 0）
%wait         → ★ 等待 CPU 调度的时间占比（pidstat 独有！top/ps 看不到）
%CPU          → %usr + %system + %guest 的总和
CPU           → 进程最后运行在哪个 CPU 核上
Command       → 进程名（来自 /proc/[pid]/comm，最多 15 字符）
```

### 关键观察

```
1. rcu_preempt（PID 17）：%system=0.02, %wait=0.01
   → RCU 回调处理只在内核态运行，有少量 CPU 等待

2. kworker/2:0H-kblockd（PID 26）：所有值为 0
   → 在采样窗口内无 CPU 活动（块设备工作线程处于空闲状态）

3. systemd（PID 1）：所有值为 0
   → 累积模式下，长期运行的 idle 进程平均 CPU 趋近 0

4. 内核线程（kworker、ksoftirqd 等）：%usr 始终为 0
   → ★ 内核线程没有用户态，只有 %system
```

### %wait 的特殊价值

```
%wait > 0 的含义：进程想运行但 CPU 被其他进程占着
                    ↓
              系统的运行队列（runqueue）有竞争
                    ↓
              可能是 CPU 过载的信号

%wait 的来源：/proc/[pid]/schedstat 的第二个字段
              cat /proc/17/schedstat
              28076541756 518710559 25203
                ↑          ↑         ↑
                运行时间   等待时间   调度次数
                (纳秒)     (纳秒)    (次)

%wait 计算：(wait_new - wait_old) / (interval × 10^9) × 100
```

---

## 场景 2：I/O 统计 — pidstat -d

### 命令

```bash
pidstat -d
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:00:52 PM   UID       PID   kB_rd/s   kB_wr/s kB_ccwr/s iodelay  Command
11:00:52 PM     0         1     10.80     28.70      0.47       0  systemd
11:00:52 PM     0       293      0.00      0.91      0.00       0  jbd2/vda3-8
11:00:52 PM     0       393      0.00      4.30      0.00       0  systemd-journal
11:00:52 PM     0      1512      0.01      0.10      0.06       0  AliYunDunMonito
```

### 逐字段解读

```
kB_rd/s    → 每秒从磁盘读取的 KB 数
             来源：/proc/[pid]/io 的 read_bytes 差值 / interval / 1024

kB_wr/s    → 每秒写入磁盘的 KB 数
             来源：/proc/[pid]/io 的 write_bytes 差值 / interval / 1024

kB_ccwr/s  → 取消的写入 KB 数
             来源：cancelled_write_bytes（文件被截断时，之前排队的写操作被取消）

iodelay    → I/O 阻塞延迟（ms）
             来源：/proc/[pid]/schedstat 中的 I/O 等待时间
```

### 验证：直接查看 /proc 原始数据

```bash
$ cat /proc/393/io          # systemd-journal (PID 393)
rchar: 6030776              ← 读取字符总数（含 page cache 命中）
wchar: 122399               ← 写入字符总数（含 page cache 缓冲）
syscr: 61995                ← read() 系统调用次数
syscw: 631                  ← write() 系统调用次数
read_bytes: 4378624         ← ★ 实际从磁盘读的字节（= 4.2MB）
write_bytes: 7857393664     ← ★ 实际写到磁盘的字节（= 7.3GB）
cancelled_write_bytes: 0    ← 取消的写入
```

```
★ 关键对比：rchar vs read_bytes
  rchar = 6MB       → 进程"请求读取"了 6MB（大部分命中 page cache）
  read_bytes = 4MB  → 实际只有 4MB 触发了磁盘 I/O

★ 关键对比：wchar vs write_bytes
  wchar = 122KB     → 进程"请求写入"了 122KB
  write_bytes = 7.3GB → 但 journal 的 flush 操作导致大量磁盘写入
                        （journal 负责将内存中的日志刷到磁盘）
```

### iodelay 为 0 的原因

```
大多数进程的 iodelay = 0，原因：
1. 系统 I/O 压力小 → 进程等待 I/O 的时间很短
2. 使用异步 I/O → I/O 提交后不阻塞
3. 设备很快（virtio-blk/SSD）→ I/O 完成快
4. 写入 page cache → 还没触发实际磁盘 I/O

iodelay 明显 > 0 的场景：
- 慢速磁盘（HDD）上的同步 I/O
- I/O 密集负载导致队列拥塞
- 使用 O_DIRECT + sync 引擎的大量写操作
```

---

## 场景 3：内存统计 — pidstat -r

### 命令

```bash
pidstat -r
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:00:53 PM   UID       PID  minflt/s  majflt/s     VSZ     RSS   %MEM  Command
11:00:53 PM     0         1      0.07      0.00   21612   12632   0.17  systemd
11:00:53 PM     0       393      0.12      0.00   65784   39204   0.51  systemd-journal
11:00:53 PM     0       685      0.00      0.00  204420   19908   0.26  rsyslogd
11:00:53 PM     0       738      0.00      0.00  546960    8908   0.12  NetworkManager
11:00:53 PM     0     30645      0.47      0.00  690460   16816   0.22  aliyun-service.
```

### 逐字段解读

```
minflt/s → 次页错误率（次/秒）
           ★ 从内存中分配页面，不触发磁盘 I/O
           ★ 包括：首次访问已映射的页面、COW（写时复制）、零页分配
           ★ 数值较高是正常的（每次 mmap 后首次访问都会产生）

majflt/s → 主页错误率（次/秒）
           ★ 需要从磁盘加载页面（真正的磁盘 I/O）
           ★ 包括：读取 mmap 文件中尚未加载的页面、swap in
           ★ majflt/s > 0 持续存在 = 内存压力的信号

VSZ      → 虚拟内存大小 (KB)
           来源：/proc/[pid]/status → VmSize
           包含所有映射区域（代码、数据、共享库、mmap、栈）
           ★ VSZ 可以远大于物理内存（mmap 了大文件但未访问）

RSS      → 常驻内存大小 (KB)
           来源：/proc/[pid]/status → VmRSS
           ★ 进程实际占用的物理内存
           ★ 是内存泄漏检测的核心指标

%MEM     → RSS / 总物理内存 × 100
           本系统 7459MB 物理内存
```

### VSZ vs RSS 差异分析

```
进程              VSZ (KB)    RSS (KB)    比值    解读
───────────────  ──────────  ──────────  ──────  ──────────────────
systemd            21,612      12,632     0.58   紧凑进程，大部分映射都在物理内存
systemd-journal    65,784      39,204     0.60   journal 缓冲区部分未驻留
rsyslogd          204,420      19,908     0.10   ★ VSZ 远大于 RSS！
                                                  大量 mmap 映射未实际加载
NetworkManager    546,960       8,908     0.02   ★ VSZ 是 RSS 的 61 倍！
                                                  GLib/D-Bus 映射了大量共享库
aliyun-service    690,460      16,816     0.02   Go 运行时的虚拟内存预留
```

**★ 结论：VSZ 大不等于浪费内存。** 很多框架会 mmap 大量共享库和运行时区域，
但只有被访问的部分才驻留在物理内存（RSS）。

### 验证：/proc/[pid]/status 中的内存数据

```bash
$ grep -E "Vm|ctxt" /proc/393/status
VmPeak:	   65784 kB     ← 虚拟内存峰值
VmSize:	   65784 kB     ← 当前虚拟内存（= pidstat 的 VSZ）
VmRSS:	   39844 kB     ← 常驻内存（≈ pidstat 的 RSS）
VmStk:	     132 kB     ← 栈大小
VmSwap:	       0 kB     ← 交换到 swap 的大小
```

---

## 场景 4：上下文切换 — pidstat -w

### 命令

```bash
pidstat -w
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:00:53 PM   UID       PID   cswch/s nvcswch/s  Command
11:00:53 PM     0         1      0.01      0.00  systemd
11:00:53 PM     0        16      0.14      0.00  ksoftirqd/0
11:00:53 PM     0        17     33.04      0.00  rcu_preempt
11:00:53 PM     0        26      0.91      0.00  kworker/2:0H-kblockd
11:00:53 PM     0        24      0.09      0.00  ksoftirqd/2
```

### 逐字段解读

```
cswch/s → 自愿上下文切换率（voluntary context switch）
          进程主动让出 CPU：
          - 调用 sleep/usleep/nanosleep
          - 等待 I/O 完成（read/write 阻塞）
          - 等待锁/信号量/条件变量
          - 等待子进程退出（waitpid）

nvcswch/s → 非自愿上下文切换率（involuntary context switch）
            进程被调度器强制换下：
            - 时间片用完（SCHED_OTHER 默认 4ms）
            - 高优先级进程抢占（SCHED_FIFO/RR）
            - cgroup CPU 配额限制
```

### 典型模式分析

```
模式 A：cswch/s 高，nvcswch/s ≈ 0
  → I/O 密集型进程（大量时间等待 I/O，主动让出 CPU）
  → 示例：等待网络数据的 nginx worker

模式 B：cswch/s ≈ 0，nvcswch/s 高
  → CPU 密集型进程 + CPU 竞争激烈
  → 进程想用 CPU 但被调度器强制换下
  → 可能需要增加 CPU 或减少并发

模式 C：两者都高
  → 混合型进程（既有 I/O 等待又有 CPU 计算）
  → 示例：数据库（读磁盘 + 计算 + 写日志）

模式 D：两者都低
  → 空闲进程或非常轻量的后台任务
```

### 内核线程的上下文切换

```
rcu_preempt（PID 17）：cswch/s = 33.04
  → ★ RCU 机制需要频繁的上下文切换
  → RCU 回调在 grace period 结束时批量处理
  → 这是正常的内核行为

kworker/2:0H-kblockd（PID 26）：cswch/s = 0.91
  → 块设备工作线程，每次处理完一个 I/O 后 sleep 等待下一个
  → 自愿切换（等 I/O 到来）

ksoftirqd/0（PID 16）：cswch/s = 0.14
  → 软中断处理线程，只在软中断积累到阈值时被唤醒
```

---

## 场景 5：综合画像 — pidstat -u -d -r -w

### 命令

```bash
# dd 读取 2GB 数据，同时 pidstat 采集全部指标
dd if=/dev/vda of=/dev/null bs=1M count=2000 &
DD_PID=$!
pidstat -u -d -r -w -p $DD_PID 1 3
```

### 输出（4 个表格同时输出）

```
11:03:04 PM   UID       PID    %usr %system  %guest   %wait    %CPU   CPU  Command
11:03:04 PM     0    447618    0.00   32.00    0.00    0.00   32.00     3  dd

11:03:04 PM   UID       PID  minflt/s  majflt/s     VSZ     RSS   %MEM  Command
11:03:04 PM     0    447618    310.00      0.00  222980    3140   0.04  dd

11:03:04 PM   UID       PID   kB_rd/s   kB_wr/s kB_ccwr/s iodelay  Command
11:03:04 PM     0    447618 197324.00      0.00      0.00       0  dd

11:03:04 PM   UID       PID   cswch/s nvcswch/s  Command
11:03:04 PM     0    447618    248.00      2.00  dd
```

### 逐指标解读

```
CPU 维度:
  %usr=0.00, %system=32.00, %CPU=32.00
  → ★ dd 读取块设备几乎全是内核态工作（block layer 处理）
  → dd 的用户态代码极其简单（read+write 循环），%usr ≈ 0
  → %system=32% 意味着 dd 占用了约 1.3 个 CPU 核

内存维度:
  minflt/s=310, majflt/s=0, VSZ=222980, RSS=3140
  → 初始阶段有 310 次/秒的次页错误（分配 buffer 页面）
  → majflt/s=0 → 无需从磁盘加载页面
  → RSS 仅 3MB → dd 的内存占用极小

I/O 维度:
  kB_rd/s=197324 (≈193MB/s), kB_wr/s=0
  → ★ 读取速率 193MB/s（接近 virtio-blk 的极限吞吐）
  → kB_wr/s=0 → 输出到 /dev/null，不产生磁盘写入
  → iodelay=0 → virtio-blk 设备太快，I/O 等待可忽略

上下文切换:
  cswch/s=248, nvcswch/s=2
  → 每秒 248 次自愿切换（每次 read() 阻塞等待 I/O 完成）
  → 仅 2 次非自愿切换（CPU 竞争很少）
  → ★ 248 次/秒 × 1MB/次 ≈ 与 kB_rd/s 吻合
```

### Average 汇总行

```
Average:      0    447618    0.00   21.67    0.00    0.00   21.67     -  dd
Average:      0    447618    103.33      0.00  222980    3140   0.04  dd
Average:      0    447618 160000.00      0.00      0.00       0  dd
Average:      0    447618    171.67      4.00  dd

→ Average 是 3 次采样的均值
→ 读取速率逐渐下降（197→164→118 MB/s）→ 数据读取接近完成
→ CPU=21.67% 是均值（对应约 0.87 个核）
```

---

## 场景 6：间隔采样 — 配合 I/O 负载观察

### 命令

```bash
# dd 从 /dev/vda 读取 625MB
dd if=/dev/vda of=/dev/null bs=64k count=10000 &
DD_PID=$!
pidstat -d -p $DD_PID 1 3
```

### 输出

```
11:02:43 PM   UID       PID   kB_rd/s   kB_wr/s kB_ccwr/s iodelay  Command
11:02:43 PM     0    447453 170880.00      0.00      0.00       0  dd
11:02:44 PM     0    447453 167168.00      0.00      0.00       0  dd
11:02:45 PM     0    447453 127744.00      0.00      0.00       0  dd
Average:        0    447453 155264.00      0.00      0.00       0  dd
```

### 逐行解读

```
采样 1：kB_rd/s = 170880 (≈167MB/s) → I/O 满载阶段
采样 2：kB_rd/s = 167168 (≈163MB/s) → 持续满载
采样 3：kB_rd/s = 127744 (≈125MB/s) → 接近完成，速率下降
Average：kB_rd/s = 155264 (≈152MB/s) → 3 次采样的均值

→ /proc/[pid]/io 中的 read_bytes 差值：
  第一次采样：read_bytes 增加了 ~175MB → 170880 KB/s
  第二次采样：read_bytes 增加了 ~171MB → 167168 KB/s
  第三次采样：read_bytes 增加了 ~131MB → 127744 KB/s（读取即将结束）
```

### 验证 /proc/[pid]/io 原始数据

```bash
$ cat /proc/447453/io     # dd 进程的 I/O 计数
rchar: 55252258           ← 读取字符数 ≈ 53MB（已读的）
wchar: 55246848           ← 写入字符数（写到 /dev/null，实际不触发磁盘）
syscr: 852                ← read() 调用次数
syscw: 843                ← write() 调用次数
read_bytes: 50880512      ← ★ 实际磁盘读取 ≈ 48.5MB
write_bytes: 0            ← 写到 /dev/null 不产生磁盘写
cancelled_write_bytes: 0
```

**★ 关键：kB_rd/s 计算过程**

```
T0: read_bytes = X
T1: read_bytes = X + 175,000,000（约）

kB_rd/s = (175,000,000 / 1秒) / 1024 = 170,898 KB/s ≈ 170880
```

### 对比：fio + O_DIRECT 到块设备

```bash
fio --name=test --ioengine=sync --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=8 --time_based --size=1G &
FIO_PID=$!
pidstat -d -p $FIO_PID 2 3
```

```
11:01:20 PM   UID       PID   kB_rd/s   kB_wr/s kB_ccwr/s iodelay  Command
11:01:22 PM     0    436225      0.00      0.00      0.00       0  fio
11:01:24 PM     0    436225      0.00      0.00      0.00       0  fio
11:01:26 PM     0    436225      0.00      0.00      0.00       0  fio
Average:        0    436225      0.00      0.00      0.00       0  fio
```

```
★ 为什么 fio 的 kB_wr/s = 0？

fio 报告：write: IOPS=4665, BW=18.2MiB/s
但 pidstat 显示 kB_wr/s = 0！

原因：fio 使用 O_DIRECT 直接写入裸块设备 /dev/vdb
      → 绕过了文件系统层
      → /proc/[pid]/io 的 write_bytes 计数器不更新
      → pidstat 读到的差值为 0

验证：
$ cat /proc/<FIO_PID>/io
  write_bytes: 0    ← ★ 裸块设备 + O_DIRECT 不更新此计数器

这是 pidstat 的固有限制：
  - 它依赖 /proc/[pid]/io 的 read_bytes/write_bytes
  - 这些计数器由文件系统的 I/O 路径更新
  - 裸块设备 + O_DIRECT 绕过了文件系统 → 计数器为 0

★ 解决方案：对块设备级 I/O 统计，使用 blktrace 而非 pidstat
```

---

## 场景 7：追踪特定进程 — pidstat -p

### 命令

```bash
# 追踪 sshd（PID 979）的 CPU、内存、上下文切换
SSHD_PID=979
pidstat -u -p $SSHD_PID
pidstat -r -p $SSHD_PID
pidstat -w -p $SSHD_PID
```

### 输出

```
--- CPU ---
11:01:25 PM     0       979    0.00    0.00    0.00    0.00    0.00     2  sshd

--- 内存 ---
11:01:25 PM     0       979      0.04      0.00   13736    5932   0.08  sshd

--- 上下文切换 ---
11:01:25 PM     0       979      0.00      0.00  sshd
```

### 解读

```
sshd (PID 979) 画像：
  CPU：       完全空闲（%usr=%system=%CPU=0）
  内存：      VSZ=13.7MB, RSS=5.9MB, 占 0.08% 物理内存
  页错误：    minflt/s=0.04（极少量，累积平均值）
  上下文切换：cswch/s=0, nvcswch/s=0（空闲等待连接）

→ sshd 监听进程处于 sleep 状态，等待新的 SSH 连接
→ RSS 仅 5.9MB，说明 sshd 是非常轻量的守护进程
```

### 用 -C 过滤多个同名进程

```bash
pidstat -C "sshd" -u
```

```
11:01:25 PM     0       979    0.00    0.00    0.00    0.00    0.00     2  sshd
11:01:25 PM     0    433214    0.00    0.00    0.00    0.00    0.00     3  sshd
11:01:25 PM     0    433217    0.00    0.00    0.00    0.00    0.00     2  sshd
11:01:25 PM     0    433288    0.00    0.00    0.00    0.00    0.00     1  sshd
11:01:25 PM     0    433298    0.00    0.00    0.00    0.00    0.00     2  sshd
```

```
5 个 sshd 进程：
  PID 979     → 主 sshd（监听端口 22）
  PID 433214  → root 登录会话 [priv]（特权进程）
  PID 433217  → root 登录会话 @notty（非交互）
  PID 433288  → 另一个 root 登录会话 [priv]
  PID 433298  → 另一个 root 登录会话 @notty
```

---

## 场景 8：线程级、完整路径、优先级等高级选项

### -t 线程模式

```bash
pidstat -t -C sshd -u
```

```
11:02:55 PM   UID      TGID       TID    %usr %system  %guest   %wait    %CPU   CPU  Command
11:02:55 PM     0       979         -    0.00    0.00    0.00    0.00    0.00     2  sshd
11:02:55 PM     0         -       979    0.00    0.00    0.00    0.00    0.00     2  |__sshd
11:02:55 PM     0    433214         -    0.00    0.00    0.00    0.00    0.00     3  sshd
11:02:55 PM     0         -    433214    0.00    0.00    0.00    0.00    0.00     3  |__sshd
```

```
TGID 行（TID = "-"）：进程级汇总
TID 行（TGID = "-"）：线程级明细
|__ 前缀：表示这是某进程的子线程

sshd 是单线程程序 → TGID 行和 TID 行的值完全相同
多线程程序（如 Java）会有多个 TID 行
```

### -l 完整命令行

```bash
pidstat -l -C sshd -u
```

```
11:02:55 PM   UID       PID    %usr %system  %guest   %wait    %CPU   CPU  Command
11:02:55 PM     0       979    0.00    0.00    0.00    0.00    0.00     2  sshd: /usr/sbin/sshd -D [listener] 0 of 10-100 startups
11:02:55 PM     0    433214    0.00    0.00    0.00    0.00    0.00     3  sshd: root [priv]
11:02:55 PM     0    433217    0.00    0.00    0.00    0.00    0.00     3  sshd: root@notty
```

```
-l 显示 /proc/[pid]/cmdline 的完整内容
→ 可以看到 sshd 的启动参数和会话类型
→ 对区分多个同名进程非常有用
```

### -R 实时优先级

```bash
pidstat -R
```

```
11:03:19 PM   UID       PID prio policy  Command
11:03:19 PM     0        18   99   FIFO  migration/0
11:03:19 PM     0        19   50   FIFO  idle_inject/0
11:03:19 PM     0        23   99   FIFO  migration/2
11:03:19 PM     0        63   50   FIFO  watchdogd
11:03:19 PM     0        69    0   IDLE  kidled
```

```
prio=99 FIFO → migration 线程使用最高实时优先级（确保 CPU 迁移立即执行）
prio=50 FIFO → idle_inject/watchdogd 使用中等实时优先级
policy=IDLE  → 最低优先级（仅在系统完全空闲时运行）

★ 大多数用户进程的 policy=OTHER, prio=0（CFS 调度器）
```

### -I 按 CPU 数分割 %CPU

```bash
# 4 CPU 系统上运行 dd
dd if=/dev/vda of=/dev/null bs=1M count=1000 &
pidstat -I -u -p $DD_PID 1 2
```

```
11:03:31 PM   UID       PID    %usr %system  %guest   %wait    %CPU   CPU  Command
11:03:31 PM     0    447843    0.00   28.00    0.00    0.00    7.07     0  dd
11:03:32 PM     0    447843    0.00   21.00    0.00    0.00    5.33     2  dd
Average:        0    447843    0.00   24.50    0.00    0.00    6.20     -  dd
```

```
%system=28.00（原始值，占用约 1.1 个核）
%CPU=7.07（= 28.00 / 4 CPU → 占总 CPU 资源的 7.07%）

★ -I 让 %CPU 表示"占全部 CPU 资源的比例"
   4 核系统上满载 = 100%，8 核系统上满载 = 100%
   不用 -I 时 4 核满载 = 400%（不直观）
```

### -v 内核表信息

```bash
pidstat -v -C sshd
```

```
11:03:53 PM   UID       PID threads   fd-nr  Command
11:03:53 PM     0       979       1       5  sshd
11:03:53 PM     0    433217       1      12  sshd
```

```
threads=1  → 单线程进程
fd-nr=5    → 打开了 5 个文件描述符（监听 socket + 日志 + 配置）
fd-nr=12   → 活跃会话有 12 个 fd（增加了连接 socket 和 pty）
```

---

## 场景 9：pidstat 与 blktrace 的关系

### 分析层次对比

```
┌─────────────────────────────────────────────────────────────┐
│                     应用层                                   │
│  pidstat -d → kB_rd/s = 167MB/s（进程的 I/O 速率）           │
├─────────────────────────────────────────────────────────────┤
│                     文件系统层                                │
│  rchar/wchar → 含 page cache 的读写量                        │
│  read_bytes/write_bytes → 实际到达 block layer 的字节数       │
├─────────────────────────────────────────────────────────────┤
│                     块设备层                                  │
│  blktrace → 每个 I/O request 的 Q/G/I/D/C 事件               │
│  blkparse → rwbs 字段（R/W/WS/FWS）、扇区号、时间戳           │
│  btt → D2C/Q2C 延迟分析                                     │
├─────────────────────────────────────────────────────────────┤
│                     设备驱动层                                │
│  virtio-blk → 实际的 virtqueue 交互                           │
└─────────────────────────────────────────────────────────────┘
```

### 联合使用示例

```bash
# 步骤 1：用 pidstat 找出"哪个进程在产生 I/O"
pidstat -d 2 5
# 发现 dd (PID 447453) 的 kB_rd/s = 167MB/s

# 步骤 2：用 blktrace 看"块设备层面的 I/O 详情"
blktrace -d /dev/vda -o trace -w 10
blkparse -i trace -o parsed.txt

# 步骤 3：用 blkparse 找到 dd 产生的 I/O 事件
grep "447453\|dd" parsed.txt | head -5
# 253,0  0  1  0.000000000  447453  Q  RA 0 + 128 [dd]
#   → dd 的 I/O 是 RA（Read + Readahead）
#   → 每次 128 扇区 = 64KB（与 bs=64k 一致）

# 步骤 4：用 btt 分析延迟
blkparse -i trace -d trace.bin
btt -i trace.bin -o btt_out
# D2C avg = 设备延迟
# Q2C avg = 端到端延迟
```

### pidstat 能看到但 blktrace 看不到的

```
pidstat 独特视角:
  - 进程的 CPU 使用率（%usr/%system/%wait）
  - 进程的内存用量（VSZ/RSS/%MEM）
  - 进程的上下文切换（cswch/s/nvcswch/s）
  - 进程的页错误率（minflt/s/majflt/s）
  - 多个进程之间的资源竞争对比

blktrace 独特视角:
  - 每个 I/O request 的精确时间戳（纳秒级）
  - I/O 在 block layer 的完整生命周期（Q→G→I→D→C）
  - I/O 调度器的行为（Plug/Unplug/Merge）
  - 设备级延迟分布（D2C 直方图）
  - FUA/Flush 请求的检测
```

---

## 快速诊断流程

```
1. 系统慢了 → 先看哪个进程占资源
   pidstat -u 2 3 | sort -k7 -rn | head -10
   → 按 %CPU 排序找 CPU 消耗者

2. 磁盘繁忙 → 找 I/O 进程
   pidstat -d 2 3 | sort -k5 -rn | head -10
   → 按 kB_wr/s 排序找写入者

3. 内存增长 → 检查泄漏
   pidstat -r -p <PID> 5 60
   → 观察 RSS 是否持续增长

4. 进程卡顿 → 看上下文切换和 CPU 等待
   pidstat -w -p <PID> 2 5
   pidstat -u -p <PID> 2 5
   → %wait 高 = CPU 排队
   → cswch/s 高 = I/O 阻塞
   → nvcswch/s 高 = CPU 被抢占

5. I/O 进程找到后 → 深入块设备层
   blktrace -d /dev/sda -o trace -p <PID> -w 10
   → 追踪该进程的 I/O 在 block layer 的行为

6. 综合分析 → 一次看全
   pidstat -u -d -r -w 2 5
   → CPU + I/O + 内存 + 上下文切换一次输出
```
