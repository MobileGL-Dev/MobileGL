// MobileGL - MobileGL/MG_Test/Pipe/SamplerEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's sampler family: the content-addressed sampler CSO, the identity-addressed sampler view
// per texture object, and the set_sampler_views / bind_sampler_states unit sets.
//
// THIS SUITE IS A NAMED GATE (`ctest -R 'SamplerEmit\.'`), and its negative control is a
// script that stops the conversion copying SamplerParameters::borderColorForm and expects this
// suite to go red NAMING that field - which it must, because all three border-colour
// representations are always numerically populated and the value alone cannot say which driver
// entry point to use.
//
// THE ONE CASE THAT LOOKS LIKE PARANOIA AND IS NOT: SamplerParameters is 100 bytes with THREE
// BYTES OF TRAILING PADDING, so a cache that hashes or memcmps the object's own bytes reads
// uninitialised memory and mints a fresh CSO per call - a 256-entry cache with a hit rate of
// zero, and nobody notices, because the pixels are right. The case that writes garbage into
// the padding through a byte pointer is what turns that into a red gate.
//
// THE SUITE IS `SamplerEmit`, not `SamplerEmitTest`: the file is XTest.cpp and the suite is X,
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
#include <MG_Impl/Pipe/SamplerEmit.h>
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
#endif // MOBILEGL_PIPE_PUSH
} // namespace

// See FramebufferEmitTest's twin for why this is a shape pin rather than a placeholder.
TEST(SamplerEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeSamplerEmitterInstance(), &MGPipeSamplerEmitterInstance());
    // One bit for the whole sampler family - the CSO, the view and all three unit sets,
    // including set_shader_images, whose emitter lives in ImageEmit.h. An operator switching
    // samplers off has to get the whole family's legacy arm, not two thirds of it.
    EXPECT_TRUE(kMGPipeWiredSamplerSubsystem == 0 ||
                kMGPipeWiredSamplerSubsystem == kMGPipeSubsystemSamplers);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

// =========================================================================================
// The APPLIER's half of the sampler family (the wire commits'): the CSO record, the
// identity-addressed view record and its back-pointer, and the two unit sets. The emitter's
// half - the content-addressed 256-entry cache, the canonical zero-initialised copy the hash
// and the memcmp run over, the padding that cannot change the hash, borderColorForm crossing -
// is the client package's and lands beside these.
// =========================================================================================

#if MOBILEGL_PIPE_PUSH
namespace {
    // Every field carries a value of its own, INCLUDING borderColorForm and all three border
    // representations: they are always numerically populated, so the value alone cannot say
    // which driver entry point to use and the form is what does.
    SamplerParameters SamplerValues(Float lodBias, BorderColorForm form) {
        SamplerParameters params{};
        params.wrapS = SamplerWrapMode::ClampToEdge;
        params.wrapT = SamplerWrapMode::MirroredRepeat;
        params.minFilter = SamplerFilterMode::Linear;
        params.magFilter = SamplerFilterMode::Nearest;
        params.mipmapMode = SamplerMipmapMode::Nearest;
        params.lodBias = lodBias;
        params.maxAnisotropy = 4.0f;
        params.compareMode = SamplerCompareMode::CompareToTexture;
        params.borderColor = {0.25f, 0.5f, 0.75f, 1.0f};
        params.borderColorI = {-1, 2, -3, 4};
        params.borderColorUI = {5u, 6u, 7u, 8u};
        params.borderColorForm = form;
        return params;
    }

    MGPSamplerDesc SamplerDesc(MGPipeHandle cso, Uint64 declaredBlobSize) {
        MGPSamplerDesc desc{};
        desc.Cso = cso;
        desc.Parameters.Size = declaredBlobSize;
        return desc;
    }

    MGPSamplerView ViewOf(MGPipeHandle cso, MGPipeHandle texture, Uint16 minLevel) {
        MGPSamplerView view{};
        view.Cso = cso;
        view.Texture = texture;
        view.InternalFormat = 0x8058u; // GL_RGBA8
        view.Target = static_cast<Uint8>(MGPipeResourceTarget::Tex2D);
        view.MinLevel = minLevel;
        view.NumLevels = 4;
        view.MinLayer = 0;
        view.NumLayers = 1;
        view.Samples = 1;
        return view;
    }

