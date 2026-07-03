# netstat + ss 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐字段解读。
> 环境：CentOS / Linux 5.x 内核，ECS 云服务器。

---

## 场景 1：ss -tlnp — TCP 监听端口

### 命令

```bash
ss -tlnp
```

### 输出

```
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port Process
LISTEN 0      128        127.0.0.1:34017      0.0.0.0:*    users:(("code-fcf604774b",pid=433236,fd=9))
LISTEN 0      128        127.0.0.1:33979      0.0.0.0:*    users:(("code-6928394f91",pid=84775,fd=12))
LISTEN 0      2048         0.0.0.0:8000       0.0.0.0:*    users:(("python3",pid=56771,fd=13))
LISTEN 0      511        127.0.0.1:13104      0.0.0.0:*    users:(("MainThread",pid=433385,fd=54))
LISTEN 0      4096         0.0.0.0:5355       0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=11))
LISTEN 0      4096   127.0.0.53%lo:53         0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=17))
LISTEN 0      128          0.0.0.0:18081      0.0.0.0:*
LISTEN 0      128          0.0.0.0:18080      0.0.0.0:*
LISTEN 0      128          0.0.0.0:22         0.0.0.0:*    users:(("sshd",pid=979,fd=3))
LISTEN 0      511          0.0.0.0:80         0.0.0.0:*    users:(("nginx",pid=54598,fd=6),("nginx",pid=54597,fd=6),("nginx",pid=54596,fd=6),("nginx",pid=54595,fd=6),("nginx",pid=54594,fd=6))
LISTEN 0      4096            [::]:5355          [::]:*    users:(("systemd-resolve",pid=646,fd=13))
LISTEN 0      128             [::]:22            [::]:*    users:(("sshd",pid=979,fd=4))
```

### 逐字段注解

```
State   → 连接状态（LISTEN = 监听中）
Recv-Q  → ★ LISTEN 状态下 = 当前 backlog 队列中等待 accept() 的连接数
           正常应为 0，> 0 说明 accept() 来不及处理
Send-Q  → ★ LISTEN 状态下 = listen() 调用时指定的 backlog 上限
           常见值：128、511、2048、4096
Local Address:Port → 本地监听地址和端口
           0.0.0.0:80  = 监听所有 IPv4 地址的 80 端口
           127.0.0.1:80 = 只监听本地回环
           [::]:22      = 监听所有 IPv6 地址的 22 端口
           127.0.0.53%lo:53 = 绑定到 127.0.0.53（%lo 是设备名）
Peer Address:Port  → 远端地址（LISTEN 状态下永远是 * = 任意）
Process → ★ 进程信息（进程名, PID, 文件描述符号）
           多个进程 = 多进程共享同一 socket（如 nginx master + workers）
           空 = 无权限查看（非 root）或进程已退出
```

### 关键发现

```
★ nginx 监听 80 端口：5 个进程共享（1 master + 4 workers）
  users:(("nginx",pid=54598,fd=6),("nginx",pid=54597,fd=6),...)
  所有 worker 继承 master 的 fd=6

★ sshd 监听 22 端口：IPv4 和 IPv6 各一个 socket
  0.0.0.0:22  → IPv4
  [::]:22     → IPv6

★ 18080/18081 没有进程信息：
  Process 列为空 → 当前用户无权限读取进程信息

★ systemd-resolved 监听多个地址：
  127.0.0.53:53 → 本地 DNS 解析器
  127.0.0.54:53 → 第二个 DNS 监听地址
  0.0.0.0:5355  → LLMNR（Link-Local Multicast Name Resolution）
```

---

## 场景 2：ss -tnp — 已建立连接

### 命令

```bash
ss -tnp
```

### 输出

