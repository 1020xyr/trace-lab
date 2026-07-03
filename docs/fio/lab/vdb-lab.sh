#!/bin/bash
# ============================================================
# fio 实战实验脚本
# ============================================================
#
# 实验目的：
#   通过 4 组对比实验，深入理解 fio 的核心参数对 I/O 性能的影响
#   - 实验 1：同步引擎 vs 异步引擎（sync vs libaio）
#   - 实验 2：iodepth 梯度测试（队列深度对 IOPS 的影响）
#   - 实验 3：顺序读 vs 随机读（访问模式对吞吐的影响）
#   - 实验 4：fio + blktrace 联动（从 block 层视角验证 fio 报告）
#
# 前置条件：
#   - yum install -y fio
#   - blktrace 已编译（实验 4 需要）：cd src/blktrace && make
#   - 测试设备：/dev/vdb（40G virtio-blk，无分区无文件系统）
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
OUTPUT_DIR="/tmp/fio_lab"
BLKTRACE_BIN="/root/code/tool_learn/src/blktrace/blktrace"
BLKPARSE_BIN="/root/code/tool_learn/src/blktrace/blkparse"
BTT_BIN="/root/code/tool_learn/src/blktrace/btt/btt"

# 默认运行时间（秒）
RUNTIME=10

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

check_device() {
    if [ ! -b "$DEVICE" ]; then
        echo "错误：设备 $DEVICE 不存在或不是块设备"
        echo "请先确认设备：lsblk $DEVICE"
        exit 1
    fi
    echo "✓ 设备 $DEVICE 就绪"
    lsblk "$DEVICE" | tail -1
    echo ""
}

check_tools() {
    for tool in fio; do
        if ! command -v "$tool" &>/dev/null; then
            echo "错误：$tool 未安装，请执行 yum install -y $tool"
            exit 1
        fi
    done
    echo "✓ fio 已安装：$(fio --version)"
}

# 从 fio JSON 输出中提取关键指标
# 用法：extract_fio_metric <json_file> <section: read|write> <metric>
extract_fio_json() {
    local json_file="$1"
    # 使用 python 解析 JSON（大多数系统自带 python）
    python3 -c "
import json, sys
with open('$json_file') as f:
    data = json.load(f)
job = data['jobs'][0]
print(json.dumps(job, indent=2))
" 2>/dev/null || echo "（python3 不可用，跳过 JSON 解析）"
}

# ─── 环境准备 ──────────────────────────────────────────────
mkdir -p "$OUTPUT_DIR"
banner "fio 实战实验 — 环境检查"
check_device
check_tools

# 获取要运行的实验列表
EXPERIMENTS="${@:-1 2 3 4}"

