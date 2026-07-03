# sar 命令输出实战解析

> 每个命令都实际执行，展示真实输出并逐列注解。
> 系统：Alibaba Cloud Linux 4.0.3，4 核 CPU，7.3 GiB 内存，sysstat-12.7.6
> 日期：2026-07-03

---

## 场景 1：CPU 使用率（基础）

### 命令

```bash
sar -u 1 5
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:24:15 PM     CPU     %user     %nice   %system   %iowait    %steal     %idle
11:24:16 PM     all     20.20      0.00      4.04      0.00      0.00     75.76
11:24:17 PM     all     17.79      0.00      4.76      0.00      0.00     77.44
11:24:18 PM     all     17.22      0.00      3.80      0.00      0.00     78.99
11:24:19 PM     all     16.24      0.00      3.55      0.00      0.00     80.20
11:24:20 PM     all     19.05      0.00      4.51      0.25      0.00     76.19
Average:        all     18.10      0.00      4.14      0.05      0.00     77.71
```

### 逐列注解

```
行 1 (文件头):
  Linux 6.6.102-...  → 内核版本
  iZbp11r90r4g...    → 主机名
  07/03/2026         → 日期
  _x86_64_           → 架构
  (4 CPU)            → ★ CPU 核心数

数据行:
  11:24:16 PM  → 采样时间戳
  all          → 所有 CPU 的汇总值
  %user        → ★ 用户态时间占比 (20.20%)
                  包含应用程序的所有计算时间
  %nice        → nice 优先级进程占比 (0.00%)
                  无 nice 进程在运行
  %system      → ★ 内核态时间占比 (4.04%)
                  系统调用、中断处理、内核线程
  %iowait      → ★ 等待 I/O 的 CPU 时间 (0.00%)
                  0.00% → 当前无 I/O 等待
  %steal       → 被 hypervisor 偷走的时间 (0.00%)
                  虚拟机环境才有值
  %idle        → ★ 空闲 CPU 时间 (75.76%)
                  约 3/4 的 CPU 时间空闲

Average 行:
  → 所有 5 次采样的算术平均值
  → %user 平均 18.10%，%system 平均 4.14%
```

**诊断要点：**
- `%user ≈ 18%` → 有后台进程在消耗 CPU（如本 sar 命令自身）
- `%iowait ≈ 0` → 磁盘不是瓶颈
- `%idle > 75%` → 系统整体空闲

---

## 场景 2：CPU 使用率（全字段）

### 命令

```bash
sar -u ALL 1 3
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:24:21 PM     CPU      %usr     %nice      %sys   %iowait    %steal      %irq     %soft    %guest    %gnice     %idle
11:24:22 PM     all     15.33      0.00      1.26      0.00      0.00      0.75      0.50      0.00      0.00     82.16
11:24:23 PM     all     37.47      0.00      5.32      0.00      0.00      2.03      0.25      0.00      0.00     54.94
11:24:24 PM     all     31.66      0.00     17.34      0.00      0.00      2.01      1.26      0.00      0.00     47.74
Average:        all     28.13      0.00      7.98      0.00      0.00      1.60      0.67      0.00      0.00     61.63
```

### 逐列注解

```
与 -u 的额外列:
  %irq     → 硬中断处理时间 (0.75%~2.03%)
             网卡中断、定时器中断等
  %soft    → ★ 软中断处理时间 (0.25%~1.26%)
             网络包处理（NET_RX/NET_TX）
             高值 → 网络包处理是瓶颈
  %guest   → 运行虚拟机的 CPU 时间 (0.00%)
             仅宿主机有意义
  %gnice   → 虚拟机 nice 时间 (0.00%)
             仅宿主机有意义
```

**-u vs -u ALL 关键区别：**
```
-u:      %system 包含 irq + soft（合并显示）
-u ALL:  %sys 不包含 irq 和 soft（单独列出）

示例对比（同一次采集）:
  -u:     %system = 4.04%  (= sys 1.26 + irq 0.75 + soft 0.50 + 其他)
  -u ALL: %sys    = 1.26%,  %irq = 0.75%,  %soft = 0.50%
```

---

## 场景 3：各 CPU 核心使用率

### 命令

```bash
sar -P ALL 1 3
```

### 输出（截取 1 次采样 + Average）

