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
#include <MG_Backend/Record/StagedTextureStore.h>
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

namespace Server = MobileGL::MG_Record; // P13 W5: the stores moved to MG_Backend/Record

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

    // PH-4: resource_subdata lands only in a level a respecify DECLARED - the emitter's
    // glTexImage*D always sends one before the bytes. The production cases below used to adopt
    // into a level nothing had declared, which PH-4 refuses by name
    // (Fatal{ProtocolCorruption, "StagedTextureStore.LevelExtent"}), so each now declares level 0
    // of the Texture2D upload target at its 4x4x1 extent through the real applier entry point.
    void DeclareTex2DLevel0(const MG_Pipe::MGPResourceDesc& desc, Uint32 width, Uint32 height) {
        const MG_Pipe::MGPRespecifiedLevel level = MG_Pipe::MGPipeMakeRespecifiedLevel(
            MG_Pipe::MGPipePackSubDataTarget(desc.Target, kTex2DTarget), 0, width, height, 1);
        ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceRespecify(desc, nullptr, &level));
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
    splitStore.NoteLevelDefined(key, kTex2DTarget, 0, extent);
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

TEST(StagedTextureStoreTest, AMutableNoncanonicalLevelKeepsItsExactDeclaredExtentAndBound) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(41, 1));
    const IntVec3 noncanonical{3, 2, 1};

    store.NoteLevelDefined(key, kTex2DTarget, 2, noncanonical,
                           static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D),
                           static_cast<Uint32>(TextureInternalFormat::RGBA8));
    EXPECT_TRUE(store.IsLevelDefined(key, kTex2DTarget, 2));
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 2), noncanonical)
        << "a null-data mutable mip lost the exact extent accepted by resource_respecify";
    EXPECT_EQ(store.LevelDeclaredByteBound(key, kTex2DTarget, 2), 24u);
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 2));
}

// AN EMPTY LEVEL IS A DEFINITION (P7 gate 5). glTexImage1D(width 0) / glTexImage2D(0 x 0) are
// legal GL and dEQP's state reset issues them after every case; the frontend reports the level's
// exact extent as {0,1,1} / {0,0,1}. The store must take it as a definition that REPLACES the
// level (the old run and its coverage go), answer the exact empty extent, and give it no byte
// bound - never a session Fatal. A negative component (a carrier word past INT_MAX) stays one.
TEST(StagedTextureStoreTest, AnEmptyLevelIsADefinitionWithNoByteBound) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(42, 1));
    constexpr Uint16 kTex1DTarget = static_cast<Uint16>(TextureUploadTarget::Texture1D);
    const auto tex1D = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex1D);
    const auto rgba8 = static_cast<Uint32>(TextureInternalFormat::RGBA8);

    store.NoteLevelDefined(key, kTex1DTarget, 0, IntVec3{4, 1, 1}, tex1D, rgba8);
    Vector<Uint8> bytes(16, 0x5A);
    store.Adopt(key, kTex1DTarget, 0, IntVec3{4, 1, 1}, bytes.data(), bytes.size());
    ASSERT_TRUE(store.IsCovered(key, kTex1DTarget, 0));

    store.NoteLevelDefined(key, kTex1DTarget, 0, IntVec3{0, 1, 1}, tex1D, rgba8);
    EXPECT_TRUE(store.IsLevelDefined(key, kTex1DTarget, 0)) << "an empty level still exists";
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex1DTarget, 0), IntVec3(0, 1, 1))
        << "the store must answer the frontend's own exact extent for an empty level";
    EXPECT_EQ(store.LevelDeclaredByteBound(key, kTex1DTarget, 0), 0u) << "an empty level has no texels";
    EXPECT_EQ(store.LevelByteSize(key, kTex1DTarget, 0), 0u) << "the 4-texel run belongs to the old level";
    EXPECT_FALSE(store.IsCovered(key, kTex1DTarget, 0));

    // And back: the next non-empty definition takes its own bytes as usual.
    store.NoteLevelDefined(key, kTex1DTarget, 0, IntVec3{2, 1, 1}, tex1D, rgba8);
    Vector<Uint8> smaller(8, 0x3C);
    store.Adopt(key, kTex1DTarget, 0, IntVec3{2, 1, 1}, smaller.data(), smaller.size());
    EXPECT_TRUE(store.IsCovered(key, kTex1DTarget, 0));
    EXPECT_EQ(store.LevelDeclaredByteBound(key, kTex1DTarget, 0), 8u);

    // The 2D and 3D spellings of the same reset.
    const Uint64 key2D = Server::StagedTextureStore::KeyForHandle(TestHandle(43, 1));
    store.NoteLevelDefined(key2D, kTex2DTarget, 0, IntVec3{0, 0, 1},
                           static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D), rgba8);
    EXPECT_EQ(store.LevelExtentOrUndefined(key2D, kTex2DTarget, 0), IntVec3(0, 0, 1));
    const Uint64 key3D = Server::StagedTextureStore::KeyForHandle(TestHandle(44, 1));
    constexpr Uint16 kTex3DTarget = static_cast<Uint16>(TextureUploadTarget::Texture3D);
    store.NoteLevelDefined(key3D, kTex3DTarget, 0, IntVec3{0, 0, 0},
                           static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex3D), rgba8);
    EXPECT_TRUE(store.IsLevelDefined(key3D, kTex3DTarget, 0));
    EXPECT_EQ(store.LevelDeclaredByteBound(key3D, kTex3DTarget, 0), 0u);
}

