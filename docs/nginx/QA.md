# nginx 性能分析问答集

> 持续更新。每个问题包含：问题背景、源码级解答、相关源码位置。

---

## 目录

- [Q1: AMD CCD 拓扑下 nginx worker 如何绑定 CPU？](#q1-amd-ccd-拓扑下-nginx-worker-如何绑定-cpu)
- [Q2: nginx worker CPU 高但 QPS 低怎么排查？](#q2-nginx-worker-cpu-高但-qps-低怎么排查)
- [Q3: nginx 的 worker_processes 设多少合适？和 CPU 核数的关系？](#q3-nginx-的-worker_processes-设多少合适和-cpu-核数的关系)

---

<!-- QA 模板：
## Q{N}: {问题标题}

**日期：** YYYY-MM-DD
**场景：** 在哪里遇到的问题
**相关文件：** 对应的 reading 文件

### 回答
（源码级解答 + 代码引用 + 表格/图示）

---
-->

## Q1: AMD CCD 拓扑下 nginx worker 如何绑定 CPU？

**日期：** 2026-07-04  
**场景：** AMD EPYC 64 核服务器上，nginx worker_processes auto 创建 64 个 worker，性能不佳  
**相关文件：** `docs/nginx/reading/04_performance_tuning.md`  

### 回答

**★ 核心策略：worker 数量 = CCD 数量，每个 worker 绑定到一个 CCD 的物理核。**

#### AMD CCD 拓扑回顾

```
AMD EPYC 7763（64C/128T，8 个 CCD）：

每个 CCD：8 个物理核（+ 8 个超线程），共享 32MB L3 缓存
同 CCD 核间通信：~10-15 ns（共享 L3）
跨 CCD 通信：~40-80 ns（走 Infinity Fabric）
```

#### 三种方案对比

| 方案 | worker 数 | 绑核 | QPS | 问题 |
|------|----------|------|-----|------|
| `auto`（64 worker，不绑核） | 64 | 无 | ~80K | worker 过多，accept 竞争激烈，跨 CCD 调度 |
| `auto` + `auto` 绑核 | 64 | 每核一个 | ~95K | worker 过多仍导致 accept 竞争 |
| ★ 8 worker + CCD 绑核 | 8 | 每 CCD 一组核 | ★ ~110K | 最优方案 |

#### 推荐配置

```nginx
worker_processes 8;    # = CCD 数量

# 每个 worker 绑定到一个 CCD 的所有物理核
# CCD 0: CPU 0-7, CCD 1: CPU 8-15, ...
worker_cpu_affinity 0000000000000000000000000000000000000000000000000000000011111111 \
                    0000000000000000000000000000000000000000000000001111111100000000 \
                    ...（共 8 行，每行对应一个 CCD）;
```

#### 查看 CCD 拓扑

```bash
# 查看 L3 缓存共享关系（同一行 = 同一 CCD）
cat /sys/devices/system/cpu/cpu*/cache/index3/shared_cpu_list
# cpu0:  0-7        ← CCD 0
# cpu8:  8-15       ← CCD 1
# ...
```

#### 为什么有效

```
8 个 worker 的优势：
  1. accept 竞争从 64:1 降到 8:1
  2. 每个 CCD 的 L3 缓存被一个 worker 独占
  3. worker 在 CCD 内部迁移（共享 L3）→ cache miss 率低
  4. 无跨 CCD 调度延迟
```

**一句话总结：** AMD EPYC 上不要用 `worker_processes auto`（核数太多），改为 CCD 数量 + CCD 绑核，QPS 可提升 30-40%。

---

## Q2: nginx worker CPU 高但 QPS 低怎么排查？

**日期：** 2026-07-04  
**场景：** nginx worker 进程 CPU 占用 80%+，但 QPS 远低于预期  
**相关文件：** `docs/nginx/reading/04_performance_tuning.md`  

### 回答

**★ 分四步排查：确认类型 → strace 看 syscall 分布 → perf top 看热点 → perf stat 看硬件指标。**

#### 排查流程

```
步骤 1: 确认 CPU 类型
  top → %usr 高还是 %sys 高？
  
步骤 2: strace -c 统计系统调用分布
  strace -c -p <worker_pid>
  
步骤 3: perf top 看热点函数
  perf top -p <worker_pid>
  
步骤 4: perf stat 看硬件指标
  perf stat -e cache-misses,cpu-migrations -p <worker_pid> -- sleep 10
```

#### 常见根因对照表

| strace -c 现象 | perf top 现象 | 根因 | 解决方案 |
|---------------|-------------|------|---------|
| accept4 占比 > 40% | — | 连接风暴（短连接多） | 启用 keepalive、limit_conn |
| epoll_wait 返回频繁但事件少 | — | busy polling 浪费 | 检查 accept_mutex_delay |
| — | `ngx_http_parse_*` > 20% | 请求头过大/复杂 | 调大 client_header_buffer_size |
| — | `copy_user_*` > 15% | 未启用 sendfile | 启用 `sendfile on` |
| — | `tcp_sendmsg` > 30% | 网络发送瓶颈 | 检查带宽、启用 sendfile |
| — | `futex` > 10% | 锁竞争（aio threads） | 减少线程池竞争 |
| cache-misses 高 | — | CPU 缓存失效 | 启用 `worker_cpu_affinity` |
| cpu-migrations > 0 | — | worker 跨核迁移 | 启用 `worker_cpu_affinity` |
| IPC < 0.5 | — | CPU 等待内存/IO | 检查内存带宽、NUMA 配置 |

#### 快速诊断命令

```bash
WORKER_PID=$(ps aux | grep "nginx: worker" | head -1 | awk '{print $2}')

# 1. strace 统计
strace -c -p $WORKER_PID    # Ctrl+C 后看分布

# 2. perf 热点
perf top -p $WORKER_PID     # q 退出

# 3. perf 硬件指标
perf stat -e cycles,instructions,cache-misses,cpu-migrations \
    -p $WORKER_PID -- sleep 10
```

**一句话总结：** 先 strace -c 看哪个 syscall 多，再 perf top 看哪个函数热，结合两者定位根因。最常见的根因是：连接风暴（accept4 多）、未启用 sendfile（copy_user 多）、未绑核（cache-miss 高）。

---

## Q3: nginx 的 worker_processes 设多少合适？和 CPU 核数的关系？

**日期：** 2026-07-04
**场景：** 配置 nginx 时不确定 worker_processes 应该设为 auto 还是手动指定，以及在多核/超线程/CCD 架构下的最优设置
**相关文件：** `docs/nginx/reading/04_performance_tuning.md`、`docs/nginx/reading/02_command_reference.md`

### 回答

**★ 核心结论：`worker_processes auto` = CPU 逻辑核数，但最优值不一定是核数。小核数机器用 auto 即可，大核数服务器（>16 核）应手动设为物理核数或 CCD 数量。**

#### worker_processes auto 的实现机制

```c
/*
 * 源码位置：src/os/unix/ngx_cpuinfo.c — ngx_ncpu
 *          src/core/ngx_cycle.c — ngx_init_cycle()
 *
 * 检测逻辑：通过 sysconf(_SC_NPROCESSORS_ONLN) 获取在线 CPU 核数
 */

// 伪代码（reading/04_performance_tuning.md）
if (ccf->worker_processes == NGX_CONF_UNSET) {
    ccf->worker_processes = ngx_ncpu;  // ★ auto = CPU 逻辑核数
}
```

`sysconf(_SC_NPROCESSORS_ONLN)` 返回的是**逻辑核数**（含超线程），不是物理核数。

#### ★ 不同场景的最优设置

| 场景 | CPU 配置 | `auto` 值 | 推荐设置 | 原因 |
|------|---------|----------|---------|------|
| 小服务器 | 4 核无超线程 | 4 | `auto`（= 4） | ★ 1 核 1 worker 最优 |
| 中服务器 | 8 核有超线程（16 线程） | 16 | `8`（物理核数） | 超线程对 nginx 无益 |
| 大服务器 | 64 核 EPYC（8 CCD） | 64 | `8`（CCD 数量） | ★ 减少 accept 竞争 |
| 容器 | 限制 2 核 | 2 | `auto`（= 2） | cgroup 感知的核数 |

#### 超线程对 nginx 的影响

```
物理 4 核（有超线程 = 8 逻辑核）：
┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐
│ C0/C0' │ │ C1/C1' │ │ C2/C2' │ │ C3/C3' │
└────────┘ └────────┘ └────────┘ └────────┘

worker_processes auto = 8（★ 含超线程，不是最优）

原因：
  nginx worker 是 CPU 密集型（事件循环 + 请求处理）
  两个 worker 共享同一物理核的 L1/L2 缓存 → cache thrashing
  两个 worker 竞争同一物理核的执行单元 → 性能下降

  → 建议：worker_processes = 物理核数（非逻辑核数）
```

（来源：reading/04_performance_tuning.md 第 55-65 行）

#### 大核数服务器为什么不用 auto

```
AMD EPYC 64 核（8 CCD，每 CCD 8 核共享 32MB L3）：

方案 A: worker_processes auto（64 worker）
  ┌─ 64 个 worker 竞争 accept()  → accept 风暴
  ├─ 每个 worker 在不同 CCD 间迁移 → L3 cache miss
  ├─ worker 间锁竞争加剧
  └─ QPS: ~80,000

方案 B: worker_processes = CCD 数量（8 worker）+ 绑核
  ┌─ 8 个 worker，accept 竞争降低 8 倍
  ├─ 每个 worker 独占一个 CCD 的 L3 缓存
  ├─ worker 在 CCD 内迁移（共享 L3），cache miss 低
  └─ QPS: ~110,000（★ 提升 37%）
```

（来源：reading/04_performance_tuning.md 第 193-235 行，Q1 也详细讨论了此问题）

#### ★ 实战配置决策流程

```
查看 CPU 拓扑：
  lscpu | grep -E '^CPU\(s\)|Thread|Core|Socket|NUMA'

                    │
                    ▼
        CPU 逻辑核数 ≤ 8？
        ├── 是 → worker_processes auto;    ★ 直接用 auto
        │
        └── 否 → 有超线程？
                 ├── 是 → worker_processes = 物理核数;
                 │         （= 逻辑核数 / 2）
                 │
                 └── 否 → 核数 > 16？
                          ├── 是 → worker_processes = CCD 数量;
                          │         + worker_cpu_affinity 绑核
                          │
                          └── 否 → worker_processes auto;
```

#### 常用配置模板

```nginx
# 小服务器（≤ 8 核）
worker_processes auto;

# 中等服务器（8-16 核，有超线程）
worker_processes 8;    # = 物理核数
worker_cpu_affinity auto;

# 大服务器（AMD EPYC 64 核，8 CCD）
worker_processes 8;    # = CCD 数量
worker_cpu_affinity 0000000000000000000000000000000000000000000000000000000011111111 \
                    0000000000000000000000000000000000000000000000001111111100000000 \
                    ...;  # 每个 worker 绑定到一个 CCD

# 容器环境
worker_processes auto;  # sysconf 会感知到 cgroup 限制
```

#### 验证命令

```bash
# 查看 CPU 拓扑
lscpu | grep -E 'Thread|Core|Socket|NUMA|CPU\(s\)'

# 查看 CCD 拓扑（AMD）
cat /sys/devices/system/cpu/cpu*/cache/index3/shared_cpu_list

# 查看当前 nginx worker 数
ps aux | grep "nginx: worker" | wc -l

# 查看 worker CPU 分布
top -bn1 | grep nginx

# 性能测试：对比不同 worker 数的 QPS
for n in 4 8 16 32 auto; do
    sed -i "s/worker_processes .*/worker_processes $n;/" /etc/nginx/nginx.conf
    nginx -s reload
    sleep 2
    wrk -t4 -c1000 -d10s http://localhost/ | grep Requests
done
```

**一句话总结：** `worker_processes auto` 等于逻辑核数（含超线程）。小机器（≤8 核）直接用 auto 即可；大机器应手动设置 — 有超线程则用物理核数，AMD EPYC 等多 CCD 架构用 CCD 数量 + 绑核，可提升 QPS 30-40%。

---

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
