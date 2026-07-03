/*
 * ======================================================================
 * Step 3: hello_fuse.c — 一个最小可编译的 FUSE 文件系统
 * ======================================================================
 *
 * 参考来源：libfuse/example/hello.c
 *   https://github.com/libfuse/libfuse/blob/master/example/hello.c
 *
 * 本文件实现了一个只读 FUSE 文件系统，包含一个固定内容的文件 "hello"。
 * 功能：
 *   - ls /mnt/myfuse/          → 列出 "hello" 文件
 *   - cat /mnt/myfuse/hello    → 输出 "Hello, FUSE World!\n"
 *   - stat /mnt/myfuse/hello   → 显示文件属性
 *
 * 编译方法：
 *   gcc -o hello_fuse hello_fuse.c $(pkg-config fuse --cflags --libs)
 *   或
 *   gcc -o hello_fuse hello_fuse.c -lfuse -D_FILE_OFFSET_BITS=64
 *
 * 运行方法：
 *   mkdir -p /tmp/myfuse
 *   ./hello_fuse -f /tmp/myfuse     # -f = 前台模式（方便观察）
 *   # 另一个终端：
 *   ls /tmp/myfuse/
 *   cat /tmp/myfuse/hello
 *   # 卸载：
 *   fusermount -u /tmp/myfuse
 *
 * 阅读要点：
 *   1. 每个回调函数的作用和参数含义
 *   2. fuse_main() 的启动流程（main 函数中的注释）
 *   3. 返回值约定（0=成功，负 errno=失败）
 *   4. struct fuse_file_info 中 fh 的使用方式
 *
 * 预计阅读时间：15 分钟
 * ======================================================================
 */

/*
 * ★ FUSE 头文件说明：
 *
 * #define FUSE_USE_VERSION 26
 *   必须在使用任何 FUSE API 之前定义！
 *   指定要使用的 FUSE API 版本号。
 *   31 = FUSE 3.x API（推荐新项目使用，需要 libfuse 3.x）
 *   26 = FUSE 2.6+ API（兼容旧代码，大多数发行版默认安装）
 *
 * ★ 注意：本示例使用 FUSE 2.x API（版本 26），
 *   因为大多数 Linux 发行版（包括 CentOS/RHEL）默认安装 fuse 2.x。
 *   如果使用 FUSE 3.x，部分回调签名会有差异（见注释中标注）。
 */
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

/* ======================================================================
 * 文件系统数据定义
 * ======================================================================
 *
 * 我们的"文件系统"只有一个根目录和一个文件 "hello"。
 * 用静态数据模拟文件内容。
 */

/* 文件内容（固定字符串） */
static const char *hello_content = "Hello, FUSE World!\n";
static const size_t hello_content_len = 19; /* strlen("Hello, FUSE World!\n") */

/* 文件名 */
static const char *hello_filename = "hello";

/*
 * ★ 路径常量
 *
 * FUSE highlevel API 使用路径（path）来标识文件。
 * 根目录是 "/"，根目录下的文件是 "/hello"。
 */
#define ROOT_PATH "/"
#define HELLO_PATH "/hello"


/* ======================================================================
 * ★ 回调函数 #1: getattr — 获取文件属性
 * ======================================================================
 *
 * 触发时机：stat(), ls -l, access(), 以及任何需要文件属性的操作
 *
 * 这是最重要的回调之一——没有它，ls 命令都无法工作。
 *
 * 参数：
 *   path  — 文件路径（如 "/", "/hello"）
 *   stbuf — 输出参数，需要填充 struct stat
 *
 * 返回值：
 *   0     = 成功
 *   -ENOENT = 文件不存在
 *   -EACCES = 权限不足
 *
 * ★ FUSE 3.x 差异：
 *   FUSE 3.x 的 getattr 增加了 struct fuse_file_info *fi 参数，
 *   FUSE 2.x 没有此参数。
 */
