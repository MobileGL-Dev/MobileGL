// MobileGL - MobileGL/MG_Test/State/TransformFeedbackLifetimeIdTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// D21 (plan B v2 §4.7.3): DirectVulkan hands every transform feedback object one of sixteen
// counter-buffer groups, and the group carries that span's resume offset. The map was keyed on
// the GL NAME, which glGenTransformFeedbacks recycles the moment the object is deleted, so an
// object created on a recycled name was served the DEAD object's group together with its
// m_xfbCountersValid / m_xfbLastSeenGeneration entries.
//
// The transform feedback object is a plain struct inside a map rather than a heap object, so the
// reuse to defend against is the NAME's, not an address's - which is why these cases live here
// and not in ObjectLifetimeIdTest.cpp with the heap-allocated object types. Keeping them in
// their own translation unit also keeps the D21 commit textually independent of the rest of the
// branch, which plan B §10.4-5 asks for so it can be cherry-picked to dev on its own.
//
// The second case pins the OTHER half of the backend contract: a bounded slot table has to be
// able to tell an object whose span is still open (and may yet resume) from one whose span is
// closed or whose object is gone.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"

#include <MG_State/GLState/Core.h>

using namespace MobileGL;

namespace {

    MG_State::GLState::GLContext& FreshContext() {
        MobileGL::Initialize();
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        return *MG_State::pGLContext;
    }

} // namespace

TEST(TransformFeedbackLifetimeIdTest, AnObjectAtARecycledNameCarriesAFreshLifetimeId) {
    auto& context = FreshContext();

    // Before anything is bound. The default object exists from the start of the context, and the
    // identity has to exist with it: a backend reading 0 here would match every FREE slot in its
    // table without ever claiming one, which is the same bug this id was added to remove.
    EXPECT_NE(context.GetBoundTransformFeedbackLifetimeId(), 0u)
        << "the default transform feedback object has no identity until something binds it";

    Vector<Uint> names;
    context.GenTransformFeedbackNames(1, names);
    ASSERT_EQ(names.size(), 1u);
    const Uint name = names[0];
    ASSERT_NE(name, 0u);

    context.BindTransformFeedbackObject(name);
    const Uint64 firstId = context.GetBoundTransformFeedbackLifetimeId();
    EXPECT_NE(firstId, 0u) << "a live transform feedback object answered to id 0, which is the value a "
                              "zero-initialised backend slot already carries";

    // The default object is a different object and must not share the id.
    context.BindTransformFeedbackObject(0);
    EXPECT_NE(context.GetBoundTransformFeedbackLifetimeId(), firstId)
        << "the default transform feedback object shares an identity with a generated one";

    // Deleting while bound reverts to the default object (GL 4.6 core 13.2.1), which is the
    // shape the backend sees; delete from there anyway so the test does not depend on it.
    context.BindTransformFeedbackObject(name);
    context.MarkTransformFeedbackObjectForDeletion(name);

    Vector<Uint> reborn;
    context.GenTransformFeedbackNames(1, reborn);
    ASSERT_EQ(reborn.size(), 1u);
    if (reborn[0] != name) {
        GTEST_SKIP() << "inconclusive, not proven: the name generator did not hand the deleted name back, so "
                        "the recycled-name case was never exercised";
    }

    context.BindTransformFeedbackObject(reborn[0]);
    EXPECT_NE(context.GetBoundTransformFeedbackLifetimeId(), firstId)
        << "a transform feedback object created on a recycled name reports the DEAD object's lifetime id - "
           "DirectVulkan would hand it the dead span's counter slot, and with it that span's resume state";
}

// The predicate DirectVulkan's slot table asks before it takes a group over. The case that
// matters is the middle one: object A is PAUSED and another object is bound and capturing, so A
// looks completely idle to a least-recently-used rule while being exactly the object whose
// counters must survive.
TEST(TransformFeedbackLifetimeIdTest, APausedSpanStaysOpenWhileAnotherObjectCaptures) {
    auto& context = FreshContext();

    Vector<Uint> names;
    context.GenTransformFeedbackNames(2, names);
    ASSERT_EQ(names.size(), 2u);
    const Uint nameA = names[0];
    const Uint nameB = names[1];

    context.BindTransformFeedbackObject(nameA);
    const Uint64 idA = context.GetBoundTransformFeedbackLifetimeId();
    EXPECT_FALSE(context.HasOpenTransformFeedbackSpan(idA)) << "an object that never began a span reads as open";

    context.BeginTransformFeedback(GL_POINTS, nullptr);
    EXPECT_TRUE(context.HasOpenTransformFeedbackSpan(idA));

    // Pausing is what makes interleaving legal (ARB_transform_feedback2); the span is still open.
    context.SetTransformFeedbackPaused(true);
    EXPECT_TRUE(context.HasOpenTransformFeedbackSpan(idA));

    // Now the shape the slot table sees: B is bound and capturing, A is paused and untouched.
    context.BindTransformFeedbackObject(nameB);
    const Uint64 idB = context.GetBoundTransformFeedbackLifetimeId();
    EXPECT_NE(idB, idA);
    context.BeginTransformFeedback(GL_POINTS, nullptr);
    EXPECT_TRUE(context.HasOpenTransformFeedbackSpan(idA))
        << "a paused span stopped reading as open the moment another object was bound - a backend "
           "reclaiming slots by 'is this owner still going' would take A's counters away";
    EXPECT_TRUE(context.HasOpenTransformFeedbackSpan(idB));

    context.EndTransformFeedback();
    EXPECT_FALSE(context.HasOpenTransformFeedbackSpan(idB)) << "a closed span still reads as open";
    EXPECT_TRUE(context.HasOpenTransformFeedbackSpan(idA));

    // A closes its own span; its slot becomes reclaimable.
    context.BindTransformFeedbackObject(nameA);
    context.EndTransformFeedback();
    EXPECT_FALSE(context.HasOpenTransformFeedbackSpan(idA));

    // A deleted object can never resume, so its identity must not hold a slot either.
    context.BindTransformFeedbackObject(nameB);
    context.BeginTransformFeedback(GL_POINTS, nullptr);
    EXPECT_TRUE(context.HasOpenTransformFeedbackSpan(idB));
    context.MarkTransformFeedbackObjectForDeletion(nameB);
    EXPECT_FALSE(context.HasOpenTransformFeedbackSpan(idB))
        << "the identity of a deleted transform feedback object still claims an open span, so its counter "
           "group would be pinned for the life of the context";

    // Identities the context never issued, and the free-slot sentinel, are not open spans.
    EXPECT_FALSE(context.HasOpenTransformFeedbackSpan(0));
    EXPECT_FALSE(context.HasOpenTransformFeedbackSpan(~0ull));
}
