#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>

#include <exception>
#include <type_traits>

namespace lexec {

struct then_t;
struct upon_error_t;
struct upon_stopped_t;

namespace detail {

template <class R>
struct value_completion_of {
    using type = completion_signatures<set_value_t(R)>;
};

template <>
struct value_completion_of<void> {
    using type = completion_signatures<set_value_t()>;
};

template <bool Callable, class Fn, class... Args>
struct call_completions {
    static_assert(Callable, "lexec::then/upon_error/upon_stopped: the function cannot be called with "
                            "the results the predecessor sender completes with");
    using type = completion_signatures<>;
};

template <class Fn, class... Args>
struct call_completions<true, Fn, Args...> {
    using type = concat_t<typename value_completion_of<call_result_t<Fn, Args...>>::type,
                          eptr_completion_if_t<not is_nothrow_callable_v<Fn, Args...>>>;
};

// Completions of calling Fn (as an rvalue) with the arguments of one completion.
template <class Fn, class... Args>
using call_completions_t = typename call_completions<is_callable_v<Fn, Args...>, Fn, Args...>::type;

template <class Fn>
struct then_transforms {
    template <class... Vs>
    using on_value = call_completions_t<Fn, Vs...>;

    template <class E>
    using on_error = call_completions_t<Fn, E>;
};

template <class SetTag, class Fn, class ChildSigs>
struct then_completions;

template <class Fn, class ChildSigs>
struct then_completions<set_value_t, Fn, ChildSigs> {
    using type = transform_completion_signatures<ChildSigs, completion_signatures<>,
                                                 then_transforms<Fn>::template on_value>;
};

template <class Fn, class ChildSigs>
struct then_completions<set_error_t, Fn, ChildSigs> {
    using type = transform_completion_signatures<ChildSigs, completion_signatures<>, default_set_value,
                                                 then_transforms<Fn>::template on_error>;
};

template <class Fn, class ChildSigs>
struct then_completions<set_stopped_t, Fn, ChildSigs> {
    using type = transform_completion_signatures<ChildSigs, completion_signatures<>, default_set_value,
                                                 default_set_error, call_completions_t<Fn>>;
};

template <class Rcvr, class Fn, class... Args>
constexpr void set_value_with_result(Rcvr &rcvr, Fn &&fn, Args &&...args) noexcept(
    is_nothrow_callable_v<Fn, Args...>) {
    if constexpr (std::is_void_v<call_result_t<Fn, Args...>>) {
        static_cast<Fn &&>(fn)(static_cast<Args &&>(args)...);
        lexec::set_value(static_cast<Rcvr &&>(rcvr));
    } else {
        lexec::set_value(static_cast<Rcvr &&>(rcvr), static_cast<Fn &&>(fn)(static_cast<Args &&>(args)...));
    }
}

template <class SetTag>
struct then_impls : default_impls {
    template <class Self, class... Env>
    using completions = typename then_completions<SetTag, data_of_t<Self>, child_completions_t<Self, 0, Env...>>::type;

    using default_impls::complete;

    template <class Index, class Fn, class Rcvr, class... Args>
    static void complete(Index, Fn &fn, Rcvr &rcvr, SetTag, Args &&...args) noexcept {
        if constexpr (is_nothrow_callable_v<Fn, Args...> or not LEXEC_HAS_EXCEPTIONS) {
            set_value_with_result(rcvr, static_cast<Fn &&>(fn), static_cast<Args &&>(args)...);
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                set_value_with_result(rcvr, static_cast<Fn &&>(fn), static_cast<Args &&>(args)...);
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(rcvr), std::current_exception());
            }
#endif
        }
    }
};

template <>
struct impls_for<then_t> : then_impls<set_value_t> {};

template <>
struct impls_for<upon_error_t> : then_impls<set_error_t> {};

template <>
struct impls_for<upon_stopped_t> : then_impls<set_stopped_t> {};

} // namespace detail

struct then_t : detail::data_adaptor<then_t> {};
struct upon_error_t : detail::data_adaptor<upon_error_t> {};
struct upon_stopped_t : detail::data_adaptor<upon_stopped_t> {};

inline constexpr then_t then{};
inline constexpr upon_error_t upon_error{};
inline constexpr upon_stopped_t upon_stopped{};

} // namespace lexec
