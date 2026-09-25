#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace lexec::detail {

template <class... Ts>
struct type_list {};

template <class T>
struct type_identity {
    using type = T;
};

template <class T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <class...>
inline constexpr bool dependent_false = false;

template <class T, class... Ts>
inline constexpr bool is_one_of_v = (std::is_same_v<T, Ts> or ...);

template <class... Checks>
using all_of_t = std::bool_constant<(Checks::value and ...)>;

template <class... Ts>
using is_nothrow_decay_copyable_t = std::bool_constant<(std::is_nothrow_constructible_v<std::decay_t<Ts>, Ts> and ...)>;

// Detection idiom: whether Op<Args...> names a valid type.
template <class AlwaysVoid, template <class...> class Op, class... Args>
struct detector : std::false_type {};

template <template <class...> class Op, class... Args>
struct detector<std::void_t<Op<Args...>>, Op, Args...> : std::true_type {};

template <template <class...> class Op, class... Args>
inline constexpr bool is_detected_v = detector<void, Op, Args...>::value;

template <class AlwaysVoid, class Default, template <class...> class Op, class... Args>
struct detected_or {
    using type = Default;
};

template <class Default, template <class...> class Op, class... Args>
struct detected_or<std::void_t<Op<Args...>>, Default, Op, Args...> {
    using type = Op<Args...>;
};

template <class Default, template <class...> class Op, class... Args>
using detected_or_t = typename detected_or<void, Default, Op, Args...>::type;

template <class Fn, class... Args>
using call_result_t = decltype(std::declval<Fn>()(std::declval<Args>()...));

template <class Fn, class... Args>
inline constexpr bool is_callable_v = is_detected_v<call_result_t, Fn, Args...>;

template <bool Callable, class Fn, class... Args>
inline constexpr bool is_nothrow_callable_impl_v = false;

template <class Fn, class... Args>
inline constexpr bool is_nothrow_callable_impl_v<true, Fn, Args...> =
    noexcept(std::declval<Fn>()(std::declval<Args>()...));

template <class Fn, class... Args>
inline constexpr bool is_nothrow_callable_v = is_nothrow_callable_impl_v<is_callable_v<Fn, Args...>, Fn, Args...>;

// Concatenates lists that share one template, e.g. L<A, B> and L<C> into L<A, B, C>.
template <class... Lists>
struct concat;

template <template <class...> class L, class... As>
struct concat<L<As...>> {
    using type = L<As...>;
};

template <template <class...> class L, class... As, class... Bs, class... Rest>
struct concat<L<As...>, L<Bs...>, Rest...> : concat<L<As..., Bs...>, Rest...> {};

template <class... Lists>
using concat_t = typename concat<Lists...>::type;

template <class... Ts>
struct type_set : type_identity<Ts>... {};

template <class Unique, class... Ts>
struct unique_impl;

template <template <class...> class L, class... Seen>
struct unique_impl<L<Seen...>> {
    using type = L<Seen...>;
};

template <template <class...> class L, class... Seen, class T, class... Rest>
struct unique_impl<L<Seen...>, T, Rest...>
    : unique_impl<std::conditional_t<std::is_base_of_v<type_identity<T>, type_set<Seen...>>, L<Seen...>, L<Seen..., T>>,
                  Rest...> {};

// Removes duplicates while keeping the first occurrence of each element.
template <class List>
struct unique;

template <template <class...> class L, class... Ts>
struct unique<L<Ts...>> : unique_impl<L<>, Ts...> {};

template <class List>
using unique_t = typename unique<List>::type;

template <class List, template <class...> class To>
struct rename;

template <template <class...> class L, class... Ts, template <class...> class To>
struct rename<L<Ts...>, To> {
    using type = To<Ts...>;
};

template <class List, template <class...> class To>
using rename_t = typename rename<List, To>::type;

template <std::size_t I, class List>
struct type_at;

template <std::size_t I, class T, class... Ts>
struct type_at<I, type_list<T, Ts...>> : type_at<I - 1, type_list<Ts...>> {};

template <class T, class... Ts>
struct type_at<0, type_list<T, Ts...>> {
    using type = T;
};

template <std::size_t I, class... Ts>
using type_at_t = typename type_at<I, type_list<Ts...>>::type;

template <class List>
struct list_size;

template <template <class...> class L, class... Ts>
struct list_size<L<Ts...>> : std::integral_constant<std::size_t, sizeof...(Ts)> {};

template <class List>
inline constexpr std::size_t list_size_v = list_size<List>::value;

// Value category and constness of Self applied to a member of type T: an rvalue Self
// yields T (to be moved from), anything else yields T const & (to be copied from).
template <class Self, class T>
using member_like_t = std::conditional_t<std::is_lvalue_reference_v<Self> or std::is_const_v<std::remove_reference_t<Self>>,
                                         T const &, T>;

} // namespace lexec::detail
