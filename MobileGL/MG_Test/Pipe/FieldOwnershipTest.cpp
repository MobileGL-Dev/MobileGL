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

#include <cctype>
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
            // P5e (gl), ID-128: the escalation flag is per-thread state PipeApplier::ApplyOne
            // stamps on every record, so a case that set it would otherwise hand its answer to
            // the next one - and the arm it selects is "admitted", so the leak is silent.
            MGPipeApplierSetCurrentRecordBarrieredByEscalation(false);
#endif
        }
        void TearDown() override {
#if MOBILEGL_BUILD_DISAGGREGATED
            MGPipeServerClearVerbBoundary();
            MGPipeApplierSetCurrentRecordBarrieredByEscalation(false);
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
TEST(FieldOwnershipTest, TheAdmittedPullTableIsID84sDerivationAndNotAList) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(FieldOwnershipTest, TheResidualFillsSuppliedMemoReKeysOnEveryInputThatMovesAnAnswer) {
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
    // The census's own arithmetic (scout-unmigrated-census section 2.1), updated by P5c rv
    // (CONTRACT-P5C.md §5.3): rv moved the NINE value-class rows to RECORD-SUPPLIED through
    // set_context_values / the amended set_vertex_attrib_defaults and the three texture
    // shutters to APPLIER-DERIVED, so 22 of the 63 fields are served by NO pushed record -
    // 15 BARRIER-PULLED (the object class: nine non-sticky rows plus six of the seven sticky
    // forwards), four APPLIER-DERIVED and three FATAL - and 41 are.
    EXPECT_EQ(kMGPipeRecordSuppliedFieldCount, SizeT{41});
    EXPECT_EQ(kMGPipeApplierDerivedFieldCount + kMGPipeBarrierPulledFieldCount + kMGPipeFatalFieldCount,
              SizeT{22});
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
// kClear's 7, kDraw's 19 and kReadback's 12) - AS P5c rv LEFT THEM (CONTRACT-P5C.md §5.3):
// NINE moved to RECORD-SUPPLIED through set_context_values (the two texture-unit counters,
// the touched-count array, the five XFB values) plus GetCurrentVertexAttribute through the
// amended set_vertex_attrib_defaults payload, and the three texture shutters moved to
// APPLIER-DERIVED ("a shutter, not a value: the server answers from its own Serial"). What
// remains BARRIER-PULLED is EXACTLY the object class - nine non-sticky fields whose storage
// is a frontend heap reference no record can carry - plus GetPixelStoreParameters, whose
// PACK half the applier writes and whose UNPACK half has no carrier and no backend reader at
// all.
TEST_F(FieldOwnershipTest, TheReducedPathsUnmigratedFieldsAreAllAccountedFor) {
    const MGPipeInputField pulled[] = {
        MGPipeInputField::GetBoundVertexArray,
        MGPipeInputField::GetBufferBindingSlot,
        MGPipeInputField::GetBufferBindingPoint,
        MGPipeInputField::GetFramebufferBindingSlot,
        MGPipeInputField::GetImageTextureBinding,
        MGPipeInputField::GetTextureUnitObject,
        MGPipeInputField::GetProgramForDraw,
        MGPipeInputField::GetProgramForDispatch,
        MGPipeInputField::GetTransformFeedbackProgram,
    };
    for (const auto field : pulled) {
        EXPECT_EQ(MGPipeFieldOwnershipOf(field), MGPipeFieldOwnership::kBarrierPulled)
            << kMGPipeInputFieldNames[Index(field)] << " left the reduced path's debt";
    }
    // rv's exit line, pinned as a SET and not only as nine rows: the non-sticky
    // BARRIER-PULLED list above is ALL the non-sticky debt - value-class membership is zero
    // (CONTRACT-P5C.md §7 table 2) - and the only other BARRIER-PULLED rows are six of the
    // seven sticky forwards (the seventh, InvalidateCompileEnv, is FATAL since P5c ev).
    const MGPipeInputField pulledSticky[] = {
        MGPipeInputField::GetBufferBindingPointCount,
        MGPipeInputField::GetProgramObject,
        MGPipeInputField::GetTextureObject,
        MGPipeInputField::HasOpenTransformFeedbackSpan,
        MGPipeInputField::ValidateProgramName,
        MGPipeInputField::RecordError,
    };
    SizeT pulledCount = 0;
    for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
        const auto field = static_cast<MGPipeInputField>(i);
        if (kMGPipeFieldOwnership[i] != MGPipeFieldOwnership::kBarrierPulled) continue;
        ++pulledCount;
        Bool named = false;
        for (const auto f : pulled) named = named || field == f;
        for (const auto f : pulledSticky) named = named || field == f;
        EXPECT_TRUE(named) << kMGPipeInputFieldNames[i]
                           << " is BARRIER-PULLED and not in the pinned object-class list";
    }
    EXPECT_EQ(pulledCount, 15u) << "9 non-sticky object rows + 6 sticky forwards";
    EXPECT_EQ(pulledCount, kMGPipeBarrierPulledFieldCount);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetPixelStoreParameters),
              MGPipeFieldOwnership::kApplierDerived);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetPixelStoreParameters, 0u),
              MGPipeFieldOwnership::kApplierDerived);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetPixelStoreParameters, 1u),
              MGPipeFieldOwnership::kFatal);

    // The two off the reduced path, each for its own checkable reason. THREE UNTIL P5b: the
    // third was GetProgramForDispatch, FATAL because "there is no compute on the reduced path",
    // and package i1 is what put compute on the path (CONTRACT-P5B.md §6.9). It is asserted
    // below in its new class rather than deleted from this case, because a field that quietly
    // left the FATAL list is exactly what this case exists to catch.
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetBoundTransformFeedbackName),
              MGPipeFieldOwnership::kFatal);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetTransformFeedbackPausedPrimitiveCounter),
              MGPipeFieldOwnership::kFatal);
    // P5b i1: launch_grid (60) crosses, the backend's PrepareForCompute pulls the compute
    // program inside it (DirectGLES.cpp:5779), and the field takes GetProgramForDraw's class
    // and its retiring phases - so it is a measured DEBT now, not a defect.
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetProgramForDispatch),
              MGPipeFieldOwnership::kBarrierPulled);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetProgramForDispatch),
              MGPipeFieldOwnershipOf(MGPipeInputField::GetProgramForDraw))
        << "GetProgramForDispatch is GetProgramForDraw's twin and must share its class";

    // AND THE NINE rv RETIRED, asserted in their NEW classes rather than deleted: a row that
    // quietly fell back to BARRIER-PULLED is exactly what this list is for.
    const MGPipeInputField suppliedByContextValues[] = {
        MGPipeInputField::GetActiveTextureUnit,
        MGPipeInputField::GetMaxTouchedTextureUnit,
        MGPipeInputField::GetTouchedBufferBindingPointCount,
        MGPipeInputField::IsTransformFeedbackActive,
        MGPipeInputField::IsTransformFeedbackPaused,
        MGPipeInputField::GetTransformFeedbackGeneration,
        MGPipeInputField::GetBoundTransformFeedbackLifetimeId,
        MGPipeInputField::GetTransformFeedbackCapturedVertices,
    };
    for (const auto field : suppliedByContextValues) {
        EXPECT_EQ(MGPipeFieldOwnershipOf(field), MGPipeFieldOwnership::kRecordSupplied)
            << kMGPipeInputFieldNames[Index(field)] << " no longer rides set_context_values";
    }
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetCurrentVertexAttribute),
              MGPipeFieldOwnership::kRecordSupplied)
        << "the amended set_vertex_attrib_defaults payload carries all three views";
    const MGPipeInputField shutters[] = {
        MGPipeInputField::GetSamplingResolutionGeneration,
        MGPipeInputField::GetTextureBindGeneration,
        MGPipeInputField::GetTextureContextId,
    };
    for (const auto field : shutters) {
        EXPECT_EQ(MGPipeFieldOwnershipOf(field), MGPipeFieldOwnership::kApplierDerived)
            << kMGPipeInputFieldNames[Index(field)]
            << " is a shutter: the server answers from its own Serial";
    }
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
    EXPECT_EQ(kMGPipeVerbBoundaryOpCount, SizeT{23});
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

