# vmstat 问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

（暂无条目，后续学习过程中追加）

---

<!--
## 模板（追加新 QA 时参考此格式）

## Q{N}: {问题标题}

**日期：** YYYY-MM-DD
**场景：** 在哪里遇到的问题
**相关文件：** 对应的 reading 文件

### 回答

（源码级解答 + 代码引用 + 表格/图示）
-->

---

## 待探索问题清单

以下问题在后续学习中遇到时需要追加到此文件：

1. vmstat 的数据来自哪些 `/proc` 文件？（`/proc/stat`、`/proc/meminfo`、`/proc/vmstat`、`/proc/diskstats`）
2. `r` 列的值是否包含当前正在各 CPU 上执行的进程？
3. `bi`/`bo` 的 block 大小在不同内核版本中是否一致？
4. vmstat 的采样精度受什么因素影响？（HZ、jiffies）
5. 为什么 `vmstat -s` 的 CPU ticks 总和与 `uptime` 不完全一致？
6. `-a` 模式的 active/inactive 与 `/proc/meminfo` 的 Active/Inactive 是否完全对应？
