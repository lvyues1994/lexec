# 基线

## 编译时间（阶段 1）

测量命令：`python3 bench/compile_time/measure.py g++-13 g++-14 clang++-18`。每个探针分别以 `-std=c++17 -O0 -c` 和再加 `-g` 编译 5 次，记录时间中位数和峰值内存；机器为 i7-13700KF（24 线程）。

| 编译器 | `include_only.cpp` `-O0` | `include_only.cpp` `-O0 -g` | `deep_pipeline.cpp` `-O0` | `deep_pipeline.cpp` `-O0 -g` |
|---|---|---|---|---|
| GCC 13.3 | 0.15 s / 69 MiB | 0.16 s / 77 MiB | 0.92 s / 192 MiB | 1.84 s / 326 MiB |
| GCC 14.2 | 0.15 s / 69 MiB | 0.17 s / 79 MiB | 0.93 s / 196 MiB | 1.80 s / 321 MiB |
| Clang 18.1 | 0.18 s / 107 MiB | 0.18 s / 108 MiB | 0.74 s / 183 MiB | 1.27 s / 347 MiB |

`deep_pipeline.cpp` 包含深度为 10、20、40 的 `then` 链（每级是不同的函数对象类型），以及一条 `upon_error | then | upon_stopped` 管道。

最初的探针在递归的函数模板里用 lambda 生成每一级。GCC 在调试信息里写出的 lambda 名字包含外层模板实参，也就包含前一级的整条管道，所以名字每级翻倍：`-g` 下 5 级用 128 MiB，10 级超过 6 GiB。Clang 没有这个问题；不带 `-g` 时 GCC 也没有，因为 mangled 名字会压缩重复部分。用户代码里把「内含 lambda 的泛型函数模板」层层嵌套时，在 GCC `-g` 下会遇到同样的增长。

## 编译时间（阶段 2）

测量方式同阶段 1，新增探针 `let_pipeline.cpp`：深度为 5、20 的 `let_value` 链，以及一条 `let_value | stopped_as_optional | into_variant` 管道。

| 编译器 | `include_only.cpp` `-O0` / `-O0 -g` | `deep_pipeline.cpp` `-O0` / `-O0 -g` | `let_pipeline.cpp` `-O0` / `-O0 -g` |
|---|---|---|---|
| GCC 13.3 | 0.15 s / 0.16 s | 0.97 s / 1.91 s | 0.84 s / 1.32 s |
| GCC 14.2 | 0.16 s / 0.18 s | 0.99 s / 1.84 s | 0.86 s / 1.34 s |
| Clang 18.1 | 0.19 s / 0.18 s | 0.84 s / 1.36 s | 0.68 s / 0.92 s |

峰值内存都在 360 MiB 以内。

- **与阶段 1 相比**：在同一会话里用两版头文件编译同一个 `deep_pipeline.cpp`，GCC 从 0.91 s 到 0.97 s（约 +7%），Clang 从 0.73 s 到 0.83 s（约 +14%）。其中 domain 分派（每次 `connect` 和每次带环境的签名计算都要确定完成 domain 与起始 domain）约占 GCC 的 3%、Clang 的 7%，其余分散在框架的扩充中。没有变换时，`connect` 直接调用 sender 的 `connect`，不经过额外的函数层；否则 `-O0` 下每个操作多出一个函数，GCC 会再慢约 8%。
- **`let` 接收者类型的教训**：前驱的接收者最初是 `let_child_receiver<State, Rcvr>`，而 `State` 本身已包含 `Rcvr`，于是接收者类型名每嵌套一层就翻倍。不带 `-g` 时，嵌套 14 层用 473 MiB，20 层超过 4 GiB。改为 `let_child_receiver<SetTag, Sndr, Rcvr>` 后，20 层只用 153 MiB。
- **编译内存保护**：开发构建通过 `prlimit` 给每个编译器进程设 4 GiB 地址空间上限（`LEXEC_COMPILE_MEMORY_LIMIT`，设为 0 关闭）。同类回归会让那个编译单元报错退出，而不是耗尽机器内存；对最初的 GCC 调试信息问题，编译在 6.4 秒时退出。

## 编译时间（阶段 4）

测量方式同阶段 1。头文件新增 `bulk` 系列，以及取自 `<execution>` 的执行策略。

| 编译器 | `include_only.cpp` `-O0` / `-O0 -g` | `deep_pipeline.cpp` `-O0` / `-O0 -g` | `let_pipeline.cpp` `-O0` / `-O0 -g` |
|---|---|---|---|
| GCC 13.3 | 0.46 s / 0.49 s | 1.29 s / 2.30 s | 1.23 s / 1.73 s |
| Clang 18.1 | 0.43 s / 0.44 s | 1.09 s / 1.71 s | 1.03 s / 1.24 s |

