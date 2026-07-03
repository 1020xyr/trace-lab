# ss 内部机制 — netlink、内核数据结构与过滤器

```
======================================================================
Step 1: ss 内部机制
======================================================================
阅读要点：
  1. ss 如何通过 netlink INET_DIAG 从内核获取 socket 信息
  2. 内核 struct sock / struct tcp_sock 的关键字段
  3. ss 过滤器语法的设计原理
  4. ss 输出字段的完整解读
预计阅读时间：15 分钟
======================================================================
```

---

## 目录

- [1. ss 的数据获取路径](#1-ss-的数据获取路径)
- [2. netlink INET_DIAG 协议](#2-netlink-inet_diag-协议)
- [3. 内核 socket 数据结构](#3-内核-socket-数据结构)
- [4. ss 过滤器语法](#4-ss-过滤器语法)
- [5. ss 输出字段详解](#5-ss-输出字段详解)
- [6. strace 实测验证](#6-strace-实测验证)

---

## 1. ss 的数据获取路径

### 整体数据流

```
┌───────────────────────────────────────────────────────────────────┐
│                        ss 数据获取全流程                           │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ① ss 启动                                                       │
│     │                                                             │
│     ▼                                                             │
│  ② socket(AF_NETLINK, SOCK_DGRAM, NETLINK_INET_DIAG)            │
│     │  创建 netlink socket（与内核通信的专用通道）                  │
│     ▼                                                             │
│  ③ bind(netlink_sockaddr)                                        │
│     │  绑定本地 netlink 地址                                      │
│     ▼                                                             │
│  ④ sendmsg(inet_diag_req_v2)                                     │
│     │  发送查询请求：                                              │
│     │  - 协议族（AF_INET / AF_INET6）                             │
│     │  - 协议（IPPROTO_TCP / IPPROTO_UDP）                        │
│     │  - 状态掩码（哪些 TCP 状态需要返回）                         │
│     │  - 扩展属性（需要 tcp_info / skmeminfo / inet_diag_meminfo）│
│     ▼                                                             │
│  ⑤ recvmsg() × N 次                                              │
│     │  内核返回批量 inet_diag_msg 结构体                           │
│     │  每次 recvmsg 可返回多条记录                                 │
│     │  ★ 过滤在内核侧执行 → 用户态只收到匹配的数据                │
│     ▼                                                             │
│  ⑥ ss 解析 + 格式化输出                                          │
│     │  解析 inet_diag_msg + 附属的 nlattr（tcp_info / skmeminfo） │
│     │  按用户指定的格式输出                                        │
│     ▼                                                             │
│  ⑦ close(netlink_socket)                                         │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

### 关键优化点

| 优化 | 说明 |
|------|------|
| ★ 内核侧过滤 | 如果用户指定了 state/port/address 过滤器，ss 将过滤条件编码到 netlink 请求中，内核只返回匹配的结果 |
| 批量返回 | 一次 recvmsg 可以接收多条 inet_diag_msg，减少系统调用次数 |
| 二进制协议 | netlink 使用结构体序列化，无需文本解析 |
| 按需扩展 | 只在用户请求 `-i/-m/-o/-e` 时才请求额外的扩展属性 |

---

## 2. netlink INET_DIAG 协议

### netlink 概述

netlink 是 Linux 内核提供的一种**进程间通信（IPC）机制**，专门用于内核与用户态之间的数据交换。

```
netlink 与其他 IPC 的对比：

  ioctl        → 适合简单查询（1 个参数进，1 个结果出）
  procfs       → 适合只读展示（但大文件遍历效率低）
  sysfs        → 适合设备属性（但不适合批量查询）
  ★ netlink    → 适合批量、双向、结构化的内核-用户态通信

  netlink 的特点：
  - 基于 socket 接口（AF_NETLINK）
  - 消息格式：nlmsghdr（16 字节头）+ payload
  - 支持多播（内核主动通知用户态）
  - 支持子协议族（ROUTE / INET_DIAG / AUDIT / ...）
```

### INET_DIAG 子协议

| 概念 | 说明 |
|------|------|
| NETLINK_INET_DIAG | netlink 子协议号（4），专门用于 socket 诊断 |
| SOCK_DIAG_BY_FAMILY | 查询命令，按协议族查询 socket |
| inet_diag_req_v2 | 请求结构体，包含协议族、协议、状态掩码、扩展标志 |
| inet_diag_msg | 响应结构体，包含 socket 基本信息 |
| INET_DIAG_MEMINFO | 扩展属性：socket 内存信息 |
| INET_DIAG_INFO | 扩展属性：tcp_info（拥塞控制参数） |
| INET_DIAG_VEGASINFO | 扩展属性：TCP Vegas/BBR 参数 |
| INET_DIAG_SKMEMINFO | 扩展属性：详细的 sk_mem 信息 |

### 请求结构体（inet_diag_req_v2）

```c
/*
 * 内核头文件：include/uapi/linux/inet_diag.h
 * ss 构建此结构体，通过 sendmsg 发送给内核
 */
struct inet_diag_req_v2 {
    __u8    sdiag_family;    /* AF_INET 或 AF_INET6 */
    __u8    sdiag_protocol;  /* IPPROTO_TCP / IPPROTO_UDP / IPPROTO_RAW */
    __u8    idiag_ext;       /* ★ 扩展标志位：请求哪些额外信息 */
    __u8    pad;
    __u32   idiag_states;    /* ★ TCP 状态掩码（位图） */
    struct inet_diag_sockid id; /* 过滤条件：地址、端口 */
};

/*
 * idiag_ext 标志位（可按位或组合）：
 *   INET_DIAG_MEMINFO    (1 << 0)  → 请求 sk_meminfo
 *   INET_DIAG_INFO       (1 << 1)  → 请求 tcp_info
 *   INET_DIAG_VEGASINFO  (1 << 2)  → 请求 tcpvegas_info
 *   INET_DIAG_TOS        (1 << 3)  → 请求 TOS
 *   INET_DIAG_TCLASS     (1 << 4)  → 请求 TCLASS
 *   INET_DIAG_SKMEMINFO  (1 << 5)  → 请求详细 sk_mem
 *   INET_DIAG_SHUTDOWN   (1 << 6)  → 请求 shutdown 状态
 *
 * 对应 ss 参数：
 *   ss -m → 设置 INET_DIAG_MEMINFO
 *   ss -i → 设置 INET_DIAG_INFO + INET_DIAG_VEGASINFO
 *   ss -e → 设置 INET_DIAG_TOS + INET_DIAG_TCLASS + ...
 */

/*
 * idiag_states 状态掩码（位图）：
 *   每个 TCP 状态对应一位：
 *   bit 0  = TCP_ESTABLISHED (1)
 *   bit 1  = TCP_SYN_SENT    (2)
 *   bit 2  = TCP_SYN_RECV    (3)
 *   ...
 *   bit 10 = TCP_CLOSE       (7)
 *
 *   ★ 全状态 = (1 << 11) - 1 = 0x7FF
 *   ★ ss state established → idiag_states = (1 << 0) = 1
 *   ★ ss state time-wait   → 需要额外的 TCP_TIME_WAIT 位
 */
```

### 响应结构体（inet_diag_msg）

```c
/*
 * 内核为每个匹配的 socket 返回一个 inet_diag_msg
 */
struct inet_diag_msg {
    __u8    idiag_family;    /* AF_INET 或 AF_INET6 */
    __u8    idiag_state;     /* TCP 状态（1-11） */
    __u8    idiag_timer;     /* ★ 定时器类型（0=无，1=重传，2=keepalive） */
    __u8    idiag_retrans;   /* 重传次数 */

    struct inet_diag_sockid id; /* 五元组：src_ip, src_port, dst_ip, dst_port */

    __u32   idiag_expires;   /* 定时器剩余时间（ms） */
    __u32   idiag_rqueue;    /* ★ Recv-Q：接收队列字节数 */
    __u32   idiag_wqueue;    /* ★ Send-Q：发送队列字节数（或 backlog） */
    __u32   idiag_uid;       /* socket 所有者 UID */
    __u32   idiag_inode;     /* socket 的 inode 号 */

    /* 后面跟着 nlattr 扩展属性（tcp_info / skmeminfo 等） */
};

/*
 * ★ idiag_timer 值含义：
 *   0 = 无定时器
 *   1 = 重传定时器（retransmit）→ ss 显示 "timer:(on,...)"
 *   2 = keepalive 定时器       → ss 显示 "timer:(keepalive,...)"
 *   3 = TIME_WAIT 定时器
 *   4 = 零窗口探测器
 */

/*
 * ★ idiag_wqueue 的双重含义：
 *   - 当 idiag_state = TCP_LISTEN 时：backlog 上限（listen 的第二个参数）
 *   - 当 idiag_state ≠ TCP_LISTEN 时：发送队列中未 ACK 的字节数
 */
```

### inet_diag_sockid（五元组）

```c
struct inet_diag_sockid {
    __be16  idiag_sport;     /* 源端口（网络字节序） */
    __be16  idiag_dport;     /* 目的端口 */
    __be32  idiag_src[4];    /* 源 IP（IPv4 用前 4 字节，IPv6 用 16 字节） */
    __be32  idiag_dst[4];    /* 目的 IP */
    __u32   idiag_if;        /* 网络接口索引 */
    __u32   idiag_cookie[2]; /* socket 标识（内部用） */
};
```

---

## 3. 内核 socket 数据结构

### struct sock — 通用 socket 结构

```c
/*
 * 简化版 — 只展示 ss 关心的字段
 * 完整定义：include/net/sock.h
 */
struct sock {
    /* === 基本信息 === */
    struct sock_common  __sk_common;  /* 地址、端口、状态 */
    /* __sk_common 包含：
     *   skc_dport        目的端口
     *   skc_rcv_saddr    源 IP
     *   skc_num          源端口
     *   skc_state        TCP 状态
     *   skc_family       协议族
     *   skc_net          网络命名空间
     */

    /* === 缓冲区 === */
    struct sk_buff_head sk_receive_queue;  /* ★ 接收队列（Recv-Q） */
    struct sk_buff_head sk_write_queue;    /* ★ 发送队列（Send-Q） */

    int                 sk_rcvbuf;    /* 接收缓冲区大小（字节） */
    int                 sk_sndbuf;    /* 发送缓冲区大小（字节） */

    /* === 内存管理 === */
    struct sk_meminfo   sk_meminfo;   /* ★ 详细内存统计 */
    /* 包含：
     *   rmem_alloc    接收已分配内存
     *   rcv_buf       接收缓冲区上限
     *   wmem_alloc    发送已分配内存
     *   snd_buf       发送缓冲区上限
     *   fwd_alloc     前向分配内存
     *   wmem_queued   发送排队内存
     *   optmem        选项内存
     *   back_log      backlog 内存
     *   sock_drop     丢包计数
     */

    /* === 定时器 === */
    struct timer_list   sk_timer;     /* keepalive 定时器 */
    unsigned long       sk_rcvtimeo;  /* 接收超时 */

    /* === 进程关联 === */
    kuid_t              sk_uid;       /* socket 所有者 UID */
    struct socket       *sk_socket;   /* 上层 socket 结构 */
};
```

### struct tcp_sock — TCP 专用扩展

```c
/*
 * tcp_sock 继承自 sock，增加 TCP 特有字段
 * 完整定义：include/linux/tcp.h
 */
struct tcp_sock {
    /* struct inet_connection_sock inet_conn; */  /* 继承链 */
    /* inet_conn 包含 struct sock sk; */

    /* === 拥塞控制 === */
    u32    snd_cwnd;        /* ★ 拥塞窗口（段数）→ ss -i 的 cwnd */
    u32    snd_ssthresh;    /* ★ 慢启动阈值 → ss -i 的 ssthresh */
    u32    mss_cache;       /* 当前 MSS → ss -i 的 mss */
    u32    snd_wnd;         /* 对端通告的接收窗口 */
    u32    rcv_wnd;         /* 本地通告给对端的接收窗口 */

    /* === RTT 测量 === */
    u32    srtt_us;         /* ★ 平滑 RTT（微秒，左移 3 位存储） */
    u32    mdev_us;         /* ★ RTT 方差（微秒） */
    u32    rtt_seq;         /* RTT 测量用的序列号 */
    u32    rcv_rtt_est;     /* 接收侧 RTT 估计 */

    /* === 重传 === */
    u8     retransmits;     /* 当前重传次数 */
    u32    total_retrans;   /* 总重传次数 → ss -i 的 retrans */
    u32    bytes_retrans;   /* 总重传字节数 → ss -i 的 bytes_retrans */

    /* === 流量统计 === */
    u64    bytes_sent;      /* 总发送字节数 → ss -i 的 bytes_sent */
    u64    bytes_acked;     /* 总 ACK 字节数 → ss -i 的 bytes_acked */
    u64    bytes_received;  /* 总接收字节数 → ss -i 的 bytes_received */
    u32    segs_out;        /* 总发送段数 */
    u32    segs_in;         /* 总接收段数 */
    u32    data_segs_out;   /* 数据段发送数 */
    u32    data_segs_in;    /* 数据段接收数 */

    /* === 拥塞控制算法状态 === */
    /* 通过 getsockopt(TCP_CC_INFO) 获取 */
    /* BBR: bw, min_rtt, cwnd_gain, pacing_gain */
    /* CUBIC: beta, c, K, origin_point, ... */
};

/*
 * ★ tcp_info 结构体（通过 getsockopt(TCP_INFO) 获取）：
 *
 * ss -i 输出的大部分字段来自 tcp_info：
 *   tcpi_rtt          → rtt 值
 *   tcpi_rttvar       → rttvar 值
 *   tcpi_snd_cwnd     → cwnd
 *   tcpi_snd_mss      → mss
 *   tcpi_snd_ssthresh → ssthresh（如果有）
 *   tcpi_rcv_rtt      → rcv_rtt
 *   tcpi_rcv_space    → rcv_space
 *   tcpi_total_retrans → retrans
 *   tcpi_pacing_rate  → pacing_rate
 *   tcpi_delivery_rate → delivery_rate
 *   tcpi_bytes_sent   → bytes_sent
 *   tcpi_bytes_acked  → bytes_acked
 *   tcpi_bytes_received → bytes_received
 */
```

### 结构体关系图

```
┌────────────────────────────────────────────────────────────────┐
│                    socket 结构体继承链                          │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│  struct sock                    ← 通用 socket（地址、缓冲区）   │
│      │                                                         │
│      ▼                                                         │
│  struct inet_sock               ← 增加 IPv4/IPv6 地址信息      │
│      │                                                         │
│      ▼                                                         │
│  struct inet_connection_sock    ← 增加连接管理（accept_queue）  │
│      │                                                         │
│      ▼                                                         │
│  struct tcp_sock                ← ★ TCP 专用（拥塞、RTT、统计） │
│                                                                │
│  ss 访问路径：                                                  │
│  - 基本信息 → inet_diag_msg（直接从 sock 提取）                │
│  - TCP 参数 → tcp_info（通过 getsockopt 或 netlink 扩展）      │
│  - 内存信息 → sk_meminfo（从 sock 的内存统计提取）             │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```

---

## 4. ss 过滤器语法

### 过滤器架构

ss 的过滤器在**内核侧执行**（部分过滤），这是性能优势的关键。

```
用户输入过滤器                    内核执行过滤
───────────────                  ────────────────

ss -tn 'sport = :22'     →    netlink 请求中编码：
  state established            idiag_states = (1 << TCP_ESTABLISHED)
  sport = :22                  idiag_sockid.idiag_sport = 22
                               → 内核只返回匹配的 socket
                               → 用户态收到的数据量大幅减少

ss -tn 'dport = :443'    →    同上，过滤目的端口

ss -tn 'dst 10.0.0.0/8'  →    idiag_sockid.idiag_dst = 10.0.0.0
                               + 前缀长度 = 8
                               → 内核过滤目的网段

ss 'state time-wait'     →    idiag_states = (1 << TCP_TIME_WAIT)
                               → 只返回 TIME_WAIT 状态的 socket
```

### 过滤器完整语法

```
ss [选项] [过滤器表达式]

过滤器表达式语法：
  [state <tcp-state>]
  [<condition> [and|or <condition>...]]

condition :=
  dst <address>[/prefix]    目的地址匹配
  src <address>[/prefix]    源地址匹配
  dport <operator> <port>   目的端口匹配
  sport <operator> <port>   源端口匹配
  dev <operator> <device>   网络设备匹配

operator :=
  =   等于
  !=  不等于
  >   大于
  <   小于
  >=  大于等于
  <=  小于等于

tcp-state :=
  established      TCP_ESTABLISHED (1)
  syn-sent         TCP_SYN_SENT    (2)
  syn-recv         TCP_SYN_RECV    (3)
  fin-wait-1       TCP_FIN_WAIT1   (4)
  fin-wait-2       TCP_FIN_WAIT2   (5)
  time-wait        TCP_TIME_WAIT   (6)
  closed           TCP_CLOSE       (7)
  close-wait       TCP_CLOSE_WAIT  (8)
  last-ack         TCP_LAST_ACK    (9)
  listening        TCP_LISTEN      (10)
  closing          TCP_CLOSING     (11)
  all              所有状态（0x7FF 掩码）

address :=
  IPv4:  10.0.0.1, 192.168.0.0/16
  IPv6:  ::1, fe80::/10
  通配:  *

port :=
  数字:  22, 80, 443
  名称:  ssh, http, https（从 /etc/services 解析）
```

### 过滤器示例表

| 过滤器 | 含义 | 内核侧行为 |
|--------|------|-----------|
| `state established` | 只看已建立连接 | idiag_states 只设 ESTAB 位 |
| `state time-wait` | 只看 TIME_WAIT | idiag_states 只设 TW 位 |
| `sport = :22` | 源端口是 22 | 过滤 skc_num == 22 |
| `dport = :https` | 目的端口是 443 | 过滤 skc_dport == 443 |
| `dst 10.0.0.0/8` | 目的网段 10.x | 过滤 dst 地址前缀 |
| `src 192.168.1.0/24` | 源网段 | 过滤 src 地址前缀 |
| `sport >= :1024` | 源端口 ≥ 1024 | 用户态过滤（非内核侧） |
| `dport = :443 or dport = :80` | HTTPS 或 HTTP | 用户态过滤 |
| `not state listening` | 排除监听状态 | idiag_states 排除 LISTEN |

### ★ 内核侧过滤 vs 用户态过滤

```
内核侧过滤（快）：
  - state <tcp-state>       → 直接编码到 idiag_states 位图
  - sport = :<port>         → 编码到 idiag_sockid
  - dport = :<port>         → 编码到 idiag_sockid
  - dst <exact-address>     → 编码到 idiag_sockid
  - src <exact-address>     → 编码到 idiag_sockid

用户态过滤（慢，但更灵活）：
  - 范围比较（>、<、>=、<=）
  - or / and 组合
  - CIDR 前缀匹配（非精确地址）
  - not 取反
  - dev 设备匹配

★ 实践建议：
  对于大连接数场景，尽量使用精确匹配过滤器（内核侧执行）
  先用精确过滤缩小范围，再用 grep/awk 做二次过滤
```

---

## 5. ss 输出字段详解

### 基本输出字段

```
State  Recv-Q  Send-Q  Local Address:Port  Peer Address:Port  Process
─────────────────────────────────────────────────────────────────────

★ State（TCP 状态）：
  LISTEN        监听中（等待连接）
  ESTAB         已建立（数据传输中）
  TIME-WAIT     主动关闭后等待 2MSL
  CLOSE-WAIT    被动关闭，等待应用层 close()
  FIN-WAIT-1    已发 FIN，等待 ACK
  FIN-WAIT-2    已收 ACK，等待对方 FIN
  SYN-SENT      已发 SYN，等待 SYN+ACK
  SYN-RECV      已收 SYN，等待 ACK
  LAST-ACK      被动关闭，等待最后一个 ACK
  CLOSING       双方同时关闭
  CLOSED        连接已关闭（极少显示）
  UNCONN        UDP/RAW socket 的状态（未连接）

★ Recv-Q（接收队列）：
  对于 LISTEN：  当前 accept 队列中已完成三次握手的连接数
                  ★ 如果 Recv-Q 接近 Send-Q（backlog）→ accept 太慢！
  对于 ESTAB：   接收缓冲区中未被应用层 read() 的字节数
                  ★ 如果持续非零 → 应用层读取太慢

★ Send-Q（发送队列）：
  对于 LISTEN：  listen() 的 backlog 参数（accept 队列上限）
                  ★ 受 min(backlog, somaxconn) 限制
  对于 ESTAB：   发送缓冲区中未被对端 ACK 的字节数
                  ★ 如果持续非零 → 网络拥塞或对端接收窗口满

★ Local Address:Port：
  格式：<IP>:<Port>
  0.0.0.0:*  → 监听所有接口
  *:Port     → 监听所有接口（IPv6）
  127.0.0.1  → 仅本地回环

★ Peer Address:Port：
  0.0.0.0:*  → LISTEN 状态时对端不确定
  具体 IP:Port → 已建立连接的对端

★ Process：
  格式：users:(("进程名",pid=PID,fd=FD))
  需要 root 权限才能看到其他用户的进程
  一个 socket 可能被多个进程共享（fork 后）
```

### timer 字段（-o 选项）

```
timer:(<type>,<remaining>,<retrans>)

type：
  keepalive    keepalive 定时器（TCP 保活）
  on           重传定时器（有未 ACK 的数据）
  timewait     TIME_WAIT 定时器
  persist      零窗口探测定时器

remaining：
  定时器剩余时间（如 52sec, 228ms, 42min）
  空字符串 → 定时器刚触发或未设置

retrans：
  已重传次数（0 = 无重传）

示例解读：
  timer:(keepalive,52sec,0)   → keepalive 定时器，52 秒后触发，无重传
  timer:(on,228ms,0)          → 重传定时器，228ms 后超时，无重传
  timer:(keepalive,42min,0)   → keepalive 定时器，42 分钟后触发
  timer:(keepalive,,0)        → keepalive 定时器刚触发
```

### skmem 字段（-m 选项）

```
skmem:(r<rmem_alloc>,rb<rcv_buf>,t<wmem_alloc>,tb<snd_buf>,
       f<fwd_alloc>,w<wmem_queued>,o<optmem>,bl<back_log>,d<dropped>)

字段详解：

| 字段 | 含义 | 单位 | 说明 |
|------|------|------|------|
| r    | rmem_alloc | 字节 | 接收缓冲区已分配的内存 |
| rb   | rcv_buf | 字节 | ★ 接收缓冲区上限（内核自动调整） |
| t    | wmem_alloc | 字节 | 发送缓冲区已分配的内存 |
| tb   | snd_buf | 字节 | ★ 发送缓冲区上限（内核自动调整） |
| f    | fwd_alloc | 字节 | 前向分配（预分配的内存页） |
| w    | wmem_queued | 字节 | 发送队列中排队的数据量 |
| o    | optmem | 字节 | socket 选项占用的内存 |
| bl   | back_log | 字节 | backlog 队列占用的内存 |
| d    | dropped | 计数 | 因内存不足丢弃的包数 |

★ 示例解读：
  skmem:(r0,rb1898851,t0,tb2626560,f0,w0,o0,bl0,d0)
  → r=0：接收缓冲区空闲
  → rb=1898851（~1.8MB）：接收缓冲区上限
  → t=0：发送缓冲区空闲
  → tb=2626560（~2.5MB）：发送缓冲区上限
  → d=0：无丢包

  skmem:(r0,rb2944302,t0,tb948736,f3100,w996,o0,bl0,d0)
  → f=3100：有 3100 字节前向分配
  → w=996：发送队列有 996 字节排队（Send-Q 积压）
```

### TCP 详细参数（-i 选项）

```
ss -i 输出示例（单行）：
  cubic wscale:9,7 rto:228 rtt:27.686/0.054 ato:51 mss:1460 pmtu:1500
  rcvmss:1460 advmss:1460 cwnd:1904 bytes_sent:4751459 bytes_acked:4751460
  bytes_received:275906 segs_out:4563 segs_in:1636 data_segs_out:3272
  data_segs_in:1293 send 803Mbps lastsnd:8412 lastrcv:72 lastack:72
  pacing_rate 1.61Gbps delivery_rate 373Mbps delivered:3273 app_limited
  busy:477ms rwnd_limited:28ms(5.9%) dsack_dups:1
  rcv_rtt:26852.6 rcv_space:86531 rcv_ssthresh:985715 minrtt:27.494
  snd_wnd:5974528 rcv_wnd:985728

字段分组解读：

【拥塞控制算法】
  cubic          当前使用的拥塞控制算法（cubic/reno/bbr/...）
  wscale:9,7     窗口缩放因子（发送侧,接收侧）

【定时器与 RTT】
  rto:228        重传超时时间（ms），基于 rtt + 4×rttvar 计算
  rtt:27.686/0.054  平滑RTT/RTT方差（ms）
  ato:51         ACK 超时时间（ms），用于延迟 ACK
  minrtt:27.494  最小 RTT（ms），BBR 用此估算带宽

【段大小】
  mss:1460       当前 MSS（最大段大小）
  pmtu:1500      路径 MTU
  rcvmss:1460    接收侧 MSS
  advmss:1460    通告给对端的 MSS

【★ 拥塞窗口 — 性能分析核心参数】
  cwnd:1904      ★ 拥塞窗口（段数），越大吞吐越高
  ssthresh:340   ★ 慢启动阈值
                 cwnd < ssthresh → 慢启动阶段
                 cwnd ≥ ssthresh → 拥塞避免阶段

【流量统计】
  bytes_sent:4751459     总发送字节数
  bytes_acked:4751460    被 ACK 的字节数
  bytes_received:275906  总接收字节数
  bytes_retrans:400      ★ 重传字节数（如果有）
  segs_out:4563          发送段数
  segs_in:1636           接收段数
  data_segs_out:3272     数据段（排除纯 ACK）
  data_segs_in:1293      数据段

【速率 — ★ 性能诊断关键】
  send 803Mbps           理论发送速率（基于 cwnd × MSS / RTT）
  pacing_rate 1.61Gbps   内核 pacing 速率上限
  delivery_rate 373Mbps  ★ 实际交付速率（最接近真实吞吐）
  delivered:3273         已交付的数据段数

【时间戳】
  lastsnd:8412           上次发送距今（ms）
  lastrcv:72             上次接收距今（ms）
  lastack:72             上次 ACK 距今（ms）
  busy:477ms             连接活跃时间

【限制因素】
  app_limited            ★ 标记：吞吐受限于应用层发送速度
  rwnd_limited:28ms(5.9%)  ★ 受限于对端接收窗口的时间和比例
  busy:477ms             连接忙碌时间

【重传与丢包】
  dsack_dups:1           DSACK 重复 ACK
  retrans:0/4            当前重传/总重传
  reord_seen:1           乱序检测次数

【接收侧】
  rcv_rtt:26852.6        接收侧 RTT 估计
  rcv_space:86531        接收缓冲区预留空间
  rcv_ssthresh:985715    接收侧慢启动阈值
  snd_wnd:5974528        对端通告的发送窗口
  rcv_wnd:985728         本地通告的接收窗口
  unacked:1              未 ACK 的段数
```

---

## 6. strace 实测验证

### 实验环境

```
系统：Alibaba Cloud Linux 4
内核：6.6.102-5.3.1.alnx4.x86_64
iproute2 版本：6.6.0
连接数：约 30 个 TCP 连接
```

### ss 的系统调用（strace 实测）

```
strace -c -e trace=network,file ss -tn

结果：
  % time     seconds  usecs/call     calls    errors syscall
  ------ ----------- ----------- --------- --------- ----------------
   45.74    0.000569         569         1           execve
   24.84    0.000309         154         2           sendmsg     ← netlink 请求
   13.42    0.000167          23         7           openat      ← 配置文件
    5.31    0.000066           8         8           newfstatat
    4.10    0.000051           8         6           recvmsg     ← netlink 响应
    1.69    0.000021          10         2         2 access
    1.53    0.000019           9         2           statfs
    1.29    0.000016           5         3           setsockopt  ← 配置 netlink
    1.21    0.000015          15         1           socket      ← 创建 netlink socket
    0.48    0.000006           6         1           bind        ← 绑定
    0.40    0.000005           5         1           getsockname
  ------ ----------- ----------- --------- --------- ----------------
  100.00    0.001244          36        34         3 total

关键调用链：
  socket(AF_NETLINK) → bind() → setsockopt() × 3 → sendmsg() × 2 → recvmsg() × 6
```

### netstat 的系统调用（strace 实测）

```
strace -c -e trace=network,file netstat -tn

结果：
  % time     seconds  usecs/call     calls    errors syscall
  ------ ----------- ----------- --------- --------- ----------------
   88.51    0.000131          10        12         6 openat       ← 读 /proc/net/*
   11.49    0.000017           3         5           newfstatat
    0.00    0.000000           0         1         1 access
    0.00    0.000000           0         1           execve
  ------ ----------- ----------- --------- --------- ----------------
  100.00    0.000148           7        19         7 total

关键行为：
  openat("/proc/net/tcp") → read × N → close
  openat("/proc/net/tcp6") → read × N → close
  ★ 12 次 openat（6 个成功，6 个失败/ENOENT）
  ★ 连接越多，openat + read 次数越多
```

### 性能对比总结

```
                    ss                    netstat
                    ──────                ──────────
数据源：            netlink（二进制）      /proc/net/tcp（文本）
系统调用模式：      socket + send/recv     open + read + close
过滤执行位置：      ★ 内核侧              用户态（grep）
30 连接耗时：       ~1.2ms                ~0.1ms
10000 连接预估：    ~5ms                  ~500ms（需遍历所有行）
★ 性能差异来源：    连接数越多，优势越大

为什么 30 个连接时 ss 反而更慢？
  - netlink 协议有固定开销（创建 socket、bind、协议握手）
  - /proc 读取在少量文件时很快（VFS 缓存）
  - ★ 交叉点在约 100-1000 个连接：此后 ss 优势开始显现
```

---

## 总结

### ss 核心机制速查

| 机制 | 说明 |
|------|------|
| 通信协议 | netlink（AF_NETLINK + NETLINK_INET_DIAG） |
| 请求格式 | inet_diag_req_v2（协议族 + 协议 + 状态掩码 + 扩展标志） |
| 响应格式 | inet_diag_msg + nlattr 扩展（tcp_info / skmeminfo） |
| 过滤位置 | ★ 精确匹配在内核侧，范围/组合在用户态 |
| 数据来源 | 内核 sock / tcp_sock 结构体 |
| 进程关联 | 通过 /proc/PID/fd 反向查找 socket inode |

### 关键字段映射

| ss 输出字段 | 内核来源 | 获取方式 |
|------------|---------|---------|
| State | sk_common.skc_state | inet_diag_msg.idiag_state |
| Recv-Q | sk_receive_queue | inet_diag_msg.idiag_rqueue |
| Send-Q | sk_write_queue | inet_diag_msg.idiag_wqueue |
| timer | sk_timer / icsk_retransmit_timer | inet_diag_msg.idiag_timer |
| cwnd | tcp_sock.snd_cwnd | tcp_info.tcpi_snd_cwnd |
| rtt | tcp_sock.srtt_us | tcp_info.tcpi_rtt |
| skmem | sock.sk_meminfo | INET_DIAG_SKMEMINFO 属性 |
| Process | 通过 inode 反查 /proc/PID/fd | 用户态扫描 |
