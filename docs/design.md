# lexec 设计

## 定位

lexec 用 C++17 实现 C++26 最终版 `std::execution` 的接口。用户写 `namespace ex = lexec;` 的代码读起来与 `std::execution` 一致，将来把 `lexec::` 换成 `std::execution::` 即可迁移。

两条性能承诺都以测试验证：

- **零堆分配**：默认路径上整条管道不做堆分配。
- **零拷贝**：
  - 同步路径上值既不拷贝也不移动，只转发引用；
  - 跨越异步边界（切换线程、等待兄弟任务、`let_*` 的后继）时，值移动一次存入 operation state，之后以引用交给后续步骤；
  - 拷贝只在用户明确要求时发生，例如对左值 sender 多次 `connect`。

## 已确定的决定

- **编译器**：GCC 10+ / Clang 12+ 与 MSVC（Visual Studio 2022）。MSVC 的全部源文件已在 Compiler Explorer 的 MSVC 19.44 上以 CI 的 Debug、Release、无异常三种选项编译通过；链接和测试运行仍以 Windows CI 为准。
- **算法实现方式**：从一开始就基于 `basic_sender` 框架。
- **异常**：同时支持开启异常和 `-fno-exceptions` 两种构建。关闭异常时，所有用户函数按不会抛出处理，完成签名里不出现 `set_error_t(std::exception_ptr)`，也不生成 try/catch。
- **功能范围**：C++26 最终版 `std::execution`，加上 stdexec 的常用扩展 `static_thread_pool`、`when_any`、`any_sender_of`。
- **优先的性能场景**：CPU 数据并行，以及低延迟任务调度。异步 IO 集成不在本轮范围内。
- **测试与基准**：doctest + nanobench，都通过 FetchContent 引入，只用于测试和基准目标。
- **执行策略**：`bulk` 系列接受的 `seq` / `par` / `par_unseq` / `unseq` 取自 `<execution>`，与 `std::execution` 的策略是同一类型；标准库没有提供时（`__cpp_lib_execution` 未定义，或 `unseq` 所需的 C++20），lexec 定义同名的替代类型，定义 `LEXEC_NO_STD_EXECUTION_POLICY` 可强制使用替代类型。代价：
  - 每个包含 lexec 的编译单元多出约 0.3 s（见 `docs/baselines.md`）；
  - 装有 TBB 头文件时，libstdc++ 以 TBB 实现 `<execution>`，这时即使只包含该头文件，未优化的程序也要链接 TBB。CMake 在配置时检测这种情况，并让 `lexec::lexec` 链接 `TBB::tbb`；不用 CMake 的用户需要自己链接 TBB。

## C++17 的约束与补偿

C++17 是这个模型能成立的最低标准，因为有**保证拷贝消除**：`connect()` 返回的 operation state 不可移动，只有从 C++17 起才能以纯右值直接构造在父 operation state 的成员里。

- **没有 concepts**：用检测惯用法加 `constexpr bool` 变量模板（`is_sender_v`、`is_receiver_of_v`）约束，在 `connect` / `sync_wait` 入口用 `static_assert` 给出可读的报错。
- **没有 `std::stop_token`**：自行实现 `inplace_stop_source/token/callback` 和 `never_stop_token`。
- **没有 consteval 和 constexpr 异常**：completion signatures 在类型层面用 `decltype` 计算。
- **没有协程**：核心库不含 `task` / `as_awaitable`。协程由可选的桥 `lexec/coro/co2.hpp` 对接 co2（C++14 无栈协程库，协议与 C++20 协程同形），见「协程桥」。
- **`[[no_unique_address]]` 是 C++20 特性**：GCC 和 Clang 在 C++17 模式下作为扩展支持，统一封装为 `LEXEC_NO_UNIQUE_ADDRESS`。Clang 18 在嵌套聚合初始化含这种空成员的类型时会崩溃，所以 `detail::tuple` 对空元素改用空基类优化，只有通过构造函数初始化的成员才使用这个宏。保证拷贝消除不适用于 `[[no_unique_address]]` 成员和基类子对象，所以框架只对空的算法状态使用这个宏；不可移动的状态（如 `let_*` 的状态）以普通成员从 prvalue 原地构造。存放子操作的 `inner_ops` 使用 `LEXEC_IMMOVABLE_NO_UNIQUE_ADDRESS`：MSVC 在 `/O2` 下对这种原地构造的 `[[msvc::no_unique_address]]` 成员会写出对象边界（C4789），所以该宏在 MSVC 上为空。

