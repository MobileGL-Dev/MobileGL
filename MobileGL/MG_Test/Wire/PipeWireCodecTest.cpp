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

// MG_Config::Transport and MG_Config::Ipc.AdoptTier: the two knobs R-6's tier gate reads.
#include <Config.h>
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

        // Pops one record, decodes it, and then does what s1's SessionConsumer::ApplyOne does:
        // advance RingControl's watermarks by ONE. THE DECODER DOES NOT WRITE RingControl -
        // appliedSeq has exactly one writer and it is the session - so this fixture has to
        // play that role, which is also what lets a case compare the session's watermark
        // against the decoder's own tally and catch a batched publish.
        //
        // Returns whether there was a record at all, so a case cannot pass because nothing was
        // there; `applied` is what the decoder reported.
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
            ++m_sessionApplied;
            m_control.appliedSeq.store(m_sessionApplied, std::memory_order_release);
            // Nothing in P5 borrows a ring slot into the GPU timeline, so a record's SEG_STAGE
            // runs retire as soon as it is applied (table 1's "retires: apply").
            m_control.retiredSeq.store(m_sessionApplied, std::memory_order_release);
            m_consumer.PublishRetired();
            if (applied != nullptr) {
                *applied = result;
            }
            return true;
        }

        std::uint64_t SessionAppliedSeq() const { return m_sessionApplied; }

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
        std::uint64_t m_sessionApplied = 0;
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
    // And the third one, which the first round missed and a reviewer found: it bites in the
    // REVERSE direction, because the encoder stamps kRecVarTail on nine opcodes and anyone
    // trusting PipeWire.inc's "MGPipeCallFlags of the call" reads those nine as kReplySlot.
    static_assert(static_cast<Uint16>(kReplySlot) == static_cast<Uint16>(Transport::kRecVarTail),
                  "kReplySlot and kRecVarTail alias");
    // Five of the six call-flag bits alias a ring bit; only kOptional is free, and only
    // because RingRecordFlags has not reached 1<<5.
    static_assert((static_cast<Uint16>(kNeedsAck | kHasBlob | kVarTail | kHostSpan | kReplySlot |
                                       kOptional) &
                   static_cast<Uint16>(Transport::kRecNeedsAck | Transport::kRecHasBlob |
                                       Transport::kRecPad | Transport::kRecBorrowSlot |
                                       Transport::kRecVarTail)) == 0x1Fu,
                  "the overlap between the two flag spaces moved");

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

TEST_F(PipeWireCodecTest, AnEmptyVarTailIsALegalRecordAndNeedsNoPlaceholderEntry) {
    // Count == 0 is legal for every kVarTail row - "bind nothing at this range" - and a caller
    // that had to pass a {nullptr, 0} entry for each absent tail would be walking into a trap
    // rather than through a check. SetStreamOutputTargets is the sharpest case: its layout has
    // TWO tails and both are empty at Count 0.
    Wire2 wire;
    MGPStreamOutputTargets header{};
    header.Count = 0;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetStreamOutputTargets, &header, sizeof(header)),
              kInvalidSeq);
    MGPVertexBuffers buffers{};
    buffers.Count = 0;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::SetVertexBuffers, &buffers, sizeof(buffers)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_FALSE(applied); // no applier for stream output; off the reduced path
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

TEST_F(PipeWireCodecTest, TierTwoUnderSplitTransportStillDeclinesRatherThanRefusing) {
    // The POSITIVE half of the two AdoptTier death cases below. Without it, those two could
    // be satisfied by an arm that aborted on every tier, which is the opposite mistake to the
    // one wave1-codex-verify.md §4 found. T2 is the only tier P5 implements and R-6 says the
    // answer there is DECLINED - a real answer, not a failure - even when the transport is
    // the split one that makes the tier question live at all.
    const MG_Config::TransportMode savedTransport = MG_Config::Transport;
    const Uint32 savedTier = MG_Config::Ipc.AdoptTier;
    struct Restore {
        MG_Config::TransportMode T;
        Uint32 A;
        ~Restore() {
            MG_Config::Transport = T;
            MG_Config::Ipc.AdoptTier = A;
        }
    } restore{savedTransport, savedTier};
    MG_Config::Transport = MG_Config::TransportMode::InProcess;
    MG_Config::Ipc.AdoptTier = 2u;

    Wire2 wire;
    const MGPHandleOnly handle = HandleOnly(5, MGPipeKind::Buffer);
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::MapPersistent, &handle, sizeof(handle)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);
    ASSERT_EQ(wire.Answers().All.size(), 1u);
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
    // Two records, two ACCEPTANCE answers - and NOT in a reply slot. Neither ResourceCreate
    // nor ResourceRespecify carries kReplySlot, and s1 sizes ReplyPool from that table, so a
    // reply written here would overwrite some waiter's slot.
    EXPECT_TRUE(wire.Answers().All.empty());
    EXPECT_EQ(wire.Decoder().AcceptedRecords() + wire.Decoder().DeclinedRecords(), 2u);
    EXPECT_TRUE(wire.Decoder().LastAcceptanceKnown());
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
// Declared padding is payload, not slack
// =====================================================================================