// The wire half of the same rule: the carrier's only noncanonical shape is a depth word past
// 32 bits. A zero component is an empty level, which the store above takes.
TEST(StagedTextureStoreTest, AZeroExtentCarrierIsCanonicalAndAHighDepthWordIsNot) {
    MG_Pipe::MGPResourceDesc desc{};
    desc.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex1D);
    MG_Pipe::MGPipeSetRespecifiedLevel(desc, kTex2DTarget, 0, 0, 1, 1);
    EXPECT_TRUE(MG_Pipe::MGPipeRespecifiedExtentCarrierIsCanonical(desc))
        << "glTexImage1D(width 0) is legal GL; its carrier must not be refused";
    MG_Pipe::MGPipeSetRespecifiedLevel(desc, kTex2DTarget, 0, 0, 0, 0);
    EXPECT_TRUE(MG_Pipe::MGPipeRespecifiedExtentCarrierIsCanonical(desc));
    desc.BufSize |= (Uint64{1} << 32);
    EXPECT_FALSE(MG_Pipe::MGPipeRespecifiedExtentCarrierIsCanonical(desc));
}

TEST(StagedTextureStoreTest, MultisampleAndTextureBufferTargetsCannotAcceptStagedTexelBytes) {
    EXPECT_FALSE(Server::StagedTextureTargetSupportsSubData(
        static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2DMS)));
    EXPECT_FALSE(Server::StagedTextureTargetSupportsSubData(
        static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2DMSArray)));
    EXPECT_FALSE(Server::StagedTextureTargetSupportsSubData(
        static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::TexBuffer)));
    EXPECT_TRUE(Server::StagedTextureTargetSupportsSubData(
        static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D)));
}

// Keys are independent, ResetLevels drops one resource's whole chain, Drop one key and
// DropAll every one - the three events the contract names (respecify, destroy, context death)
// each have their call site, and these are the answers those call sites rely on.
TEST(StagedTextureStoreTest, ResetDropAndDropAllForgetExactlyWhatTheyName) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 a = Server::StagedTextureStore::KeyForHandle(TestHandle(5, 1));
    const Uint64 b = Server::StagedTextureStore::KeyForHandle(TestHandle(6, 1));
    Vector<Uint8> bytes4x4(64, 0x22);
    Vector<Uint8> bytes2x2(16, 0x22);

    store.NoteLevelDefined(a, kTex2DTarget, 0, IntVec3{4, 4, 1});
    store.Adopt(a, kTex2DTarget, 0, IntVec3{4, 4, 1}, bytes4x4.data(), bytes4x4.size());
    store.NoteLevelDefined(a, kTex2DTarget, 1, IntVec3{2, 2, 1});
    store.Adopt(a, kTex2DTarget, 1, IntVec3{2, 2, 1}, bytes2x2.data(), bytes2x2.size());
    store.NoteLevelDefined(b, kTex2DTarget, 0, IntVec3{4, 4, 1});
    store.Adopt(b, kTex2DTarget, 0, IntVec3{4, 4, 1}, bytes4x4.data(), bytes4x4.size());
    ASSERT_EQ(store.TrackedResources(), 2u);
    ASSERT_EQ(store.TrackedLevelCount(a), 2u);

    store.ResetLevels(a);
    EXPECT_FALSE(store.HasShadow(a)) << "a whole-resource respecify forgets every level";
    EXPECT_TRUE(store.IsCovered(b, kTex2DTarget, 0));

    store.NoteLevelDefined(a, kTex2DTarget, 0, IntVec3{4, 4, 1});
    store.Adopt(a, kTex2DTarget, 0, IntVec3{4, 4, 1}, bytes4x4.data(), bytes4x4.size());
    store.Drop(a);
    EXPECT_EQ(store.TrackedResources(), 1u);
    EXPECT_TRUE(store.IsCovered(b, kTex2DTarget, 0));

    store.DropAll();
    EXPECT_EQ(store.TrackedResources(), 0u);
    EXPECT_FALSE(store.HasShadow(b));
}

// P14: the in-process display server serves several sessions from one store. A test-installed
// bucket probe stands in for SessionRuntime's: a thread "in" a bucket answers it, a thread in
// none answers false (the process-wide bucket).
namespace {
    thread_local Server::StagedBucket t_testBucket;
    thread_local Bool t_testBucketSet = false;

    Bool TestStagedBucket(Server::StagedBucket* outBucket) {
        if (!t_testBucketSet) return false;
        *outBucket = t_testBucket;
        return true;
    }

    void EnterBucket(Uint64 session, Uint64 group) {
        t_testBucket = Server::StagedBucket{session, group};
        t_testBucketSet = true;
    }

