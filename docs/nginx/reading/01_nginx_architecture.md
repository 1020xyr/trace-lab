# nginx 核心架构深度解析

> 从 master-worker 进程模型到事件驱动，从连接池到 11 phase 请求处理流水线
> 预计阅读时间：25 分钟

---

## 阅读要点

1. ★ master-worker 进程模型的职责划分和生命周期
2. ★ 事件驱动模型（epoll/kqueue/select）的实现机制
3. ★ 连接池与内存池的设计思想
4. ★ 11 个 HTTP phase handler 的执行顺序和用途
5. ★ 请求处理的完整生命周期

---

## 一、nginx 进程架构总览

```
┌─────────────────────────────────────────────────────────────────────┐
│                        nginx 进程组                                 │
│                                                                     │
│  ┌─────────────┐                                                    │
│  │   master    │  PID=1234  user=root                               │
│  │   进程      │  职责：进程管理、信号处理、配置加载                 │
│  │             │  循环：sigsuspend() 等待信号                       │
│  └──────┬──────┘                                                    │
│         │ fork()                                                    │
│    ┌────┼────┬────────┐                                             │
│    ▼    ▼    ▼        ▼                                             │
│  ┌────┐┌────┐┌────┐┌────┐                                          │
│  │ W0 ││ W1 ││ W2 ││ W3 │  各 worker PID 不同, user=nginx          │
│  │    ││    ││    ││    │  每个 worker 独立处理所有请求             │
│  └────┘└────┘└────┘└────┘                                          │
│    │    │    │    │                                                 │
│    ▼    ▼    ▼    ▼                                                 │
│  epoll  epoll  epoll  epoll  ← 每个 worker 独立的 epoll 实例       │
│  实例   实例   实例   实例                                          │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.1 master 进程详解

**源码位置：** `src/os/unix/ngx_process_cycle.c` — `ngx_master_process_cycle()`

```c
/*
 * master 进程主循环（伪代码）
 *
 * ★ 核心职责：不做任何请求处理，只管进程生命周期
 */

void ngx_master_process_cycle(ngx_cycle_t *cycle)
{
    /* 设置信号掩码，只在 sigsuspend 时接受信号 */
    sigset_t set;
    sigfillset(&set);
    sigprocmask(SIG_BLOCK, &set, NULL);

    for (;;) {
        /* ★ sigsuspend()：阻塞等待信号，等同于 sleep 直到信号唤醒 */
        sigsuspend(&set);

        /* ── 信号处理分支 ───────────────────────────── */

        if (ngx_reap) {
            /* SIGCHLD: worker 进程退出 */
            ngx_reap_processes(cycle);
            /* ★ master 检测到 worker 退出后，自动 fork 新 worker */
        }

        if (ngx_reconfigure) {
            /* SIGHUP (nginx -s reload) */
            ngx_reconfigure = 0;
            /* 1. 重新读取 nginx.conf */
            /* 2. 创建新的 listen socket */
            /* 3. fork 新 worker */
            /* 4. 优雅关闭旧 worker */
            ngx_start_worker_processes(cycle, NGX_PROCESS_JUST_RESPAWN);
        }

        if (ngx_quit) {
            /* SIGQUIT (nginx -s quit) */
            /* 优雅关闭：通知所有 worker 处理完当前请求后退出 */
            ngx_signal_worker_processes(cycle, NGX_PROCESS_QUIT_CMD);
        }

        if (ngx_terminate) {
            /* SIGTERM (nginx -s stop) */
            /* 强制关闭：直接发送 SIGTERM 给所有 worker */
            ngx_signal_worker_processes(cycle, NGX_PROCESS_TERMINATE);
        }
    }
}
```

**master 进程信号处理映射表：**

| 命令 | 信号 | master 行为 | worker 行为 |
|------|------|------------|------------|
| `nginx -s reload` | SIGHUP | 重新加载配置，启动新 worker，优雅关闭旧 worker | 旧 worker 完成当前请求后退出 |
| `nginx -s stop` | SIGTERM | 强制终止所有 worker | 立即退出 |
| `nginx -s quit` | SIGQUIT | 优雅关闭 | 处理完当前请求后退出 |
| `nginx -s reopen` | SIGUSR1 | 转发给所有 worker | 重新打开日志文件（logrotate） |
| `kill -USR2 master` | SIGUSR2 | 热升级：启动新 master | 旧 master 的 worker 继续运行 |
| `kill -WINCH master` | SIGWINCH | 热升级后关闭旧 worker | 旧 worker 优雅退出 |

### 1.2 worker 进程详解

**源码位置：** `src/os/unix/ngx_process_cycle.c` — `ngx_worker_process_cycle()`

```c
/*
 * worker 进程主循环（伪代码）
 *
 * ★ 核心职责：所有请求都在这里处理
 */

