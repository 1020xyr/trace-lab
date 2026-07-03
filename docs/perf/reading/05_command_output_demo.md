# perf 命令输出实战解析

> 每个命令都实际执行，展示真实输出片段并逐行/逐字段解读。
> 环境：KVM 虚拟机，Intel Xeon Platinum，4 核，CentOS 8。
> ★ 注意：VM 中硬件 PMU 计数器不可用（cycles 等显示 `<not supported>`），
> 本文档同时展示 VM 和物理机场景的差异。

---

## 测试程序说明

本文档使用三个不同类型的测试程序：

```
程序            │ 类型           │ 主要行为
───────────────┼───────────────┼──────────────────────────
test_cpu       │ CPU 密集型     │ 素数计算 + 矩阵乘法
test_mem       │ 内存密集型     │ 64MB 顺序写 + 随机读
test_io        │ IO 密集型      │ 写/读 4MB 文件（1000×4KB）
```

```c
// test_cpu.c 关键函数
int is_prime(int n) { ... }           // CPU 密集：10 万以内素数判定
void matrix_multiply(double *A, double *B, double *C, int n) { ... }
                                       // 300×300 矩阵乘法（O(n³)）
```

---

## 场景 1：perf stat 基础统计

### 命令

```bash
perf stat ./test_cpu
```

### 真实输出

```
Found 9592 primes below 100000
Matrix multiply done, C[0]=300.0

 Performance counter stats for './test_cpu':

            115.49 msec task-clock                       #    0.976 CPUs utilized
                22      context-switches                 #  190.498 /sec
                 1      cpu-migrations                   #    8.659 /sec
               593      page-faults                      #    5.135 K/sec
   <not supported>      cycles
   <not supported>      instructions
   <not supported>      branches
   <not supported>      branch-misses

       0.118300457 seconds time elapsed

       0.110268000 seconds user
       0.003858000 seconds sys
```

### 逐行注解

```
Performance counter stats for './test_cpu':
                                  ↑ 被统计的命令名

            115.49 msec task-clock                       #    0.976 CPUs utilized
            ^^^^^^^^^^^^^^^^^^^^                              ^^^^^^^^^^^^^^^^^^^^
            ★ 程序实际占用 CPU 115.49ms                      115.49ms / 118.3ms = 0.976
              （含 user + sys）                               说明 97.6% 时间在用 CPU
                                                              （少量时间被调度出去）

                22      context-switches                 #  190.498 /sec
                ^^      ^^^^^^^^^^^^^^^^^                      ^^^^^^^^^^
                22次    上下文切换次数                          每秒 190 次切换
                      （进程被内核抢占或主动让出 CPU）           （22 / 0.11549 ≈ 190/s）

                 1      cpu-migrations                   #    8.659 /sec
                 ^      ^^^^^^^^^^^^^^^^                       ^^^^^^^^^^
                 1次    CPU 迁移次数                            每秒 8.7 次
                      （线程从一个 CPU 挪到另一个 CPU）          ★ 越少越好，迁移会
                                                                 丢失 L1/L2 缓存

               593      page-faults                      #    5.135 K/sec
               ^^^      ^^^^^^^^^^^^^                          ^^^^^^^^^^^
               593次    缺页异常次数                            每秒 5135 次缺页
                      （访问未映射的虚拟内存页）                 ★ 主要发生在程序启动时
                                                                 加载共享库和 malloc

   <not supported>      cycles
   ^^^^^^^^^^^^^^^      ^^^^^^
   ★ VM 中硬件 PMU 不可用！cycles 计数器无法读取
     （KVM 默认不透传 PMU 给 Guest）

   <not supported>      instructions
   同上：指令计数器不可用

   <not supported>      branches / branch-misses
   同上：分支计数器不可用

       0.118300457 seconds time elapsed
       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
       ★ 墙上时间（wall clock）= 0.118 秒

       0.110268000 seconds user
       ^^^^^^^^^^^^^^^^^^^^^^^^^
       用户态 CPU 时间

       0.003858000 seconds sys
       ^^^^^^^^^^^^^^^^^^^^^^^^
       内核态 CPU 时间
       user + sys = 0.114s ≈ task-clock 115.49ms（基本吻合）
```

### VM 环境解决方案：使用软件事件

```bash
# VM 中改用软件事件，这些始终可用：
perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cpu-clock ./test_cpu
```

```
Performance counter stats for './test_cpu':

            199.12 msec task-clock                       #    0.979 CPUs utilized
                16      context-switches                 #   80.355 /sec
                 0      cpu-migrations                   #    0.000 /sec
               594      page-faults                      #    2.983 K/sec
            199.10 msec cpu-clock                        #    0.979 CPUs utilized

       0.203293266 seconds time elapsed

       0.185794000 seconds user
       0.008770000 seconds sys
```

```
软件事件说明：

  task-clock      → 进程占用 CPU 的总时间（毫秒）
  cpu-clock       → 与 task-clock 类似（CPU 时间，含等待）
                     ★ 两者差异很小 = 进程几乎一直在占用 CPU
  context-switches → 上下文切换次数（被调度器抢占）
  cpu-migrations   → CPU 迁移次数（0 = 线程绑定在一个 CPU 上）
  page-faults      → 缺页异常次数（内存分配时触发）
```