// P5e (gl), ID-116 + ID-125: THE §7 ALLOWLIST IS A DERIVATION, RESTATED HERE INDEPENDENTLY.
//
// The generator emits kMGPipeAdmittedPullMask by joining four columns; the header's own
// static_assert can only check two of them, because the wait column and the stamp map live in
// other generated files. This case walks those and asserts the rule end to end. A derivation
// that quietly dropped a term would still produce a plausible table - which is precisely how the
// hand-kept copy in test.yml came to admit GetTextureObject@CopyImageSubData while omitting
// GetFramebufferBindingSlot@ReadPixels.
//
// THE RULE (ID-125). Admitted iff the field is BARRIER-PULLED, the verb has a stamp row, the
// field is inside the verb class's may-read mask, and EITHER the verb's op is statically
// barriered OR the field's retiring phase does not name P5e. The second disjunct is the honest
// statement of ID-84: this table is consulted only on a record the server already stamped
// BARRIERED - CountBarrierPull's unbarriered arm is [[noreturn]] and fires first - so the pull
// is legal by construction and the only open question is whether THIS phase still owes the
// migration. The retiring-phase column is exactly that answer.
TEST_F(FieldOwnershipTest, TheAdmittedPullTableIsID84sDerivationAndNotAList) {
    // "Does this row's retiring phase name P5e", tokenised rather than substring-matched for
    // the generator's reason: a "P5" or a future "P5e2" must not answer for "P5e".
    const auto owedByThisPhase = [](const char* retires) {
        const std::string phase(retires == nullptr ? "" : retires);
        for (std::size_t at = phase.find("P5e"); at != std::string::npos;
             at = phase.find("P5e", at + 1)) {
            const Bool leftOk = at == 0 || !std::isalnum(static_cast<unsigned char>(phase[at - 1]));
            const std::size_t after = at + 3;
            const Bool rightOk = after >= phase.size() ||
                                 !std::isalnum(static_cast<unsigned char>(phase[after]));
            if (leftOk && rightOk) return true;
        }
        return false;
    };

    // ---- ID-125's survivors, each named with the disjunct that carries it ----------------
    // Disjunct 1, and the largest survivor in the lane (21 entries) - the row the hand-kept
    // copy omitted. The field IS this phase's debt; read_pixels is kWaitReply, so the client
    // is parked behind the record and P5C's semantics hold for it.
    EXPECT_TRUE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetFramebufferBindingSlot,
                                          MGPipeVerb::ReadPixels));
    EXPECT_TRUE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetTextureUnitObject,
                                          MGPipeVerb::CopyTexImage2D));
    // Disjunct 2: a debt a LATER phase owes, on a verb this phase does not barrier.
    // CONTRACT-P5E §5.7 rules transform feedback out of P5e entirely and this row retires in
    // P3b/P4b, so the static rule alone would have failed the lane on a row no package in this
    // phase is allowed to touch.
    EXPECT_TRUE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetTransformFeedbackProgram,
                                          MGPipeVerb::DrawArrays))
        << "the lane would be red on a transform-feedback row CONTRACT-P5E §5.7 rules out of "
           "this phase; ID-125's second disjunct is what admits it";
    EXPECT_TRUE(MGPipeBarrierPullAdmitted(MGPipeInputField::ValidateProgramName,
                                          MGPipeVerb::ShaderStorageBlockBinding));
    // BOTH DISJUNCTS, ASSERTED SEPARATELY (ID-118 and ID-125 on one pair). GetTextureObject
    // retires in P7, so disjunct 2 admits it whatever the wire says - and ID-118 ALSO moved
    // resource_copy_region to a barriered class, so disjunct 1 holds independently. Putting
    // that op back to kWaitNone must therefore leave the pair admitted and still be wrong, so
    // the wait class is asserted here in its own words rather than through the allowlist.
    EXPECT_TRUE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetTextureObject,
                                          MGPipeVerb::CopyImageSubData));
    EXPECT_NE(MGPipeWaitClassFor(MGPWireOp::ResourceCopyRegion), kWaitNone)
        << "resource_copy_region is unbarriered again (ID-118): after the flip, a copy record "
           "reading the client's texture object is an unconditional Fatal - the allowlist still "
           "admits the pair on the retiring-phase disjunct, so THIS is the assertion that sees it";
    EXPECT_FALSE(owedByThisPhase(
        kMGPipeFieldRetiringPhase[static_cast<SizeT>(MGPipeInputField::GetTextureObject)]));

    // ---- and the two rows THIS phase owes stay rejected ---------------------------------
    EXPECT_FALSE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetProgramForDraw,
                                           MGPipeVerb::DrawArrays))
        << "the draw path's pull became admitted - the allowlist is now forgiving the lane "
           "entries this phase is being run to fix";
    EXPECT_FALSE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetBoundVertexArray,
                                           MGPipeVerb::DrawArrays));
    EXPECT_FALSE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetProgramForDispatch,
                                           MGPipeVerb::DispatchCompute));
    // Magma's row: GetFramebufferBindingSlot retires in P5e on Espryt and P7 on Magma, and the
    // phase column is ONE string for both, so the derivation cannot admit it on the unbarriered
    // Clear verb. That is deliberate rather than a gap - the DirectVulkan entries carry their
    // own expected-red lane label instead, so the Espryt allowlist stays a statement about
    // Espryt (this package's item 7).
    EXPECT_FALSE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetFramebufferBindingSlot,
                                           MGPipeVerb::Clear));
    // A field that is not a debt at all is never admitted, on any verb.
    EXPECT_FALSE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetRenderStateParameters,
                                           MGPipeVerb::ReadPixels));
    // An out-of-range verb answers false rather than reading off the end: CountBarrierPull is
    // reached from a stamp, and a stamp is only as good as the op that produced it.
    EXPECT_FALSE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetFramebufferBindingSlot,
                                           MGPipeVerb::kVerbCount));

    // ---- THE RULE, RESTATED OVER THE WHOLE TABLE ----------------------------------------
    // The stamp map is INVERTED here rather than assumed: only an op MGPipeVerbForWireOp names
    // can ever be the verb a pull is reported against.
    Bool stamped[kMGPipeVerbCount] = {};
    Bool waitedFor[kMGPipeVerbCount] = {};
    for (SizeT op = 0; op < static_cast<SizeT>(MGPWireOp::kOpCount); ++op) {
        const MGPipeVerb verb = MGPipeVerbForWireOp(static_cast<MGPWireOp>(op));
        if (verb == MGPipeVerb::kVerbCount) continue;
        stamped[static_cast<SizeT>(verb)] = true;
        if (MGPipeWaitClassFor(static_cast<MGPWireOp>(op)) != kWaitNone) {
            waitedFor[static_cast<SizeT>(verb)] = true;
        }
    }
    SizeT admitted = 0;
    SizeT byWaitClass = 0;
    SizeT byRetiringPhase = 0;
    for (SizeT v = 0; v < kMGPipeVerbCount; ++v) {
        const auto verb = static_cast<MGPipeVerb>(v);
        for (SizeT f = 0; f < kMGPipeInputFieldCount; ++f) {
            const auto field = static_cast<MGPipeInputField>(f);
            if (!MGPipeBarrierPullAdmitted(field, verb)) continue;
            ++admitted;
            if (waitedFor[v]) ++byWaitClass;
            if (!owedByThisPhase(kMGPipeFieldRetiringPhase[f])) ++byRetiringPhase;
            EXPECT_TRUE(stamped[v])
                << kMGPipeInputFieldNames[f] << "@" << kMGPipeVerbNames[v]
                << " is admitted on a verb the server never stamps, so it is a row of the "
                   "allowlist nothing can exercise and nobody can retire";
            EXPECT_TRUE(waitedFor[v] || !owedByThisPhase(kMGPipeFieldRetiringPhase[f]))
                << kMGPipeInputFieldNames[f] << "@" << kMGPipeVerbNames[v]
                << " is admitted although this phase owes the row AND no barriered op stamps the "
                   "verb: after the flip that record's read is torn by construction, and "
                   "admitting it turns an unconditional Fatal into a warning line";
            EXPECT_EQ(kMGPipeFieldOwnership[f], MGPipeFieldOwnership::kBarrierPulled)
                << kMGPipeInputFieldNames[f] << " is admitted but is not a BARRIER-PULLED row";
            EXPECT_TRUE(MGPipeFieldMaskHas(
                kMGPipeClassFieldMask[static_cast<SizeT>(kMGPipeVerbClass[v])], field))
                << kMGPipeInputFieldNames[f] << "@" << kMGPipeVerbNames[v]
                << " is admitted but is outside the verb class's may-read mask, so the residual "
                   "fill never copied it for this verb";
        }
    }
    EXPECT_EQ(admitted, kMGPipeAdmittedPullPairCount)
        << "the walk and the generator disagree about how many pairs are admitted";
    EXPECT_GT(admitted, SizeT{0})
        << "the admitted set is EMPTY, which reads in the lane as rigour and is blindness: "
           "every barriered readback row would be reported as a fresh defect";
    // BOTH DISJUNCTS CARRY REAL WEIGHT. If either count were zero the rule would have collapsed
    // to the other one and a whole class of rows would be silently mis-classified.
    EXPECT_GT(byWaitClass, SizeT{0}) << "no pair is admitted by its verb's wait class";
    EXPECT_GT(byRetiringPhase, SizeT{0}) << "no pair is admitted by its retiring phase";
}

