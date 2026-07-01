/*
 * ======================================================================
 * Step 9: blk_fill_rwbs() — rwbs 字段的生成与解析
 * ======================================================================
 *
 * rwbs 是 blkparse 输出中紧跟在事件字母后面的字段，例如：
 *
 *   8,0  1  1  0.001234  1234  D  WS  12345678 + 8 [fio]
 *                            ^^^^
 *                            rwbs = "WS" = Write + Sync
 *
 * 本文件包含两部分：
 *   Part A: 内核 blk_fill_rwbs() — 在 tracepoint 触发时生成 rwbs（从 bio/rq 的 op_flags）
 *   Part B: 用户态 fill_rwbs()  — 在 blkparse 输出时生成 rwbs（从 blk_io_trace 的 action）
 *
 * 两者逻辑相同，但数据来源不同：
 *   内核版：从 bio->bi_opf 或 rq->cmd_flags 直接读取
 *   用户态版：从 blk_io_trace->action 的高 16 位（BLK_TC_* 标志）间接读取
 *
 * ======================================================================
 *
 * ★ 核心问题：N 代表什么？
 *
 *   N = None（无操作类型 / 未识别的操作）
 *
 *   当 I/O 操作不是 READ / WRITE / DISCARD / FLUSH / SECURE_ERASE 中的任何一种时，
 *   rwbs 的第一个字符就是 'N'。
 *
 *   触发 N 的常见场景：
 *   1. REQ_OP_WRITE_ZEROES — 写零（NVMe Write Zeroes）
 *   2. REQ_OP_ZONE_OPEN/CLOSE/FINISH/RESET — Zoned Block Device 操作
 *   3. REQ_OP_ZONE_APPEND — 区域追加写
 *   4. 其他内核内部使用的特殊操作码
 *
 *   换句话说：N = "这是一个 I/O 请求，但它的操作类型不在常规分类中"
 *
 * ======================================================================
 *
 * 原始路径：
 *   内核版：src/linux-5.10/kernel/trace/blktrace.c (第 1959-1999 行)
 *   用户态：src/blktrace/blkparse_fmt.c (第 53-86 行)
 *   REQ_OP 定义：src/linux-5.10/include/linux/blk_types.h
 *
 * 预计阅读时间：15 分钟
 * ======================================================================
 */


/* ======================================================================
 * 前置知识：REQ_OP_* 操作码（bio->bi_opf / rq->cmd_flags 中的低 8 位）
 * ======================================================================
 *
 * 这些值定义在 src/linux-5.10/include/linux/blk_types.h 中。
 * 每个 bio 或 request 的 opf/cmd_flags 的低 8 位存储操作码。
 *
 *   操作码               │ 值 │ rwbs 字符 │ 含义
 *   ─────────────────────┼────┼──────────┼────────────────────────────
 *   REQ_OP_READ          │  0 │ R        │ 读
 *   REQ_OP_WRITE         │  1 │ W        │ 写
 *   REQ_OP_FLUSH         │  2 │ F        │ flush（刷写缓存）
 *   REQ_OP_DISCARD       │  3 │ D        │ discard/TRIM
 *   (保留)               │  4 │          │
 *   REQ_OP_SECURE_ERASE  │  5 │ DE       │ 安全擦除（discard + erase）
 *   (保留)               │  6 │          │
 *   REQ_OP_WRITE_SAME    │  7 │ W        │ 写同一数据到多个扇区
 *   (保留)               │  8 │          │
 *   REQ_OP_WRITE_ZEROES  │  9 │ ★ N      │ 写零（NVMe Write Zeroes）
 *   REQ_OP_ZONE_OPEN     │ 10 │ ★ N      │ 打开 zone（ZBD）
 *   REQ_OP_ZONE_CLOSE    │ 11 │ ★ N      │ 关闭 zone（ZBD）
 *   REQ_OP_ZONE_FINISH   │ 12 │ ★ N      │ 完成 zone（ZBD）
 *   REQ_OP_ZONE_APPEND   │ 13 │ ★ N      │ zone 追加写（ZBD）
 *   (保留)               │ 14 │          │
 *   REQ_OP_ZONE_RESET    │ 15 │ ★ N      │ 重置 zone（ZBD）
 *   (保留)               │ 16 │          │
 *   REQ_OP_ZONE_RESET_ALL│ 17 │ ★ N      │ 重置所有 zone（ZBD）
 *
 *   低 8 位的掩码：REQ_OP_MASK = (1 << REQ_OP_BITS) - 1
 */


