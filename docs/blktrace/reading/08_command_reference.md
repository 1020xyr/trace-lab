# blktrace 完整命令参考与源码实现

> 每个常用命令选项的实现源码位置，以及完整的 RWBS / 事件类型速查表

---

## 一、blktrace 命令参考

### 基本用法

```bash
blktrace -d <device> [-o <output>] [-w <seconds>] [-a <mask>] [-b <size>] [-n <nr>]
```

### 参数与源码实现对照

| 参数 | 含义 | 用户态源码位置 | 内核源码位置 |
|------|------|---------------|-------------|
| `-d /dev/sda` | 指定追踪设备 | `blktrace.c: handle_args()` | `blktrace.c: do_blk_trace_setup()` |
| `-o trace` | 输出文件名前缀 | `blktrace.c: handle_args()` | — |
| `-w 10` | 追踪 10 秒后自动停止 | `blktrace.c: alarm(timeout)` | — |
| `-a issue` | 只追踪 D 事件 | `blktrace.c: handle_args()` → `act_mask` | `blktrace.c: act_log_check()` |
| `-a complete` | 只追踪 C 事件 | 同上 | 同上 |
| `-b 512` | relay subbuffer 大小(KB) | `blktrace.c: buf_size` | `blktrace.c: relay_open(buf_size)` |
| `-n 4` | relay subbuffer 数量 | `blktrace.c: buf_nr` | `blktrace.c: relay_open(buf_nr)` |
| `-p 1234` | 只追踪指定 PID | `blktrace.c: handle_args()` | `blktrace.c: buts->pid` |
| `-A file` | 从文件读取参数 | `blktrace.c: handle_args()` | — |
| `-k` | 停止已有追踪 | `blktrace.c: kill_running_trace` | `ioctl(BLKTRACETEARDOWN)` |

### `-a` 参数支持的 action mask 名称

```bash
# 用户态源码：blktrace.c 中的 mask_map[]
# 对应内核的 BLK_TC_* 标志位

blktrace -a read        # BLK_TC_READ     (bit 0)
blktrace -a write       # BLK_TC_WRITE    (bit 1)
blktrace -a flush       # BLK_TC_FLUSH    (bit 2)
blktrace -a sync        # BLK_TC_SYNC     (bit 3)
blktrace -a queue       # BLK_TC_QUEUE    (bit 4)  ← Q 事件
blktrace -a requeue     # BLK_TC_REQUEUE  (bit 5)
blktrace -a issue       # BLK_TC_ISSUE    (bit 6)  ← D 事件
blktrace -a complete    # BLK_TC_COMPLETE (bit 7)  ← C 事件
blktrace -a fs          # BLK_TC_FS       (bit 8)
blktrace -a pc          # BLK_TC_PC       (bit 9)
blktrace -a notify      # BLK_TC_NOTIFY   (bit 10)
blktrace -a ahead       # BLK_TC_AHEAD    (bit 11)
blktrace -a meta        # BLK_TC_META     (bit 12)
blktrace -a discard     # BLK_TC_DISCARD  (bit 13)
blktrace -a drv_data    # BLK_TC_DRV_DATA (bit 14)
blktrace -a fua         # BLK_TC_FUA      (bit 15) ← FUA 事件
blktrace -a barrier     # (已废弃，早期内核)

# 多个 -a 参数可组合使用
blktrace -d /dev/sda -a issue -a complete -a fua  # 只追踪 D、C 和 FUA
```

---

## 二、blkparse 命令参考

### 基本用法

```bash
blkparse -i <input_prefix> [-o <output>] [-d <binary_output>] [-f <format>]
```

### 参数与源码实现对照

| 参数 | 含义 | 源码位置 |
|------|------|---------|
| `-i trace` | 输入文件前缀 | `blkparse.c: main()` |
| `-o parsed.txt` | 文本输出文件 | `blkparse.c: ofp` |
| `-d trace.bin` | 二进制输出（供 btt） | `blkparse.c: output_binary()` |
| `-O` | 禁止文本输出（只生成二进制） | `blkparse.c: text_output` |
| `-f "%D %T %a"` | 自定义输出格式 | `blkparse.c: blkparse_fmt.c` |
| `-s` | 显示统计摘要 | `blkparse.c: dump_io_stats()` |
| `-S` | 隐藏统计摘要 | `blkparse.c: show_statistics` |
| `-t` | 显示 per-process 统计 | `blkparse.c: dump_process_stats()` |
| `-D` | 指定输入目录 | `blkparse.c: input_dir` |