TEST_F(PipeWireCodecTest, EveryPayloadByteCrossesIncludingTheOnesSpelledPad) {
    // A PAD IS A FIELD SOMEBODY HAS NOT CLAIMED YET, and this phase is the proof: P5 put the
    // respecify scope into MGPResourceDesc's two pads (contract table 1 row 19b) and b1 put
    // MGPSubData::Pad0's low byte to work as HasLiveHostWrites. A codec that zeroed a pad "for
    // determinism", or built a payload field by field, would DELETE those bits - and a dropped
    // HasLiveHostWrites is not a visible failure, it is IsBufferDrawClean answering "clean"
    // for a buffer with a live host writer, i.e. the frame drawing the last uploaded bytes
    // with no diagnostic at all.
    //
    // So this case asserts the whole payload byte for byte rather than the named fields: a
    // test that compared only the members would go green through exactly that bug.
    // IT NAMES NO PAD MEMBER, deliberately. The whole struct is stamped with a recognisable
    // byte first and only the fields the decoder validates are then written, so whatever is
    // left - Pad0, Pad1, or the names a later phase gives them - still carries the stamp and a
    // memcmp over the whole payload is the assertion. A case that named `Pad0` would stop
    // COMPILING the day someone claims it, which is precisely the day it is most needed.
    Wire2 wire;
    std::vector<std::uint8_t> texels(64, 0x31);

    MGPSubData upload{};
    std::memset(&upload, 0xA5, sizeof(upload));
    upload.Res = MakeHandle(101);
    upload.Target = MGPipePackSubDataTarget(static_cast<Uint32>(MGPipeResourceTarget::Tex2D), 0u);
    upload.Level = 2;
    upload.SourceIsVerbatimLevelShadow = 1;
    upload.UnionBox = MGPBox{1, 2, 0, 4, 4, 1};
    upload.RegionCount = 0;
    upload.Blob = wire.Encoder().StageBytes(texels.data(), texels.size());

    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::ResourceSubData, &upload, sizeof(upload)),
              kInvalidSeq);
    wire.Encoder().Publish();

    Transport::RingRecordView view{};
    bool corrupt = false;
    ASSERT_TRUE(wire.Consumer().Pop(view, &corrupt));
    ASSERT_FALSE(corrupt);
    ASSERT_GE(view.payloadSize, sizeof(upload));
    EXPECT_EQ(std::memcmp(view.payload, &upload, sizeof(upload)), 0)
        << "the payload did not cross byte for byte";

    // And the stamp really did survive somewhere the named fields do not cover, so the case
    // cannot pass by comparing a struct that has no unclaimed bytes left.
    const auto* crossed = static_cast<const std::uint8_t*>(view.payload);
    std::size_t stamped = 0;
    for (std::size_t i = 0; i < sizeof(upload); ++i) {
        if (crossed[i] == 0xA5) {
            ++stamped;
        }
    }
    EXPECT_GT(stamped, 0u) << "no byte of the payload was left unclaimed; the case still checks "
                              "the memcmp above, but it no longer proves anything about pads";
}

