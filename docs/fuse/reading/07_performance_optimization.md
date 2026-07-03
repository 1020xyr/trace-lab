# FUSE 性能优化与诊断专题

> 面向性能分析工程师的 FUSE 深度优化指南
> 重点关注：writeback_cache 原理、splice 优化、io_uring 最新进展、诊断流程
> 预计阅读时间：15 分钟

---

## 阅读要点

1. ★ writeback_cache 的原理和效果
2. ★ splice 优化（splice_read/splice_write）
3. ★ max_read/max_write 调大减少请求次数
4. ★ FUSE io_uring 补丁（最新进展）
5. ★ FUSE 性能诊断流程（strace + perf stat + blktrace）

---

## 一、writeback_cache 深度解析

### 1.1 默认模式（writethrough）的问题

```
每次 write() 的完整路径（默认 writethrough）：

  应用进程 write(fd, buf, 4096)
       │
       ▼
  ┌── VFS ──────────────────────────────────────────────┐
  │  检测到 FUSE 文件系统                               │
  └──────────────────────────────────────────────────────┘
       │
       ▼
  ┌── FUSE 内核模块 ────────────────────────────────────┐
  │  1. 构造 FUSE_WRITE 请求（in_header + write_in）    │
  │  2. 将用户数据拷贝到请求缓冲区                       │
  │  3. 写入 /dev/fuse → 唤醒 FUSE 守护进程            │
  │  4. ★ 阻塞等待守护进程响应                          │
  │     （当前线程被挂起，触发上下文切换）               │
  └──────────────────────────────────────────────────────┘
       │                          │
       ▼                          ▼
  内核线程阻塞              FUSE 守护进程被唤醒
       │                     │
       │                     ▼
       │                   read(/dev/fuse) → 获取 FUSE_WRITE 请求
       │                     │
       │                     ▼
       │                   处理写入（写到本地磁盘/网络等）
       │                     │
       │                     ▼
       │                   write(/dev/fuse) → 发送响应
       │                     │
       ▼                     ▼
  内核线程被唤醒         守护进程继续下一个请求
       │
       ▼
  write() 返回给应用

  ★ 延迟 = 上下文切换 × 2 + 守护进程处理时间 ≈ 25-50 μs
  ★ 每次 write() 都有一次完整的内核↔用户态往返！
```

### 1.2 writeback_cache 模式

```
启用 writeback_cache 后的 write() 路径：

  应用进程 write(fd, buf, 4096)
       │
       ▼
  ┌── VFS ──────────────────────────────────────────────┐
  │  检测到 FUSE + writeback_cache 已启用               │
  └──────────────────────────────────────────────────────┘
       │
       ▼
  ┌── page cache ──────────────────────────────────────┐
  │  ★ 将数据写入内核 page cache（内存）               │
  │  标记对应页面为 dirty（脏页）                       │
  │  ★ 立即返回！不等待 FUSE 守护进程                  │
  └──────────────────────────────────────────────────────┘
       │
       ▼
  write() 返回给应用（★ 延迟 ~1-2 μs，仅内存操作）

       │
       │ （后台异步）
       ▼
  ┌── 内核回写线程（pdflush/flush）────────────────────┐
  │  定期扫描脏页                                      │
  │  发现 FUSE 文件的脏页                              │
  │  构造 FUSE_WRITE 请求 → 发送给守护进程             │
  │  ★ 批量回写：多个脏页合并为一次 FUSE_WRITE         │
  └──────────────────────────────────────────────────────┘
```

### 1.3 性能对比

```
┌─────────────────────────┬──────────────────┬──────────────────────┐
│ 指标                     │ writethrough     │ writeback_cache      │
├─────────────────────────┼──────────────────┼──────────────────────┤
│ write() 延迟             │ ~25-50 μs       │ ★ ~1-2 μs           │
│ （应用感知延迟）         │ （等守护进程）    │ （只写 page cache）  │
├─────────────────────────┼──────────────────┼──────────────────────┤
│ 4K 写 IOPS               │ 20,000-40,000   │ ★ 100,000-200,000   │
├─────────────────────────┼──────────────────┼──────────────────────┤
│ 顺序写吞吐               │ ~100-300 MB/s   │ ★ ~800-1500 MB/s    │
├─────────────────────────┼──────────────────┼──────────────────────┤
│ 上下文切换次数           │ 每次 write 2 次  │ ★ 后台批量，大幅减少 │
├─────────────────────────┼──────────────────┼──────────────────────┤
│ 数据安全                 │ ★ 写即持久化     │ ⚠️ 脏页未刷写时     │
│                         │                  │ 断电可能丢数据       │
├─────────────────────────┼──────────────────┼──────────────────────┤
│ fsync() 行为             │ 立即返回         │ 等待所有脏页刷写     │
└─────────────────────────┴──────────────────┴──────────────────────┘
```

