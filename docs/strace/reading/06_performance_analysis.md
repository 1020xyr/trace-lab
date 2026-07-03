# strace 在性能分析中的应用

> 面向性能分析工程师的系统调用热点诊断手册
> 重点关注：syscall 开销量化、锁竞争识别、strace vs perf trace 选择
> 预计阅读时间：15 分钟

---

## 阅读要点

1. ★ strace -c 快速统计系统调用分布
2. ★ strace -T 找出最慢的系统调用
3. ★ 系统调用热点诊断（futex / sched_yield / 小 I/O）
4. ★ strace vs perf trace 的选择策略
5. ★ strace 的性能开销量化

---

## 一、strace -c：快速统计系统调用分布

### 1.1 基本用法

```bash
# ★ 附加到运行中的进程，统计系统调用分布
strace -c -p <PID>

# 运行命令并统计
strace -c <command>

# 追踪子进程 + 统计
strace -c -f -p <PID>
```

### 1.2 输出逐列解读

```bash
strace -c -p $(pgrep -f "nginx: worker" | head -1)
# Ctrl+C 后输出：

% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- --------
 45.23    0.045230          15      3015           epoll_wait
 22.10    0.022100           7      3157           accept4
 12.45    0.012450           4      3112           writev
  8.67    0.008670           3      2890           sendfile
  5.30    0.005300           2      2650           epoll_ctl
  3.85    0.003850           2      1925           write
  1.20    0.001200           4       300           close
  0.80    0.000800           3       266           read
  0.40    0.000400           5        80           stat
------ ----------- ----------- --------- --------- --------
100.00    0.100000                  17395           total
```

**逐列解读：**

| 列名 | 含义 | 性能分析用途 |
|------|------|------------|
| `% time` | ★ 该 syscall 占总时间的百分比 | 快速找到耗时最多的 syscall |
| `seconds` | 该 syscall 的累计耗时（秒） | 绝对时间开销 |
| `usecs/call` | ★ 每次调用的平均耗时（μs） | 单次 syscall 的开销 |
| `calls` | 调用次数 | 频率指标 |
| `errors` | 返回错误的次数 | 异常检测 |
| `syscall` | 系统调用名称 | 识别热点 |

### 1.3 排序选项

```bash
# 按调用次数排序（找频率最高的 syscall）
strace -c -S calls -p <PID>

# 按总时间排序（默认，找耗时最多的 syscall）
strace -c -S time -p <PID>

# 按平均耗时排序（找最慢的 syscall）
strace -c -S avg -p <PID>

# 按错误数排序（找异常最多的 syscall）
strace -c -S errors -p <PID>
```

---

## 二、strace -T：找出最慢的系统调用

### 2.1 基本用法

```bash
# ★ 显示每次系统调用的耗时
strace -T -p <PID>

# 输出格式：
# epoll_wait(16, [...], 512, 1000) = 1 <0.001234>
#                                    ↑ 耗时 1.234ms
# accept4(6, {...}, SOCK_NONBLOCK) = 3 <0.000015>
#                                    ↑ 耗时 15μs
```

### 2.2 配合排序找 TOP N

```bash
# ★ 找出最慢的 10 个系统调用
strace -T -p <PID> 2>&1 | \
  grep -oP '<\K[0-9.]+' | \
  sort -rn | head -10

# 或者用 awk 提取 syscall 名和耗时：
strace -T -p <PID> 2>&1 | \
  awk -F'[<(]' '/</ {
    gsub(/>/, "", $NF);
    split($0, a, "(");
    printf "%-20s %s sec\n", a[1], $NF
  }' | sort -t' ' -k2 -rn | head -20
```

### 2.3 延迟分布统计

```bash
# ★ 统计各 syscall 的延迟分布（min/avg/max/P99）
strace -T -p <PID> 2>&1 | \
  awk -F'[<(]' '/</ {
    syscall = $0; sub(/\(.*/, "", syscall);
    gsub(/>/, "", $NF);
    time = $NF + 0;
    count[syscall]++;
    total[syscall] += time;
    if (time > max[syscall]) max[syscall] = time;
    if (min[syscall] == 0 || time < min[syscall]) min[syscall] = time;
  }
  END {
    printf "%-20s %8s %8s %8s %8s\n", "syscall", "calls", "min(s)", "avg(s)", "max(s)";
    for (s in count)
      printf "%-20s %8d %8.6f %8.6f %8.6f\n",
        s, count[s], min[s], total[s]/count[s], max[s];
  }' | sort -k5 -rn
```

---

## 三、strace 的性能开销量化

### 3.1 开销来源