    MGPHandleOnly SamplerHandle(MGPipeHandle cso) {
        return MGPHandleOnly{cso, static_cast<Uint32>(MGPipeKind::SamplerCso), 0};
    }
    MGPHandleOnly ViewHandle(MGPipeHandle cso) {
        return MGPHandleOnly{cso, static_cast<Uint32>(MGPipeKind::SamplerViewCso), 0};
    }

    MGPResourceDesc TextureDesc(MGPipeHandle res, Uint32 glName) {
        MGPResourceDesc desc{};
        desc.Resource = res;
        desc.Target = static_cast<Uint8>(MGPipeResourceTarget::Tex2D);
        desc.GlNameForDiag = glName;
        return desc;
    }
} // namespace
#endif

// A create starts the record over and leaves Serial at 0 - so a fresh backend twin that starts
// its own synced serial at 0 agrees without either side publishing anything - while a re-issue
// on a LIVE identity counts up, which is how a value change travels on a handle whose
// generation moves only on slot reuse.
TEST(SamplerEmit, ACreateStoresTheParametersByValueAndAReissueOnALiveIdentityCountsUp) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle cso{6, 2};
    const SamplerParameters first = SamplerValues(0.5f, BorderColorForm::Int);
    MGPipeApplyCreateSamplerState(SamplerDesc(cso, 0), &first);

    ASSERT_GT(MGPipeApplier().SamplerCsos.size(), 6u);
    const MGPipeSamplerCsoRecord& record = MGPipeApplier().SamplerCsos[6];
    EXPECT_TRUE(record.Live);
    EXPECT_EQ(record.Gen, 2u);
    EXPECT_EQ(record.Serial, 0u) << "a create is not a mutation";
    EXPECT_EQ(record.Params.wrapT, SamplerWrapMode::MirroredRepeat);
    EXPECT_EQ(record.Params.magFilter, SamplerFilterMode::Nearest);
    EXPECT_EQ(record.Params.compareMode, SamplerCompareMode::CompareToTexture);
    EXPECT_FLOAT_EQ(record.Params.lodBias, 0.5f);
    EXPECT_FLOAT_EQ(record.Params.borderColor.z(), 0.75f);
    EXPECT_EQ(record.Params.borderColorI.x(), -1);
    EXPECT_EQ(record.Params.borderColorUI.w(), 8u);
    EXPECT_EQ(record.Params.borderColorForm, BorderColorForm::Int)
        << "borderColorForm crosses; without it the backend cannot choose an entry point";

    const SamplerParameters second = SamplerValues(1.5f, BorderColorForm::Uint);
    MGPipeApplyCreateSamplerState(SamplerDesc(cso, sizeof(SamplerParameters)), &second);
    EXPECT_EQ(MGPipeApplier().SamplerCsos[6].Serial, 1u);
    EXPECT_FLOAT_EQ(MGPipeApplier().SamplerCsos[6].Params.lodBias, 1.5f);
    EXPECT_EQ(MGPipeApplier().SamplerCsos[6].Params.borderColorForm, BorderColorForm::Uint);

    // A RECYCLED SLOT STARTS OVER. Inheriting one field of the previous occupant - a serial, a
    // filter - is precisely how a sampler at a recycled slot inherits its predecessor's state.
    const SamplerParameters third = SamplerValues(2.5f, BorderColorForm::Float);
    MGPipeApplyCreateSamplerState(SamplerDesc(MGPipeHandle{6, 3}, 0), &third);
    EXPECT_EQ(MGPipeApplier().SamplerCsos[6].Gen, 3u);
    EXPECT_EQ(MGPipeApplier().SamplerCsos[6].Serial, 0u) << "a recycled slot kept its predecessor's serial";
    EXPECT_FLOAT_EQ(MGPipeApplier().SamplerCsos[6].Params.lodBias, 2.5f);
#endif
}

