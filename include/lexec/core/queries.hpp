#pragma once

#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/stop_token.hpp>

namespace lexec {

namespace detail {

// A forwarding query that must be answered by the environment; there is no default.
template <class Derived>
struct required_query {
    template <class Env, class = std::enable_if_t<has_query_v<Env const &, Derived>>>
    constexpr decltype(auto) operator()(Env const &env) const noexcept {
        static_assert(noexcept(env.query(Derived{})), "environment queries must be noexcept");
        return env.query(Derived{});
    }

    static constexpr bool query(forwarding_query_t) noexcept { return true; }
};

} // namespace detail

struct get_stop_token_t {
    template <class Env>
    constexpr decltype(auto) operator()(Env const &env) const noexcept {
        if constexpr (detail::has_query_v<Env const &, get_stop_token_t>) {
            static_assert(noexcept(env.query(get_stop_token_t{})), "environment queries must be noexcept");
            static_assert(is_stoppable_token_v<detail::remove_cvref_t<decltype(env.query(get_stop_token_t{}))>>,
                          "get_stop_token must return a stoppable token");
            return env.query(get_stop_token_t{});
        } else {
            return never_stop_token{};
        }
    }

    static constexpr bool query(forwarding_query_t) noexcept { return true; }
};

struct get_allocator_t : detail::required_query<get_allocator_t> {};
struct get_scheduler_t : detail::required_query<get_scheduler_t> {};
struct get_delegation_scheduler_t : detail::required_query<get_delegation_scheduler_t> {};

template <class Tag>
struct get_completion_scheduler_t : detail::required_query<get_completion_scheduler_t<Tag>> {
    static_assert(detail::is_completion_tag_v<Tag>, "get_completion_scheduler requires a completion tag");
};

inline constexpr get_stop_token_t get_stop_token{};
inline constexpr get_allocator_t get_allocator{};
inline constexpr get_scheduler_t get_scheduler{};
inline constexpr get_delegation_scheduler_t get_delegation_scheduler{};

template <class Tag>
inline constexpr get_completion_scheduler_t<Tag> get_completion_scheduler{};

template <class Env>
using stop_token_of_t = detail::remove_cvref_t<decltype(get_stop_token(std::declval<Env>()))>;

} // namespace lexec
