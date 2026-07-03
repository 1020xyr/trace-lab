# FUSE 完整命令参考

> 涵盖挂载命令、调试选项、libfuse API 速查、编译方法、常用命令组合、内核参数
> 关联文件：`reading/01_fuse_operations.h`、`reading/02_dev_fuse_protocol.md`、`reading/03_hello_fuse.c`

---

## 一、FUSE 挂载命令

### 1.1 fusermount / fusermount3

`fusermount` 是 FUSE 文件系统的标准挂载/卸载工具，属于 ★ setuid 程序，★ 允许普通用户挂载 FUSE 文件系统。

```bash
# FUSE 2.x
fusermount [选项] <挂载点>

# FUSE 3.x
fusermount3 [选项] <挂载点>
```

#### 完整参数表

| 参数 | 含义 | 说明 |
|------|------|------|
| `-h` | 显示帮助 | 列出所有选项 |
| `-V` | 显示版本 | 输出 fusermount 版本号 |
| `-o <options>` | 挂载选项 | 逗号分隔的选项列表（见下方详解） |
| `-u` | ★ 卸载 | 卸载指定挂载点的 FUSE 文件系统 |
| `-z` | ★ 懒卸载（lazy） | 先从命名空间移除，待无引用后真正卸载（等价 `umount -l`） |
| `-q` | 安静模式 | 不输出警告信息 |

#### `-o` 选项详解

```bash
# 通用选项（fusermount 和 mount -t fuse 通用）
fusermount -o allow_other /mnt/myfs       # 允许其他用户访问
fusermount -o allow_root /mnt/myfs        # 允许 root 访问（需 user_allow_other 配置）
fusermount -o nonempty /mnt/myfs          # 允许挂载到非空目录
fusermount -o ro /mnt/myfs                # 只读挂载
fusermount -o rw /mnt/myfs                # 读写挂载（默认）
fusermount -o suid /mnt/myfs              # 允许 SUID/SGID
fusermount -o nosuid /mnt/myfs            # 禁止 SUID/SGID（默认）
fusermount -o dev /mnt/myfs               # 允许设备文件
fusermount -o nodev /mnt/myfs             # 禁止设备文件（默认）
fusermount -o exec /mnt/myfs              # 允许执行程序
fusermount -o noexec /mnt/myfs            # 禁止执行程序
fusermount -o atime /mnt/myfs             # 更新访问时间
fusermount -o noatime /mnt/myfs           # 不更新访问时间
fusermount -o dirsync /mnt/myfs           # 目录同步写入
fusermount -o sync /mnt/myfs              # 同步 I/O
fusermount -o async /mnt/myfs             # 异步 I/O（默认）

# FUSE 特有选项
fusermount -o fsname=NAME /mnt/myfs       # 设置文件系统名称（df/mount 中显示）
fusermount -o subtype=TYPE /mnt/myfs      # 设置子类型（mount 显示为 fuse.TYPE）
fusermount -o ★ max_read=N /mnt/myfs        # 最大读大小（字节）
fusermount -o max_readahead=N /mnt/myfs   # 最大预读大小（字节）
fusermount -o umask=M /mnt/myfs           # 设置文件权限掩码
fusermount -o uid=N /mnt/myfs             # 设置文件所有者
fusermount -o gid=N /mnt/myfs             # 设置文件所属组
fusermount -o ★ entry_timeout=T /mnt/myfs   # 目录项缓存超时（秒）
fusermount -o ★ negative_timeout=T /mnt/myfs # 负缓存超时（不存在的文件）
fusermount -o ★ attr_timeout=T /mnt/myfs    # 属性缓存超时（秒）
fusermount -o ac_attr_timeout=T /mnt/myfs # autocache 属性超时
fusermount -o ★ auto_unmount /mnt/myfs      # 进程退出时自动卸载

# FUSE 3.x 新增选项
fusermount3 -o auto_unmount /mnt/myfs     # 同上
fusermount3 -o modules=M /mnt/myfs        # 加载模块（如 subdir, iconv）
fusermount3 -o ★ clone_fd /mnt/myfs         # 克隆 /dev/fuse fd（多线程优化）
```

### 1.2 mount -t fuse 的用法

```bash
# 基本语法
mount -t fuse.<subtype> <source> <mountpoint> -o <options>

# 示例：挂载 sshfs
mount -t fuse.sshfs user@host:/remote /mnt/sshfs -o allow_other

# 示例：挂载自定义 FUSE 文件系统
mount -t fuse /usr/local/bin/myfs /mnt/myfs -o allow_other,nonempty

# 示例：使用 fuseblk 子类型（如 ntfs-3g）
mount -t fuseblk /dev/sda1 /mnt/ntfs -o allow_other

# 等价于 fusermount 的方式
mount -t fuse myfs /mnt/myfs -o fsname=myfs,allow_other
```

**mount -t fuse vs fusermount 的区别：**

| 特性 | `mount -t fuse` | `fusermount` |
|------|----------------|-------------|
| 权限 | ★ 需要 root | ★ setuid，普通用户可用 |
| 自动创建守护进程 | 否（需要守护进程已运行） | 否（需要守护进程自行挂载） |
| ★ 典型使用场景 | ★ /etc/fstab 中自动挂载 | ★ 手动挂载/卸载 |
| 选项传递 | `-o` 后直接跟 | `-o` 后直接跟 |

### 1.3 自动挂载（/etc/fstab）

