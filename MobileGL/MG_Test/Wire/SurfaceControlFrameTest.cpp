// MobileGL - MobileGL/MG_Test/Wire/SurfaceControlFrameTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5f, package fc: the control-plane frame and its wire codec.
//
// What is proven here:
//   * the frame is a plain bag of scalars (the invariant the package exists for - the old
//     mailbox carried a function pointer and the caller's stack address);
//   * exactly the ten framed ops have wire kinds, and the two enum mappings (SurfaceControlOp
//     <-> ::MobileGL::Wire::SurfaceOpKind, WindowBackend <-> ::MobileGL::Wire::WindowKind) are explicit tables whose
//     agreement with the schema is pinned, not assumed;
//   * every framed op round-trips through a REAL CtrlEnvelope{SurfaceOp} buffer byte-for-byte;
//   * the wire entry refuses by NAME: an ANativeWindow* arriving over the wire dies
//     Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"} (real windows are P12), and a kind the
//     schema does not define dies Fatal{ProtocolCorruption, "SurfaceOp"}.
//
// The inproc blocking path itself - posting, parking, the reply half coming back - is gated by
// ServerLoopTest's cases (AVoidForwarderCrossesAsOneDispatchedFrame and the whole
// ServerLoopEglTest fixture, which now drives every forwarder through the frame channel).

#include <MG_Remote/Protocol/SurfaceOpCodec.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/SurfaceControlFrame.h>

#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace MobileGL;

namespace Server = MobileGL::MG_Remote::Server;
using MobileGL::MG_Remote::DecodeWireSurfaceOp;
using MobileGL::MG_Remote::DecodeWireSurfaceReply;
using MobileGL::MG_Remote::EncodeSurfaceOpFrame;
using MobileGL::MG_Remote::EncodeSurfaceReplyFrame;
using MobileGL::MG_Remote::ServerApplyWireSurfaceOp;
using MobileGL::MG_Remote::SurfaceControlOpForWireKind;
using MobileGL::MG_Remote::SurfaceWireError;
using MobileGL::MG_Remote::WindowBackendForWireWindowKind;
using MobileGL::MG_Remote::WireKindForSurfaceControlOp;
using MobileGL::MG_Remote::WireWindowKindForWindowBackend;
using Server::SurfaceControlFrame;
using Server::SurfaceControlOp;

namespace {

    std::string g_logPath;

    std::string ReadLog() {
        // BOTH ROLES' LOGS (P6). A death test asserts that the CHILD said something; which
        // role's thread said it is not what these cases are about, and refusals raised on the
        // apply thread are written under the SERVER role by construction.
        return MobileGL::MG_Util::Debug::ReadRoleLogs(g_logPath.c_str());
    }

    unsigned ProcessId() {
#if defined(_WIN32)
        return static_cast<unsigned>(_getpid());
#else
        return static_cast<unsigned>(::getpid());
#endif
    }

    // Encode one frame and hand back the parsed wire op. The builder must outlive the parse.
    const ::MobileGL::Wire::SurfaceOp* EncodeAndParse(const SurfaceControlFrame& frame,
                                          flatbuffers::FlatBufferBuilder* builder) {
        if (EncodeSurfaceOpFrame(frame, builder) != SurfaceWireError::None) return nullptr;
        const auto* envelope = ::MobileGL::Wire::GetCtrlEnvelope(builder->GetBufferPointer());
        if (envelope == nullptr || envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::SurfaceOp) return nullptr;
        return envelope->msg_as_SurfaceOp();
    }

} // namespace

// The invariant the package exists for, in executable form: the channel's payload can hold no
// address. The compile-time twin is the static_assert in SurfaceControlFrame.h; this case is the
// readable one, and it is what a future "just add a pointer field" edit trips over FIRST (the
// static_assert) - the test exists so the failure also has a name in the ctest log.
TEST(SurfaceControlFrameTest, TheFrameIsAPlainBagOfScalars) {
    EXPECT_TRUE(std::is_trivially_copyable_v<SurfaceControlFrame>)
        << "the frame grew a non-trivial member; the one-slot channel copies it by value";
    EXPECT_TRUE(std::is_standard_layout_v<SurfaceControlFrame>)
        << "the frame lost standard layout; SurfaceOpCodec encodes it field by field and the two "
           "shapes must stay obvious to a reader";
}

