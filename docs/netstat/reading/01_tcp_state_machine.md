# TCP 状态机详解 — 11 个状态的生命周期

> ss / netstat 输出中的 State 列来源于内核 TCP 协议栈的状态机。
> 理解每个状态的含义和转换条件，是诊断网络问题的基础。

---

## 一、11 个 TCP 状态总览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        TCP 状态机（RFC 793 / RFC 9293）                    │
│                                                                             │
│                     ┌──────────┐                                            │
│                     │  CLOSED  │  ← 初始/终态                               │
│                     └────┬─────┘                                            │
│                          │ 主动打开 → send SYN                              │
│                          ▼                                                  │
│                     ┌──────────┐     收到 SYN+ACK                           │
│                     │ SYN_SENT │ ──────────────────┐                        │
│                     └──────────┘                    │                       │
│                          ▲                          ▼                       │
│                     收到 SYN              ┌──────────────┐                  │
│                          │                │ ESTABLISHED  │ ← 数据传输       │
│                被动打开  │                └──────┬───────┘                  │
│                          ▼                       │ 主动关闭 → send FIN      │
│                   ┌──────────┐                   ▼                          │
│                   │  LISTEN  │          ┌──────────────┐                    │
│                   └────┬─────┘          │  FIN_WAIT_1  │                    │
│                        │ 收到 SYN       └──────┬───────┘                    │
│                        ▼                       │                            │
│                   ┌──────────┐            收到 ACK                          │
│                   │ SYN_RCVD │                 │                            │
│                   └────┬─────┘                 ▼                            │
│                        │              ┌──────────────┐                      │
│                   收到 ACK            │  FIN_WAIT_2  │                      │
│                        │              └──────┬───────┘                      │
│                        └─────────────────────┘                              │
│                              │ 收到 FIN                                     │
│                              ▼                                              │
│                     ┌──────────┐                                            │
│                     │TIME_WAIT │  ← 2MSL（60s）后关闭                       │
│                     └──────────┘                                            │
│                                                                             │
│  ★ 被动关闭方的路径：                                                       │
│  收到 FIN → CLOSE_WAIT → send FIN → LAST_ACK → 收到 ACK → CLOSED          │
│                                                                             │
│  ★ 同时关闭（罕见）：                                                       │
│  发送 FIN 后又收到对方 FIN → CLOSING → 收到 ACK → TIME_WAIT               │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 状态速查表

| 状态 | ss 中的显示 | 内核常量 | 含义 | 谁处于此状态 |
|------|-----------|---------|------|------------|
| CLOSED | (不显示) | `TCP_CLOSE` | 连接已关闭或不存在 | — |
| LISTEN | `LISTEN` | `TCP_LISTEN` | 监听端口，等待连接请求 | 服务端 |
| SYN_SENT | `SYN-SENT` | `TCP_SYN_SENT` | 已发送 SYN，等待 SYN+ACK | 主动连接方（客户端） |
| SYN_RECV | `SYN-RECV` | `TCP_SYN_RECV` | 收到 SYN，已回复 SYN+ACK | 被动连接方（服务端） |
| ESTABLISHED | `ESTAB` | `TCP_ESTABLISHED` | ★ 连接已建立，正在传输数据 | 两端 |
| FIN_WAIT1 | `FIN-WAIT-1` | `TCP_FIN_WAIT1` | 已发送 FIN，等待对方 ACK | 主动关闭方 |
| FIN_WAIT2 | `FIN-WAIT-2` | `TCP_FIN_WAIT2` | 收到 ACK，等待对方 FIN | 主动关闭方 |
| TIME_WAIT | `TIME-WAIT` | `TCP_TIME_WAIT` | 等待 2MSL 后彻底关闭 | 主动关闭方 |
| CLOSE_WAIT | `CLOSE-WAIT` | `TCP_CLOSE_WAIT` | 收到 FIN，等待应用层 close() | 被动关闭方 |
| CLOSING | `CLOSING` | `TCP_CLOSING` | 双方同时关闭（罕见） | 两端 |
| LAST_ACK | `LAST-ACK` | `TCP_LAST_ACK` | 已发送 FIN，等待最后 ACK | 被动关闭方 |

---

## 二、三次握手 — 连接建立的状态变化

