// Benchmarks shared by lexec and stdexec: the including file defines namespace `ex` and
// the type `thread_pool` before including this file.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

namespace pool_bench {

using clock_type = std::chrono::steady_clock;

struct count_down_receiver {
    using receiver_concept = ex::receiver_t;

    void set_value() && noexcept { remaining->fetch_sub(1, std::memory_order_release); }
    void set_stopped() && noexcept { remaining->fetch_sub(1, std::memory_order_release); }

    std::atomic<std::int64_t> *remaining;
};

double percentile(std::vector<double> &samples, double const fraction) {
    auto const index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(index), samples.end());
    return samples[index];
}

// sync_wait(schedule(sch) | then(noop)): a hop to the pool and back to a waiting thread.
void round_trip_latency(thread_pool &pool, int const iterations) {
    auto samples = std::vector<double>();
    samples.reserve(static_cast<std::size_t>(iterations));
    auto const scheduler = pool.get_scheduler();
    for (auto i = 0; i < iterations; ++i) {
        auto const begin = clock_type::now();
        ex::sync_wait(ex::schedule(scheduler) | ex::then([]() noexcept {}));
        samples.push_back(std::chrono::duration<double, std::nano>(clock_type::now() - begin).count());
    }
    std::printf("round_trip_ns p50=%.0f p99=%.0f p999=%.0f\n", percentile(samples, 0.5), percentile(samples, 0.99),
                percentile(samples, 0.999));
}

// Each submitter starts `per_submitter` schedule operations at once; the time until all
// complete gives tasks per second. The operations are built in place, never allocated
// one by one.
void submit_throughput(thread_pool &pool, int const submitters, std::int64_t const per_submitter) {
    auto const scheduler = pool.get_scheduler();
    using operation = decltype(ex::connect(ex::schedule(scheduler), count_down_receiver{nullptr}));
    auto remaining = std::atomic<std::int64_t>{submitters * per_submitter};
    auto go = std::atomic<bool>{false};
    auto storage = std::vector<std::unique_ptr<std::aligned_storage_t<sizeof(operation), alignof(operation)>[]>>();
    for (auto s = 0; s < submitters; ++s) {
        storage.emplace_back(new std::aligned_storage_t<sizeof(operation), alignof(operation)>[static_cast<std::size_t>(per_submitter)]);
    }
    auto threads = std::vector<std::thread>();
    for (auto s = 0; s < submitters; ++s) {
        threads.emplace_back([&, s] {
            auto *const slots = storage[static_cast<std::size_t>(s)].get();
            for (auto i = std::int64_t{0}; i < per_submitter; ++i) {
                ::new (static_cast<void *>(&slots[i])) operation(ex::connect(ex::schedule(scheduler), count_down_receiver{&remaining}));
            }
            while (not go.load(std::memory_order_acquire)) {
            }
            for (auto i = std::int64_t{0}; i < per_submitter; ++i) {
                ex::start(*std::launder(reinterpret_cast<operation *>(&slots[i])));
            }
        });
    }
    auto const begin = clock_type::now();
    go.store(true, std::memory_order_release);
    while (remaining.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    auto const seconds = std::chrono::duration<double>(clock_type::now() - begin).count();
    for (auto &thread : threads) {
        thread.join();
    }
    for (auto s = 0; s < submitters; ++s) {
        auto *const slots = storage[static_cast<std::size_t>(s)].get();
        for (auto i = std::int64_t{0}; i < per_submitter; ++i) {
            std::launder(reinterpret_cast<operation *>(&slots[i]))->~operation();
        }
    }
    std::printf("throughput submitters=%d tasks_per_second=%.3g\n", submitters,
                static_cast<double>(submitters * per_submitter) / seconds);
}

// A task on the pool schedules eight children on the pool and waits for all of them.
void fan_out(thread_pool &pool, int const iterations) {
    auto const scheduler = pool.get_scheduler();
    auto const child = [scheduler] { return ex::schedule(scheduler) | ex::then([]() noexcept {}); };
    auto const begin = clock_type::now();
    for (auto i = 0; i < iterations; ++i) {
        ex::sync_wait(ex::schedule(scheduler) | ex::let_value([child]() noexcept {
                          return ex::when_all(child(), child(), child(), child(), child(), child(), child(), child());
                      }));
    }
    auto const micros = std::chrono::duration<double, std::micro>(clock_type::now() - begin).count();
    std::printf("fan_out_8 us_per_iteration=%.2f\n", micros / iterations);
}

inline void run_all() {
    auto pool = thread_pool{8};
    round_trip_latency(pool, 200000);
    for (auto const submitters : {1, 4, 8}) {
        submit_throughput(pool, submitters, 200000);
    }
    fan_out(pool, 20000);
}

} // namespace pool_bench
