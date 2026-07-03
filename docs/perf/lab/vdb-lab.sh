#!/bin/bash
# ============================================================
# perf 实战实验脚本
# ============================================================
#
# 实验目的：
#   通过 4 组实验，掌握 perf 的核心子命令和性能分析能力
#   - 实验 1：perf stat 基础（cache-miss / branch-miss / IPC）
#   - 实验 2：perf record + report 热点分析（对 fio 进程采样）
#   - 实验 3：perf top 实时观察（内核热点函数）
#   - 实验 4：perf lock 锁竞争分析
#
# 前置条件：
#   - yum install -y perf（或 perf 已在 kernel-tools 中）
#   - 内核需开启 CONFIG_PERF_EVENTS、CONFIG_LOCK_STAT（实验 4）
#   - 测试设备：/dev/vdb
#   - 以 root 身份运行
#
# 用法：
#   bash vdb-lab.sh           # 运行全部实验
#   bash vdb-lab.sh 1         # 只运行实验 1
#   bash vdb-lab.sh 2 3       # 运行实验 2 和 3
# ============================================================

set -euo pipefail

# ─── 配置 ───────────────────────────────────────────────────
DEVICE="/dev/vdb"
OUTPUT_DIR="/tmp/perf_lab"
RUNTIME=8

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
    # 安装 perf（如果不存在）
    if ! command -v perf &>/dev/null; then
        echo "perf 未安装，尝试安装..."
        yum install -y perf 2>/dev/null || {
            echo "错误：perf 安装失败，请手动安装：yum install -y perf"
            exit 1
        }
    fi
    echo "✓ perf 已安装：$(perf --version 2>&1 || echo '版本未知')"

    # 检查 perf 是否可用（内核支持）
    if ! perf stat -e cycles true &>/dev/null; then
        echo "警告：perf stat 无法运行，可能缺少内核支持或权限"
        echo "  尝试：echo -1 > /proc/sys/kernel/perf_event_paranoid"
    fi

    # 检查 fio
    if ! command -v fio &>/dev/null; then
        echo "错误：fio 未安装，请执行 yum install -y fio"
        exit 1
    fi
    echo "✓ fio 已安装"
}

check_device() {
    if [ ! -b "$DEVICE" ]; then
        echo "错误：设备 $DEVICE 不存在或不是块设备"
        exit 1
    fi
    echo "✓ 设备 $DEVICE 就绪"
}

# ─── 环境准备 ──────────────────────────────────────────────
mkdir -p "$OUTPUT_DIR"
banner "perf 实战实验 — 环境检查"
check_tools
check_device

# 降低 perf_event_paranoid（允许非 root 也能采样，但这里以 root 运行）
echo -1 > /proc/sys/kernel/perf_event_paranoid 2>/dev/null || true
echo "✓ perf_event_paranoid 已设置为 -1"
echo ""

# 获取要运行的实验列表
EXPERIMENTS="${@:-1 2 3 4}"

