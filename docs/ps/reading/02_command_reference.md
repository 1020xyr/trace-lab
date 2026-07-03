# ps 命令参考 — 参数分类、格式字段与场景命令

> 阅读要点：
>   1. ps 参数的三大分类：选择（过滤进程）、格式（控制输出列）、排序
>   2. 常用格式字段的含义速查
>   3. 10+ 个实用场景命令
>
> 预计阅读时间：25 分钟

---

## 一、ps 参数分类

ps 支持三套参数风格：

```
风格           │ 前缀 │ 示例                │ 来源
───────────────┼──────┼─────────────────────┼───────────────────
UNIX/System V  │ -    │ ps -ef, ps -p 1     │ POSIX 标准
BSD            │ 无前缀│ ps aux, ps ax       │ BSD 系统
GNU 长选项     │ --   │ ps --sort=-%cpu     │ GNU 扩展
```

★ 三套风格可以混用：`ps aux --sort=-%cpu`

---

## 二、选择参数 — 过滤哪些进程

### 2.1 基础选择

```
参数      │ 含义                          │ 示例
──────────┼──────────────────────────────┼────────────────────────────
 -e       │ ★ 所有进程（every）           │ ps -e
 -A       │ 所有进程（all，等同 -e）      │ ps -A
 -a       │ 除会话 leader 和无终端的进程  │ ps -a
 -x       │ 包含没有控制终端的进程        │ ps ax (BSD)
 -d       │ 除会话 leader 外的所有进程    │ ps -d
```

### 2.2 按属性过滤

```
参数              │ 含义                              │ 示例
──────────────────┼──────────────────────────────────┼────────────────────────
 -p <pidlist>     │ ★ 按 PID 过滤（逗号分隔）        │ ps -p 1,2,3
 -u <userlist>    │ ★ 按用户过滤                     │ ps -u root,nginx
 -U <userlist>    │ 按用户过滤（real UID）            │ ps -U www-data
 -G <grouplist>   │ 按组过滤（real GID）              │ ps -G root
 -g <grouplist>   │ 按会话 leader 或组过滤            │ ps -g 0
 -C <cmdlist>     │ ★ 按命令名过滤（精确匹配）        │ ps -C nginx,sshd
 -t <ttylist>     │ 按终端过滤                        │ ps -t pts/0
 --pid <pidlist>  │ 同 -p（GNU 长选项）              │ ps --pid 1234
 --ppid <pidlist> │ ★ 按父进程 ID 过滤               │ ps --ppid 1
 --user <user>    │ 同 -u                            │ ps --user root
 --group <group>  │ 同 -G                            │ ps --group root
 --forest         │ ★ 以树形显示父子关系              │ ps -eo pid,ppid,cmd --forest
```

### 2.3 选择参数组合

```bash
# 所有进程
ps -e
ps -A
ps ax

# root 用户的所有进程
ps -u root

# nginx 的所有进程
ps -C nginx

# 指定 PID 的进程
ps -p 1234

# 多个 PID
ps -p 1,2,100

# 特定终端上的进程
ps -t pts/0

# 所有进程但不包含会话 leader
ps -d
```

---

## 三、格式参数 — 控制输出列

### 3.1 预设格式

```
参数    │ 含义                              │ 输出列
────────┼──────────────────────────────────┼───────────────────────────────────
 -f     │ ★ 完整格式 (full)                │ UID PID PPID C STIME TTY TIME CMD
 -F     │ 更完整格式 (extra full)          │ -f + SZ RSS PSR
 -l     │ 长格式 (long)                    │ F S UID PID PPID C PRI NI ADDR SZ WCHAN TTY TIME CMD
 -j     │ 作业格式 (jobs)                  │ PID PGID SID TTY TIME CMD
 -y     │ 配合 -l，用 RSS 替代 ADDR        │ —
 -Z     │ 包含安全上下文（SELinux）         │ LABEL + 默认列
```

### 3.2 BSD 预设格式

```bash
# BSD 风格的常用组合
ps aux    # ★ 最常用的组合
# 输出列：USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND

ps ax     # 所有进程（BSD 语法）
# 输出列：PID TTY STAT TIME COMMAND

ps axjf   # 带进程树的完整格式
# 输出列：PPID PID PGID SID TTY TPGID STAT UID TIME COMMAND
```

### 3.3 自定义格式（-o）

```
参数                      │ 含义
──────────────────────────┼──────────────────────────────────
 -o <fieldlist>           │ ★ 自定义输出列（逗号分隔）
 --format <fieldlist>     │ 同 -o（GNU 长选项）
 -O <fieldlist>           │ 在默认列之后追加自定义列
```

★ **`-o` 是 ps 最强大的格式化选项。**

