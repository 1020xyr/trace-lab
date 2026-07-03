# Step 1: BPF 架构详解 — eBPF 程序类型、Verifier、Map、Helper

> ======================================================================
> 阅读要点：
>   1. eBPF 虚拟机模型 — 寄存器、指令、执行流程
>   2. BPF 程序类型 — 决定程序能 hook 到哪里
>   3. Verifier 工作原理 — 为什么 BPF 程序是安全的
>   4. BPF Map 类型 — 内核与用户态的数据通道
>   5. Helper Functions — BPF 程序可调用的内核功能
>
> 相关源码：
>   - /usr/include/linux/bpf.h        — BPF 接口定义
>   - src/libbpf/src/libbpf.c         — ELF 加载与 CO-RE
>   - src/libbpf/src/bpf.c            — bpf() 系统调用封装
>   - src/libbpf/src/bpf_helper_defs.h — Helper 函数签名
>
> 预计阅读时间：25 分钟
> ======================================================================

---

## 一、eBPF 虚拟机模型

### 1.1 什么是 eBPF

eBPF（extended Berkeley Packet Filter）是 Linux 内核中的一个**受控虚拟机**。
它允许用户在不修改内核代码、不加载内核模块的情况下，在内核中运行自定义程序。

```
传统方式：                          eBPF 方式：
┌──────────────┐                  ┌──────────────┐
│  修改内核     │                  │  编写 BPF 程序│
│  或加载模块   │                  │  (C / DSL)   │
└──────┬───────┘                  └──────┬───────┘
       │                                 │
       ▼                                 ▼
  编译内核/模块                    bpf(PROG_LOAD)
       │                                 │
       ▼                                 ▼
  ★ 可能崩溃系统                  Verifier 安全检查
                                       │
                                       ▼
                                  JIT 编译 + 附着
                                       │
                                       ▼
                                  ★ 安全、零开销
```

### 1.2 BPF 指令格式

每条 BPF 指令固定 **8 字节**，定义在 `/usr/include/linux/bpf.h` 中：

```c
struct bpf_insn {
    __u8  code;       /* 操作码：指令类别 + 具体操作 */
    __u8  dst_reg:4;  /* 目标寄存器（r0-r10） */
    __u8  src_reg:4;  /* 源寄存器 */
    __s16 off;        /* 16 位偏移（跳转距离或内存偏移） */
    __s32 imm;        /* 32 位立即数 */
};
```

**操作码编码（code 字段）：**

```
code 字段的 8 位拆分：
┌──────────┬────────┬──────────┐
│ 类别(3b) │源(1b)  │ 操作(4b) │
│ BPF_ALU  │BPF_X/K │ ADD/MOV  │
└──────────┴────────┴──────────┘

类别：
  BPF_LD    = 0x00  加载（64 位立即数）
  BPF_LDX   = 0x01  从内存加载
  BPF_ST    = 0x02  存储立即数
  BPF_STX   = 0x03  存储寄存器
  BPF_ALU   = 0x04  32 位算术
  BPF_JMP   = 0x05  跳转
  BPF_ALU64 = 0x07  64 位算术

源：
  BPF_K = 0x00  使用立即数（imm）
  BPF_X = 0x08  使用寄存器（src_reg）
```

### 1.3 寄存器模型

BPF 虚拟机有 **11 个 64 位寄存器**（r0-r10）和一个固定大小的栈：

```
┌────────────────────────────────────────────────────────────┐
│                    BPF 寄存器与栈                          │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  r0          │ 函数返回值（退出时的程序返回值）             │
│  r1          │ ★ 第一个参数：上下文指针（ctx）             │
│  r2-r5       │ 函数参数（helper 调用时传参）               │
│  r6-r9       │ 被调用者保存（callee-saved）                │
│  r10 (fp)    │ ★ 帧指针（只读，指向栈底）                  │
│              │                                            │
│  ┌─────────┐│                                            │
│  │ 512字节  ││ ← r10 指向这里（栈底）                     │
│  │ BPF 栈   ││                                            │
│  │ (固定)   ││ ← r10 - 512（栈顶）                       │
│  └─────────┘│                                            │
│                                                            │
└────────────────────────────────────────────────────────────┘

★ 关键限制：
  - 栈只有 512 字节 → 大数组必须放在 Map 中
  - r10 只读 → 不能移动栈指针
  - 未初始化的寄存器不可读 → verifier 检查
```

