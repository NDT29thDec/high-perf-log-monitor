#pragma once

// =============================================================================
// types.hpp — Shared type definitions for the Real-time Log Monitor
//
// Defines the core data structures that flow through the pipeline:
//   LogChunk   — unit of work with a recyclable buffer
//   PooledChunk — unique_ptr with custom deleter for pool recycling
//   MatchResult — output of pattern matching
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace logmonitor {

// Forward declaration — ChunkPool must be defined before PooledChunk deleter
class ChunkPool;

// =============================================================================
// LogChunk — The unit of data flowing through the producer-consumer pipeline
// =============================================================================
//
// Design for pool recycling:
//   - `data` is a std::string whose capacity is reserved ONCE at pool creation
//   - The producer fills it via resize() (no realloc if within capacity)
//   - The consumer reads it via view()
//   - When recycled, data_size is reset but the buffer is NOT deallocated
//
struct LogChunk {
    std::string   data;         // Pre-allocated buffer (capacity managed by pool)
    std::size_t   data_size{0}; // Valid bytes in `data` (may differ from data.size())
    std::uint64_t chunk_id{0};  // Monotonically increasing chunk identifier
    std::size_t   byte_offset{0}; // Byte offset of this chunk within the source file
    bool          is_poison{false}; // Poison pill sentinel for graceful shutdown

    // Returns a read-only view of the valid portion of the buffer.
    [[nodiscard]] std::string_view view() const noexcept {
        return {data.data(), data_size};
    }

    // Resets the chunk metadata for reuse. Does NOT release the buffer memory.
    void reset() noexcept {
        data_size   = 0;
        chunk_id    = 0;
        byte_offset = 0;
        is_poison   = false;
        // Note: data.clear() would set size to 0 but preserve capacity.
        // We leave data untouched — the producer will overwrite via resize().
    }

    // Factory for creating a poison pill chunk (signals consumer shutdown).
    [[nodiscard]] static LogChunk make_poison() noexcept {
        LogChunk chunk;
        chunk.is_poison = true;
        return chunk;
    }
};

// =============================================================================
// ChunkDeleter — Custom deleter that returns LogChunk to the pool
// =============================================================================
//
// When a PooledChunk (unique_ptr) goes out of scope, instead of calling
// `delete`, this deleter calls pool->release() to recycle the chunk.
// If the pool pointer is null (e.g., for standalone chunks), it falls back
// to default delete.
//
struct ChunkDeleter {
    ChunkPool* pool{nullptr};

    void operator()(LogChunk* chunk) const;
    // Implementation is in chunk_pool.hpp (after ChunkPool is fully defined)
    // to avoid circular dependency.
};

// =============================================================================
// PooledChunk — The type that flows through ThreadSafeQueue
// =============================================================================
//
// A unique_ptr<LogChunk> with a custom deleter that returns the chunk to the
// pool when it goes out of scope. This is the primary handle consumers hold.
//
//   auto chunk = pool.acquire();        // Get a recycled chunk
//   chunk->data.resize(bytes_read);     // Producer fills it
//   queue.push(std::move(chunk));       // Transfer to consumer
//   // ... consumer pops and processes ...
//   // chunk goes out of scope → deleter returns it to pool
//
using PooledChunk = std::unique_ptr<LogChunk, ChunkDeleter>;

// =============================================================================
// MatchResult — Output of the PatternMatcher (Aho-Corasick)
// =============================================================================
//
// Emitted for every pattern occurrence found in a LogChunk.
//
struct MatchResult {
    std::size_t   pattern_index;    // Index into the pattern list
    std::string   pattern;          // The matched pattern string
    std::size_t   position;         // Byte offset of match within the chunk
    std::string   line_content;     // The full line containing the match
    std::uint64_t chunk_id{0};      // Chunk that contained this match
    std::size_t   byte_offset{0};   // Absolute byte offset in source file
};

// =============================================================================
// AlertCallback — User-provided handler invoked on each match
// =============================================================================
using AlertCallback = std::function<void(const MatchResult&)>;

} // namespace logmonitor
