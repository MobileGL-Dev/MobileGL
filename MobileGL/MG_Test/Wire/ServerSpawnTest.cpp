// MobileGL - MobileGL/MG_Test/Wire/ServerSpawnTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Two MobileGL processes, started INDEPENDENTLY and joined by a name.
// Package `sm`.
//
// THE SHAPE IS THE POINT. The server is launched with an endpoint and nothing
// else - no inherited descriptor, no shared memory, no handshake from its
// parent - and the client finds it by connecting to that name. Everything these
// cases prove therefore transfers unchanged to a server somebody started by
// hand, or that an Android Service started minutes earlier, which is the only
// arrangement the end state can use.
//
// Each assertion exists because something would otherwise pass silently:
//
//   the image links          - a6 found mobilegl_server_main existed NOWHERE in
//                              the tree, only in ARCHITECTURE.md:488-496. A
//                              target that failed to link would make every case
//                              below read as "did not run".
//   the bytes crossed a      - the echo carries the SERVER's own getpid(), so a
//   PROCESS                    test that accidentally talked to itself cannot
//                              pass. ID-124's lesson at the smallest scale.
//   the process tree is clean- §9.4: exactly one child while running, zero
//                              after. Counted, not promised.
//   the server dies on EOF   - and on EOF ONLY. §5.4 forbids a timeout from ever
//                              standing in for the death fact, so the client
//                              closes and the server must go by itself.
//   SCM_RIGHTS works across   - the mechanism the four segments ride on, proven
//   the boundary               before the data plane needs it.

#include <Config.h>
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/FatalFunnel.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSpawn.h>
#include <MG_Remote/Transport/Doorbell.h>
#include <MG_Remote/Transport/FdPassing.h>
#include <MG_Remote/Transport/SocketTransport.h>
#include <MG_Remote/Transport/ILink.h>
#include <MG_Remote/Transport/Ring.h>
#include <MG_Remote/Wire/PipeWireCodec.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
#include <MG_Util/Debug/Log.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote;

#if !defined(_WIN32)

namespace {

    // A private rendezvous per case. Independent processes need a NAME, and a
    // per-case one is what stops two runs colliding the way a default endpoint
    // would.
    std::string Endpoint(const char* label) {
        return std::string("/tmp/mgl-sm-") + label + "-" + std::to_string(::getpid()) + ".sock";
    }

    struct ScopedEnvironment {
        std::string name;
        std::string oldValue;
        bool hadOldValue = false;
        ScopedEnvironment(const char* key, const std::string& value) : name(key) {
            if (const char* prior = std::getenv(key)) {
                oldValue = prior;
                hadOldValue = true;
            }
            ::setenv(key, value.c_str(), 1);
        }
        ~ScopedEnvironment() {
            if (hadOldValue) ::setenv(name.c_str(), oldValue.c_str(), 1);
            else ::unsetenv(name.c_str());
        }
    };

    // A peer can mutate command-ring record bytes AFTER the production encoder
    // accepts and writes them. Sequence, framing size, and submitted watermark stay
    // valid; selected payload fields bypass the encoder's client-side checks.
    template <class Payload, class Mutator>
    bool EncodeThenMutateHeaderAndPublish(Client::ClientSession& session, MobileGL::MG_Pipe::MGPWireOp op,
                                           const Payload& payload, Mutator&& mutate,
                                           std::uint64_t* outSeq = nullptr,
                                           const Wire::WireTail* tails = nullptr,
                                           MobileGL::Uint32 tailCount = 0) {
        if (outSeq != nullptr) *outSeq = Wire::kInvalidSeq;
        auto* link = session.DataLink();
        if (link == nullptr || !link->Attached()) return false;
        Wire::WireRecordLayout layout{};
        if (!Wire::MGPipeWireRecordLayout(op, &payload, layout) ||
            layout.PayloadBytes != sizeof(payload)) return false;
        auto& encoder = session.Encoder();
        const std::uint64_t seq = tailCount == 0
                                      ? encoder.EncodeRecord(op, &payload, sizeof(payload))
                                      : encoder.EncodeRecord(op, &payload, sizeof(payload), tails, tailCount);
        if (seq == Wire::kInvalidSeq) return false;
        auto& commands = link->CommandsOut();
        const auto* arena = link->RecordArena();
        const std::uint64_t head = commands.LocalHead();
        if (arena == nullptr || arena->Base == nullptr || arena->Capacity == 0 ||
            head < layout.TotalBytes) return false;
        const std::uint64_t start = head - layout.TotalBytes;
        auto* bytes = static_cast<std::uint8_t*>(arena->Base) + (start & arena->Mask);
        auto* header = reinterpret_cast<Transport::RingRecordHeader*>(bytes);
        if (header->kind != static_cast<std::uint16_t>(op) || header->size != layout.TotalBytes)
            return false;
        mutate(*header, bytes + sizeof(Transport::RingRecordHeader));
        // Use the real session publisher after mutation so both the ring head
        // and the producer-owned submittedSeq stay monotone and teardown knows
        // the record's sequence. Only the record bytes bypass encoder checks.
        session.Producer().PublishAndNotify(seq);
        if (link->Flush() != MOBILEGL_OK) return false;
        if (outSeq != nullptr) *outSeq = seq;
        return true;
    }

    template <class Payload>
    bool PublishRawRecordAndWait(Client::ClientSession& session, MobileGL::MG_Pipe::MGPWireOp op,
                                 const Payload& payload, std::uint64_t* outSeq = nullptr) {
        std::uint64_t seq = Wire::kInvalidSeq;
        if (!EncodeThenMutateHeaderAndPublish(
                session, op, payload, [](Transport::RingRecordHeader&, std::uint8_t*) {}, &seq)) {
            return false;
        }
        if (outSeq != nullptr) *outSeq = seq;
        return session.WaitForApplied(seq, 5000) == Transport::SessionWait::Reached;
    }

    MobileGL::MG_Pipe::MGPFramebufferState RawColorAttachmentFramebuffer(
        MobileGL::MG_Pipe::MGPipeHandle fbo, MobileGL::MG_Pipe::MGPipeHandle texture,
        MobileGL::Uint64 contentHash) {
        MobileGL::MG_Pipe::MGPFramebufferState state{};
        state.Fbo = fbo;
        state.Target = static_cast<MobileGL::Uint8>(
            MobileGL::MG_Pipe::MGPipeFramebufferTarget::Both);
        state.IsDefault = 0;
        state.Complete = 1;
        state.Width = state.Height = state.Layers = state.Samples = 1;
        state.FixedSampleLocations = 1;
        for (auto& drawBuffer : state.DrawBuffers) drawBuffer = -1;
        state.DrawBuffers[0] = 0;
        state.Color[0].Res = texture;
        state.Color[0].InternalFormat =
            static_cast<MobileGL::Uint32>(MobileGL::TextureInternalFormat::RGBA8);
        state.Color[0].Kind = MobileGL::MG_Pipe::kMGPipeSurfaceKindTexture;
        state.Color[0].UploadTarget =
            static_cast<MobileGL::Uint16>(MobileGL::TextureUploadTarget::Texture2D);
        state.Color[0].TextureTarget =
            static_cast<MobileGL::Uint16>(MobileGL::TextureTarget::Texture2D);
        state.ContentHash = contentHash;
        return state;
    }

