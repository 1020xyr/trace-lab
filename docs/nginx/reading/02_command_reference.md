# nginx 完整命令参考与配置速查

> 命令行参数、信号控制、nginx.conf 关键指令、10+ 个性能调优场景

---

## 一、nginx 命令行参数

### 基本用法

```bash
nginx [-?hvVtTq] [-s signal] [-c filename] [-p prefix] [-e filename] [-g directives]
```

### 参数与功能对照

| 参数 | 含义 | 示例 | 源码位置 |
|------|------|------|---------|
| `-c <file>` | 指定配置文件（覆盖编译时默认值） | `nginx -c /tmp/my.conf` | `nginx.c: ngx_get_options()` |
| `-p <prefix>` | 指定工作目录前缀（覆盖 prefix） | `nginx -p /opt/nginx/` | `nginx.c: ngx_get_options()` |
| `-t` | ★ 检查配置文件语法是否正确 | `nginx -t` | `nginx.c: ngx_test_config` |
| `-T` | 检查配置并打印完整配置内容 | `nginx -T` | `nginx.c: ngx_dump_config` |
| `-s <signal>` | ★ 向 master 进程发送信号 | `nginx -s reload` | `nginx.c: ngx_signal` |
| `-v` | 显示版本号 | `nginx -v` | `nginx.c: ngx_show_version` |
| `-V` | ★ 显示版本号 + 编译选项 | `nginx -V` | `nginx.c: ngx_show_version` |
| `-q` | 静默模式（抑制 `-t` 的正常输出） | `nginx -t -q` | `nginx.c: ngx_quiet` |
| `-g <directives>` | 在命令行中设置全局指令 | `nginx -g "daemon off;"` | `nginx.c: ngx_get_options()` |
| `-?` 或 `-h` | 显示帮助信息 | `nginx -h` | `nginx.c: ngx_get_options()` |
| `-e <file>` | 指定错误日志文件 | `nginx -e /tmp/err.log` | `nginx.c: ngx_error_log` |

### 常用组合

```bash
# ★ 检查配置是否合法（运维必备）
nginx -t
# 输出：
# nginx: the configuration file /etc/nginx/nginx.conf syntax is ok
# nginx: configuration file /etc/nginx/nginx.conf test is successful

# ★ 检查并打印完整配置（含 include 展开）
nginx -T
# 输出所有配置文件的完整内容，方便排查 include 问题

# ★ 在前台运行（Docker 常用）
nginx -g "daemon off;"

# ★ 指定配置启动（多环境切换）
nginx -c /etc/nginx/nginx-staging.conf

# 显示版本和编译信息
nginx -V 2>&1
```

---

## 二、信号控制

### 通过 `-s` 发送信号

```bash
nginx -s <signal>
```

| 信号 | 命令 | 效果 | master 行为 |
|------|------|------|------------|
| `stop` | `nginx -s stop` | ★ 强制关闭 | 发送 SIGTERM 给所有 worker，立即退出 |
| `quit` | `nginx -s quit` | ★ 优雅关闭 | 发送 SIGQUIT 给所有 worker，等它们处理完当前请求 |
| `reload` | `nginx -s reload` | ★ 重新加载配置 | 检查配置 → 启动新 worker → 优雅关闭旧 worker |
| `reopen` | `nginx -s reopen` | 重新打开日志文件 | 发送 SIGUSR1 给所有 worker（用于 logrotate） |

### 通过 kill 发送信号（高级用法）

```bash
# 获取 master PID
MASTER_PID=$(cat /run/nginx.pid)

# 热升级（binary upgrade）—— 不中断服务的情况下升级 nginx 二进制
kill -USR2 $MASTER_PID    # 启动新 master + 新 worker
kill -WINCH $MASTER_PID   # 优雅关闭旧 worker
# 验证新 worker 正常后：
kill -QUIT $(cat /run/nginx.pid.oldbin)  # 关闭旧 master
```

### 信号处理时序

