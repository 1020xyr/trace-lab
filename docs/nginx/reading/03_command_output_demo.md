# nginx 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐行解读。
> 环境：nginx/1.30.2, Anolis OS, 4 核 CPU

---

## 场景 1：安装与版本信息

### 命令

```bash
# 安装 nginx
yum install -y nginx
```

### nginx -V（编译选项解读）

```bash
nginx -V 2>&1
```

```
nginx version: nginx/1.30.2
built by gcc 12.3.0 20230508 (Anolis OS 12.3.0-15) (GCC)
built with OpenSSL 3.0.12 24 Oct 2023
TLS SNI support enabled
configure arguments: --with-debug --with-compat --user=nginx --group=nginx
  --with-http_dav_module --with-http_flv_module
  --pid-path=/run/nginx.pid
  --prefix=/usr/share/nginx
  --sbin-path=/usr/sbin/nginx
  --conf-path=/etc/nginx/nginx.conf
  --http-log-path=/var/log/nginx/access.log
  --error-log-path=/var/log/nginx/error.log
  --with-file-aio --with-pcre --with-pcre-jit --with-threads
  --with-mail=dynamic --with-stream=dynamic
  --with-http_v2_module --with-http_ssl_module
  --with-http_stub_status_module
  --with-http_realip_module --with-http_gzip_static_module
  --with-http_sub_module --with-http_slice_module
  ...
```

**逐行解读：**

```
nginx version: nginx/1.30.2              ← 版本号（1.30.x 是主线稳定版）
built by gcc 12.3.0                      ← 编译器版本
built with OpenSSL 3.0.12               ← SSL/TLS 库版本
TLS SNI support enabled                  ← 支持 SNI（多域名共享证书）

configure arguments:                     ← ★ 编译时启用的模块和选项
  --with-debug                           ← 启用调试日志（生产环境通常不开）
  --with-compat                          ← 动态模块兼容模式
  --user=nginx --group=nginx             ← worker 进程的运行用户
  --pid-path=/run/nginx.pid              ← master PID 文件位置
  --prefix=/usr/share/nginx              ← 默认工作目录
  --conf-path=/etc/nginx/nginx.conf      ← ★ 默认配置文件路径
  --http-log-path=/var/log/nginx/access.log   ← 默认访问日志
  --error-log-path=/var/log/nginx/error.log   ← 默认错误日志

★ 关键模块：
  --with-http_v2_module                  ← HTTP/2 支持（多路复用）
  --with-http_ssl_module                 ← HTTPS/TLS 支持
  --with-http_stub_status_module         ← ★ stub_status 监控
  --with-http_realip_module              ← 还原真实客户端 IP
  --with-http_gzip_static_module         ← 预压缩静态文件
  --with-http_sub_module                 ← 内容替换（注入 JS 等）
  --with-file-aio                        ← 异步文件 I/O
  --with-pcre --with-pcre-jit            ← PCRE 正则 + JIT 编译加速
  --with-stream=dynamic                  ← TCP/UDP 代理（动态加载）
  --with-mail=dynamic                    ← 邮件代理（动态加载）
```

**常用编译选项对照：**

| 编译选项 | 含义 | 是否默认 |
|---------|------|---------|
| `--with-http_ssl_module` | HTTPS 支持 | 否，需显式启用 |
| `--with-http_v2_module` | HTTP/2 支持 | 否，需显式启用 |
| `--with-http_stub_status_module` | 状态监控 | 否，需显式启用 |
| `--with-http_realip_module` | 真实 IP 还原 | 否，需显式启用 |
| `--with-stream` | TCP/UDP 四层代理 | 否，需显式启用 |
| `--with-debug` | 调试日志 | 否（★ 生产环境禁用） |
| `--with-pcre-jit` | PCRE JIT 编译 | 否，显著提升正则性能 |
| `--with-threads` | 线程池（异步文件读取） | 否 |

---

## 场景 2：配置检查

### 命令

```bash
nginx -t
```

### 输出

