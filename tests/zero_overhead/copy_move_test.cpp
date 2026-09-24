#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

using lexec_test::counted;

TEST_CASE("values produced during execution flow through synchronous adaptors without copies or moves") {
    counted::reset();
    auto const result = lexec::sync_wait(lexec::just() | lexec::then([]() noexcept { return counted{7}; }) |
                                         lexec::then([](counted &&c) noexcept { return c.value; }));
    CHECK(std::get<0>(*result) == 7);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 0);
}

TEST_CASE("sync_wait stores the final value with a single move") {
    counted::reset();
    auto const result = lexec::sync_wait(lexec::just() | lexec::then([]() noexcept { return counted{7}; }));
    CHECK(std::get<0>(*result).value == 7);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 1);
}

// Construction moves a captured value once into just, once per enclosing adaptor, and
// once into the operation state at connect.
TEST_CASE("building and connecting an rvalue pipeline moves captured values and never copies them") {
    counted::reset();
    auto const result = lexec::sync_wait(lexec::just(counted{7}) |
                                         lexec::then([](counted &&c) noexcept { return c.value; }));
    CHECK(std::get<0>(*result) == 7);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 3);
}

TEST_CASE("connecting an lvalue sender copies its captured values exactly once") {
    auto const sender = lexec::just(counted{7});
    counted::reset();
    auto const result = lexec::sync_wait(sender);
    CHECK(std::get<0>(*result).value == 7);
    CHECK(counted::counts.copies == 1);
    CHECK(counted::counts.moves == 1);
}
