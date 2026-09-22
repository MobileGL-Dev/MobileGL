// MobileGL - MobileGL/MG_Test/Wire/StagedTextureStoreTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Package tx's suite (P5c): the server's staged-texture shadow.
//
// THE SHAPE IS StagedShadowTest's (ServerLoopTest.cpp:606-760), for the R-16 reason stated
// there: every case must be able to go red for the reason it exists, and no other. The store
// unit cases build one store of each kind and assert the DIFFERENCE (copies is a constructor
// parameter, not a read of MG_Config::Transport); the production case drives the REAL
// resource op table - the same g_glesResourceOps RegisterBufferBackendOps installs -
// because a suite that only exercised StagedTextureStore in isolation would stay green with
// the ops-table registration deleted.
//
// E-P5c GATE #2 LIVES IN THE PRODUCTION CASE: reverting the adoption to P5's pointer-dropping
// (delete the copy inside Ops_H_TextureSubData, or the ops-table registration, or the
// ApplyTextureUpload hook call) turns it red - that is what makes "the server consumes the
// staged bytes" a checked fact rather than a design intention.

#include <Config.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Remote/Server/StagedTextureStore.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>

#include <csignal>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace MobileGL;

namespace Server = MobileGL::MG_Remote::Server;

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

    constexpr Uint16 kTex2DTarget = static_cast<Uint16>(TextureUploadTarget::Texture2D);

    MG_Pipe::MGPipeHandle TestHandle(Uint32 slot, Uint32 gen) {
        MG_Pipe::MGPipeHandle handle{};
        handle.Slot = slot;
        handle.Gen = gen;
        return handle;
    }

} // namespace

// =====================================================================================
// The store, in isolation
// =====================================================================================

// THE PROPERTY MOBILEGL_IPC_AUDIT=1's 0xDD FILL EXISTS TO TEST, at unit scope: after the
// staged source bytes are overwritten - which is what the decoder does to a retired SEG_STAGE
// run - the server's copy still reads the original. The monolith store is the control: the
// SAME calls are no-ops on it, because the monolith sync answers from the client shadow.
TEST(StagedTextureStoreTest, TheSplitArmCopiesAndSurvivesTheSourceBeingPoisoned) {
    Server::StagedTextureStore splitStore(/*copies=*/true);
    Server::StagedTextureStore monolithStore(/*copies=*/false);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(3, 1));

    Vector<Uint8> staged(64, 0xAB);
    const IntVec3 extent{4, 4, 1};
    const Uint8* splitBase = splitStore.Adopt(key, kTex2DTarget, 0, extent, staged.data(), staged.size());
    const Uint8* monolithBase =
        monolithStore.Adopt(key, kTex2DTarget, 0, extent, staged.data(), staged.size());

    ASSERT_NE(splitBase, nullptr);
    EXPECT_EQ(monolithBase, nullptr)
        << "the monolith arm allocates nothing and answers nothing - the client shadow answers";
    EXPECT_EQ(monolithStore.TrackedResources(), 0u);
    EXPECT_FALSE(monolithStore.IsCovered(key, kTex2DTarget, 0));
    EXPECT_NE(splitBase, staged.data()) << "the adoption returned the CLIENT's pointer - the "
                                           "exact rule-C violation this store exists to fix";

    // w1's retired-stage poison, by hand and at the right moment: the record has retired, so
    // the staging run is dead.
    std::fill(staged.begin(), staged.end(), Uint8{0xDD});
    for (SizeT i = 0; i < 64; ++i) {
        EXPECT_EQ(splitBase[i], 0xAB) << "byte " << i << " of the server's copy is the poison, "
                                      << "so the copy never happened";
    }
}

// Defined-ness is tracked separately from bytes: a null-data definition (NoteLevelDefined
// with no Adopt) makes the level EXIST at the derived extent without covering any bytes, and
// an extent move redefines the coordinate system, so the old run goes with it while the
// level stays defined. A same-extent re-note keeps the run - the driver keeps the old texels
// for a same-shape redefinition too.
TEST(StagedTextureStoreTest, DefinednessIsTrackedAndAnExtentMoveDropsTheBytes) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(4, 1));

    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 0), IntVec3(0, 0, 0))
        << "a level nothing defined must answer {0,0,0} - the sparse-chain skip reads exactly "
           "this, and the frontend's GetMipmapTexelSize answers the same";
    EXPECT_FALSE(store.IsLevelDefined(key, kTex2DTarget, 0));

    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{4, 4, 1});
    EXPECT_TRUE(store.IsLevelDefined(key, kTex2DTarget, 0));
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0)) << "defined-without-bytes covers nothing";
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 0), IntVec3(4, 4, 1));
    EXPECT_EQ(store.LevelByteSize(key, kTex2DTarget, 0), 0u);

    Vector<Uint8> bytes(64, 0x11);
    store.Adopt(key, kTex2DTarget, 0, IntVec3{4, 4, 1}, bytes.data(), bytes.size());
    EXPECT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    EXPECT_EQ(store.LevelByteSize(key, kTex2DTarget, 0), 64u);

    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{4, 4, 1});
    EXPECT_TRUE(store.IsCovered(key, kTex2DTarget, 0)) << "a same-extent re-definition keeps the run";

    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{8, 8, 1});
    EXPECT_TRUE(store.IsLevelDefined(key, kTex2DTarget, 0));
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0))
        << "an extent move replaced the coordinate system; the old run must not answer for it";
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 0), IntVec3(8, 8, 1));
}