TEST_F(PipeWireCodecTest, ResourceDescPadsCrossToo) {
    // The same property over the struct P5 itself put two fields into. The helpers are the
    // only legal reader (three fields are one value), but the BYTES are what the codec owes.
    Wire2 wire;
    MGPResourceDesc desc{};
    desc.Resource = MakeHandle(111);
    desc.Target = static_cast<Uint8>(MGPipeResourceTarget::TexCube);
    desc.InternalFormat = 7;
    desc.Width = 16;
    desc.Height = 16;
    desc.Depth = 1;
    desc.ArrayLayers = 6;
    desc.Levels = 3;
    desc.Samples = 1;
    MGPipeSetRespecifiedLevel(desc, 0x0304u, 2u);

    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::ResourceRespecify, &desc, sizeof(desc)),
              kInvalidSeq);
    wire.Encoder().Publish();

    Transport::RingRecordView view{};
    bool corrupt = false;
    ASSERT_TRUE(wire.Consumer().Pop(view, &corrupt));
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(std::memcmp(view.payload, &desc, sizeof(desc)), 0);
    const auto* crossed = static_cast<const MGPResourceDesc*>(view.payload);
    EXPECT_FALSE(MGPipeRespecifyIsWholeResource(*crossed));
    EXPECT_EQ(MGPipeRespecifiedUploadTargetOf(*crossed), 0x0304u);
    EXPECT_EQ(MGPipeRespecifiedLevelOf(*crossed), 2u);

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
    EXPECT_TRUE(wire.Answers().All.empty());
    // ACCEPTANCE IS NOT "APPLIED", and this case is where the difference shows. The record
    // crossed and reached MGPipeApplyResourceSubData, which is the codec's whole job; the
    // applier then DECLINED it, because no backend registered a P4a texture consumer in this
    // unit process (PipeApply.cpp's NoP4aConsumer belt - the one that turned "no consumer"
    // into lost texels on Magma). That is exactly the answer R-5 says must travel rather than
    // be re-derived on the client: a client that cleared its dirty flags on the strength of
    // having EMITTED would drop these texels for good.
    EXPECT_TRUE(wire.Decoder().LastAcceptanceKnown());
    EXPECT_FALSE(wire.Decoder().LastAcceptance());
    // Both records answered - the texture create is declined by the same NoP4aConsumer belt.
    EXPECT_EQ(wire.Decoder().AcceptedRecords() + wire.Decoder().DeclinedRecords(), 2u);
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
    // TWO COUNTS, ON PURPOSE. RingControl::appliedSeq has exactly one writer - the session -
    // and the decoder keeps its own tally. They must agree after every record, and a single
    // counter could not tell a batched publish from an honest one (R-9: a batched watermark
    // makes the client's barrier resume on work the server has not run).
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
        EXPECT_EQ(wire.SessionAppliedSeq(), wire.Decoder().AppliedSeq());
    }
    EXPECT_EQ(wire.Encoder().EmitSeq(), 5u);
}

TEST_F(PipeWireCodecTest, TheDecoderWritesNoRingControlFieldOfItsOwn) {
    // s1 owns RingControl::appliedSeq; the codec's job ends at "this record was applied". Two
    // writers of a watermark is how a waiter resumes on a record the server has not run, and
    // there is no checksum on this ring that would catch it - so this is asserted rather than
    // documented.
    Wire2 wire;
    MGPPresent present{};
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::Present, &present, sizeof(present)),
              kInvalidSeq);
    wire.Encoder().Publish();

    Transport::RingRecordView view{};
    bool corrupt = false;
    ASSERT_TRUE(wire.Consumer().Pop(view, &corrupt));
    ASSERT_FALSE(corrupt);
    const std::uint64_t appliedBefore = wire.Control().appliedSeq.load();
    const std::uint64_t retiredBefore = wire.Control().retiredSeq.load();

    (void)wire.Decoder().DecodeAndApply(view);

    EXPECT_EQ(wire.Control().appliedSeq.load(), appliedBefore);
    EXPECT_EQ(wire.Control().retiredSeq.load(), retiredBefore);
    EXPECT_EQ(wire.Decoder().AppliedSeq(), 1u); // the decoder's own tally did move
}

