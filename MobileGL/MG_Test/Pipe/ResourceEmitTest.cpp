// MobileGL - MobileGL/MG_Test/Pipe/ResourceEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P3a's resource family: the applier's record lifecycle and the client's emission of it.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT.
// Two packages fill this file in and neither of them touches MG_Test/Pipe/CMakeLists.txt to
// do it: the applier-side cases (a create marks the slot live, a respecify replaces the
// descriptor and bumps Serial, a destroy clears Live, a stale generation resolves to nothing,
// HasLiveHostWrites is false on every path this phase has, and the sub-data range ENCODING at
// both of its bounds) belong to the branch that gives the entry points their bodies; the
// emitter-side cases (the sticky BindMask over every buffer target, on create AND on a
// following respecify; the slot released at destruction; and the SPLITTER over that encoding,
// which lives in the client's ResourceTracker) belong to the client branch. They are disjoint
// TEST bodies in one file.
//
// THE SUITE IS `ResourceEmit`, not `ResourceEmitTest`: the file is XTest.cpp and the suite is
// X, which is this directory's convention (RenderStateSpansTest.cpp -> RenderStateSpans), and
// it is what the phase's gate greps for (`ctest -R '...|ResourceEmit\.'`).
//
// IT HAS ITS OWN main(), like PipeInputsTest and RenderStateSpansTest, and that is a decision
// taken here so that nobody has to come back to the CMake file for it: the applier's bounds
// gate reports through a trip wire whose verdict is a log line in a shipped push build and
// std::abort() in a poison or verify one, so a case that drives it reads the line back out of
// a file this process points MOBILEGL_LOG_FILE_PATH at before anything logs.
//
// Every case is a visible SKIP in a pull build rather than a vanishing test - the applier is
// compiled only under MOBILEGL_PIPE_PUSH - so `ctest -N` stays name-for-name identical
// between the pull and the push trees.

#include <gtest/gtest.h>

#include <cstring>
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

    std::string ReadLog() {
        std::ifstream in(g_logPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // The op table is INSTALLED BY A BACKEND, at its own bring-up, and uninstalled at its
    // teardown - it is not part of the applier's state and MGPipeApplierReset deliberately
    // does not clear it. A process with no backend in it therefore has none, and that is the
    // fact the whole family's landability rests on: with no table registered every frontend
    // dispatch falls through to the op table this one replaces, so the client half can land
    // on its own without changing a single observable.
    //
    // It is also the negative control for the registration itself. A Set that did not stick
    // would leave the family permanently dark, and nothing else in the tree would say so.
    TEST(ResourceEmit, TheResourceOpTableIsUnregisteredUntilABackendInstallsOne) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ASSERT_EQ(MGPipeGetResourceOps(), nullptr)
            << "something registered a resource op table in a unit-test process";

        static const MGPipeResourceOps ops{};
        MGPipeSetResourceOps(&ops);
        EXPECT_EQ(MGPipeGetResourceOps(), &ops);

        // A state reset is not a teardown: the table survives it, because the backend that
        // installed it is still there.
        MGPipeApplierReset();
        EXPECT_EQ(MGPipeGetResourceOps(), &ops);

        MGPipeSetResourceOps(nullptr);
        EXPECT_EQ(MGPipeGetResourceOps(), nullptr);
#endif
    }

    // =====================================================================================
    // The applier's record lifecycle.
    //
    // WHAT THESE CASES CAN SEE, AND WHY THEY ARE ENOUGH. The applier's whole job in this
    // family is identity, extent and order: which slot is live, what its declared storage is,
    // which calls move its serial, and which calls are refused before a backend is handed a
    // range it would read or write outside that storage. All four are answerable from
    // MGPipeApplier() with no context, no device and no emitter - the emitter's half (a real
    // BufferObject minting a handle, the sticky bind mask, the range splitter) is the client
    // package's, and its cases are appended to this file beside these.
    //
    // THE SPLITTER IS NOT TESTED HERE, deliberately: it lives in the client's ResourceTracker
    // and does not exist yet on this branch. What IS tested here is the thing the splitter is
    // written against - the range ENCODING and its two bounds - so that the case which proves
    // the split is contiguous, non-overlapping and reassembles has a pinned bound to split at.
    // =====================================================================================

#if MOBILEGL_PIPE_PUSH
    // A fresh applier per case, and no table left installed behind one. Every case is its own
    // process under ctest, so this is belt and braces - but running the binary by hand must
    // give the same answers as running it under ctest, or a failure cannot be reproduced.
    //
    // IT TAKES BOTH SCOPES, and that is the point of there being two: MGPipeApplierReset is a
    // make-current and deliberately KEEPS the object records (they describe share-group
    // objects that a context switch does not destroy), so a fixture that wants a genuinely
    // empty applier has to say the other one as well. A test fixture is the one caller in the
    // tree that legitimately means "this applier is going away".
    struct ApplierGuard {
        ApplierGuard() {
            MGPipeSetResourceOps(nullptr);
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
        }
        ~ApplierGuard() {
            MGPipeSetResourceOps(nullptr);
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
        }
    };

    MGPResourceDesc BufferDesc(MGPipeHandle res, Uint32 width, Uint32 glName) {
        MGPResourceDesc desc{};
        desc.Resource = res;
        desc.Width = width;
        desc.GlNameForDiag = glName;
        return desc;
    }

    MGPHandleOnly BufferHandle(MGPipeHandle res) {
        return MGPHandleOnly{res, static_cast<Uint32>(MGPipeKind::Buffer), 0};
    }

    // Built the way the emitter will build it: the destination range goes in through
    // MGPipeSetSubDataBufferRange and nothing else touches the box.
    MGPSubData BufferWrite(MGPipeHandle res, Uint64 offset, Uint64 size) {
        MGPSubData record{};
        record.Res = res;
        EXPECT_TRUE(MGPipeSetSubDataBufferRange(record, offset, size));
        return record;
    }

    // Re-read rather than held: a create can grow the record vector and invalidate a
    // reference taken before it.
    const MGPipeResourceRecord& RecordOf(Uint32 slot) {
        EXPECT_GT(MGPipeApplier().Resources.size(), static_cast<SizeT>(slot));
        return MGPipeApplier().Resources[slot];
    }

    // The backend's half, as a table that only counts. It is what proves the dispatch is BY
    // HANDLE - no frontend object reaches it, and the handle it is given is the one the record
    // names.
    struct SpyState {
        Uint32 Creates = 0;
        Uint32 Respecifies = 0;
        Uint32 SubDatas = 0;
        Uint32 Residents = 0;
        Uint32 Flushes = 0;
        Uint32 Readbacks = 0;
        Uint32 Destroys = 0;
        Uint32 Maps = 0;
        Uint32 Unmaps = 0;
        MGPipeHandle LastHandle = kMGPipeNullHandle;
        Uint64 LastMapSize = 0;
    };
    SpyState g_spy;
    Uint8 g_spyMapTarget = 0;

    void SpyCreate(MGPipeHandle res, const MGPResourceDesc&) {
        ++g_spy.Creates;
        g_spy.LastHandle = res;
    }
    void SpyRespecify(MGPipeHandle res, const MGPResourceDesc&, const void*) {
        ++g_spy.Respecifies;
        g_spy.LastHandle = res;
    }
    void SpySubData(MGPipeHandle res, const MGPSubData&, const void*) {
        ++g_spy.SubDatas;
        g_spy.LastHandle = res;
    }
    void SpyResident(MGPipeHandle res, const MGPSubData&, const void*) {
        ++g_spy.Residents;
        g_spy.LastHandle = res;
    }
    void SpyFlush(MGPipeHandle res, const MGPFlushRange&, const void*) {
        ++g_spy.Flushes;
        g_spy.LastHandle = res;
    }
    void SpyReadback(MGPipeHandle res, const MGPReadback&) {
        ++g_spy.Readbacks;
        g_spy.LastHandle = res;
    }
    void SpyDestroy(MGPipeHandle res) {
        ++g_spy.Destroys;
        g_spy.LastHandle = res;
    }
    void* SpyMap(MGPipeHandle res, Uint64 size, const void*) {
        ++g_spy.Maps;
        g_spy.LastHandle = res;
        g_spy.LastMapSize = size;
        return &g_spyMapTarget;
    }
    void SpyUnmap(MGPipeHandle res) {
        ++g_spy.Unmaps;
        g_spy.LastHandle = res;
    }

    const MGPipeResourceOps kSpyOps{SpyCreate,  SpyRespecify, SpySubData, SpyResident, SpyFlush,
                                    SpyReadback, SpyDestroy,  SpyMap,     SpyUnmap};

#if MGTEST_HAVE_FORK
    struct ChildResult {
        int Status = -1;
        std::string Log;
    };

    // PipeInputsTest's and RenderStateSpansTest's shape, and their reason: gtest's own death
    // tests are not used in this repository. The log file is removed first and the whole of
    // what the child left in it is what comes back, so a second child in one process cannot
    // read the first one's line.
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
#endif // MOBILEGL_PIPE_PUSH

    // A create is emitted from the buffer object's CONSTRUCTOR, so it defines no storage and
    // is not a mutation: it says a resource of this identity exists. Slot 0 is the reserved
    // null handle and never becomes live, whatever a record says.
    TEST(ResourceEmit, ACreateMarksTheSlotLiveAndCarriesItsDescriptor) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{7, 3};
        MGPipeApplyResourceCreate(BufferDesc(res, 0, 41));

        const MGPipeResourceRecord& record = RecordOf(res.Slot);
        EXPECT_TRUE(record.Live);
        EXPECT_EQ(record.Gen, res.Gen);
        EXPECT_EQ(record.Desc.GlNameForDiag, 41u);
        EXPECT_EQ(record.Desc.Width, 0u) << "a create defines no storage; the first respecify does";
        EXPECT_EQ(record.Serial, 0u) << "a create is not a mutation, and a fresh backend twin "
                                        "starts its own synced serial at 0";
        EXPECT_FALSE(record.HasLiveHostWrites);

        // The slots below the named one are reachable and are NOT live: growing the table is
        // not the same as populating it.
        EXPECT_FALSE(MGPipeApplier().Resources[0].Live);
        EXPECT_FALSE(MGPipeApplier().Resources[res.Slot - 1].Live);

        // And the reserved handle is refused rather than made live.
        MGPipeApplyResourceCreate(BufferDesc(kMGPipeNullHandle, 4096, 0));
        EXPECT_FALSE(MGPipeApplier().Resources[0].Live);
#endif
    }

    // The serial is the server-owned MGGen the backend twin compares against instead of
    // mirroring a frontend change serial. Exactly the four mutations move it; a readback and a
    // persistent-map acquisition do not, because neither changes what is in the store.
    TEST(ResourceEmit, ARespecifyReplacesTheDescriptorAndOnlyAMutationMovesTheSerial) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{7, 3};
        const Uint8 bytes[64] = {};
        MGPipeApplyResourceCreate(BufferDesc(res, 0, 41));

        MGPResourceDesc mutableStore = BufferDesc(res, 1024, 41);
        mutableStore.HasDefinedContent = 1;
        MGPipeApplyResourceRespecify(mutableStore, bytes);
        EXPECT_EQ(RecordOf(res.Slot).Desc.Width, 1024u);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 1u);

        // A respecify REPLACES the descriptor - it does not merge into it - so an immutable
        // store that shrinks is described as an immutable store that shrank.
        MGPResourceDesc immutableStore = BufferDesc(res, 512, 41);
        immutableStore.Immutable = 1;
        MGPipeApplyResourceRespecify(immutableStore, nullptr);
        EXPECT_EQ(RecordOf(res.Slot).Desc.Width, 512u);
        EXPECT_EQ(RecordOf(res.Slot).Desc.Immutable, 1u);
        EXPECT_EQ(RecordOf(res.Slot).Desc.HasDefinedContent, 0u);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 2u);

        // The per-record half of the call's kNeedsAck follows the RECORD and not the call:
        // one call serves both idioms and only the synchronous allocation is acknowledged.
        EXPECT_FALSE(MGPipeResourceRespecifyNeedsAck(mutableStore));
        EXPECT_TRUE(MGPipeResourceRespecifyNeedsAck(immutableStore));

        MGPipeApplyResourceSubData(BufferWrite(res, 0, 64), bytes);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 3u);
        MGPipeApplyBufferSubDataResident(BufferWrite(res, 64, 64), bytes);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 4u);
        MGPipeApplyResourceFlushRange(MGPFlushRange{res, 0, 64, 0, 0}, bytes);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 5u);

        // Neither of these two changes the store's contents, so neither may tell the twin its
        // memo is stale and buy a re-upload of what it just read.
        MGPipeApplyResourceReadback(MGPReadback{res, 0, 512});
        EXPECT_EQ(RecordOf(res.Slot).Serial, 5u);
        MGPipeApplyMapPersistent(BufferHandle(res), 512, bytes);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 5u);
