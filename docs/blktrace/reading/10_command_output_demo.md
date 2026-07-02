# blktrace 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐行解读。
> 设备：/dev/vdb (40G virtio-blk)，I/O 由 fio 生成。

---

## 场景 1：基础采集与全流程分析

### 命令

```bash
blktrace -d /dev/vdb -o trace -w 6 &       # 采集 6 秒
sleep 1
fio --name=t1 --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=4 --time_based
wait

blkparse -i trace -o parsed.txt              # 解析为文本
blkparse -i trace -O -d trace.bin            # 解析为二进制
btt -i trace.bin -o btt_out                  # btt 分析
```

### blktrace 输出（采集统计）

```
=== vdb ===
  CPU  0:    29277 events,  1373 KiB data
  CPU  1:    22008 events,  1032 KiB data
  CPU  2:    45976 events,  2156 KiB data     ← CPU 2 事件最多（I/O 处理偏向）
  CPU  3:    19901 events,   933 KiB data
  Total:   117162 events (dropped 0),  5493 KiB data
                                    ↑
                              ★ dropped=0 表示无丢失
```

### blkparse 输出（逐行解读）

```
253,16   2        1     0.000000000   441  Q   R 0 + 8 [systemd-udevd]
253,16   2        2     0.000007929   441  G   R 0 + 8 [systemd-udevd]
253,16   2        3     0.000010700   441  I   R 0 + 8 [systemd-udevd]
253,16   2        4     0.000041102    26  D   R 0 + 8 [kworker/2:0H]
253,16   2        5     0.000343403 351185  C   R 0 + 8 [0]
```

```
253,16   → 设备号 major=253, minor=16 (/dev/vdb)
2        → CPU 号
1~5      → 事件序号（per-CPU 递增）
0.000... → 时间戳（秒，纳秒精度）
441      → PID（发起 I/O 的进程）
Q/G/I/D/C → 事件字母（入队/分配/插入/下发/完成）
R        → rwbs 字段（Read）
0 + 8    → 起始扇区 0，共 8 扇区 = 4KB
[xxx]    → 进程名
[0]      → C 事件的 PID 通常是 0（中断上下文完成）
```

**一个 I/O 的完整生命周期：** Q→G→I→D→C，时间从 0 到 343μs。

### blkparse 事件类型统计

```bash
awk '{print $6}' parsed.txt | sort | uniq -c | sort -rn
```

```
  23373 D    ← Issue（下发）
  23373 C    ← Complete（完成）    D = C → 无丢失
  23371 I    ← Insert（插入调度器）
  23371 G    ← Get Request（分配）
  23361 Q    ← Queue（入队）       Q ≈ G ≈ I ≈ D ≈ C
     92 P    ← Plug（插队）
     92 U    ← Unplug（拔塞）      P = U → 配对正常
     10 X    ← Split（bio 拆分）
```

### blkparse rwbs 统计

```
 116325 WS   ← Write + Sync（fio --direct=1 → O_DIRECT → REQ_SYNC）
    510 RA   ← Read + Readahead（内核预读）
    184 N    ← None（P/U 等控制事件，bytes=0）
     24 R    ← Read（普通读）
```

**关键解读：** fio `--direct=1` 打开 O_DIRECT，内核为 direct I/O 设置 `REQ_SYNC`，所以 rwbs 显示 **WS** 而非单纯的 W。

### btt 延迟分析

```
==================== All Devices ====================
            ALL           MIN           AVG           MAX           N
--------------- ------------- ------------- ------------- -----------
Q2G               0.000000324   0.001404763   4.101086978       23371
G2I               0.000000172   0.000000346   0.000046755       23371
I2D               0.000000416   0.000003724   0.000128879       23355
D2C               0.000082166   0.000146829   0.002562170       23355
```

```
指标      AVG         MAX        解读
────────  ──────────  ─────────  ──────────────────────────────
Q2G       1.4ms       4.1s ★     request 分配是主要瓶颈（有极端值）
G2I       0.3μs       47μs       进入调度器几乎无延迟
I2D       3.7μs       129μs      调度器等待很短
D2C       147μs       2.6ms      设备处理时间（virtio-blk 很快）
```

**诊断结论：** Q2G 平均 1.4ms 远大于其他阶段 → **request 分配是瓶颈**。D2C 仅 147μs → 设备本身很快。

---

## 场景 2：只追踪 D 和 C（减少数据量）

### 命令

