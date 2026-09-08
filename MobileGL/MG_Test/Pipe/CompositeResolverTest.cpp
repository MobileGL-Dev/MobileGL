// MobileGL - MobileGL/MG_Test/Pipe/CompositeResolverTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's program-pipeline COMPOSITE: GLContext::GetProgramForDraw() already flattens a pipeline
// into one hidden composite ProgramObject entirely in the frontend, so the client pushes ONE
// handle for it, allocated out of the ShaderCso reserved high band, and the server never
// learns it is a composite - it needs no "resolved draw program" hook at all.
//
// WHAT THIS SUITE IS ACTUALLY FOR: the composite's slot has TWO INDEPENDENT RELEASE PATHS -
// the pipeline cache's LRU eviction and the composite ProgramObject's own destructor - and
// both go through one client-side death helper. Either order has to free the slot exactly
// once, and the second call has to be a proven no-op rather than a lucky one. That is what the
// eviction-then-destruction pair and its mirror pin, and it is why the composite gets a leak
// case of its own beside the five ordinary kinds.
//
// THE SUITE IS `CompositeResolver`, not `CompositeResolverTest`: the file is XTest.cpp and the
// suite is X, this directory's convention.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT - the
// resolver itself, its signature-keyed cache and the two release orders are the client
// package's, and it never has to come back to MG_Test/Pipe/CMakeLists.txt.
//
// IT HAS ITS OWN main() for ResourceEmitTest's reason. Every case is a visible SKIP in a pull
// build rather than a vanishing test, so `ctest -N` stays name-for-name identical between the
// pull and the push trees.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#define MGTEST_HAVE_FORK 0
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#define MGTEST_HAVE_FORK 1
#endif

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include "Init.h"
#include <MG_Impl/GLImpl/Program/GL_Program.h>
#include <MG_Impl/GLImpl/Program/GL_ProgramPipeline.h>
#include <MG_Impl/Pipe/CompositeResolver.h>
#include <MG_Impl/Pipe/ProgramEmit.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
// create_shader_state takes the two artefact structs by pointer beside the record, so a case
// that mints a composite record needs their definitions.
#include <MG_State/GLState/ProgramState/ProgramArtifacts.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
    String g_logPath;

    int ProcessId() {
#if defined(_WIN32)
        return _getpid();
#else
        return static_cast<int>(getpid());
#endif
    }

