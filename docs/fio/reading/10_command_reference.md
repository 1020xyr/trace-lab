# fio 完整命令参考

> 每个常用参数的含义、默认值、示例，以及与源码实现的对应关系

---

## 一、基本用法

### 命令行格式

```bash
# 直接指定参数
fio --name=<job_name> --rw=<mode> --bs=<size> --filename=<path> [选项...]

# 使用 job 文件
fio <job_file.fio>

# 混合模式：全局参数 + job 文件
fio --global-section --name=job1 job1.fio
```

### job 文件格式

```ini
# 全局段（所有 job 共享）
[global]
ioengine=libaio
direct=1
runtime=60
time_based

# job 1：随机写
[randwrite]
rw=randwrite
bs=4k
filename=/dev/sda
iodepth=32

# job 2：顺序读
[seqread]
rw=read
bs=1M
filename=/dev/sda
iodepth=16
```

### 命令行 vs job 文件等价关系

```bash
# 命令行写法
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/sda --runtime=60 --time_based --iodepth=32

# 等价的 job 文件写法
[test]
ioengine=libaio
direct=1
rw=randwrite
bs=4k
filename=/dev/sda
runtime=60
time_based
iodepth=32
```

---

## 二、核心参数速查表

### I/O 模式（--rw）

| 参数值 | 含义 | 典型场景 |
|--------|------|---------|
| `read` | 顺序读 | 测试顺序读带宽 |
| `write` | 顺序写 | 测试顺序写带宽 |
| `randread` | 随机读 | 数据库随机读性能 |
| `randwrite` | 随机写 | 数据库随机写性能 |
| `randrw` | 混合随机读写 | 模拟真实数据库负载 |
| `readwrite` | 顺序读写交替 | 顺序混合 I/O |
| `rw` | 同 `readwrite` | 同上 |
| `randtrim` | 随机 trim（SSD） | SSD trim 性能 |
| `trimwrite` | trim + write | SSD 先擦后写 |

### 块大小（--bs）

```bash
--bs=4k            # 固定 4KB（最常用）
--bs=128k          # 固定 128KB（测试带宽）
--bs=1M            # 固定 1MB（顺序读写带宽）
--bsrange=4k-64k   # 随机块大小范围（4KB 到 64KB）
--bssplit=4k/50:64k/30:1M/20   # 按比例分配：50% 4K + 30% 64K + 20% 1M
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--bs` | I/O 块大小 | 4k |
| `--bsrange` | 块大小范围（min-max） | — |
| `--bssplit` | 按比例指定不同块大小 | — |
| `--bs_unaligned` | 允许非对齐块大小 | 0（关闭） |

### I/O 引擎（--ioengine）

```bash
--ioengine=sync        # 同步 read()/write()
--ioengine=psync       # 同步 pread()/pwrite()
--ioengine=libaio      # Linux 原生异步 I/O（★ 最常用）
--ioengine=io_uring    # io_uring 异步引擎（Linux 5.1+）
--ioengine=posixaio    # POSIX AIO
--ioengine=mmap        # 内存映射 I/O
--ioengine=spdk_bdev   # SPDK 用户态驱动
--ioengine=null        # 空引擎（只计算不发 I/O）
```

| 引擎 | 系统调用 | 异步 | 典型用途 | 源码位置 |
|------|---------|------|---------|---------|
| `sync` | read/write | 否 | 基础测试 | `ioengines/sync.c` |
| `psync` | pread/pwrite | 否 | 基础测试 | `ioengines/sync.c` |
| `libaio` | io_setup/io_submit/io_getevents | 是 | 高性能异步 | `ioengines/libaio.c` |
| `io_uring` | io_uring_enter/io_uring_wait_cqe | 是 | 最新异步引擎 | `ioengines/io_uring.c` |
| `posixaio` | aio_read/aio_write | 是 | POSIX 标准 | `ioengines/posixaio.c` |
| `mmap` | mmap + memcpy | 否 | 模拟内存访问 | `ioengines/mmap.c` |
| `spdk_bdev` | SPDK NVMe API | 是 | 绕过内核 | 外部插件 |
| `null` | 无 | — | 基准测试 | `ioengines/null.c` |

