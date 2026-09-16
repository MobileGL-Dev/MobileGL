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
#include <MG_Remote/Client/BackendObject_Remote.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_State/GLState/Core.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_Pipe/PipeApply.h>
#include <Init.h>
#include <MG_Impl/EGLImpl/EGLImpl.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Drawing/GL_Drawing.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>

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

    std::string ReadLog() {
        std::ifstream in(g_logPath, std::ios::binary);
        if (!in) return {};
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
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
        { std::ofstream truncate(g_logPath, std::ios::trunc | std::ios::binary); }
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
    // CONTRACT-P5.md §7: 2 answered locally + 5 emitted + 64 Fatal at the P5b contract commit.
    // Read from the functions the table itself reports with - which is also what t1's arming
    // condition reads - rather than recomputed here, so a table that lost an emitter cannot look
    // like one that never had it.
    //
    // P5b package i1 (CONTRACT-P5B.md §2 i1) flipped SEVEN slots C -> B: BindImageTexture,
    // DispatchCompute, DispatchComputeIndirect, MemoryBarrier, MemoryBarrierByRegion,
    // CopyImageSubData, ShaderStorageBlockBinding. So the two numbers that move are 5 -> 12 and
    // 64 -> 57, and the SUM below is the invariant that does not move whichever package lands
    // next. RED ONCE BY DOING X: comment out `table.GL.MemoryBarrier = &EmitMemoryBarrier;` in
    // BuildRemoteEmitTable and this case stays green while NoSlotIsNull goes red - which is why
    // the per-package count is asserted here and the null walk is a separate case.
    EXPECT_EQ(LocallyAnsweredSlotCount(), 2u);
    EXPECT_EQ(ImplementedVerbCount(), 23u);
    EXPECT_EQ(UnmigratedSlotCount(), 46u);
    EXPECT_EQ(LocallyAnsweredSlotCount() + ImplementedVerbCount() + UnmigratedSlotCount(),
              kRemoteEmitSlotCount);
}

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
    const ChildResult r = RunInChild([] {
        RemoteEmitTable().GL.DrawElements(0x0004 /*GL_TRIANGLES*/, 3, 0x1405 /*GL_UNSIGNED_INT*/,
                                          nullptr);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedVerb, \"DrawElements\"}"), std::string::npos) << r.Log;
}