```
reload 时序：

nginx -s reload
     │
     ▼
master 收到 SIGHUP
     │
     ├── 1. 重新解析 nginx.conf
     │      语法错误 → 放弃 reload，保持旧配置运行
     │
     ├── 2. 创建新的 listen socket（如果端口变了）
     │
     ├── 3. fork() 新 worker 进程（使用新配置）
     │
     ├── 4. 发送 SIGQUIT 给旧 worker
     │      旧 worker 完成当前请求后退出
     │
     └── 5. 收到旧 worker 的 SIGCHLD
            新 worker 接管所有新连接
```

### logrotate 配合 reopen

```bash
# /etc/logrotate.d/nginx
/var/log/nginx/*.log {
    daily
    missingok
    rotate 14
    compress
    delaycompress
    notifempty
    sharedscripts
    postrotate
        # ★ 告诉 nginx 重新打开日志文件
        [ -f /run/nginx.pid ] && kill -USR1 $(cat /run/nginx.pid)
    endscript
}
```

---

## 三、nginx.conf 关键指令速查

### 核心模块（Core Module）指令

| 指令 | 默认值 | 含义 | 性能影响 |
|------|--------|------|---------|
| `worker_processes` | 1 | ★ worker 进程数 | `auto` = CPU 核数，最重要参数 |
| `worker_connections` | 512 | ★ 每个 worker 最大连接数 | 直接决定并发能力 |
| `worker_rlimit_nofile` | 系统默认 | worker 最大 fd 数 | 需 ≥ worker_connections × 2 |
| `daemon` | on | 是否以守护进程运行 | Docker 中设为 off |
| `master_process` | on | 是否启动 master 进程 | 调试时可关闭 |
| `pid` | `logs/nginx.pid` | master PID 文件路径 | — |
| `error_log` | `logs/error.log error` | 错误日志路径和级别 | 级别越低越耗性能 |
| `worker_cpu_affinity` | 不绑核 | worker 绑定到指定 CPU | ★ 减少 cache miss |
| `worker_priority` | 0 | worker 进程的 nice 值 | 负值提高优先级 |

### events 块指令

| 指令 | 默认值 | 含义 | 性能影响 |
|------|--------|------|---------|
| `use` | 自动检测 | ★ 事件模型 | Linux 自动选 epoll |
| `worker_connections` | 512 | ★ 最大连接数 | 增大可支持更多并发 |
| `multi_accept` | off | 一次 accept 多个连接 | ★ on 提高突发连接处理速度 |
| `accept_mutex` | off | accept 互斥锁 | 1.11.3+ 可关闭（EPOLLEXCLUSIVE） |
| `accept_mutex_delay` | 500ms | 未获锁后的等待时间 | 仅在 accept_mutex on 时有效 |

### http 块性能指令

| 指令 | 默认值 | 含义 | 性能影响 |
|------|--------|------|---------|
| `sendfile` | off | ★ 启用零拷贝发送 | 静态文件必须开启 |
| `tcp_nopush` | off | ★ 合并 HTTP 头和文件数据 | 配合 sendfile 使用 |
| `tcp_nodelay` | on | ★ 禁用 Nagle 算法 | 减少小包延迟 |
| `keepalive_timeout` | 75s | ★ keep-alive 超时时间 | 过长浪费连接，过短增加 TCP 握手 |
| `keepalive_requests` | 1000 | ★ 单连接最大请求数 | 高并发场景适当增大 |
| `client_header_buffer_size` | 1k | 请求头缓冲区 | 大 Cookie 场景需增大 |
| `large_client_header_buffers` | 4 8k | 大请求头缓冲区 | 长 URL/Cookie 场景 |
| `client_body_buffer_size` | 8k/16k | 请求体缓冲区 | POST 大表单时需增大 |
| `client_max_body_size` | 1m | ★ 请求体最大大小 | 文件上传场景需增大 |
| `open_file_cache` | off | ★ 文件元数据缓存 | 静态文件服务必须开启 |
| `open_file_cache_valid` | 60s | 缓存有效期 | — |
| `open_file_cache_min_uses` | 1 | 缓存最低访问次数 | — |
| `types_hash_max_size` | 1024 | MIME 类型哈希表大小 | 增大减少哈希冲突 |
| `server_names_hash_max_size` | 512 | server_name 哈希表大小 | 多虚拟主机时需增大 |