TEST(SurfaceControlFrameTest, EveryMemberIsMechanicallyCheckedAsAnIntegerOrEnum) {
    static_assert(Server::Detail::SurfaceControlFrameHasOnlyValues());
    static_assert(!Server::Detail::IsSurfaceControlValue<void*>);
    static_assert(!Server::Detail::IsSurfaceControlValue<void (*)()>);
    EXPECT_TRUE(Server::Detail::SurfaceControlFrameHasOnlyValues());
    // Why trivially-copyable alone did not enforce this invariant.
    struct PointerBag { void* pointer; };
    static_assert(std::is_trivially_copyable_v<PointerBag> && std::is_standard_layout_v<PointerBag>);
    EXPECT_FALSE(Server::Detail::IsSurfaceControlValue<decltype(PointerBag::pointer)>);
}

TEST(SurfaceControlFrameTest, ExactlyTheTenFramedOpsHaveWireKindsAndTheValuesArePinned) {
    // The ten framed ops, in schema order. The numeric agreement with ::MobileGL::Wire::SurfaceOpKind is
    // asserted per op rather than trusted from the static_asserts, because THIS is the table a
    // reader consults when the schema next changes.
    const SurfaceControlOp wired[] = {
        SurfaceControlOp::InitializeDisplay, SurfaceControlOp::CreateWindowSurface,
        SurfaceControlOp::CreatePbufferSurface, SurfaceControlOp::ResizeWindowSurface,
        SurfaceControlOp::ReleaseSurface, SurfaceControlOp::MakeCurrent,
        SurfaceControlOp::ReleaseCurrent, SurfaceControlOp::SetSwapInterval,
        SurfaceControlOp::ReleaseResources, SurfaceControlOp::SetWindowHandle,
    };
    for (const SurfaceControlOp op : wired) {
        ::MobileGL::Wire::SurfaceOpKind kind;
        ASSERT_TRUE(WireKindForSurfaceControlOp(op, &kind))
            << Server::SurfaceControlOpName(op) << " lost its wire kind";
        EXPECT_EQ(static_cast<unsigned>(kind), static_cast<unsigned>(op))
            << Server::SurfaceControlOpName(op) << ": the frame and wire enums drifted apart";
        SurfaceControlOp back;
        ASSERT_TRUE(SurfaceControlOpForWireKind(kind, &back));
        EXPECT_EQ(back, op);
    }
    // fc's three schema additions are APPEND-ONLY: these values are wire ABI, not taste.
    EXPECT_EQ(static_cast<unsigned>(::MobileGL::Wire::SurfaceOpKind::SetSwapInterval), 8u);
    EXPECT_EQ(static_cast<unsigned>(::MobileGL::Wire::SurfaceOpKind::ReleaseResources), 9u);
    EXPECT_EQ(static_cast<unsigned>(::MobileGL::Wire::SurfaceOpKind::SetWindowHandle), 10u);

    // The inproc-only kinds refuse the wire by name, both directions.
    // InitCapabilities WAS in this list and cp took it out; the eleventh op has a
    // test of its own below rather than a line here, because this test's name is
    // a statement about ten and G14 does not let a name be traded for another.
    const SurfaceControlOp inprocOnly[] = {
        SurfaceControlOp::None,
        SurfaceControlOp::SwapBuffersInprocOnly, SurfaceControlOp::InitWindowSurfaceInprocOnly,
        SurfaceControlOp::ProbeForTesting,
    };
    for (const SurfaceControlOp op : inprocOnly) {
        ::MobileGL::Wire::SurfaceOpKind kind;
        EXPECT_FALSE(WireKindForSurfaceControlOp(op, &kind))
            << Server::SurfaceControlOpName(op) << " may never leave the process";
    }
    SurfaceControlOp op;
    EXPECT_FALSE(SurfaceControlOpForWireKind(::MobileGL::Wire::SurfaceOpKind::None, &op));
    EXPECT_FALSE(SurfaceControlOpForWireKind(static_cast<::MobileGL::Wire::SurfaceOpKind>(200), &op))
        << "a wire kind the schema does not define must not decode into an op";
}

