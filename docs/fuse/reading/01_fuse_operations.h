/*
 * ======================================================================
 * Step 1: fuse_operations — FUSE 用户态文件系统的"虚函数表"
 * ======================================================================
 *
 * 原始路径：libfuse/include/fuse.h（struct fuse_operations 定义）
 * 参考来源：https://github.com/libfuse/libfuse
 *
 * 本文件是 FUSE 用户态文件系统的核心接口定义。
 * 每一个 FUSE 文件系统都需要填充这个结构体，告诉 libfuse：
 *   "当 VFS 调用 getattr 时，执行我的 my_getattr 函数"
 *   "当 VFS 调用 read 时，执行我的 my_read 函数"
 *   ...以此类推
 *
 * 这与 Linux 内核中的 file_operations / inode_operations 异曲同工，
 * 区别在于：
 *   - 内核 file_operations 运行在内核态
 *   - fuse_operations 运行在用户态（FUSE 守护进程中）
 *
 * 阅读要点：
 *   1. 每个回调函数指针的含义和触发时机
 *   2. 返回值约定（0 成功、负数 errno 失败）
 *   3. 哪些回调是必须实现的，哪些是可选的
 *   4. struct fuse_file_info 中 fh 字段的作用
 *
 * 预计阅读时间：10 分钟
 *
 * ★ FUSE 2.x vs 3.x 签名差异：
 *   本文件以 FUSE 3.x API 为基准注释（libfuse 3.x 是现代推荐版本）。
 *   多数 Linux 发行版（CentOS/RHEL/Ubuntu LTS）默认安装 FUSE 2.x，
 *   以下回调签名在 2.x 中有差异：
 *
 *   | 回调      | FUSE 2.x 签名                      | FUSE 3.x 新增参数           |
 *   |-----------|------------------------------------|---------------------------|
 *   | getattr   | (path, stbuf)                      | + fi                      |
 *   | readdir   | (path, buf, filler, off, fi)       | + flags                   |
 *   | chmod     | (path, mode)                       | + fi                      |
 *   | chown     | (path, uid, gid)                   | + fi                      |
 *   | truncate  | (path, size)                       | + fi                      |
 *   | init      | (conn)                             | + cfg                     |
 *   | rename    | (oldpath, newpath)                 | + flags                   |
 *
 *   示例代码 reading/03_hello_fuse.c 使用 FUSE 2.x API 以确保可直接编译。
 * ======================================================================
 */

#ifndef FUSE_OPERATIONS_H
#define FUSE_OPERATIONS_H

#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>

/*
 * ★ fuse_file_info — 文件打开时的上下文信息
 *
 * 在 open/create/opendir 时传入，内核分配并在后续 read/write/release 中传递。
 * 最重要的字段是 fh（file handle），用户态文件系统可以用它存储任意私有数据。
 *
 * 类比：内核文件系统中的 struct file *filp->private_data
 */
struct fuse_file_info {
    int flags;              /* open 标志（O_RDONLY, O_WRONLY, O_RDWR 等） */
    unsigned long fh_old;   /* 旧版 file handle（兼容，不使用） */
    int writepage;          /* 写操作是否来自 page cache（writeback） */
    unsigned int direct_io : 1;     /* ★ 绕过 page cache（FOPEN_DIRECT_IO） */
    unsigned int keep_cache : 1;    /* 不使缓存失效（FOPEN_KEEP_CACHE） */
    unsigned int flush : 1;         /* flush 操作标记 */
    unsigned int nonseekable : 1;   /* 文件不可 seek */
    unsigned int flock_release : 1; /* flock 释放标记 */
    unsigned int cache_readdir : 1; /* 缓存 readdir 结果 */
    unsigned int padding : 27;
    uint64_t fh;            /* ★ 文件句柄——用户自定义的私有数据指针/句柄 */
    uint64_t lock_owner;    /* 锁拥有者标识 */
    uint32_t poll_events;   /* poll 事件掩码 */
};

