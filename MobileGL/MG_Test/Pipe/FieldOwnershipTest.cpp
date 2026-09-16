// MobileGL - MobileGL/MG_Test/Pipe/FieldOwnershipTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// TABLE 2 at runtime (CONTRACT-P5.md section 3, BRIEF-P5 R-7, exit gate E4). Package p1.
//
// The generated table is checked two ways here, and they answer different questions:
//
//   the ARITHMETIC cases run in any push build and say the table PARTITIONS the field set -
//   70 rows, four classes, every BARRIER-PULLED row naming the phase that retires it;
//
//   the BEHAVIOUR cases run only in a split build and say the table is LOAD-BEARING - that a
//   server verb stamp makes the record-supplied fields readable and withdraws the rest, that a
//   BARRIER-PULLED read is counted rather than fatal, that MOBILEGL_IPC_STRICT_ERRORS=1 turns
//   it into a named abort, and that the seven sticky forwards' poison exemption is cancelled.
//
// E4's negative control is ARecordSuppliedFieldIsReadableAfterAServerStamp: move one field
// from RECORD-SUPPLIED to FATAL in MG_Pipe/FieldOwnership.def and that case goes red by name.
// The generator's own controls are `gen_pipe_field_ownership.py --self-test`.
//
// Links gtest rather than gtest_main and carries its own main(), like PipeInputsTest: the
// abort cases read the Fatal line back out of a log file this process names before anything
// logs.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>

#if MOBILEGL_PIPE_PUSH
#include <Config.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Metrics/PipeStats.h>
#endif

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

#if MOBILEGL_PIPE_PUSH
    using GLContext = MG_State::GLState::GLContext;

    // A live frontend context, restored on the way out so the cases stay independent - the
    // sticky forwards reach for one and the stamp cases must not depend on whether they found
    // it (PipeInputsTest's idiom).
    class FieldOwnershipTest : public ::testing::Test {
    protected:
        void SetUp() override {
            m_previous = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
#if MOBILEGL_BUILD_DISAGGREGATED
            MG_Config::Ipc.StrictErrors = false;
            MGPipeServerClearVerbBoundary();
            MGPipeResetResidualPullCountForTesting();
#endif
        }
        void TearDown() override {
#if MOBILEGL_BUILD_DISAGGREGATED
            MGPipeServerClearVerbBoundary();
            MG_Config::Ipc.StrictErrors = false;
#endif
            MG_State::pGLContext = Move(m_previous);
        }
        UniquePtr<GLContext> m_previous;
    };

    SizeT Index(MGPipeInputField field) { return static_cast<SizeT>(field); }

#if MGTEST_HAVE_FORK
    struct ChildResult {
        int Status = -1;
        std::string Log;
    };

    // Runs `body` in a forked child and returns its wait status and log delta. The child must
    // not use gtest assertions; it _exit(0)s when `body` returns, so a body expected to die is
    // asserted dead by the parent rather than assumed dead.
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

    Bool DiedOfAbort(const ChildResult& r) { return WIFSIGNALED(r.Status) && WTERMSIG(r.Status) == SIGABRT; }
    Bool ExitedWith(const ChildResult& r, int code) { return WIFEXITED(r.Status) && WEXITSTATUS(r.Status) == code; }
    std::string DescribeStatus(const ChildResult& r) {
        if (r.Status < 0) return "fork/waitpid failed";
        if (WIFEXITED(r.Status)) return "exited " + std::to_string(WEXITSTATUS(r.Status));
        if (WIFSIGNALED(r.Status)) return "signal " + std::to_string(WTERMSIG(r.Status));
        return "status " + std::to_string(r.Status);
    }
#endif // MGTEST_HAVE_FORK
#endif // MOBILEGL_PIPE_PUSH
} // namespace

#if !MOBILEGL_PIPE_PUSH

