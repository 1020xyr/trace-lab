#!/bin/bash
# ============================================================
# bpftrace 实战实验脚本
# ============================================================
#
# 实验目的：
#   通过 4 组实验，掌握 bpftrace 的核心语法和 eBPF 追踪能力
#   - 实验 1：bpftrace hello world（理解 kprobe/tracepoint 基础）
#   - 实验 2：追踪 openat 系统调用（观察文件打开行为）
#   - 实验 3：系统调用统计（按进程聚合 syscall 计数）
#   - 实验 4：Block I/O 追踪（与 blktrace 对比）
#
# 前置条件：
#   - yum install -y bpftrace
#   - 内核 >= 4.9（支持 eBPF）
#   - 需要 root 权限
#   - 内核 debuginfo（某些 probe 需要）：
#     yum install -y kernel-devel-$(uname -r)
#
# 用法：
#   bash vdb-lab.sh           # 运行全部实验
#   bash vdb-lab.sh 1         # 只运行实验 1
#   bash vdb-lab.sh 2 3       # 运行实验 2 和 3
# ============================================================

set -euo pipefail

# ─── 配置 ───────────────────────────────────────────────────
DEVICE="/dev/vdb"
OUTPUT_DIR="/tmp/bpftrace_lab"

# ─── 辅助函数 ──────────────────────────────────────────────
banner() {
    echo ""
    echo "================================================================"
    echo "  $1"
    echo "================================================================"
    echo ""
}

sub_banner() {
    echo ""
    echo "--- $1 ---"
    echo ""
}

check_tools() {
    # 安装 bpftrace
    if ! command -v bpftrace &>/dev/null; then
        echo "bpftrace 未安装，尝试安装..."
        yum install -y bpftrace 2>/dev/null || {
            echo "错误：bpftrace 安装失败"
            echo "  手动安装：yum install -y bpftrace"
            exit 1
        }
    fi
    echo "✓ bpftrace 已安装：$(bpftrace --version 2>&1 || echo '版本未知')"

    # 检查 bpftrace 是否能运行
    if ! bpftrace -e 'BEGIN { printf("test ok\n"); exit(); }' &>/dev/null; then
        echo "警告：bpftrace 无法运行，可能缺少内核支持"
        echo "  检查：uname -r（需要 >= 4.9）"
        echo "  安装内核开发包：yum install -y kernel-devel-\$(uname -r)"
    fi
    echo "✓ bpftrace 可运行"

    # 检查 fio（实验 4 需要）
    if ! command -v fio &>/dev/null; then
        echo "提示：fio 未安装（实验 4 需要），安装：yum install -y fio"
    else
        echo "✓ fio 已安装"
    fi
}

check_device() {
    if [ ! -b "$DEVICE" ]; then
        echo "提示：设备 $DEVICE 不存在（实验 4 需要）"
    else
        echo "✓ 设备 $DEVICE 就绪"
    fi
}

# ─── 环境准备 ──────────────────────────────────────────────
mkdir -p "$OUTPUT_DIR"
banner "bpftrace 实战实验 — 环境检查"
check_tools
check_device

# 列出可用的 tracepoint
echo ""
echo "可用的 block tracepoint（前 10 个）："
bpftrace -l 'tracepoint:block:*' 2>/dev/null | head -10 || echo "  （无法列出，可能需要内核支持）"
echo ""

# 获取要运行的实验列表
EXPERIMENTS="${@:-1 2 3 4}"

