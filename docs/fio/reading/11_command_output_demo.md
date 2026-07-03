# fio 命令输出实战解析

> 每个命令都实际执行，展示真实输出并逐行注解。
> 设备：/dev/vdb (40G virtio-blk)，fio 版本：fio-3.34

---

## 场景 1：基础随机写输出解读

### 命令

```bash
fio --name=randwrite --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/vdb --runtime=10 --time_based \
    --iodepth=32
```

### 完整输出

```
randwrite: (g=0): rw=randwrite, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=libaio, iodepth=32
fio-3.34
Starting 1 process

randwrite: (groupid=0, jobs=1): err= 0: pid=379459: Fri Jul  3 08:21:58 2026
  write: IOPS=17.6k, BW=68.9MiB/s (72.3MB/s)(690MiB/10008msec); 0 zone resets
    slat (nsec): min=1546, max=2748.2k, avg=10950.07, stdev=18399.31
    clat (usec): min=128, max=10852, avg=1801.11, stdev=2899.95
     lat (usec): min=165, max=10859, avg=1812.06, stdev=2899.65
    clat percentiles (usec):
     |  1.00th=[  302],  5.00th=[  343], 10.00th=[  367], 20.00th=[  396],
     | 30.00th=[  416], 40.00th=[  437], 50.00th=[  457], 60.00th=[  486],
     | 70.00th=[  537], 80.00th=[  783], 90.00th=[ 8160], 95.00th=[ 8291],
     | 99.00th=[ 8586], 99.50th=[ 8848], 99.90th=[ 9503], 99.95th=[ 9765],
     | 99.99th=[10421]
   bw (  KiB/s): min=63888, max=196752, per=100.00%, avg=70638.80, stdev=29684.12, samples=20
   iops        : min=15972, max=49188, avg=17659.70, stdev=7421.03, samples=20
  lat (usec)   : 250=0.12%, 500=63.66%, 750=15.81%, 1000=1.43%
  lat (msec)   : 2=1.13%, 4=0.41%, 10=17.41%, 20=0.03%
  cpu          : usr=3.75%, sys=7.45%, ctx=180267, majf=0, minf=11
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=100.0%, >=64=0.0%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.1%, 64=0.0%, >=64=0.0%
     issued rwts: total=0,176629,0,0 short=0,0,0,0 dropped=0,0,0,0
     latency   : target=0, window=0, percentile=100.00%, depth=32

Run status group 0 (all jobs):
  WRITE: bw=68.9MiB/s (72.3MB/s), 68.9MiB/s-68.9MiB/s (72.3MB/s-72.3MB/s), io=690MiB (723MB), run=10008-10008msec

Disk stats (read/write):
  vdb: ios=52/174837, merge=0/0, ticks=18/292441, in_queue=292459, util=98.75%
```

### 逐行注解

#### 标题行

```
randwrite: (g=0): rw=randwrite, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=libaio, iodepth=32
```

| 字段 | 含义 |
|------|------|
| `randwrite` | job 名称 |
| `(g=0)` | group ID = 0 |
| `rw=randwrite` | 随机写模式 |
| `bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B` | 读/写/trim 的块大小范围 |
| `ioengine=libaio` | 使用 Linux 异步 I/O 引擎 |
| `iodepth=32` | 最大 32 个在飞 I/O |

#### 结果摘要

```
randwrite: (groupid=0, jobs=1): err= 0: pid=379459: Fri Jul  3 08:21:58 2026
```

| 字段 | 含义 |
|------|------|
| `groupid=0` | 该 job 属于第 0 组 |
| `jobs=1` | 组内有 1 个 job |
| `err= 0` | 无错误 |
| `pid=379459` | 进程 ID |

#### IOPS 与带宽

```
  write: IOPS=17.6k, BW=68.9MiB/s (72.3MB/s)(690MiB/10008msec); 0 zone resets
```

```
IOPS=17.6k                    → 每秒完成 17,600 次 I/O 操作
BW=68.9MiB/s (72.3MB/s)       → 带宽：二进制单位 68.9 MiB/s，十进制单位 72.3 MB/s
690MiB                         → 总 I/O 数据量
10008msec                      → 实际运行时间（≈10 秒）
0 zone resets                  → ZBD 设备的 zone reset 次数（普通设备为 0）
```

**计算验证：** IOPS × bs = 17600 × 4096 = 72,089,600 B/s ≈ 68.8 MiB/s ✓

#### ★ slat / clat / lat 延迟三项

```
    slat (nsec): min=1546, max=2748.2k, avg=10950.07, stdev=18399.31
    clat (usec): min=128, max=10852, avg=1801.11, stdev=2899.95
     lat (usec): min=165, max=10859, avg=1812.06, stdev=2899.65
```

