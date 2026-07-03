# Step 2: iostat 完整命令参考

> ======================================================================
> 本文件是 iostat 所有参数和使用模式的完整参考手册。
>
> 阅读要点：
>   1. 每个参数的含义和输出影响
>   2. 间隔采样的各种模式
>   3. 10+ 个实战场景的命令组合
>   4. 参数之间的组合规则
>
> 预计阅读时间：10 分钟（首次通读），后续按需查阅
> ======================================================================

---

## 目录

- [一、基本用法](#一基本用法)
- [二、参数完整对照表](#二参数完整对照表)
- [三、参数详细说明](#三参数详细说明)
- [四、间隔采样模式](#四间隔采样模式)
- [五、输出格式控制](#五输出格式控制)
- [六、设备过滤](#六设备过滤)
- [七、持久名称](#七持久名称)
- [八、场景命令组合](#八场景命令组合)
- [九、与 sar 的配合](#九与-sar-的配合)
- [十、常见陷阱](#十常见陷阱)

---

## 一、基本用法

```bash
# 最简用法：显示开机以来的累计统计
iostat

# 扩展输出（最常用）
iostat -x

# 间隔采样：每 1 秒采样一次，共 10 次
iostat -x 1 10

# 跳过首次累计统计（推荐！）
iostat -x -y 1 10

# 持续采样（不指定次数，Ctrl+C 停止）
iostat -x 1
```

---

## 二、参数完整对照表

### 输出控制

| 参数 | 含义 | 默认 | 说明 |
|------|------|------|------|
| `-c` | 仅显示 CPU 统计 | — | 只显示 avg-cpu 部分，不显示设备 |
| `-d` | 仅显示设备统计 | — | 只显示 Device 部分，不显示 CPU |
| `-x` | ★ 扩展输出 | 关 | 显示 r/s、w/s、await、%util 等完整指标 |
| `-t` | 显示时间戳 | 关 | 每次采样前打印当前时间 |
| `-N` | 显示 LVM 设备名 | 关 | 显示 device mapper 的逻辑卷名 |
| `-z` | 隐藏无活动设备 | 关 | 采样期间无 I/O 的设备不显示 |

### 单位控制

| 参数 | 含义 | 默认 | 说明 |
|------|------|------|------|
| `-k` | 以 KB 为单位 | ★ 默认 | rkB/s、wkB/s |
| `-m` | 以 MB 为单位 | — | rMB/s、wMB/s |
| `--human` | 自动选择单位 | — | 打印 9.0k、159.0k 等可读格式 |

### 采样控制

| 参数 | 含义 | 默认 | 说明 |
|------|------|------|------|
| `-y` | ★ 跳过首次累计统计 | 关 | 第一次输出就是间隔采样数据 |
| `<interval>` | 采样间隔（秒） | — | 如 `1`、`5`、`60` |
| `<count>` | 采样次数 | ∞ | 不指定则持续采样 |

### 设备过滤

| 参数 | 含义 | 默认 | 说明 |
|------|------|------|------|
| `<device>` | 指定设备 | ALL | 如 `vdb`、`/dev/vdb`、`sda` |
| `-p <dev>` | 显示分区统计 | — | 如 `-p vdb`、`-p ALL` |
| `-j ID\|LABEL\|PATH\|UUID` | 持久名称 | — | 使用稳定的设备标识符 |

### 输出格式

| 参数 | 含义 | 默认 | 说明 |
|------|------|------|------|
| `--pretty` | 美化输出 | 关 | 数字加千位分隔符 |
| `--compact` | 紧凑输出 | 关 | 去掉列标题间的空行 |
| `--dec={0\|1\|2}` | 小数位数 | 2 | 控制浮点数精度 |
| `-o JSON` | JSON 格式输出 | — | 适合程序解析 |
| `-g <group>` | 设备组聚合 | — | 将多个设备聚合为一组 |
| `-H` | 显示组内设备 | — | 配合 `-g` 使用 |

### 其他

| 参数 | 含义 | 说明 |
|------|------|------|
| `-s` | 国际单位（KB=1000B） | 默认 KB=1024B |
| `-V` | 显示版本 | sysstat version 12.7.6 |
| `-h` | 显示帮助 | 打印用法信息 |

---

## 三、参数详细说明

### ★ `-x` 扩展输出 — 最常用的诊断模式

默认输出只有 `tps`、`kB_read/s`、`kB_wrtn/s` 三个设备指标。
加 `-x` 后扩展到 **24 个字段**，是日常诊断的标配：

```
Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   wrqm/s  %wrqm w_await wareq-sz     d/s     dkB/s   drqm/s  %drqm d_await dareq-sz     f/s f_await  aqu-sz  %util
```

**扩展字段分组：**

```
读指标组：       r/s  rkB/s  rrqm/s  %rrqm  r_await  rareq-sz
写指标组：       w/s  wkB/s  wrqm/s  %wrqm  w_await  wareq-sz
discard 指标组： d/s  dkB/s  drqm/s  %drqm  d_await  dareq-sz
flush 指标组：   f/s  f_await
全局指标：       aqu-sz  %util
```

**各字段含义速查：**

```
字段         │ 含义                        │ 单位    │ 诊断价值
────────────┼─────────────────────────────┼────────┼────────────────────
r/s          │ 每秒读 I/O 数               │ 次/秒   │ 读 IOPS
w/s          │ 每秒写 I/O 数               │ 次/秒   │ 写 IOPS
rkB/s        │ 每秒读吞吐                   │ KB/秒   │ 读带宽
wkB/s        │ 每秒写吞吐                   │ KB/秒   │ 写带宽
rrqm/s       │ 每秒读合并数                 │ 次/秒   │ 合并活跃度
wrqm/s       │ 每秒写合并数                 │ 次/秒   │ 合并活跃度
%rrqm        │ 读合并百分比                 │ %       │ 顺序读效率
%wrqm        │ 写合并百分比                 │ %       │ 顺序写效率
r_await      │ ★ 读平均延迟                 │ ms      │ 读延迟诊断
w_await      │ ★ 写平均延迟                 │ ms      │ 写延迟诊断
rareq-sz     │ 读平均请求大小               │ KB      │ I/O 粒度
wareq-sz     │ 写平均请求大小               │ KB      │ I/O 粒度
aqu-sz       │ ★ 平均队列深度               │ 个      │ 排队严重程度
%util        │ ★ 设备利用率                 │ %       │ 设备饱和度
f/s          │ flush 操作每秒次数           │ 次/秒   │ fsync 频率
f_await      │ flush 平均延迟               │ ms      │ 持久化延迟
```

### `-y` 跳过首次累计统计

```
不加 -y：
  第 1 次输出 = 开机以来的平均值（可能跨数月，无诊断价值）
  第 2 次输出 = 第 1 秒内的数据 ← 这才是你要的
  第 3 次输出 = 第 2 秒内的数据

加 -y：
  第 1 次输出 = 第 1 秒内的数据 ← ★ 直接就是间隔数据
  第 2 次输出 = 第 2 秒内的数据
```

**★ 实践建议：** 间隔采样时永远加 `-y`，除非你想看开机以来的总统计。

### `-t` 带时间戳

```bash
iostat -xt 1 3
```

每次采样前打印当前时间，方便关联到应用日志或告警时间：

```
07/03/2026 11:01:59 PM
avg-cpu:  %user   %nice %system %iowait  %steal   %idle
           ...
Device    r/s  rkB/s  ...
```

### `-p` 分区统计

```bash
# 显示 vdb 的所有分区
iostat -p vdb

# 显示所有设备的所有分区
iostat -p ALL
```

注意：`-p` 只显示基础输出（tps、kB_read/s 等），不支持 `-x` 扩展。

### `-z` 隐藏无活动设备

```bash
# 只显示有 I/O 的设备
iostat -xz 1
```

在多设备系统中很有用，避免被一堆 `0.00` 的设备干扰。

---

## 四、间隔采样模式

### 模式 1：固定次数

```bash
# 每 1 秒采样一次，共 5 次
iostat -x -y 1 5
```

### 模式 2：持续采样

```bash
# 每 1 秒采样一次，直到 Ctrl+C
iostat -x -y 1
```

### 模式 3：长间隔监控

```bash
# 每 60 秒采样一次，共 60 次（监控 1 小时）
iostat -x -y 60 60
```

### 模式 4：后台采样到文件

```bash
# 后台采样，输出到文件
iostat -x -y -t 1 > /tmp/iostat_log.txt &
IOSTAT_PID=$!

# ... 运行你的工作负载 ...

kill $IOSTAT_PID
```

### 模式 5：短间隔高精度

```bash
# 每 0.5 秒采样一次（sysstat 12.7.6 支持小数间隔）
iostat -x -y 0.5 20
```

---

## 五、输出格式控制

### `--human` 可读单位

```bash
iostat -x --human
```

```
Device            r/s     rkB/s   rrqm/s  %rrqm r_await rareq-sz     w/s     wkB/s   ...
vda              0.59      9.0k     0.02   3.0%    1.65    15.2k    1.95    159.0k    ...
```

数字后面自动加 `k`、`M`、`G` 后缀，大数字更易读。

### `--pretty` 千位分隔

```bash
iostat -x --pretty
```

大数字加千位分隔符：`1,234,567` 而非 `1234567`。

### `--dec` 控制小数位

```bash
# 0 位小数
iostat -x --dec=0
# 1 位小数
iostat -x --dec=1
```

### `-o JSON` 程序化输出

```bash
iostat -x -o JSON 1 3
```

输出 JSON 格式，适合监控脚本解析：

```json
{
  "sysstat": {
    "hosts": [{
      "statistics": [{
        "disk": [{
          "disk_device": "vdb",
          "r_s": 0.12,
          "rkB_s": 3.79,
          ...
        }]
      }]
    }]
  }
}
```

---

## 六、设备过滤

### 指定单个设备

```bash
iostat -x vdb
iostat -x /dev/vdb
```

### 指定多个设备

```bash
iostat -x vdb vda
```

### 设备组聚合

```bash
# 将 vda 和 vdb 聚合为 "all_disks" 组
iostat -x -g all_disks vda vdb
```

输出中会多一行 `all_disks`，是所有指定设备的合计值。

### 显示组内各设备

```bash
# -H 配合 -g，显示组内每个设备的明细
iostat -x -g all_disks -H vda vdb
```

---

## 七、持久名称

### `-j ID` 使用设备 ID

```bash
iostat -j ID vdb
```

```
      tps    kB_read/s    kB_wrtn/s    kB_dscd/s    kB_read    kB_wrtn    kB_dscd Device
     3.18         3.79        14.39         0.00    6754985   25641592          0 virtio-bp18nkmupr7gnmjai1ib
```

设备名从 `vdb` 变成了持久标识符 `virtio-bp18nkmupr7gnmjai1ib`。

**用途：** 设备名可能在重启后变化（如 `/dev/sda` 变成 `/dev/sdb`），但 ID 不变。
适合长期监控脚本使用。

### 支持的持久名称类型

```
-j ID       → /dev/disk/by-id/
-j LABEL    → /dev/disk/by-label/
-j PATH     → /dev/disk/by-path/
-j UUID     → /dev/disk/by-uuid/
```

---

## 八、场景命令组合

### 场景 1：快速检查系统 I/O 状态

```bash
# 最常用的诊断命令
iostat -x -y 1 5
```

**解读重点：** %util 和 await

### 场景 2：监控磁盘是否饱和

```bash
# 持续监控，只看设备利用率
iostat -xd -y 1
```

**解读重点：** %util 是否接近 100%

### 场景 3：检查 CPU 是否被 I/O 阻塞

```bash
# 只看 CPU 统计
iostat -c 1 10
```

**解读重点：** %iowait 是否高

### 场景 4：排查特定磁盘的性能

```bash
# 只看 vdb，带时间戳
iostat -x -y -t vdb 1 30
```

### 场景 5：长时间后台监控

```bash
# 后台采样，输出到文件
iostat -x -y -t 1 > /tmp/iostat_$(date +%Y%m%d_%H%M%S).log &
echo "iostat PID: $!"
```

### 场景 6：fio 压测时监控 I/O

```bash
# 终端 1：启动 iostat 监控
iostat -x -y 1 &

# 终端 2：运行 fio 压力测试
fio --name=test --ioengine=libaio --direct=1 --rw=randwrite --bs=4k \
    --filename=/dev/vdb --runtime=30 --time_based --iodepth=32

# 终端 1：观察 iostat 输出变化
```

### 场景 7：对比多个磁盘

```bash
# 同时看 vda 和 vdb
iostat -x -y 1 vda vdb
```

### 场景 8：排查合并效率

```bash
# 关注 rrqm/s 和 wrqm/s
iostat -x -y 1
# %rrqm 和 %wrqm 高 = I/O 合并效率好（顺序 I/O）
# %rrqm 和 %wrqm = 0 = 完全无合并（随机 I/O）
```

### 场景 9：JSON 输出到监控系统

```bash
# 每分钟输出一次 JSON，写入文件
iostat -x -o JSON 60 >> /var/log/iostat.json &
```

### 场景 10：iostat + blktrace 联合诊断

```bash
# 步骤 1：iostat 发现异常
iostat -x -y 1
# → 发现 %util=100%, await=50ms

# 步骤 2：blktrace 定位根因
blktrace -d /dev/vdb -o trace -w 30 &
sleep 1
# ... 让工作负载继续运行 ...
wait
blkparse -i trace -d trace.bin
btt -i trace.bin -o summary
# → 发现 Q2D=45ms, D2C=5ms → 软件层瓶颈
```

### 场景 11：大数字可读模式

```bash
# 大吞吐场景使用 --pretty --human
iostat -x --pretty --human -y 1
```

### 场景 12：低精度快速扫描

```bash
# 0 位小数，快速浏览
iostat -x --dec=0 -y 1 3
```

---

## 九、与 sar 的配合

### sar 记录历史 iostat 数据

```bash
# 记录 1 小时的 I/O 数据（每 10 秒一次）
sar -d 10 360 -o /tmp/sar_io_data

# 事后分析
sar -d -f /tmp/sar_io_data
```

### sar 的优势

```
iostat：实时观测，不保存历史
sar：    长期记录，可回溯分析

sar -d 的输出字段与 iostat -x 几乎一致。
sar 的数据存在二进制文件中，可以事后用 sar -d -f 查看任意时段。
```

---

## 十、常见陷阱

### 陷阱 1：首次输出的误导

```
❌ iostat 1        → 第一次输出是开机以来的平均值
✅ iostat -y 1     → 第一次输出就是最近 1 秒的数据
```

### 陷阱 2：interval 和 count 的顺序

```
✅ iostat 1 5      → 间隔=1秒，采样=5次
❌ iostat 5 1      → 间隔=5秒，采样=1次（不是你想要的意思）
```

### 陷阱 3：%util 不等于容量使用率

```
%util = 100% 不代表"磁盘已经无法更快了"
对于多队列 SSD，100% 只代表"一直有 I/O 在处理"
多队列设备在 100% util 时可能还有大量剩余吞吐
```

### 陷阱 4：await 不区分读和写

```
await = 读写混合平均值
可能读很快但写很慢，混合后看不出来
→ 分别看 r_await 和 w_await
```

### 陷阱 5：%iowait 不等于 I/O 瓶颈

```
%iowait 高 + 系统空闲 = CPU 没事干，不是 I/O 的问题
%iowait 高 + %util 高 = 确实是 I/O 瓶颈
```

### 陷阱 6：分区统计不支持 -x

```
❌ iostat -x -p vdb      → -x 对分区无效
✅ iostat -p vdb          → 只有基础输出
✅ iostat -x vdb          → 设备级别才有扩展输出
```

---

## 参数组合速查表

```
目标                     │ 推荐命令
────────────────────────┼─────────────────────────────────
快速看一眼              │ iostat -x
实时监控（持续）        │ iostat -x -y 1
监控 5 秒               │ iostat -x -y 1 5
只看特定设备            │ iostat -x -y 1 vdb
带时间戳                │ iostat -x -y -t 1
只看 CPU                │ iostat -c 1
只看设备                │ iostat -xd -y 1
MB 单位                 │ iostat -xm -y 1
可读格式                │ iostat -x --human -y 1
隐藏无活动设备          │ iostat -xz -y 1
JSON 输出               │ iostat -x -o JSON 1
分区统计                │ iostat -p vdb
持久名称                │ iostat -j ID vdb
后台记录到文件          │ iostat -x -y -t 1 > /tmp/iostat.log &
长时间监控              │ iostat -x -y 60 3600
```
