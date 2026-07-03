# netstat + ss 完整命令参考

> netstat 属于 net-tools（已停止维护），ss 属于 iproute2（活跃维护）。
> 本文覆盖两者的全部常用参数、ss filter 语法，以及 10+ 个实战场景。

---

## 一、netstat 命令参考

### 基本用法

```bash
netstat [选项]
```

### 参数速查表

| 参数 | 全称 | 含义 | 备注 |
|------|------|------|------|
| `-t` | `--tcp` | 只显示 TCP 连接 | 最常用 |
| `-u` | `--udp` | 只显示 UDP 连接 | — |
| `-l` | `--listening` | 只显示监听状态的 socket | 配合 `-t`/`-u` |
| `-n` | `--numeric` | 显示数字地址和端口（不做 DNS 反查） | ★ 必加，否则很慢 |
| `-p` | `--program` | 显示 PID 和进程名 | 需要 root 权限 |
| `-a` | `--all` | 显示所有状态的 socket（含 LISTEN） | 不加则只显示 ESTAB |
| `-s` | `--statistics` | 显示协议统计信息 | ★ 诊断利器 |
| `-c` | `--continuous` | 持续输出（每秒刷新） | 类似 top |
| `-r` | `--route` | 显示路由表 | 等同于 `route -n` |
| `-i` | `--interfaces` | 显示网络接口统计 | 等同于 `ifconfig` |
| `-M` | `--masquerade` | 显示伪装连接 | 很少用 |
| `-W` | `--wide` | 不截断长地址 | — |

### 参数组合详解

```bash
# ★ 最常用组合：查看 TCP 监听端口 + 进程
netstat -tlnp

# 查看所有 TCP 连接（含 LISTEN + ESTAB + TIME_WAIT 等）
netstat -tna

# 查看所有 TCP 已建立连接
netstat -tn

# 查看 UDP 监听端口
netstat -ulnp

# 查看所有协议统计（诊断利器）
netstat -s

# 持续监控 TCP 连接（每秒刷新）
netstat -tcn

# 同时显示 TCP + UDP
netstat -tulnp

# 查看路由表
netstat -rn

# 查看网络接口流量
netstat -i
```

### netstat 输出格式

#### `-tlnp` 输出

```
Proto Recv-Q Send-Q Local Address           Foreign Address         State       PID/Program name
tcp        0      0 0.0.0.0:22              0.0.0.0:*               LISTEN      979/sshd: /usr/sbin
tcp6       0      0 :::22                   :::*                    LISTEN      979/sshd: /usr/sbin
 ↑          ↑      ↑       ↑                    ↑                      ↑           ↑
协议    接收队列  发送队列  本地地址:端口       远端地址              状态        PID/程序名
tcp=IPv4  (LISTEN 时:   (LISTEN 时:
tcp6=IPv6  backlog长度)  backlog上限)
```

#### `-tn` 输出

```
Proto Recv-Q Send-Q Local Address           Foreign Address         State
tcp        0      0 172.27.141.144:22       183.247.2.29:13462      ESTABLISHED
 ↑          ↑      ↑       ↑                    ↑                      ↑
协议    接收队列  发送队列  本地IP:端口          远端IP:端口             状态
        (已收未读)  (已发未确认)
```

---

## 二、ss 命令参考

### 基本用法

```bash
ss [选项] [filter]
```

### 参数速查表

| 参数 | 全称 | 含义 | 备注 |
|------|------|------|------|
| `-t` | `--tcp` | 只显示 TCP 连接 | ★ 最常用 |
| `-u` | `--udp` | 只显示 UDP 连接 | — |
| `-l` | `--listening` | 只显示监听状态的 socket | 配合 `-t`/`-u` |
| `-n` | `--numeric` | 显示数字地址（不做 DNS 反查） | ★ 必加 |
| `-p` | `--processes` | 显示进程信息 | 需要 root |
| `-s` | `--summary` | 显示 socket 统计摘要 | ★ 快速概览 |
| `-a` | `--all` | 显示所有状态 | 不加则 ESTAB+LISTEN |
| `-o` | `--options` | 显示 timer 信息 | ★ 诊断 keepalive/重传 |
| `-e` | `--extended` | 显示详细信息（uid、inode、cgroup） | 排查权限问题 |
| `-m` | `--memory` | 显示 socket 内存使用 | ★ 内存分析 |
| `-4` | `--ipv4` | 只显示 IPv4 | — |
| `-6` | `--ipv6` | 只显示 IPv6 | — |
| `-i` | `--info` | 显示内部 TCP 信息（cwnd、rtt 等） | ★ 性能分析 |
| `-K` | `--kill` | 关闭匹配的 socket | 危险操作 |
| `-H` | `--no-header` | 不显示表头 | 方便 awk 处理 |
| `-N` | `--net` | 指定网络命名空间 | 容器场景 |

