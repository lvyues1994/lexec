#include "../support/test_receivers.hpp"

#include <lexec/detail/tuple.hpp>

#include <doctest/doctest.h>

namespace {

struct immovable {
    explicit immovable(int value_) : value(value_) {}
    immovable(immovable &&) = delete;
    int value;
};

immovable make_immovable(int const value) { return immovable{value}; }

struct empty_a {};
struct empty_b {};

static_assert(sizeof(lexec::detail::tuple<empty_a, int>) == sizeof(int));
static_assert(sizeof(lexec::detail::tuple<empty_a, empty_b>) == 1);
static_assert(std::is_same_v<decltype(lexec::detail::get<0>(std::declval<lexec::detail::tuple<empty_a> &>())), empty_a &>);

} // namespace

TEST_CASE("tuple constructs immovable elements in place from prvalues") {
    auto const t = lexec::detail::tuple<immovable, int>{{make_immovable(1)}, {2}};
    CHECK(lexec::detail::get<0>(t).value == 1);
    CHECK(lexec::detail::get<1>(t) == 2);
}

TEST_CASE("apply on an rvalue tuple moves its elements") {
    using lexec_test::counted;
    auto t = lexec::detail::tuple<counted, empty_a>{{counted{5}}, {}};
    counted::reset();
    auto const value = static_cast<decltype(t) &&>(t).apply([](counted &&c, empty_a &&) {
        auto const moved = counted{static_cast<counted &&>(c)};
        return moved.value;
    });
    CHECK(value == 5);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 1);
}
