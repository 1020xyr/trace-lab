# netstat + ss 学习路线

> 从 TCP 状态机到 socket 统计，从 /proc/net/tcp 到 netlink

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 学习路线图](#2-学习路线图)
  - [Step 1: TCP 状态机 — 11 个状态的生命周期](#step-1-tcp-状态机--11-个状态的生命周期)
  - [Step 2: netstat vs ss — 两代工具的原理差异](#step-2-netstat-vs-ss--两代工具的原理差异)
  - [Step 3: 数据源 — /proc/net/tcp vs netlink socket](#step-3-数据源--procnetsctp-vs-netlink-socket)
  - [Step 4: 命令参考 — 完整参数与场景](#step-4-命令参考--完整参数与场景)
  - [Step 5: 输出实战 — 真实输出逐行解读](#step-5-输出实战--真实输出逐行解读)
- [3. ★ 网络性能诊断专题](#3--网络性能诊断专题)
- [4. 核心概念索引](#4-核心概念索引)
- [5. 动手实验清单](#5-动手实验清单)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     用户态工具                               │
│  netstat（旧，net-tools 包）   ss（新，iproute2 包）         │
├─────────────────────────────────────────────────────────────┤
│                     内核接口层                               │
│  netstat → 读取 /proc/net/tcp、/proc/net/udp 等文本文件      │
│  ss      → 通过 netlink 协议（INET_DIAG）查询内核            │
├─────────────────────────────────────────────────────────────┤
│                     内核 TCP/IP 协议栈                       │
│  socket → TCP 状态机 → 连接跟踪 → 统计计数器                 │
└─────────────────────────────────────────────────────────────┘
```

### 数据流对比

```
netstat 的数据流：
  用户进程 socket()
     │
     ▼
  内核 TCP/IP 协议栈 → 维护 sock 结构体
     │
     ▼
  /proc/net/tcp（内核 seq_file 接口，遍历所有 TCP sock）
     │
     ▼
  netstat 逐行解析文本文件
     │
     ▼
  格式化输出（慢：大量连接时逐行读 /proc 效率低）

ss 的数据流：
  用户进程 socket()
     │
     ▼
  内核 TCP/IP 协议栈 → 维护 sock 结构体
     │
     ▼
  netlink INET_DIAG 接口（内核直接序列化为二进制）
     │
     ▼
  ss 通过 netlink socket 接收（一次 recvmsg 获取大量数据）
     │
     ▼
  格式化输出（快：二进制协议，支持过滤）
```

---

## 2. 学习路线图

### Step 1: TCP 状态机 — 11 个状态的生命周期

**目标：** 理解 ss/netstat 输出中每个状态列的含义

**阅读材料：** `reading/01_tcp_state_machine.md`

**关键认知：**

```
TCP 状态机（RFC 793）：

            ┌──────────┐
            │  CLOSED  │  ← 初始状态
            └────┬─────┘
                 │ 主动打开 → send SYN
                 ▼
            ┌──────────┐     收到 SYN+ACK
            │ SYN_SENT │ ──────────────────┐
            └──────────┘                    │
                 ▲                          ▼
            收到 SYN              ┌──────────────┐
            │                     │ ESTABLISHED  │  ← 数据传输
  被动打开  │                     └──────┬───────┘
            ▼                            │ 主动关闭 → send FIN
     ┌──────────┐                        ▼
     │  LISTEN  │               ┌──────────────┐
     └────┬─────┘               │  FIN_WAIT_1  │
          │ 收到 SYN             └──────┬───────┘
          ▼                            │
     ┌──────────┐                收到 ACK
     │ SYN_RCVD │                      │
     └────┬─────┘                      ▼
          │ 收到 ACK           ┌──────────────┐
          │                    │  FIN_WAIT_2  │
          └─────────────────────┘  ┌──────┬───────┘
                 同时收到 FIN+ACK   │      │ 收到 FIN
                                   │      ▼
                                   │  ┌──────────┐
                                   │  │TIME_WAIT │  ← 2MSL 后关闭
                                   │  └──────────┘
                                   │
               被动关闭端：        │
               收到 FIN            │
                 │                 │
                 ▼                 │
          ┌───────────┐           │
          │ CLOSE_WAIT│           │
          └────┬──────┘           │
               │ send FIN         │
               ▼                  │
          ┌───────────┐          │
          │ LAST_ACK  │──────────┘
          └───────────┘  收到 ACK
```

**11 个状态速查：**

| 状态 | ss 中的显示 | 含义 | 谁关心 |
|------|-----------|------|--------|
| ESTABLISHED | `ESTAB` | 连接已建立，正在传输数据 | 两端 |
| SYN_SENT | `SYN-SENT` | 已发送 SYN，等待对方 SYN+ACK | 主动连接方 |
| SYN_RECV | `SYN-RECV` | 收到 SYN，已回复 SYN+ACK | 被动连接方 |
| FIN_WAIT1 | `FIN-WAIT-1` | 已发送 FIN，等待对方 ACK | 主动关闭方 |
| FIN_WAIT2 | `FIN-WAIT-2` | 收到 ACK，等待对方 FIN | 主动关闭方 |
| TIME_WAIT | `TIME-WAIT` | 等待 2MSL 后关闭（防止旧数据） | 主动关闭方 |
| CLOSE_WAIT | `CLOSE-WAIT` | 收到 FIN，等待应用关闭 | 被动关闭方 |
| CLOSING | `CLOSING` | 双方同时关闭 | 罕见 |
| LAST_ACK | `LAST-ACK` | 已发送 FIN，等待最后一个 ACK | 被动关闭方 |
| LISTEN | `LISTEN` | 监听端口，等待连接 | 服务端 |
| CLOSED | (不显示) | 连接已关闭 | — |

**思考题：**
1. 为什么 TIME_WAIT 持续 2MSL（60 秒）而不是立即关闭？
2. 大量 CLOSE_WAIT 说明什么问题？（答：应用层没有调用 close()）
3. SYN_RECV 状态大量堆积意味着什么？（答：可能遭受 SYN flood 攻击）

---

### Step 2: netstat vs ss — 两代工具的原理差异

**目标：** 理解为什么 ss 比 netstat 更快、信息更丰富

**阅读材料：** `reading/02_command_reference.md`

**核心差异：**

```
              netstat                          ss
              ──────                          ──
数据来源    /proc/net/tcp (文本)         netlink INET_DIAG (二进制)
解析方式    逐行 sscanf                  直接解析 struct inet_diag_msg
过滤能力    无（全量读取后用户过滤）      内核侧过滤（BPF 表达式）
速度        慢（10 万连接需数秒）         快（10 万连接 <1 秒）
timer 信息  无                           ✅ 显示 keepalive/retransmit timer
内存信息    无                           ✅ skmem 详情
socket 统计  基本                        ✅ 完整的协议统计
包归属      net-tools（已停止维护）       iproute2（活跃维护）
```

---

### Step 3: 数据源 — /proc/net/tcp vs netlink socket

**目标：** 理解内核如何暴露 socket 信息

**阅读材料：** `reading/02_command_reference.md`（数据源章节）

**/proc/net/tcp 格式：**

```
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:956D 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 4235535
   ↑     ↑              ↑        ↑      ↑        ↑                                    ↑
  序号  本地地址:端口  远程地址  状态  发送队列  接收队列                              inode
       (十六进制)    (十六进制) 0A=LISTEN
```

**netlink INET_DIAG 协议：**

```c
// 用户态发送请求
struct inet_diag_req_v2 {
    __u8    sdiag_family;    // AF_INET / AF_INET6
    __u8    sdiag_protocol;  // IPPROTO_TCP / IPPROTO_UDP
    __u8    idiag_ext;       // 请求扩展信息（timer、memory 等）
    __u8    pad;
    __u32   idiag_states;    // ★ 状态过滤掩码（只看 ESTABLISHED 等）
    struct inet_diag_sockid id; // 可按地址/端口过滤
};

// 内核返回结果
struct inet_diag_msg {
    __u8    idiag_family;
    __u8    idiag_state;     // TCP_ESTABLISHED 等
    __u8    idiag_timer;     // 定时器类型
    __u8    idiag_retrans;   // 重传次数
    struct inet_diag_sockid id;
    __u32   idiag_expires;   // 定时器超时时间
    __u32   idiag_rqueue;    // 接收队列
    __u32   idiag_wqueue;    // 发送队列
    __u32   idiag_uid;       // 所属用户
    __u32   idiag_inode;     // inode 号
};
```

---

### Step 4: 命令参考 — 完整参数与场景

**目标：** 掌握 netstat 和 ss 的所有常用参数

**阅读材料：** `reading/02_command_reference.md`

**最常用的命令组合：**

```bash
# 查看 TCP 监听端口 + 进程
ss -tlnp

# 查看所有已建立的 TCP 连接
ss -tnp

# 查看 socket 统计摘要
ss -s

# 查看 TCP 内存使用
ss -tm

# 查看 timer 信息
ss -to

# 查看协议统计
netstat -s

# 统计各状态的连接数
ss -tna | awk 'NR>1{print $1}' | sort | uniq -c | sort -rn
```

---

### Step 5: 输出实战 — 真实输出逐行解读

**目标：** 理解每一列的含义，学会诊断实际问题

**阅读材料：** `reading/03_command_output_demo.md`

**诊断场景速查：**

| 问题 | 命令 | 看什么 |
|------|------|--------|
| 端口被谁占了 | `ss -tlnp 'sport = :80'` | Process 列 |
| TIME_WAIT 过多 | `ss -tn state time-wait \| wc -l` | 数量 > 数千需关注 |
| CLOSE_WAIT 堆积 | `ss -tn state close-wait` | 有 → 应用层没 close |
| 连接数排行 | `ss -tn \| awk '{print $5}' \| cut -d: -f1 \| sort \| uniq -c` | 连接最多的远端 IP |
| socket 内存占用 | `ss -tm` | skmem 中的 rb/tb |
| keepalive 状态 | `ss -to` | timer 列 |

---

## 3. ★ 网络性能诊断专题 — 从连接检查到包级分析

### 网络性能诊断的核心思路

网络问题诊断需要**自底向上**：先看连接状态，再看中断分布，最后看协议参数。

```
┌─────────────────────────────────────────────────────────────────┐
│                    网络性能诊断分层                               │
│                                                                  │
│  第 1 层：连接状态 ← ★ ss/netstat 在这里                        │
│    ss -tna → TCP 状态分布、TIME_WAIT/CLOSE_WAIT 数量            │
│    ss -s → socket 统计摘要                                       │
│                                                                  │
│  第 2 层：中断与 CPU                                             │
│    /proc/interrupts → 网卡中断在各 CPU 的分布                    │
│    mpstat -P ALL 1 → 各 CPU 的 %soft（软中断占比）              │
│                                                                  │
│  第 3 层：协议参数                                               │
│    sysctl → TCP 缓冲区、拥塞算法、RPS 配置                       │
│    netstat -s → 重传率、SYN drops、超时统计                      │
│                                                                  │
│  第 4 层：抓包与 eBPF ← ★ 高级诊断                              │
│    tcpdump / tshark → 包级别诊断                                 │
│    bpftrace / BCC → eBPF 网络追踪                                │
└─────────────────────────────────────────────────────────────────┘
```

### ★ 网络吞吐正常但 CPU 高 — 软中断问题（AMD 高频场景）

这是 AMD 服务器上的**高频问题场景**：网络带宽利用率正常（如 10Gbps 跑满），但 CPU 使用率异常高。

```
现象：
  - 网络吞吐正常（带宽利用率 > 80%）
  - CPU 使用率高（特别是 %soft 和 %system）
  - 应用延迟不稳定

诊断流程：
  ┌──────────────────────────────────────────────────────────┐
  │ Step 1: 确认 CPU 消耗在哪里                               │
  │   mpstat -P ALL 1                                        │
  │   → 看各 CPU 的 %soft 列                                │
  │   → 如果某 CPU 的 %soft > 30%，说明软中断高              │
  │                                                          │
  │ Step 2: 看中断分布                                        │
  │   cat /proc/interrupts | grep eth                        │
  │   → 看网卡中断（如 eth0-Tx-0, eth0-Rx-0）在各 CPU 的分布│
  │   → 如果集中在 1-2 个 CPU → ★ 中断不均衡                │
  │                                                          │
  │ Step 3: 判断根因                                          │
  │   ├─ 中断集中在少数 CPU                                   │
  │   │   → 需要 IRQ 亲和性调优或多队列网卡                   │
  │   ├─ 中断分布均匀但 %soft 仍然高                          │
  │   │   → 包处理量大，需要 RPS/RFS 分散软中断              │
  │   └─ 大量小包（PPS 高但带宽不高）                         │
  │       → 每个包的中断开销固定，小包场景 CPU 消耗大          │
  └──────────────────────────────────────────────────────────┘
```

#### 中断分布检查

```bash
# 查看网卡中断在各 CPU 的分布
cat /proc/interrupts | grep -E 'eth|CPU'

# 输出示例（4 CPU 系统）：
#            CPU0       CPU1       CPU2       CPU3
#  34:  12345678          0          0          0   eth0-Rx-0
#  35:          0   12345678          0          0   eth0-Tx-0
#
# ★ 问题：Rx 中断全部集中在 CPU0，Tx 中断全部集中在 CPU1
# CPU0 和 CPU1 的 %soft 会很高，而 CPU2/CPU3 几乎空闲

# 解决方案 1：IRQ 亲和性（irqbalance 或手动绑定）
# 安装 irqbalance 自动均衡
systemctl start irqbalance

# 或手动绑定
echo 0f > /proc/irq/34/smp_affinity  # 0f = CPU0-3 都可以处理

# 解决方案 2：多队列网卡（硬件支持时）
ethtool -l eth0    # 查看队列数
ethtool -L eth0 combined 4   # 设置 4 个队列
```

### ★ RPS（Receive Packet Steering）— 软件多队列

```
RPS 是什么？
─────────────
  当网卡只有 1 个硬件队列时，所有收包中断集中在 1 个 CPU。
  RPS 在软件层面将收包负载分散到多个 CPU。

  工作原理：
    网卡收包 → 单 CPU 硬中断
                ↓
           NAPI poll（单 CPU）
                ↓
           ★ RPS 按哈希将 skb 分发到其他 CPU 的 backlog
                ↓
           其他 CPU 处理 softirq（NET_RX）
                ↓
           协议栈处理（TCP/IP）→ 应用层

配置方法：
──────────
  # 查看当前 RPS 配置
  cat /sys/class/net/eth0/queues/rx-0/rps_cpus
  # 输出 00 = RPS 未启用

  # 启用 RPS：让所有 CPU 参与收包处理
  echo ff > /sys/class/net/eth0/queues/rx-0/rps_cpus
  # ff = 11111111（8 个 CPU 都参与）
  # 4 CPU 系统用 0f，8 CPU 系统用 ff

  # 配置 RFS（Receive Flow Steering）— 让包到达处理它的应用所在 CPU
  echo 32768 > /proc/sys/net/core/rps_sock_flow_entries
  echo 4096 > /sys/class/net/eth0/queues/rx-0/rps_flow_cnt

  ★ RPS 适合场景：
    - 单队列网卡（虚拟化环境常见）
    - 大流量场景（> 1Gbps）
    - CPU 软中断不均衡

  ★ RPS 不适合场景：
    - 多队列网卡 + IRQ 已均衡
    - 小包高 PPS 场景（RPS 本身的开销可能抵消收益）
```

### ★ TCP 性能调优参数

```
TCP 缓冲区调优
──────────────
  # 接收缓冲区（最小/默认/最大，单位字节）
  cat /proc/sys/net/ipv4/tcp_rmem
  # 默认：4096 131072 6291456（4KB / 128KB / 6MB）
  
  # 发送缓冲区
  cat /proc/sys/net/ipv4/tcp_wmem
  # 默认：4096 16384 6291456（4KB / 16KB / 6MB）
  
  # ★ 高带宽高延迟网络（BDP 大）需要调大缓冲区
  # BDP = 带宽 × RTT
  # 例：10Gbps × 10ms RTT = 12.5MB
  # 此时 tcp_rmem 最大值应 > 12.5MB

  # 调优示例（10Gbps 网络）
  sysctl -w net.ipv4.tcp_rmem='4096 87380 16777216'
  sysctl -w net.ipv4.tcp_wmem='4096 65536 16777216'

TCP 拥塞算法
────────────
  # 查看当前拥塞算法
  cat /proc/sys/net/ipv4/tcp_congestion_control
  # 默认：cubic
  
  # 查看可用算法
  cat /proc/sys/net/ipv4/tcp_available_congestion_control
  
  # ★ BBR vs CUBIC
  # CUBIC：Linux 默认，适合一般场景
  # BBR：Google 开发，适合高带宽高延迟 + 有丢包的网络
  #       不依赖丢包信号，而是估计带宽和 RTT
  
  # 启用 BBR（需要内核 4.9+）
  modprobe tcp_bbr
  sysctl -w net.ipv4.tcp_congestion_control=bbr
  
  # 验证
  ss -ti state established
  # 输出中应显示 "bbr" 而非 "cubic"

NAPI 和 busy polling
─────────────────────
  # NAPI（New API）：中断合并，减少中断次数
  # 查看 NAPI 配置
  cat /sys/class/net/eth0/napi_defer_hard_irqs  # 延迟硬中断次数
  cat /sys/class/net/eth0/gro_flush_timeout      # GRO 刷新超时
  
  # ★ 调优高吞吐场景
  echo 2 > /sys/class/net/eth0/napi_defer_hard_irqs
  echo 20000 > /sys/class/net/eth0/gro_flush_timeout  # 20μs
  
  # 效果：减少中断次数，提高 GRO（Generic Receive Offload）效率
  # 代价：增加少量延迟（20μs 级别）
```

### TIME_WAIT 过多 — 端口耗尽风险

```
问题：
  TIME_WAIT 连接过多（> 数万），导致：
  1. 本地端口耗尽（无法发起新连接）
  2. 内存占用增加（每个 TIME_WAIT 占用约 0.5KB）

检查：
  ss -s                    # 查看 timewait 数量
  ss -tn state time-wait | wc -l   # 精确计数
  
  # 按远端 IP 统计 TIME_WAIT 分布
  ss -tn state time-wait | awk 'NR>1{print $5}' | cut -d: -f1 | sort | uniq -c | sort -rn

优化方案：
  ┌──────────────────────────────────────────────────────────┐
  │ 1. 使用长连接（根本解决方案）                             │
  │    HTTP Keep-Alive、数据库连接池、gRPC 长连接            │
  │    → 减少连接创建和关闭的频率                             │
  │                                                          │
  │ 2. 调大 tcp_max_tw_buckets                               │
  │    sysctl -w net.ipv4.tcp_max_tw_buckets=524288         │
  │    → 允许更多 TIME_WAIT（治标）                          │
  │                                                          │
  │ 3. 启用 tcp_tw_reuse（安全复用）                         │
  │    sysctl -w net.ipv4.tcp_tw_reuse=1                    │
  │    → 允许新连接复用 TIME_WAIT 的端口                     │
  │    → 需要同时启用 tcp_timestamps                         │
  │                                                          │
  │ 4. ★ 不要启用 tcp_tw_recycle（已移除）                   │
  │    → Linux 4.12 已移除此参数                             │
  │    → 在 NAT 环境下会导致连接异常                          │
  │                                                          │
  │ 5. 客户端端口范围扩展                                     │
  │    sysctl -w net.ipv4.ip_local_port_range='1024 65535'  │
  │    → 增加可用端口数（从约 2.8 万增到约 6.4 万）          │
  └──────────────────────────────────────────────────────────┘
```

### 网络诊断命令速查表

```
问题                          │ 命令                                    │ 看什么
──────────────────────────────┼─────────────────────────────────────────┼───────────────────
CPU 高但吞吐正常              │ mpstat -P ALL 1                        │ %soft 列
中断分布不均                  │ cat /proc/interrupts \| grep eth       │ 各 CPU 的中断计数
TIME_WAIT 过多                │ ss -tn state time-wait \| wc -l        │ 数量 > 数万
CLOSE_WAIT 堆积               │ ss -tn state close-wait                │ > 0 = 应用 Bug
重传率高                      │ netstat -s \| grep retransmit          │ 重传率 > 1%
SYN flood 攻击                │ ss -tn state syn-recv \| wc -l         │ > 100 需警惕
TCP 缓冲区限制                │ cat /proc/sys/net/ipv4/tcp_rmem        │ 最小/默认/最大
拥塞算法                      │ cat /proc/sys/net/ipv4/tcp_congestion  │ cubic vs bbr
RPS 配置                      │ cat /sys/class/net/eth0/queues/rx-*/rps_cpus │ 是否启用
socket 内存                   │ ss -s                                  │ Total 和 mem 行
```

### ★ ss + BPF/eBPF 联动 — 高性能网络诊断

```
★ 大规模连接场景下，ss 的 BPF 过滤 + eBPF 追踪是最佳组合

阶段 1：ss 基础检查
────────────────────
  ss -tna → TCP 状态分布
  ss -s → socket 统计摘要
  ss -tlnp → 端口监听 + 进程归属

  ┌────────────────────────────────────────────────────────┐
  │ TIME_WAIT 过多 → 端口耗尽风险                          │
  │ CLOSE_WAIT 堆积 → 应用层未 close()                     │
  │ SYN_RECV 堆积 → 可能 SYN flood 攻击                   │
  │ ESTABLISHED 过多 → 长连接泄漏？                        │
  └────────────────────────────────────────────────────────┘

阶段 2：ss BPF 过滤 → 内核侧高效查询
─────────────────────────────────────
  ★ ss 的 BPF 过滤在内核侧执行，比 grep 快得多

  # 只看 443 端口的已建立连接（内核侧过滤）
  ss -tn state established '( dport = :443 or sport = :443 )'

  # 只看某 IP 的所有连接
  ss -tn '( dst 192.168.1.100 )'

  # 只看某进程的 socket
  ss -tnp '( sport = :8080 )'

  ★ 性能对比（10 万连接）：
    ss -tn | grep :443         → ~3 秒（全量输出 + 用户态过滤）
    ss -tn '( dport = :443 )'  → ~0.1 秒（内核侧 BPF 过滤）

阶段 3：bpftrace / BCC → eBPF 级别的网络追踪
─────────────────────────────────────────────
  ★ 当 ss 只能看到"结果"而看不到"过程"时，eBPF 追踪"过程"

  # BCC 工具：追踪新 TCP 连接创建
  tcpconnect -t
  → 输出每个新连接的时间、源/目标地址、进程

  # BCC 工具：追踪 TCP 重传
  tcpretrans -t
  → 输出每次 TCP 重传的详细信息

  # BCC 工具：追踪 TCP 连接接受
  tcpaccept -t
  → 输出每个被接受的连接

  # bpftrace：追踪 TCP 状态变化
  bpftrace -e 'kprobe:tcp_set_state { printf("%s -> %s\n", arg1, arg2); }'

  # bpftrace：统计各状态的连接创建速率
  bpftrace -e 'kprobe:tcp_set_state { @state[arg2] = count(); }
    interval:s:1 { print(@state); clear(@state); }'

★ 联动流程总结：
  ┌────────────────────────────────────────────────────────┐
  │ ss -tna → 发现异常连接状态                              │
  │   ↓                                                    │
  │ ss BPF 过滤 → 精确定位问题连接（内核侧高效）            │
  │   ↓                                                    │
  │ bpftrace/BCC → 追踪连接创建/关闭/重传的"过程"          │
  │   ↓                                                    │
  │ tcpdump → 包级别诊断（最终手段）                        │
  └────────────────────────────────────────────────────────┘
```

---

## 4. 核心概念索引

### TCP 连接的生命周期

```
┌─────────────────────────────────────────────────────────────────────┐
│                     TCP 连接建立（三次握手）                         │
│                                                                     │
│  客户端                          服务端                              │
│  ──────                          ──────                              │
│  CLOSED → SYN_SENT               LISTEN → SYN_RECV                  │
│           │ send SYN                         │ recv SYN              │
│           │ ─────────────────────────────►    │ send SYN+ACK         │
│           │                                   │ ──────────────►      │
│           │ recv SYN+ACK                      │                      │
│           │ ◄─────────────────────────────     │                      │
│           │ send ACK                          │ recv ACK             │
│           │ ─────────────────────────────►    │                      │
│  SYN_SENT → ESTABLISHED            SYN_RECV → ESTABLISHED           │
├─────────────────────────────────────────────────────────────────────┤
│                     TCP 连接关闭（四次挥手）                         │
│                                                                     │
│  主动关闭方                      被动关闭方                          │
│  ──────────                      ──────────                          │
│  ESTABLISHED → FIN_WAIT_1        ESTABLISHED                        │
│           │ send FIN                         │ recv FIN              │
│           │ ─────────────────────────────►    │                      │
│           │                                   │ ESTABLISHED →        │
│           │                                   │   CLOSE_WAIT         │
│           │ recv ACK                          │                      │
│           │ ◄─────────────────────────────     │                      │
│  FIN_WAIT_1 → FIN_WAIT_2                     │ send ACK             │
│           │                                   │ ──────────────►      │
│           │ recv FIN                          │                      │
│           │ ◄─────────────────────────────     │ CLOSE_WAIT →         │
│           │                                   │   LAST_ACK           │
│           │ send ACK                          │ send FIN             │
│           │ ─────────────────────────────►    │ ──────────────►      │
│  FIN_WAIT_2 → TIME_WAIT                      │                      │
│       (2MSL = 60s)                           │ recv ACK             │
│           │                                   │ LAST_ACK → CLOSED    │
│           ▼                                   │                      │
│        CLOSED                                 │                      │
└─────────────────────────────────────────────────────────────────────┘
```

### socket 缓冲区

```
发送端 socket buffer                  接收端 socket buffer
┌──────────────────────┐             ┌──────────────────────┐
│   Send-Q (wqueue)    │    网络     │   Recv-Q (rqueue)    │
│  ┌────┬────┬────┐   │ ────────►   │  ┌────┬────┬────┐   │
│  │data│data│data│   │             │  │data│data│data│   │
│  └────┴────┴────┘   │             │  └────┴────┴────┘   │
│  等待发送/重传的数据  │             │  等待应用读取的数据   │
└──────────────────────┘             └──────────────────────┘

ss 输出中的 Recv-Q / Send-Q：
  LISTEN 状态：Recv-Q = 当前 backlog 队列长度，Send-Q = backlog 上限
  ESTAB 状态：Recv-Q = 已接收未读取的字节，Send-Q = 已发送未确认的字节
```

### netlink vs /proc 性能对比

```
┌─────────────────────────────────────────────────────────────────┐
│ /proc/net/tcp 方式（netstat）                                   │
│                                                                 │
│ 1. open("/proc/net/tcp")                                        │
│ 2. read() → 内核 seq_file 逐条遍历，生成文本                    │
│    ★ 每次 read() 只返回一行（~100 字节）                        │
│    ★ 10 万连接 = 10 万次 read() 系统调用                        │
│ 3. 用户态 sscanf() 逐行解析十六进制文本                         │
│ 4. 无法过滤 → 必须全量读取                                      │
│                                                                 │
│ 总耗时：10 万连接 ≈ 2-5 秒                                      │
├─────────────────────────────────────────────────────────────────┤
│ netlink INET_DIAG 方式（ss）                                    │
│                                                                 │
│ 1. socket(AF_NETLINK, SOCK_DGRAM, NETLINK_INET_DIAG)           │
│ 2. sendmsg() → 发送查询请求（可带过滤条件）                     │
│ 3. recvmsg() → 内核批量返回二进制结构体                         │
│    ★ 每次 recvmsg() 返回数十条记录（~8KB 缓冲区）              │
│    ★ 10 万连接 ≈ 几百次 recvmsg()                               │
│ 4. 直接解析 struct inet_diag_msg（无文本解析开销）              │
│                                                                 │
│ 总耗时：10 万连接 ≈ 0.1-0.5 秒                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 5. 动手实验清单

### 实验 1: 基础连接查看

```bash
#!/bin/bash
# experiments/netstat/exp1_basic.sh

# 1. 查看所有 TCP 监听端口
echo "=== ss -tlnp ==="
ss -tlnp

# 2. 对比 netstat
echo "=== netstat -tlnp ==="
netstat -tlnp

# 3. 查看所有已建立的连接
echo "=== ss -tnp (ESTABLISHED) ==="
ss -tnp

# 4. 查看 socket 统计摘要
echo "=== ss -s ==="
ss -s

# 5. 查看协议统计
echo "=== netstat -s | head -40 ==="
netstat -s | head -40
```

### 实验 2: TCP 状态分布统计

```bash
#!/bin/bash
# experiments/netstat/exp2_states.sh

# 统计当前所有 TCP 连接的状态分布
echo "=== TCP 状态分布 ==="
ss -tna | awk 'NR>1{print $1}' | sort | uniq -c | sort -rn

# 查看 TIME_WAIT 连接
echo "=== TIME_WAIT 连接 ==="
ss -tn state time-wait

# 查看 CLOSE_WAIT 连接
echo "=== CLOSE_WAIT 连接 ==="
ss -tn state close-wait

# 查看 SYN_RECV 连接（如有大量，可能遭受 SYN flood）
echo "=== SYN_RECV 连接 ==="
ss -tn state syn-recv
```

### 实验 3: 连接数排行榜

```bash
#!/bin/bash
# experiments/netstat/exp3_top_connections.sh

# 按远端 IP 统计连接数 Top 10
echo "=== 远端 IP 连接数 Top 10 ==="
ss -tn | awk 'NR>1{print $5}' | cut -d: -f1 | sort | uniq -c | sort -rn | head -10

# 按本地端口统计连接数
echo "=== 本地端口连接数 ==="
ss -tn | awk 'NR>1{print $4}' | cut -d: -f2 | sort | uniq -c | sort -rn | head -10

# 按进程统计连接数
echo "=== 进程连接数 ==="
ss -tnp | awk -F'"' '{print $2}' | sort | uniq -c | sort -rn | head -10
```

### 实验 4: 端口占用排查

```bash
#!/bin/bash
# experiments/netstat/exp4_port_check.sh

# 查找谁占用了特定端口
PORT=${1:-80}

echo "=== 端口 $PORT 占用情况 ==="
ss -tlnp "sport = :$PORT"
echo ""
ss -ulnp "sport = :$PORT"

echo "=== netstat 对照 ==="
netstat -tlnp | grep ":$PORT "
```

### 实验 5: socket 内存分析

```bash
#!/bin/bash
# experiments/netstat/exp5_memory.sh

# 查看 TCP 内存使用
echo "=== ss -tm ==="
ss -tm

# 找出内存占用最大的 socket
echo "=== 内存占用 Top 5 ==="
ss -tm | grep skmem | sort -t',' -k2 -rn | head -5

# 查看内核 TCP 内存统计
echo "=== 内核 TCP 内存限制 ==="
cat /proc/sys/net/ipv4/tcp_mem
```

### 实验 6: 对比 netstat 与 ss 的性能

```bash
#!/bin/bash
# experiments/netstat/exp6_performance.sh

echo "=== netstat 耗时 ==="
time netstat -tn > /dev/null

echo "=== ss 耗时 ==="
time ss -tn > /dev/null

echo "=== 连接总数 ==="
ss -tn | wc -l
```

---

## 参考资源

- [RFC 793 - Transmission Control Protocol](https://datatracker.ietf.org/doc/html/rfc793)
- [RFC 9293 - TCP (updated)](https://datatracker.ietf.org/doc/html/rfc9293)
- [ss(8) man page](https://man7.org/linux/man-pages/man8/ss.8.html)
- [netstat(8) man page](https://man7.org/linux/man-pages/man8/netstat.8.html)
- [iproute2 ss 源码](https://git.kernel.org/pub/scm/network/iproute2/iproute2.git/tree/misc/ss.c)
- [Linux netlink 接口](https://man7.org/linux/man-pages/man7/netlink.7.html)
- [INET_DIAG 协议](https://man7.org/linux/man-pages/man7/sock_diag.7.html)
- [TCP 状态机图解 (Wikipedia)](https://en.wikipedia.org/wiki/Transmission_Control_Protocol#Protocol_operation)
