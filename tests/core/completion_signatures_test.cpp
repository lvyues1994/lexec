#include <lexec/execution.hpp>

#include <exception>
#include <tuple>
#include <type_traits>
#include <variant>

namespace {

using lexec::completion_signatures;
using lexec::set_error_t;
using lexec::set_stopped_t;
using lexec::set_value_t;

using mixed = completion_signatures<set_error_t(int), set_value_t(), set_stopped_t(), set_value_t(int, char)>;

// Additional signatures come first, then values, errors, and stopped, without duplicates.
static_assert(std::is_same_v<lexec::transform_completion_signatures<mixed, completion_signatures<set_error_t(int)>>,
                             completion_signatures<set_error_t(int), set_value_t(), set_value_t(int, char),
                                                   set_stopped_t()>>);

template <class... Vs>
using to_single_value = completion_signatures<set_value_t(double)>;

template <class E>
using drop_error = completion_signatures<>;

static_assert(std::is_same_v<lexec::transform_completion_signatures<mixed, completion_signatures<>, to_single_value,
                                                                    drop_error, completion_signatures<>>,
                             completion_signatures<set_value_t(double)>>);

struct nested_typedef_sender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<set_value_t(int &), set_error_t(std::exception_ptr)>;
};

struct env_dependent_sender {
    using sender_concept = lexec::sender_t;

    template <class Self, class Env>
    static auto get_completion_signatures() -> completion_signatures<set_value_t(Env)>;
};

static_assert(lexec::is_sender_in_v<nested_typedef_sender>);
static_assert(not lexec::is_dependent_sender_v<nested_typedef_sender>);
static_assert(lexec::is_dependent_sender_v<env_dependent_sender>);
static_assert(lexec::is_sender_in_v<env_dependent_sender, lexec::env<>>);
static_assert(std::is_same_v<lexec::completion_signatures_of_t<env_dependent_sender, lexec::env<>>,
                             completion_signatures<set_value_t(lexec::env<>)>>);

static_assert(std::is_same_v<lexec::value_types_of_t<nested_typedef_sender>, std::variant<std::tuple<int>>>);
static_assert(std::is_same_v<lexec::error_types_of_t<nested_typedef_sender>, std::variant<std::exception_ptr>>);
static_assert(not lexec::sends_stopped<nested_typedef_sender>);

static_assert(std::is_same_v<lexec::value_types_of_t<decltype(lexec::just(1, 'c'))>, std::variant<std::tuple<int, char>>>);
static_assert(lexec::sends_stopped<decltype(lexec::just_stopped())>);

} // namespace
