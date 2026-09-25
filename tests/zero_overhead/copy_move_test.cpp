#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <tuple>
#include <variant>

using lexec_test::counted;

TEST_CASE("values produced during execution flow through synchronous adaptors without copies or moves") {
    counted::reset();
    auto const result = lexec::sync_wait(lexec::just() | lexec::then([]() noexcept { return counted{7}; }) |
                                         lexec::then([](counted &&c) noexcept { return c.value; }));
    CHECK(std::get<0>(*result) == 7);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 0);
}

TEST_CASE("bulk hands values to the function and downstream by reference") {
    counted::reset();
    auto const result = lexec::sync_wait(lexec::just() | lexec::then([]() noexcept { return counted{7}; }) |
                                         lexec::bulk(lexec::par, 3, [](int, counted &c) noexcept { ++c.value; }) |
                                         lexec::then([](counted &&c) noexcept { return c.value; }));
    CHECK(std::get<0>(*result) == 10);
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

TEST_CASE("sync_wait_with_variant stores the final value with a single move") {
    counted::reset();
    auto const result =
        lexec::sync_wait_with_variant(lexec::just() | lexec::then([]() noexcept { return counted{7}; }));
    CHECK(std::get<0>(std::get<0>(*result)).value == 7);
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

TEST_CASE("let_value moves the predecessor's value once into its state and passes it on by reference") {
    counted::reset();
    auto const result =
        lexec::sync_wait(lexec::just() | lexec::then([]() noexcept { return counted{7}; }) |
                         lexec::let_value([](counted &c) noexcept { return lexec::just(c.value); }));
    CHECK(std::get<0>(*result) == 7);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 1);
}

TEST_CASE("into_variant moves each value once into the variant") {
    counted::reset();
    auto log = lexec_test::completion_log{};
    auto op = lexec::connect(lexec::just() | lexec::then([]() noexcept { return counted{7}; }) | lexec::into_variant,
                             lexec_test::checked_receiver<lexec::set_value_t(std::variant<std::tuple<counted>>)>{&log});
    lexec::start(op);
    CHECK(log.value_count == 1);
    CHECK(counted::counts.copies == 0);
    CHECK(counted::counts.moves == 1);
}

TEST_CASE("connecting an lvalue sender copies its captured values exactly once") {
    auto const sender = lexec::just(counted{7});
    counted::reset();
    auto const result = lexec::sync_wait(sender);
    CHECK(std::get<0>(*result).value == 7);
    CHECK(counted::counts.copies == 1);
    CHECK(counted::counts.moves == 1);
}
