/*
 * ======================================================================
 * Step 2: perf record — 采样与数据写入的核心流程
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/tools/perf/builtin-record.c
 *
 * perf record 是 perf 最常用的子命令。它通过 perf_event_open() 系统调用
 * 在内核中配置硬件计数器，然后通过 mmap 环形缓冲区读取采样数据，
 * 写入 perf.data 文件。
 *
 * 阅读要点：
 *   1. __cmd_record() — 主函数，理解整个 record 流程
 *   2. record__mmap_read_evlist() — 从 mmap 环形缓冲区读取数据
 *   3. 主循环的 poll → read → write 模式
 *
 * 预计阅读时间：15 分钟
 * ======================================================================
 */

/*
 * ★ perf record 数据流全路径：
 *
 *   内核 PMU (硬件计数器)
 *       │
 *       ▼ perf_event_open()
 *   内核 ring buffer (per-CPU mmap)
 *       │
 *       ▼ record__mmap_read_evlist()
 *   perf_mmap__push() → 从 mmap 读取事件
 *       │
 *       ▼ record__write()
 *   perf.data 文件
 *
 *   用户态 perf record 进程的核心工作就是：
 *   1. 配置 perf_event（通过 evlist）
 *   2. 循环 poll mmap 等待数据
 *   3. 读取并写入文件
 */


/* ======================================================================
 * 核心函数 #1: __cmd_record() — perf record 主函数
 * ======================================================================
 *
 * 源码位置：builtin-record.c:1606
 * 这是 perf record 的入口函数，理解它就能理解整个 record 流程。
 */

static int __cmd_record(struct record *rec, int argc, const char **argv)
{
    int err;
    int status = 0;
    unsigned long waking = 0;         /* 被唤醒次数（用于统计输出） */
    const bool forks = argc > 0;      /* 是否 fork 子进程执行命令 */
    struct perf_data *data = &rec->data;  /* perf.data 文件句柄 */
    struct perf_session *session;

    /* ── 1. 信号处理设置 ── */
    atexit(record__sig_exit);         /* 退出时清理 */
    signal(SIGCHLD, sig_handler);     /* 子进程结束 */
    signal(SIGINT, sig_handler);      /* Ctrl+C 停止采集 */
    signal(SIGTERM, sig_handler);     /* kill 停止 */

    /* ── 2. 创建 perf session ── */
    session = perf_session__new(data, false, tool);
    /*
     * perf_session 管理整个采集会话：
     * - 关联 evlist（事件列表）
     * - 管理 perf.data 文件
     * - 处理机器/线程信息
     */

    /* ── 3. 初始化压缩（zstd）── */
    if (zstd_init(&session->zstd_data, rec->opts.comp_level) < 0) {
        pr_err("Compression initialization failed.\n");
        return -1;
    }
    /*
     * perf.data 支持 zstd 压缩（--comp_level 参数）
     * 压缩级别越高，文件越小但 CPU 开销越大
     */

    /* ── 4. 创建 eventfd 用于唤醒 ── */
    done_fd = eventfd(0, EFD_NONBLOCK);
    err = evlist__add_pollfd(rec->evlist, done_fd);
    /*
     * eventfd 是一个轻量级的通知机制：
     * 信号处理器写入 eventfd → poll() 返回 → 主循环退出
     * 这比在信号处理器中设置全局变量更安全
     */

    /* ── 5. 配置 evlist（事件列表 + mmap）── */
    /* ... evlist 配置代码（省略，涉及 CPU/线程映射、事件打开等）... */

    /* ── 6. 如果需要 fork 子进程 ── */
    if (forks) {
        /*
         * perf record -- ls -la
         *            ↑ forks=true，fork 子进程执行 ls
         * perf record -p 1234
         *            ↑ forks=false，附加到已有进程
         */
        err = evlist__prepare_workload(rec->evlist, ...);
    }

    /* ── 7. 打开 perf_event（核心步骤！）── */
    err = evlist__open(rec->evlist);
    /*
     * evlist__open() 内部对每个 (CPU, 线程) 组合调用：
     *   perf_event_open(&attr, pid, cpu, group_fd, flags)
     *
     * 这是最关键的系统调用：
     * - 在内核中创建 perf_event 结构
     * - 配置 PMU 硬件计数器
     * - 创建 mmap 环形缓冲区
     * - 返回文件描述符
     *
     * ★ 如果 perf_event_open 失败，常见原因：
     *   - /proc/sys/kernel/perf_event_paranoid 设置过高
     *   - 事件类型不被硬件支持
     *   - 权限不足
     */

    /* ── 8. 设置 mmap 映射 ── */
    err = evlist__mmap(rec->evlist, opts->mmap_pages);
    /*
     * 对每个 perf_event fd 执行 mmap()：
     *   mmap(NULL, mmap_size, PROT_READ, MAP_SHARED, fd, 0)
     *
     * mmap 区域布局：
     *   ┌────────────────────┐
     *   │ metadata page (1页) │ ← 读写指针、事件计数等
     *   ├────────────────────┤
     *   │ ring buffer        │ ← 采样数据（默认 128 页 = 512KB）
     *   │ (mmap_pages 页)    │
     *   └────────────────────┘
     *
     * 内核写入采样数据到 ring buffer，
     * perf record 从 ring buffer 读取数据写入 perf.data
     */

    /* ── 9. 启动工作负载 ── */
    if (forks)
        evlist__start_workload(rec->evlist);

    /* ── 10. 启用事件 ── */
    evlist__enable(rec->evlist);
    /*
     * 通过 ioctl(fd, PERF_EVENT_IOC_ENABLE) 启用所有事件
     * 从此刻起，内核开始向 mmap ring buffer 写入采样数据
     */

    /* ═══════════════════════════════════════════
     * ★ 11. 主循环：poll → read → write
     * ═══════════════════════════════════════════ */
    for (;;) {
        unsigned long long hits = rec->samples;

        /* ★ 从所有 mmap ring buffer 读取数据 */
        if (record__mmap_read_all(rec, false) < 0) {
            err = -1;
            goto out_child;
        }

        /* 如果没有新数据 → poll 等待 */
        if (hits == rec->samples) {
            if (done || draining)
                break;            /* 收到信号，退出循环 */

            err = evlist__poll(rec->evlist, -1);
            /*
             * poll() 阻塞直到某个 mmap ring buffer 有新数据
             * -1 表示无限等待
             *
             * 内核在写入采样数据后，通过 epoll/wakeup 通知用户态
             * 这比 busy-polling 节省 CPU
             */
            waking++;
        }

        /* 如果收到终止信号 */
        if (done && !disabled && !target__none(&opts->target)) {
            evlist__disable(rec->evlist);  /* 禁用所有事件 */
            disabled = true;
        }
    }

    /* ── 12. 清理 ── */
    /* 最终读取一次剩余数据 */
    record__mmap_read_all(rec, true);

    /* 写 perf.data 文件头（包含统计信息） */
    perf_session__write_header(session, ...);

    /* 输出统计摘要 */
    fprintf(stderr, "[ perf record: Woken up %ld times to write data ]\n", waking);
    fprintf(stderr, "[ perf record: Captured and wrote %.3f MB %s ]\n",
            (double)rec->bytes_written / 1024.0 / 1024.0, data->file.path);

    return status;
}


