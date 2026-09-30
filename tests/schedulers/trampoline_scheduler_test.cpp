#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

namespace {

using lexec::completion_signatures;
using lexec::set_stopped_t;
using lexec::set_value_t;

using trampoline_sender = lexec::schedule_result_t<lexec::trampoline_scheduler>;
using stoppable_env = lexec::prop<lexec::get_stop_token_t, lexec::inplace_stop_token>;

static_assert(lexec::is_scheduler_v<lexec::trampoline_scheduler>);
static_assert(lexec::is_dependent_sender_v<trampoline_sender>);
static_assert(std::is_same_v<lexec::completion_signatures_of_t<trampoline_sender, lexec::env<>>,
                             completion_signatures<set_value_t()>>);
static_assert(std::is_same_v<lexec::completion_signatures_of_t<trampoline_sender, stoppable_env>,
                             completion_signatures<set_value_t(), set_stopped_t()>>);

// Each run schedules the next from inside its completion, as a loop of synchronous
// iterations does, and records how deeply the runs nest on the stack.
struct nested_runs {
    struct receiver {
        using receiver_concept = lexec::receiver_t;
        void set_value() && noexcept { runs->run(); }
        nested_runs *runs;
    };

    using operation = lexec::connect_result_t<trampoline_sender, receiver>;

    void schedule_next() {
        ops.emplace_back(new operation(lexec::connect(lexec::schedule(scheduler), receiver{this})));
        lexec::start(*ops.back());
    }

    void run() noexcept {
        ++nesting;
        deepest = std::max(deepest, nesting);
        if (++done < total) {
            schedule_next();
        }
        --nesting;
    }

    lexec::trampoline_scheduler scheduler;
    int total;
    int done = 0;
    int nesting = 0;
    int deepest = 0;
    std::vector<std::unique_ptr<operation>> ops{};
};

constexpr auto unlimited_stack = std::numeric_limits<std::size_t>::max();

} // namespace

TEST_CASE("the trampoline runs nested work inline up to its depth, and the rest once the stack unwinds") {
    auto runs = nested_runs{lexec::trampoline_scheduler{4, unlimited_stack}, 100};
    runs.schedule_next();
    CHECK(runs.done == 100);
    CHECK(runs.deepest == 4);
}

TEST_CASE("the trampoline also bounds the stack it lets nested work use") {
    auto runs = nested_runs{lexec::trampoline_scheduler{1000, 1}, 100};
    runs.schedule_next();
    CHECK(runs.done == 100);
    CHECK(runs.deepest == 1);
    auto defaults = nested_runs{lexec::trampoline_scheduler{}, 1000};
    defaults.schedule_next();
    CHECK(defaults.done == 1000);
    CHECK(defaults.deepest <= 16);
}

TEST_CASE("the trampoline completes with stopped once the receiver has asked to stop") {
    CHECK(lexec::sync_wait(lexec::schedule(lexec::trampoline_scheduler{})).has_value());
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto const stopped = lexec::sync_wait(lexec::write_env(lexec::schedule(lexec::trampoline_scheduler{}),
                                                           lexec::prop{lexec::get_stop_token, source.get_token()}));
    CHECK_FALSE(stopped.has_value());
}

TEST_CASE("trampoline schedulers are equal when their limits are") {
    CHECK(lexec::trampoline_scheduler{} == lexec::trampoline_scheduler{16, 4096});
    CHECK(lexec::trampoline_scheduler{8} != lexec::trampoline_scheduler{});
    CHECK(lexec::trampoline_scheduler{16, 1} != lexec::trampoline_scheduler{});
}
