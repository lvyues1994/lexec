#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/detail/meta.hpp>

#include <type_traits>

namespace lexec {

struct sender_t {};

namespace detail {

template <class Sndr>
using sender_concept_of_t = typename Sndr::sender_concept;

template <class Sndr, bool = is_detected_v<sender_concept_of_t, Sndr>>
inline constexpr bool enable_sender_v = false;

template <class Sndr>
inline constexpr bool enable_sender_v<Sndr, true> = std::is_base_of_v<sender_t, typename Sndr::sender_concept>;

} // namespace detail

template <class Sndr>
inline constexpr bool is_sender_v = detail::enable_sender_v<detail::remove_cvref_t<Sndr>> and
                                    std::is_move_constructible_v<detail::remove_cvref_t<Sndr>> and
                                    std::is_constructible_v<detail::remove_cvref_t<Sndr>, Sndr>;

namespace detail {

// A sender declares its completions either as a nested `completion_signatures` type,
// or, when they depend on the type of the sender or the environment, as
// `template <class Self, class... Env> static auto get_completion_signatures() -> Sigs;`
template <class Sndr, class... Env>
using completions_member_t = decltype(remove_cvref_t<Sndr>::template get_completion_signatures<Sndr, Env...>());

template <class Sndr>
using completions_typedef_t = typename remove_cvref_t<Sndr>::completion_signatures;

template <bool HasMember, bool HasTypedef, class Sndr, class... Env>
struct completion_signatures_of_impl {};

template <bool HasTypedef, class Sndr, class... Env>
struct completion_signatures_of_impl<true, HasTypedef, Sndr, Env...> {
    using type = completions_member_t<Sndr, Env...>;
};

template <class Sndr, class... Env>
struct completion_signatures_of_impl<false, true, Sndr, Env...> {
    using type = completions_typedef_t<Sndr>;
};

template <class Sndr, class... Env>
struct completion_signatures_of
    : completion_signatures_of_impl<is_detected_v<completions_member_t, Sndr, Env...>,
                                    is_detected_v<completions_typedef_t, Sndr>, Sndr, Env...> {
    static_assert(sizeof...(Env) <= 1, "completion signatures are computed with at most one environment");
};

} // namespace detail

template <class Sndr, class... Env>
using completion_signatures_of_t = typename detail::completion_signatures_of<Sndr, Env...>::type;

template <class Sndr, class... Env>
constexpr completion_signatures_of_t<Sndr, Env...> get_completion_signatures() noexcept {
    return {};
}

template <class Sndr, class... Env>
inline constexpr bool is_sender_in_v =
    is_sender_v<Sndr> and detail::is_detected_v<completion_signatures_of_t, Sndr, Env...>;

// A sender whose completions cannot be known without the receiver's environment.
template <class Sndr>
inline constexpr bool is_dependent_sender_v = is_sender_v<Sndr> and not is_sender_in_v<Sndr>;

namespace detail {

template <class Sndr, class Rcvr>
using connect_member_t = decltype(std::declval<Sndr>().connect(std::declval<Rcvr>()));

template <class Sndr, class Rcvr>
constexpr bool check_connect() noexcept {
    if constexpr (not is_receiver_v<Rcvr>) {
        static_assert(dependent_false<Rcvr>,
                      "lexec::connect: the second argument is not a receiver; "
                      "does it declare `using receiver_concept = lexec::receiver_t;`?");
    } else if constexpr (not is_sender_in_v<Sndr, env_of_t<Rcvr>>) {
        static_assert(dependent_false<Sndr>,
                      "lexec::connect: the sender's completion signatures cannot be computed "
                      "in the receiver's environment");
    } else if constexpr (not is_receiver_of_v<Rcvr, completion_signatures_of_t<Sndr, env_of_t<Rcvr>>>) {
        static_assert(dependent_false<Sndr>,
                      "lexec::connect: the receiver does not accept every completion the sender may produce");
    }
    return true;
}

} // namespace detail

struct connect_t {
    template <class Sndr, class Rcvr, class = detail::connect_member_t<Sndr, Rcvr>>
    constexpr auto operator()(Sndr &&sndr, Rcvr &&rcvr) const
        noexcept(noexcept(static_cast<Sndr &&>(sndr).connect(static_cast<Rcvr &&>(rcvr))))
            -> detail::connect_member_t<Sndr, Rcvr> {
        static_assert(detail::check_connect<Sndr, detail::remove_cvref_t<Rcvr>>());
        return static_cast<Sndr &&>(sndr).connect(static_cast<Rcvr &&>(rcvr));
    }
};

inline constexpr connect_t connect{};

template <class Sndr, class Rcvr>
using connect_result_t = decltype(connect(std::declval<Sndr>(), std::declval<Rcvr>()));

namespace detail {

template <bool SenderIn, class Sndr, class Rcvr>
inline constexpr bool is_sender_to_impl_v = false;

template <class Sndr, class Rcvr>
inline constexpr bool is_sender_to_impl_v<true, Sndr, Rcvr> =
    is_receiver_of_v<Rcvr, completion_signatures_of_t<Sndr, env_of_t<Rcvr>>> and
    is_detected_v<connect_result_t, Sndr, Rcvr>;

} // namespace detail

template <class Sndr, class Rcvr>
inline constexpr bool is_sender_to_v = detail::is_sender_to_impl_v<is_sender_in_v<Sndr, env_of_t<Rcvr>>, Sndr, Rcvr>;

} // namespace lexec
