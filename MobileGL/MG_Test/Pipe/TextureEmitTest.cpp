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
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Impl/Pipe/SamplerEmit.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/RenderbufferState/RenderbufferObject.h>
#include <MG_State/GLState/TextureState/TextureObject2D.h>
#include <MG_State/GLState/TextureState/TextureObjectView.h>

#include <algorithm>
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

// ============================================================================
// P4a package B's EMITTER-SIDE cases. The contract commit landed the file and its ctest
// registration and one shape pin; the wire package's applier-side cases and these are disjoint
// TEST bodies in one file, and a collision between them is resolved by UNION, never by
// choosing a side.
// ============================================================================
#if !MOBILEGL_PIPE_PUSH
#define MGL_TEXTURE_EMIT_CLIENT_TEST_LIST(X)                                                       \
    X(TextureEmit, EveryTextureTargetMapsToItsOwnResourceTarget)                                   \
    X(TextureEmit, EveryBindKindSetsItsBindMaskBit)                                                \
    X(TextureEmit, ABindMaskBitIsStickyAcrossARespecify)                                           \
    X(TextureEmit, AnImageBoundTextureCarriesTheImageBindableHintForever)                          \
    X(TextureEmit, TheUnionBoxAndTheRegionListDescribeTheSameTexels)                               \
    X(TextureEmit, AScatteredUploadCarriesTheLevelShadowsStridesAndNotZero)                        \
    X(TextureEmit, AWholeLevelUploadCarriesZeroStrides)                                            \
    X(TextureEmit, MoreThanKMaxDirtyRectsCollapsesToTheBoxWithRegionCountZero)                     \
    X(TextureEmit, AnUploadThroughAViewKeysOnTheStorageOwner)                                      \
    X(TextureEmit, EveryTexturesParamsNameItsBuiltinSamplerCso)                                    \
    X(TextureEmit, TwoTexturesWithIdenticalSamplingShareOneBuiltinCso)                             \
    X(TextureEmit, ADestroyedTextureReleasesItsResourceViewAndBuiltinSamplerSlots)                 \
    X(TextureEmit, ARenderbufferRespecifyPublishesItsExtentWithoutAVersionCounter)                 \
    X(TextureEmit, ABailedLevelStaysDirtyAndStaysOnTheDrainList)                                   \
    X(TextureEmit, TheApplierStoresTheRegionListTheEmitterBuiltAndNotAnEmptyOne)                   \
    X(TextureEmit, ARefusedUploadLeavesTheLevelDirtyAndOnTheDrainList)                             \
    X(TextureEmit, AnImmutableTexturesImageBindableHintReachesTheApplierAfterItsAllocation)        \
    X(TextureEmit, ALodWriteOnTheBuiltinSamplerRepublishesTheParams)                               \
    X(TextureEmit, ATexturesBuiltinSamplerHoldsOneCacheReferenceAndSwapsItWithTheContent)          \
    X(TextureEmit, ARecycledTextureSlotDoesNotInheritItsPredecessorsBindMask)                      \
    X(TextureEmit, ALevelMarkedCleanIsCollectedAtTheNextDrain)                                     \
    X(TextureEmit, WithNoBackendConsumerTheFamilyGateIsFalseAndNothingReachesTheApplier)           \
    X(TextureEmit, WithTheSamplerBitClearTheTextureFamilyGateIsFalseAndNothingReachesTheApplier)   \
    X(TextureEmit,                                                                                 \
      WithTheBufferResourceBitClearTheTextureFamilyGateIsFalseAndNothingReachesTheApplier)         \
    X(TextureEmit, EveryDKTwoDependencyRowGatesItsOwnFamilyAndTheMirrorPairsStayLive)              \
    X(TextureEmit, ALevelDefinedAfterAnEmittedButUnconsumedUploadKeepsThatUpload)                  \
    X(TextureEmit, AChainTruncationKeepsTheSurvivingLevelsPendingUploads)                          \
    X(TextureEmit, ARedefinitionOfANonBaseLevelAtANewSizeDropsOnlyThatLevelsPendingUpload)         \
    X(TextureEmit, ADeadTexturesHandleResolvesToNothingAndLeavesTheDrainList)                      \
    X(TextureEmit, ATextureRecycledOntoADeadSlotDoesNotInheritTheDrainEntry)                       \
    X(TextureEmit, ADeadRenderbuffersEntryIsRetiredWithItsSlot)                                    \
    X(TextureEmit, ARefusedParamsRecordDoesNotAdvanceTheLatch)                                     \
    X(TextureEmit, ADeadTexturesSamplerViewLatchIsRetiredAtItsDeath)

#define MGL_DECLARE_PULL_SKIP(Suite, Name)                                                         \
    TEST(Suite, Name) { GTEST_SKIP() << "compiled only under MOBILEGL_PIPE_PUSH"; }
MGL_TEXTURE_EMIT_CLIENT_TEST_LIST(MGL_DECLARE_PULL_SKIP)
#undef MGL_DECLARE_PULL_SKIP
#else
namespace {
    using GLContext = MG_State::GLState::GLContext;
    using MG_State::GLState::MipmapDirtyRegion;
    using MG_State::GLState::MipmapInput;
    using MG_State::GLState::MipmapStorage;
    using MG_State::GLState::RenderbufferObject;
    using MG_State::GLState::TextureObject2D;
    using MG_State::GLState::TextureObjectView;

    // AN RAII SCOPE RATHER THAN A gtest FIXTURE, for VertexInputEmitTest's reason: both gates
    // grep `ctest -R 'TextureEmit\.'`, a TEST_F files its cases under the FIXTURE's name, and
    // gtest refuses to mix TEST and TEST_F under one suite name - so a fixture would rename
    // every case out of the gate's reach.
    //
    // It ARMS THE SUBSYSTEM BIT, which a unit binary otherwise has cleared:
    // MG_Config::Features.PipePush defaults to 0 and only ConfigLoader ever sets the phase mask,
    // so without this every emission below would be correctly skipped and every assertion would
    // be green for the wrong reason.
    struct TextureScope {
        TextureScope() {
            m_previousPush = MG_Config::Features.PipePush;
            // D-K2's WHOLE ROW FOR BIT 10, in the fixture, because the client now enforces it
            // (S-3 / ID-41) and not only Espryt's ResolveTextureResourceSubsystemArm:
            //   - BIT 10 REQUIRES BIT 11 - MGPTextureParams::BuiltinSampler is a SamplerCso
            //     handle out of the sampler family's content-addressed cache and a null there is
            //     Fatal{ProtocolCorruption};
            //   - BIT 10 REQUIRES BIT 7 - a buffer texture's BufferForTexBuffer names a Buffer
            //     handle and only bit 7 puts twins in the resource slot table (D-D1). Arming it
            //     changes nothing else here: this file constructs no BufferObject, so no P3a
            //     resource hook has anything to fire on.
            // Without all three the family gate is FALSE and every case below would be green for
            // the wrong reason - which is what the dependency cases at the end of this file pin.
            MG_Config::Features.PipePush |= kMGPipeSubsystemResources |
                                            kMGPipeSubsystemTextureResources |
                                            kMGPipeSubsystemSamplers;
            m_previousContext = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
            MGPipeTextureEmitterInstance().ResetForTest();
            // AND THE APPLIER IS A PROCESS SINGLETON, so its object records outlive a case. With
            // kMGPipeWiredTextureSubsystem flipped the emitter's calls actually LAND, and a case
            // that inherited the previous one's records would assert against state it did not
            // create - and, worse, a case that wants to prove a REFUSAL could not construct one.
            // v1 armed the emitter here instead (ArmForTest), which is now gone: the constant is
            // its own bit, PipeFill.cpp's gate never consulted that latch, and MG_Config's bit is
            // the switch the shipped build has.
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
        }
        ~TextureScope() {
            MGPipeTextureEmitterInstance().ResetForTest();
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
            MG_State::pGLContext.reset();
            MG_State::pGLContext = Move(m_previousContext);
            MG_Config::Features.PipePush = m_previousPush;
        }
        TextureScope(const TextureScope&) = delete;
        TextureScope& operator=(const TextureScope&) = delete;

        UniquePtr<GLContext> m_previousContext;
        Uint64 m_previousPush = 0;
    };

    // ID-39's OTHER ARM, scoped. This binary registers an (empty) MGPipeResourceOps table in
    // main() because the whole suite is about the emitter and the applier as they behave under
    // a backend that CONSUMES what they publish - DirectGLES, which registers the table at its
    // own bring-up. The one case that is about a backend with no consumer takes the table away
    // for its own duration and puts it back.
    struct ScopedNoResourceOps {
        ScopedNoResourceOps() : m_saved(MGPipeGetResourceOps()) { MGPipeSetResourceOps(nullptr); }
        ~ScopedNoResourceOps() { MGPipeSetResourceOps(m_saved); }
        ScopedNoResourceOps(const ScopedNoResourceOps&) = delete;
        ScopedNoResourceOps& operator=(const ScopedNoResourceOps&) = delete;

        const MGPipeResourceOps* m_saved;
    };

    MGPipeTextureEmitter& Textures() { return MGPipeTextureEmitterInstance(); }
    GLContext& Ctx() { return *MG_State::pGLContext; }

    // The APPLIER's own view of what this emitter sent. With the family's wired constant flipped
    // the calls actually land, so a case can read the record rather than the emitter's staging
    // copy - which is the whole of M1 (v1's four region cases all read LastRegions(), the
    // emitter's own vector, and could not see that the tail was never passed).
    const MGPipeResourceRecord* AppliedTexture(MGPipeHandle handle) {
        const SizeT slot = handle.Slot;
        if (slot >= MGPipeApplier().TextureResources.size()) return nullptr;
        const MGPipeResourceRecord& record = MGPipeApplier().TextureResources[slot];
        if (!record.Live || record.Gen != handle.Gen) return nullptr;
        return &record;
    }

    const MGPipeResourceRecord::PendingUpload* AppliedUpload(const MGPipeResourceRecord& record,
                                                             Uint16 target, Uint16 level) {
        for (const auto& pending : record.PendingUploads) {
            if (pending.UploadTarget == target && pending.Level == level) return &pending;
        }
        return nullptr;
    }

    // A 2D texture with `levels` levels, each RGBA8 and each half the previous one, so the
    // bytes-per-texel the emitter derives from (byteSize / texelCount) is exactly 4 and every
    // stride assertion below is an exact number rather than a range.
    SharedPtr<TextureObject2D> MakeTexture2D(Uint name, Int size, Uint levels = 1) {
        auto texture = MakeShared<TextureObject2D>(name);
        texture->SetInternalFormat(TextureInternalFormat::RGBA8);
        for (Uint level = 0; level < levels; ++level) {
            const Int extent = std::max<Int>(size >> level, 1);
            texture->AllocateStorage(TextureUploadTarget::Texture2D, level,
                                     MipmapInput{IntVec3{extent, extent, 1},
                                                 static_cast<SizeT>(extent) * static_cast<SizeT>(extent) * 4});
        }
        return texture;
    }
} // namespace

// ============================ D-A3 ============================
//
// The exhaustiveness the contract's static_assert already pins, walked at RUNTIME over every
// enumerator - because the assert answers "is every target mapped" and this answers the
// stronger "does every target map to its OWN row". Folding rectangle onto 2D is the one
// collapse anybody would be tempted by, and it is a distinction the frontend keeps and both
// backends switch on.
TEST(TextureEmit, EveryTextureTargetMapsToItsOwnResourceTarget) {
    Vector<Uint32> seen;
    for (Int i = 0; i < static_cast<Int>(TextureTarget::TextureTargetCount); ++i) {
        const auto target = static_cast<TextureTarget>(i);
        const Uint32 resourceTarget = MGPipeResourceTargetForTextureTarget(target);
        EXPECT_NE(resourceTarget, kMGPipeResourceTargetUnmapped)
            << "TextureTarget " << i << " has no MGPResourceDesc::Target row";
        EXPECT_NE(resourceTarget, static_cast<Uint32>(MGPipeResourceTarget::Buffer))
            << "TextureTarget " << i << " maps onto the BUFFER row, which the ack predicate reads";
        EXPECT_NE(resourceTarget, static_cast<Uint32>(MGPipeResourceTarget::Renderbuffer))
            << "TextureTarget " << i << " maps onto the RENDERBUFFER row";
        for (const Uint32 previous : seen) {
            EXPECT_NE(previous, resourceTarget)
                << "TextureTarget " << i << " shares its resource target with an earlier one";
        }
        seen.push_back(resourceTarget);
    }
    EXPECT_EQ(seen.size(), static_cast<SizeT>(TextureTarget::TextureTargetCount));
}

// ============================ D-A4 ============================
TEST(TextureEmit, EveryBindKindSetsItsBindMaskBit) {
    TextureScope scope;
    const auto texture = MakeTexture2D(1, 8);
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    ASSERT_FALSE(MGPipeHandleIsNull(handle));

    const Uint16 bits[] = {kMGPipeBindSampler, kMGPipeBindShaderImage, kMGPipeBindRenderTarget,
                           kMGPipeBindDepthStencil};
    Uint16 expected = 0;
    for (const Uint16 bit : bits) {
        Textures().NoteTextureBoundAs(handle, bit);
        expected = static_cast<Uint16>(expected | bit);
        EXPECT_EQ(Textures().TextureBindMask(handle), expected) << "bind bit " << bit;
    }
    // ORed, never cleared: re-noting one bit cannot drop the others.
    Textures().NoteTextureBoundAs(handle, kMGPipeBindSampler);
    EXPECT_EQ(Textures().TextureBindMask(handle), expected);
}

TEST(TextureEmit, ABindMaskBitIsStickyAcrossARespecify) {
    TextureScope scope;
    const auto texture = MakeTexture2D(2, 8);
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    Textures().NoteTextureBoundAs(handle, kMGPipeBindRenderTarget);

    // A storage definition REPUBLISHES the mask; it does not rebuild it.
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 0,
                             MipmapInput{IntVec3{16, 16, 1}, 16 * 16 * 4});
    EXPECT_TRUE(Textures().LastDesc().Resource == handle);
    EXPECT_NE(Textures().LastDesc().BindMask & kMGPipeBindRenderTarget, 0)
        << "MGPResourceDesc::BindMask lost the RENDER_TARGET bit across a respecify";
    EXPECT_EQ(Textures().LastDesc().Width, 16u);
}