```
slat（Submission Latency，提交延迟）：
  min=1546 nsec  → 最快提交 1.5μs
  max=2748.2k ns → 最慢提交 2.75ms ★ 有极端值
  avg=10950 nsec → 平均 10.95μs
  stdev=18399    → 标准差很大（说明提交时间不稳定）

  ★ slat = fio 调用 io_submit() 到内核接受请求的时间
  ★ 对应 blktrace 中的 Q→D 阶段（大致）
  ★ 单位是 nsec（纳秒），因为 libaio 提交非常快

clat（Completion Latency，完成延迟）：
  min=128 μsec   → 最快完成 128μs
  max=10852 μsec → 最慢完成 10.85ms
  avg=1801 μsec  → 平均完成 1.8ms
  stdev=2899 μs  → 标准差 ≈ 2.9ms（长尾延迟严重）

  ★ clat = 内核接受请求到 I/O 完成的时间
  ★ 对应 blktrace 中的 D→C 阶段（大致）
  ★ 单位是 usec（微秒），因为设备处理时间远长于提交

lat（Total Latency，总延迟）：
  min=165 μsec   → 最快总延迟 165μs
  max=10859 μsec → 最慢总延迟 10.86ms
  avg=1812 μsec  → 平均总延迟 1.81ms

  ★ lat = slat + clat = 端到端延迟
  ★ 对应 blktrace 中的 Q→C 阶段（大致）
  ★ 验证：avg 10950ns + 1801μs ≈ 1812μs ✓
```

**三者关系：**

```
fio 的延迟模型：

  ┌──────────┐    slat     ┌──────────┐    clat     ┌──────────┐
  │ fio 用户态 │ ────────→ │  内核调度  │ ────────→ │  设备处理  │
  │ 提交 I/O  │  (~11μs)  │  + 驱动   │  (~1.8ms) │  完成 I/O │
  └──────────┘            └──────────┘            └──────────┘
       │                                               │
       │←──────────── lat (~1.81ms) ──────────────────→│

对应 blktrace：
  Q ──────→ G ──────→ I ──────→ D ──────────→ C
  │← slat≈Q2D →│     │← clat≈D2C →│
  │←────── lat≈Q2C ──────→│
```

#### 百分位延迟

```
    clat percentiles (usec):
     |  1.00th=[  302],  5.00th=[  343], 10.00th=[  367], 20.00th=[  396],
     | 30.00th=[  416], 40.00th=[  437], 50.00th=[  457], 60.00th=[  486],
     | 70.00th=[  537], 80.00th=[  783], 90.00th=[ 8160], 95.00th=[ 8291],
     | 99.00th=[ 8586], 99.50th=[ 8848], 99.90th=[ 9503], 99.95th=[ 9765],
     | 99.99th=[10421]
```

```
P1  = 302μs     → 1% 的 I/O 延迟 ≤ 302μs（最快的）
P50 = 457μs     → ★ 中位数延迟 457μs（一半的 I/O 比这快）
P80 = 783μs     → 80% 的 I/O 延迟 ≤ 783μs
P90 = 8160μs    → ★★ 突然跳到 8.16ms！P80→P90 出现断崖
P99 = 8586μs    → 99% 的 I/O 延迟 ≤ 8.59ms
P99.99 = 10421μs → 极端长尾延迟 10.4ms

★★ 关键发现：P80=783μs vs P90=8160μs → 延迟断崖！
   80% 的 I/O 在 1ms 内完成，但剩下 20% 突然跳到 8ms+
   这说明设备有周期性延迟抖动（可能是 virtio-blk 调度或宿主机争用）
```

#### 带宽与 IOPS 采样

```
   bw (  KiB/s): min=63888, max=196752, per=100.00%, avg=70638.80, stdev=29684.12, samples=20
   iops        : min=15972, max=49188, avg=17659.70, stdev=7421.03, samples=20
```

```
bw min=63888 KiB/s    → 最低带宽 ≈ 62.4 MiB/s
bw max=196752 KiB/s   → 最高带宽 ≈ 192 MiB/s（★ 波动很大）
bw avg=70638.80       → 平均带宽 ≈ 69 MiB/s
samples=20            → 采样 20 次（默认 500ms 一次，10s 共 20 次）
iops max=49188        → 峰值 IOPS 近 5 万（第一个采样可能突发）
```

#### 延迟分布桶

```
  lat (usec)   : 250=0.12%, 500=63.66%, 750=15.81%, 1000=1.43%
  lat (msec)   : 2=1.13%, 4=0.41%, 10=17.41%, 20=0.03%
```

```
250=0.12%    → 0.12% 的 I/O 延迟 ≤ 250μs
500=63.66%   → ★ 63.66% 的 I/O 延迟在 250-500μs（主体）
750=15.81%   → 15.81% 在 500-750μs
1000=1.43%   → 1.43% 在 750-1000μs
2=1.13%      → 1.13% 在 1-2ms
10=17.41%    → ★★ 17.41% 在 2-10ms（长尾延迟群）

延迟分布呈现"双峰"：
  - 主峰：250-500μs（63.66%）→ 正常 I/O
  - 副峰：2-10ms（17.41%）  → 设备周期性抖动
```

