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
//   retired pointer/forward reads are unconditionally named Fatal on the server, and that
//   monolith storage remains readable without ever borrowing a server stamp.
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
    EXPECT_EQ(pulled, SizeT{0}) << "P5f exit forbids reintroducing a residual pull";
}

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, TheReducedPathsUnmigratedFieldsAreAllAccountedFor) {
    // Historical name retained for G14. These are retired legacy getters, not records.
    const MGPipeInputField retired[] = {
        MGPipeInputField::GetBoundVertexArray, MGPipeInputField::GetBufferBindingSlot,
        MGPipeInputField::GetBufferBindingPoint, MGPipeInputField::GetFramebufferBindingSlot,
        MGPipeInputField::GetImageTextureBinding, MGPipeInputField::GetTextureUnitObject,
        MGPipeInputField::GetProgramForDraw, MGPipeInputField::GetProgramForDispatch,
        MGPipeInputField::GetTransformFeedbackProgram, MGPipeInputField::GetProgramObject,
        MGPipeInputField::GetTextureObject, MGPipeInputField::ValidateProgramName,
        MGPipeInputField::RecordError,
    };
    for (const auto field : retired)
        EXPECT_EQ(MGPipeFieldOwnershipOf(field), MGPipeFieldOwnership::kFatal)
            << kMGPipeInputFieldNames[Index(field)] << " revived a client-memory accessor";
    EXPECT_EQ(kMGPipeBarrierPulledFieldCount, SizeT{0});
    EXPECT_EQ(kMGPipeAdmittedPullPairCount, SizeT{0});
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetPixelStoreParameters, 0),
              MGPipeFieldOwnership::kApplierDerived);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetPixelStoreParameters, 1),
              MGPipeFieldOwnership::kFatal);
    for (const auto field : {MGPipeInputField::GetBufferBindingPointCount,
                            MGPipeInputField::HasOpenTransformFeedbackSpan,
                            MGPipeInputField::GetTextureBindGeneration,
                            MGPipeInputField::GetSamplingResolutionGeneration,
                            MGPipeInputField::GetTextureContextId})
        EXPECT_EQ(MGPipeFieldOwnershipOf(field), MGPipeFieldOwnership::kApplierDerived);
    for (const auto field : {MGPipeInputField::GetActiveTextureUnit,
                            MGPipeInputField::GetMaxTouchedTextureUnit,
                            MGPipeInputField::GetTouchedBufferBindingPointCount,
                            MGPipeInputField::IsTransformFeedbackActive,
                            MGPipeInputField::IsTransformFeedbackPaused,
                            MGPipeInputField::GetTransformFeedbackGeneration,
                            MGPipeInputField::GetBoundTransformFeedbackLifetimeId,
                            MGPipeInputField::GetTransformFeedbackCapturedVertices,
                            MGPipeInputField::GetCurrentVertexAttribute})
        EXPECT_EQ(MGPipeFieldOwnershipOf(field), MGPipeFieldOwnership::kRecordSupplied);
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

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, TheAdmittedPullTableIsID84sDerivationAndNotAList) {
    // P5f retires every production debt row. Generator self-tests retain synthetic
    // debt fixtures to exercise all historical admission disjuncts independently.
    EXPECT_EQ(kMGPipeBarrierPulledFieldCount, SizeT{0});
    EXPECT_EQ(kMGPipeAdmittedPullPairCount, SizeT{0});
    for (SizeT v = 0; v < kMGPipeVerbCount; ++v)
        for (SizeT f = 0; f < kMGPipeInputFieldCount; ++f)
            EXPECT_FALSE(MGPipeBarrierPullAdmitted(static_cast<MGPipeInputField>(f),
                                                   static_cast<MGPipeVerb>(v)))
                << kMGPipeInputFieldNames[f] << "@" << kMGPipeVerbNames[v];
    EXPECT_FALSE(MGPipeBarrierPullAdmitted(MGPipeInputField::GetFramebufferBindingSlot,
                                          MGPipeVerb::kVerbCount));
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
        // P5f fe retired the scalar forwards to server-owned answers. Their freshness
        // comes from that ownership; the remaining client forwards lose the exemption.
        const Bool serverOwned = MGPipeFieldOwnershipOf(field) == MGPipeFieldOwnership::kApplierDerived;
        EXPECT_EQ(MGPipeInputFieldIsFresh(gPipeInputs.FilledState(), field), serverOwned)
            << kMGPipeInputFieldNames[Index(field)] << " has the wrong server-stamp freshness";
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

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, ABarrierPulledReadAfterAServerStampIsCountedNotFatal) {
#if MGTEST_HAVE_FORK
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {

        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
#else
    GTEST_SKIP() << "needs fork";
#endif
}

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, AStickyForwardIsCountedAsAResidualPull) {
#if MGTEST_HAVE_FORK
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {

        MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
        (void)gPipeInputs.ValidateProgramName(1u);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"ValidateProgramName@Clear\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
#else
    GTEST_SKIP() << "needs fork";
#endif
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
    (void)gPipeInputs.GetActiveTextureUnit();
    ASSERT_EQ(MGPipeResidualPullCount(), Uint64{0});
    MGPipeServerClearVerbBoundary();
    EXPECT_FALSE(gPipeInputs.ServerStampedVerb());
    (void)gPipeInputs.ValidateProgramName(1u);
    gPipeInputs.InvalidateCompileEnv();
    EXPECT_EQ(MGPipeResidualPullCount(), Uint64{0});
}

// The verb's own may-read table still holds on the server: kClear does not read
// GetProgramForDraw, so reading it there is a forbidden legacy access,
// and it stays Fatal. Counting it would trade a loud staleness for a quiet one.
// (P5c rv: the exemplar moved - GetActiveTextureUnit is RECORD-SUPPLIED since rv and would
// say nothing about the pulled set here.)
TEST_F(FieldOwnershipTest, ABarrierPulledFieldOutsideTheVerbsClassIsStillFatal) {
#if MGTEST_HAVE_FORK
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
        (void)gPipeInputs.GetProgramForDraw(); // retired accessor, outside kClear too
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
    (void)gPipeInputs.GetCurrentVertexAttribute(0);
    EXPECT_EQ(PS::FrameCalls(PS::CallClass::ResidualPulls), Uint64{0});
    EXPECT_NE(PS::FormatWindowLine().find(" rsp=0"), std::string::npos) << PS::FormatWindowLine();
    // Prove the reporting channel did not merely become a constant zero after retirement.
    PS::AddCalls(PS::CallClass::ResidualPulls, 1);
    EXPECT_EQ(PS::FrameCalls(PS::CallClass::ResidualPulls), Uint64{1});
    EXPECT_NE(PS::FormatWindowLine().find(" rsp=1"), std::string::npos) << PS::FormatWindowLine();
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

TEST_F(FieldOwnershipTest, SplitBindingPointCapacityNeverConsultsTheFrontend) {
    RoleSplitArm guard;
    guard.Arm(true);
    MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
    EXPECT_EQ(gPipeInputs.GetBufferBindingPointCount(BufferTarget::Uniform), 84u);
    EXPECT_EQ(gPipeInputs.GetBufferBindingPointCount(BufferTarget::TransformFeedback), 84u);
    EXPECT_EQ(gPipeInputs.GetBufferBindingPointCount(BufferTarget::PixelPack), 0u);
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::GetBufferBindingPointCount),
              MGPipeFieldOwnership::kApplierDerived);
    MGPipeServerClearVerbBoundary();
}

