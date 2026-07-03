# /dev/fuse 通信协议详解

> **原始路径：** `src/linux-5.10/include/uapi/linux/fuse.h`（协议定义）
> **内核实现：** `src/linux-5.10/fs/fuse/dev.c`（/dev/fuse 设备驱动）
> **预计阅读时间：** 15 分钟

---

## 一、/dev/fuse 是什么？

`/dev/fuse` 是一个**字符设备**（主设备号 10，次设备号 229），它是 FUSE 内核模块与用户态守护进程之间的**唯一通信通道**。

```
┌──────────────────────────────────────────────────────────────┐
│                    内核态（fuse.ko）                          │
│                                                              │
│  VFS 操作（read/write/getattr/...）                          │
│       │                                                      │
│       ▼                                                      │
│  fuse_send_read() / fuse_send_write() / ...                  │
│       │ 构造 FUSE 请求                                       │
│       ▼                                                      │
│  ┌────────────────────┐       ┌────────────────────┐        │
│  │ fiq (发送队列)      │       │ pq (处理队列)       │        │
│  │ 等待用户态来取      │       │ 等待用户态回复      │        │
│  └────────┬───────────┘       └────────▲───────────┘        │
│           │                            │                     │
├───────────┼────────────────────────────┼─────────────────────┤
│           ▼ read()                write() ▲                  │
│  ┌──────────────────────────────────────────────────────┐    │
│  │              /dev/fuse (字符设备 10, 229)              │    │
│  └──────────────────────────────────────────────────────┘    │
│           │                            ▲                     │
├───────────┼────────────────────────────┼─────────────────────┤
│           ▼ read()                write() │                  │
│  ┌──────────────────────────────────────────────────────┐    │
│  │              libfuse（用户态库）                       │    │
│  │  fuse_session_receive_buf() → 解析请求               │    │
│  │  fuse_session_process_buf() → 分发到回调             │    │
│  │  fuse_session_send_buf()    → 写回响应               │    │
│  └──────────────────────────────────────────────────────┘    │
│                                                              │
│                    用户态（FUSE 守护进程）                     │
└──────────────────────────────────────────────────────────────┘
```

**关键流程：**
1. **内核 → 用户态**：内核将 VFS 请求序列化为二进制数据，放入 fiq 队列，用户态通过 `read(/dev/fuse)` 获取
2. **用户态 → 内核**：用户态处理完请求后，通过 `write(/dev/fuse)` 将响应写回内核

---

## 二、请求格式（内核 → 用户态）

### 2.1 请求包头：`fuse_in_header`

每个 FUSE 请求都以 `fuse_in_header` 开头，固定 **40 字节**：

```c
// src/linux-5.10/include/uapi/linux/fuse.h:800
struct fuse_in_header {
    uint32_t    len;        /* ★ 整个请求的总长度（含包头） */
    uint32_t    opcode;     /* ★ 操作码（FUSE_READ=15, FUSE_WRITE=16, ...） */
    uint64_t    unique;     /* ★ 请求唯一 ID（用于匹配响应） */
    uint64_t    nodeid;     /* ★ inode 编号（FUSE_ROOT_ID=1 表示根目录） */
    uint32_t    uid;        /* 发起操作的进程 UID */
    uint32_t    gid;        /* 发起操作的进程 GID */
    uint32_t    pid;        /* 发起操作的进程 PID */
    uint32_t    padding;    /* 对齐填充 */
};
// 总计：4 + 4 + 8 + 8 + 4 + 4 + 4 + 4 = 40 字节
```

```
二进制布局：
┌──────────┬──────────┬──────────┬──────────┬──────┬──────┬──────┬─────────┐
│ len(4)   │opcode(4) │unique(8) │nodeid(8) │uid(4)│gid(4)│pid(4)│padding(4)│
└──────────┴──────────┴──────────┴──────────┴──────┴──────┴──────┴─────────┘
│←                         40 字节                          →│
```

### 2.2 操作特定参数

包头之后紧跟操作特定的参数结构体。以 FUSE_READ 为例：