```bash
# /etc/fstab 格式
# <source>  <mountpoint>  <type>  <options>  <dump>  <pass>

# 挂载自定义 FUSE 文件系统
/usr/local/bin/myfs  /mnt/myfs  fuse  allow_other,auto  0  0

# 挂载 sshfs（需要密钥认证）
user@host:/remote  /mnt/sshfs  fuse.sshfs  defaults,allow_other,_netdev,IdentityFile=/root/.ssh/id_rsa  0  0

# 挂载 encfs 加密目录
/home/user/.encrypted  /home/user/decrypted  fuse.encfs  allow_other  0  0

# 挂载 ntfs-3g
/dev/sda1  /mnt/ntfs  fuseblk  allow_other,uid=1000,gid=1000  0  0
```

**卸载命令汇总：**

```bash
# 方法 1：fusermount
fusermount -u /mnt/myfs

# 方法 2：fusermount 懒卸载（如果方法 1 报 "device busy"）
fusermount -uz /mnt/myfs

# 方法 3：标准 umount
umount /mnt/myfs

# 方法 4：强制懒卸载
umount -l /mnt/myfs
```

---

## 二、FUSE 调试选项

### 2.1 前台与调试模式

```bash
# -f : 前台模式（foreground）
#   守护进程不 fork 到后台，日志输出到 stderr
#   ★ 调试时必加，否则看不到日志
./hello_fuse -f /mnt/myfs

# ★ -d : 调试模式（debug）
#   隐含 -f（前台），并输出 FUSE 协议级调试信息
#   包括每个 opcode 的参数和返回值
./hello_fuse -d /mnt/myfs

# 等价写法
./hello_fuse -f -o debug /mnt/myfs     # ★ -o debug 与 -d 等价
```

### 2.2 `-o debug` 的含义

`-o debug` 与 `-d` 等价，启用后 libfuse 会在 stderr 输出：

```
unique: 2, opcode: LOOKUP (1), nodeid: 1, insize: 48, pid: 1234
LOOKUP /hello
   NODEID: 2
   unique: 2, success, outsize: 144

unique: 3, opcode: GETATTR (3), nodeid: 2, insize: 56, pid: 1234
   unique: 3, success, outsize: 120

unique: 4, opcode: OPEN (14), nodeid: 2, insize: 48, pid: 1235
OPEN /hello
   unique: 4, success, outsize: 32

unique: 5, opcode: READ (15), nodeid: 2, insize: 80, pid: 1235
READ(3) 4096 bytes from 0
   unique: 5, success, outsize: 4112

unique: 6, opcode: RELEASE (18), nodeid: 2, insize: 64, pid: 1235
RELEASE 3
   unique: 6, success, outsize: 16
```

### 2.3 访问控制选项

| 选项 | 含义 | 安全影响 |
|------|------|---------|
| ★ `-o allow_other` | 允许挂载者以外的用户访问 | 需 `/etc/fuse.conf` 中设置 `user_allow_other` |
| `-o allow_root` | 仅允许 root 和挂载者访问 | 同上，比 `allow_other` 稍安全 |
| ★ 默认（不加） | 仅挂载者本人可访问 | ★ 最安全的默认行为 |

```bash
# 启用 allow_other 的前置条件
echo "user_allow_other" >> /etc/fuse.conf

# 然后才能使用
./myfs -o allow_other /mnt/myfs
```

### 2.4 其他常用调试/性能选项

| 选项 | 含义 | 典型用法 |
|------|------|---------|
| `-o nonempty` | 允许挂载到非空目录 | 默认禁止挂载到非空目录 |
| `-o max_read=N` | 限制单次最大读取字节数 | 调试时设小值观察拆包 |
| `-o max_write=N` | 限制单次最大写入字节数 | 同上 |
| ★ `-o writeback_cache` | 启用写回缓存 | 写性能优化（需内核 ≥ 3.14） |
| ★ `-o splice_read` | 启用 splice 读（零拷贝） | 大 I/O 性能优化 |
| ★ `-o splice_write` | 启用 splice 写（零拷贝） | 大 I/O 性能优化 |
| `-o no_splice_read` | 强制禁用 splice 读 | 调试/性能对比 |
| `-o no_splice_write` | 强制禁用 splice 写 | 调试/性能对比 |
| ★ `-o direct_io` | 绕过 page cache | 保证数据一致性 |
| ★ `-o kernel_cache` | 使用内核页缓存 | 减少重复读取 |
| `-o auto_cache` | 自动管理缓存有效性 | 基于 mtime 判断缓存 |
| `-o noforget` | 永不发送 FORGET | 调试 inode 生命周期 |
| `-o remember=T` | inode 缓存 T 秒 | 控制缓存过期 |
| `-o threads=N` | 多线程处理（FUSE 3.x） | 提高并发能力 |
| ★ `-o clone_fd` | 克隆 /dev/fuse fd | 多线程减少锁竞争 |

### 2.5 完整调试命令组合

```bash
# 最简调试：前台 + 调试输出
./myfs -d /mnt/myfs

# 完整调试：前台 + 调试 + 允许其他用户 + 非空目录
./myfs -f -o debug,allow_other,nonempty /mnt/myfs

# 性能调试：前台 + 禁用 splice + 小 max_write
./myfs -f -o debug,no_splice_read,no_splice_write,max_write=4096 /mnt/myfs

# 写回缓存调试
./myfs -f -o debug,writeback_cache /mnt/myfs
```

