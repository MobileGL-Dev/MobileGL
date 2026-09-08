// MobileGL - MobileGL/MG_Test/Pipe/FramebufferEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's framebuffer family: set_framebuffer_state, emitted per bound target, on both sides of
// the call.
//
// THIS SUITE IS A NAMED GATE. The phase's descriptor-consistency gate is "for every framebuffer
// configuration the emitted MGPFramebufferState reproduces exactly the values the backend's
// SyncToBackend family reads from the frontend today, field by field", and it is spelled
// `ctest -R 'FramebufferEmit\.'`; its negative control is a script that stops the conversion
// copying ONE field (MGPSurface::Layered) and expects this suite to go red NAMING that field.
// So a case here must fail by field name, never by a bare count, or the control cannot answer.
//
// THE SUITE IS `FramebufferEmit`, not `FramebufferEmitTest`: the file is XTest.cpp and the
// suite is X, this directory's convention, and it is what the gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT. The
// applier-side cases (a record's lifecycle, the per-target storage, what a make-current does
// and does not clear) are the wire commits'; the emitter-side cases (the resolved read
// surface, the draw-buffer array in the content hash, a recycled handle never suppressed
// against its predecessor, every attachment field surviving the surface conversion, an
// attachment point above the wire width refused rather than truncated, a re-storaged attached
// renderbuffer publishing its new extent) are the client package's - and neither of them has
// to come back to MG_Test/Pipe/CMakeLists.txt to add one.
//
// IT HAS ITS OWN main() for the same reason ResourceEmitTest and VertexInputEmitTest do: the
// applier's bounds and protocol trip wires report through a log line in a shipped push build
// and std::abort() in a poison or verify one, so a case that drives one reads the line back
// out of a file this process names before anything logs.
//
// Every case is a visible SKIP in a pull build rather than a vanishing test, so `ctest -N`
// stays name-for-name identical between the pull and the push trees.

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
#include <MG_Impl/Pipe/FramebufferEmit.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_State/GLState/RenderbufferState/RenderbufferObject.h>
#include <MG_State/GLState/TextureState/TextureObject2D.h>
#include <MG_State/GLState/TextureState/TextureObject2DCube.h>