TEST(FieldOwnershipTest, EveryFieldAndForwardIsInExactlyOneClass) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(FieldOwnershipTest, TheClassSizesPartitionTheFieldSet) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(FieldOwnershipTest, EveryBarrierPulledRowNamesTheRetiringPhase) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(FieldOwnershipTest, TheReducedPathsUnmigratedFieldsAreAllAccountedFor) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(FieldOwnershipTest, TheSevenStickyForwardsAgreeWithTheirFieldRows) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(FieldOwnershipTest, VerbBoundaryOpsCoverEveryVerbShapedCall) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}

#else // MOBILEGL_PIPE_PUSH

// ---------------------------------------------------------------------------------------
// The arithmetic: the table partitions the field set. Runs in push, verify and split.
// ---------------------------------------------------------------------------------------

TEST_F(FieldOwnershipTest, EveryFieldAndForwardIsInExactlyOneClass) {
    for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
        EXPECT_NE(kMGPipeFieldOwnership[i], MGPipeFieldOwnership::kUnclassified)
            << kMGPipeInputFieldNames[i] << " is in none of the four ownership classes";
    }
    for (SizeT i = 0; i < kMGPipeFieldOwnershipForwardCount; ++i) {
        EXPECT_NE(kMGPipeFieldOwnershipForward[i], MGPipeFieldOwnership::kUnclassified)
            << "sticky forward " << i << " is in none of the four ownership classes";
    }
    EXPECT_EQ(kMGPipeFieldOwnershipRowCount, SizeT{70});
}

TEST_F(FieldOwnershipTest, TheClassSizesPartitionTheFieldSet) {
    SizeT counted[5] = {};
    for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
        ++counted[static_cast<SizeT>(kMGPipeFieldOwnership[i])];
    }
    EXPECT_EQ(counted[static_cast<SizeT>(MGPipeFieldOwnership::kRecordSupplied)],
              kMGPipeRecordSuppliedFieldCount);
    EXPECT_EQ(counted[static_cast<SizeT>(MGPipeFieldOwnership::kApplierDerived)],
              kMGPipeApplierDerivedFieldCount);
    EXPECT_EQ(counted[static_cast<SizeT>(MGPipeFieldOwnership::kBarrierPulled)],
              kMGPipeBarrierPulledFieldCount);
    EXPECT_EQ(counted[static_cast<SizeT>(MGPipeFieldOwnership::kFatal)], kMGPipeFatalFieldCount);
    // The census's own arithmetic (scout-unmigrated-census section 2.1): 31 of the 63 fields
    // are served by NO pushed record - 24 non-sticky plus the seven sticky - so 32 are.
    EXPECT_EQ(kMGPipeRecordSuppliedFieldCount, SizeT{32});
    EXPECT_EQ(kMGPipeApplierDerivedFieldCount + kMGPipeBarrierPulledFieldCount + kMGPipeFatalFieldCount,
              SizeT{31});
}

TEST_F(FieldOwnershipTest, EveryBarrierPulledRowNamesTheRetiringPhase) {
    SizeT pulled = 0;
    for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
        const Bool isPulled = kMGPipeFieldOwnership[i] == MGPipeFieldOwnership::kBarrierPulled;
        const Bool named = std::string(kMGPipeFieldRetiringPhase[i]) != "-";
        EXPECT_EQ(isPulled, named) << kMGPipeInputFieldNames[i]
                                   << ": only a BARRIER-PULLED row has a retiring phase, and it must have one";
        pulled += isPulled ? 1 : 0;
    }
    EXPECT_EQ(pulled, kMGPipeBarrierPulledFieldCount);
}

