# nginx 性能调优与高 CPU 排查专题

> 面向 AMD 服务器（EPYC/Zen 架构）的 nginx 性能调优实战
> 重点关注：CPU 高占用低吞吐、CCD 拓扑绑核、系统调用开销
> 预计阅读时间：20 分钟

---

## 阅读要点

1. ★ worker_processes auto 与 CPU 核数的关系
2. ★ worker_cpu_affinity 绑定策略（AMD CCD 拓扑）
3. ★ multi_accept / epoll / sendfile 的原理与调优
4. ★ nginx 高 CPU 排查流程（strace + perf）

---

## 一、worker_processes auto 与 CPU 核数

### 1.1 auto 的含义

```c
/*
 * 源码位置：src/os/unix/ngx_cpuinfo.c — ngx_ncpu
 *          src/core/ngx_cycle.c — ngx_init_cycle()
 *
 * worker_processes auto 的处理逻辑：
 *   1. 通过 sysconf(_SC_NPROCESSORS_ONLN) 获取在线 CPU 核数
 *   2. 设置 worker 数量 = CPU 核数
 */

// 伪代码
if (ccf->worker_processes == NGX_CONF_UNSET) {
    ccf->worker_processes = ngx_ncpu;  // ★ auto = CPU 核数
}
```

**关键理解：**

| 场景 | worker_processes | 实际 worker 数 | 说明 |
|------|-----------------|---------------|------|
| 4 核 CPU | `auto` | 4 | ★ 最常用，1 核 1 worker |
| 64 核 EPYC | `auto` | 64 | ★ 可能过多，见 CCD 优化 |
| 64 核 EPYC | `16` | 16 | ★ 手动 = CCD 数量 |
| 超线程 8C/16T | `auto` | 16 | ⚠️ 含超线程，可能不是最优 |

### 1.2 超线程的影响

```
物理核 vs 逻辑核：

  物理 4 核（无超线程）：
  ┌────┐ ┌────┐ ┌────┐ ┌────┐
  │ C0 │ │ C1 │ │ C2 │ │ C3 │  ← worker_processes auto = 4 ✓
  └────┘ └────┘ └────┘ └────┘

  物理 4 核（有超线程 = 8 逻辑核）：
  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐
  │ C0/C0' │ │ C1/C1' │ │ C2/C2' │ │ C3/C3' │
  └────────┘ └────────┘ └────────┘ └────────┘
  worker_processes auto = 8  ← ★ 含超线程，可能不是最优
  
  ★ nginx worker 是 CPU 密集型（事件循环+请求处理）
    两个 worker 共享同一物理核的 L1/L2 缓存 → cache thrashing
    建议：worker_processes = 物理核数（非逻辑核数）
```

---

## 二、worker_cpu_affinity 绑定 CPU 核

### 2.1 为什么需要绑核

```
不绑核的问题：

  Worker 0 被调度器在 CPU 核之间迁移：
  
  时间 T1: Worker 0 在 CPU 0 上运行
           CPU 0 的 L1/L2 缓存被 Worker 0 的请求数据填满
  
  时间 T2: 调度器将 Worker 0 迁移到 CPU 3
           CPU 3 的 L1/L2 缓存是冷的（没有 Worker 0 的数据）
           CPU 0 的 L1/L2 缓存被其他进程污染
  
  时间 T3: Worker 0 又迁移回 CPU 0
           L1/L2 缓存已经被污染 → ★ cache miss 率飙升

  ★ 每次迁移 = L1/L2 缓存失效 = 延迟增加 10-30%
```

### 2.2 绑核语法

