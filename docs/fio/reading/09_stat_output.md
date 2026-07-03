# fio 输出解读指南

> 读懂 fio 输出是性能分析的基础。本文逐行解读 fio 输出的每个字段，
> 重点解释 slat/clat/lat 的含义和计算方式，以及它们与 blktrace 指标的对应关系。

---

## 一、fio 输出指标与 blktrace 的对应关系

fio 输出的延迟指标和 blktrace/btt 的延迟指标从不同视角测量同一个 I/O：

```
fio 视角：

  start_time              issue_time             complete_time
      │                       │                       │
      ▼                       ▼                       ▼
  ┌──────────┐           ┌──────────┐           ┌──────────┐
  │ get_io_u │           │ engine   │           │ engine   │
  │ (用户态) │           │ submit   │           │ complete │
  └──────────┘           └──────────┘           └──────────┘

  │←── slat ──→│←─────── clat ───────→│
  │←──────────── lat ─────────────────→│


blktrace 视角：

  Q (Queue)           D (Issue/Dispatch)       C (Complete)
      │                       │                       │
      ▼                       ▼                       ▼
  ┌──────────┐           ┌──────────┐           ┌──────────┐
  │ bio 入队 │           │ 下发设备 │           │ 设备完成 │
  │ (内核)   │           │ 驱动     │           │ 中断     │
  └──────────┘           └──────────┘           └──────────┘

  │←── Q2D ──→│←─────── D2C ────────→│
  │←──────────── Q2C ────────────────→│
```

### 对应表

| fio 指标 | 含义 | 计算方式 | 对应 blktrace |
|---------|------|---------|--------------|
| **slat** | 提交延迟 (submit latency) | issue_time − start_time | Q → D（软件层延迟） |
| **clat** | 完成延迟 (completion latency) | complete_time − issue_time | D → C（设备延迟） |
| **lat** | 总延迟 (total latency) | complete_time − start_time | Q → C（端到端延迟） |
| **bw** | 带宽 (bandwidth) | bytes / runtime | — |
| **iops** | 每秒 I/O 数 | ios / runtime | — |

### 关键区别

- **fio slat ≠ blktrace Q2D**：
  fio 的 slat 测量的是用户态从 `get_io_u()` 到引擎 `commit()` 的时间。
  blktrace 的 Q2D 测量的是内核中从 bio 入队到下发设备驱动的时间。
  fio 的 slat 包含了系统调用开销，而 Q2D 只包含内核内部调度开销。

- **fio clat ≈ blktrace D2C**：
  两者都测量"设备处理时间"，但 fio 的 clat 还包括了内核→用户态的完成通知延迟。

---

## 二、完整 fio 输出逐行解读

以下是一份真实的 fio 输出，每行都标注了含义。

### 2.1 测试命令

```bash
fio --name=test \
    --ioengine=libaio \
    --direct=1 \
    --rw=randwrite \
    --bs=4k \
    --iodepth=32 \
    --numjobs=1 \
    --filename=/dev/vdb \
    --runtime=10 \
    --time_based
```

### 2.2 完整输出

