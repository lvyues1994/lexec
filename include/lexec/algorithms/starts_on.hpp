#pragma once

#include <lexec/algorithms/continues_on.hpp>
#include <lexec/algorithms/just.hpp>
#include <lexec/algorithms/let.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/scheduler.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>

#include <type_traits>
#include <utility>

namespace lexec {

struct starts_on_t;
struct on_t;

namespace detail {

// let_value's function for starts_on: returns, once, the sender to start.
template <class Sndr>
struct return_sender_fn {
    constexpr Sndr operator()() noexcept(std::is_nothrow_move_constructible_v<Sndr>) { return static_cast<Sndr &&>(sndr); }

    Sndr sndr;
};

template <class Sndr>
constexpr auto lower_starts_on(Sndr &&sndr) {
    using child = remove_cvref_t<child_of_t<Sndr, 0>>;
    return let_value(continues_on(just(), static_cast<Sndr &&>(sndr).data),
                     return_sender_fn<child>{detail::get<0>(static_cast<Sndr &&>(sndr).children)});
}

template <class Sndr>
using lowered_starts_on_t = decltype(lower_starts_on(std::declval<Sndr>()));

// Completions do not depend on the environment the lowering happens in.
struct starts_on_impls : default_impls {
    template <class Self, class... Env>
    using completions = completion_signatures_of_t<lowered_starts_on_t<Self>, Env...>;
};

template <class Sch, class Closure>
struct on_data {
    Sch sch;
    Closure closure;
};

template <>
struct impls_for<starts_on_t> : starts_on_impls {};

template <>
struct impls_for<on_t> : lowered_impls {};

} // namespace detail

// Starts the sender on the scheduler's execution resource; the sender sees that
// scheduler as the current one.
struct starts_on_t {
    template <class Sch, class Sndr, std::enable_if_t<is_scheduler_v<Sch> and is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sch &&sch, Sndr &&sndr) const
        -> detail::basic_sender<starts_on_t, std::decay_t<Sch>, std::decay_t<Sndr>> {
        return {static_cast<Sch &&>(sch), {{static_cast<Sndr &&>(sndr)}}};
    }

    template <class Sndr, class Env>
    constexpr auto transform_sender(set_value_t, Sndr &&sndr, Env const &) const -> detail::lowered_starts_on_t<Sndr> {
        return detail::lower_starts_on(static_cast<Sndr &&>(sndr));
    }
};

inline constexpr starts_on_t starts_on{};

namespace detail {

template <class Env>
constexpr auto scheduler_to_return_to(Env const &env) noexcept {
    static_assert(std::is_invocable_v<get_scheduler_t, Env const &>,
                  "lexec::on: the receiver's environment has no current scheduler to return to");
    return get_scheduler(env);
}

template <class Sndr, class Env>
constexpr auto lower_on(Sndr &&sndr, Env const &env) {
    auto &&child = detail::get<0>(static_cast<Sndr &&>(sndr).children);
    using child_ref = decltype(child);
    if constexpr (is_scheduler_v<data_of_t<Sndr>>) {
        return continues_on(starts_on(static_cast<Sndr &&>(sndr).data, static_cast<child_ref>(child)),
                            scheduler_to_return_to(env));
    } else {
        auto &&data = static_cast<Sndr &&>(sndr).data;
        using data_ref = decltype(data);
        auto const orig = [&] {
            if constexpr (std::is_invocable_v<get_completion_scheduler_t<set_value_t>, env_of_t<child_ref>, Env const &>) {
                return get_completion_scheduler<set_value_t>(lexec::get_env(child), env);
            } else {
                return scheduler_to_return_to(env);
            }
        }();
        return continues_on(static_cast<data_ref>(data).closure(continues_on(static_cast<child_ref>(child), data.sch)),
                            orig);
    }
}

} // namespace detail

// on(sch, sndr) runs the sender on the scheduler and completes back where it was
// started; on(sndr, sch, closure) runs the closure's work on the scheduler and
// completes back where the sender completed.
struct on_t {
    template <class Sch, class Sndr, std::enable_if_t<is_scheduler_v<Sch> and is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sch &&sch, Sndr &&sndr) const
        -> detail::basic_sender<on_t, std::decay_t<Sch>, std::decay_t<Sndr>> {
        return {static_cast<Sch &&>(sch), {{static_cast<Sndr &&>(sndr)}}};
    }

    template <class Sndr, class Sch, class Closure,
              std::enable_if_t<is_sender_v<Sndr> and is_scheduler_v<Sch> and detail::is_sender_adaptor_closure_v<Closure>,
                               int> = 0>
    constexpr auto operator()(Sndr &&sndr, Sch &&sch, Closure &&closure) const
        -> detail::basic_sender<on_t, detail::on_data<std::decay_t<Sch>, std::decay_t<Closure>>, std::decay_t<Sndr>> {
        return {{static_cast<Sch &&>(sch), static_cast<Closure &&>(closure)}, {{static_cast<Sndr &&>(sndr)}}};
    }

    template <class Sch, class Closure,
              std::enable_if_t<is_scheduler_v<Sch> and detail::is_sender_adaptor_closure_v<Closure>, int> = 0>
    constexpr auto operator()(Sch &&sch, Closure &&closure) const
        -> detail::partial_closure<on_t, std::decay_t<Sch>, std::decay_t<Closure>> {
        return detail::make_partial_closure<on_t>(static_cast<Sch &&>(sch), static_cast<Closure &&>(closure));
    }

    template <class Sndr, class Env>
    constexpr auto transform_sender(set_value_t, Sndr &&sndr, Env const &env) const
        -> decltype(detail::lower_on(std::declval<Sndr>(), env)) {
        return detail::lower_on(static_cast<Sndr &&>(sndr), env);
    }
};

inline constexpr on_t on{};

} // namespace lexec