```bash
# 基本用法
ps -o pid,cmd
ps -o pid,user,stat,cmd

# 指定列标题
ps -o pid,pid=PROCESS_ID,cmd

# 追加到默认列
ps -O wchan
```

---

## 四、格式字段速查表

### 4.1 基础字段

```
字段      │ 标题        │ 含义                          │ 数据来源
──────────┼────────────┼──────────────────────────────┼───────────────────────
pid       │ PID        │ ★ 进程 ID                     │ /proc/[pid]/stat 字段1
ppid      │ PPID       │ ★ 父进程 ID                   │ /proc/[pid]/stat 字段4
user      │ USER       │ 用户名（UID 解析）             │ /proc/[pid]/status Uid
uid       │ UID        │ 用户 ID（数字）                │ /proc/[pid]/status Uid
euser     │ EUSER      │ 有效用户                       │ /proc/[pid]/status
group     │ GROUP      │ 组名                          │ /proc/[pid]/status Gid
gid       │ GID        │ 组 ID（数字）                  │ /proc/[pid]/status Gid
```

### 4.2 状态字段

```
字段      │ 标题        │ 含义                          │ 数据来源
──────────┼────────────┼──────────────────────────────┼───────────────────────
stat      │ STAT       │ ★ 进程状态 + 修饰符            │ /proc/[pid]/stat 字段3+flags
s         │ S          │ 最小状态字符（仅一个字母）      │ /proc/[pid]/stat 字段3
wchan     │ WCHAN      │ ★ 内核等待函数名               │ /proc/[pid]/wchan
flags     │ F          │ 进程标志位（十六进制）          │ /proc/[pid]/stat 字段9
```

### 4.3 资源字段

```
字段      │ 标题        │ 含义                          │ 数据来源
──────────┼────────────┼──────────────────────────────┼───────────────────────
%cpu      │ %CPU       │ ★ CPU 使用率（%）             │ stat 字段14+15 差值
%mem      │ %MEM       │ ★ 物理内存使用率（%）         │ stat 字段24 / meminfo
vsz       │ VSZ        │ ★ 虚拟内存（KB）              │ /proc/[pid]/stat 字段23
rss       │ RSS        │ ★ 常驻内存集（KB）            │ /proc/[pid]/stat 字段24
sz        │ SZ         │ 内存大小（页）                 │ stat 字段23 / PAGE_SIZE
```

### 4.4 时间字段

```
字段      │ 标题        │ 含义                          │ 数据来源
──────────┼────────────┼──────────────────────────────┼───────────────────────
start     │ START      │ ★ 启动时间                    │ /proc/[pid]/stat 字段22
lstart    │ STARTED    │ 启动时间（完整格式）           │ 同上
time      │ TIME       │ ★ 累计 CPU 时间               │ stat 字段14+15
cputime   │ TIME       │ 同 time                        │ 同上
etimes    │ ELAPSED    │ 运行时间（秒）                 │ 当前时间 - starttime
```

### 4.5 终端与会话字段

```
字段      │ 标题        │ 含义                          │ 数据来源
──────────┼────────────┼──────────────────────────────┼───────────────────────
tty       │ TTY        │ 控制终端                      │ /proc/[pid]/stat 字段7
tname     │ TT         │ 控制终端名                    │ 同上
sid       │ SID        │ 会话 ID                       │ /proc/[pid]/stat 字段6
pgid      │ PGID       │ 进程组 ID                     │ /proc/[pid]/stat 字段5
sess      │ SESS       │ 会话 ID（同 sid）             │ 同上
```

### 4.6 线程与调度字段

```
字段      │ 标题        │ 含义                          │ 数据来源
──────────┼────────────┼──────────────────────────────┼───────────────────────
nlwp      │ NLWP       │ ★ 线程数                      │ /proc/[pid]/stat 字段20
lwp       │ LWP        │ 轻量级进程 ID（线程 ID）      │ /proc/[pid]/task/ 子目录
cls       │ CLS        │ ★ 调度策略                    │ /proc/[pid]/stat 字段41
pri       │ PRI        │ 优先级                        │ stat 字段18
ni        │ NI         │ ★ nice 值 (-20~19)           │ stat 字段19
psr       │ PSR        │ ★ 最后运行的 CPU 号           │ stat 字段39
rtprio    │ RTPRIO     │ 实时优先级                    │ /proc/[pid]/stat
```

### 4.7 命令字段

```
字段      │ 标题        │ 含义                          │ 数据来源
──────────┼────────────┼──────────────────────────────┼───────────────────────
cmd       │ CMD        │ ★ 命令名（短名）              │ /proc/[pid]/stat 字段2
comm      │ COMMAND    │ 同 cmd                         │ 同上
args      │ COMMAND    │ ★ 完整命令行                  │ /proc/[pid]/cmdline
command   │ COMMAND    │ 同 args                        │ 同上
exe       │ EXE        │ 可执行文件路径                 │ /proc/[pid]/exe
```