### 1.4 BPF 程序执行流程

```
事件发生（如 kprobe 命中）
    │
    ▼
内核跳转到 BPF 程序入口
    │
    ├── r1 = ctx（上下文指针）
    │   kprobe → struct pt_regs *
    │   tracepoint → tracepoint 结构体指针
    │   XDP → struct xdp_md *
    │
    ▼
执行 BPF 指令序列
    │
    ├── 读取上下文：r2 = *(u64 *)(r1 + offset)
    ├── 调用 helper：call bpf_get_current_pid_tgid
    ├── 读写 Map：call bpf_map_lookup_elem
    ├── 条件跳转：if r1 > 0 goto label
    │
    ▼
程序退出
    │
    ├── r0 = 0 → 继续正常执行（不干预）
    ├── r0 = 1 → 特殊行为（如 XDP_PASS）
    └── r0 = 2 → 特殊行为（如 XDP_DROP）
```

---

## 二、BPF 程序类型

### 2.1 程序类型与 hook 点的关系

BPF 程序类型决定了：
1. 程序能附着到**哪里**（hook 点）
2. 程序入口的**上下文类型**（r1 指向什么）
3. 程序可以使用哪些 **helper functions**
4. 程序的**返回值含义**

### 2.2 追踪相关的程序类型（★ 重点）

#### BPF_PROG_TYPE_KPROBE（枚举值 2）

```c
/* 用于 kprobe、kretprobe、uprobe、uretprobe */

上下文：struct pt_regs *
  r1 → pt_regs（包含所有 CPU 寄存器快照）

kprobe: 在函数入口处触发
  - 可以读取函数参数（通过 pt_regs 中的寄存器）
  - x86_64 参数：rdi(arg1), rsi(arg2), rdx(arg3), rcx(arg4), r8(arg5), r9(arg6)

kretprobe: 在函数返回时触发
  - 可以读取返回值（pt_regs->ax = rax）
  - 不能读取函数参数（参数可能已被覆盖）

源码位置：
  内核：kernel/trace/trace_kprobe.c
  bpftrace：src/bpftrace/src/probe_types.h → ProbeType::kprobe
```

#### BPF_PROG_TYPE_TRACEPOINT（枚举值 5）

```c
/* 用于内核静态 tracepoint */

上下文：tracepoint 定义的参数结构体
  r1 → 指向 tracepoint 的参数数据

特点：
  - ★ 参数有明确的类型定义（不像 kprobe 需要从寄存器猜）
  - tracepoint 是内核开发者显式定义的 hook 点，ABI 稳定
  - 通过 /sys/kernel/debug/tracing/events/ 可以查看所有可用 tracepoint

示例：tracepoint:block:block_rq_issue
  参数结构体包含：dev, sector, nr_sector, bytes, rwbs, comm
  直接通过 args->sector 访问，不需要猜测寄存器

源码位置：
  内核：include/trace/events/*.h（定义）
  内核：kernel/trace/trace_events.c（注册）
  bpftrace：ProbeType::tracepoint
```

#### BPF_PROG_TYPE_RAW_TRACEPOINT（枚举值 17）

```c
/* 原始 tracepoint — 跳过参数预处理，更快 */

上下文：struct bpf_raw_tracepoint_args *
  struct bpf_raw_tracepoint_args {
      __u64 args[0];  // 直接传递 tracepoint 的原始参数
  };

特点：
  - 比 BPF_PROG_TYPE_TRACEPOINT 少一层参数拷贝
  - 需要自己知道参数的类型和顺序
  - ★ 性能更好（适合高频 tracepoint）

源码位置：
  内核：kernel/trace/bpf_trace.c
  bpftrace：ProbeType::rawtracepoint
```

