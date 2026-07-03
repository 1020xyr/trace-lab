/*
 * ======================================================================
 * Step 7: bpftrace probe 附着 — 从 DSL 到内核钩子的完整旅程
 * ======================================================================
 *
 * 原始路径：
 *   src/bpftrace/src/attached_probe.h    — AttachedProbe 基类定义
 *   src/bpftrace/src/attached_probe.cpp  — 各类型 probe 的附着实现
 *   src/bpftrace/src/ast/passes/codegen_llvm.h — 编译流水线定义
 *
 * 本文件展示 bpftrace 如何将用户的 .bt 脚本变成在内核中运行的 BPF 程序。
 * 我们关注两个核心流程：
 *   1. 编译流程：DSL → AST → LLVM IR → BPF bytecode
 *   2. 附着流程：BPF bytecode → 挂载到 kprobe/tracepoint/uprobe/profile
 *
 * 预计阅读时间：25 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ 核心 #1: 编译流水线 — 从 .bt 脚本到 BPF 字节码
 * ======================================================================
 *
 * bpftrace 的编译流程（src/ast/passes/codegen_llvm.h）：
 *
 * ┌──────────────────────────────────────────────────────────┐
 * │  bpftrace 编译流水线                                       │
 * │                                                           │
 * │  kprobe:vfs_read { printf("%s\n", comm); }                │
 * │      │                                                    │
 * │      ▼                                                    │
 * │  ① Parser（递归下降解析器）                                │
 * │     → 生成 AST（抽象语法树）                              │
 * │      │                                                    │
 * │      ▼                                                    │
 * │  ② AST Passes（语义分析 + 类型检查）                      │
 * │     ├─ FieldAnalyser     — 分析结构体字段访问              │
 * │     ├─ SemanticAnalyser  — 类型检查、变量解析              │
 * │     ├─ ResourceAnalyser  — 检查 Map/builtins 使用         │
 * │     ├─ ClangParser       — 解析内核头文件（BTF/BCC）      │
 * │     ├─ TracepointFormatParser — 解析 tracepoint 格式      │
 * │     ├─ ProbePrune        — 裁剪不需要的 probe             │
 * │     └─ FoldLiterals      — 常量折叠优化                   │
 * │      │                                                    │
 * │      ▼                                                    │
 * │  ③ CodegenLLVM — 生成 LLVM IR                            │
 * │     → 将 AST 节点转换为 LLVM 中间表示                     │
 * │     → 使用 IRBuilderBPF 生成 BPF 特定的 IR               │
 * │      │                                                    │
 * │      ▼                                                    │
 * │  ④ LinkBitcode — 链接外部 bitcode                        │
 * │     → 链接 bpftrace 标准库和外部 BPF 函数                 │
 * │      │                                                    │
 * │      ▼                                                    │
 * │  ⑤ Verify — LLVM 模块验证                                │
 * │      │                                                    │
 * │      ▼                                                    │
 * │  ⑥ Optimize — LLVM 优化                                  │
 * │     → 死代码消除、常量传播、内联等                         │
 * │      │                                                    │
 * │      ▼                                                    │
 * │  ⑦ CreateObject — 生成 ELF 格式的 BPF 字节码             │
 * │     → 输出 .o 文件（包含 BPF 程序 section）               │
 * │      │                                                    │
 * │      ▼                                                    │
 * │  ⑧ Link — 最终链接                                       │
 * │     → 解析外部符号、生成最终的 BPF 对象                   │
 * │                                                           │
 * └──────────────────────────────────────────────────────────┘
 *
 * 编译完成后，通过 libbpf 加载到内核：
 *   bpf_object__load() → bpf(BPF_PROG_LOAD) → verifier → JIT
 */

/*
 * 来自 src/bpftrace/src/ast/passes/codegen_llvm.h 的编译 pass 定义：
 *
 * AllCompilePasses() 返回完整的编译流水线，按顺序执行：
 */
