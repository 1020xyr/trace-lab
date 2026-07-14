/*
 * ======================================================================
 * 维度 2: 热升级与在线迁移
 * ======================================================================
 *
 * 核心问题：升级时不中断服务、不丢请求、能回滚
 *
 * 阅读要点：
 *   1. 热升级的核心挑战
 *   2. 进程替换方案
 *   3. 双进程过渡方案
 *   4. 连接迁移技术
 *   5. 回滚机制
 *
 * 预计阅读时间：20 分钟
 * ======================================================================
 */

# 热升级与在线迁移

## 1. 为什么需要热升级

```
传统停机升级：
  ├── 停服务 → 升级 → 重启 → 恢复
  ├── 维护窗口：凌晨 2-4 点
  ├── 影响：服务中断、用户体验差
  └── 存储/数据库：重启可能要恢复数据（几分钟到几小时）

热升级的价值：
  ★ 7x24 不中断
  ★ 无维护窗口
  ★ 用户无感知
  ★ 存储系统必须（不能停的数据）
```

## 2. 热升级的核心挑战

```
挑战 1：正在处理的请求怎么办？
  ├── 旧版本正在处理一个请求 → 不能中断
  └── 需要"优雅退出"：停止接受新请求 + 处理完已有的

挑战 2：内存状态怎么办？
  ├── 内存中有缓存/会话/计数器等
  └── 新进程没有这些状态 → 需要状态迁移

挑战 3：已建立的连接怎么办？
  ├── TCP 连接绑在旧进程的 fd 上
  └── 新进程如何接管这些连接

挑战 4：新版本有问题怎么办？
  └── 需要快速回滚到旧版本
```

## 3. 方案 1：进程替换（exec replacement）

### 原理

```
旧进程（PID=1000）          新进程
  │                          │
  │ 1. exec() 替换代码        │
  ├─────────────────────────→│
  │ （PID 不变，还是 1000）   │
  │                          │
  │ 2. socket fd 继承         │
  │ （fd 还在！）             │
  │                          │
  │ 3. 状态丢失               │
  │ （内存被新代码覆盖）       │
```

### fd 继承

```c
// 方案：SO_REUSEPORT（多进程共享端口）
int fd = socket(AF_INET, ...);
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
bind(fd, ...);
listen(fd, backlog);

// fork 多个子进程共享 fd
// 升级时：逐个替换子进程（滚轮升级）

// 方案：fd passing（sendmsg 传 fd）
struct msghdr msg = {};
struct cmsghdr *cmsg;
char buf[CMSG_SPACE(sizeof(int))];

msg.msg_control = buf;
msg.msg_controllen = sizeof(buf);
cmsg = CMSG_FIRSTHDR(&msg);
cmsg->cmsg_level = SOL_SOCKET;
cmsg->cmsg_type = SCM_RIGHTS;       // ★ 传 fd
cmsg->cmsg_len = CMSG_LEN(sizeof(int));
memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

sendmsg(sock, &msg, 0);  // 把 fd 发给新进程
```

### 优雅退出

```c
// 收到升级信号
void sigusr1_handler(int sig) {
    // ★ 1. 停止 accept 新连接
    close(listen_fd);
    
    // ★ 2. 设置退出标志，让 worker 完成已有请求
    shutdown_flag = 1;
    
    // ★ 3. 设置超时（不能无限等）
    alarm(30);  // 30 秒后强制退出
    
    // 4. worker 线程检查 shutdown_flag → 处理完当前请求后退出
}

// ★ 局限：内存状态丢失（无法迁移缓存/会话）
//   适合无状态服务或状态可重建的场景
```

## 4. 方案 2：双进程过渡（dual-process）

### 原理

```
阶段 1：旧版本单独运行
  ┌─ 旧进程 ─┐
  │ 接受请求 │
  │ 状态 A   │
  └──────────┘

阶段 2：启动新版本，流量灰度切换
  ┌─ 旧进程 ─┐    ┌─ 新进程 ─┐
  │ 70% 流量 │    │ 30% 流量 │  ← 灰度
  │ 状态 A   │    │ 状态 B   │
  └──────────┘    └──────────┘
  
  状态同步：共享内存/Redis/消息队列

阶段 3：完全切换到新版本
  ┌─ 新进程 ─┐
  │ 100% 流量 │
  │ 状态 B   │
  └──────────┘
  
  旧进程优雅退出
```

### 流量切换

