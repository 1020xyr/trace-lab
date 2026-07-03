# ss（socket statistics）学习路线

> 从 netlink 到 socket 快照，从连接状态到性能诊断

---

## 目录

- [1. ss 的定位与价值](#1-ss-的定位与价值)
- [2. ss vs netstat 的本质区别](#2-ss-vs-netstat-的本质区别)
- [3. ss 在性能分析中的角色](#3-ss-在性能分析中的角色)
- [4. TCP 连接状态与性能的关系](#4-tcp-连接状态与性能的关系)
- [5. 学习路线图](#5-学习路线图)
- [6. ★ 性能诊断专题](#6--性能诊断专题)
- [7. 核心概念索引](#7-核心概念索引)
- [8. 动手实验清单](#8-动手实验清单)

---

## 1. ss 的定位与价值

`ss`（socket statistics）是 iproute2 包中的网络连接查看工具，是 `netstat` 的**现代替代品**。

### 一句话定位

> **ss 是网络连接的"快照"工具 —— 在某一瞬间，系统中所有 socket 的状态、内存、进程归属、拥塞控制参数。**

### ss 能回答的核心问题

| 问题 | 对应命令 | 性能工程师视角 |
|------|---------|--------------|
| 哪些端口在监听？ | `ss -tlnp` | 服务是否正常启动 |
| 有多少已建立连接？ | `ss -tn state established` | 连接数是否异常 |
| TIME_WAIT 是否过多？ | `ss -s` / `ss state time-wait` | 短连接风暴 |
| CLOSE_WAIT 是否泄漏？ | `ss state close-wait` | 应用层未关闭连接 |
| 每个连接的内存占用？ | `ss -tm` | 内存压力分析 |
| 拥塞窗口和 RTT？ | `ss -ti` | ★ 吞吐瓶颈定位 |
| 重传定时器和 keepalive？ | `ss -to` | 连接保活和超时 |
| 连接属于哪个进程/cgroup？ | `ss -te` | 进程归属和网络隔离 |

### ss 在工具链中的位置

```
┌─────────────────────────────────────────────────────────┐
│                  网络性能分析工具链                       │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  连接状态      流量统计       抓包分析      eBPF 追踪    │
│  ──────      ────────      ────────     ──────────    │
│  ★ ss       ifconfig/ip   tcpdump      bcc/bpftrace   │
│  netstat    ip -s         tshark       sockstat       │
│             ethtool -S    perf         tracepoint     │
│                                                         │
│  ss 负责：连接级别的状态快照 + 过滤 + TCP 参数           │
│  不负责：流量速率、丢包率、延迟测量（需要其他工具）       │
└─────────────────────────────────────────────────────────┘
```

---

## 2. ss vs netstat 的本质区别

### ★ 核心差异：数据获取机制

这是 ss 比 netstat 快的**根本原因**，也是面试和性能分析中高频考点。

```
netstat 的数据获取：
  ┌──────────┐     多次 open+read      ┌──────────────────┐
  │ netstat  │ ──────────────────────▶ │ /proc/net/tcp    │
  │ (用户态) │ ◀────────────────────── │ /proc/net/tcp6   │
  │          │  逐行解析 ASCII 文本     │ /proc/net/udp    │
  └──────────┘                         │ /proc/net/udp6   │
     │                                 └──────────────────┘
     │  每个文件：open → read(多次) → close
     │  连接越多，读取次数越多 → O(N) 次系统调用
     ▼
  格式化输出

ss 的数据获取：
  ┌──────────┐     1次 socket + sendmsg + recvmsg    ┌──────────────────┐
  │   ss     │ ──────────────────────────────────▶   │ netlink 内核接口  │
  │ (用户态) │ ◀──────────────────────────────────   │ (INET_DIAG)      │
  │          │  接收二进制结构体                       └──────────────────┘
  └──────────┘
     │  1 个 netlink socket → 1 次 sendmsg 发送查询
     │  → N 次 recvmsg 接收批量结果（每次返回多条）
     │  连接再多，系统调用次数也很少 → O(1) 次查询
     ▼
  格式化输出
```

### strace 实测验证

以下是在本机（30 个 TCP 连接）上实际 strace 的结果：

| 指标 | ss | netstat |
|------|-----|---------|
| 系统调用总数 | **34** | 19 |
| socket 调用 | 1（netlink socket） | 0 |
| sendmsg | **2**（发送 netlink 请求） | 0 |
| recvmsg | **6**（接收批量结果） | 0 |
| openat | 7（配置文件） | **12**（读 /proc/net/*） |
| 网络相关系统调用 | 9 | 0 |
| 总耗时 | 0.0012s | 0.0001s |

**解读：**
- ss 的系统调用**更多**（34 vs 19），但这是因为 netlink 协议的握手开销
- 在 30 个连接时差异不大；**当连接数达到万级别时**，netstat 需要打开并逐行读取数十个 /proc 文件，而 ss 仍然只需要几次 recvmsg
- ss 的真正优势在**大规模连接场景**（10K+ 连接时快 10-100 倍）

### 功能差异对照

| 功能 | netstat | ss | 备注 |
|------|---------|-----|------|
| 查看 TCP 连接 | ✅ | ✅ | 基础功能 |
| 查看 UDP 连接 | ✅ | ✅ | 基础功能 |
| 查看 Unix socket | ✅ | ✅ | 基础功能 |
| 查看进程信息 | ✅ `-p` | ✅ `-p` | 都需要 root |
| TCP 拥塞窗口 | ❌ | ✅ `-i` | ★ ss 独有 |
| RTT/RTT 方差 | ❌ | ✅ `-i` | ★ ss 独有 |
| TCP BBR 参数 | ❌ | ✅ `-i` | ★ ss 独有 |
| 每个 socket 内存 | ❌ | ✅ `-m` | ★ ss 独有 |
| 全局 socket 统计 | ✅ `-s` | ✅ `-s` | ss 更详细 |
| 过滤器（状态/端口/地址） | 无，需 grep | ✅ 内置 | ★ ss 独有 |
| cgroup 信息 | ❌ | ✅ `-e` | ★ ss 独有 |
| timer 信息 | ❌ | ✅ `-o` | ★ ss 独有 |
| 内核过滤 | ❌ | ✅ | ★ 过滤在内核执行 |

---

## 3. ss 在性能分析中的角色

### 性能分析工程师的日常场景

```
场景 1: 应用报"连接超时"
  → ss -tn dport = :3306     # 看 MySQL 连接状态
  → ss -ti dport = :3306     # 看 RTT 和重传
  → ss state close-wait      # 看是否有 CLOSE_WAIT 泄漏

场景 2: 服务器内存告警
  → ss -s                     # 看 TCP 总内存
  → ss -tm                    # 看每个 socket 的内存占用
  → cat /proc/sys/net/ipv4/tcp_mem  # 看内核 TCP 内存阈值

场景 3: 高吞吐但延迟上升
  → ss -ti                    # 看 cwnd 和 ssthresh
  → ss -to                    # 看重传定时器
  → 结合 sar -n DEV 看网卡流量

场景 4: 软中断 / CPU 高
  → ss -s                     # 看连接数分布
  → ss -tn                    # 看哪些连接流量大（Send-Q 积压）
  → 结合 mpstat -P ALL 1 看 si% 分布

场景 5: 短连接风暴
  → ss -s                     # 看 timewait 计数
  → ss state time-wait        # 列出所有 TIME_WAIT
  → ss -s | grep timewait     # 持续监控趋势
```

### ss 与其他工具的配合

```
                    ┌──────────┐
                    │  告警触发  │
                    └────┬─────┘
                         │
                         ▼
                  ┌──────────────┐
                  │  ss -s 快速   │  ← 第一步：全局概览
                  │  查看概况     │
                  └──────┬───────┘
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
   ┌─────────────┐ ┌──────────┐ ┌──────────────┐
   │ TIME_WAIT多  │ │ESTAB 多  │ │CLOSE_WAIT 多  │
   │ → 短连接风暴  │ │→ 长连接  │ │→ 应用泄漏    │
   └──────┬──────┘ └────┬─────┘ └──────┬───────┘
          │             │              │
          ▼             ▼              ▼
   ┌─────────────┐ ┌──────────┐ ┌──────────────┐
   │ ss -tm      │ │ ss -ti   │ │ 查应用日志    │
   │ 看内存占用   │ │ 看 cwnd  │ │ 看是否未关闭  │
   │             │ │ 和 rtt   │ │ 连接          │
   └─────────────┘ └──────────┘ └──────────────┘
```

---

## 4. TCP 连接状态与性能的关系

### 11 个 TCP 状态速查（ss 输出中的名称）

```
                        ┌─────────────────────────────────────────┐
                        │           TCP 连接生命周期               │
                        └─────────────────────────────────────────┘

主动打开端（客户端）                         被动打开端（服务端）
─────────────────                         ─────────────────
CLOSED ──主动打开──▶ SYN_SENT               CLOSED ──被动打开──▶ LISTEN
                         │                                          │
                    收到 SYN+ACK                              收到 SYN
                         │                                          │
                         ▼                                          ▼
                     ESTAB ◀───── 收到 ACK ───────────────── SYN_RECV
                       │                                        │
                    主动关闭                                 收到 ACK
                       │                                        │
                       ▼                                        ▼
                  FIN_WAIT_1                              ★ ESTAB ◀───┘
                       │                                   （★ 最常见的状态）
                  收到 ACK                                      │
                       │                                    被动关闭
                       ▼                                        │
                  FIN_WAIT_2                                    ▼
                       │                                   CLOSE_WAIT
                  收到 FIN                                      │
                       │                                    应用关闭
                       ▼                                        │
                  TIME_WAIT                                     ▼
                       │                                   LAST_ACK
                  2MSL 超时                                     │
                       │                                    收到 ACK
                       ▼                                        ▼
                     CLOSED                                   CLOSED
```

### ★ 性能工程师最关注的状态

| 状态 | 正常情况 | 异常信号 | 排查命令 |
|------|---------|---------|---------|
| **ESTAB** | 活跃数据传输 | 数量暴增 → 连接泄漏或慢响应 | `ss -tn state established` |
| **TIME_WAIT** | 正常关闭后的等待 | 数量 > 数千 → 短连接风暴 | `ss -s` / `ss state time-wait` |
| **CLOSE_WAIT** | 对方关闭后等待本地关闭 | 数量 > 0 长时间存在 → ★ 应用层泄漏 | `ss state close-wait` |
| **SYN_RECV** | 等待三次握手完成 | 数量暴增 → SYN Flood 攻击 | `ss -tn state syn-recv` |
| **FIN_WAIT_1** | 等待对方 ACK | 数量多 → 对方可能不响应 | `ss -tn state fin-wait-1` |
| **FIN_WAIT_2** | 等待对方 FIN | 长期存在 → 对方未关闭 | `ss -tn state fin-wait-2` |

### TIME_WAIT 过多的影响与优化

```
TIME_WAIT 过多会导致：
  1. 占用本地端口（默认端口范围 32768-60999，约 28000 个）
  2. 占用内核内存（每个 TIME_WAIT socket 约 0.5KB）
  3. 如果端口耗尽 → 新连接建立失败

优化方案：
  ┌───────────────────────────────────────────────────┐
  │ 1. 应用层优化：使用连接池、长连接（Keep-Alive）    │
  │ 2. 内核参数：                                      │
  │    tcp_tw_reuse = 1  （允许复用 TIME_WAIT 端口）   │
  │    tcp_max_tw_buckets（限制 TIME_WAIT 上限）       │
  │ 3. 缩短 tcp_fin_timeout（影响 FIN_WAIT_2 → CLOSED）│
  │    注意：tcp_tw_reuse=2 是内核 4.12+ 的新默认值     │
  │    表示"仅在 loopback 接口上启用"                   │
  └───────────────────────────────────────────────────┘
```

### CLOSE_WAIT 泄漏的典型场景

```
CLOSE_WAIT 泄漏的原因：

  正常流程：                           泄漏流程：
  对端发送 FIN                         对端发送 FIN
       │                                    │
       ▼                                    ▼
  本地 TCP → CLOSE_WAIT               本地 TCP → CLOSE_WAIT
       │                                    │
  应用调用 close()                     应用忘记调用 close() ✗
       │                                    │
       ▼                                    ▼
  LAST_ACK → CLOSED                   ★ 永远停留在 CLOSE_WAIT
                                       → 泄漏！占用文件描述符和内存

常见原因：
  1. 代码中 catch 异常后未关闭 socket
  2. 连接池管理不当
  3. 多线程环境中 socket 引用计数错误
```

---

## 5. 学习路线图

### Step 1: ss 内部机制 — netlink 与内核数据结构

**目标：** 理解 ss 如何通过 netlink 从内核获取 socket 信息

**阅读材料：** `reading/01_ss_internals.md`

**学习要点：**
- netlink INET_DIAG 协议的工作原理
- 内核 struct sock / struct tcp_sock 的关键字段
- ss 过滤器语法（state/port/address/protocol）
- ss 输出字段详解（Recv-Q/Send-Q/timer 等）

---

### Step 2: 命令参考 — 完整参数与过滤器

**目标：** 掌握 ss 的所有参数和过滤器语法

**阅读材料：** `reading/02_command_reference.md`

**学习要点：**
- 协议过滤参数（-t/-u/-x/-w/-4/-6）
- 状态过滤参数（-l/-a）
- 信息级别参数（-o/-e/-m/-p/-i/-n）
- 过滤器语法（state/sport/dport/dst/src）
- ss 替代 netstat 命令对照表

---

### Step 3: 输出实战 — 真实输出逐字段解读

**目标：** 能从 ss 输出中诊断性能问题

**阅读材料：** `reading/03_command_output_demo.md`

**学习要点：**
- 12 个场景的真实输出 + 逐字段注解
- TCP 内存字段（skmem）的完整解读
- TCP 拥塞控制参数（cwnd/ssthresh/rtt）的解读
- 性能诊断流程：从 ss -s 开始，分状态排查

---

### 学习顺序建议

```
推荐路径（性能分析工程师）：

  Step 1 → Step 2 → Step 3
  (原理)   (命令)   (实战)

快速路径（只需会用）：

  Step 2 → Step 3
  (命令)   (实战)

深入路径（内核原理）：

  先阅读 docs/netstat/reading/01_tcp_state_machine.md  ← TCP 状态机基础
  再阅读 docs/netstat/learning-roadmap.md 的 netlink 章节
  然后 Step 1 → Step 2 → Step 3
```

---

## 6. ★ 性能诊断专题

### 6.1 连接数异常排查

```
诊断流程：

  ① ss -s                          # 全局概览
     → Total/estab/closed/timewait 数字

  ② ss -tan | awk '{print $1}' | sort | uniq -c | sort -rn
     → 各状态连接数排名

  ③ 针对异常状态深入：
     TIME_WAIT 多 → ss state time-wait | awk '{print $5}' | cut -d: -f1 | sort | uniq -c
                    → 看哪些目标 IP 产生大量 TIME_WAIT
     ESTAB 多    → ss -tn state established -i
                    → 看 cwnd 和 Send-Q，找"卡住"的连接
     CLOSE_WAIT  → ss state close-wait -p
                    → 看哪个进程持有未关闭的连接
```

### 6.2 TCP 吞吐瓶颈定位

```
关键参数（ss -ti 输出）：

  cwnd（拥塞窗口）
    → 越大越好（理想值 > 带宽延迟积 / MSS）
    → cwnd 很小（< 10）→ 可能丢包导致窗口缩小

  ssthresh（慢启动阈值）
    → cwnd < ssthresh 时处于慢启动阶段
    → cwnd ≥ ssthresh 时进入拥塞避免

  rtt（往返时延）/ rttvar（RTT 方差）
    → rtt 高 → 网络延迟大（跨地域/跨机房）
    → rttvar 高 → 网络不稳定（抖动大）

  send（理论发送速率）
    → 基于 cwnd × MSS / RTT 计算
    → 与实际 throughput 对比可判断瓶颈

  pacing_rate（内核 pacing 速率）
    → 如果 pacing_rate < send → 受限于 pacing
    → BBR 拥塞控制会主动设置 pacing_rate

  delivery_rate（实际交付速率）
    → ★ 最接近真实吞吐的指标
    → delivery_rate << send → 网络瓶颈

  retrans / bytes_retrans
    → 重传次数和字节数
    → 高重传 → 网络质量差

  app_limited
    → 标记连接是否受限于应用层发送速度
    → 如果 app_limited 且吞吐低 → 问题在应用层
```

### 6.3 内存压力分析

```
诊断流程：

  ① ss -s                          # 全局 TCP 内存概览
  ② ss -tm                         # 每个 socket 的内存
  ③ 解读 skmem 字段：
     skmem:(r<rmem_alloc>,rb<rcv_buf>,t<wmem_alloc>,tb<snd_buf>,
            f<fwd_alloc>,w<wmem_queued>,o<optmem>,bl<back_log>,d<dropped>)

  ④ 检查内核参数：
     cat /proc/sys/net/ipv4/tcp_mem    # 全局 TCP 内存阈值（页）
     cat /proc/sys/net/ipv4/tcp_rmem   # 每个 socket 的接收缓冲区
     cat /proc/sys/net/ipv4/tcp_wmem   # 每个 socket 的发送缓冲区

  ⑤ 如果 TCP 内存使用超过 tcp_mem[1]（pressure 阈值）
     → 内核进入内存压力模式，限制新分配的缓冲区大小
```

### 6.4 软中断与 ss 的结合分析

```
当 CPU 的 si%（软中断）偏高时：

  ① mpstat -P ALL 1                # 看各 CPU 的 si%
  ② cat /proc/interrupts | grep eth  # 看各队列中断分布
  ③ ss -s                           # 看总连接数
  ④ ss -tn                          # 看活跃连接

  分析逻辑：
  - 如果连接数很多 + 所有 CPU 的 si% 都高 → RPS 配置问题
  - 如果只有少数 CPU 的 si% 高 → 中断亲和性不均匀
  - 如果连接数不多但 si% 高 → 可能是小包风暴

  优化方向：
  - 配置 RPS（Receive Packet Steering）分散软中断
  - 调整网卡多队列中断亲和性（smp_affinity）
  - 使用 RSS（Receive Side Scaling）硬件分流
```

---

## 7. 核心概念索引

### netlink 协议

| 概念 | 说明 |
|------|------|
| netlink | Linux 内核与用户态进程之间的通信协议，基于 socket 接口 |
| NETLINK_INET_DIAG | ss 使用的 netlink 子协议，专门查询 socket 诊断信息 |
| SOCK_DIAG_BY_FAMILY | 查询特定协议族的 socket 信息 |
| inet_diag_msg | 内核返回的 socket 信息结构体 |
| tcp_info | TCP 连接的详细参数（cwnd/rtt/ssthresh 等） |
| tcpvegas_info | TCP Vegas/BBR 的额外参数 |

### ss 输出字段

| 字段 | 含义 |
|------|------|
| Recv-Q | 接收队列中未被应用读取的字节数 |
| Send-Q | 发送队列中未被对端 ACK 的字节数 |
| State | TCP 状态（ESTAB/LISTEN/TIME_WAIT 等） |
| Local Address:Port | 本地地址和端口 |
| Peer Address:Port | 对端地址和端口 |
| Process | 持有该 socket 的进程 |
| timer | 定时器类型和剩余时间 |
| skmem | socket 内存详情 |

### TCP 拥塞控制参数

| 参数 | 含义 | ss -i 字段 |
|------|------|-----------|
| cwnd | 拥塞窗口（段数） | cwnd |
| ssthresh | 慢启动阈值 | ssthresh |
| rtt | 平滑 RTT（ms） | rtt |
| rttvar | RTT 方差（ms） | rtt（斜杠后） |
| mss | 最大段大小 | mss |
| pmtu | 路径 MTU | pmtu |
| send | 理论发送速率 | send |
| pacing_rate | pacing 速率 | pacing_rate |
| delivery_rate | 实际交付速率 | delivery_rate |
| rcv_space | 接收缓冲区预留空间 | rcv_space |

---

## 8. 动手实验清单

### 实验 1：基础连接查看

```bash
# 查看所有 TCP 监听端口及对应进程
ss -tlnp

# 查看所有已建立的 TCP 连接
ss -tn state established

# 查看所有连接（含 TIME_WAIT）
ss -tan
```

### 实验 2：socket 统计摘要

```bash
# 查看全局 socket 统计
ss -s

# 持续监控（每 2 秒）
watch -n 2 ss -s
```

### 实验 3：TCP 内存分析

```bash
# 查看每个连接的内存使用
ss -tm

# 查看 TCP 内核内存参数
cat /proc/sys/net/ipv4/tcp_mem
cat /proc/sys/net/ipv4/tcp_rmem
cat /proc/sys/net/ipv4/tcp_wmem
```

### 实验 4：TCP 参数详细查看

```bash
# 查看所有已建立连接的 TCP 参数
ss -ti

# 重点看某个连接的 cwnd 和 rtt
ss -ti dport = :443
```

### 实验 5：过滤器实战

```bash
# 只看 SSH 连接
ss -tn 'sport = :22'

# 只看 HTTPS 出站连接
ss -tn 'dport = :443'

# 只看本地回环地址的连接
ss -tn 'dst 127.0.0.0/8'

# 组合过滤：SSH 端口且已建立
ss -tn 'sport = :22' state established
```

### 实验 6：性能诊断流程

```bash
# 第一步：全局概览
ss -s

# 第二步：按状态统计
ss -tan | awk 'NR>1 {print $1}' | sort | uniq -c | sort -rn

# 第三步：如果有 TIME_WAIT 过多
ss state time-wait | awk '{print $5}' | cut -d: -f1 | sort | uniq -c | sort -rn

# 第四步：看具体 TCP 参数
ss -ti state established

# 第五步：看内存
ss -tm state established
```

---

## 附录：相关文件索引

| 文件 | 内容 |
|------|------|
| `reading/01_ss_internals.md` | ss 内部机制与 netlink 原理 |
| `reading/02_command_reference.md` | 完整参数与过滤器参考 |
| `reading/03_command_output_demo.md` | 12 个场景的真实输出实战 |
| `QA.md` | 常见问题问答集 |

### 跨工具参考

| 文件 | 内容 |
|------|------|
| `docs/netstat/reading/01_tcp_state_machine.md` | TCP 状态机详解（ss 复用同一状态机） |
| `docs/netstat/learning-roadmap.md` | netstat 与 ss 的对比、netlink 原理 |
| `docs/netstat/QA.md` | 网络性能排查 QA（软中断、CPU 高等） |
