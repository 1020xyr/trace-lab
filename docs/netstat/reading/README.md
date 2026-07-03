# netstat + ss 阅读指南

> ★ **netstat/ss 是 Linux 网络诊断的基础工具** — 从 TCP 状态机到 socket 统计，快速定位连接、端口、协议问题。
> 在性能分析方法论中，netstat/ss 位于"网络性能诊断"链路的第一层：连接状态检查。

本目录包含 netstat/ss 的深入学习材料，涵盖 TCP 状态机、命令参考与实战解析。
按 01 → 03 的顺序阅读。

**预计总阅读时间：** 2.5-3.5 小时（快速入门 25 分钟，深入理解 2 小时）

---

## ★ netstat/ss 在性能分析中的定位

```
┌─────────────────────────────────────────────────────────────────┐
│                    网络性能诊断分层                               │
│                                                                  │
│  第 1 层：连接状态 ← ★ netstat/ss 在这里                        │
│    ss -tna → TCP 状态分布、TIME_WAIT/CLOSE_WAIT 数量            │
│    ss -s → socket 统计摘要                                       │
│    ss -tlnp → 端口监听 + 进程归属                               │
│                                                                  │
│  第 2 层：中断与 CPU                                             │
│    /proc/interrupts → 网卡中断在各 CPU 的分布                    │
│    mpstat -P ALL 1 → 各 CPU 的 %soft（软中断占比）              │
│                                                                  │
│  第 3 层：协议参数                                               │
│    sysctl → TCP 缓冲区、拥塞算法、RPS 配置                       │
│    netstat -s → 重传率、SYN drops、超时统计                      │
│                                                                  │
│  第 4 层：抓包分析                                               │
│    tcpdump / tshark → 包级别诊断                                 │
│    bpftrace → eBPF 网络追踪                                      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 阅读路线

| 序号 | 文件 | 内容 | 关注点 | 预计时间 |
|------|------|------|--------|---------|
| **01** | [01_tcp_state_machine.md](./01_tcp_state_machine.md) | TCP 状态机详解 | ★ 11 个状态、三次握手/四次挥手、状态转换图 | 25 分钟 |
| **02** | [02_command_reference.md](./02_command_reference.md) | 完整命令参考 | ★ netstat/ss 全部参数、10+ 场景命令组合、数据源原理 | 30 分钟 |
| **03** | [03_command_output_demo.md](./03_command_output_demo.md) | 输出实战解析 | ★★ 9 个场景的真实输出 + 逐字段注解 | 40 分钟 |

---

## 阅读建议

1. **先读 01（TCP 状态机）**：这是理解 ss/netstat 输出的基础 — 每个连接的 State 列就是 TCP 状态
2. **再读 02（命令参考）**：掌握 netstat/ss 的完整参数和数据源原理（/proc/net/tcp vs netlink），遇到具体场景可回查
3. **★ 最后 03（输出实战）**：对照真实输出逐行理解，结合自己的系统练习

---

## 阅读路径推荐

### 快速入门（25 分钟）

```
01_tcp_state_machine  → 理解 11 个 TCP 状态
03_command_output_demo → 场景 1（ss -tlnp）+ 场景 2（ss -s）
```

### 深入理解（2 小时）

```
01 → 02（速查）→ 03（全部 9 个场景）
+ learning-roadmap.md 中的网络诊断专题
```

---

## ★ 诊断场景速查

| 问题 | 命令 | 看什么 |
|------|------|--------|
| 端口被谁占了 | `ss -tlnp 'sport = :80'` | Process 列 |
| TIME_WAIT 过多 | `ss -tn state time-wait \| wc -l` | 数量 > 数千需关注 |
| CLOSE_WAIT 堆积 | `ss -tn state close-wait` | 有 → 应用层没 close |
| 连接数排行 | `ss -tn \| awk '{print $5}' \| cut -d: -f1 \| sort \| uniq -c` | 连接最多的远端 IP |
| socket 内存占用 | `ss -tm` | skmem 中的 rb/tb |
| keepalive 状态 | `ss -to` | timer 列 |
| 重传率高 | `netstat -s \| grep retransmit` | 重传率 > 1% |

---

## ★ 与其他工具的交叉引用

### ss/netstat → ss + BPF — 高性能网络诊断

```
★ 大规模连接场景下的诊断路径

1. ss -tna → 基础连接状态检查
   ├── TIME_WAIT 过多 → 端口耗尽风险
   ├── CLOSE_WAIT 堆积 → 应用层未 close()
   └── SYN_RECV 堆积 → 可能 SYN flood 攻击
   │
   ▼
2. ss 的 BPF 过滤 → 内核侧高效过滤
   ss -tn state established '( dport = :443 or sport = :443 )'
   → 比 grep 快得多（内核侧过滤，不拷贝无关数据到用户态）
   │
   ▼
3. bpftrace / BCC → eBPF 级别的网络追踪
   ├── tcpconnect → 追踪新连接创建
   ├── tcpaccept → 追踪连接接受
   ├── tcpretrans → 追踪 TCP 重传
   └── sockops → socket 操作追踪
```

### ss/netstat → mpstat — 网络软中断分析

```
1. ss -tn → 确认网络连接数和分布
2. mpstat -P ALL 1 → 各 CPU 的 %soft 是否均衡
3. cat /proc/interrupts → 网卡中断分布
4. 不均衡 → 配置 RPS/IRQ 亲和性
```

### ss/netstat → tcpdump — 包级别诊断

```
1. ss -tn → 发现异常连接（如大量 ESTABLISHED 但无数据传输）
2. tcpdump -i eth0 port 80 -w capture.pcap → 抓包
3. tshark -r capture.pcap → 分析包级别的问题（重传、窗口满、RST）
```

### ss/netstat → sar — 历史趋势

```
1. sar -n DEV → 各网卡的历史流量趋势
2. sar -n EDEV → 网络错误统计（丢包、溢出）
3. ss -tn → 当前实时连接状态
```

---

## 文件说明

- 01 为概念性文档，帮助建立 TCP 知识框架
- 02-03 为实操文档，所有命令输出均来自真实系统
- QA.md 为问答集，记录学习中遇到的问题
- ★ 标记核心重点，表格用于状态对照和命令速查

---

## 相关资源

- [learning-roadmap.md](../learning-roadmap.md) — 完整学习路线和实验清单
- [QA.md](../QA.md) — 常见问题问答集
- [sar reading](../sar/reading/) — 网络历史趋势：sar -n DEV/EDEV 分析
- [nginx reading](../nginx/reading/) — 应用层网络诊断：nginx 连接处理与性能调优
- [pidstat reading](../pidstat/reading/) — 进程级定位：哪个进程的网络 I/O 最多
