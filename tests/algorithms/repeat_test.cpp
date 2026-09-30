#include "../support/loop_thread.hpp"
#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using lexec::completion_signatures;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;
using lexec_test::same_signature_set_v;

using stoppable_env = lexec::prop<lexec::get_stop_token_t, lexec::inplace_stop_token>;

template <class Sndr, class Env = lexec::env<>>
using sigs_of = lexec::completion_signatures_of_t<Sndr, Env>;

// Completes with set_value() for its first `successes` starts, then with set_stopped()
// or set_error(7).
struct succeed_n_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<set_value_t(), set_error_t(int), set_stopped_t()>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;

        void start() & noexcept {
            if ((*starts)++ < successes) {
                lexec::set_value(std::move(rcvr));
            } else if (stop) {
                lexec::set_stopped(std::move(rcvr));
            } else {
                lexec::set_error(std::move(rcvr), 7);
            }
        }

        Rcvr rcvr;
        int *starts;
        int successes;
        bool stop;
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) const noexcept {
        return {std::move(rcvr), starts, successes, stop};
    }

    int *starts;
    int successes;
    bool stop;
};

using true_sender = decltype(lexec::just(true));

static_assert(lexec::is_dependent_sender_v<decltype(lexec::repeat_until(std::declval<true_sender>()))>);
static_assert(same_signature_set_v<completion_signatures<set_value_t()>,
                                   sigs_of<decltype(lexec::repeat_until(std::declval<true_sender>()))>>);
// A receiver that can ask to stop gets set_stopped from the check before each iteration.
static_assert(same_signature_set_v<completion_signatures<set_value_t(), set_stopped_t()>,
                                   sigs_of<decltype(lexec::repeat_until(std::declval<true_sender>())), stoppable_env>>);
// std::false_type never ends the loop.
static_assert(same_signature_set_v<completion_signatures<>, sigs_of<decltype(lexec::repeat_until(lexec::just(std::false_type{})))>>);
static_assert(same_signature_set_v<completion_signatures<set_error_t(int), set_stopped_t()>,
                                   sigs_of<decltype(lexec::repeat(succeed_n_sender{}))>>);
static_assert(same_signature_set_v<completion_signatures<set_value_t(), set_error_t(int), set_stopped_t()>,
                                   sigs_of<decltype(lexec::repeat_n(succeed_n_sender{}, 3))>>);
// Errors pass through unchanged, and copying them adds nothing.
static_assert(same_signature_set_v<completion_signatures<set_error_t(int)>, sigs_of<decltype(lexec::repeat_until(lexec::just_error(7)))>>);

#if LEXEC_HAS_EXCEPTIONS
struct throwing_bool {
    operator bool() const { throw std::runtime_error{"bool"}; }
};

// Completes with set_value(); connecting it throws once it has been connected `limit` times.
struct connect_limited_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<set_value_t()>;

    template <class Rcvr>
    struct operation {
        using operation_state_concept = lexec::operation_state_t;
        void start() & noexcept { lexec::set_value(std::move(rcvr)); }
        Rcvr rcvr;
    };

    template <class Rcvr>
    operation<Rcvr> connect(Rcvr rcvr) const {
        if ((*connects)++ == limit) {
            throw std::runtime_error{"connect"};
        }
        return {std::move(rcvr)};
    }

    int *connects;
    int limit;
};

using eptr_sig = set_error_t(std::exception_ptr);
static_assert(same_signature_set_v<completion_signatures<set_value_t(), eptr_sig>,
                                   sigs_of<decltype(lexec::repeat_until(lexec::just(throwing_bool{})))>>);
static_assert(same_signature_set_v<completion_signatures<set_value_t(), eptr_sig>,
                                   sigs_of<decltype(lexec::repeat_n(connect_limited_sender{}, 2))>>);
#endif

} // namespace

TEST_CASE("repeat_until runs the sender until it completes with true") {
    auto count = 0;
    auto const result =
        lexec::sync_wait(lexec::repeat_until(lexec::just() | lexec::then([&count]() noexcept { return ++count == 5; })));
    CHECK(result.has_value());
    CHECK(count == 5);
}