### 队列深度（--iodepth）

```bash
--iodepth=1            # 单队列（测量延迟）
--iodepth=32           # 32 并发（常见高并发）
--iodepth=128          # 128 并发（压力测试）
--iodepth_low=4        # 收割到低水位 4 时停止 reap
--iodepth_batch=8      # 每次提交 8 个
--iodepth_batch_complete_min=1  # 至少等 1 个完成
--iodepth_batch_complete_max=8  # 最多等 8 个完成
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--iodepth` | 最大在飞 I/O 数 | 1 |
| `--iodepth_low` | 低水位线（低于此值时停止 reap） | 1 |
| `--iodepth_batch` | 批量提交数量 | 1 |
| `--iodepth_batch_complete_min` | 最少等待完成数 | 1 |
| `--iodepth_batch_complete_max` | 最多等待完成数 | 同 iodepth_batch |

### 文件大小与运行控制

```bash
--size=1G              # 文件/设备大小为 1GB
--size=100%            # 使用整个设备
--runtime=60           # 运行 60 秒
--time_based           # ★ 即使数据写完也继续运行直到 runtime 到期
--loops=3              # 重复执行 3 次
--number_ios=1000      # 只执行 1000 次 I/O
--io_size=2G           # I/O 操作总量 2GB
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--size` | 文件大小 | 设备或文件大小 |
| `--runtime` | 运行时长（秒） | 无限制 |
| `--time_based` | 基于时间运行 | 0（关闭） |
| `--io_size` | I/O 总量 | 同 size |
| `--number_ios` | I/O 次数上限 | 无限制 |
| `--loops` | 循环次数 | 1 |
| `--ramp_time=10` | 热身期 10 秒（不计入统计） | 0 |

### Direct I/O（--direct）

```bash
--direct=1             # O_DIRECT，绕过 page cache（★ 性能测试标配）
--direct=0             # 使用 buffered I/O（经过 page cache）
--buffered=1           # 同 --direct=0
--sync=1               # O_SYNC，每次写后同步到磁盘
--fsync=1              # 每写 1 个 IO 后调用 fsync()
--fdatasync=32         # 每写 32 个 IO 后调用 fdatasync()
--end_fsync=1          # 测试结束后执行 fsync
```

| 参数 | 含义 | 默认值 | 效果 |
|------|------|--------|------|
| `--direct` | O_DIRECT 模式 | 0 | 绕过 page cache |
| `--sync` | O_SYNC 模式 | 0 | 每次 I/O 强制持久化 |
| `--fsync` | 每 N 次写后 fsync | 0 | 定期刷盘 |
| `--fdatasync` | 每 N 次写后 fdatasync | 0 | 定期刷数据 |
| `--end_fsync` | 结束时 fsync | 0 | 确保最终持久化 |

### 作业数与进程控制

```bash
--numjobs=4            # 启动 4 个并发 job 进程
--group_reporting      # ★ 将多个 job 的统计合并输出
--thread               # 使用线程（默认用进程）
--cpus_allowed=0-3     # 绑定到 CPU 0-3
--cpus_allowed_policy=split    # 每个 job 绑定不同 CPU
--numa_cpus=0,1        # NUMA 节点上的 CPU
--numa_mem_policy=bind:0   # 绑定到 NUMA 节点 0 的内存
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--numjobs` | 并发 job 数 | 1 |
| `--group_reporting` | 合并报告 | 0 |
| `--thread` | 用线程代替进程 | 0 |
| `--cpus_allowed` | CPU 亲和性 | 不绑定 |
| `--numa_cpus` | NUMA CPU 绑定 | 不绑定 |

### 同步选项

```bash
--sync=1               # O_SYNC：每次写操作后同步
--fsync=1              # 每 1 次写后调用 fsync()
--fsync=32             # 每 32 次写后调用 fsync()
--fdatasync=16         # 每 16 次写后调用 fdatasync()
--sync_io              # 同步 I/O 提交模式
```

### 随机分布（--random_distribution）

