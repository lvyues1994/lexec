// Nests `then` Depth times, giving each stage a distinct function object type, so the
// sender, receiver, and operation types grow with the depth of the pipeline.
#include <lexec/execution.hpp>

#include <string>
#include <utility>

namespace {

// A lambda defined inside add_stages would carry the previous pipeline in its type
// name, doubling GCC's debug information per stage.
template <int K>
struct add_k {
    int operator()(int const v) const noexcept { return v + K; }
};

template <int Depth, class Sndr>
auto add_stages(Sndr &&sndr) {
    if constexpr (Depth == 0) {
        return static_cast<Sndr &&>(sndr);
    } else {
        return add_stages<Depth - 1>(static_cast<Sndr &&>(sndr) | lexec::then(add_k<Depth>{}));
    }
}

} // namespace

int run_pipelines() {
    auto const shallow = lexec::sync_wait(add_stages<10>(lexec::just(0)));
    auto const medium = lexec::sync_wait(add_stages<20>(lexec::just(0)));
    auto const deep = lexec::sync_wait(add_stages<40>(lexec::just(0)));
    auto const mixed = lexec::sync_wait(
        lexec::just_error(std::string{"e"}) | lexec::upon_error([](std::string e) { return static_cast<int>(e.size()); }) |
        lexec::then([](int v) { return std::to_string(v); }) | lexec::upon_stopped([] { return std::string{}; }));
    return std::get<0>(*shallow) + std::get<0>(*medium) + std::get<0>(*deep) +
           static_cast<int>(std::get<0>(*mixed).size());
}