```
strace 的工作原理（基于 ptrace）：

  被追踪进程的每个系统调用：
  
  1. syscall 入口 → 内核暂停 tracee → 唤醒 tracer    ~2-5 μs
  2. tracer 读取寄存器/参数 → 打印日志                  ~1-10 μs
  3. tracer 调用 PTRACE_SYSCALL 恢复 tracee            ~1-2 μs
  4. syscall 出口 → 内核暂停 tracee → 唤醒 tracer      ~2-5 μs
  5. tracer 读取返回值 → 打印日志                       ~1-10 μs
  6. tracer 调用 PTRACE_SYSCALL 恢复 tracee            ~1-2 μs
  
  ★ 每个 syscall 额外开销：~10-35 μs（约 2 次 ptrace stop）
  
  如果一个进程每秒做 100,000 次 syscall：
    无 strace: 100,000 × ~1 μs/syscall = ~100 ms
    有 strace: 100,000 × ~20 μs/syscall = ~2000 ms
    ★ 减速 10-20 倍！
```

### 3.2 量化测试

```bash
# ★ 测试 strace 的性能开销
# 无 strace
time dd if=/dev/zero of=/tmp/test bs=4k count=10000
# real 0m0.085s

# 有 strace
time strace -o /dev/null dd if=/dev/zero of=/tmp/test bs=4k count=10000
# real 0m2.400s

# ★ 减速倍数：2.400 / 0.085 ≈ 28 倍

# 不同 syscall 频率下的减速倍数：
# ┌───────────────┬────────────┬─────────────┬──────────┐
# │ 场景           │ 无 strace  │ 有 strace   │ 减速倍数 │
# ├───────────────┼────────────┼─────────────┼──────────┤
# │ dd bs=4k 10K  │ 0.085s     │ 2.4s        │ ~28x     │
# │ dd bs=1M 100  │ 0.050s     │ 0.15s       │ ~3x      │
# │ ls /tmp       │ 0.003s     │ 0.08s       │ ~27x     │
# │ nginx 1K req  │ 0.09s      │ 2.5s        │ ~28x     │
# │ 编译（make）  │ 60s        │ 600s        │ ~10x     │
# └───────────────┴────────────┴─────────────┴──────────┘
#
# ★ 规律：syscall 越频繁、越短小，strace 开销越大
#   大 I/O（bs=1M）时开销小，因为 syscall 数量少
```

### 3.3 减小 strace 开销的方法

```bash
# 方法 1: 只追踪特定 syscall（减少 ptrace stop 次数）
strace -e trace=accept4,sendfile -p <PID>
# ★ 只拦截 2 种 syscall，其他 syscall 不暂停

# 方法 2: 使用 seccomp 过滤（内核侧过滤，减少用户态切换）
strace --seccomp -p <PID>
# ★ 内核在 syscall 入口就判断是否需要通知 tracer
#   不需要的 syscall 直接放行，减少 ~50% 开销

# 方法 3: 统计模式（-c）比详细输出更快
strace -c -p <PID>
# ★ 不需要格式化输出字符串，开销较小

# 方法 4: 输出到文件（避免终端 I/O 阻塞）
strace -o /tmp/trace.log -p <PID>
# ★ 终端输出本身就很慢（尤其大量输出时）
```

---

## 四、系统调用热点诊断

### 4.1 futex 调用多 → 锁竞争

```bash
# ★ futex（Fast Userspace muTEX）是用户态 mutex 的内核实现
#   当 mutex 在用户态竞争失败时，内核 futex 让线程睡眠等待

strace -c -f -p <PID>
# 输出示例：
# % time     seconds  usecs/call     calls    errors syscall
# ------ ----------- ----------- --------- --------- --------
#  55.30    0.123400          10     12340      8900 futex
#  20.10    0.044800           4     11200           epoll_wait
#  ...

# ★ futex 占 55% + errors 高达 8900 → 严重的锁竞争！
#
# 错误含义：
#   futex 的 error = EAGAIN（-11）或 ETIMEDOUT（-110）
#   表示"等待锁超时"或"锁已被其他线程抢占"
#
# 常见原因：
#   - 多线程程序中的 mutex 竞争
#   - 数据库引擎（MySQL/PostgreSQL）的锁争用
#   - Go/Java 运行时的 goroutine/thread 调度锁
#   - nginx 的 aio threads 模块中的线程池锁
#
# 进一步分析：
strace -f -p <PID> -e trace=futex -T 2>&1 | \
  awk '/futex.*EAGAIN/ {count++} END {print "EAGAIN count:", count}'
```

```
futex 高 → 锁竞争的判断流程：

  strace -c -f -p <PID>
       │
       ▼
  futex 占比 > 30%？
       │
  ┌────┴────┐
  │ 是      │ 否
  ▼         ▼
  检查 errors    不是锁竞争问题
  │              （继续看其他 syscall）
  ▼
  errors/calls > 50%？
  │
  ├─ 是 → ★ 严重锁竞争
  │       perf record -g -p <PID> → 看哪个函数在持锁
  │       bpftrace 追踪 futex 的 uaddr → 定位具体的锁地址
  │
  └─ 否 → 轻度竞争，可能是正常行为
          （如定时器的 futex 等待）
```

