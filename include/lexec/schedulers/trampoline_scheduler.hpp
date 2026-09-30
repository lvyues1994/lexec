#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/stop_token.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lexec {

namespace detail {

// Work the trampoline runs on the thread that hands it over: at once, or once the stack
// has unwound to the outermost trampolined run there.
struct trampoline_task {
    void (*execute)(trampoline_task *) noexcept;
    trampoline_task *next = nullptr;
};

struct trampoline_limits {
    std::size_t max_depth;
    std::size_t max_stack_bytes;

    friend constexpr bool operator==(trampoline_limits const a, trampoline_limits const b) noexcept {
        return a.max_depth == b.max_depth and a.max_stack_bytes == b.max_stack_bytes;
    }
    friend constexpr bool operator!=(trampoline_limits const a, trampoline_limits const b) noexcept {
        return not(a == b);
    }
};

inline constexpr trampoline_limits default_trampoline_limits{16, 4096};

// The outermost trampolined run on a thread, whose limits hold for every run nested in
// it. Depth and stack use count from where the run in progress began: the outermost
// run itself, or the queued task it is running.
struct trampoline_frame {
    trampoline_limits limits;
    std::uintptr_t base;
    std::size_t depth = 1;
    trampoline_task *head = nullptr;
    trampoline_task *tail = nullptr;
};

inline thread_local trampoline_frame *current_trampoline = nullptr;

inline std::uintptr_t stack_address(void const *const local) noexcept { return reinterpret_cast<std::uintptr_t>(local); }

// Runs the task inline, unless it would nest too deeply in trampolined work on this
// thread; then it waits in the outermost run's queue for the stack to unwind. The
// caller must not touch the task's owner afterwards: it may have completed.
inline void trampoline_run(trampoline_task &task, trampoline_limits const limits) noexcept {
    auto *const frame = current_trampoline;
    if (frame == nullptr) {
        auto outermost = trampoline_frame{limits, 0};
        outermost.base = stack_address(&outermost);
        current_trampoline = &outermost;
        task.execute(&task);
        while (outermost.head != nullptr) {
            auto *const queued = outermost.head;
            outermost.head = queued->next;
            if (outermost.head == nullptr) {
                outermost.tail = nullptr;
            }
            outermost.base = stack_address(&queued);
            queued->execute(queued);
        }
        current_trampoline = nullptr;
        return;
    }
    auto const here = stack_address(&frame);
    auto const used = here < frame->base ? frame->base - here : here - frame->base;
    if (frame->depth < frame->limits.max_depth and used < frame->limits.max_stack_bytes) {
        ++frame->depth;
        task.execute(&task);
        --frame->depth;
    } else {
        task.next = nullptr;
        (frame->tail != nullptr ? frame->tail->next : frame->head) = &task;
        frame->tail = &task;
    }
}

template <class Rcvr>
struct trampoline_operation : trampoline_task {
    using operation_state_concept = operation_state_t;

    trampoline_operation(Rcvr &&rcvr_, trampoline_limits const limits_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : trampoline_task{&run}, rcvr(static_cast<Rcvr &&>(rcvr_)), limits(limits_) {}

    trampoline_operation(trampoline_operation &&) = delete;

    void start() & noexcept { trampoline_run(*this, limits); }

private:
    static void run(trampoline_task *const task) noexcept {
        auto &self = *static_cast<trampoline_operation *>(task);
        if constexpr (not is_unstoppable_token_v<stop_token_of_t<env_of_t<Rcvr>>>) {
            if (lexec::get_stop_token(lexec::get_env(self.rcvr)).stop_requested()) {
                lexec::set_stopped(static_cast<Rcvr &&>(self.rcvr));
                return;
            }
        }
        lexec::set_value(static_cast<Rcvr &&>(self.rcvr));
    }

    Rcvr rcvr;
    trampoline_limits limits;
};

// Without an environment, whether the receiver can ask to stop is unknown, so the
// sender is dependent.
template <class EnvList>
struct trampoline_completions {};

template <class Env>
struct trampoline_completions<type_list<Env>> {
    using type = std::conditional_t<is_unstoppable_token_v<stop_token_of_t<Env>>, completion_signatures<set_value_t()>,
                                    completion_signatures<set_value_t(), set_stopped_t()>>;
};

struct trampoline_sender {
    using sender_concept = sender_t;

    template <class Self, class... Env>
    static auto get_completion_signatures() -> typename trampoline_completions<type_list<Env...>>::type;

    template <class Rcvr>
    trampoline_operation<Rcvr> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return trampoline_operation<Rcvr>{static_cast<Rcvr &&>(rcvr), limits};
    }

    constexpr inline_attrs<set_value_t, set_stopped_t> get_env() const noexcept { return {}; }

    trampoline_limits limits;
};

} // namespace detail

// Completes schedule() on the thread that starts it: at once, unless that would nest it
// more than max_depth runs, or max_stack_bytes of stack, deep in other trampolined work
// on that thread; then it completes once the stack has unwound to the outermost such run.
// Before completing it checks the receiver's stop token, completing with set_stopped when
// stop was requested. Loops of synchronous completions go through it to bound the stack.
struct trampoline_scheduler : detail::inline_attrs<set_value_t, set_stopped_t> {
    using scheduler_concept = scheduler_t;

    constexpr trampoline_scheduler() noexcept = default;
    constexpr explicit trampoline_scheduler(std::size_t const max_depth, std::size_t const max_stack_bytes = 4096) noexcept
        : limits{max_depth, max_stack_bytes} {}

    constexpr detail::trampoline_sender schedule() const noexcept { return {limits}; }

    friend constexpr bool operator==(trampoline_scheduler const &a, trampoline_scheduler const &b) noexcept {
        return a.limits == b.limits;
    }
    friend constexpr bool operator!=(trampoline_scheduler const &a, trampoline_scheduler const &b) noexcept {
        return not(a == b);
    }

private:
    detail::trampoline_limits limits = detail::default_trampoline_limits;
};

} // namespace lexec
