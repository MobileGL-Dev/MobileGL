// MobileGL - MobileGL/MG_Test/Wire/RemoteClientTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package c1's suite: the 71-slot emit table, the caps mirror and R-8's liveness gates.
// The helper cases below are supplemented by RemoteClientControls.inc: installed producers
// over a live session, with adversarial peer replies and a repeated-make-current control.
//
// IT LINKS gtest RATHER THAN gtest_main AND CARRIES ITS OWN main(), for PipeWireCodecTest's and
// PipeInputsTest's reason: the Fatal arms report through MGLOG_F + std::abort, and MGLOG_F
// writes to STDOUT and to a named file, NEVER to stderr - so EXPECT_DEATH's stderr regex could
// only ever match the empty string. A case that drives one FORKS and reads the Fatal line back
// out of a log file this process names before anything logs. That is what makes each control
// assert ITS OWN failure string (R-16) instead of asserting that something, somewhere, died.

#include <gtest/gtest.h>
#include <MG_Util/Debug/Log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#include "Includes.h"

#include <Config.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Client/CapsMirror.h>
#include <MG_Remote/Transport/ReplySlot.h>
#include <MG_Pipe/PipeRoute.h>
#include <MG_Remote/Client/EmitTables.h>
#include <MG_Remote/Client/WireTables.h>
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Client/PersistentMapTracker.h>
#include <MG_Remote/Client/BackendObject_Remote.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSession.h>
#include <MG_State/GLState/Core.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <Init.h>
#include <MG_Impl/EGLImpl/EGLImpl.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Drawing/GL_Drawing.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <atomic>
#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#define MGTEST_HAVE_FORK 1
#else
#define MGTEST_HAVE_FORK 0
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;
using namespace MobileGL::MG_Remote;
using namespace MobileGL::MG_Remote::Client;

namespace {

    std::string g_logPath;

    // BOTH ROLES' LOGS. P6 gives the client and the server role a file each, and a death test
    // asserts that the CHILD said something - which role's thread said it is not what these
    // cases are about. The refusals raised on the apply thread (RefuseFromApplyThread and every
    // guard that rides it) are written under the SERVER role by construction, so a reader that
    // took only g_logPath would report "the child aborted, but said nothing" for the very
    // diagnostics it exists to check.
    std::string ReadLog() {
        // BOTH ROLES' LOGS (P6). A death test asserts that the CHILD said something; which
        // role's thread said it is not what these cases are about, and refusals raised on the
        // apply thread are written under the SERVER role by construction.
        return MobileGL::MG_Util::Debug::ReadRoleLogs(g_logPath.c_str());
    }

    int ProcessId() {
#if defined(_WIN32)
        return static_cast<int>(::_getpid());
#else
        return static_cast<int>(::getpid());
#endif
    }

    // A caps snapshot the client could plausibly have received, with every field distinct from
    // its default so a mirror that answered from a zeroed struct cannot look like one that
    // adopted. THIS IS NOT THE STATE UNDER TEST - it is the INPUT to Adopt(), which is the
    // producer; the assertions below read what the mirror hands the frontend's own accessors
    // back, never what this function wrote (R-16).
    struct Snapshot {
        MGPCaps Caps{};
        MG_Backend::FormatCapabilityCache Formats{};
        RendererInfo Renderer{};
        String ApiVersion;
        BackendType Backend = BackendType::DirectGLES;
    };

    Snapshot MakeSnapshot(Uint64 consumedSubsystems, Uint64 capBits) {
        Snapshot s;
        s.Caps.CallMask = capBits | MGCapsConsumerBits(consumedSubsystems);
        s.Caps.Dynamic.MaxComputeWorkGroupCount[0] = 65531;
        s.Caps.Dynamic.MaxComputeWorkGroupCount[1] = 65532;
        s.Caps.Dynamic.MaxComputeWorkGroupCount[2] = 65533;
        s.Caps.Dynamic.MaxComputeWorkGroupSize[0] = 1021;
        s.Caps.Dynamic.MaxComputeWorkGroupSize[1] = 1022;
        s.Caps.Dynamic.MaxComputeWorkGroupSize[2] = 1023;
        s.Caps.Dynamic.UniformBufferOffsetAlignment = 64;
        s.Renderer.RendererName = "MobileGL Remote Test Renderer";
        s.Renderer.BackendName = "Espryt";
        s.Renderer.ExtraVendor = String{"c1"};
        s.Renderer.RendererGLInfo.TargetGLVersion = Version{4, 6, 0, {}, {}};
        s.Renderer.RendererGLInfo.TargetGLSLVersion = Version{4, 6, 0, {}, {}};
        s.ApiVersion = "4.6";
        return s;
    }

    void AdoptSnapshot(const Snapshot& s) {
        CapsMirrorInstance().Adopt(s.Caps, s.Formats, s.Renderer, s.ApiVersion, s.Backend);
    }

#if MGTEST_HAVE_FORK
    struct ChildResult {
        int Status = -1;
        std::string Log;
    };

    template <class Body>
    ChildResult RunInChild(Body body) {
        ChildResult result;
        // TRUNCATE, RATHER THAN REMEMBER AN OFFSET, and the difference is not cosmetic: this
        // parent never logs, so MG_Util::Debug's FILE* is opened FOR THE FIRST TIME by each
        // child - with "w", which truncates. An offset taken before the fork therefore points
        // past the end of the child's own log, and `substr` hands back the tail of a DIFFERENT
        // child's output. That is how the second death case in this file came to see the first
        // one's slot name and assert on it: a control reading another control's message, which
        // is one of the three shapes R-16 was written after.
        MobileGL::MG_Util::Debug::TruncateRoleLogs(g_logPath.c_str());
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

    bool DiedOfAbort(const ChildResult& r) {
        return WIFSIGNALED(r.Status) && WTERMSIG(r.Status) == SIGABRT;
    }
    std::string DescribeStatus(const ChildResult& r) {
        if (r.Status < 0) return "fork/waitpid failed";
        if (WIFEXITED(r.Status)) return "exited " + std::to_string(WEXITSTATUS(r.Status));
        if (WIFSIGNALED(r.Status)) return "signal " + std::to_string(WTERMSIG(r.Status));
        return "status " + std::to_string(r.Status);
    }
#endif

} // namespace

// =====================================================================================
// The emit table: the partition, and that no slot is null
// =====================================================================================

TEST(RemoteEmitTable, TheThreeClassesPartitionAllSeventyOneSlots) {
    // P5 baseline five + f1 eleven + i1 seven + t2 six emitted slots.
    EXPECT_EQ(LocallyAnsweredSlotCount(), 2u);
    EXPECT_EQ(ImplementedVerbCount(), 68u);
    EXPECT_EQ(UnmigratedSlotCount(), 1u);
    EXPECT_EQ(LocallyAnsweredSlotCount() + ImplementedVerbCount() + UnmigratedSlotCount(),
              kRemoteEmitSlotCount);
}

TEST(RemoteEmitTable, TheSixXfbAndPatchSlotsAreNonNullAndDistinct) {
    // P5b t2 (CONTRACT-P5B.md §2 t2), the half that needs no fork: the six slots exist and are
    // six DIFFERENT functions. Six identical pointers would be one emitter assigned six times,
    // which is how a copy-paste flip loses five records and still passes every count.
    //
    // Red once by assigning `table.GL.PauseTransformFeedback = &EmitResumeTransformFeedback;`
    // in BuildRemoteEmitTable - the exact copy-paste this guards: "t2 slots 2 and 3 are one
    // function".
    const MG_Backend::GlobalBackendFunctionsTable& table = RemoteEmitTable();
    const void* const six[] = {
        reinterpret_cast<const void*>(table.GL.BeginTransformFeedback),
        reinterpret_cast<const void*>(table.GL.EndTransformFeedback),
        reinterpret_cast<const void*>(table.GL.PauseTransformFeedback),
        reinterpret_cast<const void*>(table.GL.ResumeTransformFeedback),
        reinterpret_cast<const void*>(table.GL.BindTransformFeedback),
        reinterpret_cast<const void*>(table.GL.PatchParameteri),
    };
    for (SizeT i = 0; i < 6; ++i) {
        EXPECT_NE(six[i], nullptr) << "t2 slot " << i << " is null";
        for (SizeT j = i + 1; j < 6; ++j) {
            EXPECT_NE(six[i], six[j]) << "t2 slots " << i << " and " << j << " are one function";
        }
    }
    // And the one XFB slot t2 does NOT flip is still there to be Fatal - CONTRACT-P5B.md gives
    // DeleteTransformFeedback no row (unmeasured), so it must not have been swept up.
    EXPECT_NE(table.GL.DeleteTransformFeedback, nullptr);
}

#if MGTEST_HAVE_FORK
TEST(RemoteEmitTable, EachXfbAndPatchSlotIsClassBAndDemandsASessionByItsOwnName) {
    // P5b t2, THE HALF THAT DECIDES THE CLASS. A pointer comparison cannot tell a class-B
    // emitter from a class-C thunk - each unmigrated slot gets its own generated function, so
    // every slot in the table is already a distinct non-null address. What distinguishes them is
    // WHAT THEY SAY when called with no ClientSession: an emitter reaches RequireSession and
    // dies Fatal{NoClientSession, "<slot>"}; a thunk dies Fatal{UnmigratedVerb, "<slot>"}. Both
    // strings are asserted, because a case that only looked for the first would be satisfied by
    // a build where every one of these had been flipped by accident.
    //
    // Red once by SWAPPING the Pause and Resume assignments in BuildRemoteEmitTable - the two
    // emitters with the same signature, so the swap compiles and neither is orphaned (the first
    // attempt redirected one slot at another's emitter and the build failed on the signature
    // and on -Wunused-function, which is a control that did not run). The child called through
    // PauseTransformFeedback died Fatal{NoClientSession, "ResumeTransformFeedback"} and this
    // case failed with "PauseTransformFeedback did not reach the class-B emitter's session
    // demand", so the string really is the slot's own name and not a shared constant.
    struct Slot {
        const char* Name;
        void (*Call)();
    };
    static const Slot kSlots[] = {
        {"BeginTransformFeedback", [] { RemoteEmitTable().GL.BeginTransformFeedback(0x0004); }},
        {"EndTransformFeedback", [] { RemoteEmitTable().GL.EndTransformFeedback(); }},
        {"PauseTransformFeedback", [] { RemoteEmitTable().GL.PauseTransformFeedback(); }},
        {"ResumeTransformFeedback", [] { RemoteEmitTable().GL.ResumeTransformFeedback(); }},
        {"BindTransformFeedback", [] { RemoteEmitTable().GL.BindTransformFeedback(0); }},
        {"PatchParameteri", [] { RemoteEmitTable().GL.PatchParameteri(0x8E72, 3); }},
    };
    for (const Slot& slot : kSlots) {
        const ChildResult r = RunInChild([&slot] { slot.Call(); });
        ASSERT_TRUE(DiedOfAbort(r)) << slot.Name << ": " << DescribeStatus(r) << "\n" << r.Log;
        EXPECT_NE(r.Log.find(std::string("Fatal{NoClientSession, \"") + slot.Name + "\"}"),
                  std::string::npos)
            << slot.Name << " did not reach the class-B emitter's session demand:\n"
            << r.Log;
        EXPECT_EQ(r.Log.find("Fatal{UnmigratedVerb"), std::string::npos)
            << slot.Name << " is still class C:\n"
            << r.Log;
    }
}

TEST(RemoteEmitTable, DeleteTransformFeedbackHasNoRowAndStillAbortsByItsOwnName) {
    // CONTRACT-P5B.md §2 t2 and c0b-v1.md §6: the seventh slot in t2's ownership block gets NO
    // row in P5b - it is unmeasured, the driver object leaks on the server until P9's XFB
    // namespace work, and a bind of name 0 is what the backend does on delete of the bound one.
    // That is a RULING, so it is pinned rather than left to be re-derived from a count: a later
    // round that gives it a row has to delete this case and say why.
    //
    // Red once by assigning `table.GL.DeleteTransformFeedback = &EmitBindTransformFeedback;` in
    // BuildRemoteEmitTable - a package sweeping the whole XFB family into class B: the child
    // died Fatal{NoClientSession, "BindTransformFeedback"} and the UnmigratedVerb expectation
    // failed.
    const ChildResult r = RunInChild([] { RemoteEmitTable().GL.DeleteTransformFeedback(7); });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{NoClientSession, \"DeleteTransformFeedback\"}"), std::string::npos)
        << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{UnmigratedVerb"), std::string::npos) << r.Log;
}
#endif // MGTEST_HAVE_FORK

TEST(RemoteEmitTable, NoSlotIsNull) {
    // R-4's whole rule, asserted over the STRUCT rather than over the list that built it. 91
    // MG_Impl sites call through this table directly; a null slot is 91 potential null calls,
    // and the one thing a list-driven check could not catch is a slot the list forgot to name.
    //
    // Walked as a block of function pointers because that is exactly what the struct is - the
    // static_asserts in EmitTables.cpp pin that shape - so a slot ADDED to GLFunctionsTable is
    // covered here on the day it appears, without this file being edited.
    const MG_Backend::GlobalBackendFunctionsTable& table = RemoteEmitTable();
    const void* const* cells = reinterpret_cast<const void* const*>(&table);
    const SizeT cellCount = sizeof(table) / sizeof(void*);
    // The struct is 71 function pointers plus ONE cell holding the packed Bool
    // PrefersCpuXfbPrimitiveAccounting and its padding (EmitTables.cpp static_asserts exactly
    // that shape). That Bool is legitimately zero when the server did not publish
    // kCapCpuXfbPrimitiveAccounting, so at most one cell may read null - and this is stated as
    // a bound rather than an index, because an index would drift the day a slot is inserted.
    SizeT nullCells = 0;
    for (SizeT i = 0; i < cellCount; ++i) {
        if (cells[i] == nullptr) ++nullCells;
    }
    EXPECT_LE(nullCells, 1u)
        << "a slot in the remote emit table is null (" << nullCells << " null cells out of "
        << cellCount
        << "). R-4 forbids it: 91 MG_Impl sites call through this table with no null check at all";
}

TEST(RemoteEmitTable, TheFiveEmittersAreTheOnesTheCensusMeasured) {
    const MG_Backend::GlobalBackendFunctionsTable& table = RemoteEmitTable();
    // Named, so that a table which emitted a DIFFERENT five would be red rather than merely
    // counted. The census's answer is Clear, DrawArrays, ReadPixels, BlitFramebuffer, Present.
    EXPECT_NE(table.GL.Clear, nullptr);
    EXPECT_NE(table.GL.DrawArrays, nullptr);
    EXPECT_NE(table.GL.ReadPixels, nullptr);
    EXPECT_NE(table.GL.BlitFramebuffer, nullptr);
    EXPECT_NE(table.Present, nullptr);
    // And the two R-15 answers them locally, so they are not the same pointer as any Fatal one.
    EXPECT_NE(table.GL.GetIntegeri_v, nullptr);
    EXPECT_NE(table.GL.IsTimerQuerySupported, nullptr);
    EXPECT_NE(reinterpret_cast<const void*>(table.GL.DrawArrays),
              reinterpret_cast<const void*>(table.GL.DrawElements))
        << "DrawArrays is class B and DrawElements is class C; they cannot share a thunk";
}

#if MGTEST_HAVE_FORK
TEST(RemoteEmitTable, AnUnmigratedSlotAbortsAndNamesItself) {
    // THE DEATH TEST ON THE UnmigratedVerbFatal ARM. It asserts the exact wording, not merely
    // that the child died: a control that trips on any abort is satisfied by the wrong abort,
    // which is one of the three shapes R-16 was written after.
    // GetTexImage is the wave-3 tail (CONTRACT-P5B.md §7): no P5b package flips it, so this
    // case keeps its subject across the four P5b landings. (It was DrawElements until d1 made
    // that a class-B emitter.)
    const ChildResult r = RunInChild([] { RemoteEmitTable().SetSwapInterval(1); });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedVerb, \"SetSwapInterval\"}"), std::string::npos) << r.Log;
}

TEST(RemoteEmitTable, EachUnmigratedSlotNamesItsOwnSlot) {
    // The half the case above cannot state on its own: that the name in the message is the
    // slot's and not a constant. Two different slots, two different names - both from the
    // wave-3 tail, for the reason the case above gives.
    const ChildResult r = RunInChild([] { RemoteEmitTable().SetSwapInterval(1); });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedVerb, \"SetSwapInterval\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("GetTexImage"), std::string::npos)
        << "the Fatal message names a slot other than the one that was called:\n"
        << r.Log;
}

// P5b d1: a class-B draw slot with no session aborts Fatal{NoClientSession, "<slot>"} - the
// class-B shape - and NOT Fatal{UnmigratedVerb}: that is what distinguishes a flipped slot from
// the stub it replaced, by behaviour rather than by pointer. Red once by: moving DrawElements
// back into MGR_UNMIGRATED_D1_SLOTS - the log then reads UnmigratedVerb.
TEST(RemoteEmitTable, AFlippedDrawSlotDemandsASessionRatherThanNamingItselfUnmigrated) {
    const ChildResult r = RunInChild([] {
        RemoteEmitTable().GL.DrawElements(0x0004 /*GL_TRIANGLES*/, 3, 0x1405 /*GL_UNSIGNED_INT*/,
                                          nullptr);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{NoClientSession, \"DrawElements\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{UnmigratedVerb"), std::string::npos)
        << "DrawElements is class B since d1 and must not name itself unmigrated:\n"
        << r.Log;
}

TEST(RemoteEmitTable, SetSwapIntervalIsClassCAndSaysSo) {
    // The slot the verb census found by NOT mirroring GLImpl: SetSwapInterval has zero MG_Impl
    // call sites and is reached only through the EGL path, so a table built from the 89 GLImpl
    // sites would have left it null.
    const ChildResult r = RunInChild([] { RemoteEmitTable().SetSwapInterval(1); });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedVerb, \"SetSwapInterval\"}"), std::string::npos) << r.Log;
}

#endif // MGTEST_HAVE_FORK

// ---- P5b package i1 (MG_Remote/CONTRACT-P5B.md §2 i1) -------------------------------------

TEST(RemoteEmitTable, TheSevenI1SlotsAreClassBAndAreNotTheFatalThunk) {
    // The census's five measured slots plus the two companions that share their rows. Named
    // rather than counted, so a table that flipped a DIFFERENT seven is red here and not only
    // in the arithmetic. The comparison is against a slot that is still class C: a flipped slot
    // and an unflipped one cannot be the same pointer, which is what a forgotten class-B
    // assignment would look like (class C is assigned FIRST in BuildRemoteEmitTable precisely so
    // that the mistake is loud rather than null).
    const MG_Backend::GlobalBackendFunctionsTable& table = RemoteEmitTable();
    const void* fatal = reinterpret_cast<const void*>(table.SetSwapInterval);
    ASSERT_NE(fatal, nullptr);
    const void* const i1[] = {
        reinterpret_cast<const void*>(table.GL.BindImageTexture),
        reinterpret_cast<const void*>(table.GL.DispatchCompute),
        reinterpret_cast<const void*>(table.GL.DispatchComputeIndirect),
        reinterpret_cast<const void*>(table.GL.MemoryBarrier),
        reinterpret_cast<const void*>(table.GL.MemoryBarrierByRegion),
        reinterpret_cast<const void*>(table.GL.CopyImageSubData),
        reinterpret_cast<const void*>(table.GL.ShaderStorageBlockBinding),
    };
    static const char* const kNames[] = {"BindImageTexture",      "DispatchCompute",
                                         "DispatchComputeIndirect", "MemoryBarrier",
                                         "MemoryBarrierByRegion", "CopyImageSubData",
                                         "ShaderStorageBlockBinding"};
    for (SizeT i = 0; i < sizeof(i1) / sizeof(i1[0]); ++i) {
        EXPECT_NE(i1[i], nullptr) << kNames[i] << " is null";
        EXPECT_NE(i1[i], fatal) << kNames[i]
                                << " still points at an UnmigratedVerbFatal thunk; i1 flipped it "
                                   "to class B";
    }
    // The two barrier slots and the two dispatch slots share a WIRE ROW but not an emitter: the
    // discriminant (ByRegion / IsIndirect) is set by the emitter, so one thunk for both would
    // carry the wrong one.
    EXPECT_NE(i1[3], i1[4]) << "MemoryBarrier and MemoryBarrierByRegion share memory_barrier (61) "
                               "but must set opposite ByRegion values";
    EXPECT_NE(i1[1], i1[2]) << "DispatchCompute and DispatchComputeIndirect share launch_grid (60) "
                               "but must set opposite IsIndirect values";
}