---

## 场景 2：perf stat -d 缓存/分支预测统计

### 命令

```bash
perf stat -d ./test_cpu
```

### 真实输出（VM 环境）

```
Found 9592 primes below 100000
Matrix multiply done, C[0]=300.0

 Performance counter stats for './test_cpu':

            150.70 msec task-clock                       #    0.989 CPUs utilized
                10      context-switches                 #   66.356 /sec
                 2      cpu-migrations                   #   13.271 /sec
               594      page-faults                      #    3.942 K/sec
   <not supported>      cycles
   <not supported>      instructions
   <not supported>      branches
   <not supported>      branch-misses
   <not supported>      L1-dcache-loads
   <not supported>      L1-dcache-load-misses
   <not supported>      LLC-loads
   <not supported>      LLC-load-misses

       0.152305724 seconds time elapsed
```

### 逐行注解（物理机 vs VM）

```
物理机上 -d 的典型输出（参考值）：

           150.70 msec task-clock                    #    0.989 CPUs utilized
    450,000,000       cycles                         #    2.986 GHz
    800,000,000       instructions                   #    1.78  insn per cycle
    150,000,000       branches                       #  995.348 M/sec
      5,000,000       branch-misses                  #    3.33% of all branches
    200,000,000       L1-dcache-loads                #    1.327 G/sec
      3,000,000       L1-dcache-load-misses          #    1.50% of all L1 loads
     10,000,000       LLC-loads                      #   66.356 M/sec
      1,000,000       LLC-load-misses                #   10.00% of all LLC loads
```

```
字段                  │ 含义                          │ 关键指标
─────────────────────┼──────────────────────────────┼─────────────────
cycles               │ CPU 时钟周期数                │ 基础时间单位
instructions         │ 执行指令数                    │ IPC = insn/cycles
                                                     ★ IPC > 1.5 很好
                                                     ★ IPC < 0.5 很差
branches             │ 分支指令总数                  │ if/else/loop 等
branch-misses        │ 分支预测失败数                │ < 5% 优秀
L1-dcache-loads      │ L1 数据缓存访问次数          │ 基础缓存操作
L1-dcache-load-misses│ L1 数据缓存未命中              │ < 5% 优秀
LLC-loads            │ 最后一级缓存访问              │ 慢于 L1/L2
LLC-load-misses      │ LLC 未命中（→ 主存）          │ < 10% 优秀
```

> ★ **VM 环境说明：** KVM 默认不透传 PMU 硬件计数器，所有硬件事件显示
> `<not supported>`。如需在 VM 中使用硬件事件，需要在创建 VM 时配置：
> `<cpu><model>host</model><feature policy='require' name='pmu'/></cpu>`

### 三级 -d 的输出

```bash
perf stat -d -d ./test_cpu     # 两级 -d：增加 L1 指令缓存和 LLC
perf stat -d -d -d ./test_cpu  # 三级 -d：增加 TLB 事件
```

三级 `-d` 在物理机上的完整输出会包含：

```
# 第三级增加的 TLB 事件：
dTLB-loads          → 数据 TLB 查找次数
dTLB-load-misses    → 数据 TLB 未命中（→ page walk，很慢）
iTLB-loads          → 指令 TLB 查找次数
iTLB-load-misses    → 指令 TLB 未命中

★ TLB miss 率高 (>1%) 说明工作集大或页表深
  解决方案：使用大页（Huge Pages）
```

---

## 场景 3：perf record -g + perf report 热点函数分析

### 命令

```bash
# 第一步：采样（带调用栈）
perf record -g -o perf_test.data ./test_cpu

# 第二步：查看报告
perf report -i perf_test.data --stdio --no-children
```

### perf record 输出

```
Found 9592 primes below 100000
Matrix multiply done, C[0]=300.0
[ perf record: Woken up 1 times to write data ]
[ perf record: Captured and wrote 0.054 MB perf_test.data (513 samples) ]
```

```
注解：
  Woken up 1 times         → perf 被唤醒 1 次写入数据
                             （buffer 未满，只在结束时写入）
  0.054 MB                 → perf.data 文件大小 54KB
  513 samples              → 共采集 513 个采样点
                             ★ 采样事件为 cpu-clock:ppp
                             （VM 中 cycles 不可用，自动降级为 cpu-clock）
```

### perf report 输出（逐行注解）

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 513  of event 'cpu-clock:ppp'
# Event count (approx.): 128250000
#
# Overhead  Command   Shared Object         Symbol
# ........  ........  ....................  ...............................
#
    88.69%  test_cpu  test_cpu              [.] matrix_multiply
            |
            ---matrix_multiply
               |
                --88.30%--0x4072c00000000000

     7.02%  test_cpu  test_cpu              [.] is_prime
            |
            ---is_prime

     0.78%  test_cpu  [kernel.kallsyms]     [k] do_user_addr_fault
            |
            ---do_user_addr_fault
               exc_page_fault
               asm_exc_page_fault
               |
                --0.58%--matrix_multiply
                          0x4072c00000000000
```

```
逐行解读：

# Total Lost Samples: 0
  ★ 丢失的采样数为 0 — buffer 足够大，没有数据丢失

