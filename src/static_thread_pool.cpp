#include <lexec/schedulers/static_thread_pool.hpp>

#include "bwos_queue.hpp"

#include <lexec/detail/spin_wait.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace lexec::detail {

namespace {

// Small blocks keep few tasks private to the owner: its current block cannot be stolen.
constexpr std::size_t kLocalBlocks = 32;
constexpr std::size_t kLocalBlockSize = 8;
// A worker busy with local work still takes remote submissions this often.
constexpr std::uint32_t kRemotePeriod = 64;
constexpr int kSpinRounds = 2048;
constexpr int kPausesPerRound = 4;

// A lock-free intrusive stack that only its owner consumes, all of it at once.
struct remote_queue {
    void push(pool_task *const task) noexcept {
        auto head = top.load(std::memory_order_relaxed);
        do {
            task->next = head;
        } while (not top.compare_exchange_weak(head, task, std::memory_order_release, std::memory_order_relaxed));
    }

    // The tasks, newest first.
    pool_task *take_all() noexcept {
        if (top.load(std::memory_order_relaxed) == nullptr) {
            return nullptr;
        }
        return top.exchange(nullptr, std::memory_order_acquire);
    }

    alignas(kCacheLine) std::atomic<pool_task *> top{nullptr};
};

enum class worker_state : std::uint8_t { running, sleeping, notified };

struct worker {
    worker() : local(kLocalBlocks, kLocalBlockSize) {}

    bwos_queue<pool_task *> local;
    remote_queue remote;
    // A notification that arrives while the worker runs keeps it from sleeping next time,
    // so steady submissions keep it awake without system calls.
    alignas(kCacheLine) std::atomic<worker_state> state{worker_state::running};
    std::mutex mutex;
    std::condition_variable wakeup;
    std::uint32_t random_state = 0;
    std::thread thread;
};

struct current_worker_slot {
    thread_pool_impl const *pool;
    std::uint32_t index;
};

thread_local current_worker_slot current_worker{nullptr, 0};

// Each submitting thread spreads its tasks over the workers on its own, without a shared
// counter to contend on.
thread_local std::uint32_t submit_counter =
    static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

std::uint32_t next_random(std::uint32_t &state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// True when the worker was asleep. Taking the mutex orders the notification after the
// worker either saw the new state or began waiting.
bool notify(worker &w) noexcept {
    if (w.state.exchange(worker_state::notified, std::memory_order_acq_rel) != worker_state::sleeping) {
        return false;
    }
    {
        auto const lock = std::lock_guard<std::mutex>{w.mutex};
    }
    w.wakeup.notify_one();
    return true;
}

} // namespace

struct thread_pool_impl {
    explicit thread_pool_impl(std::uint32_t const count_) : workers(std::make_unique<worker[]>(count_)), count(count_) {
        for (auto i = std::uint32_t{0}; i < count; ++i) {
            workers[i].random_state = 0x9E37'79B9u * (i + 1);
        }
        for (auto i = std::uint32_t{0}; i < count; ++i) {
            workers[i].thread = std::thread{[this, i] { run(i); }};
        }
    }

    thread_pool_impl(thread_pool_impl &&) = delete;

    ~thread_pool_impl() {
        for (auto i = std::uint32_t{0}; i < count; ++i) {
            {
                auto const lock = std::lock_guard<std::mutex>{workers[i].mutex};
                stopping.store(true, std::memory_order_relaxed);
            }
            workers[i].wakeup.notify_one();
        }
        for (auto i = std::uint32_t{0}; i < count; ++i) {
            workers[i].thread.join();
        }
    }

    void enqueue(pool_task *const task) noexcept {
        if (current_worker.pool == this) {
            auto &self = workers[current_worker.index];
            if (not self.local.push_back(task)) {
                self.remote.push(task);
            }
            wake_idle(current_worker.index);
            return;
        }
        auto &target = workers[submit_counter++ % count];
        target.remote.push(task);
        notify(target);
    }

    // A task pushed to a running worker's own queue runs there in any case; a sleeping
    // worker, if there is one, may steal it sooner.
    void wake_idle(std::uint32_t const busy) noexcept {
        if (sleepers.load(std::memory_order_relaxed) == 0) {
            return;
        }
        for (auto i = std::uint32_t{0}; i < count; ++i) {
            if (i != busy and workers[i].state.load(std::memory_order_relaxed) == worker_state::sleeping and
                notify(workers[i])) {
                return;
            }
        }
    }