#if MGTEST_HAVE_FORK
TEST(RemoteEmitTable, AClassBSlotWithNoSessionAbortsRatherThanFallingThrough) {
    // The other half of "no slot may fall through to the driver". With no ClientSession the
    // emitter has nowhere to put the record, and the one thing it may not do is return quietly:
    // that is the split lane running monolith and going green.
    const ChildResult r = RunInChild([] { RemoteEmitTable().GL.Clear(0x4000 /*COLOR_BUFFER_BIT*/); });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{NoClientSession, \"Clear\"}"), std::string::npos) << r.Log;
}

TEST(RemoteEmitTable, EachI1SlotReachesRequireSessionUnderItsOwnName) {
    // The behavioural half: an i1 slot is class B, so with no ClientSession it reaches
    // RequireSession and aborts Fatal{NoClientSession, "<slot>"} - NOT Fatal{UnmigratedVerb}
    // (which would mean the flip never happened) and NOT quietly (which is the split lane
    // running monolith and going green, R-4). The name in the message is the slot's own, which
    // is the half a single case could not state.
    //
    // RED ONCE BY DOING X: put `X(MemoryBarrier, void, (GLbitfield))` back in
    // MGR_UNMIGRATED_I1_SLOTS and drop `table.GL.MemoryBarrier = &EmitMemoryBarrier;` - the
    // MemoryBarrier arm below then finds Fatal{UnmigratedVerb, "MemoryBarrier"} instead.
    const ChildResult barrier = RunInChild([] { RemoteEmitTable().GL.MemoryBarrier(0x2000); });
    ASSERT_TRUE(DiedOfAbort(barrier)) << DescribeStatus(barrier) << "\n" << barrier.Log;
    EXPECT_NE(barrier.Log.find("Fatal{NoClientSession, \"MemoryBarrier\"}"), std::string::npos)
        << barrier.Log;
    EXPECT_EQ(barrier.Log.find("Fatal{UnmigratedVerb"), std::string::npos)
        << "MemoryBarrier is class B from P5b i1 on:\n"
        << barrier.Log;

    const ChildResult dispatch = RunInChild([] { RemoteEmitTable().GL.DispatchCompute(1, 1, 1); });
    ASSERT_TRUE(DiedOfAbort(dispatch)) << DescribeStatus(dispatch) << "\n" << dispatch.Log;
    EXPECT_NE(dispatch.Log.find("Fatal{NoClientSession, \"DispatchCompute\"}"), std::string::npos)
        << dispatch.Log;

    const ChildResult bind = RunInChild(
        [] { RemoteEmitTable().GL.BindImageTexture(0, 1, 0, GL_FALSE, 0, 0x88BA, 0x8058); });
    ASSERT_TRUE(DiedOfAbort(bind)) << DescribeStatus(bind) << "\n" << bind.Log;
    EXPECT_NE(bind.Log.find("Fatal{NoClientSession, \"BindImageTexture\"}"), std::string::npos)
        << bind.Log;

    const ChildResult ssbo = RunInChild(
        [] { RemoteEmitTable().GL.ShaderStorageBlockBinding(1, "Blk", 2); });
    ASSERT_TRUE(DiedOfAbort(ssbo)) << DescribeStatus(ssbo) << "\n" << ssbo.Log;
    EXPECT_NE(ssbo.Log.find("Fatal{NoClientSession, \"ShaderStorageBlockBinding\"}"),
              std::string::npos)
        << ssbo.Log;

    const ChildResult copy = RunInChild([] {
        const MG_Backend::CopyImageEndpoint src{};
        const MG_Backend::CopyImageEndpoint dst{};
        RemoteEmitTable().GL.CopyImageSubData(src, 0x0DE1, 0, 0, 0, 0, dst, 0x0DE1, 0, 0, 0, 0, 1,
                                              1, 1);
    });
    ASSERT_TRUE(DiedOfAbort(copy)) << DescribeStatus(copy) << "\n" << copy.Log;
    EXPECT_NE(copy.Log.find("Fatal{NoClientSession, \"CopyImageSubData\"}"), std::string::npos)
        << copy.Log;
}
#endif // MGTEST_HAVE_FORK

// =====================================================================================
// The caps mirror: the three read paths the acceptance names
// =====================================================================================

TEST(CapsMirrorTest, GlGetStringReadsTheRendererStringsBackOutOfTheMirror) {
    // glGetString's path is GL_Getter.cpp:596 -> pActiveBackendObject->GetRendererInfo(), which
    // BackendObject_Remote answers from this mirror BY REFERENCE - so the test reads the
    // reference, holds it across a second adoption, and requires it to follow. A mirror that
    // handed back a temporary would pass an equality check and dangle here.
    const Snapshot first = MakeSnapshot(kMGPipeSubsystemResources, 0);
    AdoptSnapshot(first);
    const RendererInfo& bound = CapsMirrorInstance().Renderer();
    EXPECT_EQ(bound.RendererName, "MobileGL Remote Test Renderer");
    EXPECT_EQ(bound.BackendName, "Espryt");
    ASSERT_TRUE(bound.ExtraVendor.has_value());
    EXPECT_EQ(*bound.ExtraVendor, "c1");

    Snapshot second = MakeSnapshot(kMGPipeSubsystemResources, 0);
    second.Renderer.RendererName = "A Different Device";
    AdoptSnapshot(second);
    EXPECT_EQ(bound.RendererName, "A Different Device")
        << "GetRendererInfo() returns a reference, so a re-arrival must be visible through a "
           "reference a caller already holds";
}

TEST(CapsMirrorTest, GlGetIntegervReadsTheDynamicParametersBackOutOfTheMirror) {
    // glGetIntegerv's limit family is GL_Getter.cpp:2400 -> GetDynamicParameters(), which binds
    // a reference and then reads many members - which is why a partial snapshot is not an
    // option and why the whole struct crosses.
    AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemResources, 0));
    const MG_Backend::DynamicBackendParameters& dynamic = CapsMirrorInstance().Dynamic();
    EXPECT_EQ(dynamic.MaxComputeWorkGroupCount[0], 65531);
    EXPECT_EQ(dynamic.MaxComputeWorkGroupCount[2], 65533);
    EXPECT_EQ(dynamic.MaxComputeWorkGroupSize[1], 1022);
    EXPECT_EQ(dynamic.UniformBufferOffsetAlignment, 64u);
}

TEST(CapsMirrorTest, GlGetStringiReadsTheAdvertisedExtensionListBackOutOfTheMirror) {
    // glGetStringi(GL_EXTENSIONS) is GL_Getter.cpp:654 -> GetRendererInfo().RendererGLInfo, the
    // same list CompileEnv.cpp:124 copies into the compile env.
    Snapshot s = MakeSnapshot(kMGPipeSubsystemResources, 0);
    s.Renderer.RendererGLInfo.Extensions.push_back(E_GL_ARB_timer_query);
    AdoptSnapshot(s);
    const auto& extensions = CapsMirrorInstance().Renderer().RendererGLInfo.Extensions;
    ASSERT_EQ(extensions.size(), 1u);
    EXPECT_EQ(extensions[0], E_GL_ARB_timer_query);
    EXPECT_EQ(CapsMirrorInstance().Renderer().RendererGLInfo.TargetGLVersion.Major, 4);
}

TEST(CapsMirrorTest, ReArrivalIsTheInvalidationAndMovesTheGeneration) {
    // R-12 has no Invalidate(), so Generation() is the ONLY thing on the client that can see a
    // server context death - and a client memo has to key on it.
    const Uint64 before = CapsMirrorInstance().Generation();
    AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemResources, 0));
    const Uint64 after = CapsMirrorInstance().Generation();
    EXPECT_EQ(after, before + 1);
    AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemResources, 0));
    EXPECT_EQ(CapsMirrorInstance().Generation(), after + 1);
    EXPECT_TRUE(CapsMirrorInstance().Valid());
}

TEST(CapsMirrorTest, TheBackendTypeIsTheServersAndNeverANewEnumerator) {
    Snapshot s = MakeSnapshot(kMGPipeSubsystemResources, 0);
    s.Backend = BackendType::DirectVulkan;
    AdoptSnapshot(s);
    EXPECT_EQ(CapsMirrorInstance().Backend(), BackendType::DirectVulkan);
    s.Backend = BackendType::DirectGLES;
    AdoptSnapshot(s);
    EXPECT_EQ(CapsMirrorInstance().Backend(), BackendType::DirectGLES);
}

TEST(CapsMirrorTest, ThePrefersCpuXfbAnswerComesFromTheCapBitAndNotFromTheTable) {
    // GLFunctionsTable::PrefersCpuXfbPrimitiveAccounting is a member of the FUNCTION TABLE,
    // which is exactly the thing a split client never receives - so it cannot ride in
    // MGPCaps::Dynamic and must come from kCapCpuXfbPrimitiveAccounting.
    AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemResources, 0));
    EXPECT_FALSE(CapsMirrorInstance().PrefersCpuXfbPrimitiveAccounting());
    AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemResources, kCapCpuXfbPrimitiveAccounting));
    EXPECT_TRUE(CapsMirrorInstance().PrefersCpuXfbPrimitiveAccounting());
}

// =====================================================================================
// R-8: the liveness gates read the caps mirror, and a family with no consumer says so
// =====================================================================================

TEST(CapsMirrorTest, AMaskWithoutAFamilyRefusesItAndNamesIt) {
    // R-8's NEGATIVE CONTROL. "The client emits nothing for a family the server does not
    // consume" is, on its own, indistinguishable from "nothing called it" - so the refusal is
    // COUNTED at the one funnel that answers the question, and the count is what this asserts.
    AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemPrograms, 0));
    ResetConsumerRefusalsForTest();

    EXPECT_TRUE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemPrograms));
    EXPECT_EQ(ConsumerRefusals(), 0u) << "a family the server DOES consume must not be counted "
                                         "as refused";

    EXPECT_FALSE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemResources));
    EXPECT_EQ(ConsumerRefusals(), 1u);
    EXPECT_EQ(LastRefusedSubsystem(), kMGPipeSubsystemResources)
        << "the refusal must name the family; a counter that only says 'something was withheld' "
           "cannot tell five silent families apart";

    EXPECT_FALSE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemTextureResources));
    EXPECT_EQ(ConsumerRefusals(), 2u);
    EXPECT_EQ(LastRefusedSubsystem(), kMGPipeSubsystemTextureResources);
}

// =====================================================================================
// F1 (P7 wave 2): the placeholder is not an answer
// =====================================================================================
//
// THE CASE THIS REPLACES SAID THE OPPOSITE, and it was wrong in the one way a green test can
// be. `APlaceholderMirrorConsumesNothing` asserted that a mirror with no snapshot answers
// "no consumer" for every family, and called it "the safe direction ... the client emits
// nothing and the legacy pull path runs". There is no legacy pull path on the CLIENT under
// split. What actually happened, measured on lavapipe and on an Adreno 830: MG_State::Init()
// built GLContext's default texture objects before MG_Backend::Init() had started the session,
// every one of their resource_create records was withheld against callMask=0 at generation 0,
// and the only trace of it was one WARN line. The mirror must refuse, not answer.

TEST(CapsMirrorTest, APlaceholderMirrorRefusesToAnswerAndNamesTheFamily) {
#if MGTEST_HAVE_FORK
    // A DEATH, AND ITS OWN STRING (R-16). The family word and the subsystem both have to be in
    // the line, because "something aborted" is satisfied by any of the other 43 families.
    const ChildResult result = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        // AND THE SECOND HALF OF THE ARMING CONDITION, set by hand here because this suite
        // does not run MG_ConfigLoader::Init: the gate is for a process whose CONFIGURATION
        // asked for a split transport, i.e. one that is going to bring a client half up.
        MG_Config::SplitTransportRequestedByConfig = true;
        CapsMirror mirror;
        EXPECT_FALSE(mirror.Valid());
        // No session, no bring-up in flight: the wait cannot succeed and must not be taken.
        (void)mirror.ServerConsumes(kMGPipeSubsystemTextureResources);
        std::fflush(nullptr);
        ::_exit(0);
    });
    ASSERT_TRUE(DiedOfAbort(result)) << DescribeStatus(result) << "\n" << result.Log;
    EXPECT_NE(result.Log.find("Fatal{CapsBeforeFirstSnapshot, \"TextureResources\"}"),
              std::string::npos)
        << "the refusal must name the family that asked; a death with no word is invisible to "
           "the census, to run_trace_case.cmake and to every red-once:\n"
        << result.Log;
    EXPECT_NE(result.Log.find("0x400"), std::string::npos)
        << "the subsystem bit belongs in the line beside its word:\n"
        << result.Log;
#else
    GTEST_SKIP() << "the Fatal arm is only observable through a fork";
#endif
}

TEST(CapsMirrorTest, ThePlaceholderWaitsForASnapshotAnotherThreadIsFetching) {
#if MGTEST_HAVE_FORK
    // THE POSITIVE HALF OF THE WAIT, and the only shape in which "block with a bounded wait" is
    // a correct answer to F1: a bring-up that is genuinely in flight, on a thread that is not
    // the asking one. The case above is why the wait cannot be the WHOLE fix - the production
    // defect had the emitter and the handshake on ONE thread, in sequence, and no wait can
    // help there.
    //
    // DRIVEN THROUGH THE PRODUCTION BRING-UP, not a flag this case sets: MG_Backend::Init() is
    // what creates the server's backend, publishes its real mask and starts the session, and
    // the in-flight fact the wait reads is ClientSession's own BringUpScope. The server's first
    // snapshot is held back by the F1 test knob so the window is wide enough to enter on
    // purpose rather than by luck - and by LESS than the wait's own budget, so a wait that is
    // taken succeeds instead of expiring.
    const ChildResult result = RunInChild([] {
        ::alarm(30);
        ::setenv("MOBILEGL_TEST_DELAY_FIRST_CAPS_MS", "50", 1);
        // A FORK CHILD INHERITS WHATEVER THE PREVIOUS CASE ADOPTED, and a mirror that is
        // already valid would make this case a statement about that snapshot rather than about
        // the placeholder window. Back to generation 0 first, deliberately and by name.
        ResetCapsMirrorForTest();
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MG_Config::SplitTransportRequestedByConfig = true;
        MG_Config::ActiveBackendType = BackendType::DirectVulkan;
        MG_Config::Features.PipePush = kMGPipeSubsystemsMigratedAtP5e;
        MG_Pipe::MGPipeSetResourceOps(nullptr);

        std::atomic<bool> answered{false};
        std::atomic<bool> consumes{false};
        std::thread reader([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!BringUpInFlight() && PublishedCapsGeneration() == 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            consumes.store(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemResources));
            answered.store(true);
        });
        MG_Backend::Init();
        reader.join();

        EXPECT_TRUE(answered.load()) << "the reader never returned from ServerConsumes";
        EXPECT_TRUE(consumes.load())
            << "a reader that asked DURING the bring-up must get the server's real answer - "
               "Magma publishes the resource family - rather than the placeholder's false";
        EXPECT_GT(PublishedCapsGeneration(), 0u);
        ClientSessionInstance().Stop();
        MG_Remote::Server::ServerLoopInstance().Stop();
        std::fflush(nullptr);
        ::_exit(::testing::Test::HasFailure() ? 1 : 0);
    });
    ASSERT_GE(result.Status, 0) << "fork/waitpid failed";
    ASSERT_TRUE(WIFEXITED(result.Status)) << DescribeStatus(result) << "\n" << result.Log;
    EXPECT_EQ(WEXITSTATUS(result.Status), 0) << result.Log;
#else
    GTEST_SKIP() << "the production bring-up is process-global and needs fork to isolate";
#endif
}

TEST(CapsMirrorTest, ARealSnapshotThatWithholdsAFamilyStillAnswersFalse) {
    // THE OTHER NEGATIVE CONTROL, and the one that keeps this package from being a mask
    // defaulted to all-ones: a server that really does not consume a family is still answered
    // "no", counted and named. Only the PLACEHOLDER is refused.
    AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemPrograms, 0));
    ResetConsumerRefusalsForTest();
    EXPECT_TRUE(CapsMirrorInstance().Valid());
    EXPECT_FALSE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemTextureResources));
    EXPECT_EQ(ConsumerRefusals(), 1u);
    EXPECT_EQ(LastRefusedSubsystem(), kMGPipeSubsystemTextureResources);
}

TEST(CapsMirrorTest, TheReadAccessorsStillAnswerFromThePlaceholder) {
    // P5's ruling for the READ accessors is NOT narrowed by F1, and it must not be: LogBackendInfo
    // reads GetRendererInfo() during MG_Backend::Init() and an abort there is a process that
    // cannot start. Only ServerConsumes - a decision, not a read - changed.
    MGPCaps empty{};
    CapsMirror mirror;
    EXPECT_FALSE(mirror.Valid());
    EXPECT_EQ(mirror.Generation(), 0u);
    // (F1 made ServerConsumes on a placeholder a refusal, so the two decision reads that used to
    // sit here belong to the case above; the READ accessors below are what this case pins.)
    // P7 wave 2 package C, OQ-10: the NEGATIVE half of the resident-sub-data pair. It used to
    // be vacuous - nothing in the tree ever set kCapResidentSubData, so every reading of it was
    // false - and MagmaTransportPublishesRealBufferConsumersWithoutRunAhead below is the
    // positive half that makes this one mean "a mirror with no snapshot withholds the bit"
    // rather than "the bit does not exist". Withholding is the SAFE direction: the client then
    // keeps the ordered in-place host write (BufferObject::LandBytesIntoResidentStore) instead
    // of emitting opcode 49 at a server that might have no arm for it - ID-39 in miniature.
    EXPECT_FALSE(mirror.HasCap(kCapResidentSubData));
    EXPECT_EQ(mirror.CallMask(), 0u);
    EXPECT_EQ(mirror.Backend(), BackendType::Unknown);
    (void)empty;
}

TEST(CapsMirrorTest, ObjectFamilyEmissionTracksIndependentConsumerCapsWithoutBufferOps) {
#if MGTEST_HAVE_FORK
    // Deliberately synthetic masks with no buffer consumer: production Magma now
    // publishes one, but object-family independence must survive a restricted peer.
    // Isolate transport, push mask, caps generation and cached supply environment.
    const ChildResult result = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MG_Config::Features.PipePush = kMGPipeSubsystemsMigratedAtP5e;
        MG_Pipe::MGPipeSetResourceOps(nullptr);
        const auto emits = [](Uint64 family) { return MGPipeP4aFamilyEmits(family, family); };
        Snapshot snapshot = MakeSnapshot(kMGPipeSubsystemTextureResources, 0);
        snapshot.Backend = BackendType::DirectVulkan;
        AdoptSnapshot(snapshot);
        const Uint64 firstGeneration = CapsMirrorInstance().Generation();
        EXPECT_FALSE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemResources));
        EXPECT_TRUE(emits(kMGPipeSubsystemTextureResources));
        EXPECT_FALSE(emits(kMGPipeSubsystemFramebuffer));

        // The former shared buffer-consumer gate cannot distinguish these two
        // snapshots: both withhold bit 7. The production per-family gate must.
        snapshot.Caps.CallMask = MGCapsConsumerBits(kMGPipeSubsystemFramebuffer);
        AdoptSnapshot(snapshot);
        EXPECT_GT(CapsMirrorInstance().Generation(), firstGeneration);
        EXPECT_FALSE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemResources));
        EXPECT_FALSE(emits(kMGPipeSubsystemTextureResources));
        EXPECT_TRUE(emits(kMGPipeSubsystemFramebuffer));
        snapshot.Caps.CallMask = MGCapsConsumerBits(kMGPipeSubsystemTextureResources);
        AdoptSnapshot(snapshot);
        EXPECT_TRUE(emits(kMGPipeSubsystemTextureResources));
        EXPECT_FALSE(emits(kMGPipeSubsystemFramebuffer));
        // Object accessors were retired by P5f rather than made record-supplied;
        // this checks the actual per-family emitter gate, not a synthetic field mask.
        std::fflush(nullptr);
        ::_exit(::testing::Test::HasFailure() ? 1 : 0);
    });
    ASSERT_GE(result.Status, 0) << "fork/waitpid failed";
    ASSERT_TRUE(WIFEXITED(result.Status)) << DescribeStatus(result) << result.Log;
    EXPECT_EQ(WEXITSTATUS(result.Status), 0) << result.Log;
