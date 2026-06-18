// =============================================================================
// test_chunk_pool.cpp — Unit tests for ChunkPool
// =============================================================================

#include "logmonitor/chunk_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace logmonitor;

// =============================================================================
// Basic operations
// =============================================================================

TEST(ChunkPoolTest, AcquireReturnsValidChunk) {
    ChunkPool pool{4, 1024};

    auto chunk = pool.acquire();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->data_size, 0u);
    EXPECT_EQ(chunk->chunk_id, 0u);
    EXPECT_FALSE(chunk->is_poison);

    // Buffer should have reserved capacity
    EXPECT_GE(chunk->data.capacity(), 1024u);
}

TEST(ChunkPoolTest, AcquireReturnsDifferentChunks) {
    ChunkPool pool{4, 512};

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    auto c3 = pool.acquire();

    EXPECT_NE(c1.get(), c2.get());
    EXPECT_NE(c2.get(), c3.get());
    EXPECT_NE(c1.get(), c3.get());

    EXPECT_EQ(pool.available(), 1u); // 4 total, 3 acquired
}

TEST(ChunkPoolTest, ReleaseRecyclesChunk) {
    ChunkPool pool{2, 256};

    {
        auto chunk = pool.acquire();
        chunk->data = "test data";
        chunk->data_size = 9;
        chunk->chunk_id = 42;
        // chunk goes out of scope → returned to pool
    }

    EXPECT_EQ(pool.available(), 2u); // All chunks back in pool

    // Re-acquire — should get a recycled chunk
    auto recycled = pool.acquire();
    // Metadata should be reset
    EXPECT_EQ(recycled->data_size, 0u);
    EXPECT_EQ(recycled->chunk_id, 0u);
    // Buffer capacity should be preserved
    EXPECT_GE(recycled->data.capacity(), 256u);
}

TEST(ChunkPoolTest, PoolSizeAndAvailable) {
    ChunkPool pool{8, 128};

    EXPECT_EQ(pool.pool_size(), 8u);
    EXPECT_EQ(pool.available(), 8u);

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    EXPECT_EQ(pool.available(), 6u);

    c1.reset(); // Release back to pool
    EXPECT_EQ(pool.available(), 7u);
}

// =============================================================================
// Blocking behavior
// =============================================================================

TEST(ChunkPoolTest, AcquireBlocksWhenExhausted) {
    ChunkPool pool{2, 128};

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    // Pool is exhausted

    std::atomic<bool> acquired{false};
    std::thread blocked_thread([&] {
        auto c3 = pool.acquire();
        acquired.store(c3 != nullptr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(acquired.load()); // Should still be blocked

    c1.reset(); // Release one chunk
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(acquired.load()); // Now unblocked

    blocked_thread.join();
}

// =============================================================================
// Shutdown
// =============================================================================

TEST(ChunkPoolTest, ShutdownUnblocksAcquire) {
    ChunkPool pool{1, 128};

    auto c1 = pool.acquire(); // Exhaust the pool

    std::atomic<bool> got_null{false};
    std::thread blocked_thread([&] {
        auto c2 = pool.acquire(); // Should block, then return nullptr
        got_null.store(c2 == nullptr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pool.shutdown();
    blocked_thread.join();

    EXPECT_TRUE(got_null.load());
    EXPECT_TRUE(pool.is_shutdown());
}

// =============================================================================
// Concurrent acquire/release
// =============================================================================

TEST(ChunkPoolTest, ConcurrentAcquireRelease) {
    constexpr std::size_t POOL_SIZE = 16;
    constexpr int ITERATIONS = 10000;
    constexpr int NUM_THREADS = 8;

    ChunkPool pool{POOL_SIZE, 256};
    std::atomic<int> total_acquisitions{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < ITERATIONS; ++i) {
                auto chunk = pool.acquire();
                if (!chunk) break;

                // Simulate some work
                chunk->data.resize(100);
                chunk->data_size = 100;

                total_acquisitions.fetch_add(1, std::memory_order_relaxed);
                // chunk released automatically
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(total_acquisitions.load(), NUM_THREADS * ITERATIONS);
    EXPECT_EQ(pool.available(), POOL_SIZE); // All chunks returned
}

// =============================================================================
// No double-free
// =============================================================================

TEST(ChunkPoolTest, StandaloneChunkDeletesNormally) {
    // A PooledChunk with nullptr pool should use default delete
    auto standalone = PooledChunk{new LogChunk{}, ChunkDeleter{nullptr}};
    standalone->data = "standalone";
    standalone.reset(); // Should delete, not crash
}