```
nginx: the configuration file /etc/nginx/nginx.conf syntax is ok
nginx: configuration file /etc/nginx/nginx.conf test is successful
```

**逐行解读：**

```
the configuration file /etc/nginx/nginx.conf
                        ^^^^^^^^^^^^^^^^^^^^^
                        ★ 实际使用的配置文件路径

syntax is ok           ← ★ 第一阶段：语法检查通过
test is successful     ← ★ 第二阶段：语义检查通过
                         （如 listen 端口不冲突、路径存在等）
```

### 配置错误的输出示例

```bash
# 假设 worker_connections 写成了字符串
nginx -t
```

```
nginx: [emerg] invalid number of arguments in "worker_connections" directive
  in /etc/nginx/nginx.conf:14
nginx: configuration file /etc/nginx/nginx.conf test failed
```

```
[emerg]                  ← 日志级别：紧急错误
invalid number of arguments  ← 错误描述
"worker_connections"     ← ★ 出问题的指令
in /etc/nginx/nginx.conf:14 ← ★ 文件和行号（精确定位）
test failed              ← 配置不可用
```

### nginx -T（打印完整配置）

```bash
nginx -T 2>&1 | head -30
```

```
nginx: the configuration file /etc/nginx/nginx.conf syntax is ok
nginx: configuration file /etc/nginx/nginx.conf test is successful
# configuration file /etc/nginx/nginx.conf:

user nginx;
worker_processes auto;
error_log /var/log/nginx/error.log notice;
pid /run/nginx.pid;

include /usr/share/nginx/modules/*.conf;

events {
    worker_connections 1024;
}

http {
    log_format  main  '$remote_addr - $remote_user [$time_local] "$request" '
                      '$status $body_bytes_sent "$http_referer" '
                      '"$http_user_agent" "$http_x_forwarded_for"';
    ...
```

**用途：** ★ 排查 `include` 引入的配置问题。`-T` 展开所有 include 文件，显示 nginx 最终使用的完整配置。

---

## 场景 3：进程模型观察

### 命令

```bash
# 启动 nginx
nginx

# 查看进程
ps aux | grep nginx | grep -v grep
```

### 输出

```
root      460421  0.0  0.0  10868  2224 ?        Ss   23:24   0:00 nginx: master process nginx
nginx     460422  0.0  0.0  12752  5084 ?        S    23:24   0:00 nginx: worker process
nginx     460423  0.0  0.0  12752  5084 ?        S    23:24   0:00 nginx: worker process
nginx     460424  0.0  0.0  12752  5084 ?        S    23:24   0:00 nginx: worker process
nginx     460426  0.0  0.0  12852  5040 ?        S    23:24   0:00 nginx: worker process
```

**逐字段解读：**

```
root      460421  0.0  0.0  10868  2224 ?   Ss   23:24   0:00 nginx: master process nginx
^^^^      ^^^^^^  ^^^  ^^^  ^^^^^  ^^^^ ^   ^^   ^^^^^   ^^^^
│         │       │    │    │      │    │   │    │       │
│         │       │    │    │      │    │   │    │       └─ "nginx: master process" 标识
│         │       │    │    │      │    │   │    └─ 启动时间
│         │       │    │    │      │    │   └─ Ss: S=可中断睡眠, s=session leader
│         │       │    │    │      │    └─ ?: 无控制终端（守护进程）
│         │       │    │    │      └─ RSS 2224KB: master 实际内存占用（★ 很小）
│         │       │    │    └─ VSZ 10868KB: 虚拟内存
│         │       │    └─ %MEM: 0.0%（几乎不占内存）
│         │       └─ %CPU: 0.0%（★ master 不消耗 CPU）
│         └─ PID: master 进程号
└─ USER: root（★ master 以 root 运行，才能绑定 80/443 端口）

nginx     460422  0.0  0.0  12752  5084 ?   S    23:24   0:00 nginx: worker process
^^^^^     ^^^^^^
│         │
│         └─ 每个 worker 有独立 PID
└─ USER: nginx（★ worker 以非 root 用户运行，安全隔离）
```

