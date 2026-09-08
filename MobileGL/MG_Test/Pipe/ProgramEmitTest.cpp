// MobileGL - MobileGL/MG_Test/Pipe/ProgramEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's program family: create/bind/delete_shader_state, set_draw_program,
// set_dispatch_program and set_global_constants.
//
// THE ONE PIN THAT IS EASIEST TO LOSE AND WORST TO LOSE: set_global_constants' Version is
// GetUBOContentVersion(), and ~0u is the BACKENDS' "never uploaded" sentinel - the wrap skips
// it - so the client must never emit it. A record carrying the sentinel would tell a backend
// that a block it has just been handed was never uploaded.
//
// THE SUITE IS `ProgramEmit`, not `ProgramEmitTest`: the file is XTest.cpp and the suite is X,
// this directory's convention, and it is what the gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT: the
// applier-side cases are the wire commits' and the emitter-side cases are the client
// package's, and neither has to come back to MG_Test/Pipe/CMakeLists.txt to add one.
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
#include <MG_Impl/Pipe/ProgramEmit.h>
#include <MG_Pipe/PipeApply.h>
// The applier takes the two artefact structs BY POINTER beside the record, so a case that
// drives create_shader_state needs their definitions - the applier's own header deliberately
// only forward-declares them.
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

// See FramebufferEmitTest's twin for why this is a shape pin rather than a placeholder.
TEST(ProgramEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeProgramEmitterInstance(), &MGPipeProgramEmitterInstance());
    EXPECT_TRUE(kMGPipeWiredProgramSubsystem == 0 ||
                kMGPipeWiredProgramSubsystem == kMGPipeSubsystemPrograms);
    // The record the applier starts from carries the sentinel, not 0: a program that has never
    // published a default-uniform-block image must not look like one that published version 0.
    const MGPipeShaderCsoRecord fresh{};
    EXPECT_EQ(fresh.GlobalConstantsVersion, ~Uint32{0});
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

// =========================================================================================
// The APPLIER's half of the program family (the wire commits'): the shader CSO record, the
// three bindings and the default uniform block. The emitter's half - the join at the validate
// point, the never-uploaded sentinel that must never be emitted, the composite resolver - is
// the client package's and lands beside these.
// =========================================================================================

#if MOBILEGL_PIPE_PUSH
namespace {
    using MG_State::GLState::LinkArtifacts;
    using MG_State::GLState::SpirvArtifacts;

    MGPProgramDesc ProgramDesc(MGPipeHandle cso, Uint32 stageMask, Uint32 globalUboSize) {
        MGPProgramDesc desc{};
        desc.Cso = cso;
        desc.StageMask = stageMask;
        desc.GlobalUboSize = globalUboSize;
        desc.ReservedNumSamplesOffset = 32;
        desc.SpirvStatus = 1;
        desc.NativeFloat64 = 1;
        desc.PointSizeDemoted = 1;
        desc.EnableSpirvValidation = 1;
        // ALL SEVEN BLOB REFS ARE DECLARED WITH Size 0 - "this record does not declare its
        // blob" - which is exactly what a monolith emission is: the artefacts ride beside the
        // record through the two companion pointers and the codec is never called.
        return desc;
    }

    MGPHandleOnly ProgramHandle(MGPipeHandle cso) {
        return MGPHandleOnly{cso, static_cast<Uint32>(MGPipeKind::ShaderCso), 0};
    }

    MGPGlobalConstants GlobalConstants(MGPipeHandle cso, Uint32 version) {
        MGPGlobalConstants record{};
        record.ShaderCso = cso;
        record.Version = version;
        return record;
    }

    const MGPipeShaderCsoRecord& ProgramRecordOf(Uint32 slot) {
        EXPECT_GT(MGPipeApplier().ShaderCsos.size(), static_cast<SizeT>(slot));
        return MGPipeApplier().ShaderCsos[slot];
    }
} // namespace
#endif

