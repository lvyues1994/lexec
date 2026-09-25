// The data-parallel workloads every bulk benchmark runs, and how they are timed.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace bulk_bench {

using clock_type = std::chrono::steady_clock;

inline constexpr unsigned thread_counts[] = {1, 2, 4, 8, 16, 24};

// Compute-bound: a dependent chain of multiply-adds per element.
struct compute_kernel {
    static constexpr char const *name = "compute";
    static constexpr int size = 1 << 16;
    static constexpr int repetitions = 30;

    void operator()(int const i) const noexcept {
        auto x = static_cast<float>(i) * 1e-6f;
        for (auto step = 0; step < 256; ++step) {
            x = x * 0.999f + 0.001f;
        }
        (*out)[static_cast<std::size_t>(i)] = x;
    }

    std::vector<float> *out;
};

// Memory-bound: y = 3x + y over arrays much larger than the caches.
struct saxpy_kernel {
    static constexpr char const *name = "saxpy";
    static constexpr int size = 1 << 24;
    static constexpr int repetitions = 20;

    void operator()(int const i) const noexcept {
        auto const index = static_cast<std::size_t>(i);
        (*y)[index] = 3.0f * (*x)[index] + (*y)[index];
    }

    std::vector<float> const *x;
    std::vector<float> *y;
};

// Overhead-bound: so little work that the time is that of starting and joining.
struct small_kernel {
    static constexpr char const *name = "small";
    static constexpr int size = 1 << 10;
    static constexpr int repetitions = 2000;

    void operator()(int const i) const noexcept { (*out)[static_cast<std::size_t>(i)] += 1.0f; }

    std::vector<float> *out;
};

inline double median(std::vector<double> samples) {
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2), samples.end());
    return samples[samples.size() / 2];
}

// Times run(kernel) after a few warm-up runs and prints the median in microseconds.
template <class Kernel, class Run>
void measure(unsigned const threads, Kernel const &kernel, Run &&run) {
    for (auto warm_up = 0; warm_up < 3; ++warm_up) {
        run(kernel);
    }
    auto samples = std::vector<double>();
    for (auto rep = 0; rep < Kernel::repetitions; ++rep) {
        auto const begin = clock_type::now();
        run(kernel);
        samples.push_back(std::chrono::duration<double, std::micro>(clock_type::now() - begin).count());
    }
    std::printf("bulk workload=%s threads=%u us=%.2f\n", Kernel::name, threads, median(samples));
}

// Calls run_with(threads, kernel) for every thread count and workload.
template <class RunWith>
void run_all(RunWith &&run_with) {
    auto compute_out = std::vector<float>(compute_kernel::size);
    auto x = std::vector<float>(saxpy_kernel::size, 1.0f);
    auto y = std::vector<float>(saxpy_kernel::size, 2.0f);
    auto small_out = std::vector<float>(small_kernel::size);
    for (auto const threads : thread_counts) {
        run_with(threads, compute_kernel{&compute_out}, saxpy_kernel{&x, &y}, small_kernel{&small_out});
    }
}

} // namespace bulk_bench