#endif
    }

    // A destroy drops the record and keeps the generation, because the CLIENT allocator owns
    // the bump and takes it on the next handout of the slot. What the destroyed handle names
    // afterwards is nothing at all - including after the slot has been handed out again, which
    // is the ABA shape a raw address cannot express.
    TEST(ResourceEmit, ADestroyDropsTheRecordAndAStaleGenerationResolvesToNothing) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle first{7, 3};
        MGPipeApplyResourceCreate(BufferDesc(first, 0, 41));
        MGPipeApplyResourceRespecify(BufferDesc(first, 256, 41), nullptr);
        ASSERT_EQ(RecordOf(first.Slot).Serial, 1u);

        MGPipeApplyResourceDestroy(BufferHandle(first));
        EXPECT_FALSE(RecordOf(first.Slot).Live);
        EXPECT_EQ(RecordOf(first.Slot).Gen, first.Gen) << "the generation is the client's to bump";
        EXPECT_EQ(RecordOf(first.Slot).Desc.Width, 0u) << "a stale read of a destroyed slot must "
                                                          "find nothing, not the old extent";
        EXPECT_EQ(RecordOf(first.Slot).Serial, 0u);

        // The dead handle now resolves to nothing, and a mutation on it is dropped rather than
        // applied to whatever is at that slot.
        MGPipeApplyResourceRespecify(BufferDesc(first, 4096, 41), nullptr);
        MGPipeApplyResourceSubData(BufferWrite(first, 0, 16), nullptr);
        EXPECT_FALSE(RecordOf(first.Slot).Live);
        EXPECT_EQ(RecordOf(first.Slot).Desc.Width, 0u);
        EXPECT_EQ(RecordOf(first.Slot).Serial, 0u);

        // The same slot at the next generation is a DIFFERENT resource and starts over.
        const MGPipeHandle second{7, 4};
        MGPipeApplyResourceCreate(BufferDesc(second, 0, 99));
        EXPECT_TRUE(RecordOf(second.Slot).Live);
        EXPECT_EQ(RecordOf(second.Slot).Gen, second.Gen);
        EXPECT_EQ(RecordOf(second.Slot).Serial, 0u);
        EXPECT_EQ(RecordOf(second.Slot).Desc.GlNameForDiag, 99u);

        // And the predecessor's handle still resolves to nothing OVER the live record - the
        // generation compare is what stops a buffer at a recycled address from inheriting its
        // predecessor's calls.
        MGPipeApplyResourceRespecify(BufferDesc(first, 4096, 41), nullptr);
        EXPECT_EQ(RecordOf(second.Slot).Desc.Width, 0u);
        EXPECT_EQ(RecordOf(second.Slot).Desc.GlNameForDiag, 99u);
        EXPECT_EQ(RecordOf(second.Slot).Serial, 0u);
#endif
    }

    // HasLiveHostWrites is ALWAYS false in this phase and is written by nobody: it exists so
    // the phase that pushes persistent-mapped host writes can set it with no new record kind,
    // and a verify build refuses to let a producer land under it unannounced. This case walks
    // every path this phase has and pins that none of them is one.
    TEST(ResourceEmit, NoResourcePathInThisPhaseLeavesHostWritesLive) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{2, 1};
        const Uint8 bytes[64] = {};

        MGPipeApplyResourceCreate(BufferDesc(res, 0, 5));
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "resource_create";
        MGPipeApplyResourceRespecify(BufferDesc(res, 256, 5), bytes);
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "resource_respecify";
        MGPipeApplyResourceSubData(BufferWrite(res, 0, 64), bytes);
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "resource_subdata";
        MGPipeApplyBufferSubDataResident(BufferWrite(res, 64, 64), bytes);
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "buffer_subdata_resident";
        MGPipeApplyResourceFlushRange(MGPFlushRange{res, 0, 64, 0, 0}, bytes);
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "resource_flush_range";
        MGPipeApplyResourceReadback(MGPReadback{res, 0, 256});
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "resource_readback";
        // The persistent map is the one that WOULD set it in a later phase, and does not here.
        MGPipeApplyMapPersistent(BufferHandle(res), 256, bytes);
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "map_persistent";
        MGPipeApplyUnmapPersistent(BufferHandle(res));
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "unmap_persistent";
        MGPipeApplyResourceDestroy(BufferHandle(res));
        EXPECT_FALSE(RecordOf(res.Slot).HasLiveHostWrites) << "resource_destroy";

        // And an applier that is going away carries none of it over either. (A make-current on
        // its own does NOT empty the table - see
        // TheObjectRecordsSurviveAMakeCurrentAndOnlyTheWorkingStateIsReset.)
        MGPipeApplierReleaseObjectRecords();
        EXPECT_TRUE(MGPipeApplier().Resources.empty());