#if MOBILEGL_PIPE_PUSH
    std::string ReadLog() {
        std::ifstream in(g_logPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // A fresh applier per case, BOTH SCOPES, and it takes both because there are two: a reset
    // is a make-current and deliberately KEEPS the object records, so a fixture that wants a
    // genuinely empty applier has to say the other one as well. Every case is its own process
    // under ctest, so this is belt and braces - but running the binary by hand must give the
    // same answers as running it under ctest.
    struct ApplierGuard {
        ApplierGuard() {
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
        }
        ~ApplierGuard() {
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
        }
    };

#if MGTEST_HAVE_FORK
    struct ChildResult {
        int Status = -1;
        std::string Log;
    };

    template <class Body>
    ChildResult RunInChild(Body body) {
        ChildResult result;
        std::error_code ec;
        std::filesystem::remove(g_logPath, ec);
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
        result.Log = ReadLog();
        return result;
    }

    Bool DiedOfAbort(const ChildResult& r) { return WIFSIGNALED(r.Status) && WTERMSIG(r.Status) == SIGABRT; }
    std::string DescribeStatus(const ChildResult& r) {
        if (r.Status < 0) return "fork/waitpid failed";
        if (WIFEXITED(r.Status)) return "exited " + std::to_string(WEXITSTATUS(r.Status));
        if (WIFSIGNALED(r.Status)) return "signal " + std::to_string(WTERMSIG(r.Status));
        return "status " + std::to_string(r.Status);
    }
#endif // MGTEST_HAVE_FORK

    // Drives a call a trip wire must REFUSE, and asserts the wire NAMED what it refused. The
    // two arms differ by design: a poison or verify build stops the process, so the drive is a
    // forked child and the parent reads SIGABRT plus the line out of the log; a shipped push
    // build logs and carries on from a defined state, so there the line is read back in process
    // and the caller goes on to assert that nothing moved.
    template <class Body>
    void ExpectRefusedNaming(const char* needle, Body body) {
#if MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY
#if MGTEST_HAVE_FORK
        const std::string tagged = std::string("Fatal{ProtocolCorruption} ") + needle;
        const ChildResult child = RunInChild(body);
        EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child) << "; log: " << child.Log;
        EXPECT_NE(child.Log.find(tagged), std::string::npos)
            << "the gate fired without naming what it refused; wanted \"" << tagged << "\"; log: " << child.Log;
#else
        (void)needle;
        (void)body; // no fork on this platform; the verdict here is std::abort()
#endif
#else
        const std::string tagged = std::string("ProtocolCorruption ") + needle;
        const std::string before = ReadLog();
        body();
        EXPECT_NE(ReadLog().substr(before.size()).find(tagged), std::string::npos)
            << "the gate refused without saying what it refused; wanted \"" << tagged << "\"";
#endif
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace

// The contract commit's one case, and it pins the property everything else in this suite is
// built on: the composite band has EXACTLY ONE DOOR. The ordinary allocator refuses the band
// for kind ShaderCso, AllocateComposite is the only way in, and a slot from one can never be
// mistaken for a slot from the other - which is what reserving a band rather than setting a
// flag on the handle buys, and what keeps the resolver's lifetime bookkeeping out of the
// ordinary program allocator.
TEST(CompositeResolver, TheCompositeBandHasExactlyOneDoor) {
#if MOBILEGL_PIPE_PUSH
    MGPipeSlotAllocator slots;

    // The ordinary door never opens onto the band, however many times it is used.
    for (int i = 0; i < 8; ++i) {
        const MGPipeHandle ordinary = slots.Allocate(MGPipeKind::ShaderCso);
        EXPECT_FALSE(MGPipeHandleIsNull(ordinary));
        EXPECT_FALSE(MGPipeIsCompositeShaderSlot(ordinary.Slot));
    }

    // The composite door only ever opens onto it, and the handle it hands out is an ORDINARY
    // ShaderCso handle in every other respect - the same kind, the same {slot, gen} rules, the
    // same Free. The server cannot tell the difference and must not be able to.
    const MGPipeHandle composite = slots.AllocateComposite(4242);
    EXPECT_FALSE(MGPipeHandleIsNull(composite));
    EXPECT_TRUE(MGPipeIsCompositeShaderSlot(composite.Slot));
    EXPECT_TRUE(slots.IsLive(MGPipeKind::ShaderCso, composite));
    EXPECT_EQ(slots.FindByLifetimeId(MGPipeKind::ShaderCso, 4242), composite);

    // TWO RELEASE PATHS, ONE FREE. The second call resolves the same handle at a generation
    // the slot no longer has, and Free refuses it - which is what makes "the pipeline cache
    // evicted it and then the composite's destructor ran" safe in either order rather than a
    // double free that only shows up as slot theft much later.
    const Uint32 liveBefore = slots.LiveCount(MGPipeKind::ShaderCso);
    slots.Free(MGPipeKind::ShaderCso, composite);
    slots.Free(MGPipeKind::ShaderCso, composite);
    EXPECT_EQ(slots.LiveCount(MGPipeKind::ShaderCso), liveBefore - 1);
    EXPECT_FALSE(slots.IsLive(MGPipeKind::ShaderCso, composite));

    // And the slot really goes back to the band rather than to the ordinary free list: the
    // next composite reuses it with a bumped generation, and no ordinary program can be handed
    // it.
    const MGPipeHandle recycled = slots.AllocateComposite(4343);
    EXPECT_EQ(recycled.Slot, composite.Slot);
    EXPECT_NE(recycled.Gen, composite.Gen);
    EXPECT_FALSE(MGPipeIsCompositeShaderSlot(slots.Allocate(MGPipeKind::ShaderCso).Slot));
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client slot allocator in a pull build";
#endif
}

// =========================================================================================
// The APPLIER's half of the composite band (the wire commits'). The client-side resolver - the
// signature cache keyed on ComputeDrawProgramSignature, the two release paths, the eviction -
// is the client package's and lands beside these.
//
// WHY THE APPLIER HAS A BAND AT ALL. It is not because the server knows what a composite is:
// it does not, and create / bind / delete_shader_state name one exactly as they name any other
// program. It is because the band starts at 983040, so ONE pipeline composite in a
// slot-indexed vector would grow that vector to ~983k records of ~240 bytes each - a 236 MB
// spike on the first pipeline draw. Both spaces stay dense against their own high-water mark.
// =========================================================================================

#if MOBILEGL_PIPE_PUSH
namespace {
    using MG_State::GLState::LinkArtifacts;
    using MG_State::GLState::SpirvArtifacts;

    MGPProgramDesc CompositeDesc(MGPipeHandle cso, Uint32 stageMask) {
        MGPProgramDesc desc{};
        desc.Cso = cso;
        desc.StageMask = stageMask;
        return desc;
    }

    MGPHandleOnly ProgramHandle(MGPipeHandle cso) {
        return MGPHandleOnly{cso, static_cast<Uint32>(MGPipeKind::ShaderCso), 0};
    }
} // namespace
#endif

// The band's record lands in the band's own table and the ordinary one is not grown by it -
// which is the whole 236 MB of it - and every entry point still names it as an ordinary
// program.
TEST(CompositeResolver, ACompositeRecordLandsInTheBandsOwnTableAndNeverGrowsTheOrdinaryOne) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;
    const MGPipeHandle composite{kMGPipeShaderCsoCompositeSlotBase + 2, 1};
    ASSERT_TRUE(MGPipeIsCompositeShaderSlot(composite.Slot));

    MGPipeApplyCreateShaderState(CompositeDesc(composite, 0x3u), &link, &spirv);
    EXPECT_TRUE(MGPipeApplier().ShaderCsos.empty())
        << "one composite grew the ordinary table to the band's base - that is the 236 MB spike";
    ASSERT_EQ(MGPipeApplier().CompositeShaderCsos.size(), 3u)
        << "the band's table is indexed by (slot - base) and stays dense against its own high water";
    EXPECT_TRUE(MGPipeApplier().CompositeShaderCsos[2].Live);
    EXPECT_EQ(MGPipeApplier().CompositeShaderCsos[2].Gen, 1u);
    EXPECT_EQ(MGPipeApplier().CompositeShaderCsos[2].Desc.StageMask, 0x3u);

    // AND THE SERVER NEVER LEARNS IT IS A COMPOSITE: the ordinary bind and draw-program calls
    // resolve it exactly as they resolve any other program.
    MGPipeApplyBindShaderState(ProgramHandle(composite));
    MGPipeApplySetDrawProgram(ProgramHandle(composite));
    EXPECT_EQ(MGPipeApplier().BoundShaderCso, composite);
    EXPECT_EQ(MGPipeApplier().DrawProgram, composite);
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u);

    // An ordinary program lands in the other table, and the two do not see each other even
    // though the composite's record is at index 2 of its own.
    MGPipeApplyCreateShaderState(CompositeDesc(MGPipeHandle{2, 1}, 0x7u), &link, &spirv);
    ASSERT_GT(MGPipeApplier().ShaderCsos.size(), 2u);
    EXPECT_EQ(MGPipeApplier().ShaderCsos[2].Desc.StageMask, 0x7u);
    EXPECT_EQ(MGPipeApplier().CompositeShaderCsos[2].Desc.StageMask, 0x3u)
        << "an ordinary program at slot 2 wrote the composite at band index 2";
#endif
}

