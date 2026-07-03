/*
 * ======================================================================
 * Step 8: 内核 verifier.c — BPF 程序验证器（安全守门人）
 * ======================================================================
 *
 * 原始路径：src/linux-5.10/kernel/bpf/verifier.c (12000+ 行)
 *           src/linux-5.10/include/linux/bpf_verifier.h
 *           src/linux-5.10/include/linux/bpf.h (bpf_reg_type 定义)
 *
 * BPF verifier（验证器）是 BPF 子系统中最重要的安全组件。
 * 它在 BPF 程序被允许执行之前，对程序的每一条指令进行静态分析，
 * 确保程序不会做危险的操作（越界访问、无限循环、泄露内核指针等）。
 *
 * 我们关注四个核心部分：
 *   1. bpf_check()    — 验证器入口（总体流程）
 *   2. do_check()     — 指令遍历主循环
 *   3. check_mem_access() — 内存访问安全检查
 *   4. 寄存器类型系统 — verifier 如何追踪每个寄存器的含义
 *
 * 预计阅读时间：30 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ 核心 #1: bpf_check() — 验证器入口（总体流程）
 * ======================================================================
 *
 * bpf_check() 是 BPF 验证的入口函数，在 bpf(BPF_PROG_LOAD) 时被调用。
 * 它协调多个验证阶段，任何一个失败都会导致程序被拒绝。
 *
 * 验证器的本质是一个静态代码分析器：
 * - 它不执行程序，而是模拟每条指令对寄存器/栈的影响
 * - 它追踪每个寄存器中值的类型（是指针？是标量？可能为 NULL？）
 * - 它检查所有可能的执行路径（条件分支的两个方向都会分析）
 *
 * ★ 限制：
 * - 程序最多 BPF_MAXINSNS（4096）条指令
 * - 分析最多 BPF_COMPLEXITY_LIMIT_INSNS（1,000,000）步
 * - 分支路径最多 65536 条
 * - 最大栈深度 512 字节
 */

int bpf_check(struct bpf_prog **prog, union bpf_attr *attr,
              union bpf_attr __user *uattr)
{
    u64 start_time = ktime_get_ns();
    struct bpf_verifier_env *env;
    struct bpf_verifier_log *log;
    int i, len, ret = -EINVAL;
    bool is_priv;

    /* 分配验证器环境（包含所有验证状态） */
    env = kzalloc(sizeof(struct bpf_verifier_env), GFP_KERNEL);
    if (!env)
        return -ENOMEM;
    log = &env->log;

    /* 为每条指令分配辅助数据（记录每条指令的分析状态） */
    len = (*prog)->len;
    env->insn_aux_data =
        vzalloc(array_size(sizeof(struct bpf_insn_aux_data), len));
    for (i = 0; i < len; i++)
        env->insn_aux_data[i].orig_idx = i;

    env->prog = *prog;
    /* ★ 根据程序类型获取对应的验证操作表
     * 不同类型的 BPF 程序有不同的验证规则（如可访问的 helper 函数不同） */
    env->ops = bpf_verifier_ops[env->prog->type];
    is_priv = bpf_capable();

    /* 获取内核 BTF 信息（用于验证 kfunc 调用和 PTR_TO_BTF_ID） */
    bpf_get_btf_vmlinux();

    /* 设置验证器日志（用户态通过 log_buf 接收验证过程/错误信息） */
    if (attr->log_level || attr->log_buf || attr->log_size) {
        log->level = attr->log_level;
        log->ubuf = (char __user *) (unsigned long) attr->log_buf;
        log->len_total = attr->log_size;
        /* 日志缓冲区至少 128 字节 */
        if (log->len_total < 128 || log->len_total > UINT_MAX >> 2 ||
            !log->level || !log->ubuf)
            goto err_unlock;
    }

    /* 设置验证选项 */
    env->strict_alignment = !!(attr->prog_flags & BPF_F_STRICT_ALIGNMENT);
    env->allow_ptr_leaks = bpf_allow_ptr_leaks();
    env->bpf_capable = bpf_capable();

    /* 分配已探索状态的哈希表（用于状态剪枝优化） */
    env->explored_states = kvcalloc(state_htab_size(env),
                                    sizeof(struct bpf_verifier_state_list *),
                                    GFP_USER);

    /* ===== 验证阶段 ===== */

    /* 阶段 1: 检查子程序（BPF 支持函数调用，需要先识别所有子程序） */
    ret = check_subprogs(env);