void ngx_worker_process_cycle(ngx_cycle_t *cycle, void *data)
{
    /* 1. 初始化 worker */
    ngx_worker_process_init(cycle, 1);
    /*    ├── 初始化事件模块（创建 epoll 实例） */
    /*    ├── 初始化连接池（预分配 worker_connections 个连接） */
    /*    ├── 初始化内存池 */
    /*    └── 将 listen socket 加入 epoll */

    /* 2. ★ 事件循环（无限循环） */
    for (;;) {
        ngx_process_events_and_timers(cycle);
        /*    ├── ngx_process_events()  → epoll_wait() */
        /*    │     └── 处理就绪事件（accept / read / write） */
        /*    │ */
        /*    └── ngx_event_expire_timers()  → 处理超时 */
        /*          └── 清理超时的 keepalive 连接 */

        if (ngx_terminate || ngx_quit) {
            /* 收到终止信号，退出循环 */
            break;
        }
    }

    /* 3. 清理并退出 */
    ngx_worker_process_exit(cycle);
}
```

**worker 进程内部结构：**

```
┌─────────────────────────────────────────────────────┐
│  worker 进程 (PID=460582, user=nginx)               │
│                                                     │
│  ┌─────────────────────────────────────────────┐    │
│  │  事件循环 (ngx_event_loop)                  │    │
│  │  ┌──────────────────────────────────────┐    │    │
│  │  │  epoll 实例 (ep=16)                  │    │    │
│  │  │  ┌──────┐ ┌──────┐ ┌──────┐         │    │    │
│  │  │  │fd=6  │ │fd=3  │ │fd=8  │  ...    │    │    │
│  │  │  │listen│ │client│ │upstr │         │    │    │
│  │  │  │:80   │ │conn  │ │eam   │         │    │    │
│  │  │  └──────┘ └──────┘ └──────┘         │    │    │
│  │  └──────────────────────────────────────┘    │    │
│  └─────────────────────────────────────────────┘    │
│                                                     │
│  ┌────────────────────┐  ┌────────────────────┐    │
│  │  连接池            │  │  内存池            │    │
│  │  ngx_connection_t  │  │  ngx_pool_t        │    │
│  │  × 1024 (预分配)   │  │  per-request       │    │
│  └────────────────────┘  └────────────────────┘    │
│                                                     │
│  文件描述符布局：                                    │
│  fd 0,1,2  → stdin/stdout/stderr                    │
│  fd 3~N    → 客户端连接（动态分配/回收）            │
│  fd 6      → listen socket (共享)                   │
│  fd 16     → epoll 实例 (epoll_create)              │
│  fd 15     → access.log (写端)                      │
└─────────────────────────────────────────────────────┘
```

### 1.3 master-worker 交互时序图

```
时间线 ──────────────────────────────────────────────────────────→

master              worker0           worker1
  │                    │                 │
  │── fork() ────────→ │                 │
  │── fork() ──────────────────────────→ │
  │                    │                 │
  │  sigsuspend()      │  epoll_wait()   │  epoll_wait()
  │  (等待信号)        │  (处理请求)     │  (处理请求)
  │                    │                 │
  │  ←── SIGHUP ──    │                 │
  │  (nginx -s reload) │                 │
  │                    │                 │
  │── fork() ──→ new_w0                │
  │── fork() ──────────────────→ new_w1
  │                    │                 │
  │── SIGQUIT ──→ old_w0               │
  │── SIGQUIT ─────────────────→ old_w1
  │                    │                 │
  │                    │ (完成当前请求)   │ (完成当前请求)
  │                    │ exit()          │ exit()
  │  ←── SIGCHLD ──   │                 │
  │  ←── SIGCHLD ────────────────────── │
  │                    │                 │
  │  sigsuspend()      new_w0            new_w1
  │  (继续等待)        epoll_wait()      epoll_wait()
