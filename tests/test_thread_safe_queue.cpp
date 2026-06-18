// =============================================================================
// test_thread_safe_queue.cpp — Unit tests for ThreadSafeQueue
// =============================================================================

#include "logmonitor/thread_safe_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace logmonitor;

// =============================================================================
// Basic operations
// =============================================================================

TEST(ThreadSafeQueueTest, PushPopSingleThread) {
    ThreadSafeQueue<int> queue{10};

    EXPECT_TRUE(queue.push(42));
    EXPECT_TRUE(queue.push(99));
    EXPECT_EQ(queue.size(), 2u);

    auto val1 = queue.pop();
    ASSERT_TRUE(val1.has_value());
    EXPECT_EQ(val1.value(), 42);

    auto val2 = queue.pop();
    ASSERT_TRUE(val2.has_value());
    EXPECT_EQ(val2.value(), 99);

    EXPECT_TRUE(queue.empty());
}

TEST(ThreadSafeQueueTest, TryPushPopNonBlocking) {
    ThreadSafeQueue<int> queue{2};

    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_FALSE(queue.try_push(3)); // Full

    auto v = queue.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), 1);

    EXPECT_TRUE(queue.try_push(3)); // Space now

    auto empty_q = ThreadSafeQueue<int>{5};
    EXPECT_FALSE(empty_q.try_pop().has_value());
}

TEST(ThreadSafeQueueTest, FIFOOrder) {
    ThreadSafeQueue<int> queue{100};
    constexpr int N = 50;

    for (int i = 0; i < N; ++i) {
        queue.push(i);
    }

    for (int i = 0; i < N; ++i) {
        auto val = queue.pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(val.value(), i);
    }
}

// =============================================================================
// Shutdown semantics
// =============================================================================

TEST(ThreadSafeQueueTest, ShutdownUnblocksPoppers) {
    ThreadSafeQueue<int> queue{10};
    std::atomic<bool> popped{false};

    std::thread consumer([&] {
        auto val = queue.pop(); // Should block then unblock
        if (!val.has_value()) {
            popped.store(true);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.shutdown();
    consumer.join();

    EXPECT_TRUE(popped.load());
}

TEST(ThreadSafeQueueTest, ShutdownDrainsRemainingItems) {
    ThreadSafeQueue<int> queue{10};
    queue.push(1);
    queue.push(2);
    queue.push(3);
    queue.shutdown();

    // Should still be able to pop existing items
    auto v1 = queue.pop();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(v1.value(), 1);

    auto v2 = queue.pop();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2.value(), 2);

    auto v3 = queue.pop();
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(v3.value(), 3);

    // Now empty + shutdown → nullopt
    auto v4 = queue.pop();
    EXPECT_FALSE(v4.has_value());
}

TEST(ThreadSafeQueueTest, PushReturnsFalseAfterShutdown) {
    ThreadSafeQueue<int> queue{10};
    queue.shutdown();

    EXPECT_FALSE(queue.push(42));
    EXPECT_TRUE(queue.is_shutdown());
}

// =============================================================================
// Concurrency
// =============================================================================

TEST(ThreadSafeQueueTest, MultiProducerMultiConsumer) {
    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 10000;
    constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

    ThreadSafeQueue<int> queue{256};
    std::atomic<int> total_consumed{0};

    // Producers
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, p] {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                queue.push(p * ITEMS_PER_PRODUCER + i);
            }
        });
    }

    // Consumers
    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&queue, &total_consumed] {
            while (true) {
                auto val = queue.pop();
                if (!val.has_value()) break;
                total_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Wait for producers to finish, then shut down
    for (auto& t : producers) t.join();

    // Give consumers time to drain
    while (queue.size() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    queue.shutdown();

    for (auto& t : consumers) t.join();

    EXPECT_EQ(total_consumed.load(), TOTAL_ITEMS);
}

TEST(ThreadSafeQueueTest, BoundedCapacityBlocksProducer) {
    ThreadSafeQueue<int> queue{3};

    queue.push(1);
    queue.push(2);
    queue.push(3);
    // Queue is now full

    std::atomic<bool> push_completed{false};

    std::thread producer([&] {
        queue.push(4); // Should block
        push_completed.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(push_completed.load()); // Still blocked

    [[maybe_unused]] auto _ = queue.pop(); // Free one slot
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(push_completed.load()); // Now unblocked

    producer.join();
}

// =============================================================================
// Move semantics
// =============================================================================

TEST(ThreadSafeQueueTest, MoveOnlyTypes) {
    ThreadSafeQueue<std::unique_ptr<int>> queue{10};

    auto ptr = std::make_unique<int>(42);
    queue.push(std::move(ptr));
    EXPECT_EQ(ptr, nullptr); // Moved from

    auto result = queue.pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result.value(), 42);
}