    /* 阶段 2: 检查 BTF 信息（调试信息验证） */
    ret = check_btf_info(env, attr, uattr);

    /* 阶段 3: 检查 attach_btf_id（fentry/fexit 目标验证） */
    ret = check_attach_btf_id(env);

    /* 阶段 4: 解析伪 ldimm64 指令（将 map fd 替换为实际 map 指针） */
    ret = resolve_pseudo_ldimm64(env);

    /* 阶段 5: ★ 检查控制流图（CFG）
     * 第一遍扫描，检查：
     * - 程序是否是有向无环图（DAG）— 不允许循环
     * - 是否有不可达指令
     * - 跳转是否越界
     * - 总指令数是否超限 */
    ret = check_cfg(env);

    /* 阶段 6: ★★ 核心验证 — 逐条指令分析
     * 先验证所有子程序，再验证主程序 */
    ret = do_check_subprogs(env);
    ret = ret ?: do_check_main(env);

    /* ===== 验证通过后的优化和修补 ===== */

    /* 检查最大栈深度 */
    if (ret == 0)
        ret = check_max_stack_depth(env);

    /* 指令优化（特权模式下） */
    if (is_priv) {
        if (ret == 0)
            opt_hard_wire_dead_code_branches(env);  /* 消除死代码分支 */
        if (ret == 0)
            ret = opt_remove_dead_code(env);         /* 移除死代码 */
        if (ret == 0)
            ret = opt_remove_nops(env);              /* 移除空操作 */
    }

    /* ★ 将 *(u32*)(ctx + off) 转换为实际的上下文访问指令
     * 不同程序类型的 ctx 结构不同，这里做具体的指令替换 */
    if (ret == 0)
        ret = convert_ctx_accesses(env);

    /* 修补 BPF helper 调用（内联或指向实际函数地址） */
    if (ret == 0)
        ret = fixup_bpf_calls(env);

    /* ... 记录验证时间等统计信息 ... */
    return ret;
}


/* ======================================================================
 * ★ 核心 #2: 验证器环境（bpf_verifier_env）
 * ======================================================================
 *
 * bpf_verifier_env 是验证器的核心数据结构，保存所有验证状态。
 *
 * 来自 src/linux-5.10/include/linux/bpf_verifier.h:
 */

struct bpf_verifier_env {
    u32 insn_idx;                    /* 当前正在分析的指令索引 */
    u32 prev_insn_idx;               /* 上一条指令索引 */
    struct bpf_prog *prog;           /* ★ 正在被验证的 BPF 程序 */
    const struct bpf_verifier_ops *ops; /* 程序类型对应的验证操作 */

    /* ★ 验证状态栈（用于深度优先搜索所有路径）
     * 遇到条件分支时，将当前状态压栈，分析一个分支，
     * 然后弹出状态，分析另一个分支 */
    struct bpf_verifier_stack_elem *head;
    int stack_size;

    bool strict_alignment;           /* 严格对齐检查 */
    bool test_state_freq;            /* 测试用：不同剪枝频率 */

    struct bpf_verifier_state *cur_state; /* ★ 当前验证状态
                                            * 包含所有寄存器的类型信息和栈状态 */

    /* 已探索状态表（用于状态剪枝优化）
     * ★ 如果当前状态与之前某次到达同一指令时的状态等价，
     *   则可以剪枝，不需要重新分析后续指令 */
    struct bpf_verifier_state_list **explored_states;

    struct bpf_map *used_maps[MAX_USED_MAPS]; /* 程序使用的 Map 列表 */
    u32 used_map_cnt;

    u32 id_gen;                      /* 唯一 ID 生成器（用于指针引用追踪） */
    bool allow_ptr_leaks;            /* 是否允许指针泄露（特权模式） */
    bool allow_ptr_to_map_access;    /* 是否允许指向 Map 的指针访问 */
    bool bpf_capable;                /* 进程是否有 BPF 能力 */
    bool bypass_spec_v1;             /* 绕过 Spectre v1 缓解 */
    bool bypass_spec_v4;             /* 绕过 Spectre v4 缓解 */
    bool seen_direct_write;          /* 是否看到直接写入（用于优化） */

    struct bpf_insn_aux_data *insn_aux_data; /* ★ 每条指令的辅助数据 */

    struct bpf_verifier_log log;     /* 验证器日志（输出给用户态） */

