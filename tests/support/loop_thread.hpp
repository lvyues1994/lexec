#pragma once

#include <lexec/execution.hpp>

#include <thread>

namespace lexec_test {

// A run_loop driven by its own thread for as long as this object lives.
struct loop_thread {
    loop_thread() : driver{[this] { loop.run(); }} {}
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