/* ======================================================================
 * 前置知识：REQ_* 标志位（bio->bi_opf / rq->cmd_flags 中的高位）
 * ======================================================================
 *
 *   标志位          │ rwbs 字符 │ 含义
 *   ────────────────┼──────────┼─────────────────────────────────────
 *   REQ_PREFLUSH    │ F (前置)  │ flush 前缀（先刷缓存再写数据）
 *   REQ_FUA         │ F (后置)  │ Force Unit Access（强制写介质）
 *   REQ_RAHEAD      │ A         │ 预读
 *   REQ_SYNC        │ S         │ 同步 I/O
 *   REQ_META        │ M         │ 元数据操作
 */


/* ======================================================================
 * Part A: 内核版 blk_fill_rwbs()
 * ======================================================================
 *
 * 文件：src/linux-5.10/kernel/trace/blktrace.c 第 1959 行
 *
 * 在 tracepoint 触发时被调用（在 TP_fast_assign 宏中），
 * 将 bio->bi_opf 或 rq->cmd_flags 转换为 rwbs 字符串。
 *
 * 参数：
 *   rwbs  — 输出缓冲区（至少 RWBS_LEN=8 字节）
 *   op    — bio->bi_opf 或 rq->cmd_flags
 *   bytes — I/O 大小（字节），此函数中未使用（早期版本用过）
 *
 * rwbs 字符串的构建规则（按顺序）：
 *   第 1 位：PREFLUSH → 'F'（如果有）
 *   第 2 位：操作类型 → 'R'/'W'/'D'/'F'/'N'（switch 分支）
 *   第 3 位：FUA     → 'F'（如果有）
 *   第 4 位：RAHEAD  → 'A'（如果有）
 *   第 5 位：SYNC    → 'S'（如果有）
 *   第 6 位：META    → 'M'（如果有）
 *
 * 注意：'F' 可能出现两次！
 *   第 1 位的 F = PREFLUSH（先刷缓存）
 *   第 3 位的 F = FUA（强制写介质）
 *   例如 "FWS" = PREFLUSH + WRITE + SYNC（先 flush 缓存，再同步写）
 *   例如 "WF"  = WRITE + FUA（写并强制到介质）
 */