TEST_F(PipeWireCodecTest, MaxRecordBytesSeenStaysFarBelowHalfTheRing) {
    // R-10's proof obligation. P5 does no chunking and must instead show it never needed any.
    //
    // THE CAP IS ASKED FOR AT RUNTIME AND NEVER DERIVED FROM MOBILEGL_IPC_RING_MB, and this
    // phase is why: the number moved twice in one afternoon. s1 first found that the control
    // page came out of the segment (making the cap 2 MiB at the default), then fixed it the
    // other way round - MOBILEGL_IPC_RING_MB now names the RING and the segment adds a page on
    // top - so the cap is 4 MiB again and the contract is true as written. Nothing in the
    // codec changed either time, because every comparison goes through MaxRecordBytes().
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

TEST_F(PipeWireCodecTest, ABigProgramArchiveDoesNotGrowItsRecordAtAll) {
    // THE POINT OF R-10's "every blob goes through SEG_STAGE": create_shader_state's RECORD is
    // 8 + sizeof(MGPProgramDesc) == 200 bytes whether the archive is one kilobyte or one
    // megabyte, because the record carries {Seg, Offset, Size} and nothing else. So
    // create_shader_state is one of the SMALLEST records in the catalogue, not the one most
    // likely to approach MaxRecordBytes(); what approaches that cap is a var-tail, and the
    // archive's own bound is MOBILEGL_IPC_STAGE_MB with a Fatal of its own.
    Wire2 wire;
    MG_State::GLState::LinkArtifacts link;
    MG_State::GLState::SpirvArtifacts spirv;
    spirv.generatedSpirv.resize(1);
    spirv.generatedSpirv[0].assign(8 * 1024, 0x07230203u); // 32 KiB of module words
    Vector<Uint8> archive;
    MG_State::GLState::EncodeProgramArtifacts(link, spirv, archive);
    ASSERT_GT(archive.size(), 32u * 1024u);

    MGPProgramDesc desc{};
    desc.Cso = MakeHandle(91);
    desc.Reflection = wire.Encoder().StageBytes(archive.data(), archive.size());
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::CreateShaderState, &desc, sizeof(desc)),
              kInvalidSeq);
    EXPECT_EQ(wire.Encoder().MaxRecordBytesSeen(), 8u + sizeof(MGPProgramDesc));
    EXPECT_GE(wire.Encoder().StagedBytesInFlight(), archive.size());
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

    // ---- AND AN EMPTY STAGE TAKES THE WHOLE SEGMENT ---------------------------------
    // The verifier's extension of this case, kept (wave1-codex-verify.md §1). The 64-byte
    // run has retired and in-flight bytes are ZERO, so every byte of SEG_STAGE is free -
    // but head and tail are monotonic and both sit at 64, so `head % capacity` is 64 and the
    // allocator used to charge a `capacity - 64` wrap skip against a capacity that had
    // nothing in it. The test then read `2*capacity - 64 <= capacity`, false at every
    // occupancy, and a blob the segment holds WHOLE aborted with
    // `Fatal{RingOverrun, "SEG_STAGE"} ... with 0 bytes still in flight`.
    //
    // I made it red once, by doing X: X = deleting the `RebaseEmptyStage()` call at the top
    // of StageAllocate's attempt loop (PipeWireCodec.cpp). The case then dies with SIGABRT
    // inside PipeWireEncoder::StageAllocate on that message, exactly as the verifier
    // recorded it.
    //
    // THE EXACT MAXIMUM. `need = Align8(size)` and the first bound is `need > capacity`, so
    // an empty stage takes a blob of exactly the capacity the encoder adopted - here
    // Wire2::kStageBytes, and in a real session the whole SEG_STAGE view, i.e.
    // MOBILEGL_IPC_STAGE_MB (32 MiB by default; SessionRings.h keeps SEG_STAGE un-ringed and
    // un-rounded, so there is no control page to subtract).
    const Uint64 maxRecordBefore = wire.Encoder().MaxRecordBytesSeen();
    std::vector<std::uint8_t> whole(Wire2::kStageBytes, 0x5A);
    const MGPBlobRef full = wire.Encoder().StageBytes(whole.data(), whole.size());
    EXPECT_EQ(full.Offset, 0u) << "an empty stage must hand a whole-capacity blob offset zero";
    EXPECT_EQ(full.Size, Wire2::kStageBytes);
    EXPECT_EQ(full.Seg, static_cast<Uint32>(kSegStage));
    EXPECT_EQ(wire.Encoder().StagedBytesInFlight(), Wire2::kStageBytes);
    const void* back = wire.Segments().Resolve(full.Seg, full.Offset, full.Size);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back, wire.StageBase());

    // R-10's max-record counter DOES NOT SEE IT, and that is the point of R-10's carrier
    // rule: EncodeRecord feeds m_maxRecordBytes from `layout.TotalBytes` - header + payload +
    // tails, all of it SEG_CMD - while the blob leaves only {Seg, Offset, Size} in the
    // record. A quarter-megabyte of staging moved the counter by zero bytes. SEG_STAGE has
    // its own bound and its own named Fatal, and MaxRecordBytesSeen() is not it.
    EXPECT_EQ(wire.Encoder().MaxRecordBytesSeen(), maxRecordBefore);
}

