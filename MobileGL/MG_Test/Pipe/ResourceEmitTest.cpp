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
    struct ApplierGuard {
        ApplierGuard() {
            MGPipeSetResourceOps(nullptr);
            MGPipeApplierReset();
        }
        ~ApplierGuard() {
            MGPipeSetResourceOps(nullptr);
            MGPipeApplierReset();
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

        // A fresh applier carries none of it over either.
        MGPipeApplierReset();
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
