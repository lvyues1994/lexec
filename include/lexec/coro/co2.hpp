#pragma once

// The bridge between lexec and co2, the C++14 stackless coroutine library
// (https://github.com/lvyues1994/coro). It needs co2's headers, and exceptions, which co2
// relies on. In a co2 coroutine:
//
//   CO2_AWAIT_SET(value, lexec::just(1));                 // any lexec sender
//   CO2_AWAIT(lexec::schedule(pool.get_scheduler()));     // resumes on the pool
//   CO2_AWAIT(lexec::coro::as_awaitable(other_sender));   // a sender from elsewhere
//
// and in sender code:
//
//   lexec::sync_wait(lexec::coro::as_sender(make_task()));
//   lexec::schedule(lexec::coro::scheduler{co2_pool});
//
// co2 has no stopped channel: a stopped completion is thrown into the coroutine as
// lexec::coro::stopped_error, and a Task that ends with it completes with set_stopped.

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/scheduler.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/stop_token.hpp>

#include <co2/coroutine_handle.hpp>
#include <co2/scheduler.hpp>
#include <co2/stop_token.hpp>
#include <co2/task.hpp>

#include <cstddef>
#include <exception>
#include <new>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#if not LEXEC_HAS_EXCEPTIONS
#error "lexec's co2 bridge needs exceptions, as co2 does"
#endif

namespace lexec::coro {

// Thrown into a co2 coroutine whose awaited sender completed with set_stopped.
struct stopped_error : std::exception {
    char const *what() const noexcept override { return "lexec::coro: the awaited sender completed with stopped"; }
};

template <class Fn>
struct stop_callback;

// co2's stop_token as a lexec stoppable token.
struct stop_token {
    template <class Fn>
    using callback_type = coro::stop_callback<Fn>;

    bool stop_requested() const noexcept { return token.stop_requested(); }
    bool stop_possible() const noexcept { return token.stop_possible(); }

    friend bool operator==(stop_token const &lhs, stop_token const &rhs) noexcept { return lhs.token == rhs.token; }
    friend bool operator!=(stop_token const &lhs, stop_token const &rhs) noexcept { return lhs.token != rhs.token; }

    ::co2::stop_token token;
};

template <class Fn>
struct stop_callback : private ::co2::stop_callback<Fn> {
    using callback_type = Fn;

    template <class Init, std::enable_if_t<std::is_constructible_v<Fn, Init>, int> = 0>
    explicit stop_callback(stop_token const &token, Init &&init) noexcept(std::is_nothrow_constructible_v<Fn, Init>)
        : ::co2::stop_callback<Fn>(token.token, static_cast<Init &&>(init)) {}
};

namespace detail {

using lexec::detail::remove_cvref_t;
using lexec::detail::type_list;

// A handwritten co2 frame: the target of a Task's final transfer, or the handle a co2
// scheduler resumes. Resuming it calls fn(owner) and hands control back.
struct resumable_frame {
    template <class Owner, void (*Fn)(Owner &) noexcept>
    static resumable_frame make(Owner &owner) noexcept {
        return resumable_frame{{&::co2::detail::noopDestroy, &step<Owner, Fn>, false, false}, &owner};
    }

    ::co2::coroutine_handle<> handle() noexcept { return ::co2::coroutine_handle<>::from_address(&header); }

    template <class Owner, void (*Fn)(Owner &) noexcept>
    static ::co2::detail::FrameHeader *step(::co2::detail::FrameHeader *const header) {
        static_assert(std::is_standard_layout_v<resumable_frame>, "a co2 frame must be standard-layout");
        Fn(*static_cast<Owner *>(reinterpret_cast<resumable_frame *>(header)->owner));
        return nullptr;
    }

    ::co2::detail::FrameHeader header;
    void *owner;
};

template <class Promise>
auto promise_stop_token(Promise &promise, int) noexcept -> decltype(::co2::stop_token(promise.get_stop_token())) {
    return promise.get_stop_token();
}

template <class Promise>
::co2::stop_token promise_stop_token(Promise &, long) noexcept {
    return {};
}

// The environment an awaited sender sees: the awaiting coroutine's stop token.
struct awaiter_env {
    stop_token query(get_stop_token_t) const noexcept { return *token; }