#### CPU 占用

```
  cpu          : usr=3.75%, sys=7.45%, ctx=180267, majf=0, minf=11
```

```
usr=3.75%     → 用户态 CPU（fio 计算 offset、填充 buffer 等）
sys=7.45%     → 内核态 CPU（io_submit/io_getevents 系统调用）
ctx=180267    → 上下文切换次数（≈ IOPS × runtime = 17600 × 10 ≈ 176000 ✓）
majf=0        → 主缺页 0 次（所有内存已分配）
minf=11       → 次缺页 11 次（极少）
```

#### IO 队列深度分布

```
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=100.0%, >=64=0.0%
```

```
32=100.0%     → ★ 几乎所有时间队列深度都达到 32（iodepth 充分利用）
其他深度占比 0.1% → 只在启动/结束阶段有短暂的非满队列
```

#### 提交与完成分布

```
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, ...
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, ..., 32=0.1%, ...
```

```
submit 4=100.0%  → 每次 io_submit() 提交 4 个请求（fio 默认批次）
complete 4=100.0% → 每次 io_getevents() 收割 4 个完成事件
complete 32=0.1%  → 极少数时候收割了 32 个（队列满时的批量收割）
```

#### 总 I/O 统计

```
     issued rwts: total=0,176629,0,0 short=0,0,0,0 dropped=0,0,0,0
```

```
total=0,176629,0,0  → read=0, write=176629, trim=0, sync=0
short=0,0,0,0       → 无 short I/O（所有请求都完成了全部数据）
dropped=0,0,0,0     → 无丢弃的 I/O
```

#### 磁盘统计

```
Disk stats (read/write):
  vdb: ios=52/174837, merge=0/0, ticks=18/292441, in_queue=292459, util=98.75%
```

```
ios=52/174837       → 读 52 次（系统预读等），写 174837 次
merge=0/0           → 无合并（4K 随机写无法合并）
ticks=18/292441     → 读/写总耗时（毫秒）
in_queue=292459     → 队列中总等待时间
util=98.75%         → ★ 设备利用率 98.75%（几乎满负荷）
```

---

## 场景 2：顺序读输出解读

### 命令

```bash
fio --name=seqread --ioengine=libaio --direct=1 --rw=read \
    --bs=1M --filename=/dev/vdb --runtime=10 --time_based \
    --iodepth=16
```

### 完整输出（关键部分）

```
seqread: (groupid=0, jobs=1): err= 0: pid=379577: Fri Jul  3 08:22:08 2026
  read: IOPS=198, BW=198MiB/s (208MB/s)(1995MiB/10057msec)
    slat (usec): min=27, max=3809, avg=105.60, stdev=176.74
    clat (usec): min=1701, max=198564, avg=80418.04, stdev=34300.63
     lat (msec): min=2, max=198, avg=80.52, stdev=34.27
    clat percentiles (msec):
     |  1.00th=[   11],  5.00th=[   17], 10.00th=[   21], 20.00th=[   51],
     | 30.00th=[   70], 40.00th=[   81], 50.00th=[   90], 60.00th=[   90],
     | 70.00th=[  101], 80.00th=[  110], 90.00th=[  121], 95.00th=[  130],
     | 99.00th=[  161], 99.50th=[  171], 99.90th=[  199], 99.95th=[  199],
     | 99.99th=[  199]
   bw (  KiB/s): min=178176, max=567296, per=99.81%, avg=202752.00, stdev=85882.44, samples=20
  cpu          : usr=0.30%, sys=1.39%, ctx=3309, majf=0, minf=4108
  IO depths    : 1=0.1%, 2=0.1%, 4=0.2%, 8=0.4%, 16=99.2%, 32=0.0%, >=64=0.0%
     issued rwts: total=1995,0,0,0 short=0,0,0,0 dropped=0,0,0,0
```

### 与随机写的关键差异

```
对比项            随机写 (4K)          顺序读 (1M)         差异原因
───────────────  ───────────────     ───────────────     ──────────────────
IOPS             17,600              198                 1M 块太大，IOPS 自然低
BW               68.9 MiB/s          198 MiB/s           ★ 大块顺序读带宽 2.9 倍
slat avg         10.95 μs (nsec 单位) 105.60 μs          大块 I/O 提交更慢
clat avg         1,801 μs            80,418 μs           ★ 1M 块设备处理时间 80ms
lat avg          1,812 μs            80,520 μs           大块延迟远大于小块
P50 clat         457 μs              90 ms               ★ 200 倍差异！
ctx 切换          180,267             3,309               IOPS 低→上下文切换少
CPU usr          3.75%               0.30%               IOPS 低→CPU 占用少
CPU sys          7.45%               1.39%               系统调用次数少
minf             11                  4,108               大块 buffer 需要更多页
IO depths 16     -                   99.2%               16 个在飞
util             98.75%              99.02%              都很高
```

