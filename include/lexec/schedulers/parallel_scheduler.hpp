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
#include <lexec/stop_token.hpp>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>

namespace lexec {

// std::span is C++20. The backend interface takes this in every language mode, so that
// its virtual functions have one signature however lexec::runtime and users are built.
template <class T>
struct span {
    constexpr span() noexcept = default;
    constexpr span(T *const data_, std::size_t const size_) noexcept : ptr(data_), count(size_) {}

    constexpr T *data() const noexcept { return ptr; }
    constexpr std::size_t size() const noexcept { return count; }
    constexpr bool empty() const noexcept { return count == 0; }
    constexpr T *begin() const noexcept { return ptr; }
    constexpr T *end() const noexcept { return ptr + count; }
    constexpr T &operator[](std::size_t const i) const noexcept { return ptr[i]; }

private:
    T *ptr = nullptr;
    std::size_t count = 0;
};

namespace parallel_scheduler_replacement {

// The receiver of an operation launched on a backend.
struct receiver_proxy {
    virtual void set_value() noexcept = 0;
    virtual void set_error(std::exception_ptr) noexcept = 0;
    virtual void set_stopped() noexcept = 0;

    // Supports get_stop_token for inplace_stop_token, when that is the receiver's token.
    template <class P, class Query>
    std::optional<P> try_query(Query) const noexcept {
        if constexpr (std::is_same_v<Query, get_stop_token_t> and std::is_same_v<P, inplace_stop_token>) {
            auto token = inplace_stop_token{};
            if (query_stop_token(token)) {
                return token;
            }
        }
        return std::nullopt;
    }

protected:
    ~receiver_proxy() = default;

    virtual bool query_stop_token(inplace_stop_token &token) const noexcept = 0;
};

// The receiver of a bulk operation, which the backend also asks to run iterations.
struct bulk_item_receiver_proxy : receiver_proxy {
    virtual void execute(std::size_t begin, std::size_t end) noexcept = 0;

protected:
    ~bulk_item_receiver_proxy() = default;
};

// What parallel_scheduler runs work on. The storage handed to each call belongs to the
// operation and stays valid until the operation's receiver is completed.
struct parallel_scheduler_backend {
    virtual ~parallel_scheduler_backend() = default;
    virtual void schedule(receiver_proxy &, span<std::byte>) noexcept = 0;
    virtual void schedule_bulk_chunked(std::size_t, bulk_item_receiver_proxy &, span<std::byte>) noexcept = 0;
    virtual void schedule_bulk_unchunked(std::size_t, bulk_item_receiver_proxy &, span<std::byte>) noexcept = 0;
};

// Replaceable: a program's own definition takes the place of lexec::runtime's, which
// runs work on a static_thread_pool with one worker per hardware thread.
std::shared_ptr<parallel_scheduler_backend> query_parallel_scheduler_backend();

} // namespace parallel_scheduler_replacement

class parallel_scheduler;

namespace detail {

using parallel_backend_ptr = std::shared_ptr<parallel_scheduler_replacement::parallel_scheduler_backend>;

// Enough for lexec::runtime's backend, which therefore never allocates for an operation.
inline constexpr std::size_t parallel_backend_storage_size = 128;

struct parallel_backend_storage {
    span<std::byte> get() noexcept { return {bytes, sizeof(bytes)}; }

    alignas(std::max_align_t) std::byte bytes[parallel_backend_storage_size];
};

struct parallel_scheduler_access {
    static parallel_scheduler make(parallel_backend_ptr backend) noexcept;
    static parallel_backend_ptr const &backend(parallel_scheduler const &sch) noexcept;
};

template <class Env>
bool query_inplace_stop_token(Env const &env, inplace_stop_token &token) noexcept {
    if constexpr (std::is_same_v<stop_token_of_t<Env>, inplace_stop_token>) {
        token = lexec::get_stop_token(env);
        return true;
    } else {
        return false;
    }
}

// A backend may report an error; without exceptions there is no error channel for it.
template <class Rcvr>
void set_backend_error(Rcvr &rcvr, [[maybe_unused]] std::exception_ptr error) noexcept {
#if LEXEC_HAS_EXCEPTIONS
    lexec::set_error(static_cast<Rcvr &&>(rcvr), static_cast<std::exception_ptr &&>(error));
#else
    static_cast<void>(rcvr);
    std::terminate();
#endif
}

using backend_error_completion = eptr_completion_if_t<true>;

template <class Rcvr>
struct parallel_operation final : parallel_scheduler_replacement::receiver_proxy {
    using operation_state_concept = operation_state_t;