namespace {

struct countdown {
    operator bool() const noexcept { return --*left == 0; }
    int *left;
};

} // namespace

TEST_CASE("repeat_until takes values that convert to bool, and both pipe forms") {
    auto left = 3;
    lexec::sync_wait(lexec::just() | lexec::then([&left]() noexcept { return countdown{&left}; }) | lexec::repeat_until());
    CHECK(left == 0);
    CHECK(lexec::sync_wait(lexec::just(true) | lexec::repeat_until).has_value());
}

// Without the trampoline, each iteration would run inside the previous one's completion.
TEST_CASE("hundreds of thousands of synchronous iterations do not overflow the stack") {
    constexpr auto iterations = 300'000;
    auto count = 0;
    lexec::sync_wait(lexec::repeat_until(lexec::just() | lexec::then([&count]() noexcept { return ++count == iterations; })));
    CHECK(count == iterations);
    auto runs = 0;
    lexec::sync_wait(lexec::repeat_n(lexec::just() | lexec::then([&runs]() noexcept { ++runs; }), iterations));
    CHECK(runs == iterations);
}

TEST_CASE("an error or stopped from an iteration ends the loop") {
    auto log = lexec_test::completion_log{};
    auto error = lexec::connect(lexec::repeat_until(lexec::just_error(7)), lexec_test::checked_receiver<set_error_t(int)>{&log});
    lexec::start(error);
    CHECK(log.error_count == 1);
    auto stopped = lexec::connect(lexec::repeat_until(lexec::just_stopped()), lexec_test::checked_receiver<set_stopped_t()>{&log});
    lexec::start(stopped);
    CHECK(log.stopped_count == 1);
    CHECK(log.total() == 2);
}

TEST_CASE("repeat runs the sender until an error or stopped ends it") {
    auto starts = 0;
    auto log = lexec_test::completion_log{};
    auto failing = lexec::connect(lexec::repeat(succeed_n_sender{&starts, 10, false}),
                                  lexec_test::checked_receiver<set_error_t(int), set_stopped_t()>{&log});
    lexec::start(failing);
    CHECK(starts == 11);
    CHECK(log.error_count == 1);
    starts = 0;
    auto stopping = lexec::connect(succeed_n_sender{&starts, 4, true} | lexec::repeat(),
                                   lexec_test::checked_receiver<set_error_t(int), set_stopped_t()>{&log});
    lexec::start(stopping);
    CHECK(starts == 5);
    CHECK(log.stopped_count == 1);
}

TEST_CASE("repeat_n runs the sender count times, and a count of zero does not start it") {
    auto starts = 0;
    auto log = lexec_test::completion_log{};
    using receiver = lexec_test::checked_receiver<set_value_t(), set_error_t(int), set_stopped_t()>;
    auto five = lexec::connect(lexec::repeat_n(succeed_n_sender{&starts, 100, false}, 5), receiver{&log});
    lexec::start(five);
    CHECK(starts == 5);
    CHECK(log.value_count == 1);
    starts = 0;
    auto none = lexec::connect(succeed_n_sender{&starts, 100, false} | lexec::repeat_n(0), receiver{&log});
    lexec::start(none);
    CHECK(starts == 0);
    CHECK(log.value_count == 2);
    auto early = lexec::connect(lexec::repeat_n(succeed_n_sender{&starts, 2, false}, 5), receiver{&log});
    lexec::start(early);
    CHECK(starts == 3);
    CHECK(log.error_count == 1);
}

// The request comes during the third iteration; the check before the fourth ends the loop.
TEST_CASE("a stop request ends the loop with stopped before the next iteration") {
    auto source = lexec::inplace_stop_source{};
    auto count = 0;
    auto const loop = lexec::repeat_until(lexec::just() | lexec::then([&]() noexcept {
                                              if (++count == 3) {
                                                  source.request_stop();
                                              }
                                              return false;
                                          }));
    auto const env = lexec::prop{lexec::get_stop_token, source.get_token()};
    CHECK_FALSE(lexec::sync_wait(lexec::write_env(loop, env)).has_value());
    CHECK(count == 3);
    CHECK_FALSE(lexec::sync_wait(lexec::write_env(loop, env)).has_value());
    CHECK(count == 3);
}

