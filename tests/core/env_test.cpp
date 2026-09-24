#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <type_traits>

namespace {

struct custom_query_t {};                        // not a forwarding query
struct forwarded_query_t : lexec::forwarding_query_t {};

struct no_env {};

static_assert(std::is_same_v<lexec::env_of_t<no_env>, lexec::env<>>);
static_assert(lexec::is_forwarding_query_v<lexec::get_stop_token_t>);
static_assert(lexec::is_forwarding_query_v<lexec::get_scheduler_t>);
static_assert(lexec::is_forwarding_query_v<forwarded_query_t>);
static_assert(not lexec::is_forwarding_query_v<custom_query_t>);

static_assert(std::is_same_v<lexec::stop_token_of_t<lexec::env<>>, lexec::never_stop_token>);
static_assert(not std::is_invocable_v<lexec::get_scheduler_t, lexec::env<>>);

using forwarded_twice = lexec::detail::fwd_env_t<lexec::detail::fwd_env_t<lexec::env<>>>;
static_assert(std::is_same_v<forwarded_twice, lexec::detail::fwd_env<lexec::env<>>>);

} // namespace

TEST_CASE("env answers each query from the first environment that supports it") {
    auto const e = lexec::env{lexec::prop{custom_query_t{}, 1}, lexec::prop{custom_query_t{}, 2},
                              lexec::prop{forwarded_query_t{}, 3}};
    CHECK(e.query(custom_query_t{}) == 1);
    CHECK(e.query(forwarded_query_t{}) == 3);
}

TEST_CASE("fwd_env exposes only forwarding queries") {
    auto const e = lexec::env{lexec::prop{custom_query_t{}, 1}, lexec::prop{forwarded_query_t{}, 3}};
    auto const forwarded = lexec::detail::make_fwd_env(e);
    CHECK(forwarded.query(forwarded_query_t{}) == 3);
    CHECK_FALSE(lexec::detail::has_query_v<decltype(forwarded) const &, custom_query_t>);
}

TEST_CASE("get_stop_token reads the environment's token") {
    auto source = lexec::inplace_stop_source{};
    auto const e = lexec::prop{lexec::get_stop_token, source.get_token()};
    CHECK(lexec::get_stop_token(e) == source.get_token());
    source.request_stop();
    CHECK(lexec::get_stop_token(e).stop_requested());
}