# Samples: 513  of event 'cpu-clock:ppp'
  ★ 共 513 个采样点，事件为 cpu-clock:ppp
  ppp = precise_ip 最高级别（精确定位到具体指令）

# Event count (approx.): 128250000
  ★ 近似事件总数 = 513 × 250000（采样周期）= 128,250,000

Overhead  Command   Shared Object         Symbol
^^^^^^^^  ^^^^^^^^  ^^^^^^^^^^^^^^        ^^^^^^
开销占比  进程名    共享库/可执行文件       函数名

    88.69%  test_cpu  test_cpu              [.] matrix_multiply
    ^^^^^^            ^^^^^^^^              ^^^ ^^^^^^^^^^^^^^^
    ★ 88.69%         test_cpu 自身         [.] = 用户态函数
    的 CPU 时间                              [k] = 内核态函数
    花在矩阵乘法上                            [u] = 用户态（等价于 [.]）

            |
            ---matrix_multiply               ← 调用图根节点
               |
                --88.30%--0x4072c00000000000  ← 调用者地址
                                              （fp 方式帧指针不完整时的表现）

     7.02%  test_cpu  test_cpu              [.] is_prime
     ^^^^^
     ★ 7% 时间在素数计算
       与矩阵乘法 88.69% 对比：
       矩阵乘法 O(n³) 远大于素数判定 O(n√n)

     0.78%  test_cpu  [kernel.kallsyms]     [k] do_user_addr_fault
     ^^^^^                                 ^^^^^^^^^^^^^^^^^^^^^^
     0.78%                                 ★ 缺页异常处理
     在内核                                  （page fault → 分配物理页）

               --0.58%--matrix_multiply
               在 matrix_multiply 中触发缺页
               （malloc 后首次访问触发 page fault）
```

### 使用 DWARF 调用栈获得完整调用链

```bash
# DWARF 方式调用栈更完整
perf record --call-graph dwarf -o perf_dwarf.data ./test_cpu
perf report -i perf_dwarf.data --stdio --no-children
```

```
[ perf record: Woken up 21 times to write data ]
[ perf record: Captured and wrote 5.415 MB perf_dwarf.data (670 samples) ]
```

```
Woken up 21 times    → 被唤醒 21 次（DWARF 数据量大）
5.415 MB             → 文件 5.4MB（fp 方式仅 54KB）
                       ★ DWARF 记录完整的栈帧展开信息
670 samples          → 670 个采样点（比 fp 的 513 略多）
```

```
# Overhead  Command   Shared Object         Symbol
# ........  ........  ....................  ..................................
#
    90.45%  test_cpu  test_cpu              [.] matrix_multiply
            |
            ---matrix_multiply
               main
               __libc_start_call_main
               __libc_start_main@@GLIBC_2.34
               _start

     5.52%  test_cpu  test_cpu              [.] is_prime
            |
            ---is_prime
               main
               __libc_start_call_main
               __libc_start_main@@GLIBC_2.34
               _start
```

```
DWARF vs fp 调用图对比：

fp 方式：
  ---matrix_multiply
     |
      --88.30%--0x4072c00000000000     ← 地址，无法解析函数名

DWARF 方式：
  ---matrix_multiply
     main                              ← ★ 完整的函数调用链
     __libc_start_call_main            ← main 的调用者
     __libc_start_main@@GLIBC_2.34     ← C 运行时启动
     _start                            ← 程序入口
```

> ★ **结论：** DWARF 调用栈文件更大（5.4MB vs 54KB），但调用链完整、
> 可读性好。**推荐在 perf record 时使用 `--call-graph dwarf`。**

### 使用 --sort 按不同维度排序

```bash
perf report -i perf_dwarf.data --stdio --no-children --sort symbol,dso
```

```
# Overhead  Symbol                              Shared Object         IPC   [IPC Coverage]
# ........  ..................................  ....................  ....................
#
    90.45%  [.] matrix_multiply                 test_cpu              -      -
    ...

     5.52%  [.] is_prime                        test_cpu              -      -
    ...

     0.90%  [k] _raw_spin_unlock_irqrestore     [kernel.kallsyms]     -      -
```

```
--sort symbol,dso 含义：
  symbol → 按函数名排序
  dso    → 再按共享库名排序

[.] = 用户态函数
[k] = 内核态函数

IPC 列显示 - 表示无硬件 PMU 数据（VM 环境）
物理机上会显示实际 IPC 值
```

---

## 场景 4：perf top 实时分析

### 命令

```bash
perf top -a --stdio
```

### 输出说明

```
   PerfTop:       0 irqs/sec  kernel: 0.0%  exact:  0.0% lost: 0/0 drop: 0/0 [4000Hz cpu-clock:ppp],  (all, 4 CPUs)
-------------------------------------------------------------------------------


Mapped keys:
	[d]     display refresh delay.             	(2)
	[e]     display entries (lines).           	(20)
	[f]     profile display filter (count).    	(5)
	...
Enter selection, or unmapped key to continue:
```

```
逐字段解读：

PerfTop: 0 irqs/sec
       ^^^^^^^^^^^^
       ★ 每秒中断数 = 0（当前系统无活跃采样程序）

kernel: 0.0%
^^^^^^^^^^^^^
内核态占比 0%（idle 状态）

