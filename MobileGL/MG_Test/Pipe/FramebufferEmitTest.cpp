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
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Fbo, (MGPipeHandle{4, 1}));
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Width, 100u);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Color[0].InternalFormat, 0x8058u);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.DrawBuffers[0], 0);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer.Fbo, kMGPipeNullHandle)
        << "a Draw record landed in the read binding as well";
    const Uint64 afterDraw = MGPipeApplier().FramebufferSerial;
    EXPECT_GT(afterDraw, serialAtStart) << "an applied record must move the serial the twin memoises";

    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{5, 2}, MGPipeFramebufferTarget::Read, 200));
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer.Fbo, (MGPipeHandle{5, 2}));
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer.Width, 200u);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Fbo, (MGPipeHandle{4, 1}))
        << "a Read record overwrote the draw binding";
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Width, 100u);
    EXPECT_GT(MGPipeApplier().FramebufferSerial, afterDraw);

    // Both: one record, one serial bump, two destinations.
    const Uint64 beforeBoth = MGPipeApplier().FramebufferSerial;
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{6, 3}, MGPipeFramebufferTarget::Both, 300));
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Fbo, (MGPipeHandle{6, 3}));
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer.Fbo, (MGPipeHandle{6, 3}));
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Width, 300u);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer.Width, 300u);
    EXPECT_EQ(MGPipeApplier().FramebufferSerial, beforeBoth + 1)
        << "a Both record is ONE record and moves the serial once";

    // A framebuffer has a handle but NO wire lifetime, so there is no record to refuse against
    // and this entry point never counts an object refusal.
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u);
#endif
}

// A target outside the three is not a binding this server has, and guessing one would put a
// draw's attachments into the read record or the other way round.
TEST(FramebufferEmit, ATargetOutsideTheThreeBindingsIsRefusedNamingTheRecord) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    MGPFramebufferState bad = FramebufferRecord(MGPipeHandle{7, 4}, MGPipeFramebufferTarget::Draw, 100);
    bad.Target = static_cast<Uint8>(MGPipeFramebufferTarget::Count);
    const Uint64 serialBefore = MGPipeApplier().FramebufferSerial;

    ExpectRefusedNaming("set_framebuffer_state {slot=7, gen=4, target=3}: the record names no framebuffer "
                        "binding target",
                        [&bad]() { MGPipeApplySetFramebufferState(bad); });
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Fbo, kMGPipeNullHandle);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer.Fbo, kMGPipeNullHandle);
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
    ASSERT_EQ(MGPipeApplier().DrawFramebuffer.Fbo, (MGPipeHandle{8, 1}));
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

    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Width, 100u) << "a refused record was stored anyway";
    EXPECT_EQ(MGPipeApplier().FramebufferSerial, serialBefore);
#endif
}

// D-J4. The two framebuffer records are per-context WORKING state and a make-current takes
// them - but their serial ADVANCES rather than restarting, because a counter that walks back
// through values it has already stamped into a twin that outlived the switch is not a
// generation at all. Restoring `= 0` anywhere in the reset leaves this red.
TEST(FramebufferEmit, AMakeCurrentClearsBothRecordsAndAdvancesTheSerialRatherThanZeroingIt) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    MGPipeApplySetFramebufferState(FramebufferRecord(MGPipeHandle{4, 1}, MGPipeFramebufferTarget::Both, 100));
    const Uint64 serialBefore = MGPipeApplier().FramebufferSerial;
    ASSERT_EQ(MGPipeApplier().DrawFramebuffer.Width, 100u);

    MGPipeApplierReset(); // the make-current

    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Fbo, kMGPipeNullHandle);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer.Fbo, kMGPipeNullHandle);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Width, 0u);
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

    ASSERT_EQ(MGPipeApplier().DrawFramebuffer.Color[0].Res, (MGPipeHandle{9, 1}));
    ASSERT_EQ(MGPipeApplier().SamplerViewCount, 1u);
    ASSERT_EQ(MGPipeApplier().BoundSamplerViews[2].View, (MGPipeHandle{3, 1}));
    ASSERT_EQ(MGPipeApplier().SamplerStateCount, 1u);
    ASSERT_EQ(MGPipeApplier().BoundSamplerStates[2], samplerState);
    ASSERT_EQ(MGPipeApplier().ShaderImageCount, 1u);
    ASSERT_EQ(MGPipeApplier().BoundShaderImages[2].Res, (MGPipeHandle{9, 1}));

    MGPipeApplierReleaseObjectRecords();

    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Fbo, kMGPipeNullHandle);
    EXPECT_EQ(MGPipeApplier().ReadFramebuffer.Fbo, kMGPipeNullHandle);
    EXPECT_EQ(MGPipeApplier().DrawFramebuffer.Color[0].Res, kMGPipeNullHandle)
        << "a surface handle into an emptied texture table survived the teardown";
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
