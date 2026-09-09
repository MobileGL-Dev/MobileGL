// MobileGL - MobileGL/MG_Test/Pipe/ImageEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's third kVarTail unit set, set_shader_images. It rides the sampler family's subsystem
// bit: one family, one A/B.
//
// WHAT THIS SUITE IS FOR. The image set is the one whose ContentHash has to cover more than
// the binding: InternalFormat and Access are live glBindImageTexture state that the
// format-less image bake keys on, so a set whose only movement is an access mode still has to
// go out. And the zero early-out is the property an optimisation deletes by accident - it is
// what makes every draw of every application that never binds an image pay one integer test.
//
// THE SUITE IS `ImageEmit`, not `ImageEmitTest`: the file is XTest.cpp and the suite is X.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT.
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
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/Pipe/ImageEmit.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
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
    // `from` is a byte offset, and it exists because of the fork below: the library's log file
    // is already open by the time a case runs, so the child's lines are APPENDED to it rather
    // than written to a fresh file, and only what the child appended is this drive's evidence.
    std::string ReadLog(std::streamoff from = 0) {
        std::ifstream in(g_logPath, std::ios::binary);
        if (from > 0) in.seekg(from, std::ios::beg);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Where the library's log file currently ends. Reading from here after the child has
    // aborted gives exactly the lines that drive produced.
    std::streamoff LogEnd() {
        std::ifstream in(g_logPath, std::ios::binary | std::ios::ate);
        return in ? static_cast<std::streamoff>(in.tellg()) : std::streamoff{0};
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
        // THE LOG PATH IS NOT UNLINKED HERE, and that is what this helper had to learn when the
        // client's cases landed in the same file as the applier's: main() calls
        // MobileGL::Initialize(), so the library's log FILE* is already open on this path and
        // fork() duplicates it. Removing the path would leave the child writing into a deleted
        // inode and the parent reading an empty file - the child would still abort, and the
        // assertion on WHAT it named could never see the line. So the log's end is remembered
        // and only what the child appended is read back.
        const std::streamoff before = LogEnd();
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
        result.Log = ReadLog(before);
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

// See FramebufferEmitTest's twin for why this is a shape pin rather than a placeholder.
TEST(ImageEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeImageEmitterInstance(), &MGPipeImageEmitterInstance());
    // The image set has no bit of its own: set_shader_images rides the SAMPLER subsystem,
    // because the three unit sets are one family and an operator switching them off has to get
    // the whole family's legacy arm.
    EXPECT_EQ(kMGPipeMaxImageUnits, kMGPipeMaxTextureUnits);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

// =========================================================================================
// The APPLIER's half of set_shader_images (the wire commits'). The emitter's half - the
// high-water-zero early-out, the content hash covering Access and InternalFormat, the shutter
// keyed on the FRONTEND sampling-resolution generation - is the client package's.
// =========================================================================================

#if MOBILEGL_PIPE_PUSH
namespace {
    // Every field carries a value of its own, and two of them are the point: InternalFormat and
    // Access are live glBindImageTexture state that the format-less image bake keys on, so a
    // body that dropped either would leave the server baking against a format the shader was
    // not built for.
    MGPImageView ImageAt(Uint32 unit, Uint32 internalFormat, Uint8 access) {
        MGPImageView view{};
        view.Res = MGPipeHandle{unit + 1, 1};
        view.Unit = unit;
        view.InternalFormat = internalFormat;
        view.Layer = 3;
        view.Level = 2;
        view.Layered = 1;
        view.Access = access;
        return view;
    }
} // namespace
#endif

// The window rule, one field at a time: the entries land where the header says and nowhere
// else, and every field of an entry survives. Deleting the copy loop, the two window
// assignments or the serial bump leaves this red.
TEST(ImageEmit, TheImageSetLandsInItsWindowWithEveryFieldTheShaderWasBuiltAgainst) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    // Access is a Uint8 on the wire - the client's own read/write/read-write encoding, not a
    // GL enum - and InternalFormat is the application's, which the server recasts.
    const MGPImageView entries[2] = {ImageAt(2, 0x8814u /* GL_RGBA32F */, 2 /* write only */),
                                     ImageAt(3, 0x8230u /* GL_RG32F */, 3 /* read write */)};
    MGPShaderImages header{};
    header.Start = 2;
    header.Count = 2;
    header.ContentHash = 0x5150u;
    const Uint64 serialBefore = MGPipeApplier().ShaderImagesSerial;
    // The other two sets' serials, taken AFTER the fixture: a reset and a teardown each advance
    // every working serial, so "unchanged" is measured from here rather than from zero.
    const Uint64 samplerViewsSerial = MGPipeApplier().SamplerViewsSerial;

    MGPipeApplySetShaderImages(header, entries);

    EXPECT_EQ(MGPipeApplier().ShaderImageStart, 2u);
    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 2u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Res, (MGPipeHandle{3, 1}));
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].InternalFormat, 0x8814u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Access, 2u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[3].Access, 3u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Level, 2u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Layer, 3u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Layered, 1u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[3].InternalFormat, 0x8230u);
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundShaderImages[1].Res)) << "the set wrote below its window";
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundShaderImages[4].Res)) << "the set wrote above its window";
    EXPECT_GT(MGPipeApplier().ShaderImagesSerial, serialBefore);

    // "The last set as received": a narrower set says nothing about what it does not name.
    MGPShaderImages narrow{};
    narrow.Start = 2;
    narrow.Count = 1;
    MGPipeApplySetShaderImages(narrow, entries);
    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 1u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[3].InternalFormat, 0x8230u)
        << "the entry outside the new window was cleared";
    EXPECT_EQ(MGPipeApplier().SamplerViewsSerial, samplerViewsSerial)
        << "the image set moved another set's serial; the three are independent";
