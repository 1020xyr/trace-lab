# fio 学习路线

> 从 I/O 引擎到延迟统计，理解 fio 如何生成可控的 I/O 负载

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 源码阅读路线图](#2-源码阅读路线图)
- [3. 核心概念](#3-核心概念)
- [4. 动手实验清单](#4-动手实验清单)
- [5. 关键源码文件索引](#5-关键源码文件索引)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                    用户命令行                                │
│  fio --name=test --ioengine=libaio --direct=1 --rw=randwrite│
├─────────────────────────────────────────────────────────────┤
│                    初始化层                                  │
│  fio.c: main() → init.c: parse_options() → job 配置解析     │
│  options.c: 6000+ 行选项定义                                 │
├─────────────────────────────────────────────────────────────┤
│                    执行层                                    │
│  backend.c: thread_main() → do_io() 主循环                  │
│  io_u.c: io_u 管理（分配、填充、回收）                        │
│  filesetup.c: 文件打开、offset 计算、随机/顺序模式           │
├─────────────────────────────────────────────────────────────┤
│                    I/O 引擎层（插件式）                       │
│  ioengines.c: 引擎注册/加载/调用                             │
│  engines/sync.c:      read()/write()/pread()/pwrite()       │
│  engines/libaio.c:    io_submit()/io_getevents()            │
│  engines/io_uring.c:  io_uring_submit()/io_uring_wait_cqe() │
│  engines/posixaio.c:  aio_read()/aio_write()                │
│  engines/mmap.c:      mmap + memcpy                         │
│  ...40+ 引擎                                                │
├─────────────────────────────────────────────────────────────┤
│                    统计层                                    │
│  stat.c: 延迟/吞吐/IOPS 统计                                │
│  eta.c: 实时进度显示                                        │
└─────────────────────────────────────────────────────────────┘
```

### 数据流全路径

```
fio 命令行参数 / job 文件
    │
    ▼
parse_options()                    ← init.c: 解析命令行/job 文件
    │
    ▼
load_ioengine()                    ← ioengines.c: 加载 I/O 引擎
    │
    ▼
thread_main()                      ← backend.c: 每个 job 一个线程/进程
    │
    ├── setup_files()              ← filesetup.c: 创建/打开文件、计算 offset
    ├── init_io_u()                ← backend.c: 分配 io_u 池
    │
    ▼
do_io() 主循环                     ← backend.c: 核心 I/O 循环
    │
    ├── get_io_u(td)               ← io_u.c: 取一个空闲 io_u
    │     ├── 确定 ddir（读/写/trim）
    │     ├── 确定 offset（顺序/随机/zipf）
    │     └── 填充 buf（验证模式/零填充/随机）
    │
    ├── td_io_queue(td, io_u)      ← ioengines.c: 调用引擎的 queue()
    │     └── engine->queue()      ← 引擎实现（sync: write(); libaio: io_submit()）
    │
    ├── td_io_getevents()          ← ioengines.c: 等待完成（异步引擎）
    │     └── engine->getevents()  ← 引擎实现（libaio: io_getevents()）
    │
    └── io_u 回收 + 延迟统计       ← stat.c: 记录 slat/clat/lat/bw
    │
    ▼
show_thread_status()               ← stat.c: 输出统计报告
```

---

## 2. 源码阅读路线图

### Step 1: 核心数据结构 — io_u 和 thread_data

**目标：** 理解 fio 的两个核心结构体

**文件位置：**
- `src/fio/io_u.h` — `struct io_u` 定义
- `src/fio/fio.h` — `struct thread_data` 定义
- `src/fio/thread_options.h` — `struct thread_options` 定义

**阅读要点：**
- `io_u` 是一个 I/O 单元（类比 blktrace 中的 `blk_io_trace`）
- `thread_data` 是一个 job 的所有状态
- `thread_options` 是 job 的配置参数

### Step 2: main() 和初始化流程

**目标：** 理解 fio 启动到开始执行 I/O 的过程

**文件位置：**
- `src/fio/fio.c` — `main()` 入口
- `src/fio/init.c` — `parse_options()`
- `src/fio/options.c` — 选项定义（6000+ 行，选读）

### Step 3: do_io() 主循环 — fio 的心脏

**目标：** 理解 fio 如何循环提交 I/O

**文件位置：** `src/fio/backend.c` — `do_io()` (第 1151 行)

**关键流程：**
1. `get_io_u()` — 获取空闲 io_u 并填充 offset/ddir
2. `td_io_queue()` — 调用引擎的 `queue()` 提交 I/O
3. `td_io_getevents()` — 等待异步 I/O 完成
4. 记录延迟统计
5. 循环直到满足退出条件（时间/大小/IO 数）

### Step 4: io_u 管理 — get_io_u()

**目标：** 理解 I/O 的 offset 是如何计算的（顺序 vs 随机）

**文件位置：** `src/fio/io_u.c` — `get_io_u()` (第 1800+ 行)

**关键问题：**
- 顺序 I/O 的 offset 如何递增？
- 随机 I/O 的 offset 如何生成？
- `--norandommap` 和 `--random_generator` 的影响

### Step 5: I/O 引擎插件系统

**目标：** 理解 fio 的引擎是如何注册和调用的

**文件位置：**
- `src/fio/ioengines.h` — `struct ioengine_ops` 定义
- `src/fio/ioengines.c` — `load_ioengine()`、`register_ioengine()`

**核心接口（引擎必须实现的函数指针）：**

```c
struct ioengine_ops {
    int (*setup)(struct thread_data *);     // 初始化引擎
    int (*init)(struct thread_data *);      // 运行时初始化
    int (*prep)(struct thread_data *, struct io_u *);  // 准备 I/O
    enum fio_q_status (*queue)(struct thread_data *, struct io_u *); // ★ 提交 I/O
    int (*commit)(struct thread_data *);    // 批量提交（异步引擎）
    int (*getevents)(...);                  // 等待完成（异步引擎）
    struct io_u *(*event)(...);             // 获取完成事件
    void (*cleanup)(struct thread_data *);  // 清理
};
```

### Step 6: sync 引擎 — 最简单的引擎

**目标：** 理解 `read()/write()/pread()/pwrite()` 如何被封装

**文件位置：** `src/fio/engines/sync.c` (约 400 行)

### Step 7: libaio 引擎 — Linux 原生异步 I/O

**目标：** 理解 `io_submit()` / `io_getevents()` 的工作方式

**文件位置：** `src/fio/engines/libaio.c` (约 700 行)

**关键差异：** sync 引擎 queue 后直接完成，libaio 引擎 queue 后需 commit + getevents

### Step 8: io_uring 引擎 — 现代异步 I/O

**目标：** 理解 io_uring 的 SQ/CQ 机制

**文件位置：** `src/fio/engines/io_uring.c` (约 2300 行)

### Step 9: 统计输出 — stat.c

**目标：** 理解 fio 输出中 slat/clat/lat/bw/iops 的计算方式

**文件位置：** `src/fio/stat.c` (约 3800 行，选读关键函数)

---

## 3. 核心概念

### 3.1 I/O 引擎对比

| 引擎 | 系统调用 | 同步/异步 | iodepth | 适用场景 |
|------|---------|----------|---------|---------|
| **sync** | read()/write() | 同步 | 1（忽略） | 简单测试 |
| **psync** | pread()/pwrite() | 同步 | 1（忽略） | 无 lseek 开销 |
| **libaio** | io_submit()/io_getevents() | 异步 | 有效 | Linux 高性能测试 |
| **io_uring** | io_uring_submit()/wait_cqe() | 异步 | 有效 | 现代 Linux 最佳性能 |
| **posixaio** | aio_read()/aio_write() | 异步 | 有效 | POSIX 标准 |
| **mmap** | mmap + memcpy | 同步 | 1 | 内存映射 I/O |

### 3.2 同步 vs 异步引擎的 queue 流程

```
同步引擎 (sync):
  queue() 调用 write()
    → 阻塞等待完成
    → 返回 FIO_Q_COMPLETED
    → 无需 getevents()
  iodepth 无效（始终为 1）

异步引擎 (libaio):
  queue() 调用 io_submit()
    → 立即返回 FIO_Q_QUEUED
    → 不等待完成
  commit() 批量提交
  getevents() 等待一批完成
    → 返回完成的 io_event 数组
  iodepth 有效（控制并发 I/O 数）
```

### 3.3 fio 输出指标与 blktrace 指标的对应

| fio 输出 | 含义 | 对应 blktrace |
|---------|------|--------------|
| **slat** (submit latency) | 提交延迟 = issue_time - start_time | Q→D（软件层延迟） |
| **clat** (completion latency) | 完成延迟 = complete_time - issue_time | D→C（设备延迟） |
| **lat** (total latency) | 总延迟 = complete_time - start_time | Q→C（端到端延迟） |
| **bw** | 带宽 | — |
| **iops** | 每秒 I/O 数 | — |

---

## 4. 动手实验清单

### 实验 1: 引擎对比（sync vs libaio vs io_uring）

```bash
# sync 引擎（同步）
fio --name=sync --ioengine=sync --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=10 --time_based

# libaio 引擎（异步，iodepth=32）
fio --name=libaio --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=10 --time_based --iodepth=32

# io_uring 引擎（如果可用）
fio --name=io_uring --ioengine=io_uring --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=10 --time_based --iodepth=32

# 对比 IOPS 和延迟
```

### 实验 2: iodepth 对性能的影响

```bash
for depth in 1 4 8 16 32 64 128; do
    fio --name=d$depth --ioengine=libaio --direct=1 --rw=randwrite \
        --bs=4k --filename=/dev/vdb --runtime=10 --time_based \
        --iodepth=$depth --output=d${depth}.txt
done
# 画 IOPS vs iodepth 曲线
```

### 实验 3: fio + blktrace 联动

```bash
# 终端 1：blktrace 采集
blktrace -d /dev/vdb -o fio_trace -w 15 &
sleep 1

# 终端 2：fio 生成 I/O
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=10 --time_based --iodepth=16
wait

# 解析
blkparse -i fio_trace -o fio_parsed.txt
btt -i fio_trace.bin -o fio_btt

# 对比 fio 报告的 lat 和 btt 报告的 Q2C
```

### 实验 4: 顺序 vs 随机 I/O 对比

```bash
# 顺序读
fio --name=seqread --ioengine=libaio --direct=1 --rw=read --bs=128k \
    --filename=/dev/vdb --runtime=10 --time_based --iodepth=16

# 随机读
fio --name=randread --ioengine=libaio --direct=1 --rw=randread --bs=128k \
    --filename=/dev/vdb --runtime=10 --time_based --iodepth=16

# 混合随机读写
fio --name=randrw --ioengine=libaio --direct=1 --rw=randrw --bs=4k \
    --filename=/dev/vdb --runtime=10 --time_based --iodepth=16 --rwmixread=70
```

### 实验 5: 验证模式（verify）

```bash
# 写入带校验码的数据，然后读回验证
fio --name=verify --ioengine=libaio --direct=1 --rw=write --bs=4k \
    --filename=/tmp/verify_test --size=100M --iodepth=16 \
    --verify=crc32c --do_verify=1
```

---

## 5. 关键源码文件索引

### 核心文件（按阅读顺序）

| Step | 文件 | 行数 | 关注点 |
|------|------|------|--------|
| 1 | `io_u.h` | ~120 | `struct io_u` 定义 |
| 1 | `fio.h` | ~1004 | `struct thread_data` 定义 |
| 1 | `thread_options.h` | ~785 | `struct thread_options` 定义 |
| 2 | `fio.c` | ~62 | `main()` 入口 |
| 2 | `init.c` | ~3287 | `parse_options()` 参数解析 |
| 3 | `backend.c` | ~3019 | `thread_main()` + `do_io()` 主循环 |
| 4 | `io_u.c` | ~2607 | `get_io_u()` offset 计算 |
| 5 | `ioengines.h` | ~159 | `struct ioengine_ops` 引擎接口 |
| 5 | `ioengines.c` | ~745 | `load_ioengine()` 引擎加载 |
| 6 | `engines/sync.c` | ~400 | 最简单的引擎实现 |
| 7 | `engines/libaio.c` | ~700 | Linux AIO 引擎 |
| 8 | `engines/io_uring.c` | ~2337 | io_uring 引擎 |
| 9 | `stat.c` | ~3796 | 延迟/吞吐统计计算 |

### 辅助文件

| 文件 | 行数 | 关注点 |
|------|------|--------|
| `filesetup.c` | ~2285 | 文件打开、大小计算、offset 初始化 |
| `options.c` | ~6469 | 所有选项的定义和解析（按需查阅） |
| `verify.c` | ~2076 | 数据验证逻辑 |
| `iolog.c` | ~1959 | I/O 日志（录制/回放） |
| `blktrace.c` | ~812 | fio 内置的 blktrace 回放功能 |
| `eta.c` | ~743 | 实时进度显示 |
| `diskutil.c` | ~528 | 磁盘利用率采集 |

### 推荐阅读顺序

```
1. fio.c            → 理解入口
2. io_u.h           → 理解 I/O 单元结构
3. ioengines.h      → 理解引擎接口
4. engines/sync.c   → 最简单的引擎，快速理解 queue() 实现
5. backend.c        → do_io() 主循环（只看核心 while 循环）
6. io_u.c           → get_io_u() 的 offset 计算
7. engines/libaio.c → 异步引擎的 queue/commit/getevents 三步曲
8. stat.c           → slat/clat/lat 的计算方式
```

---

## 参考资源

- [fio 官方文档](https://fio.readthedocs.io/)
- [fio GitHub](https://github.com/axboe/fio)
- [fio HOWTO](https://fio.readthedocs.io/en/latest/fio_doc.html)