```bash
--random_distribution=random      # 均匀随机（★ 默认）
--random_distribution=zipf:0.8    # Zipf 分布（θ=0.8，热点集中）
--random_distribution=pareto:0.8  # Pareto 分布（80/20 法则）
--random_distribution=random:20   # 20% 的区域集中访问
--random_generator=lfsr           # 使用 LFSR 伪随机（更快）
--random_generator=tausworthe     # 使用 Tausworthe（更随机）
```

| 参数 | 含义 | 默认值 | 适用场景 |
|------|------|--------|---------|
| `random` | 均匀随机 | ★ | 通用压力测试 |
| `zipf:θ` | Zipf 分布 | — | 数据库热点模拟 |
| `pareto:p` | Pareto 分布 | — | 冷热数据模型 |
| `zoned:z1/z2` | 分区随机 | — | 冷热分区 |

### 混合读写（--rwmixread / --rwmixwrite）

```bash
--rw=randrw            # 必须先设置混合模式
--rwmixread=70         # 70% 读 + 30% 写
--rwmixwrite=30        # 同上（指定写比例）
--rwmixread=50         # 50/50 混合
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--rwmixread` | 读的比例（%） | 50 |
| `--rwmixwrite` | 写的比例（%） | 50 |
| `--rwmixread` + `--rwmixwrite` | 两者之和应为 100 | — |

### 输出格式（--output-format）

```bash
--output-format=normal    # ★ 默认人类可读格式
--output-format=json      # JSON 格式（方便程序解析）
--output-format=json+     # JSON + normal 合并输出
--output-format=terse     # 紧凑格式（一行，方便 awk 处理）
--output-format=terse_v4  # terse v4 版本
```

| 格式 | 适用场景 | 示例 |
|------|---------|------|
| `normal` | 人类阅读 | 默认输出 |
| `json` | 自动化分析、Web 展示 | `jq '.jobs[0].write.iops'` |
| `json+` | 同时需要两种 | — |
| `terse` | 批量测试、awk 处理 | 单行输出 |

### 数据验证（--verify）

```bash
--verify=crc32c        # CRC32C 校验（★ 最常用，快且可靠）
--verify=md5           # MD5 校验（更安全但更慢）
--verify=sha1          # SHA1 校验
--verify=sha256        # SHA256 校验
--verify=pattern       # 固定 pattern 校验
--verify=null          # 不校验（只检查是否完成）
--do_verify=1          # ★ 写完后执行验证读
--verify_fatal=1       # 校验失败时立即停止
--verify_dump=1        # 校验失败时 dump 数据
--verify_backlog=128   # 每写 128 块后执行验证
--verify_async=2       # 2 个异步验证线程
--verify_pattern=0xA5  # 自定义填充 pattern
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--verify` | 验证算法 | none |
| `--do_verify` | 写后执行验证 | 0 |
| `--verify_fatal` | 失败即停止 | 0 |
| `--verify_backlog` | 每 N 块验证 | 0（全部写完后验证） |
| `--verify_async` | 异步验证线程数 | 0 |

### 日志输出（--write_*_log）

```bash
--write_bw_log=bw_log           # 带宽日志
--write_lat_log=lat_log         # ★ 延迟日志（生成 slat/clat/lat 三个文件）
--write_iops_log=iops_log       # IOPS 日志
--write_hist_log=hist_log       # 直方图日志
--log_avg_msec=1000             # 日志采样间隔（毫秒，默认 500ms）
--log_offset=1                  # 日志中包含 I/O offset
--log_compression=100m          # 启用压缩（限制内存 100MB）
```

| 参数 | 生成的文件 | 格式 |
|------|-----------|------|
| `--write_bw_log=X` | `X_bw.<jobid>.log` | `时间(ms), 带宽(KB/s), ddir, bs` |
| `--write_lat_log=X` | `X_slat.<jobid>.log`<br>`X_clat.<jobid>.log`<br>`X_lat.<jobid>.log` | `时间(ms), 延迟(ns), ddir, bs` |
| `--write_iops_log=X` | `X_iops.<jobid>.log` | `时间(ms), IOPS, ddir, bs` |

### 速率控制