### 参数组合详解

```bash
# ★ 最常用组合：TCP 监听 + 进程
ss -tlnp

# 所有 TCP 连接（含所有状态）
ss -tna

# 已建立的 TCP 连接 + 进程
ss -tnp

# socket 统计摘要
ss -s

# TCP 内存使用
ss -tm

# timer 信息（keepalive / 重传计时器）
ss -to

# 扩展信息（inode、cgroup）
ss -te

# TCP 内部信息（cwnd、rtt、ssthresh）
ss -ti

# 所有协议统计
ss -a

# 只看 IPv4
ss -4 -tlnp

# 只看 IPv6
ss -6 -tlnp
```

---

## 三、ss filter 语法 ★

ss 独有的 filter 功能可在**内核侧过滤**，比 `grep` 快得多（尤其是连接数很多时）。

### filter 语法结构

```
ss [选项] <filter_expression>
```

filter 支持以下子句，用空格连接（AND 逻辑）：

```
state <state-list>          # 按 TCP 状态过滤
<field> <operator> <value>  # 按字段过滤
not <expression>            # 取反
( <expression> )            # 分组
```

### state filter

```bash
# 按 TCP 状态过滤（使用连字符形式）
ss -tn state established          # ESTABLISHED
ss -tn state time-wait            # TIME_WAIT
ss -tn state close-wait           # CLOSE_WAIT
ss -tn state syn-sent             # SYN_SENT
ss -tn state syn-recv             # SYN_RECV
ss -tn state fin-wait-1           # FIN_WAIT_1
ss -tn state fin-wait-2           # FIN_WAIT_2
ss -tn state last-ack             # LAST_ACK
ss -tn state closing              # CLOSING
ss -tn state listen               # LISTEN（等价于 -l）
ss -tn state all                  # 所有状态（等价于 -a）

# 多状态过滤
ss -tn state time-wait or state close-wait
```

### 端口过滤

```bash
# 本地端口等于 80
ss -tlnp sport = :80

# 远端端口等于 443
ss -tn dport = :443

# 本地端口大于 1024
ss -tn sport > :1024

# 本地端口在范围内
ss -tn sport >= :8000 and sport <= :9000
```

### IP 地址过滤

```bash
# 本地地址等于 127.0.0.1
ss -tn src 127.0.0.1

# 远端地址等于某个 IP
ss -tn dst 39.106.104.16

# 本地地址在某个网段
ss -tn src 172.27.0.0/16

# 远端地址匹配
ss -tn dst 100.100.0.0/16
```

### 组合过滤

```bash
# 本地端口 80 且状态为 ESTABLISHED
ss -tnp sport = :80 and state established

# 远端端口 443 的所有连接
ss -tn dport = :443

# 本地端口 22 且不是 ESTABLISHED
ss -tna sport = :22 and not state established

# 指定网段 + 指定端口
ss -tn src 172.27.0.0/16 and dport = :443
```

### filter 中的运算符

| 运算符 | 含义 | 示例 |
|--------|------|------|
| `=` 或 `==` | 等于 | `sport = :80` |
| `!=` | 不等于 | `state != established` |
| `<` | 小于 | `sport < :1024` |
| `>` | 大于 | `sport > :1024` |
| `<=` | 小于等于 | `sport <= :8080` |
| `>=` | 大于等于 | `sport >= :80` |
| `and` | 逻辑与 | `sport = :80 and state established` |
| `or` | 逻辑或 | `state time-wait or state close-wait` |
| `not` | 逻辑非 | `not state established` |

### filter 中的字段名

| 字段 | 含义 | 别名 |
|------|------|------|
| `sport` | 本地端口（source port） | — |
| `dport` | 远端端口（destination port） | — |
| `src` | 本地地址（source address） | — |
| `dst` | 远端地址（destination address） | — |
| `state` | TCP 状态 | — |
| `dev` | 网络设备 | — |

---

## 四、ss 输出格式详解

### `ss -tlnp` 输出

