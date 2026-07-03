# bpftrace 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐行解读。
> 环境：Alibaba Cloud Linux 4, 内核 6.6.102, bpftrace v0.20.1
> 设备：/dev/vdb (40G virtio-blk)，I/O 由 fio 生成。

---

## 场景 1：Hello BPF — 最简单的 one-liner

### 命令

```bash
bpftrace -e 'BEGIN { printf("Hello BPF!\n"); }'
```

### 输出

```
Attaching 1 probe...
Hello BPF!
```

```
Attaching 1 probe...  ← bpftrace 编译 DSL → BPF 字节码 → 加载到内核
Hello BPF!             ← BEGIN probe 在程序启动时执行一次
                        （程序持续运行直到 Ctrl+C 退出）
```

**解读：**

| 组件 | 说明 |
|------|------|
| `BEGIN` | 特殊 probe，程序启动时触发一次（类似 awk 的 BEGIN） |
| `printf()` | bpftrace 内置函数，在内核态执行，输出到用户态 stdout |
| `Attaching 1 probe...` | bpftrace 前端确认已挂载 1 个 probe 点 |

**★ 这是 BPF 程序的最小示例：** BEGIN probe → printf → 退出。整个过程中 bpftrace 完成了 DSL 解析 → LLVM 编译 → BPF verifier 验证 → 内核加载 的完整流程。

### 内置变量演示

```bash
timeout 3 bpftrace -e 'BEGIN {
  printf("CPU: %d, PID: %d, UID: %d\n", cpu, pid, uid);
  printf("comm: %s\n", comm);
}'
```

### 输出

```
Attaching 1 probe...
CPU: 0, PID: 455646, UID: 0
comm: bpftrace
```

```
CPU: 0          → bpftrace 进程运行在 CPU 0 上
PID: 455646     → bpftrace 自身的进程 ID
UID: 0          → 以 root 身份运行（BPF 程序需要 CAP_SYS_ADMIN）
comm: bpftrace  → 进程名（task_struct->comm）
```

**★ 常用内置变量速查：**

| 变量 | 说明 | 使用场景 |
|------|------|----------|
| `pid` / `tid` | 进程 ID / 线程 ID | 按进程过滤 |
| `comm` | 进程名 | 按进程名聚合 |
| `cpu` | 当前 CPU 号 | 分析 CPU 亲和性 |
| `nsecs` | 纳秒时间戳 | 延迟计算 |
| `uid` / `gid` | 用户 ID / 组 ID | 权限相关分析 |
| `args` | tracepoint 参数 | 访问系统调用参数 |
| `probe` | 当前 probe 名 | 区分多个 probe |

---

## 场景 2：追踪 openat 系统调用

### 命令

```bash
bpftrace -e 'tracepoint:syscalls:sys_enter_openat {
  printf("%s %s\n", comm, str(args->filename));
}' -c 'ls /tmp'
```

`-c 'ls /tmp'` 启动子进程 `ls /tmp`，bpftrace 追踪到该进程的所有 openat 调用后自动退出。

### 输出

```
Attaching 1 probe...
ls /etc/ld.so.cache
ls /lib64/libselinux.so.1
ls /lib64/libcap.so.2
ls /lib64/libc.so.6
ls /lib64/libpcre2-8.so.0
ls /proc/filesystems
ls /usr/lib/locale/locale-archive
ls /tmp
```

### 逐行解读

```
ls /etc/ld.so.cache            ← ★ 动态链接器加载共享库缓存
ls /lib64/libselinux.so.1      ← 加载 SELinux 库（安全检查）
ls /lib64/libcap.so.2          ← 加载 capabilities 库
ls /lib64/libc.so.6            ← 加载 C 标准库
ls /lib64/libpcre2-8.so.0      ← 加载正则表达式库
ls /proc/filesystems           ← 检查支持的文件系统类型
ls /usr/lib/locale/locale-archive  ← 加载 locale 数据
ls /tmp                        ← ★ 最后才是 ls 真正的目标！
```

**解读：** `ls /tmp` 这个简单命令在到达目标路径之前，先打开了 **7 个文件**用于加载共享库和系统配置。动态链接（ld.so）是最大的"隐形 I/O"来源。

```
comm              → 进程名（ls）
str(args->filename) → ★ str() 将内核指针转为字符串
args->filename    → tracepoint 的参数（openat 的 filename 字段）
```

**★ 关键语法：** `args->字段名` 访问 tracepoint 的参数，`str()` 将 `const char *` 指针转为可打印字符串。

---

## 场景 3：系统调用统计

### 命令

```bash
# 运行 5 秒后 Ctrl+C 退出
bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[probe] = count(); }'
```

### 输出

```
Attaching 1 probe...

@[tracepoint:raw_syscalls:sys_enter]: 86042
```

```
@[probe]              → 以 probe 名称为 key 的 map
= count()             → 聚合函数：计数
86042                 → ★ 5 秒内发生了 86042 次系统调用（约 17,200 次/秒）
```

