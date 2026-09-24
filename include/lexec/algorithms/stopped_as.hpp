#pragma once

#include <lexec/algorithms/just.hpp>
#include <lexec/algorithms/let.hpp>
#include <lexec/algorithms/then.hpp>
#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>

#include <optional>
#include <type_traits>
#include <utility>

namespace lexec {

struct stopped_as_optional_t;
struct stopped_as_error_t;

namespace detail {

template <class ValueLists>
struct single_value_of {
    static_assert(dependent_false<ValueLists>,
                  "lexec::stopped_as_optional requires a sender whose only value completion sends one value");
};

template <class V>
struct single_value_of<type_list<type_list<V>>> {
    using type = std::decay_t<V>;
};

template <class Sigs>
using single_value_t = typename single_value_of<gather_signatures_t<set_value_t, Sigs, type_list, type_list>>::type;

template <class V>
struct to_optional_fn {
    template <class T>
    constexpr std::optional<V> operator()(T &&value) const noexcept(std::is_nothrow_constructible_v<V, T>) {
        return std::optional<V>{std::in_place, static_cast<T &&>(value)};
    }
};

template <class V>
struct just_nullopt_fn {
    constexpr auto operator()() const noexcept { return just(std::optional<V>{}); }
};

template <class E>
struct just_error_fn {
    constexpr auto operator()() noexcept(std::is_nothrow_move_constructible_v<E>) {
        return just_error(static_cast<E &&>(err));
    }

    E err;
};

template <class Self>
struct completions_after_lowering {};

// These algorithms exist only until transform_sender lowers them, which connect always
// does; without an environment to lower in, they are dependent.
struct lowered_impls : default_impls {
    template <class Self, class... Env>
    using completions = typename completions_after_lowering<Self>::type;

    template <class Sndr, class Rcvr>
    static no_data get_state(Sndr &&, Rcvr &) noexcept {
        static_assert(dependent_false<Sndr>, "lexec::stopped_as_optional/stopped_as_error must be connected with "
                                             "lexec::connect, which lowers them");
        return {};
    }
};

template <>
struct impls_for<stopped_as_optional_t> : lowered_impls {};

template <>
struct impls_for<stopped_as_error_t> : lowered_impls {};

} // namespace detail

// Turns a stopped completion into a value: an empty std::optional of the sender's
// single value type, which the value channel sends engaged.
struct stopped_as_optional_t : sender_adaptor_closure<stopped_as_optional_t> {
    template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sndr &&sndr) const
        -> detail::basic_sender<stopped_as_optional_t, detail::no_data, std::decay_t<Sndr>> {
        return {{}, {{static_cast<Sndr &&>(sndr)}}};
    }

    template <class Sndr, class Env, class V = detail::single_value_t<detail::child_completions_t<Sndr, 0, Env>>>
    constexpr auto transform_sender(set_value_t, Sndr &&sndr, Env const &) const
        -> decltype(let_stopped(then(std::declval<detail::child_of_t<Sndr, 0>>(), detail::to_optional_fn<V>{}),
                                detail::just_nullopt_fn<V>{})) {
        return let_stopped(then(detail::get<0>(static_cast<Sndr &&>(sndr).children), detail::to_optional_fn<V>{}),
                           detail::just_nullopt_fn<V>{});
    }
};

// Turns a stopped completion into an error completion with the given error.
struct stopped_as_error_t : detail::data_adaptor<stopped_as_error_t> {
    template <class Sndr, class Env, class E = detail::data_of_t<Sndr>>
    constexpr auto transform_sender(set_value_t, Sndr &&sndr, Env const &) const
        -> decltype(let_stopped(std::declval<detail::child_of_t<Sndr, 0>>(), std::declval<detail::just_error_fn<E>>())) {
        return let_stopped(detail::get<0>(static_cast<Sndr &&>(sndr).children),
                           detail::just_error_fn<E>{static_cast<Sndr &&>(sndr).data});
    }
};

inline constexpr stopped_as_optional_t stopped_as_optional{};
inline constexpr stopped_as_error_t stopped_as_error{};

} // namespace lexec
