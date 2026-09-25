#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/manual_variant.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>
#include <lexec/execution_policy.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>

#include <atomic>
#include <exception>
#include <type_traits>

namespace lexec {

struct bulk_t;
struct bulk_chunked_t;
struct bulk_unchunked_t;

namespace detail {

// A policy that cannot be copied is held by reference, as the standard specifies.
template <class Policy>
using bulk_policy_member_t = std::conditional_t<std::is_copy_constructible_v<Policy>, Policy, Policy const &>;

// Initialized by a constructor rather than as an aggregate: Clang 18 generates invalid
// code for nested aggregate initialization of [[no_unique_address]] members.
template <class Policy, class Shape, class Fn>
struct bulk_data {
    using policy_type = remove_cvref_t<Policy>;
    using shape_type = Shape;
    using fn_type = Fn;

    template <class F>
    constexpr bulk_data(Policy const &policy_, Shape const shape_, F &&fn_) noexcept(
        std::is_nothrow_copy_constructible_v<Policy> and std::is_nothrow_constructible_v<Fn, F>)
        : policy(policy_), shape(shape_), fn(static_cast<F &&>(fn_)) {}

    LEXEC_NO_UNIQUE_ADDRESS Policy policy;
    Shape shape;
    LEXEC_NO_UNIQUE_ADDRESS Fn fn;
};

// How the function is called for values Vs: with a chunk's bounds, or with one index.
template <bool Chunked, class Fn, class Shape, class... Vs>
struct bulk_call {
    static constexpr bool callable = is_callable_v<Fn &, Shape, Vs &...>;
    static constexpr bool nothrow = is_nothrow_callable_v<Fn &, Shape, Vs &...>;
};

template <class Fn, class Shape, class... Vs>
struct bulk_call<true, Fn, Shape, Vs...> {
    static constexpr bool callable = is_callable_v<Fn &, Shape, Shape, Vs &...>;
    static constexpr bool nothrow = is_nothrow_callable_v<Fn &, Shape, Shape, Vs &...>;
};

template <class Call, class... Vs>
struct bulk_value_completions {
    static_assert(Call::callable, "lexec::bulk: the function cannot be called with an index (or, for "
                                  "bulk_chunked, a pair of indices) and lvalues of the predecessor's values");
    using type = concat_t<completion_signatures<set_value_t(Vs...)>, eptr_completion_if_t<not Call::nothrow>>;
};

template <bool Chunked, class Fn, class Shape>
struct bulk_transforms {
    template <class... Vs>
    using on_value = typename bulk_value_completions<bulk_call<Chunked, Fn, Shape, Vs...>, Vs...>::type;
};

// The values pass through; the function may add an exception_ptr error.
template <bool Chunked, class Self, class... Env>
using bulk_completions_t = transform_completion_signatures<
    child_completions_t<Self, 0, Env...>, completion_signatures<>,
    bulk_transforms<Chunked, typename data_of_t<Self>::fn_type, typename data_of_t<Self>::shape_type>::template on_value>;

template <bool Chunked>
struct bulk_impls : default_impls {
    template <class Self, class... Env>
    using completions = bulk_completions_t<Chunked, Self, Env...>;

    using default_impls::complete;

    // The default runs every iteration on the agent the predecessor completed on: the
    // whole range in one call for bulk_chunked, one call per index for bulk_unchunked.
    template <class Index, class Data, class Rcvr, class... Args>
    static void complete(Index, Data &data, Rcvr &rcvr, set_value_t, Args &&...args) noexcept {
        using shape_type = typename Data::shape_type;
        constexpr bool nothrow = bulk_call<Chunked, typename Data::fn_type, shape_type, Args...>::nothrow;
        auto const run = [&]() noexcept(nothrow) {
            if constexpr (Chunked) {
                if (shape_type{} < data.shape) {
                    data.fn(shape_type{}, shape_type(data.shape), args...);
                }
            } else {
                for (auto i = shape_type{}; i < data.shape; ++i) {
                    data.fn(shape_type(i), args...);
                }
            }
        };
        if constexpr (nothrow or not LEXEC_HAS_EXCEPTIONS) {
            run();
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                run();
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(rcvr), std::current_exception());
                return;
            }
#endif
        }
        lexec::set_value(static_cast<Rcvr &&>(rcvr), static_cast<Args &&>(args)...);
    }
};

