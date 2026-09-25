#include "../support/test_receivers.hpp"
#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

struct square_into {
    void operator()(int const i, std::vector<int> &out) const noexcept { out[static_cast<std::size_t>(i)] = i * i; }
};

struct fill_chunk {
    void operator()(int const begin, int const end, std::vector<int> &out) const noexcept {
        for (auto i = begin; i != end; ++i) {
            out[static_cast<std::size_t>(i)] = i + 1;
        }
    }
};

struct may_throw {
    void operator()(int, int &) const {}
};

struct ignore_index {
    template <class... Vs>
    void operator()(int, Vs &...) const noexcept {}
};

using vector_sender = decltype(lexec::just(std::vector<int>{}));

static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::bulk(lexec::just(std::vector<int>{}),
                                                                             lexec::par, 4, square_into{})),
                                                        lexec::env<>>,
                             completion_signatures<set_value_t(std::vector<int>)>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(std::vector<int>{}) |
                                                                 lexec::bulk_chunked(lexec::par, 4, fill_chunk{})),
                                                        lexec::env<>>,
                             completion_signatures<set_value_t(std::vector<int>)>>);
// bulk reports its completions before it is lowered, too.
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(std::vector<int>{}) |
                                                                 lexec::bulk(lexec::seq, 4, square_into{}))>,
                             completion_signatures<set_value_t(std::vector<int>)>>);
#if LEXEC_HAS_EXCEPTIONS
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1) | lexec::bulk(lexec::par, 4, may_throw{})),
                                                        lexec::env<>>,
                             completion_signatures<set_value_t(int), set_error_t(std::exception_ptr)>>);
#else
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1) | lexec::bulk(lexec::par, 4, may_throw{})),
                                                        lexec::env<>>,
                             completion_signatures<set_value_t(int)>>);
#endif
static_assert(lexec_test::same_signature_set_v<
              completion_signatures<set_value_t(int), set_stopped_t()>,
              completion_signatures_of_t<decltype(lexec_test::value_or_stopped_sender{} |
                                                  lexec::bulk_unchunked(lexec::par, 2, ignore_index{})),
                                         lexec::env<>>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just_error(3) |
                                                                 lexec::bulk(lexec::par, 2, ignore_index{})),
                                                        lexec::env<>>,
                             completion_signatures<set_error_t(int)>>);

// bulk-algo(sndr, policy, shape, f) requires an execution policy, an integral shape
// other than bool, and a copyable function.
static_assert(lexec::detail::is_callable_v<lexec::bulk_t const &, vector_sender, lexec::parallel_policy const &, int,
                                           square_into>);
static_assert(not lexec::detail::is_callable_v<lexec::bulk_t const &, vector_sender, int, int, square_into>);
static_assert(not lexec::detail::is_callable_v<lexec::bulk_t const &, vector_sender, lexec::parallel_policy, double,
                                               square_into>);
static_assert(not lexec::detail::is_callable_v<lexec::bulk_t const &, vector_sender, lexec::parallel_policy, bool,
                                               square_into>);
static_assert(lexec::is_execution_policy_v<lexec::sequenced_policy> and lexec::is_execution_policy_v<lexec::unsequenced_policy>);
static_assert(not lexec::is_execution_policy_v<int>);

// Lowered, bulk is a bulk_chunked sender.
static_assert(std::is_same_v<lexec::tag_of_t<decltype(lexec::transform_sender(
                                 lexec::just(1) | lexec::bulk(lexec::par, 2, ignore_index{}), lexec::env<>{}))>,
                             lexec::bulk_chunked_t>);

// Customizes bulk_chunked by running it as two halves through bulk_unchunked, which it
// leaves alone, so the transformation reaches a fixed point.
template <class Fn, class Shape>
struct two_halves {
    template <class... Vs>
    void operator()(Shape const half, Vs &...vs) noexcept {
        ++*calls;
        auto const middle = static_cast<Shape>(shape / 2);
        if (half == 0) {
            fn(Shape{0}, middle, vs...);
        } else {
            fn(middle, shape, vs...);
        }
    }

    Fn fn;
    Shape shape;
    int *calls;
};

inline int halves_calls = 0;

struct halving_domain {
    template <class Sndr, class Env,
              std::enable_if_t<std::is_same_v<lexec::tag_of_t<Sndr>, lexec::bulk_chunked_t>, int> = 0>
    auto transform_sender(set_value_t, Sndr &&sndr, Env const &) const {
        return static_cast<Sndr &&>(sndr).apply([](lexec::bulk_chunked_t, auto &&data, auto &&child) {
            using data_type = lexec::detail::remove_cvref_t<decltype(data)>;
            using halves = two_halves<typename data_type::fn_type, typename data_type::shape_type>;
            return lexec::bulk_unchunked(static_cast<decltype(child) &&>(child), lexec::par, 2,
                                         halves{static_cast<decltype(data) &&>(data).fn, data.shape, &halves_calls});
        });
    }
};

