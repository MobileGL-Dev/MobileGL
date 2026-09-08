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
#include <MG_Pipe/PipeApply.h>
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
