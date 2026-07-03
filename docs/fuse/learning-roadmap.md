# FUSE（Filesystem in Userspace）学习路线

> 从用户进程 `read()` 到 FUSE 文件系统的 `read` 回调，一次完整的穿越内核之旅。

---

## 一、整体架构：FUSE 三层模型

```
┌──────────────────────────────────────────────────────────────────┐
│                        用户态（User Space）                       │
│                                                                  │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────────────┐ │
│  │  应用程序     │   │  FUSE 库     │   │  FUSE 文件系统守护进程 │ │
│  │  read(fd,..) │   │ (libfuse)    │   │  myfs_read() 回调     │ │
│  └──────┬───────┘   └──────▲───────┘   └──────────▲───────────┘ │
│         │                  │                       │             │
│         │ VFS 调用         │ write() 到 /dev/fuse  │ 函数回调     │
├─────────┼──────────────────┼───────────────────────┼─────────────┤
│         ▼                  │                       │             │
│  ┌──────────────┐   ┌──────┴───────┐   ┌──────────┴───────────┐ │
│  │  VFS 层      │   │  /dev/fuse   │   │  FUSE 内核模块        │ │
│  │  fuse_read() │──▶│  字符设备     │◀──│  (fuse.ko)           │ │
│  └──────────────┘   │  read/write  │   │  请求队列 + 响应处理   │ │
│                     └──────────────┘   └──────────────────────┘ │
│                                                                  │
│                        内核态（Kernel Space）                      │
└──────────────────────────────────────────────────────────────────┘
```

### 三层职责

| 层次 | 组件 | 职责 | 源码位置 |
|------|------|------|---------|
| **VFS 适配层** | `fuse.ko` 中的 file_operations | 将 VFS 调用转换为 FUSE 请求 | `src/linux-5.10/fs/fuse/file.c` |
| **传输层** | `/dev/fuse` 字符设备 | 内核与用户态的双向通信管道 | `src/linux-5.10/fs/fuse/dev.c` |
| **用户态库** | `libfuse.so` | 读取请求 → 分发到回调 → 写回响应 | libfuse GitHub 仓库 |

---

## 二、数据流全路径：一次 read() 的完整旅程

```
应用程序调用 read(fd, buf, 4096)
         │
         ▼
┌──── 内核态 ────────────────────────────────────────────────────┐
│                                                                 │
│  1. VFS: vfs_read()                                            │
│         │                                                       │
│         ▼                                                       │
│  2. fuse_read_iter()              [fs/fuse/file.c]             │
│         │                                                       │
│         ▼                                                       │
│  3. fuse_direct_read_iter() 或 fuse_cached_read_iter()         │
│         │                                                       │
│         ▼                                                       │
│  4. fuse_send_read()              [fs/fuse/file.c]             │
│         │  构造 fuse_read_in 结构                               │
│         │  填充: fh, offset, size                               │
│         ▼                                                       │
│  5. fuse_request_alloc() + queue_request()  [fs/fuse/dev.c]    │
│         │  请求进入 fiq->pending 队列                            │
│         │  唤醒 /dev/fuse 上的 poll/select 等待                  │
│         ▼                                                       │
│  6. ★ 内核线程阻塞在 wait_answer() 等待响应                     │
│                                                                 │
└─────────────────────── /dev/fuse 边界 ──────────────────────────┘
         │
         ▼
┌──── 用户态 ────────────────────────────────────────────────────┐
│                                                                 │
│  7. libfuse: fuse_session_receive_buf()                        │
│         │  从 /dev/fuse read() 读取二进制请求                    │
│         ▼                                                       │
│  8. libfuse: fuse_session_process_buf()                        │
│         │  解析 fuse_in_header.opcode = FUSE_READ (15)          │
│         ▼                                                       │
│  9. libfuse: 调用用户注册的 se->op.read() 回调                  │
│         │  即我们实现的 myfs_read() 函数                         │
│         ▼                                                       │
│  10. libfuse: fuse_send_reply_iov_nofree()                     │
│         │  构造 fuse_out_header + 数据                          │
│         │  write() 到 /dev/fuse                                 │
│                                                                 │
└─────────────────────── /dev/fuse 边界 ──────────────────────────┘
         │
         ▼
┌──── 内核态 ────────────────────────────────────────────────────┐
│                                                                 │
│  11. fuse_dev_read() 接收响应       [fs/fuse/dev.c]            │
│         │  匹配 unique ID 找到等待中的请求                       │
│         ▼                                                       │
│  12. request_wait_answer() 被唤醒                              │
│         │  拷贝数据到用户 buf                                   │
│         ▼                                                       │
│  13. read() 系统调用返回，应用程序拿到数据                       │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**★ 核心开销点：** 步骤 6→7 和 10→11 各涉及一次**内核↔用户态上下文切换**，这是 FUSE 相比内核文件系统的最大性能差距来源。

---

## 三、源码阅读路线图

### Step 1: FUSE 核心数据结构

**文件：** `reading/01_fuse_operations.h`

| 重点内容 | 说明 |
|---------|------|
| `struct fuse_operations` | 用户态 FUSE 文件系统的"虚函数表"——所有回调函数的注册点 |
| `struct fuse_args` | FUSE 启动参数 |
| `struct fuse_conn_info` | 连接协商信息（内核与 libfuse 的能力协商） |

> 预计阅读时间：10 分钟

### Step 2: /dev/fuse 通信协议

**文件：** `reading/02_dev_fuse_protocol.md`

| 重点内容 | 说明 |
|---------|------|
| `fuse_in_header` / `fuse_out_header` | 请求/响应的二进制包头格式 |
| `enum fuse_opcode` | 49 种 FUSE 操作码（FUSE_READ, FUSE_WRITE 等） |
| VFS → FUSE 转换 | 内核如何将 `vfs_read()` 转换为 FUSE_READ 请求 |

> 预计阅读时间：15 分钟

### Step 3: FUSE 请求处理循环

**来源：** `src/linux-5.10/fs/fuse/dev.c`

```
核心调用链：
  用户态 libfuse  read(/dev/fuse)  → fuse_dev_read()     [内核把请求给用户]
  用户态 libfuse  write(/dev/fuse) → fuse_dev_write()    [用户把响应给内核]
