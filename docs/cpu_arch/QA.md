# CPU 架构与内核 CPU 子系统 — 问答集

> 持续积累的问题与解答，涵盖 CPU 微架构、内核调度器、调优实践。
> 每个问题包含：日期、场景、相关文件、源码级解答。

---

<!-- 模板: 按以下格式追加新问答 -->

<!--
## Q{N}: {问题标题}

**日期：** YYYY-MM-DD
**场景：** 在哪里遇到的问题
**相关文件：** 对应的 reading 文件

### 回答
（源码级解答 + 代码引用 + 表格/图示）
-->

---

（暂无问答，后续学习过程中逐步补充。）

### 建议关注的方向

1. **CFS vruntime 计算细节**：calc_delta_fair() 的定点数运算如何实现？
2. **调度域构建过程**：sched_domain 如何从 ACPI/DT 信息构建？
3. **PELT 衰减算法**：sched_avg 的指数衰减如何在整数运算中实现？
4. **NUMA balancing 的 page fault 机制**：PROT_NONE + page fault 的完整流程
5. **SMT 调度权重**：内核如何量化超线程的资源竞争？
6. **CFS bandwidth 控制**：cgroup CPU 限流的底层实现
7. **schedutil governor**：如何从 PELT 利用率计算目标频率？
8. **上下文切换成本**：switch_mm() + switch_to() 的具体开销分析
9. **KVM 虚拟化中的 PMU 透传**：如何让虚拟机使用硬件性能计数器？
10. **Spectre/Meltdown 缓解措施的性能影响**：PTI、Retpoline 的开销分析
