#pragma once

#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/framework/sender_adaptor_closure.hpp>
#include <lexec/scopes/counting_scope.hpp>

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lexec {

struct associate_t;

namespace detail {

template <class Token, class Sndr>
using wrap_sender_t = remove_cvref_t<decltype(std::declval<Token &>().wrap(std::declval<Sndr>()))>;

template <class Assoc, class WrapSender, class Rcvr>
struct associate_operation {
    using operation_state_concept = operation_state_t;
    using child_operation = connect_result_t<WrapSender, Rcvr>;

    template <class S>
    associate_operation(Assoc &&assoc_, S *const sndr, Rcvr &&rcvr) : assoc(static_cast<Assoc &&>(assoc_)) {
        if (assoc) {
            ::new (static_cast<void *>(storage)) child_operation(lexec::connect(static_cast<S &&>(*sndr), static_cast<Rcvr &&>(rcvr)));
        } else {
            ::new (static_cast<void *>(storage)) Rcvr(static_cast<Rcvr &&>(rcvr));
        }
    }

    associate_operation(associate_operation &&) = delete;

    // The operation goes before the association, which it keeps for its whole life.
    ~associate_operation() {
        if (assoc) {
            std::launder(reinterpret_cast<child_operation *>(storage))->~child_operation();
        } else {
            std::launder(reinterpret_cast<Rcvr *>(storage))->~Rcvr();
        }
    }

    void start() & noexcept {
        if (assoc) {
            lexec::start(*std::launder(reinterpret_cast<child_operation *>(storage)));
        } else {
            lexec::set_stopped(static_cast<Rcvr &&>(*std::launder(reinterpret_cast<Rcvr *>(storage))));
        }
    }

    Assoc assoc;
    alignas(child_operation) alignas(Rcvr) unsigned char storage[sizeof(child_operation) > sizeof(Rcvr) ? sizeof(child_operation)
                                                                                                    : sizeof(Rcvr)];
};

// ASSOCIATE-DATA: the wrapped sender exists only while the association does.
template <class Token, class Sndr>
struct associate_sender {
    using sender_concept = sender_t;
    using wrap_sender = wrap_sender_t<Token, Sndr>;
    using assoc_type = token_association_t<Token>;

    template <class Self, class... Env>
    static auto get_completion_signatures()
        -> unique_t<concat_t<completion_signatures_of_t<member_like_t<Self, wrap_sender>, fwd_env_t<Env>...>,
                             completion_signatures<set_stopped_t()>>>;

    associate_sender(Token token, Sndr &&sndr) {
        ::new (static_cast<void *>(storage)) wrap_sender(token.wrap(static_cast<Sndr &&>(sndr)));
        assoc = token.try_associate();
        if (not assoc) {
            sender().~wrap_sender();
        }
    }

    associate_sender(associate_sender const &other) noexcept(std::is_nothrow_copy_constructible_v<wrap_sender>)
        : assoc(other.assoc.try_associate()) {
        if (assoc) {
            ::new (static_cast<void *>(storage)) wrap_sender(other.sender());
        }
    }

    associate_sender(associate_sender &&other) noexcept(std::is_nothrow_move_constructible_v<wrap_sender>)
        : assoc(static_cast<assoc_type &&>(other.assoc)) {
        if (assoc) {
            ::new (static_cast<void *>(storage)) wrap_sender(static_cast<wrap_sender &&>(other.sender()));
            other.sender().~wrap_sender();
        }
    }

    associate_sender &operator=(associate_sender const &) = delete;
    associate_sender &operator=(associate_sender &&) = delete;

    ~associate_sender() {
        if (assoc) {
            sender().~wrap_sender();
        }
    }

    template <class Rcvr>
    auto connect(Rcvr rcvr) && -> associate_operation<assoc_type, wrap_sender, Rcvr> {
        // The association moves into the operation; the sender, connected or not, ends here.
        auto const release = [](wrap_sender *s) noexcept {
            if (s != nullptr) {
                s->~wrap_sender();
            }
        };
        auto const owned = std::unique_ptr<wrap_sender, decltype(release)>{assoc ? &sender() : nullptr, release};
        return {static_cast<assoc_type &&>(assoc), owned.get(), static_cast<Rcvr &&>(rcvr)};
    }

    template <class Rcvr, class W = wrap_sender, std::enable_if_t<std::is_copy_constructible_v<W>, int> = 0>
    auto connect(Rcvr rcvr) const & -> associate_operation<assoc_type, wrap_sender, Rcvr> {
        return associate_sender(*this).connect(static_cast<Rcvr &&>(rcvr));
    }

private:
    wrap_sender &sender() noexcept { return *std::launder(reinterpret_cast<wrap_sender *>(storage)); }
    wrap_sender const &sender() const noexcept { return *std::launder(reinterpret_cast<wrap_sender const *>(storage)); }

    assoc_type assoc;
    alignas(wrap_sender) unsigned char storage[sizeof(wrap_sender)];
};

} // namespace detail

// Associates a sender with an async scope for the life of the operation it becomes; if
// the scope accepts no more associations, the operation completes with set_stopped.
struct associate_t {
    template <class Sndr, class Token,
              std::enable_if_t<is_sender_v<Sndr> and is_scope_token_v<detail::remove_cvref_t<Token>>, int> = 0>
    auto operator()(Sndr &&sndr, Token &&token) const
        -> detail::associate_sender<detail::remove_cvref_t<Token>, Sndr> {
        return {static_cast<Token &&>(token), static_cast<Sndr &&>(sndr)};
    }

    template <class Token, std::enable_if_t<is_scope_token_v<detail::remove_cvref_t<Token>>, int> = 0>
    auto operator()(Token &&token) const -> detail::partial_closure<associate_t, detail::remove_cvref_t<Token>> {
        return detail::make_partial_closure<associate_t>(static_cast<Token &&>(token));
    }
};

inline constexpr associate_t associate{};

} // namespace lexec
