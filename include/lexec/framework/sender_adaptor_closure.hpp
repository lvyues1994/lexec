#pragma once

#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>
#include <lexec/framework/basic_sender.hpp>

#include <type_traits>

namespace lexec {

// Base for objects that adapt a sender through `sndr | closure`.
template <class Derived>
struct sender_adaptor_closure {};

namespace detail {

template <class T>
inline constexpr bool is_sender_adaptor_closure_v =
    std::is_base_of_v<sender_adaptor_closure<remove_cvref_t<T>>, remove_cvref_t<T>> and not is_sender_v<T>;

// Holds the trailing arguments of Tag{}(sndr, args...) until the sender arrives.
template <class Tag, class... Args>
struct partial_closure : sender_adaptor_closure<partial_closure<Tag, Args...>> {
    tuple<Args...> args;

    template <class Sndr>
    constexpr auto operator()(Sndr &&sndr) && -> call_result_t<Tag, Sndr, Args...> {
        return static_cast<tuple<Args...> &&>(args).apply(
            [&sndr](Args &&...as) { return Tag{}(static_cast<Sndr &&>(sndr), static_cast<Args &&>(as)...); });
    }

    template <class Sndr>
    constexpr auto operator()(Sndr &&sndr) const & -> call_result_t<Tag, Sndr, Args const &...> {
        return args.apply([&sndr](Args const &...as) { return Tag{}(static_cast<Sndr &&>(sndr), as...); });
    }
};

template <class Tag, class... Args>
constexpr partial_closure<Tag, std::decay_t<Args>...> make_partial_closure(Args &&...args) {
    return {{}, {{static_cast<Args &&>(args)}...}};
}

// An adaptor whose one extra argument becomes the data of the sender it builds:
// `Tag{}(sndr, arg)` builds the sender and `Tag{}(arg)` a closure for `sndr | Tag{}(arg)`.
template <class Tag>
struct data_adaptor {
    template <class Sndr, class Data, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sndr &&sndr, Data &&data) const
        -> basic_sender<Tag, std::decay_t<Data>, std::decay_t<Sndr>> {
        return {static_cast<Data &&>(data), {{static_cast<Sndr &&>(sndr)}}};
    }

    template <class Data>
    constexpr auto operator()(Data &&data) const -> partial_closure<Tag, std::decay_t<Data>> {
        return make_partial_closure<Tag>(static_cast<Data &&>(data));
    }
};

} // namespace detail

template <class Sndr, class Closure,
          std::enable_if_t<is_sender_v<Sndr> and detail::is_sender_adaptor_closure_v<Closure>, int> = 0>
constexpr auto operator|(Sndr &&sndr, Closure &&closure)
    -> decltype(static_cast<Closure &&>(closure)(static_cast<Sndr &&>(sndr))) {
    return static_cast<Closure &&>(closure)(static_cast<Sndr &&>(sndr));
}

} // namespace lexec
