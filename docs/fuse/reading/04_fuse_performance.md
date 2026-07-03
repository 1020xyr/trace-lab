# FUSE 性能分析

> **阅读前提：** 已完成 Step 1-3 的学习，理解 FUSE 的请求/响应协议
> **关联章节：** `docs/blktrace/` — FUSE 底层 I/O 可用 blktrace 追踪
> **预计阅读时间：** 15 分钟

---

## 一、FUSE 的开销来源

FUSE 相比内核文件系统（ext4、XFS 等）有显著的性能开销，主要来自以下四个方面：

### 1.1 上下文切换开销（★ 最大开销）

```
内核文件系统 read() 路径：
  用户进程 → 系统调用 → VFS → ext4 → page cache → 返回
  ★ 只有 1 次用户态→内核态切换（系统调用入口）

FUSE read() 路径：
  用户进程 → 系统调用 → VFS → FUSE 内核模块 → 阻塞等待
                                             ↓
                              用户态 FUSE 守护进程 read(/dev/fuse)
                                             ↓
                              用户态处理（查找+读取）
                                             ↓
                              用户态 FUSE 守护进程 write(/dev/fuse)
                                             ↓
  用户进程 ← 系统调用返回 ← VFS ← FUSE 内核模块

  ★ 涉及 4 次上下文切换（2 次内核→用户，2 次用户→内核）
```

每次上下文切换的开销：

| 开销来源 | 时间 | 说明 |
|---------|------|------|
| CPU 模式切换（ring 0 ↔ ring 3） | ~0.1-0.5 μs | 权限级别切换 |
| TLB 刷新（部分） | ~0.1-0.3 μs | 地址空间切换 |
| 寄存器保存/恢复 | ~0.05 μs | 上下文保存 |
| 调度器开销 | ~0.1-1 μs | 可能触发调度 |
| **单次切换总计** | **~0.5-2 μs** | 取决于 CPU 架构和频率 |

**FUSE 一次 read 的上下文切换开销：4 × ~1 μs ≈ ~4 μs**

### 1.2 数据拷贝开销

```
内核文件系统（使用 page cache）：
  磁盘 → page cache（内核页）→ 用户缓冲区
  ★ 一次拷贝（page cache → user buffer，copy_to_user）

FUSE（不启用 splice）：
  用户缓冲区（内核）→ /dev/fuse 读缓冲（用户态）  ← 拷贝 #1
  用户态处理数据
  /dev/fuse 写缓冲（用户态）→ 内核缓冲区          ← 拷贝 #2
  内核缓冲区 → 用户进程缓冲区                     ← 拷贝 #3
  ★ 三次拷贝（理想情况下两次，因为数据可能不需要额外内核缓冲）
```

### 1.3 调度延迟

FUSE 内核线程在发送请求后会**阻塞等待用户态响应**。这意味着：

```
内核线程                    用户态守护进程
────────                    ────────────
发送请求
  │
  ▼
阻塞等待 ───调度器切换──→  被唤醒
                            │
                            ▼
                          处理请求
                            │
                            ▼
                          发送响应 ───调度器切换──→ 被唤醒
                                                    │
                                                    ▼
                                                  处理响应
```

**调度延迟的影响：**
- 如果守护进程不在运行（被调度器换出），唤醒需要 ~1-10 μs
- 如果系统负载高，调度延迟可能达到 ~100 μs 甚至更多
- FUSE I/O 的延迟 = 上下文切换 + 调度延迟 + 用户处理时间

### 1.4 无法使用内核缓存机制

```
内核文件系统：
  read() → page cache 命中 → 直接返回（不访问磁盘）
  ★ 缓存命中时延迟 ~0.5 μs

FUSE（默认模式）：
  每次 read() 都要经过 /dev/fuse 到用户态
  ★ 即使数据已在用户态缓存，仍需 2 次上下文切换
```

---

## 二、性能对比：FUSE vs 内核文件系统

### 2.1 典型性能差异

