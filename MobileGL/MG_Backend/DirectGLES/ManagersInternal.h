// MobileGL - MobileGL/MG_Backend/DirectGLES/ManagersInternal.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "Managers.h"
#include "SubgroupPolicy.h"
#include <MG_Backend/MGPipe/PipeInputs.h>
// P3a: the handle-shaped resource op table, the applier's records and the reverse channel.
// Both headers are compiled into the library only under push, so they are included here
// under the same condition - a pull build must gain no declaration it cannot link.
#include <MG_Pipe/MGPipeCallbacks.h>
#include <MG_Pipe/MGPipeHostSpan.h>
#include <MG_Pipe/PipeApply.h>
// P5e (pg): the DEFINITION of the archive MGPipeShaderCsoRecord holds by SharedPtr. PipeApply.h
// needs only the name - the deleter is captured where the archive is constructed - but the
// program build reads through it, so the one translation unit that does needs the whole type.
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
// R-11's server-owned staging copy. Header-only and package v1's; see its own header block for
// why GLESBufferResource does not simply gain a member.
#include <MG_Backend/Record/StagedShadow.h>
#include <MG_Backend/Record/StagedTextureStore.h>
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Remote/Server/ServerLoop.h>
// P5c (ct): object_death's producer (CONTRACT-P5C.md §5.2) - the death notice's split arm
// emits the record through the client's emit helper instead of hopping a stack struct to the
// apply thread.
#include <MG_Remote/Client/WireTables.h>
// P11 B2 (T0): the client's AHardwareBuffer store - its reference counting and, on Android, the
// allocation the POST self-test makes.
#include <MG_Remote/Transport/AdoptT0.h>
#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#endif
#endif

#include "Utils.h"
#include "DirectGLES.h"
#include "BackendObject_DirectGLES.h"
#include <Config.h>
#include <MG_State/GLState/StateObjectDeathNotice.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/TranslationCache.h>

#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/DataTypeConverter.h>
#include <MG_Util/Converters/MGToGL/BufferEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/FramebufferEnumConverter.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_Util/Converters/GLToMG/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <cstring>
#include <regex>
#include "ContextEpoch.h"

namespace MobileGL::MG_Backend::DirectGLES {
    constexpr const char* BASE_INSTANCE_UNIFORM_NAME = "mg_BaseInstance";
    constexpr const char* DRAW_ID_UNIFORM_NAME = "mg_DrawID";
    constexpr const char* BASE_VERTEX_UNIFORM_NAME = "mg_BaseVertex";
    constexpr const char* BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME = "mg_BaseInstanceWordIndex";
    constexpr const char* INDIRECT_PARAMS_BLOCK_NAME = "mg_IndirectParams";

    Bool ViewportArrayEmulationEnabled();

    GLenum ResolveBackendMinFilter(const SamplerParameters& samplerParams, Bool avoidMipmapMinFilter);

    String EmulateBaseInstanceInVertexShader(String source, GLenum shaderType);

    String PromoteDrawParameterGlobalsToUniforms(String source, GLenum shaderType);
    constexpr const char* VIEWPORT_PASS_MASK_UNIFORM_NAME = "mg_ViewportPassMask";

    Bool PromoteViewportIndexGlobalToVarying(String& source);

    Bool InjectViewportIndexPassGate(String& source);

    namespace BufferImpl {
        using MG_State::GLState::BackendBufferResource;
        using MG_State::GLState::BufferObject;

        extern Uint g_bufferContextGeneration;

