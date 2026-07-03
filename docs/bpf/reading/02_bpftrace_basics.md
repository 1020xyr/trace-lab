# Step 2: bpftrace 基础 — Probe 类型、内置变量、Map 操作、聚合函数

> ======================================================================
> 阅读要点：
>   1. bpftrace 程序结构 — probe + action 的基本模型
>   2. 所有 probe 类型详解 — kprobe/tracepoint/uprobe/usdt/profile/...
>   3. 内置变量 — comm, pid, tid, nsecs, cpu, arg0-argN
>   4. Map 操作与聚合函数 — @, count(), sum(), avg(), hist(), lhist()
>   5. bpftrace 编译流程 — DSL → AST → LLVM IR → BPF 字节码
>
> 相关源码：
>   - src/bpftrace/src/probe_types.h      — probe 类型定义
>   - src/bpftrace/src/ast/ast.h          — AST 节点定义
>   - src/bpftrace/src/attached_probe.cpp — probe 附着逻辑
>   - src/bpftrace/src/bpftrace.cpp       — 核心运行时
>   - src/bpftrace/src/bpfmap.cpp         — Map 管理
>
> 预计阅读时间：25 分钟
> ======================================================================

---

## 一、bpftrace 程序模型

### 1.1 基本结构

bpftrace 程序由一个或多个 **probe + action** 块组成：

```
probe 类型:附着点 [, probe 类型:附着点 ...]
/过滤条件/
{
    action 语句
}
```

**最小示例：**

```bash
# 追踪每次文件打开
kprobe:do_sys_openat2
{
    printf("%s opened a file\n", comm);
}
```

**完整结构示例：**

```bash
/* 多 probe 程序 */

/* 1. 程序启动时初始化 */
BEGIN
{
    printf("Tracing file operations...\n");
}

/* 2. 追踪文件打开（kprobe） */
kprobe:do_sys_openat2
/comm == "fio"/                    ← 过滤条件：只追踪 fio 进程
{
    @opens[comm] = count();        ← 聚合统计
    @files[str(arg1)] = count();   ← 按文件名统计
}

/* 3. 定时输出（interval） */
interval:s:5                       ← 每 5 秒触发
{
    printf("--- 5 second summary ---\n");
    print(@opens);
}

/* 4. 程序结束时输出汇总 */
END
{
    printf("\n=== Final Summary ===\n");
    print(@opens);
    print(@files);
}
```

### 1.2 bpftrace 与 awk 的设计哲学对比

```
awk：  处理文本流    → pattern { action }
bpftrace：处理内核事件流 → probe   { action }

awk:     /^ERROR/ { count++ }            → 匹配文本模式
bpftrace: kprobe:vfs_read { @r=count(); } → 匹配内核事件

★ 两者都是 "事件 → 过滤 → 动作" 的模型
```

---

## 二、Probe 类型详解

### 2.1 完整 probe 类型表

| Probe 语法 | 别名 | 底层程序类型 | 说明 |
|-----------|------|-------------|------|
| `kprobe:func` | `k:func` | BPF_PROG_TYPE_KPROBE | 内核函数入口 |
| `kretprobe:func` | `kr:func` | BPF_PROG_TYPE_KPROBE | 内核函数返回 |
| `tracepoint:sub:event` | `t:sub:event` | BPF_PROG_TYPE_TRACEPOINT | 内核 tracepoint |
| `rawtracepoint:event` | `rt:event` | BPF_PROG_TYPE_RAW_TRACEPOINT | 原始 tracepoint |
| `uprobe:lib:func` | `u:lib:func` | BPF_PROG_TYPE_KPROBE | 用户态函数入口 |
| `uretprobe:lib:func` | `ur:lib:func` | BPF_PROG_TYPE_KPROBE | 用户态函数返回 |
| `usdt:lib:probe` | `U:lib:probe` | BPF_PROG_TYPE_KPROBE | USDT 探针 |
| `fentry:func` | — | BPF_PROG_TYPE_TRACING | 函数入口（BTF） |
| `fexit:func` | — | BPF_PROG_TYPE_TRACING | 函数退出（BTF） |
| `profile:hz:N` | `p:hz:N` | BPF_PROG_TYPE_PERF_EVENT | 每秒采样 N 次 |
| `profile:us:N` | `p:us:N` | BPF_PROG_TYPE_PERF_EVENT | 每 N 微秒采样 |
| `interval:s:N` | `i:s:N` | BPF_PROG_TYPE_PERF_EVENT | 每 N 秒触发 |
| `software:event:N` | `s:event:N` | BPF_PROG_TYPE_PERF_EVENT | 软件 perf 事件 |
| `hardware:event:N` | `h:event:N` | BPF_PROG_TYPE_PERF_EVENT | 硬件 PMC 事件 |
| `watchpoint:addr:len:mode` | `w:addr` | BPF_PROG_TYPE_PERF_EVENT | 内存监视 |
| `BEGIN` | — | 特殊 | 程序启动时触发 |
| `END` | — | 特殊 | 程序结束时触发 |