// Keys are independent, ResetLevels drops one resource's whole chain, Drop one key and
// DropAll every one - the three events the contract names (respecify, destroy, context death)
// each have their call site, and these are the answers those call sites rely on.
TEST(StagedTextureStoreTest, ResetDropAndDropAllForgetExactlyWhatTheyName) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 a = Server::StagedTextureStore::KeyForHandle(TestHandle(5, 1));
    const Uint64 b = Server::StagedTextureStore::KeyForHandle(TestHandle(6, 1));
    Vector<Uint8> bytes(16, 0x22);

    store.Adopt(a, kTex2DTarget, 0, IntVec3{4, 4, 1}, bytes.data(), bytes.size());
    store.Adopt(a, kTex2DTarget, 1, IntVec3{2, 2, 1}, bytes.data(), bytes.size());
    store.Adopt(b, kTex2DTarget, 0, IntVec3{4, 4, 1}, bytes.data(), bytes.size());
    ASSERT_EQ(store.TrackedResources(), 2u);
    ASSERT_EQ(store.TrackedLevelCount(a), 2u);

    store.ResetLevels(a);
    EXPECT_FALSE(store.HasShadow(a)) << "a whole-resource respecify forgets every level";
    EXPECT_TRUE(store.IsCovered(b, kTex2DTarget, 0));

    store.Adopt(a, kTex2DTarget, 0, IntVec3{4, 4, 1}, bytes.data(), bytes.size());
    store.Drop(a);
    EXPECT_EQ(store.TrackedResources(), 1u);
    EXPECT_TRUE(store.IsCovered(b, kTex2DTarget, 0));

    store.DropAll();
    EXPECT_EQ(store.TrackedResources(), 0u);
    EXPECT_FALSE(store.HasShadow(b));
}

// T5's dirty mark: a GPU-side generation dirties the SERVER's shadow, and the mark - not the
// client - answers "dirty region, level has no pending upload". It is settable and clearable
// per (uploadTarget, level), independent of bytes, and inert on a monolith store.
TEST(StagedTextureStoreTest, TheGpuDirtyMarkIsTheServersOwnDirtyAnswer) {
    Server::StagedTextureStore store(/*copies=*/true);
    Server::StagedTextureStore monolithStore(/*copies=*/false);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(7, 1));

    EXPECT_FALSE(store.IsLevelGpuDirty(key, kTex2DTarget, 2));
    store.NoteLevelDefined(key, kTex2DTarget, 2, IntVec3{2, 2, 1});
    store.MarkLevelGpuDirty(key, kTex2DTarget, 2, true);
    EXPECT_TRUE(store.IsLevelGpuDirty(key, kTex2DTarget, 2));
    EXPECT_FALSE(store.IsLevelGpuDirty(key, kTex2DTarget, 3)) << "the mark is per level";
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 2))
        << "a generated level holds no bytes; a texel read of it is the Fatal case";
    store.MarkLevelGpuDirty(key, kTex2DTarget, 2, false);
    EXPECT_FALSE(store.IsLevelGpuDirty(key, kTex2DTarget, 2));

    monolithStore.MarkLevelGpuDirty(key, kTex2DTarget, 2, true);
    EXPECT_FALSE(monolithStore.IsLevelGpuDirty(key, kTex2DTarget, 2));
    EXPECT_EQ(monolithStore.TrackedResources(), 0u);
}

// The two key namespaces share one map, so their disjointness is a property to pin, not to
// assume: handle keys carry the top bit, twin addresses (user-space, aligned) never do.
TEST(StagedTextureStoreTest, HandleKeysAndTwinAddressKeysCannotCollide) {
    const MG_Pipe::MGPipeHandle handle = TestHandle(7, 1);
    const Uint64 handleKey = Server::StagedTextureStore::KeyForHandle(handle);
    int twin = 0;
    const Uint64 twinKey = Server::StagedTextureStore::KeyForTwinAddress(&twin);
    EXPECT_NE(handleKey, twinKey);
    EXPECT_NE(handleKey, Server::StagedTextureStore::KeyForHandle(TestHandle(7, 2)))
        << "a recycled slot's new generation must key a different entry";
}