void blk_fill_rwbs(char *rwbs, unsigned int op, int bytes)
{
	int i = 0;

	/*
	 * ★ 第 1 步：检查 PREFLUSH（前置 flush）
	 *
	 * REQ_PREFLUSH 表示"在写数据之前先刷设备缓存"。
	 * 这是 ext4/XFS 在 journal commit 时常用的模式。
	 *
	 * 如果有 PREFLUSH，rwbs 的第一个字符就是 'F'。
	 * 这会导致 rwbs 中出现两个 'F'（如果同时有 FUA 的话）。
	 */
	if (op & REQ_PREFLUSH)
		rwbs[i++] = 'F';

	/*
	 * ★ 第 2 步：根据操作码（op 的低 8 位）确定主操作字符
	 *
	 * 用 REQ_OP_MASK 提取低 8 位的操作码，然后 switch 匹配。
	 * 这是 rwbs 中唯一一个"必选"的字符（一定有 R/W/D/F/N 之一）。
	 */
	switch (op & REQ_OP_MASK) {
	case REQ_OP_WRITE:          /* 操作码 = 1 */
	case REQ_OP_WRITE_SAME:     /* 操作码 = 7（写同一数据到多个扇区） */
		rwbs[i++] = 'W';    /* Write */
		break;

	case REQ_OP_DISCARD:        /* 操作码 = 3 */
		rwbs[i++] = 'D';    /* Discard / TRIM */
		break;

	case REQ_OP_SECURE_ERASE:   /* 操作码 = 5 */
		rwbs[i++] = 'D';    /* 安全擦除 = Discard */
		rwbs[i++] = 'E';    /*          + Erase */
		break;              /* rwbs = "DE" */

	case REQ_OP_FLUSH:          /* 操作码 = 2 */
		rwbs[i++] = 'F';    /* Flush（独立的 flush 请求） */
		break;

	case REQ_OP_READ:           /* 操作码 = 0 */
		rwbs[i++] = 'R';    /* Read */
		break;

	default:
		/*
		 * ★ N = None（未识别/非常规操作）
		 *
		 * 以下操作码都会落入 default 分支，产生 'N'：
		 *   - REQ_OP_WRITE_ZEROES (9)  — NVMe Write Zeroes
		 *   - REQ_OP_ZONE_OPEN (10)    — ZBD 打开 zone
		 *   - REQ_OP_ZONE_CLOSE (11)   — ZBD 关闭 zone
		 *   - REQ_OP_ZONE_FINISH (12)  — ZBD 完成 zone
		 *   - REQ_OP_ZONE_APPEND (13)  — ZBD 追加写
		 *   - REQ_OP_ZONE_RESET (15)   — ZBD 重置 zone
		 *   - REQ_OP_ZONE_RESET_ALL (17) — ZBD 重置所有 zone
		 *   - 以及未来新增的任何操作码
		 *
		 * 在内核 5.10 中，WRITE_ZEROES 和 ZBD 操作是最常见的 N 来源。
		 * 如果你在 blkparse 输出中看到 N，大概率是这些操作。
		 */
		rwbs[i++] = 'N';
	}

	/*
	 * ★ 第 3 步：检查附加标志位（按固定顺序追加字符）
	 */

	/* FUA：强制写入持久化介质（绕过设备写缓存） */
	if (op & REQ_FUA)
		rwbs[i++] = 'F';    /* 注意：和 PREFLUSH 的 'F' 相同字符！ */

	/* RAHEAD：预读（内核自动发起的顺序预读） */
	if (op & REQ_RAHEAD)
		rwbs[i++] = 'A';

	/* SYNC：同步 I/O（O_SYNC 打开的文件、fsync 等） */
	if (op & REQ_SYNC)
		rwbs[i++] = 'S';

	/* META：元数据操作（文件系统的 journal、inode、dir entry 等） */
	if (op & REQ_META)
		rwbs[i++] = 'M';

	/* 字符串结尾 */
	rwbs[i] = '\0';
}
EXPORT_SYMBOL_GPL(blk_fill_rwbs);


/* ======================================================================
 * Part B: 用户态版 fill_rwbs() — blkparse 中的实现
 * ======================================================================
 *
 * 文件：src/blktrace/blkparse_fmt.c 第 53 行
 *
 * blkparse 输出文本时调用此函数，从 blk_io_trace->action 生成 rwbs。
 *
 * 与内核版的区别：
 *   内核版直接从 bio->bi_opf 读取 REQ_OP_* 操作码和 REQ_* 标志
 *   用户态版从 blk_io_trace->action 的高 16 位读取 BLK_TC_* 类别标志
 *
 *   action 中的 BLK_TC_* 标志是在 __blk_add_trace() 中通过 MASK_TC_BIT 编码的：
 *     what |= MASK_TC_BIT(op_flags, SYNC);    → BLK_TC_SYNC
 *     what |= MASK_TC_BIT(op_flags, RAHEAD);  → BLK_TC_AHEAD
 *     what |= MASK_TC_BIT(op_flags, META);    → BLK_TC_META
 *     what |= MASK_TC_BIT(op_flags, FUA);     → BLK_TC_FUA
 *     what |= MASK_TC_BIT(op_flags, PREFLUSH); → BLK_TC_FLUSH
 *
 *   所以用户态版的 fill_rwbs 不需要知道原始 REQ_* 标志，
 *   只需要从 action 中提取对应的 BLK_TC_* 位即可。
 */