// The composite's slot has TWO independent release paths - the pipeline cache's eviction and
// the composite program's own destructor - and both go through one client helper. The second
// arrival here is a refused no-op, which is what makes the double free proven rather than
// assumed, and it clears the bindings exactly once.
TEST(CompositeResolver, ASecondDeleteOfACompositeIsARefusedNoOpRatherThanASecondRelease) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;
    const MGPipeHandle composite{kMGPipeShaderCsoCompositeSlotBase, 3};

    MGPipeApplyCreateShaderState(CompositeDesc(composite, 0x3u), &link, &spirv);
    MGPipeApplySetDrawProgram(ProgramHandle(composite));
    ASSERT_EQ(MGPipeApplier().DrawProgram, composite);

    MGPipeApplyDeleteShaderState(ProgramHandle(composite));
    EXPECT_FALSE(MGPipeApplier().CompositeShaderCsos[0].Live);
    EXPECT_EQ(MGPipeApplier().CompositeShaderCsos[0].Gen, 3u);
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().DrawProgram));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u);
    const Uint64 serialAfterFirst = MGPipeApplier().ProgramBindingSerial;

    MGPipeApplyDeleteShaderState(ProgramHandle(composite));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 1u);
    EXPECT_EQ(MGPipeApplier().ProgramBindingSerial, serialAfterFirst)
        << "the second release moved the binding serial, so it was not a no-op";

    // And the band's slot is re-usable afterwards: a recycled composite is a new identity and
    // starts its record over.
    MGPipeApplyCreateShaderState(CompositeDesc(MGPipeHandle{composite.Slot, 4}, 0x1u), &link, &spirv);
    EXPECT_TRUE(MGPipeApplier().CompositeShaderCsos[0].Live);
    EXPECT_EQ(MGPipeApplier().CompositeShaderCsos[0].Gen, 4u);
    EXPECT_EQ(MGPipeApplier().CompositeShaderCsos[0].Serial, 0u);
#endif
}

// The band is INSIDE the ShaderCso slot limit, so the bound the applier refuses at is the limit
// itself and not the band's base - a bound below it would refuse the very slots the allocator's
// one composite door is allowed to hand out.
TEST(CompositeResolver, ASlotAtTheShaderCsoLimitIsRefusedWhileTheLastBandSlotIsNot) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;

    // The positive control: the LAST slot of the band is a legal composite handle.
    const MGPipeHandle last{kMGPipeShaderCsoSlotLimit - 1, 1};
    ASSERT_TRUE(MGPipeIsCompositeShaderSlot(last.Slot));
    MGPipeApplyCreateShaderState(CompositeDesc(last, 0x3u), &link, &spirv);
    ASSERT_EQ(MGPipeApplier().CompositeShaderCsos.size(),
              static_cast<SizeT>(kMGPipeShaderCsoSlotLimit - kMGPipeShaderCsoCompositeSlotBase));
    EXPECT_TRUE(MGPipeApplier().CompositeShaderCsos.back().Live);
    EXPECT_TRUE(MGPipeApplier().ShaderCsos.empty());

    const MGPProgramDesc past = CompositeDesc(MGPipeHandle{kMGPipeShaderCsoSlotLimit, 1}, 0x3u);
    ExpectRefusedNaming("create_shader_state {slot=1048576, gen=1}: the slot is outside the record table's "
                        "bound",
                        [&past, &link, &spirv]() { MGPipeApplyCreateShaderState(past, &link, &spirv); });

    // And an ORDINARY slot at or above the band's base is out of range by definition: the
    // allocator refuses the band for an ordinary program, so nothing legal can name one.
    MGPipeApplySetDrawProgram(ProgramHandle(MGPipeHandle{kMGPipeShaderCsoCompositeSlotBase - 1, 1}));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 1u)
        << "an ordinary slot below the band resolved against a record nobody created";