/*
 * ★ fuse_conn_info — FUSE 连接协商信息
 *
 * 在 init 回调中传入，允许用户态文件系统与内核协商能力：
 *   - 协议版本
 *   - 最大读写大小
 *   - 是否启用 writeback cache
 *   - 是否支持异步读
 *
 * 类比：PCI 设备的 capability negotiation
 */
struct fuse_conn_info {
    unsigned proto_major;       /* 协议主版本号 */
    unsigned proto_minor;       /* 协议次版本号 */
    unsigned max_write;         /* ★ 最大写请求大小（字节，默认 128KB） */
    unsigned max_read;          /* 最大读请求大小 */
    unsigned max_readahead;     /* 最大预读大小 */
    unsigned capable;           /* ★ 内核支持的能力位掩码（只读） */
    unsigned want;              /* ★ 用户态请求的能力位掩码（可写） */
    unsigned max_background;    /* 最大后台请求数 */
    unsigned congestion_threshold; /* 拥塞阈值 */
    unsigned time_gran;         /* 时间精度（纳秒） */
    unsigned reserved[22];
};

/*
 * 常用 FUSE_CAP_* 能力标志（用于 capable / want 字段）：
 *
 * FUSE_CAP_ASYNC_READ       异步读
 * FUSE_CAP_WRITEBACK_CACHE  ★ 写回缓存（减少写操作的上下文切换）
 * FUSE_CAP_SPLICE_WRITE     使用 splice 写入 /dev/fuse
 * FUSE_CAP_SPLICE_READ      使用 splice 从 /dev/fuse 读取
 * FUSE_CAP_SPLICE_MOVE      splice 移动页面（零拷贝）
 * FUSE_CAP_BIG_WRITES       支持大于 4KB 的写请求
 * FUSE_CAP_AUTO_INVAL_DATA  自动使页缓存失效
 * FUSE_CAP_READDIRPLUS      支持 readdirplus（readdir + lookup 合并）
 */


/* ======================================================================
 * ★★ 核心结构体：struct fuse_operations
 * ======================================================================
 *
 * 这是每个 FUSE 文件系统的"入口表"。
 * 你在 main() 中填充这个结构体，然后传给 fuse_main()：
 *
 *   struct fuse_operations ops = {
 *       .getattr = my_getattr,
 *       .readdir = my_readdir,
 *       .open    = my_open,
 *       .read    = my_read,
 *   };
 *   fuse_main(argc, argv, &ops, NULL);
 *
 * ★ 返回值约定：
 *   - 返回 0 表示成功
 *   - 返回负数 errno 表示失败（如 -ENOENT, -EACCES）
 *   - 与内核 VFS 的返回值约定一致
 *
 * ★ NULL 回调的处理：
 *   如果某个回调为 NULL，libfuse 会返回 -ENOSYS（功能未实现）
 *   部分回调有默认行为（如 access 默认返回 0）
 */
struct fuse_operations {

    /* ==================================================================
     * 第一部分：文件属性操作
     * ================================================================== */

    /*
     * ★ getattr — 获取文件属性
     *
     * 触发时机：stat(), ls -l, access() 等任何需要文件属性的操作
     * 必须实现！这是最常用的回调之一。
     *
     * 参数：
     *   path  — 文件路径（如 "/hello"）
     *   stbuf — 输出参数，填充 struct stat（mode, size, uid, gid 等）
     *   fi    — 如果文件已打开，包含 fh（否则为 NULL）
     *
     * 返回值：0 成功，-errno 失败（常见 -ENOENT）
     *
     * 类比内核：inode_operations.getattr → generic_fillattr()
     */
    int (*getattr)(const char *path, struct stat *stbuf,
                   struct fuse_file_info *fi);

    /*
     * readlink — 读取符号链接目标
     *
     * 触发时机：readlink()
     * 如果不实现，readlink 返回 -ENOSYS
     *
     * 参数：
     *   path — 符号链接的路径
     *   buf  — 输出缓冲区
     *   size — 缓冲区大小
     */
    int (*readlink)(const char *path, char *buf, size_t size);