### 日志指令

| 指令 | 默认值 | 含义 |
|------|--------|------|
| `access_log` | `logs/access.log combined` | ★ 访问日志路径和格式 |
| `log_format` | combined | 日志格式定义 |
| `error_log` | `logs/error.log error` | 错误日志路径和级别 |
| `log_not_found` | on | 404 请求是否记录到 error_log |
| `open_log_file_cache` | off | 日志文件描述符缓存 |

### proxy（反向代理）指令

| 指令 | 默认值 | 含义 | 性能影响 |
|------|--------|------|---------|
| `proxy_pass` | — | ★ upstream 地址 | — |
| `proxy_connect_timeout` | 60s | 连接 upstream 超时 | ★ 过长导致请求堆积 |
| `proxy_read_timeout` | 60s | 等待 upstream 响应超时 | ★ 过长占用 worker 过久 |
| `proxy_send_timeout` | 60s | 发送请求到 upstream 超时 | — |
| `proxy_buffering` | on | 是否缓冲 upstream 响应 | ★ off 可减少延迟但增加连接时间 |
| `proxy_buffer_size` | 4k/8k | 响应头缓冲区 | — |
| `proxy_buffers` | 8 4k/8k | 响应体缓冲区 | 大响应需增大 |
| `proxy_cache_path` | — | ★ 缓存目录和参数 | 缓存命中可大幅减少 upstream 负载 |
| `proxy_http_version` | 1.0 | ★ upstream HTTP 版本 | 设为 1.1 启用 keepalive |

### gzip 压缩指令

| 指令 | 默认值 | 含义 | 性能影响 |
|------|--------|------|---------|
| `gzip` | off | ★ 启用 gzip 压缩 | 减少带宽，增加 CPU 开销 |
| `gzip_comp_level` | 1 | 压缩级别 (1-9) | ★ 1-2 性价比最高 |
| `gzip_min_length` | 20 | 最小压缩长度 | 小文件压缩不划算 |
| `gzip_types` | `text/html` | 压缩的 MIME 类型 | 按需添加 |
| `gzip_vary` | off | 添加 Vary: Accept-Encoding | ★ CDN 场景必须开启 |
| `gzip_buffers` | 4 4k/8k | 压缩缓冲区 | — |

---

## 四、10+ 个性能调优场景

### 场景 1: 基础调优 — 最大化并发能力

```nginx
# ★ 最基本的三项调优
worker_processes auto;              # 等于 CPU 核数
events {
    worker_connections 4096;        # 每个 worker 4096 连接
    multi_accept on;                # 一次 accept 多个
}
# 理论最大并发 = worker_processes × worker_connections
# = 4 × 4096 = 16384 个并发连接
```

**原理：** `worker_processes auto` 让每个 CPU 核运行一个 worker，避免跨核调度开销。`worker_connections 4096` 提升每个 worker 的连接容量。

### 场景 2: 静态文件服务 — 启用零拷贝

```nginx
http {
    sendfile on;                    # ★ 启用 sendfile 零拷贝
    tcp_nopush on;                  # 合并 TCP 包
    tcp_nodelay on;                 # 禁用 Nagle
    
    # ★ 文件缓存（减少 stat/open 系统调用）
    open_file_cache max=10000 inactive=60s;
    open_file_cache_valid 60s;
    open_file_cache_min_uses 2;
    open_file_cache_errors on;
}
```

**效果：** 文件从磁盘直接通过 DMA 到网卡，不经过用户态。`open_file_cache` 缓存文件的 fd、大小、修改时间，避免重复 `stat()` 和 `open()`。

### 场景 3: keepalive 优化 — 减少 TCP 握手

```nginx
http {
    keepalive_timeout 65;           # ★ 空闲超时 65 秒
    keepalive_requests 10000;       # ★ 单连接最大 10000 个请求
    keepalive_disable msie6;        # IE6 禁用 keepalive
    
    # upstream keepalive（反向代理场景）
    upstream backend {
        server 127.0.0.1:8000;
        keepalive 32;               # ★ 保持 32 个长连接到 upstream
    }
    
    server {
        location /api/ {
            proxy_pass http://backend;
            proxy_http_version 1.1;       # ★ 必须 1.1 才有 keepalive
            proxy_set_header Connection "";  # 清除 close 头
        }
    }
}
```