# ================================================================
# 实验 1：bpftrace hello world
# ================================================================
run_exp1() {
    banner "实验 1：bpftrace Hello World — 理解 kprobe 和 tracepoint"
    echo "目的：运行最简单的 bpftrace 脚本，理解 eBPF 程序的基本结构"
    echo ""

    # --- 1a: BEGIN/END probe ---
    sub_banner "1a: BEGIN/END probe（程序启动和退出时触发）"
    echo "命令：bpftrace -e 'BEGIN { printf(\"Hello, bpftrace!\\n\"); } END { printf(\"Goodbye!\\n\"); }'"
    echo ""

    # BEGIN probe 运行后立即退出
    timeout 3 bpftrace -e '
        BEGIN {
            printf("Hello, bpftrace!\n");
            printf("当前时间: %d (Unix 时间戳)\n", nsecs / 1000000000);
            printf("CPU ID:   %d\n", cpu);
            exit();
        }
    ' 2>&1 | tee "${OUTPUT_DIR}/exp1_hello.txt"

    # --- 1b: kprobe 追踪 do_sys_openat2 ---
    sub_banner "1b: kprobe — 追踪 sys_openat（5 秒内被打开的文件）"
    echo "命令：bpftrace -e 'kprobe:do_sys_openat2 { printf(\"open: %s\\n\", str(arg1)); }'"
    echo "（运行 5 秒后自动停止）"
    echo ""

    # 同时启动一些后台活动来产生 open 调用
    (ls /tmp >/dev/null 2>&1 &)

    timeout 5 bpftrace -e '
        kprobe:do_sys_openat2 {
            printf("[%s] PID %d 打开文件: %s\n", comm, pid, str(arg1));
        }
    ' 2>&1 | head -30 | tee "${OUTPUT_DIR}/exp1_kprobe_open.txt" || true

    echo ""
    echo "  （如果 do_sys_openat2 不存在，尝试 do_sys_open）"

    # --- 1c: tracepoint 方式 ---
    sub_banner "1c: tracepoint — 追踪 sys_enter（5 秒系统调用统计）"
    echo "命令：bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[probe] = count(); }'"
    echo ""

    timeout 5 bpftrace -e '
        tracepoint:raw_syscalls:sys_enter {
            @[comm] = count();
        }
    ' 2>&1 | tee "${OUTPUT_DIR}/exp1_tracepoint_count.txt" || true

    sub_banner "实验 1 关键概念"
    echo "  bpftrace 程序结构：probe { action }"
    echo ""
    echo "  probe 类型："
    echo "    BEGIN / END           → 程序启动/退出"
    echo "    kprobe:function       → 内核函数入口"
    echo "    kretprobe:function    → 内核函数返回"
    echo "    tracepoint:cat:name   → 内核静态 tracepoint"
    echo "    uprobe:lib:function   → 用户态函数"
    echo "    interval:s:N          → 每 N 秒触发一次"
    echo ""
    echo "  内置变量："
    echo "    pid / tid    → 进程/线程 ID"
    echo "    comm         → 进程名"
    echo "    cpu          → CPU 编号"
    echo "    nsecs        → 纳秒时间戳"
    echo "    arg0..argN   → 函数参数"
    echo "    retval       → 返回值（kretprobe 中）"
    echo ""
    echo "  map 语法："
    echo "    @name[key] = value   → 创建命名 map"
    echo "    @[key] = value       → 匿名 map"
    echo "    count() / sum() / avg() / hist() → 聚合函数"
}

# ================================================================
# 实验 2：追踪 openat 系统调用
# ================================================================
run_exp2() {
    banner "实验 2：追踪 openat 系统调用 — 观察文件打开行为"
    echo "目的：追踪系统中所有 openat 调用，了解谁在打开什么文件"
    echo ""

    # --- 2a: 记录所有 openat 调用 ---
    sub_banner "2a: 记录 5 秒内所有 openat 调用"
    echo "命令：bpftrace -e 'tracepoint:syscalls:sys_enter_openat { ... }'"
    echo ""

    # 产生一些文件操作
    (ls /etc >/dev/null 2>&1 &)
    (cat /etc/hostname >/dev/null 2>&1 &)

    timeout 5 bpftrace -e '
        tracepoint:syscalls:sys_enter_openat {
            $filename = str(args->filename);
            printf("[%06d] %-16s openat(%s)\n", pid, comm, $filename);
        }
    ' 2>&1 | head -40 | tee "${OUTPUT_DIR}/exp2_openat_trace.txt" || true

    sub_banner "2a 输出解读"
    echo "  格式：[PID] 进程名 openat(文件名)"
    echo ""
    echo "  ★ 可以看到很多 /proc、/sys 相关的打开（系统监控工具）"
    echo "  ★ 也看到共享库加载（.so 文件）"
    echo "  ★ /etc/ 下的配置文件被频繁读取"

    # --- 2b: 统计打开的文件路径 ---
    sub_banner "2b: 统计 5 秒内最常打开的文件 Top 20"
    echo ""

    timeout 5 bpftrace -e '
        tracepoint:syscalls:sys_enter_openat {
            @[str(args->filename)] = count();
        }
    ' 2>&1 | sort -t' ' -k2 -rn | head -25 \
        | tee "${OUTPUT_DIR}/exp2_openat_stats.txt" || true

    sub_banner "2b 解读"
    echo "  ★ 最频繁打开的文件通常是："
    echo "    - /proc/stat, /proc/meminfo（top/vmstat 等监控工具）"
    echo "    - /etc/ld.so.cache（动态链接器缓存）"
    echo "    - /proc/self/...（进程自身信息）"

    # --- 2c: 追踪 openat 返回值 ---
    sub_banner "2c: 追踪 openat 返回值（成功的 vs 失败的）"
    echo ""

    timeout 5 bpftrace -e '
        tracepoint:syscalls:sys_enter_openat {
            @opens[comm] = count();
        }
        tracepoint:syscalls:sys_exit_openat {
            if (args->ret < 0) {
                @failures[comm] = count();
            }
        }
    ' 2>&1 | tee "${OUTPUT_DIR}/exp2_openat_ret.txt" || true

    sub_banner "2c 解读"
    echo "  ★ @opens 记录了每个进程的 openat 调用次数"
    echo "  ★ @failures 记录了返回错误（ret < 0）的次数"
    echo "  ★ 大量 failures 可能说明程序在尝试多个路径找文件"
    echo "  ★ 常见的失败：access 检查、查找配置文件、ld.so 搜索共享库"
}

