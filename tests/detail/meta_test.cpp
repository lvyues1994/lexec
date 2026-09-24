#include <lexec/detail/meta.hpp>

#include <type_traits>

namespace {

using lexec::detail::concat_t;
using lexec::detail::type_list;
using lexec::detail::unique_t;

static_assert(std::is_same_v<concat_t<type_list<int>, type_list<>, type_list<char, int>>, type_list<int, char, int>>);
static_assert(std::is_same_v<unique_t<type_list<int, char, int, double, char>>, type_list<int, char, double>>);
static_assert(std::is_same_v<unique_t<type_list<>>, type_list<>>);

struct callable {
    int operator()(int) noexcept;
    void operator()(char);
};

static_assert(lexec::detail::is_callable_v<callable, int>);
static_assert(not lexec::detail::is_callable_v<callable, int, int>);
static_assert(lexec::detail::is_nothrow_callable_v<callable, int>);
static_assert(not lexec::detail::is_nothrow_callable_v<callable, char>);
static_assert(not lexec::detail::is_nothrow_callable_v<callable, int, int>);

static_assert(std::is_same_v<lexec::detail::member_like_t<int, char>, char>);
static_assert(std::is_same_v<lexec::detail::member_like_t<int &&, char>, char>);
static_assert(std::is_same_v<lexec::detail::member_like_t<int &, char>, char const &>);
static_assert(std::is_same_v<lexec::detail::member_like_t<int const &, char>, char const &>);

} // namespace
