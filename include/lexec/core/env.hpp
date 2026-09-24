#pragma once

#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace lexec {

struct forwarding_query_t {
    template <class Query>
    constexpr bool operator()(Query const &) const noexcept;
};

inline constexpr forwarding_query_t forwarding_query{};

namespace detail {

template <class Env, class Query, class... Args>
using query_result_t = decltype(std::declval<Env>().query(std::declval<Query>(), std::declval<Args>()...));

template <class Env, class Query, class... Args>
inline constexpr bool has_query_v = is_detected_v<query_result_t, Env, Query, Args...>;

template <class Query>
using forwarding_query_member_t = std::enable_if_t<Query{}.query(forwarding_query_t{}), bool>;

template <class Query>
constexpr bool is_forwarding_query() noexcept {
    if constexpr (is_detected_v<forwarding_query_member_t, Query>) {
        return true;
    } else {
        return std::is_base_of_v<forwarding_query_t, Query>;
    }
}

} // namespace detail

template <class Query>
constexpr bool forwarding_query_t::operator()(Query const &) const noexcept {
    return detail::is_forwarding_query<Query>();
}

template <class Query>
inline constexpr bool is_forwarding_query_v = detail::is_forwarding_query<Query>();

template <class Query, class Value>
struct prop {
    constexpr prop(Query, Value value_) noexcept(std::is_nothrow_move_constructible_v<Value>)
        : value(static_cast<Value &&>(value_)) {}

    constexpr Value const &query(Query) const noexcept { return value; }

    Value value;
};

template <class Query, class Value>
prop(Query, Value) -> prop<Query, Value>;

namespace detail {

template <class Query, class... Envs>
constexpr std::size_t first_env_with_query() noexcept {
    constexpr bool found[] = {has_query_v<Envs const &, Query>..., true};
    auto index = std::size_t{0};
    while (not found[index]) {
        ++index;
    }
    return index;
}

template <class Query, class... Envs>
inline constexpr std::size_t first_env_with_query_v = first_env_with_query<Query, Envs...>();

template <class Query, class... Envs>
using enable_env_query_t = std::enable_if_t<(first_env_with_query_v<Query, Envs...> < sizeof...(Envs))>;

} // namespace detail

// Joins environments; a query is answered by the first environment that supports it.
template <class... Envs>
struct env {
    constexpr explicit env(Envs... envs_) noexcept((std::is_nothrow_move_constructible_v<Envs> and ...))
        : envs{{static_cast<Envs &&>(envs_)}...} {}

    template <class Query, class = detail::enable_env_query_t<Query, Envs...>>
    constexpr decltype(auto) query(Query query_tag) const noexcept {
        return detail::get<detail::first_env_with_query_v<Query, Envs...>>(envs).query(query_tag);
    }

    detail::tuple<Envs...> envs;
};

template <>
struct env<> {};

template <class... Envs>
env(Envs...) -> env<Envs...>;

namespace detail {

template <class T>
using get_env_member_t = decltype(std::declval<T const &>().get_env());

} // namespace detail

struct get_env_t {
    template <class T>
    constexpr decltype(auto) operator()(T const &obj) const noexcept {
        if constexpr (detail::is_detected_v<detail::get_env_member_t, T>) {
            static_assert(noexcept(obj.get_env()), "get_env member must be noexcept");
            return obj.get_env();
        } else {
            return env<>{};
        }
    }
};

inline constexpr get_env_t get_env{};

template <class T>
using env_of_t = decltype(get_env(std::declval<T>()));

namespace detail {

// Exposes only the forwarding queries of the wrapped environment.
template <class Env>
struct fwd_env {
    Env base;

    template <class Query, class... Args,
              class = std::enable_if_t<is_forwarding_query_v<Query> and has_query_v<Env const &, Query, Args...>>>
    constexpr decltype(auto) query(Query query_tag, Args &&...args) const noexcept {
        return base.query(query_tag, static_cast<Args &&>(args)...);
    }
};

template <class Env>
struct fwd_env_of {
    using type = fwd_env<Env>;
};

template <class Env>
struct fwd_env_of<fwd_env<Env>> {
    using type = fwd_env<Env>;
};

template <class Env>
using fwd_env_t = typename fwd_env_of<remove_cvref_t<Env>>::type;

template <class Env>
constexpr fwd_env_t<Env> make_fwd_env(Env &&base) noexcept {
    return fwd_env_t<Env>{static_cast<Env &&>(base)};
}

} // namespace detail

} // namespace lexec
