/*
 * ======================================================================
 * 维度 1: 崩溃恢复与数据持久性
 * ======================================================================
 *
 * 核心问题：进程崩溃/断电/OOM 后重启，数据不丢、状态一致
 *
 * 阅读要点：
 *   1. fsync 的真实语义（write 返回 ≠ 落盘）
 *   2. WAL/Journaling 如何保证崩溃一致
 *   3. 崩溃恢复流程
 *   4. FUSE 崩溃恢复的特殊性
 *
 * 预计阅读时间：20 分钟
 * ======================================================================
 */

# 崩溃恢复与数据持久性

## 1. 为什么崩溃恢复是生产软件的根基

```
崩溃是必然的：
  ├── OOM killer（内存不足时内核杀进程）
  ├── bug 导致 segfault
  ├── 断电/硬件故障
  ├── kill -9（运维误操作或自动重启脚本）
  └── 内核 panic

关键不是"如何避免崩溃"，而是"崩溃后如何恢复"

存储软件的崩溃恢复要求：
  ★ 已确认落盘的数据 → 不能丢
  ★ 未确认的数据 → 要么完成、要么完全消失（不能半完成）
  ★ 元数据（目录结构/分配表）→ 必须一致
  ★ 重启后 → 能快速恢复到一致状态
```

## 2. fsync/fdatasync 的真实语义

### ★ 最常见的致命误解

```
错误认知：
  write(fd, buf, 4096) 返回 4096 → 数据已经写到磁盘了
  
真实情况：
  write() 返回只是把数据复制到 page cache（内存！）
  ↓
  内核 writeback 线程会在以后某个时刻（可能几秒后）刷到磁盘
  ↓
  在这期间崩溃 → 数据丢失！

  ★ 只有 fsync(fd) 返回 0，数据才真正落盘
```

### page cache 的写入路径

```
用户进程 write()
    │
    ▼
  page cache（内存中的脏页）  ← write() 到这里就返回了
    │
    │  内核 writeback 线程（周期性或脏页比例超阈值时）
    ▼
  block layer（I/O 调度器）
    │
    ▼
  设备写缓存（SSD/HDD 的 DRAM 缓存）  ← 还在内存里！
    │
    │  需要 flush/FUA 命令
    ▼
  ★ 持久化介质（NAND/盘片）  ← fsync 保证到这里
```

### fsync vs fdatasync

```c
// fsync：刷数据 + 元数据（文件大小、修改时间、分配的块等）
int fsync(int fd);
// 保证：fd 对应文件的所有数据和元数据都落盘

// fdatasync：只刷数据（不刷元数据，除非元数据影响数据完整性）
int fdatasync(int fd);
// 保证：数据落盘，但修改时间等元数据可能没刷
// ★ 更快，适合"不关心修改时间，只关心数据"的场景

// 关键差异：
//   fdatasync 不刷 inode 的 mtime，但会刷"新分配的数据块"信息
//   因为如果不刷块分配信息，崩溃后可能读到垃圾数据
```

### ext4 的 journal 对 fsync 的影响

```
ext4 data=ordered（默认）：
  fsync() → 触发 journal commit → 数据 + 元数据都落盘
  ★ journal 本身先落盘（有 journal 的 fsync 比裸 write 安全）

ext4 data=writeback：
  fsync() → 数据落盘，但元数据可能在 journal 中
  崩溃后可能看到旧数据（writeback 模式不保证顺序）

ext4 data=journal：
  数据也写入 journal → 最安全但最慢
```

### ★ 生产软件的持久性检查清单

```c
// 危险写法（数据可能丢）
write(fd, data, len);
close(fd);
// 崩溃 → 数据可能丢

// 安全写法
write(fd, data, len);
fsync(fd);           // ★ 数据落盘
fsync(dirfd);        // ★★ 还要刷目录（如果新建/重命名了文件）
close(fd);
// 为什么还要 fsync 目录？
//   如果新建了文件，目录条目（文件名→inode 映射）在目录的 page cache 中
//   不 fsync 目录，崩溃后文件可能存在但目录里没有名字 → 文件"丢失"
```

## 3. WAL（Write-Ahead Logging）

### 核心思想

