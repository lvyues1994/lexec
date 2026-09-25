#pragma once

#include <lexec/algorithms/just.hpp>
#include <lexec/algorithms/stop_when.hpp>
#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/scheduler.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/stop_token.hpp>

#include <atomic>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace lexec {

namespace detail {

template <class Assoc>
using try_associate_result_t = decltype(std::declval<Assoc const &>().try_associate());

template <class Assoc>
using bool_conversion_t = std::enable_if_t<noexcept(static_cast<bool>(std::declval<Assoc const &>()))>;

template <class Token>
using wrap_result_t = decltype(std::declval<Token const &>().wrap(just()));

template <class Token>
using token_association_t = remove_cvref_t<decltype(std::declval<Token &>().try_associate())>;

} // namespace detail

// scope_association: a movable, default-constructible handle that owns at most one
// association with an async scope, released when the handle is destroyed.
template <class Assoc>
inline constexpr bool is_scope_association_v =
    std::is_nothrow_move_constructible_v<Assoc> and std::is_nothrow_move_assignable_v<Assoc> and
    std::is_default_constructible_v<Assoc> and detail::is_detected_v<detail::bool_conversion_t, Assoc> and
    std::is_same_v<detail::detected_or_t<void, detail::try_associate_result_t, Assoc>, Assoc>;

// scope_token: a copyable handle to an async scope that makes associations and wraps
// senders to be associated.
template <class Token>
inline constexpr bool is_scope_token_v =
    std::is_copy_constructible_v<Token> and
    is_scope_association_v<detail::detected_or_t<void, detail::try_associate_result_t, Token>> and
    is_sender_v<detail::detected_or_t<void, detail::wrap_result_t, Token>>;

namespace detail {

// An association with a counting scope; releasing it may complete the scope's join.
template <class Scope>
class association {
public:
    association() noexcept = default;
    explicit association(Scope *const scope_) noexcept : scope(scope_) {}

    association(association &&other) noexcept : scope(std::exchange(other.scope, nullptr)) {}

    association &operator=(association &&other) noexcept {
        if (this != &other) {
            release();
            scope = std::exchange(other.scope, nullptr);
        }
        return *this;
    }

    ~association() { release(); }

    explicit operator bool() const noexcept { return scope != nullptr; }

    association try_associate() const noexcept { return scope != nullptr ? scope->try_associate() : association{}; }

private:
    void release() noexcept {
        if (scope != nullptr) {
            std::exchange(scope, nullptr)->disassociate();
        }
    }

    Scope *scope = nullptr;
};

// A join operation waiting for a scope's associations to end.
struct join_waiter {
    join_waiter *next = nullptr;
    void (*complete)(join_waiter *) noexcept = nullptr;
};

// The state machine both counting scopes share. The state and the count live in one
// word, so that associating and disassociating are single atomic updates; registering a
// join and completing the joins also hold a mutex, which orders a registration against
// the disassociation that ends the count.
class counting_scope_state {
public:
    static constexpr std::size_t max_associations = std::numeric_limits<std::size_t>::max() >> 3;

    counting_scope_state() noexcept = default;
    counting_scope_state(counting_scope_state &&) = delete;

    ~counting_scope_state() {
        auto const state = state_of(word.load(std::memory_order_relaxed));
        if (state != unused and state != unused_and_closed and state != joined) {
            std::terminate();
        }
    }

    void close() noexcept {
        auto current = word.load(std::memory_order_relaxed);
        while (true) {
            auto const state = state_of(current);
            auto const next = state == unused             ? unused_and_closed
                              : state == open             ? closed
                              : state == open_and_joining ? closed_and_joining
                                                          : state;
            if (next == state or
                word.compare_exchange_weak(current, with_state(current, next), std::memory_order_relaxed)) {
                return;
            }
        }
    }

protected:
    enum state_type : std::size_t {
        unused,
        open,
        closed,
        open_and_joining,
        closed_and_joining,
        unused_and_closed,
        joined,
    };

