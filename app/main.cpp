// =============================================================================
// main.cpp — CLI entry point for the Real-time Log Monitor
//
// Usage:
//   logmonitor <logfile> [pattern1] [pattern2] ... [--tail]
//
// Examples:
//   logmonitor /var/log/auth.log "Failed password" "Invalid user"
//   logmonitor massive_test.log "ERROR" "CRITICAL" "FATAL" --tail
// =============================================================================

#include "logmonitor/log_monitor.hpp"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex cout_mutex;

void print_usage(const char* progname) {
    std::cerr << "Usage: " << progname
              << " <logfile> <pattern1> [pattern2 ...] [--tail]\n\n"
              << "Options:\n"
              << "  --tail    Watch file for new data (like tail -f)\n"
              << "  --help    Show this help message\n\n"
              << "Example:\n"
              << "  " << progname
              << " /var/log/auth.log \"Failed password\" \"Invalid user\"\n";
}

void print_match(const logmonitor::MatchResult& match) {
    std::lock_guard lock{cout_mutex};
    std::cerr << "\033[1;31m[ALERT]\033[0m "
              << "Pattern \"\033[1;33m" << match.pattern << "\033[0m\" "
              << "found at chunk " << match.chunk_id
              << ", offset " << match.byte_offset << ":\n"
              << "  \033[0;36m" << match.line_content << "\033[0m\n";
}

void print_stats(const logmonitor::LogMonitor::Stats& stats) {
    std::cerr << "\n\033[1;32m=== Statistics ===\033[0m\n"
              << "  Chunks processed : " << stats.chunks_processed << "\n"
              << "  Matches found    : " << stats.matches_found << "\n"
              << "  Bytes read       : " << stats.bytes_read << "\n"
              << "  Pool acquisitions: " << stats.pool_acquisitions << "\n"
              << "  Elapsed time     : " << std::fixed << std::setprecision(3)
              << stats.elapsed_seconds << " s\n"
              << "  Throughput       : " << std::fixed << std::setprecision(2)
              << stats.throughput_mbps << " MB/s\n";
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Parse arguments
    std::string log_path = argv[1];
    std::vector<std::string> patterns;
    bool tail_mode = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--tail") {
            tail_mode = true;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            patterns.push_back(std::move(arg));
        }
    }

    if (patterns.empty()) {
        std::cerr << "Error: at least one pattern is required.\n";
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Print banner
    std::cerr << "\033[1;35m╔══════════════════════════════════════════╗\033[0m\n"
              << "\033[1;35m║   Real-time System Log Parser & Monitor  ║\033[0m\n"
              << "\033[1;35m╚══════════════════════════════════════════╝\033[0m\n\n"
              << "File     : " << log_path << "\n"
              << "Mode     : " << (tail_mode ? "tail (real-time)" : "batch") << "\n"
              << "Patterns : " << patterns.size() << "\n";

    for (std::size_t i = 0; i < patterns.size(); ++i) {
        std::cerr << "  [" << i << "] \"" << patterns[i] << "\"\n";
    }
    std::cerr << "\n";

    // Configure and run
    logmonitor::LogMonitor::Config config;
    config.log_path          = log_path;
    config.patterns          = std::move(patterns);
    config.on_match          = print_match;
    config.tail_mode         = tail_mode;
    config.queue_capacity    = 2048;
    config.chunk_buffer_size = 65536;

    try {
        logmonitor::LogMonitor monitor{std::move(config)};
        monitor.start();

        if (tail_mode) {
            // In tail mode, run until interrupted
            std::cerr << "Monitoring... Press Enter to stop.\n";
            std::cin.get();
            monitor.stop();
        } else {
            // In batch mode, wait for completion
            // Poll until the monitor finishes processing
            while (monitor.is_running()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            monitor.stop();
        }

        print_stats(monitor.stats());

    } catch (const std::exception& e) {
        std::cerr << "\033[1;31mFatal error:\033[0m " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
