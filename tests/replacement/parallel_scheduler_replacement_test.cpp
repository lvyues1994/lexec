// Built as its own program: it replaces query_parallel_scheduler_backend, which a program
// defines at most once.

#include <lexec/execution.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using lexec::parallel_scheduler_replacement::bulk_item_receiver_proxy;
using lexec::parallel_scheduler_replacement::parallel_scheduler_backend;
using lexec::parallel_scheduler_replacement::receiver_proxy;

enum class outcome { value, error, stopped };

// Runs everything on the calling thread, records what it is asked to do, and completes
// as told.
struct recording_backend final : parallel_scheduler_backend {
    void schedule(receiver_proxy &receiver, lexec::span<std::byte> const storage) noexcept override {
        ++schedules;
        storage_size = storage.size();
        complete(receiver);
    }

    void schedule_bulk_chunked(std::size_t const size, bulk_item_receiver_proxy &receiver,
                               lexec::span<std::byte>) noexcept override {
        calls.push_back({true, size});
        if (size != 0) {
            receiver.execute(0, size);
        }
        complete(receiver);
    }

    void schedule_bulk_unchunked(std::size_t const size, bulk_item_receiver_proxy &receiver,
                                 lexec::span<std::byte>) noexcept override {
        calls.push_back({false, size});
        for (auto i = std::size_t{0}; i < size; ++i) {
            receiver.execute(i, i + 1);
        }
        complete(receiver);
    }

    void complete(receiver_proxy &receiver) noexcept {
        switch (next) {
        case outcome::value:
            receiver.set_value();
            break;
        case outcome::error:
            receiver.set_error(std::make_exception_ptr(std::runtime_error{"backend"}));
            break;
        case outcome::stopped:
            receiver.set_stopped();
            break;
        }
    }

    struct bulk_call {
        bool chunked;
        std::size_t size;
    };

    int schedules = 0;
    std::size_t storage_size = 0;
    std::vector<bulk_call> calls;
    outcome next = outcome::value;
};

std::shared_ptr<recording_backend> const &backend() {
    static auto const instance = std::make_shared<recording_backend>();
    return instance;
}

} // namespace

std::shared_ptr<parallel_scheduler_backend> lexec::parallel_scheduler_replacement::query_parallel_scheduler_backend() {
    return backend();
}

TEST_CASE("a program's query_parallel_scheduler_backend replaces lexec's") {
    backend()->next = outcome::value;
    auto const caller = std::this_thread::get_id();
    auto const result = lexec::sync_wait(lexec::schedule(lexec::get_parallel_scheduler()) |
                                         lexec::then([]() noexcept { return std::this_thread::get_id(); }));
    CHECK(std::get<0>(*result) == caller);
    CHECK(backend()->schedules == 1);
    CHECK(backend()->storage_size == lexec::detail::parallel_backend_storage_size);
}

TEST_CASE("bulk reaches the backend with the whole shape under a parallel policy, and as one call otherwise") {
    backend()->next = outcome::value;
    backend()->calls.clear();
    auto const sch = lexec::get_parallel_scheduler();
    auto indices = std::vector<int>{};
    auto const record = [&indices](int i) { indices.push_back(i); };
    lexec::sync_wait(lexec::schedule(sch) | lexec::bulk(lexec::par, 5, record));
    lexec::sync_wait(lexec::schedule(sch) | lexec::bulk(lexec::seq, 4, record));
    lexec::sync_wait(lexec::schedule(sch) | lexec::bulk_unchunked(lexec::par_unseq, 3, record));
    lexec::sync_wait(lexec::schedule(sch) | lexec::bulk_unchunked(lexec::unseq, 2, record));
    REQUIRE(backend()->calls.size() == 4);
    CHECK(backend()->calls[0].chunked);
    CHECK(backend()->calls[0].size == 5);
    CHECK(backend()->calls[1].chunked);
    CHECK(backend()->calls[1].size == 1);
    CHECK_FALSE(backend()->calls[2].chunked);
    CHECK(backend()->calls[2].size == 3);
    CHECK_FALSE(backend()->calls[3].chunked);
    CHECK(backend()->calls[3].size == 1);
    CHECK(indices == std::vector<int>{0, 1, 2, 3, 4, 0, 1, 2, 3, 0, 1, 2, 0, 1});
}

TEST_CASE("the backend's stopped and error completions reach the receiver") {
    auto const sch = lexec::get_parallel_scheduler();
    backend()->next = outcome::stopped;
    CHECK_FALSE(lexec::sync_wait(lexec::schedule(sch)).has_value());
    CHECK_FALSE(lexec::sync_wait(lexec::schedule(sch) | lexec::bulk(lexec::par, 3, [](int) noexcept {})).has_value());
#if LEXEC_HAS_EXCEPTIONS
    backend()->next = outcome::error;
    CHECK_THROWS_AS(lexec::sync_wait(lexec::schedule(sch)), std::runtime_error);
    CHECK_THROWS_AS(lexec::sync_wait(lexec::schedule(sch) | lexec::bulk(lexec::par, 3, [](int) noexcept {})),
                    std::runtime_error);
#endif
    backend()->next = outcome::value;
}