#endif
    }

    // The buffer half of MGPSubData is a convention over a texture record's box, and
    // MGPipeSetSubDataBufferRange is its only encoder. Its two bounds are what the emitter
    // splits against, so they are pinned here exactly - one byte on either side of each.
    //
    // This case is NOT push-gated: the encoding is an inline function of the payload header
    // and exists in every build, so pinning it in the pull build too costs nothing and keeps
    // the bound honest for the transport that will read it.
    TEST(ResourceEmit, TheSubDataRangeEncodingRefusesExactlyAtItsTwoBounds) {
        MGPSubData record{};

        // The last encodable offset and the last encodable size are ACCEPTED, and both survive
        // the round trip through the box.
        EXPECT_TRUE(MGPipeSetSubDataBufferRange(record, 0x7FFFFFFFull, 0));
        EXPECT_EQ(MGPipeSubDataBufferOffset(record), 0x7FFFFFFFull);
        EXPECT_EQ(MGPipeSubDataBufferSize(record), 0u);
        EXPECT_TRUE(MGPipeSetSubDataBufferRange(record, 0, 0xFFFFFFFFull));
        EXPECT_EQ(MGPipeSubDataBufferOffset(record), 0u);
        EXPECT_EQ(MGPipeSubDataBufferSize(record), 0xFFFFFFFFull);
        EXPECT_TRUE(MGPipeSetSubDataBufferRange(record, 0x7FFFFFFFull, 0xFFFFFFFFull));

        // The box shape the applier's gate holds the record to, written by the encoder itself.
        EXPECT_EQ(record.Level, 0u);
        EXPECT_EQ(record.RegionCount, 0u);

        // One byte past either bound is REFUSED - and the record is left untouched, which is
        // what lets the emitter split against the very record it just tried.
        MGPSubData untouched = record;
        EXPECT_FALSE(MGPipeSetSubDataBufferRange(record, 0x80000000ull, 0));
        EXPECT_FALSE(MGPipeSetSubDataBufferRange(record, 0, 0x100000000ull));
        EXPECT_FALSE(MGPipeSetSubDataBufferRange(record, 0x80000000ull, 0x100000000ull));
        EXPECT_EQ(std::memcmp(&record, &untouched, sizeof(record)), 0)
            << "a refused encoding must not half-write the record";

        // A whole-buffer range at the size bound is the shape the splitter's own case will
        // start from; the split itself is the client emitter's and is asserted beside it.
        EXPECT_TRUE(MGPipeSetSubDataBufferRange(record, 0, 0xFFFFFFFFull));
    }

    // BEHAVIOUR NEUTRALITY, ASSERTED RATHER THAN ASSUMED. Every dispatch in the family is a
    // null check that falls through while no backend has installed a table - which is what
    // lets the client half land without changing a single observable - and every one of them
    // hands the backend a HANDLE and a payload, never a frontend object.
    TEST(ResourceEmit, EveryResourceCallDispatchesByHandleThroughTheInstalledTableOnly) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{4, 9};
        const Uint8 bytes[64] = {};
        g_spy = SpyState{};

        // With nothing installed the records still move and nothing is called.
        MGPipeApplyResourceCreate(BufferDesc(res, 0, 12));
        MGPipeApplyResourceRespecify(BufferDesc(res, 256, 12), bytes);
        MGPipeApplyResourceSubData(BufferWrite(res, 0, 64), bytes);
        MGPipeApplyBufferSubDataResident(BufferWrite(res, 64, 64), bytes);
        MGPipeApplyResourceFlushRange(MGPFlushRange{res, 0, 64, 0, 0}, bytes);
        MGPipeApplyResourceReadback(MGPReadback{res, 0, 256});
        EXPECT_EQ(MGPipeApplyMapPersistent(BufferHandle(res), 256, bytes), nullptr)
            << "an unregistered table declines every acquisition, which is a real answer";
        MGPipeApplyUnmapPersistent(BufferHandle(res));
        EXPECT_EQ(RecordOf(res.Slot).Serial, 4u);
        EXPECT_EQ(g_spy.Creates + g_spy.Respecifies + g_spy.SubDatas + g_spy.Residents + g_spy.Flushes +
                      g_spy.Readbacks + g_spy.Destroys + g_spy.Maps + g_spy.Unmaps,
                  0u);

        // Installed, every hook is reached exactly once and with this resource's handle.
        MGPipeSetResourceOps(&kSpyOps);
        const MGPipeHandle other{5, 1};
        MGPipeApplyResourceCreate(BufferDesc(other, 0, 13));
        EXPECT_EQ(g_spy.Creates, 1u);
        EXPECT_EQ(g_spy.LastHandle, other);
        MGPipeApplyResourceRespecify(BufferDesc(other, 256, 13), bytes);
        EXPECT_EQ(g_spy.Respecifies, 1u);
        MGPipeApplyResourceSubData(BufferWrite(other, 0, 64), bytes);
        EXPECT_EQ(g_spy.SubDatas, 1u);
        MGPipeApplyBufferSubDataResident(BufferWrite(other, 64, 64), bytes);
        EXPECT_EQ(g_spy.Residents, 1u);
        MGPipeApplyResourceFlushRange(MGPFlushRange{other, 0, 64, 0, 0}, bytes);
        EXPECT_EQ(g_spy.Flushes, 1u);
        MGPipeApplyResourceReadback(MGPReadback{other, 0, 256});
        EXPECT_EQ(g_spy.Readbacks, 1u);
        EXPECT_EQ(MGPipeApplyMapPersistent(BufferHandle(other), 256, bytes), &g_spyMapTarget)
            << "the donated pointer is the owner's answer and travels back unchanged";
        EXPECT_EQ(g_spy.LastMapSize, 256u);
        MGPipeApplyUnmapPersistent(BufferHandle(other));
        EXPECT_EQ(g_spy.Unmaps, 1u);
        MGPipeApplyResourceDestroy(BufferHandle(other));
        EXPECT_EQ(g_spy.Destroys, 1u);
        EXPECT_EQ(g_spy.LastHandle, other);
        EXPECT_FALSE(RecordOf(other.Slot).Live);

        // Uninstalled again - a backend teardown - and the family goes dark without taking the
        // applier's records with it.
        MGPipeSetResourceOps(nullptr);
        MGPipeApplyResourceSubData(BufferWrite(res, 0, 64), bytes);
        EXPECT_EQ(g_spy.SubDatas, 1u);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 5u);
#endif
    }

    // map-persistent-roundtrips counts every ACQUISITION ATTEMPT, mint or decline, because
    // every one of them needs an answer from the resource owner. Defined as "round trips
    // actually taken" it would be 0 by construction in the monolith and could never go red;
    // defined this way the number is the same in both modes, is one per storage definition,
    // and is assertable today.
    TEST(ResourceEmit, MapPersistentCountsEveryAttemptWhetherItMintsOrDeclines) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{3, 2};
        const Uint8 bytes[64] = {};
        g_spy = SpyState{};
        EXPECT_EQ(MGPipeApplier().MapPersistentRoundtrips, 0u);

        MGPipeApplyResourceCreate(BufferDesc(res, 0, 8));
        MGPipeApplyResourceRespecify(BufferDesc(res, 256, 8), bytes);
        EXPECT_EQ(MGPipeApplier().MapPersistentRoundtrips, 0u) << "a storage definition is not an "
                                                                  "acquisition";

        // Three declines still cost three answers.
        for (Uint32 i = 0; i < 3; ++i) EXPECT_EQ(MGPipeApplyMapPersistent(BufferHandle(res), 256, bytes), nullptr);
        EXPECT_EQ(MGPipeApplier().MapPersistentRoundtrips, 3u);

        // A mint costs the same one.
        MGPipeSetResourceOps(&kSpyOps);
        EXPECT_EQ(MGPipeApplyMapPersistent(BufferHandle(res), 256, bytes), &g_spyMapTarget);
        EXPECT_EQ(MGPipeApplier().MapPersistentRoundtrips, 4u);
        EXPECT_EQ(g_spy.Maps, 1u);

        // And so does an attempt on a resource the applier does not have: the client asked,
        // and asking is what the counter counts.
        MGPipeApplyResourceDestroy(BufferHandle(res));
        EXPECT_EQ(MGPipeApplyMapPersistent(BufferHandle(res), 256, bytes), nullptr);
        EXPECT_EQ(MGPipeApplier().MapPersistentRoundtrips, 5u);
        EXPECT_EQ(g_spy.Maps, 1u) << "a refused handle must not reach the backend";

        // Per context, like every other member of the applier's state.
        MGPipeApplierReset();
        EXPECT_EQ(MGPipeApplier().MapPersistentRoundtrips, 0u);
