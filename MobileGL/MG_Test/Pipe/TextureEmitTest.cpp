// MobileGL - MobileGL/MG_Test/Pipe/TextureEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's texture and renderbuffer family: resource_create from the constructor,
// resource_respecify from every storage definition, set_texture_params from the parameter
// mutators, and resource_subdata from the drain list at the validate point.
//
// THIS SUITE IS A NAMED GATE (`ctest -R 'TextureEmit\.'`), and one of its invariants is the
// one nothing else in the tree can see: the union box and the region list have to describe the
// SAME texels, because the server picks the upload shape from them and SSIM is completely
// blind to which one it picked. The Mali cliff behind that choice is ~+6 ms/frame for a
// hundred one-rect jobs against one union box.
//
// THE SUITE IS `TextureEmit`, not `TextureEmitTest`: the file is XTest.cpp and the suite is X,
// this directory's convention, and it is what the gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT. The
// applier-side cases are the wire commits'; the emitter-side cases (every texture target
// mapping to its own resource target, every bind kind setting its mask bit and the bit being
// sticky across a respecify, the image-bindable hint being forever, the box/rect invariant,
// the level shadow's strides, the collapse to the box past the rect cap, an upload through a
// view keying on the storage owner, every texture's params naming its built-in sampler CSO and
// two identical samplers sharing one, and a destroyed texture releasing its resource, view and
// sampler slots) are the client package's - and neither has to come back to
// MG_Test/Pipe/CMakeLists.txt to add one.
//
// IT HAS ITS OWN main() for ResourceEmitTest's reason: the applier's bounds and protocol trip
// wires report through a log line in a shipped push build and std::abort() in a poison or
// verify one.
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
#include <MG_Impl/Pipe/TextureEmit.h>
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

// See FramebufferEmitTest's twin for why this is a shape pin rather than a placeholder: a
// static holding client state whose destructor an exit handler can run is the exit-order
// use-after-free this design closed once already.
TEST(TextureEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeTextureEmitterInstance(), &MGPipeTextureEmitterInstance());
    EXPECT_TRUE(kMGPipeWiredTextureSubsystem == 0 ||
                kMGPipeWiredTextureSubsystem == kMGPipeSubsystemTextureResources);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

// =========================================================================================
// The APPLIER's half of the texture family (the wire commits'): set_texture_params on the
// texture's own record, and the sub-data validator plus the pending-upload set that replaces
// the frontend dirty flags the client clears at emission. The emitter's half - the descriptor
// builder for every target, the sticky bind mask, the drain list, the level-shadow strides -
// is the client package's and lands beside these.
// =========================================================================================

#if MOBILEGL_PIPE_PUSH
namespace {
    constexpr Uint16 kTex2D = static_cast<Uint16>(MGPipeResourceTarget::Tex2D);

    MGPResourceDesc TextureDesc(MGPipeHandle res, Uint32 width, Uint32 glName) {
        MGPResourceDesc desc{};
        desc.Resource = res;
        desc.Target = static_cast<Uint8>(MGPipeResourceTarget::Tex2D);
        desc.Width = width;
        desc.Height = width;
        desc.GlNameForDiag = glName;
        return desc;
    }

    // Every field carries a value of its own so a body that stored the wrong one is visible BY
    // FIELD, which is what the family's descriptor-consistency control needs of it.
    MGPTextureParams TextureParams(MGPipeHandle res, MGPipeHandle builtinSampler, Uint16 baseLevel) {
        MGPTextureParams params{};
        params.Res = res;
        params.BuiltinSampler = builtinSampler;
        params.BaseLevel = baseLevel;
        params.MaxLevel = 7;
        params.Swizzle[0] = 1;
        params.Swizzle[1] = 2;
        params.Swizzle[2] = 3;
        params.Swizzle[3] = 4;
        params.DepthStencilMode = 5;
        params.ForceResync = 1;
        params.SamplerResync = 1;
        params.MinLod = -2.0f;
        params.MaxLod = 9.0f;
        params.LodBias = 0.5f;
        return params;
    }

    MGPSubData TextureUpload(MGPipeHandle res, Uint16 level, const MGPBox& box, Uint32 regionCount) {
        MGPSubData record{};
        record.Res = res;
        record.Target = kTex2D;
        record.Level = level;
        record.SourceIsVerbatimLevelShadow = 1;
        record.UnionBox = box;
        record.RegionCount = regionCount;
        return record;
    }

    MGPSubRegion Region(Int32 x, Int32 y, Uint32 w, Uint32 h) {
        MGPSubRegion region{};
        region.X = x;
        region.Y = y;
        region.Z = 0;
        region.W = w;
        region.H = h;
        region.D = 1;
        region.SrcOffset = static_cast<Uint64>(y) * 64 + static_cast<Uint64>(x) * 4;
        region.SrcRowStride = 256;
        region.SrcSliceStride = 0;
        return region;
    }

