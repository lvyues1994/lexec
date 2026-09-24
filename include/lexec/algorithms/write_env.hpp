#pragma once

#include <lexec/core/env.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>
#include <lexec/stop_token.hpp>

#include <type_traits>

namespace lexec {

struct write_env_t;

namespace detail {

// The written environment answers first; every query of the receiver's environment,
// forwarding or not, remains visible behind it.
template <class Written, class... Env>
struct write_env_joined {
    using type = env<env_ref<Written>>;
};

template <class Written, class Env>
struct write_env_joined<Written, Env> {
    using type = env<env_ref<Written>, Env>;
};

struct write_env_impls : default_impls {
    template <class Self, class... Env>
    using completions =
        completion_signatures_of_t<child_of_t<Self, 0>, typename write_env_joined<data_of_t<Self>, Env...>::type>;

    template <class Index, class Written, class Rcvr>
    static constexpr auto get_env(Index, Written const &written, Rcvr const &rcvr) noexcept
        -> env<env_ref<Written>, env_of_t<Rcvr const &>> {
        return env<env_ref<Written>, env_of_t<Rcvr const &>>{env_ref<Written>{&written}, lexec::get_env(rcvr)};
    }
};

template <>
struct impls_for<write_env_t> : write_env_impls {};

} // namespace detail

// Connects the sender with an environment that answers queries before the receiver's.
struct write_env_t : detail::data_adaptor<write_env_t> {};

inline constexpr write_env_t write_env{};

// Hides the receiver's stop token: the sender sees a token that can never be stopped.
struct unstoppable_t : sender_adaptor_closure<unstoppable_t> {
    template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sndr &&sndr) const {
        return write_env(static_cast<Sndr &&>(sndr), prop{get_stop_token, never_stop_token{}});
    }
};

inline constexpr unstoppable_t unstoppable{};

} // namespace lexec
