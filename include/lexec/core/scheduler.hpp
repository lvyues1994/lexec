#pragma once

#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>

#include <type_traits>

namespace lexec {

namespace detail {

template <class Sch>
using schedule_member_t = decltype(std::declval<Sch>().schedule());

} // namespace detail

struct schedule_t {
    template <class Sch, class = detail::schedule_member_t<Sch>>
    constexpr auto operator()(Sch &&sch) const noexcept(noexcept(static_cast<Sch &&>(sch).schedule()))
        -> detail::schedule_member_t<Sch> {
        return static_cast<Sch &&>(sch).schedule();
    }
};

inline constexpr schedule_t schedule{};

template <class Sch>
using schedule_result_t = decltype(schedule(std::declval<Sch>()));

namespace detail {

template <class Sch>
using equality_probe_t = std::enable_if_t<std::is_convertible_v<decltype(std::declval<Sch const &>() ==
                                                                         std::declval<Sch const &>()),
                                                                bool> and
                                          std::is_convertible_v<decltype(std::declval<Sch const &>() !=
                                                                         std::declval<Sch const &>()),
                                                                bool>>;

template <class Sch>
using completion_scheduler_of_schedule_t =
    remove_cvref_t<decltype(get_completion_scheduler<set_value_t>(get_env(schedule(std::declval<Sch>()))))>;

template <class Sch, bool = is_detected_v<scheduler_concept_of_t, Sch> and is_detected_v<schedule_member_t, Sch>>
inline constexpr bool is_scheduler_impl_v = false;

template <class Sch>
inline constexpr bool is_scheduler_impl_v<Sch, true> =
    std::is_base_of_v<scheduler_t, typename Sch::scheduler_concept> and is_sender_v<schedule_result_t<Sch>> and
    std::is_same_v<detected_or_t<void, completion_scheduler_of_schedule_t, Sch>, Sch> and
    is_detected_v<equality_probe_t, Sch> and std::is_copy_constructible_v<Sch>;

} // namespace detail

// The sender returned by schedule() must report the scheduler itself as its value
// completion scheduler.
template <class Sch>
inline constexpr bool is_scheduler_v = detail::is_scheduler_impl_v<detail::remove_cvref_t<Sch>>;

} // namespace lexec