## 分层架构

依赖严格自上而下，下层不知道上层存在。

1. **基础设施（`detail`）**：配置宏、类型列表元编程、精简 tuple、`manual_variant`（不可移动对象的原地构造）。
2. **协议核心**：
   - 完成函数 `set_value` / `set_error` / `set_stopped`；
   - 定制点对象 `connect` / `start` / `get_env`；
   - 模拟的概念 `sender`、`receiver_of`、`operation_state`、`scheduler`；
   - `completion_signatures` 及其变换工具；
   - `transform_sender` 与 domain 分派：`connect` 和带环境的签名计算都作用于变换后的 sender。
3. **环境与取消**：`env` / `prop`，查询 `get_stop_token`、`get_scheduler`、`get_allocator`、`get_completion_scheduler<Tag>`、`get_domain`、`get_completion_domain<Tag>`，以及 stop token 家族。
4. **sender 框架**：
   - `basic_sender<Tag, Data, Children...>`，每个算法只写钩子：`get_completion_signatures`、`get_attrs`、`get_env`、`get_state`、`start`、`complete`；需要自己管理子操作的算法（如 `let_*`）把 `connects_children` 设为 `false`，在状态里连接子 sender；
   - 管道适配器闭包，以及以额外参数作为 sender 数据的适配器（`then`、`let_*`、`write_env` 等）。

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

- `spawn` / `spawn_future`：op 的生命周期脱离调用者的栈；
- `any_sender_of`：类型擦除。不超过 4 个指针且移动不抛异常的 sender 内联保存，构造时不分配；`connect` 时被擦除 sender 的操作类型只有它自己知道，因此分配一次。

线程池上的 `bulk` 不在此列：作业描述符就在 op state 里，见阶段 4。

## 放弃的方案

- **`tag_invoke` 定制**：C++26 已改为成员函数（P2855）。`tag_invoke` 依赖 ADL 大重载集，在没有 concepts 的 C++17 下编译更慢、报错更难读，也会让迁移到 std 变难。
- **future/promise 风格**：每个 continuation 都需要堆上共享状态加同步。
- **默认类型擦除**：阻断内联，只在 ABI 边界作为可选工具。
- **内部使用 `std::tuple` / `std::variant`**：`std::variant` 有 valueless 状态且对不可移动类型不友好；libstdc++ 的 `std::tuple` 是递归继承实现，编译慢且不是聚合体。内部用精简实现，只在标准规定的用户可见位置（`sync_wait` / `sync_wait_with_variant` 的返回值、`into_variant`）使用 std 类型。

## 与 stdexec 的关系

- **借鉴**：成员函数定制点、`__sexpr` 式的钩子描述、completion signatures 变换工具、`inplace_stop_token` 的实现、侵入式 `run_loop`。
- **规避**：编译时间问题。从阶段 1 起记录编译耗时，数据见 `docs/baselines.md`。手段包括：
  - 用 `static_cast<T&&>` 代替 `std::forward`；
  - 基于别名的元函数；
  - 编译器内建（`__type_pack_element`、`__make_integer_seq`）；
  - 减少 SFINAE 层数。

与标准的已知差异：