#else
    GTEST_SKIP() << "process-global consumer isolation requires fork";
#endif
}

// Historical name retained; the real bootstrap must now publish and arm Magma readiness.
TEST(CapsMirrorTest, MagmaTransportPublishesRealBufferConsumersWithoutRunAhead) {
#if MGTEST_HAVE_FORK
    const ChildResult result = RunInChild([] {
        ::alarm(15);
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        MG_Config::ActiveBackendType = BackendType::DirectVulkan;
        MG_Config::Features.PipePush = kMGPipeSubsystemsMigratedAtP5e;
        MG_Config::Ipc.RunAhead = 1;
        MG_Pipe::MGPipeSetResourceOps(nullptr);
        // The production bootstrap creates the server backend and publishes its
        // real mask before any native EGL/Vulkan context exists. No invented caps
        // snapshot or fake resource table can make this test pass.
        MG_Backend::Init();
        auto& server = MG_Remote::Server::ServerSessionInstance();
        EXPECT_TRUE(server.Accepted());
        EXPECT_TRUE(server.CallMaskIsSet());
        if (server.CallMaskIsSet()) {
            const Uint64 mask = server.CallMask();
            EXPECT_TRUE(MGCapsServerConsumes(mask, kMGPipeSubsystemResources));
            EXPECT_TRUE(MGCapsServerConsumes(mask, kMGPipeSubsystemBufferBindings));
            EXPECT_EQ(mask & static_cast<Uint64>(kCapRunAheadApply), static_cast<Uint64>(kCapRunAheadApply));
        }
        const auto* ops = MG_Pipe::MGPipeGetResourceOps();
        EXPECT_NE(ops, nullptr);
        if (ops) {
            EXPECT_NE(ops->Create, nullptr);
            EXPECT_NE(ops->Respecify, nullptr);
            EXPECT_NE(ops->SubData, nullptr);
            EXPECT_NE(ops->FlushRange, nullptr);
            EXPECT_NE(ops->Readback, nullptr);
            EXPECT_NE(ops->Destroy, nullptr);
            EXPECT_NE(ops->MapPersistent, nullptr);
            if (ops->MapPersistent) EXPECT_EQ(ops->MapPersistent({7, 1}, 64, nullptr), nullptr)
                << "Magma must not donate a server address as a client persistent map";
            // P7 wave 2 package C, OQ-10 (CONTRACT-P7 §5.4): THE TABLE DECIDES THE BIT, and
            // this is the assertion that says so rather than asserting a constant. Magma
            // registers a SubDataResident arm (VkBufferManager.cpp's g_vulkanWireResourceOps),
            // so the published mask must carry kCapResidentSubData - and it must carry it
            // BECAUSE of the arm, which is why the two sides are compared to each other
            // instead of both to `true`. Written this way the case also covers the backend
            // that does NOT have the arm, on the day one exists: no enum is consulted here,
            // and deleting the `|=` in MG_Backend/Init.cpp reds exactly this line (red-once).
            EXPECT_EQ((server.CallMask() & static_cast<Uint64>(kCapResidentSubData)) != 0,
                      ops->SubDataResident != nullptr)
                << "kCapResidentSubData must be published from the server's own wire resource "
                   "table (ID-39: never from the backend enum). Without it "
                   "MGPipeResourceOpsHaveSubDataResident answers false under every transport "
                   "and both backends fall back to the in-place memcpy, which makes Magma's "
                   "SubDataResident arm dead code on the wire";
            EXPECT_NE(ops->SubDataResident, nullptr)
                << "Magma's wire resource table lost its resident sub-data arm; the bit above "
                   "would then correctly go dark and opcode 49 would stop crossing";
        }
        EXPECT_TRUE(ClientSessionInstance().RunAheadArmed());
        ClientSessionInstance().Stop();
        MG_Remote::Server::ServerLoopInstance().Stop();
        std::fflush(nullptr);
        ::_exit(::testing::Test::HasFailure() ? 1 : 0);
    });
    ASSERT_GE(result.Status, 0) << "fork/waitpid failed";
    ASSERT_TRUE(WIFEXITED(result.Status)) << DescribeStatus(result) << result.Log;
    EXPECT_EQ(WEXITSTATUS(result.Status), 0) << result.Log;
#else
    GTEST_SKIP() << "process-global backend bootstrap isolation requires fork";
#endif
}

TEST(CapsMirrorTest, TheConsumerBlockDoesNotCollideWithTheFeatureBits) {
    // The two halves of CallMask, asserted against each other rather than against a constant:
    // bits 0..8 are MGPCapBit and bits 32..47 are the consumer mask, and the whole reason R-8
    // became implementable is that they do not overlap.
    AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemResources | kMGPipeSubsystemPrograms,
                               kCapTimerQuery | kCapOcclusionQuery));
    EXPECT_TRUE(CapsMirrorInstance().HasCap(kCapTimerQuery));
    EXPECT_TRUE(CapsMirrorInstance().HasCap(kCapOcclusionQuery));
    EXPECT_FALSE(CapsMirrorInstance().HasCap(kCapXfbPrimitivesQuery));
    EXPECT_TRUE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemResources));
    EXPECT_TRUE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemPrograms));
    EXPECT_FALSE(CapsMirrorInstance().ServerConsumes(kMGPipeSubsystemSamplers));
}

// =====================================================================================
// E2's emitter-drop control: the switch itself, driven through the real emitter's own counter
// =====================================================================================

// =====================================================================================
// P5b d1: the draw family's record plan (MG_Remote/CONTRACT-P5B.md §2 d1). These drive the
// PRODUCTION derivation the nineteen emitters call - PlanDrawInfo / PlanDrawRange /
// PlanDrawIndirect over a RemoteDrawBindings snapshot - so the fields on the wire are pinned
// here without a GL context (R-16: the production predicate, not a copy). The bindings' READ
// from the real context is the integration lane's (IndexedDrawFamilyScenario under
// DirectGLES.Split.), and the sink's consumption of the same fields is ServerLoopTest's.
// =====================================================================================

namespace {
    MGPipeHandle D1Handle(Uint32 slot) {
        MGPipeHandle h{};
        h.Slot = slot;
        h.Gen = 3;
        return h;
    }
    RemoteDrawBindings D1BoundElementBuffer(Uint32 slot) {
        RemoteDrawBindings b{};
        b.ElementBufferBound = true;
        b.ElementBuffer = D1Handle(slot);
        return b;
    }
} // namespace

// Red once by: returning `offset` instead of `offset / indexSize` from PlanDrawRange's
// element-buffer arm - Start reads 24 below.
TEST(RemoteDrawPlan, AnElementBufferDrawElementsCarriesTheHandleAndItsOffsetInIndices) {
    const RemoteDrawBindings b = D1BoundElementBuffer(17);
    const MGPDrawInfo info = PlanDrawInfo(0x0004 /*GL_TRIANGLES*/, RemoteIndexSizeFor(0x1403 /*USHORT*/),
                                          /*instanceCount=*/1, /*baseInstance=*/0, /*numDraws=*/1, b);
    EXPECT_EQ(info.Mode, 4u);
    EXPECT_EQ(info.IndexSize, 2u);
    EXPECT_EQ(info.IndexResource.Slot, 17u) << "IndexResource must be the VAO's element buffer";
    EXPECT_EQ(info.InstanceCount, 1u);
    EXPECT_EQ(info.StartInstance, 0u);
    EXPECT_EQ(info.Flags, 0u) << "a plain DrawElements sets no flag";
    EXPECT_EQ(info.MinIndex, ~0u);
    EXPECT_EQ(info.NumDraws, 1u);

    MGPDrawRange range{};
    ASSERT_TRUE(PlanDrawRange(b, 2, reinterpret_cast<const void*>(24), 36, 0, range));
    EXPECT_EQ(range.Start, 12u) << "Start is the byte offset in INDICES (24 / 2)";
    EXPECT_EQ(range.Count, 36u);
    EXPECT_EQ(range.IndexBias, 0);
}

// Red once by: dropping `out.IndexBias = baseVertex` - IndexBias reads 0 below. And the
// instanced head: swap InstanceCount and StartInstance in PlanDrawInfo - 7 and 2 trade places.
TEST(RemoteDrawPlan, TheInstancedBaseVertexBaseInstanceFormCarriesAllThreeNumbers) {
    const RemoteDrawBindings b = D1BoundElementBuffer(9);
    const MGPDrawInfo info = PlanDrawInfo(0x0004, 4, /*instanceCount=*/7, /*baseInstance=*/2, 1, b);
    EXPECT_EQ(info.InstanceCount, 7u);
    EXPECT_EQ(info.StartInstance, 2u);
    MGPDrawRange range{};
    ASSERT_TRUE(PlanDrawRange(b, 4, reinterpret_cast<const void*>(0), 6, /*baseVertex=*/5, range));
    EXPECT_EQ(range.Start, 0u);
    EXPECT_EQ(range.IndexBias, 5);
    // An instanced call with a count of 0 crosses as 0, never as the plain draw's 1: the sink
    // reads InstanceCount != 1 as instanced, and 0 instances must draw nothing.
    EXPECT_EQ(PlanDrawInfo(0x0004, 4, 0, 0, 1, b).InstanceCount, 0u);
}

// Red once by: making PlanDrawRange's element-buffer arm `return true` on a remainder - the
// misaligned offset below plans as index 3 instead of being refused.
TEST(RemoteDrawPlan, AMisalignedElementOffsetIsRefusedNotRounded) {
    const RemoteDrawBindings b = D1BoundElementBuffer(1);
    MGPDrawRange range{};
    EXPECT_FALSE(PlanDrawRange(b, 4, reinterpret_cast<const void*>(13), 3, 0, range))
        << "13 bytes is not a whole number of 4-byte indices; the emitter refuses "
           "\"<slot>+INDEX_OFFSET\" rather than draw from index 3";
    EXPECT_TRUE(PlanDrawRange(b, 4, reinterpret_cast<const void*>(12), 3, 0, range));
    EXPECT_EQ(range.Start, 3u);
}

// Red once by: setting `out.Start = offset` in the no-element-buffer arm - Start reads the
// pointer's low bits instead of 0 (the staged run starts at its first index).
TEST(RemoteDrawPlan, AClientIndexArrayPlansFromIndexZeroWithNoHandle) {
    RemoteDrawBindings b{}; // nothing bound: `indices` is the application's array
    const std::uint16_t clientIndices[3] = {0, 1, 2};
    const MGPDrawInfo info = PlanDrawInfo(0x0004, 2, 1, 0, 1, b);
    EXPECT_TRUE(MGPipeHandleIsNull(info.IndexResource)) << "no element buffer, no handle";
    MGPDrawRange range{};
    ASSERT_TRUE(PlanDrawRange(b, 2, clientIndices, 3, 0, range));
    EXPECT_EQ(range.Start, 0u);
    EXPECT_EQ(range.Count, 3u);
    // The span itself is added by the emission (kDrawHasUserIndices, count * IndexSize bytes
    // staged); ServerLoopTest's AClientIndexArrayCrossesAsAStagedSpan pins the other side.
}

// Red once by: returning `first * sizeof(float)` or any scaled first for arrays - Start reads
// something other than 56064 below. (Arrays spell `first` through the pointer parameter.)
TEST(RemoteDrawPlan, AnArraysRangeIsFirstAndCountWithNoBias) {
    RemoteDrawBindings b{};
    MGPDrawRange range{};
    ASSERT_TRUE(PlanDrawRange(b, 0, reinterpret_cast<const void*>(static_cast<std::intptr_t>(56064)),
                              16128, 0, range));
    EXPECT_EQ(range.Start, 56064u);
    EXPECT_EQ(range.Count, 16128u);
    EXPECT_EQ(range.IndexBias, 0);
    EXPECT_EQ(PlanDrawInfo(0x0004, 0, 1, 0, 1, b).IndexSize, 0u);
}

// Red once by: dropping `block.ParameterBuffer = ...` for the counted form - the parameter
// handle reads null below and the sink would dispatch the uncounted call.
TEST(RemoteDrawPlan, TheIndirectBlockNamesBothBuffersAndTheCallsOffsets) {
    RemoteDrawBindings b{};
    b.DrawIndirectBuffer = D1Handle(40);
    b.ParameterBuffer = D1Handle(41);
    const MGPDrawIndirect counted = PlanDrawIndirect(b, reinterpret_cast<const void*>(64),
                                                     /*drawCount=*/3, /*stride=*/20,
                                                     /*parameterOffset=*/8, /*hasParameterBuffer=*/true);
    EXPECT_EQ(counted.Buffer.Slot, 40u);
    EXPECT_EQ(counted.ParameterBuffer.Slot, 41u);
    EXPECT_EQ(counted.Offset, 64u);
    EXPECT_EQ(counted.ParameterOffset, 8u);
    EXPECT_EQ(counted.Stride, 20u);
    EXPECT_EQ(counted.DrawCount, 3u);
    const MGPDrawIndirect plain = PlanDrawIndirect(b, reinterpret_cast<const void*>(64), 1, 0, 8, false);
    EXPECT_TRUE(MGPipeHandleIsNull(plain.ParameterBuffer))
        << "an uncounted indirect draw names no parameter buffer even when one is bound";
    EXPECT_EQ(plain.ParameterOffset, 0u);
    EXPECT_EQ(plain.DrawCount, 1u);
    // The head of an indirect record declares no ranges: the server never reads the indirect
    // buffer to learn a count (MGPipeTypes.h kDrawIsIndirect).
    EXPECT_EQ(PlanDrawInfo(0x0004, 4, 1, 0, 0, b).NumDraws, 0u);
}

// Red once by: returning 4 for GL_UNSIGNED_SHORT - the second line below reads 4.
TEST(RemoteDrawPlan, TheThreeIndexTypesSizeAndNothingElseDoes) {
    EXPECT_EQ(RemoteIndexSizeFor(0x1401 /*GL_UNSIGNED_BYTE*/), 1u);
    EXPECT_EQ(RemoteIndexSizeFor(0x1403 /*GL_UNSIGNED_SHORT*/), 2u);
    EXPECT_EQ(RemoteIndexSizeFor(0x1405 /*GL_UNSIGNED_INT*/), 4u);
    EXPECT_EQ(RemoteIndexSizeFor(0x1406 /*GL_FLOAT*/), 0u) << "not an index type: the emitter refuses "
                                                             "\"<slot>+INDEX_TYPE\"";
}

TEST(RemoteEmitTable, TheNineteenDrawSlotsAreEmittersDistinctFromEveryStub) {
    // The table half of d1's flip: each of the nineteen draw pointers is non-null and is NOT the
    // class-C thunk that stood there at the contract commit. Read from the struct (R-16).
    // Red once by: leaving one `table.GL.X = &EmitX;` out of BuildRemoteEmitTable - that slot
    // would then be the MGR_UNMIGRATED thunk, which the macro no longer defines, so the build
    // breaks first; and by re-adding an X row to MGR_UNMIGRATED_D1_SLOTS, which breaks the
    // ownership static_assert. Both are build breaks, which is the point of the arithmetic.
    const MG_Backend::GlobalBackendFunctionsTable& t = RemoteEmitTable();
    const void* stub = reinterpret_cast<const void*>(t.GL.GetTexImage); // a class-C thunk
    const void* draws[19] = {
        reinterpret_cast<const void*>(t.GL.DrawElements),
        reinterpret_cast<const void*>(t.GL.DrawElementsBaseVertex),
        reinterpret_cast<const void*>(t.GL.DrawRangeElements),
        reinterpret_cast<const void*>(t.GL.DrawRangeElementsBaseVertex),
        reinterpret_cast<const void*>(t.GL.DrawElementsInstanced),
        reinterpret_cast<const void*>(t.GL.DrawElementsInstancedBaseVertex),
        reinterpret_cast<const void*>(t.GL.DrawElementsInstancedBaseInstance),
        reinterpret_cast<const void*>(t.GL.DrawElementsInstancedBaseVertexBaseInstance),
        reinterpret_cast<const void*>(t.GL.DrawArraysInstanced),
        reinterpret_cast<const void*>(t.GL.DrawArraysInstancedBaseInstance),
        reinterpret_cast<const void*>(t.GL.MultiDrawArrays),
        reinterpret_cast<const void*>(t.GL.MultiDrawElements),
        reinterpret_cast<const void*>(t.GL.MultiDrawElementsBaseVertex),
        reinterpret_cast<const void*>(t.GL.DrawArraysIndirect),
        reinterpret_cast<const void*>(t.GL.DrawElementsIndirect),
        reinterpret_cast<const void*>(t.GL.MultiDrawArraysIndirect),
        reinterpret_cast<const void*>(t.GL.MultiDrawElementsIndirect),
        reinterpret_cast<const void*>(t.GL.MultiDrawArraysIndirectCount),
        reinterpret_cast<const void*>(t.GL.MultiDrawElementsIndirectCount),
    };
    for (int i = 0; i < 19; ++i) {
        EXPECT_NE(draws[i], nullptr) << "draw slot " << i;
        EXPECT_NE(draws[i], stub) << "draw slot " << i << " is a class-C thunk";
    }
}

TEST(RemoteEmitTable, TheE2DropSwitchStartsDisarmed) {
    // The half a unit case can state. E2's statement - "drop one Clear emission and OpenRA's
    // SSIM falls below 0.99" - is a TRACE LANE's, because the picture is the thing it is about;
    // what belongs here is that the control is off unless someone armed it, so a lane that
    // forgot to disarm cannot look like a lane that was never armed.
    EXPECT_EQ(DroppedClearEmissions(), 0u);
    SetDropClearEmissionForNegativeControl(true);
    SetDropClearEmissionForNegativeControl(false);
    EXPECT_EQ(DroppedClearEmissions(), 0u)
        << "arming and disarming the control must not, by itself, drop anything";
}

// =====================================================================================
// ID-47: a readback larger than a reply slot is refused at the CLIENT, by name
// =====================================================================================

TEST(RemoteReadback, ExactlyTheCapacityPassesAndOneByteMoreIsRefusedByName) {
    // ID-47's boundary pair, and it is THE CALL SITE'S half. The refusal itself is s1's
    // (ReplySlotPool::RequireReadPixelsFits, its own cases in s1-v3.md §1); what c1 owns is
    // that the number handed to it is the one the emitter computes - ID-49's TIGHT extent -
    // and that exactly the cap is legal while one byte more is not. So this drives the real
    // pool with the real helper, over the real arithmetic, and never restates the message.
    //
    // A real pool over a real mapping, because CanHold answers false for a null base and a
    // control built on a default-constructed pool would "refuse" everything for that reason.
    constexpr std::uint32_t kSlots = 8;
    constexpr std::uint64_t kSlotBytes = 2u * 1024u * 1024u;
    std::vector<Uint8> backing(static_cast<size_t>(kSlots) * kSlotBytes);
    Transport::ReplySlotPool pool(backing.data(), backing.size(), kSlots);
    const std::uint64_t cap = pool.MaxReplyBytes();
    ASSERT_GT(cap, 0u) << "the fixture's pool is not configured, so every answer would be refused";

    // ID-47's own number: the E2 retrace snapshot reads 640x480 RGBA8 and it must now FIT.
    EXPECT_TRUE(pool.CanHold(TightReadbackByteCount(640, 480, 0x1908, 0x1401)))
        << "the read ID-47 grew SEG_REPLY for still does not fit";

    pool.RequireReadPixelsFits(724, 724, 0x1908, 0x1401, cap);
    SUCCEED() << "exactly the capacity is not an overflow";

#if MGTEST_HAVE_FORK
    const ChildResult child = RunInChild([&] {
        Transport::ReplySlotPool inner(backing.data(), backing.size(), kSlots);
        inner.RequireReadPixelsFits(640, 480, 0x1908, 0x1401, inner.MaxReplyBytes() + 1);
    });
    EXPECT_TRUE(DiedOfAbort(child)) << "one byte over the slot did not abort: " << DescribeStatus(child);
    // ITS OWN FAILURE STRING, AND THE READ'S OWN NUMBERS. A control that only asserted
    // "something died" would pass on Fatal{NoClientSession}, Fatal{UnmigratedVerb} or a
    // segfault, and this file has four other cases that abort for those reasons.
    EXPECT_NE(child.Log.find("Fatal{ReplyTooLarge"), std::string::npos) << child.Log;
    EXPECT_NE(child.Log.find("ReadPixels 640x480"), std::string::npos)
        << "the message does not name the read, so an operator cannot tell which one: " << child.Log;
    EXPECT_NE(child.Log.find(std::to_string(cap + 1)), std::string::npos)
        << "the message does not carry the byte count";
#else
    GTEST_SKIP() << "the refusal reports through a Fatal + abort and needs fork() to read back";
#endif
}

