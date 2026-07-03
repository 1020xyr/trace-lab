# ps 命令问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关数据源位置。

---

## 目录

- [Q1: ps 的 %CPU 超过 100% 是正常的吗？](#q1-ps-的-cpu-超过-100-是正常的吗)

---

## Q1: ps 的 %CPU 超过 100% 是正常的吗？

**日期：** 2026-07-04  
**场景：** `ps aux` 看到某个进程的 %CPU 显示为 350% 甚至更高，不确定这是 bug 还是正常行为  
**相关文件：** `docs/ps/reading/01_process_states.md`（字段 14+15 解读）、`docs/ps/reading/03_command_output_demo.md`（场景 4）  
**源码位置：** 内核 `fs/proc/array.c:446-557` — utime/stime 输出、`kernel/sched/cputime.c:293-333` — thread_group_cputime()

### 回答

**★ 完全正常。ps 的 %CPU 是 per-process（所有线程 CPU 时间之和），不是 per-core。多核系统上多线程进程可以轻松超过 100%。**

#### %CPU 的计算公式

```
                     Δ(utime + stime)
  %CPU = ───────────────────────────────── × 100
           Δwall_time × USER_HZ

其中：
  utime     = /proc/[pid]/stat 字段 14（用户态 CPU 时间，jiffies）
  stime     = /proc/[pid]/stat 字段 15（内核态 CPU 时间，jiffies）
  USER_HZ   = 100（x86 架构，见 asm-generic/param.h）
  Δ         = 两次采样的差值
```

#### 内核源码：utime/stime 是如何聚合所有线程的

```c
// fs/proc/array.c:446-527 — do_task_stat() 中的 CPU 时间获取
u64 cutime, cstime, utime, stime;
cutime = cstime = utime = stime = 0;

if (whole) {  // ★ whole=true 表示查看整个进程（所有线程）
    // ...
    thread_group_cputime_adjusted(task, &utime, &stime);
    //              ^^^^^^^^^^^^^
    //              ★ 聚合所有线程的 CPU 时间！
}
```

```c
// kernel/sched/cputime.c:293-333 — thread_group_cputime() 核心逻辑
void thread_group_cputime(struct task_struct *tsk, struct task_cputime *times)
{
    struct signal_struct *sig = tsk->signal;
    // ...
    times->utime = sig->utime;     // 已退出线程的累计时间
    times->stime = sig->stime;

    for_each_thread(tsk, t) {      // ★ 遍历进程的所有活跃线程
        task_cputime(t, &utime, &stime);
        times->utime += utime;     // ★ 每个线程的 utime 全部累加
        times->stime += stime;     // ★ 每个线程的 stime 全部累加
    }
}
```

```
关键发现：

  thread_group_cputime() 用 for_each_thread() 遍历进程的
  所有线程，将每个线程在各 CPU 核上消耗的时间全部相加。

  所以：一个 4 线程进程，每个线程占满一个核
  → utime 在 1 秒内增加 400 jiffies（假设 HZ=100）
  → %CPU = 400 / 100 × 100 = 400%
```

#### 多核场景下的 %CPU 上限

```
CPU 核心数 │ 单进程 %CPU 理论上限 │ 示例
──────────┼────────────────────┼──────────────────────────
  2 核     │ 200%              │ 2 线程各占满 1 核
  4 核     │ 400%              │ 4 线程各占满 1 核
  8 核     │ 800%              │ 8 线程各占满 1 核
 16 核     │ 1600%             │ 16 线程各占满 1 核
 64 核     │ 6400%             │ ★ 大型服务器上常见

★ %CPU 上限 = CPU 核心数 × 100%
```

#### USER_HZ 的定义

```c
// include/asm-generic/param.h:9-10
# define USER_HZ       100          /* some user interfaces are */
# define CLOCKS_PER_SEC (USER_HZ)   /* in "ticks" like times() */

// ★ x86 上 USER_HZ = 100
// /proc/[pid]/stat 中 utime/stime 的单位是 1/100 秒（centisecond）
```

```c
// kernel/time/time.c:674-688 — 纳秒到 clock_t 的转换
u64 nsec_to_clock_t(u64 x)
{
    // NSEC_PER_SEC = 1,000,000,000
    // USER_HZ = 100
    // 所以：x / (1,000,000,000 / 100) = x / 10,000,000
    return div_u64(x, NSEC_PER_SEC / USER_HZ);
}
```

#### /proc/[pid]/stat 中的 utime/stime 输出

```c
// fs/proc/array.c:554-557 — 输出到 /proc/[pid]/stat
seq_put_decimal_ull(m, " ", nsec_to_clock_t(utime));   // 字段 14: utime
seq_put_decimal_ull(m, " ", nsec_to_clock_t(stime));   // 字段 15: stime
seq_put_decimal_ll(m, " ", nsec_to_clock_t(cutime));   // 字段 16: cutime（子进程）
seq_put_decimal_ll(m, " ", nsec_to_clock_t(cstime));   // 字段 17: cstime（子进程）
```

#### 计算示例

```
假设：4 核机器，进程有 4 个线程，每个线程占满一个核

T=0s: /proc/[pid]/stat 字段 14+15 = 1000 + 500 = 1500 jiffies
T=1s: /proc/[pid]/stat 字段 14+15 = 1400 + 900 = 2300 jiffies

Δ(utime + stime) = 2300 - 1500 = 800 jiffies
Δwall_time = 1 秒
USER_HZ = 100

%CPU = 800 / (1 × 100) × 100 = 800%
                       ^^^^^
                       分母只有 100（1 秒 × USER_HZ），
                       不除以核心数！

★ 所以 4 个线程各占满 1 核 → 分子是 400 jiffies/s → %CPU = 400%
★ 这个例子中 800% 意味着 8 个逻辑线程全速运行
```

#### 完整数据流

```
内核调度器
  │  每个线程在 CPU 上运行时累积 se.sum_exec_runtime (纳秒)
  ▼
thread_group_cputime()  [cputime.c:293]
  │  for_each_thread → 累加所有线程的 utime + stime
  ▼
nsec_to_clock_t()  [time.c:674]
  │  纳秒 ÷ (10⁹ / 100) = 转换为 jiffies (USER_HZ=100)
  ▼
/proc/[pid]/stat 字段 14 (utime) + 字段 15 (stime)  [array.c:554-555]
  │
  ▼
ps (procps) 用户态
  │  两次采样读取 stat 字段 14+15
  │  %CPU = Δ(utime+stime) / (Δwall_time × USER_HZ) × 100
  ▼
输出 %CPU 列
  ★ 分子是所有核的 CPU 时间之和，分母不除以核心数
  ★ 所以 %CPU 可以超过 100%
```

#### 与 top 命令的对比

```
命令  │ %CPU 含义                   │ 超过 100%？
──────┼────────────────────────────┼──────────────────
ps    │ per-process（所有线程之和） │ ★ 可以超过 100%
top   │ per-process（所有线程之和） │ ★ 可以超过 100%
      │ （默认行为，按 Shift+I     │
      │   可切换 per-thread 模式） │
pidstat│ per-process（默认）        │ ★ 可以超过 100%
      │ -t 参数可查看 per-thread   │ per-thread 不超过 100%

★ 注意：top 的 per-thread 模式（按 H 或 -H）
  显示每个线程各自的 %CPU，单线程最高 100%
```

#### 排查 %CPU 异常的步骤

```bash
# 1. 查看进程的线程数
ps -p <PID> -o nlwp
# NLWP=12 → 12 个线程，理论最高 %CPU = 1200%（12 核以上机器）

# 2. 查看每个线程的 CPU 使用
ps -eLf -p <PID>
# LWP 列 = 线程 ID，C 列 = per-thread CPU 使用

# 3. 确认 CPU 核心数
nproc
# 或
grep -c ^processor /proc/cpuinfo

# 4. 计算合理的上限
# 如果进程有 N 个活跃线程，机器有 M 个核心
# %CPU 上限 = min(N, M) × 100%

# 5. 如果 %CPU 远超合理上限 → 可能是 BUG
# 例如：4 核机器上 %CPU=500% → 不可能，检查 ps 版本或 /proc 异常
```

**一句话总结：** ps 的 %CPU 超过 100% **完全正常**。因为 `thread_group_cputime()` 用 `for_each_thread()` 累加了所有线程在各 CPU 核上的时间，而分母（`Δwall_time × USER_HZ`）不除以核心数。在 N 核机器上，单进程 %CPU 的理论上限是 `N × 100%`。

---

*新的问题将追加到此文件。每个问题记录日期、场景、数据源位置。*