```

---

## 二、事件驱动模型

### 2.1 事件模块架构

```
┌─────────────────────────────────────────────────────┐
│              ngx_event_module_t 接口                │
│  ─────────────────────────────────────              │
│  ngx_event_actions_t:                               │
│    add()          → epoll_ctl(EPOLL_CTL_ADD)        │
│    del()          → epoll_ctl(EPOLL_CTL_DEL)        │
│    process_events() → epoll_wait()                  │
│    init()         → epoll_create()                  │
│    done()         → close(epoll_fd)                 │
├─────────────────────────────────────────────────────┤
│                   实现层                            │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐            │
│  │  epoll   │ │  kqueue  │ │  select  │            │
│  │ (Linux)  │ │ (BSD/OSX)│ │ (通用)   │            │
│  │ ★ 默认   │ │          │ │          │            │
│  └──────────┘ └──────────┘ └──────────┘            │
└─────────────────────────────────────────────────────┘
```

### 2.2 epoll 实现详解

**源码位置：** `src/event/modules/ngx_epoll_module.c`

```c
/*
 * ★ epoll 事件处理核心函数
 */

static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    /* 1. 调用 epoll_wait */
    int events = epoll_wait(ep, event_list, (int) nevents, timer);
    /*
     * ep        → epoll 文件描述符
     * event_list → 就绪事件数组
     * nevents   → 最大事件数（= worker_connections）
     * timer     → 超时时间（ms），-1 表示无限等待
     */

    if (events == -1) {
        /* EINTR: 被信号中断，正常返回 */
        return NGX_OK;
    }

    /* 2. 遍历就绪事件 */
    for (i = 0; i < events; i++) {
        /* 从 epoll 的 data 字段恢复 ngx_connection_t */
        c = event_list[i].data.ptr;

        /* ── 读事件 ─────────────────────────── */
        if (event_list[i].events & EPOLLIN) {
            /* ★ 调用读事件 handler */
            c->read->handler(c->read);
            /*
             * 如果 c 是 listen socket → ngx_event_accept()
             * 如果 c 是 client socket → ngx_http_process_request_line()
             */
        }

        /* ── 写事件 ─────────────────────────── */
        if (event_list[i].events & EPOLLOUT) {
            /* ★ 调用写事件 handler */
            c->write->handler(c->write);
            /*
             * → ngx_http_writer() 发送响应
             */
        }

        /* ── 错误/关闭事件 ──────────────────── */
        if (event_list[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            /* 连接异常，关闭 */
            c->read->handler(c->read);
        }
    }

    return NGX_OK;
}
```

### 2.3 epoll 边缘触发 vs 水平触发

```
水平触发 (LT)：                      边缘触发 (ET, nginx 默认)：

epoll_wait 返回条件：                 epoll_wait 返回条件：
  fd 上有数据可读 → 返回                fd 状态发生变化 → 返回
  fd 上数据没读完 → 下次还返回          ★ 只在"新数据到达"时返回

┌──────────────┐                     ┌──────────────┐
│  fd 缓冲区   │                     │  fd 缓冲区   │
│  [data data] │                     │  [data data] │
└──────┬───────┘                     └──────┬───────┘
       │                                    │
  epoll_wait()                         epoll_wait()
  每次调用都返回 ←── ★ 频繁              只返回一次 ←── ★ 高效
  直到数据读完                          ★ 必须一次读完（循环 read 到 EAGAIN）

nginx 选择 ET 的原因：
1. 减少 epoll_wait 返回次数 → 降低系统调用开销
2. 避免惊群效应
3. 配合非阻塞 I/O，循环读到 EAGAIN 即可
```

### 2.4 accept 惊群问题的解决

```
惊群问题：多个 worker 监听同一个 listen socket
         → 新连接到达时，所有 worker 都被唤醒
         → 只有一个能 accept 成功，其余白跑一趟