### `-f` 格式字符串完整说明

```
格式符 │ 含义                    │ 示例
───────┼────────────────────────┼────────────────
  %D   │ 设备号 (major,minor)   │ 8,0
  %c   │ CPU 号                 │ 1
  %C   │ CPU 号（无空格对齐）   │ 1
  %n   │ 事件序列号             │ 42
  %N   │ 传输大小（字节）       │ 4096
  %T   │ 时间戳（秒.纳秒）      │ 0.001234567
  %p   │ PID                    │ 1234
  %a   │ 事件动作字母           │ Q / D / C / G / I / M / P / U
  %m   │ 进程名                 │ fio
  %S   │ 起始扇区号             │ 12345678
  %r   │ 扇区数                 │ 8
  %d   │ 传输方向 (R/W/D)       │ R
  %u   │ rwbs 字段完整值        │ WS / RF / RA
  %e   │ 错误码                 │ 0
  %P   │ 传输大小（页）         │ 1
  %z   │ 传输大小（扇区）       │ 8
  %I   │ 请求的完成百分比       │ 100
```

---

## 三、btt 命令参考

### 基本用法

```bash
btt -i <binary_input> [-o <output_prefix>] [选项]
```

### 参数与源码实现对照

| 参数 | 含义 | 源码位置 |
|------|------|---------|
| `-i trace.bin` | 输入二进制文件 | `btt/args.c` |
| `-o summary` | 输出文件前缀 | `btt/args.c` |
| `-l` | 输出 per-IO 延迟文件 | `btt/latency.c: latency_alloc()` |
| `--q2c-latencies=name` | Q2C 延迟文件 | `btt/latency.c: latency_q2c()` |
| `--d2c-latencies=name` | D2C 延迟文件 | `btt/latency.c: latency_d2c()` |
| `--q2d-latencies=name` | Q2D 延迟文件 | `btt/latency.c: latency_q2d()` |
| `-L` | 输出周期性延迟统计 | `btt/output.c` |
| `-B` | 输出块号分布 | `btt/bno_dump.c` |
| `-I` | 输出 IOPS 统计 | `btt/iostat.c` |
| `-p` | 输出 per-IO 时间线 | `btt/trace_complete.c: per_io_ofp` |
| `-s` | 输出 seek 分析 | `btt/seek.c` |
| `-M dev_map` | DM 设备映射文件 | `btt/devmap.c` |

### btt 输出文件说明

```
<output_prefix>_q2c.dat    Q2C 延迟（时间戳 延迟值）
<output_prefix>_d2c.dat    D2C 延迟
<output_prefix>_q2d.dat    Q2D 延迟
<output_prefix>_aqd.dat    平均队列深度
<output_prefix>_iops.dat   IOPS 统计
<output_prefix>_bno.dat    块号分布
<output_prefix>_spt.dat    每追踪扇区数
<output_prefix>_dev.dat    设备级统计
```

---

## 四、RWBS 字段完整速查表

### RWBS（Read/Write/Barrier/Sync）字段

blkparse 输出中的 rwbs 字段（如 `WS`、`RF`、`RA`）由 `blk_fill_rwbs()` 函数生成。
每个字符代表一种属性，可以组合出现。

### 源码位置

- 内核：`src/linux-5.10/block/blk-core.c` 中的 `blk_fill_rwbs()`
- 用户态：`src/blktrace/blkparse.c` 中的 rwbs 解析

### 完整字符对照表

```
字符 │ 全称            │ 内核标志      │ 含义
─────┼────────────────┼──────────────┼──────────────────────────────────
  R  │ Read           │ REQ_OP_READ  │ 读操作
  W  │ Write          │ REQ_OP_WRITE │ 写操作
  D  │ Discard        │ REQ_OP_DISCARD│ TRIM/discard 操作
  F  │ Flush/FUA      │ REQ_FUA /    │ ★ 强制写入持久化介质（绕过写缓存）
     │                │ REQ_PREFLUSH │   或 flush 前缀操作
  S  │ Sync           │ REQ_SYNC     │ 同步 I/O（O_SYNC 打开的文件）
  M  │ Metadata       │ REQ_META     │ 元数据操作（如 journal 写入）
  A  │ Readahead      │ REQ_RAHEAD   │ 预读（内核自动顺序预读）
  N  │ None           │ (无标志)      │ 无特殊属性（普通异步 I/O）
```