---

## 三、libfuse API 速查

### 3.1 fuse_main() 的参数

```c
// FUSE 2.x
int fuse_main(int argc, char *argv[],
              const struct fuse_operations *op,
              void *user_data);

// FUSE 3.x
int fuse_main(int argc, char *argv[],
              const struct fuse_operations *op,
              size_t op_size);
```

| 参数 | 含义 |
|------|------|
| `argc, argv` | 命令行参数（包含挂载点、-f、-d、-o 等） |
| `op` | `fuse_operations` 结构体指针（回调函数表） |
| `user_data` (2.x) | 传递给 `init` 回调的自定义数据 |
| `op_size` (3.x) | `sizeof(struct fuse_operations)`，用于版本兼容 |

**fuse_main() 内部流程：**

```
fuse_main(argc, argv, &ops, user_data)
  │
  ├── fuse_parse_cmdline()     解析命令行参数
  ├── fuse_mount()             打开 /dev/fuse，挂载文件系统
  ├── fuse_new()               创建 FUSE 会话，注册回调
  ├── fuse_daemonize()         后台化（未指定 -f 时）
  ├── fuse_loop()              单线程事件循环
  │     或
  │   fuse_loop_mt()           多线程事件循环（-o threads=N）
  ├── fuse_unmount()           卸载
  └── fuse_destroy()           清理资源
```

### 3.2 fuse_operations 各回调函数一览表

```c
struct fuse_operations {
    // === 文件属性 ===
    int (*getattr)(const char *, struct stat *);              // stat()
    int (*setattr)(const char *, struct stat *, int,
                   struct fuse_file_info *);                  // chmod/truncate
    int (*access)(const char *, int);                         // access()

    // === 文件 I/O ===
    int (*open)(const char *, struct fuse_file_info *);       // open()
    int (*read)(const char *, char *, size_t, off_t,
                struct fuse_file_info *);                     // read()
    int (*write)(const char *, const char *, size_t, off_t,
                 struct fuse_file_info *);                    // write()
    int (*release)(const char *, struct fuse_file_info *);    // close()
    int (*flush)(const char *, struct fuse_file_info *);      // close() 内部
    int (*fsync)(const char *, int, struct fuse_file_info *); // fsync()
    int (*fallocate)(const char *, int, off_t, off_t,
                     struct fuse_file_info *);                // fallocate()

    // === 目录操作 ===
    int (*opendir)(const char *, struct fuse_file_info *);    // opendir()
    int (*readdir)(const char *, void *, fuse_fill_dir_t,
                   off_t, struct fuse_file_info *);           // readdir()
    int (*releasedir)(const char *, struct fuse_file_info *); // closedir()
    int (*mkdir)(const char *, mode_t);                       // mkdir()
    int (*rmdir)(const char *);                               // rmdir()

    // === 文件创建/删除 ===
    int (*create)(const char *, mode_t,
                  struct fuse_file_info *);                   // open(O_CREAT)
    int (*mknod)(const char *, mode_t, dev_t);                // mknod()
    int (*unlink)(const char *);                              // unlink()
    int (*rename)(const char *, const char *);                // rename()
    int (*link)(const char *, const char *);                  // link()
    int (*symlink)(const char *, const char *);               // symlink()
    int (*readlink)(const char *, char *, size_t);            // readlink()

    // === 文件系统信息 ===
    int (*statfs)(const char *, struct statvfs *);            // statfs()/df

    // === 扩展属性 ===
    int (*setxattr)(const char *, const char *, const char *,
                    size_t, int);                             // setxattr()
    int (*getxattr)(const char *, const char *, char *,
                    size_t);                                  // getxattr()
    int (*listxattr)(const char *, char *, size_t);           // listxattr()
    int (*removexattr)(const char *, const char *);           // removexattr()

    // === 文件锁 ===
    int (*lock)(const char *, struct fuse_file_info *,
                int, struct flock *);                         // fcntl(F_SETLK)

    // === 生命周期 ===
    void *(*init)(struct fuse_conn_info *);                   // 挂载时初始化
    void (*destroy)(void *);                                  // 卸载时清理

    // === 其他 ===
    int (*ioctl)(const char *, int, void *,
                 struct fuse_file_info *, unsigned int, void *); // ioctl()
    int (*poll)(const char *, struct fuse_file_info *,
                struct fuse_pollhandle *, unsigned *);         // poll()
    int (*write_buf)(const char *, struct fuse_bufvec *,
                     off_t, struct fuse_file_info *);          // 零拷贝写
    int (*read_buf)(const char *, struct fuse_bufvec **,
                    size_t, off_t, struct fuse_file_info *);   // 零拷贝读
    int (*flock)(const char *, struct fuse_file_info *, int);  // flock()
    int (*copy_file_range)(const char *, struct fuse_file_info *,
                           off_t, const char *,
                           struct fuse_file_info *, off_t,
                           size_t, int);                       // 服务端拷贝
};
```

### 3.3 fuse_lowlevel_ops 各回调函数一览表

