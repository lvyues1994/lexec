#pragma once

#include <cstdint>
#include <thread>

namespace lexec::detail {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield");
#endif
}

// Busy-waits briefly, then yields the time slice so a descheduled lock holder can run.
struct spin_wait {
    void wait() noexcept {
        if (count < kYieldThreshold) {
            ++count;
            cpu_relax();
        } else {
            std::this_thread::yield();
        }
    }

private:
    static constexpr std::uint32_t kYieldThreshold = 20;
    std::uint32_t count = 0;
};

} // namespace lexec::detail
