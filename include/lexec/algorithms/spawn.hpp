#pragma once

#include <lexec/algorithms/stop_when.hpp>
#include <lexec/algorithms/write_env.hpp>
#include <lexec/core/completion_signatures.hpp>
#include <lexec/core/completion_tags.hpp>
#include <lexec/core/env.hpp>
#include <lexec/core/operation_state.hpp>
#include <lexec/core/queries.hpp>
#include <lexec/core/receiver.hpp>
#include <lexec/core/sender.hpp>
#include <lexec/detail/config.hpp>
#include <lexec/detail/manual_variant.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/tuple.hpp>
#include <lexec/scopes/counting_scope.hpp>
#include <lexec/stop_token.hpp>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lexec {

namespace detail {

template <class Env>
using env_allocator_t = remove_cvref_t<decltype(get_allocator(std::declval<Env const &>()))>;

// The allocator for the spawned operation and the environment it runs in: the given
// environment's allocator, else the sender's, which then joins the environment, else
// std::allocator.
template <class Env, class Sndr>
auto spawn_allocation(Env const &env, Sndr const &new_sender) {
    if constexpr (is_detected_v<env_allocator_t, Env>) {
        return std::pair{get_allocator(env), env};
    } else if constexpr (is_detected_v<env_allocator_t, env_of_t<Sndr const &>>) {
        auto alloc = get_allocator(lexec::get_env(new_sender));
        return std::pair{alloc, lexec::env{prop{get_allocator, alloc}, env}};
    } else {
        return std::pair{std::allocator<std::byte>{}, env};
    }
}

template <class Alloc, class State>
using rebound_traits = typename std::allocator_traits<Alloc>::template rebind_traits<State>;

// Allocates and constructs a State with the allocator, which the State keeps a copy of.
template <class State, class Alloc, class... Args>
State *allocate_state(Alloc const &alloc, Args &&...args) {
    using traits = rebound_traits<Alloc, State>;
    auto allocator = typename traits::allocator_type(alloc);
    auto *const state = traits::allocate(allocator, 1);
#if LEXEC_HAS_EXCEPTIONS
    try {
        traits::construct(allocator, state, static_cast<Args &&>(args)...);
    } catch (...) {
        traits::deallocate(allocator, state, 1);
        throw;
    }
#else
    traits::construct(allocator, state, static_cast<Args &&>(args)...);
#endif
    return state;
}

template <class State, class Alloc>
void deallocate_state(State *const state, Alloc const &alloc) noexcept {
    using traits = rebound_traits<Alloc, State>;
    auto allocator = typename traits::allocator_type(alloc);
    traits::destroy(allocator, state);
    traits::deallocate(allocator, state, 1);
}

struct spawn_state_base {
    virtual void complete() noexcept = 0;

protected:
    ~spawn_state_base() = default;
};

struct spawn_receiver {
    using receiver_concept = receiver_t;

    void set_value() && noexcept { state->complete(); }
    void set_stopped() && noexcept { state->complete(); }

    spawn_state_base *state;
};

template <class Alloc, class Token, class Sndr>
struct spawn_state final : spawn_state_base {
    using operation = connect_result_t<Sndr, spawn_receiver>;
    using assoc_type = token_association_t<Token>;

    spawn_state(Alloc alloc_, Sndr &&sndr, Token token)
        : alloc(static_cast<Alloc &&>(alloc_)), op(lexec::connect(static_cast<Sndr &&>(sndr), spawn_receiver{this})),
          assoc(token.try_associate()) {}

    void run() noexcept {
        if (assoc) {
            lexec::start(op);
        } else {
            complete();
        }
    }

    // The association ends only once the operation's memory is released.
    void complete() noexcept override {
        auto const kept = static_cast<assoc_type &&>(assoc);
        auto const allocator = alloc;
        deallocate_state(this, allocator);
    }

