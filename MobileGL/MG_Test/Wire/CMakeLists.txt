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
        # P6 dl: the Fatal-family table and its total projection onto the wire FatalCode.
        FatalFamilyTest
        # P12 (on-screen server window): the server's own window holder, driven with a fake
        # window handle (no GL, no process, no platform window).
        ServerDisplayTest
)

if (NOT WIN32)
    # SCM_RIGHTS and fork(): POSIX only.
    list(APPEND MOBILEGL_WIRE_TESTS FdPassingTest)
    # P6 so: the SAME nine ITransport-contract cases InProcessTransportTest runs, over a real
    # socketpair with real threads, plus the three a stream has and a message queue does not.
    # POSIX for the same reason as FdPassingTest - and because CONTRACT-P6 §2.6 lands POSIX only.
    list(APPEND MOBILEGL_WIRE_TESTS SocketTransportTest StreamLinkTest StreamClientPublicationTest InitialCapsStartupTest)
    # P7 F2, PH-7 (5) and fuzz arm 1: the TCP supervisor's pre-auth gate (first-frame shapes, the
    # exact first-frame reader, the backoff's arithmetic, the knobs). Its reader is a socket read,
    # so POSIX for the same reason.
    list(APPEND MOBILEGL_WIRE_TESTS PreAuthGateTest)
    # P6 sm: two MobileGL processes. Needs the real server image beside it, so
    # the dependency is declared after the loop rather than assumed.
    list(APPEND MOBILEGL_WIRE_TESTS ServerSpawnTest)
    # P7 F2 latch (PH-1 (3)(4), Ph fuzz arm 2): one raw-peer row per latched site against a real
    # `--serve` supervisor, each followed by a healthy next session. Needs the server image too.
    list(APPEND MOBILEGL_WIRE_TESTS PeerLatchTest)
    # P12 (on-screen server window). InProcessServerTest: the in-process display server entry run on a
    # thread (sequential sessions, the latch reset, Busy, the backend pin) and D8's parent-death signal
    # for a forked session child. ServerOwnedSurfaceTest: a real MobileGL client with
    # MOBILEGL_IPC_SURFACE=server against a display-less `--serve` supervisor. Both drive real peers
    # and (the second, and D8's case) the real server image, as PeerLatchTest does.
    list(APPEND MOBILEGL_WIRE_TESTS InProcessServerTest ServerOwnedSurfaceTest)
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
    if (test STREQUAL "PeerLatchTest" OR test STREQUAL "InProcessServerTest" OR test STREQUAL "ServerOwnedSurfaceTest")
        # It builds records and a program archive by hand, so it reaches MG_Pipe and MG_State
        # the way the codec suites do. (P12's two server suites share PeerLatchTest's healthy
        # session, P12ServerRig.h, and so its include paths.)
        target_include_directories(${test} PRIVATE
                ${MGL_ROOT}/MobileGL/MG_Pipe
                ${MGL_ROOT}/3rdparty/xxHash
                ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
                ${MGL_ROOT}/3rdparty/SPIRV-Reflect
        )
    endif ()

    target_link_libraries(${test} PRIVATE
            GTest::gtest_main
            ${LINK_LIBRARIES}
    )

    if (test STREQUAL "ServerSpawnTest" OR test STREQUAL "PeerLatchTest" OR test STREQUAL "InProcessServerTest" OR
        test STREQUAL "ServerOwnedSurfaceTest")
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
#   PersistentMapFaultTest P7 X1: the persistent-map tracker's two client-facing promises -
#                          that its fault-ownership row runs in the KERNEL's address space
#                          (arm64 tags every heap pointer, si_addr is untagged) and that a
#                          map hands back a range writable without the handler
#
# THIS FILE IS THE PHASE'S ONE RECURRING MERGE CONFLICT, and it is the same-point-append shape
# BRIEF §5's ownership table exists to prevent: three packages, three targets, one end-of-file.
# The loop is what stops there being a fourth: a package adding a suite adds a NAME.
foreach (wiretest IN ITEMS RemoteClientTest ServerLoopTest SessionHandshakeTest StagedTextureStoreTest SurfaceControlFrameTest PersistentMapFaultTest)
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