- `sync_wait` / `sync_wait_with_variant` 位于 `lexec::`，标准中在 `std::this_thread::`。
- `sync_wait_with_variant` 把值直接构造进结果的 variant，每个值只移动一次；标准写成先 `into_variant` 再 `sync_wait`，再把 variant 移出来，要多移动两次整个 variant。返回类型、停止与错误的处理与标准相同。
- `then` / `upon_error` / `upon_stopped` 直接调用函数对象，暂不支持成员指针；标准使用 `std::invoke`。
- 没有 domain 变换时，`connect` 直接连接原 sender；标准的 `default_domain` 会先把右值 sender 移动成一个新值（LWG4368），这里为零拷贝省掉这次移动。公开的 `transform_sender` 仍按标准返回新值。
- `let_*` 和 `when_all` 对任何完成标签都报告同一个完成 domain（`let_*` 为各个第二 sender 与透传通道的公共 domain，`when_all` 为各子 sender 的公共 domain），没有信息时报告 `default_domain`；标准按完成标签分别计算。
- `default_domain::apply_sender`，以及 `sync_wait` / `sync_wait_with_variant` 按 domain 分派，尚未实现。
- 计数作用域的 `join()`：接收者环境没有 `get_start_scheduler` 时，在结束最后一个关联的线程上直接完成；标准此时不能连接。
- `spawn_future`：消费者收到停止请求时立即以 set_stopped 完成，共享状态在派生的操作完成后才销毁（草案的文字在此处销毁得过早）。
- `inplace_stop_source::request_stop` 允许正在执行的回调销毁这个 source（连同还没执行的回调），之后不再访问它；co2 的 `stop_source` 也有这一保证，标准没有写。`when_all`、`when_any`、`stop-when`、`any_sender_of` 都按标准的写法把接收者的停止请求转发给操作自己的 source，子操作若在这次请求里同步完成，接收者可以立即销毁整个操作，source 也随之销毁。stdexec 在 `when_all` / `when_any` 里改为转发期间多占一个计数；照此实现时 TSan 发现，这次加一若落在计数归零之后，操作会完成两次，所以这里在 source 一侧解决，转发方只调用 `request_stop`。
- `parallel_scheduler` 的后端接口以 `lexec::span<std::byte>` 代替 `std::span<std::byte>`：后端是编译进运行时的虚函数，其签名不能随语言模式改变。`receiver_proxy::try_query` 只在接收者的 stop token 本身是 `inplace_stop_token` 时返回它，其余情况返回 `nullopt`（标准允许由实现决定）。关闭异常时，完成签名里没有 `exception_ptr`，后端若报告错误则调用 `std::terminate`。
- 线程池上的 `bulk` 系列把前驱的值移动存入 op state，向下游发送的是这些衰变后的值（标准允许「值或其衰变副本」）；它的 `bulk_unchunked` 每次领取一批下标，仍逐个下标调用函数，但不保证每个下标各在一个执行代理上（标准对此只是推荐做法）。

## 协程桥

