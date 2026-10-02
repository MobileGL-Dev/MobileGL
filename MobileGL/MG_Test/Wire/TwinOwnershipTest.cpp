// MobileGL - MobileGL/MG_Test/Wire/TwinOwnershipTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P14 S6 (docs/Disaggregated/design/11-state-ownership.md): WHOSE TWIN IS THIS?
//
// WHAT THIS SUITE OWNS. A backend twin is a DRIVER object - a GL buffer id, a texture, a
// program, the storage behind them - so it belongs to one native context's object namespace.
// Until this slice the six twin registries were keyed by {slot, gen} ALONE, process-wide, which
// is wrong in both directions:
//
//   * two contexts of one session that do NOT share a share group have independent name spaces,
//     so the same handle value in both is legal and means two different objects. One table gave
//     the second context the FIRST one's twin - the same driver ids, the same persistent map,
//     the same pooled store, addressed as if they were its own;
//   * two contexts that DO share a group must see one set of objects. They were served by
//     whichever of them twinned first, which is right by accident and wrong as soon as the
//     other one asks first under a different key.
//
// This suite drives the twin tables directly - no EGL, no wire, no transport - because that is
// exactly what the slice is: `g_backendSamplerViews` (and the six TwinRegistry registries)
// resolve per {session, share group}, and every case below is a way of using two contexts that
// the process-wide table got wrong.
//
// HOW IT INSTALLS A KEY. The production resolver is the server's (SessionRuntime.cpp's
// `ResolveThreadTwinKey`, installed by ThreadSessionScope beside the native-tuple and applier
// probes, and deriving the group through MGPipeApplierShareGroupKeyFor so a twin and the record
// that describes it cannot land in different buckets). Here the resolver is this file's,
// driven by two thread_locals, so a case can be in whichever session and share group it means
// without standing up a transport. The session keys are large sentinels no real
// ServerSession address can collide with, and every case drops what it created on the way out.
//
// THE NEGATIVE CONTROL, run once and reverted (S5's shape, and the reason these cases are not
// an empty green): make SlotTableRegistry::ForCallingThread answer a single fixed {0, 0} bucket
// - i.e. restore the process-wide table this slice replaces - and FOUR of the five cases go red
// at their first per-group assertion: the nonshared pair, the per-session drop, the
// no-session case (which then cannot be told from a session's) and the six-registry one.
// `TwoContextsOfOneGroupShareTheGroupsTwins` STAYS GREEN under the control, and that is the
// correct answer rather than a gap: with ONE key both shapes agree, which is why the case
// asserts what sharing must do and not merely that two buckets exist.

#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Pipe/PipeApply.h>

// The twin tables run on the handle arm only while kMGPipeSubsystemEsprytSlots is set, and a
// unit binary never runs ConfigLoader's default (which sets every migrated subsystem). Same
// drive as ServerLoopTest's EGL fixture: set the bit, restore it on the way out.
#include <Config.h>

#include <gtest/gtest.h>

namespace {

    namespace GL = MobileGL::MG_Backend::DirectGLES;
    namespace P = MobileGL::MG_Pipe;
    using MobileGL::Bool;
    using MobileGL::SizeT;
    using MobileGL::Uint32;
    using MobileGL::Uint64;

    // The calling thread's {session, share group}, exactly the shape the production resolver
    // fills in. A zero session key is "not a session's thread" - the client process, the inproc
    // one-process shape, every unit case - and that is what keeps the process-wide bucket the
    // answer there.
    thread_local Uint64 t_sessionKey = 0;
    thread_local Uint64 t_shareGroupKey = 0;

    Bool TestTwinKeyResolver(GL::TwinKey* outKey) {
        if (t_sessionKey == 0) return false;
        outKey->SessionKey = t_sessionKey;
        outKey->ShareGroupKey = t_shareGroupKey;
        return true;
    }

    // A twin with no driver id and a trivial destructor, so a case can create and drop entries
    // on a REAL production table without a GL context and without a driver call at teardown.
    // (BackendSamplerViewObject's own comment says the same thing about itself; the alias keeps
    // the cases readable.)
    using Twin = GL::SamplerViewImpl::BackendSamplerViewObject;
    using RealTable = GL::SamplerViewImpl::BackendSamplerViewTable;

    // The six-registry shape, on the same trivial twin and on a kind no shipping path twins
    // (Query), so a case can instantiate it without touching a real slot space.
    using RegistryShapedTable = GL::TwinRegistry<MobileGL::MG_State::GLState::ITextureObject, Twin,
                                                 P::MGPipeKind::Query>;