```

| 关注点 | 说明 |
|--------|------|
| `fuse_iqueue` | 内核发送队列（待用户态读取的请求） |
| `fuse_pqueue` | 内核处理队列（待用户态响应的请求） |
| `request_wait_answer()` | 内核线程阻塞等待响应的关键函数 |
| `fuse_dev_read()` | 用户态从 /dev/fuse 读请求的实现 |

> 预计阅读时间：20 分钟

### Step 4: 一个最小 FUSE 文件系统

**文件：** `reading/03_hello_fuse.c`

| 重点内容 | 说明 |
|---------|------|
| `getattr` 回调 | 实现 `ls -l` 所需的文件属性 |
| `readdir` 回调 | 实现 `ls` 所需的目录列举 |
| `open` / `read` 回调 | 实现 `cat` 所需的文件读取 |
| `fuse_main()` 启动流程 | libfuse 的初始化和事件循环 |

> 预计阅读时间：15 分钟

### Step 5: FUSE 性能分析

**文件：** `reading/04_fuse_performance.md`

| 重点内容 | 说明 |
|---------|------|
| 上下文切换开销 | 每次 I/O 至少 2 次内核↔用户态切换 |
| 数据拷贝开销 | 数据在内核缓冲区和用户缓冲区之间的额外拷贝 |
| writeback_cache 优化 | 减少写操作的上下文切换 |
| splice 优化 | 零拷贝传输数据 |

> 预计阅读时间：15 分钟

### Step 6: FUSE 与 blktrace 的关系

**交叉学习：** 结合 `docs/blktrace/` 的知识

```
FUSE 文件系统的 I/O 路径：

  应用 read() → FUSE 内核模块 → /dev/fuse → FUSE 守护进程
                                                    │
                                              如果底层存储是块设备
                                                    │
                                                    ▼
                                          block layer (Q→D→C)
                                              ▲
                                         blktrace 可以在这里追踪！
```

| 分析维度 | 工具 | 观察点 |
|---------|------|--------|
| FUSE 协议层 | `strace -p <fuse_pid>` | read/write /dev/fuse 的系统调用 |
| Block layer | `blktrace -d /dev/sda` | FUSE 守护进程对底层磁盘的 I/O |
| 延迟分析 | `strace -T` | FUSE 守护进程处理每个请求的耗时 |
| 性能对比 | `fio` (FUSE vs ext4) | FUSE 引入的额外开销量化 |

> 预计阅读时间：10 分钟

---

## 四、核心概念速查

### 4.1 FUSE 请求/响应协议

```
请求（内核 → 用户态）：
┌──────────────────┬──────────────────┬────────────────────┐
│ fuse_in_header   │ 操作特定参数      │ 附加数据            │
│ (40 字节)        │ (如 fuse_read_in) │ (如 write 的数据)   │
│ len, opcode,     │                  │                    │
│ unique, nodeid,  │                  │                    │
│ uid, gid, pid    │                  │                    │
└──────────────────┴──────────────────┴────────────────────┘