### 1.4 启用方法

```bash
# 挂载时启用
./my_fuse_fs -o writeback_cache /mnt/myfs

# 或通过 fusermount
fusermount -o writeback_cache /mnt/myfs

# 源码中启用（libfuse 3.x）
static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    cfg->writeback = 1;  // ★ 启用 writeback_cache
    conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    return NULL;
}
```

### 1.5 writeback_cache 的限制

| 限制 | 说明 |
|------|------|
| 需要内核 ≥ 3.15 | 早期内核不支持 FUSE_WRITEBACK_CACHE |
| FUSE 守护进程必须支持 | INIT 协商中设置 FUSE_WRITEBACK_CACHE 标志 |
| 不适合 direct_io | direct_io 绕过 page cache，writeback_cache 无效 |
| 断电风险 | 脏页未刷写时断电可能丢失数据 |
| 不适合共享文件 | 多客户端通过 FUSE 共享文件时，writeback_cache 可能导致一致性延迟 |

---

## 二、splice 优化

### 2.1 传统 copy 方式的开销

```
FUSE read（不使用 splice）：

  内核空间                         用户空间（FUSE 守护进程）
  ┌──────────────┐                ┌──────────────────┐
  │ 用户缓冲区   │                │                  │
  │ (等待数据)   │                │ /dev/fuse 缓冲   │
  │              │                │ (接收请求)       │
  └──────────────┘                └──────────────────┘
         ▲                               │
         │                               ▼
         │                        read(/dev/fuse)
         │                        获取 FUSE_READ 请求
         │                               │
         │                               ▼
         │                        从底层读取数据
         │                               │
         │                        write(/dev/fuse)
         │                        ★ copy_from_user()
         │                        将数据从用户态拷贝
         │                        到内核 FUSE 缓冲区
         │                               │
         │  copy_to_user()                │
         │  ★ 从 FUSE 内核缓冲区          │
         │  拷贝到用户缓冲区              │
         │                               │

  ★ 2 次额外拷贝：
    copy_from_user: 守护进程 → 内核 FUSE 缓冲区
    copy_to_user:   内核 FUSE 缓冲区 → 用户缓冲区
```

### 2.2 splice 零拷贝方式

```
FUSE read（使用 splice）：

  内核空间                         用户空间（FUSE 守护进程）
  ┌──────────────┐                ┌──────────────────┐
  │ 用户缓冲区   │                │                  │
  │ (等待数据)   │                │ pipe 缓冲区      │
  │              │                │ (零拷贝中转)     │
  └──────────────┘                └──────────────────┘
         ▲                               │
         │                               ▼
         │                        splice_read(/dev/fuse)
         │                        ★ 移动页面所有权
         │                        不拷贝数据！
         │                               │
         │                               ▼
         │                        处理数据（通过 pipe）
         │                               │
         │                        splice_write(pipe)
         │                        ★ 移动页面所有权
         │                        到内核缓冲区
         │                               │
         │  copy_to_user()               │
         │  （最终一步仍需拷贝）          │
         │                               │

  ★ 减少 1 次拷贝（splice 移动页面而非拷贝）
    大 I/O 时效果显著
```

### 2.3 splice 的启用和效果

```c
/*
 * 源码位置：fs/fuse/dev.c — fuse_dev_splice_read() / fuse_dev_splice_write()
 *
 * FUSE 守护进程在 INIT 协商时声明支持 splice：
 *   in->flags |= FUSE_SPLICE_READ | FUSE_SPLICE_WRITE
 *
 * libfuse 自动检测并使用 splice（如果内核和守护进程都支持）
 */

// 启用方式（libfuse 3.x）
static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    conn->want |= FUSE_CAP_SPLICE_READ | FUSE_CAP_SPLICE_WRITE;
    // libfuse 会自动启用 splice（如果内核支持）
    return NULL;
}
```

**性能影响：**

| I/O 大小 | 不用 splice | 使用 splice | 提升 |
|---------|------------|------------|------|
| 4KB | ~25 μs | ~22 μs | ~12%（开销主要在上下文切换） |
| 64KB | ~35 μs | ~28 μs | ~20% |
| 1MB | ~200 μs | ~140 μs | ★ ~30%（数据拷贝占比大） |
| 16MB | ~2 ms | ~1.3 ms | ★ ~35% |

