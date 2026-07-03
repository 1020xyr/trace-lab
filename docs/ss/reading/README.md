# ss（socket statistics）阅读入口

> 从内部机制到命令实战，逐步深入 ss 的世界

---

## 前置知识

建议先阅读：
- `docs/netstat/reading/01_tcp_state_machine.md` — TCP 11 个状态的生命周期
- `docs/netstat/learning-roadmap.md` — netstat 与 ss 的对比、netlink 原理概述

---

## 阅读路线

### ★ 推荐顺序

```
01 → 02 → 03
原理 → 命令 → 实战
```

| 编号 | 文件 | 主题 | 预计时间 | 难度 |
|------|------|------|---------|------|
| 01 | `01_ss_internals.md` | ss 内部机制：netlink、内核数据结构、过滤器语法 | 15 分钟 | ★★★ |
| 02 | `02_command_reference.md` | 完整参数参考：协议/状态/信息级别/过滤器 | 20 分钟 | ★★ |
| 03 | `03_command_output_demo.md` | 12 个场景真实输出 + 逐字段注解 | 30 分钟 | ★★ |

---

## 快速查阅

### 只需要知道怎么用？

直接跳到 `02_command_reference.md`，然后看 `03_command_output_demo.md` 理解输出。

### 需要理解原理？

从 `01_ss_internals.md` 开始，理解 ss 如何通过 netlink 与内核交互。

### 遇到性能问题？

直接看 `03_command_output_demo.md` 的场景 11（网络性能诊断流程）和场景 12（TIME_WAIT/CLOSE_WAIT 优化）。

---

## 文件说明

```
reading/
├── README.md                    ← 你在这里
├── 01_ss_internals.md           # 内部机制与数据结构
├── 02_command_reference.md      # 参数与过滤器完整参考
└── 03_command_output_demo.md    # 真实输出逐字段实战
```
