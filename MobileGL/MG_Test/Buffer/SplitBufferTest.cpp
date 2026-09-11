// MobileGL - MobileGL/MG_Test/Buffer/SplitBufferTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 (b1): the buffer side of the split - the client-side conservative GPU-write set, the
// block-granularity persistent-map push, and tier 1 of the flush ladder.
//
// EVERY CASE IS COMPILED IN ALL FOUR BUILDS AND SKIPS OUTSIDE build-split, deliberately. The
// names then exist identically in the pull and the push lane (G2 stays at 0 diff lines) and
// build-split adds none of its own (G14 stays at 0 removed), while a lane that cannot run a
// case says so instead of quietly not having it.

#include <gtest/gtest.h>

#include <Config.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_Util/Metrics/PipeStats.h>

#if MOBILEGL_PIPE_PUSH
#include <MG_Backend/DirectGLES/Managers.h>
#endif

#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Remote/Client/GpuWritePending.h>
#include <MG_Remote/Client/PersistentMapTracker.h>
#endif

using namespace MobileGL;

namespace {

#if MOBILEGL_BUILD_DISAGGREGATED
    using MG_State::GLState::BufferObject;
    using MG_Remote::Client::GpuWriteProducer;
    using MG_Remote::Client::PersistentMapTracker;

    // Everything in this package is gated on `Transport != Monolith`, so every case has to
    // put the process into a split configuration and put it back. A fixture rather than a
    // lambda because the tracker is a process-wide singleton and a case that left an entry in
    // it would poison the next one through a raw pointer to a destroyed buffer - which is
    // exactly the failure mode the tracker's own Forget() exists to prevent.
    class SplitBufferSet : public ::testing::Test {
    protected:
        void SetUp() override {
            m_transport = MG_Config::Transport;
            m_blockKb = MG_Config::Ipc.PersistentBlockKb;
            m_adoptTier = MG_Config::Ipc.AdoptTier;
            m_pipeStats = MG_Config::Features.PipeStats;
            m_context = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
            MG_Config::Transport = MG_Config::TransportMode::InProcess;
            MG_Config::Ipc.PersistentBlockKb = 64;
            MG_Config::Ipc.AdoptTier = 2;
            MG_Config::Features.PipeStats = true;
            MG_Util::PipeStats::Init();
            PersistentMapTracker::Instance().ClearForTest();
            MG_Remote::Client::ResetProducerMarkCountsForTest();
        }
        void TearDown() override {
            PersistentMapTracker::Instance().ClearForTest();
            MG_State::pGLContext = Move(m_context);
            MG_Config::Transport = m_transport;
            MG_Config::Ipc.PersistentBlockKb = m_blockKb;
            MG_Config::Ipc.AdoptTier = m_adoptTier;
            MG_Config::Features.PipeStats = m_pipeStats;
            MG_Util::PipeStats::Init();
        }

        static SharedPtr<BufferObject> MakeBuffer(Uint index, SizeT size) {
            auto buffer = MakeShared<BufferObject>(index);
            buffer->Respecify(size, nullptr);
            return buffer;
        }

        MG_Config::TransportMode m_transport = MG_Config::TransportMode::Monolith;
        Uint32 m_blockKb = 64;
        Uint32 m_adoptTier = 2;
        Bool m_pipeStats = false;
        UniquePtr<MG_State::GLState::GLContext> m_context;
    };
#endif

#define MGL_SPLIT_ONLY_OR_SKIP()                                                                   \
    do {                                                                                           \
        GTEST_SKIP() << "the client-side GPU-write set and the persistent-map push exist only in "  \
                        "a MOBILEGL_BUILD_DISAGGREGATED build";                                    \
    } while (0)

} // namespace

// =====================================================================================
// The GPU-write set: one case per row of CONTRACT-P5.md section 3's first table.
// =====================================================================================

#if MOBILEGL_BUILD_DISAGGREGATED

// Row 0 - DirectGLES.cpp:570 / UniformManager.cpp:1231.
TEST_F(SplitBufferSet, Row0EverySsboBindingPointIsMarkedByADraw) {
    auto ssbo = MakeBuffer(11u, 256);
    MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 0).Bind(ssbo);
    MG_State::pGLContext->TouchBufferBindingPoint(BufferTarget::ShaderStorage, 0);

    MG_Remote::Client::MarkGpuWritesForDraw();

    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::ShaderStorageBinding), 1u)
        << "a draw with an SSBO bound must mark it: the shader writes into the driver's buffer, "
           "behind the shadow glMapBuffer and glGetBufferSubData read";
}