    stop_token const *token;
};

template <class ValueLists>
struct awaited_value {
    static_assert(lexec::detail::dependent_false<ValueLists>,
                  "lexec::coro: an awaited sender must have at most one value completion signature");
};

template <>
struct awaited_value<type_list<>> {
    using type = void;
};

template <>
struct awaited_value<type_list<type_list<>>> {
    using type = void;
};

template <class V>
struct awaited_value<type_list<type_list<V>>> {
    using type = std::decay_t<V>;
};

template <class V, class W, class... Vs>
struct awaited_value<type_list<type_list<V, W, Vs...>>> {
    using type = std::tuple<std::decay_t<V>, std::decay_t<W>, std::decay_t<Vs>...>;
};

// What co_await on the sender produces: its one value, void, or a tuple of its values.
template <class Sndr>
using awaited_value_t = typename awaited_value<lexec::detail::gather_signatures_t<
    set_value_t, completion_signatures_of_t<Sndr, awaiter_env>, type_list, type_list>>::type;

template <class E>
std::exception_ptr as_exception_ptr(E &&error) noexcept {
    if constexpr (std::is_same_v<remove_cvref_t<E>, std::exception_ptr>) {
        return static_cast<E &&>(error);
    } else if constexpr (std::is_same_v<remove_cvref_t<E>, std::error_code>) {
        return std::make_exception_ptr(std::system_error(error));
    } else {
        return std::make_exception_ptr(static_cast<E &&>(error));
    }
}

template <class Sndr>
struct sender_awaiter;

// The awaiter whose operation this thread is starting, and where to record that the
// operation completed during start.
struct starting_awaiter {
    void const *awaiter;
    bool *completed;
};

inline thread_local starting_awaiter current_start{nullptr, nullptr};

template <class Sndr>
struct awaiter_receiver {
    using receiver_concept = receiver_t;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        awaiter->complete_with_value(static_cast<Vs &&>(vs)...);
    }

    template <class E>
    void set_error(E &&error) && noexcept {
        awaiter->complete_with_error(as_exception_ptr(static_cast<E &&>(error)));
    }

    void set_stopped() && noexcept { awaiter->complete(sender_awaiter<Sndr>::outcome_kind::stopped); }

    awaiter_env get_env() const noexcept { return {&awaiter->token}; }

    sender_awaiter<Sndr> *awaiter;
};

// Awaits a sender in a co2 coroutine. co2 moves the awaiter into the frame, so until
// await_suspend it holds only the sender; there, at its final address, it connects the
// sender in the same storage, which keeps it within co2's inline awaiter slot for
// operations such as schedule(pool).
//
// A sender that completes inside start, on the starting thread, does not suspend the
// coroutine, so loops over synchronous senders stay flat. Any other completion resumes
// the coroutine where it happens, even before start returns: co2 counts the coroutine as
// suspended before await_suspend, so a completion on another thread resumes it at once,
// and await_suspend then touches nothing but its own stack.
template <class Sndr>
struct sender_awaiter {
    using value_type = awaited_value_t<Sndr>;
    using receiver = awaiter_receiver<Sndr>;
    using operation = connect_result_t<Sndr, receiver>;

    enum class outcome_kind : unsigned char { none, value, error, stopped };

    template <class S, std::enable_if_t<std::is_constructible_v<Sndr, S>, int> = 0>
    explicit sender_awaiter(S &&sndr) noexcept(std::is_nothrow_constructible_v<Sndr, S>) {
        ::new (static_cast<void *>(storage)) Sndr(static_cast<S &&>(sndr));
        holds = holding::sender;
    }

    // co2 moves an awaiter only before its first await_suspend.
    sender_awaiter(sender_awaiter &&other) noexcept(std::is_nothrow_move_constructible_v<Sndr>) {
        ::new (static_cast<void *>(storage)) Sndr(static_cast<Sndr &&>(other.sender()));
        holds = holding::sender;
    }

    sender_awaiter &operator=(sender_awaiter &&) = delete;

    ~sender_awaiter() {
        if (holds == holding::sender) {
            sender().~Sndr();
        } else if (holds == holding::operation) {
            op().~operation();
        }
        if (outcome == outcome_kind::value) {
            if constexpr (not std::is_void_v<value_type>) {
                value().~value_type();
            }
        } else if (outcome == outcome_kind::error) {
            error().~exception_ptr();
        }
    }