**★ 结论：splice 对大 I/O 效果最好（数据拷贝是主要开销），对小 I/O 提升有限（上下文切换是主要开销）。**

---

## 三、max_read / max_write 调大减少请求次数

### 3.1 默认值的限制

```
FUSE 协议默认：
  max_write = 128 KB（32 页）
  max_read  = 由 max_readahead 决定（通常 128 KB）

问题：写入 1 MB 数据时
  write(1MB) → 拆分为 8 次 FUSE_WRITE → 8 次上下文切换
  ★ 每次上下文切换 ~2-5 μs → 额外开销 16-40 μs
```

### 3.2 调大后的效果

```
调大 max_write = 1 MB：
  write(1MB) → 1 次 FUSE_WRITE → 1 次上下文切换
  ★ 减少 87.5% 的上下文切换

调大 max_readahead = 1 MB：
  顺序读取 1MB 文件时：
    默认：内核发起多个小 read 请求（每个 128KB）
    调大后：内核一次预读 1MB → 减少往返次数
```

### 3.3 配置方法

```c
/* libfuse 3.x */
static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    conn->want |= FUSE_CAP_BIG_WRITES;
    
    // ★ 调大 max_write（默认 128KB → 1MB）
    conn->max_write = 1048576;
    
    // ★ 调大 max_readahead（默认 128KB → 1MB）
    conn->max_readahead = 1048576;
    
    // 在 cfg 中也设置（libfuse 3.x）
    cfg->max_read = 1048576;
    
    return NULL;
}
```

```
┌────────────────┬────────────┬──────────────┬──────────────┐
│ max_write      │ 写 1MB 的  │ 上下文切换   │ 写入吞吐     │
│                │ FUSE_WRITE │ 次数减少     │              │
│                │ 请求次数   │              │              │
├────────────────┼────────────┼──────────────┼──────────────┤
│ 128KB (默认)   │ 8 次       │ —            │ ~200 MB/s    │
│ 256KB          │ 4 次       │ 50%          │ ~350 MB/s    │
│ 512KB          │ 2 次       │ 75%          │ ~500 MB/s    │
│ ★ 1MB          │ 1 次       │ ★ 87.5%     │ ★ ~700 MB/s │
└────────────────┴────────────┴──────────────┴──────────────┘
```

---

## 四、FUSE io_uring 补丁（最新进展）

### 4.1 传统 /dev/fuse 通信的瓶颈

```
传统 /dev/fuse 通信模型：

  内核 FUSE 模块                    FUSE 守护进程
  ┌──────────────┐                ┌──────────────────┐
  │ 请求入队     │  write()        │                  │
  │ ──────────── │ ──────────→     │ read(/dev/fuse)  │
  │              │  系统调用       │                  │
  │ 阻塞等待     │                │ 处理请求         │
  │              │  read()         │                  │
  │              │ ←──────────     │ write(/dev/fuse) │
  │ 唤醒处理     │  系统调用       │                  │
  └──────────────┘                └──────────────────┘

  ★ 每次请求 = 2 次系统调用（read + write）
  ★ 每次系统调用 = 1 次上下文切换
  ★ 这是 FUSE 性能的根本瓶颈
```

### 4.2 io_uring + FUSE 的设计

```
io_uring + FUSE（实验性，Bernd Schubert 主导开发）：

  内核 FUSE 模块                    FUSE 守护进程
  ┌──────────────┐                ┌──────────────────┐
  │ 请求入队     │                │                  │
  │ ──────────── │                │                  │
  │              │                │                  │
  │ io_uring     │  ★ 共享内存   │ io_uring         │
  │ submission   │  环形缓冲区   │ completion       │
  │ queue (SQ)   │ ──────────→   │ queue (CQ)       │
  │              │  零拷贝       │                  │
  │              │                │                  │
  └──────────────┘                └──────────────────┘

  ★ 无需系统调用！请求和响应通过共享内存传递
  ★ 消除了 read/write 系统调用的上下文切换
  ★ 理论上可减少 50-80% 的 FUSE 开销
```

### 4.3 开发状态