```
11:25:18 PM     CPU     %user     %nice   %system   %iowait    %steal     %idle
11:25:18 PM     all     18.59      0.00      3.77      0.00      0.00     77.64
11:25:18 PM       0     24.00      0.00      5.00      0.00      0.00     71.00
11:25:18 PM       1     19.39      0.00      3.06      0.00      0.00     77.55
11:25:18 PM       2     12.00      0.00      4.00      0.00      0.00     84.00
11:25:18 PM       3     19.00      0.00      3.00      0.00      0.00     78.00
```

### 逐列注解

```
CPU 列:
  all  → ★ 所有核心的加权平均
  0    → CPU 核心 0: %user=24.00%（最高）
  1    → CPU 核心 1: %user=19.39%
  2    → CPU 核心 2: %user=12.00%（最低）
  3    → CPU 核心 3: %user=19.00%

负载分布:
  CPU 0 比 CPU 2 多 12% → ★ 负载不均衡
  可能原因：中断绑定、进程 CPU 亲和性、调度器决策
```

**诊断场景：**
- 某核心 100% 其他 0% → 单线程瓶颈或 CPU 绑定
- 所有核心均匀 → 并行计算负载
- 某核心 %iowait 高 → I/O 中断绑定到该 CPU

---

## 场景 4：内存使用

### 命令

```bash
sar -r 1 3
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:24:31 PM kbmemfree   kbavail kbmemused  %memused kbbuffers  kbcached  kbcommit   %commit  kbactive   kbinact   kbdirty
11:24:32 PM   1013716   3895460   2634196     34.48    285168   3278472   4546788     59.52   2795088   3291980      2000
11:24:33 PM   1013716   3895464   2634196     34.48    285168   3278472   4546788     59.52   2795120   3291984      2000
11:24:34 PM   1013716   3895464   2634196     34.48    285168   3278472   4546788     59.52   2795020   3291984      2000
Average:      1013716   3895463   2634196     34.48    285168   3278472   4546788     59.52   2795076   3291983      2000
```

### 逐列注解

```
kbmemfree  = 1013716 KB ≈ 990 MB    ← 完全空闲的物理内存
kbavail    = 3895460 KB ≈ 3.7 GB    ← ★ 实际可用内存（含可回收 cache）
kbmemused  = 2634196 KB ≈ 2.5 GB    ← 已使用内存（含 buffers + cached）
%memused   = 34.48%                  ← 已使用占比
                                      ★ 34.48% 不高，但 kbmemfree 看起来少
                                      因为大量内存被 Page Cache 占用（正常！）
kbbuffers  = 285168 KB ≈ 278 MB      ← 内核块设备 buffer（I/O 缓冲）
kbcached   = 3278472 KB ≈ 3.1 GB     ← ★ Page Cache（文件缓存）
                                      占已用内存的大部分 → 正常，加速文件读取
kbcommit   = 4546788 KB ≈ 4.3 GB     ← 已承诺分配的内存总量
%commit    = 59.52%                   ← 承诺分配 / (物理内存+交换) 的比例
kbactive   = 2795088 KB ≈ 2.7 GB     ← 近期活跃页面（不可轻易回收）
kbinact    = 3291980 KB ≈ 3.1 GB     ← 近期不活跃页面（可回收）
kbdirty    = 2000 KB ≈ 2 MB          ← ★ 脏页（待写回磁盘），值很小说明写压力低
```

**★ 关键认知：**
```
物理内存 7.3 GB 的分配：
  ┌────────────────────────────────────────────┐
  │ 空闲     │    Page Cache    │  活跃应用  │其他│
  │ 990 MB   │    3.1 GB        │  2.7 GB    │    │
  └────────────────────────────────────────────┘

  kbavail = 空闲 + 可回收 cache = 990MB + 大部分 3.1GB ≈ 3.7GB
  ★ 所以虽然 kbmemfree 只有 990MB，但实际可用 3.7GB
  ★ 看内存是否充足，看 kbavail 而不是 kbmemfree！
```

---

## 场景 5：内存全字段

### 命令

```bash
sar -r ALL 1 3
```

### 输出

```
11:24:34 PM kbmemfree   kbavail kbmemused  %memused kbbuffers  kbcached  kbcommit   %commit  kbactive   kbinact   kbdirty  kbanonpg    kbslab  kbkstack   kbpgtbl  kbvmused
11:24:35 PM   1009252   3891056   2638528     34.54    285236   3278488   4546788     59.52   2797364   3292064      2060   2509052    427420      6704     30680     18228
Average:      1008916   3890728   2638860     34.54    285236   3278491   4546788     59.52   2796659   3292072      2055   2508361    427421      6661     30533     18185
```