```c
struct fuse_lowlevel_ops {
    // === 生命周期 ===
    void (*init)(void *, struct fuse_conn_info *);            // 初始化
    void (*destroy)(void *);                                  // 销毁

    // === 查找与属性 ===
    void (*lookup)(fuse_req_t, fuse_ino_t, const char *);     // 查找文件
    void (*forget)(fuse_req_t, fuse_ino_t, uint64_t);         // 释放 inode
    void (*getattr)(fuse_req_t, fuse_ino_t,
                    struct fuse_file_info *);                 // 获取属性
    void (*setattr)(fuse_req_t, fuse_ino_t, struct stat *,
                    int, struct fuse_file_info *);            // 设置属性
    void (*access)(fuse_req_t, fuse_ino_t, int);              // 权限检查
    void (*readlink)(fuse_req_t, fuse_ino_t);                 // 读链接

    // === 文件 I/O ===
    void (*open)(fuse_req_t, fuse_ino_t,
                 struct fuse_file_info *);                    // 打开文件
    void (*read)(fuse_req_t, fuse_ino_t, size_t, off_t,
                 struct fuse_file_info *);                    // 读数据
    void (*write)(fuse_req_t, fuse_ino_t, const char *,
                  size_t, off_t, struct fuse_file_info *);    // 写数据
    void (*flush)(fuse_req_t, fuse_ino_t,
                  struct fuse_file_info *);                   // 刷新
    void (*release)(fuse_req_t, fuse_ino_t,
                    struct fuse_file_info *);                 // 关闭
    void (*fsync)(fuse_req_t, fuse_ino_t, int,
                  struct fuse_file_info *);                   // 同步

    // === 目录操作 ===
    void (*opendir)(fuse_req_t, fuse_ino_t,
                    struct fuse_file_info *);                 // 打开目录
    void (*readdir)(fuse_req_t, fuse_ino_t, size_t, off_t,
                    struct fuse_file_info *);                 // 读目录
    void (*readdirplus)(fuse_req_t, fuse_ino_t, size_t,
                        off_t, struct fuse_file_info *);      // 读目录+属性
    void (*releasedir)(fuse_req_t, fuse_ino_t,
                       struct fuse_file_info *);              // 关闭目录

    // === 文件创建 ===
    void (*mknod)(fuse_req_t, fuse_ino_t, const char *,
                  mode_t, dev_t);                             // 创建节点
    void (*mkdir)(fuse_req_t, fuse_ino_t, const char *,
                  mode_t);                                    // 创建目录
    void (*unlink)(fuse_req_t, fuse_ino_t, const char *);     // 删除文件
    void (*rmdir)(fuse_req_t, fuse_ino_t, const char *);      // 删除目录
    void (*rename)(fuse_req_t, fuse_ino_t, const char *,
                   fuse_ino_t, const char *);                 // 重命名
    void (*link)(fuse_req_t, fuse_ino_t, fuse_ino_t,
                 const char *);                               // 硬链接
    void (*symlink)(fuse_req_t, const char *, fuse_ino_t,
                    const char *);                            // 符号链接
    void (*create)(fuse_req_t, fuse_ino_t, const char *,
                   mode_t, struct fuse_file_info *);          // 创建并打开

    // === 其他 ===
    void (*statfs)(fuse_req_t, fuse_ino_t);                   // 文件系统信息
    void (*setxattr)(fuse_req_t, fuse_ino_t, const char *,
                     const char *, size_t, int);              // 设置 xattr
    void (*getxattr)(fuse_req_t, fuse_ino_t, const char *,
                     size_t);                                 // 获取 xattr
    void (*listxattr)(fuse_req_t, fuse_ino_t, size_t);        // 列举 xattr
    void (*removexattr)(fuse_req_t, fuse_ino_t, const char *); // 删除 xattr
    void (*getlk)(fuse_req_t, fuse_ino_t,
                  struct fuse_file_info *, struct flock *);   // 获取锁
    void (*setlk)(fuse_req_t, fuse_ino_t,
                  struct fuse_file_info *, struct flock *, int); // 设置锁
    void (*bmap)(fuse_req_t, fuse_ino_t, size_t, uint64_t);  // 块映射
    void (*ioctl)(fuse_req_t, fuse_ino_t, int, void *,
                  struct fuse_file_info *, unsigned,
                  const void *, size_t, size_t);              // ioctl
    void (*poll)(fuse_req_t, fuse_ino_t,
                 struct fuse_file_info *,
                 struct fuse_pollhandle *);                   // poll
    void (*write_buf)(fuse_req_t, fuse_ino_t,
                      struct fuse_bufvec *, off_t,
                      struct fuse_file_info *);               // 零拷贝写
    void (*retrieve_reply)(fuse_req_t, void *, fuse_ino_t,
                           off_t, struct fuse_bufvec *);      // retrieve 响应
    void (*forget_multi)(fuse_req_t, size_t,
                         struct fuse_forget_data *);          // 批量释放
    void (*flock)(fuse_req_t, fuse_ino_t,
                  struct fuse_file_info *, int);              // flock
    void (*fallocate)(fuse_req_t, fuse_ino_t, int,
                      off_t, off_t, struct fuse_file_info *); // fallocate
    void (*copy_file_range)(fuse_req_t, fuse_ino_t,
                            struct fuse_file_info *, off_t,
                            fuse_ino_t, struct fuse_file_info *,
                            off_t, size_t, int);              // 服务端拷贝
    void (*lseek)(fuse_req_t, fuse_ino_t, off_t, int,
                  struct fuse_file_info *);                   // seek
};
```

### 3.4 highlevel vs lowlevel API 选择指南

