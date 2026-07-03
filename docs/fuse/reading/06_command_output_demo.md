# FUSE 命令输出实战解析

> 每个命令都实际执行，展示真实输出并逐行解读。
> 设备：/dev/vdb (40G virtio-blk)，系统：Alibaba Cloud Linux 4，内核 6.6.102
> FUSE 版本：libfuse 2.9.9（FUSE 2.x）+ libfuse 3.16.2（FUSE 3.x）

---

## 场景 1：编译运行 hello FUSE

### 编译命令

```bash
gcc -o /tmp/hello_fuse docs/fuse/reading/03_hello_fuse.c \
    $(pkg-config fuse --cflags --libs)
```

### 编译输出

```
（无输出，编译成功，退出码 0）
```

**解读：** `pkg-config fuse --cflags --libs` 展开为：
- `--cflags`：`-D_FILE_OFFSET_BITS=64 -I/usr/include/fuse`
- `--libs`：`-lfuse -lpthread`

### 挂载与测试

```bash
mkdir -p /tmp/myfuse
/tmp/hello_fuse -f /tmp/myfuse &
sleep 2
```

### mount 信息

```bash
mount | grep myfuse
```

```
hello_fuse on /tmp/myfuse type fuse.hello_fuse (rw,nosuid,nodev,relatime,user_id=0,group_id=0)
```

```
hello_fuse          → 文件系统名称（程序名自动作为 subtype）
type fuse.hello_fuse → 类型格式：fuse.<subtype>
rw                  → 读写模式
nosuid              → 禁止 SUID
nodev               → 禁止设备文件
relatime            → 相对访问时间更新
user_id=0           → 挂载者 UID（root）
group_id=0          → 挂载者 GID
```

### ls -la 输出

```bash
ls -la /tmp/myfuse
```

```
total 0
drwxr-xr-x  2 root root   0 Jan  1  1970 .
drwxrwxrwt 17 root root 680 Jul  3 08:25 ..
-r--r--r--  1 root root  19 Jan  1  1970 hello
```

```
drwxr-xr-x  → 根目录属性：S_IFDIR | 0755（由 hello_getattr 设置）
-r--r--r--  → 文件属性：S_IFREG | 0444（只读普通文件）
19           → 文件大小（"Hello, FUSE World!\n" 的长度）
Jan 1 1970   → 时间戳全为 0（hello_getattr 中 memset 清零 struct stat）
total 0      → 目录大小为 0（FUSE 文件系统的目录通常不占块）
```

**触发回调链：** `ls` → `GETATTR("/")` → `OPENDIR("/")` → `READDIR("/")` → `READDIR`（第二次，返回空）→ `RELEASEDIR`

### cat 输出

```bash
cat /tmp/myfuse/hello
```

```
Hello, FUSE World!
```

**触发回调链：**

```
cat 进程                  内核 fuse.ko                hello_fuse 守护进程
────────                 ────────────                ────────────────────
1. LOOKUP("/hello")     ──────────────────────────→ hello_getattr("/hello")
   ←── nodeid=2 ←────────────────────────────────── ←── return S_IFREG|0444

2. OPEN(nodeid=2)       ──────────────────────────→ hello_open("/hello", fi)
   ←── fh=0 ←────────────────────────────────────── ←── return 0 (成功)

3. READ(nodeid=2, 4096B, off=0) ──────────────────→ hello_read("/hello", buf, 4096, 0)
   ←── 19 字节 ←─────────────────────────────────── ←── memcpy + return 19

4. READ(nodeid=2, 4096B, off=19) ─────────────────→ hello_read("/hello", buf, 4096, 19)
   ←── 0 字节 (EOF) ←───────────────────────────── ←── offset(19) >= len(19) → return 0

5. FLUSH(nodeid=2)      ──────────────────────────→ hello_ops.flush == NULL → -ENOSYS
6. RELEASE(nodeid=2)    ──────────────────────────→ hello_release("/hello")
   ←── return 0 ←────────────────────────────────── ←── return 0
```

### stat 输出

```bash
stat /tmp/myfuse/hello
```

```
  File: /tmp/myfuse/hello
  Size: 19        	Blocks: 0          IO Block: 4096   regular file
Device: 0,45	Inode: 2           Links: 1
Access: (0444/-r--r--r--)  Uid: (    0/    root)   Gid: (    0/    root)
Access: 1970-01-01 08:00:00.000000000 +0800
Modify: 1970-01-01 08:00:00.000000000 +0800
Change: 1970-01-01 08:00:00.000000000 +0800
```

```
Size: 19        → st_size = 19（hello_content_len）
Blocks: 0       → st_blocks = 0（未设置，默认 0）
Inode: 2        → FUSE nodeid = 2（根目录是 1，hello 文件是 2）
Links: 1        → st_nlink = 1（普通文件默认值）
Device: 0,45    → FUSE 虚拟设备号（major=0, minor=45）
Access: 0444    → st_mode = S_IFREG | 0444
时间戳: 1970    → memset(stbuf, 0, ...) 导致所有时间为 epoch 0
```