    /* 子程序信息 */
    struct bpf_subprog_info subprog_info[BPF_MAX_SUBPROGS + 1];
    u32 subprog_cnt;

    /* 控制流图（CFG）分析用的临时数据 */
    struct {
        int *insn_state;
        int *insn_stack;
        int cur_stack;
    } cfg;

    /* 验证统计 */
    u32 pass_cnt;                    /* do_check() 调用次数 */
    u32 prev_insn_processed, insn_processed;  /* 已分析的指令数 */
    u32 prev_jmps_processed, jmps_processed;  /* 已分析的跳转数 */
    u64 verification_time;           /* 总验证时间（纳秒） */
    u32 max_states_per_insn;         /* 单条指令的最大状态数 */
    u32 total_states;                /* 总分配的状态数 */
    u32 peak_states;                 /* 峰值状态数（内存消耗主要来源） */
    u32 longest_mark_read_walk;      /* 最长的 liveness 标记遍历链 */
};


/* ======================================================================
 * ★ 核心 #3: 寄存器类型系统 — verifier 如何理解每个值的含义
 * ======================================================================
 *
 * verifier 的核心思想：为每个寄存器维护一个"类型"，
 * 表示这个寄存器当前持有什么类型的值。不同的类型有不同的访问规则。
 *
 * 来自 src/linux-5.10/include/linux/bpf.h:
 */

enum bpf_reg_type {
    NOT_INIT = 0,            /* ★ 未初始化 — 读取未初始化的寄存器会被拒绝 */
    SCALAR_VALUE,            /* ★ 标量值 — 不包含有效指针的普通数值 */

    /* ---- 指针类型（每种指针有不同的访问范围和规则）---- */
    PTR_TO_CTX,              /* ★ 指向 bpf_context（程序入口 R1 的初始类型） */
    CONST_PTR_TO_MAP,        /* 指向 struct bpf_map（常量 Map 指针） */
    PTR_TO_MAP_VALUE,        /* ★ 指向 Map 元素的 value（可读写） */
    PTR_TO_MAP_VALUE_OR_NULL,/* 指向 Map value 或 NULL（需要检查后才能使用） */
    PTR_TO_STACK,            /* ★ 指向栈（frame_pointer + offset） */
    PTR_TO_PACKET_META,      /* 指向 skb->data - meta_len */
    PTR_TO_PACKET,           /* 指向 skb->data（网络包数据） */
    PTR_TO_PACKET_END,       /* 指向 skb->data + headlen */
    PTR_TO_FLOW_KEYS,        /* 指向 bpf_flow_keys */

    /* socket 相关指针 */
    PTR_TO_SOCKET,           /* 指向 struct bpf_sock */
    PTR_TO_SOCKET_OR_NULL,   /* 指向 struct bpf_sock 或 NULL */
    PTR_TO_SOCK_COMMON,
    PTR_TO_SOCK_COMMON_OR_NULL,
    PTR_TO_TCP_SOCK,
    PTR_TO_TCP_SOCK_OR_NULL,
    PTR_TO_TP_BUFFER,        /* 指向 raw tracepoint 的可写缓冲区 */
    PTR_TO_XDP_SOCK,

    /* BTF 相关指针（基于类型信息的内核结构体指针） */
    PTR_TO_BTF_ID,           /* ★ 指向内核结构体（不需要 NULL 检查）
                              * 注意：虽然名字说不需要 NULL 检查，
                              * 但实际上可能是 NULL（读指针链时）。
                              * 依赖内核的 fault handling 处理。 */
    PTR_TO_BTF_ID_OR_NULL,   /* 指向内核结构体或 NULL（需要显式检查） */

    /* 通用内存指针 */
    PTR_TO_MEM,              /* 指向有效的内存区域 */
    PTR_TO_MEM_OR_NULL,
    PTR_TO_RDONLY_BUF,       /* 指向只读缓冲区 */
    PTR_TO_RDONLY_BUF_OR_NULL,
    PTR_TO_RDWR_BUF,         /* 指向读写缓冲区 */
    PTR_TO_RDWR_BUF_OR_NULL,
    PTR_TO_PERCPU_BTF_ID,    /* 指向 per-CPU 内核变量 */
};

/*
 * ★ 寄存器类型的字符串表示（用于 verifier 日志输出）
 * 当程序被拒绝时，日志中会显示这些缩写：
 */