        // --- Persistent-mapped bump rings (see Managers.h) -----------------------
        // Shared machinery behind BOTH the global-UBO ring and the texture
        // unpack-PBO ring: one EXT_buffer_storage persistent|coherent map per ring,
        // monotonic head/tail cursors, and reclamation riding the Present()
        // frame-fence watermark. The two rings differ only in size cap, offset
        // alignment and log label - the reclamation, context-loss and
        // emergency-drain rules are the part that was hard to get right, so they
        // are shared rather than copied.
        constexpr SizeT kUboRingInitialBytes = 4u * 1024u * 1024u;
        constexpr SizeT kUboRingMaxBytes = 64u * 1024u * 1024u;
        constexpr SizeT kUnpackRingInitialBytes = 4u * 1024u * 1024u;
        constexpr SizeT kUnpackRingMaxBytes = 64u * 1024u * 1024u;
        // A PBO-sourced glTexSubImage constrains the offset only to the pixel
        // TYPE's size (GL_INVALID_OPERATION otherwise), and no ES client type is
        // wider than 4 bytes - unlike a UBO bind, which owes the driver
        // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT. 64 covers every type with room to
        // spare and keeps consecutive staged blocks off each other's cache lines.
        constexpr SizeT kUnpackRingAlignment = 64;
        // glCopyBufferSubData carries no offset-alignment requirement at all; 64
        // keeps staged blocks cache-line separated, same as the unpack ring.
        constexpr SizeT kUploadRingInitialBytes = 4u * 1024u * 1024u;
        constexpr SizeT kUploadRingMaxBytes = 64u * 1024u * 1024u;
        constexpr SizeT kUploadRingAlignment = 64;

        struct PersistentRingStore {
            Uint id = 0;
            Uint8* mappedPtr = nullptr;
            SizeT size = 0;
            // Monotonic linear cursors: `head` counts every byte ever allocated
            // (incl. wrap padding); everything below `tail` is GPU-complete. Ring
            // offset of a linear position is pos % size, so in-flight bytes are
            // head - tail and must stay <= size.
            Uint64 head = 0;
            Uint64 tail = 0;
            Uint32 generation = 0; // bumped on every (re)create/grow; 0 = never valid
            Uint contextGeneration = 0;
            SizeT alignment = 256;
            // A hard storage-creation failure under this context; stop retrying
            // per use (cleared when the context generation moves on).
            Bool creationFailed = false;
        };

        // Grown-away ring stores: deletable only once the GPU finished the last
        // frame that could reference them (same watermark as the buffer pool).
        struct RetiredRingStore {
            Uint id = 0;
            Uint contextGeneration = 0;
            Uint64 retireSerial = 0;
        };

        // Present()-time high-water marks: every byte below headAtPresent was
        // written during frames <= frameSerial, so once frameSerial completes,
        // tail may advance to headAtPresent. FIFO by construction.
        struct RingFrameMark {
            Uint64 frameSerial = 0;
            Uint64 headAtPresent = 0;
        };

        // One ring: its live store, its two reclamation lists, and the immutable
        // knobs that tell it apart from the other one.
        struct PersistentRing {
            PersistentRingStore store;
            Vector<RetiredRingStore> retired;
            Vector<RingFrameMark> frameMarks;
            SizeT initialBytes = 0;
            SizeT maxBytes = 0;
            // 0: take the offset alignment from GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
            // at store-creation time (the UBO ring's binds require it).
            SizeT fixedAlignment = 0;
            const char* label = "";
        };

        // THE RINGS ARE PER NATIVE CONTEXT. Their slots are reclaimed by frame marks recorded at
        // Present, which say "everything this context wrote before its fence N is done once N is".
        // One process-wide set used to be shared by every session's contexts: a fence from one
        // context retired bytes another context's not-yet-executed copies still had to read
        // (a glTexSubImage from the unpack ring is a deferred GPU read), and the next session's
        // staging overwrote them - its pixels landed, at the wrong offsets, in the first one's
        // textures. With one set per context, a context's bytes wait for that context's own
        // fence. (The buffers themselves are shared objects; only who stages into them is split.)
        // Staging ring for app buffer updates whose destination store may still be
        // referenced by in-flight GPU work. Mali's glBufferSubData resolves that WAR
        // hazard by BLOCKING in the call (osup_sync_object_wait) until every
        // referencing job retires - under Minecraft 26.3's per-frame UBO and
        // chunk-mesh SubData streams that serialized whole frames (~1 fps while
        // chunks stream in). Staging the bytes here and issuing a
        // glCopyBufferSubData instead keeps the hazard on the GPU timeline where it
        // is just job ordering, and the CPU never waits.
        struct RingSet {
            PersistentRing ubo{{}, {}, {}, kUboRingInitialBytes, kUboRingMaxBytes, 0, "Global-UBO ring"};
            PersistentRing unpack{
                {}, {}, {}, kUnpackRingInitialBytes, kUnpackRingMaxBytes, kUnpackRingAlignment, "Texture unpack ring"};
            PersistentRing upload{
                {}, {}, {}, kUploadRingInitialBytes, kUploadRingMaxBytes, kUploadRingAlignment, "Buffer upload ring"};
            RingSet();
            ~RingSet();
            RingSet(const RingSet&) = delete;
            RingSet& operator=(const RingSet&) = delete;
        };