// §1's derivation: max(1, base >> level) per SHRINKING axis, with an array texture's layer
// count fixed. This is the mip chain's definition, so the server and the client compute the
// same number - and the layer axes are exactly where a naive shift would diverge.
TEST(StagedTextureStoreTest, TheMipExtentDerivationKeepsArrayLayersFixed) {
    EXPECT_EQ(Server::StagedTextureMipExtent(static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D), 8, 4, 1, 2),
              IntVec3(2, 1, 1));
    EXPECT_EQ(Server::StagedTextureMipExtent(static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex3D), 8, 8, 8, 3),
              IntVec3(1, 1, 1));
    EXPECT_EQ(
        Server::StagedTextureMipExtent(static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2DArray), 8, 8, 6, 2),
        IntVec3(2, 2, 6)) << "the layer count is not a dimension of the image";
    EXPECT_EQ(
        Server::StagedTextureMipExtent(static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex1DArray), 16, 4, 1, 3),
        IntVec3(2, 4, 1)) << "a 1D array's HEIGHT is the layer count";
    EXPECT_EQ(Server::StagedTextureMipExtent(static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::TexCube), 7, 7, 1, 3),
              IntVec3(1, 1, 1)) << "shrinking clamps at 1, never 0";
}

// ==================== fix A2: one level, several staged runs ====================
//
// ONE RECORD'S BLOB IS STAGED WHOLE IN SEG_STAGE, so a level shadow larger than the segment's
// chunk budget is cut by the CLIENT into whole-width slabs, one resource_subdata each
// (TextureEmit.h's MGPipeForEachTextureSlab). The server's half of that is here: place each run
// where its record says and answer "covered" only once the level is whole. All the runs of one
// level arrive in ONE apply batch (one verb), so nothing reads between them in production; what a
// case can assert is what the readers would see at each instant, and that is what these do.
TEST(StagedTextureStoreTest, StageChunkRunsAssembleTheLevelImageAndAGapIsNotCovered) {
    Server::StagedTextureStore store(/*copies=*/true);
    Server::StagedTextureStore monolithStore(/*copies=*/false);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(11, 1));
    // A 4x4 level of four bytes per texel - 64 bytes - cut into two 32-byte runs.
    const IntVec3 extent{4, 4, 1};
    Vector<Uint8> lower(32, 0x33);
    Vector<Uint8> upper(32, 0x77);

    // THE SECOND RUN FIRST, deliberately: coverage is a SET and the placement comes from the
    // record, so the order the pieces land in cannot matter.
    EXPECT_EQ(monolithStore.AdoptRun(key, kTex2DTarget, 0, extent, 32, upper.data(), upper.size()),
              nullptr);
    EXPECT_EQ(monolithStore.TrackedResources(), 0u);
    store.AdoptRun(key, kTex2DTarget, 0, extent, 32, upper.data(), upper.size());
    EXPECT_TRUE(store.IsLevelDefined(key, kTex2DTarget, 0));
    EXPECT_EQ(store.LevelByteSize(key, kTex2DTarget, 0), 64u) << "the image grew to the run's end";
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0))
        << "half a level read as covered, so a sync would take zeroes for the missing half";
    EXPECT_EQ(store.LevelCoveredRunCount(key, kTex2DTarget, 0), 1u);

    store.AdoptRun(key, kTex2DTarget, 0, extent, 0, lower.data(), lower.size());
    EXPECT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    EXPECT_EQ(store.LevelCoveredRunCount(key, kTex2DTarget, 0), 1u)
        << "two adjacent runs must merge into one covered range";
    const Uint8* base = store.RequireLevelBytes(key, kTex2DTarget, 0, "unit_stage_chunk_runs");
    ASSERT_NE(base, nullptr);
    for (SizeT i = 0; i < 32; ++i) EXPECT_EQ(base[i], 0x33) << "byte " << i << " of the lower run";
    for (SizeT i = 32; i < 64; ++i) EXPECT_EQ(base[i], 0x77) << "byte " << i << " of the upper run";

    // w1's retired-stage poison, at the store's own scope: the COPY is what answers the readers.
    std::fill(lower.begin(), lower.end(), Uint8{0xDD});
    std::fill(upper.begin(), upper.end(), Uint8{0xDD});
    EXPECT_EQ(base[0], 0x33);
    EXPECT_EQ(base[63], 0x77);

    // AND A GAP IS NOT COVERAGE: it is the missing-piece case, which is the one thing the covered
    // set exists to make visible instead of papered over.
    const Uint64 holed = Server::StagedTextureStore::KeyForHandle(TestHandle(12, 1));
    Vector<Uint8> part(16, 0x11);
    store.AdoptRun(holed, kTex2DTarget, 0, extent, 0, part.data(), part.size());
    store.AdoptRun(holed, kTex2DTarget, 0, extent, 32, part.data(), part.size());
    EXPECT_EQ(store.LevelCoveredRunCount(holed, kTex2DTarget, 0), 2u)
        << "two runs with a 16-byte gap merged into one";
    EXPECT_FALSE(store.IsCovered(holed, kTex2DTarget, 0)) << "a span across the gap reads covered";
    // AND WHAT THE CHECK CANNOT SEE, written down rather than left to be discovered: the store
    // knows no format, so it cannot know how many bytes a level SHOULD have. A leading run that
    // already reaches the image's high-water mark therefore reads as covered - the check is "the
    // range the reader asks for is covered", not "the level is the size the client meant", and no
    // reader runs between the pieces of one level anyway (they are records of ONE verb, and the
    // texture sync runs at the barrier that closes it).
    const Uint64 leading = Server::StagedTextureStore::KeyForHandle(TestHandle(14, 1));
    store.AdoptRun(leading, kTex2DTarget, 0, IntVec3{4, 4, 1}, 0, part.data(), part.size());
    EXPECT_TRUE(store.IsCovered(leading, kTex2DTarget, 0))
        << "a leading run is the whole image as far as this store can tell";
    EXPECT_EQ(store.LevelByteSize(leading, kTex2DTarget, 0), 16u);
    // Filling the gap merges all three into one run: adjacency, not proximity, is the rule.
    store.AdoptRun(holed, kTex2DTarget, 0, extent, 16, part.data(), part.size());
    EXPECT_EQ(store.LevelCoveredRunCount(holed, kTex2DTarget, 0), 1u);
    EXPECT_TRUE(store.IsCovered(holed, kTex2DTarget, 0));

    // An extent move redefines the coordinate system, so the assembled runs go with it - the same
    // rule the whole-level adoption has always been held to.
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{8, 8, 1});
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0));
    EXPECT_EQ(store.LevelCoveredRunCount(key, kTex2DTarget, 0), 0u);
    EXPECT_EQ(store.LevelByteSize(key, kTex2DTarget, 0), 0u);
}

