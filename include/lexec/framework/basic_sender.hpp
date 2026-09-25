#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/domain.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace lexec::detail {

// An algorithm is described by specializing impls_for<Tag> with a type that derives
// from default_impls, hides the hooks it customizes, and defines
// `template <class Self, class... Env> using completions = completion_signatures<...>;`
template <class Tag>
struct impls_for;

// The data of an algorithm that stores nothing besides its children.
struct no_data {};

template <class Sndr>
using data_of_t = typename remove_cvref_t<Sndr>::data_type;

// The I-th child of Sndr, as an rvalue when Sndr is an rvalue and as a const lvalue otherwise.
template <class Sndr, std::size_t I>
using child_of_t = member_like_t<Sndr, typename remove_cvref_t<Sndr>::template child_type<I>>;

// Completions of the I-th child as seen through the default receiver environment.
template <class Sndr, std::size_t I, class... Env>
using child_completions_t = completion_signatures_of_t<child_of_t<Sndr, I>, fwd_env_t<Env>...>;

struct default_impls {
    // An algorithm that connects its children itself, inside its state, sets this to
    // false; the framework then connects none of them.
    static constexpr bool connects_children = true;

    template <class Data, class... Child>
    static constexpr auto get_attrs(Data const &, Child const &...child) noexcept {
        if constexpr (sizeof...(Child) == 1) {
            return make_fwd_env(lexec::get_env(child)...);
        } else {
            return env<>{};
        }
    }

    template <class Index, class State, class Rcvr>
    static constexpr auto get_env(Index, State &, Rcvr const &rcvr) noexcept -> fwd_env_t<env_of_t<Rcvr const &>> {
        return make_fwd_env(lexec::get_env(rcvr));
    }

    template <class Sndr, class Rcvr>
    static constexpr decltype(auto) get_state(Sndr &&sndr, Rcvr &) noexcept {
        return (static_cast<Sndr &&>(sndr).data);
    }

    template <class State, class Rcvr, class... Ops>
    static constexpr void start(State &, Rcvr &, Ops &...ops) noexcept {
        (lexec::start(ops), ...);
    }

    template <class Index, class State, class Rcvr, class Tag, class... Args>
    static constexpr void complete(Index, State &, Rcvr &rcvr, Tag, Args &&...args) noexcept {
        Tag{}(static_cast<Rcvr &&>(rcvr), static_cast<Args &&>(args)...);
    }
};

template <class Self>
struct completions_after_lowering {};

// An algorithm that exists only until transform_sender lowers it, which connect always
// does; without an environment to lower in, its senders are dependent.
struct lowered_impls : default_impls {
    template <class Self, class... Env>
    using completions = typename completions_after_lowering<Self>::type;

    template <class Sndr, class Rcvr>
    static no_data get_state(Sndr &&, Rcvr &) noexcept {
        static_assert(dependent_false<Sndr>, "this lexec algorithm must be connected with lexec::connect, which lowers it");
        return {};
    }
};

template <class Sndr, class Rcvr>
using get_state_result_t =
    decltype(impls_for<tag_of_t<Sndr>>::get_state(std::declval<Sndr>(), std::declval<Rcvr &>()));

template <class Sndr, class Rcvr>
using state_type_t = std::decay_t<get_state_result_t<Sndr, Rcvr>>;

// A state returned as a prvalue of its own type is constructed in place, without a move.
template <class Sndr, class Rcvr>
inline constexpr bool is_nothrow_state_init_v =
    std::is_same_v<get_state_result_t<Sndr, Rcvr>, state_type_t<Sndr, Rcvr>> or
    std::is_nothrow_constructible_v<state_type_t<Sndr, Rcvr>, get_state_result_t<Sndr, Rcvr>>;

template <class Sndr, class Rcvr>
inline constexpr bool is_nothrow_basic_state_v =
    std::is_nothrow_move_constructible_v<Rcvr> and
    noexcept(impls_for<tag_of_t<Sndr>>::get_state(std::declval<Sndr>(), std::declval<Rcvr &>())) and
    is_nothrow_state_init_v<Sndr, Rcvr>;

// Guaranteed copy elision does not reach [[no_unique_address]] members, so only an
// empty state, the one that gains from overlapping, is declared that way; any other
// state, immovable ones included, is initialized in place from get_state's prvalue.
template <class Sndr, class Rcvr, bool = std::is_empty_v<state_type_t<Sndr, Rcvr>>>
struct basic_state {
    constexpr basic_state(Sndr &&sndr, Rcvr &&rcvr_) noexcept(is_nothrow_basic_state_v<Sndr, Rcvr>)
        : rcvr(static_cast<Rcvr &&>(rcvr_)),
          state(impls_for<tag_of_t<Sndr>>::get_state(static_cast<Sndr &&>(sndr), rcvr)) {}

    Rcvr rcvr;
    state_type_t<Sndr, Rcvr> state;
};

template <class Sndr, class Rcvr>
struct basic_state<Sndr, Rcvr, true> {
    constexpr basic_state(Sndr &&sndr, Rcvr &&rcvr_) noexcept(is_nothrow_basic_state_v<Sndr, Rcvr>)
        : rcvr(static_cast<Rcvr &&>(rcvr_)),
          state(impls_for<tag_of_t<Sndr>>::get_state(static_cast<Sndr &&>(sndr), rcvr)) {}

    Rcvr rcvr;
    LEXEC_NO_UNIQUE_ADDRESS state_type_t<Sndr, Rcvr> state;
};