// ---- M2 / M3: SEG_STAGE's cursors and the mark queue --------------------------------------

TEST_F(PipeWireCodecTest, TheEncoderNeverWritesSegStagesConsumerCursors) {
    // Ring.h makes stageAppliedTail / stageRetiredTail CONSUMER-owned and says the staging
    // allocator reclaims behind retiredSeq "and nothing else may". A producer writing them is
    // the same shape w1's own §8.1 argues against for appliedSeq, one segment over: if s1 ever
    // attaches a RingConsumer to RingCursorSet::Stage - the obvious thing to do for a segment
    // with a cursor triple - its m_localTail never moves (nothing Pops the stage ring) and the
    // two cursors get walked forward by the client and back by the server.
    Wire2 wire;
    const std::uint8_t payload[128] = {};
    (void)wire.Encoder().StageBytes(payload, sizeof(payload));
    MGPBindRenderState bind{};
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::BindRenderState, &bind, sizeof(bind)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    wire.Encoder().ReclaimStagedBytes();
    EXPECT_EQ(wire.Encoder().StagedBytesInFlight(), 0u) << "the reclaim did not run at all";

    EXPECT_EQ(wire.Control().stageHead.load(), 0u);
    EXPECT_EQ(wire.Control().stageAppliedTail.load(), 0u);
    EXPECT_EQ(wire.Control().stageRetiredTail.load(), 0u);
}

TEST_F(PipeWireCodecTest, TheStageMarkQueueStaysBoundedWithOneRecordAlwaysInFlight) {
    // The steady-state leak: the queue used to be cleared ONLY when it drained completely, so
    // one unretired record at every reclaim meant front < size() for ever and 16 bytes per
    // encoded record for the life of the context. That is exactly the regime W-3 says the
    // design exists to survive once R-1's barrier retires family by family - and no SSIM
    // comparison and no two-scenario lane would ever see it.
    Wire2 wire;
    MGPBindRenderState bind{};
    const std::uint8_t blob[32] = {};

    // PRIMING IS THE WHOLE POINT. Encoding and applying one record per iteration drains the
    // queue at every reclaim, which is the one regime the old "clear only when front reaches
    // size()" code handled - a case written that way stays green against the bug. One record
    // encoded ahead of the one being applied is what makes `front < size()` permanent.
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::BindRenderState, &bind, sizeof(bind)),
              kInvalidSeq);
    for (int i = 0; i < 4000; ++i) {
        (void)wire.Encoder().StageBytes(blob, sizeof(blob));
        ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::BindRenderState, &bind, sizeof(bind)),
                  kInvalidSeq);
        bool applied = false;
        ASSERT_TRUE(wire.PumpOne(&applied)) << "record " << i;
    }
    EXPECT_EQ(wire.Encoder().EmitSeq(), 4001u);
    EXPECT_EQ(wire.Decoder().AppliedSeq(), 4000u) << "one record must still be in flight";
    // Bounded by twice the records actually in flight, not by the records ever encoded.
    EXPECT_LE(wire.Encoder().StageMarksHeld(), 4u)
        << "the mark queue is tracking history rather than flight";
}

// ---- M4: the reply slot is for kReplySlot rows only ----------------------------------------