inline std::vector<Pass> AllCompilePasses()
{
  std::vector<Pass> passes;
  passes.emplace_back(CreateCompilePass());      // AST → LLVM IR
  passes.emplace_back(CreateLinkBitcodePass());  // 链接外部 bitcode
  passes.emplace_back(CreateVerifyPass());       // LLVM 模块验证
  passes.emplace_back(CreateOptimizePass());     // LLVM 优化
  passes.emplace_back(CreateObjectPass());       // 生成 ELF BPF 字节码
  passes.emplace_back(CreateExternObjectPass()); // 外部对象链接
  passes.emplace_back(CreateLinkPass());         // 最终链接
  return passes;
}


/* ======================================================================
 * ★ 核心 #2: Probe 类型映射 — DSL probe 到 BPF 程序类型
 * ======================================================================
 *
 * 来自 src/bpftrace/src/attached_probe.cpp 的 progtype() 函数：
 *
 * 这个函数将 bpftrace DSL 中的 probe 类型映射到内核的 BPF 程序类型。
 * 它决定了 BPF 程序加载时使用什么 prog_type，进而影响 verifier 的
 * 验证规则和可访问的 helper 函数。
 */

/* progtype(): 将 bpftrace probe 类型映射到 BPF 程序类型 */
bpf_prog_type progtype(ProbeType t)
{
  switch (t) {
      /* special/test/benchmark 使用特殊类型 */
    case ProbeType::special:    return BPF_PROG_TYPE_RAW_TRACEPOINT; break;
    case ProbeType::test:       return BPF_PROG_TYPE_XDP; break;
    case ProbeType::benchmark:  return BPF_PROG_TYPE_XDP; break;

    /* ★ kprobe/kretprobe/uprobe/uretprobe/usdt 全部使用 KPROBE 类型
     * 这是 BPF 追踪最常用的程序类型。
     * kprobe 和 kretprobe 的区别在附着时指定（opts.retprobe = true/false） */
    case ProbeType::kprobe:     return BPF_PROG_TYPE_KPROBE; break;
    case ProbeType::kretprobe:  return BPF_PROG_TYPE_KPROBE; break;
    case ProbeType::uprobe:     return BPF_PROG_TYPE_KPROBE; break;
    case ProbeType::uretprobe:  return BPF_PROG_TYPE_KPROBE; break;
    case ProbeType::usdt:       return BPF_PROG_TYPE_KPROBE; break;

    /* ★ tracepoint 使用 TRACEPOINT 类型
     * 内核 tracepoint 有固定的参数格式 */
    case ProbeType::tracepoint: return BPF_PROG_TYPE_TRACEPOINT; break;

    /* ★ profile/interval/software/hardware 使用 PERF_EVENT 类型
     * 这些是基于 perf 事件的采样型 probe */
    case ProbeType::profile:    return BPF_PROG_TYPE_PERF_EVENT; break;
    case ProbeType::interval:   return BPF_PROG_TYPE_PERF_EVENT; break;
    case ProbeType::software:   return BPF_PROG_TYPE_PERF_EVENT; break;
    case ProbeType::watchpoint: return BPF_PROG_TYPE_PERF_EVENT; break;
    case ProbeType::hardware:   return BPF_PROG_TYPE_PERF_EVENT; break;

    /* fentry/fexit/iter 使用新一代 TRACING 类型
     * 基于 BTF，比 kprobe 更高效（无 int3 断点开销） */
    case ProbeType::fentry:     return BPF_PROG_TYPE_TRACING; break;
    case ProbeType::fexit:      return BPF_PROG_TYPE_TRACING; break;
    case ProbeType::iter:       return BPF_PROG_TYPE_TRACING; break;
    case ProbeType::rawtracepoint: return BPF_PROG_TYPE_TRACING; break;

    case ProbeType::invalid:
      LOG(BUG) << "program type invalid";
  }
  return {};
}