### 2.2 kprobe / kretprobe

```bash
# kprobe：在函数入口处触发
kprobe:do_sys_openat2
{
    /* arg0, arg1, ... 对应函数的参数
     * x86_64 调用约定：
     *   arg0 = rdi, arg1 = rsi, arg2 = rdx
     *   arg3 = rcx, arg4 = r8,  arg5 = r9
     */
    printf("open: comm=%s file=%s\n", comm, str(arg1));
}

# kretprobe：在函数返回时触发
kretprobe:do_sys_openat2
{
    /* retval = 函数返回值
     * 对于 open：返回值是 fd（成功）或负数错误码
     */
    printf("open returned: %d\n", retval);
}

# 组合使用：计算函数耗时
kprobe:vfs_read
{
    @start[tid] = nsecs;           /* 记录开始时间 */
}

kretprobe:vfs_read
/@start[tid]/                       /* 过滤：只处理有开始时间的 */
{
    $dur = nsecs - @start[tid];    /* 计算耗时 */
    @read_ns = hist($dur);         /* 直方图 */
    delete(@start[tid]);           /* 清理 */
}
```

**参数访问：**

```
kprobe 参数映射（x86_64）：
  arg0 → rdi  → 第 1 个参数
  arg1 → rsi  → 第 2 个参数
  arg2 → rdx  → 第 3 个参数
  arg3 → rcx  → 第 4 个参数（注意：不是 r10）
  arg4 → r8   → 第 5 个参数
  arg5 → r9   → 第 6 个参数

kretprobe 返回值：
  retval → rax → 函数返回值

★ 注意：argN 是 bpftrace 提供的便捷别名
        底层通过 struct pt_regs 的寄存器偏移实现
```

### 2.3 tracepoint

```bash
# 语法：tracepoint:子系统:事件名
tracepoint:syscalls:sys_enter_read
{
    /* args->xxx 访问 tracepoint 定义的结构体字段 */
    printf("read: fd=%d count=%d\n", args->fd, args->count);
}

tracepoint:block:block_rq_issue
{
    /* block tracepoint 的字段（与 blktrace 追踪的是同一事件） */
    printf("block I/O: dev=%d sector=%d bytes=%d rwbs=%s\n",
           args->dev, args->sector, args->bytes, args->rwbs);
}
```

**tracepoint vs kprobe 对比：**

```
kprobe:do_sys_openat2                  tracepoint:syscalls:sys_enter_openat2
  │                                      │
  ├── arg0, arg1, ...（寄存器）          ├── args->dfd, args->filename, ...
  ├── 参数类型未知，需要猜              ├── ★ 参数有明确类型和名称
  ├── 函数内部实现变化可能导致失效      ├── ★ ABI 稳定，不会随内核变化
  └── 可以 hook 任意内核函数            └── 只能用已定义的 tracepoint

★ 选择原则：
  - 有 tracepoint 的 → 优先用 tracepoint（更稳定、更清晰）
  - 没有 tracepoint 的 → 用 kprobe（更灵活）
```

**查看可用 tracepoint：**

```bash
# 列出所有 tracepoint
bpftrace -l 'tracepoint:*' | head -20

# 查看某个 tracepoint 的参数格式
cat /sys/kernel/debug/tracing/events/syscalls/sys_enter_read/format
# 输出：
# field:int __syscall_nr;   offset:8;  size:4;
# field:unsigned int fd;    offset:16; size:8;
# field:char * buf;         offset:24; size:8;
# field:size_t count;       offset:32; size:8;
```