TEST(SurfaceControlFrameTest, TheEleventhFramedOpIsInitCapabilitiesAndItCrossesTheWire) {
    // cp. The op above this one in the enum, SetWindowHandle, was fc's last
    // append; this is P6's. It gets a test rather than a row in the ten-op table
    // because the failure it guards is specific and was REAL: with no wire kind
    // the codec answered InprocOnlyOpOnTheWire, BackendObject_Remote's
    // InitCapabilities call failed, MakeEGLCurrent reported "InitCapabilities
    // failed", and the OpenRA spawn retrace died one call after the server's
    // backend had come up green. Nothing about that read as a missing enum row.
    ::MobileGL::Wire::SurfaceOpKind kind;
    ASSERT_TRUE(WireKindForSurfaceControlOp(SurfaceControlOp::InitCapabilities, &kind))
        << "InitCapabilities lost its wire kind; a spawn client cannot ask for caps";
    EXPECT_EQ(static_cast<unsigned>(kind), 11u) << "wire ABI: append-only, and 11 is taken";
    EXPECT_EQ(static_cast<unsigned>(SurfaceControlOp::InitCapabilities), 11u);

    SurfaceControlOp back;
    ASSERT_TRUE(SurfaceControlOpForWireKind(kind, &back))
        << "the decode range's upper bound did not move with the append, so the new tag "
           "reads as out-of-range - which surfaces as Fatal{ProtocolCorruption}, not as the "
           "missing row it is";
    EXPECT_EQ(back, SurfaceControlOp::InitCapabilities);

    // And it really encodes. WireKindForSurfaceControlOp answering yes is not the
    // same fact as the encoder accepting the frame: the refusal the client hit
    // came out of EncodeSurfaceOpFrame.
    flatbuffers::FlatBufferBuilder builder(256);
    SurfaceControlFrame frame;
    frame.kind = SurfaceControlOp::InitCapabilities;
    frame.seq = 7;
    EXPECT_EQ(EncodeSurfaceOpFrame(frame, &builder), SurfaceWireError::None);
}

TEST(SurfaceControlFrameTest, WindowBackendAndWireWindowKindMapExplicitlyBothWays) {
    using MG_Backend::WindowBackend;
    const struct {
        WindowBackend backend;
        ::MobileGL::Wire::WindowKind kind;
    } table[] = {
        {WindowBackend::Android, ::MobileGL::Wire::WindowKind::AndroidNativeWindow},
        {WindowBackend::X11, ::MobileGL::Wire::WindowKind::X11},
        {WindowBackend::MetalLayer, ::MobileGL::Wire::WindowKind::MetalLayer},
        {WindowBackend::Win32, ::MobileGL::Wire::WindowKind::Win32Hwnd},
        {WindowBackend::Unknown, ::MobileGL::Wire::WindowKind::None},
    };
    for (const auto& row : table) {
        ::MobileGL::Wire::WindowKind kind;
        ASSERT_TRUE(WireWindowKindForWindowBackend(row.backend, &kind));
        EXPECT_EQ(kind, row.kind);
        WindowBackend backend;
        ASSERT_TRUE(WindowBackendForWireWindowKind(row.kind, &backend));
        EXPECT_EQ(backend, row.backend);
    }
    // fc added MetalLayer to the schema; the value is wire ABI.
    EXPECT_EQ(static_cast<unsigned>(::MobileGL::Wire::WindowKind::MetalLayer), 6u);

    // Surfaceless and Pbuffer are surface SHAPES, not window backends: no answer, by name.
    WindowBackend backend;
    EXPECT_FALSE(WindowBackendForWireWindowKind(::MobileGL::Wire::WindowKind::Surfaceless, &backend));
    EXPECT_FALSE(WindowBackendForWireWindowKind(::MobileGL::Wire::WindowKind::Pbuffer, &backend));
    EXPECT_FALSE(WindowBackendForWireWindowKind(static_cast<::MobileGL::Wire::WindowKind>(200), &backend));

    // And the two enums are NOT integer-compatible, which is the whole reason the mapping is a
    // table: a cast would shift every answer (Android is 0 in one and 1 in the other).
    EXPECT_NE(static_cast<int>(WindowBackend::Android),
              static_cast<int>(::MobileGL::Wire::WindowKind::AndroidNativeWindow))
        << "the enums became integer-compatible; a future cast-based shortcut would silently "
           "pass this test - keep it red-shaped";
}