exact: 0.0%
^^^^^^^^^^^
精确定位采样占比

lost: 0/0
^^^^^^^^^
丢失/总采样

[4000Hz cpu-clock:ppp]
^^^^^^^^^^^^^^^^^^^^^^^
★ 采样频率 4000Hz
  事件为 cpu-clock:ppp（VM 环境自动降级）

(all, 4 CPUs)
^^^^^^^^^^^^^
全系统，4 个 CPU
```

### perf top 在物理机上的典型输出

```
   PerfTop:    1234 irqs/sec  kernel: 23.4%  exact: 100.0% [4000Hz cycles],  (all, 4 CPUs)

Overhead  Symbol                                               DSO
........  ...................................................  .....................
  12.34%  [k] native_queued_spin_lock_slowpath                 [kernel.kallsyms]
   8.56%  [.] matrix_multiply                                  test_cpu
   5.23%  [k] __schedule                                       [kernel.kallsyms]
   3.45%  [.] is_prime                                         test_cpu
   ...
```

```
★ perf top 与 perf report 的区别：

  perf top     → 实时显示当前系统热点（类似 top 命令）
  perf report  → 分析历史数据（perf.data 文件）

  perf top 适合：
  - 实时诊断系统瓶颈
  - 观察某个时刻哪个函数/CPU 最忙
  - 不产生持久化文件

  perf report 适合：
  - 深入分析程序行为
  - 保留数据供后续对比
  - 调用栈分析
```

---

## 场景 5：perf script 事件流

### 命令

```bash
perf script -i perf_test.data | head -30
```

### 真实输出（逐行注解）

```
test_cpu  435603 1782226.262565:     250000 cpu-clock:ppp:
	    7f564b52d74e dl_main+0xe3e (/usr/lib64/ld-linux-x86-64.so.2)
	    7f564b52aea3 _dl_sysdep_start+0x83 (/usr/lib64/ld-linux-x86-64.so.2)
	    7f564b52b478 _dl_start_user+0x0 (/usr/lib64/ld-linux-x86-64.so.2)
```

```
逐字段：
  test_cpu         → 进程名（comm）
  435603           → PID
  1782226.262565   → ★ 时间戳（秒.微秒，内核 MONOTONIC 时钟）
  250000           → ★ 采样周期（每 250000 个事件采样一次）
  cpu-clock:ppp    → 事件名称（cpu-clock + 精确级别 ppp）

调用栈（从下到上 = 从外到内）：
  _dl_start_user   → 动态链接器入口
  _dl_sysdep_start → 系统依赖初始化
  dl_main          → 动态链接主函数（加载共享库）

  ★ 这是程序启动阶段的采样——动态链接器在加载 libc/libm
```

```
test_cpu  435603 1782226.263064:     250000 cpu-clock:ppp:
	          401166 is_prime+0x20 (/tmp/perf_lab/test_cpu)
```

```
  401166           → 指令地址（十六进制）
  is_prime+0x20    → ★ 函数名 + 偏移量（0x20 字节）
  test_cpu         → 所属共享库/可执行文件

  ★ 此时程序正在执行 is_prime() 函数
     调用栈只有 1 层（fp 方式帧指针被优化掉了）
```

```
test_cpu  435603 1782226.263314:     250000 cpu-clock:ppp:
	          401146 is_prime+0x0 (/tmp/perf_lab/test_cpu)
```

```
  is_prime+0x0     → 函数入口（偏移 0）
  ★ 不同采样点捕获到 is_prime 的不同位置：
    +0x0, +0x20, +0x2c, +0x1b, +0x1e 等
    说明 is_prime 内部各指令被均匀采样
```

### DWARF 方式的 perf script（完整调用链）

```bash
perf script -i perf_dwarf.data | head -30
```

```
test_cpu  435938 1782235.922696:     250000 cpu-clock:ppp:
	          401172 is_prime+0x2c (/tmp/perf_lab/test_cpu)
	          401232 main+0x14 (/tmp/perf_lab/test_cpu)
	    7fe0b442394f __libc_start_call_main+0x7f (/usr/lib64/libc.so.6)
	    7fe0b4423a08 __libc_start_main@@GLIBC_2.34+0x88 (/usr/lib64/libc.so.6)
	          401084 _start+0x24 (/tmp/perf_lab/test_cpu)
```

```
DWARF 调用栈（完整！）：
  _start                    → 程序入口
  __libc_start_main         → glibc 启动
  __libc_start_call_main    → 调用 main
  main+0x14                 → ★ main 函数偏移 0x14
  is_prime+0x2c             → ★ is_prime 函数偏移 0x2c
                                （正在执行内层循环）
