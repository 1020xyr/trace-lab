/*
 * ======================================================================
 * Step 3: perf report — 数据解析与报告生成的核心流程
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/tools/perf/builtin-report.c
 *
 * perf report 读取 perf.data 文件，解析采样事件，进行符号解析，
 * 生成热点函数排名报告。
 *
 * 阅读要点：
 *   1. cmd_report() — 入口函数
 *   2. 样本处理流程：perf_event → hist_entry → 排序输出
 *   3. 关键数据结构：struct hist_entry, struct symbol, struct dso
 *
 * 预计阅读时间：15 分钟
 * ======================================================================
 */

/*
 * ★ perf report 数据流：
 *
 *   perf.data 文件
 *       │
 *       ▼ perf_session__process_events()
 *   perf 事件解析（sample_event → ip/tid/time 等字段）
 *       │
 *       ▼ process_sample_event()
 *   符号解析（IP 地址 → 函数名 + DSO）
 *       │
 *       ▼ hist_entry 创建
 *   直方图条目（每个唯一调用栈一个条目）
 *       │
 *       ▼ 排序 + 折叠
 *   按采样数排序
 *       │
 *       ▼ hists__fprintf()
 *   终端输出（Overhead / Symbol / DSO）
 *
 *   示例输出：
 *     30.00%  fio      libc.so     [.] __random_r
 *     20.00%  fio      [kernel]    [k] _raw_spin_lock
 *     15.00%  fio      fio         [.] io_u_submit
 *     │       │        │            │    │
 *     │       │        │            │    └─ 函数名
 *     │       │        │            └─ 内核态(k)/用户态(.)
 *     │       │        └─ DSO（动态链接库/可执行文件/内核模块）
 *     │       └─ 进程/线程名
 *     └─ 采样占比
 */


/* ======================================================================
 * 核心数据结构
 * ======================================================================
 */

/*
 * ★ struct hist_entry — 直方图条目
 *
 * 每个唯一的 (线程, DSO, 符号, 调用栈) 组合对应一个 hist_entry。
 * 采样数越多，排名越靠前。
 *
 * 源码位置：tools/perf/util/hist.h
 */
struct hist_entry {
    struct rb_node      rb_node;        /* 红黑树节点（用于排序） */

    /* ★ 核心统计 */
    u64                 period;         /* 采样数（权重） */
    u64                 period_sys;     /* 内核态采样数 */
    u64                 period_us;      /* 用户态采样数 */

    /* ★ 标识信息 */
    struct thread       *thread;        /* 所属线程 */
    struct map          *ms;            /* 内存映射（DSO + 符号） */
    struct symbol       *sym;           /* 符号（函数名） */
    struct dso          *dso;           /* DSO（共享库/可执行文件） */

    /* ★ 调用栈（-g 选项时） */
    struct callchain_root callchain[0]; /* 调用栈树 */

    /* 排序键 */
    struct sort_entry   *sort;          /* 排序依据 */
};

/*
 * ★ struct symbol — 符号（函数/变量名）
 *
 * 源码位置：tools/perf/util/symbol.h
 */
struct symbol {
    struct rb_node      rb_node;        /* 红黑树节点 */
    u64                 start;          /* 符号起始地址 */
    u64                 end;            /* 符号结束地址 */
    u8                  binding;        /* STB_LOCAL / STB_GLOBAL */
    const char          *name;          /* ★ 符号名（函数名） */
};

/*
 * ★ struct dso — Dynamic Shared Object
 *
 * 代表一个可执行文件、共享库或内核模块。
 * perf report 中的 "libc.so" / "[kernel]" / "fio" 就是 DSO。
 *
 * 源码位置：tools/perf/util/dso.h
 */
struct dso {
    struct rb_node      rb_node;
    char                *name;          /* ★ DSO 名称 */
    enum dso_kernel_type kernel;        /* 是否是内核 DSO */
    struct rb_root      symbols;        /* 该 DSO 中的所有符号 */
};


/* ======================================================================
 * 核心函数 #1: process_sample_event() — 处理单个采样事件
 * ======================================================================
 *
 * 源码位置：builtin-report.c
 * 这是 perf report 处理每个采样事件的核心回调。
 * perf.data 中的每个 PERF_RECORD_SAMPLE 都会触发这个函数。
 */

static int process_sample_event(struct perf_tool *tool,
                                union perf_event *event,
                                struct perf_sample *sample,
                                struct evsel *evsel,
                                struct machine *machine)
{
    struct report *rep = container_of(tool, struct report, tool);
    struct addr_location al;
    struct hist_entry *he;

    /* ── 1. 解析采样地址 ── */
    /*
     * sample->ip = 采样时的指令指针（Instruction Pointer）
     * 这就是 CPU 被中断时正在执行的地址。
     *
     * thread__find_map() 将 IP 地址映射到：
     *   - DSO（哪个 .so / 可执行文件）
     *   - Symbol（哪个函数）
     */
    thread__find_map(al.thread, sample->cpumode, sample->ip, &al);
    /*
     * 映射过程：
     *   IP = 0x7f123456
     *   → 在 thread 的 mmap 列表中找到包含此 IP 的 mmap
     *   → mmap 关联到 DSO (如 libc-2.31.so)
     *   → 在 DSO 的符号表中找到包含此 IP 的 symbol (如 __random_r)
     *
     * ★ 如果找不到符号 → 显示为 [unknown] 或十六进制地址
     *   常见原因：没有调试信息、strip 过的二进制、JIT 代码
     */

    /* ── 2. 过滤 ── */
    if (rep->symfs || rep->dso_filter || rep->comm_filter) {
        /* 应用 --dso / --comm / --symbol-filter 等过滤条件 */
    }