```
┌─────────────────┬─────────────┬─────────────┬────────────┐
│ 指标             │ 内核文件系统 │ FUSE         │ 差距       │
│                 │ (ext4/XFS)  │ (默认模式)   │            │
├─────────────────┼─────────────┼─────────────┼────────────┤
│ 4K 随机读 IOPS   │ 200,000+   │ 30,000-80,000│ 2.5-7x     │
│ 4K 随机写 IOPS   │ 150,000+   │ 20,000-60,000│ 2.5-7.5x   │
│ 顺序读吞吐       │ ~2 GB/s    │ ~0.5-1.5 GB/s│ 1.3-4x     │
│ 顺序写吞吐       │ ~1.5 GB/s  │ ~0.3-1 GB/s  │ 1.5-5x     │
│ 单次读延迟       │ ~5 μs      │ ~15-30 μs    │ 3-6x       │
│ 单次写延迟       │ ~7 μs      │ ~20-50 μs    │ 3-7x       │
│ stat() 延迟      │ ~1 μs      │ ~10-20 μs    │ 10-20x     │
│ 元数据操作       │ 快          │ 慢（每次往返）│ 10-20x     │
└─────────────────┴─────────────┴─────────────┴────────────┘

注：以上数据基于 NVMe SSD 的典型测试结果，实际差异取决于：
  - FUSE 文件系统实现的效率
  - 是否启用优化选项（writeback_cache、splice 等）
  - 系统负载和 CPU 频率
```

### 2.2 延迟分解

```
一次 4K FUSE read 的延迟分解（总计 ~25 μs）：

┌──────────────────────────────────────────────────────────────┐
│                                                              │
│  VFS 到 FUSE 内核模块           ~1 μs                        │
│  ████████                                                    │
│                                                              │
│  构造请求 + 入队                  ~0.5 μs                     │
│  ████                                                        │
│                                                              │
│  ★ 上下文切换 #1（内核→用户）    ~2 μs                       │
│  ████████████████                                            │
│                                                              │
│  用户态接收 + 解析请求            ~1 μs                       │
│  ████████                                                    │
│                                                              │
│  用户态处理（从内存读取数据）     ~5 μs                       │
│  ████████████████████████████████████████                    │
│                                                              │
│  ★ 上下文切换 #2（用户→内核）    ~2 μs                       │
│  ████████████████                                            │
│                                                              │
│  内核接收响应 + 拷贝数据          ~1 μs                       │
│  ████████                                                    │
│                                                              │
│  ★ 上下文切换 #3+#4              ~4 μs                       │
│  ████████████████████████████████                            │
│                                                              │
│  唤醒等待线程 + 返回              ~1 μs                       │
│  ████████                                                    │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 2.3 不同 I/O 大小的开销比例

```
I/O 大小    FUSE 额外开销    有效带宽损失    原因
────────    ────────────    ────────────    ──────────────────
  512B       ~25 μs/次       ★★ 严重        固定开销占比太大
  4KB        ~25 μs/次       ★★ 明显        上下文切换主导
  64KB       ~30 μs/次       ★ 中等         数据拷贝开始成为主要开销
  1MB        ~100 μs/次      ★ 较小         数据拷贝主导，固定开销摊薄
  16MB       ~1 ms/次        几乎无影响     带宽接近原生
```

**★ 结论：FUSE 对小 I/O（≤ 4KB）影响最大，大 I/O 时开销被摊薄。**

---

## 三、优化手段

### 3.1 writeback_cache（写回缓存）

```
默认模式（writethrough）：
  每次 write() → FUSE_WRITE 请求 → 用户态处理 → 等待响应
  ★ 每次写都要经历一次完整的上下文切换

启用 writeback_cache：
  write() → page cache（内核缓存）→ 立即返回
                │
                ▼（后台异步）
          FUSE_WRITE → 用户态处理
  ★ 应用不等待，内核在后台批量刷写

挂载方式：
  ./myfs -o writeback_cache /mnt/myfs

源码位置：
  fs/fuse/file.c: fuse_writepage() — 写回脏页时触发
  fs/fuse/inode.c: 检查 FUSE_WRITEBACK_CACHE 标志