// The 21 the reduced path actually reads (scout-unmigrated-census section 3: the union of
// kClear's 7, kDraw's 19 and kReadback's 12). Twenty of them are BARRIER-PULLED; the
// twenty-first is GetPixelStoreParameters, whose PACK half the applier writes and whose UNPACK
// half has no carrier and no backend reader at all.
TEST_F(FieldOwnershipTest, TheReducedPathsUnmigratedFieldsAreAllAccountedFor) {
    const MGPipeInputField pulled[] = {
        MGPipeInputField::GetActiveTextureUnit,
        MGPipeInputField::GetBoundVertexArray,
        MGPipeInputField::GetBufferBindingSlot,
        MGPipeInputField::GetBufferBindingPoint,
        MGPipeInputField::GetTouchedBufferBindingPointCount,
        MGPipeInputField::GetCurrentVertexAttribute,
        MGPipeInputField::GetFramebufferBindingSlot,
        MGPipeInputField::GetImageTextureBinding,
        MGPipeInputField::GetMaxTouchedTextureUnit,
        MGPipeInputField::GetProgramForDraw,
        MGPipeInputField::GetSamplingResolutionGeneration,
        MGPipeInputField::GetTextureBindGeneration,
        MGPipeInputField::GetTextureContextId,
        MGPipeInputField::GetTextureUnitObject,
        MGPipeInputField::GetTransformFeedbackCapturedVertices,
        MGPipeInputField::GetTransformFeedbackGeneration,
        MGPipeInputField::GetTransformFeedbackProgram,
        MGPipeInputField::GetBoundTransformFeedbackLifetimeId,
        MGPipeInputField::IsTransformFeedbackActive,
        MGPipeInputField::IsTransformFeedbackPaused,
    };
    for (const auto field : pulled) {
        EXPECT_EQ(MGPipeFieldOwnershipOf(field), MGPipeFieldOwnership::kBarrierPulled)
            << kMGPipeInputFieldNames[Index(field)] << " left the reduced path's debt";
    }
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetPixelStoreParameters),
              MGPipeFieldOwnership::kApplierDerived);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetPixelStoreParameters, 0u),
              MGPipeFieldOwnership::kApplierDerived);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetPixelStoreParameters, 1u),
              MGPipeFieldOwnership::kFatal);

    // The three off the reduced path, each for its own checkable reason.
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetBoundTransformFeedbackName),
              MGPipeFieldOwnership::kFatal);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetTransformFeedbackPausedPrimitiveCounter),
              MGPipeFieldOwnership::kFatal);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetProgramForDispatch),
              MGPipeFieldOwnership::kFatal);
}

TEST_F(FieldOwnershipTest, TheSevenStickyForwardsAgreeWithTheirFieldRows) {
    ASSERT_EQ(kMGPipeFieldOwnershipForwardCount, kMGPipeInputStickyFieldCount);
    for (SizeT i = 0; i < kMGPipeFieldOwnershipForwardCount; ++i) {
        const MGPipeInputField field = kMGPipeFieldOwnershipForwardField[i];
        EXPECT_TRUE(kMGPipeInputFieldSticky[Index(field)])
            << kMGPipeInputFieldNames[Index(field)] << " has a forward row but is not sticky";
        EXPECT_EQ(kMGPipeFieldOwnershipForward[i], MGPipeFieldOwnershipOf(field));
        EXPECT_STRNE(kMGPipeFieldOwnershipForwardMechanism[i], "");
    }
}