// A create starts the record over and leaves Serial at 0; a RE-ISSUE on the same handle is how
// a relink travels, and it takes the default uniform block with it - a block sized to a layout
// that no longer exists is worse than no block, and the sentinel is the value that says
// "nothing has been uploaded for this program".
TEST(ProgramEmit, ACreateStoresTheDescriptorAndARelinkCountsUpAndDropsTheBlockKeyedToTheOldLayout) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;
    const MGPipeHandle cso{5, 2};
    const Uint8 block[64] = {};

    MGPipeApplyCreateShaderState(ProgramDesc(cso, 0x3u, 64), &link, &spirv);
    EXPECT_TRUE(ProgramRecordOf(5).Live);
    EXPECT_EQ(ProgramRecordOf(5).Gen, 2u);
    EXPECT_EQ(ProgramRecordOf(5).Serial, 0u) << "a create is not a mutation";
    EXPECT_EQ(ProgramRecordOf(5).Desc.StageMask, 0x3u);
    EXPECT_EQ(ProgramRecordOf(5).Desc.GlobalUboSize, 64u);
    EXPECT_EQ(ProgramRecordOf(5).Desc.ReservedNumSamplesOffset, 32u);
    EXPECT_EQ(ProgramRecordOf(5).Desc.NativeFloat64, 1u);
    EXPECT_EQ(ProgramRecordOf(5).GlobalConstantsVersion, ~Uint32{0})
        << "a fresh record starts at the never-uploaded sentinel";

    MGPipeApplySetGlobalConstants(GlobalConstants(cso, 7), block);
    ASSERT_EQ(ProgramRecordOf(5).GlobalConstants.size(), 64u);
    const Uint64 blockSerial = ProgramRecordOf(5).GlobalConstantsSerial;

    // The relink.
    MGPipeApplyCreateShaderState(ProgramDesc(cso, 0x7u, 32), &link, &spirv);
    EXPECT_EQ(ProgramRecordOf(5).Serial, 1u);
    EXPECT_EQ(ProgramRecordOf(5).Desc.StageMask, 0x7u);
    EXPECT_TRUE(ProgramRecordOf(5).GlobalConstants.empty())
        << "a block sized to the layout the relink replaced survived it";
    EXPECT_EQ(ProgramRecordOf(5).GlobalConstantsVersion, ~Uint32{0});
    EXPECT_GT(ProgramRecordOf(5).GlobalConstantsSerial, blockSerial)
        << "the clearing was not announced, so a twin can still match what it uploaded before";

    // A RECYCLED SLOT STARTS OVER: inheriting one field of the previous occupant is how a
    // program at a recycled slot inherits its predecessor's reflection.
    MGPipeApplyCreateShaderState(ProgramDesc(MGPipeHandle{5, 3}, 0x1u, 16), &link, &spirv);
    EXPECT_EQ(ProgramRecordOf(5).Gen, 3u);
    EXPECT_EQ(ProgramRecordOf(5).Serial, 0u) << "a recycled slot kept its predecessor's serial";
    EXPECT_EQ(ProgramRecordOf(5).Desc.StageMask, 0x1u);
#endif
}

// The three refusals a create can produce: no artefacts at all behind seven undeclared blobs, a
// default uniform block no program can have, and a slot outside the record table's bound.
TEST(ProgramEmit, ACreateWithNoArtefactsAnOversizedBlockOrACorruptSlotIsRefusedNamingTheProgram) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;

    const MGPProgramDesc desc = ProgramDesc(MGPipeHandle{4, 1}, 0x3u, 0);
    ExpectRefusedNaming("create_shader_state {slot=4, gen=1}: the record declares no blobs and carries no "
                        "artefacts",
                        [&desc, &spirv]() { MGPipeApplyCreateShaderState(desc, nullptr, &spirv); });
    ExpectRefusedNaming("create_shader_state {slot=4, gen=1}: the record declares no blobs and carries no "
                        "artefacts",
                        [&desc, &link]() { MGPipeApplyCreateShaderState(desc, &link, nullptr); });
    EXPECT_TRUE(MGPipeApplier().ShaderCsos.empty());

    const MGPProgramDesc huge = ProgramDesc(MGPipeHandle{4, 1}, 0x3u, kMGPipeMaxGlobalConstantsBytes + 1);
    ExpectRefusedNaming("create_shader_state {slot=4, gen=1}: the default uniform block is larger than any "
                        "program may declare",
                        [&huge, &link, &spirv]() { MGPipeApplyCreateShaderState(huge, &link, &spirv); });
    EXPECT_TRUE(MGPipeApplier().ShaderCsos.empty()) << "the table was grown by a refused record";

    const MGPProgramDesc pastTheBound = ProgramDesc(MGPipeHandle{kMGPipeMaxShaderCsoSlots, 1}, 0x3u, 0);
    ExpectRefusedNaming("create_shader_state {slot=1048576, gen=1}: the slot is outside the record table's "
                        "bound",
                        [&pastTheBound, &link, &spirv]() {
                            MGPipeApplyCreateShaderState(pastTheBound, &link, &spirv);
                        });
    EXPECT_TRUE(MGPipeApplier().ShaderCsos.empty());
    EXPECT_TRUE(MGPipeApplier().CompositeShaderCsos.empty());
#endif
}

