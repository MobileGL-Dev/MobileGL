// MobileGL - MobileGL/MG_Test/Wire/PipeWireCodecTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package w1's suite: encoder -> SEG_CMD -> RingConsumer -> decoder -> the real
// MGPipeApply* free functions, with no session, no transport and no thread. s1 owns the
// session; this file owns the bytes.
//
// It links gtest rather than gtest_main and carries its own main(), for PipeInputsTest's
// reason: the R-2 arms report through MGLOG_F + std::abort, so a case that drives one FORKS
// and reads the Fatal line back out of a log file this process names before anything logs.
// NEVER EXPECT_DEATH - it re-runs the whole binary and would re-enter the applier's globals.
//
// WHAT A "ROUND TRIP" MEANS HERE. The decoder implements no semantics, so a case cannot
// assert on rendering; what it asserts is that the record crossed intact and that the arm
// reached the right consumer with the right arguments. For the five class-B verbs that is a
// recording WireVerbSink; for everything else it is the real applier, whose acceptance return
// comes back through the recording ReplySink on the record's own seq (R-3/R-5).

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Includes.h"

#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Transport/Ring.h>
#include <MG_Remote/Wire/PipeWireCodec.h>
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#define MGTEST_HAVE_FORK 1
#else
#include <process.h>
#define MGTEST_HAVE_FORK 0
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;
using namespace MobileGL::MG_Remote;
using namespace MobileGL::MG_Remote::Wire;
namespace Transport = MobileGL::MG_Remote::Transport;

namespace {

    std::string g_logPath;