#include <algorithm>
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

    // A record with real values in every field a case might read back, so a body that stored
    // the wrong one - or stored nothing - is visible BY FIELD.
    MGPFramebufferState FramebufferRecord(MGPipeHandle fbo, MGPipeFramebufferTarget target, Uint16 width) {
        MGPFramebufferState state{};
        state.Fbo = fbo;
        state.Target = static_cast<Uint8>(target);
        state.Width = width;
        state.Height = 64;
        state.Layers = 1;
        state.Samples = 1;
        state.Complete = 1;
        state.ContentHash = 0x1234u + width;
        for (Uint32 i = 0; i < kMGPipeMaxColorAttachments; ++i) {
            state.DrawBuffers[i] = static_cast<Int8>(i == 0 ? 0 : -1);
        }
        state.Color[0].Res = MGPipeHandle{9, 1};
        state.Color[0].InternalFormat = 0x8058u; // GL_RGBA8
        state.Color[0].Kind = 1;
        return state;
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace

// The one case the contract commit lands, and it is not a placeholder: it pins the SHAPE every
// later case depends on. The emitter is a process singleton that is heap-constructed and
// intentionally leaked, because a static holding client state whose destructor an exit handler
// can run is the exit-order use-after-free this design closed once already - `exit` runs the
// frontend's own teardown into a pipe whose allocator has already been destroyed. One
// allocation for the life of the process, no destructor to lose.
TEST(FramebufferEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeFramebufferEmitterInstance(), &MGPipeFramebufferEmitterInstance());
    // And the family's wired-subsystem constant is either 0 or its own bit and nothing else.
    // It is 0 until this family's emitter has a body; the OR in PipeFill.cpp is what turns it
    // into the switch, so a header that set the wrong bit would switch the wrong family on.
    EXPECT_TRUE(kMGPipeWiredFramebufferSubsystem == 0 ||
                kMGPipeWiredFramebufferSubsystem == kMGPipeSubsystemFramebuffer);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

// ============================================================================
// P4a package B's EMITTER-SIDE cases. The contract commit landed the file, its ctest
// registration and one shape pin; the wire package's applier-side cases and these are disjoint
// TEST bodies in one file, and a collision between them is resolved by UNION, never by
// choosing a side.
//
// EVERY CASE FAILS BY FIELD NAME, never by a bare count: G7's scripted control stops the
// conversion copying exactly one member (MGPSurface::Layered) and expects this suite to go red
// NAMING that field, and a case that reported only "the records differ" could not answer it.
// ============================================================================
#if !MOBILEGL_PIPE_PUSH
#define MGL_FRAMEBUFFER_EMIT_CLIENT_TEST_LIST(X)                                                   \
    X(FramebufferEmit, EveryRecordsReadSurfaceComesFromItsOwnFramebuffersReadBuffer)               \
    X(FramebufferEmit, OneObjectBoundToBothTargetsEmitsOneRecordWithTargetBoth)                    \
    X(FramebufferEmit, ADrawBufferChangeAloneStillMovesTheContentHash)                             \
    X(FramebufferEmit, ARecycledFramebufferHandleIsNeverSuppressedAgainstItsPredecessor)           \
    X(FramebufferEmit, EveryAttachmentFieldSurvivesTheSurfaceConversion)                           \
    X(FramebufferEmit, AnAttachmentPointAboveTheWireWidthIsRefusedNotTruncated)                    \
    X(FramebufferEmit, ARestoragedAttachedRenderbufferPublishesItsNewExtent)                       \
    X(FramebufferEmit, AnUnchangedBindingPairEmitsNothing)                                         \
    X(FramebufferEmit, AFramebufferHandedOverByNameGetsANamedRecordWithoutMovingABinding)          \
    X(FramebufferEmit, ANamedRecordIsSuppressedPerObjectAndNeverAgainstABoundRecord)               \
    X(FramebufferEmit, ADrawBufferTokenAboveTheWireWidthIsRefusedNotTruncated)                     \
    X(FramebufferEmit, ALayeredCubeAttachmentDoesNotAssertAFaceItCannotKnow)                       \
    X(FramebufferEmit, EveryNonTexturePointCarriesTheUnknownSentinelsRatherThanZero)

#define MGL_DECLARE_PULL_SKIP(Suite, Name)                                                         \
    TEST(Suite, Name) { GTEST_SKIP() << "compiled only under MOBILEGL_PIPE_PUSH"; }
MGL_FRAMEBUFFER_EMIT_CLIENT_TEST_LIST(MGL_DECLARE_PULL_SKIP)
#undef MGL_DECLARE_PULL_SKIP
#else
namespace {
    using GLContext = MG_State::GLState::GLContext;
    using MG_State::GLState::FramebufferObject;
    using MG_State::GLState::MipmapInput;
    using MG_State::GLState::RenderbufferObject;
    using MG_State::GLState::TextureObject2D;
    using MG_State::GLState::TextureObject2DCube;

    // AN RAII SCOPE RATHER THAN A gtest FIXTURE, for VertexInputEmitTest's reason, and it arms
    // BOTH bits this family needs: the framebuffer bit for the emission itself, and the
    // texture-resource bit because MGPSurface::Res names a texture or renderbuffer handle and
    // bit 9 requires bit 10 for exactly that reason.
    struct FramebufferScope {
        FramebufferScope() {
            m_previousPush = MG_Config::Features.PipePush;
            // D-K2, both rows: bit 9 requires bit 10 (MGPSurface::Res names a texture or
            // renderbuffer handle) and bit 10 requires bit 11 (MGPTextureParams::BuiltinSampler
            // is a SamplerCso out of the sampler family's content-addressed cache).
            MG_Config::Features.PipePush |=
                kMGPipeSubsystemFramebuffer | kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers;
            m_previousContext = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
            MGPipeFramebufferEmitterInstance().ResetForTest();
            MGPipeTextureEmitterInstance().ResetForTest();
            MGPipeSetHashSuppressorInstance().InvalidateAll();
            // The applier is a process singleton and its object records outlive a case; with both
            // families' wired constants set the emissions actually land, so a case must start
            // from an empty table (v1 armed the texture emitter here instead - ArmForTest is
            // gone with the flip).
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
        }
        ~FramebufferScope() {
            MGPipeFramebufferEmitterInstance().ResetForTest();
            MGPipeTextureEmitterInstance().ResetForTest();
            MGPipeSetHashSuppressorInstance().InvalidateAll();
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
            MG_State::pGLContext.reset();
            MG_State::pGLContext = Move(m_previousContext);
            MG_Config::Features.PipePush = m_previousPush;
        }
        FramebufferScope(const FramebufferScope&) = delete;
        FramebufferScope& operator=(const FramebufferScope&) = delete;

        UniquePtr<GLContext> m_previousContext;
        Uint64 m_previousPush = 0;
    };

    MGPipeFramebufferEmitter& Framebuffers() { return MGPipeFramebufferEmitterInstance(); }
    GLContext& Ctx() { return *MG_State::pGLContext; }

    SharedPtr<TextureObject2D> MakeColorTexture(Uint name, Int size, Uint levels = 1) {
        auto texture = MakeShared<TextureObject2D>(name);
        texture->SetInternalFormat(TextureInternalFormat::RGBA8);
        for (Uint level = 0; level < levels; ++level) {
            const Int extent = std::max<Int>(size >> level, 1);
            texture->AllocateStorage(TextureUploadTarget::Texture2D, level,
                                     MipmapInput{IntVec3{extent, extent, 1},
                                                 static_cast<SizeT>(extent) * static_cast<SizeT>(extent) * 4});
        }
        return texture;
    }

    void BindDrawAndRead(const SharedPtr<FramebufferObject>& draw, const SharedPtr<FramebufferObject>& read) {
        Ctx().GetFramebufferBindingSlot(FramebufferTarget::Draw).Bind(draw);
        Ctx().GetFramebufferBindingSlot(FramebufferTarget::Read).Bind(read);
    }
} // namespace

// ============================ D-C1 / D-C2 ============================
//
// THE RESOLVED READ SURFACE IS WHAT STRUCTURALLY CLOSES THE read-buffer-shared-FBO DEFECT
// CLASS. The record carries the surface itself rather than an index, and EVERY record resolves
// it from the framebuffer ITS OWN Fbo names - Named included (c0e / MGPipeTypes.h). v1 resolved
// a DRAW record's ReadSurface from the READ-bound object, which was D-C2's letter and muddled
// in substance: no field of this record may refer to "whatever is bound", and a glReadBuffer on
// the read FBO moved the draw record's ContentHash and forced a redundant draw emission.
TEST(FramebufferEmit, EveryRecordsReadSurfaceComesFromItsOwnFramebuffersReadBuffer) {
    FramebufferScope scope;
    const auto drawColor = MakeColorTexture(1, 32);
    const auto readColor0 = MakeColorTexture(2, 32);
    const auto readColor1 = MakeColorTexture(3, 32);

    const auto drawFbo = MakeShared<FramebufferObject>(1);
    drawFbo->AttachTexture(FramebufferAttachmentType::Color0, drawColor, TextureUploadTarget::Texture2D);
    const auto readFbo = MakeShared<FramebufferObject>(2);
    readFbo->AttachTexture(FramebufferAttachmentType::Color0, readColor0, TextureUploadTarget::Texture2D);
    readFbo->AttachTexture(FramebufferAttachmentType::Color1, readColor1, TextureUploadTarget::Texture2D);
    readFbo->SetReadBuffer(FramebufferAttachmentType::Color1);
    BindDrawAndRead(drawFbo, readFbo);

    Framebuffers().EmitFramebufferState(Ctx());
    EXPECT_EQ(Framebuffers().EmissionCount(), 2u) << "two distinct bindings are two records";

    const MGPipeHandle readColor1Handle =
        MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, readColor1->GetLifetimeId());
    ASSERT_FALSE(MGPipeHandleIsNull(readColor1Handle));
    const MGPipeHandle drawColorHandle =
        MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, drawColor->GetLifetimeId());
    ASSERT_FALSE(MGPipeHandleIsNull(drawColorHandle));
    EXPECT_EQ(Framebuffers().LastDraw().Target, static_cast<Uint8>(MGPipeFramebufferTarget::Draw));
    EXPECT_TRUE(Framebuffers().LastDraw().ReadSurface.Res == drawColorHandle)
        << "the DRAW record's ReadSurface names a surface that is not part of the framebuffer its "
           "own Fbo names";
    EXPECT_EQ(Framebuffers().LastRead().Target, static_cast<Uint8>(MGPipeFramebufferTarget::Read));
    EXPECT_TRUE(Framebuffers().LastRead().ReadSurface.Res == readColor1Handle)
        << "MGPFramebufferState::ReadSurface on the READ record did not come from that "
           "framebuffer's own read buffer";

    // AND THE DRAW RECORD DOES NOT MOVE WHEN THE READ FRAMEBUFFER'S READ BUFFER DOES.
    const Uint64 drawHashBefore = Framebuffers().LastDraw().ContentHash;
    const Uint64 emissionsBefore = Framebuffers().EmissionCount();
    readFbo->SetReadBuffer(FramebufferAttachmentType::Color0);
    Framebuffers().EmitFramebufferState(Ctx());
    EXPECT_EQ(Framebuffers().EmissionCount(), emissionsBefore + 1)
        << "only the READ record moved, so exactly one record goes out";
    EXPECT_EQ(Framebuffers().LastDraw().ContentHash, drawHashBefore)
        << "a glReadBuffer on the read framebuffer moved the DRAW record's content hash";
    // And the draw-buffer array belongs to the DRAW object, whichever record carries it.
    EXPECT_EQ(Framebuffers().LastDraw().DrawBuffers[0], 0);
}

TEST(FramebufferEmit, OneObjectBoundToBothTargetsEmitsOneRecordWithTargetBoth) {
    FramebufferScope scope;
    const auto color0 = MakeColorTexture(4, 32);
    const auto color1 = MakeColorTexture(5, 32);
    const auto fbo = MakeShared<FramebufferObject>(3);
    fbo->AttachTexture(FramebufferAttachmentType::Color0, color0, TextureUploadTarget::Texture2D);
    fbo->AttachTexture(FramebufferAttachmentType::Color1, color1, TextureUploadTarget::Texture2D);
    fbo->SetReadBuffer(FramebufferAttachmentType::Color1);
    BindDrawAndRead(fbo, fbo);

    Framebuffers().EmitFramebufferState(Ctx());
    EXPECT_EQ(Framebuffers().EmissionCount(), 1u) << "one object on both targets is ONE record";
    EXPECT_EQ(Framebuffers().LastDraw().Target, static_cast<Uint8>(MGPipeFramebufferTarget::Both));
    const MGPipeHandle color1Handle =
        MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, color1->GetLifetimeId());
    EXPECT_TRUE(Framebuffers().LastDraw().ReadSurface.Res == color1Handle)
        << "the shared-FBO record's ReadSurface is not that object's own read buffer";
}