// WHICH OF THE TWO STAGED-RUN SHAPES A RECORD CARRIES, read off the run's own LENGTH because the
// wire layout is frozen and carries no field for it. The whole-level run (the shape this half has
// always staged) is the level's complete shadow and begins at the level's first byte; a slab's run
// is exactly the byte extent of its own box, so the box names where it goes. Getting this wrong
// places a whole level's bytes at a dirty box's offset, or a slab's bytes at the level's base.
TEST(StagedTextureStoreTest, TheRunImageOffsetIsReadOffTheRecordsOwnRunAndBox) {
    // THE TRACE'S SLAB: 512x128 RGBA32F, slices 8..16 of a 33-slice level.
    constexpr Uint64 kSliceStride = 512ull * 128ull * 16ull;
    constexpr Uint64 kLevelBytes = kSliceStride * 33ull;
    MG_Pipe::MGPSubData slab{};
    slab.LevelWidth = 512;
    slab.LevelHeight = 128;
    slab.LevelDepth = 33;
    slab.UnionBox = MG_Pipe::MGPBox{0, 0, 8, 512, 128, 8};
    slab.RegionCount = 1;
    slab.Blob.Size = 8ull * kSliceStride;
    MG_Pipe::MGPSubRegion region{};
    region.X = 0;
    region.Y = 0;
    region.Z = 8;
    region.W = 512;
    region.H = 128;
    region.D = 8;
    region.SrcRowStride = 512u * 16u;
    region.SrcSliceStride = static_cast<Uint32>(kSliceStride);
    EXPECT_EQ(Server::StagedTextureRunImageOffset(slab, &region), 8ull * kSliceStride);

    // THE SAME FIELDS WITH A WHOLE-LEVEL RUN - only the length differs - is the whole-level shape,
    // whose run begins at the level's first byte however the box describes the dirty texels.
    MG_Pipe::MGPSubData whole = slab;
    whole.UnionBox = MG_Pipe::MGPBox{16, 32, 0, 64, 32, 1};
    whole.Blob.Size = kLevelBytes;
    EXPECT_EQ(Server::StagedTextureRunImageOffset(whole, &region), 0u);

    // A whole-level BOX over a whole-level RUN is the same answer by the other route: the box
    // starts at the level's first byte, so "derived from the box" and "offset 0" agree.
    whole.UnionBox = MG_Pipe::MGPBox{0, 0, 0, 512, 128, 33};
    EXPECT_EQ(Server::StagedTextureRunImageOffset(whole, &region), 0u);

    // THE NO-REGION SPELLING is the whole-level one by construction: RegionCount == 0 means the
    // box is the whole story and the strides are 0 = tightly packed.
    EXPECT_EQ(Server::StagedTextureRunImageOffset(slab, nullptr), 0u);
    slab.RegionCount = 0;
    EXPECT_EQ(Server::StagedTextureRunImageOffset(slab, &region), 0u);
    slab.RegionCount = 1;

    // A RUN SHORTER THAN ITS BOX'S EXTENT is neither shape. No emitter produces one, and the
    // whole-level reading is the only one that cannot lose texels the record did carry.
    MG_Pipe::MGPSubData shortRun = slab;
    shortRun.Blob.Size = 8ull * kSliceStride - 16ull;
    EXPECT_EQ(Server::StagedTextureRunImageOffset(shortRun, &region), 0u);
}

