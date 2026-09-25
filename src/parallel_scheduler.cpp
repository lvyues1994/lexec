// lexec::runtime's parallel_scheduler backend. It is alone in this file so that a program
// that defines query_parallel_scheduler_backend never links it: a static library member
// is linked only for a symbol that nothing else defines.

#include <lexec/schedulers/parallel_scheduler.hpp>
#include <lexec/schedulers/static_thread_pool.hpp>

#include <cstddef>
#include <memory>
#include <new>

namespace lexec::detail {

namespace {

using parallel_scheduler_replacement::bulk_item_receiver_proxy;
using parallel_scheduler_replacement::parallel_scheduler_backend;
using parallel_scheduler_replacement::receiver_proxy;

struct schedule_state : pool_task {
    static void execute(pool_task *const task) noexcept {
        auto &receiver = *static_cast<schedule_state *>(task)->receiver;
        auto const token = receiver.try_query<inplace_stop_token>(get_stop_token);
        if (token.has_value() and token->stop_requested()) {
            receiver.set_stopped();
        } else {
            receiver.set_value();
        }
    }

    receiver_proxy *receiver;
};

// First a hop onto a worker, since every iteration and the completion must run on one.
struct bulk_state : pool_task, pool_bulk_job {
    static void hop(pool_task *const task) noexcept {
        auto &self = *static_cast<bulk_state *>(task);
        if (self.size == 0) {
            self.receiver->set_value();
        } else {
            run_bulk(*self.pool, self);
        }
    }

    static void run(pool_bulk_job *const job, std::size_t const begin, std::size_t const end) noexcept {
        auto &self = *static_cast<bulk_state *>(job);
        if (self.chunked) {
            self.receiver->execute(begin, end);
        } else {
            for (auto i = begin; i != end; ++i) {
                self.receiver->execute(i, i + 1);
            }
        }
    }

    static void finish(pool_bulk_job *const job) noexcept { static_cast<bulk_state *>(job)->receiver->set_value(); }

    thread_pool_impl *pool;
    bulk_item_receiver_proxy *receiver;
    bool chunked;
};

static_assert(sizeof(schedule_state) <= parallel_backend_storage_size and
              sizeof(bulk_state) <= parallel_backend_storage_size and
              alignof(bulk_state) <= alignof(std::max_align_t));

// Builds each operation's state in the storage the operation provides.
struct default_backend final : parallel_scheduler_backend {
    void schedule(receiver_proxy &receiver, span<std::byte> const storage) noexcept override {
        auto *const state = ::new (static_cast<void *>(storage.data())) schedule_state{};
        state->execute_fn = &schedule_state::execute;
        state->receiver = &receiver;
        enqueue(*pool_impl(), state);
    }

    void schedule_bulk_chunked(std::size_t const size, bulk_item_receiver_proxy &receiver,
                               span<std::byte> const storage) noexcept override {
        start_bulk(size, receiver, storage, true);
    }

    void schedule_bulk_unchunked(std::size_t const size, bulk_item_receiver_proxy &receiver,
                                 span<std::byte> const storage) noexcept override {
        start_bulk(size, receiver, storage, false);
    }

    void start_bulk(std::size_t const size, bulk_item_receiver_proxy &receiver, span<std::byte> const storage,
                    bool const chunked) noexcept {
        auto *const state = ::new (static_cast<void *>(storage.data())) bulk_state{};
        state->execute_fn = &bulk_state::hop;
        state->run_chunk = &bulk_state::run;
        state->complete = &bulk_state::finish;
        state->size = size;
        state->pool = pool_impl();
        state->receiver = &receiver;
        state->chunked = chunked;
        enqueue(*state->pool, state);
    }

    thread_pool_impl *pool_impl() noexcept { return pool.get_scheduler().pool; }

    static_thread_pool pool;
};

} // namespace

} // namespace lexec::detail

namespace lexec::parallel_scheduler_replacement {

#if defined(__GNUC__) || defined(__clang__)
// Weak as well, in case this file is linked all the same, as with a whole-archive link.
__attribute__((weak))
#endif
std::shared_ptr<parallel_scheduler_backend> query_parallel_scheduler_backend() {
    static auto const backend = std::make_shared<detail::default_backend>();
    return backend;
}

} // namespace lexec::parallel_scheduler_replacement
