#pragma once

#include <lexec/algorithms/write_env.hpp>
#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/stop_token.hpp>

#include <new>
#include <type_traits>

namespace lexec::detail {

// STOP-WHEN: the sender sees stop requests from an extra inplace_stop_token as well as
// from its receiver's token. Against a receiver that cannot stop, the extra token simply
// becomes the sender's; otherwise the operation holds a stop source that both tokens
// request stop on.

template <class Sndr>
using stop_when_child_t = decltype(write_env(std::declval<Sndr>(), prop{get_stop_token, inplace_stop_token{}}));

template <class Sndr, class Rcvr>
struct stop_when_operation;

template <class Sndr, class Rcvr>
struct stop_when_receiver {
    using receiver_concept = receiver_t;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        op->release_callbacks();
        lexec::set_value(static_cast<Rcvr &&>(op->rcvr), static_cast<Vs &&>(vs)...);
    }

    template <class E>
    void set_error(E &&e) && noexcept {
        op->release_callbacks();
        lexec::set_error(static_cast<Rcvr &&>(op->rcvr), static_cast<E &&>(e));
    }

    void set_stopped() && noexcept {
        op->release_callbacks();
        lexec::set_stopped(static_cast<Rcvr &&>(op->rcvr));
    }

    env_of_t<Rcvr> get_env() const noexcept { return lexec::get_env(op->rcvr); }

    stop_when_operation<Sndr, Rcvr> *op;
};

template <class Sndr, class Rcvr>
struct stop_when_operation {
    using operation_state_concept = operation_state_t;
    using receiver_token = stop_token_of_t<env_of_t<Rcvr>>;

    struct forward_stop {
        void operator()() noexcept { source->request_stop(); }

        inplace_stop_source *source;
    };

    using extra_callback = inplace_stop_callback<forward_stop>;
    using receiver_callback = stop_callback_for_t<receiver_token, forward_stop>;
    using child_operation = connect_result_t<stop_when_child_t<Sndr>, stop_when_receiver<Sndr, Rcvr>>;

    stop_when_operation(Sndr &&sndr, inplace_stop_token const extra_, Rcvr &&rcvr_) noexcept(
        std::is_nothrow_move_constructible_v<Rcvr> and
        noexcept(lexec::connect(std::declval<stop_when_child_t<Sndr>>(), std::declval<stop_when_receiver<Sndr, Rcvr>>())))
        : rcvr(static_cast<Rcvr &&>(rcvr_)), extra(extra_),
          child(lexec::connect(write_env(static_cast<Sndr &&>(sndr), prop{get_stop_token, source.get_token()}),
                               stop_when_receiver<Sndr, Rcvr>{this})) {}

    stop_when_operation(stop_when_operation &&) = delete;

    ~stop_when_operation() { release_callbacks(); }

    void start() & noexcept {
        ::new (static_cast<void *>(extra_storage)) extra_callback(extra, forward_stop{&source});
        ::new (static_cast<void *>(receiver_storage))
            receiver_callback(lexec::get_stop_token(lexec::get_env(rcvr)), forward_stop{&source});
        registered = true;
        lexec::start(child);
    }

    // Both callbacks go before the receiver completes.
    void release_callbacks() noexcept {
        if (registered) {
            registered = false;
            std::launder(reinterpret_cast<receiver_callback *>(receiver_storage))->~receiver_callback();
            std::launder(reinterpret_cast<extra_callback *>(extra_storage))->~extra_callback();
        }
    }

    Rcvr rcvr;
    inplace_stop_token extra;
    inplace_stop_source source;
    alignas(extra_callback) unsigned char extra_storage[sizeof(extra_callback)];
    alignas(receiver_callback) unsigned char receiver_storage[sizeof(receiver_callback)];
    bool registered = false;
    child_operation child;
};

template <class Sndr>
struct stop_when_sender {
    using sender_concept = sender_t;

    template <class Self, class... Env>
    static auto get_completion_signatures()
        -> completion_signatures_of_t<stop_when_child_t<member_like_t<Self, Sndr>>, Env...>;

    template <class Rcvr>
    auto connect(Rcvr rcvr) && {
        if constexpr (is_unstoppable_token_v<stop_token_of_t<env_of_t<Rcvr>>>) {
            return lexec::connect(write_env(static_cast<Sndr &&>(sndr), prop{get_stop_token, extra}), static_cast<Rcvr &&>(rcvr));
        } else {
            return stop_when_operation<Sndr, Rcvr>{static_cast<Sndr &&>(sndr), extra, static_cast<Rcvr &&>(rcvr)};
        }
    }

    template <class Rcvr>
    auto connect(Rcvr rcvr) const & {
        if constexpr (is_unstoppable_token_v<stop_token_of_t<env_of_t<Rcvr>>>) {
            return lexec::connect(write_env(sndr, prop{get_stop_token, extra}), static_cast<Rcvr &&>(rcvr));
        } else {
            return stop_when_operation<Sndr const &, Rcvr>{sndr, extra, static_cast<Rcvr &&>(rcvr)};
        }
    }

    decltype(auto) get_env() const noexcept { return make_fwd_env(lexec::get_env(sndr)); }

    Sndr sndr;
    inplace_stop_token extra;
};

template <class Sndr>
stop_when_sender<std::decay_t<Sndr>> stop_when(Sndr &&sndr, inplace_stop_token const extra) noexcept(
    std::is_nothrow_constructible_v<std::decay_t<Sndr>, Sndr>) {
    return {static_cast<Sndr &&>(sndr), extra};
}

} // namespace lexec::detail