```nginx
# 方式 1: 位掩码（传统方式）
worker_processes 4;
worker_cpu_affinity 0001 0010 0100 1000;
# worker 0 → CPU 0
# worker 1 → CPU 1
# worker 2 → CPU 2
# worker 3 → CPU 3

# 方式 2: auto（nginx ≥ 1.25.0）
worker_processes auto;
worker_cpu_affinity auto;
# ★ 自动将每个 worker 绑定到一个 CPU 核

# 方式 3: 指定模式（灵活绑核）
worker_processes 4;
worker_cpu_affinity 00000001 00000010 00000100 00001000;
# 绑到 CPU 0, 1, 2, 3（跳过超线程核 4-7）
```

**底层实现：**

```c
/*
 * 源码位置：src/os/unix/ngx_process_cycle.c — ngx_setaffinity()
 *
 * 绑核通过 sched_setaffinity() 系统调用实现
 */

// 伪代码
cpu_set_t mask;
CPU_ZERO(&mask);
// 根据配置位掩码设置 CPU
for (i = 0; i < CPU_COUNT; i++) {
    if (cpu_affinity & (1 << i))
        CPU_SET(i, &mask);
}
sched_setaffinity(0, sizeof(mask), &mask);
// ★ 此后该 worker 进程只能在这些 CPU 核上运行
```

---

## 三、AMD CCD 拓扑下的 nginx worker 绑定策略

### 3.1 AMD EPYC/Zen CCD 拓扑

```
★ CCD（Core Complex Die）= AMD 的核心组，共享 L3 缓存

AMD EPYC 7763（64 核，8 个 CCD）：

  ┌─────────────────────────────────────────────────────────────┐
  │                    EPYC 7763 (64C/128T)                     │
  │                                                             │
  │  ┌─────────────┐  ┌─────────────┐  ...  ┌─────────────┐   │
  │  │   CCD 0     │  │   CCD 1     │       │   CCD 7     │   │
  │  │ ┌──┐┌──┐    │  │ ┌──┐┌──┐    │       │ ┌──┐┌──┐    │   │
  │  │ │C0││C1│... │  │ │C8││C9│... │       │ │C56│C57│   │   │
  │  │ └──┘└──┘    │  │ └──┘└──┘    │       │ └──┘└──┘    │   │
  │  │ 共享 32MB   │  │ 共享 32MB   │       │ 共享 32MB   │   │
  │  │ L3 缓存     │  │ L3 缓存     │       │ L3 缓存     │   │
  │  └─────────────┘  └─────────────┘       └─────────────┘   │
  │         │                │                      │           │
  │         └────────┬───────┘                      │           │
  │           Infinity Fabric (跨 CCD 互联)          │           │
  │                  └──────────────────────────────┘           │
  └─────────────────────────────────────────────────────────────┘

  ★ 同 CCD 内核间通信延迟：~10-15 ns（共享 L3）
  ★ 跨 CCD 通信延迟：~40-80 ns（走 Infinity Fabric）
  ★ 跨 NUMA node 延迟：~100-200 ns
```

### 3.2 查看 CCD 拓扑

```bash
# 方法 1: lscpu 查看 NUMA 拓扑
lscpu | grep -i numa
# NUMA node0 CPU(s): 0-15,64-79       ← 逻辑核 64-79 是 0-15 的超线程
# NUMA node1 CPU(s): 16-31,80-95
# ...

# 方法 2: lstopo 查看完整拓扑（需安装 hwloc）
lstopo --no-icaches

# 方法 3: 查看 L3 缓存共享关系
cat /sys/devices/system/cpu/cpu*/cache/index3/shared_cpu_list
# cpu0:  0-7        ← CPU 0-7 共享同一个 L3（CCD 0）
# cpu8:  8-15       ← CPU 8-15 共享同一个 L3（CCD 1）
# cpu16: 16-23      ← CCD 2
# ...
```

### 3.3 ★ 最佳策略：每个 CCD 一个 worker

