cmake_minimum_required(VERSION 3.14)

add_executable(
        JobNodeTest
        JobNodeTest.cpp
)

target_include_directories(JobNodeTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

# GTest::gtest, not GTest::gtest_main: JobNodeTest supplies its own main so that
# MOBILEGL_LOG_FILE_PATH is set before the first log write in the process. The engine
# -selection cases read the log back to assert that an unrecognized MOBILEGL_ASYNC_POOL value
# warns, and the desktop log sink is the file (MOBILEGL_LOG_ENABLE_CONSOLE is 0).
target_link_libraries(
        JobNodeTest PRIVATE
        GTest::gtest
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(JobNodeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

# The engine comparison harness. Deliberately NOT registered with add_test: it measures wall
# time, so it has no pass/fail verdict to give CI, and it is driven by a script that varies
# MOBILEGL_ASYNC_POOL and MOBILEGL_ASYNC_SHADER_COMPILE_THREADS across a matrix. It lives
# beside JobNodeTest because it drives the same pool through the same two engines; it links
# MobileGL_s for the real glCompileShader/glLinkProgram frontend path.
add_executable(
        AsyncPoolBench
        AsyncPoolBench.cpp
)

target_include_directories(AsyncPoolBench PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        AsyncPoolBench PRIVATE
        ${LINK_LIBRARIES}
)