```c
// src/linux-5.10/include/uapi/linux/fuse.h:625
struct fuse_read_in {
    uint64_t    fh;          /* ★ 文件句柄（open 时用户态返回的） */
    uint64_t    offset;      /* ★ 读取偏移量 */
    uint32_t    size;        /* ★ 请求读取的字节数 */
    uint32_t    read_flags;  /* 读标志 */
    uint64_t    lock_owner;  /* 锁拥有者 */
    uint32_t    flags;       /* 文件打开标志 */
    uint32_t    padding;     /* 对齐 */
};
```

**完整 FUSE_READ 请求布局：**

```
┌────────────────────────────┬──────────────────────────────────────────┐
│     fuse_in_header (40B)   │          fuse_read_in (40B)              │
│  len=80, opcode=15,        │  fh=3, offset=0, size=4096,             │
│  unique=42, nodeid=2,      │  read_flags=0, lock_owner=0,            │
│  uid=1000, gid=1000,       │  flags=O_RDONLY, padding=0              │
│  pid=1234                  │                                          │
└────────────────────────────┴──────────────────────────────────────────┘
│←                              80 字节                               →│
```

### 2.3 带数据的请求（FUSE_WRITE）

FUSE_WRITE 除了包头和操作参数外，还携带要写入的数据：

```
┌────────────────────┬────────────────────┬────────────────────────────┐
│ fuse_in_header     │ fuse_write_in      │ 写入数据（size 字节）       │
│ (40B)              │ (40B)              │ (可变长度)                  │
│ opcode=16          │ fh, offset, size   │ "Hello, World!\n"          │
└────────────────────┴────────────────────┴────────────────────────────┘
│← in.h.len 指示的总长度 →│
```

---

## 三、响应格式（用户态 → 内核）

### 3.1 响应包头：`fuse_out_header`

每个 FUSE 响应都以 `fuse_out_header` 开头，固定 **16 字节**：

```c
// src/linux-5.10/include/uapi/linux/fuse.h:811
struct fuse_out_header {
    uint32_t    len;        /* ★ 整个响应的总长度（含包头） */
    int32_t     error;      /* ★ 错误码（0=成功，负数=失败） */
    uint64_t    unique;     /* ★ 对应请求的 unique ID（用于匹配） */
};
// 总计：4 + 4 + 8 = 16 字节
```

```
二进制布局：
┌──────────┬──────────┬──────────────┐
│ len(4)   │error(4)  │  unique(8)   │
└──────────┴──────────┴──────────────┘
│←         16 字节          →│
```

### 3.2 操作特定结果

以 FUSE_GETATTR 的响应为例：

```c
struct fuse_attr_out {
    uint64_t    attr_valid;      /* 属性缓存有效期（秒） */
    uint32_t    attr_valid_nsec; /* 属性缓存有效期（纳秒） */
    uint32_t    dummy;
    struct fuse_attr attr;       /* ★ 文件属性（inode/size/mode/...） */
};
```

**完整 FUSE_GETATTR 响应布局：**

```
┌─────────────────────┬──────────────────────────────────────────┐
│ fuse_out_header     │           fuse_attr_out                  │
│ (16B)               │  attr_valid=1, attr_valid_nsec=0,        │
│ len=144, error=0,   │  attr: {ino=2, size=13, blocks=8,       │
│ unique=42           │    mode=0100644, nlink=1, uid=1000, ...} │
└─────────────────────┴──────────────────────────────────────────┘
│←                         144 字节                             →│
```

### 3.3 错误响应

出错时，响应只包含 `fuse_out_header`，没有后续数据：

```
┌─────────────────────┐
│ fuse_out_header     │
│ len=16, error=-2,   │  ← error=-2 表示 -ENOENT（文件不存在）
│ unique=42           │
└─────────────────────┘
```

---

## 四、FUSE Opcode 完整列表

### 4.1 核心 Opcode（高频使用）