TEST(TextureEmit, AnImageBoundTextureCarriesTheImageBindableHintForever) {
    TextureScope scope;
    const auto texture = MakeTexture2D(3, 8);
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    EXPECT_EQ(Textures().LastDesc().ImageBindableHint, 0);

    Textures().NoteTextureBoundAs(handle, kMGPipeBindShaderImage);
    // The next respecify carries the hint - and it is the PREVENTION half of the texture-remint
    // stall class, so it must never go back to 0 afterwards.
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 1, MipmapInput{IntVec3{4, 4, 1}, 4 * 4 * 4});
    EXPECT_EQ(Textures().LastDesc().ImageBindableHint, 1);
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 2, MipmapInput{IntVec3{2, 2, 1}, 2 * 2 * 4});
    EXPECT_EQ(Textures().LastDesc().ImageBindableHint, 1);

    // AND THE ORDER THE OTHER WAY ROUND, WHICH IS THE ORDER THE BUG WAS IN (M4). v1's case only
    // ever bound BEFORE allocating - the one order in which the mask reaches the wire on the
    // next respecify - so reversing the two statements turned it red. An allocation followed by
    // a bind is the CANONICAL order and the only one an immutable texture has.
    const auto later = MakeTexture2D(31, 8);
    const MGPipeHandle laterHandle = Textures().FindTexture(*later);
    later->AllocateStorage(TextureUploadTarget::Texture2D, 0, MipmapInput{IntVec3{8, 8, 1}, 8 * 8 * 4});
    ASSERT_EQ(Textures().LastDesc().ImageBindableHint, 0);
    const Uint64 respecifiesBefore = Textures().RespecifyCount();
    Textures().NoteTextureBoundAs(laterHandle, kMGPipeBindShaderImage);
    EXPECT_GT(Textures().RespecifyCount(), respecifiesBefore)
        << "a mask change after the allocation emitted nothing, so the hint can never reach the "
           "server for a texture that has no further respecify";
    EXPECT_EQ(Textures().LastDesc().ImageBindableHint, 1);
    EXPECT_NE(Textures().LastDesc().BindMask & kMGPipeBindShaderImage, 0);
    // A SECOND note of the same bit moves nothing: the mask did not change, so there is no
    // metadata update to send.
    const Uint64 afterFirst = Textures().RespecifyCount();
    Textures().NoteTextureBoundAs(laterHandle, kMGPipeBindShaderImage);
    EXPECT_EQ(Textures().RespecifyCount(), afterFirst);

    // And the transition armed the ONE resync the client is allowed to ask for (D-E2): the
    // widened-channel carrier needs a swizzle override the frontend params version never moves
    // for. It is one-shot - the server clears its own copy, the client never clears a server
    // flag, and the client must not keep asking.
    texture->SetSwizzleParam(TextureSwizzleParam::Red, TextureSwizzleParam::Blue);
    EXPECT_EQ(Textures().LastParams().ForceResync, 1);
    texture->SetSwizzleParam(TextureSwizzleParam::Green, TextureSwizzleParam::Blue);
    EXPECT_EQ(Textures().LastParams().ForceResync, 0);
}

// ============================ D-D3 / D-D6 ============================
//
// THE INVARIANT THAT MAKES THE SERVER'S CHOICE SAFE, and nothing else in the tree can see it:
// SSIM is completely blind to whether the server uploaded one union box or N rects, and the
// Mali cliff behind that choice is ~+6 ms/frame for a hundred one-rect jobs against one box. So
// the two representations have to describe the SAME texels - every rect inside the box, and
// their union exactly the box.
TEST(TextureEmit, TheUnionBoxAndTheRegionListDescribeTheSameTexels) {
    TextureScope scope;
    const auto texture = MakeTexture2D(4, 256);
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{1, 1, 1});
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{255, 255, 0},
                                    IntVec3{1, 1, 1});
    ASSERT_EQ(Textures().DrainListSize(), 1u);

    Textures().DrainTextureSubData(Ctx());
    const MGPSubData record = Textures().LastSubData();
    const Vector<MGPSubRegion> regions = Textures().LastRegions();
    ASSERT_EQ(record.RegionCount, regions.size());
    ASSERT_GE(record.RegionCount, 2u) << "two far-apart one-texel writes must survive as two rects";

    Int32 loX = record.UnionBox.X + static_cast<Int32>(record.UnionBox.W);
    Int32 loY = record.UnionBox.Y + static_cast<Int32>(record.UnionBox.H);
    Int32 hiX = record.UnionBox.X;
    Int32 hiY = record.UnionBox.Y;
    for (const MGPSubRegion& region : regions) {
        EXPECT_GE(region.X, record.UnionBox.X) << "a rect starts left of the union box";
        EXPECT_GE(region.Y, record.UnionBox.Y) << "a rect starts above the union box";
        EXPECT_LE(region.X + static_cast<Int32>(region.W),
                  record.UnionBox.X + static_cast<Int32>(record.UnionBox.W))
            << "a rect ends right of the union box";
        EXPECT_LE(region.Y + static_cast<Int32>(region.H),
                  record.UnionBox.Y + static_cast<Int32>(record.UnionBox.H))
            << "a rect ends below the union box";
        loX = std::min(loX, region.X);
        loY = std::min(loY, region.Y);
        hiX = std::max(hiX, region.X + static_cast<Int32>(region.W));
        hiY = std::max(hiY, region.Y + static_cast<Int32>(region.H));
    }
    EXPECT_EQ(loX, record.UnionBox.X) << "the rects' union does not reach the box's left edge";
    EXPECT_EQ(loY, record.UnionBox.Y) << "the rects' union does not reach the box's top edge";
    EXPECT_EQ(hiX, record.UnionBox.X + static_cast<Int32>(record.UnionBox.W));
    EXPECT_EQ(hiY, record.UnionBox.Y + static_cast<Int32>(record.UnionBox.H));
}

TEST(TextureEmit, AScatteredUploadCarriesTheLevelShadowsStridesAndNotZero) {
    TextureScope scope;
    const auto texture = MakeTexture2D(5, 256);
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{4, 8, 0}, IntVec3{2, 2, 1});
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{200, 220, 0},
                                    IntVec3{2, 2, 1});
    Textures().DrainTextureSubData(Ctx());
    const Vector<MGPSubRegion> regions = Textures().LastRegions();
    ASSERT_GE(regions.size(), 2u);
    // A SUB-RECT'S ROWS ARE NOT CONTIGUOUS IN THE SHADOW, so it must carry the LEVEL's pitches -
    // not its own width - or the staging planner on the far side repacks the wrong bytes.
    for (const MGPSubRegion& region : regions) {
        EXPECT_EQ(region.SrcRowStride, 256u * 4u) << "SrcRowStride is not the LEVEL's row pitch";
        EXPECT_EQ(region.SrcSliceStride, 256u * 4u * 256u)
            << "SrcSliceStride is not the LEVEL's slice pitch";
        const Uint64 expectedOffset =
            static_cast<Uint64>(region.Y) * 256u * 4u + static_cast<Uint64>(region.X) * 4u;
        EXPECT_EQ(region.SrcOffset, expectedOffset) << "SrcOffset is not the first texel's byte offset";
    }
}

TEST(TextureEmit, AWholeLevelUploadCarriesZeroStrides) {
    TextureScope scope;
    const auto texture = MakeTexture2D(6, 64);
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0},
                                    IntVec3{64, 64, 1});
    Textures().DrainTextureSubData(Ctx());
    const MGPSubData record = Textures().LastSubData();
    EXPECT_EQ(record.RegionCount, 0u) << "a single whole-level rect IS the union box, not a list";
    EXPECT_EQ(record.UnionBox.X, 0);
    EXPECT_EQ(record.UnionBox.Y, 0);
    EXPECT_EQ(record.UnionBox.W, 64u);
    EXPECT_EQ(record.UnionBox.H, 64u);
    EXPECT_EQ(record.SourceIsVerbatimLevelShadow, 1)
        << "the client always declares the level shadow; the server clears it when it converts";
    EXPECT_EQ(record.Blob.Seg, kMGHostSpanSegNone);
    EXPECT_EQ(record.Blob.Size, 0u) << "a monolith record does not declare its blob";
    EXPECT_NE(record.Blob.Offset, 0u) << "Blob.Offset is the level shadow's address in monolith";
    // The upload target rides in the record's Target byte beside the resource target, which is
    // the only place a cube face could ever be carried.
    EXPECT_EQ(MGPipeSubDataResourceTargetOf(record.Target),
              static_cast<Uint8>(MGPipeResourceTarget::Tex2D));
    EXPECT_EQ(MGPipeSubDataUploadTargetOf(record.Target),
              static_cast<Uint8>(TextureUploadTarget::Texture2D));
    // And the whole-level builder really does say TIGHTLY PACKED.
    const MGPipeLevelPitch pitch = MGPipeLevelPitchOf(IntVec3{64, 64, 1}, 64 * 64 * 4);
    const MGPSubRegion whole = MGPipeBuildSubRegion(record.UnionBox, IntVec3{64, 64, 1}, pitch);
    EXPECT_EQ(whole.SrcRowStride, 0u);
    EXPECT_EQ(whole.SrcSliceStride, 0u);
    EXPECT_EQ(whole.SrcOffset, 0u);
}

TEST(TextureEmit, MoreThanKMaxDirtyRectsCollapsesToTheBoxWithRegionCountZero) {
    TextureScope scope;
    const auto texture = MakeTexture2D(7, 128);
    // Far more writes than the rect cap. The storage MERGES rather than truncates - a dropped
    // rect is a dropped write - and degrades toward the union box; what the emitter must never
    // do is invent or truncate. So the storage's own answer is the oracle, read BEFORE the drain
    // clears the level, and 0 means "the union box is the whole story".
    for (Int i = 0; i < 4 * static_cast<Int>(MipmapStorage::kMaxDirtyRects); ++i) {
        const Int x = (i * 7) % 120;
        const Int y = (i * 11) % 120;
        texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{x, y, 0},
                                        IntVec3{2, 2, 1});
    }
    MipmapDirtyRegion oracle[MipmapStorage::kMaxDirtyRects];
    const SizeT oracleCount = texture->GetStorageDirtyRects(TextureUploadTarget::Texture2D, 0, oracle,
                                                            MipmapStorage::kMaxDirtyRects);
    EXPECT_LE(oracleCount, MipmapStorage::kMaxDirtyRects);
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().LastSubData().RegionCount, static_cast<Uint32>(oracleCount))
        << "the emitted region count is not the storage's own answer";
    EXPECT_EQ(Textures().LastRegions().size(), oracleCount);
}

// ============================ D-D4 ============================
TEST(TextureEmit, AnUploadThroughAViewKeysOnTheStorageOwner) {
    TextureScope scope;
    const auto owner = MakeTexture2D(8, 64, 3);
    owner->SetImmutableLevels(3);
    const MGPipeHandle ownerHandle = Textures().FindTexture(*owner);
    const auto view = MakeShared<TextureObjectView>(9, TextureTarget::Texture2D, owner, 1, 2, 0, 1);
    const MGPipeHandle viewHandle = Textures().FindTexture(*view);
    ASSERT_FALSE(MGPipeHandleIsNull(ownerHandle));
    ASSERT_FALSE(MGPipeHandleIsNull(viewHandle));
    ASSERT_FALSE(ownerHandle == viewHandle);
    // ViewOf names the storage owner, and ONE HOP always reaches storage.
    EXPECT_TRUE(Textures().LastDesc().Resource == viewHandle);
    EXPECT_TRUE(Textures().LastDesc().ViewOf == ownerHandle)
        << "the view's descriptor does not name its storage owner";

    // An upload through the VIEW: TextureObjectView forwards the mark to the OWNER's method
    // after remapping the level, so the drain list holds exactly one entry and it is the
    // owner's - which is what makes an upload through a view and an upload through the owner one
    // key rather than two.
    view->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{4, 4, 1});
    ASSERT_EQ(Textures().DrainListSize(), 1u);
    Textures().DrainTextureSubData(Ctx());
    EXPECT_TRUE(Textures().LastSubData().Res == ownerHandle)
        << "an upload through a view was keyed on the view instead of on its storage owner";
    // The view's level 0 IS the owner's level 1.
    EXPECT_EQ(Textures().LastSubData().Level, 1u);
}