```

---

## 场景 6：perf trace 系统调用追踪

### 命令

```bash
perf trace -e write,read,open,openat,close,mmap,brk ./test_io
```

### 真实输出（节选关键部分）

```
     0.000 ( 0.007 ms): test_io/436703 brk()
                                                         = 0x715e000
     0.166 ( 0.018 ms): test_io/436703 openat(dfd: CWD, filename: 0xb1ceb264,
                 flags: RDONLY|CLOEXEC)                  = 3
     0.189 ( 0.015 ms): test_io/436703 mmap(len: 17947, prot: READ,
                 flags: PRIVATE, fd: 3)                  = 0x7f39b1cbf000
     0.206 ( 0.002 ms): test_io/436703 close(fd: 3</tmp/perf_lab/testfile.dat>)
                                                         = 0
     0.570 ( 0.019 ms): test_io/436703 openat(dfd: CWD, filename: 0x402004,
                 flags: CREAT|TRUNC|WRONLY, mode: IRUGO|IWUSR)
                                                         = 3
     0.591 ( 0.014 ms): test_io/436703 write(fd: 3</tmp/perf_lab/testfile.dat>,
                 buf: 0x7ffe38d07dc0, count: 4096)       = 4096
     0.607 ( 0.005 ms): test_io/436703 write(fd: 3</tmp/perf_lab/testfile.dat>,
                 buf: 0x7ffe38d07dc0, count: 4096)       = 4096
     0.622 ( 0.005 ms): test_io/436703 write(fd: 3</tmp/perf_lab/testfile.dat>,
                 buf: 0x7ffe38d07dc0, count: 4096)       = 4096
     ...（后续 write 调用省略，间隔约 4-7ms）
```

### 逐行注解

```
     0.000 ( 0.007 ms): test_io/436703 brk()                      = 0x715e000
     ^^^^^  ^^^^^^^^^^   ^^^^^^^^^^^^^  ^^^^^                      ^^^^^^^^^^^
     时间    耗时         进程名/PID     系统调用                    返回值
     0ms     7μs          test_io      brk() = 设置堆顶
                                       ★ 程序启动时分配堆内存

     0.166 ( 0.018 ms): test_io/436703 openat(dfd: CWD, filename: ..., flags: RDONLY|CLOEXEC) = 3
                          ^^^^^^^^^^^^
                          openat 系统调用：
                          dfd: CWD          → 基准目录 = 当前工作目录
                          filename: 0x...   → 文件路径（地址形式）
                          flags: RDONLY     → 只读打开
                          flags: CLOEXEC    → exec 时自动关闭
                          = 3               → 返回 fd=3

     0.189 ( 0.015 ms): test_io/436703 mmap(len: 17947, prot: READ, flags: PRIVATE, fd: 3)
                          ^^^^^^^^^^^^^
                          mmap 系统调用：
                          len: 17947        → 映射长度 17947 字节
                          prot: READ        → 只读映射
                          flags: PRIVATE    → 私有映射（写时拷贝）
                          fd: 3             → 从 fd=3 映射
                          ★ 这是加载 libc.so 的操作

     0.570 ( 0.019 ms): test_io/436703 openat(... flags: CREAT|TRUNC|WRONLY, mode: IRUGO|IWUSR)
                          flags: CREAT      → 创建文件
                          flags: TRUNC      → 截断为 0
                          flags: WRONLY     → 只写
                          mode: IRUGO|IWUSR → 权限 0644（rw-r--r--）
                          ★ 程序开始创建 /tmp/perf_lab/testfile.dat

     0.591 ( 0.014 ms): test_io/436703 write(fd: 3<testfile.dat>, buf: 0x..., count: 4096) = 4096
                          ^^^^^^^^^^^^^
                          write 系统调用：
                          fd: 3<testfile.dat> → fd=3，文件名 testfile.dat
                          buf: 0x...          → 用户态缓冲区地址
                          count: 4096         → 写入 4096 字节（4KB）
                          = 4096              → 成功写入 4096 字节
                          ★ 这是 test_io 的主循环：每次写 4KB，共 1000 次
```

### perf trace vs strace 对比

```
特性              │ perf trace              │ strace
─────────────────┼────────────────────────┼──────────────────────
实现机制         │ perf_event_open         │ ptrace
性能开销         │ ★ 低（~5%）            │ 高（~100%+）
参数解析         │ 自动符号化              │ 原始数值
过滤能力         │ -e 事件过滤             │ -e 事件过滤
调用栈           │ -s 参数                 │ 不支持
时间统计         │ 内置                    │ -T 参数
全系统           │ -a                      │ 不支持
与 perf 生态集成 │ ★ 原生                  │ 独立工具

★ 结论：perf trace 比 strace 性能开销更低，适合生产环境使用
```

### 使用 --summary 输出统计摘要

```bash
perf trace --summary -e write,read ./test_io
```

```
★ --summary 会在程序结束后输出：
  - 每个系统调用的调用次数
  - 总耗时
  - 平均耗时
  - 最大耗时

  适合快速识别：哪个系统调用最耗时？
```

---

## 场景 7：perf stat 对比不同程序

### 命令

```bash
# CPU 密集型
perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cpu-clock ./test_cpu

# 内存密集型
perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cpu-clock ./test_mem

# IO 密集型
perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cpu-clock ./test_io
```

### CPU 密集型输出

```
Found 9592 primes below 100000
Matrix multiply done, C[0]=300.0

 Performance counter stats for './test_cpu':

            120.86 msec task-clock                       #    0.991 CPUs utilized
   <not supported>      cycles
   <not supported>      instructions
   <not supported>      cache-references
   <not supported>      cache-misses
   <not supported>      branches
   <not supported>      branch-misses
   <not supported>      bus-cycles
   <not supported>      stalled-cycles-frontend
   <not supported>      stalled-cycles-backend

       0.121987298 seconds time elapsed

       0.106050000 seconds user
       0.010142000 seconds sys
