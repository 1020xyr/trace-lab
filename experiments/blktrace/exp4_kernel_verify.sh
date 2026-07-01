#!/bin/bash
# 实验 4：验证内核 tracepoint
# 目标：通过 ftrace 验证 blktrace 使用的内核 tracepoint

set -e

DEBUGFS="/sys/kernel/debug/tracing"

echo "=== 实验 4：验证内核 tracepoint ==="
echo ""

# 检查 debugfs 是否挂载
if [ ! -d "$DEBUGFS" ]; then
    echo "错误: debugfs 未挂载在 $DEBUGFS"
    echo "尝试: mount -t debugfs none /sys/kernel/debug"
    exit 1
fi

# 1. 列出所有 block 相关的 tracepoint
echo "=== 1. 可用的 block tracepoint ==="
grep "^block:" "$DEBUGFS/available_events" | sort
echo ""

# 2. 启用 block_rq_issue 和 block_rq_complete
echo "=== 2. 启用 tracepoint ==="
echo 1 > "$DEBUGFS/events/block/block_rq_issue/enable"
echo "  已启用: block_rq_issue"
echo 1 > "$DEBUGFS/events/block/block_rq_complete/enable"
echo "  已启用: block_rq_complete"
echo ""

# 3. 清空 trace buffer
echo > "$DEBUGFS/trace"

# 4. 生成 I/O
echo "=== 3. 生成 I/O ==="
dd if=/dev/sda of=/dev/null bs=4k count=10 2>/dev/null
echo "  完成 10 次 4K 读"
echo ""

# 5. 查看 ftrace 输出
echo "=== 4. ftrace 输出（前 20 行）==="
head -20 "$DEBUGFS/trace"
echo ""

# 6. 对比：这就是 blktrace 在内核中 hook 的位置！
echo "=== 5. tracepoint 信息解析 ==="
echo "block_rq_issue 的格式："
cat "$DEBUGFS/events/block/block_rq_issue/format" 2>/dev/null | head -20
echo ""
echo "block_rq_complete 的格式："
cat "$DEBUGFS/events/block/block_rq_complete/format" 2>/dev/null | head -20

# 7. 清理
echo ""
echo "=== 6. 清理 ==="
echo 0 > "$DEBUGFS/events/block/block_rq_issue/enable"
echo 0 > "$DEBUGFS/events/block/block_rq_complete/enable"
echo "  已关闭所有 block tracepoint"

echo ""
echo "思考题："
echo "  1. ftrace 输出的 tracepoint 事件和 blkparse 输出的事件是否一致？"
echo "  2. block_rq_issue 的 format 文件中有哪些字段？对比 blk_io_trace 结构"
echo "  3. ftrace 和 blktrace 使用相同的 tracepoint，但数据采集机制不同"
echo "     ftrace 用 ring_buffer，blktrace 用 relay buffer"
