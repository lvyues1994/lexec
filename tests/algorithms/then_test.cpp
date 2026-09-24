#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

[[maybe_unused]] auto const nothrow_to_double = [](int const v) noexcept { return v * 0.5; };
[[maybe_unused]] auto const throwing_to_double = [](int const v) { return v * 0.5; };

static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1) | lexec::then(nothrow_to_double))>,
                             completion_signatures<set_value_t(double)>>);

using throwing_then = decltype(lexec::just(1) | lexec::then(throwing_to_double));
#if LEXEC_HAS_EXCEPTIONS
static_assert(std::is_same_v<completion_signatures_of_t<throwing_then>,
                             completion_signatures<set_value_t(double), set_error_t(std::exception_ptr)>>);
#else
static_assert(std::is_same_v<completion_signatures_of_t<throwing_then>, completion_signatures<set_value_t(double)>>);
#endif

[[maybe_unused]] auto const nothrow_void = [](int) noexcept {};
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1) | lexec::then(nothrow_void))>,
                             completion_signatures<set_value_t()>>);

// Channels the adaptor does not transform pass through unchanged.
[[maybe_unused]] auto const error_to_zero = [](int) noexcept { return 0; };
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just_error(1) | lexec::then(nothrow_to_double))>,
                             completion_signatures<set_error_t(int)>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just_stopped() | lexec::upon_error(error_to_zero))>,
                             completion_signatures<set_stopped_t()>>);

} // namespace

TEST_CASE("then transforms values through a chain") {
    auto const result = lexec::sync_wait(lexec::just(20) | lexec::then([](int v) noexcept { return v + 1; }) |
                                         lexec::then([](int v) noexcept { return v * 2; }) |
                                         lexec::then([](int v) { return std::to_string(v); }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == "42");
}

TEST_CASE("then with a void function sends no values") {
    auto calls = 0;
    auto const result = lexec::sync_wait(lexec::just(1) | lexec::then([&calls](int) noexcept { ++calls; }));
    REQUIRE(result.has_value());
    CHECK(calls == 1);
    static_assert(std::is_same_v<std::remove_const_t<decltype(result)>, std::optional<std::tuple<>>>);
}

TEST_CASE("upon_error turns an error into a value") {
    auto const result =
        lexec::sync_wait(lexec::just_error(7) | lexec::upon_error([](int e) noexcept { return e * 6; }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == 42);
}

TEST_CASE("upon_stopped turns stopped into a value") {
    auto const result = lexec::sync_wait(lexec::just_stopped() | lexec::upon_stopped([]() noexcept { return 42; }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == 42);
}

TEST_CASE("then passes move-only values through") {
    auto const result = lexec::sync_wait(lexec::just(std::make_unique<int>(4)) |
                                         lexec::then([](std::unique_ptr<int> p) noexcept { return *p * 10; }));
    CHECK(std::get<0>(*result) == 40);
}

TEST_CASE("an lvalue closure and an lvalue sender can be reused") {
    auto const add_one = lexec::then([](int v) noexcept { return v + 1; });
    auto const sender = lexec::just(1) | add_one;
    CHECK(std::get<0>(*lexec::sync_wait(sender)) == 2);
    CHECK(std::get<0>(*lexec::sync_wait(sender | add_one)) == 3);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("an exception thrown by then's function completes with set_error") {
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::just(1) | lexec::then([](int) -> int { throw std::runtime_error{"boom"}; }),
                             lexec_test::checked_receiver<set_value_t(int), set_error_t(std::exception_ptr)>{&log});
    lexec::start(op);
    CHECK(log.error_count == 1);
    CHECK(log.total() == 1);
}
#endif