```

**性能影响：** 写操作延迟从 ~25 μs 降低到 ~2 μs（page cache 写入），吞吐提升 5-10 倍。

### 3.2 splice（零拷贝传输）

```
不使用 splice（默认）：
  内核缓冲区 → copy → /dev/fuse 读缓冲（用户态）
  用户态数据 → copy → /dev/fuse 写缓冲（内核态）
  ★ 至少 2 次额外数据拷贝

使用 splice：
  内核缓冲区 → splice → /dev/fuse pipe
  用户态数据 → splice → 内核缓冲区
  ★ 零拷贝：移动页面所有权，不拷贝数据

启用方式：
  1. 在 INIT 协商中设置 FUSE_SPLICE_READ / FUSE_SPLICE_WRITE 标志
  2. libfuse 自动使用 splice（如果内核支持）

源码位置：
  fs/fuse/dev.c: fuse_dev_splice_read() / fuse_dev_splice_write()
```

**性能影响：** 大 I/O 时吞吐提升 20-40%（减少数据拷贝）。

### 3.3 增大 max_write

```
默认 max_write = 128KB（32 页）
增大到 1MB（256 页）：

  小 max_write:
    写 1MB 数据 → 8 次 FUSE_WRITE → 8 次上下文切换

  大 max_write:
    写 1MB 数据 → 1 次 FUSE_WRITE → 1 次上下文切换
    ★ 减少 87.5% 的上下文切换

启用方式：
  在 init 回调中设置：
    conn->want |= FUSE_CAP_BIG_WRITES;
    conn->max_write = 1048576;  // 1MB
```

### 3.4 readahead 预读

```
默认行为：
  顺序读取文件时，每次只读请求的大小
  ★ 每次 read 都要上下文切换

启用预读：
  内核在检测到顺序读模式时自动发起预读请求
  多个预读请求可以合并（async_read）

在 INIT 协商中：
  fuse_init_out.max_readahead = 1048576;  // 1MB 预读

★ 对顺序读场景提升巨大（类似内核文件系统的 readahead 机制）
```

### 3.5 io_uring（实验性，Linux 6.x+）

```
传统 /dev/fuse 通信：
  read() / write() 系统调用 → 上下文切换

io_uring + FUSE（实验性）：
  共享内存环形缓冲区 → 无需系统调用
  ★ 理论上可以消除上下文切换开销

状态：
  内核补丁在开发中（2023-2024）
  预计可减少 50-80% 的 FUSE 开销
```

### 3.6 优化总结

```
┌──────────────────────┬────────────┬────────────┬──────────────────┐
│ 优化手段              │ 适用场景    │ 提升幅度    │ 启用方式          │
├──────────────────────┼────────────┼────────────┼──────────────────┤
│ writeback_cache      │ 写密集      │ 5-10x 写   │ -o writeback_cache│
│ splice               │ 大 I/O     │ 20-40%     │ INIT flag        │
│ 增大 max_write       │ 大写入      │ 减少切换   │ INIT 协商         │
│ readahead            │ 顺序读      │ 3-5x       │ max_readahead    │
│ readdirplus          │ 目录列举    │ 减少往返   │ INIT flag        │
│ no_open_support      │ 频繁 open   │ 减少往返   │ INIT flag        │
│ io_uring（实验）     │ 所有场景    │ 50-80%     │ 内核 6.x+        │
└──────────────────────┴────────────┴────────────┴──────────────────┘
```

---

## 四、用 blktrace + strace 分析 FUSE I/O

### 4.1 strace 分析 FUSE 守护进程

```bash
# 追踪 FUSE 守护进程的 /dev/fuse 通信
strace -p $(pidof my_fuse_fs) -e trace=read,write -T -f

# 输出示例：
# read(3, "\x50\x00\x00\x00\x0f\x00\x00\x00...", 131072) = 80 <0.000032>
#                                                         ↑ 耗时 32μs
#   → 从 /dev/fuse(fd=3) 读取了 80 字节的 FUSE_READ 请求
#
# write(3, "\x21\x00\x00\x00\x00\x00\x00\x00...", 33) = 33 <0.000015>
#                                                          ↑ 耗时 15μs
#   → 向 /dev/fuse(fd=3) 写回了 33 字节的响应（16B header + 17B 数据）