#endif
    }

    // THE BOUNDS GATE, AND THE REASON IT IS A TRIP WIRE RATHER THAN A DROPPED CALL: a record
    // whose range runs past the storage it names would have the BACKEND read or write outside
    // a store. That is the one class of fault ARCHITECTURE.md reserves Fatal{ProtocolCorruption}
    // for, and the line carries the record's identity so the resource can be named without a
    // second run.
    //
    // A poison or verify build stops the process, so the drive is a forked child there and the
    // parent reads SIGABRT and the line out of the log; a shipped push build logs and carries
    // on from a defined state, so that build asserts the same line plus the fact that the
    // refused write moved nothing.
    TEST(ResourceEmit, AWriteOutsideTheDeclaredStorageIsRefusedNamingTheResource) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{7, 3};
        const Uint8 bytes[256] = {};
        MGPipeApplyResourceCreate(BufferDesc(res, 0, 41));
        MGPipeApplyResourceRespecify(BufferDesc(res, 256, 41), nullptr);

        // The positive control first: a write that ends EXACTLY at the declared extent is
        // accepted, so the gate is refusing the range and not the arithmetic around it.
        MGPipeApplyResourceSubData(BufferWrite(res, 192, 64), bytes);
        ASSERT_EQ(RecordOf(res.Slot).Serial, 2u);

        // Encoded HERE and not inside the child: a forked child must not run a gtest assertion,
        // and BufferWrite carries one.
        MGPSubData pastTheEnd{};
        pastTheEnd.Res = res;
        ASSERT_TRUE(MGPipeSetSubDataBufferRange(pastTheEnd, 200, 64));

#if MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY
#if MGTEST_HAVE_FORK
        const ChildResult child =
            RunInChild([&pastTheEnd, &bytes]() { MGPipeApplyResourceSubData(pastTheEnd, bytes); });
        EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child) << "; log: " << child.Log;
        EXPECT_NE(child.Log.find("Fatal{ProtocolCorruption} resource_subdata {slot=7, gen=3, glName=41}"),
                  std::string::npos)
            << "the gate fired without naming the record it refused; log: " << child.Log;
#else
        GTEST_SKIP() << "no fork on this platform; the gate's verdict here is std::abort()";
#endif
#else
        const std::string before = ReadLog();
        MGPipeApplyResourceSubData(pastTheEnd, bytes);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 2u) << "a refused write must not move the serial";
        EXPECT_NE(ReadLog().substr(before.size()).find("ProtocolCorruption resource_subdata {slot=7, gen=3, "
                                                       "glName=41}"),
                  std::string::npos)
            << "the gate refused the write without saying which record it was";
#endif
#endif
    }

    // =====================================================================================
    // The applier's VERTEX-INPUT bodies, and the two scopes of a reset.
    //
    // WHY THESE ARE HERE AND NOT IN VertexInputEmitTest.cpp. C.5 gives that file's contents to
    // the client package, which is appending its conversion cases to it now; these are the
    // APPLIER's own cases and they belong to this branch, so they are appended beside the
    // resource ones instead of colliding with an edit in flight. They need no emitter, no
    // context and no device - they are direct calls into the five entry points, exactly the
    // shape the resource cases above already use.
    //
    // Each one is written so that DELETING the line of the applier it is about turns it red:
    // the two blob memcpys, the set_vertex_buffers entry loop, the Start + Count window gate,
    // the counts/Blob.Size gate, the two BufferRangeFault calls and SubDataBoxFault's Level
    // arm all have a case here that fails by field or by name when they are removed.
    // =====================================================================================