// The Fatal, and it asserts ITS OWN failure string rather than "the process died" -
// ServerLoopTest.cpp:702-704's reason: a death test that only checks for a crash goes green
// on any other abort in the same body. ONE death per case: the forked children of two
// EXPECT_EXITs would share this process's log file, and the second child's truncated open
// would erase the first's line.
#if !defined(_WIN32)
TEST(StagedTextureStoreTest, ATexelReadOutsideTheStagedCoverageIsFatalByName) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(8, 1));
    Vector<Uint8> bytes(16, 0x33);
    store.Adopt(key, kTex2DTarget, 0, IntVec3{4, 4, 1}, bytes.data(), bytes.size());

    // In coverage: no Fatal, asserted first so the death below cannot be a function that
    // aborts on everything.
    ASSERT_NE(store.RequireLevelBytes(key, kTex2DTarget, 0, "unit"), nullptr);

    // The death MODE and the diagnostic, both pinned: KilledBySignal(SIGABRT) refuses a
    // SIGSEGV, and the log grep names the exact wording. The log flush is pinned the way
    // ServerLoopTest's is: Log.cpp's WriteToFile fflushes after every write and MGLOG_F logs
    // before abort(), so the line is on disk in the forked child before it dies.
    EXPECT_EXIT(store.RequireLevelBytes(key, kTex2DTarget, 5, "unit_undefined_level"),
                ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{StageSnapshotTooNarrow, \"unit_undefined_level\"}"), std::string::npos)
        << "the abort happened but not for this rule's reason; the log says: " << log;
}

TEST(StagedTextureStoreTest, AGpuGeneratedLevelHasNoBytesAndItsTexelReadIsFatalByName) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(9, 1));
    // Defined-without-bytes (T5's GPU-generated level): the level EXISTS, and a texel read of
    // it is the same named refusal, because no byte answer exists on this side.
    store.NoteLevelDefined(key, kTex2DTarget, 1, IntVec3{2, 2, 1});
    ASSERT_TRUE(store.IsLevelDefined(key, kTex2DTarget, 1));
    ASSERT_FALSE(store.IsCovered(key, kTex2DTarget, 1));

    EXPECT_EXIT(store.RequireLevelBytes(key, kTex2DTarget, 1, "unit_gpu_level"),
                ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{StageSnapshotTooNarrow, \"unit_gpu_level\"}"), std::string::npos)
        << "the abort happened but not for this rule's reason; the log says: " << log;
}
#endif

// The same refusal for a level that arrived IN PIECES (fix A2): the sync reads the level image
// whole, so a level with one piece missing is as unreadable as one that never arrived - and it is
// the same named Fatal. ONE DEATH PER CASE, ServerLoopTest's ruling above.
#if !defined(_WIN32)
TEST(StagedTextureStoreTest, ALevelWithAPieceMissingIsRefusedAsAWholeLevelReadByName) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(13, 1));
    Vector<Uint8> part(16, 0x55);
    Vector<Uint8> whole(64, 0x22);
    // TWO RUNS WITH A HOLE BETWEEN THEM, which is what a missing piece looks like from here: the
    // image's high-water mark is the level's size and the covered set does not reach across it.
    store.AdoptRun(key, kTex2DTarget, 0, IntVec3{4, 4, 1}, 0, part.data(), part.size());
    store.AdoptRun(key, kTex2DTarget, 0, IntVec3{4, 4, 1}, 32, part.data(), part.size());
    ASSERT_FALSE(store.IsCovered(key, kTex2DTarget, 0));
    ASSERT_EQ(store.LevelCoveredRunCount(key, kTex2DTarget, 0), 2u);
    ASSERT_EQ(store.LevelByteSize(key, kTex2DTarget, 0), 48u);

    // A DIFFERENT level of the same store, adopted whole, reads fine - asserted first so that the
    // death below cannot be a function that aborts on everything.
    store.AdoptRun(key, kTex2DTarget, 1, IntVec3{4, 4, 1}, 0, whole.data(), whole.size());
    ASSERT_NE(store.RequireLevelBytes(key, kTex2DTarget, 1, "unit"), nullptr);

    EXPECT_EXIT(store.RequireLevelBytes(key, kTex2DTarget, 0, "unit_missing_piece"),
                ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{StageSnapshotTooNarrow, \"unit_missing_piece\"}"), std::string::npos)
        << "the abort happened but not for this rule's reason; the log says: " << log;
}
#endif

// =====================================================================================
// The production wiring - E-P5c gate #2 at unit scope
// =====================================================================================