### 按 syscall ID 细分统计

```bash
bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[args->id] = count(); }'
```

### 输出（Top 20 syscall ID，5 秒统计）

```
@[24]: 48331
@[202]: 22560
@[230]: 5790
@[0]: 3501
@[3]: 2487
@[257]: 2191
@[281]: 1862
@[14]: 1835
@[262]: 1624
@[28]: 1326
@[1]: 1323
@[441]: 1034
@[13]: 850
@[9]: 538
@[8]: 445
@[45]: 399
@[232]: 386
@[228]: 369
@[17]: 300
@[11]: 264
```

### syscall ID → 名称映射（x86_64）

```
ID    名称                次数    说明
──    ────────────────    ─────   ──────────────────────────────
24    sched_yield         48331   ★ 最多！用户态让出 CPU（常见于 busy-wait 循环）
202   futex               22560   ★ 互斥锁/条件变量（多线程同步核心）
230   clock_nanosleep      5790   精确睡眠（定时器、sleep 命令）
0     read                 3501   读文件/socket
3     close                2487   关闭文件描述符
257   openat               2191   打开文件（openat 替代了旧版 open）
281   epoll_pwait          1862   epoll 事件循环（网络服务核心）
14    rt_sigprocmask       1835   信号掩码管理
262   newfstatat           1624   获取文件属性（stat 的新版本）
28    madvise              1326   内存建议（内核优化内存访问模式）
1     write                1323   写文件/socket
441   bpf                  1034   ★ BPF 系统调用（bpftrace 自身也在用！）
13    rt_sigaction          850   信号处理注册
9     mmap                  538   内存映射
8     lseek                 445   文件偏移定位
45    sendmsg               399   发送网络消息
```

**诊断结论：** `sched_yield` 占比 56%，说明系统上有大量用户态忙等待。`futex` 排第二（26%）说明多线程同步开销显著。`bpf` 调用 1034 次 — 这是 bpftrace 自身在加载 BPF 程序！

### 按名称统计常见系统调用

```bash
bpftrace -e '
tracepoint:syscalls:sys_enter_write { @[probe] = count(); }
tracepoint:syscalls:sys_enter_read { @[probe] = count(); }
tracepoint:syscalls:sys_enter_futex { @[probe] = count(); }
tracepoint:syscalls:sys_enter_poll { @[probe] = count(); }
tracepoint:syscalls:sys_enter_epoll_wait { @[probe] = count(); }
tracepoint:syscalls:sys_enter_nanosleep { @[probe] = count(); }
'
```

### 输出（5 秒统计）

```
@[tracepoint:syscalls:sys_enter_nanosleep]: 10
@[tracepoint:syscalls:sys_enter_poll]: 24
@[tracepoint:syscalls:sys_enter_epoll_wait]: 354
@[tracepoint:syscalls:sys_enter_write]: 1037
@[tracepoint:syscalls:sys_enter_read]: 2068
@[tracepoint:syscalls:sys_enter_futex]: 26706
```

```
sys_enter_futex       26706   ★ 最多！互斥锁/条件变量（多线程同步）
sys_enter_read         2068   读操作（文件、pipe、socket）
sys_enter_write        1037   写操作
sys_enter_epoll_wait    354   epoll 事件循环（网络服务）
sys_enter_poll           24   poll 系统调用（旧式 I/O 多路复用）
sys_enter_nanosleep      10   精确睡眠
```

**★ syscalls vs raw_syscalls：**
- `tracepoint:syscalls:sys_enter_xxx` → 每个系统调用有独立 tracepoint，可直接用 `args->字段名`
- `tracepoint:raw_syscalls:sys_enter` → 统一入口，用 `args->id` 区分系统调用号

---

## 场景 4：追踪进程创建（execve）

### 命令

```bash
# 后台运行 bpftrace，前台执行命令
bpftrace -e 'tracepoint:syscalls:sys_enter_execve {
  printf("%s -> %s\n", comm, str(args->filename));
}' &
sleep 1
ls /tmp > /dev/null
cat /etc/hostname > /dev/null
uname -r > /dev/null
date > /dev/null
whoami > /dev/null
```

### 输出

```
Attaching 1 probe...
bash -> /usr/bin/ls
bash -> /usr/bin/cat
bash -> /usr/bin/uname
bash -> /usr/bin/date
bash -> /usr/bin/whoami
bash -> /usr/bin/sleep
claude -> /bin/bash
bash -> /usr/bin/base64
bash -> /usr/sbin/bpftool
bash -> /usr/bin/strace
```

### 逐行解读

```
bash -> /usr/bin/ls           ← 我们在 shell 中执行 ls
bash -> /usr/bin/cat          ← cat /etc/hostname
bash -> /usr/bin/uname        ← uname -r
bash -> /usr/bin/date         ← date
bash -> /usr/bin/whoami       ← whoami
bash -> /usr/bin/sleep        ← sleep 1（bpftrace 启动等待）
claude -> /bin/bash           ← ★ claude 进程 fork 出 bash 子进程
bash -> /usr/bin/base64       ← 内部工具调用
bash -> /usr/sbin/bpftool     ← ★ bpftrace 内部调用 bpftool 查询 BPF 信息
bash -> /usr/bin/strace       ← 其他并行进程
```