static int hello_getattr(const char *path, struct stat *stbuf)
{
    int res = 0;

    /* 先清空 stat 结构体 */
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, ROOT_PATH) == 0) {
        /*
         * ★ 根目录的属性
         *
         * S_IFDIR = 目录类型
         * 0755    = rwxr-xr-x（所有者可写，其他人可读可执行）
         * st_nlink = 2（标准目录硬链接数：自身 + "."）
         */
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    }
    else if (strcmp(path + 1, hello_filename) == 0) {
        /*
         * ★ 文件 "hello" 的属性
         *
         * path + 1 跳过前导 '/'
         * 例如 path="/hello" → path+1="hello"
         *
         * S_IFREG = 普通文件类型
         * 0444    = r--r--r--（只读文件）
         * st_nlink = 1（普通文件的默认硬链接数）
         * st_size  = 文件内容长度（字节）
         */
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = hello_content_len;
    }
    else {
        /* 路径不匹配 → 文件不存在 */
        res = -ENOENT;
    }

    return res;
}


/* ======================================================================
 * ★ 回调函数 #2: readdir — 读取目录内容
 * ======================================================================
 *
 * 触发时机：ls, readdir()
 *
 * 参数：
 *   path   — 目录路径（本实现只支持 "/"）
 *   buf    — 填充缓冲区（通过 filler 函数填充）
 *   filler — ★ 填充函数，每找到一个目录项调用一次
 *   offset — 偏移量（用于分页读取，0 表示从头开始）
 *   fi     — 目录文件信息
 *
 * filler 函数签名：
 *   int filler(void *buf, const char *name,
 *              const struct stat *stbuf, off_t off);
 *   参数：
 *     buf   — 就是 readdir 传入的 buf（透传）
 *     name  — 文件名（如 ".", "..", "hello"）
 *     stbuf — 可选的文件属性（传 NULL 表示不提供）
 *     off   — 下一项的 offset（传 0 表示不使用 offset 分页）
 *
 * 返回值：
 *   0       = 成功
 *   -ENOENT = 目录不存在
 *
 * ★ FUSE 3.x 差异：
 *   FUSE 3.x 的 readdir 增加了 enum fuse_readdir_flags flags 参数。
 *   FUSE 2.x 没有此参数。
 */
static int hello_readdir(const char *path, void *buf,
                         fuse_fill_dir_t filler, off_t offset,
                         struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;

    /* 本实现只有根目录 */
    if (strcmp(path, ROOT_PATH) != 0)
        return -ENOENT;

    /*
     * ★ 填充目录项
     *
     * 每个目录至少要返回 "." 和 ".."
     * filler 返回 0 表示成功，非 0 表示缓冲区已满
     */
    filler(buf, ".", NULL, 0);       /* 当前目录 */
    filler(buf, "..", NULL, 0);      /* 父目录 */
    filler(buf, hello_filename, NULL, 0);  /* 文件 "hello" */

    /*
     * filler 的第三个参数可以传 struct stat *，
     * 这样 readdirplus 模式下可以一次返回属性，减少 GETATTR 调用。
     * 这里传 NULL，ls 会对每个文件额外调用一次 getattr。
     */

    return 0;
}


/* ======================================================================
 * ★ 回调函数 #3: open — 打开文件
 * ======================================================================
 *
 * 触发时机：open()
 *
 * 参数：
 *   path — 文件路径
 *   fi   — 文件信息
 *          fi->flags 包含打开标志（O_RDONLY, O_WRONLY, O_RDWR）
 *          fi->fh 可以设置文件句柄
 *
 * 返回值：
 *   0       = 成功
 *   -EACCES = 权限不足
 *   -ENOENT = 文件不存在
 *
 * ★ 注意：
 *   如果不需要特殊处理（如权限检查、分配资源），可以设为 NULL，
 *   libfuse 默认返回成功。
 *
 *   本实现中我们检查是否试图写入只读文件。
 */
