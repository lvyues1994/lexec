#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <variant>

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

#if LEXEC_HAS_EXCEPTIONS
struct throws_on_copy {
    throws_on_copy() = default;
    throws_on_copy(throws_on_copy const &) { throw std::runtime_error{"copy"}; }
};
#endif

using int_or_string_result = std::optional<std::variant<std::tuple<int>, std::tuple<std::string>>>;
static_assert(std::is_same_v<decltype(lexec::sync_wait_with_variant(lexec_test::int_or_string_sender{false})),
                             int_or_string_result>);
static_assert(std::is_same_v<decltype(lexec::sync_wait_with_variant(lexec::just(1, 2.5))),
                             std::optional<std::variant<std::tuple<int, double>>>>);

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

// The result holds a copy of a value sent by reference; the copy's exception is rethrown.
TEST_CASE("sync_wait and sync_wait_with_variant rethrow an exception from storing the value") {
    auto source = throws_on_copy{};
    auto const send_reference = [&source] {
        return lexec::just() | lexec::then([&source]() noexcept -> throws_on_copy & { return source; });
    };
    CHECK_THROWS_AS(lexec::sync_wait(send_reference()), std::runtime_error);
    CHECK_THROWS_AS(lexec::sync_wait_with_variant(send_reference()), std::runtime_error);
}
#endif

TEST_CASE("sync_wait_with_variant returns whichever values the sender completed with") {
    auto const number = lexec::sync_wait_with_variant(lexec_test::int_or_string_sender{false});
    REQUIRE(number.has_value());
    CHECK(std::get<0>(std::get<std::tuple<int>>(*number)) == 7);
    auto const text = lexec::sync_wait_with_variant(lexec_test::int_or_string_sender{true});
    REQUIRE(text.has_value());
    CHECK(std::get<0>(std::get<std::tuple<std::string>>(*text)) == "text");
}

TEST_CASE("sync_wait_with_variant takes a sender with one value completion, or none") {
    auto const pair = lexec::sync_wait_with_variant(lexec::just(1, 2.5));
    REQUIRE(pair.has_value());
    CHECK(std::get<0>(*pair) == std::tuple<int, double>{1, 2.5});
    CHECK_FALSE(lexec::sync_wait_with_variant(lexec::just_stopped()).has_value());
}

TEST_CASE("sync_wait_with_variant returns an empty optional when the sender stops") {
    CHECK_FALSE(lexec::sync_wait_with_variant(stopping_sender{}).has_value());
}

TEST_CASE("sync_wait_with_variant provides a run_loop scheduler to the sender") {
    auto const result = lexec::sync_wait_with_variant(env_probe_sender{});
    REQUIRE(result.has_value());
    CHECK(std::get<0>(std::get<0>(*result)));
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("sync_wait_with_variant rethrows errors") {
    CHECK_THROWS_AS(lexec::sync_wait_with_variant(failing_sender{std::make_error_code(std::errc::timed_out)}),
                    std::system_error);
    CHECK_THROWS_AS(lexec::sync_wait_with_variant(failing_sender{42}), int);
}
#endif
