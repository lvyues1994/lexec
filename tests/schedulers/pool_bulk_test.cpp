#include "../support/test_receivers.hpp"
#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

struct ignore_index {
    template <class... Vs>
    void operator()(int, Vs &...) const noexcept {}
};

struct ignore_chunk {
    template <class... Vs>
    void operator()(int, int, Vs &...) const noexcept {}
};

struct value_of_seven {
    int operator()() const noexcept { return 7; }
};

using pool_schedule = decltype(lexec::schedule(std::declval<lexec::detail::pool_scheduler>()));
using parallel_bulk = decltype(std::declval<pool_schedule>() | lexec::bulk_chunked(lexec::par, 8, ignore_chunk{}));
using sequenced_bulk = decltype(std::declval<pool_schedule>() | lexec::bulk_chunked(lexec::seq, 8, ignore_chunk{}));

template <class Sndr>
using transformed_t = lexec::detail::remove_cvref_t<decltype(lexec::transform_sender(std::declval<Sndr>(), lexec::env<>{}))>;

template <class T>
inline constexpr bool is_pool_bulk_sender_v = false;
template <bool Chunked, class Child, class Shape, class Fn>
inline constexpr bool is_pool_bulk_sender_v<lexec::detail::pool_bulk_sender<Chunked, Child, Shape, Fn>> = true;

// The pool's domain takes over parallel bulk work that starts on the pool, and bulk
// through its lowering to bulk_chunked; sequenced bulk work keeps the default.
static_assert(is_pool_bulk_sender_v<transformed_t<parallel_bulk>>);
static_assert(is_pool_bulk_sender_v<transformed_t<decltype(std::declval<pool_schedule>() |
                                                           lexec::bulk(lexec::par_unseq, 8, ignore_index{}))>>);
static_assert(is_pool_bulk_sender_v<transformed_t<decltype(std::declval<pool_schedule>() |
                                                           lexec::bulk_unchunked(lexec::par, 8, ignore_index{}))>>);
static_assert(std::is_same_v<lexec::tag_of_t<transformed_t<sequenced_bulk>>, lexec::bulk_chunked_t>);
static_assert(std::is_same_v<transformed_t<decltype(lexec::just() | lexec::bulk_chunked(lexec::par, 8, ignore_chunk{}))>,
                             decltype(lexec::just() | lexec::bulk_chunked(lexec::par, 8, ignore_chunk{}))>);

// Values are stored, so the pool's version sends them decayed.
static_assert(std::is_same_v<completion_signatures_of_t<decltype(std::declval<pool_schedule>() |
                                                                 lexec::then(value_of_seven{}) |
                                                                 lexec::bulk(lexec::par, 4, ignore_index{})),
                                                        lexec::env<>>,
                             completion_signatures<set_value_t(int), set_stopped_t()>>);

// Records which indices, and which chunks, the function saw.
struct coverage {
    explicit coverage(std::size_t const size) : hits(std::make_unique<std::atomic<int>[]>(size)), size_(size) {}

    void hit(std::size_t const i) noexcept { hits[i].fetch_add(1, std::memory_order_relaxed); }

    bool each_once() const noexcept {
        for (auto i = std::size_t{0}; i < size_; ++i) {
            if (hits[i].load(std::memory_order_relaxed) != 1) {
                return false;
            }
        }
        return true;
    }

    std::unique_ptr<std::atomic<int>[]> hits;
    std::size_t size_;
};

} // namespace

TEST_CASE("bulk on a static_thread_pool runs every index exactly once") {
    for (auto const threads : {1u, 2u, 4u}) {
        auto pool = lexec::static_thread_pool{threads};
        for (auto const shape : {1, 2, 3, 7, 64, 1000, 100'003}) {
            auto seen = coverage{static_cast<std::size_t>(shape)};
            lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                             lexec::bulk(lexec::par, shape, [&seen](int i) noexcept { seen.hit(static_cast<std::size_t>(i)); }));
            CHECK_MESSAGE(seen.each_once(), "threads ", threads, ", shape ", shape);
        }
    }
}

TEST_CASE("bulk_chunked on a static_thread_pool covers the range with disjoint chunks") {
    auto pool = lexec::static_thread_pool{4};
    auto mutex = std::mutex{};
    auto chunks = std::vector<std::pair<long, long>>{};
    lexec::sync_wait(lexec::schedule(pool.get_scheduler()) | lexec::bulk_chunked(lexec::par, 1001L, [&](long b, long e) {
                         auto const lock = std::lock_guard<std::mutex>{mutex};
                         chunks.emplace_back(b, e);
                     }));
    std::sort(chunks.begin(), chunks.end());
    REQUIRE(not chunks.empty());
    CHECK(chunks.size() > 1);
    CHECK(chunks.front().first == 0);
    CHECK(chunks.back().second == 1001);
    for (auto i = std::size_t{1}; i < chunks.size(); ++i) {
        CHECK(chunks[i - 1].second == chunks[i].first);
        CHECK(chunks[i].first < chunks[i].second);
    }
}

