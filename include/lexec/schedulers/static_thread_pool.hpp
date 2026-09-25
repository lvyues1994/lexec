#pragma once

#include <lexec/algorithms/bulk.hpp>
#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/manual_variant.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>
#include <lexec/execution_policy.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <type_traits>

namespace lexec {

namespace detail {

// A unit of work queued on a static_thread_pool, embedded in the operation that runs it.
struct pool_task {
    pool_task *next = nullptr;
    void (*execute_fn)(pool_task *) noexcept = nullptr;
};

// A data-parallel job, embedded in the operation that runs it. The operation sets the
// functions and the size; the pool cuts [0, size) into chunks, hands them out to the
// threads that join, and calls complete once all have run.
struct pool_bulk_job {
    void (*run_chunk)(pool_bulk_job *, std::size_t begin, std::size_t end) noexcept = nullptr;
    void (*complete)(pool_bulk_job *) noexcept = nullptr;
    std::size_t size = 0;

    pool_bulk_job *next = nullptr;
    std::size_t chunk_count = 0;
    std::atomic<std::size_t> next_chunk{0};
    std::atomic<std::uint32_t> refs{0};
};

struct thread_pool_impl;

// Queues the task on the pool; defined in lexec::runtime.
void enqueue(thread_pool_impl &pool, pool_task *task) noexcept;

// Runs the job on the calling thread together with the pool's workers; defined in
// lexec::runtime. The job may complete, on any of those threads, before this returns.
void run_bulk(thread_pool_impl &pool, pool_bulk_job &job) noexcept;

template <class Rcvr>
struct pool_operation;

struct pool_sender;
struct pool_scheduler;

template <bool Chunked, class Child, class Shape, class Fn>
struct pool_bulk_sender;

template <class Sndr>
using bulk_child_attrs_t = env_of_t<typename remove_cvref_t<Sndr>::template child_type<0> const &>;

template <class Sndr, class Env>
using bulk_child_scheduler_t = completion_scheduler_result_t<set_value_t, bulk_child_attrs_t<Sndr>, Env>;

// The bulk senders the pool runs in parallel: those whose policy allows it, and whose
// predecessor completes on a pool, which is the one to run them.
template <class Sndr, class Env, class = void>
inline constexpr bool is_pool_bulk_v = false;

template <class Sndr, class Env>
inline constexpr bool is_pool_bulk_v<
    Sndr, Env, std::enable_if_t<is_one_of_v<tag_of_t<Sndr>, bulk_chunked_t, bulk_unchunked_t>>> =
    is_parallel_policy_v<typename data_of_t<Sndr>::policy_type> and
    std::is_same_v<detected_or_t<void, bulk_child_scheduler_t, Sndr, Env>, pool_scheduler>;

struct pool_domain {
    template <class Sndr, class Env, std::enable_if_t<is_pool_bulk_v<Sndr, Env>, int> = 0>
    auto transform_sender(set_value_t, Sndr &&sndr, Env const &env) const;
};

struct pool_scheduler {
    using scheduler_concept = scheduler_t;

    pool_sender schedule() const noexcept;

    template <class... Env>
    constexpr pool_domain query(get_completion_domain_t<set_value_t>, Env const &...) const noexcept {
        return {};
    }

    friend bool operator==(pool_scheduler const lhs, pool_scheduler const rhs) noexcept { return lhs.pool == rhs.pool; }
    friend bool operator!=(pool_scheduler const lhs, pool_scheduler const rhs) noexcept { return lhs.pool != rhs.pool; }

    thread_pool_impl *pool;
};

template <class... Tags>
struct pool_attrs {
    template <class Tag, class... Env, std::enable_if_t<is_one_of_v<Tag, Tags...>, int> = 0>
    pool_scheduler query(get_completion_scheduler_t<Tag>, Env const &...) const noexcept {
        return pool_scheduler{pool};
    }

    thread_pool_impl *pool;
};

struct pool_sender {
    using sender_concept = sender_t;
    using completion_signatures = lexec::completion_signatures<set_value_t(), set_stopped_t()>;