### 卸载

```bash
fusermount -u /tmp/myfuse
```

---

## 场景 2：debug 模式输出解读

### 命令

```bash
/tmp/hello_fuse -d /tmp/myfuse 2>/tmp/fuse_debug.log &
sleep 2
ls /tmp/myfuse
cat /tmp/myfuse/hello
sleep 1
fusermount -u /tmp/myfuse
```

### 调试输出（逐行解读）

```
FUSE library version: 2.9.9
```
→ libfuse 版本号。

```
nullpath_ok: 0
nopath: 0
utime_omit_ok: 0
```
→ 内部标志位。`nullpath_ok=0` 表示回调不接受 NULL path；`nopath=0` 表示总是传递 path。

#### INIT 协商

```
unique: 2, opcode: INIT (26), nodeid: 0, insize: 104, pid: 0
INIT: 7.39
flags=0x73fffffb
max_readahead=0x00020000
   INIT: 7.19
   flags=0x00000011
   max_readahead=0x00020000
   max_write=0x00020000
   max_background=0
   congestion_threshold=0
   unique: 2, success, outsize: 40
```

```
unique: 2        → 请求唯一 ID（INIT 总是第一个请求）
opcode: INIT (26) → FUSE_INIT 操作码 = 26
nodeid: 0        → INIT 请求的 nodeid 固定为 0
insize: 104      → 内核发送的 INIT 请求大小 = 104 字节

INIT: 7.39       → 内核支持的协议版本 = 7.39
flags=0x73fffffb → 内核支持的所有能力标志（全部置位）
max_readahead=0x00020000 → 内核建议的预读大小 = 128KB

   INIT: 7.19    → ★ 用户态选择协议版本 = 7.19（取 min(7.39, 7.19)）
   flags=0x00000011 → 用户态请求的能力：
     bit 0 (0x01) = FUSE_CAP_ASYNC_READ（异步读）
     bit 4 (0x10) = FUSE_CAP_EXPORT_SUPPORT
   max_readahead=0x00020000 → 预读大小 = 128KB
   max_write=0x00020000     → ★ 最大写大小 = 128KB（32 页）
```

#### ls 触发的操作

```
unique: 4, opcode: GETATTR (3), nodeid: 1, insize: 56, pid: 397032
getattr /
   unique: 4, success, outsize: 120
```

```
unique: 4        → 第 2 个请求（unique 递增，步长为 2）
opcode: GETATTR  → 获取文件属性
nodeid: 1        → ★ 根目录（FUSE_ROOT_ID = 1）
insize: 56       → GETATTR 请求大小 = 56B（40B header + 16B fuse_getattr_in）
pid: 397032      → ls 进程的 PID
outsize: 120     → 响应大小 = 16B header + 104B fuse_attr_out
```

```
unique: 6, opcode: OPENDIR (27), nodeid: 1, insize: 48, pid: 397032
   unique: 6, success, outsize: 32

unique: 8, opcode: READDIR (28), nodeid: 1, insize: 80, pid: 397032
readdir[0] from 0
   unique: 8, success, outsize: 112
```

```
OPENDIR(27)  → 打开目录（ls 先 opendir 再 readdir）
READDIR(28)  → 读取目录内容
  readdir[0] from 0 → fh=0, offset=0（从头开始读）
  outsize: 112 → 返回了 3 个目录项（"." + ".." + "hello"）
```

```
unique: 10, opcode: READDIR (28), nodeid: 1, insize: 80, pid: 397032
   unique: 10, success, outsize: 16

unique: 12, opcode: RELEASEDIR (29), nodeid: 1, insize: 64, pid: 0
   unique: 12, success, outsize: 16
```

```
第二次 READDIR → offset 已到末尾，返回空（outsize=16 = 只有 header）
RELEASEDIR     → 关闭目录（pid=0 说明在后台上下文执行）
```

#### cat 触发的操作

```
unique: 14, opcode: LOOKUP (1), nodeid: 1, insize: 46, pid: 397033
LOOKUP /hello
getattr /hello
   NODEID: 2
   unique: 14, success, outsize: 144
```

```
LOOKUP(1)    → 查找文件 "hello"（cat 先 lookup 再 open）
nodeid: 1    → 在根目录（nodeid=1）中查找
insize: 46   → 40B header + 6B 字符串 "hello\0"
LOOKUP /hello → libfuse 调用 getattr("/hello") 获取属性
NODEID: 2    → 用户态为 hello 文件分配的 nodeid = 2
outsize: 144 → 16B header + 128B fuse_entry_out（含完整属性）
```

```
unique: 16, opcode: OPEN (14), nodeid: 2, insize: 48, pid: 397033
open flags: 0x8000 /hello
   open[0] flags: 0x8000 /hello
   unique: 16, success, outsize: 32
```

```
OPEN(14)     → 打开文件（nodeid=2 即 hello 文件）
flags: 0x8000 → O_LARGEFILE（64 位文件标志）
open[0]      → fh=0（文件句柄）
outsize: 32  → 16B header + 16B fuse_open_out
```

