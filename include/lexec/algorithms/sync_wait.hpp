#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/core/sender_traits.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/schedulers/run_loop.hpp>

#include <exception>
#include <optional>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace lexec {

namespace detail {

struct sync_wait_env {
    run_loop_scheduler query(get_scheduler_t) const noexcept { return loop->get_scheduler(); }
    run_loop_scheduler query(get_start_scheduler_t) const noexcept { return loop->get_scheduler(); }
    run_loop_scheduler query(get_delegation_scheduler_t) const noexcept { return loop->get_scheduler(); }

    run_loop *loop;
};

template <class ValueTuples>
struct single_value_tuple {
    static_assert(dependent_false<ValueTuples>, "lexec::sync_wait requires a sender with exactly one value "
                                                "completion signature");
};

template <class ValueTuple>
struct single_value_tuple<type_list<ValueTuple>> {
    using type = ValueTuple;
};

// Both results fall back to a placeholder when the completions are unknown, so that the
// static_assert, not a substitution failure, reports the problem.
template <class Sndr, bool = is_sender_in_v<Sndr, sync_wait_env>>
struct sync_wait_result {
    using type = std::optional<std::tuple<>>;
};

template <class Sndr>
struct sync_wait_result<Sndr, true> {
    using type = std::optional<typename single_value_tuple<
        gather_signatures_t<set_value_t, completion_signatures_of_t<Sndr, sync_wait_env>, decayed_tuple, type_list>>::type>;
};

template <class Sndr>
using sync_wait_result_t = typename sync_wait_result<Sndr>::type;

template <class Sndr, bool = is_sender_in_v<Sndr, sync_wait_env>>
struct sync_wait_with_variant_result {
    using type = std::optional<std::variant<std::tuple<>>>;
};

template <class Sndr>
struct sync_wait_with_variant_result<Sndr, true> {
    using type = std::optional<value_types_of_t<Sndr, sync_wait_env>>;
};

template <class Sndr>
using sync_wait_with_variant_result_t = typename sync_wait_with_variant_result<Sndr>::type;

struct sync_wait_state {
    run_loop loop;
#if LEXEC_HAS_EXCEPTIONS
    std::exception_ptr error;
#endif
};

#if LEXEC_HAS_EXCEPTIONS
template <class E>
std::exception_ptr as_exception_ptr(E &&e) noexcept {
    if constexpr (std::is_same_v<std::decay_t<E>, std::exception_ptr>) {
        return static_cast<E &&>(e);
    } else if constexpr (std::is_same_v<std::decay_t<E>, std::error_code>) {
        return std::make_exception_ptr(std::system_error(e));
    } else {
        return std::make_exception_ptr(static_cast<E &&>(e));
    }
}
#endif

// The values go straight into the result: as its tuple for sync_wait, and for
// sync_wait_with_variant as the alternative of its variant that holds them.
template <class Result, bool InVariant>
struct sync_wait_receiver {
    using receiver_concept = receiver_t;

    // Only the values' construction can throw; the standard leaves the noexcept of the
    // variant and tuple constructors unspecified.
    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
#if LEXEC_HAS_EXCEPTIONS
        if constexpr (is_nothrow_decay_copyable_t<Vs...>::value) {
            store(static_cast<Vs &&>(vs)...);
        } else {
            try {
                store(static_cast<Vs &&>(vs)...);
            } catch (...) {
                state->error = std::current_exception();
            }
        }
#else
        store(static_cast<Vs &&>(vs)...);
#endif
        state->loop.finish();
    }

    template <class... Vs>
    void store(Vs &&...vs) {
        if constexpr (InVariant) {
            result->emplace(std::in_place_type<decayed_tuple<Vs...>>, static_cast<Vs &&>(vs)...);
        } else {
            result->emplace(static_cast<Vs &&>(vs)...);
        }
    }

    template <class E>
    void set_error(E &&e) && noexcept {
#if LEXEC_HAS_EXCEPTIONS
        state->error = as_exception_ptr(static_cast<E &&>(e));
        state->loop.finish();
#else
        static_cast<void>(e);
        std::terminate();
#endif
    }

    void set_stopped() && noexcept { state->loop.finish(); }

    sync_wait_env get_env() const noexcept { return sync_wait_env{&state->loop}; }

    sync_wait_state *state;
    Result *result;
};

// The return type is spelled out because Clang 18 skips the named return value
// optimization for deduced return types, which would move the result once more.
template <class Result, bool InVariant, class Sndr>
Result run_sync_wait(Sndr &&sndr) {
    auto state = sync_wait_state{};
    auto result = Result{};
    auto op = lexec::connect(static_cast<Sndr &&>(sndr), sync_wait_receiver<Result, InVariant>{&state, &result});
    lexec::start(op);
    state.loop.run();
#if LEXEC_HAS_EXCEPTIONS
    if (state.error) {
        std::rethrow_exception(state.error);
    }
#endif
    return result;
}

} // namespace detail

// Blocks until the sender completes, driving a run_loop on the calling thread. Returns
// the values, an empty optional when stopped, or rethrows the error.
struct sync_wait_t {
    template <class Sndr>
    auto operator()(Sndr &&sndr) const -> detail::sync_wait_result_t<Sndr> {
        static_assert(is_sender_in_v<Sndr, detail::sync_wait_env>,
                      "lexec::sync_wait: the sender's completion signatures cannot be computed");
        return detail::run_sync_wait<detail::sync_wait_result_t<Sndr>, false>(static_cast<Sndr &&>(sndr));
    }
};

// sync_wait for a sender with any number of value completions: the values come back as
// a std::variant of std::tuple, one alternative for each set of value types.
struct sync_wait_with_variant_t {
    template <class Sndr>
    auto operator()(Sndr &&sndr) const -> detail::sync_wait_with_variant_result_t<Sndr> {
        static_assert(is_sender_in_v<Sndr, detail::sync_wait_env>,
                      "lexec::sync_wait_with_variant: the sender's completion signatures cannot be computed");
        return detail::run_sync_wait<detail::sync_wait_with_variant_result_t<Sndr>, true>(static_cast<Sndr &&>(sndr));
    }
};

inline constexpr sync_wait_t sync_wait{};
inline constexpr sync_wait_with_variant_t sync_wait_with_variant{};

} // namespace lexec
