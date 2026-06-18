# ==============================================================================
# logmonitor_lib — Core static library
# ==============================================================================
add_library(logmonitor_lib STATIC
    log_reader.cpp
    pattern_matcher.cpp
    log_monitor.cpp
)

target_include_directories(logmonitor_lib
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(logmonitor_lib
    PUBLIC
        Threads::Threads
)

# Alias for cleaner usage in other targets
add_library(logmonitor::lib ALIAS logmonitor_lib)