#endif
}

// The window gate, at the bound and one past it, and the null-tail arm. The image-unit space
// is the same merged 192 the sampler units are.
TEST(ImageEmit, AnImageWindowPastTheImageUnitSpaceIsRefusedRatherThanTruncated) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPImageView entry = ImageAt(0, 0x8058u, 2);

    MGPShaderImages exact{};
    exact.Start = kMGPipeMaxImageUnits - 1;
    exact.Count = 1;
    MGPipeApplySetShaderImages(exact, &entry);
    ASSERT_EQ(MGPipeApplier().ShaderImageCount, 1u);
    const Uint64 serialBefore = MGPipeApplier().ShaderImagesSerial;

    MGPShaderImages past{};
    past.Start = kMGPipeMaxImageUnits;
    past.Count = 1;
    past.ContentHash = 9;
    ExpectRefusedNaming("set_shader_images {start=192, count=1, hash=9}: the window runs past the merged "
                        "texture-unit space",
                        [&past, &entry]() { MGPipeApplySetShaderImages(past, &entry); });

    MGPShaderImages noTail{};
    noTail.Start = 0;
    noTail.Count = 1;
    ExpectRefusedNaming("set_shader_images {start=0, count=1, hash=0}: a non-empty set carries no entries",
                        [&noTail]() { MGPipeApplySetShaderImages(noTail, nullptr); });

    EXPECT_EQ(MGPipeApplier().ShaderImagesSerial, serialBefore) << "a refused set moved the serial";
    EXPECT_EQ(MGPipeApplier().ShaderImageStart, kMGPipeMaxImageUnits - 1);
#endif
}

// An EMPTY set is not a refusal: it is what a program with no image uniforms publishes, and it
// still moves the serial, because "no images" is a state the twin has to hear about.
TEST(ImageEmit, AnEmptySetIsAppliedRatherThanRefusedAndStillMovesTheSerial) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPImageView entry = ImageAt(0, 0x8058u, 1);
    MGPShaderImages filled{};
    filled.Count = 1;
    MGPipeApplySetShaderImages(filled, &entry);
    const Uint64 serialBefore = MGPipeApplier().ShaderImagesSerial;

    MGPShaderImages empty{};
    MGPipeApplySetShaderImages(empty, nullptr);
    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 0u);
    EXPECT_GT(MGPipeApplier().ShaderImagesSerial, serialBefore);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[0].InternalFormat, 0x8058u)
        << "an empty window cleared entries it never named";