```
值  │ 名称              │ 触发 VFS 操作     │ 请求参数              │ 响应数据
────┼──────────────────┼──────────────────┼──────────────────────┼──────────────────
 1  │ FUSE_LOOKUP      │ open/stat        │ 文件名（字符串）       │ fuse_entry_out
 3  │ FUSE_GETATTR     │ stat/ls          │ fuse_getattr_in       │ fuse_attr_out
 4  │ FUSE_SETATTR     │ chmod/truncate   │ fuse_setattr_in       │ fuse_attr_out
14  │ FUSE_OPEN        │ open()           │ fuse_open_in          │ fuse_open_out
15  │ ★ FUSE_READ      │ read()/pread()   │ fuse_read_in          │ 数据（字节流）
16  │ ★ FUSE_WRITE     │ write()/pwrite() │ fuse_write_in + 数据  │ fuse_write_out
17  │ FUSE_STATFS      │ statfs()/df      │ (无)                  │ fuse_statfs_out
18  │ FUSE_RELEASE     │ close()          │ fuse_release_in       │ (无)
20  │ FUSE_FSYNC       │ fsync()          │ fuse_fsync_in         │ (无)
25  │ FUSE_FLUSH       │ close()          │ fuse_flush_in         │ (无)
26  │ ★ FUSE_INIT      │ mount 时         │ fuse_init_in          │ fuse_init_out
38  │ FUSE_DESTROY     │ umount 时        │ (无)                  │ (无)
```

### 4.2 目录操作 Opcode

```
值  │ 名称              │ 触发 VFS 操作     │ 请求参数              │ 响应数据
────┼──────────────────┼──────────────────┼──────────────────────┼──────────────────
 2  │ FUSE_FORGET      │ inode 释放       │ fuse_forget_in        │ (无响应！)
 9  │ FUSE_MKDIR       │ mkdir()          │ fuse_mkdir_in + 名称  │ fuse_entry_out
10  │ FUSE_UNLINK      │ unlink()         │ 文件名（字符串）       │ (无)
11  │ FUSE_RMDIR       │ rmdir()          │ 目录名（字符串）       │ (无)
12  │ FUSE_RENAME      │ rename()         │ fuse_rename_in + 名称 │ (无)
27  │ FUSE_OPENDIR     │ opendir()        │ fuse_open_in          │ fuse_open_out
28  │ ★ FUSE_READDIR   │ readdir()/ls     │ fuse_read_in          │ fuse_dirent 序列
29  │ FUSE_RELEASEDIR  │ closedir()       │ fuse_release_in       │ (无)
35  │ FUSE_CREATE      │ open(O_CREAT)    │ fuse_create_in + 名称 │ entry_out + open_out
44  │ FUSE_READDIRPLUS │ readdir + lookup │ fuse_read_in          │ fuse_direntplus 序列
```

### 4.3 特殊 Opcode

```
值  │ 名称              │ 说明
────┼──────────────────┼──────────────────────────────────────────
 5  │ FUSE_READLINK    │ 读取符号链接目标
 6  │ FUSE_SYMLINK     │ 创建符号链接
 8  │ FUSE_MKNOD       │ 创建设备文件
13  │ FUSE_LINK        │ 创建硬链接
21  │ FUSE_SETXATTR    │ 设置扩展属性
22  │ FUSE_GETXATTR    │ 获取扩展属性
23  │ FUSE_LISTXATTR   │ 列举扩展属性
24  │ FUSE_REMOVEXATTR │ 删除扩展属性
31  │ FUSE_GETLK       │ 获取文件锁
32  │ FUSE_SETLK       │ 设置文件锁
33  │ FUSE_SETLKW      │ 设置文件锁（等待）
34  │ FUSE_ACCESS      │ 检查访问权限
36  │ FUSE_INTERRUPT   │ 中断正在处理的请求
39  │ FUSE_IOCTL       │ ioctl 操作
40  │ FUSE_POLL        │ poll/select 事件
43  │ FUSE_FALLOCATE   │ 预分配空间
45  │ FUSE_RENAME2     │ renameat2（带标志）
46  │ FUSE_LSEEK       │ seek(SEEK_DATA/HOLE)
47  │ FUSE_COPY_FILE_RANGE │ 服务端文件拷贝
```