```bash
--rate=10m             # 限制带宽为 10MB/s
--rate_iops=1000       # 限制 IOPS 为 1000
--ratemin=5m           # 最低带宽 5MB/s（达不到则报错）
--rate_iops_min=500    # 最低 IOPS 500
--rate_process=poisson # 使用泊松分布模拟真实 I/O 到达
--rate_cycle=1000      # 速率检查周期（毫秒）
```

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--rate` | 带宽上限 | 无限制 |
| `--rate_iops` | IOPS 上限 | 无限制 |
| `--ratemin` | 带宽下限 | 0 |
| `--rate_iops_min` | IOPS 下限 | 0 |
| `--rate_process` | 速率模型（linear/poisson） | linear |

### CPU/NUMA 绑定

```bash
--cpus_allowed=0-3           # 绑定到 CPU 0-3
--cpus_allowed=0,2,4         # 绑定到 CPU 0、2、4
--cpus_allowed_policy=split  # 每个 job 绑定不同 CPU
--cpus_allowed_policy=perjobchk  # 每 job 绑定一个核心
--numa_cpus=0,1              # 绑定 NUMA CPU
--numa_mem_policy=bind:0     # 绑定 NUMA 内存节点 0
--numa_mem_policy=interleave:all  # 交错分配内存
```

### 文件名与 I/O 偏移

```bash
--filename=/dev/sda           # 指定设备
--filename=testfile.dat       # 指定文件
--filename_format=job.\$jobnum.\$filenum  # 自定义文件名
--openfiles=4                 # 同时打开 4 个文件
--file_service_type=sequential # 文件遍历方式
--offset=10%                  # 从 10% 偏移开始
--offset_increment=1G         # 多文件间偏移增量
--zonemode=zbd                # ZBD（分区设备）模式
```

---

## 三、job 文件格式详解

### 语法规则

```ini
# 注释用 # 或 ;
# 段名即 job 名，写在 [] 中
# 参数格式：key=value 或 key = value
# 空行被忽略
# [global] 段中的参数被所有 job 继承
```

### 完整 job 文件示例

```ini
# === 全局参数 ===
[global]
ioengine=libaio
direct=1
runtime=60
time_based
group_reporting
write_lat_log=/tmp/fio_lat
write_bw_log=/tmp/fio_bw

# === Job 1：随机写基准 ===
[randwrite_base]
rw=randwrite
bs=4k
filename=/dev/sda
iodepth=32
numjobs=1

# === Job 2：高并发随机写 ===
[randwrite_heavy]
rw=randwrite
bs=4k
filename=/dev/sda
iodepth=128
numjobs=4
cpus_allowed=0-3

# === Job 3：顺序读带宽 ===
[seqread_bw]
rw=read
bs=1M
filename=/dev/sda
iodepth=16
numjobs=1
```

### 多 job 混合模式

```ini
[global]
ioengine=libaio
direct=1
runtime=120
time_based

# 同时运行读和写（两个 job 并行）
[writer]
rw=randwrite
bs=4k
filename=/dev/sda
iodepth=16

[reader]
rw=randread
bs=4k
filename=/dev/sda
iodepth=16
stonewall           # ★ reader 在 writer 结束后才开始
```

### stonewall 与 thinktime

```ini
# stonewall：前一个 job 结束后才开始
[job1]
rw=randwrite
stonewall

# thinktime：模拟应用层思考时间
[job2]
rw=randwrite
thinktime=1000       # 每次 I/O 后等待 1ms
thinktime_blocks=4   # 每 4 次 I/O 等待一次
thinktime_spin=500   # 前 500μs 用 CPU 空转，剩余 sleep
```

---

## 四、常用命令组合（10 个典型场景）

### 场景 1：基础随机写性能测试

```bash
fio --name=randwrite --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/sda --runtime=60 --time_based \
    --iodepth=32 --numjobs=1
```

**关键参数说明：**
- `--direct=1`：绕过 page cache，直接测试设备性能
- `--iodepth=32`：32 个在飞 I/O，利用异步引擎并发
- `--time_based`：即使写完整个设备也继续到 runtime

### 场景 2：顺序读带宽测试

```bash
fio --name=seqread --ioengine=libaio --direct=1 --rw=read \
    --bs=1M --filename=/dev/sda --runtime=60 --time_based \
    --iodepth=16 --numjobs=1