```
先写日志，再改数据：

  时间→
  ┌──────────┐    ┌──────────┐
  │ 写 WAL   │ →  │ 改数据   │
  │ (日志)   │    │ (实际数据)│
  └──────────┘    └──────────┘
  ★ WAL 先落盘     数据可以慢慢改

为什么能保证崩溃一致：
  场景 A：WAL 写完后崩溃 → 重启时 redo WAL → 数据一致
  场景 B：改数据时崩溃 → 重启时 redo WAL → 重新改数据
  场景 C：WAL 没写完就崩溃 → 丢弃这条不完整的日志 → 数据不变
  → 无论何时崩溃，都能恢复到一致状态
```

### redo/undo 日志

```
redo log（重做日志）：
  记录"做了什么修改"
  崩溃后重做这些修改 → 恢复到崩溃前状态
  例：MySQL InnoDB 的 redo log

undo log（回滚日志）：
  记录"修改前的旧值"
  崩溃后如果事务没完成 → 回滚到旧值
  例：InnoDB 的 undo log（用于回滚和 MVCC）

组合：物理日志记录页的物理修改
  InnoDB 采用物理逻辑日志（physiological logging）
```

### group commit（组提交）

```
问题：每个事务都 fsync WAL → fsync 开销大（HDD ~5ms, SSD ~0.1ms）
解决：多个事务的 WAL 合并成一次 fsync

  事务1 ─┐
  事务2 ─┼─→ 合并写入 WAL → 一次 fsync → 三个事务都确认
  事务3 ─┘

★ 这是高吞吐数据库的关键优化
```

## 4. Journaling 文件系统

### ext4 的三种模式

```
ext4 data=ordered（默认）：
  所有数据先写到 journal 区，再写到实际位置
  但只 journal 元数据，数据按顺序写到数据区
  ★ 保证：不会看到"新数据写了一半"

ext4 data=writeback：
  只 journal 元数据，数据不保证顺序
  ★ 危险：崩溃后可能看到旧数据（写入被截断）

ext4 data=journal：
  数据和元数据都写 journal → 最安全
  ★ 最慢：所有数据写两遍
```

### journal 的原子性

```
journal commit 是原子的：
  journal 区按事务（transaction）组织
  每个事务有 commit block + 描述 block
  
  写 journal：
    1. 写描述 + 数据到 journal
    2. 写 commit block（含校验和）← 原子点
    3. checkpoint：把数据写到实际位置
    4. 释放 journal 空间

  崩溃恢复：
    - commit block 完整 → 事务有效 → redo
    - commit block 不完整 → 事务无效 → 丢弃
```

## 5. 原子写技术

### rename() 的原子性

```c
// 危险：直接写原文件，崩溃时文件可能损坏
fd = open("config", O_WRONLY|O_TRUNC);
write(fd, new_config, len);
close(fd);
// 崩溃 → config 可能是半新半旧的！

// 安全：写临时文件 + rename
fd = open("config.tmp", O_WRONLY|O_CREAT);
write(fd, new_config, len);
fsync(fd);
close(fd);
rename("config.tmp", "config");   // ★ rename 是原子的
fsync(dirfd);                     // ★ 刷目录
// 崩溃 → 要么旧 config，要么新 config，不会半新半旧
```

### O_DIRECT + FUA

```c
// 绕过 page cache + 强制写介质
fd = open(file, O_DIRECT);
write(fd, buf, 4096);  // 带 FUA 标志 → 直接到介质，不经过设备缓存
// ★ 无需 fsync，write 返回即落盘
// 但要求 buffer 对齐、大小是 block 的倍数
```

## 6. 崩溃恢复流程

```
重启后恢复流程：

  1. 扫描 WAL/Journal
     ├── 找到所有已 commit 但未 checkpoint 的事务
     └── 找到所有未 commit 的事务（日志不完整）

  2. redo（重做）
     对每个已 commit 的事务 → 重新应用到数据区
     （因为可能没来得及写到数据区就崩溃了）

  3. undo（回滚）
     对每个未 commit 的事务 → 回滚数据区中的部分修改
     （因为数据可能改了一半）

  4. 恢复完成 → 进入正常服务状态

★ 恢复时间 = redo/undo 的事务数 × 单事务处理时间
  → WAL 越长恢复越慢 → 定期 checkpoint 截断 WAL
```

## 7. 检查点（Checkpointing）

