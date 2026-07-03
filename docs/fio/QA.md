# fio 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: io_u 中 acct_ddir 的作用是什么？为什么和 ddir 分开？](#q1-io_u-中-acct_ddir-的作用是什么为什么和-ddir-分开)
- [Q2: verify_offset 和 offset 有什么区别？验证偏移的作用是什么？](#q2-verify_offset-和-offset-有什么区别验证偏移的作用是什么)
- [Q3: fio 如何维持 iodepth？保证在途 I/O 数目的机制是什么？](#q3-fio-如何维持-iodepth保证在途-io-数目的机制是什么)
- [Q4: rate_iops 限速机制如何工作？](#q4-rate_iops-限速机制如何工作)

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

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