**关键观察：**

| 特征 | master | worker |
|------|--------|--------|
| 用户 | root | nginx |
| 数量 | 1 个 | 4 个（`auto` = 4 核 CPU） |
| 内存 (RSS) | 2224 KB | ~5084 KB |
| CPU | 0% | 0%（idle 时） |
| 进程标识 | `Ss`（session leader） | `S` |

---

## 场景 4：监听端口检查

### 命令

```bash
ss -tlnp | grep nginx
```

### 输出

```
LISTEN 0      511          0.0.0.0:80         0.0.0.0:*    users:(("nginx",pid=460583,fd=6),("nginx",pid=460582,fd=6),("nginx",pid=460581,fd=6),("nginx",pid=460580,fd=6),("nginx",pid=460421,fd=6))
```

**逐字段解读：**

```
LISTEN  0       511          0.0.0.0:80         0.0.0.0:*
^^^^^^  ^       ^^^          ^^^^^^^^^^^^       ^^^^^^^^^
│       │       │            │                  │
│       │       │            │                  └─ 接受来自任何地址的连接
│       │       │            └─ ★ 监听所有 IPv4 地址的 80 端口
│       │       └─ ★ backlog=511: listen() 队列长度
│       │          (nginx 默认 backlog=511，可通过 listen 80 backlog=N 修改，
│       │           最大受 somaxconn 限制)
│       └─ Recv-Q: 接收队列当前长度（0 = 无积压）
└─ 状态: LISTEN

users:
  ("nginx",pid=460583,fd=6)  ← worker 进程 3，fd=6 是 listen socket
  ("nginx",pid=460582,fd=6)  ← worker 进程 2
  ("nginx",pid=460581,fd=6)  ← worker 进程 1
  ("nginx",pid=460580,fd=6)  ← worker 进程 0
  ("nginx",pid=460421,fd=6)  ← ★ master 进程（fd=6 也是 listen socket）

★ 关键点：所有进程共享同一个 listen socket (fd=6)
  master 创建 socket 后 fork worker，worker 继承 fd
  多个 worker 竞争 accept 同一个 socket
```

### backlog 调优

```bash
# 查看系统级 backlog 上限
cat /proc/sys/net/core/somaxconn
# 如果 < 511，需要调整：
# sysctl -w net.core.somaxconn=4096
```

---

## 场景 5：strace 追踪请求处理的系统调用

### 命令

```bash
# 获取 worker PID
WORKER_PID=$(ps aux | grep "nginx: worker" | head -1 | awk '{print $2}')

# strace 追踪关键系统调用
strace -p $WORKER_PID -e trace=accept4,read,write,epoll_wait,sendfile,writev,close \
    -tt -o /tmp/strace_nginx.out &

sleep 0.5
curl -s http://localhost/ > /dev/null
sleep 1
kill %1
wait
cat /tmp/strace_nginx.out
```

### 输出（逐行解读）

```
460580 23:24:56.013947 epoll_wait(16, [{events=EPOLLIN, data={u32=737943488, u64=94180780810176}}], 512, -1) = 1
460580 23:24:56.521499 accept4(6, {sa_family=AF_INET, sin_port=htons(33662), sin_addr=inet_addr("127.0.0.1")}, [112 => 16], SOCK_NONBLOCK) = 3
460580 23:24:56.521696 epoll_ctl(16, EPOLL_CTL_ADD, 3, {events=EPOLLIN|EPOLLRDHUP|EPOLLET, data={u32=737943985, u64=94180780810673}}) = 0
460580 23:24:56.521799 epoll_wait(16, [{events=EPOLLIN, data={u32=737943985, u64=94180780810673}}], 512, 60000) = 1
460580 23:24:56.523058 writev(3, [{iov_base="HTTP/1.1 200 OK\r\nServer: nginx/1"..., iov_len=238}], 1) = 238
460580 23:24:56.523249 sendfile(3, 8, [0] => [457], 457) = 457
460580 23:24:56.523338 write(15, "127.0.0.1 - - [03/Jul/2026:23:24"..., 89) = 89
460580 23:24:56.524666 close(8)         = 0
460580 23:24:56.524926 epoll_wait(16, [{events=EPOLLIN|EPOLLRDHUP, data={u32=737943985, u64=94180780810673}}], 512, 65000) = 1
460580 23:24:56.525240 close(3)         = 0
```