        extern DirectGLES::PerNativeContext<RingSet> g_ringSets;

        namespace {
            Bool CanTouchGLNow() {
                return DirectGLES::IsBackendContextCurrentOnThisThread();
            }
        } // namespace

        MG_Record::StagedShadowStore& ServerStaged();

        namespace {
            // THE COVERAGE RULE FOR THE THREE-TIER FLUSH DRAIN, ASSERTED BEFORE THE CALL AND NOT
            // INSIDE IT. FlushPendingRangesFrom is a G5-pinned body (p3a_untouched_regions.sh's
            // PINNED_FUNCTIONS, ID-41): the split arm CALLS it, it does not re-spell it, and it
            // does not add a line to it either. Its tier-1 arm - the range-invalidating map -
            // copies with its own Memcpy and never reaches UploadRangeFrom, so the one tier that
            // DECLARES THE OLD BYTES DEAD is the one tier no later check can see; the only place
            // left to say "every queued range is inside the staged coverage" is the instant before
            // the drain takes the queue. The clamp is the drain's own (limit = the smaller of the
            // frontend size and the backend store; bytes past either end have nowhere to land and
            // are not this rule's subject), so the two agree about which bytes are meant.
            //
            // Fires only for a base that IS this resource's server shadow: RequireCoverage
            // answers nothing for the legacy arm's MappedData(), which is valid for the whole
            // store. `pendingMutex` is taken here and released before the drain takes it again.
            void RequireStagedCoverageForPendingRanges(GLESBufferResource& resource, const Uint8* hostBase,
                                                       SizeT frontendSize, const char* site) {
                if (hostBase == nullptr) return;
                const SizeT limit = std::min(frontendSize, resource.storageSize);
                const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                for (const auto& range : resource.pendingRanges) {
                    const SizeT end = std::min(range.end, limit);
                    const SizeT start = std::min(range.start, end);
                    if (start == end) continue;
                    ServerStaged().RequireCoverage(&resource, hostBase, start, end, site);
                }
            }

// The four hostBytes sites read the same in both builds. The non-split expansion is the
// ORIGINAL EXPRESSION, character for character - `raw - offset` - so a push or verify build
// compiles exactly what it compiled before R-11 and the split arm is the only new behavior.
#define MGL_SERVER_STAGED_ADOPT(res, width, bytes, offset, size)                                                       \
    ServerStaged().Adopt(&(res), (width), (bytes), (offset), (size))
#define MGL_SERVER_STAGED_REQUIRE_PENDING(res, base, frontendSize, site)                                               \
    RequireStagedCoverageForPendingRanges((res), (base), (frontendSize), (site))
        } // namespace

        // Ring machinery shared with the UBO/unpack rings; defined further down in
        // this same unnamed namespace.
        Bool RingAllocate(PersistentRing& ring, SizeT size, SizeT& outOffset);