```
State  Recv-Q Send-Q  Local Address:Port     Peer Address:Port  Process
ESTAB  0      0           127.0.0.1:47004       127.0.0.1:13104 users:(("claude",pid=433969,fd=19))
ESTAB  0      0           127.0.0.1:53888       127.0.0.1:34017 users:(("sshd",pid=433217,fd=10))
ESTAB  0      327    172.27.141.144:42464 185.199.109.133:443   users:(("claude",pid=433969,fd=16))
ESTAB  0      0      172.27.141.144:40048   39.106.104.16:443   users:(("claude",pid=433969,fd=36))
ESTAB  0      0      172.27.141.144:22       183.247.2.29:13462 users:(("sshd",pid=433217,fd=4),("sshd",pid=433214,fd=4))
ESTAB  0      0      172.27.141.144:57858   100.100.30.25:80    users:(("AliYunDun",pid=1490,fd=9))
ESTAB  0      128872 172.27.141.144:45170   160.79.104.10:443   users:(("claude",pid=433969,fd=21))
```

### 逐字段注解

```
State  → ESTAB = ESTABLISHED（连接已建立，正在传输数据）
Recv-Q → ★ 已接收但应用层尚未 read() 的字节数
          正常应为 0（应用及时消费了数据）
          持续增长 = 应用读取太慢或已停止读取
Send-Q → ★ 已发送但尚未收到对方 ACK 的字节数
          正常应接近 0（网络通畅时很快确认）
          持续增长 = 网络拥塞、丢包、或对方接收窗口为 0
Local Address:Port  → 本地 IP 和端口
          127.0.0.1:xxx = 本地回环连接（两端都在同一台机器）
          172.27.141.144:xxx = 外网接口
Peer Address:Port   → 远端 IP 和端口
Process → 拥有此 socket 的进程
```

### 关键发现

```
★ Send-Q = 327（172.27.141.144:42464 → 185.199.109.133:443）
  327 字节已发送但未确认 → 可能有轻微网络延迟或丢包
  如果持续增长到数万，说明网络有问题

★ Send-Q = 128872（172.27.141.144:45170 → 160.79.104.10:443）
  ★ 约 126KB 数据堆积在发送缓冲区！
  可能原因：
  - 对端接收窗口满（对方处理慢）
  - 网络拥塞（丢包导致重传）
  - 对端已停止读取数据
  排查：ss -ti 查看 cwnd、rtt；检查 tcpdump

★ 本地回环连接（127.0.0.1 → 127.0.0.1）
  Recv-Q 和 Send-Q 都是 0 → 正常，本地通信延迟极低

★ sshd 连接显示两个进程
  users:(("sshd",pid=433217,fd=4),("sshd",pid=433214,fd=4))
  父进程 + 子进程共享同一 socket（fork 后继承 fd）

★ AliYunDun 连接到 100.100.30.25:80
  阿里云安骑士（安全监控 Agent）上报数据
```

---

## 场景 3：ss -s — socket 统计摘要

### 命令

```bash
ss -s
```

### 输出

```
Total: 263
TCP:   41 (estab 22, closed 4, orphaned 0, timewait 4)

Transport Total     IP        IPv6
RAW       1         0         1
UDP       7         5         2
TCP      37        35        2
INET     45        40        5
FRAG      0         0         0
```

### 逐行注解

```
Total: 263
  → ★ 当前系统中所有 socket 的总数（含 TCP/UDP/UNIX/raw 等）

TCP:   41 (estab 22, closed 4, orphaned 0, timewait 4)
  → 41    = TCP socket 总数（含所有状态）
  → estab 22    = 22 个 ESTABLISHED 连接
  → closed 4    = 4 个已关闭的 socket（CLOSED 状态）
  → orphaned 0  = ★ 0 个孤儿连接（没有关联到任何 fd 的 socket）
                   如果 > 0，说明有进程崩溃留下了残留连接
  → timewait 4  = 4 个 TIME_WAIT 连接

Transport Total     IP        IPv6
  → 各协议层的 socket 统计
  → RAW  1  = 1 个原始套接字（ICMP 等）
  → UDP  7  = 7 个 UDP socket（5 个 IPv4 + 2 个 IPv6）
  → TCP 37  = 37 个 TCP socket（35 个 IPv4 + 2 个 IPv6）
  → INET 45 = 所有 Internet socket 总数
  → FRAG 0  = IP 分片重组队列（0 = 没有正在重组的分片）
```

### 诊断要点

```
★ Total 数持续增长 → 可能有 socket 泄漏（应用没有 close）
★ orphaned > 0 → 有进程异常退出，检查是否有僵尸 socket
★ timewait 持续高位 → 短连接过多，考虑长连接或调参
★ TCP estab 接近文件描述符限制 → 检查 ulimit -n 和 somaxconn
```