TEST_CASE("bulk_unchunked on a static_thread_pool calls the function once per index") {
    auto pool = lexec::static_thread_pool{3};
    auto seen = coverage{500};
    lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                     lexec::bulk_unchunked(lexec::par, 500u, [&seen](unsigned i) noexcept { seen.hit(i); }));
    CHECK(seen.each_once());
}

TEST_CASE("pool bulk hands the function lvalues of the values and then sends them on") {
    auto pool = lexec::static_thread_pool{4};
    auto const result =
        lexec::sync_wait(lexec::schedule(pool.get_scheduler()) | lexec::then([] { return std::vector<int>(5000); }) |
                         lexec::bulk(lexec::par, 5000, [](int i, std::vector<int> &v) noexcept {
                             v[static_cast<std::size_t>(i)] = 2 * i;
                         }));
    auto const &values = std::get<0>(*result);
    auto expected = std::vector<int>(5000);
    for (auto i = 0; i < 5000; ++i) {
        expected[static_cast<std::size_t>(i)] = 2 * i;
    }
    CHECK(values == expected);
}

TEST_CASE("pool bulk moves each value once into the operation and never copies it") {
    auto pool = lexec::static_thread_pool{2};
    lexec_test::counted::reset();
    auto const result = lexec::sync_wait(
        lexec::schedule(pool.get_scheduler()) | lexec::then([]() noexcept { return lexec_test::counted{1}; }) |
        lexec::bulk(lexec::par, 16, [](int, lexec_test::counted &) noexcept {}) |
        lexec::then([](lexec_test::counted &&c) noexcept { return c.value; }));
    CHECK(std::get<0>(*result) == 1);
    CHECK(lexec_test::counted::counts.copies == 0);
    CHECK(lexec_test::counted::counts.moves == 1);
}

TEST_CASE("pool bulk passes move-only values through") {
    auto pool = lexec::static_thread_pool{2};
    auto const result = lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                                         lexec::then([] { return std::make_unique<std::atomic<int>>(0); }) |
                                         lexec::bulk(lexec::par, 100, [](int, std::unique_ptr<std::atomic<int>> &p) noexcept {
                                             p->fetch_add(1, std::memory_order_relaxed);
                                         }));
    CHECK(std::get<0>(*result)->load() == 100);
}

TEST_CASE("an empty shape on a static_thread_pool invokes nothing and sends the values") {
    auto pool = lexec::static_thread_pool{2};
    auto calls = std::atomic<int>{0};
    auto const result = lexec::sync_wait(lexec::schedule(pool.get_scheduler()) | lexec::then([] { return 3; }) |
                                         lexec::bulk(lexec::par, 0, [&calls](int, int) noexcept { ++calls; }));
    CHECK(std::get<0>(*result) == 3);
    CHECK(calls == 0);
}

TEST_CASE("consecutive pool bulk operations each see the previous one's writes") {
    auto pool = lexec::static_thread_pool{4};
    auto const result = lexec::sync_wait(
        lexec::schedule(pool.get_scheduler()) | lexec::then([] { return std::vector<int>(2048); }) |
        lexec::bulk(lexec::par, 2048, [](int i, std::vector<int> &v) noexcept { v[static_cast<std::size_t>(i)] = i; }) |
        lexec::bulk(lexec::par, 2048, [](int i, std::vector<int> &v) noexcept { v[static_cast<std::size_t>(i)] *= 3; }) |
        lexec::then([](std::vector<int> &&v) noexcept {
            auto sum = 0L;
            for (auto const x : v) {
                sum += x;
            }
            return sum;
        }));
    CHECK(std::get<0>(*result) == 3L * 2047 * 2048 / 2);
}