/*
 * ★ 关键观察：
 *   kprobe/kretprobe/uprobe/uretprobe/usdt 全部使用 BPF_PROG_TYPE_KPROBE
 *   但它们在内核中的附着机制完全不同：
 *   - kprobe: 在内核函数入口插入 int3 断点
 *   - uprobe: 在用户态函数入口插入 int3 断点
 *   - usdt:   在用户态预定义的 nop 位置插入 int3
 *
 *   profile/interval/software/hardware 全部使用 BPF_PROG_TYPE_PERF_EVENT
 *   但它们触发 BPF 程序的事件源不同：
 *   - profile:  CPU 时钟采样（PERF_COUNT_SW_CPU_CLOCK）
 *   - interval: 定时器（只在一个 CPU 上触发）
 *   - software: 软件 perf 事件（page fault/context switch 等）
 *   - hardware: 硬件性能计数器（cache miss/branch miss 等）
 */


/* ======================================================================
 * ★ 核心 #3: AttachedProbe 工厂 — 统一附着入口
 * ======================================================================
 *
 * 来自 src/bpftrace/src/attached_probe.h 的基类定义：
 *
 * AttachedProbe 是所有已附着 probe 的基类，使用工厂模式创建具体子类。
 * 每个子类封装了特定 probe 类型的附着和清理逻辑。
 */

class AttachedProbe {
public:
  /* ★ 工厂方法：根据 probe 类型创建对应的子类 */
  static Result<std::unique_ptr<AttachedProbe>> make(
      Probe &probe,              // probe 描述（类型、函数名、偏移等）
      const BpfProgram &prog,    // 编译好的 BPF 程序
      std::optional<int> pid,    // 目标进程（uprobe/profile 用）
      bool safe_mode = true      // 安全模式（检查偏移有效性）
  );

  virtual ~AttachedProbe() = default;  // 析构时自动清理附着

  /* 获取 link 的文件描述符 */
  virtual int link_fd() { return -1; }

  /* 获取实际附着的 probe 数量（multi-kprobe/uprobe 可能 >1） */
  virtual size_t probe_count() const { return 1; }

  /* 获取 probe 描述 */
  const Probe &probe() const { return probe_; }

protected:
  AttachedProbe(const Probe &probe);
  const Probe &probe_;       // probe 描述信息
};


/*
 * AttachedProbe::make() — 工厂方法的实现
 *
 * 来自 src/bpftrace/src/attached_probe.cpp:
 *
 * 根据 probe.type 分派到不同的子类构造函数。
 * 每个子类的 make() 方法执行实际的附着操作。
 */
Result<std::unique_ptr<AttachedProbe>> AttachedProbe::make(
    Probe &probe,
    const BpfProgram &prog,
    std::optional<int> pid,
    bool safe_mode)
{
  LOG(V1) << "Trying to attach probe: " << probe.name;
  switch (probe.type) {
    case ProbeType::kprobe:
    case ProbeType::kretprobe:
      if (!probe.funcs.empty()) {
        /* ★ 多 kprobe：使用 BPF_LINK_TYPE_KPROBE_MULTI
         * 一次附着多个函数，比逐个附着更高效 */
        return AttachedMultiKprobeProbe::make(probe, prog);
      }
      return AttachedKprobeProbe::make(probe, prog);

    case ProbeType::tracepoint:
      return AttachedTracepointProbe::make(probe, prog);

    case ProbeType::profile:
      return AttachedProfileProbe::make(probe, prog, pid);

    case ProbeType::interval:
      return AttachedIntervalProbe::make(probe, prog);

    case ProbeType::software:
      return AttachedSoftwareProbe::make(probe, prog, pid);

    case ProbeType::hardware:
      return AttachedHardwareProbe::make(probe, prog, pid);

    case ProbeType::fentry:
    case ProbeType::fexit:
      return AttachedFentryProbe::make(probe, prog);

    case ProbeType::iter:
      return AttachedIterProbe::make(probe, prog, pid);

    case ProbeType::rawtracepoint:
      return AttachedRawtracepointProbe::make(probe, prog);

    case ProbeType::usdt:
      return AttachedUSDTProbe::make(probe, prog, pid);

    case ProbeType::uprobe:
    case ProbeType::uretprobe:
      if (!probe.funcs.empty()) {
        return AttachedMultiUprobeProbe::make(probe, prog, pid);
      }
      return AttachedUprobeProbe::make(probe, prog, pid, safe_mode);

    /* ... */
  }
}


