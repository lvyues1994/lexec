#pragma once

// The BWoS queue of Wang et al., "BWoS: Formally Verified Block-based Work Stealing for
// Parallel Processing" (OSDI 2023), in its LIFO form, following the implementation in
// NVIDIA stdexec (exec/detail/bwos_lifo_queue.hpp, Apache-2.0 WITH LLVM-exception).

#include <lexec/detail/spin_wait.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lexec::detail {

inline constexpr std::size_t kCacheLine = 64;

constexpr std::size_t bit_ceil(std::size_t const value) noexcept {
    auto result = std::size_t{1};
    while (result < value) {
        result <<= 1;
    }
    return result;
}

// A bounded queue of T that one owner thread pushes to and pops from at the back, while
// any thread may steal from the front. Blocks let the owner run without contention
// inside a block; it synchronizes with thieves only when moving between blocks.
//
// Block counters hold a round in their upper 32 bits and a block index in the lower; the
// round, repeated in each block's head and steal positions, keeps a thief from mistaking
// a reused block for the one it started on.
template <class T>
struct bwos_queue {
    explicit bwos_queue(std::size_t const num_blocks, std::size_t const block_size)
        : blocks(std::make_unique<block[]>(bit_ceil(num_blocks))), mask(bit_ceil(num_blocks) - 1) {
        for (auto i = std::size_t{0}; i <= mask; ++i) {
            blocks[i].init(block_size);
        }
        blocks[0].reclaim(0);
    }

    bwos_queue(bwos_queue &&) = delete;

    // Owner only. T{} when the queue is empty.
    T pop_back() noexcept {
        auto owner = last_block.load(std::memory_order_relaxed);
        do {
            auto const result = blocks[owner & mask].get();
            if (result.status == fetch_status::success) {
                return result.value;
            }
            if (result.status == fetch_status::done) {
                return T{};
            }
        } while (advance_get_index(owner));
        return T{};
    }

    // Any thread. T{} when there is nothing to steal.
    T steal_front() noexcept {
        auto thief = start_block.load(std::memory_order_relaxed);
        do {
            auto const round = static_cast<std::uint32_t>(thief >> 32);
            auto &target = blocks[thief & mask];
            auto result = target.steal(round);
            while (result.status != fetch_status::done) {
                if (result.status == fetch_status::success) {
                    return result.value;
                }
                if (result.status == fetch_status::empty) {
                    return T{};
                }
                result = target.steal(round);
            }
        } while (advance_steal_index(thief));
        return T{};
    }

    // Owner only. False when the queue is full.
    bool push_back(T const value) noexcept {
        auto owner = last_block.load(std::memory_order_relaxed);
        do {
            if (blocks[owner & mask].put(value)) {
                return true;
            }
        } while (advance_put_index(owner));
        return false;
    }

private:
    enum class fetch_status : unsigned char { success, done, empty, conflict };

    struct fetch_result {
        fetch_status status;
        T value;
    };

    static constexpr std::uint64_t kIndexMask = 0xFFFF'FFFFu;