#endif
}

// D-J4: the image set is per-context WORKING state, so a make-current takes it and ADVANCES
// its serial rather than restarting it.
TEST(ImageEmit, AMakeCurrentClearsTheImageSetAndAdvancesItsSerial) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPImageView entry = ImageAt(1, 0x8058u, 1);
    MGPShaderImages header{};
    header.Start = 1;
    header.Count = 1;
    MGPipeApplySetShaderImages(header, &entry);
    const Uint64 serialBefore = MGPipeApplier().ShaderImagesSerial;

    MGPipeApplierReset();

    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 0u);
    EXPECT_EQ(MGPipeApplier().ShaderImageStart, 0u);
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundShaderImages[1].Res));
    EXPECT_GT(MGPipeApplier().ShaderImagesSerial, serialBefore);
#endif
}

#if !MOBILEGL_PIPE_PUSH
// G2 requires the pull and push ctest name sets to be identical, name for name.
#define MGL_IMAGE_EMIT_TEST_LIST(X)                                                                \
    X(ImageEmit, AZeroHighWaterMarkEmitsNothingWithoutHashing)                                      \
    X(ImageEmit, AnAccessModeChangeAloneStillEmitsTheSet)                                           \
    X(ImageEmit, AnInternalFormatChangeAloneStillEmitsTheSet)                                       \
    X(ImageEmit, TheApplicationsFormatAndAccessTravelUnrecast)                                    \
    X(ImageEmit, AnImageBoundTextureIsMarkedShaderImageBoundAtTheBind)

#define MGL_DECLARE_PULL_SKIP(Suite, Name)                                                         \
    TEST(Suite, Name) { GTEST_SKIP() << "compiled only under MOBILEGL_PIPE_PUSH"; }
MGL_IMAGE_EMIT_TEST_LIST(MGL_DECLARE_PULL_SKIP)
#undef MGL_DECLARE_PULL_SKIP
#else

namespace {
    namespace GL = MobileGL::MG_Impl::GLImpl;
    using GLContext = MG_State::GLState::GLContext;

    struct EmitterScope {
        EmitterScope() { Clear(); }
        ~EmitterScope() { Clear(); }
        EmitterScope(const EmitterScope&) = delete;
        EmitterScope& operator=(const EmitterScope&) = delete;

        static void Clear() {
            MGPipeImageEmitterInstance().Reset();
            MGPipeImageEmitterInstance().ResetCounters();
            MGPipeProgramOpaqueUnitsShared().Invalidate();
            MGPipeSetHashSuppressorInstance().InvalidateAll();
        }
    };

    GLContext& Ctx() { return *MG_State::pGLContext; }
    MGPipeImageEmitter& Emitter() { return MGPipeImageEmitterInstance(); }

    GLuint MakeComputeProgram(const char* source) {
        const GLuint shader = GL::CreateShader(GL_COMPUTE_SHADER);
        GL::ShaderSource(shader, 1, &source, nullptr);
        GL::CompileShader(shader);
        const GLuint program = GL::CreateProgram();
        GL::AttachShader(program, shader);
        GL::LinkProgram(program);
        GLint linked = GL_FALSE;
        GL::GetProgramiv(program, GL_LINK_STATUS, &linked);
        EXPECT_EQ(linked, GL_TRUE) << "the compute program did not link";
        return program;
    }

    GLuint MakeImageTexture() {
        GLuint name = 0;
        GL::GenTextures(1, &name);
        GL::BindTexture(GL_TEXTURE_2D, name);
        GL::TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
        return name;
    }

    // PROPERTY 1 OF D-G4, and it is the one an optimisation deletes: an application that never
    // binds an image pays one integer test per draw, taken BEFORE any hash and before any
    // 192-entry walk.
    //
    // The frontend has no image-unit high-water mark of its own - NoteUnitTouched is the
    // TEXTURE-unit path and glBindImageTexture does not reach it - so the window is derived
    // from the highest image unit the CURRENT PROGRAM names. A program with no image uniform
    // names none, and the set is not emitted at all.
    TEST(ImageEmit, AZeroHighWaterMarkEmitsNothingWithoutHashing) {
        EmitterScope scope;
        static const char* kNoImages = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Out { uint value; } outBuf;
void main() { outBuf.value = 1u; }
)";
        const GLuint program = MakeComputeProgram(kNoImages);
        GL::UseProgram(program);

