/*
 * ======================================================================
 * 维度 7: 安全与隔离
 * ======================================================================
 *
 * 核心问题：最小权限、失败安全、纵深防御
 *
 * 预计阅读时间：10 分钟
 * ======================================================================
 */

# 安全与隔离

## 1. 权限分离（Privilege Separation）

```
不要用 root 运行服务！

  原则：
    - 需特权的操作（bind 80 端口、打开 raw socket）→ 用 root
    - 正常请求处理 → 降权到普通用户

  Nginx 模型：
    master（root）→ bind 80 → fork worker → worker setuid(nginx)
    → worker 被攻破 → 只有 nginx 用户权限

  ★ 失败安全（fail-safe）：
    默认拒绝 → 显式允许
    默认最小权限 → 需要时提升
```

## 2. seccomp（系统调用过滤）

```
限制进程能调用的系统调用：

  // 只允许 read/write/exit 等必要调用
  // 其他（execve/fork/socket）→ 杀死进程

  seccomp_rule add read
  seccomp_rule add write
  seccomp_rule add exit
  seccomp_load  // ★ 启用过滤

  价值：
    代码有漏洞（如 buffer overflow）
    → 攻击者想 execve("/bin/sh")
    → seccomp 阻止 → 无法执行 shellcode
```

## 3. namespace 与沙箱

```
容器隔离（Linux namespaces）：
  PID namespace  → 独立进程号
  Mount namespace → 独立文件系统视图
  Network namespace → 独立网络栈
  User namespace  → 独立用户映射

  ★ 存储系统的沙箱：
    处理不可信文件 → 在容器/namespace 中处理
    → 即使被攻破 → 只影响沙箱

  ★ FUSE 的安全：
    FUSE daemon 可以运行在非特权用户
    → unprivileged mount（fusermount）
    → daemon 被攻破 → 无 root 权限
```

## 4. 输入校验

```
永远不信任输入！

  文件路径：检查 ../（路径穿越）
  文件大小：限制最大（防止内存耗尽）
  请求速率：限流（防止 DoS）
  字段长度：限制（防止 buffer overflow）

  ★ 存储系统特有：
    - 校验 I/O 请求的 offset/len（不能越界）
    - 校验文件路径（不能 /etc/passwd）
    - 校验元数据（不能伪造 inode）
```

## 5. 数据完整性

```
防止静默数据损坏（Silent Data Corruption）：

  end-to-end 校验：
    写入时：计算 CRC → 和数据一起存
    读取时：重新计算 CRC → 和存储的对比
    → 不一致 → 数据损坏

  ★ 存储系统的完整性：
    - 每个数据块有 checksum
    - 读取时校验 → 损坏则修复（从副本恢复）
    - Ceph/Btrfs/ZFS 都有 end-to-end checksum
```