TEST_F(PipeWireCodecTest, NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot) {
    // s1 sizes ReplyPool from MGPipeCallFlagsFor, so a reply written for a record the pool
    // reserved no slot for overwrites a waiter's answer - and because the slot header stamps
    // the WRITER's seq for self-check, the waiter's check then fails for ever and the barrier
    // HANGS rather than returning something wrong.
    Wire2 wire;
    ASSERT_EQ(MGPipeCallFlagsFor(MGPWireOp::ResourceCreate) & static_cast<Uint32>(kReplySlot), 0u);
    ASSERT_EQ(MGPipeCallFlagsFor(MGPWireOp::SetTextureParams) & static_cast<Uint32>(kReplySlot), 0u);

    MGPResourceDesc create{};
    create.Resource = MakeHandle(131);
    create.Target = static_cast<Uint8>(MGPipeResourceTarget::Buffer);
    create.Width = 64;
    create.Height = 1;
    create.Depth = 1;
    create.ArrayLayers = 1;
    create.Levels = 1;
    create.Samples = 1;
    ASSERT_NE(wire.Encoder().EncodeRecord(MGPWireOp::ResourceCreate, &create, sizeof(create)),
              kInvalidSeq);
    bool applied = false;
    ASSERT_TRUE(wire.PumpOne(&applied));
    EXPECT_TRUE(applied);

    EXPECT_TRUE(wire.Answers().All.empty()) << "a reply slot was written for a kNone row";
    // The acceptance answer R-5 requires is still produced - it just does not ride SEG_REPLY.
    EXPECT_TRUE(wire.Decoder().LastAcceptanceKnown());
    EXPECT_EQ(wire.Decoder().AcceptedRecords() + wire.Decoder().DeclinedRecords(), 1u);
}

