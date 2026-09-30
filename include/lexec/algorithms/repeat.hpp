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
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>
#include <lexec/schedulers/trampoline_scheduler.hpp>
#include <lexec/stop_token.hpp>

#include <cstddef>
#include <exception>
#include <type_traits>

namespace lexec {

struct repeat_until_t;
struct repeat_t;
struct repeat_n_t;

namespace detail {

template <class B>
inline constexpr bool always_done_v = std::is_same_v<std::decay_t<B>, std::true_type>;

template <class B>
inline constexpr bool never_done_v = std::is_same_v<std::decay_t<B>, std::false_type>;

// A value that does not convert is reported by until_value_completions' static_assert.
template <class B, bool = std::is_convertible_v<B, bool>>
inline constexpr bool nothrow_to_bool_v = true;

template <class B>
inline constexpr bool nothrow_to_bool_v<B, true> = noexcept(static_cast<bool>(std::declval<B>()));

// repeat_until's child ends the loop by completing with true; std::false_type never does.
template <class... Bs>
struct until_value_completions {
    static_assert(dependent_false<Bs...>,
                  "lexec::repeat_until: the sender must complete with exactly one value, convertible to bool");
    using type = completion_signatures<>;
};

template <class B>
struct until_value_completions<B> {
    static_assert(std::is_convertible_v<B, bool>, "lexec::repeat_until: the sender's value must convert to bool");
    using type = std::conditional_t<never_done_v<B>, completion_signatures<>,
                                    concat_t<completion_signatures<set_value_t()>, eptr_completion_if_t<not nothrow_to_bool_v<B>>>>;
};

template <class... Vs>
struct void_value_completions {
    static_assert(sizeof...(Vs) == 0,
                  "lexec::repeat and lexec::repeat_n: the sender must complete with set_value() and no values");
    using type = completion_signatures<>;
};

// How an iteration's value completion bears on the loop's own completions.
template <class Tag>
struct loop_policy;

template <>
struct loop_policy<repeat_until_t> {
    template <class... Vs>
    using value_completions = typename until_value_completions<Vs...>::type;
    using extra_completions = completion_signatures<>;
};

template <>
struct loop_policy<repeat_t> {
    template <class... Vs>
    using value_completions = typename void_value_completions<Vs...>::type;
    using extra_completions = completion_signatures<>;
};

// Its count running out, or being zero from the start, completes it with set_value().
template <>
struct loop_policy<repeat_n_t> {
    template <class... Vs>
    using value_completions = typename void_value_completions<Vs...>::type;
    using extra_completions = completion_signatures<set_value_t()>;
};

// Iterations after the first connect inside the previous one's completion, where an
// exception can only become set_error.
template <class Child, class Env>
inline constexpr bool nothrow_reconnect_v =
    noexcept(lexec::connect(std::declval<Child &>(), std::declval<receiver_archetype<fwd_env_t<Env>>>()));

// Without an environment, whether the receiver can ask to stop is unknown, so the loop
// is dependent; so it is when its child's completions are unknown.
template <class Tag, class Child, class EnvList, class = void>
struct loop_completions {};

template <class Tag, class Child, class Env>
struct loop_completions<Tag, Child, type_list<Env>, std::void_t<completion_signatures_of_t<Child &, fwd_env_t<Env>>>> {
    using stop_check_completions = std::conditional_t<is_unstoppable_token_v<stop_token_of_t<Env>>,
                                                      completion_signatures<>, completion_signatures<set_stopped_t()>>;
    using type = transform_completion_signatures<
        completion_signatures_of_t<Child &, fwd_env_t<Env>>,
        concat_t<typename loop_policy<Tag>::extra_completions, stop_check_completions,
                 eptr_completion_if_t<not nothrow_reconnect_v<Child, Env>>>,
        loop_policy<Tag>::template value_completions>;
};

template <class ChildAttrs, class Tag, class Env>
using child_completion_domain_t = typename completion_domain_or_unknown<ChildAttrs, Tag, fwd_env_t<Env>>::type;

template <class Env>
using start_domain_t = decltype(get_domain(std::declval<Env const &>()));

// Where the loop can complete with CTag. Iterations send every completion; the value
// completion that ends one iteration also sends the next one's failure to connect and
// its stop check; the first stop check, and repeat_n's zero count, happen where the loop
// was started.
template <class Tag, class CTag, class ChildAttrs, class Env>
struct loop_domain_sources;

template <class Tag, class ChildAttrs, class Env>
struct loop_domain_sources<Tag, set_value_t, ChildAttrs, Env> {
    using type = std::conditional_t<std::is_same_v<Tag, repeat_n_t>,
                                    type_list<child_completion_domain_t<ChildAttrs, set_value_t, Env>, start_domain_t<Env>>,
                                    type_list<child_completion_domain_t<ChildAttrs, set_value_t, Env>>>;
};

template <class Tag, class ChildAttrs, class Env>
struct loop_domain_sources<Tag, set_error_t, ChildAttrs, Env> {
    using type = type_list<child_completion_domain_t<ChildAttrs, set_value_t, Env>,
                           child_completion_domain_t<ChildAttrs, set_error_t, Env>>;
};

template <class Tag, class ChildAttrs, class Env>
struct loop_domain_sources<Tag, set_stopped_t, ChildAttrs, Env> {
    using type = type_list<child_completion_domain_t<ChildAttrs, set_value_t, Env>,
                           child_completion_domain_t<ChildAttrs, set_stopped_t, Env>, start_domain_t<Env>>;
};

// Which iteration ends the loop, and on which thread, is known only at run time, so the
// loop claims no completion scheduler; its completion domains are common to its sources.
template <class Tag, class ChildAttrs>
struct loop_attrs {
    template <class CTag, class Env, std::enable_if_t<is_completion_tag_v<CTag>, int> = 0>
    constexpr auto query(get_completion_domain_t<CTag>, Env const &) const noexcept
        -> known_domain_t<rename_t<typename loop_domain_sources<Tag, CTag, ChildAttrs, Env>::type, common_domain_t>> {
        return {};
    }
};

template <class Tag, class Sndr, class Rcvr>
struct loop_state;

// Receives an iteration's completions on behalf of the loop state. Like
// let_child_receiver, it names each type once.
template <class Tag, class Sndr, class Rcvr>
struct loop_receiver {
    using receiver_concept = receiver_t;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        state->iteration_done(static_cast<Vs &&>(vs)...);
    }