> **源码位置：** `src/linux-5.10/include/uapi/linux/fuse.h:434` — `enum fuse_opcode`

---

## 五、FUSE 生命周期：一次 read() 的完整协议交互

```
时间线    内核态（fuse.ko）              /dev/fuse         用户态（libfuse + 守护进程）
──────    ────────────────              ─────────         ──────────────────────────

 t0       应用调用 read(fd, buf, 4096)
          │
 t1       vfs_read()
          → fuse_read_iter()
          → fuse_send_read()
          │ 构造 fuse_in_header:
          │   len=80, opcode=15(READ)
          │   unique=42, nodeid=2
          │   uid=1000, pid=1234
          │ 构造 fuse_read_in:
          │   fh=3, offset=0, size=4096
          │
 t2       queue_request()
          │ 请求入 fiq->pending 队列
          │ 唤醒 /dev/fuse 的 poll 等待
          │                              poll 返回可读
 t3       ★ 内核线程阻塞                  ──────────→    read(/dev/fuse) = 80 字节
          wait_answer_interruptible()                    │ 解析 fuse_in_header:
          │                                              │  opcode=15 → FUSE_READ
 t4                                                      │ 解析 fuse_read_in:
                                                         │  fh=3, offset=0, size=4096
                                                         │
 t5                                                      │ 调用用户回调:
                                                         │  myfs_read("/hello", buf,
                                                         │            4096, 0, fi)
                                                         │ → 返回 13 字节
                                                         │
 t6                                                      │ 构造响应:
                                                         │  fuse_out_header:
                                                         │    len=29, error=0, unique=42
                                                         │  数据: "Hello, World!\n" (13B)
                                                         │
 t7                                      ←──────────     write(/dev/fuse) = 29 字节
          fuse_dev_write() 接收响应                       │
          │ 匹配 unique=42
          │ 找到等待中的请求
 t8       request_wait_answer() 被唤醒
          │ 拷贝 13 字节到用户 buf
          │ read() 系统调用返回 13
 t9       应用程序拿到数据
```

---

## 六、INIT 协商：FUSE 的"握手"过程

FUSE 挂载时，内核和用户态通过 `FUSE_INIT` 请求进行能力协商：

```
内核发送 fuse_init_in:             用户态回复 fuse_init_out:
┌──────────────────────┐          ┌──────────────────────┐
│ major = 7            │          │ major = 7            │
│ minor = 32           │ ───→     │ minor = 32           │  ← 取两者的较小值
│ max_readahead = 131072│         │ max_readahead = 131072│
│ flags = 0x07FFFFFF   │          │ flags = 0x00010017   │  ← 用户态选择需要的能力
│ (内核支持的所有能力)   │          │ max_write = 1048576  │  ← 最大写大小（1MB）
└──────────────────────┘          │ max_background = 12  │
                                  │ congestion_threshold=9│
                                  │ time_gran = 1        │  ← 时间精度（1ns）
                                  │ max_pages = 256      │
                                  └──────────────────────┘
```

### 重要的 INIT flags

| 标志 | 含义 | 性能影响 |
|------|------|---------|
| `FUSE_ASYNC_READ` | 支持异步读 | 允许并发读取，提升吞吐 |
| `FUSE_WRITEBACK_CACHE` | ★ 启用写回缓存 | 减少写操作的上下文切换 |
| `FUSE_SPLICE_WRITE` | 支持 splice 写 | 零拷贝传输数据 |
| `FUSE_SPLICE_READ` | 支持 splice 读 | 零拷贝传输数据 |
| `FUSE_BIG_WRITES` | 支持大于 4KB 的写 | 减少写请求次数 |
| `FUSE_READDIRPLUS` | readdir + lookup 合并 | 减少目录列举的请求数 |
| `FUSE_PARALLEL_DIROPS` | 允许并行目录操作 | 减少锁竞争 |
| `FUSE_NO_OPEN_SUPPORT` | 零消息 open | 省略 OPEN+RELEASE 往返 |

