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

    // A backend that MINTS a persistent mapping, for the one case that needs the adopted arm
    // (TheAdoptedArmIsNotAMemberAndItIsTheChainRowThatSaysSo). Only AcquirePersistentMap is
    // filled in: the frontend null-checks every op individually, and a table with one live
    // member is the smallest thing that makes AcquireMemoryRange's legacy adoption arm fire.
    // The storage is the CASE's, not this table's - AdoptPersistentMap keeps the pointer and
    // never owns it - so the base travels through a file-scope variable the case sets and
    // clears around the one acquisition it wants minted.
    void* g_mintedPersistentBase = nullptr;
    void* MintPersistentMap(BufferObject&) { return g_mintedPersistentBase; }
    const MG_State::GLState::BufferBackendOps g_mintingBufferOps = [] {
        MG_State::GLState::BufferBackendOps ops{};
        ops.AcquirePersistentMap = &MintPersistentMap;
        return ops;
    }();

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
            // Belt and braces for the one case that installs a minting backend: a table left
            // behind would make the NEXT case's acquisition land in the adopted arm, and every
            // membership assertion in this file would then be about a different code path.
            MG_State::GLState::SetBufferBackendOps(nullptr);
            g_mintedPersistentBase = nullptr;
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
//
// THE FIXTURE BINDS THE UNIT BY HAND, so it has to move the image-unit high-water mark by hand
// too (P5d round 3, package C): the walk is bounded by that mark now, and a case that writes the
// binding straight into the slot never went through glBindImageTexture, which is what feeds it.
// The mark's own producer has its own case - ImageEmit.TheBindFeedsTheImageUnitHighWaterMark.
TEST_F(SplitBufferSet, Row2AWritableImageBufferTextureIsMarkedByADraw) {
    auto texture = MakeShared<MG_State::GLState::TextureObjectBuffer>(8u);
    auto backing = MakeBuffer(14u, 128);
    texture->GetBufferBindingSlot().Bind(backing);
    auto& binding = MG_State::pGLContext->GetImageTextureBinding(0);
    binding.Texture = texture;
    binding.Access = GL_WRITE_ONLY;
    MG_State::pGLContext->NoteImageUnitTouched(0);

    MG_Remote::Client::MarkGpuWritesForDraw();

    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::WritableImageBufferTexture), 1u);

    binding = MG_State::GLState::ImageTextureBinding{};
}

// Row 2's BOUND, and it is the half an optimisation is allowed to get wrong in exactly one
// direction (P5d round 3, package C). The sweep used to be MAX_TEXTURE_IMAGE_UNITS wide on every
// draw - 2.15% self of the GL thread on the Minecraft inproc profile, in a workload with no image
// binding anywhere - and is now bounded by the frontend's image-unit high-water mark.
//
// THE CASE PINS BOTH ENDS OF THAT BOUND. Below the mark a writable image-buffer texture is still
// found; a mark that has never moved means the walk does not run at all, which is the whole
// saving and is what makes "the mark only ever grows" load-bearing rather than decorative. Revert
// the bound to `unit < MAX_TEXTURE_IMAGE_UNITS` and the second half goes red; drop
// NoteImageUnitTouched from glBindImageTexture and ImageEmit.TheBindFeedsTheImageUnitHighWaterMark
// goes red instead - the two together are what say the narrowing is safe.
TEST_F(SplitBufferSet, Row2TheSweepIsBoundedByTheImageUnitHighWaterMark) {
    ASSERT_EQ(MG_State::pGLContext->GetMaxTouchedImageUnit(), -1)
        << "a fresh context has bound no image unit, so the sweep has nothing to walk";

    auto texture = MakeShared<MG_State::GLState::TextureObjectBuffer>(9u);
    auto backing = MakeBuffer(18u, 128);
    texture->GetBufferBindingSlot().Bind(backing);
    constexpr Int kUnit = 5;
    auto& binding = MG_State::pGLContext->GetImageTextureBinding(kUnit);
    binding.Texture = texture;
    binding.Access = GL_READ_WRITE;

    MG_Remote::Client::MarkGpuWritesForDraw();
    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::WritableImageBufferTexture), 0u)
        << "the mark has never moved, so the sweep must not walk a single unit";

    MG_State::pGLContext->NoteImageUnitTouched(kUnit);
    MG_Remote::Client::MarkGpuWritesForDraw();
    EXPECT_EQ(MG_Remote::Client::ProducerMarkCount(GpuWriteProducer::WritableImageBufferTexture), 1u)
        << "a unit at the mark is inside the walk";

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

