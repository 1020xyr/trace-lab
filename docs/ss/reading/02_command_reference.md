# ss 命令参考 — 完整参数、过滤器与场景

```
======================================================================
Step 2: ss 命令参考
======================================================================
阅读要点：
  1. 所有参数按功能分类
  2. 过滤器完整语法与示例
  3. ss 替代 netstat 命令对照表（20+ 条）
  4. 12+ 个场景命令
预计阅读时间：20 分钟
======================================================================
```

---

## 目录

- [1. 参数分类速查](#1-参数分类速查)
- [2. 协议过滤参数](#2-协议过滤参数)
- [3. 状态与信息级别参数](#3-状态与信息级别参数)
- [4. 过滤器语法详解](#4-过滤器语法详解)
- [5. ★ ss 替代 netstat 命令对照表](#5--ss-替代-netstat-命令对照表)
- [6. 场景命令集](#6-场景命令集)
- [7. 输出格式控制](#7-输出格式控制)

---

## 1. 参数分类速查

```
ss [选项] [过滤器]

选项分为 4 大类：

┌───────────────────────────────────────────────────────────────┐
│  ① 协议过滤    -t/-u/-x/-w/-4/-6     选择查看哪种 socket     │
│  ② 状态过滤    -l/-a                  选择显示哪些状态        │
│  ③ 信息级别    -o/-e/-m/-p/-i/-n/-b  控制显示多少信息        │
│  ④ 输出控制    -H/-N/-r/-K            格式化、排序、解析      │
│                                                               │
│  过滤器        state/sport/dport/dst/src  在内核或用户态过滤  │
└───────────────────────────────────────────────────────────────┘
```

---

## 2. 协议过滤参数

| 参数 | 全称 | 说明 | 对应内核协议 |
|------|------|------|-------------|
| `-t` | --tcp | ★ 只显示 TCP socket | IPPROTO_TCP |
| `-u` | --udp | 只显示 UDP socket | IPPROTO_UDP |
| `-x` | --unix | 只显示 Unix domain socket | AF_UNIX |
| `-w` | --raw | 只显示 RAW socket | IPPROTO_RAW |
| `-4` | --ipv4 | 只显示 IPv4 socket | AF_INET |
| `-6` | --ipv6 | 只显示 IPv6 socket | AF_INET6 |

### 组合使用

```bash
# TCP + IPv4（排除 IPv6）
ss -t4

# UDP + IPv4 监听
ss -u4l

# 同时查看 TCP 和 UDP（不指定则默认全部）
ss -tu
```

### 默认行为

```
不指定协议参数时：
  ss        → 显示所有协议（TCP + UDP + Unix + RAW）
  ss -l     → 所有协议的监听 socket
  ss -a     → 所有协议的所有状态

★ 最佳实践：总是指定 -t 或 -u 来缩小范围
```

---

## 3. 状态与信息级别参数

### 状态过滤

| 参数 | 说明 |
|------|------|
| `-l` / `--listening` | ★ 只显示监听状态的 socket |
| `-a` / `--all` | 显示所有状态的 socket（包括已建立和监听） |

### 信息级别参数

| 参数 | 全称 | 说明 | netlink 扩展 |
|------|------|------|-------------|
| `-n` / `--numeric` | 数字格式 | 不解析服务名和用户名 | — |
| `-r` / `--resolve` | 反向解析 | 解析 IP 为主机名 | — |
| `-p` / `--processes` | 进程信息 | ★ 显示持有 socket 的进程 | 扫描 /proc |
| `-o` / `--options` | 定时器 | ★ 显示 timer 信息 | inet_diag_msg |
| `-e` / `--extended` | 扩展信息 | ★ 显示 uid/inode/cgroup | INET_DIAG_TOS/TCLASS |
| `-m` / `--memory` | 内存信息 | ★ 显示 socket 内存使用 | INET_DIAG_SKMEMINFO |
| `-i` / `--info` | TCP 详情 | ★ 显示拥塞控制/RTT/窗口 | INET_DIAG_INFO |
| `-b` / `--bpf` | BPF 过滤器 | 显示 socket BPF 过滤器 | — |
| `-s` / `--summary` | 统计摘要 | ★ 显示全局 socket 统计 | — |
| `-Z` / `--context` | SELinux 上下文 | 显示 SELinux 安全上下文 | — |

### ★ 信息级别对性能的影响

```
不加参数：ss -tn              → 最快（只请求基本信息）
加 -p：    ss -tnp            → 较慢（需要扫描 /proc/PID/fd）
加 -i：    ss -tni            → 较慢（需要请求 tcp_info 扩展）
加 -m：    ss -tnm            → 较慢（需要请求 skmeminfo 扩展）
加 -e：    ss -tne            → 较慢（需要请求 TOS/TCLASS 扩展）

★ 组合多个信息级别：
  ss -toemp      → 同时请求 timer + 扩展 + 内存 + 进程
  ss -toempi     → 全部信息（最慢但最全）

★ 性能分析建议：
  快速检查：  ss -tn        或 ss -s
  深入排查：  ss -tnpi      或 ss -tnoempi
  脚本监控：  ss -s         或 ss -tn | wc -l
```

---

## 4. 过滤器语法详解

### 语法格式

```
ss [选项] [过滤器表达式]

过滤器表达式：
  [state <tcp-state>]
  [<condition> [and|or <condition>...]]
  [not <condition>]
```

### 完整条件列表

| 条件 | 语法 | 示例 | 内核侧执行 |
|------|------|------|-----------|
| 目的地址 | `dst <addr>` | `dst 10.0.0.1` | ✅ 精确地址 |
| 源地址 | `src <addr>` | `src 192.168.1.0/24` | ✅ 精确地址 |
| 目的端口 | `dport <op> <port>` | `dport = :443` | ✅ 等号 |
| 源端口 | `sport <op> <port>` | `sport = :22` | ✅ 等号 |
| 网络设备 | `dev <op> <device>` | `dev = eth0` | ✅ |
| fwmark | `fwmark <op> <mark>` | `fwmark = 0x1` | ✅ |

### 操作符

| 操作符 | 说明 | 内核侧执行 |
|--------|------|-----------|
| `=` | 等于 | ✅ |
| `!=` | 不等于 | ❌ 用户态 |
| `>` | 大于 | ❌ 用户态 |
| `<` | 小于 | ❌ 用户态 |
| `>=` | 大于等于 | ❌ 用户态 |
| `<=` | 小于等于 | ❌ 用户态 |

### TCP 状态名

| ss 中的名称 | 内核常量 | 数值 |
|------------|---------|------|
| `established` | TCP_ESTABLISHED | 1 |
| `syn-sent` | TCP_SYN_SENT | 2 |
| `syn-recv` | TCP_SYN_RECV | 3 |
| `fin-wait-1` | TCP_FIN_WAIT1 | 4 |
| `fin-wait-2` | TCP_FIN_WAIT2 | 5 |
| `time-wait` | TCP_TIME_WAIT | 6 |
| `closed` | TCP_CLOSE | 7 |
| `close-wait` | TCP_CLOSE_WAIT | 8 |
| `last-ack` | TCP_LAST_ACK | 9 |
| `listening` | TCP_LISTEN | 10 |
| `closing` | TCP_CLOSING | 11 |
| `all` | — | 所有 |

### 端口表示

```
数字:   :22, :80, :443, :8080
名称:   :ssh, :http, :https  （从 /etc/services 解析）
范围:   >= :1024              （用户态过滤）
```

### 地址表示

```
精确:   127.0.0.1, 10.0.0.1
CIDR:   10.0.0.0/8, 192.168.0.0/16
IPv6:   ::1, fe80::/10
通配:   *
```

### 组合示例

```bash
# 已建立的 SSH 连接
ss -tn state established sport = :22

# HTTPS 出站连接
ss -tn 'dport = :443'

# 本地回环的连接
ss -tn 'dst 127.0.0.0/8'

# HTTP 或 HTTPS
ss -tn 'dport = :80 or dport = :443'

# 非监听状态
ss -tn 'not state listening'

# 组合：SSH 端口 + 已建立
ss -tn state established 'sport = :22'

# 排除本地连接
ss -tn 'not dst 127.0.0.0/8'
```

---

## 5. ★ ss 替代 netstat 命令对照表

### 基础命令对照（20+ 条）

| # | 场景 | netstat 命令 | ss 命令 | 备注 |
|---|------|-------------|---------|------|
| 1 | 所有 TCP 连接 | `netstat -tn` | `ss -tn` | 直接替换 |
| 2 | 所有 TCP 监听 | `netstat -tln` | `ss -tln` | 直接替换 |
| 3 | TCP 监听 + 进程 | `netstat -tlnp` | `ss -tlnp` | ★ 最常用 |
| 4 | 所有 UDP 监听 | `netstat -ulnp` | `ss -ulnp` | 直接替换 |
| 5 | 所有连接（含状态） | `netstat -an` | `ss -tan` | 直接替换 |
| 6 | 所有协议所有状态 | `netstat -a` | `ss -a` | 直接替换 |
| 7 | 统计摘要 | `netstat -s` | `ss -s` | ss 输出更精炼 |
| 8 | 已建立连接 | `netstat -tn \| grep ESTAB` | `ss -tn state established` | ★ ss 内置过滤 |
| 9 | 监听端口 | `netstat -tln \| grep LISTEN` | `ss -tln` | ss 直接支持 |
| 10 | TIME_WAIT 连接 | `netstat -tn \| grep TIME` | `ss -tn state time-wait` | ★ ss 内置过滤 |
| 11 | CLOSE_WAIT 连接 | `netstat -tn \| grep CLOSE` | `ss -tn state close-wait` | ★ ss 内置过滤 |
| 12 | 特定端口连接 | `netstat -tn \| grep :80` | `ss -tn 'dport = :80'` | ★ ss 内置过滤 |
| 13 | 特定 IP 连接 | `netstat -tn \| grep 10.0.0` | `ss -tn 'dst 10.0.0.0/8'` | ★ ss 支持 CIDR |
| 14 | TCP + 进程 + 定时器 | `netstat -tnpo` | `ss -tnpo` | 直接替换 |
| 15 | TCP 内存 | 不支持 | `ss -tm` | ★ ss 独有 |
| 16 | TCP 拥塞参数 | 不支持 | `ss -ti` | ★ ss 独有 |
| 17 | 扩展信息 | 不支持 | `ss -te` | ★ ss 独有 |
| 18 | Unix socket | `netstat -x` | `ss -x` | 直接替换 |
| 19 | Unix socket + 进程 | `netstat -xp` | `ss -xp` | 直接替换 |
| 20 | RAW socket | `netstat --raw` | `ss -w` | 直接替换 |
| 21 | 路由表 | `netstat -rn` | `ip route` | ss 不支持路由 |
| 22 | 接口统计 | `netstat -i` | `ip -s link` | ss 不支持接口 |
| 23 | 多播组 | `netstat -g` | `ip maddr` | ss 不支持多播 |

### ★ 关键差异说明

```
1. netstat -s 输出的是 /proc/net/snmp 和 /proc/net/netstat 的原始数据
   ss -s 输出的是从内核 socket 层直接统计的数据
   → 两者内容不完全相同，ss -s 更精炼

2. netstat 没有 -i 选项的等价物（显示 TCP 拥塞参数）
   ss -ti 提供的 cwnd/rtt/ssthresh 信息是性能分析的★核心

3. netstat 的 -o 选项显示 keepalive timer
   ss 的 -o 选项显示所有类型的 timer（keepalive/retransmit/persist/timewait）

4. 进程信息（-p）在 ss 中需要 root 权限才能看到所有用户的进程
   非 root 用户只能看到自己的进程
```

---

## 6. 场景命令集

### 场景 1：快速查看服务监听状态

```bash
# ★ 最常用命令：TCP 监听 + 进程 + 数字格式
ss -tlnp

# 输出示例：
# State  Recv-Q Send-Q Local Address:Port  Peer Address:Port Process
# LISTEN 0      128      127.0.0.1:22          0.0.0.0:*    users:(("sshd",pid=979,fd=3))
# LISTEN 0      511        0.0.0.0:80          0.0.0.0:*    users:(("nginx",pid=460421,fd=6))

# 只看 IPv4 的监听
ss -t4lnp

# 只看 IPv6 的监听
ss -t6lnp
```

### 场景 2：查看已建立的连接

```bash
# 所有已建立的 TCP 连接
ss -tn state established

# 已建立的连接 + 进程信息
ss -tnp state established

# 已建立的连接 + TCP 参数
ss -tni state established
```

### 场景 3：全局统计摘要

```bash
# ★ 性能排查第一步
ss -s

# 输出解读：
# Total: 269                          ← 所有 socket 总数
# TCP:   32 (estab 15, closed 2,      ← TCP 统计
#         orphaned 0, timewait 2)
#
# Transport Total     IP        IPv6  ← 按协议分类
# RAW       1         0         1
# UDP       7         5         2
# TCP       30        28        2
# INET      38        33        5
# FRAG      0         0         0
```

### 场景 4：TCP 内存分析

```bash
# 查看所有连接的内存使用
ss -tm

# 只看已建立连接的内存
ss -tm state established

# 输出中的 skmem 字段详解见 01_ss_internals.md
```

### 场景 5：定时器和 keepalive

```bash
# 查看所有连接的定时器
ss -to

# 关键看：
# timer:(keepalive,52sec,0)  → keepalive 定时器正常
# timer:(on,228ms,0)         → 重传定时器活跃（有未 ACK 数据）
# timer:(keepalive,,0)       → keepalive 刚触发
```

### 场景 6：TCP 详细信息（★ 性能分析核心）

```bash
# 查看所有连接的 TCP 参数
ss -ti

# 只看特定连接的参数
ss -ti 'dport = :443'
ss -ti 'sport = :22'

# 重点看：
# cwnd         拥塞窗口
# rtt          往返时延
# send         理论发送速率
# delivery_rate 实际交付速率
# retrans      重传次数
# app_limited  应用层限速
```

### 场景 7：扩展信息

```bash
# 查看 uid、inode、cgroup
ss -te

# 输出包含：
# ino:4479331                          ← inode 号
# sk:1b                                ← socket 内核地址
# cgroup:/user.slice/user-0.slice/...  ← ★ cgroup 路径
```

### 场景 8：TIME_WAIT 排查

```bash
# 查看 TIME_WAIT 连接
ss -tn state time-wait

# 统计 TIME_WAIT 数量
ss -s | grep timewait

# 按目标 IP 统计 TIME_WAIT
ss state time-wait | awk 'NR>1 {print $5}' | cut -d: -f1 | sort | uniq -c | sort -rn

# 持续监控 TIME_WAIT
watch -n 2 'ss -s | grep timewait'
```

### 场景 9：CLOSE_WAIT 泄漏检测

```bash
# 查看 CLOSE_WAIT 连接
ss -tn state close-wait

# CLOSE_WAIT + 进程信息（定位泄漏进程）
ss -tnp state close-wait

# 按进程统计 CLOSE_WAIT
ss -tnp state close-wait | awk '{print $NF}' | sort | uniq -c | sort -rn
```

### 场景 10：端口占用排查

```bash
# 查看谁在监听某个端口
ss -tlnp 'sport = :8080'

# 查看某个端口的所有连接（含已建立）
ss -tan 'sport = :8080'

# 查看出站 HTTPS 连接
ss -tn 'dport = :443'

# 查看所有本地回环连接
ss -tn 'dst 127.0.0.0/8'
```

### 场景 11：SSH 连接监控

```bash
# 查看所有 SSH 连接
ss -tnp 'sport = :22'

# SSH 连接 + TCP 参数
ss -tnpi 'sport = :22'

# 重点看：
# Send-Q 积压 → 网络拥塞或客户端慢
# cwnd 很小 → 可能丢包
# retrans 大 → 网络质量差
```

### 场景 12：UDP 排查

```bash
# 所有 UDP 监听
ss -ulnp

# UDP 已连接（较少见）
ss -unp

# RAW socket
ss -wlnp
```

### 场景 13：Unix domain socket

```bash
# 所有 Unix socket 监听
ss -xlnp

# 所有 Unix socket
ss -xap
```

### 场景 14：持续监控

```bash
# 每 2 秒刷新全局统计
watch -n 2 ss -s

# 每 1 秒刷新已建立连接
watch -n 1 ss -tn state established

# 监控特定端口的连接数变化
watch -n 1 'ss -tn "dport = :443" | wc -l'
```

---

## 7. 输出格式控制

### 格式化参数

| 参数 | 说明 |
|------|------|
| `-n` / `--numeric` | 不解析端口名和主机名（推荐在脚本中使用） |
| `-r` / `--resolve` | 解析 IP 为主机名（DNS 查询，可能很慢） |
| `-N` / `--net` | 显示指定网络命名空间的 socket |
| `-H` / `--no-header` | 不显示表头（适合脚本处理） |
| `-K` / `--kill` | 尝试关闭匹配的 socket（需要 root） |

### 输出解析技巧

```bash
# 统计各状态连接数
ss -tan | awk 'NR>1 {print $1}' | sort | uniq -c | sort -rn

# 统计各监听端口的 backlog 使用率
ss -tln | awk 'NR>1 {printf "%-20s Recv-Q=%-5s Send-Q=%-5s\n", $4, $2, $3}'

# 找出 Send-Q 积压的连接
ss -tn | awk 'NR>1 && $3>0 {print}'

# 找出 Recv-Q 积压的连接
ss -tn | awk 'NR>1 && $2>0 {print}'

# 统计每个目标 IP 的连接数
ss -tn state established | awk 'NR>1 {split($5,a,":"); print a[1]}' | sort | uniq -c | sort -rn

# 提取 ss -i 中的 RTT
ss -ti state established | grep -oP 'rtt:\K[0-9.]+'

# 提取 ss -i 中的 cwnd
ss -ti state established | grep -oP 'cwnd:\K[0-9]+'
```

### ★ 与 awk/grep 配合的最佳实践

```bash
# 1. 用 -H 去除表头
ss -tnH state established | wc -l

# 2. 用 -n 避免 DNS 解析延迟
ss -tn   # 默认就是 -n（数字格式）
ss -trn  # 如果要解析主机名，加 -r

# 3. 输出到 JSON（ss 不原生支持，需 awk 转换）
ss -tnH state established | awk '{
  printf "{\"state\":\"%s\",\"local\":\"%s\",\"peer\":\"%s\"}\n", $1, $4, $5
}'

# 4. 定时采样（性能分析用）
for i in $(seq 1 10); do
  echo "=== Sample $i ==="
  ss -s
  sleep 5
done
```

---

## 总结：ss 命令速查卡

```
┌────────────────────────────────────────────────────────────────┐
│                    ss 命令速查卡                                │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│  协议：  -t TCP   -u UDP   -x Unix   -w RAW   -4 IPv4  -6 IPv6│
│  状态：  -l 监听   -a 所有                                      │
│  信息：  -p 进程  -o 定时器  -e 扩展  -m 内存  -i TCP详情      │
│  统计：  -s 摘要                                                │
│  格式：  -n 数字  -r 解析  -H 无表头                            │
│                                                                │
│  过滤器：state <tcp-state>                                     │
│          sport/dport <op> <port>                               │
│          src/dst <address>[/prefix]                            │
│          not / and / or                                        │
│                                                                │
│  ★ 最常用组合：                                                │
│    ss -tlnp           TCP 监听 + 进程                          │
│    ss -tnp            TCP 已建立 + 进程                        │
│    ss -s              全局统计摘要                              │
│    ss -ti             TCP 详细参数（性能分析）                  │
│    ss state time-wait TIME_WAIT 排查                           │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```