/* ======================================================================
 * ★ 核心 #4: kprobe 附着 — 在内核函数入口插入断点
 * ======================================================================
 *
 * 来自 src/bpftrace/src/attached_probe.cpp:
 *
 * kprobe 的附着流程：
 *   1. 解析函数名 + 偏移（如 vfs_read+0x10）
 *   2. 通过 libbpf 的 bpf_program__attach_kprobe_opts() 附着
 *   3. libbpf 内部：
 *      a. 写入 /sys/kernel/debug/tracing/kprobe_events
 *         格式: p:kprobes/p_vfs_read_0 vfs_read+0x10
 *      b. 打开 perf_event（类型为 PERF_TYPE_TRACEPOINT）
 *      c. 通过 ioctl(PERF_EVENT_IOC_SET_BPF) 将 BPF 程序关联到事件
 *      d. 创建 bpf_link 管理生命周期
 */

class AttachedKprobeProbe : public AttachedProbe {
public:
  static Result<std::unique_ptr<AttachedKprobeProbe>> make(
      Probe &probe,
      const BpfProgram &prog);
  ~AttachedKprobeProbe() override;   // ★ 析构时自动销毁 link
  int link_fd() override;
private:
  AttachedKprobeProbe(const Probe &probe, struct bpf_link *link);
  struct bpf_link *link_;           // ★ libbpf 管理的 link 对象
};

/* 析构函数：自动清理 kprobe 附着 */
AttachedKprobeProbe::~AttachedKprobeProbe()
{
  if (bpf_link__destroy(link_)) {
    LOG(WARNING) << "failed to destroy link for kprobe probe: "
                 << strerror(errno);
  }
  /* bpf_link__destroy() 内部会：
   * 1. 关闭 perf_event fd
   * 2. 从 /sys/kernel/debug/tracing/kprobe_events 中删除条目
   * 3. 释放 bpf_link 结构 */
}

/* ★ kprobe 附着的核心逻辑 */
Result<std::unique_ptr<AttachedKprobeProbe>> AttachedKprobeProbe::make(
    Probe &probe,
    const BpfProgram &prog)
{
  /* 构造 "module:function" 格式的函数名
   * 如果指定了内核模块（如 ext4），格式为 "ext4:ext4_file_write_iter" */
  std::string funcname;
  if (!probe.path.empty() && probe.path != "vmlinux") {
    funcname = probe.path + ":" + probe.attach_point;
  } else {
    funcname = probe.attach_point;  // 例如: "vfs_read"
  }

  const char *kprobe_func;
  uint64_t offset;

  /* kprobe 可以通过符号名+偏移 或 绝对地址 两种方式指定
   * 符号名+偏移：kprobe:vfs_read+0x10
   * 绝对地址：    kprobe:0xffffffff81234560（需要 unsafe 模式） */
  bool is_symbol_kprobe = !probe.attach_point.empty();
  if (is_symbol_kprobe) {
    /* 解析偏移量：检查 offset 是否在函数范围内 */
    auto offset_res = resolve_offset_kprobe(probe);
    if (!offset_res) {
      return offset_res.takeError();
    }
    kprobe_func = funcname.c_str();
    offset = *offset_res;
  } else {
    /* 地址模式：函数名为 NULL，内核将 offset 解释为绝对地址 */
    kprobe_func = nullptr;
    offset = probe.address;
  }

  /* ★ 通过 libbpf 附着 kprobe
   * bpf_kprobe_opts 配置 kprobe 选项 */
  DECLARE_LIBBPF_OPTS(bpf_kprobe_opts, opts);
  opts.offset = offset;                            // 函数内偏移
  opts.retprobe = probe.type == ProbeType::kretprobe;  // kretprobe = 函数返回时触发

  /* ★ 核心调用：libbpf 完成实际的附着工作
   * 内部流程：
   * 1. 通过 ftrace 接口创建 kprobe
   * 2. 打开 perf_event
   * 3. 通过 BPF_LINK_CREATE 创建 bpf_link
   * 4. 返回 link 对象管理生命周期 */
  auto *link = bpf_program__attach_kprobe_opts(prog.bpf_prog(),
                                               kprobe_func,
                                               &opts);
  if (!link) {
    if (errno == EILSEQ)
      return make_error<AttachError>(
          "Possible attachment attempt in the middle of an instruction, "
          "try a different offset.");
    return make_error<AttachError>();
  }

  return std::unique_ptr<AttachedKprobeProbe>(
      new AttachedKprobeProbe(probe, link));
}