响应（用户态 → 内核）：
┌──────────────────┬──────────────────┬────────────────────┐
│ fuse_out_header  │ 操作特定结果      │ 返回数据            │
│ (16 字节)        │ (如 fuse_attr_out)│ (如 read 的数据)    │
│ len, error,      │                  │                    │
│ unique           │                  │                    │
└──────────────────┴──────────────────┴────────────────────┘
```

### 4.2 fuse_lowlevel vs fuse_highlevel API

| 特性 | Highlevel API (`fuse.h`) | Lowlevel API (`fuse_lowlevel.h`) |
|------|------------------------|---------------------------------|
| **抽象层次** | 基于路径（path-based） | 基于 inode（inode-based） |
| **参数** | `const char *path` | `fuse_ino_t ino` |
| **路径解析** | libfuse 自动解析 | 用户自己管理 inode 映射 |
| **适用场景** | 简单文件系统（hello world） | 高性能/复杂文件系统 |
| **性能** | 每次操作需路径查找 | 直接按 inode 操作，更高效 |
| **复杂度** | 低 | 高（需要自行管理 inode 生命周期） |

```c
// Highlevel: 路径级别
int myfs_read(const char *path, char *buf, size_t size,
              off_t offset, struct fuse_file_info *fi);

// Lowlevel: inode 级别
void myfs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
               off_t off, struct fuse_file_info *fi);
```

### 4.3 FUSE 性能瓶颈分析

```
一次 FUSE read 的开销分解：

┌──────────────────────────────────────────────────────────────┐
│  内核态 → 用户态 切换 1      ~1-5 μs  (上下文切换)           │
│  /dev/fuse read() 拷贝       ~0.5 μs  (数据从内核到用户)     │
│  用户态处理（查找+读取）     ~变化大   (取决于文件系统实现)   │
│  /dev/fuse write() 拷贝      ~0.5 μs  (响应从用户到内核)     │
│  用户态 → 内核态 切换 2      ~1-5 μs  (上下文切换)           │
│  数据拷贝到用户 buf          ~0.5 μs  (最终数据交付)          │
├──────────────────────────────────────────────────────────────┤
│  总额外开销                     ~5-15 μs（不含用户处理时间） │
│  内核文件系统（如 ext4）开销   ~1-3 μs                       │
│  FUSE 开销倍数                  ~3-5x（小 I/O 时更明显）     │
└──────────────────────────────────────────────────────────────┘
```

---

## 五、动手实验

### 实验 1：编译运行 hello world FUSE 文件系统

```bash
# 安装依赖
yum install -y fuse-devel fuse

# 编译（使用 reading/03_hello_fuse.c）
gcc -o hello_fuse reading/03_hello_fuse.c $(pkg-config fuse --cflags --libs)

# 创建挂载点
mkdir -p /tmp/myfuse

# 挂载运行（前台模式，方便观察）
./hello_fuse -f /tmp/myfuse

# 另一个终端测试
ls -la /tmp/myfuse
cat /tmp/myfuse/hello
```

### 实验 2：用 strace 观察 FUSE 的 /dev/fuse 通信

```bash
# 终端 1：启动 FUSE（前台模式）
./hello_fuse -f /tmp/myfuse &
FUSE_PID=$!

# 终端 2：strace 追踪 FUSE 守护进程
strace -p $FUSE_PID -e trace=read,write -f -T

# 终端 3：触发文件操作
cat /tmp/myfuse/hello

# 观察 strace 输出：
# read(3, ...)   ← 从 /dev/fuse 读取请求
# write(3, ...)  ← 向 /dev/fuse 写入响应
```

### 实验 3：用 blktrace 追踪 FUSE 文件系统的底层 I/O

```bash
# 如果 FUSE 文件系统底层使用块设备存储（如 sshfs 的网络 I/O 不经过 block layer）
# 但如果是基于本地磁盘的 FUSE 文件系统（如 encfs）：

# 创建基于本地磁盘的 FUSE 文件系统目录
mkdir -p /data/plain /tmp/encfs_mount
encfs /data/plain /tmp/encfs_mount  # 需要安装 encfs

# 追踪底层块设备
blktrace -d /dev/vdb -o fuse_trace -w 30 &
sleep 1

# 在 FUSE 挂载点上执行 I/O
dd if=/dev/zero of=/tmp/encfs_mount/test.dat bs=4k count=100