// The stamp map, in both directions: the four P5 class-B boundaries, the eight mapped ahead of
// the phase that will emit them, and the three verb-shaped calls that are exempt by name.
TEST_F(FieldOwnershipTest, VerbBoundaryOpsCoverEveryVerbShapedCall) {
    // CONTRACT §7 class B minus Present - the only four that can arrive in P5.
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::Clear), MGPipeVerb::Clear);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::DrawVbo), MGPipeVerb::DrawArrays);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::ReadPixels), MGPipeVerb::ReadPixels);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::Blit), MGPipeVerb::BlitFramebuffer);
    // Class C today, mapped anyway: an OMITTED stamp row is silent, because the record would
    // apply under the previous verb's serial, mask and name.
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::LaunchGrid), MGPipeVerb::DispatchCompute);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::MemoryBarrier), MGPipeVerb::MemoryBarrier);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::BeginStreamOutput), MGPipeVerb::BeginTransformFeedback);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::EndStreamOutput), MGPipeVerb::EndTransformFeedback);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::PauseStreamOutput), MGPipeVerb::PauseTransformFeedback);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::ResumeStreamOutput), MGPipeVerb::ResumeTransformFeedback);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::GenerateMipmap), MGPipeVerb::GenerateMipmap);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::GetTextureImage), MGPipeVerb::GetTextureImage);
    // P5b (MG_Remote/CONTRACT-P5B.md): the renamed boundary the file left to "the phase that
    // emits it" - resource_copy_region is glCopyImageSubData only - and the five appended verbs,
    // each stamped as the GLFunctionsTable verb it reproduces. copy_framebuffer_to_texture also
    // carries glCopyTexSubImage2D and stamps CopyTexImage2D for both: one kBlitOrCopy mask.
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::ResourceCopyRegion), MGPipeVerb::CopyImageSubData);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::BindShaderImage), MGPipeVerb::BindImageTexture);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::PatchParameter), MGPipeVerb::PatchParameteri);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::BindStreamOutput), MGPipeVerb::BindTransformFeedback);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::SetStorageBlockBinding),
              MGPipeVerb::ShaderStorageBlockBinding);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::CopyFramebufferToTexture), MGPipeVerb::CopyTexImage2D);
    EXPECT_EQ(kMGPipeVerbBoundaryOpCount, SizeT{18});
    EXPECT_EQ(kMGPipeVerbBoundaryExemptCount, SizeT{3});

    // Present is class B (it is emitted in P5) and is STILL not a verb boundary:
    // FillPoints.def:21 - "Present and SetSwapInterval go through BackendObject virtuals and
    // read no frontend state, so they are not verbs here". Stamping there would retire the
    // previous verb's answers with nothing to put in their place.
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::Present), MGPipeVerb::kVerbCount);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::SetSwapInterval), MGPipeVerb::kVerbCount);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::Flush), MGPipeVerb::kVerbCount);
    // ... and a record that is part of a verb rather than a boundary of one.
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::SetDynamicState), MGPipeVerb::kVerbCount);
    EXPECT_EQ(MGPipeVerbForWireOp(MGPWireOp::GetCaps), MGPipeVerb::kVerbCount);
}

// ---------------------------------------------------------------------------------------
// The behaviour: the table is load-bearing. Split builds only - it is the server verb stamp
// that arms all of it, and nothing in a monolith lane stamps.
// ---------------------------------------------------------------------------------------

#if MOBILEGL_BUILD_DISAGGREGATED

TEST_F(FieldOwnershipTest, AServerStampMakesRecordSuppliedFieldsFreshAndWithdrawsTheRest) {
    MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
    EXPECT_TRUE(gPipeInputs.ServerStampedVerb());
    EXPECT_EQ(gPipeInputs.CurrentVerb(), MGPipeVerb::Clear);
    const MGPipeFieldMask& mask = kMGPipeClassFieldMask[static_cast<SizeT>(
        kMGPipeVerbClass[static_cast<SizeT>(MGPipeVerb::Clear)])];
    SizeT stamped = 0;
    for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
        const auto field = static_cast<MGPipeInputField>(i);
        const Bool fresh = MGPipeInputFieldIsFresh(gPipeInputs.FilledState(), field);
        // The stamp respects the verb's own may-read table for the client fill's reason: a
        // field outside the class was never copied for this verb, so answering it would hand
        // the server the previous verb's value.
        const Bool answerable = MGPipeFieldMaskHas(mask, field) &&
                                (kMGPipeFieldOwnership[i] == MGPipeFieldOwnership::kRecordSupplied ||
                                 kMGPipeFieldOwnership[i] == MGPipeFieldOwnership::kApplierDerived);
        EXPECT_EQ(fresh, answerable)
            << kMGPipeInputFieldNames[i] << " is " << MGPipeFieldOwnershipName(kMGPipeFieldOwnership[i])
            << " and the stamp answered " << (fresh ? "fresh" : "stale");
        stamped += fresh ? 1 : 0;
    }
    // Not vacuous: kClear really does have record-supplied fields to stamp.
    EXPECT_GT(stamped, SizeT{0});

    // AND FOUR NAMED FIELDS, NOT DERIVED FROM THE ARRAY UNDER TEST. The loop above recomputes
    // `answerable` out of kMGPipeFieldOwnership, so it can only catch a stamp that disagrees
    // with the table - never a table that is wrong. These four say what the stamp must do for
    // four fields whose class is an argument of this package rather than a lookup.
    const MGPipeFilledState& filled = gPipeInputs.FilledState();
    EXPECT_TRUE(MGPipeInputFieldIsFresh(filled, MGPipeInputField::GetClearColor));         // supplied
    EXPECT_TRUE(MGPipeInputFieldIsFresh(filled, MGPipeInputField::GetRenderStateParameters));
    EXPECT_FALSE(MGPipeInputFieldIsFresh(filled, MGPipeInputField::GetFramebufferBindingSlot)); // pulled
    EXPECT_FALSE(MGPipeInputFieldIsFresh(filled, MGPipeInputField::RecordError));           // sticky
}