// ============================ D-E1 ============================
TEST(TextureEmit, EveryTexturesParamsNameItsBuiltinSamplerCso) {
    TextureScope scope;
    const auto texture = MakeTexture2D(10, 8);
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    texture->SetSwizzleParamRGBA(Vec4<TextureSwizzleParam>{TextureSwizzleParam::Alpha,
                                                           TextureSwizzleParam::Blue,
                                                           TextureSwizzleParam::Green,
                                                           TextureSwizzleParam::Red});
    const MGPTextureParams params = Textures().LastParams();
    EXPECT_TRUE(params.Res == handle);
    // kMGPipeNullHandle is ILLEGAL here: every texture object owns a sampler object, so a null
    // is Fatal{ProtocolCorruption} on the far side rather than "no sampler".
    EXPECT_FALSE(MGPipeHandleIsNull(params.BuiltinSampler))
        << "MGPTextureParams::BuiltinSampler must never be the null handle";
    // IT COMES FROM THE SAMPLER FAMILY'S CONTENT-ADDRESSED CACHE (ID-14), not from a slot this
    // package mints. v1 took MGPipeSlots().Acquire(SamplerCso, the SamplerObject's lifetime id),
    // which no create_sampler_state ever names - so on the integrated tree the applier would hold
    // nothing for the handle every texture's params record carries. A content-addressed CSO has
    // NO lifetime-id mapping at all, so FindByLifetimeId is the wrong question to ask about it.
    EXPECT_TRUE(MGPipeSamplerCsoCacheInstance().RecordIsPublished(params.BuiltinSampler))
        << "MGPTextureParams::BuiltinSampler names a handle the sampler CSO cache never minted a "
           "create_sampler_state for";
    EXPECT_GE(MGPipeSamplerCsoCacheInstance().RefCountOf(params.BuiltinSampler), 1u)
        << "the texture holds no reference on its built-in sampler, so an LRU eviction could take "
           "the handle out from under a standing set_texture_params record";
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeSlots().FindByLifetimeId(
        MGPipeKind::SamplerCso, texture->GetSamplerObject()->GetLifetimeId())))
        << "a SamplerCso slot was minted against the SamplerObject's lifetime id; the cache "
           "deliberately allocates without one, so ~SamplerObject frees nothing for this kind";
    EXPECT_EQ(params.Swizzle[0], static_cast<Uint8>(TextureSwizzleParam::Alpha));
    EXPECT_EQ(params.Swizzle[1], static_cast<Uint8>(TextureSwizzleParam::Blue));
    EXPECT_EQ(params.Swizzle[2], static_cast<Uint8>(TextureSwizzleParam::Green));
    EXPECT_EQ(params.Swizzle[3], static_cast<Uint8>(TextureSwizzleParam::Red));
    EXPECT_EQ(params.DepthStencilMode, kMGPipeDepthStencilModeDepth);
    // D-E3's deliverable at the one site that proves it: a texture nothing has bound - no
    // sampler view, no image unit, only ever an attachment - still publishes the mode, because
    // set_texture_params is addressed by RESOURCE and is independent of every binding.
    texture->SetDepthStencilTextureMode(GL_STENCIL_INDEX);
    EXPECT_EQ(Textures().LastParams().DepthStencilMode, kMGPipeDepthStencilModeStencil);
}

TEST(TextureEmit, TwoTexturesWithIdenticalSamplingShareOneBuiltinCso) {
    TextureScope scope;
    static_assert(kMGPipeWiredSamplerSubsystem != 0,
                  "ID-14: this package takes MGPTextureParams::BuiltinSampler from the sampler "
                  "family's content-addressed cache, so bit 10 requires bit 11 and this case is "
                  "no longer allowed to skip");
    const auto first = MakeTexture2D(11, 8);
    const auto second = MakeTexture2D(12, 8);
    first->SetSwizzleParam(TextureSwizzleParam::Red, TextureSwizzleParam::Green);
    const MGPipeHandle firstCso = Textures().LastParams().BuiltinSampler;
    second->SetSwizzleParam(TextureSwizzleParam::Red, TextureSwizzleParam::Green);
    EXPECT_TRUE(Textures().LastParams().BuiltinSampler == firstCso);
}

// ============================ D-I1 ============================
TEST(TextureEmit, ADestroyedTextureReleasesItsResourceViewAndBuiltinSamplerSlots) {
    TextureScope scope;
    const Uint32 texturesBefore = MGPipeSlots().LiveCount(MGPipeKind::Texture);
    const Uint32 viewsBefore = MGPipeSlots().LiveCount(MGPipeKind::SamplerViewCso);
    MGPipeHandle handle{};
    {
        const auto texture = MakeTexture2D(13, 8);
        handle = Textures().FindTexture(*texture);
        ASSERT_FALSE(MGPipeHandleIsNull(handle));
        EXPECT_TRUE(MGPipeSlots().IsLive(MGPipeKind::Texture, handle));
        // The sampler VIEW is minted off the TEXTURE's own lifetime id - one per ITextureObject
        // (D-F2) - so the texture's death is the only thing that can release it.
        (void)MGPipeSlots().Acquire(MGPipeKind::SamplerViewCso, texture->GetLifetimeId());
    }
    EXPECT_FALSE(MGPipeSlots().IsLive(MGPipeKind::Texture, handle))
        << "~TextureObjectBase did not return the texture's slot";
    EXPECT_EQ(MGPipeSlots().LiveCount(MGPipeKind::Texture), texturesBefore)
        << "a destroyed texture leaked its resource slot";
    EXPECT_EQ(MGPipeSlots().LiveCount(MGPipeKind::SamplerViewCso), viewsBefore)
        << "a destroyed texture leaked the sampler view minted off its lifetime id";
    // THE BUILT-IN SAMPLER'S CSO SLOT IS NOT CHECKED HERE, and the absence is a statement rather
    // than an omission (ID-14/ID-17): a content-addressed CSO belongs to a VALUE and not to this
    // object - two identical SamplerObjects share one - so it is allocated with NO lifetime id,
    // ~SamplerObject's helper correctly frees nothing for it, and the only death path for that
    // slot is the cache's own LRU eviction. What this package owes is the REFERENCE, and it is
    // dropped when the texture's slot is recycled (the one moment a client emitter can see a
    // texture die, the death helper being the contract's).
    //
    // A second release on the same handle is a proven no-op: Free bumps no generation of its
    // own, so a double free cannot skip one.
    MGPipeSlots().Free(MGPipeKind::Texture, handle);
    EXPECT_EQ(MGPipeSlots().LiveCount(MGPipeKind::Texture), texturesBefore);
}

// ============================ D-D2 ============================
TEST(TextureEmit, ARenderbufferRespecifyPublishesItsExtentWithoutAVersionCounter) {
    TextureScope scope;
    const auto renderbuffer = MakeShared<RenderbufferObject>(1);
    const MGPipeHandle handle = Textures().FindRenderbuffer(*renderbuffer);
    ASSERT_FALSE(MGPipeHandleIsNull(handle));
    EXPECT_TRUE(Textures().LastDesc().Resource == handle);
    EXPECT_EQ(Textures().LastDesc().Target, static_cast<Uint8>(MGPipeResourceTarget::Renderbuffer));
    EXPECT_EQ(Textures().LastDesc().HasDefinedContent, 0) << "a create carries no storage";

    // The three setters bump no version and raise no notice, and the framebuffer bit's shutter
    // does not move for an ALREADY-ATTACHED renderbuffer - which is exactly why the hole is
    // closed by EMISSION from the storage entry point rather than by a new counter (a member
    // would resize the pull build's object) or a wider shutter.
    renderbuffer->SetInternalFormat(TextureInternalFormat::Depth24Stencil8);
    renderbuffer->AllocateStorage(IntVec2{320, 240});
    renderbuffer->SetSamples(4);
    const MGPResourceDesc desc = Textures().LastDesc();
    EXPECT_TRUE(desc.Resource == handle);
    EXPECT_EQ(desc.Width, 320u);
    EXPECT_EQ(desc.Height, 240u);
    EXPECT_EQ(desc.Samples, 4u);
    EXPECT_EQ(desc.HasDefinedContent, 1);
    EXPECT_EQ(desc.InternalFormat, static_cast<Uint32>(TextureInternalFormat::Depth24Stencil8));

    // THE DEDUPE, which is what keeps one glRenderbufferStorage one record rather than three.
    const Uint64 respecifiesBefore = Textures().RespecifyCount();
    renderbuffer->AllocateStorage(IntVec2{320, 240});
    EXPECT_EQ(Textures().RespecifyCount(), respecifiesBefore);
}

// ============================ D-D5 ============================
TEST(TextureEmit, ABailedLevelStaysDirtyAndStaysOnTheDrainList) {
    TextureScope scope;
    // A level with no storage at all: there is nothing to upload, the record cannot be built,
    // and the texels are still owed. Naively clearing the flag here is exactly how a bail loses
    // texels, which is the failure D-D5's three steps exist to make impossible.
    // A level with a real EXTENT and no BYTES: the region is non-empty so the level is genuinely
    // dirty, and the emitter cannot derive a bytes-per-texel or find a shadow to point at.
    const auto texture = MakeShared<TextureObject2D>(14);
    texture->SetInternalFormat(TextureInternalFormat::RGBA8);
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 0, MipmapInput{IntVec3{8, 8, 1}, 0});
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{4, 4, 1});
    ASSERT_TRUE(texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0));
    ASSERT_EQ(Textures().DrainListSize(), 1u);
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().DrainListSize(), 1u)
        << "a level the emitter could not describe was dropped from the drain list";
    EXPECT_EQ(Textures().SubDataCount(), 0u);

    // And a level whose record the applier ACCEPTED leaves both the flag and the list entry
    // behind it. The acceptance is the applier's answer and not the emitter's - see
    // ARefusedUploadLeavesTheLevelDirtyAndOnTheDrainList for the other half, which is the one
    // v1 could not distinguish.
    const auto good = MakeTexture2D(15, 16);
    good->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{4, 4, 1});
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), 1u);
    EXPECT_EQ(Textures().RefusedSubDataCount(), 0u) << "the applier refused a record it holds";
    EXPECT_FALSE(good->IsStorageDirty(TextureUploadTarget::Texture2D, 0))
        << "the client must clear its own flag for a level whose record the applier accepted";
}

// ============================ M1 ============================
//
// THE REGION LIST IS BUILT, COUNTED AND HANDED OVER. v1 filled m_regions, wrote its size into
// MGPSubData::RegionCount and passed NOTHING - and every case that touched regions read the
// emitter's own staging vector back, so the omission was invisible to the whole suite. On this
// base the applier's tail exists, so the oracle is what the APPLIER stored: a record declaring
// N regions with a null tail is refused outright, and one whose rects the applier never saw
// would let Espryt fall back to the union box and silently discard D-D6's entire measured
// argument (a 635 KB/frame box against 40 KB/frame of rects).
TEST(TextureEmit, TheApplierStoresTheRegionListTheEmitterBuiltAndNotAnEmptyOne) {
    TextureScope scope;
    const auto texture = MakeTexture2D(16, 64);
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    // Two FAR-APART writes, which is the scattered case D-D5 names by fixture (atlas traffic):
    // the storage keeps two rects and their union box is most of the level.
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{4, 4, 1});
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{48, 48, 0}, IntVec3{4, 4, 1});
    Textures().DrainTextureSubData(Ctx());
    ASSERT_EQ(Textures().RefusedSubDataCount(), 0u)
        << "the applier refused the record, which is what a declared-but-missing tail looks like";
    const Vector<MGPSubRegion>& emitted = Textures().LastRegions();
    ASSERT_EQ(Textures().LastSubData().RegionCount, static_cast<Uint32>(emitted.size()));
    ASSERT_GE(emitted.size(), 2u) << "the storage merged the two writes; the case proves nothing";

    const MGPipeResourceRecord* stored = AppliedTexture(handle);
    ASSERT_NE(stored, nullptr);
    const MGPipeResourceRecord::PendingUpload* pending =
        AppliedUpload(*stored, Textures().LastSubData().Target, 0);
    ASSERT_NE(pending, nullptr) << "the applier accumulated no pending upload for the level";
    ASSERT_EQ(pending->Regions.size(), emitted.size())
        << "the applier's stored region count is not the emitted one";
    for (SizeT i = 0; i < pending->Regions.size(); ++i) {
        EXPECT_EQ(pending->Regions[i].X, emitted[i].X) << "MGPSubRegion::X at region " << i;
        EXPECT_EQ(pending->Regions[i].Y, emitted[i].Y) << "MGPSubRegion::Y at region " << i;
        EXPECT_EQ(pending->Regions[i].W, emitted[i].W) << "MGPSubRegion::W at region " << i;
        EXPECT_EQ(pending->Regions[i].H, emitted[i].H) << "MGPSubRegion::H at region " << i;
        EXPECT_EQ(pending->Regions[i].SrcOffset, emitted[i].SrcOffset)
            << "MGPSubRegion::SrcOffset at region " << i;
        EXPECT_EQ(pending->Regions[i].SrcRowStride, emitted[i].SrcRowStride)
            << "MGPSubRegion::SrcRowStride at region " << i;
        EXPECT_EQ(pending->Regions[i].SrcSliceStride, emitted[i].SrcSliceStride)
            << "MGPSubRegion::SrcSliceStride at region " << i;
    }
}

// ============================ M3 ============================
//
// D-D5 STEP 1 READ LITERALLY: the client clears a level's flags ONLY for a record the applier
// ACCEPTED. v1 cleared on dispatch, so any refusal left the server with nothing and the client
// with a clean flag - and MG_Impl contains no reader of a texture's dirty state, so the level
// simply stopped updating for the life of the texture.
TEST(TextureEmit, ARefusedUploadLeavesTheLevelDirtyAndOnTheDrainList) {
    TextureScope scope;
    const auto texture = MakeTexture2D(17, 32);
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{8, 8, 1});
    ASSERT_EQ(Textures().DrainListSize(), 1u);

    // THE REFUSAL, constructed rather than mocked: the applier is told to drop every object
    // record, so the handle the record names resolves to nothing. That is the ordinary shape of
    // a texture born while the subsystem bit was clear, and it is a COUNTED NO-OP on the far
    // side - invisible from the call site without the acceptance return.
    MGPipeApplierReleaseObjectRecords();
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), 1u) << "the record was not even emitted";
    EXPECT_EQ(Textures().RefusedSubDataCount(), 1u) << "the applier did not refuse a record it lost";
    EXPECT_TRUE(texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0))
        << "the client cleared its dirty flag for a level the applier REFUSED, so those texels "
           "exist nowhere and the level stops updating for the life of the texture";
    EXPECT_EQ(Textures().DrainListSize(), 1u)
        << "a refused level was dropped from the drain list, so nothing will retry it";

    // AND IT SELF-HEALS. The publication LATCH cannot drive this half - it answers "did a
    // create for this handle go out", which is still true after the records were dropped - so
    // the applier's own REFUSAL of the respecify is what republishes the create. That is the
    // second use of the acceptance return and the reason the emitter asks for it on all three
    // calls rather than only on the upload.
    //
    // A REAL storage definition, not a restatement of the one it already has: the emitter dedupes
    // a respecify on the built descriptor itself, so a call that moves no field returns before
    // reaching the applier at all and there is nothing for the refusal to answer.
    const Uint64 createsBefore = Textures().CreateCount();
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 0, MipmapInput{IntVec3{64, 64, 1}, 64 * 64 * 4});
    EXPECT_GT(Textures().CreateCount(), createsBefore)
        << "a respecify the applier refused did not republish the create it is missing";
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{8, 8, 1});
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().RefusedSubDataCount(), 1u) << "the retry was refused as well";
    EXPECT_FALSE(texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0));
    EXPECT_EQ(Textures().DrainListSize(), 0u);
}