```
策略对比：

方案 A: worker_processes auto（64 个 worker）
────────────────────────────────────────────
  每个逻辑核一个 worker
  问题：
    - 64 个 worker → master 管理开销大
    - 超线程核共享 L1/L2 → cache thrashing
    - worker 间竞争 accept（惊群）
    - ★ 64 个 worker 的 epoll_wait 竞争同一个 listen socket

方案 B: worker_processes = CCD 数量，绑核到每个 CCD 的第一个物理核
────────────────────────────────────────────
  8 个 CCD → 8 个 worker
  每个 worker 绑定到一个 CCD 的物理核上

  worker_cpu_affinity 00000001 000000000100000000 \
                      000000000000000100000000 ...
  # 简化写法：绑到每个 CCD 的第一个物理核

  优点：
    ★ worker 数量少 → accept 竞争少
    ★ 同一 CCD 的 L3 缓存被同一个 worker 独占
    ★ 无跨 CCD 调度延迟
    ★ worker 可自由在该 CCD 的物理核间迁移（L3 共享）

方案 C: 每个 CCD 一个 worker，但绑核到该 CCD 的所有物理核
────────────────────────────────────────────
  worker 0 → CPU 0-7（CCD 0 的所有物理核）
  worker 1 → CPU 8-15（CCD 1 的所有物理核）
  ...

  worker_cpu_affinity 0000000011111111 1111111100000000 ...

  优点：worker 可以在 CCD 内部迁移，利用 CCD 内的空闲核
  ★ 推荐用于 CCD 核数 ≥ 8 的 EPYC 处理器
```

### 3.4 实际配置示例

```nginx
# AMD EPYC 7763 (8 CCD, 每 CCD 8 物理核, 共 64C/128T)
# ★ 推荐配置：8 个 worker，每个绑到一个 CCD

worker_processes 8;    # CCD 数量

# 绑核：每个 worker 绑定到一个 CCD 的 8 个物理核
# CCD 0: CPU 0-7, CCD 1: CPU 8-15, ...
worker_cpu_affinity 0000000000000000000000000000000000000000000000000000000011111111 \
                    0000000000000000000000000000000000000000000000001111111100000000 \
                    0000000000000000000000000000000000000000111111110000000000000000 \
                    0000000000000000000000000000000011111111000000000000000000000000 \
                    0000000000000000000000001111111100000000000000000000000000000000 \
                    0000000000000000111111110000000000000000000000000000000000000000 \
                    0000000011111111000000000000000000000000000000000000000000000000 \
                    1111111100000000000000000000000000000000000000000000000000000000;
# ★ 每个 worker 可以在其 CCD 的 8 个物理核之间自由调度
# 共享 L3 缓存 → cache miss 率低

events {
    worker_connections 4096;
    multi_accept on;
    use epoll;
}
```

### 3.5 性能对比（预期数据）

| 配置 | QPS（静态文件） | P99 延迟 | cpu-migrations |
|------|---------------|---------|---------------|
| `auto` (64 worker, 不绑核) | ~80,000 | 5-8ms | 高（~5000/s） |
| `auto` + `auto` 绑核 | ~95,000 | 3-5ms | ≈ 0 |
| 8 worker (CCD 数) + CCD 绑核 | ★ ~110,000 | ★ 2-3ms | ≈ 0 |

---

## 四、multi_accept on vs off

### 4.1 工作原理

```
multi_accept off（默认）：
──────────────────────────
  epoll_wait 返回 N 个事件
  对于 listen socket 的 EPOLLIN：
    accept() 一次  ← ★ 只接受一个连接
    返回事件循环
    下一次 epoll_wait 才能再 accept

  问题：突发大量连接时，每个连接需要一次 epoll_wait 循环

multi_accept on：
──────────────────────────
  epoll_wait 返回 N 个事件
  对于 listen socket 的 EPOLLIN：
    while (accept() 成功) {
        接受连接
        分配到连接池
        加入 epoll
    }
    ★ 循环 accept 直到没有新连接（返回 EAGAIN）

  优点：突发连接处理更快
  缺点：一次性 accept 大量连接可能阻塞其他事件处理
```

**源码逻辑：**