// THE STICKY EXEMPTION, CANCELLED. generated/PipeFilled.inc answers "fresh" for a sticky field
// whatever the serial says - but it tests "never filled" FIRST, so the stamp's withdrawal
// (FilledGen = 0) wins over kMGPipeInputFieldSticky without a line of the generated file
// changing. Before this, the seven most dangerous fields were exempt by construction.
TEST_F(FieldOwnershipTest, TheStickyExemptionIsCancelledByTheServerStamp) {
    MGPipeValidateForVerb(MGPipeVerb::Clear); // the CLIENT fills and stamps all 63, sticky included
    EXPECT_TRUE(MGPipeInputFieldIsFresh(gPipeInputs.FilledState(), MGPipeInputField::RecordError));
    MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
    for (SizeT i = 0; i < kMGPipeFieldOwnershipForwardCount; ++i) {
        const MGPipeInputField field = kMGPipeFieldOwnershipForwardField[i];
        EXPECT_FALSE(MGPipeInputFieldIsFresh(gPipeInputs.FilledState(), field))
            << kMGPipeInputFieldNames[Index(field)] << " is still exempt under split";
    }
}

// E4's NEGATIVE CONTROL. Move one field from RECORD-SUPPLIED to FATAL in FieldOwnership.def
// and this case goes red by name: the read below aborts instead of completing.
TEST_F(FieldOwnershipTest, ARecordSuppliedFieldIsReadableAfterAServerStamp) {
    ASSERT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetClearColor),
              MGPipeFieldOwnership::kRecordSupplied);
    MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
    (void)gPipeInputs.GetClearColor();
    (void)gPipeInputs.GetRenderStateParameters();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{0});
    // The pixel store is in kReadback's class, not kClear's, so its readable half is exercised
    // under the verb that actually reads it.
    MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
    (void)gPipeInputs.GetPixelStoreParameters(false); // the half that has a carrier
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{0});
}

TEST_F(FieldOwnershipTest, ABarrierPulledReadAfterAServerStampIsCountedNotFatal) {
    MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
    ASSERT_EQ(MGPipeResidualPullCount(), Uint64{0});
    (void)gPipeInputs.GetActiveTextureUnit();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{1});
    (void)gPipeInputs.GetTextureContextId();
    (void)gPipeInputs.GetMaxTouchedTextureUnit();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{3});
}

// The seven carry no MGP_INPUT_CHECK, so freshness can never reach them; this is the only
// thing that puts them in `rsp`.
TEST_F(FieldOwnershipTest, AStickyForwardIsCountedAsAResidualPull) {
    MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
    ASSERT_EQ(MGPipeResidualPullCount(), Uint64{0});
    (void)gPipeInputs.ValidateProgramName(1u);
    (void)gPipeInputs.GetProgramObject(1u);
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{2});
}

// ... and outside a server-stamped verb they are ordinary monolith calls, which is what keeps
// InvalidateCompileEnv reachable from backend initialisation - the case the exemption exists
// for - and what keeps build-split's monolith lanes behaving as a verify build's do.
TEST_F(FieldOwnershipTest, NothingIsCountedOutsideAServerStampedVerb) {
    MGPipeValidateForVerb(MGPipeVerb::ReadPixels);
    (void)gPipeInputs.ValidateProgramName(1u);
    gPipeInputs.InvalidateCompileEnv();
    (void)gPipeInputs.GetActiveTextureUnit();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{0});
    EXPECT_FALSE(gPipeInputs.ServerStampedVerb());
}