    MobileGL::MG_Pipe::MGPClear RawColorClear(MobileGL::MG_Pipe::MGPipeHandle fbo) {
        MobileGL::MG_Pipe::MGPClear clear{};
        clear.Fbo = fbo;
        clear.Kind = MobileGL::MG_Pipe::kMGPipeClearKindWhole;
        clear.DrawBufferIndex = -1;
        clear.BufferMask = GL_COLOR_BUFFER_BIT;
        clear.ValueClass = MobileGL::MG_Pipe::kMGPipeClearValueClassFloat;
        return clear;
    }

    std::string ServerImage() {
        if (const char* explicitPath = std::getenv("MOBILEGL_TEST_SERVER_PATH")) {
            return explicitPath;
        }
        return "libMobileGLServer.so";
    }

    struct Session {
        Server::LaunchedServer server;
        std::unique_ptr<Transport::SocketTransport> client;
    };

    // Keep the negative-path case leak-free even when an ASSERT returns early.
    struct ScopedSessionCleanup {
        Session& session;
        ~ScopedSessionCleanup() {
            Client::ClientSessionInstance().Stop();
            if (session.client) session.client->Shutdown();
            if (session.server.pid > 0) {
                int ignored = -1;
                if (Server::ReapServer(session.server, 50, &ignored) != MOBILEGL_OK) {
                    ::kill(session.server.pid, SIGKILL);
                    (void)Server::ReapServer(session.server, 5000, &ignored);
                }
            }
        }
    };

    // The two halves a real deployment does separately: start a process, then
    // connect to it. The bounded connect retry is the CLIENT's, not the
    // launcher's - a launcher that waited for the server to be ready would be
    // coupling the two processes again by the back door.
    bool Bring(const char* label, Session* out) {
        const std::string endpoint = Endpoint(label);
        if (Server::LaunchServer(ServerImage(), endpoint, &out->server) != MOBILEGL_OK) {
            return false;
        }
        return Transport::SocketTransport::ConnectTo(endpoint, 10000, out->client) == MOBILEGL_OK;
    }

    // Closing the connection is what the server sees as EOF, and EOF is the whole
    // exit condition. The launcher holds nothing to close.
    void CloseAndReap(Session& session, int* outExitCode) {
        if (session.client) {
            session.client->Shutdown();
        }
        ASSERT_EQ(Server::ReapServer(session.server, 5000, outExitCode), MOBILEGL_OK);
    }

    // The REAL handshake, not an echo. ServerMain runs ServerSession::Accept,
    // which wants a Hello and answers Welcome plus six SCM_RIGHTS offers - so a
    // test that spoke anything else would only ever prove the socket works.
    // Driving ClientSession is what proves the SEGMENTS crossed.
    MobileGLResult Handshake(Session& session) {
        return Client::ClientSessionInstance().StartOverSocket(std::move(session.client));
    }

    std::string Exchange(Transport::ITransport& transport, const std::string& message,
                         std::uint32_t timeoutMs = 4000) {
        const MobileGLResult sent =
            transport.SendFrame(MobileGLByteSpan{message.data(), message.size()});
        if (sent != MOBILEGL_OK) {
            return "<send-failed>";
        }
        std::vector<std::uint8_t> buffer(64 * 1024);
        std::uint64_t size = 0;
        MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
        const MobileGLResult got = transport.ReceiveFrame(span, &size, timeoutMs);
        if (got != MOBILEGL_OK) {
            return std::string("<result=") + std::to_string(static_cast<int>(got)) + ">";
        }
        return std::string(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<std::size_t>(size));
    }

} // namespace

TEST(ServerSpawnTest, ARawPeerMutatesARecordAfterTheClientEncoderAcceptedIt) {
    const std::string endpoint = Endpoint("raw-peer");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("raw-peer", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    MobileGL::MG_Pipe::MGPMemoryBarrier barrier{};
    barrier.Bits = 0x2000u;
    barrier.ByRegion = 0;
    auto& client = Client::ClientSessionInstance();
    auto* link = client.DataLink();
    ASSERT_NE(link, nullptr);
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::MemoryBarrier, barrier,
        [](Transport::RingRecordHeader& header, std::uint8_t*) { header.kind = 0xFFFFu; }))
        << "the test peer failed to mutate and publish the accepted record bytes";

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK) << "server did not reject the mutated record promptly";
    // PH-1 (3): the opcode is the peer's byte, so the session child LATCHES by name (the
    // decoder's pre-gate asks the generated gate's question first) and closes with
    // kSessionLatchedExitCode instead of aborting in MGPipeWireProtocolFatal, whose line carries
    // no `Fatal{` marker at all. Red-once: drop AdmitsTheGeneratedGate and this reads -SIGABRT and
    // "protocol corruption applying <unknown opcode>" again.
    EXPECT_EQ(exitCode, MobileGL::MG_Remote::kSessionLatchedExitCode)
        << "the malformed opcode did not latch the session by name";
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    EXPECT_NE(log.find("Fatal{ProtocolCorruption, \"opcode\"} got=65535"), std::string::npos)
        << "the server process exited without the named opcode fault";
    EXPECT_NE(log.find("SessionLatch{ProtocolCorruption}"), std::string::npos)
        << "the fault was not latched";
    EXPECT_EQ(log.find("protocol corruption applying"), std::string::npos)
        << "the record reached the generated gate's unnamed death";
    client.Stop();
}

TEST(ServerSpawnTest, RawPeerOutOfRangeRenderStateCsoSlotGetsNamedProtocolCorruption) {
    const std::string endpoint = Endpoint("bad-renderstate-slot");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-renderstate-slot", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    auto& client = Client::ClientSessionInstance();
    MobileGL::MG_Pipe::MGPRenderStateDesc desc{};
    desc.Cso = {7u, 1u};
    desc.ChunkMask = MobileGL::MG_Pipe::MGPipeRenderStateChunkDetail::kAllPipelineHalfBits;
    const auto chunkBytes = MobileGL::MG_Pipe::MGPipePipelineChunkBlobBytes(desc.ChunkMask);
    std::vector<std::uint8_t> chunks(chunkBytes, 0x5A);
    desc.Blob = client.Encoder().StageBytes(chunks.data(), chunks.size());
    const MobileGL::Uint32 encoderAcceptedSlot = desc.Cso.Slot;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::CreateRenderState, desc,
        [](Transport::RingRecordHeader&, std::uint8_t* payloadBytes) {
            auto* wireDesc = reinterpret_cast<MobileGL::MG_Pipe::MGPRenderStateDesc*>(payloadBytes);
            wireDesc->Cso.Slot = MobileGL::MG_Pipe::kMGPipeMaxRenderStateCsoSlots;
        })) << "the test peer failed to mutate and publish the encoder-accepted record bytes";
    EXPECT_LT(encoderAcceptedSlot, MobileGL::MG_Pipe::kMGPipeMaxRenderStateCsoSlots);

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK) << "server did not reject the mutated record promptly";
    EXPECT_EQ(exitCode, -SIGABRT) << "the malformed CSO slot did not reach the server applier";
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    EXPECT_NE(log.find("Fatal{ProtocolCorruption, \"CreateRenderState.Cso.Slot\"}"),
              std::string::npos)
        << "the server process exited without the named protocol corruption diagnostic";
    client.Stop();
}

