#pragma once

#include <lexec/execution.hpp>

#include <type_traits>

namespace lexec_test {

struct completion_log {
    int value_count = 0;
    int error_count = 0;
    int stopped_count = 0;

    int total() const noexcept { return value_count + error_count + stopped_count; }
};

// Accepts exactly the completions in Sigs, so a sender that completes with a
// signature it did not declare fails to compile.
template <class... Sigs>
struct checked_receiver {
    using receiver_concept = lexec::receiver_t;

    template <class... Vs,
              std::enable_if_t<lexec::detail::is_one_of_v<lexec::set_value_t(Vs...), Sigs...>, int> = 0>
    void set_value(Vs &&...) && noexcept {
        ++log->value_count;
    }

    template <class E, std::enable_if_t<lexec::detail::is_one_of_v<lexec::set_error_t(E), Sigs...>, int> = 0>
    void set_error(E &&) && noexcept {
        ++log->error_count;
    }

    template <class Sig = lexec::set_stopped_t(), std::enable_if_t<lexec::detail::is_one_of_v<Sig, Sigs...>, int> = 0>
    void set_stopped() && noexcept {
        ++log->stopped_count;
    }

    completion_log *log;
};

struct lifetime_counts {
    int copies = 0;
    int moves = 0;
};

// Counts copies and moves of every instance; tests reset the counts before measuring.
struct counted {
    static inline lifetime_counts counts{};

    static void reset() noexcept { counts = {}; }

    explicit counted(int value_) noexcept : value(value_) {}
    counted(counted const &other) noexcept : value(other.value) { ++counts.copies; }
    counted(counted &&other) noexcept : value(other.value) { ++counts.moves; }
    counted &operator=(counted const &) = delete;
    counted &operator=(counted &&) = delete;

    int value;
};

} // namespace lexec_test