解决方案演进：

  方案 1: accept_mutex (nginx < 1.11.3)
  ─────────────────────────────────────
  worker 通过共享内存锁竞争 accept 权利
  只有获得锁的 worker 才将 listen fd 加入 epoll
  ★ 缺点：锁竞争开销，负载不均

  方案 2: EPOLLEXCLUSIVE (nginx ≥ 1.11.3, Linux ≥ 4.5)
  ─────────────────────────────────────
  所有 worker 都将 listen fd 加入 epoll
  但使用 EPOLLEXCLUSIVE 标志
  ★ 内核保证只唤醒一个 worker → 无惊群

  方案 3: SO_REUSEPORT (Linux ≥ 3.9)
  ─────────────────────────────────────
  每个 worker 创建独立的 listen socket
  内核在多个 socket 之间负载均衡
  ★ 性能最好，但需要每个 worker 独立 bind
```

---

## 三、连接池与内存池

### 3.1 连接池（Connection Pool）

**源码位置：** `src/core/ngx_connection.c`

```
连接池数据结构：

┌──────────────────────────────────────────────────────────────┐
│  ngx_cycle->free_connections (空闲连接链表)                  │
│  ngx_cycle->free_connection_n = 1024 (空闲连接数)            │
│                                                              │
│  预分配的 ngx_connection_t 数组（worker_connections 个）：   │
│                                                              │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐               │
│  │ conn[0]   │  │ conn[1]   │  │ conn[2]   │  ...          │
│  │ fd=3      │  │ fd=-1     │  │ fd=8      │               │
│  │ data=     │  │ data=     │  │ data=     │               │
│  │ read_ev   │  │ read_ev   │  │ read_ev   │               │
│  │ write_ev  │  │ write_ev  │  │ write_ev  │               │
│  │ pool      │  │ pool      │  │ pool      │               │
│  │ next ──→  │  │ next ──→  │  │ next=NULL │               │
│  └───────────┘  └───────────┘  └───────────┘               │
│       ↑                                    ↑                │
│   正在使用                              空闲链表中           │
└──────────────────────────────────────────────────────────────┘
```

**关键函数：**

```c
/*
 * ★ 获取一个空闲连接 — O(1) 时间复杂度
 */
ngx_connection_t *
ngx_get_connection(ngx_socket_t s, ngx_log_t *log)
{
    /* 从空闲链表头取出一个连接 */
    ngx_connection_t *c = ngx_cycle->free_connections;

    if (c == NULL) {
        /* ★ 连接池耗尽！这是 worker_connections 不够用的根因 */
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "no free connections");
        return NULL;
    }

    /* 更新空闲链表 */
    ngx_cycle->free_connections = c->next;
    ngx_cycle->free_connection_n--;

    /* 初始化连接 */
    c->fd = s;              /* 绑定文件描述符 */
    c->read->active = 0;
    c->write->active = 0;

    /* 为连接创建内存池 */
    c->pool = ngx_create_pool(256, log);

    return c;
}

/*
 * ★ 归还连接到空闲链表 — O(1) 时间复杂度
 */
void
ngx_free_connection(ngx_connection_t *c)
{
    /* 插入空闲链表头 */
    c->next = ngx_cycle->free_connections;
    ngx_cycle->free_connections = c;
    ngx_cycle->free_connection_n++;

    /* 销毁连接内存池 */
    if (c->pool) {
        ngx_destroy_pool(c->pool);
        c->pool = NULL;
    }
}
```

### 3.2 内存池（Memory Pool）

**源码位置：** `src/core/ngx_palloc.c`

```
内存池结构：

ngx_pool_t (每个请求/连接独立创建)
┌───────────────────────────────────────────────────────┐
│  ngx_pool_t                                           │
│  ├── d.last       当前 block 的已分配位置             │
│  ├── d.end        当前 block 的结束位置               │
│  ├── d.next ─────→ block 1 ────→ block 2 ────→ ...   │
│  ├── d.failed     分配失败次数（决定用哪个 block）    │
│  │                                                     │
│  ├── max          小块/大块的分界线（通常 4095 字节） │
│  ├── current      从哪个 block 开始分配               │
│  ├── large ──────→ 大块 malloc 链表                   │
│  └── cleanup ────→ 清理回调链表                       │
└───────────────────────────────────────────────────────┘

分配策略：
  size ≤ max → 在当前 block 中分配（O(1)，移动指针）
               当前 block 空间不足 → 新建 block
  size > max → 直接 malloc，挂到 large 链表

释放策略：
  ★ 整个请求结束后，调用 ngx_destroy_pool() 一次性释放
  → 所有 block + 所有 large 分配 → 无内存泄漏，无碎片