### 额外列注解

```
kbanonpg  = 2509052 KB ≈ 2.4 GB   ← 匿名页面（进程堆、栈分配）
                                    不可回收，只能交换到 swap
kbslab    = 427420 KB ≈ 417 MB    ← 内核 slab 缓存
                                    dentry cache + inode cache
kbkstack  = 6704 KB               ← 内核栈空间
                                    每线程 ~8KB，约 838 个内核线程
kbpgtbl   = 30680 KB ≈ 30 MB      ← 页表占用内存
                                    管理虚拟→物理地址映射的表
kbvmused  = 18228 KB ≈ 18 MB      ← vmalloc 已用空间
                                    内核动态分配的虚拟内存
```

---

## 场景 6：磁盘 I/O（块设备）

### 命令

```bash
sar -d -p 1 3
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:24:37 PM       tps     rkB/s     wkB/s     dkB/s   areq-sz    aqu-sz     await     %util DEV
11:24:38 PM      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00 vda
11:24:38 PM      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00 vdb

11:24:38 PM       tps     rkB/s     wkB/s     dkB/s   areq-sz    aqu-sz     await     %util DEV
11:24:39 PM      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00 vda
11:24:39 PM      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00 vdb

11:24:39 PM       tps     rkB/s     wkB/s     dkB/s   areq-sz    aqu-sz     await     %util DEV
11:24:40 PM      3.00      0.00     68.00      0.00     22.67      0.00      0.67      0.20 vda
11:24:40 PM      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00 vdb

Average:          tps     rkB/s     wkB/s     dkB/s   areq-sz    aqu-sz     await     %util DEV
Average:         1.00      0.00     22.67      0.00     22.67      0.00      0.67      0.07 vda
Average:         0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00 vdb
```

### 逐列注解

```
tps      = 3.00 次/秒     ← ★ IOPS（每秒传输次数）
                            3 次写入（可能是日志/监控写入）
rkB/s    = 0.00 KB/s      ← 读速率（无读操作）
wkB/s    = 68.00 KB/s     ← ★ 写速率
                            3 次写入 × 22.67 KB/次 ≈ 68 KB/s
dkB/s    = 0.00 KB/s      ← discard 速率（TRIM，SSD 才有）
areq-sz  = 22.67 KB       ← ★ 平均请求大小
                            22.67 KB → 中等大小请求
aqu-sz   = 0.00           ← 平均队列深度（几乎为 0）
                            ★ < 1.0 → 设备不繁忙
await    = 0.67 ms        ← ★ 平均等待时间
                            < 1ms → 非常快（virtio-blk 云盘）
%util    = 0.20%          ← ★★ 设备利用率
                            0.20% → 磁盘极度空闲
DEV      = vda            ← 设备名（-p 参数使用友好名称）
```

**★ 磁盘诊断三板斧：**

```
检查顺序:
  1. %util > 90%?     → 磁盘饱和，需扩容或优化
  2. await > 20ms?    → 延迟高，可能饱和或设备慢
  3. aqu-sz > 4?      → 队列积压严重

本例:
  %util = 0.20%  → 极度空闲 ✅
  await = 0.67ms → 非常快 ✅
  aqu-sz = 0.00  → 无排队 ✅
```

**与 iostat 的差异：**
```
iostat 有更多细节:
  r/s, w/s (分离的读写 IOPS)
  rrqm/s, wrqm/s (合并请求)
  r_await, w_await (分离的读写延迟)

sar -d 更简洁:
  tps (合并的 IOPS)
  await (合并的延迟)
  ★ 但支持历史查询！
```

---

## 场景 7：缓冲区 I/O

### 命令

```bash
sar -b 1 3
```

### 输出

```
11:24:40 PM       tps      rtps      wtps      dtps   bread/s   bwrtn/s   bdscd/s
11:24:41 PM      0.00      0.00      0.00      0.00      0.00      0.00      0.00
11:24:42 PM      4.00      0.00      4.00      0.00      0.00    160.00      0.00
11:24:43 PM      0.00      0.00      0.00      0.00      0.00      0.00      0.00
Average:         1.33      0.00      1.33      0.00      0.00     53.33      0.00
```