// The one Blob rule, on this family's own blob: a non-zero declared length must be exactly one
// SamplerParameters, a zero means "this record does not declare its blob" - which is what a
// monolith emission is - and either way the bytes read are bounded by the TYPE.
TEST(SamplerEmit, ARecordThatDoesNotDescribeItsOwnParametersIsRefusedNamingTheLength) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const SamplerParameters values = SamplerValues(0.0f, BorderColorForm::Float);

    const MGPSamplerDesc lying = SamplerDesc(MGPipeHandle{4, 1}, sizeof(SamplerParameters) + 1);
    ExpectRefusedNaming("create_sampler_state {slot=4, gen=1}: the declared blob length is not one "
                        "SamplerParameters",
                        [&lying, &values]() { MGPipeApplyCreateSamplerState(lying, &values); });
    EXPECT_TRUE(MGPipeApplier().SamplerCsos.empty());

    const MGPSamplerDesc undeclared = SamplerDesc(MGPipeHandle{4, 1}, 0);
    ExpectRefusedNaming("create_sampler_state {slot=4, gen=1}: the record declares no parameters and "
                        "carries none",
                        [&undeclared]() { MGPipeApplyCreateSamplerState(undeclared, nullptr); });
    EXPECT_TRUE(MGPipeApplier().SamplerCsos.empty());

    // And the slot bound, which is the one number in the family that reaches an allocator.
    const MGPSamplerDesc pastTheBound = SamplerDesc(MGPipeHandle{kMGPipeMaxSamplerCsoSlots, 1}, 0);
    ExpectRefusedNaming("create_sampler_state {slot=65536, gen=1}: the slot is outside the record table's "
                        "bound",
                        [&pastTheBound, &values]() { MGPipeApplyCreateSamplerState(pastTheBound, &values); });
    EXPECT_TRUE(MGPipeApplier().SamplerCsos.empty()) << "the table was grown by a corrupt slot";
#endif
}

// A death notice on a record the applier does not have is the ONE refusal a legal sequence
// produces - the teardown order - so it stays a defined no-op, and it is COUNTED because
// MOBILEGL_ASSERT compiles out at INFO and every build that matters is one.
TEST(SamplerEmit, ADeleteDropsTheRecordAndAStaleNoticeIsCountedRatherThanSilentlyDropped) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle cso{3, 7};
    const SamplerParameters values = SamplerValues(0.0f, BorderColorForm::Float);
    MGPipeApplyCreateSamplerState(SamplerDesc(cso, 0), &values);
    ASSERT_TRUE(MGPipeApplier().SamplerCsos[3].Live);

    MGPipeApplyDeleteSamplerState(SamplerHandle(cso));
    EXPECT_FALSE(MGPipeApplier().SamplerCsos[3].Live);
    EXPECT_EQ(MGPipeApplier().SamplerCsos[3].Gen, 7u) << "a destroy keeps the generation";
    EXPECT_EQ(MGPipeApplier().SamplerCsos[3].Params.wrapT, SamplerWrapMode::Repeat)
        << "a stale read of a deleted slot must find nothing, not the state that used to be there";
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u);

    // The second notice - the one a teardown produces - is refused and counted.
    MGPipeApplyDeleteSamplerState(SamplerHandle(cso));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 1u);
    MGPipeApplyDeleteSamplerState(SamplerHandle(MGPipeHandle{3, 8}));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 2u);
#endif
}

// A sampler view is IDENTITY-addressed one per texture object, minted off that object's
// lifetime id, so a restriction change is a re-issue on the same handle rather than a new one.
// The texture's own back-pointer is written here and cleared by the delete, and both are silent
// lookups: the texture bit and the sampler bit are independent, so a view arriving without its
// texture is an ordering fact and not a refusal.
TEST(SamplerEmit, AViewIsReissuedOnTheSameHandleAndKeepsItsTexturesBackPointerInStep) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{5, 1};
    const MGPipeHandle view{9, 2};
    MGPipeApplyResourceCreate(TextureDesc(texture, 42));

    MGPipeApplyCreateSamplerView(ViewOf(view, texture, 0));
    ASSERT_GT(MGPipeApplier().SamplerViewCsos.size(), 9u);
    EXPECT_TRUE(MGPipeApplier().SamplerViewCsos[9].Live);
    EXPECT_EQ(MGPipeApplier().SamplerViewCsos[9].Serial, 0u);
    EXPECT_EQ(MGPipeApplier().SamplerViewCsos[9].View.InternalFormat, 0x8058u);
    EXPECT_EQ(MGPipeApplier().SamplerViewCsos[9].View.NumLevels, 4u);
    EXPECT_EQ(MGPipeApplier().TextureResources[5].ViewCso, view)
        << "the texture's back-pointer to its one view was not written";

    // A restriction change: same handle, serial up, nothing started over.
    MGPipeApplyCreateSamplerView(ViewOf(view, texture, 2));
    EXPECT_EQ(MGPipeApplier().SamplerViewCsos[9].Serial, 1u);
    EXPECT_EQ(MGPipeApplier().SamplerViewCsos[9].View.MinLevel, 2u);

    // A view whose texture this applier has not been told about is stored anyway - refusing it
    // would make one legal A/B arm drop every view - and it counts no refusal.
    MGPipeApplyCreateSamplerView(ViewOf(MGPipeHandle{10, 1}, MGPipeHandle{77, 1}, 0));
    EXPECT_TRUE(MGPipeApplier().SamplerViewCsos[10].Live);
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 0u);

    MGPipeApplyDeleteSamplerView(ViewHandle(view));
    EXPECT_FALSE(MGPipeApplier().SamplerViewCsos[9].Live);
    EXPECT_EQ(MGPipeApplier().SamplerViewCsos[9].Gen, 2u);
    EXPECT_EQ(MGPipeApplier().TextureResources[5].ViewCso, kMGPipeNullHandle)
        << "the texture kept a back-pointer to a view that is gone";
    MGPipeApplyDeleteSamplerView(ViewHandle(view));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, 1u);