    class TwinOwnershipTest : public ::testing::Test {
    protected:
        // Sentinels no real ServerSession address can be, so a stray registration elsewhere in
        // the process can never be what a case reads.
        static constexpr Uint64 kSessionA = 0x5E5510C000000000ull;
        static constexpr Uint64 kSessionB = 0x5E5510D000000000ull;
        // Share-group tokens are minted by the frontend (EGLState::CreateContext), dense and
        // 0-free: two contexts that share pass the SAME number, two that do not pass different
        // ones - which is why the key can be the group itself.
        static constexpr Uint64 kGroupOne = 0x2001;
        static constexpr Uint64 kGroupTwo = 0x2002;

        Uint64 m_savedPush = 0;

        void SetUp() override {
            m_savedPush = MobileGL::MG_Config::Features.PipePush;
            MobileGL::MG_Config::Features.PipePush |= P::kMGPipeSubsystemEsprytSlots;
            GL::SetTwinKeyResolver(&TestTwinKeyResolver);
            BindKey(kSessionA, kGroupOne);
        }

        void TearDown() override {
            MobileGL::MG_Config::Features.PipePush = m_savedPush;
            GL::SamplerViewImpl::g_backendSamplerViews.DropBucketsOfSession(kSessionA);
            GL::SamplerViewImpl::g_backendSamplerViews.DropBucketsOfSession(kSessionB);
            // The process-wide bucket the no-session case writes ({0, 0}): nothing else in this
            // binary uses it, and leaving it would make that case order-dependent.
            GL::SamplerViewImpl::g_backendSamplerViews.DropBucket(GL::TwinKey{0, 0});
            t_sessionKey = 0;
            t_shareGroupKey = 0;
            GL::SetTwinKeyResolver(nullptr);
        }

        static void BindKey(Uint64 session, Uint64 shareGroup) {
            t_sessionKey = session;
            t_shareGroupKey = shareGroup;
        }

        // Resolve-or-create on the real table, then stamp the twin so a case can tell WHICH
        // object it got back. The pointer is the twin the driver ids would hang off.
        static Twin* Mint(RealTable& table, P::MGPipeHandle handle, Uint64 stamp) {
            MobileGL::SharedPtr<Twin>& slot = table.GetOrCreate(handle);
            slot = MobileGL::MakeShared<Twin>();
            slot->SyncedSerial = stamp;
            return slot.get();
        }
    };

    // THE DEFECT THIS SLICE EXISTS FOR, in its first direction. Two contexts of one session that
    // do NOT share a group have independent name spaces, so the same handle value in both is
    // legal and means two different objects. One process-wide table answered both handles with
    // one twin, and the second context was handed the first one's driver ids.
    TEST_F(TwinOwnershipTest, TheSameHandleInTwoNonsharedGroupsIsTwoTwins) {
        const P::MGPipeHandle handle{7u, 1u};
        RealTable& table = GL::SamplerViewImpl::g_backendSamplerViews;

        BindKey(kSessionA, kGroupOne);
        EXPECT_EQ(table.FindByHandle(handle), nullptr) << "the group began with a twin it never built";
        Twin* const first = Mint(table, handle, 11u);
        ASSERT_NE(first, nullptr);

        BindKey(kSessionA, kGroupTwo);
        EXPECT_EQ(table.FindByHandle(handle), nullptr)
            << "a context of a group the object was NOT made in resolved the other group's twin - the "
               "same {slot, gen} in two name spaces is two objects, and this answers one";
        Twin* const second = Mint(table, handle, 22u);
        ASSERT_NE(second, nullptr);
        EXPECT_NE(first, second) << "two groups that do not share were given one twin";

        // AND THE FIRST GROUP'S OBJECT IS UNTOUCHED - the direction the old shape fails hardest,
        // because the second create overwrote the first twin's contents in place.
        BindKey(kSessionA, kGroupOne);
        const MobileGL::SharedPtr<Twin>* const back = table.FindByHandle(handle);
        ASSERT_NE(back, nullptr) << "returning to the first group lost its twin";
        EXPECT_EQ(back->get(), first) << "a twin was minted for a group that already had one";
        EXPECT_EQ((*back)->SyncedSerial, 11u)
            << "the other group's sync rewrote this group's twin - the two contexts were one name space";
        EXPECT_EQ(table.BucketCount(), 2u) << "two groups resolved to one bucket";
    }