**效果：** 减少 TCP 三次握手和 TLS 握手的开销。upstream keepalive 避免每次请求都重建 TCP 连接。

### 场景 4: 反向代理超时优化

```nginx
http {
    # ★ 缩短超时时间，避免慢 upstream 拖垮 worker
    proxy_connect_timeout 5s;       # 连接 upstream 最多等 5 秒
    proxy_read_timeout 30s;         # 等待响应最多 30 秒
    proxy_send_timeout 10s;         # 发送请求最多 10 秒
    
    # 启用缓冲（避免 upstream 慢导致 worker 被占用）
    proxy_buffering on;
    proxy_buffer_size 8k;
    proxy_buffers 8 16k;
    proxy_busy_buffers_size 32k;
}
```

**原理：** 默认 `proxy_connect_timeout 60s` 太长 — 如果 upstream 挂了，worker 会等 60 秒才释放。缩短到 5s 可以快速失败，释放 worker 处理其他请求。

### 场景 5: gzip 压缩 — 减少带宽

```nginx
http {
    gzip on;
    gzip_comp_level 2;              # ★ 级别 2，性价比最高
    gzip_min_length 1024;           # 小于 1KB 不压缩
    gzip_vary on;                   # ★ CDN 必须
    gzip_proxied any;               # 对所有代理请求也压缩
    gzip_types
        text/plain
        text/css
        application/json
        application/javascript
        text/xml
        application/xml
        image/svg+xml;
}
```

**效果：** 文本类资源通常压缩 60-80%。`gzip_comp_level 2` 在压缩率和 CPU 开销之间取得最佳平衡。

### 场景 6: 限流 — 防止过载

```nginx
http {
    # ★ 基于 IP 的请求速率限制
    limit_req_zone $binary_remote_addr zone=req_limit:10m rate=100r/s;
    
    # ★ 基于 IP 的并发连接限制
    limit_conn_zone $binary_remote_addr zone=conn_limit:10m;
    
    server {
        location /api/ {
            limit_req zone=req_limit burst=50 nodelay;
            limit_conn conn_limit 20;
            limit_req_status 429;       # Too Many Requests
        }
    }
}
```

**原理：** `limit_req` 使用令牌桶算法 — 每秒生成 100 个令牌，请求消耗令牌，桶满时允许突发（burst=50）。`nodelay` 表示突发请求立即处理而非排队。

### 场景 7: CPU 绑核 — 减少 cache miss

```nginx
# 4 核 CPU
worker_processes 4;
worker_cpu_affinity 0001 0010 0100 1000;
# 或自动绑核（1.25.0+）
# worker_cpu_affinity auto;
```

**原理：** 将 worker 绑定到固定 CPU 核，使得该核的 L1/L2 缓存被同一个 worker 的数据占据，减少 cache miss。

### 场景 8: 文件描述符上限

```nginx
# nginx.conf 顶层
worker_rlimit_nofile 65535;         # ★ worker 最大 fd 数

events {
    worker_connections 16384;       # 需要 worker_rlimit_nofile ≥ 2 × 16384
}
```

```bash
# 同时需要修改系统级限制
# /etc/security/limits.conf
nginx soft nofile 65535
nginx hard nofile 65535

# 或通过 systemd
# /etc/systemd/system/nginx.service.d/override.conf
[Service]
LimitNOFILE=65535
```

### 场景 9: 缓冲区优化

```nginx
http {
    # ★ 请求头缓冲区（大 Cookie 场景）
    client_header_buffer_size 4k;
    large_client_header_buffers 4 16k;
    
    # ★ 请求体缓冲区（大表单/文件上传）
    client_body_buffer_size 128k;
    client_max_body_size 50m;
    
    # ★ 代理响应缓冲区
    proxy_buffer_size 16k;
    proxy_buffers 4 64k;
    proxy_busy_buffers_size 128k;
    
    # 大响应写到临时文件
    proxy_temp_file_write_size 128k;
    proxy_max_temp_file_size 1024m;
}
```

