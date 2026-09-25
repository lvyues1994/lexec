#pragma once

#include <lexec/execution.hpp>

#include <type_traits>
#include <utility>

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

// An operation on the heap that its receiver destroys on completion, as spawn does its
// own: nothing may touch an operation once it has completed.
template <class Sndr>
class destroyed_on_completion {
public:
    static void start(Sndr sndr, completion_log &log) {
        lexec::start((new destroyed_on_completion(std::move(sndr), log))->op);
    }

private:
    struct receiver {
        using receiver_concept = lexec::receiver_t;

        template <class... Vs>
        void set_value(Vs &&...) && noexcept {
            ++log->value_count;
            delete self;
        }

        template <class E>
        void set_error(E &&) && noexcept {
            ++log->error_count;
            delete self;
        }

        void set_stopped() && noexcept {
            ++log->stopped_count;
            delete self;
        }

        destroyed_on_completion *self;
        completion_log *log;
    };

    destroyed_on_completion(Sndr sndr, completion_log &log)
        : op(lexec::connect(std::move(sndr), receiver{this, &log})) {}

    lexec::connect_result_t<Sndr, receiver> op;
};

template <class Sndr>
void start_destroyed_on_completion(Sndr sndr, completion_log &log) {
    destroyed_on_completion<Sndr>::start(std::move(sndr), log);
}

template <class Sig, class Sigs>
inline constexpr bool contains_signature_v = false;

template <class Sig, class... Sigs>
inline constexpr bool contains_signature_v<Sig, lexec::completion_signatures<Sigs...>> =
    lexec::detail::is_one_of_v<Sig, Sigs...>;

// Equality of two sets of completion signatures, whatever their order.
template <class Expected, class Actual>
inline constexpr bool same_signature_set_v = false;

template <class... Expected, class... Actual>
inline constexpr bool same_signature_set_v<lexec::completion_signatures<Expected...>,
                                           lexec::completion_signatures<Actual...>> =
    sizeof...(Expected) == sizeof...(Actual) and
    (contains_signature_v<Expected, lexec::completion_signatures<Actual...>> and ...);

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