**关键洞察：** 顺序读 1M 块时，虽然 IOPS 只有 198（每次 I/O 处理 1MB 数据），但带宽达到 198 MiB/s，远超 4K 随机写的 68.9 MiB/s。这说明**大块顺序 I/O 能更充分利用磁盘带宽**。

---

## 场景 3：混合读写输出解读

### 命令

```bash
fio --name=randrw --ioengine=libaio --direct=1 --rw=randrw \
    --bs=4k --filename=/dev/vdb --runtime=10 --iodepth=16 \
    --rwmixread=70
```

### 完整输出

```
randrw: (groupid=0, jobs=1): err= 0: pid=379703: Fri Jul  3 08:22:19 2026
  read: IOPS=12.3k, BW=47.9MiB/s (50.2MB/s)(479MiB/10001msec)
    slat (nsec): min=1537, max=9994.6k, avg=17688.23, stdev=74974.92
    clat (usec): min=84, max=18253, avg=932.51, stdev=1665.86
     lat (usec): min=153, max=18265, avg=950.20, stdev=1667.52
    clat percentiles (usec):
     |  1.00th=[  194],  5.00th=[  229], 10.00th=[  251], 20.00th=[  285],
     | 30.00th=[  314], 40.00th=[  338], 50.00th=[  367], 60.00th=[  404],
     | 70.00th=[  461], 80.00th=[  603], 90.00th=[ 1975], 95.00th=[ 6128],
     | 99.00th=[ 7242], 99.50th=[ 7701], 99.90th=[ 9372], 99.95th=[10421],
     | 99.99th=[12780]
  write: IOPS=5246, BW=20.5MiB/s (21.5MB/s)(205MiB/10001msec); 0 zone resets
    slat (nsec): min=1675, max=9256.6k, avg=19452.56, stdev=97099.37
    clat (usec): min=81, max=18096, avg=804.73, stdev=1620.23
     lat (usec): min=129, max=18106, avg=824.18, stdev=1623.23
    clat percentiles (usec):
     |  1.00th=[  161],  5.00th=[  188], 10.00th=[  204], 20.00th=[  225],
     | 30.00th=[  243], 40.00th=[  260], 50.00th=[  277], 60.00th=[  302],
     | 70.00th=[  334], 80.00th=[  433], 90.00th=[ 1352], 95.00th=[ 5932],
     | 99.00th=[ 7111], 99.50th=[ 7439], 99.90th=[ 9110], 99.95th=[10290],
     | 99.99th=[12649]
     issued rwts: total=122579,52470,0,0 short=0,0,0,0 dropped=0,0,0,0
```

### 注解

```
★ 混合读写输出包含两个 section：read 和 write

read section:
  IOPS=12.3k              → 读 IOPS
  rwmixread=70 → 期望 70% 读
  实际 read=122579, write=52470
  读占比 = 122579/(122579+52470) = 70.0% ✓

  P50 clat=367μs          → 读的延迟中位数 367μs
  P90 clat=1975μs         → 读的 P90 延迟 1.97ms

write section:
  IOPS=5246               → 写 IOPS（比纯写低，因为 iodepth 分给了读）
  P50 clat=277μs          → ★ 写的中位延迟比读更低！
  P90 clat=1352μs         → 写的 P90 也低于读

★ 关键发现：
  混合 I/O 时，读延迟略高于写延迟。原因：
  1. 读操作需要从设备取数据到内存
  2. 写操作可以被设备缓存（写缓存）
  3. 读和写共享 iodepth=16，竞争队列资源

  issued rwts: total=122579,52470,0,0
                read↑     write↑ trim↑ sync↑
  ★ 格式：逗号分隔的四个值 = read, write, trim, sync
```

---

## 场景 4：iodepth 对比实验

### 命令

```bash
for depth in 1 4 16 32 64 128; do
    fio --name=depth$depth --ioengine=libaio --direct=1 --rw=randwrite \
        --bs=4k --filename=/dev/vdb --runtime=6 --time_based \
        --iodepth=$depth
done
```

### 结果汇总

```
iodepth │ IOPS    │ BW (MiB/s) │ slat avg    │ clat avg   │ lat avg    │ util
────────┼─────────┼────────────┼─────────────┼────────────┼────────────┼───────
   1    │  4,252  │  16.6      │ 58.70 μs    │ 170.57 μs  │ 229.27 μs  │ 85%
   4    │ 18,700  │  73.2      │ 12.57 μs    │ 199.61 μs  │ 212.17 μs  │ -
  16    │ 16,700  │  65.3      │ 10.88 μs    │ 945.30 μs  │ 956.17 μs  │ -
  32    │ 16,700  │  65.1      │ 24.33 μs    │ 1894 μs    │ 1918 μs    │ 99%
  64    │ 16,700  │  65.4      │ 15.77 μs    │ 3805 μs    │ 3821 μs    │ -
 128    │ 16,800  │  65.5      │ 14.87 μs    │ 7619 μs    │ 7634 μs    │ -
```