// THE APPLIER'S OWN CLEAR, reached directly rather than through the client's fill. In a spawned
// server MG_Impl is not in the process, so MGPipeValidateForVerb/MGPipeLeaveVerb never run and
// MGPipeServerClearVerbBoundary is the ONLY thing that can disarm the flag; without it the
// server latches TRUE after its first stamp and the sticky exemption - the one
// InvalidateCompileEnv is reached from backend initialisation under - is gone for good.
TEST_F(FieldOwnershipTest, TheAppliersOwnClearDisarmsTheStampWithoutTheClientsFill) {
    MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
    ASSERT_TRUE(gPipeInputs.ServerStampedVerb());
    (void)gPipeInputs.ValidateProgramName(1u);
    ASSERT_EQ(MGPipeResidualPullCount(), Uint64{1});

    MGPipeServerClearVerbBoundary(); // what PipeApplier must call on leaving the applier
    EXPECT_FALSE(gPipeInputs.ServerStampedVerb());
    (void)gPipeInputs.ValidateProgramName(1u);
    gPipeInputs.InvalidateCompileEnv();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{1}) << "a forward outside a stamped verb was counted";
}

// The verb's own may-read table still holds on the server: kClear does not read
// GetActiveTextureUnit, so reading it there is a stale answer rather than a residual pull,
// and it stays Fatal. Counting it would trade a loud staleness for a quiet one.
TEST_F(FieldOwnershipTest, ABarrierPulledFieldOutsideTheVerbsClassIsStillFatal) {
#if MGTEST_HAVE_FORK
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
        (void)gPipeInputs.GetActiveTextureUnit(); // BARRIER-PULLED, but not in kClear's class
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetActiveTextureUnit@Clear\"}"), std::string::npos)
        << r.Log;
    EXPECT_EQ(r.Log.find("BARRIER-PULLED"), std::string::npos) << r.Log;
#else
    GTEST_SKIP() << "needs fork";
#endif
}

TEST_F(FieldOwnershipTest, ResidualPullsReachThePublishedPerFrameCounter) {
    namespace PS = MG_Util::PipeStats;
    PS::SetEnabledForTesting(true);
    PS::ResetForTesting();
    MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
    (void)gPipeInputs.GetActiveTextureUnit();
    EXPECT_EQ(PS::FrameCalls(PS::CallClass::ResidualPulls), Uint64{1});
    EXPECT_NE(PS::FormatWindowLine().find("rsp="), std::string::npos) << PS::FormatWindowLine();
    PS::ResetForTesting();
    PS::SetEnabledForTesting(false);
}

#if MGTEST_HAVE_FORK

// R-7.3's proof that the instrumentation can go red. An instrumentation that cannot is
// decoration, and the set it counts is not empty.
TEST_F(FieldOwnershipTest, StrictErrorsTurnsABarrierPulledReadIntoANamedAbort) {
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
        (void)gPipeInputs.GetActiveTextureUnit();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetActiveTextureUnit@ReadPixels\"}"),
              std::string::npos)
        << r.Log;
    EXPECT_NE(r.Log.find("BARRIER-PULLED"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("MOBILEGL_IPC_STRICT_ERRORS=1"), std::string::npos) << r.Log;
    // The strict line names the phase that owes the answer; a strict abort that did not would
    // leave the reader exactly where the gate found them.
    EXPECT_NE(r.Log.find("retires in P3b/P4b"), std::string::npos) << r.Log;
}

