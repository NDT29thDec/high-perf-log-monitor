#pragma once

// =============================================================================
// log_reader.hpp — High-speed file I/O for log ingestion
//
// Reads log files using either memory-mapped I/O (mmap) or chunked buffered
// reads. Pool-integrated: acquires LogChunk objects from ChunkPool, fills
// the pre-allocated buffer, and pushes PooledChunks into the ThreadSafeQueue.
//
// Supports two modes:
//   - Batch mode (read_all): Reads the entire file from start to end.
//   - Tail mode (tail):      Watches for file growth and reads appended data.
// =============================================================================

#include "chunk_pool.hpp"
#include "thread_safe_queue.hpp"
#include "types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace logmonitor {

// Strategy for how the file is read into memory.
enum class ReadStrategy {
    Chunked,  // Portable buffered read() with configurable buffer size
    MMap      // Memory-mapped I/O (POSIX mmap / Win32 MapViewOfFile)
};

class LogReader {
public:
    // =========================================================================
    // Construction
    // =========================================================================
    //
    // Parameters:
    //   filepath    — Path to the log file to read.
    //   strategy    — I/O strategy (Chunked is portable, MMap is Linux-optimized).
    //   buffer_size — Read buffer size in bytes (for Chunked strategy).
    //                 Should match ChunkPool's buffer_capacity for zero-realloc.
    //
    explicit LogReader(std::filesystem::path filepath,
                       ReadStrategy strategy    = ReadStrategy::Chunked,
                       std::size_t  buffer_size = 65536);

    // Non-copyable, movable
    LogReader(const LogReader&)            = delete;
    LogReader& operator=(const LogReader&) = delete;
    LogReader(LogReader&&)                 = default;
    LogReader& operator=(LogReader&&)      = default;

    ~LogReader() = default;

    // =========================================================================
    // read_all() — Batch mode: read entire file
    // =========================================================================
    //
    // Reads the file from beginning to end. For each buffer-sized segment:
    //   1. Acquires a LogChunk from `pool`
    //   2. Reads data into the chunk's pre-allocated buffer
    //   3. Adjusts boundaries to split at newlines (no partial lines)
    //   4. Pushes the PooledChunk into `output_queue`
    //
    // Returns when the entire file has been read, or stop() was called.
    // Does NOT push a poison pill — the caller (LogMonitor) handles shutdown.
    //
    void read_all(ChunkPool&                     pool,
                  ThreadSafeQueue<PooledChunk>&   output_queue);

    // =========================================================================
    // tail() — Real-time mode: watch for file growth
    // =========================================================================
    //
    // Starts at the current end of file and watches for appended data.
    // Uses inotify (Linux) or periodic polling (portable fallback).
    // Runs indefinitely until stop() is called.
    //
    void tail(ChunkPool&                     pool,
              ThreadSafeQueue<PooledChunk>&   output_queue);

    // =========================================================================
    // stop() — Request graceful termination
    // =========================================================================
    //
    // Thread-safe. Can be called from any thread to interrupt read_all()
    // or tail(). The reader will finish the current chunk and return.
    //
    void stop() noexcept;

    // =========================================================================
    // Observers
    // =========================================================================

    // Total bytes read from the file so far.
    [[nodiscard]] std::size_t bytes_read() const noexcept;

    // Total chunks produced so far.
    [[nodiscard]] std::uint64_t chunks_produced() const noexcept;

    // The file path being read.
    [[nodiscard]] const std::filesystem::path& filepath() const noexcept;

private:
    std::filesystem::path  filepath_;
    ReadStrategy           strategy_;
    std::size_t            buffer_size_;

    std::atomic<bool>      stop_requested_{false};
    std::atomic<std::size_t>   total_bytes_read_{0};
    std::atomic<std::uint64_t> chunk_counter_{0};
};

} // namespace logmonitor
