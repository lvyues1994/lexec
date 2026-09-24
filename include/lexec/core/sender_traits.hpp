#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>

#include <tuple>
#include <type_traits>
#include <variant>

namespace lexec {

template <class... Ts>
using decayed_tuple = std::tuple<std::decay_t<Ts>...>;

namespace detail {

struct empty_variant {
    empty_variant() = delete;
};

template <class... Ts>
struct variant_or_empty {
    using type = rename_t<unique_t<type_list<std::decay_t<Ts>...>>, std::variant>;
};

template <>
struct variant_or_empty<> {
    using type = empty_variant;
};

template <class T>
using type_identity_t = typename type_identity<T>::type;

} // namespace detail

template <class... Ts>
using variant_or_empty = typename detail::variant_or_empty<Ts...>::type;

template <class Sndr, class Env = env<>, template <class...> class Tuple = decayed_tuple,
          template <class...> class Variant = variant_or_empty>
using value_types_of_t = detail::gather_signatures_t<set_value_t, completion_signatures_of_t<Sndr, Env>, Tuple, Variant>;

template <class Sndr, class Env = env<>, template <class...> class Variant = variant_or_empty>
using error_types_of_t =
    detail::gather_signatures_t<set_error_t, completion_signatures_of_t<Sndr, Env>, detail::type_identity_t, Variant>;

template <class Sndr, class Env = env<>>
inline constexpr bool sends_stopped =
    detail::count_signatures_v<set_stopped_t, completion_signatures_of_t<Sndr, Env>> != 0;

template <class Sndr, class Env = env<>, class AdditionalSigs = completion_signatures<>,
          template <class...> class SetValue = detail::default_set_value,
          template <class> class SetError = detail::default_set_error,
          class SetStopped = completion_signatures<set_stopped_t()>>
using transform_completion_signatures_of =
    transform_completion_signatures<completion_signatures_of_t<Sndr, Env>, AdditionalSigs, SetValue, SetError,
                                    SetStopped>;

} // namespace lexec