    bool associate() noexcept {
        auto current = word.load(std::memory_order_relaxed);
        while (true) {
            auto const state = state_of(current);
            if (count_of(current) == max_associations or (state != unused and state != open and state != open_and_joining)) {
                return false;
            }
            auto const next = make(count_of(current) + 1, state == unused ? open : state);
            if (word.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    // The last disassociation of a joining scope makes it joined in the same update, so
    // no association can slip in between.
    void disassociate() noexcept {
        auto current = word.load(std::memory_order_relaxed);
        while (true) {
            auto const state = state_of(current);
            auto const count = count_of(current) - 1;
            auto const finishing = count == 0 and (state == open_and_joining or state == closed_and_joining);
            if (word.compare_exchange_weak(current, make(count, finishing ? joined : state), std::memory_order_acq_rel)) {
                if (finishing) {
                    complete_joins();
                }
                return;
            }
        }
    }

    // True if the scope has no associations and is joined now; otherwise the waiter is
    // registered and completes when the last association ends.
    bool start_join(join_waiter &waiter) noexcept {
        auto const lock = std::lock_guard<std::mutex>{mutex};
        auto current = word.load(std::memory_order_acquire);
        while (true) {
            auto const state = state_of(current);
            auto const next = count_of(current) == 0                   ? joined
                              : state == open or state == open_and_joining ? open_and_joining
                                                                          : closed_and_joining;
            if (word.compare_exchange_weak(current, with_state(current, next), std::memory_order_acq_rel)) {
                if (next == joined) {
                    return true;
                }
                waiter.next = waiters;
                waiters = &waiter;
                return false;
            }
        }
    }

private:
    static constexpr std::size_t state_mask = 7;
    static constexpr std::size_t one = 8;

    static constexpr state_type state_of(std::size_t const w) noexcept { return static_cast<state_type>(w & state_mask); }
    static constexpr std::size_t count_of(std::size_t const w) noexcept { return w / one; }
    static constexpr std::size_t make(std::size_t const count, state_type const state) noexcept {
        return count * one + state;
    }
    static constexpr std::size_t with_state(std::size_t const w, state_type const state) noexcept {
        return make(count_of(w), state);
    }

    // Completing a join may destroy the scope, so the waiters are taken first.
    void complete_joins() noexcept {
        join_waiter *list = nullptr;
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            list = std::exchange(waiters, nullptr);
        }
        while (list != nullptr) {
            auto *const next = list->next;
            list->complete(list);
            list = next;
        }
    }

    std::atomic<std::size_t> word{unused};
    std::mutex mutex;
    join_waiter *waiters = nullptr;
};

template <class Env>
using start_scheduler_of_t = remove_cvref_t<decltype(get_start_scheduler(std::declval<Env const &>()))>;

template <class Env>
inline constexpr bool has_start_scheduler_v = is_detected_v<start_scheduler_of_t, Env>;

template <class Env, bool = has_start_scheduler_v<Env>>
struct join_completions {
    using type = completion_signatures<set_value_t()>;
};

// Completing asynchronously, a join goes through the receiver's start scheduler.
template <class Env>
struct join_completions<Env, true> {
    using type = unique_t<concat_t<completion_signatures<set_value_t()>,
                                   completion_signatures_of_t<schedule_result_t<start_scheduler_of_t<Env> &>, Env>>>;
};

template <class Scope, class Rcvr>
struct join_operation;

template <class Scope, class Rcvr>
struct join_schedule_receiver {
    using receiver_concept = receiver_t;

    void set_value() && noexcept { lexec::set_value(static_cast<Rcvr &&>(op->rcvr)); }

    template <class E>
    void set_error(E &&e) && noexcept {
        lexec::set_error(static_cast<Rcvr &&>(op->rcvr), static_cast<E &&>(e));
    }

    void set_stopped() && noexcept { lexec::set_stopped(static_cast<Rcvr &&>(op->rcvr)); }

    env_of_t<Rcvr> get_env() const noexcept { return lexec::get_env(op->rcvr); }

    join_operation<Scope, Rcvr> *op;
};

template <class Scope, class Rcvr, bool = has_start_scheduler_v<env_of_t<Rcvr>>>
struct join_schedule {
    explicit join_schedule(join_operation<Scope, Rcvr> *) noexcept {}

    static void start(join_operation<Scope, Rcvr> &op) noexcept { lexec::set_value(static_cast<Rcvr &&>(op.rcvr)); }
};

template <class Scope, class Rcvr>
struct join_schedule<Scope, Rcvr, true> {
    using sender = schedule_result_t<start_scheduler_of_t<env_of_t<Rcvr>> &>;
    using operation = connect_result_t<sender, join_schedule_receiver<Scope, Rcvr>>;

    explicit join_schedule(join_operation<Scope, Rcvr> *const op_)
        : op(lexec::connect(lexec::schedule(get_start_scheduler(lexec::get_env(op_->rcvr))),
                            join_schedule_receiver<Scope, Rcvr>{op_})) {}

    static void start(join_operation<Scope, Rcvr> &self) noexcept { lexec::start(self.schedule.op); }

    operation op;
};

template <class Scope, class Rcvr>
struct join_operation : join_waiter {
    using operation_state_concept = operation_state_t;

    join_operation(Scope *const scope_, Rcvr &&rcvr_)
        : join_waiter{nullptr, &complete_later}, scope(scope_), rcvr(static_cast<Rcvr &&>(rcvr_)), schedule(this) {}

    join_operation(join_operation &&) = delete;

    void start() & noexcept {
        if (scope->start_join(*this)) {
            lexec::set_value(static_cast<Rcvr &&>(rcvr));
        }
    }

    static void complete_later(join_waiter *const waiter) noexcept {
        auto &self = *static_cast<join_operation *>(waiter);
        join_schedule<Scope, Rcvr>::start(self);
    }

    Scope *scope;
    Rcvr rcvr;
    join_schedule<Scope, Rcvr> schedule;
};

template <class Scope>
struct join_sender {
    using sender_concept = sender_t;

    // Which completions a join has depends on the receiver's start scheduler.
    template <class Self, class Env>
    static auto get_completion_signatures() -> typename join_completions<Env>::type;

    template <class Rcvr>
    join_operation<Scope, Rcvr> connect(Rcvr rcvr) const {
        return {scope, static_cast<Rcvr &&>(rcvr)};
    }

    Scope *scope;
};

} // namespace detail

// Counts the associations of the work it tracks; join() completes once none remain.
// Destroying the scope before it is joined terminates, unless it was never used.
class simple_counting_scope : public detail::counting_scope_state {
public:
    using association = detail::association<simple_counting_scope>;

