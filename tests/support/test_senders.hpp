#pragma once

#include <lexec/execution.hpp>

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