#endif
}

#if !MOBILEGL_PIPE_PUSH
// G2 requires the pull and push ctest name sets to be identical, name for name.
#define MGL_COMPOSITE_RESOLVER_TEST_LIST(X)                                                        \
    X(CompositeResolver, ACompositeIsMintedFromTheReservedBand)                                     \
    X(CompositeResolver, ASignatureThatHasNotMovedReusesOneComposite)                               \
    X(CompositeResolver, TwoPipelinesWithTheSameSignatureKeepTheirOwnComposite)                     \
    X(CompositeResolver, EvictionThenDestructionFreesTheSlotExactlyOnce)                            \
    X(CompositeResolver, DestructionThenEvictionFreesTheSlotExactlyOnce)                            \
    X(CompositeResolver, ASignatureMoveAfterAMakeCurrentStillReleasesThroughTheResolver)         \
    X(CompositeResolver, TwoContextsHoldingOnePipelineNameKeepTheirOwnComposites)                \
    X(CompositeResolver, ADestroyedContextsEntryIsDroppedRatherThanReleasedASecondTime)

#define MGL_DECLARE_PULL_SKIP(Suite, Name)                                                         \
    TEST(Suite, Name) { GTEST_SKIP() << "compiled only under MOBILEGL_PIPE_PUSH"; }
MGL_COMPOSITE_RESOLVER_TEST_LIST(MGL_DECLARE_PULL_SKIP)
#undef MGL_DECLARE_PULL_SKIP
#else

namespace {
    namespace GL = MobileGL::MG_Impl::GLImpl;
    using GLContext = MG_State::GLState::GLContext;
    using MG_State::GLState::ProgramObject;
    using MG_State::GLState::ProgramPipelineObject;

    struct ResolverScope {
        ResolverScope() { Clear(); }
        ~ResolverScope() {
            GL::BindProgramPipeline(0);
            GL::UseProgram(0);
            Clear();
        }
        ResolverScope(const ResolverScope&) = delete;
        ResolverScope& operator=(const ResolverScope&) = delete;

        static void Clear() {
            MGPipeProgramEmitterInstance().Reset();
            MGPipeProgramEmitterInstance().ResetCounters();
            MGPipeCompositeResolverInstance().ResetCounters();
        }
    };

    GLContext& Ctx() { return *MG_State::pGLContext; }

    const char* kVs = R"(#version 430 core
out gl_PerVertex { vec4 gl_Position; };
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";
    const char* kFs = R"(#version 430 core
out vec4 o_color;
void main() { o_color = vec4(1.0); }
)";
    // A SECOND fragment stage, so a pipeline's draw-program signature can be made to move for
    // real: ComputeDrawProgramSignature is the per-stage {lifetimeId, GetLinkVersion()} array,
    // and a different ProgramObject is a different lifetime id.
    const char* kFs2 = R"(#version 430 core