// =====================================================================================
// ID-49: the pack state never crosses; the client scatters the tight rows
// =====================================================================================

TEST(RemoteReadback, DstSizeIsTheTightExtentAndNeverThePackedOne) {
    // The number v1 allocates on the server. It must not move with the pack state, because the
    // server reads with a NEUTRAL one and cannot see the client's: the first version of this
    // emitter declared GL 8.4.4's PACKED size, v1 allocated exactly that, and the driver then
    // wrote past it - two SEGFAULTs on the joint inproc lane, both backends.
    constexpr GLenum kRgba = 0x1908;
    constexpr GLenum kUByte = 0x1401;
    EXPECT_EQ(TightReadbackByteCount(4, 3, kRgba, kUByte), 4u * 3u * 4u);
    EXPECT_EQ(TightReadbackByteCount(640, 480, kRgba, kUByte), 640u * 480u * 4u);
    // ID-47's own arithmetic depends on this: 640x480 RGBA8 is what the E2 retrace snapshot
    // reads, and 1,228,800 is the number that forced SEG_REPLY to grow.
    EXPECT_EQ(TightReadbackByteCount(640, 480, kRgba, kUByte), 1228800u);
}

TEST(RemoteReadback, TheTightRowsAreScatteredWhereThePackStateSaysAndTheGapsAreLeftAlone) {
    // ID-49's control, and it is the exact case the cross-family review found: 4x3 RGBA8 with
    // PACK_ROW_LENGTH=8, SKIP_ROWS=1, SKIP_PIXELS=2. Stride = 8*4 = 32; the first written byte
    // is 1*32 + 2*4 = 40; each row writes 4*4 = 16 bytes and the remaining 16 of its stride
    // belong to the application.
    constexpr Uint64 kBpp = 4;
    constexpr GLsizei kW = 4;
    constexpr GLsizei kH = 3;
    constexpr Uint8 kSentinel = 0xCD;

    PixelStoreParameters pack{};
    pack.RowLength = 8;
    pack.SkipRows = 1;
    pack.SkipPixels = 2;
    pack.Alignment = 4;

    // THE INPUT, not the state under test: every source byte is distinct, so a scatter that
    // wrote the right COUNT of bytes from the wrong offset cannot look correct.
    std::vector<Uint8> tight(static_cast<size_t>(kW) * kH * kBpp);
    for (size_t i = 0; i < tight.size(); ++i) tight[i] = static_cast<Uint8>(i);

    std::vector<Uint8> destination(512, kSentinel);
    ScatterTightReadbackIntoPackState(tight.data(), destination.data(), kW, kH, kBpp, pack);

    const size_t stride = 8 * 4;
    const size_t first = 1 * stride + 2 * 4;
    for (size_t row = 0; row < static_cast<size_t>(kH); ++row) {
        for (size_t byte = 0; byte < static_cast<size_t>(kW) * kBpp; ++byte) {
            EXPECT_EQ(destination[first + row * stride + byte],
                      tight[row * static_cast<size_t>(kW) * kBpp + byte])
                << "row " << row << " byte " << byte << " landed somewhere else";
        }
    }

    // THE GAPS, which is the half that makes this a control rather than a copy of the loop
    // above: the bytes the pack state does not name belong to the application and must still
    // hold their sentinel. Removing the skips from the scatter passes the loop above and fails
    // here; widening the per-row copy to the stride passes both loops above and fails here.
    size_t touched = 0;
    for (size_t i = 0; i < destination.size(); ++i) {
        const bool inWrittenRow =
            i >= first && ((i - first) % stride) < static_cast<size_t>(kW) * kBpp &&
            ((i - first) / stride) < static_cast<size_t>(kH);
        if (!inWrittenRow) {
            EXPECT_EQ(destination[i], kSentinel)
                << "byte " << i << " is outside the rectangle GL names and was overwritten";
        } else {
            ++touched;
        }
    }
    EXPECT_EQ(touched, tight.size()) << "the scatter wrote a different number of bytes than the "
                                        "reply carried";
}

TEST(RemoteReadback, PackSkipImagesIsIgnoredForATwoDimensionalRead) {
    // codex 6: SKIP_IMAGES (and IMAGE_HEIGHT) are image-level pack parameters and GL ignores
    // them for glReadPixels, a 2-D read - the monolith conversion path says so with
    // honorPackImageParams=false (DirectGLES.cpp:10905). The first cut applied SKIP_IMAGES as a
    // whole-image offset: a 4x3 RGBA8 read with SKIP_IMAGES=1 wrote bytes 48..95 of a 96-byte
    // destination sized for one image, overrunning it. With the fix the reply lands at bytes
    // 0..47 and the second image's worth of bytes keeps its sentinel.
    constexpr Uint64 kBpp = 4;
    constexpr GLsizei kW = 4;
    constexpr GLsizei kH = 3;
    constexpr Uint8 kSentinel = 0xEE;

    PixelStoreParameters pack{};
    pack.SkipImages = 1;      // the parameter under test
    pack.ImageHeight = kH;    // and its companion; both must be ignored

    std::vector<Uint8> tight(static_cast<size_t>(kW) * kH * kBpp);
    for (size_t i = 0; i < tight.size(); ++i) tight[i] = static_cast<Uint8>(i + 1);

    std::vector<Uint8> destination(96, kSentinel);
    ScatterTightReadbackIntoPackState(tight.data(), destination.data(), kW, kH, kBpp, pack);

    for (size_t i = 0; i < tight.size(); ++i)
        EXPECT_EQ(destination[i], tight[i]) << "byte " << i << " should hold the reply at offset 0";
    for (size_t i = tight.size(); i < destination.size(); ++i)
        EXPECT_EQ(destination[i], kSentinel)
            << "byte " << i << " is past the read's own extent and SKIP_IMAGES must not have moved "
               "the write there";
    // And the fast path takes it: an otherwise-neutral read with only SKIP_IMAGES set is tight.
    EXPECT_TRUE(ReadbackPackStateIsTightForTest(kW, kBpp, pack))
        << "SKIP_IMAGES alone must not force the bounce path for a 2-D read";
}

TEST(RemoteReadback, TheFastPathIsTakenExactlyWhenTheScatterWouldChangeNothing) {
    // EmitReadPixels reads the reply STRAIGHT into the application's pointer when
    // ReadbackPackStateIsTight says so, and pays for a bounce buffer otherwise. That is only
    // legal if the predicate and the scatter AGREE - so this case drives BOTH and compares
    // them, rather than testing either alone. A predicate that said "tight" for a layout the
    // scatter would have rearranged is a silently wrong picture with no bounce to blame.
    constexpr Uint64 kBpp = 4;
    constexpr GLsizei kW = 5;
    constexpr GLsizei kH = 3;
    std::vector<Uint8> tight(static_cast<size_t>(kW) * kH * kBpp);
    for (size_t i = 0; i < tight.size(); ++i) tight[i] = static_cast<Uint8>(i * 7 + 1);

    // Six layouts, chosen so both answers appear: a bare default, an explicit equal row
    // length, an alignment the row already satisfies, an alignment it does not, a skip, and a
    // wider row. If every case agreed on "tight" the comparison below would be vacuous, so the
    // count of each answer is asserted too.
    std::vector<PixelStoreParameters> layouts(6);
    layouts[1].RowLength = kW;
    layouts[2].Alignment = 4;   // 5*4 = 20, already a multiple of 4
    layouts[3].Alignment = 8;   // 20 is not a multiple of 8 - the rows gain padding
    layouts[4].SkipPixels = 1;
    layouts[5].RowLength = 8;

    int tightCount = 0;
    for (size_t i = 0; i < layouts.size(); ++i) {
        const PixelStoreParameters& pack = layouts[i];
        std::vector<Uint8> destination(4096, 0);
        ScatterTightReadbackIntoPackState(tight.data(), destination.data(), kW, kH, kBpp, pack);
        destination.resize(tight.size());
        const bool scatterChangedNothing = (destination == tight);
        const bool predicateSaysTight =
            ReadbackPackStateIsTightForTest(kW, kBpp, pack) != 0;
        EXPECT_EQ(predicateSaysTight, scatterChangedNothing)
            << "layout " << i << ": the fast-path predicate and the scatter disagree";
        if (predicateSaysTight) ++tightCount;
    }
    EXPECT_GT(tightCount, 0) << "no layout took the fast path, so the equality above is vacuous";
    EXPECT_LT(tightCount, static_cast<int>(layouts.size()))
        << "every layout took the fast path, so the equality above is vacuous";
}

// =====================================================================================
// R-17: the routing, its reply mailbox, and the arm that is actually installed
// =====================================================================================

TEST(PipeRouting, TheTablesAreInstalledWithoutAnybodyHavingRememberedTo) {
    // The gate on the install MECHANISM rather than on the table contents (PipeCatalogueTest
    // owns the partition). Nothing in this file calls an installer; the tables are installed
    // because MG_Pipe/PipeRoute.h's inline variable is in this binary, which is the property
    // that a static initialiser inside PipeRoute.cpp did NOT have - the linker dropped that
    // object from every test binary that named no symbol in it, and five CsoCacheTest cases
    // took a null function pointer.
    EXPECT_TRUE(MGPipeTablesAreInstalled());
    EXPECT_EQ(static_cast<int>(MGPipeInstalledArm()), static_cast<int>(MGPipeRouteArm::kMonolith))
        << "this process has no ClientSession, so the monolith adapters must be the arm";
}

TEST(PipeRouting, AnAnswerIsWhatTheRowSaidAndDeclinedIsFalseRatherThanAFailure) {
    const Uint64 takenBefore = MGPipeRepliesTaken();
    const Uint64 declinedBefore = MGPipeRepliesDeclined();

    const MGPReplySlot ok = MGPipeMintReplySlot();
    MGPipePostReply(ok, 0 /*OK*/, 1);
    EXPECT_TRUE(MGPipeTakeReplyBool(ok, "unit"));

    const MGPReplySlot declined = MGPipeMintReplySlot();
    MGPipePostReply(declined, 1 /*DECLINED*/, 0);
    EXPECT_FALSE(MGPipeTakeReplyBool(declined, "unit"))
        << "DECLINED is how the four Bool acceptance rows say false (R-5), not how they fail";

    EXPECT_EQ(MGPipeRepliesTaken(), takenBefore + 2u);
    EXPECT_EQ(MGPipeRepliesDeclined(), declinedBefore + 1u)
        << "the refusal was not COUNTED, so 'the client accepted everything' and 'the client "
           "never asked' are still the same observation from outside";

    // The two slots are different ids, which is what makes the mismatch Fatal below meaningful.
    EXPECT_NE(ok.Id, declined.Id);
    EXPECT_NE(ok.Id, 0u) << "slot 0 is ReplySlot.h's 'no record' and must stay unmintable";
}

#if MGTEST_HAVE_FORK
TEST(PipeRouting, AnUnansweredRowIsFatalRatherThanAcceptedOrRefused) {
    // R-5's whole point, made structural. There is no default: "always accept" is ID-39's 66
    // lost DirectVulkan uploads with a wire in between, and "always refuse" is an emitter that
    // re-sends for ever. A row that forgets to answer has to be impossible to READ.
    const ChildResult child = RunInChild([] {
        const MGPReplySlot slot = MGPipeMintReplySlot();
        (void)MGPipeTakeReplyBool(slot, "resource_create");
    });
    EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child);
    EXPECT_NE(child.Log.find("Fatal{ReplyMissing"), std::string::npos) << child.Log;
    EXPECT_NE(child.Log.find("resource_create"), std::string::npos)
        << "the Fatal does not name the row, so it cannot say WHICH answer went missing";
}

TEST(PipeRouting, TwoOutstandingAnswersAreFatalBecauseTheBarrierIsOneDeep) {
    // The mailbox is one entry deep because R-1's verb barrier makes the in-flight depth one
    // (ReplySlot.h). A second posting before the first is taken is not a capacity problem, it
    // is a barrier that has stopped holding - so it must not be absorbed by a deeper mailbox.
    const ChildResult child = RunInChild([] {
        const MGPReplySlot first = MGPipeMintReplySlot();
        const MGPReplySlot second = MGPipeMintReplySlot();
        MGPipePostReply(first, 0, 1);
        MGPipePostReply(second, 0, 1);
    });
    EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child);
    EXPECT_NE(child.Log.find("Fatal{ReplyOverrun"), std::string::npos) << child.Log;
}

TEST(PipeRouting, AnErrorStatusIsNotFoldedIntoAcceptedOrRefused) {
    // ERROR is a transport fault and DECLINED is a resource decision. Folding the first into
    // either arm of the second makes a broken wire look like a server that said no.
    const ChildResult child = RunInChild([] {
        const MGPReplySlot slot = MGPipeMintReplySlot();
        MGPipePostReply(slot, 2 /*ERROR*/, 0);
        (void)MGPipeTakeReplyBool(slot, "set_texture_params");
    });
    EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child);
    EXPECT_NE(child.Log.find("Fatal{ReplyError"), std::string::npos) << child.Log;
    EXPECT_NE(child.Log.find("set_texture_params"), std::string::npos) << child.Log;
}
#endif // MGTEST_HAVE_FORK

// =====================================================================================
// B3 / codex 9: the CLIENT arm's 38 rows are observed, not just the monolith install
// =====================================================================================

namespace {
    // How many function-pointer cells of `a` differ from `b`, walked as a block of void* -
    // the structs ARE their function pointers (PipeCatalogueTest static_asserts that shape). A
    // routed row that stayed on the monolith adapter reads EQUAL and is not counted, which is
    // exactly the defect this measures.
    template <class T>
    SizeT CountDifferingCells(const T& a, const T& b) {
        const void* const* pa = reinterpret_cast<const void* const*>(&a);
        const void* const* pb = reinterpret_cast<const void* const*>(&b);
        SizeT n = 0;
        for (SizeT i = 0; i < sizeof(T) / sizeof(void*); ++i)
            if (pa[i] != pb[i]) ++n;
        return n;
    }
} // namespace

TEST(PipeRouting, TheInstalledClientArmIsWireAndNotMonolithAndEveryRoutedRowMoved) {
    // WHAT B3 SAYS IS MISSING. PipeCatalogueTest installs and COUNTS the monolith table, so a
    // client row that was never overwritten stays non-null and still counts - deleting one
    // `gMGPipe*.X = &Wire_X` assignment left every gate green (the cross-family verifier
    // reproduced it: catalogue 32/32, all lanes green). The only thing that catches it is
    // observing that the CLIENT install actually MOVED each routed row off the monolith adapter.
    //
    // The monolith adapters are kept beside the installed tables (MGPipeMonolith*()), so the
    // client install differs from them at exactly the routed rows and nowhere else. This reads
    // the pointers back rather than constructing them (R-16): a deleted assignment reads equal.
    InstallClientWireTables();
    EXPECT_EQ(static_cast<int>(MGPipeInstalledArm()), static_cast<int>(MGPipeRouteArm::kClientWire))
        << "InstallClientWireTables did not record the client-wire arm";

    const SizeT movedScreen = CountDifferingCells(gMGPipeScreen, MGPipeMonolithScreen());
#define C1F_MOVED(Table, Row) EXPECT_NE(gMGPipe##Table.Row, MGPipeMonolith##Table().Row) << #Table "." #Row
    C1F_MOVED(Screen, ResourceCreate);
    C1F_MOVED(Screen, ResourceDestroy);
    C1F_MOVED(Screen, UnmapPersistent);
    C1F_MOVED(Context, CreateRenderState);
    C1F_MOVED(Context, BindRenderState);
    C1F_MOVED(Context, DeleteRenderState);
    C1F_MOVED(Context, CreateVertexElements);
    C1F_MOVED(Context, BindVertexElements);
    C1F_MOVED(Context, DeleteVertexElements);
    C1F_MOVED(Context, CreateSamplerState);
    C1F_MOVED(Context, DeleteSamplerState);
    C1F_MOVED(Context, CreateSamplerView);
    C1F_MOVED(Context, DeleteSamplerView);
    C1F_MOVED(Context, BindShaderState);
    C1F_MOVED(Context, DeleteShaderState);
    C1F_MOVED(Context, SetDrawProgram);
    C1F_MOVED(Context, SetDispatchProgram);
    C1F_MOVED(Context, SetDynamicState);
    C1F_MOVED(Context, SetFramebufferState);
    C1F_MOVED(Context, SetVertexBuffers);
    C1F_MOVED(Context, SetIndexBuffer);
    C1F_MOVED(Context, SetSamplerViews);
    C1F_MOVED(Context, BindSamplerStates);
    C1F_MOVED(Context, SetShaderImages);
    // P5e (sb, CONTRACT-P5E.md §5.6): set_shader_buffers is the 35th routed row. It rides both
    // tables like every other set_* with an applier entry point - the class is a FIELD of the
    // payload, so all three binding-point classes go through this one cell.
    C1F_MOVED(Context, SetShaderBuffers);
    C1F_MOVED(Context, SetGlobalConstants);
    C1F_MOVED(Context, SetVertexAttribDefaults);
    C1F_MOVED(Context, SetPixelPackState);
    C1F_MOVED(Context, SetPatchState);
    // P5c rv (CONTRACT-P5C.md §5.3): the residual-value record is the 34th routed row - an
    // ordinary set_* row with an MGPipeApply* entry point, so it rides BOTH tables like its
    // siblings (its producer is transport-gated in PipeFill.cpp, which is a different gate's
    // business).
    C1F_MOVED(Context, SetContextValues);
    C1F_MOVED(Context, SetResidualValueState);
    C1F_MOVED(Context, SetTextureParams);
    C1F_MOVED(Context, ResourceSubData);
    C1F_MOVED(Context, BufferSubDataResident);
    C1F_MOVED(Context, ResourceReadback);
#undef C1F_MOVED
#define C1F_ESCAPE(Row) EXPECT_NE(gMGPipeRouteEscapes.Row, MGPipeMonolithEscapes().Row) << #Row
    C1F_ESCAPE(ResourceRespecify);
    C1F_ESCAPE(ResourceFlushRange);
    C1F_ESCAPE(MapPersistent);
    C1F_ESCAPE(CreateShaderState);
    // P5e (pg): set_program_bindings is the fifth escape - three tails in three index spaces
    // plus a parallel name array, which no generated row can express (MG_Pipe/PipeRoute.h).
    C1F_ESCAPE(SetProgramBindings);
#undef C1F_ESCAPE
    const SizeT movedContext = CountDifferingCells(gMGPipeContext, MGPipeMonolithContext());
    EXPECT_EQ(movedScreen + movedContext, 35u)
        << "exactly the 35 generated routed rows must differ from the monolith adapters "
           "(33 at P5, + set_context_values at P5c rv, + set_shader_buffers at P5e sb); "
        << movedScreen + movedContext
        << " did, so a row was left on the monolith adapter (it would run the applier on the GL "
           "thread under split) or an unrouted row was overwritten";

    const SizeT movedEscapes = CountDifferingCells(gMGPipeRouteEscapes, MGPipeMonolithEscapes());
    EXPECT_EQ(movedEscapes, 5u)
        << "the five escape routes must move off the monolith escapes too";

    // Restore the monolith arm for the sibling cases that assert it (and for a clean binary).
    MGPipeInstallMonolithTables();
    EXPECT_EQ(static_cast<int>(MGPipeInstalledArm()), static_cast<int>(MGPipeRouteArm::kMonolith));
}