### 逐列注解

```
tps      = 4.00 次/秒   ← I/O 传输总次数（合并后）
rtps     = 0.00         ← 读传输次数
wtps     = 4.00         ← ★ 写传输次数（4 次写）
dtps     = 0.00         ← discard 次数
bread/s  = 0.00 块/秒   ← ★ 读取的块数（512 字节/块）
                            0 → 无读操作
bwrtn/s  = 160.00 块/秒 ← ★ 写入的块数
                            160 × 512 = 81920 字节 = 80 KB/s
bdscd/s  = 0.00         ← discard 的块数

单位换算:
  bread/bwrtn 的单位是 512 字节块
  → KB/s = bread/s × 0.5
  → 160 块/秒 × 0.5 = 80 KB/s
```

---

## 场景 8：网络接口流量

### 命令

```bash
sar -n DEV 1 3
```

### 输出

```
11:24:46 PM     IFACE   rxpck/s   txpck/s    rxkB/s    txkB/s   rxcmp/s   txcmp/s  rxmcst/s   %ifutil
11:24:47 PM        lo     20.00     20.00      1.80      1.80      0.00      0.00      0.00      0.00
11:24:47 PM      eth0    108.00    104.00     23.66     12.52      0.00      0.00      0.00      0.00

11:24:47 PM     IFACE   rxpck/s   txpck/s    rxkB/s    txkB/s   rxcmp/s   txcmp/s  rxmcst/s   %ifutil
11:24:48 PM        lo     27.00     27.00      2.32      2.32      0.00      0.00      0.00      0.00
11:24:48 PM      eth0    121.00    117.00     30.04      7.15      0.00      0.00      0.00      0.00

11:24:48 PM     IFACE   rxpck/s   txpck/s    rxkB/s    txkB/s   rxcmp/s   txcmp/s  rxmcst/s   %ifutil
11:24:49 PM        lo     34.00     34.00      4.22      4.22      0.00      0.00      0.00      0.00
11:24:49 PM      eth0    135.00    116.00     26.86    175.83      0.00      0.00      0.00      0.00

Average:        IFACE   rxpck/s   txpck/s    rxkB/s    txkB/s   rxcmp/s   txcmp/s  rxmcst/s   %ifutil
Average:           lo     27.00     27.00      2.78      2.78      0.00      0.00      0.00      0.00
Average:         eth0    121.33    112.33     26.86     65.17      0.00      0.00      0.00      0.00
```

### 逐列注解

```
eth0 接口（主要网络接口）:
  IFACE    = eth0           ← 接口名
  rxpck/s  = 108~135 pps    ← ★ 每秒接收包数 (~120 pps)
  txpck/s  = 104~117 pps    ← ★ 每秒发送包数 (~112 pps)
  rxkB/s   = 23~30 KB/s     ← ★ 每秒接收流量 (~27 KB/s)
  txkB/s   = 7~176 KB/s     ← ★ 每秒发送流量（波动大）
                              第 3 秒 txkB=175.83 → 突发发送
  rxcmp/s  = 0              ← 压缩包（通常 0）
  txcmp/s  = 0              ← 压缩包
  rxmcst/s = 0              ← 多播包
  %ifutil  = 0.00%          ← ★ 接口利用率（远低于带宽上限）

lo 接口（本地回环）:
  rxpck = txpck (对称)      ← 本地通信
  rxkB = txkB (对称)        ← 本地回环收发相同
```

**★ 平均包大小分析：**
```
eth0 平均包大小 = rxkB/s / rxpck/s
  = 26.86 KB/s / 121.33 pps
  = 0.22 KB/包 = 226 字节/包

★ 小包（226 字节）→ 典型的管理流量/SSH/监控
  大包（> 1KB）→ 文件传输/数据同步
```

---

## 场景 9：网络错误

### 命令

```bash
sar -n EDEV 1 3
```

### 输出

```
11:24:50 PM     IFACE   rxerr/s   txerr/s    coll/s  rxdrop/s  txdrop/s  txcarr/s  rxfram/s  rxfifo/s  txfifo/s
11:24:51 PM        lo      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00
11:24:51 PM      eth0      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00
Average:           lo      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00
Average:         eth0      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00      0.00
```

### 注解

