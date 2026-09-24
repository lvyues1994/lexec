#include "../support/test_receivers.hpp"
#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <type_traits>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

struct answer_t {
    template <class Env>
    constexpr auto operator()(Env const &env) const noexcept -> decltype(env.query(*this)) {
        return env.query(*this);
    }
};

inline constexpr answer_t answer{};

// The written environment answers what read_env asks, so no receiver environment is needed.
using answered = decltype(lexec::write_env(lexec::read_env(answer), lexec::prop{answer, 42}));
static_assert(lexec::is_sender_in_v<answered>);
static_assert(std::is_same_v<completion_signatures_of_t<answered>, completion_signatures<set_value_t(int const &)>>);

// The child's completions are the write_env's.
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::write_env(lexec_test::value_or_stopped_sender{},
                                                                                  lexec::prop{answer, 1}))>,
                             completion_signatures<set_value_t(int), set_stopped_t()>>);

} // namespace

TEST_CASE("write_env passes every channel of its child through") {
    auto log = lexec_test::completion_log{};
    auto error_op = lexec::connect(lexec::write_env(lexec::just_error(1), lexec::prop{answer, 1}),
                                   lexec_test::checked_receiver<set_error_t(int)>{&log});
    lexec::start(error_op);
    auto stopped_op = lexec::connect(lexec::write_env(lexec_test::value_or_stopped_sender{true, 0}, lexec::prop{answer, 1}),
                                     lexec_test::checked_receiver<set_value_t(int), set_stopped_t()>{&log});
    lexec::start(stopped_op);
    CHECK(log.error_count == 1);
    CHECK(log.stopped_count == 1);
    CHECK(log.total() == 2);
}

TEST_CASE("write_env carries move-only values") {
    auto const result =
        lexec::sync_wait(lexec::write_env(lexec::just(std::make_unique<int>(3)), lexec::prop{answer, 1}));
    CHECK(*std::get<0>(*result) == 3);
}

TEST_CASE("write_env answers the child's queries") {
    CHECK(std::get<0>(*lexec::sync_wait(lexec::write_env(lexec::read_env(answer), lexec::prop{answer, 42}))) == 42);
    CHECK(std::get<0>(*lexec::sync_wait(lexec::read_env(answer) | lexec::write_env(lexec::prop{answer, 7}))) == 7);
}

TEST_CASE("the innermost written environment shadows the outer ones") {
    auto const sender =
        lexec::write_env(lexec::write_env(lexec::read_env(answer), lexec::prop{answer, 1}), lexec::prop{answer, 2});
    CHECK(std::get<0>(*lexec::sync_wait(sender)) == 1);
}

TEST_CASE("the receiver's environment stays visible behind the written one") {
    auto const result = lexec::sync_wait(lexec::write_env(lexec::read_env(lexec::get_scheduler), lexec::prop{answer, 1}));
    static_assert(
        std::is_same_v<std::remove_const_t<decltype(result)>, std::optional<std::tuple<lexec::detail::run_loop_scheduler>>>);
    CHECK(result.has_value());
}

TEST_CASE("write_env passes a stop token to its child and unstoppable hides it") {
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto const with_token = lexec::prop{lexec::get_stop_token, source.get_token()};

    auto const seen = lexec::sync_wait(lexec::write_env(lexec::read_env(lexec::get_stop_token), with_token));
    CHECK(std::get<0>(*seen).stop_requested());

    auto const hidden =
        lexec::sync_wait(lexec::write_env(lexec::read_env(lexec::get_stop_token) | lexec::unstoppable, with_token));
    static_assert(std::is_same_v<std::remove_const_t<decltype(hidden)>, std::optional<std::tuple<lexec::never_stop_token>>>);
    CHECK(hidden.has_value());
}