#if MGTEST_HAVE_FORK
TEST(PipeRouting, AClientWireRowWithNoSessionRefusesByNameRatherThanApplying) {
    // THE RUNTIME HALF of B3, and it distinguishes a Wire_* row from the monolith adapter by
    // BEHAVIOUR: with the client tables installed and no session, a routed call reaches
    // RequireSession and aborts Fatal{NoClientSession}. The monolith adapter (the deleted-
    // assignment state) would instead run MGPipeApply* and NOT abort with that string - so the
    // control goes red the moment a row falls back to monolith.
    const ChildResult child = RunInChild([] {
        InstallClientWireTables();
        MGPHandleOnly handle{};
        handle.Handle = MGPipeHandle{1, 0};
        handle.Kind = static_cast<Uint32>(MGPipeKind::Renderbuffer);
        gMGPipeScreen.ResourceDestroy(&handle); // Wire_ResourceDestroy, no session
    });
    EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child) << "\n" << child.Log;
    EXPECT_NE(child.Log.find("Fatal{NoClientSession, \"ResourceDestroy\"}"), std::string::npos)
        << "the installed row did not refuse by name; it may be the monolith adapter (B3)\n"
        << child.Log;
}

TEST(PipeRouting, ARoutedCallDuringTeardownRefusesByNameNotRunsTheApplier) {
    // codex 4: UninstallClientWireTables marks the tables uninstalled; a routed call in that
    // window must abort by name rather than run the applier on the caller. Reverting Uninstall
    // to reinstall the monolith adapters (round 2's behaviour) makes this call run the applier
    // and NOT abort with this string - the red-once.
    const ChildResult child = RunInChild([] {
        InstallClientWireTables();
        UninstallClientWireTables();
        MGPHandleOnly handle{};
        handle.Handle = MGPipeHandle{1, 0};
        handle.Kind = static_cast<Uint32>(MGPipeKind::Renderbuffer);
        gMGPipeScreen.ResourceDestroy(&handle);
    });
    EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child) << "\n" << child.Log;
    EXPECT_NE(child.Log.find("Fatal{ClientTablesUninstalled, \"ResourceDestroy\"}"), std::string::npos)
        << "a routed call after UninstallClientWireTables ran the applier on the caller instead "
           "of refusing by name (codex 4)\n"
        << child.Log;
}
#endif // MGTEST_HAVE_FORK

// =====================================================================================
// M2 / codex 11: a short or non-OK reply is refused, never scattered as pixels
// =====================================================================================

TEST(RemoteReadback, AReplyIsScatteredOnlyWhenItIsOkAndExactlyTheReadsExtent) {
    // EmitReadPixels decides on ReadbackReplyIsComplete before it scatters or returns (the Fatal
    // wording each mode owns is at the call site). Driving the production predicate directly:
    // only a full OK reply is complete; a short OK reply, and a DECLINE or ERROR with a zero
    // payload, are not - and those are the shapes that would otherwise spray stale destination
    // bytes as pixels. `tight` is the read's own DstSize (CONTRACT-P5 row 23).
    constexpr Int32 kOk = 0, kDeclined = 1, kError = 2;
    const Uint64 tight = TightReadbackByteCount(4, 3, 0x1908, 0x1401); // 48
    EXPECT_TRUE(ReadbackReplyIsComplete(kOk, tight, tight)) << "a full OK reply is the only one scattered";
    EXPECT_FALSE(ReadbackReplyIsComplete(kOk, tight - 16, tight))
        << "a SHORT OK reply (one row missing) must not be scattered - the missing rows would be "
           "whatever the destination held";
    EXPECT_FALSE(ReadbackReplyIsComplete(kDeclined, 0, tight))
        << "a DECLINED reply carries no pixels";
    EXPECT_FALSE(ReadbackReplyIsComplete(kError, 0, tight)) << "an ERROR reply carries no pixels";
    EXPECT_FALSE(ReadbackReplyIsComplete(kOk, 0, tight)) << "an OK reply of zero bytes is not the extent";
}

#include "RemoteClientControls.inc"
#include <MG_Impl/Pipe/FramebufferEmit.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_State/GLState/TextureState/TextureObject2D.h>

// f1: the installed emitters are decoded by a peer on the apply thread.
#if MGTEST_HAVE_FORK
namespace {
struct F1Peer : Codec::WireVerbSink {
    MGPClear clear{};
    MGPCopyFromFramebuffer copy{};
    MGPMipPlan mip{};
    MGPBlit blit{};
    unsigned calls = 0;
    Bool OnClear(const MGPClear& v) override { clear = v; ++calls; return true; }
    Bool OnCopyFramebufferToTexture(const MGPCopyFromFramebuffer& v) override { copy = v; ++calls; return true; }
    Bool OnGenerateMipmap(const MGPMipPlan& v) override { mip = v; ++calls; return true; }
    Bool OnBlit(const MGPBlit& v) override { blit = v; ++calls; return true; }
    void Install() {
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
            auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
            decoder.SetVerbSink(static_cast<F1Peer*>(self));
            return MOBILEGL_OK;
        }, this) != MOBILEGL_OK) ::_exit(82);
    }
};
}

