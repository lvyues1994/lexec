#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/detail/tuple.hpp>

#include <type_traits>

namespace lexec {

struct just_t;
struct just_error_t;
struct just_stopped_t;

namespace detail {

template <class SetTag, class Data>
struct just_completions;

template <class SetTag, class Indices, class... Ts>
struct just_completions<SetTag, tuple_impl<Indices, Ts...>> {
    using type = completion_signatures<SetTag(Ts...)>;
};

template <class SetTag>
struct just_impls : default_impls {
    template <class Self, class... Env>
    using completions = typename just_completions<SetTag, data_of_t<Self>>::type;

    template <class State, class Rcvr>
    static constexpr void start(State &state, Rcvr &rcvr) noexcept {
        static_cast<State &&>(state).apply([&rcvr](auto &&...vs) noexcept {
            SetTag{}(static_cast<Rcvr &&>(rcvr), static_cast<decltype(vs) &&>(vs)...);
        });
    }
};

template <>
struct impls_for<just_t> : just_impls<set_value_t> {};

template <>
struct impls_for<just_error_t> : just_impls<set_error_t> {};

template <>
struct impls_for<just_stopped_t> : just_impls<set_stopped_t> {};

} // namespace detail

// Deduced return types keep `tuple<std::decay_t<Ts>...>` out of the mangled signature,
// which GCC cannot mangle.
struct just_t {
    template <class... Ts>
    constexpr auto operator()(Ts &&...ts) const
        noexcept((std::is_nothrow_constructible_v<std::decay_t<Ts>, Ts> and ...)) {
        return detail::basic_sender<just_t, detail::tuple<std::decay_t<Ts>...>>{{{static_cast<Ts &&>(ts)}...}, {}};
    }
};

struct just_error_t {
    template <class E>
    constexpr auto operator()(E &&e) const noexcept(std::is_nothrow_constructible_v<std::decay_t<E>, E>) {
        return detail::basic_sender<just_error_t, detail::tuple<std::decay_t<E>>>{{{static_cast<E &&>(e)}}, {}};
    }
};

struct just_stopped_t {
    constexpr auto operator()() const noexcept { return detail::basic_sender<just_stopped_t, detail::tuple<>>{{}, {}}; }
};

inline constexpr just_t just{};
inline constexpr just_error_t just_error{};
inline constexpr just_stopped_t just_stopped{};

} // namespace lexec