```
State  Recv-Q Send-Q  Local Address:Port   Peer Address:Port  Process
LISTEN 0      128           0.0.0.0:22           0.0.0.0:*    users:(("sshd",pid=979,fd=3))
  ↑      ↑      ↑               ↑                  ↑                       ↑
状态   接收    发送          本地地址:端口       远端地址             进程信息
       队列    队列          (监听端口)          (* = 任意)           (进程名,PID,fd号)
```

### `ss -tn` 输出（ESTABLISHED）

```
State  Recv-Q Send-Q  Local Address:Port   Peer Address:Port  Process
ESTAB  0      0       172.27.141.144:22    183.247.2.29:13462
  ↑      ↑      ↑             ↑                    ↑
状态   接收    发送          本地IP:端口          远端IP:端口
       队列    队列
       (已收    (已发
       未读)   未确认)
```

### `ss -to` 输出（含 timer 信息）

```
ESTAB  0   0   172.27.141.144:42464  185.199.109.133:https  timer:(on,2.972ms,7)
                                                            ↑
                                                    timer 信息：
                                                    (类型,剩余时间,已触发次数)

timer 类型：
  on           = 重传定时器（有未确认的数据）
  keepalive    = keepalive 定时器
  timewait     = TIME_WAIT 定时器
  persist      = 持续定时器（探测窗口）
  userspace    = 用户态设置的定时器
```

### `ss -tm` 输出（含内存信息）

```
ESTAB  0   0   172.27.141.144:42464  185.199.109.133:https
       skmem:(r0,rb131072,t0,tb87040,f2873,w1223,o0,bl0,d0)
              ↑   ↑       ↑  ↑      ↑     ↑     ↑  ↑   ↑
              │   │       │  │      │     │     │  │   └─ d: 丢弃数据包数
              │   │       │  │      │     │     │  └───── bl: backlog 长度
              │   │       │  │      │     │     └──────── o: 选项内存
              │   │       │  │      │     └────────────── w: 已分配发送内存(字节)
              │   │       │  │      └──────────────────── f: 预分配转发内存(字节)
              │   │       │  └─────────────────────────── tb: 发送 buffer 总量(字节)
              │   │       └────────────────────────────── t: 已分配接收内存(字节)
              │   └────────────────────────────────────── rb: 接收 buffer 总量(字节)
              └────────────────────────────────────────── r: 接收内存(字节)
```

### `ss -te` 输出（扩展信息）

```
ESTAB  0   0   172.27.141.144:42464  185.199.109.133:https
       ino:4486214 sk:1c cgroup:/user.slice/user-0.slice/session-29.scope <->
       ↑           ↑     ↑
       │           │     └─ cgroup: 进程所属 cgroup
       │           └─────── sk: socket 内核指针（唯一标识）
       └────────────────── ino: inode 号（/proc/<pid>/fd 中可见）
```

### `ss -ti` 输出（TCP 内部信息）

```
ESTAB  0   0   172.27.141.144:42464  185.199.109.133:https
       cubic wscale:7,7 rto:201 rtt:1.234/0.617 mss:1448 pmtu:1500
       rcvmss:536 advmss:1448 cwnd:10 ssthresh:7 bytes_sent:12345
       bytes_received:67890 segs_out:100 segs_in:50 send 93.1Mbps
       lastsnd:234 lastrcv:123 lastack:456 pacing_rate 186.2Mbps
       retrans:0/100 reord:0 delivered:50
       ↑        ↑     ↑
       │        │     └─ 拥塞窗口、慢启动阈值、发送/接收字节数等
       │        └─────── 拥塞算法、窗口缩放因子、RTO、RTT
       └──────────────── TCP 内部调试信息（需要 -i 参数）
```

---

## 五、10+ 个实战场景

### 场景 1：查看所有 TCP 监听端口

```bash
# ss 方式（推荐）
ss -tlnp

# netstat 方式（对照）
netstat -tlnp
```

### 场景 2：查看所有已建立连接

```bash
ss -tnp
```

### 场景 3：统计各 TCP 状态分布

```bash
ss -tna | awk 'NR>1{print $1}' | sort | uniq -c | sort -rn

# 输出示例：
#   21 ESTAB
#   15 LISTEN
#    5 TIME-WAIT
```

### 场景 4：查看 TIME_WAIT 连接

```bash
ss -tn state time-wait

# 统计数量
ss -tn state time-wait | wc -l
```

### 场景 5：查看 CLOSE_WAIT 连接（诊断应用 Bug）

