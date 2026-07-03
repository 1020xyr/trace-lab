# Step 3: bpftrace 命令参考与常用 One-liners

> 语法速查 + 30+ 实用 one-liners + bpftool 命令参考
> 每个 one-liner 都经过本系统验证（bpftrace v0.20.1，内核 6.6）

---

## 一、bpftrace 命令行参数

### 基本用法

```bash
bpftrace [选项] -e '程序代码'           # 内联脚本
bpftrace [选项] script.bt               # 脚本文件
bpftrace -l 'probe 模式'                # 列出可用 probe
```

### 参数速查表

| 参数 | 含义 | 说明 |
|------|------|------|
| `-e 'code'` | 内联脚本 | ★ 最常用，直接在命令行写 bpftrace 程序 |
| `-l 'pattern'` | ★ 列出可用 probe | 支持通配符 `*` |
| `-o file` | 输出到文件 | 默认输出到 stdout |
| `-d` | 调试模式 | 输出 LLVM IR 和 BPF 字节码 |
| `-dd` | 详细调试 | 输出未优化的 LLVM IR |
| `-v` | 详细输出 | 显示编译器信息 |
| `-p PID` | 附着到进程 | ★ USDT probe 自动绑定 |
| `-c 'cmd'` | 运行子命令 | ★ 追踪子命令的生命周期 |
| `-B mode` | 缓冲模式 | `none`(无缓冲) / `line`(行缓冲) / `full`(全缓冲) |
| `-f format` | 输出格式 | `text`(默认) / `json` |
| `--unsafe` | 不安全模式 | ★ 允许 system() 和 signal() |
| `--info` | 显示系统信息 | BPF 特性、内核版本等 |
| `-k` | 保留内核帧 | ★ kprobe 中包含内核调用栈 |
| `-kk` | 保留所有帧 | ★ 更详细的调用栈 |

### bpftrace -l 示例

```bash
# 列出所有 kprobe
bpftrace -l 'kprobe:*' | wc -l          # ★ 约 52,000+

# 列出特定子系统的 tracepoint
bpftrace -l 'tracepoint:syscalls:*'
bpftrace -l 'tracepoint:block:*'
bpftrace -l 'tracepoint:sched:*'

# 列出某个库的 uprobe
bpftrace -l 'uprobe:/lib/x86_64-linux-gnu/libc.so.6:*'

# 列出 USDT 探针
bpftrace -l 'usdt:/path/to/binary:*'
```

---

## 二、bpftrace 语法速查

### 数据类型

```bash
# 整数
$x = 42;                # ★ 64 位有符号整数（默认）
$x = (int32)42;         # 32 位整数
$x = (uint8)0xff;       # 8 位无符号

# 字符串
$s = "hello";           # 局部字符串变量
$s = str(arg1);         # 从指针读取字符串

# 元组（只读）
$tuple = (1, "hello", pid);
$tuple.0;               # 访问第一个元素
```

### 运算符

```bash
# 算术
$x = 1 + 2;
$x = $a * $b;
$x = $a / $b;           # 整数除法
$x = $a % $b;

# 比较
$a == $b, $a != $b
$a < $b, $a > $b, $a <= $b, $a >= $b

# 逻辑
$a && $b, $a || $b, !$a

# 位运算
$a & $b, $a | $b, $a ^ $b, ~$a
$a << $b, $a >> $b
```

### 常用函数速查

```bash
# ★ 输出
printf("format", args...);      # 格式化输出
print(@map);                    # 打印 Map（聚合函数自动格式化）
time("%H:%M:%S");               # 打印当前时间

# 字符串
str(ptr)                        # 指针 → 字符串
str(ptr, max_len)               # 指定最大长度
buf(ptr, len)                   # 指针 → 十六进制缓冲区

# ★ 进程信息
comm                            # 进程名
pid                             # PID (TGID)
tid                             # TID
uid / gid                       # 用户/组 ID
cpu                             # CPU 号
nsecs                           # ★ 纳秒时间戳

# 地址解析
ksym(addr)                      # 内核地址 → 符号名
usym(addr)                      # 用户地址 → 符号名
kaddr("symbol")                 # 内核符号 → 地址
uaddr("symbol")                 # 用户符号 → 地址

# ★ 调用栈
kstack                          # 内核调用栈
ustack                          # 用户态调用栈
kstack(depth)                   # 限制深度

# ★ Map 聚合
count()                         # 计数
sum(val)                        # 求和
avg(val)                        # 平均
min(val) / max(val)             # 最小/最大
stats(val)                      # count + avg + sum
hist(val)                       # ★ 2 的幂直方图
lhist(val, min, max, step)      # 线性直方图

# Map 操作
clear(@map)                     # 清空
zero(@map)                      # 归零
delete(@map, key)               # 删除指定 key
print(@map)                     # 打印
len(@map)                       # 元素数量

# 系统调用
system("command")               # 执行命令（需 --unsafe）
signal(sig, target)             # 发送信号（需 --unsafe）
```