        // Push every queued range of `resource` from the shadow into the backend
        // store, without ever letting a driver resolve the WAR hazard against
        // in-flight frames at the WHOLE BUFFER's expense. Three tiers:
        //
        //   1. glMapBufferRange(WRITE | INVALIDATE_RANGE) + memcpy. The entire
        //      mapped range is rewritten from the authoritative shadow, so
        //      declaring its old bytes dead is exact - and it lets the driver
        //      swap fresh pages in for JUST that range. This is the only tier
        //      whose cost scales with the RANGE on this Mali driver: both the
        //      immediate glBufferSubData (pre-queueing) and a staged
        //      glCopyBufferSubData into a busy MUTABLE store ghost the whole
        //      destination with a worker-thread memcpy - Minecraft 26.3 streams
        //      ~1MB section meshes into 128MB arenas about nine times a frame
        //      during a camera pan, and 9 x 128MB of ghosting per frame is
        //      ~380ms, the measured 2-4 fps. (Backing the arenas with immutable
        //      stores also kills the ghost, but eagerly commits every arena's
        //      full extent - +hundreds of MB - which LMK'd the whole device.)
        //   2. The staging ring + glCopyBufferSubData: the copy is ordered on
        //      the GPU timeline, no CPU wait (MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH
        //      forces this tier as the map path's negative control).
        //   3. Direct glBufferSubData (potentially stalling) when neither the
        //      map entry points nor the ring exist.
        //
        // The ranges are flushed AS QUEUED (VecRange1D::Add already merges
        // near-adjacent ones): bytes, not flush calls, are the cost axis here,
        // and collapsing a scattered flush into its union re-copied nearly whole
        // chunk-mesh arenas every frame.
        // The caller owns syncedChangeSerial; this only drains the queue.
        //
        // ONE body for both arms, and deliberately so: the three-tier decision (whole-buffer
        // orphan map / >= 128 KiB range-invalidating map / staged ring copy) is what the MC
        // 26.3 p99 depends on, and a second copy of it for the handle arm is exactly how a
        // tier silently changes. The arms differ only in where `hostBase` and `frontendSize`
        // come from.
        //
        // R-11'S PARAMETER, AND IT IS A PARAMETER RATHER THAN AN ASSUMPTION (P5 b1).
        // `hostBase` is re-read at every use today precisely because a shadow resize or an
        // adoption moves what an earlier base pointed at. Under split the server may hold
        // no pointer into the client's shadow at all, so the base becomes a SNAPSHOT taken
        // into SEG_STAGE at emission - and a snapshot covers a RANGE, not the store. The
        // two extra arguments say which range `hostBase` is good for; the default is the
        // whole store, which is exactly what a live shadow is, so every caller today is
        // byte-identical. The moment w1 passes a real snapshot extent, tier 1's widening
        // refusal below stops being unreachable and a too-narrow snapshot is named instead
        // of silently clobbering GPU-written bytes with stale ones.
        constexpr SizeT kHostBaseCoversWholeStore = ~static_cast<SizeT>(0);

        void FlushPendingRangesFrom(GLESBufferResource& resource, const Uint8* hostBase, SizeT frontendSize,
                                    SizeT hostBaseFrom = 0, SizeT hostBaseTo = kHostBaseCoversWholeStore);

        void DrainResidentWritesNow(GLESBufferResource& resource);

        namespace {
            // The two-argument spelling the legacy arm's call sites use, kept so those sites
            // are the SAME text in both builds (G1). The object was never read.
            void DrainResidentWritesNow(GLESBufferResource& resource, BufferObject& bufferObject) {
                (void)bufferObject;
                DrainResidentWritesNow(resource);
            }

            // ---- P3a: the same seven ops plus the create/unmap pair, BY HANDLE ------------
            //
            // Every body below is its Ops_* counterpart above with exactly the substitutions
            // D-A2's table names and nothing else: the frontend object's GetSize() / GetUsage()
            // / HasDefinedContent() / MappedData() / GetChangeSerial() become the applier
            // record's Desc.Width / Desc.Usage / Desc.HasDefinedContent, the shadow base the
            // call carried, and the record's server-owned Serial. Every branch survives -
            // pendingRespecify early-out, the off-thread queue, the adopted zero-copy stamp,
            // the upload-ring kill switch, the Mali WAR-stall queue-only default.
            //
            // NOTHING here reads a frontend type. The one thing a handle op cannot do is ask
            // an object for its shadow, which is why GLESBufferResource::hostBytes exists: the
            // three content-carrying calls hand the base over and the later drains read it.

            // The applier's record for this resource, or null when the client never created it
            // (or created it into a slot that has since been recycled).
            const MG_Pipe::MGPipeResourceRecord* ResourceRecordOf(MG_Pipe::MGPipeHandle res) {
                if (MG_Pipe::MGPipeHandleIsNull(res)) return nullptr;
                const auto& records = MG_Pipe::MGPipeApplier().Resources;
                if (res.Slot >= records.size()) return nullptr;
                const auto& record = records[res.Slot];
                if (!record.Live || record.Gen != res.Gen) return nullptr;
                return &record;
            }