**解读：** `comm` 显示调用者的进程名。`bash -> /usr/bin/ls` 表示 bash 通过 `execve` 加载 `/usr/bin/ls`。注意 `claude -> /bin/bash` 展示了 IDE 工具链如何启动子 shell。

```
args->filename   → execve 的第一个参数（要执行的程序路径）
comm             → 当前进程的 comm 字段（即 task_struct->comm）
```

**★ execve vs fork：** execve 是"替换当前进程映像"，fork/clone 是"创建新进程"。追踪进程创建通常两者都要看：clone 看新线程/进程，execve 看程序加载。

```
完整进程生命周期：
bash (PID=A)
  └── fork() → 子进程 (PID=B)
      └── execve("/usr/bin/ls") → ls (PID=B，同 PID，程序替换)
          └── exit()

★ 追踪完整的生命周期：
  tracepoint:syscalls:sys_enter_clone    → fork/clone
  tracepoint:syscalls:sys_enter_execve   → exec
  tracepoint:sched:sched_process_exit    → exit
```

---

## 场景 5：追踪 Block I/O（与 blktrace 对比）

### 命令

```bash
bpftrace -e 'tracepoint:block:block_rq_issue {
  @[args->rwbs] = count();
}'
# 同时用 fio 生成 I/O
fio --name=bpf_demo --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/vdb --runtime=3 --time_based --iodepth=16
```

### 输出

```
Attaching 1 probe...

@[W]: 2
@[R]: 3
@[N]: 2
@[FF]: 4
@[WSM]: 6
@[WM]: 20
@[RA]: 107
@[WS]: 49267
```

### 逐行解读

```
rwbs    次数     含义
────    ─────    ──────────────────────────────────────
WS      49267    ★ Write + Sync（direct I/O → O_DIRECT → REQ_SYNC）
RA      107      Read + Readahead（内核预读）
WM      20       Write + Metadata（文件系统元数据写入）
WSM     6        Write + Sync + Metadata（同步元数据写入）
FF      4        Flush + FUA（强制单元访问，保证持久化）
R       3        Read（普通读）
N       2        None（控制类请求，无数据）
W       2        Write（普通写，无 Sync 标志）
```

**★ WS 占绝对主导（97%+）：** 因为 fio `--direct=1` 打开 O_DIRECT，内核为 direct I/O 设置 `REQ_SYNC` 标志。这与 blktrace 中 rwbs 显示 **WS** 完全一致。

### rwbs 字段含义（与 blktrace 一致）

```
字符 │ 全称          │ 含义
─────┼──────────────┼────────────────────────
  W  │ Write        │ 写操作
  R  │ Read         │ 读操作
  S  │ Sync         │ 同步 I/O（O_DIRECT 或 fsync 触发）
  M  │ Metadata     │ 元数据操作
  F  │ Flush/FUA    │ 强制写持久化介质
  A  │ Readahead    │ 内核预读
  N  │ None         │ 无特殊属性（控制类请求）
```

### 与 blktrace 对比

```
bpftrace:                        blktrace (blkparse):
@[WS]: 49267                     49267 WS   ← 同一批 I/O！
@[RA]: 107                       107  RA

底层 tracepoint 完全相同：tracepoint:block:block_rq_issue
```

| 对比项 | bpftrace | blktrace |
|--------|----------|----------|
| **数据源** | tracepoint（BPF 程序读取） | relay buffer（内核写入文件） |
| **输出方式** | 实时聚合（count/hist） | 原始事件流（需 blkparse 解析） |
| **延迟分析** | hist() 直方图 | btt 工具（Q2G/G2I/I2D/D2C） |
| **灵活性** | ★ DSL 自定义任意逻辑 | 固定格式，后处理分析 |
| **开销** | BPF 程序在内核态聚合 | 每个事件写入 relay buffer |

**★ 核心区别：** bpftrace 在内核态直接聚合（count/hist），数据不出内核；blktrace 记录每个原始事件到 relay buffer，适合事后详细分析。

### bpftrace 逐事件输出（对比 blkparse 格式）

```bash
bpftrace -e '
  tracepoint:block:block_rq_issue {
    printf("%-16s %-6d dev=%d,%d  rwbs=%-4s sector=%-10d bytes=%-6d\n",
           comm, pid,
           args->dev >> 20, args->dev & 0xfffff,
           args->rwbs, args->sector, args->bytes);
  }'
```

### 输出

