#!/bin/bash
# ============================================================
# strace 实战实验脚本
# ============================================================
#
# 实验目的：
#   通过 5 组实验，掌握 strace 的核心用法和系统调用分析能力
#   - 实验 1：strace ls 基础追踪（理解系统调用流程）
#   - 实验 2：strace -c 统计模式（系统调用计数和耗时汇总）
#   - 实验 3：strace -T 耗时标注（每个系统调用的耗时）
#   - 实验 4：strace -e trace=file 文件操作过滤（只看文件相关调用）
#   - 实验 5：strace -f 子进程追踪（追踪 fork/exec 树）
#
# 前置条件：
#   - strace 已安装（大多数发行版自带）
#   - 以 root 身份运行（追踪其他进程时需要）
#
# 用法：
#   bash vdb-lab.sh           # 运行全部实验
#   bash vdb-lab.sh 1         # 只运行实验 1
#   bash vdb-lab.sh 2 3       # 运行实验 2 和 3
# ============================================================

set -euo pipefail

# ─── 配置 ───────────────────────────────────────────────────
OUTPUT_DIR="/tmp/strace_lab"

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
    if ! command -v strace &>/dev/null; then
        echo "错误：strace 未安装"
        echo "  安装：yum install -y strace  或  apt install -y strace"
        exit 1
    fi
    echo "✓ strace 已安装：$(strace -V 2>&1 | head -1)"
}

# ─── 环境准备 ──────────────────────────────────────────────
mkdir -p "$OUTPUT_DIR"
banner "strace 实战实验 — 环境检查"
check_tools
echo ""

# 获取要运行的实验列表
EXPERIMENTS="${@:-1 2 3 4 5}"

# ================================================================
# 实验 1：strace ls 基础追踪
# ================================================================
run_exp1() {
    banner "实验 1：strace ls 基础追踪 — 理解系统调用流程"
    echo "目的：追踪 ls 命令的系统调用，理解一个简单命令的完整生命周期"
    echo ""

    sub_banner "1a: 追踪 ls /tmp（完整输出）"
    echo "命令：strace -o exp1_ls_full.txt ls /tmp"
    echo ""

    strace -o "${OUTPUT_DIR}/exp1_ls_full.txt" ls /tmp 2>&1

    echo "ls 执行完成，strace 输出前 40 行："
    echo "──────────────────────────────────────────"
    head -40 "${OUTPUT_DIR}/exp1_ls_full.txt"
    echo "──────────────────────────────────────────"
    echo ""
    echo "总系统调用数：$(wc -l < "${OUTPUT_DIR}/exp1_ls_full.txt")"

    sub_banner "1a 输出解读"
    echo "  每行格式：syscall_name(args) = return_value"
    echo ""
    echo "  典型流程（从上到下）："
    echo "    1. execve()          → 加载 ls 可执行文件"
    echo "    2. brk() / mmap()    → 初始化堆和内存映射"
    echo "    3. access()          → 检查共享库是否存在"
    echo "    4. openat() / read() → 加载共享库（.so）"
    echo "    5. arch_prctl()      → 设置 TLS（线程本地存储）"
    echo "    6. openat() / getdents64() → ★ 打开目录并读取文件列表"
    echo "    7. write()           → 输出文件名到终端"
    echo "    8. close()           → 关闭文件描述符"
    echo "    9. exit_group()      → 退出进程"

    # --- 1b: 统计系统调用类型 ---
    sub_banner "1b: 系统调用类型分布"
    echo "命令：从 strace 输出中提取系统调用名并统计"
    echo ""

    # 提取系统调用名（行首到第一个括号之间的部分）
    awk -F'[()]' '{print $1}' "${OUTPUT_DIR}/exp1_ls_full.txt" \
        | awk '{print $NF}' \
        | sort | uniq -c | sort -rn \
        | head -20 \
        | tee "${OUTPUT_DIR}/exp1_syscall_dist.txt"

    echo ""
    echo "  ★ ls 命令中 openat/getdents64 最多（读目录内容）"
    echo "  ★ mmap/mprotect 用于加载共享库"
    echo "  ★ fstat/newfstatat 用于获取文件属性（ls -l 时需要）"
}

