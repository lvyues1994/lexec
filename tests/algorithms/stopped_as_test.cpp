#include "../support/test_senders.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_value_t;

using optional_just = decltype(lexec::stopped_as_optional(lexec::just(1)));

// The algorithms are lowered by transform_sender, which needs the receiver's environment.
static_assert(lexec::is_dependent_sender_v<optional_just>);
static_assert(std::is_same_v<completion_signatures_of_t<optional_just, lexec::env<>>,
                             completion_signatures<set_value_t(std::optional<int>)>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec_test::value_or_stopped_sender{} |
                                                                 lexec::stopped_as_optional),
                                                        lexec::env<>>,
                             completion_signatures<set_value_t(std::optional<int>)>>);
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec_test::value_or_stopped_sender{} |
                                                                 lexec::stopped_as_error(42)),
                                                        lexec::env<>>,
                             completion_signatures<set_value_t(int), set_error_t(int)>>);
// Errors pass through stopped_as_error unchanged.
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just_error(1) | lexec::stopped_as_error(2)),
                                                        lexec::env<>>,
                             completion_signatures<set_error_t(int)>>);
static_assert(not std::is_same_v<lexec::detail::remove_cvref_t<decltype(lexec::transform_sender(
                                     lexec::stopped_as_optional(lexec::just(1)), lexec::env<>{}))>,
                                 optional_just>);

struct int_receiver {
    using receiver_concept = lexec::receiver_t;

    void set_value(int const v) && noexcept { *value = v; }
    void set_error(int const e) && noexcept { *error = e; }

    int *value;
    int *error;
};

} // namespace

TEST_CASE("stopped_as_optional sends an engaged optional for a value and an empty one for stopped") {
    auto const value = lexec::sync_wait(lexec_test::value_or_stopped_sender{false, 3} | lexec::stopped_as_optional);
    auto const stopped = lexec::sync_wait(lexec_test::value_or_stopped_sender{true, 0} | lexec::stopped_as_optional);
    REQUIRE(std::get<0>(*value).has_value());
    CHECK(*std::get<0>(*value) == 3);
    CHECK_FALSE(std::get<0>(*stopped).has_value());
}

TEST_CASE("stopped_as_optional carries move-only values") {
    auto const result = lexec::sync_wait(lexec::just(std::make_unique<int>(4)) | lexec::stopped_as_optional);
    CHECK(**std::get<0>(*result) == 4);
}

TEST_CASE("stopped_as_error turns stopped into the given error and passes values through") {
    auto value = 0;
    auto error = 0;
    auto stopped_op = lexec::connect(lexec_test::value_or_stopped_sender{true, 0} | lexec::stopped_as_error(42),
                                     int_receiver{&value, &error});
    lexec::start(stopped_op);
    CHECK(error == 42);
    auto value_op = lexec::connect(lexec_test::value_or_stopped_sender{false, 5} | lexec::stopped_as_error(42),
                                   int_receiver{&value, &error});
    lexec::start(value_op);
    CHECK(value == 5);
    auto error_op = lexec::connect(lexec::just_error(7) | lexec::stopped_as_error(42), int_receiver{&value, &error});
    lexec::start(error_op);
    CHECK(error == 7);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("stopped_as_optional passes errors through") {
    auto const failing =
        lexec::just(1) | lexec::then([](int) -> int { throw std::runtime_error{"boom"}; }) | lexec::stopped_as_optional;
    CHECK_THROWS_AS(lexec::sync_wait(failing), std::runtime_error);
}
#endif
