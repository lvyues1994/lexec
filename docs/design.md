# lexec 设计

## 定位

lexec 用 C++17 实现 C++26 最终版 `std::execution` 的接口。用户写 `namespace ex = lexec;` 的代码读起来与 `std::execution` 一致，将来把 `lexec::` 换成 `std::execution::` 即可迁移。

两条性能承诺都以测试验证：

- **零堆分配**：默认路径上整条管道不做堆分配。
- **零拷贝**：
  - 同步路径上值既不拷贝也不移动，只转发引用；
  - 跨越异步边界（切换线程、等待兄弟任务、`let_*` 的后继）时，值移动一次存入 operation state，之后以引用交给后续步骤；
  - 拷贝只在用户明确要求时发生，例如对左值 sender 多次 `connect`，或 `split` 有多个消费者。

## 已确定的决定

- **编译器**：GCC 10+ / Clang 12+，先只支持 Linux，MSVC 在阶段 3 加入。
- **算法实现方式**：从一开始就基于 `basic_sender` 框架。
- **异常**：同时支持开启异常和 `-fno-exceptions` 两种构建。关闭异常时，所有用户函数按不会抛出处理，完成签名里不出现 `set_error_t(std::exception_ptr)`，也不生成 try/catch。
- **功能范围**：C++26 最终版 `std::execution`，加上 stdexec 的常用扩展 `static_thread_pool`、`when_any`、`any_sender_of`。
- **优先的性能场景**：CPU 数据并行，以及低延迟任务调度。异步 IO 集成不在本轮范围内。
- **测试与基准**：doctest + nanobench，都通过 FetchContent 引入，只用于测试和基准目标。

## C++17 的约束与补偿

C++17 是这个模型能成立的最低标准，因为有**保证拷贝消除**：`connect()` 返回的 operation state 不可移动，只有从 C++17 起才能以纯右值直接构造在父 operation state 的成员里。

- **没有 concepts**：用检测惯用法加 `constexpr bool` 变量模板（`is_sender_v`、`is_receiver_of_v`）约束，在 `connect` / `sync_wait` 入口用 `static_assert` 给出可读的报错。
- **没有 `std::stop_token`**：自行实现 `inplace_stop_source/token/callback` 和 `never_stop_token`。
- **没有 consteval 和 constexpr 异常**：completion signatures 在类型层面用 `decltype` 计算。
- **没有协程**：核心库不含 `task` / `as_awaitable`，将来可提供仅在 C++20 下启用的可选头文件。
- **`[[no_unique_address]]` 是 C++20 特性**：GCC 和 Clang 在 C++17 模式下作为扩展支持，统一封装为 `LEXEC_NO_UNIQUE_ADDRESS`。Clang 18 在嵌套聚合初始化含这种空成员的类型时会崩溃，所以 `detail::tuple` 对空元素改用空基类优化，只有通过构造函数初始化的成员才使用这个宏。

## 分层架构

依赖严格自上而下，下层不知道上层存在。

1. **基础设施（`detail`）**：配置宏、类型列表元编程、精简 tuple、`manual_lifetime`、原地构造辅助。
2. **协议核心**：
   - 完成函数 `set_value` / `set_error` / `set_stopped`；
   - 定制点对象 `connect` / `start` / `get_env`；
   - 模拟的概念 `sender`、`receiver_of`、`operation_state`、`scheduler`；
   - `completion_signatures` 及其变换工具。
3. **环境与取消**：`env` / `prop`，查询 `get_stop_token`、`get_scheduler`、`get_allocator`、`get_completion_scheduler<Tag>`、`get_domain`，以及 stop token 家族。
4. **sender 框架**：
   - `basic_sender<Tag, Data, Children...>`，每个算法只写钩子：`get_completion_signatures`、`get_attrs`、`get_env`、`get_state`、`start`、`complete`；
   - 管道适配器闭包；
   - `transform_sender` 与 domain 分派。

   每个 sender 都能拆成「标签、数据、子 sender」，线程池等后端替换算法（如 `bulk`）依赖这个结构。
5. **算法**：工厂、适配器、消费者、async_scope。
6. **执行资源**：`inline_scheduler`、`run_loop`、`static_thread_pool`，之后是可替换后端的 `parallel_scheduler`。这是唯一包含需要编译的运行时代码的一层，放在抽象接口之后（冷路径）；热路径全部静态分派。

第 1–3 层是地基，在任何算法之前稳定下来。类型擦除（`any_sender_of`）和 C++20 协程桥放在最后，作为可选的边界工具。

## 核心协议

形状与 C++26 一致：