TEST(SurfaceControlFrameTest, EveryFramedOpRoundTripsThroughTheWireEnvelope) {
    const SurfaceControlOp wired[] = {
        SurfaceControlOp::InitializeDisplay, SurfaceControlOp::CreateWindowSurface,
        SurfaceControlOp::CreatePbufferSurface, SurfaceControlOp::ResizeWindowSurface,
        SurfaceControlOp::ReleaseSurface, SurfaceControlOp::MakeCurrent,
        SurfaceControlOp::ReleaseCurrent, SurfaceControlOp::SetSwapInterval,
        SurfaceControlOp::ReleaseResources, SurfaceControlOp::SetWindowHandle,
    };
    for (const SurfaceControlOp op : wired) {
        SurfaceControlFrame frame;
        frame.kind = op;
        frame.seq = 0x1122334455667788ull;
        frame.display = 11;
        frame.surface = 22;
        frame.readSurface = 33;
        frame.context = 44;
        frame.width = 1920;
        frame.height = 1080;
        frame.swapInterval = 3;
        if (op == SurfaceControlOp::CreateWindowSurface || op == SurfaceControlOp::SetWindowHandle) {
            frame.windowBackend = static_cast<Int>(MG_Backend::WindowBackend::X11);
            frame.nativeToken = 0xDEADBEEF;
        }
        flatbuffers::FlatBufferBuilder builder(256);
        const ::MobileGL::Wire::SurfaceOp* wireOp = EncodeAndParse(frame, &builder);
        ASSERT_NE(wireOp, nullptr) << Server::SurfaceControlOpName(op) << " failed to encode";

        SurfaceControlFrame back;
        ASSERT_EQ(DecodeWireSurfaceOp(*wireOp, &back), SurfaceWireError::None)
            << Server::SurfaceControlOpName(op);
        EXPECT_EQ(back.kind, frame.kind);
        EXPECT_EQ(back.seq, frame.seq);
        EXPECT_EQ(back.display, frame.display);
        EXPECT_EQ(back.surface, frame.surface);
        EXPECT_EQ(back.readSurface, frame.readSurface) << "readSurface must round-trip (the "
                                                          "MakeCurrent four-tuple)";
        EXPECT_EQ(back.context, frame.context);
        EXPECT_EQ(back.width, frame.width);
        EXPECT_EQ(back.height, frame.height);
        EXPECT_EQ(back.swapInterval, frame.swapInterval);
        EXPECT_EQ(back.windowBackend, frame.windowBackend)
            << Server::SurfaceControlOpName(op);
        EXPECT_EQ(back.nativeToken, frame.nativeToken);
    }
}

TEST(SurfaceControlFrameTest, TheReplyHalfRoundTripsAsASurfaceReply) {
    SurfaceControlFrame frame;
    frame.kind = SurfaceControlOp::InitializeDisplay;
    frame.seq = 4242;
    frame.ok = true;
    frame.eglMajor = 1;
    frame.eglMinor = 5;

    flatbuffers::FlatBufferBuilder builder(256);
    EncodeSurfaceReplyFrame(frame, &builder);
    const auto* envelope = ::MobileGL::Wire::GetCtrlEnvelope(builder.GetBufferPointer());
    ASSERT_NE(envelope, nullptr);
    ASSERT_EQ(envelope->msg_type(), ::MobileGL::Wire::CtrlMsg::SurfaceReply);
    const ::MobileGL::Wire::SurfaceReply* reply = envelope->msg_as_SurfaceReply();
    ASSERT_NE(reply, nullptr);

    SurfaceControlFrame back;
    DecodeWireSurfaceReply(*reply, &back);
    EXPECT_EQ(back.seq, 4242ull) << "a reply must name its op";
    EXPECT_TRUE(back.ok);
    EXPECT_EQ(back.eglMajor, 1);
    EXPECT_EQ(back.eglMinor, 5);
}