TEST_F(FieldOwnershipTest, SplitOpenSpansAreOwnedByTheApplierAndSurviveOtherBindings) {
    RoleSplitArm guard;
    guard.Arm(true);
    auto& state = MGPipeApplier();
    state.StreamOutputSpans.clear();
    MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
    EXPECT_FALSE(gPipeInputs.HasOpenTransformFeedbackSpan(0));
    EXPECT_FALSE(gPipeInputs.HasOpenTransformFeedbackSpan(11));
    state.StreamOutputSpans[11] = {};
    state.StreamOutputSpans[12] = {};
    state.BoundStreamOutputLifetimeId = 12;
    EXPECT_TRUE(gPipeInputs.HasOpenTransformFeedbackSpan(11));
    EXPECT_TRUE(gPipeInputs.HasOpenTransformFeedbackSpan(12));
    state.StreamOutputSpans.erase(12);
    EXPECT_TRUE(gPipeInputs.HasOpenTransformFeedbackSpan(11));
    EXPECT_FALSE(gPipeInputs.HasOpenTransformFeedbackSpan(12));
    EXPECT_EQ(MGPipeFieldOwnershipOf(MGPipeInputField::HasOpenTransformFeedbackSpan),
              MGPipeFieldOwnership::kApplierDerived);
    state.StreamOutputSpans.clear();
    state.BoundStreamOutputLifetimeId = 0;
    MGPipeServerClearVerbBoundary();
}

