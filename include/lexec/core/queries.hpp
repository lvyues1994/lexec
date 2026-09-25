#pragma once

#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/stop_token.hpp>

#include <type_traits>
#include <utility>

namespace lexec {

struct scheduler_t {};

namespace detail {

template <class Sch>
using scheduler_concept_of_t = typename Sch::scheduler_concept;

// Only the opt-in tag: queries cannot depend on the full scheduler concept, which
// itself depends on senders.
template <class Sch, bool = is_detected_v<scheduler_concept_of_t, Sch>>
inline constexpr bool enable_scheduler_v = false;

template <class Sch>
inline constexpr bool enable_scheduler_v<Sch, true> = std::is_base_of_v<scheduler_t, typename Sch::scheduler_concept>;

template <class Q, class Tag, class... Args>
using query_with_args_t = decltype(std::declval<Q const &>().query(Tag{}, std::declval<Args>()...));

template <class Q, class Tag>
using query_without_args_t = decltype(std::declval<Q const &>().query(Tag{}));

// TRY-QUERY: q.query(tag, args...) when valid, otherwise q.query(tag).
template <class Q, class Tag, class... Args>
inline constexpr bool has_try_query_v =
    is_detected_v<query_with_args_t, Q, Tag, Args...> or is_detected_v<query_without_args_t, Q, Tag>;

template <class Tag, class Q, class... Args>
constexpr decltype(auto) try_query(Q const &q, [[maybe_unused]] Args const &...args) noexcept {
    if constexpr (is_detected_v<query_with_args_t, Q, Tag, Args const &...>) {
        static_assert(noexcept(q.query(Tag{}, args...)), "environment queries must be noexcept");
        return q.query(Tag{}, args...);
    } else {
        static_assert(noexcept(q.query(Tag{})), "environment queries must be noexcept");
        return q.query(Tag{});
    }
}

template <class Tag, class Q, class... Args>
using try_query_result_t = remove_cvref_t<decltype(try_query<Tag>(std::declval<Q const &>(), std::declval<Args const &>()...))>;

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
struct get_delegation_scheduler_t : detail::required_query<get_delegation_scheduler_t> {};

struct get_scheduler_t;
struct get_domain_t;

namespace detail {

// HIDE-SCHED: the environment without its scheduler and domain, so that asking the
// current scheduler about itself cannot recurse back into the environment.
template <class Env>
struct hide_sched_env {
    template <class Query, class... Args,
              std::enable_if_t<not is_one_of_v<Query, get_scheduler_t, get_domain_t> and
                                   has_query_v<Env const &, Query, Args...>,
                               int> = 0>
    constexpr decltype(auto) query(Query query_tag, Args &&...args) const noexcept {
        return target->query(query_tag, static_cast<Args &&>(args)...);
    }

    Env const *target;
};

template <class Sch, class... Envs>
constexpr auto recurse_completion_scheduler(Sch const &sch, Envs const &...envs) noexcept;

} // namespace detail

// Asks a sender's attributes where it completes. The optional environment is the
// receiver's, for senders that complete wherever they are started. A scheduler asked
// with an environment reports itself.
template <class Tag>
struct get_completion_scheduler_t {
    static_assert(detail::is_completion_tag_v<Tag>, "get_completion_scheduler requires a completion tag");

    template <class Q, class... Envs,
              std::enable_if_t<detail::has_try_query_v<Q, get_completion_scheduler_t, Envs const &...> or
                                   (detail::enable_scheduler_v<Q> and sizeof...(Envs) != 0),
                               int> = 0>
    constexpr auto operator()(Q const &q, [[maybe_unused]] Envs const &...envs) const noexcept {
        static_assert(sizeof...(Envs) <= 1, "get_completion_scheduler accepts at most one environment");
        if constexpr (detail::has_try_query_v<Q, get_completion_scheduler_t, Envs const &...>) {
            return detail::recurse_completion_scheduler(detail::try_query<get_completion_scheduler_t>(q, envs...),
                                                        envs...);
        } else {
            return q;
        }
    }

    static constexpr bool query(forwarding_query_t) noexcept { return true; }
};

namespace detail {

// RECURSE-QUERY: a scheduler may itself report the scheduler it completes on, such as
// an inline scheduler reporting the one it was started on; follow that to the end.
template <class Sch, class... Envs>
constexpr auto recurse_completion_scheduler(Sch const &sch, [[maybe_unused]] Envs const &...envs) noexcept {
    static_assert(enable_scheduler_v<Sch>, "get_completion_scheduler must return a scheduler");
    using query = get_completion_scheduler_t<set_value_t>;
    if constexpr (not has_try_query_v<Sch, query, Envs const &...>) {
        return sch;
    } else if constexpr (std::is_same_v<try_query_result_t<query, Sch, Envs...>, Sch>) {
        return try_query<query>(sch, envs...);
    } else {
        return recurse_completion_scheduler(try_query<query>(sch, envs...), envs...);
    }
}

} // namespace detail

// The environment's scheduler, or rather the scheduler work started on it completes on.
struct get_scheduler_t {
    template <class Env, std::enable_if_t<detail::has_query_v<Env const &, get_scheduler_t>, int> = 0>
    constexpr auto operator()(Env const &env) const noexcept {
        static_assert(noexcept(env.query(get_scheduler_t{})), "environment queries must be noexcept");
        return get_completion_scheduler_t<set_value_t>{}(env.query(get_scheduler_t{}), detail::hide_sched_env<Env>{&env});
    }

    static constexpr bool query(forwarding_query_t) noexcept { return true; }
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