static const char * const reg_type_str[] = {
    [NOT_INIT]                = "?",          /* 未初始化 */
    [SCALAR_VALUE]            = "inv",        /* 无效（标量） */
    [PTR_TO_CTX]              = "ctx",        /* 上下文指针 */
    [CONST_PTR_TO_MAP]        = "map_ptr",    /* Map 指针 */
    [PTR_TO_MAP_VALUE]        = "map_value",  /* Map value 指针 */
    [PTR_TO_MAP_VALUE_OR_NULL]= "map_value_or_null",
    [PTR_TO_STACK]            = "fp",         /* 栈指针（frame pointer） */
    [PTR_TO_PACKET]           = "pkt",        /* 数据包指针 */
    [PTR_TO_PACKET_META]      = "pkt_meta",
    [PTR_TO_PACKET_END]       = "pkt_end",
    [PTR_TO_SOCKET]           = "sock",       /* socket 指针 */
    [PTR_TO_SOCKET_OR_NULL]   = "sock_or_null",
    [PTR_TO_BTF_ID]           = "ptr_",       /* BTF 结构体指针 */
    [PTR_TO_BTF_ID_OR_NULL]   = "ptr_or_null_",
    [PTR_TO_MEM]              = "mem",        /* 通用内存指针 */
    [PTR_TO_MEM_OR_NULL]      = "mem_or_null",
    [PTR_TO_RDONLY_BUF]       = "rdonly_buf", /* 只读缓冲区 */
    [PTR_TO_RDWR_BUF]         = "rdwr_buf",   /* 读写缓冲区 */
};

/*
 * ★ 类型判断辅助函数
 * verifier 通过这些函数判断寄存器类型的属性：
 */

/* 判断是否为可能为 NULL 的指针类型 */
static bool reg_type_may_be_null(enum bpf_reg_type type)
{
    return type == PTR_TO_MAP_VALUE_OR_NULL ||
           type == PTR_TO_SOCKET_OR_NULL ||
           type == PTR_TO_SOCK_COMMON_OR_NULL ||
           type == PTR_TO_TCP_SOCK_OR_NULL ||
           type == PTR_TO_BTF_ID_OR_NULL ||
           type == PTR_TO_MEM_OR_NULL ||
           type == PTR_TO_RDONLY_BUF_OR_NULL ||
           type == PTR_TO_RDWR_BUF_OR_NULL;
}

/* 判断是否为引用计数类型的指针（需要显式释放） */
static bool reg_type_may_be_refcounted_or_null(enum bpf_reg_type type)
{
    return type == PTR_TO_SOCKET ||
           type == PTR_TO_SOCKET_OR_NULL ||
           type == PTR_TO_TCP_SOCK ||
           type == PTR_TO_TCP_SOCK_OR_NULL ||
           type == PTR_TO_MEM ||
           type == PTR_TO_MEM_OR_NULL;
}

/*
 * ★ NULL 检查的类型转换（check_cond_jmp_op 中触发）
 *
 * 当 verifier 遇到条件跳转 "if (reg != 0) goto +off" 时：
 *
 *   BPF 程序：                          verifier 的处理：
 *
 *   val = bpf_map_lookup_elem(map, &key);
 *   // R0 类型: PTR_TO_MAP_VALUE_OR_NULL
 *
 *   if (val == 0)                       // true 分支：R0 = CONST_IMM (值为0)
 *       return 0;                       //   → 引用被释放
 *                                       // false 分支：R0 = PTR_TO_MAP_VALUE
 *   *val = 42;                          //   → 可以安全访问 Map value
 *   // R0 类型: PTR_TO_MAP_VALUE
 *
 * 这就是为什么 BPF 程序中 helper 返回值必须检查 NULL！
 * 如果不检查，R0 保持 PTR_TO_MAP_VALUE_OR_NULL 类型，
 * 后续通过 R0 访问内存时 verifier 会拒绝。
 */


/* ======================================================================
 * ★ 核心 #4: do_check() — 指令遍历主循环
 * ======================================================================
 *
 * do_check() 是 verifier 的核心：逐条分析 BPF 指令，
 * 模拟每条指令对寄存器和栈状态的影响。
 *
 * 它使用深度优先搜索（DFS）遍历所有可能的执行路径：
 * - 遇到条件分支时，先分析一个方向，将另一个方向压栈
 * - 遇到 BPF_EXIT 时，弹出栈中保存的状态继续分析
 * - 如果当前状态与之前到达同一指令的状态等价，则剪枝
 *
 * 来自 src/linux-5.10/kernel/bpf/verifier.c:
 */