// THE GATE. The REAL resource op table (RegisterBufferBackendOps installs g_glesResourceOps,
// the exact table the apply path dispatches through), a REAL applier record, and the REAL
// TextureSubData hook ApplyTextureUpload calls - then w1's poison. Reverting ANY link of the
// adoption - the ops-table registration, the hook call in PipeApply.cpp, or the copy inside
// Ops_H_TextureSubData (i.e. going back to P5's pointer-dropping) - turns this red, which is
// the R-16 red-once for "the server consumes the staged bytes" (E-P5c #2).
TEST(StagedTextureProductionTest, TextureSubDataThroughTheRealOpsTableCopiesAndSurvivesTheSourcePoison) {
    // MG_Config::Transport is InProcess (main), so ServerStagedTexture() latches its copying
    // arm on - the same latch ServerLoopTest's R-11 production case relies on.
    MG_Backend::DirectGLES::BufferImpl::RegisterBufferBackendOps();
    const MG_Pipe::MGPipeResourceOps* ops = MG_Pipe::MGPipeGetResourceOps();
    ASSERT_NE(ops, nullptr) << "RegisterBufferBackendOps did not install the resource op table";
    ASSERT_NE(ops->TextureSubData, nullptr)
        << "the texture half of resource_subdata has no adoption hook - the staged bytes are "
           "dropped at apply time and the sync re-reads the client";

    // A real applier record: the hook derives the level's extent from the record's
    // descriptor, so the record must exist the way resource_create makes it.
    const MG_Pipe::MGPipeHandle res = TestHandle(41, 1);
    MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = res;
    desc.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.Levels = 1;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));

    Vector<Uint8> src(64, 0xAB); // 4x4 texels, 4 bytes each
    MG_Pipe::MGPSubData rec{};
    rec.Res = res;
    rec.Target = MG_Pipe::MGPipePackSubDataTarget(
        static_cast<Uint32>(MG_Pipe::MGPipeResourceTarget::Tex2D),
        static_cast<Uint32>(TextureUploadTarget::Texture2D));
    rec.Level = 0;
    rec.UnionBox = {0, 0, 0, 4, 4, 1};
    rec.RegionCount = 0;
    // Under split the codec declares the run's length: "the bytes this record declares ARE
    // the level shadow" (TextureEmit.h:1285). The adoption reads exactly this field.
    rec.Blob.Size = src.size();

    // THE PRODUCTION CALL. Not StagedTextureStore::Adopt directly - the whole point of the
    // gate is that the OPS TABLE carries the bytes into the store.
    ops->TextureSubData(res, rec, src.data(), nullptr);

    auto& store = Server::ServerStagedTexture();
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(res);
    ASSERT_TRUE(store.IsCovered(key, kTex2DTarget, 0))
        << "the hook ran but nothing was adopted - the sync will Fatal or re-read the client";
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 0), IntVec3(4, 4, 1));
    EXPECT_EQ(store.LevelByteSize(key, kTex2DTarget, 0), src.size());
    const Uint8* base = store.RequireLevelBytes(key, kTex2DTarget, 0, "unit_production");
    ASSERT_NE(base, nullptr);
    EXPECT_NE(base, static_cast<const Uint8*>(src.data()))
        << "the sync's texel base points into the CLIENT's staging run - the pointer-dropping "
           "shape P5 had; restoring it turns this red, and that is the gate";

    // w1's retired-stage poison, by hand and at the right moment: the record has retired, so
    // the staging run is dead. A server that copied still reads the original bytes.
    std::fill(src.begin(), src.end(), Uint8{0xDD});
    for (SizeT i = 0; i < 64; ++i) {
        ASSERT_EQ(base[i], 0xAB) << "byte " << i << " of the sync's texel source is the poison, "
                                 << "so the adoption never happened";
    }

    // And the death drops the key, through the same ops table the applier dispatches.
    MG_Pipe::MGPHandleOnly death{};
    death.Handle = res;
    death.Kind = static_cast<Uint32>(MG_Pipe::MGPipeKind::Texture);
    MG_Pipe::MGPipeApplyResourceDestroy(death);
    EXPECT_FALSE(store.HasShadow(key))
        << "a destroyed texture's staged levels must not answer for the slot's next owner";
}

TEST(StagedTextureProductionTest, HooklessTextureConsumerOwnsBytesAndScopedStorageLifetime) {
    // A record-only texture consumer has no texture callbacks in the buffer ops table.
    // The empty fixture table admits records without installing any adoption callback.
    const MG_Pipe::MGPipeResourceOps emptyOps{};
    struct RestoreOps {
        const MG_Pipe::MGPipeResourceOps* Previous = MG_Pipe::MGPipeGetResourceOps();
        ~RestoreOps() { MG_Pipe::MGPipeSetResourceOps(Previous); }
    } restore;
    MG_Pipe::MGPipeSetResourceOps(&emptyOps);
    auto& store = Server::ServerStagedTexture();
    const auto res = TestHandle(47, 2);
    const auto key = Server::StagedTextureStore::KeyForHandle(res);
    MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = res;
    desc.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.Levels = 3;
    MG_Pipe::MGPRespecifiedLevel level{};
    level.UploadTarget = MG_Pipe::MGPipePackSubDataTarget(desc.Target, kTex2DTarget);
    level.Level = 0;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceRespecify(desc, nullptr, &level));
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 0), IntVec3(4, 4, 1))
        << "null-data glTexImage defines the level even though no upload follows";
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0));

    Vector<Uint8> src(64, 0xAB);
    MG_Pipe::MGPSubData upload{};
    upload.Res = res;
    upload.Target = level.UploadTarget;
    upload.UnionBox = {0, 0, 0, 4, 4, 1};
    upload.Blob.Size = src.size();
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceSubData(upload, src.data(), nullptr));
    ASSERT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    std::fill(src.begin(), src.end(), Uint8{0xDD});
    EXPECT_EQ(store.RequireLevelBytes(key, kTex2DTarget, 0, "hookless_source_poison")[0], 0xAB);

    // Defining another mip must retain bytes already accepted for the first mip.
    level.Level = 1;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceRespecify(desc, nullptr, &level));
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 1), IntVec3(2, 2, 1));
    EXPECT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    desc.Width = 8;
    desc.Height = 8;
    desc.Immutable = 1;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceRespecify(desc, nullptr, nullptr));
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0));
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 0), IntVec3(8, 8, 1));
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 2), IntVec3(2, 2, 1));
    MG_Pipe::MGPHandleOnly death{};
    death.Handle = res;
    death.Kind = static_cast<Uint32>(MG_Pipe::MGPipeKind::Texture);
    MG_Pipe::MGPipeApplyResourceDestroy(death);
    EXPECT_FALSE(store.HasShadow(key));

    // Context object release has the same ownership even without a backend hook.
    desc.Resource = TestHandle(47, 3);
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));
    desc.Width = 16;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceRespecify(desc, nullptr, nullptr));
    const auto nextKey = Server::StagedTextureStore::KeyForHandle(desc.Resource);
    ASSERT_TRUE(store.HasShadow(nextKey));
    MG_Pipe::MGPipeApplierReleaseObjectRecords();
    EXPECT_FALSE(store.HasShadow(nextKey));
}