out vec4 o_color;
void main() { o_color = vec4(0.5); }
)";

    // Built by hand rather than through glCreateShaderProgramv, for ProgramPipelineCompositeTest's
    // reason: that entry point detaches the shader right after linking, so a relink would leave
    // the stage program with nothing to composite from.
    GLuint MakeSeparableProgram(GLenum stage, const char* source) {
        const GLuint shader = GL::CreateShader(stage);
        GL::ShaderSource(shader, 1, &source, nullptr);
        GL::CompileShader(shader);
        const GLuint program = GL::CreateProgram();
        GL::ProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
        GL::AttachShader(program, shader);
        GL::LinkProgram(program);
        GLint linked = GL_FALSE;
        GL::GetProgramiv(program, GL_LINK_STATUS, &linked);
        EXPECT_EQ(linked, GL_TRUE) << "separable stage program did not link";
        return program;
    }

    GLuint MakeBoundPipeline(GLuint vs, GLuint fs) {
        GLuint pipeline = 0;
        GL::GenProgramPipelines(1, &pipeline);
        GL::BindProgramPipeline(pipeline);
        GL::UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
        GL::UseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);
        GL::UseProgram(0);
        return pipeline;
    }

    TEST(CompositeResolver, ACompositeIsMintedFromTheReservedBand) {
        ResolverScope scope;
        const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kVs);
        const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kFs);
        MakeBoundPipeline(vs, fs);

        const SharedPtr<ProgramObject> composite = Ctx().GetProgramForDraw();
        ASSERT_TRUE(composite) << "the frontend has to flatten the pipeline for this to mean anything";
        // The composite is the one ProgramObject in the system with external index 0: it is
        // deliberately not a named program, must not answer glIsProgram and must not consume a
        // name, and glCreateProgram never returns 0.
        EXPECT_TRUE(MGPipeProgramIsPipelineComposite(*composite));
        EXPECT_EQ(composite->GetExternalIndex(), 0u);

        ASSERT_GT(MGPipeProgramEmitterInstance().EmitShaderState(Ctx()), 0u);
        const MGPipeHandle cso = MGPipeProgramEmitterInstance().DrawCso();
        ASSERT_FALSE(MGPipeHandleIsNull(cso));
        EXPECT_TRUE(MGPipeIsCompositeShaderSlot(cso.Slot))
            << "a composite's slot comes out of the reserved band and nowhere else";
        // AND IT IS AN ORDINARY create_shader_state. The server never learns it is a composite.
        EXPECT_EQ(MGPipeProgramEmitterInstance().LastProgramDesc().Cso, cso);
        EXPECT_TRUE(MGPipeProgramEmitterInstance().RecordIsPublished(cso));
        EXPECT_EQ(MGPipeCompositeResolverInstance().GetCounters().Mints, 1u);
    }

    TEST(CompositeResolver, ASignatureThatHasNotMovedReusesOneComposite) {
        ResolverScope scope;
        const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kVs);
        const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kFs);
        MakeBoundPipeline(vs, fs);

        ASSERT_GT(MGPipeProgramEmitterInstance().EmitShaderState(Ctx()), 0u);
        const MGPipeHandle first = MGPipeProgramEmitterInstance().DrawCso();
        ASSERT_FALSE(MGPipeHandleIsNull(first));
        ASSERT_EQ(MGPipeProgramEmitterInstance().CreateCount(), 1u);

        // The stage set did not move, so the frontend hands back the cached composite and the
        // resolver reuses its handle - no second identity, no second record, and nothing
        // released. THE KEY IS ComputeDrawProgramSignature's {lifetimeId, linkVersion} array
        // and deliberately NOT GetBackendStateVersion, which a glUniform1i to a sampler moves
        // and which used to rebuild the composite on every draw.
        MGPipeProgramEmitterInstance().EmitShaderState(Ctx());
        EXPECT_EQ(MGPipeProgramEmitterInstance().DrawCso(), first);
        EXPECT_EQ(MGPipeProgramEmitterInstance().CreateCount(), 1u);
        EXPECT_EQ(MGPipeCompositeResolverInstance().GetCounters().Releases, 0u);
        EXPECT_GE(MGPipeCompositeResolverInstance().GetCounters().Reuses, 1u);
    }

    // [deviation] The brief names this case "TwoPipelinesWithTheSameSignatureShareOneComposite".
    // Sharing one HANDLE between two pipeline objects is not implementable safely and the
    // property that is true is the opposite one, so the case is named for what it asserts.
    //
    // The reason is the death path: a composite is an ordinary ProgramObject with its OWN
    // lifetime id, and the client-side death helper resolves the handle FROM that lifetime id.
    // Two composites sharing one handle would put only one of the two ids in the allocator's
    // map, so the first ~ProgramObject would free a slot the second still names - a premature
    // free that reappears later as slot theft, which is the exact class this whole band exists
    // to prevent. The frontend does not share either: each ProgramPipelineObject carries its
    // own one-slot draw-program cache, so two pipeline objects with identical stage sets are
    // two composites in the frontend too.
    TEST(CompositeResolver, TwoPipelinesWithTheSameSignatureKeepTheirOwnComposite) {
        ResolverScope scope;
        const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kVs);
        const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kFs);

        MakeBoundPipeline(vs, fs);
        ASSERT_GT(MGPipeProgramEmitterInstance().EmitShaderState(Ctx()), 0u);
        const MGPipeHandle firstCso = MGPipeProgramEmitterInstance().DrawCso();
        ASSERT_FALSE(MGPipeHandleIsNull(firstCso));

        MakeBoundPipeline(vs, fs); // a SECOND pipeline object, the same stage set
        MGPipeProgramEmitterInstance().EmitShaderState(Ctx());
        const MGPipeHandle secondCso = MGPipeProgramEmitterInstance().DrawCso();
        ASSERT_FALSE(MGPipeHandleIsNull(secondCso));

        EXPECT_NE(firstCso, secondCso);
        EXPECT_TRUE(MGPipeIsCompositeShaderSlot(firstCso.Slot));
        EXPECT_TRUE(MGPipeIsCompositeShaderSlot(secondCso.Slot));
        // Both are still resolvable, which is the property a shared handle would have broken.
        EXPECT_TRUE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, firstCso));
        EXPECT_TRUE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, secondCso));
    }

    // THE TWO RELEASE ORDERS, driven at the level where both of them are representable. Either
    // order frees the slot exactly once and the second call is a PROVEN no-op, because
    // MGPipeSlotAllocator::Free refuses a slot that is not live at that generation and bumps no
    // generation of its own - so a double release cannot skip a generation either.
    TEST(CompositeResolver, EvictionThenDestructionFreesTheSlotExactlyOnce) {
        ResolverScope scope;
        constexpr Uint64 kCompositeLifetimeId = 918273645ull;
        const Uint32 liveBefore = MGPipeSlots().LiveCount(MGPipeKind::ShaderCso);
        const MGPipeHandle cso = MGPipeSlots().AllocateComposite(kCompositeLifetimeId);
        ASSERT_FALSE(MGPipeHandleIsNull(cso));
        ASSERT_TRUE(MGPipeIsCompositeShaderSlot(cso.Slot));
        ASSERT_EQ(MGPipeSlots().LiveCount(MGPipeKind::ShaderCso), liveBefore + 1);

        // 1. the pipeline cache drops it
        MGPipeEmitShaderCsoDestroyAndFree(kCompositeLifetimeId);
        EXPECT_FALSE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, cso));
        EXPECT_EQ(MGPipeSlots().LiveCount(MGPipeKind::ShaderCso), liveBefore);
        // 2. and then ~ProgramObject runs, and finds nothing to do
        MGPipeEmitShaderCsoDestroyAndFree(kCompositeLifetimeId);
        EXPECT_EQ(MGPipeSlots().LiveCount(MGPipeKind::ShaderCso), liveBefore);
    }

    TEST(CompositeResolver, DestructionThenEvictionFreesTheSlotExactlyOnce) {
        ResolverScope scope;
        constexpr Uint64 kCompositeLifetimeId = 918273646ull;
        const Uint32 liveBefore = MGPipeSlots().LiveCount(MGPipeKind::ShaderCso);
        const MGPipeHandle cso = MGPipeSlots().AllocateComposite(kCompositeLifetimeId);
        ASSERT_FALSE(MGPipeHandleIsNull(cso));

        // The mirror order, and it is the one that actually happens today: the frontend's
        // one-slot cache drops its SharedPtr as it overwrites it, so ~ProgramObject usually
        // runs first and the resolver's release is the second, redundant path.
        MGPipeEmitShaderCsoDestroyAndFree(kCompositeLifetimeId);
        EXPECT_FALSE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, cso));
        MGPipeEmitShaderCsoDestroyAndFree(kCompositeLifetimeId);
        EXPECT_EQ(MGPipeSlots().LiveCount(MGPipeKind::ShaderCso), liveBefore);

        // And the slot really came back: the next composite is handed the same slot with a
        // bumped generation, so the stale handle can never resolve to it.
        const MGPipeHandle recycled = MGPipeSlots().AllocateComposite(kCompositeLifetimeId + 1);
        EXPECT_EQ(recycled.Slot, cso.Slot);
        EXPECT_NE(recycled.Gen, cso.Gen);
        MGPipeSlots().Free(MGPipeKind::ShaderCso, recycled);
    }

    // THE RELEASE PATH THE TWO CASES ABOVE DO NOT TOUCH. Both of them call the death helper
    // directly, so the resolver is not in the picture at all and its own release - the
    // pipeline-cache path, the one the header says exists precisely because "usually" is not a
    // contract - had no case of its own. This drives it, and it drives it AFTER A
    // MAKE-CURRENT, which is where it used to be permanently disarmed:
    //
    //   Reset() cleared the entry's one flag; the next Observe took the reuse branch and
    //   returned before anything could restore it; from then on ReleaseEntry saw !Live and
    //   returned immediately - no delete_shader_state, no Free, no counter movement - and the
    //   old composite's handle was simply overwritten out of the resolver. Nothing leaked
    //   today only because the frontend's one-slot pipeline cache drops the last SharedPtr on
    //   the overwrite, which is exactly the "a client that only reacted to destructors" case
    //   the design refuses to rely on.
    //
    // The release obligation now lives in its own flag and a make-current does not touch it.
    TEST(CompositeResolver, ASignatureMoveAfterAMakeCurrentStillReleasesThroughTheResolver) {
        ResolverScope scope;
        const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kVs);
        const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kFs);
        const GLuint pipeline = MakeBoundPipeline(vs, fs);

        ASSERT_GT(MGPipeProgramEmitterInstance().EmitShaderState(Ctx()), 0u);
        const MGPipeHandle first = MGPipeProgramEmitterInstance().DrawCso();
        ASSERT_FALSE(MGPipeHandleIsNull(first));
        ASSERT_TRUE(MGPipeIsCompositeShaderSlot(first.Slot));
        ASSERT_TRUE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, first));
        ASSERT_EQ(MGPipeCompositeResolverInstance().GetCounters().Releases, 0u);

        // THE MAKE-CURRENT. This is exactly what PipeFill's FreshlyPrimed arm does, and it
        // reaches MGPipeCompositeResolver::Reset() through the program emitter's own Reset().
        MGPipeProgramEmitterInstance().Reset();
        // ... followed by a draw whose stage set has NOT moved, which is the reuse branch.
        MGPipeProgramEmitterInstance().EmitShaderState(Ctx());
        EXPECT_EQ(MGPipeProgramEmitterInstance().DrawCso(), first)
            << "the same stage set is the same composite and the same handle";
        EXPECT_GE(MGPipeCompositeResolverInstance().GetCounters().Reuses, 1u);
        EXPECT_EQ(MGPipeCompositeResolverInstance().GetCounters().Releases, 0u)
            << "a reuse releases nothing";

        // NOW THE STAGE SET REALLY MOVES: a different fragment stage program is a different
        // lifetime id, so ComputeDrawProgramSignature moves and the frontend builds a second
        // composite. The resolver has to speak the release for the first one.
        const GLuint fs2 = MakeSeparableProgram(GL_FRAGMENT_SHADER, kFs2);
        GL::UseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs2);
        GL::UseProgram(0);

        MGPipeProgramEmitterInstance().EmitShaderState(Ctx());
        const MGPipeHandle second = MGPipeProgramEmitterInstance().DrawCso();
        ASSERT_FALSE(MGPipeHandleIsNull(second));
        EXPECT_NE(second, first) << "a moved signature is a second composite with its own handle";
        EXPECT_EQ(MGPipeCompositeResolverInstance().GetCounters().Releases, 1u)
            << "the resolver's own release path, spoken after a make-current";
        EXPECT_FALSE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, first))
            << "and the first composite's slot really went back, exactly once";
        EXPECT_TRUE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, second));
    }

    // THE RESOLVER IS A PROCESS SINGLETON AND A PIPELINE's GL NAME IS PER CONTEXT (C2-M1).
    // GLContext owns m_programPipelines AND its own name generator m_programPipelineNames, so
    // name N names two different ProgramPipelineObjects in two contexts, each with its own
    // composite and its own handle. Keyed on the name alone, the first Observe after a
    // make-current found the OTHER context's entry: the signature matched - two composites of
    // the same stage set have the same signature, and here two default pipelines have the same
    // all-zero one - while the handle could not, because two composites are two ProgramObjects
    // with two lifetime ids. So it fell into ReleaseEntry and emitted delete_shader_state for a
    // LIVE composite, cleared its publication latch and handed its band slot back while the
    // frontend ProgramObject was still alive.
    //
    // DRIVEN AT THE RESOLVER RATHER THAN THROUGH GL, because one test process has one
    // GLContext. Everything the case supplies is what the single production call site supplies:
    // the context id is GLContext::GetTextureContextId()'s value, the two same-named pipeline
    // objects are what two contexts hold, and the composites are ordinary ProgramObjects at
    // external index 0 out of the reserved band, exactly as Core.cpp builds them.
    TEST(CompositeResolver, TwoContextsHoldingOnePipelineNameKeepTheirOwnComposites) {
        ResolverScope scope;
        auto& resolver = MGPipeCompositeResolverInstance();
        const LinkArtifacts link;
        const SpirvArtifacts spirv;

        constexpr Uint kSharedPipelineName = 9u;
        constexpr Uint64 kContextA = 0x51A00001ull;
        constexpr Uint64 kContextB = 0x51A00002ull;
        const ProgramPipelineObject pipelineA{kSharedPipelineName};
        const ProgramPipelineObject pipelineB{kSharedPipelineName};
        const SharedPtr<ProgramObject> compositeA = MakeShared<ProgramObject>(0u);
        const SharedPtr<ProgramObject> compositeB = MakeShared<ProgramObject>(0u);
        ASSERT_TRUE(MGPipeProgramIsPipelineComposite(*compositeA));
        ASSERT_EQ(pipelineA.GetExternalIndex(), pipelineB.GetExternalIndex());

        const MGPipeHandle handleA = MGPipeSlots().AllocateComposite(compositeA->GetLifetimeId());
        const MGPipeHandle handleB = MGPipeSlots().AllocateComposite(compositeB->GetLifetimeId());
        ASSERT_NE(handleA, handleB);
        for (const MGPipeHandle handle : {handleA, handleB}) {
            MGPipeApplyCreateShaderState(CompositeDesc(handle, 0x3u), &link, &spirv);
            MGPipeNoteHandlePublished(MGPipeKind::ShaderCso, handle);
        }
        const Uint64 releasesBefore = resolver.GetCounters().Releases;

        EXPECT_EQ(resolver.Observe(kContextA, pipelineA, *compositeA, handleA), handleA);
        // THE MAKE-CURRENT, which is what PipeFill's FreshlyPrimed arm reaches through the
        // program emitter's own Reset()...
        MGPipeProgramEmitterInstance().Reset();
        // ... and then context B draws with ITS pipeline of the same name.
        EXPECT_EQ(resolver.Observe(kContextB, pipelineB, *compositeB, handleB), handleB);

        EXPECT_EQ(resolver.GetCounters().Releases, releasesBefore)
            << "the other context's entry was released - it is not this pipeline's entry";
        EXPECT_TRUE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, handleA))
            << "A's band slot went back while A's composite ProgramObject was still alive";
        EXPECT_TRUE(MGPipeHandleIsPublished(MGPipeKind::ShaderCso, handleA))
            << "delete_shader_state went out for a live composite and cleared its latch";
        EXPECT_TRUE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, handleB));

        // AND BACK TO A. Its obligation stayed owed to its own context, so the unmoved
        // signature is a reuse of the same handle and still nothing is released.
        const Uint64 reusesBefore = resolver.GetCounters().Reuses;
        MGPipeProgramEmitterInstance().Reset();
        EXPECT_EQ(resolver.Observe(kContextA, pipelineA, *compositeA, handleA), handleA);
        EXPECT_EQ(resolver.GetCounters().Reuses, reusesBefore + 1u);
        EXPECT_EQ(resolver.GetCounters().Releases, releasesBefore);
    }

    // WHERE A DESTROYED CONTEXT's ENTRIES ARE RELEASED, and it is not in the resolver. The
    // context's death drops m_programPipelines, which drops the ProgramPipelineObject, which
    // drops the composite it cached; ~ProgramObject then runs the one client-side death helper
    // and the band slot goes back EXACTLY ONCE. The resolver speaks no second delete - the
    // entry can never be found again, and the allocator has erased the lifetime-id mapping
    // anyway - and the next make-current DROPS the stranded entry, which is what keeps the
    // vector bounded now that its key carries the context.
    TEST(CompositeResolver, ADestroyedContextsEntryIsDroppedRatherThanReleasedASecondTime) {
        ResolverScope scope;
        auto& resolver = MGPipeCompositeResolverInstance();
        const LinkArtifacts link;
        const SpirvArtifacts spirv;

        constexpr Uint kPipelineName = 11u;
        constexpr Uint64 kDoomedContext = 0x51A00003ull;
        // ResolverScope's Clear() has already run one Reset(), so every entry standing here has
        // a live composite slot and nothing but this case's own entry can be swept below.
        const SizeT sizeBefore = resolver.Size();
        const Uint32 liveBefore = MGPipeSlots().LiveCount(MGPipeKind::ShaderCso);

        const ProgramPipelineObject pipeline{kPipelineName};
        SharedPtr<ProgramObject> composite = MakeShared<ProgramObject>(0u);
        const MGPipeHandle handle = MGPipeSlots().AllocateComposite(composite->GetLifetimeId());
        MGPipeApplyCreateShaderState(CompositeDesc(handle, 0x3u), &link, &spirv);
        MGPipeNoteHandlePublished(MGPipeKind::ShaderCso, handle);
        ASSERT_EQ(resolver.Observe(kDoomedContext, pipeline, *composite, handle), handle);
        ASSERT_EQ(resolver.Size(), sizeBefore + 1u);
        const Uint64 releasesBefore = resolver.GetCounters().Releases;
        const Uint64 sweepsBefore = resolver.GetCounters().Sweeps;

        // THE CONTEXT DIES: the last SharedPtr to its composite goes with its pipeline.
        composite.reset();
        EXPECT_FALSE(MGPipeSlots().IsLive(MGPipeKind::ShaderCso, handle))
            << "~ProgramObject is the release path for this entry and it freed the slot";
        EXPECT_FALSE(MGPipeHandleIsPublished(MGPipeKind::ShaderCso, handle))
            << "and it took the publication latch with the delete";
        EXPECT_EQ(MGPipeSlots().LiveCount(MGPipeKind::ShaderCso), liveBefore) << "exactly once";
        EXPECT_EQ(resolver.GetCounters().Releases, releasesBefore)
            << "the resolver spoke no release of its own for it";

        // THE NEXT CONTEXT's FIRST VERB. The stranded entry is dropped, not released.
        MGPipeProgramEmitterInstance().Reset();
        EXPECT_EQ(resolver.GetCounters().Sweeps, sweepsBefore + 1u);
        EXPECT_EQ(resolver.GetCounters().Releases, releasesBefore)
            << "a dropped entry emits nothing and frees nothing - the obligation was discharged";
        EXPECT_EQ(resolver.Size(), sizeBefore)
            << "the vector is bounded by the pairs whose composite slot is actually live";
    }
} // namespace
#endif // MOBILEGL_PIPE_PUSH

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-compositeresolver-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
#if MOBILEGL_PIPE_PUSH
    MobileGL::Initialize();
#endif
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
