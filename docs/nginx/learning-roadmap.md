# nginx 性能分析学习路线

> 从 master-worker 架构到事件驱动模型，从请求生命周期到性能调优实战

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 源码阅读路线图](#2-源码阅读路线图)
  - [Step 1: 进程模型 — master-worker 架构](#step-1-进程模型--master-worker-架构)
  - [Step 2: 事件驱动 — epoll 与事件循环](#step-2-事件驱动--epoll-与事件循环)
  - [Step 3: 连接池与内存池 — 资源管理机制](#step-3-连接池与内存池--资源管理机制)
  - [Step 4: 请求处理 — 11 个 phase handler](#step-4-请求处理--11-个-phase-handler)
  - [Step 5: 缓冲链 — buffer chain 与 sendfile](#step-5-缓冲链--buffer-chain-与-sendfile)
  - [Step 6: 日志与监控 — access.log 与 stub_status](#step-6-日志与监控--accesslog-与-stub_status)
- [3. 核心概念速查](#3-核心概念速查)
- [4. 与性能工具的关系](#4-与性能工具的关系)
- [5. 动手实验清单](#5-动手实验清单)
- [6. 关键源码文件索引](#6-关键源码文件索引)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     客户端请求                               │
│          curl / ab / wrk / 浏览器 → HTTP 请求               │
├─────────────────────────────────────────────────────────────┤
│                     网络接口层                               │
│          listen() → accept() → epoll 事件循环               │
├─────────────────────────────────────────────────────────────┤
│                     nginx 进程模型                           │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐                │
│  │  master  │──→│ worker 0 │   │ worker 1 │   ...          │
│  │  (管理)  │   │ (处理)   │   │ (处理)   │                │
│  └──────────┘   └──────────┘   └──────────┘                │
├─────────────────────────────────────────────────────────────┤
│                     请求处理流水线                           │
│  accept → read → 11 phases → write → log                   │
│  NGX_HTTP_POST_READ_PHASE → ... → NGX_HTTP_LOG_PHASE       │
├─────────────────────────────────────────────────────────────┤
│                     底层支撑                                 │
│  epoll/kqueue  │  内存池  │  连接池  │  buffer chain        │
└─────────────────────────────────────────────────────────────┘
```

### 请求处理全路径

```
客户端 TCP 连接
     │
     ▼
 ┌── listen() ──────────────────────────────┐
 │  master 进程创建监听 socket               │
 │  SO_REUSEPORT (可选，多 worker 共享)      │
 └──────────────────────────────────────────┘
     │
     ▼
 ┌── accept4(SOCK_NONBLOCK) ────────────────┐
 │  worker 通过 epoll 竞争 accept            │
 │  返回非阻塞连接 fd                        │
 └──────────────────────────────────────────┘
     │
     ▼
 ┌── 从连接池分配 ngx_connection_t ─────────┐
 │  从内存池分配请求相关内存                 │
 │  创建 ngx_http_request_t                 │
 └──────────────────────────────────────────┘
     │
     ▼
 ┌── epoll_ctl(ADD) ────────────────────────┐
 │  将连接 fd 加入 epoll，监听 EPOLLIN      │
 │  设置 EPOLLET (边缘触发)                 │
 └──────────────────────────────────────────┘
     │
     ▼
 ┌── 读取 HTTP 请求 ────────────────────────┐
 │  read() → 解析请求行 → 解析请求头        │
 │  ngx_http_read_request_header()          │
 └──────────────────────────────────────────┘
     │
     ▼
 ┌── 11 个 Phase Handler 流水线 ────────────┐
 │  POST_READ → SERVER_REWRITE → FIND_CONFIG│
 │  → REWRITE → POST_REWRITE → PREACCESS    │
 │  → ACCESS → POST_ACCESS → PRECONTENT     │
 │  → CONTENT → LOG                          │
 └──────────────────────────────────────────┘
     │
     ▼
 ┌── 发送响应 ──────────────────────────────┐
 │  writev() 发送响应头                     │
 │  sendfile() 发送文件体（零拷贝）         │
 │  或 write() 发送动态内容                 │
 └──────────────────────────────────────────┘
     │
     ▼
 ┌── 日志记录 ──────────────────────────────┐
 │  LOG_PHASE: 写入 access.log              │
 │  归还连接到连接池                        │
 └──────────────────────────────────────────┘
```

---

## 2. 源码阅读路线图

### Step 1: 进程模型 — master-worker 架构

**目标：** 理解 nginx 的进程模型，master 和 worker 的职责划分

**文件位置：**
- nginx 源码：`src/nginx/src/core/nginx.c` — `main()` 入口
- 进程管理：`src/nginx/src/os/unix/ngx_process_cycle.c` — master/worker 循环

**阅读要点：**

```
nginx 进程模型：

┌────────────────────────────────────┐
│          master 进程 (root)        │
│  ────────────────────────────────  │
│  1. 读取并解析 nginx.conf          │
│  2. 创建 listen socket             │
│  3. fork() worker 子进程           │
│  4. 信号处理 (reload/stop/quit)    │
│  5. 监控 worker 健康状态           │
│  6. 管理热升级 (binary upgrade)    │
└────────────────────────────────────┘
         │ fork() × N
         ▼
┌────────────────────────────────────┐
│      worker 进程 (nginx 用户)      │
│  ────────────────────────────────  │
│  1. 继承 listen socket             │
│  2. 创建 epoll 实例                │
│  3. 事件循环 (event loop)          │
│  4. accept → 处理请求 → 响应      │
│  5. 每个 worker 单线程             │
│  6. 处理数万并发连接               │
└────────────────────────────────────┘
```

**master vs worker 职责对比：**

| 维度 | master 进程 | worker 进程 |
|------|------------|------------|
| **数量** | 1 个 | worker_processes 个（通常 = CPU 核数） |
| **权限** | root | nginx/worker 用户 |
| **核心循环** | `ngx_master_process_cycle()` — 等待信号 | `ngx_worker_process_cycle()` — epoll_wait |
| **阻塞方式** | `sigsuspend()` 等待信号 | `epoll_wait()` 等待 I/O |
| **职责** | 进程管理、信号转发 | 处理所有客户端请求 |
| **CPU 消耗** | 接近 0 | 承载全部业务负载 |

**思考题：**
1. 为什么每个 worker 是单线程而不是多线程？（答：避免锁竞争，简化编程模型，epoll 单线程已能处理数万连接）
2. master 挂了会怎样？（答：worker 继续运行，但无法 reload/重启 worker，需手动干预）
3. worker 挂了会怎样？（答：master 检测到 SIGCHLD，自动 fork 新 worker 替代）

**实操：** 用 `ps aux | grep nginx` 观察 master 和 worker 进程的 PID 和用户。

---

### Step 2: 事件驱动 — epoll 与事件循环

**目标：** 理解 nginx 事件驱动模型的核心机制

**文件位置：**
- 事件循环：`src/nginx/src/event/ngx_event.c` — `ngx_process_events_and_timers()`
- epoll 模块：`src/nginx/src/event/modules/ngx_epoll_module.c` — epoll 实现
- 连接处理：`src/nginx/src/event/ngx_event_accept.c` — accept 处理

**阅读要点：**

```
事件循环核心逻辑：

ngx_worker_process_cycle()
 │
 └── for (;;) {                           ← ★ 无限循环
       ngx_process_events_and_timers(cycle);
       │
       ├── ngx_process_events()           ← ★ epoll_wait()
       │     ├── epoll_wait(ep, events, nevents, timer)
       │     └── 遍历就绪事件
       │           ├── accept 事件 → ngx_event_accept()
       │           ├── 读事件 → rev->handler(rev)
       │           └── 写事件 → wev->handler(wev)
       │
       └── ngx_event_expire_timers()      ← 处理超时
             └── 遍历超时定时器红黑树
```

**epoll 关键参数：**

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `worker_connections` | 1024 | ★ 每个 worker 最大连接数（含 listen fd + upstream） |
| `multi_accept` | off | 一次 epoll_wait 接受所有就绪连接 vs 只接受一个 |
| `use epoll` | 自动 | Linux 默认使用 epoll（macOS 用 kqueue） |
| `accept_mutex` | off | worker 间竞争 accept 的互斥锁（1.11.3 后用 EPOLLEXCLUSIVE） |

**epoll 事件类型映射：**

| epoll 事件 | nginx 处理 | 对应阶段 |
|-----------|-----------|---------|
| `EPOLLIN` (listen fd) | `ngx_event_accept()` | accept 新连接 |
| `EPOLLIN` (client fd) | `rev->handler()` | 读取请求数据 |
| `EPOLLOUT` (client fd) | `wev->handler()` | 发送响应数据 |
| `EPOLLRDHUP` | 关闭连接 | 客户端断开 |
| `EPOLLET` | 边缘触发模式 | ★ nginx 默认使用 ET |

**思考题：**
1. 为什么 nginx 使用边缘触发（ET）而不是水平触发（LT）？（答：ET 减少 epoll_wait 返回次数，避免惊群，但要求非阻塞 + 循环读写直到 EAGAIN）
2. `worker_connections` 设置为 1024 时，实际最大并发连接数是多少？（答：`worker_processes × worker_connections`，但要减去 listen fd 和 upstream 连接）
3. `accept_mutex` 关闭后如何解决惊群问题？（答：Linux 4.5+ 的 `EPOLLEXCLUSIVE` 标志，内核保证只唤醒一个 worker）

---

### Step 3: 连接池与内存池 — 资源管理机制

**目标：** 理解 nginx 如何通过连接池和内存池实现高效的资源管理

**文件位置：**
- 连接池：`src/nginx/src/core/ngx_connection.c` — `ngx_get_connection()`
- 内存池：`src/nginx/src/core/ngx_palloc.c` — `ngx_palloc()`

**阅读要点：**

```
连接池（Connection Pool）：

预分配 N 个 ngx_connection_t（N = worker_connections）
┌───────────────────────────────────────────┐
│  free_connections (空闲链表)              │
│  ┌────┐   ┌────┐   ┌────┐   ┌────┐      │
│  │c[0]│→  │c[1]│→  │c[2]│→  │c[3]│→... │
│  └────┘   └────┘   └────┘   └────┘      │
└───────────────────────────────────────────┘
                    │ ngx_get_connection()
                    ▼
            分配一个空闲连接
                    │ ngx_free_connection()
                    ▼
            归还到空闲链表

★ 优点：避免频繁 malloc/free，O(1) 分配和回收
```

```
内存池（Memory Pool）：

每个连接/请求创建独立的内存池
┌─────────────────────────────────────────┐
│  ngx_pool_t                             │
│  ┌────────┐  ┌────────┐  ┌────────┐   │
│  │block 0 │→ │block 1 │→ │block 2 │→… │
│  │4096 B  │  │4096 B  │  │8192 B  │   │
│  │[已分配]│  │[已分配]│  │[当前]  │   │
│  └────────┘  └────────┘  └────────┘   │
│  large: [大块 malloc 链表]              │
│  cleanup: [清理回调链表]                │
└─────────────────────────────────────────┘
请求结束 → 一次性释放整个内存池
★ 优点：无内存碎片，无内存泄漏，释放 O(1)
```

**连接池 vs 内存池对比：**

| 维度 | 连接池 | 内存池 |
|------|--------|--------|
| **分配对象** | `ngx_connection_t` | 任意大小的内存块 |
| **分配策略** | 预分配 + 空闲链表 | 按需分配 block + 大块走 malloc |
| **释放策略** | 归还到空闲链表 | 销毁时一次性释放 |
| **生命周期** | worker 进程生命周期 | 单次请求/连接生命周期 |
| **核心优势** | O(1) 分配/回收，无碎片 | 无泄漏，批量释放 |

---

### Step 4: 请求处理 — 11 个 phase handler

**目标：** 理解 HTTP 请求在 nginx 中经过的完整处理流水线

**文件位置：**
- phase 定义：`src/nginx/src/http/ngx_http_core_module.h`
- phase 初始化：`src/nginx/src/http/ngx_http_core_module.c` — `ngx_http_core_run_phases()`
- 请求处理：`src/nginx/src/http/ngx_http_request.c` — `ngx_http_process_request()`

**阅读要点：**

```
HTTP 请求处理的 11 个 Phase：

  Phase #  │ Phase 名称                 │ 典型用途
  ─────────┼────────────────────────────┼────────────────────────────────
    0      │ POST_READ                  │ realip 模块（还原真实 IP）
    1      │ SERVER_REWRITE             │ server 级别的 rewrite
    2      │ FIND_CONFIG                │ ★ location 匹配（内部使用）
    3      │ REWRITE                    │ location 级别的 rewrite
    4      │ POST_REWRITE               │ rewrite 后跳转（内部使用）
    5      │ PREACCESS                  │ limit_conn, limit_req（限流）
    6      │ ACCESS                     │ ★ access 控制（allow/deny, auth）
    7      │ POST_ACCESS                │ satisfy any/all 逻辑（内部使用）
    8      │ PRECONTENT                 │ try_files, realip 后处理
    9      │ CONTENT                    │ ★ 内容生成（静态文件、proxy_pass、fastcgi）
   10      │ LOG                        │ access_log 记录

★ 核心 Phase：FIND_CONFIG（路由匹配）、ACCESS（权限控制）、CONTENT（内容生成）
```

**请求处理流程图：**

```
ngx_http_process_request()
     │
     ▼
 ┌── POST_READ_PHASE ──┐
 │  realip: X-Real-IP  │
 └──────────┬──────────┘
            ▼
 ┌── SERVER_REWRITE ───┐
 │  rewrite 规则       │
 └──────────┬──────────┘
            ▼
 ┌── FIND_CONFIG ──────┐     ★ 核心：location 匹配
 │  location 前缀匹配  │     正则匹配按配置文件顺序
 │  正则匹配           │
 └──────────┬──────────┘
            ▼
 ┌── REWRITE ──────────┐
 │  location rewrite   │
 │  return 指令        │
 └──────────┬──────────┘
            ▼
 ┌── PREACCESS ────────┐
 │  limit_req (限流)   │
 │  limit_conn (限连接) │
 └──────────┬──────────┘
            ▼
 ┌── ACCESS ───────────┐     ★ 权限检查
 │  allow / deny       │     403 Forbidden
 │  auth_basic         │     401 Unauthorized
 └──────────┬──────────┘
            ▼
 ┌── PRECONTENT ───────┐
 │  try_files          │     ★ try_files $uri $uri/ /index.html
 └──────────┬──────────┘
            ▼
 ┌── CONTENT ──────────┐     ★ 内容生成（最重要的 phase）
 │  static: 发送文件   │     sendfile() 零拷贝
 │  proxy_pass: 反代   │     upstream 连接
 │  fastcgi: PHP-FPM   │     FastCGI 协议
 │  gzip: 压缩         │     动态压缩
 └──────────┬──────────┘
            ▼
 ┌── LOG ──────────────┐
 │  access_log 写入    │     记录请求信息
 │  清理请求资源       │     释放内存池
 └─────────────────────┘
```

**思考题：**
1. 为什么 FIND_CONFIG 是"内部" phase？（答：由 core 模块自动执行，用户不能注册 handler）
2. `try_files` 在哪个 phase 执行？（答：PRECONTENT phase，在 CONTENT 之前）
3. 一个请求能同时经过 CONTENT phase 的多个 handler 吗？（答：不能，CONTENT phase 的 handler 是互斥的，第一个返回 OK 的就终止后续处理）

---

### Step 5: 缓冲链 — buffer chain 与 sendfile

**目标：** 理解 nginx 如何高效地传输响应数据

**文件位置：**
- buffer 定义：`src/nginx/src/core/ngx_buf.h`
- 输出过滤链：`src/nginx/src/http/ngx_http_write_filter_module.c`
- sendfile：`src/nginx/src/os/unix/ngx_sendfile.c`

**阅读要点：**

```
Buffer Chain（缓冲链）：

HTTP 响应 = 多个 ngx_buf_t 串成链表

  ngx_chain_t → ngx_chain_t → ngx_chain_t → NULL
  ┌──────────┐  ┌──────────┐  ┌──────────┐
  │ ngx_buf_t│  │ ngx_buf_t│  │ ngx_buf_t│
  │ 响应头   │  │ 文件片段 │  │ 尾部数据 │
  │ pos→last │  │ file_pos │  │ pos→last │
  │ memory=1 │  │ file_last│  │ memory=1 │
  │          │  │ in_file=1│  │          │
  └──────────┘  └──────────┘  └──────────┘

★ 关键设计：内存 buffer 和文件 buffer 可以混合在同一条链中
```

```
sendfile() 零拷贝机制：

传统方式（read + write）：        sendfile 方式（零拷贝）：

  磁盘 → [内核缓冲区]              磁盘 → [内核缓冲区]
         │                                   │
         ▼                                   │ (DMA copy)
     用户空间 buffer                         │
         │                                   ▼
         ▼                              socket 发送缓冲区
     write()                                │
         │                                   ▼
         ▼                              网络接口
     socket 发送缓冲区
         │
         ▼
     网络接口

  4 次数据拷贝 + 4 次上下文切换      2 次数据拷贝 + 2 次上下文切换
  ★ sendfile 减少 2 次拷贝，CPU 和带宽节省显著
```

**sendfile 相关配置：**

| 指令 | 默认值 | 含义 |
|------|--------|------|
| `sendfile on` | off | ★ 启用 sendfile 零拷贝 |
| `tcp_nopush on` | off | 配合 sendfile，合并 HTTP 头和文件到一个 TCP 包 |
| `tcp_nodelay on` | on | 禁用 Nagle 算法，减少小包延迟 |
| `sendfile_max_chunk 512k` | 0（不限） | 单次 sendfile 最大传输量（防止大文件阻塞 worker） |

---

### Step 6: 日志与监控 — access.log 与 stub_status

**目标：** 理解 nginx 的日志系统和运行状态监控

**文件位置：**
- 日志模块：`src/nginx/src/http/modules/ngx_http_log_module.c`
- stub_status：`src/nginx/src/http/modules/ngx_http_stub_status_module.c`

**阅读要点：**

**默认 access.log 格式：**

```
$remote_addr - $remote_user [$time_local] "$request"
$status $body_bytes_sent "$http_referer"
"$http_user_agent" "$http_x_forwarded_for"
```

**字段映射：**

| 变量 | 含义 | 示例值 |
|------|------|--------|
| `$remote_addr` | 客户端 IP | `127.0.0.1` |
| `$remote_user` | HTTP Basic Auth 用户名 | `-`（无认证） |
| `$time_local` | 服务器本地时间 | `[03/Jul/2026:23:24:35 +0800]` |
| `$request` | 请求行 | `GET / HTTP/1.1` |
| `$status` | ★ HTTP 状态码 | `200` |
| `$body_bytes_sent` | 响应体大小（字节） | `457` |
| `$http_referer` | Referer 头 | `-` |
| `$http_user_agent` | 客户端标识 | `curl/8.4.0` |
| `$http_x_forwarded_for` | X-Forwarded-For 头 | `-` |

**stub_status 输出解读：**

```
Active connections: 3            ← ★ 当前活跃连接数（含等待中的 keepalive）
server accepts handled requests  ← 标题行
 12345 12345 67890               ← 已接受 / 已处理 / 总请求数
Reading: 0 Writing: 1 Waiting: 2 ← ★ 读/写/等待状态分布
```

| 指标 | 含义 | 异常信号 |
|------|------|---------|
| Active connections | 当前所有连接 | 持续增长 → 连接泄漏或慢客户端 |
| accepts | 已接受的连接总数 | accepts > handled → worker 资源不足 |
| handled | 已处理的连接总数 | 应与 accepts 相等 |
| requests | 总请求数 | requests/handled = 平均 keepalive 请求数 |
| Reading | 正在读取请求头的连接 | 大量 Reading → 慢客户端攻击 |
| Writing | 正在发送响应的连接 | 大量 Writing → 后端响应慢 |
| Waiting | keepalive 空闲连接 | 正常值，受 keepalive_timeout 控制 |

---

## 3. 核心概念速查

### event loop（事件循环）

```
┌─────────────────────────────────────────┐
│            nginx worker 事件循环        │
│                                         │
│  ┌──────────────────────────────────┐   │
│  │  1. epoll_wait(timeout)          │   │
│  │     等待 I/O 事件或定时器到期   │   │
│  └──────────────┬───────────────────┘   │
│                 │ 返回就绪事件           │
│                 ▼                        │
│  ┌──────────────────────────────────┐   │
│  │  2. 遍历就绪事件                 │   │
│  │     调用对应的 handler           │   │
│  └──────────────┬───────────────────┘   │
│                 │                        │
│                 ▼                        │
│  ┌──────────────────────────────────┐   │
│  │  3. 处理定时器事件               │   │
│  │     超时连接、keepalive 清理     │   │
│  └──────────────┬───────────────────┘   │
│                 │                        │
│                 └──→ 回到步骤 1          │
└─────────────────────────────────────────┘
```

### connection pool（连接池）

- **预分配**：worker 启动时一次性分配 `worker_connections` 个 `ngx_connection_t`
- **O(1) 获取**：从空闲链表头取出，`free_connections--`
- **O(1) 归还**：插入空闲链表头，`free_connections++`
- **上限**：`worker_connections` 包含所有连接（listen + client + upstream）

### buffer chain（缓冲链）

- **ngx_buf_t**：可以是内存 buffer（`pos/last`）或文件 buffer（`file_pos/file_last`）
- **ngx_chain_t**：单向链表节点，`buf` + `next`
- **输出过滤链**：headers_filter → body_filter → write_filter（逆序注册，正序执行）
- **零拷贝**：文件类型的 buffer 直接调用 `sendfile()`，避免数据拷贝到用户态

### phase handler（阶段处理器）

- **注册时**：各模块在 `postconfiguration` 回调中注册 handler 到对应 phase
- **执行时**：`ngx_http_core_run_phases()` 按 phase 顺序依次调用
- **返回值**：`NGX_OK`（停止）、`NGX_DECLINED`（继续下一个）、`NGX_AGAIN`（等待事件）
- **CONTENT phase 特殊**：handler 互斥，只执行一个（由 location 配置决定）

---

## 4. 与性能工具的关系

### strace 追踪 nginx 系统调用

```bash
# 追踪 worker 处理一个请求的完整系统调用链
WORKER_PID=$(ps aux | grep "nginx: worker" | head -1 | awk '{print $2}')
strace -p $WORKER_PID -e trace=accept4,read,write,epoll_wait,sendfile,writev,close -tt
```

**预期看到的系统调用序列：**

```
epoll_wait()     ← 等待事件
accept4()        ← 接受新连接（SOCK_NONBLOCK）
epoll_ctl(ADD)   ← 将新连接加入 epoll
epoll_wait()     ← 等待客户端数据
read()           ← 读取 HTTP 请求
writev()         ← 发送响应头
sendfile()       ← 发送文件体（零拷贝）
write()          ← 写入 access.log
close()          ← 关闭文件描述符
epoll_wait()     ← 回到事件循环
```

### perf 分析 CPU 热点

```bash
# 压测 + perf stat 统计
perf stat -e context-switches,cpu-migrations \
    -p $(pgrep -d, -f "nginx: worker") -- \
    ab -n 10000 -c 10 http://localhost/

# perf record 采样热点函数（需要 debug 符号）
perf record -g -p $WORKER_PID -- ab -n 100000 -c 50 http://localhost/
perf report --stdio | head -30
```

**常见热点函数：**

| 函数 | 含义 | 优化方向 |
|------|------|---------|
| `epoll_wait` | 事件循环等待 | 正常，idle 时占比高 |
| `ngx_http_parse_request_line` | 解析请求行 | 检查是否正则回溯 |
| `ngx_http_process_request_headers` | 处理请求头 | 大量 header 时可能热点 |
| `ngx_sendfile` | 文件发送 | 检查磁盘 I/O |
| `ngx_palloc` | 内存分配 | 通常很快，除非内存池耗尽 |

### bpftrace 追踪请求延迟

```bash
# 追踪每个 HTTP 请求的处理时间
bpftrace -e '
uprobe:/usr/sbin/nginx:ngx_http_process_request {
    @start[tid] = nsecs;
}

uretprobe:/usr/sbin/nginx:ngx_http_finalize_request {
    if (@start[tid]) {
        @latency = hist(nsecs - @start[tid]);
        delete(@start[tid]);
    }
}
'
```

### 工具选择矩阵

| 场景 | 工具 | 目的 |
|------|------|------|
| 单个请求慢 | strace | 看系统调用序列，找阻塞点 |
| CPU 占用高 | perf | 采样热点函数 |
| 大量 TIME_WAIT | ss / netstat | 检查连接状态分布 |
| 请求延迟分布 | bpftrace | 直方图统计延迟 |
| 内存增长 | valgrind / pprof | 检查内存泄漏 |
| 连接数异常 | stub_status | 实时监控连接状态 |
| 磁盘 I/O 瓶颈 | blktrace | 追踪 sendfile 底层 I/O |

---

## 5. 动手实验清单

### 实验 1: 基础进程模型观察

```bash
#!/bin/bash
# 观察 nginx master-worker 架构

# 1. 启动 nginx
nginx

# 2. 查看进程树
ps -eo pid,ppid,user,args | grep nginx | grep -v grep

# 3. 预期输出：
# master (root) → worker × N (nginx 用户)
# master PID = $(cat /run/nginx.pid)

# 4. 查看 worker 打开的文件描述符
WORKER_PID=$(ps aux | grep "nginx: worker" | head -1 | awk '{print $2}')
ls -la /proc/$WORKER_PID/fd/ | head -20

# 5. 预期看到：
# listen socket (fd=6), epoll fd, eventfd, access.log fd 等
```

### 实验 2: strace 追踪请求生命周期

```bash
#!/bin/bash
# strace 追踪单个 HTTP 请求的系统调用序列

# 1. 获取 worker PID
WORKER_PID=$(ps aux | grep "nginx: worker" | head -1 | awk '{print $2}')

# 2. 启动 strace
strace -p $WORKER_PID -e trace=accept4,read,write,epoll_wait,sendfile,writev,close \
    -tt -o /tmp/nginx_strace.out &

# 3. 发送请求
sleep 0.5
curl -s http://localhost/ > /dev/null

# 4. 停止 strace
sleep 1
kill %1
wait

# 5. 分析输出
cat /tmp/nginx_strace.out
```

### 实验 3: ab 压测 + stub_status 监控

```bash
#!/bin/bash
# 压测 nginx 并观察 stub_status

# 1. 记录压测前状态
echo "=== 压测前 ==="
curl -s http://localhost/nginx_status

# 2. 启动压测（后台）
ab -n 100000 -c 50 http://localhost/ &

# 3. 压测期间观察
sleep 2
echo "=== 压测中 ==="
curl -s http://localhost/nginx_status

# 4. 等待压测完成
wait
echo "=== 压测后 ==="
curl -s http://localhost/nginx_status
```

### 实验 4: perf stat 性能分析

```bash
#!/bin/bash
# perf stat 统计 nginx 处理请求时的硬件事件

# 获取所有 worker PID
WORKER_PIDS=$(pgrep -d, -f "nginx: worker")

# perf stat + ab 压测
perf stat -e context-switches,cpu-migrations \
    -p $WORKER_PIDS -- \
    ab -n 10000 -c 10 http://localhost/

# 关注指标：
# - context-switches: 上下文切换次数（越少越好）
# - cpu-migrations: CPU 迁移次数（worker 绑核后应为 0）
```

### 实验 5: 配置调优对比

```bash
#!/bin/bash
# 对比不同配置的性能差异

# --- 配置 A: 默认配置 ---
# worker_processes auto;
# worker_connections 1024;
ab -n 50000 -c 100 http://localhost/ 2>&1 | grep "Requests per second"

# --- 配置 B: 调优配置 ---
# worker_processes auto;
# worker_connections 4096;
# sendfile on;
# tcp_nopush on;
# tcp_nodelay on;
# keepalive_timeout 65;
# open_file_cache max=10000 inactive=60s;
nginx -s reload
sleep 1
ab -n 50000 -c 100 http://localhost/ 2>&1 | grep "Requests per second"
```

---

## 6. 关键源码文件索引

### 核心框架

| 文件 | 关注点 |
|------|--------|
| `src/core/nginx.c` | `main()` 入口，命令行参数解析 |
| `src/core/ngx_cycle.c` | 配置循环（reload 时的核心逻辑） |
| `src/core/ngx_connection.c` | ★ 连接池：`ngx_get_connection()`、`ngx_free_connection()` |
| `src/core/ngx_palloc.c` | ★ 内存池：`ngx_palloc()`、`ngx_pfree()` |
| `src/core/ngx_buf.h` | buffer 和 chain 的数据结构定义 |
| `src/core/ngx_string.c` | 字符串处理工具函数 |

### 事件模块

| 文件 | 关注点 |
|------|--------|
| `src/event/ngx_event.c` | ★ 事件循环核心：`ngx_process_events_and_timers()` |
| `src/event/ngx_event_accept.c` | accept 处理：`ngx_event_accept()`、`ngx_accept_disabled` |
| `src/event/modules/ngx_epoll_module.c` | ★ epoll 实现：`ngx_epoll_process_events()` |
| `src/event/ngx_event_timer.c` | 定时器红黑树实现 |
| `src/event/ngx_event_connect.c` | upstream 连接管理 |

### HTTP 模块

| 文件 | 关注点 |
|------|--------|
| `src/http/ngx_http_core_module.h` | ★ 11 个 phase 的枚举定义 |
| `src/http/ngx_http_core_module.c` | phase handler 注册与执行 |
| `src/http/ngx_http_request.c` | ★ 请求处理全流程：accept → process → finalize |
| `src/http/ngx_http_parse.c` | HTTP 协议解析（请求行、请求头） |
| `src/http/ngx_http_upstream.c` | upstream 反向代理逻辑 |
| `src/http/modules/ngx_http_log_module.c` | access_log 模块 |
| `src/http/modules/ngx_http_stub_status_module.c` | stub_status 模块 |

### 进程管理

| 文件 | 关注点 |
|------|--------|
| `src/os/unix/ngx_process_cycle.c` | ★ master/worker 进程循环 |
| `src/os/unix/ngx_process.c` | 信号处理、进程管理 |
| `src/os/unix/ngx_daemon.c` | 守护进程化 |
| `src/os/unix/ngx_sendfile.c` | sendfile 系统调用封装 |

### 推荐阅读顺序

```
1. nginx.c (main)                    → 理解启动流程
2. ngx_process_cycle.c               → 理解 master-worker 模型
3. ngx_event.c + ngx_epoll_module.c  → 理解事件循环
4. ngx_connection.c + ngx_palloc.c   → 理解资源管理
5. ngx_http_request.c                → 理解请求处理
6. ngx_http_core_module.h            → 理解 11 phase 定义
7. ngx_http_log_module.c             → 理解日志机制
```

---

## 参考资源

- [nginx 官方文档](http://nginx.org/en/docs/)
- [nginx 源码 GitHub 镜像](https://github.com/nginx/nginx)
- [nginx 开发指南](http://nginx.org/en/docs/dev/development_guide.html)
- [Understanding nginx Architecture](https://www.aosabook.org/en/nginx.html)
- [nginx 源码分析 — 事件驱动](https://nginx.org/en/docs/events.html)
- [High Performance Browser Networking — nginx 章节](https://hpbn.co/)
- [nginx 性能调优实战 (DigitalOcean)](https://www.digitalocean.com/community/tutorials/how-to-optimize-nginx-configuration)
- [Linux sendfile(2) man page](https://man7.org/linux/man-pages/man2/sendfile.2.html)
- [epoll(7) man page](https://man7.org/linux/man-pages/man7/epoll.7.html)