static int do_check(struct bpf_verifier_env *env)
{
    bool pop_log = !(env->log.level & BPF_LOG_LEVEL2);
    struct bpf_verifier_state *state = env->cur_state;
    struct bpf_insn *insns = env->prog->insnsi;  /* ★ BPF 指令数组 */
    struct bpf_reg_state *regs;
    int insn_cnt = env->prog->len;
    bool do_print_state = false;
    int prev_insn_idx = -1;

    /* ★ 主循环：逐条分析指令 */
    for (;;) {
        struct bpf_insn *insn;
        u8 class;
        int err;

        env->prev_insn_idx = prev_insn_idx;

        /* 检查指令索引是否越界 */
        if (env->insn_idx >= insn_cnt) {
            verbose(env, "invalid insn idx %d insn_cnt %d\n",
                    env->insn_idx, insn_cnt);
            return -EFAULT;
        }

        insn = &insns[env->insn_idx];
        class = BPF_CLASS(insn->code);  /* 提取指令类别 */

        /* ★ 复杂度检查：已分析的指令数不能超限 */
        if (++env->insn_processed > BPF_COMPLEXITY_LIMIT_INSNS) {
            verbose(env,
                    "BPF program is too large. Processed %d insn\n",
                    env->insn_processed);
            return -E2BIG;
        }

        /* ★ 状态剪枝：检查是否已经分析过等价状态
         * 如果当前到达这条指令时的寄存器/栈状态与之前某次等价，
         * 则后续分析结果也一样，可以安全跳过（剪枝） */
        err = is_state_visited(env, env->insn_idx);
        if (err < 0)
            return err;
        if (err == 1) {
            /* 找到等价状态，可以安全跳过 */
            goto process_bpf_exit;
        }

        /* 获取当前寄存器状态 */
        regs = cur_regs(env);
        prev_insn_idx = env->insn_idx;

        /* ===== 按指令类别分派检查函数 ===== */

        if (class == BPF_ALU || class == BPF_ALU64) {
            /* ★ 算术运算指令（ADD/SUB/MUL/DIV/MOV 等）
             * 更新目标寄存器的类型（如指针+立即数=新指针） */
            err = check_alu_op(env, insn);
            if (err)
                return err;

        } else if (class == BPF_LDX) {
            /* ★ 内存读取指令：dst = *(size *)(src + off)
             * 1. 检查 src 寄存器的类型是否允许被读取
             * 2. 检查偏移是否在合法范围内
             * 3. 更新 dst 寄存器的类型 */
            err = check_reg_arg(env, insn->src_reg, SRC_OP);
            if (err)
                return err;
            err = check_reg_arg(env, insn->dst_reg, DST_OP_NO_MARK);
            if (err)
                return err;

            /* ★ 核心检查：内存访问是否合法 */
            err = check_mem_access(env, env->insn_idx, insn->src_reg,
                                   insn->off, BPF_SIZE(insn->code),
                                   BPF_READ, insn->dst_reg, false);
            if (err)
                return err;

        } else if (class == BPF_STX) {
            /* ★ 内存写入指令：*(size *)(dst + off) = src
             * 检查 dst 指向的内存是否可写 */
            err = check_mem_access(env, env->insn_idx, insn->dst_reg,
                                   insn->off, BPF_SIZE(insn->code),
                                   BPF_WRITE, insn->src_reg, false);
            if (err)
                return err;

        } else if (class == BPF_JMP || class == BPF_JMP32) {
            u8 opcode = BPF_OP(insn->code);

            if (opcode == BPF_CALL) {
                /* ★ 函数调用指令
                 * BPF_PSEUDO_CALL = BPF 子程序调用
                 * 其他 = BPF helper 函数调用 */
                if (insn->src_reg == BPF_PSEUDO_CALL)
                    err = check_func_call(env, insn, &env->insn_idx);
                else
                    err = check_helper_call(env, insn->imm, env->insn_idx);
                if (err)
                    return err;

            } else if (opcode == BPF_JA) {
                /* 无条件跳转 */
                env->insn_idx += insn->off + 1;
                continue;

            } else if (opcode == BPF_EXIT) {
                /* ★ 程序退出
                 * 检查：
                 * 1. 是否有未释放的引用（如 socket 引用）
                 * 2. 返回值 R0 是否合法
                 * 3. 是否持有 spin_lock */
                err = check_reference_leak(env);
                if (err)
                    return err;
                err = check_return_code(env);
                if (err)
                    return err;

process_bpf_exit:
                /* 弹出栈中保存的状态，继续分析其他路径 */
                err = pop_stack(env, &prev_insn_idx,
                                &env->insn_idx, pop_log);
                if (err < 0) {
                    if (err != -ENOENT)
                        return err;
                    break;  /* 栈为空，所有路径分析完毕 */
                }
                continue;

            } else {
                /* ★ 条件跳转指令（if R1 > R2 goto +off）
                 * 分析两个分支：
                 * 1. true 分支的状态压栈
                 * 2. 继续分析 false 分支
                 * 在分支汇合点，合并两个分支的寄存器状态 */
                err = check_cond_jmp_op(env, insn, &env->insn_idx);
                if (err)
                    return err;
            }
        } else if (class == BPF_LD) {
            /* 立即数加载指令（64 位常量加载） */
            u8 mode = BPF_MODE(insn->code);
            if (mode == BPF_IMM) {
                err = check_ld_imm(env, insn);
                if (err)
                    return err;
                env->insn_idx++;  /* ldimm64 是两条指令（占 16 字节） */
            }
        }

        env->insn_idx++;
    }

    return 0;
}