    static constexpr bool await_ready() noexcept { return false; }

    template <class Promise>
    bool await_suspend(::co2::coroutine_handle<Promise> const awaiting) {
        waiter = awaiting;
        token = stop_token{promise_stop_token(awaiting.promise(), 0)};
        auto sndr = static_cast<Sndr &&>(sender());
        sender().~Sndr();
        holds = holding::nothing;
        ::new (static_cast<void *>(storage)) operation(lexec::connect(static_cast<Sndr &&>(sndr), receiver{this}));
        holds = holding::operation;
        auto completed_inline = false;
        auto const outer = current_start;
        current_start = starting_awaiter{this, &completed_inline};
        lexec::start(op());
        current_start = outer;
        return not completed_inline;
    }

    value_type await_resume() {
        switch (outcome) {
        case outcome_kind::value:
            if constexpr (std::is_void_v<value_type>) {
                return;
            } else {
                return static_cast<value_type &&>(value());
            }
        case outcome_kind::error:
            std::rethrow_exception(error());
        case outcome_kind::stopped:
        case outcome_kind::none:
            break;
        }
        throw stopped_error{};
    }

    template <class... Vs>
    void complete_with_value(Vs &&...vs) noexcept {
        try {
            if constexpr (not std::is_void_v<value_type>) {
                ::new (static_cast<void *>(result)) value_type(static_cast<Vs &&>(vs)...);
            }
        } catch (...) {
            complete_with_error(std::current_exception());
            return;
        }
        complete(outcome_kind::value);
    }

    void complete_with_error(std::exception_ptr e) noexcept {
        ::new (static_cast<void *>(result)) std::exception_ptr(static_cast<std::exception_ptr &&>(e));
        complete(outcome_kind::error);
    }

    // After resuming, nothing here is touched: the coroutine may destroy the awaiter.
    void complete(outcome_kind const kind) noexcept {
        outcome = kind;
        if (current_start.awaiter == this) {
            *current_start.completed = true;
            return;
        }
        waiter.resume();
    }

private:
    friend receiver;

    enum class holding : unsigned char { nothing, sender, operation };

    Sndr &sender() noexcept { return *std::launder(reinterpret_cast<Sndr *>(storage)); }
    operation &op() noexcept { return *std::launder(reinterpret_cast<operation *>(storage)); }
    std::exception_ptr &error() noexcept { return *std::launder(reinterpret_cast<std::exception_ptr *>(result)); }

    template <class T = value_type>
    T &value() noexcept {
        return *std::launder(reinterpret_cast<T *>(result));
    }

    template <class T>
    static constexpr std::size_t size_of() noexcept {
        if constexpr (std::is_void_v<T>) {
            return 1;
        } else {
            return sizeof(T);
        }
    }

    template <class T>
    static constexpr std::size_t align_of() noexcept {
        if constexpr (std::is_void_v<T>) {
            return 1;
        } else {
            return alignof(T);
        }
    }

    static constexpr std::size_t storage_size = sizeof(Sndr) > sizeof(operation) ? sizeof(Sndr) : sizeof(operation);
    static constexpr std::size_t storage_align = alignof(Sndr) > alignof(operation) ? alignof(Sndr) : alignof(operation);
    static constexpr std::size_t result_size =
        size_of<value_type>() > sizeof(std::exception_ptr) ? size_of<value_type>() : sizeof(std::exception_ptr);
    static constexpr std::size_t result_align =
        align_of<value_type>() > alignof(std::exception_ptr) ? align_of<value_type>() : alignof(std::exception_ptr);

    alignas(storage_align) unsigned char storage[storage_size];
    ::co2::coroutine_handle<> waiter;
    stop_token token;
    alignas(result_align) unsigned char result[result_size];
    outcome_kind outcome = outcome_kind::none;
    holding holds = holding::nothing;
};

// A co2 Task run as an operation. The operation holds a handwritten co2 frame that the
// Task's final transfer resumes, so nothing is allocated besides the Task's own frame.
template <class T, class Rcvr>
struct task_operation {
    using operation_state_concept = operation_state_t;
    using receiver_token = stop_token_of_t<env_of_t<Rcvr>>;
    // A receiver whose token is not co2's gets its stop requests forwarded to a co2
    // stop_source, which allocates; one that cannot stop gets an empty co2 token.
    static constexpr bool forwards_stop =
        not std::is_same_v<receiver_token, stop_token> and not is_unstoppable_token_v<receiver_token>;