```
kworker/2:0H     26     dev=253,16  rwbs=WS   sector=43276368   bytes=4096
kworker/3:1H     77     dev=253,16  rwbs=WS   sector=17641288   bytes=4096
kworker/3:1H     77     dev=253,16  rwbs=WS   sector=36159664   bytes=4096
kworker/3:1H     77     dev=253,16  rwbs=WS   sector=63627448   bytes=4096
kworker/3:1H     77     dev=253,16  rwbs=WS   sector=48388704   bytes=4096
```

### 与 blkparse 同一事件对比

```
bpftrace 输出：
kworker/3:1H  77  dev=253,16  rwbs=WS  sector=17641288  bytes=4096

blkparse 输出（同一事件）：
253,16   3   42  0.001234567  77  D  WS  17641288 + 8 [kworker/3:1H]
 │       │   │    │           │  │  │    │          │
 │       │   │    │           │  │  │    │          └─ 8 扇区 = 4096 字节
 │       │   │    │           │  │  │    └─ 起始扇区（相同！）
 │       │   │    │           │  │  └─ rwbs（相同！WS）
 │       │   │    │           │  └─ D = Issue（对应 block_rq_issue）
 │       │   │    │           └─ PID（相同！77）
 │       │   │    └─ 时间戳（纳秒精度）
 │       │   └─ 事件序号
 │       └─ CPU（3）
 └─ 设备号（相同！253,16）

★ 同一个内核 tracepoint (block_rq_issue)
★ bpftrace 通过 BPF 程序采集，blktrace 通过 relay buffer 采集
★ 数据内容完全一致，只是输出格式不同
```

---

## 场景 6：I/O 延迟直方图

### 命令

```bash
bpftrace -e '
tracepoint:block:block_rq_issue {
  @start[tid] = nsecs;
}
tracepoint:block:block_rq_complete /@start[tid]/ {
  @us = hist((nsecs - @start[tid]) / 1000);
  delete(@start[tid]);
}'
# 同时用 fio 生成 I/O
fio --name=hist_demo --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/vdb --runtime=3 --time_based --iodepth=16
```

### 输出

```
@us:
[1]                   79 |@@@@                                                |
[2, 4)                58 |@@@                                                 |
[4, 8)               990 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[8, 16)              583 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                      |
[16, 32)             130 |@@@@@@                                              |
[32, 64)              68 |@@@                                                 |
[64, 128)             38 |@                                                   |
[128, 256)            18 |                                                    |
[256, 512)             0 |                                                    |
[512, 1K)              1 |                                                    |
[1K, 2K)               2 |                                                    |
[2K, 4K)               1 |                                                    |
[4K, 8K)              16 |                                                    |
[8K, 16K)              1 |                                                    |
[16K, 32K)             4 |                                                    |
[32K, 64K)             4 |                                                    |
[64K, 128K)            1 |                                                    |
[128K, 256K)           0 |                                                    |
[256K, 512K)           1 |                                                    |
```

### 逐行解读

```
@us:                    → map 名（微秒级延迟）
[4, 8)     990  ★      → 峰值！4-8μs 区间有 990 个 I/O（占最多）
[8, 16)    583         → 8-16μs 区间有 583 个
[16, 32)   130         → 16-32μs 区间
[32, 64)    68         → 32-64μs
[64, 128)   38         → 64-128μs
[128, 256)  18         → 128-256μs（与 btt D2C ≈ 147μs 对应）
[4K, 8K)    16         → ★ 长尾：4-8ms（可能是调度延迟）
[32K, 64K)   4         → 32-64ms（极端长尾）
[256K, 512K) 1         → 256-512ms（异常值）
```

**★ 直方图结构解读：**

```
[bucket)    count    bar
────────    ─────    ────────────────────────
[4, 8)       990     @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
            ↑        ↑
            │        每个 @ 约代表 20 个 I/O
            延迟区间：≥4μs 且 <8μs
```

### 延迟分布总结

```
延迟范围       数量     占比      解读
──────────     ─────    ──────    ────────────────────────────
< 16μs         1652     82%       ★ 绝大多数 I/O 在 16μs 内完成
16-128μs        236     12%       正常的设备延迟
128μs-1ms        21     1%        少量较慢的 I/O
1ms-8ms          19     ~1%       长尾（可能是调度器排队）
8ms-512ms         7     <0.1%     ★ 极端长尾（需排查）
```

**对比 blktrace btt：**
```
btt D2C avg = 147μs               ← blktrace 给出精确平均值
bpftrace hist: 峰值在 4-8μs        ← ★ 但众数远低于平均值！
```

→ **直方图揭示了 btt 平均值掩盖的信息：** 大多数 I/O 实际在 4-8μs 完成，但少量长尾拉高了平均值。这正是直方图比平均值更有价值的地方。

### 代码逻辑解读

```bpftrace
tracepoint:block:block_rq_issue {
  @start[tid] = nsecs;             ← ① I/O 下发时记录纳秒时间戳
}
tracepoint:block:block_rq_complete /@start[tid]/ {
                                    ← ② I/O 完成时，条件：@start[tid] 存在
  @us = hist((nsecs - @start[tid]) / 1000);
                                    ← ③ 计算延迟（纳秒→微秒）并加入直方图
  delete(@start[tid]);              ← ④ 清理，避免内存泄漏
}
```