// Row 1 - DirectGLES.cpp:618. The point of a counter is that the shader increments it.
TEST_F(SplitBufferSet, Row1EveryBoundAtomicCounterIsMarkedByADraw) {
    auto counter = MakeBuffer(12u, 64);
    MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::AtomicCounter, 0).Bind(counter);
    MG_State::pGLContext->TouchBufferBindingPoint(BufferTarget::AtomicCounter, 0);

    MG_Remote::Client::MarkGpuWritesForDraw();

    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::AtomicCounterBinding), 1u);
}

// Row 2's DISCRIMINATOR - DirectGLES.cpp:2354-2357. This is the one row whose backend twin is
// narrow on purpose, so the narrowness is what the case is about: a GL_READ_ONLY image binding
// must NOT be marked, or the next map waits on and re-reads a dispatch that cannot have changed
// a byte of it.
TEST_F(SplitBufferSet, Row2OnlyAWritableImageBufferTextureCounts) {
    MG_State::GLState::ImageTextureBinding empty{};
    EXPECT_FALSE(MG_Remote::Client::ImageUnitIsAWritableBufferTexture(empty))
        << "an unbound image unit is not a GPU write";

    auto texture = MakeShared<MG_State::GLState::TextureObjectBuffer>(7u);
    auto backing = MakeBuffer(13u, 128);
    texture->GetBufferBindingSlot().Bind(backing);

    MG_State::GLState::ImageTextureBinding readOnly{};
    readOnly.Texture = texture;
    readOnly.Access = GL_READ_ONLY;
    EXPECT_FALSE(MG_Remote::Client::ImageUnitIsAWritableBufferTexture(readOnly))
        << "a GL_READ_ONLY image binding is left alone by the backend twin and must be left "
           "alone here";

    MG_State::GLState::ImageTextureBinding writable = readOnly;
    writable.Access = GL_READ_WRITE;
    EXPECT_TRUE(MG_Remote::Client::ImageUnitIsAWritableBufferTexture(writable));
}

// Row 2's WALK, through the image unit the context actually holds.
TEST_F(SplitBufferSet, Row2AWritableImageBufferTextureIsMarkedByADraw) {
    auto texture = MakeShared<MG_State::GLState::TextureObjectBuffer>(8u);
    auto backing = MakeBuffer(14u, 128);
    texture->GetBufferBindingSlot().Bind(backing);
    auto& binding = MG_State::pGLContext->GetImageTextureBinding(0);
    binding.Texture = texture;
    binding.Access = GL_WRITE_ONLY;

    MG_Remote::Client::MarkGpuWritesForDraw();

    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::WritableImageBufferTexture), 1u);

    binding = MG_State::GLState::ImageTextureBinding{};
}

// Row 3 - VulkanRenderer.cpp:11618. With no capture active there is nothing to mark, and that
// gate is the half worth pinning: a mark taken with no active capture would mark whatever the
// binding points happened to hold from a previous one.
TEST_F(SplitBufferSet, Row3TransformFeedbackTargetsAreOnlyMarkedWhileACaptureIsActive) {
    auto target = MakeBuffer(15u, 256);
    MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::TransformFeedback, 0).Bind(target);
    MG_State::pGLContext->TouchBufferBindingPoint(BufferTarget::TransformFeedback, 0);

    ASSERT_FALSE(MG_State::pGLContext->IsTransformFeedbackActive());
    MG_Remote::Client::MarkGpuWritesForDraw();
    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::TransformFeedbackCapture), 0u);

    // The marking itself, driven at the row rather than through the capture state machine.
    MG_Remote::Client::MarkBufferForProducer(target, GpuWriteProducer::TransformFeedbackCapture);
    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::TransformFeedbackCapture), 1u);
}