    std::string ReadLog() {
        std::ifstream in(g_logPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    long ProcessId() {
#if defined(_WIN32)
        return static_cast<long>(::_getpid());
#else
        return static_cast<long>(::getpid());
#endif
    }

    // ---- the fixture ----------------------------------------------------------------
    //
    // Two rings over two byte arrays plus a SegmentTable that covers exactly those arrays.
    // "Exactly" is load-bearing: StageBytes asserts that the SEG_STAGE view resolves a staged
    // run to the same address the producer wrote it at, so a view installed over the wrong
    // base is a Fatal rather than a plausible pointer.
    class Wire2 {
    public:
        static constexpr std::uint64_t kCmdBytes = 64 * 1024;
        static constexpr std::uint64_t kStageBytes = 256 * 1024;

        Wire2() : m_cmdBytes(kCmdBytes), m_stageBytes(kStageBytes) {
            Transport::InitRingControl(m_control);
            m_cmd = Transport::RingProducer(&m_control, m_cmdBytes.data(), kCmdBytes,
                                            Transport::RingCursorSet::Cmd);
            m_stage = Transport::RingProducer(&m_control, m_stageBytes.data(), kStageBytes,
                                              Transport::RingCursorSet::Stage);
            m_consumer = Transport::RingConsumer(&m_control, m_cmdBytes.data(), kCmdBytes,
                                                 Transport::RingCursorSet::Cmd);
            m_segments.Install(kSegCmd, SegmentView{m_cmdBytes.data(), kCmdBytes});
            m_segments.Install(kSegStage, SegmentView{m_stageBytes.data(), kStageBytes});
            m_encoder = PipeWireEncoder(&m_control, &m_cmd, &m_stage, &m_segments);
            m_decoder = PipeWireDecoder(&m_control, &m_segments, &m_replies);
            m_decoder.SetVerbSink(&m_verbs);
        }

        PipeWireEncoder& Encoder() { return m_encoder; }
        PipeWireDecoder& Decoder() { return m_decoder; }
        SegmentTable& Segments() { return m_segments; }
        Transport::RingControl& Control() { return m_control; }
        Transport::RingProducer& Cmd() { return m_cmd; }
        Transport::RingConsumer& Consumer() { return m_consumer; }
        std::uint8_t* StageBase() { return m_stageBytes.data(); }

        // Pops one record and decodes it. Returns whether the decoder reported "applied";
        // `popped` says whether there was a record at all, so a case cannot pass because
        // nothing was there.
        bool PumpOne(bool* applied) {
            m_encoder.Publish();
            Transport::RingRecordView view{};
            bool corrupt = false;
            if (!m_consumer.Pop(view, &corrupt)) {
                return false;
            }
            if (corrupt) {
                return false;
            }
            const bool result = m_decoder.DecodeAndApply(view);
            m_consumer.PublishRetired();
            if (applied != nullptr) {
                *applied = result;
            }
            return true;
        }

        // ---- recorded answers ----
        struct Reply {
            std::uint64_t Seq = 0;
            std::int32_t Status = 0;
            std::vector<std::uint8_t> Bytes;
        };

        class Replies : public ReplySink {
        public:
            void PostReply(Uint64 seq, Int32 status, const void* bytes, Uint64 size) override {
                Reply r;
                r.Seq = seq;
                r.Status = status;
                if (bytes != nullptr && size != 0) {
                    const auto* p = static_cast<const std::uint8_t*>(bytes);
                    r.Bytes.assign(p, p + size);
                }
                All.push_back(std::move(r));
            }
            std::vector<Reply> All;
        };

        class Verbs : public WireVerbSink {
        public:
            Bool OnClear(const MGPClear& clear) override {
                Clears.push_back(clear);
                return true;
            }
            Bool OnBlit(const MGPBlit& blit) override {
                Blits.push_back(blit);
                return true;
            }
            Bool OnPresent(const MGPPresent& present) override {
                Presents.push_back(present);
                return true;
            }
            Bool OnReadPixels(const MGPReadbackInfo& info, Uint64 seq, ReplySink* replies) override {
                Readbacks.push_back(info);
                ReadbackSeqs.push_back(seq);
                if (replies != nullptr) {
                    const std::uint8_t pixels[4] = {1, 2, 3, 4};
                    replies->PostReply(seq, ReplySink::kStatusOk, pixels, sizeof(pixels));
                }
                return true;
            }
            Bool OnDrawVbo(const MGPDrawInfo& info, const MGPDrawRange* ranges,
                           const MGHostSpan* userIndices) override {
                Draws.push_back(info);
                DrawRanges.clear();
                for (Uint32 i = 0; i < info.NumDraws; ++i) {
                    DrawRanges.push_back(ranges[i]);
                }
                SawUserIndices = userIndices != nullptr;
                if (userIndices != nullptr) {
                    LastSpan = *userIndices;
                }
                return true;
            }
            std::vector<MGPClear> Clears;
            std::vector<MGPBlit> Blits;
            std::vector<MGPPresent> Presents;
            std::vector<MGPReadbackInfo> Readbacks;
            std::vector<Uint64> ReadbackSeqs;
            std::vector<MGPDrawInfo> Draws;
            std::vector<MGPDrawRange> DrawRanges;
            bool SawUserIndices = false;
            MGHostSpan LastSpan{};
        };

        Replies& Answers() { return m_replies; }
        Verbs& Sink() { return m_verbs; }

    private:
        Transport::RingControl m_control{};
        std::vector<std::uint8_t> m_cmdBytes;
        std::vector<std::uint8_t> m_stageBytes;
        Transport::RingProducer m_cmd;
        Transport::RingProducer m_stage;
        Transport::RingConsumer m_consumer;
        SegmentTable m_segments;
        PipeWireEncoder m_encoder;
        PipeWireDecoder m_decoder;
        Replies m_replies;
        Verbs m_verbs;
    };

    MGPipeHandle MakeHandle(Uint32 slot, Uint32 gen = 1) {
        MGPipeHandle handle{};
        handle.Slot = slot;
        handle.Gen = gen;
        return handle;
    }

    MGPHandleOnly HandleOnly(Uint32 slot, MGPipeKind kind) {
        MGPHandleOnly record{};
        record.Handle = MakeHandle(slot);
        record.Kind = static_cast<Uint32>(kind);
        return record;
    }

    class PipeWireCodecTest : public ::testing::Test {
    protected:
        void SetUp() override { MGPipeApplierReleaseObjectRecords(); }
        void TearDown() override { MGPipeApplierReleaseObjectRecords(); }
    };

#if MGTEST_HAVE_FORK
    struct ChildResult {
        int Status = -1;
        std::string Log;
    };

    // Runs `body` in a forked child. The child must not use gtest assertions; it _exit(0)s
    // when `body` returns, so a body expected to die must be ASSERTED dead by the parent
    // (WIFSIGNALED), never assumed.
    template <class Body>
    ChildResult RunInChild(Body body) {
        ChildResult result;
        const std::string before = ReadLog();
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) return result;
        if (pid == 0) {
            body();
            ::_exit(0);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid) return result;
        result.Status = status;
        result.Log = ReadLog().substr(before.size());
        return result;
    }

    bool DiedOfAbort(const ChildResult& r) {
        return WIFSIGNALED(r.Status) && WTERMSIG(r.Status) == SIGABRT;
    }
    std::string DescribeStatus(const ChildResult& r) {
        if (r.Status < 0) return "fork/waitpid failed";
        if (WIFEXITED(r.Status)) return "exited " + std::to_string(WEXITSTATUS(r.Status));
        if (WIFSIGNALED(r.Status)) return "signal " + std::to_string(WTERMSIG(r.Status));
        return "status " + std::to_string(r.Status);
    }

    // Forges ONE record straight into SEG_CMD, bypassing the encoder. Every R-2 arm needs
    // this: the encoder REFUSES to build a dishonest record, which is the point of it, so a
    // case that drove the encoder could only ever test the encoder's own check.
    void ForgeAndDecode(Wire2& wire, MGPWireOp op, const void* payload, std::uint64_t payloadBytes,
                        const void* tail, std::uint64_t tailBytes) {
        const std::uint64_t body = payloadBytes + tailBytes;
        void* slot = wire.Cmd().Reserve(static_cast<std::uint16_t>(op), Transport::kRecNone, body);
        if (slot == nullptr) {
            std::_Exit(9);
        }
        std::memcpy(slot, payload, static_cast<std::size_t>(payloadBytes));
        if (tailBytes != 0) {
            std::memcpy(static_cast<std::uint8_t*>(slot) + payloadBytes, tail,
                        static_cast<std::size_t>(tailBytes));
        }
        wire.Cmd().Publish();
        Transport::RingRecordView view{};
        bool corrupt = false;
        if (!wire.Consumer().Pop(view, &corrupt) || corrupt) {
            std::_Exit(10);
        }
        (void)wire.Decoder().DecodeAndApply(view);
    }
#endif // MGTEST_HAVE_FORK

} // namespace

// =====================================================================================
// Segment table and staging
// =====================================================================================

TEST_F(PipeWireCodecTest, SegmentZeroIsNeverARealSegment) {
    SegmentTable table;
    std::vector<std::uint8_t> bytes(256);
    table.Install(kSegStage, SegmentView{bytes.data(), bytes.size()});
    EXPECT_EQ(table.Resolve(kSegNone, 0, 8), nullptr);
    EXPECT_EQ(table.Get(kSegNone).Base, nullptr);
    // P8's index-mirror sentinel is reserved, not resolvable in this phase.
    EXPECT_EQ(table.Resolve(kMGHostSpanSegFromServerIndexMirror, 0, 8), nullptr);
}

TEST_F(PipeWireCodecTest, ResolveRefusesEveryRunThatLeavesItsSegment) {
    SegmentTable table;
    std::vector<std::uint8_t> bytes(256);
    table.Install(kSegStage, SegmentView{bytes.data(), bytes.size()});
    EXPECT_EQ(table.Resolve(kSegStage, 0, 256), bytes.data());
    EXPECT_EQ(table.Resolve(kSegStage, 248, 8), bytes.data() + 248);
    EXPECT_EQ(table.Resolve(kSegStage, 249, 8), nullptr);
    EXPECT_EQ(table.Resolve(kSegStage, 256, 1), nullptr);
    EXPECT_EQ(table.Resolve(kSegStage, 0, 0), nullptr);
    // The arithmetic is a subtraction, so an offset that would wrap offset+size cannot come
    // back as "inside".
    EXPECT_EQ(table.Resolve(kSegStage, 0xFFFFFFFFFFFFFFF0ull, 32), nullptr);
}

TEST_F(PipeWireCodecTest, StagedBytesResolveBackToTheSameAddress) {
    Wire2 wire;
    const std::uint8_t pattern[37] = {0};
    std::uint8_t source[37];
    for (std::size_t i = 0; i < sizeof(source); ++i) {
        source[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    (void)pattern;
    const MGPBlobRef ref = wire.Encoder().StageBytes(source, sizeof(source));
    EXPECT_EQ(ref.Seg, static_cast<Uint32>(kSegStage));
    EXPECT_EQ(ref.Size, sizeof(source));
    const void* back = wire.Segments().Resolve(ref.Seg, ref.Offset, ref.Size);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(std::memcmp(back, source, sizeof(source)), 0);
}

// =====================================================================================
// THE TRAP: two flag spaces in one 16-bit field
// =====================================================================================

TEST_F(PipeWireCodecTest, AVarTailRecordIsNotSkippedAsAWrapFiller) {
    // MGPipeCallFlags::kVarTail is 1<<2 and RingRecordFlags::kRecPad is 1<<2, and
    // MGPWireRecHeader::Flags IS RingRecordHeader::flags. An encoder that stamped the call's
    // own flags - which the generated comment invites - would make RingConsumer::Pop skip
    // every one of the nine kVarTail records as a wrap filler, silently, with no checksum
    // anywhere on this ring. This case is the trip wire for that.
    static_assert(static_cast<Uint16>(kVarTail) == static_cast<Uint16>(Transport::kRecPad),
                  "the collision this case exists for is gone; keep the case anyway");

    Wire2 wire;
    MGPVertexBuffers header{};
    header.Start = 0;
    header.Count = 2;
    header.ContentHash = 0x1234;
    MGPVertexBuffer tail[2]{};
    tail[0].Res = MakeHandle(11);
    tail[0].Stride = 16;
    tail[1].Res = MakeHandle(12);
    tail[1].Stride = 32;

    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetVertexBuffers, &header, sizeof(header), tail,
                                          sizeof(tail)),
              kInvalidSeq);
    wire.Encoder().Publish();

    Transport::RingRecordView view{};
    bool corrupt = false;
    ASSERT_TRUE(wire.Consumer().Pop(view, &corrupt)) << "the record was skipped as a pad";
    EXPECT_FALSE(corrupt);
    EXPECT_EQ(view.kind, static_cast<std::uint16_t>(MGPWireOp::SetVertexBuffers));
    EXPECT_EQ(view.flags & Transport::kRecPad, 0u);
    EXPECT_NE(view.flags & Transport::kRecVarTail, 0u);
}

// =====================================================================================
// One round trip per flag class
// =====================================================================================

TEST_F(PipeWireCodecTest, KNoneRoundTripsAndReachesItsApplier) {
    Wire2 wire;
    MGPBindRenderState bind{};
    bind.Cso = MakeHandle(4);
    bind.Version = 7;
    bind.PipelineVersion = 3;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::BindRenderState, &bind, sizeof(bind)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
    EXPECT_EQ(wire.Decoder().AppliedSeq(), 1u);
    EXPECT_EQ(wire.Control().appliedSeq.load(), 1u);
    EXPECT_EQ(wire.Control().retiredSeq.load(), 1u);
}

TEST_F(PipeWireCodecTest, KHasBlobRoundTripsWithARealChunkBlob) {
    Wire2 wire;
    // A brand-new CSO must name EVERY pipeline chunk - the applier refuses an incremental
    // create with no BaseCso (Fatal{PipeIncompleteCso}) - so this is the whole half.
    const Uint32 mask = MGPipeRenderStateChunkDetail::kAllPipelineHalfBits;
    const SizeT blobBytes = MGPipePipelineChunkBlobBytes(mask);
    ASSERT_GT(blobBytes, 0u);
    std::vector<std::uint8_t> chunks(blobBytes);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        chunks[i] = static_cast<std::uint8_t>(i * 7 + 1);
    }