```
┌─────────────────────┬──────────────────────┬───────────────────────────┐
│ 特性                 │ Highlevel API         │ Lowlevel API              │
├─────────────────────┼──────────────────────┼───────────────────────────┤
│ 标识方式             │ 路径 (const char*)    │ inode 号 (fuse_ino_t)     │
│ 路径解析             │ libfuse 自动管理      │ 用户自行管理               │
│ inode 生命周期       │ libfuse 自动管理      │ 用户处理 FORGET            │
│ 目录缓存             │ 由 libfuse 控制       │ 用户完全控制               │
│ ★ 性能                 │ ★ 较低（路径查找开销）   │ ★ 较高（直接 inode 操作）     │
│ 实现复杂度           │ 低（5-10 个回调）      │ 高（需管理 inode 表）       │
│ ★ 适用场景             │ ★ 简单文件系统、原型     │ ★ 高性能/生产级文件系统       │
│ 代表项目             │ hello.c, sshfs        │ CephFS FUSE, GlusterFS    │
│ 并发能力             │ 受限（路径锁）         │ 灵活（inode 级并发）        │
│ readdirplus 支持     │ 自动（可选）           │ 需手动实现                 │
│ 零拷贝 (bufvec)      │ 部分支持              │ 完整支持                   │
└─────────────────────┴──────────────────────┴───────────────────────────┘

选择建议：
  - ★ 学习 FUSE / 写原型 / 简单工具 → Highlevel API
  - ★ 生产级文件系统 / 需要高性能 → Lowlevel API
  - ★ 不确定时 → 先用 Highlevel，性能不够再迁移到 Lowlevel
```

---

## 四、编译命令

### 4.1 gcc + pkg-config 编译

```bash
# === FUSE 2.x（fuse-devel）===

# 获取编译标志
pkg-config fuse --cflags     # 输出：-D_FILE_OFFSET_BITS=64 -I/usr/include/fuse
pkg-config fuse --libs       # 输出：-lfuse -lpthread

# 一步编译
gcc -o hello_fuse hello_fuse.c $(pkg-config fuse --cflags --libs)

# 分步查看
gcc -o hello_fuse hello_fuse.c -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lfuse -lpthread

# === FUSE 3.x（fuse3-devel）===

# 获取编译标志
pkg-config fuse3 --cflags    # 输出：-D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3
pkg-config fuse3 --libs      # 输出：-lfuse3 -lpthread

# 一步编译
gcc -o hello_fuse3 hello_fuse3.c $(pkg-config fuse3 --cflags --libs)
```

### 4.2 CMake 方式

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(myfuse C)

# 查找 FUSE 包
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE REQUIRED fuse)    # FUSE 2.x
# pkg_check_modules(FUSE REQUIRED fuse3) # FUSE 3.x

# 添加可执行文件
add_executable(myfs myfs.c)

# 链接 FUSE
target_include_directories(myfs PRIVATE ${FUSE_INCLUDE_DIRS})
target_compile_options(myfs PRIVATE ${FUSE_CFLAGS_OTHER})
target_link_libraries(myfs ${FUSE_LIBRARIES})

# 必须定义文件偏移量宏
target_compile_definitions(myfs PRIVATE _FILE_OFFSET_BITS=64)
```

```bash
# 编译
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 4.3 Makefile 方式

```makefile
# Makefile
CC = gcc
CFLAGS = -Wall -Wextra -g -D_FILE_OFFSET_BITS=64
FUSE_CFLAGS = $(shell pkg-config fuse --cflags)
FUSE_LIBS = $(shell pkg-config fuse --libs)

# FUSE 3.x 版本：
# FUSE_CFLAGS = $(shell pkg-config fuse3 --cflags)
# FUSE_LIBS = $(shell pkg-config fuse3 --libs)

all: hello_fuse

hello_fuse: hello_fuse.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)

clean:
	rm -f hello_fuse

.PHONY: all clean
```

```bash
make
```

### 4.4 编译常见问题

| 问题 | 原因 | 解决方案 |
|------|------|---------|
| `fatal error: fuse.h: No such file` | 缺少开发包 | `yum install fuse-devel` |
| `undefined reference to 'fuse_main'` | 未链接 libfuse | 添加 `-lfuse` |
| `warning: implicit declaration of 'fuse_main'` | 缺少 `_FILE_OFFSET_BITS` | 添加 `-D_FILE_OFFSET_BITS=64` |
| `fuse.h uses 64-bit offsets` 错误 | 32/64 位偏移不匹配 | 确保定义 `_FILE_OFFSET_BITS=64` |
| 运行时 `fusermount: option allow_other only allowed if 'user_allow_other'` | 配置限制 | 编辑 `/etc/fuse.conf` |

---

## 五、常用命令组合

### 场景 1：编译运行 hello FUSE

```bash
# 安装依赖
yum install -y fuse-devel fuse

# 编译
gcc -o /tmp/hello_fuse docs/fuse/reading/03_hello_fuse.c \
    $(pkg-config fuse --cflags --libs)

# 创建挂载点并挂载
mkdir -p /tmp/myfuse
/tmp/hello_fuse -f /tmp/myfuse &
sleep 1

# 测试
ls -la /tmp/myfuse
cat /tmp/myfuse/hello
stat /tmp/myfuse/hello

# 卸载
fusermount -u /tmp/myfuse
```

### 场景 2：调试模式运行（前台 + debug）