TEST_CASE("a loop sender can be connected more than once") {
    auto runs = 0;
    auto const loop = lexec::repeat_n(lexec::just() | lexec::then([&runs]() noexcept { ++runs; }), 3);
    lexec::sync_wait(loop);
    lexec::sync_wait(loop);
    CHECK(runs == 6);
}

TEST_CASE("iterations may complete on other threads") {
    auto pool = lexec::static_thread_pool{2};
    auto count = 0;
    lexec::sync_wait(
        lexec::repeat_until(lexec::schedule(pool.get_scheduler()) | lexec::then([&count]() noexcept { return ++count == 200; })));
    CHECK(count == 200);
    auto loop = lexec_test::loop_thread{};
    auto runs = 0;
    lexec::sync_wait(lexec::repeat_n(lexec::schedule(loop.scheduler()) | lexec::then([&runs]() noexcept { ++runs; }), 200));
    CHECK(runs == 200);
}

// Each loop is destroyed by its receiver inside its completion: nothing may touch the
// loop's state after the completion, whichever iteration or check sends it.
TEST_CASE("the receiver may destroy a loop as soon as it completes") {
    auto log = lexec_test::completion_log{};
    auto count = 0;
    lexec_test::start_destroyed_on_completion(
        lexec::repeat_until(lexec::just() | lexec::then([&count]() noexcept { return ++count == 3; })), log);
    CHECK(log.value_count == 1);
    lexec_test::start_destroyed_on_completion(lexec::repeat(lexec::just_error(7)), log);
    CHECK(log.error_count == 1);
    auto starts = 0;
    lexec_test::start_destroyed_on_completion(lexec::repeat_n(succeed_n_sender{&starts, 1, true}, 5), log);
    CHECK(log.stopped_count == 1);
    auto source = lexec::inplace_stop_source{};
    auto stopping = 0;
    lexec_test::start_destroyed_on_completion(
        lexec::write_env(lexec::repeat(lexec::just() | lexec::then([&]() noexcept {
                                           if (++stopping == 2) {
                                               source.request_stop();
                                           }
                                       })),
                         lexec::prop{lexec::get_stop_token, source.get_token()}),
        log);
    CHECK(stopping == 2);
    CHECK(log.stopped_count == 2);
    CHECK(log.total() == 4);
}

TEST_CASE("a loop over pool work completes with values in the pool's domain") {
    auto pool = lexec::static_thread_pool{1};
    auto const work = lexec::schedule(pool.get_scheduler()) | lexec::then([]() noexcept { return true; });
    auto const env = lexec::env<>{};
    using work_domain = decltype(lexec::get_completion_domain<set_value_t>(lexec::get_env(work), env));
    using loop_domain = decltype(lexec::get_completion_domain<set_value_t>(lexec::get_env(lexec::repeat_until(work)), env));
    static_assert(std::is_same_v<loop_domain, work_domain>);
    static_assert(not std::is_same_v<loop_domain, lexec::default_domain>);
    // A count of zero completes where the loop starts, outside the pool's domain.
    using zero_domain = decltype(lexec::get_completion_domain<set_value_t>(
        lexec::get_env(lexec::repeat_n(lexec::schedule(pool.get_scheduler()), 1)), env));
    static_assert(not std::is_same_v<zero_domain, work_domain>);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("an exception converting a value to bool completes the loop with it") {
    CHECK_THROWS_AS(lexec::sync_wait(lexec::repeat_until(lexec::just(throwing_bool{}))), std::runtime_error);
}

// The first connect is the loop's own; the third happens inside the second iteration's
// completion, where the exception becomes the loop's set_error.
TEST_CASE("an exception connecting a later iteration completes the loop with it") {
    auto connects = 0;
    CHECK_THROWS_AS(lexec::sync_wait(lexec::repeat_n(connect_limited_sender{&connects, 2}, 5)), std::runtime_error);
    CHECK(connects == 3);
    connects = 0;
    CHECK_THROWS_AS(lexec::sync_wait(lexec::repeat_n(connect_limited_sender{&connects, 0}, 5)), std::runtime_error);
    CHECK(connects == 1);
}
#endif