static int hello_open(const char *path, struct fuse_file_info *fi)
{
    /* 只支持根目录下的 hello 文件 */
    if (strcmp(path, HELLO_PATH) != 0)
        return -ENOENT;

    /*
     * ★ 权限检查
     *
     * fi->flags 包含 open 的标志位。
     * O_ACCMODE 是读/写标志的掩码（0x03），
     * 提取后与 O_RDONLY(0)、O_WRONLY(1)、O_RDWR(2) 比较。
     *
     * 我们的文件是只读的，拒绝任何写操作。
     */
    if ((fi->flags & O_ACCMODE) != O_RDONLY)
        return -EACCES;

    /*
     * ★ 可选：设置文件句柄 fi->fh
     *
     * fi->fh 是一个 uint64_t，可以存储任何值：
     *   - 真实文件的 fd
     *   - 自定义结构体的指针
     *   - 简单的索引值
     *
     * 后续的 read/write/release 回调都能通过 fi->fh 访问这个值。
     *
     * 本实现不需要 fh（内容固定），所以不设置。
     * 如果是真实文件系统，可以这样：
     *   fi->fh = (uint64_t) open_real_file(path);
     */

    return 0;
}


/* ======================================================================
 * ★★ 回调函数 #4: read — 读取文件内容
 * ======================================================================
 *
 * 触发时机：read(), pread()
 *
 * 这是 FUSE 文件系统中 I/O 热路径上的核心回调。
 *
 * 参数：
 *   path   — 文件路径
 *   buf    — 输出缓冲区（将数据拷贝到这里）
 *   size   — 请求读取的字节数
 *   offset — 文件偏移量（从哪个位置开始读）
 *   fi     — 文件信息（fi->fh 包含 open 时设置的句柄）
 *
 * 返回值：
 *   > 0    = 实际读取的字节数（可以小于 size）
 *   0      = EOF（文件结束）
 *   -errno = 错误
 *
 * ★ 语义与标准 read() 一致：
 *   - 不需要保证读满 size 字节
 *   - offset + 返回值 = 下次读取的起始位置
 *   - 返回 0 表示到达文件末尾
 */
static int hello_read(const char *path, char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
    (void) fi;

    /* 只支持 hello 文件 */
    if (strcmp(path, HELLO_PATH) != 0)
        return -ENOENT;

    /*
     * ★ 读取逻辑
     *
     * 从固定字符串 hello_content 的 offset 位置开始，
     * 拷贝 size 字节到 buf。需要处理边界情况：
     *   - offset >= 文件长度 → 返回 0（EOF）
     *   - offset + size > 文件长度 → 只返回剩余部分
     */
    size_t len = hello_content_len;

    if ((size_t)offset >= len)
        return 0;   /* EOF */

    if (offset + size > len)
        size = len - offset;  /* 截断到文件末尾 */

    /*
     * ★ 核心：将数据拷贝到 buf
     *
     * 在真实文件系统中，这里可能是：
     *   - 从磁盘读取数据
     *   - 从网络获取数据
     *   - 从内存数据库查询数据
     *
     * 数据拷贝到 buf 后，libfuse 会构造 FUSE_READ 响应：
     *   fuse_out_header { len=16+size, error=0, unique=请求的unique }
     *   + 数据（size 字节）
     * 然后 write() 到 /dev/fuse
     */
    memcpy(buf, hello_content + offset, size);

    return size;  /* 返回实际读取的字节数 */
}


/* ======================================================================
 * ★ 回调函数 #5: release — 释放文件资源
 * ======================================================================
 *
 * 触发时机：当文件的所有 fd 都关闭后（最后一次 close）
 *
 * 参数：
 *   path — 文件路径
 *   fi   — 文件信息（fi->fh 包含 open 时设置的句柄）
 *
 * 返回值：被忽略（release 不能失败）
 *
 * ★ 本实现不需要释放资源（open 没有分配任何东西），
 *   所以可以不实现此回调。这里列出是为了演示完整生命周期。
 */