```
  客户端                              服务端
  ──────                              ──────
  CLOSED                              LISTEN
    │                                    │
    │ 1. connect() → send SYN            │
    │ ─────────────────────────────────► │
    ▼                                    │
  SYN_SENT                               │
    │                                    │ 2. recv SYN → send SYN+ACK
    │                                    │ ──────────────────────────►
    │                                    ▼
    │                                  SYN_RCVD
    │ 3. recv SYN+ACK → send ACK        │
    │ ◄───────────────────────────────── │
    │                                    │
    ▼                                    │
  ESTABLISHED                            │
    │                                    │ 4. recv ACK
    │                                    ▼
    │                                  ESTABLISHED
    │                                    │
    ═══════════════════════════════════════
    ║    连接建立完成，开始数据传输       ║
    ═══════════════════════════════════════
```

### 三次握手状态变化详解

| 步骤 | 方向 | 报文 | 客户端状态 | 服务端状态 |
|------|------|------|-----------|-----------|
| 0 | — | — | CLOSED | LISTEN |
| 1 | 客户端→服务端 | SYN (seq=x) | **SYN_SENT** | LISTEN |
| 2 | 服务端→客户端 | SYN+ACK (seq=y, ack=x+1) | SYN_SENT | **SYN_RCVD** |
| 3 | 客户端→服务端 | ACK (ack=y+1) | **ESTABLISHED** | **ESTABLISHED** |

**★ 关键问题：**

- **SYN_SENT 大量堆积**：客户端发了 SYN 但没收到回复。可能原因：
  - 目标服务器宕机或网络不可达
  - 防火墙丢弃了 SYN 包
  - DNS 解析到了不可达的 IP

- **SYN_RECV 大量堆积**：服务端收到大量 SYN 但没完成握手。可能原因：
  - ★ **SYN Flood 攻击**（故意不发 ACK）
  - 客户端网络中断（ACK 丢失）
  - 排查命令：`ss -tn state syn-recv | wc -l`

- **内核限制参数**：
  ```
  /proc/sys/net/ipv4/tcp_max_syn_backlog    # SYN_RECV 队列上限（默认 128~1024）
  /proc/sys/net/ipv4/tcp_syncookies         # SYN Cookie 开关（1=启用）
  /proc/sys/net/ipv4/tcp_syn_retries        # SYN 重传次数（默认 6）
  ```

---

## 三、四次挥手 — 连接关闭的状态变化

```
  主动关闭方                        被动关闭方
  ──────────                        ──────────
  ESTABLISHED                       ESTABLISHED
       │                                 │
       │ 1. close() → send FIN           │
       │ ─────────────────────────────►  │
       ▼                                 │
  FIN_WAIT_1                             │
       │                                 │ 2. recv FIN
       │                                 ▼
       │                               CLOSE_WAIT
       │ 3. send ACK                     │
       │ ◄─────────────────────────────  │
       ▼                                 │
  FIN_WAIT_2                             │
       │                                 │ 4. 应用层 close() → send FIN
       │                                 │ ─────────────────────────────►
       │                                 ▼
       │                               LAST_ACK
       │ 5. recv FIN → send ACK          │
       │ ◄─────────────────────────────  │
       ▼                                 │
  TIME_WAIT                              │
       │ 6. 收到 ACK                     │
       │ (等待 2MSL = 60s)               ▼
       ▼                               CLOSED
  CLOSED
```

### 四次挥手状态变化详解

| 步骤 | 方向 | 报文 | 主动方状态 | 被动方状态 |
|------|------|------|-----------|-----------|
| 0 | — | — | ESTABLISHED | ESTABLISHED |
| 1 | 主动→被动 | FIN (seq=u) | **FIN_WAIT_1** | ESTABLISHED |
| 2 | 被动→主动 | ACK (ack=u+1) | FIN_WAIT_1 | **CLOSE_WAIT** |
| 3 | 被动→主动 | FIN (seq=w) | **FIN_WAIT_2** | CLOSE_WAIT |
| 4 | 主动→被动 | ACK (ack=w+1) | **TIME_WAIT** | **LAST_ACK** |
| 5 | — | — | TIME_WAIT (2MSL) | **CLOSED** |
| 6 | — | 超时 | **CLOSED** | CLOSED |

> **注意：** 步骤 2 和 3 可以合并（TCP 延迟 ACK），即服务端先 ACK，
> 然后在应用层 close() 后发送 FIN。此时 FIN_WAIT_1 → TIME_WAIT 直接跳转。