```c
/*
 * 源码位置：src/event/ngx_event_accept.c — ngx_event_accept()
 */

void ngx_event_accept(ngx_event_t *ev)
{
    do {
        s = accept4(lc->fd, &sa, &socklen, SOCK_NONBLOCK);
        // ★ SOCK_NONBLOCK: 非阻塞连接

        if (s == -1) {
            if (err == NGX_EAGAIN)  // 没有更多连接
                return;
            // 其他错误处理...
        }

        // 分配连接、初始化、加入 epoll...

    } while (ev->available);
    // ★ ev->available = multi_accept 设置
    //   off → available = 0 → 循环只执行一次
    //   on  → available = 1 → 循环到 EAGAIN
}
```

### 4.2 何时开启

| 场景 | multi_accept | 原因 |
|------|-------------|------|
| 高并发（>1000 QPS） | ★ on | 快速接受突发连接 |
| 低并发（<100 QPS） | off | 差异不大 |
| 反向代理 | ★ on | 减少连接建立延迟 |
| worker_connections 很大 | ★ on | 充分利用连接池 |

---

## 五、epoll vs kqueue vs select

### 5.1 三种事件模型对比

```
┌────────────┬─────────────┬──────────┬──────────────────────────────────┐
│ 特性        │ epoll        │ kqueue   │ select/poll                      │
│            │ (Linux)      │ (BSD)    │ (通用)                            │
├────────────┼─────────────┼──────────┼──────────────────────────────────┤
│ 时间复杂度  │ ★ O(1)      │ ★ O(1)  │ O(n) — 遍历所有 fd              │
│            │ (只返回就绪)  │          │                                  │
├────────────┼─────────────┼──────────┼──────────────────────────────────┤
│ 最大 fd 数  │ 系统上限     │ 系统上限 │ select: FD_SETSIZE (1024)       │
│            │ (~1048576)   │          │ poll: 无硬限制但效率低            │
├────────────┼─────────────┼──────────┼──────────────────────────────────┤
│ 触发模式    │ ET + LT     │ ET       │ 仅 LT                           │
├────────────┼─────────────┼──────────┼──────────────────────────────────┤
│ 数据传递    │ 共享内核     │ kevent   │ 每次调用拷贝 fd_set              │
│            │ 红黑树       │ 结构体   │ 到内核                           │
├────────────┼─────────────┼──────────┼──────────────────────────────────┤
│ 适用平台    │ ★ Linux     │ FreeBSD  │ 所有平台（fallback）             │
│            │              │ macOS    │                                  │
└────────────┴─────────────┴──────────┴──────────────────────────────────┘

★ 在 Linux 上，epoll 是唯一正确选择
  nginx 自动检测并使用 epoll（除非强制指定 use select）
```

### 5.2 epoll 内部机制

```
epoll 的内部数据结构：

  ┌── epoll 实例（epoll_create 返回的 fd）────────────────────┐
  │                                                          │
  │  红黑树（rb_tree）：存储所有被监控的 fd                    │
  │  ┌───────┐                                               │
  │  │ fd=3  │    添加/删除/修改：O(log n)                   │
  │  │  /  \ │    epoll_ctl(EPOLL_CTL_ADD/DEL/MOD)           │
  │  │ fd=5  fd=8│                                           │
  │  └───────┘                                               │
  │                                                          │
  │  就绪链表（rdllist）：只包含有事件的 fd                   │
  │  ┌──────┐  ┌──────┐  ┌──────┐                           │
  │  │ fd=3 │→ │ fd=8 │→ │ fd=15│→ NULL                     │
  │  └──────┘  └──────┘  └──────┘                           │
  │                                                          │
  │  epoll_wait() 只遍历就绪链表 → ★ O(1)                   │
  │  （不管总共有多少 fd，只返回有事件的）                    │
  └──────────────────────────────────────────────────────────┘
```

---

## 六、sendfile on 的零拷贝原理

### 6.1 传统 read + write（4 次拷贝）

