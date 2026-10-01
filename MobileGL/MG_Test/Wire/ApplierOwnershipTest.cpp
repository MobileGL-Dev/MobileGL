// MobileGL - MobileGL/MG_Test/Wire/ApplierOwnershipTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P14 S5 (docs/Disaggregated/design/11-state-ownership.md): WHOSE APPLIER IS IT?
//
// WHAT THIS SUITE OWNS. Until this slice the applier was one process-wide block
// (PipeApply.cpp's `g_applier`), and the block mixed two lifetimes: the OBJECT RECORDS that
// describe GL objects and live in a SHARE GROUP, and the WORKING STATE (bindings, the current
// program, the unit windows, the CSO store) that belongs to one CONTEXT. One block for the whole
// process made a second served context either inherit bindings it must not inherit or lose the
// object records of a share group it is a member of.
//
// This suite drives the registry and the resolution directly, with no EGL and no wire, because
// that is exactly what the slice is: `MGPipeApplier()` answers per {session, context}, its object
// records come from the {session, share group} the context belongs to, and every case below is a
// way of using two contexts that the single block got wrong.
//
// HOW IT INSTALLS A KEY. The production resolver is the server's (SessionRuntime.cpp's
// ThreadSessionScope installs it and answers with the served session's current context token).
// Here the resolver is this file's, driven by two thread_locals, so a case can be in whichever
// session and context it means without standing up a transport - and MGPipeApplierRegisterContext
// is the same entry point ServerSession::CreateContext calls. The keys the cases use are large
// sentinels that no real session address can collide with, and every case releases its sessions on
// the way out.
//
// THE OBSERVABLE IS THE APPLIER'S OWN STATE. `MGPipeApplier()` is the function the backend reads,
// so a case asserts on what the backend would see: which record a handle resolves to, whether the
// record is visible from the other context, and whether the two contexts' working states are the
// same object. Turning the keying off (making MGPipeApplier ignore the resolver) turns the
// nonshared and the two-session cases red; keying the object records by CONTEXT instead of by
// share group turns the shared case red - both directions are covered.

#include <MG_Pipe/PipeApply.h>

#include <gtest/gtest.h>

namespace {

    namespace P = MobileGL::MG_Pipe;
    using MobileGL::Bool;
    using MobileGL::Uint32;
    using MobileGL::Uint64;

    // The calling thread's {session, context}, exactly the shape the production resolver fills in.
    // A zero session key is "not a session's thread", which is what every other caller in the tree
    // is and what keeps the process-wide applier the answer there.
    thread_local Uint64 t_sessionKey = 0;
    thread_local Uint64 t_contextToken = 0;

    Bool TestApplierKeyResolver(P::MGPipeApplierKey* outKey) {
        if (t_sessionKey == 0) return false;
        outKey->SessionKey = t_sessionKey;
        outKey->ContextToken = t_contextToken;
        return true;
    }

    class ApplierOwnershipTest : public ::testing::Test {
    protected:
        // Sentinels no real session's address can be, so a stray registration elsewhere in the
        // process can never be what a case reads.
        static constexpr Uint64 kSessionA = 0x5E5510A000000000ull;
        static constexpr Uint64 kSessionB = 0x5E5510B000000000ull;

        // The share-group tokens the frontend mints (EGLState::CreateContext): dense and 0-free,
        // which is why two contexts that share pass the SAME number and two that do not pass
        // different ones.
        static constexpr Uint64 kGroupOne = 0x1001;
        static constexpr Uint64 kGroupTwo = 0x1002;

        void SetUp() override {
            P::MGPipeSetApplierKeyResolver(&TestApplierKeyResolver);
            BindSession(kSessionA, 0);
        }

        void TearDown() override {
            P::MGPipeApplierReleaseSession(kSessionA);
            P::MGPipeApplierReleaseSession(kSessionB);
            t_sessionKey = 0;
            t_contextToken = 0;
            P::MGPipeSetApplierKeyResolver(nullptr);
        }

