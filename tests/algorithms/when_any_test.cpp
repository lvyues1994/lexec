#include "../support/loop_thread.hpp"
#include "../support/test_receivers.hpp"
#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <exception>
#include <thread>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using lexec::completion_signatures;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(int), set_value_t(double), set_stopped_t()>,
              lexec::completion_signatures_of_t<decltype(lexec::when_any(lexec::just(1), lexec::just(2.5))), lexec::env<>>>);
static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(int), set_error_t(int), set_stopped_t()>,
              lexec::completion_signatures_of_t<decltype(lexec::when_any(lexec::just(1), lexec::just_error(2))), lexec::env<>>>);

// Records its completion where another thread may wait for it.
struct flag_receiver {
    using receiver_concept = lexec::receiver_t;

    void set_value(int) && noexcept { done->store(1, std::memory_order_release); }
    void set_error(std::exception_ptr) && noexcept { done->store(3, std::memory_order_release); }
    void set_stopped() && noexcept { done->store(2, std::memory_order_release); }

    std::atomic<int> *done;
};

} // namespace

TEST_CASE("when_any completes with the first sender to complete and stops the others") {
    auto const first = lexec::sync_wait(lexec::when_any(lexec::just(1), lexec_test::until_stopped_sender{}));
    CHECK(std::get<0>(*first) == 1);
    auto const later = lexec::sync_wait(lexec::when_any(lexec_test::until_stopped_sender{}, lexec::just(2)));
    CHECK(std::get<0>(*later) == 2);
}

TEST_CASE("when_any passes on the winner's values, whatever their type") {
    auto const result = lexec::sync_wait(lexec::into_variant(lexec::when_any(lexec::just(3), lexec::just(4.5))));
    auto const &values = std::get<0>(*result);
    REQUIRE(std::holds_alternative<std::tuple<int>>(values));
    CHECK(std::get<0>(std::get<std::tuple<int>>(values)) == 3);
}

TEST_CASE("an error or a stop that comes first wins as well") {
    auto log = lexec_test::completion_log{};
    auto failed = lexec::connect(lexec::when_any(lexec::just_error(7), lexec_test::until_stopped_sender{}),
                                 lexec_test::checked_receiver<set_error_t(int), set_stopped_t()>{&log});
    lexec::start(failed);
    CHECK(log.error_count == 1);
    auto const stopped = lexec::sync_wait(lexec::when_any(lexec::just_stopped(), lexec::just(1)));
    CHECK_FALSE(stopped.has_value());
}

TEST_CASE("when_any completes with stopped when its receiver asks it to stop") {
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(
        lexec::write_env(lexec::when_any(lexec_test::until_stopped_sender{}, lexec_test::until_stopped_sender{}),
                         lexec::prop{lexec::get_stop_token, source.get_token()}),
        lexec_test::checked_receiver<set_stopped_t()>{&log});
    lexec::start(op);
    CHECK(log.stopped_count == 1);

    auto later = lexec::inplace_stop_source{};
    auto later_log = lexec_test::completion_log{};
    auto running = lexec::connect(
        lexec::write_env(lexec::when_any(lexec_test::until_stopped_sender{}, lexec_test::until_stopped_sender{}),
                         lexec::prop{lexec::get_stop_token, later.get_token()}),
        lexec_test::checked_receiver<set_stopped_t()>{&later_log});
    lexec::start(running);
    CHECK(later_log.stopped_count == 0);
    auto requester = std::thread{[&later] { later.request_stop(); }};
    requester.join();
    CHECK(later_log.stopped_count == 1);
}

TEST_CASE("when_any of work on other threads completes once with one of the results") {
    auto pool = lexec::static_thread_pool{4};
    auto const sch = pool.get_scheduler();
    for (auto round = 0; round < 200; ++round) {
        auto const result = lexec::sync_wait(lexec::when_any(lexec::schedule(sch) | lexec::then([] { return 1; }),
                                                             lexec::schedule(sch) | lexec::then([] { return 2; }),
                                                             lexec::schedule(sch) | lexec::then([] { return 3; })));
        REQUIRE(result.has_value());
        auto const value = std::get<0>(*result);
        CHECK((value >= 1 and value <= 3));
    }
}

// The children complete inside the stop request, and so complete when_any from within
// its stop source; the stop request must be done with the source by then.
TEST_CASE("a receiver may destroy when_any as soon as a stop request completes it") {
    auto source = lexec::inplace_stop_source{};
    auto log = lexec_test::completion_log{};
    lexec_test::start_destroyed_on_completion(
        lexec::write_env(lexec::when_any(lexec_test::until_stopped_sender{}, lexec_test::until_stopped_sender{}),
                         lexec::prop{lexec::get_stop_token, source.get_token()}),
        log);
    source.request_stop();
    CHECK(log.stopped_count == 1);
    CHECK(log.total() == 1);
}

// Children finish on several threads while stop requests arrive; TSan checks the count.
TEST_CASE("when_any under concurrent completion and stop requests stays consistent") {
    auto first = lexec_test::loop_thread{};
    auto second = lexec_test::loop_thread{};
    auto completions = 0;
    for (auto round = 0; round < 300; ++round) {
        auto source = lexec::inplace_stop_source{};
        auto done = std::atomic<int>{0};
        auto op = lexec::connect(
            lexec::write_env(lexec::when_any(lexec::schedule(first.scheduler()) | lexec::then([]() noexcept { return 1; }),
                                             lexec::schedule(second.scheduler()) | lexec::then([]() noexcept { return 2; })),
                             lexec::prop{lexec::get_stop_token, source.get_token()}),
            flag_receiver{&done});
        lexec::start(op);
        auto requester = std::thread{[&source] { source.request_stop(); }};
        requester.join();
        // The when_any completes only after both children have, so op may go once done.
        while (done.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        ++completions;
    }
    CHECK(completions == 300);
}