    MGPRenderStateDesc desc{};
    desc.Cso = MakeHandle(9);
    desc.ChunkMask = mask;
    desc.Blob = wire.Encoder().StageBytes(chunks.data(), chunks.size());
    // R-2.2 in one line: what a monolith emission writes is Size 0, and what crosses must not
    // be.
    EXPECT_NE(desc.Blob.Size, 0u);
    EXPECT_EQ(desc.Blob.Seg, static_cast<Uint32>(kSegStage));

    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::CreateRenderState, &desc, sizeof(desc)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
}

TEST_F(PipeWireCodecTest, KVarTailRoundTripsWithItsTailIntact) {
    Wire2 wire;
    MGPSamplerViews header{};
    header.Start = 3;
    header.Count = 4;
    header.ContentHash = 99;
    MGPBoundView tail[4]{};
    for (Uint32 i = 0; i < 4; ++i) {
        tail[i].View = MakeHandle(100 + i);
        tail[i].Texture = MakeHandle(200 + i);
        tail[i].Unit = 3 + i;
    }
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetSamplerViews, &header, sizeof(header), tail,
                                          sizeof(tail)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
}

TEST_F(PipeWireCodecTest, KReplySlotMapPersistentIsAConstantDecline) {
    // R-6 / R-2.4. DECLINED is a real answer, not a failure, and the applier is not called at
    // all: the record's payload is a bare MGPHandleOnly and carries NEITHER the size NOR the
    // seedBytes MGPipeApplyMapPersistent takes.
    Wire2 wire;
    const MGPHandleOnly handle = HandleOnly(5, MGPipeKind::Buffer);
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::MapPersistent, &handle, sizeof(handle)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
    ASSERT_EQ(wire.Answers().All.size(), 1u);
    EXPECT_EQ(wire.Answers().All[0].Seq, 1u);
    EXPECT_EQ(wire.Answers().All[0].Status, ReplySink::kStatusDeclined);
    EXPECT_TRUE(wire.Answers().All[0].Bytes.empty());
}

TEST_F(PipeWireCodecTest, KNeedsAckRespecifyCarriesItsRedefinitionScope) {
    // Contract table 1 row 19b. Without the carrier every per-level glTexImage*D would take
    // the whole-resource arm on the far side and eat the other levels' pending uploads, so
    // this case is about the SCOPE surviving, not about the descriptor.
    Wire2 wire;
    MGPResourceDesc create{};
    create.Resource = MakeHandle(21);
    create.Target = static_cast<Uint8>(MGPipeResourceTarget::Tex2D);
    create.InternalFormat = 1;
    create.Width = 8;
    create.Height = 8;
    create.Depth = 1;
    create.ArrayLayers = 1;
    create.Levels = 2;
    create.Samples = 1;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::ResourceCreate, &create, sizeof(create)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));

    MGPResourceDesc respecify = create;
    MGPipeSetRespecifiedLevel(respecify, 0x0102u, 1u);
    EXPECT_FALSE(MGPipeRespecifyIsWholeResource(respecify));
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::ResourceRespecify, &respecify, sizeof(respecify)),
              kInvalidSeq);
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
    // Two records, two acceptance answers, on their own seqs (R-3: the seq IS the id).
    ASSERT_EQ(wire.Answers().All.size(), 2u);
    EXPECT_EQ(wire.Answers().All[0].Seq, 1u);
    EXPECT_EQ(wire.Answers().All[1].Seq, 2u);
}

TEST_F(PipeWireCodecTest, KOptionalUnmapPersistentRoundTrips) {
    Wire2 wire;
    const MGPHandleOnly handle = HandleOnly(6, MGPipeKind::Buffer);
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::UnmapPersistent, &handle, sizeof(handle)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
}

TEST_F(PipeWireCodecTest, KHostSpanClassIsValidatedEvenThoughP5ProducesNone) {
    // kCapNeedsHostUboBytes is 0 for the whole of P5 (table 0), so the second tail is always
    // absent here - and the record still has to be REFUSED if it ever is not honest.
    Wire2 wire;
    MGPShaderBuffers header{};
    header.Class = 0;
    header.Start = 0;
    header.Count = 2;
    header.HostSpanCount = 0;
    MGPBufferRange ranges[2]{};
    ranges[0].Res = MakeHandle(31);
    ranges[0].Size = 64;
    ranges[1].Res = MakeHandle(32);
    ranges[1].Size = 128;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetShaderBuffers, &header, sizeof(header),
                                          ranges, sizeof(ranges)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    // No applier entry point exists and the call is off the reduced path, so the honest answer
    // is "this build does not implement it" - after the tails have been checked.
    EXPECT_FALSE(applied);
}

// =====================================================================================
// The two double-tailed rows, and DrawVbo's conditional one
// =====================================================================================

TEST_F(PipeWireCodecTest, SetShaderBuffersCarriesBothTailsWhenTheSpanTailIsPresent) {
    Wire2 wire;
    MGPShaderBuffers header{};
    header.Class = 0;
    header.Count = 2;
    header.HostSpanCount = 2; // 0 or Count, never anything else
    MGPBufferRange ranges[2]{};
    ranges[0].Res = MakeHandle(41);
    ranges[1].Res = MakeHandle(42);
    MGHostSpan spans[2]{};
    spans[0].Ptr = nullptr;
    spans[0].Seg = kSegStage;
    spans[0].Size = 8;
    spans[0].Offset = 0;
    spans[1] = spans[0];

    const WireTail tails[2] = {{ranges, sizeof(ranges)}, {spans, sizeof(spans)}};
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetShaderBuffers, &header, sizeof(header),
                                          tails, 2),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_FALSE(applied);
}

