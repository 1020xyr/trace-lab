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

*新的问题将追加到此文件。每个问题记录日期、场景、源码位置。*