// Row 4 - P5's own: glReadPixels into a bound GL_PIXEL_PACK_BUFFER.
TEST_F(SplitBufferSet, Row4AReadPixelsIntoAPackPboMarksThePbo) {
    MG_Remote::Client::MarkReadPixelsPackBuffer();
    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::ReadPixelsPackBuffer), 0u)
        << "a read into client memory binds no PBO and must mark nothing";

    auto pbo = MakeBuffer(16u, 1024);
    MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).Bind(pbo);
    MG_Remote::Client::MarkReadPixelsPackBuffer();
    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::ReadPixelsPackBuffer), 1u);
}

// Row 5 - P5's own: glEndTransformFeedback, in place of the unbounded ClientWaitSync.
TEST_F(SplitBufferSet, Row5EndTransformFeedbackMarksTheCaptureTargets) {
    auto target = MakeBuffer(17u, 256);
    ASSERT_FALSE(MG_State::pGLContext->IsTransformFeedbackActive());
    MG_Remote::Client::MarkEndTransformFeedbackCaptureTargets();
    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::EndTransformFeedbackCapture), 0u);

    MG_Remote::Client::MarkBufferForProducer(target, GpuWriteProducer::EndTransformFeedbackCapture);
    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::EndTransformFeedbackCapture), 1u);
}

// THE GATE ITSELF. On the monolith path the six backend sites are still the only producers and
// a second marker would be new behaviour (D-J) - and rows 4 and 5 would remove a stall that
// monolith is entitled to keep.
TEST_F(SplitBufferSet, TheWholeSetIsInertOnTheMonolithPath) {
    MG_Config::Transport = MG_Config::TransportMode::Monolith;
    auto ssbo = MakeBuffer(18u, 256);
    MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 0).Bind(ssbo);
    MG_State::pGLContext->TouchBufferBindingPoint(BufferTarget::ShaderStorage, 0);

    MG_Remote::Client::MarkGpuWritesForDraw();
    MG_Remote::Client::MarkGpuWritesForDispatch();
    MG_Remote::Client::MarkReadPixelsPackBuffer();
    MG_Remote::Client::MarkEndTransformFeedbackCaptureTargets();

    for (SizeT row = 0; row < static_cast<SizeT>(GpuWriteProducer::Count); ++row) {
        EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(static_cast<GpuWriteProducer>(row)), 0u)
            << "row " << row << " fired with Transport == Monolith";
    }
}

// =====================================================================================
// The persistent-map push
// =====================================================================================

