#pragma once

#include <lexec/detail/meta.hpp>

#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>

namespace lexec::detail {

template <class T, class... Ts>
constexpr std::size_t index_in_pack() noexcept {
    constexpr bool matches[] = {std::is_same_v<T, Ts>..., true};
    auto index = std::size_t{0};
    while (not matches[index]) {
        ++index;
    }
    return index;
}

template <class... Ts>
constexpr std::size_t max_of(std::size_t const fallback, Ts const... values) noexcept {
    auto result = fallback;
    ((result = values > result ? values : result), ...);
    return result;
}

// Holds at most one object of the distinct types Ts, none of which need be movable:
// each is constructed in place from the prvalue a function returns.
template <class... Ts>
struct manual_variant {
    static_assert(list_size_v<unique_t<type_list<Ts...>>> == sizeof...(Ts), "manual_variant alternatives must be distinct");

    manual_variant() noexcept = default;
    manual_variant(manual_variant &&) = delete;
    ~manual_variant() { reset(); }

    // Destroys the current object first, so Make may reuse its resources.
    template <class T, class Make>
    T &emplace_with(Make &&make) noexcept(noexcept(T(static_cast<Make &&>(make)()))) {
        static_assert(is_one_of_v<T, Ts...>, "manual_variant: not an alternative");
        reset();
        auto *const object = ::new (static_cast<void *>(storage)) T(static_cast<Make &&>(make)());
        active = index_in_pack<T, Ts...>();
        return *object;
    }

    template <class T>
    T &get() noexcept {
        assert(active == (index_in_pack<T, Ts...>()) and "manual_variant: the requested alternative is not active");
        return *std::launder(reinterpret_cast<T *>(storage));
    }

    // Calls fn with the active object; there must be one.
    template <class Fn>
    void visit(Fn &&fn) noexcept {
        assert(active != kEmpty and "manual_variant: no active alternative to visit");
        static_cast<void>(
            ((active == index_in_pack<Ts, Ts...>() ? (static_cast<Fn &&>(fn)(get<Ts>()), true) : false) or ...));
    }

    void reset() noexcept {
        [[maybe_unused]] auto const current = active;
        active = kEmpty;
        (destroy_if_active<Ts>(current), ...);
    }

private:
    static constexpr std::size_t kEmpty = sizeof...(Ts);

    template <class T>
    void destroy_if_active(std::size_t const current) noexcept {
        if (current == index_in_pack<T, Ts...>()) {
            std::launder(reinterpret_cast<T *>(storage))->~T();
        }
    }

    alignas(max_of(1, alignof(Ts)...)) unsigned char storage[max_of(1, sizeof(Ts)...)];
    std::size_t active = kEmpty;
};

} // namespace lexec::detail
