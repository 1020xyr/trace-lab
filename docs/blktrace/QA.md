# blktrace / blkparse / btt 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: BLK_TC_FS 中 TC 表示什么？BLK_TC_FS 的含义是什么？](#q1-blk_tc_fs-中-tc-表示什么blk_tc_fs-的含义是什么)
- [Q2: 为什么 BLK_TC_QUEUE 和 \_\_BLK_TA_ISSUE 都是入队？](#q2-为什么-blk_tc_queue-和-__blk_ta_issue-都是入队)
- [Q3: rwbs 为 N 时如何知道它具体在干嘛？](#q3-rwbs-为-n-时如何知道它具体在干嘛)
- [Q4: sync、fsync、flush、FUA 有什么差异？](#q4-syncfsyncflush-fua-有什么差异)

---

## Q1: BLK_TC_FS 中 TC 表示什么？BLK_TC_FS 的含义是什么？

**日期：** 2026-07-01  
**场景：** 阅读 `blktrace_api.h` 时产生的疑问  
**相关文件：** `01_blktrace_api.h`

### 回答

**TC = Trace Category（追踪类别）**

`BLK_TC_*` 全称 **Block Trace Category**，是 `action` 字段**高 16 位**的类别属性标志，用来描述事件的"性质"。

```
action (32 位)
┌──────────────────────┬────────────┬──────────────┐
│ 类别标志 (高16位)      │  保留      │ 操作类型(低8位) │
│ BLK_TC_*             │            │ __BLK_TA_*    │
│ "这个事件有什么属性？"  │            │ "发生了什么动作？"│
└──────────────────────┴────────────┴──────────────┘
```

一个事件的 action 可以同时包含多个类别标志，例如一个 FUA 写请求下发：
```
action = BLK_TC_WRITE | BLK_TC_FUA | BLK_TC_FS | BLK_TC_ACT(BLK_TC_ISSUE)
         "写"          "强制写介质"  "文件系统请求"   "下发"
```

**BLK_TC_FS = File System request（文件系统请求）**

标记来自文件系统的正常 I/O 请求，与 `BLK_TC_PC`（SCSI Passthrough Command）区分。

**源码位置：** `src/linux-5.10/kernel/trace/blktrace.c` — `blk_add_trace_rq()`

```c
static void blk_add_trace_rq(struct request *rq, ...)
{
    if (blk_rq_is_passthrough(rq))
        what |= BLK_TC_ACT(BLK_TC_PC);    // SCSI 直通命令（sg_io ioctl 等）
    else
        what |= BLK_TC_ACT(BLK_TC_FS);    // ★ 普通文件系统 I/O
}
```

| 标志 | 全称 | 含义 | 典型场景 |
|------|------|------|---------|
| `BLK_TC_FS` | File System | 来自文件系统的块 I/O | `read()`/`write()` 文件 |
| `BLK_TC_PC` | Passthrough Command | SCSI 直通命令 | `smartctl`、`sg_format` |

blkparse 内部也用这个标志决定解析方式：
```c
// blkparse.c: dump_trace()
if (t->action & BLK_TC_ACT(BLK_TC_PC))
    dump_trace_pc(t, pdi, pci);   // SCSI 格式解析
else
    dump_trace_fs(t, pdi, pci);   // 文件系统格式解析
```

---

## Q2: 为什么 BLK_TC_QUEUE 和 \_\_BLK_TA_ISSUE 都是入队？

**日期：** 2026-07-01  
**场景：** 阅读 `blktrace_api.h` 时混淆了两套命名  
**相关文件：** `01_blktrace_api.h`

### 回答

**它们不是同一个含义。** 混淆的原因是两个不同维度的概念共用相似的命名。

### 两个维度，两套命名

| 名称 | 维度 | 位置 | 含义 |
|------|------|------|------|
| `BLK_TC_QUEUE` | **类别**（高 16 位） | bit 4 | "队列/合并**阶段**" — 包含 Q/G/S/I/M/P/U 多种操作 |
| `__BLK_TA_ISSUE` | **操作**（低 8 位） | 值 7 | "**下发**到设备驱动" — 不是入队，是从队列中**出去** |

### `BLK_TC_QUEUE` 是个大分类

它不表示"入队"，而是**"队列/合并阶段"**。所有发生在"request 进入调度器之前/之中"的事件都归在这个类别下：

```c
// 以下操作的类别都是 BLK_TC_QUEUE：
#define BLK_TA_QUEUE       (__BLK_TA_QUEUE      | BLK_TC_ACT(BLK_TC_QUEUE))  // Q: 入队
#define BLK_TA_BACKMERGE   (__BLK_TA_BACKMERGE  | BLK_TC_ACT(BLK_TC_QUEUE))  // M: 后向合并
#define BLK_TA_FRONTMERGE  (__BLK_TA_FRONTMERGE | BLK_TC_ACT(BLK_TC_QUEUE))  // M: 前向合并
#define BLK_TA_GETRQ       (__BLK_TA_GETRQ      | BLK_TC_ACT(BLK_TC_QUEUE))  // G: 分配 request
#define BLK_TA_SLEEPRQ     (__BLK_TA_SLEEPRQ    | BLK_TC_ACT(BLK_TC_QUEUE))  // S: 等待分配
#define BLK_TA_PLUG        (__BLK_TA_PLUG       | BLK_TC_ACT(BLK_TC_QUEUE))  // P: 插队
#define BLK_TA_UNPLUG_IO   (__BLK_TA_UNPLUG_IO  | BLK_TC_ACT(BLK_TC_QUEUE))  // U: 拔塞
#define BLK_TA_INSERT      (__BLK_TA_INSERT     | BLK_TC_ACT(BLK_TC_QUEUE))  // I: 插入调度器

// 以下属于不同的类别：
#define BLK_TA_ISSUE       (__BLK_TA_ISSUE      | BLK_TC_ACT(BLK_TC_ISSUE))    // D: ★ 下发
#define BLK_TA_COMPLETE    (__BLK_TA_COMPLETE   | BLK_TC_ACT(BLK_TC_COMPLETE)) // C: ★ 完成
#define BLK_TA_REQUEUE     (__BLK_TA_REQUEUE    | BLK_TC_ACT(BLK_TC_REQUEUE))  // R: 重入队
```

**一句话总结：** `BLK_TC_QUEUE` 是"阶段分类"，`__BLK_TA_ISSUE` 是"具体动作"，两者不在同一维度。

---

## Q3: rwbs 为 N 时如何知道它具体在干嘛？

**日期：** 2026-07-02  
**场景：** 在 blkparse 输出中看到 rwbs=N，不知道这些 I/O 在做什么  
**相关文件：** `09_blk_fill_rwbs.c`

### 回答

**N = None（无操作类型 / 未识别的操作）。** 有两种来源：

#### 来源 A：内核版 — 操作码不在常规分类中

```c
// kernel/trace/blktrace.c: blk_fill_rwbs()
switch (op & REQ_OP_MASK) {
case REQ_OP_WRITE:       rwbs[i++] = 'W'; break;
case REQ_OP_DISCARD:     rwbs[i++] = 'D'; break;
case REQ_OP_FLUSH:       rwbs[i++] = 'F'; break;
case REQ_OP_READ:        rwbs[i++] = 'R'; break;
default:                 rwbs[i++] = 'N'; break;
}
```

以下操作码会落入 default 分支：
- `REQ_OP_WRITE_ZEROES` (9) — NVMe Write Zeroes
- `REQ_OP_ZONE_OPEN/CLOSE/FINISH/RESET` (10-17) — ZBD 操作
- `REQ_OP_ZONE_APPEND` (13) — ZBD 追加写

#### 来源 B：用户态版 — bytes=0 且不是写/discard

```c
// blkparse_fmt.c: fill_rwbs()
if (d)        rwbs[i++] = 'D';
else if (w)   rwbs[i++] = 'W';
else if (t->bytes)
              rwbs[i++] = 'R';
else          rwbs[i++] = 'N';   // bytes=0 且不是 W/D
```

### 排查方法

**方法 1：看事件字母 + bytes**

```bash
awk '$7 ~ /^N/{print}' parsed.txt
```

| 事件字母 | N 时的含义 | bytes |
|---------|----------|-------|
| P | Plug（延迟下发） | 0 — 正常 |
| U | Unplug（开始下发） | 0 — 正常 |
| G | Get Request（分配 request） | 0 — 正常 |
| S | Sleep on Request（等待分配） | 0 — 正常 |
| D/C + FN | Flush 命令 | 0 — fsync 时常见 |
| D/C + N | Write Zeroes / ZBD | >0 — NVMe/ZBD 设备 |

**方法 2：看进程名**

```bash
awk '$7 ~ /^N/{print $NF}' parsed.txt | sort | uniq -c | sort -rn
```

**方法 3：通过上下文事件链追踪**

找到同一个 sector 的 Q→P→U→D→C 事件链，P/U 的 N 就是正常控制事件。

### N 事件速查表

| rwbs | 事件 | 实际操作 | 常见吗 |
|------|------|---------|--------|
| N | P | Plug | ✅ 很常见 |
| N | U | Unplug | ✅ 很常见 |
| N | G | Get Request | ✅ 常见 |
| N | S | Sleep on Request | ⚠️ 队列压力大时 |
| FN | D/C | Flush 命令 | ✅ fsync 时常见 |
| N | D/C | Write Zeroes | ⚠️ NVMe 设备 |
| N | D/C | Zone 操作 | ⚠️ ZBD 设备 |

---

## Q4: sync、fsync、flush、FUA 有什么差异？

**日期：** 2026-07-02  
**场景：** 实验 3 中看到 FWS/FN 后产生的疑问  
**相关文件：** `03_kernel_blktrace.c`、`09_blk_fill_rwbs.c`  
**内核源码：** `block/blk-flush.c`、`fs/sync.c`、`fs/block_dev.c`

### 回答

四个概念在不同层次运作：

```
┌──────────────────────────────────────────────────────────────┐
│  用户态（系统调用层）                                          │
│  fsync(fd)          应用程序调用 → 刷盘（数据+元数据）        │
│  O_SYNC 打开文件    每次 write() 都自动刷盘                   │
├──────────────────────────────────────────────────────────────┤
│  Block Layer（块设备层）                                      │
│  REQ_SYNC    标志位：I/O 优先级更高（调度器优待）             │
│  REQ_FUA     标志位：这次写入必须到达持久化介质               │
│  REQ_PREFLUSH 标志位：写入之前先刷设备缓存                    │
│  REQ_OP_FLUSH 操作码：独立的 flush 命令（刷全部缓存）         │
├──────────────────────────────────────────────────────────────┤
│  硬件层（设备）                                               │
│  Flush 命令  → 将设备写缓存中所有数据刷到介质                │
│  FUA 命令    → 单次写入绕过缓存，直接到介质                   │
└──────────────────────────────────────────────────────────────┘
```

#### 1. REQ_SYNC — 同步标志

**本质：** bio/request 上的标志位，不是独立操作。

**作用：** 告诉内核"I/O 优先级高"。调度器给同步请求更高优先级。

**触发：** O_DIRECT → 自动带 REQ_SYNC。

**在 rwbs 中：** 出现 **S**。

**源码：** `include/linux/blk_types.h`
```c
static inline bool op_is_sync(unsigned int op)
{
    return op & (REQ_SYNC | REQ_FUA | REQ_PREFLUSH);
    // ★ FUA 和 PREFLUSH 也被视为"同步"
}
```

#### 2. fsync() — 系统调用

**本质：** 用户态系统调用，要求将文件的所有脏数据刷到磁盘。

**内核调用链：**
```
fsync(fd)
  → do_fsync()                        // fs/sync.c
  → vfs_fsync()                       // fs/sync.c
  → file->f_op->fsync()              // 调用文件系统的 fsync 实现
      │
      ├── 块设备: blkdev_fsync()      // fs/block_dev.c
      │     ├── file_write_and_wait_range()   // 先写脏页
      │     └── blkdev_issue_flush()          // ★ 再发 flush
      │
      └── ext4: ext4_sync_file()      // fs/ext4/fsync.c
            ├── jbd2_complete_transaction()    // 提交 journal
            └── blkdev_issue_flush()           // flush
```

**`blkdev_issue_flush()` 实现：**
```c
int blkdev_issue_flush(struct block_device *bdev, gfp_t gfp_mask)
{
    struct bio *bio = bio_alloc(gfp_mask, 0);     // 空 bio
    bio->bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;    // ★ PREFLUSH
    ret = submit_bio_wait(bio);                    // 同步等待完成
    bio_put(bio);
    return ret;
}
```

→ **fsync 在块设备上 = 写脏数据 + 发一个 PREFLUSH bio**

**在 rwbs 中：** 产生 FWS 或 FN 序列。

#### 3. Flush — 刷缓存操作

**本质：** 块设备层的操作，将设备写缓存中的所有数据刷到介质。

**两种形式：**

| 形式 | 含义 | rwbs |
|------|------|------|
| `REQ_OP_FLUSH` | 独立的 flush 命令（不携带数据） | **FN**（F 是操作字符，N 因 bytes=0） |
| `REQ_PREFLUSH` | 附加在写请求上的"前缀 flush" | **F** 在首位，如 **FWS** |

**内核如何分解 PREFLUSH/FUA（blk-flush.c 注释）：**

```
设备有写缓存 + 支持 FUA：
  PREFLUSH 请求 → 先发 FLUSH → 再执行 DATA
  FUA 请求     → 直接带 FUA 标志执行 DATA（★ 无需额外 flush）

设备有写缓存 + 不支持 FUA：
  PREFLUSH 请求 → 先发 FLUSH → 再执行 DATA
  FUA 请求     → 执行 DATA → 再发 FLUSH（POSTFLUSH）
  ★ 性能更差！每次 FUA 写变成 3 步

设备无写缓存：
  PREFLUSH/FUA → 无意义，直接执行 DATA
```

#### 4. FUA — Force Unit Access

**本质：** bio/request 上的标志位，表示"这次写入直接到持久化介质"。

**与 Flush 的核心区别：**

```
Flush：  "把缓存里所有脏数据都刷到介质"
  → 影响范围：整个设备缓存
  → 开销：取决于缓存中有多少脏数据
  → 类比：把整个垃圾桶倒掉

FUA：   "把这一次写的直接放到介质上，不走缓存"
  → 影响范围：仅当前这一次写
  → 开销：仅当前写的数据量
  → 类比：这次扔的垃圾直接送到垃圾站，不放进垃圾桶
```

**触发：** ext4/XFS 写 journal → REQ_FUA。O_SYNC 打开文件 → write() 自动带 REQ_FUA。

**在 rwbs 中：** F 出现在操作字符**后面**（如 **WF**）。

#### 对比总结

| 概念 | 层次 | 本质 | 影响范围 | rwbs | 性能开销 |
|------|------|------|---------|------|---------|
| **REQ_SYNC** | 标志位 | "这个 I/O 是同步的" | 单个 I/O | **S** | 低 |
| **fsync()** | 系统调用 | "把这个文件刷盘" | 一个文件所有脏数据 | FWS/FN | 高 |
| **Flush** | 块设备操作 | "刷设备全部缓存" | 整个设备 | **FN** 或首位 **F** | 高 |
| **FUA** | 标志位 | "这次写直接到介质" | 单次写 | 操作后的 **F** | 中 |

#### 实际场景中的组合

```
ext4 journal commit:
  写 journal 数据  →  WF (Write + FUA)
  提交 journal     →  FN (Flush + None)
  → blkparse 中看到 WF 和 FN 交替出现

O_SYNC write:
  写数据           →  WSF (Write + Sync + FUA)
  → 一次 I/O 搞定

fsync 到块设备:
  写回脏数据       →  WS（可能多个）
  发 flush         →  FWS 或 FN
  → 看到 WS...WS → FWS/FN 的模式

dd oflag=sync:
  每次 write()     →  FWSF (Preflush + Write + Sync + FUA)
  → 最重型的持久化保证
```

---

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