TEST(ServerSpawnTest, RawPeerOversizedReadbackGetsErrorAndSessionContinues) {
    const std::string endpoint = Endpoint("bad-readback-size");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-readback-size", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    auto& client = Client::ClientSessionInstance();
    MobileGL::MG_Pipe::MGPReadbackInfo info{};
    info.Res = MobileGL::MG_Pipe::kMGPipeNullHandle;
    info.Box = MobileGL::MG_Pipe::MGPBox{0, 0, 0, 1, 1, 1};
    info.Format = GL_RGBA;
    info.Type = GL_UNSIGNED_BYTE;
    info.DstSize = 4;

    const std::uint64_t maxReplyBytes = client.MaxReplyBytes();
    ASSERT_GT(maxReplyBytes, 0u);
    std::uint64_t readSeq = Wire::kInvalidSeq;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::ReadPixels, info,
        [maxReplyBytes](Transport::RingRecordHeader&, std::uint8_t* payloadBytes) {
            auto* wireInfo = reinterpret_cast<MobileGL::MG_Pipe::MGPReadbackInfo*>(payloadBytes);
            const auto width = static_cast<MobileGL::Uint32>(maxReplyBytes / 4 + 1);
            wireInfo->Box.W = width;
            wireInfo->Box.H = 1;
            wireInfo->DstSize = static_cast<std::uint64_t>(width) * 4;
        }, &readSeq)) << "the peer failed to mutate and publish the encoder-accepted readback";
    ASSERT_NE(readSeq, Wire::kInvalidSeq);
    ASSERT_EQ(client.WaitForApplied(readSeq, 5000), Transport::SessionWait::Reached);

    MobileGL::Int32 status = Wire::ReplySink::kStatusDeclined;
    std::uint64_t replySize = 99;
    ASSERT_TRUE(client.ReadReply(readSeq, nullptr, 0, &status, &replySize));
    EXPECT_EQ(status, Wire::ReplySink::kStatusError);
    EXPECT_EQ(replySize, 0u);

    // A valid follow-up record proves this refusal answered the slot without ending the
    // server session or stranding the command consumer.
    MobileGL::MG_Pipe::MGPMemoryBarrier barrier{};
    barrier.Bits = 0x2000u;
    const auto followupSeq = client.EmitAndWait(
        MobileGL::MG_Pipe::MGPWireOp::MemoryBarrier, &barrier, sizeof(barrier),
        nullptr, 0, nullptr, 0, nullptr);
    ASSERT_NE(followupSeq, Wire::kInvalidSeq);
    EXPECT_EQ(client.WaitForApplied(followupSeq, 5000), Transport::SessionWait::Reached);
    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0);
}

TEST(ServerSpawnTest, RawPeerTextureRunPastDeclaredLevelBoundIsNamedProtocolCorruption) {
    const std::string endpoint = Endpoint("bad-texture-run-bound");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-texture-run-bound", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    auto& client = Client::ClientSessionInstance();
    MobileGL::MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = {73u, 1u};
    desc.Target = static_cast<MobileGL::Uint8>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2D);
    desc.StorageKind = static_cast<MobileGL::Uint8>(MobileGL::TextureStorageType::Mipmap);
    desc.InternalFormat = static_cast<MobileGL::Uint32>(MobileGL::TextureInternalFormat::RGBA8);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.ArrayLayers = 1;
    desc.Levels = 1;
    desc.Samples = 1;

    MobileGL::Int32 status = Wire::ReplySink::kStatusError;
    auto seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceCreate, &desc, sizeof(desc),
                                  nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    MobileGL::MG_Pipe::MGPResourceDesc respecify = desc;
    respecify.HasDefinedContent = 1;
    MobileGL::MG_Pipe::MGPipeSetRespecifiedLevel(
        respecify, MobileGL::MG_Pipe::MGPipePackSubDataTarget(
                       static_cast<MobileGL::Uint32>(respecify.Target),
                       static_cast<MobileGL::Uint32>(MobileGL::TextureUploadTarget::Texture2D)),
        0, 4, 4, 1);
    seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceRespecify, &respecify,
                             sizeof(respecify), nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    // Reserve the post-mutation 96-byte stage run, but declare an encoder-accepted 48-byte
    // 4x3 box from Y=1. The mutation changes only the declared run geometry/size.
    std::vector<std::uint8_t> staged(96, 0x5A);
    MobileGL::MG_Pipe::MGPSubData upload{};
    upload.Res = desc.Resource;
    upload.Target = MobileGL::MG_Pipe::MGPipePackSubDataTarget(
        static_cast<MobileGL::Uint32>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2D),
        static_cast<MobileGL::Uint32>(MobileGL::TextureUploadTarget::Texture2D));
    upload.Level = 0;
    upload.UnionBox = MobileGL::MG_Pipe::MGPBox{0, 1, 0, 4, 3, 1};
    upload.RegionCount = 1;
    upload.Blob = client.Encoder().StageBytes(staged.data(), staged.size());
    upload.Blob.Size = 48;
    upload.LevelWidth = 4;
    upload.LevelHeight = 4;
    upload.LevelDepth = 1;
    MobileGL::MG_Pipe::MGPSubRegion region{};
    region.Y = 1;
    region.W = 4;
    region.H = 3;
    region.D = 1;
    region.SrcOffset = 0;
    region.SrcRowStride = 16;
    region.SrcSliceStride = 64;
    const Wire::WireTail tail{&region, sizeof(region)};
    std::uint64_t uploadSeq = Wire::kInvalidSeq;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::ResourceSubData, upload,
        [](Transport::RingRecordHeader&, std::uint8_t* payloadBytes) {
            auto* wireUpload = reinterpret_cast<MobileGL::MG_Pipe::MGPSubData*>(payloadBytes);
            wireUpload->Blob.Size = 96;
            auto* wireRegion = reinterpret_cast<MobileGL::MG_Pipe::MGPSubRegion*>(
                payloadBytes + sizeof(MobileGL::MG_Pipe::MGPSubData));
            wireRegion->SrcRowStride = 32;
            wireRegion->SrcSliceStride = 96;
        }, &uploadSeq, &tail, 1))
        << "the peer failed to publish the encoder-accepted texture run";
    ASSERT_NE(uploadSeq, Wire::kInvalidSeq);

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK) << "server did not reject the out-of-level run promptly";
    EXPECT_EQ(exitCode, -SIGABRT);
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    const auto firstProtocolFatal = log.find("Fatal{ProtocolCorruption");
    const auto boundFatal = log.find("Fatal{ProtocolCorruption, \"StagedTextureStore.CopyRunInto\"}");
    EXPECT_NE(boundFatal, std::string::npos)
        << "the server did not reject the run at the declared level byte bound";
    EXPECT_EQ(firstProtocolFatal, boundFatal)
        << "the run was refused before it reached the staged-level byte bound";
    client.Stop();
}

