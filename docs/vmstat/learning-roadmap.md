# vmstat 学习路线

> 虚拟内存统计 — 5 秒内判断系统瓶颈方向的宏观工具

---

## 定位

```
性能分析工具层次：

第一层（宏观概览）：★ vmstat ← 你在这里
  → 5 秒内判断：CPU? 内存? I/O? 网络?

第二层（维度深入）：
  CPU → sar -u / pidstat -u / perf
  内存 → sar -r / /proc/meminfo
  I/O → iostat / blktrace
  网络 → sar -n DEV / ss

第三层（内核机制）：
  perf lock / bpftrace / 内核源码
```

## vmstat 的数据来源

```
vmstat 从三个 /proc 文件读取数据：

/proc/stat         → CPU 时间（us/sy/id/wa/st）、中断数(in)、上下文切换(cs)
/proc/meminfo      → 内存使用（free/buff/cache/swpd）
/proc/vmstat       → 虚拟内存事件（pgpgin/pgpgout/pgscan/pgsteal）

  ┌──────────┐    ┌────────────┐    ┌───────────┐
  │/proc/stat│    │/proc/meminfo│    │/proc/vmstat│
  └────┬─────┘    └──────┬─────┘    └─────┬─────┘
       │                 │                 │
       └────────┬────────┴────────┬────────┘
                ▼                 ▼
           vmstat 差值计算（两次采样之间的增量）
                │
                ▼
         vmstat 输出（每秒一行）
```

## 学习路线

| Step | 文件 | 关注点 |
|------|------|--------|
| 1 | `reading/01_vmstat_architecture.md` | 各列含义 + /proc 数据源映射 |
| 2 | `reading/02_command_reference.md` | 参数速查 + 场景命令 |
| 3 | `reading/03_command_output_demo.md` | 实战输出 + 逐列注解 |

## 动手实验

```bash
# 实验 1：基础监控
vmstat 1 30    # 每秒采样，共 30 秒

# 实验 2：系统摘要
vmstat -s

# 实验 3：磁盘统计
vmstat -d

# 实验 4：配合 fio 观察 I/O 压力
vmstat 1 &
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=10 --time_based --iodepth=32
wait

# 实验 5：诊断 CPU 高但吞吐低
# 关注 cs（上下文切换）和 in（中断）列
vmstat 1 | awk 'NR>2{print "cs="$16, "in="$12, "us="$13, "sy="$14}'
```