/* ======================================================================
 * ★ 核心 #5: tracepoint 附着 — 连接到内核静态追踪点
 * ======================================================================
 *
 * tracepoint 是内核预定义的静态追踪点，比 kprobe 更稳定。
 * 格式：tracepoint:category:event（如 tracepoint:syscalls:sys_enter_read）
 *
 * 附着流程比 kprobe 简单得多：
 * 1. 从 probe.path 获取 category（如 "syscalls"）
 * 2. 从 probe 获取 event name（如 "sys_enter_read"）
 * 3. 直接调用 libbpf 的 bpf_program__attach_tracepoint()
 */

class AttachedTracepointProbe : public AttachedProbe {
public:
  static Result<std::unique_ptr<AttachedTracepointProbe>> make(
      Probe &probe,
      const BpfProgram &prog);
  ~AttachedTracepointProbe() override;
private:
  AttachedTracepointProbe(const Probe &probe, struct bpf_link *link);
  struct bpf_link *link_;
};

AttachedTracepointProbe::~AttachedTracepointProbe()
{
  if (bpf_link__destroy(link_)) {
    LOG(WARNING) << "failed to destroy link for tracepiont probe: "
                 << strerror(errno);
  }
}

/* ★ tracepoint 附着：一行核心调用 */
Result<std::unique_ptr<AttachedTracepointProbe>> AttachedTracepointProbe::make(
    Probe &probe,
    const BpfProgram &prog)
{
  /* bpf_program__attach_tracepoint() 内部流程：
   * 1. 从 /sys/kernel/debug/tracing/events/<category>/<event>/id 读取事件 ID
   * 2. 打开 perf_event（type=PERF_TYPE_TRACEPOINT, config=event_id）
   * 3. 通过 BPF_LINK_CREATE 创建 bpf_link
   * 4. 启用 perf_event
   */
  auto *link = bpf_program__attach_tracepoint(prog.bpf_prog(),
                                              probe.path.c_str(),        // category
                                              eventname(probe, 0).c_str()); // event name

  if (!link) {
    return make_error<AttachError>();
  }

  return std::unique_ptr<AttachedTracepointProbe>(
      new AttachedTracepointProbe(probe, link));
}


/* ======================================================================
 * ★ 核心 #6: uprobe 附着 — 在用户态函数入口插入断点
 * ======================================================================
 *
 * uprobe 与 kprobe 类似，但目标是用户态进程的函数。
 * 格式：uprobe:/path/to/binary:function_name
 *       uprobe:/lib/libc.so.6:malloc
 *
 * 附着流程：
 * 1. 解析 ELF 二进制文件，找到函数的虚拟地址
 * 2. 计算文件偏移 = 虚拟地址 - 加载基地址
 * 3. 通过 libbpf 附着 uprobe
 */