// ID-39: THE CLIENT'S HALF OF THE "NO CONSUMER" RULE, and the defect it closes.
//
// A P4a family's gate used to be two conjuncts - the operator's subsystem bit in
// MOBILEGL_PIPE_PUSH, and this build having WIRED the family. P3a's buffers have always had a
// THIRD (MGPipeResourceSubsystemEnabled() is bit 7 AND MGPipeGetResourceOps() != nullptr) and
// P4a's four families did not. Magma (DirectVulkan) registers no table and has none of P4a's
// twins, so with kMGPipeWiredTextureSubsystem = 1 the client emitted, the applier ACCEPTED, the
// emitter cleared the level's dirty flags on that acceptance (D-D5 as amended by ID-18 M3), and
// Magma's legacy upload path then found nothing to upload: 66 texture-upload-shaped
// DirectVulkan integration-gpu cases red on the push build while the pull build stayed green.
//
// WHAT THIS PINS IS "NOTHING AT ALL", NOT "LESS". No create, no respecify, no params, no entry
// on the drain list - and the FRONTEND's own dirty flag still set, which is the state the legacy
// pull path reads and the one whose loss no pixel comparison on this side can see. The applier's
// belt (ResourceEmit.EveryP4aFamilyEntryPointDeclinesWhenNoBackendRegisteredTheConsumer) is
// under this and is asserted here to have caught NOTHING: if it had, the gate would be the thing
// that failed.
TEST(TextureEmit, WithNoBackendConsumerTheFamilyGateIsFalseAndNothingReachesTheApplier) {
    TextureScope scope;
    {
        ScopedNoResourceOps noConsumer;

        // All four families, because all four ride the one signal (D-D1: a texture and a
        // renderbuffer ARE resource rows, and the other three name texture handles).
        EXPECT_FALSE(
            MGPipeP4aFamilyEmits(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem));
        EXPECT_FALSE(MGPipeP4aFamilyEmits(kMGPipeSubsystemFramebuffer, kMGPipeSubsystemFramebuffer));
        EXPECT_FALSE(MGPipeP4aFamilyEmits(kMGPipeSubsystemSamplers, kMGPipeSubsystemSamplers));
        EXPECT_FALSE(MGPipeP4aFamilyEmits(kMGPipeSubsystemPrograms, kMGPipeSubsystemPrograms));
        // And the P2/P3a families are NOT narrowed by it - their bits are outside
        // kMGPipeP4aFamilySubsystems, which is what makes "nothing that emits today changes"
        // checkable rather than asserted. The bit is set here because TextureScope sets only
        // the two this suite needs, and restored before anything else runs.
        const Uint64 savedPush = MG_Config::Features.PipePush;
        MG_Config::Features.PipePush |= kMGPipeSubsystemVertexInput;
        EXPECT_TRUE(MGPipeP4aFamilyEmits(kMGPipeSubsystemVertexInput, kMGPipeSubsystemVertexInput));
        MG_Config::Features.PipePush = savedPush;

        const auto texture = MakeTexture2D(41, 32);
        const MGPipeHandle handle = Textures().FindTexture(*texture);
        // THE HANDLE IS STILL MINTED, deliberately: the mint is unconditional (PipeFill.cpp),
        // costs one free-list pop and emits nothing, and other families name a texture by
        // handle whether or not this family is switched on. What the gate withholds is the
        // EMISSION, never the identity.
        ASSERT_FALSE(MGPipeHandleIsNull(handle));

        EXPECT_FALSE(MGPipeHandleIsPublished(MGPipeKind::Texture, handle))
            << "a create was published to an applier no backend reads";
        EXPECT_EQ(Textures().CreateCount(), 0u);
        EXPECT_EQ(Textures().RespecifyCount(), 0u);
        EXPECT_TRUE(MGPipeApplier().TextureResources.empty());

        texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0},
                                        IntVec3{8, 8, 1});
        EXPECT_EQ(Textures().DrainListSize(), 0u) << "a level was queued for a drain that has no consumer";
        EXPECT_EQ(Textures().SubDataCount(), 0u);
        // THE ONE THAT MATTERED. The frontend's flag is what Magma's legacy path uploads from,
        // and clearing it on an acceptance nobody would read is the whole of the defect.
        EXPECT_TRUE(texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0))
            << "the level's dirty flag was cleared on a backend whose legacy path still owes the "
               "upload, so those texels exist nowhere";

        EXPECT_EQ(MGPipeApplier().RefusedNoConsumer, 0u)
            << "the client emitted anyway and the applier's belt caught it; the GATE is what must "
               "have stopped it";
    }

    // ---- and with a consumer registered, the same sequence lands ----
    EXPECT_TRUE(MGPipeP4aFamilyEmits(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem));
    const auto consumed = MakeTexture2D(42, 32);
    const MGPipeHandle live = Textures().FindTexture(*consumed);
    ASSERT_FALSE(MGPipeHandleIsNull(live));
    EXPECT_TRUE(MGPipeHandleIsPublished(MGPipeKind::Texture, live));
    EXPECT_GT(Textures().CreateCount(), 0u);
    consumed->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0},
                                     IntVec3{8, 8, 1});
    EXPECT_EQ(Textures().DrainListSize(), 1u);
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), 1u);
    EXPECT_EQ(Textures().RefusedSubDataCount(), 0u);
    EXPECT_EQ(MGPipeApplier().RefusedNoConsumer, 0u);
    EXPECT_FALSE(consumed->IsStorageDirty(TextureUploadTarget::Texture2D, 0));
}

// ==================== S-3 (ID-41): D-K2's DEPENDENCY TABLE, ON THE CLIENT ====================
//
// THE SAME DEFECT AS THE CASE ABOVE, ONE SIGNAL OVER. Espryt's four
// `Resolve<Family>SubsystemArm()` functions REFUSE a family whose D-K2 dependency bit is clear
// and run the legacy arm instead - the backstop. But the client's emission used to be gated on
// MOBILEGL_PIPE_PUSH's own bit alone, so at 0x7ff (bit 10 set, bit 11 clear) it emitted the whole
// texture family anyway, the applier accepted it, the emitter cleared each level's dirty flag on
// that acceptance - and the server then refused bit 10 and ran a legacy path with nothing left to
// upload. 438/491 on the DirectGLES integration lane, the same 47 texture-upload failures ID-39
// saw on Magma for the consumer-less version of the identical mistake.
//
// WHAT THESE THREE CASES PIN IS "NOTHING AT ALL", NOT "LESS", exactly as the consumer case above
// does: no create, no respecify, no params, no entry on the drain list - and the FRONTEND's own
// dirty flag still set, which is the state the legacy pull path reads and the one whose loss no
// pixel comparison on this side can see.
TEST(TextureEmit, WithTheSamplerBitClearTheTextureFamilyGateIsFalseAndNothingReachesTheApplier) {
    TextureScope scope;
    // THE 0x7ff SHAPE EXACTLY: bit 10 set, bit 11 clear. D-K2's FOURTH row (ID-14/ID-15) -
    // MGPTextureParams::BuiltinSampler is a SamplerCso handle, only bit 11 mints sampler CSOs,
    // and the applier's verdict for a null one is Fatal{ProtocolCorruption}.
    MG_Config::Features.PipePush &= ~kMGPipeSubsystemSamplers;

    EXPECT_FALSE(MGPipeP4aFamilyEmits(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem))
        << "bit 10 is set and bit 11 is clear; Espryt refuses this family, so the client must not "
           "emit into it";

    const auto texture = MakeTexture2D(43, 32);
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    // The mint is unconditional and stays that way: what a dependency withholds is the EMISSION,
    // never the identity, exactly as for the consumer conjunct.
    ASSERT_FALSE(MGPipeHandleIsNull(handle));
    EXPECT_FALSE(MGPipeHandleIsPublished(MGPipeKind::Texture, handle))
        << "a create was published for a family the server refuses at this mask";
    EXPECT_EQ(Textures().CreateCount(), 0u);
    EXPECT_EQ(Textures().RespecifyCount(), 0u);
    EXPECT_TRUE(MGPipeApplier().TextureResources.empty());

    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0},
                                    IntVec3{8, 8, 1});
    EXPECT_EQ(Textures().DrainListSize(), 0u);
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), 0u);
    // THE ONE THAT MATTERED, and it is the whole of S-3: the level's flag is what Espryt's LEGACY
    // texture arm uploads from once it has refused bit 10.
    EXPECT_TRUE(texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0))
        << "the level's dirty flag was cleared at a mask whose server-side arm is the legacy one, "
           "so those texels exist nowhere";
    EXPECT_EQ(MGPipeApplier().RefusedNoConsumer, 0u)
        << "the belt is about the consumer; the DEPENDENCY is the gate's job and the gate is what "
           "must have stopped this";

    // ---- and with bit 11 back, the same sequence lands ----
    MG_Config::Features.PipePush |= kMGPipeSubsystemSamplers;
    EXPECT_TRUE(MGPipeP4aFamilyEmits(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem));
    const auto live = MakeTexture2D(44, 32);
    const MGPipeHandle liveHandle = Textures().FindTexture(*live);
    ASSERT_FALSE(MGPipeHandleIsNull(liveHandle));
    EXPECT_TRUE(MGPipeHandleIsPublished(MGPipeKind::Texture, liveHandle));
    live->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0},
                                 IntVec3{8, 8, 1});
    EXPECT_EQ(Textures().DrainListSize(), 1u);
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), 1u);
    EXPECT_FALSE(live->IsStorageDirty(TextureUploadTarget::Texture2D, 0));
}

// THE OTHER HALF OF THE SAME ROW: bit 10 requires bit 7 as well (D-D1). A buffer texture's
// MGPResourceDesc::BufferForTexBuffer names a Buffer handle and only bit 7 puts twins in the
// resource slot table, so ResolveTextureResourceSubsystemArm refuses bit 10 without it - and a
// refused family must not have had its flags cleared by an emission that already went out. This
// is the arm the 0x5ff-shaped masks reach from the other side.
TEST(TextureEmit, WithTheBufferResourceBitClearTheTextureFamilyGateIsFalseAndNothingReachesTheApplier) {
    TextureScope scope;
    MG_Config::Features.PipePush &= ~kMGPipeSubsystemResources;

    EXPECT_FALSE(MGPipeP4aFamilyEmits(kMGPipeSubsystemTextureResources, kMGPipeWiredTextureSubsystem))
        << "bit 10 is set and bit 7 is clear; Espryt refuses this family, so the client must not "
           "emit into it";

    const auto texture = MakeTexture2D(45, 32);
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    ASSERT_FALSE(MGPipeHandleIsNull(handle));
    EXPECT_FALSE(MGPipeHandleIsPublished(MGPipeKind::Texture, handle));
    EXPECT_EQ(Textures().CreateCount(), 0u);
    EXPECT_EQ(Textures().RespecifyCount(), 0u);
    EXPECT_TRUE(MGPipeApplier().TextureResources.empty());

    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0},
                                    IntVec3{8, 8, 1});
    EXPECT_EQ(Textures().DrainListSize(), 0u);
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), 0u);
    EXPECT_TRUE(texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0));
    EXPECT_EQ(MGPipeApplier().RefusedNoConsumer, 0u);
}