# P7 wave 2-F, F fix round (PH-7 (4) review). MOBILEGL_PROTOCOL_CONTROL_REVISION is a hand-bumped
# integer mixed into wireFingerprint, and nothing tied it to protocol.fbs's content: a schema edit
# that forgot the bump would leave old and new peers agreeing on the fingerprint and surface as a
# silently absent field. This entry runs the pin ({revision -> sha256(protocol.fbs)},
# scripts/ci/protocol_revision_pin.py) with its self-test, so that edit is red in the unit lane.
# No compilation, no GL; label unit, the way MobileGLPurity.IncludeClosure is registered.
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_test(NAME ProtocolSchema.RevisionPinsDigest
             COMMAND ${Python3_EXECUTABLE} ${MGL_ROOT}/scripts/ci/protocol_revision_pin.py --self-test)
    set_tests_properties(ProtocolSchema.RevisionPinsDigest PROPERTIES LABELS unit)
    # P7 F2 latch (fuzz arm 2): every per-session latch site in the four PH-1 (3) files has a
    # PeerLatchTest row whose marker names it, or is argued unreachable in the script; the
    # self-test drops each covering row and requires its site to go unmapped.
    add_test(NAME PeerLatch.SiteMap
             COMMAND ${Python3_EXECUTABLE} ${MGL_ROOT}/scripts/ci/ph_latch_sites.py --self-test)
    set_tests_properties(PeerLatch.SiteMap PROPERTIES LABELS unit)
    # The census's rule 2 now covers SessionLatch as well as SessionFail / WireLogFatal; its
    # self-test feeds it one unmarked call of each and requires every one to be reported.
    add_test(NAME FatalCensus.RuleTwoSelfTest
             COMMAND ${Python3_EXECUTABLE} ${MGL_ROOT}/scripts/ci/fatal_census.py --self-test)
    set_tests_properties(FatalCensus.RuleTwoSelfTest PROPERTIES LABELS unit)
endif ()

# Ph fuzz arm 3 (PH-6, ID-P7-2): a `--serve` supervisor, a peer that stops draining SEG_EVENT, and
# the next connection. NOT discovered into `unit`: the peer brings a pbuffer context up on the
# server, which needs the integration lanes' EGL environment, so MG_IntegrationTest/CMakeLists.txt
# registers its two cases - SpawnLane.EventForfeitPeer (integration-spawn, the shm data plane) and
# TcpLane.EventForfeitPeer (integration-tcp, the stream data plane) - beside the TCP supervisor's
# own protocol controls. It execs the server image, so it is built after it, as ServerSpawnTest is.
if (NOT WIN32)
    add_executable(EventForfeitPeerTest EventForfeitPeerTest.cpp)
    target_include_directories(EventForfeitPeerTest PRIVATE
            ${MGL_ROOT}/include
            ${MGL_ROOT}/MobileGL
            ${MGL_ROOT}/3rdparty/flatbuffers/include
    )
    target_link_libraries(EventForfeitPeerTest PRIVATE
            GTest::gtest_main
            ${LINK_LIBRARIES}
    )
    if (TARGET MobileGLServer)
        add_dependencies(EventForfeitPeerTest MobileGLServer)
    endif ()
endif ()

# P6.5 manual fault-injection probe: requires an explicit live peer and is never
# registered with ctest, so a device-less unit run cannot silently skip this gate.
if (NOT WIN32)
    add_executable(MobileGLPeerLossProbe PeerLossProbe.cpp)
    target_include_directories(MobileGLPeerLossProbe PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/flatbuffers/include
    )
    target_link_libraries(MobileGLPeerLossProbe PRIVATE ${LINK_LIBRARIES})
endif ()
