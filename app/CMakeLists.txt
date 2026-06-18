# ==============================================================================
# logmonitor — CLI executable
# ==============================================================================
add_executable(logmonitor main.cpp)

target_link_libraries(logmonitor
    PRIVATE
        logmonitor::lib
)