```
unique: 18, opcode: READ (15), nodeid: 2, insize: 80, pid: 397033
read[0] 4096 bytes from 0 flags: 0x8000
   read[0] 19 bytes from 0
   unique: 18, success, outsize: 35
```

```
READ(15)     → ★ 读取数据（FUSE I/O 热路径上的核心操作）
nodeid: 2    → 读取 hello 文件
read[0]      → fh=0
4096 bytes from 0 → 请求从 offset=0 读取 4096 字节
   read[0] 19 bytes from 0 → 实际返回 19 字节（"Hello, FUSE World!\n"）
   outsize: 35 → 16B header + 19B 数据
```

```
unique: 20, opcode: FLUSH (25), nodeid: 2, insize: 64, pid: 397033
   unique: 20, error: -38 (Function not implemented), outsize: 16
```

```
FLUSH(25)    → close() 时内核发送 FLUSH
error: -38   → -ENOSYS（hello_fuse 未实现 flush 回调）
              → ★ 这不影响功能，FLUSH 错误会被内核忽略
```

```
unique: 22, opcode: RELEASE (18), nodeid: 2, insize: 64, pid: 0
release[0] flags: 0x8000
   unique: 22, success, outsize: 16
```

```
RELEASE(18)  → 文件最终关闭（所有 fd 都关闭后触发）
pid: 0       → 在后台上下文执行
release[0]   → 释放 fh=0
```

---

## 场景 3：strace 追踪 FUSE 通信

### 命令

```bash
strace -f -e trace=read,write,writev -T -s 64 -o /tmp/fuse_strace.log \
    /tmp/hello_fuse -f /tmp/myfuse &
sleep 2
cat /tmp/myfuse/hello
sleep 1
fusermount -u /tmp/myfuse
```

### strace 输出（/dev/fuse 通信记录）

以下是 fd=3（/dev/fuse）上的 read/writev 通信，已过滤库加载噪声：

#### INIT 请求与响应

```
read(3, "h\0\0\0\32\0\0\0\2\0\0\0...", 135168) = 104 <0.000015>
```

```
fd=3          → /dev/fuse 设备文件描述符
135168        → 读缓冲区大小（128KB + header）
= 104         → ★ 实际读取 104 字节（INIT 请求）
<0.000015>    → 耗时 15μs

二进制解析（前 40 字节 = fuse_in_header）：
  h\0\0\0     → len = 0x68 = 104（请求总长度）
  \32\0\0\0   → opcode = 0x1A = 26（FUSE_INIT）
  \2\0\0\0\0\0\0\0 → unique = 2
  \0\0\0\0\0\0\0\0 → nodeid = 0（INIT 固定为 0）
  后续 64 字节 = fuse_init_in（版本、能力等）
```

```
writev(3, [{iov_base="(\0\0\0\0\0\0\0\2\0\0\0\0\0\0\0", iov_len=16},
         {iov_base="\7\0\0\0\23\0\0\0...", iov_len=24}], 2) = 40 <0.000143>
```

```
writev        → ★ FUSE 响应使用 writev（scatter/gather I/O）
iov_base[0]   → fuse_out_header（16 字节）
  (\0\0\0     → len = 0x28 = 40（响应总长度）
  \0\0\0\0    → error = 0（成功）
  \2\0\0\0\0\0\0\0 → unique = 2（匹配 INIT 请求）
iov_base[1]   → fuse_init_out（24 字节：版本、能力、max_write 等）
= 40          → 写入 40 字节
```

#### LOOKUP 请求与响应

```
read(3, ".\0\0\0\1\0\0\0\4\0\0\0...", 135168) = 46 <1.990706>
```

```
.\0\0\0       → len = 0x2E = 46
\1\0\0\0      → opcode = 1（FUSE_LOOKUP）
\4\0\0\0\0\0\0\0 → unique = 4
\1\0\0\0\0\0\0\0 → nodeid = 1（根目录）
hello\0       → 查找文件名 "hello"
<1.990706>    → ★ 阻塞等待了约 2 秒（因为 cat 还没执行）
```

```
writev(3, [{iov_base="\220\0\0\0\0\0\0\0\10\0\0\0...", iov_len=16},
         {iov_base="\2\0\0\0\0\0\0\0...", iov_len=128}], 2) = 144 <0.000040>
```

```
len = 0x90 = 144（16B header + 128B fuse_entry_out）
unique = 8    → 匹配 LOOKUP 请求
nodeid=2 的属性在 iov_base[1] 中（128B fuse_entry_out）
```

#### OPEN 请求与响应

```
read(3, "0\0\0\0\16\0\0\0\6\0\0\0...", 135168) = 48 <1.987506>
```

```
len=48, opcode=14(0x0E)=FUSE_OPEN, unique=6, nodeid=2
后续 8 字节 = fuse_open_in（flags, mode）
```