#### BPF_PROG_TYPE_TRACING（枚举值 26）

```c
/* fentry/fexit — 基于 BTF 的新一代追踪 */

上下文：被追踪函数的参数（由 BTF 推导）
  fentry: 直接访问函数参数
  fexit:  访问函数参数 + 返回值

特点：
  - ★ 不需要 kprobe 的 int3 断点机制，开销更低
  - 通过 BTF 知道参数类型，不需要手动从寄存器提取
  - 内核 5.5+ 支持

fentry 与 kprobe 对比：
  kprobe: int3 断点 → save_regs → BPF(ctx=pt_regs) → iret
  fentry: 函数入口直接调用 BPF(args...) → 返回
  ★ fentry 少了一次中断 + 寄存器保存，开销低约 50%

源码位置：
  内核：kernel/bpf/trampoline.c（trampoline 生成）
  bpftrace：ProbeType::fentry / ProbeType::fexit
```

#### BPF_PROG_TYPE_PERF_EVENT（枚举值 7）

```c
/* 用于 profile、interval、watchpoint 等 */

上下文：struct bpf_perf_event_data *
  struct bpf_perf_event_data {
      struct pt_regs regs;     // CPU 寄存器快照
      __u64 sample_period;     // 采样周期
      __u64 addr;              // 事件地址
  };

用途：
  - profile: 每 N 毫秒采样一次（CPU profiling）
  - interval: 每 N 秒触发一次（定时聚合输出）
  - watchpoint: 内存地址被读写时触发
  - software/hardware: perf 事件触发

源码位置：
  内核：kernel/events/core.c
  bpftrace：ProbeType::profile / interval / watchpoint
```

### 2.3 非追踪类程序类型（了解即可）

| 类型 | 用途 | hook 点 |
|------|------|---------|
| `XDP` | 网卡驱动层包处理 | 网卡收到包后、协议栈之前 |
| `CGROUP_SKB` | cgroup 网络过滤 | cgroup 的入/出网络包 |
| `CGROUP_SOCK` | cgroup socket 控制 | socket 创建/连接 |
| `SOCK_OPS` | socket 操作 hook | TCP 连接建立/重传等 |
| `STRUCT_OPS` | 替换内核操作表 | TCP 拥塞控制等 |
| `LSM` | 安全模块 | 文件/进程/网络权限检查 |
| `NETFILTER` | netfilter hook | iptables 规则链 |

---

## 三、Verifier 工作原理

### 3.1 Verifier 的角色

Verifier 是 BPF 安全的**核心**。它在程序加载时运行，通过静态分析确保程序不会危害内核。

```
用户提交 BPF 字节码
    │
    ▼
bpf(BPF_PROG_LOAD, {prog, insns, type, license})
    │
    ▼
┌──────────────────────────────────────────────┐
│              Verifier 两阶段检查              │
│                                              │
│  阶段 1：静态分析（DAG 检查）                │
│  ┌────────────────────────────────────────┐  │
│  │ • 检查指令数量 ≤ 1,000,000             │  │
│  │ • 检查所有路径都能到达 exit             │  │
│  │ • 检查无 unreachable 指令               │  │
│  │ • 检查无不可控循环（bounded loop 除外） │  │
│  │ • 检查跳转目标在程序范围内             │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  阶段 2：模拟执行（逐指令类型推断）          │
│  ┌────────────────────────────────────────┐  │
│  │ • 模拟每条指令的执行                    │  │
│  │ • 追踪每个寄存器的类型和值范围          │  │
│  │ • 检查内存访问的边界和类型安全          │  │
│  │ • 检查 helper 调用的参数类型            │  │
│  │ • 检查 Map 访问的 key 范围              │  │
│  │ • 在分支点合并状态（状态空间探索）      │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  通过 → JIT 编译 + 返回 prog_fd             │
│  失败 → 返回错误 + verifier log              │
└──────────────────────────────────────────────┘
```