```bash
# 调试模式：-d 隐含 -f，输出 FUSE 协议级日志
/tmp/hello_fuse -d /tmp/myfuse &
FUSE_PID=$!
sleep 1

# 在另一个终端操作（触发 FUSE 请求）
ls /tmp/myfuse
cat /tmp/myfuse/hello

# 观察 stderr 中的调试输出：
# unique: 2, opcode: LOOKUP (1), nodeid: 1, insize: 48
# LOOKUP /hello → NODEID: 2
# unique: 3, opcode: GETATTR (3), nodeid: 2, insize: 56
# unique: 4, opcode: OPEN (14), nodeid: 2, insize: 48
# unique: 5, opcode: READ (15), nodeid: 2, insize: 80
# READ(0) 4096 bytes from 0
# unique: 6, opcode: RELEASE (18), nodeid: 2, insize: 64

# 卸载
fusermount -u /tmp/myfuse
```

### 场景 3：用 strace 追踪 FUSE 的 /dev/fuse 通信

```bash
# 启动 FUSE 守护进程
/tmp/hello_fuse -f /tmp/myfuse &
FUSE_PID=$!
sleep 1

# strace 追踪 FUSE 守护进程的 read/write 系统调用
# -f   = 追踪子线程
# -e   = 只追踪 read,write,ioctl
# -T   = 显示每个系统调用的耗时
# -s   = 增大字符串输出长度
strace -f -e trace=read,write,ioctl -T -s 256 -p $FUSE_PID 2>/tmp/fuse_strace.log &
sleep 1

# 触发文件操作
cat /tmp/myfuse/hello

# 查看 strace 输出
cat /tmp/fuse_strace.log

# 典型输出：
# read(3, "\x50\x00\x00\x00\x01\x00..." , 131072) = 80 <0.000025>
#   → 从 /dev/fuse(fd=3) 读取 80 字节的 LOOKUP 请求，耗时 25μs
# write(3, "\x90\x00\x00\x00\x00\x00..." , 144) = 144 <0.000012>
#   → 向 /dev/fuse 写回 144 字节的 LOOKUP 响应，耗时 12μs

# 卸载
fusermount -u /tmp/myfuse
```

### 场景 4：用 blktrace 追踪 FUSE 文件系统的底层 I/O

```bash
# 前提：FUSE 文件系统底层使用块设备存储（如 /dev/vdb 上的 ext4）
# 本场景演示 FUSE 守护进程对底层设备的 I/O

# 先在 /dev/vdb 上创建 ext4 并挂载
mkfs.ext4 -F /dev/vdb
mkdir -p /mnt/ext4_base
mount /dev/vdb /mnt/ext4_base

# 创建一个基于 ext4 的简单 FUSE 透传文件系统（bindfs）
yum install -y bindfs
mkdir -p /tmp/fuse_overlay
bindfs /mnt/ext4_base /tmp/fuse_overlay

# 启动 blktrace 追踪底层设备
blktrace -d /dev/vdb -o fuse_blk -w 10 &
sleep 1

# 在 FUSE 挂载点上执行 I/O
dd if=/dev/zero of=/tmp/fuse_overlay/test.dat bs=4k count=100 conv=fsync

# 等待 blktrace 结束
wait

# 解析
blkparse -i fuse_blk -o fuse_blk_parsed.txt

# 观察 FUSE 守护进程产生的 block I/O
awk '{print $6}' fuse_blk_parsed.txt | sort | uniq -c | sort -rn
# 重点看事件类型分布和 rwbs 字段

# 卸载
fusermount -u /tmp/fuse_overlay
umount /mnt/ext4_base
```

### 场景 5：FUSE vs ext4 性能对比（用 fio）

```bash
# 准备 ext4 基准目录
mkfs.ext4 -F /dev/vdb
mkdir -p /mnt/ext4_base
mount /dev/vdb /mnt/ext4_base

# 准备 FUSE 挂载
bindfs /mnt/ext4_base /tmp/fuse_overlay

# === ext4 基准测试 ===
fio --name=ext4_test \
    --directory=/mnt/ext4_base \
    --ioengine=libaio --direct=1 \
    --rw=randwrite --bs=4k \
    --size=100M --numjobs=1 \
    --runtime=10 --time_based \
    --output=/tmp/ext4_result.txt

# === FUSE 性能测试 ===
fio --name=fuse_test \
    --directory=/tmp/fuse_overlay \
    --ioengine=libaio --direct=1 \
    --rw=randwrite --bs=4k \
    --size=100M --numjobs=1 \
    --runtime=10 --time_based \
    --output=/tmp/fuse_result.txt

# === 对比结果 ===
echo "=== ext4 IOPS ==="
grep "iops" /tmp/ext4_result.txt | head -3
echo "=== FUSE IOPS ==="
grep "iops" /tmp/fuse_result.txt | head -3

# 预期：★ FUSE 的 IOPS 下降 30-70%，延迟增加 2-5 倍
# 原因：★ 每次 I/O 需要 2 次额外的上下文切换（内核↔用户态）

# 清理
fusermount -u /tmp/fuse_overlay
umount /mnt/ext4_base
```

### 场景 6：fio 测试 FUSE 挂载点