    Alloc alloc;
    operation op;
    assoc_type assoc;
};

template <class Sigs>
struct spawnable_completions : std::false_type {};

template <class... Sigs>
struct spawnable_completions<completion_signatures<Sigs...>>
    : std::bool_constant<((is_one_of_v<Sigs, set_value_t(), set_stopped_t()>) and ...)> {};

// ---- spawn_future ----

template <class Sig>
struct future_result_of;

template <class Tag, class... Args>
struct future_result_of<Tag(Args...)> {
    using type = tuple<Tag, std::decay_t<Args>...>;
};

template <class Sigs>
struct future_results;

template <class... Sigs>
struct future_results<completion_signatures<Sigs...>> {
    static constexpr bool nothrow =
        gather_signatures_t<set_value_t, completion_signatures<Sigs...>, is_nothrow_decay_copyable_t, all_of_t>::value and
        gather_signatures_t<set_error_t, completion_signatures<Sigs...>, is_nothrow_decay_copyable_t, all_of_t>::value;
    using type = rename_t<
        unique_t<type_list<tuple<set_stopped_t>, typename future_result_of<Sigs>::type...,
                           std::conditional_t<nothrow or not LEXEC_HAS_EXCEPTIONS, tuple<set_stopped_t>,
                                              tuple<set_error_t, std::exception_ptr>>>>,
        manual_variant>;
};

template <class Sigs>
struct future_consumer {
    void (*deliver)(future_consumer *, typename future_results<Sigs>::type &) noexcept;
    void (*stop)(future_consumer *) noexcept;
};

template <class Sigs>
struct spawn_future_state_base {
    using results_type = typename future_results<Sigs>::type;

    enum : unsigned char { completed = 1, consumed = 2, stopped = 4, abandoned = 8 };

    // The spawned operation has finished and the result is stored.
    void complete() noexcept {
        auto const previous = flags.fetch_or(completed, std::memory_order_acq_rel);
        if ((previous & abandoned) != 0) {
            destroy();
        } else if ((previous & consumed) != 0) {
            if ((previous & stopped) == 0) {
                consumer->deliver(consumer, result);
            }
            destroy();
        }
    }

    // After this, *this is not touched unless the result is delivered here.
    void consume(future_consumer<Sigs> &waiting) noexcept {
        consumer = &waiting;
        auto const previous = flags.fetch_or(consumed, std::memory_order_acq_rel);
        if ((previous & completed) != 0) {
            waiting.deliver(&waiting, result);
            destroy();
        } else if ((previous & stopped) != 0) {
            waiting.stop(&waiting);
        }
    }

    // A stop request from the consumer: true if the consumer, registered and not yet
    // given a result, is to complete with stopped itself.
    bool try_cancel() noexcept {
        request_stop();
        auto const previous = flags.fetch_or(stopped, std::memory_order_acq_rel);
        return (previous & consumed) != 0 and (previous & completed) == 0;
    }

    // The future was dropped: the operation is asked to stop and cleans up on finishing.
    void abandon() noexcept {
        request_stop();
        if ((flags.fetch_or(abandoned, std::memory_order_acq_rel) & completed) != 0) {
            destroy();
        }
    }

    results_type result;

protected:
    ~spawn_future_state_base() = default;

    virtual void destroy() noexcept = 0;
    virtual void request_stop() noexcept = 0;

private:
    std::atomic<unsigned char> flags{0};
    future_consumer<Sigs> *consumer = nullptr;
};

template <class Sigs>
struct spawn_future_receiver {
    using receiver_concept = receiver_t;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        set_complete<set_value_t>(static_cast<Vs &&>(vs)...);
    }

    template <class E>
    void set_error(E &&e) && noexcept {
        set_complete<set_error_t>(static_cast<E &&>(e));
    }

    void set_stopped() && noexcept { set_complete<set_stopped_t>(); }

    template <class Tag, class... Vs>
    void set_complete(Vs &&...vs) noexcept {
        using stored = tuple<Tag, std::decay_t<Vs>...>;
        auto const store = [&] { state->result.template emplace_with<stored>([&] { return stored{{Tag{}}, {static_cast<Vs &&>(vs)}...}; }); };
        if constexpr (is_nothrow_decay_copyable_t<Vs...>::value or not LEXEC_HAS_EXCEPTIONS) {
            store();
        } else {
#if LEXEC_HAS_EXCEPTIONS
            try {
                store();
            } catch (...) {
                using failed = tuple<set_error_t, std::exception_ptr>;
                state->result.template emplace_with<failed>([] { return failed{{set_error_t{}}, {std::current_exception()}}; });
            }
#endif
        }
        state->complete();
    }