```bash
ss -tn state close-wait

# 按进程统计
ss -tnp state close-wait | awk -F'"' '{print $2}' | sort | uniq -c | sort -rn
```

### 场景 6：查看 socket 统计摘要

```bash
ss -s
```

### 场景 7：查看 TCP 内存使用

```bash
ss -tm
```

### 场景 8：查看 keepalive / 重传 timer

```bash
ss -to
```

### 场景 9：查找占用特定端口的进程

```bash
# 查找谁在监听 80 端口
ss -tlnp 'sport = :80'

# 查找谁在监听 22 端口
ss -tlnp 'sport = :22'

# netstat 方式
netstat -tlnp | grep ':80 '
```

### 场景 10：查看远端 IP 连接数排行

```bash
ss -tn | awk 'NR>1{print $5}' | cut -d: -f1 | sort | uniq -c | sort -rn | head -10
```

### 场景 11：查看本地端口连接数排行

```bash
ss -tn | awk 'NR>1{print $4}' | cut -d: -f2 | sort | uniq -c | sort -rn | head -10
```

### 场景 12：按进程统计连接数

```bash
ss -tnp | awk -F'"' '{print $2}' | sort | uniq -c | sort -rn | head -10
```

### 场景 13：查看特定网段的连接

```bash
# 查看连到 100.100.0.0/16 网段的所有连接
ss -tn dst 100.100.0.0/16
```

### 场景 14：查看 TCP 内部信息（RTT、cwnd）

```bash
ss -ti

# 只看 ESTABLISHED 的 TCP 信息
ss -ti state established
```

### 场景 15：查看协议统计（netstat -s）

```bash
# netstat 的 -s 选项提供详细的协议统计（ss -s 只看 socket 摘要）
netstat -s

# 只看 TCP 部分
netstat -s | sed -n '/^Tcp:/,/^[A-Z]/p'
```

### 场景 16：持续监控连接变化

```bash
# netstat 方式（-c = continuous，每秒刷新）
netstat -tcn

# ss 没有 -c 选项，用 watch 替代
watch -n 1 'ss -tna | awk "NR>1{print \$1}" | sort | uniq -c | sort -rn'
```

### 场景 17：查看 SYN_RECV 连接（诊断 SYN flood）

```bash
ss -tn state syn-recv

# 统计数量（> 100 需要警惕）
ss -tn state syn-recv | wc -l
```

### 场景 18：查看 UDP 监听端口

```bash
ss -ulnp
netstat -ulnp
```

---

## 六、ss 替代 netstat 命令对照表

| netstat 命令 | 等效 ss 命令 | 说明 |
|-------------|-------------|------|
| `netstat -tlnp` | `ss -tlnp` | TCP 监听端口 |
| `netstat -tn` | `ss -tn` | TCP 已建立连接 |
| `netstat -tna` | `ss -tna` | 所有 TCP 连接 |
| `netstat -ulnp` | `ss -ulnp` | UDP 监听端口 |
| `netstat -tulnp` | `ss -tulnp` | TCP+UDP 监听 |
| `netstat -s` | `ss -s`（部分） | 协议统计 |
| `netstat -rn` | `ip route` | 路由表（属于 iproute2） |
| `netstat -i` | `ip -s link` | 接口统计（属于 iproute2） |
| `netstat -an \| grep :80` | `ss -tlnp 'sport = :80'` | 查找端口（ss filter 更高效） |
| `netstat -an \| grep ESTABLISHED` | `ss -tn state established` | 按状态过滤 |
| `netstat -an \| grep TIME_WAIT` | `ss -tn state time-wait` | TIME_WAIT |
| `netstat -tcn` | `watch -n1 ss -tn` | 持续监控 |
| `netstat -p \| grep <pid>` | `ss -tnp \| grep <pid>` | 按进程查 |

> ★ **注意**：`netstat -s` 的内容比 `ss -s` 丰富得多。
> `ss -s` 只显示 socket 摘要（总数、各协议计数），
> `netstat -s` 包含详细的 IP/TCP/UDP/ICMP 统计。
> 两者不可直接替代。

---

## 七、内核参数速查