```bash
blktrace -d /dev/vdb -o trace_dc -a issue -a complete -w 5
blkparse -i trace_dc -o dc_parsed.txt
```

### 输出

```
=== 事件类型统计 ===
  17206 D
  17206 C

=== 输出片段 ===
253,16   3   1  0.000000000    77  D   R 0 + 8 [kworker/3:1H]
253,16   2   1  0.000452476 352275  C   R 0 + 8 [0]
253,16   3   2  0.000675399    77  D   R 8 + 8 [kworker/3:1H]
253,16   2   2  0.000946082 352275  C   R 8 + 8 [0]
```

**对比场景 1：**
```
全事件：117162 个（Q+G+I+D+C+P+U+X）
仅 D+C：34496 个  → 数据量减少 71%
```

**用途：** 只关心设备处理延迟（D2C）时，用 `-a issue -a complete` 大幅减少数据量和 buffer 压力。

---

## 场景 3：检测 FUA / Flush 请求

### 命令

```bash
# 用 sync 引擎 + fsync=1 触发 FUA/Flush
blktrace -d /dev/vdb -o trace_fua -w 5 &
sleep 1
fio --name=t3 --ioengine=sync --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=3 --fsync=1
```

### rwbs 中含 F 的请求统计

```bash
awk '$7 ~ /F/{print $7}' fua_parsed.txt | sort | uniq -c | sort -rn
```

```
  21524 FWS   ← Preflush + Write + Sync
  21524 FN    ← Flush + None（独立 flush 命令）
```

### 实际事件示例

```
253,16  1  15  0.102796926  359880  Q  FWS  [fio]       ← fio 提交 FWS 请求
253,16  1  16  0.102798580  359880  G  FWS  [fio]       ← 分配 request
253,16  1  17  0.102814241      82  D  FN   [kworker]   ← ★ 内核先下发 flush（FN）
253,16  1  18  0.102846431       0  C  FN   0 [0]       ← flush 完成
```

**解读：** `--fsync=1` 使 fio 每次写后调用 fsync，内核将其分解为：
1. **FN**：先发 flush 命令（刷设备缓存）
2. **FWS**：再写数据（Preflush + Write + Sync）

FWS 中 F 在首位 = Preflush，N 在 FN 中 = bytes=0（flush 不携带数据）。

---

## 场景 4：自定义输出格式

### 命令

```bash
blkparse -i trace -f "%D %T %p %a %u %S+%r\n" -o custom.txt
```

### 输出

```
253,16  0.000000000  441  Q  R  0+8
253,16  0.000007929  441  G  R  0+8
253,16  0.000010700  441  I  R  0+8
253,16  0.000041102   26  D  R  0+8
253,16  0.000343403    0  C  R  0+8
```

```
%D = 设备号 (253,16)
%T = 时间戳 (0.000343403)
%p = PID (441, 0)
%a = 事件字母 (Q/G/I/D/C)
%u = rwbs (R/W/WS/FWS...)
%S = 起始扇区 (0)
%r = 扇区数 (8)
```

**用途：** 自定义格式方便用 awk/sort/uniq 做二次分析，例如：

```bash
# 统计各事件的平均延迟
awk '{print $4, $2}' custom.txt | sort | uniq -c
# 找 FUA 事件
awk '$5 ~ /F/' custom.txt
```

---

## 场景 5：btt seek 分析（随机 vs 顺序）

### 命令

```bash
# 随机写
blktrace -d /dev/vdb -o rand -w 5 &
fio --name=rand --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=3 --time_based

# 顺序读
blktrace -d /dev/vdb -o seq -w 5 &
fio --name=seq --ioengine=libaio --direct=1 --rw=read --bs=128k \
    --filename=/dev/vdb --runtime=3 --time_based --iodepth=16

# 分别分析
blkparse -i rand -O -d rand.bin && btt -i rand.bin -o rand_btt
blkparse -i seq -O -d seq.bin && btt -i seq.bin -o seq_btt
```

### Q2Q Seek 对比

```
随机写 (4K randwrite):
==================== Device Q2Q Seek Information ====================
       DEV |          NSEEKS            MEAN          MEDIAN | MODE
---------- | --------------- --------------- --------------- | ---------------
 (253, 16) |           17818      27867241.2               0 | 8(23)
                           ^^^^^^^^^^^^^^^^^^
                           ★ MEAN ≈ 2786 万扇区 ≈ 13GB

顺序读 (128K sequential read):
==================== Device Q2Q Seek Information ====================
       DEV |          NSEEKS            MEAN          MEDIAN | MODE
---------- | --------------- --------------- --------------- | ---------------
 (253, 16) |            5832             0.0               0 | 0(5832)
                                  ^^^^^
                                  ★ MEAN = 0（完美顺序！）
```

