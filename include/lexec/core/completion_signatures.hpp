#pragma once

#include <lexec/core/completion_tags.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>

#include <exception>
#include <type_traits>

namespace lexec {

namespace detail {

template <class Sig>
inline constexpr bool is_completion_signature_v = false;

template <class... Vs>
inline constexpr bool is_completion_signature_v<set_value_t(Vs...)> = true;

template <class E>
inline constexpr bool is_completion_signature_v<set_error_t(E)> = true;

template <>
inline constexpr bool is_completion_signature_v<set_stopped_t()> = true;

} // namespace detail

template <class... Sigs>
struct completion_signatures {
    static_assert((detail::is_completion_signature_v<Sigs> and ...),
                  "completion_signatures accepts only set_value_t(Vs...), set_error_t(E), and set_stopped_t()");
};

template <class T>
inline constexpr bool is_completion_signatures_v = false;

template <class... Sigs>
inline constexpr bool is_completion_signatures_v<completion_signatures<Sigs...>> = true;

namespace detail {

template <class... Vs>
using default_set_value = completion_signatures<set_value_t(Vs...)>;

template <class E>
using default_set_error = completion_signatures<set_error_t(E)>;

// A transform that drops the signatures of its channel.
template <class...>
using no_signatures = completion_signatures<>;

// Transforms for results that are stored decayed and then sent as rvalues.
template <class... Vs>
using decayed_set_value = completion_signatures<set_value_t(std::decay_t<Vs>...)>;

template <class E>
using decayed_set_error = completion_signatures<set_error_t(std::decay_t<E>)>;

// Applies the transform for Tag's channel to Sig, or yields no signatures when Sig
// belongs to another channel.
template <class Tag, class Sig, template <class...> class SetValue, template <class> class SetError, class SetStopped>
struct transform_signature {
    using type = completion_signatures<>;
};

template <class... Vs, template <class...> class SetValue, template <class> class SetError, class SetStopped>
struct transform_signature<set_value_t, set_value_t(Vs...), SetValue, SetError, SetStopped> {
    using type = SetValue<Vs...>;
};

template <class E, template <class...> class SetValue, template <class> class SetError, class SetStopped>
struct transform_signature<set_error_t, set_error_t(E), SetValue, SetError, SetStopped> {
    using type = SetError<E>;
};

template <template <class...> class SetValue, template <class> class SetError, class SetStopped>
struct transform_signature<set_stopped_t, set_stopped_t(), SetValue, SetError, SetStopped> {
    using type = SetStopped;
};

template <class InputSigs, class AdditionalSigs, template <class...> class SetValue, template <class> class SetError,
          class SetStopped>
struct transform_completion_signatures_impl;

template <class... Sigs, class AdditionalSigs, template <class...> class SetValue, template <class> class SetError,
          class SetStopped>
struct transform_completion_signatures_impl<completion_signatures<Sigs...>, AdditionalSigs, SetValue, SetError,
                                            SetStopped> {
    using type =
        unique_t<concat_t<AdditionalSigs,
                          typename transform_signature<set_value_t, Sigs, SetValue, SetError, SetStopped>::type...,
                          typename transform_signature<set_error_t, Sigs, SetValue, SetError, SetStopped>::type...,
                          typename transform_signature<set_stopped_t, Sigs, SetValue, SetError, SetStopped>::type...>>;
};

} // namespace detail

// Maps each input signature through the transform for its channel, prepends
// AdditionalSigs, and removes duplicates. Each transform yields completion_signatures.
template <class InputSigs, class AdditionalSigs = completion_signatures<>,
          template <class...> class SetValue = detail::default_set_value,
          template <class> class SetError = detail::default_set_error,
          class SetStopped = completion_signatures<set_stopped_t()>>
using transform_completion_signatures =
    typename detail::transform_completion_signatures_impl<InputSigs, AdditionalSigs, SetValue, SetError,
                                                          SetStopped>::type;

namespace detail {

template <class Tag, class Sig, template <class...> class Tuple>
struct gather_signature {
    using type = type_list<>;
};

template <class... Vs, template <class...> class Tuple>
struct gather_signature<set_value_t, set_value_t(Vs...), Tuple> {
    using type = type_list<Tuple<Vs...>>;
};

template <class E, template <class...> class Tuple>
struct gather_signature<set_error_t, set_error_t(E), Tuple> {
    using type = type_list<Tuple<E>>;
};

template <template <class...> class Tuple>
struct gather_signature<set_stopped_t, set_stopped_t(), Tuple> {
    using type = type_list<Tuple<>>;
};

template <class Tag, class Sigs, template <class...> class Tuple, template <class...> class Variant>
struct gather_signatures_impl;

template <class Tag, class... Sigs, template <class...> class Tuple, template <class...> class Variant>
struct gather_signatures_impl<Tag, completion_signatures<Sigs...>, Tuple, Variant> {
    using type = rename_t<concat_t<type_list<>, typename gather_signature<Tag, Sigs, Tuple>::type...>, Variant>;
};

// Variant<Tuple<Args...>...> over the signatures of channel Tag, in signature order.
template <class Tag, class Sigs, template <class...> class Tuple, template <class...> class Variant>
using gather_signatures_t = typename gather_signatures_impl<Tag, Sigs, Tuple, Variant>::type;

template <class Tag, class Sigs>
inline constexpr std::size_t count_signatures_v = list_size_v<gather_signatures_t<Tag, Sigs, type_list, type_list>>;

template <bool MayThrow>
using eptr_completion_if_t = std::conditional_t<LEXEC_HAS_EXCEPTIONS and MayThrow,
                                                completion_signatures<set_error_t(std::exception_ptr)>,
                                                completion_signatures<>>;

} // namespace detail

} // namespace lexec