| 参数路径 | 默认值 | 含义 |
|---------|--------|------|
| `/proc/sys/net/ipv4/tcp_fin_timeout` | 60 | FIN_WAIT_2 / TIME_WAIT 超时时间（秒） |
| `/proc/sys/net/ipv4/tcp_keepalive_time` | 7200 | keepalive 空闲时间（秒） |
| `/proc/sys/net/ipv4/tcp_keepalive_intvl` | 75 | keepalive 探测间隔（秒） |
| `/proc/sys/net/ipv4/tcp_keepalive_probes` | 9 | keepalive 探测次数 |
| `/proc/sys/net/ipv4/tcp_max_syn_backlog` | 128~1024 | SYN_RECV 队列上限 |
| `/proc/sys/net/ipv4/tcp_syncookies` | 1 | SYN Cookie 开关 |
| `/proc/sys/net/ipv4/tcp_max_tw_buckets` | 262144 | TIME_WAIT 最大数量 |
| `/proc/sys/net/ipv4/tcp_tw_reuse` | 2 | TIME_WAIT 复用（2=安全复用） |
| `/proc/sys/net/ipv4/tcp_syn_retries` | 6 | SYN 重传次数 |
| `/proc/sys/net/ipv4/tcp_mem` | 三值 | TCP 内存限制（低/压力/高）页 |
| `/proc/sys/net/ipv4/tcp_rmem` | 三值 | TCP 接收 buffer 限制（最小/默认/最大） |
| `/proc/sys/net/ipv4/tcp_wmem` | 三值 | TCP 发送 buffer 限制（最小/默认/最大） |
| `/proc/sys/net/core/somaxconn` | 128~4096 | listen() backlog 上限 |

---

## 八、数据来源对比

```
┌─────────────────────────────────────────────────────────────────────┐
│ netstat 数据来源                                                    │
│                                                                     │
│ /proc/net/tcp          → 所有 TCP socket（十六进制文本）            │
│ /proc/net/tcp6         → 所有 IPv6 TCP socket                       │
│ /proc/net/udp          → 所有 UDP socket                            │
│ /proc/net/udp6         → 所有 IPv6 UDP socket                       │
│ /proc/net/netstat      → TCP/IP 扩展统计                            │
│ /proc/net/snmp         → 协议级统计（TCP、UDP、ICMP 等）            │
│ /proc/net/sockstat     → socket 全局统计                            │
│                                                                     │
│ ★ 限制：只能读文本，无法过滤，连接多时很慢                          │
├─────────────────────────────────────────────────────────────────────┤
│ ss 数据来源                                                         │
│                                                                     │
│ netlink socket (NETLINK_INET_DIAG / NETLINK_SOCK_DIAG)              │
│   → 内核直接返回 struct inet_diag_msg（二进制）                     │
│   → 支持内核侧过滤（state、地址、端口）                             │
│                                                                     │
│ /proc/net/sockstat     → socket 全局统计（-s 使用）                 │
│ /proc/net/snmp         → 协议级统计（部分）                         │
│                                                                     │
│ ★ 优势：二进制协议 + 内核过滤，连接多时极快                         │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 九、性能对比

```
场景：10 万 TCP 连接

netstat -tn：
  - 读取 /proc/net/tcp（每次 read() 一行）
  - 10 万行 × ~100 字节 = ~10MB 文本
  - 用户态 sscanf() 逐行解析
  - 耗时：2-5 秒

ss -tn：
  - 通过 netlink 批量接收（每次 recvmsg() 数十条）
  - 直接解析 struct inet_diag_msg
  - 耗时：0.1-0.5 秒

ss -tn state established：
  - 内核侧过滤（只返回 ESTABLISHED 状态）
  - 如果只有 5 万 ESTABLISHED，只传输一半数据
  - 耗时：0.05-0.3 秒

★ 结论：ss 在所有场景下都优于 netstat，尤其是连接数多时
```

---

## 十、常见错误与排查

| 错误/现象 | 原因 | 解决方法 |
|----------|------|---------|
| `ss -p` 没有进程信息 | 非 root 运行 | `sudo ss -tlnp` |
| netstat 输出很慢 | DNS 反查（没加 `-n`） | 加 `-n` 参数 |
| `cannot find process` | 进程已退出 | 连接可能是孤儿连接 |
| TIME_WAIT 一直不消失 | 正常（60 秒才消失） | 等待或调参 |
| CLOSE_WAIT 持续增长 | 应用 Bug | 检查代码 close() 调用 |
| `ss filter` 语法错误 | 状态名写错 | 使用连字符形式：`fin-wait-1` |
| 端口号显示为服务名 | 没加 `-n` | 加 `-n` 参数 |
