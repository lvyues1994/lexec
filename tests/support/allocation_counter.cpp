#include "allocation_counter.hpp"

#include <lexec/detail/config.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::size_t> allocations{0};

void *counted_allocate(std::size_t const size, std::size_t const alignment) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    auto const bytes = size == 0 ? alignment : (size + alignment - 1) / alignment * alignment;
    auto *const memory = alignment <= alignof(std::max_align_t) ? std::malloc(bytes) : std::aligned_alloc(alignment, bytes);
    if (memory == nullptr) {
#if LEXEC_HAS_EXCEPTIONS
        throw std::bad_alloc{};
#else
        std::abort();
#endif
    }
    return memory;
}

} // namespace

std::size_t lexec_test::allocation_count() noexcept { return allocations.load(std::memory_order_relaxed); }

void *operator new(std::size_t const size) { return counted_allocate(size, alignof(std::max_align_t)); }
void *operator new[](std::size_t const size) { return counted_allocate(size, alignof(std::max_align_t)); }
void *operator new(std::size_t const size, std::align_val_t const alignment) {
    return counted_allocate(size, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t const size, std::align_val_t const alignment) {
    return counted_allocate(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *const memory) noexcept { std::free(memory); }
void operator delete[](void *const memory) noexcept { std::free(memory); }
void operator delete(void *const memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *const memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *const memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void *const memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void *const memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void *const memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