```
test: (g=0): rw=randwrite, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=libaio, iodepth=32
fio-3.37
Starting 1 process

test: (groupid=0, jobs=1): err= 0: pid=12345: Thu Jul  3 10:00:00 2026
  write: IOPS=5123, BW=20.0MiB/s (20.0MB/s)(200MiB/10001msec); 0 zone resets
    slat (nsec): min=1234, max=98765, avg=2345.67, stdev=1234.56
    clat (usec): min=45, max=2345, avg=123.45, stdev=45.67
     lat (usec): min=47, max=2356, avg=125.80, stdev=46.12
    clat percentiles (usec):
     |  1.00th=[   56],  5.00th=[   67], 10.00th=[   78], 20.00th=[   89],
     | 30.00th=[   98], 40.00th=[  107], 50.00th=[  117], 60.00th=[  126],
     | 70.00th=[  136], 80.00th=[  150], 90.00th=[  177], 95.00th=[  205],
     | 99.00th=[  285], 99.50th=[  338], 99.90th=[  523], 99.95th=[  676],
     | 99.99th=[ 1352]
   bw (  KiB/s): min=19456, max=21504, per=100.00%, avg=20480.00, stdev=512.00, samples=20
   iops        : min= 4864, max= 5376, per=100.00%, avg=5120.00, stdev=128.00, samples=20
  lat (usec)   : 50=0.23%, 100=34.56%, 250=63.21%, 500=1.89%, 750=0.08%
  lat (usec)   : 1000=0.02%, 2000=0.01%
  cpu          : usr=2.34%, sys=5.67%, ctx=51234, majf=0, minf=12
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=99.5%, >=64=0.0%
     submit    : 0=0.0%, 4=0.0%, 8=0.0%, 16=0.0%, 32=100.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=0.0%, 8=0.0%, 16=0.0%, 32=100.0%, 64=0.0%, >=64=0.0%
     issued rwt: total=0,51234,0
     completed : total=0,51234,0
  Latency (ms) :
       sync : min=    0, max=    0, avg= 0.00, stdev= 0.00
     lat_percentiles: 1

Run status group 0 (all jobs):
  WRITE: bw=20.0MiB/s (20.0MB/s), 20.0MiB/s-20.0MiB/s (20.0MB/s-20.0MB/s), io=200MiB (210MB), run=10001-10001msec
```

### 2.3 逐行注解

#### 头部信息

```
test: (g=0): rw=randwrite, bs=(R) 4096B-4096B, (W) 4096B-4096B, ...
```
- `g=0` — group ID
- `rw=randwrite` — I/O 模式
- `bs=(R) 4096B-4096B` — 读操作的块大小范围（min-max）
- `(W) 4096B-4096B` — 写操作的块大小范围
- `ioengine=libaio` — 使用的 I/O 引擎
- `iodepth=32` — 队列深度

#### 性能概要

```
  write: IOPS=5123, BW=20.0MiB/s (20.0MB/s)(200MiB/10001msec)
```
- `IOPS=5123` — ★ 每秒 I/O 操作数 = 总 IO 数 / 运行时间
- `BW=20.0MiB/s` — ★ 带宽（MiB = 1024×1024，MB = 1000×1000）
- `(200MiB/10001msec)` — 总传输量 / 总运行时间

**IOPS 和 BW 的计算：**
```
IOPS = 完成的 IO 数 / 运行秒数 = 51234 / 10.001 ≈ 5123
BW   = 传输字节数 / 运行秒数   = 200MiB / 10.001s ≈ 20.0MiB/s
```

#### slat（提交延迟）

```
    slat (nsec): min=1234, max=98765, avg=2345.67, stdev=1234.56
```
- ★ **slat = issue_time − start_time**
- 含义：从 fio 准备好 io_u（get_io_u 完成）到引擎真正提交（io_submit）的时间
- 单位：这里是纳秒（nsec），通常很小（1-5μs）
- 包含：io_prep + 引擎 queue() + commit() 的开销
- 对应 blktrace：大致对应 Q→D 中的"软件调度延迟"

**为什么 slat 很小？** 对于 libaio，queue() 只是把 iocb 放入环缓冲，commit() 调用 io_submit()。这些操作都在用户态完成，不涉及设备 I/O。

#### clat（完成延迟）

```
    clat (usec): min=45, max=2345, avg=123.45, stdev=45.67
```
- ★ **clat = complete_time − issue_time**
- 含义：从引擎提交 I/O 到设备完成的时间
- 单位：微秒（usec）
- 包含：内核 block layer 处理 + 设备驱动 + 设备物理处理 + 中断/轮询通知
- 对应 blktrace：大致对应 D→C
- ★ **这是衡量设备性能的核心指标**

#### lat（总延迟）

```
     lat (usec): min=47, max=2356, avg=125.80, stdev=46.12
```
- ★ **lat = complete_time − start_time = slat + clat**
- 含义：从 fio 准备 io_u 到 I/O 完成的端到端延迟
- 对应 blktrace：Q→C（整个 I/O 生命周期）