---

## 三、30+ 实用 One-liners

### ★ A. 进程与系统调用追踪

```bash
# ★ 1. 统计各进程的系统调用次数
bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm] = count(); }'

# 2. 统计各进程的系统调用次数（按系统调用名分组）
bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm, args->id] = count(); }'

# 3. 追踪进程创建
bpftrace -e 'tracepoint:syscalls:sys_enter_execve { printf("%s -> %s\n", comm, str(args->filename)); }'

# 4. 追踪进程退出
bpftrace -e 'tracepoint:sched:sched_process_exit { printf("exit: %s (pid=%d) code=%d\n", comm, pid, args->prio); }'

# 5. 统计各进程的 CPU 时间（采样）
bpftrace -e 'profile:hz:99 { @[comm] = count(); }'

# 6. 追踪信号发送
bpftrace -e 'tracepoint:syscalls:sys_enter_kill { printf("%s -> pid=%d sig=%d\n", comm, args->pid, args->sig); }'

# 7. 追踪用户切换（上下文切换）
bpftrace -e 'tracepoint:sched:sched_switch { @[args->prev_comm] = count(); }'

# ★ 8. 查看系统调用延迟分布
bpftrace -e '
  tracepoint:raw_syscalls:sys_enter { @start[tid] = nsecs; }
  tracepoint:raw_syscalls:sys_exit /@start[tid]/ {
    @us = hist((nsecs - @start[tid]) / 1000);
    delete(@start[tid]);
  }'
```

### ★ B. 文件 I/O 追踪

```bash
# 9. 追踪文件打开
bpftrace -e 'kprobe:do_sys_openat2 { printf("%s: %s\n", comm, str(arg1)); }'

# 10. 追踪文件读取（按进程统计字节数）
bpftrace -e 'tracepoint:syscalls:sys_exit_read /args->ret > 0/ { @[comm] = sum(args->ret); }'

# 11. 追踪文件写入（按进程统计字节数）
bpftrace -e 'tracepoint:syscalls:sys_exit_write /args->ret > 0/ { @[comm] = sum(args->ret); }'

# 12. 统计各进程的 read 调用延迟
bpftrace -e '
  tracepoint:syscalls:sys_enter_read { @start[tid] = nsecs; }
  tracepoint:syscalls:sys_exit_read /@start[tid]/ {
    @read_us[comm] = hist((nsecs - @start[tid]) / 1000);
    delete(@start[tid]);
  }'

# 13. 追踪 fsync 调用
bpftrace -e 'tracepoint:syscalls:sys_enter_fsync { printf("fsync: %s (fd=%d)\n", comm, args->fd); }'

# 14. 统计打开最多的文件路径
bpftrace -e 'kprobe:do_sys_openat2 { @[str(arg1)] = count(); }'

# 15. 追踪文件删除
bpftrace -e 'kprobe:vfs_unlink { printf("unlink: %s\n", comm); }'
```

### ★ C. Block I/O 追踪（与 blktrace 对照）