TEST(RemoteEmitTable, EachUnmigratedSlotNamesItsOwnSlot) {
    // The half the case above cannot state on its own: that the name in the message is the
    // slot's and not a constant. Two different slots, two different names.
    const ChildResult r = RunInChild([] { RemoteEmitTable().GL.GetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedVerb, \"GetTexImage\"}"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("DrawElements"), std::string::npos)
        << "the Fatal message names a slot other than the one that was called:\n"
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
    const void* fatal = reinterpret_cast<const void*>(table.GL.GetTexImage); // wave-3 tail, class C
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

TEST(CapsMirrorTest, APlaceholderMirrorConsumesNothing) {
    // The safe direction, stated as a case. With no snapshot the mask is zero, every family
    // answers "no consumer", the client emits nothing and the legacy pull path runs. The unsafe
    // direction - emitting to a server that has no consumer - is ID-39's 66 lost uploads.
    MGPCaps empty{};
    CapsMirror mirror;
    EXPECT_FALSE(mirror.Valid());
    EXPECT_FALSE(mirror.ServerConsumes(kMGPipeSubsystemResources));
    EXPECT_FALSE(mirror.ServerConsumes(kMGPipeSubsystemPrograms));
    EXPECT_FALSE(mirror.HasCap(kCapResidentSubData));
    (void)empty;
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
// B3 / codex 9: the CLIENT arm's 37 rows are observed, not just the monolith install
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
    C1F_MOVED(Context, SetGlobalConstants);
    C1F_MOVED(Context, SetVertexAttribDefaults);
    C1F_MOVED(Context, SetPixelPackState);
    C1F_MOVED(Context, SetPatchState);
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
#undef C1F_ESCAPE
    const SizeT movedContext = CountDifferingCells(gMGPipeContext, MGPipeMonolithContext());
    EXPECT_EQ(movedScreen + movedContext, 33u)
        << "exactly the 33 generated routed rows must differ from the monolith adapters; "
        << movedScreen + movedContext
        << " did, so a row was left on the monolith adapter (it would run the applier on the GL "
           "thread under split) or an unrouted row was overwritten";

    const SizeT movedEscapes = CountDifferingCells(gMGPipeRouteEscapes, MGPipeMonolithEscapes());
    EXPECT_EQ(movedEscapes, 4u)
        << "the four escape routes must move off the monolith escapes too";

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
    unsigned calls = 0;
    Bool OnClear(const MGPClear& v) override { clear = v; ++calls; return true; }
    Bool OnCopyFramebufferToTexture(const MGPCopyFromFramebuffer& v) override { copy = v; ++calls; return true; }
    Bool OnGenerateMipmap(const MGPMipPlan& v) override { mip = v; ++calls; return true; }
    void Install() {
        if (Srv::ServerLoopInstance().RunOnApplyThread([](void* self) {
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
        const auto& r = peer.mip;
        if (peer.calls != 1 || r.Res != MGPipeTextureEmitterInstance().FindTexture(*tex) ||
            MGPipeHandleIsNull(r.Res) || r.Target != GL_TEXTURE_2D || r.BaseLevel != 1 || r.LevelCount != 3)
            ::_exit(101);
        ClientSessionInstance().Stop();
    });
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << "F1.GenerateMipmap.fields: " << DescribeStatus(child) << child.Log;
}
#endif



#if MGTEST_HAVE_FORK

TEST(RemoteF1, UnboundNamedfvRefusesByName) {
    // Red once (executed, reverted): disable the named-FBO refusal; its exact Fatal disappears.
    const auto child = RunInChild([] {
        CapsPeer backend;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPClear r{};
        r.Fbo = {701, 1};
        r.Kind = kMGPipeClearKindColor;
        r.ValueClass = kMGPipeClearValueClassFloat;
        sink.OnClear(r);
    });
    ExpectNamedAbort(child, "Fatal{UnmigratedVerb, \"ClearNamedFramebufferfv+UNBOUND\"}");
}

TEST(RemoteF1, UnboundNamedivRefusesByName) {
    // Red once (executed, reverted): disable the named-FBO refusal; its exact Fatal disappears.
    const auto child = RunInChild([] {
        CapsPeer backend;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPClear r{};
        r.Fbo = {701, 1};
        r.Kind = kMGPipeClearKindColor;
        r.ValueClass = kMGPipeClearValueClassInt;
        sink.OnClear(r);
    });
    ExpectNamedAbort(child, "Fatal{UnmigratedVerb, \"ClearNamedFramebufferiv+UNBOUND\"}");
}

TEST(RemoteF1, UnboundNameduivRefusesByName) {
    // Red once (executed, reverted): disable the named-FBO refusal; its exact Fatal disappears.
    const auto child = RunInChild([] {
        CapsPeer backend;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPClear r{};
        r.Fbo = {701, 1};
        r.Kind = kMGPipeClearKindColor;
        r.ValueClass = kMGPipeClearValueClassUint;
        sink.OnClear(r);
    });
    ExpectNamedAbort(child, "Fatal{UnmigratedVerb, \"ClearNamedFramebufferuiv+UNBOUND\"}");
}

TEST(RemoteF1, UnboundNamedfiRefusesByName) {
    // Red once (executed, reverted): disable the named-FBO refusal; its exact Fatal disappears.
    const auto child = RunInChild([] {
        CapsPeer backend;
        Srv::ServerVerbSink sink;
        sink.SetBackend(&backend);
        MGPClear r{};
        r.Fbo = {701, 1};
        r.Kind = kMGPipeClearKindDepthStencil;
        r.ValueClass = kMGPipeClearValueClassFloat;
        sink.OnClear(r);
    });
    ExpectNamedAbort(child, "Fatal{UnmigratedVerb, \"ClearNamedFramebufferfi+UNBOUND\"}");
}
#endif

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