    struct ScopedTestBucketProbe {
        Server::StagedBucketResolver Previous;
        ScopedTestBucketProbe() : Previous(Server::g_stagedBucketResolver.load()) {
            Server::SetStagedBucketResolver(&TestStagedBucket);
        }
        ~ScopedTestBucketProbe() {
            t_testBucketSet = false;
            Server::SetStagedBucketResolver(Previous);
        }
    };
} // namespace

// The anland Plasma crash, at unit scope: KWin and another client both hold texture {38, 0};
// the other client's session ends (DestroyEGLContext -> DropAll), and KWin's next upload into
// the level it declared long ago used to read "arrived before the server declared texture
// level" and abort the display process. Each session's {slot, gen} is its own texture, and a
// session's DropAll takes only its own levels.
TEST(StagedTextureStoreTest, TwoSessionsHoldTheSameHandleAndOneSessionsDropAllSparesTheOther) {
    ScopedTestBucketProbe probe;
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(38, 0));
    Vector<Uint8> kwinBytes(64, 0x11);
    Vector<Uint8> appBytes(16, 0x22);

    EnterBucket(/*session=*/1, /*group=*/10);
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{4, 4, 1});
    store.Adopt(key, kTex2DTarget, 0, IntVec3{4, 4, 1}, kwinBytes.data(), kwinBytes.size());

    EnterBucket(/*session=*/2, /*group=*/20);
    EXPECT_FALSE(store.IsLevelDefined(key, kTex2DTarget, 0))
        << "another session's {slot, gen} answered for this one's";
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{2, 2, 1});
    store.Adopt(key, kTex2DTarget, 0, IntVec3{2, 2, 1}, appBytes.data(), appBytes.size());

    EnterBucket(1, 10);
    EXPECT_EQ(store.LevelExtentOrUndefined(key, kTex2DTarget, 0), (IntVec3{4, 4, 1}))
        << "the second session's definition redefined the first's level";

    EnterBucket(2, 20);
    store.DropAll();
    EXPECT_FALSE(store.HasShadow(key));

    EnterBucket(1, 10);
    ASSERT_TRUE(store.IsLevelDefined(key, kTex2DTarget, 0))
        << "a neighbour session's teardown dropped this session's declared level";
    EXPECT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    EXPECT_NE(store.AdoptRun(key, kTex2DTarget, 0, IntVec3{4, 4, 1}, 0, kwinBytes.data(), kwinBytes.size()),
              nullptr);

    EnterBucket(1, 11);
    EXPECT_FALSE(store.HasShadow(key)) << "a non-shared group of the same session is its own name space";

    t_testBucketSet = false;
    store.DropAll();
    EnterBucket(1, 10);
    EXPECT_FALSE(store.HasShadow(key)) << "a thread that is no session's drops the whole store";
}