```

**内存池 vs 传统 malloc/free 对比：**

| 维度 | 传统 malloc/free | nginx 内存池 |
|------|-----------------|-------------|
| 分配速度 | 较慢（查找空闲块） | ★ 极快（移动指针） |
| 释放速度 | 逐个 free | ★ 一次性释放整个池 |
| 内存碎片 | 容易产生 | ★ 无碎片 |
| 内存泄漏 | 容易遗漏 free | ★ 不可能泄漏（池销毁时全部释放） |
| 适用场景 | 长生命周期对象 | ★ 短生命周期（单次请求） |

---

## 四、HTTP 请求处理的 11 个 Phase

### 4.1 Phase 枚举定义

**源码位置：** `src/http/ngx_http_core_module.h`

```c
/*
 * ★ HTTP 请求处理的 11 个阶段
 *
 * 每个请求按顺序经过这些 phase
 * 某些 phase 是内部使用的，不能注册自定义 handler
 */
typedef enum {
    NGX_HTTP_POST_READ_PHASE = 0,       /* 读取请求后 */
    NGX_HTTP_SERVER_REWRITE_PHASE,      /* server 级 rewrite */
    NGX_HTTP_FIND_CONFIG_PHASE,         /* ★ location 匹配（内部） */
    NGX_HTTP_REWRITE_PHASE,             /* location 级 rewrite */
    NGX_HTTP_POST_REWRITE_PHASE,        /* rewrite 后跳转（内部） */
    NGX_HTTP_PREACCESS_PHASE,           /* 访问前检查（限流） */
    NGX_HTTP_ACCESS_PHASE,              /* ★ 访问控制 */
    NGX_HTTP_POST_ACCESS_PHASE,         /* satisfy 逻辑（内部） */
    NGX_HTTP_PRECONTENT_PHASE,          /* 内容前处理（try_files） */
    NGX_HTTP_CONTENT_PHASE,             /* ★ 内容生成 */
    NGX_HTTP_LOG_PHASE,                 /* 日志记录 */

    NGX_HTTP_LAST_PHASE                 /* 标记结束 */
} ngx_http_phases;
```

### 4.2 Phase Handler 注册与执行

```
注册流程（启动时）：

各模块 postconfiguration 回调
     │
     ▼
cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
ph = cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers.elts;
/* ★ 将自己的 handler push 到对应 phase 的 handlers 数组 */

执行流程（请求处理时）：

ngx_http_core_run_phases(r)
 │
 ├── phase = 0 (POST_READ)
 │     ├── handler[0](r) → NGX_DECLINED → 继续
 │     ├── handler[1](r) → NGX_DECLINED → 继续
 │     └── 无更多 handler → phase++
 │
 ├── phase = 1 (SERVER_REWRITE)
 │     └── rewrite handler → NGX_DECLINED → phase++
 │
 ├── phase = 2 (FIND_CONFIG)
 │     └── ★ ngx_http_core_find_config_phase(r)
 │         ├── URI 前缀匹配 → 找到最长匹配
 │         ├── 正则匹配 → 按配置顺序，首个匹配胜出
 │         └── 设置 r->loc_conf → phase++
 │
 ├── ... (省略中间 phase)
 │
 ├── phase = 9 (CONTENT)
 │     └── ★ 特殊：handler 互斥
 │         ├── proxy_pass handler? → 反向代理
 │         ├── static handler? → sendfile() 发送文件
 │         ├── fastcgi handler? → FastCGI 协议
 │         └── ★ 只执行一个 handler，返回 NGX_OK 则停止
 │
 └── phase = 10 (LOG)
       └── ngx_http_log_handler(r) → 写入 access.log
```

### 4.3 各 Phase 对应的常用模块

```
Phase # │ Phase 名称        │ 常用模块                     │ 配置指令
────────┼──────────────────┼──────────────────────────────┼────────────────────
  0     │ POST_READ         │ realip                       │ set_real_ip_from
        │                   │                              │ real_ip_header
────────┼──────────────────┼──────────────────────────────┼────────────────────
  1     │ SERVER_REWRITE    │ rewrite                      │ rewrite (server级)
────────┼──────────────────┼──────────────────────────────┼────────────────────
  2     │ FIND_CONFIG       │ ★ core (内部)               │ location {}