# ================================================================
# 实验 1：同步引擎 vs 异步引擎（sync vs libaio）
# ================================================================
run_exp1() {
    banner "实验 1：同步引擎 vs 异步引擎（sync vs libaio）"
    echo "目的：对比 sync（每次 I/O 等待完成）和 libaio（异步批量提交）的 IOPS 差异"
    echo "参数：4K 随机写，direct=1，iodepth=1（sync）/ iodepth=32（libaio）"
    echo ""

    # --- 1a: sync 引擎 ---
    sub_banner "1a: sync 引擎（iodepth=1，每次写等待完成）"
    fio --name=sync_test \
        --ioengine=sync \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime="$RUNTIME" \
        --time_based \
        --iodepth=1 \
        --output-format=json \
        --output="${OUTPUT_DIR}/exp1_sync.json" \
        --group_reporting

    echo ""
    echo "sync 引擎结果："
    python3 -c "
import json
with open('${OUTPUT_DIR}/exp1_sync.json') as f:
    data = json.load(f)
w = data['jobs'][0]['write']
print(f\"  IOPS:     {w['iops']:.1f}\")
print(f\"  BW:       {w['bw']/1024:.1f} MiB/s\")
print(f\"  lat avg:  {w['lat_ns']['mean']/1000:.1f} us\")
print(f\"  slat avg: {w['slat_ns']['mean']/1000:.1f} us\")
print(f\"  clat avg: {w['clat_ns']['mean']/1000:.1f} us\")
" 2>/dev/null || echo "（请手动查看 ${OUTPUT_DIR}/exp1_sync.json）"

    # --- 1b: libaio 引擎 ---
    sub_banner "1b: libaio 引擎（iodepth=32，异步批量提交）"
    fio --name=libaio_test \
        --ioengine=libaio \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime="$RUNTIME" \
        --time_based \
        --iodepth=32 \
        --output-format=json \
        --output="${OUTPUT_DIR}/exp1_libaio.json" \
        --group_reporting

    echo ""
    echo "libaio 引擎结果："
    python3 -c "
import json
with open('${OUTPUT_DIR}/exp1_libaio.json') as f:
    data = json.load(f)
w = data['jobs'][0]['write']
print(f\"  IOPS:     {w['iops']:.1f}\")
print(f\"  BW:       {w['bw']/1024:.1f} MiB/s\")
print(f\"  lat avg:  {w['lat_ns']['mean']/1000:.1f} us\")
print(f\"  slat avg: {w['slat_ns']['mean']/1000:.1f} us\")
print(f\"  clat avg: {w['clat_ns']['mean']/1000:.1f} us\")
" 2>/dev/null || echo "（请手动查看 ${OUTPUT_DIR}/exp1_libaio.json）"

    # --- 对比汇总 ---
    sub_banner "实验 1 对比汇总"
    python3 -c "
import json

with open('${OUTPUT_DIR}/exp1_sync.json') as f:
    sync = json.load(f)['jobs'][0]['write']
with open('${OUTPUT_DIR}/exp1_libaio.json') as f:
    aio = json.load(f)['jobs'][0]['write']

print(f'{\"指标\":<16} {\"sync(iodepth=1)\":>20} {\"libaio(iodepth=32)\":>20} {\"倍数\":>8}')
print('-' * 68)
print(f'{\"IOPS\":<16} {sync[\"iops\"]:>20.1f} {aio[\"iops\"]:>20.1f} {aio[\"iops\"]/max(sync[\"iops\"],1):>8.1f}x')
print(f'{\"BW (MiB/s)\":<16} {sync[\"bw\"]/1024:>20.1f} {aio[\"bw\"]/1024:>20.1f} {aio[\"bw\"]/max(sync[\"bw\"],1):>8.1f}x')
print(f'{\"lat avg (us)\":<16} {sync[\"lat_ns\"][\"mean\"]/1000:>20.1f} {aio[\"lat_ns\"][\"mean\"]/1000:>20.1f}')
print(f'{\"slat avg (us)\":<16} {sync[\"slat_ns\"][\"mean\"]/1000:>20.1f} {aio[\"slat_ns\"][\"mean\"]/1000:>20.1f}')
print(f'{\"clat avg (us)\":<16} {sync[\"clat_ns\"][\"mean\"]/1000:>20.1f} {aio[\"clat_ns\"][\"mean\"]/1000:>20.1f}')
print()
print('★ 关键发现：')
print('  - sync 引擎每次 I/O 都要等完成才能提交下一个，IOPS 受限于单次延迟')
print('  - libaio 引擎批量提交，IOPS 远高于 sync（pipeline 效应）')
print('  - libaio 的 clat（完成延迟）更高，因为请求在队列中等待')
print('  - libaio 的 slat（提交延迟）极低，因为 io_submit 只是入队')
" 2>/dev/null || echo "（python3 不可用，请手动对比两个 JSON 文件）"
}

# ================================================================
# 实验 2：iodepth 梯度测试
# ================================================================
run_exp2() {
    banner "实验 2：iodepth 梯度测试（1/4/16/32/64）"
    echo "目的：观察队列深度（iodepth）对 IOPS 和延迟的影响"
    echo "参数：4K 随机写，libaio 引擎，direct=1"
    echo ""

    DEPTHS=(1 4 16 32 64)
    JSON_FILES=()

    for depth in "${DEPTHS[@]}"; do
        sub_banner "iodepth=$depth"
        outfile="${OUTPUT_DIR}/exp2_depth${depth}.json"
        JSON_FILES+=("$outfile")

        fio --name="depth${depth}" \
            --ioengine=libaio \
            --direct=1 \
            --rw=randwrite \
            --bs=4k \
            --numjobs=1 \
            --filename="$DEVICE" \
            --runtime="$RUNTIME" \
            --time_based \
            --iodepth="$depth" \
            --output-format=json \
            --output="$outfile" \
            --group_reporting

        echo "  iodepth=$depth 完成"
    done

    # --- 汇总表格 ---
    sub_banner "实验 2 汇总表格"
    python3 -c "
import json

depths = [1, 4, 16, 32, 64]
print(f'{\"iodepth\":>8} {\"IOPS\":>10} {\"BW(MiB/s)\":>12} {\"slat(us)\":>10} {\"clat(us)\":>10} {\"lat(us)\":>10} {\"p99 clat(us)\":>14}')
print('-' * 80)

for d in depths:
    with open('${OUTPUT_DIR}/exp2_depth{}.json'.format(d)) as f:
        w = json.load(f)['jobs'][0]['write']

    # p99 clat
    p99 = 0
    for pct_str, val in w['clat_ns']['percentile'].items():
        pct = float(pct_str)
        if abs(pct - 99.0) < 0.5 or abs(pct - 99.00) < 0.01:
            p99 = val / 1000
            break
    if p99 == 0:
        # 取最接近 99% 的值
        p99 = list(w['clat_ns']['percentile'].values())[-2] / 1000 if w['clat_ns']['percentile'] else 0

    print(f'{d:>8} {w[\"iops\"]:>10.1f} {w[\"bw\"]/1024:>12.1f} {w[\"slat_ns\"][\"mean\"]/1000:>10.1f} {w[\"clat_ns\"][\"mean\"]/1000:>10.1f} {w[\"lat_ns\"][\"mean\"]/1000:>10.1f} {p99:>14.1f}')

print()
print('★ 关键发现：')
print('  - iodepth=1 时 IOPS 最低（无法利用异步优势）')
print('  - 随 iodepth 增大，IOPS 先快速增长后趋于饱和（设备达到上限）')
print('  - clat 随 iodepth 线性增长（请求在队列中排队等待）')
print('  - 存在一个最优 iodepth，超过后 IOPS 增长停滞但延迟持续增加')
" 2>/dev/null || echo "（python3 不可用，请手动查看 ${OUTPUT_DIR}/exp2_depth*.json）"
}

# ================================================================
# 实验 3：顺序读 vs 随机读对比（128K bs）
# ================================================================
run_exp3() {
    banner "实验 3：顺序读 vs 随机读对比（128K bs）"
    echo "目的：对比顺序和随机访问模式对吞吐量和 IOPS 的影响"
    echo "参数：128K 块大小，libaio 引擎，direct=1，iodepth=16"
    echo ""

    # --- 3a: 顺序读 ---
    sub_banner "3a: 顺序读（128K sequential read）"
    fio --name=seq_read \
        --ioengine=libaio \
        --direct=1 \
        --rw=read \
        --bs=128k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime="$RUNTIME" \
        --time_based \
        --iodepth=16 \
        --output-format=json \
        --output="${OUTPUT_DIR}/exp3_seq_read.json" \
        --group_reporting

    echo "  顺序读完成"

    # --- 3b: 随机读 ---
    sub_banner "3b: 随机读（128K random read）"
    fio --name=rand_read \
        --ioengine=libaio \
        --direct=1 \
        --rw=randread \
        --bs=128k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime="$RUNTIME" \
        --time_based \
        --iodepth=16 \
        --output-format=json \
        --output="${OUTPUT_DIR}/exp3_rand_read.json" \
        --group_reporting

    echo "  随机读完成"

    # --- 对比汇总 ---
    sub_banner "实验 3 对比汇总"
    python3 -c "
import json

with open('${OUTPUT_DIR}/exp3_seq_read.json') as f:
    seq = json.load(f)['jobs'][0]['read']
with open('${OUTPUT_DIR}/exp3_rand_read.json') as f:
    rand = json.load(f)['jobs'][0]['read']

print(f'{\"指标\":<16} {\"顺序读(seq)\":>20} {\"随机读(rand)\":>20} {\"倍数\":>8}')
print('-' * 68)
print(f'{\"IOPS\":<16} {seq[\"iops\"]:>20.1f} {rand[\"iops\"]:>20.1f} {seq[\"iops\"]/max(rand[\"iops\"],1):>8.1f}x')
print(f'{\"BW (MiB/s)\":<16} {seq[\"bw\"]/1024:>20.1f} {rand[\"bw\"]/1024:>20.1f} {seq[\"bw\"]/max(rand[\"bw\"],1):>8.1f}x')
print(f'{\"lat avg (us)\":<16} {seq[\"lat_ns\"][\"mean\"]/1000:>20.1f} {rand[\"lat_ns\"][\"mean\"]/1000:>20.1f}')
print(f'{\"clat avg (us)\":<16} {seq[\"clat_ns\"][\"mean\"]/1000:>20.1f} {rand[\"clat_ns\"][\"mean\"]/1000:>20.1f}')
print()
print('★ 关键发现：')
print('  - 顺序读吞吐（BW）远高于随机读 — 磁盘/virtio 对连续访问有优化')
print('  - 顺序读延迟更低 — 无需寻址/切换位置')
print('  - 128K 大块 I/O 下 IOPS 数值较小，但 BW 更有参考意义')
print('  - 随机读的 clat 更高 — 设备需要处理不连续的扇区')
" 2>/dev/null || echo "（python3 不可用，请手动对比两个 JSON 文件）"
}

# ================================================================
# 实验 4：fio + blktrace 联动
# ================================================================
run_exp4() {
    banner "实验 4：fio + blktrace 联动分析"
    echo "目的：同时运行 fio 和 blktrace，对比 fio 报告的 slat/clat 与 blktrace 的 Q2D/D2C"
    echo "参数：4K 随机写，libaio 引擎，direct=1，iodepth=32"
    echo ""

    # 检查 blktrace 工具
    for bin in "$BLKTRACE_BIN" "$BLKPARSE_BIN" "$BTT_BIN"; do
        if [ ! -x "$bin" ]; then
            echo "警告：$bin 不存在或不可执行"
            echo "  请先编译 blktrace：cd /root/code/tool_learn/src/blktrace && make"
            echo "  跳过实验 4"
            return
        fi
    done
    echo "✓ blktrace 工具就绪"
    echo ""

    TRACE_PREFIX="${OUTPUT_DIR}/exp4_trace"
    # 清理旧的 trace 文件
    rm -f "${TRACE_PREFIX}".blktrace.* "${TRACE_PREFIX}"*.txt "${TRACE_PREFIX}"*.bin

    # 启动 blktrace（追踪 /dev/vdb，比 fio 多 2 秒以确保捕获所有事件）
    echo "[1/5] 启动 blktrace（$(( RUNTIME + 2 )) 秒）..."
    "$BLKTRACE_BIN" -d "$DEVICE" -o "$TRACE_PREFIX" -w $(( RUNTIME + 2 )) &
    BLKTRACE_PID=$!
    sleep 1

    # 运行 fio
    echo "[2/5] 运行 fio（${RUNTIME} 秒）..."
    fio --name=blktrace_test \
        --ioengine=libaio \
        --direct=1 \
        --rw=randwrite \
        --bs=4k \
        --numjobs=1 \
        --filename="$DEVICE" \
        --runtime="$RUNTIME" \
        --time_based \
        --iodepth=32 \
        --output-format=json \
        --output="${OUTPUT_DIR}/exp4_fio.json" \
        --group_reporting

    # 等待 blktrace 完成
    echo "[3/5] 等待 blktrace 完成..."
    wait "$BLKTRACE_PID" 2>/dev/null || true

    # 解析 blktrace
    echo "[4/5] blkparse 解析 + btt 分析..."
    "$BLKPARSE_BIN" -i "$TRACE_PREFIX" -o "${TRACE_PREFIX}_parsed.txt" 2>/dev/null
    "$BLKPARSE_BIN" -i "$TRACE_PREFIX" -O -d "${TRACE_PREFIX}.bin" 2>/dev/null
    "$BTT_BIN" -i "${TRACE_PREFIX}.bin" -o "${TRACE_PREFIX}_btt" 2>/dev/null

    # --- 对比分析 ---
    echo "[5/5] 对比分析"
    echo ""

    sub_banner "fio 报告的延迟指标"
    python3 -c "
import json
with open('${OUTPUT_DIR}/exp4_fio.json') as f:
    w = json.load(f)['jobs'][0]['write']
print(f'  IOPS:     {w[\"iops\"]:.1f}')
print(f'  slat avg: {w[\"slat_ns\"][\"mean\"]/1000:.1f} us  (提交延迟：io_submit 耗时)')
print(f'  clat avg: {w[\"clat_ns\"][\"mean\"]/1000:.1f} us  (完成延迟：从提交到完成)')
print(f'  lat avg:  {w[\"lat_ns\"][\"mean\"]/1000:.1f} us  (总延迟：slat + clat)')
" 2>/dev/null || echo "（python3 不可用）"

    sub_banner "blktrace btt 报告的延迟指标"
    if [ -f "${TRACE_PREFIX}_btt.avg" ]; then
        cat "${TRACE_PREFIX}_btt.avg"
        echo ""
        echo "  Q2G = 等待 request 分配（对应 fio 的部分 slat）"
        echo "  G2I = 插入调度器（微秒级，几乎可忽略）"
        echo "  I2D = 调度器排队等待"
        echo "  D2C = 设备处理时间（从下发到完成）"
        echo ""
        echo "  ★ Q2D ≈ fio 的 slat + clat 中「在 block 层排队」的部分"
        echo "  ★ D2C ≈ 纯设备处理时间"
        echo "  ★ fio 的 clat ≈ Q2D + D2C（从 fio 提交到 fio 看到完成）"
    else
        echo "  btt 输出文件不存在，请检查 blktrace 是否正常运行"
    fi

    sub_banner "blktrace 事件统计"
    if [ -f "${TRACE_PREFIX}_parsed.txt" ]; then
        echo "事件类型分布："
        awk '{print $6}' "${TRACE_PREFIX}_parsed.txt" | sort | uniq -c | sort -rn
        echo ""
        echo "rwbs 字段分布："
        awk '{print $7}' "${TRACE_PREFIX}_parsed.txt" | sort | uniq -c | sort -rn
    else
        echo "  blkparse 输出文件不存在"
    fi

    sub_banner "fio slat vs blktrace Q2D 对比解读"
    echo "  fio 的 slat（submit latency）= 调用 io_submit 到返回的时间"
    echo "    → 包含：用户态 → 内核态切换 + request 分配 + 插入调度器"
    echo "    → 对应 blktrace 的 Q2G + G2I"
    echo ""
    echo "  fio 的 clat（complete latency）= 从 io_submit 返回到 io_getevents 收到完成"
    echo "    → 包含：调度器排队 + 设备处理 + 中断回调"
    echo "    → 对应 blktrace 的 I2D + D2C"
    echo ""
    echo "  ★ 如果 slat 远小于 Q2G → 说明 fio 的 slat 没有包含完整的排队时间"
    echo "  ★ 如果 clat ≈ I2D + D2C → 验证了 clat 主要是排队 + 设备时间"
    echo ""
    echo "  数据文件保存在：${OUTPUT_DIR}/exp4_*"
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
ls -lh "$OUTPUT_DIR"/*.json "$OUTPUT_DIR"/*.txt "$OUTPUT_DIR"/*.bin 2>/dev/null || echo "  （无输出文件）"
echo ""
echo "后续分析建议："
echo "  1. 对比实验 1 的 sync/libaio JSON，理解异步 I/O 的 pipeline 效应"
echo "  2. 在实验 2 的表格中找到 IOPS 饱和点（拐点）"
echo "  3. 对比实验 3 的 BW，理解顺序 vs 随机的本质差异"
echo "  4. 在实验 4 中验证 fio 报告的 slat/clat 与 blktrace 的 Q2D/D2C 的对应关系"
