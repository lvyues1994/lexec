#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/manual_variant.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>

#include <exception>
#include <type_traits>
#include <utility>

namespace lexec {

struct let_value_t;
struct let_error_t;
struct let_stopped_t;

namespace detail {

// SCHED-ENV: an environment whose current scheduler is Sch.
template <class Sch>
struct sched_env {
    constexpr Sch query(get_scheduler_t) const noexcept { return sch; }

    template <class S = Sch, std::enable_if_t<has_query_v<S const &, get_domain_t>, int> = 0>
    constexpr auto query(get_domain_t) const noexcept {
        return sch.query(get_domain_t{});
    }

    Sch sch;
};

// let-env: where the second sender starts, which is where the predecessor completed
// with SetTag, as far as the predecessor's attributes tell.
template <class SetTag, class Attrs, class... Env>
constexpr auto make_let_env(Attrs const &attrs, Env const &...rcvr_env) noexcept {
    if constexpr (is_detected_v<completion_scheduler_result_t, SetTag, Attrs, fwd_env_t<Env>...>) {
        using scheduler = completion_scheduler_result_t<SetTag, Attrs, fwd_env_t<Env>...>;
        return sched_env<scheduler>{get_completion_scheduler<SetTag>(attrs, make_fwd_env(rcvr_env)...)};
    } else if constexpr (is_detected_v<completion_domain_type_t, SetTag, Attrs, fwd_env_t<Env>...>) {
        return prop{get_domain, get_completion_domain<SetTag>(attrs, make_fwd_env(rcvr_env)...)};
    } else {
        return env<>{};
    }
}

template <class SetTag, class Attrs, class... Env>
using let_env_t = decltype(make_let_env<SetTag>(std::declval<Attrs const &>(), std::declval<Env const &>()...));

// The second sender's environment: let-env, then the receiver's forwarding queries.
template <class LetEnv, class... Env>
using let_second_env_t = env<env_ref<LetEnv>, fwd_env_t<Env>...>;

// Stands in for the second sender's receiver where only its environment is known.
template <class Env>
struct receiver_archetype {
    using receiver_concept = receiver_t;

    template <class... Vs>
    void set_value(Vs &&...) && noexcept;
    template <class E>
    void set_error(E &&) && noexcept;
    void set_stopped() && noexcept;
    Env get_env() const noexcept;
};

template <class Fn, class Env2, class... Vs>
struct let_bind_traits {
    static_assert(is_callable_v<Fn, std::decay_t<Vs> &...>,
                  "lexec::let_value/let_error/let_stopped: the function cannot be called with lvalues of the "
                  "results the predecessor sender completes with");
    using sender_type = call_result_t<Fn, std::decay_t<Vs> &...>;
    static_assert(is_sender_v<sender_type>, "lexec::let_value/let_error/let_stopped: the function must return a sender");

