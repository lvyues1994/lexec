#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/scheduler.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <type_traits>

namespace lexec {

struct run_loop;

namespace detail {

// Intrusive queue node embedded in each scheduled operation; enqueueing never allocates.
struct run_loop_task {
    run_loop_task *next = nullptr;
    void (*execute_fn)(run_loop_task *) noexcept = nullptr;
};

template <class Rcvr>
struct run_loop_operation;

struct run_loop_sender;

struct run_loop_scheduler {
    using scheduler_concept = scheduler_t;

    run_loop_sender schedule() const noexcept;

    static constexpr forward_progress_guarantee query(get_forward_progress_guarantee_t) noexcept {
        return forward_progress_guarantee::parallel;
    }

    friend bool operator==(run_loop_scheduler lhs, run_loop_scheduler rhs) noexcept { return lhs.loop == rhs.loop; }
    friend bool operator!=(run_loop_scheduler lhs, run_loop_scheduler rhs) noexcept { return lhs.loop != rhs.loop; }

    run_loop *loop;
};

struct run_loop_attrs {
    template <class Tag, class... Env, std::enable_if_t<is_one_of_v<Tag, set_value_t, set_stopped_t>, int> = 0>
    run_loop_scheduler query(get_completion_scheduler_t<Tag>, Env const &...) const noexcept {
        return run_loop_scheduler{loop};
    }

    run_loop *loop;
};

struct run_loop_sender {
    using sender_concept = sender_t;
    using completion_signatures = concat_t<lexec::completion_signatures<set_value_t(), set_stopped_t()>,
                                           eptr_completion_if_t<true>>;

    template <class Rcvr>
    run_loop_operation<Rcvr> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {loop, static_cast<Rcvr &&>(rcvr)};
    }

    run_loop_attrs get_env() const noexcept { return run_loop_attrs{loop}; }

    run_loop *loop;
};

inline run_loop_sender run_loop_scheduler::schedule() const noexcept { return run_loop_sender{loop}; }

} // namespace detail

// A FIFO execution resource driven by whichever thread calls run().
struct run_loop {
    run_loop() noexcept {}
    run_loop(run_loop &&) = delete;
    ~run_loop();

    detail::run_loop_scheduler get_scheduler() noexcept { return detail::run_loop_scheduler{this}; }

    // Executes queued work until finish() has been called and the queue is empty.
    void run();
    void finish();

private:
    template <class Rcvr>
    friend struct detail::run_loop_operation;

    enum class loop_state : std::uint8_t { starting, running, finishing, finished };

    void push_back(detail::run_loop_task *task);
    detail::run_loop_task *pop_front();

    std::mutex mutex;
    std::condition_variable cv;
    detail::run_loop_task *head = nullptr;
    detail::run_loop_task *tail = nullptr;
    loop_state state = loop_state::starting;
};

namespace detail {

template <class Rcvr>
struct run_loop_operation : run_loop_task {
    using operation_state_concept = operation_state_t;

    run_loop_operation(run_loop *loop_, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : run_loop_task{nullptr, &execute_impl}, loop(loop_), rcvr(static_cast<Rcvr &&>(rcvr_)) {}

    run_loop_operation(run_loop_operation &&) = delete;

    void start() & noexcept {
#if LEXEC_HAS_EXCEPTIONS
        try {
            loop->push_back(this);
        } catch (...) {
            lexec::set_error(static_cast<Rcvr &&>(rcvr), std::current_exception());
        }
#else
        loop->push_back(this);
#endif
    }

    static void execute_impl(run_loop_task *task) noexcept {
        auto &self = *static_cast<run_loop_operation *>(task);
        if (lexec::get_stop_token(lexec::get_env(self.rcvr)).stop_requested()) {
            lexec::set_stopped(static_cast<Rcvr &&>(self.rcvr));
        } else {
            lexec::set_value(static_cast<Rcvr &&>(self.rcvr));
        }
    }

    run_loop *loop;
    Rcvr rcvr;
};

} // namespace detail

inline run_loop::~run_loop() {
    if (head != nullptr or state == loop_state::running) {
        std::terminate();
    }
}

inline void run_loop::run() {
    {
        auto const lock = std::lock_guard<std::mutex>{mutex};
        assert(state == loop_state::starting or state == loop_state::finishing);
        if (state == loop_state::starting) {
            state = loop_state::running;
        }
    }
    while (auto *const task = pop_front()) {
        task->execute_fn(task);
    }
}

// Notifying while holding the lock matters: once it is released, the thread in run()
// may return and destroy this loop.
inline void run_loop::finish() {
    auto const lock = std::lock_guard<std::mutex>{mutex};
    assert(state == loop_state::starting or state == loop_state::running);
    state = loop_state::finishing;
    cv.notify_all();
}

inline void run_loop::push_back(detail::run_loop_task *const task) {
    auto const lock = std::lock_guard<std::mutex>{mutex};
    task->next = nullptr;
    if (tail == nullptr) {
        head = task;
    } else {
        tail->next = task;
    }
    tail = task;
    cv.notify_one();
}

inline detail::run_loop_task *run_loop::pop_front() {
    auto lock = std::unique_lock<std::mutex>{mutex};
    cv.wait(lock, [this] { return head != nullptr or state == loop_state::finishing; });
    if (head == nullptr) {
        state = loop_state::finished;
        return nullptr;
    }
    auto *const task = head;
    head = task->next;
    if (head == nullptr) {
        tail = nullptr;
    }
    return task;
}

} // namespace lexec