### 3.2 Verifier 类型系统（★ 核心）

Verifier 为每个寄存器维护一个**类型标签**，确保类型安全：

```c
/* verifier 内部使用的寄存器类型（简化） */
enum bpf_reg_type {
    NOT_INIT = 0,              /* 未初始化 */
    SCALAR_VALUE,              /* 普通整数（有已知范围） */
    PTR_TO_CTX,                /* ★ 指向上下文（如 pt_regs） */
    PTR_TO_MAP_KEY,            /* 指向 Map 的 key */
    PTR_TO_MAP_VALUE,          /* ★ 指向 Map 的 value */
    CONST_PTR_TO_MAP,          /* 指向 Map 结构的常量指针 */
    PTR_TO_STACK,              /* 指向栈空间 */
    PTR_TO_PACKET,             /* 指向网络包数据 */
    PTR_TO_PACKET_END,         /* 指向包尾（边界检查用） */
    PTR_TO_BTF_ID,             /* 指向 BTF 描述的内核结构体 */
    PTR_TO_MEM,                /* 指向已知大小的内存区域 */
};
```

**类型检查示例：**

```
; int val = *map_value;          ← PTR_TO_MAP_VALUE，合法
; int ctx_val = *ctx;            ← PTR_TO_CTX，合法
; int kernel = *(int *)0xffff888000000000;  ← SCALAR，★ 非法！
                                            不能直接解引用任意地址

; bpf_probe_read(&buf, sizeof(buf), ptr);  ← 通过 helper 读内核内存，合法
```

### 3.3 边界检查示例

```
假设 Map value 大小 = 8 字节：

  r1 = map_lookup_elem(...)     → r1 = PTR_TO_MAP_VALUE (size=8)
  r2 = *(u64 *)(r1 + 0)         → 合法：offset=0, size=8, 0+8 ≤ 8
  r3 = *(u64 *)(r1 + 4)         → 非法：offset=4, size=8, 4+8=12 > 8
                                   ★ Verifier 拒绝！

  if (key < MAX) {              → Verifier 追踪 key 的范围
    val = map[key];             → 在 if 分支内，key < MAX 已知
  }                             → 边界检查通过
```

### 3.4 Verifier 失败时如何调试

```bash
# 方法 1：bpftool 加载并获取 log
bpftool prog load prog.bpf.o /sys/fs/bpf/test 2>&1

# 方法 2：libbpf 设置 log buffer
bpf_program__set_log_level(prog, 1);
bpf_program__set_log_buf(prog, log_buf, sizeof(log_buf));
bpf_program__load(prog);
printf("%s", log_buf);  // 打印 verifier 的详细日志

# 方法 3：bpftrace 自动显示
bpftrace -v -e '...'  # -v 显示编译细节，包括 verifier 错误
```

---

## 四、BPF Map 类型详解

### 4.1 Map 的通用操作接口

所有 Map 类型共享相同的 bpf() 系统调用接口：

```c
/* 用户态通过 bpf() 系统调用操作 Map */
union bpf_attr attr = {
    .map_fd = map_fd,
    .key    = (u64)&key,
    .value  = (u64)&value,
    .flags  = BPF_ANY,        /* BPF_ANY/BPF_NOEXIST/BPF_EXIST */
};

bpf(BPF_MAP_CREATE, ...)      /* 创建 Map */
bpf(BPF_MAP_LOOKUP_ELEM, ...)  /* 根据 key 查找 value */
bpf(BPF_MAP_UPDATE_ELEM, ...)  /* 插入/更新键值对 */
bpf(BPF_MAP_DELETE_ELEM, ...)  /* 删除键值对 */
bpf(BPF_MAP_GET_NEXT_KEY, ...) /* 遍历所有 key */
```

