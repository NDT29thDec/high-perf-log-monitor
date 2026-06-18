#pragma once

// =============================================================================
// chunk_pool.hpp — Thread-safe object pool for LogChunk recycling
//
// Eliminates per-chunk heap allocation by pre-allocating a fixed pool of
// LogChunk objects. The producer acquires chunks, fills them, pushes to the
// queue. When consumers are done, the PooledChunk's custom deleter returns
// the chunk to the pool automatically.
//
// Header-only implementation.
// =============================================================================

#include "types.hpp"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace logmonitor {

class ChunkPool {
public:
    // =========================================================================
    // Construction
    // =========================================================================
    //
    // Pre-allocates `pool_size` LogChunk objects, each with a string buffer
    // whose capacity is reserved to `buffer_capacity` bytes.
    //
    // Recommended pool_size = queue_capacity + consumer_threads + 1
    //
    explicit ChunkPool(std::size_t pool_size,
                       std::size_t buffer_capacity = 65536)
        : pool_size_{pool_size}
        , buffer_capacity_{buffer_capacity}
        , shutdown_{false}
    {
        assert(pool_size > 0 && "ChunkPool: pool_size must be > 0");

        // Pre-allocate all chunks with reserved buffer capacity.
        // Using a vector of unique_ptr to ensure stable addresses —
        // pointers pushed to free_list_ remain valid even if storage_ grows.
        storage_.reserve(pool_size);
        free_list_.reserve(pool_size);

        for (std::size_t i = 0; i < pool_size; ++i) {
            auto chunk = std::make_unique<LogChunk>();
            chunk->data.reserve(buffer_capacity);
            free_list_.push_back(chunk.get());
            storage_.push_back(std::move(chunk));
        }
    }

    // Non-copyable, non-movable (chunks hold raw pointers back to this pool)
    ChunkPool(const ChunkPool&)            = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
    ChunkPool(ChunkPool&&)                 = delete;
    ChunkPool& operator=(ChunkPool&&)      = delete;

    ~ChunkPool() {
        shutdown();
    }

    // =========================================================================
    // acquire() — Get a recycled chunk from the pool
    // =========================================================================
    //
    // Blocks if the pool is exhausted (all chunks are in-flight). This acts
    // as natural backpressure — the producer slows down when consumers can't
    // keep up, preventing unbounded memory growth.
    //
    // Returns a PooledChunk (unique_ptr with custom deleter). When it goes
    // out of scope, the chunk is automatically returned to this pool.
    //
    // Returns a PooledChunk with nullptr if the pool has been shut down.
    //
    [[nodiscard]] PooledChunk acquire() {
        std::unique_lock lock{mutex_};
        not_empty_cv_.wait(lock, [this] {
            return !free_list_.empty() || shutdown_.load(std::memory_order_relaxed);
        });

        if (shutdown_.load(std::memory_order_relaxed)) {
            return PooledChunk{nullptr, ChunkDeleter{nullptr}};
        }

        LogChunk* chunk = free_list_.back();
        free_list_.pop_back();

        // Reset chunk metadata for reuse (buffer memory is preserved)
        chunk->reset();

        return PooledChunk{chunk, ChunkDeleter{this}};
    }

    // =========================================================================
    // release() — Return a chunk to the pool (called by ChunkDeleter)
    // =========================================================================
    //
    // This is NOT meant to be called directly by user code. It is invoked
    // automatically when a PooledChunk goes out of scope.
    //
    void release(LogChunk* chunk) {
        if (!chunk) return;

        {
            std::lock_guard lock{mutex_};
            free_list_.push_back(chunk);
        }
        not_empty_cv_.notify_one();
    }

    // =========================================================================
    // shutdown() — Unblock all waiters for graceful teardown
    // =========================================================================
    void shutdown() {
        shutdown_.store(true, std::memory_order_release);
        not_empty_cv_.notify_all();
    }

    // =========================================================================
    // Observers
    // =========================================================================

    [[nodiscard]] std::size_t pool_size() const noexcept {
        return pool_size_;
    }

    [[nodiscard]] std::size_t buffer_capacity() const noexcept {
        return buffer_capacity_;
    }

    [[nodiscard]] std::size_t available() const {
        std::lock_guard lock{mutex_};
        return free_list_.size();
    }

    [[nodiscard]] bool is_shutdown() const noexcept {
        return shutdown_.load(std::memory_order_acquire);
    }

private:
    std::size_t pool_size_;
    std::size_t buffer_capacity_;

    // Stable storage — chunks live here for the lifetime of the pool.
    std::vector<std::unique_ptr<LogChunk>> storage_;

    // Free list of available chunks (raw pointers into storage_).
    std::vector<LogChunk*> free_list_;

    mutable std::mutex      mutex_;
    std::condition_variable  not_empty_cv_;
    std::atomic<bool>        shutdown_;
};

// =============================================================================
// ChunkDeleter::operator() implementation
// =============================================================================
//
// Defined here (after ChunkPool is complete) to resolve the forward
// declaration in types.hpp. Returns the chunk to the pool if a pool
// is associated; otherwise performs a default delete (for standalone chunks).
//
inline void ChunkDeleter::operator()(LogChunk* chunk) const {
    if (!chunk) return;

    if (pool) {
        pool->release(chunk);
    } else {
        // Fallback: standalone chunk not managed by a pool.
        delete chunk;
    }
}

} // namespace logmonitor
