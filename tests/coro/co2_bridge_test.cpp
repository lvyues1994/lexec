#include "../support/test_receivers.hpp"

#include <lexec/coro/co2.hpp>
#include <lexec/execution.hpp>

#include <co2/co2.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <ostream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {

using lexec::completion_signatures;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

struct add_one {
    int operator()(int const v) const noexcept { return v + 1; }
};

struct current_thread {
    std::thread::id operator()() const noexcept { return std::this_thread::get_id(); }
};

using awaiter_for_just = lexec::coro::detail::sender_awaiter<decltype(lexec::just(1))>;
static_assert(std::is_same_v<awaiter_for_just::value_type, int>);
static_assert(std::is_same_v<lexec::coro::detail::sender_awaiter<decltype(lexec::just())>::value_type, void>);
static_assert(std::is_same_v<lexec::coro::detail::sender_awaiter<decltype(lexec::just(1, 2.5))>::value_type,
                             std::tuple<int, double>>);
static_assert(lexec::is_stoppable_token_v<lexec::coro::stop_token>);
static_assert(lexec::is_scheduler_v<lexec::coro::scheduler>);
static_assert(std::is_same_v<lexec::completion_signatures_of_t<lexec::coro::task_sender<int>>,
                             completion_signatures<set_value_t(int), set_error_t(std::exception_ptr), set_stopped_t()>>);

auto await_value() CO2_BEG(co2::Task<int>, (), int value{};) {
    CO2_AWAIT_SET(value, lexec::just(41) | lexec::then(add_one{}));
    CO2_RETURN(value);
}
CO2_END

auto await_nothing() CO2_BEG(co2::Task<int>, ()) {
    CO2_AWAIT(lexec::just());
    CO2_RETURN(7);
}
CO2_END

auto await_values() CO2_BEG((co2::Task<std::tuple<int, double>>), (), std::tuple<int, double> values;) {
    CO2_AWAIT_SET(values, lexec::just(1, 2.5));
    CO2_RETURN(values);
}
CO2_END

auto await_exception() CO2_BEG(co2::Task<int>, ()) {
    CO2_AWAIT(lexec::just_error(std::make_exception_ptr(std::runtime_error{"sender"})));
    CO2_RETURN(0);
}
CO2_END

auto await_error_code() CO2_BEG(co2::Task<int>, ()) {
    CO2_AWAIT(lexec::just_error(std::make_error_code(std::errc::invalid_argument)));
    CO2_RETURN(0);
}
CO2_END

auto await_int_error() CO2_BEG(co2::Task<int>, ()) {
    CO2_AWAIT(lexec::just_error(5));
    CO2_RETURN(0);
}
CO2_END

auto await_stopped() CO2_BEG(co2::Task<>, ()) {
    CO2_AWAIT(lexec::just_stopped());
}
CO2_END

auto await_many(int count) CO2_BEG(co2::Task<int>, (count), int i{}; int sum{}; int one{};) {
    for (i = 0; i < count; ++i) {
        CO2_AWAIT_SET(one, lexec::just(1));
        sum += one;
    }
    CO2_RETURN(sum);
}
CO2_END

template <class Sch>
auto hop_to(Sch sch) CO2_BEG(co2::Task<std::thread::id>, (sch), std::thread::id where{};) {
    CO2_AWAIT(lexec::schedule(sch));
    CO2_AWAIT_SET(where, lexec::just() | lexec::then(current_thread{}));
    CO2_RETURN(where);
}
CO2_END

auto read_stop_token() CO2_BEG(co2::Task<bool>, (), lexec::coro::stop_token token;) {
    CO2_AWAIT_SET(token, lexec::read_env(lexec::get_stop_token));
    CO2_RETURN(token.stop_requested());
}
CO2_END

auto spin_until_stopped(co2::Scheduler &sch) CO2_BEG(co2::Task<int>, (sch), co2::stop_token token; int rounds{};) {
    CO2_AWAIT_SET(token, co2::getStopToken());
    while (not token.stop_requested()) {
        ++rounds;
        CO2_AWAIT(co2::scheduleOn(sch));
    }
    CO2_RETURN(rounds);
}
CO2_END

template <class Sch>
auto hop_repeatedly(Sch sch, int count) CO2_BEG(co2::Task<int>, (sch, count), int i{};) {
    for (i = 0; i < count; ++i) {
        CO2_AWAIT(lexec::schedule(sch));
    }
    CO2_RETURN(count);
}
CO2_END

auto void_task() CO2_BEG(co2::Task<>, ()) {
    CO2_AWAIT(co2::suspend_never{});
}
CO2_END

auto throwing_task() CO2_BEG(co2::Task<int>, ()) {
    throw std::logic_error{"task"};
    CO2_RETURN(0);
}
CO2_END

} // namespace

TEST_CASE("a co2 coroutine awaits a lexec sender's value") {
    CHECK(co2::syncWait(await_value()) == 42);
    CHECK(co2::syncWait(await_nothing()) == 7);
    CHECK(co2::syncWait(await_values()) == std::tuple<int, double>{1, 2.5});
}

