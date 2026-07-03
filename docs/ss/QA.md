# ss（socket statistics）问答集

> 持续追加的技术问答，记录学习和使用中遇到的问题

---

## 目录

- [Q1: Recv-Q 和 Send-Q 在 LISTEN 状态下为什么含义不同？](#q1-recv-q-和-send-q-在-listen-状态下为什么含义不同)
- [Q2: ss -i 显示的 cwnd 和 rtt 是什么？如何利用这些信息调优 TCP？](#q2-ss--i-显示的-cwnd-和-rtt-是什么如何利用这些信息调优-tcp)

---

## Q1: ss 输出中 Recv-Q 和 Send-Q 在 LISTEN 状态下为什么含义不同？

**日期：** 2026-07-04
**场景：** 使用 `ss -tln` 查看监听端口时，对 Recv-Q 和 Send-Q 的含义产生疑惑
**相关文件：** `reading/01_ss_internals.md`、`reading/03_command_output_demo.md`

### 回答

在 ss 输出中，Recv-Q 和 Send-Q 的含义取决于 socket 的状态：

| 状态 | Recv-Q 含义 | Send-Q 含义 |
|------|------------|------------|
| **LISTEN** | accept 队列中已完成三次握手的连接数 | listen() 的 backlog 参数（队列上限） |
| **ESTAB** | 接收缓冲区中未被应用 read() 的字节数 | 发送缓冲区中未被对端 ACK 的字节数 |

#### 内核源码依据

ss 获取的 `inet_diag_msg` 结构体中，rqueue/wqueue 的赋值在 `tcp_diag.c` 中：

```c
// 内核源码：src/linux-5.10/net/ipv4/tcp_diag.c:18-35
// tcp_diag_get_info() 函数（★ 注意不是 inet_diag.c）

static void tcp_diag_get_info(struct sock *sk, struct inet_diag_msg *r,
                              void *_info)
{
    struct tcp_info *info = _info;

    if (inet_sk_state_load(sk) == TCP_LISTEN) {
        // ★ LISTEN 状态：Recv-Q = accept 队列长度，Send-Q = backlog
        r->idiag_rqueue = READ_ONCE(sk->sk_ack_backlog);      // 已完成的连接数
        r->idiag_wqueue = READ_ONCE(sk->sk_max_ack_backlog);  // backlog 上限
    } else if (sk->sk_type == SOCK_STREAM) {
        const struct tcp_sock *tp = tcp_sk(sk);
        // ★ 其他状态：Recv-Q = 接收队列字节数，Send-Q = 发送队列字节数
        r->idiag_rqueue = max_t(int, READ_ONCE(tp->rcv_nxt) -
                                     READ_ONCE(tp->copied_seq), 0);
        r->idiag_wqueue = READ_ONCE(tp->write_seq) - tp->snd_una;
    }
    if (info)
        tcp_get_info(sk, info);  // 填充 tcp_info（ss -i 的数据来源）
}
```

**关键字段解读：**
- `sk_ack_backlog`：当前 accept 队列中已完成三次握手的连接数
- `sk_max_ack_backlog`：listen() 设置的 backlog 上限
- `rcv_nxt - copied_seq`：已到达内核但尚未被用户态 read() 拷贝的字节数
- `write_seq - snd_una`：已发送但尚未被对端 ACK 确认的字节数

#### ★ 性能分析要点

当 LISTEN 状态的 Recv-Q 接近 Send-Q 时：
- 说明 accept 队列即将满
- 新的三次握手完成后会被丢弃（或进入 SYN_RECV 等待）
- 此时应检查：
  1. 应用 accept() 是否太慢
  2. backlog 是否设置太小
  3. somaxconn 是否限制了 backlog

验证命令：
```bash
# 检查 somaxconn
cat /proc/sys/net/core/somaxconn  # 本机 = 4096

# 检查 listen overflow
cat /proc/net/netstat | grep -i listenoverflows
```

---

## Q2: ss -i 显示的 cwnd 和 rtt 是什么？如何利用这些信息调优 TCP？

**日期：** 2026-07-04
**场景：** 使用 `ss -ti` 查看 TCP 连接参数时，看到 cwnd 和 rtt 值，想知道它们的数据来源和调优方法
**相关文件：** `reading/02_command_reference.md`、`reading/03_command_output_demo.md`

### 回答

**cwnd（拥塞窗口）和 rtt（往返时延）是 TCP 性能分析的两大核心指标。** 它们直接来自内核 `tcp_sock` 结构体，由 `tcp_get_info()` 函数填充到 `tcp_info` 中，再通过 netlink 接口传递给 ss。

#### 数据来源：内核源码

**1. tcp_info 结构体定义（UAPI 接口）**

```c
// 源码：src/linux-5.10/include/uapi/linux/tcp.h:214
struct tcp_info {
    __u8    tcpi_state;
    // ... 省略中间字段 ...
    __u32   tcpi_rtt;          // ★ RTT（微秒）， smoothed RTT
    __u32   tcpi_rttvar;       // RTT 方差
    // ...
    __u32   tcpi_snd_cwnd;     // ★ 拥塞窗口（MSS 个数）
    // ...
    __u32   tcpi_rcv_rtt;      // 接收端估算的 RTT
    __u32   tcpi_rcv_space;    // 接收缓冲区空间
    __u32   tcpi_total_retrans; // 总重传次数
    // ...
    __u32   tcpi_min_rtt;      // 最小 RTT
    __u64   tcpi_delivery_rate; // 实际交付速率
};
```

**2. tcp_get_info() 填充逻辑**

```c
// 源码：src/linux-5.10/net/ipv4/tcp.c:3364-3480
void tcp_get_info(struct sock *sk, struct tcp_info *info)
{
    const struct tcp_sock *tp = tcp_sk(sk);
    // ...
    // ★ cwnd：直接从 tcp_sock 读取当前拥塞窗口
    info->tcpi_snd_cwnd = tp->snd_cwnd;  // 行 3389

    // ★ rtt：srtt_us 右移 3 位（÷8），因为内核用 8 倍精度存储
    info->tcpi_rtt = tp->srtt_us >> 3;   // 行 3443

    // ★ rttvar：mdev_us 右移 2 位（÷4）
    info->tcpi_rttvar = tp->mdev_us >> 2; // 行 3444

    // ★ 接收端 RTT
    info->tcpi_rcv_rtt = tp->rcv_rtt_est.rtt_us >> 3;  // 行 3451

    // ★ 最小 RTT
    info->tcpi_min_rtt = tcp_min_rtt(tp);  // 行 3464
    // ...
}
```

**3. ss 如何获取这些数据**

```c
// 源码：src/linux-5.10/net/ipv4/tcp_diag.c:18-36
static void tcp_diag_get_info(struct sock *sk, struct inet_diag_msg *r,
                              void *_info)
{
    struct tcp_info *info = _info;
    // ... 填充 rqueue/wqueue ...
    if (info)
        tcp_get_info(sk, info);  // ★ 调用 tcp_get_info 填充 tcp_info
}
```

ss 通过 `NETLINK_SOCK_DIAG` → `TCP_DIAG_GETSOCK` → `tcp_diag_get_info()` → `tcp_get_info()` 链路获取数据。

#### ★ 关键指标解读表

| ss -i 字段 | 内核变量 | 含义 | 单位 | 初始值 |
|-----------|---------|------|------|-------|
| `cwnd` | `tp->snd_cwnd` | 拥塞窗口（可发送的最大未确认段数） | MSS 个数 | 10（`TCP_INIT_CWND`） |
| `rtt` | `tp->srtt_us >> 3` | 平滑往返时延 | 微秒 | 首次测量值 |
| `rttvar` | `tp->mdev_us >> 2` | RTT 偏差（波动大小） | 微秒 | — |
| `minrtt` | `tcp_min_rtt(tp)` | 历史最小 RTT（≈ 真实传播延迟） | 微秒 | — |
| `ssthresh` | `tp->snd_ssthresh` | 慢启动阈值 | MSS 个数 | — |
| `retrans` | `tp->retrans_out` | 当前未确认的重传段数 | 段数 | — |
| `send` | 计算值 | 理论发送速率 | bps | — |
| `delivery_rate` | `tcp_compute_delivery_rate()` | 实际交付速率 | bps | — |

**初始值源码确认：**

```c
// 源码：src/linux-5.10/include/net/tcp.h:229
#define TCP_INIT_CWND   10    // ★ 初始拥塞窗口 = 10 个 MSS

// 源码：src/linux-5.10/net/ipv4/tcp.c:431
tp->snd_cwnd = TCP_INIT_CWND;
```

#### 如何利用 cwnd 和 rtt 调优 TCP

**场景 1：cwnd 很小（< 10）+ 有重传 → 丢包导致拥塞窗口缩减**

```bash
# 查看所有连接的 cwnd 分布
ss -ti state established | grep -oP 'cwnd:\K[0-9]+' | sort -n

# cwnd 普遍很小 → 网络丢包严重
# 排查：
#   1. ping -c 100 <目标IP>  → 看丢包率
#   2. mtr <目标IP>          → 看哪跳丢包
#   3. 检查 MTU 是否匹配：ip link show
```

**场景 2：rtt 很高（> 100ms）→ 高延迟链路**

```bash
# 提取所有连接的 rtt
ss -ti state established | grep -oP 'rtt:\K[0-9.]+' | sort -n

# 高 RTT 链路的优化：
#   1. 增大初始窗口：sysctl -w net.ipv4.tcp_init_cwnd=20
#   2. 启用 TCP BBR 拥塞控制（对高 RTT 链路效果好）：
#      sysctl net.ipv4.tcp_congestion_control=bbr
#   3. 增大窗口缩放因子：
#      sysctl -w net.core.rmem_max=16777216
#      sysctl -w net.core.wmem_max=16777216
```

**场景 3：delivery_rate 远小于 send → 应用层限速（app_limited）**

```bash
# ss -ti 输出中出现 app_limited 标记
# 说明不是网络瓶颈，而是应用发送数据不够快
# 优化方向：应用层（减少处理延迟、批量发送）
```

**场景 4：rtt 正常但 cwnd 上不去 → 拥塞控制算法问题**

```bash
# 查看当前拥塞控制算法
sysctl net.ipv4.tcp_congestion_control

# 推荐算法：
#   - bbr：适合高带宽高延迟链路（★ 推荐）
#   - cubic：Linux 默认，通用场景
#   - dctcp：数据中心低延迟场景

# 切换到 BBR（需要内核 ≥ 4.9）：
modprobe tcp_bbr
sysctl -w net.ipv4.tcp_congestion_control=bbr
```

#### ★ 实战诊断流程图

```
ss -ti state established
│
├── cwnd < 10 + retrans > 0
│   → 丢包问题 → mtr 定位丢包节点
│
├── rtt > 100ms
│   → 高延迟链路 → 启用 BBR + 增大 buffer
│
├── rttvar 很大（> rtt 的 50%）
│   → 网络抖动大 → 检查网络链路质量
│
├── minrtt 低但 rtt 高
│   → 拥塞导致排队 → 检查中间设备 buffer
│
├── delivery_rate << send + app_limited
│   → 应用层限速 → 优化应用发送逻辑
│
└── cwnd 大 + rtt 低 + delivery_rate 高
    → TCP 状态健康 → 瓶颈在其他地方
```

**一句话总结：** `ss -ti` 的 cwnd 来自 `tp->snd_cwnd`（初始值 10 MSS），rtt 来自 `tp->srtt_us >> 3`（平滑 RTT），两者是判断 TCP 连接健康度的核心指标 — cwnd 小看重传说明丢包，rtt 高看 minrtt 区分拥塞还是链路延迟。

---

<!-- 后续问答在此追加 -->
