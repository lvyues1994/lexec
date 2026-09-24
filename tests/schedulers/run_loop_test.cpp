#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <thread>
#include <vector>

namespace {

struct append_receiver {
    using receiver_concept = lexec::receiver_t;

    void set_value() && noexcept { order->push_back(id); }
    void set_stopped() && noexcept {}
    void set_error(std::exception_ptr) && noexcept {}

    std::vector<int> *order;
    int id;
};

struct stop_env {
    lexec::inplace_stop_token query(lexec::get_stop_token_t) const noexcept { return token; }
    lexec::inplace_stop_token token;
};

struct stoppable_receiver {
    using receiver_concept = lexec::receiver_t;

    void set_value() && noexcept { ++log->value_count; }
    void set_stopped() && noexcept { ++log->stopped_count; }
    void set_error(std::exception_ptr) && noexcept { ++log->error_count; }
    stop_env get_env() const noexcept { return stop_env{token}; }

    lexec_test::completion_log *log;
    lexec::inplace_stop_token token;
};

} // namespace

TEST_CASE("run returns once finish has been called and the queue is empty") {
    auto loop = lexec::run_loop{};
    loop.finish();
    loop.run();
}

TEST_CASE("run_loop executes scheduled work in FIFO order") {
    auto loop = lexec::run_loop{};
    auto order = std::vector<int>{};
    auto const sch = loop.get_scheduler();
    auto op0 = lexec::connect(lexec::schedule(sch), append_receiver{&order, 0});
    auto op1 = lexec::connect(lexec::schedule(sch), append_receiver{&order, 1});
    auto op2 = lexec::connect(lexec::schedule(sch), append_receiver{&order, 2});
    lexec::start(op0);
    lexec::start(op1);
    lexec::start(op2);
    CHECK(order.empty());
    loop.finish();
    loop.run();
    CHECK(order == std::vector<int>{0, 1, 2});
}

TEST_CASE("run_loop completes with set_stopped when the receiver's token is stopped") {
    auto loop = lexec::run_loop{};
    auto source = lexec::inplace_stop_source{};
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::schedule(loop.get_scheduler()), stoppable_receiver{&log, source.get_token()});
    lexec::start(op);
    source.request_stop();
    loop.finish();
    loop.run();
    CHECK(log.stopped_count == 1);
    CHECK(log.total() == 1);
}

TEST_CASE("work scheduled onto a run_loop runs on the thread driving it") {
    auto loop = lexec::run_loop{};
    auto driver = std::thread{[&loop] { loop.run(); }};
    auto const driver_id = driver.get_id();
    auto const result = lexec::sync_wait(lexec::schedule(loop.get_scheduler()) |
                                         lexec::then([]() noexcept { return std::this_thread::get_id(); }));
    loop.finish();
    driver.join();
    REQUIRE(result.has_value());
    auto const ran_on_driver = std::get<0>(*result) == driver_id;
    CHECK(ran_on_driver);
}

TEST_CASE("the schedule sender reports its scheduler for value and stopped completions") {
    auto loop = lexec::run_loop{};
    auto const sch = loop.get_scheduler();
    auto const attrs = lexec::get_env(lexec::schedule(sch));
    CHECK(lexec::get_completion_scheduler<lexec::set_value_t>(attrs) == sch);
    CHECK(lexec::get_completion_scheduler<lexec::set_stopped_t>(attrs) == sch);
    loop.finish();
    loop.run();
}