---

## 四、★ TIME_WAIT — 为什么必须存在

### 成因

TIME_WAIT 只出现在**主动关闭方**（谁先调 `close()` 谁就是主动方）。
持续时间为 **2MSL**（Maximum Segment Lifetime，最大报文段生存时间）= 60 秒。

```
为什么需要 TIME_WAIT？

1. 确保最后一个 ACK 被对方收到
   主动方发送 ACK 后，如果 ACK 丢失，被动方会重传 FIN。
   主动方需要在 TIME_WAIT 期间能重新发送 ACK。

2. 防止旧连接的数据包混入新连接
   网络中可能还残留旧连接的延迟数据包。
   等待 2MSL 确保这些包在网络中消失后，
   才能用相同的四元组（源IP:源端口-目的IP:目的端口）建立新连接。

时间线：
  0s          30s          60s
  │───────────│───────────│
  TIME_WAIT   等待中       关闭
  (2MSL)      (MSL)       (MSL)
  ← 可以重发 ACK →
  ←──── 旧数据包过期 ────→
```

### 内核参数

```
/proc/sys/net/ipv4/tcp_fin_timeout     = 60    # TIME_WAIT 持续时间（默认 60 秒）
/proc/sys/net/ipv4/tcp_tw_reuse        = 2     # 是否允许复用 TIME_WAIT socket（2=仅安全复用）
/proc/sys/net/ipv4/tcp_max_tw_buckets  = 262144 # TIME_WAIT 最大数量
```

### TIME_WAIT 过多的影响与排查

```bash
# 查看 TIME_WAIT 数量
ss -tn state time-wait | wc -l

# 查看 TIME_WAIT 占用的端口分布
ss -tn state time-wait | awk '{print $4}' | cut -d: -f2 | sort | uniq -c | sort -rn | head

# 内核全局统计
cat /proc/sys/net/ipv4/tcp_max_tw_buckets   # 上限
cat /proc/net/sockstat                       # 当前 socket 使用统计
```

**常见场景：**
- 短连接服务（HTTP 1.0、短轮询 API）会产生大量 TIME_WAIT
- **解决方案**：
  - 使用长连接（HTTP Keep-Alive、连接池）
  - 调大 `tcp_max_tw_buckets`（治标）
  - 启用 `tcp_tw_reuse`（需配合 `tcp_timestamps`）
  - ★ **不要** 设置 `tcp_tw_recycle`（已在 Linux 4.12 移除，会导致 NAT 穿透问题）

### 快速诊断表

| 现象 | 含义 | 严重程度 | 处理建议 |
|------|------|---------|---------|
| TIME_WAIT > 数千 | 短连接频繁关闭 | ⚠️ 中 | 改用长连接或连接池 |
| TIME_WAIT > 数万 | 接近 `tcp_max_tw_buckets` | 🔴 高 | 调大上限 + 检查是否有连接泄漏 |
| TIME_WAIT 集中在某端口 | 该端口的服务是短连接 | ⚠️ 中 | 优化该服务的连接复用 |

---

## 五、★ CLOSE_WAIT — 应用层的"债务"

### 成因

CLOSE_WAIT 只出现在**被动关闭方**。当收到对方的 FIN 后，内核自动进入 CLOSE_WAIT，
但**必须等待应用层调用 `close()`** 才能发送自己的 FIN 并进入 LAST_ACK。

```
正常的关闭流程（被动方视角）：

  ESTABLISHED → recv FIN → CLOSE_WAIT → 应用 close() → LAST_ACK → recv ACK → CLOSED

  ★ CLOSE_WAIT 停留时间 = 应用层处理延迟
  ★ 如果应用不调 close()，CLOSE_WAIT 永远不会消失！
```

### CLOSE_WAIT 过多的排查

```bash
# 查看 CLOSE_WAIT 连接
ss -tn state close-wait

# 统计 CLOSE_WAIT 数量
ss -tn state close-wait | wc -l

# 按进程统计 CLOSE_WAIT
ss -tnp state close-wait | awk -F'"' '{print $2}' | sort | uniq -c | sort -rn
```

**CLOSE_WAIT 堆积 = 应用层 Bug**

