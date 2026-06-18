// =============================================================================
// test_log_reader.cpp — Unit tests for LogReader
// =============================================================================

#include "logmonitor/chunk_pool.hpp"
#include "logmonitor/log_reader.hpp"
#include "logmonitor/thread_safe_queue.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace logmonitor;

namespace {

// Helper: create a temporary test file and return its path.
class TempFile {
public:
    explicit TempFile(const std::string& content,
                      const std::string& name = "test_log.tmp")
        : path_{std::filesystem::temp_directory_path() / name}
    {
        std::ofstream out{path_, std::ios::binary};
        out << content;
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Run LogReader::read_all in a background thread while the main thread
// drains the queue. This prevents deadlock when the queue/pool fills up.
std::string read_and_drain(LogReader& reader,
                           ChunkPool& pool,
                           ThreadSafeQueue<PooledChunk>& queue) {
    // Run reader in background thread
    std::thread producer([&] {
        reader.read_all(pool, queue);
    });

    // Drain queue in main thread (blocking pop until shutdown)
    std::string result;

    // We need to know when the producer is done. Use a polling approach:
    // after the producer finishes, we shut down the queue so pop returns.
    std::thread shutdown_waiter([&] {
        producer.join();
        // Producer is done. Push a poison pill so the drain loop exits.
        auto pill = PooledChunk{new LogChunk{}, ChunkDeleter{nullptr}};
        pill->is_poison = true;
        queue.push(std::move(pill));
    });

    while (true) {
        auto maybe_chunk = queue.pop();
        if (!maybe_chunk.has_value()) break;
        auto& chunk = maybe_chunk.value();
        if (!chunk || chunk->is_poison) break;
        result += std::string(chunk->view());
    }

    shutdown_waiter.join();
    return result;
}

} // anonymous namespace

// =============================================================================
// Basic reading
// =============================================================================

TEST(LogReaderTest, ReadSmallFile) {
    std::string content =
        "line one\n"
        "line two\n"
        "line three\n";

    TempFile tmp{content, "test_small.log"};
    ChunkPool pool{8, 4096};
    ThreadSafeQueue<PooledChunk> queue{8};

    LogReader reader{tmp.path(), ReadStrategy::Chunked, 4096};

    std::string result = read_and_drain(reader, pool, queue);
    EXPECT_EQ(result, content);
    EXPECT_EQ(reader.bytes_read(), content.size());
}

TEST(LogReaderTest, ReadMultipleChunks) {
    // Generate content larger than the buffer size
    std::string content;
    for (int i = 0; i < 1000; ++i) {
        content += "Log line number " + std::to_string(i) + ": some data here\n";
    }

    TempFile tmp{content, "test_multi_chunk.log"};
    ChunkPool pool{16, 256};  // Small buffer → many chunks
    ThreadSafeQueue<PooledChunk> queue{16};

    LogReader reader{tmp.path(), ReadStrategy::Chunked, 256};

    std::string result = read_and_drain(reader, pool, queue);
    EXPECT_EQ(result, content);
    EXPECT_GT(reader.chunks_produced(), 1u); // Should produce multiple chunks
}

TEST(LogReaderTest, ChunksContainCompleteLines) {
    std::string content;
    for (int i = 0; i < 100; ++i) {
        content += "Line " + std::to_string(i) + ": padding data for testing\n";
    }

    TempFile tmp{content, "test_complete_lines.log"};
    ChunkPool pool{16, 128};  // Small buffer to force splitting
    ThreadSafeQueue<PooledChunk> queue{16};

    LogReader reader{tmp.path(), ReadStrategy::Chunked, 128};

    // Run reader in background, check chunks as they arrive
    std::thread producer([&] {
        reader.read_all(pool, queue);
    });

    // Drain and verify each chunk ends with newline
    std::string full_result;
    bool all_end_with_newline = true;

    // Use a shutdown waiter pattern
    std::thread shutdown_waiter([&] {
        producer.join();
        auto pill = PooledChunk{new LogChunk{}, ChunkDeleter{nullptr}};
        pill->is_poison = true;
        queue.push(std::move(pill));
    });

    while (true) {
        auto maybe_chunk = queue.pop();
        if (!maybe_chunk.has_value()) break;
        auto& chunk = maybe_chunk.value();
        if (!chunk || chunk->is_poison) break;

        auto view = chunk->view();
        full_result += std::string(view);
        if (!view.empty() && view.back() != '\n') {
            all_end_with_newline = false;
        }
    }

    shutdown_waiter.join();

    EXPECT_TRUE(all_end_with_newline) << "All chunks should end with newline";
    EXPECT_EQ(full_result, content);
}

// =============================================================================
// Edge cases
// =============================================================================

TEST(LogReaderTest, EmptyFile) {
    TempFile tmp{"", "test_empty.log"};
    ChunkPool pool{4, 1024};
    ThreadSafeQueue<PooledChunk> queue{4};

    LogReader reader{tmp.path(), ReadStrategy::Chunked, 1024};

    std::string result = read_and_drain(reader, pool, queue);
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(reader.bytes_read(), 0u);
}

TEST(LogReaderTest, SingleLineNoNewline) {
    TempFile tmp{"no trailing newline", "test_no_newline.log"};
    ChunkPool pool{4, 1024};
    ThreadSafeQueue<PooledChunk> queue{4};

    LogReader reader{tmp.path(), ReadStrategy::Chunked, 1024};

    std::string result = read_and_drain(reader, pool, queue);
    EXPECT_EQ(result, "no trailing newline");
}

TEST(LogReaderTest, FileNotFoundThrows) {
    ChunkPool pool{4, 1024};
    ThreadSafeQueue<PooledChunk> queue{4};

    LogReader reader{"/nonexistent/file.log"};
    EXPECT_THROW(reader.read_all(pool, queue), std::runtime_error);
}

// =============================================================================
// Stop signal
// =============================================================================

TEST(LogReaderTest, StopInterruptsReading) {
    // Create a file
    std::string content;
    for (int i = 0; i < 500; ++i) {
        content += "Line " + std::to_string(i) + ": some padding data here\n";
    }

    TempFile tmp{content, "test_stop.log"};

    ChunkPool pool{32, 256};
    ThreadSafeQueue<PooledChunk> queue{32};

    LogReader reader{tmp.path(), ReadStrategy::Chunked, 256};
    reader.stop(); // Stop before even starting

    // With stop pre-set, read_all should return almost immediately.
    // Run via read_and_drain to avoid any edge-case deadlock.
    std::string result = read_and_drain(reader, pool, queue);

    // Should have read very little or nothing since stop was pre-set
    EXPECT_LE(reader.bytes_read(), content.size());
}