---

## 场景 4：ss -tm — TCP 内存使用

### 命令

```bash
ss -tm
```

### 输出（节选）

```
State  Recv-Q Send-Q  Local Address:Port     Peer Address:Port
ESTAB  0      0           127.0.0.1:47004       127.0.0.1:13104
       skmem:(r0,rb131072,t0,tb2626560,f0,w0,o0,bl0,d0)
ESTAB  0      327    172.27.141.144:42464 185.199.109.133:https
       skmem:(r0,rb131072,t0,tb87040,f2873,w1223,o0,bl0,d6)
ESTAB  0      0      172.27.141.144:40676   39.106.104.16:https
       skmem:(r0,rb1943995,t0,tb87040,f0,w0,o0,bl0,d0)
ESTAB  0      100    172.27.141.144:ssh      183.247.2.29:13462
       skmem:(r0,rb2944302,t0,tb948736,f3100,w996,o0,bl0,d0)
```

### skmem 字段详解

```
skmem:(r0,rb131072,t0,tb2626560,f0,w0,o0,bl0,d0)
        │    │       │    │       │  │  │   │   │
        │    │       │    │       │  │  │   │   └─ d: drops（被丢弃的数据包数）
        │    │       │    │       │  │  │   └───── bl: backlog（积压的连接数）
        │    │       │    │       │  │  └───────── o: option memory（TCP 选项占用的内存）
        │    │       │    │       │  └──────────── w: wmem_alloc（实际已分配的发送缓冲区内存）
        │    │       │    │       └─────────────── f: fwd_alloc（内核预分配的转发内存）
        │    │       │    └─────────────────────── tb: sndbuf（发送缓冲区总量上限，字节）
        │    │       └──────────────────────────── t: rmem_alloc（实际已分配的接收缓冲区内存）
        │    └──────────────────────────────────── rb: rcvbuf（接收缓冲区总量上限，字节）
        └───────────────────────────────────────── r: rmem_alloc 的另一种表示
```

### 关键发现

```
★ sshd 连接（172.27.141.144:ssh → 183.247.2.29:13462）：
  rb = 2944302（约 2.8MB）→ 接收缓冲区上限很大
  tb = 948736（约 926KB）→ 发送缓冲区上限
  f  = 3100（预分配了 3100 字节转发内存）
  w  = 996（实际已分配 996 字节发送内存）
  ★ SSH 长连接，buffer 分配合理

★ claude 连接（172.27.141.144:42464 → 185.199.109.133:https）：
  d  = 6 → ★ 有 6 个数据包被丢弃！
  可能原因：接收缓冲区满了，来不及处理的数据被丢弃
  排查：检查应用是否及时 read()

★ claude 连接（172.27.141.144:40676 → 39.106.104.16:https）：
  rb = 1943995（约 1.85MB）→ 接收缓冲区上限很大
  ★ 内核根据带宽-延迟积自动调大了接收窗口

★ 本地回环连接（127.0.0.1:47004 → 127.0.0.1:13104）：
  tb = 2626560（约 2.5MB）→ 本地连接 buffer 上限较大
  r0, t0, f0, w0 → 当前没有数据在缓冲区中
```

### 内核 TCP 内存参数对照

```bash
cat /proc/sys/net/ipv4/tcp_mem
```

```
87954  117273  175908
  ↑      ↑       ↑
  │      │       └─ 高水位（high）：超过此值，内核开始积极回收内存
  │      └───────── 压力水位（pressure）：超过此值，内核开始限制分配
  └──────────────── 低水位（low）：低于此值，内核认为内存充足

单位：页（page），每页 4096 字节
87954 页 = 343MB（低水位）
117273 页 = 458MB（压力水位）
175908 页 = 687MB（高水位）
```

---

## 场景 5：ss -to — timer 信息

### 命令

```bash
ss -to
```

### 输出