#if MOBILEGL_PIPE_PUSH
    MGPHandleOnly ElementsHandle(MGPipeHandle cso) {
        return MGPHandleOnly{cso, static_cast<Uint32>(MGPipeKind::VertexElementsCso), 0};
    }

    const MGPipeVertexElementsRecord& ElementsOf(Uint32 slot) {
        EXPECT_GT(MGPipeApplier().VertexElementsCsos.size(), static_cast<SizeT>(slot));
        return MGPipeApplier().VertexElementsCsos[slot];
    }

    // Every field of both wire views carries a value derived from its own index, so a copy
    // that lands in the wrong slot - or does not land at all - is visible BY FIELD rather than
    // by a count, which is what the family's negative control needs of it.
    MGPVertexAttribWire AttribAt(Uint32 i) {
        MGPVertexAttribWire wire{};
        wire.Offset = 0x1000ull + i;
        wire.Stride = static_cast<Int32>(64 + i);
        wire.Type = 0x1400u + i;
        wire.Size = static_cast<Uint8>(1 + (i % 4));
        wire.Enabled = static_cast<Uint8>(i % 2);
        wire.Normalized = static_cast<Uint8>((i + 1) % 2);
        wire.IsInteger = static_cast<Uint8>((i % 3) == 0 ? 1 : 0);
        wire.IsLong = static_cast<Uint8>((i % 5) == 0 ? 1 : 0);
        wire.IsBgra = static_cast<Uint8>((i % 7) == 0 ? 1 : 0);
        wire.BindingIndex = static_cast<Uint8>((i * 3) % kMGPipeMaxVertexAttribs);
        return wire;
    }

    MGPVertexBindingPointWire BindingAt(Uint32 i) {
        MGPVertexBindingPointWire wire{};
        wire.Offset = 0x2000ull + i;
        wire.Stride = static_cast<Int32>(16 + i);
        wire.Divisor = i * 2;
        return wire;
    }

    void ExpectAttribEq(const MGPVertexAttribWire& got, const MGPVertexAttribWire& want, Uint32 i) {
        EXPECT_EQ(got.Offset, want.Offset) << "attribute " << i << ": Offset";
        EXPECT_EQ(got.Stride, want.Stride) << "attribute " << i << ": Stride";
        EXPECT_EQ(got.Type, want.Type) << "attribute " << i << ": Type";
        EXPECT_EQ(got.Size, want.Size) << "attribute " << i << ": Size";
        EXPECT_EQ(got.Enabled, want.Enabled) << "attribute " << i << ": Enabled";
        EXPECT_EQ(got.Normalized, want.Normalized) << "attribute " << i << ": Normalized";
        EXPECT_EQ(got.IsInteger, want.IsInteger) << "attribute " << i << ": IsInteger";
        EXPECT_EQ(got.IsLong, want.IsLong) << "attribute " << i << ": IsLong";
        EXPECT_EQ(got.IsBgra, want.IsBgra) << "attribute " << i << ": IsBgra";
        EXPECT_EQ(got.BindingIndex, want.BindingIndex) << "attribute " << i << ": BindingIndex";
    }

    void ExpectBindingEq(const MGPVertexBindingPointWire& got, const MGPVertexBindingPointWire& want, Uint32 i) {
        EXPECT_EQ(got.Offset, want.Offset) << "binding point " << i << ": Offset";
        EXPECT_EQ(got.Stride, want.Stride) << "binding point " << i << ": Stride";
        EXPECT_EQ(got.Divisor, want.Divisor) << "binding point " << i << ": Divisor";
    }

    void ExpectVertexBufferEq(const MGPVertexBuffer& got, const MGPVertexBuffer& want, Uint32 i) {
        EXPECT_EQ(got.Res, want.Res) << "vertex buffer " << i << ": Res";
        EXPECT_EQ(got.Offset, want.Offset) << "vertex buffer " << i << ": Offset";
        EXPECT_EQ(got.Stride, want.Stride) << "vertex buffer " << i << ": Stride";
        EXPECT_EQ(got.Divisor, want.Divisor) << "vertex buffer " << i << ": Divisor";
        EXPECT_EQ(got.BindingIndex, want.BindingIndex) << "vertex buffer " << i << ": BindingIndex";
    }

    // The blob laid out exactly as create_vertex_elements declares it: the attribute wires
    // first, then the binding-point wires, both in ascending index order. `declareBlobSize`
    // picks which half of the Blob rule the record is exercising - a transport that fills the
    // length in, or a monolith emission that leaves it 0 and carries the bytes beside it.
    struct ElementsBlob {
        Vector<Uint8> Bytes;
        MGPVertexElements Desc{};
        const void* Data() const { return Bytes.empty() ? nullptr : Bytes.data(); }
    };

    ElementsBlob MakeElements(MGPipeHandle cso, Uint32 attributes, Uint32 bindings, Bool declareBlobSize) {
        ElementsBlob out;
        out.Bytes.resize(attributes * sizeof(MGPVertexAttribWire) + bindings * sizeof(MGPVertexBindingPointWire));
        for (Uint32 i = 0; i < attributes; ++i) {
            const MGPVertexAttribWire wire = AttribAt(i);
            std::memcpy(out.Bytes.data() + i * sizeof(wire), &wire, sizeof(wire));
        }
        for (Uint32 i = 0; i < bindings; ++i) {
            const MGPVertexBindingPointWire wire = BindingAt(i);
            std::memcpy(out.Bytes.data() + attributes * sizeof(MGPVertexAttribWire) + i * sizeof(wire), &wire,
                        sizeof(wire));
        }
        out.Desc.Cso = cso;
        out.Desc.AttributeCount = attributes;
        out.Desc.BindingPointCount = bindings;
        out.Desc.Blob.Size = declareBlobSize ? static_cast<Uint64>(out.Bytes.size()) : 0;
        return out;
    }

    // Drives a call that a trip wire must REFUSE, and asserts the wire named what it refused.
    // The two arms are this file's existing ones and the tag differs between them by design:
    // a poison or verify build stops the process, so the drive is a forked child and the
    // parent reads SIGABRT and the line out of the log; a shipped push build logs
    // `ProtocolCorruption` and carries on from a defined state, so there the line is read back
    // in process and the caller goes on to assert that nothing moved.
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

    // C1. A make-current is NOT a teardown. MGPipeApplierReset runs at every change of the
    // current context - including a make-current back to a context that is still alive - and a
    // GL object lives in a SHARE GROUP, not in a context. So the working state goes and the
    // object records stay: a buffer created before the switch is the same buffer with the same
    // storage after it, and the write that follows must land rather than resolve to nothing.
    // Only the applier's own teardown takes the records.
    TEST(ResourceEmit, TheObjectRecordsSurviveAMakeCurrentAndOnlyTheWorkingStateIsReset) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{7, 3};
        const MGPipeHandle cso{2, 1};
        const Uint8 bytes[256] = {};

        MGPipeApplyResourceCreate(BufferDesc(res, 0, 41));
        MGPipeApplyResourceRespecify(BufferDesc(res, 256, 41), nullptr);
        MGPipeApplyResourceSubData(BufferWrite(res, 0, 64), bytes);
        ASSERT_EQ(RecordOf(res.Slot).Serial, 2u);

        const ElementsBlob elements = MakeElements(cso, 4, 2, true);
        MGPipeApplyCreateVertexElements(elements.Desc, elements.Data());
        MGPipeApplyBindVertexElements(ElementsHandle(cso));
        MGPVertexBuffers hdr{};
        hdr.Count = 1;
        hdr.BaseInstance = 9;
        MGPVertexBuffer entry{};
        entry.Res = res;
        entry.Stride = 12;
        MGPipeApplySetVertexBuffers(hdr, &entry);
        MGPipeApplySetIndexBuffer(MGPIndexBuffer{res, 64, 2, 0});
        const Uint64 vertexBuffersSerial = MGPipeApplier().VertexBuffersSerial;
        const Uint64 indexBufferSerial = MGPipeApplier().IndexBufferSerial;

        MGPipeApplierReset(); // the make-current

        // The WORKING state is gone, and the two serials moved FORWARD rather than back to 0.
        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundVertexElements));
        EXPECT_EQ(MGPipeApplier().VertexBufferCount, 0u);
        EXPECT_EQ(MGPipeApplier().VertexFetchBaseInstance, 0u);
        EXPECT_EQ(MGPipeApplier().IndexBuffer.IndexSize, 0u);
        EXPECT_GT(MGPipeApplier().VertexBuffersSerial, vertexBuffersSerial);
        EXPECT_GT(MGPipeApplier().IndexBufferSerial, indexBufferSerial);

        // The OBJECT RECORDS are not, and this is the whole of C1: the context switch
        // destroyed no buffer, so the record that carries this store's extent and its mutation
        // serial - the two facts the backend's draw-clean memo is re-keyed onto - is still here.
        ASSERT_TRUE(RecordOf(res.Slot).Live) << "a make-current dropped a share-group object's record";
        EXPECT_EQ(RecordOf(res.Slot).Desc.Width, 256u);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 2u) << "the record's serial is not working state";

        MGPipeApplyResourceSubData(BufferWrite(res, 64, 64), bytes);
        EXPECT_EQ(RecordOf(res.Slot).Serial, 3u) << "the first write after a make-current was dropped";
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 0u) << "and it was dropped silently";

        // Same for the vertex-elements CSO: it can be re-bound without being re-created.
        ASSERT_TRUE(ElementsOf(cso.Slot).Live);
        EXPECT_EQ(ElementsOf(cso.Slot).AttributeCount, 4u);
        EXPECT_EQ(ElementsOf(cso.Slot).ContentSerial, 1u);
        MGPipeApplyBindVertexElements(ElementsHandle(cso));
        EXPECT_EQ(MGPipeApplier().BoundVertexElements, cso);
        EXPECT_EQ(MGPipeApplier().RefusedVertexInputCalls, 0u);

        // The other scope: the served context is going away and the applier with it.
        MGPipeApplierReleaseObjectRecords();
        EXPECT_TRUE(MGPipeApplier().Resources.empty());
        EXPECT_TRUE(MGPipeApplier().VertexElementsCsos.empty());
        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundVertexElements));
#endif
    }

    // C1's observable. A call that names a record this applier does not have is a DEFINED
    // no-op - nothing stored, nothing dispatched, no serial moved - because the teardown order
    // makes exactly one such sequence legal (release the records, then every ~BufferObject
    // sends its death notice into them). But MOBILEGL_ASSERT compiles out at INFO, which is
    // what all three gate builds and every shipped build are, so a no-op alone would make a
    // dropped glBufferSubData invisible everywhere it matters. It is counted instead.
    TEST(ResourceEmit, ACallOnARecordTheApplierDoesNotHaveIsCountedRatherThanSilentlyDropped) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{7, 3};
        const MGPipeHandle cso{2, 1};
        const Uint8 bytes[64] = {};

        // A legal sequence leaves both counters at 0 - which is what makes a non-zero one
        // evidence rather than noise.
        MGPipeApplyResourceCreate(BufferDesc(res, 0, 41));
        MGPipeApplyResourceRespecify(BufferDesc(res, 256, 41), nullptr);
        MGPipeApplyResourceSubData(BufferWrite(res, 0, 64), bytes);
        MGPipeApplyResourceFlushRange(MGPFlushRange{res, 0, 64, 0, 0}, bytes);
        MGPipeApplyResourceReadback(MGPReadback{res, 0, 256});
        const ElementsBlob elements = MakeElements(cso, 2, 1, true);
        MGPipeApplyCreateVertexElements(elements.Desc, elements.Data());
        MGPipeApplyBindVertexElements(ElementsHandle(cso));
        ASSERT_EQ(MGPipeApplier().RefusedResourceCalls, 0u);
        ASSERT_EQ(MGPipeApplier().RefusedVertexInputCalls, 0u);

        // The destroy is legal; everything that names the handle afterwards is not, and every
        // one of them is counted.
        MGPipeApplyResourceDestroy(BufferHandle(res));
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 0u) << "the destroy itself named a live record";

        MGPipeApplyResourceRespecify(BufferDesc(res, 4096, 41), nullptr);
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 1u) << "resource_respecify";
        MGPipeApplyResourceSubData(BufferWrite(res, 0, 64), bytes);
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 2u) << "resource_subdata";
        MGPipeApplyBufferSubDataResident(BufferWrite(res, 0, 64), bytes);
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 3u) << "buffer_subdata_resident";
        MGPipeApplyResourceFlushRange(MGPFlushRange{res, 0, 64, 0, 0}, bytes);
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 4u) << "resource_flush_range";
        MGPipeApplyResourceReadback(MGPReadback{res, 0, 64});
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 5u) << "resource_readback";
        EXPECT_EQ(MGPipeApplyMapPersistent(BufferHandle(res), 64, bytes), nullptr);
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 6u) << "map_persistent";
        MGPipeApplyUnmapPersistent(BufferHandle(res));
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 7u) << "unmap_persistent";
        MGPipeApplyResourceDestroy(BufferHandle(res));
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 8u) << "resource_destroy on an already-dead record";

        // A slot the table has never grown to is the same refusal and not a resize.
        const SizeT tableSize = MGPipeApplier().Resources.size();
        MGPipeApplyResourceSubData(BufferWrite(MGPipeHandle{4096, 1}, 0, 4), bytes);
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 9u) << "an unknown slot";
        EXPECT_EQ(MGPipeApplier().Resources.size(), tableSize) << "a refusal must not grow the table";

        // The vertex-input family keeps its own count, and the delete that drops a record is
        // legal exactly once.
        MGPipeApplyDeleteVertexElements(ElementsHandle(cso));
        EXPECT_EQ(MGPipeApplier().RefusedVertexInputCalls, 0u);
        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundVertexElements))
            << "a delete must clear a binding that named the record it dropped";
        MGPipeApplyBindVertexElements(ElementsHandle(cso));
        EXPECT_EQ(MGPipeApplier().RefusedVertexInputCalls, 1u) << "bind_vertex_elements";
        MGPipeApplyDeleteVertexElements(ElementsHandle(cso));
        EXPECT_EQ(MGPipeApplier().RefusedVertexInputCalls, 2u) << "delete_vertex_elements";

        // Both are per context, like the four render-state wire counters beside them.
        MGPipeApplierReset();
        EXPECT_EQ(MGPipeApplier().RefusedResourceCalls, 0u);
        EXPECT_EQ(MGPipeApplier().RefusedVertexInputCalls, 0u);
