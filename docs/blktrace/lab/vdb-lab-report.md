# blktrace / blkparse / btt 实战手册

> 设备：/dev/vdb (40G virtio-blk)  
> 系统：Alibaba Cloud Linux 4  
> 工具：从 src/blktrace 源码编译

---

## 零、环境准备

### 编译 blktrace

```bash
cd src/blktrace
make -j$(nproc)
# 如果 btreplay 编译失败（缺少 libaio.h），手动链接核心工具：
gcc -o blktrace blktrace.o rbtree.o act_mask.o -lpthread
gcc -o blkparse blkparse.o blkparse_fmt.o rbtree.o act_mask.o
# btt 在 btt/ 子目录中：btt/btt
```

### 安装 fio

```bash
yum install -y fio    # 或 apt install fio
```

### 验证环境

```bash
lsblk /dev/vdb
# NAME MAJ:MIN RM SIZE RO TYPE
# vdb  253:16   0  40G  0 disk
```

---

## 实验 1：基础采集 — fio 随机写 + blktrace + blkparse + btt

### 目标

体验完整的 blktrace → blkparse → btt 工作流，理解每个工具的职责。

### 步骤

```bash
mkdir -p /tmp/blktrace_lab && cd /tmp/blktrace_lab

# 1. 启动 blktrace（追踪 /dev/vdb，8 秒后自动停止）
blktrace -d /dev/vdb -o exp1 -w 8 &
sleep 1

# 2. 用 fio 生成 I/O 负载（4K 随机写，libaio 引擎，direct I/O）
fio --name=test1 \
    --ioengine=libaio \
    --direct=1 \
    --rw=randwrite \
    --bs=4k \
    --numjobs=1 \
    --size=50M \
    --filename=/dev/vdb \
    --runtime=6 \
    --time_based

# 3. 等待 blktrace 完成
wait
```

### blktrace 输出

```
=== vdb ===
  CPU  0:    36845 events,  1728 KiB data
  CPU  1:    22708 events,  1065 KiB data
  CPU  2:    63366 events,  2971 KiB data
  CPU  3:    29089 events,  1364 KiB data
  Total:    152008 events (dropped 0),  7126 KiB data
```

**解读：**
- 4 个 CPU 各生成了一个二进制文件：`exp1.blktrace.0` ~ `exp1.blktrace.3`
- 共 152008 个事件，无 dropped（buffer 够用）
- 每个 CPU 的事件数不均匀（CPU 2 最多），说明 I/O 处理偏向某些 CPU

### blkparse 解析

```bash
# 解析为文本
blkparse -i exp1 -o exp1_parsed.txt

# 同时生成二进制文件（供 btt 使用）
blkparse -i exp1 -O -d exp1.bin
```

**blkparse 输出解读（前几行）：**

```
253,16   2   1  0.000000000   441  Q   R 0 + 8 [systemd-udevd]
253,16   2   2  0.000008516   441  G   R 0 + 8 [systemd-udevd]
253,16   2   3  0.000010763   441  I   R 0 + 8 [systemd-udevd]
253,16   2   4  0.000051409    26  D   R 0 + 8 [kworker/2:0H]
253,16   2   5  0.000383052 330818  C   R 0 + 8 [0]
```

```
字段解析：
253,16   → 设备号 (major=253, minor=16) = /dev/vdb
2        → CPU 号
1        → 事件序号
0.000... → 时间戳（秒）
441      → PID
Q        → 事件类型（Queue = bio 进入 block layer）
R        → rwbs 字段（Read）
0 + 8    → 起始扇区 0，共 8 个扇区（4KB）
[xxx]    → 进程名
```

**事件类型统计：**

```bash
awk '{print $6}' exp1_parsed.txt | sort | uniq -c | sort -rn
```

```
  30333 D    ← Issue（下发到驱动）
  30333 C    ← Complete（完成）
  30331 I    ← Insert（插入调度器）
  30331 G    ← Get Request（分配 request）
  30321 Q    ← Queue（入队）
     92 U    ← Unplug（拔塞）
     92 P    ← Plug（插队）
     10 X    ← Split（bio 拆分）
```