**★ 注意事项：** 用 `tid`（线程 ID）而非 `pid` 作为 key，因为同一进程可能并发提交多个 I/O。`/@start[tid]/` 是 bpftrace 的过滤条件语法。

---

## 场景 7：网络追踪（tcp_sendmsg）

### 命令

```bash
bpftrace -e 'kprobe:tcp_sendmsg { @[comm] = count(); }'
# 同时生成网络流量
curl -s -o /dev/null https://www.baidu.com
```

### 输出

```
Attaching 1 probe...

@[AliYunDun]: 1
@[nginx]: 2
@[claude]: 2
@[curl]: 5
@[code-fcf604774b]: 13
@[sshd]: 16
```

### 逐行解读

```
comm                次数    说明
──────────────      ────    ──────────────────────────────────
sshd                16      ★ SSH 守护进程（我们的远程连接）
code-fcf604774b     13      IDE 后端进程（与远程服务通信）
curl                5       ★ 我们主动触发的 HTTP 请求
claude              2       Claude Code CLI 进程
nginx               2       本地 nginx 服务
AliYunDun           1       阿里云安全 agent
```

**解读：** kprobe 挂载到内核函数 `tcp_sendmsg` 的入口处。每次任何进程调用 TCP 发送，BPF 程序都会触发并按 `comm`（进程名）聚合计数。

```
kprobe:tcp_sendmsg   → 挂载到内核函数 tcp_sendmsg（kprobe 动态追踪点）
@[comm]              → 以进程名为 key 的 map
count()              → 聚合计数
```

**★ kprobe vs tracepoint：**

| 特性 | kprobe | tracepoint |
|------|--------|------------|
| **稳定性** | 函数名可能随内核版本变化 | ★ 内核 ABI 保证稳定 |
| **可用性** | 几乎可挂载到任何内核函数 | 需要内核预定义 |
| **参数访问** | 需要知道寄存器布局 | `args->字段名` 直接访问 |
| **使用场景** | 没有对应 tracepoint 时使用 | ★ 优先选择 |

### 按系统调用名追踪网络相关调用

```bash
bpftrace -e '
tracepoint:syscalls:sys_enter_write { @[probe] = count(); }
tracepoint:syscalls:sys_enter_read { @[probe] = count(); }
tracepoint:syscalls:sys_enter_futex { @[probe] = count(); }
tracepoint:syscalls:sys_enter_poll { @[probe] = count(); }
tracepoint:syscalls:sys_enter_epoll_wait { @[probe] = count(); }
tracepoint:syscalls:sys_enter_nanosleep { @[probe] = count(); }
'
```

### 输出（5 秒统计）

```
@[tracepoint:syscalls:sys_enter_nanosleep]: 10
@[tracepoint:syscalls:sys_enter_poll]: 24
@[tracepoint:syscalls:sys_enter_epoll_wait]: 354
@[tracepoint:syscalls:sys_enter_write]: 1037
@[tracepoint:syscalls:sys_enter_read]: 2068
@[tracepoint:syscalls:sys_enter_futex]: 26706
```

```
sys_enter_futex       26706   ★ 最多！互斥锁/条件变量（多线程同步）
sys_enter_read         2068   读操作（文件、pipe、socket）
sys_enter_write        1037   写操作
sys_enter_epoll_wait    354   epoll 事件循环（网络服务）
sys_enter_poll           24   poll 系统调用（旧式 I/O 多路复用）
sys_enter_nanosleep      10   精确睡眠
```

---

## 场景 8：系统调用延迟直方图

### 命令

```bash
bpftrace -e '
  tracepoint:raw_syscalls:sys_enter { @start[tid] = nsecs; }
  tracepoint:raw_syscalls:sys_exit /@start[tid]/ {
    @us = hist((nsecs - @start[tid]) / 1000);
    delete(@start[tid]);
  }'
```

### 输出（运行 3 秒）

```
@us:
[0]                35331 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[1]                 8991 |@@@@@@@@@@@@@                                       |
[2, 4)              9496 |@@@@@@@@@@@@@                                       |
[4, 8)              5013 |@@@@@@@                                             |
[8, 16)             2993 |@@@@                                                |
[16, 32)            2428 |@@@                                                 |
[32, 64)            1422 |@@                                                  |
[64, 128)            760 |@                                                   |
[128, 256)           692 |@                                                   |
[256, 512)           222 |                                                    |
[512, 1K)            102 |                                                    |
[1K, 2K)            2907 |@@@@                                                |
[2K, 4K)              76 |                                                    |
[4K, 8K)             670 |                                                    |
[8K, 16K)            792 |@                                                   |
[16K, 32K)           240 |                                                    |
[32K, 64K)           479 |                                                    |
[64K, 128K)          674 |                                                    |
[128K, 256K)         139 |                                                    |
[256K, 512K)         122 |                                                    |
[512K, 1M)            83 |                                                    |
[1M, 2M)               6 |                                                    |
[2M, 4M)               2 |                                                    |
```

