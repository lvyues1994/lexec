#include "../support/test_receivers.hpp"
#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

struct throwing_move {
    throwing_move() = default;
    throwing_move(throwing_move &&) {}
};

[[maybe_unused]] auto const double_it = [](int &v) noexcept { return lexec::just(v * 2.0); };
[[maybe_unused]] auto const throwing_double = [](int &v) { return lexec::just(v * 2.0); };
[[maybe_unused]] auto const recover = [](int &e) noexcept { return lexec::just(e); };
[[maybe_unused]] auto const read_scheduler = []() noexcept { return lexec::read_env(lexec::get_scheduler); };
[[maybe_unused]] auto const just_throwing_move = [](int &) noexcept { return lexec::just(throwing_move{}); };
[[maybe_unused]] auto const ignore_value = [](throwing_move &) noexcept { return lexec::just(); };

static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1) | lexec::let_value(double_it))>,
                             completion_signatures<set_value_t(double)>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just_error(5) | lexec::let_error(recover))>,
                             completion_signatures<set_value_t(int)>>);

// Channels the let does not handle pass through, and its function need not accept them.
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just_error(1) | lexec::let_value(double_it))>,
                             completion_signatures<set_error_t(int)>>);
static_assert(
    std::is_same_v<completion_signatures_of_t<decltype(lexec_test::value_or_stopped_sender{} | lexec::let_value(double_it))>,
                   completion_signatures<set_value_t(double), set_stopped_t()>>);

// A second sender that reads the environment makes the let dependent.
static_assert(lexec::is_dependent_sender_v<decltype(lexec::just() | lexec::let_value(read_scheduler))>);

#if LEXEC_HAS_EXCEPTIONS
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1) | lexec::let_value(throwing_double))>,
                             completion_signatures<set_value_t(double), set_error_t(std::exception_ptr)>>);
// Connecting a second sender that must move a value that may throw...
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1) | lexec::let_value(just_throwing_move))>,
                             completion_signatures<set_value_t(throwing_move), set_error_t(std::exception_ptr)>>);
// ...and storing such a value from the predecessor may throw too.
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(throwing_move{}) | lexec::let_value(ignore_value))>,
                             completion_signatures<set_value_t(), set_error_t(std::exception_ptr)>>);
#endif

} // namespace

TEST_CASE("let_value continues with the sender its function returns") {
    auto const result = lexec::sync_wait(lexec::just(20) | lexec::let_value([](int &v) noexcept { return lexec::just(v + 22); }));
    CHECK(std::get<0>(*result) == 42);
}

TEST_CASE("let_error and let_stopped continue from their channels") {
    auto const from_error =
        lexec::sync_wait(lexec::just_error(5) | lexec::let_error([](int &e) noexcept { return lexec::just(e * 2); }));
    auto const from_stopped =
        lexec::sync_wait(lexec::just_stopped() | lexec::let_stopped([]() noexcept { return lexec::just(7); }));
    CHECK(std::get<0>(*from_error) == 10);
    CHECK(std::get<0>(*from_stopped) == 7);
}

TEST_CASE("let passes the channels it does not handle through") {
    auto log = lexec_test::completion_log{};
    auto error_op = lexec::connect(lexec::just_error(1) | lexec::let_value(double_it),
                                   lexec_test::checked_receiver<set_error_t(int)>{&log});
    lexec::start(error_op);
    auto stopped_op = lexec::connect(lexec_test::value_or_stopped_sender{true, 0} | lexec::let_value(double_it),
                                     lexec_test::checked_receiver<set_value_t(double), set_stopped_t()>{&log});
    lexec::start(stopped_op);
    auto value_op =
        lexec::connect(lexec::just(4) | lexec::let_error(recover), lexec_test::checked_receiver<set_value_t(int)>{&log});
    lexec::start(value_op);
    CHECK(log.error_count == 1);
    CHECK(log.stopped_count == 1);
    CHECK(log.value_count == 1);
}

TEST_CASE("the predecessor's results stay alive until the second sender completes") {
    auto const result = lexec::sync_wait(lexec::just(std::string{"hello"}) | lexec::let_value([](std::string &text) {
                                             return lexec::read_env(lexec::get_scheduler) |
                                                    lexec::let_value([&text](auto &scheduler) {
                                                        return lexec::schedule(scheduler) |
                                                               lexec::then([&text] { return text + " world"; });
                                                    });
                                         }));
    CHECK(std::get<0>(*result) == "hello world");
}

TEST_CASE("the second sender starts on the scheduler where the predecessor completed") {
    auto other = lexec::run_loop{};
    auto driver = std::thread{[&other] { other.run(); }};
    auto const other_scheduler = other.get_scheduler();
    auto const result = lexec::sync_wait(lexec::schedule(other_scheduler) | lexec::let_value(read_scheduler));
    other.finish();
    driver.join();
    REQUIRE(result.has_value());
    auto const started_on_other = std::get<0>(*result) == other_scheduler;
    CHECK(started_on_other);
}

TEST_CASE("let_value continues each value channel with the sender returned for it") {
    auto const handle = lexec_test::overloaded{[](int &v) noexcept { return lexec::just(v * 6); },
                                               [](std::string &s) noexcept { return lexec::just(std::move(s)); }};
    auto const run = [&handle](bool const send_text) {
        return lexec::sync_wait(lexec_test::int_or_string_sender{send_text} | lexec::let_value(handle) | lexec::into_variant);
    };
    auto const number = run(false);
    auto const text = run(true);
    CHECK(std::get<0>(std::get<0>(std::get<0>(*number))) == 42);
    CHECK(std::get<0>(std::get<1>(std::get<0>(*text))) == "text");
}

TEST_CASE("let destroys the predecessor's operation before calling its function") {
    auto destroyed = 0;
    auto destroyed_when_called = -1;
    auto const result = lexec::sync_wait(lexec_test::tracked_sender{&destroyed, 5} | lexec::let_value([&](int &v) noexcept {
                                             destroyed_when_called = destroyed;
                                             return lexec::just(v);
                                         }));
    CHECK(std::get<0>(*result) == 5);
    CHECK(destroyed_when_called == 1);
    CHECK(destroyed == 1);
}

TEST_CASE("let_value carries move-only values") {
    auto const result = lexec::sync_wait(
        lexec::just(std::make_unique<int>(5)) |
        lexec::let_value([](std::unique_ptr<int> &p) noexcept { return lexec::just(std::move(p)); }) |
        lexec::then([](std::unique_ptr<int> p) noexcept { return *p * 2; }));
    CHECK(std::get<0>(*result) == 10);
}

TEST_CASE("an lvalue let sender can be connected repeatedly") {
    auto const sender = lexec::just(20) | lexec::let_value([](int &v) noexcept { return lexec::just(v + 1); });
    CHECK(std::get<0>(*lexec::sync_wait(sender)) == 21);
    CHECK(std::get<0>(*lexec::sync_wait(sender)) == 21);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("an exception thrown by let's function completes with set_error") {
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(
        lexec::just(1) | lexec::let_value([](int &) -> decltype(lexec::just(0)) { throw std::runtime_error{"boom"}; }),
        lexec_test::checked_receiver<set_value_t(int), set_error_t(std::exception_ptr)>{&log});
    lexec::start(op);
    CHECK(log.error_count == 1);
    CHECK(log.total() == 1);
}
#endif
