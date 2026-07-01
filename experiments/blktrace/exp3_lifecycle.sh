#!/bin/bash
# 实验 3：观察完整 I/O 生命周期
# 目标：追踪一个 I/O 从 Q 到 C 的完整路径

set -e

DEVICE="${1:-/dev/sda}"
OUTPUT_DIR="/tmp/blktrace_exp3"
mkdir -p "$OUTPUT_DIR"

echo "=== 实验 3：I/O 完整生命周期 ==="
echo "设备: $DEVICE"
echo ""

# 清理旧数据
rm -f "$OUTPUT_DIR"/*.blktrace.* "$OUTPUT_DIR"/*.txt

# 追踪所有事件类型（包括 notify 事件，可看到 G/I/M 等）
echo "[1/4] 启动 blktrace（全事件类型）..."
blktrace -d "$DEVICE" -o "$OUTPUT_DIR/full" \
    -a queue -a issue -a complete -a requeue -a notify \
    -w 5 &
sleep 1

# 生成简单的顺序读
echo "[2/4] 生成 I/O（dd 读取 100 个 4K 块）..."
dd if="$DEVICE" of=/dev/null bs=4k count=100 2>/dev/null

wait

# 解析
echo "[3/4] 解析输出..."
blkparse -i "$OUTPUT_DIR/full" -o "$OUTPUT_DIR/full_parsed.txt" 2>/dev/null

echo ""
echo "=== 完整事件流（前 50 行）==="
head -50 "$OUTPUT_DIR/full_parsed.txt"

echo ""
echo "=== 事件类型统计 ==="
awk '{print $6}' "$OUTPUT_DIR/full_parsed.txt" | sort | uniq -c | sort -rn

echo ""
echo "[4/4] 完成！"
echo ""
echo "思考题："
echo "  1. 找到同一个 sector 的 Q → G → I → M → D → C 事件序列"
echo "  2. 计算每个阶段的耗时：Q2G、G2I、I2D、D2C"
echo "  3. notify 事件（G/I/M）说明了调度器的什么行为？"
echo "  4. 有没有 requeue 事件？如果有，说明设备拒绝了某个请求"