# ================================================================
# 实验 1：perf stat 基础（-d 看 cache-miss / branch-miss / IPC）
# ================================================================
run_exp1() {
    banner "实验 1：perf stat 基础 — 硬件计数器概览"
    echo "目的：使用 perf stat -d 获取 CPU 硬件性能计数器数据"
    echo "指标：IPC、cache-miss、branch-miss、instructions、cycles"
    echo ""

    # --- 1a: perf stat -d 对 fio 进程统计 ---
    sub_banner "1a: perf stat -d 统计 fio 随机写（${RUNTIME}秒）"
    echo "命令：perf stat -d -- fio --name=stat_test ..."
    echo ""

    perf stat -d -o "${OUTPUT_DIR}/exp1_stat_default.txt" -- \
        fio --name=stat_test \
            --ioengine=libaio \
            --direct=1 \
            --rw=randwrite \
            --bs=4k \
            --numjobs=1 \
            --filename="$DEVICE" \
            --runtime="$RUNTIME" \
            --time_based \
            --iodepth=32 \
            --group_reporting 2>&1

    echo "perf stat -d 输出："
    echo "──────────────────────────────────────────"
    cat "${OUTPUT_DIR}/exp1_stat_default.txt"
    echo "──────────────────────────────────────────"

    sub_banner "1a 指标解读"
    echo "  cycles           → CPU 周期数（越高消耗越多 CPU 时间）"
    echo "  instructions     → 执行指令数"
    echo "  IPC              → instructions / cycles（每周期执行指令数，越高越好）"
    echo "                      IPC > 1.0 表示流水线利用良好"
    echo "                      IPC < 0.5 表示大量 stall（等待内存/分支预测失败）"
    echo "  cache-misses     → LLC（Last Level Cache）未命中次数"
    echo "  cache-references → LLC 访问总次数"
    echo "  cache-miss rate  → cache-misses / cache-references（越低越好）"
    echo "  branch-misses    → 分支预测失败次数"
    echo "  branches         → 分支指令总次数"
    echo "  branch-miss rate → branch-misses / branches（越低越好，< 5% 正常）"

    # --- 1b: 自定义事件 ---
    sub_banner "1b: 自定义事件 — 关注 block I/O 相关"
    echo "命令：perf stat -e cycles,instructions,context-switches,cpu-migrations,..."
    echo ""

    perf stat \
        -e cycles \
        -e instructions \
        -e context-switches \
        -e cpu-migrations \
        -e page-faults \
        -e L1-dcache-load-misses \
        -e L1-icache-load-misses \
        -e dTLB-load-misses \
        -o "${OUTPUT_DIR}/exp1_stat_custom.txt" -- \
        fio --name=stat_custom \
            --ioengine=libaio \
            --direct=1 \
            --rw=randwrite \
            --bs=4k \
            --numjobs=1 \
            --filename="$DEVICE" \
            --runtime="$RUNTIME" \
            --time_based \
            --iodepth=32 \
            --group_reporting 2>&1

    echo "自定义事件输出："
    echo "──────────────────────────────────────────"
    cat "${OUTPUT_DIR}/exp1_stat_custom.txt"
    echo "──────────────────────────────────────────"

    sub_banner "1b 指标解读"
    echo "  context-switches     → 上下文切换次数（高 = 频繁切换，可能是 I/O 等待）"
    echo "  cpu-migrations       → 进程在 CPU 之间迁移次数（高 = 缓存不友好）"
    echo "  page-faults          → 缺页中断（major fault = 需要从磁盘加载）"
    echo "  L1-dcache-load-misses → L1 数据缓存未命中（需要访问 L2/L3）"
    echo "  L1-icache-load-misses → L1 指令缓存未命中"
    echo "  dTLB-load-misses      → 数据 TLB 未命中（需要 walk 页表）"
}