    // Moves the worker's remote submissions into its local queue, oldest last so that
    // they run first in, first out; returns the oldest.
    static pool_task *take_remote(worker &self) noexcept {
        auto *task = self.remote.take_all();
        if (task == nullptr) {
            return nullptr;
        }
        while (task->next != nullptr) {
            auto *const next = task->next;
            if (not self.local.push_back(task)) {
                self.remote.push(task);
            }
            task = next;
        }
        return task;
    }

    pool_task *find_task(std::uint32_t const index) noexcept {
        auto &self = workers[index];
        if (auto *const task = self.local.pop_back()) {
            return task;
        }
        if (auto *const task = take_remote(self)) {
            return task;
        }
        for (auto attempt = std::uint32_t{0}; attempt < 2 * count; ++attempt) {
            auto const victim = next_random(self.random_state) % count;
            if (victim != index) {
                if (auto *const task = workers[victim].local.steal_front()) {
                    return task;
                }
            }
        }
        return nullptr;
    }

    // Spins, then sleeps until notified; null once the pool is stopping and drained.
    // Spinning pauses without yielding: a yield is a system call that makes a waiting
    // worker slow to notice work, which is the latency the spinning is there to avoid.
    pool_task *wait_for_task(std::uint32_t const index) noexcept {
        auto &self = workers[index];
        for (auto round = 0; round < kSpinRounds; ++round) {
            for (auto pause = 0; pause < kPausesPerRound; ++pause) {
                cpu_relax();
            }
            if (auto *const task = find_task(index)) {
                return task;
            }
        }
        while (true) {
            {
                auto lock = std::unique_lock<std::mutex>{self.mutex};
                if (stopping.load(std::memory_order_relaxed)) {
                    lock.unlock();
                    return find_task(index);
                }
                auto expected = worker_state::running;
                // A submission since the worker last ran makes this fail: look again.
                if (self.state.compare_exchange_strong(expected, worker_state::sleeping, std::memory_order_acq_rel)) {
                    if (auto *const task = take_remote(self)) {
                        self.state.store(worker_state::running, std::memory_order_relaxed);
                        return task;
                    }
                    sleepers.fetch_add(1, std::memory_order_relaxed);
                    self.wakeup.wait(lock, [&] {
                        return self.state.load(std::memory_order_acquire) != worker_state::sleeping or
                               stopping.load(std::memory_order_relaxed);
                    });
                    sleepers.fetch_sub(1, std::memory_order_relaxed);
                }
                self.state.store(worker_state::running, std::memory_order_relaxed);
            }
            if (auto *const task = find_task(index)) {
                return task;
            }
        }
    }

    void run(std::uint32_t const index) noexcept {
        current_worker = current_worker_slot{this, index};
        auto &self = workers[index];
        for (auto executed = std::uint32_t{1};; ++executed) {
            auto *task = executed % kRemotePeriod == 0 ? take_remote(self) : nullptr;
            if (task == nullptr) {
                task = find_task(index);
            }
            if (task == nullptr) {
                task = wait_for_task(index);
            }
            if (task == nullptr) {
                return;
            }
            task->execute_fn(task);
        }
    }

    std::unique_ptr<worker[]> workers;
    std::uint32_t count;
    alignas(kCacheLine) std::atomic<std::uint32_t> sleepers{0};
    std::atomic<bool> stopping{false};
};

void enqueue(thread_pool_impl &pool, pool_task *const task) noexcept { pool.enqueue(task); }

} // namespace lexec::detail

namespace lexec {

static_thread_pool::static_thread_pool(std::uint32_t const thread_count)
    : impl(std::make_unique<detail::thread_pool_impl>(thread_count == 0 ? 1 : thread_count)) {}

static_thread_pool::~static_thread_pool() = default;

std::uint32_t static_thread_pool::available_parallelism() const noexcept { return impl->count; }

std::uint32_t static_thread_pool::default_thread_count() noexcept {
    auto const hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 1 : hardware;
}

} // namespace lexec