### 2.4 uprobe / uretprobe / usdt

```bash
# uprobe：追踪用户态库函数
uprobe:/lib/x86_64-linux-gnu/libc.so.6:malloc
{
    printf("malloc(%d) called by %s\n", arg0, comm);
}

# uretprobe：追踪返回值
uretprobe:/lib/x86_64-linux-gnu/libc.so.6:malloc
{
    printf("malloc returned %p\n", retval);
}

# usdt：追踪应用内嵌的 DTrace 探针
# 需要先确认应用有 USDT 探针：
bpftrace -l 'usdt:/usr/lib/jvm/java-*/lib/server/libjvm.so:*'

usdt:/usr/lib/jvm/java-*/lib/server/libjvm.so:gc__begin
{
    printf("GC started\n");
}
```

### 2.5 fentry / fexit（新一代追踪）

```bash
# fentry：基于 BTF 的函数入口追踪（内核 5.5+）
fentry:do_sys_openat2
{
    /* 直接通过 BTF 知道参数名和类型！ */
    printf("open: %s\n", str(args->filename));
}

# fexit：函数退出时，可以同时访问参数和返回值
fexit:do_sys_openat2
{
    printf("open(%s) = %d\n", str(args->filename), retval);
}

★ fentry/fexit vs kprobe/kretprobe：
  - fentry/fexit 基于 BTF，参数有类型信息
  - 性能更好（无 int3 断点开销）
  - 需要内核 CONFIG_DEBUG_INFO_BTF=y
```

### 2.6 profile / interval

```bash
# profile：CPU 采样（每秒 99 次，质数避免与周期事件同步）
profile:hz:99
{
    @[comm] = count();    /* 统计各进程被采样到的次数 */
}

# profile：带调用栈的 CPU profiling
profile:hz:99
{
    @[kstack] = count();  /* 统计各内核调用栈 */
}

# interval：定时间隔输出
interval:s:1
{
    printf("--- 每秒报告 ---\n");
    print(@reads);
    clear(@reads);
}
```

---

## 三、内置变量

### 3.1 进程与线程信息

| 变量 | 类型 | 说明 |
|------|------|------|
| `pid` | u32 | ★ 进程 ID（PID / TGID，高 32 位） |
| `tid` | u32 | ★ 线程 ID（内核 TID，低 32 位） |
| `uid` | u32 | 用户 ID |
| `gid` | u32 | 组 ID |
| `comm` | string | ★ 进程名（16 字节截断，如 "fio"） |
| `cgroup` | u64 | cgroup ID |

```bash
# 示例：显示谁在读取文件
kprobe:vfs_read
{
    printf("pid=%d tid=%d comm=%s uid=%d\n", pid, tid, comm, uid);
}
```

### 3.2 时间与 CPU

| 变量 | 类型 | 说明 |
|------|------|------|
| `nsecs` | u64 | ★ 纳秒时间戳（bpf_ktime_get_ns()） |
| `elapsed` | u64 | 程序运行以来的纳秒数 |
| `cpu` | u32 | 当前 CPU 号 |
| `cgroup` | u64 | cgroup v2 ID |

```bash
# 示例：计算函数耗时
kprobe:vfs_read { @start[tid] = nsecs; }
kretprobe:vfs_read /@start[tid]/ {
    $dur_ns = nsecs - @start[tid];
    @latency_us = hist($dur_ns / 1000);  /* 转微秒 */
    delete(@start[tid]);
}
```

### 3.3 函数参数与返回值

| 变量 | 适用 probe | 说明 |
|------|-----------|------|
| `arg0` - `arg9` | kprobe, uprobe | 函数参数（通过寄存器） |
| `args->field` | tracepoint, fentry/fexit | tracepoint 结构体字段 |
| `retval` | kretprobe, uretprobe, fexit | 函数返回值 |
| `reg("rax")` | kprobe | 读取特定寄存器 |

