// The baseline: a fixed team of std::threads, the calling thread included, that splits
// [0, size) into one equal part per thread. Helpers spin between jobs, so starting a job
// never waits for a thread to wake, and no scheduler stands between the caller and them.

#include "bulk_workloads.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace bulk_bench {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

struct team {
    explicit team(unsigned const threads) : size(threads) {
        for (auto index = 1u; index < threads; ++index) {
            helpers.emplace_back([this, index] { help(index); });
        }
    }

    team(team &&) = delete;

    ~team() {
        stopping.store(true, std::memory_order_release);
        for (auto &helper : helpers) {
            helper.join();
        }
    }

    template <class Kernel>
    void run(Kernel const &kernel) {
        work = &run_part<Kernel>;
        work_data = &kernel;
        finished.store(0, std::memory_order_relaxed);
        epoch.fetch_add(1, std::memory_order_release);
        work(work_data, 0, size);
        while (finished.load(std::memory_order_acquire) != size - 1) {
            cpu_relax();
        }
    }

    template <class Kernel>
    static void run_part(void const *const data, unsigned const index, unsigned const parts) {
        auto const &kernel = *static_cast<Kernel const *>(data);
        auto const begin = static_cast<int>(static_cast<std::int64_t>(Kernel::size) * index / parts);
        auto const end = static_cast<int>(static_cast<std::int64_t>(Kernel::size) * (index + 1) / parts);
        for (auto i = begin; i != end; ++i) {
            kernel(i);
        }
    }

    void help(unsigned const index) {
        auto seen = std::uint64_t{0};
        while (true) {
            auto current = epoch.load(std::memory_order_acquire);
            while (current == seen) {
                if (stopping.load(std::memory_order_acquire)) {
                    return;
                }
                cpu_relax();
                current = epoch.load(std::memory_order_acquire);
            }
            seen = current;
            work(work_data, index, size);
            finished.fetch_add(1, std::memory_order_release);
        }
    }

    unsigned size;
    void (*work)(void const *, unsigned, unsigned) = nullptr;
    void const *work_data = nullptr;
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<unsigned> finished{0};
    std::atomic<bool> stopping{false};
    std::vector<std::thread> helpers;
};

} // namespace bulk_bench

int main() {
    bulk_bench::run_all([](unsigned const threads, auto const &...kernels) {
        auto workers = bulk_bench::team{threads};
        auto const run = [&workers](auto const &kernel) { workers.run(kernel); };
        (bulk_bench::measure(threads, kernels, run), ...);
    });
}