            // The server-owned MGGen this backend mirrors in syncedChangeSerial. Zero for a
            // resource with no record, which is the same "never synced" answer a frontend
            // change serial of zero gave.
            Uint64 ResourceSerialOf(MG_Pipe::MGPipeHandle res) {
                const auto* record = ResourceRecordOf(res);
                return record != nullptr ? record->Serial : 0;
            }
            SizeT ResourceWidthOf(MG_Pipe::MGPipeHandle res) {
                const auto* record = ResourceRecordOf(res);
                return record != nullptr ? static_cast<SizeT>(record->Desc.Width) : 0;
            }
        } // namespace

#if MOBILEGL_BUILD_DISAGGREGATED && defined(__ANDROID__)
        using MG_Util::BackendLoader::HasExtensionWord;
#endif // MOBILEGL_BUILD_DISAGGREGATED && __ANDROID__

        const Vector<TextureUploadTarget>& StagedUploadTargetsForPipeTarget(Uint8 pipeResourceTarget);

        TextureTarget StagedTextureTargetForPipeTarget(Uint8 pipeResourceTarget);

        void BumpBufferMutationEpoch();

        GLESBufferResource* FindBufferResourceForHandle(MG_Pipe::MGPipeHandle res);

        SizeT ResourceWidthForHandle(MG_Pipe::MGPipeHandle res);

        Uint64 ResourceSerialForHandle(MG_Pipe::MGPipeHandle res);

        GLESBufferResource* EnsureBufferResourceForHandle(
            const SharedPtr<MG_State::GLState::BufferObject>& bufferObject, MG_Pipe::MGPipeHandle res);

        void BindBufferId(GLenum target, Uint id);

        void BindPixelUnpackBufferId(Uint id);

        void NoteBufferIdDeleted(Uint id);

        Bool CreateRingStorage(PersistentRing& ring, SizeT minBytes);
        Bool RingAllocateSlow(PersistentRing& ring, SizeT size, SizeT& outOffset);

        Bool RingAllocate(PersistentRing& ring, SizeT size, SizeT& outOffset);

        Bool RingAllocateSlow(PersistentRing& ring, SizeT size, SizeT& outOffset);

        Bool UnpackRingAvailable();

        Bool UnpackRingAllocate(SizeT size, SizeT& outOffset);

        void* UnpackRingMappedPtr();

        Uint UnpackRingBufferId();

        SizeT UnpackRingMaxBytes();
    } // namespace BufferImpl