    const MGPipeResourceRecord& TextureRecordOf(Uint32 slot) {
        EXPECT_GT(MGPipeApplier().TextureResources.size(), static_cast<SizeT>(slot));
        return MGPipeApplier().TextureResources[slot];
    }
} // namespace
#endif

// set_texture_params IS ADDRESSED BY RESOURCE AND BY NOTHING ELSE, which is the whole reason
// the call exists: a texture that is only an FBO attachment, only an image-unit binding or
// only a glCopyImageSubData endpoint has no sampler view to hang its parameters on. Deleting
// the store or the ParamsSerial bump leaves this red.
TEST(TextureEmit, ATexturesParametersLandOnItsOwnRecordAndMoveOnlyTheirOwnSerial) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{7, 3};
    const MGPipeHandle sampler{2, 1};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 41));
    MGPipeApplyResourceRespecify(TextureDesc(texture, 64, 41), nullptr);
    ASSERT_EQ(TextureRecordOf(7).ParamsSerial, 0u) << "a create and a respecify are not a parameter push";

    MGPipeApplySetTextureParams(TextureParams(texture, sampler, 2));

    const MGPipeResourceRecord& record = TextureRecordOf(7);
    EXPECT_EQ(record.Params.BuiltinSampler, sampler);
    EXPECT_EQ(record.Params.BaseLevel, 2u);
    EXPECT_EQ(record.Params.MaxLevel, 7u);
    EXPECT_EQ(record.Params.Swizzle[2], 3u);
    EXPECT_EQ(record.Params.DepthStencilMode, 5u);
    EXPECT_EQ(record.Params.ForceResync, 1u);
    EXPECT_EQ(record.Params.SamplerResync, 1u) << "the second resync bit is carried, not dropped";
    EXPECT_FLOAT_EQ(record.Params.MinLod, -2.0f);
    EXPECT_FLOAT_EQ(record.Params.LodBias, 0.5f);
    EXPECT_EQ(record.ParamsSerial, 1u);
    EXPECT_EQ(record.Serial, 1u) << "a parameter push is not a storage mutation and must not move Serial";

    MGPipeApplySetTextureParams(TextureParams(texture, sampler, 3));
    EXPECT_EQ(TextureRecordOf(7).Params.BaseLevel, 3u);
    EXPECT_EQ(TextureRecordOf(7).ParamsSerial, 2u);

    // A stale generation resolves to nothing: the call is a DEFINED no-op and it is COUNTED,
    // because MOBILEGL_ASSERT compiles out at INFO and a no-op nobody can see is a dropped
    // parameter push nobody can see.
    const Uint64 refusedBefore = MGPipeApplier().RefusedObjectCalls;
    MGPipeApplySetTextureParams(TextureParams(MGPipeHandle{7, 4}, sampler, 6));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, refusedBefore + 1);
    EXPECT_EQ(TextureRecordOf(7).Params.BaseLevel, 3u) << "a stale handle wrote the live record";
    EXPECT_EQ(TextureRecordOf(7).ParamsSerial, 2u);

    // And a BUFFER of the same slot is not a texture: the two tables are independent, so this
    // is a refusal rather than a parameter push onto somebody else's record.
    MGPipeApplySetTextureParams(TextureParams(MGPipeHandle{9, 1}, sampler, 1));
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, refusedBefore + 2);
#endif
}

// EVERY ITextureObject OWNS A SamplerObject, so a null built-in sampler CSO is not "no
// sampler" - it is a record that would have the backend sample with whatever filter and wrap
// state the unit last left behind. It is the corrupt-record verdict rather than the dropped-
// call one, so it must NOT be counted as a refusal.
TEST(TextureEmit, ARecordWithNoBuiltinSamplerCsoIsRefusedNamingTheTexture) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{6, 2};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 77));
    MGPipeApplySetTextureParams(TextureParams(texture, MGPipeHandle{2, 1}, 1));
    ASSERT_EQ(TextureRecordOf(6).ParamsSerial, 1u);
    const Uint64 refusedBefore = MGPipeApplier().RefusedObjectCalls;

    const MGPTextureParams noSampler = TextureParams(texture, kMGPipeNullHandle, 4);
    ExpectRefusedNaming("set_texture_params {slot=6, gen=2, glName=77}: the record names no built-in "
                        "sampler CSO",
                        [&noSampler]() { MGPipeApplySetTextureParams(noSampler); });
    EXPECT_EQ(TextureRecordOf(6).Params.BaseLevel, 1u) << "a refused record was stored anyway";
    EXPECT_EQ(TextureRecordOf(6).ParamsSerial, 1u);
    EXPECT_EQ(MGPipeApplier().RefusedObjectCalls, refusedBefore)
        << "a corrupt record is not a dropped call and must not be counted as one";
