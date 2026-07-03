# sar / sysstat 问答集

> 持续更新。每个问题包含：问题背景、解答、相关文件位置。

---

## 目录

- [Q1: sar 发现 %system 很高但吞吐低怎么排查？](#q1-sar-发现-system-很高但吞吐低怎么排查)
- [Q2: 如何用 sar 判断网络软中断 CPU 不均衡？](#q2-如何用-sar-判断网络软中断-cpu-不均衡)

---

## Q1: sar 发现 %system 很高但吞吐低怎么排查？

**日期：** 2026-07-04
**场景：** AMD 服务器，应用报告 CPU 高占用低吞吐
**相关文件：** `reading/02_command_reference.md`, `reading/04_diagnostic_workflow.md`

### 回答

`%system` 高说明内核态占用大量 CPU 时间。需要逐步缩小范围。

**Step 1：区分 %system 的构成**

```bash
# 使用 -u ALL 获取 irq 和 soft 的单独列
sar -u ALL 1 10
```

```
-u:     %system 包含 sys + irq + soft（合并显示）
-u ALL: %sys, %irq, %soft 分开显示

如果 %irq + %soft 占了大部分 → 中断/软中断问题
如果 %sys 本身高 → 锁竞争/系统调用风暴
```

**Step 2：看上下文切换和进程创建**

```bash
sar -w 1 10
```

| 指标 | 正常（4 核） | 异常 |
|------|-------------|------|
| cswch/s | < 20000 | > 50000 → 过度调度/锁竞争 |
| proc/s | < 50 | > 100 → 频繁 fork |

**Step 3：看各 CPU 分布**

```bash
sar -P ALL 1 10
```

| 模式 | 含义 |
|------|------|
| 所有 CPU %system 均匀高 | 全局锁竞争 |
| 某个 CPU %system 特别高 | 中断/锁绑定到该 CPU |
| 某个 CPU %soft 特别高 | 网络中断不均衡 |

**Step 4：看运行队列**

```bash
sar -q 1 10
```

- `runq-sz > CPU 核数` → 进程排队等待
- `blocked > 3` → I/O 阻塞

**Step 5：用专业工具深入**

```
%sys 高 + cswch/s 高 → perf lock record → 锁竞争分析
%sys 高 + 某函数 12%  → perf top → 内核热点函数
%soft 高 + CPU 不均衡 → 启用 irqbalance / 配置 RSS
```

**★ 快速总结：** sar 告诉你"哪个维度异常"，但定位根因需要 perf/bpftrace 深入。

---

## Q2: 如何用 sar 判断网络软中断 CPU 不均衡？

**日期：** 2026-07-04
**场景：** 网络吞吐上不去，怀疑软中断不均衡
**相关文件：** `reading/02_command_reference.md`, `reading/04_diagnostic_workflow.md`

### 回答

需要组合三个 sar 命令来判断。

**Step 1：确认软中断存在**

```bash
sar -u ALL 1 5
# 关注 %soft 列
# %soft > 5% → 软中断占比高，需要关注
```

**Step 2：看各 CPU 的 %soft 分布**

```bash
sar -P ALL -u ALL 1 5
# 对比各 CPU 的 %soft 列
```

```
正常情况（均衡）：
  CPU 0: %soft = 3%
  CPU 1: %soft = 3%
  CPU 2: %soft = 2%
  CPU 3: %soft = 3%

异常情况（不均衡）：
  CPU 0: %soft = 25%   ★ 承担了大量软中断
  CPU 1: %soft = 1%
  CPU 2: %soft = 1%
  CPU 3: %soft = 1%
```

**Step 3：看网络包速率**

```bash
sar -n DEV 1 5
# 关注 rxpck/s（每秒接收包数）
# 小包密集（rxpck/s 高但 rxkB/s 低）最容易暴露不均衡问题
```

**Step 4：看网络错误**

```bash
sar -n EDEV 1 5
# 关注 rxdrop/s（接收丢弃数）
# rxdrop/s > 0 → CPU 处理不过来，包被丢弃
```

**Step 5：看中断分布**

```bash
sar -I XALL 1 5
# 关注网络相关中断线在各 CPU 上的分布
# 如果集中在一个 CPU → 中断亲和性问题
```

**★ 判断标准：**

```
正常:
  - 各 CPU %soft 差异 < 3%
  - rxdrop/s = 0
  - 中断分布均匀

不均衡:
  - 某个 CPU %soft 是其他 CPU 的 5x 以上
  - rxdrop/s > 0
  - 中断集中在一个 CPU

解决方案:
  1. systemctl start irqbalance（自动均衡中断）
  2. 配置网卡多队列 + RSS:
     ethtool -L eth0 combined 4
  3. 启用 RPS（Receive Packet Steering）:
     echo f > /sys/class/net/eth0/queues/rx-0/rps_cpus
```

---

*新的问题将追加到此文件。每个问题记录日期、场景、相关文件。*
