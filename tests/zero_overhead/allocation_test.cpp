#include "../support/allocation_counter.hpp"
#include "../support/loop_thread.hpp"

#include <lexec/any_sender_of.hpp>
#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// MSVC constructs a module's thread_local objects, doctest's streams among them, when a
// thread starts, so measuring begins only after every worker has run work. Submissions
// from a non-worker thread go to the workers in turn, and only a worker takes its own.
void wait_until_workers_started(lexec::static_thread_pool &pool) {
    auto workers = std::set<std::thread::id>{};
    while (workers.size() < pool.available_parallelism()) {
        auto const ran_on = lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                                             lexec::then([]() noexcept { return std::this_thread::get_id(); }));
        workers.insert(std::get<0>(*ran_on));
    }
}

} // namespace

TEST_CASE("a synchronous pipeline allocates nothing") {
    auto const before = lexec_test::allocation_count();
    auto const result = lexec::sync_wait(lexec::just(20) | lexec::then([](int v) noexcept { return v + 1; }) |
                                         lexec::then([](int v) noexcept { return v * 2; }));
    auto const after = lexec_test::allocation_count();
    CHECK(std::get<0>(*result) == 42);
    CHECK(after == before);
}

TEST_CASE("bulk on the completing agent allocates nothing") {
    auto values = std::vector<int>(64);
    auto const before = lexec_test::allocation_count();
    lexec::sync_wait(lexec::just() | lexec::bulk(lexec::par, 64, [&values](int i) noexcept {
                         values[static_cast<std::size_t>(i)] = i;
                     }) |
                     lexec::bulk_chunked(lexec::par, 64, [&values](int b, int e) noexcept {
                         for (; b != e; ++b) {
                             values[static_cast<std::size_t>(b)] *= 2;
                         }
                     }));
    auto const after = lexec_test::allocation_count();
    CHECK(values[63] == 126);
    CHECK(after == before);
}

TEST_CASE("completing on another thread's run_loop allocates nothing") {
    auto loop = lexec::run_loop{};
    auto driver = std::thread{[&loop] { loop.run(); }};
    lexec::sync_wait(lexec::schedule(loop.get_scheduler()));
    auto const before = lexec_test::allocation_count();
    auto const result =
        lexec::sync_wait(lexec::schedule(loop.get_scheduler()) | lexec::then([]() noexcept { return 42; }));
    auto const after = lexec_test::allocation_count();
    loop.finish();
    driver.join();
    CHECK(std::get<0>(*result) == 42);
    CHECK(after == before);
}

TEST_CASE("let, into_variant, and stopped_as_optional allocate nothing") {
    auto const before = lexec_test::allocation_count();
    auto const result = lexec::sync_wait(lexec::just(20) |
                                         lexec::let_value([](int &v) noexcept { return lexec::just(v + 22); }) |
                                         lexec::stopped_as_optional | lexec::into_variant);
    auto const after = lexec_test::allocation_count();
    CHECK(*std::get<0>(std::get<0>(std::get<0>(*result))) == 42);
    CHECK(after == before);
}

TEST_CASE("moving work to another thread's scheduler and back allocates nothing") {
    auto other = lexec_test::loop_thread{};
    auto const before = lexec_test::allocation_count();
    auto const result = lexec::sync_wait(
        lexec::on(other.scheduler(), lexec::just(20) | lexec::then([](int v) noexcept { return v + 22; })) |
        lexec::continues_on(lexec::inline_scheduler{}));
    auto const after = lexec_test::allocation_count();
    CHECK(std::get<0>(*result) == 42);
    CHECK(after == before);
}

TEST_CASE("scheduling on static_thread_pool allocates nothing") {
    auto pool = lexec::static_thread_pool{2};
    wait_until_workers_started(pool);
    auto const before = lexec_test::allocation_count();
    auto const result = lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                                         lexec::then([]() noexcept { return 42; }) |
                                         lexec::continues_on(pool.get_scheduler()));
    auto const after = lexec_test::allocation_count();
    CHECK(std::get<0>(*result) == 42);
    CHECK(after == before);
}

TEST_CASE("parallel bulk on static_thread_pool allocates nothing") {
    auto pool = lexec::static_thread_pool{4};
    wait_until_workers_started(pool);
    auto values = std::vector<int>(4096);
    auto const before = lexec_test::allocation_count();
    lexec::sync_wait(lexec::schedule(pool.get_scheduler()) | lexec::bulk(lexec::par, 4096, [&values](int i) noexcept {
                         values[static_cast<std::size_t>(i)] = i;
                     }));
    auto const after = lexec_test::allocation_count();
    CHECK(values[4095] == 4095);
    CHECK(after == before);
}

TEST_CASE("scheduling and bulk work on the parallel scheduler allocate nothing") {
    auto const sch = lexec::get_parallel_scheduler();
    // The backend's workers start with it; each must have run work before measuring.
    auto workers = std::set<std::thread::id>{};
    for (auto round = 0; round < 10'000 and workers.size() < lexec::static_thread_pool::default_thread_count(); ++round) {
        auto const ran_on = lexec::sync_wait(lexec::schedule(sch) |
                                             lexec::then([]() noexcept { return std::this_thread::get_id(); }));
        workers.insert(std::get<0>(*ran_on));
    }
    auto values = std::vector<int>(4096);
    auto const before = lexec_test::allocation_count();
    lexec::sync_wait(lexec::schedule(sch) | lexec::bulk(lexec::par, 4096, [&values](int i) noexcept {
                         values[static_cast<std::size_t>(i)] = i;
                     }));
    auto const after = lexec_test::allocation_count();
    CHECK(values[4095] == 4095);
    CHECK(after == before);
}

TEST_CASE("an any_sender stores a small sender inline and allocates only its operation") {
    auto const before = lexec_test::allocation_count();
    lexec::any_sender_of<lexec::set_value_t(int)> erased = lexec::just(7);
    auto const constructed = lexec_test::allocation_count();
    auto const result = lexec::sync_wait(std::move(erased));
    auto const after = lexec_test::allocation_count();
    CHECK(std::get<0>(*result) == 7);
    CHECK(constructed == before);
    CHECK(after - constructed == 1);
}

TEST_CASE("the allocation counter observes allocations") {
    auto const before = lexec_test::allocation_count();
    auto const text = std::string(64, 'x');
    CHECK(lexec_test::allocation_count() > before);
    CHECK(text.size() == 64);
}