#### 百分位延迟（Percentiles）

```
    clat percentiles (usec):
     |  1.00th=[   56],  5.00th=[   67], 10.00th=[   78], ...
     | 50.00th=[  117],  ...  99.00th=[  285], 99.90th=[  523],
     | 99.99th=[ 1352]
```
- ★ **百分位延迟表示：X% 的 I/O 延迟低于此值**
- `50.00th=[117]` — 中位数延迟 117μs（一半 IO 快于一半 IO 慢于这个值）
- `99.00th=[285]` — P99 延迟 285μs（99% 的 IO 快于 285μs）
- `99.99th=[1352]` — P99.99 延迟 1352μs（万分之一最慢的 IO）

**为什么百分位很重要？** 平均值可能被少数极端值拉高或拉低。
P99/P99.9 更能反映"用户感知到的延迟"——
如果一个服务每秒处理 10000 个请求，P99=285μs 意味着每 100ms 内最多有 1 个请求超过 285μs。

#### bw/iops 采样

```
   bw (  KiB/s): min=19456, max=21504, per=100.00%, avg=20480.00, stdev=512.00, samples=20
   iops        : min= 4864, max= 5376, per=100.00%, avg=5120.00, stdev=128.00, samples=20
```
- fio 每隔一段时间（默认 500ms）采样一次 bw/iops
- `min/max` — 采样期间的最小/最大值
- `avg/stdev` — 平均值和标准差
- `samples=20` — 共采样 20 次（10 秒 / 0.5 秒 = 20）
- `per=100.00%` — 该 job 占 group 的百分比

#### 延迟分布直方图

```
  lat (usec)   : 50=0.23%, 100=34.56%, 250=63.21%, 500=1.89%, ...
  lat (usec)   : 1000=0.02%, 2000=0.01%
```
- `50=0.23%` — 0.23% 的 I/O 延迟 < 50μs
- `100=34.56%` — 34.56% 的 I/O 延迟在 50-100μs 之间
- `250=63.21%` — 63.21% 的 I/O 延迟在 100-250μs 之间
- 这些百分数加起来 = 100%

#### CPU 使用率

```
  cpu          : usr=2.34%, sys=5.67%, ctx=51234, majf=0, minf=12
```
- `usr` — 用户态 CPU 使用率
- `sys` — 内核态 CPU 使用率（系统调用、中断处理等）
- `ctx` — 上下文切换次数
- `majf` — 主缺页错误数（需要从磁盘加载页面）
- `minf` — 次缺页错误数（需要从其他来源加载页面）

#### IO 深度分布

```
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=99.5%, >=64=0.0%
```
- ★ 表示运行期间实际队列深度的分布
- `32=99.5%` — 99.5% 的时间队列深度为 32（说明 iodepth=32 被充分利用）
- 如果大部分时间在低深度，说明设备处理太快，提交跟不上

#### submit/complete 深度分布

```
     submit    : 0=0.0%, 4=0.0%, 8=0.0%, 16=0.0%, 32=100.0%, ...
     complete  : 0=0.0%, 4=0.0%, 8=0.0%, 16=0.0%, 32=100.0%, ...
```
- `submit: 32=100.0%` — 每次 commit 都提交 32 个 I/O
- `complete: 32=100.0%` — 每次 getevents 都收割 32 个完成事件
- 这说明 iodepth_batch_complete 设置合理

---

## 三、slat/clat/lat 的计算方式（源码视角）

### 3.1 时间戳的记录点

```
源码位置                              记录的时间戳
─────────────────────────────────────────────────────
io_u.c: get_io_u()                   io_u->start_time = now
  → td_io_prep()
  → 返回 io_u

ioengines.c: td_io_queue()           (引擎 queue 前)
  → engine->queue()

libaio.c: fio_libaio_commit()        io_u->issue_time = now
  → io_submit()                        (在 fio_libaio_queued() 中设置)
  → fio_libaio_queued()
      → memcpy(&io_u->issue_time, &now, sizeof(now))

io_u.c: io_u_queued_complete()       complete_time = now
  → 或 getevents() 返回后
```

