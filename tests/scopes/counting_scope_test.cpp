#include "../support/test_receivers.hpp"
#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using lexec::completion_signatures;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

static_assert(lexec::is_scope_token_v<lexec::simple_counting_scope::token>);
static_assert(lexec::is_scope_token_v<lexec::counting_scope::token>);
static_assert(lexec::is_scope_association_v<lexec::simple_counting_scope::association>);
static_assert(lexec::is_scope_association_v<lexec::counting_scope::association>);
static_assert(not std::is_copy_constructible_v<lexec::counting_scope::association>);
static_assert(not lexec::is_scope_token_v<int>);

// associate adds set_stopped, for a scope that refuses the association.
static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(int), set_stopped_t()>,
              lexec::completion_signatures_of_t<decltype(lexec::associate(lexec::just(1), std::declval<lexec::simple_counting_scope::token>())),
                                                lexec::env<>>>);

// Counts allocations through it, and hands them to std::allocator.
template <class T>
struct counting_allocator {
    using value_type = T;

    counting_allocator() = default;
    explicit counting_allocator(int *count_) noexcept : count(count_) {}
    template <class U>
    counting_allocator(counting_allocator<U> const &other) noexcept : count(other.count) {}

    T *allocate(std::size_t const n) {
        ++*count;
        return std::allocator<T>{}.allocate(n);
    }

    void deallocate(T *const p, std::size_t const n) noexcept { std::allocator<T>{}.deallocate(p, n); }

    friend bool operator==(counting_allocator const &a, counting_allocator const &b) noexcept { return a.count == b.count; }
    friend bool operator!=(counting_allocator const &a, counting_allocator const &b) noexcept { return a.count != b.count; }

    int *count = nullptr;
};

} // namespace

TEST_CASE("an unused counting scope can be destroyed without joining") {
    auto scope = lexec::simple_counting_scope{};
    static_cast<void>(scope);
    auto closed = lexec::counting_scope{};
    closed.close();
}

TEST_CASE("join completes once every association has ended") {
    auto scope = lexec::simple_counting_scope{};
    auto log = lexec_test::completion_log{};
    {
        auto assoc = scope.get_token().try_associate();
        REQUIRE(static_cast<bool>(assoc));
        auto second = assoc.try_associate();
        CHECK(static_cast<bool>(second));
        auto op = lexec::connect(scope.join(), lexec_test::checked_receiver<set_value_t()>{&log});
        lexec::start(op);
        CHECK(log.value_count == 0);
        second = {};
        CHECK(log.value_count == 0);
        assoc = {};
        CHECK(log.value_count == 1);
    }
    // Joined: no new associations.
    CHECK_FALSE(static_cast<bool>(scope.get_token().try_associate()));
}

TEST_CASE("a closed scope accepts no associations and still joins") {
    auto scope = lexec::counting_scope{};
    auto kept = scope.get_token().try_associate();
    scope.close();
    CHECK_FALSE(static_cast<bool>(scope.get_token().try_associate()));
    CHECK_FALSE(static_cast<bool>(kept.try_associate()));
    kept = {};
    CHECK(lexec::sync_wait(scope.join()).has_value());
}

TEST_CASE("join completes on the waiting thread's start scheduler") {
    auto pool = lexec::static_thread_pool{2};
    auto scope = lexec::counting_scope{};
    lexec::spawn(lexec::schedule(pool.get_scheduler()) |
                     lexec::then([]() noexcept { std::this_thread::sleep_for(std::chrono::milliseconds{5}); }),
                 scope.get_token());
    auto const joined_on =
        lexec::sync_wait(scope.join() | lexec::then([]() noexcept { return std::this_thread::get_id(); }));
    CHECK(std::get<0>(*joined_on) == std::this_thread::get_id());
}

TEST_CASE("associate runs the sender while the scope is open, and stops once it is closed") {
    auto scope = lexec::simple_counting_scope{};
    auto const value = lexec::sync_wait(lexec::associate(lexec::just(5), scope.get_token()));
    CHECK(std::get<0>(*value) == 5);
    auto const piped = lexec::sync_wait(lexec::just(6) | lexec::associate(scope.get_token()));
    CHECK(std::get<0>(*piped) == 6);
    scope.close();
    auto const refused = lexec::sync_wait(lexec::associate(lexec::just(7), scope.get_token()));
    CHECK_FALSE(refused.has_value());
    CHECK(lexec::sync_wait(scope.join()).has_value());
}

TEST_CASE("an associate operation keeps its association until it is destroyed") {
    auto scope = lexec::simple_counting_scope{};
    auto join_log = lexec_test::completion_log{};
    auto join = lexec::connect(scope.join(), lexec_test::checked_receiver<set_value_t()>{&join_log});
    {
        auto log = lexec_test::completion_log{};
        auto op = lexec::connect(lexec::associate(lexec::just(), scope.get_token()),
                                 lexec_test::checked_receiver<set_value_t(), set_stopped_t()>{&log});
        lexec::start(join);
        lexec::start(op);
        CHECK(log.value_count == 1);
        CHECK(join_log.value_count == 0);
    }
    CHECK(join_log.value_count == 1);
}

TEST_CASE("copying an associate sender makes a new association") {
    auto scope = lexec::simple_counting_scope{};
    {
        auto const original = lexec::associate(lexec::just(1), scope.get_token());
        auto copy = original;
        CHECK(std::get<0>(*lexec::sync_wait(original)) == 1);
        CHECK(std::get<0>(*lexec::sync_wait(std::move(copy))) == 1);
    }
    // A used scope must be joined, which waits for every sender holding an association.
    CHECK(lexec::sync_wait(scope.join()).has_value());
}