**解读：**
- Q → G → I → D → C 是标准 I/O 生命周期，数量基本一致（~30331）
- P/U 各 92 次 → plug 机制被使用了 92 次
- X 有 10 次 → 有 10 个 bio 被拆分
- D = C = 30333 → 每个下发的请求都完成了，无丢失

**rwbs 字段统计：**

```bash
awk '{print $7}' exp1_parsed.txt | sort | uniq -c | sort -rn
```

```
 151125 WS   ← Write + Sync（占绝大多数！）
    510 RA   ← Read + Readahead（预读）
    184 N    ← None（P/U 等通知事件，bytes=0）
     24 R    ← Read
```

**解读：**
- **WS = Write + Sync** → fio 使用 `--direct=1`（O_DIRECT），内核为 direct I/O 设置 `REQ_SYNC` 标志
- **RA = Read + Readahead** → 内核自动发起的顺序预读
- **N = None** → Plug/Unplug/GetRq 等事件的 rwbs 字段，bytes=0 没有实际读写

### btt 分析

```bash
btt -i exp1.bin -o exp1_btt
cat exp1_btt.avg
```

```
==================== All Devices ====================
            ALL           MIN           AVG           MAX           N
--------------- ------------- ------------- ------------- -----------
Q2G               0.000000344   0.004581352   6.103389173       30331
G2I               0.000000172   0.000000413   0.000069464       30331
I2D               0.000000409   0.000004187   0.000239133       30293
D2C               0.000089691   0.000169321   0.004942750       30293
```

**解读：**

| 指标 | 平均值 | 含义 |
|------|--------|------|
| **Q2G** | 4.58ms | 等待 request 分配（★ 占延迟的主要部分！） |
| **G2I** | 0.4μs | 从分配到插入调度器（几乎无延迟） |
| **I2D** | 4.2μs | 在调度器中等待（调度器很快） |
| **D2C** | 0.17ms | 设备处理时间（virtio-blk 很快） |

**关键发现：**
- **Q2G 平均值高达 4.58ms** — 说明 request 分配是瓶颈
- **D2C 平均 0.17ms** — 虚拟磁盘很快，设备不是瓶颈
- **Q2C = Q2G + G2I + I2D + D2C ≈ 4.75ms** — 端到端延迟

---

## 实验 2：顺序读 — 对比读写模式差异

### 步骤

```bash
blktrace -d /dev/vdb -o exp2 -w 8 &
sleep 1

# 128K 顺序读，iodepth=16（队列深度 16）
fio --name=seqread \
    --ioengine=libaio \
    --direct=1 \
    --rw=read \
    --bs=128k \
    --numjobs=1 \
    --size=100M \
    --filename=/dev/vdb \
    --runtime=6 \
    --time_based \
    --iodepth=16

wait
blkparse -i exp2 -o exp2_parsed.txt 2>/dev/null
blkparse -i exp2 -O -d exp2.bin 2>/dev/null
btt -i exp2.bin -o exp2_btt 2>/dev/null
```

### 结果

**事件类型统计：**

```
  10142 Q    ← Queue
  10119 I    ← Insert
  10119 G    ← Get Request
  10119 D    ← Issue
  10119 C    ← Complete
     23 M    ← Merge（合并了 23 次）
```

**rwbs 字段统计：**

```
  50641 R    ← Read（纯读，没有 S 标志）
```

**btt 延迟对比：**

```
            ALL           MIN           AVG           MAX           N
--------------- ------------- ------------- ------------- -----------
Q2G               0.000000326   0.000001349   0.000043230       10119
G2I               0.000000126   0.000000425   0.000040242       10119
I2D               0.000001593   0.000004546   0.000172699       10119
D2C               0.000259782   0.009394294   0.030126612       10142
```

**与实验 1 的对比：**

| 指标 | 实验 1（随机写） | 实验 2（顺序读） | 差异原因 |
|------|-----------------|-----------------|---------|
| Q2G avg | 4.58ms | **0.001ms** | 顺序读 request 分配很快 |
| D2C avg | 0.17ms | **9.4ms** | 128K 大块读，设备耗时更长 |
| I2D avg | 4.2μs | 4.5μs | 调度器开销类似 |
| rwbs | WS | R | direct write=WS, read=R |
| Merge | 0 | 23 | 顺序读触发了 bio 合并 |