### 3.2 延迟计算公式

```
slat = issue_time − start_time
     = (io_submit 时刻) − (get_io_u 完成时刻)
     ≈ io_prep + queue 入环 + commit 调用的开销

clat = complete_time − issue_time
     = (io_getevents 返回时刻) − (io_submit 时刻)
     ≈ 内核处理 + 设备处理 + 完成通知

lat  = complete_time − start_time
     = slat + clat
     = 端到端延迟
```

### 3.3 统计数据结构

fio 内部使用 `struct io_stat` 记录每个方向的延迟统计：

```c
struct io_stat {
    uint64_t max_val;      // 最大值
    uint64_t min_val;      // 最小值
    uint64_t samples;      // 采样数
    double mean;           // 平均值
    double S;              // 用于计算标准差的中间量
    // ... 百分位桶（bucket） ...
};
```

fio 为每个 job 维护 4 个 `io_stat`：
- `slat_stat[DDIR_READ]` / `slat_stat[DDIR_WRITE]`
- `clat_stat[DDIR_READ]` / `clat_stat[DDIR_WRITE]`
- `lat_stat[DDIR_READ]` / `lat_stat[DDIR_WRITE]`
- `bw_stat` / `iops_stat`

---

## 四、实战对比：fio 输出 vs blktrace 输出

### 4.1 同时运行 fio + blktrace

```bash
# 终端 1：blktrace
blktrace -d /dev/vdb -o fio_trace -w 15 &
sleep 1

# 终端 2：fio
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --iodepth=16 --filename=/dev/vdb --runtime=10 --time_based
wait

# 解析 blktrace
blkparse -i fio_trace -o fio_parsed.txt
btt -i fio_trace.bin -o fio_btt
```

### 4.2 对比结果

```
fio 输出：
  write: IOPS=4800, slat avg=2.1μs, clat avg=180μs, lat avg=182μs

btt 输出：
  ALL        Q2D       D2C       Q2C
  avg     5.2μs    175.3μs    180.5μs
```

### 4.3 差异解释

| 指标 | fio | btt | 差异原因 |
|------|-----|-----|---------|
| slat / Q2D | 2.1μs | 5.2μs | fio slat 不含内核调度，btt Q2D 含内核调度 |
| clat / D2C | 180μs | 175.3μs | fio clat 含完成通知延迟，btt D2C 不含 |
| lat / Q2C | 182μs | 180.5μs | 两者基本一致（端到端延迟） |

**结论：** fio 的 lat 和 btt 的 Q2C 最接近，因为都是端到端延迟。
slat vs Q2D 和 clat vs D2C 有差异是正常的——它们测量的是不同阶段。

---

## 五、常见问题

### Q1: 为什么 clat 远大于 slat？

因为 clat 包含了设备实际处理时间（磁盘寻道、NAND 编程等），而 slat 只是用户态的提交开销。对于 NVMe SSD，clat 通常 20-100μs；对于 HDD，clat 可能 5-15ms。

### Q2: iodepth 越大，IOPS 一定越高吗？

不一定。iodepth 增加会提高 IOPS（更多的并发 I/O），但也会增加延迟（设备队列更深）。存在一个"拐点"——超过后 IOPS 不再增长但延迟显著增加。这个拐点取决于设备的队列处理能力。

### Q3: P99 和 P99.9 差距很大说明什么？

说明延迟分布有"长尾"——少数 I/O 的延迟远高于大多数。常见原因：
- GC（垃圾回收）干扰
- 设备内部队列管理
- CPU 调度抖动
- 中断风暴

### Q4: 如何让 fio 输出更详细？

```bash
fio ... --output-format=json+    # JSON 格式（便于脚本解析）
fio ... --lat_percentiles=1      # 输出 lat 的百分位（而不仅是 clat）
fio ... --log_avg_msec=100       # 每 100ms 采样一次 bw/iops/lat 日志
fio ... --write_bw_log=bw        # 写入 bw 日志文件
fio ... --write_lat_log=lat      # 写入 lat 日志文件
```