// Three bindings, one serial, and each of them follows its OWN handle: set_draw_program and
// set_dispatch_program are two calls because the frontend has two joins. A null handle is legal
// and means "nothing bound"; a dead one leaves the previous binding standing and is counted.
TEST(ProgramEmit, TheThreeBindingsFollowTheirOwnHandleAndADeadOneLeavesThePreviousBindingStanding) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;
    const MGPipeHandle draw{2, 1};
    const MGPipeHandle dispatch{3, 1};
    MGPipeApplyCreateShaderState(ProgramDesc(draw, 0x3u, 0), &link, &spirv);
    MGPipeApplyCreateShaderState(ProgramDesc(dispatch, 0x20u, 0), &link, &spirv);

    const Uint64 serialBefore = MGPipeApplier().ProgramBindingSerial;
    MGPipeApplyBindShaderState(ProgramHandle(draw));
    MGPipeApplySetDrawProgram(ProgramHandle(draw));
    MGPipeApplySetDispatchProgram(ProgramHandle(dispatch));
    EXPECT_EQ(MGPipeApplier().BoundShaderCso, draw);
    EXPECT_EQ(MGPipeApplier().DrawProgram, draw);
    EXPECT_EQ(MGPipeApplier().DispatchProgram, dispatch);
    EXPECT_EQ(MGPipeApplier().ProgramBindingSerial, serialBefore + 3);

    // A dead handle: previous binding untouched, and COUNTED - a no-op nobody can see is a
    // dropped bind nobody can see.
    const Uint64 refusedBefore = MGPipeApplier().RefusedObjectCalls;
    MGPipeApplySetDrawProgram(ProgramHandle(MGPipeHandle{2, 9}));
    MGPipeApplyBindShaderState(ProgramHandle(MGPipeHandle{99, 1}));
    MGPipeApplySetDispatchProgram(ProgramHandle(MGPipeHandle{3, 9}));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, refusedBefore + 3);
    EXPECT_EQ(MGPipeApplier().DrawProgram, draw);
    EXPECT_EQ(MGPipeApplier().BoundShaderCso, draw);
    EXPECT_EQ(MGPipeApplier().DispatchProgram, dispatch);
    EXPECT_EQ(MGPipeApplier().ProgramBindingSerial, serialBefore + 3) << "a refused bind moved the serial";

    // The null handle is a state, not an error.
    MGPipeApplySetDrawProgram(ProgramHandle(kMGPipeNullHandle));
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().DrawProgram));
    EXPECT_EQ(MGPipeApplier().ProgramBindingSerial, serialBefore + 4);
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, refusedBefore + 3) << "a null bind was counted as a refusal";
#endif
}

// A delete drops the record whole, keeps the generation, and clears EVERY binding that named
// it - unlike the unit sets, which are "the last set as received". A binding left pointing at a
// dropped record would make the next verb refuse a state the applier itself created.
TEST(ProgramEmit, ADeleteDropsTheRecordAndClearsEveryBindingThatNamedIt) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;
    const MGPipeHandle cso{6, 4};
    MGPipeApplyCreateShaderState(ProgramDesc(cso, 0x3u, 0), &link, &spirv);
    MGPipeApplyBindShaderState(ProgramHandle(cso));
    MGPipeApplySetDrawProgram(ProgramHandle(cso));
    MGPipeApplySetDispatchProgram(ProgramHandle(cso));
    const Uint64 serialBefore = MGPipeApplier().ProgramBindingSerial;

    MGPipeApplyDeleteShaderState(ProgramHandle(cso));
    EXPECT_FALSE(ProgramRecordOf(6).Live);
    EXPECT_EQ(ProgramRecordOf(6).Gen, 4u) << "a destroy keeps the generation";
    EXPECT_EQ(ProgramRecordOf(6).Desc.StageMask, 0u)
        << "a stale read of a deleted slot must find nothing, not the program that used to be there";
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundShaderCso));
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().DrawProgram));
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().DispatchProgram));
    EXPECT_GT(MGPipeApplier().ProgramBindingSerial, serialBefore);

    // THE SECOND NOTICE IS A REFUSED NO-OP. A composite's slot has two independent release
    // paths and both arrive here; the second finding nothing is what makes the double free
    // proven rather than assumed.
    const Uint64 serialAfter = MGPipeApplier().ProgramBindingSerial;
    MGPipeApplyDeleteShaderState(ProgramHandle(cso));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 1u);
    EXPECT_EQ(MGPipeApplier().ProgramBindingSerial, serialAfter);
#endif
}