// ============================ D-C4 ============================
//
// THE fragColor BROADCAST TRAP. The backend derives the broadcast count from the draw-buffer
// array, at the verb, from the framebuffer state it then holds - precisely so a program can
// relink inside the same draw. A hash that did not cover the array would let a suppressed
// set_framebuffer_state mean "the draw buffers did not move" when they had, and the shader
// would be specialised for the previous output shape.
TEST(FramebufferEmit, ADrawBufferChangeAloneStillMovesTheContentHash) {
    FramebufferScope scope;
    const auto color0 = MakeColorTexture(6, 32);
    const auto color1 = MakeColorTexture(7, 32);
    const auto fbo = MakeShared<FramebufferObject>(4);
    fbo->AttachTexture(FramebufferAttachmentType::Color0, color0, TextureUploadTarget::Texture2D);
    fbo->AttachTexture(FramebufferAttachmentType::Color1, color1, TextureUploadTarget::Texture2D);
    BindDrawAndRead(fbo, fbo);

    Framebuffers().EmitFramebufferState(Ctx());
    ASSERT_EQ(Framebuffers().EmissionCount(), 1u);
    const Uint64 before = Framebuffers().LastDraw().ContentHash;
    const Int8 slot1Before = Framebuffers().LastDraw().DrawBuffers[1];

    // NOTHING BUT THE DRAW-BUFFER ARRAY MOVES: the same attachments, the same extent, the same
    // completeness answer, the same handle.
    fbo->SetDrawBuffer(1, FramebufferAttachmentType::Color1);
    Framebuffers().EmitFramebufferState(Ctx());
    EXPECT_EQ(Framebuffers().EmissionCount(), 2u)
        << "a draw-buffer change alone was suppressed, which would freeze the fragColor "
           "broadcast count at the previous output shape";
    EXPECT_NE(Framebuffers().LastDraw().ContentHash, before)
        << "MGPFramebufferState::ContentHash does not cover DrawBuffers[]";
    EXPECT_NE(Framebuffers().LastDraw().DrawBuffers[1], slot1Before);
    EXPECT_EQ(Framebuffers().LastDraw().DrawBuffers[1], 1);
}

TEST(FramebufferEmit, ARecycledFramebufferHandleIsNeverSuppressedAgainstItsPredecessor) {
    FramebufferScope scope;
    // The SAME attachment set on both objects, so every other field of the record is identical
    // and Fbo is the only thing that can move the hash.
    const auto color = MakeColorTexture(8, 32);
    Uint64 firstHash = 0;
    MGPipeHandle firstHandle{};
    {
        const auto first = MakeShared<FramebufferObject>(5);
        first->AttachTexture(FramebufferAttachmentType::Color0, color, TextureUploadTarget::Texture2D);
        BindDrawAndRead(first, first);
        Framebuffers().EmitFramebufferState(Ctx());
        ASSERT_EQ(Framebuffers().EmissionCount(), 1u);
        firstHash = Framebuffers().LastDraw().ContentHash;
        firstHandle = Framebuffers().LastDraw().Fbo;
        BindDrawAndRead(nullptr, nullptr);
    }
    const auto second = MakeShared<FramebufferObject>(5);
    second->AttachTexture(FramebufferAttachmentType::Color0, color, TextureUploadTarget::Texture2D);
    BindDrawAndRead(second, second);
    Framebuffers().EmitFramebufferState(Ctx());
    const MGPFramebufferState& record = Framebuffers().LastDraw();
    EXPECT_EQ(record.Fbo.Slot, firstHandle.Slot) << "the slot was not recycled; the case proves nothing";
    EXPECT_NE(record.Fbo.Gen, firstHandle.Gen) << "a recycled slot must carry a new generation";
    EXPECT_NE(record.ContentHash, firstHash)
        << "MGPFramebufferState::ContentHash does not cover Fbo, so a recycled framebuffer handle "
           "would be suppressed against its predecessor's record";
}

// ============================ G6 ============================
//
// "For every framebuffer configuration the emitted MGPFramebufferState reproduces exactly the
// values the backend's SyncToBackend family reads from the frontend today, field by field."
// The oracle is the frontend attachment itself, read back through the same getters the twin
// uses, so this cannot drift into asserting what the emitter happens to do.
TEST(FramebufferEmit, EveryAttachmentFieldSurvivesTheSurfaceConversion) {
    FramebufferScope scope;
    const auto fbo = MakeShared<FramebufferObject>(6);
    Vector<SharedPtr<TextureObject2D>> colors;
    for (SizeT i = 0; i < kMGPipeMaxColorAttachments; ++i) {
        colors.push_back(MakeColorTexture(static_cast<Uint>(20 + i), 32, 2));
        // Distinct level, layer and layered flag per point, so a conversion that dropped one
        // field could not be masked by another point's value.
        fbo->AttachTexture(static_cast<FramebufferAttachmentType>(
                               static_cast<Int>(FramebufferAttachmentType::Color0) + static_cast<Int>(i)),
                           colors.back(), TextureUploadTarget::Texture2D,
                           static_cast<int>(i % 2), static_cast<int>(i), (i % 2) == 0);
    }
    const auto depth = MakeShared<RenderbufferObject>(2);
    depth->SetInternalFormat(TextureInternalFormat::Depth24Stencil8);
    depth->AllocateStorage(IntVec2{32, 32});
    fbo->AttachRenderbuffer(FramebufferAttachmentType::Depth, depth);
    const auto stencil = MakeColorTexture(40, 32);
    fbo->AttachTexture(FramebufferAttachmentType::Stencil, stencil, TextureUploadTarget::Texture2D);
    BindDrawAndRead(fbo, fbo);

    Framebuffers().EmitFramebufferState(Ctx());
    ASSERT_EQ(Framebuffers().EmissionCount(), 1u);
    const MGPFramebufferState& record = Framebuffers().LastDraw();

    for (SizeT i = 0; i < kMGPipeMaxColorAttachments; ++i) {
        const auto type = static_cast<FramebufferAttachmentType>(
            static_cast<Int>(FramebufferAttachmentType::Color0) + static_cast<Int>(i));
        const auto& attachment = fbo->GetAttachment(type);
        const MGPSurface& surface = record.Color[i];
        const MGPipeHandle expected =
            MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, attachment.GetTexture()->GetLifetimeId());
        EXPECT_TRUE(surface.Res == expected) << "MGPSurface::Res at colour point " << i;
        EXPECT_EQ(surface.Kind, kMGPipeSurfaceKindTexture) << "MGPSurface::Kind at colour point " << i;
        EXPECT_EQ(surface.InternalFormat, static_cast<Uint32>(attachment.GetTexture()->GetFormat()))
            << "MGPSurface::InternalFormat at colour point " << i;
        EXPECT_EQ(surface.Level, static_cast<Uint16>(attachment.GetTextureLevel()))
            << "MGPSurface::Level at colour point " << i;
        EXPECT_EQ(surface.Layer, static_cast<Uint32>(attachment.GetTextureLayer()))
            << "MGPSurface::Layer at colour point " << i;
        EXPECT_EQ(surface.Layered, attachment.IsLayered() ? 1 : 0)
            << "MGPSurface::Layered at colour point " << i;
        EXPECT_EQ(surface.UploadTarget, static_cast<Uint16>(TextureUploadTarget::Texture2D))
            << "MGPSurface::UploadTarget at colour point " << i;
        // ID-12 DV-5: three of the four cross-object masks reduce to (format, TEXTURE TARGET)
        // and no TextureUploadTarget -> TextureTarget inverse exists anywhere in the tree.
        EXPECT_EQ(surface.TextureTarget,
                  static_cast<Uint16>(attachment.GetTexture()->GetTarget()))
            << "MGPSurface::TextureTarget at colour point " << i;
    }

    EXPECT_EQ(record.Depth.Kind, kMGPipeSurfaceKindRenderbuffer) << "MGPSurface::Kind on the depth point";
    EXPECT_EQ(record.Depth.InternalFormat, static_cast<Uint32>(depth->GetInternalFormat()))
        << "MGPSurface::InternalFormat on the depth point";
    EXPECT_TRUE(record.Depth.Res ==
                MGPipeSlots().FindByLifetimeId(MGPipeKind::Renderbuffer, depth->GetLifetimeId()))
        << "MGPSurface::Res on the depth point";
    EXPECT_EQ(record.Depth.Layered, 0) << "MGPSurface::Layered on the depth point";
    EXPECT_EQ(record.Depth.TextureTarget, kMGPipeSurfaceNoTextureTarget)
        << "MGPSurface::TextureTarget on the RENDERBUFFER point must be the Unknown sentinel";
    EXPECT_EQ(record.Stencil.Kind, kMGPipeSurfaceKindTexture) << "MGPSurface::Kind on the stencil point";
    EXPECT_TRUE(record.Stencil.Res ==
                MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, stencil->GetLifetimeId()))
        << "MGPSurface::Res on the stencil point";

    EXPECT_EQ(record.Width, 32u) << "MGPFramebufferState::Width";
    EXPECT_EQ(record.Height, 32u) << "MGPFramebufferState::Height";
    EXPECT_EQ(record.IsDefault, 0) << "MGPFramebufferState::IsDefault";
    // Complete is FramebufferObject::CheckCompleteness(), the FRONTEND-only answer, and never
    // glCheckFramebufferStatus's - that entry point additionally consults the backend's probed
    // format-capability cache, which a client emitting it would be reading from the wrong side.
    EXPECT_EQ(record.Complete, fbo->CheckCompleteness() ? 1 : 0) << "MGPFramebufferState::Complete";
    // And an empty point really is {null, None} and every other field zero.
    const auto emptyFbo = MakeShared<FramebufferObject>(7);
    emptyFbo->AttachTexture(FramebufferAttachmentType::Color0, colors[0], TextureUploadTarget::Texture2D);
    BindDrawAndRead(emptyFbo, emptyFbo);
    Framebuffers().EmitFramebufferState(Ctx());
    EXPECT_TRUE(MGPipeHandleIsNull(Framebuffers().LastDraw().Color[1].Res));
    EXPECT_EQ(Framebuffers().LastDraw().Color[1].Kind, kMGPipeSurfaceKindNone);
    EXPECT_EQ(Framebuffers().LastDraw().Color[1].InternalFormat, 0u);
    // BOTH TARGET FIELDS CARRY THE SENTINEL, not 0 (m6 / ID-12 DV-5): a Uint16 zero is
    // TextureUploadTarget::Texture1D and TextureTarget::Texture1D, so a reader that forgot to
    // gate on Kind would read a plausible wrong answer instead of a nonsense one.
    EXPECT_EQ(Framebuffers().LastDraw().Color[1].UploadTarget,
              static_cast<Uint16>(TextureUploadTarget::Unknown));
    EXPECT_EQ(Framebuffers().LastDraw().Color[1].TextureTarget, kMGPipeSurfaceNoTextureTarget);
}

