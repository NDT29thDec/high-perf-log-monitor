#pragma once

// =============================================================================
// thread_safe_queue.hpp — Bounded, blocking producer-consumer queue
//
// A thread-safe, bounded queue that provides backpressure when full and
// blocks consumers when empty. Designed for the producer-consumer pipeline
// where PooledChunk objects flow from LogReader to LogMonitor consumers.
//
// Header-only implementation.
//
// Key properties:
//   - Bounded capacity prevents unbounded memory growth (MLE prevention)
//   - push() blocks when full (backpressure on producer)
//   - pop() blocks when empty (consumers wait for work)
//   - shutdown() unblocks all waiters for graceful teardown
//   - All operations use move semantics (zero-copy for PooledChunk)
// =============================================================================

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <atomic>

namespace logmonitor {

template <typename T>
class ThreadSafeQueue {
public:
    // =========================================================================
    // Construction
    // =========================================================================
    explicit ThreadSafeQueue(std::size_t max_capacity = 1024)
        : max_capacity_{max_capacity}
        , shutdown_{false}
    {}

    // Non-copyable, non-movable
    ThreadSafeQueue(const ThreadSafeQueue&)            = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&)                 = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&)      = delete;

    ~ThreadSafeQueue() {
        shutdown();
    }

    // =========================================================================
    // push() — Enqueue an item (producer side)
    // =========================================================================
    //
    // Blocks if the queue is at max_capacity. Unblocks when space is available
    // or shutdown() is called.
    //
    // Returns true if the item was successfully enqueued.
    // Returns false if the queue has been shut down.
    //
    bool push(T item) {
        {
            std::unique_lock lock{mutex_};
            not_full_cv_.wait(lock, [this] {
                return queue_.size() < max_capacity_
                    || shutdown_.load(std::memory_order_relaxed);
            });

            if (shutdown_.load(std::memory_order_relaxed)) {
                return false;
            }

            queue_.push(std::move(item));
        }
        not_empty_cv_.notify_one();
        return true;
    }

    // =========================================================================
    // pop() — Dequeue an item (consumer side)
    // =========================================================================
    //
    // Blocks if the queue is empty. Unblocks when an item is available
    // or shutdown() is called.
    //
    // Returns the item wrapped in std::optional.
    // Returns std::nullopt if the queue has been shut down AND is empty.
    //
    [[nodiscard]] std::optional<T> pop() {
        std::unique_lock lock{mutex_};
        not_empty_cv_.wait(lock, [this] {
            return !queue_.empty()
                || shutdown_.load(std::memory_order_relaxed);
        });

        // Drain remaining items even after shutdown signal
        if (queue_.empty()) {
            return std::nullopt;
        }

        T item = std::move(queue_.front());
        queue_.pop();

        lock.unlock();
        not_full_cv_.notify_one();

        return item;
    }

    // =========================================================================
    // try_push() — Non-blocking enqueue attempt
    // =========================================================================
    //
    // Returns true if the item was enqueued, false if the queue is full
    // or shut down. Never blocks.
    //
    bool try_push(T item) {
        {
            std::lock_guard lock{mutex_};
            if (shutdown_.load(std::memory_order_relaxed)
                || queue_.size() >= max_capacity_) {
                return false;
            }
            queue_.push(std::move(item));
        }
        not_empty_cv_.notify_one();
        return true;
    }

    // =========================================================================
    // try_pop() — Non-blocking dequeue attempt
    // =========================================================================
    //
    // Returns the item if available, std::nullopt otherwise. Never blocks.
    //
    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard lock{mutex_};
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop();
        not_full_cv_.notify_one();
        return item;
    }

    // =========================================================================
    // shutdown() — Signal all waiters to unblock
    // =========================================================================
    //
    // After shutdown, push() returns false and pop() drains remaining items
    // then returns nullopt. Idempotent.
    //
    void shutdown() {
        shutdown_.store(true, std::memory_order_release);
        not_full_cv_.notify_all();
        not_empty_cv_.notify_all();
    }

    // =========================================================================
    // Observers
    // =========================================================================

    [[nodiscard]] bool is_shutdown() const noexcept {
        return shutdown_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock{mutex_};
        return queue_.size();
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock{mutex_};
        return queue_.empty();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return max_capacity_;
    }

private:
    std::size_t              max_capacity_;
    std::queue<T>            queue_;
    mutable std::mutex       mutex_;
    std::condition_variable  not_full_cv_;
    std::condition_variable  not_empty_cv_;
    std::atomic<bool>        shutdown_;
};

} // namespace logmonitor