| 原因 | 表现 | 排查方法 |
|------|------|---------|
| 应用没有调用 `close()` | CLOSE_WAIT 持续增长 | 检查代码中的 socket 关闭逻辑 |
| 连接池未释放 | 进程退出前连接池满 | 检查连接池配置和超时 |
| 异常退出未清理 | 进程崩溃后 socket 残留 | 检查进程是否正常退出 |
| 死锁/阻塞 | 应用卡在某个操作上 | `strace -p <pid>` 查看系统调用 |

### ★ 关键区分

```
TIME_WAIT vs CLOSE_WAIT：

             TIME_WAIT                    CLOSE_WAIT
             ─────────                    ──────────
谁的问题    正常行为（TCP 协议要求）      应用层 Bug（没有 close）
出现在      主动关闭方                    被动关闭方
持续时间    60 秒（2MSL）                 永久（直到应用 close 或进程退出）
能否解决    调参 + 长连接                ★ 修复代码！
大量出现时  占用端口资源                  占用文件描述符
```

---

## 六、其他状态的排查要点

### FIN_WAIT_1 / FIN_WAIT_2

```
FIN_WAIT_1：
  - 主动关闭方已发 FIN，等待对方 ACK
  - 大量堆积 = 对方没有回复 ACK（网络问题或对方已崩溃）
  - 排查：ss -tn state fin-wait-1

FIN_WAIT_2：
  - 收到 ACK 后，等待对方 FIN
  - 大量堆积 = 对方收到 FIN 但一直没 close
  - 排查：ss -tn state fin-wait-2
  - 内核参数：tcp_orphan_retries（孤儿连接 FIN_WAIT_2 超时重试次数）
```

### LAST_ACK

```
LAST_ACK：
  - 被动关闭方已发 FIN，等待最后的 ACK
  - 大量堆积 = 主动关闭方已退出（收不到 ACK）
  - 通常很快消失（ms 级），看到说明有网络问题
  - 排查：ss -tn state last-ack
```

### CLOSING

```
CLOSING：
  - 双方几乎同时发 FIN
  - 极其罕见（只在两端几乎同时调 close() 时出现）
  - 排查：ss -tn state closing
```

### SYN_SENT

```
SYN_SENT：
  - 客户端发了 SYN，等待 SYN+ACK
  - 大量堆积 = 目标不可达
  - 排查：ss -tn state syn-sent
  - 内核参数：
    /proc/sys/net/ipv4/tcp_syn_retries = 6  # SYN 重传次数
    /proc/sys/net/ipv4/tcp_connect_timeout  # 连接超时
```

---

## 七、内核 TCP 状态常量源码位置

```c
// 文件：src/linux-5.10/include/net/tcp_states.h

enum {
    TCP_ESTABLISHED = 1,    // 对应 ss 中的 ESTAB
    TCP_SYN_SENT,           // SYN-SENT
    TCP_SYN_RECV,           // SYN-RECV
    TCP_FIN_WAIT1,          // FIN-WAIT-1
    TCP_FIN_WAIT2,          // FIN-WAIT-2
    TCP_TIME_WAIT,          // TIME-WAIT
    TCP_CLOSE,              // (不显示)
    TCP_CLOSE_WAIT,         // CLOSE-WAIT
    TCP_LAST_ACK,           // LAST-ACK
    TCP_LISTEN,             // LISTEN
    TCP_CLOSING,            // CLOSING
    TCP_NEW_SYN_RECV,       // ★ Linux 特有：SYN 收到但还未完全建立

    TCP_MAX_STATES          // 状态总数（用作数组大小）
};
```

### ss 输出中的状态映射

```
内核状态常量          ss 输出名称          netstat 输出名称
─────────────────    ───────────────      ───────────────
TCP_ESTABLISHED      ESTAB                ESTABLISHED
TCP_SYN_SENT         SYN-SENT             SYN_SENT
TCP_SYN_RECV         SYN-RECV             SYN_RECV
TCP_FIN_WAIT1        FIN-WAIT-1           FIN_WAIT1
TCP_FIN_WAIT2        FIN-WAIT-2           FIN_WAIT2
TCP_TIME_WAIT        TIME-WAIT            TIME_WAIT
TCP_CLOSE            (不显示)             —
TCP_CLOSE_WAIT       CLOSE-WAIT           CLOSE_WAIT
TCP_LAST_ACK         LAST-ACK             LAST_ACK
TCP_LISTEN           LISTEN               LISTEN
TCP_CLOSING          CLOSING              CLOSING
```