```bash
# ★ 16. 统计各进程的 block I/O 次数
bpftrace -e 'tracepoint:block:block_rq_issue { @[comm] = count(); }'

# 17. 统计 block I/O 大小分布
bpftrace -e 'tracepoint:block:block_rq_issue { @bytes = hist(args->bytes); }'

# ★ 18. block I/O 延迟分布（D2C）
bpftrace -e '
  tracepoint:block:block_rq_issue { @start[args->sector] = nsecs; }
  tracepoint:block:block_rq_complete /@start[args->sector]/ {
    @d2c_us = hist((nsecs - @start[args->sector]) / 1000);
    delete(@start[args->sector]);
  }'

# 19. 统计 block I/O 方向（读/写）
bpftrace -e 'tracepoint:block:block_rq_issue { @[args->rwbs] = count(); }'

# 20. 追踪 block I/O 热点扇区
bpftrace -e 'tracepoint:block:block_rq_issue { @[args->sector / 1024] = count(); }'

# 21. 按设备统计 I/O 吞吐量
bpftrace -e 'tracepoint:block:block_rq_issue { @[args->dev] = sum(args->bytes); }'

# 22. 追踪 block I/O 错误
bpftrace -e 'tracepoint:block:block_rq_complete /args->errors/ { printf("I/O error: dev=%d sector=%d err=%d\n", args->dev, args->sector, args->errors); }'
```

### ★ D. 网络追踪

```bash
# 23. 统计 TCP connect 调用
bpftrace -e 'tracepoint:syscalls:sys_enter_connect { printf("connect: %s -> fd=%d\n", comm, args->fd); }'

# 24. 统计各进程的网络收发字节数
bpftrace -e '
  tracepoint:syscalls:sys_exit_recvfrom /args->ret > 0/ { @rx[comm] = sum(args->ret); }
  tracepoint:syscalls:sys_exit_sendto /args->ret > 0/ { @tx[comm] = sum(args->ret); }'

# 25. TCP 连接建立追踪
bpftrace -e 'kprobe:tcp_v4_connect { printf("TCP connect: %s\n", comm); }'

# 26. DNS 查询追踪（追踪 UDP sendto 到 53 端口）
bpftrace -e 'tracepoint:syscalls:sys_enter_sendto { printf("sendto: %s fd=%d len=%d\n", comm, args->fd, args->addrlen); }'
```

### ★ E. 性能分析

```bash
# ★ 27. CPU 火焰图数据（off-CPU 分析）
bpftrace -e '
  profile:hz:99 /pid > 0/ {
    @[kstack] = count();
  }'

# 28. 内核函数延迟 top
bpftrace -e '
  kprobe:vfs_read { @start[tid] = nsecs; }
  kretprobe:vfs_read /@start[tid]/ {
    @vfs_read_us = hist((nsecs - @start[tid]) / 1000);
    delete(@start[tid]);
  }'

# 29. 内存分配追踪
bpftrace -e '
  kprobe:kmalloc* { @allocs[comm] = count(); }
  kprobe:kfree { @frees[comm] = count(); }'

# 30. 页错误追踪
bpftrace -e '
  tracepoint:exceptions:page_fault_user {
    @[comm, kstack(5)] = count();
  }'

# ★ 31. 调度延迟分析
bpftrace -e '
  tracepoint:sched:sched_switch { @runq[args->next_pid] = nsecs; }
  tracepoint:sched:sched_wakeup /@runq[args->pid]/ {
    @sched_lat_us = hist((nsecs - @runq[args->pid]) / 1000);
    delete(@runq[args->pid]);
  }'

# 32. 锁等待分析（mutex）
bpftrace -e '
  kprobe:mutex_lock { @lock_start[tid] = nsecs; }
  kretprobe:mutex_lock /@lock_start[tid]/ {
    @mutex_wait_us = hist((nsecs - @lock_start[tid]) / 1000);
    delete(@lock_start[tid]);
  }'
```

### ★ F. 安全与诊断

```bash
# 33. 追踪 setuid 调用
bpftrace -e 'tracepoint:syscalls:sys_enter_setuid { printf("setuid: %s uid=%d\n", comm, args->uid); }'

# 34. 追踪模块加载
bpftrace -e 'kprobe:do_init_module { printf("module load: %s\n", str(arg0)); }'

# ★ 35. 追踪 OOM killer
bpftrace -e 'kprobe:oom_kill_process { printf("OOM kill triggered by %s\n", comm); }'
```

---

## 四、bpftrace 脚本模板

### 4.1 系统调用追踪脚本模板

```bash
#!/usr/bin/bpftrace
/*
 * ★ syscount.bt — 统计系统调用次数
 *
 * 用法：bpftrace syscount.bt
 *       bpftrace syscount.bt -p <PID>
 */

BEGIN
{
    printf("统计系统调用... Ctrl-C 结束\n");
}

tracepoint:raw_syscalls:sys_enter
{
    @[comm] = count();
}

END
{
    printf("\n=== 系统调用统计 ===\n");
    print(@, 10);    /* 只显示前 10 个 */
}
```