TEST(SurfaceControlFrameTest, MalformedAndInprocOnlyFramesRefuseToEncode) {
    flatbuffers::FlatBufferBuilder builder(256);
    SurfaceControlFrame frame; // kind == None
    EXPECT_EQ(EncodeSurfaceOpFrame(frame, &builder), SurfaceWireError::UnknownOpKind);

    frame.kind = SurfaceControlOp::ProbeForTesting;
    EXPECT_EQ(EncodeSurfaceOpFrame(frame, &builder), SurfaceWireError::InprocOnlyOpOnTheWire);
    frame.kind = SurfaceControlOp::SwapBuffersInprocOnly;
    EXPECT_EQ(EncodeSurfaceOpFrame(frame, &builder), SurfaceWireError::InprocOnlyOpOnTheWire);
    frame.kind = SurfaceControlOp::InitWindowSurfaceInprocOnly;
    EXPECT_EQ(EncodeSurfaceOpFrame(frame, &builder), SurfaceWireError::InprocOnlyOpOnTheWire);

    // A window op whose backend tag names no WindowBackend.
    frame.kind = SurfaceControlOp::CreateWindowSurface;
    frame.windowBackend = 99;
    EXPECT_EQ(EncodeSurfaceOpFrame(frame, &builder), SurfaceWireError::UnknownWindowKind);
}

TEST(SurfaceControlFrameTest, AWireWindowKindThatNamesNoBackendFailsDecodeByName) {
    flatbuffers::FlatBufferBuilder builder(256);
    const auto op = ::MobileGL::Wire::CreateSurfaceOp(
        builder, 1, ::MobileGL::Wire::SurfaceOpKind::SetWindowHandle, 0, 0, ::MobileGL::Wire::WindowKind::Surfaceless, 0, 0,
        0, 0, 0, 0);
    const auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::SurfaceOp, op.Union());
    ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
    const ::MobileGL::Wire::SurfaceOp* wireOp =
        ::MobileGL::Wire::GetCtrlEnvelope(builder.GetBufferPointer())->msg_as_SurfaceOp();
    ASSERT_NE(wireOp, nullptr);

    SurfaceControlFrame frame;
    EXPECT_EQ(DecodeWireSurfaceOp(*wireOp, &frame), SurfaceWireError::WindowKindNamesNoBackend);

    // And the Android refusal is a DECODE answer before it is a Fatal: the pure half.
    flatbuffers::FlatBufferBuilder androidBuilder(256);
    const auto androidOp = ::MobileGL::Wire::CreateSurfaceOp(
        androidBuilder, 1, ::MobileGL::Wire::SurfaceOpKind::CreateWindowSurface, 0, 0,
        ::MobileGL::Wire::WindowKind::AndroidNativeWindow, 0x1234, 800, 600, 0, 0, 0);
    const auto androidEnvelope =
        ::MobileGL::Wire::CreateCtrlEnvelope(androidBuilder, ::MobileGL::Wire::CtrlMsg::SurfaceOp, androidOp.Union());
    ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(androidBuilder, androidEnvelope);
    const ::MobileGL::Wire::SurfaceOp* androidWireOp =
        ::MobileGL::Wire::GetCtrlEnvelope(androidBuilder.GetBufferPointer())->msg_as_SurfaceOp();
    ASSERT_NE(androidWireOp, nullptr);
    EXPECT_EQ(DecodeWireSurfaceOp(*androidWireOp, &frame),
              SurfaceWireError::AndroidNativeWindowArrived);
}

