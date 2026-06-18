#pragma once

// =============================================================================
// log_monitor.hpp — Producer-consumer orchestrator
//
// Owns all pipeline components: ChunkPool, ThreadSafeQueue, PatternMatcher,
// LogReader, and the thread pool. Spawns a producer thread that reads the
// log file through the pool-integrated pipeline, and N consumer threads
// that run Aho-Corasick pattern matching on each chunk.
//
// Lifecycle:
//   LogMonitor monitor(config);
//   monitor.start();   // Non-blocking: spawns all threads
//   // ... do other work or wait ...
//   monitor.stop();    // Blocks until all threads complete
// =============================================================================

#include "chunk_pool.hpp"
#include "log_reader.hpp"
#include "pattern_matcher.hpp"
#include "thread_safe_queue.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace logmonitor {

class LogMonitor {
public:
    // =========================================================================
    // Configuration
    // =========================================================================
    struct Config {
        std::filesystem::path      log_path;           // Path to log file
        std::vector<std::string>   patterns;           // Patterns for Aho-Corasick
        AlertCallback              on_match;           // Callback invoked per match

        std::size_t consumer_threads   = 0;            // 0 = auto-detect (hw_concurrency - 1)
        std::size_t queue_capacity     = 1024;         // Bounded queue size
        std::size_t chunk_buffer_size  = 65536;        // Buffer capacity per chunk (64 KB)
        bool        tail_mode          = false;        // true = tail -f mode
    };

    // =========================================================================
    // Construction / Destruction
    // =========================================================================
    //
    // Constructs all internal components (pool, queue, matcher, reader) but
    // does NOT start any threads. Call start() to begin processing.
    //
    explicit LogMonitor(Config config);

    // Destructor calls stop() if the monitor is still running.
    ~LogMonitor();

    // Non-copyable, non-movable (owns threads)
    LogMonitor(const LogMonitor&)            = delete;
    LogMonitor& operator=(const LogMonitor&) = delete;
    LogMonitor(LogMonitor&&)                 = delete;
    LogMonitor& operator=(LogMonitor&&)      = delete;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    // Spawns the producer thread and consumer thread pool. Non-blocking.
    // Throws std::runtime_error if already running.
    void start();

    // Signals shutdown, waits for all threads to complete. Blocking.
    // Idempotent — safe to call multiple times.
    void stop();

    // Returns true if threads are running.
    [[nodiscard]] bool is_running() const noexcept;

    // =========================================================================
    // Runtime statistics
    // =========================================================================
    struct Stats {
        std::uint64_t chunks_processed{0};    // Total chunks consumed
        std::uint64_t matches_found{0};       // Total pattern matches
        std::size_t   bytes_read{0};          // Total bytes read from file
        std::uint64_t pool_acquisitions{0};   // Total pool.acquire() calls
        double        elapsed_seconds{0.0};   // Wall-clock time since start()
        double        throughput_mbps{0.0};   // MB/s read throughput
    };

    // Returns a snapshot of current statistics. Thread-safe.
    [[nodiscard]] Stats stats() const;

private:
    // =========================================================================
    // Thread entry points
    // =========================================================================

    // Producer: reads file → acquires chunks from pool → pushes to queue
    void producer_loop();

    // Consumer: pops chunks from queue → runs Aho-Corasick → invokes callback
    void consumer_loop();

    // =========================================================================
    // Configuration and owned components
    // =========================================================================
    Config                           config_;
    std::unique_ptr<ChunkPool>       pool_;
    std::unique_ptr<ThreadSafeQueue<PooledChunk>> queue_;
    std::unique_ptr<PatternMatcher>  matcher_;
    std::unique_ptr<LogReader>       reader_;

    // =========================================================================
    // Threading
    // =========================================================================
    std::thread              producer_thread_;
    std::vector<std::thread> consumer_threads_;
    std::atomic<bool>        running_{false};

    // =========================================================================
    // Statistics (updated atomically by consumer threads)
    // =========================================================================
    std::atomic<std::uint64_t> stat_chunks_processed_{0};
    std::atomic<std::uint64_t> stat_matches_found_{0};
    std::atomic<std::uint64_t> stat_pool_acquisitions_{0};

    using Clock = std::chrono::steady_clock;
    Clock::time_point start_time_;
};

} // namespace logmonitor
