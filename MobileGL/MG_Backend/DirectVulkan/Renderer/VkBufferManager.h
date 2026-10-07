// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "BufferArena.h"
#include "MG_State/GLState/BufferState/BufferObject.h"
#include "../VkIncludes.h"
#include <Includes.h>
#include <vk_mem_alloc.h>
#include "MG_Pipe/MGPipeTypes.h"
#include <unordered_map>

namespace MobileGL::MG_Backend::DirectVulkan {
    enum class BufferKind : Uint8 {
        Vertex,
        Index,
        Uniform,
        TextureBuffer,
        ShaderStorage,
        Indirect,
    };

    struct VkBufferManagerInitInfo {
        VmaAllocator allocator = nullptr;
        Uint32 frameCount = 0;
        VkDeviceSize minUploadBytes = 4 * 1024 * 1024;
        VmaMemoryUsage transientMemoryUsage = VMA_MEMORY_USAGE_AUTO;
        VmaAllocationCreateFlags transientAllocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        Bool transientPersistentMapping = false;
        // VK_EXT_transform_feedback is enabled: persistent-map storage additionally
        // carries the transform feedback usage so capture targets can bind directly.
        Bool transformFeedbackUsageEnabled = false;
    };

    // Supplies a command buffer that is recording and outside any render pass,
    // for staged buffer-range copies. Implemented by VulkanRenderer.
    class IBufferCopyCommandProvider {
    public:
        virtual ~IBufferCopyCommandProvider() = default;
        virtual VkCommandBuffer AcquireBufferCopyCommandBuffer() = 0;
    };

    class VkBufferManager {
    public:
        Bool Initialize(const VkBufferManagerInitInfo& initInfo);
        void Shutdown();

        // Registered before caps publication; the initialized renderer owns the storage.
        static void RegisterWireResourceOps();
        // Transport resources are owned by their complete wire handle, never by a
        // frontend BufferObject. Acquires expose the full GPU store, without a CPU
        // pointer: CPU consumers must use ReadWireBuffer for ordered, current bytes.
        Bool AcquireWireSlice(BufferKind kind, MG_Pipe::MGPipeHandle res, BufferSlice& outSlice);
        // The handle names a store this session created (whatever its size, zero included).
        Bool IsKnownWireBuffer(MG_Pipe::MGPipeHandle res) { return FindWireBuffer(res) != nullptr; }
        // Names, for a Fatal about an unknown handle, whether this manager ever saw it.
        void DescribeWireBufferHistory(MG_Pipe::MGPipeHandle res) const;
        Bool ReadWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size, void* dst);
        Bool CopyWireBufferRangeToSlice(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size,
                                        const BufferSlice& dst);
        // P7 A.1, the sub-word half of the one above: the same copy for a window whose start
        // or end does not land on a four-byte boundary. The read of the APPLICATION's store is
        // rounded OUT to whole words and clamped to the store's end, so the widened window can
        // never touch a byte the application does not own; it lands in a transient staging
        // slice, and only the staging -> dst shift is sub-word - inside our own arena, where an
        // unaligned region cannot alias anything else. vkCmdCopyBuffer places no alignment rule
        // on a region's offsets or size (unlike vkCmdUpdateBuffer / vkCmdFillBuffer), so the
        // shift is a plain legal copy. `dstSkip` is the byte inside `dst` the window starts at.
        // This retires `uniform-buffer-byte-tail`.
        Bool CopyWireBufferSubWordRangeToSlice(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size,
                                               Uint32 frameIndex, const BufferSlice& dst, Uint64 dstSkip);
        void MarkWireBufferGpuWritten(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size);
        // P8-D: whether a shader write was marked on this store since the last indirect draw
        // read it natively, clearing the mark. The caller records the INDIRECT_COMMAND_READ
        // barrier before the draw when this answers true. Transfer writes need no mark: their own
        // after-barriers already make them visible to MEMORY_READ at ALL_COMMANDS.
        Bool TakeWireIndirectReadBarrier(MG_Pipe::MGPipeHandle res);
        // P8-D: every WaitForWireBufferHostAccess that actually waited, over this manager's life -
        // the attribution base a caller reads before and after its own ReadWireBuffer.
        Uint64 GetWireHostWaitCount() const { return m_wireHostWaits; }