        static void BindSession(Uint64 session, Uint64 context) {
            t_sessionKey = session;
            t_contextToken = context;
        }

        // The buffer descriptor ServerLoopTest's DeclareBuffer builds, trimmed to the fields a
        // buffer record needs. `width` is what tells two records of the SAME handle apart.
        static P::MGPResourceDesc BufferDesc(Uint32 slot, Uint32 width) {
            P::MGPResourceDesc desc{};
            desc.Resource = P::MGPipeHandle{slot, 1u};
            desc.Target = P::kMGPipeResourceTargetBuffer;
            desc.Width = width;
            desc.Height = 1;
            desc.Depth = 1;
            desc.ArrayLayers = 1;
            desc.Levels = 1;
            desc.Samples = 1;
            desc.HasDefinedContent = 1;
            return desc;
        }

        // resource_create writes the record into the OBJECT RECORDS of the applier the calling
        // thread is in - no backend, no EGL: with no resource op table registered the dispatch to
        // the backend is skipped and the record is the whole of the call's effect.
        static void DeclareBuffer(P::MGPipeHandle handle, Uint32 width) {
            ASSERT_TRUE(P::MGPipeApplyResourceCreate(BufferDesc(handle.Slot, width)))
                << "resource_create refused slot " << handle.Slot;
        }

        static const P::MGPipeResourceRecord* ResourceRecord(P::MGPipeHandle handle) {
            const P::MGPipeApplierState& state = P::MGPipeApplier();
            if (handle.Slot >= state.Resources.size()) return nullptr;
            const P::MGPipeResourceRecord& record = state.Resources[handle.Slot];
            if (!record.Live || record.Gen != handle.Gen) return nullptr;
            return &record;
        }
    };

    // THE DEFECT THIS SLICE EXISTS FOR, in its first direction. Two contexts of one session that
    // do NOT share: the frontend gives them independent name spaces, so the same handle value in
    // both is legal and means two different objects. One process-wide applier answered both
    // handles with one record, and the second create overwrote the first's descriptor - the
    // "same handle in two contexts" collision the whole keying exists to stop.
    TEST_F(ApplierOwnershipTest, TheSameHandleInTwoNonsharedContextsIsTwoRecords) {
        const P::MGPipeHandle handle{7u, 1u};
        P::MGPipeApplierRegisterContext(kSessionA, 1, kGroupOne);
        P::MGPipeApplierRegisterContext(kSessionA, 2, kGroupTwo);
        EXPECT_EQ(P::MGPipeApplierShareGroupCountForTesting(kSessionA), 2u)
            << "two contexts that name different share groups ended up in one group";

        BindSession(kSessionA, 1);
        const P::MGPipeApplierState* first = &P::MGPipeApplier();
        ASSERT_EQ(ResourceRecord(handle), nullptr) << "context 1 began with a record it never created";
        DeclareBuffer(handle, 64u);
        const P::MGPipeResourceRecord* firstRecord = ResourceRecord(handle);
        ASSERT_NE(firstRecord, nullptr) << "context 1's own create did not produce a record";
        EXPECT_EQ(firstRecord->Desc.Width, 64u);

        BindSession(kSessionA, 2);
        const P::MGPipeApplierState* second = &P::MGPipeApplier();
        // Two contexts, two appliers. One shared applier would make the two pointers equal and
        // every assertion below vacuous.
        EXPECT_NE(first, second) << "the two contexts of one session share one applier";
        EXPECT_EQ(ResourceRecord(handle), nullptr)
            << "context 2 inherited a record created in a context it does not share with";

        DeclareBuffer(handle, 1024u);
        const P::MGPipeResourceRecord* secondRecord = ResourceRecord(handle);
        ASSERT_NE(secondRecord, nullptr);
        EXPECT_EQ(secondRecord->Desc.Width, 1024u);

        // AND THE FIRST CONTEXT'S OBJECT IS UNTOUCHED. This is the assertion the old shape fails:
        // one slot, one record, so the second create rewrote the first object's descriptor.
        BindSession(kSessionA, 1);
        EXPECT_EQ(&P::MGPipeApplier(), first) << "returning to a context minted a second applier for it";
        const P::MGPipeResourceRecord* firstAgain = ResourceRecord(handle);
        ASSERT_NE(firstAgain, nullptr) << "context 1's record went missing while context 2 was current";
        EXPECT_EQ(firstAgain->Desc.Width, 64u)
            << "context 2's create on the same handle value rewrote context 1's object";

        // The reset a make-current performs clears the WORKING state and leaves the group's
        // records standing, which is the other half of the old defect: a shared object's record
        // must survive a switch away and back.
        P::MGPipeApplierReset();
        EXPECT_EQ(ResourceRecord(handle), firstAgain) << "a make-current dropped the object record";
    }