// THE ADOPTED ARM IS NOT A MEMBER, AND IT IS THE CHAIN THAT SAYS SO - NOT THE TIER (ID-42).
//
// IsLivePersistentMap's second row is `if (buffer.IsBackendPersistentMapped()) return false;`,
// and its comment promises that the row answers for ITSELF: at tier T2 the arm is unreachable
// because MapPersistent declines, but a build that reaches T0/T1 later must get the same answer
// out of the same row. Nothing drove that promise. The case above only ever sees the declined
// arm, and PersistentCoherentMapScenario's membership assertion cannot pin it either - an
// integration entry sees whichever arm its driver and transport hand it, which is exactly how
// that assertion came to state the emulated arm's property as if it were universal and go red on
// seven split-monolith entries the first time it ran.
//
// So the statement is pinned HERE, where both answers can be produced on demand. The adoption is
// made by the PRODUCTION path - AcquireMemoryRange dispatching to the backend's
// AcquirePersistentMap and calling PipeResource::AdoptPersistentMap - and not by the case
// reaching into the buffer, because a hand-set flag would still be "true" with the production
// adoption deleted (R-16). The transport is Monolith for exactly that call, because R-6's decline
// is ANDed with `Transport != Monolith` and there is no other way to reach a mint in this build;
// the PREDICATE is then asked with the tier back at T2/InProcess, which is the whole point: the
// tier says "emulated, always" and the chain still says "not a member".
TEST_F(SplitBufferSet, TheAdoptedArmIsNotAMemberAndItIsTheChainRowThatSaysSo) {
    constexpr SizeT kSize = 4096;
    // The storage the backend "mints". Declared first so it outlives the buffer: AdoptPersistentMap
    // stores the pointer and releases the shadow, and it never owns what it was handed.
    Vector<Uint8> minted(kSize, static_cast<Uint8>(0));
    g_mintedPersistentBase = minted.data();

    auto adopted = MakeBuffer(27u, kSize);
    auto declined = MakeBuffer(28u, kSize);

    // The declined twin first, with no backend ops at all: same flags, same size, same call.
    declined->AcquireMemoryRange(Range1D{0, kSize},
                                 BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    ASSERT_FALSE(declined->IsBackendPersistentMapped());
    ASSERT_TRUE(PersistentMapTracker::IsLivePersistentMap(*declined))
        << "the emulated arm IS a member - if this fails the two halves are not comparable and the "
           "assertion below proves nothing";

    {
        MG_State::GLState::SetBufferBackendOps(&g_mintingBufferOps);
        MG_Config::Transport = MG_Config::TransportMode::Monolith;
        adopted->AcquireMemoryRange(Range1D{0, kSize},
                                    BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MG_State::GLState::SetBufferBackendOps(nullptr);
    }
    ASSERT_TRUE(adopted->IsBackendPersistentMapped())
        << "the backend declined the mint, so there is no adopted arm here to ask about";

    // THE TIER SAYS EMULATED. The chain must still say "not a member".
    ASSERT_TRUE(MG_Remote::Client::AdoptTierIsEmulate());
    ASSERT_NE(MG_Config::Transport, MG_Config::TransportMode::Monolith);
    EXPECT_FALSE(PersistentMapTracker::IsLivePersistentMap(*adopted))
        << "an adopted store's bytes are already in host-visible coherent GPU memory and there is "
           "nothing for the push to ship, so IsBackendPersistentMapped() takes it out of the set. "
           "This answer must come from that ROW and not from the adopt tier: the tier is T2 and the "
           "transport is InProcess right now, which is the configuration in which R-6 says every "
           "acquisition declines - and the store in front of the predicate is adopted anyway, "
           "because a later phase's T0/T1 will mint one. A predicate that read the tier would call "
           "it a member and the push would read an adopted store's Bytes() as if it were the "
           "shadow.";

    // The SET agrees with the predicate, asked through the production entry point rather than by
    // reading a member: NoteMapStateChanged is what every one of the five maintenance events
    // calls, and it must refuse to enrol an adopted store.
    PersistentMapTracker::Instance().NoteMapStateChanged(*adopted);
    EXPECT_EQ(PersistentMapTracker::Instance().MemberCount(), 1u)
        << "only the declined twin: enrolling an adopted store would make the push read its "
           "Bytes() - which is the GPU map, not the shadow - as if it were bytes to ship";

    // ...and the consequence, which is the one that would actually corrupt something.
    const Uint64 before = MG_Util::PipeStats::TotalBytes(MG_Util::PipeStats::ByteClass::PersistentMapPush);
    MG_Remote::Client::PushPersistentMapsBeforeVerb();
    EXPECT_EQ(MG_Util::PipeStats::TotalBytes(MG_Util::PipeStats::ByteClass::PersistentMapPush) - before,
              static_cast<Uint64>(kSize))
        << "the declined twin's 4096 bytes and nothing else: the adopted buffer must contribute no "
           "pushed bytes at all";

    declined->ReleaseMemory(false);
    // The adopted one is NOT released through ReleaseMemory: ReleasePersistentMap is for a store
    // being redefined, and a persistent map the application holds outlives every unmap by
    // definition (PipeResource.h:121-127). It is dropped here with the mapping still adopted,
    // which is also the shape ~BufferObject has to survive.
    adopted.reset();
    g_mintedPersistentBase = nullptr;
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

// THE STATE RECORD ON BOTH EDGES (B-1). The rising edge is what breaks the cycle the probe
// would otherwise latch: a coherent persistent map behind a static VAO emits NO content record
// of its own until the push runs, the push runs inside the ensure path, and a draw-clean answer
// skips that ensure and then latches it. So the map itself has to publish, and the unmap has to
// publish the retraction - one block each, observable here as exactly one serial bump each.
TEST_F(SplitBufferSet, BothEdgesOfAWriteMapPublishOneStateRecord) {
    constexpr SizeT kSize = 4096;
    auto buffer = MakeBuffer(26u, kSize);
    EXPECT_FALSE(buffer->HasLiveHostWritesForWire());

    const Uint64 beforeMap = buffer->GetChangeSerial();
    buffer->AcquireMemoryRange(Range1D{0, kSize},
                               BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    EXPECT_TRUE(buffer->HasLiveHostWritesForWire());
    EXPECT_EQ(buffer->GetChangeSerial(), beforeMap + 1)
        << "the rising edge published nothing, so the server cannot know a host writer is live "
           "until a content record it may never emit";

    const Uint64 beforeUnmap = buffer->GetChangeSerial();
    buffer->ReleaseMemory(/*landStagedWrites=*/true);
    EXPECT_FALSE(buffer->HasLiveHostWritesForWire());
    EXPECT_EQ(buffer->GetChangeSerial(), beforeUnmap + 2)
        << "the unmap's own NotifyFlushMappedRange plus the falling-edge state record: without "
           "the second the record stays dirty for the buffer's life";
}

// E3(a)'s NEGATIVE CONTROL: 0 disables the push, it does not mean "one unlimited block" - and
// it disables the STATE RECORDS with it. An earlier cut skipped the blocks and still shipped a
// whole buffer at unmap, so a scenario that unmapped before reading back went green and the
// control was dead.
TEST_F(SplitBufferSet, AZeroBlockSizeTurnsThePushOffRatherThanMakingItUnlimited) {
    MG_Config::Ipc.PersistentBlockKb = 0;
    constexpr SizeT kSize = 128u * 1024u;
    auto buffer = MakeBuffer(23u, kSize);

    const Uint64 beforeMap = buffer->GetChangeSerial();
    buffer->AcquireMemoryRange(Range1D{0, kSize},
                               BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    ASSERT_TRUE(PersistentMapTracker::IsLivePersistentMap(*buffer));
    EXPECT_EQ(buffer->GetChangeSerial(), beforeMap)
        << "the rising-edge state record went out with the push disabled";

    const Uint64 before = MG_Util::PipeStats::TotalBytes(MG_Util::PipeStats::ByteClass::PersistentMapPush);
    MG_Remote::Client::PushPersistentMapsBeforeVerb();
    EXPECT_EQ(PersistentMapTracker::Instance().BlocksPushed(), 0u);
    EXPECT_EQ(MG_Util::PipeStats::TotalBytes(MG_Util::PipeStats::ByteClass::PersistentMapPush), before)
        << "MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0 must ship nothing, so that "
           "PersistentCoherentMapScenario goes red under it";

    const Uint64 beforeUnmap = buffer->GetChangeSerial();
    buffer->ReleaseMemory(/*landStagedWrites=*/true);
    EXPECT_EQ(buffer->GetChangeSerial(), beforeUnmap + 1)
        << "with the push off the unmap must emit only its own NotifyFlushMappedRange - a "
           "falling-edge record here delivers a whole buffer the control exists to withhold, "
           "and a negative control that still delivers the bytes is not a control";
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

// =====================================================================================
// THE MPROTECT TRACKER'S OUTWARD ALIGNMENT (P5d round 3). A split build's shadow is
// page-aligned AND page-granular (PipeResource.h), so every page a mapped range touches is
// the shadow's own and the tracker protects the range's containing pages whole: no
// unprotected edge bytes, no per-draw XXH3 (8.5% of the client thread at VD12 before this).
// White-box on purpose - which pages a map protected is not observable through any push
// count, and the epoch (the one thing a protected page moves) is the tracker's own.
// =====================================================================================

// A sub-page map used to register with an EMPTY interior and two hashed edges; now it is
// one protected page, and a write through the pointer FAULTS - which is the whole point.
TEST_F(SplitBufferSet, ASubPageMapIsOneProtectedPageAndAWriteThroughItFaults) {
    if (!PersistentMapTracker::MprotectArmAvailableForTest()) {
        GTEST_SKIP() << "no mprotect arm on this host (handler not installable, or the kernel "
                        "page is not the tracker's 4 KB); the hash arm serves every map here";
    }
    constexpr SizeT kSize = 100; // well under a page, and not a multiple of anything
    auto buffer = MakeBuffer(26u, kSize);
    ASSERT_EQ(buffer->ShadowAllocationBytes() % 4096u, 0u)
        << "the split build's shadow allocator rounds the SIZE to whole pages, not just the base";
    ASSERT_GE(buffer->ShadowAllocationBytes(), kSize);
    auto* mapped = static_cast<Uint8*>(buffer->AcquireMemoryRange(
        Range1D{0, kSize}, BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent));
    ASSERT_NE(mapped, nullptr);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(mapped) % 4096u, 0u) << "the shadow base is page-aligned";

    const auto* slot = PersistentMapTracker::TrackedSlotForTest(*buffer);
    ASSERT_NE(slot, nullptr) << "a page-aligned, page-granular shadow registers on the mprotect arm";
    EXPECT_EQ(slot->pageCount, 1u)
        << "a sub-page map is ONE protected page under outward alignment, not an empty interior";
    EXPECT_FALSE(slot->hasEdges) << "outward alignment leaves no unprotected edge bytes to hash";
    const uintptr_t base = slot->base.load();
    const uintptr_t end = slot->end.load();
    EXPECT_EQ(base, reinterpret_cast<uintptr_t>(mapped));
    EXPECT_EQ(end, base + 4096u);
    EXPECT_LE(end, reinterpret_cast<uintptr_t>(mapped) + buffer->ShadowAllocationBytes())
        << "the protected span never runs past the shadow's own allocation";

    // Fresh state ships everything once, then re-arms; with nothing written a second push
    // ships nothing (and, with no edges, hashes nothing).
    MG_Remote::Client::PushPersistentMapsBeforeVerb();
    EXPECT_EQ(PersistentMapTracker::Instance().BlocksPushed(), 1u);
    EXPECT_EQ(PersistentMapTracker::Instance().BytesPushed(), static_cast<Uint64>(kSize));
    MG_Remote::Client::PushPersistentMapsBeforeVerb();
    EXPECT_EQ(PersistentMapTracker::Instance().BlocksPushed(), 1u);

    // The one page IS protected: a write through the application's pointer faults into the
    // handler (the epoch moves), and the next push ships the page's block clamped to the
    // mapped range. Under the old empty-interior registration nothing faulted here.
    const Uint64 epochBefore = PersistentMapTracker::FaultEpochForTest();
    mapped[kSize - 1] = 0x5A;
    EXPECT_GT(PersistentMapTracker::FaultEpochForTest(), epochBefore)
        << "the sub-page map's page was not protected: the write did not fault";
    MG_Remote::Client::PushPersistentMapsBeforeVerb();
    EXPECT_EQ(PersistentMapTracker::Instance().BlocksPushed(), 2u);
    EXPECT_EQ(PersistentMapTracker::Instance().BytesPushed(), static_cast<Uint64>(2 * kSize))
        << "the push is clamped to [begin, end) even though the protected page is wider";

    // The unmap retires the slot and restores the page writable: an API write into the
    // shadow afterwards must not fault (an unowned fault chains to SIG_DFL and dies).
    buffer->ReleaseMemory(false);
    EXPECT_EQ(PersistentMapTracker::TrackedSlotForTest(*buffer), nullptr);
    const Uint8 byte = 0x3C;
    buffer->UploadSubData(DataPtr{const_cast<Uint8*>(&byte), 1}, 0);
}

// The inward fallback is kept for any shadow that fails the page-granular test, and the
// only way to reach it on this build - whose allocator never hands out such a shadow - is
// to register a range by hand with a base or an extent the tracker cannot vouch for.
TEST_F(SplitBufferSet, AShadowThatFailsThePageGranularTestKeepsTheInwardAlignmentAndItsEdges) {
    if (!PersistentMapTracker::MprotectArmAvailableForTest()) {
        GTEST_SKIP() << "no mprotect arm on this host (handler not installable, or the kernel "
                        "page is not the tracker's 4 KB); the hash arm serves every map here";
    }
    // Three whole pages from the shadow allocator itself: page-aligned, page-granular.
    MG_State::GLState::MapAlignedData store(3u * 4096u);
    const auto storeBase = reinterpret_cast<uintptr_t>(store.data());
    ASSERT_EQ(storeBase % 4096u, 0u);
    const SizeT extent = MG_State::GLState::ShadowAllocationBytesFor(store.capacity());
    ASSERT_EQ(extent, 3u * 4096u);
    // Never a real buffer's id: BufferObject lifetime ids count up from 1.
    constexpr Uint64 kId = ~0ull - 7u;

    // An UNALIGNED base (the shadow's own storage, offset by 64) with a range of two pages:
    // only the one page fully inside [base+64, base+64+8192) is protected, both ends are
    // edges - the pre-round-3 shape, and still the safe answer for a base like this.
    const auto* slot = PersistentMapTracker::TrackForTest(kId, store.data() + 64, 0, 2u * 4096u, extent);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->pageCount, 1u);
    EXPECT_TRUE(slot->hasEdges);
    EXPECT_EQ(slot->base.load(), storeBase + 4096u);
    EXPECT_EQ(slot->end.load(), storeBase + 2u * 4096u);
    PersistentMapTracker::UntrackForTest(kId);

    // An aligned base with an extent the caller could NOT vouch for (0): inward as well.
    slot = PersistentMapTracker::TrackForTest(kId, store.data(), 64, 2u * 4096u + 64, 0);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->pageCount, 1u);
    EXPECT_TRUE(slot->hasEdges);
    PersistentMapTracker::UntrackForTest(kId);

    // A range that runs PAST the extent it was told about is never widened into it.
    slot = PersistentMapTracker::TrackForTest(kId, store.data(), 64, 2u * 4096u + 64, 2u * 4096u);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->pageCount, 1u);
    EXPECT_TRUE(slot->hasEdges);
    PersistentMapTracker::UntrackForTest(kId);

    // And the same range on the same aligned base with the TRUE extent widens outward to
    // its three containing pages - exactly the extent, so the clamp sits on its boundary.
    slot = PersistentMapTracker::TrackForTest(kId, store.data(), 64, 2u * 4096u + 64, extent);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->pageCount, 3u);
    EXPECT_FALSE(slot->hasEdges);
    EXPECT_EQ(slot->base.load(), storeBase);
    EXPECT_EQ(slot->end.load(), storeBase + extent);
    PersistentMapTracker::UntrackForTest(kId);
    // Restored writable before the store goes out of scope: freeing a still-protected page
    // would fault inside the allocator with no owner to answer.
    store[0] = 1;
    store[extent - 1] = 1;
}

// THE DRAIN THE EPOCH SKIP RESTS ON. A fault marks a page and moves the epoch; the next
// draw's walk consumes the epoch. If that walk pushed only the buffers the draw bound, a
// faulted map the draw did NOT bind would be left marked, writable and unable to move the
// epoch ever again - and the next draw that does bind it would skip, and the server would
// draw its pre-write bytes. Before round 3 the skip's edge service drained every member
// with edges by accident (nearly all of them); with page-granular shadows nothing has
// edges, so the walk drains every marked member on purpose. Two maps, one bound: the write
// to the unbound one must ship before the draw that binds it. Driven through
// PushDrawConsumers itself - PushPersistentMapsBeforeVerb never reaches the skip.
TEST_F(SplitBufferSet, AFaultedMapTheDrawDidNotBindStillShipsBeforeTheDrawThatDoes) {
    if (!PersistentMapTracker::MprotectArmAvailableForTest()) {
        GTEST_SKIP() << "no mprotect arm on this host (handler not installable, or the kernel "
                        "page is not the tracker's 4 KB); the hash arm serves every map here";
    }
    constexpr SizeT kSize = 4096; // one page, one block each
    auto a = MakeBuffer(29u, kSize);
    auto b = MakeBuffer(30u, kSize);
    auto* aMapped = static_cast<Uint8*>(a->AcquireMemoryRange(
        Range1D{0, kSize}, BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent));
    b->AcquireMemoryRange(Range1D{0, kSize},
                          BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    ASSERT_NE(aMapped, nullptr);
    ASSERT_NE(PersistentMapTracker::TrackedSlotForTest(*a), nullptr)
        << "both maps must be on the mprotect arm, or the epoch is not what decides";
    ASSERT_NE(PersistentMapTracker::TrackedSlotForTest(*b), nullptr);
    auto& tracker = PersistentMapTracker::Instance();
    auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, 0);

    // Draw 1 binds B only. Both maps are fresh - every page marked, and the registration
    // moved the epoch - so this walk ships both once: the unbound A as well.
    point.Bind(b);
    tracker.PushDrawConsumers();
    EXPECT_EQ(tracker.BlocksPushed(), 2u) << "first push ships everything once, the unbound map too";
    // Draw 2, nothing written: the epoch is unmoved and the skip ships nothing.
    tracker.PushDrawConsumers();
    EXPECT_EQ(tracker.BlocksPushed(), 2u);

    // The application writes A through its pointer (a fault: A's page is marked, the epoch
    // moves) and the next draw binds B only.
    const Uint64 epochBefore = PersistentMapTracker::FaultEpochForTest();
    aMapped[kSize / 2] = 0x5A;
    ASSERT_GT(PersistentMapTracker::FaultEpochForTest(), epochBefore)
        << "the write did not fault: A's page was not re-armed by its first push";
    tracker.PushDrawConsumers();
    EXPECT_EQ(tracker.BlocksPushed(), 3u)
        << "the walk that consumed the epoch must drain A's marked page although this draw "
           "bound only B - nothing will ever move the epoch for that page again";

    // Draw 4 binds A. The epoch is unmoved and the skip fires - correctly, because A's
    // write already crossed. With the drain reverted, this is the draw that reads stale.
    point.Bind(a);
    tracker.PushDrawConsumers();
    EXPECT_EQ(tracker.BlocksPushed(), 3u) << "nothing left to ship";
    EXPECT_EQ(tracker.BytesPushed(), static_cast<Uint64>(3u * kSize));

    point.Bind(nullptr);
    a->ReleaseMemory(false);
    b->ReleaseMemory(false);
}

#else

// THE SAME NAMES, ONE FOR ONE, SO THE ctest NAME SET DOES NOT MOVE BETWEEN LANES. (The count
// is deliberately NOT written out here: it read "SIXTEEN" against seventeen stubs before this
// commit and "SEVENTEEN" against eighteen after, so it has never once matched the list it
// claims to describe - and the list below is the authority either way.) G2 compares the
// pull and push name lists line for line and G14 allows build-split to ADD names but never to
// remove one, so a case that exists only where it can run would break both gates for a reason
// that has nothing to do with what it tests. It skips instead, and says why.
TEST(SplitBufferSet, Row0EverySsboBindingPointIsMarkedByADraw) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row1EveryBoundAtomicCounterIsMarkedByADraw) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row2OnlyAWritableImageBufferTextureCounts) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row2AWritableImageBufferTextureIsMarkedByADraw) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row2TheSweepIsBoundedByTheImageUnitHighWaterMark) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row3TransformFeedbackTargetsAreOnlyMarkedWhileACaptureIsActive) {
    MGL_SPLIT_ONLY_OR_SKIP();
}
TEST(SplitBufferSet, Row4AReadPixelsIntoAPackPboMarksThePbo) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, Row5EndTransformFeedbackMarksTheCaptureTargets) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, TheWholeSetIsInertOnTheMonolithPath) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, MembershipIsSyncPersistentMappedRangesOwnEarlyOutChain) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, TheAdoptedArmIsNotAMemberAndItIsTheChainRowThatSaysSo) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, ThePushCutsTheMappedSpanIntoBlocksAndMovesPmap) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, TheLastBlockIsTheRemainderAndNotAWholeBlock) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, BothEdgesOfAWriteMapPublishOneStateRecord) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, AZeroBlockSizeTurnsThePushOffRatherThanMakingItUnlimited) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, ADestroyedBufferLeavesTheSet) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, UnderSplitTheWritebackClearsThePendingFlagAndNotTheRequest) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, OnlyAdoptTierTwoIsImplemented) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, ASubPageMapIsOneProtectedPageAndAWriteThroughItFaults) { MGL_SPLIT_ONLY_OR_SKIP(); }
TEST(SplitBufferSet, AShadowThatFailsThePageGranularTestKeepsTheInwardAlignmentAndItsEdges) {
    MGL_SPLIT_ONLY_OR_SKIP();
}
TEST(SplitBufferSet, AFaultedMapTheDrawDidNotBindStillShipsBeforeTheDrawThatDoes) { MGL_SPLIT_ONLY_OR_SKIP(); }

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