    parallel_operation(parallel_backend_ptr backend_, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : backend(static_cast<parallel_backend_ptr &&>(backend_)), rcvr(static_cast<Rcvr &&>(rcvr_)) {}

    parallel_operation(parallel_operation &&) = delete;

    void start() & noexcept { backend->schedule(*this, storage.get()); }

    void set_value() noexcept override { lexec::set_value(static_cast<Rcvr &&>(rcvr)); }
    void set_error(std::exception_ptr error) noexcept override { set_backend_error(rcvr, static_cast<std::exception_ptr &&>(error)); }
    void set_stopped() noexcept override { lexec::set_stopped(static_cast<Rcvr &&>(rcvr)); }

private:
    bool query_stop_token(inplace_stop_token &token) const noexcept override {
        return query_inplace_stop_token(lexec::get_env(rcvr), token);
    }

    // The operation keeps the backend alive until it completes.
    parallel_backend_ptr backend;
    Rcvr rcvr;
    parallel_backend_storage storage;
};

template <class... Tags>
struct parallel_attrs {
    template <class Tag, class... Env, std::enable_if_t<is_one_of_v<Tag, Tags...>, int> = 0>
    parallel_scheduler query(get_completion_scheduler_t<Tag>, Env const &...) const noexcept;

    parallel_backend_ptr const *backend;
};

struct parallel_sender {
    using sender_concept = sender_t;
    using completion_signatures =
        concat_t<lexec::completion_signatures<set_value_t(), set_stopped_t()>, backend_error_completion>;

    template <class Rcvr>
    parallel_operation<Rcvr> connect(Rcvr rcvr) && noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {static_cast<parallel_backend_ptr &&>(backend), static_cast<Rcvr &&>(rcvr)};
    }

    template <class Rcvr>
    parallel_operation<Rcvr> connect(Rcvr rcvr) const & noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {backend, static_cast<Rcvr &&>(rcvr)};
    }

    parallel_attrs<set_value_t, set_stopped_t> get_env() const noexcept { return {&backend}; }

    parallel_backend_ptr backend;
};

template <bool Chunked, bool Parallel, class Child, class Shape, class Fn>
struct parallel_bulk_sender;

template <class Sndr, class Env>
inline constexpr bool is_parallel_scheduler_bulk_v =
    std::is_same_v<detected_or_t<void, bulk_child_scheduler_t, Sndr, Env>, parallel_scheduler>;

// Runs bulk_chunked and bulk_unchunked on the backend, under any policy: one that does
// not allow parallel execution runs as a single iteration.
struct parallel_scheduler_domain {
    template <class Sndr, class Env,
              std::enable_if_t<is_one_of_v<detected_or_t<void, tag_of_t, Sndr>, bulk_chunked_t, bulk_unchunked_t> and
                                   is_parallel_scheduler_bulk_v<Sndr, Env>,
                               int> = 0>
    auto transform_sender(set_value_t, Sndr &&sndr, Env const &env) const;
};

} // namespace detail

// The scheduler of the system's parallel execution resource, whose backend a program may
// replace; see parallel_scheduler_replacement.
class parallel_scheduler {
public:
    using scheduler_concept = scheduler_t;

    detail::parallel_sender schedule() const noexcept { return detail::parallel_sender{backend}; }

    template <class... Env>
    constexpr detail::parallel_scheduler_domain query(get_completion_domain_t<set_value_t>, Env const &...) const noexcept {
        return {};
    }

    static constexpr forward_progress_guarantee query(get_forward_progress_guarantee_t) noexcept {
        return forward_progress_guarantee::parallel;
    }

    friend bool operator==(parallel_scheduler const &lhs, parallel_scheduler const &rhs) noexcept {
        return lhs.backend == rhs.backend;
    }
    friend bool operator!=(parallel_scheduler const &lhs, parallel_scheduler const &rhs) noexcept {
        return lhs.backend != rhs.backend;
    }

private:
    friend struct detail::parallel_scheduler_access;

    explicit parallel_scheduler(detail::parallel_backend_ptr backend_) noexcept
        : backend(static_cast<detail::parallel_backend_ptr &&>(backend_)) {}

    detail::parallel_backend_ptr backend;
};

// The parallel_scheduler of the backend query_parallel_scheduler_backend returns;
// terminates if it returns none.
inline parallel_scheduler get_parallel_scheduler() {
    auto backend = parallel_scheduler_replacement::query_parallel_scheduler_backend();
    if (backend == nullptr) {
        std::terminate();
    }
    return detail::parallel_scheduler_access::make(static_cast<detail::parallel_backend_ptr &&>(backend));
}