    /* ==================================================================
     * 第二部分：目录操作
     * ================================================================== */

    /*
     * mknod — 创建设备特殊文件
     *
     * 触发时机：mknod()
     * 大多数 FUSE 文件系统不需要实现（返回 -ENOSYS 即可）
     */
    int (*mknod)(const char *path, mode_t mode, dev_t rdev);

    /*
     * mkdir — 创建目录
     *
     * 触发时机：mkdir()
     * 参数：
     *   path — 新目录路径
     *   mode — 目录权限（如 0755）
     */
    int (*mkdir)(const char *path, mode_t mode);

    /*
     * unlink — 删除文件
     *
     * 触发时机：unlink(), remove()
     * 参数：
     *   path — 要删除的文件路径
     */
    int (*unlink)(const char *path);

    /*
     * rmdir — 删除目录
     *
     * 触发时机：rmdir()
     * 参数：
     *   path — 要删除的目录路径
     */
    int (*rmdir)(const char *path);

    /*
     * symlink — 创建符号链接
     *
     * 触发时机：symlink()
     * 参数：
     *   link   — 链接目标路径
     *   newpath — 符号链接本身的路径
     */
    int (*symlink)(const char *link, const char *newpath);

    /*
     * rename — 重命名文件/目录
     *
     * 触发时机：rename(), mv
     */
    int (*rename)(const char *oldpath, const char *newpath,
                  unsigned int flags);

    /*
     * link — 创建硬链接
     *
     * 触发时机：link()
     */
    int (*link)(const char *oldpath, const char *newpath);

    /* ==================================================================
     * 第三部分：文件权限操作
     * ================================================================== */

    /*
     * chmod — 修改文件权限
     *
     * 触发时机：chmod()
     */
    int (*chmod)(const char *path, mode_t mode,
                 struct fuse_file_info *fi);

    /*
     * chown — 修改文件所有者
     *
     * 触发时机：chown()
     */
    int (*chown)(const char *path, uid_t uid, gid_t gid,
                 struct fuse_file_info *fi);

    /*
     * truncate — 截断文件
     *
     * 触发时机：truncate(), open(O_TRUNC)
     * 参数：
     *   path — 文件路径
     *   size — 新大小（0 表示清空文件）
     */
    int (*truncate)(const char *path, off_t size,
                    struct fuse_file_info *fi);

    /* ==================================================================
     * 第四部分：★ 文件打开/读写/关闭（核心 I/O 路径）
     * ================================================================== */

    /*
     * ★ open — 打开文件
     *
     * 触发时机：open()
     * 这是 I/O 热路径的入口。
     *
     * 参数：
     *   path — 文件路径
     *   fi   — 文件信息（fi->flags 包含 O_RDONLY 等）
     *          ★ fi->fh 可以设置文件句柄，后续 read/write 中使用
     *
     * 返回值：
     *   0 = 打开成功
     *   如果设置 fi->direct_io = 1，则绕过 page cache
     *   如果设置 fi->keep_cache = 1，则不使已有缓存失效
     *
     * 类比内核：file_operations.open
     *
     * ★ 如果不需要特殊处理，可以设为 NULL（默认返回成功）
     */
    int (*open)(const char *path, struct fuse_file_info *fi);

    /*
     * ★★ read — 读取文件内容
     *
     * 触发时机：read(), pread()
     * 这是 FUSE 文件系统最核心的回调之一。
     *
     * 参数：
     *   path   — 文件路径
     *   buf    — 输出缓冲区（将数据拷贝到这里）
     *   size   — 请求读取的字节数
     *   offset — 文件偏移量
     *   fi     — 文件信息（fi->fh 包含 open 时设置的句柄）
     *
     * 返回值：
     *   实际读取的字节数（可以小于 size，表示读到文件末尾附近）
     *   0 = EOF
     *   -errno = 错误
     *
     * ★ 注意：与标准 read() 语义一致——不需要保证读满 size 字节
     *
     * 类比内核：file_operations.read_iter → generic_file_read_iter()
     */
    int (*read)(const char *path, char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi);