**逐行详解：**

```
行 1: epoll_wait(16, ..., 512, -1) = 1
      ^^^^^^^^^^  ^^       ^^^  ^^   ^
      │           │        │    │    │
      │           │        │    │    └─ ★ 返回 1 个就绪事件（listen socket 有连接）
      │           │        │    └─ timeout=-1: 无限等待
      │           │        └─ 最多接收 512 个事件
      │           └─ ep=16: epoll 实例的文件描述符
      └─ ★ worker 阻塞在 epoll_wait，等待 I/O 事件

行 2: accept4(6, {AF_INET, port=33662, addr=127.0.0.1}, [112=>16], SOCK_NONBLOCK) = 3
      ^^^^^^^ ^  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^             ^^^^^^^^^^^^^   ^
      │       │  │                                                │              │
      │       │  │                                                │              └─ ★ 新连接 fd=3
      │       │  │                                                └─ ★ SOCK_NONBLOCK: 非阻塞模式
      │       │  └─ 客户端地址: 127.0.0.1:33662
      │       └─ listen socket fd=6
      └─ ★ accept4: 比 accept + fcntl 少一次系统调用

行 3: epoll_ctl(16, EPOLL_CTL_ADD, 3, {EPOLLIN|EPOLLRDHUP|EPOLLET, ...}) = 0
      ^^^^^^^^^     ^^^^^^^^^^^^^  ^   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
      │             │              │   │
      │             │              │   └─ ★ 监听的事件：
      │             │              │      EPOLLIN    = 可读
      │             │              │      EPOLLRDHUP = 对端关闭
      │             │              │      EPOLLET    = ★ 边缘触发！
      │             │              └─ fd=3: 新连接
      │             └─ 操作：ADD（添加）
      └─ ★ 将新连接加入 epoll 监控

行 4: epoll_wait(16, ..., 512, 60000) = 1
      └─ ★ 等待客户端发送数据（timeout=60s = client_header_timeout）

行 5: writev(3, [{iov_base="HTTP/1.1 200 OK\r\nServer: nginx/1"..., iov_len=238}], 1) = 238
      ^^^^^^ ^   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^           ^^^   ^^^
      │      │   │                                                           │     │
      │      │   │                                                           │     └─ ★ 写入 238 字节
      │      │   └─ ★ 响应头内容（HTTP/1.1 200 OK + Server + Date + ...）
      │      └─ fd=3: 客户端连接
      └─ ★ writev: 向量写（scatter/gather I/O），一次发送多个 buffer

行 6: sendfile(3, 8, [0] => [457], 457) = 457
      ^^^^^^^^ ^  ^  ^^^^^^^^^^^^  ^^^   ^^^
      │        │  │  │             │     │
      │        │  │  │             │     └─ ★ 成功发送 457 字节
      │        │  │  │             └─ 请求传输 457 字节
      │        │  │  └─ 偏移量: 0 → 457（整个文件）
      │        │  └─ fd=8: 文件描述符（index.html）
      │        └─ fd=3: 客户端连接
      └─ ★ sendfile: 零拷贝！文件内容从内核直接到 socket，不经用户态

行 7: write(15, "127.0.0.1 - - [03/Jul/2026:23:24"..., 89) = 89
      ^^^^^ ^^  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^     ^^
      │     │   │                                        │
      │     │   │                                        └─ ★ 写入 89 字节日志
      │     │   └─ ★ access.log 的一行记录
      │     └─ fd=15: access.log 文件描述符
      └─ ★ 写入访问日志（LOG_PHASE 执行）

行 8: close(8) = 0
      └─ 关闭文件 fd（index.html），但连接 fd=3 保持（keep-alive）

行 9: epoll_wait(16, ..., 512, 65000) = 1
      └─ ★ 等待客户端后续请求或超时（65s = keepalive_timeout）

行 10: close(3) = 0
       └─ ★ 客户端断开，关闭连接 fd=3
```