#if !defined(_WIN32)
// The refusal, as a death. An ANativeWindow* is a pointer into the CLIENT's process; on the
// wire it can only mean "a real window tried to cross", and that is P12's story, not a surface
// the P5f/P6 server can create. The child dies by name, and the log line is asserted - a death
// test that only checks for a crash goes green on any other abort in the same body.
TEST(SurfaceControlFrameTest, AnAndroidNativeWindowOnTheWireIsRefusedByName) {
    flatbuffers::FlatBufferBuilder builder(256);
    const auto op = ::MobileGL::Wire::CreateSurfaceOp(
        builder, 9, ::MobileGL::Wire::SurfaceOpKind::CreateWindowSurface, 1, 2,
        ::MobileGL::Wire::WindowKind::AndroidNativeWindow, 0x1234, 800, 600, 0, 0, 0);
    const auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::SurfaceOp, op.Union());
    ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
    const ::MobileGL::Wire::SurfaceOp* wireOp = ::MobileGL::Wire::GetCtrlEnvelope(builder.GetBufferPointer())->msg_as_SurfaceOp();
    ASSERT_NE(wireOp, nullptr);

    EXPECT_EXIT(ServerApplyWireSurfaceOp(*wireOp, nullptr), ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{UnmigratedSurface, \"AndroidNativeWindow@P12\"}"), std::string::npos)
        << "the abort happened but not for this rule's reason; the log says: " << log;
}

// A kind the schema does not define (a NEWER client's op reaching an OLDER server, or
// corruption) is Fatal{ProtocolCorruption}, never ignored: an ignored control op is a client
// waiting on a reply that never comes, which is a hang wearing a green lane.
TEST(SurfaceControlFrameTest, AMetalLayerOnTheWireIsRefusedByName) {
    // Both operations that consume a native window must refuse the process-local object.
    for (const auto kind : {::MobileGL::Wire::SurfaceOpKind::CreateWindowSurface,
                            ::MobileGL::Wire::SurfaceOpKind::SetWindowHandle}) {
        flatbuffers::FlatBufferBuilder builder(256);
        const auto op = ::MobileGL::Wire::CreateSurfaceOp(
            builder, 19, kind, 1, 2, ::MobileGL::Wire::WindowKind::MetalLayer, 0x1234, 800, 600);
        const auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(
            builder, ::MobileGL::Wire::CtrlMsg::SurfaceOp, op.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        const auto* wireOp = ::MobileGL::Wire::GetCtrlEnvelope(builder.GetBufferPointer())->msg_as_SurfaceOp();
        ASSERT_NE(wireOp, nullptr);
        SurfaceControlFrame frame;
        EXPECT_EQ(DecodeWireSurfaceOp(*wireOp, &frame), SurfaceWireError::MetalLayerArrived);
        EXPECT_EXIT(ServerApplyWireSurfaceOp(*wireOp, nullptr), ::testing::KilledBySignal(SIGABRT), ".*");
        EXPECT_NE(ReadLog().find("Fatal{UnmigratedSurface, \"MetalLayer@P12\"}"), std::string::npos);
    }
}

TEST(SurfaceControlFrameTest, AnUnknownWireOpKindIsProtocolCorruptionByName) {
    flatbuffers::FlatBufferBuilder builder(256);
    const auto op = ::MobileGL::Wire::CreateSurfaceOp(builder, 9, static_cast<::MobileGL::Wire::SurfaceOpKind>(200), 1, 2,
                                          ::MobileGL::Wire::WindowKind::None, 0, 0, 0, 0, 0, 0);
    const auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::SurfaceOp, op.Union());
    ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
    const ::MobileGL::Wire::SurfaceOp* wireOp = ::MobileGL::Wire::GetCtrlEnvelope(builder.GetBufferPointer())->msg_as_SurfaceOp();
    ASSERT_NE(wireOp, nullptr);

    EXPECT_EXIT(ServerApplyWireSurfaceOp(*wireOp, nullptr), ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{ProtocolCorruption, \"SurfaceOp\"}"), std::string::npos)
        << "the abort happened but not for this rule's reason; the log says: " << log;
}
#endif

int main(int argc, char** argv) {
    // Before anything logs: MG_Util::Debug::InitFile() reads the variable once, on the first
    // write, and caches the FILE*. The name carries this process's pid, because
    // gtest_discover_tests runs every case as its own process, in parallel under ctest -j.
    // (ServerLoopTest's main, verbatim - the Fatal arms report through MGLOG_F, which writes to
    // a named file and never to stderr.)
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-surfaceframe-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
