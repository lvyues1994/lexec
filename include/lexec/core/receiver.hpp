#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/detail/meta.hpp>

#include <type_traits>

namespace lexec {

struct receiver_t {};

namespace detail {

template <class Rcvr>
using receiver_concept_of_t = typename Rcvr::receiver_concept;

template <class Rcvr, bool = is_detected_v<receiver_concept_of_t, Rcvr>>
inline constexpr bool enable_receiver_v = false;

template <class Rcvr>
inline constexpr bool enable_receiver_v<Rcvr, true> = std::is_base_of_v<receiver_t, typename Rcvr::receiver_concept>;

template <class Rcvr, class Sig>
inline constexpr bool accepts_signature_v = false;

template <class Rcvr, class... Vs>
inline constexpr bool accepts_signature_v<Rcvr, set_value_t(Vs...)> = std::is_invocable_v<set_value_t, Rcvr, Vs...>;

template <class Rcvr, class E>
inline constexpr bool accepts_signature_v<Rcvr, set_error_t(E)> = std::is_invocable_v<set_error_t, Rcvr, E>;

template <class Rcvr>
inline constexpr bool accepts_signature_v<Rcvr, set_stopped_t()> = std::is_invocable_v<set_stopped_t, Rcvr>;

template <class Rcvr, class Completions>
inline constexpr bool accepts_completions_v = false;

template <class Rcvr, class... Sigs>
inline constexpr bool accepts_completions_v<Rcvr, completion_signatures<Sigs...>> =
    (accepts_signature_v<Rcvr, Sigs> and ...);

} // namespace detail

template <class Rcvr>
inline constexpr bool is_receiver_v = detail::enable_receiver_v<detail::remove_cvref_t<Rcvr>> and
                                      std::is_move_constructible_v<detail::remove_cvref_t<Rcvr>> and
                                      std::is_constructible_v<detail::remove_cvref_t<Rcvr>, Rcvr>;

template <class Rcvr, class Completions>
inline constexpr bool is_receiver_of_v =
    is_receiver_v<Rcvr> and detail::accepts_completions_v<detail::remove_cvref_t<Rcvr>, Completions>;

} // namespace lexec