```
State  Recv-Q Send-Q  Local Address:Port     Peer Address:Port  Process
ESTAB  0      0           127.0.0.1:47004       127.0.0.1:13104
ESTAB  0      0           127.0.0.1:53888       127.0.0.1:34017
ESTAB  0      327    172.27.141.144:42464 185.199.109.133:https timer:(on,2.972ms,7)
ESTAB  0      0      172.27.141.144:40048   39.106.104.16:https timer:(keepalive,35sec,0)
ESTAB  0      0      172.27.141.144:40034   39.106.104.16:https timer:(keepalive,56sec,0)
ESTAB  0      0      172.27.141.144:40676   39.106.104.16:https timer:(keepalive,9.439ms,0)
ESTAB  0      116    172.27.141.144:ssh      183.247.2.29:13462 timer:(on,215ms,0)
ESTAB  0      0      172.27.141.144:46324   39.106.80.255:https timer:(keepalive,52sec,0)
```

### timer 字段详解

```
timer:(on,2.972ms,7)
        ↑   ↑       ↑
        │   │       └─ 已触发次数（此 timer 已经超时触发了 7 次）
        │   └───────── 剩余时间（2.972 毫秒后触发）
        └───────────── timer 类型

timer:(keepalive,35sec,0)
        ↑          ↑    ↑
        │          │    └─ 已触发次数（0 = keepalive 还没触发过）
        │          └─────── 剩余时间（35 秒后发送 keepalive 探测）
        └────────────────── timer 类型
```

### timer 类型说明

```
类型         │ 含义                          │ 触发条件
─────────────┼──────────────────────────────┼──────────────────────
on           │ 重传定时器                    │ 有数据发送但未收到 ACK
             │ (retransmit timer)            │ 超时后重传数据
keepalive    │ keepalive 定时器              │ 连接空闲超过 keepalive_time
             │                               │ 发送探测包检查对端是否存活
timewait     │ TIME_WAIT 定时器              │ TIME_WAIT 状态
             │                               │ 2MSL（60 秒）后释放 socket
persist      │ 持续定时器                    │ 对端接收窗口为 0
             │ (persist timer)               │ 定期发送探测包检查窗口恢复
userspace    │ 用户态设置的定时器            │ 应用通过 SO_SNDTIMEO 等设置
```

### 关键发现

```
★ timer:(on,2.972ms,7) — claude → 185.199.109.133:https
  on = 重传定时器已激活
  2.972ms = 即将触发重传
  7 = 已经重传了 7 次！
  ★ 说明这个连接有严重的丢包或延迟问题
  结合 Send-Q = 327（有数据未确认），可以确认网络异常

★ timer:(keepalive,35sec,0) — claude → 39.106.104.16:https
  keepalive = keepalive 定时器已激活
  35sec = 还有 35 秒发送 keepalive 探测
  0 = 还没触发过 keepalive（连接比较活跃）
  ★ 正常行为

★ timer:(on,215ms,0) — sshd 连接
  on = 重传定时器（SSH 有未确认数据）
  215ms = 重传间隔
  Send-Q = 116（有 116 字节未确认）
  ★ SSH 交互式连接，偶尔有数据未确认是正常的

★ 本地回环连接没有 timer 信息
  本地通信不需要 keepalive 和重传（延迟极低）
```

### keepalive 内核参数对照

```bash
cat /proc/sys/net/ipv4/tcp_keepalive_time   # 7200（2 小时空闲后开始探测）
cat /proc/sys/net/ipv4/tcp_keepalive_intvl  # 75（探测间隔 75 秒）
cat /proc/sys/net/ipv4/tcp_keepalive_probes # 9（最多探测 9 次）
```

---

## 场景 6：netstat -s — 协议统计

### 命令

```bash
netstat -s
```

### 输出（节选关键部分）

