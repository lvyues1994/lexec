#include "../support/loop_thread.hpp"
#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

namespace {

struct outcome_receiver {
    using receiver_concept = lexec::receiver_t;

    template <class... Vs>
    void set_value(Vs &&...) && noexcept {
        values->fetch_add(1, std::memory_order_relaxed);
        done->store(true, std::memory_order_release);
    }

    void set_stopped() && noexcept {
        stops->fetch_add(1, std::memory_order_relaxed);
        done->store(true, std::memory_order_release);
    }

    template <class E>
    void set_error(E &&) && noexcept {
        errors->fetch_add(1, std::memory_order_relaxed);
        done->store(true, std::memory_order_release);
    }

    std::atomic<int> *values;
    std::atomic<int> *stops;
    std::atomic<int> *errors;
    std::atomic<bool> *done;
};

void wait_for(std::atomic<bool> const &done) {
    while (not done.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

} // namespace

TEST_CASE("an error on one thread stops a sibling waiting for stop on another") {
    auto first = lexec_test::loop_thread{};
    auto second = lexec_test::loop_thread{};
    for (auto i = 0; i < 200; ++i) {
        auto const result = lexec::sync_wait(
            lexec::when_all(lexec::starts_on(first.scheduler(), lexec_test::until_stopped_sender{}),
                            lexec::schedule(second.scheduler()) |
                                lexec::let_value([]() noexcept { return lexec::just_error(7); })) |
            lexec::upon_error([](auto) noexcept {}));
        REQUIRE(result.has_value());
    }
}

TEST_CASE("a receiver's stop request racing with completions on two threads completes exactly once") {
    auto first = lexec_test::loop_thread{};
    auto second = lexec_test::loop_thread{};
    auto values = std::atomic<int>{0};
    auto stops = std::atomic<int>{0};
    auto errors = std::atomic<int>{0};
    for (auto i = 0; i < 500; ++i) {
        auto source = lexec::inplace_stop_source{};
        auto done = std::atomic<bool>{false};
        auto op = lexec::connect(
            lexec::write_env(lexec::when_all(lexec::schedule(first.scheduler()) | lexec::then([]() noexcept { return 1; }),
                                             lexec::schedule(second.scheduler()) | lexec::then([]() noexcept { return 2; })),
                             lexec::prop{lexec::get_stop_token, source.get_token()}),
            outcome_receiver{&values, &stops, &errors, &done});
        lexec::start(op);
        source.request_stop();
        wait_for(done);
    }
    CHECK(values.load() + stops.load() == 500);
    CHECK(errors.load() == 0);
}