TEST(RemoteF1, ClearBufferfvFieldsCross) {
    // Red once (executed, reverted): zero the emitted clear values; F1.ClearBufferfv.fields fails.
    const auto child = RunInChild([] {
        StartControlSession();
        F1Peer peer; peer.Install();
        const GLfloat value[4] = {1, 7, 13, 23};

        RemoteEmitTable().GL.ClearBufferfv(GL_COLOR, 3, value);
        const auto& r = peer.clear;
        if (peer.calls != 1 || r.Kind != kMGPipeClearKindColor || r.ValueClass != kMGPipeClearValueClassFloat ||
            r.DrawBufferIndex != 3 || std::memcmp(r.ColorValue, value, sizeof(value)) != 0 ||
            !MGPipeHandleIsNull(r.Fbo)) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.ClearBufferfv.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, ClearNamedFramebufferfvFieldsCross) {
    // Red once (executed, reverted): zero the emitted clear values; F1.ClearNamedFramebufferfv.fields fails.
    const auto child = RunInChild([] {
        StartControlSession();
        F1Peer peer; peer.Install();
        const GLfloat value[4] = {1, 7, 13, 23};
        const auto fbo = MakeShared<MG_State::GLState::FramebufferObject>(73);
        RemoteEmitTable().GL.ClearNamedFramebufferfv(fbo, GL_COLOR, 3, value);
        const auto& r = peer.clear;
        if (peer.calls != 1 || r.Kind != kMGPipeClearKindColor || r.ValueClass != kMGPipeClearValueClassFloat ||
            r.DrawBufferIndex != 3 || std::memcmp(r.ColorValue, value, sizeof(value)) != 0 ||
            r.Fbo != MGPipeFramebufferEmitter::HandleFor(*fbo)) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.ClearNamedFramebufferfv.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, ClearBufferivFieldsCross) {
    // Red once (executed, reverted): zero the emitted clear values; F1.ClearBufferiv.fields fails.
    const auto child = RunInChild([] {
        StartControlSession();
        F1Peer peer; peer.Install();
        const GLint value[4] = {1, 7, 13, 23};

        RemoteEmitTable().GL.ClearBufferiv(GL_COLOR, 3, value);
        const auto& r = peer.clear;
        if (peer.calls != 1 || r.Kind != kMGPipeClearKindColor || r.ValueClass != kMGPipeClearValueClassInt ||
            r.DrawBufferIndex != 3 || std::memcmp(r.ColorValue, value, sizeof(value)) != 0 ||
            !MGPipeHandleIsNull(r.Fbo)) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.ClearBufferiv.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, ClearNamedFramebufferivFieldsCross) {
    // Red once (executed, reverted): zero the emitted clear values; F1.ClearNamedFramebufferiv.fields fails.
    const auto child = RunInChild([] {
        StartControlSession();
        F1Peer peer; peer.Install();
        const GLint value[4] = {1, 7, 13, 23};
        const auto fbo = MakeShared<MG_State::GLState::FramebufferObject>(73);
        RemoteEmitTable().GL.ClearNamedFramebufferiv(fbo, GL_COLOR, 3, value);
        const auto& r = peer.clear;
        if (peer.calls != 1 || r.Kind != kMGPipeClearKindColor || r.ValueClass != kMGPipeClearValueClassInt ||
            r.DrawBufferIndex != 3 || std::memcmp(r.ColorValue, value, sizeof(value)) != 0 ||
            r.Fbo != MGPipeFramebufferEmitter::HandleFor(*fbo)) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.ClearNamedFramebufferiv.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, ClearBufferuivFieldsCross) {
    // Red once (executed, reverted): zero the emitted clear values; F1.ClearBufferuiv.fields fails.
    const auto child = RunInChild([] {
        StartControlSession();
        F1Peer peer; peer.Install();
        const GLuint value[4] = {1, 7, 13, 23};

        RemoteEmitTable().GL.ClearBufferuiv(GL_COLOR, 3, value);
        const auto& r = peer.clear;
        if (peer.calls != 1 || r.Kind != kMGPipeClearKindColor || r.ValueClass != kMGPipeClearValueClassUint ||
            r.DrawBufferIndex != 3 || std::memcmp(r.ColorValue, value, sizeof(value)) != 0 ||
            !MGPipeHandleIsNull(r.Fbo)) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.ClearBufferuiv.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, ClearNamedFramebufferuivFieldsCross) {
    // Red once (executed, reverted): zero the emitted clear values; F1.ClearNamedFramebufferuiv.fields fails.
    const auto child = RunInChild([] {
        StartControlSession();
        F1Peer peer; peer.Install();
        const GLuint value[4] = {1, 7, 13, 23};
        const auto fbo = MakeShared<MG_State::GLState::FramebufferObject>(73);
        RemoteEmitTable().GL.ClearNamedFramebufferuiv(fbo, GL_COLOR, 3, value);
        const auto& r = peer.clear;
        if (peer.calls != 1 || r.Kind != kMGPipeClearKindColor || r.ValueClass != kMGPipeClearValueClassUint ||
            r.DrawBufferIndex != 3 || std::memcmp(r.ColorValue, value, sizeof(value)) != 0 ||
            r.Fbo != MGPipeFramebufferEmitter::HandleFor(*fbo)) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.ClearNamedFramebufferuiv.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, ClearBufferfiFieldsCross) {
    // Red once (executed, reverted): zero the emitted clear values; F1.ClearBufferfi.fields fails.
    const auto child = RunInChild([] {
        StartControlSession(); F1Peer peer; peer.Install();

        RemoteEmitTable().GL.ClearBufferfi(GL_DEPTH_STENCIL, 0, 0.375f, 91);
        const auto& r = peer.clear;
        if (peer.calls != 1 || r.Kind != kMGPipeClearKindDepthStencil || r.DrawBufferIndex != 0 ||
            r.DepthValue != 0.375f || r.StencilValue != 91 ||
            !MGPipeHandleIsNull(r.Fbo)) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.ClearBufferfi.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, ClearNamedFramebufferfiFieldsCross) {
    // Red once (executed, reverted): zero the emitted clear values; F1.ClearNamedFramebufferfi.fields fails.
    const auto child = RunInChild([] {
        StartControlSession(); F1Peer peer; peer.Install();
        const auto fbo = MakeShared<MG_State::GLState::FramebufferObject>(73);
        RemoteEmitTable().GL.ClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, 0.375f, 91);
        const auto& r = peer.clear;
        if (peer.calls != 1 || r.Kind != kMGPipeClearKindDepthStencil || r.DrawBufferIndex != 0 ||
            r.DepthValue != 0.375f || r.StencilValue != 91 ||
            r.Fbo != MGPipeFramebufferEmitter::HandleFor(*fbo)) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.ClearNamedFramebufferfi.fields: " << DescribeStatus(child) << child.Log;
}

namespace {
SharedPtr<MG_State::GLState::TextureObject2D> F1Texture() {
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
    auto tex = MakeShared<MG_State::GLState::TextureObject2D>(91);
    tex->SetInternalFormat(TextureInternalFormat::RGBA8);
    for (Uint level = 0; level != 3; ++level) {
        const Int size = 8 >> level;
        tex->AllocateStorage(TextureUploadTarget::Texture2D, level,
            MG_State::GLState::MipmapInput{IntVec3{size, size, 1}, static_cast<SizeT>(size * size * 4)});
    }
    tex->SetBaseLevel(1);
    MGPipeTextureEmitterInstance().AcquireTexture(tex->GetLifetimeId(), tex.get());
    MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).Bind(tex);
    return tex;
}
}

TEST(RemoteF1, CopyTexImage2DFieldsCross) {
    // Red once (executed, reverted): increment the emitted copy level; F1.CopyTexImage2D.fields fails.
    const auto child = RunInChild([] {
        StartControlSession(); F1Peer peer; peer.Install(); const auto tex = F1Texture();
        RemoteEmitTable().GL.CopyTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA8, -3, 4, 11, 13, 0);
        const auto& r = peer.copy;
        if (peer.calls != 1 || r.Dst != MGPipeTextureEmitterInstance().FindTexture(*tex) ||
            MGPipeHandleIsNull(r.Dst) || r.Target != GL_TEXTURE_2D || r.Level != 2 ||
            r.InternalFormat != GL_RGBA8 || r.X != -3 || r.Y != 4 || r.Width != 11 || r.Height != 13 ||
            r.XOffset != 0 || r.YOffset != 0 || r.SubImage != 0) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.CopyTexImage2D.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, CopyTexSubImage2DFieldsCross) {
    // Red once (executed, reverted): increment the emitted copy level; F1.CopyTexSubImage2D.fields fails.
    const auto child = RunInChild([] {
        StartControlSession(); F1Peer peer; peer.Install(); const auto tex = F1Texture();
        RemoteEmitTable().GL.CopyTexSubImage2D(GL_TEXTURE_2D, 2, 5, 7, -3, 4, 11, 13);
        const auto& r = peer.copy;
        if (peer.calls != 1 || r.Dst != MGPipeTextureEmitterInstance().FindTexture(*tex) ||
            MGPipeHandleIsNull(r.Dst) || r.Target != GL_TEXTURE_2D || r.Level != 2 ||
            r.InternalFormat != 0 || r.X != -3 || r.Y != 4 || r.Width != 11 || r.Height != 13 ||
            r.XOffset != 5 || r.YOffset != 7 || r.SubImage != 1) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.CopyTexSubImage2D.fields: " << DescribeStatus(child) << child.Log;
}

TEST(RemoteF1, GenerateMipmapFieldsCross) {
    // Red once (executed, reverted): increment the emitted base level; F1.GenerateMipmap.fields fails.
    const auto child = RunInChild([] {
        StartControlSession(); F1Peer peer; peer.Install(); const auto tex = F1Texture();
        RemoteEmitTable().GL.GenerateMipmap(GL_TEXTURE_2D);
        // THE FENCE. Under MOBILEGL_IPC_BATCH_WAITS (default on) GenerateMipmap is kCtxObject -
        // a value-class record whose emit no longer waits for its own apply (production-safe:
        // the record carries its whole value and the in-order ring applies it before the next
        // waited verb). The peer lives on the apply thread, so reading it needs a wait
        // boundary: a following kCtxVerb still waits, and its wait covers everything emitted
        // before it. When this clear returns, the peer has seen both records.
        const GLfloat fence[4] = {0, 0, 0, 0};
        RemoteEmitTable().GL.ClearBufferfv(GL_COLOR, 0, fence);
        const auto& r = peer.mip;
        if (peer.calls != 2 || r.Res != MGPipeTextureEmitterInstance().FindTexture(*tex) ||
            MGPipeHandleIsNull(r.Res) || r.Target != GL_TEXTURE_2D || r.BaseLevel != 1 || r.LevelCount != 3)
            ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.GenerateMipmap.fields: " << DescribeStatus(child) << child.Log;
}
#endif



#if MGTEST_HAVE_FORK

namespace {
    MGPipeHandle observedNamedClear{}, observedNamedClearRead{};
    Uint32 observedNamedClearValues[4]{};
    Uint32 observedNamedClearCalls = 0;
    GLenum observedNamedClearTarget = 0;
    GLint observedNamedClearIndex = 0;
    GLfloat observedNamedClearDepth = 0;
    GLint observedNamedClearStencil = 0;
    Bool observedNamedClearRecord = false;

    void ObserveNamedClear(GLenum target, GLint index) {
        const auto& state = MGPipeApplier();
        observedNamedClear = state.BoundFramebuffer[0];
        observedNamedClearRead = state.BoundFramebuffer[1];
        observedNamedClearRecord = state.FramebufferRecordFor(observedNamedClear) != nullptr;
        observedNamedClearTarget = target;
        observedNamedClearIndex = index;
        ++observedNamedClearCalls;
    }

    void CheckNamedClearScopedTarget(Uint8 kind, Uint8 valueClass) {
        MGPipeApplierReset();
        MGPipeResourceOps resources{};
        MGPipeSetResourceOps(&resources);
        MGPFramebufferState state{};
        state.Fbo = {31, 1};
        state.Target = static_cast<Uint8>(MGPipeFramebufferTarget::Draw);
        MGPipeApplySetFramebufferState(state);
        const auto originalDraw = state.Fbo;
        state.Fbo = {32, 2};
        state.Target = static_cast<Uint8>(MGPipeFramebufferTarget::Read);
        MGPipeApplySetFramebufferState(state);
        const auto originalRead = state.Fbo;
        state.Fbo = {33, 3};
        state.Target = static_cast<Uint8>(MGPipeFramebufferTarget::Named);
        MGPipeApplySetFramebufferState(state);
        CapsPeer backend;
        backend.table.GL.ClearBufferfv = +[](GLenum target, GLint index, const GLfloat* values) {
            ObserveNamedClear(target, index);
            std::memcpy(observedNamedClearValues, values, sizeof(observedNamedClearValues));
        };
        backend.table.GL.ClearBufferiv = +[](GLenum target, GLint index, const GLint* values) {
            ObserveNamedClear(target, index);
            std::memcpy(observedNamedClearValues, values, sizeof(observedNamedClearValues));
        };
        backend.table.GL.ClearBufferuiv = +[](GLenum target, GLint index, const GLuint* values) {
            ObserveNamedClear(target, index);
            std::memcpy(observedNamedClearValues, values, sizeof(observedNamedClearValues));
        };
        backend.table.GL.ClearBufferfi = +[](GLenum target, GLint index, GLfloat depth, GLint stencil) {
            ObserveNamedClear(target, index);
            observedNamedClearDepth = depth;
            observedNamedClearStencil = stencil;
        };
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPClear clear{};
        clear.Fbo = state.Fbo;
        clear.Kind = kind;
        clear.ValueClass = valueClass;
        clear.DrawBufferIndex = kind == kMGPipeClearKindDepthStencil ? 0 : 3;
        const Uint32 bits[4]{0x3e800000u, 0x3f000000u, 0xff000011u, 0x3f800000u};
        std::memcpy(clear.ColorValue, bits, sizeof(bits));
        clear.DepthValue = 0.375f;
        clear.StencilValue = 91;
        if (!sink.OnClear(clear) || observedNamedClearCalls != 1) ::_exit(101);
        if (observedNamedClear != clear.Fbo || !observedNamedClearRecord ||
            observedNamedClearRead != originalRead) ::_exit(102);
        if (MGPipeApplier().BoundFramebuffer[0] != originalDraw ||
            MGPipeApplier().BoundFramebuffer[1] != originalRead) ::_exit(103);
        if (observedNamedClearIndex != clear.DrawBufferIndex) ::_exit(104);
        if (kind == kMGPipeClearKindDepthStencil) {
            if (observedNamedClearTarget != GL_DEPTH_STENCIL || observedNamedClearDepth != clear.DepthValue ||
                observedNamedClearStencil != clear.StencilValue) ::_exit(105);
        } else if (observedNamedClearTarget != GL_COLOR ||
                   std::memcmp(observedNamedClearValues, clear.ColorValue, sizeof(bits))) ::_exit(106);
        MGPipeSetResourceOps(nullptr);
        sink.SetBackend(nullptr);
    }
}

TEST(RemoteF1, UnboundNamedfvRefusesByName) {
    // Historical name retained; a named target now reaches the native clear hook.
    const auto child = RunInChild([] { CheckNamedClearScopedTarget(kMGPipeClearKindColor, kMGPipeClearValueClassFloat); });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, UnboundNamedivRefusesByName) {
    const auto child = RunInChild([] { CheckNamedClearScopedTarget(kMGPipeClearKindColor, kMGPipeClearValueClassInt); });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, UnboundNameduivRefusesByName) {
    const auto child = RunInChild([] { CheckNamedClearScopedTarget(kMGPipeClearKindColor, kMGPipeClearValueClassUint); });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, UnboundNamedfiRefusesByName) {
    const auto child = RunInChild([] { CheckNamedClearScopedTarget(kMGPipeClearKindDepthStencil, kMGPipeClearValueClassFloat); });
    ExpectChildSuccess(child);
}
#endif

// =====================================================================================
// P5c (hd, CONTRACT-P5C §3): the record's handles are the server's resolution. The sink
// publishes each verb's own handles into the applier's verb stash (§3.2), the named blit's
// client emitter no longer rebinds anything (§3.3), and the layer-1 guards refuse the
// client-only surfaces from the apply thread by name (§3.1, §3.7, §3.8).
// =====================================================================================
#if MGTEST_HAVE_FORK
#include <MG_Backend/DirectGLES/SlotTables.h>
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_State/GLState/BufferState/BufferObject.h>

TEST(RemoteF1, BlitNamedCarriesHandlesAndLeavesTheClientBindingsAlone) {
    // Red once (executed, reverted): re-bind the named arguments in the client shadow before
    // emitting (the deleted ScopedBlitBindings); the two binding-slot asserts fail.
    const auto child = RunInChild([] {
        StartControlSession();
        F1Peer peer;
        peer.Install();
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        auto boundRead = MakeShared<MG_State::GLState::FramebufferObject>(11);
        auto boundDraw = MakeShared<MG_State::GLState::FramebufferObject>(12);
        auto namedRead = MakeShared<MG_State::GLState::FramebufferObject>(13);
        auto namedDraw = MakeShared<MG_State::GLState::FramebufferObject>(14);
        MG_State::pGLContext->GetFramebufferBindingSlot(MobileGL::FramebufferTarget::Read).Bind(boundRead);
        MG_State::pGLContext->GetFramebufferBindingSlot(MobileGL::FramebufferTarget::Draw).Bind(boundDraw);

        RemoteEmitTable().GL.BlitNamedFramebuffer(namedRead, namedDraw, 0, 0, 8, 8, 0, 0, 8, 8,
                                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
        const auto& r = peer.blit;
        if (peer.calls != 1 || r.ReadFbo != MGPipeFramebufferEmitter::HandleFor(*namedRead) ||
            r.DrawFbo != MGPipeFramebufferEmitter::HandleFor(*namedDraw)) ::_exit(101);
        // The client's own bindings are untouched: the server resolves the named pair from the
        // record, not from a staged binding override (T3).
        if (MG_State::pGLContext->GetFramebufferBindingSlot(MobileGL::FramebufferTarget::Read)
                .GetBoundObject()
                .get() != boundRead.get()) ::_exit(102);
        if (MG_State::pGLContext->GetFramebufferBindingSlot(MobileGL::FramebufferTarget::Draw)
                .GetBoundObject()
                .get() != boundDraw.get()) ::_exit(103);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

namespace {
    Bool g_stubBlitCalled = false;
    MGPipeHandle g_observedBlitRead = kMGPipeNullHandle;
    MGPipeHandle g_observedBlitDraw = kMGPipeNullHandle;
    void StubBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) {
        g_stubBlitCalled = true;
        const auto& st = MG_Pipe::MGPipeApplier();
        g_observedBlitRead = st.VerbBlitReadFbo;
        g_observedBlitDraw = st.VerbBlitDrawFbo;
    }
    void StubBlitFramebufferConsuming(GLint x0, GLint y0, GLint x1, GLint y1, GLint dx0, GLint dy0,
                                      GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
        StubBlitFramebuffer(x0, y0, x1, y1, dx0, dy0, dx1, dy1, mask, filter);
        MG_Pipe::MGPipeApplier().VerbBlitNamedConsumed = true;
    }

    MGPipeHandle g_observedCopyDst = kMGPipeNullHandle;
    void StubCopyTexImage2D(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) {
        g_observedCopyDst = MG_Pipe::MGPipeApplier().VerbCopyTexDst;
    }
    MGPipeHandle g_observedMipRes = kMGPipeNullHandle;
    void StubGenerateMipmap(GLenum) { g_observedMipRes = MG_Pipe::MGPipeApplier().VerbMipRes; }
    MGPipeHandle g_observedIndirect = kMGPipeNullHandle;
    MGPipeHandle g_observedParameter = kMGPipeNullHandle;
    void StubMultiDrawArraysIndirectCount(GLenum, const void*, GLintptr, GLsizei, GLsizei) {
        const auto& st = MG_Pipe::MGPipeApplier();
        g_observedIndirect = st.VerbIndirectBuffer;
        g_observedParameter = st.VerbIndirectParameterBuffer;
    }
    MGPipeHandle g_observedDispatchIndirect = kMGPipeNullHandle;
    void StubDispatchComputeIndirect(GLintptr) {
        g_observedDispatchIndirect = MG_Pipe::MGPipeApplier().VerbDispatchIndirectBuffer;
    }
} // namespace

TEST(RemoteF1, NamedBlitStashesHandlesAndDeclinesWhenTheBackendHasNoNamedArm) {
    // Red once (executed, reverted): do not write the verb stash; the observed handles are null.
    const auto child = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        CapsPeer backend;
        backend.table.GL.BlitFramebuffer = &StubBlitFramebuffer;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPBlit r{};
        r.ReadFbo = {41, 2};
        r.DrawFbo = {42, 3};
        r.Mask = GL_COLOR_BUFFER_BIT;
        r.Filter = GL_NEAREST;
        const Bool applied = sink.OnBlit(r);
        if (applied) ::_exit(101); // a named pair the backend did not consume must decline
        if (!g_stubBlitCalled) ::_exit(102);
        // The backend saw the record's handles as the verb's own state (CONTRACT-P5C §3.2/§3.3).
        if (g_observedBlitRead != r.ReadFbo || g_observedBlitDraw != r.DrawFbo) ::_exit(103);
        ::_exit(0);
    });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, NamedBlitIsAppliedWhenTheBackendConsumesThePair) {
    const auto child = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        CapsPeer backend;
        backend.table.GL.BlitFramebuffer = &StubBlitFramebufferConsuming;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPBlit r{};
        r.ReadFbo = {41, 2};
        r.DrawFbo = {42, 3};
        r.Mask = GL_COLOR_BUFFER_BIT;
        r.Filter = GL_NEAREST;
        if (!sink.OnBlit(r)) ::_exit(101);
        if (g_observedBlitRead != r.ReadFbo || g_observedBlitDraw != r.DrawFbo) ::_exit(102);
        ::_exit(0);
    });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, BoundBlitCarriesNoHandlesAndIsAppliedAsBefore) {
    const auto child = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        CapsPeer backend;
        backend.table.GL.BlitFramebuffer = &StubBlitFramebuffer;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPBlit r{};
        r.Mask = GL_COLOR_BUFFER_BIT;
        r.Filter = GL_NEAREST;
        if (!sink.OnBlit(r)) ::_exit(101);
        if (!MGPipeHandleIsNull(g_observedBlitRead) || !MGPipeHandleIsNull(g_observedBlitDraw))
            ::_exit(102);
        ::_exit(0);
    });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, CopyFramebufferToTextureStashesTheDestinationHandle) {
    // Red once (executed, reverted): do not write VerbCopyTexDst; the observed handle is null.
    const auto child = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        CapsPeer backend;
        backend.table.GL.CopyTexImage2D = &StubCopyTexImage2D;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPCopyFromFramebuffer r{};
        r.Dst = {51, 4};
        r.Target = GL_TEXTURE_2D;
        r.Level = 1;
        r.InternalFormat = GL_RGBA8;
        r.Width = 8;
        r.Height = 8;
        if (!sink.OnCopyFramebufferToTexture(r)) ::_exit(101);
        if (g_observedCopyDst != r.Dst) ::_exit(102);
        ::_exit(0);
    });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, GenerateMipmapStashesTheTextureHandle) {
    // Red once (executed, reverted): do not write VerbMipRes; the observed handle is null.
    const auto child = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        CapsPeer backend;
        backend.table.GL.GenerateMipmap = &StubGenerateMipmap;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPMipPlan r{};
        r.Res = {52, 5};
        r.Target = GL_TEXTURE_2D;
        if (!sink.OnGenerateMipmap(r)) ::_exit(101);
        if (g_observedMipRes != r.Res) ::_exit(102);
        ::_exit(0);
    });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, AnIndirectDrawStashesTheCommandAndParameterBuffers) {
    // Red once (executed, reverted): do not write the indirect stash; the observed handles are null.
    const auto child = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        CapsPeer backend;
        backend.table.GL.MultiDrawArraysIndirectCount = &StubMultiDrawArraysIndirectCount;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPDrawInfo info{};
        info.Mode = GL_TRIANGLES;
        info.IndexSize = 0;
        info.NumDraws = 0;
        info.InstanceCount = 1;
        MGPDrawIndirect indirect{};
        indirect.Buffer = {61, 1};
        indirect.ParameterBuffer = {62, 1};
        indirect.Offset = 64;
        indirect.DrawCount = 3;
        indirect.Stride = 16;
        if (!sink.OnDrawVbo(info, nullptr, nullptr, &indirect)) ::_exit(101);
        if (g_observedIndirect != indirect.Buffer || g_observedParameter != indirect.ParameterBuffer)
            ::_exit(102);
        ::_exit(0);
    });
    ExpectChildSuccess(child);
}

TEST(RemoteF1, DispatchIndirectStashesTheCommandBuffer) {
    // Red once (executed, reverted): do not write VerbDispatchIndirectBuffer; the observed
    // handle is null.
    const auto child = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        CapsPeer backend;
        backend.table.GL.DispatchComputeIndirect = &StubDispatchComputeIndirect;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPGridInfo grid{};
        grid.IsIndirect = 1;
        grid.IndirectBuffer = {63, 1};
        grid.IndirectOffset = 128;
        if (!sink.OnLaunchGrid(grid)) ::_exit(101);
        if (g_observedDispatchIndirect != grid.IndirectBuffer) ::_exit(102);
        ::_exit(0);
    });
    ExpectChildSuccess(child);
}

// ---- the layer-1 guards (CONTRACT-P5C §6), each verified red once by its own Fatal ---------

TEST(RemoteGuards, AllocatorAcquireFromTheApplyThreadIsFatalByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            MGPipeSlots().Acquire(MGPipeKind::Texture, 424242);
            return MOBILEGL_OK;
        }, nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
}

TEST(RemoteGuards, AllocatorFindByLifetimeIdFromTheApplyThreadIsFatalByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            MGPipeSlots().FindByLifetimeId(MGPipeKind::Texture, 424242);
            return MOBILEGL_OK;
        }, nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
}

TEST(RemoteGuards, AllocatorFreeFromTheApplyThreadIsFatalByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            MGPipeSlots().Free(MGPipeKind::Texture, {7, 1});
            return MOBILEGL_OK;
        }, nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
}

namespace {
    struct FakeTwin {
        int marker = 0;
    };
    using TestTextureTable =
        MG_Backend::DirectGLES::BackendSlotTable<MG_State::GLState::TextureObject2D, FakeTwin,
                                                 MGPipeKind::Texture>;
} // namespace

TEST(RemoteGuards, SlotTableHandleOfFromTheApplyThreadIsFatalByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
            TestTextureTable table;
            MG_State::GLState::TextureObject2D tex(44);
            table.HandleOf(&tex);
            return MOBILEGL_OK;
        }, nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
}

TEST(RemoteGuards, SlotTableMintingGetOrCreateFromTheApplyThreadIsFatalByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
            TestTextureTable table;
            auto tex = MakeShared<MG_State::GLState::TextureObject2D>(45);
            table.GetOrCreate(tex);
            return MOBILEGL_OK;
        }, nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
}

// The four accessor drives share one shape: the BufferObject is constructed on the CLIENT
// thread (its own resource_create publication is a legal client-side emit), and only the
// accessor runs on the apply thread, where it must die at the accessor's own guard.
#define MGL_BUFFER_GUARD_TEST(Name, Id, Call)                                                          TEST(RemoteGuards, Name) {                                                                                 const auto child = RunInChild([] {                                                                         StartControlSession();                                                                                 MG_State::GLState::BufferObject buffer(Id);                                                            Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {                                                auto& buffer = *static_cast<MG_State::GLState::BufferObject*>(self);                                   Call;                                                                                                  return MOBILEGL_OK;                                                                                }, &buffer);                                                                                           ClientSessionInstance().Stop();                                                                    });                                                                                                    ExpectNamedAbort(child, "Fatal{RoleViolation, \"buffer-legacy-arm\"}");                           }
MGL_BUFFER_GUARD_TEST(BufferMappedDataFromTheApplyThreadIsFatalByName, 703, (void)buffer.MappedData())
MGL_BUFFER_GUARD_TEST(BufferIsMappedFromTheApplyThreadIsFatalByName, 704, (void)buffer.IsMapped())
MGL_BUFFER_GUARD_TEST(BufferChangeSerialFromTheApplyThreadIsFatalByName, 705, (void)buffer.GetChangeSerial())
MGL_BUFFER_GUARD_TEST(BufferSyncPersistentMappedRangeFromTheApplyThreadIsFatalByName, 706,
                      buffer.SyncPersistentMappedRange())
MGL_BUFFER_GUARD_TEST(BufferHasDefinedContentFromTheApplyThreadIsFatalByName, 707, (void)buffer.HasDefinedContent())
#undef MGL_BUFFER_GUARD_TEST

// Historical P5d/P5e probes remain named for compatibility. P5f retires both
// scope exemptions, including barriered records; all allocator access on apply is
// client-memory access regardless of whether that memory happens to be stable.
TEST(RemoteGuards, AnAllocatorProbeFromTheApplyThreadOutsideEveryScopeIsFatalByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                MG_Pipe::MGPipeRefuseAllocatorFromApplyThread("FindByLifetimeId");
                return MOBILEGL_OK;
            },
            nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
}

// Construct on the client and run only HandleOf on apply. The false barrier stamp
// keeps the original P5e scenario; the tests below add the old barriered loophole.
TEST(RemoteGuards, AnAllocatorProbeInsideAnExemptionScopeFromTheApplyThreadIsFatalEvenAtTheOldDebtSites) {
    struct Probe {
        TestTextureTable* table;
        MG_State::GLState::TextureObject2D* texture;
    };
    const auto child = RunInChild([] {
        StartControlSession();
        MG_State::GLState::TextureObject2D texture(46);
        TestTextureTable table;
        Probe probe{&table, &texture};
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void* self) -> MobileGLResult {
                auto& probe = *static_cast<Probe*>(self);
                // The record being applied is one the client did NOT park behind. The stamp is
                // the thread_local the sink writes (ID-103), so setting it here puts this
                // thread in exactly the state an unbarriered apply is in.
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(false);
                const MG_Pipe::MGPipeFrontendKeyedRegistryScope frontendKeyedRegistry;
                probe.table->HandleOf(probe.texture);
                return MOBILEGL_OK;
            },
            &probe);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
}

// P5f (fr): barriers and old named scopes no longer admit frontend identity.
// Construct the object and table on the client so each death comes from the exact
// probed member, not a constructor's resource_create or a missing record.
TEST(RemoteGuards, BarrieredLegacyScopesCannotExemptAllocator) {
    for (const auto backend : {BackendType::DirectGLES, BackendType::DirectVulkan}) {
        const auto child = RunInChild([backend] {
            StartControlSession();
            auto selectedBackend = backend;
            Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(+[](void* self) -> MobileGLResult {
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(true);
                MG_Config::ActiveBackendType = *static_cast<BackendType*>(self);
                const MG_Pipe::MGPipeFrontendKeyedRegistryScope registryScope;
                const MG_Pipe::MagmaP7AllocatorDebtScope magmaScope;
                // HighWater had no per-method guard in the old implementation.
                (void)MGPipeSlots().HighWater(MGPipeKind::Texture);
                return MOBILEGL_OK;
            }, &selectedBackend);
            ClientSessionInstance().Stop();
        });
        ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
    }
}

TEST(RemoteGuards, BarrieredFrontendRegistryMembersRefuseBothLegacyScopes) {
    struct Probe {
        TestTextureTable* table;
        SharedPtr<MG_State::GLState::TextureObject2D>* texture;
        MGPipeHandle handle;
        int operation;
    };
    const char* members[] = {"HandleOf", "GetOrCreate(StatePtr)", "NoteStateForHandle", "StateForHandle"};
    for (int operation = 0; operation != 4; ++operation) {
        SCOPED_TRACE(members[operation]);
        const auto child = RunInChild([operation] {
            StartControlSession();
            auto texture = MakeShared<MG_State::GLState::TextureObject2D>(47);
            TestTextureTable table;
            table.GetOrCreate(texture) = MakeShared<FakeTwin>();
            Probe probe{&table, &texture, table.HandleOf(texture.get()), operation};
            Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(+[](void* self) -> MobileGLResult {
                auto& probe = *static_cast<Probe*>(self);
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(true);
                MG_Config::ActiveBackendType = BackendType::DirectVulkan;
                const MG_Pipe::MGPipeFrontendKeyedRegistryScope registryScope;
                const MG_Pipe::MagmaP7AllocatorDebtScope magmaScope;
                switch (probe.operation) {
                case 0: probe.table->HandleOf(probe.texture->get()); break;
                case 1: probe.table->GetOrCreate(*probe.texture); break;
                case 2: probe.table->NoteStateForHandle(probe.handle, *probe.texture); break;
                case 3: probe.table->StateForHandle(probe.handle); break;
                }
                return MOBILEGL_OK;
            }, &probe);
            ClientSessionInstance().Stop();
        });
        ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
        EXPECT_NE(child.Log.find(std::string{"BackendSlotTable::"} + members[operation]), std::string::npos);
    }
}

TEST(RemoteGuards, LegacyRegistryWrapperCannotExposeFrontendKeysOnApply) {
    using Registry = MG_Backend::DirectGLES::StateBackendObjectRegistry<
        MG_State::GLState::TextureObject2D, FakeTwin, MGPipeKind::Texture>;
    struct Probe { Registry* registry; SharedPtr<MG_State::GLState::TextureObject2D>* texture; int operation; };
    for (int operation = 0; operation != 3; ++operation) {
        const auto child = RunInChild([operation] {
            StartControlSession();
            auto texture = MakeShared<MG_State::GLState::TextureObject2D>(48);
            Registry registry;
            Probe probe{&registry, &texture, operation};
            Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(+[](void* self) -> MobileGLResult {
                auto& probe = *static_cast<Probe*>(self);
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(true);
                const MG_Pipe::MGPipeFrontendKeyedRegistryScope scope;
                switch (probe.operation) {
                case 0: probe.registry->Find(probe.texture->get()); break;
                case 1: probe.registry->GetOrCreate(*probe.texture); break;
                case 2: (void)probe.registry->begin(); break;
                }
                return MOBILEGL_OK;
            }, &probe);
            ClientSessionInstance().Stop();
        });
        ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
        EXPECT_NE(child.Log.find("BackendSlotTable::Registry."), std::string::npos);
    }
}

TEST(RemoteGuards, HandleRegistryMembersWorkOnBarrieredAndUnbarrieredApply) {
    const auto child = RunInChild([] {
        StartControlSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(+[](void*) -> MobileGLResult {
            TestTextureTable table;
            for (const bool barriered : {true, false}) {
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(barriered);
                const MGPipeHandle handle{71, barriered ? 1u : 2u};
                auto& twin = table.GetOrCreate(handle);
                twin = MakeShared<FakeTwin>();
                twin->marker = 19;
                auto* found = table.FindByHandle(handle);
                if (!found || !*found || (*found)->marker != 19) _exit(81);
                unsigned live = 0;
                table.ForEachLive([&](MGPipeHandle actual, const auto& backend) {
                    if (actual.Slot != handle.Slot || actual.Gen != handle.Gen || backend->marker != 19) _exit(82);
                    ++live;
                });
                if (live != 1 || !table.ReleaseByHandle(handle) || table.FindByHandle(handle)) _exit(83);
            }
            return MOBILEGL_OK;
        }, nullptr);
        ClientSessionInstance().Stop();
    });
    ASSERT_TRUE(WIFEXITED(child.Status)) << child.Log;
    EXPECT_EQ(WEXITSTATUS(child.Status), 0) << child.Log;
}

// A marker held by the GL thread never affects the apply guard either.
TEST(RemoteGuards, AnExemptionScopeHeldOnTheGLThreadDoesNotExemptTheApplyThread) {
    const auto child = RunInChild([] {
        StartControlSession();
        const MG_Pipe::MGPipeFrontendKeyedRegistryScope frontendKeyedRegistry;
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                MG_Pipe::MGPipeRefuseAllocatorFromApplyThread("FindByLifetimeId");
                return MOBILEGL_OK;
            },
            nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
}

TEST(RemoteGuards, CapsMirrorFallbackWithNoServerBackendIsFatalByName) {
    const auto child = RunInChild([] {
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        // No server backend exists in this process: the format-caps fallback to the client
        // mirror is refused by name (CONTRACT-P5C §3.7).
        (void)MG_Backend::DirectGLES::ActiveBackendFormatCaps();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"caps-mirror\"}");
}

// P5c (gt, CONTRACT-P5C §6 layer 1): the TEXTURE family's object-surface list, in executable
// form - this block IS the pinned list §5.4(b) asks for (the buffer family's five are the
// MGL_BUFFER_GUARD_TEST block above). Every drive shares the buffer family's shape: the texture
// is constructed on the CLIENT thread (its resource_create mint and emission are legal
// client-side), and only the probed method runs on the apply thread, where the guard fires
// BEFORE any level precondition could - the method names below are exactly the ones the guard
// (MipmapStorage.cpp's RefuseLegacyTextureArmFromApplyThread) is hung on:
//
//   AllocateStorage, TruncateMipmapLevels, UpdateMipmapSubData, MapMipmapData,
//   MarkStorageDirty, MarkStorageDirtyRegion, IsStorageDirty, GetStorageDirtyRegion,
//   GetStorageDirtyRects
//
// P5e (tx2): THE SHAPE READS JOIN THE LIST, and the sentence that stood here is why they could
// not before - "the per-draw binding walk reads them every draw". P5e is the commit that makes
// that untrue: the unit work list is st.BoundSamplerViews[], the clean gate is
// IsDrawSyncCleanByRecord, and the three sync bodies read the resource record and the server's
// staged-texture store. Nothing on the draw path asks a frontend texture for a level count, a
// level extent, a level byte size or a compressed level any more, so the exemption becomes a
// guard and reverting any handle arm aborts BY ACCESSOR NAME:
//
//   GetMipmapLevelCount, GetMipmapTexelSize, GetMipmapByteSize,
//   GetCompressedFormat, GetCompressedByteSize, MapCompressedMipmapData,
//   GetRequestedCompressedFormat
//
// Keyed on the SERVER BACKEND being DirectGLES (ruling 12's shape): Magma is lockstep through
// P7 and its named blit still asks a frontend texture IsComplete(), which is legal for a client
// parked in its own wait. Still NOT in the list: GetUploadTargets / GetTarget / IsComplete,
// which are TextureObject's own and not MipmapStorage's - their rows retire with the object
// class in P7/P9.
#define MGL_TEXTURE_GUARD_TEST(Name, Id, Call)                                                         \
    TEST(RemoteGuards, Name) {                                                                         \
        const auto child = RunInChild([] {                                                             \
            StartControlSession();                                                                     \
            MG_State::GLState::TextureObject2D texture(Id);                                            \
            Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(                                                \
                [](void* self) {                                                                       \
                    auto& texture = *static_cast<MG_State::GLState::TextureObject2D*>(self);           \
                    Call;                                                                              \
                    return MOBILEGL_OK;                                                                \
                },                                                                                     \
                &texture);                                                                             \
            ClientSessionInstance().Stop();                                                            \
        });                                                                                            \
        ExpectNamedAbort(child, "Fatal{RoleViolation, \"texture-legacy-arm\"}");                       \
    }
MGL_TEXTURE_GUARD_TEST(TextureAllocateStorageFromTheApplyThreadIsFatalByName, 711,
                       texture.AllocateStorage(TextureUploadTarget::Texture2D, 0,
                                               {{4, 4, 1}, 64}))
MGL_TEXTURE_GUARD_TEST(TextureTruncateMipmapLevelsFromTheApplyThreadIsFatalByName, 712,
                       texture.TruncateMipmapLevels(TextureUploadTarget::Texture2D, 1))
MGL_TEXTURE_GUARD_TEST(TextureUpdateMipmapSubDataFromTheApplyThreadIsFatalByName, 713,
                       texture.UpdateMipmapSubData(TextureUploadTarget::Texture2D, 0, {}))
MGL_TEXTURE_GUARD_TEST(TextureMapMipmapDataFromTheApplyThreadIsFatalByName, 714,
                       (void)texture.MapMipmapData(TextureUploadTarget::Texture2D, 0))
MGL_TEXTURE_GUARD_TEST(TextureMarkStorageDirtyFromTheApplyThreadIsFatalByName, 715,
                       texture.MarkStorageDirty(TextureUploadTarget::Texture2D, 0, true))
MGL_TEXTURE_GUARD_TEST(TextureMarkStorageDirtyRegionFromTheApplyThreadIsFatalByName, 716,
                       texture.MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0,
                                                      {0, 0, 0}, {1, 1, 1}))
MGL_TEXTURE_GUARD_TEST(TextureIsStorageDirtyFromTheApplyThreadIsFatalByName, 717,
                       (void)texture.IsStorageDirty(TextureUploadTarget::Texture2D, 0))
MGL_TEXTURE_GUARD_TEST(TextureGetStorageDirtyRegionFromTheApplyThreadIsFatalByName, 718,
                       (void)texture.GetStorageDirtyRegion(TextureUploadTarget::Texture2D, 0))
MGL_TEXTURE_GUARD_TEST(TextureGetStorageDirtyRectsFromTheApplyThreadIsFatalByName, 719,
                       (void)texture.GetStorageDirtyRects(TextureUploadTarget::Texture2D, 0,
                                                          nullptr, 0))
// ---- P5e (tx2): the seven shape reads the P4a/P5c list exempted ----------------------------
MGL_TEXTURE_GUARD_TEST(TextureGetMipmapLevelCountFromTheApplyThreadIsFatalByName, 721,
                       (void)texture.GetMipmapLevelCount())
MGL_TEXTURE_GUARD_TEST(TextureGetMipmapTexelSizeFromTheApplyThreadIsFatalByName, 722,
                       (void)texture.GetMipmapTexelSize(TextureUploadTarget::Texture2D, 0))
MGL_TEXTURE_GUARD_TEST(TextureGetMipmapByteSizeFromTheApplyThreadIsFatalByName, 723,
                       (void)texture.GetMipmapByteSize(TextureUploadTarget::Texture2D, 0))
MGL_TEXTURE_GUARD_TEST(TextureGetCompressedFormatFromTheApplyThreadIsFatalByName, 724,
                       (void)texture.GetMipmapCompressedFormat(TextureUploadTarget::Texture2D, 0))
MGL_TEXTURE_GUARD_TEST(TextureGetCompressedByteSizeFromTheApplyThreadIsFatalByName, 725,
                       (void)texture.GetMipmapCompressedByteSize(TextureUploadTarget::Texture2D, 0))
MGL_TEXTURE_GUARD_TEST(TextureMapCompressedMipmapDataFromTheApplyThreadIsFatalByName, 726,
                       (void)texture.MapMipmapCompressedImage(TextureUploadTarget::Texture2D, 0))
MGL_TEXTURE_GUARD_TEST(TextureGetRequestedCompressedFormatFromTheApplyThreadIsFatalByName, 727,
                       (void)texture.GetMipmapRequestedCompressedFormat(TextureUploadTarget::Texture2D, 0))
#undef MGL_TEXTURE_GUARD_TEST

// P5c (gt, CONTRACT-P5C §6 layer 2 / audit A1): the client-side half of the gPipeInputs
// single-writer rule. The apply thread is NOT actually inside the applier in this case - the
// flag is raised by hand, which is the exact overlap window the check exists to refuse: a fill
// that ran while a real apply was in flight would race the applier's reads of gPipeInputs.
//
// MOBILEGL_IPC_BATCH_WAITS (default 1) makes that overlap the INTENDED shape (the fill
// writes only fields no record supplies; pulled reads stay fenced by the pull-verbs' own
// wait), so the guard fires only with the batch off. This case forces it off and keeps the
// Fatal; the case below it pins the batched arm as legal.
TEST(RemoteGuards, ClientPipeInputsFillWhileTheApplierOwnsItIsFatalByName) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.BatchWaits = 0;
        StartControlSession();
        ClientSession::NoteApplyThreadEnteredApplier();
        MGPipeValidateForVerb(MGPipeVerb::Clear);
        ClientSession::NoteApplyThreadLeftApplier();
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"gPipeInputs\"}");
}

// The batched arm: with MOBILEGL_IPC_BATCH_WAITS=1 the fill is expected to run while the
// apply thread is inside the applier - that is the batch's whole point, and the disjoint-
// field model (record-supplied fields are never filled; pulled reads stay fenced) is what
// makes it safe.
TEST(RemoteGuards, ClientPipeInputsFillWhileTheApplierOwnsItIsAllowedWhenBatchingIsOn) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.BatchWaits = 1;
        StartControlSession();
        ClientSession::NoteApplyThreadEnteredApplier();
        MGPipeValidateForVerb(MGPipeVerb::Clear);
        ClientSession::NoteApplyThreadLeftApplier();
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// The same fill with the flag down is the legal shape - this is the control that keeps the
// check from being "abort unconditionally".
TEST(RemoteGuards, ClientPipeInputsFillWithTheApplierIdleIsAllowed) {
    const auto child = RunInChild([] {
        StartControlSession();
        MGPipeValidateForVerb(MGPipeVerb::Clear);
        MGPipeLeaveVerb();
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// ===========================================================================================
// P5e (ra) - the wait rule, the present credit and gPipeInputs' ownership
// (MG_Remote/CONTRACT-P5E.md §1, §2.3, §2.4, §3.1, §3.3, §3.5)
// ===========================================================================================
//
// EVERY CASE BELOW ARMS RUN-AHEAD BY PUBLISHING THE CAP BIT, because that is the only thing
// that arms it: RunAheadArmed() is a conjunction whose third term is the SERVER's own
// statement that it applies an unbarriered record without reading client memory, and in a
// tree where kMGPipeP5eRunAheadReady is still false no production session sets it. Driving the
// arm through the caps mirror rather than through a test-only setter is deliberate (ID-102's
// lesson): what these cases exercise is then the production latch and not a branch beside it.
//
// NONE OF THEM DRAWS. The draw path is the family packages' to migrate; until they land, a
// draw under run-ahead would abort inside the backend for a reason that has nothing to do with
// what is being asserted here.
void StartRunAheadSession() {
    ::alarm(15);
    MG_Config::Transport = MG_Config::TransportMode::InProcess;
    MG_Config::Ipc.RunAhead = 1;
    Srv::ServerSessionInstance().SetCapabilityBits(
        static_cast<Uint64>(MG_Pipe::kCapRunAheadApply));
    Srv::ServerSessionInstance().SetConsumedSubsystems(kMGPipeSubsystemsMigratedAtP4a);
    // The test peer publishes the first snapshot through the real handshake.
    Srv::ServerSessionInstance().SetBackend(ControlCapsPeer());
    if (ClientSessionInstance().Start(MG_Config::TransportMode::InProcess, {}) != MOBILEGL_OK) {
        ::_exit(81);
    }
    if (!ClientSessionInstance().RunAheadArmed()) ::_exit(83); // the latch never took
}

// THE SERVER HALF OF §2.4, SUBSTITUTED - and it is substituted because this fixture has no
// backend object at all, so ServerVerbSink::OnPresent DECLINES before it can reach
// ServerSession::ReturnPresentCredit ("present arrived with no backend object"). What the
// cases below assert is therefore the CLIENT's half of the credit: that it pays, when, and in
// whose id space. That the server returns exactly one credit per swap, after Present() has
// returned, is PipeApplier::OnPresent's own three lines and the device exit's `credit-waits`
// reading; a peer here cannot pin it without a backend to swap on.
struct PresentCreditPeer : Codec::WireVerbSink {
    std::atomic<Uint64> lastSerial{0};
    Bool OnPresent(const MGPPresent& present) override {
        lastSerial.store(present.FrameSerial);
        Srv::ServerSessionInstance().ReturnPresentCredit(present.FrameSerial);
        return true;
    }
    void Install() {
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
                [](void* self) {
                    auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
                    decoder.SetVerbSink(static_cast<PresentCreditPeer*>(self));
                    return MOBILEGL_OK;
                },
                this) != MOBILEGL_OK) {
            ::_exit(82);
        }
    }
};

// §1: the conjunction, and the half of it that is not the client's to decide. The cap bit is
// absent here, so the knob alone changes nothing - which is the whole reason the knob is
// parsed on every arm rather than only where it means something (Config.h).
TEST(RemoteRunAhead, TheKnobAloneDoesNotArmRunAheadWithoutTheServersCapBit) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.RunAhead = 1;
        StartControlSession(); // SetCapabilityBits(0)
        if (ClientSessionInstance().RunAheadArmed()) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// §1 again, from the other side: MOBILEGL_IPC_VERB_BARRIER=0 is the LOCKSTEP arm's negative
// control and it disarms run-ahead outright, because the barrier is the first term of the
// conjunction. Under run-ahead its documented red is the first barriered row's stale pull -
// that one is a scenario, not a unit case; what is pinned here is that the two controls do not
// silently compose into a third mode nobody designed.
TEST(RemoteRunAhead, ClearingTheVerbBarrierDisarmsRunAheadEvenWithTheCapBit) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.RunAhead = 1;
        MG_Config::Ipc.VerbBarrier = 0;
        ::alarm(15);
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        Srv::ServerSessionInstance().SetCapabilityBits(
            static_cast<Uint64>(MG_Pipe::kCapRunAheadApply));
        Srv::ServerSessionInstance().SetConsumedSubsystems(kMGPipeSubsystemsMigratedAtP4a);
        Srv::ServerSessionInstance().SetBackend(ControlCapsPeer());
        if (ClientSessionInstance().Start(MG_Config::TransportMode::InProcess, {}) != MOBILEGL_OK) {
            ::_exit(81);
        }
        if (ClientSessionInstance().RunAheadArmed()) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// §2.4's RED-ONCE, BY COUNT. With a credit of 1 the client may have one present in flight, so
// the second and third presents each pay the credit before they encode - `credit-waits` is 2
// after three presents and the present-ack watermark has reached 2. Delete the
// WaitForPresentAck arm from ClientSession::AcquirePresentCredit and the counter stays 0 while
// all three presents publish: red by count, which is the only way a wait that is usually
// already satisfied can be tested at all.
//
// The serial is asserted too, because the counter alone would survive a credit paid against
// the wrong id space (the pre-P5e FrameSerial was 0 and the server stamped its own).
TEST(RemoteRunAhead, ACreditOneClientPaysTheCreditAtEveryPresentAfterTheFirst) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.PresentCredit = 1;
        StartRunAheadSession();
        PresentCreditPeer peer;
        peer.Install();
        ClientSession& session = ClientSessionInstance();
        if (session.PresentCreditWaits() != 0) ::_exit(101);
        RemoteEmitTable().Present();
        if (session.PresentCreditWaits() != 0) ::_exit(102); // the first one is free
        RemoteEmitTable().Present();
        RemoteEmitTable().Present();
        if (session.PresentCreditWaits() != 2) ::_exit(103);
        // ONE CREDIT PER SWAP, in the client's own 1-based space: the server returned the
        // second present's credit, so the watermark is at least 2.
        if (session.Control() == nullptr ||
            session.Control()->Progress.presentAckSerial.load() < 2u) {
            ::_exit(104);
        }
        session.Stop();
    });
    ExpectChildSuccess(child);
}