TEST_F(FieldOwnershipTest, SplitCaptureSnapshotSurvivesMakeCurrentUntilObjectRelease) {
    RoleSplitArm guard;
    guard.Arm(true);
    auto& state = MGPipeApplier();
    state.StreamOutputSpans.clear();
    MGPStreamOutputBegin begin{};
    begin.LifetimeId = 0x100000002ull;
    begin.CaptureProgram = {7, 2};
    begin.Targets[3] = {{9, 3}, 16, 64};
    state.StreamOutputSpans[begin.LifetimeId] = begin;
    state.BoundStreamOutputLifetimeId = begin.LifetimeId;

    // Make-current resets binding state, not a live object's open capture. The
    // returning context emits context values again, but never repeats Begin.
    MGPipeApplierReset();
    MGPContextValues returning{};
    returning.BoundTransformFeedbackLifetimeId = begin.LifetimeId;
    returning.IsTransformFeedbackActive = 1;
    MGPipeApplySetContextValues(returning);
    EXPECT_EQ(state.BoundStreamOutputLifetimeId, begin.LifetimeId);
    EXPECT_TRUE(gPipeInputs.HasOpenTransformFeedbackSpan(begin.LifetimeId));
    const auto found = state.StreamOutputSpans.find(begin.LifetimeId);
    EXPECT_NE(found, state.StreamOutputSpans.end());
    if (found != state.StreamOutputSpans.end()) {
        EXPECT_EQ(found->second.CaptureProgram, begin.CaptureProgram);
        EXPECT_EQ(found->second.Targets[3].Res, begin.Targets[3].Res);
        EXPECT_EQ(found->second.Targets[3].Offset, 16u);
        EXPECT_EQ(found->second.Targets[3].Size, 64u);
    }
    MGPipeApplierReleaseObjectRecords();
    EXPECT_FALSE(gPipeInputs.HasOpenTransformFeedbackSpan(begin.LifetimeId));
    EXPECT_EQ(state.BoundStreamOutputLifetimeId, 0u);
}

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

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, AClientFreshStampCannotReviveARetiredServerGetter) {
    const ChildResult r = RunInChild([] {
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        // Simulate a regressed writer stamping the old mirror as fresh. FATAL is a
        // representation verdict, not a freshness verdict, and must still reject it.
        auto& filled = const_cast<MGPipeFilledState&>(gPipeInputs.FilledState());
        filled.FilledGen[Index(MGPipeInputField::GetBoundVertexArray)] = filled.VerbSerial;
        (void)gPipeInputs.GetBoundVertexArray();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"),
              std::string::npos) << r.Log;
}

TEST_F(FieldOwnershipTest, StrictErrorsTurnsABarrierPulledReadIntoANamedAbort) {
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
}

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, TheSameReadWithoutStrictErrorsSurvivesAndIsCounted) {
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = false;
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
}

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, AnEscalatedRecordsPullIsAdmittedAndSaysWhy) {
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeApplierSetCurrentRecordBarrieredByEscalation(true);
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
}

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, StrictErrorsReachTheStickyForwardsAndNameThemByField) {
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.ValidateProgramName(1u);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"ValidateProgramName@DrawArrays\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
}

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, AnAdmittedBarrierPullIsLoudOnceAndNotFatal) {
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        MGPipeServerStampVerbBoundary(MGPipeVerb::ReadPixels);
        (void)gPipeInputs.ValidateProgramName(1u);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"ValidateProgramName@ReadPixels\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
}

// P5f terminal contract; the historical registration name is retained for G14.
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

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, TheComputeProgramIsServedUnderADispatchStampFromP5bOn) {
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {

        MGPipeServerStampVerbBoundary(MGPipeVerb::DispatchCompute);
        (void)gPipeInputs.GetProgramForDispatch();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetProgramForDispatch@DispatchCompute\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
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

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, DualBlockMakesABarrierPulledReadANamedAbortWithoutStrict) {
    // Historical name retained for G14: the formerly admitted read is now forbidden.
    const ChildResult r = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MG_Config::Ipc.RoleSplitState = true;
        MG_Config::Ipc.StrictErrors = false;
        MGPipeServerStampVerbBoundary(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedPipeInput, \"GetBoundVertexArray@DrawArrays\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Admitted{"), std::string::npos) << r.Log;
}

// P5f terminal contract; the historical registration name is retained for G14.
TEST_F(FieldOwnershipTest, DualBlockDoesNotArmUnderMonolithTransport) {
    const ChildResult r = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::Monolith;
        MG_Config::Ipc.RoleSplitState = true;
        // A real monolith fill, not a manufactured server stamp on a monolith process.
        MGPipeValidateForVerb(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetBoundVertexArray();
        if (MGPipeRoleSplitActive() || MGPipeResidualPullCount() != 0) ::_exit(7);
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