// THE TABLE ITSELF, ONE ROW PER FAMILY, AND THE MIRROR PAIRS THAT STAY LIVE. The two cases above
// drive the texture family end to end because its emitter is the one this suite owns; the
// framebuffer, sampler and program families are asserted through the gate itself, which is the
// single predicate every one of their birth hooks and every `wants()` row in the walk resolves
// through (MGPipeP4aFamilyEmits returns FamilyIsLive, not a second copy of it). `wired` is passed
// as the family's own bit, which is what that constant is once the family has landed its emitter.
//
// THE ROWS ARE NON-TRANSITIVE ON PURPOSE. Espryt's resolvers classify their arms from
// MOBILEGL_PIPE_PUSH alone, so this table asks the same question they ask - "is the dependency
// BIT set" - and not "is the other family live". At a mask like 0x7ff that means the framebuffer
// family stays LIVE on both sides while the texture family is dead on both sides, which is the
// state the two must agree on; a client that withheld more than the server refuses would leave
// the server's handle arm live with no records to read.
TEST(TextureEmit, EveryDKTwoDependencyRowGatesItsOwnFamilyAndTheMirrorPairsStayLive) {
    TextureScope scope;
    const auto emits = [](Uint64 family) { return MGPipeP4aFamilyEmits(family, family); };
    const Uint64 kAll = kMGPipeSubsystemResources | kMGPipeSubsystemFramebuffer |
                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers |
                        kMGPipeSubsystemPrograms;

    // ---- every dependency satisfied: all four families live ----
    MG_Config::Features.PipePush = kAll;
    EXPECT_TRUE(emits(kMGPipeSubsystemFramebuffer));
    EXPECT_TRUE(emits(kMGPipeSubsystemTextureResources));
    EXPECT_TRUE(emits(kMGPipeSubsystemSamplers));
    EXPECT_TRUE(emits(kMGPipeSubsystemPrograms));

    // ---- ROW 1: bit 9 requires bit 10 (MGPSurface::Res names a texture or renderbuffer) ----
    MG_Config::Features.PipePush = kAll & ~kMGPipeSubsystemTextureResources;
    EXPECT_FALSE(emits(kMGPipeSubsystemFramebuffer)) << "bit 9 set, bit 10 clear";
    // ROW 3 falls out of the same mask: bit 11 requires bit 10.
    EXPECT_FALSE(emits(kMGPipeSubsystemSamplers)) << "bit 11 set, bit 10 clear";
    EXPECT_TRUE(emits(kMGPipeSubsystemPrograms)) << "bit 12 depends on nothing";

    // ---- ROW 2a: bit 10 requires bit 11 - the 0x7ff shape ----
    MG_Config::Features.PipePush = kAll & ~kMGPipeSubsystemSamplers;
    EXPECT_FALSE(emits(kMGPipeSubsystemTextureResources)) << "bit 10 set, bit 11 clear";
    EXPECT_TRUE(emits(kMGPipeSubsystemFramebuffer))
        << "bit 9's row names bit 10 and bit 10 IS set at this mask - the rows are non-transitive "
           "because Espryt's ResolveFramebufferSubsystemArm is";
    EXPECT_TRUE(emits(kMGPipeSubsystemPrograms));

    // ---- ROW 2b: bit 10 requires bit 7 - the D-D1 half ----
    MG_Config::Features.PipePush = kAll & ~kMGPipeSubsystemResources;
    EXPECT_FALSE(emits(kMGPipeSubsystemTextureResources)) << "bit 10 set, bit 7 clear";
    EXPECT_TRUE(emits(kMGPipeSubsystemSamplers)) << "bit 11's only row is bit 10, which is set";
    EXPECT_TRUE(emits(kMGPipeSubsystemPrograms));

    // ---- ROW 4: bit 12 depends on NOTHING, so it is live entirely on its own ----
    MG_Config::Features.PipePush = kMGPipeSubsystemPrograms;
    EXPECT_TRUE(emits(kMGPipeSubsystemPrograms));

    // ---- THE MIRROR PAIRS, said out loud: an unreachable branch that says something different
    //      is how the reachable one drifts (Managers.cpp:2377-2381's own words) ----
    // bits 10 + 11 without bit 9: FINE, and it is the 0xdff arm.
    MG_Config::Features.PipePush = kAll & ~kMGPipeSubsystemFramebuffer;
    EXPECT_TRUE(emits(kMGPipeSubsystemTextureResources));
    EXPECT_TRUE(emits(kMGPipeSubsystemSamplers));
    EXPECT_TRUE(emits(kMGPipeSubsystemPrograms));
    // bit 7 without bit 10: FINE, and it is P3a's shipped configuration - no P4a family is live
    // because none of their own bits is set, and that is the ONLY reason.
    MG_Config::Features.PipePush = kMGPipeSubsystemResources;
    EXPECT_FALSE(emits(kMGPipeSubsystemFramebuffer));
    EXPECT_FALSE(emits(kMGPipeSubsystemTextureResources));
    EXPECT_FALSE(emits(kMGPipeSubsystemSamplers));
    EXPECT_FALSE(emits(kMGPipeSubsystemPrograms));

    // ---- AND THE P2/P3a FAMILIES ARE NOT NARROWED BY ANY OF IT: their bits are outside
    //      kMGPipeP4aFamilySubsystems, so they have no dependency row and no consumer conjunct.
    //      This is what makes "nothing that emits today changes" checkable rather than asserted.
    MG_Config::Features.PipePush = kMGPipeSubsystemVertexInput;
    EXPECT_TRUE(emits(kMGPipeSubsystemVertexInput))
        << "bit 8 alone, with bit 7 clear: P3a's own rule, which this table must not touch";
}

// ============================ M4 ============================
//
// THE CANONICAL ORDER FOR THE TEXTURES THE HINT WAS WRITTEN FOR: glTexStorage2D, then
// glBindImageTexture. An IMMUTABLE texture has no further respecify - that is what immutable
// means - so before this the applier's record kept ImageBindableHint = 0 for ever and the
// PREVENTION half of the texture-remint stall class was a no-op for exactly its own target.
TEST(TextureEmit, AnImmutableTexturesImageBindableHintReachesTheApplierAfterItsAllocation) {
    TextureScope scope;
    const auto texture = MakeTexture2D(18, 32);
    texture->SetImmutableLevels(1);
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 0, MipmapInput{IntVec3{32, 32, 1}, 32 * 32 * 4});
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    ASSERT_TRUE(texture->IsImmutable());

    const MGPipeResourceRecord* stored = AppliedTexture(handle);
    ASSERT_NE(stored, nullptr);
    ASSERT_EQ(stored->Desc.ImageBindableHint, 0);
    const Uint64 serialBefore = stored->Serial;
    const Uint32 width = stored->Desc.Width;

    // A pending upload standing at the moment the mask moves, because the metadata update must
    // not eat it: a mask change arriving between a glTexSubImage2D and the sync that consumes it
    // replaces no storage and therefore replaces no coordinate system.
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{4, 4, 1});
    Textures().DrainTextureSubData(Ctx());
    ASSERT_EQ(Textures().RefusedSubDataCount(), 0u);
    ASSERT_NE(AppliedTexture(handle), nullptr);
    ASSERT_NE(AppliedUpload(*AppliedTexture(handle), Textures().LastSubData().Target, 0), nullptr);

    Textures().NoteTextureBoundAs(handle, kMGPipeBindShaderImage);
    const MGPipeResourceRecord* after = AppliedTexture(handle);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->Desc.ImageBindableHint, 1)
        << "an immutable texture bound to an image unit AFTER its allocation never told the "
           "server it may be image-bound";
    EXPECT_NE(after->Desc.BindMask & kMGPipeBindShaderImage, 0) << "MGPResourceDesc::BindMask";
    EXPECT_EQ(after->Desc.Width, width) << "a metadata update moved a storage-defining field";
    EXPECT_GT(after->Serial, serialBefore) << "the metadata update did not publish";
    EXPECT_NE(AppliedUpload(*after, Textures().LastSubData().Target, 0), nullptr)
        << "the metadata respecify dropped a pending upload it does not replace the storage of";
}

// ============================ M2 ============================
//
// THE THIRTEENTH MGP_NOTE_AGGREGATE(TextureParams) SITE. MGPTextureParams takes MinLod, MaxLod
// and LodBias off the texture's built-in SamplerObject, and every glTexParameter that writes
// them lands on that object and on nothing the texture's own params version watches - so
// `glTexStorage2D(...); glTexParameterf(GL_TEXTURE_MIN_LOD, 2.0f); draw;` left the applier's
// record saying MinLod = 0 and Espryt pushed the wrong LOD clamp. Wrong pixels.
//
// THE CALL THIS DRIVES IS EXACTLY THE ONE GL_Texture.cpp MAKES at its three glTexParameter
// choke points (the granted call sites); what it pins here is the emitter's half - that the
// hook republishes when either version moved and stays silent when neither did.
TEST(TextureEmit, ALodWriteOnTheBuiltinSamplerRepublishesTheParams) {
    TextureScope scope;
    const auto texture = MakeTexture2D(19, 16);
    MG_Pipe::MGPipeEmitTextureParams(*texture);
    const Uint64 paramsBefore = Textures().ParamCount();
    ASSERT_GT(paramsBefore, 0u);
    ASSERT_EQ(Textures().LastParams().MinLod, texture->GetSamplerObject()->GetMinLod());

    // NOTHING MOVED: the version-first skip reads both counters and emits nothing.
    MG_Pipe::MGPipeEmitTextureParams(*texture);
    EXPECT_EQ(Textures().ParamCount(), paramsBefore)
        << "an unchanged texture re-emitted its parameters";

    // A WRITE THAT ONLY THE SAMPLER OBJECT SEES.
    const Uint16 textureVersionBefore = texture->GetTextureParamsVersion();
    texture->GetSamplerObject()->SetLodRange(2.0f, 7.0f);
    EXPECT_EQ(texture->GetTextureParamsVersion(), textureVersionBefore)
        << "the texture's own params version moved, so this case is not testing the gap";
    MG_Pipe::MGPipeEmitTextureParams(*texture);
    EXPECT_GT(Textures().ParamCount(), paramsBefore)
        << "a LOD write on the built-in sampler published no set_texture_params, so the applier's "
           "record keeps the previous LOD clamp";
    EXPECT_FLOAT_EQ(Textures().LastParams().MinLod, 2.0f);
    EXPECT_FLOAT_EQ(Textures().LastParams().MaxLod, 7.0f);

    texture->GetSamplerObject()->SetLodBias(1.5f);
    const Uint64 afterLodRange = Textures().ParamCount();
    MG_Pipe::MGPipeEmitTextureParams(*texture);
    EXPECT_GT(Textures().ParamCount(), afterLodRange);
    EXPECT_FLOAT_EQ(Textures().LastParams().LodBias, 1.5f);
}

// ============================ ID-17 ============================
TEST(TextureEmit, ATexturesBuiltinSamplerHoldsOneCacheReferenceAndSwapsItWithTheContent) {
    TextureScope scope;
    MGPipeSamplerCsoCache& cache = MGPipeSamplerCsoCacheInstance();
    const auto texture = MakeTexture2D(20, 16);
    MG_Pipe::MGPipeEmitTextureParams(*texture);
    const MGPipeHandle first = Textures().LastParams().BuiltinSampler;
    ASSERT_FALSE(MGPipeHandleIsNull(first));
    EXPECT_EQ(cache.RefCountOf(first), 1u)
        << "EVERY Acquire takes a reference and this emitter owes exactly one - two would pin the "
           "entry for ever and none would let the LRU take a handle a standing record names";

    // A re-emission with the value unchanged hands the second reference straight back.
    texture->SetSwizzleParam(TextureSwizzleParam::Red, TextureSwizzleParam::Blue);
    EXPECT_TRUE(Textures().LastParams().BuiltinSampler == first)
        << "a texture-only parameter moved the content-addressed sampler handle";
    EXPECT_EQ(cache.RefCountOf(first), 1u) << "the re-emission leaked a second reference";

    // A SAMPLER write moves the value, so the content-addressed handle moves with it - and the
    // previous handle's reference is given back at that moment.
    texture->GetSamplerObject()->SetLodBias(3.25f);
    MG_Pipe::MGPipeEmitTextureParams(*texture);
    const MGPipeHandle second = Textures().LastParams().BuiltinSampler;
    ASSERT_FALSE(second == first) << "a sampler parameter change did not move the CSO handle";
    EXPECT_EQ(cache.RefCountOf(second), 1u);
    EXPECT_EQ(cache.RefCountOf(first), 0u)
        << "the previous built-in sampler handle was never released, so its entry is pinned for "
           "the life of the process";
}

// ============================ m4 ============================
TEST(TextureEmit, ARecycledTextureSlotDoesNotInheritItsPredecessorsBindMask) {
    TextureScope scope;
    MGPipeSamplerCsoCache& cache = MGPipeSamplerCsoCacheInstance();
    MGPipeHandle firstHandle{};
    MGPipeHandle firstCso{};
    {
        const auto first = MakeTexture2D(21, 8);
        firstHandle = Textures().FindTexture(*first);
        ASSERT_FALSE(MGPipeHandleIsNull(firstHandle));
        // A sampler value NOTHING ELSE in this case shares, so the CSO the entry pins is this
        // texture's alone and its reference count is an exact statement about this entry.
        first->GetSamplerObject()->SetLodBias(9.75f);
        MG_Pipe::MGPipeEmitTextureParams(*first);
        firstCso = Textures().BuiltinSamplerOf(firstHandle);
        ASSERT_FALSE(MGPipeHandleIsNull(firstCso));
        ASSERT_EQ(cache.RefCountOf(firstCso), 1u);
        // The framebuffer emitter ORs these into the entry whether or not the texture family is
        // on, so a sticky mask really can outlive its object.
        Textures().NoteTextureBoundAs(firstHandle, kMGPipeBindRenderTarget);
        Textures().NoteTextureBoundAs(firstHandle, kMGPipeBindShaderImage);
        ASSERT_NE(Textures().TextureBindMask(firstHandle) & kMGPipeBindRenderTarget, 0);
    }
    // Since the final review's C-2 the reference goes back AT THE DEATH, through the death
    // helper's forward, and not first at the recycle.
    EXPECT_EQ(cache.RefCountOf(firstCso), 0u)
        << "the dead texture's cache reference survived its death; the death helper did not reach the emitter";
    EXPECT_EQ(Textures().TextureBindMask(firstHandle), 0u) << "a dead handle still reads its sticky mask";
    const auto second = MakeTexture2D(22, 8);
    const MGPipeHandle secondHandle = Textures().FindTexture(*second);
    ASSERT_EQ(secondHandle.Slot, firstHandle.Slot) << "the slot was not recycled; the case proves nothing";
    ASSERT_NE(secondHandle.Gen, firstHandle.Gen);
    EXPECT_EQ(Textures().TextureBindMask(secondHandle), 0u)
        << "a recycled slot's new texture inherited the dead one's sticky bind mask, so its very "
           "first descriptor said RENDER_TARGET and image-bindable about an object nothing bound";
    EXPECT_EQ(Textures().LastDesc().ImageBindableHint, 0);
    EXPECT_TRUE(Textures().BuiltinSamplerOf(secondHandle) != firstCso)
        << "the recycled entry is still pinning the dead texture's built-in sampler";
    EXPECT_EQ(cache.RefCountOf(firstCso), 0u)
        << "the dead texture's cache reference was never given back, so its entry is pinned for "
           "the life of the process - and this is the only moment a client emitter can see a "
           "texture die, the death helper being the contract's";
}