static inline void fill_rwbs(char *rwbs, struct blk_io_trace *t)
{
	/*
	 * 从 action 的高 16 位提取各类别标志
	 *
	 * BLK_TC_ACT(BLK_TC_WRITE) = BLK_TC_WRITE << 16 = (1<<1) << 16 = 0x00020000
	 * t->action & 0x00020000 → 非零表示这是一个写操作
	 */
	int w = t->action & BLK_TC_ACT(BLK_TC_WRITE);     /* 写 */
	int a = t->action & BLK_TC_ACT(BLK_TC_AHEAD);     /* 预读 */
	int s = t->action & BLK_TC_ACT(BLK_TC_SYNC);      /* 同步 */
	int m = t->action & BLK_TC_ACT(BLK_TC_META);      /* 元数据 */
	int d = t->action & BLK_TC_ACT(BLK_TC_DISCARD);   /* discard */
	int f = t->action & BLK_TC_ACT(BLK_TC_FLUSH);     /* flush / preflush */
	int u = t->action & BLK_TC_ACT(BLK_TC_FUA);       /* FUA */
	int i = 0;

	/*
	 * ★ 第 1 步：PREFLUSH → 'F'（和内核版一致）
	 */
	if (f)
		rwbs[i++] = 'F'; /* flush / preflush */

	/*
	 * ★ 第 2 步：确定主操作字符
	 *
	 * 注意用户态版和内核版的差异：
	 *   内核版用 switch(op & REQ_OP_MASK) 精确匹配操作码
	 *   用户态版用 if/else 检查 BLK_TC_* 标志位
	 *
	 *   用户态版无法区分 SECURE_ERASE 和普通 DISCARD（都只有 BLK_TC_DISCARD）
	 *   用户态版的 N 判断逻辑也不同：bytes == 0 时输出 N
	 */
	if (d)
		rwbs[i++] = 'D';            /* discard */
	else if (w)
		rwbs[i++] = 'W';            /* write */
	else if (t->bytes)
		rwbs[i++] = 'R';            /* read（bytes > 0 的默认操作） */
	else
		rwbs[i++] = 'N';            /* ★ None：bytes=0 且不是 W/D */

	/*
	 * ★ 第 3 步：附加标志位（和内核版顺序一致）
	 */
	if (u)
		rwbs[i++] = 'F'; /* FUA（注意：和 flush 相同字符 'F'） */
	if (a)
		rwbs[i++] = 'A'; /* readahead */
	if (s)
		rwbs[i++] = 'S'; /* sync */
	if (m)
		rwbs[i++] = 'M'; /* metadata */

	rwbs[i] = '\0';
}