    template <class Rcvr>
    pool_operation<Rcvr> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {pool, static_cast<Rcvr &&>(rcvr)};
    }

    pool_attrs<set_value_t, set_stopped_t> get_env() const noexcept { return {pool}; }

    thread_pool_impl *pool;
};

template <class Rcvr>
struct pool_operation : pool_task {
    using operation_state_concept = operation_state_t;

    pool_operation(thread_pool_impl *pool_, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : pool_task{nullptr, &execute_impl}, pool(pool_), rcvr(static_cast<Rcvr &&>(rcvr_)) {}

    pool_operation(pool_operation &&) = delete;

    void start() & noexcept { detail::enqueue(*pool, this); }

    static void execute_impl(pool_task *const task) noexcept {
        auto &self = *static_cast<pool_operation *>(task);
        if (lexec::get_stop_token(lexec::get_env(self.rcvr)).stop_requested()) {
            lexec::set_stopped(static_cast<Rcvr &&>(self.rcvr));
        } else {
            lexec::set_value(static_cast<Rcvr &&>(self.rcvr));
        }
    }

    thread_pool_impl *pool;
    Rcvr rcvr;
};

inline pool_sender pool_scheduler::schedule() const noexcept { return pool_sender{pool}; }

// The function runs on several threads with lvalues of the values, which are therefore
// moved into the operation first.
template <bool Chunked, class Fn, class Shape, class... Vs>
struct pool_bulk_call {
    using call = bulk_call<Chunked, Fn, Shape, std::decay_t<Vs>...>;
    static constexpr bool callable = call::callable;
    static constexpr bool nothrow = call::nothrow and is_nothrow_decay_copyable_t<Vs...>::value;
};

template <bool Chunked, class Fn, class Shape>
struct pool_bulk_transforms {
    template <class... Vs>
    using on_value = typename bulk_value_completions<pool_bulk_call<Chunked, Fn, Shape, Vs...>, std::decay_t<Vs>...>::type;
};

template <bool Chunked, class CvChild, class Shape, class Fn, class... Env>
using pool_bulk_completions_t =
    transform_completion_signatures<completion_signatures_of_t<CvChild, fwd_env_t<Env>...>, completion_signatures<>,
                                    pool_bulk_transforms<Chunked, Fn, Shape>::template on_value>;

template <class... Vs>
using decayed_tuple_t = tuple<std::decay_t<Vs>...>;

template <class Sigs>
using pool_bulk_values_t = rename_t<unique_t<gather_signatures_t<set_value_t, Sigs, decayed_tuple_t, type_list>>, manual_variant>;

// The first exception any invocation of the function throws.
struct pool_bulk_failure {
    std::atomic<bool> failed{false};
    std::exception_ptr error;
};

template <bool Chunked, class Fn, class Shape, class Stored>
inline constexpr bool pool_bulk_may_throw_v = false;

template <bool Chunked, class Fn, class Shape, class Indices, class... Vs>
inline constexpr bool pool_bulk_may_throw_v<Chunked, Fn, Shape, tuple_impl<Indices, Vs...>> =
    LEXEC_HAS_EXCEPTIONS and not bulk_call<Chunked, Fn, Shape, Vs...>::nothrow;

template <bool Chunked, class Fn, class Shape, class Values>
inline constexpr bool pool_bulk_can_fail_v = false;

template <bool Chunked, class Fn, class Shape, class... Stored>
inline constexpr bool pool_bulk_can_fail_v<Chunked, Fn, Shape, manual_variant<Stored...>> =
    (pool_bulk_may_throw_v<Chunked, Fn, Shape, Stored> or ...);

template <bool Chunked, class CvChild, class Shape, class Fn, class Rcvr>
struct pool_bulk_operation;

template <bool Chunked, class CvChild, class Shape, class Fn, class Rcvr>
struct pool_bulk_receiver {
    using receiver_concept = receiver_t;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        op->start_job(static_cast<Vs &&>(vs)...);
    }

