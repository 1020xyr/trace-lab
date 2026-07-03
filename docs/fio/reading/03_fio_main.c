/*
 * ======================================================================
 * Step 3: fio.c — main() 入口与启动流程
 * ======================================================================
 *
 * 原始路径：src/fio/fio.c (62 行)
 *
 * fio 的 main() 只有 35 行有效代码，但每个调用都至关重要。
 * 它完成了从命令行参数到 I/O 执行的全部调度工作。
 *
 * fio 有两种运行模式：
 *   1. 本地模式（默认）：直接在本机执行 I/O
 *   2. 客户端/服务器模式：fio --client=host 远程执行
 *
 * 阅读要点：
 *   1. initialize_fio() — 全局初始化
 *   2. parse_options() — 解析命令行参数和 job 文件
 *   3. fio_backend() — 本地模式的执行入口
 *   4. nr_clients 分支 — 客户端/服务器模式
 *
 * 预计阅读时间：5 分钟
 * ======================================================================
 */

/*
 * ======================================================================
 * ★ 启动流程图
 * ======================================================================
 *
 *   命令行: fio --name=test --ioengine=libaio --direct=1 --rw=randwrite
 *     │
 *     ▼
 *   main()
 *     │
 *     ├── initialize_fio(envp)         ← 全局初始化（信号、时间、日志等）
 *     │
 *     ├── fio_server_create_sk_key()   ← 创建 server 共享内存 key
 *     │
 *     ├── parse_options(argc, argv)    ← ★ 解析命令行 / job 文件
 *     │     │                             填充 threads[] 数组
 *     │     │                             每个 --name 对应一个 thread_data
 *     │     ▼
 *     │   threads[] = { td[0], td[1], ... }
 *     │   每个 td 包含：
 *     │     - thread_options o  （配置参数）
 *     │     - ioengine_ops      （引擎接口）
 *     │     - files[]           （目标文件列表）
 *     │
 *     ├── setvbuf(stdout, _IOLBF)      ← 行缓冲（多线程输出不混淆）
 *     │
 *     ├── fio_time_init()              ← 初始化时间基准
 *     │
 *     ├── if (nr_clients):             ← 客户端/服务器模式
 *     │     ├── fio_start_all_clients()
 *     │     └── fio_handle_clients()
 *     │
 *     └── else:                        ← ★ 本地模式（最常见）
 *           └── fio_backend(NULL)      ← 进入 backend.c
 *                 │
 *                 ▼
 *               thread_main()          ← 每个 job 一个线程/进程
 *                 │
 *                 ├── setup_files()    ← 创建/打开文件
 *                 ├── init_io_u()      ← 分配 io_u 池
 *                 └── do_io()          ← ★ I/O 主循环
 */

#include "fio.h"

int main(int argc, char *argv[], char *envp[])
{
	int ret = 1;

	/* ─── 第一步：全局初始化 ─── */
	/*
	 * initialize_fio() 做的事：
	 *   - 设置信号处理（SIGINT/SIGTERM → 优雅退出）
	 *   - 初始化日志系统
	 *   - 初始化随机数生成器
	 *   - 解析环境变量（如 FIO_OUTPUT_FORMAT）
	 *   - 检测系统能力（是否支持 libaio/io_uring 等）
	 */
	if (initialize_fio(envp))
		return 1;

	/* 创建 server 模式的共享内存 key（本地模式不使用） */
	if (fio_server_create_sk_key())
		goto done;

	/* ─── 第二步：★ 解析命令行参数 ─── */
	/*
	 * parse_options() 是 init.c 中的函数，负责：
	 *   1. 解析 --name=xxx 开始的 job 定义
	 *   2. 为每个 job 创建一个 thread_data 结构
	 *   3. 解析 --ioengine, --direct, --rw, --bs 等选项
	 *   4. 加载 I/O 引擎（load_ioengine()）
	 *   5. 验证参数组合的合法性
	 *
	 * 解析完成后：
	 *   - threads[] 数组包含了所有 job 的 thread_data
	 *   - nr_threads = job 数量
	 *   - 每个 thread_data 的 o 字段（thread_options）已填充
	 */
	if (parse_options(argc, argv))
		goto done_key;

	/* 行缓冲 stdout，防止多线程输出交错 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* 初始化时间基准（所有时间戳的起点） */
	fio_time_init();

	if (nr_clients) {
		/*
		 * ─── 客户端/服务器模式 ───
		 * 命令行类似：fio --client=192.168.1.100 job.fio
		 * fio 作为客户端，将 job 发送到远程 fio server 执行
		 */
		set_genesis_time();

		if (fio_start_all_clients())
			goto done_key;
		ret = fio_handle_clients(&fio_client_ops);
	} else
		/*
		 * ─── ★ 本地模式（最常见） ───
		 * fio_backend() 在 backend.c 中实现，负责：
		 *   1. 为每个 job 创建线程或进程（fork/pthread）
		 *   2. 每个线程执行 thread_main()
		 *   3. thread_main() 内部调用 do_io() 执行 I/O
		 *   4. 等待所有线程完成
		 *   5. 汇总并输出统计结果
		 */
		ret = fio_backend(NULL);

done_key:
	fio_server_destroy_sk_key();
done:
	deinitialize_fio();
	return ret;
}


/*
 * ======================================================================
 * 总结：fio 启动到 I/O 执行的完整路径
 * ======================================================================
 *
 *   ┌───────────────────────────────────────────────────────────────┐
 *   │ main()  (fio.c)                                              │
 *   │   ├── initialize_fio()     全局初始化                         │
 *   │   ├── parse_options()      ★ 解析参数，填充 thread_data[]     │
 *   │   └── fio_backend()        进入执行层                         │
 *   │         │                                                     │
 *   │         ▼                                                     │
 *   │   thread_main()  (backend.c)  每个 job 一个线程               │
 *   │         ├── setup_files()    打开文件/设备                     │
 *   │         ├── init_io_u()      分配 io_u 池                     │
 *   │         ├── td_io_init()     调用引擎 init()                  │
 *   │         └── do_io()          ★ I/O 主循环                     │
 *   │               ├── get_io_u()      取 io_u + 填 offset        │
 *   │               ├── td_io_queue()   调用引擎 queue()            │
 *   │               ├── td_io_getevents() 等待完成                  │
 *   │               └── 统计 + 循环                                 │
 *   └───────────────────────────────────────────────────────────────┘
 *
 * 关键洞察：
 *   main() 非常薄——真正的逻辑在 parse_options()（init.c）和
 *   fio_backend()（backend.c）中。fio.c 只负责"粘合"。
 */