    struct token {
        template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
        Sndr &&wrap(Sndr &&sndr) const noexcept {
            return static_cast<Sndr &&>(sndr);
        }

        association try_associate() const noexcept { return scope->try_associate(); }

        simple_counting_scope *scope;
    };

    simple_counting_scope() noexcept = default;

    token get_token() noexcept { return token{this}; }
    detail::join_sender<simple_counting_scope> join() noexcept { return {this}; }

private:
    friend association;
    template <class, class>
    friend struct detail::join_operation;

    association try_associate() noexcept { return associate() ? association{this} : association{}; }
};

// A counting scope that can also request stop on the work it tracks: its token wraps
// each sender so that it sees the scope's stop requests too.
class counting_scope : public detail::counting_scope_state {
public:
    using association = detail::association<counting_scope>;

    struct token {
        template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
        auto wrap(Sndr &&sndr) const noexcept(std::is_nothrow_constructible_v<std::decay_t<Sndr>, Sndr>) {
            return detail::stop_when(static_cast<Sndr &&>(sndr), scope->source.get_token());
        }

        association try_associate() const noexcept { return scope->try_associate(); }

        counting_scope *scope;
    };

    counting_scope() noexcept = default;

    token get_token() noexcept { return token{this}; }
    detail::join_sender<counting_scope> join() noexcept { return {this}; }
    void request_stop() noexcept { source.request_stop(); }

private:
    friend association;
    template <class, class>
    friend struct detail::join_operation;

    association try_associate() noexcept { return associate() ? association{this} : association{}; }

    inplace_stop_source source;
};

} // namespace lexec
