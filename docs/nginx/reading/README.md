# nginx 源码阅读指南

> ★ **nginx 是高性能 Web 服务器的架构典范** — master-worker 模型、事件驱动、零拷贝，是理解高并发网络编程的最佳案例。
> 在性能分析方法论中，nginx 位于"应用层性能调优"环节，将 CPU/网络/架构知识落地到实际服务优化。

本目录包含 nginx 核心架构的源码阅读材料，每个文件都带有详细中文注释。
按 Step 1 → 4 的顺序阅读。

**预计总阅读时间：** 2.5-3.5 小时（快速入门 25 分钟，深入理解 2 小时）

---

## ★ nginx 在性能分析中的定位

```
┌─────────────────────────────────────────────────────────────────┐
│                    应用层性能分析                                 │
│                                                                  │
│  第 1 层：系统级发现                                              │
│    sar -u → CPU 高    sar -n → 网络流量大    ss → 连接数多      │
│                                                                  │
│  第 2 层：进程级定位                                              │
│    pidstat -u → nginx worker 进程的 CPU 使用                    │
│    pidstat -w → 上下文切换模式                                  │
│                                                                  │
│  第 3 层：应用层调优 ← ★ nginx 在这里                            │
│    nginx.conf 调优 → worker 数、连接数、缓冲区                  │
│    架构理解 → epoll 模型、phase handler、sendfile                │
│    CPU 亲和性 → AMD CCD 绑核、NUMA 感知                         │
│    瓶颈定位 → 高 CPU 排查流程、延迟分析                          │
└─────────────────────────────────────────────────────────────────┘
```

---

## 阅读路线

| Step | 文件 | 关注点 | 行数 | 预计时间 |
|------|------|--------|------|---------|
| **1** | [01_nginx_architecture.md](./01_nginx_architecture.md) | ★ master-worker 模型、事件驱动、连接池/内存池、11 phase 流水线 | ~450 | 25 分钟 |
| **2** | [02_command_reference.md](./02_command_reference.md) | 命令行参数、信号控制、nginx.conf 关键指令、性能调优场景 | ~450 | 20 分钟 |
| **3** | [03_command_output_demo.md](./03_command_output_demo.md) | ★ 实际命令执行、真实输出片段、逐行注解 | ~550 | 30 分钟 |
| **4** | [04_performance_tuning.md](./04_performance_tuning.md) | ★★ AMD CCD 绑核策略、sendfile 零拷贝、高 CPU 排查流程 | ~400 | 25 分钟 |

---

## 阅读建议

1. **先读 Step 1**：建立 nginx 架构的全局认知 — master-worker、epoll、phase handler 是三个核心支柱
2. **再读 Step 2**：掌握日常运维和性能调优所需的命令与配置
3. **★ 最后读 Step 3**：通过真实命令输出验证前两步学到的知识，加深理解
4. **★★ Step 4 调优专题**：AMD CCD 绑核、sendfile 零拷贝、高 CPU 排查 — 实战价值最高

---

## 阅读路径推荐

### 快速入门（25 分钟）

```
01_nginx_architecture   → 理解 master-worker + epoll + phase handler
03_command_output_demo  → 看 nginx -t、nginx -s 的真实输出
```

### 深入理解（2 小时）

```
01 → 02（速查）→ 03 → 04（重点）
```

### 完整学习（半天）

```
01 → 02 → 03 → 04
+ 结合 cpu_arch/reading/ 理解 AMD CCD 拓扑
+ 结合 perf/reading/ 做函数级热点分析
```

---

## ★ 性能调优速查

| 优化方向 | 关键配置 | 效果 | 详见 |
|---------|---------|------|------|
| worker 数量 | `worker_processes auto` | 匹配 CPU 核心数 | Step 2 |
| 连接数 | `worker_connections 65535` | 提高并发上限 | Step 2 |
| 零拷贝 | `sendfile on` | 减少内核态/用户态拷贝 | Step 4 |
| CPU 绑定 | `worker_cpu_affinity` | 减少跨核缓存失效 | Step 4 |
| 缓冲区 | `client_body_buffer_size` | 减少临时文件写入 | Step 2 |
| keepalive | `keepalive_timeout` | 减少连接建立开销 | Step 2 |

---

## ★ 与其他工具的交叉引用

### nginx + cpu_arch — AMD CCD 绑核优化

```
★ 这是 AMD 服务器上 nginx 性能调优的核心策略

1. cpu_arch/05_amd_microarchitecture.md → 理解 CCD/CCX 拓扑
   → 同一 CCX 内 8 核共享 L3 cache
   → 跨 CCX 访问 L3 延迟显著增加
   │
   ▼
2. lscpu → 确认 CPU 拓扑（哪些核在同一 CCX）
   │
   ▼
3. nginx worker_cpu_affinity → 绑定 worker 到同一 CCX
   → 减少跨 CCX 的 L3 miss
   → 效果：延迟降低 10-30%
```

### nginx + perf — 高 CPU 排查

```
1. pidstat -u → 找到 nginx worker 进程的 %CPU
2. perf record -p <worker_PID> -g -- sleep 10 → 采集热点函数
3. perf report → 火焰图分析
   ├── SSL 握手占大量 CPU → 考虑 SSL 硬件加速
   ├── 正则匹配占大量 CPU → 优化 location 配置
   └── epoll_wait 占大量 CPU → 正常（等待事件）
```

### nginx + ss/netstat — 连接分析

```
1. ss -tnp | grep nginx → 查看 nginx 的所有连接
2. ss -s → socket 统计摘要
3. ss -tn state time-wait → TIME_WAIT 是否过多
4. 优化：调整 keepalive_timeout、worker_connections
```

### nginx + sar — 长期趋势

```
1. sar -u → CPU 趋势（nginx worker 导致的 CPU 增长）
2. sar -n DEV → 网络流量趋势
3. sar -n SOCK → socket 使用趋势
```

---

## 文件说明

- 所有文件为 Markdown 格式，包含 ASCII 流程图、表格、★ 标记
- 每个文件顶部标注阅读要点和预计时间
- ★ 标记核心重点，帮助快速定位关键信息
- 每个文件底部有 ★ 总结段，概括全文件要点

## 前置知识

- 了解基本的 HTTP 协议（请求行、请求头、响应码）
- 了解 Linux 系统调用（epoll、sendfile、mmap）
- 了解进程模型（fork、信号、进程间通信）

---

## 相关资源

- [learning-roadmap.md](../learning-roadmap.md) — 完整学习路线和实验清单
- [QA.md](../QA.md) — 常见问题问答集
- [cpu_arch reading](../cpu_arch/reading/) — ★ AMD CCD 绑核必读：理解 CPU 拓扑对性能的影响
- [perf reading](../perf/reading/) — 函数级分析：nginx 高 CPU 时的热点函数定位
- [netstat reading](../netstat/reading/) — 连接诊断：ss/netstat 分析 nginx 连接状态
- [sar reading](../sar/reading/) — 长期趋势：sar 监控 nginx 的 CPU/网络变化
- [pidstat reading](../pidstat/reading/) — 进程级分析：nginx worker 的资源使用