```
Ip:
    Forwarding: 2
    3602744 total packets received
    4 with invalid addresses
    0 forwarded
    0 incoming packets discarded
    3602740 incoming packets delivered
    3095587 requests sent out
    2 outgoing packets fragmented ok
    4 fragments created
    OutTransmits: 3095589

Tcp:
    46073 active connection openings
    11513 passive connection openings
    62575 failed connection attempts
    1928 connection resets received
    21 connections established
    3474792 segments received
    3573849 segments sent out
    19293 segments retransmitted
    24 bad segments received
    92500 resets sent
    InCsumErrors: 4

Udp:
    33771 packets received
    104 packets to unknown port received
    0 packet receive errors
    54339 packets sent
    0 receive buffer errors
    0 send buffer errors

TcpExt:
    62481 resets received for embryonic SYN_RECV sockets
    48960 TCP sockets finished time wait in fast timer
    4 packets rejected in established connections because of timestamp
    93504 delayed acks sent
    159 delayed acks further delayed because of locked socket
    Quick ack mode was activated 337 times
    2 SYNs to LISTEN sockets dropped
    359785 packet headers predicted
    402638 acknowledgments not containing data payload received
    904383 predicted acknowledgments
    TCPSackRecovery: 84
    Detected reordering 19681 times using SACK
    Detected reordering 34 times using reno fast retransmit
    166 fast retransmits
    215 retransmits in slow start
    TCPTimeouts: 22985
    TCPLossProbes: 980
    TCPLossProbeRecovery: 36
```

### 逐段注解

#### IP 层

```
3602744 total packets received      → 总共收到的 IP 包数
4 with invalid addresses            → 4 个包目的地址无效
0 forwarded                         → 没有转发（非路由器）
3602740 incoming packets delivered  → 成功交付给上层协议的包数
3095587 requests sent out           → 发出的 IP 包数
2 outgoing packets fragmented ok    → 2 个包被分片（MTU 限制）

★ 收 > 发 → 典型的服务器模式（请求少但响应多，或接收大量数据）
```

#### TCP 层

```
46073 active connection openings    → 主动发起的连接数（客户端角色）
11513 passive connection openings   → 被动接受的连接数（服务端角色）
62575 failed connection attempts    → ★ 失败连接数（SYN 无响应/被拒绝）
                                      大量失败 = 目标不可达或服务异常
1928 connection resets received     → 收到 RST 包（对方拒绝连接）
21 connections established          → 当前活跃的连接数
3474792 segments received           → 收到的 TCP 段数
3573849 segments sent out           → 发出的 TCP 段数
19293 segments retransmitted        → ★ 重传段数
                                      重传率 = 19293 / 3573849 = 0.54%
                                      ★ < 1% 正常，> 5% 需要排查网络
24 bad segments received            → 收到的损坏段（校验和错误等）
92500 resets sent                   → ★ 发送的 RST 包数
                                      大量 RST = 拒绝了大量连接请求
```

#### TCP 扩展统计

```
62481 resets received for embryonic SYN_RECV sockets
  → ★ 大量 SYN_RECV 状态被 RST 重置
  → 说明有大量未完成的三次握手（可能是端口扫描或 SYN flood 探测）

48960 TCP sockets finished time wait in fast timer
  → 48960 个 TIME_WAIT socket 通过快速定时器释放

93504 delayed acks sent
  → 延迟 ACK 机制发送的 ACK 数（TCP 优化：合并 ACK 减少包数）

2 SYNs to LISTEN sockets dropped
  → ★ 2 个 SYN 包被丢弃（backlog 满或 SYN Cookie 触发）
  → 少量丢弃正常，大量说明遭受 SYN flood

19681 times using SACK (detected reordering)
  → SACK 检测到 19681 次乱序
  → 网络中存在包重排序（常见于多路径路由）

166 fast retransmits
  → 快速重传（收到 3 个重复 ACK 后立即重传，不等超时）

TCPTimeouts: 22985
  → ★ TCP 超时次数（含连接超时、重传超时等）
  → 大量超时 = 网络不稳定或对端不可达
```

### 诊断要点

```
关键指标                    本例值           正常范围        异常信号
────────────────────────    ──────           ────────        ──────────
重传率                      0.54%            < 1%            > 5%
failed connections          62575            少量            突增
resets sent                 92500            少量            突增
SYN drops                   2                0               > 100
TCPTimeouts                 22985            少量            突增
delayed acks                93504            —               正常优化行为
```

---

## 场景 7：ss state time-wait — TIME_WAIT 统计

### 命令

```bash
ss -tn state time-wait
```

### 输出