```
writev(3, [{iov_base=" \0\0\0\0\0\0\0\n\0\0\0...", iov_len=16},
         {iov_base="\0\0\0\0\0\0\0\0...", iov_len=16}], 2) = 32 <0.000036>
```

```
len=32, error=0, unique=10
fuse_open_out: fh=0, open_flags=0
```

#### READ 请求与响应（★ 核心 I/O）

```
read(3, "P\0\0\0\17\0\0\0\f\0\0\0...", 135168) = 80 <0.000031>
```

```
len=80, opcode=15(0x0F)=FUSE_READ, unique=12, nodeid=2
后续 40 字节 = fuse_read_in:
  fh=0, offset=0, size=4096(0x1000)
<0.000031>    → ★ 31μs 就收到了下一个请求（cat 在等 OPEN 返回后立即发 READ）
```

```
writev(3, [{iov_base="#\0\0\0\0\0\0\0\f\0\0\0...", iov_len=16},
         {iov_base="Hello, FUSE World!\n", iov_len=19}], 2) = 35 <0.000038>
```

```
★ 响应中直接包含文件数据！
iov_base[0] → fuse_out_header: len=35, error=0, unique=12
iov_base[1] → "Hello, FUSE World!\n"（19 字节）
= 35          → 16B header + 19B 数据
<0.000038>    → 写响应耗时 38μs
```

#### FLUSH 与 RELEASE

```
read(3, "@\0\0\0\31\0\0\0\16\0\0\0...", 135168) = 64 <0.000031>
writev(3, [{iov_base="\20\0\0\0\332\377\377\377\16\0\0\0...", iov_len=16}], 1) = 16 <0.000032>
```

```
FLUSH: len=64, opcode=25(0x19), error=-38(0xFFFFFFDA = -ENOSYS)
★ error 为负数 → FLUSH 失败（未实现），内核忽略此错误
```

```
read(3, "@\0\0\0\22\0\0\0\f\0\0\0...", 135168) = 64 <0.000118>
writev(3, [{iov_base="\20\0\0\0\0\0\0\0\20\0\0\0...", iov_len=16}], 1) = 16 <0.000032>
```

```
RELEASE: len=64, opcode=18(0x12), error=0, 成功
```

### 关键发现

```
系统调用          耗时         解读
──────────────    ──────────   ─────────────────────────────
read(INIT)        15μs         INIT 请求（挂载时立即到达）
read(LOOKUP)      ~2.0s ★      阻塞等待用户操作（cat 触发后才返回）
writev(LOOKUP)    40μs         返回文件属性
read(OPEN)        ~2.0s ★      同上（LOOKUP 返回后 cat 立即 open）
writev(OPEN)      36μs         返回文件句柄
read(READ)        31μs         OPEN 返回后立即读取
writev(READ)      38μs         ★ 返回 19 字节数据
read(FLUSH)       31μs         close 触发
writev(FLUSH)     32μs         返回 -ENOSYS
read(RELEASE)     118μs        后台释放
writev(RELEASE)   32μs         确认释放

★ read() 的长耗时 = 等待下一个 FUSE 请求到达（阻塞模型）
★ writev() 的耗时 = 实际处理时间（~30-40μs，包含上下文切换）
```

---

## 场景 4：FUSE vs ext4 性能对比

### 命令

```bash
# ext4 基准测试（使用 page cache）
fio --name=ext4_randwrite --directory=/mnt/ext4_base \
    --ioengine=sync --rw=randwrite --bs=4k \
    --size=100M --numjobs=1 --runtime=10 --time_based

# FUSE 测试（同一底层设备，无 writeback_cache）
/tmp/passthrough3_nowbc /mnt/ext4_base /tmp/fuse_pt -f &
fio --name=fuse_randwrite --directory=/tmp/fuse_pt \
    --ioengine=sync --rw=randwrite --bs=4k \
    --size=100M --numjobs=1 --runtime=10 --time_based
```

### ext4 输出（关键指标）

```
  write: IOPS=50.1k, BW=196MiB/s (205MB/s)(2000MiB/10217msec); 0 zone resets
    clat (usec): min=2, max=69415, avg= 4.28, stdev=97.75
     lat (usec): min=2, max=69415, avg= 4.33, stdev=97.76
    clat percentiles (usec):
     | 50.00th=[    4], 90.00th=[    5], 99.00th=[   13], 99.90th=[  120]
   bw (  KiB/s): min=94328, max=409600, avg=204779.15
  cpu          : usr=6.79%, sys=27.00%, ctx=6440, majf=0, minf=11
```

```
IOPS=50.1k       → ★ ext4 使用 page cache，写操作直接缓存，IOPS 极高
avg lat=4.28μs   → 平均延迟 4.28 微秒（仅 page cache 写入开销）
p50=4μs           → 中位数延迟 4μs
p99=13μs          → 99 分位仅 13μs
ctx=6,440         → ★ 上下文切换极少（page cache 不涉及额外切换）
sys=27%           → 内核态占比高（VFS + ext4 + page cache 全在内核）
```

### FUSE 输出（关键指标）