---

## 实验 3：FUA / Flush 检测

### 步骤

```bash
blktrace -d /dev/vdb -o exp3 -w 8 &
sleep 1

# sync 引擎 + fsync=1（每次写后 fsync，触发 flush/FUA）
fio --name=fua \
    --ioengine=sync \
    --rw=randwrite \
    --bs=4k \
    --numjobs=1 \
    --size=50M \
    --filename=/dev/vdb \
    --runtime=6 \
    --time_based \
    --fsync=1

wait
blkparse -i exp3 -o exp3_parsed.txt 2>/dev/null
```

### 结果

**rwbs 字段统计：**

```
 144096 WS    ← Write + Sync（普通同步写）
  48216 N     ← None（flush 请求的 bytes=0）
  48032 FWS   ← ★ Preflush + Write + Sync（先 flush 再写）
  48032 FN    ← ★ Flush + None（独立 flush 请求）
    485 RA    ← Read + Readahead
```

**★ 关键发现：FWS 和 FN！**

```
FWS = Preflush(F) + Write(W) + Sync(S)
  → 先刷设备缓存 → 写数据 → 同步标志
  → ext4 ordered 模式的典型行为

FN = Flush(F) + None(N)
  → 独立的 flush 请求（fsync 内部发送的 flush 命令）
  → bytes=0 所以 rwbs 第二位是 N
```

**含 Flush/FUA 的事件示例：**

```
253,16  2  51  0.101256734  331590  Q  FWS  [fio]      ← fio 发起 FWS 写
253,16  2  52  0.101260952  331590  G  FWS  [fio]
253,16  2  53  0.101271121     26  D  FN   [kworker]   ← 驱动先处理 flush
253,16  2  54  0.101344635      0  C  FN   0 [0]       ← flush 完成
```

**如何识别 FUA vs Flush：**

```bash
# 方法 1：查找 F 在非首位的（即 FUA）
awk '$7 ~ /.+F/{print}' exp3_parsed.txt

# 方法 2：查找 F 在首位的（即 Preflush / Flush）
awk '$7 ~ /^F/{print}' exp3_parsed.txt

# 方法 3：统计不同类型的 F 请求
awk '$7 ~ /F/{print $7}' exp3_parsed.txt | sort | uniq -c | sort -rn
```

---

## 实验 4：action 过滤 — 只追踪 D 和 C

### 目标

减少数据量，只关注下发和完成事件。

### 步骤

```bash
# -a issue -a complete → 只追踪 D(ISSUE) 和 C(COMPLETE) 事件
blktrace -d /dev/vdb -o exp4 -a issue -a complete -w 5 &
sleep 1

fio --name=test4 \
    --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --numjobs=1 --size=50M \
    --filename=/dev/vdb --runtime=3 --time_based

wait
blkparse -i exp4 -o exp4_parsed.txt 2>/dev/null
```

### 结果

```bash
awk '{print $6}' exp4_parsed.txt | sort | uniq -c | sort -rn
```

```
  17877 D    ← 只有 D
  17877 C    ← 只有 C
```

**对比实验 1（不过滤）：**
- 实验 1: 152008 个事件（Q+G+I+D+C+P+U+...）
- 实验 4: 35754 个事件（只有 D+C）
- **数据量减少 76%！**

### 使用场景

- 只想分析 D2C 延迟（设备处理时间）
- 高 IOPS 场景下减少 buffer 压力（避免 dropped）
- 快速判断设备是否是瓶颈

---

## 实验 5：自定义输出格式

### 步骤

```bash
# 使用 -f 自定义 blkparse 输出格式
blkparse -i exp4 -f "%D %T %p %a %u %S+%r\n"
```

### 输出

```
253,16  0.000000000  165  D  R  0+8
253,16  0.000384562    0  C  R  0+8
253,16  0.000440441  165  D  R  8+8
253,16  0.000687662    0  C  R  8+8
```