★ **cmd vs args 的区别：**
  - `cmd` / `comm`：短名（如 `bash`），来自 /proc/[pid]/stat 字段2
  - `args` / `command`：完整命令行（如 `bash -c "ls -la"`），来自 /proc/[pid]/cmdline

---

## 五、排序参数

```
参数                    │ 含义
────────────────────────┼──────────────────────────────────
 --sort=<field>         │ 按字段升序排列
 --sort=-<field>        │ 按字段降序排列（前缀 -）
 --sort=+<field>        │ 按字段升序排列（前缀 +，可省略）
```

★ 可以多级排序：`--sort=-%cpu,-%mem`（先按 CPU 降序，再按内存降序）

```bash
# CPU 使用率最高的进程
ps aux --sort=-%cpu | head

# 内存使用率最高的进程
ps aux --sort=-%mem | head

# 按 PID 排序
ps -eo pid,cmd --sort=pid

# 按启动时间排序（最早启动的在前）
ps -eo pid,start,cmd --sort=start

# 多级排序
ps aux --sort=-%cpu,-%mem | head -20
```

---

## 六、线程参数

```
参数    │ 含义                              │ 示例
────────┼──────────────────────────────────┼──────────────────────
 -L     │ ★ 显示每个线程（LWP）            │ ps -eLf
 -T     │ 显示每个线程（SPID 列）          │ ps -eT
 -H     │ 显示进程层次（缩进）             │ ps -eH
```

### -eLf 输出列说明

```
UID   PID  PPID  LWP  C NLWP STIME TTY   TIME CMD
root    1     0     1  0    1 Jun13 ?   00:00:20 /usr/lib/systemd/systemd ...
                     ↑        ↑
                     │        └─ NLWP = 进程包含的线程总数
                     └─ LWP = 线程 ID（lightweight process）
```

---

## 七、其他有用参数

```
参数              │ 含义                              │ 示例
──────────────────┼──────────────────────────────────┼──────────────────────
 --forest         │ ★ 树形显示父子关系               │ ps -eo pid,cmd --forest
 --no-headers     │ 不输出标题行                     │ ps -eo pid --no-headers
 --cols <N>       │ 设置输出宽度                     │ ps -eo cmd --cols 200
 -ww              │ 不截断命令行（wide wide）         │ ps -eo cmd -ww
 -w               │ 宽输出（不截断）                  │ ps auxw
 --sort           │ 排序                             │ ps --sort=-%cpu
 -M               │ 显示安全上下文                    │ ps -eM
 -V / --version   │ 显示版本                         │ ps --version
```

---

## 八、场景命令集

### 场景 1：最常用的进程列表

```bash
# BSD 格式 — 最经典
ps aux

# System V 格式 — 显示父子关系
ps -ef

# 自定义格式 — 关注状态和资源
ps -eo pid,user,stat,%cpu,%mem,vsz,rss,cmd
```

### 场景 2：CPU 占用排行

```bash
# 前 10 个 CPU 消耗者
ps aux --sort=-%cpu | head -11

# 自定义格式，更详细
ps -eo pid,user,stat,%cpu,%mem,wchan,cmd --sort=-%cpu | head -15
```

### 场景 3：内存占用排行

```bash
# 前 10 个内存消耗者
ps aux --sort=-%mem | head -11

# VSZ vs RSS 对比
ps -eo pid,vsz,rss,cmd --sort=-rss | head -15
```

### 场景 4：查找特定命令

```bash
# 按命令名精确匹配
ps -C nginx -f

# 按命令名模糊匹配（结合 grep）
ps aux | grep [j]ava
# ★ 技巧：[j]ava 避免 grep 自身出现在结果中

# 按父进程 ID 过滤（找某个进程的所有子进程）
ps --ppid 1 -o pid,ppid,cmd
```

### 场景 5：查看特定进程的详情

```bash
# 完整信息
ps -p 1234 -o pid,ppid,user,stat,%cpu,%mem,vsz,rss,wchan,cmd

# 线程级别
ps -eLf -p 1234

# 调度信息
ps -p 1234 -o pid,cls,pri,ni,psr,cmd
```

### 场景 6：查看所有僵尸进程

```bash
# 方法 1：用 STAT 字段过滤
ps -eo pid,ppid,stat,cmd | awk '$3 ~ /^Z/'

# 方法 2：用 grep
ps aux | awk '$8 ~ /^Z/'
```

### 场景 7：查看所有 D 状态进程

```bash
# 找出不可中断睡眠的进程
ps -eo pid,ppid,stat,wchan,cmd | awk '$3 ~ /^D/'

# 结合 stack 排查
for pid in $(ps -eo pid,stat | awk '$2 ~ /^D/{print $1}'); do
    echo "=== PID $pid ==="
    cat /proc/$pid/wchan
    cat /proc/$pid/stack
done
```