```
  write: IOPS=22.4k, BW=87.4MiB/s (91.7MB/s)(874MiB/10001msec); 0 zone resets
    clat (usec): min=15, max=6444, avg=42.51, stdev=40.28
     lat (usec): min=16, max=6444, avg=42.64, stdev=40.30
    clat percentiles (usec):
     | 50.00th=[   38], 90.00th=[   55], 99.00th=[  120], 99.90th=[  375]
   bw (  KiB/s): min=71496, max=97760, avg=89416.00
  cpu          : usr=2.18%, sys=28.07%, ctx=223809, majf=0, minf=10
```

```
IOPS=22.4k       → FUSE IOPS 是 ext4 的 44.7%（下降 55%）
avg lat=42.51μs  → ★ 延迟是 ext4 的 9.9 倍（42.51 / 4.28）
p50=38μs          → 中位数延迟 38μs（vs ext4 的 4μs）
p99=120μs         → 99 分位 120μs（vs ext4 的 13μs）
ctx=223,809       → ★ 上下文切换是 ext4 的 34.8 倍！
                    原因：每次 write 需要 2 次内核↔用户态切换
usr=2.18%         → 用户态 CPU 占比很低（大部分时间在等待内核）
```

### 性能对比汇总

```
指标              ext4            FUSE            差异倍数
──────────────    ────────────    ────────────    ──────────
IOPS              50,100          22,400          2.24x ↓
平均延迟          4.28 μs         42.51 μs        9.9x ↑
P50 延迟          4 μs            38 μs           9.5x ↑
P99 延迟          13 μs           120 μs          9.2x ↑
吞吐              196 MiB/s       87.4 MiB/s      2.24x ↓
上下文切换        6,440           223,809         34.8x ↑
```

**根因分析：**

```
FUSE 每次 write() 的额外开销：
  1. 内核构造 FUSE_WRITE 请求            ~0.5μs
  2. 请求入队 + 唤醒守护进程             ~1μs
  3. ★ 上下文切换（内核→用户态）         ~2μs
  4. 用户态 libfuse 解析请求             ~1μs
  5. 用户态 pwrite() 系统调用            ~3μs
  6. ★ 上下文切换（用户态→内核，pwrite） ~2μs
  7. 用户态构造响应                      ~0.5μs
  8. ★ 上下文切换（用户态→内核，writev） ~2μs
  9. 内核处理响应 + 唤醒等待线程          ~1μs
  ──────────────────────────────────────
  总额外开销                             ~13-38μs

  ext4 write() 仅涉及 page cache 写入，开销 ~4μs
  FUSE 的额外开销占总延迟的 ~80%
```

---

## 场景 5：blktrace 追踪 FUSE 的底层 I/O

### 命令

```bash
# 启动 FUSE 透传文件系统
/tmp/passthrough3_nowbc /mnt/ext4_base /tmp/fuse_pt -f &
sleep 2

# 启动 blktrace
cd /tmp/fuse_blk_dir && blktrace -d /dev/vdb -o fuse_trace -w 12 &
sleep 1

# 在 FUSE 挂载点上执行 I/O（fsync=1 强制刷盘）
fio --name=fuse_blk_test --directory=/tmp/fuse_pt \
    --ioengine=sync --rw=randwrite --bs=4k \
    --size=50M --numjobs=1 --runtime=8 --time_based --fsync=1

wait
blkparse -i fuse_trace -o fuse_parsed.txt
```

### blktrace 采集统计

```
=== vdb ===
  CPU  0:                45064 events,     2113 KiB data
  CPU  1:                44819 events,     2101 KiB data
  CPU  2:               119389 events,     5597 KiB data
  CPU  3:                38521 events,     1806 KiB data
  Total:                247793 events (dropped 0),    11616 KiB data
```

```
247,793 个事件    → 8 秒内捕获近 25 万个 block 事件
dropped 0         → ★ 无事件丢失（buffer 足够）
CPU 2 事件最多     → I/O 处理偏向 CPU 2（中断亲和性）
```

### 事件类型分布

```bash
awk '{print $6}' fuse_parsed.txt | sort | uniq -c | sort -rn
```

```
  53625 Q    ← Queue（bio 入队）
  45936 C    ← Complete（I/O 完成）
  38280 D    ← Dispatch（下发到设备）
  30657 M    ← Merge（请求合并）
  22968 I    ← Insert（插入调度器）
  22968 G    ← Get Request（分配请求）
  15312 U    ← Unplug（拔塞）
  15312 P    ← Plug（插队）
```

```
Q > G > I > D > C 的原因：
  Q(53625) = G(22968) + M(30657)
  → 53625 个 bio 入队，其中 30657 个被合并（Merge）
  → 合并后只剩 22968 个独立 request
  → 这 22968 个 request 经过 I(22968) → D(38280) → C(45936)

  D(38280) > G(22968) 因为 D 包含 flush 请求
  C(45936) > D(38280) 因为 C 还包含 jbd2 的完成事件
```

