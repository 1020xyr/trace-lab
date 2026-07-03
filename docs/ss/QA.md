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

ss 获取的 `inet_diag_msg` 结构体中：

```c
// 内核源码：net/ipv4/inet_diag.c
// inet_csk_diag_fill() 函数中：

if (sk->sk_state == TCP_LISTEN) {
    // LISTEN 状态：Recv-Q = accept 队列长度，Send-Q = backlog
    r->idiag_rqueue = sk->sk_ack_backlog;     // 已完成的连接数
    r->idiag_wqueue = sk->sk_max_ack_backlog; // backlog 上限
} else {
    // 其他状态：Recv-Q = 接收队列字节数，Send-Q = 发送队列字节数
    r->idiag_rqueue = max_t(int, tp->rcv_nxt - tp->copied_seq, 0);
    r->idiag_wqueue = tp->write_seq - tp->snd_una;
}
```

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