```c
/* BPF 程序内部通过 helper 操作 Map */
void *bpf_map_lookup_elem(map, key);        /* 返回指向 value 的指针 */
long  bpf_map_update_elem(map, key, val, flags);
long  bpf_map_delete_elem(map, key);
```

### 4.2 常用 Map 类型详解

#### HASH — 通用哈希表

```c
/* 定义 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);     /* 最大元素数量 */
    __type(key, pid_t);             /* key 类型 */
    __type(value, u64);             /* value 类型 */
} counts SEC(".maps");

/* 使用场景：统计每个进程的系统调用次数 */
counts 在内存中的布局：
┌──────────────────────────────────┐
│ bucket[0]: {pid=1234: 42} → NULL│
│ bucket[1]: {pid=5678: 17} → NULL│
│ bucket[2]: {pid=9012: 103}→NULL │
│ ...                              │
│ bucket[N-1]: (empty)             │
└──────────────────────────────────┘
查找/插入/删除：O(1) 平均
★ 最通用的 Map 类型
```

#### PERCPU_HASH — per-CPU 无锁哈希表

```c
/* 定义 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 10240);
    __type(key, pid_t);
    __type(value, u64);
} percpu_counts SEC(".maps");

/* 每个 CPU 有独立的 value 副本 */
CPU 0: {pid=1234: 42}
CPU 1: {pid=1234: 17}
CPU 2: {pid=1234: 31}
              ↓ 用户态聚合
        总计: 42 + 17 + 31 = 90

★ 优点：写入无需加锁（每个 CPU 只写自己的副本）
★ 缺点：读取时需要聚合所有 CPU 的值
★ 用途：高并发计数、聚合统计
```

#### PERF_EVENT_ARRAY — perf buffer

```c
/* 定义 */
struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(int));      /* CPU 号 */
    __uint(value_size, sizeof(int));    /* perf fd */
    __uint(max_entries, 128);           /* CPU 数量 */
} events SEC(".maps");

/* BPF 程序端：发送事件 */
bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &event, sizeof(event));

/* 用户态：poll + read */
perf_buffer__poll(pb, 100);

数据流：
  BPF 程序 → per-CPU perf buffer → 用户态 poll/read
  ★ 注意：per-CPU 独立缓冲，可能丢事件（buffer 满时）
  ★ 老内核的标准方式，5.8+ 推荐用 RINGBUF
```

#### RINGBUF — 环形缓冲区（★ 推荐）

```c
/* 定义 */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);  /* 256KB 环形缓冲 */
} rb SEC(".maps");

/* BPF 程序端：两种写入方式 */
/* 方式 1：直接输出（简单但可能有拷贝） */
bpf_ringbuf_output(&rb, &event, sizeof(event), 0);

/* 方式 2：预留 + 提交（零拷贝） */
void *e = bpf_ringbuf_reserve(&rb, sizeof(event), 0);
if (e) {
    memcpy(e, &event, sizeof(event));
    bpf_ringbuf_submit(e, 0);
}

/* 用户态：mmap + callback */
ring_buffer__new(map_fd, callback, NULL, NULL);
ring_buffer__poll(rb, 100);

★ 优势：
  - 全局共享（非 per-CPU），内存利用率更高
  - 支持零拷贝（reserve + submit）
  - 几乎不丢事件
  - 内核 5.8+ 支持
```

#### STACK_TRACE — 调用栈存储

```c
/* 定义 */
struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(max_entries, 10240);
    __uint(key_size, sizeof(u64));      /* stack_id */
    __uint(value_size, 127 * sizeof(u64)); /* 最多 127 层调用栈 */
} stacks SEC(".maps");

/* BPF 程序端：捕获调用栈 */
u64 stack_id = bpf_get_stackid(ctx, &stacks, BPF_F_FAST_STACK_CMP);

/* 用户态：读取调用栈 */
struct bpf_stacktrace trace;
bpf_map_lookup_elem(map_fd, &stack_id, &trace);
for (int i = 0; i < trace.nr && trace.ip[i]; i++) {
    printf("  %lx\n", trace.ip[i]);  // 指令地址
}
```