#endif
    }

    // The blob unpack, over ALL 32 attribute and 32 binding-point slots, and the shrink that
    // has to leave nothing of the configuration before it. Deleting either memcpy, or the two
    // zeroing lines that precede them, fails this case by field name.
    TEST(ResourceEmit, AVertexElementsBlobRoundTripsAndAShrinkLeavesNothingOfTheOneBeforeIt) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle cso{3, 1};
        const ElementsBlob full = MakeElements(cso, kMGPipeMaxVertexAttribs, kMGPipeMaxVertexAttribs, true);
        MGPipeApplyCreateVertexElements(full.Desc, full.Data());

        ASSERT_TRUE(ElementsOf(cso.Slot).Live);
        EXPECT_EQ(ElementsOf(cso.Slot).Gen, cso.Gen);
        EXPECT_EQ(ElementsOf(cso.Slot).AttributeCount, kMGPipeMaxVertexAttribs);
        EXPECT_EQ(ElementsOf(cso.Slot).BindingPointCount, kMGPipeMaxVertexAttribs);
        EXPECT_EQ(ElementsOf(cso.Slot).ContentSerial, 1u)
            << "the first create of an identity lands on 1, so 0 means never created";
        for (Uint32 i = 0; i < kMGPipeMaxVertexAttribs; ++i) {
            ExpectAttribEq(ElementsOf(cso.Slot).Attributes[i], AttribAt(i), i);
            ExpectBindingEq(ElementsOf(cso.Slot).BindingPoints[i], BindingAt(i), i);
        }

        // A RE-CREATE on the same handle is how a configuration change travels: the serial
        // counts up and the entries above the new counts describe nothing at all.
        const ElementsBlob small = MakeElements(cso, 2, 1, true);
        MGPipeApplyCreateVertexElements(small.Desc, small.Data());
        EXPECT_EQ(ElementsOf(cso.Slot).ContentSerial, 2u) << "a re-create on the same handle counts up";
        EXPECT_EQ(ElementsOf(cso.Slot).AttributeCount, 2u);
        EXPECT_EQ(ElementsOf(cso.Slot).BindingPointCount, 1u);
        for (Uint32 i = 0; i < 2; ++i) ExpectAttribEq(ElementsOf(cso.Slot).Attributes[i], AttribAt(i), i);
        ExpectBindingEq(ElementsOf(cso.Slot).BindingPoints[0], BindingAt(0), 0);
        const MGPVertexAttribWire zeroAttrib{};
        const MGPVertexBindingPointWire zeroBinding{};
        for (Uint32 i = 2; i < kMGPipeMaxVertexAttribs; ++i) {
            ExpectAttribEq(ElementsOf(cso.Slot).Attributes[i], zeroAttrib, i);
        }
        for (Uint32 i = 1; i < kMGPipeMaxVertexAttribs; ++i) {
            ExpectBindingEq(ElementsOf(cso.Slot).BindingPoints[i], zeroBinding, i);
        }
        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundVertexElements))
            << "a create must not rebind; it changes what the binding points at";

        // A create at a RECYCLED slot is a different resource and starts over, which is what
        // lets the backend twin key on the handle and the serial together.
        const MGPipeHandle recycled{3, 2};
        const ElementsBlob other = MakeElements(recycled, 1, 1, true);
        MGPipeApplyCreateVertexElements(other.Desc, other.Data());
        EXPECT_EQ(ElementsOf(recycled.Slot).Gen, recycled.Gen);
        EXPECT_EQ(ElementsOf(recycled.Slot).ContentSerial, 1u) << "a recycled slot starts over";
        EXPECT_EQ(ElementsOf(recycled.Slot).AttributeCount, 1u);
#endif
    }

    // The counts/blob gate, in both build arms, plus the half of the Blob rule that says a
    // record which declares NO length is not a fault: 0 means "this record does not declare
    // its blob", which is what a monolith emission is, and the counts are what bound the read.
    TEST(ResourceEmit, AVertexElementsRecordThatDoesNotDescribeItsOwnBlobIsRefusedNamingIt) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle cso{5, 2};

        // Positive controls: the declared length agrees, and then is not declared at all.
        const ElementsBlob declared = MakeElements(cso, 3, 2, true);
        MGPipeApplyCreateVertexElements(declared.Desc, declared.Data());
        ASSERT_EQ(ElementsOf(cso.Slot).ContentSerial, 1u);
        const ElementsBlob undeclared = MakeElements(cso, 3, 2, false);
        MGPipeApplyCreateVertexElements(undeclared.Desc, undeclared.Data());
        ASSERT_EQ(ElementsOf(cso.Slot).ContentSerial, 2u) << "a zero Blob.Size is a monolith emission, "
                                                             "not a fault";

        // A NON-ZERO length that is not the one the counts describe.
        MGPVertexElements shortBlob = declared.Desc;
        shortBlob.Blob.Size -= 1;
        const void* blobBytes = declared.Data();
        ExpectRefusedNaming("create_vertex_elements {slot=5, gen=2}: the declared blob length is not the "
                            "byte length the two counts describe",
                            [&shortBlob, blobBytes]() { MGPipeApplyCreateVertexElements(shortBlob, blobBytes); });
        EXPECT_EQ(ElementsOf(cso.Slot).ContentSerial, 2u) << "a refused record must not move the serial";
        EXPECT_EQ(ElementsOf(cso.Slot).AttributeCount, 3u) << "nor replace the configuration before it";

        // And a count above the destination it would be unpacked into.
        MGPVertexElements tooManyAttributes = declared.Desc;
        tooManyAttributes.AttributeCount = kMGPipeMaxVertexAttribs + 1;
        ExpectRefusedNaming("create_vertex_elements {slot=5, gen=2}: the declared attribute count is above "
                            "GL's attribute limit",
                            [&tooManyAttributes, blobBytes]() {
                                MGPipeApplyCreateVertexElements(tooManyAttributes, blobBytes);
                            });
        MGPVertexElements tooManyBindings = declared.Desc;
        tooManyBindings.BindingPointCount = kMGPipeMaxVertexAttribs + 1;
        ExpectRefusedNaming("create_vertex_elements {slot=5, gen=2}: the declared binding-point count is "
                            "above GL's attribute limit",
                            [&tooManyBindings, blobBytes]() {
                                MGPipeApplyCreateVertexElements(tooManyBindings, blobBytes);
                            });
        EXPECT_EQ(ElementsOf(cso.Slot).ContentSerial, 2u);
        EXPECT_EQ(ElementsOf(cso.Slot).AttributeCount, 3u);
