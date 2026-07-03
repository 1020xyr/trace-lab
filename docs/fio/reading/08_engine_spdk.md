# SPDK 引擎介绍与使用指南

> SPDK（Storage Performance Development Kit）是 Intel 开源的用户态存储栈。
> 它绕过 Linux 内核，直接从用户空间访问 NVMe 设备，获得极致的 I/O 性能。
> 本文介绍 SPDK 的 fio 插件原理、编译方法、以及与 libaio/io_uring 的性能对比。

---

## 一、SPDK 是什么

### 1.1 传统 I/O 路径 vs SPDK 路径

```
传统内核路径（libaio/io_uring）：

  用户进程
    │  io_submit() / io_uring_submit()
    ▼
  ┌──────────────────────────────────────────┐
  │  Linux 内核                               │
  │  ├── VFS（虚拟文件系统）                   │
  │  ├── Block Layer（I/O 调度器、合并）       │
  │  ├── NVMe Driver（内核驱动）               │
  │  └── 中断处理                              │
  └──────────────────────────────────────────┘
    │
    ▼
  NVMe 设备

SPDK 用户态路径：

  用户进程
    │  spdk_bdev_write() / spdk_bdev_read()
    ▼
  ┌──────────────────────────────────────────┐
  │  SPDK（用户态）                           │
  │  ├── bdev 层（块设备抽象）                │
  │  ├── NVMe Driver（用户态驱动，UIO/VFIO）  │
  │  └── 轮询模式（无中断开销）               │
  └──────────────────────────────────────────┘
    │
    ▼
  NVMe 设备
```

### 1.2 SPDK 的核心优势

| 特性 | 内核路径 | SPDK 路径 |
|------|---------|----------|
| **上下文切换** | 每次系统调用都有用户态↔内核态切换 | 无切换，全程用户态 |
| **中断处理** | 每个 I/O 完成触发中断 | 轮询模式，无中断开销 |
| **I/O 调度器** | 经过 mq-deadline/BFQ 等调度器 | 无调度器，直接提交到硬件队列 |
| **内存拷贝** | 可能需要内核 buffer 拷贝 | 零拷贝，DMA 直接到用户缓冲区 |
| **锁开销** | 内核自旋锁/互斥锁 | 无锁或轻量级锁 |
| **CPU 绑定** | 中断可能在不同 CPU 上处理 | 每个 CPU 核绑定固定的 NVMe 队列 |

### 1.3 性能差距

在现代 NVMe 设备上，SPDK 通常能获得：

| 指标 | libaio (iodepth=128) | SPDK (1 核) | 提升 |
|------|---------------------|-------------|------|
| **IOPS (4K 随机读)** | ~800K | ~1.5M | ~1.9x |
| **延迟 (4K 随机读)** | ~80μs | ~20μs | ~4x |
| **CPU 效率** | 需要多核中断处理 | 单核即可打满 | — |

---

## 二、SPDK fio 插件的工作原理

### 2.1 架构

```
┌──────────────────────────────────────────────────────────────┐
│  fio                                                         │
│  ├── do_io() 主循环                                          │
│  │     ├── get_io_u()                                        │
│  │     ├── td_io_queue() → spdk_fio_queue()                  │
│  │     │     └── spdk_bdev_read/write(bdev, io_channel, ...) │
│  │     │           └── NVMe 命令直接提交到硬件队列            │
│  │     └── td_io_getevents() → spdk_fio_getevents()          │
│  │           └── spdk_bdev_io_complete() 回调收割            │
│  │                                                           │
│  └── 引擎: spdk_fio_plugin (bdev fio plugin)                 │
└──────────────────────────────────────────────────────────────┘
```

### 2.2 关键机制

**bdev 层**：SPDK 的块设备抽象层，提供统一的 read/write/unmap 接口。
底层可以是 NVMe、AIO、malloc（内存盘）等不同后端。

**io_channel**：每个线程一个 io_channel，绑定到固定的 CPU 核。
避免了多线程竞争同一个 NVMe 队列的问题。

**轮询模式**：SPDK 不使用中断，而是持续轮询 NVMe 的完成队列（CQ）。
这消除了中断处理的开销（~5-10μs/次），但需要独占 CPU 核。

**UIO/VFIO**：SPDK 通过 UIO 或 VFIO 将 NVMe 设备的 MMIO 空间映射到用户态，
用户态程序直接读写 NVMe 寄存器和提交队列。

### 2.3 SPDK fio 插件的函数映射

| ioengine_ops 函数指针 | SPDK 实现 | 功能 |
|----------------------|----------|------|
| `setup` | `spdk_fio_setup()` | 初始化 SPDK 环境、打开 bdev |
| `init` | `spdk_fio_init()` | 创建 io_channel |
| `queue` | `spdk_fio_queue()` | 调用 spdk_bdev_read/write |
| `getevents` | `spdk_fio_getevents()` | 收割完成的 I/O |
| `event` | `spdk_fio_event()` | 返回完成的 io_u |
| `cleanup` | `spdk_fio_cleanup()` | 销毁 io_channel、关闭 bdev |
| `open_file` | `spdk_fio_open()` | 打开 bdev 设备 |
| `close_file` | `spdk_fio_close()` | 关闭 bdev 设备 |

---

## 三、编译和使用

### 3.1 编译 SPDK

```bash
# 1. 安装依赖
yum install -y gcc gcc-c++ make libuuid-devel libaio-devel \
    ncurses-devel libiscsi-devel python3

# 2. 克隆 SPDK（含 dpdk 子模块）
git clone https://github.com/spdk/spdk.git
cd spdk
git submodule update --init

# 3. 配置（启用 fio 插件）
./configure --with-fio=/usr/include \
            --with-crypto \
            --disable-tests

# 4. 编译
make -j$(nproc)

# 5. 安装
make install
```