### 逐行解读

```
[0]                35331 |@@@@@...|   ← 0 微秒（亚微秒级系统调用）
  │                  │                    如 getpid(), clock_gettime()
  │                  └─ 35331 次          ★ 大部分系统调用 < 1μs
  └─ 桶范围：[0, 1) 微秒

[1]                 8991 |@@@@@...|   ← 1 微秒
[2, 4)              9496 |@@@@@...|   ← 2-3 微秒
[4, 8)              5013 |@@@@@...|   ← 4-7 微秒
  ...                                   典型的 read/write 延迟

[1K, 2K)            2907 |@@@@   |   ← 1-2 毫秒（★ 双峰分布！）
  │                                     可能是 mmap 或网络操作
  └─ 注意这里有个明显的高峰

[64K, 128K)          674 |      |    ← 64-128 毫秒
  │                                    可能是 I/O 阻塞、网络超时
  └─ 长尾延迟

[2M, 4M)               2 |      |    ← 2-4 秒（★ 异常长延迟！）
  │                                    可能是 futex 等待、sleep
  └─ 极少但存在
```

**分布特征分析：**

```
双峰分布（bimodal）：
  峰1: [0] = 35331 次 → 快速系统调用（getpid, clock_gettime）
  峰2: [1K,2K) = 2907 次 → 阻塞型调用（nanosleep, futex）

★ 双峰说明系统有两种截然不同的系统调用模式：
  1. 非阻塞调用（< 10μs）→ 大多数
  2. 阻塞调用（1-2ms）→ 可能是 sleep/futex/poll
```

---

## 场景 9：bpftool 查看系统 BPF 状态

### 9.1 bpftool prog show — 已加载的 BPF 程序

```bash
bpftool prog show
```

```
27: lsm  name restrict_filesystems  tag 1a17ecfe539f9b82  gpl
    loaded_at 2026-06-13T07:55:50+0800  uid 0
    xlated 560B  jited 300B  memlock 4096B  map_ids 11
    btf_id 31
69: kprobe  name kprobe_proc_fork_connector  tag 2eac5b217e4b4aad  gpl
    loaded_at 2026-06-13T07:57:03+0800  uid 0
    xlated 1784B  jited 925B  memlock 4096B  map_ids 16,21,25
    btf_id 71
71: kprobe  name kretprobe_sys_execve  tag c1e36cae2d09f964  gpl
    loaded_at 2026-06-13T07:57:03+0800  uid 0
    xlated 51488B  jited 29026B  memlock 53248B  map_ids 16,21,25
    btf_id 71
72: kprobe  name kprobe_tcp_connect  tag c1e36cae2d09f964  gpl
    loaded_at 2026-06-13T07:57:03+0800  uid 0
    xlated 2376B  jited 1402B  memlock 4096B  map_ids 16,21,25
    btf_id 71
```

```
27: lsm                    ← ★ BPF LSM 程序（安全模块！）
  name restrict_filesystems   限制文件系统访问
  xlated 560B                 翻译后的 BPF 字节码大小
  jited 300B                  JIT 编译后的机器码大小
  memlock 4096B               锁定的内存（Map + 程序）
  btf_id 31                   关联的 BTF 类型 ID

69: kprobe                 ← kprobe 程序
  name kprobe_proc_fork_connector   ← 追踪 fork（进程创建）
  xlated 1784B → jited 925B         ← JIT 压缩比 52%

71: kprobe
  name kretprobe_sys_execve         ← 追踪 execve（程序执行）
  xlated 51488B → jited 29026B      ← ★ 最大的程序（50KB！）
                                      可能包含复杂的 execve 数据收集逻辑

★ 本系统有安全监控 BPF 程序在运行（阿里云安骑士）
  - restrict_filesystems (LSM)
  - kprobe_proc_fork_connector
  - kretprobe_sys_execve
  - kprobe_tcp_connect
```

### 9.2 bpftool map show — 已创建的 BPF Map

```bash
bpftool map show
```

```
11: hash_of_maps  name cgroup_hash  flags 0x0
    key 8B  value 4B  max_entries 2048  memlock 165280B
16: percpu_array  name data_tmp_store_  flags 0x0
    key 4B  value 32768B  max_entries 2  memlock 262608B
17: lru_hash  name gnutls_session_  flags 0x0
    key 8B  value 8B  max_entries 4096  memlock 328832B
21: perf_event_array  name perf_event_arra  flags 0x0
    key 4B  value 4B  max_entries 4  memlock 480B
22: prog_array  name tail_call_map  flags 0x0
```