// The buffer half: its shadows are keyed by twin address (already unique), but DropAll ran the
// same process-wide clear on a session's teardown.
TEST(StagedTextureStoreTest, OneSessionsBufferDropAllSparesTheOthersShadows) {
    ScopedTestBucketProbe probe;
    Server::StagedShadowStore store(/*copies=*/true);
    const int kwinTwin = 0;
    const int appTwin = 0;
    Vector<Uint8> bytes(32, 0x33);

    EnterBucket(1, 10);
    store.Adopt(&kwinTwin, bytes.size(), bytes.data(), 0, bytes.size());
    EnterBucket(2, 20);
    store.Adopt(&appTwin, bytes.size(), bytes.data(), 0, bytes.size());

    store.DropAll();
    EXPECT_FALSE(store.HasShadow(&appTwin));
    EXPECT_TRUE(store.HasShadow(&kwinTwin)) << "a neighbour session's teardown dropped this session's bytes";
    EXPECT_TRUE(store.IsCovered(&kwinTwin, 0, bytes.size()));

    t_testBucketSet = false;
    store.DropAll();
    EXPECT_FALSE(store.HasShadow(&kwinTwin));
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

// ---- P8-E (docs/Disaggregated/notes/p8/E.md): the store follows glCopyImageSubData -----------
//
// Two 4x4 RGBA8 levels (four bytes a texel), each adopted whole with a distinct byte pattern so a
// mis-addressed row or a swapped endpoint shows as a wrong byte, not a coincidentally right one.
namespace {
    const IntVec3 kFollowExtent{4, 4, 1};

    Vector<Uint8> FollowPattern(Uint8 base) {
        Vector<Uint8> bytes(64);
        for (SizeT i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<Uint8>(base + i);
        return bytes;
    }

    void DefineAndAdopt(Server::StagedTextureStore& store, Uint64 key, const Vector<Uint8>& bytes) {
        store.NoteLevelDefined(key, kTex2DTarget, 0, kFollowExtent);
        store.Adopt(key, kTex2DTarget, 0, kFollowExtent, bytes.data(), bytes.size());
    }

    // The byte (x, y, channel) of a 4x4 RGBA8 level image.
    SizeT At(Int x, Int y, Int c) { return static_cast<SizeT>((y * 4 + x) * 4 + c); }
} // namespace

// THE MOVE: the 2x3 window at (1, 1) of the source lands at (2, 0) of the destination, every other
// destination byte and every source byte stays, and only the destination is flagged as holding a
// copy the client never saw.
TEST(StagedTextureStoreTest, AFollowedCopyMovesTheWindowAndLeavesTheSourceAlone) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 src = Server::StagedTextureStore::KeyForHandle(TestHandle(60, 1));
    const Uint64 dst = Server::StagedTextureStore::KeyForHandle(TestHandle(61, 1));
    const auto srcBytes = FollowPattern(0x00);
    const auto dstBytes = FollowPattern(0x80);
    DefineAndAdopt(store, src, srcBytes);
    DefineAndAdopt(store, dst, dstBytes);

    ASSERT_TRUE(store.FollowCopy(src, kTex2DTarget, 0, IntVec3{1, 1, 0}, dst, kTex2DTarget, 0, IntVec3{2, 0, 0},
                                 IntVec3{2, 3, 1}));
    EXPECT_TRUE(store.LevelHoldsFollowedCopy(dst, kTex2DTarget, 0));
    EXPECT_FALSE(store.LevelHoldsFollowedCopy(src, kTex2DTarget, 0));
    const Uint8* got = store.RequireLevelBytes(dst, kTex2DTarget, 0, "unit");
    const Uint8* source = store.RequireLevelBytes(src, kTex2DTarget, 0, "unit");
    for (Int y = 0; y < 4; ++y) {
        for (Int x = 0; x < 4; ++x) {
            const Bool copied = x >= 2 && y < 3;
            for (Int c = 0; c < 4; ++c) {
                const Uint8 want = copied ? srcBytes[At(x - 2 + 1, y + 1, c)] : dstBytes[At(x, y, c)];
                ASSERT_EQ(got[At(x, y, c)], want) << "destination texel (" << x << ", " << y << ")";
                ASSERT_EQ(source[At(x, y, c)], srcBytes[At(x, y, c)]) << "the copy moved a SOURCE byte";
            }
        }
    }
}

// P13 W4b: THE ALIASING STORE FOLLOWS TOO, WITHOUT WRITING FRONTEND MEMORY. On monolith's record
// arm both levels are the frontend's own level shadows, held by address. The copy takes the
// destination into a server-owned copy and moves the window there; the frontend bytes it stood for
// stay exactly as they were, and a later client run lands only its boxes on the copy.
#if MOBILEGL_BUILD_DISAGGREGATED
TEST(StagedTextureStoreTest, AnAliasingStoreFollowsACopyIntoAServerOwnedDestination) {
    const auto savedTransport = MG_Config::Transport;
    MG_Config::Transport = MG_Config::TransportMode::Monolith;
    struct Restore {
        MG_Config::TransportMode transport;
        ~Restore() { MG_Config::Transport = transport; }
    } restore{savedTransport};
    Server::StagedTextureStore store(/*copies=*/false);
    ASSERT_TRUE(store.Aliases());
    const Uint64 src = Server::StagedTextureStore::KeyForHandle(TestHandle(66, 1));
    const Uint64 dst = Server::StagedTextureStore::KeyForHandle(TestHandle(67, 1));
    const auto srcBytes = FollowPattern(0x00);
    auto dstFrontend = FollowPattern(0x80);
    const auto dstBefore = dstFrontend;
    store.NoteLevelDefined(src, kTex2DTarget, 0, kFollowExtent);
    store.Adopt(src, kTex2DTarget, 0, kFollowExtent, srcBytes.data(), srcBytes.size(), /*frontendShadow=*/true);
    store.NoteLevelDefined(dst, kTex2DTarget, 0, kFollowExtent);
    store.Adopt(dst, kTex2DTarget, 0, kFollowExtent, dstFrontend.data(), dstFrontend.size(), /*frontendShadow=*/true);
    ASSERT_EQ(store.RequireLevelBytes(dst, kTex2DTarget, 0, "unit"), dstFrontend.data()) << "the level is aliased";

    ASSERT_TRUE(store.FollowCopy(src, kTex2DTarget, 0, IntVec3{1, 1, 0}, dst, kTex2DTarget, 0, IntVec3{2, 0, 0},
                                 IntVec3{2, 3, 1}));
    EXPECT_TRUE(store.LevelHoldsFollowedCopy(dst, kTex2DTarget, 0));
    EXPECT_EQ(dstFrontend, dstBefore) << "the copy wrote the frontend's level shadow";
    const Uint8* got = store.RequireLevelBytes(dst, kTex2DTarget, 0, "unit");
    ASSERT_NE(got, dstFrontend.data()) << "the destination is still aliased after a followed copy";
    for (Int y = 0; y < 4; ++y) {
        for (Int x = 0; x < 4; ++x) {
            const Bool copied = x >= 2 && y < 3;
            for (Int c = 0; c < 4; ++c) {
                const Uint8 want = copied ? srcBytes[At(x - 2 + 1, y + 1, c)] : dstBefore[At(x, y, c)];
                ASSERT_EQ(got[At(x, y, c)], want) << "destination texel (" << x << ", " << y << ")";
            }
        }
    }

    // The client then writes texel (0, 3) of its shadow: only that box lands on the copy.
    dstFrontend[At(0, 3, 0)] = 0x11;
    const Server::StagedTextureStore::ClientTexelBox box{0, 3, 0, 1, 1, 1};
    ASSERT_TRUE(store.AdoptClientBoxes(dst, kTex2DTarget, 0, kFollowExtent, 0, dstFrontend.data(), dstFrontend.size(),
                                       &box, 1));
    got = store.RequireLevelBytes(dst, kTex2DTarget, 0, "unit");
    EXPECT_EQ(got[At(0, 3, 0)], 0x11);
    EXPECT_EQ(got[At(2, 0, 0)], srcBytes[At(1, 1, 0)]) << "the client's box wiped the followed copy";
}
#endif

// Every shape the store cannot address, and a source it can no longer vouch for, is REFUSED with
// the destination untouched - the caller marks it driver-written instead. The monolith store
// follows nothing at all.
TEST(StagedTextureStoreTest, ACopyTheStoreCannotVouchForIsNotFollowed) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 src = Server::StagedTextureStore::KeyForHandle(TestHandle(62, 1));
    const Uint64 dst = Server::StagedTextureStore::KeyForHandle(TestHandle(63, 1));
    const Uint64 narrow = Server::StagedTextureStore::KeyForHandle(TestHandle(64, 1));
    const Uint64 bare = Server::StagedTextureStore::KeyForHandle(TestHandle(65, 1));
    const auto dstBytes = FollowPattern(0x80);
    DefineAndAdopt(store, src, FollowPattern(0x00));
    DefineAndAdopt(store, dst, dstBytes);
    store.NoteLevelDefined(narrow, kTex2DTarget, 0, IntVec3{8, 8, 1},
                           static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D),
                           static_cast<Uint32>(TextureInternalFormat::R8)); // 64 bytes, one a texel
    Vector<Uint8> narrowBytes(64, 0x11);
    store.Adopt(narrow, kTex2DTarget, 0, IntVec3{8, 8, 1}, narrowBytes.data(), narrowBytes.size());
    store.NoteLevelDefined(bare, kTex2DTarget, 0, kFollowExtent); // defined, no bytes crossed

    const IntVec3 origin{0, 0, 0};
    EXPECT_FALSE(store.FollowCopy(src, kTex2DTarget, 0, IntVec3{3, 0, 0}, dst, kTex2DTarget, 0, origin,
                                  IntVec3{2, 1, 1}))
        << "the box runs off the source's right edge";
    EXPECT_FALSE(store.FollowCopy(narrow, kTex2DTarget, 0, origin, dst, kTex2DTarget, 0, origin, IntVec3{1, 1, 1}))
        << "a one-byte texel moved into a four-byte one";
    EXPECT_FALSE(store.FollowCopy(src, kTex2DTarget, 0, origin, bare, kTex2DTarget, 0, origin, IntVec3{2, 2, 1}))
        << "a partial copy into a level none of whose bytes crossed";
    EXPECT_FALSE(store.FollowCopy(bare, kTex2DTarget, 0, origin, dst, kTex2DTarget, 0, origin, IntVec3{1, 1, 1}))
        << "a source none of whose bytes crossed";
    store.MarkDriverWritten(src);
    EXPECT_FALSE(store.FollowCopy(src, kTex2DTarget, 0, origin, dst, kTex2DTarget, 0, origin, IntVec3{1, 1, 1}))
        << "the driver wrote the source behind the store's back, so its bytes are not the texels copied";
    EXPECT_FALSE(store.LevelHoldsFollowedCopy(dst, kTex2DTarget, 0));
    const Uint8* got = store.RequireLevelBytes(dst, kTex2DTarget, 0, "unit");
    for (SizeT i = 0; i < dstBytes.size(); ++i) ASSERT_EQ(got[i], dstBytes[i]) << "a refused copy moved byte " << i;

    Server::StagedTextureStore monolithStore(/*copies=*/false);
    EXPECT_FALSE(monolithStore.FollowCopy(src, kTex2DTarget, 0, origin, dst, kTex2DTarget, 0, origin,
                                          IntVec3{1, 1, 1}));
}