    struct block {
        void init(std::size_t const size_) {
            size = size_;
            ring = std::make_unique<T[]>(size_);
            head.store(0xFFFF'FFFF'0000'0000u | size_, std::memory_order_relaxed);
            tail.store(size_, std::memory_order_relaxed);
            steal_count.store(size_, std::memory_order_relaxed);
            steal_tail.store(0xFFFF'FFFF'0000'0000u | size_, std::memory_order_relaxed);
        }

        bool put(T const value) noexcept {
            auto const back = tail.load(std::memory_order_relaxed);
            if ((back & kIndexMask) >= size) {
                return false;
            }
            ring[static_cast<std::size_t>(back & kIndexMask)] = value;
            tail.store(back + 1, std::memory_order_release);
            return true;
        }

        fetch_result get() noexcept {
            auto const back = tail.load(std::memory_order_relaxed);
            auto const back_index = back & kIndexMask;
            if (back_index == 0 or (head.load(std::memory_order_relaxed) & kIndexMask) == back_index) {
                return {fetch_status::empty, T{}};
            }
            auto const value = ring[static_cast<std::size_t>(back_index - 1)];
            tail.store(back - 1, std::memory_order_release);
            return {fetch_status::success, value};
        }

        fetch_result steal(std::uint32_t const thief_round) noexcept {
            auto position = steal_tail.load(std::memory_order_relaxed);
            auto const index = position & kIndexMask;
            if (index == size) {
                return {static_cast<std::uint32_t>(position >> 32) == thief_round ? fetch_status::done : fetch_status::empty, T{}};
            }
            // Acquiring the tail makes the owner's writes of the stolen slot visible.
            if (index == tail.load(std::memory_order_acquire)) {
                return {fetch_status::empty, T{}};
            }
            if (not steal_tail.compare_exchange_strong(position, position + 1, std::memory_order_relaxed)) {
                return {fetch_status::conflict, T{}};
            }
            auto const value = ring[static_cast<std::size_t>(index)];
            // Releasing the count tells a reclaiming owner this read is done.
            steal_count.fetch_add(1, std::memory_order_release);
            return {fetch_status::success, value};
        }

        // The owner moves back to this block: thieves may finish what they claimed.
        void takeover() noexcept {
            auto const old_head = head.load(std::memory_order_relaxed);
            head.store(steal_tail.exchange(old_head, std::memory_order_relaxed), std::memory_order_relaxed);
        }

        // The owner moves on from this block: all of it becomes stealable.
        void grant() noexcept {
            auto const block_end = steal_tail.load(std::memory_order_relaxed);
            auto const old_head = head.exchange(block_end, std::memory_order_relaxed);
            steal_tail.store(old_head, std::memory_order_release);
        }

        void reduce_round() noexcept {
            auto const position = steal_tail.load(std::memory_order_relaxed);
            auto const round = static_cast<std::uint32_t>(position >> 32) - 1u;
            steal_tail.store((static_cast<std::uint64_t>(round) << 32) | (position & kIndexMask), std::memory_order_relaxed);
        }

        bool is_writable(std::uint32_t const round) const noexcept {
            auto const exhausted = (static_cast<std::uint64_t>(round - 1u) << 32) | size;
            return steal_tail.load(std::memory_order_relaxed) == exhausted;
        }

        // Waits until every thief that claimed an item of this block has read it.
        void reclaim(std::uint32_t const round) noexcept {
            auto const claimed = head.load(std::memory_order_relaxed) & kIndexMask;
            auto spin = spin_wait{};
            while (steal_count.load(std::memory_order_acquire) != claimed) {
                spin.wait();
            }
            auto const expanded_round = static_cast<std::uint64_t>(round) << 32;
            head.store(expanded_round, std::memory_order_relaxed);
            tail.store(0, std::memory_order_relaxed);
            steal_tail.store(expanded_round | size, std::memory_order_relaxed);
            steal_count.store(0, std::memory_order_relaxed);
        }

        alignas(kCacheLine) std::atomic<std::uint64_t> head{};
        alignas(kCacheLine) std::atomic<std::uint64_t> tail{};
        alignas(kCacheLine) std::atomic<std::uint64_t> steal_count{};
        alignas(kCacheLine) std::atomic<std::uint64_t> steal_tail{};
        std::unique_ptr<T[]> ring;
        std::size_t size = 0;
    };

    std::uint64_t next_counter(std::uint64_t const counter) const noexcept {
        auto const next_index = ((counter & kIndexMask) + 1) & mask;
        auto const round = static_cast<std::uint32_t>(counter >> 32) + static_cast<std::uint32_t>(next_index == 0);
        return (static_cast<std::uint64_t>(round) << 32) | next_index;
    }

    std::uint64_t previous_counter(std::uint64_t const counter) const noexcept {
        auto const index = counter & kIndexMask;
        auto const previous_index = (index + mask) & mask;
        auto const round = static_cast<std::uint32_t>(counter >> 32) - static_cast<std::uint32_t>(index == 0);
        return (static_cast<std::uint64_t>(round) << 32) | previous_index;
    }

    bool advance_get_index(std::uint64_t &owner) noexcept {
        if (start_block.load(std::memory_order_relaxed) == owner) {
            return false;
        }
        auto const predecessor = previous_counter(owner);
        blocks[owner & mask].reduce_round();
        blocks[predecessor & mask].takeover();
        last_block.store(predecessor, std::memory_order_relaxed);
        owner = predecessor;
        return true;
    }

    bool advance_put_index(std::uint64_t &owner) noexcept {
        auto const next_index = ((owner & kIndexMask) + 1) & mask;
        if (next_index == (owner & mask)) {
            return false;
        }
        auto const next_round = static_cast<std::uint32_t>(owner >> 32) + static_cast<std::uint32_t>(next_index == 0);
        auto &next = blocks[next_index];
        if (not next.is_writable(next_round)) {
            return false;
        }
        auto const first = start_block.load(std::memory_order_relaxed);
        if (next_index == (first & mask)) {
            start_block.store(next_counter(first), std::memory_order_relaxed);
        }
        blocks[owner & mask].grant();
        owner = (static_cast<std::uint64_t>(next_round) << 32) | next_index;
        next.reclaim(next_round);
        last_block.store(owner, std::memory_order_relaxed);
        return true;
    }

    bool advance_steal_index(std::uint64_t &thief) const noexcept {
        thief = next_counter(thief);
        return thief < last_block.load(std::memory_order_relaxed);
    }

    alignas(kCacheLine) std::atomic<std::uint64_t> last_block{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> start_block{0};
    std::unique_ptr<block[]> blocks;
    std::uint64_t mask;
};

} // namespace lexec::detail
