#pragma once

#include <lexec/execution.hpp>

namespace lexec_test {

// Hand-written then over a child that sends one int: the oracle that the framework's
// then must match in object size and generated code.
template <class Child, class Fn, class Rcvr>
struct reference_then_operation {
    struct child_receiver {
        using receiver_concept = lexec::receiver_t;

        void set_value(int const v) && noexcept {
            lexec::set_value(static_cast<Rcvr &&>(op->rcvr), static_cast<Fn &&>(op->fn)(v));
        }

        lexec::env_of_t<Rcvr> get_env() const noexcept { return lexec::get_env(op->rcvr); }

        reference_then_operation *op;
    };

    using operation_state_concept = lexec::operation_state_t;

    reference_then_operation(Child &&child_, Fn &&fn_, Rcvr &&rcvr_) noexcept
        : rcvr(static_cast<Rcvr &&>(rcvr_)), fn(static_cast<Fn &&>(fn_)),
          child(lexec::connect(static_cast<Child &&>(child_), child_receiver{this})) {}

    reference_then_operation(reference_then_operation &&) = delete;

    void start() & noexcept { lexec::start(child); }

    Rcvr rcvr;
    LEXEC_NO_UNIQUE_ADDRESS Fn fn;
    lexec::connect_result_t<Child, child_receiver> child;
};

template <class Child, class Fn>
struct reference_then_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(int)>;

    template <class Rcvr>
    reference_then_operation<Child, Fn, Rcvr> connect(Rcvr rcvr) && noexcept {
        return {static_cast<Child &&>(child), static_cast<Fn &&>(fn), static_cast<Rcvr &&>(rcvr)};
    }

    Child child;
    Fn fn;
};

struct int_sink {
    using receiver_concept = lexec::receiver_t;

    void set_value(int const v) && noexcept { *out = v; }

    int *out;
};

inline constexpr auto triple_plus_one = [](int const v) noexcept { return v * 3 + 1; };

using triple_plus_one_t = std::remove_const_t<decltype(triple_plus_one)>;

} // namespace lexec_test
