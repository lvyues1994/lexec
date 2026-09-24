#include "../support/reference_then.hpp"

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

namespace {

using just_int = decltype(lexec::just(1));
using framework_operation =
    lexec::connect_result_t<decltype(lexec::just(1) | lexec::then(lexec_test::triple_plus_one)), lexec_test::int_sink>;
using reference_operation =
    lexec::connect_result_t<lexec_test::reference_then_sender<just_int, lexec_test::triple_plus_one_t>,
                            lexec_test::int_sink>;

static_assert(sizeof(framework_operation) == sizeof(reference_operation),
              "the framework's then must not be larger than a hand-written then");

} // namespace

TEST_CASE("framework then and hand-written then compute the same result") {
    auto framework_out = 0;
    auto reference_out = 0;
    auto framework_op =
        lexec::connect(lexec::just(5) | lexec::then(lexec_test::triple_plus_one), lexec_test::int_sink{&framework_out});
    auto reference_op = lexec::connect(
        lexec_test::reference_then_sender<just_int, lexec_test::triple_plus_one_t>{lexec::just(5),
                                                                                  lexec_test::triple_plus_one},
        lexec_test::int_sink{&reference_out});
    lexec::start(framework_op);
    lexec::start(reference_op);
    CHECK(framework_out == 16);
    CHECK(reference_out == 16);
}