---

## 七、nodeid：FUSE 的 inode 编号体系

FUSE 使用 `nodeid`（64 位）标识文件/目录，与内核的 inode 编号不同：

```
nodeid 含义
────── ──────────────────────────────
  0    无效（不存在的文件）
  1    ★ 根目录（FUSE_ROOT_ID）
  2+   用户态分配的其他文件/目录
```

### nodeid 生命周期

```
1. 内核发送 FUSE_LOOKUP（查找文件）
   → 用户态返回 fuse_entry_out { nodeid=5, ... }
   → 内核记录 nodeid=5 对应的 inode

2. 后续操作都使用 nodeid=5
   → FUSE_GETATTR(nodeid=5)
   → FUSE_OPEN(nodeid=5)
   → FUSE_READ(nodeid=5, fh=...)

3. 内核不再需要该 inode 时
   → 发送 FUSE_FORGET(nodeid=5, nlookup=N)
   → 用户态可以释放 nodeid=5 的资源
   ★ FUSE_FORGET 是唯一不需要响应的 opcode！
```

---

## 八、请求/响应的 size 约束

| 约束 | 值 | 说明 |
|------|-----|------|
| 最小读缓冲区 | 8192 字节（8KB） | `FUSE_MIN_READ_BUFFER` |
| 默认最大读大小 | 128KB（32 页） | `FUSE_DEFAULT_MAX_PAGES_PER_REQ × PAGE_SIZE` |
| 最大页数上限 | 256 页（1MB） | `FUSE_MAX_MAX_PAGES × PAGE_SIZE` |
| /dev/fuse 单次读 | header + 操作参数 + 数据 | 受 max_read 限制 |
| /dev/fuse 单次写 | header + 操作结果 + 数据 | 受 max_write 限制 |

---

## 九、与 blktrace 的对比

| 特性 | FUSE /dev/fuse | blktrace relay |
|------|---------------|----------------|
| **通信方向** | 双向（read 取请求，write 送响应） | 单向（内核→用户态） |
| **设备类型** | 字符设备（10, 229） | debugfs relay 文件 |
| **数据格式** | 结构化（header + 参数 + 数据） | 固定结构（blk_io_trace 48B） |
| **匹配机制** | unique ID 配对请求/响应 | sequence + timestamp 配对事件 |
| **缓冲策略** | 单请求阻塞等待 | per-CPU relay buffer |
| **阻塞模型** | 内核线程阻塞等待用户态响应 | 内核非阻塞写入 relay |

---

## 十、调用链总结

```
用户进程 read(fd, buf, size)
  │
  ▼
VFS: vfs_read()
  │
  ▼
fuse_read_iter()                         [fs/fuse/file.c]
  │
  ▼
fuse_send_read()                         [fs/fuse/file.c]
  │  构造 fuse_read_in { fh, offset, size }
  ▼
fuse_request_alloc()                     [fs/fuse/dev.c]
  │  分配 fuse_req，填充 fuse_in_header
  ▼
queue_request()                          [fs/fuse/dev.c]
  │  入队 fiq->pending，唤醒 poll
  ▼
request_wait_answer()                    [fs/fuse/dev.c]
  │  ★ 内核线程在此阻塞
  │
  │  ──── /dev/fuse 边界 ────
  │
  │  用户态 read(/dev/fuse)  → 拿到请求
  │  用户态处理（回调）       → 得到数据
  │  用户态 write(/dev/fuse) → 送回响应
  │
  │  ──── /dev/fuse 边界 ────
  │
  ▼
fuse_dev_write()                         [fs/fuse/dev.c]
  │  匹配 unique，找到等待中的请求
  ▼
request_wait_answer() 被唤醒             [fs/fuse/dev.c]
  │  拷贝数据到用户 buf
  ▼
fuse_read_iter() 返回
  │
  ▼
vfs_read() 返回 → 用户进程拿到数据
```