```
客户端请求文件：GET /index.html

传统方式（sendfile off）：

  ┌─────────────┐     ┌──────────────┐     ┌─────────────┐
  │  磁盘       │     │  内核空间     │     │  用户空间    │
  │  (文件数据) │     │              │     │  (nginx)    │
  └──────┬──────┘     └──────┬───────┘     └──────┬──────┘
         │                   │                    │
    ① DMA 拷贝              │                    │
    ──────────→  page cache │                    │
         │      (内核缓冲区) │                    │
         │                   │  ② CPU 拷贝        │
         │                   │ ──────────→  用户缓冲区  │
         │                   │              (read 返回) │
         │                   │                    │
         │                   │  ③ CPU 拷贝        │
         │                   │ ←──────────  socket 缓冲区 │
         │                   │              (write 调用) │
         │                   │                    │
         │              ④ DMA 拷贝               │
         │              ──────────→  网卡发送     │
         │                   │                    │

  ★ 4 次拷贝：2 次 DMA + 2 次 CPU
  ★ 4 次上下文切换：read 进出 + write 进出
  ★ CPU 参与 2 次拷贝 → 浪费 CPU 周期
```

### 6.2 sendfile（2 次拷贝，零拷贝）

```
sendfile on 方式：

  ┌─────────────┐     ┌──────────────┐     ┌─────────────┐
  │  磁盘       │     │  内核空间     │     │  用户空间    │
  │  (文件数据) │     │              │     │  (nginx)    │
  └──────┬──────┘     └──────┬───────┘     └──────┬──────┘
         │                   │                    │
    ① DMA 拷贝              │                    │
    ──────────→  page cache │                    │
         │      (内核缓冲区) │                    │
         │                   │                    │
         │                   │  sendfile() 调用    │
         │                   │  用户态只发起调用    │
         │                   │  ★ 数据不经过用户态 │
         │                   │                    │
         │              ② DMA 拷贝               │
         │              ──────────→  网卡发送     │
         │     (page cache → socket buffer → NIC) │
         │                   │                    │

  ★ 2 次拷贝：2 次 DMA，0 次 CPU 拷贝
  ★ 2 次上下文切换：sendfile 进出
  ★ CPU 不参与数据搬运 → 释放 CPU 处理更多请求

  注意：sendfile 不是用 mmap，而是通过内核系统调用直接在内核空间
  完成 page cache → socket buffer 的数据传输（或 DMA scatter-gather）
```

### 6.3 sendfile 的限制

| 限制 | 说明 |
|------|------|
| 只能发送文件 | 不能发送动态生成的内容（proxy_pass 响应等） |
| 文件大小限制 | 早期内核有 2GB 限制，现代内核已无此问题 |
| 不经过用户态 | 无法在发送前修改文件内容（不能 gzip） |
| 需要配合 tcp_nopush | 合并 HTTP 头和文件数据到一个 TCP 包 |

---

## 七、nginx 高 CPU 排查流程

### 7.1 排查总流程图

```
nginx worker CPU 高占用
         │
         ▼
  ┌── 第一步：确认现象 ─────────────────────────────┐
  │  top -p $(pgrep -d, -f "nginx: worker")         │
  │  确认哪个 worker 的 CPU 高                       │
  │  确认是 %usr 高还是 %sys 高                     │
  └─────────────────────────────────────────────────┘
         │
    ┌────┴────┐
    ▼         ▼
 %usr 高    %sys 高
    │         │
    ▼         ▼
 用户态     内核态
 热点       系统调用
    │         │
    ▼         ▼
 perf top   strace -c
```

### 7.2 strace -p 看系统调用分布