// FIX A2's PRODUCTION WIRING, hookless arm: the two runs of ONE level cross the applier as two
// records and reach the store through PipeApply.cpp's AdoptTextureWithoutBackendHook, which has no
// backend hook to lean on. Each must land where its OWN box and carried strides say - the record is
// the only place the placement exists. The second run is sent first so that a store that ignored
// the box and grew the image from the front cannot pass.
TEST(StagedTextureProductionTest, StageChunkRunsThroughTheHooklessApplierLandWhereTheirBoxesSay) {
    // A record-only texture consumer has no texture callbacks in the buffer ops table, which is
    // exactly the arm that takes the fallback rather than Ops_H_TextureSubData.
    const MG_Pipe::MGPipeResourceOps emptyOps{};
    struct RestoreOps {
        const MG_Pipe::MGPipeResourceOps* Previous = MG_Pipe::MGPipeGetResourceOps();
        ~RestoreOps() { MG_Pipe::MGPipeSetResourceOps(Previous); }
    } restore;
    MG_Pipe::MGPipeSetResourceOps(&emptyOps);

    auto& store = Server::ServerStagedTexture();
    const auto res = TestHandle(45, 1);
    const auto key = Server::StagedTextureStore::KeyForHandle(res);
    MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = res;
    desc.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.Levels = 1;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));

    // A 4x4 RGBA8 level: 64 bytes, four bytes per texel, a 16-byte row pitch. The two runs are its
    // upper and lower halves, 32 bytes each.
    Vector<Uint8> upper(32, 0x77);
    Vector<Uint8> lower(32, 0x33);
    MG_Pipe::MGPSubData upperRecord{};
    upperRecord.Res = res;
    upperRecord.Target = MG_Pipe::MGPipePackSubDataTarget(
        static_cast<Uint32>(MG_Pipe::MGPipeResourceTarget::Tex2D),
        static_cast<Uint32>(TextureUploadTarget::Texture2D));
    upperRecord.LevelWidth = 4;
    upperRecord.LevelHeight = 4;
    upperRecord.LevelDepth = 1;
    upperRecord.UnionBox = MG_Pipe::MGPBox{0, 2, 0, 4, 2, 1};
    upperRecord.RegionCount = 1;
    upperRecord.Blob.Size = upper.size();
    MG_Pipe::MGPSubRegion upperRegion{};
    upperRegion.Y = 2;
    upperRegion.W = 4;
    upperRegion.H = 2;
    upperRegion.D = 1;
    upperRegion.SrcOffset = 0;
    upperRegion.SrcRowStride = 16;
    upperRegion.SrcSliceStride = 64;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceSubData(upperRecord, upper.data(), &upperRegion));
    EXPECT_EQ(store.LevelByteSize(key, kTex2DTarget, 0), 64u)
        << "the image did not grow to the second half's end";
    EXPECT_EQ(store.LevelCoveredRunCount(key, kTex2DTarget, 0), 1u);
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0)) << "only half the level has arrived";

    MG_Pipe::MGPSubData lowerRecord = upperRecord;
    lowerRecord.UnionBox = MG_Pipe::MGPBox{0, 0, 0, 4, 2, 1};
    MG_Pipe::MGPSubRegion lowerRegion = upperRegion;
    lowerRegion.Y = 0;
    lowerRegion.SrcOffset = 0;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceSubData(lowerRecord, lower.data(), &lowerRegion));
    EXPECT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    EXPECT_EQ(store.LevelCoveredRunCount(key, kTex2DTarget, 0), 1u)
        << "the two halves did not merge into one covered range";
    const Uint8* base = store.RequireLevelBytes(key, kTex2DTarget, 0, "unit_production_runs");
    ASSERT_NE(base, nullptr);
    for (SizeT i = 0; i < 32; ++i) EXPECT_EQ(base[i], 0x33) << "byte " << i << " of the lower run";
    for (SizeT i = 32; i < 64; ++i) EXPECT_EQ(base[i], 0x77) << "byte " << i << " of the upper run";

    MG_Pipe::MGPHandleOnly death{};
    death.Handle = res;
    death.Kind = static_cast<Uint32>(MG_Pipe::MGPipeKind::Texture);
    MG_Pipe::MGPipeApplyResourceDestroy(death);
    EXPECT_FALSE(store.HasShadow(key));
}