```
所有列均为 0.00 → ★ 网络完全健康

各列含义:
  rxerr/s  → 接收错误（CRC 校验失败等）
  txerr/s  → 发送错误
  coll/s   → 冲突（半双工才有）
  rxdrop/s → ★ 接收丢弃（ring buffer 满 → 内核来不及处理）
  txdrop/s → 发送丢弃（发送队列满）
  txcarr/s → 载波错误（物理层问题）
  rxfram/s → 帧对齐错误
  rxfifo/s → FIFO 溢出
  txfifo/s → FIFO 溢出
```

**诊断场景：**
- `rxdrop/s > 0` → 增加 ring buffer：`ethtool -G eth0 rx 4096`
- `coll/s > 0` → 半双工链路问题，改为全双工
- `rxerr/s > 0` → 物理层/网卡驱动问题

---

## 场景 10：套接字统计

### 命令

```bash
sar -n SOCK 1 3
```

### 输出

```
11:24:53 PM    totsck    tcpsck    udpsck    rawsck   ip-frag    tcp-tw
11:24:54 PM       277        34         5         0         0        11
11:24:55 PM       288        34         5         0         0        11
11:24:56 PM       278        34         5         0         0        11
Average:          281        34         5         0         0        11
```

### 逐列注解

```
totsck   = 277~288   ← 当前使用的套接字总数
tcpsck   = 34        ← ★ TCP 套接字数（34 个连接）
udpsck   = 5         ← UDP 套接字数
rawsck   = 0         ← 原始套接字（通常 0）
ip-frag  = 0         ← IP 分片数（0 = 无 MTU 问题）
tcp-tw   = 11        ← ★ TIME_WAIT 连接数
                        11 → 正常范围
                        > 1000 → 短连接太多，考虑连接池
```

---

## 场景 11：TCP 活动

### 命令

```bash
sar -n TCP 1 3
```

### 输出

```
11:24:56 PM  active/s passive/s    iseg/s    oseg/s
11:24:57 PM      2.00      1.00    178.00    175.00
11:24:58 PM      0.00      0.00    145.00    140.00
11:24:59 PM      0.00      0.00    150.00    236.00
Average:         0.67      0.33    157.67    183.67
```

### 逐列注解

```
active/s  = 0~2 次/秒   ← 主动连接（客户端发起）
                           第 1 秒有 2 个新连接
passive/s = 0~1 次/秒   ← 被动连接（服务端接受）
iseg/s    = 145~178     ← ★ 每秒接收的 TCP 段数
oseg/s    = 140~236     ← ★ 每秒发送的 TCP 段数
                           第 3 秒 oseg 突增 → 突发发送
```

---

## 场景 12：运行队列与负载

### 命令

```bash
sar -q 1 3
```

### 输出

```
11:24:59 PM   runq-sz  plist-sz   ldavg-1   ldavg-5  ldavg-15   blocked
11:25:00 PM         3       415      1.57      1.09      0.89         0
11:25:01 PM         0       414      1.57      1.09      0.89         0
11:25:02 PM         7       414      1.57      1.09      0.89         0
Average:            3       414      1.57      1.09      0.89         0
```

### 逐列注解

```
runq-sz   = 0~7        ← ★ 运行队列长度
                          第 3 秒突然到 7 → 短暂的进程排队
                          4 核 CPU 下 runq > 4 说明有排队
plist-sz  = 414~415    ← 进程/线程总数（稳定）
ldavg-1   = 1.57       ← ★ 1 分钟负载均值
                          1.57 / 4 核 = 39% → 正常
ldavg-5   = 1.09       ← 5 分钟负载均值
ldavg-15  = 0.89       ← 15 分钟负载均值
                          ldavg-1 > ldavg-5 > ldavg-15
                          → ★ 负载在上升（近期变忙了）
blocked   = 0          ← ★ 阻塞等待 I/O 的进程数
                          0 → 无 I/O 阻塞
```

**负载趋势解读：**
```
ldavg-1 > ldavg-5 > ldavg-15  → 负载上升中
ldavg-1 < ldavg-5 < ldavg-15  → 负载下降中
ldavg-1 ≈ ldavg-5 ≈ ldavg-15  → 负载稳定

本例: 1.57 > 1.09 > 0.89 → ★ 负载在上升
  可能是刚启动了新的后台任务
```

---

## 场景 13：内存分页

### 命令

```bash
sar -B 1 3
```

### 输出