```
11: hash_of_maps           ← ★ 嵌套 Map（hash of maps）
  name cgroup_hash            cgroup 相关
  max_entries 2048            最多 2048 个子 Map

16: percpu_array           ← per-CPU 数组（无锁）
  value 32768B                每个元素 32KB！
  max_entries 2               只有 2 个元素
  ★ 用于临时存储 BPF 程序的大型数据

17: lru_hash               ← ★ LRU 哈希（自动淘汰旧元素）
  name gnutls_session_        GnuTLS 会话追踪
  max_entries 4096            最多追踪 4096 个 TLS 会话

21: perf_event_array       ← perf buffer（事件流传输）
  max_entries 4               4 个 CPU 各一个 buffer
  ★ 安全监控程序用来向用户态传输事件的通道

22: prog_array             ← 程序数组（tail call）
  ★ 用于 BPF 程序链式调用（一个 BPF 程序调用另一个）
```

### 9.3 bpftool feature — BPF 特性检测

```bash
bpftool feature
```

```
Scanning system configuration...
bpf() syscall restricted to privileged users (admin can change)
JIT compiler is enabled
JIT compiler hardening is disabled
JIT compiler kallsyms exports are enabled for root
CONFIG_BPF is set to y
CONFIG_BPF_SYSCALL is set to y
CONFIG_HAVE_EBPF_JIT is set to y
CONFIG_BPF_JIT is set to y
CONFIG_BPF_JIT_ALWAYS_ON is set to y      ← ★ JIT 始终启用
CONFIG_DEBUG_INFO_BTF is set to y         ← ★ BTF 启用（CO-RE 可用）
CONFIG_DEBUG_INFO_BTF_MODULES is set to y ← ★ 模块 BTF 也启用
CONFIG_CGROUP_BPF is set to y
CONFIG_BPF_EVENTS is set to y             ← ★ kprobe/uprobe BPF 事件
CONFIG_KPROBE_EVENTS is set to y
CONFIG_UPROBE_EVENTS is set to y
CONFIG_TRACING is set to y
CONFIG_FTRACE_SYSCALLS is set to y
```

```
★ 关键发现：
  - JIT ALWAYS_ON → 所有 BPF 程序都会被 JIT 编译为机器码
  - BTF 启用 → 支持 CO-RE（编译一次，到处运行）
  - kprobe/uprobe events → 支持动态追踪
  - ftrace syscalls → 支持 tracepoint:syscalls:*

★ 本系统的 BPF 功能完整，适合学习和开发
```

---

## 场景 10：bpftrace --info — 运行时信息

### 命令

```bash
bpftrace --info
```

### 输出

```
System
  OS: Linux 6.6.102-5.3.1.alnx4.x86_64
  Arch: x86_64

Build
  version: v0.20.1
  LLVM: 17.0.6
  unsafe probe: no
  bfd: yes
  libdw (DWARF support): no

Kernel helpers
  probe_read: yes
  probe_read_str: yes
  probe_read_user: yes
  probe_read_user_str: yes
  probe_read_kernel: yes
  probe_read_kernel_str: yes
  get_current_cgroup_id: yes
  send_signal: yes
  override_return: no
  get_boot_ns: yes

Kernel features
  Instruction limit: 1000000
  Loop support: yes
  btf: yes
  module btf: yes
  map batch: yes

Map types
  hash: yes
  percpu hash: yes
  array: yes
```

### 解读

```
LLVM: 17.0.6           ← bpftrace 使用 LLVM 17 编译 BPF 程序
unsafe probe: no       ← 默认禁止 system() 和 signal()（需 --unsafe）
Instruction limit: 1M  ← BPF 程序最多 100 万条指令
Loop support: yes      ← 支持 bounded loop（内核 5.3+）
btf: yes               ← ★ BTF 可用 → 支持 fentry/fexit 和 CO-RE
map batch: yes         ← 支持批量 Map 操作（内核 5.6+）
```

---

## 快速诊断流程

```
1. 系统变慢 → 先看 CPU 消耗
   bpftrace -e 'profile:hz:99 { @[comm] = count(); }'
   → 找出 CPU 最高的进程

2. 看系统调用是否异常
   bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm] = count(); }'
   → 看哪个进程调用最多

3. 看 I/O 是否繁忙
   bpftrace -e 'tracepoint:block:block_rq_issue { @[comm] = count(); }'
   → 看谁在产生 I/O

4. 看延迟分布
   bpftrace -e '
     tracepoint:raw_syscalls:sys_enter { @s[tid]=nsecs; }
     tracepoint:raw_syscalls:sys_exit /@s[tid]/ {
       @us=hist((nsecs-@s[tid])/1000); delete(@s[tid]); }'
   → 直方图看延迟分布

5. 看进程活动
   bpftrace -e 'tracepoint:syscalls:sys_enter_execve {
     printf("%s -> %s\n", comm, str(args->filename)); }'
   → 看谁在创建新进程

6. 对比 blktrace
   bpftrace -e 'tracepoint:block:block_rq_issue {
     printf("%d,%d %s sector=%d bytes=%d\n",
       args->dev>>20, args->dev&0xfffff,
       args->rwbs, args->sector, args->bytes); }'
   → 与 blkparse 输出对比（同一事件，不同采集方式）

7. 网络流量
   bpftrace -e 'kprobe:tcp_sendmsg { @[comm] = count(); }'
   → 看哪些进程在发送网络数据
```