### 分析

```
★ IOPS 饱和曲线：

  IOPS
  20k ┤         ●━━━━━●━━━━━●━━━━━●
  18k ┤    ●
  16k ┤
      │
   5k ┤
   4k ┤ ●
      └──┬────┬────┬────┬────┬────┬──→ iodepth
         1    4    16   32   64  128

关键发现：
  1. iodepth=1 → 4,252 IOPS（单队列延迟限制）
  2. iodepth=4 → 18,700 IOPS（★ 拐点！4 倍增长）
  3. iodepth=16 → 16,700 IOPS（★ 饱和！不再增长）
  4. iodepth=32~128 → 稳定在 16,700-16,800 IOPS

★ 延迟增长曲线：

  clat avg
  8ms ┤                                          ●
  4ms ┤                              ●
  2ms ┤                    ●
  1ms ┤          ●
200μs ┤    ●  ●
      └──┬────┬────┬────┬────┬────┬──→ iodepth
         1    4    16   32   64  128

  ★ iodepth=1 时延迟最低（170μs），但 IOPS 也最低
  ★ iodepth=4 时延迟仍然低（200μs），IOPS 达到峰值
  ★ iodepth>16 后延迟线性增长（排队效应），但 IOPS 不变

★ 最佳 iodepth = 4（最高 IOPS + 最低延迟的最佳平衡点）
  对于此 virtio-blk 设备，iodepth=4 就足够饱和带宽。
```

---

## 场景 5：单队列延迟测试

### 命令

```bash
fio --name=lat_test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/vdb --runtime=10 --time_based \
    --iodepth=1 --numjobs=1
```

### 完整输出

```
lat_test: (groupid=0, jobs=1): err= 0: pid=396233: Fri Jul  3 08:24:40 2026
  write: IOPS=5559, BW=21.7MiB/s (22.8MB/s)(217MiB/10001msec); 0 zone resets
    slat (usec): min=3, max=3096, avg=25.26, stdev=54.74
    clat (usec): min=2, max=3125, avg=151.56, stdev=70.38
     lat (usec): min=119, max=3158, avg=176.82, stdev=88.88
    clat percentiles (usec):
     |  1.00th=[  100],  5.00th=[  122], 10.00th=[  126], 20.00th=[  131],
     | 30.00th=[  135], 40.00th=[  139], 50.00th=[  141], 60.00th=[  145],
     | 70.00th=[  151], 80.00th=[  159], 90.00th=[  176], 95.00th=[  202],
     | 99.00th=[  355], 99.50th=[  490], 99.90th=[ 1123], 99.95th=[ 1369],
     | 99.99th=[ 2343]
  cpu          : usr=3.48%, sys=6.45%, ctx=109649, majf=0, minf=11
  IO depths    : 1=100.0%, 2=0.0%, 4=0.0%, 8=0.0%, 16=0.0%, 32=0.0%, >=64=0.0%
```

### 注解

```
★ iodepth=1 的特殊输出特征：

  IO depths: 1=100.0%     → 始终只有 1 个在飞 I/O（符合预期）

  clat P50 = 141μs        → ★ 中位延迟只有 141μs！
  clat P90 = 176μs        → P90 也只有 176μs
  clat P99 = 355μs        → P99 延迟 355μs（远好于 iodepth=32 的 8.6ms）

  ★ 对比 iodepth=1 vs iodepth=32：
                iodepth=1    iodepth=32    差异
    clat P50    141 μs       457 μs        3.2 倍
    clat P90    176 μs       8160 μs       ★★ 46 倍！
    clat P99    355 μs       8586 μs       ★★ 24 倍！
    IOPS        5,559        17,600        3.2 倍

  ★★ 关键结论：
    - iodepth=32 时 IOPS 高 3.2 倍，但 P90 延迟高 46 倍！
    - 这就是"排队效应"：并发 I/O 互相等待设备资源
    - 测试延迟必须用 iodepth=1，测试吞吐用高 iodepth

  ctx=109649              → 上下文切换 ≈ IOPS × runtime = 5559 × 10 ≈ 55590
                            实际 109649 ≈ 2 倍（submit + complete 各一次）
```

---

## 场景 6：JSON 输出解读

### 命令

```bash
fio --name=json_test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/vdb --runtime=8 --time_based \
    --iodepth=32 --output-format=json
```

### JSON 输出（关键片段）

