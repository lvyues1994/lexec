#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/core/sender_traits.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/framework/basic_sender.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>

#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace lexec {

struct into_variant_t;

namespace detail {

// Only the elements' construction can throw; the standard leaves the noexcept of the
// variant and tuple constructors unspecified.
template <class... Vs>
using is_nothrow_decay_copyable_t = std::bool_constant<(std::is_nothrow_constructible_v<std::decay_t<Vs>, Vs> and ...)>;

template <class...>
using no_signatures = completion_signatures<>;

template <class ChildSigs>
struct into_variant_completions {
    using variant_type = gather_signatures_t<set_value_t, ChildSigs, decayed_tuple, lexec::variant_or_empty>;
    static constexpr bool nothrow =
        gather_signatures_t<set_value_t, ChildSigs, is_nothrow_decay_copyable_t, all_of_t>::value;
    using type = transform_completion_signatures<
        ChildSigs, concat_t<completion_signatures<set_value_t(variant_type)>, eptr_completion_if_t<not nothrow>>,
        no_signatures>;
};

struct into_variant_impls : default_impls {
    template <class Self, class... Env>
    using completions = typename into_variant_completions<child_completions_t<Self, 0, Env...>>::type;

    template <class Sndr, class Rcvr>
    static constexpr auto get_state(Sndr &&, Rcvr &) noexcept -> type_identity<
        typename into_variant_completions<child_completions_t<Sndr, 0, env_of_t<Rcvr>>>::variant_type> {
        return {};
    }

    using default_impls::complete;

    template <class Index, class State, class Rcvr, class... Args>
    static void complete(Index, State &, Rcvr &rcvr, set_value_t, Args &&...args) noexcept {
        using variant_type = typename State::type;
        using tuple_type = decayed_tuple<Args...>;
        if constexpr (is_nothrow_decay_copyable_t<Args...>::value or not LEXEC_HAS_EXCEPTIONS) {
            lexec::set_value(static_cast<Rcvr &&>(rcvr),
                             variant_type{std::in_place_type<tuple_type>, static_cast<Args &&>(args)...});
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                lexec::set_value(static_cast<Rcvr &&>(rcvr),
                                 variant_type{std::in_place_type<tuple_type>, static_cast<Args &&>(args)...});
            } catch (...) {
                lexec::set_error(static_cast<Rcvr &&>(rcvr), std::current_exception());
            }
#endif
        }
    }
};

template <>
struct impls_for<into_variant_t> : into_variant_impls {};

} // namespace detail

// Completes with a single value: a std::variant holding a std::tuple of whichever
// values the sender completed with.
struct into_variant_t : sender_adaptor_closure<into_variant_t> {
    template <class Sndr, std::enable_if_t<is_sender_v<Sndr>, int> = 0>
    constexpr auto operator()(Sndr &&sndr) const -> detail::basic_sender<into_variant_t, detail::no_data, std::decay_t<Sndr>> {
        return {{}, {{static_cast<Sndr &&>(sndr)}}};
    }
};

inline constexpr into_variant_t into_variant{};

} // namespace lexec