TEST_F(PipeWireCodecTest, ResourceSubDataCarriesABlobAndARegionTailTogether) {
    // The only row in the catalogue that is BOTH kHasBlob and kVarTail, and the one rule A
    // changes most: the texture half declared Size 0 in monolith "because the byte count is
    // the server's to compute", which cannot be a bounds check.
    Wire2 wire;
    MGPResourceDesc create{};
    create.Resource = MakeHandle(61);
    create.Target = static_cast<Uint8>(MGPipeResourceTarget::Tex2D);
    create.InternalFormat = 1;
    create.Width = 4;
    create.Height = 4;
    create.Depth = 1;
    create.ArrayLayers = 1;
    create.Levels = 1;
    create.Samples = 1;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::ResourceCreate, &create, sizeof(create)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));

    std::vector<std::uint8_t> texels(4 * 4 * 4, 0x5A);
    MGPSubData upload{};
    upload.Res = create.Resource;
    upload.Target = MGPipePackSubDataTarget(static_cast<Uint32>(MGPipeResourceTarget::Tex2D), 0u);
    upload.Level = 0;
    upload.UnionBox = MGPBox{0, 0, 0, 4, 4, 1};
    upload.RegionCount = 1;
    upload.Blob = wire.Encoder().StageBytes(texels.data(), texels.size());
    MGPSubRegion region{};
    region.W = 4;
    region.H = 4;
    region.D = 1;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::ResourceSubData, &upload, sizeof(upload),
                                          &region, sizeof(region)),
              kInvalidSeq);
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
    ASSERT_EQ(wire.Answers().All.size(), 2u);
    // ACCEPTANCE IS NOT "APPLIED", and this case is where the difference shows. The record
    // crossed and reached MGPipeApplyResourceSubData, which is the codec's whole job; the
    // applier then DECLINED it, because no backend registered a P4a texture consumer in this
    // unit process (PipeApply.cpp's NoP4aConsumer belt - the one that turned "no consumer"
    // into lost texels on Magma). That is exactly the answer R-5 says must travel rather than
    // be re-derived on the client: a client that cleared its dirty flags on the strength of
    // having EMITTED would drop these texels for good.
    EXPECT_EQ(wire.Answers().All[1].Seq, 2u);
    EXPECT_EQ(wire.Answers().All[1].Status, ReplySink::kStatusDeclined);
    // And the texels themselves crossed intact: the decline is the applier's, not the wire's.
    const void* staged =
        wire.Segments().Resolve(upload.Blob.Seg, upload.Blob.Offset, upload.Blob.Size);
    ASSERT_NE(staged, nullptr);
    EXPECT_EQ(std::memcmp(staged, texels.data(), texels.size()), 0);
}

TEST_F(PipeWireCodecTest, SetGlobalConstantsCarriesTheDefaultUniformBlock) {
    Wire2 wire;
    std::vector<std::uint8_t> block(256, 0x11);
    MGPGlobalConstants record{};
    record.ShaderCso = MakeHandle(71);
    record.Version = 2;
    record.Blob = wire.Encoder().StageBytes(block.data(), block.size());
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetGlobalConstants, &record, sizeof(record)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
}

TEST_F(PipeWireCodecTest, SetStreamOutputTargetsCarriesItsRangesAndItsOffsets) {
    Wire2 wire;
    MGPStreamOutputTargets header{};
    header.Count = 3;
    header.Generation = 5;
    MGPBufferRange ranges[3]{};
    Uint32 offsets[3] = {16, 32, 48};
    for (Uint32 i = 0; i < 3; ++i) {
        ranges[i].Res = MakeHandle(51 + i);
        ranges[i].Size = 256;
    }
    const WireTail tails[2] = {{ranges, sizeof(ranges)}, {offsets, sizeof(offsets)}};
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetStreamOutputTargets, &header, sizeof(header),
                                          tails, 2),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_FALSE(applied); // no applier entry point; off the reduced path
}

TEST_F(PipeWireCodecTest, DrawVboWithoutUserIndicesHasExactlyOneTail) {
    Wire2 wire;
    MGPDrawInfo info{};
    info.Mode = 4;
    info.InstanceCount = 1;
    info.NumDraws = 3; // odd * 12 bytes: the case that makes the alignment rule matter
    MGPDrawRange ranges[3] = {{0, 3, 0}, {3, 6, 0}, {9, 3, 1}};
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::DrawVbo, &info, sizeof(info), ranges,
                                          sizeof(ranges)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
    ASSERT_EQ(wire.Sink().Draws.size(), 1u);
    EXPECT_EQ(wire.Sink().Draws[0].NumDraws, 3u);
    ASSERT_EQ(wire.Sink().DrawRanges.size(), 3u);
    EXPECT_EQ(wire.Sink().DrawRanges[2].Start, 9u);
    EXPECT_EQ(wire.Sink().DrawRanges[2].IndexBias, 1);
    EXPECT_FALSE(wire.Sink().SawUserIndices);
}

TEST_F(PipeWireCodecTest, DrawVboConditionalSpanTailIsEightAlignedAndSurvives) {
    Wire2 wire;
    MGPDrawInfo info{};
    info.Mode = 4;
    info.IndexSize = 2;
    info.Flags = kDrawHasUserIndices;
    info.InstanceCount = 1;
    info.NumDraws = 1; // 12 bytes: the span behind it would land on a 4-byte boundary

    const std::uint16_t indices[4] = {0, 1, 2, 3};
    const MGPBlobRef staged = wire.Encoder().StageBytes(indices, sizeof(indices));
    MGHostSpan span{};
    span.Ptr = nullptr; // rule B
    span.Seg = staged.Seg;
    span.Offset = staged.Offset;
    span.Size = staged.Size;

    MGPDrawRange ranges[1] = {{0, 4, 0}};
    const WireTail tails[2] = {{ranges, sizeof(ranges)}, {&span, sizeof(span)}};

    WireRecordLayout layout{};
    ASSERT_TRUE(MGPipeWireRecordLayout(MGPWireOp::DrawVbo, &info, layout));
    EXPECT_EQ(layout.TailCount, 2u);
    EXPECT_EQ(layout.TailOffset[1] % 8, 0u) << "MGPDrawRange is twelve bytes; the span behind an "
                                               "odd NumDraws must still be 8-aligned";

    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::DrawVbo, &info, sizeof(info), tails, 2),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
    EXPECT_TRUE(wire.Sink().SawUserIndices);
    EXPECT_EQ(wire.Sink().LastSpan.Ptr, nullptr);
    EXPECT_EQ(wire.Sink().LastSpan.Size, sizeof(indices));
}

// =====================================================================================
// CreateShaderState's seven blobs
// =====================================================================================

