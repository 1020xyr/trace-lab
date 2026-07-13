# fio 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: io_u 中 acct_ddir 的作用是什么？为什么和 ddir 分开？](#q1-io_u-中-acct_ddir-的作用是什么为什么和-ddir-分开)
- [Q2: verify_offset 和 offset 有什么区别？验证偏移的作用是什么？](#q2-verify_offset-和-offset-有什么区别验证偏移的作用是什么)
- [Q3: fio 如何维持 iodepth？保证在途 I/O 数目的机制是什么？](#q3-fio-如何维持-iodepth保证在途-io-数目的机制是什么)
- [Q4: rate_iops 限速机制如何工作？](#q4-rate_iops-限速机制如何工作)
- [Q5: fio 测延迟为什么必须用 iodepth=1？](#q5-fio-测延迟为什么必须用-iodepth1)
- [Q6: fio 的 slat/clat 与 blktrace 的 Q2D/D2C 是什么关系？](#q6-fio-的-slatclat-与-blktrace-的-q2dd2c-是什么关系)
- [Q7: fio 的 --numjobs 和 --iodepth 有什么区别？对 IOPS 的影响是什么？](#q7-fio-的---numjobs-和---iodepth-有什么区别对-iops-的影响是什么)
- [Q8: 为什么 fio 用 libaio 引擎时必须加 --direct=1？](#q8-为什么-fio-用-libaio-引擎时必须加---direct1)
- [Q9: io_u 空闲池（io_u_freelist）的个数就是 iodepth 参数吗？](#q9-io_u-空闲池io_u_freelist的个数就是-iodepth-参数吗)
- [Q10: get_next_offset 中 offset 为什么不能大于 io_size？](#q10-get_next_offset-中-offset-为什么不能大于-io_size)
- [Q11: fio 中的"文件"是什么含义？能否直接对裸设备读写？](#q11-fio-中的文件是什么含义能否直接对裸设备读写)
- [Q12: sync 引擎的 .prep 何时调用？能否通过多 job/多 qd 实现并发？](#q12-sync-引擎的-prep-何时调用能否通过多-job多-qd-实现并发)
- [Q13: fio 的线程模式是什么？默认是什么？为什么 SPDK 需要 --thread？](#q13-fio-的线程模式是什么默认是什么为什么-spdk-需要---thread)
- [Q14: 为什么 fio 默认进程模式？线程模式看起来没坏处](#q14-为什么-fio-默认进程模式线程模式看起来没坏处)
- [Q15: 如何用 IO depth 分布诊断瓶颈？](#q15-如何用-io-depth-分布诊断瓶颈)

---

## Q1: io_u 中 acct_ddir 的作用是什么？为什么和 ddir 分开？

**日期：** 2026-07-03  
**场景：** 阅读 `01_io_u_struct.h` 时发现 `acct_ddir` 字段不理解  
**相关文件：** `docs/fio/reading/01_io_u_struct.h`  
**源码位置：** `src/fio/io_u.h:199` — `acct_ddir()` 函数

### 回答

**`ddir` = 实际 I/O 方向，`acct_ddir` = 统计归类方向。** 两者分离是为了支持 I/O 回放场景中"实际做的"和"统计算的"不一致。

#### 核心机制

```c
// io_u.h:199 — acct_ddir() 是一个 inline 函数
static inline enum fio_ddir acct_ddir(struct io_u *io_u)
{
    if (io_u->acct_ddir != -1)    // ★ 如果设置了归类方向
        return io_u->acct_ddir;   //    用归类方向做统计
    return io_u->ddir;            //    否则用实际方向
}
```

- `ddir`：这个 I/O **实际**是读还是写（由引擎执行）
- `acct_ddir`：这个 I/O 的延迟/带宽**统计到哪个类别**里

#### 使用场景：blktrace 回放

```bash
# 录制一个写工作负载
blktrace -d /dev/vdb -o workload -w 30

# 回放时，用 --read_iolog 读取 blktrace 文件
# 如果想在只读设备上回放（只能读不能写），可以：
fio --read_iolog=workload.blktrace.0 --filename=/dev/vdb --rw=read
```

此时：
- `ddir = DDIR_WRITE`（blktrace 中记录的是写操作）
- `acct_ddir = DDIR_READ`（用户指定统计到"读"类别中）

fio 在统计延迟/带宽时，使用 `acct_ddir()` 而不是 `ddir`：

```c
// backend.c:1909 — 统计 I/O 数量时
if (ddir_rw(acct_ddir(io_u))) {
    td->io_issues[acct_ddir(io_u)]++;  // ★ 用 acct_ddir 决定计入读/写/trim 哪个桶
}
```

#### 默认行为

```c
// io_u.c:884 — 正常 I/O 中两者相同
io_u->ddir = io_u->acct_ddir = ddir;

// io_u.c:1792 — 初始化为 -1（表示"未设置，用 ddir"）
io_u->acct_ddir = -1;
```

**一句话总结：** 正常使用时 `acct_ddir = ddir`，两者没有区别。只有在 I/O 回放（`--read_iolog`）且需要把写操作统计为读操作时，`acct_ddir` 才与 `ddir` 不同。

---

## Q2: verify_offset 和 offset 有什么区别？验证偏移的作用是什么？

**日期：** 2026-07-03  
**场景：** 阅读 `01_io_u_struct.h` 时不理解 `verify_offset` 字段  
**相关文件：** `docs/fio/reading/01_io_u_struct.h`  
**源码位置：** `src/fio/options.c:3521` — 选项定义；`src/fio/verify.c` — 验证逻辑

### 回答

**`offset` 是文件级概念（写到文件的哪里），`verify_offset` 是缓冲区级概念（校验头藏在数据块内的哪个位置）。**

#### 数据验证机制图解

```
写入时的数据块布局（4KB 块）：

  offset = 文件中的偏移（如 0x1000 = 第 4KB 位置）
  
  ┌─ io_u->buf（数据缓冲区）──────────────────────┐
  │                                                │
  │  ← verify_offset →│← verify_header →│         │
  │  (偏移 N 字节)     │ (校验头: CRC/MD5│         │
  │                    │  + offset 值)   │         │
  │                    └────────────────┘          │
  │                                                │
  │  剩余数据...                                    │
  │                                                │
  └────────────────────────────────────────────────┘
  
  verify_offset = 0（默认）：校验头在缓冲区开头
  verify_offset = 512：校验头跳过前 512 字节
```

#### 源码证据

```c
// options.c:3521 — verify_offset 选项定义
{
    .name    = "verify_offset",
    .help    = "Offset verify header location by N bytes",  // "校验头偏移 N 字节"
    .minval  = sizeof(struct verify_header),  // 最小值 = 校验头大小
    .parent  = "verify",                       // 只有启用 --verify 时才有效
    .hide    = 1,                              // 隐藏选项（不常用）
}

// verify.c:1281 — 写入校验头
hdr->offset = io_u->verify_offset + header_num * td->o.verify_interval;
//            ^^^^^^^^^^^^^^^^^^^^
//            校验头记录的 offset = 缓冲区偏移 + 第几个头 × 间隔

// verify.c:946 — 读回验证时检查
if (hdr->offset != io_u->verify_offset + hdr_num * td->o.verify_interval)
    // 验证校验头中记录的 offset 是否匹配
```

#### 使用场景

| 场景 | verify_offset | 原因 |
|------|--------------|------|
| **常规验证** | 0（默认） | 校验头放在数据块开头，最简单 |
| **模拟文件系统** | 512 或更大 | 文件系统元数据在块开头，校验头需避开 |
| **兼容已有格式** | 自定义 | 数据块前 N 字节有特定格式要求 |
| **只验证部分区域** | 自定义 | 只校验数据块中某个偏移之后的部分 |

#### 示例

```bash
# 默认验证：校验头在缓冲区开头
fio --verify=crc32c --rw=write --bs=4k --size=10M --filename=/tmp/test

# 偏移验证：校验头从第 64 字节开始
fio --verify=crc32c --verify_offset=64 --rw=write --bs=4k --size=10M --filename=/tmp/test
```

**一句话总结：** `offset` = "I/O 写到文件的哪个位置"（文件偏移），`verify_offset` = "校验头藏在数据缓冲区的哪个字节"（缓冲区内偏移）。默认 `verify_offset=0`，特殊场景（模拟文件系统、兼容数据格式）下可偏移。

---

## Q3: fio 如何维持 iodepth？保证在途 I/O 数目的机制是什么？

**日期：** 2026-07-03  
**场景：** 阅读 `04_backend_do_io.c` 中的水位线示意图时不理解 reap 机制  
**相关文件：** `docs/fio/reading/04_backend_do_io.c`  
**源码位置：** `src/fio/backend.c` — `do_io()`, `wait_for_completions()`; `src/fio/io_u.c` — `queue_full()`, `io_u_queued_complete()`

### 回答

**默认情况下 fio 精确维持 cur_depth ≈ iodepth。只有显式配置 batch 参数时才会出现波动。**

#### 关键源码：iodepth_low 的默认值

```c
// init.c:810
if (o->iodepth_low > o->iodepth || !o->iodepth_low)
    o->iodepth_low = o->iodepth;    // ★ 默认 iodepth_low = iodepth！
```

这意味着默认的收割循环只执行 1 次就停止：

```c
// backend.c:424 — wait_for_completions()
do {
    ret = io_u_queued_complete(td, min_evts);  // 收割 min_evts 个（默认 1）
} while (full && (td->cur_depth > td->o.iodepth_low));
//  cur_depth(31) > iodepth_low(32) → false → 立即停止！
```

#### 默认行为（无 batch 参数）：精确维持

```
默认值：
  iodepth = 32
  iodepth_low = 32        ← 默认等于 iodepth
  iodepth_batch_complete_min = 1  ← 每次只收割 1 个

cur_depth
 32 │ ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■  ← 几乎恒定！
    │     reap 1个       reap 1个
    │     ↓ 立刻补1个     ↓ 立刻补1个
 31 │     ■               ■
    └──────┴───────────────┴────────────────
```

→ **收割 1 个，提交 1 个，cur_depth 始终 ≈ iodepth。`iodepth` 参数名不误导。**

#### 配置 batch 参数后：才会波动

```bash
fio --iodepth=32 --iodepth_batch_complete_min=8 --iodepth_low=4
```

```
cur_depth
 32 │ ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■  满！
    │ ↓ 一次收割 8 个
 24 │ ■■■■■■■■■■■■■■■■■■■■■■■■■■
    │ ↓ 继续收割（cur_depth > iodepth_low=4）
  4 │ ■■■■  ← 停！（cur_depth <= iodepth_low）
    │ ↓ 回去批量提交
 32 │ ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■  又满了
```

#### 三种模式对比

| 配置 | 实际行为 | iodepth 参数是否"误导" |
|------|---------|----------------------|
| 默认（无 batch） | cur_depth ≈ iodepth，恒定 | ❌ 不误导，符合预期 |
| `iodepth_batch_complete_min=8` | cur_depth 在 low~high 波动 | ⚠️ 用户需理解 batch 含义 |
| `iodepth_low=4` | cur_depth 降到 4 才停收割 | ⚠️ 用户主动配置的 |

#### 为什么提供 batch 参数？

| 方案 | 优点 | 缺点 |
|------|------|------|
| 精确维持（默认） | 恒定并发度，符合直觉 | 每完成 1 个就调用 1 次 getevents，系统调用多 |
| 批量收割（batch） | 减少系统调用开销 | 在飞数波动，实际平均 QD < iodepth |

高 IOPS 场景（>500K）下，系统调用开销显著，batch 模式可提升吞吐 5-10%。

---

## Q4: rate_iops 限速机制如何工作？

**日期：** 2026-07-03  
**场景：** 阅读 `04_backend_do_io.c` 时看到 `rate_iops` 不理解  
**相关文件：** `docs/fio/reading/04_backend_do_io.c`  
**源码位置：** `src/fio/backend.c` — `usec_for_io()`, `check_min_rate()`

### 回答

**rate_iops 通过计算每个 I/O 之间的最小间隔时间来限速，确保不超过指定的 IOPS。**

#### 工作原理

```
--rate_iops=1000（每秒最多 1000 次 I/O）

每次提交 I/O 后：
  rate_next_io_time = usec_for_io()
                    = 1000000 / 1000 = 1000μs

下一个 I/O 必须在 1000μs 之后才能提交：
  if (now < rate_next_io_time) → 等待
  if (now >= rate_next_io_time) → 立即提交
```

#### 核心函数

```c
// backend.c:856 — 计算下一个 I/O 的允许提交时间
static long long usec_for_io(struct thread_data *td, enum fio_ddir ddir)
{
    uint64_t bps = td->rate_bps[ddir];

    if (td->o.rate_process == RATE_PROCESS_POISSON) {
        // ★ 泊松分布模式：随机间隔（模拟真实流量）
        iops = bps / min_bs;
        val = (1000000 / iops) * -logf(random());
        td->last_usec[ddir] += val;
    } else {
        // ★ 均匀模式（默认）：固定间隔
        iops = bps / min_bs;
        td->last_usec[ddir] += 1000000 / iops;
    }
    return td->last_usec[ddir];
}

// backend.c:1347 — 每次提交后更新下一个允许时间
if (should_check_rate(td))
    td->rate_next_io_time[ddir] = usec_for_io(td, ddir);
```

#### 参数速查

| 参数 | 含义 | 效果 |
|------|------|------|
| `--rate=10M` | 限制带宽 10MB/s | 根据 bs 自动换算间隔 |
| `--rate_iops=1000` | 限制 IOPS 1000 | 每 1000μs 一个 I/O |
| `--rate_min=5M` | 最低带宽保证 | 达不到则报错退出 |
| `--rate_iops_min=500` | 最低 IOPS 保证 | 达不到则报错退出 |
| `--rate_process=poisson` | 泊松分布随机间隔 | 模拟真实流量 |

#### rate_iops vs rate 的区别

```
--rate=10M --bs=4k:
  bps = 10485760 bytes/s
  间隔 = 4096 / 10485760 = 390μs → IOPS ≈ 2560

--rate_iops=1000 --bs=4k:
  间隔 = 1000000 / 1000 = 1000μs → 带宽 = 4MB/s
```

**一句话总结：** `rate_iops` 通过 `usec_for_io()` 计算每次 I/O 之间的最小间隔时间，fio 在提交前检查当前时间是否已超过 `rate_next_io_time`，未到则等待，从而实现限速。

---

## Q5: fio 测延迟为什么必须用 iodepth=1？

**日期：** 2026-07-04  
**场景：** 测试 NVMe SSD 延迟时用了 iodepth=32，测出 clat avg=450μs，远高于设备标称的 ~80μs  
**相关文件：** `docs/fio/reading/12_best_practices.md`  
**源码位置：** `src/fio/backend.c` — `do_io()` 中的 in-flight I/O 管理

### 回答

**★ iodepth > 1 时测的是"排队延迟"而非"设备延迟"。iodepth=1 才能测出设备的真实延迟。**

#### 排队效应图解

```
iodepth=1（正确测延迟）：

  I/O 1: [──── 80μs ────]
  I/O 2:                 [──── 75μs ────]
  I/O 3:                                [──── 85μs ────]
  ★ 每个 I/O 独占设备，lat ≈ 设备真实延迟 = ~80μs

iodepth=32（延迟被排队放大）：

  32 个 I/O 同时提交：
  I/O  1: [─── 80μs ───]
  I/O  2: [──── 120μs ─────]  ← 等待 I/O 1 释放设备资源
  I/O  3: [───── 180μs ──────]  ← 排队更久
  ...
  I/O 32: [──────────── 800μs ──────────────]  ← 排队延迟！
  
  ★ 平均 clat ≈ 450μs（包含了排队等待时间）
  ★ 这不是设备真实延迟，是排队延迟
```

#### 源码视角

```c
// backend.c — do_io() 中的 iodepth 维持逻辑
// 当 iodepth=32 时：
//   fio 先提交 32 个 I/O，然后每完成 1 个才提交 1 个
//   这意味着任意时刻都有 ~32 个 I/O 在设备队列中
//
// 设备处理 32 个并发 I/O 时：
//   每个 I/O 的延迟 = 设备处理时间 + 排队等待时间
//   排队等待时间 ≈ (iodepth - 1) × 单次设备处理时间 / 并行度
```

#### 正确的延迟测试配置

```bash
# ★ 延迟测试黄金配置
fio --name=latency_test \
    --ioengine=libaio \
    --direct=1 \
    --rw=randwrite \
    --bs=4k \
    --iodepth=1 \
    --numjobs=1 \
    --filename=/dev/vdb \
    --runtime=30 \
    --time_based
```

#### 各参数缺一不可

| 参数 | 必须值 | 设错的后果 |
|------|--------|----------|
| `iodepth` | ★ 1 | iodepth > 1 → 排队延迟，结果偏高 3-10 倍 |
| `numjobs` | 1 | numjobs > 1 → 多 job 竞争设备，延迟偏高 |
| `direct` | ★ 1 | direct=0 → page cache 命中，延迟 ~0.5μs，不真实 |
| `bs` | 4k | bs 越大 → 传输时间越长 → 延迟偏高 |

**一句话总结：** iodepth=1 保证每个 I/O 独占设备资源，测出的是设备真实延迟。iodepth > 1 时多个 I/O 排队竞争，测的是排队延迟（包含了等待时间），不是设备性能指标。

---

## Q6: fio 的 slat/clat 与 blktrace 的 Q2D/D2C 是什么关系？

**日期：** 2026-07-04  
**场景：** 同时运行 fio + blktrace，对比 slat 和 Q2D、clat 和 D2C 时发现数值不一致  
**相关文件：** `docs/fio/reading/09_stat_output.md`, `docs/fio/reading/12_best_practices.md`  
**源码位置：** `src/fio/ioengines.c` — `td_io_queue()`; `src/fio/io_u.c` — `io_u_queued_complete()`

### 回答

**★ slat ≈ Q2D，clat ≈ D2C，lat ≈ Q2C，但不精确等价。差异来自测量视角不同（用户态 vs 内核态）。**

#### 时间线对照

```
fio 用户态视角：
  start_time ──→ issue_time ──→ complete_time
  │←─ slat ──→│←── clat ───→│
  │←──────── lat ──────────→│

blktrace 内核视角：
  Q (bio入队) ──→ D (下发设备) ──→ C (中断完成)
  │←── Q2D ───→│←─── D2C ────→│
  │←────────── Q2C ──────────→│
```

#### 精确对照表

| fio 指标 | blktrace 对应 | 精确度 | 差异原因 |
|---------|-------------|-------|---------|
| **slat** | Q2D | ★ 近似 | slat = 用户态 get_io_u → io_submit，含 syscall 开销；Q2D = 内核 bio 入队 → 下发驱动，含 I/O 调度开销 |
| **clat** | D2C | ★ 近似 | clat 包含完成通知延迟（中断→getevents），D2C 只到中断完成 |
| **lat** | Q2C | ★★ 最接近 | 两者都是端到端延迟，lat 略大于 Q2C（包含用户态首尾开销） |

#### 实战数据对比

```
fio 输出：
  slat avg = 2.1μs,  clat avg = 180μs,  lat avg = 182μs

btt 输出：
  Q2D avg = 5.2μs,  D2C avg = 175.3μs,  Q2C avg = 180.5μs

分析：
  Q2D (5.2) > slat (2.1)  → 差值 3.1μs = 内核 I/O 调度开销
  clat (180) > D2C (175.3) → 差值 4.7μs = 中断通知到用户态的开销
  lat (182) ≈ Q2C (180.5)  → 差值 1.5μs = 用户态首尾开销
```

#### 差异的根因

```
slat vs Q2D 的差异来源：

  fio:     get_io_u() → io_prep → io_submit()  ← 全在用户态
  kernel:                                        io_submit() 进入内核
  blktrace:                                      Q (bio 入队) → I/O 调度 → D (下发)
  
  ★ slat 不含内核调度时间，Q2D 含 → Q2D 通常 > slat
  
clat vs D2C 的差异来源：

  blktrace: D (下发设备) → 设备处理 → C (中断完成)
  kernel:                                         中断处理 → 完成 io_request
  fio:                                            getevents() 返回 → clat 计算
  
  ★ clat 包含中断→用户态通知时间，D2C 不含 → clat 通常 > D2C
```

#### 如何正确使用

```
1. 评估设备性能：看 D2C（blktrace）或 clat（fio）
   → 两者都是设备处理时间，差异 < 5% 可忽略
   
2. 评估全链路延迟：看 Q2C（blktrace）或 lat（fio）
   → 两者最接近，差异 < 1%
   
3. 定位瓶颈层：
   slat >> clat → 瓶颈在软件层（io_submit 慢）
   clat >> slat → 瓶颈在设备层（设备处理慢）
   Q2D >> D2C → 瓶颈在内核调度层（I/O 调度器开销）
   D2C >> Q2D → 瓶颈在设备层
```

**一句话总结：** fio slat ≈ blktrace Q2D（提交延迟），fio clat ≈ blktrace D2C（设备延迟），fio lat ≈ blktrace Q2C（端到端延迟）。差异来自测量视角（用户态 vs 内核态），lat/Q2C 最接近。

---

## Q7: fio 的 --numjobs 和 --iodepth 有什么区别？对 IOPS 的影响是什么？

**日期：** 2026-07-04
**场景：** 测试 SSD 性能时混淆 numjobs 和 iodepth，导致 IOPS 结果不符合预期
**相关文件：** `docs/fio/reading/04_backend_do_io.c`
**源码位置：** `src/fio/options.c:3193-3201`（numjobs 定义）、`src/fio/options.c:2427-2437`（iodepth 定义）、`src/fio/init.c:1965-1997`（job 克隆逻辑）

### 回答

**★ numjobs 创建多个独立的 job（线程/进程），iodepth 控制每个 job 内部的在途 I/O 数。两者是乘法关系：总并发 I/O = numjobs × iodepth。**

#### 核心区别

```
numjobs（作业级并发）：
  options.c:3197 — .help = "Duplicate this job this many times"
  init.c:1965-1997 — 通过 get_new_job() 克隆完全相同的 thread_data

iodepth（I/O 级并发）：
  options.c:2431 — .help = "Number of IO buffers to keep in flight"
  backend.c — 每个 job 独立维持在途 I/O 数
```

#### 架构图解

```
--numjobs=4 --iodepth=8：

  Job 1（线程/进程 1）
  ├─ I/O 1 ─┐
  ├─ I/O 2 ─┤
  ├─ ...     ├─ 8 个在途 I/O
  └─ I/O 8 ─┘

  Job 2（线程/进程 2）
  ├─ I/O 1 ─┐
  ├─ I/O 2 ─┤
  ├─ ...     ├─ 8 个在途 I/O
  └─ I/O 8 ─┘

  Job 3、Job 4 同上...

  ★ 总并发 I/O = 4 × 8 = 32 个同时在途
```

#### 源码验证

```c
// init.c:1965-1972 — numjobs 通过克隆 job 实现
numjobs = o->numjobs;
while (--numjobs) {
    struct thread_data *td_new = get_new_job(false, td, true, jobname);
    td_new->o.numjobs = 1;       // ★ 子 job 的 numjobs 设为 1，避免递归
    td_new->subjob_number = numjobs;
    add_job(td_new, jobname, numjobs, 1, client_type);
}

// backend.c:2802-2806 — 每个 job 创建独立线程
if (td->o.use_thread)
    ret = pthread_create(&td->thread, NULL, thread_main, fd);

// options.c:3193-3201 — numjobs 定义
{
    .name = "numjobs",
    .help = "Duplicate this job this many times",
    .def  = "1",              // ★ 默认值 = 1（不复制）
}

// options.c:2427-2437 — iodepth 定义
{
    .name = "iodepth",
    .help = "Number of IO buffers to keep in flight",
    .def  = "1",              // ★ 默认值 = 1（串行 I/O）
}
```

#### 对 IOPS 的影响

| 配置 | 总并发 I/O | IOPS（SSD 示例） | 说明 |
|------|----------|----------------|------|
| `numjobs=1, iodepth=1` | 1 | ~15K | 串行，最低 IOPS |
| `numjobs=1, iodepth=32` | 32 | ~350K | 单 job 高并发 |
| `numjobs=4, iodepth=8` | 32 | ~340K | 总并发相同，但多 job 有锁开销 |
| `numjobs=4, iodepth=32` | 128 | ~400K | ★ 并发翻倍，IOPS 继续增长 |
| `numjobs=32, iodepth=32` | 1024 | ~380K | ★ 过多 job 导致竞争，IOPS 反降 |

#### ★ 关键区别：numjobs 增加有额外开销

```
iodepth 增加：
  → 仅增加单个 job 内部的在途 I/O 数
  → 无额外线程/进程开销
  → ★ 推荐优先用 iodepth 提升并发度

numjobs 增加：
  → 创建独立的线程/进程
  → 每个 job 有独立的：内存分配、io_context、文件描述符
  → 多 job 竞争同一设备 → accept/IO 锁竞争
  → ★ 适用于模拟多用户/多应用并发场景
```

#### 使用场景对照

| 场景 | 推荐配置 | 原因 |
|------|---------|------|
| **测试设备最大 IOPS** | `numjobs=1, iodepth=128` | 单 job 高并发，无多进程竞争 |
| **模拟数据库** | `numjobs=8, iodepth=32` | 模拟多个 DB worker 线程 |
| **测试延迟** | `numjobs=1, iodepth=1` | 完全串行，测真实设备延迟 |
| **压力测试** | `numjobs=16, iodepth=64` | 最大化并发，压满设备 |

**一句话总结：** numjobs = "几个人同时干活"（作业级并发），iodepth = "每个人手里同时拿着几个任务"（I/O 级并发）。总并发 = numjobs × iodepth。优先用 iodepth 提升并发度，numjobs 用于模拟多用户场景。

---

## Q8: 为什么 fio 用 libaio 引擎时必须加 --direct=1？

**日期：** 2026-07-04
**场景：** 使用 `fio --ioengine=libaio --iodepth=32` 但没有加 `--direct=1`，发现 IOPS 远低于预期，iodepth 似乎不起作用
**相关文件：** `docs/fio/reading/04_backend_do_io.c`
**源码位置：** `src/fio/engines/libaio.c:325-343`（io_submit 调用）、`src/linux-5.10/mm/filemap.c:2503-2553`（IOCB_DIRECT 分支）、`src/linux-5.10/fs/aio.c:1496-1513`（aio_rw_done）

### 回答

**★ libaio + buffered IO（无 direct=1）时，内核在 io_submit() 中同步完成 I/O，iodepth 形同虚设。加 direct=1 才能实现真正的异步 I/O。**

#### 问题现象

```bash
# 没有 direct=1 — iodepth 不起作用
fio --ioengine=libaio --iodepth=32 --rw=randread --bs=4k --filename=/dev/vdb
# IOPS ≈ 15K（等同于 iodepth=1 的 sync 引擎！）

# 加了 direct=1 — iodepth 正常工作
fio --ioengine=libaio --iodepth=32 --direct=1 --rw=randread --bs=4k --filename=/dev/vdb
# IOPS ≈ 350K（真正的异步并发）
```

#### 根因：内核的 IOCB_DIRECT 分支

```c
// mm/filemap.c:2503-2553 — generic_file_read_iter()
ssize_t generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
    if (iocb->ki_flags & IOCB_DIRECT) {
        // ★ O_DIRECT 路径：调用驱动的 direct_IO，真正异步
        retval = mapping->a_ops->direct_IO(iocb, iter);
        // → 返回 -EIOCBQUEUED → I/O 真正在后台执行
        // ...
    }

    // ★ 非 O_DIRECT 路径：走 buffered read，同步完成！
    retval = generic_file_buffered_read(iocb, iter, retval);
    // → 在当前线程中同步读取 page cache
    // → io_submit() 期间 I/O 就完成了
}
```

```c
// fs/aio.c:1496-1513 — aio_rw_done()
static inline void aio_rw_done(struct kiocb *req, ssize_t ret)
{
    switch (ret) {
    case -EIOCBQUEUED:
        break;            // ★ 异步：I/O 已排队，等完成回调
    default:
        req->ki_complete(req, ret, 0);  // ★ 同步：I/O 已完成，立即回调
    }
}
```

#### 流程对比图

```
buffered IO（无 direct=1）：

  fio: io_submit(iocb_1, iocb_2, ..., iocb_32)
       │
       ├─ 内核 generic_file_read_iter()
       │  └─ IOCB_DIRECT 未设置 → generic_file_buffered_read()
       │     └─ 同步从 page cache 读取 → I/O 1 完成 ✓
       │  └─ 同理 I/O 2~32 全部同步完成 ✓
       │
       └─ io_submit() 返回时，32 个 I/O 已经全部完成！
          ★ 实际是串行处理，iodepth 无意义

O_DIRECT（direct=1）：

  fio: io_submit(iocb_1, iocb_2, ..., iocb_32)
       │
       ├─ 内核 generic_file_read_iter()
       │  └─ IOCB_DIRECT 设置 → mapping->a_ops->direct_IO()
       │     └─ 提交到块设备驱动 → 返回 -EIOCBQUEUED
       │  └─ 32 个 I/O 全部排队到设备 ← ★ 真正并发！
       │
       └─ io_submit() 立即返回
          fio: io_getevents() 等待完成通知
```

#### fio 的 libaio 引擎不检查 direct

```c
// engines/libaio.c:458-464 — libaio 引擎标志
FIO_STATIC struct ioengine_ops ioengine = {
    .name = "libaio",
    .flags = FIO_ASYNCIO_SYNC_TRIM |
             FIO_ASYNCIO_SYNC_SYNCFS |
             FIO_ASYNCIO_SETS_ISSUE_TIME |
             FIO_ATOMICWRITES,
    // ★ 注意：没有 FIO_SYNCIO 标志！
    // fio 认为 libaio 是异步引擎，不会警告 iodepth 问题
};

// engines/libaio.c:267-297 — fio_libaio_queue() 总是返回 FIO_Q_QUEUED
static enum fio_q_status fio_libaio_queue(struct thread_data *td,
                                          struct io_u *io_u)
{
    // ...
    ld->iocbs[ld->head] = &io_u->iocb;
    ring_inc(ld, &ld->head, 1);
    ld->queued++;
    return FIO_Q_QUEUED;  // ★ 告诉 fio"I/O 已排队"
                           // 但实际 buffered IO 在 io_submit 时已同步完成
}
```

#### 官方示例验证

```bash
# fio 官方 aio 示例也用了 buffered=0（等价于 direct=1）
# src/fio/examples/aio-read.fio
[global]
ioengine=libaio
buffered=0       # ★ buffered=0 等价于 direct=1
rw=randread
bs=128k
```

#### 解决方案

```bash
# ★ 方案 1（推荐）：加 direct=1
fio --ioengine=libaio --direct=1 --iodepth=32 ...

# ★ 方案 2：如果必须测 buffered IO，改用 io_uring 引擎
fio --ioengine=io_uring --iodepth=32 ...
# io_uring 在 buffered IO 下也能实现真正的异步

# ★ 方案 3：用 sync 引擎测 buffered IO（接受 iodepth=1）
fio --ioengine=sync --iodepth=1 ...
# 明确知道是同步 I/O，不设虚假的 iodepth
```

#### 各引擎 + direct 对照

| 引擎 | direct=1 | direct=0 | 说明 |
|------|----------|----------|------|
| **libaio** | ★ 真正异步，iodepth 生效 | ⚠️ 实质同步，iodepth 无效 | 必须加 direct=1 |
| **io_uring** | ★ 真正异步 | ★ 也支持异步 | 不需要 direct=1 |
| **sync** | 无意义 | 无意义 | 总是同步，iodepth=1 |
| **posixaio** | ★ 真正异步 | ⚠️ 可能同步 | 推荐加 direct=1 |

**一句话总结：** Linux AIO（libaio）的设计只保证 O_DIRECT 路径下的真正异步。不加 `direct=1` 时，内核在 `io_submit()` 中同步完成 I/O，iodepth 形同虚设。这是内核 `generic_file_read_iter()` 的代码逻辑决定的，不是 fio 的 bug。

---

## Q9: io_u 空闲池（io_u_freelist）的个数就是 iodepth 参数吗？

**日期：** 2026-07-04  
**场景：** 阅读 `04_backend_do_io.c` 中 get_io_u 从空闲池弹出 io_u 的描述，想确认池大小与 iodepth 的关系  
**相关文件：** `docs/fio/reading/04_backend_do_io.c`  
**源码位置：** `src/fio/backend.c:1507` — `init_io_u()` 函数

### 回答

**是的，io_u 空闲池的容量 = iodepth。** fio 在启动时预分配 `iodepth` 个 io_u 结构体，全部放入 `io_u_freelist`。

#### 源码证据

```c
// backend.c:1507 — init_io_u()
static int init_io_u(struct thread_data *td)
{
    // ★ 1. 创建三个队列，容量都是 td->o.iodepth
    err += !io_u_rinit(&td->io_u_requeues, td->o.iodepth);           // 重排队列
    err += !io_u_qinit(&td->io_u_freelist, td->o.iodepth, false);   // ★ 空闲池
    err += !io_u_qinit(&td->io_u_all, td->o.iodepth, ...);          // 全部 io_u

    // ★ 2. 循环 iodepth 次，每次分配一个 io_u
    for (i = 0; i < td->o.iodepth; i++) {                           // ← 上限 = iodepth
        io_u = fio_memalign(cl_align, sizeof(*io_u), ...);          // 分配内存
        io_u->flags = IO_U_F_FREE;                                  // 标记为空闲
        io_u_qpush(&td->io_u_freelist, io_u);                       // ★ 推入空闲池
    }
}
```

#### 三个队列的关系

| 队列 | 容量 | 实际元素数 | 用途 |
|------|------|----------|------|
| `io_u_freelist` | iodepth | 0 ~ iodepth（动态） | ★ 空闲池，get_io_u 从这里取 |
| `io_u_all` | iodepth | 始终 = iodepth | 所有 io_u（含在飞的），用于遍历 |
| `io_u_requeues` | iodepth | 0 ~ iodepth | 需要重新提交的 io_u（部分完成等） |

三者容量都是 `iodepth`，但只有 `io_u_freelist` 的实际元素数会随 `cur_depth` 变化。

#### 运行时机制

```
iodepth = 32

启动:  init_io_u() 分配 32 个 io_u → 全部放入 freelist
       freelist=[32个]  cur_depth=0

运行:  get_io_u() 弹出 1 个 → freelist=[31个]  cur_depth=1
       ...重复...
       get_io_u() 弹出 1 个 → freelist=[0个]   cur_depth=32  ← 满了！
       
       queue_full() = freelist为空 → wait_for_completions()
       收割 1 个 → put_io_u() 归还 → freelist=[1个]  cur_depth=31
       继续提交...
```

#### queue_full() 的判断依据

```c
// io_u.c:1745
bool queue_full(const struct thread_data *td)
{
    return io_u_qempty(&td->io_u_freelist);  // ★ 空闲池为空 = 满
}
```

fio 不直接比较 `cur_depth == iodepth`，而是检查**空闲池是否为空**。因为池大小 = iodepth，所以池空等价于在飞数达到上限。

**一句话总结：** `init_io_u()` 在启动时预分配 `iodepth` 个 io_u 放入空闲池，运行时从池中取出/归还。池的容量就是 iodepth，这是 fio 控制并发 I/O 数的物理基础。

---

## Q10: get_next_offset 中 offset 为什么不能大于 io_size？

**日期：** 2026-07-04  
**场景：** 阅读 `05_io_u_get.c` 中 get_next_offset() 的边界检查不理解  
**相关文件：** `docs/fio/reading/05_io_u_get.c`  
**源码位置：** `src/fio/io_u.c:553` — `get_next_offset()`；`src/fio/filesetup.c:1236`

### 回答

**io_size 是 fio 的"I/O 工作区域大小"（由 --size 参数控制），offset 是相对该区域的偏移，必须 < io_size，否则 I/O 会落到测试区域外。**

#### 三个文件大小概念

```
文件在磁盘上的布局：

  ┌──────────────────────────────────────────────┐
  │  file_offset    │← io_size →│                │
  │  (起始偏移)       │  I/O 区域  │                │
  │                  │           │                │
  0                  ↑           ↑                ↑
                  file_offset   file_offset      real_file_size
                                  +io_size

  file_offset    = I/O 区域起始位置（--offset 参数，默认 0）
  io_size        = I/O 区域大小（--size 参数）
  real_file_size = 文件在磁盘上的实际大小
```

#### 两层边界检查

```c
// io_u.c:553 — get_next_offset()
static int get_next_offset(struct thread_data *td, struct io_u *io_u, ...)
{
    // get_next_block() 生成"相对偏移"（应在 [0, io_size) 范围内）
    if (get_next_block(td, io_u, ddir, ...))
        return 1;

    // ★ 检查 1：相对偏移不能超过 io_size
    if (io_u->offset >= f->io_size) {
        return 1;  // 越界，重新生成
    }

    // 相对偏移 → 绝对偏移（加上 file_offset）
    io_u->offset += f->file_offset;

    // ★ 检查 2：绝对偏移不能超过 real_file_size
    if (io_u->offset >= f->real_file_size) {
        return 1;  // 越界，重新生成
    }
}
```

#### 为什么 offset 不能大于 io_size？

| I/O 模式 | offset 生成方式 | 超过 io_size 的含义 |
|---------|----------------|-------------------|
| **随机 I/O** | 随机数 ∈ [0, io_size) | 生成器越界 → I/O 写到测试区域外 |
| **顺序 I/O** | 每次递增 bs | 已遍历完整个区域 → 需回绕或停止 |

```
示例：--size=100M --offset=1G
  file_offset = 1G, io_size = 100M

  正确：随机 offset ∈ [0, 100M) → 绝对位置 ∈ [1G, 1.1G)
  错误：offset = 150M → 绝对位置 = 1.15G → 超出 I/O 区域！
  → 返回 1，fio 重新生成 offset
```

#### io_size 的设置（filesetup.c:1236）

```c
// --size=100M 且 2 个文件 → 每个文件 io_size = 50M
f->io_size = fs;  // size / nr_files

// io_size 不能超过 real_file_size
if (f->io_size > f->real_file_size)
    f->io_size = f->real_file_size;
```

**一句话总结：** `io_size` 是 I/O 工作区域大小，offset 是相对偏移。检查 `offset >= io_size` 防止随机/顺序生成器产生越界值，确保 I/O 只落在用户指定的测试区域内。

---

## Q11: fio 中的"文件"是什么含义？能否直接对裸设备读写？

**日期：** 2026-07-04  
**场景：** 不理解 fio 中 file/file_offset/io_size 的含义，疑惑是否经过文件系统  
**相关文件：** `docs/fio/reading/05_io_u_get.c`  
**源码位置：** `src/fio/file.h:20` — `enum fio_filetype`；`src/fio/filesetup.c:1720` — `get_file_type()`

### 回答

**fio 的"文件"是广义概念，支持普通文件、块设备、字符设备、管道。用 `--filename=/dev/vdb` 可以直接读写裸设备，完全绕过文件系统。**

#### 四种文件类型

```c
// file.h:20
enum fio_filetype {
    FIO_TYPE_FILE = 1,   /* 普通文件（经过文件系统） */
    FIO_TYPE_BLOCK,      /* ★ 块设备（裸设备，如 /dev/sdb） */
    FIO_TYPE_CHAR,       /* 字符设备（如 /dev/null） */
    FIO_TYPE_PIPE,       /* 管道（stdin "-"） */
};
```

#### 类型判断（get_file_type）

```c
// filesetup.c:1720
static void get_file_type(struct fio_file *f)
{
    struct stat sb;
    f->filetype = FIO_TYPE_FILE;  // 默认

    if (!stat(f->file_name, &sb)) {
        if (S_ISBLK(sb.st_mode))       // ★ 块设备
            f->filetype = FIO_TYPE_BLOCK;
        else if (S_ISCHR(sb.st_mode))  // 字符设备
            f->filetype = FIO_TYPE_CHAR;
        else if (S_ISFIFO(sb.st_mode)) // 管道
            f->filetype = FIO_TYPE_PIPE;
    }
}
```

#### 裸设备 vs 普通文件

```
普通文件 --filename=/tmp/testfile：
  open() → VFS → ext4 → block layer → 设备
  ★ 经过文件系统（inode/dentry/page cache/journal）

裸设备 --filename=/dev/vdb：
  open() → block layer → 设备
  ★★★ 完全绕过文件系统，直接按扇区读写
```

#### 块设备的特殊处理

```c
// filesetup.c:951 — 设备大小用 ioctl 获取，不用 stat
if (f->filetype == FIO_TYPE_BLOCK || f->filetype == FIO_TYPE_CHAR) {
    ret += f->real_file_size;  // continue，跳过文件系统检查
}

// filesetup.c:745 — 块设备不需要创建文件
if (f->filetype != FIO_TYPE_FILE)
    return 0;

// filesetup.c:1325 — 块设备不能 truncate（大小固定）
if (f->filetype == FIO_TYPE_FILE && ...)
```

#### 实际使用

```bash
# 1. 普通文件（经过 ext4）
fio --filename=/tmp/testfile --size=1G --ioengine=libaio --direct=1

# 2. ★ 裸设备（绕过文件系统）
fio --filename=/dev/vdb --ioengine=libaio --direct=1 --size=1G --offset=0

# 3. 裸设备 + 跳过前 1GB（保护分区表）
fio --filename=/dev/vdb --offset=1G --size=1G
```

#### 为什么裸设备测试更准确？

| 方式 | 路径 | 测的是什么 |
|------|------|----------|
| 普通文件 | fio → ext4 → block layer → 设备 | 文件系统 + 设备综合性能 |
| 裸设备 | fio → block layer → 设备 | ★ 纯设备性能（无 FS 开销） |

**一句话总结：** fio 的"文件"包含块设备。用 `--filename=/dev/vdb` 直接读写裸设备，绕过文件系统，测得纯存储性能。这是测试存储硬件最准确的方式。

---

## Q12: sync 引擎的 .prep 何时调用？能否通过多 job/多 qd 实现并发？

**日期：** 2026-07-04  
**场景：** 阅读 `06_engine_sync.c` 中 `.prep = fio_syncio_prep` 不理解调用时机，以及 sync 引擎的并发能力  
**相关文件：** `docs/fio/reading/06_engine_sync.c`  
**源码位置：** `src/fio/ioengines.c:271` — `td_io_prep()`；`src/fio/backend.c:2137`

### 回答

**.prep 在每个 io_u 提交（queue）前调用。sync 引擎可以通过 `--numjobs` 实现并发（多进程），但不能通过 `--iodepth` 实现并发（fio 会强制 qd=1）。**

#### .prep 的调用时机

```c
// ioengines.c:271
int td_io_prep(struct thread_data *td, struct io_u *io_u)
{
    if (td->io_ops->prep)
        return td->io_ops->prep(td, io_u);  // ★ 调用引擎的 prep
}

// backend.c:248 — 主循环中的顺序
td_io_prep(td, io_u);     // ★ 1. prep（准备）
td_io_queue(td, io_u);    //    2. queue（提交执行）
```

sync 引擎的 prep 做 lseek 定位：

```c
// sync.c:92
static int fio_syncio_prep(...)
{
    if (LAST_POS(f) == io_u->offset)
        return 0;                    // ★ 连续 I/O 跳过 lseek（优化）
    lseek(f->fd, io_u->offset, SEEK_SET);  // 否则 lseek 定位
}
```

→ prep 在 queue 前准备 I/O。sync 用 read/write（不带偏移），需先 lseek；psync 用 pread/pwrite（自带偏移），不需要 prep。

#### sync 引擎 + iodepth 的限制

```c
// backend.c:2137 — fio 启动时的警告！
if (FIO_SYNCIO && iodepth > 1 && io_submit_mode != IO_MODE_OFFLOAD) {
    log_info("note: both iodepth >= 1 and synchronous I/O engine "
             "are selected, queue depth will be capped at 1");
    //              ★ iodepth 被强制限制为 1！
}
```

**原因：sync 的 queue() 返回 FIO_Q_COMPLETED（阻塞完成）**

```c
// sync.c:220 — fio_syncio_queue()
ret = write(f->fd, ...);        // ★ 阻塞直到完成
return FIO_Q_COMPLETED;         // 直接返回"已完成"

// 对比 libaio：
io_submit(...);                 // 立即返回
return FIO_Q_QUEUED;            // 返回"已入队"，可继续提交
```

#### 三种并发方式对比

| 方式 | 机制 | sync 引擎 | libaio 引擎 |
|------|------|----------|------------|
| `--iodepth=32` | 单 job 多在飞 I/O | ✗ 无效（qd 被强制=1） | ★ 有效 |
| `--numjobs=4` | 多进程各自独立 I/O | ★ 有效 | ★ 有效 |
| `--iodepth=32 --numjobs=4` | 组合 | qd=1×4=4 并发 | ★ 32×4=128 并发 |

```
sync + numjobs=4：
  Job1: write()阻塞  ├─ Job2: write()阻塞
                     ├─ Job3: write()阻塞
                     └─ Job4: write()阻塞
  → 4 个并发 I/O（每个 qd=1）

libaio + iodepth=32 + numjobs=4：
  Job1: 32个异步在飞  ├─ Job2: 32个异步在飞
                      ├─ Job3: 32个异步在飞
                      └─ Job4: 32个异步在飞
  → 128 个并发 I/O
```

#### 实际测试

```bash
# sync + iodepth=32 → 警告，实际 qd=1
fio --ioengine=sync --iodepth=32 --filename=/dev/vdb
# 输出：queue depth will be capped at 1

# ★ sync + numjobs=4 → 4 并发（有效）
fio --ioengine=sync --numjobs=4 --filename=/dev/vdb

# libaio + iodepth=32 → 32 并发（有效）
fio --ioengine=libaio --iodepth=32 --direct=1 --filename=/dev/vdb
```

**一句话总结：** `.prep` 在 queue 前调用（sync 做 lseek）。sync 引擎用 `--numjobs` 实现并发（多进程），不能用 `--iodepth`（queue 阻塞返回 COMPLETED，fio 强制 qd=1）。

---

## Q13: fio 的线程模式是什么？默认是什么？为什么 SPDK 需要 --thread？

**日期：** 2026-07-04  
**场景：** 阅读 `08_engine_spdk.md` 中 SPDK 要求 `--thread`，不理解线程模式  
**相关文件：** `docs/fio/reading/08_engine_spdk.md`  
**源码位置：** `src/fio/options.c` — `--thread` 选项；`src/fio/backend.c:2802`

### 回答

**fio 默认是进程模式（fork），`--thread` 切换为线程模式（pthread_create）。SPDK 依赖线程局部存储（TLS），所以必须用 `--thread`。**

#### 选项定义与默认值

```c
// options.c — --thread 选项
{
    .name = "thread",
    .type = FIO_OPT_STR_SET,           // STR_SET = 默认 0（关闭）
    .help = "Use threads instead of processes",
#ifdef CONFIG_NO_SHM
    .def = "1",                        // 只有无共享内存时默认 1
#endif
}
// ★ Linux 上默认 use_thread=0（进程模式）
```

#### 两种创建方式

```c
// backend.c:2802
if (td->o.use_thread) {
    pthread_create(&td->thread, NULL, thread_main, fd);  // ★ 线程
} else {
    pid = fork();                                         // ★ 进程
    if (!pid) thread_main(fd);
}

// backend.c:1964 — thread_main 中设置 PID
if (!o->use_thread)
    td->pid = getpid();    // 进程模式：独立 PID
else
    td->pid = gettid();    // 线程模式：线程 ID
```

#### 进程模式 vs 线程模式

```
进程模式（默认）：
  ┌─ 进程1 (PID=1001) ─┐  ┌─ 进程2 (PID=1002) ─┐
  │ 独立地址空间         │  │ 独立地址空间         │
  │ 独立 fd 表          │  │ 独立 fd 表          │
  │ 独立内存            │  │ 独立内存            │
  └────────────────────┘  └────────────────────┘
  ★ 内存隔离，更安全；共享需 mmap(MAP_SHARED)

线程模式（--thread）：
  ┌─ 进程 (PID=2000) ─────────────────────┐
  │ ┌─ 线程1 (TID=2000) ─┐                │
  │ │ 共享地址空间         │                │
  │ ├─ 线程2 (TID=2001) ─┤  ← 共享一切    │
  │ │ 共享 fd/内存        │                │
  │ └────────────────────┘                │
  └────────────────────────────────────────┘
  ★ 共享地址空间、fd、内存；支持 TLS
```

#### 为什么 SPDK 需要 --thread

```
SPDK（Storage Performance Development Kit）使用线程局部存储（TLS）：
  - SPDK 的 per-thread 状态（I/O 通道、poller）存储在 TLS 中
  - fork() 后子进程不继承父进程的 TLS 初始化状态
  - pthread_create() 会正确初始化 TLS

  → SPDK 的 bdev 层、NVMe 驱动假设运行在线程中
  → 进程模式（fork）下 TLS 数据丢失 → 崩溃
  → 必须用 --thread
```

#### 性能对比

| 特性 | 进程模式（默认） | 线程模式 |
|------|----------------|---------|
| 创建开销 | 高（fork 复制页表） | 低（pthread_create） |
| 内存共享 | 否 | 是 |
| TLS 支持 | ✗ | ★ 是（SPDK 需要） |
| 崩溃影响 | 独立 | 整个进程退出 |

#### 实际使用

```bash
# 默认进程模式
fio --name=test --numjobs=4 --filename=/dev/vdb    # 4 个独立进程

# 线程模式
fio --name=test --numjobs=4 --thread --filename=/dev/vdb    # 4 个线程

# SPDK 必须 --thread
fio --ioengine=spdk_bdev --thread --filename=Bdev0 ...
```

**一句话总结：** fio 默认进程模式（fork），`--thread` 切换线程模式。SPDK 依赖 TLS（线程局部存储），fork 不继承 TLS 初始化，所以必须用 `--thread`。

---

## Q14: 为什么 fio 默认进程模式？线程模式看起来没坏处

**日期：** 2026-07-04  
**场景：** 理解了线程/进程模式后，疑惑为什么默认用进程模式  
**相关文件：** `docs/fio/reading/08_engine_spdk.md`  
**源码位置：** `src/fio/init.c:651`、`src/fio/smalloc.c`、`src/fio/backend.c:1988`

### 回答

**不只是崩溃隔离。进程模式默认的原因：verify 内存隔离、PSHARED 共享内存架构、库线程安全性低要求、信号处理简单。**

#### 原因 1：崩溃隔离

```
进程模式：Job1 崩溃 → 只影响 Job1，其他继续
线程模式：Job1 线程崩溃 → 整个进程退出 → 所有结果丢失
```

#### 原因 2：★ verify 模式的内存损坏隔离（核心原因）

```
verify 模式验证数据完整性：
  - 如果某 job buffer overflow 损坏了自己的内存
  - 进程模式：只损坏自己，其他 job 验证不受影响
  - 线程模式：损坏共享地址空间 → 其他 job 也出错 → 误报
  ★ 这是 verify 测试选进程模式的核心原因
```

#### 原因 3：进程间共享互斥锁（源码证据）

```c
// init.c:651 — 进程模式是首选，线程模式是 fallback
#ifndef CONFIG_PSHARED    // 不支持进程间共享互斥锁
    if (!o->use_thread) {
        log_info("fio: this platform does not support process shared "
                 "mutexes, forcing use of threads.\n");
        o->use_thread = 1;    // ★ 被迫用线程模式
    }
#endif
```

fio 用 PTHREAD_PROCESS_SHARED 实现跨进程同步：

```c
// backend.c:1988
mutex_cond_init_pshared(&td->io_u_lock, &td->free_cond);
//   ↑ pshared = 进程间共享，用 mmap(MAP_SHARED) + pthread_mutexattr_setpshared
```

#### 原因 4：smalloc 共享内存架构

```c
// smalloc.c — 共享内存分配器
ptr = mmap(NULL, alloc_size, PROT_READ|PROT_WRITE,
           OS_MAP_ANON | MAP_SHARED, -1, 0);
//                              ↑ 进程间共享
```

```
进程模式架构：
  ┌─进程1─┐ ┌─进程2─┐ ┌─进程3─┐
  │私有内存│ │私有内存│ │私有内存│
  └───┬───┘ └───┬───┘ └───┬───┘
      └─────────┼─────────┘
            ┌────┴─────┐
            │mmap共享区│ ← smalloc 分配的全局状态
            │(统计/配置)│   进程间通过共享内存通信
            └──────────┘
  ★ 天然隔离 + 可控共享
```

#### 原因 5：I/O 库线程安全性

```
不是所有引擎/库都线程安全：
  进程模式：fork 出独立状态 → 无竞争
  线程模式：共享库状态 → 可能竞争/崩溃
  ★ 某些引擎必须 --thread（如 SPDK 用 TLS 解决线程安全）
```

#### 原因 6：信号处理简单

```
进程模式：每个进程独立信号处理 → SIGINT 干净退出
线程模式：信号发给哪个线程？复杂
```

#### 完整对比

| 因素 | 进程模式（默认） | 线程模式 |
|------|----------------|---------|
| 崩溃隔离 | ★ 是 | ✗ |
| 内存损坏隔离 | ★ verify 关键 | ✗ |
| 库线程安全要求 | ★ 不要求 | 要求 |
| 信号处理 | ★ 简单 | 复杂 |
| PSHARED 依赖 | ★ 需要 | 不需要 |
| TLS 支持 | ✗ | ★ 有（SPDK） |
| 创建开销 | 高 | 低 |

**一句话总结：** fio 默认进程模式的原因：崩溃隔离、verify 内存损坏隔离、PSHARED 共享内存 IPC 架构、库线程安全性要求低、信号处理简单。线程模式是 PSHARED 不可用或引擎需 TLS（SPDK）时的选择。

---

## Q15: 如何用 IO depth 分布诊断瓶颈？

**日期：** 2026-07-04  
**场景：** 阅读 fio 输出中的 IO depths / submit / complete 分布，想用它判断瓶颈是否合理  
**相关文件：** `docs/fio/reading/09_stat_output.md`  
**源码位置：** `src/fio/io_u.c:1188-1224`、`src/fio/stat.c:1825`

### 回答

**三种分布分别看提交效率、实际并发度、收割效率，组合判断瓶颈在设备还是 CPU。**

#### 三种分布的源码

```c
// io_u.c:1200 — IO depths：记录采样时的 cur_depth（在飞数）
void io_u_mark_depth(struct thread_data *td, unsigned int nr)
{
    switch (td->cur_depth) {     // ★ 当前在飞 I/O 数
    case 1:         idx = 0;    // 桶：1
    case 2 ... 3:   idx = 1;    // 桶：2-3
    case 4 ... 7:   idx = 2;    // 桶：4-7
    case 8 ... 15:  idx = 3;    // 桶：8-15
    case 16 ... 31: idx = 4;    // 桶：16-31
    case 32 ... 63: idx = 5;    // 桶：32-63
    default:        idx = 6;    // 桶：>=64
    }
    td->ts.io_u_depth[idx]++;
}

// io_u.c:1188 — submit：每次 commit 提交了几个 I/O
void io_u_mark_submit(struct thread_data *td, unsigned int nr) {
    __io_u_mark_map(td->ts.io_u_submit, nr);  // nr = 本次提交数
}

// io_u.c:1194 — complete：每次收割了几个完成事件
void io_u_mark_complete(struct thread_data *td, unsigned int nr) {
    __io_u_mark_map(td->ts.io_u_complete, nr);  // nr = 本次收割数
}
```

#### 三种分布的诊断维度

```
  提交侧              在飞                收割侧
┌──────────┐    ┌──────────┐    ┌──────────┐
│ submit   │→→→│ IO depths│→→→│ complete │
│ 每次     │    │ 运行期间 │    │ 每次     │
│ commit   │    │ 在飞几个 │    │ reap     │
│ 提交几个 │    │          │    │ 收割几个 │
└──────────┘    └──────────┘    └──────────┘
   提交效率        实际并发度       收割效率
```

#### 瓶颈诊断矩阵（5 种场景）

**场景 1：理想（均衡）**
```
IO depths : 32=99.8%
submit    : 32=100%
complete  : 32=100%
→ iodepth 充分利用，设备与提交匹配
```

**场景 2：★ 设备瓶颈（队列满）**
```
IO depths : 32=99.5%       ← 队列总是满的 → 设备是瓶颈
submit    : 32=100%        ← 提交很快（CPU 不是瓶颈）
complete  : 4=30%,8=40%    ← 分散，设备完成不均匀
→ 优化：增大 iodepth 或换更快设备
```

**场景 3：★ 提交瓶颈（队列不满）**
```
IO depths : 1=60%,2=20%    ← 大部分时间只有 1 个在飞 → 提交跟不上
submit    : 0=50%          ← 50% 时没有 I/O 可提交！
complete  : 4=90%          ← 设备很快完成
→ 瓶颈在 CPU/应用（offset 计算慢/单线程提交）
→ 优化：检查 random_generator、调 iodepth_batch_submit、--numjobs
```

**场景 4：★ 收割瓶颈**
```
IO depths : 32=95%         ← 队列满
submit    : 32=100%        ← 提交正常
complete  : 0=80%          ← ★ 80% 收割了 0 个！io_getevents 超时
→ 收割循环空转
→ 优化：调 iodepth_batch_complete 参数
```

**场景 5：iodepth 过大**
```
IO depths : >=64=100%      ← 全在 64+ 桶
IOPS: iodepth=64 和 128 几乎相同 ← 已饱和
延迟: iodepth=128 比 64 翻倍 ← 排队效应
→ 优化：降到 IOPS 拐点
```

#### 实战诊断流程

```
Step 1: 看 IO depths
  ├─ 高深度占 >90% → 队列满 → 设备瓶颈 → 看 complete
  └─ 低深度占 >50% → 队列不满 → 提交瓶颈 → 看 submit

Step 2a（设备瓶颈）：complete 分散 = 正常
Step 2b（提交瓶颈）：submit 有 0 = 应用慢

Step 3: 对比不同 iodepth 的 IOPS 找拐点
  iodepth=1:  50K, 0.02ms
  iodepth=4:  150K, 0.03ms  ← 线性
  iodepth=16: 300K, 0.05ms  ← 放缓
  iodepth=32: 320K, 0.10ms  ← ★ 拐点
  iodepth=64: 325K, 0.20ms  ← 过了拐点
  → 最佳 iodepth = 32
```

#### 速查表

| 分布 | 高深度/大数 | 低深度/有 0 | 含义 |
|------|------------|------------|------|
| IO depths | 队列满→设备瓶颈 | 队列空→提交瓶颈 | 实际并发度 |
| submit | 提交正常 | 有 0→应用慢 | 提交效率 |
| complete | 收割正常 | 有 0→收割不及时 | 收割效率 |

**一句话总结：** IO depths 看实际并发度（高=设备瓶颈，低=提交瓶颈），submit 看提交效率（有 0=应用慢），complete 看收割效率（有 0=收割不及时）。三者组合精确定位瓶颈在设备还是 CPU。

---

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