// A copy that covers ALL of a destination level whose bytes never crossed (glTexStorage, no upload)
// makes that level's content known: the store completes it.
TEST(StagedTextureStoreTest, AWholeLevelCopyCompletesADestinationThatHadNoBytes) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 src = Server::StagedTextureStore::KeyForHandle(TestHandle(66, 1));
    const Uint64 dst = Server::StagedTextureStore::KeyForHandle(TestHandle(67, 1));
    const auto srcBytes = FollowPattern(0x40);
    DefineAndAdopt(store, src, srcBytes);
    store.NoteLevelDefined(dst, kTex2DTarget, 0, kFollowExtent);
    ASSERT_FALSE(store.IsCovered(dst, kTex2DTarget, 0));
    ASSERT_TRUE(store.FollowCopy(src, kTex2DTarget, 0, IntVec3{0, 0, 0}, dst, kTex2DTarget, 0, IntVec3{0, 0, 0},
                                 kFollowExtent));
    ASSERT_TRUE(store.IsCovered(dst, kTex2DTarget, 0));
    const Uint8* got = store.RequireLevelBytes(dst, kTex2DTarget, 0, "unit");
    for (SizeT i = 0; i < srcBytes.size(); ++i) ASSERT_EQ(got[i], srcBytes[i]) << "byte " << i;
}