```
Recv-Q Send-Q  Local Address:Port     Peer Address:Port
0      0      172.27.141.144:40532 100.100.167.126:443
0      0      172.27.141.144:51432 100.100.167.125:443
0      0      172.27.141.144:51374  100.100.100.10:80
0      0      172.27.141.144:59598   100.100.2.148:80
0      0      172.27.141.144:46940 100.100.167.125:443
```

### 注解

```
★ 5 个 TIME_WAIT 连接，全部指向 100.100.x.x 网段
  100.100.x.x = 阿里云内网地址（元数据服务、监控等）
  说明本机频繁连接阿里云内部服务（短连接）

★ 远端端口分布：443（HTTPS）和 80（HTTP）
  典型的 API 调用后关闭连接 → 产生 TIME_WAIT

★ Recv-Q 和 Send-Q 都是 0
  TIME_WAIT 状态下不需要缓冲区（连接已关闭，只是等待超时）
```

### TIME_WAIT 统计命令

```bash
# 统计 TIME_WAIT 数量
ss -tn state time-wait | wc -l

# 按远端 IP 分组统计
ss -tn state time-wait | awk 'NR>1{print $5}' | cut -d: -f1 | sort | uniq -c | sort -rn

# 按本地端口分组（看哪些本地端口被 TIME_WAIT 占用）
ss -tn state time-wait | awk 'NR>1{print $4}' | cut -d: -f2 | sort | uniq -c | sort -rn
```

### TIME_WAIT 健康度评估

```
当前数量     tcp_max_tw_buckets     评估
────────     ──────────────────     ────
5            262144                 ★ 完全健康（使用率 < 0.01%）

> 1 万       262144                 ⚠️ 需关注（使用率 ~4%）
> 10 万      262144                 🔴 接近上限（使用率 ~38%）
> 26 万      262144                 💀 已达上限，新连接可能被拒绝
```

---

## 场景 8：查找占用特定端口的进程

### 命令

```bash
# ss 方式（推荐）
ss -tlnp 'sport = :22'

# netstat 方式（对照）
netstat -tlnp | grep ':22 '
```

### ss 输出

```
State  Recv-Q Send-Q Local Address:Port Peer Address:Port Process
LISTEN 0      128          0.0.0.0:22        0.0.0.0:*    users:(("sshd",pid=979,fd=3))
LISTEN 0      128             [::]:22           [::]:*    users:(("sshd",pid=979,fd=4))
```

### netstat 输出

```
Proto Recv-Q Send-Q Local Address           Foreign Address         State       PID/Program name
tcp        0      0 0.0.0.0:22              0.0.0.0:*               LISTEN      979/sshd: /usr/sbin
tcp6       0      0 :::22                   :::*                    LISTEN      979/sshd: /usr/sbin
```

### 注解

```
★ 端口 22 被 sshd 占用
  PID = 979
  监听两个地址：0.0.0.0:22 (IPv4) 和 [::]:22 (IPv6)
  fd=3 是 IPv4 socket，fd=4 是 IPv6 socket

★ ss 的 filter 语法比 grep 更高效
  'sport = :22' 在内核侧过滤，只返回匹配的行
  grep ':22 ' 在用户侧过滤，需要先读取所有数据
  当连接数 > 10 万时，ss filter 的优势更明显
```

### 常用端口查找命令

```bash
# 查找 80 端口
ss -tlnp 'sport = :80'

# 查找 443 端口
ss -tlnp 'sport = :443'

# 查找 8000-9000 范围的端口
ss -tlnp 'sport >= :8000 and sport <= :9000'

# 查找 UDP 端口
ss -ulnp 'sport = :53'

# 同时查找 TCP + UDP
ss -tulnp 'sport = :53'
```

---

## 场景 9：TCP 状态分布统计

### 命令

```bash
ss -tna | awk 'NR>1{print $1}' | sort | uniq -c | sort -rn
```

### 输出

```
     21 ESTAB
     15 LISTEN
      5 TIME-WAIT
```

### 注解

```
21 ESTAB     → 21 个已建立连接（活跃数据传输中）
15 LISTEN    → 15 个监听端口（等待新连接）
 5 TIME-WAIT → 5 个等待关闭的连接（60 秒后释放）

★ 总连接数 = 21 + 5 = 26（LISTEN 不算在活跃连接中）
★ 没有 CLOSE_WAIT、FIN_WAIT、SYN_RECV → 健康状态

★ 异常信号：
  - CLOSE_WAIT > 0 → 应用层没有 close()
  - SYN_RECV > 100 → 可能 SYN flood
  - FIN_WAIT > 100 → 对端不响应 ACK
  - ESTAB 异常多 → 连接泄漏或爬虫
```