```bash
# 测试 FUSE 挂载点的多种 I/O 模式

# 随机读
fio --name=fuse_randread \
    --directory=/tmp/fuse_overlay \
    --ioengine=sync --rw=randread --bs=4k \
    --size=50M --numjobs=1 \
    --runtime=10 --time_based

# 顺序读
fio --name=fuse_seqread \
    --directory=/tmp/fuse_overlay \
    --ioengine=sync --rw=read --bs=128k \
    --size=100M --numjobs=1 \
    --runtime=10 --time_based

# 随机写
fio --name=fuse_randwrite \
    --directory=/tmp/fuse_overlay \
    --ioengine=sync --rw=randwrite --bs=4k \
    --size=50M --numjobs=1 \
    --runtime=10 --time_based

# 顺序写
fio --name=fuse_seqwrite \
    --directory=/tmp/fuse_overlay \
    --ioengine=sync --rw=write --bs=128k \
    --size=100M --numjobs=1 \
    --runtime=10 --time_based

# 混合读写（70/30）
fio --name=fuse_randrw \
    --directory=/tmp/fuse_overlay \
    --ioengine=sync --rw=randrw --rwmixread=70 --bs=4k \
    --size=50M --numjobs=1 \
    --runtime=10 --time_based
```

### 场景 7：writeback_cache 性能优化

```bash
# 挂载 FUSE（不启用 writeback_cache）
bindfs /mnt/ext4_base /tmp/fuse_no_wbc

# 挂载 FUSE（启用 writeback_cache）
bindfs /mnt/ext4_base /tmp/fuse_wbc -o writeback_cache

# === 无 writeback_cache 的写性能 ===
fio --name=no_wbc \
    --directory=/tmp/fuse_no_wbc \
    --ioengine=sync --rw=randwrite --bs=4k \
    --size=50M --numjobs=1 \
    --runtime=10 --time_based \
    --output=/tmp/no_wbc_result.txt

# === 有 writeback_cache 的写性能 ===
fio --name=with_wbc \
    --directory=/tmp/fuse_wbc \
    --ioengine=sync --rw=randwrite --bs=4k \
    --size=50M --numjobs=1 \
    --runtime=10 --time_based \
    --output=/tmp/wbc_result.txt

# === 对比 ===
echo "=== 无 writeback_cache ==="
grep "write.*iops" /tmp/no_wbc_result.txt
echo "=== 有 writeback_cache ==="
grep "write.*iops" /tmp/wbc_result.txt

# 预期：★ writeback_cache 可将写 IOPS 提升 3-10 倍
# 原因：write() 直接写入 page cache，内核异步批量刷写

# 清理
fusermount -u /tmp/fuse_no_wbc
fusermount -u /tmp/fuse_wbc
```

### 场景 8：多 FUSE 文件系统叠加（FUSE on FUSE）

```bash
# 演示多层 FUSE 叠加的性能衰减

# 第 1 层：ext4 → FUSE
bindfs /mnt/ext4_base /tmp/fuse_layer1

# 第 2 层：FUSE → FUSE
bindfs /tmp/fuse_layer1 /tmp/fuse_layer2

# 第 3 层：FUSE → FUSE → FUSE
bindfs /tmp/fuse_layer2 /tmp/fuse_layer3

# 分别在各层上运行 fio
for layer in /mnt/ext4_base /tmp/fuse_layer1 /tmp/fuse_layer2 /tmp/fuse_layer3; do
    echo "=== Testing: $layer ==="
    fio --name=layer_test \
        --directory=$layer \
        --ioengine=sync --rw=randwrite --bs=4k \
        --size=20M --numjobs=1 \
        --runtime=5 --time_based 2>&1 | grep "iops"
done

# 预期结果：★ 每叠加一层 FUSE，IOPS 下降约 30-50%
# 因为每层增加 2 次上下文切换
#
# ext4 直接：    ★ ~50,000 IOPS
# FUSE 1 层：    ★ ~20,000 IOPS  (↓ 60%)
# FUSE 2 层：    ★ ~10,000 IOPS  (↓ 80%)
# FUSE 3 层：    ★ ~6,000 IOPS   (↓ 88%)

# 清理（从最内层开始卸载）
fusermount -u /tmp/fuse_layer3
fusermount -u /tmp/fuse_layer2
fusermount -u /tmp/fuse_layer1
```

---

## 六、FUSE 相关的内核参数

### 6.1 /sys/fs/fuse/connections/

每个活跃的 FUSE 连接在此目录下有一个子目录：

```bash
# 列出所有 FUSE 连接
ls /sys/fs/fuse/connections/
# 输出示例：39  41  43

# 查看某个连接的信息
ls /sys/fs/fuse/connections/39/
# 文件列表：
# abort        — 写入 1 可强制终止连接
# congestion_threshold — 拥塞阈值（后台请求超过此数时内核开始等待）
# max_background — 最大后台请求数
# waiting      — 当前等待用户态响应的请求数
```

| 文件 | 含义 | 典型值 |
|------|------|--------|
| `waiting` | 等待用户态处理的请求数 | 0（正常）/ >0（积压） |
| `max_background` | 最大后台请求数 | ★ 12（默认） |
| `congestion_threshold` | 拥塞阈值 | ★ 9（默认，= max_background × 75%） |
| `abort` | 强制终止连接 | 写入 `1` 触发 |

```bash
# 查看所有 FUSE 连接的等待请求数
for conn in /sys/fs/fuse/connections/*/; do
    echo "Connection $(basename $conn): waiting=$(cat $conn/waiting)"
done

# 查看最大后台请求数
cat /sys/fs/fuse/connections/*/max_background

# 紧急中止一个挂死的 FUSE 连接
echo 1 > /sys/fs/fuse/connections/39/abort
```

### 6.2 /proc/sys/fs/fuse/ 相关参数