// THE SAME, through the REAL texture hook DirectGLES registers (Ops_H_TextureSubData), which is the
// arm the shipped backend takes. Reverting the offset - passing 0, or adopting every run as if it
// were the whole level - puts both halves at the front of the image and turns the byte checks below
// red.
TEST(StagedTextureProductionTest, StageChunkRunsThroughTheRealTextureHookLandWhereTheirBoxesSay) {
    MG_Backend::DirectGLES::BufferImpl::RegisterBufferBackendOps();
    const MG_Pipe::MGPipeResourceOps* ops = MG_Pipe::MGPipeGetResourceOps();
    ASSERT_NE(ops, nullptr);
    ASSERT_NE(ops->TextureSubData, nullptr);

    auto& store = Server::ServerStagedTexture();
    const auto res = TestHandle(46, 1);
    const auto key = Server::StagedTextureStore::KeyForHandle(res);
    MG_Pipe::MGPResourceDesc desc{};
    desc.Resource = res;
    desc.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.Levels = 1;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));

    Vector<Uint8> upper(32, 0x77);
    Vector<Uint8> lower(32, 0x33);
    MG_Pipe::MGPSubData upperRecord{};
    upperRecord.Res = res;
    upperRecord.Target = MG_Pipe::MGPipePackSubDataTarget(
        static_cast<Uint32>(MG_Pipe::MGPipeResourceTarget::Tex2D),
        static_cast<Uint32>(TextureUploadTarget::Texture2D));
    upperRecord.LevelWidth = 4;
    upperRecord.LevelHeight = 4;
    upperRecord.LevelDepth = 1;
    upperRecord.UnionBox = MG_Pipe::MGPBox{0, 2, 0, 4, 2, 1};
    upperRecord.RegionCount = 1;
    upperRecord.Blob.Size = upper.size();
    MG_Pipe::MGPSubRegion upperRegion{};
    upperRegion.Y = 2;
    upperRegion.W = 4;
    upperRegion.H = 2;
    upperRegion.D = 1;
    upperRegion.SrcRowStride = 16;
    upperRegion.SrcSliceStride = 64;
    // THE HOOK, not MGPipeApplyResourceSubData: the applier's own dispatch is what routes a
    // texture record into Ops_H_TextureSubData, and that is the call site under test.
    ops->TextureSubData(res, upperRecord, upper.data(), &upperRegion);
    EXPECT_EQ(store.LevelByteSize(key, kTex2DTarget, 0), 64u);
    EXPECT_EQ(store.LevelCoveredRunCount(key, kTex2DTarget, 0), 1u);
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0));

    MG_Pipe::MGPSubData lowerRecord = upperRecord;
    lowerRecord.UnionBox = MG_Pipe::MGPBox{0, 0, 0, 4, 2, 1};
    MG_Pipe::MGPSubRegion lowerRegion = upperRegion;
    lowerRegion.Y = 0;
    ops->TextureSubData(res, lowerRecord, lower.data(), &lowerRegion);
    EXPECT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    const Uint8* base = store.RequireLevelBytes(key, kTex2DTarget, 0, "unit_hook_runs");
    ASSERT_NE(base, nullptr);
    for (SizeT i = 0; i < 32; ++i) EXPECT_EQ(base[i], 0x33) << "byte " << i << " of the lower run";
    for (SizeT i = 32; i < 64; ++i) EXPECT_EQ(base[i], 0x77) << "byte " << i << " of the upper run";

    MG_Pipe::MGPHandleOnly death{};
    death.Handle = res;
    death.Kind = static_cast<Uint32>(MG_Pipe::MGPipeKind::Texture);
    MG_Pipe::MGPipeApplyResourceDestroy(death);
    EXPECT_FALSE(store.HasShadow(key));
}

int main(int argc, char** argv) {
    // Before anything logs: MG_Util::Debug::InitFile() reads the variable once, on the first
    // write, and caches the FILE*. The name carries this process's pid, because
    // gtest_discover_tests runs every case as its own process, in parallel under ctest -j.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-stagedtexture-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
    // THIS PROCESS IS A SPLIT ONE - ServerLoopTest's main() ruling, and it matters twice here:
    // ServerStagedTexture() latches its copying arm off MG_Config::Transport at first use,
    // and a suite that left it at Monolith would be testing the monolith answers.
    MG_Config::Transport = MG_Config::TransportMode::InProcess;
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