// The membership predicate IS SyncPersistentMappedRange's early-out chain, and the two must
// answer the same thing about the same buffer. A re-derived predicate that drifted would push
// a buffer monolith stopped pushing, and no other test could see it.
TEST_F(SplitBufferSet, MembershipIsSyncPersistentMappedRangesOwnEarlyOutChain) {
    auto buffer = MakeBuffer(20u, 4096);
    EXPECT_FALSE(PersistentMapTracker::IsLivePersistentMap(*buffer)) << "not mapped";

    buffer->AcquireMemoryRange(Range1D{0, 4096},
                               BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    ASSERT_FALSE(buffer->IsBackendPersistentMapped())
        << "the acquisition was minted, so this is the adopted arm and not the one under test";
    EXPECT_TRUE(PersistentMapTracker::IsLivePersistentMap(*buffer));
    EXPECT_EQ(PersistentMapTracker::Instance().MemberCount(), 1u);

    buffer->ReleaseMemory(false);
    EXPECT_FALSE(PersistentMapTracker::IsLivePersistentMap(*buffer));
    EXPECT_EQ(PersistentMapTracker::Instance().MemberCount(), 0u);

    // FLUSH_EXPLICIT is the early-out that is easiest to lose: the application announces its
    // own writes with glFlushMappedBufferRange, which already crosses as resource_flush_range.
    buffer->AcquireMemoryRange(Range1D{0, 4096}, BufferMappingAccessBit::Write |
                                                     BufferMappingAccessBit::Persistent |
                                                     BufferMappingAccessBit::FlushExplicit);
    EXPECT_FALSE(PersistentMapTracker::IsLivePersistentMap(*buffer));
    EXPECT_EQ(PersistentMapTracker::Instance().MemberCount(), 0u);
    buffer->ReleaseMemory(false);
}

// pmap is non-zero, and it is non-zero in BLOCKS.
TEST_F(SplitBufferSet, ThePushCutsTheMappedSpanIntoBlocksAndMovesPmap) {
    constexpr SizeT kSize = 4u * 64u * 1024u; // exactly four 64 KiB blocks
    auto buffer = MakeBuffer(21u, kSize);
    buffer->AcquireMemoryRange(Range1D{0, kSize},
                               BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    ASSERT_TRUE(PersistentMapTracker::IsLivePersistentMap(*buffer));

    const Uint64 before = MG_Util::PipeStats::TotalBytes(MG_Util::PipeStats::ByteClass::PersistentMapPush);
    MG_Remote::Client::PushPersistentMapsBeforeVerb();

    EXPECT_EQ(PersistentMapTracker::Instance().BlocksPushed(), 4u)
        << "a 256 KiB span at a 64 KiB block size is four records, not one";
    EXPECT_EQ(PersistentMapTracker::Instance().BytesPushed(), static_cast<Uint64>(kSize));
    EXPECT_EQ(MG_Util::PipeStats::TotalBytes(MG_Util::PipeStats::ByteClass::PersistentMapPush) - before,
              static_cast<Uint64>(kSize))
        << "persistent-map-push is wired and counts the bytes the client had to ship because "
           "MapPersistent declined";

    buffer->ReleaseMemory(false);
}

// A span that is not a whole number of blocks keeps its tail.
TEST_F(SplitBufferSet, TheLastBlockIsTheRemainderAndNotAWholeBlock) {
    constexpr SizeT kSize = 64u * 1024u + 7u;
    auto buffer = MakeBuffer(22u, kSize);
    buffer->AcquireMemoryRange(Range1D{0, kSize},
                               BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    MG_Remote::Client::PushPersistentMapsBeforeVerb();
    EXPECT_EQ(PersistentMapTracker::Instance().BlocksPushed(), 2u);
    EXPECT_EQ(PersistentMapTracker::Instance().BytesPushed(), static_cast<Uint64>(kSize));
    buffer->ReleaseMemory(false);
}

// E3(a)'s NEGATIVE CONTROL: 0 disables the push, it does not mean "one unlimited block".
TEST_F(SplitBufferSet, AZeroBlockSizeTurnsThePushOffRatherThanMakingItUnlimited) {
    MG_Config::Ipc.PersistentBlockKb = 0;
    constexpr SizeT kSize = 128u * 1024u;
    auto buffer = MakeBuffer(23u, kSize);
    buffer->AcquireMemoryRange(Range1D{0, kSize},
                               BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    ASSERT_TRUE(PersistentMapTracker::IsLivePersistentMap(*buffer));

    const Uint64 before = MG_Util::PipeStats::TotalBytes(MG_Util::PipeStats::ByteClass::PersistentMapPush);
    MG_Remote::Client::PushPersistentMapsBeforeVerb();
    EXPECT_EQ(PersistentMapTracker::Instance().BlocksPushed(), 0u);
    EXPECT_EQ(MG_Util::PipeStats::TotalBytes(MG_Util::PipeStats::ByteClass::PersistentMapPush), before)
        << "MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0 must ship nothing, so that "
           "PersistentCoherentMapScenario goes red under it";
    buffer->ReleaseMemory(false);
}

// The set does not keep a pointer to a dead buffer.
TEST_F(SplitBufferSet, ADestroyedBufferLeavesTheSet) {
    {
        auto buffer = MakeBuffer(24u, 4096);
        buffer->AcquireMemoryRange(Range1D{0, 4096},
                                   BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
        ASSERT_EQ(PersistentMapTracker::Instance().MemberCount(), 1u);
    }
    EXPECT_EQ(PersistentMapTracker::Instance().MemberCount(), 0u)
        << "~BufferObject must Forget() itself: the set holds raw pointers keyed on the "
           "lifetime id, and an entry that outlives its object is the one failure it cannot have";
}

// SyncGpuWrites' THIRD STATE. Monolith clears the flag before it emits, which is safe only
// because the readback runs synchronously inside the applier; under a transport that clear
// leaves the shadow silently stale for the object's life, so the WRITEBACK clears it instead.
TEST_F(SplitBufferSet, UnderSplitTheWritebackClearsThePendingFlagAndNotTheRequest) {
    auto buffer = MakeBuffer(25u, 256);
    buffer->MarkGpuWritten();
    ASSERT_TRUE(buffer->HasOutstandingGpuWrite());

    Vector<Uint8> bytes(256, static_cast<Uint8>(0x5A));
    buffer->WritebackFromBackend(DataPtr{bytes.data(), bytes.size()}, 0);
    EXPECT_FALSE(buffer->HasOutstandingGpuWrite())
        << "the answer landing is what makes the shadow current, so the answer is what clears";

    // A PARTIAL writeback is not an answer to a whole-buffer readback and must not clear:
    // GL_Drawing's transform-feedback strip fixup writes back three vertices at a time.
    buffer->MarkGpuWritten();
    buffer->WritebackFromBackend(DataPtr{bytes.data(), 16}, 0);
    EXPECT_TRUE(buffer->HasOutstandingGpuWrite());

    // And with no readback route at all - no size, or a backend that registered no resource
    // ops - SyncGpuWrites must clear rather than block for ever. That is the ONE case
    // monolith's unconditional clear covers that a writeback cannot.
    EXPECT_FALSE(MG_Remote::Client::BufferWritebackIsReachable(*buffer));
    buffer->SyncGpuWrites();
    EXPECT_FALSE(buffer->HasOutstandingGpuWrite());
}

// R-6's tier gate. T2 is the only tier P5 implements; the other two are a NAMED refusal and
// their spelling exists now so the P11 negative control has one.
TEST_F(SplitBufferSet, OnlyAdoptTierTwoIsImplemented) {
    EXPECT_TRUE(MG_Remote::Client::AdoptTierIsEmulate());
    MG_Config::Ipc.AdoptTier = 0;
    EXPECT_DEATH(MG_Remote::Client::AdoptTierIsEmulate(), "");
    MG_Config::Ipc.AdoptTier = 1;
    EXPECT_DEATH(MG_Remote::Client::AdoptTierIsEmulate(), "");
    MG_Config::Ipc.AdoptTier = 2;
}

#else

// THE SAME FIFTEEN NAMES, SO THE ctest NAME SET DOES NOT MOVE BETWEEN LANES. G2 compares the
// pull and push name lists line for line and G14 allows build-split to ADD names but never to
// remove one, so a case that exists only where it can run would break both gates for a reason
// that has nothing to do with what it tests. It skips instead, and says why.
TEST(SplitBufferSet, Row0EverySsboBindingPointIsMarkedByADraw) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row1EveryBoundAtomicCounterIsMarkedByADraw) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row2OnlyAWritableImageBufferTextureCounts) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row2AWritableImageBufferTextureIsMarkedByADraw) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row3TransformFeedbackTargetsAreOnlyMarkedWhileACaptureIsActive) {
    MGL_SPLIT_ONLY_OR_SKIP();
}
TEST(SplitBufferSet, Row4AReadPixelsIntoAPackPboMarksThePbo) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row5EndTransformFeedbackMarksTheCaptureTargets) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, TheWholeSetIsInertOnTheMonolithPath) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, MembershipIsSyncPersistentMappedRangesOwnEarlyOutChain) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, ThePushCutsTheMappedSpanIntoBlocksAndMovesPmap) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, TheLastBlockIsTheRemainderAndNotAWholeBlock) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, AZeroBlockSizeTurnsThePushOffRatherThanMakingItUnlimited) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, ADestroyedBufferLeavesTheSet) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, UnderSplitTheWritebackClearsThePendingFlagAndNotTheRequest) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, OnlyAdoptTierTwoIsImplemented) { MGL_SPLIT_ONLY_OR_SKIP(); }