    template <class E>
    void set_error(E &&e) && noexcept {
        lexec::set_error(static_cast<Rcvr &&>(op->rcvr), static_cast<E &&>(e));
    }

    void set_stopped() && noexcept { lexec::set_stopped(static_cast<Rcvr &&>(op->rcvr)); }

    fwd_env_t<env_of_t<Rcvr>> get_env() const noexcept { return make_fwd_env(lexec::get_env(op->rcvr)); }

    pool_bulk_operation<Chunked, CvChild, Shape, Fn, Rcvr> *op;
};

template <bool Chunked, class CvChild, class Shape, class Fn, class Rcvr>
struct pool_bulk_operation : pool_bulk_job {
    using operation_state_concept = operation_state_t;
    using child_receiver = pool_bulk_receiver<Chunked, CvChild, Shape, Fn, Rcvr>;
    using values_type = pool_bulk_values_t<completion_signatures_of_t<CvChild, fwd_env_t<env_of_t<Rcvr>>>>;

    template <class Stored>
    static constexpr bool may_throw = pool_bulk_may_throw_v<Chunked, Fn, Shape, Stored>;

    template <class Child, class F>
    pool_bulk_operation(thread_pool_impl *pool_, Child &&child, Shape const shape_, F &&fn_, Rcvr &&rcvr_) noexcept(
        std::is_nothrow_constructible_v<Fn, F> and std::is_nothrow_move_constructible_v<Rcvr> and
        noexcept(lexec::connect(std::declval<Child>(), std::declval<child_receiver>())))
        : pool(pool_), shape(shape_), fn(static_cast<F &&>(fn_)), rcvr(static_cast<Rcvr &&>(rcvr_)),
          child_op(lexec::connect(static_cast<Child &&>(child), child_receiver{this})) {}

    pool_bulk_operation(pool_bulk_operation &&) = delete;

    void start() & noexcept { lexec::start(child_op); }

    template <class... Vs>
    void start_job(Vs &&...vs) noexcept {
        using stored = decayed_tuple_t<Vs...>;
        auto const store = [&] { values.template emplace_with<stored>([&] { return stored{{static_cast<Vs &&>(vs)}...}; }); };
        if constexpr (is_nothrow_decay_copyable_t<Vs...>::value or not LEXEC_HAS_EXCEPTIONS) {
            store();
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                store();
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(rcvr), std::current_exception());
                return;
            }
#endif
        }
        if (not(Shape{} < shape)) {
            send_values<stored>();
            return;
        }
        this->size = static_cast<std::size_t>(shape);
        this->run_chunk = &run_chunk_of<stored>;
        this->complete = &complete_of<stored>;
        detail::run_bulk(*pool, *this);
    }

    template <class Stored>
    void invoke(std::size_t const begin, std::size_t const end) {
        values.template get<Stored>().apply([&](auto &...vs) {
            if constexpr (Chunked) {
                fn(static_cast<Shape>(begin), static_cast<Shape>(end), vs...);
            } else {
                for (auto i = begin; i != end; ++i) {
                    fn(static_cast<Shape>(i), vs...);
                }
            }
        });
    }

    // After a failure the remaining chunks are claimed but not run, as the standard allows.
    template <class Stored>
    static void run_chunk_of(pool_bulk_job *const job, std::size_t const begin, std::size_t const end) noexcept {
        auto &self = *static_cast<pool_bulk_operation *>(job);
        if constexpr (may_throw<Stored>) {
#if LEXEC_HAS_EXCEPTIONS
            if (self.failure.failed.load(std::memory_order_relaxed)) {
                return;
            }
            try {
                self.template invoke<Stored>(begin, end);
            } catch (...) {
                if (not self.failure.failed.exchange(true, std::memory_order_relaxed)) {
                    self.failure.error = std::current_exception();
                }
            }
#endif
        } else {
            self.template invoke<Stored>(begin, end);
        }
    }

    template <class Stored>
    static void complete_of(pool_bulk_job *const job) noexcept {
        auto &self = *static_cast<pool_bulk_operation *>(job);
        if constexpr (may_throw<Stored>) {
            if (self.failure.failed.load(std::memory_order_relaxed)) {
                lexec::set_error(static_cast<Rcvr &&>(self.rcvr), static_cast<std::exception_ptr &&>(self.failure.error));
                return;
            }
        }
        self.template send_values<Stored>();
    }

    template <class Stored>
    void send_values() noexcept {
        values.template get<Stored>().apply([this](auto &...vs) noexcept {
            lexec::set_value(static_cast<Rcvr &&>(rcvr), static_cast<std::remove_reference_t<decltype(vs)> &&>(vs)...);
        });
    }

    static constexpr bool can_fail = pool_bulk_can_fail_v<Chunked, Fn, Shape, values_type>;

    thread_pool_impl *pool;
    Shape shape;
    Fn fn;
    Rcvr rcvr;
    values_type values;
    LEXEC_NO_UNIQUE_ADDRESS std::conditional_t<can_fail, pool_bulk_failure, no_data> failure;
    connect_result_t<CvChild, child_receiver> child_op;
};