---

## 场景 10：ss -te — 扩展信息（inode、cgroup）

### 命令

```bash
ss -te
```

### 输出（节选）

```
ESTAB  0   0   172.27.141.144:42464  185.199.109.133:https
       timer:(on,,7) ino:4486214 sk:1c cgroup:/user.slice/user-0.slice/session-29.scope <->
ESTAB  0   0   172.27.141.144:ssh   183.247.2.29:13462
       timer:(keepalive,115min,0) ino:4480210 sk:20 cgroup:/system.slice/sshd.service <->
ESTAB  0   0   172.27.141.144:57858  100.100.30.25:http
       ino:8869 sk:1015 cgroup:/aegis <->
```

### 注解

```
ino:4486214   → inode 号（内核中唯一标识此 socket）
                可以在 /proc/<pid>/fd/ 中找到对应的符号链接
                ls -la /proc/433969/fd/ | grep socket

sk:1c         → socket 内核指针（唯一标识，调试用）

cgroup:/user.slice/user-0.slice/session-29.scope
              → ★ 进程所属的 cgroup 路径
              → session-29.scope = 第 29 个登录会话
              → 可用于追踪 socket 属于哪个 systemd 服务

cgroup:/system.slice/sshd.service
              → 明确标识为 sshd 服务

cgroup:/aegis
              → 阿里云安骑士（aegis）的 cgroup

<->           → 连接方向标记（<-> = 双向连接）
```

### 实用场景

```bash
# 通过 inode 找到对应的进程文件描述符
ls -la /proc/*/fd 2>/dev/null | grep 'socket:\[4486214\]'

# 按 cgroup 过滤 socket（查看某个服务的所有连接）
ss -te | grep 'sshd.service'

# 查看特定 cgroup 的连接数
ss -te | grep -c 'user.slice'
```

---

## 场景 11：ss 替代 netstat 命令对照表（实战验证）

### 对照表

```
┌──────────────────────────────────┬──────────────────────────────────────┬──────────────────────┐
│ netstat 命令                     │ ss 等效命令                          │ 说明                 │
├──────────────────────────────────┼──────────────────────────────────────┼──────────────────────┤
│ netstat -tlnp                    │ ss -tlnp                             │ TCP 监听端口         │
│ netstat -tn                      │ ss -tn                               │ TCP 已建立连接       │
│ netstat -tna                     │ ss -tna                              │ 所有 TCP 连接        │
│ netstat -ulnp                    │ ss -ulnp                             │ UDP 监听端口         │
│ netstat -tulnp                   │ ss -tulnp                            │ TCP+UDP 监听         │
│ netstat -s                       │ netstat -s ★                         │ 协议统计（ss -s 不同）│
│ netstat -an | grep :80           │ ss -tlnp 'sport = :80'               │ 查找端口（ss 更快）  │
│ netstat -an | grep ESTABLISHED   │ ss -tn state established             │ 按状态过滤           │
│ netstat -an | grep TIME_WAIT     │ ss -tn state time-wait               │ TIME_WAIT 过滤       │
│ netstat -tcn                     │ watch -n1 ss -tn                     │ 持续监控             │
│ netstat -rn                      │ ip route                             │ 路由表               │
│ netstat -i                       │ ip -s link                           │ 接口统计             │
│ netstat -p | grep <pid>          │ ss -tnp | grep <pid>                 │ 按进程查             │
└──────────────────────────────────┴──────────────────────────────────────┴──────────────────────┘
```

### ★ 重要区别

