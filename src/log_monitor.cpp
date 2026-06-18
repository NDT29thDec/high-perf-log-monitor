// =============================================================================
// log_monitor.cpp — Producer-consumer orchestrator
//
// Owns all pipeline components and manages the thread lifecycle:
//   Producer thread  → LogReader fills chunks from pool → pushes to queue
//   Consumer threads → pop chunks → Aho-Corasick search → alert callback
//                    → chunk auto-recycled to pool on scope exit
// =============================================================================

#include "logmonitor/log_monitor.hpp"

#include <algorithm>
#include <stdexcept>

namespace logmonitor {

// =============================================================================
// Construction
// =============================================================================

LogMonitor::LogMonitor(Config config)
    : config_{std::move(config)}
{
    // Resolve auto-detect consumer threads
    if (config_.consumer_threads == 0) {
        auto hw = std::thread::hardware_concurrency();
        config_.consumer_threads = (hw > 1) ? (hw - 1) : 1;
    }

    // Pool size = queue capacity + consumer threads + 1 (producer's in-flight chunk)
    std::size_t pool_size = config_.queue_capacity
                          + config_.consumer_threads + 1;

    pool_    = std::make_unique<ChunkPool>(pool_size, config_.chunk_buffer_size);
    queue_   = std::make_unique<ThreadSafeQueue<PooledChunk>>(config_.queue_capacity);
    matcher_ = std::make_unique<PatternMatcher>(config_.patterns);
    reader_  = std::make_unique<LogReader>(config_.log_path,
                                           ReadStrategy::Chunked,
                                           config_.chunk_buffer_size);
}

LogMonitor::~LogMonitor() {
    stop();
}

// =============================================================================
// start() — Spawn all threads
// =============================================================================

void LogMonitor::start() {
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("LogMonitor: already running");
    }

    running_.store(true, std::memory_order_release);
    start_time_ = Clock::now();

    // Reset stats
    stat_chunks_processed_.store(0, std::memory_order_relaxed);
    stat_matches_found_.store(0, std::memory_order_relaxed);
    stat_pool_acquisitions_.store(0, std::memory_order_relaxed);

    // Spawn consumer threads first (so they're ready to process)
    consumer_threads_.reserve(config_.consumer_threads);
    for (std::size_t i = 0; i < config_.consumer_threads; ++i) {
        consumer_threads_.emplace_back(&LogMonitor::consumer_loop, this);
    }

    // Spawn producer thread
    producer_thread_ = std::thread(&LogMonitor::producer_loop, this);
}

// =============================================================================
// stop() — Graceful shutdown
// =============================================================================

void LogMonitor::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return; // Already stopped or never started
    }

    // 1. Tell the reader to stop producing
    reader_->stop();

    // 2. Shut down the pool (unblocks producer if blocked on acquire)
    pool_->shutdown();

    // 3. Wait for producer to finish
    if (producer_thread_.joinable()) {
        producer_thread_.join();
    }

    // 4. Push poison pills for each consumer (using standalone chunks, not from pool)
    for (std::size_t i = 0; i < consumer_threads_.size(); ++i) {
        auto poison = PooledChunk{new LogChunk{}, ChunkDeleter{nullptr}};
        poison->is_poison = true;
        queue_->push(std::move(poison));
    }

    // 5. Shut down the queue (unblocks consumers if blocked on pop)
    queue_->shutdown();

    // 6. Wait for all consumers to finish
    for (auto& t : consumer_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    consumer_threads_.clear();
}

// =============================================================================
// producer_loop() — Reads file, fills chunks, pushes to queue
// =============================================================================

void LogMonitor::producer_loop() {
    try {
        if (config_.tail_mode) {
            reader_->tail(*pool_, *queue_);
        } else {
            reader_->read_all(*pool_, *queue_);
        }
    } catch (const std::exception& /*e*/) {
        // Log or handle error — for now, silently stop.
        // In production, this would go to a logging framework.
    }

    // Producer is done. Poison pills are sent by stop().
}

// =============================================================================
// consumer_loop() — Pops chunks, runs Aho-Corasick, invokes callback
// =============================================================================

void LogMonitor::consumer_loop() {
    while (true) {
        auto maybe_chunk = queue_->pop();

        if (!maybe_chunk.has_value()) {
            // Queue shut down and drained
            break;
        }

        auto& chunk = maybe_chunk.value();

        if (!chunk || chunk->is_poison) {
            // Poison pill received — this consumer exits.
            // The PooledChunk with nullptr pool deleter will just delete the pill.
            break;
        }

        stat_pool_acquisitions_.fetch_add(1, std::memory_order_relaxed);

        // Run Aho-Corasick on this chunk
        auto matches = matcher_->search(
            chunk->view(),
            chunk->chunk_id,
            chunk->byte_offset
        );

        // Invoke callback for each match
        if (config_.on_match) {
            for (const auto& match : matches) {
                config_.on_match(match);
                stat_matches_found_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            stat_matches_found_.fetch_add(matches.size(), std::memory_order_relaxed);
        }

        stat_chunks_processed_.fetch_add(1, std::memory_order_relaxed);

        // chunk (PooledChunk) goes out of scope here → custom deleter
        // returns the LogChunk back to the ChunkPool for recycling.
    }
}

// =============================================================================
// Observers
// =============================================================================

bool LogMonitor::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

LogMonitor::Stats LogMonitor::stats() const {
    Stats s;
    s.chunks_processed  = stat_chunks_processed_.load(std::memory_order_relaxed);
    s.matches_found     = stat_matches_found_.load(std::memory_order_relaxed);
    s.bytes_read        = reader_->bytes_read();
    s.pool_acquisitions = stat_pool_acquisitions_.load(std::memory_order_relaxed);

    auto now = Clock::now();
    s.elapsed_seconds = std::chrono::duration<double>(now - start_time_).count();

    if (s.elapsed_seconds > 0.0) {
        s.throughput_mbps =
            static_cast<double>(s.bytes_read) / (1024.0 * 1024.0) / s.elapsed_seconds;
    } else {
        s.throughput_mbps = 0.0;
    }

    return s;
}

} // namespace logmonitor