// ---------------------------------------------------------------------------------------
// The behaviour: the table is load-bearing. Split builds only - it is the server verb stamp
// that arms all of it, and nothing in a monolith lane stamps.
// ---------------------------------------------------------------------------------------

// P5d round 3 (package C): THE RESIDUAL FILL's SUPPLIED-SET MEMO, and the one thing about it
// that can be wrong. The predicate is the same expression step 4 used to spell once per field
// per verb, so the change carries no new verdict - what it carries is a KEY, and a key that
// misses one of its inputs answers the PREVIOUS environment's question at the new one. In the
// direction that loses that means the fill SKIPS a field no call supplied - and that is SILENT,
// not an abort: the walk stamps FilledGen from the verb serial whether or not it copied, so the
// field reads FRESH with the previous verb's value and the draw goes out with a stale binding
// slot. Poison catches an UNSTAMPED read; it cannot catch a stamped-but-uncopied one. That is
// exactly why this case exists - there is no second line of defence behind it.
//
// So each input is moved on its own with the answer read back in between, and in an order where
// a memo that ignored that input would have to return the stale answer rather than the right one
// by luck. This case lives outside the split-only block because the memo is in the push build
// too (PipeFill.cpp is compiled at MOBILEGL_PIPE_PUSH), and the fields it names are the same
// three in both.
//
// THE FOURTH KEY INPUT - the P4a consumer signal - GETS STEP 4 AND A DIFFERENT SHAPE, because
// it cannot move an answer today and no honest case can pretend otherwise: every field whose
// emitter belongs to a P4a family is also a field the applier cannot supply whole (its storage
// is a frontend pointer), so that conjunct is DOMINATED and the mask is identical either way.
// Step 4 moves the signal for real and pins the domination instead, so that the day a P4a row
// gains a twin the case goes red and says what to write - the same day PipeFill.cpp's
// NoP4aFamilyFieldIsWhollySupplied() static_assert fires.
TEST_F(FieldOwnershipTest, TheResidualFillsSuppliedMemoReKeysOnEveryInputThatMovesAnAnswer) {
    constexpr Uint64 kNoSubsystems = 0;
    constexpr Uint64 kEverySubsystem = ~Uint64{0};

    // 1. THE PUSH MASK. create_render_state carries GetRenderStateParameters whole and the
    //    applier writes it without deriving, so at a mask that carries the render-state bit the
    //    fill skips it - and at 0, which is what a unit lane runs at (Features.PipePush defaults
    //    to 0), nothing is emitted and every field is pulled.
    EXPECT_FALSE(MGPipeResidualFillSuppliesField(MGPipeInputField::GetRenderStateParameters,
                                                 kNoSubsystems, true, false));
    EXPECT_TRUE(MGPipeResidualFillSuppliesField(MGPipeInputField::GetRenderStateParameters,
                                                kEverySubsystem, true, false));
    EXPECT_FALSE(MGPipeResidualFillSuppliesField(MGPipeInputField::GetRenderStateParameters,
                                                 kNoSubsystems, true, false))
        << "the memo answered the previous mask: MOBILEGL_PIPE_PUSH is not in its key, and a "
           "per-subsystem A/B would then fill from the wrong subsystem set";

    // 2. THE DERIVATION LATCH. GetViewport reaches PipeInputs only through
    //    MGPipeDeriveRenderStateFields, so a build whose derivation is a stub must keep PULLING
    //    it - skipping it there leaves the mirror unwritten and the backend reading a default.
    EXPECT_TRUE(MGPipeResidualFillSuppliesField(MGPipeInputField::GetViewport, kEverySubsystem,
                                                true, false));
    EXPECT_FALSE(MGPipeResidualFillSuppliesField(MGPipeInputField::GetViewport, kEverySubsystem,
                                                 false, false))
        << "the memo answered the previous latch: ApplierDerivesRenderStateFields is not in its key";

    // 3. P5c rv's WIRE GATE (CONTRACT-P5C.md §5.3). set_context_values has no producer without a
    //    live wire, so its eight value-class fields keep being pulled under monolith - G1's
    //    byte-for-byte rule - and are skipped only when the record really crosses.
    EXPECT_FALSE(MGPipeResidualFillSuppliesField(MGPipeInputField::GetActiveTextureUnit,
                                                 kEverySubsystem, true, false));
    EXPECT_TRUE(MGPipeResidualFillSuppliesField(MGPipeInputField::GetActiveTextureUnit,
                                                kEverySubsystem, true, true))
        << "the memo answered the previous wire state: contextValuesWireLive is not in its key, "
           "and the emission's half of the gate and the fill's half would then disagree";

    // 4. THE P4a CONSUMER SIGNAL, which is in the key and is DOMINATED, so this step pins the
    //    domination rather than pretending to move an answer. Under monolith the signal IS "did
    //    a backend register the resource op table" (P4aFamilyHasItsConsumer, PipeFill.cpp), and
    //    MG_Config::Transport is a constexpr Monolith in every lane but a split one - so the
    //    signal really moves here, at a mask that carries all four P4a bits, which is the only
    //    mask at which the fill reads it at all.
    if (MG_Config::Transport == MG_Config::TransportMode::Monolith) {
        const MGPipeResourceOps* const savedOps = MGPipeGetResourceOps();
        MGPipeResourceOps ops{};
        MGPipeFieldMask withConsumer{};
        MGPipeFieldMask withoutConsumer{};

        // An EMPTY table is enough: the predicate only asks whether ONE IS REGISTERED, which is
        // how MG_Test/Pipe arms this subsystem everywhere else.
        MGPipeSetResourceOps(&ops);
        ASSERT_NE(MGPipeGetResourceOps(), nullptr)
            << "the fixture did not move the signal it names, so this step observes nothing";
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            if (MGPipeResidualFillSuppliesField(static_cast<MGPipeInputField>(i), kEverySubsystem,
                                                true, true)) {
                withConsumer.Words[i / 64] |= (Uint64{1} << (i % 64));
            }
        }

        MGPipeSetResourceOps(nullptr);
        ASSERT_EQ(MGPipeGetResourceOps(), nullptr);
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            if (MGPipeResidualFillSuppliesField(static_cast<MGPipeInputField>(i), kEverySubsystem,
                                                true, true)) {
                withoutConsumer.Words[i / 64] |= (Uint64{1} << (i % 64));
            }
        }
        MGPipeSetResourceOps(savedOps);

        // THE NAMED ROW, both ways: GetFramebufferBindingSlot is emitted by set_framebuffer_state
        // and so rides the framebuffer family's consumer gate - and is pulled regardless, because
        // its storage is a BindingSlot<FramebufferObject> no payload can carry.
        EXPECT_FALSE(MGPipeFieldMaskHas(withConsumer, MGPipeInputField::GetFramebufferBindingSlot));
        EXPECT_FALSE(
            MGPipeFieldMaskHas(withoutConsumer, MGPipeInputField::GetFramebufferBindingSlot));

        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            const auto field = static_cast<MGPipeInputField>(i);
            EXPECT_EQ(MGPipeFieldMaskHas(withConsumer, field),
                      MGPipeFieldMaskHas(withoutConsumer, field))
                << kMGPipeInputFieldNames[i]
                << "'s supplied answer moved with the P4a consumer signal. That is CORRECT "
                   "behaviour and this assertion is a trip wire, not a defect report: the "
                   "conjunct stopped being dominated, so the memo's fourth key input is now "
                   "observable and needs the move-it-and-read-it-back pair steps 1-3 give the "
                   "other three - under split through CapsMirrorInstance().Adopt() with and "
                   "without kMGPipeSubsystemResources in the CallMask, as "
                   "SanityTest.ACapsMaskWithoutTheResourceFamilyEmitsNothingAndCountsTheRefusal "
                   "already does. Rewrite this step into that pair; PipeFill.cpp's "
                   "NoP4aFamilyFieldIsWhollySupplied() static_assert fires on the same change";
        }
    }
}

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
    // under the verb that actually reads it. And P5c rv's record-supplied rows join it there:
    // GetActiveTextureUnit is exactly the read that used to count into `rsp`.
    MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
    (void)gPipeInputs.GetPixelStoreParameters(false); // the half that has a carrier
    (void)gPipeInputs.GetActiveTextureUnit();         // P5c rv: set_context_values carries it
    (void)gPipeInputs.GetMaxTouchedTextureUnit();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{0});
    // And a shutter answers the applier's own Serial under a server stamp (APPLIER-DERIVED),
    // not the client's residual-fill copy: still zero pulls, and the answer MOVES when the
    // applier's texture state does.
    (void)gPipeInputs.GetTextureBindGeneration();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{0});
}