    struct forward_stop {
        void operator()() noexcept { source->request_stop(); }

        ::co2::stop_source *source;
    };

    using forward_callback = stop_callback_for_t<receiver_token, forward_stop>;

    task_operation(::co2::Task<T> &&task_, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : task(static_cast<::co2::Task<T> &&>(task_)), rcvr(static_cast<Rcvr &&>(rcvr_)),
          frame(resumable_frame::make<task_operation, &task_operation::finish>(*this)) {}

    task_operation(task_operation &&) = delete;

    ~task_operation() {
        if constexpr (forwards_stop) {
            if (callback_alive) {
                callback().~forward_callback();
            }
            if (bridged) {
                source().~stop_source();
            }
        }
    }

    void start() & noexcept { ::co2::detail::TaskAccess::start(task, frame.handle(), root_token()); }

private:
    ::co2::stop_token root_token() noexcept {
        auto const token = lexec::get_stop_token(lexec::get_env(rcvr));
        if constexpr (std::is_same_v<receiver_token, stop_token>) {
            return token.token;
        } else if constexpr (forwards_stop) {
            if (not token.stop_possible()) {
                return {};
            }
            ::new (static_cast<void *>(source_storage)) ::co2::stop_source();
            bridged = true;
            ::new (static_cast<void *>(callback_storage)) forward_callback(token, forward_stop{&source()});
            callback_alive = true;
            return source().get_token();
        } else {
            static_cast<void>(token);
            return {};
        }
    }

    // Runs when the Task transfers to the frame at its final suspend point.
    // The receiver's stop requests stop reaching the Task before the receiver completes.
    static void finish(task_operation &self) noexcept {
        if constexpr (forwards_stop) {
            if (self.callback_alive) {
                self.callback().~forward_callback();
                self.callback_alive = false;
            }
        }
        if (::co2::detail::TaskAccess::hasError(self.task)) {
            try {
                ::co2::detail::TaskAccess::rethrowError(self.task);
            } catch (stopped_error const &) {
                lexec::set_stopped(static_cast<Rcvr &&>(self.rcvr));
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(self.rcvr), std::current_exception());
            }
            return;
        }
        if constexpr (std::is_void_v<T>) {
            ::co2::detail::TaskAccess::takeResult(self.task);
            lexec::set_value(static_cast<Rcvr &&>(self.rcvr));
        } else {
            // Taking the value moves it, which may throw.
            try {
                self.send_value(::co2::detail::TaskAccess::takeResult(self.task));
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(self.rcvr), std::current_exception());
            }
        }
    }

    template <class V>
    void send_value(V &&v) noexcept {
        lexec::set_value(static_cast<Rcvr &&>(rcvr), static_cast<V &&>(v));
    }

    ::co2::stop_source &source() noexcept { return *std::launder(reinterpret_cast<::co2::stop_source *>(source_storage)); }
    forward_callback &callback() noexcept {
        return *std::launder(reinterpret_cast<forward_callback *>(callback_storage));
    }

    ::co2::Task<T> task;
    Rcvr rcvr;
    resumable_frame frame;
    alignas(::co2::stop_source) unsigned char source_storage[forwards_stop ? sizeof(::co2::stop_source) : 1];
    alignas(forward_callback) unsigned char callback_storage[forwards_stop ? sizeof(forward_callback) : 1];
    bool bridged = false;
    bool callback_alive = false;
};

template <class T>
struct task_value_completion {
    using type = completion_signatures<set_value_t(T)>;
};

template <>
struct task_value_completion<void> {
    using type = completion_signatures<set_value_t()>;
};

} // namespace detail

// A co2 Task as a sender: starting it starts the Task, which then runs wherever it
// schedules itself; it completes with the Task's value, its exception, or stopped when
// that exception is stopped_error. The receiver's stop requests reach the Task's token.
template <class T>
struct task_sender {
    using sender_concept = sender_t;
    using completion_signatures =
        lexec::detail::concat_t<typename detail::task_value_completion<T>::type,
                                lexec::completion_signatures<set_error_t(std::exception_ptr), set_stopped_t()>>;