// ============================ D-C3 ============================
TEST(FramebufferEmit, AnAttachmentPointAboveTheWireWidthIsRefusedNotTruncated) {
    FramebufferScope scope;
    const auto color = MakeColorTexture(50, 32);
    const auto beyond = MakeColorTexture(51, 32);
    const auto fbo = MakeShared<FramebufferObject>(8);
    fbo->AttachTexture(FramebufferAttachmentType::Color0, color, TextureUploadTarget::Texture2D);
    BindDrawAndRead(fbo, fbo);
    Framebuffers().EmitFramebufferState(Ctx());
    ASSERT_EQ(Framebuffers().EmissionCount(), 1u);
    ASSERT_EQ(Framebuffers().RefusedCount(), 0u);

    // MGPFramebufferState::Color[] is eight wide and MaxColorAttachments is the driver's raw ES
    // cap, not clamped to eight on the GLES path. Truncating silently is the bug class this
    // phase is closing, so the record is REFUSED and the legacy arm runs.
    fbo->AttachTexture(static_cast<FramebufferAttachmentType>(
                           static_cast<Int>(FramebufferAttachmentType::Color0) +
                           static_cast<Int>(kMGPipeMaxColorAttachments)),
                       beyond, TextureUploadTarget::Texture2D);
    Framebuffers().EmitFramebufferState(Ctx());
    EXPECT_EQ(Framebuffers().EmissionCount(), 1u)
        << "an attachment above the wire width was truncated into a record instead of refused";
    EXPECT_GE(Framebuffers().RefusedCount(), 1u) << "the refusal was not counted";
}

// ============================ D-D2 ============================
//
// RED BEFORE THIS PACKAGE LANDED. RenderbufferObject's three storage setters bump no version
// and raise no notice, and the framebuffer bit's shutter does not move when an ALREADY-ATTACHED
// renderbuffer is re-storaged - so `glBindRenderbuffer; glRenderbufferStorage(newSize)` on an
// attached renderbuffer published nothing at all.
TEST(FramebufferEmit, ARestoragedAttachedRenderbufferPublishesItsNewExtent) {
    FramebufferScope scope;
    const auto renderbuffer = MakeShared<RenderbufferObject>(3);
    renderbuffer->SetInternalFormat(TextureInternalFormat::Depth24Stencil8);
    renderbuffer->AllocateStorage(IntVec2{64, 64});
    const auto fbo = MakeShared<FramebufferObject>(9);
    fbo->AttachRenderbuffer(FramebufferAttachmentType::Depth, renderbuffer);
    BindDrawAndRead(fbo, fbo);
    Framebuffers().EmitFramebufferState(Ctx());

    const MGPipeHandle handle =
        MGPipeSlots().FindByLifetimeId(MGPipeKind::Renderbuffer, renderbuffer->GetLifetimeId());
    ASSERT_FALSE(MGPipeHandleIsNull(handle));
    const Uint64 respecifiesBefore = MGPipeTextureEmitterInstance().RespecifyCount();

    // NO BIND, NO ATTACHMENT CHANGE, NO DIRTY BIT - only the storage entry point.
    renderbuffer->AllocateStorage(IntVec2{128, 96});
    EXPECT_GT(MGPipeTextureEmitterInstance().RespecifyCount(), respecifiesBefore)
        << "a re-storaged attached renderbuffer published nothing";
    const MGPResourceDesc desc = MGPipeTextureEmitterInstance().LastDesc();
    EXPECT_TRUE(desc.Resource == handle);
    EXPECT_EQ(desc.Width, 128u);
    EXPECT_EQ(desc.Height, 96u);
}

TEST(FramebufferEmit, AnUnchangedBindingPairEmitsNothing) {
    FramebufferScope scope;
    const auto color = MakeColorTexture(60, 32);
    const auto fbo = MakeShared<FramebufferObject>(10);
    fbo->AttachTexture(FramebufferAttachmentType::Color0, color, TextureUploadTarget::Texture2D);
    BindDrawAndRead(fbo, fbo);
    const Uint64 bytes = Framebuffers().EmitFramebufferState(Ctx());
    EXPECT_EQ(bytes, sizeof(MGPFramebufferState));
    EXPECT_EQ(Framebuffers().EmissionCount(), 1u);
    // The version-first skip: nothing moved, so nothing goes out and nothing is hashed twice.
    EXPECT_EQ(Framebuffers().EmitFramebufferState(Ctx()), 0u);
    EXPECT_EQ(Framebuffers().EmissionCount(), 1u);
}