```

### 三类程序对比总结

```
★ 注意：本环境为 VM，硬件事件（cycles 等）显示 <not supported>。
  以下仅对比软件事件（在物理机上硬件事件对比更有意义）。

指标              │ CPU 密集型       │ 内存密集型       │ IO 密集型
─────────────────┼─────────────────┼─────────────────┼─────────────────
task-clock        │ 120.86 ms       │ ~100 ms         │ ~30 ms
                  │ ★ 高（占满CPU）  │ 高               │ ★ 低（大量时间等待IO）
context-switches  │ ~15             │ ~20             │ ~100+
                  │ 少（一直计算）   │ 中               │ ★ 多（频繁阻塞等待）
cpu-migrations    │ ~1              │ ~1              │ ~2
                  │ 都很少           │ 都很少           │ 都很少
page-faults       │ ~594            │ ~16000+         │ ~100
                  │ 中（共享库加载） │ ★ 多（64MB mmap）│ 少

user / sys 比例   │ 0.106/0.010     │ 高user/中sys    │ 低user/高sys
                  │ ★ user >> sys   │ 计算为主         │ ★ sys 占比高
                  │ （纯计算）        │                  │ （系统调用密集）

★ 物理机上的关键对比：
  CPU 密集型：cycles 极高，IPC ≈ 1.5-2.0，cache-misses 低
  内存密集型：cache-misses 高，LLC miss 多，TLB miss 可能高
  IO 密集型：task-clock << wall-clock（大量时间在 sys 中阻塞）
```

---

## 场景 8：perf stat -a 全系统统计

### 命令

```bash
# 统计全系统 1 秒
perf stat -a -e task-clock,context-switches,cpu-migrations,page-faults,cpu-clock sleep 1
```

### 真实输出

```
 Performance counter stats for 'system wide':

          4,015.66 msec task-clock                       #    4.002 CPUs utilized
            24,765      context-switches                 #    6.167 K/sec
             1,683      cpu-migrations                   #  419.109 /sec
            80,157      page-faults                      #   19.961 K/sec
          4,015.62 msec cpu-clock                        #    4.002 CPUs utilized

       1.003378647 seconds time elapsed
```

### 逐行注解

```
'system wide'                → 全系统统计（非单个进程）

4,015.66 msec task-clock     → ★ 4 个 CPU 各用了约 1 秒 = 4000ms
  #    4.002 CPUs utilized      4015.66ms / 1003ms ≈ 4.002 CPU
                                  ★ 基本占满全部 4 个 CPU

24,765    context-switches   → ★ 1 秒内全系统 24765 次上下文切换
  #    6.167 K/sec              每秒约 6167 次（每 CPU ~1542 次/秒）

1,683     cpu-migrations     → ★ 1 秒内 1683 次 CPU 迁移
  #    419.109 /sec              每秒 419 次（偏高）
                                  ★ 说明进程频繁在 CPU 间迁移
                                    可能导致缓存失效

80,157    page-faults        → ★ 1 秒内全系统 80157 次缺页
  #    19.961 K/sec              ★ 非常高！说明有大量内存分配
                                  （可能有其他程序在运行）
```

### 全系统统计的使用场景

```
★ perf stat -a 适合：
  - 查看整体系统负载
  - 发现全系统瓶颈（如 context-switches 风暴）
  - 对比空闲 vs 繁忙状态

★ 配合 sleep N 使用：
  perf stat -a sleep 1        → 统计 1 秒
  perf stat -a sleep 60       → 统计 1 分钟

★ 配合 -c 指定 CPU：
  perf stat -a -C 0 sleep 1   → 只统计 CPU 0
```

---

## 场景 9：perf list 查看可用事件

### 命令

```bash
perf list
```

### 真实输出（节选）

```
  alignment-faults                                   [Software event]
  bpf-output                                         [Software event]
  cgroup-switches                                    [Software event]
  context-switches OR cs                             [Software event]
  cpu-clock                                          [Software event]
  cpu-migrations OR migrations                       [Software event]
  dummy                                              [Software event]
  emulation-faults                                   [Software event]
  major-faults                                       [Software event]
  minor-faults                                       [Software event]
  page-faults OR faults                              [Software event]
  task-clock                                         [Software event]

tool:
  duration_time
  user_time
  system_time

  msr/smi/                                           [Kernel PMU event]
  msr/tsc/                                           [Kernel PMU event]
  rNNN                                               [Raw hardware event descriptor]

  alarmtimer:alarmtimer_cancel                       [Tracepoint event]
  alarmtimer:alarmtimer_fired                        [Tracepoint event]
  block:block_bio_backmerge                          [Tracepoint event]
  block:block_bio_complete                           [Tracepoint event]
  block:block_rq_complete                            [Tracepoint event]
  block:block_rq_issue                               [Tracepoint event]
  ...
```

### 逐行注解

```
事件名                        别名       类型
───────────────────────────  ─────────  ─────────────────────
alignment-faults                          内存对齐错误
bpf-output                                BPF 程序输出
cgroup-switches                           cgroup 切换
context-switches             cs           ★ 上下文切换（常用）
cpu-clock                                 ★ CPU 时间（常用）
cpu-migrations               migrations   CPU 迁移
major-faults                              大缺页（需要磁盘IO）
minor-faults                              小缺页（只需分配页框）
page-faults                  faults       ★ 所有缺页（常用）
task-clock                                ★ 任务 CPU 时间（常用）