    /*
     * ★★ write — 写入文件内容
     *
     * 触发时机：write(), pwrite()
     *
     * 参数：
     *   path   — 文件路径
     *   buf    — 输入缓冲区（数据源）
     *   size   — 要写入的字节数
     *   offset — 文件偏移量
     *   fi     — 文件信息
     *
     * 返回值：实际写入的字节数，或 -errno
     *
     * ★ 重要：必须返回 size（表示全部写入成功），否则内核会认为写入了短数据
     *
     * 类比内核：file_operations.write_iter → generic_file_write_iter()
     */
    int (*write)(const char *path, const char *buf, size_t size,
                 off_t offset, struct fuse_file_info *fi);

    /*
     * statfs — 获取文件系统统计信息
     *
     * 触发时机：statfs(), df
     * 如果不实现，df 命令会报错但不影响正常使用
     */
    int (*statfs)(const char *path, struct statvfs *stbuf);

    /*
     * flush — 刷新文件缓冲
     *
     * 触发时机：close() 时（每次 close 都会调用，不同于 fsync）
     * 参数：
     *   path — 文件路径
     *   fi   — 文件信息
     *
     * ★ 注意：flush 在每次 close() 时调用（包括 dup 后的 close），
     *   而 fsync 只在显式 fsync() 时调用。
     *   大多数文件系统不需要实现 flush。
     */
    int (*flush)(const char *path, struct fuse_file_info *fi);

    /*
     * ★ release — 释放文件资源
     *
     * 触发时机：当文件的所有 fd 都关闭后（最后一次 close）
     * 与 flush 的区别：flush 每次 close 都调用，release 只调用一次
     *
     * 参数：
     *   path — 文件路径
     *   fi   — 文件信息（fi->fh 包含 open 时设置的句柄）
     *
     * ★ 用于释放 open 时分配的资源（如内存、fd 等）
     * 返回值被忽略（release 不能失败）
     *
     * 类比内核：file_operations.release
     */
    int (*release)(const char *path, struct fuse_file_info *fi);

    /*
     * fsync — 同步文件数据到存储
     *
     * 触发时机：fsync(), fdatasync()
     * 参数：
     *   path     — 文件路径
     *   datasync — 非零表示只同步数据（fdatasync），零表示同步数据+元数据
     */
    int (*fsync)(const char *path, int datasync,
                 struct fuse_file_info *fi);

    /* ==================================================================
     * 第五部分：扩展属性
     * ================================================================== */

    /*
     * setxattr / getxattr / listxattr / removexattr
     *
     * 扩展属性操作，触发时机：setfattr/getfattr 命令
     * SELinux、ACL 等依赖扩展属性
     */
    int (*setxattr)(const char *path, const char *name,
                    const char *value, size_t size, int flags);
    int (*getxattr)(const char *path, const char *name,
                    char *value, size_t size);
    int (*listxattr)(const char *path, char *list, size_t size);
    int (*removexattr)(const char *path, const char *name);

    /* ==================================================================
     * 第六部分：★ 目录读取
     * ================================================================== */

    /*
     * opendir — 打开目录
     *
     * 触发时机：opendir()
     * 类比 open，但用于目录
     * 如果不需要特殊处理，可以设为 NULL
     */
    int (*opendir)(const char *path, struct fuse_file_info *fi);

    /*
     * ★★ readdir — 读取目录内容
     *
     * 触发时机：readdir(), ls 命令
     * 这是目录操作中最核心的回调。
     *
     * 参数：
     *   path   — 目录路径
     *   buf    — 填充缓冲区（通过 filler 函数填充）
     *   filler — ★ 填充函数，每找到一个目录项就调用一次
     *   offset — 偏移量（用于分页读取，0 表示从头开始）
     *   fi     — 目录文件信息
     *
     * filler 函数签名：
     *   int filler(void *buf, const char *name,
     *              const struct stat *stbuf, off_t off);
     *
     * 使用示例：
     *   filler(buf, ".", NULL, 0);         // 当前目录
     *   filler(buf, "..", NULL, 0);        // 父目录
     *   filler(buf, "hello", &st, 0);      // 文件 "hello"
     *
     * 返回值：0 成功，-errno 失败
     *
     * 类比内核：file_operations.iterate_shared
     */
    int (*readdir)(const char *path, void *buf,
                   fuse_fill_dir_t filler, off_t offset,
                   struct fuse_file_info *fi,
                   enum fuse_readdir_flags flags);

