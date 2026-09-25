#pragma once

#include <lexec/algorithms/continues_on.hpp>
#include <lexec/algorithms/when_all.hpp>
#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
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

struct when_any_t;

namespace detail {

// Every child's completions, decayed, since the first is stored until the others finish,
// and stopped, which the when_any's own stop requests may cause.
template <class... ChildSigs>
struct when_any_signature_set {
    static constexpr bool nothrow =
        ((gather_signatures_t<set_value_t, ChildSigs, is_nothrow_decay_copyable_t, all_of_t>::value and
          gather_signatures_t<set_error_t, ChildSigs, is_nothrow_decay_copyable_t, all_of_t>::value) and
         ...);
    using type = unique_t<concat_t<
        completion_signatures<set_stopped_t()>,
        transform_completion_signatures<ChildSigs, completion_signatures<>, decayed_set_value, decayed_set_error>...,
        eptr_completion_if_t<not nothrow>>>;
};

template <class Self, class Indices, class EnvList, class = void>
struct when_any_completions {};

template <class Self, std::size_t... Is, class... Env>
struct when_any_completions<
    Self, std::index_sequence<Is...>, type_list<Env...>,
    std::void_t<completion_signatures_of_t<child_of_t<Self, Is>, when_all_child_env_t<Env...>>...>> {
    using type = typename when_any_signature_set<
        completion_signatures_of_t<child_of_t<Self, Is>, when_all_child_env_t<Env...>>...>::type;
};

template <class Sndr, class Rcvr, class Indices = std::make_index_sequence<remove_cvref_t<Sndr>::child_count>>
struct when_any_state;

template <class Sndr, class Rcvr, std::size_t... Is>
struct when_any_state<Sndr, Rcvr, std::index_sequence<Is...>> {
    using rcvr_env = env_of_t<Rcvr>;
    using completions = typename when_any_completions<Sndr, std::index_sequence<Is...>, type_list<rcvr_env>>::type;
    using results_type = rename_t<typename result_tuples<completions>::type, manual_variant>;
    using stop_token_type = stop_token_of_t<rcvr_env>;

    using stop_callback_type =
        manual_variant<std::conditional_t<is_unstoppable_token_v<stop_token_type>, no_data,
                                          stop_callback_for_t<stop_token_type, on_stop_request>>>;

    when_any_state() noexcept = default;
    when_any_state(when_any_state &&) = delete;

    // The first completion of any kind wins and stops the others.
    template <class Tag, class... Args>
    void notify(Tag, Args &&...args) noexcept {
        auto expected = false;
        if (emplaced.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            using stored = tuple<Tag, std::decay_t<Args>...>;
            auto const store = [&] { result.template emplace_with<stored>([&] { return stored{{Tag{}}, {static_cast<Args &&>(args)}...}; }); };
            if constexpr (is_nothrow_decay_copyable_t<Args...>::value or not LEXEC_HAS_EXCEPTIONS) {
                store();
            } else {
#if LEXEC_HAS_EXCEPTIONS
                try {
                    store();
                } catch (...) {
                    using failed = tuple<set_error_t, std::exception_ptr>;
                    result.template emplace_with<failed>([] { return failed{{set_error_t{}}, {std::current_exception()}}; });
                }
#endif
            }
            stop_source.request_stop();
        }
    }

    // The last arrival completes; every child's writes happen before its decrement.
    void arrive(Rcvr &rcvr) noexcept {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            complete(rcvr);
        }
    }

    void complete(Rcvr &rcvr) noexcept {
        on_stop.reset();
        if (lexec::get_stop_token(lexec::get_env(rcvr)).stop_requested()) {
            lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
            return;
        }
        result.visit([&rcvr](auto &stored) noexcept {
            stored.apply([&rcvr](auto &tag, auto &...args) noexcept {
                tag(static_cast<Rcvr &&>(rcvr), static_cast<std::remove_reference_t<decltype(args)> &&>(args)...);
            });
        });
    }

    results_type result;
    std::atomic<bool> emplaced{false};
    std::atomic<std::size_t> remaining{sizeof...(Is)};
    inplace_stop_source stop_source;
    stop_callback_type on_stop;
};

struct when_any_impls : default_impls {
    template <class Self, class... Env>
    using completions = typename when_any_completions<Self, std::make_index_sequence<remove_cvref_t<Self>::child_count>,
                                                      type_list<Env...>>::type;

    template <class Data, class... Children>
    static constexpr env<> get_attrs(Data const &, Children const &...) noexcept {
        return {};
    }

    template <class Index, class State, class Rcvr>
    static constexpr auto get_env(Index, State const &state, Rcvr const &rcvr) noexcept
        -> env<when_all_stop_env, fwd_env_t<env_of_t<Rcvr const &>>> {
        return env<when_all_stop_env, fwd_env_t<env_of_t<Rcvr const &>>>{when_all_stop_env{&state.stop_source},
                                                                          make_fwd_env(lexec::get_env(rcvr))};
    }

    template <class Sndr, class Rcvr>
    static auto get_state(Sndr &&, Rcvr &) noexcept -> when_any_state<Sndr, Rcvr> {
        return {};
    }

    // A stop requested before start completes the when_any at once, without its children.
    template <class State, class Rcvr, class... Ops>
    static void start(State &state, Rcvr &rcvr, Ops &...ops) noexcept {
        if constexpr (not is_unstoppable_token_v<typename State::stop_token_type>) {
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
        state.notify(Tag{}, static_cast<Args &&>(args)...);
        state.arrive(rcvr);
    }
};

template <>
struct impls_for<when_any_t> : when_any_impls {};

} // namespace detail

// Completes with whichever sender completes first, value, error, or stopped, once the
// others, asked to stop, have completed too.
struct when_any_t {
    template <class... Sndrs, std::enable_if_t<(sizeof...(Sndrs) != 0) and (is_sender_v<Sndrs> and ...), int> = 0>
    constexpr auto operator()(Sndrs &&...sndrs) const
        -> detail::basic_sender<when_any_t, detail::no_data, std::decay_t<Sndrs>...> {
        return {{}, {{static_cast<Sndrs &&>(sndrs)}...}};
    }
};

inline constexpr when_any_t when_any{};

} // namespace lexec