    static constexpr bool computable = is_sender_in_v<sender_type, Env2>;
};

// Storing the results, calling the function, and connecting the second sender.
template <class Fn, class Env2, class... Vs>
constexpr bool is_nothrow_let_bind() noexcept {
    if constexpr (not LEXEC_HAS_EXCEPTIONS) {
        return true;
    } else {
        using sender_type = typename let_bind_traits<Fn, Env2, Vs...>::sender_type;
        return (std::is_nothrow_constructible_v<std::decay_t<Vs>, Vs> and ...) and
               is_nothrow_callable_v<Fn, std::decay_t<Vs> &...> and
               noexcept(lexec::connect(std::declval<sender_type>(), std::declval<receiver_archetype<Env2>>()));
    }
}

template <class Fn, class Env2, class... Vs>
struct let_bind_completions {
    using type = concat_t<completion_signatures_of_t<typename let_bind_traits<Fn, Env2, Vs...>::sender_type, Env2>,
                          eptr_completion_if_t<not is_nothrow_let_bind<Fn, Env2, Vs...>()>>;
};

template <class Fn, class Env2>
struct let_bind_computable {
    template <class... Vs>
    using check = std::bool_constant<let_bind_traits<Fn, Env2, Vs...>::computable>;
};

template <bool Computable, class SetTag, class Fn, class ChildSigs, class Env2>
struct let_completions_select {};

template <class Fn, class ChildSigs, class Env2>
struct let_completions_select<true, set_value_t, Fn, ChildSigs, Env2> {
    template <class... Vs>
    using on_value = typename let_bind_completions<Fn, Env2, Vs...>::type;
    using type = transform_completion_signatures<ChildSigs, completion_signatures<>, on_value>;
};

template <class Fn, class ChildSigs, class Env2>
struct let_completions_select<true, set_error_t, Fn, ChildSigs, Env2> {
    template <class E>
    using on_error = typename let_bind_completions<Fn, Env2, E>::type;
    using type = transform_completion_signatures<ChildSigs, completion_signatures<>, default_set_value, on_error>;
};

template <class Fn, class ChildSigs, class Env2>
struct let_completions_select<true, set_stopped_t, Fn, ChildSigs, Env2> {
    using on_stopped = typename std::conditional_t<count_signatures_v<set_stopped_t, ChildSigs> != 0,
                                                   let_bind_completions<Fn, Env2>,
                                                   type_identity<completion_signatures<>>>::type;
    using type = transform_completion_signatures<ChildSigs, completion_signatures<>, default_set_value,
                                                 default_set_error, on_stopped>;
};

// No `select` member when the predecessor's completions are unknown, and no
// `select::type` when a second sender's completions are: either way the let sender is
// dependent rather than ill-formed.
template <class SetTag, class Fn, class Child, class EnvList, class = void>
struct let_completions {};

template <class SetTag, class Fn, class Child, class... Env>
struct let_completions<SetTag, Fn, Child, type_list<Env...>,
                       std::void_t<completion_signatures_of_t<Child, fwd_env_t<Env>...>>> {
    using child_sigs = completion_signatures_of_t<Child, fwd_env_t<Env>...>;
    using second_env = let_second_env_t<let_env_t<SetTag, env_of_t<Child>, Env...>, Env...>;
    static constexpr bool computable =
        gather_signatures_t<SetTag, child_sigs, let_bind_computable<Fn, second_env>::template check, all_of_t>::value;
    using select = let_completions_select<computable, SetTag, Fn, child_sigs, second_env>;
};

// Receives the second sender's completions and passes them to the let's receiver.
template <class Rcvr, class LetEnv>
struct let_receiver {
    using receiver_concept = receiver_t;
    using env_type = let_second_env_t<LetEnv, env_of_t<Rcvr>>;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        lexec::set_value(static_cast<Rcvr &&>(*rcvr), static_cast<Vs &&>(vs)...);
    }

    template <class E>
    void set_error(E &&e) && noexcept {
        lexec::set_error(static_cast<Rcvr &&>(*rcvr), static_cast<E &&>(e));
    }

    void set_stopped() && noexcept { lexec::set_stopped(static_cast<Rcvr &&>(*rcvr)); }

    env_type get_env() const noexcept { return env_type{env_ref<LetEnv>{let_env}, make_fwd_env(lexec::get_env(*rcvr))}; }

    Rcvr *rcvr;
    LetEnv const *let_env;
};

template <class SetTag, class Sndr, class Rcvr>
struct let_state;

// Receives the predecessor's completions on behalf of the let state. Its parameters name
// each type once: taking the state type and the receiver separately would repeat the
// receiver, doubling the type's size with every enclosing let.
template <class SetTag, class Sndr, class Rcvr>
struct let_child_receiver {
    using receiver_concept = receiver_t;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        state->complete(set_value_t{}, static_cast<Vs &&>(vs)...);
    }

    template <class E>
    void set_error(E &&e) && noexcept {
        state->complete(set_error_t{}, static_cast<E &&>(e));
    }

    void set_stopped() && noexcept { state->complete(set_stopped_t{}); }

    fwd_env_t<env_of_t<Rcvr>> get_env() const noexcept { return make_fwd_env(lexec::get_env(*state->rcvr)); }

    let_state<SetTag, Sndr, Rcvr> *state;
};

template <class... Ts>
using decayed_args_t = tuple<std::decay_t<Ts>...>;

template <class Fn, class Args, class SecondRcvr>
struct second_operation_of;

template <class Fn, class Indices, class... Ts, class SecondRcvr>
struct second_operation_of<Fn, tuple_impl<Indices, Ts...>, SecondRcvr> {
    using type = connect_result_t<call_result_t<Fn, Ts &...>, SecondRcvr>;
};

template <class Fn, class ArgsList, class SecondRcvr>
struct second_operations;

template <class Fn, class... Args, class SecondRcvr>
struct second_operations<Fn, type_list<Args...>, SecondRcvr> {
    using type = type_list<typename second_operation_of<Fn, Args, SecondRcvr>::type...>;
};