### rwbs 字段分布

```bash
awk '{print $7}' fuse_parsed.txt | sort | uniq -c | sort -rn | head -5
```

```
 130218 WSM   ← Write + Sync + Metadata（ext4 journal 元数据写入）
  38280 WS    ← Write + Sync（FUSE 守护进程的数据写入）
  30624 N     ← None（Plug/Unplug 控制事件，无数据）
  30624 FN    ← Flush + None（flush 命令）
  15312 FWFSM ← Flush + Write + FUA + Sync + Metadata
```

```
★ 关键发现：WSM 占 53%！

FUSE 写操作的 I/O 放大效应：
  应用层 1 次 write → FUSE 守护进程 1 次 pwrite → ext4 写入：
    1. WS  → 数据写入（FUSE 守护进程的 pwrite）
    2. WSM → ext4 journal 元数据写入（事务日志）
    3. FN  → flush 命令（fsync 触发）
    4. FWFSM → journal commit（事务提交）

  → 1 次用户写操作 ≈ 4 次底层 block I/O
  → I/O 放大倍数 ≈ 4x
```

### 一个 FUSE I/O 的完整事件链

```
253,16   1   1  0.000000000 399616  Q  WS 800800 + 8 [passthrough3_no]
253,16   1   2  0.000004799 399616  G  WS 800800 + 8 [passthrough3_no]
253,16   1   3  0.000005401 399616  P   N [passthrough3_no]
253,16   1   4  0.000006028 399616  U   N [passthrough3_no] 1
253,16   1   5  0.000006643 399616  I  WS 800800 + 8 [passthrough3_no]
253,16   1   6  0.000011022 399616  D  WS 800800 + 8 [passthrough3_no]
253,16   2   1  0.000224479     0   C  WS 800800 + 8 [0]
```

```
399616          → ★ PID = FUSE 守护进程（passthrough3_nowbc）
[ passthrough3_no] → 进程名（截断为 15 字符）

时间线：
  t=0.000000000  Q  入队（bio 进入 block layer）
  t=0.000004799  G  分配 request（4.8μs 后）
  t=0.000005401  P  插队（延迟下发）
  t=0.000006028  U  拔塞（开始下发）
  t=0.000006643  I  插入调度器
  t=0.000011022  D  下发到设备驱动（Q→D = 11μs，软件层延迟）
  t=0.000224479  C  设备完成（D→C = 213μs，设备处理时间）

Q→D = 11μs      → 软件层开销很小
D→C = 213μs     → ★ 设备处理时间（virtio-blk 到云盘后端）
Q→C = 224μs     → 端到端延迟

[0]             → C 事件在中断上下文完成（PID=0）
```

### jbd2 元数据 I/O（ext4 journal 写入）

```
253,16   0   1  0.000322135 396902  Q WSM 42206600 + 8 [jbd2/vdb-8]
253,16   0   2  0.000323833 396902  G WSM 42206600 + 8 [jbd2/vdb-8]
253,16   0   3  0.000324076 396902  P   N [jbd2/vdb-8]
253,16   0   4  0.000333030 396902  Q WSM 42206608 + 8 [jbd2/vdb-8]
253,16   0   5  0.000333920 396902  M WSM 42206608 + 8 [jbd2/vdb-8]
```

```
396902          → jbd2/vdb-8 内核线程的 PID
[jbd2/vdb-8]    → ext4 的 journal 守护进程
WSM             → Write + Sync + Metadata（元数据写入）
M 事件          → ★ 多个 journal 块被合并（Merge）
                  42206600, 42206608, 42206616... 连续扇区被合并为一个 request
```

### 各进程的事件分布

```bash
awk '{match($0, /\[([^\]]+)\]/, m); if (m[1]!="") procs[m[1]]++}
     END {for (p in procs) print procs[p], p}' fuse_parsed.txt | sort -rn | head
```

```
122562 jbd2/vdb-8       ← ext4 journal 线程（占 49.7%！）
 45936 passthrough3_no   ← FUSE 守护进程（占 18.6%）
 45936 [0]               ← 中断上下文（I/O 完成）
 25209 kworker/2:0H      ← 内核工作线程（block 层下发）
  1989 kworker/0:1H
  1794 kworker/1:1H
  1632 kworker/3:1H
```

```
★ 关键发现：
  jbd2/vdb-8 占了 49.7% 的 I/O 事件 → ext4 journal 写入是主要 I/O 来源
  FUSE 守护进程只占 18.6%            → 大量 I/O 来自底层文件系统的元数据操作
  kworker 占 12.3%                   → 内核 block 层处理

  → FUSE 文件系统的底层 I/O 不仅有数据写入，
    还有大量 ext4 journal 元数据写入，
    这是 FUSE I/O 放大效应的重要原因。
```

---

## 场景 6：writeback_cache 优化效果

### 命令