TEST(ServerSpawnTest, RawPeerTextureSubDataMustMatchTheAcceptedMutableLevelExtent) {
    const std::string endpoint = Endpoint("bad-texture-level-extent");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-texture-level-extent", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    auto& client = Client::ClientSessionInstance();
    MobileGL::MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = {74u, 1u};
    desc.Target = static_cast<MobileGL::Uint8>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2D);
    desc.StorageKind = static_cast<MobileGL::Uint8>(MobileGL::TextureStorageType::Mipmap);
    desc.InternalFormat = static_cast<MobileGL::Uint32>(MobileGL::TextureInternalFormat::RGBA8);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.ArrayLayers = 1;
    desc.Levels = 1;
    desc.Samples = 1;
    MobileGL::Int32 status = Wire::ReplySink::kStatusError;
    auto seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceCreate, &desc, sizeof(desc),
                                  nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    MobileGL::MG_Pipe::MGPResourceDesc respecify = desc;
    respecify.HasDefinedContent = 1;
    MobileGL::MG_Pipe::MGPipeSetRespecifiedLevel(
        respecify, MobileGL::MG_Pipe::MGPipePackSubDataTarget(
                       static_cast<MobileGL::Uint32>(respecify.Target),
                       static_cast<MobileGL::Uint32>(MobileGL::TextureUploadTarget::Texture2D)),
        0, 4, 4, 1);
    seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceRespecify, &respecify,
                             sizeof(respecify), nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    std::vector<std::uint8_t> staged(64, 0x5A);
    MobileGL::MG_Pipe::MGPSubData upload{};
    upload.Res = desc.Resource;
    upload.Target = MobileGL::MG_Pipe::MGPipePackSubDataTarget(
        static_cast<MobileGL::Uint32>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2D),
        static_cast<MobileGL::Uint32>(MobileGL::TextureUploadTarget::Texture2D));
    upload.Level = 0;
    upload.UnionBox = MobileGL::MG_Pipe::MGPBox{0, 0, 0, 4, 4, 1};
    upload.Blob = client.Encoder().StageBytes(staged.data(), staged.size());
    upload.LevelWidth = 4;
    upload.LevelHeight = 4;
    upload.LevelDepth = 1;
    std::uint64_t uploadSeq = Wire::kInvalidSeq;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::ResourceSubData, upload,
        [](Transport::RingRecordHeader&, std::uint8_t* payloadBytes) {
            auto* wireUpload = reinterpret_cast<MobileGL::MG_Pipe::MGPSubData*>(payloadBytes);
            // Still within the device limit and still contains the dirty box; the server must
            // refuse it because ResourceRespecify accepted 4x4 for this exact level.
            wireUpload->LevelWidth = 8;
        }, &uploadSeq))
        << "the peer failed to publish the encoder-accepted texture record";
    ASSERT_NE(uploadSeq, Wire::kInvalidSeq);

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK) << "server did not reject a changed mutable-level extent promptly";
    EXPECT_EQ(exitCode, -SIGABRT);
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    const auto firstProtocolFatal = log.find("Fatal{ProtocolCorruption");
    const auto extentFatal = log.find("Fatal{ProtocolCorruption, \"StagedTextureStore.LevelExtent\"}");
    EXPECT_NE(extentFatal, std::string::npos)
        << "server did not refuse the upload extent that disagreed with ResourceRespecify";
    EXPECT_EQ(firstProtocolFatal, extentFatal)
        << "the record was refused before staged-level extent matching";
    client.Stop();
}

TEST(ServerSpawnTest, RawPeerMultisampleTextureSubDataIsNamedProtocolCorruption) {
    const std::string endpoint = Endpoint("bad-multisample-texture-subdata");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-multisample-texture-subdata", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    auto& client = Client::ClientSessionInstance();
    MobileGL::MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = {75u, 1u};
    desc.Target = static_cast<MobileGL::Uint8>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2DMS);
    desc.StorageKind = static_cast<MobileGL::Uint8>(MobileGL::TextureStorageType::Mipmap);
    desc.InternalFormat = static_cast<MobileGL::Uint32>(MobileGL::TextureInternalFormat::RGBA8);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.ArrayLayers = 1;
    desc.Levels = 1;
    desc.Samples = 4;
    desc.Immutable = 1;
    MobileGL::Int32 status = Wire::ReplySink::kStatusError;
    auto seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceCreate, &desc, sizeof(desc),
                                  nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    MobileGL::MG_Pipe::MGPResourceDesc respecify = desc;
    respecify.HasDefinedContent = 1;
    seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceRespecify, &respecify,
                             sizeof(respecify), nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    std::vector<std::uint8_t> staged(64, 0x5A);
    MobileGL::MG_Pipe::MGPSubData upload{};
    upload.Res = desc.Resource;
    upload.Target = MobileGL::MG_Pipe::MGPipePackSubDataTarget(
        static_cast<MobileGL::Uint32>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2DMS),
        static_cast<MobileGL::Uint32>(MobileGL::TextureUploadTarget::Texture2DMultisample));
    upload.Level = 0;
    upload.UnionBox = MobileGL::MG_Pipe::MGPBox{0, 0, 0, 4, 4, 1};
    upload.Blob = client.Encoder().StageBytes(staged.data(), staged.size());
    upload.LevelWidth = 4;
    upload.LevelHeight = 4;
    upload.LevelDepth = 1;
    std::uint64_t uploadSeq = Wire::kInvalidSeq;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::ResourceSubData, upload,
        [](Transport::RingRecordHeader&, std::uint8_t*) {}, &uploadSeq));
    ASSERT_NE(uploadSeq, Wire::kInvalidSeq);

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK) << "server accepted an illegal multisample subimage record";
    EXPECT_EQ(exitCode, -SIGABRT);
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    const auto firstProtocolFatal = log.find("Fatal{ProtocolCorruption");
    const auto targetFatal = log.find("Fatal{ProtocolCorruption, \"StagedTextureStore.Target\"}");
    EXPECT_NE(targetFatal, std::string::npos)
        << "server did not refuse multisample bytes through the staged texture target gate";
    EXPECT_EQ(firstProtocolFatal, targetFatal)
        << "multisample record was refused before the staged target gate";
    client.Stop();
}

TEST(ServerSpawnTest, RawPeerNoncanonicalMutableExtentCarrierIsRefused) {
    const std::string endpoint = Endpoint("bad-respecify-extent-carrier");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-respecify-extent-carrier", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    auto& client = Client::ClientSessionInstance();
    MobileGL::MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = {76u, 1u};
    desc.Target = static_cast<MobileGL::Uint8>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2D);
    desc.StorageKind = static_cast<MobileGL::Uint8>(MobileGL::TextureStorageType::Mipmap);
    desc.InternalFormat = static_cast<MobileGL::Uint32>(MobileGL::TextureInternalFormat::RGBA8);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.ArrayLayers = 1;
    desc.Levels = 1;
    desc.Samples = 1;
    MobileGL::Int32 status = Wire::ReplySink::kStatusError;
    auto seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceCreate, &desc, sizeof(desc),
                                  nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    MobileGL::MG_Pipe::MGPResourceDesc respecify = desc;
    respecify.HasDefinedContent = 1;
    MobileGL::MG_Pipe::MGPipeSetRespecifiedLevel(
        respecify, MobileGL::MG_Pipe::MGPipePackSubDataTarget(
                       static_cast<MobileGL::Uint32>(respecify.Target),
                       static_cast<MobileGL::Uint32>(MobileGL::TextureUploadTarget::Texture2D)),
        0, 4, 4, 1);
    std::uint64_t respecifySeq = Wire::kInvalidSeq;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::ResourceRespecify, respecify,
        [](Transport::RingRecordHeader&, std::uint8_t* payloadBytes) {
            auto* wireDesc = reinterpret_cast<MobileGL::MG_Pipe::MGPResourceDesc*>(payloadBytes);
            wireDesc->BufSize |= (std::uint64_t{1} << 32);
        }, &respecifySeq));
    ASSERT_NE(respecifySeq, Wire::kInvalidSeq);

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK) << "server accepted a noncanonical mip extent carrier";
    EXPECT_EQ(exitCode, -SIGABRT);
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    const auto firstProtocolFatal = log.find("Fatal{ProtocolCorruption");
    const auto carrierFatal = log.find("Fatal{ProtocolCorruption, \"ResourceRespecify.ExtentCarrier\"}");
    EXPECT_NE(carrierFatal, std::string::npos)
        << "server did not reject high BufSize bits in the mutable extent carrier";
    EXPECT_EQ(firstProtocolFatal, carrierFatal)
        << "carrier was refused before the canonicality gate";
    client.Stop();
}

