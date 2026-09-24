#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <stdexcept>
#include <system_error>

namespace {

// Declares a value completion but completes with set_stopped.
struct stopping_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(int), lexec::set_stopped_t()>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;
        void start() & noexcept { lexec::set_stopped(static_cast<Rcvr &&>(rcvr)); }
        Rcvr rcvr;
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) && noexcept {
        return {static_cast<Rcvr &&>(rcvr)};
    }
};

// Sends whether the receiver's environment names the same scheduler for both queries.
struct env_probe_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(bool)>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;
        void start() & noexcept {
            auto const env = lexec::get_env(rcvr);
            lexec::set_value(static_cast<Rcvr &&>(rcvr),
                             lexec::get_scheduler(env) == lexec::get_delegation_scheduler(env));
        }
        Rcvr rcvr;
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) && noexcept {
        return {static_cast<Rcvr &&>(rcvr)};
    }
};

// Declares a value completion but completes with set_error(E).
template <class E>
struct failing_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(int), lexec::set_error_t(E)>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;
        void start() & noexcept { lexec::set_error(static_cast<Rcvr &&>(rcvr), static_cast<E &&>(error)); }
        Rcvr rcvr;
        E error;
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) && noexcept {
        return {static_cast<Rcvr &&>(rcvr), static_cast<E &&>(error)};
    }

    E error;
};

template <class E>
failing_sender(E) -> failing_sender<E>;

} // namespace

TEST_CASE("sync_wait returns an empty optional when the sender stops") {
    CHECK_FALSE(lexec::sync_wait(stopping_sender{}).has_value());
}

TEST_CASE("sync_wait provides a run_loop scheduler to the sender") {
    auto const result = lexec::sync_wait(env_probe_sender{});
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result));
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("sync_wait rethrows errors") {
    CHECK_THROWS_AS(lexec::sync_wait(failing_sender{std::make_exception_ptr(std::runtime_error{"boom"})}),
                    std::runtime_error);
    CHECK_THROWS_AS(lexec::sync_wait(failing_sender{std::make_error_code(std::errc::timed_out)}), std::system_error);
    CHECK_THROWS_AS(lexec::sync_wait(failing_sender{42}), int);
    CHECK_THROWS_AS(lexec::sync_wait(lexec::just(1) | lexec::then([](int) -> int { throw std::logic_error{"then"}; })),
                    std::logic_error);
}
#endif
