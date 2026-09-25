#pragma once

#include <lexec/detail/config.hpp>

#include <type_traits>

#if __has_include(<version>)
#include <version>
#endif

// The standard execution policies when the standard library provides them, stand-ins
// otherwise. LEXEC_NO_STD_EXECUTION_POLICY forces the stand-ins, for libraries whose
// <execution> does not compile, such as that of GCC 9 and 10 next to oneTBB's headers.
#if defined(__cpp_lib_execution) && __cpp_lib_execution >= 201603L && !defined(LEXEC_NO_STD_EXECUTION_POLICY)
#define LEXEC_STD_EXECUTION_POLICY 1
#include <execution>
#else
#define LEXEC_STD_EXECUTION_POLICY 0
#endif

namespace lexec {

#if LEXEC_STD_EXECUTION_POLICY

using std::execution::parallel_policy;
using std::execution::parallel_unsequenced_policy;
using std::execution::sequenced_policy;

using std::execution::par;
using std::execution::par_unseq;
using std::execution::seq;

template <class T>
inline constexpr bool is_execution_policy_v = std::is_execution_policy_v<T>;

#else

struct sequenced_policy {};
struct parallel_policy {};
struct parallel_unsequenced_policy {};

inline constexpr sequenced_policy seq{};
inline constexpr parallel_policy par{};
inline constexpr parallel_unsequenced_policy par_unseq{};

template <class T>
inline constexpr bool is_execution_policy_v = false;
template <>
inline constexpr bool is_execution_policy_v<sequenced_policy> = true;
template <>
inline constexpr bool is_execution_policy_v<parallel_policy> = true;
template <>
inline constexpr bool is_execution_policy_v<parallel_unsequenced_policy> = true;

#endif

// unseq is C++20; MSVC's library, for one, offers it only in C++20 mode.
#if LEXEC_STD_EXECUTION_POLICY && __cpp_lib_execution >= 201902L

using std::execution::unseq;
using std::execution::unsequenced_policy;

#else

struct unsequenced_policy {};

inline constexpr unsequenced_policy unseq{};

template <>
inline constexpr bool is_execution_policy_v<unsequenced_policy> = true;

#endif

template <class T>
struct is_execution_policy : std::bool_constant<is_execution_policy_v<T>> {};

namespace detail {

// Whether a policy lets invocations run on several execution agents at once. Policies
// other than the standard four, which the library may define, are treated as sequenced.
template <class Policy>
inline constexpr bool is_parallel_policy_v =
    std::is_same_v<Policy, parallel_policy> or std::is_same_v<Policy, parallel_unsequenced_policy>;

} // namespace detail

} // namespace lexec
