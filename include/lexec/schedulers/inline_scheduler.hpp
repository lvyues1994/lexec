#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/sender.hpp>

#include <type_traits>

namespace lexec {

namespace detail {

template <class Rcvr>
struct inline_operation {
    using operation_state_concept = operation_state_t;

    void start() & noexcept { lexec::set_value(static_cast<Rcvr &&>(rcvr)); }

    Rcvr rcvr;
};

struct inline_sender {
    using sender_concept = sender_t;
    using completion_signatures = lexec::completion_signatures<set_value_t()>;

    template <class Rcvr>
    constexpr inline_operation<Rcvr> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {static_cast<Rcvr &&>(rcvr)};
    }

    constexpr inline_attrs<set_value_t> get_env() const noexcept { return {}; }
};

} // namespace detail

// Completes schedule() at once on the thread that starts it. Asked with an environment,
// it reports the scheduler it was started on as the one it completes on.
struct inline_scheduler : detail::inline_attrs<set_value_t> {
    using scheduler_concept = scheduler_t;

    constexpr detail::inline_sender schedule() const noexcept { return {}; }

    friend constexpr bool operator==(inline_scheduler, inline_scheduler) noexcept { return true; }
    friend constexpr bool operator!=(inline_scheduler, inline_scheduler) noexcept { return false; }
};

} // namespace lexec