### 4.3 Map 类型选择决策树

```
需要存储键值对？
  ├── 是 → 需要多线程无锁写入？
  │         ├── 是 → PERCPU_HASH / PERCPU_ARRAY
  │         └── 否 → HASH / ARRAY
  │                  ├── key 范围固定 → ARRAY（更快）
  │                  └── key 任意 → HASH
  │
  └── 否 → 需要传输事件流？
            ├── 是 → 内核 ≥ 5.8？
            │         ├── 是 → ★ RINGBUF（推荐）
            │         └── 否 → PERF_EVENT_ARRAY
            │
            └── 否 → 需要调用栈？
                      ├── 是 → STACK_TRACE
                      ├── 否 → 需要 FIFO/LIFO？
                      │         ├── FIFO → QUEUE
                      │         └── LIFO → STACK
                      └── 其他 → BLOOM_FILTER / LPM_TRIE
```

---

## 五、Helper Functions

### 5.1 Helper 的本质

Helper Functions 是 BPF 程序与内核交互的**唯一合法途径**（除了 Map 操作）。
它们本质上是内核导出的 C 函数，BPF 程序通过 `call` 指令调用。

```
BPF 字节码：                    内核实现：
  call 14                       → bpf_get_current_pid_tgid()
  call 15                       → bpf_get_current_uid_gid()
  call 16                       → bpf_get_current_comm()
  call 1                        → bpf_map_lookup_elem()
  call 2                        → bpf_map_update_elem()
  call 6                        → bpf_trace_printk()

★ Helper 编号在 bpf_helper_defs.h 中定义
★ 不是所有 Helper 对所有程序类型都可用
```

### 5.2 Helper 分类（★ 追踪常用）

#### 信息获取类

| Helper | 返回值 | 说明 |
|--------|--------|------|
| `bpf_get_current_pid_tgid()` | `(tgid << 32) \| pid` | 当前 PID（注意高 32 位是 TGID） |
| `bpf_get_current_uid_gid()` | `(gid << 32) \| uid` | 当前用户 ID |
| `bpf_get_current_comm(buf, size)` | 0 / 错误码 | 当前进程名（如 "fio"、"bash"） |
| `bpf_ktime_get_ns()` | u64 纳秒 | 内核单调时钟 |
| `bpf_get_smp_processor_id()` | CPU 号 | 当前 CPU |

#### 内存访问类

| Helper | 说明 |
|--------|------|
| `bpf_probe_read_kernel(dst, size, src)` | ★ 安全读取内核内存 |
| `bpf_probe_read_user(dst, size, src)` | 安全读取用户态内存 |
| `bpf_probe_read_kernel_str(dst, size, src)` | 读取内核字符串（自动截断） |
| `bpf_probe_read_user_str(dst, size, src)` | 读取用户态字符串 |

**为什么需要 probe_read 而不是直接指针解引用？**

```
直接解引用：                    probe_read：
r2 = *(u64 *)(r1)               bpf_probe_read(&buf, 8, ptr)
  │                               │
  ▼                               ▼
如果 r1 是非法地址              内核安全读取：
  → 内核崩溃 (Oops)               - 检查地址合法性
  → verifier 拒绝                 - 捕获 page fault
                                  - 失败时返回错误码
                                  ★ 不会崩溃系统
```

#### 输出类

| Helper | 说明 |
|--------|------|
| `bpf_trace_printk(fmt, size, ...)` | 输出到 `/sys/kernel/debug/tracing/trace_pipe` |
| `bpf_perf_event_output(ctx, map, flags, data, size)` | 写入 perf buffer |
| `bpf_ringbuf_output(map, data, size, flags)` | 写入 ringbuf |
| `bpf_ringbuf_reserve(map, size, flags)` | 预留 ringbuf 空间 |
| `bpf_ringbuf_submit(data, flags)` | 提交到 ringbuf |
| `bpf_ringbuf_discard(data, flags)` | 丢弃预留空间 |

