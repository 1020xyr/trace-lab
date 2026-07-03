# FUSE（Filesystem in Userspace）问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: FUSE 文件系统性能瓶颈通常在哪里？](#q1-fuse-文件系统性能瓶颈通常在哪里)
- [Q2: writeback_cache 为什么能提升 FUSE 写性能？](#q2-writeback_cache-为什么能提升-fuse-写性能)

---

## Q1: FUSE 文件系统性能瓶颈通常在哪里？

**日期：** 2026-07-04  
**场景：** FUSE 文件系统（如 SSHFS/encfs）的 4K 随机读 IOPS 只有 ext4 的 1/5，延迟高 5 倍  
**相关文件：** `docs/fuse/reading/04_fuse_performance.md`, `docs/fuse/reading/07_performance_optimization.md`  
**源码位置：** `fs/fuse/dev.c` — `fuse_dev_read()` / `fuse_dev_write()`; `fs/fuse/file.c` — `fuse_read_iter()`

### 回答

**★ FUSE 性能瓶颈主要在上下文切换开销（占 ~60%），其次是数据拷贝（~20%）和调度延迟（~15%）。瓶颈层次取决于 I/O 大小。**

#### 四大开销来源

```
┌──────────────────┬────────┬──────────────────────────────────────────┐
│ 开销来源          │ 占比   │ 机制                                      │
├──────────────────┼────────┼──────────────────────────────────────────┤
│ ★ 上下文切换      │ ~60%  │ 每次 I/O 需要 4 次上下文切换              │
│                  │        │ 内核→FUSE守护进程→内核→应用              │
│                  │        │ 每次切换 ~1-2μs → 总计 ~4-8μs            │
├──────────────────┼────────┼──────────────────────────────────────────┤
│ 数据拷贝          │ ~20%  │ 数据在内核↔用户态间额外拷贝 2-3 次        │
│                  │        │ （内核文件系统只需 1 次 copy_to_user）    │
├──────────────────┼────────┼──────────────────────────────────────────┤
│ 调度延迟          │ ~15%  │ 内核线程阻塞等待守护进程响应              │
│                  │        │ 唤醒延迟 ~1-10μs（负载高时可达 ~100μs）  │
├──────────────────┼────────┼──────────────────────────────────────────┤
│ 缓存失效          │ ~5%   │ 默认无法使用内核 page cache               │
│                  │        │ 每次 read 都要经过 /dev/fuse 往返        │
└──────────────────┴────────┴──────────────────────────────────────────┘
```

#### 瓶颈层次与 I/O 大小的关系

```
小 I/O（≤ 4KB）：
  ★ 上下文切换是主要瓶颈（固定开销 ~4-8μs）
  4KB 数据的传输时间 ~0.5μs，但 FUSE 额外开销 ~25μs
  → 开销占比 > 95% → IOPS 下降 5-7 倍

大 I/O（≥ 1MB）：
  ★ 数据拷贝是主要瓶颈
  1MB 数据的传输时间 ~100μs，FUSE 额外拷贝增加 ~50μs
  → 开销占比 ~33% → 吞吐下降 1.3-1.5 倍
```

#### 诊断方法

```bash
# 1. strace 看 /dev/fuse 通信频率
strace -p $(pidof fuse_daemon) -e trace=read,write -c
# calls = FUSE 请求数，usecs/call = 每次请求处理时间

# 2. perf stat 看上下文切换
perf stat -e context-switches -p $(pidof fuse_daemon) -- sleep 10
# 高 context-switches = 上下文切换是瓶颈

# 3. blktrace 看底层 I/O
blktrace -d /dev/vda -o fuse_io -w 30
# 底层 I/O 延迟 vs FUSE 端到端延迟的差异 = FUSE 开销
```

#### 优化方向

| 瓶颈 | 优化手段 | 预期效果 |
|------|---------|---------|
| 上下文切换 | writeback_cache、增大 max_write | ★ 减少 80-90% 切换 |
| 数据拷贝 | splice | 减少 20-40% 拷贝 |
| 调度延迟 | 多线程（fuse_loop_mt）、绑核 | 减少唤醒延迟 |
| 缓存失效 | 不设置 direct_io、启用 readahead | 利用 page cache |

**一句话总结：** FUSE 性能瓶颈主要在上下文切换（4 次/IO，占 ~60% 开销）。小 I/O 时上下文切换是主因，大 I/O 时数据拷贝是主因。用 writeback_cache + splice + 增大 max_write 可综合优化。

---

## Q2: writeback_cache 为什么能提升 FUSE 写性能？

**日期：** 2026-07-04  
**场景：** 启用 writeback_cache 后 4K 写 IOPS 从 30K 提升到 150K（5 倍提升）  
**相关文件：** `docs/fuse/reading/07_performance_optimization.md`  
**源码位置：** `fs/fuse/file.c` — `fuse_writepage()`; `fs/fuse/inode.c` — FUSE_WRITEBACK_CACHE 标志检查

### 回答

**★ writeback_cache 将同步写变为异步写：write() 只写 page cache（~1μs），不等待 FUSE 守护进程响应（~25μs），内核在后台批量刷写脏页。**

#### 默认模式 vs writeback_cache

```
默认模式（writethrough）：

  write(fd, buf, 4K)
       │
       ▼
  FUSE 内核模块构造 FUSE_WRITE 请求
       │
       ▼
  写入 /dev/fuse → 唤醒守护进程
       │
       ▼
  ★ 阻塞等待守护进程响应（~25-50μs）
       │
       ▼
  守护进程处理完毕 → 内核收到响应
       │
       ▼
  write() 返回
  ★ 延迟 = ~25-50μs

writeback_cache 模式：

  write(fd, buf, 4K)
       │
       ▼
  数据写入内核 page cache（内存操作）
       │
       ▼
  标记页面为 dirty
       │
       ▼
  ★ write() 立即返回（不等待守护进程）
  ★ 延迟 = ~1-2μs（仅内存操作）

       │
       │ （后台异步）
       ▼
  内核回写线程定期扫描脏页
       │
       ▼
  批量构造 FUSE_WRITE 请求 → 发送给守护进程
  ★ 多个脏页合并为一次请求 → 减少上下文切换
```

#### 性能对比

| 指标 | writethrough | writeback_cache | 提升 |
|------|-------------|----------------|------|
| write() 延迟 | ~25-50 μs | ★ ~1-2 μs | 15-50 倍 |
| 4K 写 IOPS | 20K-40K | ★ 100K-200K | 3-5 倍 |
| 顺序写吞吐 | ~100-300 MB/s | ★ ~800-1500 MB/s | 4-8 倍 |
| 上下文切换/I/O | 2 次 | ★ ~0.2 次（批量） | 10 倍 |

#### 启用方法

```bash
# 挂载时启用
./my_fuse_fs -o writeback_cache /mnt/myfs

# 源码中启用（libfuse 3.x）
static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    cfg->writeback = 1;
    conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    return NULL;
}
```

#### 注意事项

| 注意点 | 说明 |
|--------|------|
| 内核版本 | 需要 ≥ 3.15（FUSE_WRITEBACK_CACHE 标志） |
| 数据安全 | ⚠️ 脏页未刷写时断电可能丢数据 |
| fsync() | 调用 fsync() 时内核会等待所有脏页刷写 |
| direct_io | 与 writeback_cache 互斥（direct_io 绕过 page cache） |
| 共享文件 | 多客户端共享文件时，writeback_cache 可能导致一致性延迟 |

**一句话总结：** writeback_cache 将每次 write() 从"同步等待 FUSE 守护进程"（~25μs）变为"只写 page cache 立即返回"（~1μs），内核在后台批量合并脏页发送给守护进程。写性能提升 5-10 倍，代价是断电时脏页可能丢失。

---

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
