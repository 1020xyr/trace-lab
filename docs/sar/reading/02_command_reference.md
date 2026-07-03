# sar 命令参考手册

> 按系统维度分类，覆盖所有常用参数。每个参数附带使用场景和诊断要点。
> 版本：sysstat-12.7.6

---

## 目录

- [1. 基本语法](#1-基本语法)
- [2. CPU 相关](#2-cpu-相关)
- [3. 内存相关](#3-内存相关)
- [4. I/O 相关](#4-io-相关)
- [5. 网络相关](#5-网络相关)
- [6. 进程与队列相关](#6-进程与队列相关)
- [7. 中断相关](#7-中断相关)
- [8. 其他参数](#8-其他参数)
- [9. 历史数据查询](#9-历史数据查询)
- [10. 输出控制](#10-输出控制)
- [11. 场景速查表](#11-场景速查表)

---

## 1. 基本语法

```bash
# 实时采集
sar [选项] [间隔秒数] [采样次数]

# 历史查询
sar [选项] -f [数据文件]

# 示例
sar -u 1 5                  # CPU 使用率，1 秒 1 次，共 5 次
sar -r -f /var/log/sa/sa01  # 读取 1 号的内存历史
```

**通用规则：**
- 不指定 `-f` → 实时模式（调用 sadc 子进程）
- 指定 `-f` → 历史模式（读取 sa 二进制文件）
- `interval count` 只在实时模式下有效
- 省略 count → 持续采集直到 Ctrl+C
- 省略 interval 和 count → 读取当天全部历史

---

## 2. CPU 相关

### 2.1 sar -u — CPU 使用率（基础）

```bash
sar -u 1 5
```

```
列名       含义                          正常范围       异常信号
─────────  ────────────────────────────  ────────────  ──────────────────
%user      用户态 CPU 时间占比            < 70%         > 90% → 用户程序密集计算
%nice      nice 优先级进程占比            < 5%          高值 → 有人在用 nice
%system    内核态 CPU 时间占比            < 30%         > 50% → 内核开销大（锁/系统调用）
%iowait    ★ 等待 I/O 完成的 CPU 时间    < 5%          > 10% → 磁盘瓶颈
%steal     被 hypervisor 偷走的时间       0%            > 0% → 虚拟机环境
%idle      空闲 CPU 时间占比              > 20%         < 5% → CPU 几乎满负载
```

**诊断要点：**
- `%user` 高 → 正常业务负载，或死循环/bug
- `%system` 高 → 检查系统调用频率（strace）、锁竞争（perf）
- `%iowait` 高 → 配合 `iostat -xz` 定位哪块磁盘慢
- `%steal` 非零 → 宿主机超卖，或 Noisy Neighbor

### 2.2 sar -u ALL — CPU 使用率（全字段）

```bash
sar -u ALL 1 3
```

```
额外列      含义                          说明
──────────  ────────────────────────────  ──────────────────
%usr        = %user（不同显示名）          同 %user
%sys        = %system（不同显示名）        同 %system
%irq        硬中断处理时间占比             高值 → 网络中断风暴
%soft       软中断处理时间占比             高值 → 网络包处理瓶颈
%guest      运行虚拟机的 CPU 时间          仅宿主机有意义
%gnice      虚拟机 nice 时间              仅宿主机有意义
```

**-u vs -u ALL 的区别：**
- `-u`：将 irq + soft 合并到 %system 中
- `-u ALL`：将 irq 和 soft 单独列出

### 2.3 sar -P ALL — 各 CPU 核心

```bash
sar -P ALL 1 3
```

**输出增加一列 CPU 编号（0, 1, 2, 3, ...），每行对应一个核心。**

**诊断场景：**
- 某个 CPU 核心 100% 而其他空闲 → CPU 亲和性绑定 / 单线程瓶颈
- 所有 CPU 均匀 100% → 计算密集型并行负载
- 某个 CPU %iowait 高 → I/O 中断绑定到该 CPU

---

## 3. 内存相关

### 3.1 sar -r — 内存使用

```bash
sar -r 1 3
```

```
列名         单位    含义                              诊断要点
───────────  ──────  ────────────────────────────────  ──────────────────
kbmemfree    KB      完全空闲的物理内存                  低不一定有问题
kbavail      KB      ★ 可用内存（含可回收的 cache）     ★ 最重要的指标
kbmemused    KB      已使用内存（含 buffers/cache）     配合 %memused 看
%memused     %       已使用占比                         > 90% 需关注
kbbuffers    KB      内核块设备 buffer                  反映 I/O 缓冲量
kbcached     KB      页面缓存（Page Cache）             大值正常，可回收
kbcommit     KB      已承诺分配（Committed_AS）         超过物理内存 → OOM 风险
%commit      %       承诺分配占 (内存+交换) 的比例       > 100% → 超承诺
kbactive     KB      最近活跃的页面                     不可轻易回收
kbinact      KB      最近不活跃的页面                   可被回收
kbdirty      KB      ★ 脏页（待写回磁盘）               大值 → 写压力大
```

**关键认知：**
- `kbavail` ≠ `kbmemfree`。avail 包含可回收的 cache，更接近真实可用量
- `%memused` 高但 `kbavail` 充足 → 正常（cache 占了很多内存但可回收）
- `kbdirty` 持续增长 → 可能有大量写入，检查 I/O

### 3.2 sar -r ALL — 内存全字段

```bash
sar -r ALL 1 3
```

**额外字段：**

```
列名         单位    含义                              说明
───────────  ──────  ────────────────────────────────  ──────────────────
kbanonpg     KB      匿名页面（进程堆/栈）              不可回收
kbslab       KB      内核 slab 缓存                     dentry/inode cache
kbkstack     KB      内核栈空间                         每线程 ~8KB
kbpgtbl      KB      页表占用内存                       大内存机器更显著
kbvmused     KB      vmalloc 已用空间                   内核动态分配
```

### 3.3 sar -B — 内存分页（Paging）

```bash
sar -B 1 3
```

```
列名       含义                                  正常范围    ★ 异常信号
─────────  ────────────────────────────────────  ──────────  ──────────────────
pgpgin/s   从磁盘读入内存的页面数 (KB/s)          视负载     持续高值 → 大量读 I/O
pgpgout/s  写出到磁盘的页面数 (KB/s)              视负载     持续高值 → 大量写 I/O
fault/s    ★ 页面错误总数 (含 minor)              有进程就>0  极高 → 内存映射密集
majflt/s   ★★ 主缺页错误 (需磁盘 I/O)           ≈ 0       > 0 → ★ 严重！内存不足
pgfree/s   释放的页面数                           正常       —
pgscank/s  kswapd 扫描的页面数                    0          > 0 → 内存压力
pgscand/s  直接回收扫描的页面数                    0          > 0 → ★ 严重内存压力
pgsteal/s  成功回收的页面数                       0          与 scan 比 → 回收效率
pgprom/s   页面从 lowmem 提升到 highmem           0          32 位系统
pgdem/s    页面从 highmem 降级到 lowmem           0          32 位系统
```

**★ 最重要的指标：`majflt/s`**
- 值为 0 → 正常
- 值 > 0 → 进程需要等待磁盘 I/O 来获取内存页面 → 性能严重下降
- 常见原因：内存不足导致频繁换页，或内存映射文件访问

### 3.4 sar -W — 交换活动（Swap）

```bash
sar -W 1 3
```

```
列名        含义                    正常范围    异常信号
──────────  ──────────────────────  ──────────  ──────────────────
pswpin/s    每秒换入的页面数         0          > 0 → ★ 内存不足，在用交换
pswpout/s   每秒换出的页面数         0          > 0 → ★ 内存不足，页面被换出
```

**任何非零值都是警告信号** — 说明物理内存不够用，内核在用交换空间。

### 3.5 sar -S — 交换空间使用率

```bash
sar -S 1 3
```

```
列名         含义                      正常范围
───────────  ────────────────────────  ──────────────────
kbswpfree    空闲交换空间 (KB)          > 0
kbswpused    已用交换空间 (KB)          ≈ 0
%swpused     交换空间使用率 (%)         < 10%
kbswpcad     缓存的交换页面 (KB)        —
%swpcad      缓存占已用交换的比例 (%)   —
```

### 3.6 sar -H — Huge Pages

```bash
sar -H 1 3
```

```
列名         含义                      说明
───────────  ────────────────────────  ──────────────────
kbhugfree    空闲 Huge Pages (KB)      预分配的大页面
kbhugused    已用 Huge Pages (KB)      数据库/DPDK 等使用
%hugused     Huge Pages 使用率 (%)     —
kbhugrsvd    已保留未使用 (KB)         —
kbhugsurp    超额分配的 (KB)           超出默认池的部分
```

---

## 4. I/O 相关

### 4.1 sar -b — 缓冲区 I/O

```bash
sar -b 1 3
```

```
列名       含义                              单位        说明
─────────  ────────────────────────────────  ──────────  ──────────────────
tps        ★ 每秒 I/O 传输次数              次/秒       合并后的传输数
rtps       每秒读传输次数                    次/秒       —
wtps       每秒写传输次数                    次/秒       —
dtps       每秒 discard 传输次数             次/秒       TRIM
bread/s    ★ 每秒读取的块数                块/秒       1 块 = 512 字节
bwrtn/s    ★ 每秒写入的块数                块/秒       1 块 = 512 字节
bdscd/s    每秒 discard 的块数              块/秒       —
```

**注意：`bread/s` 和 `bwrtn/s` 的单位是 512 字节块，不是 KB！**
- bread/s = 100 → 50 KB/s 的读
- 换算：KB/s = bread/s × 0.5

### 4.2 sar -d — 块设备 I/O

```bash
sar -d 1 3
```

**显示每个块设备的 I/O 统计。** 设备名使用内核内部编号（如 dev8-0）。

### 4.3 sar -d -p — 块设备 I/O（友好设备名）

```bash
sar -d -p 1 3
```

```
列名       含义                                    单位      正常范围     ★ 异常信号
─────────  ───────────────────────────────────────  ────────  ──────────  ──────────────────
tps        ★ 每秒传输次数 (IOPS)                   次/秒     视设备      接近设备极限 → 饱和
rkB/s      每秒读取 KB                             KB/s      视负载      —
wkB/s      每秒写入 KB                             KB/s      视负载      —
dkB/s      每秒 discard KB                         KB/s      通常 0      —
areq-sz    ★ 平均请求大小                          KB        视应用      小 = 随机，大 = 顺序
aqu-sz     ★ 平均队列深度                          —         < 2         > 4 → 队列积压
await      ★ 平均等待时间 (队列+服务)               ms        < 10ms      > 20ms → 延迟高
%util      ★★ 设备利用率                           %         < 80%       > 90% → ★ 磁盘饱和
DEV        设备名                                  —         —           —
```

**★ 诊断三板斧：**
1. `%util > 90%` → 磁盘饱和
2. `await > 20ms` → 延迟高（可能饱和或设备慢）
3. `aqu-sz > 4` → 队列积压严重

**与 iostat 对应关系：**

| sar -d -p | iostat -x | 含义 |
|-----------|-----------|------|
| tps | r/s + w/s | IOPS |
| rkB/s | rkB/s | 读速率 |
| wkB/s | wkB/s | 写速率 |
| areq-sz | rareq-sz / wareq-sz | 请求大小 |
| aqu-sz | aqu-sz | 队列深度 |
| await | r_await / w_await | 延迟（合并 vs 分离） |
| %util | %util | 利用率 |

### 4.4 sar -F — 文件系统统计

```bash
sar -F 1 3
sar -F MOUNT 1 3     # 按挂载点显示
```

```
列名       含义                              说明
─────────  ────────────────────────────────  ──────────────────
MBfsfree   文件系统空闲空间 (MB)             —
MBfsused   文件系统已用空间 (MB)             —
%fsused    文件系统使用率 (%)                > 90% 需清理
%ufsused   用户空间使用率 (%)                排除保留块
Ifree      空闲 inode 数                     —
Iused      已用 inode 数                     —
%Iused     inode 使用率 (%)                  > 90% 无法创建新文件
```

---

## 5. 网络相关

### 5.1 sar -n DEV — 网络接口流量

```bash
sar -n DEV 1 3
```

```
列名       含义                              单位      诊断要点
─────────  ────────────────────────────────  ────────  ──────────────────
IFACE      网络接口名                         —         —
rxpck/s    ★ 每秒接收包数                    pps        配合 rxkB/s 看平均包大小
txpck/s    ★ 每秒发送包数                    pps        —
rxkB/s     ★ 每秒接收 KB                     KB/s       接近带宽上限 → 瓶颈
txkB/s     ★ 每秒发送 KB                     KB/s       —
rxcmp/s    每秒接收的压缩包                  pps        通常 0
txcmp/s    每秒发送的压缩包                  pps        通常 0
rxmcst/s   每秒接收的多播包                  pps        高值 → 多播风暴
%ifutil    ★ 接口利用率                      %          接近 100% → 带宽饱和
```

**诊断场景：**
- `rxkB/s` 高 + `rxpck/s` 低 → 大包传输（如文件传输）
- `rxkB/s` 低 + `rxpck/s` 高 → 小包密集（如 DNS 查询、微服务调用）
- `%ifutil` 接近 100% → 带宽瓶颈，考虑升级网卡

### 5.2 sar -n EDEV — 网络错误

```bash
sar -n EDEV 1 3
```

```
列名       含义                              正常值     异常信号
─────────  ────────────────────────────────  ─────────  ──────────────────
rxerr/s    接收错误数                         0          > 0 → 网卡/链路故障
txerr/s    发送错误数                         0          > 0 → 网卡/链路故障
coll/s     冲突数 (半双工)                    0          > 0 → 半双工链路问题
rxdrop/s   接收丢弃数                         0          > 0 → ★ 内核 ring buffer 满
txdrop/s   发送丢弃数                         0          > 0 → 发送队列满
txcarr/s   载波错误                           0          > 0 → 物理链路问题
rxfram/s   帧对齐错误                        0          > 0 → 物理链路问题
rxfifo/s   接收 FIFO 溢出                    0          > 0 → 内核来不及处理
txfifo/s   发送 FIFO 溢出                    0          > 0 → —
```

**★ 任何非零值都需要关注**，特别是 rxdrop/s（包被丢弃 → 重传 → 延迟）。

### 5.3 sar -n SOCK — 套接字统计

```bash
sar -n SOCK 1 3
```

```
列名       含义                              说明
─────────  ────────────────────────────────  ──────────────────
totsck     当前使用的套接字总数               —
tcpsck     TCP 套接字数                       高值 → 连接数多
udpsck     UDP 套接字数                       —
rawsck     原始套接字数                       通常 0
ip-frag    IP 分片数                          高值 → MTU 问题
tcp-tw     TIME_WAIT 状态的 TCP 连接数         ★ 高值 → 短连接太多
```

**★ tcp-tw 高值诊断：**
- 大量 TIME_WAIT → 短连接频繁创建/关闭
- 解决方案：启用 `tcp_tw_reuse`，使用连接池

### 5.4 sar -n TCP — TCP 活动

```bash
sar -n TCP 1 3
```

```
列名       含义                              说明
─────────  ────────────────────────────────  ──────────────────
active/s   每秒主动建立的 TCP 连接数          客户端发起的连接
passive/s  每秒被动接受的 TCP 连接数          服务器接收的连接
iseg/s     每秒接收的 TCP 段数                入站流量
oseg/s     每秒发送的 TCP 段数                出站流量
```

### 5.5 sar -n ETCP — TCP 扩展错误

```bash
sar -n ETCP 1 3
```

```
列名         含义                              正常值     异常信号
───────────  ────────────────────────────────  ─────────  ──────────────────
atmptf/s     每秒失败的连接尝试                 0          > 0 → 连接失败
retrans/s    ★ 每秒重传段数                    < 1%       高值 → 网络质量差
estres/s     每秒连接重置数                    0          高值 → 服务端主动断开
retrseg/s    每秒重传段数 (旧版字段)           —          —
```

**★ retrans/s 是关键指标：**
- retrans/s ÷ oseg/s > 1% → 网络丢包严重
- 常见原因：网络拥塞、链路故障、中间设备丢包

### 5.6 sar -n SOCK6 / TCP6 / EDEV6 — IPv6 版本

```bash
sar -n SOCK6 1 3     # IPv6 套接字
sar -n TCP6 1 3      # IPv6 TCP 活动
sar -n EDEV6 1 3     # IPv6 网络错误
```

---

## 6. 进程与队列相关

### 6.1 sar -q — 运行队列与负载

```bash
sar -q 1 3
```

```
列名       含义                                    正常范围           ★ 异常信号
─────────  ───────────────────────────────────────  ────────────────  ──────────────────
runq-sz    ★ 运行队列长度（可运行 + 等待 I/O 进程） < CPU 核数        > CPU 核数 → 排队
plist-sz   进程/线程总数                            视系统            持续增长 → 进程泄漏
ldavg-1    ★ 1 分钟负载均值                        < CPU 核数        > CPU 核数 → 过载
ldavg-5    5 分钟负载均值                           < CPU 核数        —
ldavg-15   15 分钟负载均值                          < CPU 核数        持续高 → 长期过载
blocked    ★ 阻塞等待 I/O 的进程数                 0~1              > 3 → I/O 瓶颈
```

**负载解读：**
```
4 核 CPU:
  ldavg-1 = 2.0  → 正常（50% 负载）
  ldavg-1 = 4.0  → 刚好满载
  ldavg-1 = 8.0  → ★ 严重过载（排队严重）
  ldavg-1 = 0.5  → 非常空闲

★ load average 包含 D 状态（不可中断睡眠，通常是等 I/O）的进程
  所以 ldavg 高 + %iowait 高 → I/O 瓶颈
  所以 ldavg 高 + %user 高   → CPU 计算瓶颈
```

### 6.2 sar -q ALL — 队列全字段

```bash
sar -q ALL 1 3
```

在某些 sysstat 版本中，`ALL` 会额外显示：
- `nr_running` — 当前可运行的进程数
- `nr_threads` — 线程总数

### 6.3 sar -w — 进程创建与上下文切换

```bash
sar -w 1 3
```

```
列名       含义                              说明
─────────  ────────────────────────────────  ──────────────────
proc/s     ★ 每秒创建的进程数                高值 → 频繁 fork
cswch/s    ★ 每秒上下文切换次数              高值 → 线程竞争密集
```

**诊断要点：**
- `cswch/s` 高 + `%system` 高 → 大量系统调用或锁竞争
- `proc/s` 高 → 可能 shell 脚本频繁 fork，或 cron 任务密集

---

## 7. 中断相关

### 7.1 sar -I SUM — 中断汇总

```bash
sar -I SUM 1 3
```

```
列名       含义                    说明
─────────  ──────────────────────  ──────────────────
INTR       中断类型 (sum = 汇总)   —
all        所有 CPU 的每秒中断数    系统总中断频率
```

### 7.2 sar -I XALL — 各中断线详情

```bash
sar -I XALL 1 3
```

**显示每条中断线在每个 CPU 上的中断次数。** 常见中断线：

```
中断号      来源                高值含义
─────────  ──────────────────  ──────────────────
0           定时器 (timer)       正常，每个 CPU 每秒 ~1000
1           键盘                 —
NMI         不可屏蔽中断         watchdog
LOC         本地定时器           正常
SPU         虚假中断             —
RES         重调度中断           CPU 间调度
CAL         函数调用中断         CPU 间 IPI
TLB         TLB 刷新             正常
```

---

## 8. 其他参数

### 8.1 sar -A — 所有统计

```bash
sar -A 1 3    # 输出所有维度的统计
```

**等价于：`sar -bBdFHqSruvw -I SUM -n ALL -P ALL`**

输出非常长，适合全面了解系统状态或生成每日报告。

### 8.2 sar -m — 硬件监控

```bash
sar -m CPU 1 3      # CPU 温度
sar -m FAN 1 3      # 风扇转速
sar -m ALL 1 3      # 所有传感器
```

**依赖 `/sys/class/hwmon/` 下的传感器数据。**

### 8.3 sar -x — 扩展统计

```bash
sar -x 1 3    # 显示扩展信息（如 blkio 等）
```

### 8.4 sar -y — TTY 统计

```bash
sar -y 1 3    # 终端设备统计（过时）
```

### 8.5 sar -v — 内核表统计

```bash
sar -v 1 3
```

```
列名           含义                    说明
─────────────  ──────────────────────  ──────────────────
dentunusd      未使用的 dentry 缓存数   高值 → 目录访问多但可回收
file-nr        打开的文件描述符数       接近 file-max → 需要调大
inode-nr       分配的 inode 数          —
pty-nr         伪终端数                —
```

---

## 9. 历史数据查询

### 9.1 读取当天的 sa 文件

```bash
# 默认读取 /var/log/sa/saXX (XX = 当天日期)
sar -u -f /var/log/sa/sa03

# 也可以直接用日期
sar -u -03              # 等价于 -f /var/log/sa/sa03
```

### 9.2 时间范围过滤

```bash
# 只看 14:00 到 16:00
sar -u -f /var/log/sa/sa15 -s 14:00:00 -e 16:00:00

# 只看下午 2 点之后
sar -u -f /var/log/sa/sa15 -s 14:00:00

# 只看下午 4 点之前
sar -u -f /var/log/sa/sa15 -e 16:00:00
```

### 9.3 间隔采样

```bash
# 每 5 分钟 (300 秒) 显示一个点
sar -u -f /var/log/sa/sa15 -i 300
```

### 9.4 查看文件头

```bash
# 用 sadf 查看 sa 文件头信息
sadf -H /var/log/sa/sa03
```

---

## 10. 输出控制

### 10.1 时间格式

```bash
# 12 小时制（默认）
sar -u 1 3

# 24 小时制
S_TIME_FORMAT=ISO sar -u 1 3

# 或在 /etc/profile 中设置
export S_TIME_FORMAT=ISO
```

### 10.2 人性化输出

```bash
# --human: 自动添加单位 (K/M/G)
sar -d -p --human 1 3

# --pretty: 美化格式
sar -u --pretty 1 3

# -h: 重复显示标题行
sar -u -h 1 100
```

### 10.3 精度控制

```bash
# --dec=2: 显示 2 位小数
sar -u --dec=2 1 3
```

### 10.4 设备过滤

```bash
# 只看特定磁盘
sar -d -p --dev=vda 1 3

# 只看特定网络接口
sar -n DEV --iface=eth0 1 3

# 只看特定中断线
sar -I XALL --int=9 1 3
```

---

## 11. 场景速查表

| 场景 | 命令 | 关注指标 |
|------|------|---------|
| **快速系统巡检** | `sar -A 1 5` | 全部 |
| **CPU 瓶颈排查** | `sar -u ALL -P ALL 1 10` | %user/%system/%iowait，per-CPU |
| **内存不足排查** | `sar -r -B -W -S 1 10` | kbavail, majflt/s, pswpout/s |
| **磁盘饱和排查** | `sar -d -p 1 10` | %util, await, aqu-sz |
| **网络带宽排查** | `sar -n DEV -n EDEV 1 10` | rxkB/s, txkB/s, %ifutil, rxdrop/s |
| **网络质量排查** | `sar -n TCP -n ETCP 1 10` | retrans/s, active/s, tcp-tw |
| **系统负载排查** | `sar -q -w 1 10` | runq-sz, ldavg-1, cswch/s |
| **历史回溯** | `sar -u -f /var/log/sa/saXX -s HH:MM` | %user, %iowait |
| **I/O 延迟排查** | `sar -d -p -B 1 10` | await, %util, pgpgout/s |
| **连接数排查** | `sar -n SOCK -n TCP 1 10` | tcpsck, tcp-tw, active/s |
| **中断风暴排查** | `sar -I XALL 1 10` | 各中断线频率 |
| **进程泄漏排查** | `sar -q -w -v 1 10` | plist-sz, proc/s, file-nr |
| **虚拟机偷时间** | `sar -u ALL 1 10` | %steal |
| **数据导出 CSV** | `sadf -d saXX -- -u > csv` | — |
| **生成 SVG 图表** | `sadf -g saXX -- -u > svg` | — |

---

## 附录：参数速查表

```
CPU:     -u [ALL]  -P { cpu_list | ALL }
内存:    -r [ALL]  -B  -W  -S  -H
I/O:     -b  -d  -p  -F [MOUNT]
网络:    -n { DEV | EDEV | SOCK | TCP | ETCP | SOCK6 | TCP6 | EDEV6 | ALL }
队列:    -q [ALL]
中断:    -I { SUM | ALL | XALL }
进程:    -w  -v
硬件:    -m { CPU | FAN | ALL }
综合:    -A (全部)
历史:    -f [file]  -s [time]  -e [time]  -i interval  -[0-9]+
输出:    --human  --pretty  --dec={0|1|2}  -h
过滤:    --dev=  --iface=  --int=  --fs=
```
