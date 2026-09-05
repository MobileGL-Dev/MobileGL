cmake_minimum_required(VERSION 3.14)

add_executable(
        JobNodeTest
        JobNodeTest.cpp
)

target_include_directories(JobNodeTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        JobNodeTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

# The log-severity ordering and the MOBILEGL_ASSERT gate keyed to it. Links gtest
# (not gtest_main): the suite needs its own main() to point MOBILEGL_LOG_FILE_PATH at a
# temp file before anything in the process logs and latches the sink's FILE*.
add_executable(
        LogLevelTest
        LogLevelTest.cpp
)

target_include_directories(LogLevelTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        LogLevelTest PRIVATE
        GTest::gtest
        ${LINK_LIBRARIES}
)

# The MGPipe boundary counters: the enable latch, the byte/call/gate arithmetic, the
# payload histogram's bucketing and the two report formats. No GL context, no driver.
add_executable(
        PipeStatsTest
        PipeStatsTest.cpp
)

target_include_directories(PipeStatsTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        PipeStatsTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(JobNodeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(LogLevelTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(PipeStatsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
