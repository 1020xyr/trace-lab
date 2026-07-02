# blktrace / blkparse / btt 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: BLK_TC_FS 中 TC 表示什么？BLK_TC_FS 的含义是什么？](#q1-blk_tc_fs-中-tc-表示什么blk_tc_fs-的含义是什么)
- [Q2: 为什么 BLK_TC_QUEUE 和 \_\_BLK_TA_ISSUE 都是入队？](#q2-为什么-blk_tc_queue-和-__blk_ta_issue-都是入队)
- [Q3: rwbs 为 N 时如何知道它具体在干嘛？](#q3-rwbs-为-n-时如何知道它具体在干嘛)
- [Q4: sync、fsync、flush、FUA 有什么差异？](#q4-syncfsyncflush-fua-有什么差异)
- [Q5: 操作类型用 blk_fill_rwbs 解析，事件类型（D/C/Q）在哪解析？](#q5-操作类型用-blk_fill_rwbs-解析事件类型dcq在哪解析)
- [Q6: setup_buts() 中 buts 是什么的缩写？](#q6-setup_buts-中-buts-是什么的缩写)
- [Q7: 为什么 per-CPU 设计？relay buffer 为什么还可能溢出？](#q7-为什么-per-cpu-设计relay-buffer-为什么还可能溢出)

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

## Q5: 操作类型用 blk_fill_rwbs 解析，事件类型（D/C/Q）在哪解析？

**日期：** 2026-07-02  
**场景：** 理解了 rwbs 字段（R/W/S/F/N）由 `blk_fill_rwbs()` 生成后，想知道事件字母（Q/D/C/G/I/M/P/U）的解析在哪  
**相关文件：** `05_blkparse.c`  
**源码位置：** `src/blktrace/blkparse.c` 第 1539-1620 行 — `dump_trace_fs()`

### 回答

action 字段（32 位）被 blkparse **拆成两个独立部分**分别解析：

```
action (32 位)
┌───────────────────────────┬──────────────────────────┐
│ 高 16 位: 类别标志 (BLK_TC_*) │ 低 16 位: 操作类型 (__BLK_TA_*) │
│ → fill_rwbs() 解析         │ → dump_trace_fs() 解析     │
│ → 输出 rwbs (R/W/S/F/M/A/N)│ → 输出事件字母 (Q/D/C/G/I/..)│
│ → blkparse_fmt.c:53        │ → blkparse.c:1539          │
└───────────────────────────┴──────────────────────────┘
```

### 核心代码：`dump_trace_fs()`

```c
// blkparse.c:1539
static void dump_trace_fs(struct blk_io_trace *t, struct per_dev_info *pdi,
                          struct per_cpu_info *pci)
{
    int w = (t->action & BLK_TC_ACT(BLK_TC_WRITE)) != 0;
    int act = t->action & 0xffff;    // ★ 提取低 16 位

    switch (act) {
        case __BLK_TA_QUEUE:        log_queue(pci, t, "Q");          break;
        case __BLK_TA_INSERT:       log_insert(pdi, pci, t, "I");    break;
        case __BLK_TA_BACKMERGE:    log_merge(pdi, pci, t, "M");     break;
        case __BLK_TA_FRONTMERGE:   log_merge(pdi, pci, t, "F");     break;
        case __BLK_TA_GETRQ:        log_generic(pci, t, "G");        break;
        case __BLK_TA_SLEEPRQ:      log_generic(pci, t, "S");        break;
        case __BLK_TA_REQUEUE:      log_queue(pci, t, "R");          break;
        case __BLK_TA_ISSUE:        log_issue(pdi, pci, t, "D");     break;
        case __BLK_TA_COMPLETE:     log_complete(pdi, pci, t, "C");  break;
        case __BLK_TA_PLUG:         log_action(pci, t, "P");         break;
        case __BLK_TA_UNPLUG_IO:    log_unplug(pci, t, "U");         break;
        case __BLK_TA_UNPLUG_TIMER: log_unplug(pci, t, "UT");        break;
        case __BLK_TA_SPLIT:        log_split(pci, t, "X");          break;
        case __BLK_TA_BOUNCE:       log_generic(pci, t, "B");        break;
        case __BLK_TA_REMAP:        log_generic(pci, t, "A");        break;
    }
}
```

### 完整映射表

| \_\_BLK_TA\_\* | 值 | 字母 | 含义 |
|---------------|-----|------|------|
| QUEUE | 1 | Q | bio 入队 |
| BACKMERGE | 2 | M | 后向合并 |
| FRONTMERGE | 3 | F | 前向合并 |
| GETRQ | 4 | G | 分配 request |
| SLEEPRQ | 5 | S | 等待分配 |
| REQUEUE | 6 | R | 重新入队 |
| ISSUE | 7 | **D** | 下发到驱动 |
| COMPLETE | 8 | **C** | I/O 完成 |
| PLUG | 9 | P | 插队 |
| UNPLUG_IO | 10 | U | I/O 触发拔塞 |
| UNPLUG_TIMER | 11 | UT | 定时器拔塞 |
| INSERT | 12 | I | 插入调度器 |
| SPLIT | 13 | X | bio 拆分 |
| BOUNCE | 14 | B | bounce buffer |
| REMAP | 15 | A | 重映射（DM/LVM） |

### 调用链

```
dump_trace(t, pci, pdi)                  // blkparse.c:1622
  │
  ├── action == BLK_TN_MESSAGE → handle_notify()
  │
  ├── action & BLK_TC_ACT(BLK_TC_PC)
  │     → dump_trace_pc()                // SCSI 请求（同样的 switch 逻辑）
  │
  └── else
        → dump_trace_fs()                // ★ 文件系统请求
            switch (action & 0xffff)     // 低 16 位匹配 → 事件字母
            同时 fill_rwbs()             // 高 16 位提取 → rwbs 字段
```

**一句话总结：** rwbs 和事件字母是 action 字段的两个独立维度 — 高 16 位通过 `fill_rwbs()` 生成 rwbs，低 16 位通过 `dump_trace_fs()` 的 switch 生成事件字母。

---

## Q6: setup_buts() 中 buts 是什么的缩写？

**日期：** 2026-07-02  
**场景：** 阅读用户态 blktrace.c 时遇到 `setup_buts()` 函数名不理解  
**相关文件：** `04_userspace_blktrace.c`  
**源码位置：** `src/blktrace/blktrace.c` — `setup_buts()` 函数

### 回答

**buts = Block User Trace Setup**

它是 `struct blk_user_trace_setup` 类型变量的名字：

```
blk  = Block
user = User
trace = Trace
setup = Setup
→ buts
```

从源码中可以看到：

```c
static void setup_buts(void)
{
    __list_for_each(p, &devpaths) {
        struct blk_user_trace_setup buts;   // ★ buts 是这个结构体的变量名

        buts.buf_size = buf_size;
        buts.buf_nr = buf_nr;
        buts.act_mask = act_mask;

        ioctl(dpp->fd, BLKTRACESETUP, &buts);
    }
}
```

`setup_buts()` = "对每个设备配置（setup）一个 `blk_user_trace_setup`（buts）结构体"。  
同理 `start_buts()` = 用已配置好的 buts 调用 `BLKTRACESTART` ioctl。

这是 Linux 内核社区常见的命名习惯 — 用结构体类型名的首字母缩写作为变量名：
- `struct request_queue *rq`
- `struct block_device *bdev`
- `struct blk_user_trace_setup buts`

---

## Q7: 为什么 per-CPU 设计？relay buffer 为什么还可能溢出？

**日期：** 2026-07-02  
**场景：** 阅读 `04_userspace_blktrace.c` 中的 relay buffer 和 per-CPU 线程设计时产生的疑问  
**相关文件：** `03_kernel_blktrace.c`、`04_userspace_blktrace.c`  
**源码位置：** `kernel/trace/blktrace.c` — `__blk_add_trace()`、`blktrace.c` — `thread_main()`

### 回答

#### 一、为什么 per-CPU？不仅仅是无锁

**首要原因：热路径零锁竞争。** `__blk_add_trace()` 运行在 I/O 热路径上，每秒可能调用百万次。全局锁会导致严重串行化。

```c
// kernel/trace/blktrace.c: __blk_add_trace()
cpu = raw_smp_processor_id();       // 获取当前 CPU 号

local_irq_save(flags);              // ★ 关中断（不是关锁！）
t = relay_reserve(bt->rchan, ...);  // 在当前 CPU 的 buffer 中预留空间
local_irq_restore(flags);           // 恢复中断
```

关中断（而非加锁）的原因是防止**同一 CPU** 上的中断处理程序也触发 `__blk_add_trace()`，干扰 `relay_reserve()` 的内部状态。跨 CPU 之间完全无干扰。

| 好处 | 说明 |
|------|------|
| **零锁竞争** | 每个 CPU 写自己的 buffer，无需 spin_lock |
| **缓存局部性** | 写自己 CPU 的 buffer，不会导致其他 CPU 缓存行失效 |
| **NUMA 友好** | buffer 分配在本地 NUMA 节点 |
| **事件顺序** | 同一 CPU 上天然有序 |
| **可伸缩性** | 增加 CPU 不增加竞争，追踪开销恒定 |

#### 二、全链路 per-CPU 架构

从内核到用户态全程 per-CPU：

```
__blk_add_trace(CPU 0) → relay_buf_0 → debugfs/trace0 → thread_0(绑定CPU 0) → sda.blktrace.0
__blk_add_trace(CPU 1) → relay_buf_1 → debugfs/trace1 → thread_1(绑定CPU 1) → sda.blktrace.1
```

用户态线程绑定 CPU 的源码：

```c
// blktrace.c: thread_main()
static void *thread_main(void *arg)
{
    struct tracer *tp = arg;
    ret = lock_on_cpu(tp->cpu);     // ★ 绑定到指定 CPU
    while (!tp->is_done) {
        ndone = poll(tp->pfds, ndevs, to_val);  // 只 poll 自己 CPU 的 relay 文件
        // ...
    }
}
```

#### 三、缺点

| 缺点 | 说明 | 解决方案 |
|------|------|---------|
| **事件乱序** | 不同 CPU 的事件时间不保证顺序 | blkparse 归并排序（按时间戳合并） |
| **内存开销 ×N** | 8 核 = 8 × 2MB = 16MB | 可接受，可通过 `-b`/`-n` 调整 |
| **配对复杂** | btt 需跨 CPU 配对 Q 和 C（C 在中断上下文，可能在不同 CPU） | btt 用 device+sector 配对 |

**最大缺点是事件乱序**——这正是 blkparse 存在的核心原因。

#### 四、relay buffer 大小考量

```
默认参数：
  BUF_SIZE = 512KB (每个 subbuffer)
  BUF_NR   = 4     (每个 CPU 4 个 subbuffer)
  每个 CPU 总缓冲 = 512KB × 4 = 2MB

┌──────────┬──────────┬──────────┬──────────┐
│ subbuf 0 │ subbuf 1 │ subbuf 2 │ subbuf 3 │
│  512KB   │  512KB   │  512KB   │  512KB   │
│  [写满]  │  [写入中] │  [空闲]  │ [用户态读]│
└──────────┴──────────┴──────────┴──────────┘
     ↑ write                     ↑ read
     kernel                      userspace
```

512KB 的计算依据：
```
每个 blk_io_trace = 48 字节
512KB / 48 ≈ 10,000 个事件/subbuffer
假设 IOPS=100,000/s × 5 事件/I/O = 500,000 事件/s
每 subbuffer 容纳 ≈ 0.02 秒事件
4 个 subbuffer ≈ 0.08 秒缓冲
用户态需在 80ms 内读完一个 subbuffer（绰绰有余）
```

#### 五、为什么持续 poll 仍然可能溢出？

**正常情况下确实不会溢出。** 但以下场景会：

**场景 1：I/O 突发（burst）**
```
瞬间百万事件 → 4 个 subbuffer 在几毫秒内全满
→ poll 还没返回（最小延迟 ~1μs）→ dropped!
```

**场景 2：用户态线程被抢占**
```
线程被调度器换出 → relay buffer 继续写入
→ 恢复时 buffer 已被覆盖 → dropped!
```

**场景 3：输出磁盘慢**
```
write() 到慢速磁盘（如 NFS）→ 阻塞 → 无法回到 poll()
→ relay buffer 积压 → dropped!
```

**场景 4：极高 IOPS 设备**
```
企业级 NVMe: 1,000,000+ IOPS × 5 事件 = 5,000,000 事件/s
× 48 字节 = 240 MB/s → 大多数磁盘写不了这么快 → dropped!
```

**检查 dropped：**
```bash
cat /sys/kernel/debug/block/vdb/dropped
# blktrace 结束时也会报告：
# Total: 152008 events (dropped 0)     ← 无丢失
# Total: 152008 events (dropped 1234)  ← 有 1234 个丢失
```

**避免 dropped 的方法：**
```bash
blktrace -d /dev/vdb -b 4096 -n 8            # 增大 buffer（32MB/CPU）
blktrace -d /dev/vdb -a issue -a complete     # 过滤事件（减少 76% 数据量）
blktrace -d /dev/vdb -o /tmp/trace            # 输出到快速磁盘
blktrace -d /dev/vdb -o - | blkparse -i -     # 管道模式（不落盘）
```

---

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