**解读：**

| 指标 | 随机写 | 顺序读 | 差异 |
|------|--------|--------|------|
| **Q2Q MEAN** | 27,867,241 扇区 | **0** 扇区 | ★ ∞ 倍 |
| **D2D MEAN** | 27,859,423 扇区 | **0** 扇区 | ★ ∞ 倍 |
| **MODE** | 8(23次) | 0(5832次) | 顺序读所有 seek 距离为 0 |

→ **MEAN = 0 是完美顺序 I/O 的标志。** MEAN 越大越随机。

### btt Merge 信息

```
==================== Device Merge Information ====================
       DEV |       #Q       #D   Ratio |   BLKmin   BLKavg   BLKmax    Total
---------- | -------- -------- ------- | -------- -------- -------- --------
 (253, 16) |    17823    17823     1.0 |        8        8      256   145960
```

```
#Q = 17823   ← 入队 request 数
#D = 17823   ← 下发 request 数
Ratio = 1.0  ← 无合并（Ratio=1.0 说明没有合并发生）
BLKavg = 8   ← 平均 8 扇区 = 4KB（符合 --bs=4k）
```

→ 4K 随机写没有合并机会（扇区不连续），所以 Ratio=1.0。

### btt Plug 信息

```
==================== Plug Information ====================
       DEV |    # Plugs # Timer Us  | % Time Q Plugged
---------- | ---------- ----------  | ----------------
 (253, 16) |         92(         0) |   0.001502582%
```

```
#Plugs = 92           ← 发生了 92 次 plug
#Timer Us = 0         ← 没有定时器触发的 unplug
% Time Q Plugged = 0.0015% ← 极少时间处于 plugged 状态
```

→ libaio 引擎的 I/O 提交模式很少触发 plug（直接提交），所以 Plug 次数很少。

---

## 场景 6：btt 延迟数据文件（可画图）

### 命令

```bash
btt -i trace.bin \
    --q2c-latencies=lat_q2c \
    --d2c-latencies=lat_d2c \
    -o lat
```

### 生成的文件

```
lat_q2c_253,16_q2c.dat   ← 每行：时间戳(秒) Q2C延迟(秒)
lat_d2c_253,16_d2c.dat   ← 每行：时间戳(秒) D2C延迟(秒)
```

### Q2C 延迟数据样本

```
0.000000 0.000343    ← 第 0ms 时的 Q2C = 343μs
0.000463 0.000415    ← 第 0.46ms 时的 Q2C = 415μs
0.005343 0.000501    ← 第 5.34ms 时的 Q2C = 501μs
0.005889 0.000500
0.006417 0.000238
```

### D2C 延迟统计

```bash
awk '{sum+=$2;n++;if($2>max)max=$2} END{printf "N=%d avg=%.6f max=%.6f\n",n,sum/n,max}' \
    lat_d2c_253,16_d2c.dat
```

```
N=23355 avg=0.000147 max=0.002562
         ^^^^^^^^^   ^^^^^^^^^
         ★ 147μs     ★ 2.6ms（长尾延迟）
```

**用途：** `.dat` 文件可直接用 gnuplot 画延迟散点图，观察延迟随时间的变化趋势。

---

## 场景 7：blkiomon 实时监控

### 命令

```bash
# blkiomon 从 stdin 读取（必须用管道）
blktrace -d /dev/vdb -o - -w 6 | blkiomon -I 3 -h /tmp/blkiomon_out.txt
# 同时生成 I/O
fio --name=t --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=4 --time_based
```

### 输出（每 3 秒一次）

```
time: Thu Jul  2 23:29:13 2026
device: 253,16
sizes read (bytes): num 54, min 20, max 65536, sum 1069076, avg 19797.7
sizes write (bytes): num 9967, min 4096, max 4096, sum 40824832, avg 4096.0
d2c read (usec): num 54, min 83, max 1488, avg 323.9
d2c write (usec): num 9967, min 108, max 2148, avg 145.6
throughput read (bytes/msec): avg 63970.2
throughput write (bytes/msec): avg 29310.2
sizes histogram (bytes):
            0:     0         1024:     1         2048:     0         4096:  9998
         8192:     2        16384:     2        32768:     5        65536:    13
d2c histogram (usec):
            0:     0            8:     0           16:     0           32:     0
           64:     0          128:  2656          256:  7148          512:   187
         1024:    18         2048:    11         4096:     1         8192:     0
bidirectional requests: 0
```

