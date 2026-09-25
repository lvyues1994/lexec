#pragma once

#include <lexec/algorithms/into_variant.hpp>
#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/manual_variant.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/stop_token.hpp>

#include <atomic>
#include <cstddef>
#include <exception>
#include <type_traits>
#include <utility>

namespace lexec {

struct when_all_t;
struct when_all_with_variant_t;

namespace detail {

// The children's stop token comes from the when_all's own stop source.
struct when_all_stop_env {
    inplace_stop_token query(get_stop_token_t) const noexcept { return source->get_token(); }

    inplace_stop_source const *source;
};

template <class... Env>
struct when_all_child_env {
    using type = env<when_all_stop_env>;
};

template <class Env>
struct when_all_child_env<Env> {
    using type = env<when_all_stop_env, fwd_env_t<Env>>;
};

template <class... Env>
using when_all_child_env_t = typename when_all_child_env<Env...>::type;

template <class... Env>
inline constexpr bool is_stoppable_env_v = (not is_unstoppable_token_v<stop_token_of_t<Env>> or ...);

template <class Sigs>
using value_args_of_t = gather_signatures_t<set_value_t, Sigs, type_list, type_list>;

template <class ValueLists>
struct single_value_args {
    using type = type_list<>;
    static constexpr bool present = false;
};

template <class... Vs>
struct single_value_args<type_list<type_list<Vs...>>> {
    using type = type_list<std::decay_t<Vs>...>;
    static constexpr bool present = true;
};

template <class Sigs>
using error_signatures_t =
    transform_completion_signatures<Sigs, completion_signatures<>, no_signatures, decayed_set_error, completion_signatures<>>;

template <class... Vs>
struct set_value_signature {
    using type = set_value_t(Vs...);
};

// What the children's completions make of the when_all's: their values concatenated,
// their errors, and stopped.
template <class... ChildSigs>
struct when_all_signature_set {
    static_assert(((count_signatures_v<set_value_t, ChildSigs> <= 1) and ...),
                  "lexec::when_all requires each sender to have at most one value completion signature");

    static constexpr bool sends_values = (single_value_args<value_args_of_t<ChildSigs>>::present and ...);
    static constexpr bool sends_stopped = ((count_signatures_v<set_stopped_t, ChildSigs> != 0) or ...);
    static constexpr bool nothrow =
        ((gather_signatures_t<set_value_t, ChildSigs, is_nothrow_decay_copyable_t, all_of_t>::value and
          gather_signatures_t<set_error_t, ChildSigs, is_nothrow_decay_copyable_t, all_of_t>::value) and
         ...);

    using values = std::conditional_t<
        sends_values,
        completion_signatures<typename rename_t<
            concat_t<type_list<>, typename single_value_args<value_args_of_t<ChildSigs>>::type...>,
            set_value_signature>::type>,
        completion_signatures<>>;
    using errors = concat_t<completion_signatures<>, error_signatures_t<ChildSigs>...>;
};

template <class Self, class Indices, class EnvList, class = void>
struct when_all_completions {};

template <class Self, std::size_t... Is, class... Env>
struct when_all_completions<
    Self, std::index_sequence<Is...>, type_list<Env...>,
    std::void_t<completion_signatures_of_t<child_of_t<Self, Is>, when_all_child_env_t<Env...>>...>> {
    using signatures =
        when_all_signature_set<completion_signatures_of_t<child_of_t<Self, Is>, when_all_child_env_t<Env...>>...>;
    // A stop requested by the receiver before the when_all starts stops it at once.
    using type = unique_t<concat_t<typename signatures::values, typename signatures::errors,
                                   std::conditional_t<signatures::sends_stopped or is_stoppable_env_v<Env...>,
                                                      completion_signatures<set_stopped_t()>, completion_signatures<>>,
                                   eptr_completion_if_t<not signatures::nothrow>>>;
};

template <class Sigs>
struct child_values_storage {
    using type = rename_t<std::conditional_t<single_value_args<value_args_of_t<Sigs>>::present,
                                             type_list<rename_t<typename single_value_args<value_args_of_t<Sigs>>::type, tuple>>,
                                             type_list<>>,
                          manual_variant>;
};

template <class ErrorSig>
struct error_type;

template <class E>
struct error_type<set_error_t(E)> {
    using type = E;
};

template <class ErrorSigs>
struct error_types;

// MSVC does not match `completion_signatures<set_error_t(Es)...>` against an empty list.
template <class... ErrorSigs>
struct error_types<completion_signatures<ErrorSigs...>> {
    using type = type_list<typename error_type<ErrorSigs>::type...>;
};

enum class when_all_disposition : unsigned char { started, error, stopped };

struct on_stop_request {
    void operator()() noexcept { source->request_stop(); }