```cpp
namespace ex = lexec;

struct my_receiver {
    using receiver_concept = ex::receiver_t;
    void set_value(int v) && noexcept;
    void set_stopped() && noexcept;
    auto get_env() const noexcept -> my_env;
};

template <class Rcvr>
struct my_op {
    using operation_state_concept = ex::operation_state_t;
    my_op(my_op &&) = delete;
    void start() & noexcept;
    Rcvr rcvr;
};

struct my_sender {
    using sender_concept = ex::sender_t;
    using completion_signatures =
        ex::completion_signatures<ex::set_value_t(int), ex::set_stopped_t()>;

    template <class Rcvr>
    auto connect(Rcvr rcvr) && -> my_op<Rcvr>;
    auto get_env() const noexcept -> my_attrs;
};
```

签名依赖环境的 sender 声明 `template <class Self, class... Env> static auto get_completion_signatures() -> completion_signatures<...>;`，只有声明，供 `decltype` 使用。

不变量（测试中由 `checked_receiver` 检查）：

1. **惰性**：`start()` 之前什么都不做。
2. **恰好完成一次**：每个启动的 op，三个完成函数中恰好调用一个，且只调用一次。
3. **`noexcept`**：`start` 和三个完成函数都是 `noexcept`，错误只走 `set_error`；`connect` 允许抛异常。
4. **地址稳定**：op state 从 `start` 到完成之间地址不变。
5. **完成后不再访问 `this`**：接收方可能在完成回调里销毁整个 op。
6. **签名诚实**：只调用声明过的完成；`set_error_t(std::exception_ptr)` 只在用户函数可能抛异常时出现。

## 数据与存储

值只存放在 op state 里；子 op 作为父 op 的成员嵌套，整条管道是一个连续对象，通常位于 `sync_wait` 的栈帧上。子接收者只是一个指向父 op 的指针：

```cpp
template <class Child, class Fn, class Rcvr>
struct then_op {
    struct child_receiver {
        using receiver_concept = receiver_t;
        then_op *op;

        template <class... Vs>
        void set_value(Vs &&...vs) && noexcept {
            op->complete(static_cast<Vs &&>(vs)...);
        }
        // 返回类型必须显式写出：推导它需要 then_op 完整，而此时 then_op 尚未定义完。
        auto get_env() const noexcept -> env_of_t<Rcvr> { return lexec::get_env(op->rcvr); }
    };

    explicit then_op(Child &&child_, Fn &&fn_, Rcvr &&rcvr_)
        : rcvr(static_cast<Rcvr &&>(rcvr_)), fn(static_cast<Fn &&>(fn_)),
          child(lexec::connect(static_cast<Child &&>(child_), child_receiver{this})) {}

    void start() & noexcept { lexec::start(child); }

    template <class... Vs>
    void complete(Vs &&...vs) noexcept;

    Rcvr rcvr;
    LEXEC_NO_UNIQUE_ADDRESS Fn fn;
    connect_result_t<Child, child_receiver> child;
};
```

不用就不付费：

- 接收端的 stop token 是 `never_stop_token` 时，编译期跳过所有 stop callback 注册。
- 用户函数为 `noexcept` 时，不生成 try/catch，不加异常签名，下游不实例化错误路径。
- 调度器队列是侵入式的：`schedule` 的 op state 本身就是队列节点（一个 `next` 指针加一个函数指针），入队不分配。

必须分配的地方只有以下几处，分配器都取自 `get_allocator(env)`：

- `split`：多个消费者共享状态；
- `spawn` / `spawn_future`：op 的生命周期脱离调用者的栈；
- `any_sender_of`：类型擦除；
- 线程池上 `bulk` 的分块状态：是否需要，由阶段 4 的基准决定。

## 放弃的方案

- **`tag_invoke` 定制**：C++26 已改为成员函数（P2855）。`tag_invoke` 依赖 ADL 大重载集，在没有 concepts 的 C++17 下编译更慢、报错更难读，也会让迁移到 std 变难。
- **future/promise 风格**：每个 continuation 都需要堆上共享状态加同步。
- **默认类型擦除**：阻断内联，只在 ABI 边界作为可选工具。
- **内部使用 `std::tuple` / `std::variant`**：`std::variant` 有 valueless 状态且对不可移动类型不友好；libstdc++ 的 `std::tuple` 是递归继承实现，编译慢且不是聚合体。内部用精简实现，只在标准规定的用户可见位置（`sync_wait` 返回值、`into_variant`）使用 std 类型。

## 与 stdexec 的关系

- **借鉴**：成员函数定制点、`__sexpr` 式的钩子描述、completion signatures 变换工具、`inplace_stop_token` 的实现、侵入式 `run_loop`。
- **规避**：编译时间问题。从阶段 1 起记录编译耗时，数据见 `docs/baselines.md`。手段包括：
  - 用 `static_cast<T&&>` 代替 `std::forward`；
  - 基于别名的元函数；
  - 编译器内建（`__type_pack_element`、`__make_integer_seq`）；
  - 减少 SFINAE 层数。

与标准的已知差异：

- `sync_wait` 位于 `lexec::sync_wait`，标准中是 `std::this_thread::sync_wait`。
- `then` / `upon_error` / `upon_stopped` 直接调用函数对象，暂不支持成员指针；标准使用 `std::invoke`。