Result<std::unique_ptr<AttachedUprobeProbe>> AttachedUprobeProbe::make(
    Probe &probe,
    const BpfProgram &prog,
    std::optional<int> pid,
    bool safe_mode)
{
  /* ★ 解析偏移量：读取 ELF 文件，找到函数在文件中的偏移 */
  auto offset_res = resolve_offset_uprobe(probe, safe_mode);
  if (!offset_res) {
    return offset_res.takeError();
  }

  uint64_t offset = *offset_res;

  DECLARE_LIBBPF_OPTS(bpf_uprobe_opts, opts);
  opts.retprobe = probe.type == ProbeType::uretprobe;

  /* 如果目标是 zip 归档中的库，需要指定函数名 */
  if (!probe.attach_point.empty() && util::is_archive_path(probe.path)) {
    opts.func_name = probe.attach_point.c_str();
    offset = probe.func_offset;
  }

  /* ★ 核心调用：libbpf 附着 uprobe
   * 内部流程：
   * 1. 打开 /sys/kernel/debug/tracing/uprobe_events
   *    写入: p:uprobes/p_malloc_0 /lib/libc.so.6:0x87540
   * 2. 打开 perf_event（指定 pid 过滤进程）
   * 3. 创建 bpf_link */
  auto *link = bpf_program__attach_uprobe_opts(prog.bpf_prog(),
                                               pid.has_value() ? *pid : -1,
                                               probe.path.c_str(),  // 二进制路径
                                               offset,              // 文件内偏移
                                               &opts);

  if (!link) {
    return make_error<AttachError>();
  }

  return std::unique_ptr<AttachedUprobeProbe>(
      new AttachedUprobeProbe(probe, link));
}


/* ======================================================================
 * ★ 核心 #7: profile 附着 — 基于 CPU 时钟采样
 * ======================================================================
 *
 * profile probe 是采样型 probe，基于 perf_event 的 CPU 时钟事件。
 * 格式：profile:hz:99（每秒采样 99 次，选奇数避免与调度频率共振）
 *       profile:ms:10（每 10 毫秒采样一次）
 *
 * 与 kprobe/tracepoint 不同，profile 需要在每个 CPU 上都创建
 * perf_event，因此会返回多个 bpf_link。
 */

/* perf_event_open 的封装 */
int open_perf_event(uint32_t ev_type,      /* PERF_TYPE_SOFTWARE/HARDWARE */
                    uint32_t ev_config,    /* PERF_COUNT_SW_CPU_CLOCK 等 */
                    uint64_t sample_period,/* 采样周期（0 = 使用频率） */
                    uint64_t sample_freq,  /* 采样频率（0 = 使用周期） */
                    pid_t pid,             /* 目标进程（-1 = 所有进程） */
                    int cpu,               /* 目标 CPU */
                    int group_fd)          /* 事件组 fd */
{
  struct perf_event_attr attr = {};
  attr.type = ev_type;
  attr.size = sizeof(struct perf_event_attr);
  attr.config = ev_config;
  if (sample_freq > 0) {
    attr.freq = 1;
    attr.sample_freq = sample_freq;    /* 频率模式 */
  } else {
    attr.sample_period = sample_period; /* 周期模式 */
  }
  if (pid > 0)
    attr.inherit = 1;                  /* 子进程继承 */

  /* ★ 核心系统调用：打开 perf 事件 */
  return syscall(
      __NR_perf_event_open, &attr, pid, cpu, group_fd, PERF_FLAG_FD_CLOEXEC);
}

Result<std::unique_ptr<AttachedProfileProbe>> AttachedProfileProbe::make(
    Probe &probe,
    const BpfProgram &prog,
    std::optional<int> pid)
{
  /* 解析采样参数：hz(频率)/s(秒)/ms(毫秒)/us(微秒) */
  uint64_t period, freq;
  if (probe.path == "hz") {
    period = 0;
    freq = probe.freq;          /* 频率模式：每秒 N 次 */
  } else if (probe.path == "s") {
    period = probe.freq * 1e9;  /* 周期模式：N 秒 = N * 10^9 纳秒 */
    freq = 0;
  } else if (probe.path == "ms") {
    period = probe.freq * 1e6;  /* N 毫秒 */
    freq = 0;
  } else if (probe.path == "us") {
    period = probe.freq * 1e3;  /* N 微秒 */
    freq = 0;
  }

  /* ★ 在每个在线 CPU 上创建 perf_event 并附着 BPF 程序 */
  std::vector<struct bpf_link *> links;
  std::vector<int> cpus = util::get_online_cpus();
  for (int cpu : cpus) {
    /* 1. 打开 perf_event：CPU 时钟事件 */
    int perf_event_fd = open_perf_event(PERF_TYPE_SOFTWARE,
                                        PERF_COUNT_SW_CPU_CLOCK,
                                        period, freq,
                                        pid.has_value() ? *pid : -1,
                                        cpu, -1);

    /* 2. 将 BPF 程序附着到 perf_event */
    auto *link = bpf_program__attach_perf_event(prog.bpf_prog(), perf_event_fd);
    links.push_back(link);
  }

  return std::unique_ptr<AttachedProfileProbe>(
      new AttachedProfileProbe(probe, links));
}


