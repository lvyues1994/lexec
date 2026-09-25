#pragma once

#include <lexec/detail/config.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace lexec::detail {

// Empty elements are stored as base classes rather than [[no_unique_address]] members:
// Clang 18 crashes generating code for nested aggregate initialization of the latter.
template <std::size_t I, class T, bool = std::is_empty_v<T> and not std::is_final_v<T>>
struct box {
    T value;
};

template <std::size_t I, class T>
struct box<I, T, true> : T {};

template <std::size_t I, class T>
constexpr T &get(box<I, T, false> &b) noexcept {
    return b.value;
}

template <std::size_t I, class T>
constexpr T const &get(box<I, T, false> const &b) noexcept {
    return b.value;
}

template <std::size_t I, class T>
constexpr T &&get(box<I, T, false> &&b) noexcept {
    return static_cast<T &&>(b.value);
}

template <std::size_t I, class T>
constexpr T &get(box<I, T, true> &b) noexcept {
    return b;
}

template <std::size_t I, class T>
constexpr T const &get(box<I, T, true> const &b) noexcept {
    return b;
}

template <std::size_t I, class T>
constexpr T &&get(box<I, T, true> &&b) noexcept {
    return static_cast<T &&>(b);
}

template <class Indices, class... Ts>
struct tuple_impl;

// An aggregate: `tuple<A, B>{{a}, {b}}` initializes each element directly from its
// initializer, so a prvalue of an immovable type is constructed in place.
template <std::size_t... Is, class... Ts>
struct tuple_impl<std::index_sequence<Is...>, Ts...> : box<Is, Ts>... {
    template <class Fn>
    constexpr decltype(auto) apply(Fn &&fn) & noexcept(noexcept(static_cast<Fn &&>(fn)(std::declval<Ts &>()...))) {
        return static_cast<Fn &&>(fn)(detail::get<Is>(*this)...);
    }

    template <class Fn>
    constexpr decltype(auto) apply(Fn &&fn) const & noexcept(
        noexcept(static_cast<Fn &&>(fn)(std::declval<Ts const &>()...))) {
        return static_cast<Fn &&>(fn)(detail::get<Is>(*this)...);
    }

    template <class Fn>
    constexpr decltype(auto) apply(Fn &&fn) && noexcept(noexcept(static_cast<Fn &&>(fn)(std::declval<Ts>()...))) {
        return static_cast<Fn &&>(fn)(detail::get<Is>(static_cast<tuple_impl &&>(*this))...);
    }
};

template <class... Ts>
using tuple = tuple_impl<std::index_sequence_for<Ts...>, Ts...>;

} // namespace lexec::detail