// ============================ the declared clean-arm deviation ============================
//
// The contract's MGPipeNoteTextureLevelDirty carries no `dirty` flag, so a level that goes
// clean is not removed from the drain list at the moment it goes clean - it is COLLECTED at the
// next drain, where !IsStorageDirty is the first test EmitOneLevel makes. What must never
// happen is the level being dropped while it is still dirty, or a re-dirty after the collection
// failing to re-append.
TEST(TextureEmit, ALevelMarkedCleanIsCollectedAtTheNextDrain) {
    TextureScope scope;
    const auto texture = MakeTexture2D(23, 16);
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{4, 4, 1});
    ASSERT_EQ(Textures().DrainListSize(), 1u);

    texture->MarkStorageDirty(TextureUploadTarget::Texture2D, 0, false);
    EXPECT_EQ(Textures().DrainListSize(), 1u) << "the entry is collected at the drain, not here";
    const Uint64 emissionsBefore = Textures().SubDataCount();
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), emissionsBefore)
        << "a level that is no longer dirty was uploaded anyway";
    EXPECT_EQ(Textures().DrainListSize(), 0u) << "the clean level was never collected";

    // And the key really was dropped from the per-slot list, so a later write re-appends.
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{4, 4, 1});
    EXPECT_EQ(Textures().DrainListSize(), 1u)
        << "a re-dirtied level did not go back on the drain list, so its texels are owed for ever";
}
// ============================ final review C-1 ============================
//
// THE CLIENT PASSES THE LEVEL IT REDEFINES. AllocateStorage is per (uploadTarget, level) while
// the descriptor carries only the base extent and the level count, so only the caller can tell
// the applier WHICH storage a respecify replaces (wire C1's MGPRespecifiedLevel); before the fix
// every texture respecify took the whole-resource arm and dropped every pending upload of the
// texture - including a level the applier had already accepted and whose client flag was
// therefore already clear (D-D5 step 1). Driven through the real AllocateStorage.
TEST(TextureEmit, ALevelDefinedAfterAnEmittedButUnconsumedUploadKeepsThatUpload) {
    TextureScope scope;
    const auto texture = MakeShared<TextureObject2D>(90);
    texture->SetInternalFormat(TextureInternalFormat::RGBA8);
    // glTexImage2D(level 0, data)
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 0, MipmapInput{IntVec3{64, 64, 1}, 64 * 64 * 4});
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{64, 64, 1});
    // A verb the texture is not reached by: the drain emits level 0, the applier accepts, the
    // client clears its flag. Nothing has consumed the entry.
    Textures().DrainTextureSubData(Ctx());
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    const MGPipeResourceRecord* record = AppliedTexture(handle);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(Textures().RefusedSubDataCount(), 0u);
    ASSERT_EQ(record->PendingUploads.size(), 1u);
    ASSERT_FALSE(texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0));
    // glTexImage2D(level 1, data): a DIFFERENT level.
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 1, MipmapInput{IntVec3{32, 32, 1}, 32 * 32 * 4});
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 1, IntVec3{0, 0, 0}, IntVec3{32, 32, 1});
    record = AppliedTexture(handle);
    ASSERT_NE(record, nullptr);
    Bool levelZeroPending = false;
    for (const auto& pending : record->PendingUploads) {
        if (pending.Level == 0) levelZeroPending = true;
    }
    EXPECT_TRUE(levelZeroPending)
        << "defining level 1 dropped level 0's accepted-but-unconsumed pending upload (PendingUploads.size()="
        << record->PendingUploads.size() << ") while level 0's client dirty flag is "
        << (texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0) ? "set" : "CLEAR - the texels are owed by nobody");
    // And after the next drain both levels stand in the set.
    Textures().DrainTextureSubData(Ctx());
    record = AppliedTexture(handle);
    ASSERT_NE(record, nullptr);
    Bool zeroAfter = false;
    Bool oneAfter = false;
    for (const auto& pending : record->PendingUploads) {
        if (pending.Level == 0) zeroAfter = true;
        if (pending.Level == 1) oneAfter = true;
    }
    EXPECT_TRUE(oneAfter);
    EXPECT_TRUE(zeroAfter) << "level 0's texels are lost: not pending, flag clear";
}

// A chain truncation - glGenerateMipmap fitting the chain, a base redefinition discarding its
// tail - removes the levels at and above the cut and nothing below it. Before the fix it was a
// whole-resource respecify and took level 0's standing upload with the tail.
TEST(TextureEmit, AChainTruncationKeepsTheSurvivingLevelsPendingUploads) {
    TextureScope scope;
    const auto texture = MakeTexture2D(93, 64, /*levels=*/3);
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{64, 64, 1});
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 2, IntVec3{0, 0, 0}, IntVec3{16, 16, 1});
    Textures().DrainTextureSubData(Ctx());
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    const MGPipeResourceRecord* record = AppliedTexture(handle);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->PendingUploads.size(), 2u);
    ASSERT_FALSE(texture->IsStorageDirty(TextureUploadTarget::Texture2D, 0));

    texture->TruncateMipmapLevels(TextureUploadTarget::Texture2D, 1);
    record = AppliedTexture(handle);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->Desc.Levels, 1u);
    Bool zeroPending = false;
    Bool twoPending = false;
    for (const auto& pending : record->PendingUploads) {
        if (pending.Level == 0) zeroPending = true;
        if (pending.Level == 2) twoPending = true;
    }
    EXPECT_TRUE(zeroPending) << "truncating the chain above level 0 dropped level 0's standing upload";
    EXPECT_FALSE(twoPending) << "a level the truncation removed kept a pending upload against storage that is gone";
}

// A non-base level redefined at a new size moves NO descriptor field (the descriptor carries
// the base extent and the level count), so the emitter's descriptor dedupe used to swallow the
// respecify and the applier kept a box sized for the OLD level - which Espryt would have
// uploaded past the end of the new one. A per-level respecify reaches the applier whether or
// not the descriptor moved, and drops exactly that level.
TEST(TextureEmit, ARedefinitionOfANonBaseLevelAtANewSizeDropsOnlyThatLevelsPendingUpload) {
    TextureScope scope;
    const auto texture = MakeTexture2D(96, 16, /*levels=*/2);
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{16, 16, 1});
    texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 1, IntVec3{0, 0, 0}, IntVec3{8, 8, 1});
    Textures().DrainTextureSubData(Ctx());
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    const MGPipeResourceRecord* record = AppliedTexture(handle);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->PendingUploads.size(), 2u);
    const Uint64 serialBefore = record->Serial;

    // glTexImage2D(level 1) at 4x4: the base is still 16x16 and the chain still two levels.
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 1, MipmapInput{IntVec3{4, 4, 1}, 4 * 4 * 4});
    record = AppliedTexture(handle);
    ASSERT_NE(record, nullptr);
    EXPECT_GT(record->Serial, serialBefore) << "the per-level respecify never reached the applier";
    Bool zeroPending = false;
    Bool onePending = false;
    for (const auto& pending : record->PendingUploads) {
        if (pending.Level == 0) zeroPending = true;
        if (pending.Level == 1) onePending = true;
    }
    EXPECT_TRUE(zeroPending) << "redefining level 1 dropped level 0's standing upload";
    EXPECT_FALSE(onePending) << "level 1's 8x8 box survived its redefinition onto a 4x4 level";
}

// ============================ final review C-2 ============================
//
// A DEAD HANDLE RESOLVES TO NOTHING AND THE DRAIN LIST DROPS IT AT THE DEATH. The allocator's
// generation moves only at the next hand-out, so between a death and a recycle the dead handle
// compared equal to the slot's generation and the emitter answered the freed ITextureObject*;
// the drain then called a virtual on it once per verb until something recycled the slot.
TEST(TextureEmit, ADeadTexturesHandleResolvesToNothingAndLeavesTheDrainList) {
    TextureScope scope;
    MGPipeHandle handle{};
    {
        const auto texture = MakeShared<TextureObject2D>(91);
        texture->SetInternalFormat(TextureInternalFormat::RGBA8);
        texture->AllocateStorage(TextureUploadTarget::Texture2D, 0, MipmapInput{IntVec3{16, 16, 1}, 16 * 16 * 4});
        // glTexSubImage2D: the level goes on the drain list; NO verb follows before the delete.
        texture->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{16, 16, 1});
        handle = Textures().FindTexture(*texture);
        ASSERT_FALSE(MGPipeHandleIsNull(handle));
        ASSERT_EQ(Textures().DrainListSize(), 1u);
    } // glDeleteTextures: the last SharedPtr drops, ~TextureObjectBase frees the slot
    EXPECT_FALSE(MGPipeSlots().IsLive(MGPipeKind::Texture, handle));
    EXPECT_EQ(Textures().ResolveTexture(handle), nullptr)
        << "ResolveTexture hands back the freed ITextureObject* of a dead-but-not-recycled handle";
    EXPECT_EQ(Textures().DrainListSize(), 0u)
        << "the dead texture's level is still on the drain list, so the next verb walks it";
    // And the next drain has nothing to say about it: no record, no refusal, no emission.
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), 0u);
    EXPECT_EQ(Textures().RefusedSubDataCount(), 0u);
    // The null came from the DEATH PATH retiring the entry, not from the loud refusal that
    // guards a death path that skipped the emitter.
    EXPECT_EQ(Textures().DeadResolveCount(), 0u)
        << "the dead handle was refused by ResolveTexture's guard, so the death helper never told the emitter";
}

// The sampler view minted off the texture's lifetime id (D-F2) has its own record memo in the
// sampler emitter; the texture's death retires it through the view's death helper.
TEST(TextureEmit, ADeadTexturesSamplerViewLatchIsRetiredAtItsDeath) {
    TextureScope scope;
    MGPipeHandle viewHandle{};
    {
        const auto texture = MakeTexture2D(89, 8);
        const MGPipeHandle handle = Textures().FindTexture(*texture);
        Uint64 bytes = 0;
        viewHandle = MGPipeSamplerEmitterInstance().AcquireSamplerView(*texture, handle, bytes);
        ASSERT_FALSE(MGPipeHandleIsNull(viewHandle));
        ASSERT_TRUE(MGPipeSamplerEmitterInstance().RecordIsPublished(viewHandle));
    }
    EXPECT_FALSE(MGPipeSlots().IsLive(MGPipeKind::SamplerViewCso, viewHandle));
    EXPECT_FALSE(MGPipeSamplerEmitterInstance().RecordIsPublished(viewHandle))
        << "a dead sampler view still reads as published in the sampler emitter's memo";
}

// ABA: the recycled slot's new texture owns the drain list entry it makes and none of its
// predecessor's.
TEST(TextureEmit, ATextureRecycledOntoADeadSlotDoesNotInheritTheDrainEntry) {
    TextureScope scope;
    MGPipeHandle deadHandle{};
    {
        const auto dead = MakeTexture2D(97, 8);
        dead->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{8, 8, 1});
        deadHandle = Textures().FindTexture(*dead);
        ASSERT_EQ(Textures().DrainListSize(), 1u);
    }
    EXPECT_EQ(Textures().DrainListSize(), 0u) << "the death did not retire the drain entry";
    const auto successor = MakeTexture2D(98, 32);
    const MGPipeHandle handle = Textures().FindTexture(*successor);
    ASSERT_EQ(handle.Slot, deadHandle.Slot) << "the slot was not recycled; the case proves nothing";
    ASSERT_NE(handle.Gen, deadHandle.Gen);
    EXPECT_EQ(Textures().DrainListSize(), 0u) << "the successor inherited a drain entry it never made";
    successor->MarkStorageDirtyRegion(TextureUploadTarget::Texture2D, 0, IntVec3{0, 0, 0}, IntVec3{32, 32, 1});
    EXPECT_EQ(Textures().DrainListSize(), 1u);
    Textures().DrainTextureSubData(Ctx());
    EXPECT_EQ(Textures().SubDataCount(), 1u) << "exactly the successor's level went out";
    EXPECT_TRUE(Textures().LastSubData().Res == handle);
    EXPECT_EQ(Textures().LastSubData().UnionBox.W, 32u);
    EXPECT_EQ(Textures().DrainListSize(), 0u);
}

// The renderbuffer table has no pointer to dangle but the same stale entry: a dead
// renderbuffer's sticky mask must not be readable through its dead handle.
TEST(TextureEmit, ADeadRenderbuffersEntryIsRetiredWithItsSlot) {
    TextureScope scope;
    MGPipeHandle handle{};
    {
        const auto renderbuffer = MakeShared<RenderbufferObject>(94);
        handle = Textures().FindRenderbuffer(*renderbuffer);
        ASSERT_FALSE(MGPipeHandleIsNull(handle));
        Textures().NoteRenderbufferBoundAs(handle, kMGPipeBindRenderTarget);
        ASSERT_NE(Textures().RenderbufferBindMask(handle) & kMGPipeBindRenderTarget, 0);
    }
    EXPECT_FALSE(MGPipeSlots().IsLive(MGPipeKind::Renderbuffer, handle));
    EXPECT_EQ(Textures().RenderbufferBindMask(handle), 0u)
        << "a dead renderbuffer's entry still answers through its dead handle";
    const auto successor = MakeShared<RenderbufferObject>(99);
    const MGPipeHandle successorHandle = Textures().FindRenderbuffer(*successor);
    ASSERT_EQ(successorHandle.Slot, handle.Slot);
    EXPECT_EQ(Textures().RenderbufferBindMask(successorHandle), 0u);
}

// ============================ final review m-1 (audit F-7) ============================
//
// set_texture_params LATCHES ON ACCEPTANCE, like the sub-data and respecify paths. A record the
// applier refused (it holds nothing for the handle) used to advance the version latch anyway,
// so the parameters were not re-sent until the next glTexParameter* moved a version.
TEST(TextureEmit, ARefusedParamsRecordDoesNotAdvanceTheLatch) {
    TextureScope scope;
    const auto texture = MakeTexture2D(95, 8);
    const MGPipeHandle handle = Textures().FindTexture(*texture);
    ASSERT_NE(AppliedTexture(handle), nullptr);

    // The served context's teardown scope: every object record is dropped while the frontend
    // objects live on. A parameter then moves (a LOD write on the built-in sampler, which the
    // format setter's earlier publication did not carry) and its set_texture_params is refused.
    MGPipeApplierReleaseObjectRecords();
    texture->GetSamplerObject()->SetLodBias(0.5f);
    const Uint64 paramsBefore = Textures().ParamCount();
    MG_Pipe::MGPipeEmitTextureParams(*texture);
    EXPECT_EQ(Textures().ParamCount(), paramsBefore + 1) << "the record was not even emitted";
    EXPECT_EQ(Textures().RefusedParamCount(), 1u) << "the emitter did not see the refusal";

    // The record comes back through the self-healing create the next respecify carries.
    texture->AllocateStorage(TextureUploadTarget::Texture2D, 0, MipmapInput{IntVec3{16, 16, 1}, 16 * 16 * 4});
    const MGPipeResourceRecord* record = AppliedTexture(handle);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->ParamsSerial, 0u);

    // The same parameters, no version moved: with the latch taken on the REFUSED call this
    // returns early and the record never learns them.
    MG_Pipe::MGPipeEmitTextureParams(*texture);
    EXPECT_EQ(Textures().ParamCount(), paramsBefore + 2)
        << "a refused set_texture_params advanced the latch, so the parameters are not re-sent";
    EXPECT_EQ(record->ParamsSerial, 1u) << "the record never learned the LOD write";
    EXPECT_EQ(record->Params.LodBias, 0.5f);
}