// P5c rv (CONTRACT-P5C.md §5.3): the residual-value record's applier write, read back through
// the accessors a backend uses, under the stamps that publish them - and the three texture
// shutters answering the applier's own serials rather than the client's fill. kReadback's
// class carries the two texture-unit counters, kDraw's the rest.
TEST_F(FieldOwnershipTest, SetContextValuesLandsInPipeInputsAndTheShuttersAnswerTheApplier) {
    MGPContextValues values{};
    values.ActiveTextureUnit = 5;
    values.MaxTouchedTextureUnit = 23;
    values.TouchedBufferBindingPointCount[static_cast<Uint32>(BufferTarget::Uniform)] = 7;
    values.IsTransformFeedbackActive = 1;
    values.IsTransformFeedbackPaused = 0;
    values.TransformFeedbackGeneration = 0x1112131415161718ull;
    values.BoundTransformFeedbackLifetimeId = 0x2122232425262728ull;
    values.TransformFeedbackCapturedVertices = 0x3132333435363738ull;
    MGPipeApplySetContextValues(values);

    MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
    EXPECT_EQ(gPipeInputs.GetActiveTextureUnit(), 5);
    EXPECT_EQ(gPipeInputs.GetMaxTouchedTextureUnit(), 23);
    MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
    EXPECT_EQ(gPipeInputs.GetTouchedBufferBindingPointCount(BufferTarget::Uniform), SizeT{7});
    EXPECT_EQ(gPipeInputs.GetTouchedBufferBindingPointCount(BufferTarget::Vertex), SizeT{0});
    EXPECT_TRUE(gPipeInputs.IsTransformFeedbackActive());
    EXPECT_FALSE(gPipeInputs.IsTransformFeedbackPaused());
    EXPECT_EQ(gPipeInputs.GetTransformFeedbackGeneration(), 0x1112131415161718ull);
    EXPECT_EQ(gPipeInputs.GetBoundTransformFeedbackLifetimeId(), 0x2122232425262728ull);
    EXPECT_EQ(gPipeInputs.GetTransformFeedbackCapturedVertices(), 0x3132333435363738ull);
    // Eight record-supplied reads and not one residual pull.
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{0});

    // The three shutters answer the applier's own serials under a stamped verb. They are
    // SHUTTERS - the value matters only in that it MOVES when the server's texture state does
    // and never walks backwards - so the pin is the identity with the applier's counters, not
    // any particular number.
    EXPECT_EQ(gPipeInputs.GetTextureBindGeneration(), MGPipeApplierTextureShutterSerial());
    EXPECT_EQ(gPipeInputs.GetSamplingResolutionGeneration(), MGPipeApplierTextureShutterSerial());
    EXPECT_EQ(gPipeInputs.GetTextureContextId(), MGPipeApplierContextSerial());
    const Uint64 before = MGPipeApplierTextureShutterSerial();
    MGPipeApplierNoteTextureStateMoved();
    EXPECT_EQ(gPipeInputs.GetTextureBindGeneration(), before + 1)
        << "a texture-state apply moved the serial but the shutter did not answer with it";
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{0});
}