TEST(ServerSpawnTest, RawPeerWholeImageRespecifyCannotSmuggleBufferRangeFields) {
    const std::string endpoint = Endpoint("bad-whole-respecify-buffer-range");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-whole-respecify-buffer-range", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    auto& client = Client::ClientSessionInstance();
    MobileGL::MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = {77u, 1u};
    desc.Target = static_cast<MobileGL::Uint8>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2D);
    desc.StorageKind = static_cast<MobileGL::Uint8>(MobileGL::TextureStorageType::Mipmap);
    desc.InternalFormat = static_cast<MobileGL::Uint32>(MobileGL::TextureInternalFormat::RGBA8);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.ArrayLayers = 1;
    desc.Levels = 1;
    desc.Samples = 1;
    MobileGL::Int32 status = Wire::ReplySink::kStatusError;
    auto seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceCreate, &desc, sizeof(desc),
                                  nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    MobileGL::MG_Pipe::MGPResourceDesc respecify = desc;
    respecify.HasDefinedContent = 1;
    std::uint64_t respecifySeq = Wire::kInvalidSeq;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::ResourceRespecify, respecify,
        [](Transport::RingRecordHeader&, std::uint8_t* payloadBytes) {
            auto* wireDesc = reinterpret_cast<MobileGL::MG_Pipe::MGPResourceDesc*>(payloadBytes);
            wireDesc->BufOffset = 1;
        }, &respecifySeq));
    ASSERT_NE(respecifySeq, Wire::kInvalidSeq);

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK) << "server accepted a buffer range on a whole image texture";
    EXPECT_EQ(exitCode, -SIGABRT);
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    const auto firstProtocolFatal = log.find("Fatal{ProtocolCorruption");
    const auto rangeFatal = log.find("Fatal{ProtocolCorruption, \"ResourceRespecify.BufferRange\"}");
    EXPECT_NE(rangeFatal, std::string::npos)
        << "server did not reject BufOffset/BufSize on the whole image descriptor";
    EXPECT_EQ(firstProtocolFatal, rangeFatal)
        << "descriptor was refused before the canonical buffer-range gate";
    client.Stop();
}

TEST(ServerSpawnTest, RawPeerFramebufferAttachmentPastBackendSlotLimitIsNamedProtocolCorruption) {
    const std::string endpoint = Endpoint("bad-fbo-texture-slot");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-fbo-texture-slot", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);
    auto& client = Client::ClientSessionInstance();

    const MobileGL::MG_Pipe::MGPipeHandle fbo{20u, 1u};
    const MobileGL::MG_Pipe::MGPipeHandle outOfRange{1u << 20, 1u};
    const auto framebuffer = RawColorAttachmentFramebuffer(fbo, outOfRange, 1);
    ASSERT_TRUE(PublishRawRecordAndWait(client, MobileGL::MG_Pipe::MGPWireOp::SetFramebufferState,
                                        framebuffer));
    const auto clear = RawColorClear(fbo);
    std::uint64_t clearSeq = Wire::kInvalidSeq;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::Clear, clear,
        [](Transport::RingRecordHeader&, std::uint8_t*) {}, &clearSeq));
    ASSERT_NE(clearSeq, Wire::kInvalidSeq);
    // This final clear is expected to kill the server, so there is no applied watermark to wait on.

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK)
        << "server did not reject the FBO attachment whose texture slot exceeded BackendSlotTable's cap";
    EXPECT_EQ(exitCode, -SIGABRT);
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    const auto firstProtocolFatal = log.find("Fatal{ProtocolCorruption");
    const auto slotFatal = log.find("Fatal{ProtocolCorruption, \"BackendSlotTable.HandleSlot\"}");
    EXPECT_NE(slotFatal, std::string::npos) << "server did not name the backend texture slot bound";
    EXPECT_EQ(firstProtocolFatal, slotFatal)
        << "the peer record was refused before the backend twin adoption guard";
    client.Stop();
}