#endif // MOBILEGL_PIPE_PUSH

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

// A WHOLE-RESOURCE respecify - a null MGPRespecifiedLevel*, which is every glBufferData,
// glBufferStorage, glTexStorage* and texture view - redefines every level at once, so the boxes
// and rects against all of them go with it: a box kept across a shrink would have the backend
// upload past the end of the new level. THE SCOPE IS THE WHOLE POINT: this case proves the
// whole-resource arm ONLY, and its per-level twin below proves that the other arm may not do
// this.
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
    MGPipeApplyResourceSubData(TextureUpload(texture, 1, MGPBox{0, 0, 0, 32, 32, 1}, 0), texels);
    ASSERT_EQ(TextureRecordOf(3).PendingUploads.size(), 2u);

    MGPipeApplyResourceRespecify(TextureDesc(texture, 8, 55), nullptr);
    EXPECT_TRUE(TextureRecordOf(3).PendingUploads.empty())
        << "a 64-wide box survived onto an 8-wide store";
    EXPECT_EQ(TextureRecordOf(3).Desc.Width, 8u);
#endif
}

// C1, AND IT IS THE CANONICAL MIP-BUILDING SEQUENCE. A mutable texture defines its levels one
// glTexImage*D at a time, and MG_State's AllocateStorage / MarkStorageDirty are per
// (uploadTarget, level) - so defining level 1 re-marks LEVEL 1 AND NOTHING ELSE. Level 0's
// client dirty flag was cleared at its own emission (D-D5 step 1) and the applier's entry is
// the only thing that still owes those texels, because Espryt's incomplete-texture bail is
// exactly the arm this set exists for. A blanket clear here destroys them silently, in every
// build, with no counter and no log line: this case goes red the moment the level scoping is
// dropped and green with it.
TEST(TextureEmit, ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{10, 1};
    const Uint8 texels[4096] = {};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 121));

    // EVERY DESCRIPTOR BELOW CARRIES THE LEVEL COUNT THE CALL IT MODELS WOULD CARRY, and that
    // is not decoration. A mutable mip build grows MipmapStorage's level count as it defines
    // levels, so package B's descriptor (TextureEmit.h: `desc.Levels =
    // mipmap->GetMipmapLevelCount()`) MOVES on the glTexImage2D that adds level 1 - which is
    // also why B's own memcmp dedupe emits that respecify at all. A respecify whose
    // storage-defining fields are all unchanged is a METADATA update (ID-18 M4) and drops
    // nothing whatever level it names, so a case that fed the same descriptor three times would
    // be exercising that arm rather than this one.
    auto levelDesc = [&](Uint16 levels, Uint32 internalFormat) {
        MGPResourceDesc desc = TextureDesc(texture, 64, 121);
        desc.Levels = levels;
        desc.InternalFormat = internalFormat;
        return desc;
    };

    // glTexImage2D(level 0, data): the respecify names the level it defines, and the drain then
    // emits level 0's shape, which the applier accepts.
    const MGPRespecifiedLevel levelZero{kTex2D, 0};
    MGPipeApplyResourceRespecify(levelDesc(1, 0x8058u /*GL_RGBA8*/), nullptr, &levelZero);
    ASSERT_TRUE(MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{0, 0, 0, 64, 64, 1}, 0), texels));
    // A SECOND FACE OF THE SAME LEVEL, keyed the way the packed Target keys it (ID-12: high
    // byte = the cube-face upload target, low byte = the resource target), so what survives is
    // a SET and not one lucky entry - and so that the level number alone cannot be what matched.
    const Uint16 secondFace = MGPipePackSubDataTarget(kTex2D, 1u);
    const MGPRespecifiedLevel faceOfLevelZero{secondFace, 0};
    MGPipeApplyResourceRespecify(levelDesc(1, 0x8058u), nullptr, &faceOfLevelZero);
    MGPSubData otherFace = TextureUpload(texture, 0, MGPBox{0, 0, 0, 64, 64, 1}, 0);
    otherFace.Target = secondFace;
    ASSERT_TRUE(MGPipeApplyResourceSubData(otherFace, texels));
    ASSERT_EQ(TextureRecordOf(10).PendingUploads.size(), 2u);

    // Espryt BAILS - the texture is not mipmap-complete for its min filter - so both entries
    // are still owed when the next GL call arrives.
    //
    // glTexImage2D(level 1, data): this redefines level 1 of the (kTex2D, *) face only, and the
    // level count moves 1 -> 2 with it.
    const MGPRespecifiedLevel levelOne{kTex2D, 1};
    MGPipeApplyResourceRespecify(levelDesc(2, 0x8058u), nullptr, &levelOne);

    ASSERT_EQ(TextureRecordOf(10).PendingUploads.size(), 2u)
        << "a respecify of level 1 dropped the pending uploads of levels it never redefined - "
           "those texels are lost for good, because their dirty flags were cleared at emission";
    EXPECT_EQ(TextureRecordOf(10).PendingUploads[0].UploadTarget, kTex2D);
    EXPECT_EQ(TextureRecordOf(10).PendingUploads[0].Level, 0u);
    EXPECT_EQ(TextureRecordOf(10).PendingUploads[0].UnionBox.W, 64u);
    EXPECT_EQ(TextureRecordOf(10).PendingUploads[1].UploadTarget, secondFace);

    // And the key it DOES name goes, because that level's coordinate system has been replaced.
    // The redefinition modelled here is glTexImage2D(level 1) with a NEW internal format - a
    // legal thing to do to a mutable texture, and a real redefinition of that level's storage,
    // so the descriptor moves and the metadata arm does not claim it.
    ASSERT_TRUE(MGPipeApplyResourceSubData(TextureUpload(texture, 1, MGPBox{0, 0, 0, 32, 32, 1}, 0), texels));
    ASSERT_EQ(TextureRecordOf(10).PendingUploads.size(), 3u);
    MGPipeApplyResourceRespecify(levelDesc(2, 0x8051u /*GL_RGB8*/), nullptr, &levelOne);
    ASSERT_EQ(TextureRecordOf(10).PendingUploads.size(), 2u)
        << "the level the respecify DOES redefine kept its box across the redefinition";
    for (const auto& entry : TextureRecordOf(10).PendingUploads) {
        EXPECT_FALSE(entry.UploadTarget == kTex2D && entry.Level == 1u)
            << "the redefined (upload target, level) survived";
    }
#endif
}

// THE OTHER HALF OF THE LEVEL-SCOPED ERASE KEY (wire review v2 MINOR-1, integrator grant).
//
// wire's C1 fix scopes a respecify's PendingUploads clear to the redefined entry, and the key is
// the PAIR (UploadTarget, Level). The case above pins the LEVEL half: it respecifies level 1 and
// checks that level 0's two entries survive. It cannot pin the UPLOAD TARGET half, because both
// survivors are at a different LEVEL from the redefined one - so an erase that compared only the
// level would still keep them, and the review measured exactly that: DELETING THE UploadTarget
// COMPARISON FROM THE ERASE PREDICATE WAS GREEN ON ALL 74 APPLIER TESTS.
//
// This case is the missing half. Two cube FACES of the SAME level are pending at once - which is
// the ordinary shape of building a cube map, six glTexImage2D calls at level 0 - and one of them
// is redefined. The level number is therefore identical on both sides of the erase, so the only
// thing that can distinguish the entry that must go from the entry that must stay is the upload
// target. An erase keyed on the level alone drops BOTH, and those texels are gone for good: the
// client cleared its dirty flags when the applier accepted them (D-D5 step 1), so there is
// nothing left to re-emit from.
//
// The descriptor's InternalFormat moves on the redefining respecify, exactly as in the case
// above, so this is a real storage redefinition and not ID-18 M4's metadata update - which
// drops nothing at all and would make the case vacuous in the other direction.
TEST(TextureEmit, ARespecifyOfOneCubeFaceKeepsTheOtherFacesUploadOfTheSameLevel) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{11, 1};
    const Uint8 texels[4096] = {};
    MGPResourceDesc cube = TextureDesc(texture, 0, 131);
    cube.Target = static_cast<Uint8>(MGPipeResourceTarget::TexCube);
    MGPipeApplyResourceCreate(cube);

    constexpr Uint16 kTexCube = static_cast<Uint16>(MGPipeResourceTarget::TexCube);
    // The packed Target's two bytes (ID-12): low = the RESOURCE target, high = the per-call
    // UPLOAD target, which for a cube map is the FACE. Both faces below name the same resource
    // target and the same level and differ only in the face, which is the whole point.
    const Uint16 positiveX = MGPipePackSubDataTarget(
        kTexCube, static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX));
    const Uint16 negativeX = MGPipePackSubDataTarget(
        kTexCube, static_cast<Uint32>(TextureUploadTarget::CubeMapNegativeX));
    ASSERT_NE(positiveX, negativeX);
    ASSERT_EQ(MGPipeSubDataResourceTargetOf(positiveX), MGPipeSubDataResourceTargetOf(negativeX))
        << "the two faces must differ in the UPLOAD byte alone, or this case would be measuring "
           "the resource target instead of the face";

    auto faceDesc = [&](Uint32 internalFormat) {
        MGPResourceDesc desc = cube;
        desc.Width = 64;
        desc.Height = 64;
        desc.Levels = 1;
        desc.InternalFormat = internalFormat;
        return desc;
    };
    auto faceUpload = [&](Uint16 face) {
        MGPSubData record = TextureUpload(texture, 0, MGPBox{0, 0, 0, 64, 64, 1}, 0);
        record.Target = face;
        return record;
    };

    // glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, level 0, data), then the same for -X. Espryt
    // bails on both (a cube map with one face defined is not cube-complete), so both are still
    // owed when the next call arrives.
    const MGPRespecifiedLevel positiveXLevelZero{positiveX, 0};
    MGPipeApplyResourceRespecify(faceDesc(0x8058u /*GL_RGBA8*/), nullptr, &positiveXLevelZero);
    ASSERT_TRUE(MGPipeApplyResourceSubData(faceUpload(positiveX), texels));
    const MGPRespecifiedLevel negativeXLevelZero{negativeX, 0};
    MGPipeApplyResourceRespecify(faceDesc(0x8058u), nullptr, &negativeXLevelZero);
    ASSERT_TRUE(MGPipeApplyResourceSubData(faceUpload(negativeX), texels));
    ASSERT_EQ(TextureRecordOf(11).PendingUploads.size(), 2u)
        << "the two faces of one level collapsed onto a single pending entry, so this case cannot "
           "say anything about the erase key";

    // glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_X, level 0) again, with a new internal format:
    // -X's level 0 is redefined and +X's is NOT TOUCHED.
    //
    // THE SECOND-ADDED FACE IS THE ONE REDEFINED, AND THAT CHOICE IS THE WHOLE MUTATION
    // SENSITIVITY. The erase walks PendingUploads in order and `break`s at the first entry the
    // key matches, so a key that compared only the level would still remove exactly ONE entry -
    // and if the redefined face were the first in the vector it would remove the RIGHT one, by
    // the accident of insertion order, and this case would be green on a key with no upload-target
    // comparison at all. Redefining the SECOND makes the level-only key erase +X, which is the
    // face that had to survive: the count is still 1 and the identity is wrong, which is what the
    // assertion below reads. (Measured: with the comparison deleted, the size assertion alone is
    // green and the identity assertion is red.)
    MGPipeApplyResourceRespecify(faceDesc(0x8051u /*GL_RGB8*/), nullptr, &negativeXLevelZero);

    ASSERT_EQ(TextureRecordOf(11).PendingUploads.size(), 1u)
        << "a respecify of ONE cube face's level 0 also dropped the OTHER face's pending upload "
           "of the same level. The erase key is (UploadTarget, Level) and both entries carry "
           "level 0, so an erase that compares the level alone cannot tell them apart - and the "
           "face it never redefined loses its texels for good, because the client cleared that "
           "level's dirty flag when the applier accepted it (D-D5 step 1).";
    EXPECT_EQ(TextureRecordOf(11).PendingUploads[0].UploadTarget, positiveX)
        << "the entry that survived the respecify of GL_TEXTURE_CUBE_MAP_NEGATIVE_X's level 0 is "
           "not the +X face it never named. Both entries carry level 0 and differ only in the "
           "upload-target byte of the packed Target (ID-12), so an erase keyed on the level alone "
           "removes whichever of the two it reaches first - here +X - and the face the application "
           "actually redefined keeps a pending upload describing storage that no longer exists, "
           "while the face it did not redefine has lost its texels.";
    EXPECT_EQ(TextureRecordOf(11).PendingUploads[0].Level, 0u);
    EXPECT_EQ(TextureRecordOf(11).PendingUploads[0].UnionBox.W, 64u)
        << "the untouched face's box was rewritten by the other face's redefinition";
#endif
}

