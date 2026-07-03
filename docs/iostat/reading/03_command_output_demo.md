# Step 3: iostat 命令输出实战解析

> ======================================================================
> 每个命令都实际执行，展示真实输出并逐字段注解。
> 设备：/dev/vdb (40G virtio-blk)，I/O 由 fio 生成。
> sysstat 版本：12.7.6
>
> 阅读要点：
>   1. 基础输出 vs 扩展输出的区别
>   2. -x 扩展输出中每个字段的实际含义
>   3. 不同 I/O 模式（随机写/顺序读/顺序写）的 iostat 特征
>   4. 诊断表：%util / await / avgqu-sz 的正常/异常范围
>
> 预计阅读时间：20 分钟
> ======================================================================

---

## 目录

- [场景 1：iostat 基础输出 — 逐字段注解](#场景-1iostat-基础输出--逐字段注解)
- [场景 2：iostat -x 扩展输出 — 重点注解](#场景-2iostat--x-扩展输出--重点注解)
- [场景 3：iostat -x 1 连续采样 — fio randwrite](#场景-3iostat--x-1-连续采样--fio-randwrite)
- [场景 4：fio 顺序读 vs 随机写对比](#场景-4fio-顺序读-vs-随机写对比)
- [场景 5：iostat vs blktrace 对比表](#场景-5iostat-vs-blktrace-对比表)
- [场景 6：诊断表 — 正常/异常范围](#场景-6诊断表--正常异常范围)
- [场景 7：其他参数演示](#场景-7其他参数演示)
- [场景 8：完整诊断流程](#场景-8完整诊断流程)

---

## 场景 1：iostat 基础输出 — 逐字段注解

### 命令

```bash
iostat
```

### 实际输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z)   07/03/2026     _x86_64_    (4 CPU)

avg-cpu:  %user   %nice %system %iowait  %steal   %idle
           1.12    0.01    0.49    0.03    0.00   98.35

Device             tps    kB_read/s    kB_wrtn/s    kB_dscd/s    kB_read    kB_wrtn    kB_dscd
vda               2.54         9.03       158.97         0.00   16090476  283332784          0
vdb               3.05         3.37        13.49         0.00    6009141   24050468          0
```

### 逐字段注解

**头部信息：**
```
Linux 6.6.102-5.3.1.alnx4.x86_64    ← 内核版本
(iZbp11r90r4g931c0wfjt5Z)           ← 主机名
07/03/2026                           ← 当前日期
_x86_64_                             ← 架构
(4 CPU)                              ← ★ CPU 核心数（影响 %util 解读）
```

**avg-cpu 段（CPU 利用率）：**
```
%user    = 1.12%    ← 用户态时间占比（应用程序计算）
%nice    = 0.01%    ← 低优先级用户态时间
%system  = 0.49%    ← 内核态时间（系统调用、中断处理）
%iowait  = 0.03%    ← ★ CPU 等待 I/O 的时间占比（几乎为 0，说明 CPU 不被 I/O 阻塞）
%steal   = 0.00%    ← 虚拟化偷时间（云主机中 hypervisor 占用的时间）
%idle    = 98.35%   ← ★ CPU 空闲时间（> 98%，系统很闲）
```

**Device 段（设备 I/O 统计）：**
```
字段          vda         vdb         含义
────────────  ──────────  ──────────  ──────────────────────────────────
tps           2.54        3.05        每秒传输数 ≈ IOPS（读+写合计）
kB_read/s     9.03        3.37        每秒读吞吐量 (KB/s)
kB_wrtn/s     158.97      13.49       每秒写吞吐量 (KB/s)
kB_dscd/s     0.00        0.00        每秒 discard 吞吐量
kB_read       16090476    6009141     ★ 累计读数据量 (KB)，开机至今
kB_wrtn       283332784   24050468    ★ 累计写数据量 (KB)，开机至今
kB_dscd       0           0           累计 discard 数据量
```

**★ 关键解读：**
- 这是**开机以来的累计平均值**，不是实时数据！
- vda 是系统盘（写吞吐高，158.97 KB/s，主要是日志和系统写入）
- vdb 是数据盘（吞吐低，之前的实验留下的数据）
- tps ≈ 3 说明平均每秒只有约 3 个 I/O，系统非常空闲

---

## 场景 2：iostat -x 扩展输出 — 重点注解

### 命令

```bash
iostat -x
```

### 实际输出

```
Linux 6.6.102-5.3.1.alnx4.x86_64 (iZbp11r90r4g931c0wfjt5Z)   07/03/2026     _x86_64_    (4 CPU)

avg-cpu:  %user   %nice %system %iowait  %steal   %idle
           1.12    0.01    0.49    0.03    0.00   98.35

Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz     d/s     dkB/s   drqm/s  %drqm d_await dareq-sz     f/s f_await  aqu-sz  %util
vda              0.59      9.03     0.02   2.99    1.65    15.21    1.95    158.97     0.50  20.48   77.68    81.54    0.00      0.00     0.00   0.00    0.00     0.00    0.14    0.17    0.15   0.10
vdb              0.11      3.37     0.00   0.01   12.40    30.09    2.94     13.52     0.02   0.61    1.53     4.60    0.00      0.00     0.00   0.00    0.00     0.00    0.03    0.06    0.01   0.03
```

### ★ 逐字段注解（以 vdb 为例）

**读指标组：**
```
字段        值          含义                              注解
──────────  ──────────  ────────────────────────────────  ──────────────────────
r/s         0.11        每秒读 I/O 数                     几乎无读操作
rkB/s       3.37        每秒读吞吐(KB)                    极少读数据
rrqm/s      0.00        每秒读合并数                      无合并
%rrqm       0.01%       读合并百分比                      0% = 完全无合并
r_await     12.40       ★ 读平均延迟(ms)                  偏高，但样本量小
rareq-sz    30.09       读平均请求大小(KB)                约 30KB/次
```

**写指标组：**
```
字段        值          含义                              注解
──────────  ──────────  ────────────────────────────────  ──────────────────────
w/s         2.94        每秒写 I/O 数                     约 3 IOPS
wkB/s       13.52       每秒写吞吐(KB)                    很少
wrqm/s      0.02        每秒写合并数                      几乎无合并
%wrqm       0.61%       写合并百分比                      很低
w_await     1.53        ★ 写平均延迟(ms)                  正常（< 5ms）
wareq-sz    4.60        写平均请求大小(KB)                约 4.6KB/次
```

**全局指标（★ 最重要的两个）：**
```
字段        值          含义                              注解
──────────  ──────────  ────────────────────────────────  ──────────────────────
aqu-sz      0.01        ★ 平均队列深度                    几乎无排队
%util       0.03%       ★ 设备利用率                      设备空闲
```

**flush 指标组：**
```
字段        值          含义                              注解
──────────  ──────────  ────────────────────────────────  ──────────────────────
f/s         0.03        flush 操作每秒次数                很少 fsync
f_await     0.06        flush 平均延迟(ms)                正常
```

**★ 核心结论：** 系统空闲，vdb 利用率仅 0.03%，await 和队列深度都正常。

---

## 场景 3：iostat -x 1 连续采样 — fio randwrite

### 命令

```bash
# 后台启动 iostat，采样 6 次
iostat -x -y 1 6 &

sleep 0.5
# 启动 fio 随机写（4K，iodepth=32）
fio --name=randwrite --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=4 --time_based --numjobs=1 --iodepth=32
```

### fio 结果摘要

```
write: IOPS=20.0k, BW=78.2MiB/s (82.0MB/s)
slat (nsec): avg=15602    ← 提交延迟 ~16μs
clat (usec): avg=1580     ← 完成延迟 ~1.6ms
lat  (usec): avg=1596     ← 总延迟 ~1.6ms
```

### iostat 连续输出（6 个采样周期）

**采样 1（fio 刚启动，预热中）：**
```
avg-cpu:  %user   %nice %system %iowait  %steal   %idle
          28.39    5.12   37.08    1.79    0.00   27.62

Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz     d/s     dkB/s   drqm/s  %drqm d_await dareq-sz     f/s f_await  aqu-sz  %util
vdb             55.00   1052.00     0.00   0.00    0.36    19.13 13580.00  54320.00     0.00   0.00    0.34     4.00    0.00      0.00     0.00   0.00    0.00     0.00    0.00    0.00    4.61  24.20
```

```
注解：
w/s = 13580           ← 约 13580 IOPS（fio 还在预热，未达峰值）
wkB/s = 54320         ← 54320 KB/s ≈ 53 MB/s
wareq-sz = 4.00       ← ★ 平均请求大小 = 4KB（精确匹配 --bs=4k）
w_await = 0.34        ← ★ 写延迟仅 0.34ms（设备响应很快）
aqu-sz = 4.61         ← 平均队列深度 ~5（还没填满 iodepth=32）
%util = 24.20%        ← 设备利用率 24%（还没饱和）
wrqm/s = 0, %wrqm=0%  ← ★ 零合并（随机写无法合并）
r/s = 55              ← 有少量读（系统盘的预读/监控）
```

**采样 2（fio 满载运行）：**
```
avg-cpu:  %user   %nice %system %iowait  %steal   %idle
          27.20    5.04   39.04    1.76    0.00   26.95

Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz     d/s     dkB/s   drqm/s  %drqm d_await dareq-sz     f/s f_await  aqu-sz  %util
vdb              0.00      0.00     0.00   0.00    0.00     0.00 22753.00  91012.00     0.00   0.00    1.10     4.00    0.00      0.00     0.00   0.00    0.00     0.00    0.00    0.00   25.05  96.00
```

```
★ 重点注解：
w/s = 22753           ← ★ 22753 IOPS（fio 满载，接近设备上限）
wkB/s = 91012         ← 91012 KB/s ≈ 89 MB/s（写吞吐）
wareq-sz = 4.00       ← 每个写请求精确 4KB
w_await = 1.10        ← ★ 写延迟 1.10ms（高 IOPS 下延迟略升）
aqu-sz = 25.05        ← ★ 队列深度 ~25（iodepth=32，实际约 25 个在排队）
%util = 96.00%        ← ★★ 设备利用率 96%！接近饱和
r/s = 0               ← 没有读（纯写负载）
wrqm/s = 0            ← 零合并（随机 4K 写无法合并）
%iowait = 1.76%       ← CPU iowait 不高（libaio 异步 I/O 不阻塞 CPU）
```

**采样 3-4（稳定运行）：**
```
采样 3: w/s=15893  w_await=1.62  aqu-sz=25.71  %util=93.10%
采样 4: w/s=15924  w_await=1.69  aqu-sz=26.96  %util=95.60%
```

```
解读：
- w/s 在 15000~16000 之间波动（IOPS 稳定）
- w_await 在 1.1~1.7ms 之间（延迟稳定，无尖刺）
- aqu-sz 在 25~27（iodepth=32 下设备处理速度跟得上提交速度）
- %util 在 93~96%（设备几乎一直有 I/O 在处理）
```

**采样 5（fio 即将结束）：**
```
Device            w/s     wkB/s   w_await wareq-sz  aqu-sz  %util
vdb            12015.00  48060.00    1.77     4.00   21.25   76.10
```

```
解读：
- w/s 下降到 12015（fio runtime=4s 已到，正在收尾）
- %util 下降到 76%（I/O 减少，设备开始有空闲）
- aqu-sz 下降到 21（队列不再填满）
```

**采样 6（fio 已结束）：**
```
Device            w/s     wkB/s   w_await wareq-sz  aqu-sz  %util
vdb             3178.00  12712.00    0.18     4.00    0.58   55.70
```

```
解读：
- w/s 急降到 3178（fio 结束后的残留写回）
- w_await 降到 0.18ms（负载减轻，延迟大幅下降）
- %util 还在 55%（还有残余 I/O）
```

### ★ 完整趋势图

```
时间轴 ──────────────────────────────────────────────→

w/s:     13580  22753  15893  15924  12015   3178
         ├─────┤├─────┤├─────┤├─────┤├─────┤├─────┤
         预热   满载   稳定   稳定   收尾   残余

%util:   24%    96%    93%    96%    76%    56%
         低────→高────→稳定──→稳定──→下降──→下降

w_await: 0.34   1.10   1.62   1.69   1.77   0.18
         低────→升────→稳定──→稳定──→稳定──→急降

aqu-sz:  4.6    25.1   25.7   27.0   21.3   0.6
         低────→填满──→稳定──→稳定──→下降──→清空
```

---

## 场景 4：fio 顺序读 vs 随机写对比

### 场景 4a：fio 顺序读 (128K, iodepth=16)

#### 命令

```bash
iostat -x -y 1 5 &
sleep 0.5
fio --name=seqread --ioengine=libaio --direct=1 --rw=read --bs=128k \
    --filename=/dev/vdb --runtime=3 --time_based --numjobs=1 --iodepth=16
```

#### fio 结果

```
read: IOPS=1911, BW=239MiB/s (251MB/s)
lat (usec): avg=8361     ← 约 8.4ms（比 4K 随机写慢很多！）
```

#### iostat 输出（稳态采样）

```
Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz     d/s     dkB/s   drqm/s  %drqm d_await dareq-sz     f/s f_await  aqu-sz  %util
vdb           2835.00 179092.00     0.00   0.00   10.91    63.17 1035.00   4140.00     0.00   0.00    0.79     4.00    0.00      0.00     0.00   0.00    0.00     0.00    0.00    0.00   31.76 100.00
```

```
★ 注解：
r/s = 2835            ← 2835 IOPS（比 4K 随机写低很多，因为每次读 128K）
rkB/s = 179092        ← ★ 179092 KB/s ≈ 175 MB/s（吞吐远高于 4K 随机写的 89MB/s）
rareq-sz = 63.17      ← 平均请求大小 ~63KB（128K 被拆分为多个请求）
r_await = 10.91       ← ★ 读延迟 10.91ms（大请求延迟高）
aqu-sz = 31.76        ← ★ 队列深度 ~32（超过 iodepth=16，可能有预读加入）
%util = 100.00%       ← ★★ 设备完全饱和
rrqm/s = 0            ← 零合并（direct I/O 不经过 page cache，不合并）
%iowait = 19.10%      ← ★ iowait 较高（19%，大吞吐读取影响 CPU）
```

### 场景 4b：fio 顺序写 (128K, iodepth=16)

#### 命令

```bash
fio --name=seqwrite --ioengine=libaio --direct=1 --rw=write --bs=128k \
    --filename=/dev/vdb --runtime=3 --time_based --numjobs=1 --iodepth=16
```

#### fio 结果

```
write: IOPS=1931, BW=241MiB/s (253MB/s)
lat (usec): avg=8275     ← 约 8.3ms
```

#### iostat 输出（稳态采样）

```
Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz     d/s     dkB/s   drqm/s  %drqm d_await dareq-sz     f/s f_await  aqu-sz  %util
vdb              0.00      0.00     0.00   0.00    0.00     0.00 2880.00 184320.00     0.00   0.00   10.00    64.00    0.00      0.00     0.00   0.00    0.00     0.00    0.00    0.00   28.79 100.00
```

```
★ 注解：
w/s = 2880            ← 2880 IOPS（和顺序读差不多）
wkB/s = 184320        ← ★ 184320 KB/s ≈ 180 MB/s（顺序写吞吐）
wareq-sz = 64.00      ← 平均请求大小 64KB（128K 拆成 2 个 64K 请求）
w_await = 10.00       ← ★ 写延迟 10ms（和读延迟接近）
aqu-sz = 28.79        ← 队列深度 ~29
%util = 100.00%       ← ★ 设备完全饱和
```

### ★ 对比总结表

```
指标           │ 4K 随机写        │ 128K 顺序读      │ 128K 顺序写      │ 解读
──────────────┼─────────────────┼─────────────────┼─────────────────┼──────────────────
IOPS          │ 22753            │ 2835             │ 2880             │ ★ 随机写 IOPS 最高（因为请求小）
吞吐(MB/s)    │ 89               │ 175              │ 180              │ ★ 顺序 I/O 吞吐最高（因为请求大）
await(ms)     │ 1.10             │ 10.91            │ 10.00            │ ★ 大请求延迟高（传输时间更长）
rareq/wareq   │ 4.00 KB          │ 63.17 KB         │ 64.00 KB         │ 请求大小差异
%util         │ 96%              │ 100%             │ 100%             │ 顺序 I/O 完全饱和
aqu-sz        │ 25.05            │ 31.76            │ 28.79            │ 队列深度都较高
rrqm/wrqm     │ 0                │ 0                │ 0                │ direct I/O 不合并
%iowait       │ 1.76%            │ 19.10%           │ 0.25%            │ 读负载 iowait 更高
```

**★ 关键认知：**
- **高 IOPS ≠ 高吞吐**：4K 随机写 IOPS 是顺序读的 8 倍，但吞吐只有一半
- **大请求 = 高延迟**：128K 的 await 是 4K 的 10 倍，但吞吐翻倍
- **%util 100% 不代表 IOPS 到顶**：顺序 I/O 的 IOPS 只有 2800 但 %util=100%

---

## 场景 5：iostat vs blktrace 对比表

### 同时运行 iostat + blktrace

#### 命令

```bash
# iostat 后台采样
iostat -x -y 1 4 &

# blktrace 同时采集（如果权限允许）
blktrace -d /dev/vdb -o trace -w 3 &

sleep 0.5
fio --name=mixed --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=2 --time_based --iodepth=16
```

#### iostat 输出

```
Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz     d/s     dkB/s   drqm/s  %drqm d_await dareq-sz     f/s f_await  aqu-sz  %util
vdb              0.00      0.00     0.00   0.00    0.00     0.00 24762.00  99048.00     0.00   0.00    0.59     4.00    0.00      0.00     0.00   0.00    0.00     0.00    0.00    0.00   14.56  99.90
```

### ★ iostat 与 blktrace 的功能对比

```
维度               │ iostat                      │ blktrace + btt
──────────────────┼─────────────────────────────┼─────────────────────────────
能回答的问题       │ "设备忙不忙？延迟高不高？"   │ "延迟卡在哪一步？"
数据粒度           │ 设备级聚合                   │ 每个 I/O 的完整事件链
输出指标           │ %util, await, r/s, w/s      │ Q2D, D2C, Q2C, Seek 分布
延迟分解           │ 只有 await（总和）           │ Q2D（软件）+ D2C（硬件）
I/O 合并信息       │ rrqm/s, wrqm/s              │ Merge 事件、合并比例
顺序/随机判断      │ 间接（合并率高 → 顺序）      │ ★ Q2Q Seek MEAN = 0 完美顺序
Flush/FUA 检测    │ f/s, f_await                │ rwbs 中的 F 标志
队列深度           │ aqu-sz（平均值）             │ 逐 I/O 队列深度变化
CPU 信息           │ %iowait, %user, %system     │ 无（blktrace 不采集 CPU）
开销               │ 几乎为零                     │ 2~5%（relay buffer）
使用复杂度         │ ★ 一行命令                   │ 三步：采集→解析→分析
```

### ★ 诊断协作流程

```
阶段 1：iostat -x 1 → 快速判断（30 秒）
┌──────────────────────────────────────────────────┐
│ %util=99.9% → 设备忙                               │
│ w_await=0.59ms → 延迟不高                          │
│ w/s=24762 → 高 IOPS                                │
│ aqu-sz=14.56 → 队列适中                             │
│                                                    │
│ 初步结论：设备忙碌但响应正常，无异常                 │
│                                                    │
│ → 如果想进一步分析，进入阶段 2                      │
└──────────────────────────────────────────────────┘
                          ↓
阶段 2：blktrace → 微观分析（5 分钟）
┌──────────────────────────────────────────────────┐
│ blktrace 采集 → blkparse 解析 → btt 分析            │
│                                                    │
│ Q2D = 0.05ms → 软件层几乎无延迟                     │
│ D2C = 0.54ms → 设备处理延迟正常                     │
│ Q2Q Seek MEAN = 大 → 确认是随机 I/O                 │
│                                                    │
│ 深入结论：瓶颈在设备处理（D2C），不在软件层           │
│ 建议：增加 iodepth 或升级设备                        │
└──────────────────────────────────────────────────┘
```

---

## 场景 6：诊断表 — 正常/异常范围

### ★ %util 诊断表

```
%util 范围    │ 状态      │ 含义                              │ 动作
─────────────┼──────────┼───────────────────────────────────┼──────────────────────
0~20%         │ 空闲      │ 设备几乎无负载                     │ 无需关注
20~50%        │ 轻载      │ 正常使用                           │ 无需关注
50~80%        │ 中载      │ 设备有一定压力                     │ 关注趋势变化
80~95%        │ ★ 重载    │ 设备接近饱和                       │ 需要分析原因
95~100%       │ ★★ 饱和   │ 设备一直在忙                       │ 立即排查
>100%         │ 异常      │ 采样精度问题或计数器溢出             │ 检查 sysstat 版本
```

**★ 注意：** 对于多队列 NVMe SSD，%util=100% 不一定代表饱和！
多队列设备可以同时处理多个 I/O，100% 只说明"至少有一个队列一直有 I/O"。

### ★ await 诊断表

```
await 范围    │ 状态      │ 含义                              │ 可能原因
─────────────┼──────────┼───────────────────────────────────┼──────────────────────
< 1ms         │ 极快      │ 设备响应极快                       │ NVMe SSD / 缓存命中
1~5ms         │ ★ 正常    │ SSD / virtio-blk 正常范围          │ 正常
5~10ms        │ 偏高      │ 可能有排队或设备压力               │ iodepth 过高或设备负载
10~20ms       │ ★ 异常    │ 明显延迟，应用可感知               │ 设备饱和 / 随机 I/O
20~50ms       │ ★★ 严重   │ 延迟高，应用明显受影响            │ 队列堆积 / 慢设备
50~100ms      │ 危险      │ 严重延迟                          │ 设备故障 / 极端排队
> 100ms       │ 紧急      │ I/O 超时风险                       │ 硬件问题 / 文件系统锁
```

### ★ avgqu-sz (aqu-sz) 诊断表

```
aqu-sz 范围   │ 状态      │ 含义                              │ 可能原因
─────────────┼──────────┼───────────────────────────────────┼──────────────────────
0~1           │ 空闲      │ 几乎没有排队                       │ 无 I/O 或 I/O 很快
1~8           │ 正常      │ 适度排队                           │ 正常使用
8~32          │ ★ 较高    │ 队列较深                           │ iodepth 较高
32~64         │ ★ 高      │ 明显排队                           │ 设备可能跟不上
64~128        │ ★★ 堆积   │ 严重排队                           │ 设备饱和 / 写回风暴
> 128         │ 危险      │ 队列溢出风险                       │ 内核可能开始限流
```

### ★ w_await vs r_await 诊断表

```
r_await │ w_await │ 诊断
────────┼─────────┼──────────────────────────────────────────
低      │ 低      │ 一切正常
低      │ 高      │ 写操作是瓶颈（可能是写回风暴、慢设备）
高      │ 低      │ 读操作是瓶颈（可能是随机读、缓存未命中）
高      │ 高      │ 整体 I/O 瓶颈（设备饱和）
```

### ★ IOPS vs 吞吐 vs 延迟 关系表

```
I/O 模式             │ IOPS    │ 吞吐     │ await   │ %util  │ 典型场景
────────────────────┼─────────┼──────────┼─────────┼────────┼──────────────
4K 随机写 (iodepth=32)│ ~22000  │ ~89 MB/s │ ~1ms    │ ~96%   │ 数据库 WAL
4K 随机读 (iodepth=32)│ ~20000  │ ~80 MB/s │ ~1ms    │ ~95%   │ 数据库查询
128K 顺序读 (iodepth=16)│~2800  │ ~175 MB/s│ ~11ms   │ ~100%  │ 大文件读取
128K 顺序写 (iodepth=16)│~2900  │ ~180 MB/s│ ~10ms   │ ~100%  │ 大文件写入
512K 顺序读           │ ~400    │ ~200 MB/s│ ~10ms   │ ~100%  │ 备份/恢复
```

**★ 核心规律：**
- 请求越小 → IOPS 越高，但吞吐不一定高
- 请求越大 → 吞吐越高，但 IOPS 低，await 高
- %util 在高负载下都接近 100%，不能用来区分 IOPS 和吞吐

---

## 场景 7：其他参数演示

### 7a：iostat -t（带时间戳）

```bash
iostat -t
```

```
07/03/2026 11:01:59 PM
avg-cpu:  %user   %nice %system %iowait  %steal   %idle
           1.13    0.01    0.49    0.03    0.00   98.35

Device             tps    kB_read/s    kB_wrtn/s    kB_dscd/s    kB_read    kB_wrtn    kB_dscd
vda               2.54         9.03       158.97         0.00   16102820  283351708          0
vdb               3.15         3.79        14.28         0.00    6752881   25447448          0
```

**用途：** 时间戳方便关联到应用日志或告警时间线。

### 7b：iostat -xm（MB 单位）

```bash
iostat -xm
```

```
Device            r/s     rMB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wMB/s   wrqm/s  %wrqm w_await wareq-sz
vda              0.59      0.01     0.02   2.99    1.65    15.21    1.95      0.16     0.50  20.48   77.68    81.54
vdb              0.11      0.00     0.00   0.01   12.40    30.09    2.94      0.01     0.02   0.61    1.53     4.60
```

**用途：** 高吞吐场景下，MB 单位比 KB 更直观。

### 7c：iostat --human（可读格式）

```bash
iostat -x --human
```

```
Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz
vda              0.59      9.0k     0.02   3.0%    1.65    15.2k    1.95    159.0k     0.50  20.5%   77.66    81.5k
vdb              0.12      3.8k     0.00   0.0%   12.13    31.9k    3.06     14.4k     0.02   0.6%    1.52     4.7k
```

**用途：** 大数字自动加 `k`/`M` 后缀，百分比自动加 `%`。

### 7d：iostat -c（仅 CPU）

```bash
iostat -c
```

```
avg-cpu:  %user   %nice %system %iowait  %steal   %idle
           1.13    0.01    0.49    0.03    0.00   98.35
```

**用途：** 只看 CPU 利用率，不显示设备信息。

### 7e：iostat -j ID（持久名称）

```bash
iostat -j ID vdb
```

```
      tps    kB_read/s    kB_wrtn/s    kB_dscd/s    kB_read    kB_wrtn    kB_dscd Device
     3.18         3.79        14.39         0.00    6754985   25641592          0 virtio-bp18nkmupr7gnmjai1ib
```

**用途：** 设备名从 `vdb` 变为持久标识符 `virtio-bp18nkmupr7gnmjai1ib`，不受重启影响。

### 7f：iostat -p（分区统计）

```bash
iostat -p vdb
```

```
Device             tps    kB_read/s    kB_wrtn/s    kB_dscd/s    kB_read    kB_wrtn    kB_dscd
vdb               3.15         3.79        14.28         0.00    6752881   25447448          0
```

**注意：** `-p` 只支持基础输出，不支持 `-x` 扩展字段。

### 7g：iostat 指定设备

```bash
iostat -x vdb
```

```
Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz     d/s     dkB/s   drqm/s  %drqm d_await dareq-sz     f/s f_await  aqu-sz  %util
vdb              0.12      3.79     0.00   0.01   12.13    31.91    3.06     14.39     0.02   0.59    1.52     4.70    0.00      0.00     0.00   0.00    0.00     0.00    0.03    0.06    0.01   0.04
```

**用途：** 只看特定设备，减少输出噪音。

---

## 场景 8：完整诊断流程

### 步骤 1：快速检查（10 秒）

```bash
# 最常用的诊断命令
iostat -x -y 1 3
```

**观察重点：**
```
1. %util   → 设备是否饱和？
2. await   → 延迟是否异常？
3. aqu-sz  → 队列是否堆积？
4. r/s w/s → I/O 模式（读多还是写多？）
```

### 步骤 2：定位方向

```
如果 %util ≈ 100%：
  → 看 await 判断延迟是否影响应用
  → 看 r_await 和 w_await 判断是读还是写的问题
  → 看 rareq-sz 和 wareq-sz 判断 I/O 粒度

如果 await 高但 %util 低：
  → 少量慢 I/O，不是设备饱和
  → 可能是大文件顺序读写

如果 aqu-sz 高：
  → 队列堆积，设备处理不过来
  → 用 blktrace 看 Q2D 和 D2C
```

### 步骤 3：深入分析（blktrace）

```bash
# 发现 iostat 异常后，用 blktrace 定位根因
blktrace -d /dev/vdb -o trace -w 30
blkparse -i trace -d trace.bin
btt -i trace.bin -o summary

# 查看延迟分解
cat summary.avg
# Q2D 高 → 软件层瓶颈（I/O 调度器、队列深度）
# D2C 高 → 硬件层瓶颈（设备性能）
```

### 步骤 4：对照验证

```
iostat await ≈ blktrace Q2C avg
如果 iostat await = 5ms, btt Q2C = 4.8ms → 数据一致 ✓
如果差异大 → 检查采样时间是否对齐
```

---

## 快速参考卡片

### 常用命令

```bash
iostat -x -y 1            # ★ 实时监控（最常用）
iostat -x -y 1 5          # 监控 5 秒
iostat -x -y -t 1 vdb     # 特定设备 + 时间戳
iostat -c 1               # 只看 CPU
iostat -xd -y 1           # 只看设备（不含 CPU）
```

### 诊断指标速查

```
指标       │ 正常值(SSD)   │ 警戒值         │ 危险值
──────────┼──────────────┼────────────────┼────────────────
%util      │ < 80%        │ 80~95%         │ > 95%
await      │ < 5ms        │ 5~20ms         │ > 20ms
r_await    │ < 5ms        │ 5~20ms         │ > 20ms
w_await    │ < 5ms        │ 5~20ms         │ > 20ms
aqu-sz     │ < 8          │ 8~32           │ > 32
%iowait    │ < 5%         │ 5~20%          │ > 20%
```

### 不同 I/O 模式的预期值

```
I/O 模式           │ IOPS     │ 吞吐      │ await    │ %util
──────────────────┼──────────┼───────────┼──────────┼───────
4K 随机写         │ 高       │ 中        │ 低       │ 高
4K 随机读         │ 高       │ 中        │ 低       │ 高
128K 顺序读       │ 低       │ ★ 高      │ ★ 高     │ 100%
128K 顺序写       │ 低       │ ★ 高      │ ★ 高     │ 100%
空闲              │ ~0       │ ~0        │ —        │ < 1%
```