    spawn_future_state_base<Sigs> *state;
};

template <class Sndr, class Env>
using future_spawned_sender_t =
    decltype(write_env(stop_when(std::declval<Sndr>(), std::declval<inplace_stop_token>()), std::declval<Env>()));

template <class Sndr, class Env>
using future_sigs_t = completion_signatures_of_t<future_spawned_sender_t<Sndr, Env>, lexec::env<>>;

template <class Alloc, class Token, class Sndr, class Env>
struct spawn_future_state final : spawn_future_state_base<future_sigs_t<Sndr, Env>> {
    using sigs = future_sigs_t<Sndr, Env>;
    using receiver = spawn_future_receiver<sigs>;
    using operation = connect_result_t<future_spawned_sender_t<Sndr, Env>, receiver>;
    using assoc_type = token_association_t<Token>;

    spawn_future_state(Alloc alloc_, Sndr &&sndr, Token token, Env env)
        : alloc(static_cast<Alloc &&>(alloc_)),
          op(lexec::connect(write_env(stop_when(static_cast<Sndr &&>(sndr), source.get_token()), static_cast<Env &&>(env)),
                            receiver{this})),
          assoc(token.try_associate()) {}

    void run() noexcept {
        if (assoc) {
            lexec::start(op);
        } else {
            receiver{this}.set_stopped();
        }
    }

private:
    void request_stop() noexcept override { source.request_stop(); }

    void destroy() noexcept override {
        auto const kept = static_cast<assoc_type &&>(assoc);
        auto const allocator = alloc;
        deallocate_state(this, allocator);
    }

    Alloc alloc;
    inplace_stop_source source;
    operation op;
    assoc_type assoc;
};

template <class State>
struct abandon_state {
    void operator()(State *const state) const noexcept { state->abandon(); }
};

template <class Sigs, class Rcvr>
struct future_operation final : future_consumer<Sigs> {
    using operation_state_concept = operation_state_t;
    using state_type = spawn_future_state_base<Sigs>;

    struct cancel {
        void operator()() noexcept {
            if (op->state->try_cancel()) {
                op->finish_stopped();
            }
        }

        future_operation *op;
    };

    using stop_token_type = stop_token_of_t<env_of_t<Rcvr>>;
    using stop_callback_type = stop_callback_for_t<stop_token_type, cancel>;

    template <class Owned>
    future_operation(Owned &&owned, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : future_consumer<Sigs>{&deliver_result, &deliver_stopped}, rcvr(static_cast<Rcvr &&>(rcvr_)),
          pending(static_cast<Owned &&>(owned)) {}

    future_operation(future_operation &&) = delete;

    ~future_operation() { drop_callback(); }

    void start() & noexcept {
        state = pending.get();
        if constexpr (not is_unstoppable_token_v<stop_token_type>) {
            ::new (static_cast<void *>(callback_storage)) stop_callback_type(lexec::get_stop_token(lexec::get_env(rcvr)), cancel{this});
            has_callback = true;
        }
        pending.release();
        state->consume(*this);
    }

    void finish_stopped() noexcept {
        drop_callback();
        lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
    }

private:
    void drop_callback() noexcept {
        if constexpr (not is_unstoppable_token_v<stop_token_type>) {
            if (has_callback) {
                has_callback = false;
                std::launder(reinterpret_cast<stop_callback_type *>(callback_storage))->~stop_callback_type();
            }
        }
    }

    static void deliver_result(future_consumer<Sigs> *const consumer, typename state_type::results_type &results) noexcept {
        auto &self = *static_cast<future_operation *>(consumer);
        self.drop_callback();
        results.visit([&self](auto &result) noexcept {
            result.apply([&self](auto &tag, auto &...values) noexcept {
                tag(static_cast<Rcvr &&>(self.rcvr), static_cast<std::remove_reference_t<decltype(values)> &&>(values)...);
            });
        });
    }

    static void deliver_stopped(future_consumer<Sigs> *const consumer) noexcept {
        static_cast<future_operation *>(consumer)->finish_stopped();
    }

    Rcvr rcvr;
    std::unique_ptr<state_type, abandon_state<state_type>> pending;
    state_type *state = nullptr;
    alignas(stop_callback_type) unsigned char callback_storage[sizeof(stop_callback_type)];
    bool has_callback = false;
};

template <class Sigs>
struct future_completions {
    static constexpr bool nothrow = future_results<Sigs>::nothrow;
    using type = transform_completion_signatures<
        Sigs, concat_t<completion_signatures<set_stopped_t()>, eptr_completion_if_t<not nothrow>>, decayed_set_value,
        decayed_set_error>;
};

// The result of spawn_future: completes with the spawned operation's result, moved out,
// or with set_stopped. Dropping it without starting it asks the operation to stop.
template <class Sigs>
struct future_sender {
    using sender_concept = sender_t;
    using completion_signatures = typename future_completions<Sigs>::type;
    using state_type = spawn_future_state_base<Sigs>;