```
目的：缩短恢复时间

无 checkpoint：
  WAL 从系统启动一直增长 → 恢复时扫描全部 → 慢

有 checkpoint：
  定期把内存状态刷到磁盘 → 记录"到此为止的数据已落盘"
  → 恢复时只需处理 checkpoint 之后的 WAL → 快

  时间→
  WAL: █████████████░░░░░░░░░░░░
                 ↑ checkpoint
                 之前的已落盘
                 之后的重做

  checkpoint 的代价：
    - 全量 checkpoint：刷所有脏页 → 慢但恢复快
    - 增量 checkpoint：只刷部分 → 快但恢复稍慢
    - fuzzy checkpoint：不阻塞写入 → 适合在线服务
```

## 8. ★ FUSE 崩溃恢复

### FUSE daemon 崩溃时的内核行为

```
FUSE daemon 崩溃：
  内核 FUSE 模块 → /dev/fuse 的 fd 关闭
  
  正在处理的请求：
    用户态 read(/dev/fuse) → 阻塞中
    daemon 崩溃 → fd 关闭 → read 返回错误
    
  ★ 内核 FUSE 模块的行为：
    - 所有挂起的请求 → 返回错误（通常是 EIO 或 ENOTCONN）
    - 新请求 → 立即失败（没有 daemon 处理）
    - 已打开的 fd → 可能还能继续操作（取决于内核版本）

  挂载点状态：
    - daemon 崩溃 → 挂载点变成"stale"（不可用）
    - 访问挂载点 → 返回 ENOTCONN 或 Transport endpoint is not connected
```

### FUSE 恢复策略

```
策略 1：重启 daemon + 重新挂载
  ┌─────────────────────────────────────────┐
  │ daemon 崩溃                              │
  │   ↓                                      │
  │ 内核标记挂载点 stale                      │
  │   ↓                                      │
  │ 监控脚本检测到 daemon 死亡                 │
  │   ↓                                      │
  │ umount（lazy umount，处理已有 fd）       │
  │   ↓                                      │
  │ 重启 daemon                              │
  │   ↓                                      │
  │ 重新挂载                                 │
  │   ↓                                      │
  │ 恢复服务                                 │
  └─────────────────────────────────────────┘
  ★ 问题：umount 期间已有 fd 的操作失败

策略 2：多 daemon 热备
  主 daemon + 备 daemon
  主崩溃 → 备接管 /dev/fuse 的 fd
  ★ 挑战：内核 FUSE 不原生支持 fd 迁移

策略 3：客户端重试
  客户端检测到 ENOTCONN → 重试/切换到备用挂载点
  ★ 需要客户端配合
```

### 崩溃期间的请求管理

```
关键设计：幂等性 + 重试

  客户端 → FUSE 写请求 → daemon 崩溃 → 请求失败
  
  如果写操作幂等：
    客户端重试 → 新 daemon 处理 → 成功
    ★ 安全（重试不会导致重复写入）

  如果写操作不幂等（如 append）：
    重试可能导致重复写入
    ★ 需要：唯一 request_id + 去重表

  超时处理：
    客户端设置超时 → daemon 恢复慢则超时失败
    ★ 比无限等待好（避免客户端积压）
```

## 9. 实战检查清单

### 生产存储软件上线前必须检查

```
持久性：
  [ ] 所有关键数据写入后有 fsync()
  [ ] 新建文件后 fsync 父目录
  [ ] 配置文件用"临时文件 + rename"更新
  [ ] 了解文件系统的 journal 模式（ordered/writeback）

崩溃恢复：
  [ ] 有 WAL/journal 机制保证一致
  [ ] 有 checkpoint 缩短恢复时间
  [ ] 重启后能自动扫描并恢复
  [ ] 恢复时间有上限（不能无限增长）

异常处理：
  [ ] SIGTERM 能优雅退出（flush 脏数据）
  [ ] 能被 kill -9 后安全恢复
  [ ] OOM 时不会损坏数据（或能检测到损坏）
  [ ] 有幂等设计支持重试

可观测：
  [ ] 记录每次 checkpoint 的时间点
  [ ] 记录恢复过程的日志
  [ ] 有 fsync 延迟监控
  [ ] 有 WAL 增长速度监控

测试：
  [ ] 崩溃注入测试（断电、kill -9）
  [ ] fsync 失败时的行为测试
  [ ] 磁盘满时的行为测试
  [ ] 恢复时间压力测试
```