#endif
}

// THE WINDOW IS THE BOUND AND ENTRIES OUTSIDE IT ARE NOT CLEARED: a set that names four units
// has said nothing about the other 188, and clearing them would unbind textures the client
// never mentioned. Deleting the entry loop, the window gate or either serial bump leaves this
// red.
TEST(SamplerEmit, TheTwoUnitSetsLandInTheirWindowAndLeaveEverythingOutsideItAlone) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;

    MGPBoundView views[2] = {};
    views[0].View = MGPipeHandle{1, 1};
    views[0].Texture = MGPipeHandle{2, 1};
    views[0].Unit = 4;
    views[1].View = kMGPipeNullHandle; // a unit the program does not resolve is legal
    views[1].Texture = MGPipeHandle{3, 1};
    views[1].Unit = 5;
    MGPSamplerViews viewHeader{};
    viewHeader.Start = 4;
    viewHeader.Count = 2;
    viewHeader.ContentHash = 0xABCDu;

    const Uint64 viewSerial = MGPipeApplier().SamplerViewsSerial;
    MGPipeApplySetSamplerViews(viewHeader, views);
    EXPECT_EQ(MGPipeApplier().SamplerViewStart, 4u);
    EXPECT_EQ(MGPipeApplier().SamplerViewCount, 2u);
    EXPECT_EQ(MGPipeApplier().BoundSamplerViews[4].View, (MGPipeHandle{1, 1}));
    EXPECT_EQ(MGPipeApplier().BoundSamplerViews[5].Texture, (MGPipeHandle{3, 1}));
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundSamplerViews[5].View));
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundSamplerViews[3].View)) << "the set wrote below its window";
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundSamplerViews[6].View)) << "the set wrote above its window";
    EXPECT_GT(MGPipeApplier().SamplerViewsSerial, viewSerial);

    MGPipeHandle states[2] = {MGPipeHandle{8, 1}, kMGPipeNullHandle};
    MGPSamplerStates stateHeader{};
    stateHeader.Start = 4;
    stateHeader.Count = 2;
    const Uint64 stateSerial = MGPipeApplier().SamplerStatesSerial;
    MGPipeApplyBindSamplerStates(stateHeader, states);
    EXPECT_EQ(MGPipeApplier().BoundSamplerStates[4], (MGPipeHandle{8, 1}));
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundSamplerStates[5]))
        << "a unit with no sampler object carries a null CSO and the texture's built-in one applies";
    EXPECT_EQ(MGPipeApplier().SamplerStateCount, 2u);
    EXPECT_GT(MGPipeApplier().SamplerStatesSerial, stateSerial);
    EXPECT_EQ(MGPipeApplier().SamplerViewsSerial, viewSerial + 1)
        << "one set moved the other set's serial; the three are independent";

    // A NARROWER SET DOES NOT CLEAR WHAT IT DOES NOT NAME - "the last set as received".
    MGPSamplerViews narrow{};
    narrow.Start = 4;
    narrow.Count = 1;
    MGPipeApplySetSamplerViews(narrow, views);
    EXPECT_EQ(MGPipeApplier().SamplerViewCount, 1u);
    EXPECT_EQ(MGPipeApplier().BoundSamplerViews[5].Texture, (MGPipeHandle{3, 1}))
        << "the entry outside the new window was cleared";