```
11:25:05 PM  pgpgin/s pgpgout/s   fault/s  majflt/s  pgfree/s pgscank/s pgscand/s pgsteal/s  pgprom/s   pgdem/s
11:25:06 PM      0.00      0.00   1239.00      0.00   1410.00      0.00      0.00      0.00      0.00      0.00
11:25:07 PM      0.00    920.00    794.00      0.00    431.00      0.00      0.00      0.00      0.00      0.00
11:25:08 PM      0.00    184.00    297.00      0.00    512.00      0.00      0.00      0.00      0.00      0.00
Average:         0.00    368.00    776.67      0.00    784.33      0.00      0.00      0.00      0.00      0.00
```

### 逐列注解

```
pgpgin/s  = 0.00 KB/s   ← 从磁盘读入的页面（无）
pgpgout/s = 0~920 KB/s  ← ★ 写出到磁盘的页面
                           第 2 秒 920 KB/s → 有写操作（脏页刷盘）
fault/s   = 297~1239    ← 页面错误总数（含 minor fault）
                           非零是正常的！程序访问新页面就触发
majflt/s  = 0.00        ← ★★★ 主缺页错误（需磁盘 I/O）
                           0.00 → ★ 没有严重内存问题
                           > 0 → 内存不足，需从磁盘加载页面
pgfree/s  = 431~1410    ← 释放的页面数
pgscank/s = 0.00        ← ★ kswapd 扫描页数（0 = 无内存压力）
pgscand/s = 0.00        ← ★ 直接回收扫描（0 = 无严重内存压力）
pgsteal/s = 0.00        ← 成功回收的页面（0 = 无需回收）
pgprom/s  = 0.00        ← 页面提升（32 位概念，64 位总是 0）
pgdem/s   = 0.00        ← 页面降级（32 位概念，64 位总是 0）
```

**★ 内存压力诊断阶梯：**

```
健康度   指标表现                                  含义
───────  ────────────────────────────────────────  ──────────────────
✅ 健康    majflt=0, scan=0, steal=0               内存充足
⚠️ 注意   majflt=0, pgscank>0                     kswapd 开始回收
🔴 警告   majflt>0, pgscand>0                     直接回收，性能下降
🔴🔴 严重 majflt>0, pgsteal/pgscan < 50%          回收效率差，OOM 风险
```

**本例判断：所有压力指标均为 0 → ✅ 内存健康**

---

## 场景 14：交换活动

### 命令

```bash
sar -W 1 3
```

### 输出

```
11:25:08 PM  pswpin/s pswpout/s
11:25:09 PM      0.00      0.00
11:25:10 PM      0.00      0.00
11:25:11 PM      0.00      0.00
Average:         0.00      0.00
```

### 注解

```
pswpin/s  = 0.00  ← 换入页面数（0 = 无）
pswpout/s = 0.00  ← 换出页面数（0 = 无）

★ 任何非零值都是警告：
  pswpout > 0 → 物理内存不够，页面被换出到 swap
  pswpin > 0  → 进程需要已换出的页面，等待磁盘读取

本系统无 swap 分区，所以始终为 0
```

---

## 场景 15：交换空间使用率

### 命令

```bash
sar -S 1 3
```

### 输出

```
11:25:11 PM kbswpfree kbswpused  %swpused  kbswpcad   %swpcad
11:25:12 PM         0         0      0.00         0      0.00
11:25:13 PM         0         0      0.00         0      0.00
11:25:14 PM         0         0      0.00         0      0.00
Average:            0         0      0.00         0      0.00
```

### 注解

```
kbswpfree = 0   ← 无 swap 分区（全部为 0）
kbswpused = 0   ← —
%swpused  = 0%  ← —

本系统未配置 swap：
  - 云服务器通常不配 swap
  - 内存充足时无需 swap
  - 如果 %swpused > 50% → 需增加内存或 swap
```

---

## 场景 16：中断统计

### 命令

```bash
sar -I SUM 1 3
```

### 输出

```
11:25:14 PM      INTR       all
11:25:15 PM       sum   5813.00

11:25:15 PM      INTR       all
11:25:16 PM       sum   6214.00

11:25:16 PM      INTR       all
11:25:17 PM       sum   6389.11

Average:         INTR       all
Average:          sum   6139.53
```

### 逐列注解