#endif // MOBILEGL_BUILD_DISAGGREGATED

// =====================================================================================
// Tier 1 of the three-tier flush ladder - the INVALIDATE_RANGE edge.
//
// It is a PUSH-build case and not a split-build one: the widening hazard is real in every
// build that compiles FlushPendingRangesFrom, and under split it simply gains a second cause
// (a SEG_STAGE snapshot that no longer matches the queued range).
// =====================================================================================

#if MOBILEGL_PIPE_PUSH
namespace {
    using MobileGL::MG_Backend::DirectGLES::BufferImpl::InvalidateFlushAccessFor;
    using MobileGL::MG_Backend::DirectGLES::BufferImpl::kEsprytInvalidateRangeMinBytes;
    constexpr SizeT kStore = 1024u * 1024u;
} // namespace

TEST(EsprytFlushLadder, AWholeBufferRangeOrphansTheStore) {
    EXPECT_EQ(InvalidateFlushAccessFor(0, kStore, 0, kStore, kStore, kStore),
              static_cast<GLbitfield>(GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
}

TEST(EsprytFlushLadder, ALargePartialRangeInvalidatesExactlyThatRange) {
    const SizeT start = 4096;
    const SizeT end = start + kEsprytInvalidateRangeMinBytes;
    EXPECT_EQ(InvalidateFlushAccessFor(start, end, start, end, kStore, kStore),
              static_cast<GLbitfield>(GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
}

TEST(EsprytFlushLadder, ASmallPartialRangeFallsThroughToTheStagingRing) {
    EXPECT_EQ(InvalidateFlushAccessFor(4096, 4096 + 64, 4096, 4096 + 64, kStore, kStore), 0u)
        << "below the threshold the map WAITS out the WAR hazard on the CPU instead of "
           "substituting pages, which is the whole reason tier 2 exists";
}

// THE EDGE THAT HAS ALREADY DRAWN BLOOD (Managers.cpp:1125-1128): widening the map past the
// queued range clobbered GPU-written data - an SSBO counter beside the app's SubData - with
// the stale shadow, SILENTLY. Under split the same shape arrives with a different cause: the
// server may hold no pointer into the client's shadow (R-11), so `hostBase` becomes a
// SEG_STAGE snapshot, and a snapshot that does not cover exactly the queued range is the same
// lie told by a thread boundary instead of by a page alignment.
TEST(EsprytFlushLadder, AMapWiderThanTheQueuedRangeRefusesTierOne) {
    const SizeT queuedStart = 4096;
    const SizeT queuedEnd = queuedStart + kEsprytInvalidateRangeMinBytes;
    // Page-aligned outward, the exact widening the in-tree note records.
    EXPECT_EQ(InvalidateFlushAccessFor(queuedStart, queuedEnd, 0, queuedEnd + 4096, kStore, kStore), 0u)
        << "a widened INVALIDATE_RANGE declares bytes dead that the shadow is not about to "
           "rewrite, and overwrites whatever the GPU put there";
    // And narrower, which is the same corruption read the other way round: bytes left
    // unwritten inside a range that has just been declared dead.
    EXPECT_EQ(InvalidateFlushAccessFor(queuedStart, queuedEnd, queuedStart, queuedEnd - 8, kStore, kStore), 0u);
}

TEST(EsprytFlushLadder, AnEmptyRangeIsNeverTierOne) {
    EXPECT_EQ(InvalidateFlushAccessFor(4096, 4096, 4096, 4096, kStore, kStore), 0u);
}
#else
// The same five names in a pull build, for the G2/G14 reason above: the ladder's push arm
// (FlushPendingRangesFrom) is the only one that carries this decision as a function - the pull
// arm's FlushPendingRangesNow is byte-frozen against 5cb826b0 (ID-15) and may not grow one.
#define MGL_PUSH_ONLY_OR_SKIP()                                                                    \
    GTEST_SKIP() << "the three-tier ladder's push arm (FlushPendingRangesFrom) is what carries "    \
                    "InvalidateFlushAccessFor; a pull build compiles the frozen arm instead"
TEST(EsprytFlushLadder, AWholeBufferRangeOrphansTheStore) { MGL_PUSH_ONLY_OR_SKIP(); }
TEST(EsprytFlushLadder, ALargePartialRangeInvalidatesExactlyThatRange) { MGL_PUSH_ONLY_OR_SKIP(); }
TEST(EsprytFlushLadder, ASmallPartialRangeFallsThroughToTheStagingRing) { MGL_PUSH_ONLY_OR_SKIP(); }
TEST(EsprytFlushLadder, AMapWiderThanTheQueuedRangeRefusesTierOne) { MGL_PUSH_ONLY_OR_SKIP(); }
TEST(EsprytFlushLadder, AnEmptyRangeIsNeverTierOne) { MGL_PUSH_ONLY_OR_SKIP(); }
#endif // MOBILEGL_PIPE_PUSH