// D-D5 step 1 says the client clears its dirty flag "only for levels whose record the applier
// ACCEPTED", and the call is the only thing that can say so: a dead or stale handle is a
// counted no-op and a corrupt record is a Fatal that deliberately moves NO counter, so in a
// shipped push build a refused upload and an accumulated one are otherwise identical from the
// call site. An emitter that clears on the strength of having emitted loses those texels.
TEST(TextureEmit, TheSubDataCallAnswersWhetherTheRecordWasAcceptedSoTheClientCanClearItsFlag) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{8, 1};
    const Uint8 texels[4096] = {};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 131));
    MGPipeApplyResourceRespecify(TextureDesc(texture, 64, 131), nullptr);

    EXPECT_TRUE(MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{0, 0, 0, 64, 64, 1}, 0), texels))
        << "an accumulated upload answered 'not accepted' and the client would re-send it forever";
    ASSERT_EQ(TextureRecordOf(8).PendingUploads.size(), 1u);

    // A stale generation is the refusal that is NOT a Fatal, so it is the one the answer has to
    // carry: the record is gone, the texels were never taken, and the flag may not be cleared.
    const Uint64 refusedBefore = MGPipeApplier().RefusedResourceCalls;
    EXPECT_FALSE(
        MGPipeApplyResourceSubData(TextureUpload(MGPipeHandle{8, 2}, 0, MGPBox{0, 0, 0, 8, 8, 1}, 0), texels))
        << "a refused upload answered 'accepted' and the client would clear a flag nothing owes";
    EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, refusedBefore + 1);
    EXPECT_EQ(TextureRecordOf(8).PendingUploads.size(), 1u);

    // The buffer half answers on the same terms, and its acceptance does not depend on a
    // backend table being registered - a unit process has none.
    const MGPipeHandle buffer{8, 1};
    MGPResourceDesc bufferDesc{};
    bufferDesc.Resource = buffer;
    bufferDesc.Target = kMGPipeResourceTargetBuffer;
    bufferDesc.Width = 256;
    bufferDesc.GlNameForDiag = 132;
    MGPipeApplyResourceCreate(bufferDesc);
    MGPipeApplyResourceRespecify(bufferDesc, nullptr);
    MGPSubData write{};
    write.Res = buffer;
    write.Target = kMGPipeResourceTargetBuffer;
    ASSERT_TRUE(MGPipeSetSubDataBufferRange(write, 0, 64));
    EXPECT_TRUE(MGPipeApplyResourceSubData(write, texels));
    write.Res = MGPipeHandle{8, 9};
    EXPECT_FALSE(MGPipeApplyResourceSubData(write, texels));
#endif
}

// m3. MGPSubData::Target is PACKED - low byte = MGPipeResourceTarget, high byte = the cube-face
// upload target (ID-12) - so MGPipeResourceTarget::Renderbuffer is a perfectly well-formed
// value for it, and without a gate a renderbuffer record would route to TextureResources and
// accumulate a pending upload onto whatever TEXTURE holds that slot. It is the same argument
// ResourceTableForTarget makes for returning null on an unknown enumerator, and the same
// verdict: acting outside the storage the record names is corruption, not a dropped call.
TEST(TextureEmit, ASubDataRecordWhoseResourceTargetNamesNoTextureIsRefusedRatherThanRouted) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{9, 1};
    const Uint8 texels[4096] = {};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 141));
    MGPipeApplyResourceRespecify(TextureDesc(texture, 64, 141), nullptr);
    MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{0, 0, 0, 64, 64, 1}, 0), texels);
    ASSERT_EQ(TextureRecordOf(9).PendingUploads.size(), 1u);
    const Uint64 serialBefore = TextureRecordOf(9).Serial;
    const Uint64 refusedBefore = MGPipeApplier().RefusedResourceCalls;

    MGPSubData renderbuffer = TextureUpload(texture, 0, MGPBox{0, 0, 0, 8, 8, 1}, 0);
    renderbuffer.Target = static_cast<Uint16>(MGPipeResourceTarget::Renderbuffer);
    ExpectRefusedNaming("resource_subdata {slot=9, gen=1}: the record's resource target names no texture "
                        "to upload into",
                        [&renderbuffer, &texels]() { MGPipeApplyResourceSubData(renderbuffer, texels); });

    // And so is a value at or above the catalogue, and so is the buffer target arriving with a
    // non-zero upload-target half - which the whole-field buffer test above cannot see.
    MGPSubData pastTheCatalogue = renderbuffer;
    pastTheCatalogue.Target = static_cast<Uint16>(MGPipeResourceTarget::Count);
    ExpectRefusedNaming("resource_subdata {slot=9, gen=1}: the record's resource target names no texture "
                        "to upload into",
                        [&pastTheCatalogue, &texels]() {
                            MGPipeApplyResourceSubData(pastTheCatalogue, texels);
                        });

    MGPSubData packedBuffer = renderbuffer;
    packedBuffer.Target = static_cast<Uint16>(0x0100u | kMGPipeResourceTargetBuffer);
    ExpectRefusedNaming("resource_subdata {slot=9, gen=1}: the record's resource target names no texture "
                        "to upload into",
                        [&packedBuffer, &texels]() { MGPipeApplyResourceSubData(packedBuffer, texels); });

    EXPECT_EQ(TextureRecordOf(9).PendingUploads.size(), 1u)
        << "a record naming no texture was accumulated onto the texture holding that slot";
    EXPECT_EQ(TextureRecordOf(9).Serial, serialBefore) << "not one refusal may move the serial";
    EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, refusedBefore)
        << "a corrupt record is not a dropped call and must not be counted as one";
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

// ID-18 M4, AND IT IS THE ONE ARM AN IMMUTABLE TEXTURE HAS. A sticky BindMask /
// ImageBindableHint bit has exactly one way onto the wire - a respecify - and glTexStorage2D
// leaves a texture with no further respecify to carry it, so for the canonical order (allocate,
// THEN bind as an image or attach) the hint that exists to prevent a texture re-mint would never
// arrive at all. B therefore republishes the descriptor when the mask moves, and the applier has
// to tell that call apart from a redefinition: it replaces the descriptor and moves the serial,
// and it drops NOTHING - the storage it is against was not replaced, so no level's coordinate
// system moved. A mask change landing between a glTexSubImage2D and the sync that consumes it
// must not eat those texels, which is C1's bug with a different trigger and just as silent.
TEST(TextureEmit, ARespecifyThatRedefinesNoStorageCarriesTheStickyMaskAndKeepsThePendingUploads) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{11, 1};
    const Uint8 texels[4096] = {};
    MGPipeApplyResourceCreate(TextureDesc(texture, 0, 151));

    // glTexStorage2D: an IMMUTABLE store, which is the whole reason this arm exists.
    MGPResourceDesc allocated = TextureDesc(texture, 64, 151);
    allocated.Immutable = 1;
    allocated.Levels = 2; // level 1 exists for the named-level clause below
    allocated.InternalFormat = 0x8058u; // GL_RGBA8
    allocated.BindMask = static_cast<Uint16>(kMGPipeBindSampler);
    ASSERT_TRUE(MGPipeApplyResourceRespecify(allocated, nullptr));

    // glTexSubImage2D: texels whose client-side dirty flag was cleared at THIS emission, so the
    // applier's entry is the only thing that still owes them.
    ASSERT_TRUE(MGPipeApplyResourceSubData(TextureUpload(texture, 0, MGPBox{0, 0, 0, 64, 64, 1}, 0), texels));
    ASSERT_EQ(TextureRecordOf(11).PendingUploads.size(), 1u);
    const Uint64 serialBefore = TextureRecordOf(11).Serial;

    // glBindImageTexture: the mask moves and nothing about the storage does.
    MGPResourceDesc masked = allocated;
    masked.BindMask = static_cast<Uint16>(allocated.BindMask | kMGPipeBindShaderImage);
    masked.ImageBindableHint = 1;
    ASSERT_FALSE(MGPipeResourceRespecifyNeedsAck(masked))
        << "a metadata respecify must never ask for a reallocation acknowledgement";
    ASSERT_TRUE(MGPipeApplyResourceRespecify(masked, nullptr));

    EXPECT_EQ(TextureRecordOf(11).Desc.BindMask, masked.BindMask)
        << "the mask this call exists to carry did not reach the record";
    EXPECT_EQ(TextureRecordOf(11).Desc.ImageBindableHint, 1);
    ASSERT_EQ(TextureRecordOf(11).PendingUploads.size(), 1u)
        << "a respecify that redefined no storage ate the texels standing against it";
    EXPECT_EQ(TextureRecordOf(11).PendingUploads[0].UnionBox.W, 64u);
    EXPECT_GT(TextureRecordOf(11).Serial, serialBefore)
        << "the serial is the whole publication of a metadata update - the twin re-derives its "
           "storage flags from the new mask on the strength of it";

    // A SECOND MASK MOVE WITH A NULL LEVEL STILL DROPS NOTHING - the client's mask republish
    // passes null on purpose (wire-v3 §5 item 6) and this is its shape.
    MGPResourceDesc maskedAgain = masked;
    maskedAgain.BindMask = static_cast<Uint16>(masked.BindMask | kMGPipeBindRenderTarget);
    ASSERT_TRUE(MGPipeApplyResourceRespecify(maskedAgain, nullptr, nullptr));
    ASSERT_EQ(TextureRecordOf(11).PendingUploads.size(), 1u)
        << "a metadata update with no level dropped a standing upload";
    EXPECT_EQ(TextureRecordOf(11).Desc.BindMask, maskedAgain.BindMask);

    // BUT A NAMED LEVEL IS DROPPED WHETHER OR NOT THE DESCRIPTOR MOVED (P4a final review C-1,
    // refining the W11 clause that stood here): the pointer is the caller's statement that it
    // reallocated that level, and the descriptor cannot contradict it - a non-base level
    // redefined at a new size moves no descriptor field, so "identical storage fields" says
    // nothing about that level's coordinate system. Level 1's entry goes; level 0's stays.
    ASSERT_TRUE(MGPipeApplyResourceSubData(TextureUpload(texture, 1, MGPBox{0, 0, 0, 32, 32, 1}, 0), texels));
    ASSERT_EQ(TextureRecordOf(11).PendingUploads.size(), 2u);
    const MGPRespecifiedLevel levelOne{kTex2D, 1};
    ASSERT_TRUE(MGPipeApplyResourceRespecify(maskedAgain, nullptr, &levelOne));
    ASSERT_EQ(TextureRecordOf(11).PendingUploads.size(), 1u)
        << "a level-scoped respecify on an unchanged descriptor did not drop the level it named";
    EXPECT_EQ(TextureRecordOf(11).PendingUploads[0].Level, 0u) << "it dropped the wrong level";
    const MGPRespecifiedLevel levelZero{kTex2D, 0};

    // THE NEGATIVE CONTROL, in the same case: move ONE storage-defining field and the same call
    // is a redefinition again, which takes the level it names with it.
    MGPResourceDesc reallocated = maskedAgain;
    reallocated.Width = 32;
    reallocated.Height = 32;
    ASSERT_TRUE(MGPipeApplyResourceRespecify(reallocated, nullptr, &levelZero));
    EXPECT_TRUE(TextureRecordOf(11).PendingUploads.empty())
        << "a 64-wide box survived a redefinition onto a 32-wide level";
    EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 0u);
#endif
}

// D-D5 step 1 again, for the two calls that DEFINE the storage an upload lands in (ID-18 M3).
// The emitter cannot see either refusal from its call site: a dead or stale handle is a counted
// no-op and a corrupt record is a Fatal that deliberately moves no counter, so a create or a
// respecify the applier dropped is indistinguishable from one it took. A client that goes on to
// clear a level's dirty flags, or to advance its own descriptor dedupe, on the strength of
// having emitted has lost those texels for good.
TEST(TextureEmit, TheCreateAndRespecifyCallsAnswerWhetherTheRecordWasAccepted) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPipeHandle texture{12, 1};
    EXPECT_TRUE(MGPipeApplyResourceCreate(TextureDesc(texture, 0, 161)));
    EXPECT_TRUE(MGPipeApplyResourceRespecify(TextureDesc(texture, 64, 161), nullptr));
    ASSERT_TRUE(TextureRecordOf(12).Live);

    // THE RESERVED SLOT is refused by the create, and the answer says so.
    EXPECT_FALSE(MGPipeApplyResourceCreate(TextureDesc(MGPipeHandle{0, 1}, 0, 162)))
        << "resource_create answered accepted for the reserved slot 0";

    // A STALE GENERATION is the one refusal that is not a Fatal, so it is the only one the
    // return can carry, and it is counted on the way out.
    const Uint64 refusedBefore = MGPipeApplier().RefusedResourceCalls;
    MGPipeHandle recycled = texture;
    recycled.Gen = 2;
    EXPECT_FALSE(MGPipeApplyResourceRespecify(TextureDesc(recycled, 64, 161), nullptr))
        << "resource_respecify answered accepted for a handle it refused";
    EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, refusedBefore + 1);
    EXPECT_EQ(TextureRecordOf(12).Desc.Width, 64u) << "a refused respecify moved the record anyway";

    // AND THE BUFFER HALF ANSWERS ON THE SAME TERMS WITH NO BACKEND TABLE REGISTERED. Whether a
    // backend installed MGPipeResourceOps is a property of the BUILD and not of the record; an
    // emitter that read "not accepted" off an unregistered table would re-send a call the
    // applier has already taken responsibility for.
    MGPResourceDesc buffer{};
    buffer.Resource = MGPipeHandle{13, 1};
    buffer.Target = kMGPipeResourceTargetBuffer;
    buffer.GlNameForDiag = 163;
    EXPECT_TRUE(MGPipeApplyResourceCreate(buffer));
    buffer.Width = 256;
    EXPECT_TRUE(MGPipeApplyResourceRespecify(buffer, nullptr));
    MGPResourceDesc deadBuffer = buffer;
    deadBuffer.Resource.Gen = 7;
    EXPECT_FALSE(MGPipeApplyResourceRespecify(deadBuffer, nullptr));
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
#if MOBILEGL_PIPE_PUSH
    // ID-39: A BACKEND IS PRESENT, for the whole binary. Since ID-39 every P4a-family entry
    // point in MG_Pipe/PipeApply.cpp declines a record - and the client's own gate in
    // MG_Impl/Pipe/PipeFill.cpp emits none at all - when no backend has registered
    // MGPipeResourceOps, because acceptance is a contract with the emitter and an accepted
    // record nothing reads makes the client clear a dirty flag the legacy pull path still owed.
    // Every case in this suite is about the arm where a backend DOES consume the records, which
    // is the shipped DirectGLES configuration, so the suite installs the same signal that
    // backend installs. The table is empty because none of its hooks is on a texture path: a
    // non-buffer resource row is stored and returned, never dispatched.
    // WithNoBackendConsumerTheFamilyGateIsFalseAndNothingReachesTheApplier takes it away again.
    static const MGPipeResourceOps kConsumerPresent{};
    MGPipeSetResourceOps(&kConsumerPresent);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
