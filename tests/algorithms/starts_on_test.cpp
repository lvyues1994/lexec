#include "../support/loop_thread.hpp"
#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_value_t;

// starts_on is lowered to let_value, whose completions need no environment.
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::starts_on(lexec::inline_scheduler{}, lexec::just(1)))>,
                             completion_signatures<set_value_t(int)>>);

// on returns to the scheduler it was started on, which only the receiver knows.
static_assert(lexec::is_dependent_sender_v<decltype(lexec::on(lexec::inline_scheduler{}, lexec::just(1)))>);

} // namespace

TEST_CASE("starts_on starts the sender on the scheduler, which it sees as current") {
    auto other = lexec_test::loop_thread{};
    auto const result = lexec::sync_wait(lexec::starts_on(
        other.scheduler(), lexec::read_env(lexec::get_scheduler) | lexec::then([](auto scheduler) noexcept {
                               return std::pair{scheduler, std::this_thread::get_id()};
                           })));
    auto const sees_other = std::get<0>(*result).first == other.scheduler();
    auto const runs_on_other = std::get<0>(*result).second == other.id();
    CHECK(sees_other);
    CHECK(runs_on_other);
}

TEST_CASE("on runs the sender on the scheduler and completes where it was started") {
    auto other = lexec_test::loop_thread{};
    auto const result = lexec::sync_wait(
        lexec::on(other.scheduler(), lexec::just() | lexec::then([]() noexcept { return std::this_thread::get_id(); })) |
        lexec::then([](std::thread::id worker) noexcept { return std::pair{worker, std::this_thread::get_id()}; }));
    auto const ran_on_other = std::get<0>(*result).first == other.id();
    auto const returned_here = std::get<0>(*result).second == std::this_thread::get_id();
    CHECK(ran_on_other);
    CHECK(returned_here);
}

TEST_CASE("on with a closure runs the closure on the scheduler and completes where the sender did") {
    auto other = lexec_test::loop_thread{};
    auto const result = lexec::sync_wait(
        lexec::just(1) |
        lexec::on(other.scheduler(),
                  lexec::then([](int v) noexcept { return std::pair{v + 1, std::this_thread::get_id()}; })) |
        lexec::then([](std::pair<int, std::thread::id> step) noexcept {
            return std::tuple{step.first, step.second, std::this_thread::get_id()};
        }));
    auto const [value, worker, finisher] = std::get<0>(*result);
    auto const ran_on_other = worker == other.id();
    auto const returned_here = finisher == std::this_thread::get_id();
    CHECK(value == 2);
    CHECK(ran_on_other);
    CHECK(returned_here);
}
