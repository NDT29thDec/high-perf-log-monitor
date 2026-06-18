// =============================================================================
// log_reader.cpp — High-speed file I/O for log ingestion
//
// Implements the Chunked read strategy: reads the file in buffer-sized
// segments, splitting at newline boundaries so each LogChunk contains
// only complete lines. Partial lines at buffer boundaries are carried
// over to the next read cycle.
//
// Pool-integrated: acquires LogChunks from ChunkPool, writes directly
// into the pre-allocated buffer, and pushes PooledChunks to the queue.
// =============================================================================

#include "logmonitor/log_reader.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace logmonitor {

// =============================================================================
// Construction
// =============================================================================

LogReader::LogReader(std::filesystem::path filepath,
                     ReadStrategy           strategy,
                     std::size_t            buffer_size)
    : filepath_{std::move(filepath)}
    , strategy_{strategy}
    , buffer_size_{buffer_size}
{}

// =============================================================================
// read_all() — Batch mode: read entire file
// =============================================================================

void LogReader::read_all(ChunkPool&                   pool,
                         ThreadSafeQueue<PooledChunk>& output_queue)
{
    std::ifstream file{filepath_, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error(
            "LogReader: cannot open file: " + filepath_.string());
    }

    // Leftover bytes from the previous chunk that ended mid-line.
    std::string carry_over;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // 1. Acquire a pre-allocated chunk from the pool
        auto chunk = pool.acquire();
        if (!chunk) {
            break; // Pool shut down
        }

        // 2. Prepare the buffer: prepend any carry-over from previous iteration
        auto& buf = chunk->data;
        std::size_t carry_len = carry_over.size();

        // Resize to fit carry-over + new data (within reserved capacity → no realloc)
        buf.resize(carry_len + buffer_size_);

        // Copy carry-over to the front
        if (carry_len > 0) {
            std::memcpy(buf.data(), carry_over.data(), carry_len);
            carry_over.clear();
        }

        // 3. Read from file into the buffer after the carry-over
        file.read(buf.data() + static_cast<std::ptrdiff_t>(carry_len),
                  static_cast<std::streamsize>(buffer_size_));
        auto bytes_read = static_cast<std::size_t>(file.gcount());

        if (bytes_read == 0 && carry_len == 0) {
            // EOF with no leftover data — we're done.
            // Chunk goes out of scope → returned to pool automatically.
            break;
        }

        std::size_t total = carry_len + bytes_read;
        buf.resize(total);

        // 4. Split at the last newline to ensure complete lines only.
        //    Any partial line at the end becomes the carry-over for the next chunk.
        std::size_t split_pos = total;
        if (!file.eof()) {
            // Not EOF: find the last newline
            auto last_nl = buf.rfind('\n', total - 1);
            if (last_nl != std::string::npos) {
                split_pos = last_nl + 1; // Include the newline in this chunk
                carry_over = buf.substr(split_pos);
            }
            // If no newline found, the entire buffer is one huge line.
            // Send it as-is (the next chunk will continue the line).
        }

        // 5. Finalize chunk metadata
        chunk->data_size   = split_pos;
        buf.resize(split_pos);
        chunk->chunk_id    = chunk_counter_.fetch_add(1, std::memory_order_relaxed);
        chunk->byte_offset = total_bytes_read_.load(std::memory_order_relaxed);

        total_bytes_read_.fetch_add(split_pos, std::memory_order_relaxed);

        // 6. Push to the queue
        if (!output_queue.push(std::move(chunk))) {
            break; // Queue shut down
        }

        // If EOF, handle remaining carry-over
        if (file.eof() && !carry_over.empty()) {
            auto tail_chunk = pool.acquire();
            if (!tail_chunk) break;

            tail_chunk->data      = std::move(carry_over);
            tail_chunk->data_size = tail_chunk->data.size();
            tail_chunk->chunk_id  = chunk_counter_.fetch_add(1, std::memory_order_relaxed);
            tail_chunk->byte_offset = total_bytes_read_.load(std::memory_order_relaxed);
            total_bytes_read_.fetch_add(tail_chunk->data_size, std::memory_order_relaxed);

            output_queue.push(std::move(tail_chunk));
            carry_over.clear();
            break;
        }
    }
}

// =============================================================================
// tail() — Real-time mode: watch for file appends
// =============================================================================

void LogReader::tail(ChunkPool&                   pool,
                     ThreadSafeQueue<PooledChunk>& output_queue)
{
    std::ifstream file{filepath_, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error(
            "LogReader: cannot open file: " + filepath_.string());
    }

    // Start from the end of the file
    file.seekg(0, std::ios::end);

    std::string carry_over;
    constexpr auto poll_interval = std::chrono::milliseconds(100);

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        auto pos_before = file.tellg();

        // Try to read new data
        auto chunk = pool.acquire();
        if (!chunk) break;

        auto& buf = chunk->data;
        std::size_t carry_len = carry_over.size();
        buf.resize(carry_len + buffer_size_);

        if (carry_len > 0) {
            std::memcpy(buf.data(), carry_over.data(), carry_len);
            carry_over.clear();
        }

        file.read(buf.data() + static_cast<std::ptrdiff_t>(carry_len),
                  static_cast<std::streamsize>(buffer_size_));
        auto bytes_read = static_cast<std::size_t>(file.gcount());

        if (bytes_read == 0 && carry_len == 0) {
            // No new data — wait and retry.
            // Clear EOF flag so we can read again after new data is appended.
            file.clear();
            file.seekg(pos_before);
            // chunk goes out of scope → returned to pool
            std::this_thread::sleep_for(poll_interval);
            continue;
        }

        std::size_t total = carry_len + bytes_read;
        buf.resize(total);

        // Split at last newline
        std::size_t split_pos = total;
        auto last_nl = buf.rfind('\n', total - 1);
        if (last_nl != std::string::npos) {
            split_pos = last_nl + 1;
            carry_over = buf.substr(split_pos);
        }

        chunk->data_size   = split_pos;
        buf.resize(split_pos);
        chunk->chunk_id    = chunk_counter_.fetch_add(1, std::memory_order_relaxed);
        chunk->byte_offset = total_bytes_read_.load(std::memory_order_relaxed);
        total_bytes_read_.fetch_add(split_pos, std::memory_order_relaxed);

        if (split_pos > 0) {
            if (!output_queue.push(std::move(chunk))) {
                break;
            }
        }

        // Clear EOF for next iteration
        if (file.eof()) {
            file.clear();
        }
    }
}

// =============================================================================
// stop()
// =============================================================================

void LogReader::stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
}

// =============================================================================
// Observers
// =============================================================================

std::size_t LogReader::bytes_read() const noexcept {
    return total_bytes_read_.load(std::memory_order_relaxed);
}

std::uint64_t LogReader::chunks_produced() const noexcept {
    return chunk_counter_.load(std::memory_order_relaxed);
}

const std::filesystem::path& LogReader::filepath() const noexcept {
    return filepath_;
}

} // namespace logmonitor
