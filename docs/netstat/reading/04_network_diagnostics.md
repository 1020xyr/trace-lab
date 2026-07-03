# 网络诊断实战 — 从 netstat 发现异常到定位根因

> ======================================================================
> 本文件通过实际执行命令，演示完整的网络性能诊断流程。
> 01 文件已讲解 TCP 状态机，本文件聚焦于**/proc/net 数据源解析**和**诊断实战**。
>
> 阅读要点：
>   1. /proc/net/tcp 的字段详解（netstat 的底层数据源）
>   2. /proc/net/snmp 协议统计（netstat -s 的数据源）
>   3. /proc/net/sockstat socket 全局统计
>   4. TIME_WAIT / CLOSE_WAIT / SYN_RECV 过多的完整排查流程
>   5. 实际执行 netstat 各命令并逐行注解
>
> 预计阅读时间：20 分钟
> ======================================================================

---

## 目录

- [一、/proc/net/tcp 字段详解](#一procnettcp-字段详解)
- [二、/proc/net/snmp 协议统计](#二procnetsnmp-协议统计)
- [三、/proc/net/sockstat 全局 socket 统计](#三procnetsockstat-全局-socket-统计)
- [四、netstat 命令实战与注解](#四netstat-命令实战与注解)
- [五、TCP 状态异常排查完整流程](#五tcp-状态异常排查完整流程)
- [六、网络诊断决策树](#六网络诊断决策树)
- [七、netstat vs ss 对比](#七netstat-vs-ss-对比)

---

## 一、/proc/net/tcp 字段详解

### ★ 实际输出

```bash
$ cat /proc/net/tcp | head -5
```

```
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:84E1 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 4480261 1 ffff888106e447c0 100 0 0 10 0
   1: 0100007F:84BB 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 360241 1 ffff888106e41480 100 0 0 10 0
   2: 00000000:1F40 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 138138 1 ffff888106c28000 100 0 0 10 0
   3: 908D1BAC:D876 F9C66027:01BB 01 00000000:00000000 02:0000115E 00000000     0        0 4855961 2 ffff8881807dc7c0 22 4 24 1175 -1
```

### ★ 字段逐一解读

```
列号 │ 列名          │ 含义                            │ 解读示例（第 3 行）
─────┼──────────────┼─────────────────────────────────┼─────────────────────────
  0  │ sl            │ 序号（slot number）             │ 3
  1  │ local_address │ ★ 本地 IP:端口（十六进制）      │ 908D1BAC:D876
     │              │   IP: 小端序，每 2 位一个字节    │   → AC1B8D90
     │              │   端口: 大端序                   │   → 172.27.141.144:55414
  2  │ rem_address   │ ★ 远端 IP:端口                  │ F9C66027:01BB
     │              │                                  │   → 39.96.198.249:443
  3  │ st            │ ★ TCP 状态（十六进制）          │ 01 = ESTABLISHED
  4  │ tx_queue      │ ★ 发送队列中的字节数            │ 00000000 = 0 字节待发
     │ :rx_queue     │ ★ 接收队列中的字节数            │ 00000000 = 0 字节待读
  5  │ tr            │ 定时器类型                      │ 02 = 重传定时器
     │ :tm->when     │   定时器到期时间（jiffies）     │ 0000115E
  6  │ retrnsmt      │ 重传次数                        │ 00000000 = 0 次重传
  7  │ uid           │ 创建 socket 的用户 ID           │ 0 = root
  8  │ timeout       │ 超时值                          │ 0
  9  │ inode         │ socket 的 inode 号              │ 4855961
```

### ★ IP 地址解码方法

```
十六进制 IP（小端序）→ 实际 IP：
  908D1BAC
  → 拆分字节：90 8D 1B AC
  → 反转（小端→大端）：AC 1B 8D 90
  → 转十进制：172.27.141.144

十六进制端口（大端序）→ 实际端口：
  D876
  → 转十进制：0xD876 = 55414

★ 快速解码脚本：
$ printf "%d.%d.%d.%d:%d\n" 0xAC 0x1B 0x8D 0x90 0xD876
172.27.141.144:55414
```

### ★ TCP 状态码对照表

```
十六进制 │ 十进制 │ TCP 状态        │ netstat 显示
─────────┼────────┼────────────────┼──────────────
01       │ 1      │ ESTABLISHED    │ ESTABLISHED
02       │ 2      │ SYN_SENT       │ SYN_SENT
03       │ 3      │ SYN_RECV       │ SYN_RECV
04       │ 4      │ FIN_WAIT1      │ FIN_WAIT1
05       │ 5      │ FIN_WAIT2      │ FIN_WAIT2
06       │ 6      │ TIME_WAIT      │ TIME_WAIT
07       │ 7      │ CLOSE          │ (不显示)
08       │ 8      │ CLOSE_WAIT     │ CLOSE_WAIT
09       │ 9      │ LAST_ACK       │ LAST_ACK
0A       │ 10     │ LISTEN         │ LISTEN
0B       │ 11     │ CLOSING        │ CLOSING
```

### ★ 实际数据中的状态分布

```bash
# 统计 /proc/net/tcp 中的状态分布
$ awk 'NR>1 {print $4}' /proc/net/tcp | sort | uniq -c | sort -rn
```

```
     13 0A    ← LISTEN（13 个监听端口）
     18 01    ← ESTABLISHED（18 个已建立连接）
      1 06    ← TIME_WAIT（1 个 TIME_WAIT）
```

**与 netstat 对照：**

```bash
$ netstat -an | awk '/^tcp/ {print $6}' | sort | uniq -c | sort -rn
     18 ESTABLISHED    ← 与 /proc/net/tcp 的 01 计数一致 ✓
     15 LISTEN         ← 含 IPv6（tcp6 有 2 个额外的 LISTEN）
      1 TIME_WAIT      ← 与 /proc/net/tcp 的 06 计数一致 ✓
```

### ★ tx_queue / rx_queue 的诊断意义

```
┌────────────────────────────────────────────────────────────────┐
│  ESTABLISHED 状态下的 tx_queue 和 rx_queue                       │
│                                                                  │
│  tx_queue > 0（发送队列有数据）：                                │
│    → 应用已 write() 但内核还没发出去                            │
│    → 可能原因：网络拥塞、对端接收窗口为 0                        │
│    → 持续增大 = 对端不消费数据                                   │
│                                                                  │
│  rx_queue > 0（接收队列有数据）：                                │
│    → 内核已收到数据但应用还没 read()                             │
│    → 可能原因：应用处理太慢、应用已挂起                          │
│    → ★ 持续增长 = 应用层瓶颈（消费不过来）                       │
│                                                                  │
│  LISTEN 状态下的含义不同：                                       │
│    rx_queue = 已完成三次握手但未被 accept() 的连接数              │
│    tx_queue = listen() 时设置的 backlog 上限                     │
│    ★ rx_queue 接近 tx_queue = backlog 快满了                     │
└────────────────────────────────────────────────────────────────┘
```

---

## 二、/proc/net/snmp 协议统计

### ★ 实际输出

```bash
$ cat /proc/net/snmp
```

```
Ip: Forwarding DefaultTTL InReceives InHdrErrors InAddrErrors ...
Ip: 2 64 4290046 0 4 0 0 0 4290042 3765177 0 0 0 0 0 0 2 0 4 3765179

Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors
Tcp: 1 200 120000 -1 56589 21551 62755 1984 18 4158779 4504843 19411 25 96095 4

Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors IgnoredMulti MemErrors
Udp: 34375 106 0 55289 0 0 0 1 0
```

### ★ TCP 统计字段详解

```
字段            │ 值       │ 含义                              │ 诊断意义
───────────────┼─────────┼───────────────────────────────────┼──────────────────────
RtoAlgorithm   │ 1       │ RTO 算法（1=固定，非 RFC 标准）    │ —
RtoMin         │ 200     │ 最小 RTO（ms）                     │ TCP 重传最小间隔
RtoMax         │ 120000  │ 最大 RTO（ms）= 2 分钟             │ 重传超时上限
MaxConn        │ -1      │ 最大连接数（-1=无限制）            │ —
★ ActiveOpens  │ 56589   │ 主动发起的连接数（客户端行为）     │ 出站连接总数
★ PassiveOpens │ 21551   │ 被动接受的连接数（服务端行为）     │ 入站连接总数
★ AttemptFails │ 62755   │ ★ 连接尝试失败数                   │ 高值 = 网络/目标问题
EstabResets    │ 1984    │ 已建立连接被 RST 重置的次数        │ 异常断开数
CurrEstab      │ 18      │ ★ 当前已建立的连接数               │ 实时快照
★ InSegs       │ 4158779 │ 收到的 TCP 段数                    │ 入站流量
★ OutSegs      │ 4504843 │ 发送的 TCP 段数                    │ 出站流量
★ RetransSegs  │ 19411   │ ★ 重传的 TCP 段数                  │ 高值 = 网络丢包
InErrs         │ 25      │ 接收错误的段数                     │ 校验和/格式错误
OutRsts        │ 96095   │ 发送的 RST 段数                    │ 拒绝连接数
InCsumErrors   │ 4       │ 校验和错误数                       │ 数据损坏
```

### ★ 关键诊断指标

```
重传率 = RetransSegs / OutSegs × 100%
       = 19411 / 4504843 × 100%
       = 0.43%

★ 重传率参考：
  < 1%   → 网络质量好（当前系统）
  1~5%   → 网络有轻微丢包
  > 5%   → 网络严重丢包，需排查

连接失败率 = AttemptFails / (ActiveOpens + PassiveOpens) × 100%
           = 62755 / (56589 + 21551) × 100%
           = 80.3%

★ 这个值异常高！原因分析：
  AttemptFails 包含了所有连接尝试失败（包括 SYN 超时、RST 拒绝等）
  在云服务器环境中，可能有大量健康检查探测被拒绝
  或者有自动化脚本频繁尝试连接不可达的目标
```

### ★ netstat -s 如何读取这些数据

```bash
$ netstat -s | head -30
```

```
Ip:
    Forwarding: 2
    4291024 total packets received
    4 with invalid addresses
    0 forwarded
    0 incoming packets discarded
    4291020 incoming packets delivered
    3766077 requests sent out
...
Tcp:
    56589 active connection openings
    21551 passive connection openings
    62755 failed connection attempts
    1984 connection resets received
    17 connections established
    4159757 segments received
    4506034 segments sent out
    19411 segments retransmitted
    25 bad segments received
    96095 resets sent
```

**★ netstat -s 就是 /proc/net/snmp 的人类可读版本！** 两者数据完全一致。

---

## 三、/proc/net/sockstat 全局 socket 统计

### ★ 实际输出

```bash
$ cat /proc/net/sockstat
sockets: used 266
TCP: inuse 30 orphan 0 tw 1 alloc 32 mem 0
UDP: inuse 5 mem 0
UDPLITE: inuse 0
RAW: inuse 0
FRAG: inuse 0 memory 0
```

### ★ 字段详解

```
字段       │ 值  │ 含义                                    │ 诊断意义
──────────┼─────┼─────────────────────────────────────────┼─────────────────
used       │ 266 │ ★ 系统当前使用的 socket 总数            │ 接近限制需警惕
inuse      │ 30  │ ★ 当前活跃的 TCP socket 数              │ 包含所有状态
orphan     │ 0   │ ★ 孤儿连接（不属于任何进程）            │ > 0 有连接泄漏
tw         │ 1   │ ★ TIME_WAIT 状态的 socket 数            │ 高值需关注
alloc      │ 32  │ 已分配的 TCP socket 数                  │ 通常 ≥ inuse
mem        │ 0   │ TCP socket 占用的内存页数               │ —
```

### ★ sockstat 与 netstat/ss 的对应

```bash
$ ss -s
Total: 265
TCP:   33 (estab 18, closed 0, orphaned 0, timewait 0)
```

```
ss -s 输出            │ sockstat 字段      │ 对应关系
──────────────────────┼───────────────────┼──────────
Total: 265            │ sockets: used 266 │ 近似（统计时机差异）
TCP: 33               │ TCP: inuse 30     │ 近似（含 IPv6 的 tcp6）
estab 18              │ —                 │ ss 自己统计
orphaned 0            │ orphan 0          │ ✓ 一致
timewait 0            │ tw 1              │ 统计时机差异
```

---

## 四、netstat 命令实战与注解

### 实验 1：查看所有连接状态分布

```bash
$ netstat -an | awk '/^tcp/ {print $6}' | sort | uniq -c | sort -rn
```

```
     18 ESTABLISHED    ← ★ 正常：活跃的数据传输连接
     15 LISTEN         ← 正常：监听端口的服务端
      1 TIME_WAIT      ← 正常：刚关闭的连接（60 秒后消失）
```

**诊断结论：** 系统健康，所有状态都在正常范围。

### 实验 2：查看 LISTEN 端口详情

```bash
$ netstat -tlnp
```

```
Proto Recv-Q Send-Q Local Address         Foreign Address        State       PID/Program name
tcp        0      0 127.0.0.1:34017       0.0.0.0:*              LISTEN      -
tcp        0      0 0.0.0.0:8000          0.0.0.0:*              LISTEN      -
tcp        0      0 0.0.0.0:5355          0.0.0.0:*              LISTEN      -
tcp        0      0 127.0.0.53:53         0.0.0.0:*              LISTEN      -
tcp        0      0 0.0.0.0:18081         0.0.0.0:*              LISTEN      -
tcp        0      0 0.0.0.0:18080         0.0.0.0:*              LISTEN      -
tcp        0      0 0.0.0.0:22            0.0.0.0:*              LISTEN      -
tcp        0      0 0.0.0.0:80            0.0.0.0:*              LISTEN      -
```

**逐行注解：**

```
端口  │ 绑定地址         │ 服务推测         │ 安全评估
──────┼─────────────────┼─────────────────┼────────────────────
22    │ 0.0.0.0          │ SSH             │ ⚠️ 监听所有接口
80    │ 0.0.0.0          │ HTTP/Nginx      │ ⚠️ 监听所有接口
53    │ 127.0.0.53       │ DNS (systemd)   │ ✓ 仅本地
5355  │ 0.0.0.0          │ LLMNR           │ ⚠️ 可关闭
8000  │ 0.0.0.0          │ 自定义 HTTP     │ ⚠️ 监听所有接口
18080 │ 0.0.0.0          │ 自定义服务      │ ⚠️ 监听所有接口
18081 │ 0.0.0.0          │ 自定义服务      │ ⚠️ 监听所有接口
34017 │ 127.0.0.1        │ 内部服务        │ ✓ 仅本地
```

### 实验 3：查看 ESTABLISHED 连接的详细信息

```bash
$ netstat -tnp | grep ESTABLISHED
```

```
Proto Recv-Q Send-Q Local Address               Foreign Address           State       PID/Program
tcp        0      0 127.0.0.1:53888             127.0.0.1:34017           ESTABLISHED -
tcp        0      0 172.27.141.144:55414        39.96.198.249:443         ESTABLISHED -
tcp        0    100 172.27.141.144:22           183.247.2.29:13462        ESTABLISHED -
tcp        0      0 172.27.141.144:54314        8.140.217.18:443          ESTABLISHED -
tcp        0     64 127.0.0.1:43113             127.0.0.1:37866           ESTABLISHED -
```

**关键发现：**

```
连接 1：127.0.0.1:53888 → 127.0.0.1:34017
  → 本地回环连接（进程间通信）
  → Recv-Q=0, Send-Q=0 → 正常

连接 2：172.27.141.144:55414 → 39.96.198.249:443
  → HTTPS 出站连接
  → Recv-Q=0, Send-Q=0 → 正常

连接 3：172.27.141.144:22 → 183.247.2.29:13462  ★
  → SSH 入站连接（有人连上了这台服务器）
  → Send-Q = 100 → ★ 有 100 字节在发送队列中
  → 可能是终端输出正在发送（正常行为）

连接 4：127.0.0.1:43113 → 127.0.0.1:37866
  → Send-Q = 64 → 有 64 字节待发
  → 进程间通信，正常
```

### 实验 4：查看 TIME_WAIT 连接

```bash
$ netstat -tn | grep TIME_WAIT
```

```
tcp   0  0 172.27.141.144:41380  100.100.100.10:80  TIME_WAIT
```

**分析：**

```
100.100.100.10:80 → 阿里云内部元数据服务
  这是一个短连接（HTTP 请求元数据后关闭）
  TIME_WAIT 状态 = 主动关闭方（本机先调 close()）
  60 秒后自动消失 → 正常行为
```

### 实验 5：查看协议统计

```bash
$ netstat -s | grep -A5 "^Tcp:"
```

```
Tcp:
    56589 active connection openings
    21551 passive connection openings
    62755 failed connection attempts
    1984 connection resets received
    17 connections established
    4159757 segments received
    4506034 segments sent out
    19411 segments retransmitted
```

### 实验 6：查看网络接口统计

```bash
$ netstat -i
```

```
Iface   MTU    RX-OK RX-ERR RX-DRP RX-OVR    TX-OK TX-ERR TX-DRP TX-OVR Flg
eth0   1500        0      0      0 0             0      0      0      0 BMRU
lo    65536        0      0      0 0             0      0      0      0 LRU
```

**★ 关键发现**：eth0 的 RX-OK 和 TX-OK 都是 0！这是因为 netstat -i 显示的是自上次查询以来的增量（对于某些实现），或接口统计数据可能通过其他方式获取。实际应使用 `/proc/net/dev` 或 `ip -s link`。

### 实验 7：查看路由表

```bash
$ netstat -r
```

```
Kernel IP routing table
Destination     Gateway         Genmask         Flags   MSS Window  irtt Iface
default         _gateway        0.0.0.0         UG        0 0          0 eth0
172.27.128.0    0.0.0.0         255.255.240.0   U         0 0          0 eth0
```

**解读：**

```
路由 1：default → _gateway via eth0
  → 默认路由，所有非本地流量走网关
  → Flags: U(Up) + G(Gateway) = 默认网关路由

路由 2：172.27.128.0/20 → 直连 eth0
  → 本地子网，不需要经过网关
  → Flags: U(Up) = 直连路由
  → Genmask = 255.255.240.0 = /20 子网掩码
```

---

## 五、TCP 状态异常排查完整流程

### ★ 场景 1：TIME_WAIT 过多

```
┌────────────────────────────────────────────────────────────────┐
│  发现：netstat 显示大量 TIME_WAIT                                │
│                                                                  │
│  步骤 1：确认数量                                                │
│  $ ss -tn state time-wait | wc -l                                │
│  → 如果 > 数千，需要关注                                        │
│                                                                  │
│  步骤 2：查看 TIME_WAIT 连接分布                                 │
│  $ netstat -tn | grep TIME_WAIT | awk '{print $5}' | cut -d: -f1 \│
│    | sort | uniq -c | sort -rn | head                            │
│  → 看远端 IP 分布，判断是哪些服务产生的                          │
│                                                                  │
│  步骤 3：检查内核参数                                            │
│  $ cat /proc/sys/net/ipv4/tcp_max_tw_buckets                     │
│  → 262144（默认上限）                                            │
│  $ cat /proc/sys/net/ipv4/tcp_tw_reuse                           │
│  → 2（安全复用已启用）                                           │
│  $ cat /proc/sys/net/ipv4/tcp_fin_timeout                        │
│  → 60（默认 60 秒）                                              │
│                                                                  │
│  步骤 4：优化措施                                                │
│  - 应用层改用长连接（HTTP Keep-Alive、连接池）                   │
│  - 增大 tcp_max_tw_buckets（治标）                               │
│  - ★ 不要关闭 tcp_tw_reuse（默认值 2 已是最优）                 │
│  - ★ 绝对不要使用 tcp_tw_recycle（已从 Linux 4.12 移除）        │
└────────────────────────────────────────────────────────────────┘
```

### ★ 场景 2：CLOSE_WAIT 过多

```
┌────────────────────────────────────────────────────────────────┐
│  发现：netstat 显示大量 CLOSE_WAIT                                │
│                                                                  │
│  ★ CLOSE_WAIT = 应用层 Bug（没有调用 close()）                   │
│                                                                  │
│  步骤 1：确认数量和所属进程                                      │
│  $ ss -tnp state close-wait                                      │
│  → 看 PID/Program name，找到有问题的进程                         │
│                                                                  │
│  步骤 2：查看应用日志                                            │
│  → 应用可能因为异常退出、死锁、异常处理中                        │
│     而忘记关闭 socket                                            │
│                                                                  │
│  步骤 3：用 strace 跟踪                                          │
│  $ strace -p <PID> -e trace=close                                │
│  → 看应用是否在调用 close()                                      │
│                                                                  │
│  步骤 4：修复代码                                                │
│  → 确保所有 socket 在使用后都调用 close()                        │
│  → 使用 try-finally 或 defer 确保异常路径也能关闭                │
└────────────────────────────────────────────────────────────────┘
```

### ★ 场景 3：SYN_RECV 过多

```
┌────────────────────────────────────────────────────────────────┐
│  发现：netstat 显示大量 SYN_RECV                                  │
│                                                                  │
│  ★ 可能是 SYN Flood 攻击！                                       │
│                                                                  │
│  步骤 1：确认数量                                                │
│  $ ss -tn state syn-recv | wc -l                                 │
│  → 个位数 = 正常（三次握手进行中）                               │
│  → > 100 = 可能是攻击                                            │
│                                                                  │
│  步骤 2：检查 SYN Cookie 是否启用                                │
│  $ cat /proc/sys/net/ipv4/tcp_syncookies                         │
│  → 1 = 已启用（能抵御 SYN Flood）                                │
│                                                                  │
│  步骤 3：检查 SYN backlog 大小                                   │
│  $ cat /proc/sys/net/ipv4/tcp_max_syn_backlog                    │
│  → 1024（当前系统）                                              │
│  $ cat /proc/sys/net/core/somaxconn                              │
│  → 4096（listen backlog 上限）                                   │
│                                                                  │
│  步骤 4：检查 /proc/net/snmp 中的 AttemptFails                   │
│  → 如果 AttemptFails 快速增长，说明有大量连接失败                │
│                                                                  │
│  步骤 5：防御措施                                                │
│  - 启用 tcp_syncookies（如果未启用）                             │
│  - 增大 tcp_max_syn_backlog                                      │
│  - 使用 iptables 限速 SYN 包                                    │
│  - 使用 CDN / WAF 清洗流量                                      │
└────────────────────────────────────────────────────────────────┘
```

---

## 六、网络诊断决策树

### ★ 完整决策流程

```
netstat 发现异常
    │
    ├── TIME_WAIT 过多
    │   │
    │   ├── 集中在某端口 → 该服务是短连接，改用长连接
    │   ├── 分散在多端口 → 系统级短连接太多
    │   └── 接近 tcp_max_tw_buckets → 调大上限 + 优化应用
    │
    ├── CLOSE_WAIT 过多
    │   │
    │   └── ★ 应用层 Bug → 定位进程 → 检查代码 → 修复 close()
    │
    ├── SYN_RECV 过多
    │   │
    │   ├── 来自少数 IP → 可能是恶意扫描/攻击
    │   └── 来自多数 IP → 可能是 SYN Flood
    │       └── 启用 syncookies + 限速
    │
    ├── ESTABLISHED 的 Send-Q 持续增长
    │   │
    │   ├── 对端不消费数据 → 对端应用挂起或处理太慢
    │   └── 网络拥塞 → 检查网络链路
    │
    ├── ESTABLISHED 的 Recv-Q 持续增长
    │   │
    │   └── 本端应用读取太慢 → 优化应用处理速度
    │
    └── 重传率高（RetransSegs / OutSegs > 1%）
        │
        ├── 所有连接都高 → 网络基础设施问题
        └── 特定连接高 → 该路径的网络问题
```

### ★ 内核参数速查表

```
参数                              │ 默认值  │ 含义                  │ 调优建议
─────────────────────────────────┼────────┼──────────────────────┼──────────────────
tcp_tw_reuse                     │ 2      │ TIME_WAIT 复用        │ 保持默认
tcp_fin_timeout                  │ 60     │ TIME_WAIT 超时(秒)    │ 可调到 15~30
tcp_max_tw_buckets               │ 262144 │ TIME_WAIT 上限        │ 高并发可调大
tcp_max_syn_backlog              │ 1024   │ SYN 队列上限          │ 高并发可调到 4096+
tcp_syncookies                   │ 1      │ SYN Cookie 开关       │ ★ 必须保持 1
somaxconn                        │ 4096   │ listen backlog 上限   │ 高并发可调大
tcp_keepalive_time               │ 7200   │ keepalive 间隔(秒)    │ 可降到 600
tcp_keepalive_intvl              │ 75     │ keepalive 探测间隔    │ 可降到 30
tcp_keepalive_probes             │ 9      │ keepalive 探测次数    │ 可降到 3
```

---

## 七、netstat vs ss 对比

### ★ 功能对比

```
功能                  │ netstat              │ ss
─────────────────────┼─────────────────────┼──────────────────────
数据源               │ /proc/net/tcp 等     │ netlink（直接查内核）
速度                 │ 慢（逐文件读取解析）  │ ★ 快（二进制协议）
TCP 状态过滤         │ 需 awk/grep          │ ★ 内置 filter 语法
显示 socket 内存     │ ✗                    │ ★ -m 选项
显示 cgroup          │ ✗                    │ ★ 支持
显示 timer 信息      │ ✗                    │ ★ -o 选项
进程信息             │ -p 选项              │ -p 选项
输出格式             │ 传统表格             │ 更紧凑
维护状态             │ ★ 已废弃(deprecated) │ ★ 推荐工具
```

### ★ ss 的 filter 语法（netstat 不支持）

```bash
# 只看 ESTABLISHED 连接
$ ss -tn state established

# 只看连接到某端口的连接
$ ss -tn dst :443

# 只看某进程的连接
$ ss -tnp | grep pid=1234

# 查看所有 TIME_WAIT
$ ss -tn state time-wait

# 查看 socket 内存使用
$ ss -tm
```

### ★ 推荐诊断流程

```
1. 快速总览：ss -s
2. 状态分布：ss -tna | awk 'NR>1{print $1}' | sort | uniq -c | sort -rn
3. 详细查看：ss -tnp state <state>
4. 协议统计：netstat -s（ss 没有等价功能）
5. 接口统计：ip -s link（netstat -i 已过时）
6. 路由表：ip route（netstat -r 已过时）
```

---

## 总结

### ★ 核心映射关系

```
netstat 功能          /proc 数据源                说明
─────────────────    ─────────────────────       ─────────────────────
连接列表             /proc/net/tcp + tcp6        每行一个 socket
状态分布             解析 st 字段                十六进制状态码
协议统计(-s)         /proc/net/snmp              各协议收发计数
socket 总数          /proc/net/sockstat           inuse/orphan/tw
接口统计(-i)         /proc/net/dev               网卡收发计数
路由表(-r)           /proc/net/route             内核路由表
```

### ★ 诊断优先级

```
优先级 │ 异常状态       │ 严重程度 │ 处理方式
──────┼───────────────┼─────────┼──────────────────────────
  1   │ CLOSE_WAIT 多  │ 🔴 高   │ 应用 Bug，修复代码
  2   │ SYN_RECV 多    │ 🔴 高   │ 可能是攻击，启用 syncookies
  3   │ TIME_WAIT 多   │ ⚠️ 中   │ 正常行为，改用长连接优化
  4   │ 重传率高       │ ⚠️ 中   │ 网络丢包，排查链路
  5   │ Send-Q 堆积    │ ⚠️ 中   │ 对端消费慢或网络拥塞
  6   │ Recv-Q 堆积    │ ⚠️ 中   │ 本端应用处理慢
```
