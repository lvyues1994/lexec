#pragma once

#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/detail/meta.hpp>

#include <type_traits>
#include <utility>

namespace lexec {

namespace detail {

template <class Sndr, class Env>
using completion_domain_query_t =
    remove_cvref_t<decltype(get_completion_domain<>(get_env(std::declval<Sndr>()), std::declval<Env const &>()))>;

// The domain whose transformations apply first: where the sender completes.
template <class Sndr, class Env>
using completion_domain_of_t = detected_or_t<default_domain, completion_domain_query_t, Sndr, Env>;

// The domain whose transformations apply second: where the operation is started.
template <class Env>
using start_domain_of_t = remove_cvref_t<decltype(get_domain(std::declval<Env const &>()))>;

template <class Domain, class Tag, class Sndr, class Env>
using domain_transform_result_t =
    decltype(std::declval<Domain>().transform_sender(Tag{}, std::declval<Sndr>(), std::declval<Env const &>()));

enum class transform_kind : unsigned char { identity, by_domain, by_tag };

// default_domain only forwards to the algorithm tag, so it is resolved at that level to
// avoid instantiating its identity case.
template <class Domain, class Tag, class Sndr, class Env>
constexpr transform_kind find_transform_kind() noexcept {
    if constexpr (not std::is_same_v<Domain, default_domain> and
                  is_detected_v<domain_transform_result_t, Domain, Tag, Sndr, Env>) {
        using result = remove_cvref_t<domain_transform_result_t<Domain, Tag, Sndr, Env>>;
        return std::is_same_v<result, remove_cvref_t<Sndr>> ? transform_kind::identity : transform_kind::by_domain;
    } else if constexpr (is_detected_v<tag_transform_result_t, Tag, Sndr, Env>) {
        using result = remove_cvref_t<tag_transform_result_t<Tag, Sndr, Env>>;
        return std::is_same_v<result, remove_cvref_t<Sndr>> ? transform_kind::identity : transform_kind::by_tag;
    } else {
        return transform_kind::identity;
    }
}

template <class Domain, class Tag, class Sndr, class Env>
inline constexpr bool is_identity_transform_v =
    find_transform_kind<Domain, Tag, Sndr, Env>() == transform_kind::identity;

template <class Domain, class Tag, class Sndr, class Env>
constexpr bool is_nothrow_transform_step() noexcept {
    if constexpr (find_transform_kind<Domain, Tag, Sndr, Env>() == transform_kind::by_domain) {
        return noexcept(std::declval<Domain>().transform_sender(Tag{}, std::declval<Sndr>(), std::declval<Env const &>()));
    } else {
        return noexcept(
            std::declval<tag_of_t<Sndr>>().transform_sender(Tag{}, std::declval<Sndr>(), std::declval<Env const &>()));
    }
}

// One transformation that changes the sender's type.
template <class Domain, class Tag, class Sndr, class Env>
constexpr decltype(auto) transform_step(Sndr &&sndr, Env const &env) noexcept(
    is_nothrow_transform_step<Domain, Tag, Sndr, Env>()) {
    if constexpr (find_transform_kind<Domain, Tag, Sndr, Env>() == transform_kind::by_domain) {
        return Domain{}.transform_sender(Tag{}, static_cast<Sndr &&>(sndr), env);
    } else {
        return tag_of_t<Sndr>{}.transform_sender(Tag{}, static_cast<Sndr &&>(sndr), env);
    }
}

template <class Domain, class Tag, class Sndr, class Env>
using transform_step_result_t =
    remove_cvref_t<decltype(transform_step<Domain, Tag>(std::declval<Sndr>(), std::declval<Env const &>()))>;

// A value transformation continues in the new sender's completion domain; a start
// transformation stays in the environment's domain.
template <class Tag, class Sndr, class Env>
using next_domain_t =
    std::conditional_t<std::is_same_v<Tag, start_t>, start_domain_of_t<Env>, completion_domain_of_t<Sndr, Env>>;

template <class Domain, class Tag, class Sndr, class Env, bool = is_identity_transform_v<Domain, Tag, Sndr, Env>>
struct is_nothrow_transform_chain {
    static constexpr bool value = true;
};

template <class Domain, class Tag, class Sndr, class Env>
struct is_nothrow_transform_chain<Domain, Tag, Sndr, Env, false> {
    using next_sndr = transform_step_result_t<Domain, Tag, Sndr, Env>;
    static constexpr bool value = is_nothrow_transform_step<Domain, Tag, Sndr, Env>() and
                                  is_nothrow_transform_chain<next_domain_t<Tag, next_sndr, Env>, Tag, next_sndr, Env>::value;
};

// Transforms until the type no longer changes. The sender has already changed type at
// least once, so each step's result is a new prvalue, returned without being moved.
template <class Domain, class Tag, class Sndr, class Env>
constexpr auto transform_until_fixed(Sndr &&sndr, Env const &env) noexcept(
    is_nothrow_transform_chain<Domain, Tag, Sndr, Env>::value) {
    using next_sndr = transform_step_result_t<Domain, Tag, Sndr, Env>;
    using next_domain = next_domain_t<Tag, next_sndr, Env>;
    if constexpr (is_identity_transform_v<next_domain, Tag, next_sndr, Env>) {
        return transform_step<Domain, Tag>(static_cast<Sndr &&>(sndr), env);
    } else {
        return transform_until_fixed<next_domain, Tag>(transform_step<Domain, Tag>(static_cast<Sndr &&>(sndr), env), env);
    }
}

// Returns the sender itself, not a copy, when no transformation applies.
template <class Domain, class Tag, class Sndr, class Env>
constexpr decltype(auto) transform_all(Sndr &&sndr, Env const &env) noexcept(
    is_nothrow_transform_chain<Domain, Tag, Sndr, Env>::value) {
    if constexpr (is_identity_transform_v<Domain, Tag, Sndr, Env>) {
        return static_cast<Sndr &&>(sndr);
    } else {
        return transform_until_fixed<Domain, Tag>(static_cast<Sndr &&>(sndr), env);
    }
}

template <class Sndr, class Env>
using value_transform_result_t =
    decltype(transform_all<completion_domain_of_t<Sndr, Env>, set_value_t>(std::declval<Sndr>(), std::declval<Env const &>()));

// The common case, decided from types alone so that no transformation function needs
// to be instantiated for it.
template <class Sndr, class Env>
inline constexpr bool is_identity_connect_v =
    is_identity_transform_v<completion_domain_of_t<Sndr, Env>, set_value_t, Sndr, Env> and
    is_identity_transform_v<start_domain_of_t<Env>, start_t, Sndr, Env>;

template <class Sndr, class Env, bool = is_identity_connect_v<Sndr, Env>>
inline constexpr bool is_nothrow_transform_v = true;

template <class Sndr, class Env>
inline constexpr bool is_nothrow_transform_v<Sndr, Env, false> =
    is_nothrow_transform_chain<completion_domain_of_t<Sndr, Env>, set_value_t, Sndr, Env>::value and
    is_nothrow_transform_chain<start_domain_of_t<Env>, start_t, value_transform_result_t<Sndr, Env>, Env>::value;

// First the completion domain's set_value transformations, then the starting domain's
// start transformations, for a sender at least one of them changes. A sender produced
// by the first stage is a temporary here, so it is returned by value, never by reference.
template <class Sndr, class Env>
constexpr auto transform_changed_sender(Sndr &&sndr, Env const &env) noexcept(is_nothrow_transform_v<Sndr, Env>) {
    using completion_domain = completion_domain_of_t<Sndr, Env>;
    using start_domain = start_domain_of_t<Env>;
    using value_result = value_transform_result_t<Sndr, Env>;
    if constexpr (std::is_reference_v<value_result>) {
        return transform_all<start_domain, start_t>(static_cast<Sndr &&>(sndr), env);
    } else if constexpr (is_identity_transform_v<start_domain, start_t, value_result, Env>) {
        return transform_until_fixed<completion_domain, set_value_t>(static_cast<Sndr &&>(sndr), env);
    } else {
        return transform_until_fixed<start_domain, start_t>(
            transform_until_fixed<completion_domain, set_value_t>(static_cast<Sndr &&>(sndr), env), env);
    }
}

// transform_sender as used by connect: the sender itself, not a copy, when no
// transformation applies.
template <class Sndr, class Env>
constexpr decltype(auto) transform_sender_for_connect(Sndr &&sndr, Env const &env) noexcept(
    is_nothrow_transform_v<Sndr, Env>) {
    if constexpr (is_identity_connect_v<Sndr, Env>) {
        return static_cast<Sndr &&>(sndr);
    } else {
        return transform_changed_sender(static_cast<Sndr &&>(sndr), env);
    }
}

template <class Sndr, class Env, bool = is_identity_connect_v<Sndr, Env>>
struct connected_sender {
    using type = Sndr;
};

template <class Sndr, class Env>
struct connected_sender<Sndr, Env, false> {
    using type = decltype(transform_changed_sender(std::declval<Sndr>(), std::declval<Env const &>()));
};

// The sender that connect actually connects: Sndr itself unless a domain transforms it.
template <class Sndr, class Env>
using connected_sender_t = typename connected_sender<Sndr, Env>::type;

} // namespace detail

// Applies the domains' transformations to a sender. When none applies, an rvalue sender
// is returned as a new value, as the standard specifies; connect avoids that move.
template <class Sndr, class Env>
constexpr decltype(auto) transform_sender(Sndr &&sndr, Env const &env) noexcept(
    detail::is_nothrow_transform_v<Sndr, Env> and
    (std::is_lvalue_reference_v<Sndr> or std::is_nothrow_constructible_v<detail::remove_cvref_t<Sndr>, Sndr>)) {
    if constexpr (detail::is_identity_connect_v<Sndr, Env>) {
        return static_cast<Sndr>(static_cast<Sndr &&>(sndr));
    } else {
        return detail::transform_changed_sender(static_cast<Sndr &&>(sndr), env);
    }
}

} // namespace lexec
