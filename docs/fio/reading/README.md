# fio 源码阅读指南

> ★ **fio 是 Linux 存储性能测试的事实标准工具** — 生成可控的 I/O 负载，测量延迟/吞吐/IOPS。
> 在性能分析方法论中，fio 位于"主动压测 → 观测指标"链路的核心环节。

本目录包含 fio 核心源码的阅读材料，每个文件都带有详细中文注释。
按 Step 1 → 12 的顺序阅读，从数据结构到引擎实现，逐层深入。

**预计总阅读时间：** 4-6 小时（快速入门 30 分钟，深入理解 2 小时）

---

## ★ fio 在性能分析中的定位

```
┌─────────────────────────────────────────────────────────────────┐
│                    存储性能分析工具链                              │
│                                                                  │
│  第 1 层：宏观发现                                                │
│    iostat -x 1 → %util 高？await 大？                           │
│                                                                  │
│  第 2 层：主动压测 ← ★ fio 在这里                                │
│    fio 生成可控 I/O 负载 → 测量 slat/clat/lat/bw/iops           │
│    复现问题、验证优化效果、建立性能基线                            │
│                                                                  │
│  第 3 层：微观追踪                                                │
│    blktrace + blkparse + btt → 每个 I/O 的 Q2D/D2C 延迟        │
│    Q2D 高 = 软件层瓶颈    D2C 高 = 硬件层瓶颈                    │
└─────────────────────────────────────────────────────────────────┘
```

---

## 阅读路线

| Step | 文件 | 来源 | 行数 | 关注点 | 预计时间 |
|------|------|------|------|--------|---------|
| **1** | [01_io_u_struct.h](./01_io_u_struct.h) | `src/fio/io_u.h` | 213 | ★ `struct io_u` 定义 — fio 的 I/O 单元，类比 blktrace 的 `blk_io_trace` | 15 分钟 |
| **2** | [02_ioengines.h](./02_ioengines.h) | `src/fio/ioengines.h` | 159 | ★ `struct ioengine_ops` — 引擎插件接口，所有引擎必须实现的函数指针 | 10 分钟 |
| **3** | [03_fio_main.c](./03_fio_main.c) | `src/fio/fio.c` | 62 | `main()` 启动流程 — 从命令行到 I/O 执行的调度 | 5 分钟 |
| **4** | [04_backend_do_io.c](./04_backend_do_io.c) | `src/fio/backend.c` | ~290 | ★★ `do_io()` 主循环 — fio 的心脏，get_io_u → queue → getevents | 30 分钟 |
| **5** | [05_io_u_get.c](./05_io_u_get.c) | `src/fio/io_u.c` | ~600 | ★ `get_io_u()` 与 offset 计算 — 顺序 vs 随机的核心逻辑 | 25 分钟 |
| **6** | [06_engine_sync.c](./06_engine_sync.c) | `src/fio/engines/sync.c` | 519 | sync 引擎 — 最简单的引擎，read()/write() 直接阻塞完成 | 15 分钟 |
| **7** | [07_engine_libaio.c](./07_engine_libaio.c) | `src/fio/engines/libaio.c` | 489 | ★ libaio 引擎 — queue/commit/getevents 异步三步曲 | 20 分钟 |
| **8** | [08_engine_spdk.md](./08_engine_spdk.md) | — | — | SPDK 引擎介绍 — 用户态存储栈，绕过内核直接访问 NVMe | 15 分钟 |
| **9** | [09_stat_output.md](./09_stat_output.md) | — | — | fio 输出解读 — slat/clat/lat/bw/iops 的含义和计算方式 | 20 分钟 |
| **10** | [10_command_reference.md](./10_command_reference.md) | — | — | 完整命令参考 — 核心参数速查、10 个典型场景 | 20 分钟 |
| **11** | [11_command_output_demo.md](./11_command_output_demo.md) | 实际执行 | — | 命令输出实战解析 — 9 个场景的真实输出 | 25 分钟 |
| **12** | [12_best_practices.md](./12_best_practices.md) | — | — | ★ 性能基准测试最佳实践 — 延迟/吞吐/IOPS 配置、fio+blktrace 联合分析 | 20 分钟 |

---

## 阅读建议

