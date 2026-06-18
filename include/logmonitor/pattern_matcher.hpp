#pragma once

// =============================================================================
// pattern_matcher.hpp — Aho-Corasick multi-pattern matching engine
//
// Builds a finite-state automaton from all patterns at construction time.
// The search() method scans input text in a SINGLE PASS, emitting all
// matches for all patterns simultaneously.
//
// Complexity:
//   Build:  O(M * K)  where M = total pattern length, K = alphabet size (128)
//   Search: O(N + Z)  where N = text length, Z = number of matches
//   Total:  O(N + M + Z) — optimal for multi-pattern matching
//
// Thread safety:
//   The automaton is IMMUTABLE after construction. Multiple consumer threads
//   can call search() concurrently with zero synchronization overhead.
//
// Memory layout:
//   Uses a flat std::vector<TrieNode> instead of heap-allocated tree nodes
//   for cache-friendly traversal. Each node's goto table is a fixed-size
//   std::array<int, 128> for ASCII — this trades ~512 bytes/node for O(1)
//   transitions (no hash map lookups in the hot loop).
// =============================================================================

#include "types.hpp"

#include <array>
#include <cstddef>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace logmonitor {

class PatternMatcher {
public:
    // =========================================================================
    // Construction — builds the full Aho-Corasick automaton
    // =========================================================================
    //
    // Accepts a list of patterns to search for. The automaton is built
    // immediately and is immutable for the lifetime of the object.
    //
    // Throws std::invalid_argument if patterns is empty.
    //
    explicit PatternMatcher(std::vector<std::string> patterns);

    // =========================================================================
    // search() — Single-pass multi-pattern scan
    // =========================================================================
    //
    // Scans `text` exactly once. For every character, the automaton transitions
    // to the next state via the goto table or failure links. Whenever a state
    // has a non-empty output list, MatchResults are emitted.
    //
    // Parameters:
    //   text     — the text to search (typically LogChunk::view())
    //   chunk_id — propagated into MatchResult for traceability
    //
    // Returns:
    //   A vector of MatchResult, one per occurrence of any registered pattern.
    //   Results are ordered by position in the text (left to right).
    //
    // Thread safety: SAFE to call from multiple threads concurrently.
    //
    [[nodiscard]] std::vector<MatchResult> search(
        std::string_view text,
        std::uint64_t    chunk_id    = 0,
        std::size_t      byte_offset = 0) const;

    // =========================================================================
    // Observers
    // =========================================================================

    // Returns the number of registered patterns.
    [[nodiscard]] std::size_t pattern_count() const noexcept;

    // Returns the number of nodes in the trie (automaton size).
    [[nodiscard]] std::size_t automaton_size() const noexcept;

    // Returns a const reference to the registered patterns.
    [[nodiscard]] const std::vector<std::string>& patterns() const noexcept;

private:
    // =========================================================================
    // TrieNode — A single node in the Aho-Corasick automaton
    // =========================================================================
    //
    // goto_table:   For each ASCII char c, goto_table[c] is the index of the
    //               child node, or -1 if no transition exists (before BFS).
    //               After build_automaton(), root-level -1s are replaced with
    //               0 (self-loop on root) so the search loop never needs to
    //               check for -1.
    //
    // failure_link: Index of the node representing the longest proper suffix
    //               of the current path that is also a prefix of some pattern.
    //               Computed via BFS during build_automaton().
    //
    // output:       List of pattern indices whose corresponding patterns end
    //               at this node. Includes both direct matches (patterns that
    //               terminate here) and dictionary suffix links (patterns that
    //               are suffixes of the path to this node).
    //
    struct TrieNode {
        std::array<int, 128> goto_table;
        int                  failure_link{0};
        std::vector<int>     output;

        TrieNode() {
            goto_table.fill(-1);
        }
    };

    // =========================================================================
    // Automaton construction
    // =========================================================================

    // Phase 1: Insert all patterns into the trie.
    void build_trie();

    // Phase 2: Compute failure links and merge output lists via BFS.
    void build_failure_links();

    // Convenience wrapper that calls build_trie() then build_failure_links().
    void build_automaton();

    // =========================================================================
    // Internal helper — extract the line containing a match position
    // =========================================================================
    //
    // Given text and a byte position, finds the enclosing newline-delimited
    // line and returns it as a string. Used to populate MatchResult::line_content.
    //
    [[nodiscard]] static std::string extract_line(std::string_view text,
                                                   std::size_t position);

    // =========================================================================
    // Data members
    // =========================================================================

    std::vector<TrieNode>    nodes_;     // Flat trie (node 0 = root)
    std::vector<std::string> patterns_;  // Original patterns (for MatchResult)
};

} // namespace logmonitor