```

**关键参数说明：**
- `--bs=1M`：大块顺序读，充分利用带宽
- `--iodepth=16`：适中的并发度，平衡延迟与吞吐

### 场景 3：混合随机读写（70/30）

```bash
fio --name=randrw --ioengine=libaio --direct=1 --rw=randrw \
    --bs=4k --filename=/dev/sda --runtime=60 --time_based \
    --iodepth=16 --rwmixread=70
```

**关键参数说明：**
- `--rwmixread=70`：模拟 70% 读 + 30% 写的真实数据库负载
- 输出会包含 read 和 write 两个 section

### 场景 4：iodepth 梯度测试

```bash
for depth in 1 4 8 16 32 64 128; do
    fio --name=depth$depth --ioengine=libaio --direct=1 \
        --rw=randwrite --bs=4k --filename=/dev/sda \
        --runtime=30 --time_based --iodepth=$depth \
        --output-format=json > depth_${depth}.json
done
```

**用途：** 找出 IOPS 饱和拐点，确定最佳 iodepth。

### 场景 5：延迟测试（iodepth=1, numjobs=1）

```bash
fio --name=lat_test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/sda --runtime=60 --time_based \
    --iodepth=1 --numjobs=1 --write_lat_log=lat_log
```

**关键参数说明：**
- `--iodepth=1 --numjobs=1`：消除队列效应，测量真实单次延迟
- `--write_lat_log`：生成延迟日志，用于分析延迟分布

### 场景 6：fio + blktrace 联动分析

```bash
# 同时运行 blktrace 和 fio
blktrace -d /dev/sda -o trace -w 60 &
sleep 1
fio --name=bt_test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/sda --runtime=58 --time_based \
    --iodepth=32
wait

# 解析
blkparse -i trace -O -d trace.bin
btt -i trace.bin -o summary

# 对比 fio 报告的延迟和 btt 报告的延迟
```

**用途：** fio 报告的是端到端延迟（slat + clat），btt 可以拆分出 Q2D（软件延迟）和 D2C（设备延迟）。

### 场景 7：数据验证模式

```bash
fio --name=verify --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/tmp/verify_test --size=1G \
    --verify=crc32c --do_verify=1 --verify_fatal=1
```

**关键参数说明：**
- `--verify=crc32c`：写入时填充校验和，读取时验证
- `--do_verify=1`：写完后自动执行验证读
- 用于检测存储链路中的数据损坏

### 场景 8：I/O 日志录制与回放

```bash
# 录制：运行 fio 并保存 I/O 日志
fio --name=record --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/sda --runtime=60 --time_based \
    --iodepth=32 --write_iops_log=iops_log --write_lat_log=lat_log

# 回放：用 blktrace 文件作为 I/O 模式
fio --name=replay --read_iolog=/tmp/trace.bin \
    --replay_redirect=/dev/sda
```

**用途：** 录制真实工作负载的 I/O 模式，然后精确回放进行复现或回归测试。

### 场景 9：带宽日志 + gnuplot 画图

```bash
# 运行 fio 并生成带宽日志
fio --name=bw_test --ioengine=libaio --direct=1 --rw=randwrite \
    --bs=4k --filename=/dev/sda --runtime=60 --time_based \
    --iodepth=32 --write_bw_log=bw_log --log_avg_msec=1000

# gnuplot 画图
gnuplot -e '
  set terminal png size 1200,400
  set output "bw.png"
  set xlabel "Time (sec)"
  set ylabel "BW (KB/s)"
  plot "bw_log_bw.1.log" using ($1/1000):2 with lines title "Write BW"
'
```

**日志文件格式：**
```
# 时间(ms), 带宽(KB/s), ddir, bs
1001, 70640, 1, 4096
2002, 68976, 1, 4096
3003, 71232, 1, 4096
```

### 场景 10：SPDK fio 插件用法

```bash
# 使用 SPDK 用户态 NVMe 驱动（绕过内核）
fio --name=spdk_test --ioengine=spdk_bdev \
    --filename=trtype=PCIe traddr=0000:04:00.0 ns=1 \
    --rw=randwrite --bs=4k --runtime=60 --time_based \
    --iodepth=128 --thread --cpus_allowed=0-3 \
    --numjobs=4 --group_reporting