    template <class Rcvr>
    future_operation<Sigs, Rcvr> connect(Rcvr rcvr) && noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {static_cast<std::unique_ptr<state_type, abandon_state<state_type>> &&>(state), static_cast<Rcvr &&>(rcvr)};
    }

    std::unique_ptr<state_type, abandon_state<state_type>> state;
};

} // namespace detail

// Starts a sender eagerly within an async scope; the operation is allocated and cleans
// up after itself. If the scope accepts no more associations, the sender never starts.
// The sender may complete only with set_value() or set_stopped().
struct spawn_t {
    template <class Sndr, class Token, class Env = env<>,
              std::enable_if_t<is_sender_v<Sndr> and is_scope_token_v<detail::remove_cvref_t<Token>>, int> = 0>
    void operator()(Sndr &&sndr, Token &&token, Env &&env = {}) const {
        auto const &tkn = token;
        auto &&wrapped = tkn.wrap(static_cast<Sndr &&>(sndr));
        auto [alloc, senv] = detail::spawn_allocation(env, wrapped);
        using spawned = decltype(write_env(static_cast<decltype(wrapped) &&>(wrapped), senv));
        static_assert(detail::spawnable_completions<completion_signatures_of_t<spawned, lexec::env<>>>::value,
                      "lexec::spawn: the sender may complete only with set_value() or set_stopped()");
        using state = detail::spawn_state<decltype(alloc), detail::remove_cvref_t<Token>, spawned>;
        detail::allocate_state<state>(alloc, alloc, write_env(static_cast<decltype(wrapped) &&>(wrapped), senv), tkn)->run();
    }
};

// Starts a sender eagerly within an async scope and returns a sender of its result.
struct spawn_future_t {
    template <class Sndr, class Token, class Env = env<>,
              std::enable_if_t<is_sender_v<Sndr> and is_scope_token_v<detail::remove_cvref_t<Token>>, int> = 0>
    auto operator()(Sndr &&sndr, Token &&token, Env &&env = {}) const {
        auto const &tkn = token;
        auto &&wrapped = tkn.wrap(static_cast<Sndr &&>(sndr));
        auto [alloc, senv] = detail::spawn_allocation(env, wrapped);
        using wrapped_type = decltype(wrapped);
        using state = detail::spawn_future_state<decltype(alloc), detail::remove_cvref_t<Token>, wrapped_type, decltype(senv)>;
        auto *const created = detail::allocate_state<state>(alloc, alloc, static_cast<wrapped_type &&>(wrapped), tkn, senv);
        using sigs = typename state::sigs;
        auto future = detail::future_sender<sigs>{
            std::unique_ptr<detail::spawn_future_state_base<sigs>,
                            detail::abandon_state<detail::spawn_future_state_base<sigs>>>{created}};
        created->run();
        return future;
    }
};

inline constexpr spawn_t spawn{};
inline constexpr spawn_future_t spawn_future{};

} // namespace lexec