template <>
struct impls_for<bulk_chunked_t> : bulk_impls<true> {};

template <>
struct impls_for<bulk_unchunked_t> : bulk_impls<false> {};

// bulk exists until transform_sender lowers it to bulk_chunked.
template <>
struct impls_for<bulk_t> : bulk_impls<false> {
    template <class Sndr, class Rcvr>
    static no_data get_state(Sndr &&, Rcvr &) noexcept {
        static_assert(dependent_false<Sndr>, "lexec::bulk must be connected with lexec::connect, which lowers it");
        return {};
    }
};

// Runs bulk's per-index function over each chunk bulk_chunked hands out.
template <class Fn>
struct bulk_chunk_loop {
    template <class Shape, class... Vs, std::enable_if_t<is_callable_v<Fn &, Shape, Vs &...>, int> = 0>
    constexpr void operator()(Shape begin, Shape end, Vs &...vs) noexcept(is_nothrow_callable_v<Fn &, Shape, Vs &...>) {
        for (; begin != end; ++begin) {
            fn(Shape(begin), vs...);
        }
    }

    Fn fn;
};

// For schedulers that run the function on other threads: they store the predecessor's
// values first, so the function gets lvalues of decayed copies, which then go downstream.

template <bool Chunked, class Fn, class Shape, class... Vs>
struct stored_bulk_call {
    using call = bulk_call<Chunked, Fn, Shape, std::decay_t<Vs>...>;
    static constexpr bool callable = call::callable;
    static constexpr bool nothrow = call::nothrow and is_nothrow_decay_copyable_t<Vs...>::value;
};

template <bool Chunked, class Fn, class Shape>
struct stored_bulk_transforms {
    template <class... Vs>
    using on_value = typename bulk_value_completions<stored_bulk_call<Chunked, Fn, Shape, Vs...>, std::decay_t<Vs>...>::type;
};

template <class... Vs>
using decayed_tuple_t = tuple<std::decay_t<Vs>...>;

template <class Sigs>
using stored_bulk_values_t =
    rename_t<unique_t<gather_signatures_t<set_value_t, Sigs, decayed_tuple_t, type_list>>, manual_variant>;

// The first exception any invocation of the function throws.
struct bulk_failure {
    std::atomic<bool> failed{false};
    std::exception_ptr error;
};

template <bool Chunked, class Fn, class Shape, class Stored>
inline constexpr bool stored_bulk_may_throw_v = false;

template <bool Chunked, class Fn, class Shape, class Indices, class... Vs>
inline constexpr bool stored_bulk_may_throw_v<Chunked, Fn, Shape, tuple_impl<Indices, Vs...>> =
    LEXEC_HAS_EXCEPTIONS and not bulk_call<Chunked, Fn, Shape, Vs...>::nothrow;

template <bool Chunked, class Fn, class Shape, class Values>
inline constexpr bool stored_bulk_can_fail_v = false;

template <bool Chunked, class Fn, class Shape, class... Stored>
inline constexpr bool stored_bulk_can_fail_v<Chunked, Fn, Shape, manual_variant<Stored...>> =
    (stored_bulk_may_throw_v<Chunked, Fn, Shape, Stored> or ...);

// Where the predecessor of a bulk sender completes, which is where a scheduler that takes
// over the bulk work runs it.
template <class Sndr>
using bulk_child_attrs_t = env_of_t<typename remove_cvref_t<Sndr>::template child_type<0> const &>;

template <class Sndr, class Env>
using bulk_child_scheduler_t = completion_scheduler_result_t<set_value_t, bulk_child_attrs_t<Sndr>, Env>;

