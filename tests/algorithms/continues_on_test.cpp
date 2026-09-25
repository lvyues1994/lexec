#include "../support/loop_thread.hpp"
#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace continues_on_test {

struct test_domain {};

// Outside the anonymous namespace so that its unused comparisons are not diagnosed.
struct domain_scheduler {
    using scheduler_concept = lexec::scheduler_t;

    lexec::detail::inline_sender schedule() const noexcept { return {}; }
    test_domain query(lexec::get_completion_domain_t<lexec::set_value_t>) const noexcept { return {}; }

    friend bool operator==(domain_scheduler, domain_scheduler) noexcept { return true; }
    friend bool operator!=(domain_scheduler, domain_scheduler) noexcept { return false; }
};

} // namespace continues_on_test

namespace {

using continues_on_test::domain_scheduler;
using continues_on_test::test_domain;
using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;
using lexec_test::counted;

using run_loop_scheduler = lexec::detail::run_loop_scheduler;

struct shared_int {
    int const &operator()() const noexcept {
        static int const value = 1;
        return value;
    }
};

// The results are decayed into the operation; the scheduler's own error and stopped
// completions join them.
using onto_run_loop = decltype(lexec::just() | lexec::then(shared_int{}) |
                               lexec::continues_on(std::declval<run_loop_scheduler>()));
#if LEXEC_HAS_EXCEPTIONS
static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(int), set_stopped_t(), set_error_t(std::exception_ptr)>,
              completion_signatures_of_t<onto_run_loop>>);
#else
static_assert(lexec_test::same_signature_set_v<completion_signatures<set_value_t(int), set_stopped_t()>,
                                               completion_signatures_of_t<onto_run_loop>>);
#endif

// continues_on dispatches algorithms to the scheduler's domain; the schedule_from it
// wraps its predecessor in dispatches to the predecessor's.
using domain_env = lexec::prop<lexec::get_domain_t, test_domain>;
static_assert(std::is_same_v<decltype(lexec::get_completion_domain<>(
                                 lexec::get_env(lexec::continues_on(lexec::just(1), domain_scheduler{})), lexec::env<>{})),
                             test_domain>);
static_assert(std::is_same_v<decltype(lexec::get_completion_domain<>(lexec::get_env(lexec::schedule_from(lexec::just(1))),
                                                                     std::declval<domain_env>())),
                             test_domain>);

} // namespace

TEST_CASE("continues_on completes on the scheduler's thread") {
    auto other = lexec_test::loop_thread{};
    auto const result = lexec::sync_wait(lexec::just(20) | lexec::continues_on(other.scheduler()) |
                                         lexec::then([](int v) noexcept { return std::pair{v + 1, std::this_thread::get_id()}; }));
    CHECK(std::get<0>(*result).first == 21);
    auto const on_other = std::get<0>(*result).second == other.id();
    CHECK(on_other);
}

TEST_CASE("continues_on replays errors and stopped on the scheduler's thread") {
    auto other = lexec_test::loop_thread{};
    auto error_thread = std::thread::id{};
    auto stopped_thread = std::thread::id{};
    lexec::sync_wait(lexec::just_error(1) | lexec::continues_on(other.scheduler()) | lexec::upon_error([&](auto) noexcept {
                         error_thread = std::this_thread::get_id();
                         return 0;
                     }));
    lexec::sync_wait(lexec::just_stopped() | lexec::continues_on(other.scheduler()) | lexec::upon_stopped([&]() noexcept {
                         stopped_thread = std::this_thread::get_id();
                         return 0;
                     }));
    auto const errors_on_other = error_thread == other.id();
    auto const stopped_on_other = stopped_thread == other.id();
    CHECK(errors_on_other);
    CHECK(stopped_on_other);
}

TEST_CASE("continues_on completes with stopped when stop is requested before the scheduler runs it") {
    auto other = lexec_test::loop_thread{};
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto const result = lexec::sync_wait(lexec::write_env(lexec::just(1) | lexec::continues_on(other.scheduler()),
                                                          lexec::prop{lexec::get_stop_token, source.get_token()}));
    CHECK_FALSE(result.has_value());
}

TEST_CASE("continues_on reports the scheduler as its completion scheduler") {
    auto other = lexec_test::loop_thread{};
    auto const reported =
        lexec::get_completion_scheduler<set_value_t>(lexec::get_env(lexec::just(1) | lexec::continues_on(other.scheduler())));
    auto const is_scheduler = reported == other.scheduler();
    CHECK(is_scheduler);
}

TEST_CASE("continues_on moves each value once into its operation") {
    counted::reset();
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::just() | lexec::then([]() noexcept { return counted{7}; }) |
                                 lexec::continues_on(lexec::inline_scheduler{}),
                             lexec_test::checked_receiver<set_value_t(counted)>{&log});
    lexec::start(op);
    CHECK(log.value_count == 1);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 1);
}

TEST_CASE("continues_on carries move-only values") {
    auto const result =
        lexec::sync_wait(lexec::just(std::make_unique<int>(3)) | lexec::continues_on(lexec::inline_scheduler{}));
    CHECK(*std::get<0>(*result) == 3);
}
