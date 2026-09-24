#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <exception>
#include <string>
#include <type_traits>

namespace {

using lexec::completion_signatures;
using lexec::completion_signatures_of_t;
using lexec::set_error_t;
using lexec::set_value_t;

struct add_one {
    int operator()(int const v) const noexcept { return v + 1; }
};

struct add_two {
    int operator()(int const v) const noexcept { return v + 2; }
};

struct describe {
    std::string operator()(int const v) const { return "custom " + std::to_string(v); }
};

template <class Sndr>
using data_or_void_t = lexec::detail::detected_or_t<void, lexec::detail::data_of_t, Sndr>;

template <class Sndr, class Fn>
inline constexpr bool is_then_with_v =
    std::is_same_v<lexec::detail::detected_or_t<void, lexec::tag_of_t, Sndr>, lexec::then_t> and
    std::is_same_v<data_or_void_t<Sndr>, Fn>;

template <class Replacement>
struct replace_function {
    template <class Tag, class Data, class Child>
    auto operator()(Tag, Data &&, Child &&child) const {
        return lexec::then(static_cast<Child &&>(child), Replacement{});
    }
};

// Rewrites then(add_one) to then(add_two), and then(add_two) to then(describe), for
// senders completing in this domain; the second rewrite applies to the first's result.
struct rewriting_domain {
    template <class Sndr, class Env, std::enable_if_t<is_then_with_v<Sndr, add_one>, int> = 0>
    auto transform_sender(set_value_t, Sndr &&sndr, Env const &) const {
        return static_cast<Sndr &&>(sndr).apply(replace_function<add_two>{});
    }

    template <class Sndr, class Env, std::enable_if_t<is_then_with_v<Sndr, add_two>, int> = 0>
    auto transform_sender(set_value_t, Sndr &&sndr, Env const &) const {
        return static_cast<Sndr &&>(sndr).apply(replace_function<describe>{});
    }
};

struct start_marker {};
using marked_sender = decltype(lexec::just(start_marker{}));

// Replaces just(start_marker{}) when an operation starts in this domain.
struct starting_domain {
    template <class Sndr, class Env, std::enable_if_t<std::is_same_v<lexec::detail::remove_cvref_t<Sndr>, marked_sender>, int> = 0>
    auto transform_sender(lexec::start_t, Sndr &&, Env const &) const {
        return lexec::just(42);
    }
};

using rewriting_env = lexec::prop<lexec::get_domain_t, rewriting_domain>;
using add_one_sender = decltype(lexec::just(41) | lexec::then(add_one{}));

// The completions in an environment are those of the sender after its transformation.
static_assert(std::is_same_v<completion_signatures_of_t<add_one_sender>, completion_signatures<set_value_t(int)>>);
#if LEXEC_HAS_EXCEPTIONS
static_assert(std::is_same_v<completion_signatures_of_t<add_one_sender, rewriting_env>,
                             completion_signatures<set_value_t(std::string), set_error_t(std::exception_ptr)>>);
#else
static_assert(
    std::is_same_v<completion_signatures_of_t<add_one_sender, rewriting_env>, completion_signatures<set_value_t(std::string)>>);
#endif

// Without a transformation, transform_sender returns an rvalue sender as a new value,
// and an lvalue sender as itself.
static_assert(std::is_same_v<decltype(lexec::transform_sender(lexec::just(1), lexec::env<>{})), decltype(lexec::just(1))>);
[[maybe_unused]] auto const lvalue_sender = lexec::just(1);
static_assert(std::is_same_v<decltype(lexec::transform_sender(lvalue_sender, lexec::env<>{})), decltype(lvalue_sender) &>);

} // namespace

auto const rewriting = rewriting_env{lexec::get_domain, rewriting_domain{}};

TEST_CASE("connect applies the completion domain's transformations until the sender stops changing") {
    auto const result = lexec::sync_wait(lexec::write_env(lexec::just(41) | lexec::then(add_one{}), rewriting));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == "custom 41");
}

TEST_CASE("senders the domain does not transform connect unchanged") {
    auto const result =
        lexec::sync_wait(lexec::write_env(lexec::just(1) | lexec::then([](int v) noexcept { return v * 3; }), rewriting));
    CHECK(std::get<0>(*result) == 3);
}

TEST_CASE("connect applies the starting domain's start transformations") {
    auto const result =
        lexec::sync_wait(lexec::write_env(lexec::just(start_marker{}), lexec::prop{lexec::get_domain, starting_domain{}}));
    CHECK(std::get<0>(*result) == 42);
}