```bash
# FUSE 模块参数（挂载后生效）
ls /proc/sys/fs/ 2>/dev/null | grep fuse
# 注意：FUSE 模块可能不在 /proc/sys/fs/ 下暴露参数

# 可通过 /sys/module/fuse/parameters/ 查看模块参数
ls /sys/module/fuse/parameters/ 2>/dev/null
```

### 6.3 /sys/module/fuse/ 参数

```bash
# 查看 FUSE 内核模块信息
ls /sys/module/fuse/
# 目录结构：
# coresize  holders/  initsize  notes/       refcnt  taint
# drivers/  initstate  modules/  parameters/  srcversion  uevent  version

# 查看模块版本
cat /sys/module/fuse/version

# 查看模块参数（如果有）
ls /sys/module/fuse/parameters/
# 可能为空（FUSE 模块不一定暴露运行时参数）

# 查看模块引用计数（当前挂载的 FUSE 文件系统数量相关）
cat /sys/module/fuse/refcnt

# 查看模块状态
cat /sys/module/fuse/initstate
# 输出：live
```

### 6.4 /etc/fuse.conf 配置文件

```bash
# /etc/fuse.conf 内容
cat /etc/fuse.conf
# 典型内容：
# # Set the maximum number of FUSE mounts allowed to non-root users.
# #mount_max = 10
#
# # Allow non-root users to specify the allow_other or allow_root mount options.
# #user_allow_other

# 启用 allow_other 功能
echo "user_allow_other" | tee -a /etc/fuse.conf

# 设置非 root 用户最大挂载数
echo "mount_max = 10" | tee -a /etc/fuse.conf
```

### 6.5 内核 FUSE 源码中的关键常量

```c
// src/linux-5.10/fs/fuse/fuse_i.h

// 默认最大后台请求数
#define FUSE_DEFAULT_MAX_BACKGROUND     ★ 12

// 默认拥塞阈值（= max_background × 75%）
#define FUSE_DEFAULT_CONGESTION_THRESHOLD  (FUSE_DEFAULT_MAX_BACKGROUND * 75 / 100)

// FUSE 请求最大页数
#define FUSE_MAX_MAX_PAGES              ★ 256   // 256 × 4KB = 1MB

// 默认每请求最大页数
#define FUSE_DEFAULT_MAX_PAGES_PER_REQ  ★ 32    // 32 × 4KB = 128KB

// /dev/fuse 最小读缓冲区
#define FUSE_MIN_READ_BUFFER            ★ 8192  // 8KB
```

---

## 七、源码文件快速索引

### 按功能查找源码

```bash
# 找 FUSE opcode 枚举定义
grep -n "enum fuse_opcode" src/linux-5.10/include/uapi/linux/fuse.h

# 找 fuse_in_header（请求包头）
grep -n "struct fuse_in_header" src/linux-5.10/include/uapi/linux/fuse.h

# 找内核 FUSE read 实现
grep -n "fuse_read_iter\|fuse_send_read" src/linux-5.10/fs/fuse/file.c

# 找 /dev/fuse 的读写实现
grep -n "fuse_dev_read\|fuse_dev_write" src/linux-5.10/fs/fuse/dev.c

# 找请求队列管理
grep -n "fuse_iqueue\|queue_request\|request_wait_answer" src/linux-5.10/fs/fuse/dev.c

# 找 FUSE mount 流程
grep -n "fuse_fill_super\|fuse_get_tree" src/linux-5.10/fs/fuse/inode.c

# 找 writeback_cache 实现
grep -n "FUSE_WRITEBACK_CACHE\|writeback" src/linux-5.10/fs/fuse/inode.c

# 找 splice 实现
grep -n "fuse_dev_splice" src/linux-5.10/fs/fuse/dev.c

# 找 FUSE 连接管理（/sys/fs/fuse/connections/）
grep -n "fuse_ctl\|connections" src/linux-5.10/fs/fuse/control.c

# 找 INIT 协商
grep -n "FUSE_INIT\|fuse_send_init" src/linux-5.10/fs/fuse/inode.c

# 找 INIT flags 定义
grep -n "FUSE_ASYNC_READ\|FUSE_WRITEBACK_CACHE\|FUSE_SPLICE" \
    src/linux-5.10/include/uapi/linux/fuse.h
```

---

## 八、诊断流程速查

```
1. FUSE 挂载失败
   fusermount -f /mnt/myfs 2>&1
   → 检查 /etc/fuse.conf（allow_other）
   → 检查挂载点是否存在、是否非空（nonempty）
   → 检查 /dev/fuse 是否存在（modprobe fuse）

2. FUSE 文件系统无响应
   cat /sys/fs/fuse/connections/*/waiting
   → waiting > 0 表示请求积压，守护进程可能挂死
   → echo 1 > /sys/fs/fuse/connections/N/abort 强制中止

3. FUSE 性能差
   strace -p $(pidof fuse_daemon) -T -c
   → 查看系统调用耗时分布
   → 检查是否启用 writeback_cache / splice

4. FUSE I/O 错误
   fio --directory=/mnt/fuse --rw=randread --bs=4k --size=10M
   → 观察 I/O 错误信息
   → strace 追踪守护进程的底层 I/O

5. FUSE 卸载失败（device busy）
   fusermount -uz /mnt/myfs       # 懒卸载
   fuser -m /mnt/myfs             # 查看谁在使用
   lsof +D /mnt/myfs              # 列出打开的文件
```