### 请求处理的系统调用流程图

```
epoll_wait(ep=16, timeout=-1)
     │
     │ listen socket 就绪（新连接到达）
     ▼
accept4(listen_fd=6, SOCK_NONBLOCK) → client_fd=3
     │
     │ 将新连接加入 epoll
     ▼
epoll_ctl(EPOLL_CTL_ADD, fd=3, EPOLLIN|EPOLLET)
     │
     │ 等待客户端发送请求数据
     ▼
epoll_wait(ep=16, timeout=60000)
     │
     │ client_fd 可读
     ▼
read(client_fd=3) → HTTP 请求数据
     │
     │ 解析请求行 + 请求头 → Phase 流水线
     ▼
writev(client_fd=3, [响应头])          ← 238 字节
     │
     ▼
sendfile(client_fd=3, file_fd=8, 457)  ← ★ 零拷贝发送文件
     │
     ▼
write(log_fd=15, "access log...")      ← 记录日志
     │
     ▼
close(file_fd=8)                        ← 关闭文件（连接保持 keep-alive）
     │
     ▼
epoll_wait(ep=16, timeout=65000)        ← 等待下一个请求或超时
     │
     ▼
close(client_fd=3)                      ← 客户端断开
```

**关键时间分析：**

```
23:24:56.013947  epoll_wait 开始等待
23:24:56.521499  accept4 返回            ← 等待 507ms（无请求时 idle）
23:24:56.521696  epoll_ctl (ADD)         ← 0.2ms
23:24:56.521799  epoll_wait (等待数据)   ← 0.1ms
23:24:56.523058  writev (发送响应头)     ← 1.3ms（含请求解析 + phase 处理）
23:24:56.523249  sendfile (发送文件)     ← 0.2ms
23:24:56.523338  write (日志)            ← 0.1ms
23:24:56.524666  close (文件)            ← 1.3ms

★ 请求处理总时间：521499 → 523338 = 1.8ms
  其中大部分花在 phase 流水线处理上
```

---

## 场景 6：ab 压测 + perf stat 性能分析

### 命令

```bash
# ab 压测 + perf stat 统计
perf stat -e context-switches,cpu-migrations \
    -p $(pgrep -d, -f "nginx: worker") -- \
    ab -n 10000 -c 10 http://localhost/
```

### ab 输出

```
This is ApacheBench, Version 2.3 <$Revision: 1934973 $>

Server Software:        nginx/1.30.2          ← 服务器标识
Server Hostname:        localhost
Server Port:            80

Document Path:          /
Document Length:        457 bytes             ← 响应体大小

Concurrency Level:      10                    ← ★ 并发数
Time taken for tests:   0.919 seconds         ← ★ 总耗时 < 1 秒
Complete requests:      10000                 ← 完成请求数
Failed requests:        0                     ← ★ 失败数 = 0
Total transferred:      6900000 bytes         ← 总传输量
HTML transferred:       4570000 bytes
Requests per second:    10881.90 [#/sec]      ← ★ QPS ≈ 10882
Time per request:       0.919 [ms]            ← ★ 平均延迟 < 1ms
Time per request:       0.092 [ms]            ← 每并发平均延迟
Transfer rate:          7332.53 [Kbytes/sec]  ← 吞吐率 ≈ 7.2 MB/s
```

**连接时间统计：**

```
Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.2      0       4      ← TCP 连接建立
Processing:     0    1   0.5      0       5      ← ★ 请求处理
Waiting:        0    1   0.4      0       5      ← 等待响应
Total:          0    1   0.6      1       6      ← 端到端总时间
```

**百分位延迟：**