    inplace_stop_source *source;
};

template <class Sndr, class Rcvr, class Indices = std::make_index_sequence<remove_cvref_t<Sndr>::child_count>>
struct when_all_state;

template <class Sndr, class Rcvr, std::size_t... Is>
struct when_all_state<Sndr, Rcvr, std::index_sequence<Is...>> {
    using rcvr_env = env_of_t<Rcvr>;
    using child_env = when_all_child_env_t<rcvr_env>;
    using signatures = when_all_signature_set<completion_signatures_of_t<child_of_t<Sndr, Is>, child_env>...>;
    using values_type = tuple<typename child_values_storage<completion_signatures_of_t<child_of_t<Sndr, Is>, child_env>>::type...>;
    using errors_type = rename_t<unique_t<concat_t<typename error_types<typename signatures::errors>::type,
                                                   std::conditional_t<signatures::nothrow or not LEXEC_HAS_EXCEPTIONS, type_list<>,
                                                                      type_list<std::exception_ptr>>>>,
                                 manual_variant>;
    using stop_token_type = stop_token_of_t<rcvr_env>;
    static constexpr bool stoppable = not is_unstoppable_token_v<stop_token_type>;
    using stop_callback_type =
        manual_variant<std::conditional_t<stoppable, stop_callback_for_t<stop_token_type, on_stop_request>, no_data>>;

    when_all_state() noexcept = default;
    when_all_state(when_all_state &&) = delete;

    // The first error wins over stopped and over every later error.
    template <class E>
    void set_error(E &&e) noexcept {
        if (disposition.exchange(when_all_disposition::error, std::memory_order_acq_rel) == when_all_disposition::error) {
            return;
        }
        if constexpr (is_nothrow_decay_copyable_t<E>::value or not LEXEC_HAS_EXCEPTIONS) {
            errors.template emplace_with<std::decay_t<E>>([&] { return std::decay_t<E>(static_cast<E &&>(e)); });
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                errors.template emplace_with<std::decay_t<E>>([&] { return std::decay_t<E>(static_cast<E &&>(e)); });
            } catch (...) {
                errors.template emplace_with<std::exception_ptr>([] { return std::current_exception(); });
            }
#endif
        }
        stop_source.request_stop();
    }

    void set_stopped() noexcept {
        auto expected = when_all_disposition::started;
        if (disposition.compare_exchange_strong(expected, when_all_disposition::stopped, std::memory_order_acq_rel)) {
            stop_source.request_stop();
        }
    }

    template <std::size_t I, class... Args>
    void store_values(Args &&...args) noexcept {
        if (disposition.load(std::memory_order_relaxed) != when_all_disposition::started) {
            return;
        }
        using stored = tuple<std::decay_t<Args>...>;
        auto &slot = detail::get<I>(values);
        if constexpr (is_nothrow_decay_copyable_t<Args...>::value or not LEXEC_HAS_EXCEPTIONS) {
            slot.template emplace_with<stored>([&] { return stored{{static_cast<Args &&>(args)}...}; });
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                slot.template emplace_with<stored>([&] { return stored{{static_cast<Args &&>(args)}...}; });
            } catch (...) {
                set_error(std::current_exception());
            }
#endif
        }
    }

    // The last child to complete completes the when_all; every child's writes happen
    // before its decrement, which the last one acquires.
    void arrive(Rcvr &rcvr) noexcept {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            complete(rcvr);
        }
    }

    void complete(Rcvr &rcvr) noexcept {
        on_stop.reset();
        switch (disposition.load(std::memory_order_relaxed)) {
        case when_all_disposition::started:
            if constexpr (signatures::sends_values) {
                send_values<0>(rcvr);
            } else {
                // A child without value completions ends with an error or stopped.
                std::terminate();
            }
            break;
        case when_all_disposition::error:
            errors.visit([&rcvr](auto &error) noexcept {
                lexec::set_error(static_cast<Rcvr &&>(rcvr), static_cast<std::remove_reference_t<decltype(error)> &&>(error));
            });
            break;
        case when_all_disposition::stopped:
            if constexpr (signatures::sends_stopped or stoppable) {
                lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
            } else {
                std::terminate();
            }
            break;
        }
    }

    // Passes every child's stored values, in order, as rvalue references.
    template <std::size_t I, class... Vs>
    void send_values(Rcvr &rcvr, Vs &&...vs) noexcept {
        if constexpr (I == sizeof...(Is)) {
            lexec::set_value(static_cast<Rcvr &&>(rcvr), static_cast<Vs &&>(vs)...);
        } else {
            using stored = remove_cvref_t<decltype(detail::get<I>(values))>;
            using tuple_type = typename stored::template alternative<0>;
            detail::get<I>(values).template get<tuple_type>().apply([&](auto &...next) noexcept {
                send_values<I + 1>(rcvr, static_cast<Vs &&>(vs)...,
                                   static_cast<std::remove_reference_t<decltype(next)> &&>(next)...);
            });
        }
    }

    values_type values;
    errors_type errors;
    std::atomic<std::size_t> remaining{sizeof...(Is)};
    std::atomic<when_all_disposition> disposition{when_all_disposition::started};
    inplace_stop_source stop_source;
    stop_callback_type on_stop;
};

