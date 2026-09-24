#include <lexec/detail/manual_variant.hpp>

#include <doctest/doctest.h>

namespace {

struct immovable {
    explicit immovable(int value_, int *destroyed_) noexcept : value(value_), destroyed(destroyed_) {}
    immovable(immovable &&) = delete;
    ~immovable() { ++*destroyed; }

    int value;
    int *destroyed;
};

struct other_immovable {
    explicit other_immovable(int *destroyed_) noexcept : destroyed(destroyed_) {}
    other_immovable(other_immovable &&) = delete;
    ~other_immovable() { ++*destroyed; }

    int *destroyed;
};

using variant = lexec::detail::manual_variant<immovable, other_immovable>;

} // namespace

TEST_CASE("manual_variant constructs an immovable object in place from a prvalue") {
    auto destroyed = 0;
    auto v = variant{};
    auto &object = v.emplace_with<immovable>([&] { return immovable{42, &destroyed}; });
    CHECK(object.value == 42);
    CHECK(&v.get<immovable>() == &object);
    CHECK(destroyed == 0);
}

TEST_CASE("manual_variant destroys the current object before making the next one") {
    auto destroyed_first = 0;
    auto destroyed_second = 0;
    auto destroyed_when_made = -1;
    auto v = variant{};
    v.emplace_with<immovable>([&] { return immovable{1, &destroyed_first}; });
    v.emplace_with<other_immovable>([&] {
        destroyed_when_made = destroyed_first;
        return other_immovable{&destroyed_second};
    });
    CHECK(destroyed_when_made == 1);
    v.reset();
    CHECK(destroyed_second == 1);
    v.reset();
    CHECK(destroyed_second == 1);
}

TEST_CASE("manual_variant destroys its active object when it is destroyed") {
    auto destroyed = 0;
    {
        auto v = variant{};
        v.emplace_with<immovable>([&] { return immovable{1, &destroyed}; });
    }
    CHECK(destroyed == 1);
}
