#include "../support/test_receivers.hpp"

#include <lexec/execution.hpp>

#include <type_traits>

namespace {

using int_receiver = lexec_test::checked_receiver<lexec::set_value_t(int)>;

struct not_a_receiver {
    void set_value(int) && noexcept;
};

static_assert(lexec::is_receiver_v<int_receiver>);
static_assert(not lexec::is_receiver_v<not_a_receiver>);
static_assert(lexec::is_receiver_of_v<int_receiver, lexec::completion_signatures<lexec::set_value_t(int)>>);
static_assert(not lexec::is_receiver_of_v<int_receiver, lexec::completion_signatures<lexec::set_stopped_t()>>);

// Completion functions consume the receiver: lvalues and const rvalues are rejected.
static_assert(std::is_invocable_v<lexec::set_value_t, int_receiver, int>);
static_assert(not std::is_invocable_v<lexec::set_value_t, int_receiver &, int>);
static_assert(not std::is_invocable_v<lexec::set_value_t, int_receiver const, int>);

using just_int = decltype(lexec::just(1));

static_assert(lexec::is_sender_v<just_int>);
static_assert(not lexec::is_sender_v<int>);
static_assert(lexec::is_sender_to_v<just_int, int_receiver>);
static_assert(not lexec::is_sender_to_v<decltype(lexec::just_stopped()), int_receiver>);
static_assert(lexec::is_operation_state_v<lexec::connect_result_t<just_int, int_receiver>>);
static_assert(not std::is_move_constructible_v<lexec::connect_result_t<just_int, int_receiver>>);

using run_loop_scheduler = decltype(std::declval<lexec::run_loop &>().get_scheduler());

static_assert(lexec::is_scheduler_v<run_loop_scheduler>);
static_assert(not lexec::is_scheduler_v<int>);
static_assert(lexec::is_sender_v<lexec::schedule_result_t<run_loop_scheduler>>);

} // namespace