/*
 * ★ 总结：bpftrace probe 附着的完整数据流
 *
 *  用户脚本:
 *    kprobe:vfs_read { printf("read by %s\n", comm); }
 *
 *  ┌──────────────────────────────────────────────────────────┐
 *  │  bpftrace 执行流程                                        │
 *  │                                                           │
 *  │  ① 解析脚本 → AST                                        │
 *  │     Probe { type=kprobe, attach_point="vfs_read" }       │
 *  │      │                                                    │
 *  │      ▼                                                    │
 *  │  ② 语义分析 → 检查 probe 合法性                          │
 *  │     - vfs_read 是有效的内核函数吗？                       │
 *  │     - comm 是合法的内置变量吗？                           │
 *  │      │                                                    │
 *  │      ▼                                                    │
 *  │  ③ 代码生成 → LLVM IR → BPF bytecode                    │
 *  │     prog_type = BPF_PROG_TYPE_KPROBE                     │
 *  │      │                                                    │
 *  │      ▼                                                    │
 *  │  ④ bpf(BPF_PROG_LOAD)                                    │
 *  │     → verifier 静态分析                                   │
 *  │     → JIT 编译                                            │
 *  │     → 返回 prog_fd                                        │
 *  │      │                                                    │
 *  │      ▼                                                    │
 *  │  ⑤ AttachedKprobeProbe::make()                           │
 *  │     → bpf_program__attach_kprobe_opts()                  │
 *  │       ├─ 写入 kprobe_events（ftrace 接口）               │
 *  │       ├─ perf_event_open（打开追踪事件）                  │
 *  │       ├─ BPF_LINK_CREATE（创建 link）                    │
 *  │       └─ 返回 bpf_link 对象                              │
 *  │      │                                                    │
 *  │      ▼                                                    │
 *  │  ⑥ 运行中：                                              │
 *  │     进程调用 vfs_read()                                   │
 *  │       → int3 断点触发                                     │
 *  │       → kprobe handler                                    │
 *  │       → 执行 BPF 程序                                     │
 *  │       → printf 输出到 perf buffer                         │
 *  │       → 用户态 poll perf buffer 读取输出                  │
 *  │                                                           │
 *  │  ⑦ 退出时：                                              │
 *  │     ~AttachedKprobeProbe()                               │
 *  │       → bpf_link__destroy()                              │
 *  │       → 清理 kprobe + perf_event                          │
 *  └──────────────────────────────────────────────────────────┘
 *
 * ★ 与 blktrace 对比：
 *
 * +---------------+---------------------------+---------------------------+
 * |               | blktrace                   | bpftrace                   |
 * +---------------+---------------------------+---------------------------+
 * | 追踪机制       | relay buffer              | perf buffer / ring buffer  |
 * | 内核接口       | ioctl(BLKTRACESETUP)      | bpf() 系统调用             |
 * | 用户态读取     | mmap relay 文件            | poll perf_event fd         |
 * | 数据格式       | 固定 blk_io_trace 结构     | 用户自定义（灵活）          |
 * | 事件过滤       | act_mask 位掩码            | BPF 程序内 if 判断         |
 * | 附着方式       | debugfs + relay            | ftrace + perf_event + link |
 * +---------------+---------------------------+---------------------------+
 */