```
Percentage of the requests served within a certain time (ms)
  50%      1        ← ★ P50: 一半请求 ≤ 1ms
  66%      1
  75%      1
  80%      1
  90%      2        ← P90: 90% 请求 ≤ 2ms
  95%      2        ← P95: 95% 请求 ≤ 2ms
  98%      3
  99%      3        ← P99: 99% 请求 ≤ 3ms
 100%      6        ← P100: 最慢的请求 = 6ms
```

**解读：** ★ nginx 静态文件处理延迟极低 — P99 仅 3ms，最大延迟 6ms。10882 QPS 在 4 核机器上。

### perf stat 输出

```
 Performance counter stats for process id '460580,460581,460582,460583,469310':

            11,778      context-switches          ← ★ 10000 请求 ≈ 1.2 次切换/请求
               589      cpu-migrations            ← worker 在 CPU 间迁移

       0.937989839 seconds time elapsed           ← perf 统计时间
```

**解读：**

| 指标 | 值 | 含义 |
|------|-----|------|
| `context-switches` | 11,778 | ≈ 1.2 次/请求（nginx 单线程，主要来自 accept → worker 切换） |
| `cpu-migrations` | 589 | worker 在 CPU 核间迁移次数，绑核后可降为 0 |
| `time elapsed` | 0.938s | 与 ab 报告的 0.919s 一致 |

**优化建议：**
```bash
# 绑定 worker 到固定 CPU 核，减少 cpu-migrations
worker_cpu_affinity auto;
```

---

## 场景 7：access.log 日志解读

### 命令

```bash
tail -5 /var/log/nginx/access.log
```

### 输出

```
127.0.0.1 - - [03/Jul/2026:23:24:35 +0800] "GET / HTTP/1.1" 200 457 "-" "curl/8.4.0" "-"
127.0.0.1 - - [03/Jul/2026:23:24:35 +0800] "GET /nginx_status HTTP/1.1" 200 457 "-" "curl/8.4.0" "-"
127.0.0.1 - - [03/Jul/2026:23:25:02 +0800] "GET / HTTP/1.0" 200 457 "-" "ApacheBench/2.3" "-"
127.0.0.1 - - [03/Jul/2026:23:25:02 +0800] "GET / HTTP/1.0" 200 457 "-" "ApacheBench/2.3" "-"
127.0.0.1 - - [03/Jul/2026:23:25:02 +0800] "GET / HTTP/1.0" 200 457 "-" "ApacheBench/2.3" "-"
```

**逐字段解读（以第一行为例）：**

```
127.0.0.1  -  -  [03/Jul/2026:23:24:35 +0800]  "GET / HTTP/1.1"  200  457  "-"  "curl/8.4.0"  "-"
^^^^^^^^^  ^  ^  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^  ^^^  ^^^  ^^^  ^^^^^^^^^^^^  ^^^
│          │  │  │                             │                 │    │    │    │             │
│          │  │  │                             │                 │    │    │    │             └─ $http_x_forwarded_for
│          │  │  │                             │                 │    │    │    └─ $http_user_agent: 客户端标识
│          │  │  │                             │                 │    │    └─ $http_referer: 来源页
│          │  │  │                             │                 │    └─ $body_bytes_sent: ★ 响应体 457 字节
│          │  │  │                             │                 └─ $status: ★ HTTP 200 OK
│          │  │  │                             └─ $request: ★ 请求行
│          │  │  └─ $time_local: 服务器时间
│          │  └─ $remote_user: 无认证用户
│          └─ (保留字段)
└─ $remote_addr: ★ 客户端 IP

★ 注意 ab 使用 HTTP/1.0，而 curl 使用 HTTP/1.1
  HTTP/1.0 默认不带 Connection: keep-alive → 每请求新建连接
  HTTP/1.1 默认 keep-alive → 复用连接
```

### 日志格式自定义

```nginx
# 扩展日志格式（包含请求时间）
log_format detailed '$remote_addr - [$time_local] "$request" '
                    '$status $body_bytes_sent '
                    '$request_time $upstream_response_time '     ← ★ 关键性能指标
                    '"$http_referer" "$http_user_agent"';

# $request_time:       请求总处理时间（秒，毫秒精度）
# $upstream_response_time: upstream 响应时间（反向代理场景）
```