    // THE SECOND DIRECTION, and the half a literal per-context keying would break: two contexts
    // that DO share a group must see ONE set of objects. That is what makes
    // `eglCreateContext(share = ...)` mean anything at the driver.
    TEST_F(TwinOwnershipTest, TwoContextsOfOneGroupShareTheGroupsTwins) {
        const P::MGPipeHandle handle{9u, 1u};
        RealTable& table = GL::SamplerViewImpl::g_backendSamplerViews;

        BindKey(kSessionA, kGroupOne);
        Twin* const first = Mint(table, handle, 33u);

        // The same group, reached by the sibling context. Same key, therefore same bucket: the
        // resolver's group half is what decides, not the context that happens to be current.
        BindKey(kSessionA, kGroupOne);
        const MobileGL::SharedPtr<Twin>* const found = table.FindByHandle(handle);
        ASSERT_NE(found, nullptr)
            << "a sharing context cannot see the object its group already has - driver-level sharing "
               "is impossible without this";
        EXPECT_EQ(found->get(), first) << "the sharing context was given a twin of its own";
        EXPECT_EQ((*found)->SyncedSerial, 33u) << "the sharing context resolved an empty object";
        EXPECT_EQ(table.BucketCount(), 1u) << "one share group was split into two buckets";
    }

    // A SHARE GROUP DOES NOT CROSS A SESSION. Two served sessions are two connections and no
    // driver id crosses between them, so the same group token and the same handle in both must
    // be two objects - and a session's teardown must take ITS twins and no one else's.
    TEST_F(TwinOwnershipTest, ASessionDropTakesOnlyThatSessionsTwins) {
        const P::MGPipeHandle handle{5u, 1u};
        RealTable& table = GL::SamplerViewImpl::g_backendSamplerViews;

        BindKey(kSessionA, kGroupOne);
        Twin* const inA = Mint(table, handle, 44u);
        BindKey(kSessionB, kGroupOne);
        EXPECT_EQ(table.FindByHandle(handle), nullptr)
            << "session B resolved the twin session A built under the same names";
        Twin* const inB = Mint(table, handle, 55u);
        EXPECT_NE(inA, inB);

        // This is what DropEveryTwinForEndedServerSession now does, and the reason it is scoped:
        // it runs on the apply thread of the session that is ENDING, while a neighbour session
        // may still be rendering in this process (P14 S2).
        EXPECT_EQ(table.DropBucketsOfSession(kSessionA), 1u) << "the ended session's group was not dropped";
        BindKey(kSessionA, kGroupOne);
        EXPECT_EQ(table.FindByHandle(handle), nullptr)
            << "the ended session still resolves a twin it built";
        BindKey(kSessionB, kGroupOne);
        const MobileGL::SharedPtr<Twin>* const survivor = table.FindByHandle(handle);
        ASSERT_NE(survivor, nullptr)
            << "ending one session dropped another session's twin - the process-wide drop this slice "
               "replaces would have done exactly that";
        EXPECT_EQ(survivor->get(), inB);
    }

    // THE PROCESS-WIDE TABLE IS STILL THE ANSWER FOR EVERY THREAD THAT BELONGS TO NO SESSION,
    // and a session's twins must not leak into it or out of it. This is the property the whole
    // single-context world - the monolith, the client process, every unit case - depends on.
    TEST_F(TwinOwnershipTest, AThreadWithNoSessionStillGetsTheProcessWideTwinTable) {
        const P::MGPipeHandle handle{3u, 1u};
        RealTable& table = GL::SamplerViewImpl::g_backendSamplerViews;

        t_sessionKey = 0;
        t_shareGroupKey = 0;
        GL::TwinKey key;
        EXPECT_FALSE(GL::CurrentTwinKey(&key)) << "the resolver claimed a thread that is no session's";
        EXPECT_EQ(key.SessionKey, 0u);
        EXPECT_EQ(key.ShareGroupKey, 0u);
        Twin* const processWide = Mint(table, handle, 66u);

        BindKey(kSessionA, kGroupOne);
        EXPECT_EQ(table.FindByHandle(handle), nullptr)
            << "a served context resolved the process-wide twin";
        EXPECT_NE(Mint(table, handle, 77u), processWide);

        t_sessionKey = 0;
        const MobileGL::SharedPtr<Twin>* const back = table.FindByHandle(handle);
        ASSERT_NE(back, nullptr);
        EXPECT_EQ(back->get(), processWide) << "the process-wide bucket followed the session's";
    }