1. **先读 Step 1-2**：理解 `io_u` 和 `ioengine_ops` — 这是 fio 的两个核心抽象
2. **Step 3 快速过**：`main()` 只有 35 行，了解启动流程即可
3. **★ Step 4 是核心**：`do_io()` 是 fio 的心脏，花时间理解 get_io_u → queue → getevents 循环
4. **Step 5 理解 offset**：顺序 I/O 和随机 I/O 的 offset 计算逻辑都在这里
5. **Step 6 → 7 对比学习**：先看最简单的 sync 引擎，再看 libaio 的异步模式
6. **Step 8 了解前沿**：SPDK 代表了 I/O 性能的上限（绕过内核）
7. **Step 9 实践结合**：读懂 fio 输出，将源码与性能数据联系起来

---

## 阅读路径推荐

### 快速入门（30 分钟）

```
01_io_u_struct.h    → 理解 I/O 单元
02_ioengines.h      → 理解引擎接口
06_engine_sync.c    → 理解最简单的 queue() 实现
```

### 深入理解（2 小时）

```
01 → 02 → 03 → 04（重点）→ 05 → 06 → 07
```

### 完整学习（半天）

```
01 → 02 → 03 → 04 → 05 → 06 → 07 → 08 → 09
+ 结合 learning-roadmap.md 中的实验动手操作
```

---

## 核心概念速查

### io_u 的一生

```
分配: __get_io_u()           ← 从空闲池弹出
填充: fill_io_u()            ← 设置 ddir/offset/buflen
提交: engine->queue()        ← 交给引擎（sync: 阻塞; libaio: 入队）
完成: engine->getevents()    ← 异步收割
回收: put_io_u()             ← 归还到空闲池
```

### 同步 vs 异步引擎

| | sync 引擎 | libaio 引擎 |
|---|----------|------------|
| queue() | read()/write() 阻塞 | 入队，返回 QUEUED |
| commit() | 不需要 | io_submit() 批量提交 |
| getevents() | 不需要 | io_getevents() 批量收割 |
| iodepth | 始终 1 | 可配置（32/64/128） |

### 延迟指标对应

| fio | blktrace | 含义 |
|-----|---------|------|
| slat | Q→D | 提交延迟（用户态开销） |
| clat | D→C | 完成延迟（设备处理时间） |
| lat | Q→C | 总延迟（端到端） |

---

## ★ 与其他工具的交叉引用

### fio + blktrace — 联合诊断工作流

```
★ 这是存储性能分析最强大的组合

1. fio 生成 I/O 负载 + 输出宏观指标（slat/clat/lat/bw/iops）
   │
   ├── 发现 clat 高 → 设备处理慢（硬件瓶颈？）
   ├── 发现 slat 高 → 提交开销大（软件瓶颈？）
   └── 发现 lat 波动大 → 偶发慢 I/O？
   │
   ▼
2. blktrace 同时采集 → 追踪每个 I/O 的 Q/D/C 事件
   │
   ▼
3. btt 延迟分析
   ├── Q2D 高 → 软件层瓶颈（调度器、队列深度、锁竞争）
   └── D2C 高 → 硬件层瓶颈（设备性能、固件 GC）
```

### fio + iostat — 宏观验证

```
1. iostat -x 1 → 观察设备级 %util、await、avgqu-sz
2. fio 生成负载 → 验证 iostat 指标是否与 fio 输出一致
3. iostat 的 await ≈ fio 的 lat（端到端延迟）
```

### fio + perf — CPU 开销分析

```
1. perf stat -e cycles,instructions,cache-misses fio ...
   → 测量 fio 运行期间的 CPU 效率（IPC、cache-miss 率）
2. perf record -g fio ... → 火焰图分析 fio 的 CPU 热点函数
3. 发现 fio 的 CPU 开销占比高 → 引擎选择或参数优化
```

---

## 文件说明

- C 源文件（`.h`/`.c`）：带注释的源码摘录，不可编译（仅供阅读）
- Markdown 文件（`.md`）：概念介绍和使用指南
- 每个文件头部标注原始路径，方便在 `src/fio/` 中查找完整版
- 长文件（> 200 行）仅截取关键函数，跳过无关代码

---

## 相关资源

- [fio 官方文档](https://fio.readthedocs.io/)
- [fio GitHub](https://github.com/axboe/fio)
- [fio HOWTO](https://fio.readthedocs.io/en/latest/fio_doc.html)
- [learning-roadmap.md](../learning-roadmap.md) — 完整学习路线和实验清单
- [QA.md](../QA.md) — 常见问题问答集
- [blktrace reading](../blktrace/reading/) — ★ 联合分析必读：blktrace 微观追踪
- [iostat reading](../iostat/reading/) — ★ 宏观 I/O 观测：iostat 设备级统计
- [perf reading](../perf/reading/) — CPU 开销分析：perf 硬件计数器与火焰图