TEST_F(PipeWireCodecTest, EveryRowThatDoesWriteAReplyCarriesKReplySlot) {
    // The other half: the three rows in P5 that answer into a slot all carry the flag, so
    // PostReply's gate cannot be firing on any of them.
    for (const MGPWireOp op :
         {MGPWireOp::MapPersistent, MGPWireOp::ResourceReadback, MGPWireOp::ReadPixels}) {
        EXPECT_NE(MGPipeCallFlagsFor(op) & static_cast<Uint32>(kReplySlot), 0u) << WireOpName(op);
    }
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

TEST_F(PipeWireCodecTest, AContentBlobCarriedOutsideSegStageIsFatalAtTheDecoder) {
    // R-2.3's second half, and the verifier's finding-3 fixture kept as its own case
    // (wave1-codex-verify.md §3). Contract table 1 row 17 puts CreateSamplerState's bytes in
    // SEG_STAGE; here they sit in a mapped SEG_REPLY - the SERVER-owned reply pool, whose
    // reuse has nothing to do with stage retirement - and the record names that segment. It
    // used to be ACCEPTED and APPLIED, because the only test was that the run resolved
    // somewhere: the verifier's probe printed `seg=3 accepted=1 poisoned=0`.
    //
    // The audit is armed, so the second half of the finding is nailed down too: with the
    // poison ON, the record must DIE rather than be applied with PoisonedStageBytes() left at
    // zero. NoteResolvedRun used to return silently for any non-stage carrier, which made
    // rule C's only mechanical control dark on exactly the record it exists to catch; it is
    // now a Fatal of its own and unreachable behind this arm.
    //
    // I made it red once, by doing X: X = deleting the `blob.Seg != kSegStage` arm in
    // CheckBlobIsHonest (PipeWireCodec.cpp). The child then exits 0 instead of aborting and
    // this case fails on DiedOfAbort - the verifier's `accepted=1` state.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        std::vector<std::uint8_t> replyBytes(4096, 0);
        SamplerParameters params{};
        params.borderColorForm = BorderColorForm::Int;
        std::memcpy(replyBytes.data(), &params, sizeof(params));
        wire.Segments().Install(kSegReply, SegmentView{replyBytes.data(), replyBytes.size()});
        wire.Decoder().SetAuditPoison(true);

        MGPSamplerDesc desc{};
        desc.Cso = MakeHandle(88);
        desc.Parameters.Seg = static_cast<Uint32>(kSegReply);
        desc.Parameters.Offset = 0;
        desc.Parameters.Size = sizeof(SamplerParameters);
        ForgeAndDecode(wire, MGPWireOp::CreateSamplerState, &desc, sizeof(desc), nullptr, 0);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("is not SEG_STAGE"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("CreateSamplerState.blob"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("seg=3"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, TheEncoderRefusesTheNonStageCarrierTheDecoderCallsFatal) {
    // THE ENCODER MUST NOT ACCEPT A RECORD THE DECODER FATALS ON - the same symmetry
    // TheEncoderRefusesThePerStageSpirvRunTheDecoderCallsFatal states one arm over. Under
    // `inproc` a SEG_REPLY pointer resolves, so an emitter that staged into the reply pool
    // would get a valid seq here and a Fatal on a peer, which is the asymmetry EncodeRecord's
    // own honesty loop exists to prevent.
    //
    // I made it red once, by doing X: X = deleting the `blob.Seg != kSegStage` arm in
    // CheckBlobIsHonest. EncodeRecord then returns a real seq and the child exits 0.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        std::vector<std::uint8_t> replyBytes(4096, 0);
        wire.Segments().Install(kSegReply, SegmentView{replyBytes.data(), replyBytes.size()});
        MGPSamplerDesc desc{};
        desc.Cso = MakeHandle(88);
        desc.Parameters.Seg = static_cast<Uint32>(kSegReply);
        desc.Parameters.Offset = 0;
        desc.Parameters.Size = sizeof(SamplerParameters);
        (void)wire.Encoder().EncodeRecord(MGPWireOp::CreateSamplerState, &desc, sizeof(desc));
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("is not SEG_STAGE"), std::string::npos) << r.Log;
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

// ---- M1: R-2 arm 4 over MGHostSpan, on both host-span rows ------------------------------

TEST_F(PipeWireCodecTest, AHostSpanRunPastItsSegmentIsFatalOnSetShaderBuffers) {
    // Before the fix this child exited 0: CheckHostSpanIsHonest took no SegmentTable and no
    // caller resolved, so arm 4 was implemented for blobrefs only. The span names a REAL
    // segment and a run that leaves it.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPShaderBuffers header{};
        header.Count = 1;
        header.HostSpanCount = 1;
        MGPBufferRange range{};
        MGHostSpan span{};
        span.Ptr = nullptr; // rule B satisfied, so only arm 4 can catch this
        span.Seg = kSegStage;
        span.Offset = Wire2::kStageBytes - 8;
        span.Size = 1024;
        std::vector<std::uint8_t> tail(sizeof(range) + sizeof(span));
        std::memcpy(tail.data(), &range, sizeof(range));
        std::memcpy(tail.data() + sizeof(range), &span, sizeof(span));
        ForgeAndDecode(wire, MGPWireOp::SetShaderBuffers, &header, sizeof(header), tail.data(),
                       tail.size());
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{ProtocolCorruption, \"host-span\"}"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("does not lie inside that segment (R-2.3 arm 4)"), std::string::npos)
        << r.Log;
}

TEST_F(PipeWireCodecTest, AHostSpanRunPastItsSegmentIsFatalOnDrawVbo) {
    // The same span on the other host-span row - the one whose sink WireVerbSink's header
    // promises is handed "a DECODED, VALIDATED argument list". Before the fix it reached
    // OnDrawVbo.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPDrawInfo info{};
        info.Mode = 4;
        info.IndexSize = 2;
        info.Flags = kDrawHasUserIndices;
        info.NumDraws = 1;
        MGPDrawRange ranges[1] = {{0, 4, 0}};
        MGHostSpan span{};
        span.Ptr = nullptr;
        span.Seg = kSegStage;
        span.Offset = 0xFFFFFFFFull;
        span.Size = 0xFFFFu;
        // The layout realigns the span to 8 after a 12-byte MGPDrawRange, so the forged tail
        // has to carry the same four pad bytes the encoder would.
        std::vector<std::uint8_t> tail(16 + sizeof(span), 0);
        std::memcpy(tail.data(), ranges, sizeof(ranges));
        std::memcpy(tail.data() + 16, &span, sizeof(span));
        ForgeAndDecode(wire, MGPWireOp::DrawVbo, &info, sizeof(info), tail.data(), tail.size());
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("does not lie inside that segment (R-2.3 arm 4)"), std::string::npos)
        << r.Log;
}

// ---- M5 / m8 / q2 ------------------------------------------------------------------------

TEST_F(PipeWireCodecTest, BufferSubDataResidentMayNotDeclareRegions) {
    // Its catalogue row has no kVarTail and its applier takes no regions, so a non-zero
    // RegionCount is a fault rather than a tail. The layout used to share ResourceSubData's
    // arm, which REQUIRED 80 bytes of tail the arm then dropped.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPSubData rec{};
        rec.Res = MakeHandle(7);
        rec.RegionCount = 2;
        MGPipeSetSubDataBufferRange(rec, 0, 64);
        rec.RegionCount = 2; // after the helper, which zeroes it
        ForgeAndDecode(wire, MGPWireOp::BufferSubDataResident, &rec, sizeof(rec), nullptr, 0);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("BufferSubDataResident.RegionCount"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, TheEncoderRefusesThePerStageSpirvRunTheDecoderCallsFatal) {
    // The rule lived only in the decoder, so an emitter that declared a per-stage run got a
    // valid seq here and a Fatal on a peer that could only report "corrupt stream".
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
        (void)wire.Encoder().EncodeRecord(MGPWireOp::CreateShaderState, &desc, sizeof(desc));
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("declares 16 bytes at the ENCODER"), std::string::npos) << r.Log;
}

TEST_F(PipeWireCodecTest, APartiallyZeroUploadBoxIsFatalRatherThanReadAsEmpty) {
    // The content predicate used to be "all three extents non-zero", so a 4x4x0 box was read
    // as carrying nothing and rule A's arm 2 sat out for a record that named a real
    // destination. A box is either empty or whole.
    const ChildResult r = RunInChild([] {
        Wire2 wire;
        MGPSubData rec{};
        rec.Res = MakeHandle(9);
        rec.Target = MGPipePackSubDataTarget(static_cast<Uint32>(MGPipeResourceTarget::Tex2D), 0u);
        rec.UnionBox = MGPBox{0, 0, 0, 4, 4, 0};
        ForgeAndDecode(wire, MGPWireOp::ResourceSubData, &rec, sizeof(rec), nullptr, 0);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("has a zero extent on some axes and not others"), std::string::npos)
        << r.Log;
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

// ---- R-6 / contract §5: the two forbidden adoption tiers die ON THE WIRE PATH TOO --------
//
// wave1-codex-verify.md §4: `AdoptTier` had ZERO references anywhere on the codec path, so
// MOBILEGL_IPC_ADOPT_TIER=0 and =1 - which contract §5 promises "parse and are Fatal at use,
// naming P11" - decoded as an ordinary DECLINED. The verifier set each forbidden tier inside
// KReplySlotMapPersistentIsAConstantDecline and watched its successful-decline assertions
// still pass, on BOTH tiers.
//
// THESE ARE FORKED, NOT EXPECT_DEATH, for the reason at the top of this file - and forking is
// what lets the case REQUIRE THE DIAGNOSTIC rather than any abort: r.Log is searched for the
// exact sentence AdoptTierIsEmulate prints. ID-46 finding 10 is an empty death regex; the
// EXPECT_NE lines below are the opposite of that, and a crash for any other reason fails the
// case on the log it prints.
//
// I made both red once, by doing X: X = restoring the unconditional decline in
// PipeWireCodec.cpp's MapPersistent arm (deleting the AdoptTierIsEmulate call). Both children
// then exit 0 having posted a clean DECLINED, and both cases fail on DiedOfAbort.

TEST_F(PipeWireCodecTest, AdoptTierZeroIsFatalOnTheWirePathAndNamesP11) {
    const ChildResult r = RunInChild([] {
        // The child dies; nothing needs restoring. The transport half is the same conjunction
        // MGPipeApplyMapPersistent uses - a monolith TRANSPORT mints like push (ID-42) and is
        // not the arm this record can arrive on.
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MG_Config::Ipc.AdoptTier = 0u;
        Wire2 wire;
        const MGPHandleOnly handle = HandleOnly(5, MGPipeKind::Buffer);
        (void)wire.Encoder().EncodeRecord(MGPWireOp::MapPersistent, &handle, sizeof(handle));
        bool applied = false;
        (void)wire.PumpOne(&applied);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("MOBILEGL_IPC_ADOPT_TIER=0 names adoption tier T0, which P11 implements"),
              std::string::npos)
        << r.Log;
}

TEST_F(PipeWireCodecTest, AdoptTierOneIsFatalOnTheWirePathAndNamesP11) {
    const ChildResult r = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MG_Config::Ipc.AdoptTier = 1u;
        Wire2 wire;
        const MGPHandleOnly handle = HandleOnly(5, MGPipeKind::Buffer);
        (void)wire.Encoder().EncodeRecord(MGPWireOp::MapPersistent, &handle, sizeof(handle));
        bool applied = false;
        (void)wire.PumpOne(&applied);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("MOBILEGL_IPC_ADOPT_TIER=1 names adoption tier T1, which P11 implements"),
              std::string::npos)
        << r.Log;
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
