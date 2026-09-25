#include "bwos_queue.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

using queue = lexec::detail::bwos_queue<int *>;

} // namespace

TEST_CASE("the owner pops what it pushed, last in first out") {
    auto q = queue{4, 4};
    int items[8] = {};
    for (auto &item : items) {
        REQUIRE(q.push_back(&item));
    }
    for (auto i = 7; i >= 0; --i) {
        CHECK(q.pop_back() == &items[i]);
    }
    CHECK(q.pop_back() == nullptr);
}

TEST_CASE("a full queue rejects pushes and still returns everything it accepted") {
    auto q = queue{2, 2};
    int items[8] = {};
    auto accepted = 0;
    while (accepted < 8 and q.push_back(&items[accepted])) {
        ++accepted;
    }
    CHECK(accepted >= 2);
    CHECK(accepted <= 4);
    auto popped = 0;
    while (q.pop_back() != nullptr) {
        ++popped;
    }
    CHECK(popped == accepted);
}

TEST_CASE("thieves steal from the front of the blocks the owner has moved past") {
    auto q = queue{4, 2};
    int items[6] = {};
    for (auto &item : items) {
        REQUIRE(q.push_back(&item));
    }
    for (auto i = 0; i < 4; ++i) {
        CHECK(q.steal_front() == &items[i]);
    }
    // The owner's current block is its own until it moves on.
    CHECK(q.steal_front() == nullptr);
    CHECK(q.pop_back() == &items[5]);
    CHECK(q.pop_back() == &items[4]);
    CHECK(q.pop_back() == nullptr);
}

TEST_CASE("an owner and concurrent thieves take every item exactly once") {
    constexpr auto kItems = std::size_t{50000};
    auto q = queue{32, 8};
    auto values = std::vector<int>(kItems);
    auto taken = std::vector<std::atomic<int>>(kItems);
    auto const take = [&](int *const item) { taken[static_cast<std::size_t>(item - values.data())].fetch_add(1); };
    auto done = std::atomic<bool>{false};
    auto thieves = std::vector<std::thread>{};
    for (auto t = 0; t < 3; ++t) {
        thieves.emplace_back([&] {
            while (not done.load(std::memory_order_acquire)) {
                if (auto *const item = q.steal_front()) {
                    take(item);
                }
            }
        });
    }
    for (auto i = std::size_t{0}; i < kItems; ++i) {
        while (not q.push_back(&values[i])) {
            if (auto *const item = q.pop_back()) {
                take(item);
            }
        }
        if (i % 3 == 0) {
            if (auto *const item = q.pop_back()) {
                take(item);
            }
        }
    }
    while (auto *const item = q.pop_back()) {
        take(item);
    }
    done.store(true, std::memory_order_release);
    for (auto &thief : thieves) {
        thief.join();
    }
    auto missed_or_repeated = 0;
    for (auto const &count : taken) {
        missed_or_repeated += count.load() == 1 ? 0 : 1;
    }
    CHECK(missed_or_repeated == 0);
}