/* ======================================================================
 * ★ 核心 #5: check_mem_access() — 内存访问安全检查
 * ======================================================================
 *
 * 每当 BPF 程序读取或写入内存时，verifier 都调用此函数检查合法性。
 * 这是 verifier 防止越界访问的核心。
 *
 * 不同的基地址类型有不同的检查规则：
 * - PTR_TO_MAP_VALUE: 检查偏移是否在 [0, value_size) 范围内
 * - PTR_TO_CTX:       调用程序类型特定的 is_valid_access() 回调
 * - PTR_TO_STACK:     检查偏移是否在栈范围内（-512 ~ 0）
 * - PTR_TO_PACKET:    检查是否可能越界（数据包长度运行时才知道）
 */

static int check_mem_access(struct bpf_verifier_env *env, int insn_idx,
                            u32 regno,     /* 基地址寄存器编号 */
                            int off,       /* 偏移量 */
                            int bpf_size,  /* 访问大小（BPF_B/W/DW） */
                            enum bpf_access_type t,  /* READ 或 WRITE */
                            int value_regno,  /* 目标寄存器（READ 时） */
                            bool strict_alignment_once)
{
    struct bpf_reg_state *regs = cur_regs(env);
    struct bpf_reg_state *reg = regs + regno;
    int size, err = 0;

    size = bpf_size_to_bytes(bpf_size);
    if (size < 0)
        return size;

    /* ★ 检查指针对齐（某些架构要求严格对齐） */
    err = check_ptr_alignment(env, reg, off, size, strict_alignment_once);
    if (err)
        return err;

    off += reg->off;  /* 加上寄存器中记录的累积偏移 */

    if (reg->type == PTR_TO_MAP_VALUE) {
        /* ★ 访问 Map value */
        if (t == BPF_WRITE && value_regno >= 0 &&
            is_pointer_value(env, value_regno)) {
            /* 不允许将指针值写入 Map（防止指针泄露到用户态） */
            verbose(env, "R%d leaks addr into map\n", value_regno);
            return -EACCES;
        }
        /* 检查访问类型是否合法（如 spin_lock 区域的特殊规则） */
        err = check_map_access_type(env, regno, off, size, t);
        if (err)
            return err;
        /* ★ 检查偏移是否在 Map value 的范围内 */
        err = check_map_access(env, regno, off, size, false);
        if (!err && t == BPF_READ && value_regno >= 0) {
            struct bpf_map *map = reg->map_ptr;
            /* 如果 Map 是只读的，可以追踪读到的具体值 */
            if (tnum_is_const(reg->var_off) &&
                bpf_map_is_rdonly(map) &&
                map->ops->map_direct_value_addr) {
                /* 直接读取常量值（优化：verifier 知道具体值） */
                int map_off = off + reg->var_off.value;
                u64 val = 0;
                err = bpf_map_direct_read(map, map_off, size, &val);
                regs[value_regno].type = SCALAR_VALUE;
                __mark_reg_known(&regs[value_regno], val);
            } else {
                /* 标记目标寄存器为未知标量值 */
                mark_reg_unknown(env, regs, value_regno);
            }
        }

    } else if (reg->type == PTR_TO_CTX) {
        /* ★ 访问 bpf_context（程序上下文）
         * 不同程序类型有不同的 context 结构：
         * - KPROBE:  struct pt_regs
         * - TRACEPOINT: tracepoint 参数结构
         * - XDP:     struct xdp_md
         * - SCHED_CLS: struct __sk_buff
         */
        if (t == BPF_WRITE && value_regno >= 0 &&
            is_pointer_value(env, value_regno)) {
            verbose(env, "R%d leaks addr into ctx\n", value_regno);
            return -EACCES;
        }

        err = check_ctx_reg(env, reg, regno);
        if (err < 0)
            return err;

        /* ★ 调用程序类型特定的访问检查回调
         * ops->is_valid_access() 检查特定偏移和大小的访问是否合法 */
        err = check_ctx_access(env, insn_idx, off, size, t, &reg_type, &btf_id);

        if (!err && t == BPF_READ && value_regno >= 0) {
            /* 读取成功：更新目标寄存器的类型
             * 可能是 SCALAR_VALUE 或 PTR_TO_PACKET 等 */
            if (reg_type == SCALAR_VALUE) {
                mark_reg_unknown(env, regs, value_regno);
            } else {
                mark_reg_known_zero(env, regs, value_regno);
            }
        }
    }

    /* ... PTR_TO_STACK, PTR_TO_PACKET 等其他类型的检查 ... */

    return err;
}