auto const halving = lexec::prop{lexec::get_domain, halving_domain{}};

} // namespace

TEST_CASE("bulk invokes the function for every index with lvalues of the values it then sends") {
    auto const result = lexec::sync_wait(lexec::just(std::vector<int>(6)) | lexec::bulk(lexec::par, 6, square_into{}));
    CHECK(std::get<0>(*result) == std::vector<int>{0, 1, 4, 9, 16, 25});
}

TEST_CASE("bulk_chunked by default calls the function once with the whole range") {
    auto chunks = std::vector<std::pair<int, int>>{};
    auto const result = lexec::sync_wait(
        lexec::just(std::vector<int>(5)) | lexec::bulk_chunked(lexec::par, 5, [&chunks](int b, int e, std::vector<int> &v) {
            chunks.emplace_back(b, e);
            fill_chunk{}(b, e, v);
        }));
    CHECK(chunks == std::vector<std::pair<int, int>>{{0, 5}});
    CHECK(std::get<0>(*result) == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("bulk_unchunked calls the function once per index") {
    auto indices = std::vector<long>{};
    lexec::sync_wait(lexec::just() | lexec::bulk_unchunked(lexec::seq, 4L, [&indices](long i) { indices.push_back(i); }));
    CHECK(indices == std::vector<long>{0, 1, 2, 3});
}

TEST_CASE("an empty or negative shape invokes nothing and still sends the values") {
    auto calls = 0;
    auto const count = [&calls](auto &&...) noexcept { ++calls; };
    auto const empty = lexec::sync_wait(lexec::just(7) | lexec::bulk(lexec::par, 0, count));
    auto const negative = lexec::sync_wait(lexec::just(8) | lexec::bulk_chunked(lexec::par, -3, count));
    auto const unsigned_empty = lexec::sync_wait(lexec::just(9) | lexec::bulk_unchunked(lexec::par, 0u, count));
    CHECK(calls == 0);
    CHECK(std::get<0>(*empty) == 7);
    CHECK(std::get<0>(*negative) == 8);
    CHECK(std::get<0>(*unsigned_empty) == 9);
}

TEST_CASE("bulk accepts every standard execution policy and the direct call form") {
    auto sum = 0;
    auto const add = [&sum](int i) noexcept { sum += i; };
    lexec::sync_wait(lexec::bulk(lexec::just(), lexec::seq, 3, add));
    lexec::sync_wait(lexec::bulk(lexec::just(), lexec::par, 3, add));
    lexec::sync_wait(lexec::bulk(lexec::just(), lexec::par_unseq, 3, add));
    lexec::sync_wait(lexec::bulk(lexec::just(), lexec::unseq, 3, add));
    CHECK(sum == 12);
}

TEST_CASE("bulk passes move-only values through") {
    auto const result = lexec::sync_wait(lexec::just(std::make_unique<int>(2)) |
                                         lexec::bulk(lexec::par, 3, [](int i, std::unique_ptr<int> &p) noexcept { *p += i; }));
    CHECK(*std::get<0>(*result) == 5);
}

TEST_CASE("errors and stopped pass through bulk without invoking the function") {
    auto calls = 0;
    auto log = lexec_test::completion_log{};
    auto error_op = lexec::connect(lexec::just_error(1) | lexec::bulk(lexec::par, 4, [&calls](int) noexcept { ++calls; }),
                                   lexec_test::checked_receiver<set_error_t(int)>{&log});
    lexec::start(error_op);
    auto stopped_op =
        lexec::connect(lexec::just_stopped() | lexec::bulk_chunked(lexec::par, 4, [&calls](int, int) noexcept { ++calls; }),
                       lexec_test::checked_receiver<set_stopped_t()>{&log});
    lexec::start(stopped_op);
    CHECK(log.error_count == 1);
    CHECK(log.stopped_count == 1);
    CHECK(calls == 0);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("an exception from the function becomes an error completion") {
    auto const failing = lexec::just() | lexec::bulk(lexec::par, 4, [](int i) {
                             if (i == 2) {
                                 throw std::runtime_error{"boom"};
                             }
                         });
    CHECK_THROWS_AS(lexec::sync_wait(failing), std::runtime_error);
}
#endif

TEST_CASE("a domain that customizes bulk_chunked also customizes bulk, which lowers to it") {
    halves_calls = 0;
    auto const chunked = lexec::sync_wait(
        lexec::write_env(lexec::just(std::vector<int>(6)) | lexec::bulk_chunked(lexec::par, 6, fill_chunk{}), halving));
    CHECK(halves_calls == 2);
    CHECK(std::get<0>(*chunked) == std::vector<int>{1, 2, 3, 4, 5, 6});
    auto const per_index = lexec::sync_wait(
        lexec::write_env(lexec::just(std::vector<int>(5)) | lexec::bulk(lexec::par, 5, square_into{}), halving));
    CHECK(halves_calls == 4);
    CHECK(std::get<0>(*per_index) == std::vector<int>{0, 1, 4, 9, 16});
}