```json
{
  "fio version" : "fio-3.34",
  "timestamp" : 1783038203,
  "timestamp_ms" : 1783038203611,
  "time" : "Fri Jul  3 08:23:23 2026",
  "jobs" : [
    {
      "jobname" : "json_test",
      "groupid" : 0,
      "error" : 0,
      "elapsed" : 9,
      "job options" : {
        "name" : "json_test",
        "ioengine" : "libaio",
        "direct" : "1",
        "rw" : "randwrite",
        "bs" : "4k",
        "filename" : "/dev/vdb",
        "runtime" : "8",
        "iodepth" : "32"
      },
```

```json
      "read" : {
        "io_bytes" : 0,        ← 读方向无数据（randwrite 模式）
        "io_kbytes" : 0,
        "bw_bytes" : 0,
        "bw" : 0,
        "iops" : 0.000000,
        "runtime" : 0,
        "total_ios" : 0
      },
```

```json
      "write" : {
        "io_bytes" : 556036096,    ← 总写入 556MB
        "io_kbytes" : 543004,      ← 543004 KB
        "bw_bytes" : 69478457,     ← 精确带宽 69,478,457 B/s
        "bw" : 67850,              ← 67850 KiB/s ≈ 66.3 MiB/s
        "iops" : 16962.514057,     ← ★ 浮点数精度的 IOPS
        "runtime" : 8003,          ← 实际运行 8003ms
        "total_ios" : 135751,      ← 总 I/O 次数
        "short_ios" : 0,           ← 无短 I/O
        "drop_ios" : 0,            ← 无丢弃 I/O
```

```json
        "slat_ns" : {
          "min" : 1541,            ← 纳秒单位
          "max" : 2996104,
          "mean" : 10544.175461,
          "stddev" : 14991.243580,
          "N" : 135751             ← ★ 采样数 = 总 I/O 次数
        },
        "clat_ns" : {
          "min" : 118296,
          "max" : 11609718,
          "mean" : 1874829.401065,
          "stddev" : 2972441.559955,
          "N" : 135751,
          "percentile" : {
            "1.000000" : 292864,   ← P1 = 292.864μs
            "5.000000" : 337920,   ← P5 = 337.92μs
            "50.000000" : 456704,  ← ★ P50 = 456.7μs
            "90.000000" : 8159232, ← ★ P90 = 8.16ms
            "99.000000" : 8585216, ← P99 = 8.59ms
            "99.900000" : 9371648, ← P99.9 = 9.37ms
            "99.990000" : 10158080 ← P99.99 = 10.16ms
          }
        },
```

```json
      "iodepth_level" : {
        "1" : 0.100000,            ← 0.1% 时间 iodepth=1
        "2" : 0.100000,
        "4" : 0.100000,
        "8" : 0.100000,
        "16" : 0.100000,
        "32" : 99.976783,          ← ★ 99.98% 时间 iodepth=32
        ">=64" : 0.000000
      },
```

```json
  "disk_util" : [
    {
      "name" : "vdb",
      "read_ios" : 55,             ← 读 I/O 次数（系统活动）
      "write_ios" : 131816,        ← 写 I/O 次数
      "read_merges" : 0,           ← 无读合并
      "write_merges" : 0,          ← 无写合并
      "read_ticks" : 17,           ← 读总耗时（ms）
      "write_ticks" : 236596,      ← 写总耗时（ms）
      "in_queue" : 236613,         ← 队列中总时间
      "util" : 98.636307           ← ★ 设备利用率 98.6%
    }
  ]
}
```

### 用 jq 提取关键指标

```bash
# 提取 IOPS
fio ... --output-format=json | jq '.jobs[0].write.iops'
# 输出：16962.514057

# 提取 P99 延迟
fio ... --output-format=json | jq '.jobs[0].write.clat_ns.percentile["99.000000"]'
# 输出：8585216（纳秒，÷1000 = 8585μs）

# 提取设备利用率
fio ... --output-format=json | jq '.disk_util[0].util'
# 输出：98.636307
```

---

## 场景 7：延迟日志 + 画图

### 命令

```bash
fio --name=lat_log --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/vdb --runtime=8 --time_based \
    --iodepth=32 --write_lat_log=fio_lat_log
```

### 生成的文件

```
fio_lat_log_clat.1.log    ← clat 延迟日志
fio_lat_log_slat.1.log    ← slat 延迟日志
fio_lat_log_lat.1.log     ← lat 总延迟日志
```

**文件命名规则：** `<前缀>_<类型>.<jobid>.log`

### clat 日志格式

```bash
head -20 fio_lat_log_clat.1.log
```

```
1, 366423, 1, 4096, 0
1, 382471, 1, 4096, 0
1, 382272, 1, 4096, 0
1, 409220, 1, 4096, 0
1, 413323, 1, 4096, 0
1, 414696, 1, 4096, 0
1, 415319, 1, 4096, 0
1, 415763, 1, 4096, 0
1, 431830, 1, 4096, 0
1, 432974, 1, 4096, 0
1, 419932, 1, 4096, 0
1, 411597, 1, 4096, 0
1, 429112, 1, 4096, 0
1, 405377, 1, 4096, 0
1, 396198, 1, 4096, 0
1, 382463, 1, 4096, 0
1, 454597, 1, 4096, 0
1, 441932, 1, 4096, 0
1, 470637, 1, 4096, 0
1, 450446, 1, 4096, 0
```