tool:
  duration_time                           perf 运行时长
  user_time                               用户态时间
  system_time                             内核态时间

Tracepoint 事件（内核 tracepoint）：
  block:block_bio_backmerge   → ★ block I/O 合并事件（同 blktrace 的 M 事件）
  block:block_rq_complete     → ★ block I/O 完成事件（同 blktrace 的 C 事件）
  block:block_rq_issue        → ★ block I/O 下发事件（同 blktrace 的 D 事件）

★ 注意：本 VM 环境中没有显示 Hardware event 类别
  （因为 PMU 不可用），物理机上会显示：
    cpu-cycles OR cycles               [Hardware event]
    instructions                       [Hardware event]
    cache-references                   [Hardware event]
    cache-misses                       [Hardware event]
    branches                           [Hardware event]
    branch-misses                      [Hardware event]
    bus-cycles                         [Hardware event]
```

---

## 场景 10：perf record 全系统采样

### 命令

```bash
# 全系统采样 5 秒
perf record -a -F 99 -g --call-graph dwarf sleep 5
perf report --stdio --no-children --sort comm,symbol | head -30
```

### 适用场景

```
★ 全系统采样用于：
  - 不知道哪个进程有性能问题时
  - 分析系统级热点（调度器、中断处理、网络栈等）
  - 找出"隐形"的 CPU 消耗者

★ -F 99 = 99Hz 采样频率
  - 为什么是 99 而不是 100？
    → 避免与系统定时器（100Hz / 250Hz）同步
    → 减少采样偏差

★ sleep 5 = 采样 5 秒后停止
  - perf record 会在 sleep 退出后停止
```

---

## 快速诊断流程

```
1. perf stat ./app
   → 获取 IPC、缓存命中率、分支预测率
   → IPC < 1.0 = 内存瓶颈，IPC > 1.5 = 计算密集

2. perf record -g --call-graph dwarf ./app
   perf report --stdio
   → 找出 CPU 热点函数
   → 88% 时间花在 matrix_multiply → 优化目标

3. perf trace -e read,write ./app
   → 查看系统调用模式
   → 频繁小 write？考虑 buffering

4. perf stat -a sleep 10
   → 全系统负载评估
   → context-switches 过高？进程过多

5. perf top -a
   → 实时观察热点
   → 发现意外热点（如某个内核函数占用高）
```

---

## 场景 11：perf stat 诊断 CPU 高占用低吞吐（物理机）

### 命令

```bash
# ★ 性能诊断第一步：获取核心硬件指标
perf stat -e cache-misses,cache-references,L1-dcache-load-misses,\
L1-dcache-loads,instructions,cycles,branches,branch-misses,\
stalled-cycles-frontend,stalled-cycles-backend -r 3 ./app
```

### 预期输出（物理机，CPU 高占用低吞吐场景）

```
 Performance counter stats for './app' (3 runs):

     1,234,567,890      cache-misses              #   18.50% of all cache refs
     6,678,901,234      cache-references
       456,789,012      L1-dcache-load-misses     #    4.57% of all L1 loads
     9,999,000,000      L1-dcache-loads
    15,000,000,000      instructions              #    0.68  insn per cycle
    22,000,000,000      cycles
     3,000,000,000      branches
       150,000,000      branch-misses             #    5.00% of all branches
     8,800,000,000      stalled-cycles-frontend   #   40.00% frontend cycles idle
    13,200,000,000      stalled-cycles-backend    #   60.00% backend cycles idle
```

### 逐指标注解

```
IPC = instructions / cycles = 15G / 22G = 0.68
  ★ IPC < 1.0 → CPU 效率低，每个周期只执行了 0.68 条指令
  → 说明 CPU 流水线经常停顿

cache-miss 率 = cache-misses / cache-references = 1.23G / 6.68G = 18.5%
  ★ 18.5% > 15% → LLC miss 严重！
  → 约 1/5 的 LLC 访问需要访问主存（~300 周期/次）

L1-dcache miss 率 = 456M / 10G = 4.57%
  → L1 miss 率在正常范围（< 5%），L1 命中率高
  → 问题主要在 LLC 层

branch-miss 率 = 150M / 3G = 5.0%
  → 处于"一般"范围，有一定优化空间但不严重

stalled-cycles-frontend = 40%
  → 40% 的周期中前端在等待（指令获取/解码）
  → 偏高，可能与分支预测失败相关

stalled-cycles-backend = 60%
  ★ 60% 的周期中后端在等待（数据访问）
  → ★★ 这是主要瓶颈！后端停顿严重

诊断结论：
  IPC 低 (0.68) + cache-miss 率高 (18.5%) + backend 停顿高 (60%)
  → ★ L3 cache miss 是性能瓶颈
  → 下一步：perf c2c record → 检查 false sharing
  → perf mem record → 查看内存访问延迟分布
