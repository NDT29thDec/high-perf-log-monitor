# ==============================================================================
# Tests — Google Test via FetchContent
# ==============================================================================
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

# Prevent GoogleTest from overriding our compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Helper function to create test targets
function(add_logmonitor_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            logmonitor::lib
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# Register all test targets
add_logmonitor_test(test_thread_safe_queue  test_thread_safe_queue.cpp)
add_logmonitor_test(test_chunk_pool         test_chunk_pool.cpp)
add_logmonitor_test(test_pattern_matcher    test_pattern_matcher.cpp)
add_logmonitor_test(test_log_reader         test_log_reader.cpp)