#endif
    }

    // set_vertex_buffers: the window is the bound, the entries land inside it and nowhere
    // else, and the base instance is stored RAW. Deleting the copy loop, or the window gate,
    // fails this case.
    TEST(ResourceEmit, TheVertexBufferWindowIsBoundedAndItsEntriesLandWhereItSays) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        Array<MGPVertexBuffer, kMGPipeMaxVertexAttribs> wide{};
        for (Uint32 i = 0; i < kMGPipeMaxVertexAttribs; ++i) {
            wide[i].Res = MGPipeHandle{i + 1, 1};
            wide[i].Offset = 0x300ull + i;
            wide[i].Stride = 8 + i;
            wide[i].Divisor = i;
            wide[i].BindingIndex = i;
        }
        MGPVertexBuffers hdr{};
        hdr.Count = kMGPipeMaxVertexAttribs;
        hdr.BaseInstance = 7;
        hdr.ContentHash = 0xBEEF;
        const Uint64 serialBefore = MGPipeApplier().VertexBuffersSerial;
        MGPipeApplySetVertexBuffers(hdr, wide.data());

        EXPECT_EQ(MGPipeApplier().VertexBufferStart, 0u);
        EXPECT_EQ(MGPipeApplier().VertexBufferCount, kMGPipeMaxVertexAttribs);
        EXPECT_EQ(MGPipeApplier().VertexFetchBaseInstance, 7u)
            << "the RAW value is stored; whether the fetch shift is emulated is the backend's question";
        EXPECT_EQ(MGPipeApplier().VertexBuffersSerial, serialBefore + 1);
        for (Uint32 i = 0; i < kMGPipeMaxVertexAttribs; ++i) {
            ExpectVertexBufferEq(MGPipeApplier().VertexBuffers[i], wide[i], i);
        }

        // A narrower set writes its window and NOTHING else: the record is "the last set as
        // received", and a set that names two entries has said nothing about the other 30.
        MGPVertexBuffer narrow[2]{};
        narrow[0].Res = MGPipeHandle{99, 1};
        narrow[0].Stride = 1000;
        narrow[1].Res = MGPipeHandle{98, 1};
        narrow[1].Stride = 1001;
        MGPVertexBuffers narrowHdr{};
        narrowHdr.Start = 2;
        narrowHdr.Count = 2;
        MGPipeApplySetVertexBuffers(narrowHdr, narrow);
        EXPECT_EQ(MGPipeApplier().VertexBufferStart, 2u);
        EXPECT_EQ(MGPipeApplier().VertexBufferCount, 2u);
        EXPECT_EQ(MGPipeApplier().VertexBuffersSerial, serialBefore + 2);
        ExpectVertexBufferEq(MGPipeApplier().VertexBuffers[2], narrow[0], 2);
        ExpectVertexBufferEq(MGPipeApplier().VertexBuffers[3], narrow[1], 3);
        for (Uint32 i = 0; i < kMGPipeMaxVertexAttribs; ++i) {
            if (i == 2 || i == 3) continue;
            ExpectVertexBufferEq(MGPipeApplier().VertexBuffers[i], wide[i], i);
        }
        EXPECT_EQ(MGPipeApplier().VertexFetchBaseInstance, 0u) << "the base instance travels with every set";

        // Start + Count is the destination's own capacity, so 32 is accepted above and 33 is a
        // var-tail header describing more than the applier holds.
        MGPVertexBuffers past{};
        past.Start = 1;
        past.Count = kMGPipeMaxVertexAttribs;
        past.ContentHash = 0xBEEF;
        ExpectRefusedNaming("set_vertex_buffers {start=1, count=32, hash=48879}: the window runs past GL's "
                            "attribute limit",
                            [&past, &wide]() { MGPipeApplySetVertexBuffers(past, wide.data()); });
        EXPECT_EQ(MGPipeApplier().VertexBuffersSerial, serialBefore + 2) << "a refused set must move no serial";
        EXPECT_EQ(MGPipeApplier().VertexBufferStart, 2u);
        EXPECT_EQ(MGPipeApplier().VertexBufferCount, 2u);
        ExpectVertexBufferEq(MGPipeApplier().VertexBuffers[2], narrow[0], 2);

        MGPVertexBuffers noEntries{};
        noEntries.Count = 4;
        noEntries.ContentHash = 0xBEEF;
        ExpectRefusedNaming("set_vertex_buffers {start=0, count=4, hash=48879}: a non-empty set carries no "
                            "entries",
                            [&noEntries]() { MGPipeApplySetVertexBuffers(noEntries, nullptr); });
        EXPECT_EQ(MGPipeApplier().VertexBuffersSerial, serialBefore + 2);
#endif
    }

    // set_index_buffer is an INDEPENDENT call and not a subset of the vertex-elements
    // configuration (D5), which is exactly what the backend's two separate compares need; and
    // the binding follows the handle, including the null one.
    TEST(ResourceEmit, SetIndexBufferMovesOnlyItsOwnSerialAndTheBindingFollowsTheHandle) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle cso{4, 1};
        const ElementsBlob elements = MakeElements(cso, 2, 1, true);
        MGPipeApplyCreateVertexElements(elements.Desc, elements.Data());
        const Uint64 contentSerial = ElementsOf(cso.Slot).ContentSerial;
        const Uint64 vertexBuffersSerial = MGPipeApplier().VertexBuffersSerial;
        const Uint64 indexBufferSerial = MGPipeApplier().IndexBufferSerial;

        MGPipeApplySetIndexBuffer(MGPIndexBuffer{MGPipeHandle{9, 1}, 128, 2, 0});
        EXPECT_EQ(MGPipeApplier().IndexBuffer.Res, (MGPipeHandle{9, 1}));
        EXPECT_EQ(MGPipeApplier().IndexBuffer.Offset, 128u);
        EXPECT_EQ(MGPipeApplier().IndexBuffer.IndexSize, 2u);
        EXPECT_EQ(MGPipeApplier().IndexBufferSerial, indexBufferSerial + 1);
        EXPECT_EQ(MGPipeApplier().VertexBuffersSerial, vertexBuffersSerial)
            << "the index slot is not part of the vertex-elements configuration";
        EXPECT_EQ(ElementsOf(cso.Slot).ContentSerial, contentSerial);

        // A null Res is the state a client-memory index draw is in, and it is a legal set.
        MGPipeApplySetIndexBuffer(MGPIndexBuffer{kMGPipeNullHandle, 0, 0, 0});
        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().IndexBuffer.Res));
        EXPECT_EQ(MGPipeApplier().IndexBufferSerial, indexBufferSerial + 2);

        // The null handle is a legal BIND too - GL's unbound state is a state, not an error.
        MGPipeApplyBindVertexElements(ElementsHandle(cso));
        EXPECT_EQ(MGPipeApplier().BoundVertexElements, cso);
        MGPipeApplyBindVertexElements(ElementsHandle(kMGPipeNullHandle));
        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundVertexElements));
        EXPECT_EQ(MGPipeApplier().RefusedVertexInputCalls, 0u) << "unbinding is not a refusal";

        // A DEAD handle leaves the previous binding untouched rather than clearing it.
        MGPipeApplyBindVertexElements(ElementsHandle(cso));
        MGPipeApplyBindVertexElements(ElementsHandle(MGPipeHandle{cso.Slot, cso.Gen + 1}));
        EXPECT_EQ(MGPipeApplier().BoundVertexElements, cso)
            << "a dead handle must neither steal the binding nor clear it";
        EXPECT_EQ(MGPipeApplier().RefusedVertexInputCalls, 1u);

        // A delete drops the record whole, keeps the generation for the client allocator, and
        // clears a binding that named it.
        MGPipeApplyDeleteVertexElements(ElementsHandle(cso));
        EXPECT_FALSE(ElementsOf(cso.Slot).Live);
        EXPECT_EQ(ElementsOf(cso.Slot).Gen, cso.Gen) << "the generation is the client's to bump";
        EXPECT_EQ(ElementsOf(cso.Slot).ContentSerial, 0u) << "0 means never created";
        EXPECT_EQ(ElementsOf(cso.Slot).AttributeCount, 0u);
        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundVertexElements));