# 分析每个请求的处理时间
strace -p $(pidof my_fuse_fs) -e trace=read,write -T -f 2>&1 | \
  awk '/read\(3/ {split($NF, t, /[<>]/); print "receive: " t[2] "s"}
       /write\(3/ {split($NF, t, /[<>]/); print "respond: " t[2] "s"}'
```

### 4.2 strace 分析应用进程

```bash
# 追踪使用 FUSE 文件系统的应用进程
strace -e trace=openat,read,write,close,stat -T cat /tmp/myfuse/hello

# 输出示例：
# stat("/tmp/myfuse/hello", {...}) = 0 <0.000045>
#   → stat 耗时 45μs（包含 FUSE_GETATTR 往返）
# openat(AT_FDCWD, "/tmp/myfuse/hello", O_RDONLY) = 3 <0.000038>
#   → open 耗时 38μs（包含 FUSE_OPEN 往返）
# read(3, "Hello, FUSE World!\n", 131072) = 19 <0.000035>
#   → read 耗时 35μs（包含 FUSE_READ 往返）
# close(3) = 0 <0.000020>
#   → close 耗时 20μs（包含 FUSE_FLUSH + FUSE_RELEASE）
```

### 4.3 blktrace 分析 FUSE 底层 I/O

```bash
# 如果 FUSE 文件系统底层使用块设备存储
# （例如 encfs、goofys 挂载在本地磁盘上）

# 步骤 1：启动 blktrace
blktrace -d /dev/vdb -o fuse_io -w 30 &

# 步骤 2：在 FUSE 挂载点上执行 I/O
sleep 1
dd if=/dev/zero of=/mnt/myfuse/test.dat bs=4k count=1000

# 步骤 3：分析
wait
blkparse -i fuse_io -o fuse_parsed.txt
btt -i fuse_io.bin -o fuse_btt

# ★ 观察要点：
# 1. FUSE 守护进程产生的 I/O 模式（Q/D/C 事件）
# 2. 与直接写 /dev/vdb 的 I/O 模式对比
# 3. 额外的元数据 I/O（FUSE 文件系统可能需要额外的元数据更新）

# 对比实验
blktrace -d /dev/vdb -o direct_io -w 30 &
sleep 1
dd if=/dev/zero of=/data/test.dat bs=4k count=1000  # 直接写 ext4
wait
blkparse -i direct_io -o direct_parsed.txt
btt -i direct_io.bin -o direct_btt

# 比较 fuse_btt 和 direct_btt 的 Q2C 延迟和 IOPS
```

### 4.4 perf 分析 FUSE 热点

```bash
# 录制 FUSE I/O 期间的 perf 数据
perf record -g -p $(pidof my_fuse_fs) -- sleep 10

# 在另一个终端触发 I/O
dd if=/dev/zero of=/mnt/myfuse/test.dat bs=4k count=10000

# 分析热点函数
perf report

# 预期看到的热点函数：
#   fuse_dev_read          — 从 /dev/fuse 读取请求
#   fuse_dev_write         — 向 /dev/fuse 写入响应
#   copy_to_user           — 数据拷贝到用户空间
#   copy_from_user         — 从用户空间拷贝数据
#   context_switch         — 上下文切换
#   schedule               — 调度器
```

---

## 五、FUSE 性能优化检查清单

```
在部署 FUSE 文件系统前，检查以下配置：

□ 1. 协议版本
     init 回调中确认 proto_major >= 7.23（支持 writeback_cache）

□ 2. writeback_cache
     如果写操作频繁，启用 -o writeback_cache

□ 3. max_write
     设置足够大的 max_write（至少 128KB，建议 1MB）

□ 4. max_readahead
     设置足够大的 max_readahead（建议 128KB-1MB）

□ 5. splice
     确认 INIT flag 包含 FUSE_SPLICE_READ | FUSE_SPLICE_WRITE

□ 6. 线程数
     使用多线程模式（fuse_loop_mt）提高并发处理能力

□ 7. direct_io vs cache
     根据场景选择：
     - 频繁读写 → 使用 page cache（不设置 direct_io）
     - 大文件顺序读写 → direct_io 可能更好（避免 page cache 抖动）

□ 8. 底层存储
     FUSE 守护进程的底层存储使用快速设备（NVMe > SSD > HDD）
     ★ 底层越慢，FUSE 额外开销占比越小
```

---

## 六、与 blktrace 的交叉分析

FUSE 守护进程如果对底层块设备进行 I/O，这些 I/O 会经过 block layer，可以被 blktrace 捕获：

```
应用进程                    FUSE 守护进程              block layer
────────                    ────────────              ───────────
read(/mnt/fuse/file)
  │
  ▼
FUSE 内核模块
  │ FUSE_READ 请求
  ▼
  │ read(/dev/fuse)
  │ 用户态处理
  │ 需要从磁盘读取数据
  │
  ▼
  │ open/read 底层文件
  │
  ▼                                ┌─────────────────┐
  │ vfs_read()                     │ block layer      │
  │   → ext4_read_iter()           │ Q → I → D → C   │
  │     → submit_bio()         ───→│                  │
  │                                │ ★ blktrace 可以  │
  │                                │   在这里追踪！    │
  │                                └─────────────────┘
  │ blktrace 看到的：
  │   Q: bio 入队（FUSE 守护进程的 PID）
  │   D: 下发到设备驱动
  │   C: 设备完成 I/O
  │
  │ ★ blktrace 中的 pid 是 FUSE 守护进程的 PID
  │   不是应用进程的 PID！
```

**分析方法：**

```bash
# 1. 同时运行 blktrace 和 strace
blktrace -d /dev/vdb -o fuse_block -w 30 &
strace -p $(pidof fuse_daemon) -e trace=read,write -T -o fuse_strace.log &

# 2. 在 FUSE 挂载点执行 I/O
sleep 1
dd if=/dev/zero of=/mnt/fuse/test.dat bs=4k count=100

# 3. 分析
blkparse -i fuse_block -o fuse_block_parsed.txt

# blkparse 输出中，找到 FUSE 守护进程 PID 对应的 I/O：
# 253,16  1  0.001234  5678  Q  WS  12345 + 8 [fuse_daemon]
#                                         ↑ PID
# 253,16  1  0.001300  5678  D  WS  12345 + 8 [fuse_daemon]
# 253,16  1  0.001450  5678  C  WS  12345 + 8 [0]
#                                          ↑ C 事件在中断上下文，PID 可能是 0

# 对比 strace 日志中的时间戳，可以关联 FUSE 请求和底层 I/O
```

---

## 七、调用链总结

```
FUSE I/O 性能分析工具链：

  ┌─────────────────────────────────────────────────────────────────┐
  │  分析层次                  工具              观察点              │
  ├───────────────────────────┼────────────────┼───────────────────┤
  │  应用层                   │ strace -T      │ 系统调用延迟       │
  │  （cat/dd/fio 等）        │                │ （含 FUSE 开销）   │
  ├───────────────────────────┼────────────────┼───────────────────┤
  │  FUSE 协议层              │ strace -p fuse │ /dev/fuse 的       │
  │  （FUSE 守护进程）        │ pidof fuse     │ read/write 延迟    │
  ├───────────────────────────┼────────────────┼───────────────────┤
  │  FUSE 内核模块            │ perf record    │ 热点函数           │
  │  （fuse.ko）             │ ftrace         │ 上下文切换次数     │
  ├───────────────────────────┼────────────────┼───────────────────┤
  │  Block layer              │ blktrace       │ 底层 I/O 的        │
  │  （底层块设备）           │ blkparse/btt   │ Q2D/D2C/Q2C 延迟  │
  ├───────────────────────────┼────────────────┼───────────────────┤
  │  设备层                   │ blkparse       │ 设备处理时间       │
  │  （磁盘/SSD）            │ iostat         │ 利用率、队列深度   │
  └───────────────────────────┴────────────────┴───────────────────┘
```