TEST_F(FieldOwnershipTest, ABarrierPulledReadAfterAServerStampIsCountedNotFatal) {
    // P5c rv: the exemplars are OBJECT-class now - the value rows this case used to read
    // (GetActiveTextureUnit / GetTextureContextId / GetMaxTouchedTextureUnit) are
    // RECORD-SUPPLIED / APPLIER-DERIVED since rv, and reading them here would count nothing.
    // The three below are all of kDraw's class, all SharedPtr reads, and all safe on
    // never-filled storage.
    MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
    ASSERT_EQ(MGPipeResidualPullCount(), Uint64{0});
    (void)gPipeInputs.GetBoundVertexArray();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{1});
    (void)gPipeInputs.GetProgramForDraw();
    (void)gPipeInputs.GetTransformFeedbackProgram();
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
// GetProgramForDraw, so reading it there is a stale answer rather than a residual pull,
// and it stays Fatal. Counting it would trade a loud staleness for a quiet one.
// (P5c rv: the exemplar moved - GetActiveTextureUnit is RECORD-SUPPLIED since rv and would
// say nothing about the pulled set here.)
TEST_F(FieldOwnershipTest, ABarrierPulledFieldOutsideTheVerbsClassIsStillFatal) {
#if MGTEST_HAVE_FORK
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
        (void)gPipeInputs.GetProgramForDraw(); // BARRIER-PULLED, but not in kClear's class
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetProgramForDraw@Clear\"}"), std::string::npos)
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
    MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
    (void)gPipeInputs.GetBoundVertexArray();
    EXPECT_EQ(PS::FrameCalls(PS::CallClass::ResidualPulls), Uint64{1});
    EXPECT_NE(PS::FormatWindowLine().find("rsp="), std::string::npos) << PS::FormatWindowLine();
    PS::ResetForTesting();
    PS::SetEnabledForTesting(false);
}

