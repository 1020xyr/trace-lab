# ss（socket statistics）问答集

> 持续追加的技术问答，记录学习和使用中遇到的问题

---

## 目录

（随问答追加自动更新）

---

## Q1: ss 输出中 Recv-Q 和 Send-Q 在 LISTEN 状态下为什么含义不同？

**日期：** 2026-07-04
**场景：** 使用 `ss -tln` 查看监听端口时，对 Recv-Q 和 Send-Q 的含义产生疑惑
**相关文件：** `reading/01_ss_internals.md`、`reading/03_command_output_demo.md`

### 回答

在 ss 输出中，Recv-Q 和 Send-Q 的含义取决于 socket 的状态：

| 状态 | Recv-Q 含义 | Send-Q 含义 |
|------|------------|------------|
| **LISTEN** | accept 队列中已完成三次握手的连接数 | listen() 的 backlog 参数（队列上限） |
| **ESTAB** | 接收缓冲区中未被应用 read() 的字节数 | 发送缓冲区中未被对端 ACK 的字节数 |

#### 内核源码依据

ss 获取的 `inet_diag_msg` 结构体中，rqueue/wqueue 的赋值在 `tcp_diag.c` 中：

```c
// 内核源码：src/linux-5.10/net/ipv4/tcp_diag.c:18-35
// tcp_diag_get_info() 函数（★ 注意不是 inet_diag.c）

static void tcp_diag_get_info(struct sock *sk, struct inet_diag_msg *r,
                              void *_info)
{
    struct tcp_info *info = _info;

    if (inet_sk_state_load(sk) == TCP_LISTEN) {
        // ★ LISTEN 状态：Recv-Q = accept 队列长度，Send-Q = backlog
        r->idiag_rqueue = READ_ONCE(sk->sk_ack_backlog);      // 已完成的连接数
        r->idiag_wqueue = READ_ONCE(sk->sk_max_ack_backlog);  // backlog 上限
    } else if (sk->sk_type == SOCK_STREAM) {
        const struct tcp_sock *tp = tcp_sk(sk);
        // ★ 其他状态：Recv-Q = 接收队列字节数，Send-Q = 发送队列字节数
        r->idiag_rqueue = max_t(int, READ_ONCE(tp->rcv_nxt) -
                                     READ_ONCE(tp->copied_seq), 0);
        r->idiag_wqueue = READ_ONCE(tp->write_seq) - tp->snd_una;
    }
    if (info)
        tcp_get_info(sk, info);  // 填充 tcp_info（ss -i 的数据来源）
}
```

**关键字段解读：**
- `sk_ack_backlog`：当前 accept 队列中已完成三次握手的连接数
- `sk_max_ack_backlog`：listen() 设置的 backlog 上限
- `rcv_nxt - copied_seq`：已到达内核但尚未被用户态 read() 拷贝的字节数
- `write_seq - snd_una`：已发送但尚未被对端 ACK 确认的字节数

#### ★ 性能分析要点

当 LISTEN 状态的 Recv-Q 接近 Send-Q 时：
- 说明 accept 队列即将满
- 新的三次握手完成后会被丢弃（或进入 SYN_RECV 等待）
- 此时应检查：
  1. 应用 accept() 是否太慢
  2. backlog 是否设置太小
  3. somaxconn 是否限制了 backlog

验证命令：
```bash
# 检查 somaxconn
cat /proc/sys/net/core/somaxconn  # 本机 = 4096

# 检查 listen overflow
cat /proc/net/netstat | grep -i listenoverflows
```

---

<!-- 后续问答在此追加 -->