    // The release-build VOICE for StateBackendObjectRegistry::GetOrCreateByHandle's two
    // silent refusals, shared by the kinds P4a re-keys so the wording cannot drift between
    // them. It is the shape GetOrCreateBufferResourceForHandle gives P3a's buffer family,
    // lifted into one template because a copy per kind is a chance per kind to write one of
    // them differently.
    //
    // ITS CALLERS IN THIS PACKAGE ARE THE FRAMEBUFFER ATTACHMENT WALK'S TWO
    // (FramebufferImpl::SyncAttachmentSurface: the texture registry and the renderbuffer
    // registry), because that is where a HANDLE arrives in a payload - MGPSurface::Res -
    // and an ADOPTION is what the arm owes rather than a lookup. The other three re-keyed
    // kinds resolve at sites in DirectGLES.cpp (package E's SyncCurrentFBO, the lazy
    // sampler mint and SyncCurrentProgram), so their first adoption is E's to write; this
    // template is what they call so all five say the same thing.
    //
    // WHY A VOICE AT ALL: the table asserts, and MOBILEGL_ASSERT compiles out at INFO -
    // which all three gate builds and every shipped build are - so an object that silently
    // stops being twinned is exactly the failure mode the refusal exists to replace.
    //
    // Returns null on the legacy arm too, so a caller that reaches this without checking
    // its family's arm gets nothing rather than a twin the legacy arm would never see.
    //
    // SPLIT IN TWO (review N-3), and the split is what makes the two refusals REACHABLE.
    // Both of this package's call sites stand behind a cross-check that the record's Res
    // equals registry.HandleOf(frontendObject) - the LIVE handle - so by the time the
    // adoption ran, the slot was in bounds and the generation was current BY CONSTRUCTION
    // and neither refusal below could ever fire: the letter of M-4 was closed and its
    // substance was not. The record's handle is the UNTRUSTED input and the frontend is the
    // corroboration, so the validity question is asked FIRST, on its own, at both sites;
    // the adoption then performs the resolve. Asking it first is also side-effect free,
    // which the ordering rule N-1 states for the framebuffer refusal wants here too:
    // GetOrCreateByHandle can reset an incumbent twin, and a handle this predicate rejects
    // must not reach it.
    template <typename Registry>
    Bool PipeTwinHandleIsAdoptable(Registry& registry, MG_Pipe::MGPipeHandle handle, const char* kindName) {
        if (MG_Pipe::MGPipeHandleIsNull(handle)) return false;
        if (handle.Slot >= Registry::SlotTable::kMaxHandleSlot) {
#if MOBILEGL_BUILD_DISAGGREGATED
            MG_Pipe::MGPipeSessionFail( // @Ph-declined (ID-P7-1): PH-2 stays Fatal, CONTRACT-P7 §12
                MG_Pipe::MGPipeFatalFamily::ProtocolCorruption,
                "MGPipe: Fatal{ProtocolCorruption, \"BackendSlotTable.HandleSlot\"} - "
                "%s handle slot %u is past this table's %u bound",
                kindName, handle.Slot, Registry::SlotTable::kMaxHandleSlot);
#else
            MGLOG_E_ONCE("MGPipe: %s handle slot %u is past the backend table's %u bound - "
                         "refusing to twin it",
                         kindName, handle.Slot, Registry::SlotTable::kMaxHandleSlot);
            return false;
#endif
        }
        const Uint32 liveGen = registry.LiveGenAt(handle.Slot);
        if (liveGen != 0 && liveGen > handle.Gen) {
#if MOBILEGL_BUILD_DISAGGREGATED
            MG_Pipe::MGPipeSessionFail( // @Ph-declined (ID-P7-1): PH-2 stays Fatal, CONTRACT-P7 §12
                MG_Pipe::MGPipeFatalFamily::ProtocolCorruption,
                "MGPipe: Fatal{ProtocolCorruption, \"BackendSlotTable.Generation\"} - "
                "%s handle {%u, %u} names generation behind live generation %u",
                kindName, handle.Slot, handle.Gen, liveGen);
#else
            // SlotTables.h:301-321: forward is a recycle and resets the twin, BACKWARD is
            // refused, because adopting it would release the incumbent LIVE twin's driver
            // id and then stamp the slot back to the dead object's generation, after which
            // the incumbent's own FindByHandle refuses it and it is silently handed a
            // fresh, empty twin - a leak AND an object that loses its storage with no
            // diagnostic. That is the shape commit d7655247 fixed on the buffer side.
            MGLOG_E_ONCE("MGPipe: %s handle {%u, %u} names a generation BEHIND the live "
                         "twin's %u - refusing rather than dropping the incumbent's driver "
                         "object",
                         kindName, handle.Slot, handle.Gen, liveGen);
            return false;
#endif
        }
        return true;
    }

    namespace {
        template <typename Registry>
        typename Registry::BackendPtr* AdoptTwinByHandle(Registry& registry, MG_Pipe::MGPipeHandle handle,
                                                         const char* kindName) {
            if (!PipeTwinHandleIsAdoptable(registry, handle, kindName)) return nullptr;
            return registry.GetOrCreateByHandle(handle);
        }
    } // namespace

    // ---- P4a: the record readers (Managers.h documents the contract) ----

    namespace {
        // One shape for all five, so a kind cannot grow a different liveness rule than its
        // neighbors: index the applier's own dense table, refuse a slot it has not grown to,
        // refuse a record that is not Live, and refuse a generation that has moved on under
        // the caller. The last of the three is the one that matters: a stale handle resolving
        // to its successor's record is how a twin ends up describing another object's storage.
        template <typename Record>
        const Record* PipeRecordAt(const Vector<Record>& table, MG_Pipe::MGPipeHandle handle) {
            if (MG_Pipe::MGPipeHandleIsNull(handle)) return nullptr;
            if (handle.Slot >= table.size()) return nullptr;
            const Record& record = table[handle.Slot];
            if (!record.Live || record.Gen != handle.Gen) return nullptr;
            return &record;
        }
    } // namespace

    const MG_Pipe::MGPipeResourceRecord* PipeTextureRecordForHandle(MG_Pipe::MGPipeHandle res);

    const MG_Pipe::MGPipeSamplerCsoRecord* PipeSamplerCsoRecordForHandle(MG_Pipe::MGPipeHandle cso);