/* ======================================================================
 * ★ 核心 #6: 常见 verifier 拒绝原因
 * ======================================================================
 *
 * 以下列出 BPF 程序被 verifier 拒绝的常见原因，
 * 以及对应的源码检查位置。
 *
 * +-----------------------------------------+----------------------------+
 * | 拒绝原因                                 | 检查函数                    |
 * +-----------------------------------------+----------------------------+
 * | 程序包含循环（back-edge）                 | check_cfg()                |
 * | 程序过大（> 4096 条指令）                 | check_cfg()                |
 * | 存在不可达指令                            | check_cfg()                |
 * | 读取未初始化的寄存器                       | check_reg_arg()            |
 * | 内存越界访问（Map/栈/ctx）                | check_mem_access()         |
 * | 指针泄露到用户态（将指针写入 Map）         | check_mem_access()         |
 * | 栈溢出（> 512 字节）                      | check_stack_access()       |
 * | 调用未授权的 helper 函数                  | check_helper_call()        |
 * | helper 参数类型不匹配                     | check_helper_call()        |
 * | 未检查 NULL 就使用指针                    | check_cond_jmp_op()        |
 * | 未释放引用（如 socket）就退出              | check_reference_leak()     |
 * | 持有 spin_lock 时调用函数或退出            | do_check() BPF_CALL/EXIT  |
 * | 除零错误（除以可能为 0 的值）              | check_alu_op()             |
 * | 非法指令（保留字段不为 0）                 | do_check() 各分支          |
 * +-----------------------------------------+----------------------------+
 *
 * ★ 调试技巧：
 * 加载程序时设置 log_level=1 或 2，verifier 会在 log_buf 中输出：
 * - 每条指令的寄存器状态
 * - 被拒绝的具体原因和指令位置
 * - 完整的分析路径
 *
 * 示例 verifier 错误输出：
 *   0: (bf) r6 = r1
 *   1: (79) r7 = *(u64 *)(r6 +16)
 *   2: (b7) r1 = 0
 *   3: (63) *(u32 *)(r10 -4) = r1
 *   4: (bf) r2 = r10
 *   5: (07) r2 += -4
 *   6: (18) r1 = 0xffff888012345678  ; map_ptr
 *   8: (85) call bpf_map_lookup_elem#1
 *   9: (15) if r0 == 0x0 goto pc+2
 *   10: (72) *(u8 *)(r0 +0) = r1     ; ← 如果忘记检查 NULL：
 *   R0 invalid pointer access        ;    verifier 会在这条拒绝
 */


/* ======================================================================
 * ★ 核心 #7: 辅助函数追踪 — 资源泄漏检测
 * ======================================================================
 *
 * verifier 追踪 BPF 程序中的资源获取和释放。
 * 某些 helper 函数会分配资源（如 socket 引用），
 * 对应的释放函数必须在使用完毕后调用。
 *
 * 来自 verifier.c:
 */

