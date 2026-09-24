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