template <class Shape>
inline constexpr bool is_bulk_shape_v = std::is_integral_v<Shape> and not std::is_same_v<Shape, bool>;

template <class Policy, class Shape, class Fn>
inline constexpr bool is_bulk_args_v = is_execution_policy_v<remove_cvref_t<Policy>> and is_bulk_shape_v<Shape> and
                                       std::is_copy_constructible_v<std::decay_t<Fn>>;

// bulk-algo(sndr, policy, shape, f) builds the sender; bulk-algo(policy, shape, f) the
// closure for `sndr | bulk-algo(policy, shape, f)`.
template <class Tag>
struct bulk_adaptor {
    template <class Sndr, class Policy, class Shape, class Fn,
              std::enable_if_t<is_sender_v<Sndr> and is_bulk_args_v<Policy, Shape, Fn>, int> = 0>
    constexpr auto operator()(Sndr &&sndr, Policy &&policy, Shape shape, Fn &&fn) const
        -> basic_sender<Tag, bulk_data<bulk_policy_member_t<remove_cvref_t<Policy>>, Shape, std::decay_t<Fn>>,
                        std::decay_t<Sndr>> {
        return {{policy, shape, static_cast<Fn &&>(fn)}, {{static_cast<Sndr &&>(sndr)}}};
    }

    template <class Policy, class Shape, class Fn, std::enable_if_t<is_bulk_args_v<Policy, Shape, Fn>, int> = 0>
    constexpr auto operator()(Policy &&policy, Shape shape, Fn &&fn) const
        -> partial_closure<Tag, remove_cvref_t<Policy>, Shape, std::decay_t<Fn>> {
        return make_partial_closure<Tag>(static_cast<Policy &&>(policy), shape, static_cast<Fn &&>(fn));
    }
};

} // namespace detail

// Invokes f(i, vs...) for every i in [0, shape), where vs are lvalues of the
// predecessor's values, then sends those values on.
struct bulk_t : detail::bulk_adaptor<bulk_t> {
    template <class Sndr, class Env, class Data = detail::data_of_t<Sndr>>
    constexpr auto transform_sender(set_value_t, Sndr &&sndr, Env const &) const
        -> decltype(std::declval<bulk_chunked_t const &>()(
            std::declval<detail::child_of_t<Sndr, 0>>(), std::declval<detail::member_like_t<Sndr, Data>>().policy,
            std::declval<typename Data::shape_type>(),
            std::declval<detail::bulk_chunk_loop<typename Data::fn_type>>()));
};

// Invokes f(b, e, vs...) over chunks [b, e) that together cover [0, shape) once.
struct bulk_chunked_t : detail::bulk_adaptor<bulk_chunked_t> {};

// Invokes f(i, vs...) once for every i in [0, shape), never combining indices.
struct bulk_unchunked_t : detail::bulk_adaptor<bulk_unchunked_t> {};

inline constexpr bulk_t bulk{};
inline constexpr bulk_chunked_t bulk_chunked{};
inline constexpr bulk_unchunked_t bulk_unchunked{};

template <class Sndr, class Env, class Data>
constexpr auto bulk_t::transform_sender(set_value_t, Sndr &&sndr, Env const &) const
    -> decltype(std::declval<bulk_chunked_t const &>()(
        std::declval<detail::child_of_t<Sndr, 0>>(), std::declval<detail::member_like_t<Sndr, Data>>().policy,
        std::declval<typename Data::shape_type>(), std::declval<detail::bulk_chunk_loop<typename Data::fn_type>>())) {
    auto &&data = static_cast<Sndr &&>(sndr).data;
    return bulk_chunked(detail::get<0>(static_cast<Sndr &&>(sndr).children), data.policy, data.shape,
                        detail::bulk_chunk_loop<typename Data::fn_type>{static_cast<decltype(data) &&>(data).fn});
}

} // namespace lexec
