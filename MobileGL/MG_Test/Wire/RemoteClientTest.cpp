// MobileGL - MobileGL/MG_Test/Wire/RemoteClientTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package c1's suite: the 71-slot emit table, the caps mirror and R-8's liveness gates. No
// session, no transport and no thread - s1's SessionTest owns those and w1's PipeWireCodecTest
// owns the bytes.
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
#include <string>

#include "Includes.h"

#include <Config.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Client/CapsMirror.h>
#include <MG_Remote/Client/EmitTables.h>

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
    // CONTRACT-P5.md §7: 2 answered locally + 5 emitted + 64 Fatal. Read from the functions the
    // table itself reports with - which is also what t1's arming condition reads - rather than
    // recomputed here, so a table that lost an emitter cannot look like one that never had it.
    EXPECT_EQ(LocallyAnsweredSlotCount(), 2u);
    EXPECT_EQ(ImplementedVerbCount(), 5u);
    EXPECT_EQ(UnmigratedSlotCount(), 64u);
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
    const ChildResult r = RunInChild([] { RemoteEmitTable().GL.GenerateMipmap(0x0DE1); });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{UnmigratedVerb, \"GenerateMipmap\"}"), std::string::npos) << r.Log;
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

TEST(RemoteEmitTable, AClassBSlotWithNoSessionAbortsRatherThanFallingThrough) {
    // The other half of "no slot may fall through to the driver". With no ClientSession the
    // emitter has nowhere to put the record, and the one thing it may not do is return quietly:
    // that is the split lane running monolith and going green.
    const ChildResult r = RunInChild([] { RemoteEmitTable().GL.Clear(0x4000 /*COLOR_BUFFER_BIT*/); });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{NoClientSession, \"Clear\"}"), std::string::npos) << r.Log;
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
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
