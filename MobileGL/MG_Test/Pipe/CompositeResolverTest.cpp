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
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/PipeApply.h>
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
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