# ================================================================
# 实验 2：strace -c 统计模式
# ================================================================
run_exp2() {
    banner "实验 2：strace -c 统计模式 — 系统调用计数和耗时汇总"
    echo "目的：使用 -c 快速了解程序的系统调用分布，无需逐行阅读"
    echo ""

    # --- 2a: 对 ls 统计 ---
    sub_banner "2a: strace -c ls /tmp"
    echo "命令：strace -c ls /tmp"
    echo ""

    strace -c -o "${OUTPUT_DIR}/exp2_ls_stats.txt" ls /tmp 2>&1
    echo "统计结果："
    echo "──────────────────────────────────────────"
    cat "${OUTPUT_DIR}/exp2_ls_stats.txt"
    echo "──────────────────────────────────────────"

    sub_banner "2a 输出列解读"
    echo "  % time  → 该系统调用占总耗时的百分比"
    echo "  seconds → 该系统调用的总耗时（秒）"
    echo "  usecs/call → 平均每次调用耗时（微秒）"
    echo "  calls   → 调用次数"
    echo "  errors  → 返回错误的次数"
    echo ""
    echo "  ★ 关注 % time 列：占比最高的调用是性能优化的重点"
    echo "  ★ 关注 errors 列：某些错误是正常的（如 access 返回 ENOENT）"

    # --- 2b: 对更复杂的命令统计 ---
    sub_banner "2b: strace -c find /tmp -maxdepth 1"
    echo "命令：strace -c find /tmp -maxdepth 1"
    echo ""

    strace -c -o "${OUTPUT_DIR}/exp2_find_stats.txt" find /tmp -maxdepth 1 2>/dev/null
    echo "统计结果："
    echo "──────────────────────────────────────────"
    cat "${OUTPUT_DIR}/exp2_find_stats.txt"
    echo "──────────────────────────────────────────"

    sub_banner "2b: ls vs find 对比"
    echo "  ls 主要用 getdents64 读目录"
    echo "  find 除了 getdents64 还会对每个文件调用 stat/lstat"
    echo "  ★ 对比 calls 列可以看出两个命令的工作方式差异"
}

# ================================================================
# 实验 3：strace -T 耗时标注
# ================================================================
run_exp3() {
    banner "实验 3：strace -T 耗时标注 — 找出慢系统调用"
    echo "目的：使用 -T 标注每个系统调用的耗时，快速定位性能瓶颈"
    echo ""

    # --- 3a: 对 ping 追踪（涉及网络，有等待时间） ---
    sub_banner "3a: strace -T ping -c 3 127.0.0.1"
    echo "命令：strace -T -o exp3_ping.txt ping -c 3 127.0.0.1"
    echo ""

    strace -T -o "${OUTPUT_DIR}/exp3_ping.txt" ping -c 3 127.0.0.1 2>&1 || true
    echo "输出（耗时 > 1ms 的调用）："
    echo "──────────────────────────────────────────"
    # 筛选出耗时较长的调用（包含 >0.001 的行）
    grep -E '<[0-9]+\.[0-9]+>' "${OUTPUT_DIR}/exp3_ping.txt" \
        | sort -t'<' -k2 -rn \
        | head -20 \
        | tee "${OUTPUT_DIR}/exp3_slow_calls.txt"
    echo "──────────────────────────────────────────"

    sub_banner "3a 输出格式解读"
    echo "  格式：syscall(args) = ret <耗时>"
    echo "  示例：poll([{fd=5, events=POLLIN}], 1, 1000) = 1 ([{fd=5, revents=POLLIN}]) <0.001023>"
    echo ""
    echo "  <0.001023> 表示这次 poll 耗时 1.023 毫秒"
    echo ""
    echo "  ★ ping 中 poll/select/recvfrom 通常耗时最长（等待网络响应）"
    echo "  ★ sendto 通常很快（只是放入内核缓冲区）"

    # --- 3b: 对文件 I/O 追踪 ---
    sub_banner "3b: strace -T dd if=/dev/zero of=/tmp/strace_test bs=4k count=100"
    echo "命令：strace -T -o exp3_dd.txt dd ..."
    echo ""

    strace -T -o "${OUTPUT_DIR}/exp3_dd.txt" \
        dd if=/dev/zero of=/tmp/strace_test_file bs=4k count=100 2>&1

    echo "dd 系统调用耗时 Top 10："
    echo "──────────────────────────────────────────"
    grep -E '<[0-9]+\.[0-9]+>' "${OUTPUT_DIR}/exp3_dd.txt" \
        | sort -t'<' -k2 -rn \
        | head -10
    echo "──────────────────────────────────────────"
    echo ""

    echo "dd 系统调用统计："
    awk -F'[()]' '{print $1}' "${OUTPUT_DIR}/exp3_dd.txt" \
        | awk '{print $NF}' \
        | sort | uniq -c | sort -rn | head -10

    # 清理测试文件
    rm -f /tmp/strace_test_file

    sub_banner "3b 关键发现"
    echo "  ★ dd 的核心调用是 read（从 /dev/zero）和 write（到文件）"
    echo "  ★ write 的耗时受文件系统缓存影响（首次可能很快，脏页回写时变慢）"
    echo "  ★ 观察 write 耗时的波动，可以理解内核 writeback 机制"
}