### 场景 10: SSL/TLS 优化

```nginx
http {
    # ★ SSL 会话缓存（避免重复握手）
    ssl_session_cache shared:SSL:10m;    # 10MB ≈ 40000 个会话
    ssl_session_timeout 1d;              # 会话有效期 1 天
    ssl_session_tickets off;             # 安全考虑关闭 tickets
    
    # ★ 选择高效 cipher
    ssl_ciphers ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256;
    ssl_prefer_server_ciphers on;
    
    # ★ OCSP Stapling
    ssl_stapling on;
    ssl_stapling_verify on;
    
    # HTTP/2（多路复用，减少连接数）
    listen 443 ssl http2;
}
```

### 场景 11: 高并发反向代理模板

```nginx
# ★ 综合调优：高并发反向代理场景

worker_processes auto;
worker_rlimit_nofile 65535;

events {
    worker_connections 16384;
    multi_accept on;
}

http {
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 65;
    keepalive_requests 10000;
    
    # 限流
    limit_req_zone $binary_remote_addr zone=api:10m rate=1000r/s;
    
    # upstream 连接池
    upstream backend {
        least_conn;                    # 最少连接算法
        server 127.0.0.1:8000;
        server 127.0.0.1:8001;
        keepalive 64;                  # ★ 64 个长连接
    }
    
    # gzip
    gzip on;
    gzip_comp_level 2;
    gzip_types text/plain application/json application/javascript;
    
    server {
        listen 80;
        
        location /api/ {
            limit_req zone=api burst=200 nodelay;
            
            proxy_pass http://backend;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_connect_timeout 3s;
            proxy_read_timeout 30s;
        }
    }
}
```

### 场景 12: 静态资源 CDN 源站模板

```nginx
# ★ 综合调优：静态资源 CDN 回源场景

worker_processes auto;

events {
    worker_connections 8192;
    multi_accept on;
}

http {
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    
    # ★ 文件缓存（CDN 回源频繁，缓存 fd 和元数据）
    open_file_cache max=50000 inactive=300s;
    open_file_cache_valid 120s;
    open_file_cache_min_uses 1;
    
    # ★ 长 keepalive（CDN 复用连接）
    keepalive_timeout 300;
    keepalive_requests 100000;
    
    server {
        listen 80;
        root /data/cdn;
        
        # 静态资源直接返回
        location / {
            expires 30d;
            add_header Cache-Control "public, immutable";
            access_log off;             # ★ 关闭 access_log 减少 I/O
        }
        
        # 健康检查（给 CDN 探测用）
        location /health {
            access_log off;
            return 200 "OK";
        }
    }
}
```

---

## ★ 总结：性能调优速查表

```
调优方向        │ 关键指令                              │ 预期效果
───────────────┼───────────────────────────────────────┼───────────────────
并发能力        │ worker_processes auto                │ CPU 核数个 worker
                │ worker_connections 4096+             │ 万级并发
                │ worker_rlimit_nofile 65535           │ fd 上限匹配
                │                                      │
静态文件        │ sendfile on                          │ 零拷贝发送
                │ tcp_nopush on                        │ 合并 TCP 包
                │ open_file_cache max=N                │ 缓存文件元数据
                │                                      │
连接复用        │ keepalive_timeout 65                 │ 减少 TCP 握手
                │ keepalive_requests 10000             │ 长连接多请求
                │ upstream keepalive N                 │ upstream 长连接
                │                                      │
反向代理        │ proxy_connect_timeout 3-5s           │ 快速失败
                │ proxy_read_timeout 30s               │ 避免堆积
                │ proxy_http_version 1.1               │ upstream keepalive
                │                                      │
压缩            │ gzip on                              │ 减少 60-80% 带宽
                │ gzip_comp_level 2                    │ 低 CPU 开销
                │                                      │
安全限流        │ limit_req_zone + limit_req           │ 防 DDoS/爬虫
                │ limit_conn_zone + limit_conn         │ 限制并发连接
                │                                      │
CPU 亲和        │ worker_cpu_affinity auto             │ 减少 cache miss
```