// ================================================================================
// P5f (f1): THE DUAL-BLOCK REHEARSAL AT THE BLOCK LEVEL (P5F-WIRE-COMPLETENESS.md §4)
// ================================================================================
//
// MOBILEGL_IPC_ROLE_SPLIT_STATE=1 gives the fill side its own PipeInputs block
// (MGPipeClientInputs()) and leaves gPipeInputs to the server alone. These cases pin the
// mechanism's three arms without a transport running: the selection folds to the shared block
// when the knob is off OR the transport is monolith, the two blocks are distinct objects when
// it is armed, and a BARRIER-PULLED read under it is a NAMED Fatal with no strict knob
// involved (the fork cases below).

namespace {
    // Arms the rehearsal by hand - the knob is parsed from the environment once per process,
    // so a case sets the two globals directly, and the guard puts them back on every exit
    // path, ASSERT death included.
    struct RoleSplitArm {
        RoleSplitArm() {
            m_previousTransport = MG_Config::Transport;
            m_previousKnob = MG_Config::Ipc.RoleSplitState;
        }
        ~RoleSplitArm() {
            MG_Config::Transport = m_previousTransport;
            MG_Config::Ipc.RoleSplitState = m_previousKnob;
        }
        void Arm(Bool arm) {
            MG_Config::Ipc.RoleSplitState = arm;
            MG_Config::Transport = arm ? MG_Config::TransportMode::InProcess
                                       : MG_Config::TransportMode::Monolith;
        }
        MG_Config::TransportMode m_previousTransport;
        Bool m_previousKnob;
    };
} // namespace

TEST_F(FieldOwnershipTest, RoleSplitOffFoldsTheFillSideOntoTheSharedBlock) {
    EXPECT_FALSE(MGPipeRoleSplitActive());
    EXPECT_EQ(&MGPipeClientInputs(), &gPipeInputs);
}

// The knob alone is not the arm: under monolith transport the two roles are one thread and
// there is exactly one block, or every read would starve. This is the fold the whole unit
// lane and the integration-gpu lane of a split build stand on.
TEST_F(FieldOwnershipTest, RoleSplitUnderMonolithTransportIsStillOneBlock) {
    RoleSplitArm guard;
    MG_Config::Ipc.RoleSplitState = true;
    MG_Config::Transport = MG_Config::TransportMode::Monolith;
    EXPECT_FALSE(MGPipeRoleSplitActive());
    EXPECT_EQ(&MGPipeClientInputs(), &gPipeInputs);
}

TEST_F(FieldOwnershipTest, RoleSplitGivesTheFillSideADistinctBlockTheStampNeverTouches) {
    RoleSplitArm guard;
    guard.Arm(true);
    ASSERT_TRUE(MGPipeRoleSplitActive());
    ASSERT_NE(&MGPipeClientInputs(), &gPipeInputs);

    const Uint64 serverSerialBefore = gPipeInputs.FilledState().CurrentVerbSerial;
    const Uint64 clientSerialBefore = MGPipeClientInputs().FilledState().CurrentVerbSerial;
    MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
    EXPECT_EQ(gPipeInputs.FilledState().CurrentVerbSerial, serverSerialBefore + 1);
    EXPECT_EQ(MGPipeClientInputs().FilledState().CurrentVerbSerial, clientSerialBefore)
        << "the server's verb stamp reached into the client block";
    EXPECT_TRUE(gPipeInputs.ServerStampedVerb());
    EXPECT_FALSE(MGPipeClientInputs().ServerStampedVerb());

    // The fill side's clear is the client block's own: it must not disarm the server's stamp,
    // which is CONTRACT-P5E §3.2's "the server's stamp is server-private" as a fact rather
    // than as a comment.
    MGPipeClientClearVerbBoundary();
    EXPECT_TRUE(gPipeInputs.ServerStampedVerb())
        << "the client-role clear withdrew the SERVER's stamp";
    MGPipeServerClearVerbBoundary();
    EXPECT_FALSE(gPipeInputs.ServerStampedVerb());

    // And the server block's identity is server-owned under the rehearsal (§3.2's other half):
    // the stamp set it, and it is non-null - a null identity reads as a hit against
    // DirectGLES' zero-initialised fb-slot memo cache, which is the unnamed crash this line
    // exists to preclude.
    EXPECT_NE(gPipeInputs.ContextIdentity(), nullptr);
}

// The fill side's writers land in the client block alone. MGPipeLeaveVerb rather than
// MGPipeValidateForVerb: the validate point's step 3 EMITS, which is session machinery this
// process does not have, while LeaveVerb's serial bump and verb reset are exactly the fill
// side's write shape with nothing else in the way.
TEST_F(FieldOwnershipTest, TheClientVerbLeaveWritesTheClientBlockAlone) {
    RoleSplitArm guard;
    guard.Arm(true);
    ASSERT_NE(&MGPipeClientInputs(), &gPipeInputs);
    const Uint64 serverSerialBefore = gPipeInputs.FilledState().CurrentVerbSerial;
    const Uint64 clientSerialBefore = MGPipeClientInputs().FilledState().CurrentVerbSerial;
    MGPipeLeaveVerb();
    EXPECT_EQ(MGPipeClientInputs().FilledState().CurrentVerbSerial, clientSerialBefore + 1);
    EXPECT_EQ(gPipeInputs.FilledState().CurrentVerbSerial, serverSerialBefore)
        << "a fill-side write reached the server block";
    EXPECT_EQ(MGPipeClientInputs().CurrentVerb(), MGPipeVerb::kVerbCount);
}

#if MGTEST_HAVE_FORK

