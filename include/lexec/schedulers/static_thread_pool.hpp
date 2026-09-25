#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace lexec {

namespace detail {

// A unit of work queued on a static_thread_pool, embedded in the operation that runs it.
struct pool_task {
    pool_task *next = nullptr;
    void (*execute_fn)(pool_task *) noexcept = nullptr;
};

struct thread_pool_impl;

// Queues the task on the pool; defined in lexec::runtime.
void enqueue(thread_pool_impl &pool, pool_task *task) noexcept;

template <class Rcvr>
struct pool_operation;

struct pool_sender;

struct pool_scheduler {
    using scheduler_concept = scheduler_t;

    pool_sender schedule() const noexcept;

    friend bool operator==(pool_scheduler const lhs, pool_scheduler const rhs) noexcept { return lhs.pool == rhs.pool; }
    friend bool operator!=(pool_scheduler const lhs, pool_scheduler const rhs) noexcept { return lhs.pool != rhs.pool; }

    thread_pool_impl *pool;
};

struct pool_attrs {
    template <class Tag, class... Env, std::enable_if_t<is_one_of_v<Tag, set_value_t, set_stopped_t>, int> = 0>
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

    pool_attrs get_env() const noexcept { return pool_attrs{pool}; }

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

} // namespace detail

// A fixed set of worker threads, each with a work-stealing queue. Scheduling never
// allocates: the operation is itself the queued task. Destruction waits for the threads
// to drain their queues and exit, so nothing may be scheduled on the pool by then.
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