# ================================================================
# 实验 2：perf record + report 热点分析
# ================================================================
run_exp2() {
    banner "实验 2：perf record + report — 函数级热点分析"
    echo "目的：对 fio 进程进行采样，找出 CPU 热点函数"
    echo ""

    PERF_DATA="${OUTPUT_DIR}/exp2_perf.data"
    rm -f "$PERF_DATA"

    # --- 2a: 后台运行 fio，perf record 采样 ---
    sub_banner "2a: perf record 采样 fio 进程"
    echo "启动 fio（后台）..."

    fio --name=record_test \
        --ioengine=libaio \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime="$RUNTIME" \
        --time_based \
        --iodepth=32 \
        --group_reporting &
    FIO_PID=$!
    echo "fio PID: $FIO_PID"

    # 等待 fio 启动
    sleep 1

    echo "perf record 采样（频率 99Hz，持续 $(( RUNTIME - 2 )) 秒）..."
    # -F 99: 采样频率 99Hz（避免与定时器频率 100Hz 共振）
    # -g: 记录调用栈
    # -p: 追踪指定 PID
    perf record -F 99 -g -p "$FIO_PID" \
        -o "$PERF_DATA" \
        -- sleep $(( RUNTIME - 2 )) 2>&1 || true

    # 等待 fio 完成
    wait "$FIO_PID" 2>/dev/null || true
    echo "fio 完成"

    # --- 2b: perf report ---
    sub_banner "2b: perf report — 热点函数 Top 20"
    echo "命令：perf report -i exp2_perf.data --stdio --no-children -n --sort=dso,sym"
    echo ""

    perf report -i "$PERF_DATA" --stdio --no-children -n \
        --sort=dso,sym \
        --percent-limit 0.5 2>&1 \
        | head -60 \
        | tee "${OUTPUT_DIR}/exp2_report.txt"

    sub_banner "2b 解读指南"
    echo "  Overhead → 该函数占总采样比例的百分比"
    echo "  Samples  → 该函数被采样到的次数"
    echo "  Shared Object → 所属的库/内核模块"
    echo "  Symbol   → 函数名"
    echo ""
    echo "  ★ 如果 [kernel.kallsyms] 占大头 → I/O 主要在内核态处理"
    echo "  ★ 如果 fio 自身函数占大头 → 用户态有开销（提交/收割 I/O）"
    echo "  ★ 关注 io_submit / io_getevents 相关的内核函数"

    # --- 2c: 带调用栈的报告 ---
    sub_banner "2c: perf report — 带调用栈（callee 视角）"
    echo "命令：perf report -i exp2_perf.data --stdio --no-children --call-graph=callee"
    echo ""

    perf report -i "$PERF_DATA" --stdio --no-children \
        --call-graph=callee \
        --percent-limit 1.0 2>&1 \
        | head -80 \
        | tee "${OUTPUT_DIR}/exp2_callgraph.txt"

    echo ""
    echo "  ★ 调用栈展示了热点函数的调用路径"
    echo "  ★ 从叶子函数向上追溯，可以看到完整的 I/O 路径"
}

# ================================================================
# 实验 3：perf top 实时观察
# ================================================================
run_exp3() {
    banner "实验 3：perf top — 实时热点观察"
    echo "目的：使用 perf top 实时观察系统和内核的热点函数"
    echo "注意：perf top 是交互式命令，这里使用批处理模式 (--stdio)"
    echo ""

    # --- 3a: 系统级 perf top ---
    sub_banner "3a: perf top 系统级快照（10 秒）"
    echo "命令：perf top -b --stdio -d 10"
    echo "（-b 使用 backward ring buffer，--stdio 非交互模式，-d 10 持续 10 秒）"
    echo ""

    # 先启动 fio 作为负载
    fio --name=top_test \
        --ioengine=libaio \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime=15 \
        --time_based \
        --iodepth=32 \
        --group_reporting &
    FIO_PID=$!
    sleep 1

    # perf top 批处理模式：-d 10 表示显示 10 秒后退出
    # 注意：某些版本 perf top 不支持 -d，改用 perf record + report 替代
    echo "采集系统级热点（10 秒）..."
    perf record -F 99 -g -a \
        -o "${OUTPUT_DIR}/exp3_system.data" \
        -- sleep 10 2>&1 || true

    echo ""
    echo "系统级热点函数 Top 20："
    echo "──────────────────────────────────────────"
    perf report -i "${OUTPUT_DIR}/exp3_system.data" --stdio --no-children \
        --percent-limit 0.5 2>&1 \
        | head -40 \
        | tee "${OUTPUT_DIR}/exp3_system_report.txt"
    echo "──────────────────────────────────────────"

    # --- 3b: 内核态热点 ---
    sub_banner "3b: 仅内核态热点（过滤用户态）"
    echo "命令：perf report --stdio --dso=[kernel.kallsyms]"
    echo ""

    perf report -i "${OUTPUT_DIR}/exp3_system.data" --stdio --no-children \
        --dso='[kernel.kallsyms]' \
        --percent-limit 0.5 2>&1 \
        | head -40 \
        | tee "${OUTPUT_DIR}/exp3_kernel_report.txt"

    # 等待 fio 完成
    wait "$FIO_PID" 2>/dev/null || true

    sub_banner "实验 3 解读指南"
    echo "  ★ 系统级采样可以看到所有 CPU 上的热点函数"
    echo "  ★ 内核态热点通常包括："
    echo "    - blk_mq_* 系列：block 多队列层函数"
    echo "    - virtio_* / vring_*：virtio 驱动函数"
    echo "    - io_submit / io_getevents：异步 I/O 路径"
    echo "    - __schedule / schedule：调度器（I/O 等待时出现）"
    echo "  ★ 用户态热点通常包括："
    echo "    - io_submit / io_getevents (libc wrapper)"
    echo "    - fio 自身的 I/O 提交和完成处理函数"
}