// ============================ ID-19(c) ============================
//
// THE HOLE esprytobj's C-1 FOUND. The applier keeps framebuffer records PER OBJECT, but v1 only
// ever built the two BOUND-target records at the validate point - so glClearNamedFramebufferfv
// on an fbo bound to NEITHER binding reached a backend that minted a driver framebuffer with no
// attachments, found no record for it, declined, and cleared against it anyway.
TEST(FramebufferEmit, AFramebufferHandedOverByNameGetsANamedRecordWithoutMovingABinding) {
    FramebufferScope scope;
    const auto boundColor = MakeColorTexture(70, 32);
    const auto namedColor = MakeColorTexture(71, 16);
    const auto bound = MakeShared<FramebufferObject>(20);
    bound->AttachTexture(FramebufferAttachmentType::Color0, boundColor, TextureUploadTarget::Texture2D);
    BindDrawAndRead(bound, bound);
    Framebuffers().EmitFramebufferState(Ctx());
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    const MGPipeHandle boundHandle = MGPipeApplier().DrawFramebuffer()->Fbo;
    const Uint64 serialBefore = MGPipeApplier().FramebufferSerial;

    // The object the DSA entry point is about to hand over, bound to neither binding.
    const auto named = MakeShared<FramebufferObject>(21);
    named->AttachTexture(FramebufferAttachmentType::Color0, namedColor, TextureUploadTarget::Texture2D);
    const Uint64 bytes = Framebuffers().EmitFramebufferByName(*named);
    EXPECT_EQ(bytes, sizeof(MGPFramebufferState));
    EXPECT_EQ(Framebuffers().LastNamed().Target, static_cast<Uint8>(MGPipeFramebufferTarget::Named));
    EXPECT_GT(MGPipeApplier().FramebufferSerial, serialBefore) << "a Named record must publish";

    const MGPFramebufferState* stored =
        MGPipeApplier().FramebufferRecordFor(Framebuffers().LastNamed().Fbo);
    ASSERT_NE(stored, nullptr) << "the framebuffer the server is about to receive by name has no record";
    EXPECT_EQ(stored->Width, 16u) << "the Named record does not describe the object it names";

    // AND IT MOVED NEITHER BINDING.
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_TRUE(MGPipeApplier().DrawFramebuffer()->Fbo == boundHandle)
        << "a Named record took the draw binding";
    ASSERT_NE(MGPipeApplier().ReadFramebuffer(), nullptr);
    EXPECT_TRUE(MGPipeApplier().ReadFramebuffer()->Fbo == boundHandle)
        << "a Named record took the read binding";
}

TEST(FramebufferEmit, ANamedRecordIsSuppressedPerObjectAndNeverAgainstABoundRecord) {
    FramebufferScope scope;
    const auto colorA = MakeColorTexture(72, 32);
    const auto colorB = MakeColorTexture(73, 32);
    const auto first = MakeShared<FramebufferObject>(22);
    first->AttachTexture(FramebufferAttachmentType::Color0, colorA, TextureUploadTarget::Texture2D);
    const auto second = MakeShared<FramebufferObject>(23);
    second->AttachTexture(FramebufferAttachmentType::Color0, colorB, TextureUploadTarget::Texture2D);

    // TWO DIFFERENT OBJECTS' Named RECORDS IN A ROW MUST BOTH GO OUT: the suppressor is keyed by
    // the framebuffer the record names and never by one global slot.
    EXPECT_GT(Framebuffers().EmitFramebufferByName(*first), 0u);
    EXPECT_GT(Framebuffers().EmitFramebufferByName(*second), 0u);
    EXPECT_EQ(Framebuffers().EmissionCount(), 2u);
    // A repeat of the same object with nothing moved costs nothing.
    EXPECT_EQ(Framebuffers().EmitFramebufferByName(*second), 0u);
    EXPECT_EQ(Framebuffers().EmissionCount(), 2u);
    // ...and the first object's record is still its own, not the second's.
    EXPECT_EQ(Framebuffers().EmitFramebufferByName(*first), 0u);

    // A BOUND OBJECT IS NEVER HANDED A Named RECORD, because the record would then say "no
    // binding" while BoundFramebuffer still resolves through it.
    BindDrawAndRead(first, nullptr);
    Framebuffers().EmitFramebufferByName(*first);
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Target, static_cast<Uint8>(MGPipeFramebufferTarget::Draw))
        << "a by-name publish of a DRAW-bound framebuffer overwrote its record with Target = Named";
}

// ============================ m2 ============================
TEST(FramebufferEmit, ADrawBufferTokenAboveTheWireWidthIsRefusedNotTruncated) {
    FramebufferScope scope;
    const auto color = MakeColorTexture(74, 32);
    const auto fbo = MakeShared<FramebufferObject>(24);
    fbo->AttachTexture(FramebufferAttachmentType::Color0, color, TextureUploadTarget::Texture2D);
    BindDrawAndRead(fbo, fbo);
    Framebuffers().EmitFramebufferState(Ctx());
    ASSERT_EQ(Framebuffers().EmissionCount(), 1u);
    ASSERT_EQ(Framebuffers().RefusedCount(), 0u);

    // A LEGAL STATE THE ATTACHMENT SCAN CANNOT SEE: nothing is attached at the point the token
    // names, so D-C3's attachment loop passes, and MGPipeDrawBufferIndex would write an index of
    // 8 or more into an 8-wide array.
    fbo->SetDrawBuffer(0, static_cast<FramebufferAttachmentType>(
                              static_cast<Int>(FramebufferAttachmentType::Color0) +
                              static_cast<Int>(kMGPipeMaxColorAttachments) + 2));
    Framebuffers().EmitFramebufferState(Ctx());
    EXPECT_EQ(Framebuffers().EmissionCount(), 1u)
        << "a draw-buffer token naming a colour point at or above the wire width was truncated "
           "into a record instead of refused";
    EXPECT_GE(Framebuffers().RefusedCount(), 1u) << "the refusal was not counted";
}

// ============================ m1 ============================
TEST(FramebufferEmit, ALayeredCubeAttachmentDoesNotAssertAFaceItCannotKnow) {
    FramebufferScope scope;
    const auto cube = MakeShared<TextureObject2DCube>(75);
    cube->SetInternalFormat(TextureInternalFormat::RGBA8);
    for (const TextureUploadTarget face : cube->GetUploadTargets()) {
        cube->AllocateStorage(face, 0, MipmapInput{IntVec3{16, 16, 1}, 16 * 16 * 4});
    }
    ASSERT_GT(cube->GetUploadTargets().size(), 1u);

    const auto fbo = MakeShared<FramebufferObject>(25);
    // The LAYERED entry point carries no face token, so the attachment stores Unknown. v1 fell
    // back to GetUploadTargets()[0] - which is CubeMapPositiveX - and the record then ASSERTED a
    // face that is not the truth for an attachment naming all six. The precedent it copied
    // (FramebufferAttachmentObject::GetSize) only needs an EXTENT, which is identical across the
    // six faces; face identity is not.
    fbo->AttachTexture(FramebufferAttachmentType::Color0, cube, TextureUploadTarget::Unknown, 0, 0, true);
    BindDrawAndRead(fbo, fbo);
    Framebuffers().EmitFramebufferState(Ctx());
    ASSERT_EQ(Framebuffers().EmissionCount(), 1u);
    EXPECT_EQ(Framebuffers().LastDraw().Color[0].Layered, 1);
    EXPECT_EQ(Framebuffers().LastDraw().Color[0].UploadTarget,
              static_cast<Uint16>(TextureUploadTarget::Unknown))
        << "a layered cube attachment resolved to one face, so the record asserts a face the "
           "attachment does not name";
    EXPECT_EQ(Framebuffers().LastDraw().Color[0].TextureTarget,
              static_cast<Uint16>(TextureTarget::TextureCubeMap))
        << "MGPSurface::TextureTarget is what a cross-object mask reads instead";
}

