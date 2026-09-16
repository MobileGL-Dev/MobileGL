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

# P5 c1's and v1's suites. Both are registered on their own, and both for
# PipeWireCodecTest's reason - MGLOG_F writes to stdout and to a named file and NEVER to
# stderr, so a Fatal arm can only be asserted by forking and reading that file back, which
# needs a main() of its own. Both also reach MG_Pipe, MG_Backend and MG_State, so both carry
# those include paths.
#
#   RemoteClientTest  the 71-slot emit table, the caps mirror, R-8's liveness gates and
#                     R-17's routing (c1)
#   ServerLoopTest    the apply thread, the blocking control mailbox, the verb stamp and
#                     R-11's server-owned staging copy (v1)
#
# THE ONLY CONFLICT IN THE c1/v1 MERGE, and it is the same-point-append shape BRIEF §5's
# ownership table exists to prevent: two packages each added a target at the end of one file.
# Both blocks are kept verbatim; nothing is chosen over anything.
foreach (wiretest IN ITEMS RemoteClientTest ServerLoopTest)
    add_executable(${wiretest} ${wiretest}.cpp)

    target_include_directories(${wiretest} PRIVATE
            ${MGL_ROOT}/include
            ${MGL_ROOT}/MobileGL
            ${MGL_ROOT}/MobileGL/MG_Pipe
            ${MGL_ROOT}/3rdparty/flatbuffers/include
            ${MGL_ROOT}/3rdparty/xxHash
            ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
            ${MGL_ROOT}/3rdparty/SPIRV-Reflect
    )

    target_link_libraries(${wiretest} PRIVATE
            GTest::gtest
            ${LINK_LIBRARIES}
    )

    if (MSVC)
        target_compile_options(${wiretest} PRIVATE /Zc:preprocessor)
    endif ()

    gtest_discover_tests(${wiretest} DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
endforeach ()