#endif
    }

    // The three refusals the sub-data case above does not reach, each on the call that owns
    // it: the flush's range, the readback's range, a buffer write that carries a mip level,
    // and a buffer write whose declared blob length is not its own byte size. Removing any one
    // of those four gates leaves this case red.
    TEST(ResourceEmit, EveryContentCallsOwnBoundsGateRefusesAndNamesTheResource) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        const MGPipeHandle res{6, 2};
        const Uint8 bytes[256] = {};
        MGPipeApplyResourceCreate(BufferDesc(res, 0, 77));
        MGPipeApplyResourceRespecify(BufferDesc(res, 256, 77), nullptr);

        // Positive controls first, each ending EXACTLY at the declared extent, so what follows
        // is refusing the range and not the arithmetic around it.
        MGPipeApplyResourceFlushRange(MGPFlushRange{res, 192, 64, 0, 0}, bytes);
        ASSERT_EQ(RecordOf(res.Slot).Serial, 2u);
        MGPipeApplyResourceReadback(MGPReadback{res, 192, 64});
        ASSERT_EQ(RecordOf(res.Slot).Serial, 2u) << "a readback does not mutate the store";
        MGPSubData declaredBlob = BufferWrite(res, 0, 64);
        declaredBlob.Blob.Size = 64; // a transport that fills the length in agrees with it
        MGPipeApplyResourceSubData(declaredBlob, bytes);
        ASSERT_EQ(RecordOf(res.Slot).Serial, 3u);

        const MGPFlushRange pastFlush{res, 200, 64, 0, 0};
        ExpectRefusedNaming("resource_flush_range {slot=6, gen=2, glName=77}: the range runs past the "
                            "resource's declared storage",
                            [&pastFlush, &bytes]() { MGPipeApplyResourceFlushRange(pastFlush, bytes); });
        EXPECT_EQ(RecordOf(res.Slot).Serial, 3u) << "a refused flush must not move the serial";

        const MGPReadback pastReadback{res, 200, 64};
        ExpectRefusedNaming("resource_readback {slot=6, gen=2, glName=77}: the range runs past the "
                            "resource's declared storage",
                            [&pastReadback]() { MGPipeApplyResourceReadback(pastReadback); });

        MGPSubData leveled = BufferWrite(res, 0, 64);
        leveled.Level = 1;
        ExpectRefusedNaming("resource_subdata {slot=6, gen=2, glName=77}: the buffer half carries a mip level",
                            [&leveled, &bytes]() { MGPipeApplyResourceSubData(leveled, bytes); });

        MGPSubData lyingBlob = BufferWrite(res, 0, 64);
        lyingBlob.Blob.Size = 65;
        ExpectRefusedNaming("resource_subdata {slot=6, gen=2, glName=77}: the declared blob length is not "
                            "the record's own byte size",
                            [&lyingBlob, &bytes]() { MGPipeApplyResourceSubData(lyingBlob, bytes); });
        EXPECT_EQ(RecordOf(res.Slot).Serial, 3u) << "not one of the four refusals may move the serial";
#endif
    }

    // D-A4's pin, with the producer this phase does not have. NoResourcePathInThisPhaseLeaves
    // HostWritesLive above proves that nothing SETS HasLiveHostWrites; this proves that the
    // wire which is supposed to catch a producer can actually fire - otherwise it is a gate
    // that cannot go red, which is the mistake the wire's own justification is avoiding. The
    // flag is set here by hand, which is exactly what the phase that pushes persistent-mapped
    // host writes will do, and map_persistent is the call it will do it on.
    TEST(ResourceEmit, TheLiveHostWritesWireFiresOnTheCallAPersistentMapProducerWouldSetItOn) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#elif !(MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY)
        GTEST_SKIP() << "Fatal{PipeLiveHostWrites} is a MOBILEGL_PIPE_VERIFY wire and is compiled out here";
#elif !MGTEST_HAVE_FORK
        GTEST_SKIP() << "no fork on this platform; the wire's verdict is std::abort()";
#else
        ApplierGuard guard;
        const MGPipeHandle res{8, 4};
        const Uint8 bytes[64] = {};
        MGPipeApplyResourceCreate(BufferDesc(res, 0, 55));
        MGPipeApplyResourceRespecify(BufferDesc(res, 256, 55), nullptr);

        // The negative control: with the flag clear the same call is silent and answers
        // normally, so what follows is the flag firing and not the call.
        EXPECT_EQ(MGPipeApplyMapPersistent(BufferHandle(res), 256, bytes), nullptr);
        EXPECT_EQ(ReadLog().find("PipeLiveHostWrites"), std::string::npos);

        struct Drive {
            MGPipeHandle Res;
            const char* Call;
        };
        const Drive drives[] = {
            {res, "map_persistent"}, {res, "resource_respecify"}, {res, "resource_subdata"},
            {res, "resource_flush_range"}, {res, "resource_readback"},
        };
        for (const Drive& drive : drives) {
            const ChildResult child = RunInChild([&drive, &bytes]() {
                // Set in the CHILD: the parent's applier must stay honest for the next drive.
                MGPipeApplier().Resources[drive.Res.Slot].HasLiveHostWrites = true;
                const String call = drive.Call;
                if (call == "map_persistent") {
                    MGPipeApplyMapPersistent(MGPHandleOnly{drive.Res, static_cast<Uint32>(MGPipeKind::Buffer), 0},
                                             256, bytes);
                } else if (call == "resource_respecify") {
                    MGPResourceDesc desc{};
                    desc.Resource = drive.Res;
                    desc.Width = 256;
                    desc.GlNameForDiag = 55;
                    MGPipeApplyResourceRespecify(desc, nullptr);
                } else if (call == "resource_subdata") {
                    MGPSubData record{};
                    record.Res = drive.Res;
                    MGPipeSetSubDataBufferRange(record, 0, 64);
                    MGPipeApplyResourceSubData(record, bytes);
                } else if (call == "resource_flush_range") {
                    MGPipeApplyResourceFlushRange(MGPFlushRange{drive.Res, 0, 64, 0, 0}, bytes);
                } else {
                    MGPipeApplyResourceReadback(MGPReadback{drive.Res, 0, 256});
                }
            });
            EXPECT_TRUE(DiedOfAbort(child))
                << drive.Call << ": " << DescribeStatus(child) << "; log: " << child.Log;
            const std::string wanted =
                std::string("Fatal{PipeLiveHostWrites} ") + drive.Call + " {slot=8, gen=4}";
            EXPECT_NE(child.Log.find(wanted), std::string::npos)
                << "wanted \"" << wanted << "\"; log: " << child.Log;
        }
#endif
    }

    // M-D. The slot is the one number in the family that reaches an ALLOCATOR, so it is
    // policed like every other: a slot outside the table's bound is Fatal{ProtocolCorruption}
    // and never a resize. Removing the bound turns this case into a multi-gigabyte allocation.
    TEST(ResourceEmit, ASlotOutsideTheRecordTablesBoundIsRefusedRatherThanAllocated) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ApplierGuard guard;
        // An ordinary slot is ordinary, and the table grows to it and no further.
        const MGPipeHandle ordinary{9, 1};
        MGPipeApplyResourceCreate(BufferDesc(ordinary, 0, 1));
        ASSERT_TRUE(RecordOf(ordinary.Slot).Live);
        const SizeT tableSize = MGPipeApplier().Resources.size();

        // The bound is exact: the first slot AT it is refused. The last slot BELOW it is
        // deliberately not driven - naming it is a ~90 MB allocation, and the direction that
        // matters here is the one that reaches the allocator.
        const MGPResourceDesc atBound = BufferDesc(MGPipeHandle{kMGPipeMaxResourceSlots, 1}, 0, 2);
        ExpectRefusedNaming("resource_create {slot=1048576, gen=1, glName=2}: the slot is outside the "
                            "record table's bound",
                            [&atBound]() { MGPipeApplyResourceCreate(atBound); });
        EXPECT_EQ(MGPipeApplier().Resources.size(), tableSize) << "the refusal must not have grown the table";

        const MGPResourceDesc past = BufferDesc(MGPipeHandle{0xFFFFFFFEu, 1}, 0, 3);
        ExpectRefusedNaming("resource_create {slot=4294967294, gen=1, glName=3}: the slot is outside the "
                            "record table's bound",
                            [&past]() { MGPipeApplyResourceCreate(past); });
        EXPECT_EQ(MGPipeApplier().Resources.size(), tableSize) << "the refusal must not have grown the table";

        const ElementsBlob elements = MakeElements(MGPipeHandle{kMGPipeMaxVertexElementsSlots, 1}, 1, 1, true);
        const void* blobBytes = elements.Data();
        const MGPVertexElements desc = elements.Desc;
        ExpectRefusedNaming("create_vertex_elements {slot=65536, gen=1}: the slot is outside the record "
                            "table's bound",
                            [&desc, blobBytes]() { MGPipeApplyCreateVertexElements(desc, blobBytes); });
        EXPECT_TRUE(MGPipeApplier().VertexElementsCsos.empty());
#endif
    }
} // namespace

int main(int argc, char** argv) {
    // Before anything logs: the logger reads this variable once, on its first write, and
    // caches the handle. The name carries this process's pid, and the file is removed on the
    // way out.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-resourceemit-test-" + std::to_string(ProcessId()) + ".log");
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