// The client's next run for a level holding a followed copy is the CLIENT's shadow, which never saw
// the copy: only the boxes it says it wrote land, the copied window survives, and a box whose rows
// are not in the run is refused with nothing written.
TEST(StagedTextureStoreTest, AClientRunOverAFollowedCopyLandsOnlyItsBoxes) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 src = Server::StagedTextureStore::KeyForHandle(TestHandle(68, 1));
    const Uint64 dst = Server::StagedTextureStore::KeyForHandle(TestHandle(69, 1));
    const auto srcBytes = FollowPattern(0x00);
    const auto dstBytes = FollowPattern(0x80);
    DefineAndAdopt(store, src, srcBytes);
    DefineAndAdopt(store, dst, dstBytes);
    ASSERT_TRUE(store.FollowCopy(src, kTex2DTarget, 0, IntVec3{0, 0, 0}, dst, kTex2DTarget, 0, IntVec3{0, 0, 0},
                                 IntVec3{2, 2, 1}));

    // The client's whole-level shadow: its own old bytes everywhere, 0xEE in the one row it wrote.
    Vector<Uint8> run = dstBytes;
    for (Int x = 0; x < 4; ++x) {
        for (Int c = 0; c < 4; ++c) run[At(x, 3, c)] = 0xEE;
    }
    const Server::StagedTextureStore::ClientTexelBox lastRow{0, 3, 0, 4, 1, 1};
    ASSERT_TRUE(store.AdoptClientBoxes(dst, kTex2DTarget, 0, kFollowExtent, 0, run.data(), run.size(), &lastRow, 1));
    const Uint8* got = store.RequireLevelBytes(dst, kTex2DTarget, 0, "unit");
    for (Int y = 0; y < 4; ++y) {
        for (Int x = 0; x < 4; ++x) {
            for (Int c = 0; c < 4; ++c) {
                const Uint8 want = y == 3 ? Uint8{0xEE} : (x < 2 && y < 2) ? srcBytes[At(x, y, c)] : dstBytes[At(x, y, c)];
                ASSERT_EQ(got[At(x, y, c)], want) << "texel (" << x << ", " << y << ")";
            }
        }
    }
    EXPECT_TRUE(store.LevelHoldsFollowedCopy(dst, kTex2DTarget, 0));
    EXPECT_FALSE(store.IsDriverWritten(dst));

    // A slab run that holds rows 2..3 only (imageOffset 32): row 0 is not in it.
    const Server::StagedTextureStore::ClientTexelBox firstRow{0, 0, 0, 4, 1, 1};
    Vector<Uint8> slab(32, 0x33);
    EXPECT_FALSE(store.AdoptClientBoxes(dst, kTex2DTarget, 0, kFollowExtent, 32, slab.data(), slab.size(), &firstRow, 1));
    EXPECT_EQ(got[At(0, 0, 0)], srcBytes[At(0, 0, 0)]) << "a refused run wrote into the level";
}

// A run adopted WHOLE over a followed copy (any caller that does not take AdoptClientBoxes' route)
// puts the client's stale texels back over it - so the store stops standing for the texture.
TEST(StagedTextureStoreTest, AWholeRunOverAFollowedCopyMarksTheTextureDriverWritten) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 src = Server::StagedTextureStore::KeyForHandle(TestHandle(70, 1));
    const Uint64 dst = Server::StagedTextureStore::KeyForHandle(TestHandle(71, 1));
    const auto dstBytes = FollowPattern(0x80);
    DefineAndAdopt(store, src, FollowPattern(0x00));
    DefineAndAdopt(store, dst, dstBytes);
    ASSERT_TRUE(store.FollowCopy(src, kTex2DTarget, 0, IntVec3{0, 0, 0}, dst, kTex2DTarget, 0, IntVec3{0, 0, 0},
                                 IntVec3{1, 1, 1}));
    store.AdoptRun(dst, kTex2DTarget, 0, kFollowExtent, 0, dstBytes.data(), dstBytes.size());
    EXPECT_TRUE(store.IsDriverWritten(dst));
    EXPECT_FALSE(store.LevelHoldsFollowedCopy(dst, kTex2DTarget, 0));
    EXPECT_FALSE(store.IsDriverWritten(src));
}

