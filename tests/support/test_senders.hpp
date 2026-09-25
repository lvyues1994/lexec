#pragma once

#include <lexec/execution.hpp>

#include <new>
#include <string>
#include <utility>

namespace lexec_test {

template <class... Fns>
struct overloaded : Fns... {
    using Fns::operator()...;
};

template <class... Fns>
overloaded(Fns...) -> overloaded<Fns...>;

// Completes with set_value(value), or with set_stopped() when `stop` is set.
struct value_or_stopped_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(int), lexec::set_stopped_t()>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;

        void start() & noexcept {
            if (stop) {
                lexec::set_stopped(std::move(rcvr));
            } else {
                lexec::set_value(std::move(rcvr), std::move(value));
            }
        }

        Rcvr rcvr;
        bool stop;
        int value;
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) const noexcept {
        return {std::move(rcvr), stop, value};
    }

    bool stop;
    int value;
};

// Completes on one of two value channels: with a std::string when `send_text` is set,
// with an int otherwise.
struct int_or_string_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures =
        lexec::completion_signatures<lexec::set_value_t(int), lexec::set_value_t(std::string)>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;

        void start() & noexcept {
            if (send_text) {
                lexec::set_value(std::move(rcvr), std::string{"text"});
            } else {
                lexec::set_value(std::move(rcvr), 7);
            }
        }

        Rcvr rcvr;
        bool send_text;
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) const noexcept {
        return {std::move(rcvr), send_text};
    }

    bool send_text;
};

// Completes with set_stopped() once, and only once, stop is requested of it.
struct until_stopped_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_stopped_t()>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;

        struct on_stop {
            void operator()() noexcept { lexec::set_stopped(std::move(op->rcvr)); }

            operation *op;
        };

        using callback_type = lexec::stop_callback_for_t<lexec::stop_token_of_t<lexec::env_of_t<Rcvr>>, on_stop>;

        explicit operation(Rcvr rcvr_) noexcept : rcvr(std::move(rcvr_)) {}
        operation(operation &&) = delete;

        ~operation() {
            if (engaged) {
                std::launder(reinterpret_cast<callback_type *>(storage))->~callback_type();
            }
        }

        // Once registered, the callback may complete and so destroy this operation on
        // another thread; nothing may touch the operation after the construction.
        void start() & noexcept {
            engaged = true;
            ::new (static_cast<void *>(storage)) callback_type(lexec::get_stop_token(lexec::get_env(rcvr)), on_stop{this});
        }

        Rcvr rcvr;
        bool engaged = false;
        alignas(callback_type) unsigned char storage[sizeof(callback_type)];
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) const noexcept {
        return operation<Rcvr>{std::move(rcvr)};
    }
};

// Counts the destruction of each of its operations in *destroyed.
struct tracked_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(int)>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;

        ~operation() { ++*destroyed; }

        void start() & noexcept { lexec::set_value(std::move(rcvr), std::move(value)); }

        Rcvr rcvr;
        int *destroyed;
        int value;
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) const noexcept {
        return {std::move(rcvr), destroyed, value};
    }

    int *destroyed;
    int value;
};

} // namespace lexec_test
