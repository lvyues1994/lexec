#pragma once

#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/detail/meta.hpp>

#include <type_traits>
#include <utility>

namespace lexec {

// The algorithm tag of a sender built by the sender framework.
template <class Sndr>
using tag_of_t = typename detail::remove_cvref_t<Sndr>::tag_type;

namespace detail {

template <class Tag, class Sndr, class... Env>
using tag_transform_result_t =
    decltype(std::declval<tag_of_t<Sndr>>().transform_sender(Tag{}, std::declval<Sndr>(), std::declval<Env const &>()...));

template <class Tag, class Sndr, class... Env>
constexpr bool is_nothrow_default_transform() noexcept {
    if constexpr (is_detected_v<tag_transform_result_t, Tag, Sndr, Env...>) {
        return noexcept(
            std::declval<tag_of_t<Sndr>>().transform_sender(Tag{}, std::declval<Sndr>(), std::declval<Env const &>()...));
    } else {
        return std::is_lvalue_reference_v<Sndr> or std::is_nothrow_constructible_v<remove_cvref_t<Sndr>, Sndr>;
    }
}

} // namespace detail

// Applies the transformation the sender's own algorithm tag defines for Tag, if any.
struct default_domain {
    template <class Tag, class Sndr, class... Env>
    static constexpr decltype(auto) transform_sender(Tag, Sndr &&sndr, Env const &...env) noexcept(
        detail::is_nothrow_default_transform<Tag, Sndr, Env...>()) {
        static_assert(sizeof...(Env) <= 1, "transform_sender accepts at most one environment");
        if constexpr (detail::is_detected_v<detail::tag_transform_result_t, Tag, Sndr, Env...>) {
            return tag_of_t<Sndr>{}.transform_sender(Tag{}, static_cast<Sndr &&>(sndr), env...);
        } else {
            // A prvalue for an rvalue sender, so the result never refers to a temporary.
            return static_cast<Sndr>(static_cast<Sndr &&>(sndr));
        }
    }
};

struct get_domain_t;

template <class Tag = void>
struct get_completion_domain_t;

namespace detail {

template <class>
struct domain_not_found {};

// HIDE-SCHED: the environment without its scheduler and domain, so that asking the
// current scheduler for its domain cannot recurse into get_domain.
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

template <class Tag, class Q, class... Envs>
using completion_scheduler_result_t =
    remove_cvref_t<decltype(get_completion_scheduler_t<Tag>{}(std::declval<Q const &>(), std::declval<Envs const &>()...))>;

template <class Tag, class Attrs, class... Envs>
constexpr auto find_completion_domain() noexcept;

template <class Tag, class Attrs, class... Envs>
using completion_domain_type_t = typename decltype(find_completion_domain<Tag, Attrs, Envs...>())::type;

// The domain of the scheduler a sender completes on, when the attributes do not name
// a domain directly.
template <class Tag, class Attrs, class... Envs>
constexpr auto find_scheduler_domain() noexcept {
    if constexpr (is_detected_v<completion_scheduler_result_t, Tag, Attrs, Envs...>) {
        using scheduler = completion_scheduler_result_t<Tag, Attrs, Envs...>;
        if constexpr (has_try_query_v<scheduler, get_completion_domain_t<set_value_t>, Envs const &...>) {
            return type_identity<try_query_result_t<get_completion_domain_t<set_value_t>, scheduler, Envs...>>{};
        } else {
            return domain_not_found<Tag>{};
        }
    } else {
        return domain_not_found<Tag>{};
    }
}

template <class Tag, class Attrs, class... Envs>
constexpr auto find_completion_domain() noexcept {
    if constexpr (has_try_query_v<Attrs, get_completion_domain_t<Tag>, Envs const &...>) {
        return type_identity<try_query_result_t<get_completion_domain_t<Tag>, Attrs, Envs...>>{};
    } else if constexpr (std::is_void_v<Tag>) {
        return find_completion_domain<set_value_t, Attrs, Envs...>();
    } else if constexpr (not std::is_same_v<decltype(find_scheduler_domain<Tag, Attrs, Envs...>()), domain_not_found<Tag>>) {
        return find_scheduler_domain<Tag, Attrs, Envs...>();
    } else if constexpr (enable_scheduler_v<Attrs> and sizeof...(Envs) != 0) {
        return type_identity<default_domain>{};
    } else {
        return domain_not_found<Tag>{};
    }
}

template <class Env>
using env_domain_query_t = remove_cvref_t<decltype(std::declval<Env const &>().query(std::declval<get_domain_t>()))>;

template <class Env>
using env_scheduler_t = remove_cvref_t<decltype(get_scheduler(std::declval<Env const &>()))>;

template <class Env>
using scheduler_domain_t = completion_domain_type_t<set_value_t, env_scheduler_t<Env>, hide_sched_env<Env>>;

template <class Env>
constexpr auto find_env_domain() noexcept {
    if constexpr (is_detected_v<env_domain_query_t, Env>) {
        return type_identity<env_domain_query_t<Env>>{};
    } else if constexpr (is_detected_v<scheduler_domain_t, Env>) {
        return type_identity<scheduler_domain_t<Env>>{};
    } else {
        return type_identity<default_domain>{};
    }
}

} // namespace detail

// The domain where operations started with this environment begin: the environment's
// own domain, else the domain of its scheduler, else default_domain.
struct get_domain_t {
    template <class Env>
    constexpr auto operator()(Env const &) const noexcept {
        return typename decltype(detail::find_env_domain<Env>())::type{};
    }

    static constexpr bool query(forwarding_query_t) noexcept { return true; }
};

// The domain where a sender completes with Tag; get_completion_domain<> asks for the
// domain used to dispatch algorithm customizations.
template <class Tag>
struct get_completion_domain_t {
    static_assert(std::is_void_v<Tag> or detail::is_completion_tag_v<Tag>,
                  "get_completion_domain requires a completion tag or no tag");

    template <class Attrs, class... Envs, class Domain = detail::completion_domain_type_t<Tag, Attrs, Envs...>>
    constexpr Domain operator()(Attrs const &, Envs const &...) const noexcept {
        static_assert(sizeof...(Envs) <= 1, "get_completion_domain accepts at most one environment");
        return Domain{};
    }

    static constexpr bool query(forwarding_query_t) noexcept { return true; }
};

inline constexpr get_domain_t get_domain{};

template <class Tag = void>
inline constexpr get_completion_domain_t<Tag> get_completion_domain{};

namespace detail {

// Attributes of a sender that completes, with one of Tags, wherever it is started.
template <class... Tags>
struct inline_attrs {
    template <class Tag, class Env,
              std::enable_if_t<is_one_of_v<Tag, Tags...> and std::is_invocable_v<get_scheduler_t, Env const &>, int> = 0>
    constexpr auto query(get_completion_scheduler_t<Tag>, Env const &env) const noexcept {
        return get_scheduler(env);
    }

    template <class Tag, class Env, std::enable_if_t<is_one_of_v<Tag, Tags...>, int> = 0>
    constexpr auto query(get_completion_domain_t<Tag>, Env const &env) const noexcept {
        return get_domain(env);
    }
};

} // namespace detail

} // namespace lexec
