#include "../support/loop_thread.hpp"
#include "../support/test_receivers.hpp"
#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;
using lexec_test::counted;

using stop_token_env = lexec::prop<lexec::get_stop_token_t, lexec::inplace_stop_token>;

static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::when_all(lexec::just(1), lexec::just(2.0)))>,
                             completion_signatures<set_value_t(int, double)>>);
static_assert(std::is_same_v<
              completion_signatures_of_t<decltype(lexec::when_all(lexec::just(1), lexec::just_error(std::string{})))>,
              completion_signatures<set_error_t(std::string)>>);
static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(int, int), set_stopped_t()>,
              completion_signatures_of_t<decltype(lexec::when_all(lexec_test::value_or_stopped_sender{}, lexec::just(1)))>>);

// A receiver that can request stop may stop the when_all before it starts.
static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(int), set_stopped_t()>,
              completion_signatures_of_t<decltype(lexec::when_all(lexec::just(1))), stop_token_env>>);

static_assert(std::is_same_v<
              completion_signatures_of_t<decltype(lexec::when_all_with_variant(lexec_test::int_or_string_sender{}, lexec::just(1))),
                                         lexec::env<>>,
              completion_signatures<set_value_t(std::variant<std::tuple<int>, std::tuple<std::string>>,
                                                std::variant<std::tuple<int>>)>>);

struct throws_on_move {
    throws_on_move() = default;
    throws_on_move(throws_on_move &&) {
#if LEXEC_HAS_EXCEPTIONS
        throw 1;
#endif
    }
};

struct make_throws_on_move {
    throws_on_move operator()() const noexcept { return {}; }
};

struct int_error_receiver {
    using receiver_concept = lexec::receiver_t;

    void set_error(int const e) && noexcept { *error = e; }
    void set_stopped() && noexcept { *stopped = true; }

    int *error;
    bool *stopped;
};

} // namespace

TEST_CASE("when_all sends the values of every sender, in order") {
    auto const result = lexec::sync_wait(lexec::when_all(lexec::just(1), lexec::just(std::string{"a"}), lexec::just()));
    CHECK(std::get<0>(*result) == 1);
    CHECK(std::get<1>(*result) == "a");
}

TEST_CASE("when_all completes after senders on different threads complete") {
    auto first = lexec_test::loop_thread{};
    auto second = lexec_test::loop_thread{};
    auto const result = lexec::sync_wait(
        lexec::when_all(lexec::schedule(first.scheduler()) | lexec::then([]() noexcept { return 1; }),
                        lexec::schedule(second.scheduler()) | lexec::then([]() noexcept { return 2; })));
    CHECK(std::get<0>(*result) == 1);
    CHECK(std::get<1>(*result) == 2);
}

TEST_CASE("the first error completes when_all and stops the other senders") {
    auto error = 0;
    auto stopped = false;
    auto op = lexec::connect(lexec::when_all(lexec_test::until_stopped_sender{}, lexec::just_error(42)),
                             int_error_receiver{&error, &stopped});
    lexec::start(op);
    CHECK(error == 42);
    CHECK_FALSE(stopped);
}

TEST_CASE("a stopped sender completes when_all with stopped and stops the others") {
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::when_all(lexec_test::until_stopped_sender{}, lexec::just_stopped()),
                             lexec_test::checked_receiver<set_stopped_t()>{&log});
    lexec::start(op);
    CHECK(log.stopped_count == 1);
    CHECK(log.total() == 1);
}

TEST_CASE("a stop requested by the receiver stops every sender") {
    auto source = lexec::inplace_stop_source{};
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::write_env(lexec::when_all(lexec_test::until_stopped_sender{},
                                                              lexec_test::until_stopped_sender{}),
                                              lexec::prop{lexec::get_stop_token, source.get_token()}),
                             lexec_test::checked_receiver<set_stopped_t()>{&log});
    lexec::start(op);
    CHECK(log.total() == 0);
    source.request_stop();
    CHECK(log.stopped_count == 1);
    CHECK(log.total() == 1);
}

TEST_CASE("a stop requested before start completes with stopped without starting the senders") {
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto started = 0;
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(
        lexec::write_env(lexec::when_all(lexec::just() | lexec::then([&started]() noexcept { ++started; })),
                         lexec::prop{lexec::get_stop_token, source.get_token()}),
        lexec_test::checked_receiver<set_value_t(), set_stopped_t()>{&log});
    lexec::start(op);
    CHECK(started == 0);
    CHECK(log.stopped_count == 1);
}

TEST_CASE("when_all moves each value once into its operation") {
    counted::reset();
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::when_all(lexec::just() | lexec::then([]() noexcept { return counted{7}; })),
                             lexec_test::checked_receiver<set_value_t(counted)>{&log});
    lexec::start(op);
    CHECK(log.value_count == 1);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 1);
}

TEST_CASE("when_all carries move-only values") {
    auto const result = lexec::sync_wait(lexec::when_all(lexec::just(std::make_unique<int>(1)), lexec::just(2)));
    CHECK(*std::get<0>(*result) == 1);
    CHECK(std::get<1>(*result) == 2);
}

TEST_CASE("when_all_with_variant sends a variant for each sender") {
    auto const result = lexec::sync_wait(lexec::when_all_with_variant(lexec_test::int_or_string_sender{true}, lexec::just(1)));
    auto const &[first, second] = *result;
    REQUIRE(first.index() == 1);
    CHECK(std::get<0>(std::get<1>(first)) == "text");
    CHECK(std::get<0>(std::get<0>(second)) == 1);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("an exception storing a value completes when_all with set_error") {
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::when_all(lexec::just() | lexec::then(make_throws_on_move{})),
                             lexec_test::checked_receiver<set_value_t(throws_on_move), set_error_t(std::exception_ptr)>{&log});
    lexec::start(op);
    CHECK(log.error_count == 1);
    CHECK(log.total() == 1);
}
#endif