### 场景 8：查看线程详情

```bash
# 所有线程列表
ps -eLf

# 某进程的线程
ps -eLf -p $(pgrep -d, java)

# 线程数最多的进程
ps -eo pid,nlwp,cmd --sort=-nlwp | head -15
```

### 场景 9：进程树

```bash
# 树形显示
ps -eo pid,ppid,cmd --forest

# 或 BSD 格式
ps axjf

# 层次缩进
ps -eH
```

### 场景 10：监控进程变化（脚本中使用）

```bash
# 只输出 PID，用于脚本
ps -C nginx -o pid= --no-headers

# 检查进程是否存在
if ps -p $PID > /dev/null 2>&1; then
    echo "进程 $PID 正在运行"
fi

# 统计各用户的进程数
ps -eo user --no-headers | sort | uniq -c | sort -rn
```

### 场景 11：按终端过滤

```bash
# 当前终端的进程
ps -t $(tty | sed 's|/dev/||')

# 所有 pts 终端上的进程
ps -eo pid,tty,cmd | grep pts
```

### 场景 12：查看进程的打开文件和资源

```bash
# 虽然 ps 不直接显示 fd，但可以配合其他工具
PID=1234

# 打开的文件数
ls /proc/$PID/fd | wc -l

# 内存映射
cat /proc/$PID/maps | head

# I/O 统计
cat /proc/$PID/io
```

---

## 九、ps 输出格式对照

### BSD 格式（ps aux）vs System V 格式（ps -ef）

```
BSD (ps aux):
USER       PID %CPU %MEM    VSZ   RSS TTY  STAT START   TIME COMMAND
root         1  0.0  0.1  21612 12632 ?    Ss   Jun13   0:20 /usr/lib/systemd/systemd ...

System V (ps -ef):
UID        PID  PPID  C STIME TTY          TIME CMD
root         1     0  0 Jun13 ?        00:00:20 /usr/lib/systemd/systemd ...
```

```
列名      │ BSD(aux) │ System V(-ef) │ 含义
──────────┼──────────┼───────────────┼─────────────────────────
用户       │ USER     │ UID           │ 进程所有者
PID       │ PID      │ PID           │ 进程 ID
PPID      │ —        │ PPID          │ 父进程 ID（aux 没有）
CPU 使用   │ %CPU     │ C             │ BSD 用百分比，V 用整数
内存使用   │ %MEM     │ —             │ 仅 BSD 有
虚拟内存   │ VSZ      │ —             │ 仅 BSD 有
常驻内存   │ RSS      │ —             │ 仅 BSD 有
状态       │ STAT     │ S             │ BSD 更详细（含修饰符）
启动时间   │ START    │ STIME         │ 格式略有不同
CPU 时间   │ TIME     │ TIME          │ 累计 CPU 时间
命令       │ COMMAND  │ CMD           │ 完整命令行
```

★ **关键区别：**
- BSD 格式多了 %CPU、%MEM、VSZ、RSS、STAT — 更适合性能诊断
- System V 格式多了 PPID — 更适合看父子关系
- 两者可以通过 `-o` 自定义格式取长补短

---

## 十、常用命令组合速查

```bash
# ===== 日常快速查看 =====
ps aux                                # 所有进程概览
ps -ef                                # 带父子关系
ps aux --sort=-%cpu | head -11        # CPU Top 10
ps aux --sort=-%mem | head -11        # 内存 Top 10

# ===== 按条件过滤 =====
ps -C nginx -f                        # 按命令名
ps -u www-data -o pid,stat,cmd        # 按用户
ps -p 1234 -f                         # 按 PID
ps --ppid 1 -o pid,cmd               # 按父 PID

# ===== 自定义格式 =====
ps -eo pid,ppid,user,stat,%cpu,%mem,vsz,rss,wchan,cmd
ps -eo pid,stat,cmd --sort=-%cpu | head -20
ps -eo pid,nlwp,cmd --sort=-nlwp | head -10

# ===== 线程 =====
ps -eLf                               # 所有线程
ps -eLf -p 1234                       # 特定进程的线程

# ===== 进程树 =====
ps -eo pid,ppid,cmd --forest          # 树形显示
ps axjf                               # BSD 树形

# ===== 排查专用 =====
ps -eo pid,stat,wchan,cmd | grep " D"  # D 状态进程
ps -eo pid,ppid,stat,cmd | grep " Z"   # 僵尸进程
ps -eo pid,stat,%cpu,wchan,cmd --sort=-%cpu | head  # CPU 消耗者

# ===== 脚本中使用 =====
ps -C nginx -o pid= --no-headers      # 只输出 PID
ps -eo user --no-headers | sort | uniq -c | sort -rn  # 用户进程统计
```