/* ======================================================================
 * 核心函数 #2: record__mmap_read_evlist() — 从 mmap 读取采样数据
 * ======================================================================
 *
 * 源码位置：builtin-record.c:1097
 * 这个函数遍历所有 CPU 的 mmap ring buffer，读取采样事件，
 * 写入 perf.data 文件。
 */

static int record__mmap_read_evlist(struct record *rec, struct evlist *evlist,
                                    bool overwrite, bool synch)
{
    int i;
    int rc = 0;
    struct mmap *maps;

    maps = overwrite ? evlist->overwrite_mmap : evlist->mmap;

    /* ★ 遍历每个 CPU 的 mmap ring buffer */
    for (i = 0; i < evlist->core.nr_mmaps; i++) {
        struct mmap *map = &maps[i];

        if (map->core.base) {
            /*
             * perf_mmap__push() 从 ring buffer 读取数据
             *
             * ring buffer 读取流程：
             * 1. 读取 metadata page 中的 head/tail 指针
             * 2. 如果 head > tail，说明有新数据
             * 3. 从 ring buffer 中 memcpy 数据到临时缓冲区
             * 4. 更新 tail 指针
             *
             * 每次 push 的数据通过 record__pushfn() 写入 perf.data
             */
            if (perf_mmap__push(map, rec, record__pushfn) < 0) {
                rc = -1;
                goto out;
            }
        }
    }

out:
    /* 更新写入字节计数 */
    if (rec->bytes_written != bytes_written)
        record__write(rec, NULL, &finished_round_event, ...);

    return rc;
}


/*
 * ★ 总结：perf record 的核心流程
 *
 *  __cmd_record()
 *    │
 *    ├── perf_session__new()       → 创建会话
 *    ├── evlist__open()            → perf_event_open() × N（每个 CPU/线程）
 *    ├── evlist__mmap()            → mmap() × N（每个 perf_event fd）
 *    ├── evlist__enable()          → ioctl(PERF_EVENT_IOC_ENABLE)
 *    │
 *    └── for (;;) {                 → ★ 主循环
 *          record__mmap_read_all()  → 从所有 mmap 读取采样
 *            └── record__mmap_read_evlist()
 *                  └── for each CPU:
 *                        perf_mmap__push()  → 读 ring buffer
 *                          └── record__pushfn()  → 写入 perf.data
 *
 *          evlist__poll()           → 等待新数据（内核通知）
 *
 *          if (done) break          → 收到信号退出
 *        }
 *
 * 关键理解：
 *   1. perf record 本质上是一个 poll + mmap read + file write 循环
 *   2. 采样由内核完成（PMU 触发中断 → 内核写入 ring buffer）
 *   3. perf record 只负责从 ring buffer 搬运数据到文件
 *   4. mmap ring buffer 是内核和用户态的共享内存通道
 */