// §2.4's other half: with the credit raised the client stops paying until it is that far
// ahead. It is the control that keeps the case above from passing because of an unconditional
// counter rather than because of the credit.
TEST(RemoteRunAhead, ACreditThreeClientPaysNothingForItsFirstThreePresents) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.PresentCredit = 3;
        StartRunAheadSession();
        PresentCreditPeer peer;
        peer.Install();
        ClientSession& session = ClientSessionInstance();
        RemoteEmitTable().Present();
        RemoteEmitTable().Present();
        RemoteEmitTable().Present();
        if (session.PresentCreditWaits() != 0) ::_exit(101);
        RemoteEmitTable().Present();
        if (session.PresentCreditWaits() != 1) ::_exit(102);
        session.Stop();
    });
    ExpectChildSuccess(child);
}

// §3.1 / §3.5's RED-ONCE. `Clear` is a kWaitNone row, so under run-ahead the client publishes
// it and moves on - and therefore does not fill gPipeInputs for it. This case is the GREEN
// half: the validate point runs the tracker walk and the emitters and touches the block not at
// all, so nothing aborts.
//
// THE RED: in MGPipeValidateForVerb change `const Bool fillOwed = barriered;` to `= true` -
// which is exactly "leave one CopyField in an unbarriered fill" - and the guard below it fires
// Fatal{RoleViolation, "gPipeInputs"} on this very call.
TEST(RemoteRunAhead, AnUnbarrieredVerbDoesNotFillPipeInputsAndIsAllowed) {
    const auto child = RunInChild([] {
        StartRunAheadSession();
        MGPipeValidateForVerb(MGPipeVerb::Clear);
        MGPipeLeaveVerb();
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// §3.5, the aborting arm, driven at the guard itself: a GL-thread touch that is NOT the
// residual fill of a barriered record. Under lockstep this same call is a no-op (the apply
// thread is not inside the applier and MOBILEGL_IPC_BATCH_WAITS is 1); under run-ahead there
// is no such window to be outside of, because the client never parks for an unbarriered
// record - so the answer stops depending on timing and becomes the rule.
TEST(RemoteGuards, ClientPipeInputsTouchOutsideABarrieredFillUnderRunAheadIsFatalByName) {
    const auto child = RunInChild([] {
        StartRunAheadSession();
        ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt("ra-test-surface",
                                                               /*isBarrieredFill=*/false);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"gPipeInputs\"}");
}

// ... and the control: the barriered fill says so and is let through. Without this the case
// above would pass just as well against "abort unconditionally".
//
// P5e (ra2): THIS CONTROL IS NOW ALSO HALF OF A PAIR, and the half it does NOT state is the
// one that cost the flip its lane. "The applier is not inside" is true here because nothing in
// this child ever entered it; the case below is the same call with that one fact reversed.
TEST(RemoteGuards, ClientPipeInputsTouchInsideABarrieredFillUnderRunAheadIsAllowed) {
    const auto child = RunInChild([] {
        StartRunAheadSession();
        ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt("ra-test-surface",
                                                               /*isBarrieredFill=*/true);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// ---- P5e (ra2): THE FLIP'S OWN RED-ONCE, AND THE CLASS THE STRICT LANE CANNOT SEE ----------
//
// The strict lane runs under LOCKSTEP - ApplyOne stamps every record barriered - so nothing
// that happens only once the client stops waiting is visible to it, by construction (ID-132).
// This pair is what measures it instead, and it runs in the ordinary unit lane on any head,
// flip thrown or not, because StartRunAheadSession arms run-ahead itself.
//
// WHAT IT PINS: `isBarrieredFill` is the caller's sentence "this touch is the residual fill of
// a record this thread is about to park behind". That is a claim about the FUTURE. The order at
// the validate point is fill, then emit, then park, so at the instant of the write the apply
// thread is still draining the UNBARRIERED records the client ran ahead of - and the exemption
// that took the claim on trust made this guard unfireable on precisely the class it exists for.
// Measured on the flipped head: the GL thread bumped CurrentVerbSerial, withdrew the server's
// stamp and renamed m_currentVerb underneath a record the applier was inside, and the applier
// aborted with `Fatal{UnmigratedPipeInput, "<field>@<the CLIENT's verb>"}` - a verb no applier
// stamp can produce, which is what identifies the writer.
//
// THE PROBE IS THE GUARD CALL ITSELF (ID-102): the flag is raised by the same raw entry point
// PipeApplier's ScopedApplierEntry uses, on the client thread, BEFORE the call under test - so
// what aborts is this probe and not some object built on the wrong thread.
//
// THE RED: in ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt put the run-ahead arm back
// to `if (isBarrieredFill) return;` and this case stops aborting, while the control above keeps
// passing - which is the difference between a rule and an exemption.
TEST(RemoteGuards, ClientBarrieredFillWhileTheApplierIsInsideUnderRunAheadIsFatalByName) {
    const auto child = RunInChild([] {
        StartRunAheadSession();
        ClientSession::NoteApplyThreadEnteredApplier();
        ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt("ra-test-surface",
                                                               /*isBarrieredFill=*/true);
        ClientSession::NoteApplyThreadLeftApplier();
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"gPipeInputs\"}");
}

// ---- P5e (ra2): AND THE OTHER HALF - AN INDEXED DRAW IS AN UNBARRIERED VERB ----------------
//
// MGP_VERB_OP_LIST joins the whole draw family through ONE row, `DrawVbo -> DrawArrays`, so
// inverting it verb-first answers kOpCount for DrawElements and for every other indexed /
// instanced / multi / indirect verb. The "unknown verb answers barriered" default then made the
// client FILL for each of them while never parking, because the wait is decided per record and
// the record is a draw_vbo (kWaitNone). On the flipped lane that was 21 of the 69 red entries.
//
// The case states it where it is decidable without a GL context: the guard runs before the
// validate point's null-context return, so a verb that answers UNBARRIERED never reaches the
// guard at all and the child exits 0 even with the applier flag up.
//
// THE RED: delete the kDraw fallback in ClientVerbIsBarriered (PipeFill.cpp) and DrawElements
// answers barriered again - the fill runs, the guard above it sees the raised flag, and this
// case aborts with Fatal{RoleViolation, "gPipeInputs"} instead of exiting 0.
TEST(RemoteRunAhead, AnIndexedDrawVerbIsUnbarrieredAndTouchesPipeInputsNotAtAll) {
    const auto child = RunInChild([] {
        StartRunAheadSession();
        ClientSession::NoteApplyThreadEnteredApplier();
        MGPipeValidateForVerb(MGPipeVerb::DrawElements);
        ClientSession::NoteApplyThreadLeftApplier();
        MGPipeLeaveVerb();
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// §3.3's RED-ONCE, and the one that turns the strict lane into a gate. The probe is the
// sticky forward itself, run on the apply thread inside a server-stamped verb with the
// current record marked UNBARRIERED - which is the state the sink puts that thread in for
// every kWaitNone row once the wait rule is live. MOBILEGL_IPC_STRICT_ERRORS is deliberately
// NOT set: the point is that the knob has stopped being the deciding input, because a row the
// client never filled has no value to count.
//
// THE RED: put CountBarrierPull's first line back to `if (MG_Config::Ipc.StrictErrors)` only
// and this case stops aborting - and, on the lane, every scenario that still pulls a row goes
// from a named abort to a wrong picture with a number beside it.
TEST(RemoteGuards, AResidualPullUnderAnUnbarrieredRecordIsFatalWithoutTheStrictKnob) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.StrictErrors = false;
        StartRunAheadSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                MG_Pipe::MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(false);
                MG_Pipe::MGPipeStickyForwardPull(MGPipeInputField::GetProgramObject);
                return MOBILEGL_OK;
            },
            nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{UnmigratedPipeInput, \"GetProgramObject@Clear\"}");
}

// P5f retires this formerly admitted read. A barrier can order a record but cannot
// turn a client ProgramObject into a server-owned value. The historical name stays
// registered for G14; both barrier states now have the same named refusal.
TEST(RemoteGuards, AResidualPullUnderABarrieredRecordStillOnlyCounts) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.StrictErrors = false;
        StartRunAheadSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                MG_Pipe::MGPipeServerStampVerbBoundary(MGPipeVerb::Clear);
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(true);
                const Uint64 before = MG_Pipe::MGPipeResidualPullCount();
                MG_Pipe::MGPipeStickyForwardPull(MGPipeInputField::GetProgramObject);
                if (MG_Pipe::MGPipeResidualPullCount() != before + 1) ::_exit(101);
                return MOBILEGL_OK;
            },
            nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{UnmigratedPipeInput, \"GetProgramObject@Clear\"}");
}

// §2.6's RED-ONCE, and the one the brief names: ~300 KiB of kEventGpuWritten published from
// the apply thread behind a sequence nobody waited for. SEG_EVENT is 256 KiB, so this is
// several ringfuls; under P5C's rule the FIRST Reserve that failed was Fatal{EventRingOverflow}
// and this case aborts by that name. With flow control the producer publishes, rings, parks on
// the latch and retries, and the burst completes.
//
// THE DRAIN RUNS ON A THREAD OF ITS OWN, and that is forced by the fixture rather than chosen:
// RunSurfaceControlFrame is synchronous, so the thread that posted the burst cannot also be the
// thread that empties the ring. In production it is the GL thread draining at its own waits
// (§2.6's deadlock argument); here it is a drainer beside the poster, which puts the producer
// in exactly the state that argument describes.
TEST(RemoteRunAhead, AnEventBurstBehindAnUnwaitedSequenceFlowControlsInsteadOfAborting) {
    const auto child = RunInChild([] {
        StartRunAheadSession();
        std::atomic<bool> stop{false};
        std::atomic<Uint64> delivered{0};
        std::thread drainer([&stop, &delivered] {
            // THE FIRST DRAIN IS DELAYED ON PURPOSE. A drainer that starts immediately can
            // keep a 256 KiB ring from ever being full, and a red-once that depends on the
            // scheduler losing a race is not a red-once. 50 ms is four orders of magnitude
            // more than the burst needs to fill the ring, so the producer is provably parked
            // on the latch before anything empties it.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            while (!stop.load(std::memory_order_acquire)) {
                delivered.fetch_add(ClientSessionInstance().DrainPublishedEvents());
                std::this_thread::yield();
            }
            delivered.fetch_add(ClientSessionInstance().DrainPublishedEvents());
        });
        // 3000 records of 64 ranges each is ~3 MB against a 256 KiB ring - twelve ringfuls,
        // with a drainer racing it - so the full latch is hit many times over rather than
        // maybe once. That margin is what makes the red-once (restore the Fatal) reliable:
        // a burst that merely might fill the ring would be a red-once that merely might be red.
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                MG_Pipe::MGPRange ranges[64];
                for (Uint32 r = 0; r < 64; ++r) ranges[r] = MG_Pipe::MGPRange{r * 64ull, 64ull};
                for (Uint32 i = 0; i < 3000; ++i) {
                    MG_Pipe::gMGPipeCallbacks.OnGpuWritten(MG_Pipe::MGPipeHandle{7, 1}, 64, ranges);
                }
                return MOBILEGL_OK;
            },
            nullptr);
        stop.store(true, std::memory_order_release);
        drainer.join();
        // LOSSLESS, which is the property flow control had to preserve: every one of the 3000
        // records crossed. A drop policy would have been the other way to survive a full ring,
        // and P5C's §4.4 refuses it - a writeback or a GPU-write mark that did not arrive is a
        // stale buffer, not a missing statistic.
        if (delivered.load() < 3000u) ::_exit(101);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// ===========================================================================================
// PH-6 (ID-P7-2): a client that does not drain SEG_EVENT FORFEITS the reverse channel
// ===========================================================================================
//
// The flow-control case above is the healthy half: a client that drains late gets every event.
// These two are the other half, and they are why ReserveEventOrBlock's two BUSY refusals became
// a latch. Each child reads its OWN outcome - the latch, the server's drop tally, the shared
// page's eventDropped the client sees, the time spent against MOBILEGL_IPC_EVENT_WAIT_MS, and the
// apply thread leaving by itself - and exits non-zero on the first that is wrong; the parent then
// asserts the named line and the ABSENCE of Fatal{EventRingOverflow}.
namespace {
    // Waits for mgl-srv-apply to leave on its own: the forfeit's stop is the ordinary one, so
    // nobody calls Stop() to get it out.
    bool ApplyThreadLeavesWithin(std::chrono::milliseconds budget) {
        const auto until = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < until) {
            if (!Srv::ServerLoopInstance().Running()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    Uint64 MillisecondsSince(std::chrono::steady_clock::time_point start) {
        return static_cast<Uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
    }
} // namespace

// STONEWALL: the client never drains. 3000 GPU-write marks of ~1 KiB against a 256 KiB ring:
// ~250 fit, the next one waits MOBILEGL_IPC_EVENT_WAIT_MS (300 ms here) and forfeits, and the
// remaining ~2750 are drops that cost no wait at all - so the whole burst returns in a little over
// the knob, not in 30 s and not with an abort.
//
// THE RED (recorded in the F2 package note): put ReserveEventOrBlock back to its two 30000 ms
// rounds and this child is killed by StartRunAheadSession's alarm(15) inside the first wait -
// before that change it would have aborted at 30 s with Fatal{EventRingOverflow} "waited 30000 ms".
TEST(RemoteRunAhead, AStonewallingClientForfeitsTheReverseChannelWithinTheWaitKnob) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.EventWaitMs = 300;
        StartRunAheadSession();
        const auto start = std::chrono::steady_clock::now();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                MG_Pipe::MGPRange ranges[64];
                for (Uint32 r = 0; r < 64; ++r) ranges[r] = MG_Pipe::MGPRange{r * 64ull, 64ull};
                for (Uint32 i = 0; i < 3000; ++i) {
                    MG_Pipe::gMGPipeCallbacks.OnGpuWritten(MG_Pipe::MGPipeHandle{7, 1}, 64, ranges);
                }
                return MOBILEGL_OK;
            },
            nullptr);
        const Uint64 elapsedMs = MillisecondsSince(start);
        auto& server = Srv::ServerSessionInstance();
        if (!server.ReverseChannelForfeited()) ::_exit(101);
        const Uint64 drops = server.ForfeitDrops();
        // Some fitted (the ring was empty) and some did not (it holds ~250 of 3000).
        if (drops == 0 || drops >= 3000u) ::_exit(102);
        // The shared page's counter is the one a client can read; the server's own tally is the
        // one a peer cannot write. They agree when nothing else counts.
        if (ClientSessionInstance().Events().DroppedEvents() != drops) ::_exit(103);
        // The knob, spent once: at least most of it, and nowhere near the old 30 s.
        if (elapsedMs < 250u || elapsedMs > 5000u) ::_exit(104);
        if (!ApplyThreadLeavesWithin(std::chrono::milliseconds(3000))) ::_exit(105);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_NE(child.Log.find("ReverseChannelForfeit{NotDraining} - kEventGpuWritten"), std::string::npos)
        << child.Log;
    EXPECT_NE(child.Log.find("mgl-srv-apply stops on ReverseChannelForfeit"), std::string::npos) << child.Log;
    EXPECT_EQ(child.Log.find("Fatal{EventRingOverflow"), std::string::npos) << child.Log;
}

// TRICKLE: the client drains, one record per 50 ms, and the record the server is waiting to place
// is twenty times bigger than what each drain frees. Every drain clears the latch and rings the
// server; every retry still does not fit. The old shape gave up after TWO such rounds and called
// it "a drained ring that still refuses a record it fits is a corrupt cursor set" - ~100 ms into a
// session whose only fault was a slow reader, and a Fatal. Now the rounds share one deadline:
// the server keeps retrying for the whole knob (600 ms here) and then forfeits by name, TooSlow,
// with the short drains counted.
//
// THE RED (recorded in the F2 package note): the old two-round ReserveEventOrBlock aborts this
// child with Fatal{EventRingOverflow} "could not reserve ... on an emptied SEG_EVENT".
TEST(RemoteRunAhead, ATricklingClientGetsTheWholeWaitThenForfeitsByNameInsteadOfAborting) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.EventWaitMs = 600;
        StartRunAheadSession();
        std::atomic<bool> stop{false};
        std::atomic<Uint32> popped{0};
        std::thread trickler([&stop, &popped] {
            Transport::EventRingConsumer& events = ClientSessionInstance().Events();
            while (!stop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                Transport::RingRecordView view;
                if (events.Pop(view)) popped.fetch_add(1);
                // Releases what was popped, clears the latch and rings the server: a real drain,
                // just a stingy one.
                events.Drained();
            }
        });
        const auto start = std::chrono::steady_clock::now();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                // Fill with ONE-range marks (48 bytes a record) until under 4 KiB is left, without
                // ever blocking - so the fill is not paced by the trickle - then ask for 64-range
                // marks (~1 KiB each). The first of those that does not fit is the one the server
                // waits for, and one popped 48-byte record never makes room for it.
                auto& ring = Srv::ServerSessionInstance().Events().Ring();
                const MG_Pipe::MGPRange one{0, 64};
                while (ring.FreeBytes() > 4096u) {
                    MG_Pipe::gMGPipeCallbacks.OnGpuWritten(MG_Pipe::MGPipeHandle{7, 1}, 1, &one);
                }
                MG_Pipe::MGPRange ranges[64];
                for (Uint32 r = 0; r < 64; ++r) ranges[r] = MG_Pipe::MGPRange{r * 64ull, 64ull};
                for (Uint32 i = 0; i < 16; ++i) {
                    MG_Pipe::gMGPipeCallbacks.OnGpuWritten(MG_Pipe::MGPipeHandle{7, 1}, 64, ranges);
                }
                return MOBILEGL_OK;
            },
            nullptr);
        const Uint64 elapsedMs = MillisecondsSince(start);
        stop.store(true, std::memory_order_release);
        trickler.join();
        auto& server = Srv::ServerSessionInstance();
        if (!server.ReverseChannelForfeited()) ::_exit(101);
        if (server.ForfeitDrops() == 0) ::_exit(102);
        // The trickle really happened while the server waited - otherwise this is the stonewall
        // case under another name.
        if (popped.load() < 2u) ::_exit(103);
        // The WHOLE budget was spent retrying, not two rounds of it.
        if (elapsedMs < 540u || elapsedMs > 5000u) ::_exit(104);
        if (!ApplyThreadLeavesWithin(std::chrono::milliseconds(3000))) ::_exit(105);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_NE(child.Log.find("ReverseChannelForfeit{TooSlow} - kEventGpuWritten"), std::string::npos)
        << child.Log;
    EXPECT_EQ(child.Log.find("Fatal{EventRingOverflow"), std::string::npos) << child.Log;
}

// STOP WHILE THE SERVER WAITS (PH-6 fix round). ServerLoop::Stop() arrives while the apply thread is
// parked inside the reservation, on a bell that is still ALIVE - the spawn session's shape when its
// control stream ends (EOF on a half-close, a malformed frame, a LogFlush ack) while the client is
// not draining. The knob is 20 s, four times Stop()'s 5000 ms bounded join, so this child can only
// exit cleanly if the wait ends on the stop request itself and forfeits as `Stopped`.
//
// THE RED: take ApplyStopRequested() out of ReserveEventOrBlock's predicate and the ring Stop()
// sends is swallowed by a wait that re-tests only "has the client drained?"; the reservation sits
// out its 20 s and Stop() aborts the child after 5 s with Fatal{ApplyThreadJoinTimeout}.
TEST(RemoteRunAhead, AStopWhileTheServerWaitsForADrainEndsTheWaitByNameInsteadOfTheJoinTimeout) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.EventWaitMs = 20000;
        StartRunAheadSession();
        // The producer's probe blocks inside the reservation, so it is posted from a thread of its
        // own: RunProbeOnApplyThreadForTesting waits for the probe to return.
        std::thread producer([] {
            (void)Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
                +[](void*) -> MobileGLResult {
                    MG_Pipe::MGPRange ranges[64];
                    for (Uint32 r = 0; r < 64; ++r) ranges[r] = MG_Pipe::MGPRange{r * 64ull, 64ull};
                    for (Uint32 i = 0; i < 3000; ++i) {
                        MG_Pipe::gMGPipeCallbacks.OnGpuWritten(MG_Pipe::MGPipeHandle{7, 1}, 64, ranges);
                    }
                    return MOBILEGL_OK;
                },
                nullptr);
        });
        // The reservation is waiting once SEG_EVENT's latch is up: nothing in this child drains.
        const auto signals = Srv::ServerSessionInstance().DataLink()->Signals();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (signals.EventRingFull->load(std::memory_order_acquire) == 0) {
            if (std::chrono::steady_clock::now() > until) ::_exit(106);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // well into the park
        const auto start = std::chrono::steady_clock::now();
        Srv::ServerLoopInstance().Stop();
        const Uint64 stopMs = MillisecondsSince(start);
        producer.join();
        auto& server = Srv::ServerSessionInstance();
        if (!server.ReverseChannelForfeited()) ::_exit(101);
        if (server.ForfeitDrops() == 0) ::_exit(102);
        // Stop() came back on the request, not on the join's 5000 ms or the knob's 20 s.
        if (stopMs > 2000u) ::_exit(104);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_NE(child.Log.find("ReverseChannelForfeit{Stopped} - kEventGpuWritten"), std::string::npos)
        << child.Log;
    EXPECT_EQ(child.Log.find("Fatal{ApplyThreadJoinTimeout"), std::string::npos) << child.Log;
    EXPECT_EQ(child.Log.find("Fatal{EventRingOverflow"), std::string::npos) << child.Log;
}

// §2.7 / ruling 13: the deferred-destroy queue stays, and an enqueue from an unbarriered
// apply is the finding. Under strict it is the abort; this is the arm the lane owns.
TEST(RemoteGuards, ADeferredDestroyFromAnUnbarrieredApplyIsFatalUnderStrict) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        StartRunAheadSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(false);
                (void)MG_Pipe::MGPipeDeferDestroyAndFreeIfOnApplyThread(MG_Pipe::MGPipeKind::Texture,
                                                                        4242);
                return MOBILEGL_OK;
            },
            nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"deferred-destroy\"}");
}