```
INTR  = sum        ← 汇总所有中断线
all   = 5813~6389  ← ★ 每秒总中断数
                      约 6000 次/秒
                      4 核 × ~1000 timer + 网络中断 + 其他

每秒 6000 次中断的构成（典型）：
  LOC (本地定时器): 4 核 × 1000/s = 4000
  网络中断:         ~1500 (与 rxpck+txpck 相关)
  其他:             ~500 (磁盘、IPI 等)
```

---

## 场景 17：上下文切换与进程创建

### 命令

```bash
sar -w 1 3
```

### 输出

```
11:25:20 PM    proc/s   cswch/s
11:25:21 PM      9.00   8827.00
11:25:22 PM      0.00   8453.00
11:25:23 PM     57.00   8721.00
Average:        22.00   8667.00
```

### 逐列注解

```
proc/s   = 0~57 次/秒   ← 每秒创建的进程数
                           0 = 无新进程
                           57 = 批量创建（可能是 shell 命令展开）
cswch/s  = 8453~8827    ← ★★ 每秒上下文切换次数
                           ~8700 次/秒
                           4 核系统正常范围
```

**★ 上下文切换诊断：**
```
正常范围（4 核系统）：
  空闲:     2000~5000/s
  轻负载:   5000~15000/s
  重负载:   15000~50000/s
  异常:     > 100000/s → ★ 锁竞争或线程过多

本例 ~8700/s → 轻负载，正常 ✅

高 cswch + 高 %system → 锁竞争
  → perf record -g → 找内核热点函数
```

---

## 场景 18：历史数据查询

### 命令

```bash
sar -u -f /var/log/sa/sa03
```

### 输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z) 	07/03/2026 	_x86_64_	(4 CPU)

11:23:24 PM  LINUX RESTART	(4 CPU)
```

### 注解

```
11:23:24 PM  LINUX RESTART (4 CPU)
  │          │              │
  │          │              └─ 4 个 CPU 核心
  │          └─ ★ 系统重启标记（sysstat.service 写入）
  └─ 时间戳

为什么只有 LINUX RESTART？
  → 系统刚重启不久，sadc 还没采集到足够数据
  → 正常采集后，会有多个时间戳记录

★ LINUX RESTART 的作用：
  告诉 sar "之前的计数器数据属于上一轮启动"
  不能将重启前后的数据混合计算差值
  （因为 /proc/stat 中的计数器在重启后归零）
```

---

## 场景 19：系统全貌速查

### 命令

```bash
sar -A 1 1
```

**输出包含所有维度，按以下顺序：**
```
1. CPU 使用率 (-u)
2. 各 CPU 核心 (-P ALL)
3. 内存使用 (-r)
4. 内存分页 (-B)
5. 缓冲区 I/O (-b)
6. 块设备 I/O (-d)
7. 文件系统 (-F)
8. 交换活动 (-W)
9. 交换空间 (-S)
10. 运行队列 (-q)
11. 网络接口 (-n DEV)
12. 网络错误 (-n EDEV)
13. 套接字 (-n SOCK)
14. TCP (-n TCP)
15. 中断 (-I SUM)
16. 上下文切换 (-w)
17. Huge Pages (-H)
18. 内核表 (-v)
```

**使用场景：** 系统巡检第一步，一次性看所有维度，快速发现异常。

---

## 综合诊断流程

```
Step 1: 系统巡检
  sar -A 1 3
  │
  ├── %iowait > 10%?
  │   └──→ sar -d -p 1 10 → 哪块磁盘 await 高
  │
  ├── %system > 30%?
  │   └──→ sar -P ALL 1 10 → 是所有 CPU 还是个别的
  │
  ├── %memused > 90%?
  │   └──→ sar -B 1 10 → majflt > 0? → 内存不足
  │
  ├── runq-sz > CPU 核数?
  │   └──→ sar -q 1 10 → blocked > 0? → I/O 瓶颈
  │
  └── 一切正常? → 检查应用层面

Step 2: 历史回溯（故障发生在过去）
  sar -u -f /var/log/sa/saXX -s HH:MM -e HH:MM
  │
  └──→ 找到异常时间段 → 扩大维度查看
       sar -A -f /var/log/sa/saXX -s HH:MM -e HH:MM

Step 3: 数据导出（进一步分析）
  sadf -d /var/log/sa/saXX -- -u > cpu.csv
  sadf -g /var/log/sa/saXX -- -u > cpu.svg
```