────────┼──────────────────┼──────────────────────────────┼────────────────────
  3     │ REWRITE           │ rewrite                      │ rewrite (location级)
        │                   │                              │ return
────────┼──────────────────┼──────────────────────────────┼────────────────────
  4     │ POST_REWRITE      │ ★ core (内部)               │ (自动跳转)
────────┼──────────────────┼──────────────────────────────┼────────────────────
  5     │ PREACCESS         │ limit_req                    │ limit_req_zone
        │                   │ limit_conn                   │ limit_conn_zone
────────┼──────────────────┼──────────────────────────────┼────────────────────
  6     │ ACCESS            │ access                       │ allow / deny
        │                   │ auth_basic                   │ auth_basic
        │                   │ auth_request                 │ auth_request
────────┼──────────────────┼──────────────────────────────┼────────────────────
  7     │ POST_ACCESS       │ ★ core (内部)               │ satisfy
────────┼──────────────────┼──────────────────────────────┼────────────────────
  8     │ PRECONTENT        │ try_files                    │ try_files
────────┼──────────────────┼──────────────────────────────┼────────────────────
  9     │ CONTENT           │ ★ static / proxy / fastcgi   │ root / proxy_pass
        │                   │ gzip / sub_filter            │ gzip / sub_filter
────────┼──────────────────┼──────────────────────────────┼────────────────────
 10     │ LOG               │ log                          │ access_log
```

### 4.4 请求处理的返回值语义

| 返回值 | 含义 | 框架行为 |
|--------|------|---------|
| `NGX_OK` (0) | 处理成功 | ★ 停止 phase 链，进入 finalize |
| `NGX_DECLINED` (-5) | 不处理/跳过 | 继续当前 phase 的下一个 handler |
| `NGX_AGAIN` (-2) | 等待事件 | 返回事件循环，等待 I/O 就绪后继续 |
| `NGX_DONE` (-4) | 处理完成 | 类似 NGX_OK，但需要额外清理 |
| `NGX_HTTP_*` (≥300) | HTTP 错误码 | 返回错误页面（301/302/403/404/500 等） |

---

## 五、请求处理完整生命周期

```
客户端发起 TCP 连接
     │
     ▼
┌──────────────────────────────────────────────────────────────────┐
│  阶段 1: 连接建立                                               │
│  ─────────────────────────────────────────────────────          │
│  listen socket epoll_wait() 返回 EPOLLIN                        │
│       │                                                         │
│       ▼                                                         │
│  ngx_event_accept()                                             │
│  ├── accept4(fd, SOCK_NONBLOCK)     ★ 非阻塞连接               │
│  ├── ngx_get_connection()             从连接池分配               │
│  ├── ngx_create_pool()               为连接创建内存池           │
│  └── epoll_ctl(EPOLL_CTL_ADD, c->fd, EPOLLIN|EPOLLET)           │
│      ★ 将新连接加入 epoll，边缘触发                             │
└──────────────────────────────────────────────────────────────────┘
     │
     ▼
┌──────────────────────────────────────────────────────────────────┐
│  阶段 2: 读取请求                                               │
│  ─────────────────────────────────────────────────────          │
│  client fd epoll_wait() 返回 EPOLLIN                            │
│       │                                                         │
│       ▼                                                         │
│  ngx_http_process_request_line()                                │
│  ├── read(c->fd, buf, size)           读取数据                  │
│  ├── ngx_http_parse_request_line()    解析请求行                │
│  │     "GET /index.html HTTP/1.1\r\n"                           │
│  │      ^^^ ^^^^^^^^^^^ ^^^^^^^^                                 │
│  │      方法 URI        版本                                    │
│  └── ngx_http_process_request_headers() 解析请求头              │
│        Host: example.com                                        │
│        Connection: keep-alive                                   │
│        Accept: text/html                                        │
│        ...                                                      │
└──────────────────────────────────────────────────────────────────┘
     │
     ▼
