#!/bin/bash
# 实验 1：blktrace 基础采集与解析
# 目标：体验完整的 blktrace → blkparse → btt 流程

set -e

DEVICE="${1:-/dev/sda}"
OUTPUT_DIR="/tmp/blktrace_exp1"
mkdir -p "$OUTPUT_DIR"

echo "=== 实验 1：blktrace 基础采集与解析 ==="
echo "设备: $DEVICE"
echo "输出目录: $OUTPUT_DIR"
echo ""

# 清理旧数据
rm -f "$OUTPUT_DIR"/*.blktrace.* "$OUTPUT_DIR"/*.txt "$OUTPUT_DIR"/*.bin

# 1. 启动 blktrace（追踪 10 秒）
echo "[1/7] 启动 blktrace（10 秒）..."
blktrace -d "$DEVICE" -o "$OUTPUT_DIR/trace" -w 10 &
TRACE_PID=$!
sleep 1

# 2. 用 fio 生成 I/O 负载
echo "[2/7] 生成 I/O 负载（fio randwrite 8秒）..."
fio --name=test \
    --ioengine=libaio \
    --direct=1 \
    --rw=randwrite \
    --bs=4k \
    --numjobs=1 \
    --size=100M \
    --filename="${OUTPUT_DIR}/testfile" \
    --runtime=8 \
    --time_based \
    --output="${OUTPUT_DIR}/fio_output.txt"

# 3. 等待 blktrace 结束
echo "[3/7] 等待 blktrace 完成..."
wait $TRACE_PID 2>/dev/null || true

# 4. 解析为文本
echo "[4/7] blkparse 解析为文本..."
blkparse -i "$OUTPUT_DIR/trace" -o "$OUTPUT_DIR/parsed.txt" 2>/dev/null

# 5. 显示前 20 行
echo ""
echo "=== blkparse 输出（前 20 行） ==="
head -20 "$OUTPUT_DIR/parsed.txt"
echo ""

# 6. 生成二进制文件
echo "[5/7] 生成二进制文件供 btt 分析..."
blkparse -i "$OUTPUT_DIR/trace" -d "$OUTPUT_DIR/trace.bin" 2>/dev/null

# 7. btt 分析
echo "[6/7] btt 延迟分析..."
btt -i "$OUTPUT_DIR/trace.bin" -o "$OUTPUT_DIR/btt_summary.txt" 2>/dev/null

echo ""
echo "=== btt 分析结果 ==="
cat "$OUTPUT_DIR/btt_summary.txt"

echo ""
echo "[7/7] 完成！"
echo "所有输出文件在: $OUTPUT_DIR"
echo "  - parsed.txt     (blkparse 格式化文本)"
echo "  - trace.bin      (二进制，供 btt 使用)"
echo "  - btt_summary.txt (延迟统计)"
echo ""
echo "思考题："
echo "  1. 在 parsed.txt 中找到 Q、D、C 事件，计算某个 I/O 的 Q2C 延迟"
echo "  2. 在 btt_summary.txt 中对比 Q2D 和 D2C 的比例，瓶颈在哪？"
