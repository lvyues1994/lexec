#include <lexec/schedulers/static_thread_pool.hpp>

#include "bwos_queue.hpp"

#include <lexec/detail/spin_wait.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
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
constexpr std::uint32_t kSpinStealAttempts = 2;
// A bulk job is cut into this many chunks per worker, so that a worker that joins late
// or runs slow chunks leaves its share to the others.
constexpr std::size_t kChunksPerWorker = 4;

struct chunk_range {
    std::size_t begin;
    std::size_t end;
};

// The chunk-th of chunk_count near-equal parts of [0, size).
chunk_range chunk_bounds(std::size_t const size, std::size_t const chunk_count, std::size_t const chunk) noexcept {
    auto const quotient = size / chunk_count;
    auto const remainder = size % chunk_count;
    auto const begin = chunk * quotient + std::min(chunk, remainder);
    return {begin, begin + quotient + (chunk < remainder ? 1 : 0)};
}

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

    // A spinning worker steals from few victims per round, so that it gets back soon to
    // its own submissions and to bulk jobs.
    pool_task *find_task(std::uint32_t const index, std::uint32_t const steal_attempts) noexcept {
        auto &self = workers[index];
        if (auto *const task = self.local.pop_back()) {
            return task;
        }
        if (auto *const task = take_remote(self)) {
            return task;
        }
        for (auto attempt = std::uint32_t{0}; attempt < steal_attempts; ++attempt) {
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
    // Bulk jobs found meanwhile are joined here, and spinning starts over after them.
    pool_task *wait_for_task(std::uint32_t const index) noexcept {
        auto &self = workers[index];
        while (true) {
            for (auto round = 0; round < kSpinRounds; ++round) {
                for (auto pause = 0; pause < kPausesPerRound; ++pause) {
                    cpu_relax();
                }
                if (join_bulk(index)) {
                    round = 0;
                } else if (auto *const task = find_task(index, kSpinStealAttempts)) {
                    return task;
                }
            }
            while (true) {
                {
                    auto lock = std::unique_lock<std::mutex>{self.mutex};
                    if (stopping.load(std::memory_order_relaxed)) {
                        lock.unlock();
                        return find_task(index, 2 * count);
                    }
                    auto expected = worker_state::running;
                    // A submission since the worker last ran makes this fail: look again.
                    if (self.state.compare_exchange_strong(expected, worker_state::sleeping, std::memory_order_acq_rel)) {
                        if (auto *const task = take_remote(self)) {
                            self.state.store(worker_state::running, std::memory_order_relaxed);
                            return task;
                        }
                        // A job is listed under the bulk mutex before its publisher looks
                        // for sleepers: either this sees the job, or the publisher sees
                        // this worker counted and asleep.
                        sleepers.fetch_add(1, std::memory_order_relaxed);
                        if (not has_bulk_job()) {
                            self.wakeup.wait(lock, [&] {
                                return self.state.load(std::memory_order_acquire) != worker_state::sleeping or
                                       stopping.load(std::memory_order_relaxed);
                            });
                        }
                        sleepers.fetch_sub(1, std::memory_order_relaxed);
                    }
                    self.state.store(worker_state::running, std::memory_order_relaxed);
                }
                if (auto *const task = find_task(index, 2 * count)) {
                    return task;
                }
                if (join_bulk(index)) {
                    break;
                }
            }
        }
    }

    void run(std::uint32_t const index) noexcept {
        current_worker = current_worker_slot{this, index};
        auto &self = workers[index];
        for (auto executed = std::uint32_t{1};; ++executed) {
            if (join_bulk(index)) {
                continue;
            }
            auto *task = executed % kRemotePeriod == 0 ? take_remote(self) : nullptr;
            if (task == nullptr) {
                task = find_task(index, 2 * count);
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

    // Bulk jobs. The pool lists each job until all its chunks are claimed, and holds a
    // reference to it meanwhile; each participant holds another while it takes part.
    // Whoever drops the last reference completes the job, after which nothing touches it.

    // The oldest listed job that still has chunks to claim, with bulk_mutex held. A listed
    // job is alive, since the list holds a reference to it.
    pool_bulk_job *joinable_job() const noexcept {
        auto *job = bulk_head;
        while (job != nullptr and job->next_chunk.load(std::memory_order_relaxed) >= job->chunk_count) {
            job = job->next;
        }
        return job;
    }

    bool has_bulk_job() noexcept {
        auto const lock = std::lock_guard<std::mutex>{bulk_mutex};
        return joinable_job() != nullptr;
    }

    // Takes part in a job that still has chunks to claim; false if there is none.
    bool join_bulk(std::uint32_t const index) noexcept {
        if (bulk_front.load(std::memory_order_relaxed) == nullptr) {
            return false;
        }
        pool_bulk_job *job = nullptr;
        {
            auto const lock = std::lock_guard<std::mutex>{bulk_mutex};
            job = joinable_job();
            if (job == nullptr) {
                return false;
            }
            job->refs.fetch_add(1, std::memory_order_relaxed);
        }
        participate(*job, index);
        return true;
    }

    void run_bulk(pool_bulk_job &job) noexcept {
        auto const self = current_worker.pool == this ? current_worker.index : count;
        job.chunk_count = std::min(job.size, std::size_t{count} * kChunksPerWorker);
        job.next_chunk.store(0, std::memory_order_relaxed);
        if (count == 1 or job.chunk_count == 1) {
            job.run_chunk(&job, 0, job.size);
            job.complete(&job);
            return;
        }
        job.refs.store(2, std::memory_order_relaxed);
        {
            auto const lock = std::lock_guard<std::mutex>{bulk_mutex};
            job.next = nullptr;
            if (bulk_tail == nullptr) {
                bulk_head = &job;
            } else {
                bulk_tail->next = &job;
            }
            bulk_tail = &job;
            bulk_front.store(bulk_head, std::memory_order_relaxed);
        }
        participate(job, self);
    }

    // Claims chunks until none are left. Each participant that still sees two unclaimed
    // chunks wakes one more worker, so waking spreads over the participants.
    void participate(pool_bulk_job &job, std::uint32_t const self) noexcept {
        if (job.next_chunk.load(std::memory_order_relaxed) + 1 < job.chunk_count) {
            wake_idle(self);
        }
        auto dropped = std::uint32_t{1};
        while (true) {
            auto const chunk = job.next_chunk.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= job.chunk_count) {
                // Exactly one participant claims one past the last chunk; it unlists the
                // job and drops the pool's reference along with its own.
                if (chunk == job.chunk_count) {
                    unlist(job);
                    dropped = 2;
                }
                break;
            }
            auto const range = chunk_bounds(job.size, job.chunk_count, chunk);
            job.run_chunk(&job, range.begin, range.end);
        }
        // Every participant's chunks happen before the last one's completion.
        if (job.refs.fetch_sub(dropped, std::memory_order_acq_rel) == dropped) {
            job.complete(&job);
        }
    }

    void unlist(pool_bulk_job &job) noexcept {
        auto const lock = std::lock_guard<std::mutex>{bulk_mutex};
        pool_bulk_job *previous = nullptr;
        auto *current = bulk_head;
        while (current != &job) {
            previous = current;
            current = current->next;
        }
        (previous == nullptr ? bulk_head : previous->next) = job.next;
        if (bulk_tail == &job) {
            bulk_tail = previous;
        }
        bulk_front.store(bulk_head, std::memory_order_relaxed);
    }

    std::unique_ptr<worker[]> workers;
    std::uint32_t count;
    alignas(kCacheLine) std::atomic<std::uint32_t> sleepers{0};
    std::atomic<bool> stopping{false};
    // Read by every worker on every iteration, and written only when a job is listed or
    // unlisted, so it gets a line of its own.
    alignas(kCacheLine) std::atomic<pool_bulk_job *> bulk_front{nullptr};
    alignas(kCacheLine) std::mutex bulk_mutex;
    pool_bulk_job *bulk_head = nullptr;
    pool_bulk_job *bulk_tail = nullptr;
};

void enqueue(thread_pool_impl &pool, pool_task *const task) noexcept { pool.enqueue(task); }

void run_bulk(thread_pool_impl &pool, pool_bulk_job &job) noexcept { pool.run_bulk(job); }

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