> **★ 注意**：ss 使用连字符（`FIN-WAIT-1`），netstat 使用下划线（`FIN_WAIT1`）。
> ss 的 filter 语法中要用连字符形式：`ss -tn state fin-wait-1`

---

## 八、实战诊断速查表

| 问题 | 命令 | 正常值 | 异常信号 |
|------|------|--------|---------|
| 各状态分布 | `ss -tna \| awk 'NR>1{print $1}' \| sort \| uniq -c \| sort -rn` | ESTAB 占大多数 | 非 ESTAB 状态异常多 |
| TIME_WAIT 数量 | `ss -tn state time-wait \| wc -l` | < 数千 | > 数万 |
| CLOSE_WAIT 数量 | `ss -tn state close-wait \| wc -l` | 0 | > 0 就有问题 |
| SYN_RECV 堆积 | `ss -tn state syn-recv \| wc -l` | 个位数 | > 100 可能是 SYN flood |
| FIN_WAIT 堆积 | `ss -tn state fin-wait-1 \| wc -l` | 个位数 | > 100 说明对端不响应 |
| 孤儿连接 | `cat /proc/sys/net/ipv4/tcp_orphan_retries` | 0 | 默认值合理 |
| TIME_WAIT 上限 | `cat /proc/sys/net/ipv4/tcp_max_tw_buckets` | 262144 | 根据业务调整 |
| socket 使用统计 | `cat /proc/net/sockstat` | — | 关注 TCP inuse/orphans |

---

## 九、socket 缓冲区与 Recv-Q / Send-Q

ss 输出中的 Recv-Q 和 Send-Q 在不同状态下含义不同：

### ESTABLISHED 状态

```
Recv-Q = 已接收但应用层尚未读取的字节数
  ★ 正常应为 0（应用及时读取了数据）
  ★ 持续增长 = 应用读取太慢或已停止读取

Send-Q = 已发送但尚未收到 ACK 的字节数
  ★ 正常应接近 0（网络通畅时很快确认）
  ★ 持续增长 = 网络拥塞或对方接收窗口为 0
```

### LISTEN 状态

```
Recv-Q = 当前已建立连接的 backlog 队列长度
  ★ 正常应为 0（没有等待 accept 的连接）
  ★ > 0 = accept() 来不及处理

Send-Q = listen() 时指定的 backlog 上限
  ★ 通常 128、511、2048、4096
  ★ Recv-Q 接近 Send-Q = backlog 快满了
```

### 缓冲区内存（ss -tm 输出中的 skmem）

```
skmem:(r0,rb131072,t0,tb2626560,f0,w0,o0,bl0,d0)
        │   │       │   │       │  │  │  │   │
        │   │       │   │       │  │  │  │   └─ d: drops（丢弃的数据包）
        │   │       │   │       │  │  │  └───── bl: backlog（积压的连接数）
        │   │       │   │       │  │  └──────── o: option memory（选项内存）
        │   │       │   │       │  └─────────── w: wmem_alloc（已分配的发送内存）
        │   │       │   │       └────────────── f: fwd_alloc（预分配的转发内存）
        │   │       │   └────────────────────── tb: tb (发送 buffer 总量)
        │   │       └────────────────────────── t: tmem（总内存）
        │   └────────────────────────────────── rb: rb (接收 buffer 总量)
        └────────────────────────────────────── r: rmem_alloc（已分配的接收内存）
```

---

## 十、总结：TCP 状态机的核心记忆法

```
★ 连接建立：CLOSED → SYN_SENT → ESTABLISHED（客户端）
              LISTEN → SYN_RECV → ESTABLISHED（服务端）

★ 主动关闭：ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT → CLOSED
            （谁先 close 谁走这条路）

★ 被动关闭：ESTABLISHED → CLOSE_WAIT → LAST_ACK → CLOSED
            （等对方关，自己最后关）

★ 排查要点：
  - TIME_WAIT 多 → 正常但可优化（长连接）
  - CLOSE_WAIT 多 → ★ Bug（应用没 close）
  - SYN_RECV 多 → ★ 安全问题（SYN flood）
  - FIN_WAIT 多 → 网络问题（对端不响应）
```