```bash
# 无 writeback_cache（FUSE 3.x 默认模式）
/tmp/passthrough3_nowbc /mnt/ext4_base /tmp/fuse_pt -f &
fio --name=fuse3_nowbc --directory=/tmp/fuse_pt \
    --ioengine=sync --rw=randwrite --bs=4k \
    --size=100M --numjobs=1 --runtime=10 --time_based

# 有 writeback_cache（init 回调中启用 FUSE_CAP_WRITEBACK_CACHE）
/tmp/passthrough3 /mnt/ext4_base /tmp/fuse_pt -f &
fio --name=fuse3_wbc --directory=/tmp/fuse_pt \
    --ioengine=sync --rw=randwrite --bs=4k \
    --size=100M --numjobs=1 --runtime=10 --time_based
```

### 无 writeback_cache 输出

```
  write: IOPS=22.4k, BW=87.4MiB/s (91.7MB/s)(874MiB/10001msec); 0 zone resets
    clat (usec): min=15, max=6444, avg=42.51, stdev=40.28
    clat percentiles (usec):
     | 50.00th=[   38], 90.00th=[   55], 99.00th=[  120], 99.90th=[  375]
  cpu          : usr=2.18%, sys=28.07%, ctx=223809
```

```
IOPS=22,400      → 每次 write 都经过 FUSE 到用户态再返回
lat=42.51μs      → 上下文切换 + 调度延迟 + 用户态处理
ctx=223,809      → 每次 I/O 约 10 次上下文切换（223809 / 22400 ≈ 10）
```

### 有 writeback_cache 输出

```
[init] FUSE_CAP_WRITEBACK_CACHE 已请求 (kernel 支持时自动启用)
```

```
  write: IOPS=85.0k, BW=332MiB/s (348MB/s)(3320MiB/10001msec); 0 zone resets
    clat (usec): min=2, max=10398, avg= 9.43, stdev=109.60
    clat percentiles (usec):
     | 50.00th=[    5], 90.00th=[    8], 99.00th=[   39], 99.90th=[ 1221]
  cpu          : usr=13.04%, sys=45.00%, ctx=18167
```

```
IOPS=85,000      → ★ writeback_cache 下 IOPS 提升 3.8 倍！
lat=9.43μs       → ★ 延迟降低 4.5 倍（接近 ext4 的 4.28μs）
p50=5μs           → 中位数延迟仅 5μs（大部分写入命中 page cache）
ctx=18,167       → ★ 上下文切换减少 12.3 倍（18167 vs 223809）
usr=13%           → 用户态 CPU 增加（后台异步刷写）
sys=45%           → 内核态 CPU 增加（page cache 管理 + 后台写回）
```

### 优化效果汇总

```
指标              无 wbc          有 wbc          提升倍数
──────────────    ────────────    ────────────    ──────────
IOPS              22,400          85,000          3.8x ↑
平均延迟          42.51 μs        9.43 μs         4.5x ↓
P50 延迟          38 μs           5 μs            7.6x ↓
P99 延迟          120 μs          39 μs           3.1x ↓
吞吐              87.4 MiB/s      332 MiB/s       3.8x ↑
上下文切换        223,809         18,167          12.3x ↓
```

### 优化原理

```
无 writeback_cache（默认 writethrough 模式）：
  write() → FUSE 内核模块 → 阻塞等待
                              ↓
                    FUSE 守护进程 pwrite() → 底层 ext4
                              ↓
                    响应返回 → 唤醒等待线程
  ★ 每次 write() 都是一次完整的同步往返

有 writeback_cache：
  write() → FUSE 内核模块 → page cache → 立即返回！
                              ↓（后台异步）
                    FUSE 守护进程 pwrite() → 底层 ext4
  ★ 应用直接写入 page cache，无需等待用户态响应
  ★ 内核在后台批量将脏页刷写到 FUSE 守护进程

延迟分解：
  writethrough: 2 次上下文切换 + 调度 + 用户态处理 ≈ 42μs
  writeback:    直接写 page cache ≈ 9μs（仅内核开销）
  ext4 直写:    直接写 page cache ≈ 4μs（无 FUSE 开销）
```

---

## 场景 7：/sys/fs/fuse/connections/ 信息解读

### 命令

```bash
# 挂载 FUSE 后查看连接信息
/tmp/hello_fuse -f /tmp/myfuse &
sleep 2
ls /sys/fs/fuse/connections/
for conn_dir in /sys/fs/fuse/connections/*/; do
    conn=$(basename "$conn_dir")
    echo "=== Connection $conn ==="
    for f in "$conn_dir"/*; do
        echo "  $(basename $f) = $(cat $f 2>/dev/null)"
    done
done
```

### 输出

```
=== FUSE 连接列表 ===
45

=== Connection 45 ===
  abort =
  congestion_threshold = 9
  max_background = 12
  waiting = 0
```

### 逐字段解读