```

**关键参数说明：**
- `--ioengine=spdk_bdev`：使用 SPDK 的 fio 插件
- `--thread`：SPDK 要求使用线程模式
- `--filename` 格式不同：使用 PCIe 地址而非 /dev/ 路径
- 性能通常比内核 NVMe 驱动高 2-3 倍

---

## 五、fio 输出格式字段说明

### normal 输出的完整字段

```
randwrite: (groupid=0, jobs=1): err= 0: pid=12345: <时间戳>
  write: IOPS=17.6k, BW=68.9MiB/s (72.3MB/s)(690MiB/10008msec); 0 zone resets
```

| 字段 | 含义 | 示例值 |
|------|------|--------|
| `groupid` | 组 ID | 0 |
| `jobs` | 组内 job 数 | 1 |
| `err` | 错误码（0=成功） | 0 |
| `pid` | 进程 ID | 12345 |
| `IOPS` | 每秒 I/O 操作数 | 17.6k |
| `BW` | 带宽（二进制/十进制） | 68.9MiB/s (72.3MB/s) |
| `io` | 总 I/O 量 | 690MiB |
| `run` | 运行时间 | 10008msec |

### 延迟字段详解

```
    slat (nsec): min=1546, max=2748.2k, avg=10950.07, stdev=18399.31
    clat (usec): min=128, max=10852, avg=1801.11, stdev=2899.95
     lat (usec): min=165, max=10859, avg=1812.06, stdev=2899.65
```

| 字段 | 全称 | 含义 | 对应 blktrace |
|------|------|------|-------------|
| `slat` | Submission latency | 提交延迟：fio 调用 engine→内核接受 | Q→D（大致） |
| `clat` | Completion latency | 完成延迟：内核接受→I/O 完成 | D→C（大致） |
| `lat` | Total latency | 总延迟：slat + clat | Q→C（大致） |

```
fio 的延迟模型：

  ┌──────────┐         ┌──────────┐         ┌──────────┐
  │  fio 用户态 │  slat   │  内核调度  │  clat   │  设备处理  │
  │  提交 I/O  │────────→│  + 驱动   │────────→│  完成 I/O │
  └──────────┘         └──────────┘         └──────────┘
       │                     │                     │
       │←── slat ──→│←──────── clat ────────→│
       │←──────────── lat ────────────────────→│

blktrace 的延迟模型：

  Q ───→ G ───→ I ───→ D ───→ C
  │←── Q2G ──→│← G2I →│← I2D →│←── D2C ──→│
  │←───────── Q2D ────────────→│
  │←────────────── Q2C ────────────────────→│

对应关系（大致）：
  fio slat ≈ blktrace Q2D  （软件层延迟）
  fio clat ≈ blktrace D2C  （设备层延迟）
  fio lat  ≈ blktrace Q2C  （端到端延迟）
```

### 百分位延迟

```
    clat percentiles (usec):
     |  1.00th=[  302],  5.00th=[  343], 10.00th=[  367], 20.00th=[  396],
     | 30.00th=[  416], 40.00th=[  437], 50.00th=[  457], 60.00th=[  486],
     | 70.00th=[  537], 80.00th=[  783], 90.00th=[ 8160], 95.00th=[ 8291],
     | 99.00th=[ 8586], 99.50th=[ 8848], 99.90th=[ 9503], 99.95th=[ 9765],
     | 99.99th=[10421]
```

**关键百分位含义：**
- **50th（P50/中位数）**：一半的 I/O 延迟低于此值
- **99th（P99）**：99% 的 I/O 延迟低于此值 → SLA 常用指标
- **99.9th（P99.9）**：长尾延迟指标
- **99.99th（P99.99）**：极端长尾

### 延迟分布

```
  lat (usec)   : 250=0.12%, 500=63.66%, 750=15.81%, 1000=1.43%
  lat (msec)   : 2=1.13%, 4=0.41%, 10=17.41%, 20=0.03%