TEST_CASE("stopped passes through pool bulk") {
    auto pool = lexec::static_thread_pool{2};
    auto calls = std::atomic<int>{0};
    auto const stopped = lexec::sync_wait(
        lexec::schedule(pool.get_scheduler()) |
        lexec::let_value([] { return lexec_test::value_or_stopped_sender{true, 0}; }) |
        lexec::bulk(lexec::par, 4, [&calls](int, int) noexcept { calls.fetch_add(1, std::memory_order_relaxed); }));
    CHECK_FALSE(stopped.has_value());
    CHECK(calls == 0);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("errors pass through pool bulk") {
    auto pool = lexec::static_thread_pool{2};
    auto calls = std::atomic<int>{0};
    auto const failing =
        lexec::schedule(pool.get_scheduler()) | lexec::then([]() -> int { throw std::logic_error{"before"}; }) |
        lexec::bulk(lexec::par, 4, [&calls](int, int) noexcept { calls.fetch_add(1, std::memory_order_relaxed); });
    CHECK_THROWS_AS(lexec::sync_wait(failing), std::logic_error);
    CHECK(calls == 0);
}

TEST_CASE("the first exception from the function on a static_thread_pool becomes the error") {
    auto pool = lexec::static_thread_pool{4};
    auto const failing = lexec::schedule(pool.get_scheduler()) | lexec::bulk(lexec::par, 10'000, [](int i) {
                             if (i % 1000 == 999) {
                                 throw std::runtime_error{"boom"};
                             }
                         });
    CHECK_THROWS_AS(lexec::sync_wait(failing), std::runtime_error);
}
#endif

TEST_CASE("pool bulk spreads the work over the workers") {
    auto pool = lexec::static_thread_pool{4};
    auto mutex = std::mutex{};
    auto threads = std::set<std::thread::id>{};
    // Each chunk waits a little, so that sleeping workers have time to join.
    lexec::sync_wait(lexec::schedule(pool.get_scheduler()) | lexec::bulk_chunked(lexec::par, 16, [&](int, int) {
                         std::this_thread::sleep_for(std::chrono::milliseconds{2});
                         auto const lock = std::lock_guard<std::mutex>{mutex};
                         threads.insert(std::this_thread::get_id());
                     }));
    CHECK(threads.size() > 1);
}

TEST_CASE("bulk jobs submitted concurrently from several threads all complete correctly") {
    auto pool = lexec::static_thread_pool{4};
    constexpr auto submitters = 4;
    constexpr auto rounds = 50;
    auto failures = std::atomic<int>{0};
    auto run = [&] {
        for (auto round = 0; round < rounds; ++round) {
            auto const shape = 100 + round * 37;
            auto seen = coverage{static_cast<std::size_t>(shape)};
            lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                             lexec::bulk(lexec::par, shape, [&seen](int i) noexcept { seen.hit(static_cast<std::size_t>(i)); }));
            if (not seen.each_once()) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    auto threads = std::vector<std::thread>{};
    for (auto i = 0; i < submitters; ++i) {
        threads.emplace_back(run);
    }
    for (auto &t : threads) {
        t.join();
    }
    CHECK(failures == 0);
}

// Many jobs with few chunks list and unlist often; the pauses let workers fall asleep
// between jobs, so that publishing races with going to sleep.
TEST_CASE("small bulk jobs from several threads, with idle gaps, all complete correctly") {
    auto pool = lexec::static_thread_pool{4};
    auto failures = std::atomic<int>{0};
    auto run = [&](int const seed) {
        for (auto job = 0; job < 300; ++job) {
            auto const shape = 2 + (job + seed) % 8;
            auto seen = coverage{static_cast<std::size_t>(shape)};
            lexec::sync_wait(lexec::schedule(pool.get_scheduler()) |
                             lexec::bulk(lexec::par, shape, [&seen](int i) noexcept { seen.hit(static_cast<std::size_t>(i)); }));
            if (not seen.each_once()) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
            if (job % 50 == 49) {
                std::this_thread::sleep_for(std::chrono::milliseconds{3});
            }
        }
    };
    auto threads = std::vector<std::thread>{};
    for (auto i = 0; i < 3; ++i) {
        threads.emplace_back(run, i);
    }
    for (auto &t : threads) {
        t.join();
    }
    CHECK(failures == 0);
}

TEST_CASE("bulk under when_all on a static_thread_pool runs the jobs side by side") {
    auto pool = lexec::static_thread_pool{4};
    auto first = coverage{3000};
    auto second = coverage{5000};
    auto const sch = pool.get_scheduler();
    lexec::sync_wait(lexec::when_all(
        lexec::schedule(sch) | lexec::bulk(lexec::par, 3000, [&first](int i) noexcept { first.hit(static_cast<std::size_t>(i)); }),
        lexec::schedule(sch) |
            lexec::bulk(lexec::par, 5000, [&second](int i) noexcept { second.hit(static_cast<std::size_t>(i)); })));
    CHECK(first.each_once());
    CHECK(second.each_once());
}