### 4.2 sched_yield 多 → 自旋锁或 busy wait

```bash
strace -c -p <PID>
# 输出示例：
# % time     seconds  usecs/call     calls    errors syscall
# ------ ----------- ----------- --------- --------- --------
#  70.50    0.352500           1    352500           sched_yield
#  15.20    0.076000           2     38000           read
#  ...

# ★ sched_yield 占 70% 且调用 35 万次 → 自旋等待！
#
# 含义：进程主动让出 CPU，等待某个条件满足
#   用户态的 spin lock 在竞争时会调用 sched_yield
#   或 busy-wait 循环中让出 CPU
#
# 常见场景：
#   - 用户态自旋锁（pthread_spin_lock）
#   - 无锁队列的空等待
#   - 生产者-消费者模型中的忙等待
#   - 错误的 busy-wait 实现
#
# 解决方案：
#   - 将自旋锁改为互斥锁（mutex）或条件变量（condvar）
#   - 添加适当的等待/通知机制
#   - 使用 futex-based 锁替代纯自旋锁
```

### 4.3 nanosleep / clock_nanosleep 多 → 主动等待

```bash
strace -c -p <PID>
# 输出示例：
# % time     seconds  usecs/call     calls    errors syscall
# ------ ----------- ----------- --------- --------- --------
#  80.00    4.000000       10000       400           nanosleep
#  10.50    0.525000          50     10500           read
#  ...

# ★ nanosleep 占 80%，每次 10ms → 进程在周期性睡眠
#
# 常见场景：
#   - 轮询式检查（polling loop）
#   - 限速器（rate limiter）
#   - 心跳检测
#   - 不精确的定时器实现
#
# 诊断：
#   检查 nanosleep 的 sleep 时间参数
strace -p <PID> -e trace=nanosleep -T
# nanosleep({tv_sec=0, tv_nsec=10000000}, NULL) = 0 <0.010032>
#              ^^^^^^^^^^^^^^^^^^^^^^^^
#              ★ 10ms 睡眠 → 100Hz 的轮询频率
```

### 4.4 read/write 小 I/O → 系统调用开销大

```bash
strace -c -p <PID>
# 输出示例：
# % time     seconds  usecs/call     calls    errors syscall
# ------ ----------- ----------- --------- --------- --------
#  45.00    0.450000           1    450000           write
#  35.00    0.350000           1    350000           read
#  ...

# ★ read/write 共 80 万次，每次 1μs → 大量小 I/O！
#
# 进一步查看每次 I/O 的大小：
strace -p <PID> -e trace=read,write 2>&1 | head -20
# write(5, "OK\n", 3) = 3        ← ★ 每次只写 3 字节！
# read(5, "x", 1) = 1            ← ★ 每次只读 1 字节！
# write(5, "y", 1) = 1           ← ★ 系统调用风暴！

# ★ 小 I/O 问题：
#   每次 read/write 都有一次用户态→内核态切换
#   1 字节的 write 的 syscall 开销 ≈ 1μs，
#   但传输的有效数据只有 1 字节 → 效率极低
#
# 解决方案：
#   1. 增大缓冲区（buffered I/O）
#   2. 使用 writev/readv 批量发送
#   3. 考虑 io_uring（批量化 + 零拷贝）
#   4. 使用 mmap 替代 read/write
```

### 4.5 热点诊断速查表

```
syscall 热点     │ 含义                      │ 排查方向
────────────────┼──────────────────────────┼─────────────────────
futex            │ 用户态锁竞争              │ perf record 找持锁函数
  errors 高      │ ★ 严重锁争用              │ 考虑减少锁粒度
────────────────┼──────────────────────────┼─────────────────────
sched_yield      │ 自旋等待 / busy wait      │ 改为 mutex/condvar
  调用次数多     │ ★ CPU 空转浪费              │ 添加等待/通知机制
────────────────┼──────────────────────────┼─────────────────────
nanosleep        │ 主动等待 / 轮询           │ 检查轮询频率
  usecs/call 大  │ ★ 周期性睡眠              │ 改为事件驱动
────────────────┼──────────────────────────┼─────────────────────
read/write       │ 小 I/O 风暴               │ 增大缓冲区
  calls 多       │ ★ syscall 开销占比大       │ 使用 writev/io_uring
  bytes 少       │                           │ 或 mmap
────────────────┼──────────────────────────┼─────────────────────
epoll_wait       │ 空轮询                    │ 检查超时设置
  返回 0 多      │ ★ busy polling             │ 检查 timer 事件
────────────────┼──────────────────────────┼─────────────────────
mmap/munmap      │ 频繁内存映射              │ 检查是否有内存泄漏
  calls 多       │ ★ 可能是 JVM GC           │ 或频繁的 dlopen
────────────────┼──────────────────────────┼─────────────────────
openat/close     │ 频繁打开关闭文件           │ 启用文件缓存
  calls 多       │ ★ 可能是日志频繁 rotate    │ open_file_cache
```