/* 判断是否为资源释放函数 */
static bool is_release_function(enum bpf_func_id func_id)
{
    return func_id == BPF_FUNC_sk_release ||      /* 释放 socket 引用 */
           func_id == BPF_FUNC_ringbuf_submit ||   /* 提交 ringbuf 数据 */
           func_id == BPF_FUNC_ringbuf_discard;    /* 丢弃 ringbuf 数据 */
}

/* 判断是否为资源获取函数 */
static bool is_acquire_function(enum bpf_func_id func_id,
                                const struct bpf_map *map)
{
    enum bpf_map_type map_type = map ? map->map_type : BPF_MAP_TYPE_UNSPEC;

    if (func_id == BPF_FUNC_sk_lookup_tcp ||     /* 获取 TCP socket 引用 */
        func_id == BPF_FUNC_sk_lookup_udp ||     /* 获取 UDP socket 引用 */
        func_id == BPF_FUNC_skc_lookup_tcp ||    /* 获取 socket common 引用 */
        func_id == BPF_FUNC_ringbuf_reserve)     /* 预留 ringbuf 空间 */
        return true;

    /* SOCKMAP/SOCKHASH 的 map_lookup_elem 也返回需要释放的引用 */
    if (func_id == BPF_FUNC_map_lookup_elem &&
        (map_type == BPF_MAP_TYPE_SOCKMAP ||
         map_type == BPF_MAP_TYPE_SOCKHASH))
        return true;

    return false;
}

/*
 * ★ 引用追踪流程：
 *
 * 1. 调用获取函数（如 bpf_sk_lookup_tcp）
 *    → acquire_reference_state() 分配一个 ptr_id
 *    → 返回的寄存器类型为 PTR_TO_SOCKET_OR_NULL
 *
 * 2. NULL 检查
 *    → 通过条件跳转后，true 分支: PTR_TO_SOCKET（已确认非 NULL）
 *    → false 分支: CONST_IMM（值为 0，引用被释放）
 *
 * 3. 使用引用
 *    → 可以通过 PTR_TO_SOCKET 类型的寄存器访问 socket 字段
 *
 * 4. 释放引用（调用 bpf_sk_release）
 *    → release_reference_state() 删除 ptr_id
 *
 * 5. 程序退出时
 *    → check_reference_leak() 检查是否还有未释放的引用
 *    → 如果有 → 程序被拒绝！
 *
 * 示例（会被拒绝的程序）：
 *   sk = bpf_sk_lookup_tcp(ctx, ...);
 *   if (sk) {
 *       // 使用 sk...
 *       // 忘记调用 bpf_sk_release(sk)！
 *   }
 *   return 0;
 *   → verifier 报错：Unreleased reference
 */


/*
 * ★ 总结：BPF 验证器的完整验证流程
 *
 *  bpf(BPF_PROG_LOAD, &attr)
 *      │
 *      ▼
 *  bpf_check()
 *      │
 *      ├── check_subprogs()         ← 识别子程序
 *      ├── check_cfg()              ← 控制流图检查（无循环、无死代码）
 *      ├── resolve_pseudo_ldimm64() ← 解析 map fd → map 指针
 *      │
 *      ├── do_check()               ← ★★ 核心：逐条指令分析
 *      │   │
 *      │   ├── for 每条指令:
 *      │   │   ├── ALU:  check_alu_op()        ← 算术运算检查
 *      │   │   ├── LDX:  check_mem_access()    ← 内存读取检查
 *      │   │   │           ├── PTR_TO_MAP_VALUE → 范围检查
 *      │   │   │           ├── PTR_TO_CTX       → 类型特定检查
 *      │   │   │           └── PTR_TO_STACK     → 栈边界检查
 *      │   │   ├── STX:  check_mem_access()    ← 内存写入检查
 *      │   │   ├── CALL: check_helper_call()   ← helper 参数检查
 *      │   │   ├── EXIT: check_reference_leak() ← 引用泄漏检查
 *      │   │   └── JMP:  check_cond_jmp_op()   ← 分支状态分裂
 *      │   │
 *      │   └── is_state_visited()   ← 状态剪枝优化
 *      │
 *      ├── check_max_stack_depth()  ← 栈深度检查（≤ 512 字节）
 *      ├── convert_ctx_accesses()   ← 上下文访问指令转换
 *      ├── fixup_bpf_calls()        ← helper 调用地址修补
 *      └── opt_subreg_zext_lo32()   ← 32 位子寄存器优化
 *
 *  全部通过 → JIT 编译 → 可以附着执行
 *  任一失败 → 返回错误码 + 日志（log_buf 中的拒绝原因）
 */