```bash
# ★ 第一步：统计系统调用分布（快速定位热点）
WORKER_PID=$(ps aux | grep "nginx: worker" | head -1 | awk '{print $2}')
strace -c -p $WORKER_PID

# 等 10 秒后 Ctrl+C，查看统计报告：

# 预期输出（正常状态）：
# % time     seconds  usecs/call     calls    errors syscall
# ------ ----------- ----------- --------- --------- --------
#  65.23    0.012345          12      1029           epoll_wait
#  15.67    0.002965           3       988           accept4
#   8.45    0.001600           2       800           writev
#   5.23    0.000990           1       990           sendfile
#   3.12    0.000590           1       590           epoll_ctl
#   2.30    0.000435           1       435           write

# ★ 正常 nginx 的 syscall 分布：
#   epoll_wait 最多（等待事件）
#   accept4 ≈ writev ≈ sendfile（每个请求一组）
#   epoll_ctl 略少（只在 add/del 时调用）
```

### 7.3 诊断：accept4 占比异常高

```bash
# 如果 strace -c 显示 accept4 占比 > 40%：

# 可能的原因：连接风暴
#   - 大量短连接（客户端不使用 keep-alive）
#   - DDoS/CC 攻击
#   - CDN 回源未开启 keepalive

# 验证方法：
strace -p $WORKER_PID -e trace=accept4 -T 2>&1 | head -20

# 输出示例：
# accept4(6, {AF_INET, ...}, SOCK_NONBLOCK) = 3 <0.000015>
# accept4(6, {AF_INET, ...}, SOCK_NONBLOCK) = 4 <0.000012>
# accept4(6, {AF_INET, ...}, SOCK_NONBLOCK) = 5 <0.000010>
# ★ 每次 accept4 耗时 < 20μs，但频率极高

# 排查步骤：
# 1. 检查 access.log 中是否有大量不同 IP 的连接
# 2. 检查 HTTP 版本（HTTP/1.0 = 无 keep-alive）
#    grep "HTTP/1.0" /var/log/nginx/access.log | wc -l
# 3. 检查 stub_status 的 Active connections

# 解决方案：
# - 启用 upstream keepalive
# - 配置 limit_req/limit_conn
# - 检查 CDN 是否正确使用 keep-alive 回源
```

### 7.4 诊断：epoll_wait 频繁返回但事件少

```bash
# 如果 strace -c 显示 epoll_wait 调用次数 >> accept4 + read + write 之和：

# 可能的原因：busy polling 浪费
#   epoll_wait 被频繁唤醒但实际没有需要处理的事件
#   或者超时时间设置过短

# 验证方法：
strace -p $WORKER_PID -e trace=epoll_wait -T 2>&1 | head -30

# 输出示例：
# epoll_wait(16, [], 512, 0) = 0 <0.000005>    ← ★ 超时 0ms，返回 0 事件！
# epoll_wait(16, [], 512, 0) = 0 <0.000004>    ← 频繁空轮询
# epoll_wait(16, [{EPOLLIN, ...}], 512, 0) = 1 <0.000010>
# epoll_wait(16, [], 512, 0) = 0 <0.000003>    ← 又是空轮询

# ★ 如果大量 epoll_wait 返回 0 且 timeout=0：
#   说明有其他机制在驱动轮询（如 timer 事件）
#   或者 accept_mutex_delay 设置过短

# 解决方案：
# - 检查 accept_mutex_delay（默认 500ms，不要设太小）
# - 检查 timer_resolution（如果设置了，减少系统调用频率）
# - 检查是否有不必要的 timer 事件
```

### 7.5 perf top 看热点函数

