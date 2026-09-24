#include <lexec/detail/config.hpp>

#include <doctest/doctest.h>

namespace {

struct empty_tag {};

struct holder {
    LEXEC_NO_UNIQUE_ADDRESS empty_tag tag;
    int value;
};

static_assert(sizeof(holder) == sizeof(int), "LEXEC_NO_UNIQUE_ADDRESS must let empty members occupy no storage");

} // namespace

TEST_CASE("exception detection matches the build mode") {
    CHECK(LEXEC_HAS_EXCEPTIONS == LEXEC_TEST_EXCEPTIONS_ENABLED);
}