- **增量几乎全部来自 `<execution>`**：定义 `LEXEC_NO_STD_EXECUTION_POLICY`（不包含 `<execution>`，改用 lexec 自己的策略类型）时，`include_only.cpp` 为 GCC 0.16 s、Clang 0.20 s，与阶段 2 相当；包含时为 0.45 s、0.44 s。本机装有 oneTBB 的头文件，libstdc++ 因此以 TBB 实现 `<execution>`，这部分开销也来自 TBB 的头文件。
- **峰值内存**：GCC `deep_pipeline.cpp -g` 最高，为 485 MiB。

## 线程池（阶段 3）

测量命令：`cmake --preset gcc-bench`、`cmake --build --preset gcc-bench`，然后 `python3 bench/pool/compare.py build/gcc-bench 5`。两个库各用 8 个 worker 的 `static_thread_pool`，交替各运行 5 次，每项取中位数；stdexec 为提交 `ead186b`，以 C++20 编译。

| 指标 | lexec | stdexec |
|---|---|---|
| 调度往返延迟 p50 / p99 / p99.9（`sync_wait(schedule \| then)`） | 1.85 / 4.2 / 6.6 µs | 3.44 / 6.0 / 13.1 µs |
| 吞吐，1 个提交线程 | 1120 万任务/秒 | 1040 万任务/秒 |
| 吞吐，4 个提交线程 | 1560 万任务/秒 | 2510 万任务/秒 |
| 吞吐，8 个提交线程 | 1960 万任务/秒 | 5130 万任务/秒 |
| 池内任务向池扇出 8 个子任务 | 2.77 µs | 3.57 µs |

- 往返延迟的 p50 呈双峰：worker 在主线程进入等待之前接走任务时约 0.8 µs，否则多一次 futex 唤醒，约 3 µs。
- 多提交线程时落后的原因：所有提交线程都对同一个 worker 远程栈的栈顶做 CAS；stdexec 为每个提交线程分配各自的一组远程队列，提交之间没有争用。阶段 4 发现另一半原因是空闲 worker 自旋时大范围窃取，减少窃取后差距大半消失，见「数据并行（阶段 4）」。
- 调优中排除的做法：远程提交时另外唤醒一个休眠 worker（每次提交都触发 futex，吞吐降到约 300 万任务/秒）；允许空闲 worker 拿走别人的远程队列（与提交线程争用同一批栈顶）；自旋中使用 `yield`（worker 对新任务反应变慢，p50 退到约 3.6 µs）。

## 数据并行（阶段 4）

测量命令：`cmake --preset gcc-bench`、`cmake --build --preset gcc-bench`，然后 `python3 bench/bulk/compare.py build/gcc-bench 5`。三种实现交替各运行 5 次，每项取中位数：

- **lexec / stdexec**：`sync_wait(schedule(pool) | bulk(par, n, f))`，线程池有 T 个 worker；时间包括跳到线程池和回到等待线程；
- **手写**（`bench/bulk/manual_bulk_bench.cpp`）：T−1 个常驻 `std::thread` 加上调用线程，各算均分的一段；辅助线程在作业之间自旋，从不需要唤醒，也没有调度器居中。

工作负载：`compute` 为 65536 个元素、每个 256 步相互依赖的乘加；`saxpy` 为 1600 万个 `float` 的 `y = 3x + y`；`small` 为 1024 个元素、每个一次加法。机器为 i7-13700KF（8 个性能核带超线程，加 8 个能效核，共 24 线程）。

| 负载 | 线程数 | lexec | stdexec | 手写 |
|---|---|---|---|---|
| `compute` | 1 | 4578 µs | 4582 µs | 4561 µs |
| `compute` | 8 | 741 µs（6.2×） | 735 µs（6.2×） | 718 µs（6.4×） |
| `compute` | 16 | 378 µs（12.1×） | 406 µs（11.3×） | 368 µs（12.4×） |
| `compute` | 24 | 280 µs（16.4×） | 337 µs（13.6×） | 241 µs（19.0×） |
| `saxpy` | 1 / 8 / 24 | 5304 / 2601 / 2863 µs | 5308 / 2562 / 2835 µs | 5271 / 2513 / 3026 µs |
| `small` | 1 / 8 / 16 / 24 | 1.9 / 4.5 / 6.2 / 9.1 µs | 0.7 / 8.6 / 13.2 / 20.2 µs | 0.1 / 0.7 / 1.6 / 1.7 µs |

