// MobileGL - MobileGL/MG_Test/Pipe/ShaderBufferEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5e's set_shader_buffers - the INDEXED BUFFER BINDING POINTS - on both sides of the call
// (MG_Remote/CONTRACT-P5E.md §5.6, rulings 10 and 11; package sb).
//
// WHAT THIS SUITE IS FOR. Three properties of this family are invisible in a picture and are
// exactly what an optimisation or a merge deletes:
//   1. the ZERO EARLY-OUT - a class nothing has bound emits nothing, before any hash;
//   2. a BASE binding travels as kMGPipeWholeBuffer and never as a resolved extent, so a
//      glBufferData between the emission and the apply binds the NEW extent as GL does;
//   3. THREE SUPPRESSOR SLOTS, one per class, so an emission of one class never cancels
//      another's (ruling 11) - a single slot would make the shader-storage set disappear
//      whenever a uniform set happened to hash the same way.
// Plus ID-104's widened WritableMask, whose whole point is the points a Uint32 could not
// describe: the case that matters is point 83, not point 3.
//
// THE SUITE IS `ShaderBufferEmit`, not `ShaderBufferEmitTest`: the file is XTest.cpp and the
// suite is X, this directory's convention.
//
// IT HAS ITS OWN main() for VertexInputEmitTest's reason: the applier refuses a malformed
// window, and that verdict is a log line in a shipped push build and std::abort() in a poison
// or verify one. Every case is a visible SKIP in a pull build rather than a vanishing test, so
// `ctest -N` stays name-for-name identical between the pull and the push trees (G2/G14).

#include <gtest/gtest.h>
#include <MG_Util/Debug/Log.h>

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
#include <Config.h>
// MOBILEGL_PIPE_POISON is DERIVED in the header below (PipeInputs.h:20-26) and nowhere else,
// so a TU that tests it without this include silently reads it as 0 - and then
// ExpectRefusedNaming takes the in-process arm while the applier's trip wire really does
// abort, which takes the whole binary down with it. ImageEmitTest carries the same include for
// the same reason and learned it the same way.
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/ShaderBufferEmit.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeRoute.h>
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
} // namespace

#if !MOBILEGL_PIPE_PUSH
namespace {
    // G2/G14: the pull build gets the same ctest names, skipping.
#define MGL_SHADER_BUFFER_EMIT_TEST_LIST(X)                                                        \
    X(ShaderBufferEmit, TheEmitterIsOneNeverDestroyedProcessSingleton)                              \
    X(ShaderBufferEmit, AnUntouchedClassEmitsNothingWithoutHashing)                                 \
    X(ShaderBufferEmit, ABaseBindingTravelsAsWholeBufferAndARangeBindingTravelsResolved)            \
    X(ShaderBufferEmit, OnlyTheWritableClassesSetAMaskAndItReachesTheLastPoint)                     \
    X(ShaderBufferEmit, EachClassLatchesItsOwnSuppressorSlot)                                       \
    X(ShaderBufferEmit, TheAppliedWindowIsPerClassAndTheSerialMovesOnEveryRecord)                   \
    X(ShaderBufferEmit, AWindowPastTheCapacityOrAnUnknownClassIsRefusedRatherThanStored)            \
    X(ShaderBufferEmit, AResetApplierCarriesNoBindingPointStateOverAndTheSerialAdvances)

#define MGL_DECLARE_PULL_SKIP(Suite, Name)                                                         \
    TEST(Suite, Name) { GTEST_SKIP() << "compiled only under MOBILEGL_PIPE_PUSH"; }
    MGL_SHADER_BUFFER_EMIT_TEST_LIST(MGL_DECLARE_PULL_SKIP)
#undef MGL_DECLARE_PULL_SKIP
} // namespace
#else

namespace {
    namespace GL = MobileGL::MG_Impl::GLImpl;
    using GLContext = MG_State::GLState::GLContext;

    // Each case starts from an emitter with no latch, an applier with no window and a
    // suppressor that has never seen a hash, because every property below is about what the
    // NEXT emission does.
    struct EmitterScope {
        EmitterScope() { Clear(); }
        ~EmitterScope() { Clear(); }
        EmitterScope(const EmitterScope&) = delete;
        EmitterScope& operator=(const EmitterScope&) = delete;