    template <class E>
    void set_error(E &&e) && noexcept {
        lexec::set_error(static_cast<Rcvr &&>(*state->rcvr), static_cast<E &&>(e));
    }

    void set_stopped() && noexcept { lexec::set_stopped(static_cast<Rcvr &&>(*state->rcvr)); }

    fwd_env_t<env_of_t<Rcvr>> get_env() const noexcept { return make_fwd_env(lexec::get_env(*state->rcvr)); }

    loop_state<Tag, Sndr, Rcvr> *state;
};

// Each iteration begins as a task on the trampoline, which keeps iterations that
// complete synchronously from deepening the stack without bound, and connects the child
// afresh, as an lvalue. The iteration's operation stays until the next iteration
// replaces it or the loop is destroyed, so errors, which may live in it, are forwarded
// without being copied.
template <class Tag, class Sndr, class Rcvr>
struct loop_state : trampoline_task {
    using data_type = data_of_t<Sndr>;
    using child_type = remove_cvref_t<child_of_t<Sndr, 0>>;
    using receiver = loop_receiver<Tag, Sndr, Rcvr>;
    using child_op = connect_result_t<child_type &, receiver>;
    using rcvr_env = env_of_t<Rcvr>;

    loop_state(Sndr &&sndr, Rcvr &rcvr_) noexcept(
        std::is_nothrow_constructible_v<child_type, child_of_t<Sndr, 0>> and
        noexcept(lexec::connect(std::declval<child_type &>(), std::declval<receiver>())))
        : trampoline_task{&run_iteration}, data(static_cast<Sndr &&>(sndr).data),
          child(detail::get<0>(static_cast<Sndr &&>(sndr).children)), rcvr(&rcvr_) {
        if constexpr (std::is_same_v<Tag, repeat_n_t>) {
            if (data == 0) {
                return;
            }
        }
        connect_child();
    }

    loop_state(loop_state &&) = delete;

    void start() noexcept {
        if constexpr (std::is_same_v<Tag, repeat_n_t>) {
            if (data == 0) {
                lexec::set_value(static_cast<Rcvr &&>(*rcvr));
                return;
            }
        }
        trampoline_run(*this, default_trampoline_limits);
    }

    template <class... Vs>
    void iteration_done(Vs &&...vs) noexcept {
        if constexpr (std::is_same_v<Tag, repeat_until_t>) {
            until_done(static_cast<Vs &&>(vs)...);
        } else if constexpr (std::is_same_v<Tag, repeat_n_t>) {
            if (--data == 0) {
                lexec::set_value(static_cast<Rcvr &&>(*rcvr));
            } else {
                next_iteration();
            }
        } else {
            next_iteration();
        }
    }

    LEXEC_NO_UNIQUE_ADDRESS data_type data;
    child_type child;
    Rcvr *rcvr;
    manual_variant<child_op> ops;

private:
    static constexpr bool nothrow_reconnect = nothrow_reconnect_v<child_type, rcvr_env>;

    static void run_iteration(trampoline_task *const task) noexcept { static_cast<loop_state *>(task)->iterate(); }

    void iterate() noexcept {
        if constexpr (not is_unstoppable_token_v<stop_token_of_t<rcvr_env>>) {
            if (lexec::get_stop_token(lexec::get_env(*rcvr)).stop_requested()) {
                lexec::set_stopped(static_cast<Rcvr &&>(*rcvr));
                return;
            }
        }
        lexec::start(ops.template get<child_op>());
    }