┌──────────────────────────────────────────────────────────────────┐
│  阶段 3: Phase 流水线处理                                       │
│  ─────────────────────────────────────────────────────          │
│  ngx_http_core_run_phases(r)                                    │
│  │                                                              │
│  ├── POST_READ    → realip 还原真实 IP                         │
│  ├── SERVER_REWRITE → server 级 rewrite                        │
│  ├── FIND_CONFIG  → ★ location 匹配                           │
│  │     匹配 /index.html → 找到 location / {}                   │
│  ├── REWRITE      → location 级 rewrite/return                 │
│  ├── PREACCESS    → limit_req/limit_conn 检查                  │
│  ├── ACCESS       → allow/deny, auth_basic 检查                │
│  ├── PRECONTENT   → try_files $uri $uri/ /index.html           │
│  └── CONTENT      → ★ 发送文件                                 │
│        ngx_http_static_handler()                                │
│        ├── open("/usr/share/nginx/html/index.html")             │
│        ├── 设置 Content-Type, Content-Length                    │
│        └── ngx_http_send_header() + ngx_http_output_filter()   │
└──────────────────────────────────────────────────────────────────┘
     │
     ▼
┌──────────────────────────────────────────────────────────────────┐
│  阶段 4: 发送响应                                               │
│  ─────────────────────────────────────────────────────          │
│  ngx_http_output_filter()                                       │
│  ├── headers filter chain → 修改/添加响应头                    │
│  ├── body filter chain → gzip 压缩等                           │
│  └── write filter → ★ 实际发送                                 │
│        ├── writev(c->fd, header_iovec)   发送响应头             │
│        └── sendfile(c->fd, file_fd, offset, size) 发送文件体   │
│            ★ 零拷贝：数据从磁盘直接到网卡，不经用户态           │
└──────────────────────────────────────────────────────────────────┘
     │
     ▼
┌──────────────────────────────────────────────────────────────────┐
│  阶段 5: 日志与清理                                             │
│  ─────────────────────────────────────────────────────          │
│  NGX_HTTP_LOG_PHASE                                             │
│  ├── ngx_http_log_handler()                                     │
│  │     写入 access.log:                                         │
│  │     "127.0.0.1 - - [03/Jul/2026:23:24:35 +0800]              │
│  │      "GET / HTTP/1.1" 200 457 "-" "curl/8.4.0" "-""          │
│  │                                                              │
│  ├── keep-alive 判断                                            │
│  │     ├── Connection: keep-alive → 保持连接，回到 epoll_wait   │
│  │     └── Connection: close → 关闭连接                        │
│  │                                                              │
│  └── 清理                                                       │
│        ├── ngx_http_free_request()  释放请求内存池              │
│        └── ngx_free_connection()    归还连接到连接池            │
└──────────────────────────────────────────────────────────────────┘
```

---

## ★ 总结：nginx 架构核心映射表

```
概念                    │ 源码位置                           │ 关键函数/结构
────────────────────────┼───────────────────────────────────┼────────────────────────
master 进程循环         │ ngx_process_cycle.c               │ ngx_master_process_cycle()
worker 进程循环         │ ngx_process_cycle.c               │ ngx_worker_process_cycle()
事件循环核心            │ ngx_event.c                       │ ngx_process_events_and_timers()
epoll 事件处理          │ ngx_epoll_module.c                │ ngx_epoll_process_events()
accept 处理             │ ngx_event_accept.c                │ ngx_event_accept()
连接池分配/回收         │ ngx_connection.c                  │ ngx_get_connection()/ngx_free_connection()
内存池分配/释放         │ ngx_palloc.c                      │ ngx_palloc()/ngx_destroy_pool()
HTTP 请求处理入口       │ ngx_http_request.c                │ ngx_http_process_request()
Phase handler 执行      │ ngx_http_core_module.c            │ ngx_http_core_run_phases()
HTTP 协议解析           │ ngx_http_parse.c                  │ ngx_http_parse_request_line()
输出过滤链              │ ngx_http_write_filter_module.c    │ ngx_http_write_filter()
日志记录                │ ngx_http_log_module.c             │ ngx_http_log_handler()
```

### 一句话记住每个核心组件

| 组件 | 一句话 |
|------|--------|
| **master** | 只管进程生死，不碰请求 |
| **worker** | 单线程事件循环，一人扛万连接 |
| **epoll** | 高效多路复用，边缘触发减少唤醒 |
| **连接池** | 预分配 + 链表，O(1) 分配回收 |
| **内存池** | 请求级内存管理，一次性释放无泄漏 |
| **phase** | 11 个阶段流水线，模块化处理请求 |
| **sendfile** | 零拷贝传输，减少 CPU 和内存开销 |
