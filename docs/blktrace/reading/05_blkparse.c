/*
 * ======================================================================
 * Step 5: blkparse.c — 二进制文件的解析与格式化输出
 * ======================================================================
 *
 * 原始路径：src/blktrace/blkparse.c (2959 行)
 *
 * blkparse 的职责：
 *   1. 打开所有 per-CPU 的 .blktrace.N 二进制文件
 *   2. 逐个读取 blk_io_trace 结构
 *   3. 按时间戳归并排序（合并多 CPU 的事件流）
 *   4. 解析 action 位掩码，格式化输出
 *
 * 本文件只关注核心解析函数 dump_trace()。
 *
 * 预计阅读时间：10 分钟
 * ======================================================================
 */


/* ======================================================================
 * ★ dump_trace() — 处理单个事件
 * ======================================================================
 *
 * 这是 blkparse 处理每个 blk_io_trace 事件的核心入口。
 *
 * 参数：
 *   t   — 当前事件的 blk_io_trace 结构
 *   pci — 该事件来自哪个 CPU
 *   pdi — 该事件属于哪个设备
 */
static void dump_trace(struct blk_io_trace *t, struct per_cpu_info *pci,
		       struct per_dev_info *pdi)
{
	/* 如果启用了文本输出（默认启用） */
	if (text_output) {
		if (t->action == BLK_TN_MESSAGE)
			/* 通知消息：特殊处理 */
			handle_notify(t);
		else if (t->action & BLK_TC_ACT(BLK_TC_PC))
			/* PC 请求（SCSI passthrough）：用 SCSI 格式输出 */
			dump_trace_pc(t, pdi, pci);
		else
			/* ★ 文件系统请求：用标准格式输出 */
			dump_trace_fs(t, pdi, pci);
	}

	/* 更新事件计数 */
	if (!pdi->events)
		pdi->first_reported_time = t->time;
	pdi->events++;

	/* 如果启用了二进制输出（-d 参数），也写入二进制文件 */
	if (bin_output_msgs ||
		    !(t->action & BLK_TC_ACT(BLK_TC_NOTIFY) &&
		      t->action == BLK_TN_MESSAGE))
		output_binary(t, sizeof(*t) + t->pdu_len);
}


/*
 * ★ dump_trace_fs() — 格式化文件系统请求的输出
 *
 * 这个函数将 blk_io_trace 结构转换为人类可读的文本行。
 * 它从 action 字段中提取：
 *   - 操作类型（Q/D/C/G/I/M/P/U 等）
 *   - 读写方向（R/W/D）
 *   - 属性标志（S/F/M/A）
 *
 * 输出格式示例：
 *   8,0    1        1     0.000000000  1234  Q   WS 12345678 + 8 [fio]
 *   │      │        │          │        │    │   ││      │       │
 *   │      │        │          │        │    │   ││      │       └─ 进程名
 *   │      │        │          │        │    │   ││      └─ 扇区 + 扇区数
 *   │      │        │          │        │    │   │└─ 属性（S=Sync, F=FUA, M=Meta, A=Ahead）
 *   │      │        │          │        │    │   └─ 方向（R=Read, W=Write, D=Discard）
 *   │      │        │          │        │    └─ 事件类型字母
 *   │      │        │          │        └─ PID
 *   │      │        │          └─ 时间戳（秒.纳秒）
 *   │      │        └─ 序列号
 *   │      └─ CPU 号
 *   └─ 设备号 (major,minor)
 *
 * 注意：blkparse 的 -f 参数可以自定义输出格式：
 *   blkparse -i trace -f "%D %c %T %p %a %m\n"
 *   %D = 设备号
 *   %c = CPU
 *   %n = 序列号
 *   %T = 时间戳
 *   %p = PID
 *   %a = action
 *   %m = 进程名
 *   %S = sector
 *   %N = bytes
 */