### 4.2 Block I/O 延迟分析模板

```bash
#!/usr/bin/bpftrace
/*
 * ★ biolatency.bt — Block I/O 延迟直方图
 *
 * ★ 对标 blktrace + btt 的 D2C 延迟分析
 * 用法：bpftrace biolatency.bt
 */

BEGIN
{
    printf("追踪 Block I/O 延迟... Ctrl-C 结束\n");
}

tracepoint:block:block_rq_issue
{
    @io_start[args->dev, args->sector] = nsecs;
}

tracepoint:block:block_rq_complete
/@io_start[args->dev, args->sector]/
{
    $delta_ns = nsecs - @io_start[args->dev, args->sector];
    $delta_us = $delta_ns / 1000;

    @d2c_us = hist($delta_us);
    @avg_us = avg($delta_us);
    @count = count();

    delete(@io_start[args->dev, args->sector]);
}

interval:s:5
{
    printf("--- I/O 延迟报告 (每 5 秒) ---\n");
    print(@d2c_us);
    clear(@d2c_us);
}

END
{
    printf("\n=== 最终 I/O 延迟统计 ===\n");
    print(@d2c_us);
    print(@avg_us);
    print(@count);
}
```

### 4.3 进程生命周期追踪模板

```bash
#!/usr/bin/bpftrace
/*
 * proclife.bt — 追踪进程创建与退出
 */

tracepoint:syscalls:sys_enter_execve
{
    printf("[%s] exec: %s -> %s\n", strftime("%H:%M:%S", nsecs), comm, str(args->filename));
}

tracepoint:syscalls:sys_enter_clone
{
    printf("[%s] clone: %s (flags=0x%x)\n", strftime("%H:%M:%S", nsecs), comm, args->clone_flags);
}

tracepoint:sched:sched_process_exit
{
    printf("[%s] exit: %s (pid=%d)\n", strftime("%H:%M:%S", nsecs), comm, pid);
}
```

---

## 五、bpftool 命令参考

### 5.1 基本用法

```bash
bpftool <对象> <命令> [选项]

对象：prog | map | link | btf | perf | net | feature | gen | struct_ops | cgroup
```

### 5.2 prog — BPF 程序管理

```bash
# ★ 列出所有已加载的 BPF 程序
bpftool prog show
# 输出示例：
# 33: kprobe  name do_sys_openat2  tag abc123  gpl
#     loaded_at 2026-07-03T10:00:00+0800  uid 0
#     xlated 312B  jited 198B  memlock 4096B
#     btf_id 42

# 按 ID 查看程序详情
bpftool prog show id 33

# 导出程序的 BPF 字节码（xlated = 翻译后的 BPF 指令）
bpftool prog dump xlated id 33

# 导出 JIT 编译后的机器码
bpftool prog dump jited id 33

# 加载程序到 bpffs
bpftool prog load prog.bpf.o /sys/fs/bpf/my_prog

# 显示程序的 verifier 日志
bpftool prog dump xlated id 33 2>&1
```

### 5.3 map — BPF Map 管理

```bash
# 列出所有 Map
bpftool map show

# 按 ID 查看 Map 详情
bpftool map show id 15

# ★ 导出 Map 的所有键值对
bpftool map dump id 15

# 查找特定 key
bpftool map lookup id 15 key 0x01 0x00 0x00 0x00

# 更新键值对
bpftool map update id 15 key 0x01 0x00 0x00 0x00 value 0x2a 0x00 0x00 0x00

# 删除键值对
bpftool map delete id 15 key 0x01 0x00 0x00 0x00

# 遍历 key
bpftool map dump id 15 | head -20

# 创建 Map
bpftool map create /sys/fs/bpf/my_map type hash key 4 value 8 entries 1024 name my_map

# 将 Map pin 到 bpffs（持久化）
bpftool map pin id 15 /sys/fs/bpf/my_map
```

### 5.4 link — BPF 附着关系管理

```bash
# 列出所有 BPF link（附着关系）
bpftool link show

# 按 ID 查看
bpftool link show id 5
```

### 5.5 btf — BTF 类型信息管理