    /*
     * releasedir — 释放目录资源
     *
     * 触发时机：closedir()
     * 类比 release，但用于目录
     */
    int (*releasedir)(const char *path, struct fuse_file_info *fi);

    /*
     * fsyncdir — 同步目录
     *
     * 触发时机：fsync() on directory fd
     */
    int (*fsyncdir)(const char *path, int datasync,
                    struct fuse_file_info *fi);

    /* ==================================================================
     * 第七部分：初始化和销毁
     * ================================================================== */

    /*
     * ★ init — 文件系统初始化
     *
     * 触发时机：mount 完成后，第一次操作之前
     * 用于：
     *   - 初始化私有数据结构
     *   - 协商连接能力（修改 conn->want）
     *   - 启动后台线程
     *
     * 参数：
     *   conn — 连接信息（可修改 want 字段请求能力）
     *   cfg  — FUSE 配置
     *
     * 返回值：私有数据指针（会传给后续所有回调的 private_data）
     */
    void *(*init)(struct fuse_conn_info *conn,
                  struct fuse_config *cfg);

    /*
     * destroy — 文件系统销毁
     *
     * 触发时机：umount 或 FUSE 守护进程退出时
     * 用于：
     *   - 释放 init 分配的资源
     *   - 保存持久化状态
     *
     * 参数：
     *   private_data — init 返回的私有数据
     */
    void (*destroy)(void *private_data);

    /* ==================================================================
     * 第八部分：其他操作
     * ================================================================== */

    /*
     * access — 检查文件访问权限
     *
     * 触发时机：access()
     * 如果不实现，默认返回 0（允许所有访问）
     * 如果 mount 时指定了 default_permissions，内核自动检查，不调用此回调
     */
    int (*access)(const char *path, int mask);

    /*
     * ★ create — 创建并打开文件
     *
     * 触发时机：open(O_CREAT | O_WRONLY, ...)
     * 这是 mknod + open 的组合操作（性能优化）
     *
     * 参数：
     *   path — 新文件路径
     *   mode — 文件权限
     *   fi   — 文件信息（设置 fi->fh 为文件句柄）
     *
     * 如果不实现，FUSE 会先调 mknod 再调 open（两次 round-trip）
     * 实现 create 只需要一次 round-trip
     */
    int (*create)(const char *path, mode_t mode,
                  struct fuse_file_info *fi);

    /*
     * lock — 文件锁操作
     *
     * 触发时机：fcntl(F_SETLK), flock()
     * 需要在 mount 时启用 -o flock 或 -o posix_lock
     */
    int (*lock)(const char *path, struct fuse_file_info *fi,
                int cmd, struct flock *lock);

    /*
     * utimens — 修改文件时间戳
     *
     * 触发时机：utimensat(), touch
     * 比 utime 更精确（纳秒级）
     */
    int (*utimens)(const char *path, const struct timespec tv[2],
                   struct fuse_file_info *fi);

    /*
     * bmap — 块映射
     *
     * 触发时机：FIBMAP ioctl
     * 极少使用，仅 fuseblk 类型需要
     */
    int (*bmap)(const char *path, size_t blocksize, uint64_t *idx);

    /*
     * ioctl — 设备控制
     *
     * 触发时机：ioctl()
     * 用于自定义控制命令
     */
    int (*ioctl)(const char *path, unsigned int cmd, void *arg,
                 struct fuse_file_info *fi, unsigned int flags,
                 void *data);