    template <class Rcvr>
    detail::task_operation<T, Rcvr> connect(Rcvr rcvr) && noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {static_cast<::co2::Task<T> &&>(task), static_cast<Rcvr &&>(rcvr)};
    }

    ::co2::Task<T> task;
};

template <class T>
task_sender<T> as_sender(::co2::Task<T> task) noexcept {
    return task_sender<T>{static_cast<::co2::Task<T> &&>(task)};
}

// Awaits any sender in a co2 coroutine; lexec's own senders are awaitable directly.
template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
detail::sender_awaiter<std::decay_t<Sndr>> as_awaitable(Sndr &&sndr) noexcept(
    std::is_nothrow_constructible_v<std::decay_t<Sndr>, Sndr>) {
    return detail::sender_awaiter<std::decay_t<Sndr>>{static_cast<Sndr &&>(sndr)};
}

class scheduler;

namespace detail {

template <class Rcvr>
struct schedule_operation {
    using operation_state_concept = operation_state_t;

    schedule_operation(::co2::Scheduler *const target_, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : target(target_), rcvr(static_cast<Rcvr &&>(rcvr_)),
          frame(resumable_frame::make<schedule_operation, &schedule_operation::run>(*this)) {}

    schedule_operation(schedule_operation &&) = delete;

    void start() & noexcept { target->schedule(frame.handle()); }

private:
    static void run(schedule_operation &self) noexcept {
        if (lexec::get_stop_token(lexec::get_env(self.rcvr)).stop_requested()) {
            lexec::set_stopped(static_cast<Rcvr &&>(self.rcvr));
        } else {
            lexec::set_value(static_cast<Rcvr &&>(self.rcvr));
        }
    }

    ::co2::Scheduler *target;
    Rcvr rcvr;
    resumable_frame frame;
};

struct schedule_attrs {
    template <class Tag, class... Env, std::enable_if_t<lexec::detail::is_one_of_v<Tag, set_value_t, set_stopped_t>, int> = 0>
    coro::scheduler query(get_completion_scheduler_t<Tag>, Env const &...) const noexcept;

    ::co2::Scheduler *target;
};

struct schedule_sender {
    using sender_concept = sender_t;
    using completion_signatures = lexec::completion_signatures<set_value_t(), set_stopped_t()>;

    template <class Rcvr>
    schedule_operation<Rcvr> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {target, static_cast<Rcvr &&>(rcvr)};
    }

    schedule_attrs get_env() const noexcept { return {target}; }

    ::co2::Scheduler *target;
};

} // namespace detail

// A co2 Scheduler, such as co2::ThreadPool or co2::ManualExecutor, as a lexec scheduler.
// Scheduling queues a frame embedded in the operation, so it never allocates.
class scheduler {
public:
    using scheduler_concept = scheduler_t;

    explicit scheduler(::co2::Scheduler &target_) noexcept : target(&target_) {}

    detail::schedule_sender schedule() const noexcept { return {target}; }

    friend bool operator==(scheduler const lhs, scheduler const rhs) noexcept { return lhs.target == rhs.target; }
    friend bool operator!=(scheduler const lhs, scheduler const rhs) noexcept { return lhs.target != rhs.target; }

private:
    ::co2::Scheduler *target;
};

template <class Tag, class... Env, std::enable_if_t<lexec::detail::is_one_of_v<Tag, set_value_t, set_stopped_t>, int>>
coro::scheduler detail::schedule_attrs::query(get_completion_scheduler_t<Tag>, Env const &...) const noexcept {
    return coro::scheduler{*target};
}

} // namespace lexec::coro

namespace lexec::detail {

// Found by argument-dependent lookup from co2's co_await, which C++14 spells
// operator_co_await: every sender whose type lexec defines is awaitable in a co2 coroutine.
template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
coro::detail::sender_awaiter<std::decay_t<Sndr>> operator_co_await(Sndr &&sndr) noexcept(
    std::is_nothrow_constructible_v<std::decay_t<Sndr>, Sndr>) {
    return coro::detail::sender_awaiter<std::decay_t<Sndr>>{static_cast<Sndr &&>(sndr)};
}

} // namespace lexec::detail