TEST(FramebufferEmit, EveryNonTexturePointCarriesTheUnknownSentinelsRatherThanZero) {
    FramebufferScope scope;
    const auto color = MakeColorTexture(76, 32);
    const auto depth = MakeShared<RenderbufferObject>(4);
    depth->SetInternalFormat(TextureInternalFormat::Depth24Stencil8);
    depth->AllocateStorage(IntVec2{32, 32});
    const auto fbo = MakeShared<FramebufferObject>(26);
    fbo->AttachTexture(FramebufferAttachmentType::Color0, color, TextureUploadTarget::Texture2D);
    fbo->AttachRenderbuffer(FramebufferAttachmentType::Depth, depth);
    BindDrawAndRead(fbo, fbo);
    Framebuffers().EmitFramebufferState(Ctx());
    const MGPFramebufferState& record = Framebuffers().LastDraw();

    // The RENDERBUFFER point, the EMPTY colour points and the STENCIL point that names nothing.
    for (const MGPSurface* surface : {&record.Depth, &record.Stencil, &record.Color[1]}) {
        EXPECT_NE(surface->Kind, kMGPipeSurfaceKindTexture);
        EXPECT_EQ(surface->UploadTarget, static_cast<Uint16>(TextureUploadTarget::Unknown))
            << "a non-texture point says TextureUploadTarget::Texture1D, which is what 0 means";
        EXPECT_EQ(surface->TextureTarget, kMGPipeSurfaceNoTextureTarget)
            << "a non-texture point says TextureTarget::Texture1D, which is what 0 means";
    }
    // And the texture point is unaffected.
    EXPECT_EQ(record.Color[0].TextureTarget, static_cast<Uint16>(TextureTarget::Texture2D));

    // THE FIELD IS IN THE HASH, so a texture-target change alone cannot be suppressed.
    MGPSurface probe = record.Color[0];
    MGPFramebufferState probeState = record;
    probeState.Color[0].TextureTarget = static_cast<Uint16>(TextureTarget::TextureRectangle);
    EXPECT_NE(MGPipeFramebufferStateContentHash(probeState), MGPipeFramebufferStateContentHash(record))
        << "MGPipeCopySurfaceForHash does not copy MGPSurface::TextureTarget, so a record whose "
           "only moved field is the attachment's texture target would be suppressed";
    (void)probe;
}
#endif // MOBILEGL_PIPE_PUSH

// =========================================================================================
// The APPLIER's half of set_framebuffer_state (the wire commits'). The emitter's half - the
// resolved read surface, the draw-buffer array in the content hash, a recycled handle never
// suppressed against its predecessor, an attachment point above the wire width refused rather
// than truncated - is the client package's and lands beside these.
// =========================================================================================

// THE WHOLE POINT OF THE Target BYTE. GL has two independent framebuffer bindings and this
// record carries one Fbo and one ReadSurface, so a record says which binding it describes;
// Both is one object bound to both and writes both. Deleting either store, or the serial bump,
// leaves this red.
TEST(FramebufferEmit, ADrawRecordAndAReadRecordAreKeptApartAndBothWritesBoth) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const Uint64 serialAtStart = MGPipeApplier().FramebufferSerial;

    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{4, 1}, MGPipeFramebufferTarget::Draw, 100));
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Fbo, (MGPipeHandle{4, 1}));
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Width, 100u);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Color[0].InternalFormat, 0x8058u);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->DrawBuffers[0], 0);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer(), nullptr)
        << "a Draw record landed in the read binding as well";
    const Uint64 afterDraw = MGPipeApplier().FramebufferSerial;
    EXPECT_GT(afterDraw, serialAtStart) << "an applied record must move the serial the twin memoises";

    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{5, 2}, MGPipeFramebufferTarget::Read, 200));
    ASSERT_NE(MGPipeApplier().ReadFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer()->Fbo, (MGPipeHandle{5, 2}));
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer()->Width, 200u);
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr) << "a Read record overwrote the draw binding";
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Fbo, (MGPipeHandle{4, 1}));
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Width, 100u);
    EXPECT_GT(MGPipeApplier().FramebufferSerial, afterDraw);

    // Both: one record, one serial bump, two destinations.
    const Uint64 beforeBoth = MGPipeApplier().FramebufferSerial;
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{6, 3}, MGPipeFramebufferTarget::Both, 300));
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    ASSERT_NE(MGPipeApplier().ReadFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Fbo, (MGPipeHandle{6, 3}));
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer()->Fbo, (MGPipeHandle{6, 3}));
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Width, 300u);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer()->Width, 300u);
    EXPECT_EQ(MGPipeApplier().FramebufferSerial, beforeBoth + 1)
        << "a Both record is ONE record and moves the serial once";
    // The two earlier framebuffers keep their own records - the table is keyed by the handle,
    // so binding a third displaced neither (ID-19(b)).
    ASSERT_NE(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{4, 1}), nullptr);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{4, 1})->Width, 100u);
    ASSERT_NE(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{5, 2}), nullptr);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{5, 2})->Width, 200u);

    // A framebuffer has a handle but NO wire lifetime, so there is no record to refuse against
    // and this entry point never counts an object refusal.
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u);
#endif
}

// A target outside the FOUR is not a target this server has, and guessing one would put a
// draw's attachments into the read binding or the other way round. Named (3) is legal since
// ID-19(b) and has its own case below; the first refused value is the one above it.
TEST(FramebufferEmit, ATargetOutsideTheThreeBindingsIsRefusedNamingTheRecord) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    MGPFramebufferState bad = FramebufferRecord(MGPipeHandle{7, 4}, MGPipeFramebufferTarget::Draw, 100);
    bad.Target = static_cast<Uint8>(MGPipeFramebufferTarget::Count);
    const Uint64 serialBefore = MGPipeApplier().FramebufferSerial;

    ExpectRefusedNaming("set_framebuffer_state {slot=7, gen=4, target=4}: the record names no framebuffer "
                        "binding target",
                        [&bad]() { MGPipeApplySetFramebufferState(bad); });
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{7, 4}), nullptr)
        << "a refused record was written into the per-object table anyway";
    EXPECT_EQ(MGPipeApplier().FramebufferSerial, serialBefore)
        << "a refused record must not move the serial";
#endif
}