        // Resource-op entry points. All run on the server apply owner.
        void CreateWireBuffer(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc);
        void RespecifyWireBuffer(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc,
                                 const void* initialBytes);
        void WriteWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size, const void* bytes);
        void FlushWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size, const void* bytes);
        void ReadbackWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 offset, Uint64 size);
        void DestroyWireBuffer(MG_Pipe::MGPipeHandle res);
        // P11 B2 (T0): make the client's AHardwareBuffer `ahb` (a BLOB of `size` bytes) buffer
        // `res`'s store - a VkBuffer on memory imported through
        // VK_ANDROID_external_memory_android_hardware_buffer, host-visible and mapped like every
        // wire store, so every wire path (draw binds, WriteWireBuffer's ordered copy, CPU readers)
        // applies unchanged. The T2 store it replaces goes through DeferWireRelease. False = not
        // imported (the T2 store is untouched and the client is DECLINED).
        Bool ImportWireBuffer(MG_Pipe::MGPipeHandle res, void* ahb, Uint64 size);
        // P13 W4: monolith's record arm donates the wire store itself as the client's persistent
        // map (see the definition). Null when the store cannot be handed over.
        void* DonateWireBuffer(MG_Pipe::MGPipeHandle res, Uint64 size, const void* seedBytes);
        // The POST self-test of the sustained-lock pattern on this device (see the definition).
        static Bool SelfTestWireImport(char* why, Uint64 whyBytes);

        // Recreate all per-frame transient arenas
        Bool RecreateTransientArenas(Uint32 frameCount);
        void BeginFrame(Uint32 frameIndex);
        // Drains every frame slot's deferred buffer/resource releases. Only valid when
        // the caller has proven every queue submission complete; used by the present-less
        // frame-boundary drain. Deliberately does NOT touch the transient arena's parked
        // superseded blocks: those are still named by this frame's slices (see the
        // definition), and only a frame rewind retires them.
        void CollectAllDeferredReleases();
        // All previously submitted GPU work has completed (vkDeviceWaitIdle).
        void NotifyDeviceIdle();
        // A frame slot's submission fence has been waited: every serial up to
        // and including `serial` is complete. Raises the completed floor so
        // GetCompletedSerial reflects real fence progress instead of only the
        // frameSerial-minus-frameCount inference.
        void NotifyFrameSerialComplete(Uint64 serial);
        void SetCopyCommandProvider(IBufferCopyCommandProvider* provider);
        // The owning renderer's submission counter: AcquireWireSlice stamps the next submission
        // from it directly rather than finding the renderer through the session-local slot on
        // every bind (a thread-local probe per vertex, index and uniform buffer of every draw).
        void SetSubmitCounterSource(const Uint64* submitCounter) { m_submitCounter = submitCounter; }

        Bool UploadTransient(BufferKind kind, Uint32 frameIndex, const void* data, VkDeviceSize size,
                             VkDeviceSize alignment, BufferSlice& outSlice);

        // The descriptor a shader storage block gets when the program declares it and the
        // application bound no buffer at its GL binding point. GL 4.6 core 7.8 makes that a
        // legal state - the block simply has no store, so reads are undefined and writes go
        // nowhere - whereas Vulkan has no such thing as an unwritten descriptor, so something
        // real has to sit in the set or the whole draw/dispatch is lost. One zero-filled
        // buffer, created once and shared by every unbound binding: bindings that are only
        // declared (the case this exists for) never touch it, and one that is actually read
        // sees zeros, which is inside GL's "undefined". robustBufferAccess bounds anything
        // that indexes past it.
        BufferSlice AcquireUnboundStorageDescriptor();

        // The store a texel-buffer descriptor - `samplerBuffer` or `imageBuffer` - gets when the
        // unit the program's uniform names has no buffer texture on it, or the buffer texture on
        // it has no GL buffer attached. Both are legal GL states that make a fetch return
        // undefined values (GL 4.6 core 8.9: a buffer texture with no attached buffer object is
        // incomplete, and sampling an incomplete texture is undefined - not a lost draw), and both
        // used to take the whole draw or dispatch with them. The VIEW over this - one per format,
        // and the descriptor is a VkBufferView, not a buffer - is built by
        // UniformManager::AcquireUnboundTexelBufferView.
        BufferSlice AcquireUnboundTexelBufferDescriptor();

        Uint64 GetFrameSerial() const { return m_frameSerial; }
        // Bumped every time this manager destroys a WIRE store's VkBuffer (see
        // m_wireStoreDestroyEpoch). Unchanged since a memo was taken means no VkBuffer handle
        // that memo names can have been freed and re-minted in between, which is the one
        // fact a handle-keyed memo of wire descriptors needs and cannot read off the handle.
        Uint64 GetWireStoreDestroyEpoch() const { return m_wireStoreDestroyEpoch; }
        // Highest frame serial whose GPU work is known complete; serials at or
        // below it may be considered signaled. Drives IsResourceBusy and the
        // backend GL fence objects.
        Uint64 GetCompletedSerial() const;
    private:
        struct WireBufferResource {
            VkBufferObject buffer;
            Uint64 size = 0;
            Uint64 lastUseSerial = 0;
            // P7 wave 2 package B3: the submission expected to carry the most recent GPU use
            // of this buffer - the busy predicate's DEFENCE term, not its guarantee (see
            // WriteWireBuffer). IsSubmitIndexComplete polls the real fence and reports an
            // unsubmitted index as incomplete, but the stamp is taken before the draw is
            // recorded and a mid-draw flush can submit it without the draw.
            Uint64 lastUseSubmitIndex = 0;
            Bool gpuWritesPending = false;
            // Only ranges actually submitted by resource_subdata are covered. No
            // shadow is retained: flush cannot replay stale bytes over GPU writes.
            Vector<Range1D> stagedCoverage;
            // P11 B2 (T0): `buffer` is the client's imported AHardwareBuffer. Its readback posts
            // no bytes (the client reads its own pages once the wait below it is done).
            Bool imported = false;
            // P8-D: a shader write was marked (MarkWireBufferGpuWritten) and no indirect draw has
            // recorded its INDIRECT_COMMAND_READ barrier since (TakeWireIndirectReadBarrier).
            Bool indirectReadBarrierPending = false;
        };
        // P8-D: see GetWireHostWaitCount.
        Uint64 m_wireHostWaits = 0;
        static Uint64 WireBufferKey(MG_Pipe::MGPipeHandle res) {
            return (static_cast<Uint64>(res.Gen) << 32) | res.Slot;
        }
        WireBufferResource* FindWireBuffer(MG_Pipe::MGPipeHandle res);
        Bool WaitForWireBufferHostAccess(WireBufferResource& resource);
        // P11 B2: the serial a T0 store's release waits behind (never "destroy at once").
        Uint64 T0ReleaseSerial(Uint64 lastUseSerial) const;

        // P7 wave 4 (M2), ID-P7-27: THE WIRE ARM'S ORPHANS NEED A RECLAIM THAT IS NOT A FRAME
        // BOUNDARY.
        //
        // RespecifyWireBuffer - which is every glBufferData that crosses the wire - orphans the
        // old store unconditionally, because unlike OnRespecify it has no shadow to upload in
        // place from and no cheap way to know the client is re-sending the same size. The orphan
        // is legitimate (M1 measured the conditional-orphan mirror: 6701 live against 6702).
        // What was missing is the RECLAIM. DeferRelease parks into m_deferredBufferReleases,
        // whose only sweep is CollectDeferredReleases from a frame boundary, and on a pbuffer
        // replay the server sees ONE present record for the whole run - so on
        // minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 the buckets held 25,923 dead stores
        // against 28 live wire buffers, one memfd mapping each, and the server died in scudo's
        // secondary allocator.
        //
        // So a wire store is parked HERE instead, with the facts that say when it is dead, and
        // THE DEFER PATH ITSELF RECLAIMS - every park sweeps, and the frame boundary is only one
        // more sweep point (CollectDeferredReleases), never the one this depends on:
        //
        //   * `lastUseSerial == 0` - read BEFORE RespecifyWireBuffer zeroes it. Zero means no
        //     GPU command has named the store since it was minted or since the last host-access
        //     wait proved every recorded command complete (WaitForWireBufferHostAccess); every
        //     path that hands the store to the GPU stamps m_frameSerial, which starts at 1. Such
        //     a store is destroyed at the park and never enters the list.
        //   * `submitIndex` - the renderer's GetSyncPointSubmitIndex() at park time, which is by
        //     construction the LAST submission that can name this store: every command that
        //     names it was recorded before the park (the record left behind a bumped slice
        //     epoch, so no memo can hand it out again), and a recorded command is either already
        //     submitted (<= m_submitCounter) or in the batch that becomes m_submitCounter + 1.
        //     IsSubmitIndexComplete(submitIndex) is a fence observation, so this is the gate
        //     that empties the set MID-FRAME - the shape CollectWireObjects already uses for
        //     the wire image/view tables.
        //   * THE RENDERER IS IDLE - IsSubmitIndexComplete(GetSyncPointSubmitIndex()): nothing
        //     is recorded and every submission has retired. Then every parked store is dead,
        //     including one tagged for a batch that was abandoned rather than submitted (a
        //     minimized Present drops its recording), whose own index may never complete.
        //
        // THE FRAME-SERIAL FLOOR IS DELIBERATELY NOT A PROOF HERE. It cannot move inside a
        // frame (NotifyFrameSerialComplete refuses the current serial), so it frees nothing in
        // the one-present replay this exists for; the submit index above is the fact that CAN
        // move mid-frame, and it is a fence observation rather than a count.
        //
        // AND EVERY DESTROY HERE HAPPENS MID-FRAME, which the memos above this manager were not
        // written for. UniformManager's descriptor memos are keyed on the VkBuffer HANDLE, and
        // before M2 a wire store only ever died at the same boundary that clears those memos
        // (UniformManager::BeginFrame, from Present or a drain). A store destroyed here can
        // have its handle value re-minted by the next Create - a heap pointer under lavapipe -
        // while a memo still maps that handle to a descriptor set baked to the dead store's
        // memory: silent wrong bytes, no Fatal (ID-P7-43). m_wireStoreDestroyEpoch is the
        // fact the memos fold in: every wire-store destroy bumps it, so a memo taken before
        // the destroy cannot match after it. Deliberately not bumped by WriteWireBuffer, which
        // would defeat the memo on every glBufferSubData.
        struct DeferredWireRelease {
            VkBufferObject buffer;
            Uint64 lastUseSerial = 0;
            Uint64 submitIndex = 0;
            // Cached: GetSize() is gone once the object is destroyed, and the watermark's
            // running total has to stay exact as entries leave.
            Uint64 bytes = 0;
        };
        // Park a wire store, sweep, then hold the watermark. `lastUseSerial` is the releasing
        // resource's, captured before the caller resets it.
        void DeferWireRelease(VkBufferObject&& buffer, Uint64 lastUseSerial);
        // Destroy every parked store proven dead by the rules above. Returns how many.
        SizeT SweepDeferredWireReleases();
        // MOBILEGL_IPC_WIRE_DEFERRED_MB (Config.h has the semantics): when the parked bytes
        // still exceed the budget after a sweep, take the sync point WaitForWireBufferHostAccess
        // takes - flush what is recorded, wait for it - and sweep again, which retires every
        // parked store, because none can be tagged past the sync point it just waited out.
        void EnforceWireDeferredWatermark();
        // ...and the same sync point when more than this many stores are parked, whatever their
        // bytes (see the definition for the measurement). Not a knob: it bounds an object count
        // the byte budget cannot see, and MOBILEGL_IPC_WIRE_DEFERRED_MB=0 disables it too.
        static constexpr SizeT kWireDeferredCountCeiling = 1024;
        // Teardown: the caller has proven the device idle (Shutdown / RecreateTransientArenas).
        void DestroyAllDeferredWireReleases();
        // THE one Fatal{ResourceUnavailable, "buffer-write-sync"} site (rule I: no second abort
        // for the census to count), shared by the host write that cannot wait for the GPU and
        // the watermark that cannot. `site` says which.
        [[noreturn]] static void WireBufferSyncFatal(const char* site);
        // True when the wait or copy that just failed failed because the device is lost: the
        // session has been latched (VulkanRenderer::LatchWireDeviceLoss) and the caller returns
        // instead of taking its Fatal.
        static Bool LatchedOnDeviceLoss(const char* site);
        // The wbuf[] gauges (PipeStats.h, Gauge::WireBuffers..WireDeferredSyncs). The peaks are
        // taken at the two points the numbers can rise - a park and a mint - and published when
        // the stats channel is on; MagmaWireReclaimScenario reads them off the server's line.
        void NoteWireStorePeaks();
        void PublishWireReclaimGauges();
        Bool InitializeTransientArenas();
        Bool StagedWireRangeCopy(WireBufferResource& resource, const void* data, SizeT offset, SizeT size);
        void CollectDeferredReleases(Uint32 frameIndex);
        void DestroyAllDeferredReleases();
        VkBufferManagerInitInfo m_initInfo{};
        BufferArena m_transientUploadArena;
        // See AcquireUnboundStorageDescriptor. Lazily created, never re-created, torn down
        // with the manager.
        VkBufferObject m_unboundStorageBuffer;
        // See AcquireUnboundTexelBufferDescriptor. Same lifetime rules.
        VkBufferObject m_unboundTexelBuffer;
        IBufferCopyCommandProvider* m_copyProvider = nullptr;
        const Uint64* m_submitCounter = nullptr;
        Vector<Vector<VkBufferObject>> m_deferredBufferReleases;
        std::unordered_map<Uint64, WireBufferResource> m_wireBuffers;
        // P14: a slot-indexed front for m_wireBuffers (every draw resolves each vertex, index and
        // uniform buffer). An entry is believed only for the generation it was taken at ({slot,
        // gen} is never reused), and the erase of that node clears it: a node never moves until
        // erased. Slot-indexed rather than direct-mapped because a Minecraft frame draws from a
        // few thousand live buffers, which a small hashed table mostly missed.
        struct WireBufferLookup {
            Uint32 gen = 0;
            WireBufferResource* resource = nullptr;
        };
        static constexpr SizeT kWireBufferLookupMaxSlots = SizeT{1} << 20;
        Vector<WireBufferLookup> m_wireBufferLookup;
        void ForgetWireBufferLookup(Uint64 key) {
            const SizeT slot = static_cast<SizeT>(key & 0xffffffffu);
            if (slot < m_wireBufferLookup.size()) m_wireBufferLookup[slot] = {};
        }
        // See DeferredWireRelease. ONE FLAT LIST rather than the per-frame-slot buckets above:
        // the whole point is that these entries do not wait for a frame slot to come round.
        Vector<DeferredWireRelease> m_deferredWireReleases;
        // Bytes currently parked in that list, kept exact so the watermark needs no walk.
        Uint64 m_deferredWireBytes = 0;
        // Live VkBuffers this arm owns: the stores held by m_wireBuffers plus the parked ones.
        // Maintained rather than counted, because the publish runs on every park.
        Uint64 m_wireStoreCount = 0;
        // Run maxima of the two numbers above and the watermark's sync count, for the gauges.
        Uint64 m_wireStoreCountPeak = 0;
        Uint64 m_deferredWireBytesPeak = 0;
        Uint64 m_wireDeferredSyncs = 0;
        // See GetWireStoreDestroyEpoch and the DeferredWireRelease comment. Bumped on every
        // path that destroys a wire store's VkBuffer, and never reset (not even by Shutdown),
        // so that a memo taken before a re-initialize must not match
        // a handle minted after it.
        Uint64 m_wireStoreDestroyEpoch = 0;
        Uint32 m_currentFrameIndex = 0;
        Uint64 m_frameSerial = 1;
        Uint64 m_completedSerialFloor = 0;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
