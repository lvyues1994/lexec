#include "../support/test_receivers.hpp"
#include "../support/test_senders.hpp"

#include <lexec/any_sender_of.hpp>
#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <array>
#include <exception>
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

using int_sender = lexec::any_sender_of<set_value_t(int), set_error_t(std::exception_ptr), set_stopped_t()>;
using void_sender = lexec::any_sender_of<set_value_t(), set_stopped_t()>;

struct answer_t {
    template <class Env>
    auto operator()(Env const &env) const noexcept -> decltype(env.query(*this)) {
        return env.query(*this);
    }
};

inline constexpr answer_t answer{};

using answering_receiver = lexec::any_receiver<completion_signatures<set_value_t(int)>, lexec::queries<int(answer_t) noexcept>>;

struct add_one {
    int operator()(int const v) const noexcept { return v + 1; }
};

static_assert(std::is_same_v<lexec::completion_signatures_of_t<int_sender>,
                             completion_signatures<set_value_t(int), set_error_t(std::exception_ptr), set_stopped_t()>>);
static_assert(std::is_constructible_v<int_sender, decltype(lexec::just(1))>);
// A sender whose completions the erased sender cannot deliver cannot be erased.
static_assert(not std::is_constructible_v<void_sender, decltype(lexec::just(1))>);
static_assert(not std::is_copy_constructible_v<int_sender>);
static_assert(lexec::is_scheduler_v<lexec::any_scheduler<void_sender>>);

} // namespace

namespace any_sender_test {

// A stoppable token of its own type, so that the erased operation has to forward stop
// requests to an inplace_stop_token.
struct wrapped_token;

template <class Fn>
struct wrapped_callback {
    template <class Init>
    explicit wrapped_callback(wrapped_token const &token, Init &&init) noexcept;

    lexec::inplace_stop_callback<Fn> callback;
};

struct wrapped_token {
    template <class Fn>
    using callback_type = wrapped_callback<Fn>;

    bool stop_requested() const noexcept { return inner.stop_requested(); }
    bool stop_possible() const noexcept { return inner.stop_possible(); }
    friend bool operator==(wrapped_token const &a, wrapped_token const &b) noexcept { return a.inner == b.inner; }
    friend bool operator!=(wrapped_token const &a, wrapped_token const &b) noexcept { return a.inner != b.inner; }

    lexec::inplace_stop_token inner;
};

template <class Fn>
template <class Init>
wrapped_callback<Fn>::wrapped_callback(wrapped_token const &token, Init &&init) noexcept
    : callback(token.inner, static_cast<Init &&>(init)) {}

static_assert(lexec::is_stoppable_token_v<wrapped_token>);

} // namespace any_sender_test

using any_sender_test::wrapped_token;

TEST_CASE("an any_sender holds and runs any sender with the declared completions") {
    int_sender value = lexec::just(41) | lexec::then(add_one{});
    CHECK(std::get<0>(*lexec::sync_wait(std::move(value))) == 42);
    lexec::any_sender_of<set_value_t(std::unique_ptr<int>)> owned = lexec::just(std::make_unique<int>(3));
    CHECK(*std::get<0>(*lexec::sync_wait(std::move(owned))) == 3);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("an any_sender passes its sender's errors on") {
    int_sender failure = lexec::just() | lexec::then([]() -> int { throw std::runtime_error{"erased"}; });
    CHECK_THROWS_AS(lexec::sync_wait(std::move(failure)), std::runtime_error);
}
#endif

TEST_CASE("senders of different types share one any_sender type") {
    auto pool = lexec::static_thread_pool{2};
    auto senders = std::vector<int_sender>{};
    senders.emplace_back(lexec::just(1));
    senders.emplace_back(lexec::just(1) | lexec::then(add_one{}));
    senders.emplace_back(lexec::schedule(pool.get_scheduler()) | lexec::then([] { return 3; }));
    senders.emplace_back(lexec::just(std::array<int, 16>{}) | lexec::then([](std::array<int, 16> a) { return static_cast<int>(a.size()); }));
    auto total = 0;
    for (auto &sender : senders) {
        total += std::get<0>(*lexec::sync_wait(std::move(sender)));
    }
    CHECK(total == 1 + 2 + 3 + 16);
}

TEST_CASE("an erased receiver's queries reach the erased sender") {
    lexec::any_sender<answering_receiver> reader = lexec::read_env(answer);
    auto const result = lexec::sync_wait(lexec::write_env(std::move(reader), lexec::prop{answer, 42}));
    CHECK(std::get<0>(*result) == 42);
}

TEST_CASE("the erased sender sees the receiver's stop requests") {
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    lexec::any_sender_of<set_value_t(), set_stopped_t()> direct = lexec_test::until_stopped_sender{};
    CHECK_FALSE(lexec::sync_wait(lexec::write_env(std::move(direct), lexec::prop{lexec::get_stop_token, source.get_token()}))
                    .has_value());

    auto other = lexec::inplace_stop_source{};
    auto log = lexec_test::completion_log{};
    lexec::any_sender_of<set_value_t(), set_stopped_t()> forwarded = lexec_test::until_stopped_sender{};
    auto op = lexec::connect(lexec::write_env(std::move(forwarded), lexec::prop{lexec::get_stop_token, wrapped_token{other.get_token()}}),
                             lexec_test::checked_receiver<set_value_t(), set_stopped_t()>{&log});
    lexec::start(op);
    CHECK(log.stopped_count == 0);
    other.request_stop();
    CHECK(log.stopped_count == 1);
}

// The forwarded request completes the erased sender inside the erased operation's own
// stop source, and the receiver destroys the operation at once.
TEST_CASE("an erased operation may go as soon as a forwarded stop request completes it") {
    auto source = lexec::inplace_stop_source{};
    auto log = lexec_test::completion_log{};
    lexec::any_sender_of<set_value_t(), set_stopped_t()> erased = lexec_test::until_stopped_sender{};
    lexec_test::start_destroyed_on_completion(
        lexec::write_env(std::move(erased), lexec::prop{lexec::get_stop_token, wrapped_token{source.get_token()}}), log);
    source.request_stop();
    CHECK(log.stopped_count == 1);
}

TEST_CASE("an any_sender can be moved before it is connected") {
    int_sender first = lexec::just(5);
    int_sender second = std::move(first);
    int_sender third = lexec::just(0);
    third = std::move(second);
    CHECK(std::get<0>(*lexec::sync_wait(std::move(third))) == 5);
}

TEST_CASE("an any_scheduler schedules on the scheduler it holds") {
    using scheduler = lexec::any_scheduler<void_sender>;
    auto pool = lexec::static_thread_pool{2};
    auto const on_pool = scheduler{pool.get_scheduler()};
    auto const inline_one = scheduler{lexec::inline_scheduler{}};
    auto const where = lexec::sync_wait(lexec::schedule(on_pool) | lexec::then([] { return std::this_thread::get_id(); }));
    CHECK(std::get<0>(*where) != std::this_thread::get_id());
    CHECK(lexec::sync_wait(lexec::schedule(inline_one)).has_value());
    auto const copy = on_pool;
    CHECK(copy == on_pool);
    CHECK(on_pool != inline_one);
    auto other_pool = lexec::static_thread_pool{1};
    CHECK(on_pool != scheduler{other_pool.get_scheduler()});
    auto const completes_on = lexec::get_completion_scheduler<set_value_t>(lexec::get_env(lexec::schedule(on_pool)));
    CHECK(completes_on == on_pool);
}