namespace detail {

inline parallel_scheduler parallel_scheduler_access::make(parallel_backend_ptr backend) noexcept {
    return parallel_scheduler{static_cast<parallel_backend_ptr &&>(backend)};
}

inline parallel_backend_ptr const &parallel_scheduler_access::backend(parallel_scheduler const &sch) noexcept {
    return sch.backend;
}

template <class... Tags>
template <class Tag, class... Env, std::enable_if_t<is_one_of_v<Tag, Tags...>, int>>
parallel_scheduler parallel_attrs<Tags...>::query(get_completion_scheduler_t<Tag>, Env const &...) const noexcept {
    return parallel_scheduler_access::make(*backend);
}

// The values pass through decayed, since the operation stores them; the backend may
// also report an error or stopped.
template <bool Chunked, class CvChild, class Shape, class Fn, class... Env>
using parallel_bulk_completions_t = transform_completion_signatures<
    completion_signatures_of_t<CvChild, fwd_env_t<Env>...>,
    concat_t<completion_signatures<set_stopped_t()>, backend_error_completion>,
    stored_bulk_transforms<Chunked, Fn, Shape>::template on_value>;

template <bool Chunked, bool Parallel, class CvChild, class Shape, class Fn, class Rcvr>
struct parallel_bulk_operation;

template <bool Chunked, bool Parallel, class CvChild, class Shape, class Fn, class Rcvr>
struct parallel_bulk_receiver {
    using receiver_concept = receiver_t;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        op->start_backend(static_cast<Vs &&>(vs)...);
    }

    template <class E>
    void set_error(E &&e) && noexcept {
        lexec::set_error(static_cast<Rcvr &&>(op->rcvr), static_cast<E &&>(e));
    }

    void set_stopped() && noexcept { lexec::set_stopped(static_cast<Rcvr &&>(op->rcvr)); }

    fwd_env_t<env_of_t<Rcvr>> get_env() const noexcept { return make_fwd_env(lexec::get_env(op->rcvr)); }

    parallel_bulk_operation<Chunked, Parallel, CvChild, Shape, Fn, Rcvr> *op;
};

template <bool Chunked, bool Parallel, class CvChild, class Shape, class Fn, class Rcvr>
struct parallel_bulk_operation final : parallel_scheduler_replacement::bulk_item_receiver_proxy {
    using operation_state_concept = operation_state_t;
    using child_receiver = parallel_bulk_receiver<Chunked, Parallel, CvChild, Shape, Fn, Rcvr>;
    using values_type = stored_bulk_values_t<completion_signatures_of_t<CvChild, fwd_env_t<env_of_t<Rcvr>>>>;

    template <class Stored>
    static constexpr bool may_throw = stored_bulk_may_throw_v<Chunked, Fn, Shape, Stored>;

    template <class Child, class F>
    parallel_bulk_operation(parallel_backend_ptr backend_, Child &&child, Shape const shape_, F &&fn_, Rcvr &&rcvr_) noexcept(
        std::is_nothrow_constructible_v<Fn, F> and std::is_nothrow_move_constructible_v<Rcvr> and
        noexcept(lexec::connect(std::declval<Child>(), std::declval<child_receiver>())))
        : backend(static_cast<parallel_backend_ptr &&>(backend_)), shape(shape_), fn(static_cast<F &&>(fn_)),
          rcvr(static_cast<Rcvr &&>(rcvr_)), child_op(lexec::connect(static_cast<Child &&>(child), child_receiver{this})) {}

    parallel_bulk_operation(parallel_bulk_operation &&) = delete;

    void start() & noexcept { lexec::start(child_op); }

    template <class... Vs>
    void start_backend(Vs &&...vs) noexcept {
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
        run = &run_of<stored>;
        send = &send_of<stored>;
        auto const count = Parallel ? (Shape{} < shape ? static_cast<std::size_t>(shape) : 0) : std::size_t{1};
        if constexpr (Chunked) {
            backend->schedule_bulk_chunked(count, *this, storage.get());
        } else {
            backend->schedule_bulk_unchunked(count, *this, storage.get());
        }
    }

    void execute(std::size_t const begin, std::size_t const end) noexcept override { run(*this, begin, end); }

    void set_value() noexcept override { send(*this); }
    void set_error(std::exception_ptr error) noexcept override { set_backend_error(rcvr, static_cast<std::exception_ptr &&>(error)); }
    void set_stopped() noexcept override { lexec::set_stopped(static_cast<Rcvr &&>(rcvr)); }

private:
    friend child_receiver;

    bool query_stop_token(inplace_stop_token &token) const noexcept override {
        return query_inplace_stop_token(lexec::get_env(rcvr), token);
    }

