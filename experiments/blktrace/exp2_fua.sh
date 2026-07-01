#!/bin/bash
# 实验 2：FUA vs 非 FUA 对比实验
# 目标：观察 FUA 标志对 D2C 延迟的影响

set -e

DEVICE="${1:-/dev/sda}"
OUTPUT_DIR="/tmp/blktrace_exp2"
mkdir -p "$OUTPUT_DIR"

echo "=== 实验 2：FUA vs 非 FUA 对比 ==="
echo "设备: $DEVICE"
echo ""

# 清理旧数据
rm -f "$OUTPUT_DIR"/*.blktrace.* "$OUTPUT_DIR"/*.txt "$OUTPUT_DIR"/*.bin

# --- 测试 1: 普通 direct write（不带 FUA）---
echo "=== 测试 1: 普通 direct write ==="
blktrace -d "$DEVICE" -o "$OUTPUT_DIR/nofua" -w 10 &
sleep 1

fio --name=nofua \
    --ioengine=libaio \
    --direct=1 \
    --rw=randwrite \
    --bs=4k \
    --numjobs=1 \
    --size=50M \
    --filename="${OUTPUT_DIR}/test_nofua" \
    --runtime=8 \
    --time_based \
    --output="${OUTPUT_DIR}/nofua_fio.txt"

wait
blkparse -i "$OUTPUT_DIR/nofua" -o "$OUTPUT_DIR/nofua_parsed.txt" 2>/dev/null
blkparse -i "$OUTPUT_DIR/nofua" -d "$OUTPUT_DIR/nofua.bin" 2>/dev/null
btt -i "$OUTPUT_DIR/nofua.bin" -o "$OUTPUT_DIR/nofua_btt.txt" 2>/dev/null

# --- 测试 2: 带 FUA 的 write ---
echo ""
echo "=== 测试 2: 带 FUA 的 write（--dsync=1）==="
blktrace -d "$DEVICE" -o "$OUTPUT_DIR/fua" -w 10 &
sleep 1

fio --name=fua \
    --ioengine=libaio \
    --direct=1 \
    --rw=randwrite \
    --bs=4k \
    --numjobs=1 \
    --size=50M \
    --filename="${OUTPUT_DIR}/test_fua" \
    --runtime=8 \
    --time_based \
    --dsync=1 \
    --output="${OUTPUT_DIR}/fua_fio.txt"

wait
blkparse -i "$OUTPUT_DIR/fua" -o "$OUTPUT_DIR/fua_parsed.txt" 2>/dev/null
blkparse -i "$OUTPUT_DIR/fua" -d "$OUTPUT_DIR/fua.bin" 2>/dev/null
btt -i "$OUTPUT_DIR/fua.bin" -o "$OUTPUT_DIR/fua_btt.txt" 2>/dev/null

# --- 对比结果 ---
echo ""
echo "============================================"
echo "=== 对比结果 ==="
echo "============================================"
echo ""
echo "--- 非 FUA D2C ---"
grep -A2 "D2C" "$OUTPUT_DIR/nofua_btt.txt" || echo "(无数据)"
echo ""
echo "--- FUA D2C ---"
grep -A2 "D2C" "$OUTPUT_DIR/fua_btt.txt" || echo "(无数据)"
echo ""
echo "--- 非 FUA Q2C ---"
grep -A2 "Q2C" "$OUTPUT_DIR/nofua_btt.txt" || echo "(无数据)"
echo ""
echo "--- FUA Q2C ---"
grep -A2 "Q2C" "$OUTPUT_DIR/fua_btt.txt" || echo "(无数据)"
echo ""

# 检查 FUA 事件
echo "--- FUA 事件检查（nofua）---"
grep -c "F" "$OUTPUT_DIR/nofua_parsed.txt" || echo "0 个 FUA 事件"
echo "--- FUA 事件检查（fua）---"
grep -c "F" "$OUTPUT_DIR/fua_parsed.txt" || echo "0 个 FUA 事件"

echo ""
echo "思考题："
echo "  1. FUA 写和普通写的 D2C 差异有多大？"
echo "  2. 如果差异很大，说明你的设备写缓存对性能影响显著"
echo "  3. 检查 FUA 事件中 rwbs 字段的 'F' 标志"