```
方式 1：DNS 切换
  旧 IP → 新 IP（DNS TTL 控制切换速度）
  ★ 简单但慢（TTL 内的客户端还连旧 IP）

方式 2：负载均衡器切换
  Nginx/Haproxy 调整 upstream 权重
  ★ 快（秒级切换）

方式 3：SO_REUSEPORT
  多进程共享端口，内核负载均衡
  新旧进程同时 listen 同一端口
  ★ 内核层面切换，无缝
```

### 状态同步

```
方式 1：共享内存（SHM）
  旧进程退出前 → 写状态到 mmap(MAP_SHARED)
  新进程启动 → 读 SHM 恢复状态
  ★ Nginx reload 用这个传递配置和共享变量

方式 2：外部存储
  状态序列化到 Redis/文件 → 新进程读取
  ★ 适合大状态

方式 3：不迁移（重建）
  新进程从持久化数据重建状态
  ★ 适合状态可重建的场景（如缓存）
```

## 5. 方案 3：状态保持的热升级

### Nginx 的 reload 机制

```
Nginx reload（SIGHUP）流程：
  1. master 进程收到 SIGHUP
  2. master 重新读配置
  3. master 启动新 worker 进程（用新配置）
  4. 旧 worker 不再 accept 新连接
  5. 旧 worker 处理完已有连接后退出
  6. ★ 连接 fd 由新 worker 继承（SO_REUSEPORT 或 master 分配）

  ┌─ master ─┐
  │          │
  │  ┌─旧worker─┐  ┌─新worker─┐
  │  │ 处理已有  │  │ 接受新请求 │
  │  │ 连接      │  │          │
  │  └────┬─────┘  └──────────┘
  │       │ 处理完后退出
  └───────┘
```

### Redis 的在线升级

```
Redis 升级（BGSAVE + 重启）：
  1. BGSAVE → 异步把内存 dump 到 RDB 文件
  2. 关闭旧 Redis（优雅退出）
  3. 启动新 Redis
  4. 新 Redis 从 RDB 文件恢复内存状态
  5. 恢复完成 → 继续服务

  ★ 短暂中断（RDB 恢复期间）
  ★ 适合可接受秒级中断的场景
  ★ 主从架构：先升级从，再切流量，再升级主 → 无中断
```

## 6. 连接迁移技术

### SO_REUSEPORT

```c
// 多进程共享同一端口
int fd = socket(AF_INET, ...);
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
bind(fd, (struct sockaddr*)&addr, sizeof(addr));
listen(fd, 128);

// 多个进程 bind 同一端口 → 内核负载均衡
// 升级时：逐个进程替换，端口始终可用

  ┌─ 端口 80 ──────────────────┐
  │  内核负载均衡               │
  │ ┌──┐┌──┐┌──┐┌──┐          │
  │ │P1││P2││P3││P4│ ← 4 进程 │
  │ └──┘└──┘└──┘└──┘          │
  │ 替换 P3：停止 P3 accept →  │
  │ 新 P3 启动 → 无缝           │
  └────────────────────────────┘
```

### fd passing（fd 传递）

```c
// 旧进程把 socket fd 发给新进程
struct msghdr msg = {0};
char cbuf[CMSG_SPACE(sizeof(int))];
int fd_to_pass = client_fd;

msg.msg_control = cbuf;
msg.msg_controllen = sizeof(cbuf);

struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
cmsg->cmsg_level = SOL_SOCKET;
cmsg->cmsg_type = SCM_RIGHTS;  // ★ 传 fd 的类型
cmsg->cmsg_len = CMSG_LEN(sizeof(int));
memcpy(CMSG_DATA(cmsg), &fd_to_pass, sizeof(int));

sendmsg(unix_socket, &msg, 0);

// 新进程接收 fd
recvmsg(unix_socket, &msg, 0);
cmsg = CMSG_FIRSTHDR(&msg);
int new_fd;
memcpy(&new_fd, CMSG_DATA(cmsg), sizeof(int));
// ★ new_fd 和旧进程的 client_fd 指向同一个连接
```

### 连接 draining（连接排空）

```
优雅停止的连接处理：

  时间→
  收到 SIGTERM
  ↓
  1. close(listen_fd)          ← 不接受新连接
  ↓
  2. 设置超时定时器（如 30s）
  ↓
  3. 处理已有连接               ← 继续处理
  ↓
  ├── 所有连接处理完 → 正常退出
  └── 超时到 → 强制关闭剩余连接 → 退出
  
  ★ 客户端检测到连接关闭 → 重试到新进程
```

## 7. 回滚机制

### 自动回滚