        static void Clear() {
            MGPipeInstallMonolithTables(); // idempotent; the route has to reach the applier
            MGPipeShaderBufferEmitterInstance().Reset();
            MGPipeShaderBufferEmitterInstance().ResetCounters();
            MGPipeSetHashSuppressorInstance().InvalidateAll();
            for (Uint32 cls = 0; cls < kMGPipeShaderBufferClassCount; ++cls) {
                MGPipeApplier().BoundShaderBuffers[cls] = {};
                MGPipeApplier().ShaderBufferStart[cls] = 0;
                MGPipeApplier().ShaderBufferCount[cls] = 0;
                for (Uint32 w = 0; w < kMGPipeShaderBufferWritableMaskWords; ++w) {
                    MGPipeApplier().ShaderBufferWritableMask[cls][w] = 0;
                }
            }
        }
    };

    GLContext& Ctx() { return *MG_State::pGLContext; }
    MGPipeShaderBufferEmitter& Emitter() { return MGPipeShaderBufferEmitterInstance(); }

    // ---- driving a refusal, in ImageEmitTest's shape and for its reason ------------------
    //
    // The applier's trip wires report through a log line in a shipped push build and
    // std::abort() in a poison or verify one - and MOBILEGL_PIPE_POISON is DERIVED from
    // MOBILEGL_BUILD_DISAGGREGATED behind PipeInputs.h, so the split build this campaign gates
    // on is the aborting arm. A case that drove one in process would therefore take the whole
    // binary down, which is why the drive is a forked child there and the parent reads SIGABRT
    // plus the line the wire appended to the log.
    std::string ReadLog(std::streamoff from = 0) {
        std::ifstream in(g_logPath, std::ios::binary);
        if (from > 0) in.seekg(from, std::ios::beg);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::streamoff LogEnd() {
        std::ifstream in(g_logPath, std::ios::binary | std::ios::ate);
        return in ? static_cast<std::streamoff>(in.tellg()) : std::streamoff{0};
    }

#if MGTEST_HAVE_FORK
    struct ChildResult {
        int Status = -1;
        std::string Log;
    };

    template <class Body>
    ChildResult RunInChild(Body body) {
        ChildResult result;
        // The log path is NOT unlinked: main() has already opened it, fork() duplicates the
        // handle, and removing it would leave the child writing into a deleted inode while the
        // parent read an empty file. Only what the child appended is this drive's evidence.
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

    Bool DiedOfAbort(const ChildResult& r) {
        return WIFSIGNALED(r.Status) && WTERMSIG(r.Status) == SIGABRT;
    }
    std::string DescribeStatus(const ChildResult& r) {
        if (r.Status < 0) return "fork/waitpid failed";
        if (WIFEXITED(r.Status)) return "exited " + std::to_string(WEXITSTATUS(r.Status));
        if (WIFSIGNALED(r.Status)) return "signal " + std::to_string(WTERMSIG(r.Status));
        return "status " + std::to_string(r.Status);
    }
#endif // MGTEST_HAVE_FORK

    template <class Body>
    void ExpectRefusedNaming(const char* needle, Body body) {
#if MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY
#if MGTEST_HAVE_FORK
        const std::string tagged = std::string("Fatal{ProtocolCorruption} ") + needle;
        const ChildResult child = RunInChild(body);
        EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child) << "; log: " << child.Log;
        EXPECT_NE(child.Log.find(tagged), std::string::npos)
            << "the gate fired without naming what it refused; wanted \"" << tagged
            << "\"; log: " << child.Log;
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

    GLuint MakeBuffer(GLenum target, GLsizeiptr size) {
        GLuint name = 0;
        GL::GenBuffers(1, &name);
        GL::BindBuffer(target, name);
        GL::BufferData(target, size, nullptr, GL_DYNAMIC_DRAW);
        return name;
    }

    // The frontend keeps the touched high-water mark per target for the life of the context and
    // only ever raises it, so a case that binds point 83 raises it for every case after it in
    // the same process. Each case therefore states the window it expects rather than assuming
    // it is the number of points it just bound.
    Uint32 TouchedWindow(BufferTarget target) {
        return static_cast<Uint32>(Ctx().GetTouchedBufferBindingPointCount(target));
    }

    TEST(ShaderBufferEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
        EXPECT_EQ(&MGPipeShaderBufferEmitterInstance(), &MGPipeShaderBufferEmitterInstance());
    }

    // PROPERTY 1, and it is what makes this whole family cost one integer read per validate
    // point on a workload that binds no indexed buffer - which is every Minecraft frame:
    // MC's touched SSBO and atomic-counter counts are both 0.
    TEST(ShaderBufferEmit, AnUntouchedClassEmitsNothingWithoutHashing) {
        EmitterScope scope;
        ASSERT_EQ(TouchedWindow(BufferTarget::AtomicCounter), 0u)
            << "this case needs a class nothing in this process has ever bound";
        EXPECT_EQ(Emitter().EmitShaderBuffers(Ctx()), 0u);
        EXPECT_EQ(Emitter().EmissionCount(kMGPipeShaderBufferClassAtomicCounter), 0u);
        EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassAtomicCounter], 0u);
    }