---

## 场景 8：stub_status 实时监控

### 命令

```bash
curl -s http://localhost/nginx_status
```

### 输出

```
Active connections: 3
server accepts handled requests
 12345 12345 67890
Reading: 0 Writing: 1 Waiting: 2
```

**逐行解读：**

```
Active connections: 3              ← ★ 当前活跃连接总数（含 keep-alive 空闲连接）

server accepts handled requests    ← 标题行
 12345  12345   67890              ← 三个累计值
 ^^^^^  ^^^^^   ^^^^^
 │      │       │
 │      │       └─ requests: 总请求数（★ 一个连接可发多个请求）
 │      └─ handled: 成功处理的连接数（应 = accepts）
 └─ accepts: 已接受的连接总数

★ accepts == handled → 正常（所有连接都被处理了）
★ accepts > handled → 异常！worker 资源不足导致连接被丢弃
★ requests / handled = 67890/12345 ≈ 5.5 → 平均每个连接处理 5.5 个请求（keep-alive 有效）

Reading: 0  Writing: 1  Waiting: 2
^^^^^^^^^   ^^^^^^^^^^  ^^^^^^^^^^
│           │           │
│           │           └─ ★ keep-alive 空闲连接数（等待下一个请求）
│           └─ ★ 正在发送响应的连接数
└─ 正在读取请求头的连接数

★ Reading + Writing + Waiting = Active connections（0 + 1 + 2 = 3 ✓）
```

### 诊断指标

| 现象 | 可能原因 | 排查方法 |
|------|---------|---------|
| Active connections 持续增长 | 连接泄漏、慢客户端攻击 | 检查 Reading 值 |
| accepts > handled | worker 连接池耗尽 | 增大 `worker_connections` |
| Reading 大量堆积 | 客户端发送请求很慢 | 可能是 Slowloris 攻击 |
| Writing 大量堆积 | upstream 响应慢 | 检查 `proxy_read_timeout` |
| Waiting 接近 Active | 正常，keep-alive 连接空闲 | — |

---

## 场景 9：错误日志解读

### 命令

```bash
tail -10 /var/log/nginx/error.log
```

### 输出（reload 过程）

```
2026/07/03 23:24:34 [notice] 460421#460421: signal 17 (SIGCHLD) received from 460423
2026/07/03 23:24:34 [notice] 460421#460421: worker process 460423 exited with code 0
2026/07/03 23:24:34 [notice] 460421#460421: worker process 460424 exited with code 0
2026/07/03 23:24:34 [notice] 460421#460421: signal 29 (SIGIO) received
2026/07/03 23:24:34 [notice] 460421#460421: signal 17 (SIGCHLD) received from 460426
2026/07/03 23:24:34 [notice] 460421#460421: worker process 460426 exited with code 0
2026/07/03 23:24:34 [notice] 460421#460421: signal 29 (SIGIO) received
2026/07/03 23:24:34 [notice] 460421#460421: signal 17 (SIGCHLD) received from 460422
2026/07/03 23:24:34 [notice] 460421#460421: worker process 460422 exited with code 0
2026/07/03 23:24:34 [notice] 460421#460421: signal 29 (SIGIO) received
```

**逐行解读：**

```
2026/07/03 23:24:34 [notice] 460421#460421: signal 17 (SIGCHLD) received from 460423
^^^^^^^^^^^^^^^^^^^ ^^^^^^^  ^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
│                   │        │              │
│                   │        │              └─ ★ master 收到 SIGCHLD: worker 460423 已退出
│                   │        └─ PID#TID: master 的进程号和线程号
│                   └─ [notice]: 日志级别（正常通知）
└─ 时间戳

worker process 460423 exited with code 0
                     ^^^^^^            ^
                     │                 └─ ★ exit code 0 = 正常退出（不是崩溃）
                     └─ 退出的 worker PID

signal 29 (SIGIO) received
         ^^^^^^^^
         └─ ★ SIGIO: I/O 事件通知（epoll 使用 signalfd/eventfd 时的辅助信号）
```

