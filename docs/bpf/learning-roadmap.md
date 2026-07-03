# BPF/eBPF 学习路线 — bcc + bpftrace + libbpf

> 从 BPF 虚拟机到 JIT 编译，从 verifier 到 map，从 one-liner 到 CO-RE

---

## 目录

- [1. 整体架构](#1-整体架构)
- [2. 三大工具对比](#2-三大工具对比)
- [3. 源码阅读路线图](#3-源码阅读路线图)
  - [Step 1: BPF 虚拟机与指令集](#step-1-bpf-虚拟机与指令集)
  - [Step 2: Verifier — 安全校验器](#step-2-verifier--安全校验器)
  - [Step 3: BPF Map — 内核与用户态的数据桥梁](#step-3-bpf-map--内核与用户态的数据桥梁)
  - [Step 4: Helper Functions — BPF 程序的系统调用](#step-4-helper-functions--bpf-程序的系统调用)
  - [Step 5: bpftrace 源码 — 从 DSL 到 BPF 字节码](#step-5-bpftrace-源码--从-dsl-到-bpf-字节码)
  - [Step 6: libbpf 源码 — BPF 程序的加载与附着](#step-6-libbpf-源码--bpf-程序的加载与附着)
- [4. BPF 程序类型全览](#4-bpf-程序类型全览)
- [5. BPF Map 类型全览](#5-bpf-map-类型全览)
- [6. 动手实验清单](#6-动手实验清单)
- [7. 关键源码文件索引](#7-关键源码文件索引)
- [8. 与 blktrace 的对照学习](#8-与-blktrace-的对照学习)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        用户态工具层                              │
│                                                                 │
│  bpftrace          bcc (Python/Lua)        libbpf (C/C++)       │
│  ┌──────────┐     ┌──────────────────┐    ┌──────────────┐     │
│  │ DSL 脚本 │     │ Python + 内嵌 C  │    │ ELF + skeleton│     │
│  │ one-liner│     │ BPF 程序文本     │    │ CO-RE        │     │
│  └────┬─────┘     └────────┬─────────┘    └──────┬───────┘     │
│       │ 解析/编译           │ LLVM 编译           │ ELF 加载    │
├───────┼────────────────────┼─────────────────────┼─────────────┤
│       ▼                    ▼                     ▼              │
│  ┌─────────────────────────────────────────────────────┐       │
│  │              bpf() 系统调用 (syscall 321)             │       │
│  │  BPF_PROG_LOAD / BPF_MAP_CREATE / BPF_MAP_*_ELEM    │       │
│  │  BPF_PROG_ATTACH / BPF_LINK_CREATE                  │       │
│  └─────────────────────────┬───────────────────────────┘       │
├────────────────────────────┼───────────────────────────────────-┤
│                            ▼                                    │
│  ┌─────────────────────────────────────────────────────┐       │
│  │              内核 BPF 子系统                         │       │
│  │                                                     │       │
│  │  ┌─────────────┐  ┌──────────┐  ┌───────────────┐  │       │
│  │  │  Verifier   │  │   JIT    │  │   BPF Maps    │  │       │
│  │  │  安全检查    │  │ 编译机器码│  │  数据存储     │  │       │
│  │  │  • 无环检测  │  │ x86/ARM │  │  hash/array   │  │       │
│  │  │  • 边界检查  │  │ 原生代码 │  │  ringbuf      │  │       │
│  │  │  • 指针安全  │  │ ★零开销  │  │  percpu       │  │       │
│  │  └─────────────┘  └──────────┘  └───────────────┘  │       │
│  │                                                     │       │
│  │  ┌─────────────────────────────────────────────┐   │       │
│  │  │          BPF Helper Functions                │   │       │
│  │  │  bpf_probe_read() bpf_get_current_comm()    │   │       │
│  │  │  bpf_map_lookup_elem() bpf_trace_printk()   │   │       │
│  │  │  bpf_perf_event_output() ...                │   │       │
│  │  └─────────────────────────────────────────────┘   │       │
├──┼─────────────────────────────────────────────────────┼───────┤
│  ▼                     内核 Hook 点                     ▼       │
│  kprobe/kretprobe    tracepoint      uprobe/uretprobe          │
│  fentry/fexit        perf_event      cgroup                    │
│  XDP (网卡驱动层)    TC (流量控制)    LSM (安全模块)            │
└─────────────────────────────────────────────────────────────────┘
```

### BPF 程序生命周期

```
用户编写 BPF 代码（C / bpftrace DSL / Python 内嵌 C）
    │
    ▼
LLVM 编译为 BPF 字节码（ELF .o 文件）
    │
    ▼
libbpf / bcc 加载 ELF → bpf(BPF_PROG_LOAD) 系统调用
    │
    ▼
内核 Verifier 校验字节码
    ├── 检查指令数量（≤ 100 万条）
    ├── 检查无环（DAG 验证）
    ├── 检查内存访问边界
    ├── 检查指针类型安全
    └── 检查除零、溢出等
    │
    ▼ （校验通过）
JIT 编译器将 BPF 字节码转为本地机器码（x86_64 / ARM64）
    │
    ▼
附着到 hook 点（kprobe / tracepoint / uprobe / ...）
    │
    ▼
事件触发 → 执行 BPF 程序 → 结果写入 Map / perf buffer / ringbuf
    │
    ▼
用户态程序从 Map / buffer 中读取结果
```

---

## 2. 三大工具对比

| 维度 | bpftrace | bcc | libbpf |
|------|----------|-----|--------|
| **定位** | 高级追踪 DSL（类似 awk） | Python 框架 + 内嵌 C | 底层 C 库 |
| **上手难度** | ★ 最简单（one-liner） | ★★ 中等 | ★★★ 较难 |
| **编程语言** | bpftrace 脚本语言 | Python + C（BPF 端） | C/C++（纯 BPF C） |
| **编译方式** | 内置 LLVM JIT | 运行时 LLVM 编译 | 预编译 ELF + CO-RE |
| **依赖** | 仅 bpftrace 二进制 | Python + LLVM + clang | 仅 libbpf + libelf |
| **CO-RE 支持** | 有限 | 不支持 | ★ 完整支持 |
| **BTF 依赖** | 可选（自动检测） | 不依赖 | 需要 BTF（CO-RE） |
| **适用场景** | 快速诊断、one-liner | 复杂追踪工具 | 生产级 BPF 应用 |
| **性能开销** | 中等 | 中等（运行时编译） | ★ 最低 |
| **源码位置** | `src/bpftrace/` | `src/bcc/` | `src/libbpf/` |

### 选择建议

```
想快速查看系统行为？
    │
    ├── 是 → bpftrace（一行命令搞定）
    │        bpftrace -e 'kprobe:do_sys_open { printf("%s\n", comm); }'
    │
    └── 否 → 需要复杂的用户态逻辑？
              │
              ├── 是 → bcc（Python 处理数据 + C 采集数据）
              │        # opensnoop.py, biolatency.py 等
              │
              └── 否 → 需要生产级性能 / 嵌入式部署？
                        │
                        └── 是 → libbpf（预编译 + CO-RE）
                                 # 编译一次，到处运行
```

---

## 3. 源码阅读路线图

### Step 1: BPF 虚拟机与指令集

**目标：** 理解 eBPF 程序的执行模型和指令格式

**文件位置：**
- 内核定义：`/usr/include/linux/bpf.h`（`struct bpf_insn`）
- libbpf 头文件：`src/libbpf/src/bpf.h`
- 内核 JIT 实现：`src/linux-5.10/arch/x86/net/bpf_jit_comp.c`（参考）

**阅读要点：**

```c
/* BPF 指令格式（每条 8 字节） */
struct bpf_insn {
    __u8 code;        /* 操作码（如 BPF_ALU64 | BPF_ADD | BPF_X） */
    __u8 dst_reg:4;   /* 目标寄存器（r0-r10） */
    __u8 src_reg:4;   /* 源寄存器 */
    __s16 off;        /* 偏移量（跳转或内存访问） */
    __s32 imm;        /* 立即数 */
};
```

**BPF 寄存器模型：**

```
┌─────────────────────────────────────────┐
│          BPF 虚拟机寄存器                │
├─────────────────────────────────────────┤
│ r0        │ 函数返回值 / 程序退出码      │
│ r1-r5     │ 函数参数（r1=ctx 上下文）    │
│ r6-r9     │ 被调用者保存（callee-saved） │
│ r10       │ ★ 帧指针（只读，指向栈顶）   │
├─────────────────────────────────────────┤
│ 栈空间    │ 512 字节（固定大小）         │
└─────────────────────────────────────────┘

调用约定：
  BPF 程序入口：r1 = 上下文指针（如 struct pt_regs *）
  BPF 程序退出：r0 = 返回值（0 = 继续执行，1 = 终止）
```

**BPF 指令类别：**

| 类别 | 操作码前缀 | 示例 | 说明 |
|------|-----------|------|------|
| ALU64 | `BPF_ALU64` | `r1 += r2` | 64 位算术 |
| ALU | `BPF_ALU` | `w1 += w2` | 32 位算术 |
| LDX | `BPF_LDX` | `r1 = *(u64 *)(r2 + 8)` | 从内存加载 |
| STX | `BPF_STX` | `*(u64 *)(r1 + 8) = r2` | 存储到内存 |
| JMP | `BPF_JMP` | `if r1 > r2 goto +3` | 条件跳转 |
| CALL | `BPF_CALL` | `call bpf_map_lookup_elem` | 调用 helper |
| EXIT | `BPF_EXIT` | `exit` | 程序退出 |

**思考题：**
1. 为什么 BPF 栈只有 512 字节？（答：限制程序复杂度，确保 verifier 能在有限时间内完成分析）
2. 为什么 r10 是只读的？（答：防止程序破坏栈帧，保证安全退出）
3. BPF_CALL 最多支持几个参数？（答：5 个，r1-r5，与 Linux x86_64 调用约定一致）

---

### Step 2: Verifier — 安全校验器

**目标：** 理解 BPF 程序加载前必须通过的安全检查机制

**文件位置：**
- 内核 verifier：`src/linux-5.10/kernel/bpf/verifier.c`（参考）
- libbpf 加载入口：`src/libbpf/src/libbpf.c`（`bpf_object__load()`）

**Verifier 核心检查项：**

```
┌──────────────────────────────────────────────────────┐
│                  Verifier 检查流程                    │
├──────────────────────────────────────────────────────┤
│                                                      │
│  1. 静态分析                                         │
│     ├── 指令数量 ≤ 1,000,000                        │
│     ├── 无环检测（DAG：所有路径必须可达 exit）       │
│     └── 寄存器使用分析（未初始化不可读）             │
│                                                      │
│  2. 类型推断（逐指令模拟执行）                       │
│     ├── 指针类型追踪（PTR_TO_CTX, PTR_TO_MAP_VALUE） │
│     ├── 整数范围追踪（已知范围用于边界检查）         │
│     └── ★ 内存访问边界验证（不能越界）              │
│                                                      │
│  3. 安全检查                                         │
│     ├── 不能直接解引用用户指针（必须用 helper）      │
│     ├── 不能访问内核任意内存（必须用 bpf_probe_read）│
│     ├── Map 访问必须检查 key 边界                    │
│     └── 不能做不可控的循环（bounded loops 除外）    │
│                                                      │
│  4. 权限检查                                         │
│     ├── CAP_BPF / CAP_SYS_ADMIN                     │
│     ├── 非特权程序只能用受限 helper 集               │
│     └── unprivileged_bpf_disabled sysctl             │
│                                                      │
└──────────────────────────────────────────────────────┘
```

**Verifier 日志 示例：**

```
; int pid = bpf_get_current_pid_tgid() >> 32;
0: (85) call bpf_get_current_pid_tgid#14    ← 调用 helper
1: (77) r0 >>= 32                           ← 右移取 PID
2: (63) *(u32 *)(r10 -4) = r0               ← 存到栈上
; if (pid == 1234) {
3: (61) r1 = *(u32 *)(r10 -4)               ← 从栈上读回
4: (55) if r1 != 0x4d2 goto pc+3            ← 条件跳转
```

**思考题：**
1. Verifier 拒绝程序时，如何查看拒绝原因？（答：`bpftool prog load` 或 `bpf_program__set_log_buf()` 获取 verifier log）
2. 什么是 bounded loop？（答：verifier 能证明循环次数有限的循环，内核 5.3+ 支持）
3. 为什么需要 BTF（BPF Type Format）？（答：让 verifier 理解内核结构体的布局，实现 CO-RE）

---

### Step 3: BPF Map — 内核与用户态的数据桥梁

**目标：** 理解 BPF 程序如何存储和传递数据

**文件位置：**
- 内核定义：`/usr/include/linux/bpf.h`（`enum bpf_map_type`）
- libbpf 操作：`src/libbpf/src/bpf.h`（`bpf_map_create()`、`bpf_map__*()`）
- 用户态读写：`src/libbpf/src/libbpf.c`

**Map 基本操作（bpf() 系统调用）：**

```
用户态                          内核（BPF 程序）
───────                         ──────────────
bpf(BPF_MAP_CREATE, ...)        → 创建 Map，返回 fd
bpf(BPF_MAP_UPDATE_ELEM, ...)   → 写入键值对
bpf(BPF_MAP_LOOKUP_ELEM, ...)   → 读取键值对
bpf(BPF_MAP_DELETE_ELEM, ...)   → 删除键值对
bpf(BPF_MAP_GET_NEXT_KEY, ...)  → 遍历 Map

                                  bpf_map_lookup_elem(&map, &key)
                                  bpf_map_update_elem(&map, &key, &val, flags)
                                  bpf_map_delete_elem(&map, &key)
```

**常用 Map 类型：**

| 类型 | 数据结构 | 用途 | 典型场景 |
|------|---------|------|---------|
| `BPF_MAP_TYPE_HASH` | 哈希表 | 通用键值存储 | 统计进程系统调用次数 |
| `BPF_MAP_TYPE_ARRAY` | 数组 | 固定索引查找 | 直方图分桶 |
| `BPF_MAP_TYPE_PERCPU_HASH` | per-CPU 哈希 | 无锁聚合 | 高性能计数 |
| `BPF_MAP_TYPE_PERF_EVENT_ARRAY` | per-CPU perf buffer | 事件流传输 | 实时事件输出 |
| `BPF_MAP_TYPE_RINGBUF` | 环形缓冲区 | 高效事件流 | ★ 替代 perf buffer |
| `BPF_MAP_TYPE_STACK_TRACE` | 栈追踪 | 调用栈存储 | profiling |
| `BPF_MAP_TYPE_PROG_ARRAY` | 程序数组 | tail call | 程序链式调用 |
| `BPF_MAP_TYPE_LRU_HASH` | LRU 哈希 | 有限大小缓存 | 连接追踪 |

**ASCII 图解 perf buffer vs ringbuf：**

```
perf_event_array（per-CPU）：
┌── CPU 0 ──┐  ┌── CPU 1 ──┐  ┌── CPU 2 ──┐
│ [event][ev │  │ [event][ev │  │ [event]    │
│  ent][even │  │  ent]      │  │            │
└───────────┘  └───────────┘  └───────────┘
每个 CPU 独立 buffer → 用户态逐个读取 → 可能丢事件

ringbuf（全局共享）：
┌──────────────────────────────────────────┐
│  [event1][event2][event3][event4]...      │
│              ↑ consumer    ↑ producer     │
└──────────────────────────────────────────┘
全局环形缓冲 → mmap 共享 → 几乎零丢失
★ 内核 5.8+ 支持，推荐优先使用
```

---

### Step 4: Helper Functions — BPF 程序的系统调用

**目标：** 理解 BPF 程序可以调用哪些内核功能

**文件位置：**
- helper 定义：`src/libbpf/src/bpf_helper_defs.h`（自动生成的 helper 签名）
- 内核实现：`src/linux-5.10/kernel/bpf/helpers.c`（参考）
- 追踪专用：`src/linux-5.10/kernel/trace/bpf_trace.c`（参考）

**常用 Helper 分类：**

```
┌─── 基础信息 ───────────────────────────────────────┐
│ bpf_get_current_pid_tgid()  → 当前进程 PID+TGID    │
│ bpf_get_current_uid_gid()   → 当前用户 UID+GID     │
│ bpf_get_current_comm(buf)   → 当前进程名           │
│ bpf_ktime_get_ns()          → 纳秒时间戳           │
└────────────────────────────────────────────────────┘

┌─── 内存访问 ───────────────────────────────────────┐
│ bpf_probe_read_kernel(dst, size, src)  → 读内核内存│
│ bpf_probe_read_user(dst, size, src)    → 读用户内存│
│ bpf_probe_read_str(dst, size, src)     → 读字符串  │
└────────────────────────────────────────────────────┘

┌─── Map 操作 ───────────────────────────────────────┐
│ bpf_map_lookup_elem(map, key)       → 查找元素     │
│ bpf_map_update_elem(map, key, val)  → 更新元素     │
│ bpf_map_delete_elem(map, key)       → 删除元素     │
└────────────────────────────────────────────────────┘

┌─── 输出与事件 ─────────────────────────────────────┐
│ bpf_trace_printk(fmt, ...)          → debugfs 输出 │
│ bpf_perf_event_output(ctx, map, ..) → perf buffer  │
│ bpf_ringbuf_output(map, data, ...)  → ringbuf      │
│ bpf_ringbuf_reserve(map, size, ..)  → 预留空间     │
│ bpf_ringbuf_submit(data, flags)     → 提交数据     │
└────────────────────────────────────────────────────┘
```

---

### Step 5: bpftrace 源码 — 从 DSL 到 BPF 字节码

**目标：** 理解 bpftrace 如何将一行脚本编译为 BPF 程序

**文件位置：**
- 主入口：`src/bpftrace/src/main.cpp`
- 解析器：`src/bpftrace/src/parser/`
- AST 定义：`src/bpftrace/src/ast/ast.h`
- 代码生成：`src/bpftrace/src/ast/passes/`（codegen）
- 探针附着：`src/bpftrace/src/attached_probe.cpp`

**bpftrace 编译流程：**

```
bpftrace -e 'kprobe:do_sys_open { printf("%s\n", comm); }'
    │
    ▼
1. 词法分析（Lexer）→ Token 流
    │  kprobe:do_sys_open → PROBE_TYPE(kprobe), IDENT(do_sys_open)
    │  { printf(...) }    → LBRACE, IDENT(printf), ...
    │
    ▼
2. 语法分析（Parser）→ AST
    │  Program
    │  └── ProbeAttachPoint(kprobe, do_sys_open)
    │      └── Block
    │          └── Call(printf, [Builtin(comm)])
    │
    ▼
3. 语义分析（Semantic Analyzer）
    │  ├── 检查 probe 类型是否有效
    │  ├── 检查变量类型匹配
    │  ├── 检查 helper 可用性
    │  └── 推断 map 类型（count/avg/hist/...）
    │
    ▼
4. 资源分配（Resource Analyser）
    │  ├── 分配 Map（@, @stats, ...）
    │  ├── 分配栈空间
    │  └── 计算 BPF 指令数
    │
    ▼
5. 代码生成（Codegen）→ LLVM IR → BPF 字节码
    │  ├── 生成 LLVM IR
    │  ├── LLVM 优化
    │  └── LLVM 后端生成 BPF ELF
    │
    ▼
6. 加载与附着
    │  ├── bpf(PROG_LOAD) → 加载到内核
    │  ├── perf_event_open() → 创建 perf event
    │  └── bpf_link_create() → 附着到 kprobe
    │
    ▼
7. 事件循环
    └── poll(perf buffer) → 打印输出
```

**bpftrace 探针类型（源码 `probe_types.h`）：**

| ProbeType | 别名 | 内核程序类型 | 说明 |
|-----------|------|-------------|------|
| `kprobe` | `k` | `BPF_PROG_TYPE_KPROBE` | 内核函数入口 |
| `kretprobe` | `kr` | `BPF_PROG_TYPE_KPROBE` | 内核函数返回 |
| `tracepoint` | `t` | `BPF_PROG_TYPE_TRACEPOINT` | 内核静态追踪点 |
| `uprobe` | `u` | `BPF_PROG_TYPE_KPROBE` | 用户态函数入口 |
| `uretprobe` | `ur` | `BPF_PROG_TYPE_KPROBE` | 用户态函数返回 |
| `usdt` | `U` | `BPF_PROG_TYPE_KPROBE` | 用户态 DTrace 探针 |
| `profile` | `p` | `BPF_PROG_TYPE_PERF_EVENT` | 定时采样 |
| `interval` | `i` | `BPF_PROG_TYPE_PERF_EVENT` | 定时间隔 |
| `software` | `s` | `BPF_PROG_TYPE_PERF_EVENT` | 软件事件 |
| `hardware` | `h` | `BPF_PROG_TYPE_PERF_EVENT` | 硬件事件（PMC） |
| `fentry` | — | `BPF_PROG_TYPE_TRACING` | 函数入口（BTF） |
| `fexit` | — | `BPF_PROG_TYPE_TRACING` | 函数退出（BTF） |
| `rawtracepoint` | `rt` | `BPF_PROG_TYPE_RAW_TRACEPOINT` | 原始 tracepoint |
| `watchpoint` | `w` | `BPF_PROG_TYPE_PERF_EVENT` | 内存监视 |

**实操任务：**
1. 打开 `src/bpftrace/src/ast/ast.h`，找到 `Program`、`AttachPoint`、`Probe` 等节点定义
2. 打开 `src/bpftrace/src/attached_probe.cpp`，看 `make()` 如何根据 ProbeType 选择 attach 方式
3. 在 `src/bpftrace/src/probe_types.h` 中查看完整的 PROBE_LIST

---

### Step 6: libbpf 源码 — BPF 程序的加载与附着

**目标：** 理解 BPF ELF 文件如何被加载到内核

**文件位置：**
- 核心加载：`src/libbpf/src/libbpf.c`（`bpf_object__load()`、`bpf_object__open()`）
- 低层 syscall：`src/libbpf/src/bpf.c`（`bpf_prog_load()`、`bpf_map_create()`）
- BTF 处理：`src/libbpf/src/btf.c`
- CO-RE 重定位：`src/libbpf/src/libbpf.c`（`bpf_core_relo()`）

**libbpf 加载流程：**

```
bpf_object__open_file("prog.bpf.o")
    │
    ├── 1. ELF 解析
    │      ├── 解析 ELF sections（.text, maps, license, ...）
    │      ├── 解析符号表和重定位表
    │      └── 识别 BPF 程序（按 section 名判断类型）
    │
    ├── 2. BTF 解析
    │      ├── 解析 .BTF section（类型信息）
    │      └── 解析 .BTF.ext section（func_info, line_info）
    │
    └── 3. CO-RE 重定位（★ Compile Once, Run Everywhere）
           ├── 读取 .BTF.ext 中的 CO-RE relocations
           ├── 对每个 relocation，在目标内核 BTF 中查找匹配类型
           └── 修改 BPF 指令中的偏移量/大小
               （让同一个 .o 文件在不同内核版本上运行）

bpf_object__load(obj)
    │
    ├── 4. 创建 Map
    │      └── 对每个 map 调用 bpf(BPF_MAP_CREATE, ...)
    │
    ├── 5. 加载程序
    │      ├── 应用重定位（Map fd、全局变量）
    │      └── bpf(BPF_PROG_LOAD, prog, verifier_log)
    │          → 内核 verifier 校验 → 返回 prog_fd
    │
    └── 6. 附着
           ├── bpf_program__attach_kprobe(prog, "do_sys_open")
           ├── bpf_program__attach_tracepoint(prog, "syscalls", "sys_enter_read")
           └── bpf_link_create() 或 perf_event_open() + ioctl
```

**CO-RE 原理示意：**

```
编译时（开发机，内核 5.15）：
  struct task_struct {
      ...
      int pid;           // offset = 2240
      ...
  }
  BPF 代码：pid = task->pid  → 指令：*(r1 + 2240)

运行时（目标机，内核 6.6）：
  struct task_struct {
      ...
      int pid;           // offset = 2368（变了！）
      ...
  }
  CO-RE 重定位：自动将 2240 修改为 2368
  BPF 指令：*(r1 + 2368)

  ★ 不需要在目标机上重新编译！
  ★ 前提是目标内核有 BTF 信息（CONFIG_DEBUG_INFO_BTF=y）
```

**思考题：**
1. 为什么 CO-RE 需要 BTF？（答：BTF 记录了结构体的完整布局，libbpf 用它来计算字段偏移）
2. libbpf 如何识别 section 名？（答：约定俗成，如 `kprobe/do_sys_open`、`tracepoint/syscalls/sys_enter_read`）
3. 什么是 BPF skeleton？（答：libbpf 自动生成的 C 头文件，封装了 open/load/attach 操作）

---

## 4. BPF 程序类型全览

内核定义的所有 `bpf_prog_type`（`/usr/include/linux/bpf.h`）：

| 类型 | 枚举值 | 用途 | 上下文 |
|------|--------|------|--------|
| `BPF_PROG_TYPE_KPROBE` | 2 | kprobe/kretprobe/uprobe | `struct pt_regs *` |
| `BPF_PROG_TYPE_TRACEPOINT` | 5 | tracepoint | tracepoint 结构体指针 |
| `BPF_PROG_TYPE_XDP` | 6 | XDP（网卡驱动层包处理） | `struct xdp_md *` |
| `BPF_PROG_TYPE_PERF_EVENT` | 7 | perf 事件/profile | `struct bpf_perf_event_data *` |
| `BPF_PROG_TYPE_CGROUP_SKB` | 9 | cgroup 网络过滤 | `struct __sk_buff *` |
| `BPF_PROG_TYPE_CGROUP_SOCK` | 10 | cgroup socket 过滤 | `struct bpf_sock *` |
| `BPF_PROG_TYPE_SOCK_OPS` | 14 | socket 操作 hook | `struct bpf_sock_ops *` |
| `BPF_PROG_TYPE_RAW_TRACEPOINT` | 17 | 原始 tracepoint | `struct bpf_raw_tracepoint_args *` |
| `BPF_PROG_TYPE_TRACING` | 26 | fentry/fexit/iter | BTF 函数参数 |
| `BPF_PROG_TYPE_STRUCT_OPS` | 27 | 替换内核结构体操作 | 自定义结构体 |
| `BPF_PROG_TYPE_LSM` | 29 | 安全模块 hook | 各 LSM hook 参数 |
| `BPF_PROG_TYPE_SYSCALL` | 32 | BPF 系统调用代理 | `struct bpf_sysctl *` |

**追踪相关（★ 本学习重点）：**
- `KPROBE` — kprobe、kretprobe、uprobe、uretprobe 都用此类型
- `TRACEPOINT` — 内核静态 tracepoint
- `RAW_TRACEPOINT` — 原始 tracepoint（无参数预处理，更快）
- `TRACING` — fentry/fexit（基于 BTF 的新一代追踪，性能更好）
- `PERF_EVENT` — profile、interval、watchpoint 等定时采样

---

## 5. BPF Map 类型全览

| 类型 | 键/值 | 特点 | 典型用途 |
|------|-------|------|---------|
| `HASH` | 任意/任意 | 通用哈希表 | 进程统计、连接追踪 |
| `ARRAY` | u32/任意 | 固定大小数组 | 直方图、配置 |
| `PROG_ARRAY` | u32/fd | tail call 跳转表 | 程序链式调用 |
| `PERF_EVENT_ARRAY` | u32/fd | per-CPU perf buffer | 实时事件流 |
| `PERCPU_HASH` | 任意/任意 | per-CPU 无锁哈希 | 高性能聚合计数 |
| `PERCPU_ARRAY` | u32/任意 | per-CPU 无锁数组 | per-CPU 直方图 |
| `STACK_TRACE` | u64/stack | 存储调用栈 | profiling |
| `LRU_HASH` | 任意/任意 | LRU 淘汰策略 | 有限大小连接表 |
| `LPM_TRIE` | 前缀/任意 | 最长前缀匹配 | IP 路由表 |
| `ARRAY_OF_MAPS` | u32/map_fd | Map 嵌套 | 动态子 Map |
| `HASH_OF_MAPS` | 任意/map_fd | Map 嵌套 | 动态子 Map |
| `RINGBUF` | —/任意 | ★ 全局环形缓冲 | 高效事件流（推荐） |
| `QUEUE` | —/任意 | FIFO 队列 | 任务调度 |
| `STACK` | —/任意 | LIFO 栈 | 工作队列 |
| `BLOOM_FILTER` | —/任意 | 布隆过滤器 | 快速存在性检查 |

---

## 6. 动手实验清单

### 实验 1：bpftrace 基础 one-liners

```bash
# 1. 追踪文件打开
bpftrace -e 'kprobe:do_sys_openat2 { printf("%s -> %s\n", comm, str(arg1)); }'

# 2. 统计系统调用次数
bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm] = count(); }'

# 3. 追踪进程创建
bpftrace -e 'tracepoint:syscalls:sys_enter_execve { printf("%s -> %s\n", comm, str(args->filename)); }'

# 4. 统计 block I/O 事件（与 blktrace 对比）
bpftrace -e 'tracepoint:block:block_rq_issue { @[comm] = count(); }'

# 5. 直方图：系统调用延迟分布
bpftrace -e '
  tracepoint:raw_syscalls:sys_enter { @start[tid] = nsecs; }
  tracepoint:raw_syscalls:sys_exit /@start[tid]/ {
    @us = hist((nsecs - @start[tid]) / 1000);
    delete(@start[tid]);
  }'
```

### 实验 2：bpftool 查看已加载的 BPF 程序

```bash
# 查看所有已加载的 BPF 程序
bpftool prog show

# 查看所有 BPF map
bpftool map show

# 查看某个 map 的内容
bpftool map dump id <MAP_ID>

# 查看 BTF 信息
bpftool btf list

# 导出内核 BTF
bpftool btf dump file /sys/kernel/btf/vmlinux format c | head -50
```

### 实验 3：bpftrace 与 blktrace 对比

```bash
# blktrace 方式：追踪 block I/O
blktrace -d /dev/vdb -o trace -w 5 &
sleep 1
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=3 --time_based
wait
blkparse -i trace | head -10

# bpftrace 方式：追踪相同的 block 事件
bpftrace -e '
  tracepoint:block:block_rq_issue {
    printf("%s %d %s sector=%d bytes=%d\n",
           comm, pid, args->rwbs, args->sector, args->bytes);
  }' &
sleep 1
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=3 --time_based
```

### 实验 4：验证 BPF verifier

```bash
# 加载一个 BPF 程序并查看 verifier log
bpftool prog load /path/to/prog.bpf.o /sys/fs/bpf/test

# 查看 verifier 的完整检查日志
bpftool prog dump xlated pinned /sys/fs/bpf/test

# 查看 JIT 编译后的机器码
bpftool prog dump jited pinned /sys/fs/bpf/test
```

### 实验 5：BPF ringbuf vs perf buffer

```bash
# perf buffer 方式
bpftrace -e '
  kprobe:vfs_read {
    @reads[comm] = count();
  }'

# ringbuf 方式（bpftrace 默认在支持的内核上使用）
bpftrace -e '
  kprobe:vfs_read {
    printf("%s read\n", comm);
  }'

# 对比两者的事件丢失率（高负载下）
```

---

## 7. 关键源码文件索引

### 内核侧（/usr/include/linux/）

| 文件 | 关注点 |
|------|--------|
| `bpf.h` | BPF 指令格式、程序类型、Map 类型、Helper 定义 |
| `bpf_common.h` | BPF 指令类别常量 |
| `btf.h` | BTF 类型格式定义 |

### libbpf（src/libbpf/）

| 文件 | 关注点 |
|------|--------|
| `src/libbpf.c` | ★ 核心：ELF 解析、CO-RE 重定位、程序加载 |
| `src/libbpf.h` | 公开 API：`bpf_object__open/load/attach` |
| `src/bpf.c` | 底层 bpf() 系统调用封装 |
| `src/bpf.h` | `bpf_prog_load()`、`bpf_map_create()` 等 |
| `src/bpf_helper_defs.h` | 自动生成的 helper 函数签名 |
| `src/btf.c` | BTF 解析和 CO-RE 重定位实现 |
| `src/elf.c` | ELF 文件解析 |
| `src/libbpf_internal.h` | 内部结构和宏定义 |

### bpftrace（src/bpftrace/）

| 文件 | 关注点 |
|------|--------|
| `src/main.cpp` | 主入口：参数解析、脚本加载 |
| `src/ast/ast.h` | AST 节点定义（Program、Probe、Call） |
| `src/ast/passes/` | 各阶段 pass：语义分析、代码生成 |
| `src/attached_probe.cpp` | ★ 探针附着逻辑（perf_event_open + link） |
| `src/probe_types.h` | 探针类型枚举和别名映射 |
| `src/bpfmap.cpp` | BPF Map 创建和管理 |
| `src/bpfprogram.cpp` | BPF 程序加载封装 |
| `src/bpffeature.cpp` | 内核 BPF 特性检测 |

### bcc（src/bcc/）

| 文件 | 关注点 |
|------|--------|
| `src/python/bcc/__init__.py` | ★ Python API 入口（BPF 类） |
| `src/cc/bcc_btf.cc` | BTF 处理 |
| `src/cc/bcc_syms.cc` | 符号解析（kprobe 地址查找） |
| `tools/` | 预置工具（opensnoop、biolatency 等） |
| `examples/` | 示例程序 |

### 推荐阅读顺序

```
1. /usr/include/linux/bpf.h           → 理解 BPF 指令、程序类型、Map 类型
2. src/libbpf/src/bpf.h               → 理解 bpf() 系统调用封装
3. src/libbpf/src/libbpf.h            → 理解 libbpf 高层 API
4. src/bpftrace/src/probe_types.h     → 理解 bpftrace 支持的探针类型
5. src/bpftrace/src/attached_probe.h  → 理解探针附着机制
6. src/bpftrace/src/ast/ast.h         → 理解 bpftrace 编译流程
7. src/bcc/src/python/bcc/__init__.py → 理解 bcc Python API
```

---

## 8. 与 blktrace 的对照学习

本仓库的 blktrace 学习材料追踪了 block I/O 的完整生命周期。BPF 可以用更灵活的方式实现类似功能：

### 对照表

| blktrace 方式 | BPF/bpftrace 方式 | 差异 |
|--------------|-------------------|------|
| `blktrace -d /dev/sda` | `bpftrace -e 'tracepoint:block:block_rq_issue {...}'` | bpftrace 更灵活，可自定义输出 |
| `blkparse -i trace` | bpftrace 直接输出可读文本 | 无需中间二进制文件 |
| `btt -i trace.bin` | bpftrace map 聚合（hist/avg/count） | 实时计算，无需后处理 |
| relay buffer | perf buffer / ringbuf | ringbuf 更高效，per-CPU 开销更低 |
| ioctl(BLKTRACESETUP) | bpf(BPF_PROG_LOAD) | BPF 通用接口，不仅限于 block |
| 只能追踪 block 事件 | 可追踪任意内核函数 | BPF 可 hook 到 kprobe/tracepoint/fentry |

### 同一事件的两种追踪方式

```bash
# blktrace 追踪 block_rq_issue（D 事件）
blktrace -d /dev/vdb -o trace -a issue -w 5
blkparse -i trace
# 输出：253,16  0  1  0.000343403  26  D  R  0 + 8 [kworker/0:1H]

# bpftrace 追踪同一事件
bpftrace -e 'tracepoint:block:block_rq_issue {
  printf("%d,%d  %s  sector=%d  bytes=%d  [%s]\n",
         args->dev >> 20, args->dev & 0xfffff,
         args->rwbs, args->sector, args->bytes, comm);
}'
# 输出：253,16  R  sector=0  bytes=4096  [kworker/0:1H]
```

→ **同一个内核 tracepoint（block_rq_issue），blktrace 通过 relay buffer 采集，bpftrace 通过 BPF 程序采集。**

---

## 参考资源

- [BPF Design Q&A](https://git.kernel.org/pub/scm/linux/kernel/git/bpf/bpf-next.git/tree/Documentation/bpf/bpf_design_QA.rst)
- [libbpf GitHub](https://github.com/libbpf/libbpf)
- [bpftrace GitHub](https://github.com/bpftrace/bpftrace)
- [bcc GitHub](https://github.com/iovisor/bcc)
- [BPF Helpers Reference](https://man7.org/linux/man-pages/man7/bpf-helpers.7.html)
- [bpf(2) man page](https://man7.org/linux/man-pages/man2/bpf.2.html)
- [bpftool(8) man page](https://man7.org/linux/man-pages/man8/bpftool.8.html)
- [BPF Performance Tools (Brendan Gregg)](http://www.brendangregg.com/bpf-performance-tools-book.html)
- [Linux Observability with BPF (O'Reilly)](https://www.oreilly.com/library/view/linux-observability-with/9781492050193/)
- [LWN: BPF — the universal in-kernel virtual machine](https://lwn.net/Articles/740157/)
- [Cilium BPF Reference Guide](https://docs.cilium.io/en/latest/bpf/)
