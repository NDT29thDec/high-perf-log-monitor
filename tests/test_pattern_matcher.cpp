// =============================================================================
// test_pattern_matcher.cpp — Unit tests for Aho-Corasick PatternMatcher
// =============================================================================

#include "logmonitor/pattern_matcher.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace logmonitor;

// =============================================================================
// Basic matching
// =============================================================================

TEST(PatternMatcherTest, SinglePatternSingleMatch) {
    PatternMatcher matcher{{"hello"}};

    auto results = matcher.search("say hello world");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].pattern, "hello");
    EXPECT_EQ(results[0].position, 4u);
    EXPECT_EQ(results[0].pattern_index, 0u);
}

TEST(PatternMatcherTest, SinglePatternMultipleMatches) {
    PatternMatcher matcher{{"ab"}};

    auto results = matcher.search("ababab");
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].position, 0u);
    EXPECT_EQ(results[1].position, 2u);
    EXPECT_EQ(results[2].position, 4u);
}

TEST(PatternMatcherTest, MultiplePatternsSinglePass) {
    PatternMatcher matcher{{"Failed password", "Invalid user", "Connection refused"}};

    std::string text =
        "sshd[1234]: Failed password for root from 1.2.3.4\n"
        "sshd[5678]: Invalid user hacker from 5.6.7.8\n"
        "sshd[9999]: Accepted publickey for admin\n";

    auto results = matcher.search(text);
    ASSERT_EQ(results.size(), 2u);

    // Results should be ordered by position
    EXPECT_EQ(results[0].pattern, "Failed password");
    EXPECT_EQ(results[1].pattern, "Invalid user");
}

TEST(PatternMatcherTest, NoMatches) {
    PatternMatcher matcher{{"ERROR", "FATAL"}};
    auto results = matcher.search("INFO: everything is fine");
    EXPECT_TRUE(results.empty());
}

TEST(PatternMatcherTest, EmptyText) {
    PatternMatcher matcher{{"test"}};
    auto results = matcher.search("");
    EXPECT_TRUE(results.empty());
}

// =============================================================================
// Overlapping patterns
// =============================================================================

TEST(PatternMatcherTest, OverlappingPatterns) {
    // "he", "her", "hers" all overlap at position 0 in "hers"
    PatternMatcher matcher{{"he", "her", "hers"}};

    auto results = matcher.search("hers");
    ASSERT_EQ(results.size(), 3u);

    // Sort by pattern length for deterministic checking
    std::sort(results.begin(), results.end(),
              [](const MatchResult& a, const MatchResult& b) {
                  return a.pattern.size() < b.pattern.size();
              });

    EXPECT_EQ(results[0].pattern, "he");
    EXPECT_EQ(results[1].pattern, "her");
    EXPECT_EQ(results[2].pattern, "hers");
}

TEST(PatternMatcherTest, PatternsAsSubstringsOfOthers) {
    // "sh", "she", "he", "her", "hers" — classic Aho-Corasick test case
    PatternMatcher matcher{{"sh", "she", "he", "her", "hers"}};

    auto results = matcher.search("ushers");

    // Expected matches:
    //   position 1: "sh"
    //   position 1: "she"
    //   position 2: "he"
    //   position 2: "her"
    //   position 2: "hers"
    ASSERT_EQ(results.size(), 5u);
}

// =============================================================================
// Edge cases
// =============================================================================

TEST(PatternMatcherTest, PatternIsEntireText) {
    PatternMatcher matcher{{"exact"}};
    auto results = matcher.search("exact");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].position, 0u);
}

TEST(PatternMatcherTest, SingleCharPatterns) {
    PatternMatcher matcher{{"a", "b"}};
    auto results = matcher.search("abba");
    ASSERT_EQ(results.size(), 4u);
}

TEST(PatternMatcherTest, RepeatedPattern) {
    PatternMatcher matcher{{"aa"}};
    auto results = matcher.search("aaaa");
    // "aa" at positions 0, 1, 2
    ASSERT_EQ(results.size(), 3u);
}

// =============================================================================
// Line extraction
// =============================================================================

TEST(PatternMatcherTest, LineContentExtraction) {
    PatternMatcher matcher{{"ERROR"}};
    std::string text =
        "line one: OK\n"
        "line two: ERROR here\n"
        "line three: OK\n";

    auto results = matcher.search(text);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].line_content, "line two: ERROR here");
}

TEST(PatternMatcherTest, LineContentMultipleMatches) {
    PatternMatcher matcher{{"WARN", "ERROR"}};
    std::string text =
        "2025-01-01 INFO: startup complete\n"
        "2025-01-01 WARN: disk usage high\n"
        "2025-01-01 ERROR: disk full\n";

    auto results = matcher.search(text);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_NE(results[0].line_content.find("WARN"), std::string::npos);
    EXPECT_NE(results[1].line_content.find("ERROR"), std::string::npos);
}

// =============================================================================
// Chunk metadata propagation
// =============================================================================

TEST(PatternMatcherTest, ChunkIdAndOffsetPropagated) {
    PatternMatcher matcher{{"test"}};
    auto results = matcher.search("this is a test", 42, 1000);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].chunk_id, 42u);
    EXPECT_EQ(results[0].byte_offset, 1010u); // 1000 + 10
}

// =============================================================================
// Observers
// =============================================================================

TEST(PatternMatcherTest, PatternCount) {
    PatternMatcher matcher{{"a", "bb", "ccc"}};
    EXPECT_EQ(matcher.pattern_count(), 3u);
}

TEST(PatternMatcherTest, AutomatonSize) {
    // "abc" → 4 nodes (root + a + b + c)
    PatternMatcher matcher{{"abc"}};
    EXPECT_EQ(matcher.automaton_size(), 4u);
}

TEST(PatternMatcherTest, AutomatonSizeWithSharedPrefixes) {
    // "abc" and "abd" share prefix "ab" → 5 nodes (root + a + b + c + d)
    PatternMatcher matcher{{"abc", "abd"}};
    EXPECT_EQ(matcher.automaton_size(), 5u);
}

// =============================================================================
// Construction errors
// =============================================================================

TEST(PatternMatcherTest, EmptyPatternsThrows) {
    EXPECT_THROW(PatternMatcher{{}}, std::invalid_argument);
}

// =============================================================================
// Large-scale test
// =============================================================================

TEST(PatternMatcherTest, ManyPatternsManyMatches) {
    std::vector<std::string> patterns;
    for (int i = 0; i < 100; ++i) {
        // Use brackets to prevent PAT[1] matching inside PAT[10]
        patterns.push_back("[PAT" + std::to_string(i) + "]");
    }

    PatternMatcher matcher{std::move(patterns)};

    // Build text with all patterns
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += "log line with [PAT" + std::to_string(i) + "] inside\n";
    }

    auto results = matcher.search(text);
    EXPECT_EQ(results.size(), 100u);
}