/* ======================================================================
 * ★ 内核版 vs 用户态版对比
 * ======================================================================
 *
 *   方面            │ 内核版 blk_fill_rwbs()     │ 用户态版 fill_rwbs()
 *   ────────────────┼───────────────────────────┼───────────────────────────
 *   数据源          │ bio->bi_opf / rq->cmd_flags │ blk_io_trace->action
 *   操作码提取      │ switch(op & REQ_OP_MASK)    │ if/else 检查 BLK_TC_* 位
 *   N 的触发条件    │ 操作码不在 R/W/D/F/SE 中   │ bytes==0 且不是 W/D
 *   能区分 PREFLUSH │ 能（REQ_PREFLUSH 标志）     │ 能（BLK_TC_FLUSH 标志）
 *   和 FUA          │ 能（REQ_FUA 标志）          │ 能（BLK_TC_FUA 标志）
 *   SECURE_ERASE    │ 能输出 "DE"                 │ 不能，只能输出 "D"
 *
 *
 * ★ 关于 N 的两版差异
 *
 *   内核版：操作码是 REQ_OP_WRITE_ZEROES/ZBD 等非常规操作 → 'N'
 *   用户态版：bytes == 0 且不是写/discard → 'N'
 *     （例如 PLUG/UNPLUG/GETRQ 等通知类事件，bytes 通常为 0）
 *
 *   实际使用中，blkparse 输出里看到 N 通常是以下情况：
 *     - 通知事件（TN_PROCESS / TN_TIMESTAMP）
 *     - PLUG/UNPLUG 事件（bytes=0）
 *     - GETRQ/SLEEPRQ 事件（bytes=0）
 *
 *
 * ★ rwbs 字符顺序总结
 *
 *   位置 │ 字符 │ 来源                          │ 示例
 *   ─────┼──────┼───────────────────────────────┼─────────
 *   1    │ F    │ REQ_PREFLUSH / BLK_TC_FLUSH   │ F (preflush)
 *   2    │ R    │ REQ_OP_READ                    │
 *        │ W    │ REQ_OP_WRITE / WRITE_SAME      │
 *        │ D    │ REQ_OP_DISCARD                 │
 *        │ DE   │ REQ_OP_SECURE_ERASE            │
 *        │ F    │ REQ_OP_FLUSH                   │
 *        │ N    │ 其他操作码 / bytes=0           │
 *   3    │ F    │ REQ_FUA / BLK_TC_FUA          │ (FUA)
 *   4    │ A    │ REQ_RAHEAD / BLK_TC_AHEAD     │
 *   5    │ S    │ REQ_SYNC / BLK_TC_SYNC        │
 *   6    │ M    │ REQ_META / BLK_TC_META        │
 *
 *   注意：位置 1 和位置 3 都可能是 'F'！
 *   区分方式：位置 1 的 F = preflush，位置 3 的 F = FUA
 *
 *
 * ★ 常见 rwbs 组合详解
 *
 *   rwbs │ 分解                  │ 含义                          │ 常见场景
 *   ─────┼──────────────────────┼──────────────────────────────┼──────────────────
 *   R    │ Read                  │ 普通异步读                    │ cat file
 *   W    │ Write                 │ 普通异步写                    │ writeback
 *   RS   │ Read + Sync           │ 同步读                        │ O_DIRECT read
 *   WS   │ Write + Sync          │ 同步写                        │ O_SYNC write
 *   WM   │ Write + Meta          │ 写元数据                      │ inode update
 *   WSM  │ Write+Sync+Meta       │ 同步写元数据                  │ journal commit
 *   WF   │ Write + FUA           │ FUA 写（强制到介质）          │ ext4 journal
 *   WSF  │ Write+Sync+FUA        │ 同步 FUA 写                  │ XFS log write
 *   FWS  │ Preflush+Write+Sync   │ 先 flush 再同步写            │ ext4 ordered mode
 *   FW   │ Preflush + Write      │ 先 flush 再写                │ ordered write
 *   RA   │ Read + Readahead      │ 预读                          │ 顺序读文件
 *   D    │ Discard               │ TRIM                          │ fstrim
 *   DE   │ Discard + Erase       │ 安全擦除                      │ secure erase
 *   FF   │ Preflush + FUA        │ flush + FUA（双重保证）       │ 极端持久化需求
 *   F    │ Flush                 │ 独立 flush 请求               │ fsync 内部
 *   N    │ None                  │ 非常规操作 / bytes=0          │ plug/unplug/getrq
 *
 *
 * ★ 如何区分两个 'F'？
 *
 *   在 rwbs 字符串 "FWS" 中：
 *     F 在位置 1 → 是 PREFLUSH
 *     W 在位置 2 → 是 WRITE
 *     S 在位置 3 → 是 SYNC
 *
 *   在 rwbs 字符串 "WF" 中：
 *     W 在位置 1 → 是 WRITE（没有 preflush，所以位置 1 被 W 占据）
 *     F 在位置 2 → 是 FUA（因为 W 已经占了位置 1，FUA 追加在后面）
 *
 *   在 rwbs 字符串 "FWF" 中：
 *     F 在位置 1 → 是 PREFLUSH
 *     W 在位置 2 → 是 WRITE
 *     F 在位置 3 → 是 FUA
 *     含义：先 flush 缓存 → 写数据 → 强制到介质（三重保证！）
 *
 *   规则：
 *     - 如果 F 出现在操作字符（R/W/D/N）之前 → 是 PREFLUSH
 *     - 如果 F 出现在操作字符之后 → 是 FUA
 */


/* ======================================================================
 * 实操：在 blkparse 输出中识别各种 rwbs
 * ======================================================================
 *
 *   # 运行 blktrace + fio 生成 I/O
 *   blktrace -d /dev/sda -o trace -w 10 &
 *   fio --name=test --ioengine=libaio --direct=1 --rw=randwrite \
 *       --bs=4k --size=100M --filename=/dev/sda --runtime=8
 *   wait
 *   blkparse -i trace -o parsed.txt
 *
 *   # 查看不同的 rwbs 类型
 *   awk '{print $7}' parsed.txt | sort | uniq -c | sort -rn
 *
 *   # 预期输出示例：
 *   #  1234 WS       ← 同步写（direct I/O）
 *   #   567 W        ← 异步写
 *   #    89 FWS      ← preflush + 写 + sync（journal commit）
 *   #    12 WF       ← FUA 写
 *   #     5 D        ← discard
 *   #     3 F        ← 独立 flush
 *   #     2 N        ← 非常规操作
 *
 *   # 过滤出 FUA 请求
 *   awk '$7 ~ /F/ && $7 !~ /^F/' parsed.txt
 *   # 这排除了 preflush 的 F（在首位），只保留 FUA 的 F（在非首位）
 *
 *   # 过滤出所有含 F 的请求（包括 preflush 和 FUA）
 *   grep "F" parsed.txt | awk '{print $7}' | sort | uniq -c | sort -rn
 */
