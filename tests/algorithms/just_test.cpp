#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <type_traits>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;
using lexec_test::checked_receiver;
using lexec_test::completion_log;

static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1, std::string{}))>,
                             completion_signatures<set_value_t(int, std::string)>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just())>, completion_signatures<set_value_t()>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just_error(1.5))>,
                             completion_signatures<set_error_t(double)>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just_stopped())>,
                             completion_signatures<set_stopped_t()>>);
static_assert(std::is_empty_v<decltype(lexec::just_stopped())>);

} // namespace

TEST_CASE("just is lazy and completes exactly once when started") {
    auto log = completion_log{};
    auto op = lexec::connect(lexec::just(1, 2), checked_receiver<set_value_t(int, int)>{&log});
    CHECK(log.total() == 0);
    lexec::start(op);
    CHECK(log.value_count == 1);
    CHECK(log.total() == 1);
}

TEST_CASE("just sends its values") {
    auto const result = lexec::sync_wait(lexec::just(7, std::string{"seven"}));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == 7);
    CHECK(std::get<1>(*result) == "seven");
}

TEST_CASE("just accepts move-only values") {
    auto const result = lexec::sync_wait(lexec::just(std::make_unique<int>(3)));
    REQUIRE(result.has_value());
    CHECK(*std::get<0>(*result) == 3);
}

TEST_CASE("just_error and just_stopped complete on their channels") {
    auto log = completion_log{};
    auto error_op = lexec::connect(lexec::just_error(5), checked_receiver<set_error_t(int)>{&log});
    auto stopped_op = lexec::connect(lexec::just_stopped(), checked_receiver<set_stopped_t()>{&log});
    lexec::start(error_op);
    lexec::start(stopped_op);
    CHECK(log.error_count == 1);
    CHECK(log.stopped_count == 1);
    CHECK(log.total() == 2);
}

TEST_CASE("an lvalue just sender can be connected repeatedly") {
    auto const sender = lexec::just(std::string{"again"});
    CHECK(std::get<0>(*lexec::sync_wait(sender)) == "again");
    CHECK(std::get<0>(*lexec::sync_wait(sender)) == "again");
}