    // THE SECOND DIRECTION, and the one the S4 report named as the reason a literal "key the
    // whole applier by share group" would be worse than no change at all: two contexts that DO
    // share see one set of OBJECT RECORDS and may not see one set of bindings, a current program
    // or a CSO store. So the records must be one object and the appliers must be two.
    TEST_F(ApplierOwnershipTest, TwoContextsOfOneShareGroupShareRecordsAndNotWorkingState) {
        const P::MGPipeHandle shared{9u, 1u};
        P::MGPipeApplierRegisterContext(kSessionA, 3, kGroupOne);
        P::MGPipeApplierRegisterContext(kSessionA, 4, kGroupOne);
        EXPECT_EQ(P::MGPipeApplierShareGroupCountForTesting(kSessionA), 1u)
            << "two contexts that name the same share group were given two of them";

        BindSession(kSessionA, 3);
        P::MGPipeApplierState& third = P::MGPipeApplier();
        DeclareBuffer(shared, 64u);
        const P::MGPipeResourceRecord* record = ResourceRecord(shared);
        ASSERT_NE(record, nullptr);
        // Working state, written by hand the way an applied record would: a bound program and a
        // touched vertex-buffer window.
        third.DrawProgram = P::MGPipeHandle{11u, 1u};
        third.VertexBufferCount = 3;

        BindSession(kSessionA, 4);
        P::MGPipeApplierState& fourth = P::MGPipeApplier();
        EXPECT_NE(&third, &fourth) << "two contexts of one share group share one applier - the "
                                     "bindings, the current program and the CSO store would all "
                                     "overwrite each other";
        EXPECT_EQ(&third.Resources, &fourth.Resources)
            << "two contexts of one share group were given different object tables";
        EXPECT_NE(ResourceRecord(shared), nullptr)
            << "the sharing context cannot see the object its group already has - driver-level "
               "sharing is impossible without this";

        EXPECT_TRUE(P::MGPipeHandleIsNull(fourth.DrawProgram))
            << "the sharing context inherited the other context's current program";
        EXPECT_EQ(fourth.VertexBufferCount, 0u) << "the sharing context inherited the other context's "
                                                   "touched vertex-buffer window";
        EXPECT_FALSE(P::MGPipeHandleIsNull(third.DrawProgram)) << "the first context's working state "
                                                                  "was cleared by the second's";

        // AND A RECORD CREATED THROUGH THE SHARING CONTEXT IS THE SAME OBJECT. One table, so the
        // create is visible to both - which is what "a buffer made in one context is usable in
        // its group's other context" means at this layer.
        DeclareBuffer(shared, 4096u);
        const P::MGPipeResourceRecord* respecified = ResourceRecord(shared);
        ASSERT_NE(respecified, nullptr);
        EXPECT_EQ(respecified->Desc.Width, 4096u)
            << "a respecify through the sharing context did not reach the group's record";
    }