TEST_F(PipeWireCodecTest, CreateShaderStateCrossesAsOneArchiveAndSixUndeclaredRuns) {
    Wire2 wire;
    MG_State::GLState::LinkArtifacts link;
    MG_State::GLState::SpirvArtifacts spirv;
    spirv.spirvStatus = true;
    spirv.nativeFloat64 = false;
    spirv.generatedSpirv.resize(6);
    for (std::size_t stage = 0; stage < 6; ++stage) {
        spirv.generatedSpirv[stage].assign(4 + stage, static_cast<unsigned>(0x07230203 + stage));
    }
    spirv.globalUboScratch.assign(32, 0xAB);

    Vector<Uint8> archive;
    MG_State::GLState::EncodeProgramArtifacts(link, spirv, archive);
    ASSERT_FALSE(archive.empty());

    MGPProgramDesc desc{};
    desc.Cso = MakeHandle(77);
    desc.StageMask = 0x3f;
    desc.SpirvStatus = 1;
    // The ruling: Reflection names the WHOLE archive - which already carries every stage's
    // modules - and Spirv[0..5] stay all-zero. Shipping the modules twice would double the
    // biggest record in the catalogue for a reader that does not exist.
    desc.Reflection = wire.Encoder().StageBytes(archive.data(), archive.size());
    for (Uint32 i = 0; i < 6; ++i) {
        EXPECT_EQ(desc.Spirv[i].Size, 0u);
        EXPECT_EQ(desc.Spirv[i].Seg, 0u);
        EXPECT_EQ(desc.Spirv[i].Offset, 0u);
    }

    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::CreateShaderState, &desc, sizeof(desc)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);

    // And the archive really did carry the six modules: decode it the way the arm does.
    MG_State::GLState::LinkArtifacts back;
    MG_State::GLState::SpirvArtifacts backSpirv;
    ASSERT_TRUE(MG_State::GLState::DecodeProgramArtifacts(archive.data(), archive.size(), back,
                                                          backSpirv));
    ASSERT_EQ(backSpirv.generatedSpirv.size(), 6u);
    for (std::size_t stage = 0; stage < 6; ++stage) {
        EXPECT_EQ(backSpirv.generatedSpirv[stage].size(), 4 + stage);
        EXPECT_EQ(backSpirv.generatedSpirv[stage][0], 0x07230203u + stage);
    }
    EXPECT_TRUE(backSpirv.spirvStatus);
}

// =====================================================================================
// SetResidualValueState - table 1's hardest row
// =====================================================================================

TEST_F(PipeWireCodecTest, ResidualValueBlockCrossesAsItsOwnBlob) {
    // The applier takes `const ResidualValueBlock&` and MGPResidualValueState is never
    // instantiated on the live path, so this is the first code in the tree that fills either.
    Wire2 wire;
    ResidualValueBlock block{};
    block.CapabilityBits = 0x0123456789ABCDEFull;

    MGPResidualValueState record{};
    record.Version = 3;
    record.Blob = wire.Encoder().StageBytes(&block, sizeof(block));
    EXPECT_EQ(record.Blob.Size, static_cast<Uint64>(MGL_RESIDUAL_BLOCK_SIZE));

    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetResidualValueState, &record, sizeof(record)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
}

// =====================================================================================
// CreateSamplerState - a POD memcpy in which borderColorForm must survive
// =====================================================================================

TEST_F(PipeWireCodecTest, SamplerParametersCrossByteForByteIncludingBorderColorForm) {
    Wire2 wire;
    SamplerParameters params{};
    params.borderColorForm = BorderColorForm::Int;
    params.minLod = -3.5f;
    params.maxLod = 11.25f;

    MGPSamplerDesc desc{};
    desc.Cso = MakeHandle(88);
    desc.Parameters = wire.Encoder().StageBytes(&params, sizeof(params));
    EXPECT_EQ(desc.Parameters.Size, sizeof(SamplerParameters));

    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::CreateSamplerState, &desc, sizeof(desc)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);

    const void* staged = wire.Segments().Resolve(desc.Parameters.Seg, desc.Parameters.Offset,
                                                 desc.Parameters.Size);
    ASSERT_NE(staged, nullptr);
    SamplerParameters back{};
    std::memcpy(&back, staged, sizeof(back));
    EXPECT_EQ(static_cast<int>(back.borderColorForm), static_cast<int>(BorderColorForm::Int));
    EXPECT_FLOAT_EQ(back.minLod, -3.5f);
    EXPECT_FLOAT_EQ(back.maxLod, 11.25f);
}

// =====================================================================================
// The class-B verbs
// =====================================================================================

TEST_F(PipeWireCodecTest, TheFiveClassBVerbsReachTheSinkAndNothingElse) {
    Wire2 wire;
    MGPClear clear{};
    clear.Kind = 0;
    clear.BufferMask = 0x4000;
    clear.ColorValue[0] = 0x3f800000u;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::Clear, &clear, sizeof(clear)), kInvalidSeq);

    MGPBlit blit{};
    blit.SrcX1 = 64;
    blit.DstX1 = 64;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::Blit, &blit, sizeof(blit)), kInvalidSeq);

    MGPReadbackInfo readback{};
    readback.Box = MGPBox{0, 0, 0, 2, 2, 1};
    readback.DstSize = 16;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::ReadPixels, &readback, sizeof(readback)),
              kInvalidSeq);

    MGPPresent present{};
    present.FrameSerial = 12;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::Present, &present, sizeof(present)), kInvalidSeq);

    bool applied = false;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(wire.PumpOne(&applied)) << "record " << i;
        EXPECT_TRUE(applied) << "record " << i;
    }
    EXPECT_EQ(wire.Sink().Clears.size(), 1u);
    EXPECT_EQ(wire.Sink().Blits.size(), 1u);
    EXPECT_EQ(wire.Sink().Presents.size(), 1u);
    ASSERT_EQ(wire.Sink().Readbacks.size(), 1u);
    EXPECT_EQ(wire.Sink().Presents[0].FrameSerial, 12u);
    // read_pixels BLOCKS in P5 and its pixels come back in the reply slot the record's own
    // seq names (contract table 1 row 23).
    ASSERT_EQ(wire.Answers().All.size(), 1u);
    EXPECT_EQ(wire.Answers().All[0].Seq, wire.Sink().ReadbackSeqs[0]);
    EXPECT_EQ(wire.Answers().All[0].Bytes.size(), 4u);
    EXPECT_EQ(wire.Decoder().AppliedSeq(), 4u);
}

TEST_F(PipeWireCodecTest, AClassBVerbWithNoSinkDeclinesRatherThanInventsASemantics) {
    Wire2 wire;
    wire.Decoder().SetVerbSink(nullptr);
    MGPClear clear{};
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::Clear, &clear, sizeof(clear)), kInvalidSeq);
    bool applied = true;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_FALSE(applied);
}

// =====================================================================================
// R-9, R-10, R-11
// =====================================================================================