template <class Sndr, class Rcvr, std::size_t I>
struct basic_receiver {
    using receiver_concept = receiver_t;
    using impl = impls_for<tag_of_t<Sndr>>;
    using index = std::integral_constant<std::size_t, I>;
    using env_type =
        decltype(impl::get_env(index{}, std::declval<state_type_t<Sndr, Rcvr> &>(), std::declval<Rcvr const &>()));

    // Completions are unconstrained: each algorithm validates its channels when it
    // computes its completion signatures, and connect validates those against the
    // outer receiver.
    template <class... Vs>
    constexpr void set_value(Vs &&...vs) && noexcept {
        impl::complete(index{}, op->state, op->rcvr, set_value_t{}, static_cast<Vs &&>(vs)...);
    }

    template <class E>
    constexpr void set_error(E &&e) && noexcept {
        impl::complete(index{}, op->state, op->rcvr, set_error_t{}, static_cast<E &&>(e));
    }

    constexpr void set_stopped() && noexcept { impl::complete(index{}, op->state, op->rcvr, set_stopped_t{}); }

    constexpr env_type get_env() const noexcept {
        return impl::get_env(index{}, op->state, static_cast<Rcvr const &>(op->rcvr));
    }

    basic_state<Sndr, Rcvr> *op;
};

template <class Sndr>
inline constexpr std::size_t connected_child_count_v =
    impls_for<tag_of_t<Sndr>>::connects_children ? remove_cvref_t<Sndr>::child_count : 0;

template <class Sndr, class Rcvr, class Indices = std::make_index_sequence<connected_child_count_v<Sndr>>>
struct basic_operation;

template <class Sndr, class Rcvr, std::size_t... Is>
struct basic_operation<Sndr, Rcvr, std::index_sequence<Is...>> : basic_state<Sndr, Rcvr> {
    using operation_state_concept = operation_state_t;
    using inner_ops_type = tuple<connect_result_t<child_of_t<Sndr, Is>, basic_receiver<Sndr, Rcvr, Is>>...>;

    constexpr basic_operation(Sndr &&sndr, Rcvr &&rcvr_) noexcept(
        std::is_nothrow_constructible_v<basic_state<Sndr, Rcvr>, Sndr, Rcvr> and
        (noexcept(lexec::connect(std::declval<child_of_t<Sndr, Is>>(), std::declval<basic_receiver<Sndr, Rcvr, Is>>())) and
         ...))
        : basic_state<Sndr, Rcvr>(static_cast<Sndr &&>(sndr), static_cast<Rcvr &&>(rcvr_)),
          inner_ops{{lexec::connect(detail::get<Is>(static_cast<Sndr &&>(sndr).children),
                                    basic_receiver<Sndr, Rcvr, Is>{this})}...} {}

    basic_operation(basic_operation &&) = delete;

    constexpr void start() & noexcept {
        inner_ops.apply([this](auto &...ops) noexcept {
            impls_for<tag_of_t<Sndr>>::start(this->state, this->rcvr, ops...);
        });
    }

    LEXEC_NO_UNIQUE_ADDRESS inner_ops_type inner_ops;
};

// The sender type behind every lexec algorithm: an aggregate of the algorithm's data and
// its child senders, decomposable through apply().
template <class Tag, class Data, class... Child>
struct basic_sender {
    using sender_concept = sender_t;
    using tag_type = Tag;
    using data_type = Data;
    using children_type = tuple<Child...>;
    static constexpr std::size_t child_count = sizeof...(Child);

    template <std::size_t I>
    using child_type = std::remove_reference_t<decltype(detail::get<I>(std::declval<children_type &>()))>;

    using attrs_type = decltype(impls_for<Tag>::get_attrs(std::declval<Data const &>(), std::declval<Child const &>()...));

    template <class Self, class... Env>
    static auto get_completion_signatures() -> typename impls_for<Tag>::template completions<Self, Env...>;

    constexpr attrs_type get_env() const noexcept {
        return children.apply(
            [this](Child const &...child) noexcept { return impls_for<Tag>::get_attrs(data, child...); });
    }

    template <class Rcvr>
    constexpr auto connect(Rcvr rcvr) && noexcept(
        std::is_nothrow_constructible_v<basic_operation<basic_sender, Rcvr>, basic_sender, Rcvr>)
        -> basic_operation<basic_sender, Rcvr> {
        return basic_operation<basic_sender, Rcvr>{static_cast<basic_sender &&>(*this), static_cast<Rcvr &&>(rcvr)};
    }

    template <class Rcvr>
    constexpr auto connect(Rcvr rcvr) const & noexcept(
        std::is_nothrow_constructible_v<basic_operation<basic_sender const &, Rcvr>, basic_sender const &, Rcvr>)
        -> basic_operation<basic_sender const &, Rcvr> {
        return basic_operation<basic_sender const &, Rcvr>{*this, static_cast<Rcvr &&>(rcvr)};
    }

    // Invokes fn(Tag{}, data, children...) with the value category of the sender.
    template <class Fn>
    constexpr decltype(auto) apply(Fn &&fn) && {
        return static_cast<children_type &&>(children).apply([this, &fn](Child &&...child) -> decltype(auto) {
            return static_cast<Fn &&>(fn)(Tag{}, static_cast<Data &&>(data), static_cast<Child &&>(child)...);
        });
    }

    template <class Fn>
    constexpr decltype(auto) apply(Fn &&fn) const & {
        return children.apply([this, &fn](Child const &...child) -> decltype(auto) {
            return static_cast<Fn &&>(fn)(Tag{}, data, child...);
        });
    }

    LEXEC_NO_UNIQUE_ADDRESS Data data;
    LEXEC_NO_UNIQUE_ADDRESS children_type children;
};

} // namespace lexec::detail
