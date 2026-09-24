#pragma once

#include <lexec/detail/meta.hpp>

#include <type_traits>

namespace lexec {

struct operation_state_t {};

namespace detail {

template <class Op>
using start_member_t = decltype(std::declval<Op &>().start());

template <class Op>
using operation_state_concept_of_t = typename Op::operation_state_concept;

template <class Op, bool = is_detected_v<operation_state_concept_of_t, Op>>
inline constexpr bool enable_operation_state_v = false;

template <class Op>
inline constexpr bool enable_operation_state_v<Op, true> =
    std::is_base_of_v<operation_state_t, typename Op::operation_state_concept>;

} // namespace detail

struct start_t {
    template <class Op, class = detail::start_member_t<Op>>
    constexpr void operator()(Op &op) const noexcept {
        static_assert(noexcept(op.start()), "operation_state::start must be noexcept");
        op.start();
    }
};

inline constexpr start_t start{};

template <class Op>
inline constexpr bool is_operation_state_v = detail::enable_operation_state_v<Op> and std::is_object_v<Op> and
                                             std::is_invocable_v<start_t, Op &>;

} // namespace lexec