        // A texture IS bound to an image unit. The set still does not go out, because no
        // shader can read it - which is the whole point of deriving the window from the
        // program rather than walking 192 units to find out.
        const GLuint texture = MakeImageTexture();
        GL::BindImageTexture(0, texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);

        EXPECT_EQ(Emitter().EmitShaderImages(Ctx()), 0u);
        EXPECT_EQ(Emitter().ImageSetCount(), 0u);
        EXPECT_EQ(Emitter().Window(), 0u);
        GL::UseProgram(0);
    }

    const char* kImageCompute = R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 1, rgba8) uniform image2D img;
void main() { imageStore(img, ivec2(0), vec4(1.0)); }
)";

    // The record carries the APPLICATION's format and access verbatim. The bind-format recast -
    // a GL_RG32F bind is INVALID_VALUE on most non-core formats on Adreno - and the
    // buffer-texture split view are SERVER-side and stay there, so a client that pre-applied
    // either of them would be answering a driver question from the wrong side of the boundary.
    TEST(ImageEmit, TheApplicationsFormatAndAccessTravelUnrecast) {
        EmitterScope scope;
        const GLuint program = MakeComputeProgram(kImageCompute);
        GL::UseProgram(program);
        const GLuint texture = MakeImageTexture();
        GL::BindImageTexture(1, texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        // THE FIXTURE HAS TO SET UP WHAT THE CASE IS ABOUT, and it is asserted rather than
        // assumed: the window comes from the program's own image uniforms, so a shader whose
        // image uniform did not reach the reflection would make every case below pass for the
        // wrong reason - by emitting nothing at all.
        const SharedPtr<MG_State::GLState::ProgramObject>& object = Ctx().GetProgramObject(program);
        ASSERT_TRUE(object);
        const auto& resolution = MGPipeProgramOpaqueUnitsShared().For(object.get());
        ASSERT_EQ(resolution.MaxImageUnit, 1)
            << "maxUniformLocation=" << object->GetMaxUniformLocation()
            << " unit@0=" << object->GetUniformSamplerOrImageUnitIndex(0)
            << " linked=" << object->GetLinkStatus();

        // ASSERTED ON THE COUNTER, NOT ON THIS CALL'S RETURN VALUE, and the reason is worth
        // writing down because it surprised this suite: the glBindImageTexture above ALREADY
        // reached the validate point and emitted the set, so a direct call afterwards is
        // correctly suppressed as unchanged. What the case is about is what went out, not who
        // sent it.
        Emitter().EmitShaderImages(Ctx());
        ASSERT_GE(Emitter().ImageSetCount(), 1u);
        ASSERT_GE(Emitter().LastShaderImages().Count, 2u);
        const MGPImageView& view = Emitter().LastImageViews()[1];
        EXPECT_EQ(view.Unit, 1u);
        EXPECT_FALSE(MGPipeHandleIsNull(view.Res));
        EXPECT_EQ(view.InternalFormat, static_cast<Uint32>(GL_RGBA8));
        EXPECT_EQ(view.Access, 1u) << "GL_WRITE_ONLY, folded into the one byte the wire carries";
        EXPECT_EQ(view.Level, 0u);
        EXPECT_EQ(view.Layered, 0u);
        EXPECT_EQ(view.Layer, 0u);
        GL::UseProgram(0);
    }

    TEST(ImageEmit, AnAccessModeChangeAloneStillEmitsTheSet) {
        EmitterScope scope;
        const GLuint program = MakeComputeProgram(kImageCompute);
        GL::UseProgram(program);
        const GLuint texture = MakeImageTexture();
        GL::BindImageTexture(1, texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        Emitter().EmitShaderImages(Ctx());
        const Uint64 before = Emitter().ImageSetCount();
        ASSERT_GE(before, 1u);
        ASSERT_EQ(Emitter().LastImageViews()[1].Access, 0u) << "GL_READ_ONLY";

        // The same texture, the same unit, the same format - only the access mode moves. The
        // hash has to cover it, or a shader that now writes where it used to read is bound with
        // the previous barrier and coherence semantics.
        GL::BindImageTexture(1, texture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
        Emitter().EmitShaderImages(Ctx());
        EXPECT_GT(Emitter().ImageSetCount(), before);
        EXPECT_EQ(Emitter().LastImageViews()[1].Access, 2u);
        GL::UseProgram(0);
    }

    TEST(ImageEmit, AnInternalFormatChangeAloneStillEmitsTheSet) {
        EmitterScope scope;
        const GLuint program = MakeComputeProgram(kImageCompute);
        GL::UseProgram(program);
        const GLuint texture = MakeImageTexture();
        GL::BindImageTexture(1, texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        Emitter().EmitShaderImages(Ctx());
        const Uint64 before = Emitter().ImageSetCount();
        ASSERT_GE(before, 1u);
        ASSERT_EQ(Emitter().LastImageViews()[1].InternalFormat, static_cast<Uint32>(GL_RGBA8));

        // The format the shader was built against is live glBindImageTexture state, and the
        // format-less image bake keys on it: a set suppressed because "the binding did not
        // move" would leave the server baking against the previous format.
        GL::BindImageTexture(1, texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8UI);
        Emitter().EmitShaderImages(Ctx());
        EXPECT_GT(Emitter().ImageSetCount(), before);
        EXPECT_EQ(Emitter().LastImageViews()[1].InternalFormat, static_cast<Uint32>(GL_RGBA8UI));
        GL::UseProgram(0);
    }

    // FINAL REVIEW M-A: glBindImageTexture IS THE EARLIEST PRODUCER OF kMGPipeBindShaderImage -
    // the bit the ImageBindableHint is derived from - and the emitted image set's walk is D-A4's
    // (any texture named in an emitted MGPImageView). The hint is the PREVENTION half of the
    // texture-remint stall class: a texture the server knows may be image-bound is allocated
    // image-bindable up front, so it has to arrive before the first sync, i.e. at the bind.
    // Nothing produced the bit before the fix round.
    TEST(ImageEmit, AnImageBoundTextureIsMarkedShaderImageBoundAtTheBind) {
        EmitterScope scope;
        MGPipeTextureEmitterInstance().ResetForTest();
        const GLuint name = MakeImageTexture();
        const auto& texture = Ctx().GetTextureObject(name);
        ASSERT_TRUE(texture);
        const MGPipeHandle handle = MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, texture->GetLifetimeId());
        ASSERT_FALSE(MGPipeHandleIsNull(handle));
        EXPECT_EQ(MGPipeTextureEmitterInstance().TextureBindMask(handle) & kMGPipeBindShaderImage, 0)
            << "nothing has image-bound this texture yet";

        GL::BindImageTexture(0, name, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        EXPECT_NE(MGPipeTextureEmitterInstance().TextureBindMask(handle) & kMGPipeBindShaderImage, 0)
            << "glBindImageTexture did not mark the texture image-bound";

        static const char* kOneImage = R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 0, rgba8) uniform image2D img;
void main() { imageStore(img, ivec2(0, 0), vec4(1.0)); }
)";
        const GLuint program = MakeComputeProgram(kOneImage);
        GL::UseProgram(program);
        Emitter().EmitShaderImages(Ctx());
        ASSERT_GE(Emitter().Window(), 1u);
        EXPECT_TRUE(Emitter().LastImageViews()[0].Res == handle);
        EXPECT_NE(MGPipeTextureEmitterInstance().TextureBindMask(handle) & kMGPipeBindShaderImage, 0)
            << "the emitted image set's walk does not carry the bit either";
        GL::UseProgram(0);
        GL::BindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    }
} // namespace
#endif // MOBILEGL_PIPE_PUSH

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-imageemit-test-" + std::to_string(ProcessId()) + ".log");
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