```
45              → 连接 ID（内核分配的序号，每次挂载递增）

waiting = 0     → ★ 当前等待用户态处理的请求数
                  0 = 正常（无积压）
                  >0 = 有请求在等待（守护进程可能忙或挂死）
                  如果持续 >0，说明守护进程处理不及时

max_background = 12   → 最大后台请求数
                        后台请求指 FUSE 内核模块异步发送的请求
                        （如 readahead 预读、writeback 写回）
                        超过此数后，新后台请求会被拒绝或等待

congestion_threshold = 9  → 拥塞阈值（= max_background × 75%）
                            当后台请求数超过此值时，
                            内核开始对 FUSE 连接进行流量控制
                            （减慢新请求的发送速度）

abort =（空）   → 写入 "1" 可强制终止此连接
                  用于处理挂死的 FUSE 文件系统：
                  echo 1 > /sys/fs/fuse/connections/45/abort
```

### FUSE 模块信息

```bash
echo "version: $(cat /sys/module/fuse/version)"
echo "refcnt: $(cat /sys/module/fuse/refcnt)"
echo "initstate: $(cat /sys/module/fuse/initstate)"
```

```
version:（空）    → 内核 FUSE 模块不一定暴露版本号
refcnt: 3        → ★ 模块引用计数 = 3
                    表示当前有 3 个地方引用了 fuse.ko 模块
                    （挂载点、连接、内核内部引用等）
initstate: live  → 模块已加载并处于活跃状态
```

### /etc/fuse.conf 配置

```bash
cat /etc/fuse.conf
```

```
# The file /etc/fuse.conf allows for the following parameters:
#
# user_allow_other - Using the allow_other mount option works fine as root, but
# in order to have it work as a regular user, you need to set user_allow_other
# in /etc/fuse.conf as well.
#user_allow_other

# mount_max = n - this option sets the maximum number of mounts.
#mount_max = 1000
```

```
#user_allow_other  → 被注释 → 普通用户不能使用 -o allow_other
                      取消注释后，普通用户挂载的文件系统可被其他用户访问

#mount_max = 1000  → 最大挂载数（默认无限制）
                      取消注释可限制系统中 FUSE 挂载点总数
```

### 紧急中止挂死的 FUSE

```bash
# 如果 FUSE 文件系统挂死（无响应），可以用以下方式强制中止：

# 方法 1：通过 /sys/fs/fuse/connections/
echo 1 > /sys/fs/fuse/connections/45/abort

# 方法 2：懒卸载
fusermount -uz /tmp/myfuse

# 方法 3：强制卸载
umount -f /tmp/myfuse

# 方法 4：杀掉守护进程
kill -9 $(pidof hello_fuse)
# → 内核检测到 /dev/fuse 关闭后自动卸载
```

---

## 场景 8：hello_fuse 的编译命令详解

### pkg-config 输出

```bash
pkg-config fuse --cflags
```

```
-D_FILE_OFFSET_BITS=64 -I/usr/include/fuse
```

```
-D_FILE_OFFSET_BITS=64  → ★ 将 off_t 定义为 64 位
                           FUSE 必须使用 64 位偏移量（支持大文件）
                           如果不定义，32 位 off_t 只能支持 2GB 文件
-I/usr/include/fuse     → FUSE 头文件搜索路径
```

```bash
pkg-config fuse --libs
```

```
-lfuse -lpthread
```

```
-lfuse     → 链接 libfuse 共享库
-lpthread  → 链接 POSIX 线程库（FUSE 内部使用多线程）
```

### 完整编译命令展开

```bash
# 等效的完整命令
gcc -o /tmp/hello_fuse docs/fuse/reading/03_hello_fuse.c \
    -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse \
    -lfuse -lpthread
```

### FUSE 3.x 编译对比

```bash
# FUSE 3.x
pkg-config fuse3 --cflags
# 输出：-D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3

pkg-config fuse3 --libs
# 输出：-lfuse3 -lpthread

gcc -o hello_fuse3 hello_fuse3.c $(pkg-config fuse3 --cflags --libs)
```

---

## 快速诊断流程

```
1. FUSE 挂载后无响应
   cat /sys/fs/fuse/connections/*/waiting
   → waiting > 0 = 请求积压
   → echo 1 > /sys/fs/fuse/connections/N/abort 强制中止

2. FUSE 性能差
   fio --directory=/mnt/fuse --ioengine=sync --rw=randwrite --bs=4k --size=50M --runtime=10
   → 观察 IOPS 和 ctx（上下文切换数）
   → ctx >> IOPS × 2 = 正常（每次 I/O 至少 2 次切换）
   → 启用 writeback_cache 提升写性能

3. FUSE I/O 错误排查
   strace -p $(pidof fuse_daemon) -e trace=read,write,writev -T
   → 检查 /dev/fuse 的 read/writev 是否返回错误
   → 检查响应中的 error 字段

4. 底层 I/O 分析
   blktrace -d /dev/vdb -o fuse_io -w 10
   → 观察 FUSE 守护进程对底层设备的 I/O 模式
   → 对比直接写 ext4 的 I/O 模式（I/O 放大效应）

5. 协议调试
   ./myfs -d /mnt/myfs
   → 查看每个 FUSE opcode 的参数和返回值
   → 验证回调函数的调用顺序和内容
```