# ================================================================
# 实验 4：strace -e trace=file 文件操作过滤
# ================================================================
run_exp4() {
    banner "实验 4：strace -e trace=file — 只看文件相关系统调用"
    echo "目的：使用 -e trace=file 过滤，只关注文件打开/读写/关闭等操作"
    echo ""

    # --- 4a: 追踪 cat /etc/passwd ---
    sub_banner "4a: strace -e trace=file cat /etc/passwd"
    echo "命令：strace -e trace=file -o exp4_cat_file.txt cat /etc/passwd"
    echo ""

    strace -e trace=file -o "${OUTPUT_DIR}/exp4_cat_file.txt" \
        cat /etc/passwd 2>&1

    echo "文件操作追踪结果："
    echo "──────────────────────────────────────────"
    cat "${OUTPUT_DIR}/exp4_cat_file.txt"
    echo "──────────────────────────────────────────"

    sub_banner "4a 解读"
    echo "  -e trace=file 追踪的系统调用包括："
    echo "    open/openat   → 打开文件"
    echo "    close          → 关闭文件"
    echo "    read/write     → 读写文件"
    echo "    stat/lstat/fstat → 获取文件属性"
    echo "    access         → 检查权限"
    echo "    unlink         → 删除文件"
    echo "    rename         → 重命名"
    echo "    link/symlink   → 创建链接"
    echo "    mkdir/rmdir    → 创建/删除目录"
    echo "    chdir          → 切换目录"

    # --- 4b: 追踪更复杂的操作 ---
    sub_banner "4b: strace -e trace=file cp /etc/passwd /tmp/strace_cp_test"
    echo ""

    strace -e trace=file -o "${OUTPUT_DIR}/exp4_cp_file.txt" \
        cp /etc/passwd /tmp/strace_cp_test 2>&1

    echo "cp 的文件操作："
    echo "──────────────────────────────────────────"
    cat "${OUTPUT_DIR}/exp4_cp_file.txt"
    echo "──────────────────────────────────────────"

    # 清理
    rm -f /tmp/strace_cp_test

    sub_banner "4b: cat vs cp 文件操作对比"
    echo "  cat: openat(源文件) → read → write(stdout) → close"
    echo "  cp:  openat(源文件) → openat(目标文件) → read → write → close × 2"
    echo "  ★ cp 多了创建目标文件的操作（openat + O_WRONLY|O_CREAT）"

    # --- 4c: 其他过滤类别 ---
    sub_banner "4c: 其他 -e trace 过滤类别一览"
    echo "  -e trace=file    → 文件操作（open, read, write, stat...）"
    echo "  -e trace=process → 进程操作（fork, exec, exit, wait...）"
    echo "  -e trace=network → 网络操作（socket, bind, connect, send...）"
    echo "  -e trace=signal  → 信号操作（kill, sigaction, sigreturn...）"
    echo "  -e trace=desc    → 文件描述符操作（close, dup, fcntl, select...）"
    echo "  -e trace=memory  → 内存操作（mmap, mprotect, munmap, brk...）"
    echo "  -e trace=open    → 仅 open/openat"
    echo ""
    echo "  示例：只看网络调用"
    echo "    strace -e trace=network curl -s https://example.com"
    echo ""
    echo "  示例：只看进程创建"
    echo "    strace -e trace=process bash -c 'echo hello'"
}