/*
 * ★ action 字段的解析逻辑（简化版）
 *
 * action 是 32 位值，blkparse 会分别提取：
 *
 *   高 16 位 → 类别标志（BLK_TC_*）
 *     bit 0  → R (READ)
 *     bit 1  → W (WRITE)
 *     bit 2  → F (FLUSH)
 *     bit 3  → S (SYNC)
 *     bit 4  → Q (QUEUE)    ← 在 blkparse 输出中显示为 "Q"
 *     bit 5  → (REQUEUE)
 *     bit 6  → D (ISSUE)    ← 在 blkparse 输出中显示为 "D"
 *     bit 7  → C (COMPLETE) ← 在 blkparse 输出中显示为 "C"
 *     bit 8  → (FS)
 *     bit 9  → (PC)
 *     bit 10 → (NOTIFY)
 *     bit 11 → A (AHEAD)
 *     bit 12 → M (META)
 *     bit 13 → D (DISCARD)
 *     bit 14 → (DRV_DATA)
 *     bit 15 → F (FUA)      ← 在 rwbs 字段中显示为 "F"
 *
 *   低 8 位 → 操作类型
 *     1  → QUEUE      → Q
 *     2  → BACKMERGE  → M
 *     3  → FRONTMERGE → M
 *     4  → GETRQ      → G
 *     5  → SLEEPRQ    → S
 *     6  → REQUEUE    → (requeue)
 *     7  → ISSUE      → D
 *     8  → COMPLETE   → C
 *     9  → PLUG       → P
 *     10 → UNPLUG_IO  → U
 *     11 → UNPLUG_TIMER → U
 *     12 → INSERT     → I
 *     13 → SPLIT      → (split)
 *     14 → BOUNCE     → (bounce)
 *     15 → REMAP      → (remap)
 *     16 → ABORT      → (abort)
 */


/*
 * ★ 完整事件类型对照表
 *
 *   字母 │ 操作名           │ 说明
 *   ─────┼──────────────────┼──────────────────────────────────────────
 *     Q  │ Queue            │ bio 进入 block layer（I/O 生命周期起点）
 *     G  │ Get Request      │ 分配 request 结构体
 *     S  │ Sleep on Request │ 等待 request 分配（队列资源不足）
 *     I  │ Insert           │ request 插入 I/O 调度器队列
 *     M  │ Merge            │ bio/request 与已有请求合并（前向/后向）
 *     P  │ Plug             │ 插队：暂时阻止请求下发，等待合并机会
 *     U  │ Unplug           │ 拔塞：开始下发累积的请求
 *     D  │ Issue/Dispatch   │ request 下发到设备驱动（软件→硬件的分界点）
 *     C  │ Complete         │ 设备驱动完成 I/O（I/O 生命周期终点）
 *     R  │ Requeue          │ 设备拒绝请求，重新放回队列（错误重试）
 *     A  │ Readahead        │ 预读请求（内核自动发起的顺序预读）
 *     F  │ Flush/FUA        │ flush 或 FUA 请求（强制写入持久化介质）
 *     B  │ Bounce           │ 使用 bounce buffer（DMA 地址限制）
 *     X  │ Split            │ bio 被拆分（跨设备边界或大小限制）
 *     m  │ Remap            │ bio 被重映射（DM/LVM/分区映射）
 *
 *   I/O 完整生命周期：
 *   Q → G → S(可选) → I → M(可选) → P → U → D → C
 *
 *   延迟指标：
 *   Q2G = G 时间 - Q 时间  （等待 request 分配）
 *   Q2I = I 时间 - Q 时间  （进入调度器前）
 *   I2D = D 时间 - I 时间  （在调度器中等待）
 *   Q2D = D 时间 - Q 时间  （软件层总延迟 = Q2G + Q2I + I2D）
 *   D2C = C 时间 - D 时间  （设备处理延迟）
 *   Q2C = C 时间 - Q 时间  （端到端总延迟 = Q2D + D2C）
 */
