#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

static_assert(lexec::is_scheduler_v<lexec::detail::pool_scheduler>);
static_assert(std::is_same_v<lexec::completion_signatures_of_t<lexec::detail::pool_sender>,
                             lexec::completion_signatures<lexec::set_value_t(), lexec::set_stopped_t()>>);

template <class Sch>
auto increments(Sch scheduler, std::atomic<int> &counter) {
    auto const one = [scheduler, &counter] {
        return lexec::schedule(scheduler) | lexec::then([&counter]() noexcept { counter.fetch_add(1); });
    };
    return lexec::when_all(one(), one(), one(), one(), one(), one(), one(), one());
}

} // namespace

TEST_CASE("static_thread_pool runs scheduled work on its own threads") {
    auto pool = lexec::static_thread_pool{2};
    CHECK(pool.available_parallelism() == 2);
    auto const result = lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                                         lexec::then([]() noexcept { return std::this_thread::get_id(); }));
    auto const on_worker = std::get<0>(*result) != std::this_thread::get_id();
    CHECK(on_worker);
    auto const reported = lexec::get_completion_scheduler<lexec::set_value_t>(lexec::get_env(lexec::schedule(pool.get_scheduler())));
    auto const reports_pool = reported == pool.get_scheduler();
    CHECK(reports_pool);
}

TEST_CASE("every task scheduled from many threads runs exactly once") {
    auto pool = lexec::static_thread_pool{4};
    auto counter = std::atomic<int>{0};
    auto submitters = std::vector<std::thread>{};
    for (auto s = 0; s < 4; ++s) {
        submitters.emplace_back([&] {
            for (auto i = 0; i < 250; ++i) {
                lexec::sync_wait(increments(pool.get_scheduler(), counter));
            }
        });
    }
    for (auto &submitter : submitters) {
        submitter.join();
    }
    CHECK(counter.load() == 4 * 250 * 8);
}

TEST_CASE("work that a pool task schedules on the pool runs on the pool") {
    auto pool = lexec::static_thread_pool{3};
    auto const scheduler = pool.get_scheduler();
    auto const result = lexec::sync_wait(lexec::schedule(scheduler) | lexec::let_value([scheduler]() noexcept {
                                             auto const worker_id = [] { return std::this_thread::get_id(); };
                                             return lexec::when_all(lexec::schedule(scheduler) | lexec::then(worker_id),
                                                                    lexec::schedule(scheduler) | lexec::then(worker_id));
                                         }));
    auto const &[first, second] = *result;
    auto const off_main = first != std::this_thread::get_id() and second != std::this_thread::get_id();
    CHECK(off_main);
}

TEST_CASE("a task whose receiver requested stop completes with stopped") {
    auto pool = lexec::static_thread_pool{1};
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto const result = lexec::sync_wait(
        lexec::write_env(lexec::schedule(pool.get_scheduler()), lexec::prop{lexec::get_stop_token, source.get_token()}));
    CHECK_FALSE(result.has_value());
}

TEST_CASE("destroying an idle pool joins its threads") {
    for (auto i = 0; i < 20; ++i) {
        auto const pool = lexec::static_thread_pool{3};
    }
    CHECK(true);
}