# ================================================================
# 实验 5：strace -f 子进程追踪
# ================================================================
run_exp5() {
    banner "实验 5：strace -f 子进程追踪 — 追踪 fork/exec 树"
    echo "目的：使用 -f 追踪子进程，理解 shell 脚本/命令的多进程行为"
    echo ""

    # --- 5a: 追踪 bash -c（带子进程） ---
    sub_banner "5a: strace -f bash -c 'echo hello; ls /tmp | head -3'"
    echo "命令：strace -f -e trace=process -o exp5_bash_fork.txt bash -c 'echo hello; ls /tmp | head -3'"
    echo ""

    strace -f -e trace=process -o "${OUTPUT_DIR}/exp5_bash_fork.txt" \
        bash -c 'echo hello; ls /tmp | head -3' 2>&1

    echo "子进程追踪结果（只显示进程相关调用）："
    echo "──────────────────────────────────────────"
    cat "${OUTPUT_DIR}/exp5_bash_fork.txt"
    echo "──────────────────────────────────────────"

    sub_banner "5a 解读"
    echo "  PID 列：不同 PID 代表不同进程"
    echo "    - 父进程（bash）: 看到 clone/fork 创建子进程"
    echo "    - 子进程（ls）  : 看到 execve 加载 ls 程序"
    echo "    - 子进程（head）: 看到 execve 加载 head 程序"
    echo ""
    echo "  ★ clone() / fork() → 创建子进程"
    echo "  ★ execve()         → 加载并运行新程序"
    echo "  ★ wait4()          → 等待子进程退出"
    echo "  ★ exit_group()     → 进程退出"

    # --- 5b: 统计子进程数量 ---
    sub_banner "5b: 统计子进程创建和 exec 调用"
    echo ""

    echo "各 PID 的系统调用统计："
    awk -F' ' '{
        # 提取 PID（行首数字或 [pid XXXX] 格式）
        if (match($0, /^\[pid ([0-9]+)\]/, m)) {
            pid = m[1]
        } else if (match($0, /^([0-9]+) /, m)) {
            pid = m[1]
        } else {
            pid = "main"
        }
        # 提取系统调用名
        if (match($0, /([a-z_]+)\(/, m2)) {
            syscall = m2[1]
            count[pid][syscall]++
        }
    }
    END {
        for (pid in count) {
            printf "  PID %s:\n", pid
            for (sc in count[pid]) {
                printf "    %-20s %d 次\n", sc, count[pid][sc]
            }
        }
    }' "${OUTPUT_DIR}/exp5_bash_fork.txt" 2>/dev/null || {
        # 如果 awk 不支持二维数组，用简单方式
        echo "（使用简化统计）"
        echo "clone/fork 调用次数："
        grep -c 'clone\|fork' "${OUTPUT_DIR}/exp5_bash_fork.txt" 2>/dev/null || echo "0"
        echo "execve 调用次数："
        grep -c 'execve' "${OUTPUT_DIR}/exp5_bash_fork.txt" 2>/dev/null || echo "0"
        echo "wait4 调用次数："
        grep -c 'wait4' "${OUTPUT_DIR}/exp5_bash_fork.txt" 2>/dev/null || echo "0"
    }

    # --- 5c: 追踪 make（多进程典型场景） ---
    sub_banner "5c: strace -f -c make --version（统计模式 + 子进程追踪）"
    echo ""

    if command -v make &>/dev/null; then
        strace -f -c -o "${OUTPUT_DIR}/exp5_make_stats.txt" make --version 2>&1 || true
        echo "make --version 系统调用统计（含子进程）："
        echo "──────────────────────────────────────────"
        cat "${OUTPUT_DIR}/exp5_make_stats.txt"
        echo "──────────────────────────────────────────"
    else
        echo "make 未安装，使用替代命令"
        strace -f -c -o "${OUTPUT_DIR}/exp5_grep_stats.txt" \
            grep -r "root" /etc/passwd 2>&1 || true
        echo "──────────────────────────────────────────"
        cat "${OUTPUT_DIR}/exp5_grep_stats.txt"
        echo "──────────────────────────────────────────"
    fi

    sub_banner "5c 关键发现"
    echo "  ★ -f 和 -c 组合使用可以快速了解多进程程序的整体行为"
    echo "  ★ make 会 fork 大量子进程执行编译命令"
    echo "  ★ 统计中 clone/vfork/fork 的 calls 数 = 创建的子进程数"
    echo "  ★ execve 的 calls 数 = 实际执行的程序数"
    echo ""
    echo "  ★ 不带 -f 时，strace 只追踪主进程"
    echo "  ★ 带 -f 后，所有 clone/fork 出来的子进程都会被追踪"
    echo "  ★ -ff 选项可以为每个子进程生成单独的文件（PID 后缀）"
}

# ─── 运行实验 ──────────────────────────────────────────────
for exp in $EXPERIMENTS; do
    case "$exp" in
        1) run_exp1 ;;
        2) run_exp2 ;;
        3) run_exp3 ;;
        4) run_exp4 ;;
        5) run_exp5 ;;
        *) echo "未知实验编号：$exp（有效范围：1-5）" ;;
    esac
done

banner "全部实验完成！"
echo "输出目录：$OUTPUT_DIR"
echo ""
echo "文件清单："
ls -lh "$OUTPUT_DIR"/* 2>/dev/null || echo "  （无输出文件）"
echo ""
echo "后续分析建议："
echo "  1. 实验 1：阅读 ls 的完整 strace 输出，标记每个阶段的系统调用"
echo "  2. 实验 2：对比 ls 和 find 的 % time 列，理解两者的性能差异"
echo "  3. 实验 3：从 -T 输出中找出最慢的 5 个系统调用"
echo "  4. 实验 4：用 -e trace=network 追踪 curl/wget 的网络调用序列"
echo "  5. 实验 5：用 -ff 选项追踪 make，观察每个子进程的独立输出文件"