#### 网络类（非追踪，了解即可）

| Helper | 说明 |
|--------|------|
| `bpf_skb_load_bytes(skb, offset, to, len)` | 从网络包读取数据 |
| `bpf_skb_store_bytes(skb, offset, from, len, flags)` | 写入网络包 |
| `bpf_redirect(ifindex, flags)` | 重定向网络包 |
| `bpf_xdp_adjust_head(xdp, offset)` | 调整 XDP 包头 |

### 5.3 Helper 可用性矩阵

不是所有 helper 对所有程序类型可用。以下是追踪相关的可用性：

| Helper | KPROBE | TRACEPOINT | RAW_TP | TRACING | PERF_EVENT |
|--------|--------|-----------|--------|---------|------------|
| `get_current_pid_tgid` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `get_current_comm` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `probe_read_kernel` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `probe_read_user` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `ktime_get_ns` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `get_stackid` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `perf_event_output` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `ringbuf_output` | ✅ | ✅ | ✅ | ✅ | ✅ |

---

## 六、JIT 编译器

### 6.1 JIT 的作用

BPF 字节码默认由内核解释器逐条执行。JIT（Just-In-Time）编译器将字节码转为**本地机器码**，实现接近原生的性能。

```
无 JIT（解释执行）：              有 JIT（本地机器码）：
┌─────────────┐                 ┌─────────────┐
│ BPF 字节码   │                 │ BPF 字节码   │
│  8字节/指令  │                 │  8字节/指令  │
└──────┬──────┘                 └──────┬──────┘
       │                               │
       ▼                               ▼
  内核解释器                      JIT 编译器
  (逐条翻译执行)                  (bpf_jit_comp.c)
       │                               │
       ▼                               ▼
  ★ 慢 5-10x                     ★ x86_64 原生指令
                                   ★ 接近零开销
```

### 6.2 JIT 配置

```bash
# 查看 JIT 状态
cat /proc/sys/net/core/bpf_jit_enable
# 1 = 启用（推荐），0 = 禁用

# 查看 JIT 详细日志
echo 2 > /proc/sys/net/core/bpf_jit_enable  # 2 = 启用 + 打印日志
dmesg | grep -i "bpf_jit"

# 查看已加载程序的 JIT 状态
bpftool prog show
# 输出中 "jited" 字段：1 = 已 JIT 编译
```

### 6.3 JIT 安全

```
CONFIG_BPF_JIT_ALWAYS_ON=y  ← ★ 本系统配置
  - JIT 始终启用，解释器被禁用
  - 防止 JIT 喷射攻击（攻击者利用解释器的间接跳转）
  - 生产环境推荐配置
```

---

## 总结

### 核心概念映射表

| 概念 | 说明 | 类比 |
|------|------|------|
| BPF 程序 | 在内核中运行的沙箱代码 | 内核模块（但更安全） |
| BPF Map | 内核 ↔ 用户态的数据通道 | 共享内存 |
| Verifier | 加载时的安全检查器 | 代码审查 |
| JIT | 字节码 → 机器码 | Java JIT |
| Helper | BPF 可调用的内核函数 | 系统调用 |
| CO-RE | 编译一次，到处运行 | Java "Write Once, Run Anywhere" |
| BTF | 类型信息，支持 CO-RE | 调试符号（DWARF） |

### 调用链总结

```
用户编写代码
    ↓
LLVM → BPF 字节码 (ELF)
    ↓
libbpf → bpf(PROG_LOAD)
    ↓
Verifier 检查 → 通过
    ↓
JIT 编译 → 本地机器码
    ↓
perf_event_open + bpf_link_create → 附着
    ↓
事件触发 → BPF 程序执行
    ↓
结果 → Map / perf buffer / ringbuf
    ↓
用户态 → poll/read/mmap → 读取结果
```
