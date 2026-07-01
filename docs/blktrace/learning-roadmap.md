# blktrace + blkparse + btt 学习路线

> 从 tracepoint 到 relay buffer，从二进制文件到延迟统计

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 源码阅读路线图](#2-源码阅读路线图)
  - [Step 1: 数据结构 — blk_io_trace](#step-1-数据结构--blk_io_trace)
  - [Step 2: Action 标志位 — Q/D/C 的含义](#step-2-action-标志位--qdc-的含义)
  - [Step 3: 内核 tracepoint — hook 在哪里](#step-3-内核-tracepoint--hook-在哪里)
  - [Step 4: relay buffer — 数据如何从内核到用户态](#step-4-relay-buffer--数据如何从内核到用户态)
  - [Step 5: 用户态 blktrace 源码](#step-5-用户态-blktrace-源码)
  - [Step 6: blkparse 源码 — 如何解析二进制](#step-6-blkparse-源码--如何解析二进制)
  - [Step 7: btt 源码 — 延迟如何计算](#step-7-btt-源码--延迟如何计算)
- [3. FUA 问题深度分析](#3-fua-问题深度分析)
- [4. 动手实验清单](#4-动手实验清单)
- [5. 关键源码文件索引](#5-关键源码文件索引)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     用户态工具                               │
│  blktrace(采集)  →  blkparse(解析)  →  btt(统计分析)         │
├─────────────────────────────────────────────────────────────┤
│                     内核接口层                               │
│  ioctl(BLKTRACESETUP/START/STOP)  +  debugfs + relay buffer │
├─────────────────────────────────────────────────────────────┤
│                     内核 Block Layer                         │
│  tracepoint: block_rq_issue / block_rq_complete / ...       │
│  __blk_add_trace() → blk_io_trace → per-CPU relay subbuffer │
└─────────────────────────────────────────────────────────────┘
```

### 数据流全路径

```
 用户进程 write()
     │
     ▼
 VFS → 文件系统 → 生成 bio
     │
     ▼
 submit_bio()
     │
     ▼
 ┌── trace_block_bio_queue() ──────────────────┐
 │  __blk_add_trace(action=BLK_TC_QUEUE)        │  ← Q 事件
 └─────────────────────────────────────────────┘
     │
     ▼
 blk_mq_submit_bio() → 分配 request → 加入调度队列
     │
     ├── trace_block_rq_insert()  → I 事件
     │
     ▼
 调度器（mq-deadline / BFQ / kyber）选择 request
     │
     ▼
 ┌── trace_block_rq_issue() ───────────────────┐
 │  __blk_add_trace(action=BLK_TC_ISSUE)        │  ← D 事件
 └─────────────────────────────────────────────┘
     │
     ▼
 设备驱动 → NVMe/SCSI → 硬件执行 I/O
     │
     ▼
 中断回调
     │
     ▼
 ┌── trace_block_rq_complete() ────────────────┐
 │  __blk_add_trace(action=BLK_TC_COMPLETE)     │  ← C 事件
 └─────────────────────────────────────────────┘
     │
     ▼
 relay buffer (per-CPU subbuffer)
     │
     ▼
 debugfs 文件 (/sys/kernel/debug/block/<dev>/traceN)
     │
     ▼
 blktrace 用户态进程 (mmap + poll)
     │
     ▼
 磁盘二进制文件 (sda.blktrace.0, sda.blktrace.1, ...)
     │
     ▼
 blkparse (合并、排序、格式化)
     │
     ▼
 btt (配对 Q/D/C 事件，计算延迟统计)
```

---

## 2. 源码阅读路线图

### Step 1: 数据结构 — blk_io_trace

**目标：** 理解每个 I/O 事件记录的本质结构

**文件位置：**
- 内核定义：`src/linux-5.10/include/uapi/linux/blktrace_api.h`
- 用户态副本：`src/blktrace/blktrace_api.h`

**阅读要点：**

```c
struct blk_io_trace {
    __u32 device;     // 设备号 (major:minor)，如 8,0 = /dev/sda
    __u16 cpu;        // 事件发生在哪个 CPU
    __u16 error;      // 错误码，0 = 成功
    __u16 pdu_len;    // PDU 附加数据长度（如 SCSI CDB）
    __s16 pdu_len_pad;// 对齐填充
    __u16 pid;        // 发起 I/O 的进程 PID
    __u32 action;     // ★ 核心字段：事件类型（位掩码）
    __u32 bytes;      // I/O 大小（字节）
    __u64 time;       // 时间戳（纳秒）
    __u64 sector;     // 起始扇区号（512字节扇区）
};
```

**思考题：**
1. 为什么 `action` 是 `__u32` 而不仅仅是 `__u8`？（答：因为是位掩码，低 16 位是 action 类别，高 16 位可以是额外标志）
2. `sector` 的单位是什么？（答：512 字节扇区，即使设备是 4K 扇区）
3. 为什么需要 `pdu_len`？（答：某些请求（如 SCSI）有额外的命令数据附加在结构体后面）

**实操：** 打开文件，搜索 `struct blk_io_trace`，对照字段理解。

---

### Step 2: Action 标志位 — Q/D/C 的含义

**目标：** 理解事件类型的位掩码机制，以及为什么 FUA 容易被忽略

**文件位置：**
- `src/linux-5.10/include/uapi/linux/blktrace_api.h`

**阅读要点：**

```c
// 低 16 位：action 类别（事件发生在哪个阶段）
enum {
    BLK_TC_READ     = 1 << 0,   // 读操作
    BLK_TC_WRITE    = 1 << 1,   // 写操作
    BLK_TC_FLUSH    = 1 << 2,   // flush 操作
    BLK_TC_SYNC     = 1 << 3,   // 同步 I/O
    BLK_TC_QUEUE    = 1 << 4,   // ★ Q：入队/合并
    BLK_TC_REQUEUE  = 1 << 5,   // 重新入队
    BLK_TC_ISSUE    = 1 << 6,   // ★ D：下发到设备驱动
    BLK_TC_COMPLETE = 1 << 7,   // ★ C：I/O 完成
    BLK_TC_FS       = 1 << 8,   // 文件系统请求
    BLK_TC_PC       = 1 << 9,   // 块设备命令（SCSI PC）
    BLK_TC_NOTIFY   = 1 << 10,  // 特殊消息
    BLK_TC_AHEAD    = 1 << 11,  // 预读
    BLK_TC_META     = 1 << 12,  // 元数据
    BLK_TC_DISCARD  = 1 << 13,  // TRIM/discard
    BLK_TC_DRV_DATA = 1 << 14,  // 驱动私有数据
    BLK_TC_FUA      = 1 << 15,  // ★ Force Unit Access（你忽略的！）
};

// 高 16 位：action 通知类型
#define BLK_TC_ACT(action)  ((action) << BLK_TC_SHIFT)
```

**关键认知：`action` 是位掩码，不是单值枚举！**

一个带 FUA 的写请求下发事件，其 action 值为：
```
BLK_TC_WRITE | BLK_TC_FUA | BLK_TC_ISSUE = 0x8002 | 0x0040 = 0x8042
```

如果只检查 `action == BLK_TC_ISSUE`（即 `action == 0x0040`），就会遗漏这个事件。
正确做法是 `action & BLK_TC_ISSUE`（位运算）。

**blkparse 输出的字母映射：**

| 字母 | 标志位 | 含义 |
|------|--------|------|
| Q | BLK_TC_QUEUE | 入队 |
| D | BLK_TC_ISSUE | 下发 |
| C | BLK_TC_COMPLETE | 完成 |
| G | (notify) | get_request |
| I | (notify) | insert |
| M | (notify) | merge |
| F | BLK_TC_FLUSH / BLK_TC_FUA | flush 或 FUA |
| R | BLK_TC_READ | 读 |
| W | BLK_TC_WRITE | 写 |

**思考题：**
1. 为什么 FUA 和 FLUSH 共用字母 `F`？它们有什么区别？
2. 在 blkparse 输出中如何区分 FUA 和 FLUSH？（提示：看 rwbs 字段的其他位）

---

### Step 3: 内核 tracepoint — hook 在哪里

**目标：** 找到每个事件在内核代码中的具体 hook 位置

**文件位置：**
- tracepoint 定义：`src/linux-5.10/include/trace/events/block.h`
- blktrace hook：`src/linux-5.10/kernel/trace/blktrace.c`
- 调用点（blk-mq）：`src/linux-5.10/block/blk-mq.c`

#### 3.1 先读 tracepoint 定义

```bash
# 在 linux-5.10 源码中搜索 block tracepoint
grep -n "DECLARE_EVENT\|DEFINE_EVENT\|TRACE_EVENT" \
    src/linux-5.10/include/trace/events/block.h
```

重点关注：
- `TRACE_EVENT(block_rq_issue, ...)` → D 事件
- `TRACE_EVENT(block_rq_complete, ...)` → C 事件
- `TRACE_EVENT(block_bio_queue, ...)` → Q 事件
- `TRACE_EVENT(block_rq_insert, ...)` → I 事件

#### 3.2 看 __blk_add_trace() 实现

```bash
# 找到核心函数
grep -n "__blk_add_trace\|blk_add_trace" \
    src/linux-5.10/kernel/trace/blktrace.c
```

**关键代码路径：**

```c
// kernel/trace/blktrace.c
static void __blk_add_trace(struct blk_trace *bt, sector_t sector,
                            int bytes, int opf, u32 what, int error,
                            int pdu_len, void *pdu_data)
{
    struct blk_io_trace *t;
    unsigned long flags = 0;
    u32 pid;

    if (unlikely(!bt))
        return;

    // 1. 获取当前进程的 PID
    pid = tsk->tgid;

    // 2. 从 relay buffer 预留空间（per-CPU，无锁）
    t = relay_reserve(bt->rchan, sizeof(*t) + pdu_len, NULL);
    if (!t)
        return;

    // 3. 填充 blk_io_trace
    t->device  = bt->dev;
    t->cpu     = raw_smp_processor_id();
    t->pdu_len = pdu_len;
    t->pid     = pid;
    t->action  = what;       // ★ 这就是事件类型
    t->bytes   = bytes;
    t->error   = error;
    t->time    = ktime_to_ns(ktime_get());
    t->sector  = sector;

    // 4. 如果有 PDU 数据，拷贝到后面
    if (pdu_len)
        memcpy((void *)t + sizeof(*t), pdu_data, pdu_len);
}
```

#### 3.3 追踪调用链

```bash
# 在 blk-mq.c 中找 tracepoint 调用点
grep -n "trace_block_rq_issue\|trace_block_rq_complete\|trace_block_bio_queue\|trace_block_rq_insert" \
    src/linux-5.10/block/blk-mq.c
```

预期找到：
```
block/blk-mq.c:
  blk_mq_dispatch_request()  → trace_block_rq_issue()     // D 事件
  blk_mq_complete_request()  → trace_block_rq_complete()   // C 事件

block/blk-core.c:
  submit_bio_noacct()        → trace_block_bio_queue()      // Q 事件
  blk_mq_sched_insert_request() → trace_block_rq_insert()   // I 事件
```

**实操任务：**
1. 打开 `include/trace/events/block.h`，找到 `block_rq_issue` 的 TRACE_EVENT 定义
2. 看它的 `TP_ARGS`、`TP_STRUCT__entry`、`TP_fast_assign`，理解哪些数据被记录
3. 在 `block/blk-mq.c` 中找到调用 `trace_block_rq_issue()` 的位置，理解在什么时机调用

---

### Step 4: relay buffer — 数据如何从内核到用户态

**目标：** 理解内核如何高效地把大量 trace 事件传递到用户态

**文件位置：**
- relay 实现：`src/linux-5.10/kernel/relay.c`
- relay 文档：`src/linux-5.10/Documentation/filesystems/relay.rst`
- blktrace 中的使用：`src/linux-5.10/kernel/trace/blktrace.c`（搜索 `relay_open`、`relay_reserve`）

**核心概念：**

```
relay channel = N 个 per-CPU buffer
每个 buffer = M 个 subbuffer（环形缓冲）

┌─── CPU 0 ───────────────────────┐
│  subbuffer[0] [1] [2] [3]       │  ← 4 个 subbuffer，每个 512KB
│         ↑ write    ↑ read       │
└─────────────────────────────────┘
┌─── CPU 1 ───────────────────────┐
│  subbuffer[0] [1] [2] [3]       │
│         ↑ write    ↑ read       │
└─────────────────────────────────┘

- 写入：relay_reserve() 在当前 CPU 的当前 subbuffer 中预留空间
- subbuffer 满 → 切换到下一个 subbuffer → 通知用户态
- 用户态：通过 mmap 或 read 从 debugfs 文件中读取
```

**关键参数（通过 BLKTRACESETUP ioctl 传入）：**
```c
#define BUF_SIZE  (512 * 1024)   // 每个 subbuffer 512KB
#define BUF_NR    (4)            // 每个 CPU 4 个 subbuffer
// 每个 CPU 的总缓冲 = 512KB × 4 = 2MB
```

**思考题：**
1. 为什么用 per-CPU buffer 而不是全局 buffer？（答：避免锁竞争，减少追踪开销）
2. 如果 subbuffer 满了但用户态没来得及读，会发生什么？（答：覆盖旧数据，blktrace 会报告 overrun）
3. relay 和 ring_buffer（ftrace 用的）有什么区别？

---

### Step 5: 用户态 blktrace 源码

**目标：** 理解 blktrace 命令如何与内核交互

**文件位置：** `src/blktrace/blktrace.c`

#### 5.1 文件结构一览

```bash
ls -la src/blktrace/*.c src/blktrace/*.h
```

```
blktrace.c        — 主程序，包含 main()
blktrace.h        — 内部头文件
blktrace_api.h    — 用户态 API（blk_io_trace、ioctl 定义）
blkparse.c        — blkparse 工具
blkparse_fmt.c    — 格式化输出逻辑
blkiomon.c        — 实时监控工具
btt/              — btt 分析工具目录
```

#### 5.2 main() 流程分析

```bash
# 找到 main 函数
grep -n "int main" src/blktrace/blktrace.c
```

**main() 执行流程：**

```
main()
 │
 ├── 1. 解析命令行参数
 │      getopt_long()
 │      -d device, -b buf_size, -n buf_nr,
 │      -a action_mask, -w timeout, -p pid
 │
 ├── 2. 对每个设备调用 setup_dev()
 │      ├── 打开块设备 fd
 │      ├── 填充 struct blk_user_trace_setup
 │      │     .act_mask  = 要追踪的事件类型
 │      │     .buf_size  = relay subbuffer 大小
 │      │     .buf_nr    = relay subbuffer 数量
 │      │     .pid       = 过滤 PID（0=全部）
 │      └── ioctl(fd, BLKTRACESETUP, &setup)
 │
 ├── 3. 启动追踪
 │      ioctl(fd, BLKTRACESTART)
 │
 ├── 4. 主循环 do_trace()
 │      ├── 打开 per-CPU relay 文件
 │      │     /sys/kernel/debug/block/<dev>/trace0
 │      │     /sys/kernel/debug/block/<dev>/trace1
 │      │     ...
 │      ├── mmap 映射 relay buffer
 │      ├── poll() 等待新数据
 │      └── 读取事件 → 写入磁盘文件
 │            sda.blktrace.0
 │            sda.blktrace.1
 │            ...
 │
 └── 5. 停止追踪
        ioctl(fd, BLKTRACESTOP)
        ioctl(fd, BLKTRACETEARDOWN)
```

#### 5.3 关键 ioctl

```c
// blktrace_api.h
#define BLKTRACESETUP   _IOWR(0x12, 115, struct blk_user_trace_setup)
#define BLKTRACESTART   _IO(0x12, 116)
#define BLKTRACESTOP    _IO(0x12, 117)
#define BLKTRACETEARDOWN _IO(0x12, 118)
```

**实操任务：**
1. 打开 `src/blktrace/blktrace.c`，找到 `main()` 函数
2. 跟踪 `setup_dev()` 的调用，看 `blk_user_trace_setup` 如何填充
3. 找到 `do_trace()` 或主循环，看 `mmap()` 和 `poll()` 的使用
4. 看数据如何从 relay buffer 写入到磁盘文件

---

### Step 6: blkparse 源码 — 如何解析二进制

**目标：** 理解 blkparse 如何将二进制 blktrace 文件转换为可读文本

**文件位置：**
- `src/blktrace/blkparse.c` — 主解析逻辑
- `src/blktrace/blkparse_fmt.c` — 格式化输出

#### 6.1 blkparse 核心流程

```
blkparse 输入：
  sda.blktrace.0  (CPU 0 的二进制事件流)
  sda.blktrace.1  (CPU 1 的二进制事件流)
  ...

blkparse 处理：
  1. 打开所有 per-CPU 文件
  2. 逐个读取 blk_io_trace 结构
  3. 按时间戳归并排序（合并多个 CPU 的事件流）
  4. 对每个事件，解析 action 位掩码
  5. 按格式字符串输出

blkparse 输出（默认格式）：
  8,0    1        1     0.000000000  1234  Q   R 12345678 + 8 [fio]
```

#### 6.2 action 解析逻辑

```bash
# 搜索 action 字段的解析代码
grep -n "action\|BLK_TC_\|act_to_str" src/blkparse.c
```

blkparse 会检查 action 的每一位，组合出完整的描述：
```
action = 0x8042
  → bit 1 (BLK_TC_WRITE)  → "W"
  → bit 6 (BLK_TC_ISSUE)  → "D"
  → bit 15 (BLK_TC_FUA)   → "F"
  → 输出：D W F（下发 + 写 + FUA）
```

#### 6.3 输出格式控制

blkparse 的 `-f` 选项可以自定义输出格式：
```bash
# 默认格式
blkparse -i sda -f "%D %c %n %T %p %a %m\n"

# 各字段含义：
#   %D = 设备号
#   %c = CPU
#   %n = 序列号
#   %T = 时间戳
#   %p = PID
#   %a = action（Q/D/C 等）
#   %m = 进程名
```

**实操任务：**
1. 打开 `src/blktrace/blkparse.c`，找到主循环
2. 找到读取 `blk_io_trace` 结构的代码
3. 找到 action 解析和格式化输出的代码
4. 理解多 CPU 事件流的归并排序逻辑

---

### Step 7: btt 源码 — 延迟如何计算

**目标：** 理解 btt 如何配对事件、计算 Q2C/Q2D/D2C 延迟

**文件位置：** `src/blktrace/btt/`

#### 7.1 btt 目录结构

```bash
ls src/blktrace/btt/
```

```
btt/
├── args.c          — 命令行参数解析
├── devmap.c        — 设备映射（DM/LVM 支持）
├── dip.c           — 延迟间隔处理
├── globals.c       — 全局变量
├── iolat.c         — I/O 延迟计算（★ 核心）
├── misc.c          — 辅助函数
├── output.c        — 输出格式化
├── plat.c          — 平台相关
├── proc.c          — 进程信息
├── trace.c         — trace 事件处理
├── trace_complete.c — 完成事件处理
├── trace_im.c      — insert/merge 事件处理
├── trace_issue.c   — 下发事件处理
├── trace_queue.c   — 入队事件处理
├── trace_requeue.c — 重入队事件处理
└── btt.h           — 头文件
```

#### 7.2 延迟计算原理

**核心思想：通过 device + sector + pid 配对同一 I/O 的 Q/D/C 事件**

```
Q 事件：device=8,0  sector=12345  pid=1234  time=T1
                                        ↓
D 事件：device=8,0  sector=12345  pid=1234  time=T2
                                        ↓
C 事件：device=8,0  sector=12345  pid=0     time=T3
        (注意：完成事件的 pid 通常是 0，因为在中断上下文中完成)
```

**延迟公式：**
```
Q2D = T2 - T1    (在队列/调度器中等待的时间)
D2C = T3 - T2    (设备处理时间)
Q2C = T3 - T1    (端到端总延迟 = Q2D + D2C)
```

#### 7.3 诊断价值

| 指标 | 正常范围 | 异常含义 |
|------|---------|---------|
| Q2D avg | < 1ms | 调度器/队列压力 |
| D2C avg | 取决于设备 | 设备性能问题 |
| Q2C avg | Q2D + D2C | 总体 I/O 延迟 |
| Q2D/Q2C 比 | < 30% | >50% 说明软件层是瓶颈 |
| Q2C max | 无异常尖峰 | 长尾延迟 → 检查 retry/timeout |

**btt 输出文件后缀说明：**
- `q2c.dat` — Q2C 延迟数据
- `d2c.dat` — D2C 延迟数据
- `aqd.dat` — 平均队列深度
- `iops.dat` — IOPS 统计
- `bno.dat` — 块号分布
- `spt.dat` — 每追踪扇区数

**实操任务：**
1. 打开 `src/blktrace/btt/trace_queue.c`，看 Q 事件如何被记录
2. 打开 `src/blktrace/btt/trace_issue.c`，看 D 事件如何与 Q 配对
3. 打开 `src/blktrace/btt/trace_complete.c`，看 C 事件如何计算 D2C
4. 打开 `src/blktrace/btt/iolat.c`，看延迟统计的计算逻辑

---

## 3. FUA 问题深度分析

### 什么是 FUA

**FUA（Force Unit Access）**：强制将数据直接写入持久化介质，绕过设备写缓存。

```
普通写入：               FUA 写入：
┌──────┐                ┌──────┐
│ 数据  │                │ 数据  │
└──┬───┘                └──┬───┘
   │                       │
   ▼                       ▼
┌──────┐                ┌──────┐  ← 直接跳过缓存
│ 写缓存│                │ 介质  │
└──┬───┘                └──────┘
   │
   ▼ (异步刷盘)
┌──────┐
│ 介质  │
└──────┘
```

### FUA 在内核中的触发路径

```
文件系统 (ext4/XFS)
  │ 写 journal 或 metadata 时设置 REQ_FUA
  ▼
bio->bi_opf |= REQ_FUA
  │
  ▼
blk_mq_dispatch_request()
  │ 检查 REQ_FUA 标志
  │
  ├── 设备支持 FUA → 直接带 FUA 标志下发
  │   trace: action |= BLK_TC_FUA
  │
  └── 设备不支持 FUA → 拆分为：
      1. 先发 FLUSH（刷缓存）
      2. 再发 WRITE
      3. 再发 FLUSH
      （性能更差！三次 I/O）
```

### 如何检测 FUA

```bash
# 在 blkparse 输出中，FUA 请求的 rwbs 字段包含 'F'
# 例如：
#   8,0  0  1  0.001234  1234  D  WF  12345 + 8 [fio]
#                                ^^
#                                └─ F = FUA

# 用 blkparse 过滤 FUA 事件
blkparse -i trace -f "%D %T %a %m\n" | grep "F"

# 用 btt 分析时，FUA 写和普通写的 D2C 差异
# FUA 的 D2C 通常比普通写大很多（因为要等到介质确认）
```

### 你的问题根因

之前忽略 FUA 的可能场景：
1. 只看 Q/D/C 事件类型，没检查 action 中的 BLK_TC_FUA 位
2. 不知道 FUA 写请求的 D2C 比普通写大很多
3. 没有意识到文件系统（如 ext4 journal）默认使用 FUA

---

## 4. 动手实验清单

### 实验 1: 基础采集与解析

```bash
#!/bin/bash
# experiments/blktrace/exp1_basic.sh

# 1. 启动 blktrace（后台运行，追踪 10 秒）
blktrace -d /dev/sda -o trace -w 10 &

# 2. 生成 I/O 负载
sleep 1
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --numjobs=1 --size=100M --filename=/tmp/testfile \
    --runtime=8 --time_based

# 3. 等待 blktrace 结束
wait

# 4. 解析
blkparse -i trace -o parsed.txt

# 5. 查看前 20 行
head -20 parsed.txt

# 6. 生成二进制文件供 btt 分析
blkparse -i trace -d trace.bin

# 7. btt 分析
btt -i trace.bin -o btt_summary.txt

# 8. 查看 btt 结果
cat btt_summary.txt
```

### 实验 2: FUA vs 非 FUA 对比

```bash
#!/bin/bash
# experiments/blktrace/exp2_fua.sh

# --- 测试 1: 普通 direct write ---
blktrace -d /dev/sda -o nofua -w 10 &
sleep 1
fio --name=nofua --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --numjobs=1 --size=50M --filename=/tmp/test_nofua \
    --runtime=8 --time_based
wait
blkparse -i nofua -o nofua_parsed.txt
blkparse -i nofua -d nofua.bin
btt -i nofua.bin -o nofua_btt.txt

# --- 测试 2: 带 FUA 的 write ---
blktrace -d /dev/sda -o fua -w 10 &
sleep 1
fio --name=fua --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --numjobs=1 --size=50M --filename=/tmp/test_fua \
    --runtime=8 --time_based --dsync=1  # dsync 触发 FUA
wait
blkparse -i fua -o fua_parsed.txt
blkparse -i fua -d fua.bin
btt -i fua.bin -o fua_btt.txt

# --- 对比 ---
echo "=== 非 FUA D2C ==="
grep "D2C" nofua_btt.txt
echo "=== FUA D2C ==="
grep "D2C" fua_btt.txt
```

### 实验 3: 观察完整 I/O 生命周期

```bash
#!/bin/bash
# experiments/blktrace/exp3_lifecycle.sh

# 追踪所有事件类型
blktrace -d /dev/sda -o full -a queue -a issue -a complete \
    -a requeue -a notify -w 5 &
sleep 1

# 生成简单的顺序读
dd if=/dev/sda of=/dev/null bs=4k count=100

wait

# 解析并查看完整生命周期
blkparse -i full -o full_parsed.txt

# 找到某个特定 sector 的 Q→I→D→C 全过程
grep "12345" full_parsed.txt  # 替换为实际扇区号
```

### 实验 4: 验证内核源码中的 tracepoint

```bash
#!/bin/bash
# experiments/blktrace/exp4_kernel_verify.sh

# 查看内核中 block tracepoint 的状态
cat /sys/kernel/debug/tracing/available_events | grep block

# 通过 ftrace 验证 blktrace 使用的 tracepoint
echo 1 > /sys/kernel/debug/tracing/events/block/block_rq_issue/enable
echo 1 > /sys/kernel/debug/tracing/events/block/block_rq_complete/enable

# 生成 I/O
dd if=/dev/sda of=/dev/null bs=4k count=10

# 查看 ftrace 输出
cat /sys/kernel/debug/tracing/trace | head -20

# 关闭
echo 0 > /sys/kernel/debug/tracing/events/block/block_rq_issue/enable
echo 0 > /sys/kernel/debug/tracing/events/block/block_rq_complete/enable
```

---

## 5. 关键源码文件索引

### 内核侧（src/linux-5.10/）

| 文件 | 关注点 |
|------|--------|
| `include/uapi/linux/blktrace_api.h` | blk_io_trace 结构、BLK_TC_* 标志位、ioctl 定义 |
| `kernel/trace/blktrace.c` | `__blk_add_trace()`、relay buffer 初始化、tracepoint 回调注册 |
| `include/trace/events/block.h` | 所有 block tracepoint 的 TRACE_EVENT 定义 |
| `block/blk-mq.c` | `blk_mq_dispatch_request()`（D）、`blk_mq_complete_request()`（C）|
| `block/blk-core.c` | `submit_bio_noacct()`（Q）、request 分配 |
| `block/blk-mq-sched.c` | 调度器 insert（I）、merge（M）|
| `kernel/relay.c` | relay buffer 实现：`relay_open()`、`relay_reserve()`、`relay_switch_subbuf()` |

### 用户态工具（src/blktrace/）

| 文件 | 关注点 |
|------|--------|
| `blktrace.c` | `main()`、`setup_dev()`、`do_trace()`、relay mmap |
| `blktrace.h` | 内部结构、BUF_SIZE、BUF_NR |
| `blktrace_api.h` | 用户态 API 副本 |
| `blkparse.c` | 主解析循环、多 CPU 归并排序、action 解析 |
| `blkparse_fmt.c` | 格式化字符串处理 |
| `btt/iolat.c` | 延迟统计核心计算 |
| `btt/trace_queue.c` | Q 事件处理 |
| `btt/trace_issue.c` | D 事件处理、Q→D 配对 |
| `btt/trace_complete.c` | C 事件处理、D→C 配对 |

### 推荐阅读顺序

```
1. blktrace_api.h          → 理解数据结构和 API
2. include/trace/events/block.h → 理解 tracepoint 定义
3. kernel/trace/blktrace.c → 理解 __blk_add_trace 和 relay
4. blktrace/blktrace.c     → 理解用户态采集逻辑
5. blktrace/blkparse.c     → 理解解析逻辑
6. blktrace/btt/iolat.c    → 理解延迟计算
7. block/blk-mq.c          → 理解 tracepoint 在 I/O 路径中的位置
```

---

## 参考资源

- [blktrace GitHub 镜像](https://github.com/sdsc/blktrace)
- [内核 blktrace.c 源码](https://github.com/torvalds/linux/blob/master/kernel/trace/blktrace.c)
- [blktrace(8) man page](https://man7.org/linux/man-pages/man8/blktrace.8.html)
- [blkparse(1) man page](https://man7.org/linux/man-pages/man1/blkparse.1.html)
- [btt(1) man page](https://man7.org/linux/man-pages/man1/btt.1.html)
- [blktrace User Guide (PDF)](https://allstar.jhuapl.edu/repo/p1/amd64/blktrace/doc/blktrace.pdf)
- [btt User Guide (PDF)](https://allstar.jhuapl.edu/repo/p1/amd64/blktrace/doc/btt.pdf)
- [内核 relay 接口文档](https://docs.kernel.org/filesystems/relay.html)
- [Block I/O Layer Tracing (HP 演讲 PDF)](https://www.mimuw.edu.pl/~lichota/09-10/Optymalizacja-open-source/Materialy/10%20-%20Dysk/gelato_ICE06apr_blktrace_brunelle_hp.pdf)
- [Deep in blktrace (知乎)](https://zhuanlan.zhihu.com/p/355548200)
- [LWN: blk queue io tracing support](https://lwn.net/Articles/148761/)