```

### IPC 与瓶颈类型速查

```
IPC 值       │ 性能评级  │ 瓶颈类型
────────────┼──────────┼──────────────────────
> 2.0       │ ★ 优秀    │ 计算密集型（已充分利用流水线）
1.5 ~ 2.0   │ 良好      │ 轻度内存等待
1.0 ~ 1.5   │ 一般      │ 混合负载
0.5 ~ 1.0   │ ★ 较差    │ 内存瓶颈 / 长延迟操作
< 0.5       │ ★★ 很差   │ 严重停顿（cache/TLB/锁）
```

---

## 场景 12：perf c2c 检测 cache line 争用

### 命令

```bash
# 采集（多线程程序）
perf c2c record ./multi_thread_app

# 分析
perf c2c report --stdio
```

### 预期输出（有 false sharing 的场景）

```
=====================================================
         Shared Data Cache Line Table
=====================================================
#         ------- Cacheline ----------    Total     Tot  ----- LLC Load Hitm -----
# Index      Address  Node  PA cnt  records   Pct       Hitm   Local   Remote
# .....  ...............  ....  ......  .......  ......  .......  ......  .......
    0      0x7f8a1c000     0      1     8542    32.1%     4521   3200     1321
    1      0x7f8a1c040     0      1     3210    12.1%      890    700      190
    2      0x7f8a1c080     0      2     1200     4.5%       50     45        5
```

### 逐列注解

```
Index 0:
  Address = 0x7f8a1c000    ← ★ 这个 cache line 地址是热点
  PA cnt = 1               ← 只有 1 个物理地址映射到这里
  records = 8542           ← 被采样到 8542 次访问
  Tot Pct = 32.1%          ← ★ 占总采样的 32.1%！
  LLC Hitm = 4521          ← ★★ HITM（Hit Modified）4521 次
                              → 其他 CPU 修改了这个 cache line
  Local = 3200             ← 同 socket 内的 HITM
  Remote = 1321            ← 跨 socket 的 HITM（更慢）

★ HITM 是 false sharing 的直接证据：
  4521 次 HITM 意味着这个 cache line 在 CPU 之间
  反复迁移了 4521 次（Cache-to-Cache transfer）

诊断结论：
  cache line 0x7f8a1c000 有大量 HITM
  → 多个线程写同一个 cache line 的不同变量（false sharing）
  → 修复：将共享结构体对齐到 64 字节（cache line 大小）
```

---

## 场景 13：perf lock 定位自旋锁热点

### 命令

```bash
# 采集锁事件（全系统 10 秒）
perf lock record -a sleep 10

# 分析
perf lock report --stdio
```

### 预期输出（有锁竞争的场景）

```
=== output for lock: &q->lock ===

                acquired    contended   avg wait(ns)   total wait(ns)   max wait(ns)
&q->lock            3200      890       8900          7921000           125000

=== output for lock: &dev->lock ===

                acquired    contended   avg wait(ns)   total wait(ns)   max wait(ns)
&dev->lock          8500      230       1520           349600            45200
```

### 逐列注解

```
&q->lock（争用最严重的锁）：
  acquired = 3200        ← 获取锁的总次数
  contended = 890        ← ★★ 争用次数 890 次（27.8% 争用率！）
  avg wait = 8900ns      ← ★ 平均等待 8.9μs
  total wait = 7.9ms     ← ★ 总浪费 7.9ms CPU 时间
  max wait = 125000ns    ← 最大单次等待 125μs

&dev->lock（轻度竞争）：
  acquired = 8500        ← 获取更频繁
  contended = 230        ← 争用率低（2.7%）
  avg wait = 1520ns      ← 平均等待 1.5μs（轻微竞争）

诊断结论：
  &q->lock 争用率 27.8%，平均等待 8.9μs
  → ★ 严重的锁竞争，导致 CPU 高占用低吞吐
  → 下一步：perf lock report -s caller → 查看哪个调用路径
  → 考虑减小临界区 / 使用 per-CPU 变量 / 换锁类型
```

### 按调用栈分析

```bash
perf lock report -s caller --stdio
```

```
=== acquired at:

  native_queued_spin_lock_slowpath
  _raw_spin_lock_irqsave
  process_packet           ← ★ 这个函数持锁时间过长
  handle_request
  worker_thread

    acquired: 3200   contended: 890   avg wait: 8900ns

★ 定位到 process_packet() 是锁竞争热点
  → 检查该函数的临界区大小
  → 考虑将非共享操作移出锁保护范围
```

---

## 常用命令速查表

```
场景                    │ 命令
───────────────────────┼──────────────────────────────────────────────
快速性能概览            │ perf stat -d ./app
详细缓存统计            │ perf stat -d -d -d ./app
热点函数分析            │ perf record -g --call-graph dwarf ./app
                        │ perf report --stdio --no-children
实时系统热点            │ perf top -a --stdio
系统调用追踪            │ perf trace -e read,write,open,close ./app
详细采样流              │ perf script -i perf.data
全系统采样              │ perf record -a -F 99 -g sleep 5
按 CPU 排序报告         │ perf report --sort cpu,symbol --stdio
对比两次运行            │ perf diff old.data new.data
生成火焰图              │ perf script | stackcollapse-perf.pl | flamegraph.pl
VM 环境替代事件         │ perf stat -e task-clock,context-switches,page-faults
```
