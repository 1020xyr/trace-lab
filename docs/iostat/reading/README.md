# iostat 学习材料阅读指南

> ★ **iostat 是 Linux I/O 性能诊断的第一步** — 通过 /proc/diskstats 快速判断"设备忙不忙、延迟高不高"。
> 在性能分析方法论中，iostat 位于"宏观发现 → 微观定位"链路的起点。

本目录包含 iostat 的工作原理、命令参考和实战输出解析。
按 Step 1 → 3 的顺序阅读。

**预计总阅读时间：** 2.5-3.5 小时（快速入门 20 分钟，深入理解 2 小时）

---

## ★ iostat 在性能分析中的定位

```
┌─────────────────────────────────────────────────────────────────┐
│                    I/O 性能诊断分层                               │
│                                                                  │
│  第 1 层：宏观发现 ← ★ iostat 在这里                            │
│    iostat -x 1 → %util 高不高？await 大不大？                   │
│    ┌──────────────────────────────────────────────────┐          │
│    │ 发现异常 → 进入第 2 层                            │          │
│    │ 一切正常 → 瓶颈不在 I/O，转向 CPU/内存/网络分析   │          │
│    └──────────────────────────────────────────────────┘          │
│                                                                  │
│  第 2 层：进程定位                                                │
│    pidstat -d → 哪个进程的 kB_wr/s 或 kB_rd/s 最高？           │
│                                                                  │
│  第 3 层：微观追踪                                                │
│    blktrace + blkparse + btt → 每个 I/O 的 Q2D/D2C 延迟        │
└─────────────────────────────────────────────────────────────────┘
```

---

## 阅读路线

| Step | 文件 | 行数 | 关注点 | 预计时间 |
|------|------|------|--------|---------|
| **1** | [01_iostat_architecture.md](./01_iostat_architecture.md) | ~520 | ★ iostat 工作原理：/proc/diskstats 数据源、字段计算逻辑、与 blktrace 的差异 | 30 分钟 |
| **2** | [02_command_reference.md](./02_command_reference.md) | ~580 | ★ 完整命令参考：所有参数、间隔采样模式、12 个场景组合 | 30 分钟 |
| **3** | [03_command_output_demo.md](./03_command_output_demo.md) | ~730 | ★★ 输出实战解析：8 个场景的真实输出 + 逐行注解 + 诊断表 | 45 分钟 |

---

## 阅读建议

1. **先读 Step 1**：理解 `/proc/diskstats` 的 20 个字段 — 这是 iostat 所有数据的来源
2. **Step 2 当手册用**：遇到不认识的参数时查阅
3. **★ Step 3 是核心**：每个场景都有真实命令输出 + 逐字段注解，对照自己的输出理解
4. **重点关注 `-x` 扩展输出**：这是日常诊断最常用的模式

---

## 阅读路径推荐

### 快速入门（20 分钟）

```
01_iostat_architecture → 理解 /proc/diskstats 数据源
03_command_output_demo → 场景 1（基础输出）+ 场景 2（-x 扩展输出）
```

### 深入理解（2 小时）

```
01 → 02（速查）→ 03（全部 8 个场景）
+ learning-roadmap.md 中的诊断思维链和实验
```

---

## ★ 核心指标异常阈值

| 指标 | 命令 | 正常值 | 异常值 | 诊断方向 |
|------|------|--------|--------|---------|
| %util | `iostat -x` | < 80% | > 90% | → blktrace 分析 Q2D/D2C |
| await | `iostat -x` | < 5ms (SSD) | > 20ms | → pidstat -d 找进程 |
| avgqu-sz | `iostat -x` | < 设备队列深度 | > 32 | → 降低 iodepth |
| %iowait | `iostat` (CPU 区) | < 5% | > 10% | → I/O 阻塞 CPU |

---

## ★ 与其他工具的交叉引用

### iostat → blktrace — 从发现到定位

```
★ 这是 I/O 诊断最经典的组合

1. iostat -x 1 → 发现异常指标
   ├── %util ≈ 100% → 设备饱和
   ├── await > 20ms → I/O 延迟大
   ├── avgqu-sz > 32 → 队列堆积
   └── %iowait > 50% → CPU 被 I/O 阻塞
   │
   ▼
2. blktrace -d /dev/vdb -o trace -w 30 → 追踪微观细节
   │
   ▼
3. blkparse + btt → 看每个 I/O 的 Q/D/C 事件
   ├── Q2D 高 → 软件层瓶颈（调度器、队列深度）
   └── D2C 高 → 硬件层瓶颈（设备性能）
```

### iostat → fio — 压测验证

```
1. iostat -x 1 → 观察到生产环境的 I/O 模式
2. fio 复现类似 I/O 模式 → 在可控条件下验证问题
3. fio 输出 slat/clat/lat → 与 iostat 的 await 对照
```

### iostat → pidstat — 进程定位

```
1. iostat -x 1 → 发现设备级 I/O 异常
2. pidstat -d 1 → 找到产生 I/O 最多的进程
3. 两者对照 → 确认"谁"导致了"什么设备"的压力
```

### iostat → sar — 实时 vs 历史

```
iostat = "现在进行时"（实时观测）
sar -d = "过去完成时"（历史回溯）

数据完全相同（都来自 /proc/diskstats），区别在于：
- 收到告警 → sar -d -f 查看历史 I/O 趋势
- 确认问题 → iostat -x 1 实时观测当前状态
```

---

## 文件说明

- 所有输出均来自实际执行（/dev/vdb，virtio-blk 40G）
- I/O 负载由 fio 生成
- 每个场景标注了"逐字段注解"的字段列表
- ★ 标记核心重点，表格用于字段对照和诊断速查

---

## 相关资源

- [learning-roadmap.md](../learning-roadmap.md) — 完整学习路线和实验清单
- [QA.md](../QA.md) — 常见问题问答集
- [blktrace reading](../blktrace/reading/) — ★ 微观追踪必读：每个 I/O 的 Q2D/D2C 延迟拆解
- [fio reading](../fio/reading/) — 主动压测工具：复现 I/O 问题、验证优化效果
- [pidstat reading](../pidstat/reading/) — 进程级 I/O 定位：找到产生 I/O 的进程
- [sar reading](../sar/reading/) — 历史回溯分析：sar -d 查看过去的 I/O 趋势
