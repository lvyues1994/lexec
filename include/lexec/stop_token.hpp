#pragma once

#include <lexec/detail/config.hpp>
#include <lexec/detail/meta.hpp>
#include <lexec/detail/spin_wait.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>

namespace lexec {

struct never_stop_token {
private:
    struct callback_type_impl {
        template <class Fn>
        constexpr explicit callback_type_impl(never_stop_token, Fn &&) noexcept {}
    };

public:
    template <class>
    using callback_type = callback_type_impl;

    static constexpr bool stop_requested() noexcept { return false; }
    static constexpr bool stop_possible() noexcept { return false; }

    friend constexpr bool operator==(never_stop_token, never_stop_token) noexcept { return true; }
    friend constexpr bool operator!=(never_stop_token, never_stop_token) noexcept { return false; }
};

struct inplace_stop_source;
struct inplace_stop_token;

template <class Fn>
struct inplace_stop_callback;

namespace detail {

struct inplace_stop_callback_base {
    void execute() noexcept { execute_fn(this); }

protected:
    using execute_fn_t = void(inplace_stop_callback_base *) noexcept;

    explicit inplace_stop_callback_base(inplace_stop_source const *source_, execute_fn_t *execute_fn_) noexcept
        : source(source_), execute_fn(execute_fn_) {}

    void register_callback() noexcept;

    friend inplace_stop_source;

    inplace_stop_source const *source;
    execute_fn_t *execute_fn;
    inplace_stop_callback_base *next = nullptr;
    inplace_stop_callback_base **prev_ptr = nullptr;
    bool *removed_during_callback = nullptr;
    std::atomic<bool> callback_completed{false};
};

} // namespace detail

// A callback may destroy the source whose request_stop() is running it; request_stop()
// then returns without touching the source again. Operations that forward stop requests
// to a source of their own rely on this: their children may complete them, and so have
// them destroyed, inside the forwarded request.
struct inplace_stop_source {
    inplace_stop_source() noexcept = default;
    inplace_stop_source(inplace_stop_source &&) = delete;
    ~inplace_stop_source() {
        assert(callbacks == nullptr and "inplace_stop_source destroyed with registered callbacks");
        if (destroyed_during_callback != nullptr) {
            *destroyed_during_callback = true;
        }
    }

    inplace_stop_token get_token() const noexcept;

    // Returns true only for the call that transitions the source to the stop-requested state.
    bool request_stop() noexcept;

    static constexpr bool stop_possible() noexcept { return true; }
    bool stop_requested() const noexcept { return (state.load(std::memory_order_acquire) & kStopRequestedFlag) != 0; }

private:
    friend detail::inplace_stop_callback_base;
    template <class>
    friend struct inplace_stop_callback;

    std::uint8_t lock_state() const noexcept;
    void unlock_state(std::uint8_t old_state) const noexcept;
    bool try_lock_unless_stop_requested(bool set_stop_requested) const noexcept;
    bool try_add_callback(detail::inplace_stop_callback_base *callback) const noexcept;
    void remove_callback(detail::inplace_stop_callback_base *callback) const noexcept;

    static constexpr std::uint8_t kStopRequestedFlag = 1;
    static constexpr std::uint8_t kLockedFlag = 2;

    // `callbacks` and `notifying_thread` are only accessed while kLockedFlag is held.
    mutable std::atomic<std::uint8_t> state{0};
    mutable detail::inplace_stop_callback_base *callbacks = nullptr;
    std::thread::id notifying_thread;
    // Set while request_stop() runs callbacks, on whose thread alone the source may go.
    bool *destroyed_during_callback = nullptr;
};

struct inplace_stop_token {
    template <class Fn>
    using callback_type = inplace_stop_callback<Fn>;

    inplace_stop_token() noexcept = default;

    bool stop_requested() const noexcept { return source != nullptr and source->stop_requested(); }
    bool stop_possible() const noexcept { return source != nullptr; }

    void swap(inplace_stop_token &other) noexcept { std::swap(source, other.source); }

    friend bool operator==(inplace_stop_token lhs, inplace_stop_token rhs) noexcept { return lhs.source == rhs.source; }
    friend bool operator!=(inplace_stop_token lhs, inplace_stop_token rhs) noexcept { return lhs.source != rhs.source; }

private:
    friend inplace_stop_source;
    template <class>
    friend struct inplace_stop_callback;

    explicit inplace_stop_token(inplace_stop_source const *source_) noexcept : source(source_) {}

    inplace_stop_source const *source = nullptr;
};

template <class Fn>
struct inplace_stop_callback : private detail::inplace_stop_callback_base {
    static_assert(std::is_invocable_v<Fn>, "inplace_stop_callback requires a callable invocable with no arguments");

    using callback_type = Fn;

    template <class Init, std::enable_if_t<std::is_constructible_v<Fn, Init>, int> = 0>
    explicit inplace_stop_callback(inplace_stop_token const token, Init &&init) noexcept(
        std::is_nothrow_constructible_v<Fn, Init>)
        : inplace_stop_callback_base(token.source, &execute_impl), fn(static_cast<Init &&>(init)) {
        register_callback();
    }

    inplace_stop_callback(inplace_stop_callback &&) = delete;

    // Waits for the callback to finish if another thread is executing it right now.
    ~inplace_stop_callback() {
        if (source != nullptr) {
            source->remove_callback(this);
        }
    }

private:
    static void execute_impl(inplace_stop_callback_base *base) noexcept {
        static_cast<Fn &&>(static_cast<inplace_stop_callback *>(base)->fn)();
    }