    // The value may live in the iteration's operation, so it is converted before the
    // next iteration replaces that operation.
    template <class B>
    void until_done(B &&b) noexcept {
        if constexpr (always_done_v<B>) {
            lexec::set_value(static_cast<Rcvr &&>(*rcvr));
        } else if constexpr (never_done_v<B>) {
            next_iteration();
        } else if constexpr (nothrow_to_bool_v<B> or not LEXEC_HAS_EXCEPTIONS) {
            if (static_cast<bool>(static_cast<B &&>(b))) {
                lexec::set_value(static_cast<Rcvr &&>(*rcvr));
            } else {
                next_iteration();
            }
        } else {
#if LEXEC_HAS_EXCEPTIONS
            auto done = false;
            try {
                done = static_cast<bool>(static_cast<B &&>(b));
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(*rcvr), std::current_exception());
                return;
            }
            if (done) {
                lexec::set_value(static_cast<Rcvr &&>(*rcvr));
            } else {
                next_iteration();
            }
#endif
        }
    }

    child_op &connect_child() noexcept(noexcept(lexec::connect(std::declval<child_type &>(), std::declval<receiver>()))) {
        return ops.template emplace_with<child_op>([this] { return lexec::connect(child, receiver{this}); });
    }

    // Replaces the operation whose completion is running, then hands the iteration to
    // the trampoline, which may run the whole rest of the loop before returning.
    void next_iteration() noexcept {
        if constexpr (nothrow_reconnect or not LEXEC_HAS_EXCEPTIONS) {
            connect_child();
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                connect_child();
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(*rcvr), std::current_exception());
                return;
            }
#endif
        }
        trampoline_run(*this, default_trampoline_limits);
    }
};

template <class Tag>
struct loop_impls : default_impls {
    static constexpr bool connects_children = false;

    template <class Self, class... Env>
    using completions = typename loop_completions<Tag, remove_cvref_t<child_of_t<Self, 0>>, type_list<Env...>>::type;

    template <class Data, class Child>
    static constexpr loop_attrs<Tag, env_of_t<Child const &>> get_attrs(Data const &, Child const &) noexcept {
        return {};
    }

    template <class Sndr, class Rcvr>
    static auto get_state(Sndr &&sndr, Rcvr &rcvr) noexcept(
        std::is_nothrow_constructible_v<loop_state<Tag, Sndr, Rcvr>, Sndr, Rcvr &>) -> loop_state<Tag, Sndr, Rcvr> {
        return loop_state<Tag, Sndr, Rcvr>{static_cast<Sndr &&>(sndr), rcvr};
    }

    template <class State, class Rcvr>
    static void start(State &state, Rcvr &) noexcept {
        state.start();
    }
};

template <>
struct impls_for<repeat_until_t> : loop_impls<repeat_until_t> {};

template <>
struct impls_for<repeat_t> : loop_impls<repeat_t> {};

template <>
struct impls_for<repeat_n_t> : loop_impls<repeat_n_t> {};

} // namespace detail

// The loops below connect their sender afresh, as an lvalue, for each iteration, and
// begin each iteration on a trampoline_scheduler: iterations that complete synchronously
// do not deepen the stack without bound, and a stop request from the receiver ends the
// loop with set_stopped before the next iteration. Errors and stopped from an iteration
// end the loop and are passed on unchanged.

// Repeats the sender, which completes with one value converting to bool, until it
// completes with true, then completes with set_value().
struct repeat_until_t : sender_adaptor_closure<repeat_until_t> {
    template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sndr &&sndr) const
        -> detail::basic_sender<repeat_until_t, detail::no_data, std::decay_t<Sndr>> {
        return {{}, {{static_cast<Sndr &&>(sndr)}}};
    }

    // `sndr | repeat_until()`, as stdexec spells it; `sndr | repeat_until` works too.
    constexpr repeat_until_t operator()() const noexcept { return {}; }
};

// Repeats the sender, which completes with set_value(), until an error or stopped ends it.
struct repeat_t : sender_adaptor_closure<repeat_t> {
    template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sndr &&sndr) const -> detail::basic_sender<repeat_t, detail::no_data, std::decay_t<Sndr>> {
        return {{}, {{static_cast<Sndr &&>(sndr)}}};
    }

    constexpr repeat_t operator()() const noexcept { return {}; }
};

// Runs the sender, which completes with set_value(), count times one after another,
// then completes with set_value(); a count of zero completes without starting it.
struct repeat_n_t {
    template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sndr &&sndr, std::size_t const count) const
        -> detail::basic_sender<repeat_n_t, std::size_t, std::decay_t<Sndr>> {
        return {count, {{static_cast<Sndr &&>(sndr)}}};
    }

    constexpr auto operator()(std::size_t const count) const -> detail::partial_closure<repeat_n_t, std::size_t> {
        return detail::make_partial_closure<repeat_n_t>(count);
    }
};

inline constexpr repeat_until_t repeat_until{};
inline constexpr repeat_t repeat{};
inline constexpr repeat_n_t repeat_n{};

} // namespace lexec