### 常见组合

```
rwbs │ 含义                                    │ 常见场景
─────┼────────────────────────────────────────┼──────────────────────
  R  │ 普通异步读                              │ 普通文件读取
  W  │ 普通异步写                              │ 后台写回（writeback）
  RS │ 同步读                                  │ O_DIRECT 读取
  WS │ 同步写                                  │ O_SYNC 写入、fsync()
  WF │ 写 + FUA                                │ ext4 journal 写入
  WM │ 写 + 元数据                             │ 文件系统元数据更新
  WSM│ 同步写 + 元数据                         │ journal commit
  RA │ 读 + 预读                               │ 顺序读文件的预读
  D  │ Discard                                 │ fstrim / SSD TRIM
  FF │ Flush                                   │ 显式 flush（fsync 内部）
  FS │ Flush + Sync                            │ 同步 flush
```

### FUA 与 Flush 的区别

```
FUA（Force Unit Access）：
  - 只针对当前这一次写入，强制写到介质
  - 不需要额外的 flush 命令
  - 性能开销：一次 I/O（但比普通写慢，因为要等介质确认）
  - 内核标志：REQ_FUA

Flush：
  - 将设备写缓存中所有数据刷到介质
  - 影响所有之前未刷盘的写入
  - 性能开销：可能需要多次 I/O（取决于缓存中有多少脏数据）
  - 内核标志：REQ_PREFLUSH（前缀 flush）或 REQ_FPOSTFLUSH（后缀 flush）

如果设备不支持 FUA：
  内核会自动将 FUA 写入转换为 FLUSH + WRITE + FLUSH（三次 I/O！）
```

---

## 五、完整事件类型速查表

### 所有事件类型（含 P/U/G/I/M 等）

```
字母 │ 操作名           │ 内核 tracepoint         │ 说明
─────┼──────────────────┼────────────────────────┼────────────────────────────
  Q  │ Queue            │ block_bio_queue         │ bio 进入 block layer
  G  │ Get Request      │ block_getrq             │ 分配 request 结构
  S  │ Sleep            │ block_sleeprq           │ 等待 request 分配
  I  │ Insert           │ block_rq_insert         │ 插入 I/O 调度器
  M  │ Merge            │ block_bio_backmerge     │ 与已有 request 合并
     │                  │ block_bio_frontmerge    │
  P  │ Plug             │ block_plug              │ 插队（延迟下发）
  U  │ Unplug           │ block_unplug            │ 拔塞（开始下发）
  D  │ Issue/Dispatch   │ block_rq_issue          │ 下发到设备驱动
  C  │ Complete         │ block_rq_complete       │ 设备完成 I/O
  R  │ Requeue          │ block_rq_requeue        │ 设备拒绝，重新入队
  A  │ Readahead        │ (通过 rwbs 中的 A)      │ 预读请求
  F  │ Flush/FUA        │ (通过 rwbs 中的 F)      │ flush 或 FUA
  B  │ Bounce           │ block_bio_bounce        │ 使用 bounce buffer
  X  │ Split            │ block_split             │ bio 被拆分
  m  │ Remap            │ block_bio_remap         │ bio 被重映射(DM/LVM)
```

### 延迟指标完整对照

```
指标  │ 公式                    │ 含义                      │ 瓶颈诊断
──────┼────────────────────────┼──────────────────────────┼──────────────
Q2G   │ G_time - Q_time        │ 等待 request 分配         │ 系统内存/队列资源
Q2I   │ I_time - Q_time        │ 进入调度器前的延迟         │ block layer 开销
I2D   │ D_time - I_time        │ 在调度器中等待             │ I/O 调度器压力
Q2D   │ D_time - Q_time        │ 软件层总延迟              │ 内核软件栈
D2C   │ C_time - D_time        │ 设备处理延迟              │ 硬件/驱动性能
Q2C   │ C_time - Q_time        │ 端到端总延迟              │ 整体性能
Q2Q   │ 相邻 Q 事件的间隔       │ I/O 到达率                │ 应用层行为
D2D   │ 相邻 D 事件的间隔       │ 设备下发率                │ 设备吞吐
```

---

## 六、常用命令组合

### 场景 1：基础采集与全流程分析

```bash
# 采集（10 秒，全事件类型）
blktrace -d /dev/sda -o trace -w 10

# 解析为文本
blkparse -i trace -o parsed.txt

# 解析为二进制（供 btt 分析）
blkparse -i trace -d trace.bin

# btt 分析
btt -i trace.bin -o summary
cat summary_q2c.dat  # Q2C 延迟数据
```