TEST_F(FieldOwnershipTest, TheSameReadWithoutStrictErrorsSurvivesAndIsCounted) {
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
        (void)gPipeInputs.GetActiveTextureUnit();
        if (MGPipeResidualPullCount() != 1) ::_exit(7);
    });
    ASSERT_TRUE(ExitedWith(r, 0)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{"), std::string::npos) << r.Log;
}

TEST_F(FieldOwnershipTest, StrictErrorsAlsoPromotesTheStickyForwards) {
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.ValidateProgramName(1u);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"ValidateProgramName@DrawArrays\"}"),
              std::string::npos)
        << r.Log;
}

// A FATAL-class read aborts whatever the knob says: no carrier, and the reduced path never
// reads it, so it is a real defect rather than a debt.
TEST_F(FieldOwnershipTest, AFatalClassReadAbortsEvenWithoutStrictErrors) {
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetProgramForDispatch();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetProgramForDispatch@DrawArrays\"}"),
              std::string::npos)
        << r.Log;
}

// The argument-keyed row: the pack half is answerable and the unpack half is not, and the
// difference is the accessor's own argument rather than a second field id. Splitting the field
// into two ids would have made the unpack half BARRIER-PULLED - silently served - which is
// weaker than this.
TEST_F(FieldOwnershipTest, TheUnpackHalfOfThePixelStoreAbortsWhileThePackHalfDoesNot) {
    const ChildResult fatal = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
        (void)gPipeInputs.GetPixelStoreParameters(true);
    });
    ASSERT_TRUE(DiedOfAbort(fatal)) << DescribeStatus(fatal) << "\n" << fatal.Log;
    EXPECT_NE(fatal.Log.find("Fatal{UnmigratedPipeInput, \"GetPixelStoreParameters@ReadPixels\"}"),
              std::string::npos)
        << fatal.Log;
    // The line must say WHICH HALF. Without this the message is byte-identical to what a
    // genuinely stale read of the pack half would print, and the whole case for narrowing by
    // argument instead of by a second field id is that the reader is told which half they
    // asked for.
    EXPECT_NE(fatal.Log.find("argument 0 = 1 is FATAL while the field is APPLIER-DERIVED"),
              std::string::npos)
        << fatal.Log;

    const ChildResult ok = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
        (void)gPipeInputs.GetPixelStoreParameters(false);
        if (MGPipeResidualPullCount() != 0) ::_exit(7);
    });
    ASSERT_TRUE(ExitedWith(ok, 0)) << DescribeStatus(ok) << "\n" << ok.Log;
    EXPECT_EQ(ok.Log.find("Fatal{"), std::string::npos) << ok.Log;
}

// One function edit, five call sites (Managers.cpp:5334, DirectGLES.cpp:8051, :8702, :8997,
// :10623). The arm is the TRANSPORT, not the build: build-split's own lanes run monolith and
// several of the five are on ordinary monolith paths they exercise.
TEST_F(FieldOwnershipTest, AnUnmigratedEmulationIsFatalUnderARealTransportAndInertUnderMonolith) {
    const ChildResult monolith = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::Monolith;
        MGPipeUnmigratedEmulation("get-tex-image-shadow");
    });
    ASSERT_TRUE(ExitedWith(monolith, 0)) << DescribeStatus(monolith) << "\n" << monolith.Log;
    EXPECT_EQ(monolith.Log.find("Fatal{"), std::string::npos) << monolith.Log;

    const ChildResult split = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MGPipeUnmigratedEmulation("get-tex-image-shadow");
    });
    ASSERT_TRUE(DiedOfAbort(split)) << DescribeStatus(split) << "\n" << split.Log;
    EXPECT_NE(split.Log.find("Fatal{UnmigratedEmulation, \"get-tex-image-shadow\"}"), std::string::npos)
        << split.Log;
}

#endif // MGTEST_HAVE_FORK
#endif // MOBILEGL_BUILD_DISAGGREGATED
#endif // MOBILEGL_PIPE_PUSH

int main(int argc, char** argv) {
    // Before anything logs: MG_Util::Debug::InitFile() reads the variable once, on the first
    // write, and caches the FILE*. PipeInputsTest's idiom, and for its reason - the abort cases
    // read their Fatal line back out of this file.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-fieldownership-test-" + std::to_string(ProcessId()) + ".log");
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
