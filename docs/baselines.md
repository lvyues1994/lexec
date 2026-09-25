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
- 多提交线程时落后的原因：所有提交线程都对同一个 worker 远程栈的栈顶做 CAS；stdexec 为每个提交线程分配各自的一组远程队列，提交之间没有争用。这是下一步的调优项。
- 调优中排除的做法：远程提交时另外唤醒一个休眠 worker（每次提交都触发 futex，吞吐降到约 300 万任务/秒）；允许空闲 worker 拿走别人的远程队列（与提交线程争用同一批栈顶）；自旋中使用 `yield`（worker 对新任务反应变慢，p50 退到约 3.6 µs）。

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

- **拷贝与移动**：`bulk` 系列在前驱完成的执行代理上运行时，值以左值引用交给函数、再以右值引用交给下游，0 次拷贝，0 次移动。
- **堆分配**：`bulk` 与 `bulk_chunked` 组成的管道不分配。