TEST(ServerSpawnTest, RawPeerFramebufferAttachmentBackwardGenerationIsNamedProtocolCorruption) {
    const std::string endpoint = Endpoint("bad-fbo-texture-generation");
    const std::string logBase = endpoint + ".log";
    ScopedEnvironment logPath("MOBILEGL_LOG_FILE_PATH", logBase);
    Session session;
    ScopedSessionCleanup cleanup{session};
    ASSERT_TRUE(Bring("bad-fbo-texture-generation", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);
    auto& client = Client::ClientSessionInstance();

    const MobileGL::MG_Pipe::MGPipeHandle texture{37u, 2u};
    MobileGL::MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = texture;
    desc.Target = static_cast<MobileGL::Uint8>(MobileGL::MG_Pipe::MGPipeResourceTarget::Tex2D);
    desc.StorageKind = static_cast<MobileGL::Uint8>(MobileGL::TextureStorageType::Mipmap);
    desc.BindMask = MobileGL::MG_Pipe::kMGPipeBindRenderTarget;
    desc.InternalFormat = static_cast<MobileGL::Uint32>(MobileGL::TextureInternalFormat::RGBA8);
    desc.Width = desc.Height = desc.Depth = 1;
    desc.ArrayLayers = desc.Levels = desc.Samples = 1;
    MobileGL::Int32 status = Wire::ReplySink::kStatusError;
    auto seq = client.EmitAndWait(MobileGL::MG_Pipe::MGPWireOp::ResourceCreate, &desc, sizeof(desc),
                                  nullptr, 0, nullptr, 0, &status);
    ASSERT_NE(seq, Wire::kInvalidSeq);
    ASSERT_EQ(status, Wire::ReplySink::kStatusOk);
    ASSERT_EQ(client.WaitForApplied(seq, 5000), Transport::SessionWait::Reached);

    const MobileGL::MG_Pipe::MGPipeHandle fbo{20u, 1u};
    auto current = RawColorAttachmentFramebuffer(fbo, texture, 1);
    ASSERT_TRUE(PublishRawRecordAndWait(client, MobileGL::MG_Pipe::MGPWireOp::SetFramebufferState,
                                        current));
    const auto clear = RawColorClear(fbo);
    ASSERT_TRUE(PublishRawRecordAndWait(client, MobileGL::MG_Pipe::MGPWireOp::Clear, clear));

    // PipeApply accepts this attachment verbatim; the server's already-live {37,2} twin is
    // the only fact that lets the apply-side adoption guard identify the stale generation.
    current.Color[0].Res.Gen = 1;
    current.ContentHash = 2;
    ASSERT_TRUE(PublishRawRecordAndWait(client, MobileGL::MG_Pipe::MGPWireOp::SetFramebufferState,
                                        current));
    std::uint64_t badClearSeq = Wire::kInvalidSeq;
    ASSERT_TRUE(EncodeThenMutateHeaderAndPublish(
        client, MobileGL::MG_Pipe::MGPWireOp::Clear, clear,
        [](Transport::RingRecordHeader&, std::uint8_t*) {}, &badClearSeq));
    ASSERT_NE(badClearSeq, Wire::kInvalidSeq);

    int exitCode = -1;
    const auto reaped = Server::ReapServer(session.server, 5000, &exitCode);
    if (reaped != MOBILEGL_OK) {
        client.Stop();
        if (session.server.pid > 0) {
            ::kill(session.server.pid, SIGKILL);
            (void)Server::ReapServer(session.server, 5000, &exitCode);
        }
    }
    ASSERT_EQ(reaped, MOBILEGL_OK)
        << "server did not reject the FBO attachment whose generation moved backwards";
    EXPECT_EQ(exitCode, -SIGABRT);
    const std::string log = MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    const auto firstProtocolFatal = log.find("Fatal{ProtocolCorruption");
    const auto generationFatal = log.find("Fatal{ProtocolCorruption, \"BackendSlotTable.Generation\"}");
    EXPECT_NE(generationFatal, std::string::npos)
        << "server did not name the stale attachment generation";
    EXPECT_EQ(firstProtocolFatal, generationFatal)
        << "the peer record was refused before the backend twin generation guard";
    client.Stop();
}

TEST(ServerSpawnTest, StartsAServerProcessAndHandshakesAcrossIt) {
    const int before = Server::CountOwnChildren();
    ASSERT_GE(before, 0) << "/proc unreadable; the process-tree gate cannot run here";

    Session session;
    ASSERT_TRUE(Bring("handshake", &session));
    ASSERT_GT(session.server.pid, 0);
    EXPECT_NE(session.server.pid, static_cast<int>(::getpid()))
        << "that is this process, not a separate one";
    EXPECT_EQ(Server::CountOwnChildren(), before + 1);

    // THE WHOLE POINT. Hello crosses, Welcome comes back with four SegmentRefs,
    // six descriptors arrive over SCM_RIGHTS, and this process maps memory that
    // another process created. Nothing here was inherited.
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);
    EXPECT_TRUE(Client::ClientSession::Active() != nullptr)
        << "a handshake that returned OK must leave the session emit-armed";

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0) << "the server must exit cleanly on EOF, not be killed";
    EXPECT_EQ(Server::CountOwnChildren(), before) << "a zombie would still be counted here";
}

TEST(ServerSpawnTest, AnEglControlOpCrossesToTheOtherProcessAndAnswers) {
    // `cp`. All twelve Server* EGL forwarders funnel through
    // ServerLoop::RunSurfaceControlFrame, so this exercises the ONE seam that
    // sends them: encode -> control socket -> the server's pump ->
    // ServerApplyWireSurfaceOp -> its one-slot mailbox -> the apply thread ->
    // SurfaceReply -> back here.
    //
    // Under inproc this same call posts into a mailbox in THIS process. The
    // assertion that it is not doing that here is the server's pid in the
    // handshake plus the fact that ServerLoopInstance() in this process was
    // never started - a local post would find no apply thread and time out.
    Session session;
    ASSERT_TRUE(Bring("eglop", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    // eglInitialize's forwarder: the simplest op that has a real answer.
    EGLint major = -1;
    EGLint minor = -1;
    const bool ok = Server::ServerInitializeEGLDisplay(EGL_NO_DISPLAY, &major, &minor);

    // WHAT IS ASSERTED IS THE ROUND TRIP, not the EGL result. A headless CI
    // machine has no display to initialise, so `ok` is expected to be false -
    // and that is fine, because `cp` is done when the op reaches the other
    // process and an answer comes back.
    //
    // THE OUT-PARAMETERS ARE THE PROOF. ServerInitializeEGLDisplay writes them
    // "whenever the dispatch ran, success or not" (ServerLoop.cpp's own comment
    // at the forwarder), so they move off -1 if and only if a SurfaceReply came
    // back from the other process. A transport failure returns before the
    // write-back and leaves both at -1; that is the difference this asserts.
    EXPECT_NE(major, -1) << "no SurfaceReply came back: the op never reached the other process";
    EXPECT_NE(minor, -1) << "no SurfaceReply came back: the op never reached the other process";
    (void)ok;

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
}

namespace {
    // p7/spawnhang. The client's two reply budgets, shortened for one case and put back after it,
    // and the harness's headless EGL for the server it starts (EventForfeitPeerTest's PinHeadlessEgl:
    // the first surface creation runs a real eglInitialize, and a runner has no DISPLAY).
    struct ShortReplyBudgets {
        MobileGL::Uint32 steady = MobileGL::MG_Config::Ipc.ControlTimeoutMs;
        MobileGL::Uint32 cold = MobileGL::MG_Config::Ipc.ColdStartMs;
        explicit ShortReplyBudgets(MobileGL::Uint32 ms) {
            MobileGL::MG_Config::Ipc.ControlTimeoutMs = ms;
            MobileGL::MG_Config::Ipc.ColdStartMs = ms;
        }
        ~ShortReplyBudgets() {
            MobileGL::MG_Config::Ipc.ControlTimeoutMs = steady;
            MobileGL::MG_Config::Ipc.ColdStartMs = cold;
        }
    };

    std::string HeadlessEglPlatform() {
        const char* pinned = std::getenv("EGL_PLATFORM");
        return pinned != nullptr ? pinned : "surfaceless";
    }

    Server::SurfaceControlFrame PbufferCreation() {
        Server::SurfaceControlFrame frame{};
        frame.kind = Server::SurfaceControlOp::CreatePbufferSurface;
        frame.surface = 1;
        frame.width = 16;
        frame.height = 16;
        return frame;
    }

    long long MillisecondsSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    }
} // namespace

// p7/spawnhang: retrace-split run 35912252677, in miniature and in seconds instead of twenty.
//
// A spawned server brings its native backend up lazily, INSIDE the first surface creation
// (eglInitialize). On a cold CI runner that read a software rasteriser off disk for ~20 s - the
// whole MOBILEGL_IPC_COLD_START_MS - and the client, which measured the op's DURATION against a
// budget meant for SILENCE, reported the server ALIVE BUT SILENT and failed the pbuffer with
// EGL_BAD_ALLOC while the server was about to answer. The server now reports progress while its
// apply thread runs an op it has taken (Wire::SurfaceProgress), and each report restarts the
// client's budget.
//
// Here the server's own test lever holds its first CreatePbufferSurface for 3000 ms and the client's
// budgets are 1000 ms - the same shape as 20 s of bring-up against a 20 s budget, three times over.
// Against the client and server as they were (one untimed wait on the server, one deadline on the
// client) the op comes back TIMEOUT at ~1000 ms: the CI failure, named the way CI named it. With
// the progress reports it comes back answered, after the lever let it go.
TEST(ServerSpawnTest, AColdBringUpThatOutlastsTheReplyBudgetIsWaitedForBecauseTheServerReportsProgress) {
    const ScopedEnvironment lever("MOBILEGL_TEST_DELAY_FIRST_BRINGUP_MS", "3000");
    const ScopedEnvironment headless("EGL_PLATFORM", HeadlessEglPlatform());
    const ShortReplyBudgets budgets(1000);

    Session session;
    ASSERT_TRUE(Bring("coldprogress", &session));
    ScopedSessionCleanup cleanup{session};
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    Server::SurfaceControlFrame frame = PbufferCreation();
    const auto start = std::chrono::steady_clock::now();
    const MobileGLResult rc = Client::ClientSessionInstance().RunRemoteSurfaceControlFrame(frame);
    const long long elapsedMs = MillisecondsSince(start);

    // THE ROUND TRIP, not the EGL answer: whether a headless runner can make a pbuffer is not
    // this case's question (`ok` is whatever the driver said); whether the reply was WAITED FOR is.
    EXPECT_EQ(rc, MOBILEGL_OK)
        << "the op came back rc=" << static_cast<int>(rc) << " after " << elapsedMs
        << " ms: the client gave up on a server that was still running it - the retrace-split "
           "failure (\"ALIVE BUT SILENT - no SurfaceReply for CreatePbufferSurface seq 2\")";
    if (rc == MOBILEGL_OK) {
        EXPECT_GE(elapsedMs, 3000)
            << "the reply came back before the server's lever released the op, so the lever did not "
               "hold the bring-up and this case proved nothing about waiting for one";
    }
    EXPECT_FALSE(Client::ClientSession::DeviceLost()) << "a slow server is not a lost device (5.4)";
}

// The other half, and the reason the budget still exists: a server that is ALIVE BUT FROZEN reports
// nothing, so the same op against it still ends at the budget, by name, rather than waiting on
// progress that never comes. SIGSTOP freezes the whole process - the pump that would report as well
// as the apply thread - and keeps every descriptor open, so it is not a hangup either.
TEST(ServerSpawnTest, AFrozenServerReportsNoProgressAndTheOpStillTimesOutAtTheBudget) {
    const ScopedEnvironment headless("EGL_PLATFORM", HeadlessEglPlatform());
    const ShortReplyBudgets budgets(1000);

    Session session;
    ASSERT_TRUE(Bring("frozenprogress", &session));
    ScopedSessionCleanup cleanup{session};
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);
    ASSERT_EQ(::kill(session.server.pid, SIGSTOP), 0);

    Server::SurfaceControlFrame frame = PbufferCreation();
    const auto start = std::chrono::steady_clock::now();
    const MobileGLResult rc = Client::ClientSessionInstance().RunRemoteSurfaceControlFrame(frame);
    const long long elapsedMs = MillisecondsSince(start);
    (void)::kill(session.server.pid, SIGCONT);

    EXPECT_EQ(rc, MOBILEGL_ERR_TIMEOUT)
        << "a frozen server was not timed out (rc=" << static_cast<int>(rc) << ")";
    EXPECT_LT(elapsedMs, 10000)
        << "took " << elapsedMs << " ms against a 1000 ms budget: something other than a progress "
           "report restarted the wait";
    EXPECT_FALSE(Client::ClientSession::DeviceLost()) << "a frozen server is not a lost device (5.4)";
}