TEST_CASE("an awaited sender's error is thrown into the coroutine") {
    CHECK_THROWS_AS(co2::syncWait(await_exception()), std::runtime_error);
    CHECK_THROWS_AS(co2::syncWait(await_error_code()), std::system_error);
    CHECK_THROWS_AS(co2::syncWait(await_int_error()), int);
}

TEST_CASE("an awaited sender's stopped completion is thrown as stopped_error") {
    CHECK_THROWS_AS(co2::syncWait(await_stopped()), lexec::coro::stopped_error);
}

TEST_CASE("awaiting senders that complete synchronously does not grow the stack") {
    CHECK(co2::syncWait(await_many(200'000)) == 200'000);
}

// The pool may run the operation before await_suspend returns; the coroutine must resume
// on the pool all the same, not continue on the thread that awaited.
TEST_CASE("awaiting schedule resumes the coroutine on the scheduler") {
    auto pool = lexec::static_thread_pool{2};
    auto on_caller = 0;
    for (auto i = 0; i < 200; ++i) {
        if (co2::syncWait(hop_to(pool.get_scheduler())) == std::this_thread::get_id()) {
            ++on_caller;
        }
    }
    CHECK(on_caller == 0);
}

TEST_CASE("an awaited sender sees the coroutine's co2 stop token") {
    auto source = co2::stop_source{};
    CHECK_FALSE(co2::syncWait(read_stop_token(), source.get_token()));
    source.request_stop();
    CHECK(co2::syncWait(read_stop_token(), source.get_token()));
    auto pool = lexec::static_thread_pool{1};
    CHECK_THROWS_AS(co2::syncWait(hop_to(pool.get_scheduler()), source.get_token()), lexec::coro::stopped_error);
}

TEST_CASE("a co2 Task runs as a sender") {
    CHECK(std::get<0>(*lexec::sync_wait(lexec::coro::as_sender(await_value()))) == 42);
    CHECK(lexec::sync_wait(lexec::coro::as_sender(void_task())).has_value());
    CHECK_THROWS_AS(lexec::sync_wait(lexec::coro::as_sender(throwing_task())), std::logic_error);
    CHECK_FALSE(lexec::sync_wait(lexec::coro::as_sender(await_stopped())).has_value());
    auto const both = lexec::sync_wait(
        lexec::when_all(lexec::coro::as_sender(await_value()), lexec::coro::as_sender(await_nothing())));
    CHECK(std::get<0>(*both) == 42);
    CHECK(std::get<1>(*both) == 7);
}

TEST_CASE("a lexec stop request reaches a co2 Task run as a sender") {
    auto pool = co2::ThreadPool{2};
    // A Task runs once, so its sender is connected as an rvalue.
    auto failing = lexec::when_all(lexec::coro::as_sender(spin_until_stopped(pool)),
                                   lexec::just() | lexec::then([]() -> int { throw std::runtime_error{"sibling"}; }));
    CHECK_THROWS_AS(lexec::sync_wait(std::move(failing)), std::runtime_error);
}

TEST_CASE("a stop requested before a co2 Task starts as a sender reaches it at once") {
    auto pool = co2::ThreadPool{1};
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto const rounds = lexec::sync_wait(lexec::write_env(lexec::coro::as_sender(spin_until_stopped(pool)),
                                                          lexec::prop{lexec::get_stop_token, source.get_token()}));
    CHECK(std::get<0>(*rounds) == 0);
}

// Completions race with await_suspend on other threads; TSan checks the handshake.
TEST_CASE("co2 coroutines awaiting a pool concurrently all complete") {
    auto pool = lexec::static_thread_pool{4};
    auto const sch = pool.get_scheduler();
    auto const hops = lexec::sync_wait(lexec::when_all(
        lexec::coro::as_sender(hop_repeatedly(sch, 2000)), lexec::coro::as_sender(hop_repeatedly(sch, 2000)),
        lexec::coro::as_sender(hop_repeatedly(sch, 2000)), lexec::coro::as_sender(hop_repeatedly(sch, 2000))));
    CHECK(std::get<0>(*hops) + std::get<1>(*hops) + std::get<2>(*hops) + std::get<3>(*hops) == 8000);
}

TEST_CASE("a co2 scheduler is a lexec scheduler") {
    auto pool = co2::ThreadPool{2};
    auto const sch = lexec::coro::scheduler{pool};
    CHECK(sch == lexec::coro::scheduler{pool});
    auto const on_pool = lexec::sync_wait(lexec::schedule(sch) | lexec::then([&pool] { return pool.isWorkerThread(); }));
    CHECK(std::get<0>(*on_pool));
    auto const started = lexec::sync_wait(lexec::starts_on(sch, lexec::just(3) | lexec::then(add_one{})));
    CHECK(std::get<0>(*started) == 4);

    auto executor = co2::ManualExecutor{};
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::schedule(lexec::coro::scheduler{executor}),
                             lexec_test::checked_receiver<set_value_t(), set_stopped_t()>{&log});
    lexec::start(op);
    CHECK(log.value_count == 0);
    CHECK(executor.runOne());
    CHECK(log.value_count == 1);
}