```bash
# ★ 找出 CPU 消耗在哪些内核/用户态函数上
WORKER_PID=$(ps aux | grep "nginx: worker" | head -1 | awk '{print $2}')
perf top -p $WORKER_PID

# 预期输出（正常状态）：
#   Overhead  Symbol
#     15.20%  [kernel]  epoll_wait
#      8.50%  [kernel]  tcp_sendmsg
#      7.30%  [kernel]  tcp_v4_rcv
#      5.80%  nginx     ngx_http_process_request
#      4.20%  nginx     ngx_http_parse_request_line
#      3.90%  [kernel]  copy_user_enhanced_fast_string
#      3.50%  nginx     ngx_http_write_filter
#      2.80%  [kernel]  __netif_receive_skb_core
#      ...

# ★ 异常输出分析：
#
# 如果 ngx_http_parse_* 占比 > 20%：
#   → 请求头过大或过于复杂（大 Cookie、长 URL）
#   → 检查 client_header_buffer_size
#
# 如果 tcp_sendmsg 占比 > 30%：
#   → 网络发送瓶颈（带宽打满或 TCP 窗口问题）
#   → 检查是否启用了 sendfile
#
# 如果 futex 或 spin_lock 占比 > 10%：
#   → 锁竞争（不太可能在 nginx 单线程 worker 中出现）
#   → 除非使用了多线程模块（如 aio threads）
#
# 如果 copy_user_* 占比 > 15%：
#   → 数据拷贝开销大（未启用 sendfile）
#   → 启用 sendfile on
```

### 7.6 perf stat 快速评估

```bash
# ★ 快速评估 worker 的资源使用效率
perf stat -e cycles,instructions,cache-misses,cache-references,\
    context-switches,cpu-migrations,branch-misses \
    -p $WORKER_PID -- sleep 10

# 关键指标：
#
# IPC（Instructions Per Cycle）= instructions / cycles
#   IPC > 1.0  → CPU 利用率高，流水线饱满 ✓
#   IPC < 0.5  → CPU 等待（内存/缓存/IO）⚠️
#
# cache-miss 率 = cache-misses / cache-references
#   < 5%  → 缓存命中率高 ✓
#   > 15% → 缓存失效多，考虑绑核 ⚠️
#
# branch-misses
#   > 5% of instructions → 分支预测失败多，可能与请求模式有关
#
# cpu-migrations
#   > 0 → worker 在 CPU 核间迁移，启用 worker_cpu_affinity
```

### 7.7 综合排查清单

```
nginx 高 CPU 低 QPS 排查清单：

□ 1. 确认 CPU 占用类型
     top → %usr 高还是 %sys 高？
     
□ 2. strace -c 统计系统调用分布
     accept4 多 → 连接风暴
     epoll_wait 多但事件少 → busy polling
     write/sendfile 多 → 大量数据传输
     
□ 3. perf top 看热点函数
     用户态热点 → 请求处理逻辑
     内核态热点 → 网络栈/内存拷贝
     
□ 4. perf stat 看硬件指标
     cache-miss 高 → 绑核
     cpu-migrations > 0 → 绑核
     IPC 低 → 等待瓶颈
     
□ 5. 检查 nginx 配置
     worker_cpu_affinity 是否设置？
     sendfile 是否启用？
     keepalive_timeout 是否合理？
     open_file_cache 是否启用？
     
□ 6. 检查系统层面
     somaxconn 是否足够大？
     文件描述符限制是否足够？
     TCP 参数是否调优？
```

---

## ★ 总结：nginx 性能调优速查表

```
调优方向              │ 关键配置                         │ AMD 特别注意
─────────────────────┼──────────────────────────────────┼──────────────────────
worker 数量          │ worker_processes = CCD 数量       │ ★ 不是 CPU 核数！
                     │                                   │ 64 核 EPYC → 8 worker
绑核策略             │ worker_cpu_affinity               │ ★ 每 CCD 绑一组核
                     │ (按 CCD 拓扑)                     │ 避免跨 CCD 调度
事件模型             │ use epoll                         │ Linux 上唯一选择
连接接受             │ multi_accept on                   │ 高并发必开
零拷贝               │ sendfile on                       │ 静态文件必开
                     │ tcp_nopush on                     │ 配合 sendfile
文件缓存             │ open_file_cache max=N             │ 减少 stat/open 开销
连接复用             │ keepalive_requests 10000          │ 减少 TCP 握手
上游连接池           │ upstream keepalive N              │ 减少后端连接建立
```
