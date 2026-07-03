# ss 命令输出实战 — 真实输出逐字段解读

```
======================================================================
Step 3: ss 命令输出实战
======================================================================
阅读要点：
  1. 每个命令都实际执行，展示真实输出
  2. 逐字段注解，不遗漏任何细节
  3. ★ 标记性能分析中最关键的字段
  4. 每个场景末尾有"诊断要点"总结
实验环境：
  系统：Alibaba Cloud Linux 4
  内核：6.6.102-5.3.1.alnx4.x86_64
  iproute2：6.6.0
  CPU：AMD EPYC（多核服务器）
预计阅读时间：30 分钟
======================================================================
```

---

## 目录

- [场景 1：ss -tlnp — TCP 监听端口](#场景-1ss--tlnp--tcp-监听端口)
- [场景 2：ss -tnp — 已建立连接](#场景-2ss--tnp--已建立连接)
- [场景 3：ss -s — ★ socket 统计摘要](#场景-3ss--s--socket-统计摘要)
- [场景 4：ss -tm — TCP 内存使用](#场景-4ss--tm--tcp-内存使用)
- [场景 5：ss -to — timer 信息](#场景-5ss--to--timer-信息)
- [场景 6：ss -ti — ★ TCP 详细信息](#场景-6ss--ti--tcp-详细信息)
- [场景 7：ss -te — 扩展信息](#场景-7ss--te--扩展信息)
- [场景 8：ss state time-wait — TIME_WAIT 分析](#场景-8ss-state-time-wait--time_wait-分析)
- [场景 9：ss state close-wait — CLOSE_WAIT 泄漏检测](#场景-9ss-state-close-wait--close_wait-泄漏检测)
- [场景 10：ss 过滤器高级用法](#场景-10ss-过滤器高级用法)
- [场景 11：★ 网络性能诊断流程](#场景-11-网络性能诊断流程)
- [场景 12：TIME_WAIT/CLOSE_WAIT 优化方案](#场景-12time_waitclose_wait-优化方案)

---

## 场景 1：ss -tlnp — TCP 监听端口

### 命令

```bash
ss -tlnp
```

### 实际输出

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
LISTEN 0      511          0.0.0.0:80         0.0.0.0:*    users:(("nginx",pid=473961,fd=6),("nginx",pid=473960,fd=6),("nginx",pid=473959,fd=6),("nginx",pid=473956,fd=6),("nginx",pid=460421,fd=6))
LISTEN 0      128        127.0.0.1:39185      0.0.0.0:*    users:(("code-fcf604774b",pid=433322,fd=9))
LISTEN 0      4096      127.0.0.54:53         0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=19))
LISTEN 0      511        127.0.0.1:10426      0.0.0.0:*    users:(("MainThread",pid=433384,fd=39))
LISTEN 0      4096            [::]:5355          [::]:*    users:(("systemd-resolve",pid=646,fd=13))
LISTEN 0      128             [::]:22            [::]:*    users:(("sshd",pid=979,fd=4))
```

### 逐字段注解

```
字段 1: State = LISTEN
  → 所有行的状态都是 LISTEN（因为用了 -l 参数）
  → 表示正在等待新的连接

字段 2: Recv-Q（★ 对 LISTEN 状态有特殊含义）
  → 当前 accept 队列中已完成三次握手的连接数
  → 全部为 0 → 没有等待 accept() 的连接（正常）
  → ★ 如果 Recv-Q > 0 且持续不减少 → 应用 accept() 太慢！

字段 3: Send-Q（★ listen backlog）
  → listen() 的 backlog 参数（accept 队列上限）
  → 实际生效值 = min(应用指定的 backlog, somaxconn)
  → 本机的 somaxconn = 4096
  → 例如：
    128    → 较小的 backlog（开发工具）
    2048   → 中等 backlog（python3 应用）
    4096   → 达到 somaxconn 上限（systemd-resolved）
    511    → nginx 默认 backlog

  ★ 性能分析要点：
    如果 Recv-Q 接近 Send-Q → 全队列溢出 → 连接被丢弃
    可以用 ss -tln 检查，或检查 /proc/net/netstat 的 ListenOverflows

字段 4: Local Address:Port
  → 监听的本地地址和端口
  → 0.0.0.0:22    → 监听所有 IPv4 接口的 22 端口
  → 127.0.0.1:22  → 仅监听本地回环
  → 127.0.0.53%lo:53 → 监听特定 IP（systemd-resolved），%lo 是设备名
  → [::]:22       → 监听所有 IPv6 接口

字段 5: Peer Address:Port
  → LISTEN 状态下固定为 0.0.0.0:* 或 [::]:*
  → 表示对端地址尚未确定

字段 6: Process
  → 持有该监听 socket 的进程
  → users:(("进程名",pid=PID,fd=FD))
  → ★ nginx 有 5 个进程共享同一个监听 socket（master + 4 workers）
  → 部分端口没有 Process 信息（如 18080/18081）→ 需要 root 权限或进程已退出
```

### 诊断要点

| 检查项 | 正常值 | 异常信号 |
|--------|--------|---------|
| Recv-Q | 0 | > 0 且持续 → accept 慢 |
| Send-Q (backlog) | 128~4096 | < 10 → 可能丢连接 |
| 无 Process 信息 | 权限不足 | root 下仍无 → 进程异常 |

---

## 场景 2：ss -tnp — 已建立连接

### 命令

```bash
ss -tnp
```

### 实际输出

```
State Recv-Q Send-Q  Local Address:Port   Peer Address:Port Process
ESTAB 0      0           127.0.0.1:53888     127.0.0.1:34017 users:(("sshd",pid=433217,fd=10))
ESTAB 0      0      172.27.141.144:46512 39.106.104.16:443   users:(("claude",pid=480229,fd=34))
ESTAB 0      0      172.27.141.144:55932 39.106.80.255:443   users:(("claude",pid=480229,fd=20))
ESTAB 0      0      172.27.141.144:60560 160.79.104.10:443   users:(("claude",pid=201857,fd=19))
ESTAB 0      100    172.27.141.144:22     183.247.2.29:13462 users:(("sshd",pid=433217,fd=4),("sshd",pid=433214,fd=4))
ESTAB 0      0           127.0.0.1:39185     127.0.0.1:36228 users:(("code-fcf604774b",pid=433322,fd=12))
ESTAB 161    0      172.27.141.144:35996 39.106.80.255:443   users:(("claude",pid=480229,fd=22))
ESTAB 0      0      172.27.141.144:51348 39.106.104.16:443   users:(("claude",pid=480229,fd=30))
ESTAB 0      0           127.0.0.1:34017     127.0.0.1:53888 users:(("code-fcf604774b",pid=433236,fd=12))
ESTAB 0      0      172.27.141.144:51356 39.106.104.16:443   users:(("claude",pid=480229,fd=31))
ESTAB 0      0           127.0.0.1:36228     127.0.0.1:39185 users:(("sshd",pid=433298,fd=10))
ESTAB 0      0      172.27.141.144:46102 39.106.104.16:443   users:(("claude",pid=480229,fd=26))
ESTAB 0      100    172.27.141.144:22     183.247.2.29:13472 users:(("sshd",pid=433298,fd=4),("sshd",pid=433288,fd=4))
ESTAB 0      0      172.27.141.144:40604 39.106.80.255:443   users:(("claude",pid=480229,fd=27))
ESTAB 0      0      172.27.141.144:57858 100.100.30.25:80    users:(("AliYunDun",pid=1490,fd=9))
```

### 逐字段注解

```
字段 1: State = ESTAB
  → 所有连接都已建立（因为未用 -a，默认只显示非监听状态）

字段 2: Recv-Q（★ 性能关键）
  → 接收缓冲区中未被应用层 read() 的字节数
  → 大部分为 0 → 应用层读取正常
  → ★ 第 7 行 Recv-Q=161 → claude 进程有 161 字节未读取
     可能原因：应用正在处理其他任务，暂时未 read
     如果持续增长 → 应用读取速度跟不上网络速度

字段 3: Send-Q（★ 性能关键）
  → 发送缓冲区中未被对端 ACK 的字节数
  → 大部分为 0 → 数据都被及时 ACK
  → ★ 第 5 行 Send-Q=100 → sshd 有 100 字节等待 ACK
     第 13 行 Send-Q=100 → 同样的 SSH 连接
     → 可能是客户端网络延迟或 TCP 延迟 ACK 机制
     → 100 字节很小，通常不值得担心

字段 4: Local Address:Port
  → 本机的 IP 和端口
  → 172.27.141.144 → 主网卡 IP（阿里云 VPC）
  → 127.0.0.1 → 本地回环

字段 5: Peer Address:Port
  → 对端的 IP 和端口
  → 39.106.104.16:443 → 远程 HTTPS 服务（claude API）
  → 183.247.2.29:13462 → SSH 客户端来源 IP
  → 100.100.30.25:80 → 阿里云内网服务（AliYunDun 安全客户端）

字段 6: Process（★ 进程归属）
  → users:(("进程名",pid=PID,fd=FD))
  → ★ SSH 连接有两个进程（fork 模型）：
    ("sshd",pid=433217,fd=4),("sshd",pid=433214,fd=4)
    → pid=433214 是 master 进程，433217 是子进程
  → claude 进程（pid=480229）有多个 HTTPS 连接
    → 多个 fd 对应不同的连接
```

### 诊断要点

| 检查项 | 正常值 | 异常信号 |
|--------|--------|---------|
| Recv-Q | 0（瞬时值，偶尔非零正常） | 持续增长 → 应用读取慢 |
| Send-Q | 0 | 持续 > 0 → 网络拥塞或对端慢 |
| 连接数 | 取决于应用 | 突然暴增 → 连接泄漏或攻击 |

---

## 场景 3：ss -s — ★ socket 统计摘要

### 命令

```bash
ss -s
```

### 实际输出

```
Total: 269
TCP:   32 (estab 15, closed 2, orphaned 0, timewait 2)

Transport Total     IP        IPv6
RAW	  1         0         1
UDP	  7         5         2
TCP	  30        28        2
INET	  38        33        5
FRAG	  0         0         0
```

### 逐字段注解

```
★ 第一行：Total: 269
  → 系统中所有 socket 的总数（包括 TCP、UDP、Unix、RAW、netlink 等）
  → 269 个 socket → 本系统负载较轻

★ 第二行：TCP 统计（★ 性能分析最重要的信息）
  TCP: 32 (estab 15, closed 2, orphaned 0, timewait 2)

  32         → TCP socket 总数（含所有状态）
  estab 15   → ★ 已建立连接数（活跃数据传输）
  closed 2   → 已关闭的连接（处于清理中）
  orphaned 0 → ★ 孤儿连接数（已被用户进程关闭但 TCP 仍在处理）
               ★ 如果 orphaned > 0 且持续增长 → 可能内存泄漏
  timewait 2 → ★ TIME_WAIT 连接数
               ★ 如果 timewait > 数千 → 短连接风暴

  ★ 性能分析关注点：
    1. estab 数量 → 活跃连接规模
    2. timewait 数量 → 短连接压力
    3. orphaned 数量 → 潜在泄漏

第三行开始：按协议分类

  Transport  Total   IP     IPv6
  ─────────  ─────  ─────  ─────
  RAW        1       0      1      → RAW socket（icmp6 用于 NDP）
  UDP        7       5      2      → UDP socket（DNS + NTP）
  TCP        30      28     2      → TCP socket（不含 TIME_WAIT）
  INET       38      33     5      → 所有 Internet socket
  FRAG       0       0      0      → IP 分片重组队列

  ★ 注意：TCP 行显示 30，但上面显示 32
    差异来自 TIME_WAIT（2 个）和 CLOSED（2 个）
    TIME_WAIT 的 socket 在 hash 表中但不在常规的 TCP 统计中
```

### 诊断要点

```
★ ss -s 是性能排查的第一步！

检查流程：
  1. Total → 总 socket 数是否异常（正常系统 < 1000，高并发可达数万）
  2. estab → 活跃连接数是否符合预期
  3. timewait → 短连接是否过多
  4. orphaned → 是否有连接泄漏

异常阈值参考（取决于业务场景）：
  estab > 10000    → 需要检查连接是否正常
  timewait > 5000  → 短连接风暴，考虑连接池
  orphaned > 100   → 应用层可能有泄漏
```

---

## 场景 4：ss -tm — TCP 内存使用

### 命令

```bash
ss -tm
```

### 实际输出（选取有代表性的连接）

```
State Recv-Q Send-Q  Local Address:Port   Peer Address:Port Process
ESTAB 0      0           127.0.0.1:53888     127.0.0.1:34017
	 skmem:(r0,rb1898851,t0,tb2626560,f0,w0,o0,bl0,d0)
ESTAB 0      100    172.27.141.144:22     183.247.2.29:13462
	 skmem:(r0,rb2944302,t0,tb948736,f3100,w996,o0,bl0,d0)
ESTAB 0      0      172.27.141.144:35996 39.106.80.255:https
	 skmem:(r0,rb1948501,t0,tb870400,f0,w0,o0,bl0,d0)
ESTAB 0      0      172.27.141.144:57858 100.100.30.25:http
	 skmem:(r0,rb2368477,t0,tb87040,f0,w0,o0,bl0,d0)
```

### 逐字段注解

```
★ skmem 字段完整解读：

格式：skmem:(r<rmem_alloc>,rb<rcv_buf>,t<wmem_alloc>,tb<snd_buf>,
             f<fwd_alloc>,w<wmem_queued>,o<optmem>,bl<back_log>,d<dropped>)

第 1 个连接（本地回环 sshd ↔ code）：
  r0         → 接收缓冲区已用 0 字节（空闲）
  rb1898851  → 接收缓冲区上限 ~1.8MB（内核自动调整）
  t0         → 发送缓冲区已用 0 字节（空闲）
  tb2626560  → 发送缓冲区上限 ~2.5MB
  f0         → 前向分配 0 字节
  w0         → 发送队列 0 字节
  o0         → 选项内存 0 字节
  bl0        → backlog 内存 0 字节
  d0         → 丢包 0

  ★ 解读：本地回环连接，缓冲区充裕，无积压

第 2 个连接（SSH 到外网 ★ 有积压）：
  r0         → 接收空闲
  rb2944302  → ★ 接收缓冲区上限 ~2.8MB（较大，说明 SSH 流量大）
  t0         → 发送缓冲区已用 0 字节
  tb948736   → 发送缓冲区上限 ~927KB
  ★ f3100    → ★ 前向分配 3100 字节（内核预分配内存页）
  ★ w996     → ★ 发送队列有 996 字节排队（对应 Send-Q=100 的积压）
  o0         → 选项内存 0
  bl0        → backlog 0
  d0         → 无丢包

  ★ 解读：SSH 连接有少量发送积压（996 字节），但很小
    f=3100 表示内核为此 socket 预分配了内存
    这在活跃连接中很常见

第 3 个连接（HTTPS 到 claude API）：
  rb1948501  → 接收缓冲区上限 ~1.9MB
  tb870400   → 发送缓冲区上限 ~850KB
  全零积压   → 连接空闲或数据流动顺畅

第 4 个连接（AliYunDun 到内网 HTTP）：
  rb2368477  → 接收缓冲区上限 ~2.3MB
  ★ tb87040  → ★ 发送缓冲区上限仅 ~85KB（很小）
    → 可能是因为该连接流量小，内核未扩展发送缓冲区
    → TCP 缓冲区是按需扩展的（从 tcp_wmem[0] 到 tcp_wmem[2]）
```

### 内核 TCP 内存参数对照

```bash
$ cat /proc/sys/net/ipv4/tcp_mem
87954	117273	175908

# 单位是页（4KB），转换为 MB：
# min = 87954 × 4KB = ~343MB
# pressure = 117273 × 4KB = ~458MB
# max = 175908 × 4KB = ~687MB

# ★ 含义：
# 当 TCP 总内存 < 343MB → 正常模式
# 当 TCP 总内存 343-458MB → 压力模式（限制新分配）
# 当 TCP 总内存 > 458MB → 超限模式（拒绝新分配，可能丢包）

$ cat /proc/sys/net/ipv4/tcp_rmem
4096	131072	6291456
# 每个 socket 的接收缓冲区：
# min = 4KB → 初始大小
# default = 128KB → 默认大小
# max = 6MB → ★ 上限（上面 rb 值不会超过此值）

$ cat /proc/sys/net/ipv4/tcp_wmem
4096	16384	4194304
# 每个 socket 的发送缓冲区：
# min = 4KB → 初始大小
# default = 16KB → 默认大小
# max = 4MB → ★ 上限（上面 tb 值不会超过此值）
```

### 诊断要点

| 检查项 | 正常值 | 异常信号 |
|--------|--------|---------|
| rb（接收上限） | 128KB~6MB | 接近 tcp_rmem[2] → 大流量连接 |
| tb（发送上限） | 16KB~4MB | 很小（< 64KB）→ 流量小或被限制 |
| d（丢包） | 0 | ★ > 0 → 内存不足导致丢包 |
| w（发送排队） | 0 | 持续 > 0 → 发送积压 |
| TCP 总内存 | < tcp_mem[0] | > tcp_mem[1] → 压力模式 |

---

## 场景 5：ss -to — timer 信息

### 命令

```bash
ss -to
```

### 实际输出

```
State Recv-Q Send-Q  Local Address:Port   Peer Address:Port Process
ESTAB 0      0           127.0.0.1:53888     127.0.0.1:34017
ESTAB 0      0      172.27.141.144:46512 39.106.104.16:https timer:(keepalive,52sec,0)
ESTAB 0      0      172.27.141.144:55932 39.106.80.255:https timer:(keepalive,52sec,0)
ESTAB 0      0      172.27.141.144:60560 160.79.104.10:https timer:(keepalive,392ms,0)
ESTAB 0      100    172.27.141.144:22     183.247.2.29:13462 timer:(on,228ms,0)
ESTAB 0      0           127.0.0.1:39185     127.0.0.1:36228
ESTAB 0      0      172.27.141.144:35996 39.106.80.255:https timer:(keepalive,19sec,0)
ESTAB 0      0      172.27.141.144:51348 39.106.104.16:https timer:(keepalive,47sec,0)
ESTAB 0      0           127.0.0.1:34017     127.0.0.1:53888
ESTAB 0      0      172.27.141.144:51356 39.106.104.16:https timer:(keepalive,46sec,0)
ESTAB 0      0           127.0.0.1:36228     127.0.0.1:39185
ESTAB 0      0      172.27.141.144:46102 39.106.104.16:https timer:(keepalive,,0)
ESTAB 0      0      172.27.141.144:22     183.247.2.29:13472 timer:(keepalive,42min,0)
ESTAB 0      0      172.27.141.144:40604 39.106.80.255:https timer:(keepalive,45sec,0)
ESTAB 0      0      172.27.141.144:57858 100.100.30.25:http
```

### 逐字段注解

```
timer 格式：timer:(<type>,<remaining>,<retrans>)

类型 1：无定时器（本地回环连接）
  第 1 行：127.0.0.1:53888 → 无 timer 字段
  → 本地回环连接不需要 keepalive（延迟极低）

类型 2：keepalive 定时器
  timer:(keepalive,52sec,0)
  → type = keepalive：TCP 保活定时器
  → remaining = 52sec：距离下次发送 keepalive 探测包还有 52 秒
  → retrans = 0：未重传
  → ★ keepalive 默认间隔 = 7200 秒（tcp_keepalive_time）
  → 如果 remaining 很小（< 60 秒）→ 说明应用设置了较短的 keepalive 间隔

类型 3：重传定时器（★ 需要关注）
  timer:(on,228ms,0)
  → type = on：重传定时器活跃
  → remaining = 228ms：距离下次重传超时还有 228ms
  → retrans = 0：尚未重传
  → ★ 这说明有数据发出但未收到 ACK，正在等待重传超时
  → 配合 Send-Q=100 验证：确实有 100 字节未 ACK

类型 4：keepalive 刚触发
  timer:(keepalive,,0)
  → remaining 为空 → keepalive 刚触发或定时器刚重置

特殊观察：
  SSH 连接 183.247.2.29:13472 的 keepalive 剩余 42min
  → 这个 SSH 连接设置了较长的 keepalive 间隔（约 42 分钟）
  → SSH 默认 TCPKeepAlive = yes（使用系统 tcp_keepalive_time = 7200s）
  → 42min ≈ 2520s，说明连接已活跃了一段时间

★ 性能分析要点：
  - timer:(on,...) 频繁出现 → 网络丢包严重
  - timer:(on,...,N) 中 N > 3 → 多次重传，网络质量差
  - keepalive remaining 持续减小 → 对端可能已断开
```

### 诊断要点

| 检查项 | 正常值 | 异常信号 |
|--------|--------|---------|
| timer:(keepalive,...) | 正常保活 | remaining 持续为 0 → 对端无响应 |
| timer:(on,...) | 偶尔出现 | ★ 频繁出现 → 网络丢包 |
| retrans > 0 | 偶尔 1-2 次 | ★ > 5 → 网络质量差 |

---

## 场景 6：ss -ti — ★ TCP 详细信息

### 命令

```bash
ss -ti
```

这是**性能分析最重要的命令**，输出包含 TCP 拥塞控制、RTT、窗口等核心参数。

### 实际输出（选取 4 个典型连接）

**连接 A：本地回环（ssh 内部通信）**
```
ESTAB 0      0           127.0.0.1:53888     127.0.0.1:34017
	 cubic wscale:7,7 rto:202 rtt:1.834/0.75 ato:145 mss:65483 pmtu:65535
	 rcvmss:65483 advmss:65483 cwnd:10 bytes_sent:457430 bytes_acked:457431
	 bytes_received:9250931 segs_out:23037 segs_in:22893 data_segs_out:353
	 data_segs_in:22783 send 2.86Gbps lastsnd:56472 lastrcv:40 lastack:40
	 pacing_rate 5.71Gbps delivery_rate 47.6Gbps delivered:354 app_limited
	 busy:1722ms rcv_rtt:1 rcv_space:69149 rcv_ssthresh:1868173 minrtt:0.041
	 snd_wnd:4376064 rcv_wnd:1868288
```

**连接 B：外网 HTTPS（claude API）**
```
ESTAB 0      0      172.27.141.144:46512 39.106.104.16:https
	 cubic wscale:9,7 rto:228 rtt:27.686/0.054 ato:51 mss:1460 pmtu:1500
	 rcvmss:1460 advmss:1460 cwnd:1904 bytes_sent:4751459 bytes_acked:4751460
	 bytes_received:275906 segs_out:4563 segs_in:1636 data_segs_out:3272
	 data_segs_in:1293 send 803Mbps lastsnd:8412 lastrcv:72 lastack:72
	 pacing_rate 1.61Gbps delivery_rate 373Mbps delivered:3273 app_limited
	 busy:477ms rwnd_limited:28ms(5.9%) dsack_dups:1
	 rcv_rtt:26852.6 rcv_space:86531 rcv_ssthresh:985715 minrtt:27.494
	 snd_wnd:5974528 rcv_wnd:985728
```

**连接 C：SSH 远程连接（★ 有重传）**
```
ESTAB 0      100    172.27.141.144:22     183.247.2.29:13462
	 cubic wscale:8,7 rto:259 rtt:58.054/3.616 ato:40 mss:1452 pmtu:1500
	 rcvmss:1452 advmss:1460 cwnd:340 ssthresh:340 bytes_sent:10102441
	 bytes_retrans:400 bytes_acked:10101941 bytes_received:489609
	 segs_out:27957 segs_in:20089 data_segs_out:27679 data_segs_in:735
	 send 68Mbps lastsnd:40 lastrcv:56472 lastack:330 pacing_rate 81.6Mbps
	 delivery_rate 202Mbps delivered:27677 app_limited busy:1082755ms
	 rwnd_limited:12ms(0.0%) unacked:1 retrans:0/4 dsack_dups:2 reord_seen:1
	 rcv_rtt:17.34 rcv_space:112040 rcv_ssthresh:864760 minrtt:7.361
	 snd_wnd:1048320 rcv_wnd:864768
```

**连接 D：高延迟连接（海外 API）**
```
ESTAB 0      0      172.27.141.144:60560 160.79.104.10:https
	 cubic wscale:13,7 rto:425 rtt:203.184/14.135 ato:40 mss:1448 pmtu:1500
	 rcvmss:1044 advmss:1448 cwnd:12 bytes_sent:58237 bytes_acked:58238
	 bytes_received:5845 segs_out:293 segs_in:274 data_segs_out:42
	 data_segs_in:17 send 684kbps lastsnd:221012 lastrcv:220805 lastack:251
	 pacing_rate 1.37Mbps delivery_rate 385kbps delivered:43 app_limited
	 busy:1841ms rcv_space:14480 rcv_ssthresh:75245 minrtt:148.281
	 snd_wnd:155648 rcv_wnd:75264
```

### ★ 完整字段注解

```
【拥塞控制算法】
  cubic              → 当前使用 CUBIC 算法（Linux 默认）
                       其他常见：reno（经典）、bbr（Google 高吞吐）
  wscale:9,7         → 窗口缩放因子：发送侧=9，接收侧=7
                       实际窗口 = 通告窗口 × 2^wscale
                       wscale=9 → 放大 512 倍
                       wscale=7 → 放大 128 倍

【定时器】
  rto:228            → 重传超时 = 228ms
                       ★ 计算公式：RTO = SRTT + max(G, 4 × RTTVAR)
                       rto 越大 → RTT 波动越大或 RTT 本身越大
  ato:51             → ACK 超时 = 51ms
                       用于延迟 ACK 机制（收到数据后等 ato 再发 ACK）

【★ RTT（往返时延）— 网络质量核心指标】
  rtt:27.686/0.054   → 平滑 RTT / RTT 方差（ms）

  连接对比：
  ┌──────────┬────────────┬──────────┬──────────────────┐
  │ 连接      │ RTT (ms)   │ RTT 方差  │ 网络质量判断     │
  ├──────────┼────────────┼──────────┼──────────────────┤
  │ 本地回环  │ 1.834      │ 0.75     │ ★ 极好           │
  │ 国内 HTTPS│ 27.686     │ 0.054    │ ★ 好（方差极小） │
  │ SSH 外网  │ 58.054     │ 3.616    │ 一般（方差较大）  │
  │ 海外 API  │ 203.184    │ 14.135   │ ★ 差（高延迟高抖动）│
  └──────────┴────────────┴──────────┴──────────────────┘

  ★ RTT 方差大 → 网络不稳定（抖动大）→ 影响吞吐

【段大小】
  mss:1460           → 最大段大小（以太网 1500 - 40 字节 IP+TCP 头）
  pmtu:1500          → 路径 MTU
  rcvmss:1460        → 接收侧 MSS
  advmss:1460        → 通告给对端的 MSS

  ★ 本地回环 mss=65483 → 因为回环接口没有 MTU 限制
    pmtu=65535 → 回环接口的 MTU

【★ 拥塞窗口 — 吞吐的决定性因素】
  cwnd:1904          → 拥塞窗口 = 1904 段（连接 B）
                       ★ cwnd × MSS = 1904 × 1460 = ~2.7MB
                       即一次 RTT 可发送 2.7MB 数据
  cwnd:340           → 拥塞窗口 = 340 段（SSH 连接 C）
  cwnd:12            → ★ 拥塞窗口仅 12 段（海外连接 D）
                       → 说明此连接经历过丢包，窗口被大幅缩小
  cwnd:10            → 初始窗口（本地回环 A，刚建立或空闲）

  ★ cwnd 与吞吐的关系：
    理论吞吐 ≈ cwnd × MSS / RTT
    连接 B：1904 × 1460 / 0.027686 ≈ 100MB/s = 803Mbps ✓（与 send 一致）
    连接 D：12 × 1448 / 0.203184 ≈ 85KB/s = 684kbps ✓

  ssthresh:340       → ★ 慢启动阈值（仅在有丢包时显示）
    cwnd < ssthresh → 慢启动阶段（指数增长）
    cwnd ≥ ssthresh → 拥塞避免阶段（线性增长）
    连接 C：cwnd=340, ssthresh=340 → 刚好在阈值处

【流量统计】
  bytes_sent:4751459     → 总发送 4.5MB
  bytes_acked:4751460    → 被 ACK 确认 4.5MB
  bytes_received:275906  → 总接收 269KB
  bytes_retrans:400      → ★ 重传 400 字节（连接 C，SSH）
                           占 bytes_sent 的 0.004% → 很轻微
  segs_out:4563          → 发送段数
  segs_in:1636           → 接收段数
  data_segs_out:3272     → 数据段（排除纯 ACK）
  data_segs_in:1293      → 数据段

  ★ bytes_sent >> bytes_received → 这是一个以上传为主的连接

【★ 速率 — 性能诊断核心】
  send 803Mbps           → 理论发送速率（基于 cwnd/RTT 计算）
  pacing_rate 1.61Gbps   → 内核 pacing 上限
  delivery_rate 373Mbps  → ★ 实际交付速率（最接近真实吞吐）
  delivered:3273         → 已交付的数据段数

  ★ 速率对比分析：
  ┌──────────┬──────────┬──────────────┬──────────────┬───────────────────────┐
  │ 连接      │ send     │ delivery_rate │ 比值          │ 判断                  │
  ├──────────┼──────────┼──────────────┼──────────────┼───────────────────────┤
  │ 本地回环  │ 2.86Gbps │ 47.6Gbps     │ > 1          │ 回环无瓶颈            │
  │ 国内 HTTPS│ 803Mbps  │ 373Mbps      │ 46%          │ 正常（burst 效应）    │
  │ SSH 外网  │ 68Mbps   │ 202Mbps      │ > 1          │ burst 效应            │
  │ 海外 API  │ 684kbps  │ 385kbps      │ 56%          │ 受限于高 RTT          │
  └──────────┴──────────┴──────────────┴──────────────┴───────────────────────┘

【限制因素 — ★ 定位瓶颈】
  app_limited            → ★ 吞吐受限于应用层发送速度
                           （不是网络瓶颈，是应用发得不够快）
  rwnd_limited:28ms(5.9%) → 5.9% 的时间受限于对端接收窗口
                             → 对端偶尔读取慢

  ★ 连接 C 的特殊字段：
    unacked:1            → 1 个段未被 ACK
    retrans:0/4          → 当前 0 次重传 / 历史总共 4 次重传
    dsack_dups:2         → 2 次 DSACK 重复
    reord_seen:1         → 1 次乱序检测

【时间戳】
  lastsnd:8412           → 上次发送距今 8412ms（8.4 秒前）
  lastrcv:72             → 上次接收距今 72ms
  lastack:72             → 上次 ACK 距今 72ms
  busy:477ms             → 连接活跃时间

  ★ lastsnd=8412 但 lastrcv=72 → 此连接当前以接收为主
    发送方（本机）暂时没有数据要发

【接收侧】
  rcv_rtt:26852.6        → 接收侧 RTT 估计（26.8ms）
  rcv_space:86531        → 接收缓冲区预留空间
  rcv_ssthresh:985715    → 接收侧慢启动阈值
  minrtt:27.494          → ★ 历史最小 RTT（BBR 用此估算带宽）
  snd_wnd:5974528        → 对端通告的发送窗口 ~5.7MB
  rcv_wnd:985728         → 本地通告的接收窗口 ~963KB

  ★ snd_wnd >> rcv_wnd → 对端有更大的接收能力
    本地接收窗口较小 → 可能限制了对端的发送速度
```

### 诊断要点

| 参数 | 正常值 | 异常信号 | 优化方向 |
|------|--------|---------|---------|
| cwnd | > 100 | < 10 → 严重拥塞 | 检查丢包，考虑 BBR |
| rtt | < 50ms | > 200ms → 高延迟 | 就近部署 |
| rttvar | < rtt/4 | > rtt/2 → 抖动大 | 改善网络质量 |
| retrans | 0 | > 100 → 丢包严重 | 检查链路质量 |
| delivery_rate | 接近 send | << send → 网络瓶颈 | 增大 cwnd 或降低 RTT |
| app_limited | 常见 | 如果吞吐低 + app_limited → 应用问题 | 优化应用发送速度 |

---

## 场景 7：ss -te — 扩展信息

### 命令

```bash
ss -te
```

### 实际输出（选取代表性连接）

```
State Recv-Q Send-Q  Local Address:Port   Peer Address:Port Process
ESTAB 0      0           127.0.0.1:53888     127.0.0.1:34017 ino:4479331 sk:1b cgroup:/user.slice/user-0.slice/session-29.scope <->
ESTAB 0      0      172.27.141.144:46512 39.106.104.16:https timer:(keepalive,55sec,0) ino:4778695 sk:2007 cgroup:/user.slice/user-0.slice/session-1.scope <->
ESTAB 0      108    172.27.141.144:22     183.247.2.29:13462 timer:(on,202ms,0) ino:4480210 sk:20 cgroup:/system.slice/sshd.service <->
ESTAB 0      0      172.27.141.144:57858 100.100.30.25:http  ino:8869 sk:1015 cgroup:/aegis <->
```

### 逐字段注解

```
ino:4479331
  → socket 的 inode 号（在 /proc/PID/fd 中可见）
  → 用于关联 socket 与进程

sk:1b
  → socket 内核地址的简写（用于调试）

cgroup:/user.slice/user-0.slice/session-29.scope
  → ★ socket 所属的 cgroup 路径
  → user.slice → 用户会话
  → session-29.scope → 第 29 个登录会话
  → ★ 用途：在容器/多用户环境中追踪 socket 归属

cgroup:/system.slice/sshd.service
  → SSH 连接属于 sshd 系统服务

cgroup:/aegis
  → ★ AliYunDun 安全客户端有自己的 cgroup

<->
  → 表示双向连接（TCP 默认就是双向的）
```

### 诊断要点

| 字段 | 用途 |
|------|------|
| ino | 与 /proc/PID/fd 关联，定位进程 |
| cgroup | ★ 容器环境中追踪 socket 归属 |
| sk | 内核调试用（一般不需要关注） |

---

## 场景 8：ss state time-wait — TIME_WAIT 分析

### 命令

```bash
ss -tn state time-wait
```

### 实际输出

```
Recv-Q Send-Q  Local Address:Port    Peer Address:Port Process
0      0      172.27.141.144:36622 100.100.100.10:443
0      0      172.27.141.144:56578 100.100.100.10:80
```

### 注解

```
当前状态：
  只有 2 个 TIME_WAIT 连接 → ★ 非常健康
  两个连接都指向同一个目标 IP：100.100.100.10（阿里云元数据服务）
  → 短连接到元数据服务后正常关闭

TIME_WAIT 解读：
  Recv-Q = 0, Send-Q = 0 → TIME_WAIT 状态下这两个字段无意义
  本地端口：36622, 56578 → 临时端口（ephemeral port）
  对端端口：443 (HTTPS), 80 (HTTP) → 短连接到元数据服务

★ 为什么没有 Process 信息？
  TIME_WAIT 的 socket 已经脱离了进程
  进程调用 close() 后，TCP 进入 FIN_WAIT → TIME_WAIT
  此时进程已不再持有该 socket

本机 TIME_WAIT 趋势分析：
  ss -s 显示 timewait 2 → 正常
  如果 timewait > 5000 → 需要关注
  如果 timewait > 28000 → 端口可能耗尽
    （默认端口范围 32768-60999 ≈ 28000 个）
```

### TIME_WAIT 过多的排查命令

```bash
# 按目标 IP 统计 TIME_WAIT
ss state time-wait | awk 'NR>1 {split($4,a,":"); print a[1]}' | sort | uniq -c | sort -rn

# 按目标端口统计
ss state time-wait | awk 'NR>1 {split($4,a,":"); print a[2]}' | sort | uniq -c | sort -rn

# 持续监控 TIME_WAIT 数量
watch -n 2 'ss -s | grep timewait'
```

### 诊断要点

| 数量 | 评估 | 处理 |
|------|------|------|
| < 100 | 正常 | 无需处理 |
| 100-5000 | 较多但可接受 | 关注趋势 |
| 5000-28000 | ★ 异常 | 检查短连接来源，优化为长连接 |
| > 28000 | ★ 危险 | 端口即将耗尽，必须优化 |

---

## 场景 9：ss state close-wait — CLOSE_WAIT 泄漏检测

### 命令

```bash
ss -tn state close-wait
```

### 实际输出

```
Recv-Q Send-Q Local Address:Port Peer Address:Port
```

（空输出，无 CLOSE_WAIT 连接）

### 注解

```
当前状态：
  ★ 无 CLOSE_WAIT 连接 → 正常
  说明没有连接泄漏问题

CLOSE_WAIT 的含义：
  对端已发送 FIN（关闭连接）
  本地 TCP 已回复 ACK
  ★ 但本地应用还未调用 close()
  → 如果长期存在 CLOSE_WAIT → ★ 应用层 Bug

CLOSE_WAIT 泄漏的典型场景：

  ┌───────────────────────────────────────────────────┐
  │ 正常情况：                                         │
  │   对端 FIN → 本地 CLOSE_WAIT → 应用 close() → 消除│
  │                                                    │
  │ 泄漏情况：                                         │
  │   对端 FIN → 本地 CLOSE_WAIT → 应用忘记 close()   │
  │   → ★ CLOSE_WAIT 永久存在                          │
  │   → 占用文件描述符 + 内核内存                      │
  │   → 累积到 ulimit -n 后无法新建连接                │
  └───────────────────────────────────────────────────┘

常见原因：
  1. 异常处理中忘记关闭 socket
  2. 连接池回收逻辑有 Bug
  3. 多线程引用计数不正确
```

### CLOSE_WAIT 排查命令

```bash
# 查看 CLOSE_WAIT + 进程
ss -tnp state close-wait

# 按进程统计 CLOSE_WAIT
ss -tnp state close-wait | awk -F'"' '{print $2}' | sort | uniq -c | sort -rn

# 持续监控
watch -n 5 'ss -tn state close-wait | wc -l'
```

### 诊断要点

| 数量 | 评估 | 处理 |
|------|------|------|
| 0 | ★ 正常 | 无需处理 |
| 1-10 | 瞬时存在 | 正常（应用正在关闭中） |
| > 10 持续存在 | ★ 泄漏 | 定位进程，检查代码 |
| > 100 | ★ 严重泄漏 | 重启进程 + 修复 Bug |

---

## 场景 10：ss 过滤器高级用法

### 10.1 按端口过滤

```bash
# SSH 连接
$ ss -tn 'sport = :22'
State Recv-Q Send-Q  Local Address:Port Peer Address:Port Process
ESTAB 0      0      172.27.141.144:22   183.247.2.29:13462
ESTAB 0      100    172.27.141.144:22   183.247.2.29:13472
```

注解：2 个 SSH 连接，来自同一个客户端 IP，其中一个有 Send-Q 积压。

```bash
# HTTPS 出站连接
$ ss -tn 'dport = :https'
State Recv-Q Send-Q  Local Address:Port   Peer Address:Port Process
ESTAB 0      0      172.27.141.144:46512 39.106.104.16:443
ESTAB 0      0      172.27.141.144:55932 39.106.80.255:443
ESTAB 0      0      172.27.141.144:60560 160.79.104.10:443
ESTAB 0      0      172.27.141.144:35996 39.106.80.255:443
ESTAB 0      0      172.27.141.144:51348 39.106.104.16:443
ESTAB 0      0      172.27.141.144:51356 39.106.104.16:443
ESTAB 0      0      172.27.141.144:46102 39.106.104.16:443
ESTAB 0      0      172.27.141.144:40604 39.106.80.255:443
```

注解：8 个 HTTPS 出站连接，分布在 3 个不同的目标 IP。

### 10.2 按网段过滤

```bash
# 本地回环连接
$ ss -tn 'dst 127.0.0.0/8'
State Recv-Q Send-Q Local Address:Port  Peer Address:Port Process
ESTAB 0      0          127.0.0.1:53888    127.0.0.1:34017
ESTAB 0      87         127.0.0.1:39185    127.0.0.1:36228
ESTAB 0      0          127.0.0.1:34017    127.0.0.1:53888
ESTAB 0      0          127.0.0.1:36228    127.0.0.1:39185
```

注解：4 个本地回环连接（2 对双向连接），其中 1 个有 Send-Q=87 的积压。

### 10.3 按状态过滤

```bash
# TIME_WAIT 连接
$ ss -tn state time-wait
Recv-Q Send-Q  Local Address:Port    Peer Address:Port Process
0      0      172.27.141.144:36622 100.100.100.10:443
0      0      172.27.141.144:56578 100.100.100.10:80

# 已建立连接（注意：state 在选项之后）
$ ss -tn state established
# （输出省略，15 个连接）

# 监听状态
$ ss -tn state listening
# （输出省略，15 个监听端口）
```

### 10.4 组合过滤

```bash
# SSH 端口 + 已建立状态
$ ss -tn state established 'sport = :22'
# 结果：2 个 SSH 连接

# 组合：HTTP 或 HTTPS 端口
$ ss -tn 'dport = :80 or dport = :443'
# 结果：HTTP + HTTPS 出站连接

# 排除本地连接（看外网连接）
$ ss -tn 'not dst 127.0.0.0/8' state established
# 结果：只有外网连接
```

### ★ 过滤器性能建议

```
连接数 < 100：     任意过滤器都可以，性能差异不大
连接数 100-1000：  优先用精确匹配（=），避免范围比较
连接数 > 10000：   ★ 必须用精确匹配过滤器（内核侧执行）
                   避免 or/and 组合（用户态过滤）

最佳实践：
  1. 先用内核侧过滤缩小范围
  2. 再用 grep/awk 做二次过滤

  例：先看所有 ESTAB 连接到某个端口
  ss -tn state established 'dport = :443' | grep 10.0.0
```

---

## 场景 11：★ 网络性能诊断流程

### 完整诊断流程

```
┌─────────────────────────────────────────────────────────┐
│                 网络性能诊断流程                         │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Step 1: 全局概览                                       │
│    ss -s                                                │
│    → 总连接数、各状态分布、TCP 内存                      │
│                                                         │
│  Step 2: 按状态统计                                     │
│    ss -tan | awk 'NR>1{print $1}' | sort | uniq -c     │
│    → 哪个状态异常多？                                    │
│                                                         │
│  Step 3: 分状态深入                                     │
│    TIME_WAIT 多  → 场景 8 的排查命令                    │
│    CLOSE_WAIT 多 → 场景 9 的排查命令                    │
│    ESTAB 异常    → 看 Send-Q/Recv-Q + ss -ti           │
│                                                         │
│  Step 4: TCP 参数分析                                   │
│    ss -ti                                               │
│    → cwnd/rtt/delivery_rate/retrans                     │
│                                                         │
│  Step 5: 内存分析                                       │
│    ss -tm                                               │
│    → skmem 字段 + tcp_mem 参数                          │
│                                                         │
│  Step 6: 结合其他工具                                   │
│    sar -n DEV 1    → 网卡流量                           │
│    mpstat -P ALL 1 → CPU 使用率（si%）                  │
│    /proc/interrupts → 中断分布                          │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 实战演练：诊断本机的网络状态

**Step 1：全局概览**
```bash
$ ss -s
Total: 269
TCP:   32 (estab 15, closed 2, orphaned 0, timewait 2)

Transport Total     IP        IPv6
RAW	  1         0         1
UDP	  7         5         2
TCP	  30        28        2
INET	  38        33        5
FRAG	  0         0         0
```

分析：
- 总 socket 269 → 正常
- estab 15 → 活跃连接数合理
- orphaned 0 → 无泄漏
- timewait 2 → ★ 非常健康

**Step 2：按状态统计**
```bash
$ ss -tan | awk 'NR>1{print $1}' | sort | uniq -c | sort -rn
     15 LISTEN
     15 ESTAB
      2 TIME-WAIT
```

分析：
- LISTEN 15 → 15 个监听端口
- ESTAB 15 → 15 个已建立连接
- TIME-WAIT 2 → 极少

**Step 3：检查异常**

检查 Send-Q 积压：
```bash
$ ss -tn | awk 'NR>1 && $3>0'
State Recv-Q Send-Q  Local Address:Port   Peer Address:Port Process
ESTAB 0      100    172.27.141.144:22     183.247.2.29:13462
ESTAB 0      100    172.27.141.144:22     183.247.2.29:13472
ESTAB 0      87         127.0.0.1:39185    127.0.0.1:36228
```

分析：
- 2 个 SSH 连接有 100 字节积压 → 正常（SSH 延迟 ACK）
- 1 个本地连接有 87 字节积压 → 正常（瞬时值）

检查 Recv-Q 积压：
```bash
$ ss -tn | awk 'NR>1 && $2>0'
State Recv-Q Send-Q  Local Address:Port   Peer Address:Port Process
ESTAB 161    0      172.27.141.144:35996 39.106.80.255:443
```

分析：
- 1 个连接有 161 字节未读 → 正常（应用正在处理）

**Step 4：TCP 参数检查**

重点看 SSH 连接（有积压的连接）：
```bash
$ ss -ti 'sport = :22'
# 连接 C 的完整参数已在场景 6 中分析

# 关键参数：
# cwnd:340 → 窗口正常
# rtt:58ms → 中等延迟
# retrans:0/4 → 历史有 4 次重传，当前无
# bytes_retrans:400 → 很少
```

**Step 5：检查 TIME_WAIT 目标分布**
```bash
$ ss state time-wait
# 2 个 TIME_WAIT 都指向 100.100.100.10（阿里云元数据服务）
# → 正常的短连接行为
```

### 诊断结论

```
本机网络状态：★ 健康

  ✓ 连接数正常（15 estab + 2 timewait）
  ✓ 无孤儿连接（orphaned = 0）
  ✓ TIME_WAIT 极少（2 个）
  ✓ 无 CLOSE_WAIT 泄漏
  ✓ Send-Q/Recv-Q 积压很小
  ✓ 重传次数极少（4 次历史重传）

如有异常，应关注的方向：
  - 如果 estab 暴增 → 检查是否有连接泄漏
  - 如果 timewait > 1000 → 检查短连接来源
  - 如果 retrans 多 → 检查链路质量
  - 如果 cwnd 很小 → 网络拥塞或丢包
```

---

## 场景 12：TIME_WAIT/CLOSE_WAIT 优化方案

### 12.1 TIME_WAIT 过多优化

#### 原因分析

```
TIME_WAIT 产生的条件：
  主动关闭连接的一方会进入 TIME_WAIT
  等待 2 × MSL（Maximum Segment Lifetime）= 60 秒

TIME_WAIT 过多的典型场景：
  1. HTTP 短连接（每次请求新建连接）
  2. 微服务间 RPC 调用（无连接池）
  3. 健康检查过于频繁
```

#### 诊断命令

```bash
# 1. 查看 TIME_WAIT 数量
ss -s | grep timewait

# 2. 按目标 IP 统计
ss state time-wait | awk 'NR>1{split($4,a,":");print a[1]}' | sort | uniq -c | sort -rn

# 3. 按目标端口统计
ss state time-wait | awk 'NR>1{split($4,a,":");print a[2]}' | sort | uniq -c | sort -rn

# 4. 监控端口使用情况
cat /proc/sys/net/ipv4/ip_local_port_range
# 默认：32768 60999 → 约 28000 个可用端口
```

#### 优化方案

```
┌────────────────────────────────────────────────────────────────┐
│                TIME_WAIT 优化方案                               │
├───────────────┬────────────────────────────────────────────────┤
│ 方案           │ 说明                                          │
├───────────────┼────────────────────────────────────────────────┤
│ ★ 应用层优化   │                                               │
│  连接池        │ 使用连接池复用 TCP 连接                        │
│  HTTP Keep-Alive│ HTTP 请求复用连接（Connection: keep-alive）  │
│  gRPC 长连接   │ gRPC 默认使用 HTTP/2 多路复用                 │
├───────────────┼────────────────────────────────────────────────┤
│ ★ 内核参数优化 │                                               │
│  tcp_tw_reuse  │ echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse    │
│                │ 允许复用 TIME_WAIT 端口（安全）               │
│                │ ★ 注意：内核 4.12+ 默认值=2                   │
│                │   值=0：禁用                                  │
│                │   值=1：全局启用                              │
│                │   值=2：仅 loopback（当前默认）               │
├───────────────┼────────────────────────────────────────────────┤
│  tcp_max_tw_   │ echo 5000 > /proc/sys/net/ipv4/              │
│  buckets       │   tcp_max_tw_buckets                          │
│                │ 限制 TIME_WAIT 最大数量（超出直接回收）       │
├───────────────┼────────────────────────────────────────────────┤
│ tcp_fin_timeout│ echo 30 > /proc/sys/net/ipv4/tcp_fin_timeout │
│                │ 缩短 FIN_WAIT_2 超时（默认 60 秒）           │
│                │ ★ 注意：不影响 TIME_WAIT 持续时间             │
├───────────────┼────────────────────────────────────────────────┤
│ 扩大端口范围   │ echo "1024 65535" > /proc/sys/net/ipv4/      │
│                │   ip_local_port_range                          │
│                │ 增加可用端口数（从 ~28000 到 ~64000）         │
└───────────────┴────────────────────────────────────────────────┘

★ 本机当前配置：
  tcp_tw_reuse = 2（仅 loopback 启用）
  tcp_fin_timeout = 60（默认值）
```

### 12.2 CLOSE_WAIT 泄漏处理

#### 诊断命令

```bash
# 1. 查看 CLOSE_WAIT 数量
ss -tn state close-wait | wc -l

# 2. 定位泄漏进程
ss -tnp state close-wait

# 3. 查看进程的 fd 使用情况
ls -la /proc/<PID>/fd | grep socket | wc -l

# 4. 查看进程的 socket 限制
cat /proc/<PID>/limits | grep "open files"
```

#### 处理方案

```
CLOSE_WAIT 泄漏处理流程：

  1. 临时措施：重启泄漏进程
     systemctl restart <service>
     → 释放所有 CLOSE_WAIT socket

  2. 定位根因：
     a. 查看应用日志，找到连接关闭相关的错误
     b. 检查代码中 socket 的 close() 调用是否在所有分支中执行
     c. 特别注意异常处理路径（try/catch/finally）
     d. 检查连接池配置是否正确

  3. 代码修复示例（Java）：
     // 修复前（泄漏）
     try {
         Socket socket = new Socket(host, port);
         // ... 使用 socket
     } catch (IOException e) {
         log.error(e);  // 忘记关闭 socket！
     }

     // 修复后
     try (Socket socket = new Socket(host, port)) {
         // ... 使用 socket
     } catch (IOException e) {
         log.error(e);
     }  // try-with-resources 自动关闭

  4. 监控：
     在监控系统中添加 CLOSE_WAIT 告警
     ss -tn state close-wait | wc -l > 阈值时触发告警
```

### 12.3 综合优化检查清单

```
┌────────────────────────────────────────────────────────────────┐
│              网络连接优化检查清单                                │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│  □ ss -s 检查全局统计                                          │
│    - estab 数量是否合理？                                      │
│    - timewait 数量是否在可接受范围？                            │
│    - orphaned 是否为 0？                                       │
│                                                                │
│  □ ss -tan | awk 检查状态分布                                  │
│    - 是否有异常的 CLOSE_WAIT/FIN_WAIT？                        │
│                                                                │
│  □ ss -ti 检查 TCP 参数                                       │
│    - cwnd 是否太小？                                           │
│    - rtt 是否符合预期？                                        │
│    - retrans 是否过多？                                        │
│                                                                │
│  □ ss -tm 检查内存                                             │
│    - d（dropped）是否为 0？                                    │
│    - TCP 总内存是否接近 tcp_mem[1]？                           │
│                                                                │
│  □ 检查内核参数                                                │
│    - tcp_tw_reuse                                              │
│    - tcp_fin_timeout                                           │
│    - somaxconn                                                 │
│    - tcp_max_syn_backlog                                       │
│    - ip_local_port_range                                       │
│                                                                │
│  □ 检查应用层                                                  │
│    - 是否使用连接池？                                          │
│    - HTTP Keep-Alive 是否启用？                                │
│    - socket 是否正确关闭？                                     │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```

---

## 附录：其他协议输出

### UDP 监听

```bash
$ ss -ulnp
State  Recv-Q Send-Q Local Address:Port Peer Address:Port Process
UNCONN 0      0         127.0.0.54:53        0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=18))
UNCONN 0      0      127.0.0.53%lo:53        0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=16))
UNCONN 0      0          127.0.0.1:323       0.0.0.0:*    users:(("chronyd",pid=693,fd=5))
UNCONN 0      0            0.0.0.0:5355      0.0.0.0:*    users:(("systemd-resolve",pid=646,fd=10))
UNCONN 0      0              [::1]:323          [::]:*    users:(("chronyd",pid=693,fd=6))
UNCONN 0      0               [::]:5355         [::]:*    users:(("systemd-resolve",pid=646,fd=12))
```

注解：
- UNCONN → UDP 是无连接的，所以状态显示 UNCONN（不是 ESTAB）
- 323 → chronyd（NTP 客户端）
- 53/5355 → systemd-resolved（DNS 解析）

### RAW socket

```bash
$ ss -wlnp
State  Recv-Q Send-Q Local Address:Port Peer Address:Port Process
UNCONN 0      0                  *:58              *:*    users:(("NetworkManager",pid=738,fd=26))
```

注解：
- Port 58 → ICMP 协议号（不是端口号）
- NetworkManager 使用 RAW socket 接收 ICMP（用于网络探测）

### Unix domain socket（截取）

```bash
$ ss -xlnp | head -8
Netid State  Recv-Q Send-Q                                                       Local Address:Port    Peer Address:Port Process
u_str LISTEN 0      511                                 /run/user/0/vscode-git-8695dbc367.sock 4479698            * 0    users:(("MainThread",pid=433385,fd=27))
u_str LISTEN 0      511                                 /run/user/0/vscode-git-282c6b00b9.sock 4482525            * 0    users:(("MainThread",pid=433384,fd=27))
u_str LISTEN 0      128                                                    /tmp/tmux-0/default 115174             * 0    users:(("tmux: server",pid=43055,fd=6))
```

注解：
- u_str → Unix STREAM socket（面向连接）
- u_dgr → Unix DGRAM socket（无连接）
- Local Address → socket 文件路径
- Port → inode 号（不是真正的端口）
