# High-Performance Real-Time Log Monitor

A robust, real-time system log parser and monitoring tool built with modern C++20. Designed to handle massive log streams (e.g., `/var/log/auth.log`) efficiently without memory leaks or CPU bottlenecks.

## 🚀 Key Features
*   **Real-Time Monitoring:** Continuously tails and processes system log files.
*   **Blazing Fast Pattern Matching:** Utilizes the **Aho-Corasick algorithm** for simultaneous multi-pattern matching with $O(N + M + Z)$ time complexity. Eliminates Time Limit Exceeded (TLE) issues.
*   **Zero-Allocation Pipeline:** Implements a custom **Memory Pool** (`ChunkPool`) to recycle objects, preventing memory fragmentation and Garbage Collection overhead.
*   **Thread-Safe Concurrency:** Uses a strictly bounded Producer-Consumer architecture (`ThreadSafeQueue`) to guarantee a fixed memory footprint regardless of file size, effectively preventing Memory Limit Exceeded (MLE).
*   **Optimized I/O:** Chunked buffer reading via POSIX APIs.

## 🛠️ Tech Stack
*   **Language:** C++20
*   **Build System:** CMake
*   **Testing:** Google Test (GTest)
*   **Core Concepts:** Multi-threading, Synchronization (Mutex/Condition Variables), Advanced Data Structures (Trie/Automaton), POSIX I/O.

## ⚙️ Build and Run

### Prerequisites
* GCC 10+ or Clang 11+ (C++20 support required)
* CMake 3.15+

### Build Instructions
```bash
# Clone the repository
git clone [https://github.com/your-username/high-perf-log-monitor.git](https://github.com/your-username/high-perf-log-monitor.git)
cd high-perf-log-monitor

# Configure and build
mkdir build && cd build
cmake ..
cmake --build .

# Run tests
ctest --output-on-failure

```

## 👤 Author

**Nguyễn Đức Thuận**
- **GitHub:** [@NDT29thDec](https://github.com/NDT29thDec)
- **Email:** [fire29122007@gmail.com]
