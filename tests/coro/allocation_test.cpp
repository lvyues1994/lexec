#include "../support/allocation_counter.hpp"

#include <lexec/coro/co2.hpp>
#include <lexec/execution.hpp>

#include <co2/co2.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <exception>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

namespace {

auto hop_many(lexec::detail::pool_scheduler sch, int count) CO2_BEG(co2::Task<int>, (sch, count), int i{};) {
    for (i = 0; i < count; ++i) {
        CO2_AWAIT(lexec::schedule(sch));
    }
    CO2_RETURN(count);
}
CO2_END

auto just_value() CO2_BEG(co2::Task<int>, ()) {
    CO2_RETURN(5);
}
CO2_END

// MSVC constructs a module's thread_local objects when a thread starts, so measuring
// begins only after every worker has run work.
void wait_until_workers_started(lexec::static_thread_pool &pool) {
    auto workers = std::set<std::thread::id>{};
    while (workers.size() < pool.available_parallelism()) {
        auto const ran_on = lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                                             lexec::then([]() noexcept { return std::this_thread::get_id(); }));
        workers.insert(std::get<0>(*ran_on));
    }
}

using schedule_awaiter = lexec::coro::detail::sender_awaiter<decltype(lexec::schedule(
    std::declval<lexec::detail::pool_scheduler>()))>;

static_assert(sizeof(std::exception_ptr) != sizeof(void *) or co2::detail::AwaitSlot<>::isInline<schedule_awaiter>(),
              "awaiting schedule on a pool must fit co2's inline awaiter slot");

} // namespace

TEST_CASE("awaiting schedule on a pool allocates nothing beyond the coroutine frame") {
    auto pool = lexec::static_thread_pool{2};
    wait_until_workers_started(pool);
    // Where exception_ptr is two pointers wide, as on MSVC, the awaiter outgrows co2's
    // inline slot, and co2 allocates it.
    constexpr auto per_await = co2::detail::AwaitSlot<>::isInline<schedule_awaiter>() ? 0 : 1;
    auto const before = lexec_test::allocation_count();
    CHECK(co2::syncWait(hop_many(pool.get_scheduler(), 1000)) == 1000);
    auto const after = lexec_test::allocation_count();
    CHECK(after - before == std::size_t{1} + std::size_t{per_await} * 1000);
}

TEST_CASE("a co2 Task run as a sender allocates nothing beyond the coroutine frame") {
    auto const before = lexec_test::allocation_count();
    CHECK(std::get<0>(*lexec::sync_wait(lexec::coro::as_sender(just_value()))) == 5);
    auto const after = lexec_test::allocation_count();
    CHECK(after - before == 1);
}