### 场景 2：只追踪 D 和 C（减少数据量）

```bash
blktrace -d /dev/sda -o trace -a issue -a complete -w 10
blkparse -i trace -o parsed.txt
```

### 场景 3：检测 FUA 请求

```bash
# 方法 1：通过 blkparse 过滤 rwbs 中的 'F'
blktrace -d /dev/sda -o trace -w 10
blkparse -i trace -o parsed.txt
grep "F" parsed.txt  # 找出所有 FUA/Flush 请求

# 方法 2：只追踪 FUA 事件
blktrace -d /dev/sda -o trace -a fua -w 10
blkparse -i trace -o fua_only.txt
```

### 场景 4：延迟分析 + 画图

```bash
blktrace -d /dev/sda -o trace -w 30
blkparse -i trace -d trace.bin

# btt 生成延迟文件
btt -i trace.bin \
    --q2c-latencies=q2c \
    --d2c-latencies=d2c \
    --q2d-latencies=q2d

# 用 gnuplot 画图
gnuplot -e '
  set terminal png size 1200,400
  set output "latency.png"
  plot "trace_sda_q2c.dat" using 1:2 with dots title "Q2C", \
       "trace_sda_d2c.dat" using 1:2 with dots title "D2C", \
       "trace_sda_q2d.dat" using 1:2 with dots title "Q2D"
'
```

### 场景 5：追踪特定进程

```bash
blktrace -d /dev/sda -o trace -p 1234 -w 10
blkparse -i trace -o parsed.txt
```

### 场景 6：实时监控（blkiomon）

```bash
# blkiomon 是 blktrace 工具包中的实时监控工具
blkiomon -I 5 -d /dev/sda
# -I 5 = 每 5 秒刷新一次
```

### 场景 7：通过 ftrace 验证 blktrace 使用的 tracepoint

```bash
# 查看可用的 block tracepoint
cat /sys/kernel/debug/tracing/available_events | grep block

# 启用 block tracepoint
echo 1 > /sys/kernel/debug/tracing/events/block/block_rq_issue/enable
echo 1 > /sys/kernel/debug/tracing/events/block/block_rq_complete/enable

# 生成 I/O
dd if=/dev/sda of=/dev/null bs=4k count=10

# 查看输出
cat /sys/kernel/debug/tracing/trace | head -20

# 关闭
echo 0 > /sys/kernel/debug/tracing/events/block/enable
```

### 场景 8：增大 buffer 避免 drop

```bash
# 默认 buffer: 512KB × 4 = 2MB/CPU
# 高 IOPS 场景可能不够，增大到 4MB × 8 = 32MB/CPU

blktrace -d /dev/sda -o trace -b 4096 -n 8 -w 10

# 检查是否有 dropped 事件
cat /sys/kernel/debug/block/sda/dropped
# 如果 > 0，说明 buffer 不够，需要增大
```

---

## 七、源码文件快速索引

### 按功能查找源码

```bash
# 找 blk_io_trace 结构体定义
grep -rn "struct blk_io_trace" src/linux-5.10/include/

# 找所有 block tracepoint 定义
grep -n "TRACE_EVENT\|DEFINE_EVENT" src/linux-5.10/include/trace/events/block.h

# 找 __blk_add_trace 函数
grep -n "__blk_add_trace" src/linux-5.10/kernel/trace/blktrace.c

# 找 tracepoint 在 blk-mq 中的调用位置
grep -n "trace_block_rq_issue\|trace_block_rq_complete\|trace_block_bio_queue" \
    src/linux-5.10/block/blk-mq.c

# 找 blkparse 的 action 解析逻辑
grep -n "action\|BLK_TC_" src/blktrace/blkparse.c

# 找 btt 的延迟计算
grep -n "q2c\|d2c\|q2d" src/blktrace/btt/trace_complete.c src/blktrace/btt/trace_issue.c

# 找 rwbs 填充逻辑
grep -n "blk_fill_rwbs" src/linux-5.10/block/blk-core.c

# 找 relay buffer 初始化
grep -n "relay_open\|relay_reserve" src/linux-5.10/kernel/trace/blktrace.c

# 找 ioctl 定义
grep -n "BLKTRACESETUP\|BLKTRACESTART\|BLKTRACESTOP" src/blktrace/blktrace_api.h
```