```bash
# argN 示例
kprobe:do_sys_openat2
{
    /* do_sys_openat2(int dfd, const char *filename,
     *                struct open_how *how, unsigned int flags) */
    printf("dfd=%d file=%s flags=0x%x\n", arg0, str(arg1), arg3);
}

# args-> 示例
tracepoint:syscalls:sys_enter_write
{
    printf("write: fd=%d count=%d\n", args->fd, args->count);
}
```

### 3.4 调用栈

| 变量 | 说明 |
|------|------|
| `kstack` | ★ 内核调用栈 |
| `ustack` | 用户态调用栈 |
| `kstack(5)` | 内核调用栈（限制 5 层） |

```bash
# CPU profiling 带调用栈
profile:hz:99
/pid == 1234/
{
    @[kstack, ustack] = count();
}
```

---

## 四、Map 操作与聚合函数

### 4.1 Map 变量

```bash
# 以 @ 开头的变量自动成为 BPF Map
@                    → 全局 map（匿名）
@counts              → 命名 map
@start[tid]          → 以 tid 为 key 的 map
@latency[comm, cpu]  → 以 (comm, cpu) 为 key 的 map

# 以 $ 开头的变量是 bpftrace 局部变量（存在 BPF 栈上）
$x = 42;             → 局部整数变量
$s = "hello";        → 局部字符串变量
```

### 4.2 聚合函数

| 函数 | 说明 | 示例 |
|------|------|------|
| `count()` | ★ 计数 | `@syscalls[comm] = count();` |
| `sum(n)` | 求和 | `@bytes[comm] = sum(arg2);` |
| `avg(n)` | 平均值 | `@avg_lat[comm] = avg($dur);` |
| `min(n)` | 最小值 | `@min_lat = min($dur);` |
| `max(n)` | 最大值 | `@max_lat = max($dur);` |
| `stats(n)` | 统计（count/avg/sum） | `@s = stats($dur);` |
| `hist(n)` | ★ 2 的幂次直方图 | `@lat = hist($dur_ns / 1000);` |
| `lhist(n, min, max, step)` | 线性直方图 | `@lat = lhist($dur_us, 0, 1000, 100);` |

**hist() 输出示例：**

```
@lat:
[0]                2 |@@                                      |
[1]               15 |@@@@@@@@@@                              |
[2, 4)            23 |@@@@@@@@@@@@@@@                         |
[4, 8)            47 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@         |
[8, 16)           72 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[16, 32)          38 |@@@@@@@@@@@@@@@@@@@@@@@@@               |
[32, 64)          12 |@@@@@@@@                                |
[64, 128)          3 |@@                                      |
[128, 256)         1 |                                        |

★ 每行是一个 2 的幂次桶（bucket）
  [8, 16) 表示 8-15 微秒的范围
  数字是落在该范围的计数
  @@@ 是可视化条形图
```

**lhist() 输出示例：**

```
@lat:
[0, 100)          15 |@@@@@@@@@@@@@@@                         |
[100, 200)        47 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[200, 300)        23 |@@@@@@@@@@@@@@@                         |
[300, 400)         8 |@@@@@                                   |
[400, 500)         2 |@                                       |

★ 线性分桶，适合已知范围的延迟分布
```

### 4.3 Map 操作函数

| 函数 | 说明 |
|------|------|
| `print(@map)` | ★ 打印 Map 内容（聚合函数会格式化） |
| `clear(@map)` | 清空 Map（重置所有聚合值） |
| `zero(@map)` | 归零 Map（保留 key，值归零） |
| `delete(@map, key)` | 删除特定 key |
| `len(@map)` | Map 中的元素数量 |

```bash
# 定期打印并清零
interval:s:5
{
    print(@syscalls);
    clear(@syscalls);          /* 清空，下一个 5 秒重新统计 */
}

# 对比 clear vs zero
# clear(@map)  → 删除所有 key-value 对
# zero(@map)   → 保留 key，value 设为 0
# 对于 hist()：clear 删除直方图，zero 保留桶但计数归零
```

### 4.4 字符串操作