# 分析 blktrace 输出
blkparse -i fuse_trace -o parsed.txt
btt -i fuse_trace.bin

# ★ 观察：FUSE 守护进程对 /dev/vdb 的 I/O 模式
# 与直接写 /dev/vdb 的 I/O 模式对比，FUSE 会引入额外的元数据操作
```

### 实验 4：FUSE vs 原生 ext4 性能对比

```bash
# 基准测试：原生 ext4
mkdir -p /data/benchmark
fio --name=ext4_test --directory=/data/benchmark --ioengine=libaio \
    --direct=1 --rw=randread --bs=4k --size=100M --numjobs=1 \
    --runtime=10 --time_based --output=ext4_result.json

# 基准测试：FUSE 文件系统（以 bindfs 为例，最简单的 FUSE）
bindfs /data/benchmark /tmp/fuse_benchmark
fio --name=fuse_test --directory=/tmp/fuse_benchmark --ioengine=libaio \
    --direct=1 --rw=randread --bs=4k --size=100M --numjobs=1 \
    --runtime=10 --time_based --output=fuse_result.json

# 对比 IOPS 和延迟
# 预期：FUSE 的 IOPS 下降 30-60%，延迟增加 2-5 倍
```

---

## 六、关键源码文件索引

### 内核侧（`src/linux-5.10/fs/fuse/`）

| 文件 | 职责 | 行数 | 阅读优先级 |
|------|------|------|-----------|
| `fuse_i.h` | 内核 FUSE 核心数据结构 | ~800 | ★★★ 必读 |
| `dev.c` | /dev/fuse 设备实现（请求队列、读写处理） | ~2400 | ★★★ 核心 |
| `file.c` | 文件操作实现（read/write/mmap） | ~3000 | ★★★ 必读 |
| `dir.c` | 目录操作实现（lookup/readdir） | ~1800 | ★★ 按需 |
| `inode.c` | inode 管理、mount 初始化 | ~1400 | ★★ 按需 |
| `readdir.c` | readdir 缓存机制 | ~800 | ★ 可选 |
| `control.c` | FUSE 控制文件系统（/sys/fs/fuse/） | ~400 | ★ 可选 |

### 内核 UAPI 头文件

| 文件 | 职责 |
|------|------|
| `src/linux-5.10/include/uapi/linux/fuse.h` | ★ FUSE 协议定义（所有结构体和 opcode） |

### libfuse 用户态库（外部来源）

| 文件 | 职责 | 来源 |
|------|------|------|
| `include/fuse.h` | highlevel API 定义 | [libfuse GitHub](https://github.com/libfuse/libfuse) |
| `include/fuse_lowlevel.h` | lowlevel API 定义 | 同上 |
| `lib/fuse.c` | highlevel 实现（路径→inode 转换） | 同上 |
| `lib/fuse_loop.c` | 单线程事件循环 | 同上 |
| `lib/fuse_loop_mt.c` | 多线程事件循环 | 同上 |
| `lib/fuse_lowlevel.c` | lowlevel 实现（协议解析） | 同上 |
| `example/hello.c` | 官方 hello world 示例 | 同上 |
| `example/hello_ll.c` | lowlevel API 示例 | 同上 |

### 按功能查找源码

```bash
# 找 FUSE opcode 定义
grep -n "enum fuse_opcode" src/linux-5.10/include/uapi/linux/fuse.h

# 找 fuse_in_header（请求包头）
grep -n "struct fuse_in_header" src/linux-5.10/include/uapi/linux/fuse.h

# 找内核 FUSE read 实现
grep -n "fuse_read_iter\|fuse_send_read" src/linux-5.10/fs/fuse/file.c

# 找 /dev/fuse 的读写实现
grep -n "fuse_dev_read\|fuse_dev_write" src/linux-5.10/fs/fuse/dev.c

# 找请求队列管理
grep -n "fuse_iqueue\|queue_request\|request_wait_answer" src/linux-5.10/fs/fuse/dev.c

# 找 FUSE mount 流程
grep -n "fuse_fill_super\|fuse_get_tree" src/linux-5.10/fs/fuse/inode.c
```

---

## 七、与其他章节的关联

| 章节 | 关联点 |
|------|--------|
| **blktrace** | FUSE 守护进程对底层块设备的 I/O 可以被 blktrace 追踪 |
| **fio** | 用 fio 对比 FUSE 与原生文件系统的性能差异 |
| **strace** | strace 观察 FUSE 守护进程的 /dev/fuse 通信 |
| **perf** | perf 分析 FUSE 上下文切换的热点函数 |