    void NotePipeSubDataTargetLowByte(const MG_Pipe::MGPipeResourceRecord& record, Uint16 packedTarget, Uint16 level);

    const MG_Pipe::MGPipeResourceRecord::PendingUpload* FindPipeTextureUpload(
        const MG_Pipe::MGPipeResourceRecord& record, Uint16 uploadTarget, Uint16 level);

    void ConsumePipeTextureUpload(MG_Pipe::MGPipeHandle res, Uint16 uploadTarget, Uint16 level);

    Bool RearmPipeTextureLevelUpload(MG_Pipe::MGPipeHandle res, Uint16 packedTarget, Uint16 level,
                                     const MG_Pipe::MGPBox& wholeLevel);

    namespace TextureImpl {
        // Sets the backend GL unpack state to MobileGL's upload default for the scope,
        // then restores it. The previous state is read from a shadow instead of via
        // glGetIntegerv - that query forces a driver pipeline sync and, because texture
        // uploads run it per dirty texture per frame, it dominated the DirectGLES draw
        // path. The backend unpack state is set ONLY by MobileGL's own save/restore
        // helpers (this class and, historically, the R32F copy path), all of
        // which restore to the resting default, so the shadow stays accurate; a one-time
        // forced sync pins the backend to that known default up front. Apply() is
        // compare-and-set, so the (now redundant) glPixelStorei calls also usually no-op.
        class ScopedDefaultUnpackState {
        public:
            ScopedDefaultUnpackState() {
                EnsureShadowSynced();
                m_prevAlignment = s_alignment;
                m_prevRowLength = s_rowLength;
                m_prevSkipRows = s_skipRows;
                m_prevSkipPixels = s_skipPixels;
                m_prevImageHeight = s_imageHeight;
                m_prevSkipImages = s_skipImages;
                // Shadow mip data is tightly packed (ProcessTexturePixelsDataUnpack emits
                // width * bpp rows with no padding), so uploads must use UNPACK_ALIGNMENT = 1.
                // Alignment 4 made the driver read e.g. 7-byte R8 rows at an 8-byte stride,
                // shifting every row of a non-multiple-of-4 upload by one pixel.
                Apply(1, 0, 0, 0, 0, 0);
            }

            ~ScopedDefaultUnpackState() {
                Apply(m_prevAlignment, m_prevRowLength, m_prevSkipRows, m_prevSkipPixels, m_prevImageHeight,
                      m_prevSkipImages);
            }

        private:
            static void EnsureShadowSynced() {
#if MOBILEGL_BUILD_DISAGGREGATED
                const auto epoch = CurrentContextEpoch();
                if (s_epoch != epoch) {
                    s_synced = false;
                    s_epoch = epoch;
                }
#endif
                if (s_synced) {
                    return;
                }
                s_synced = true;
                g_GLESFuncs.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
                s_alignment = 4;
                s_rowLength = 0;
                s_skipRows = 0;
                s_skipPixels = 0;
                s_imageHeight = 0;
                s_skipImages = 0;
            }

            static void Apply(GLint alignment, GLint rowLength, GLint skipRows, GLint skipPixels, GLint imageHeight,
                              GLint skipImages) {
                if (alignment != s_alignment) {
                    g_GLESFuncs.glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
                    s_alignment = alignment;
                }
                if (rowLength != s_rowLength) {
                    g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
                    s_rowLength = rowLength;
                }
                if (skipRows != s_skipRows) {
                    g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
                    s_skipRows = skipRows;
                }
                if (skipPixels != s_skipPixels) {
                    g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
                    s_skipPixels = skipPixels;
                }
                if (imageHeight != s_imageHeight) {
                    g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, imageHeight);
                    s_imageHeight = imageHeight;
                }
                if (skipImages != s_skipImages) {
                    g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_IMAGES, skipImages);
                    s_skipImages = skipImages;
                }
            }

            GLint m_prevAlignment = 4;
            GLint m_prevRowLength = 0;
            GLint m_prevSkipRows = 0;
            GLint m_prevSkipPixels = 0;
            GLint m_prevImageHeight = 0;
            GLint m_prevSkipImages = 0;