| 函数 | 说明 |
|------|------|
| `str(ptr)` | ★ 将指针转为字符串（安全读取） |
| `str(ptr, len)` | 指定最大长度 |
| `buf(ptr, len)` | 将内存区域转为十六进制缓冲区 |
| `ksym(addr)` | 将内核地址转为符号名 |
| `usym(addr)` | 将用户态地址转为符号名 |
| `kaddr("name")` | 将内核符号名转为地址 |
| `uaddr("name")` | 将用户态符号名转为地址 |

```bash
# str() 示例：打印打开的文件名
kprobe:do_sys_openat2
{
    printf("open: %s\n", str(arg1));    /* arg1 是 const char *filename */
}

# ksym() 示例：将调用栈地址转为函数名
profile:hz:99
{
    @[kstack] = count();
}
/* 输出时自动使用 ksym 转换：
 *   bpf_trace_run1+0x1a
 *   __bpf_prog_enter+0x44
 *   vfs_read+0x88
 */
```

---

## 五、控制流

### 5.1 条件过滤

```bash
# 前置过滤（probe 后面 /.../)
kprobe:vfs_read
/pid == 1234/                   /* 只追踪 PID 1234 */
{
    @reads = count();
}

# 等价于 if 语句
kprobe:vfs_read
{
    if (pid == 1234) {
        @reads = count();
    }
}
```

### 5.2 if/else

```bash
tracepoint:syscalls:sys_exit_read
{
    if (args->ret >= 0) {
        @success_bytes = sum(args->ret);
    } else {
        @errors = count();
    }
}
```

### 5.3 三元运算符

```bash
kretprobe:vfs_read
{
    $status = retval >= 0 ? "ok" : "err";
    printf("read: %s (%d)\n", $status, retval);
}
```

### 5.4 循环（unroll）

```bash
# bpftrace 不支持任意循环（verifier 限制）
# 但支持 unroll（编译时展开）
kprobe:vfs_read
{
    unroll(5) {
        printf("iteration %d\n", $i);   /* 展开为 5 条 printf */
    }
}
```

---

## 六、bpftrace 编译流程（源码角度）

### 6.1 从 DSL 到 BPF 字节码

```
bpftrace -e 'kprobe:do_sys_openat2 { printf("%s\n", comm); }'
    │
    ▼
① Lexing（词法分析）
   │  src/bpftrace/src/lexer.l (flex)
   │  输入字符串 → Token 流
   │  "kprobe" → KPROBE
   │  ":"      → COLON
   │  "do_sys_openat2" → IDENT
   │
    ▼
② Parsing（语法分析）
   │  src/bpftrace/src/parser.yy (bison)
   │  Token 流 → AST
   │
   │  AST 结构：
   │  Program
   │  └── Probe
   │      ├── AttachPoint(kprobe, do_sys_openat2)
   │      └── Block
   │          └── Call("printf", [
   │                FormatString("%s\n"),
   │                Builtin("comm")
   │              ])
   │
    ▼
③ Semantic Analysis（语义分析）
   │  src/bpftrace/src/ast/passes/semantic_analyser.cpp
   │  ├── 检查 probe 类型是否合法
   │  ├── 检查变量类型匹配
   │  ├── 检查 comm/argN 在 kprobe 中可用
   │  └── 推断 @ 的 Map 类型（这里不需要聚合 Map）
   │
    ▼
④ Resource Analysis（资源分析）
   │  src/bpftrace/src/ast/passes/resource_analyser.cpp
   │  ├── 计算需要的 Map 数量和类型
   │  ├── 计算栈空间使用
   │  └── 检查 BPF 指令数限制
   │
    ▼
⑤ Codegen（代码生成）
   │  src/bpftrace/src/ast/passes/codegen_llvm.cpp
   │  └── src/bpftrace/src/ast/irbuilderbpf.cpp
   │
   │  生成 LLVM IR：
   │  define i64 @kprobe__do_sys_openat2_1(ptr %ctx) {
   │    ; 获取 comm
   │    %comm = alloca [16 x i8]
   │    call i64 @bpf_get_current_comm(ptr %comm, i64 16)
   │    ; printf
   │    call i64 @bpf_trace_printk(ptr %fmt, i64 4, ptr %comm)
   │    ret i64 0
   │  }
   │
   │  LLVM 优化 → BPF 后端 → BPF 字节码
   │
    ▼
⑥ Loading（加载）
   │  bpf(BPF_PROG_LOAD, ...)
   │  Verifier 检查 → 返回 prog_fd
   │
    ▼
⑦ Attaching（附着）
   │  src/bpftrace/src/attached_probe.cpp
   │  ├── perf_event_open(kprobe, do_sys_openat2)
   │  └── ioctl(perf_fd, PERF_EVENT_IOC_SET_BPF, prog_fd)
   │
    ▼
⑧ Event Loop（事件循环）
   │  poll(perf_buffer) → 读取输出 → 打印到终端
   │
   └── 用户按 Ctrl+C → END probe → 打印汇总 → 退出
```

