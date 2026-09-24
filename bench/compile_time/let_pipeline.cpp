// Nests let_value Depth times; every level computes a second sender's completions in
// its own environment and holds its operation in the let's variant.
#include <lexec/execution.hpp>

namespace {

// A lambda defined inside add_lets would carry the previous pipeline in its type name,
// doubling GCC's debug information per stage.
template <int K>
struct continue_with {
    auto operator()(int const &v) const noexcept { return lexec::just(v + K); }
};

template <int Depth, class Sndr>
auto add_lets(Sndr &&sndr) {
    if constexpr (Depth == 0) {
        return static_cast<Sndr &&>(sndr);
    } else {
        return add_lets<Depth - 1>(static_cast<Sndr &&>(sndr) | lexec::let_value(continue_with<Depth>{}));
    }
}

} // namespace

int run_lets() {
    auto const shallow = lexec::sync_wait(add_lets<5>(lexec::just(0)));
    auto const deep = lexec::sync_wait(add_lets<20>(lexec::just(0)));
    auto const mixed = lexec::sync_wait(lexec::just(1) | lexec::let_value(continue_with<1>{}) |
                                        lexec::stopped_as_optional | lexec::into_variant);
    return std::get<0>(*shallow) + std::get<0>(*deep) + static_cast<int>(std::get<0>(*mixed).index());
}