```
升级流程（带健康检查）：

  1. 启动新版本（灰度 10%）
  ↓
  2. 健康检查（持续 60s）
  ├── 健康 → 继续灰度到 100%
  └── 不健康 → ★ 自动回滚（流量切回旧版本）
  ↓
  3. 灰度 30% → 50% → 100%
  ↓
  4. 旧版本下线

★ 关键：新旧版本同时存活期间，流量可快速切换
```

### 状态兼容性

```
数据格式版本兼容：

  升级前：数据格式 v1
  升级后：数据格式 v2

  向前兼容（新版本读旧数据）：
    新版本必须能读 v1 格式 → 否则升级后数据丢失

  向后兼容（旧版本读新数据）：
    v2 格式写的数据，如果回滚到 v1 → v1 能读吗？
    ★ 不能 → 回滚会失败

  设计原则：
    - 数据格式只加字段，不删字段
    - 新字段有默认值
    - 用版本号标记格式
    ★ 这样回滚时旧版本能忽略新字段
```

## 8. 存储系统特有挑战

```
存储系统的热升级更复杂：

  1. 数据格式版本
     ├── 磁盘上的数据格式升级 → 需要"在线转换"
     └── 或支持新旧格式并存（读时按格式解析）

  2. 元数据格式升级
     ├── inode/目录/分配表的结构变化
     └── 需要"在线 schema 变更"

  3. 在线迁移（data migration）
     ├── 数据从一个版本格式迁移到另一个
     └── 不能阻塞正常 I/O

  4. 升级期间的 I/O 一致性
     ├── 旧版本正在写的文件 → 新版本能正确处理
     └── WAL 格式新旧版本兼容
```

## 9. FUSE 文件系统的热升级

### 挑战

```
FUSE daemon 升级的难点：
  ├── /dev/fuse 的 fd 绑定在旧进程
  ├── 已打开的文件 fd 绑定在旧进程
  ├── 挂载点不能消失（否则客户端 I/O 失败）
  └── 内存中的状态（缓存/锁）需要迁移
```

### 方案：lazy umount + remount

```
方案 1：lazy umount
  1. umount -l /mnt/fuse     ← lazy umount，挂载点从 namespace 消失
  2. 但已有 fd 还能用（直到关闭）
  3. 重启 daemon → mount 新挂载点
  ★ 问题：新挂载点路径可能不同

方案 2：多挂载点 + 客户端切换
  /mnt/fuse_v1（旧）和 /mnt/fuse_v2（新）并存
  客户端切换访问路径
  ★ 需要客户端配合

方案 3：fd 继承（新 daemon 接管 fd）
  旧 daemon 退出前 → sendmsg 传 /dev/fuse 的 fd
  新 daemon → recvmsg 接收 fd → 继续 read/write
  ★ 最无缝，但实现复杂
```

## 10. 实战案例

### Nginx 的 reload（生产验证）

```bash
# Nginx reload
nginx -s reload

# 内部流程：
# 1. master 收到 SIGHUP
# 2. 解析新配置
# 3. fork 新 worker（用新配置）
# 4. 通知旧 worker "优雅退出"
# 5. 旧 worker 停止 accept，处理完已有连接后退出
# 6. ★ 期间服务不中断
```

### Ceph 的滚动升级

```
Ceph（分布式存储）的升级策略：
  ├── 滚动升级：逐个 OSD 升级
  ├── 每个 OSD 退出前 → 把 PG（数据分片）迁移到其他 OSD
  ├── 升级后 → PG 迁回
  └── ★ 全程不中断，但有数据迁移开销

 ceph osd set noout       # 不让 OSD 退出集群
 ceph osd down <osd_id>   # 停止 OSD
 升级 OSD 二进制
 ceph osd up <osd_id>     # 启动新版本 OSD
```

## 11. 实战检查清单

```
热升级设计检查清单：

连接管理：
  [ ] 支持 SO_REUSEPORT 或 fd passing
  [ ] 有连接 draining 机制（停止 accept + 处理已有）
  [ ] 有优雅退出超时（不能无限等）

状态管理：
  [ ] 状态可通过共享内存/外部存储迁移
  [ ] 或状态可重建（无状态服务）

回滚机制：
  [ ] 新旧版本能同时运行
  [ ] 流量可快速切换
  [ ] 有健康检查自动触发回滚
  [ ] 数据格式向前/向后兼容

测试：
  [ ] 升级过程压测（请求不丢）
  [ ] 回滚测试
  [ ] 升级期间故障注入（升级到一半断电）
  [ ] 长时间运行后升级（状态积累多）
```