static int hello_release(const char *path, struct fuse_file_info *fi)
{
    (void) path;
    (void) fi;

    /*
     * 如果 open 时分配了资源，在这里释放：
     *
     *   if (fi->fh) {
     *       close((int)fi->fh);     // 关闭真实文件 fd
     *       fi->fh = 0;
     *   }
     *
     * 或者释放自定义结构体：
     *
     *   struct my_file_ctx *ctx = (struct my_file_ctx *)fi->fh;
     *   free(ctx);
     */

    return 0;
}


/* ======================================================================
 * ★ fuse_operations 结构体填充
 * ======================================================================
 *
 * 将所有回调函数注册到 fuse_operations 结构体中。
 * 未设置的字段为 NULL，libfuse 会返回 -ENOSYS 或默认值。
 */
static struct fuse_operations hello_ops = {
    .getattr  = hello_getattr,    /* ★ 必须实现 */
    .readdir  = hello_readdir,    /* ★ 目录列举 */
    .open     = hello_open,       /* 文件打开（可省略） */
    .read     = hello_read,       /* ★ 文件读取 */
    .release  = hello_release,    /* 文件关闭（可省略） */
};


/* ======================================================================
 * ★★ main() — FUSE 启动入口
 * ======================================================================
 *
 * fuse_main() 是 libfuse 提供的"一站式启动函数"。
 * 它做了以下事情：
 *
 *   1. 解析命令行参数（-f 前台、-d 调试、-o 选项等）
 *   2. 打开 /dev/fuse，获取 fd
 *   3. 调用 mount("fuse", mountpoint, ...) 挂载文件系统
 *   4. 创建 fuse_session，注册回调
 *   5. 发送 FUSE_INIT 请求与内核协商能力
 *   6. 进入事件循环：
 *      while (!退出) {
 *          read(/dev/fuse)   → 获取请求
 *          解析 opcode       → 确定操作类型
 *          调用对应回调       → 执行用户代码
 *          write(/dev/fuse)  → 送回响应
 *      }
 *   7. umount + 清理资源
 *
 * ★ fuse_main() 的签名：
 *   int fuse_main(int argc, char *argv[],
 *                 const struct fuse_operations *op,
 *                 void *user_data);
 *
 *   参数：
 *     argc, argv — 命令行参数
 *     op         — fuse_operations 结构体（回调函数表）
 *     user_data  — 自定义数据（传给 init 回调，这里不需要）
 *
 *   返回值：
 *     0 = 正常退出
 *     1 = 错误
 */