### 格式字符串完整参考

| 格式符 | 含义 | 示例 |
|--------|------|------|
| `%D` | 设备号 | 253,16 |
| `%T` | 时间戳 | 0.001234567 |
| `%p` | PID | 1234 |
| `%a` | 事件字母 | D |
| `%u` | rwbs 字段 | WS |
| `%S` | 起始扇区 | 12345678 |
| `%r` | 扇区数 | 8 |
| `%N` | 字节数 | 4096 |
| `%c` | CPU 号 | 2 |
| `%n` | 序列号 | 42 |
| `%m` | 进程名 | fio |
| `%e` | 错误码 | 0 |

---

## 实验 6：btt 延迟数据文件 + 画图

### 步骤

```bash
# btt 生成 per-IO 延迟数据文件
btt -i exp1.bin \
    --q2c-latencies=exp1_q2c \
    --d2c-latencies=exp1_d2c \
    --q2d-latencies=exp1_q2d \
    -o exp1_btt_lat 2>/dev/null

# 生成的文件
ls exp1_*_253,16_*.dat
# exp1_q2c_253,16_q2c.dat  — 每行：时间戳(秒) 延迟(秒)
# exp1_d2c_253,16_d2c.dat
# exp1_q2d_253,16_q2d.dat
```

### 文件格式

```bash
head -5 exp1_q2c_253,16_q2c.dat
```

```
0.000000 0.000383    ← 第 0ms 时的 Q2C 延迟 = 383μs
0.000460 0.000292    ← 第 0.46ms 时的 Q2C 延迟 = 292μs
0.005283 0.000467    ← 第 5.28ms 时的 Q2C 延迟 = 467μs
0.005813 0.000272
0.006136 0.000258
```

### D2C 统计

```bash
awk '{sum+=$2; n++} END{print "记录数:", n, "平均D2C:", sum/n, "秒"}' exp1_d2c_253,16_d2c.dat
# 记录数: 30293 平均D2C: 0.000169319 秒
# → 平均设备处理时间 = 0.17ms
```

### 画图（可选）

```bash
# 使用 gnuplot 画延迟散点图
gnuplot -e '
  set terminal png size 1200,400
  set output "latency.png"
  set xlabel "Time (s)"
  set ylabel "Latency (s)"
  set title "Block I/O Latency"
  plot "exp1_q2c_253,16_q2c.dat" using 1:2 with dots title "Q2C", \
       "exp1_d2c_253,16_d2c.dat" using 1:2 with dots title "D2C"
'
```

---

## 总结：常用命令速查

```bash
# ===== 采集 =====
blktrace -d /dev/vdb -o trace -w 10           # 追踪 10 秒
blktrace -d /dev/vdb -o trace -a issue -a complete  # 只追踪 D+C
blktrace -d /dev/vdb -o trace -b 4096 -n 8    # 增大 buffer（避免 drop）
blktrace -d /dev/vdb -o trace -p 1234         # 追踪指定 PID

# ===== 解析 =====
blkparse -i trace -o parsed.txt               # 文本输出
blkparse -i trace -O -d trace.bin             # 二进制输出（供 btt）
blkparse -i trace -f "%T %a %u %S+%r\n"      # 自定义格式

# ===== 分析 =====
btt -i trace.bin -o summary                   # 完整分析
btt -i trace.bin --q2c-latencies=q2c \
                 --d2c-latencies=d2c           # 生成延迟数据文件
btt -i trace.bin -p per_io                    # per-IO 时间线

# ===== 快速检查 =====
# 事件类型分布
blkparse -i trace -o - 2>/dev/null | awk '{print $6}' | sort | uniq -c | sort -rn

# rwbs 分布
blkparse -i trace -o - 2>/dev/null | awk '{print $7}' | sort | uniq -c | sort -rn

# 找 FUA/Flush 请求
blkparse -i trace -o - 2>/dev/null | awk '$7 ~ /F/'

# 检查 dropped 事件
cat /sys/kernel/debug/block/vdb/dropped

# 通过 ftrace 验证 tracepoint
cat /sys/kernel/debug/tracing/available_events | grep block
```