---

## 五、strace vs perf trace 选择

### 5.1 机制对比

```
┌────────────────┬────────────────────────┬──────────────────────────┐
│ 特性            │ strace                  │ perf trace               │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 底层机制        │ ★ ptrace               │ ★ perf_event_open        │
│                │ 每次 syscall 暂停进程    │ 内核环形缓冲区记录事件   │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 性能开销        │ ★ 10-100x 减速         │ ★ 1-5% 开销             │
│                │ ptrace stop 开销巨大     │ 采样式，不暂停进程       │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 信息详细程度    │ ★ 完整参数 + 返回值     │ 部分参数（不如 strace）  │
│                │ 字符串、结构体都展开     │ 通常只显示 syscall 名    │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 输出格式        │ 类 C 函数调用格式       │ 时间戳 + syscall 名      │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 统计模式        │ strace -c              │ perf trace -s            │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 过滤能力        │ ★ 强大（-e trace 分类） │ 有限（-e 指定 syscall）  │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 附加进程        │ strace -p PID          │ perf trace -p PID        │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 多进程追踪      │ strace -f              │ perf trace --all-cpus    │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 内核侧过滤      │ --seccomp（有限）       │ ★ perf_event 天然高效    │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 生产环境        │ ⚠️ 不推荐（开销太大）   │ ★ 推荐                   │
├────────────────┼────────────────────────┼──────────────────────────┤
│ 开发/调试环境   │ ★ 推荐（信息最全）      │ 可用但信息较少           │
└────────────────┴────────────────────────┴──────────────────────────┘
```

### 5.2 选择决策树

```
需要追踪系统调用
         │
         ▼
  ┌── 生产环境？──────────────────────────────┐
  │                                            │
  │  是                          否            │
  │  │                           │             │
  │  ▼                           ▼             │
  │  perf trace                 strace         │
  │  ★ 低开销                  ★ 信息全        │
  │  perf trace -p PID          strace -c -p PID│
  │  perf trace -s -p PID       strace -T -p PID│
  │                                            │
  │  如果需要完整参数：                         │
  │  perf trace --strace -p PID                │
  │  （perf trace 的 --strace 模式，           │
  │   信息比 strace 少但开销小）               │
  └────────────────────────────────────────────┘
```

### 5.3 perf trace 实用命令

```bash
# 基本追踪（低开销）
perf trace -p <PID>

# 统计模式（类似 strace -c）
perf trace -s -p <PID>

# 追踪特定 syscall
perf trace -e read,write,epoll_wait -p <PID>

# 显示耗时
perf trace -p <PID> 2>&1 | head -20
# 输出格式：
#  0.000 (   0.002 ms): epoll_wait(epfd: 16, events: 0x7ff..., maxevents: 512, timeout: -1) = 1
#  0.002 (   0.001 ms): accept4(fd: 6, usockaddr: 0x7ff..., uaddr_len: 0x7ff..., flags: NONBLOCK) = 3

# ★ 对比 strace：
#   strace: 每个 syscall ~10-20μs 开销
#   perf trace: 每个 syscall ~0.1-0.5μs 开销（约 20-100 倍更小）
```

### 5.4 组合使用策略

```bash
# ★ 最佳实践：perf trace 粗筛 → strace 精确定位

# 第 1 步：生产环境用 perf trace 发现问题
perf trace -s -p <PID> -- sleep 30
# 发现 futex 调用异常多

# 第 2 步：开发/测试环境用 strace 详细分析
strace -c -f -e trace=futex -p <PID>
# 查看 futex 的 error 分布

# 第 3 步：用 strace -T 找最慢的 futex
strace -f -e trace=futex -T -p <PID> 2>&1 | \
  sort -t'<' -k2 -rn | head -10
# 找到最慢的 futex 调用，分析锁地址
```

---

## ★ 总结：strace 性能分析速查

```
分析目标              │ 命令                                    │ 关注点
─────────────────────┼─────────────────────────────────────────┼────────────────
快速统计 syscall 分布 │ strace -c -p PID                        │ % time 列
找最慢的 syscall     │ strace -T -p PID                        │ <time> 值
锁竞争检测           │ strace -c -f -p PID → 看 futex         │ errors 列
自旋等待检测         │ strace -c -p PID → 看 sched_yield      │ calls 列
小 I/O 检测          │ strace -p PID -e trace=read,write       │ 每次传输大小
生产环境低开销追踪   │ perf trace -s -p PID                    │ 开销 < 5%
```