- **计算密集**：到 16 线程，lexec 的加速比与手写的均分相差不到 3%。加入能效核后，均分受最慢的线程拖累，而动态领取的分块会避开慢线程；手写版在同一台机器上也出现过 24 线程慢到 3000 µs 的一次。
- **内存带宽**：三者在 8 线程左右到达约 2 倍的上限，即约 75 GB/s。
- **小作业**：时间几乎全是开销。在同一个线程池上拆开测量：跳到线程池再回来约 1 µs；`bulk(seq)` 在一个 worker 上算完这 1024 个元素，却要 2.4–3 µs，因为每次接手的 worker 不同，4 KiB 的数据要从上一个核的缓存搬过来。并行时，各个分块每次落在不同的线程上，搬运更多。手写版每个线程每次都算同一段，数据留在各自的缓存里，所以快一个数量级。这是动态分块用负载均衡换来的代价；stdexec 在各线程数下都比 lexec 慢一倍左右。
- **分块粒度**：每个 worker 分 1 / 2 / 4 / 8 块时，`compute` 在 8 线程为 728 / 729 / 741 / 647 µs，`small` 在 24 线程为 8.8 / 9.4 / 10.7 / 15.6 µs。取 4 块：更细的分块在超线程和能效核上平衡得更好，但小作业的开销增长更快。
- **排除的做法**：作业链表改用自旋锁。十几个 worker 同时加入作业时，自旋的兄弟超线程拖慢持锁者，`small` 在 16 / 24 线程退到 7.6 / 16 µs，不如互斥锁的 6.6 / 10.7 µs。

同一阶段对调度器的一处调整：自旋中的 worker 每轮只从 2 个随机对象窃取，而不是 2×worker 数个，以便更快回到自己的远程提交和 bulk 作业上。阶段 3 的指标随之变为（与 stdexec 同机同时测量）：往返延迟 p50 / p99 / p99.9 为 1.93 / 3.7 / 5.5 µs；吞吐在 1、4、8 个提交线程时为 1400 万、2470 万、2730 万任务/秒（stdexec 为 650 万、2700 万、5160 万）；扇出 2.95 µs。多提交线程时的差距因此大半消失，说明阶段 3 分析的另一半原因是空闲 worker 大范围窃取造成的争用。

## 零开销（阶段 1）

以下各项都由测试持续检查：

- **对象大小**：框架版 `then` 的 operation state 与手写 `then` 的 `sizeof` 相同（`tests/zero_overhead/reference_then_test.cpp`）。
- **生成代码**：`-O2` 下，直接调用函数、框架版 `then`、手写 `then` 生成相同的指令（`tests/asm/`）。GCC 13 为 `lea 0x1(%rdi,%rdi,2),%eax; ret`，Clang 18 为 `lea (%rdi,%rdi,2),%eax; inc %eax; ret`。
- **拷贝与移动**（`tests/zero_overhead/copy_move_test.cpp`）：
  - 执行中产生的值经过同步适配器：0 次拷贝，0 次移动；
  - `sync_wait` 保存最终结果：1 次移动；
  - 构造并连接右值管道 `just(x) | then(f)`：0 次拷贝，3 次移动；
  - 连接左值 sender：1 次拷贝。
- **堆分配**：同步管道和跨线程完成到 `run_loop` 都不分配（`tests/zero_overhead/allocation_test.cpp`，sanitizer 构建中不运行）。

## 零开销（阶段 2）

- **domain 分派**：没有变换时，`connect` 连接原 sender 本身，右值管道的移动次数仍是阶段 1 的 3 次。
- **拷贝与移动**：
  - `let_value`：前驱的值移动 1 次存入 `let` 的状态，之后以左值引用交给函数，0 次拷贝；
  - `into_variant`：每个值移动 1 次存入 variant，0 次拷贝。
- **堆分配**：由 `let_value`、`stopped_as_optional`、`into_variant` 组成的管道不分配。

## 零开销（阶段 3）

- **拷贝与移动**：`continues_on` 和 `when_all` 都是每个值移动 1 次存入操作，0 次拷贝。
- **堆分配**：在 `static_thread_pool` 上调度、`on` 到另一个线程的 `run_loop` 再回来，都不分配。

## 零开销（阶段 4）

- **拷贝与移动**：
  - `bulk` 系列在前驱完成的执行代理上运行时，值以左值引用交给函数、再以右值引用交给下游，0 次拷贝，0 次移动；
  - 在 `static_thread_pool` 上并行运行时，值移动 1 次存入 op state，0 次拷贝（`tests/schedulers/pool_bulk_test.cpp`）。
- **堆分配**：`bulk` 与 `bulk_chunked` 组成的管道不分配；在 `static_thread_pool` 上并行运行 `bulk` 也不分配。