`lexec/coro/co2.hpp` 把 lexec 与 [co2](https://github.com/lvyues1994/coro) 接起来，不在 `execution.hpp` 里，需要 co2 的头文件，且只在开启异常时可用（co2 依赖异常）。命名空间是 `lexec::coro`。

- **在 co2 协程里等待 sender**：lexec 的 sender 可以直接 `CO2_AWAIT(sndr)`（`lexec::detail` 里的 `operator_co_await` 经 ADL 被 co2 找到），其他命名空间的 sender 用 `coro::as_awaitable(sndr)`。结果与标准的 sender-awaitable 相同：一个值、`void`，或多个值的 `std::tuple`；错误以异常抛出（`exception_ptr` 原样，`error_code` 变成 `system_error`，其余包装成异常）。
  - co2 把 awaiter 移进帧里的 awaiter 槽，所以 awaiter 在 `await_suspend` 之前只存 sender，到了最终地址才在同一块存储里 connect，op state 由此不必可移动；`schedule(pool)` 这类操作因此放得进 co2 的 64 字节内联槽，不分配。代价是 sender 在 connect 之前多移动一次。
  - 在 `start` 里、在同一线程上同步完成的 sender 不挂起协程，同步 sender 的循环不会加深栈；其他完成都在完成处恢复协程，即使 `start` 还没返回（co2 在调用 `await_suspend` 之前就把协程视为挂起），因此 `schedule` 之后协程一定在调度器的线程上继续。
  - 被等待的 sender 看到的环境只有协程的 stop token：co2 的 `stop_token` 包成 lexec 的可停止 token `coro::stop_token`，其回调就是 co2 的侵入式 `stop_callback`，不分配。
- **co2 的 `Task` 当作 sender**：`coro::as_sender(task)`。op state 里有一个手写的 co2 帧，作为 Task 最终转移的目标，所以除 Task 自己的帧外不分配。接收者的 token 是 `coro::stop_token` 时直接传给 Task；不可停止时传空 token；否则建一个 co2 `stop_source` 并注册 lexec 回调转发停止请求（`stop_source` 分配一次）。
- **co2 的 `Scheduler` 当作 lexec 调度器**：`coro::scheduler{pool}`，排队的是嵌在操作里的帧，不分配。
- **stopped**：co2 没有 stopped 通道，所以等待到 stopped 时向协程抛出 `coro::stopped_error`；以它结束的 Task 被当作 sender 时，又映射回 `set_stopped`。
- **与 co2 的耦合**：启动 Task 用 co2 根适配器共用的 `co2::detail::TaskAccess`，手写帧依赖 `co2::detail::FrameHeader` 的布局。开发构建通过 FetchContent 固定到 co2 的一个提交；`FETCHCONTENT_SOURCE_DIR_CO2` 可以指向本地副本。
- **与 C++26 的差异**：C++26 的 `co_await sndr` 在协程的 `unhandled_stopped()` 处理 stopped，不经过异常；awaiter 超出 co2 的内联槽（例如 MSVC 上 `exception_ptr` 为两个指针宽）时，co2 会为它分配一次。

## 命名与风格

统一使用标准库风格的 snake_case。其余规则：用 `struct` 关键字、east const、不写裸 `new`、严格的 `noexcept` 纪律。抽象接口只出现在冷路径（线程池后端、`parallel_scheduler` 后端）。

## 工程结构

```
lexec/
  CMakeLists.txt  CMakePresets.json
  cmake/         仅用于自身开发构建的选项
  include/lexec/
    execution.hpp  execution_policy.hpp  stop_token.hpp  any_sender_of.hpp（类型擦除，需单独包含）
    detail/      config.hpp meta.hpp tuple.hpp spin_wait.hpp manual_variant.hpp
    core/        completion_tags.hpp completion_signatures.hpp env.hpp queries.hpp domain.hpp
                 transform_sender.hpp receiver.hpp operation_state.hpp sender.hpp
                 sender_traits.hpp scheduler.hpp
    framework/   basic_sender.hpp sender_adaptor_closure.hpp
    algorithms/  just.hpp then.hpp let.hpp read_env.hpp write_env.hpp into_variant.hpp
                 stopped_as.hpp sync_wait.hpp when_all.hpp continues_on.hpp starts_on.hpp bulk.hpp
                 associate.hpp spawn.hpp（spawn / spawn_future） stop_when.hpp ...
    scopes/      counting_scope.hpp（scope 概念、simple_counting_scope、counting_scope）
    schedulers/  run_loop.hpp inline_scheduler.hpp static_thread_pool.hpp parallel_scheduler.hpp
    coro/        co2.hpp（与 co2 协程库的桥，可选）
  src/           static_thread_pool.cpp parallel_scheduler.cpp（默认后端） bwos_queue.hpp（运行时库的私有实现）
  tests/         按层组织，含 static_assert 编译期测试、头文件自包含检查和 -O2 汇编比对；
                 replacement/ 为替换 parallel_scheduler 后端的独立测试程序；coro/ 为协程桥的测试程序
                 （拉取 co2）
  bench/         编译时间探针；pool/ 与 bulk/ 下为与 stdexec 对比的线程池和数据并行基准（stdexec 版以 C++20 编译），bulk/ 另含手写线程组作参照
  examples/
```

- `lexec::lexec`：只含头文件的核心（INTERFACE 目标），除标准库外没有依赖；标准库以 TBB 实现 `<execution>` 时链接 `TBB::tbb`（见「执行策略」）。
- `lexec::runtime`：线程池等需要编译的运行时（STATIC 目标），阶段 3 引入。
- Presets：GCC 和 Clang 各有 debug、release、asan（含 ubsan）、tsan、noexcept 五种。
- 开发构建通过 `prlimit` 给每个编译器进程设 4 GiB 地址空间上限（`LEXEC_COMPILE_MEMORY_LIMIT`），模板实例化失控时编译报错退出，而不是耗尽机器内存。
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

- 内容：
  - `let_value` / `let_error` / `let_stopped`：前驱与第二个操作共用 `detail::manual_variant` 存储，前驱的操作在调用函数之前销毁（P3373）；
  - `read_env`、`write_env`、`unstoppable`、`into_variant`；
  - `stopped_as_optional` / `stopped_as_error`：与标准相同，由 `transform_sender` 降级为 `let_stopped`、`then`、`just` 的组合；
  - 按 C++26 最终版（P3826）实现的 domain：`default_domain`、`get_domain`、`get_completion_domain<Tag>`、带环境参数的 `get_completion_scheduler`、`transform_sender(sndr, env)`；`connect` 和带环境的签名计算都作用于变换后的 sender。
- 验收：每个算法的值、错误、停止三个通道测试；noexcept 传播测试；只能移动的类型的测试；自定义 domain 的值变换、启动变换和多步变换测试；编译时间与阶段 1 对比。

**阶段 3：并发**

- 内容：
  - `when_all` / `when_all_with_variant`，自带 `inplace_stop_source`，任一子任务出错或被取消时取消其余兄弟任务；
  - `schedule` / `starts_on` / `continues_on` / `on` / `schedule_from`；
  - `static_thread_pool`（编译进 `lexec::runtime`）：每个 worker 一个 BWoS 本地队列（32 块 × 8 槽，属主当前块不可被窃取，所以块要小）和一个只由属主取的无锁远程栈；worker 内部的提交进本地队列，外部提交按线程局部计数轮转到各 worker 的远程栈；worker 用「运行 / 休眠 / 已通知」三态加互斥锁休眠，提交方只在目标休眠时才进入系统调用；空闲时先以 pause 自旋轮询，再休眠。数据见 `docs/baselines.md`；
  - MSVC 支持（未验证，见上）；
  - 不含 `split`：它已被 P3682 从 C++26 移除，分叉执行由阶段 5 的 `spawn_future` 覆盖。
- 验收：TSan 全部通过；取消竞态压力测试；与 stdexec 的 `static_thread_pool` 对比调度往返延迟（p50 / p99）和多提交线程下的吞吐。

**阶段 4：数据并行**

- 内容：
  - `bulk` / `bulk_chunked` / `bulk_unchunked`：与标准相同，`bulk` 由 `transform_sender` 降级为 `bulk_chunked`，所以定制 `bulk_chunked` 的 domain 也定制了 `bulk`；默认实现在前驱完成的执行代理上运行，`bulk_chunked` 以整个区间调用一次函数，`bulk_unchunked` 逐个下标调用；
  - 线程池通过 domain 定制 `bulk_chunked` / `bulk_unchunked`，不分配：op state 里只有一个作业描述符，发布到线程池的作业链表；作业切成 min(元素数, 4×worker 数) 块，worker 取任务前先加入还有分块可领的作业，以原子计数领取分块，发布作业的线程自己也立即参与；作业以引用计数管理，最后离开的线程通知下游，这也保证作业的内存在无人访问之后才可能被释放；前驱的值跨线程时移动一次存入 op state；自旋中的 worker 每轮只从 2 个对象窃取，以便尽快看到新作业；
  - `parallel_scheduler` 与 `get_parallel_scheduler()`，以及 `get_forward_progress_guarantee`：
    - 后端接口 `parallel_scheduler_replacement::parallel_scheduler_backend` 与标准相同；每个操作自身就是 `receiver_proxy` / `bulk_item_receiver_proxy`，并为后端预留 128 字节存储；
    - 它的 domain 在任何策略下接管 `bulk_chunked` / `bulk_unchunked`：并行策略按 `shape` 调用后端，其余策略以一次调用跑完全部下标；
    - `lexec::runtime` 的默认后端是一个每硬件线程一个 worker 的 `static_thread_pool`，在预留存储里建立任务和 bulk 作业，不分配；它单独在一个源文件里，程序自己定义 `query_parallel_scheduler_backend` 时，静态库的这个成员就不会被链接（GCC / Clang 上它另外是弱符号）。
- 验收：从 1 到 N 线程的扩展性曲线，与手写 `std::thread` 常驻线程加屏障分块以及 stdexec 对比；分块粒度由基准决定。

**阶段 5：结构化并发与边界工具**

- 内容：
  - `simple_counting_scope` / `counting_scope`、`spawn` / `spawn_future` / `associate`，按 C++26 最终草案（`scope_association` / `scope_token`）实现：
    - 作用域把状态与计数放在一个原子字里，关联与解除关联都是一次原子更新；最后一次解除关联在同一次更新里把正在 join 的作用域变为 joined，其间不会插入新的关联；登记 join 与完成 join 另外持有一把互斥锁；
    - `join()` 异步完成时经接收者的 `get_start_scheduler` 调度；为此新增该查询，`sync_wait` 的环境与 SCHED-ENV 都回答它；
    - `counting_scope` 的 token 以 `stop-when` 包装 sender，实现为操作里的一个 `inplace_stop_source`，作用域的 token 与接收者的 token 都向它请求停止；
    - `associate` 的关联随操作存活，操作销毁后才解除；
    - `spawn` / `spawn_future` 用环境、sender 环境或 `std::allocator` 中的分配器分配状态，释放内存之后才解除关联；`spawn_future` 的完成、消费、停止、放弃各是一个标志位，每一方都只在自己的 `fetch_or` 之后决定由谁投递结果、由谁销毁状态；消费者的停止请求另占一个「取消中」位：派生的操作可能就在这次请求里完成，这期间完成方和消费方都只置位，请求返回后由取消方决定结果和状态的去向；
  - `any_sender_of`（`lexec/any_sender_of.hpp`，不在 `execution.hpp` 里），形状与 stdexec 的新接口相同：`any_receiver<Sigs, queries<R(Q) noexcept...>>` 描述擦除后的接收者，`any_sender<AnyReceiver, SenderQueries>` 是擦除后的 sender，`any_scheduler<AnySender, SchedulerQueries>` 是擦除后的调度器，`any_sender_of<Sigs...>` 是只有完成签名的简写：
    - 擦除后的接收者是一个指针加一张静态 vtable，环境只回答列出的查询和 `get_stop_token`（`inplace_stop_token`）；接收者的 token 是 `inplace_stop_token` 时直接传递，不可停止时给空 token，否则由操作里的 stop source 转发；
    - 不超过 4 个指针、nothrow 可移动的 sender 内联存放，否则放在堆上；connect 总要分配一次，因为操作的类型只有被擦除的 sender 知道；
    - `any_scheduler` 同样内联存放调度器，`schedule()` 的 sender 在属性里报告这个 `any_scheduler` 为完成调度器；两个 `any_scheduler` 相等，当且仅当所持调度器类型相同且相等；
  - `when_any`，语义与 stdexec 相同：第一个完成的子 sender 胜出，不论哪个通道；它的结果衰变后存入 op state，其余子 sender 被请求停止，全部结束后才把结果交给接收者，接收者此时已请求停止则改为 set_stopped。完成签名是各子 sender 签名衰变后的并集加上 set_stopped，存储结果可能抛异常时再加 `set_error_t(std::exception_ptr)`；环境与 stop source 的转发和 `when_all` 共用；
  - `sync_wait_with_variant`：阶段 1 漏掉了，这时补上，与 `sync_wait` 共用等待和存储结果的实现；
  - 循环类算法，需要 trampoline 防止同步完成导致栈溢出。

  在此之前先完成了与 co2 协程库的桥，见「协程桥」。

## 风险

- **编译时间**：C++17 的 SFINAE 比 concepts 更贵，从阶段 1 起持续记录。
- **stop token 的并发正确性**：回调正在另一个线程执行时，注销方必须等待其完成。
- **同步完成的栈深度**：长链或循环在 inline 完成时递归，阶段 5 用 trampoline 解决。
- **类型名的超线性增长**：同一类型在模板实参里重复出现，会让嵌套类型的名字逐层翻倍，编译内存随之指数增长；阶段 1 的 GCC 调试信息和阶段 2 的 `let` 接收者都出现过。深层嵌套的编译时间探针和每个编译进程的内存上限用来及早发现它。