    // A SHARE GROUP DOES NOT CROSS A SESSION. Two sessions that name the same group token and the
    // same context token are two connections (an EGL share group is a property of one client's
    // display), so they must not resolve one applier or one object table.
    TEST_F(ApplierOwnershipTest, TwoSessionsThatNameTheSameGroupDoNotShareAnything) {
        const P::MGPipeHandle handle{5u, 1u};
        P::MGPipeApplierRegisterContext(kSessionA, 1, kGroupOne);
        P::MGPipeApplierRegisterContext(kSessionB, 1, kGroupOne);

        BindSession(kSessionA, 1);
        P::MGPipeApplierState& inA = P::MGPipeApplier();
        DeclareBuffer(handle, 32u);

        BindSession(kSessionB, 1);
        P::MGPipeApplierState& inB = P::MGPipeApplier();
        EXPECT_NE(&inA, &inB) << "two sessions with identical tokens resolved one applier";
        EXPECT_NE(&inA.Resources, &inB.Resources);
        EXPECT_EQ(ResourceRecord(handle), nullptr)
            << "session B resolved an object record session A created";
    }

    // THE GROUP'S RECORDS GO WITH ITS LAST CONTEXT, NOT ITS FIRST. A context of a live group is
    // still using them; the group's teardown is the per-group spelling of
    // MGPipeApplierReleaseObjectRecords, which the one-applier shape could only apply to the whole
    // process.
    TEST_F(ApplierOwnershipTest, AGroupKeepsItsRecordsUntilItsLastContextGoes) {
        const P::MGPipeHandle handle{13u, 1u};
        P::MGPipeApplierRegisterContext(kSessionA, 5, kGroupOne);
        P::MGPipeApplierRegisterContext(kSessionA, 6, kGroupOne);

        BindSession(kSessionA, 5);
        DeclareBuffer(handle, 128u);
        ASSERT_NE(ResourceRecord(handle), nullptr);

        P::MGPipeApplierUnregisterContext(kSessionA, 5);
        BindSession(kSessionA, 6);
        EXPECT_NE(ResourceRecord(handle), nullptr)
            << "destroying one context dropped the share group its sibling is still using";
        EXPECT_EQ(P::MGPipeApplierShareGroupCountForTesting(kSessionA), 1u);

        P::MGPipeApplierUnregisterContext(kSessionA, 6);
        EXPECT_EQ(P::MGPipeApplierShareGroupCountForTesting(kSessionA), 0u)
            << "the group outlived its last context";

        // A RECREATED CONTEXT MUST NOT INHERIT THE DEAD GROUP'S RECORDS. The frontend never
        // re-uses a token, so this is the belt: a token that comes back resolves to a fresh
        // working state in a fresh group, not to the wreckage of the old one.
        P::MGPipeApplierRegisterContext(kSessionA, 5, kGroupOne);
        BindSession(kSessionA, 5);
        EXPECT_EQ(ResourceRecord(handle), nullptr)
            << "a context registered after its group was torn down resolved the dead group's record";
    }

    // THE PROCESS-WIDE APPLIER IS STILL THE ANSWER FOR EVERY THREAD THAT BELONGS TO NO SESSION,
    // and a session's registration must not drag it into that world. This is the property the
    // ~2000 unit cases that drive the applier directly depend on.
    TEST_F(ApplierOwnershipTest, AThreadWithNoSessionStillGetsTheProcessWideApplier) {
        const P::MGPipeHandle handle{17u, 1u};
        P::MGPipeApplierRegisterContext(kSessionA, 1, kGroupOne);
        BindSession(kSessionA, 1);
        DeclareBuffer(handle, 64u);

        t_sessionKey = 0;
        t_contextToken = 0;
        P::MGPipeApplierState& processWide = P::MGPipeApplier();
        EXPECT_EQ(ResourceRecord(handle), nullptr)
            << "the process-wide applier resolved a record a session registered";

        BindSession(kSessionA, 1);
        EXPECT_NE(&P::MGPipeApplier(), &processWide) << "a session resolved the process-wide applier";
        EXPECT_NE(ResourceRecord(handle), nullptr);
    }

} // namespace
