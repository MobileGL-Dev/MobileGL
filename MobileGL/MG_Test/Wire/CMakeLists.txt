cmake_minimum_required(VERSION 3.14)

# The MG_Remote wire layer: framing, the SPSC ring, the in-process transport,
# SCM_RIGHTS descriptor passing and the generated control-plane schema. Only
# reachable with MOBILEGL_BUILD_DISAGGREGATED=ON (see MG_Test/CMakeLists.txt).

set(MOBILEGL_WIRE_TESTS
        FramingTest
        RingTest
        InProcessTransportTest
        ProtocolSmokeTest
        # P5 s1: the ring-owning session pair - four ShmSegment-backed segments, the five
        # watermarks with real writers, the reply slot pool and the event ring.
        SessionTest
)

if (NOT WIN32)
    # SCM_RIGHTS and fork(): POSIX only.
    list(APPEND MOBILEGL_WIRE_TESTS FdPassingTest)
endif()

include(GoogleTest)

foreach (test IN LISTS MOBILEGL_WIRE_TESTS)
    add_executable(${test} ${test}.cpp)

    target_include_directories(${test} PRIVATE
            ${MGL_ROOT}/include
            ${MGL_ROOT}/MobileGL
            ${MGL_ROOT}/3rdparty/flatbuffers/include
    )

    target_link_libraries(${test} PRIVATE
            GTest::gtest_main
            ${LINK_LIBRARIES}
    )

    gtest_discover_tests(${test} DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
endforeach ()

# P5 w1's G3 codec suite. It is registered on its own rather than in the list above because it
# links gtest, NOT gtest_main: the R-2 Fatal arms report through MGLOG_F + std::abort, so the
# cases that drive one fork and read the Fatal line back out of a log file the process names
# before anything logs - which needs a main() of its own (PipeInputsTest's shape). It also
# reaches MG_Pipe and MG_State, so it carries their include paths.
add_executable(PipeWireCodecTest PipeWireCodecTest.cpp)

target_include_directories(PipeWireCodecTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/MobileGL/MG_Pipe
        ${MGL_ROOT}/3rdparty/flatbuffers/include
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(PipeWireCodecTest PRIVATE
        GTest::gtest
        ${LINK_LIBRARIES}
)

if (MSVC)
    target_compile_options(PipeWireCodecTest PRIVATE /Zc:preprocessor)
endif ()

gtest_discover_tests(PipeWireCodecTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)

# P5 v1's suite: the apply thread, the blocking control mailbox, the verb stamp and R-11's
# server-owned staging copy. Registered on its own for PipeWireCodecTest's reason - it links
# gtest and carries its own main(), because the Fatal arms report through MGLOG_F + std::abort
# and a case that asserts one has to name the log file BEFORE anything logs. It also reaches
# MG_Pipe, MG_Backend and MG_State, so it carries their include paths.
add_executable(ServerLoopTest ServerLoopTest.cpp)

target_include_directories(ServerLoopTest PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/MobileGL/MG_Pipe
        ${MGL_ROOT}/3rdparty/flatbuffers/include
        ${MGL_ROOT}/3rdparty/xxHash
        ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(ServerLoopTest PRIVATE
        GTest::gtest
        ${LINK_LIBRARIES}
)

if (MSVC)
    target_compile_options(ServerLoopTest PRIVATE /Zc:preprocessor)
endif ()

gtest_discover_tests(ServerLoopTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
