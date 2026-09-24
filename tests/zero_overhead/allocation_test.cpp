#include "../support/allocation_counter.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <string>
#include <thread>

TEST_CASE("a synchronous pipeline allocates nothing") {
    auto const before = lexec_test::allocation_count();
    auto const result = lexec::sync_wait(lexec::just(20) | lexec::then([](int v) noexcept { return v + 1; }) |
                                         lexec::then([](int v) noexcept { return v * 2; }));
    auto const after = lexec_test::allocation_count();
    CHECK(std::get<0>(*result) == 42);
    CHECK(after == before);
}

TEST_CASE("completing on another thread's run_loop allocates nothing") {
    auto loop = lexec::run_loop{};
    auto driver = std::thread{[&loop] { loop.run(); }};
    auto const before = lexec_test::allocation_count();
    auto const result =
        lexec::sync_wait(lexec::schedule(loop.get_scheduler()) | lexec::then([]() noexcept { return 42; }));
    auto const after = lexec_test::allocation_count();
    loop.finish();
    driver.join();
    CHECK(std::get<0>(*result) == 42);
    CHECK(after == before);
}

TEST_CASE("let, into_variant, and stopped_as_optional allocate nothing") {
    auto const before = lexec_test::allocation_count();
    auto const result = lexec::sync_wait(lexec::just(20) |
                                         lexec::let_value([](int &v) noexcept { return lexec::just(v + 22); }) |
                                         lexec::stopped_as_optional | lexec::into_variant);
    auto const after = lexec_test::allocation_count();
    CHECK(*std::get<0>(std::get<0>(std::get<0>(*result))) == 42);
    CHECK(after == before);
}

TEST_CASE("the allocation counter observes allocations") {
    auto const before = lexec_test::allocation_count();
    auto const text = std::string(64, 'x');
    CHECK(lexec_test::allocation_count() > before);
    CHECK(text.size() == 64);
}