    // THE SIX REGISTRIES ARE THE SAME SHAPE. The cases above drive the buffer/sampler-view
    // table, which IS a SlotTableRegistry; the six TwinRegistry registries reach the same
    // resolution through their own forwarding members, and this is what pins that they do -
    // including the generation gate, which must still refuse a handle from a slot's past.
    TEST_F(TwinOwnershipTest, TheSixTwinRegistriesResolvePerGroupToo) {
        RegistryShapedTable table;
        const P::MGPipeHandle handle{21u, 1u};

        BindKey(kSessionA, kGroupOne);
        MobileGL::SharedPtr<Twin>* slot = table.GetOrCreateByHandle(handle);
        ASSERT_NE(slot, nullptr);
        *slot = MobileGL::MakeShared<Twin>();
        (*slot)->SyncedSerial = 88u;
        Twin* const first = slot->get();
        ASSERT_EQ(table.LiveGenAt(handle.Slot), handle.Gen);

        BindKey(kSessionA, kGroupTwo);
        EXPECT_EQ(table.FindByHandle(handle), nullptr)
            << "the six-registry shape is still one process-wide table";
        EXPECT_EQ(table.LiveGenAt(handle.Slot), 0u) << "the other group's slot reads as live";
        MobileGL::SharedPtr<Twin>* const other = table.GetOrCreateByHandle(handle);
        ASSERT_NE(other, nullptr);
        *other = MobileGL::MakeShared<Twin>();
        (*other)->SyncedSerial = 99u;
        EXPECT_NE(other->get(), first);
        EXPECT_EQ(table.BucketCount(), 2u);

        BindKey(kSessionA, kGroupOne);
        MobileGL::SharedPtr<Twin>* const back = table.FindByHandle(handle);
        ASSERT_NE(back, nullptr);
        EXPECT_EQ(back->get(), first);
        EXPECT_EQ((*back)->SyncedSerial, 88u);

        // The generation gate is per bucket, because a bucket is a name space: a handle behind
        // THIS group's live generation is still refused.
        const P::MGPipeHandle stale{handle.Slot, handle.Gen - 1u};
        EXPECT_EQ(table.FindByHandle(stale), nullptr) << "a stale generation resolved a live twin";

        EXPECT_EQ(table.DropBucketsOfSession(kSessionA), 2u);
        EXPECT_EQ(table.BucketCount(), 0u);
    }

    // AN OBJECT'S DEATH IS ITS OWN SESSION'S. Every client numbers its objects with its own
    // allocator, so the same {slot, gen} is live in other sessions at the same time and names
    // their objects. The object_death release used to walk every holder of the kind: a client
    // deleting a texture released the compositor's twin with the same handle - its driver
    // texture was deleted while it was still drawn from (the lock screen exiting turned the
    // desktop's windows to noise).
    TEST_F(TwinOwnershipTest, AnObjectDeathReleasesOnlyItsOwnSessionsTwins) {
        RegistryShapedTable table;
        const P::MGPipeHandle handle{23u, 1u};
        const auto mint = [&](Uint64 session, Uint64 group, Uint64 stamp) {
            BindKey(session, group);
            MobileGL::SharedPtr<Twin>* slot = table.GetOrCreateByHandle(handle);
            ASSERT_NE(slot, nullptr);
            *slot = MobileGL::MakeShared<Twin>();
            (*slot)->SyncedSerial = stamp;
        };
        mint(kSessionA, kGroupOne, 1u);
        mint(kSessionA, kGroupTwo, 2u);
        mint(kSessionB, kGroupOne, 3u);

        // Session A's client deleted its object; the death is applied on A's thread.
        BindKey(kSessionA, kGroupOne);
        EXPECT_TRUE(RegistryShapedTable::ReleaseByHandle(handle));
        EXPECT_EQ(table.FindByHandle(handle), nullptr) << "the dying object's twin survived its death";
        // A handle is unique across one client's share groups, so the session's other group lets go too.
        BindKey(kSessionA, kGroupTwo);
        EXPECT_EQ(table.FindByHandle(handle), nullptr);

        BindKey(kSessionB, kGroupOne);
        const MobileGL::SharedPtr<Twin>* const survivor = table.FindByHandle(handle);
        ASSERT_NE(survivor, nullptr) << "another session's live object was released by this session's death";
        EXPECT_EQ((*survivor)->SyncedSerial, 3u);

        // A thread that belongs to no session (the in-library backend) keeps the process-wide walk.
        BindKey(0, 0);
        EXPECT_TRUE(RegistryShapedTable::ReleaseByHandle(handle));
        BindKey(kSessionB, kGroupOne);
        EXPECT_EQ(table.FindByHandle(handle), nullptr);

        table.DropBucketsOfSession(kSessionA);
        table.DropBucketsOfSession(kSessionB);
    }

} // namespace