// R-7.3's proof that the instrumentation can go red. An instrumentation that cannot is
// decoration, and the set it counts is not empty. (P5c rv: the exemplar is object-class now -
// the value rows this case was written against ride set_context_values and answer
// RECORD-SUPPLIED.)
TEST_F(FieldOwnershipTest, StrictErrorsTurnsABarrierPulledReadIntoANamedAbort) {
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"),
              std::string::npos)
        << r.Log;
    EXPECT_NE(r.Log.find("BARRIER-PULLED"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("MOBILEGL_IPC_STRICT_ERRORS=1"), std::string::npos) << r.Log;
    // The strict line names the phase that owes the answer; a strict abort that did not would
    // leave the reader exactly where the gate found them. P5e re-annotated this row (and the
    // five object-class rows beside it) from "P8" to the phase that actually retires the pull,
    // which is why the expected text moved with FieldOwnership.def rather than the case being
    // re-pointed at another field: the string IS the debt entry, read out of the generated
    // table, and a case that stopped checking it would let the annotation rot.
    EXPECT_NE(r.Log.find("retires in P5e (Espryt unbarriered), P7 (Magma)]"), std::string::npos)
        << r.Log;
}

TEST_F(FieldOwnershipTest, TheSameReadWithoutStrictErrorsSurvivesAndIsCounted) {
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
        if (MGPipeResidualPullCount() != 1) ::_exit(7);
    });
    ASSERT_TRUE(ExitedWith(r, 0)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{"), std::string::npos) << r.Log;
}

// P5e (gl) RED-ONCE, ID-128: THE THIRD DISJUNCT, AND IT IS A RUNTIME FACT ABOUT THE RECORD.
//
// GetBoundVertexArray@DrawArrays is this phase's debt on an unbarriered verb, so the static
// table rejects it and strict aborts - that is the case two blocks up, and it must keep doing
// that for the ORDINARY draw path vi and mv retired. But the two
// DirectGLES.Split.ClientVertexArrayScenario entries pull the same pair on a record the client
// IS parked behind: MGPipeBarriered escalates a draw carrying client vertex arrays (ID-83), and
// ID-82 puts client arrays outside P5e entirely - under run-ahead the client refuses them by
// name and staging is P8's. The read site (SyncClientSideVertexArraysForDrawArrays) aborts with
// Fatal{RoleViolation, "MGPipeSlots"} if it is ever applied unbarriered, so the pull is legal.
//
// ONE PAIR, TWO VERDICTS, DECIDED BY THE RECORD AND NOT BY THE TABLE. That is why the flag is
// stamped in ApplyOne beside the barriered stamp rather than folded into the generated table:
// no table indexed by (field, verb) can tell these two entries apart.
//
// THE RED: drop `&& !escalated` from CountBarrierPull's strict arm and this case dies of
// SIGABRT on Fatal{UnmigratedPipeInput, "GetBoundVertexArray@DrawArrays"}.
//
// THE CONTROL IS THE CASE TWO BLOCKS UP, NOT A SECOND CHILD HERE.
// StrictErrorsTurnsABarrierPulledReadIntoANamedAbort drives the IDENTICAL pull - same field,
// same verb, same knob - with the escalation flag at its `false` default, and asserts the abort.
// Reading the log delta of two forks in one case is what a shared log file cannot support.
TEST_F(FieldOwnershipTest, AnEscalatedRecordsPullIsAdmittedAndSaysWhy) {
    const ChildResult escalated = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        // What PipeApplier::ApplyOne stamps for a draw whose payload carries kDrawClientArrays:
        // MGPipeBarriered says barriered, the static wait class of draw_vbo says kWaitNone.
        MGPipeApplierSetCurrentRecordBarrieredByEscalation(true);
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
        if (MGPipeResidualPullCount() != 1) ::_exit(7);
    });
    ASSERT_TRUE(ExitedWith(escalated, 0)) << DescribeStatus(escalated) << "\n" << escalated.Log;
    EXPECT_EQ(escalated.Log.find("Fatal{UnmigratedPipeInput"), std::string::npos) << escalated.Log;
    // THE MARKER SAYS WHICH DISJUNCT, and that is what keeps the lane's comparison exact: a pair
    // admitted by the table is checkable against `--print-admitted`, one admitted by escalation
    // is not in any static list and must not widen one.
    EXPECT_NE(escalated.Log.find("Admitted{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"
                                 " [BARRIER-PULLED, ADMITTED-ESCALATED, retires in "),
              std::string::npos)
        << escalated.Log;
}

// THE SEVEN STICKY FORWARDS ARE NOT EXEMPT FROM THE DETECTOR, which is what this case has
// always been for - it was written as "strict PROMOTES them to a Fatal" because before ID-125
// every barrier-pulled read under strict aborted.
//
// P5e (gl), ID-125 CHANGED THE VERDICT AND NOT THE REACH, and the rename says which. All seven
// sticky forwards retire in a LATER phase (P7, P7/P9, P7/P13, P9 - FieldOwnership.def's forward
// list; not one names P5e), so on any stamped verb the second disjunct admits them: the record
// is barriered, the pull is legal by construction, and the phase that owes the migration is not
// this one. The thing that would be a defect is a sticky forward the detector never REACHED -
// a read that answered out of gPipeInputs without being counted or named - and that is what is
// asserted here, by the counter moving and by the marker naming the field and its phase.
//
// The Fatal half of the mechanism keeps its own case beside this one:
// StrictErrorsTurnsABarrierPulledReadIntoANamedAbort uses GetBoundVertexArray, which IS this
// phase's debt on an unbarriered verb and therefore still aborts.
TEST_F(FieldOwnershipTest, StrictErrorsReachTheStickyForwardsAndNameThemByField) {
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.ValidateProgramName(1u);
        // NOT EXEMPT: the forward went through CountBarrierPull like any field accessor. Without
        // this, "it did not abort" would be indistinguishable from "it was never detected", and
        // the sticky set is exactly where the gate is structurally blind if it is.
        if (MGPipeResidualPullCount() != 1) ::_exit(7);
    });
    ASSERT_TRUE(ExitedWith(r, 0)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{UnmigratedPipeInput"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("Admitted{UnmigratedPipeInput, \"ValidateProgramName@DrawArrays\"} "
                         "[BARRIER-PULLED, ADMITTED, retires in P9]"),
              std::string::npos)
        << "the sticky forward was neither aborted on nor named; a debt nobody prints is a debt "
           "nobody retires\n"
        << r.Log;
}