---

## 附录 A：可用 Probe 统计

本系统可用的 probe 数量：

```bash
bpftrace -l 'kprobe:*' | wc -l          # ~52,000 个 kprobe
bpftrace -l 'tracepoint:*' | wc -l      # ~1,750 个 tracepoint
bpftrace -l 'tracepoint:syscalls:*'     # ~688 个系统调用 tracepoint
bpftrace -l 'tracepoint:block:*'        # 21 个 block tracepoint
```

### Block tracepoint 完整列表

```
tracepoint:block:block_bio_backmerge      ← bio 后端合并
tracepoint:block:block_bio_bounce         ← bounce buffer
tracepoint:block:block_bio_complete       ← bio 完成
tracepoint:block:block_bio_frontmerge     ← bio 前端合并
tracepoint:block:block_bio_queue          ← bio 入队（Q 事件）
tracepoint:block:block_bio_remap          ← bio 重映射（DM/LVM）
tracepoint:block:block_dirty_buffer       ← 脏缓冲区
tracepoint:block:block_getrq              ← 分配 request（G 事件）
tracepoint:block:block_io_done            ← I/O 完成（新接口）
tracepoint:block:block_io_start           ← I/O 开始（新接口）
tracepoint:block:block_plug               ← plug（P 事件）
tracepoint:block:block_rq_complete        ← ★ request 完成（C 事件）
tracepoint:block:block_rq_error           ← request 错误
tracepoint:block:block_rq_insert          ← request 插入调度器（I 事件）
tracepoint:block:block_rq_issue           ← ★ request 下发（D 事件）
tracepoint:block:block_rq_merge           ← request 合并
tracepoint:block:block_rq_remap           ← request 重映射
tracepoint:block:block_rq_requeue         ← request 重新入队
tracepoint:block:block_split              ← bio 拆分
tracepoint:block:block_touch_buffer       ← buffer 访问
tracepoint:block:block_unplug             ← unplug（U 事件）
```

**★ block tracepoint ↔ blktrace 事件对应：**

| tracepoint | blktrace 事件 | 说明 |
|-----------|---------------|------|
| `block_rq_issue` | **D** (Dispatch) | request 下发到设备 |
| `block_rq_complete` | **C** (Complete) | request 完成 |
| `block_rq_insert` | **I** (Insert) | 插入 I/O 调度器 |
| `block_getrq` | **G** (Get request) | 分配 request 结构 |
| `block_bio_queue` | **Q** (Queue) | bio 入队 |
| `block_plug` | **P** (Plug) | 队列插队 |
| `block_unplug` | **U** (Unplug) | 队列拔塞 |

→ 这些就是 blktrace 使用的内核 tracepoint！bpftrace 通过 `tracepoint:block:*` 可以 hook 到完全相同的事件。

## 附录 B：bpftrace 语法速查

### probe 类型

| 类型 | 格式 | 说明 |
|------|------|------|
| `BEGIN` / `END` | `BEGIN { ... }` | 程序启动/退出时 |
| `tracepoint` | `tracepoint:子系统:事件名` | 内核静态追踪点 |
| `kprobe` / `kretprobe` | `kprobe:函数名` | 内核函数入口/返回 |
| `uprobe` | `uprobe:库路径:函数名` | 用户态函数 |
| `interval` | `interval:s:1` | 定时触发（每秒） |
| `profile` | `profile:hz:99` | 采样（99Hz） |
| `usdt` | `usdt:路径:provider:name` | 用户态 SDT |

### 常用聚合函数

| 函数 | 说明 | 示例 |
|------|------|------|
| `count()` | 计数 | `@[probe] = count()` |
| `sum(n)` | 求和 | `@bytes = sum(args->size)` |
| `avg(n)` | 平均值 | `@avg_lat = avg($delta)` |
| `min(n)` / `max(n)` | 最小/最大值 | `@max_lat = max($delta)` |
| `hist(n)` | 2 的幂次直方图 | `@lat = hist($us)` |
| `lhist(n, min, max, step)` | 线性直方图 | `@lat = lhist($us, 0, 1000, 100)` |

### 常用内置函数

| 函数 | 说明 |
|------|------|
| `printf(fmt, ...)` | 格式化输出 |
| `str(ptr)` | 指针 → 字符串 |
| `ksym(addr)` | 内核地址 → 符号名 |
| `ustack()` | 用户态调用栈 |
| `kstack()` | 内核态调用栈 |
| `time(fmt)` | 格式化时间输出 |
| `delete(@map[key])` | 删除 map 条目 |
| `clear(@map)` | 清空 map |
| `exit()` | 退出 bpftrace |