#endif
}

// D-D5's safety net. The client clears its own dirty flags AT EMISSION and the backend's
// upload loop has bail arms that would otherwise lose exactly those texels, so the emitted
// shape accumulates SERVER-SIDE: boxes union, rect lists concatenate, and the moment either
// side says "box only" the entry becomes box only - which is the frontend's own model, where
// zero rects means "upload the union box instead" and covers every reason at once.
TEST(TextureEmit, AnAccumulatedUploadUnionsItsBoxesAndCollapsesToTheBoxWhenARectListCannotDescribeIt) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{4, 1};
    const Uint8 texels[4096] = {};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 88));
    MGPipeApplyResourceRespecify(TextureDesc(texture, 64, 88), nullptr);

    const MGPSubRegion first[2] = {Region(0, 0, 4, 4), Region(8, 8, 4, 4)};
    MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{0, 0, 0, 12, 12, 1}, 2), texels, first);
    ASSERT_EQ(TextureRecordOf(4).PendingUploads.size(), 1u);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].UploadTarget, kTex2D);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].Level, 0u);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].UnionBox.W, 12u);
    ASSERT_EQ(TextureRecordOf(4).PendingUploads[0].Regions.size(), 2u);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].Regions[1].X, 8);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].Regions[1].SrcRowStride, 256u)
        << "the strides are CARRIED, never inferred: the pointer comparison they replace cannot "
           "survive a split";
    EXPECT_EQ(TextureRecordOf(4).Serial, 2u) << "an accepted upload moves the record's serial";

    // A second emission behind a backend bail: the boxes union and the lists concatenate.
    const MGPSubRegion second[1] = {Region(16, 0, 8, 8)};
    MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{16, 0, 0, 8, 8, 1}, 1), texels, second);
    ASSERT_EQ(TextureRecordOf(4).PendingUploads.size(), 1u) << "the (target, level) key split in two";
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].UnionBox.X, 0);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].UnionBox.W, 24u);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].UnionBox.H, 12u);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].Regions.size(), 3u);

    // A contribution with NO regions means "the box is the whole story", and the accumulated
    // entry has to say the same thing afterwards or the box would cover texels the rect list
    // does not name.
    MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{0, 0, 0, 64, 64, 1}, 0), texels);
    ASSERT_EQ(TextureRecordOf(4).PendingUploads.size(), 1u);
    EXPECT_TRUE(TextureRecordOf(4).PendingUploads[0].Regions.empty())
        << "a box-only contribution left a rect list that no longer covers the box";
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].UnionBox.W, 64u);

    // A different level is a different key, and a different upload target would be too.
    MGPipeApplyResourceSubData(TextureUpload(texture, 3, MGPBox{0, 0, 0, 8, 8, 1}, 0), texels);
    ASSERT_EQ(TextureRecordOf(4).PendingUploads.size(), 2u);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[1].Level, 3u);
    EXPECT_EQ(TextureRecordOf(4).PendingUploads[0].UnionBox.W, 64u) << "level 3 rewrote level 0's box";
#endif
}

