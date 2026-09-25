// The bulk benchmark on a sender library's thread pool: the including file defines
// namespace `ex` and the type `thread_pool` before including this file.

#include "bulk_workloads.hpp"

namespace bulk_bench {

// sync_wait(schedule(pool) | bulk(par, size, kernel)): the time includes the hop to the
// pool and the completion back on the waiting thread.
inline void run_on_pools() {
    run_all([](unsigned const threads, auto const &... kernels) {
        auto pool = thread_pool{threads};
        auto const scheduler = pool.get_scheduler();
        auto const run = [scheduler](auto const &kernel) {
            ex::sync_wait(ex::schedule(scheduler) |
                          ex::bulk(ex::par, std::remove_reference_t<decltype(kernel)>::size, kernel));
        };
        (measure(threads, kernels, run), ...);
    });
}

} // namespace bulk_bench