            // Shadow of the backend GL unpack state (GL defaults). See class comment.
            static inline Bool s_synced = false;
#if MOBILEGL_BUILD_DISAGGREGATED
            static inline ContextEpoch s_epoch{};
#endif
            static inline GLint s_alignment = 4;
            static inline GLint s_rowLength = 0;
            static inline GLint s_skipRows = 0;
            static inline GLint s_skipPixels = 0;
            static inline GLint s_imageHeight = 0;
            static inline GLint s_skipImages = 0;
        };

        void NoteInPlaceTextureUpload();

        Uint GetWidenableClientComponentCount(TextureInternalFormat format);

        Bool IsIntegerWidenableFormat(TextureInternalFormat format);

        // P5e (tx2): the GL name a log line wants, from whichever side of the arm can answer it.
        // Desc.GlNameForDiag is the declared diagnostics carrier (MGPipeTypes.h) and it is the
        // ONLY thing the by-handle arm may say about a texture's frontend name - a GL name is
        // never an identity, never a memo key and never part of a hash (section 4.2.1). The
        // twenty-odd GetExternalIndex() log reads this family carried (scout G-S2-4) all become
        // this call.
        static Uint32 TextureDiagName(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject,
                                      const MG_Pipe::MGPipeResourceRecord* record) {
            if (stateTextureObject) return static_cast<Uint32>(stateTextureObject->GetExternalIndex());
            return record != nullptr ? record->Desc.GlNameForDiag : 0u;
        }

        // P5e (fix1, ID-81 / CONTRACT-P5E §5.8): THE NULL FRONTEND OBJECT IS A CONTRACT AND THIS
        // IS WHERE IT IS CHECKED, ONCE, FOR ALL THREE SYNC BODIES.
        //
        // tx2's three sync bodies accept a null `stateTextureObject` because the RECORD answers
        // every read they make - but only where the record arm is actually SELECTED. Two reads
        // inside SyncMipmapsToBackend are gated on `Transport != Monolith` (the texture-VIEW test
        // and RequireImageBindableStorage's re-dirty transition, §5.2), and every read in all
        // three is gated on TextureResourceSubsystemEnabled() through `pushedStorage`. So a null
        // object is servable exactly when BOTH hold, and the by-handle entries beside them
        // (SyncTextureToBackendByHandle / SyncMipmapsToBackendByHandle) pass null on the strength
        // of a NOTED HANDLE, which says nothing about either.
        //
        // The device found the gap: with MOBILEGL_TRANSPORT=monolith the push build still drives
        // its framebuffer attachments from the record (P4a, D-C2), fb's SyncAttachmentSurface
        // called the by-handle storage sync from that arm, and `stateTextureObject->IsTextureView()`
        // dereferenced the null SharedPtr in Lightmap.<init> -> clearColorTexture -> glClear.
        // The arm selection is repaired at that call site; this refusal is what makes the shape
        // UNREPEATABLE rather than merely repaired - a null frontend object reaching a body that
        // can still read one aborts BY NAME instead of taking a SIGSEGV three frames deep.
        //

        // Returns true when the caller must decline (no handle noted: the caller's bug it always
        // was, named by the caller's own log line), false when the record arm can serve the call,
        // and never returns at all when a handle was noted on an arm that cannot serve it.
        static Bool RefuseNullFrontendTextureOffTheHandleArm(const char* entry, MG_Pipe::MGPipeHandle notedHandle) {
            if (MG_Pipe::MGPipeHandleIsNull(notedHandle)) return true;
            return false;
        }
// The guard the three sync bodies spell. In the PULL build it folds to the constant the
// pre-fix1 text folded to, so that build's preprocessed text gains no call (D-P).
#define MGB_TEXTURE_NULL_FRONTEND_REFUSED(twin, entry)                                                                 \
    (RefuseNullFrontendTextureOffTheHandleArm((entry), (twin).PushedSyncHandle()))

        extern Array<Array<BackendTextureObject*, (SizeT)TextureTarget::TextureTargetCount>,
                     MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundTexturesCache;

        void NoteDriverSideTextureWriteByHandle(MG_Pipe::MGPipeHandle res, Bool storeFollowsTheWrite);
    } // namespace TextureImpl

    namespace ScratchFBOImpl {
        void NoteTextureIdDeleted(Uint textureId);
    } // namespace ScratchFBOImpl
} // namespace MobileGL::MG_Backend::DirectGLES