// The predecessor's operation and the second operation share one variant: the
// predecessor's is destroyed once its results are stored, before the second starts.
template <class SetTag, class Sndr, class Rcvr>
struct let_state {
    using fn_type = data_of_t<Sndr>;
    using child_type = child_of_t<Sndr, 0>;
    using rcvr_env = env_of_t<Rcvr>;
    using let_env_type = let_env_t<SetTag, env_of_t<child_type>, rcvr_env>;
    using second_env = let_second_env_t<let_env_type, rcvr_env>;
    using child_receiver = let_child_receiver<SetTag, Sndr, Rcvr>;
    using second_receiver = let_receiver<Rcvr, let_env_type>;
    using child_op = connect_result_t<child_type, child_receiver>;
    using args_list = unique_t<
        gather_signatures_t<SetTag, completion_signatures_of_t<child_type, fwd_env_t<rcvr_env>>, decayed_args_t, type_list>>;
    using args_variant = rename_t<args_list, manual_variant>;
    using ops_variant = rename_t<
        unique_t<concat_t<type_list<child_op>, typename second_operations<fn_type, args_list, second_receiver>::type>>,
        manual_variant>;

    let_state(Sndr &&sndr, Rcvr &rcvr_) noexcept(
        std::is_nothrow_constructible_v<fn_type, member_like_t<Sndr, fn_type>> and
        noexcept(lexec::connect(std::declval<child_type>(), std::declval<child_receiver>())))
        : fn(static_cast<Sndr &&>(sndr).data),
          let_env(make_let_env<SetTag>(lexec::get_env(detail::get<0>(sndr.children)), lexec::get_env(rcvr_))),
          rcvr(&rcvr_) {
        ops.template emplace_with<child_op>(
            [&] { return lexec::connect(detail::get<0>(static_cast<Sndr &&>(sndr).children), child_receiver{this}); });
    }

    let_state(let_state &&) = delete;

    template <class Tag, class... Args>
    void complete(Tag, Args &&...args) noexcept {
        if constexpr (not std::is_same_v<Tag, SetTag>) {
            Tag{}(static_cast<Rcvr &&>(*rcvr), static_cast<Args &&>(args)...);
        } else if constexpr (is_nothrow_let_bind<fn_type, second_env, Args...>()) {
            bind(static_cast<Args &&>(args)...);
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                bind(static_cast<Args &&>(args)...);
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(*rcvr), std::current_exception());
            }
#endif
        }
    }

    // The results may live in the predecessor's operation, so they are moved into this
    // state before that operation is destroyed.
    template <class... Args>
    void bind(Args &&...args) {
        using args_type = decayed_args_t<Args...>;
        using second_op = typename second_operation_of<fn_type, args_type, second_receiver>::type;
        auto &stored =
            stored_args.template emplace_with<args_type>([&] { return args_type{{static_cast<Args &&>(args)}...}; });
        auto &op = ops.template emplace_with<second_op>([&] {
            return lexec::connect(stored.apply(static_cast<fn_type &&>(fn)), second_receiver{rcvr, &let_env});
        });
        lexec::start(op);
    }

    LEXEC_NO_UNIQUE_ADDRESS fn_type fn;
    LEXEC_NO_UNIQUE_ADDRESS let_env_type let_env;
    Rcvr *rcvr;
    args_variant stored_args;
    ops_variant ops;
};

template <class SetTag>
struct let_impls : default_impls {
    static constexpr bool connects_children = false;

    template <class Self, class... Env>
    using completions =
        typename let_completions<SetTag, data_of_t<Self>, child_of_t<Self, 0>, type_list<Env...>>::select::type;

    // Where the let completes depends on which second sender runs, so it claims no
    // completion scheduler or domain.
    template <class Data, class Child>
    static constexpr env<> get_attrs(Data const &, Child const &) noexcept {
        return env<>{};
    }

    template <class Sndr, class Rcvr>
    static auto get_state(Sndr &&sndr, Rcvr &rcvr) noexcept(
        std::is_nothrow_constructible_v<let_state<SetTag, Sndr, Rcvr>, Sndr, Rcvr &>) -> let_state<SetTag, Sndr, Rcvr> {
        return let_state<SetTag, Sndr, Rcvr>{static_cast<Sndr &&>(sndr), rcvr};
    }

    template <class State, class Rcvr>
    static void start(State &state, Rcvr &) noexcept {
        lexec::start(state.ops.template get<typename State::child_op>());
    }
};

template <>
struct impls_for<let_value_t> : let_impls<set_value_t> {};

template <>
struct impls_for<let_error_t> : let_impls<set_error_t> {};

template <>
struct impls_for<let_stopped_t> : let_impls<set_stopped_t> {};

} // namespace detail

// Calls the function with lvalues of the predecessor's results for that channel and
// continues with the sender it returns; the results stay alive until it completes.
struct let_value_t : detail::data_adaptor<let_value_t> {};
struct let_error_t : detail::data_adaptor<let_error_t> {};
struct let_stopped_t : detail::data_adaptor<let_stopped_t> {};

inline constexpr let_value_t let_value{};
inline constexpr let_error_t let_error{};
inline constexpr let_stopped_t let_stopped{};

} // namespace lexec