### 日志字段注解

```
格式：时间(ms), 延迟(ns), ddir, bs, offset(可选)

1, 366423, 1, 4096, 0
│  │       │  │     │
│  │       │  │     └── offset（需 --log_offset=1 才有值）
│  │       │  └──────── I/O 块大小 4096 字节
│  │       └─────────── 方向：0=read, 1=write, 2=trim
│  └─────────────────── 延迟值：366423 ns = 366.4 μs
└────────────────────── 采样时间：第 1ms

★ 每一行代表一个完成的 I/O 的延迟记录
★ 与 normal 输出的百分位数据对应：
  - 大部分值在 300,000-500,000 ns（300-500μs）→ 对应 P50=457μs
  - 偶尔出现 >8,000,000 ns（8ms+）→ 对应 P90=8160μs
```

### gnuplot 画延迟散点图

```bash
gnuplot -e '
  set terminal png size 1200,400
  set output "lat_scatter.png"
  set xlabel "Time (ms)"
  set ylabel "clat (ns)"
  set title "fio clat latency over time"
  plot "fio_lat_log_clat.1.log" using 1:2 with dots title "clat"
'
```

### gnuplot 画延迟分布直方图

```bash
# 先用 awk 统计延迟分布
awk '{
  v = int($2 / 100000)   # 按 100μs 分桶
  bins[v]++
}
END {
  for (b in bins) print b*100, bins[b]
}' fio_lat_log_clat.1.log | sort -n > lat_hist.dat

# gnuplot 画直方图
gnuplot -e '
  set terminal png size 1200,400
  set output "lat_hist.png"
  set xlabel "clat (usec)"
  set ylabel "count"
  set title "fio clat latency distribution"
  set style fill solid
  plot "lat_hist.dat" using 1:2 with boxes title "clat distribution"
'
```

---

## 场景 8：数据验证模式

### 命令

```bash
fio --name=verify_test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/tmp/fio_verify_test --size=50M \
    --verify=crc32c --do_verify=1
```

### 完整输出

```
verify_test: (groupid=0, jobs=1): err= 0: pid=396191: Fri Jul  3 08:24:24 2026
  read: IOPS=171k, BW=667MiB/s (699MB/s)(50.0MiB/75msec)
    slat (nsec): min=1420, max=681375, avg=3819.64, stdev=12379.70
    clat (nsec): min=595, max=353169, avg=991.22, stdev=6304.24
     lat (usec): min=2, max=696, avg= 4.81, stdev=14.18
  write: IOPS=173k, BW=676MiB/s (708MB/s)(50.0MiB/74msec); 0 zone resets
    slat (usec): min=3, max=221, avg= 4.75, stdev= 4.89
    clat (nsec): min=592, max=85504, avg=694.52, stdev=1403.78
     lat (usec): min=4, max=227, avg= 5.44, stdev= 5.19
     issued rwts: total=12800,12800,0,0 short=0,0,0,0 dropped=0,0,0,0
```

### 注解

```
★ 验证模式的特殊行为：

  1. write phase（写阶段）：
     - 50MB 数据，每块填充 CRC32C 校验和
     - 12800 次写 = 50MB / 4KB ✓
     - IOPS=173k（文件在 /tmp → tmpfs，极快）

  2. read/verify phase（验证读阶段）：
     - 读回 12800 块数据
     - 对每块重新计算 CRC32C 并与存储的校验和对比
     - 如果不匹配 → 报错（verify_fatal=1 时停止）

  3. 延迟极低的原因：
     - /tmp 是 tmpfs（内存文件系统），无实际磁盘 I/O
     - 验证阶段的 clat avg=991ns（不到 1μs！）
     - 实际设备测试时延迟会高得多

  4. issued rwts: total=12800,12800,0,0
     read=12800  ← 验证读的次数（= write 次数）
     write=12800 ← 写入次数
     ★ read == write 说明每块都成功验证了

  5. err= 0 → 无验证错误 → 数据完整性通过 ✓
```

---

## 场景 9：fio 输出与 btt 对比

### 实验设置

同时运行 fio 和 blktrace，对比两者的延迟报告。

```bash
# 启动 blktrace 采集
blktrace -d /dev/vdb -o fio_blktrace -w 8 &
sleep 1

# 运行 fio
fio --name=bt_test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/vdb --runtime=6 --time_based --iodepth=32
wait

# blkparse + btt 分析
blkparse -i ./fio_blktrace -O -d fio_blktrace.bin
btt -i fio_blktrace.bin -o btt_fio
```

### fio 报告的延迟