// The driver-written mark is per texture and sticky: a whole-resource respecify does not clear it
// (an attachment made before it keeps writing after it), only the key's death does. Inert on a
// monolith store.
TEST(StagedTextureStoreTest, TheDriverWrittenMarkOutlivesAResetAndDiesWithTheKey) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 a = Server::StagedTextureStore::KeyForHandle(TestHandle(72, 1));
    const Uint64 b = Server::StagedTextureStore::KeyForHandle(TestHandle(73, 1));
    EXPECT_FALSE(store.IsDriverWritten(a));
    store.MarkDriverWritten(a);
    store.MarkDriverWritten(b);
    EXPECT_TRUE(store.IsDriverWritten(a));
    store.ResetLevels(a);
    EXPECT_TRUE(store.IsDriverWritten(a)) << "a respecify forgot that the driver writes this texture";
    store.Drop(a);
    EXPECT_FALSE(store.IsDriverWritten(a)) << "a destroyed texture's mark must not answer for the slot's next owner";
    EXPECT_TRUE(store.IsDriverWritten(b));
    store.DropAll();
    EXPECT_FALSE(store.IsDriverWritten(b));

    Server::StagedTextureStore monolithStore(/*copies=*/false);
    monolithStore.MarkDriverWritten(a);
    EXPECT_FALSE(monolithStore.IsDriverWritten(a));
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

    store.NoteLevelDefined(key, kTex2DTarget, 0, extent);
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
    store.NoteLevelDefined(holed, kTex2DTarget, 0, extent);
    Vector<Uint8> part(16, 0x11);
    store.AdoptRun(holed, kTex2DTarget, 0, extent, 0, part.data(), part.size());
    store.AdoptRun(holed, kTex2DTarget, 0, extent, 32, upper.data(), upper.size());
    EXPECT_EQ(store.LevelCoveredRunCount(holed, kTex2DTarget, 0), 2u)
        << "two runs with a 16-byte gap merged into one";
    EXPECT_FALSE(store.IsCovered(holed, kTex2DTarget, 0)) << "a span across the gap reads covered";
    // A short leading run is not a complete level even if its current high-water mark ends at
    // that run. The store checks coverage against the server-derived byte bound, not its prefix.
    const Uint64 leading = Server::StagedTextureStore::KeyForHandle(TestHandle(14, 1));
    store.NoteLevelDefined(leading, kTex2DTarget, 0, IntVec3{4, 4, 1});
    store.AdoptRun(leading, kTex2DTarget, 0, IntVec3{4, 4, 1}, 0, part.data(), part.size());
    EXPECT_FALSE(store.IsCovered(leading, kTex2DTarget, 0))
        << "the leading prefix must not stand in for the declared level";
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

#if MOBILEGL_BUILD_DISAGGREGATED
// B4's rule under PH-4's declared extents: a level redefined at a new extent (the respecify's
// NoteLevelDefined, which every run now needs before it lands) starts with no coverage, so the
// first run of the new definition cannot be completed by the old definition's bytes. A run that
// still carries the old extent is StagedTextureStore.LevelExtent's refusal (ServerSpawnTest's
// raw-peer control), so the new-extent runs are the only ones that can land here.
TEST(StagedTextureStoreTest, AdoptRunExtentMoveDoesNotInheritOldCoverage) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(15, 1));
    Vector<Uint8> oldRun(64, 0x44);  // the whole 4x4 RGBA8 level
    Vector<Uint8> newRun(64, 0x99);  // the upper half of the 8x4 RGBA8 level
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{4, 4, 1});
    store.AdoptRun(key, kTex2DTarget, 0, IntVec3{4, 4, 1}, 0, oldRun.data(), oldRun.size());
    ASSERT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{8, 4, 1});
    ASSERT_EQ(store.LevelDeclaredByteBound(key, kTex2DTarget, 0), 128u);
    store.AdoptRun(key, kTex2DTarget, 0, IntVec3{8, 4, 1}, 64, newRun.data(), newRun.size());
    EXPECT_FALSE(store.IsCovered(key, kTex2DTarget, 0))
        << "the old level's [0,64) run was mistaken for bytes of the new extent";
    EXPECT_EQ(store.LevelCoveredRunCount(key, kTex2DTarget, 0), 1u);
    Vector<Uint8> newLower(64, 0xAA);
    store.AdoptRun(key, kTex2DTarget, 0, IntVec3{8, 4, 1}, 0, newLower.data(), newLower.size());
    const Uint8* base = store.RequireLevelBytes(key, kTex2DTarget, 0, "unit_extent_move");
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base[0], 0xAAu);
    EXPECT_EQ(base[64], 0x99u);
}
#endif

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
    Vector<Uint8> bytes(64, 0x33);
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{4, 4, 1});
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
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{4, 4, 1});
    store.NoteLevelDefined(key, kTex2DTarget, 1, IntVec3{4, 4, 1});
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

