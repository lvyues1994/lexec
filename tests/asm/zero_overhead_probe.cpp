// Compiled at -O2 and disassembled by compare_functions.py: the three functions must
// produce identical instructions.
#include "../support/reference_then.hpp"

#include <lexec/execution.hpp>

extern "C" int lexec_probe_direct(int const x) { return lexec_test::triple_plus_one(x); }

extern "C" int lexec_probe_framework(int const x) {
    auto out = 0;
    auto op = lexec::connect(lexec::just(x) | lexec::then(lexec_test::triple_plus_one), lexec_test::int_sink{&out});
    lexec::start(op);
    return out;
}

extern "C" int lexec_probe_reference(int const x) {
    auto out = 0;
    using sender = lexec_test::reference_then_sender<decltype(lexec::just(x)), lexec_test::triple_plus_one_t>;
    auto op = lexec::connect(sender{lexec::just(x), lexec_test::triple_plus_one}, lexec_test::int_sink{&out});
    lexec::start(op);
    return out;
}