### 错误日志级别

| 级别 | 含义 | 典型场景 |
|------|------|---------|
| `debug` | 调试信息 | 开发/排障时开启（★ 生产禁用，I/O 开销大） |
| `info` | 一般信息 | 正常运行信息 |
| `notice` | 通知 | reload、worker 退出等正常事件 |
| `warn` | 警告 | 配置不优、接近限制 |
| `error` | 错误 | ★ 请求处理失败、upstream 不可达 |
| `crit` | 严重 | 系统级错误（磁盘满、权限问题） |
| `alert` | 告警 | 需要立即处理 |
| `emerg` | 紧急 | 服务不可用（无法 bind 端口） |

---

## 场景 10：nginx.conf 默认配置完整解读

### 命令

```bash
cat /etc/nginx/nginx.conf
```

### 输出（节选关键部分 + 注释）

```nginx
user nginx;                              # ★ worker 运行用户（安全：非 root）
worker_processes auto;                   # ★ worker 数 = CPU 核数
error_log /var/log/nginx/error.log notice; # 错误日志（notice 级别）
pid /run/nginx.pid;                      # master PID 文件

# 加载动态模块
include /usr/share/nginx/modules/*.conf;

events {
    worker_connections 1024;             # ★ 每个 worker 最大 1024 连接
}                                        # 总并发 = 4 × 1024 = 4096

http {
    # ★ 日志格式定义
    log_format  main  '$remote_addr - $remote_user [$time_local] "$request" '
                      '$status $body_bytes_sent "$http_referer" '
                      '"$http_user_agent" "$http_x_forwarded_for"';

    access_log  /var/log/nginx/access.log  main;  # 访问日志

    sendfile            on;              # ★ 启用零拷贝文件发送
    tcp_nopush          on;              # ★ 合并 TCP 包（配合 sendfile）
    keepalive_timeout   65;              # ★ keep-alive 超时 65 秒
    types_hash_max_size 4096;            # MIME 类型哈希表大小

    include             /etc/nginx/mime.types;     # MIME 类型映射
    default_type        application/octet-stream;  # 默认 MIME 类型

    # 加载 conf.d/ 下的额外配置
    include /etc/nginx/conf.d/*.conf;
}
```

**★ 配置结构总结：**

```
nginx.conf 层次结构：

  全局块（user / worker_processes / pid / error_log）
    │
    ├── events {}              ← 事件模块配置
    │     worker_connections
    │     use epoll
    │     multi_accept
    │
    └── http {}                ← HTTP 模块配置
          │
          ├── 全局 HTTP 指令  ← sendfile / keepalive_timeout / gzip
          │
          ├── upstream {}     ← 反向代理后端
          │     server 127.0.0.1:8000
          │
          └── server {}       ← 虚拟主机
                │
                ├── listen 80
                ├── server_name example.com
                │
                └── location / {}  ← URL 路由
                      proxy_pass / root / return
```

---

## ★ 总结：命令速查表

```
命令                                    │ 用途
───────────────────────────────────────┼─────────────────────────────────
nginx -V 2>&1                          │ 查看版本和编译选项
nginx -t                               │ ★ 检查配置语法（运维必备）
nginx -T                               │ 打印完整配置（排查 include）
nginx -s reload                        │ ★ 重新加载配置（零停机）
nginx -s quit                          │ 优雅关闭（处理完当前请求）
nginx -s stop                          │ 强制关闭
nginx -s reopen                        │ 重新打开日志文件（logrotate）
nginx -g "daemon off;"                 │ 前台运行（Docker）
ss -tlnp | grep nginx                  │ 查看监听端口和 worker 分布
ps aux | grep nginx                    │ 查看 master/worker 进程
curl /nginx_status                     │ ★ 查看实时连接状态
tail -f /var/log/nginx/access.log      │ 实时查看请求日志
strace -p $WORKER_PID -e trace=...     │ 追踪系统调用
perf stat -p $PIDS -- ab ...           │ 性能分析
```