```
1. netstat -s ≠ ss -s
   netstat -s → 详细的 IP/TCP/UDP/ICMP 协议统计（收发包数、重传、错误等）
   ss -s      → 仅 socket 摘要（总数、各协议计数）
   ★ 需要协议统计时，仍然用 netstat -s

2. netstat 的进程信息格式不同
   netstat: PID/Program name（如 "979/sshd: /usr/sbin"）
   ss:      users:(("sshd",pid=979,fd=3))
   ★ ss 的格式更适合解析（可直接提取 pid 和 fd）

3. netstat 没有的功能
   - timer 信息（ss -to）
   - socket 内存（ss -tm）
   - TCP 内部参数（ss -ti：cwnd、rtt、ssthresh）
   - cgroup 信息（ss -te）
   - 内核侧过滤（ss filter）
```

---

## 场景 12：UDP 监听端口

### 命令

```bash
ss -ulnp
```

### 输出

```
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port Process
UNCONN 0      0         127.0.0.54:53        0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=18))
UNCONN 0      0      127.0.0.53%lo:53        0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=16))
UNCONN 0      0          127.0.0.1:323       0.0.0.0:*    users:(("chronyd",pid=693,fd=5))
UNCONN 0      0            0.0.0.0:5355      0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=10))
UNCONN 0      0              [::1]:323          [::]:*    users:(("chronyd",pid=693,fd=6))
UNCONN 0      0               [::]:5355         [::]:*    users:(("systemd-resolve",pid=646,fd=12))
```

### 注解

```
State → UNCONN = UDP 无连接状态（UDP 是无连接协议，没有 ESTABLISHED）

★ systemd-resolved 的 UDP 端口：
  127.0.0.53:53  → 本地 DNS 解析（stub listener）
  127.0.0.54:53  → 第二个 DNS 地址
  0.0.0.0:5355   → LLMNR（链路本地多播名称解析）

★ chronyd 的 NTP 端口：
  127.0.0.1:323  → NTP 时间同步（IPv4）
  [::1]:323      → NTP 时间同步（IPv6）
  323 是 chrony 的非特权端口（标准 NTP 是 123）

★ UDP 没有 LISTEN/ESTABLISHED 的概念
  ss 用 UNCONN 表示所有 UDP socket
  netstat 中 UDP 的 State 列通常是空的
```

---

## 场景 13：ss -4 -tlnp — 只看 IPv4

### 命令

```bash
ss -4 -tlnp
```

### 输出（节选）

```
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port Process
LISTEN 0      128        127.0.0.1:34017      0.0.0.0:*    users:(("code-fcf604774b",pid=433236,fd=9))
LISTEN 0      2048         0.0.0.0:8000       0.0.0.0:*    users:(("python3",pid=56771,fd=13))
LISTEN 0      128          0.0.0.0:22         0.0.0.0:*    users:(("sshd",pid=979,fd=3))
LISTEN 0      511          0.0.0.0:80         0.0.0.0:*    users:(("nginx",pid=54598,fd=6),...)
...
```

### 注解

```
★ -4 参数只显示 IPv4 socket
  对比不加 -4 的输出：去掉了 [::]:22 和 [::]:5355 等 IPv6 条目
  在双栈环境中非常有用（区分 IPv4 和 IPv6 的监听情况）

★ -6 参数只显示 IPv6 socket
  ss -6 -tlnp  → 只看 IPv6 监听端口
```

---

## 诊断速查总结

| 问题 | 命令 | 看什么 | 健康标准 |
|------|------|--------|---------|
| 端口被谁占了 | `ss -tlnp 'sport = :80'` | Process 列 | — |
| TIME_WAIT 过多 | `ss -tn state time-wait \| wc -l` | 数量 | < 数千 |
| CLOSE_WAIT 堆积 | `ss -tn state close-wait` | 有就有问题 | 0 |
| SYN flood 检测 | `ss -tn state syn-recv \| wc -l` | 数量 | < 100 |
| 网络丢包/重传 | `ss -to` | timer:(on,x,次数) | 重传次数少 |
| socket 内存占用 | `ss -tm` | skmem 中的 rb/tb | — |
| 缓冲区积压 | `ss -tn` | Send-Q 持续增长 | < 数千 |
| 连接数排行 | `ss -tn \| awk` | 远端 IP 分组统计 | — |
| 协议级统计 | `netstat -s` | 重传率、失败连接 | 重传率 < 1% |
| 进程归属 | `ss -te` | cgroup 信息 | — |
| socket 泄漏 | `ss -s` | Total 持续增长 | 稳定 |
