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
    # P6 so: the SAME nine ITransport-contract cases InProcessTransportTest runs, over a real
    # socketpair with real threads, plus the three a stream has and a message queue does not.
    # POSIX for the same reason as FdPassingTest - and because CONTRACT-P6 §2.6 lands POSIX only.
    list(APPEND MOBILEGL_WIRE_TESTS SocketTransportTest)
    # P6 sm: two MobileGL processes. Needs the real server image beside it, so
    # the dependency is declared after the loop rather than assumed.
    list(APPEND MOBILEGL_WIRE_TESTS ServerSpawnTest)
endif()

include(GoogleTest)

# ServerSpawnTest execs a real second process, so the image has to exist before
# the test runs and the test has to be told where it landed. Both stated here
# rather than left to a working-directory coincidence.
set(MOBILEGL_SERVER_IMAGE_PATH "${CMAKE_BINARY_DIR}/libMobileGLServer.so")

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

    if (test STREQUAL "ServerSpawnTest")
        # It execs the real image, so it must be built first and must be told
        # where it is. MOBILEGL_TEST_SERVER_PATH is read by the test's
        # ServerImage() helper; without it the test would depend on the working
        # directory, which ctest does not promise.
        if (TARGET MobileGLServer)
            add_dependencies(${test} MobileGLServer)
        endif()
        gtest_discover_tests(${test} DISCOVERY_TIMEOUT 30
                PROPERTIES LABELS unit
                ENVIRONMENT "MOBILEGL_TEST_SERVER_PATH=${MOBILEGL_SERVER_IMAGE_PATH}")
    else ()
        gtest_discover_tests(${test} DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
    endif ()
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

# P5 c1's, v1's and s1's suites. All three are registered on their own, and all three for
# PipeWireCodecTest's reason: a Fatal arm reports through MGLOG_F, which writes to stdout and to
# a named file and NEVER to stderr (and with the console sink compiled out, Defines.h, only to
# the file), so asserting one means naming a log file before anything logs - which needs a
# main() of its own. All three also reach MG_Pipe, MG_Backend and MG_State, so all three carry
# those include paths.
#
#   RemoteClientTest       the 71-slot emit table, the caps mirror, R-8's liveness gates, R-17's
#                          routing and ID-47/ID-49's readback rules (c1)
#   ServerLoopTest         the apply thread, the blocking control mailbox, the verb stamp and
#                          R-11's server-owned staging copy (v1)
#   SessionHandshakeTest   the two null-union guards driven THROUGH ServerSession::Accept and
#                          ClientSession::StartOverTransportPair, and the ABI fingerprint's
#                          sensitivity case driven from CapsAbiFingerprint() (s1, ID-46 6 and 7)
#   StagedTextureStoreTest the texture half of R-11: tx's staged-texture store, its
#                          Fatal{StageSnapshotTooNarrow}, and the E-P5c #2 production wiring
#                          through the real resource op table (P5c tx)
#   SurfaceControlFrameTest P5f fc: the value frame that replaced the control mailbox's
#                          function-pointer payload, its wire codec against protocol.fbs's
#                          SurfaceOp/SurfaceReply, and the two named refusals at the wire entry
#
# THIS FILE IS THE PHASE'S ONE RECURRING MERGE CONFLICT, and it is the same-point-append shape
# BRIEF §5's ownership table exists to prevent: three packages, three targets, one end-of-file.
# The loop is what stops there being a fourth: a package adding a suite adds a NAME.
foreach (wiretest IN ITEMS RemoteClientTest ServerLoopTest SessionHandshakeTest StagedTextureStoreTest SurfaceControlFrameTest)
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