    /*
     * poll — I/O 多路复用
     *
     * 触发时机：poll(), select(), epoll
     */
    int (*poll)(const char *path, struct fuse_file_info *fi,
                struct fuse_pollhandle *ph, unsigned *reventsp);

    /*
     * write_buf — 写入（带缓冲区链）
     *
     * 与 write 类似，但接收 fuse_bufvec（可包含 splice 缓冲区）
     * 如果实现了 write_buf，write 不会被调用
     * 用于零拷贝优化
     */
    int (*write_buf)(const char *path, struct fuse_bufvec *buf,
                     off_t off, struct fuse_file_info *fi);

    /*
     * read_buf — 读取（返回缓冲区链）
     *
     * 与 read 类似，但返回 fuse_bufvec
     * 用于零拷贝优化
     */
    int (*read_buf)(const char *path, struct fuse_bufvec **bufp,
                    size_t size, off_t off, struct fuse_file_info *fi);

    /*
     * fallocate — 预分配空间
     *
     * 触发时机：fallocate()
     */
    int (*fallocate)(const char *path, int mode, off_t offset,
                     off_t length, struct fuse_file_info *fi);

    /*
     * copy_file_range — 服务端拷贝
     *
     * 触发时机：copy_file_range()
     * 允许在 FUSE 文件系统内部高效拷贝文件（不经过用户缓冲区）
     */
    ssize_t (*copy_file_range)(const char *path_in,
                               struct fuse_file_info *fi_in,
                               off_t off_in,
                               const char *path_out,
                               struct fuse_file_info *fi_out,
                               off_t off_out,
                               size_t len, int flags);

    /*
     * lseek — 文件偏移操作
     *
     * 触发时机：lseek(SEEK_DATA), lseek(SEEK_HOLE)
     * 用于稀疏文件的 SEEK_DATA/SEEK_HOLE 支持
     */
    off_t (*lseek)(const char *path, off_t off, int whence,
                   struct fuse_file_info *fi);
};


/* ======================================================================
 * 回调函数分类总结
 * ======================================================================
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                    fuse_operations 回调分类                  │
 *   ├──────────────┬──────────────────────────────────────────────┤
 *   │ ★ 必须实现   │ getattr                                      │
 *   ├──────────────┼──────────────────────────────────────────────┤
 *   │ ★ 常用       │ readdir, open, read, write, release          │
 *   ├──────────────┼──────────────────────────────────────────────┤
 *   │ 按需实现     │ mkdir, unlink, rmdir, rename, chmod, chown,  │
 *   │              │ truncate, create, symlink, link, readlink    │
 *   ├──────────────┼──────────────────────────────────────────────┤
 *   │ 可选         │ init, destroy, access, statfs, flush, fsync, │
 *   │              │ setxattr, getxattr, listxattr, removexattr   │
 *   ├──────────────┼──────────────────────────────────────────────┤
 *   │ 极少使用     │ bmap, ioctl, poll, write_buf, read_buf,      │
 *   │              │ fallocate, copy_file_range, lseek, lock      │
 *   └──────────────┴──────────────────────────────────────────────┘
 *
 * ★ 最小 FUSE 文件系统只需实现：
 *   1. getattr — 让 ls -l 能工作
 *   2. readdir — 让 ls 能列出文件
 *   3. open    — 让 cat 能打开文件（可以为 NULL，默认成功）
 *   4. read    — 让 cat 能读取内容
 *
 * ★ 调用链（以 cat /mnt/hello 为例）：
 *
 *   cat 进程                  FUSE 内核模块              FUSE 守护进程
 *   ──────                    ────────────              ────────────
 *   stat("/mnt/hello")   →   FUSE_GETATTR           →  getattr("/hello")
 *   open("/mnt/hello")   →   FUSE_OPEN              →  open("/hello")
 *   read(fd, buf, 4096)  →   FUSE_READ              →  read("/hello",...)
 *   close(fd)            →   FUSE_FLUSH + RELEASE   →  flush() + release()
 */

#endif /* FUSE_OPERATIONS_H */
