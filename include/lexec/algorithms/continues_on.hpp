#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/scheduler.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/manual_variant.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>

#include <exception>
#include <type_traits>

namespace lexec {

struct continues_on_t;
struct schedule_from_t;

namespace detail {

// SCHED-ATTRS: attributes of a sender that completes on Sch. The completion scheduler
// is answered with or without the receiver's environment, so that joined behind these
// attributes, a predecessor's answer for that environment cannot take precedence.
template <class Sch>
struct sched_attrs {
    template <class Tag, class... Env, std::enable_if_t<is_one_of_v<Tag, set_value_t, set_stopped_t>, int> = 0>
    constexpr Sch query(get_completion_scheduler_t<Tag>, Env const &...) const noexcept {
        return sch;
    }

    template <class Tag, class... Env,
              std::enable_if_t<is_one_of_v<Tag, void, set_value_t, set_stopped_t> and
                                   std::is_invocable_v<get_completion_domain_t<set_value_t>, Sch const &, Env const &...>,
                               int> = 0>
    constexpr auto query(get_completion_domain_t<Tag>, Env const &...env) const noexcept {
        return get_completion_domain<set_value_t>(sch, env...);
    }

    Sch sch;
};

template <class Sig>
struct result_tuple_of;

template <class Tag, class... Args>
struct result_tuple_of<Tag(Args...)> {
    using type = tuple<Tag, std::decay_t<Args>...>;
};

template <class Sigs>
struct result_tuples;

template <class... Sigs>
struct result_tuples<completion_signatures<Sigs...>> {
    using type = unique_t<type_list<typename result_tuple_of<Sigs>::type...>>;
};

template <class Sch, class ChildSigs, class... FwdEnv>
struct continues_on_completions {
    using schedule_sigs = completion_signatures_of_t<schedule_result_t<Sch &>, FwdEnv...>;
    static constexpr bool nothrow =
        gather_signatures_t<set_value_t, ChildSigs, is_nothrow_decay_copyable_t, all_of_t>::value and
        gather_signatures_t<set_error_t, ChildSigs, is_nothrow_decay_copyable_t, all_of_t>::value;
    // The results move into the state before the scheduler runs, then move to the receiver.
    using type = transform_completion_signatures<
        ChildSigs,
        concat_t<transform_completion_signatures<schedule_sigs, completion_signatures<>, no_signatures>,
                 eptr_completion_if_t<not nothrow>>,
        decayed_set_value, decayed_set_error>;
};

template <class Sndr, class Rcvr>
struct continues_on_state;

// Receives the completion of schedule(sch), after which the stored results are replayed.
// Naming Sndr and Rcvr, not the state type, keeps the receiver type from repeating Rcvr.
template <class Sndr, class Rcvr>
struct continues_on_receiver {
    using receiver_concept = receiver_t;

    void set_value() && noexcept { state->replay(); }

    template <class E>
    void set_error(E &&e) && noexcept {
        lexec::set_error(static_cast<Rcvr &&>(*state->rcvr), static_cast<E &&>(e));
    }

    void set_stopped() && noexcept { lexec::set_stopped(static_cast<Rcvr &&>(*state->rcvr)); }

    fwd_env_t<env_of_t<Rcvr>> get_env() const noexcept { return make_fwd_env(lexec::get_env(*state->rcvr)); }

    continues_on_state<Sndr, Rcvr> *state;
};

template <class Sndr, class Rcvr>
struct continues_on_state {
    using scheduler_type = data_of_t<Sndr>;
    using results_type =
        rename_t<typename result_tuples<child_completions_t<Sndr, 0, env_of_t<Rcvr>>>::type, manual_variant>;
    using schedule_receiver = continues_on_receiver<Sndr, Rcvr>;
    using schedule_op = connect_result_t<schedule_result_t<scheduler_type &>, schedule_receiver>;

    continues_on_state(scheduler_type &sch, Rcvr &rcvr_) noexcept(
        noexcept(lexec::connect(lexec::schedule(sch), std::declval<schedule_receiver>())))
        : rcvr(&rcvr_), op(lexec::connect(lexec::schedule(sch), schedule_receiver{this})) {}

