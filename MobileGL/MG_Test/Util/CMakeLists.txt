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

# The partial-present bookkeeping: damage rectangles, the y-flip, the per-buffer missed damage of
# rotating shared images and the per-image buffer age. Pure, no GL context.
add_executable(
        DamageTest
        DamageTest.cpp
)

target_include_directories(DamageTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        DamageTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

# The system configuration files under the environment: the precedence MG_ConfigLoader::Init()
# and its pre-init LookupSetting readers share. No GL context, no driver.
add_executable(
        ConfigFileTest
        ConfigFileTest.cpp
)

target_include_directories(ConfigFileTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        ConfigFileTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

# The X-connection safety rule: MobileGL never dials an X display its own process serves (the
# Xwayland-on-glamor deadlock), with the /proc facts injected and once for real in a child.
add_executable(
        X11DisplayGuardTest
        X11DisplayGuardTest.cpp
)

target_include_directories(X11DisplayGuardTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        X11DisplayGuardTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

# GLX presentation through DRI3 + Present against a fake X server and fake shared images, and the
# glFlush-as-shared-image-boundary policy (docs/Disaggregated/notes/anland/plan-x11-gpu.md).
add_executable(
        X11PresentTest
        X11PresentTest.cpp
        SharedImageFlushPolicyTest.cpp
)

target_include_directories(X11PresentTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
)

target_link_libraries(
        X11PresentTest PRIVATE
        GTest::gtest_main
        ${LINK_LIBRARIES}
)

include(GoogleTest)
gtest_discover_tests(X11DisplayGuardTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(X11PresentTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(JobNodeTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(LogLevelTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(PipeStatsTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(DamageTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
gtest_discover_tests(ConfigFileTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