// The draw-buffer array is an INDEX into this record's own Color[], and -1 is NONE. An entry
// outside that range would have the server read a colour attachment the record does not carry,
// which is the truncation the wire width's cap refusal exists to prevent upstream.
TEST(FramebufferEmit, ADrawBufferEntryOutsideTheRecordsOwnArrayIsRefusedRatherThanRead) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;

    // The positive control first: -1 everywhere and the last legal index are both fine, so
    // what follows is refusing the value and not the loop around it.
    MGPFramebufferState legal = FramebufferRecord(MGPipeHandle{8, 1}, MGPipeFramebufferTarget::Draw, 100);
    legal.DrawBuffers[7] = static_cast<Int8>(kMGPipeMaxColorAttachments - 1);
    MGPipeApplySetFramebufferState(legal);
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    ASSERT_EQ(MGPipeApplier().DrawFramebuffer()->Fbo, (MGPipeHandle{8, 1}));
    const Uint64 serialBefore = MGPipeApplier().FramebufferSerial;

    MGPFramebufferState past = FramebufferRecord(MGPipeHandle{8, 1}, MGPipeFramebufferTarget::Draw, 111);
    past.DrawBuffers[3] = static_cast<Int8>(kMGPipeMaxColorAttachments);
    ExpectRefusedNaming("set_framebuffer_state {slot=8, gen=1, target=0}: a draw-buffer entry names a "
                        "colour attachment outside the record's own array",
                        [&past]() { MGPipeApplySetFramebufferState(past); });

    MGPFramebufferState negative = FramebufferRecord(MGPipeHandle{8, 1}, MGPipeFramebufferTarget::Draw, 222);
    negative.DrawBuffers[0] = -2;
    ExpectRefusedNaming("set_framebuffer_state {slot=8, gen=1, target=0}: a draw-buffer entry names a "
                        "colour attachment outside the record's own array",
                        [&negative]() { MGPipeApplySetFramebufferState(negative); });

    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Width, 100u) << "a refused record was stored anyway";
    EXPECT_EQ(MGPipeApplier().FramebufferSerial, serialBefore);
#endif
}

// D-J4, as ID-19(b) leaves it. The two framebuffer BINDINGS are per-context working state and a
// make-current takes them - so both accessors answer null afterwards, exactly as the zeroed
// records used to answer a null Fbo - while the per-object RECORD survives, because a
// framebuffer that is only ever addressed BY NAME has no re-emission trigger at all. The serial
// ADVANCES rather than restarting, because a counter that walks back through values it has
// already stamped into a twin that outlived the switch is not a generation at all. Restoring
// `= 0` anywhere in the reset, or clearing the table there, leaves this red.
TEST(FramebufferEmit, AMakeCurrentClearsBothRecordsAndAdvancesTheSerialRatherThanZeroingIt) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{4, 1}, MGPipeFramebufferTarget::Both, 100));
    const Uint64 serialBefore = MGPipeApplier().FramebufferSerial;
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    ASSERT_EQ(MGPipeApplier().DrawFramebuffer()->Width, 100u);

    MGPipeApplierReset(); // the make-current

    EXPECT_EQ(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().BoundFramebuffer[0], kMGPipeNullHandle);
    EXPECT_EQ(MGPipeApplier().BoundFramebuffer[1], kMGPipeNullHandle);
    ASSERT_NE(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{4, 1}), nullptr)
        << "the per-object record is not working state and a make-current may not take it";
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{4, 1})->Width, 100u);
    EXPECT_GT(MGPipeApplier().FramebufferSerial, serialBefore)
        << "the serial was carried over or restarted; the cleared window is itself a change the "
           "twin has to hear about, and no stamped value may ever recur";

    // And the teardown scope advances it again, for the same reason.
    const Uint64 afterReset = MGPipeApplier().FramebufferSerial;
    MGPipeApplierReleaseObjectRecords();
    EXPECT_GT(MGPipeApplier().FramebufferSerial, afterReset);
#endif
}

// THE TEARDOWN SCOPE DROPS THE OBJECT RECORDS, SO IT MUST DROP EVERY WORKING HANDLE THAT NAMES
// ONE. The two framebuffer records hold eleven MGPSurface::Res naming texture and renderbuffer
// records, and the three unit windows hold entries naming sampler-view, sampler-CSO and texture
// records; a window left standing after the tables are emptied is a set of handles into empty
// tables, which the next resolve either refuses and counts or - on a slot the next context
// re-mints - resolves onto somebody else's record. Deleting any one of the eleven clears in
// MGPipeApplierReleaseObjectRecords leaves this red.
TEST(FramebufferEmit, AReleaseOfTheObjectRecordsAlsoClearsTheWorkingHandlesThatCouldNameThem) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{4, 1}, MGPipeFramebufferTarget::Both, 100));

    // The three kVarTail sets, each with one entry naming a record the release is about to
    // drop, and each at a non-zero Start so the window itself is visible in the assertions.
    MGPBoundView view{};
    view.View = MGPipeHandle{3, 1};
    view.Texture = MGPipeHandle{9, 1};
    view.Unit = 2;
    MGPipeApplySetSamplerViews(MGPSamplerViews{2, 1, 0xAAAAu}, &view);

    const MGPipeHandle samplerState{5, 1};
    MGPipeApplyBindSamplerStates(MGPSamplerStates{2, 1, 0xBBBBu}, &samplerState);

    MGPImageView image{};
    image.Res = MGPipeHandle{9, 1};
    image.Unit = 2;
    image.InternalFormat = 0x8058u; // GL_RGBA8
    MGPipeApplySetShaderImages(MGPShaderImages{2, 1, 0xCCCCu}, &image);

    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    ASSERT_EQ(MGPipeApplier().DrawFramebuffer()->Color[0].Res, (MGPipeHandle{9, 1}));
    ASSERT_EQ(MGPipeApplier().SamplerViewCount, 1u);
    ASSERT_EQ(MGPipeApplier().BoundSamplerViews[2].View, (MGPipeHandle{3, 1}));
    ASSERT_EQ(MGPipeApplier().SamplerStateCount, 1u);
    ASSERT_EQ(MGPipeApplier().BoundSamplerStates[2], samplerState);
    ASSERT_EQ(MGPipeApplier().ShaderImageCount, 1u);
    ASSERT_EQ(MGPipeApplier().BoundShaderImages[2].Res, (MGPipeHandle{9, 1}));

    MGPipeApplierReleaseObjectRecords();

    EXPECT_EQ(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{4, 1}), nullptr)
        << "a framebuffer record holding eleven MGPSurface::Res into the emptied texture and "
           "renderbuffer tables survived the teardown";
    EXPECT_TRUE(MGPipeApplier().FramebufferRecords.empty());
    EXPECT_EQ(MGPipeApplier().SamplerViewStart, 0u);
    EXPECT_EQ(MGPipeApplier().SamplerViewCount, 0u);
    EXPECT_EQ(MGPipeApplier().BoundSamplerViews[2].View, kMGPipeNullHandle);
    EXPECT_EQ(MGPipeApplier().SamplerStateStart, 0u);
    EXPECT_EQ(MGPipeApplier().SamplerStateCount, 0u);
    EXPECT_EQ(MGPipeApplier().BoundSamplerStates[2], kMGPipeNullHandle);
    EXPECT_EQ(MGPipeApplier().ShaderImageStart, 0u);
    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 0u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Res, kMGPipeNullHandle);
#endif
}