```

**解读：** 延迟在 (上一个桶上限, 当前桶上限] 范围内的 IO 百分比。
例如 `500=63.66%` 表示 63.66% 的 I/O 延迟在 250-500μs 之间。

### CPU 与队列深度统计

```
  cpu          : usr=3.75%, sys=7.45%, ctx=180267, majf=0, minf=11
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=100.0%, >=64=0.0%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.1%, 64=0.0%, >=64=0.0%
     issued rwts: total=0,176629,0,0 short=0,0,0,0 dropped=0,0,0,0
```

| 字段 | 含义 |
|------|------|
| `usr` | 用户态 CPU 占用 |
| `sys` | 内核态 CPU 占用 |
| `ctx` | 上下文切换次数 |
| `majf/minf` | 主/次缺页次数 |
| `IO depths` | 在飞 I/O 数的分布 |
| `submit` | 每次提交的批量大小分布 |
| `complete` | 每次完成的批量大小分布 |
| `issued rwts` | read/write/trim/sync 的 I/O 总数 |

---

## 六、fio 与 blktrace 的关系

### fio 回放 blktrace 文件

```bash
# fio 可以直接读取 blktrace 的二进制文件，回放其中的 I/O 模式
fio --name=replay --read_iolog=trace.bin \
    --replay_redirect=/dev/sda \
    --ioengine=sync

# 或者在 job 文件中指定
[replay]
read_iolog=trace.bin
replay_redirect=/dev/sda
ioengine=sync
```

**关键参数：**
| 参数 | 含义 | 源码位置 |
|------|------|---------|
| `--read_iolog` | 读取 blktrace 二进制文件 | `blktrace.c: init_blktrace_read()` |
| `--replay_redirect` | 重定向到不同设备 | `blktrace.c: blktrace_lookup_device()` |
| `--merge_blktrace_iologs` | 合并多个 blktrace 文件 | `blktrace.c: merge_blktrace_iologs()` |
| `--merge_blktrace_dev` | 指定合并设备映射 | 同上 |
| `--replay_scale` | 缩放回放速度 | 同上 |
| `--replay_align` | 对齐回放块大小 | 同上 |

### fio 内建的 blktrace.c

fio 源码中包含 `blktrace.c`（约 750 行），实现了 blktrace 文件的解析和回放：

```
src/fio/blktrace.c 主要函数：

  is_blktrace()           → 检测文件是否为 blktrace 格式（检查 magic）
  init_blktrace_read()    → 初始化 blktrace 读取
  read_blktrace()         → 读取所有 blk_io_trace 条目，转换为 io_log
  blktrace_lookup_device() → 根据 major:minor 查找设备路径
  merge_blktrace_iologs() → 合并多个 blktrace 文件的 I/O 日志
  merge_finish_file()     → 完成单个文件的合并
  find_earliest_io()      → 找最早到达的 I/O（用于多文件合并排序）
```

### fio 延迟 vs blktrace/btt 延迟对比

| fio 指标 | blktrace/btt 指标 | 含义 |
|---------|------------------|------|
| `slat`（提交延迟） | `Q2D`（大致） | 从 fio 提交到内核接受 |
| `clat`（完成延迟） | `D2C`（大致） | 从内核接受到设备完成 |
| `lat`（总延迟） | `Q2C`（大致） | 端到端延迟 |

**注意：** 这是大致对应关系。fio 的 slat 测量的是 engine->queue() 返回的时间，
而 blktrace 的 Q2D 包含 Q→G→I→D 全部阶段。差异取决于引擎实现。

---

## 七、源码文件快速索引

```bash
# 找 fio 主循环 do_io()
grep -n "static void do_io" src/fio/backend.c

# 找 io_u 结构体
grep -n "struct io_u {" src/fio/io_u.h

# 找 I/O 引擎接口
grep -n "struct ioengine_ops" src/fio/ioengines.h

# 找 libaio 引擎实现
cat src/fio/ioengines/libaio.c

# 找 io_uring 引擎实现
cat src/fio/ioengines/io_uring.c

# 找 blktrace 回放
cat src/fio/blktrace.c

# 找统计输出
grep -n "show_thread_status\|__show_run_stats" src/fio/stat.c

# 找速率控制
grep -n "usec_for_io\|check_min_rate" src/fio/backend.c

# 找 thinktime 处理
grep -n "handle_thinktime" src/fio/backend.c
```
