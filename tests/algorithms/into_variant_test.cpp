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

struct throwing_copy {
    throwing_copy() = default;
    throwing_copy(throwing_copy const &) {}
};

struct return_shared_lvalue {
    throwing_copy const &operator()() const noexcept {
        static auto const shared = throwing_copy{};
        return shared;
    }
};

static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec::just(1, 2.0) | lexec::into_variant)>,
                             completion_signatures<set_value_t(std::variant<std::tuple<int, double>>)>>);
static_assert(
    std::is_same_v<completion_signatures_of_t<decltype(lexec_test::int_or_string_sender{} | lexec::into_variant)>,
                   completion_signatures<set_value_t(std::variant<std::tuple<int>, std::tuple<std::string>>)>>);

// Other channels pass through unchanged.
static_assert(std::is_same_v<completion_signatures_of_t<decltype(lexec_test::value_or_stopped_sender{} | lexec::into_variant)>,
                             completion_signatures<set_value_t(std::variant<std::tuple<int>>), set_stopped_t()>>);

// Copying an lvalue into the variant may throw.
using copied_lvalue = decltype(lexec::just() | lexec::then(return_shared_lvalue{}) | lexec::into_variant);
#if LEXEC_HAS_EXCEPTIONS
static_assert(std::is_same_v<completion_signatures_of_t<copied_lvalue>,
                             completion_signatures<set_value_t(std::variant<std::tuple<throwing_copy>>),
                                                   set_error_t(std::exception_ptr)>>);
#endif

} // namespace

TEST_CASE("into_variant sends the tuple of whichever value channel completed") {
    auto const text = lexec::sync_wait(lexec::into_variant(lexec_test::int_or_string_sender{true}));
    auto const number = lexec::sync_wait(lexec::into_variant(lexec_test::int_or_string_sender{false}));
    auto const &text_variant = std::get<0>(*text);
    auto const &number_variant = std::get<0>(*number);
    REQUIRE(text_variant.index() == 1);
    REQUIRE(number_variant.index() == 0);
    CHECK(std::get<0>(std::get<1>(text_variant)) == "text");
    CHECK(std::get<0>(std::get<0>(number_variant)) == 7);
}

TEST_CASE("into_variant passes errors and stopped through") {
    auto log = lexec_test::completion_log{};
    using variant = std::variant<std::tuple<int>>;
    auto error_op = lexec::connect(
        lexec::just_error(3) | lexec::into_variant,
        lexec_test::checked_receiver<set_value_t(lexec::detail::empty_variant), set_error_t(int)>{&log});
    lexec::start(error_op);
    auto stopped_op =
        lexec::connect(lexec_test::value_or_stopped_sender{true, 0} | lexec::into_variant,
                       lexec_test::checked_receiver<set_value_t(variant), set_stopped_t()>{&log});
    lexec::start(stopped_op);
    CHECK(log.error_count == 1);
    CHECK(log.stopped_count == 1);
    CHECK(log.total() == 2);
}

TEST_CASE("into_variant carries move-only values") {
    auto result = lexec::sync_wait(lexec::just(std::make_unique<int>(5)) | lexec::into_variant);
    auto &value = std::get<0>(std::get<0>(std::get<0>(*result)));
    CHECK(*value == 5);
}
