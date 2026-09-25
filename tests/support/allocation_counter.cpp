#include "allocation_counter.hpp"

#include <lexec/detail/config.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<std::size_t> allocations{0};

void *checked(void *const memory) {
    if (memory == nullptr) {
#if LEXEC_HAS_EXCEPTIONS
        throw std::bad_alloc{};
#else
        std::abort();
#endif
    }
    return memory;
}

void *counted_allocate(std::size_t const size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    return checked(std::malloc(size == 0 ? 1 : size));
}

// MSVC's CRT has no std::aligned_alloc, and its aligned blocks must be released with _aligned_free.
void *counted_allocate(std::size_t const size, std::align_val_t const align) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    auto const alignment = static_cast<std::size_t>(align);
    auto const bytes = size == 0 ? alignment : (size + alignment - 1) / alignment * alignment;
#if defined(_MSC_VER)
    return checked(_aligned_malloc(bytes, alignment));
#else
    return checked(std::aligned_alloc(alignment, bytes));
#endif
}

void release_aligned(void *const memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace

std::size_t lexec_test::allocation_count() noexcept { return allocations.load(std::memory_order_relaxed); }

void *operator new(std::size_t const size) { return counted_allocate(size); }
void *operator new[](std::size_t const size) { return counted_allocate(size); }
void *operator new(std::size_t const size, std::align_val_t const alignment) { return counted_allocate(size, alignment); }
void *operator new[](std::size_t const size, std::align_val_t const alignment) { return counted_allocate(size, alignment); }

void operator delete(void *const memory) noexcept { std::free(memory); }
void operator delete[](void *const memory) noexcept { std::free(memory); }
void operator delete(void *const memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *const memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *const memory, std::align_val_t) noexcept { release_aligned(memory); }
void operator delete[](void *const memory, std::align_val_t) noexcept { release_aligned(memory); }
void operator delete(void *const memory, std::size_t, std::align_val_t) noexcept { release_aligned(memory); }
void operator delete[](void *const memory, std::size_t, std::align_val_t) noexcept { release_aligned(memory); }