## 命名与风格

统一使用标准库风格的 snake_case。其余规则：用 `struct` 关键字、east const、不写裸 `new`、严格的 `noexcept` 纪律。抽象接口只出现在冷路径（线程池后端、`parallel_scheduler` 后端）。

## 工程结构

```
lexec/
  CMakeLists.txt  CMakePresets.json
  cmake/         仅用于自身开发构建的选项
  include/lexec/
    execution.hpp  stop_token.hpp
    detail/      config.hpp meta.hpp tuple.hpp spin_wait.hpp manual_lifetime.hpp
    core/        completion_tags.hpp completion_signatures.hpp env.hpp queries.hpp
                 receiver.hpp operation_state.hpp sender.hpp sender_traits.hpp scheduler.hpp
    framework/   basic_sender.hpp sender_adaptor_closure.hpp transform_sender.hpp
    algorithms/  just.hpp then.hpp sync_wait.hpp let.hpp when_all.hpp continues_on.hpp bulk.hpp ...
    schedulers/  run_loop.hpp inline_scheduler.hpp static_thread_pool.hpp
  src/           static_thread_pool.cpp
  tests/         按层组织，含 static_assert 编译期测试、头文件自包含检查和 -O2 汇编比对
  bench/         编译时间探针；之后另含一个 C++20 目标，用于和 stdexec 对比
  examples/
```

- `lexec::lexec`：只含头文件的核心（INTERFACE 目标），零依赖。
- `lexec::runtime`：线程池等需要编译的运行时（STATIC 目标），阶段 3 引入。
- Presets：GCC 和 Clang 各有 debug、release、asan（含 ubsan）、tsan、noexcept 五种。
- 测试以 `-std=c++17` 构建，警告全开并使用 `-Werror`；CI 另外以 C++20 / C++23 构建，并在 GCC 10 / Clang 12 上验证最低版本。

## 实施计划

**阶段 0：工程骨架**

- 内容：CMake 目标、presets、doctest、编译矩阵、CI。
- 验收：所有 preset 下构建和测试通过。

**阶段 1：协议地基与框架**

- 内容：
  - 第 1–3 层的全部内容；
  - `basic_sender` 框架；
  - 基于框架实现 `just` / `just_error` / `just_stopped`、`then` / `upon_error` / `upon_stopped`、`run_loop`、`sync_wait`。
- 验收：
  - completion signatures 的编译期测试；
  - 拷贝/移动计数测试，证明 `just(x) | then(f) | then(g)` 满足零拷贝定义；
  - 统计全局 `operator new` 调用次数，证明零分配；
  - stop token 并发注销的压力测试在 TSan 下通过；
  - 测试中保留一个手写的 `then` 作为参照：框架版 op state 的 `sizeof` 与之相同，`-O2` 下生成的汇编也相同；
  - 建立编译时间基线。

**阶段 2：单线程算法与定制**

- 内容：`let_*`（连同它需要的 `detail/manual_lifetime`）、`read_env`、`write_env`、`into_variant`、`stopped_as_optional/error`、`unstoppable`、`transform_sender` 与 domain（含 `get_domain` 查询）。
- 验收：每个算法的值、错误、停止三个通道测试；noexcept 传播测试；只能移动的类型的测试。

**阶段 3：并发**

- 内容：
  - `when_all` / `when_all_with_variant`，自带 `inplace_stop_source`，任一子任务出错或被取消时取消其余兄弟任务；
  - `schedule` / `starts_on` / `continues_on` / `on` / `schedule_from`；
  - `static_thread_pool`：每个 worker 一个本地队列加工作窃取，任务节点侵入式，空闲时先自旋再休眠；自旋时长和窃取策略由基准决定；
  - `split`；
  - MSVC 支持。
- 验收：TSan 全部通过；取消竞态压力测试；与 stdexec 的 `static_thread_pool` 对比调度往返延迟（p50 / p99）和多提交线程下的吞吐。

**阶段 4：数据并行**

- 内容：`bulk` / `bulk_chunked` / `bulk_unchunked`；线程池通过 domain 定制 `bulk_chunked`，按 worker 数切块，最后完成的块通知下游。
- 验收：从 1 到 N 线程的扩展性曲线，与手写 `std::thread` 分块以及 stdexec 对比。

**阶段 5：结构化并发与边界工具**

- 内容：
  - `simple_counting_scope` / `counting_scope`、`spawn` / `spawn_future` / `associate`；
  - `any_sender_of`、`when_any`；
  - 循环类算法，需要 trampoline 防止同步完成导致栈溢出；
  - 可选的 C++20 协程桥。

## 风险

- **编译时间**：C++17 的 SFINAE 比 concepts 更贵，从阶段 1 起持续记录。
- **stop token 的并发正确性**：回调正在另一个线程执行时，注销方必须等待其完成。
- **同步完成的栈深度**：长链或循环在 inline 完成时递归，阶段 5 用 trampoline 解决。