TEST_CASE("spawned work runs to completion before join completes") {
    auto pool = lexec::static_thread_pool{4};
    auto scope = lexec::counting_scope{};
    auto done = std::atomic<int>{0};
    for (auto i = 0; i < 100; ++i) {
        lexec::spawn(lexec::schedule(pool.get_scheduler()) | lexec::then([&done]() noexcept { done.fetch_add(1); }),
                     scope.get_token());
    }
    lexec::sync_wait(scope.join());
    CHECK(done == 100);
}

TEST_CASE("a counting scope's stop request reaches the spawned work") {
    auto scope = lexec::counting_scope{};
    lexec::spawn(lexec_test::until_stopped_sender{}, scope.get_token());
    lexec::spawn(lexec_test::until_stopped_sender{}, scope.get_token());
    scope.request_stop();
    CHECK(lexec::sync_wait(scope.join()).has_value());
}

TEST_CASE("spawn allocates with the environment's allocator, and not at all into a closed scope") {
    auto count = 0;
    auto scope = lexec::simple_counting_scope{};
    auto ran = false;
    lexec::spawn(lexec::just() | lexec::then([&ran]() noexcept { ran = true; }), scope.get_token(),
                 lexec::prop{lexec::get_allocator, counting_allocator<std::byte>{&count}});
    CHECK(ran);
    CHECK(count == 1);
    scope.close();
    auto late = false;
    lexec::spawn(lexec::just() | lexec::then([&late]() noexcept { late = true; }), scope.get_token(),
                 lexec::prop{lexec::get_allocator, counting_allocator<std::byte>{&count}});
    CHECK_FALSE(late);
    lexec::sync_wait(scope.join());
}

TEST_CASE("spawn_future completes with the spawned sender's result") {
    auto pool = lexec::static_thread_pool{2};
    auto scope = lexec::counting_scope{};
    auto future = lexec::spawn_future(lexec::schedule(pool.get_scheduler()) | lexec::then([]() noexcept { return 42; }),
                                      scope.get_token());
    CHECK(std::get<0>(*lexec::sync_wait(std::move(future))) == 42);
    auto ready = lexec::spawn_future(lexec::just(std::make_unique<int>(3)), scope.get_token());
    CHECK(*std::get<0>(*lexec::sync_wait(std::move(ready))) == 3);
    auto failed = lexec::spawn_future(lexec::just_error(9) | lexec::let_error([](int e) { return lexec::just(e); }),
                                      scope.get_token());
    CHECK(std::get<0>(*lexec::sync_wait(std::move(failed))) == 9);
    lexec::sync_wait(scope.join());
}

TEST_CASE("spawn_future into a closed scope completes with stopped") {
    auto scope = lexec::simple_counting_scope{};
    scope.close();
    auto future = lexec::spawn_future(lexec::just(1), scope.get_token());
    CHECK_FALSE(lexec::sync_wait(std::move(future)).has_value());
    lexec::sync_wait(scope.join());
}

TEST_CASE("dropping a future asks its operation to stop") {
    auto scope = lexec::counting_scope{};
    {
        auto future = lexec::spawn_future(lexec_test::until_stopped_sender{}, scope.get_token());
    }
    CHECK(lexec::sync_wait(scope.join()).has_value());
}

// The consumer's stop request also asks the spawned work to stop; work that ignores it
// runs on after the consumer has completed.
TEST_CASE("a consumer's stop request completes the future with stopped while the work goes on") {
    auto scope = lexec::counting_scope{};
    auto loop = lexec::run_loop{};
    auto ran = false;
    auto future = lexec::spawn_future(
        lexec::unstoppable(lexec::schedule(loop.get_scheduler()) | lexec::then([&ran]() noexcept { ran = true; })),
        scope.get_token());
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto const stopped =
        lexec::sync_wait(lexec::write_env(std::move(future), lexec::prop{lexec::get_stop_token, source.get_token()}));
    CHECK_FALSE(stopped.has_value());
    CHECK_FALSE(ran);
    loop.finish();
    loop.run();
    CHECK(ran);
    CHECK(lexec::sync_wait(scope.join()).has_value());
}

// Many threads spawn and associate at once; TSan checks the scope's state machine.
TEST_CASE("concurrent spawns, associations, and futures all end before join completes") {
    auto pool = lexec::static_thread_pool{4};
    auto scope = lexec::counting_scope{};
    auto done = std::atomic<int>{0};
    auto futures_sum = std::atomic<int>{0};
    auto threads = std::vector<std::thread>{};
    for (auto t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (auto i = 0; i < 200; ++i) {
                lexec::spawn(lexec::schedule(pool.get_scheduler()) | lexec::then([&done]() noexcept { done.fetch_add(1); }),
                             scope.get_token());
                auto assoc = scope.get_token().try_associate();
                CHECK(static_cast<bool>(assoc));
                auto future = lexec::spawn_future(lexec::schedule(pool.get_scheduler()) | lexec::then([]() noexcept { return 1; }),
                                                  scope.get_token());
                futures_sum.fetch_add(std::get<0>(*lexec::sync_wait(std::move(future))));
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    lexec::sync_wait(scope.join());
    CHECK(done == 800);
    CHECK(futures_sum == 800);
}