    LEXEC_NO_UNIQUE_ADDRESS Fn fn;
};

template <class Fn>
inplace_stop_callback(inplace_stop_token, Fn) -> inplace_stop_callback<Fn>;

template <class Token, class Fn>
using stop_callback_for_t = typename Token::template callback_type<Fn>;

namespace detail {

struct probe_callback {
    void operator()() noexcept {}
};

template <class Token>
using stop_callback_probe_t = stop_callback_for_t<Token, probe_callback>;

template <class Token>
using stop_queries_probe_t = std::enable_if_t<noexcept(std::declval<Token const &>().stop_requested()) and
                                              noexcept(std::declval<Token const &>().stop_possible()) and
                                              std::is_convertible_v<decltype(std::declval<Token const &>() ==
                                                                             std::declval<Token const &>()),
                                                                    bool>>;

template <class Token>
using stop_impossible_probe_t = std::enable_if_t<not Token::stop_possible()>;

} // namespace detail

template <class Token>
inline constexpr bool is_stoppable_token_v = detail::is_detected_v<detail::stop_callback_probe_t, Token> and
                                             detail::is_detected_v<detail::stop_queries_probe_t, Token> and
                                             std::is_nothrow_copy_constructible_v<Token>;

template <class Token>
inline constexpr bool is_unstoppable_token_v =
    is_stoppable_token_v<Token> and detail::is_detected_v<detail::stop_impossible_probe_t, Token>;

inline inplace_stop_token inplace_stop_source::get_token() const noexcept { return inplace_stop_token{this}; }

inline bool inplace_stop_source::request_stop() noexcept {
    if (not try_lock_unless_stop_requested(true)) {
        return false;
    }
    notifying_thread = std::this_thread::get_id();
    auto destroyed = false;
    destroyed_during_callback = &destroyed;
    while (callbacks != nullptr) {
        auto *const callback = callbacks;
        callback->prev_ptr = nullptr;
        callbacks = callback->next;
        if (callbacks != nullptr) {
            callbacks->prev_ptr = &callbacks;
        }
        state.store(kStopRequestedFlag, std::memory_order_release);

        auto removed_during_callback = false;
        callback->removed_during_callback = &removed_during_callback;
        callback->execute();
        if (destroyed) {
            return true;
        }
        if (not removed_during_callback) {
            callback->removed_during_callback = nullptr;
            callback->callback_completed.store(true, std::memory_order_release);
        }
        lock_state();
    }
    destroyed_during_callback = nullptr;
    state.store(kStopRequestedFlag, std::memory_order_release);
    return true;
}

inline std::uint8_t inplace_stop_source::lock_state() const noexcept {
    auto spin = detail::spin_wait{};
    auto old_state = state.load(std::memory_order_relaxed);
    do {
        while ((old_state & kLockedFlag) != 0) {
            spin.wait();
            old_state = state.load(std::memory_order_relaxed);
        }
    } while (not state.compare_exchange_weak(old_state, static_cast<std::uint8_t>(old_state | kLockedFlag),
                                             std::memory_order_acquire, std::memory_order_relaxed));
    return old_state;
}

inline void inplace_stop_source::unlock_state(std::uint8_t const old_state) const noexcept {
    state.store(old_state, std::memory_order_release);
}

inline bool inplace_stop_source::try_lock_unless_stop_requested(bool const set_stop_requested) const noexcept {
    auto spin = detail::spin_wait{};
    auto old_state = state.load(std::memory_order_relaxed);
    auto const new_state = set_stop_requested ? static_cast<std::uint8_t>(kLockedFlag | kStopRequestedFlag) : kLockedFlag;
    do {
        while (old_state != 0) {
            if ((old_state & kStopRequestedFlag) != 0) {
                return false;
            }
            spin.wait();
            old_state = state.load(std::memory_order_relaxed);
        }
    } while (not state.compare_exchange_weak(old_state, new_state, std::memory_order_acq_rel, std::memory_order_relaxed));
    return true;
}

inline bool inplace_stop_source::try_add_callback(detail::inplace_stop_callback_base *const callback) const noexcept {
    if (not try_lock_unless_stop_requested(false)) {
        return false;
    }
    callback->next = callbacks;
    callback->prev_ptr = &callbacks;
    if (callbacks != nullptr) {
        callbacks->prev_ptr = &callback->next;
    }
    callbacks = callback;
    unlock_state(0);
    return true;
}

inline void inplace_stop_source::remove_callback(detail::inplace_stop_callback_base *const callback) const noexcept {
    auto const old_state = lock_state();
    if (callback->prev_ptr != nullptr) {
        *callback->prev_ptr = callback->next;
        if (callback->next != nullptr) {
            callback->next->prev_ptr = callback->prev_ptr;
        }
        unlock_state(old_state);
        return;
    }
    auto const notifier = notifying_thread;
    unlock_state(old_state);
    if (std::this_thread::get_id() == notifier) {
        if (callback->removed_during_callback != nullptr) {
            *callback->removed_during_callback = true;
        }
        return;
    }
    auto spin = detail::spin_wait{};
    while (not callback->callback_completed.load(std::memory_order_acquire)) {
        spin.wait();
    }
}

inline void detail::inplace_stop_callback_base::register_callback() noexcept {
    if (source != nullptr and not source->try_add_callback(this)) {
        source = nullptr;
        execute();
    }
}

} // namespace lexec
