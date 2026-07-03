# netstat + ss 问答集

> 学习网络诊断工具过程中遇到的问题与解答。

---

## Q1: 网络吞吐正常但 CPU 高怎么排查？

**日期：** 2026-07-04
**场景：** AMD 服务器，网络带宽利用率正常（如 10Gbps 跑满），但 CPU 使用率异常高
**相关文件：** reading/02_command_reference.md（netstat -s 协议统计）

### 回答

这是高性能网络场景下的**经典问题**。根因通常是**软中断（softirq）集中在少数 CPU 核心**上。

#### 诊断步骤

```
Step 1：确认 CPU 消耗分布
───────────────────────────
  mpstat -P ALL 1
  
  重点看 %soft 列：
  ┌────────────────────────────────────────────────────────┐
  │ CPU0: %soft=45%  ← ★ 软中断高！                       │
  │ CPU1: %soft=3%                                         │
  │ CPU2: %soft=1%                                         │
  │ CPU3: %soft=0%                                         │
  │                                                        │
  │ 结论：软中断集中在 CPU0                                 │
  └────────────────────────────────────────────────────────┘

Step 2：确认中断来源
──────────────────────
  cat /proc/interrupts | grep -E 'CPU|eth'
  
  输出示例：
             CPU0       CPU1       CPU2       CPU3
    34:  45678901          0          0          0   eth0-rx-0
    35:         0   12345678          0          0   eth0-tx-0
  
  ★ 问题：eth0 的 rx 中断全部由 CPU0 处理
    10Gbps 网络 ≈ 每秒数百万个包
    每个包触发一次软中断 → CPU0 的 %soft 极高

Step 3：查看网络协议统计
──────────────────────────
  netstat -s | grep -E 'packet|soft|error'
  
  关注：
  - 总收包数（确认 PPS 是否很高）
  - 软中断相关错误
  - 丢包计数
```

#### 根因与解决方案

```
根因 1：单队列网卡 + 中断集中
──────────────────────────────
  解决：
  a) 启用 irqbalance 自动均衡
     systemctl enable --now irqbalance
  
  b) 手动设置 IRQ 亲和性
     echo 0f > /proc/irq/<IRQ_NUM>/smp_affinity
  
  c) 启用多队列网卡（如果硬件支持）
     ethtool -L eth0 combined $(nproc)

根因 2：多队列已启用但 RPS 未配置
──────────────────────────────────
  解决：启用 RPS（Receive Packet Steering）
  echo ff > /sys/class/net/eth0/queues/rx-0/rps_cpus
  
  ★ RPS 在软件层模拟多队列，将收包负载分散到多个 CPU

根因 3：小包场景（PPS 高但带宽不高）
──────────────────────────────────────
  每个包的中断开销是固定的（~数千 CPU 周期）
  小包场景下 CPU 消耗 = PPS × 每包开销
  
  解决：
  a) GRO/GSO 合并（减少包数）
     ethtool -K eth0 gro on gso on tso on
  b) NAPI busy polling
     echo 2 > /sys/class/net/eth0/napi_defer_hard_irqs
  c) 使用 DPDK/XDP 绕过内核协议栈
```

#### ★ 结论

**网络吞吐正常但 CPU 高 → 首先检查软中断分布。** 用 `mpstat -P ALL 1` 看各 CPU 的 %soft，用 `/proc/interrupts` 确认中断分布。解决方案的核心思路是**将网络处理负载分散到多个 CPU**。

---

## Q2: TIME_WAIT 过多怎么优化？

**日期：** 2026-07-04
**场景：** `ss -s` 显示 timewait 数量达到数万，担心端口耗尽
**相关文件：** reading/01_tcp_state_machine.md（TIME_WAIT 成因与排查）

### 回答

#### TIME_WAIT 过多的影响

```
TIME_WAIT 的危害：
  1. 端口耗尽
     每个 TIME_WAIT 占用一个本地端口
     默认端口范围 32768-60999 = 约 28231 个端口
     TIME_WAIT 持续 60 秒（2MSL）
     如果 60 秒内建立 > 28231 个短连接 → 端口耗尽，新连接失败
  
  2. 内存占用
     每个 TIME_WAIT socket 约占 0.5KB 内存
     10 万个 TIME_WAIT ≈ 50MB（通常不是主要问题）
  
  3. 查找效率
     大量 TIME_WAIT 增加内核 socket hash 表的查找时间

检查当前状态：
  ss -s                    # 看 timewait 数量
  ss -tn state time-wait | wc -l
  cat /proc/sys/net/ipv4/tcp_max_tw_buckets  # 上限
  cat /proc/sys/net/ipv4/ip_local_port_range  # 端口范围
```

#### 优化方案（按优先级）

```
方案 1：使用长连接（★ 根本解决）
─────────────────────────────────
  问题根源是短连接频繁创建和关闭。
  
  HTTP：启用 Keep-Alive（默认已启用），设置合理的超时
  数据库：使用连接池（如 HikariCP、PgBouncer）
  RPC：使用 gRPC 长连接替代 HTTP 短连接
  
  效果：TIME_WAIT 从数万降到数百

方案 2：扩展端口范围
──────────────────────
  sysctl -w net.ipv4.ip_local_port_range='1024 65535'
  
  可用端口：65535 - 1024 = 64511 个（原来约 28231 个）
  
  ★ 注意：端口 < 1024 是特权端口，需要 root 才能绑定
    所以 1024 是安全下限

方案 3：启用 tcp_tw_reuse
──────────────────────────
  sysctl -w net.ipv4.tcp_tw_reuse=1
  
  允许新连接复用 TIME_WAIT 状态的 socket（如果安全的话）
  "安全"条件：新连接的 timestamp > 旧连接的最后 timestamp
  
  ★ 前提：必须启用 tcp_timestamps
  cat /proc/sys/net/ipv4/tcp_timestamps  # 应为 1

方案 4：调大 tcp_max_tw_buckets
─────────────────────────────────
  sysctl -w net.ipv4.tcp_max_tw_buckets=524288
  
  允许更多 TIME_WAIT 存在（治标不治本）
  超过上限后内核会打印警告并强制回收

★ 不要使用的方案：
  tcp_tw_recycle → Linux 4.12 已移除
  在 NAT 环境下会导致连接异常（多个客户端共享同一 IP 时 timestamp 乱序）
```

#### 持久化配置

```bash
# /etc/sysctl.d/99-network.conf
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_max_tw_buckets = 524288
net.ipv4.tcp_timestamps = 1
net.ipv4.tcp_fin_timeout = 30

# 应用配置
sysctl -p /etc/sysctl.d/99-network.conf
```

#### 监控建议

```bash
# 定期检查 TIME_WAIT 数量
ss -s | grep timewait

# 设置告警阈值（如 > 10000）
TW_COUNT=$(ss -tn state time-wait | wc -l)
if [ $TW_COUNT -gt 10000 ]; then
    echo "WARNING: TIME_WAIT=$TW_COUNT"
fi
```

#### ★ 结论

**TIME_WAIT 过多是短连接场景的常见问题。** 根本解决方案是使用长连接和连接池。临时方案是扩展端口范围 + 启用 tcp_tw_reuse。不要使用已移除的 tcp_tw_recycle。