    // Without a parallel policy the backend runs one iteration, which runs them all.
    template <class Stored>
    void invoke(std::size_t const begin, std::size_t const end) {
        values.template get<Stored>().apply([&](auto &...vs) {
            if constexpr (not Parallel) {
                static_cast<void>(begin);
                static_cast<void>(end);
                if constexpr (Chunked) {
                    if (Shape{} < shape) {
                        fn(Shape{}, shape, vs...);
                    }
                } else {
                    for (auto i = Shape{}; i < shape; ++i) {
                        fn(Shape(i), vs...);
                    }
                }
            } else if constexpr (Chunked) {
                fn(static_cast<Shape>(begin), static_cast<Shape>(end), vs...);
            } else {
                fn(static_cast<Shape>(begin), vs...);
            }
        });
    }

    template <class Stored>
    static void run_of(parallel_bulk_operation &self, std::size_t const begin, std::size_t const end) noexcept {
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
    static void send_of(parallel_bulk_operation &self) noexcept {
        if constexpr (may_throw<Stored>) {
            if (self.failure.failed.load(std::memory_order_relaxed)) {
                lexec::set_error(static_cast<Rcvr &&>(self.rcvr), static_cast<std::exception_ptr &&>(self.failure.error));
                return;
            }
        }
        self.values.template get<Stored>().apply([&self](auto &...vs) noexcept {
            lexec::set_value(static_cast<Rcvr &&>(self.rcvr), static_cast<std::remove_reference_t<decltype(vs)> &&>(vs)...);
        });
    }

    static constexpr bool can_fail = stored_bulk_can_fail_v<Chunked, Fn, Shape, values_type>;

    parallel_backend_ptr backend;
    Shape shape;
    Fn fn;
    Rcvr rcvr;
    values_type values;
    void (*run)(parallel_bulk_operation &, std::size_t, std::size_t) noexcept = nullptr;
    void (*send)(parallel_bulk_operation &) noexcept = nullptr;
    LEXEC_NO_UNIQUE_ADDRESS std::conditional_t<can_fail, bulk_failure, no_data> failure;
    parallel_backend_storage storage;
    connect_result_t<CvChild, child_receiver> child_op;
};

template <bool Chunked, bool Parallel, class Child, class Shape, class Fn>
struct parallel_bulk_sender {
    using sender_concept = sender_t;

    template <class Self, class... Env>
    static auto get_completion_signatures()
        -> parallel_bulk_completions_t<Chunked, member_like_t<Self, Child>, Shape, Fn, Env...>;

    template <class Rcvr>
    auto connect(Rcvr rcvr) && -> parallel_bulk_operation<Chunked, Parallel, Child, Shape, Fn, Rcvr> {
        return {static_cast<parallel_backend_ptr &&>(backend), static_cast<Child &&>(child), shape, static_cast<Fn &&>(fn),
                static_cast<Rcvr &&>(rcvr)};
    }

    template <class Rcvr>
    auto connect(Rcvr rcvr) const & -> parallel_bulk_operation<Chunked, Parallel, Child const &, Shape, Fn, Rcvr> {
        return {backend, child, shape, fn, static_cast<Rcvr &&>(rcvr)};
    }

    // The backend completes on its own execution agents.
    parallel_attrs<set_value_t> get_env() const noexcept { return {&backend}; }

    parallel_backend_ptr backend;
    Child child;
    Shape shape;
    Fn fn;
};

template <class Sndr, class Env,
          std::enable_if_t<is_one_of_v<detected_or_t<void, tag_of_t, Sndr>, bulk_chunked_t, bulk_unchunked_t> and
                               is_parallel_scheduler_bulk_v<Sndr, Env>,
                           int>>
auto parallel_scheduler_domain::transform_sender(set_value_t, Sndr &&sndr, Env const &env) const {
    auto sch = get_completion_scheduler<set_value_t>(lexec::get_env(detail::get<0>(sndr.children)), env);
    return static_cast<Sndr &&>(sndr).apply([&sch](auto tag, auto &&data, auto &&child) {
        using data_type = remove_cvref_t<decltype(data)>;
        using bulk_sender =
            parallel_bulk_sender<std::is_same_v<decltype(tag), bulk_chunked_t>,
                                 is_parallel_policy_v<typename data_type::policy_type>, remove_cvref_t<decltype(child)>,
                                 typename data_type::shape_type, typename data_type::fn_type>;
        return bulk_sender{parallel_scheduler_access::backend(sch), static_cast<decltype(child) &&>(child), data.shape,
                           static_cast<decltype(data) &&>(data).fn};
    });
}

} // namespace detail

} // namespace lexec