```
sizes write avg 4096.0     ← 写大小全是 4K（--bs=4k）
d2c write avg 145.6 μs     ← ★ 写延迟平均 146μs
d2c histogram:              ← D2C 分布：
  128μs: 2656                  大部分在 128-256μs
  256μs: 7148                  ← 峰值在这里
  512μs: 187
  1024μs: 18                   少量长尾
throughput: 29310 bytes/msec ≈ 29 MB/s
```

→ blkiomon 适合**实时监控** I/O 模式变化，无需等采集完成。

---

## 场景 8：ftrace 验证 tracepoint

### 命令

```bash
# 启用 block tracepoint
echo 1 > /sys/kernel/debug/tracing/events/block/block_rq_issue/enable
echo 1 > /sys/kernel/debug/tracing/events/block/block_rq_complete/enable

# 生成 I/O
fio --name=ft --ioengine=sync --direct=1 --rw=read --bs=4k \
    --filename=/dev/vdb --size=40k

# 查看 ftrace 输出
cat /sys/kernel/debug/tracing/trace | grep block_rq | head -8

# 关闭
echo 0 > /sys/kernel/debug/tracing/events/block/enable
```

### ftrace 输出

```
kworker/0:1H-165  [000] ..... 1698028.241505: block_rq_issue: 253,16 RS 4096 () 0 + 8 [kworker/0:1H]
<idle>-0          [002] ..s1. 1698028.241837: block_rq_complete: 253,16 RS () 0 + 8 [0]
kworker/0:1H-165  [000] ..... 1698028.241877: block_rq_issue: 253,16 RS 4096 () 8 + 8 [kworker/0:1H]
<idle>-0          [002] ..s1. 1698028.242122: block_rq_complete: 253,16 RS () 8 + 8 [0]
```

```
kworker/0:1H-165  → 内核工作线程 PID=165（代替用户进程下发 I/O）
[000]             → CPU 0
1698028.241505    → ftrace 时间戳
block_rq_issue    → ★ 这就是 blktrace D 事件的内核 tracepoint
253,16            → 设备号
RS                → rwbs（Read + Sync）
4096              → I/O 大小
0 + 8             → 扇区 0，8 扇区
<idle>-0          → C 事件在 idle 线程中（中断上下文）
..s1.             → s=softirq 上下文
```

**关键对比：**
```
ftrace:   block_rq_issue: 253,16 RS 4096 () 0 + 8
blkparse: 253,16  0  4  0.000041102  26  D  R  0 + 8 [kworker/2:0H]
                                                ↑
                                            同一个事件！
```

→ ftrace 和 blktrace 使用**相同的内核 tracepoint**，但数据采集机制不同（ring_buffer vs relay buffer）。

### tracepoint 格式信息

```bash
cat /sys/kernel/debug/tracing/events/block/block_rq_issue/format
```

```
name: block_rq_issue
format:
    field:dev_t dev;              offset:8;   size:4;    ← 设备号
    field:sector_t sector;        offset:16;  size:8;    ← 起始扇区
    field:unsigned int nr_sector; offset:24;  size:4;    ← 扇区数
    field:unsigned int bytes;     offset:28;  size:4;    ← 字节数
    field:char rwbs[8];           offset:32;  size:8;    ← R/W/S/F 标志
    field:char comm[16];          offset:40;  size:16;   ← 进程名
```

→ 这些字段就是 blktrace `blk_io_trace` 结构体中记录的内容来源。

---

## 快速诊断流程

```
1. blktrace 采集 → blkparse 解析 → 看事件分布
   awk '{print $6}' parsed.txt | sort | uniq -c
   → 了解 I/O 生命周期各阶段事件数量

2. 看 rwbs 分布
   awk '{print $7}' parsed.txt | sort | uniq -c
   → 了解 I/O 类型（读/写/FUA/预读）

3. btt 分析延迟
   btt -i trace.bin -o summary && cat summary.avg
   → D2C = 设备延迟, Q2C = 端到端延迟

4. 判断顺序/随机
   grep "Q2Q Seek" summary.avg
   → MEAN=0 完美顺序, MEAN>10000 明显随机

5. 检测 FUA/Flush
   awk '$7 ~ /F/' parsed.txt
   → 含 F 的请求都是持久化保证写入
```
