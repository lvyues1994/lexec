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
    static constexpr decltype(auto) transform_sender(Tag, Sndr &&sndr, [[maybe_unused]] Env const &...env) noexcept(
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

namespace detail {

template <class Domain, class Tag, class Sndr, class Env>
using own_transform_result_t =
    decltype(std::declval<Domain>().transform_sender(Tag{}, std::declval<Sndr>(), std::declval<Env const &>()));

template <class Domain, class Tag, class Sndr, class Env, bool = is_detected_v<own_transform_result_t, Domain, Tag, Sndr, Env>>
inline constexpr bool agrees_with_default_v = true;

template <class Domain, class Tag, class Sndr, class Env>
inline constexpr bool agrees_with_default_v<Domain, Tag, Sndr, Env, true> =
    std::is_same_v<remove_cvref_t<own_transform_result_t<Domain, Tag, Sndr, Env>>,
                   remove_cvref_t<own_transform_result_t<default_domain, Tag, Sndr, Env>>>;

} // namespace detail

// The domain of a sender that may complete in any of several domains: it transforms as
// default_domain does, which each of those domains must agree with.
template <class... Domains>
struct indeterminate_domain {
    template <class Tag, class Sndr, class Env>
    static constexpr decltype(auto) transform_sender(Tag, Sndr &&sndr, Env const &env) noexcept(
        detail::is_nothrow_default_transform<Tag, Sndr, Env>()) {
        static_assert((detail::agrees_with_default_v<Domains, Tag, Sndr, Env> and ...),
                      "a sender that may complete in several domains is transformed differently by one of them");
        return default_domain::transform_sender(Tag{}, static_cast<Sndr &&>(sndr), env);
    }
};

namespace detail {

template <class Domain>
struct domain_components {
    using type = type_list<Domain>;
};

template <class... Domains>
struct domain_components<indeterminate_domain<Domains...>> {
    using type = type_list<Domains...>;
};

template <class Components>
struct common_domain_of {
    using type = rename_t<Components, indeterminate_domain>;
};

template <class Domain>
struct common_domain_of<type_list<Domain>> {
    using type = Domain;
};

} // namespace detail

// COMMON-DOMAIN: the one domain all of Domains are, or an indeterminate_domain of them.
template <class... Domains>
using common_domain_t = typename detail::common_domain_of<
    detail::unique_t<detail::concat_t<detail::type_list<>, typename detail::domain_components<Domains>::type...>>>::type;

struct get_domain_t;

template <class Tag = void>
struct get_completion_domain_t;

namespace detail {

template <class>
struct domain_not_found {};

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

// COMPL-DOMAIN: where a sender with these attributes completes with Tag, or, when its
// attributes cannot tell, no information.
template <class Attrs, class Tag, class Env, bool = std::is_invocable_v<get_completion_domain_t<Tag>, Attrs, Env>>
struct completion_domain_or_unknown {
    using type = indeterminate_domain<>;
};

template <class Attrs, class Tag, class Env>
struct completion_domain_or_unknown<Attrs, Tag, Env, true> {
    using type = remove_cvref_t<std::invoke_result_t<get_completion_domain_t<Tag>, Attrs, Env>>;
};

// No domain information at all dispatches like default_domain, which it then is.
template <class Domain>
using known_domain_t = std::conditional_t<std::is_same_v<Domain, indeterminate_domain<>>, default_domain, Domain>;

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
