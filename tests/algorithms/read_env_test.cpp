#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <stdexcept>
#include <type_traits>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_value_t;

struct answer_t {
    template <class Env>
    constexpr auto operator()(Env const &env) const noexcept -> decltype(env.query(*this)) {
        return env.query(*this);
    }
};

inline constexpr answer_t answer{};

struct throwing_query_t {
    template <class Env>
    int operator()(Env const &) const {
#if LEXEC_HAS_EXCEPTIONS
        throw std::runtime_error{"query failed"};
#else
        return 0;
#endif
    }
};

using answer_env = lexec::prop<answer_t, int>;

// Without an environment there is nothing to read.
static_assert(lexec::is_dependent_sender_v<decltype(lexec::read_env(answer))>);
static_assert(not lexec::is_sender_in_v<decltype(lexec::read_env(answer)), lexec::env<>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::read_env(answer)), answer_env>,
                             completion_signatures<set_value_t(int const &)>>);
#if LEXEC_HAS_EXCEPTIONS
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::read_env(throwing_query_t{})), lexec::env<>>,
                             completion_signatures<set_value_t(int), set_error_t(std::exception_ptr)>>);
#endif

} // namespace

TEST_CASE("read_env reads the scheduler that sync_wait provides") {
    auto const result = lexec::sync_wait(lexec::read_env(lexec::get_scheduler) | lexec::let_value([](auto &scheduler) {
                                             return lexec::schedule(scheduler) | lexec::then([]() noexcept { return 42; });
                                         }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == 42);
}

TEST_CASE("read_env of the stop token gives a never_stop_token when the receiver has none") {
    auto const result = lexec::sync_wait(lexec::read_env(lexec::get_stop_token));
    static_assert(std::is_same_v<std::remove_const_t<decltype(result)>, std::optional<std::tuple<lexec::never_stop_token>>>);
    CHECK(result.has_value());
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("an exception thrown by the query completes read_env with set_error") {
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::read_env(throwing_query_t{}),
                             lexec_test::checked_receiver<set_value_t(int), set_error_t(std::exception_ptr)>{&log});
    lexec::start(op);
    CHECK(log.error_count == 1);
    CHECK(log.total() == 1);
}
#endif