```
┌──────────┬──────────────────────────────────────────────────┐
│ 时间      │ 进展                                              │
├──────────┼──────────────────────────────────────────────────┤
│ 2023 Q2  │ Bernd Schubert 提交第一版 RFC 补丁               │
│ 2023 Q4  │ v2 补丁集，增加 splice 支持                       │
│ 2024 Q1  │ v3-v5 迭代，API 稳定化                            │
│ 2024 Q2  │ 进入 linux-fsdevel 讨论                           │
│ 2024 H2  │ 持续改进中，预计 Linux 6.12+ 可能合入             │
│ 2025     │ 社区测试和性能验证                                │
│ 2026     │ ★ 部分发行版已开始 backport 补丁                  │
└──────────┴──────────────────────────────────────────────────┘

★ 当前状态：补丁尚未合入主线，但在活跃开发中
  性能数据（来自补丁作者的测试）：
    - 4K 随机读 IOPS 提升 ~50%
    - 元数据操作延迟降低 ~40%
    - 上下文切换次数减少 ~70%
```

### 4.4 io_uring FUSE 的使用方式（预期）

```bash
# 预期使用方式（补丁合入后）：
# 挂载时指定使用 io_uring
./my_fuse_fs -o io_uring /mnt/myfs

# 或通过 mount 选项
mount -t fuse myfs /mnt/myfs -o io_uring

# FUSE 守护进程需要使用支持 io_uring 的 libfuse 版本
# （预计 libfuse 4.x 或 libfuse 3.x 的后续版本）
```

---

## 五、FUSE 性能诊断流程

### 5.1 诊断总流程

```
FUSE 文件系统性能问题
         │
         ▼
  ┌── 第 1 步：确认瓶颈层次 ───────────────────────┐
  │                                                 │
  │  A. strace -p fuse_pid → /dev/fuse 通信频率    │
  │  B. perf stat -p fuse_pid → context-switches   │
  │  C. blktrace → 底层 I/O 延迟                   │
  │                                                 │
  └─────────────────────────────────────────────────┘
         │
    ┌────┴────┬──────────┐
    ▼         ▼          ▼
  FUSE 协议  上下文     底层 I/O
  层瓶颈     切换瓶颈   瓶颈
```

### 5.2 strace 分析 /dev/fuse 通信

```bash
# ★ 追踪 FUSE 守护进程的 /dev/fuse 读写频率
FUSE_PID=$(pidof my_fuse_fs)
strace -p $FUSE_PID -e trace=read,write -T -c

# 关注：
# 1. read/write 的调用次数 → 等于 FUSE 请求数
# 2. 每次 read 的大小 → 等于 FUSE 请求的大小
# 3. 每次 read/write 的耗时 → 包含等待和传输时间

# 示例输出：
# % time     seconds  usecs/call     calls    errors syscall
# ------ ----------- ----------- --------- --------- --------
#  55.30    0.553000          27     20000           read
#  44.70    0.447000          22     20000           write
# ★ read 和 write 各 20000 次 → 20000 个 FUSE 请求
# ★ 每次 read ~27μs → 等待请求到达 + 读取请求数据
# ★ 每次 write ~22μs → 发送响应数据

# 进一步查看请求大小：
strace -p $FUSE_PID -e trace=read,write 2>&1 | head -10
# read(3, "\x50\x00\x00\x00\x0f\x00\x00\x00...", 131072) = 80
#                                                         ^^^
#                                          ★ 80 字节 = FUSE_GETATTR 请求
# write(3, "\x21\x00\x00\x00\x00\x00\x00\x00...", 33) = 33
#                                                    ^^
#                                          ★ 33 字节 = GETATTR 响应

# ★ 通过请求/响应大小可以推断 FUSE opcode：
#   80 字节请求 → GETATTR (in_header 40B + getattr_in 8B + padding)
#   56 字节请求 → READ (in_header 40B + read_in 16B)
#   72 字节请求 → WRITE (in_header 40B + write_in 32B)
#   大量小请求 → 元数据密集型操作
#   大量大请求 → 数据传输密集型操作
```

### 5.3 perf stat 分析上下文切换

