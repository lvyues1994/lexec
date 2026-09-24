#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/framework/basic_sender.hpp>

#include <exception>

namespace lexec {

struct read_env_t;

namespace detail {

// Without an environment there is nothing to read, so the sender is dependent.
template <class Query, class EnvList, class = void>
struct read_env_completions {};

template <class Query, class Env>
struct read_env_completions<Query, type_list<Env>, std::enable_if_t<is_callable_v<Query, Env const &>>> {
    using type = concat_t<completion_signatures<set_value_t(call_result_t<Query, Env const &>)>,
                          eptr_completion_if_t<not is_nothrow_callable_v<Query, Env const &>>>;
};

struct read_env_impls : default_impls {
    template <class Self, class... Env>
    using completions = typename read_env_completions<data_of_t<Self>, type_list<Env...>>::type;

    template <class Data>
    static constexpr inline_attrs<set_value_t> get_attrs(Data const &) noexcept {
        return {};
    }

    template <class Query, class Rcvr>
    static void start(Query &query, Rcvr &rcvr) noexcept {
        if constexpr (is_nothrow_callable_v<Query &, env_of_t<Rcvr &>> or not LEXEC_HAS_EXCEPTIONS) {
            lexec::set_value(static_cast<Rcvr &&>(rcvr), query(lexec::get_env(rcvr)));
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                lexec::set_value(static_cast<Rcvr &&>(rcvr), query(lexec::get_env(rcvr)));
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(rcvr), std::current_exception());
            }
#endif
        }
    }
};

template <>
struct impls_for<read_env_t> : read_env_impls {};

} // namespace detail

// Completes with the value a query returns for the receiver's environment.
struct read_env_t {
    template <class Query>
    constexpr auto operator()(Query query) const noexcept -> detail::basic_sender<read_env_t, Query> {
        return {query, {}};
    }
};

inline constexpr read_env_t read_env{};

} // namespace lexec
