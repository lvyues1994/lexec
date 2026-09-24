#include <lexec/stop_token.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

struct count_calls {
    int *calls;
    void operator()() noexcept { ++*calls; }
};

using counting_callback = lexec::inplace_stop_callback<count_calls>;

static_assert(lexec::is_stoppable_token_v<lexec::inplace_stop_token>);
static_assert(lexec::is_stoppable_token_v<lexec::never_stop_token>);
static_assert(lexec::is_unstoppable_token_v<lexec::never_stop_token>);
static_assert(not lexec::is_unstoppable_token_v<lexec::inplace_stop_token>);
static_assert(not std::is_move_constructible_v<counting_callback>);

} // namespace

TEST_CASE("request_stop runs a registered callback once and reports only the first request") {
    auto source = lexec::inplace_stop_source{};
    auto calls = 0;
    auto const callback = counting_callback{source.get_token(), count_calls{&calls}};
    CHECK_FALSE(source.stop_requested());
    CHECK(source.request_stop());
    CHECK_FALSE(source.request_stop());
    CHECK(source.stop_requested());
    CHECK(calls == 1);
}

TEST_CASE("a callback registered after stop was requested runs immediately") {
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto calls = 0;
    auto const callback = counting_callback{source.get_token(), count_calls{&calls}};
    CHECK(calls == 1);
}

TEST_CASE("a destroyed callback does not run") {
    auto source = lexec::inplace_stop_source{};
    auto calls = 0;
    auto callback = std::optional<counting_callback>{};
    callback.emplace(source.get_token(), count_calls{&calls});
    callback.reset();
    source.request_stop();
    CHECK(calls == 0);
}

TEST_CASE("tokens without a source never stop") {
    auto const token = lexec::inplace_stop_token{};
    CHECK_FALSE(token.stop_possible());
    CHECK_FALSE(token.stop_requested());
    auto calls = 0;
    auto const callback = counting_callback{token, count_calls{&calls}};
    CHECK(calls == 0);
}

namespace {

struct callback_slot;

struct self_destroying_callback {
    callback_slot *slot;
    void operator()() noexcept;
};

struct callback_slot {
    std::optional<lexec::inplace_stop_callback<self_destroying_callback>> callback;
};

void self_destroying_callback::operator()() noexcept { slot->callback.reset(); }

} // namespace

TEST_CASE("a callback may destroy itself while it runs") {
    auto source = lexec::inplace_stop_source{};
    auto slot = callback_slot{};
    slot.callback.emplace(source.get_token(), self_destroying_callback{&slot});
    source.request_stop();
    CHECK_FALSE(slot.callback.has_value());
}

TEST_CASE("destroying a callback waits while another thread runs it") {
    for (auto iteration = 0; iteration < 500; ++iteration) {
        auto source = lexec::inplace_stop_source{};
        auto target = std::make_unique<std::atomic<int>>(0);
        auto requester = std::thread{[&source] { source.request_stop(); }};
        {
            auto const callback = lexec::inplace_stop_callback{
                source.get_token(), [counter = target.get()]() noexcept { counter->fetch_add(1); }};
        }
        // Freeing the target is only safe if the destructor above waited for the callback.
        target.reset();
        requester.join();
    }
}

TEST_CASE("concurrent stop requests have exactly one winner") {
    for (auto iteration = 0; iteration < 200; ++iteration) {
        auto source = lexec::inplace_stop_source{};
        auto winners = std::atomic<int>{0};
        auto threads = std::vector<std::thread>{};
        for (auto i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                if (source.request_stop()) {
                    winners.fetch_add(1);
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        CHECK(winners.load() == 1);
    }
}

TEST_CASE("callbacks registered concurrently with a stop request each run exactly once") {
    for (auto iteration = 0; iteration < 200; ++iteration) {
        auto source = lexec::inplace_stop_source{};
        auto requested = std::atomic<bool>{false};
        auto failures = std::atomic<int>{0};
        auto registrars = std::vector<std::thread>{};
        for (auto i = 0; i < 3; ++i) {
            registrars.emplace_back([&] {
                auto calls = std::atomic<int>{0};
                auto const callback = lexec::inplace_stop_callback{
                    source.get_token(), [&calls]() noexcept { calls.fetch_add(1); }};
                while (not requested.load()) {
                    std::this_thread::yield();
                }
                if (calls.load() != 1) {
                    failures.fetch_add(1);
                }
            });
        }
        source.request_stop();
        requested.store(true);
        for (auto &thread : registrars) {
            thread.join();
        }
        CHECK(failures.load() == 0);
    }
}