// The control, and the whole of ruling 13: a BARRIERED apply may still be a last owner (its
// fill's O-class rows, XFB's pinned targets), so the queue takes the death and the GL thread
// replays it. Deleting the queue would have made this an allocator touch from the server role.
TEST(RemoteGuards, ADeferredDestroyFromABarrieredApplyIsStillServed) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.StrictErrors = true;
        StartRunAheadSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
            +[](void*) -> MobileGLResult {
                MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(true);
                if (!MG_Pipe::MGPipeDeferDestroyAndFreeIfOnApplyThread(MG_Pipe::MGPipeKind::Texture,
                                                                        4243)) {
                    ::_exit(101);
                }
                return MOBILEGL_OK;
            },
            nullptr);
        MG_Pipe::MGPipeDrainDeferredDestroys();
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}
#endif

// §2.5 / ruling 15 (ID-93): which half of resource_subdata wants its answer. One predicate,
// read by MGPipeRouteResourceSubData and by Wire_ResourceSubData - a second spelling of it is
// how the client comes to wait for an answer the emitter told the server not to bother with.
// The buffer target's whole packed field is 0 (MGPipeTypes.h asserts it beside the packer),
// which is what makes the test a comparison and not a mask.
TEST(RemoteRunAhead, OnlyTheTextureHalfOfResourceSubDataWantsItsReply) {
    MG_Pipe::MGPSubData buffer{};
    buffer.Target = MG_Pipe::MGPipePackSubDataTarget(MG_Pipe::kMGPipeResourceTargetBuffer, 0u);
    EXPECT_FALSE(MG_Pipe::MGPipeSubDataWantsItsReply(buffer));

    MG_Pipe::MGPSubData texture{};
    texture.Target = MG_Pipe::MGPipePackSubDataTarget(
        static_cast<Uint32>(MG_Pipe::MGPipeResourceTarget::Tex2D),
        static_cast<Uint32>(TextureUploadTarget::Texture2D));
    EXPECT_TRUE(MG_Pipe::MGPipeSubDataWantsItsReply(texture));
}


#include "RemoteClientE1Controls.inc"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-remoteclient-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
#if MGTEST_HAVE_FORK
    // Explicit GPU control, kept out of the CPU-only unit label. No silent skip is allowed.
    if (argc == 2 && std::string(argv[1]).starts_with("--c1f-pack-gpu=")) {
        const bool split = std::string(argv[1]) == "--c1f-pack-gpu=inproc";
        const int rc = RunPackGpuControl(split);
        fs::remove(path, ec);
        return rc;
    }
#endif
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