template <bool Chunked, class Child, class Shape, class Fn>
struct pool_bulk_sender {
    using sender_concept = sender_t;

    template <class Self, class... Env>
    static auto get_completion_signatures()
        -> pool_bulk_completions_t<Chunked, member_like_t<Self, Child>, Shape, Fn, Env...>;

    template <class Rcvr>
    auto connect(Rcvr rcvr) && -> pool_bulk_operation<Chunked, Child, Shape, Fn, Rcvr> {
        return {pool, static_cast<Child &&>(child), shape, static_cast<Fn &&>(fn), static_cast<Rcvr &&>(rcvr)};
    }

    template <class Rcvr>
    auto connect(Rcvr rcvr) const & -> pool_bulk_operation<Chunked, Child const &, Shape, Fn, Rcvr> {
        return {pool, child, shape, fn, static_cast<Rcvr &&>(rcvr)};
    }

    // The last thread to finish a chunk completes the operation, and that is a worker
    // unless the pool never shared the work.
    pool_attrs<set_value_t> get_env() const noexcept { return {pool}; }

    thread_pool_impl *pool;
    Child child;
    Shape shape;
    Fn fn;
};

template <class Sndr, class Env, std::enable_if_t<is_pool_bulk_v<Sndr, Env>, int>>
auto pool_domain::transform_sender(set_value_t, Sndr &&sndr, Env const &env) const {
    auto const sch = get_completion_scheduler<set_value_t>(lexec::get_env(detail::get<0>(sndr.children)), env);
    return static_cast<Sndr &&>(sndr).apply([pool = sch.pool](auto tag, auto &&data, auto &&child) {
        using data_type = remove_cvref_t<decltype(data)>;
        using bulk_sender = pool_bulk_sender<std::is_same_v<decltype(tag), bulk_chunked_t>, remove_cvref_t<decltype(child)>,
                                             typename data_type::shape_type, typename data_type::fn_type>;
        return bulk_sender{pool, static_cast<decltype(child) &&>(child), data.shape,
                           static_cast<decltype(data) &&>(data).fn};
    });
}

} // namespace detail

// A fixed set of worker threads, each with a work-stealing queue. Scheduling never
// allocates: the operation is itself the queued task. Its domain runs bulk_chunked and
// bulk_unchunked under a parallel policy on all workers, and bulk through them; that
// never allocates either. Destruction waits for the threads to drain their queues and
// exit, so nothing may be scheduled on the pool by then.
struct static_thread_pool {
    explicit static_thread_pool(std::uint32_t thread_count = default_thread_count());
    static_thread_pool(static_thread_pool &&) = delete;
    ~static_thread_pool();

    detail::pool_scheduler get_scheduler() noexcept { return detail::pool_scheduler{impl.get()}; }
    std::uint32_t available_parallelism() const noexcept;

    static std::uint32_t default_thread_count() noexcept;

private:
    std::unique_ptr<detail::thread_pool_impl> impl;
};

} // namespace lexec
