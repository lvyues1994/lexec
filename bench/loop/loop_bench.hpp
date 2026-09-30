// Benchmarks shared by lexec and stdexec: the including file defines namespace `ex`, the
// namespace `loops` that holds repeat_n and repeat_until, and the type `thread_pool`,
// before including this file.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace loop_bench {

using clock_type = std::chrono::steady_clock;

template <class Fn>
double nanoseconds_per(std::int64_t const iterations, Fn &&fn) {
    auto const begin = clock_type::now();
    fn();
    return std::chrono::duration<double, std::nano>(clock_type::now() - begin).count() / static_cast<double>(iterations);
}

void check(std::int64_t const count, std::int64_t const expected) {
    if (count != expected) {
        std::fprintf(stderr, "loop ran %lld times instead of %lld\n", static_cast<long long>(count),
                     static_cast<long long>(expected));
        std::abort();
    }
}

// Iterations that complete synchronously measure the loop itself: reconnecting the child
// and going through the trampoline, which bounces every few iterations.
void synchronous(std::int64_t const iterations) {
    auto count = std::int64_t{0};
    auto const repeat_n = nanoseconds_per(iterations, [&] {
        ex::sync_wait(loops::repeat_n(ex::just() | ex::then([&count]() noexcept { ++count; }),
                                      static_cast<std::size_t>(iterations)));
    });
    check(count, iterations);
    count = 0;
    auto const repeat_until = nanoseconds_per(iterations, [&] {
        ex::sync_wait(loops::repeat_until(
            ex::just() | ex::then([&count, iterations]() noexcept { return ++count == iterations; })));
    });
    check(count, iterations);
    std::printf("sync_repeat_n ns_per_iteration=%.2f\n", repeat_n);
    std::printf("sync_repeat_until ns_per_iteration=%.2f\n", repeat_until);
}

// Each iteration hops to a one-worker pool, submitting from the worker itself.
void on_pool(thread_pool &pool, std::int64_t const iterations) {
    auto const scheduler = pool.get_scheduler();
    auto count = std::int64_t{0};
    auto const repeat_n = nanoseconds_per(iterations, [&] {
        ex::sync_wait(loops::repeat_n(ex::schedule(scheduler) | ex::then([&count]() noexcept { ++count; }),
                                      static_cast<std::size_t>(iterations)));
    });
    check(count, iterations);
    std::printf("pool_repeat_n ns_per_iteration=%.1f\n", repeat_n);
}

inline void run_all() {
    synchronous(10'000'000);
    auto pool = thread_pool{1};
    on_pool(pool, 1'000'000);
}

} // namespace loop_bench