    // PROPERTY 2, the one that is silent under run-ahead: glBindBufferBase does not freeze an
    // extent - GL resolves it against the object's size at every USE - so resolving it at emit
    // would make a glBufferData issued between the emission and the apply bind the OLD size,
    // with the right handle and no diagnostic anywhere. glBindBufferRange DOES pin a window,
    // and that one travels resolved.
    TEST(ShaderBufferEmit, ABaseBindingTravelsAsWholeBufferAndARangeBindingTravelsResolved) {
        EmitterScope scope;
        GLint alignment = 0;
        GL::GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
        if (alignment <= 0) alignment = 256;
        const GLuint whole = MakeBuffer(GL_UNIFORM_BUFFER, 1024);
        const GLuint ranged = MakeBuffer(GL_UNIFORM_BUFFER, 1024);
        GL::BindBufferBase(GL_UNIFORM_BUFFER, 0, whole);
        GL::BindBufferRange(GL_UNIFORM_BUFFER, 1, ranged, alignment, 256);

        ASSERT_NE(Emitter().EmitConstBuffers(Ctx()), 0u) << "the uniform window did not emit";
        const MGPShaderBuffers& header = Emitter().LastHeader(kMGPipeShaderBufferClassUniform);
        EXPECT_EQ(header.Class, kMGPipeShaderBufferClassUniform);
        EXPECT_EQ(header.Start, 0u);
        EXPECT_EQ(header.Count, TouchedWindow(BufferTarget::Uniform));
        EXPECT_EQ(header.HostSpanCount, 0u)
            << "kCapNeedsHostUboBytes is 0 for the whole of P5, so Espryt ships no second tail";

        const auto& ranges = Emitter().LastRanges();
        EXPECT_EQ(ranges[0].Offset, 0u);
        EXPECT_EQ(ranges[0].Size, kMGPipeWholeBuffer)
            << "a base binding must let the SERVER re-resolve the extent at use";
        EXPECT_FALSE(MGPipeHandleIsNull(ranges[0].Res));
        EXPECT_EQ(ranges[1].Offset, static_cast<Uint64>(alignment));
        EXPECT_EQ(ranges[1].Size, 256u);
        EXPECT_NE(ranges[0].Res.Slot, ranges[1].Res.Slot) << "two buffers, two handles";

        // And the record reached the applier through the monolith adapter this route installs.
        EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassUniform], header.Count);
        EXPECT_EQ(MGPipeApplier().BoundShaderBuffers[kMGPipeShaderBufferClassUniform][0].Size,
                  kMGPipeWholeBuffer);
    }

    // ID-104's case. The mask is what replaces the backend's own GPU-write walk, and until this
    // phase it was a single Uint32 against an 84-point window - so a storage buffer bound at
    // point 32 or above was unwritable as far as the record said, with nothing able to see it.
    //
    // THE EMITTER HALF BINDS THE HIGHEST POINT THE TARGET ALLOWS rather than a literal 83: the
    // frontend's array is 84 deep, but glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ...) is
    // bounded by GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, which is a DEVICE number and is exactly
    // what ruling 10 says the client must not pre-clamp the WIRE to. The wire-field half - that
    // the mask reaches point 83 at all - is driven through the applier below, where the window
    // is 84 by construction, and pinned again in PipeCatalogueTest.
    TEST(ShaderBufferEmit, OnlyTheWritableClassesSetAMaskAndItReachesTheLastPoint) {
        EmitterScope scope;
        GLint storageBindings = 0;
        GL::GetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &storageBindings);
        ASSERT_GT(storageBindings, 0);
        const Uint32 top = static_cast<Uint32>(storageBindings) - 1u;
        const GLuint storage = MakeBuffer(GL_SHADER_STORAGE_BUFFER, 512);
        const GLuint uniform = MakeBuffer(GL_UNIFORM_BUFFER, 512);
        GL::BindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(top), storage);
        GL::BindBufferBase(GL_UNIFORM_BUFFER, 0, uniform);

        ASSERT_NE(Emitter().EmitShaderBuffers(Ctx()), 0u);
        const MGPShaderBuffers& storageHeader =
            Emitter().LastHeader(kMGPipeShaderBufferClassShaderStorage);
        ASSERT_EQ(storageHeader.Count, top + 1u)
            << "the window is the touched high-water mark, so binding the top point describes "
               "every point below it too";
        EXPECT_TRUE(MGPipeShaderBufferMaskHas(storageHeader.WritableMask, top))
            << "the shader may write through binding point " << top << " and the mask must say so";
        EXPECT_FALSE(MGPipeShaderBufferMaskHas(storageHeader.WritableMask, 0u))
            << "nothing is bound at storage point 0, so nothing is writable there";

        ASSERT_NE(Emitter().EmitConstBuffers(Ctx()), 0u);
        const MGPShaderBuffers& uniformHeader =
            Emitter().LastHeader(kMGPipeShaderBufferClassUniform);
        for (Uint32 w = 0; w < kMGPipeShaderBufferWritableMaskWords; ++w) {
            EXPECT_EQ(uniformHeader.WritableMask[w], 0u)
                << "a UBO is read-only to the shader by definition (word " << w << ")";
        }
        // And the applier carries the mask by class, not by record.
        EXPECT_TRUE(MGPipeShaderBufferMaskHas(
            MGPipeApplier().ShaderBufferWritableMask[kMGPipeShaderBufferClassShaderStorage], top));
        EXPECT_FALSE(MGPipeShaderBufferMaskHas(
            MGPipeApplier().ShaderBufferWritableMask[kMGPipeShaderBufferClassUniform], top));

        // THE WIRE FIELD's OWN HALF (ID-104): a mask bit at the LAST point the window may
        // describe survives the record and the applier. A Uint32 field could not have carried
        // it at all - it would have been silently dropped, and the server would have read the
        // point as read-only while the shader wrote to it.
        const Uint32 last = kMGPipeMaxBufferBindingPoints - 1;
        MGPBufferRange range{};
        range.Res = MGPipeHandle{41, 1};
        range.Size = kMGPipeWholeBuffer;
        MGPShaderBuffers forged{};
        forged.Class = kMGPipeShaderBufferClassAtomicCounter;
        forged.Start = last;
        forged.Count = 1;
        MGPipeShaderBufferMaskSet(forged.WritableMask, last);
        MGPipeApplySetShaderBuffers(forged, &range);
        EXPECT_TRUE(MGPipeShaderBufferMaskHas(
            MGPipeApplier().ShaderBufferWritableMask[kMGPipeShaderBufferClassAtomicCounter], last))
            << "the widened mask must describe the whole 84-point window the record declares";
    }

    // RULING 11. One slot for the call would make each class's emission cancel the previous
    // class's latch: the storage set would go out, the uniform set would go out behind it, and
    // then a storage set that had NOT changed would go out again - or, worse on the other side
    // of the coin, an unchanged uniform set would be re-sent and a changed storage set would
    // not, because the slot held the other class's hash.
    TEST(ShaderBufferEmit, EachClassLatchesItsOwnSuppressorSlot) {
        EmitterScope scope;
        const GLuint storage = MakeBuffer(GL_SHADER_STORAGE_BUFFER, 512);
        const GLuint uniform = MakeBuffer(GL_UNIFORM_BUFFER, 512);
        GL::BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, storage);
        GL::BindBufferBase(GL_UNIFORM_BUFFER, 2, uniform);

        ASSERT_NE(Emitter().EmitConstBuffers(Ctx()), 0u);
        ASSERT_NE(Emitter().EmitShaderBuffers(Ctx()), 0u);
        const Uint64 uniformEmissions = Emitter().EmissionCount(kMGPipeShaderBufferClassUniform);
        const Uint64 storageEmissions =
            Emitter().EmissionCount(kMGPipeShaderBufferClassShaderStorage);

        // Nothing moved: both classes must be suppressed, and each on its OWN latch.
        EXPECT_EQ(Emitter().EmitConstBuffers(Ctx()), 0u);
        EXPECT_EQ(Emitter().EmitShaderBuffers(Ctx()), 0u);
        EXPECT_EQ(Emitter().EmissionCount(kMGPipeShaderBufferClassUniform), uniformEmissions);
        EXPECT_EQ(Emitter().EmissionCount(kMGPipeShaderBufferClassShaderStorage), storageEmissions);

        // ONE class moves. The other must stay suppressed - with a shared slot it would be
        // re-sent, and the case would pass for the wrong reason if it only asserted the mover.
        GL::BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
        EXPECT_NE(Emitter().EmitShaderBuffers(Ctx()), 0u);
        EXPECT_EQ(Emitter().EmissionCount(kMGPipeShaderBufferClassShaderStorage),
                  storageEmissions + 1u);
        EXPECT_EQ(Emitter().EmitConstBuffers(Ctx()), 0u)
            << "the uniform class did not move and must not be re-sent on the storage class's "
               "account";
        EXPECT_EQ(Emitter().EmissionCount(kMGPipeShaderBufferClassUniform), uniformEmissions);
    }

    // The applier's half, driven directly so the window arithmetic is stated rather than
    // inferred from an emission: three classes, three windows, ONE serial.
    TEST(ShaderBufferEmit, TheAppliedWindowIsPerClassAndTheSerialMovesOnEveryRecord) {
        EmitterScope scope;
        MGPBufferRange ranges[2]{};
        ranges[0].Res = MGPipeHandle{11, 1};
        ranges[0].Size = kMGPipeWholeBuffer;
        ranges[1].Res = MGPipeHandle{12, 1};
        ranges[1].Offset = 64;
        ranges[1].Size = 128;

        const Uint64 before = MGPipeApplier().ShaderBuffersSerial;
        MGPShaderBuffers header{};
        header.Class = kMGPipeShaderBufferClassShaderStorage;
        header.Start = 0;
        header.Count = 2;
        MGPipeShaderBufferMaskSet(header.WritableMask, 1);
        MGPipeApplySetShaderBuffers(header, ranges);

        EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassShaderStorage], 2u);
        EXPECT_EQ(MGPipeApplier().BoundShaderBuffers[kMGPipeShaderBufferClassShaderStorage][1].Offset,
                  64u);
        EXPECT_EQ(MGPipeApplier().ShaderBuffersSerial, before + 1u);
        // THE OTHER TWO CLASSES ARE UNTOUCHED, which is the whole reason Class is a field: one
        // record describes one array and says nothing about the other two.
        EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassUniform], 0u);
        EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassAtomicCounter], 0u);

        header.Class = kMGPipeShaderBufferClassUniform;
        header.Count = 1;
        MGPipeApplySetShaderBuffers(header, ranges);
        EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassUniform], 1u);
        EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassShaderStorage], 2u)
            << "the storage window survived a uniform record";
        EXPECT_EQ(MGPipeApplier().ShaderBuffersSerial, before + 2u)
            << "ONE serial for all three classes, and it moves on every applied record";
    }

    // The two refusals the contract names by string (§1), and they are asserted BY NAME: a
    // trip wire that fired without saying which field it refused would leave a reader of the
    // lane guessing between a class nobody allocated and a window nobody can hold.
    TEST(ShaderBufferEmit, AWindowPastTheCapacityOrAnUnknownClassIsRefusedRatherThanStored) {
        EmitterScope scope;
        ExpectRefusedNaming("SetShaderBuffers.Count", [] {
            MGPBufferRange range{};
            range.Res = MGPipeHandle{21, 1};
            MGPShaderBuffers header{};
            header.Class = kMGPipeShaderBufferClassAtomicCounter;
            header.Start = kMGPipeMaxBufferBindingPoints - 1;
            header.Count = 4; // Start + Count = 87 > 84
            const Uint64 before = MGPipeApplier().ShaderBuffersSerial;
            MGPipeApplySetShaderBuffers(header, &range);
            // Only reached on the non-aborting arm, and the point of reaching it is that a
            // refusal which had stored half a window would be worse than an abort.
            EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassAtomicCounter], 0u);
            EXPECT_EQ(MGPipeApplier().ShaderBuffersSerial, before)
                << "a refused record must not move the serial a twin memoises on";
        });
        ExpectRefusedNaming("SetShaderBuffers.Class", [] {
            MGPBufferRange range{};
            range.Res = MGPipeHandle{22, 1};
            MGPShaderBuffers header{};
            header.Class = kMGPipeShaderBufferClassCount; // one past the last class
            header.Start = 0;
            header.Count = 1;
            const Uint64 before = MGPipeApplier().ShaderBuffersSerial;
            MGPipeApplySetShaderBuffers(header, &range);
            EXPECT_EQ(MGPipeApplier().ShaderBuffersSerial, before);
        });
    }

    // A make-current clears the three windows and ADVANCES the serial, for the reason
    // VertexBuffersSerial's own comment gives: a counter that restarts walks back through every
    // value it has already stamped into a backend memo that outlived the switch.
    TEST(ShaderBufferEmit, AResetApplierCarriesNoBindingPointStateOverAndTheSerialAdvances) {
        EmitterScope scope;
        MGPipeApplierState& applier = MGPipeApplier();
        applier.ShaderBufferStart[kMGPipeShaderBufferClassUniform] = 0;
        applier.ShaderBufferCount[kMGPipeShaderBufferClassUniform] = 7;
        applier.BoundShaderBuffers[kMGPipeShaderBufferClassUniform][0].Res = MGPipeHandle{31, 1};
        MGPipeShaderBufferMaskSet(
            applier.ShaderBufferWritableMask[kMGPipeShaderBufferClassShaderStorage],
            kMGPipeMaxBufferBindingPoints - 1);
        applier.ShaderBuffersSerial = 99;

        MGPipeApplierReset();

        EXPECT_EQ(MGPipeApplier().ShaderBufferCount[kMGPipeShaderBufferClassUniform], 0u);
        EXPECT_TRUE(MGPipeHandleIsNull(
            MGPipeApplier().BoundShaderBuffers[kMGPipeShaderBufferClassUniform][0].Res));
        EXPECT_FALSE(MGPipeShaderBufferMaskHas(
            MGPipeApplier().ShaderBufferWritableMask[kMGPipeShaderBufferClassShaderStorage],
            kMGPipeMaxBufferBindingPoints - 1));
        EXPECT_EQ(MGPipeApplier().ShaderBuffersSerial, 100u);
        EXPECT_GT(MGPipeApplier().ShaderBuffersSerial, 99u)
            << "ADVANCED, never zeroed: no value this counter has handed a twin may come back";
    }
} // namespace
#endif // MOBILEGL_PIPE_PUSH

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() /
                          ("mobilegl-shaderbufferemit-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
    // P6: MOBILEGL_LOG_FILE_PATH is a BASE NAME and the library writes one file per role. These
    // cases read the log by OFFSET (a single growing file), and every marker they assert is
    // raised by the encoder on THIS thread - the client role. So g_logPath, which is the read
    // path from here on, becomes the client-derived name; the env keeps the base. The rule is
    // the library's own, not a copy.
    g_logPath = MobileGL::MG_Util::Debug::RoleLogPath(g_logPath.c_str(),
                                                      MobileGL::MG_Util::Debug::LogRole::Client);
#if MOBILEGL_PIPE_PUSH
    MobileGL::Initialize();
#endif
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