// F2 LANE REPAIR, R8: TextureInternalFormat::R8 is enumerator 0, and PH-4's first draft read
// `InternalFormat == 0` as "never declared" - so an R8 level a respecify HAD declared refused its
// own upload as `StagedTextureStore.LevelExtent`. Red-once: put the `== 0` test back and the run
// below dies by that name.
TEST(StagedTextureStoreTest, AnR8LevelIsDeclaredAndTakesItsUpload) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(15, 1));
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{4, 4, 1},
                           static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D),
                           static_cast<Uint32>(TextureInternalFormat::R8));
    ASSERT_EQ(store.LevelDeclaredByteBound(key, kTex2DTarget, 0), 16u) << "4x4 texels of one byte";
    Vector<Uint8> texels(16, 0x3C);
    store.AdoptRun(key, kTex2DTarget, 0, IntVec3{4, 4, 1}, 0, texels.data(), texels.size());
    EXPECT_TRUE(store.IsCovered(key, kTex2DTarget, 0));
    EXPECT_EQ(store.RequireLevelBytes(key, kTex2DTarget, 0, "unit_r8")[15], 0x3C);
}

// F2 LANE REPAIR, BYTELESS DECLARATIONS: a level whose format has no uncompressed size here
// (TextureInternalFormat::Unknown - what the TextureView / TextureViewAlias / TextureUploadShape
// split lanes declare) and an EMPTY level (glTexImage*D with a zero extent -
// UnboundImageDescriptorScenario's incomplete default texture) are both legal DECLARATIONS: no
// Fatal at the respecify, the level exists, and its byte bound is 0. A staged run into such a
// level is then refused by its own name. Red-once: drop the LevelBound refusal and the same run
// dies as `StagedTextureStore.CopyRunInto` instead (end 4 > bound 0), which the name check
// below reads as red. ONE DEATH PER CASE, ServerLoopTest's ruling.
#if !defined(_WIN32)
TEST(StagedTextureStoreTest, ABytelessDeclaredLevelIsLegalAndRefusesARunByName) {
    Server::StagedTextureStore store(/*copies=*/true);
    const Uint64 key = Server::StagedTextureStore::KeyForHandle(TestHandle(16, 1));
    const auto tex2D = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
    store.NoteLevelDefined(key, kTex2DTarget, 0, IntVec3{1, 1, 1}, tex2D,
                           static_cast<Uint32>(TextureInternalFormat::Unknown));
    store.NoteLevelDefined(key, kTex2DTarget, 1, IntVec3{0, 0, 1}, tex2D,
                           static_cast<Uint32>(TextureInternalFormat::RGBA8));
    ASSERT_TRUE(store.IsLevelDefined(key, kTex2DTarget, 0));
    ASSERT_TRUE(store.IsLevelDefined(key, kTex2DTarget, 1));
    EXPECT_EQ(store.LevelDeclaredByteBound(key, kTex2DTarget, 0), 0u);
    EXPECT_EQ(store.LevelDeclaredByteBound(key, kTex2DTarget, 1), 0u);

    Vector<Uint8> texel(4, 0x5A);
    EXPECT_EXIT(store.AdoptRun(key, kTex2DTarget, 0, IntVec3{1, 1, 1}, 0, texel.data(), texel.size()),
                ::testing::KilledBySignal(SIGABRT), ".*");
    const std::string log = ReadLog();
    EXPECT_NE(log.find("Fatal{ProtocolCorruption, \"StagedTextureStore.LevelBound\"}"), std::string::npos)
        << "the run died, but not as the byteless-level refusal; the log says: " << log;
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
    // RGBA8: the 64 bytes below are 4x4 texels of four bytes, and PH-4 bounds the run by the
    // declared level's w*h*d*bpp (a zero-initialised format is R8, one byte a texel).
    desc.InternalFormat = static_cast<Uint32>(TextureInternalFormat::RGBA8);
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.Levels = 1;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));
    DeclareTex2DLevel0(desc, 4, 4);

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
    // RGBA8 for the 64-byte 4x4 upload below (PH-4's w*h*d*bpp bound; zero would be R8).
    desc.InternalFormat = static_cast<Uint32>(TextureInternalFormat::RGBA8);
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.Levels = 3;
    MG_Pipe::MGPRespecifiedLevel level = MG_Pipe::MGPipeMakeRespecifiedLevel(
        MG_Pipe::MGPipePackSubDataTarget(desc.Target, kTex2DTarget), 0, 4, 4, 1);
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

    // Defining another mip must retain bytes already accepted for the first mip. PH-4: the scope
    // carries that mip's EXACT extent (TextureEmit.h reads it off the frontend's level), so level 1
    // of a 4x4 base crosses as 2x2x1 rather than inheriting level 0's numbers.
    level.Level = 1;
    level.Width = 2;
    level.Height = 2;
    level.Depth = 1;
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
    desc.InternalFormat = static_cast<Uint32>(TextureInternalFormat::RGBA8); // PH-4 bound: 4x4x4
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.Levels = 1;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));
    DeclareTex2DLevel0(desc, 4, 4);

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
    desc.InternalFormat = static_cast<Uint32>(TextureInternalFormat::RGBA8); // PH-4 bound: 4x4x4
    desc.Width = 4;
    desc.Height = 4;
    desc.Depth = 1;
    desc.Levels = 1;
    ASSERT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));
    DeclareTex2DLevel0(desc, 4, 4);

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