// The default uniform block lands on the PROGRAM's record - it is (ShaderCso, Version) keyed
// and belongs to the program, not to the context that uploaded it - and the length it is held
// to is the program's own GlobalUboSize, which the create already bounded.
TEST(ProgramEmit, TheDefaultUniformBlockLandsOnTheProgramsRecordAndTheSentinelIsRefused) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;
    const MGPipeHandle cso{7, 1};
    MGPipeApplyCreateShaderState(ProgramDesc(cso, 0x3u, 8), &link, &spirv);

    Uint8 block[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    MGPipeApplySetGlobalConstants(GlobalConstants(cso, 11), block);
    ASSERT_EQ(ProgramRecordOf(7).GlobalConstants.size(), 8u);
    EXPECT_EQ(ProgramRecordOf(7).GlobalConstants[7], 8u);
    EXPECT_EQ(ProgramRecordOf(7).GlobalConstantsVersion, 11u);
    EXPECT_EQ(ProgramRecordOf(7).GlobalConstantsSerial, 1u);
    EXPECT_EQ(ProgramRecordOf(7).Serial, 0u) << "a block upload is not a relink";

    // A DECLARED blob length that agrees is fine; one that does not is refused, and so is the
    // sentinel the backends read as "never uploaded".
    MGPGlobalConstants declared = GlobalConstants(cso, 12);
    declared.Blob.Size = 8;
    MGPipeApplySetGlobalConstants(declared, block);
    EXPECT_EQ(ProgramRecordOf(7).GlobalConstantsVersion, 12u);

    MGPGlobalConstants lying = GlobalConstants(cso, 13);
    lying.Blob.Size = 9;
    ExpectRefusedNaming("set_global_constants {slot=7, gen=1}: the declared blob length is not the "
                        "program's own default uniform block size",
                        [&lying, &block]() { MGPipeApplySetGlobalConstants(lying, block); });

    const MGPGlobalConstants sentinel = GlobalConstants(cso, ~Uint32{0});
    ExpectRefusedNaming("set_global_constants {slot=7, gen=1}: the version is the backends' "
                        "never-uploaded sentinel",
                        [&sentinel, &block]() { MGPipeApplySetGlobalConstants(sentinel, block); });

    const MGPGlobalConstants noBytes = GlobalConstants(cso, 14);
    ExpectRefusedNaming("set_global_constants {slot=7, gen=1}: a non-empty block carries no bytes",
                        [&noBytes]() { MGPipeApplySetGlobalConstants(noBytes, nullptr); });

    EXPECT_EQ(ProgramRecordOf(7).GlobalConstantsVersion, 12u) << "a refused block was stored anyway";
    EXPECT_EQ(ProgramRecordOf(7).GlobalConstantsSerial, 2u);

    // And a block for a program this applier does not have is the ordinary counted refusal.
    MGPipeApplySetGlobalConstants(GlobalConstants(MGPipeHandle{7, 2}, 15), block);
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 1u);
#endif
}

// D-J4 for this family: the program record is share-group state and survives a make-current -
// re-emitting create_shader_state for a record the applier still holds would move its serial
// for nothing - while the three bindings are working state and do not.
TEST(ProgramEmit, TheProgramRecordSurvivesAMakeCurrentWhileTheThreeBindingsDoNot) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const LinkArtifacts link;
    const SpirvArtifacts spirv;
    const MGPipeHandle cso{8, 1};
    const Uint8 block[4] = {9, 9, 9, 9};
    MGPipeApplyCreateShaderState(ProgramDesc(cso, 0x3u, 4), &link, &spirv);
    MGPipeApplySetGlobalConstants(GlobalConstants(cso, 21), block);
    MGPipeApplyBindShaderState(ProgramHandle(cso));
    MGPipeApplySetDrawProgram(ProgramHandle(cso));
    const Uint64 bindingSerial = MGPipeApplier().ProgramBindingSerial;

    MGPipeApplierReset();

    ASSERT_TRUE(ProgramRecordOf(8).Live) << "a make-current dropped a share-group program record";
    EXPECT_EQ(ProgramRecordOf(8).GlobalConstantsVersion, 21u);
    EXPECT_EQ(ProgramRecordOf(8).GlobalConstants.size(), 4u);
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundShaderCso));
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().DrawProgram));
    EXPECT_GT(MGPipeApplier().ProgramBindingSerial, bindingSerial)
        << "the binding serial was carried over or restarted rather than advanced";

    // The bind that follows the switch still resolves, which is the whole point of the rule.
    MGPipeApplySetDrawProgram(ProgramHandle(cso));
    EXPECT_EQ(MGPipeApplier().DrawProgram, cso);
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u);

    MGPipeApplierReleaseObjectRecords();
    EXPECT_TRUE(MGPipeApplier().ShaderCsos.empty());
#endif
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-programemit-test-" + std::to_string(ProcessId()) + ".log");
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
