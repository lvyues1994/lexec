#pragma once

#include <lexec/execution.hpp>

#include <thread>

namespace lexec_test {

// A run_loop driven by its own thread for as long as this object lives. The driver has
// run work once the constructor returns: MSVC constructs a module's thread_local objects,
// doctest's streams among them, when a thread starts, and allocation tests must not count
// that.
struct loop_thread {
    loop_thread() : driver{[this] { loop.run(); }} { lexec::sync_wait(lexec::schedule(loop.get_scheduler())); }
    loop_thread(loop_thread &&) = delete;
    ~loop_thread() {
        loop.finish();
        driver.join();
    }

    auto scheduler() noexcept { return loop.get_scheduler(); }
    std::thread::id id() const noexcept { return driver.get_id(); }

    lexec::run_loop loop;
    std::thread driver;
};

} // namespace lexec_test