### 3.2 绑定 NVMe 设备到 SPDK

```bash
# 卸载内核 NVMe 驱动（危险操作！确保不是系统盘）
HUGEMEM=4096 scripts/setup.sh

# 验证设备绑定
scripts/setup.sh status

# 查看 SPDK 可见的设备
build/examples/spdk_bdevperf -h
```

### 3.3 使用 SPDK fio 插件

```bash
# 设置环境变量
export LD_PRELOAD=/usr/local/lib/libspdk_bdev_fio_plugin.so

# 运行 fio（使用 SPDK 引擎）
fio --name=spdk_test \
    --ioengine=spdk_bdev \
    --filename=NVMe0n1 \
    --rw=randread \
    --bs=4k \
    --iodepth=64 \
    --direct=1 \
    --runtime=60 \
    --time_based \
    --thread \
    --numjobs=1 \
    --cpus_allowed=0
```

**关键参数说明：**

| 参数 | 含义 |
|------|------|
| `--ioengine=spdk_bdev` | 使用 SPDK bdev 插件 |
| `--filename=NVMe0n1` | SPDK bdev 设备名（不是 /dev/nvme0n1） |
| `--thread` | 必须使用线程模式（SPDK 依赖线程局部存储） |
| `--numjobs=1` | 单 job 单核（SPDK 一个核就够了） |
| `--cpus_allowed=0` | 绑定到 CPU 0（SPDK 轮询模式需要独占核心） |

---

## 四、SPDK vs libaio vs io_uring 对比

### 4.1 架构对比

```
              libaio                    io_uring                    SPDK
  ┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────────────┐
  │ io_submit()          │  │ io_uring_submit()    │  │ spdk_bdev_read/      │
  │   ↓                  │  │   ↓                  │  │   write()            │
  │ 内核 AIO 层          │  │ io_uring 层          │  │   ↓                  │
  │   ↓                  │  │   ↓                  │  │ 用户态 NVMe 驱动     │
  │ Block Layer          │  │ Block Layer          │  │   ↓                  │
  │   ↓                  │  │   ↓                  │  │ 直接访问硬件队列     │
  │ NVMe 内核驱动        │  │ NVMe 内核驱动        │  │                      │
  │   ↓                  │  │   ↓                  │  │                      │
  │ 中断处理              │  │ 中断/轮询            │  │ 轮询（无中断）        │
  └──────────────────────┘  └──────────────────────┘  └──────────────────────┘
  系统调用 + 中断开销       较少系统调用开销             零系统调用开销
  iodepth 有效              SQ/CQ 共享内存              独占 CPU 核心
```

### 4.2 性能对比（典型 NVMe SSD，4K 随机读）

| 引擎 | IOPS | 平均延迟 | P99 延迟 | CPU 使用 |
|------|------|---------|---------|---------|
| **sync** | ~15K | ~65μs | ~100μs | 1 核（100%） |
| **libaio** (depth=128) | ~800K | ~150μs | ~300μs | 多核 |
| **io_uring** (depth=128) | ~1.0M | ~120μs | ~250μs | 多核 |
| **SPDK** (1 核, depth=64) | ~1.5M | ~40μs | ~60μs | 1 核（100%） |

**注意：** 以上数据为参考值，实际取决于 NVMe 设备型号、CPU 频率、内存带宽等。

### 4.3 选择建议

| 场景 | 推荐引擎 | 原因 |
|------|---------|------|
| 简单功能测试 | sync | 最简单，无额外依赖 |
| 通用性能测试 | libaio | 成熟稳定，广泛使用 |
| 最新 Linux 内核 | io_uring | 最佳内核路径性能 |
| 极限性能评估 | SPDK | 消除内核开销，测试硬件极限 |
| 延迟敏感测试 | SPDK | 最低延迟，最小抖动 |

---

## 五、示例 fio 配置文件

### 5.1 SPDK 随机读性能测试

```ini
[global]
ioengine=spdk_bdev
thread=1
direct=1
bs=4k
iodepth=64
runtime=120
time_based=1
numjobs=1
cpus_allowed=0

[randread-4k]
filename=NVMe0n1
rw=randread
```

### 5.2 SPDK vs libaio 对比测试

```ini
[global]
thread=1
direct=1
bs=4k
runtime=60
time_based=1
filename=NVMe0n1
filename_libaio=/dev/nvme0n1

[spdk-randread]
ioengine=spdk_bdev
iodepth=64
numjobs=1
cpus_allowed=0
rw=randread

[libaio-randread]
ioengine=libaio
iodepth=64
numjobs=4
rw=randread
stonewall
```

### 5.3 SPDK 混合随机读写

```ini
[global]
ioengine=spdk_bdev
thread=1
direct=1
bs=4k
iodepth=32
runtime=60
time_based=1
numjobs=1
cpus_allowed=0

[randrw-70-30]
filename=NVMe0n1
rw=randrw
rwmixread=70
```

---

## 六、注意事项

1. **SPDK 需要独占 NVMe 设备**：绑定到 SPDK 后，设备从 /dev/ 中消失，其他程序无法访问
2. **SPDK 需要大页内存**：通过 `HUGEMEM` 环境变量分配，通常需要 4GB+
3. **SPDK 使用轮询模式**：每个 SPDK 线程会 100% 占用一个 CPU 核心
4. **SPDK 与 blktrace 不兼容**：由于绕过内核，blktrace 看不到 SPDK 的 I/O
5. **恢复设备**：测试完成后运行 `scripts/setup.sh reset` 将设备还给内核