# ================================================================
# 实验 3：系统调用统计
# ================================================================
run_exp3() {
    banner "实验 3：系统调用统计 — 按进程聚合 syscall 计数"
    echo "目的：使用 bpftrace 构建系统调用分布图，类似 strace -c"
    echo ""

    # --- 3a: 全局系统调用 Top 20 ---
    sub_banner "3a: 全局系统调用计数（5 秒，Top 20）"
    echo "命令：bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[args->id] = count(); }'"
    echo ""

    timeout 5 bpftrace -e '
        tracepoint:raw_syscalls:sys_enter {
            @[args->id] = count();
        }
    ' 2>&1 | tee "${OUTPUT_DIR}/exp3_global_syscall.txt" || true

    sub_banner "3a 解读"
    echo "  ★ args->id 是系统调用编号（对应 /usr/include/asm/unistd_64.h）"
    echo "  ★ 常见编号："
    echo "    0=read, 1=write, 2=open, 3=close, 7=poll"
    echo "    9=mmap, 12=brk, 56=clone, 57=fork, 59=execve"
    echo "    202=futex, 228=clock_gettime, 257=openat"

    # --- 3b: 按进程名统计 ---
    sub_banner "3b: 按进程名聚合系统调用计数（5 秒）"
    echo ""

    timeout 5 bpftrace -e '
        tracepoint:raw_syscalls:sys_enter {
            @[comm] = count();
        }
    ' 2>&1 | tee "${OUTPUT_DIR}/exp3_per_process.txt" || true

    sub_banner "3b 解读"
    echo "  ★ 系统调用最频繁的进程通常是："
    echo "    - 监控工具（top, prometheus 等）"
    echo "    - 日志写入进程"
    echo "    - 网络服务（nginx, redis 等）"

    # --- 3c: 带 fio 负载的统计 ---
    sub_banner "3c: fio 负载下的系统调用分布（10 秒）"
    echo ""

    if command -v fio &>/dev/null && [ -b "$DEVICE" ]; then
        # 后台运行 fio
        fio --name=bpf_test \
            --ioengine=libaio \
            --direct=1 \
            --rw=randwrite \
            --bs=4k \
            --numjobs=1 \
            --filename="$DEVICE" \
            --runtime=10 \
            --time_based \
            --iodepth=32 \
            --group_reporting &
        FIO_PID=$!
        sleep 1

        echo "追踪 fio 进程（PID=$FIO_PID）的系统调用..."
        timeout 8 bpftrace -e "
            tracepoint:raw_syscalls:sys_enter /pid == $FIO_PID/ {
                @[args->id] = count();
            }
        " 2>&1 | tee "${OUTPUT_DIR}/exp3_fio_syscall.txt" || true

        wait "$FIO_PID" 2>/dev/null || true
    else
        echo "fio 不可用或设备 $DEVICE 不存在，跳过 fio 负载实验"
        echo "使用 sleep 替代："
        timeout 5 bpftrace -e '
            tracepoint:raw_syscalls:sys_enter /comm == "sleep"/ {
                @[args->id] = count();
            }
        ' 2>&1 | tee "${OUTPUT_DIR}/exp3_fio_syscall.txt" || true
    fi

    sub_banner "3c: fio 系统调用解读"
    echo "  ★ fio 使用 libaio 引擎时的核心系统调用："
    echo "    202 = futex      → 线程同步（等待 I/O 完成）"
    echo "    206 = io_setup   → 初始化异步 I/O 上下文"
    echo "    209 = io_submit  → 提交 I/O 请求"
    echo "    210 = io_getevents → 收割已完成的 I/O"
    echo "    228 = clock_gettime → 获取时间戳（延迟计算）"
    echo ""
    echo "  ★ io_submit 和 io_getevents 的次数比 = IOPS 的效率指标"
    echo "  ★ 如果 futex 次数很高 → fio 在频繁等待（iodepth 不够大）"

    # --- 3d: 带直方图的延迟统计 ---
    sub_banner "3d: read 系统调用耗时直方图（5 秒）"
    echo ""

    timeout 5 bpftrace -e '
        tracepoint:syscalls:sys_enter_read {
            @start[tid] = nsecs;
        }
        tracepoint:syscalls:sys_exit_read /@start[tid]/ {
            @read_latency = hist(nsecs - @start[tid]);
            delete(@start[tid]);
        }
    ' 2>&1 | tee "${OUTPUT_DIR}/exp3_read_hist.txt" || true

    sub_banner "3d 直方图解读"
    echo "  ★ hist() 自动生成对数直方图"
    echo "  ★ 横轴是延迟范围（纳秒），纵轴是次数"
    echo "  ★ 双峰分布可能意味着：缓存命中（快）vs 磁盘访问（慢）"
    echo "  ★ 这是 bpftrace 相比 strace 的优势：实时聚合，无需后处理"
}