TEST_F(PipeWireCodecTest, AppliedSeqAdvancesByExactlyOnePerRecordAndIsNeverBatched) {
    Wire2 wire;
    MGPPresent present{};
    for (Uint64 i = 1; i <= 5; ++i) {
        present.FrameSerial = i;
        ASSERT_EQ(wire.Encoder().EncodeRecord(MGPWireOp::Present, &present, sizeof(present)), i);
    }
    wire.Encoder().Publish();
    for (Uint64 i = 1; i <= 5; ++i) {
        bool applied = false;
        ASSERT_TRUE(wire.PumpOne(&applied));
        EXPECT_EQ(wire.Decoder().AppliedSeq(), i);
        EXPECT_EQ(wire.Control().appliedSeq.load(), i);
    }
    EXPECT_EQ(wire.Encoder().EmitSeq(), 5u);
}

TEST_F(PipeWireCodecTest, MaxRecordBytesSeenStaysFarBelowHalfTheRing) {
    // R-10's proof obligation. P5 does no chunking and must instead show it never needed any.
    Wire2 wire;
    MGPDrawInfo info{};
    info.NumDraws = 64;
    std::vector<MGPDrawRange> ranges(64);
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::DrawVbo, &info, sizeof(info), ranges.data(),
                                          ranges.size() * sizeof(MGPDrawRange)),
              kInvalidSeq);
    const Uint64 largest = wire.Encoder().MaxRecordBytesSeen();
    EXPECT_GT(largest, 0u);
    EXPECT_EQ(largest, 8u + sizeof(MGPDrawInfo) + 64u * sizeof(MGPDrawRange));
    EXPECT_LT(largest, wire.Cmd().MaxRecordBytes());
    // The biggest fixed payload in the whole catalogue is MGPProgramDesc at 192 bytes plus
    // MGPFramebufferState at 304, so a record only ever grows through its TAIL - which is why
    // the counter is on the encoder and not a constant.
    EXPECT_LT(8u + sizeof(MGPFramebufferState), wire.Cmd().MaxRecordBytes());
}

TEST_F(PipeWireCodecTest, StagedBytesAreReclaimedOnlyBehindRetiredSeq) {
    Wire2 wire;
    const std::uint8_t payload[64] = {};
    const MGPBlobRef first = wire.Encoder().StageBytes(payload, sizeof(payload));
    (void)first;
    const Uint64 inFlight = wire.Encoder().StagedBytesInFlight();
    EXPECT_GE(inFlight, sizeof(payload));

    MGPBindRenderState bind{};
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::BindRenderState, &bind, sizeof(bind)),
              kInvalidSeq);
    // Nothing is retired yet, so nothing may be released: R-11's whole content on this side.
    wire.Encoder().ReclaimStagedBytes();
    EXPECT_EQ(wire.Encoder().StagedBytesInFlight(), inFlight);

    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    wire.Encoder().ReclaimStagedBytes();
    EXPECT_EQ(wire.Encoder().StagedBytesInFlight(), 0u);
}

TEST_F(PipeWireCodecTest, TheAuditFillOverwritesExactlyTheRunsTheRecordResolved) {
    // R-2.5, the only mechanical control on rule C ("no applier entry point retains a pointer
    // past its return"). An instrumentation that cannot be observed to have run is decoration,
    // so this case asserts the bytes, not the flag.
    Wire2 wire;
    wire.Decoder().SetAuditPoison(true);
    ResidualValueBlock block{};
    block.CapabilityBits = 0x5555555555555555ull;
    MGPResidualValueState record{};
    record.Blob = wire.Encoder().StageBytes(&block, sizeof(block));
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetResidualValueState, &record, sizeof(record)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
    EXPECT_EQ(wire.Decoder().PoisonedStageBytes(), sizeof(ResidualValueBlock));
    const auto* staged = wire.StageBase() + record.Blob.Offset;
    for (std::size_t i = 0; i < sizeof(ResidualValueBlock); ++i) {
        EXPECT_EQ(staged[i], 0xDD) << "byte " << i;
    }
}

TEST_F(PipeWireCodecTest, TheAuditFillIsOffByDefaultSoTheHotPathPaysNothing) {
    Wire2 wire;
    ResidualValueBlock block{};
    block.CapabilityBits = 0x77ull;
    MGPResidualValueState record{};
    record.Blob = wire.Encoder().StageBytes(&block, sizeof(block));
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetResidualValueState, &record, sizeof(record)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_EQ(wire.Decoder().PoisonedStageBytes(), 0u);
    const auto* staged = wire.StageBase() + record.Blob.Offset;
    EXPECT_NE(staged[0], 0xDD);
}

// =====================================================================================
// MGPCaps's two blob codecs
// =====================================================================================

TEST_F(PipeWireCodecTest, FormatCapabilitiesRoundTrip) {
    MG_Backend::FormatCapabilityCache cache;
    cache.FullCaps[0][1] = MG_Backend::FormatCapabilityFlags(
        static_cast<Uint64>(MG_Backend::FormatCapability::Creatable) |
        static_cast<Uint64>(MG_Backend::FormatCapability::Sampled));
    cache.CaveatCaps[2][3] =
        MG_Backend::FormatCapabilityFlags(static_cast<Uint64>(MG_Backend::FormatCapability::LinearFilter));
    cache.SampleCounts[1][4] = Vector<Int>{1, 2, 4, 8};

    Vector<Uint8> bytes;
    ASSERT_TRUE(EncodeFormatCapabilities(cache, bytes));
    ASSERT_FALSE(bytes.empty());

    MG_Backend::FormatCapabilityCache back;
    ASSERT_TRUE(DecodeFormatCapabilities(bytes.data(), bytes.size(), back));
    EXPECT_EQ(back.FullCaps[0][1].GetRaw(), cache.FullCaps[0][1].GetRaw());
    EXPECT_EQ(back.CaveatCaps[2][3].GetRaw(), cache.CaveatCaps[2][3].GetRaw());
    EXPECT_EQ(back.SampleCounts[1][4], cache.SampleCounts[1][4]);
    EXPECT_EQ(back.FullCaps[5][5].GetRaw(), 0u);
    EXPECT_TRUE(back.SampleCounts[0][0].empty());

    // Sparse: an almost-empty cache must not cost the square of two enum spaces.
    EXPECT_LT(bytes.size(), 4096u);
}

TEST_F(PipeWireCodecTest, FormatCapabilitiesDecoderRefusesTruncationAndTrailingBytes) {
    MG_Backend::FormatCapabilityCache cache;
    cache.FullCaps[0][0] =
        MG_Backend::FormatCapabilityFlags(static_cast<Uint64>(MG_Backend::FormatCapability::Creatable));
    Vector<Uint8> bytes;
    ASSERT_TRUE(EncodeFormatCapabilities(cache, bytes));

    MG_Backend::FormatCapabilityCache back;
    for (SizeT cut = 1; cut < bytes.size(); ++cut) {
        EXPECT_FALSE(DecodeFormatCapabilities(bytes.data(), cut, back)) << "truncated to " << cut;
    }
    Vector<Uint8> longer = bytes;
    longer.push_back(0);
    EXPECT_FALSE(DecodeFormatCapabilities(longer.data(), longer.size(), back));
    // A version word this build does not read is a refusal, never a guess.
    Vector<Uint8> wrongVersion = bytes;
    wrongVersion[0] = static_cast<Uint8>(wrongVersion[0] + 1);
    EXPECT_FALSE(DecodeFormatCapabilities(wrongVersion.data(), wrongVersion.size(), back));
}

