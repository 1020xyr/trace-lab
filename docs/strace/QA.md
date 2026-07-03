# strace 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: strace -c 显示 futex 调用最多说明什么？](#q1-strace--c-显示-futex-调用最多说明什么)
- [Q2: strace 的性能开销有多大？生产环境能用吗？](#q2-strace-的性能开销有多大生产环境能用吗)

---

## Q1: strace -c 显示 futex 调用最多说明什么？

**日期：** 2026-07-04  
**场景：** strace -c -f 追踪多线程程序，futex 占 55% 且 errors 高达 70%  
**相关文件：** `docs/strace/reading/06_performance_analysis.md`  
**源码位置：** 内核 `kernel/futex.c` — `futex_wait()` / `futex_wake()`

### 回答

**★ futex 调用多 + errors 高 = 严重的用户态锁竞争。**

#### futex 是什么

```
futex（Fast Userspace muTEX）是 Linux 内核提供的底层同步原语。
用户态的 mutex、条件变量、读写锁、信号量底层都通过 futex 实现。

无竞争时（快速路径）：
  用户态原子操作（CAS）直接获取锁 → 不进入内核 → 零开销

有竞争时（慢速路径）：
  用户态 CAS 失败 → 调用 futex(FUTEX_WAIT) → 内核挂起线程
  持锁者释放锁 → 调用 futex(FUTEX_WAKE) → 内核唤醒等待线程
  ★ 每次竞争 = 2 次系统调用 + 2 次上下文切换
```

#### strace -c 分析

```bash
strace -c -f -p <PID>

# 典型锁竞争输出：
# % time     seconds  usecs/call     calls    errors syscall
# ------ ----------- ----------- --------- --------- --------
#  55.30    0.123400          10     12340      8900 futex
#                                                     ^^^^
#                                          ★ errors/calls = 72%
```

**errors 的含义：**

| errno | 含义 | 说明 |
|-------|------|------|
| EAGAIN (11) | ★ 锁已被其他线程持有 | 最常见，说明竞争激烈 |
| ETIMEDOUT (110) | 等待锁超时 | 可能死锁或严重竞争 |
| EINTR (4) | 被信号中断 | 通常无害 |

#### 锁竞争 vs 正常等待

```
锁竞争（需要排查）：
  futex calls 多（> 10K/s）
  errors/calls > 50%
  usecs/call 差异大（有些很快，有些很慢）
  ★ 根因：多线程竞争同一个 mutex

正常等待（无需处理）：
  futex calls 少（< 1K/s）
  errors 少
  usecs/call 比较稳定
  ★ 例：定时器的 futex 等待、条件变量的正常等待
```

#### 进一步定位

```bash
# 1. 用 perf record 找到持锁函数
perf record -g -p <PID> -- sleep 10
perf report
# 找 pthread_mutex_lock / std::mutex 相关的调用栈

# 2. 用 bpftrace 追踪 futex 地址
bpftrace -e '
  tracepoint:syscalls:sys_enter_futex {
    @futex_addr[arg0] = count();
  }
'
# ★ 找出被竞争最多的 futex 地址 → 对应具体的锁变量

# 3. 查看锁竞争的调用栈
perf record -e sched:sched_stat_wait -p <PID> -- sleep 10
perf report
```

**一句话总结：** strace -c 显示 futex 多 + errors 高 = 多线程锁竞争严重。用 `perf record -g` 找到持锁热点函数，用 bpftrace 追踪 futex 地址定位具体的锁。

---

## Q2: strace 的性能开销有多大？生产环境能用吗？

**日期：** 2026-07-04  
**场景：** 需要在生产环境追踪进程的系统调用，担心 strace 影响服务性能  
**相关文件：** `docs/strace/reading/06_performance_analysis.md`  
**源码位置：** 内核 `kernel/ptrace.c` — `ptrace_stop()`

### 回答

**★ strace 会导致 10-100 倍减速，不推荐在生产环境使用。生产环境应使用 perf trace。**

#### 开销来源

```
strace 基于 ptrace 机制：

每个系统调用需要 2 次 ptrace stop（入口 + 出口）：

  被追踪进程:
    syscall 入口 → ptrace_stop() → 暂停 ~5μs
    tracer 读取参数 → ~5μs
    PTRACE_SYSCALL 恢复 → ~2μs
    syscall 执行 → 正常时间
    syscall 出口 → ptrace_stop() → 暂停 ~5μs
    tracer 读取返回值 → ~5μs
    PTRACE_SYSCALL 恢复 → ~2μs
    
  ★ 每个 syscall 额外开销：~20-30 μs
  ★ 如果进程每秒 10 万次 syscall → 额外 2-3 秒/秒 = 完全无法运行
```

#### 量化测试数据

| 场景 | 无 strace | 有 strace | 减速倍数 |
|------|----------|----------|---------|
| dd bs=4k count=10000 | 0.085s | 2.4s | ★ ~28x |
| dd bs=1M count=100 | 0.050s | 0.15s | ~3x |
| ls /tmp（大量 stat） | 0.003s | 0.08s | ★ ~27x |
| nginx 1K 请求 | 0.09s | 2.5s | ★ ~28x |
| make（编译项目） | 60s | 600s | ~10x |

**★ 规律：syscall 越频繁、越小，strace 开销越大。**

#### 减小开销的方法

```bash
# 方法 1: 只追踪特定 syscall（★ 最有效）
strace -e trace=accept4,sendfile -p <PID>
# 其他 syscall 不触发 ptrace stop → 开销大幅降低

# 方法 2: seccomp 内核侧过滤
strace --seccomp -e trace=accept4 -p <PID>
# 内核在 syscall 入口就过滤，减少 ~50% 开销

# 方法 3: 统计模式
strace -c -p <PID>
# 不需要格式化输出，开销较小

# 方法 4: 输出到文件（避免终端阻塞）
strace -o /tmp/trace.log -p <PID>
```

#### 生产环境替代方案

```bash
# ★ 生产环境推荐：perf trace（开销 < 5%）
perf trace -p <PID>                    # 实时追踪
perf trace -s -p <PID>                 # 统计模式（类似 strace -c）
perf trace -e read,write -p <PID>      # 追踪特定 syscall

# 对比：
# strace 每个 syscall 开销：~20-30 μs
# perf trace 每个 syscall 开销：~0.1-0.5 μs
# ★ perf trace 开销约为 strace 的 1/50 到 1/100
```

#### 选择决策

```
生产环境？
  │
  ├─ 是 → perf trace（★ 低开销，推荐）
  │
  └─ 否（开发/测试环境）→ strace（★ 信息全，推荐）
```

**一句话总结：** strace 基于 ptrace，每个 syscall 额外 ~20-30μs 开销，导致 10-100 倍减速。生产环境用 `perf trace`（开销 < 5%），开发环境用 `strace`（信息最全）。如果必须在生产用 strace，用 `-e trace=` 限定 syscall 类型 + `--seccomp` 减小开销。

---

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