```bash
# ★ 统计 FUSE I/O 期间的上下文切换
FUSE_PID=$(pidof my_fuse_fs)

# 在另一个终端触发 I/O
dd if=/dev/zero of=/mnt/myfs/test.dat bs=4k count=10000 &

# 统计 FUSE 守护进程的 context-switches
perf stat -e context-switches,cpu-migrations,cycles,instructions \
    -p $FUSE_PID -- sleep 10

# 输出示例：
#        380,000      context-switches          ← ★ 38K 次/10s = 3800 次/s
#              0      cpu-migrations
#    850,000,000      cycles
#    420,000,000      instructions
#
# ★ 分析：
#   4K write × 10000 次 / 10s = 1000 次/s
#   context-switches = 3800 次/s
#   → 每次 write 触发 ~3.8 次上下文切换
#   → 接近理论值 4 次（2 次请求 + 2 次响应）
#
# ★ 优化后（writeback_cache）：
#   context-switches 应降低到 ~500 次/s（减少 ~87%）

# IPC 分析：
#   IPC = instructions / cycles = 420M / 850M = 0.49
#   ★ IPC < 0.5 → CPU 经常等待（等待数据/调度）
#   这是 FUSE 的典型特征：大量时间在上下文切换中
```

### 5.4 blktrace 追踪 FUSE 底层 I/O

```bash
# ★ FUSE 守护进程的写入最终会经过 block layer
#   FUSE write → FUSE 守护进程处理 → 写入底层 ext4 → block layer

# 启动 blktrace
blktrace -d /dev/vda -o fuse_underlying -w 30 &
sleep 1

# 在 FUSE 挂载点执行写入
dd if=/dev/zero of=/mnt/myfs/test.dat bs=4k count=1000

# 等待完成
wait

# 分析底层 I/O
blkparse -i fuse_underlying -o fuse_blk.txt
btt -i fuse_underlying.bin -o fuse_btt

# ★ btt 输出分析：
# ALL        Q2D       D2C       Q2C
# avg     8.5μs    120.3μs    128.8μs
#
# ★ 关键观察：
#   1. Q2D = 8.5μs（正常，内核调度开销）
#   2. D2C = 120μs（设备处理时间，正常 NVMe）
#   3. 但 FUSE 端到端延迟 = ~25-50μs（strace 测得）
#   4. 差异 = FUSE 协议层开销（25-50μs - 底层 I/O 128μs）
#
# ★ 如果底层 I/O 延迟 < FUSE 端到端延迟：
#   → 瓶颈在 FUSE 协议层（上下文切换 + 数据拷贝）
#   → 优化 writeback_cache/splice/max_write
#
# ★ 如果底层 I/O 延迟 ≈ FUSE 端到端延迟：
#   → 瓶颈在底层存储设备
#   → 优化设备/更换更快的 SSD
```

### 5.5 综合诊断清单

```
FUSE 性能诊断清单：

□ 1. strace -p fuse_pid -e trace=read,write -c
     → /dev/fuse 的读写频率 = FUSE 请求频率
     → 频率高 → 考虑增大 max_write/max_read

□ 2. perf stat -e context-switches -p fuse_pid
     → 上下文切换次数 / I/O 次数 = 每次 I/O 的切换数
     → > 4 次/I/O → 异常（可能有额外的调度）
     → writeback_cache 后应大幅降低

□ 3. blktrace 追踪底层设备
     → 底层 I/O 延迟 vs FUSE 端到端延迟
     → 差异大 → 瓶颈在 FUSE 协议层
     → 差异小 → 瓶颈在底层存储

□ 4. 检查 FUSE 挂载选项
     □ writeback_cache 是否启用？
     □ max_write 是否足够大？
     □ max_readahead 是否足够大？
     □ splice 是否启用？

□ 5. 检查 FUSE 守护进程
     □ 是否使用多线程（fuse_loop_mt）？
     □ 线程数是否足够？
     □ 是否有内部锁竞争？
     □ 是否绑核？

□ 6. 检查内核参数
     □ /sys/fs/fuse/connections/ 的 max_background
     □ /sys/fs/fuse/connections/ 的 congestion_threshold
```

---

## ★ 总结：FUSE 优化效果速查

```
优化手段              │ 适用场景      │ 预期提升          │ 启用方式
─────────────────────┼──────────────┼──────────────────┼──────────────────
writeback_cache      │ 写密集        │ ★ 5-10x 写性能   │ -o writeback_cache
splice               │ 大 I/O       │ 20-40%           │ INIT flag
增大 max_write       │ 大写入        │ 减少 87% 切换     │ INIT 协商
增大 max_readahead   │ 顺序读        │ 3-5x 读性能       │ INIT 协商
多线程               │ 并发请求      │ 2-4x 吞吐        │ fuse_loop_mt
io_uring（实验）     │ 所有场景      │ ★ 50-80%         │ 内核 6.12+（预计）
绑核                 │ CPU 密集      │ 减少 cache miss   │ taskset/sched_setaffinity
```