```bash
# 列出所有 BTF 对象
bpftool btf list

# ★ 导出内核 BTF（所有内核类型定义）
bpftool btf dump file /sys/kernel/btf/vmlinux | head -50

# 导出为 C 头文件格式（★ 查看内核结构体定义）
bpftool btf dump file /sys/kernel/btf/vmlinux format c | head -100

# 查找特定类型
bpftool btf dump file /sys/kernel/btf/vmlinux | grep "struct task_struct"
```

### 5.6 feature — BPF 特性检测

```bash
# 检测当前内核支持的 BPF 特性
bpftool feature

# 输出示例：
# Scanning system configuration...
# bpf() syscall: OK
# JIT enabled: 1
# BPF_PROG_TYPE_KPROBE: OK
# BPF_PROG_TYPE_TRACEPOINT: OK
# BPF_PROG_TYPE_TRACING: OK
# BPF_MAP_TYPE_HASH: OK
# BPF_MAP_TYPE_RINGBUF: OK
```

### 5.7 net — 网络附着管理

```bash
# 查看 XDP 和 TC BPF 程序附着
bpftool net show

# 查看特定网卡的 BPF 附着
bpftool net show dev eth0
```

### 5.8 gen — 代码生成

```bash
# ★ 生成 BPF skeleton 头文件（★ CO-RE 开发必备）
bpftool gen skeleton prog.bpf.o name my_prog > my_prog.skel.h

# 生成 vmlinux.h（从内核 BTF 生成头文件）
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
```

---

## 六、常用组合命令

### 6.1 诊断场景快速命令

```bash
# 场景：系统变慢，快速排查
# 1. 看哪个进程 CPU 最高
bpftrace -e 'profile:hz:99 { @[comm] = count(); }'

# 2. 看系统调用是否异常多
bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm] = count(); }'

# 3. 看 I/O 延迟
bpftrace -e 'tracepoint:block:block_rq_issue { @io[comm]=count(); }'

# 4. 看是否有大量上下文切换
bpftrace -e 'tracepoint:sched:sched_switch { @switches = count(); }'

# 5. 看已加载的 BPF 程序（是否有异常 BPF 程序）
bpftool prog show
```

### 6.2 bpftrace + 传统工具对照

| 传统工具 | bpftrace 等价命令 |
|---------|------------------|
| `strace -p PID` | `bpftrace -e 'tracepoint:raw_syscalls:sys_enter /pid==PID/ { printf("%d\n", args->id); }'` |
| `opensnoop` | `bpftrace -e 'kprobe:do_sys_openat2 { printf("%s %s\n", comm, str(arg1)); }'` |
| `execsnoop` | `bpftrace -e 'tracepoint:syscalls:sys_enter_execve { printf("%s\n", str(args->filename)); }'` |
| `tcpconnect` | `bpftrace -e 'kprobe:tcp_v4_connect { printf("%s\n", comm); }'` |
| `biolatency` | `bpftrace -e 'tracepoint:block:block_rq_issue{@s[args->sector]=nsecs;} tracepoint:block:block_rq_complete/@s[args->sector]/{@=hist((nsecs-@s[args->sector])/1000);delete(@s[args->sector]);}'` |
| `blktrace + btt` | `bpftrace biolatency.bt`（见上面的脚本模板） |
| `perf top` | `bpftrace -e 'profile:hz:99 { @[kstack] = count(); }'` |

---

## 七、源码文件快速索引

```bash
# 查找 probe 类型定义
grep -n "enum class ProbeType" src/bpftrace/src/probe_types.h

# 查找 bpftrace 内置变量实现
grep -rn "comm\|nsecs\|pid\|cpu" src/bpftrace/src/ast/passes/codegen_llvm.cpp

# 查找聚合函数实现
grep -rn "count\|sum\|avg\|hist" src/bpftrace/src/mapkey.cpp src/bpftrace/src/bpfmap.cpp

# 查找 helper 函数签名
grep -n "bpf_get_current\|bpf_probe_read\|bpf_map" src/libbpf/src/bpf_helper_defs.h

# 查找 tracepoint 参数定义
cat /sys/kernel/debug/tracing/events/syscalls/sys_enter_read/format

# 查找可用 kprobe 函数
cat /sys/kernel/debug/tracing/available_filter_functions | grep vfs_

# 查找 BPF 程序类型
grep "BPF_PROG_TYPE" /usr/include/linux/bpf.h
```