### 6.2 attached_probe.cpp 关键逻辑

```cpp
/* src/bpftrace/src/attached_probe.cpp（简化） */

Result<std::unique_ptr<AttachedProbe>> AttachedProbe::make(
    Probe &probe, const BpfProgram &prog, ...)
{
    switch (probe.type) {
    case ProbeType::kprobe:
        // 1. 通过 /sys/kernel/debug/tracing/kprobe_events
        //    或 perf_event_open + BPF_LINK_CREATE 注册 kprobe
        return attach_kprobe(probe, prog);

    case ProbeType::tracepoint:
        // 1. perf_event_open() with tracepoint type
        // 2. ioctl(PERF_EVENT_IOC_SET_BPF, prog_fd)
        return attach_tracepoint(probe, prog);

    case ProbeType::uprobe:
        // 1. 解析 ELF 找到函数偏移
        // 2. perf_event_open() with uprobe type
        return attach_uprobe(probe, prog);

    case ProbeType::fentry:
    case ProbeType::fexit:
        // 1. 通过 BTF 找到函数 ID
        // 2. bpf_link_create(TRACING)
        return attach_fentry_fexit(probe, prog);

    case ProbeType::profile:
        // 1. perf_event_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK)
        return attach_profile(probe, prog);

    // ... 其他类型
    }
}
```

---

## 七、bpftrace 限制

### 7.1 语言限制

| 限制 | 原因 | 替代方案 |
|------|------|---------|
| 无任意循环 | Verifier 限制 | `unroll(N)` 编译时展开 |
| 无动态内存分配 | BPF 栈仅 512 字节 | 使用 Map 存储 |
| 字符串长度限制 | 最大 64 字节（默认） | `str(ptr, len)` 指定长度 |
| 无浮点数 | BPF 不支持浮点运算 | 用整数乘除模拟 |
| 无嵌套 Map | BPF Map 不支持嵌套 | 用复合 key |
| Map 大小固定 | 创建时指定 max_entries | 合理预估 |

### 7.2 探针限制

| 限制 | 说明 |
|------|------|
| kprobe 不能 hook inline 函数 | kprobe 只能 hook 导出的全局函数 |
| kretprobe 有嵌套限制 | 默认 maxactive = 系统参数 |
| uprobe 需要符号存在 | stripped binary 可能无法解析 |
| tracepoint ABI 稳定 | 但内核版本间可能新增/删除 |
| fentry/fexit 需要 BTF | 内核必须启用 CONFIG_DEBUG_INFO_BTF |

---

## 总结

### bpftrace 核心语法速记

```
probe 类型:附着点 /过滤/ { action; }

常用 probe：  kprobe: / kretprobe: / tracepoint: / profile: / interval:
常用变量：    comm / pid / tid / nsecs / arg0-argN / args->field / retval
常用函数：    printf / str / count / sum / avg / hist / lhist
常用 Map 操作： print / clear / zero / delete
特殊 probe： BEGIN / END
```

### 从 bpftrace 到 libbpf 的映射

| bpftrace 概念 | libbpf 对应 |
|--------------|------------|
| `@counts[comm] = count()` | BPF_MAP_TYPE_HASH + 用户态聚合 |
| `@lat = hist($dur)` | BPF_MAP_TYPE_ARRAY（直方图桶） |
| `printf(...)` | bpf_trace_printk() 或 ringbuf |
| `kprobe:func` | SEC("kprobe/func") + attach |
| `tracepoint:sub:event` | SEC("tracepoint/sub/event") + attach |
| `profile:hz:99` | SEC("perf_event") + perf_event_open |