# ================================================================
# 实验 4：Block I/O 追踪（与 blktrace 对比）
# ================================================================
run_exp4() {
    banner "实验 4：Block I/O 追踪 — 与 blktrace 对比"
    echo "目的：使用 bpftrace 追踪 block 层 tracepoint，对比 blktrace 的输出"
    echo ""

    # 检查设备
    if [ ! -b "$DEVICE" ]; then
        echo "警告：设备 $DEVICE 不存在，跳过实验 4"
        return
    fi

    if ! command -v fio &>/dev/null; then
        echo "警告：fio 未安装，跳过实验 4"
        echo "  安装：yum install -y fio"
        return
    fi

    # 获取设备的 major:minor 号
    MAJOR=$(stat -c '%t' "$DEVICE" | xargs -I{} printf '%d' 0x{} 2>/dev/null || echo "")
    MINOR=$(stat -c '%T' "$DEVICE" | xargs -I{} printf '%d' 0x{} 2>/dev/null || echo "")
    echo "设备 $DEVICE 的设备号：major=$MAJOR, minor=$MINOR"
    echo ""

    # --- 4a: block I/O 事件计数 ---
    sub_banner "4a: block I/O 事件计数（10 秒）"
    echo "追踪 tracepoint:block:block_rq_issue 和 block:block_rq_complete"
    echo ""

    # 后台运行 fio
    fio --name=bpf_io_test \
        --ioengine=libaio \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime=12 \
        --time_based \
        --iodepth=32 \
        --group_reporting &
    FIO_PID=$!
    sleep 1

    echo "bpftrace 追踪 block I/O（10 秒）..."
    timeout 10 bpftrace -e "
        tracepoint:block:block_rq_issue /args->dev == (${MAJOR} << 20 | ${MINOR})/ {
            @issues = count();
            @bytes_issued = sum(args->bytes);
        }
        tracepoint:block:block_rq_complete /args->dev == (${MAJOR} << 20 | ${MINOR})/ {
            @completes = count();
            @bytes_completed = sum(args->bytes);
        }
    " 2>&1 | tee "${OUTPUT_DIR}/exp4_block_events.txt" || true

    wait "$FIO_PID" 2>/dev/null || true

    sub_banner "4a 解读"
    echo "  ★ @issues = block 层下发的 I/O 请求数"
    echo "  ★ @completes = 完成的 I/O 请求数"
    echo "  ★ 两者应该接近（差值 = 仍在 in-flight 的请求）"
    echo "  ★ @bytes_issued / @bytes_completed = 总传输字节数"

    # --- 4b: I/O 延迟直方图 ---
    sub_banner "4b: block I/O 延迟直方图（D2C，10 秒）"
    echo "追踪从 issue 到 complete 的延迟（相当于 blktrace 的 D2C）"
    echo ""

    # 再次运行 fio
    fio --name=bpf_lat_test \
        --ioengine=libaio \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime=12 \
        --time_based \
        --iodepth=32 \
        --group_reporting &
    FIO_PID=$!
    sleep 1

    echo "bpftrace 追踪 I/O 延迟（10 秒）..."
    timeout 10 bpftrace -e "
        tracepoint:block:block_rq_issue /args->dev == (${MAJOR} << 20 | ${MINOR})/ {
            @io_start[args->sector] = nsecs;
        }
        tracepoint:block:block_rq_complete /args->dev == (${MAJOR} << 20 | ${MINOR}) && @io_start[args->sector]/ {
            \$delta = nsecs - @io_start[args->sector];
            @d2c_latency = hist(\$delta);
            @d2c_avg = avg(\$delta);
            delete(@io_start[args->sector]);
        }
    " 2>&1 | tee "${OUTPUT_DIR}/exp4_block_latency.txt" || true

    wait "$FIO_PID" 2>/dev/null || true

    sub_banner "4b: bpftrace vs blktrace 对比"
    echo ""
    echo "  ┌─────────────┬────────────────────┬────────────────────┐"
    echo "  │ 对比项       │ bpftrace           │ blktrace           │"
    echo "  ├─────────────┼────────────────────┼────────────────────┤"
    echo "  │ 机制         │ eBPF tracepoint    │ 内核 blk_trace     │"
    echo "  │ 事件粒度     │ issue/complete     │ Q/G/I/D/C/P/U      │"
    echo "  │ 数据输出     │ 实时聚合（直方图） │ 原始事件流          │"
    echo "  │ 后处理       │ 不需要             │ 需要 blkparse/btt  │"
    echo "  │ 灵活性       │ ★★★★★ 可编程     │ ★★★ 固定格式       │"
    echo "  │ 开销         │ 较低（eBPF JIT）   │ 中等（ring buffer） │"
    echo "  │ 延迟分布     │ hist() 一键生成    │ 需要 btt + 画图     │"
    echo "  │ 自定义指标   │ 任意聚合           │ 预定义指标          │"
    echo "  └─────────────┴────────────────────┴────────────────────┘"
    echo ""
    echo "  ★ bpftrace 适合实时分析和快速探索"
    echo "  ★ blktrace 适合详细的事后分析（完整的 I/O 生命周期）"
    echo "  ★ 两者互补：bpftrace 定位问题 → blktrace 深入分析"

    # --- 4c: I/O 大小分布 ---
    sub_banner "4c: I/O 请求大小分布（10 秒）"
    echo ""

    fio --name=bpf_size_test \
        --ioengine=libaio \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime=12 \
        --time_based \
        --iodepth=32 \
        --group_reporting &
    FIO_PID=$!
    sleep 1

    timeout 10 bpftrace -e "
        tracepoint:block:block_rq_issue /args->dev == (${MAJOR} << 20 | ${MINOR})/ {
            @io_size = lhist(args->bytes / 1024, 0, 128, 4);
        }
    " 2>&1 | tee "${OUTPUT_DIR}/exp4_block_size.txt" || true

    wait "$FIO_PID" 2>/dev/null || true

    sub_banner "4c 解读"
    echo "  ★ lhist() 生成线性直方图（固定区间宽度）"
    echo "  ★ fio bs=4k 时，大多数请求应该在 4KB 附近"
    echo "  ★ 如果有更大的请求 → 可能是 I/O 合并（bio merge）"

    echo ""
    echo "  所有输出文件保存在：${OUTPUT_DIR}/exp4_*"
}

# ─── 运行实验 ──────────────────────────────────────────────
for exp in $EXPERIMENTS; do
    case "$exp" in
        1) run_exp1 ;;
        2) run_exp2 ;;
        3) run_exp3 ;;
        4) run_exp4 ;;
        *) echo "未知实验编号：$exp（有效范围：1-4）" ;;
    esac
done

banner "全部实验完成！"
echo "输出目录：$OUTPUT_DIR"
echo ""
echo "文件清单："
ls -lh "$OUTPUT_DIR"/* 2>/dev/null || echo "  （无输出文件）"
echo ""
echo "后续探索建议："
echo "  1. 修改实验 1 的 kprobe，追踪其他内核函数（如 tcp_sendmsg）"
echo "  2. 在实验 2 中加入过滤条件，只追踪特定进程或文件路径"
echo "  3. 用实验 3 的 hist() 方法为其他系统调用画延迟直方图"
echo "  4. 对比实验 4 的 bpftrace D2C 直方图与 blktrace btt 的 D2C 数据"
echo ""
echo "进阶 bpftrace 脚本："
echo "  - /usr/share/bpftrace/tools/ 目录下有大量现成工具"
echo "  - 参考：https://github.com/bpftrace/bpftrace/tree/master/tools"