    continues_on_state(continues_on_state &&) = delete;

    void replay() noexcept {
        results.visit([this](auto &result) noexcept {
            result.apply([this](auto &tag, auto &...args) noexcept {
                tag(static_cast<Rcvr &&>(*rcvr), static_cast<std::remove_reference_t<decltype(args)> &&>(args)...);
            });
        });
    }

    Rcvr *rcvr;
    results_type results;
    schedule_op op;
};

struct continues_on_impls : default_impls {
    template <class Self, class... Env>
    using completions =
        typename continues_on_completions<data_of_t<Self>, child_completions_t<Self, 0, Env...>, fwd_env_t<Env>...>::type;

    template <class Sch, class Child>
    static constexpr auto get_attrs(Sch const &sch, Child const &child) noexcept
        -> env<sched_attrs<Sch>, fwd_env_t<env_of_t<Child const &>>> {
        return env<sched_attrs<Sch>, fwd_env_t<env_of_t<Child const &>>>{sched_attrs<Sch>{sch},
                                                                         make_fwd_env(lexec::get_env(child))};
    }

    template <class Sndr, class Rcvr>
    static auto get_state(Sndr &&sndr, Rcvr &rcvr) noexcept(
        std::is_nothrow_constructible_v<continues_on_state<Sndr, Rcvr>, data_of_t<Sndr> &, Rcvr &>)
        -> continues_on_state<Sndr, Rcvr> {
        return continues_on_state<Sndr, Rcvr>{sndr.data, rcvr};
    }

    template <class Index, class State, class Rcvr, class Tag, class... Args>
    static void complete(Index, State &state, [[maybe_unused]] Rcvr &rcvr, Tag, Args &&...args) noexcept {
        using result = tuple<Tag, std::decay_t<Args>...>;
        auto const store = [&] {
            state.results.template emplace_with<result>([&] { return result{{Tag{}}, {static_cast<Args &&>(args)}...}; });
        };
        if constexpr (is_nothrow_decay_copyable_t<Args...>::value or not LEXEC_HAS_EXCEPTIONS) {
            store();
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                store();
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(rcvr), std::current_exception());
                return;
            }
#endif
        }
        lexec::start(state.op);
    }
};

// Passes its child through unchanged. Wrapping the predecessor of continues_on lets the
// domain the predecessor completes in customize how work leaves it.
struct schedule_from_impls : default_impls {
    template <class Self, class... Env>
    using completions = child_completions_t<Self, 0, Env...>;
};

template <>
struct impls_for<continues_on_t> : continues_on_impls {};

template <>
struct impls_for<schedule_from_t> : schedule_from_impls {};

} // namespace detail

struct schedule_from_t {
    template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sndr &&sndr) const -> detail::basic_sender<schedule_from_t, detail::no_data, std::decay_t<Sndr>> {
        return {{}, {{static_cast<Sndr &&>(sndr)}}};
    }
};

inline constexpr schedule_from_t schedule_from{};

// Completes on the scheduler's execution resource with the results of the sender,
// which move once into the operation while the scheduler runs.
struct continues_on_t {
    template <class Sndr, class Sch, std::enable_if_t<is_sender_v<Sndr> and is_scheduler_v<Sch>, int> = 0>
    constexpr auto operator()(Sndr &&sndr, Sch &&sch) const
        -> detail::basic_sender<continues_on_t, std::decay_t<Sch>,
                                detail::basic_sender<schedule_from_t, detail::no_data, std::decay_t<Sndr>>> {
        return {static_cast<Sch &&>(sch), {{schedule_from(static_cast<Sndr &&>(sndr))}}};
    }

    template <class Sch, std::enable_if_t<is_scheduler_v<Sch>, int> = 0>
    constexpr auto operator()(Sch &&sch) const -> detail::partial_closure<continues_on_t, std::decay_t<Sch>> {
        return detail::make_partial_closure<continues_on_t>(static_cast<Sch &&>(sch));
    }
};

inline constexpr continues_on_t continues_on{};

} // namespace lexec