```
bt_test: write: IOPS=18.8k, BW=73.2MiB/s
    slat (nsec): min=1792, max=2954.1k, avg=12795.49
    clat (usec): min=125, max=10741, avg=1692.23
     lat (usec): min=137, max=10755, avg=1705.02
    clat percentiles (usec):
     | 50.00th=[  482], 90.00th=[ 7963], 99.00th=[ 8586]
```

### btt 报告的延迟

```
==================== All Devices ====================

            ALL           MIN           AVG           MAX           N
--------------- ------------- ------------- ------------- -----------
Q2G               0.000000276   0.000470810   6.110704651      112762
G2I               0.000000116   0.000000255   0.000091479      112762
I2D               0.000000430   0.000004013   0.000403983      112746
D2C               0.000082490   0.001504971   0.009264542      112746
Q2C               0.000084921   0.001564036   6.109742962      112746
```

### 对比分析

```
指标对比：

                    fio 报告         btt 报告        差异       原因
──────────────────  ───────────────  ──────────────  ─────────  ───────────────
提交延迟 (slat/Q2D) 12.80 μs         474.81 μs ★     ★ 37 倍    见下文分析
设备延迟 (clat/D2C) 1692.23 μs       1504.97 μs      12%        测量点不同
总延迟 (lat/Q2C)    1705.02 μs       1564.04 μs      9%         累加差异

★★ slat vs Q2D 的巨大差异分析：

fio slat = 12.80 μs
btt Q2D avg = 474.81 μs
差异 = 37 倍！

原因分析：
  1. fio slat 测量的是 engine->queue() 返回时间
     - 对于 libaio：io_submit() 返回的时间（内核接受请求）
     - 这个时间非常短（微秒级），因为 io_submit() 只是入队

  2. btt Q2D = D_time - Q_time
     - Q 事件在 block_bio_queue tracepoint（bio 进入 block layer）
     - D 事件在 block_rq_issue tracepoint（request 下发到设备）
     - 中间包含 Q→G→I→D 的全部阶段

  3. 关键差异在于 G 阶段（Get Request）：
     - btt Q2G avg = 470.81 μs ← ★ 占 Q2D 的 99%！
     - request 分配等待时间是主要开销
     - fio slat 不包含这个等待（io_submit 返回时 request 还没分配）

  4. 总结：
     fio slat + fio clat ≈ btt Q2C
     12.80 + 1692.23 = 1705.03 μs ≈ btt Q2C 1564.04 μs（差 9%）

     差异 9% 的原因：
     - fio 和 btt 的时钟源不同
     - fio 的 slat/clat 测量点与 blktrace 的 Q/D/C 事件点不完全对应
     - 统计时间窗口略有不同
```

```
fio 延迟 vs blktrace 延迟的完整映射：

  fio 用户态：
  ┌──────────────────────────────────────────────────┐
  │  slat                    │        clat           │
  │  (engine->queue)         │  (queue→completion)   │
  └──────────────────────────────────────────────────┘

  blktrace 内核态：
  ┌────────┬──────┬──────┬────────────────────────────┐
  │  Q2G   │ G2I  │ I2D  │          D2C              │
  │ 0.47ms │0.26μs│ 4μs  │        1.50ms             │
  └────────┴──────┴──────┴────────────────────────────┘
  │←─────── Q2D ──────────→│←──────── D2C ──────────→│
  │←────────────────── Q2C ──────────────────────────→│

  ★ fio slat  ≈ io_submit() 系统调用时间（不含 Q2G 等待）
  ★ fio clat  ≈ D2C + 部分 Q2D（内核接受→完成的全部时间）
  ★ fio lat   ≈ Q2C（端到端，但测量点略有差异）

  ★ 要获得完整的延迟分解（Q2G/G2I/I2D/D2C），
    必须用 blktrace + btt，fio 只能给出端到端的 slat/clat/lat。
```

---

## 快速诊断流程

```
1. 确定测试目标：
   - 测延迟 → iodepth=1, numjobs=1, --write_lat_log
   - 测吞吐 → iodepth=32+, bs=1M, --time_based
   - 测 IOPS → iodepth=32+, bs=4k, --time_based

2. 分析输出关键字段：
   - IOPS：每秒 I/O 操作数（越高越好）
   - BW：带宽（MiB/s，越大越好）
   - clat P50：中位延迟（越低越好）
   - clat P99：长尾延迟（SLA 指标）
   - util：设备利用率（接近 100% 说明饱和）

3. 延迟分解：
   - slat 高 → fio 提交慢（CPU 瓶颈 / io_submit 系统调用开销）
   - clat 高 → 设备处理慢（磁盘性能 / 队列拥塞）
   - P90 突然跳变 → 设备有周期性延迟抖动

4. 结合 blktrace 深度分析：
   - fio 报告的是端到端延迟
   - blktrace/btt 可以拆分出 Q2G/G2I/I2D/D2C
   - 用 btt 定位瓶颈在软件层（Q2D）还是硬件层（D2C）
```