TEST(ServerSpawnTest, TheHandshakeCarriesTwoDifferentProcessIdsAndNotTwoZeroes) {
    // CONTRACT-P6 4.3. Hello::pid was hard-coded 0 and Welcome::serverPid ECHOED it back, so the
    // handshake carried 0 and 0 and named nothing at all. Both now carry real ids, which makes
    // the pair the cheapest arm proof in the protocol: under spawn they MUST differ, and a
    // same-process session cannot produce two different values.
    //
    // ServerSession records the pid it welcomed; ClientSession knows the child it launched. The
    // assertion is that those two agree AND that neither is this process.
    Session session;
    ASSERT_TRUE(Bring("pids", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    const std::uint32_t self = static_cast<std::uint32_t>(::getpid());
    const std::uint32_t server = static_cast<std::uint32_t>(session.server.pid);
    EXPECT_NE(server, 0u) << "the launcher did not record a child pid";
    EXPECT_NE(server, self) << "that is this process, so there is nothing to prove";

    // THE VALUE THAT CROSSED THE WIRE, not the one the launcher happens to remember. Welcome
    // carries the server's own getpid(), so this equality says the handshake reached the process
    // we started - rather than some other listener that answered on the name - and it says it
    // from the server's own mouth.
    EXPECT_EQ(Client::ClientSessionInstance().PeerServerPid(), server)
        << "Welcome::serverPid does not name the process we launched";
    EXPECT_NE(Client::ClientSessionInstance().PeerServerPid(), self)
        << "the peer stated OUR pid; serverPid is echoing the client again (4.3)";

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
}

TEST(ServerSpawnTest, AKilledServerLatchesDeviceLostFromTheHangupAndNotFromADeadline) {
    // `dl`, and exit gate S2: kill -9 the server mid-session and the client must find out FROM A
    // DESCRIPTOR. This is the red-once for CONTRACT-P6 D5c, whose whole content is that the
    // client's own bell CANNOT witness the death - it holds both ends of that socketpair, so the
    // fact has to come from somewhere else.
    //
    // WHAT MAKES THIS FALSIFIABLE IS THE CLOCK, not the boolean. A timeout would eventually
    // report "gone" too, and 5.4 forbids that: under P5e run-ahead a server one frame behind is
    // the INTENDED steady state, so anything armed by a deadline fires on a healthy session under
    // load. The bound below is therefore deliberately far under every wait in the system - the
    // verb barrier is 120000 ms and the control reply 5000 ms - so a pass here cannot be a
    // timeout wearing the right answer.
    Session session;
    ASSERT_TRUE(Bring("devicelost", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);
    EXPECT_FALSE(Client::ClientSession::DeviceLost())
        << "a healthy session must not read as a lost device";

    Transport::Doorbell* bell = Client::ClientSessionInstance().SelfDoorbellForTest();
    ASSERT_NE(bell, nullptr);
    EXPECT_FALSE(bell->PeerHungUp()) << "nothing has hung up yet";

    ASSERT_EQ(::kill(session.server.pid, SIGKILL), 0);

    // Park with a SHORT budget, repeatedly, until the hangup lands. Each Park is what a real
    // waiter does; the loop exists because the kill is asynchronous and the first poll may win
    // the race. 200 x 10 ms is two seconds of patience against waits measured in minutes.
    const auto start = std::chrono::steady_clock::now();
    for (int attempt = 0; attempt < 200 && !bell->PeerHungUp(); ++attempt) {
        (void)bell->Park(10);
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start).count();

    EXPECT_TRUE(bell->PeerHungUp())
        << "the witness never reported the hangup; the bell is watching a descriptor this side "
           "also holds, which is the exact defect D5c names";
    EXPECT_TRUE(bell->Dead()) << "a peer that hung up is a dead bell";
    EXPECT_LT(elapsedMs, 3000)
        << "took " << elapsedMs << " ms: too slow to be a hangup and fast enough only for a "
           "descriptor, so this assertion is what separates the two mechanisms";

    // AND THE LATCH ITSELF. PeerHungUp is the cause; DeviceLost is the session-scoped
    // consequence, and it is what glGetGraphicsResetStatus reports.
    Client::ClientSessionInstance().LatchDeviceLost("the test killed the server");
    EXPECT_TRUE(Client::ClientSession::DeviceLost());

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    (void)Server::ReapServer(session.server, 5000, &exitCode);
}

TEST(ServerSpawnTest, AnOrderlyStopIsNotADeviceLoss) {
    // The session-level control: a clean shutdown must leave the latch disarmed, or every exit
    // would report a context reset and the status would mean nothing.
    //
    // NOTE WHAT THIS DOES *NOT* PROVE, because the first draft of this comment claimed it did
    // and the R-16 falsification caught the lie. Collapsing PeerHungUp() into Dead() leaves this
    // test green: Stop() kills the bell through CondVarDoorbell::Kill(), and under SPAWN the
    // self bell is a SocketDoorbell, which has no Kill at all - so Dead() is false here either
    // way. ADeadBellIsNotAlwaysAHungUpPeer below is the case that separates them.
    Session session;
    ASSERT_TRUE(Bring("orderly", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    Transport::Doorbell* bell = Client::ClientSessionInstance().SelfDoorbellForTest();
    ASSERT_NE(bell, nullptr);

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0) << "the server must have exited cleanly, not been killed";

    EXPECT_FALSE(bell->PeerHungUp())
        << "an orderly Stop() must not look like a peer that died";
    EXPECT_FALSE(Client::ClientSession::DeviceLost())
        << "a clean teardown armed the device-lost latch; every exit would report a reset";
}

TEST(ServerSpawnTest, ADeadBellIsNotAlwaysAHungUpPeer) {
    // THE ASSERTION THAT MAKES `dl`'s CAUSE/FACT SPLIT LOAD-BEARING, and it exists because the
    // session-level control above turned out not to. Park() latches m_dead for several reasons
    // that are NOT a peer death - POLLERR, POLLNVAL, an unrecognised revents - and each of them
    // is a fault in THIS process's descriptor. A latch armed from Dead() would report a lost
    // GPU for a bug in our own fd handling.
    //
    // Falsified by construction: make PeerHungUp() return Dead() and this goes red, which is
    // exactly what the contract's D5c distinction has to be able to do.
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    // ownsFds=false: the bell must not close what this test is about to invalidate underneath
    // it, or the destructor closes a descriptor number somebody else has since been given.
    Transport::SocketDoorbell bell(pair[0], pair[1], /*code=*/1, /*ownsFds=*/false);
    bell.SetDeathWitness(-1); // no witness: nothing here can legitimately report a hangup
    EXPECT_FALSE(bell.Dead());
    EXPECT_FALSE(bell.PeerHungUp());

    // Close the park end UNDER the bell. poll() then answers POLLNVAL, which Park treats as a
    // descriptor it must never poll again - dead, but not bereaved.
    ASSERT_EQ(::close(pair[0]), 0);
    (void)bell.Park(50);

    EXPECT_TRUE(bell.Dead()) << "a bell polling a closed fd cannot be anything but dead";
    EXPECT_FALSE(bell.PeerHungUp())
        << "our own descriptor went bad and the bell called it a peer death; a device-lost latch "
           "taken from this would report a lost GPU for a local fd bug";

    ::close(pair[1]);
}

TEST(ServerSpawnTest, AnAbstractEndpointNeedsNoWritableDirectoryAndLeavesNoFile) {
    // THE ONLY RENDEZVOUS THAT WORKS ON ANDROID, and the reason is not a preference.
    // An app has no writable /tmp; the first device run of the spawn retrace launched
    // its server, bind() had nowhere to put the node, and the client refused by name
    // 20 s later. ClientSession::StartSpawned mints '@' names now, so this pins the
    // shape that path depends on.
    //
    // TWO FACTS, and the second is what a filesystem endpoint cannot give: the
    // handshake works over it, and NOTHING IS LEFT BEHIND - no node to go stale, no
    // directory to be writable, no unlink to forget after a crash.
    const std::string endpoint = std::string("@mgl-abstract-") + std::to_string(::getpid());

    Session session;
    ASSERT_EQ(Server::LaunchServer(ServerImage(), endpoint, &session.server), MOBILEGL_OK);
    ASSERT_EQ(Transport::SocketTransport::ConnectTo(endpoint, 10000, session.client), MOBILEGL_OK);
    ASSERT_EQ(Handshake(session), MOBILEGL_OK)
        << "an abstract endpoint carried the connection but not the session";

    // A leading '@' is a NAME, not a path. If FillAddress had treated it as one, the
    // bind would have created a file literally called "@mgl-..." in the working
    // directory - which would still have worked here, and would have failed on the
    // one platform this exists for.
    EXPECT_NE(::access(endpoint.c_str(), F_OK), 0)
        << "an abstract name must leave no filesystem node: " << endpoint;

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0);
}

TEST(ServerSpawnTest, AnUnresolvableImageIsANamedRefusalAndStartsNothing) {
    const int before = Server::CountOwnChildren();

    // S1 (CONTRACT-P6 §9). The accident this prevents is the one
    // ConfigLoader.cpp names in its own comment: a lane that asked for spawn,
    // silently got monolith, and went green on the wrong arm.
    Server::LaunchedServer server;
    EXPECT_EQ(Server::LaunchServer("/nonexistent/libMobileGLServer.so", Endpoint("missing"),
                                   &server),
              MOBILEGL_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(server.pid, -1);
    EXPECT_EQ(Server::CountOwnChildren(), before) << "a refused launch must leave no process";
}

TEST(ServerSpawnTest, ConnectingToNothingIsANamedRefusalNotAHang) {
    // The other half of S1: the client's side. A rendezvous nobody is listening
    // on must be a bounded, named failure - never an unbounded wait, which is
    // what a caller would experience as the whole application freezing.
    std::unique_ptr<Transport::SocketTransport> client;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_NE(Transport::SocketTransport::ConnectTo(Endpoint("nobody"), 300, client), MOBILEGL_OK);
    EXPECT_FALSE(client);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              4000);
}

TEST(ServerSpawnTest, ADescriptorCrossesBetweenTheTwoProcesses) {
    // The mechanism the four shared segments ride on. Proving it now, on a pipe,
    // is what stops it being first exercised on the day the data plane needs it -
    // the same argument SessionRings.h makes about the attach half.
    //
    // NOTE it crosses the AUX connection, which is the second of the two the
    // client made. An fd offer is a sendmsg whose ancillary data rides with
    // specific bytes, so it may not share a socket with the framed control
    // stream: the frame reassembler and the descriptor receiver would race for
    // the same bytes.
    Session session;
    ASSERT_TRUE(Bring("scm", &session));

    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipeFds), 0);
    const std::string sideband = "SegmentRef{id=1,kind=Cmd}";
    EXPECT_EQ(session.client->ShareFd(pipeFds[0],
                                      MobileGLByteSpan{sideband.data(), sideband.size()}),
              MOBILEGL_OK);

    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
    int exitCode = -1;
    CloseAndReap(session, &exitCode);
}

#endif // !_WIN32