# ================================================================
# 实验 4：perf lock 锁竞争分析
# ================================================================
run_exp4() {
    banner "实验 4：perf lock 锁竞争分析"
    echo "目的：使用 perf lock 检测内核锁竞争，找出热点锁"
    echo "前置：内核需开启 CONFIG_LOCK_STAT 和 CONFIG_LOCKDEP"
    echo ""

    # 检查 perf lock 是否可用
    if ! perf lock record --help &>/dev/null 2>&1; then
        echo "警告：perf lock 不可用（内核可能未开启 CONFIG_LOCK_STAT）"
        echo "  跳过实验 4"
        return
    fi

    LOCK_DATA="${OUTPUT_DIR}/exp4_lock.data"
    rm -f "$LOCK_DATA"

    # --- 4a: perf lock record ---
    sub_banner "4a: perf lock record — 记录锁事件"
    echo "启动 fio（后台）..."

    fio --name=lock_test \
        --ioengine=libaio \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=4 \
        --filename="$DEVICE" \
        --runtime="$RUNTIME" \
        --time_based \
        --iodepth=32 \
        --group_reporting &
    FIO_PID=$!
    sleep 1

    echo "perf lock record 采集（$(( RUNTIME - 2 )) 秒）..."
    echo "（追踪 lock:lock_acquire / lock_release / lock_contend 等 tracepoint）"
    perf lock record -a \
        -o "$LOCK_DATA" \
        -- sleep $(( RUNTIME - 2 )) 2>&1 || true

    wait "$FIO_PID" 2>/dev/null || true
    echo "fio 完成"

    # --- 4b: perf lock report ---
    sub_banner "4b: perf lock report — 锁等待时间统计"
    echo "命令：perf lock report（按等待时间排序）"
    echo ""

    perf lock report -i "$LOCK_DATA" 2>&1 \
        | head -50 \
        | tee "${OUTPUT_DIR}/exp4_lock_report.txt"

    # --- 4c: perf lock contention ---
    sub_banner "4c: 锁竞争 Top 10（按竞争次数排序）"
    echo ""

    # 某些版本的 perf lock 支持 -c (contention) 选项
    perf lock contention -i "$LOCK_DATA" 2>&1 \
        | head -30 \
        | tee "${OUTPUT_DIR}/exp4_lock_contention.txt" || echo "（perf lock contention 不可用）"

    sub_banner "实验 4 解读指南"
    echo "  perf lock report 输出列："
    echo "    Name           → 锁实例名称（地址或符号名）"
    echo "    acquired       → 获取次数"
    echo "    contended      → 竞争次数（需要等待的次数）"
    echo "    avg wait       → 平均等待时间"
    echo "    total wait     → 总等待时间"
    echo "    max wait       → 最大等待时间"
    echo ""
    echo "  ★ contended / acquired 比值越高 → 锁竞争越严重"
    echo "  ★ 在 I/O 密集场景下，关注以下锁："
    echo "    - q->queue_lock（请求队列锁）"
    echo "    - blk_mq 相关的锁"
    echo "    - RCU 相关（通常不是问题）"
    echo "  ★ 如果 numjobs > 1，锁竞争通常更明显"

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
echo "后续分析建议："
echo "  1. 实验 1：对比 IPC，IPC < 1.0 说明 CPU 在等待（内存/IO stall）"
echo "  2. 实验 2：从 perf report 中找到 top 5 热点函数，追溯调用链"
echo "  3. 实验 3：对比系统级和内核态热点，理解 I/O 路径上的函数分布"
echo "  4. 实验 4：关注 contended 列，找出竞争最严重的锁"
