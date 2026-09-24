#pragma once

#include <lexec/detail/meta.hpp>

#include <type_traits>

namespace lexec {

namespace detail {

// Completion functions consume the receiver: they accept only non-const rvalues.
template <class Rcvr>
inline constexpr bool is_consumable_receiver_v = not std::is_reference_v<Rcvr> and not std::is_const_v<Rcvr>;

template <class Rcvr, class... Vs>
using set_value_member_t = decltype(std::declval<Rcvr>().set_value(std::declval<Vs>()...));

template <class Rcvr, class E>
using set_error_member_t = decltype(std::declval<Rcvr>().set_error(std::declval<E>()));

template <class Rcvr>
using set_stopped_member_t = decltype(std::declval<Rcvr>().set_stopped());

} // namespace detail

struct set_value_t {
    template <class Rcvr, class... Vs,
              std::enable_if_t<detail::is_consumable_receiver_v<Rcvr> and
                                   detail::is_detected_v<detail::set_value_member_t, Rcvr, Vs...>,
                               int> = 0>
    constexpr void operator()(Rcvr &&rcvr, Vs &&...vs) const noexcept {
        static_assert(noexcept(static_cast<Rcvr &&>(rcvr).set_value(static_cast<Vs &&>(vs)...)),
                      "receiver::set_value must be noexcept");
        static_cast<Rcvr &&>(rcvr).set_value(static_cast<Vs &&>(vs)...);
    }
};

struct set_error_t {
    template <class Rcvr, class E,
              std::enable_if_t<detail::is_consumable_receiver_v<Rcvr> and
                                   detail::is_detected_v<detail::set_error_member_t, Rcvr, E>,
                               int> = 0>
    constexpr void operator()(Rcvr &&rcvr, E &&e) const noexcept {
        static_assert(noexcept(static_cast<Rcvr &&>(rcvr).set_error(static_cast<E &&>(e))),
                      "receiver::set_error must be noexcept");
        static_cast<Rcvr &&>(rcvr).set_error(static_cast<E &&>(e));
    }
};

struct set_stopped_t {
    template <class Rcvr, std::enable_if_t<detail::is_consumable_receiver_v<Rcvr> and
                                               detail::is_detected_v<detail::set_stopped_member_t, Rcvr>,
                                           int> = 0>
    constexpr void operator()(Rcvr &&rcvr) const noexcept {
        static_assert(noexcept(static_cast<Rcvr &&>(rcvr).set_stopped()), "receiver::set_stopped must be noexcept");
        static_cast<Rcvr &&>(rcvr).set_stopped();
    }
};

inline constexpr set_value_t set_value{};
inline constexpr set_error_t set_error{};
inline constexpr set_stopped_t set_stopped{};

namespace detail {

template <class Tag>
inline constexpr bool is_completion_tag_v = is_one_of_v<Tag, set_value_t, set_error_t, set_stopped_t>;

} // namespace detail

} // namespace lexec
