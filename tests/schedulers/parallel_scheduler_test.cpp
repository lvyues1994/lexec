#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <ostream>
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

static_assert(lexec::is_scheduler_v<lexec::parallel_scheduler>);
#if LEXEC_HAS_EXCEPTIONS
static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(), set_error_t(std::exception_ptr), set_stopped_t()>,
              completion_signatures_of_t<decltype(lexec::schedule(std::declval<lexec::parallel_scheduler>()))>>);
#else
static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(), set_stopped_t()>,
              completion_signatures_of_t<decltype(lexec::schedule(std::declval<lexec::parallel_scheduler>()))>>);
#endif

template <class T>
inline constexpr bool is_parallel_bulk_sender_v = false;
template <bool Chunked, bool Parallel, class Child, class Shape, class Fn>
inline constexpr bool is_parallel_bulk_sender_v<lexec::detail::parallel_bulk_sender<Chunked, Parallel, Child, Shape, Fn>> =
    true;

using parallel_schedule = decltype(lexec::schedule(std::declval<lexec::parallel_scheduler>()));
template <class Sndr>
using transformed_t = lexec::detail::remove_cvref_t<decltype(lexec::transform_sender(std::declval<Sndr>(), lexec::env<>{}))>;

// The parallel scheduler's domain takes over bulk work under every policy.
static_assert(is_parallel_bulk_sender_v<
              transformed_t<decltype(std::declval<parallel_schedule>() | lexec::bulk(lexec::par, 4, ignore_index{}))>>);
static_assert(is_parallel_bulk_sender_v<
              transformed_t<decltype(std::declval<parallel_schedule>() | lexec::bulk_unchunked(lexec::seq, 4, ignore_index{}))>>);

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

TEST_CASE("get_parallel_scheduler returns equal schedulers with parallel forward progress") {
    auto const first = lexec::get_parallel_scheduler();
    auto const second = lexec::get_parallel_scheduler();
    CHECK(first == second);
    CHECK_FALSE(first != second);
    CHECK(lexec::get_forward_progress_guarantee(first) == lexec::forward_progress_guarantee::parallel);
    CHECK(lexec::get_forward_progress_guarantee(lexec::inline_scheduler{}) ==
          lexec::forward_progress_guarantee::weakly_parallel);
}

TEST_CASE("work scheduled on the parallel scheduler runs on another thread") {
    auto const caller = std::this_thread::get_id();
    auto const result = lexec::sync_wait(lexec::schedule(lexec::get_parallel_scheduler()) |
                                         lexec::then([]() noexcept { return std::this_thread::get_id(); }));
    CHECK(std::get<0>(*result) != caller);
}

TEST_CASE("the parallel scheduler completes with stopped when stop was requested") {
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto const result = lexec::sync_wait(
        lexec::write_env(lexec::schedule(lexec::get_parallel_scheduler()), lexec::prop{lexec::get_stop_token, source.get_token()}));
    CHECK_FALSE(result.has_value());
}

TEST_CASE("bulk, bulk_chunked, and bulk_unchunked on the parallel scheduler run every index once") {
    auto const sch = lexec::get_parallel_scheduler();
    for (auto const shape : {1, 5, 64, 10'007}) {
        auto per_index = coverage{static_cast<std::size_t>(shape)};
        auto chunked = coverage{static_cast<std::size_t>(shape)};
        auto unchunked = coverage{static_cast<std::size_t>(shape)};
        auto sequenced = coverage{static_cast<std::size_t>(shape)};
        lexec::sync_wait(lexec::schedule(sch) |
                         lexec::bulk(lexec::par, shape, [&](int i) noexcept { per_index.hit(static_cast<std::size_t>(i)); }));
        lexec::sync_wait(lexec::schedule(sch) | lexec::bulk_chunked(lexec::par_unseq, shape, [&](int b, int e) noexcept {
                             for (; b != e; ++b) {
                                 chunked.hit(static_cast<std::size_t>(b));
                             }
                         }));
        lexec::sync_wait(lexec::schedule(sch) | lexec::bulk_unchunked(lexec::par, shape, [&](int i) noexcept {
                             unchunked.hit(static_cast<std::size_t>(i));
                         }));
        lexec::sync_wait(lexec::schedule(sch) |
                         lexec::bulk(lexec::seq, shape, [&](int i) noexcept { sequenced.hit(static_cast<std::size_t>(i)); }));
        CHECK_MESSAGE(per_index.each_once(), "shape ", shape);
        CHECK_MESSAGE(chunked.each_once(), "shape ", shape);
        CHECK_MESSAGE(unchunked.each_once(), "shape ", shape);
        CHECK_MESSAGE(sequenced.each_once(), "shape ", shape);
    }
}

TEST_CASE("bulk on the parallel scheduler passes the values through") {
    auto const result = lexec::sync_wait(lexec::schedule(lexec::get_parallel_scheduler()) |
                                         lexec::then([] { return std::vector<int>(3000); }) |
                                         lexec::bulk(lexec::par, 3000, [](int i, std::vector<int> &v) noexcept {
                                             v[static_cast<std::size_t>(i)] = i + 1;
                                         }));
    auto const &values = std::get<0>(*result);
    CHECK(values.front() == 1);
    CHECK(values.back() == 3000);
}

TEST_CASE("an empty shape on the parallel scheduler invokes nothing and sends the values") {
    auto calls = std::atomic<int>{0};
    auto const result = lexec::sync_wait(lexec::schedule(lexec::get_parallel_scheduler()) | lexec::then([] { return 9; }) |
                                         lexec::bulk(lexec::par, 0, [&calls](int, int) noexcept { ++calls; }));
    CHECK(std::get<0>(*result) == 9);
    CHECK(calls == 0);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("an exception from bulk work on the parallel scheduler becomes the error") {
    auto const failing = lexec::schedule(lexec::get_parallel_scheduler()) | lexec::bulk(lexec::par, 1000, [](int i) {
                             if (i == 500) {
                                 throw std::runtime_error{"boom"};
                             }
                         });
    CHECK_THROWS_AS(lexec::sync_wait(failing), std::runtime_error);
}
#endif
