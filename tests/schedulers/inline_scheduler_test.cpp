#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <type_traits>

// Outside the anonymous namespace so that their unused comparisons are not diagnosed.
namespace inline_scheduler_test {

using lexec::set_value_t;

struct target_scheduler {
    using scheduler_concept = lexec::scheduler_t;

    lexec::detail::inline_sender schedule() const noexcept { return {}; }

    friend bool operator==(target_scheduler, target_scheduler) noexcept { return true; }
    friend bool operator!=(target_scheduler, target_scheduler) noexcept { return false; }
};

// Reports that work scheduled on it completes on target_scheduler.
struct forwarding_scheduler {
    using scheduler_concept = lexec::scheduler_t;

    lexec::detail::inline_sender schedule() const noexcept { return {}; }
    target_scheduler query(lexec::get_completion_scheduler_t<set_value_t>) const noexcept { return {}; }

    friend bool operator==(forwarding_scheduler, forwarding_scheduler) noexcept { return true; }
    friend bool operator!=(forwarding_scheduler, forwarding_scheduler) noexcept { return false; }
};

} // namespace inline_scheduler_test

namespace {

using inline_scheduler_test::forwarding_scheduler;
using inline_scheduler_test::target_scheduler;
using lexec::set_value_t;

static_assert(lexec::is_scheduler_v<lexec::inline_scheduler>);

// get_scheduler follows each scheduler to the one its work completes on.
static_assert(std::is_same_v<decltype(lexec::get_scheduler(lexec::prop{lexec::get_scheduler, forwarding_scheduler{}})),
                             target_scheduler>);
static_assert(std::is_same_v<decltype(lexec::get_scheduler(lexec::prop{lexec::get_scheduler, lexec::inline_scheduler{}})),
                             lexec::inline_scheduler>);

} // namespace

TEST_CASE("inline_scheduler completes schedule() during start") {
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::schedule(lexec::inline_scheduler{}), lexec_test::checked_receiver<set_value_t()>{&log});
    lexec::start(op);
    CHECK(log.value_count == 1);
    CHECK(std::get<0>(*lexec::sync_wait(lexec::schedule(lexec::inline_scheduler{}) |
                                        lexec::then([]() noexcept { return 42; }))) == 42);
}

TEST_CASE("inline_scheduler reports the scheduler it was started on as its completion scheduler") {
    auto loop = lexec::run_loop{};
    auto const scheduler = loop.get_scheduler();
    auto const reported = lexec::get_completion_scheduler<set_value_t>(
        lexec::get_env(lexec::schedule(lexec::inline_scheduler{})), lexec::prop{lexec::get_scheduler, scheduler});
    auto const is_started_scheduler = reported == scheduler;
    CHECK(is_started_scheduler);
}