// The texture half of the sub-data validator. Each of its four statements is about a record
// that would make the server upload texels it was never told about, or read a tail it was not
// given; removing any one of them leaves this red.
TEST(TextureEmit, TheSubDataValidatorRefusesALevelABoxAndARegionTheRecordCannotDescribe) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{5, 2};
    const Uint8 texels[4096] = {};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 99));
    MGPipeApplyResourceRespecify(TextureDesc(texture, 64, 99), nullptr);

    // Positive control: the last legal level, a whole-level box and a region exactly filling
    // it are all fine, so what follows is refusing the value and not the arithmetic round it.
    const MGPSubRegion exact[1] = {Region(0, 0, 8, 8)};
    MGPipeApplyResourceSubData(TextureUpload(texture, 31, MGPBox{0, 0, 0, 8, 8, 1}, 1), texels, exact);
    ASSERT_EQ(TextureRecordOf(5).PendingUploads.size(), 1u);
    const Uint64 serialBefore = TextureRecordOf(5).Serial;

    const MGPSubData deepLevel = TextureUpload(texture, 32, MGPBox{0, 0, 0, 8, 8, 1}, 0);
    ExpectRefusedNaming("resource_subdata {slot=5, gen=2, glName=99}: the level is above the bound any "
                        "texture's storage can have",
                        [&deepLevel, &texels]() { MGPipeApplyResourceSubData(deepLevel, texels); });

    const MGPSubData negative = TextureUpload(texture, 0, MGPBox{-1, 0, 0, 8, 8, 1}, 0);
    ExpectRefusedNaming("resource_subdata {slot=5, gen=2, glName=99}: the union box has a negative origin "
                        "or runs past the bound one record can encode",
                        [&negative, &texels]() { MGPipeApplyResourceSubData(negative, texels); });

    const MGPSubData missingTail = TextureUpload(texture, 0, MGPBox{0, 0, 0, 8, 8, 1}, 2);
    ExpectRefusedNaming("resource_subdata {slot=5, gen=2, glName=99}: the record declares sub-regions and "
                        "carries none",
                        [&missingTail, &texels]() { MGPipeApplyResourceSubData(missingTail, texels); });

    // THE ONE INVARIANT THAT MATTERS: the union box IS the union of the regions. The server
    // picks the upload shape from the pair, so a region outside the box means the box misses
    // its texels and the region writes where the box never said it would.
    const MGPSubRegion outside[1] = {Region(16, 0, 4, 4)};
    const MGPSubData escapes = TextureUpload(texture, 0, MGPBox{0, 0, 0, 8, 8, 1}, 1);
    ExpectRefusedNaming("resource_subdata {slot=5, gen=2, glName=99}: a sub-region is not inside the union "
                        "box the record declares",
                        [&escapes, &texels, &outside]() {
                            MGPipeApplyResourceSubData(escapes, texels, outside);
                        });

    const MGPSubData nothing = TextureUpload(texture, 0, MGPBox{0, 0, 0, 0, 0, 0}, 0);
    ExpectRefusedNaming("resource_subdata {slot=5, gen=2, glName=99}: the record describes no texels at all",
                        [&nothing, &texels]() { MGPipeApplyResourceSubData(nothing, texels); });

    EXPECT_EQ(TextureRecordOf(5).PendingUploads.size(), 1u)
        << "a refused record was accumulated anyway";
    EXPECT_EQ(TextureRecordOf(5).Serial, serialBefore) << "not one refusal may move the serial";
#endif
}

// A respecify redefines the store, so the boxes and rects that describe the level it replaces
// go with it - a box kept across a shrink would have the backend upload past the end of the
// new level. Nothing is lost by it: the frontend entry points that respecify a texture re-mark
// the levels they define.
TEST(TextureEmit, ARespecifyDropsThePendingUploadsAgainstTheStorageItReplaces) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{3, 1};
    const Uint8 texels[4096] = {};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 55));
    MGPipeApplyResourceRespecify(TextureDesc(texture, 64, 55), nullptr);
    MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{0, 0, 0, 64, 64, 1}, 0), texels);
    ASSERT_EQ(TextureRecordOf(3).PendingUploads.size(), 1u);

    MGPipeApplyResourceRespecify(TextureDesc(texture, 8, 55), nullptr);
    EXPECT_TRUE(TextureRecordOf(3).PendingUploads.empty())
        << "a 64-wide box survived onto an 8-wide store";
    EXPECT_EQ(TextureRecordOf(3).Desc.Width, 8u);
#endif
}

// D-J4, for the kind that made the rule matter: a TEXTURE lives in a share group exactly as a
// buffer does, so its record - and the parameters and the pending uploads that ride on it -
// outlives a make-current, and only the applier's own teardown takes it.
TEST(TextureEmit, TheTextureRecordAndItsParamsAndPendingUploadsSurviveAMakeCurrent) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{2, 5};
    const Uint8 texels[4096] = {};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 66));
    MGPipeApplyResourceRespecify(TextureDesc(texture, 32, 66), nullptr);
    MGPipeApplySetTextureParams(TextureParams(texture, MGPipeHandle{1, 1}, 2));
    MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{0, 0, 0, 32, 32, 1}, 0), texels);

    MGPipeApplierReset(); // the make-current

    ASSERT_TRUE(TextureRecordOf(2).Live) << "a make-current dropped a share-group object's record";
    EXPECT_EQ(TextureRecordOf(2).Desc.Width, 32u);
    EXPECT_EQ(TextureRecordOf(2).Params.BaseLevel, 2u);
    EXPECT_EQ(TextureRecordOf(2).ParamsSerial, 1u);
    ASSERT_EQ(TextureRecordOf(2).PendingUploads.size(), 1u)
        << "the pending uploads are the safety net for a backend bail and cannot be per context";

    // The write that follows the switch still lands, which is the whole point of the rule.
    MGPipeApplyResourceSubData(TextureUpload(texture, 1, MGPBox{0, 0, 0, 16, 16, 1}, 0), texels);
    EXPECT_EQ(TextureRecordOf(2).PendingUploads.size(), 2u);
    EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 0u);

    // And the teardown scope - the only other thing that clears a record - does take it.
    MGPipeApplierReleaseObjectRecords();
    EXPECT_TRUE(MGPipeApplier().TextureResources.empty());
#endif
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-textureemit-test-" + std::to_string(ProcessId()) + ".log");
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