// P5e (gl) RED-ONCE, ID-117: THE THIRD STATE, AND IT IS THE SAME FIELD AS THE CASE ABOVE.
//
// ValidateProgramName@DrawArrays aborts (draw_vbo is kWaitNone: the client will not be parked
// behind that record and the read is torn by construction). ValidateProgramName@ReadPixels is
// the same sticky forward under a verb whose op IS statically barriered, so the client is
// parked, the value is real and ordered, and the debt is P9's rather than this phase's - it
// says so and the entry COMPLETES. One field, two verbs, opposite verdicts: that is the whole
// of ID-84 in two cases, and before this change the lane could not tell them apart because
// strict aborted on both, which made CI's allowlist comparison unreachable code.
//
// THE RED: delete the `if (!MGPipeBarrierPullAdmitted(...))` guard in CountBarrierPull so the
// strict arm is unconditional again, and this case dies of SIGABRT instead of exiting 0.
TEST_F(FieldOwnershipTest, AnAdmittedBarrierPullIsLoudOnceAndNotFatal) {
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
        (void)gPipeInputs.ValidateProgramName(1u);
        (void)gPipeInputs.ValidateProgramName(1u);
        (void)gPipeInputs.ValidateProgramName(2u);
        // rsp counts every barriered pull, and under strict every surviving barriered pull is an
        // admitted one - ID-119's reworded pin, from the counter's side.
        if (MGPipeResidualPullCount() != 3) ::_exit(7);
    });
    ASSERT_TRUE(ExitedWith(r, 0)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{UnmigratedPipeInput"), std::string::npos)
        << "an admitted pull aborted: the lane can never be green while a debt this phase "
           "deliberately leaves standing is fatal\n"
        << r.Log;
    // THE GRAMMAR, VERBATIM. `<field>@<verb>` and the bracketed tail are the Fatal's, character
    // for character; only the leading tag differs, so every filter written since P5c that
    // matches `Fatal{UnmigratedPipeInput` still means exactly "red".
    EXPECT_NE(r.Log.find("MGPipe: Admitted{UnmigratedPipeInput, \"ValidateProgramName@ReadPixels\"}"
                         " [BARRIER-PULLED, ADMITTED, retires in P9]"),
              std::string::npos)
        << r.Log;
    // DEDUPED PER (field, verb): three pulls, one line. An 852-draw frame must not write 852.
    SizeT lines = 0;
    for (std::size_t at = r.Log.find("Admitted{UnmigratedPipeInput"); at != std::string::npos;
         at = r.Log.find("Admitted{UnmigratedPipeInput", at + 1)) {
        ++lines;
    }
    EXPECT_EQ(lines, SizeT{1})
        << "the admitted marker is not deduped per (field, verb); a frame of draws would write "
           "one line per pull and the marker census could not be read\n"
        << r.Log;
}

// A FATAL-class read aborts whatever the knob says: no carrier, and the reduced path never
// reads it, so it is a real defect rather than a debt.
// P5b i1: the exemplar MOVED. This case used GetProgramForDispatch, which is BARRIER-PULLED
// from i1 on (a debt the server serves, not an abort), so it would now assert that a served
// read aborts - green for the wrong reason at best. GetTransformFeedbackPausedPrimitiveCounter
// is the same statement with a field that is still FATAL: reachable only from class kQuery,
// which the reduced path never enters.
// RED ONCE BY DOING X: put GetProgramForDispatch back in the FATAL block of FieldOwnership.def
// and TheFieldOwnershipTableIsTheContractsTableRow's new kBarrierPulled expectation goes red by
// name; swap the field below for GetProgramForDispatch and THIS case goes red instead, because
// a barrier-pulled read under a stamp does not abort.
TEST_F(FieldOwnershipTest, AFatalClassReadAbortsEvenWithoutStrictErrors) {
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetTransformFeedbackPausedPrimitiveCounter();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, "
                         "\"GetTransformFeedbackPausedPrimitiveCounter@DrawArrays\"}"),
              std::string::npos)
        << r.Log;
}

// And the field that LEFT the FATAL class is served rather than fatal, under the verb that made
// it reachable. This is i1's half of the §6.9 grant made checkable: a dispatch stamp plus a read
// of the compute program must NOT abort, which is precisely the statement "compute is on the
// path now". RED ONCE BY DOING X: revert the FieldOwnership.def row to FATAL and this child
// aborts with Fatal{UnmigratedPipeInput, "GetProgramForDispatch@DispatchCompute"}.
TEST_F(FieldOwnershipTest, TheComputeProgramIsServedUnderADispatchStampFromP5bOn) {
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::DispatchCompute);
        (void)gPipeInputs.GetProgramForDispatch();
        MGPipeServerClearVerbBoundary();
    });
    EXPECT_TRUE(ExitedWith(r, 0)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{UnmigratedPipeInput, \"GetProgramForDispatch"), std::string::npos)
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

// P5f (f1): THE REHEARSAL'S FATAL ARM, AND THE PROOF IT NEEDS NO OTHER KNOB. Under
// MOBILEGL_IPC_ROLE_SPLIT_STATE=1 a BARRIER-PULLED read is Fatal{UnmigratedPipeInput} with
// StrictErrors FALSE - the server block has no residual fill to answer from, so the read has
// no legal value whatever the strict knob says, and the marker names the knob that armed it.
// This is also the red-once for the lane's "no unnamed crashes" gate: the read below is an
// O-class SharedPtr field, which without this arm would hand the backend an empty pointer to
// dereference.
TEST_F(FieldOwnershipTest, DualBlockMakesABarrierPulledReadANamedAbortWithoutStrict) {
    const ChildResult r = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MG_Config::Ipc.RoleSplitState = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"),
              std::string::npos)
        << r.Log;
    EXPECT_NE(r.Log.find("BARRIER-PULLED"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("MOBILEGL_IPC_ROLE_SPLIT_STATE=1"), std::string::npos) << r.Log;
}

// ... and the same read with the knob set but the transport MONOLITH is the folded arm: one
// block, the pull counted, no Fatal. The knob is not the mechanism; the knob AND a real
// transport are.
TEST_F(FieldOwnershipTest, DualBlockDoesNotArmUnderMonolithTransport) {
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.RoleSplitState = true; // the knob, with no transport behind it
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
        if (MGPipeResidualPullCount() != 1) ::_exit(7);
    });
    ASSERT_TRUE(ExitedWith(r, 0)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{"), std::string::npos) << r.Log;
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
