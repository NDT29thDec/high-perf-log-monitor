// =============================================================================
// pattern_matcher.cpp — Aho-Corasick multi-pattern matching implementation
//
// Build:   O(M * K)  — M = total pattern length, K = 128 (ASCII)
// Search:  O(N + Z)  — N = text length, Z = number of matches
// Total:   O(N + M + Z) per search call
// =============================================================================

#include "logmonitor/pattern_matcher.hpp"

#include <algorithm>
#include <cassert>
#include <queue>
#include <stdexcept>

namespace logmonitor {

// =============================================================================
// Construction
// =============================================================================

PatternMatcher::PatternMatcher(std::vector<std::string> patterns)
    : patterns_{std::move(patterns)}
{
    if (patterns_.empty()) {
        throw std::invalid_argument("PatternMatcher: patterns must not be empty");
    }

    build_automaton();
}

// =============================================================================
// build_automaton() — Master builder
// =============================================================================

void PatternMatcher::build_automaton() {
    // Start with a root node (index 0)
    nodes_.clear();
    nodes_.emplace_back(); // root

    build_trie();
    build_failure_links();
}

// =============================================================================
// build_trie() — Phase 1: Insert all patterns into the trie
// =============================================================================
//
// For each pattern, walk from the root and create nodes as needed.
// When a pattern terminates at a node, record its index in the output list.
//
void PatternMatcher::build_trie() {
    for (int pat_idx = 0; pat_idx < static_cast<int>(patterns_.size()); ++pat_idx) {
        const auto& pattern = patterns_[static_cast<std::size_t>(pat_idx)];
        int current = 0; // Start at root

        for (char ch : pattern) {
            // Clamp to ASCII range [0, 127] to avoid out-of-bounds access
            int c = static_cast<unsigned char>(ch);
            if (c >= 128) c = 0; // Non-ASCII → map to NUL (won't match anything useful)

            if (nodes_[static_cast<std::size_t>(current)].goto_table[static_cast<std::size_t>(c)] == -1) {
                // Create a new node
                nodes_[static_cast<std::size_t>(current)].goto_table[static_cast<std::size_t>(c)] =
                    static_cast<int>(nodes_.size());
                nodes_.emplace_back();
            }

            current = nodes_[static_cast<std::size_t>(current)].goto_table[static_cast<std::size_t>(c)];
        }

        // Mark this node as an output for this pattern
        nodes_[static_cast<std::size_t>(current)].output.push_back(pat_idx);
    }
}

// =============================================================================
// build_failure_links() — Phase 2: BFS to compute failure + output links
// =============================================================================
//
// The failure link of a node u points to the node representing the longest
// proper suffix of the string spelled by the path from root to u, that is
// also a prefix of some pattern in the trie.
//
// After computing failure links, we merge output lists: if node u's failure
// chain leads to a node with outputs, those outputs are also matches at u.
// This handles patterns that are suffixes of other paths (dictionary suffix links).
//
void PatternMatcher::build_failure_links() {
    std::queue<int> bfs_queue;

    // Initialize: all direct children of root get failure_link = 0 (root).
    // Also, set root's missing transitions to self-loop (0) so the search
    // loop never needs to check for -1 at the root level.
    for (int c = 0; c < 128; ++c) {
        int next = nodes_[0].goto_table[static_cast<std::size_t>(c)];
        if (next == -1) {
            // Root self-loop: if no transition from root on char c, stay at root
            nodes_[0].goto_table[static_cast<std::size_t>(c)] = 0;
        } else {
            // Direct child of root: failure link → root
            nodes_[static_cast<std::size_t>(next)].failure_link = 0;
            bfs_queue.push(next);
        }
    }

    // BFS: process nodes level by level
    while (!bfs_queue.empty()) {
        int u = bfs_queue.front();
        bfs_queue.pop();

        for (int c = 0; c < 128; ++c) {
            int v = nodes_[static_cast<std::size_t>(u)].goto_table[static_cast<std::size_t>(c)];

            if (v == -1) {
                // No direct transition: inherit from failure chain.
                // This "completes" the goto function so the search loop
                // can simply follow goto_table without ever hitting -1.
                nodes_[static_cast<std::size_t>(u)].goto_table[static_cast<std::size_t>(c)] =
                    nodes_[static_cast<std::size_t>(
                        nodes_[static_cast<std::size_t>(u)].failure_link
                    )].goto_table[static_cast<std::size_t>(c)];
            } else {
                // v exists: compute its failure link.
                // Follow u's failure chain to find the longest suffix match.
                int fail = nodes_[static_cast<std::size_t>(u)].failure_link;
                nodes_[static_cast<std::size_t>(v)].failure_link =
                    nodes_[static_cast<std::size_t>(fail)].goto_table[static_cast<std::size_t>(c)];

                // Merge output lists: v inherits all patterns from its failure node.
                // This implements "dictionary suffix links" — if v's failure chain
                // leads to a node that is the end of a pattern, that pattern also
                // matches at v's position.
                const auto& fail_output =
                    nodes_[static_cast<std::size_t>(
                        nodes_[static_cast<std::size_t>(v)].failure_link
                    )].output;

                if (!fail_output.empty()) {
                    nodes_[static_cast<std::size_t>(v)].output.insert(
                        nodes_[static_cast<std::size_t>(v)].output.end(),
                        fail_output.begin(),
                        fail_output.end()
                    );
                }

                bfs_queue.push(v);
            }
        }
    }
}

// =============================================================================
// search() — Single-pass multi-pattern scan
// =============================================================================

std::vector<MatchResult> PatternMatcher::search(
    std::string_view text,
    std::uint64_t    chunk_id,
    std::size_t      byte_offset) const
{
    std::vector<MatchResult> results;

    if (text.empty()) {
        return results;
    }

    int state = 0; // Start at root

    for (std::size_t i = 0; i < text.size(); ++i) {
        // Clamp to ASCII
        int c = static_cast<unsigned char>(text[i]);
        if (c >= 128) c = 0;

        // Transition — guaranteed to succeed because we filled in all gaps
        // during build_failure_links() (no -1 entries remain in goto_table).
        state = nodes_[static_cast<std::size_t>(state)].goto_table[static_cast<std::size_t>(c)];

        // Check outputs at this state
        const auto& output = nodes_[static_cast<std::size_t>(state)].output;
        if (!output.empty()) {
            for (int pat_idx : output) {
                auto idx = static_cast<std::size_t>(pat_idx);
                const auto& pattern = patterns_[idx];

                // Match position is (i - pattern.length() + 1) in the text
                std::size_t match_pos = i - pattern.size() + 1;

                MatchResult result;
                result.pattern_index = idx;
                result.pattern       = pattern;
                result.position      = match_pos;
                result.line_content  = extract_line(text, match_pos);
                result.chunk_id      = chunk_id;
                result.byte_offset   = byte_offset + match_pos;

                results.push_back(std::move(result));
            }
        }
    }

    return results;
}

// =============================================================================
// extract_line() — Find the enclosing newline-delimited line
// =============================================================================

std::string PatternMatcher::extract_line(std::string_view text, std::size_t position) {
    if (text.empty() || position >= text.size()) {
        return {};
    }

    // Find the start of the line (search backwards for '\n')
    std::size_t line_start = position;
    while (line_start > 0 && text[line_start - 1] != '\n') {
        --line_start;
    }

    // Find the end of the line (search forwards for '\n')
    std::size_t line_end = position;
    while (line_end < text.size() && text[line_end] != '\n') {
        ++line_end;
    }

    return std::string(text.substr(line_start, line_end - line_start));
}

// =============================================================================
// Observers
// =============================================================================

std::size_t PatternMatcher::pattern_count() const noexcept {
    return patterns_.size();
}

std::size_t PatternMatcher::automaton_size() const noexcept {
    return nodes_.size();
}

const std::vector<std::string>& PatternMatcher::patterns() const noexcept {
    return patterns_;
}

} // namespace logmonitor
