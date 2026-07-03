# Top-Down Microarchitecture Analysis — 问答集

> 持续追加的 Q&A，记录学习和实践中的关键问题

---

<!-- 格式说明：
## Q{N}: {问题标题}

**日期：** YYYY-MM-DD
**场景：** 在哪里遇到的问题
**相关文件：** 对应的 reading 文件
**源码位置：** 具体文件和行号

### 回答
（源码级解答 + 代码引用 + 表格/图示）
-->

---

<!-- 以下为模板示例，实际使用中逐步替换 -->

## Q1: 为什么 perf stat --topdown 在虚拟机上无法使用？

**日期：** 2026-07-03
**场景：** 在 KVM 虚拟机上运行 perf stat --topdown 时报 PMU 事件不可用
**相关文件：** reading/02_command_reference.md

### 回答

**根因：** PMU（Performance Monitoring Unit）是 CPU 的硬件计数器，需要通过虚拟化透传才能使用。

| 虚拟化方式 | PMU 可用性 | 说明 |
|-----------|-----------|------|
| 物理机 | ★ 完全可用 | 直接访问 PMU 寄存器 |
| KVM + PMU passthrough | 部分可用 | 需要 `-cpu host` 且主机支持 |
| KVM 默认 | 不可用 | 虚拟 CPU 不模拟 PMU |
| VMware/VirtualBox | 不可用 | 不透传 PMU |

**验证方法：**
```bash
# 检查 PMU 是否可用
perf stat -e cycles,instructions -- sleep 1
# 如果显示 <not supported>，说明 PMU 不可用

# 检查 dmesg 中的 PMU 信息
dmesg | grep -i pmu
# 正常应看到：Performance Events: PEBS fmt3+, 32-deep x86_64 PMU
```

**解决方案：**
1. 使用物理机进行测试
2. KVM 启动时添加 `-cpu host` 参数透传 PMU
3. 使用 toplev.py 的 `--emulate` 模式（近似结果）

---

## Q2: TMAM 的 Pipeline Width（流水线宽度）为什么因 CPU 代际不同？

**日期：** 2026-07-03
**场景：** 不同 CPU 上 topdown 百分比计算基数不同
**相关文件：** reading/01_pipeline_slots.md

### 回答

Pipeline Width = 每周期最大退休 μop 数量。不同微架构的宽度不同：

| CPU 微架构 | Pipeline Width | 代号 |
|-----------|---------------|------|
| Sandy Bridge / Ivy Bridge | 4 | SNB/IVB |
| Haswell / Broadwell | 4 | HSW/BDW |
| Skylake / Kaby Lake / Coffee Lake | 4 | SKL/KBL/CFL |
| Ice Lake / Tiger Lake | 4 | ICL/TGL |
| Alder Lake (P-Core) | 4 | ADL-P |
| Alder Lake (E-Core) | 4 | ADL-E（但执行端口更少） |
| Sapphire Rapids / Emerald Rapids | 4 | SPR/EMR |

**★ 虽然宽度都是 4，但内部执行资源（端口数、ROB 大小、缓存层次）差异很大。** 这导致同一程序在不同 CPU 上的 topdown 分布不同。

**公式：**
```
Total Slots = Pipeline_Width × CPU_CLK_UNHALTED.THREAD
Retiring%   = INST_RETIRED.ANY / Total Slots
```