TEST_F(PipeWireCodecTest, RendererInfoRoundTrips) {
    RendererInfo info;
    info.RendererName = "Espryt";
    info.BackendName = "DirectGLES";
    info.ExtraVendor = String("Qualcomm");
    info.RendererGLInfo.TargetGLVersion = Version{4, 6, 0, Optional<String>(), Optional<VersionType>()};
    info.RendererGLInfo.TargetGLSLVersion =
        Version{4, 60, 0, Optional<String>(String("-dev")), Optional<VersionType>(VersionType::Development)};
    info.RendererGLInfo.Extensions = Vector<GLExtension>{static_cast<GLExtension>(1),
                                                         static_cast<GLExtension>(7)};
    info.RendererGLInfo.IsCompatibilityProfile = true;
    info.StaticBackendCapability.AllowVSOnlyPrograms = true;

    Vector<Uint8> bytes;
    ASSERT_TRUE(EncodeRendererInfo(info, bytes));

    RendererInfo back;
    ASSERT_TRUE(DecodeRendererInfo(bytes.data(), bytes.size(), back));
    EXPECT_EQ(back.RendererName, info.RendererName);
    EXPECT_EQ(back.BackendName, info.BackendName);
    ASSERT_TRUE(back.ExtraVendor.has_value());
    EXPECT_EQ(*back.ExtraVendor, "Qualcomm");
    EXPECT_EQ(back.RendererGLInfo.TargetGLVersion.Major, 4);
    EXPECT_EQ(back.RendererGLInfo.TargetGLSLVersion.Minor, 60);
    ASSERT_TRUE(back.RendererGLInfo.TargetGLSLVersion.Suffix.has_value());
    EXPECT_EQ(*back.RendererGLInfo.TargetGLSLVersion.Suffix, "-dev");
    ASSERT_TRUE(back.RendererGLInfo.TargetGLSLVersion.Type.has_value());
    EXPECT_EQ(static_cast<int>(*back.RendererGLInfo.TargetGLSLVersion.Type),
              static_cast<int>(VersionType::Development));
    ASSERT_EQ(back.RendererGLInfo.Extensions.size(), 2u);
    EXPECT_EQ(static_cast<int>(back.RendererGLInfo.Extensions[1]), 7);
    EXPECT_TRUE(back.RendererGLInfo.IsCompatibilityProfile);
    EXPECT_TRUE(back.StaticBackendCapability.AllowVSOnlyPrograms);
}

TEST_F(PipeWireCodecTest, RendererInfoDecoderRefusesEveryTruncation) {
    RendererInfo info;
    info.RendererName = "Magma";
    info.BackendName = "DirectVulkan";
    Vector<Uint8> bytes;
    ASSERT_TRUE(EncodeRendererInfo(info, bytes));
    RendererInfo back;
    for (SizeT cut = 1; cut < bytes.size(); ++cut) {
        EXPECT_FALSE(DecodeRendererInfo(bytes.data(), cut, back)) << "truncated to " << cut;
    }
    EXPECT_FALSE(DecodeRendererInfo(nullptr, 0, back));
}

TEST_F(PipeWireCodecTest, TheAbiFingerprintIsStableWithinABuildAndNotZero) {
    const Uint64 first = CapsAbiFingerprint();
    EXPECT_NE(first, 0u);
    EXPECT_EQ(first, CapsAbiFingerprint());
}

TEST_F(PipeWireCodecTest, TheConsumerMaskAnswersPerFamilyAndNotPerOpTable) {
    // R-8's only legal client-side spelling. Under inproc a client that read
    // MGPipeGetResourceOps() would be right BY ACCIDENT; under spawn that table is null and
    // five whole record families emit nothing at all, silently.
    const Uint64 mask = MGCapsConsumerBits(kMGPipeSubsystemResources | kMGPipeSubsystemPrograms);
    EXPECT_TRUE(MGCapsServerConsumes(mask, kMGPipeSubsystemResources));
    EXPECT_TRUE(MGCapsServerConsumes(mask, kMGPipeSubsystemPrograms));
    EXPECT_FALSE(MGCapsServerConsumes(mask, kMGPipeSubsystemTextureResources));
    // The feature bits below it are untouched by the consumer block.
    EXPECT_EQ(mask & 0xFFFFFFFFull, 0u);
}

// =====================================================================================
// The Fatal arms. Forked, never EXPECT_DEATH.
// =====================================================================================

#if MGTEST_HAVE_FORK