// ID-19's CORRECTION, AND THE CASE THAT SAYS WHAT THE FOURTH TARGET IS FOR. Every DSA entry
// point - BlitNamedFramebuffer and the four ClearNamedFramebuffer* - hands Espryt a framebuffer
// BY NAME, and that framebuffer is very often bound to neither binding. With only the two bound
// records the server had no description of it at all, bound its driver FBO with no attachments
// and cleared or blitted into nothing (esprytobj C-1). A Named record fixes that WITHOUT lying
// about the bindings: the record is written and addressable by handle, and BoundFramebuffer
// does not move. Making the Named arm touch either binding leaves this red.
TEST(FramebufferEmit, ANamedRecordDescribesTheFramebufferItNamesWithoutMovingEitherBinding) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    // TWO DIFFERENT FRAMEBUFFERS ON THE TWO BINDINGS FIRST, so "the bindings did not move" is an
    // assertion about values rather than about null.
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{4, 1}, MGPipeFramebufferTarget::Draw, 100));
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{5, 2}, MGPipeFramebufferTarget::Read, 200));
    const Uint64 serialBefore = MGPipeApplier().FramebufferSerial;

    MGPFramebufferState named = FramebufferRecord(MGPipeHandle{6, 3}, MGPipeFramebufferTarget::Draw, 300);
    named.Target = static_cast<Uint8>(MGPipeFramebufferTarget::Named);
    named.Color[0].Res = MGPipeHandle{21, 1};
    MGPipeApplySetFramebufferState(named);

    // (a) THE DSA LOOKUP FINDS IT, BY HANDLE, WITH ITS ATTACHMENTS. This is the call package D
    // makes at every named blit and clear.
    ASSERT_NE(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{6, 3}), nullptr)
        << "a framebuffer handed to the server by name has no record, which is the state that "
           "clears into a driver framebuffer with no attachments";
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{6, 3})->Width, 300u);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{6, 3})->Color[0].Res, (MGPipeHandle{21, 1}));
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{6, 3})->Target, static_cast<Uint8>(MGPipeFramebufferTarget::Named));

    // (b) AND NEITHER BINDING MOVED.
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    ASSERT_NE(MGPipeApplier().ReadFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Fbo, (MGPipeHandle{4, 1}))
        << "a Named record claimed the draw binding";
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer()->Fbo, (MGPipeHandle{5, 2}))
        << "a Named record claimed the read binding";
    EXPECT_EQ(MGPipeApplier().BoundFramebuffer[0], (MGPipeHandle{4, 1}));
    EXPECT_EQ(MGPipeApplier().BoundFramebuffer[1], (MGPipeHandle{5, 2}));

    // (c) The serial moves for a Named record too: a twin memoising a framebuffer's attachments
    // has to hear that they moved, and whether it is bound is a different question.
    EXPECT_EQ(MGPipeApplier().FramebufferSerial, serialBefore + 1);

    // (d) And the same framebuffer can then be BOUND, which moves the binding and restates the
    // record - the two targets are not two tables.
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{6, 3}, MGPipeFramebufferTarget::Draw, 400));
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Fbo, (MGPipeHandle{6, 3}));
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Width, 400u);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer()->Fbo, (MGPipeHandle{5, 2}));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u);
#endif
}

// STALE-GENERATION REFUSAL, ON THE ONE TABLE WHOSE OBJECT HAS NO WIRE LIFETIME. A framebuffer is
// never destroyed on the wire, so its slot is simply overwritten by its successor - and until
// that successor describes itself, a handle naming the DEAD one must be refused rather than
// answered with the predecessor's attachments. That answer would be a blit or a clear into
// somebody else's colour buffer. It is LOUD (counted, and logged once) because the only way to
// reach it is an emitter defect, and it is counted APART from RefusedObjectCalls because this is
// a read by the server's own sync path and not a call the applier refused.
TEST(FramebufferEmit, AFramebufferHandleWhoseGenerationHasMovedOnIsRefusedRatherThanAnswered) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{12, 1}, MGPipeFramebufferTarget::Draw, 100));
    ASSERT_NE(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{12, 1}), nullptr);
    const Uint64 staleBefore = MGPipeApplier().StaleFramebufferRecordLookups;

    // The slot has been recycled and the successor has not described itself yet.
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{12, 2}), nullptr)
        << "a handle at a recycled slot was answered with its predecessor's record";
    EXPECT_EQ(MGPipeApplier().StaleFramebufferRecordLookups, staleBefore + 1);

    // Now it does, and the predecessor's handle becomes the stale one - in the other direction.
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{12, 2}, MGPipeFramebufferTarget::Draw, 200));
    ASSERT_NE(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{12, 2}), nullptr);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{12, 2})->Width, 200u);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{12, 1}), nullptr);
    EXPECT_EQ(MGPipeApplier().StaleFramebufferRecordLookups, staleBefore + 2);

    // THE TWO SILENT NULLS, and they are silent on purpose. "Nothing is bound to this binding"
    // is what a make-current leaves behind and arrives on every draw of a context that has not
    // described its framebuffers; "no record at this slot" is what every framebuffer looks like
    // before its first set_framebuffer_state. Counting either would bury the one that matters.
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(kMGPipeNullHandle), nullptr);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(MGPipeHandle{99, 1}), nullptr);
    EXPECT_EQ(MGPipeApplier().StaleFramebufferRecordLookups, staleBefore + 2)
        << "an unbound binding or an undescribed slot was counted as a stale generation";
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u)
        << "the framebuffer family may never move the object-refusal counter";
#endif
}

// THE TWO REFUSALS THE PER-OBJECT TABLE ADDED. The null handle is what "nothing is bound" reads
// as, so a record installed at {0,0} would be answered to every caller asking about an EMPTY
// binding; and Slot is a client-supplied Uint32 that now reaches an allocator, so it takes the
// same bound the five object tables take. Every emitter has a handle for every framebuffer it
// describes - kMGPipeDefaultFramebuffer {0,1} for the default one - so neither value is
// producible by a correct client, which is why both are Fatal rather than counted refusals.
TEST(FramebufferEmit, AFramebufferRecordThatNamesNoUsableHandleIsRefusedRatherThanStored) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    // The positive control first: the DEFAULT framebuffer is slot 0 at generation 1 and is
    // perfectly legal, so what follows refuses the null handle and not slot 0.
    MGPipeApplySetFramebufferState(
        FramebufferRecord(kMGPipeDefaultFramebuffer, MGPipeFramebufferTarget::Both, 128));
    ASSERT_NE(MGPipeApplier().FramebufferRecordFor(kMGPipeDefaultFramebuffer), nullptr);
    EXPECT_EQ(MGPipeApplier().FramebufferRecordFor(kMGPipeDefaultFramebuffer)->Width, 128u);
    const Uint64 serialBefore = MGPipeApplier().FramebufferSerial;

    MGPFramebufferState nullHandle = FramebufferRecord(kMGPipeNullHandle, MGPipeFramebufferTarget::Draw, 300);
    ExpectRefusedNaming("set_framebuffer_state {slot=0, gen=0, target=0}: the record names the null "
                        "framebuffer handle",
                        [&nullHandle]() { MGPipeApplySetFramebufferState(nullHandle); });

    MGPFramebufferState pastTheBound = FramebufferRecord(
        MGPipeHandle{kMGPipeMaxFramebufferSlots, 1}, MGPipeFramebufferTarget::Draw, 400);
    ExpectRefusedNaming("set_framebuffer_state {slot=65536, gen=1, target=0}: the framebuffer slot is "
                        "outside the record table's bound",
                        [&pastTheBound]() { MGPipeApplySetFramebufferState(pastTheBound); });
    static_assert(kMGPipeMaxFramebufferSlots == 65536u,
                  "the refusal line above names the bound; move both together");

    EXPECT_EQ(MGPipeApplier().FramebufferSerial, serialBefore) << "a refused record moved the serial";
    ASSERT_NE(MGPipeApplier().DrawFramebuffer(), nullptr);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer()->Fbo, kMGPipeDefaultFramebuffer)
        << "a refused record took the draw binding";
    EXPECT_LT(MGPipeApplier().FramebufferRecords.size(),
              static_cast<SizeT>(kMGPipeMaxFramebufferSlots))
        << "an out-of-range slot resized the table instead of being refused";
#endif
}

int main(int argc, char** argv) {
    // Before anything logs: the logger reads this variable once, on its first write, and
    // caches the handle. The name carries this process's pid, and the file is removed on the
    // way out.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-framebufferemit-test-" + std::to_string(ProcessId()) + ".log");
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