int main(int argc, char *argv[])
{
    /*
     * ★ 命令行参数示例：
     *
     *   ./hello_fuse /tmp/myfuse           # 后台挂载
     *   ./hello_fuse -f /tmp/myfuse        # 前台模式（日志输出到 stderr）
     *   ./hello_fuse -f -d /tmp/myfuse     # 前台 + 调试模式（详细日志）
     *   ./hello_fuse -o allow_other /tmp/myfuse  # 允许其他用户访问
     *   ./hello_fuse -o ro /tmp/myfuse     # 只读挂载
     *
     * fuse_main() 会自动解析这些参数，
     * 最后一个非选项参数是挂载点路径。
     */

    return fuse_main(argc, argv, &hello_ops, NULL);

    /*
     * ★ fuse_main() 内部流程详解：
     *
     *   fuse_main(argc, argv, &hello_ops, NULL)
     *     │
     *     ├── fuse_parse_cmdline()     解析命令行参数
     *     │     提取挂载点、前台/后台、调试模式等
     *     │
     *     ├── fuse_mount()             挂载文件系统
     *     │     ├── open("/dev/fuse")  打开 FUSE 设备，获取 fd
     *     │     └── mount("fuse", mountpoint, ..., "fd=N,rootmode=...")
     *     │
     *     ├── fuse_new()               创建 FUSE 会话
     *     │     ├── 分配 fuse_session
     *     │     ├── 注册 hello_ops 中的回调
     *     │     └── 发送 FUSE_INIT 请求（协商协议版本和能力）
     *     │
     *     ├── fuse_daemonize()         后台化（如果没指定 -f）
     *     │
     *     └── fuse_loop()              ★ 进入主事件循环
     *           │
     *           │  ┌──────────────────────────────────────┐
     *           │  │  while (!session_exit) {             │
     *           │  │    read(/dev/fuse) → 获取请求        │
     *           │  │    解析 fuse_in_header.opcode        │
     *           │  │    switch (opcode) {                 │
     *           │  │      case FUSE_GETATTR:              │
     *           │  │        hello_ops.getattr(...)        │
     *           │  │        break;                        │
     *           │  │      case FUSE_READDIR:              │
     *           │  │        hello_ops.readdir(...)        │
     *           │  │        break;                        │
     *           │  │      case FUSE_OPEN:                 │
     *           │  │        hello_ops.open(...)           │
     *           │  │        break;                        │
     *           │  │      case FUSE_READ:                 │
     *           │  │        hello_ops.read(...)           │
     *           │  │        break;                        │
     *           │  │      ...                             │
     *           │  │    }                                 │
     *           │  │    write(/dev/fuse) → 送回响应       │
     *           │  │  }                                   │
     *           │  └──────────────────────────────────────┘
     *           │
     *           ├── 退出时发送 FUSE_DESTROY
     *           └── umount + close(/dev/fuse)
     */
}


/*
 * ======================================================================
 * 总结：cat /tmp/myfuse/hello 的完整调用链
 * ======================================================================
 *
 *   cat 进程                内核 fuse.ko              FUSE 守护进程（hello_fuse）
 *   ───────                ────────────              ─────────────────────────
 *
 *   1. stat("/tmp/myfuse/hello")
 *      → vfs_getattr()
 *        → fuse_getattr()
 *          → FUSE_GETATTR        ───→   hello_getattr("/hello", &st, NULL)
 *                                       → st.st_mode = S_IFREG | 0444
 *                                       → st.st_size = 19
 *          ←── fuse_attr_out     ←───
 *
 *   2. open("/tmp/myfuse/hello", O_RDONLY)
 *      → vfs_open()
 *        → fuse_open()
 *          → FUSE_OPEN           ───→   hello_open("/hello", fi)
 *                                       → 检查 flags == O_RDONLY ✓
 *                                       → return 0
 *          ←── fuse_open_out     ←───   { fh=0, open_flags=0 }
 *
 *   3. read(fd, buf, 4096)
 *      → vfs_read()
 *        → fuse_read_iter()
 *          → fuse_send_read()
 *            → FUSE_READ         ───→   hello_read("/hello", buf, 4096, 0, fi)
 *                                       → memcpy(buf, "Hello, FUSE World!\n", 19)
 *                                       → return 19
 *            ←── 数据(19B)       ←───
 *      ←── 19 字节
 *
 *   4. read(fd, buf, 4096)       // 第二次 read
 *      → FUSE_READ               ───→   hello_read("/hello", buf, 4096, 19, fi)
 *                                       → offset(19) >= len(19) → return 0 (EOF)
 *            ←── 0 字节           ←───
 *      ←── 0 (EOF)
 *
 *   5. close(fd)
 *      → vfs_close()
 *        → fuse_flush()
 *          → FUSE_FLUSH          ───→   (hello_ops.flush == NULL → 返回 0)
 *        → fuse_release()
 *          → FUSE_RELEASE        ───→   hello_release("/hello", fi)
 *                                       → return 0
 *
 *   cat 输出: "Hello, FUSE World!\n"
 */