TEST_F(PipeWireCodecTest, ANonNullHostSpanPointerIsFatal) {
    // R-2 arm 1 / rule B. In ONE address space this pointer works, which is exactly why the
    // rule has to be mechanical.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPShaderBuffers header{};
        header.Count = 1;
        header.HostSpanCount = 1;
        MGPBufferRange range{};
        MGHostSpan span{};
        std::uint64_t here = 0;
        span.Ptr = &here; // the inproc cheat
        span.Seg = kSegStage;
        span.Size = 8;
        std::vector<std::uint8_t> tail(sizeof(range) + sizeof(span));
        std::memcpy(tail.data(), &range, sizeof(range));
        std::memcpy(tail.data() + sizeof(range), &span, sizeof(span));
        ForgeAndDecode(wire, MGPWireOp::SetShaderBuffers, &header, sizeof(header), tail.data(),
                       tail.size());
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{ProtocolCorruption, \"host-span\"}"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, AContentRecordThatDeclaresNoBlobIsFatal) {
    // R-2 arm 2. This is the arm that inverts today's legal state: Blob.Size == 0 means "this
    // record does not declare its blob", which is right for monolith and a lie under split.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPVertexElements desc{};
        desc.Cso = MakeHandle(3);
        desc.AttributeCount = 1;
        desc.BindingPointCount = 1;
        desc.Blob = MGPBlobRef{}; // all three fields zero: "absent"
        ForgeAndDecode(wire, MGPWireOp::CreateVertexElements, &desc, sizeof(desc), nullptr, 0);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("carries content and its blob declares none"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, ANonZeroSizeWithNoSegmentIsFatal) {
    // R-2 arm 3.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPVertexElements desc{};
        desc.Cso = MakeHandle(3);
        desc.AttributeCount = 1;
        desc.Blob.Seg = kSegNone;
        desc.Blob.Offset = 0;
        desc.Blob.Size = 64;
        ForgeAndDecode(wire, MGPWireOp::CreateVertexElements, &desc, sizeof(desc), nullptr, 0);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("with no segment"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, ARunThatLeavesItsSegmentIsFatal) {
    // R-2 arm 4.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPVertexElements desc{};
        desc.Cso = MakeHandle(3);
        desc.AttributeCount = 1;
        desc.Blob.Seg = kSegStage;
        desc.Blob.Offset = Wire2::kStageBytes - 8;
        desc.Blob.Size = 4096;
        ForgeAndDecode(wire, MGPWireOp::CreateVertexElements, &desc, sizeof(desc), nullptr, 0);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("does not lie inside that segment"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, AHalfDeclaredBlobIsFatalRatherThanReadAsAbsent) {
    // The shape a MONOLITH emitter produces - Seg None, Offset a host address, Size 0. Reading
    // it as "absent" would silently drop the bytes of every record an unconverted emitter sent.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPRenderStateDesc desc{};
        desc.Cso = MakeHandle(3);
        desc.ChunkMask = 0;
        desc.Blob.Seg = kMGHostSpanSegNone;
        desc.Blob.Offset = 0xDEADBEEFull; // a host address, the way ProgramEmit.h writes one
        desc.Blob.Size = 0;
        ForgeAndDecode(wire, MGPWireOp::CreateRenderState, &desc, sizeof(desc), nullptr, 0);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("a blob with Size 0 declares"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, ATailThatDoesNotMatchItsOwnCountIsFatal) {
    // THE CROSS-CHECK THIS PACKAGE EXISTS FOR. MGP_WIRE_CHECK_BOUNDS proves
    // `size >= sizeof(MGPWireRec_X)` and CANNOT SEE THE TAIL, so this record - Count = 4000
    // with eight bytes behind it - passes the generated gate today.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPSamplerViews header{};
        header.Start = 0;
        header.Count = 4000;
        const std::uint64_t eightBytes = 0;
        ForgeAndDecode(wire, MGPWireOp::SetSamplerViews, &header, sizeof(header), &eightBytes,
                       sizeof(eightBytes));
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    // The count is past the unit bound, so the layout refuses it before the length even
    // matters - which is the stronger of the two answers.
    EXPECT_NE(r.Log.find("Fatal{ProtocolCorruption"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, ATailWhoseLengthDisagreesWithItsCountIsFatal) {
    // The same defect inside the legal count range, so the SIZE arithmetic is what catches it
    // rather than the bound.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPSamplerViews header{};
        header.Start = 0;
        header.Count = 4; // 4 * sizeof(MGPBoundView) == 96 bytes of tail
        const std::uint64_t eightBytes = 0;
        ForgeAndDecode(wire, MGPWireOp::SetSamplerViews, &header, sizeof(header), &eightBytes,
                       sizeof(eightBytes));
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("its own count fields describe"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, VertexAttribDefaultsMustAgreeWithItsOwnMask) {
    // Two declarants, Count and popcount(Mask), and nothing checked them before.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPVertexAttribDefaults header{};
        header.Mask = 0x7; // three bits
        header.Count = 2;  // two entries
        MGPAttribValue tail[2]{};
        ForgeAndDecode(wire, MGPWireOp::SetVertexAttribDefaults, &header, sizeof(header), tail,
                       sizeof(tail));
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("SetVertexAttribDefaults.Count"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, APadRecordReachingTheDecoderIsFatal) {
    // R-9: a pad does not advance seq and both sides skip it BEFORE counting. One that reached
    // here has already been counted, and because the seq IS the reply-slot id, a drift of one
    // silently reads another call's answer rather than failing.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        Transport::RingRecordView view{};
        std::uint8_t bytes[16] = {};
        view.kind = Transport::kRingPadRecordKind;
        view.flags = Transport::kRecPad;
        view.payload = bytes + 8;
        view.payloadSize = 8;
        (void)wire.Decoder().DecodeAndApply(view);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("wrap filler reached the decoder"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, ADeclaredPerStageSpirvRunIsFatal) {
    // w1's ruling: the archive already carries every stage's modules, so a per-stage run would
    // be a second, forgeable way to say the same thing.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MG_State::GLState::LinkArtifacts link;
        MG_State::GLState::SpirvArtifacts spirv;
        Vector<Uint8> archive;
        MG_State::GLState::EncodeProgramArtifacts(link, spirv, archive);
        MGPProgramDesc desc{};
        desc.Cso = MakeHandle(3);
        desc.Reflection = wire.Encoder().StageBytes(archive.data(), archive.size());
        const std::uint32_t words[4] = {1, 2, 3, 4};
        desc.Spirv[0] = wire.Encoder().StageBytes(words, sizeof(words));
        ForgeAndDecode(wire, MGPWireOp::CreateShaderState, &desc, sizeof(desc), nullptr, 0);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("CreateShaderState.Spirv[0]"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, AHostSpanCountThatIsNeitherZeroNorCountIsFatal) {
    // MGPipeTypes.h:820-823: HostSpanCount is 0 OR Count, never anything else, so the two
    // arrays stay index-aligned. A third value lets a record describe spans for ranges it does
    // not have - and the arrays would then be read off by one for the rest of the tail.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPShaderBuffers header{};
        header.Count = 4;
        header.HostSpanCount = 3;
        MGPBufferRange ranges[4]{};
        MGHostSpan spans[3]{};
        std::vector<std::uint8_t> tail(sizeof(ranges) + sizeof(spans));
        std::memcpy(tail.data(), ranges, sizeof(ranges));
        std::memcpy(tail.data() + sizeof(ranges), spans, sizeof(spans));
        ForgeAndDecode(wire, MGPWireOp::SetShaderBuffers, &header, sizeof(header), tail.data(),
                       tail.size());
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("SetShaderBuffers.HostSpanCount"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, ASecondProcessResolverIsFatalRatherThanASilentRace) {
    // Table 3: one gMGPipeSegmentResolver per process, installed by the SERVER role only.
    const ChildResult r = RunInChild([] {
        SegmentTable a;
        SegmentTable b;
        std::vector<std::uint8_t> bytes(64);
        a.Install(kSegStage, SegmentView{bytes.data(), bytes.size()});
        b.Install(kSegStage, SegmentView{bytes.data(), bytes.size()});
        a.InstallProcessResolver();
        b.InstallProcessResolver();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("already installed"), std::string::npos) << r.Log;
}

#else

TEST_F(PipeWireCodecTest, TheFatalArmsNeedFork) {
    GTEST_SKIP() << "the R-2 Fatal arms are asserted by forking; POSIX only";
}

#endif // MGTEST_HAVE_FORK

TEST_F(PipeWireCodecTest, TheProcessResolverRoundTripsThroughMGPipeHostBytes) {
    SegmentTable table;
    std::vector<std::uint8_t> bytes(128);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i);
    }
    table.Install(kSegStage, SegmentView{bytes.data(), bytes.size()});
    table.InstallProcessResolver();

    MGHostSpan span{};
    span.Ptr = nullptr;
    span.Seg = kSegStage;
    span.Offset = 16;
    span.Size = 32;
    EXPECT_EQ(MGPipeHostBytes(span), bytes.data() + 16);

    SegmentTable::UninstallProcessResolver();
    EXPECT_EQ(MGPipeHostBytes(span), nullptr);
}

int main(int argc, char** argv) {
    // Before anything logs: MG_Util::Debug::InitFile() reads the variable once, on the first
    // write, and caches the FILE*. The name carries this process's pid, because
    // gtest_discover_tests runs every case as its own process, in parallel under ctest -j.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-pipewirecodec-test-" + std::to_string(ProcessId()) + ".log");
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