    /* ── 3. 创建/更新 hist_entry ── */
    he = hists__add_entry(hists, &al, NULL, NULL, NULL,
                          sample->period,  /* ★ 采样权重 */
                          sample->weight,
                          sample->transaction,
                          sample->time,
                          true);
    /*
     * hists__add_entry() 的逻辑：
     *   如果已存在相同 (thread, dso, symbol, callchain) 的条目：
     *     → period += sample->period  （累加采样数）
     *   如果不存在：
     *     → 创建新的 hist_entry
     *
     * ★ 这就是为什么高频函数排名靠前：
     *   被采样到的次数越多 → period 越大 → 排名越高
     */

    /* ── 4. 处理调用栈（-g 选项） ── */
    if (rep->callchain_enabled && sample->callchain) {
        /*
         * sample->callchain 是一个地址数组：
         *   [caller_ip, caller_caller_ip, ..., entry_point]
         *
         * 对每个 IP 进行符号解析，构建调用栈树。
         * 调用栈折叠策略：
         *   - callee（默认）：从被调者到调用者
         *   - caller：从调用者到被调者
         */
        callchain_append(he->callchain, ...);
    }

    return 0;
}


/* ======================================================================
 * 核心函数 #2: cmd_report() — 入口函数
 * ======================================================================
 *
 * 源码位置：builtin-report.c
 * perf report 的入口，编排整个报告生成流程。
 */

int cmd_report(int argc, const char **argv)
{
    struct perf_session *session;
    struct report report = { .tool = { .sample = process_sample_event, ... } };

    /* ── 1. 解析命令行参数 ── */
    /* --stdio / --tui / --gtk  输出模式 */
    /* -g                      显示调用栈 */
    /* --sort                  排序键 */
    /* --dso                   DSO 过滤 */
    /* --comm                  进程名过滤 */

    /* ── 2. 打开 perf.data 文件 ── */
    session = perf_session__new(&data, false, &report.tool);
    /*
     * perf_session 读取 perf.data 文件头：
     * - 事件类型和配置
     * - 采样频率/周期
     * - 机器信息（CPU 型号、内核版本）
     * - 线程列表（PID → 进程名映射）
     * - DSO 列表（mmap 映射）
     */

    /* ── 3. 加载符号表 ── */
    /*
     * 对每个 DSO 加载符号信息：
     * - ELF 符号表 (.symtab / .dynsym)
     * - DWARF 调试信息 (.debug_info)
     * - /proc/kallsyms（内核符号）
     * - /usr/lib/debug/（分离的调试信息包）
     */

    /* ── 4. 处理所有事件 ★ ── */
    perf_session__process_events(session, &report.tool);
    /*
     * 遍历 perf.data 中的每个事件：
     *
     *   PERF_RECORD_SAMPLE  → process_sample_event()
     *   PERF_RECORD_MMAP    → 更新 DSO 映射表
     *   PERF_RECORD_COMM    → 更新线程名映射
     *   PERF_RECORD_FORK    → 创建新线程
     *   PERF_RECORD_EXIT    → 标记线程退出
     *   PERF_RECORD_LOST    → 记录丢失的采样数
     *
     * ★ PERF_RECORD_LOST 很重要：
     *   如果 ring buffer 溢出，内核会发送 LOST 事件
     *   perf report 输出的 "lost samples" 就是这些
     */

    /* ── 5. 排序 ── */
    hists__collapse_resort(&report.hists);
    /*
     * 将所有 hist_entry 按排序键排序：
     * 默认排序：period（采样数，降序）
     * 其他排序：comm, dso, symbol, parent, srcline 等
     */

    /* ── 6. 输出报告 ── */
    hists__fprintf(&report.hists, stdout, ...);
    /*
     * 生成最终的表格输出：
     *
     * # Overhead  Command  Shared Object     Symbol
     * # ........  .......  ................  ......................
     *     30.00%  fio      libc.so           [.] __random_r
     *     20.00%  fio      [kernel.kallsyms] [k] _raw_spin_lock
     *     15.00%  fio      fio               [.] io_u_submit
     *     10.00%  fio      [kernel.kallsyms] [k] __schedule
     *      5.00%  fio      libaio.so         [.] io_submit
     *      ...
     */

    return 0;
}


/*
 * ★ 总结：perf report 的核心流程
 *
 *  cmd_report()
 *    │
 *    ├── perf_session__new()         → 打开 perf.data
 *    ├── 加载符号表                   → ELF / DWARF / kallsyms
 *    │
 *    ├── perf_session__process_events() → ★ 遍历每个事件
 *    │     │
 *    │     ├── PERF_RECORD_SAMPLE → process_sample_event()
 *    │     │     ├── thread__find_map()   → IP → DSO + Symbol
 *    │     │     └── hists__add_entry()   → 创建/累加 hist_entry
 *    │     │
 *    │     ├── PERF_RECORD_MMAP  → 更新 DSO 映射
 *    │     ├── PERF_RECORD_COMM  → 更新线程名
 *    │     └── PERF_RECORD_LOST  → 统计丢失采样
 *    │
 *    ├── hists__collapse_resort()    → 按采样数排序
 *    │
 *    └── hists__fprintf()            → 输出报告表格
 *
 * 关键理解：
 *   1. perf report 不重新采样，只读取 perf.data 中已记录的事件
 *   2. 符号解析是关键步骤：IP 地址 → 函数名需要 ELF/DWARF 信息
 *   3. hist_entry 的 period 累加机制决定了排名
 *   4. -g 选项会额外解析调用栈，构建调用树
 *   5. 没有调试信息时，函数名显示为 [unknown]（安装 debuginfo 可解决）
 */