// when_all completes wherever its last child does, with whatever that child completed with.
template <class... Children>
struct when_all_attrs {
    template <class Tag, class Env>
    constexpr auto query(get_completion_domain_t<Tag>, Env const &) const noexcept
        -> known_domain_t<common_domain_t<typename completion_domain_or_unknown<
            env_of_t<Children const &>, void, when_all_child_env_t<Env>>::type...>> {
        return {};
    }
};

struct when_all_impls : default_impls {
    template <class Self, class... Env>
    using completions = typename when_all_completions<Self, std::make_index_sequence<remove_cvref_t<Self>::child_count>,
                                                      type_list<Env...>>::type;

    template <class Data, class... Children>
    static constexpr when_all_attrs<Children...> get_attrs(Data const &, Children const &...) noexcept {
        return {};
    }

    template <class Index, class State, class Rcvr>
    static constexpr auto get_env(Index, State const &state, Rcvr const &rcvr) noexcept
        -> env<when_all_stop_env, fwd_env_t<env_of_t<Rcvr const &>>> {
        return env<when_all_stop_env, fwd_env_t<env_of_t<Rcvr const &>>>{when_all_stop_env{&state.stop_source},
                                                                          make_fwd_env(lexec::get_env(rcvr))};
    }

    template <class Sndr, class Rcvr>
    static auto get_state(Sndr &&, Rcvr &) noexcept -> when_all_state<Sndr, Rcvr> {
        return {};
    }

    template <class State, class Rcvr, class... Ops>
    static void start(State &state, Rcvr &rcvr, Ops &...ops) noexcept {
        if constexpr (State::stoppable) {
            using callback = typename State::stop_callback_type::template alternative<0>;
            state.on_stop.template emplace_with<callback>([&] {
                return callback{lexec::get_stop_token(lexec::get_env(rcvr)), on_stop_request{&state.stop_source}};
            });
            if (state.stop_source.stop_requested()) {
                state.on_stop.reset();
                lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
                return;
            }
        }
        (lexec::start(ops), ...);
    }

    template <class Index, class State, class Rcvr, class Tag, class... Args>
    static void complete(Index, State &state, Rcvr &rcvr, Tag, Args &&...args) noexcept {
        if constexpr (std::is_same_v<Tag, set_value_t>) {
            state.template store_values<Index::value>(static_cast<Args &&>(args)...);
        } else if constexpr (std::is_same_v<Tag, set_error_t>) {
            state.set_error(static_cast<Args &&>(args)...);
        } else {
            state.set_stopped();
        }
        state.arrive(rcvr);
    }
};

struct when_all_with_variant_impls : lowered_impls {};

template <>
struct impls_for<when_all_t> : when_all_impls {};

template <>
struct impls_for<when_all_with_variant_t> : when_all_with_variant_impls {};

} // namespace detail

// Completes with the values of every sender, in order, once all have completed with
// values; the first error or a stop requests stop of the others and wins.
struct when_all_t {
    template <class... Sndrs, std::enable_if_t<(sizeof...(Sndrs) != 0) and (is_sender_v<Sndrs> and ...), int> = 0>
    constexpr auto operator()(Sndrs &&...sndrs) const
        -> detail::basic_sender<when_all_t, detail::no_data, std::decay_t<Sndrs>...> {
        return {{}, {{static_cast<Sndrs &&>(sndrs)}...}};
    }
};

inline constexpr when_all_t when_all{};

// when_all of senders with several value completions, each sending a std::variant of
// its value tuples.
struct when_all_with_variant_t {
    template <class... Sndrs, std::enable_if_t<(sizeof...(Sndrs) != 0) and (is_sender_v<Sndrs> and ...), int> = 0>
    constexpr auto operator()(Sndrs &&...sndrs) const
        -> detail::basic_sender<when_all_with_variant_t, detail::no_data, std::decay_t<Sndrs>...> {
        return {{}, {{static_cast<Sndrs &&>(sndrs)}...}};
    }

    template <class Sndr, class Env>
    constexpr auto transform_sender(set_value_t, Sndr &&sndr, Env const &) const {
        return static_cast<Sndr &&>(sndr).apply([](when_all_with_variant_t, auto &&, auto &&...children) {
            return when_all(into_variant(static_cast<decltype(children) &&>(children))...);
        });
    }
};

inline constexpr when_all_with_variant_t when_all_with_variant{};

} // namespace lexec
