#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <new>

namespace {

// Destroys and frees the operation that completes it, as sync_wait and when_all may do.
// Under AddressSanitizer any access to the operation after completion is reported.
struct destroying_receiver {
    using receiver_concept = lexec::receiver_t;

    void set_value(int const v) && noexcept {
        auto *const target = out;
        destroy(operation);
        *target = v;
    }

    void (*destroy)(void *) noexcept;
    void *operation;
    int *out;
};

template <class Op>
void destroy_operation(void *const op) noexcept {
    static_cast<Op *>(op)->~Op();
    ::operator delete(op);
}

template <class Sndr>
int run_until_destroyed(Sndr const &sender) {
    using operation = lexec::connect_result_t<Sndr const &, destroying_receiver>;
    auto out = 0;
    auto *const storage = ::operator new(sizeof(operation));
    auto *const op = ::new (storage)
        operation(lexec::connect(sender, destroying_receiver{&destroy_operation<operation>, storage, &out}));
    lexec::start(*op);
    return out;
}

} // namespace

TEST_CASE("an operation may be destroyed from inside its own completion") {
    CHECK(run_until_destroyed(lexec::just(6) | lexec::then([](int v) noexcept { return v * 7; })) == 42);
}

TEST_CASE("a let operation may be destroyed from inside its second operation's completion") {
    CHECK(run_until_destroyed(lexec::just(6) | lexec::let_value([](int &v) noexcept { return lexec::just(v * 7); })) == 42);
}