#endif
}

// The window gate itself, at the bound and one past it, plus the null-tail arm. A header that
// describes more than its destination can hold is the same class of fault as a blob outside
// its segment, and the destination here is the merged 192-unit space.
TEST(SamplerEmit, AUnitWindowPastTheMergedUnitSpaceIsRefusedRatherThanTruncated) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    MGPBoundView entry{};
    entry.Texture = MGPipeHandle{2, 1};

    // The positive control: a window ending EXACTLY at the bound is fine.
    MGPSamplerViews exact{};
    exact.Start = kMGPipeMaxTextureUnits - 1;
    exact.Count = 1;
    MGPipeApplySetSamplerViews(exact, &entry);
    ASSERT_EQ(MGPipeApplier().SamplerViewCount, 1u);
    const Uint64 serialBefore = MGPipeApplier().SamplerViewsSerial;

    MGPSamplerViews past{};
    past.Start = kMGPipeMaxTextureUnits - 1;
    past.Count = 2;
    past.ContentHash = 7;
    ExpectRefusedNaming("set_sampler_views {start=191, count=2, hash=7}: the window runs past the merged "
                        "texture-unit space",
                        [&past, &entry]() { MGPipeApplySetSamplerViews(past, &entry); });

    MGPSamplerStates statesPast{};
    statesPast.Start = 0;
    statesPast.Count = kMGPipeMaxTextureUnits + 1;
    ExpectRefusedNaming("bind_sampler_states {start=0, count=193, hash=0}: the window runs past the merged "
                        "texture-unit space",
                        [&statesPast]() {
                            MGPipeHandle one = kMGPipeNullHandle;
                            MGPipeApplyBindSamplerStates(statesPast, &one);
                        });

    MGPSamplerViews noTail{};
    noTail.Start = 0;
    noTail.Count = 3;
    ExpectRefusedNaming("set_sampler_views {start=0, count=3, hash=0}: a non-empty set carries no entries",
                        [&noTail]() { MGPipeApplySetSamplerViews(noTail, nullptr); });

    EXPECT_EQ(MGPipeApplier().SamplerViewsSerial, serialBefore) << "a refused set moved the serial";
    EXPECT_EQ(MGPipeApplier().SamplerViewStart, kMGPipeMaxTextureUnits - 1);
#endif
}

// D-J4 for this family: the CSO and the view are OBJECT records and survive a make-current;
// the two unit sets are WORKING state and do not, and their serials advance rather than
// restarting.
TEST(SamplerEmit, AMakeCurrentTakesTheUnitSetsAndLeavesTheCsoAndViewRecordsStanding) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const SamplerParameters values = SamplerValues(3.0f, BorderColorForm::Float);
    MGPipeApplyCreateSamplerState(SamplerDesc(MGPipeHandle{2, 1}, 0), &values);
    MGPipeApplyCreateSamplerView(ViewOf(MGPipeHandle{3, 1}, MGPipeHandle{4, 1}, 1));
    MGPBoundView entry{};
    entry.Texture = MGPipeHandle{4, 1};
    MGPSamplerViews header{};
    header.Count = 1;
    MGPipeApplySetSamplerViews(header, &entry);
    const Uint64 viewsSerial = MGPipeApplier().SamplerViewsSerial;
    const Uint64 statesSerial = MGPipeApplier().SamplerStatesSerial;

    MGPipeApplierReset();

    EXPECT_TRUE(MGPipeApplier().SamplerCsos[2].Live) << "a make-current dropped a share-group CSO record";
    EXPECT_FLOAT_EQ(MGPipeApplier().SamplerCsos[2].Params.lodBias, 3.0f);
    EXPECT_TRUE(MGPipeApplier().SamplerViewCsos[3].Live);
    EXPECT_EQ(MGPipeApplier().SamplerViewCount, 0u) << "the unit set is per context and must be cleared";
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundSamplerViews[0].Texture));
    EXPECT_GT(MGPipeApplier().SamplerViewsSerial, viewsSerial);
    EXPECT_GT(MGPipeApplier().SamplerStatesSerial, statesSerial);
#endif
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-sampleremit-test-" + std::to_string(ProcessId()) + ".log");
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
