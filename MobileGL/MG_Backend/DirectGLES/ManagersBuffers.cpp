// MobileGL - MobileGL/MG_Backend/DirectGLES/ManagersBuffers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ManagersInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    constexpr Bool PREFER_MAP_BUFFER_RANGE_FOR_BUFFER_SYNC = false;

    namespace BufferImpl {
        namespace {
            // GL_ARRAY_BUFFER redundant-bind cache (id 0 = unknown/none).
            Uint g_boundArrayBufferId = 0;
            Bool g_boundArrayBufferKnown = false;

            // Driver-level GL_PIXEL_PACK/UNPACK_BUFFER binding shadows (see
            // Managers.h). Resting state between operations is 0; scopes in the
            // readback/upload paths bind what they need through the cache and
            // return to 0, so a stale user PBO can never capture a later
            // readback that meant to target client memory.
            Uint g_boundPixelPackBufferId = 0;
            Bool g_boundPixelPackBufferKnown = false;
            Uint g_boundPixelUnpackBufferId = 0;
            Bool g_boundPixelUnpackBufferKnown = false;
        } // namespace

            // Bumped whenever the backend ES context is destroyed; resources with
            // an older generation hold ids from a dead context.
            Uint g_bufferContextGeneration = 1;

        namespace {
            // Buffer-mutation epoch backing store (contract, mutation-site list and
            // memory-ordering rules: Managers.h at the accessor declarations).
            // Starts at 1 so the memo stamps' 0 means "never stamped". Atomic:
            // frontend buffer ops may run on non-draw threads while the draw thread
            // reads; the release-bump-AFTER-mutation / acquire-read-BEFORE-probes
            // pairing makes a stamp taken against stale state impossible to consume.
            std::atomic<Uint64> g_bufferMutationEpoch{1};

            // Defined next to the indexed-binding shadow below; forward-declared so
            // every glDeleteBuffers site in this namespace can scrub stale shadow
            // entries (GL resets a deleted buffer's bindings - indexed and pixel
            // pack/unpack alike - to 0, and a recycled name matching a stale shadow
            // entry would otherwise false-skip the rebind).
            void ScrubBufferBindingShadowsForId(Uint id);

            // Defined next to the same shadow, and the counterpart to the scrub above for a
            // buffer whose STORE was re-specified rather than deleted: the binding survives -
            // nothing unbound the id - but the extent the driver resolved for it at bind time
            // does not. Marks those points unknown so the next sync issues a real
            // glBindBufferBase/Range instead of skipping it.
            void InvalidateIndexedBufferBindingShadowsForId(Uint id);

            // Resources whose owning BufferObject died; ids deleted at the next
            // sync point with a current ES context.
            Vector<SharedPtr<BackendBufferResource>> g_deferredBufferReleases;
            std::mutex g_deferredBufferReleasesMutex;
            // Cheap emptiness probe so the per-draw drain can skip the mutex and
            // context check when nothing was enqueued (the overwhelmingly common
            // case). Written only under the mutex; read lock-free.
            std::atomic<Bool> g_hasDeferredBufferReleases{false};

            // --- Buffer-storage pool (buffer-object recycling) -----------------------
            // Recycle idle GL buffer ids of an EXACT byte size instead of glDeleteBuffers
            // (which triggers the kgsl_sharedmem_free -> mmu_unmap -> smmu/power/bandwidth
            // cascade that dominated per-frame driver cost). An id retired during frame N
            // is handed back only once the GPU has completed frame N (fence watermark, see
            // DirectGLES::CompletedFrameSerial), then reseeded in place with glBufferSubData
            // (no glBufferData realloc). All GL access is on the ES-context-owning thread;
            // the mutex only guards against off-thread deferred-release enrollment races.
            struct PooledBuffer {
                Uint id = 0;
                SizeT size = 0;
                Uint contextGeneration = 0;
                Uint64 retireSerial = 0;
            };
            UnorderedMap<SizeT, Vector<PooledBuffer>> g_bufferPool;
            SizeT g_pooledBytes = 0;
            std::mutex g_poolMutex;
            constexpr SizeT kMaxPoolableBufferBytes = 8u * 1024u * 1024u; // bigger buffers: delete now
            constexpr SizeT kMaxPoolBytes = 64u * 1024u * 1024u;          // total pool budget
            constexpr SizeT kMaxEntriesPerBucket = 32;

            Bool IsPoolable(const GLESBufferResource& r) {
                // Require working fences: recycling is gated on the frame-completion
                // watermark, which only advances if Present can insert/poll fences.
                return g_GLESFuncs.glFenceSync != nullptr && g_GLESFuncs.glGetSynciv != nullptr &&
                       r.id != 0 && !r.persistentMapped && !r.immutableStorage &&
                       r.contextGeneration == g_bufferContextGeneration && r.storageInitialized &&
                       r.storageSize > 0 && r.storageSize <= kMaxPoolableBufferBytes;
            }

            // Retire a buffer id into the pool (owning thread; caller verified IsPoolable).
            // Zeroes r.id to keep the single-owner invariant {live | deferred | pool}.
            void EnrollIntoPool(GLESBufferResource& r) {
                if (g_boundArrayBufferKnown && g_boundArrayBufferId == r.id) {
                    InvalidateArrayBufferBindingCache();
                }
                // Pooling keeps the id alive (and thus any driver binding of it);
                // drop to unknown rather than claiming the post-delete 0 state.
                if ((g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == r.id) ||
                    (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == r.id)) {
                    InvalidatePixelBufferBindingCaches();
                }
                const std::lock_guard<std::mutex> lock(g_poolMutex);
                auto& bucket = g_bufferPool[r.storageSize];
                if (bucket.size() >= kMaxEntriesPerBucket || g_pooledBytes + r.storageSize > kMaxPoolBytes) {
                    ScrubBufferBindingShadowsForId(r.id);
                    g_GLESFuncs.glDeleteBuffers(1, &r.id); // over budget: don't pool
                    r.id = 0;
                    return;
                }
                // +1: Present increments the serial at frame END, so during the frame
                // now being built CurrentFrameSerial() reads (frame-1). A buffer used
                // this frame is only GPU-done once THIS frame's fence (serial+1) signals.
                bucket.push_back(
                    {r.id, r.storageSize, r.contextGeneration, DirectGLES::CurrentFrameSerial() + 1});
                g_pooledBytes += r.storageSize;
                r.id = 0;
            }

            // Hand back an idle pooled id of EXACTLY `size` whose GPU work is complete,
            // else 0. Owning thread only. Drops stale-generation entries encountered.
            Uint AcquireFromPool(SizeT size) {
                const Uint64 completed = DirectGLES::CompletedFrameSerial();
                const std::lock_guard<std::mutex> lock(g_poolMutex);
                auto it = g_bufferPool.find(size);
                if (it == g_bufferPool.end()) return 0;
                auto& bucket = it->second;
                for (SizeT i = bucket.size(); i-- > 0;) { // newest-first: hottest + most-likely-idle
                    PooledBuffer& e = bucket[i];
                    if (e.contextGeneration != g_bufferContextGeneration) {
                        g_pooledBytes -= e.size; // dead-context id: drop, no GL
                        bucket[i] = bucket.back();
                        bucket.pop_back();
                        continue;
                    }
                    if (e.retireSerial <= completed) {
                        const Uint id = e.id;
                        g_pooledBytes -= e.size;
                        bucket[i] = bucket.back();
                        bucket.pop_back();
                        return id;
                    }
                }
                return 0;
            }

            // A dead context's ring buffers: shared objects, so they outlive it; deleted at the next
            // Present of any context of the same share-group generation.
            struct OrphanRingBuffer {
                Uint id = 0;
                Uint contextGeneration = 0;
            };
            std::mutex g_orphanRingBuffersMutex;
            Vector<OrphanRingBuffer>& OrphanRingBuffers() {
                static auto* orphans = new Vector<OrphanRingBuffer>();
                return *orphans;
            }

            // Each set's generations start in a range of their own: a per-program slot cache keyed
            // by (generation, offset) must never take one context's ring for another's.
            std::atomic<Uint32> g_ringGenerationBases{0};
        } // namespace

            RingSet::RingSet() {
                const Uint32 base = (g_ringGenerationBases.fetch_add(1, std::memory_order_relaxed) + 1) << 12;
                ubo.store.generation = base;
                unpack.store.generation = base;
                upload.store.generation = base;
            }

            RingSet::~RingSet() {
                const std::lock_guard<std::mutex> lock(g_orphanRingBuffersMutex);
                for (const PersistentRing* ring : {&ubo, &unpack, &upload}) {
                    if (ring->store.id != 0) {
                        OrphanRingBuffers().push_back({ring->store.id, ring->store.contextGeneration});
                    }
                    for (const auto& retired : ring->retired) {
                        if (retired.id != 0) OrphanRingBuffers().push_back({retired.id, retired.contextGeneration});
                    }
                }
            }

            DirectGLES::PerNativeContext<RingSet> g_ringSets;

        namespace {
            PersistentRing& UboRing() { return g_ringSets.Current().ubo; }
            PersistentRing& UnpackRing() { return g_ringSets.Current().unpack; }
            PersistentRing& UploadRing() { return g_ringSets.Current().upload; }

            // The ES context the ring's id/map belonged to is gone (or was never
            // seen): drop every handle without GL calls and re-arm creation. The
            // generation counter must survive the reset — frame serials also survive
            // context recreation, so a restarted counter could revalidate a stale
            // per-program slot cache against the new ring.
            void ResetRingForNewContext(PersistentRing& ring) {
                const Uint32 keptGeneration = ring.store.generation;
                ring.store = {};
                ring.store.generation = keptGeneration;
                ring.store.contextGeneration = g_bufferContextGeneration;
                // Keep the ring's own alignment across the wipe: the slow path rounds a
                // request with it BEFORE the store that would set it exists.
                if (ring.fixedAlignment != 0) ring.store.alignment = ring.fixedAlignment;
                ring.retired.clear();
                ring.frameMarks.clear();
            }

            // P3a: ONE body per helper, parameterised on where its host bytes and its
            // extent come from, so the legacy arm and the handle arm of a push build share
            // the three-tier flush, the respecify and the range upload rather than owning a
            // copy each - a second copy of the tier ladder is exactly how a tier changes in
            // silence. The #else below is the PULL build, whose text must stay byte-identical
            // to the pre-P3a one (G1: the pull build gains no symbol and resizes none); it is
            // frozen by that gate and retires with the pull path at P13.
            // (Re)specify backend storage from the shadow copy: glBufferData.
            // The orphaning point - the ES driver performs the actual rename.
            // TODO(buffer-pool Phase 2): orphan-on-respecify is NOT yet implemented.
            // When the current id is BUSY (lastUseFrameSerial > CompletedFrameSerial())
            // && !persistentMapped && !noOrphan, express the orphan as an id-swap
            // (retire the busy id into the pool, bind a fresh/pooled id) instead of the
            // in-place glBufferData below, to avoid the driver's own rename/stall. Not
            // pursued yet: glBufferData/glBufferSubData currently sit below profiler
            // noise, so respecify is not a hot path in the profiled scenes.
            // The body both arms run. `size`, `usage`, `initialData` and `syncedSerial` are the
            // four things the legacy arm reads off the frontend object and the handle arm reads
            // off the applier's stored descriptor and the shadow base the call carried; nothing
            // else in here differs, so there is ONE glBufferData and one extent rule rather
            // than two that can drift apart.
            void RespecifyStorageWith(GLESBufferResource& resource, SizeT size, GLenum usage,
                                      const void* initialData, Uint64 syncedSerial) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                // Read BEFORE the fields below are overwritten: whether this respecify changes
                // the store's EXTENT is what decides if the indexed-binding shadow still
                // describes the driver.
                const Bool extentChanged = !resource.storageInitialized || resource.storageSize != size;
                BindBufferId(TempBufferTarget, resource.id);
                g_GLESFuncs.glBufferData(TempBufferTarget, (GLsizeiptr)size, initialData, usage);
                if (MG_Util::PipeStats::Enabled() && initialData != nullptr) {
                    // An ORPHANING respecify passes NULL and moves nothing, which is exactly
                    // why the test is on initialData rather than on size.
                    MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                                 static_cast<Uint64>(size));
                }
                resource.storageSize = size;
                resource.storageInitialized = true;
                resource.pendingRespecify = false;
                resource.pendingRanges.clear();
                resource.pendingResidentWrites.clear();
                resource.syncedChangeSerial = syncedSerial;
                // A GROWN store keeps its indexed bindings, and BindBufferBaseCached skips a
                // rebind whenever the shadow already records this id at that index - so on a
                // driver that resolves a whole-buffer indexed binding's extent at BIND time
                // (Adreno does; Mali does not) the shader keeps seeing the old, smaller range:
                // stores past it are dropped and loads return zero. Forget what the shadow
                // claims for this id so the next SyncBufferBindingPoints issues the bind for
                // real. Only when the extent actually moved: an orphaning respecify at the same
                // size is Minecraft's per-frame hot path and its bindings are still exact.
                if (extentChanged) {
                    InvalidateIndexedBufferBindingShadowsForId(resource.id);
                }
            }

            void RespecifyStorageNow(GLESBufferResource& resource, BufferObject& bufferObject) {
                const SizeT size = bufferObject.GetSize();
                // An orphaning respecify (glBufferData with NULL, content never
                // written since) stays a pure NULL reallocation: the driver renames
                // the store without a stall and nothing is transferred. Uploading
                // the stale shadow here turned Minecraft-style orphaning into a
                // full-size synchronized upload.
                const void* initialData =
                    (size > 0 && bufferObject.HasDefinedContent()) ? bufferObject.MappedData() : nullptr;
                RespecifyStorageWith(resource, size, MG_Util::ConvertBufferUsageToGLEnum(bufferObject.GetUsage()),
                                     initialData, bufferObject.GetChangeSerial());
            }

            Bool StorageMatchesSize(const GLESBufferResource& resource, SizeT size) {
                return resource.storageInitialized && !resource.pendingRespecify && resource.storageSize == size;
            }

            Bool StorageMatches(const GLESBufferResource& resource, const BufferObject& bufferObject) {
                return StorageMatchesSize(resource, bufferObject.GetSize());
            }
        } // namespace

            // -------------------------------------------------------------------------------
            // R-11 - THE SERVER'S OWN COPY OF THE STAGED BYTES. Package v1.
            //
            // The rule, the evidence and the reason the storage is a side table rather than a
            // member of GLESBufferResource are all in MG_Remote/Server/StagedShadow.h. This is
            // only the instance and the four call sites.
            //
            // ONE PER PROCESS, and its copying arm is decided ONCE at first use: the two arms
            // hold the authoritative bytes in DIFFERENT places, so an answer that changed
            // mid-run would strand every resource already staged - the same reason
            // ResolveResourceSubsystemArm latches (Managers.h). Leaked at exit like every other
            // MG_Remote singleton (ID-8): ~BufferObject reaches the destroy path from exit
            // handlers, after this TU's globals would already be gone.
            // -------------------------------------------------------------------------------
            MG_Record::StagedShadowStore& ServerStaged() {
                static MG_Record::StagedShadowStore& store =
                    *new MG_Record::StagedShadowStore(
                        MG_Config::Transport != MG_Config::TransportMode::Monolith);
                return store;
            }

        namespace {
#if MOBILEGL_BUILD_DISAGGREGATED
            // -------------------------------------------------------------------------------
            // P11 B2 (T0) - WHEN AN IMPORTED STORE'S AHardwareBuffer MAY BE LET GO.
            //
            // A T0 store is the client's AHardwareBuffer imported with glBufferStorageExternalEXT
            // (Ops_H_ImportExternal below). The twin holds its own reference, and every path that
            // ends the store - a respecify, a destroy, a re-import - deletes the GL id and then
            // RETIRES the reference here: a fence is inserted after the delete, and the reference is
            // released only once that fence has signalled, i.e. once every command that could still
            // read or write the pages has completed on this context's GPU. The client's own
            // reference ends independently (it releases on respecify/destroy); the pages live until
            // the last of the two. No link watermark is involved (F-L2): the fence is this server's.
            //
            // Swept (non-blocking) at every import, every deferred-release drain and every ensure;
            // a context loss releases the lot, because the GPU work those fences named ended with
            // the context. Apply thread only, like every GL call here.
            // -------------------------------------------------------------------------------
            struct T0RetiredImport {
                GLsync fence = nullptr;
                void* ahb = nullptr;
                Uint generation = 0;
            };
            Vector<T0RetiredImport> g_t0Retired;
            // T0 twins whose destroy found no current context: Ops_OnDestroy queued them on the
            // deferred-release list and keeps a second reference here, so the reference is
            // retired only after that drain deleted the id (ProcessDeferredT0Retires). Guarded by
            // g_deferredBufferReleasesMutex, like the list it shadows.
            Vector<SharedPtr<BackendBufferResource>> g_t0DeferredDestroys;
            std::atomic<Bool> g_hasT0DeferredDestroys{false};
            Uint64 g_t0ImportsLive = 0;

            void SweepT0Retired(Bool contextGone) {
                for (SizeT i = 0; i < g_t0Retired.size();) {
                    T0RetiredImport& retired = g_t0Retired[i];
                    const Bool sameContext = !contextGone && retired.generation == g_bufferContextGeneration &&
                                             CanTouchGLNow();
                    Bool done = contextGone || retired.generation != g_bufferContextGeneration ||
                                retired.fence == nullptr;
                    if (!done && sameContext && g_GLESFuncs.glClientWaitSync) {
                        const GLenum status = g_GLESFuncs.glClientWaitSync(retired.fence, 0, 0);
                        done = status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED ||
                               status == GL_WAIT_FAILED;
                    }
                    if (!done) {
                        ++i;
                        continue;
                    }
                    if (retired.fence != nullptr && sameContext && g_GLESFuncs.glDeleteSync)
                        g_GLESFuncs.glDeleteSync(retired.fence);
                    MG_Remote::Transport::AdoptT0::ReleaseImported(retired.ahb);
                    retired = g_t0Retired.back();
                    g_t0Retired.pop_back();
                }
            }

            // Call AFTER the store's GL id is deleted (or known dead): the fence then follows every
            // command that used it. Clears the twin's reference whatever the context state.
            void RetireT0Import(GLESBufferResource& resource) {
                if (resource.externalAhb == nullptr) return;
                T0RetiredImport retired;
                retired.ahb = resource.externalAhb;
                retired.generation = resource.contextGeneration;
                if (CanTouchGLNow() && resource.contextGeneration == g_bufferContextGeneration &&
                    g_GLESFuncs.glFenceSync) {
                    retired.fence = g_GLESFuncs.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                }
                resource.externalAhb = nullptr;
                if (g_t0ImportsLive > 0) --g_t0ImportsLive;
                g_t0Retired.push_back(retired);
                SweepT0Retired(false);
            }
#else
            // No transport, no imported store: nothing ever needs retiring (P13 W5).
            inline void RetireT0Import(GLESBufferResource&) {}
#endif // MOBILEGL_BUILD_DISAGGREGATED
#define MGL_SERVER_STAGED_DROP(res) ServerStaged().Drop(&(res))
#define MGL_SERVER_STAGED_DROP_ALL() ServerStaged().DropAll()
#define MGL_SERVER_STAGED_REQUIRE(res, base, start, end, site)                                                         \
    ServerStaged().RequireCoverage(&(res), (base), (start), (end), (site))

            // The host bytes a range upload/flush reads. On the legacy arm the frontend object's
            // shadow; on the handle arm the base the last content-carrying call handed over.
            void UploadRangeFrom(GLESBufferResource& resource, const Uint8* hostBase, SizeT start, SizeT end) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                if (start >= end || hostBase == nullptr) return;
                MGL_SERVER_STAGED_REQUIRE(resource, hostBase, start, end, "upload_range");
                BindBufferId(TempBufferTarget, resource.id);
                g_GLESFuncs.glBufferSubData(TempBufferTarget, (GLintptr)start, (GLsizeiptr)(end - start),
                                            hostBase + start);
            }

            void UploadRangeNow(GLESBufferResource& resource, BufferObject& bufferObject, SizeT start, SizeT end) {
                UploadRangeFrom(resource, bufferObject.MappedData(), start, end);
            }
            Bool RingAvailable(PersistentRing& ring);

            // True when a pending-range flush can go through the staging ring right
            // now: kill switch off, the ES copy entry point resolved, and the ring's
            // own availability gate (EXT_buffer_storage + fences + live context) up.
            Bool UploadRingUsableNow() {
                if (MG_Config::Features.EsprytDisableUploadRing) return false;
                if (!g_GLESFuncs.glCopyBufferSubData) return false;
                return RingAvailable(UploadRing());
            }

            // A partial range below this goes through the staging ring instead of a
            // range-invalidating map: the map's page-substitution fast path needs a
            // sizeable (page-coverable) range to engage, and below it the driver
            // falls back to waiting out the WAR hazard on the CPU.
            constexpr SizeT kInvalidateRangeMinBytes = 128u * 1024u;
            // The push arm's copy of this threshold lives in Managers.h, where a unit case can
            // reach it (InvalidateFlushAccessFor). Two constants, one value, and the compiler
            // is what keeps them one: the pull arm's FlushPendingRangesNow is byte-frozen
            // against 5cb826b0 (ID-15), so the constant it reads may not move to a header.
            static_assert(kInvalidateRangeMinBytes == kEsprytInvalidateRangeMinBytes,
                          "the two arms of the three-tier ladder must use the same tier-1 threshold");
        } // namespace
        void FlushPendingRangesFrom(GLESBufferResource& resource, const Uint8* hostBase, SizeT frontendSize,
                                    SizeT hostBaseFrom, SizeT hostBaseTo) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                VecRange1D ranges;
                {
                    const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                    if (resource.pendingRanges.empty()) return;
                    ranges = std::move(resource.pendingRanges);
                    resource.pendingRanges.clear();
                }
                // No shadow to read from: only reachable on the handle arm, where the base
                // arrives with the call, and only for a resource that queued a range before any
                // content-carrying call named one. The queue is already drained, so the next
                // full re-upload is what puts the store right.
                if (hostBase == nullptr) return;
                // Clamp against BOTH extents: the readback flush may run while the
                // frontend size and the backend store disagree (a pending respecify
                // resolves that later; bytes past either end have nowhere to land).
                const SizeT limit = std::min(frontendSize, resource.storageSize);
                const Bool mapUsable = !MG_Config::Features.EsprytDisableInvalidateFlush &&
                                       g_GLESFuncs.glMapBufferRange && g_GLESFuncs.glUnmapBuffer;
                const Bool ringUsable = UploadRingUsableNow();
                for (const auto& range : ranges) {
                    const SizeT end = std::min(range.end, limit);
                    const SizeT start = std::min(range.start, end);
                    const SizeT size = end - start;
                    if (size == 0) continue;
                    if (MG_Util::PipeStats::Enabled()) {
                        // Counted once per queued range, before the three delivery shapes
                        // below diverge: all three move exactly these bytes, and it is the
                        // byte count - not the shape - that sizes SEG_STAGE.
                        MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                                     static_cast<Uint64>(size));
                    }
                    // The invalidating map's fast path is SHAPE-dependent on this Mali
                    // driver: a whole-buffer invalidation renames the store outright,
                    // and a large range gets fresh pages - but a small unaligned range
                    // of a busy store makes the map WAIT (osup_sync_object_wait, ~9%
                    // of a Minecraft 26.3 replay). So: whole buffer -> orphan-map;
                    // large range -> range-invalidating map; small range -> the staged
                    // ring copy, whose worst case (a whole-destination ghost) is only
                    // ever the small destination itself.
                    //
                    // The map covers EXACTLY the queued range: only those bytes are the
                    // shadow's to rewrite. Widening to page bounds looked free and was
                    // not - the widened bytes clobbered GPU-written data (an SSBO
                    // counter beside the app's SubData) with the stale shadow.
                    // The extent the bytes behind `hostBase` are actually good for, clamped
                    // to the store. With the default it IS [start, end), so the refusal below
                    // cannot fire and the ladder is unchanged; with a real SEG_STAGE snapshot
                    // it is the snapshot's window and a disagreement drops this range to the
                    // staging ring instead of letting a widened INVALIDATE_RANGE declare bytes
                    // dead that nothing is about to rewrite.
                    const SizeT coveredFrom = hostBaseFrom > start ? hostBaseFrom : start;
                    const SizeT coveredTo = hostBaseTo < end ? hostBaseTo : end;
#if MOBILEGL_PIPE_VERIFY
                    if (coveredFrom != start || coveredTo != end) {
                        MGLOG_E_ONCE("MGPipe: Fatal{StageSnapshotTooNarrow} flush_pending_ranges: the "
                                     "staged bytes cover [%zu, %zu) and the queued range is [%zu, %zu) "
                                     "- tiers 2 and 3 would copy from outside the snapshot",
                                     hostBaseFrom, hostBaseTo, start, end);
                    }
#endif
                    // Read per range, like the pull arm: a ring copy staged for an earlier range of
                    // this same drain puts every later partial range of the store behind it
                    // (InvalidateFlushAccessFor's queued-copy rule, dev f973008c).
                    const Bool ringCopyInFlight = resource.ringCopyRetireSerial > DirectGLES::CompletedFrameSerial();
                    const GLbitfield access =
                        mapUsable ? InvalidateFlushAccessFor(start, end, coveredFrom, coveredTo, limit,
                                                             resource.storageSize, ringCopyInFlight)
                                  : 0u;
                    if (access != 0) {
                        BindBufferId(TempBufferTarget, resource.id);
                        void* dst = g_GLESFuncs.glMapBufferRange(TempBufferTarget, (GLintptr)start,
                                                                 (GLsizeiptr)size, access);
                        if (dst) {
                            Memcpy(dst, hostBase + start, size);
                            g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
                            continue;
                        }
                    }
                    SizeT ringOffset = 0;
                    if (ringUsable && size <= kUploadRingMaxBytes &&
                        RingAllocate(UploadRing(), size, ringOffset)) {
                        Memcpy(UploadRing().store.mappedPtr + ringOffset, hostBase + start, size);
                        BindBufferId(GL_COPY_READ_BUFFER, UploadRing().store.id);
                        BindBufferId(GL_COPY_WRITE_BUFFER, resource.id);
                        g_GLESFuncs.glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                                        (GLintptr)ringOffset, (GLintptr)start, (GLsizeiptr)size);
                        // Commands recorded now retire with the fence the next Present inserts.
                        resource.ringCopyRetireSerial = DirectGLES::CurrentFrameSerial() + 1;
                    } else {
                        UploadRangeFrom(resource, hostBase, start, end);
                    }
                }
        }

        namespace {
            // THE TWO-ARM SHAPE, AND BOTH ARMS ARE HASHED FROM HERE ON (ID-15, which
            // supersedes ID-13's "FlushPendingRangesNow is defined exactly once, outside any
            // `#if`").
            //
            // This file has TWO three-tier drains, and exactly one of them is compiled into any
            // given build: `FlushPendingRangesFrom` above, inside `#if MOBILEGL_PIPE_PUSH`, is
            // what a PUSH build runs - both of its call sites, the legacy one and
            // Ops_H_Readback, reach it - and `FlushPendingRangesNow` inside the `#else` below,
            // byte-identical to 5cb826b0, is what a PULL build runs. A push build compiles no
            // FlushPendingRangesNow at all.
            //
            // NO FORWARDER HERE, which is what forced the two-arm shape: G5's extractor is
            // preprocessor-blind, so a `FlushPendingRangesNow` that forwarded onto the ladder
            // above would leave TWO definitions of that one name in the file and the gate would
            // exit 2 - a gate that cannot run - rather than compare anything. (The handle arm
            // cannot call FlushPendingRangesNow itself either: its signature takes a
            // BufferObject&, and having no frontend object to offer is the whole point of the
            // conversion.)
            //
            // WHAT ID-15 ADDS. The shape above is sound, but a gate keyed on the NAME
            // `FlushPendingRangesNow` alone would from here on protect only the text the
            // shipping build never compiles: a tier-threshold or map-access-bit edit made in
            // FlushPendingRangesFrom - the ladder that actually runs, and the one MC 26.3's p99
            // depends on - would satisfy both G1 (the pull text did not move) and G5 (the pull
            // name still hashes the same). So scripts/p3a_untouched_regions.sh hashes BOTH
            // names: eleven functions, the pull ladder compared against the P3a base ref and
            // this one against a sha pinned at 3e298c9a. Neither ladder may drift, and neither
            // may drift AWAY FROM THE OTHER without the gate saying so.

            // P15: the small-store write rename (WholeStoreRenameApplies, Managers.h), applied to the
            // queue BEFORE the drain takes it. The ladder above is pinned and is not touched: it is
            // handed one whole-store range, which its own tier 1 already answers with the
            // whole-buffer orphan-map. Draw path only - the readback drains keep their exact ranges.
            void RenameSmallStoreForDrain(GLESBufferResource& resource, const Uint8* hostBase, SizeT frontendSize) {
                const SizeT store = resource.storageSize;
                const Bool mapUsable = !MG_Config::Features.EsprytDisableInvalidateFlush &&
                                       g_GLESFuncs.glMapBufferRange && g_GLESFuncs.glUnmapBuffer;
                const Bool foreignBytes = resource.serverGpuWritten || resource.persistentMapped ||
                                          resource.immutableStorage || resource.externalAhb != nullptr;
                // The size gate first, with the whole store as the queue: it answers every refusal
                // that does not depend on the queue before any lock is taken.
                if (!WholeStoreRenameApplies(store, frontendSize, store, hostBase != nullptr, foreignBytes, mapUsable))
                    return;
                // Under split the host bytes are the server's staged copy, which only holds what the
                // content records delivered; monolith's base is the frontend shadow, the whole store.
                if (ServerStaged().CopiesIntoServerStorage() && !ServerStaged().IsCovered(&resource, 0, store)) return;
                const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                if (resource.pendingRanges.empty() || !resource.pendingResidentWrites.empty()) return;
                SizeT queuedBytes = 0;
                for (const auto& range : resource.pendingRanges) {
                    const SizeT end = std::min(range.end, store);
                    queuedBytes += end - std::min(range.start, end);
                }
                if (!WholeStoreRenameApplies(store, frontendSize, queuedBytes, true, false, true)) return;
                resource.pendingRanges.clear();
                resource.pendingRanges.Add({0, store});
            }
        } // namespace

            // Land the app bytes queued for an ADOPTED store on the GPU timeline: staged
            // into the upload ring and delivered by glCopyBufferSubData. The destination
            // is the IMMUTABLE persistent store, which the driver can neither rename nor
            // ghost, so the copy is plain job ordering - after every in-flight reader,
            // before the next consumer - which is exactly glBufferSubData's contract.
            // (The in-place host write these bytes replaced tore the frames still
            // reading the old vertex data: one-frame wrong geometry during fast camera
            // movement.) Fallback: direct glBufferSubData - the adopted store carries
            // DYNAMIC_STORAGE, and immutability again forbids the whole-store ghost.
            //
            // It never read the frontend object (the bytes are on the resource's own queue and
            // the extent is the backend store's), so it takes none: the handle arm calls exactly
            // this function with exactly these semantics.
            void DrainResidentWritesNow(GLESBufferResource& resource) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                Vector<GLESBufferResource::PendingResidentWrite> writes;
                {
                    const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                    if (resource.pendingResidentWrites.empty()) return;
                    writes = std::move(resource.pendingResidentWrites);
                    resource.pendingResidentWrites.clear();
                }
                const SizeT limit = resource.storageSize;
                const Bool ringUsable = UploadRingUsableNow();
                for (const auto& write : writes) {
                    if (write.offset >= limit) continue;
                    const SizeT size = std::min(write.bytes.size(), limit - write.offset);
                    if (size == 0) continue;
                    if (MG_Util::PipeStats::Enabled()) {
                        MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                                     static_cast<Uint64>(size));
                    }
                    SizeT ringOffset = 0;
                    if (ringUsable && size <= kUploadRingMaxBytes &&
                        RingAllocate(UploadRing(), size, ringOffset)) {
                        Memcpy(UploadRing().store.mappedPtr + ringOffset, write.bytes.data(), size);
                        BindBufferId(GL_COPY_READ_BUFFER, UploadRing().store.id);
                        BindBufferId(GL_COPY_WRITE_BUFFER, resource.id);
                        g_GLESFuncs.glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                                        (GLintptr)ringOffset, (GLintptr)write.offset,
                                                        (GLsizeiptr)size);
                    } else {
                        BindBufferId(TempBufferTarget, resource.id);
                        g_GLESFuncs.glBufferSubData(TempBufferTarget, (GLintptr)write.offset, (GLsizeiptr)size,
                                                    write.bytes.data());
                    }
                }
            }

        namespace {
            // EXT_buffer_storage bit values (same numeric values as the desktop ARB
            // tokens); defined locally so this compiles regardless of which GLES headers
            // expose the EXT tokens.
            constexpr GLbitfield kMapPersistentBit = 0x0040;
            constexpr GLbitfield kMapCoherentBit = 0x0080;
            constexpr GLbitfield kDynamicStorageBit = 0x0100;

            void Ops_OnDestroy(SharedPtr<BackendBufferResource>&& resource) {
                if (!resource) return;
                auto* glesResource = static_cast<GLESBufferResource*>(resource.get());
                if (glesResource->contextGeneration != g_bufferContextGeneration) {
                    glesResource->id = 0; // id belonged to a destroyed context
                    RetireT0Import(*glesResource); // P11 B2: its GL store died with that context
                    return;
                }
                if (CanTouchGLNow()) {
                    if (IsPoolable(*glesResource)) {
                        EnrollIntoPool(*glesResource); // recycle instead of glDeleteBuffers
                        return;
                    }
                    if (glesResource->id != 0) {
                        if (g_boundArrayBufferKnown && g_boundArrayBufferId == glesResource->id) {
                            InvalidateArrayBufferBindingCache();
                        }
                        ScrubBufferBindingShadowsForId(glesResource->id);
                        g_GLESFuncs.glDeleteBuffers(1, &glesResource->id);
                        glesResource->id = 0;
                    }
                    // P11 B2: a T0 store's AHardwareBuffer goes only after a fence behind the delete.
                    RetireT0Import(*glesResource);
                    return;
                }
                const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
#if MOBILEGL_BUILD_DISAGGREGATED
                if (glesResource->externalAhb != nullptr) { // P11 B2: retired after the drain deletes it
                    g_t0DeferredDestroys.push_back(resource);
                    g_hasT0DeferredDestroys.store(true, std::memory_order_release);
                }
#endif
                g_deferredBufferReleases.push_back(std::move(resource));
                g_hasDeferredBufferReleases.store(true, std::memory_order_release);
            }
            // ResourceContentIsDeclared MOVED OUT OF THIS ANONYMOUS NAMESPACE (P3b/P4b espryt
            // D1 slice 1) to sit beside RequireStagedCoverage at namespace scope: the XFB
            // scatter in DirectGLES.cpp has to ask the same M-3 question this file's pool-reuse
            // ladder asks, and the two must not be able to answer it differently.

            void Ops_H_Create(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc) {
                (void)res;
                (void)desc;
                // Nothing, and that is the row D-A2 writes: storage is defined lazily by the
                // first resource_respecify, and the ensure path already tolerates a resource
                // with none. Minting the twin here would only move the allocation earlier.
            }

            void Ops_H_Respecify(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc,
                                 const void* initialBytes) {
                auto* resource = FindBufferResourceForHandle(res);
                // P13 W4a (§1.4): ON MONOLITH'S RECORD ARM THE TWIN IS MINTED HERE, not lazily at
                // the first draw. The record arm's readers have no frontend object to re-read a
                // base from, so the base this call carries - the client's shadow, for an orphaning
                // respecify too (PipeFill passes it there on this arm) - is the only way they learn
                // it. Minting is GL-free; the storage stays lazy.
                const Bool aliasesFrontend = MG_Config::RecordArmAliasesFrontend();
                if (resource == nullptr && aliasesFrontend && initialBytes != nullptr) {
                    resource = GetOrCreateBufferResourceForHandle(res);
                }
                if (resource != nullptr) {
                    // A respecify's companion pointer IS the shadow base (offset 0) - and an
                    // ORPHANING one (glBufferData with NULL) carries HasDefinedContent clear and
                    // therefore no pointer at all. That is also the call that RESIZES the
                    // client's shadow (PipeResource::ResizeShadow is reserve + resize, so a grow
                    // past the reserve reallocates and frees the block a previous call's base
                    // pointed into), so the recorded base is dead exactly when no new one
                    // arrives. FORGET it here rather than leave a pointer nothing refreshes:
                    // every reader below treats a null base as "no bytes to move", which is the
                    // honest answer, and the ensure path re-reads the live base from the
                    // frontend object it still holds.
                    // P13 W4a: an ALIASING store records the base whether or not the content is
                    // defined - the pointer is the frontend shadow's new allocation either way, and
                    // the upload below stays gated on the descriptor (RespecifyStorageWith's
                    // initialData), so an orphan still uploads nothing.
                    if ((desc.HasDefinedContent != 0 || aliasesFrontend) && initialBytes != nullptr) {
                        // R-11: in monolith this is the client's shadow base, unchanged; under
                        // split it is copied into server-owned storage first. A respecify's
                        // companion pointer is the base at offset 0 and covers the whole store.
                        // Under split it is ALWAYS null (contract table 1 row 19 -
                        // initialBytes does not cross, and the content arrives as
                        // resource_subdata records right behind this record), so in practice
                        // this arm is the monolith one and the else arm is the split one.
                        resource->hostBytes = MGL_SERVER_STAGED_ADOPT(*resource, static_cast<SizeT>(desc.Width),
                                                                      initialBytes, 0,
                                                                      static_cast<SizeT>(desc.Width));
                    } else {
                        MGL_SERVER_STAGED_DROP(*resource);
                        resource->hostBytes = nullptr;
                    }
                    // P8-C: whatever a shader wrote belonged to the store this call replaces.
                    resource->serverGpuWritten = false;
                    resource->contentSinceRespecify = false;
                }
                if (!resource) return; // lazy: the ensure path full-uploads on creation
                if (resource->immutableStorage) {
                    resource->persistentMapped = false;
                    resource->persistentPtr = nullptr;
                    if (resource->id != 0 && CanTouchGLNow() &&
                        resource->contextGeneration == g_bufferContextGeneration) {
                        NoteBufferIdDeleted(resource->id);
                        // Frontend VAO bindings survive respecification; force their
                        // backend twins to bind the replacement buffer name. (dev@d7655247,
                        // carried into this arm as well: the handle arm duplicates the
                        // immediate retire path, so a fix that lands in only one of the two
                        // leaves the bug alive on whichever arm the operator selects.)
                        ++g_bufferBackendIdGeneration;
                        g_GLESFuncs.glDeleteBuffers(1, &resource->id);
                        resource->id = 0;
                        resource->immutableStorage = false;
                    }
                    // P11 B2: a respecified T0 store ends here on the server's side too. Its
                    // AHardwareBuffer is released only after a fence behind the delete (or at once
                    // when the store's context is already gone) - never under a frame in flight,
                    // however the client's own release (at this respecify) raced it.
                    RetireT0Import(*resource);
                    resource->storageInitialized = false;
                    resource->storageSize = 0;
                    resource->pendingRespecify = true;
                    resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                    return;
                }
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration) {
                    resource->pendingRespecify = true;
                    resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                    return;
                }
                if (desc.Width == 0) {
                    resource->storageInitialized = false;
                    resource->storageSize = 0;
                    resource->pendingRespecify = false;
                    resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                    return;
                }
                // Same orphaning rule, expressed on the descriptor: a NULL-data respecify has
                // HasDefinedContent clear and the client sends no bytes for it.
                const void* initialData = desc.HasDefinedContent != 0 ? initialBytes : nullptr;
                RespecifyStorageWith(*resource, static_cast<SizeT>(desc.Width),
                                     MG_Util::ConvertBufferUsageToGLEnum(static_cast<BufferUsage>(desc.Usage)),
                                     initialData, ResourceSerialOf(res));
            }

            void Ops_H_SubData(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPSubData& record, const void* bytes) {
                const SizeT offset = static_cast<SizeT>(MG_Pipe::MGPipeSubDataBufferOffset(record));
                const SizeT size = static_cast<SizeT>(MG_Pipe::MGPipeSubDataBufferSize(record));
                auto* resource = FindBufferResourceForHandle(res);
                // R-11 / ID-52 item 3: under an ACTIVE TRANSPORT this record's bytes are the ONLY
                // delivery of the buffer's content - the client sends no companion pointer and the
                // server has no MappedData to fall back on. But the twin is created LAZILY at the
                // first draw (Ops_H_Create is a no-op by D-A2), which is AFTER this record, so a
                // subdata that finds no twin would return here and the bytes would be lost - the
                // draw then uploads an empty/shifted store (TriangleScenario read a blue triangle
                // before this). So under split the subdata MINTS the twin it needs to stage into.
                // Monolith is untouched: the twin stays lazy there because the frontend object's
                // MappedData is the source and nothing is lost by deferring the allocation (D-A2).
                if (resource == nullptr && bytes != nullptr) {
                    resource = GetOrCreateBufferResourceForHandle(res);
                }
                if (!resource) return;
                // P11 B2: A T0 STORE TAKES A SERVER-ORIGINATED WRITE GPU-ORDERED. Its pages are the
                // client's AHardwareBuffer, so there is no staged copy to refresh and the adopted
                // early return below ("the coherent map already has the bytes") is false for bytes
                // the SERVER produced - P9 W1's pack-buffer landing (PipeApplier.cpp
                // LandReadbackInBuffer) is the one producer, and it would lose its pixels there. They
                // are queued exactly as a resident SubData is (Ops_H_ResidentSubData): landed after
                // every command already recorded, before the next one that reads the store.
                if (resource->externalAhb != nullptr && bytes != nullptr && size != 0) {
                    {
                        const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                        auto& write = resource->pendingResidentWrites.emplace_back();
                        write.offset = offset;
                        const auto* source = static_cast<const Uint8*>(bytes);
                        write.bytes.assign(source, source + size);
                    }
                    resource->syncedChangeSerial = ResourceSerialOf(res);
                    return;
                }
                // P13 W4b: A SERVER-ORIGINATED WRITE INTO AN ALIASING STORE (monolith's record arm)
                // lands in the GL store, never in the base: the bytes are the sink's scratch, and the
                // base is the frontend's shadow, which this side does not write. Queued uploads land
                // first, so they cannot later overwrite these bytes from the stale shadow; the store
                // is then marked GPU-written, which sends the frontend (its own pack-buffer mark) and
                // every CPU reader here (SplitHostBytesForCpuRead) back to the GL store for them.
                if (MG_Pipe::MGPipeApplyingServerOriginatedWrite() && MG_Config::RecordArmAliasesFrontend() &&
                    bytes != nullptr && size != 0) {
                    // The store must exist to take them: a pack buffer is often never drawn from, so
                    // nothing else has built it. Only a store that does not exist yet is ensured: the
                    // applier has already counted THIS write into the record's serial, so the ensure
                    // of a built store would read it as an untracked mutation and re-upload the
                    // shadow over the runs this landing wrote before it.
                    if (CanTouchGLNow() && (resource->id == 0 || !resource->storageInitialized ||
                                            resource->pendingRespecify ||
                                            resource->contextGeneration != g_bufferContextGeneration)) {
                        resource = EnsureBufferResourceForHandle(nullptr, res);
                    }
                    if (resource != nullptr && CanTouchGLNow() && resource->id != 0 && resource->storageInitialized &&
                        !resource->persistentMapped && !resource->pendingRespecify &&
                        resource->contextGeneration == g_bufferContextGeneration &&
                        StorageMatchesSize(*resource, ResourceWidthOf(res))) {
                        FlushPendingRangesFrom(*resource, resource->hostBytes, ResourceWidthOf(res));
                        BindBufferId(TempBufferTarget, resource->id);
                        g_GLESFuncs.glBufferSubData(TempBufferTarget, static_cast<GLintptr>(offset),
                                                    static_cast<GLsizeiptr>(size), bytes);
                        // The store holds everything the record now says (the applier bumped its
                        // serial for this write): without the stamp the next ensure would see a
                        // newer serial and re-upload the shadow over the bytes that just landed.
                        resource->syncedChangeSerial = ResourceSerialOf(res);
                    } else if (resource != nullptr) {
                        const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                        auto& write = resource->pendingResidentWrites.emplace_back();
                        write.offset = offset;
                        const auto* source = static_cast<const Uint8*>(bytes);
                        write.bytes.assign(source, source + size);
                    }
                    if (resource != nullptr) resource->serverGpuWritten = true;
                    return;
                }
                // M-2: UNDER pendingMutex, because this line runs BEFORE the CanTouchGLNow()
                // test below - i.e. on the arm D-A2 deliberately keeps reachable off the render
                // thread - while every reader of hostBytes (Ops_H_Readback's drain, the
                // kill-switch map arm of Ops_H_FlushRange, the ensure path, the fp64 narrowing)
                // is on the render thread. It was the one member of this struct that the
                // off-thread path touched with neither a lock nor an atomic: pendingRanges and
                // pendingResidentWrites are under this mutex and syncedChangeSerial is an
                // std::atomic read with acquire. The lock it is put under is deliberately THAT
                // one and not a new one - the base and the queued range are one fact ("these
                // bytes, at this base"), the drain takes this mutex to lift the ranges, and a
                // drain that sees a range therefore sees the base that range was queued
                // against. The store is kept on both arms rather than restricted to the
                // CanTouchGLNow() one because Ops_H_Readback can drain with no draw in between,
                // and the ensure path's republication (the 3c55e027 fix) is what runs at a draw.
                if (bytes != nullptr) {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    // R-11: MONOLITH keeps the client's base; SPLIT copies [offset, offset+size)
                    // into server-owned storage and points hostBytes at that. Inside the SAME
                    // lock as the queued range, because the base and the range are one fact
                    // ("these bytes, at this base") and the drain takes this mutex to lift them.
                    resource->hostBytes =
                        MGL_SERVER_STAGED_ADOPT(*resource, ResourceWidthOf(res), bytes, offset, size);
                    if (size != 0) resource->contentSinceRespecify = true;
                }
                if (resource->pendingRespecify) return; // full re-upload pending anyway
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration ||
                    !StorageMatchesSize(*resource, ResourceWidthOf(res))) {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.Add({offset, offset + size});
                    return;
                }
                // The adopted zero-copy store already HAS the bytes; a driver upload here would
                // re-synchronize what coherent mapping made free.
                if (resource->persistentMapped && resource->persistentPtr) {
                    resource->syncedChangeSerial = ResourceSerialOf(res);
                    return;
                }
                if (MG_Config::Features.EsprytDisableUploadRing) {
                    UploadRangeFrom(*resource, resource->hostBytes, offset, offset + size);
                    resource->syncedChangeSerial = ResourceSerialOf(res);
                    return;
                }
                // The Mali WAR-stall fix, unchanged: queue and let draw-time sync stage the
                // merged ranges through the upload ring.
                const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                resource->pendingRanges.Add({offset, offset + size});
            }

            void Ops_H_ResidentSubData(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPSubData& record,
                                       const void* bytes) {
                const SizeT offset = static_cast<SizeT>(MG_Pipe::MGPipeSubDataBufferOffset(record));
                const SizeT size = static_cast<SizeT>(MG_Pipe::MGPipeSubDataBufferSize(record));
                auto* resource = FindBufferResourceForHandle(res);
                if (!resource || size == 0 || bytes == nullptr) return;
                // DELIBERATELY not stored as hostBytes: these are the application's staging
                // store and are valid for the duration of the call only (BufferObject.h:84-85),
                // which is exactly why they are COPIED here rather than referenced later.
                const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                auto& write = resource->pendingResidentWrites.emplace_back();
                write.offset = offset;
                const auto* source = static_cast<const Uint8*>(bytes);
                write.bytes.assign(source, source + size);
            }

            void Ops_H_FlushRange(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPFlushRange& record,
                                  const void* bytes) {
                auto* resource = FindBufferResourceForHandle(res);
                if (!resource) return;
                const SizeT start = static_cast<SizeT>(record.Offset);
                const SizeT end = start + static_cast<SizeT>(record.Size);
                // M-2, the second store site - same lock, same reason as Ops_H_SubData's, and
                // the same R-11 copy. UNDER SPLIT `bytes` IS ALWAYS NULL HERE by ruling (C-6 /
                // contract table 1 row 20: resource_flush_range carries no bytes at all - the
                // ladder it drives rewrites its range from the authoritative shadow, which rule
                // C makes server-owned, so resource_subdata is already the only way bytes reach
                // it and a second carrier would be a forgeable way to say the same thing). So
                // this arm keeps whatever the preceding subdata records staged, which is
                // exactly what the ladder must read.
                if (bytes != nullptr) {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->hostBytes = MGL_SERVER_STAGED_ADOPT(*resource, ResourceWidthOf(res), bytes,
                                                                  start, end - start);
                    if (end > start) resource->contentSinceRespecify = true;
                }
                if (resource->pendingRespecify) return;
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration ||
                    !StorageMatchesSize(*resource, ResourceWidthOf(res))) {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.Add({start, end});
                    return;
                }
                if (resource->persistentMapped && resource->persistentPtr) {
                    resource->syncedChangeSerial = ResourceSerialOf(res);
                    return;
                }
                if (!MG_Config::Features.EsprytDisableUploadRing) {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.Add({start, end});
                    return;
                }
                // The kill-switch arm, and it reads the application's REAL flags per call -
                // which is why MGPFlushRange carries them unnormalised.
                const Flags<BufferMappingAccessBit> appAccess{
                    static_cast<std::underlying_type_t<BufferMappingAccessBit>>(record.AccessFlags)};
                const Bool invalidate = (appAccess & BufferMappingAccessBit::InvalidateRange) ||
                                        (appAccess & BufferMappingAccessBit::InvalidateBuffer);
                const Bool unsynchronized = static_cast<Bool>(appAccess & BufferMappingAccessBit::Unsynchronized);
                if (PREFER_MAP_BUFFER_RANGE_FOR_BUFFER_SYNC && (invalidate || unsynchronized) &&
                    resource->hostBytes != nullptr) {
#ifdef TRACY_ENABLE
                    ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                    // The kill-switch arm's own Memcpy, and the one that passes
                    // GL_MAP_INVALIDATE_RANGE_BIT - i.e. the one that tells the driver the old
                    // bytes are dead. Bytes outside the staged coverage are exactly the ones
                    // that claim is false for.
                    MGL_SERVER_STAGED_REQUIRE(*resource, resource->hostBytes, start, end,
                                              "flush_range_invalidate_map");
                    BindBufferId(TempBufferTarget, resource->id);
                    void* mappedData = g_GLESFuncs.glMapBufferRange(
                        TempBufferTarget, (GLintptr)start, (GLsizeiptr)(end - start),
                        GL_MAP_WRITE_BIT | (invalidate ? GL_MAP_INVALIDATE_RANGE_BIT : 0) |
                            (unsynchronized ? GL_MAP_UNSYNCHRONIZED_BIT : 0));
                    if (mappedData) {
                        Memcpy(mappedData, resource->hostBytes + start, end - start);
                        g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
                        resource->syncedChangeSerial = ResourceSerialOf(res);
                        return;
                    }
                    MGLOG_E_ONCE("Failed to map buffer with ID: %u for flush, falling back to glBufferSubData",
                                 resource->id);
                }
                UploadRangeFrom(*resource, resource->hostBytes, start, end);
                resource->syncedChangeSerial = ResourceSerialOf(res);
            }

            void Ops_H_Readback(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPReadback& record) {
                // An empty capture may conservatively request a writeback before
                // any draw materialized this buffer. Resolve its staged resource
                // exactly as a GPU consumer would, then return the real bytes.
                auto* resource = EnsureBufferResourceForHandle(nullptr, res);
                if (!resource || resource->id == 0 || !resource->storageInitialized) return;
                if (!CanTouchGLNow() || resource->contextGeneration != g_bufferContextGeneration) return;
                if (resource->persistentMapped) {
                    // Queued resident SubData bytes land first (GPU-ordered), then the finish
                    // makes them - and any shader writes already queued on this context -
                    // visible through the coherent mapping the reads use. There is no backend
                    // copy to read back in this case.
                    DrainResidentWritesNow(*resource);
                    if (g_GLESFuncs.glFinish) g_GLESFuncs.glFinish();
                    return;
                }
                if (!g_GLESFuncs.glMapBufferRange || !g_GLESFuncs.glUnmapBuffer) return;
                // The map below starts at record.Offset where the legacy arm mapped at literal
                // zero, so the clamp has to be against what is left of the BACKEND store past
                // that offset: the applier's BufferRangeFault checked the range against
                // Desc.Width, and a store that is smaller than the descriptor (a respecify the
                // backend has not applied yet) would otherwise be mapped past its end.
                const SizeT readOffset = static_cast<SizeT>(record.Offset);
                if (readOffset >= resource->storageSize) return;
                const SizeT size =
                    std::min<SizeT>(static_cast<SizeT>(record.Size), resource->storageSize - readOffset);
                if (size == 0) return;

                // Queued app writes must land in the backend store before it is read back, or
                // the writeback below would revert them in the shadow. Under split every queued
                // range must lie inside the server shadow's staged coverage first (R-11 / the
                // M-6 ruling): the drain's tier-1 map would otherwise declare live GPU bytes
                // dead over a range nothing staged.
                MGL_SERVER_STAGED_REQUIRE_PENDING(*resource, resource->hostBytes, ResourceWidthOf(res),
                                                  "readback_flush_pending");
                FlushPendingRangesFrom(*resource, resource->hostBytes, ResourceWidthOf(res));

                BindBufferId(TempBufferTarget, resource->id);
                void* mapped = g_GLESFuncs.glMapBufferRange(TempBufferTarget, (GLintptr)record.Offset,
                                                            static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
                if (mapped == nullptr) {
                    MGLOG_E_ONCE("Ops_H_Readback: glMapBufferRange(read) failed for buffer %u", resource->id);
                    return;
                }
                // The reverse channel replaces BufferObject::WritebackFromBackend: the backend
                // no longer reaches into the frontend's address space, it ANSWERS. In monolith
                // the client's implementation is one call away, so SyncGpuWrites' caller still
                // sees the reconciled shadow on return, exactly as before.
                //
                // THE ORDER FROM HERE IS A CORRECTNESS RULE, NOT A PREFERENCE
                // (ARCHITECTURE.md 8.2: "写回的 epoch bump 必须在任何后续读该 handle 的命令之前被
                // server 应用；反向通道需要与正向通道相同的有序保证"):
                //   1. writeback   - the client's shadow takes the bytes,
                //   2. unmap       - the read mapping goes away,
                //   3. serial stamp- syncedChangeSerial catches up with the record, so the next
                //                    draw does not re-upload the readback over itself,
                //   4. epoch bump  - in Ops_H_ReadbackTracked, i.e. strictly AFTER 1-3, so a
                //                    draw-clean memo re-probed by the bump can never observe a
                //                    half-reconciled resource. The bump must NEVER move earlier.
                if (MG_Pipe::gMGPipeCallbacks.OnBufferWriteback != nullptr) {
                    MG_Pipe::gMGPipeCallbacks.OnBufferWriteback(
                        res, record.Offset,
                        MG_Pipe::MGPBlobRef{reinterpret_cast<Uint64>(mapped), static_cast<Uint64>(size),
                                            MG_Pipe::kMGHostSpanSegNone, 0});
                } else {
                    MGLOG_E_ONCE("Ops_H_Readback: no reverse channel is installed, so the GPU-written bytes of "
                                 "buffer %u cannot reach the client shadow",
                                 resource->id);
                }
                g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
                // The shadow now matches the backend byte for byte; without this the next draw
                // would see a newer serial and re-upload the readback over it.
                resource->syncedChangeSerial = ResourceSerialOf(res);
                // P13 W4a: on monolith's record arm the base this twin reads IS the shadow the
                // writeback just filled, so a whole-store readback is also the refresh
                // SplitHostBytesForCpuRead would owe. (A copying store's copy is not the
                // client's shadow, and keeps its mark.)
                if (MG_Config::RecordArmAliasesFrontend() && readOffset == 0 && size >= ResourceWidthOf(res)) {
                    resource->serverGpuWritten = false;
                }
            }

            void Ops_H_Destroy(MG_Pipe::MGPipeHandle res) {
                // R-11's server copy dies with the resource, and BEFORE the twin leaves the
                // table - the side map is keyed by the twin's address, so this is the last
                // moment that address can be looked up.
                if (GLESBufferResource* dying = FindBufferResourceForHandle(res); dying != nullptr) {
                    MGL_SERVER_STAGED_DROP(*dying);
                }
                // The twin comes OUT of the table first, so the three outcomes below are
                // reached with the entry already retired. The SLOT is the client's to free,
                // after this returns (D-L).
                SharedPtr<BackendBufferResource> twin = g_backendBufferResources.ReleaseByHandle(res);
                // Verbatim Ops_OnDestroy: stale generation -> zero the id; on-thread ->
                // IsPoolable/EnrollIntoPool else scrub + glDeleteBuffers; off-thread ->
                // g_deferredBufferReleases plus the lock-free flag.
                Ops_OnDestroy(std::move(twin));
            }

            // D-E: the ONLY two changes are the signature and the three reads that went
            // through the frontend object (GetSize() -> size, MappedData() -> seedBytes,
            // SetBackendResource(...) -> the slot table's GetOrCreate). Every other statement
            // is byte-identical to Ops_AcquirePersistentMap above - the four-way capability
            // gate, the stale-generation wipe BEFORE the new stamp, the idempotency hit, the
            // fresh-id sequence with ++g_bufferBackendIdGeneration, immutableStorage set as
            // soon as the store exists, the MGLOG_E_ONCE decline and the success stamps.
            void* Ops_H_MapPersistent(MG_Pipe::MGPipeHandle res, Uint64 size, const void* seedBytes) {
                if (!CanTouchGLNow() || !g_GLESFuncs.glBufferStorageEXT || !g_GLESFuncs.glMapBufferRange ||
                    !g_GLESFuncs.glGenBuffers) {
                    return nullptr;
                }
                if (size == 0) return nullptr;

                auto* resource = GetOrCreateBufferResourceForHandle(res);
                if (!resource) return nullptr;
                // Before the generation is stamped, not after: everything on the resource
                // describes a context that is gone, and the idempotency check below would
                // otherwise hand the caller the dead context's mapped pointer.
                if (resource->contextGeneration != g_bufferContextGeneration) {
                    resource->id = 0;
                    resource->persistentMapped = false;
                    resource->persistentPtr = nullptr;
                    resource->immutableStorage = false;
                    resource->storageInitialized = false;
                    resource->storageSize = 0;
                }
                resource->contextGeneration = g_bufferContextGeneration;

                if (resource->persistentMapped && resource->persistentPtr && resource->storageSize == size) {
                    return resource->persistentPtr; // idempotent
                }

                // Need a fresh id: glBufferStorage fails on a buffer that already has
                // immutable storage, and any prior mutable store is replaced anyway.
                if (resource->id != 0) {
                    NoteBufferIdDeleted(resource->id);
                    // Driver VAOs may have this id baked into attribute/element bindings
                    // keyed on versions this re-mint does not move.
                    ++g_bufferBackendIdGeneration;
                    g_GLESFuncs.glDeleteBuffers(1, &resource->id);
                    resource->id = 0;
                    resource->immutableStorage = false;
                }
                g_GLESFuncs.glGenBuffers(1, &resource->id);
                if (resource->id == 0) return nullptr;

                // Seed from the shadow (the client's bytes are still live at this point: it
                // adopts and drops them only after this returns).
                BindBufferId(TempBufferTarget, resource->id);
                const void* initial = seedBytes;
                g_GLESFuncs.glBufferStorageEXT(TempBufferTarget, static_cast<GLsizeiptr>(size), initial,
                                               GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit |
                                                   kDynamicStorageBit);
                // Set as soon as the store exists, not once the map succeeds: the failure
                // path below leaves this id holding immutable storage, and whoever touches
                // it next has to know that glBufferData cannot redefine it.
                resource->immutableStorage = true;
                void* ptr = g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(size),
                                                         GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                if (!ptr) {
                    MGLOG_E_ONCE("Ops_H_MapPersistent: glMapBufferRange(persistent) failed for buffer %u",
                                 resource->id);
                    resource->persistentMapped = false;
                    resource->persistentPtr = nullptr;
                    return nullptr;
                }
                resource->persistentPtr = ptr;
                resource->persistentMapped = true;
                // The ONE statement here that is not in Ops_AcquirePersistentMap, and it is
                // about a member the legacy arm does not have rather than about the map: the
                // client adopts this pointer the instant we return it and
                // PipeResource::AdoptPersistentMap then does m_shadow->clear() +
                // shrink_to_fit(), so the shadow base any earlier content-carrying call
                // recorded is a FREED allocation from here on. The live bytes are persistentPtr.
                // R-11's server copy goes with it: the bytes are the coherent map's now, and a
                // stale server shadow would answer a later drain with pre-map content.
                MGL_SERVER_STAGED_DROP(*resource);
                resource->hostBytes = nullptr;
                resource->storageSize = static_cast<SizeT>(size);
                resource->storageInitialized = true;
                resource->pendingRespecify = false;
                {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                }
                resource->syncedChangeSerial = ResourceSerialOf(res);
                return ptr;
            }

            void Ops_H_UnmapPersistent(MG_Pipe::MGPipeHandle res) {
                (void)res;
                // P3a emits this from nowhere: the donation is permanent for the life of the
                // store and is ended by the respecify / destroy paths, which already retire
                // the immutable id. The entry exists so the transport has both halves of the
                // pair (D-E), and giving it a body that unmapped a live coherent store would
                // be the one thing D-B4 forbids.
            }

            // The epoch-tracking wrappers, duplicated for this table - same contract as the
            // seven above, including AcquirePersistentMap's bump EVEN ON DECLINE (the client
            // still enters a persistent map the per-draw probes must start seeing).
            void Ops_H_RespecifyTracked(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc,
                                        const void* initialBytes) {
                Ops_H_Respecify(res, desc, initialBytes);
                BumpBufferMutationEpoch();
            }
            void Ops_H_SubDataTracked(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPSubData& record,
                                      const void* bytes) {
                Ops_H_SubData(res, record, bytes);
                BumpBufferMutationEpoch();
            }
            void Ops_H_ResidentSubDataTracked(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPSubData& record,
                                              const void* bytes) {
                Ops_H_ResidentSubData(res, record, bytes);
                BumpBufferMutationEpoch();
            }
            void Ops_H_FlushRangeTracked(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPFlushRange& record,
                                         const void* bytes) {
                Ops_H_FlushRange(res, record, bytes);
                BumpBufferMutationEpoch();
            }
            void Ops_H_ReadbackTracked(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPReadback& record) {
                Ops_H_Readback(res, record);
                BumpBufferMutationEpoch();
            }
            void Ops_H_DestroyTracked(MG_Pipe::MGPipeHandle res) {
                Ops_H_Destroy(res);
                BumpBufferMutationEpoch();
            }
            void* Ops_H_MapPersistentTracked(MG_Pipe::MGPipeHandle res, Uint64 size, const void* seedBytes) {
                void* result = Ops_H_MapPersistent(res, size, seedBytes);
                // Bump even on decline: the client still enters a persistent map the per-draw
                // probes must start seeing.
                BumpBufferMutationEpoch();
                return result;
            }

#if MOBILEGL_BUILD_DISAGGREGATED && defined(__ANDROID__)
            // ---- P11 B2: T0 - THE CLIENT'S AHardwareBuffer AS A STORE'S RESIDENT TWIN ------------
            //
            // The split twin of Ops_H_MapPersistent: where a monolith map_persistent mints a
            // coherent persistent store (glBufferStorageEXT) and hands its pointer to the frontend,
            // a T0 map_persistent IMPORTS the pages the client already holds and maps -
            // glBufferStorageExternalEXT on eglGetNativeClientBufferANDROID(ahb) - and the result
            // is the SAME resource shape (immutable, persistentMapped, persistentPtr = this
            // server's coherent map of those pages), so every adopted-store path in this file
            // applies unchanged: draws bind it with nothing to upload, resident SubData lands
            // GPU-ordered (DrainResidentWritesNow), a readback drains and waits and posts no
            // bytes. The R-11 staged copy is dropped: the pages are the client's.
            //
            // THE ENTRY POINTS ARE OPTIONAL: neither is in the loader's tables (an EGL function
            // missing there is MGLOG_F), and eglGetProcAddress hands back live-looking stubs, so
            // the extension strings decide and the pointers are only then taken.
            using T0GetNativeClientBufferFn = EGLClientBuffer (*)(const struct AHardwareBuffer*);
            using T0BufferStorageExternalFn = void (*)(GLenum, GLintptr, GLsizeiptr, EGLClientBuffer, GLbitfield);
            T0GetNativeClientBufferFn g_t0GetNativeClientBuffer = nullptr;
            T0BufferStorageExternalFn g_t0BufferStorageExternal = nullptr;
            constexpr GLbitfield kT0MapReadBit = 0x0001;
            constexpr GLbitfield kT0MapWriteBit = 0x0002;
            constexpr GLbitfield kT0StorageFlags =
                kT0MapReadBit | kT0MapWriteBit | kMapPersistentBit | kMapCoherentBit | kDynamicStorageBit;
            constexpr GLbitfield kT0MapFlags = kT0MapReadBit | kT0MapWriteBit | kMapPersistentBit | kMapCoherentBit;

            Bool ResolveT0EntryPoints(String& why) {
                if (g_t0GetNativeClientBuffer != nullptr && g_t0BufferStorageExternal != nullptr) return true;
                if (!g_EGLFuncs.eglGetProcAddress || !g_EGLFuncs.eglQueryString || !g_EGLFuncs.eglGetCurrentDisplay ||
                    !g_GLESFuncs.glGetString || !g_GLESFuncs.glMapBufferRange || !g_GLESFuncs.glGenBuffers) {
                    why = "the loader has no eglGetProcAddress/eglQueryString/glGetString/glMapBufferRange";
                    return false;
                }
                const char* egl = g_EGLFuncs.eglQueryString(g_EGLFuncs.eglGetCurrentDisplay(), EGL_EXTENSIONS);
                const char* gl = reinterpret_cast<const char*>(g_GLESFuncs.glGetString(GL_EXTENSIONS));
                if (!HasExtensionWord(egl, "EGL_ANDROID_get_native_client_buffer")) {
                    why = "EGL_ANDROID_get_native_client_buffer is not advertised";
                    return false;
                }
                if (!HasExtensionWord(gl, "GL_EXT_external_buffer") || !HasExtensionWord(gl, "GL_EXT_buffer_storage")) {
                    why = "GL_EXT_external_buffer / GL_EXT_buffer_storage is not advertised";
                    return false;
                }
                g_t0GetNativeClientBuffer = reinterpret_cast<T0GetNativeClientBufferFn>(
                    g_EGLFuncs.eglGetProcAddress("eglGetNativeClientBufferANDROID"));
                g_t0BufferStorageExternal = reinterpret_cast<T0BufferStorageExternalFn>(
                    g_EGLFuncs.eglGetProcAddress("glBufferStorageExternalEXT"));
                if (g_t0GetNativeClientBuffer == nullptr || g_t0BufferStorageExternal == nullptr) {
                    why = "eglGetProcAddress gave no eglGetNativeClientBufferANDROID / glBufferStorageExternalEXT";
                    g_t0GetNativeClientBuffer = nullptr;
                    g_t0BufferStorageExternal = nullptr;
                    return false;
                }
                return true;
            }

            void DrainGLErrors() {
                if (!g_GLESFuncs.glGetError) return;
                for (Uint guard = 0; guard < 16 && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++guard) {
                }
            }

            // A fresh GL buffer whose storage IS `ahb`, mapped persistent + coherent. 0 on failure
            // (with `why`); nothing on the resource is touched here. `throwaway` = the POST's own
            // context is current: bind raw and keep Espryt's binding caches (which describe the
            // session's context, whose name space this is not) out of it.
            Uint ImportAhbAsBuffer(void* ahb, Uint64 size, void** outPtr, String& why, Bool throwaway = false) {
                *outPtr = nullptr;
                const EGLClientBuffer clientBuffer =
                    g_t0GetNativeClientBuffer(static_cast<const struct AHardwareBuffer*>(ahb));
                if (clientBuffer == nullptr) {
                    why = "eglGetNativeClientBufferANDROID returned null";
                    return 0;
                }
                Uint id = 0;
                g_GLESFuncs.glGenBuffers(1, &id);
                if (id == 0) {
                    why = "glGenBuffers gave no name";
                    return 0;
                }
                DrainGLErrors();
                if (throwaway) g_GLESFuncs.glBindBuffer(TempBufferTarget, id);
                else BindBufferId(TempBufferTarget, id);
                g_t0BufferStorageExternal(TempBufferTarget, 0, static_cast<GLsizeiptr>(size), clientBuffer,
                                          kT0StorageFlags);
                const GLenum stored = g_GLESFuncs.glGetError ? g_GLESFuncs.glGetError() : GL_NO_ERROR;
                void* ptr = stored == GL_NO_ERROR
                    ? g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(size), kT0MapFlags)
                    : nullptr;
                if (ptr == nullptr) {
                    char text[160];
                    std::snprintf(text, sizeof(text), "glBufferStorageExternalEXT -> 0x%04x, map %s", stored,
                                  stored == GL_NO_ERROR ? "failed" : "not attempted");
                    why = text;
                    if (!throwaway) {
                        NoteBufferIdDeleted(id);
                        ++g_bufferBackendIdGeneration;
                    }
                    g_GLESFuncs.glDeleteBuffers(1, &id);
                    return 0;
                }
                *outPtr = ptr;
                return id;
            }

            Bool T0Answer(char* out, Uint64 outBytes, Bool ok, const char* fmt, ...) {
                va_list args;
                va_start(args, fmt);
                std::vsnprintf(out, static_cast<SizeT>(outBytes), fmt, args);
                va_end(args);
                return ok;
            }

            // Installs an imported store on the twin. The caller has retired whatever was there.
            void InstallT0Store(GLESBufferResource& resource, Uint id, void* ptr, Uint64 size) {
                resource.id = id;
                resource.immutableStorage = true;
                resource.persistentPtr = ptr;
                resource.persistentMapped = true;
                resource.storageSize = static_cast<SizeT>(size);
                resource.storageInitialized = true;
                resource.pendingRespecify = false;
                resource.contextGeneration = g_bufferContextGeneration;
                const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                resource.pendingRanges.clear();
                resource.pendingResidentWrites.clear();
            }

            Bool Ops_H_ImportExternal(MG_Pipe::MGPipeHandle res, void* ahb, Uint64 size) {
                SweepT0Retired(false);
                if (!CanTouchGLNow() || size == 0 || ahb == nullptr) return false;
                String why;
                if (!ResolveT0EntryPoints(why)) {
                    MGLOG_W_ONCE("Espryt T0: no import on this context - %s", why.c_str());
                    return false;
                }
                auto* resource = GetOrCreateBufferResourceForHandle(res);
                if (resource == nullptr) return false;
                // IMPORT FIRST, retire second: a refused import leaves the T2 store (and its staged
                // copy) exactly as it was, so the DECLINED the client gets leaves nothing to repair.
                void* ptr = nullptr;
                const Uint id = ImportAhbAsBuffer(ahb, size, &ptr, why);
                if (id == 0) {
                    MGLOG_W("Espryt T0: store {slot=%u, gen=%u} (%llu bytes) not imported - %s", res.Slot, res.Gen,
                            static_cast<unsigned long long>(size), why.c_str());
                    return false;
                }
                if (resource->contextGeneration == g_bufferContextGeneration && resource->id != 0) {
                    NoteBufferIdDeleted(resource->id);
                    // Driver VAOs may have the old id baked into bindings keyed on versions this
                    // replacement does not move (the Ops_H_MapPersistent re-mint's rule).
                    ++g_bufferBackendIdGeneration;
                    g_GLESFuncs.glDeleteBuffers(1, &resource->id);
                }
                resource->id = 0;
                RetireT0Import(*resource); // a previous import of this handle, fenced
                InstallT0Store(*resource, id, ptr, size);
                MG_Remote::Transport::AdoptT0::AcquireImported(ahb);
                resource->externalAhb = ahb;
                ++g_t0ImportsLive;
                // The bytes are the client's pages now; R-11's copy would answer a later drain
                // with pre-import content.
                MGL_SERVER_STAGED_DROP(*resource);
                resource->hostBytes = nullptr;
                resource->syncedChangeSerial = ResourceSerialOf(res);
                MGLOG_D("Espryt T0: imported {slot=%u, gen=%u} as buffer %u (%llu bytes); %llu live imports",
                        res.Slot, res.Gen, id, static_cast<unsigned long long>(size),
                        static_cast<unsigned long long>(g_t0ImportsLive));
                return true;
            }

            Bool Ops_H_ImportExternalTracked(MG_Pipe::MGPipeHandle res, void* ahb, Uint64 size) {
                const Bool imported = Ops_H_ImportExternal(res, ahb, size);
                BumpBufferMutationEpoch();
                return imported;
            }

            // The POST's second round: the CPU writes region 1 through the held lock only now, after
            // the import and a GPU use of it, and reads region 4 (the first round's fill) back into
            // its cache; a second dispatch copies region 1 out and overwrites region 4. Appends its
            // counts to `detail`.
            Bool SelfTestExternalSteady(GLuint imported, GLuint out, void* heldPtr, char* detail, SizeT detailBytes) {
                constexpr Uint32 kWords = 1024;
                constexpr Uint32 kSteadyAtWord = kWords;   // region 1
                constexpr Uint32 kFillAtWord = 4 * kWords; // region 4
                auto* held = static_cast<volatile Uint32*>(heldPtr);
                Uint32 cached = 0;
                for (Uint32 i = 0; i < kWords; ++i) {
                    held[kSteadyAtWord + i] = 0xA5000000u ^ (i * 40503u);
                    cached ^= held[kFillAtWord + i];
                }
                (void)cached;
                static const char* kSource =
                    "#version 310 es\n"
                    "layout(local_size_x = 64) in;\n"
                    "layout(std430, binding = 0) buffer Ext { uint w[]; } ext;\n"
                    "layout(std430, binding = 1) buffer Out { uint w[]; } outb;\n"
                    "void main() {\n"
                    "  uint i = gl_GlobalInvocationID.x;\n"
                    "  outb.w[i] = ext.w[1024u + i];\n"
                    "  ext.w[4096u + i] = 0x3D5EA11Eu ^ i;\n"
                    "}\n";
                const GLuint program = g_GLESFuncs.glCreateShaderProgramv(GL_COMPUTE_SHADER, 1, &kSource);
                GLint linked = 0;
                if (program != 0) g_GLESFuncs.glGetProgramiv(program, GL_LINK_STATUS, &linked);
                const SizeT used = std::strlen(detail);
                if (!linked) {
                    if (program != 0) g_GLESFuncs.glDeleteProgram(program);
                    std::snprintf(detail + used, detailBytes - used, "; steady: the second program did not link");
                    return false;
                }
                g_GLESFuncs.glUseProgram(program);
                g_GLESFuncs.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, imported);
                g_GLESFuncs.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, out);
                g_GLESFuncs.glMemoryBarrier(0x00004000 /*GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT*/);
                g_GLESFuncs.glDispatchCompute(kWords / 64, 1, 1);
                g_GLESFuncs.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                                            0x00004000 /*GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT*/);
                g_GLESFuncs.glFinish();
                Uint32 steadyRead = 0, steadySaw = 0;
                for (Uint32 i = 0; i < kWords; ++i)
                    if (held[kFillAtWord + i] == (0x3D5EA11Eu ^ i)) ++steadySaw;
                g_GLESFuncs.glBindBuffer(GL_SHADER_STORAGE_BUFFER, out);
                const auto* copied = static_cast<const Uint32*>(
                    g_GLESFuncs.glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, kWords * 4, GL_MAP_READ_BIT));
                if (copied != nullptr) {
                    for (Uint32 i = 0; i < kWords; ++i)
                        if (copied[i] == (0xA5000000u ^ (i * 40503u))) ++steadyRead;
                    g_GLESFuncs.glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                }
                g_GLESFuncs.glDeleteProgram(program);
                std::snprintf(detail + used, detailBytes - used,
                              "; with the import in use, GPU read %u/%u words the CPU wrote after it, GPU write "
                              "seen through the held lock %u/%u words",
                              steadyRead, kWords, steadySaw, kWords);
                return steadyRead == kWords && steadySaw == kWords;
            }

            // THE POST (B2, the standing rule: decide by probe, never by driver name). The AHB API
            // promises CPU coherence only across an unlock/lock; T0 keeps the client's lock for the
            // store's life, so the pattern it depends on is exactly what this exercises, on this
            // context's own driver, before kCapAdoptT0 may be published:
            //   hold  - a 64 KiB BLOB allocated and locked by this test, never unlocked while it runs;
            //   read  - this server's own host map of the import reads the pattern the CPU wrote
            //           through the held pointer (every hostBytes fallback reads persistentPtr);
            //   gpu   - a compute dispatch copies that pattern out of the import (the GPU read) and
            //           fills another region of it (the GPU write);
            //   back  - after glFinish the fill is read through the HELD pointer and the copy is
            //           read back from the dispatch's output;
            //   steady - the same again with bytes the CPU wrote AFTER the import and its first GPU
            //           use (VkBufferManager::SelfTestWireImport says why the first round is not enough).
            // In a throwaway ES 3.1 pbuffer context, so none of this context's state or Espryt's
            // binding caches is disturbed; the caller's bind is restored before returning.
            Bool Ops_H_SelfTestExternal(char* whyOut, Uint64 whyBytes) {
                const auto answer = [&](Bool ok, const char* text, const char* arg = "") {
                    return T0Answer(whyOut, whyBytes, ok, text, arg);
                };
                String why;
                if (!ResolveT0EntryPoints(why)) return answer(false, "%s", why.c_str());
                if (!g_EGLFuncs.eglChooseConfig || !g_EGLFuncs.eglCreateContext || !g_EGLFuncs.eglMakeCurrent ||
                    !g_EGLFuncs.eglCreatePbufferSurface || !g_GLESFuncs.glCreateShaderProgramv ||
                    !g_GLESFuncs.glDispatchCompute || !g_GLESFuncs.glMemoryBarrier || !g_GLESFuncs.glFinish)
                    return answer(false, "the loader lacks the EGL/GLES 3.1 entry points the test needs");
                const EGLDisplay display = g_EGLFuncs.eglGetCurrentDisplay();
                const EGLContext savedContext = g_EGLFuncs.eglGetCurrentContext();
                const EGLSurface savedDraw = g_EGLFuncs.eglGetCurrentSurface(EGL_DRAW);
                const EGLSurface savedRead = g_EGLFuncs.eglGetCurrentSurface(EGL_READ);
                const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, 0x0040 /*EGL_OPENGL_ES3_BIT*/, EGL_SURFACE_TYPE,
                                                EGL_PBUFFER_BIT, EGL_NONE};
                EGLConfig config = nullptr;
                EGLint configs = 0;
                if (!g_EGLFuncs.eglChooseConfig(display, configAttribs, &config, 1, &configs) || configs < 1)
                    return answer(false, "no ES3 pbuffer config for the throwaway context");
                const EGLint surfaceAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
                const EGLSurface surface = g_EGLFuncs.eglCreatePbufferSurface(display, config, surfaceAttribs);
                const EGLint contextAttribs[] = {0x3098 /*EGL_CONTEXT_MAJOR_VERSION*/, 3, 0x30FB /*MINOR*/, 1, EGL_NONE};
                const EGLContext context = surface != EGL_NO_SURFACE
                    ? g_EGLFuncs.eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs)
                    : EGL_NO_CONTEXT;
                const auto restore = [&] {
                    g_EGLFuncs.eglMakeCurrent(display, savedDraw, savedRead, savedContext);
                    if (context != EGL_NO_CONTEXT) g_EGLFuncs.eglDestroyContext(display, context);
                    if (surface != EGL_NO_SURFACE) g_EGLFuncs.eglDestroySurface(display, surface);
                };
                if (context == EGL_NO_CONTEXT || !g_EGLFuncs.eglMakeCurrent(display, surface, surface, context)) {
                    restore();
                    return answer(false, "the throwaway ES 3.1 context could not be made current");
                }

                constexpr Uint64 kBytes = 64 * 1024;
                constexpr Uint32 kWords = 1024;          // one 4 KiB region
                constexpr Uint32 kFillAtWord = 4 * kWords; // region 4
                MG_Remote::Transport::AdoptT0::HeldStore held;
                Vector<Uint32> pattern(kBytes / 4);
                for (SizeT i = 0; i < pattern.size(); ++i) pattern[i] = 0x5A000000u ^ static_cast<Uint32>(i * 2654435761u);
                if (!MG_Remote::Transport::AdoptT0::AllocateHeld(kBytes, pattern.data(), held, why)) {
                    restore();
                    return answer(false, "hold: %s", why.c_str());
                }
                Bool ok = false;
                GLuint out = 0, program = 0;
                void* hostMap = nullptr;
                const GLuint imported = ImportAhbAsBuffer(held.ahb, kBytes, &hostMap, why, /*throwaway=*/true);
                char detail[512] = {};
                if (imported == 0) {
                    std::snprintf(detail, sizeof(detail), "import: %s", why.c_str());
                } else if (std::memcmp(hostMap, pattern.data(), kWords * 4) != 0) {
                    std::snprintf(detail, sizeof(detail), "read: the server's host map of the import does not "
                                                          "hold the CPU's pattern");
                } else {
                    static const char* kSource =
                        "#version 310 es\n"
                        "layout(local_size_x = 64) in;\n"
                        "layout(std430, binding = 0) buffer Ext { uint w[]; } ext;\n"
                        "layout(std430, binding = 1) buffer Out { uint w[]; } outb;\n"
                        "void main() {\n"
                        "  uint i = gl_GlobalInvocationID.x;\n"
                        "  outb.w[i] = ext.w[i];\n"
                        "  ext.w[4096u + i] = 0x6C0FFEE0u ^ i;\n"
                        "}\n";
                    program = g_GLESFuncs.glCreateShaderProgramv(GL_COMPUTE_SHADER, 1, &kSource);
                    GLint linked = 0;
                    if (program != 0) g_GLESFuncs.glGetProgramiv(program, GL_LINK_STATUS, &linked);
                    g_GLESFuncs.glGenBuffers(1, &out);
                    g_GLESFuncs.glBindBuffer(GL_SHADER_STORAGE_BUFFER, out);
                    g_GLESFuncs.glBufferData(GL_SHADER_STORAGE_BUFFER, kWords * 4, nullptr, GL_DYNAMIC_READ);
                    if (!linked) {
                        std::snprintf(detail, sizeof(detail), "gpu: the test's compute program did not link");
                    } else {
                        g_GLESFuncs.glUseProgram(program);
                        g_GLESFuncs.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, imported);
                        g_GLESFuncs.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, out);
                        g_GLESFuncs.glDispatchCompute(kWords / 64, 1, 1);
                        g_GLESFuncs.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                                                    0x00004000 /*GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT*/);
                        g_GLESFuncs.glFinish();
                        Uint32 gpuRead = 0, heldSaw = 0;
                        const auto* heldWords = static_cast<const Uint32*>(held.ptr);
                        for (Uint32 i = 0; i < kWords; ++i)
                            if (heldWords[kFillAtWord + i] == (0x6C0FFEE0u ^ i)) ++heldSaw;
                        g_GLESFuncs.glBindBuffer(GL_SHADER_STORAGE_BUFFER, out);
                        const auto* copied = static_cast<const Uint32*>(g_GLESFuncs.glMapBufferRange(
                            GL_SHADER_STORAGE_BUFFER, 0, kWords * 4, GL_MAP_READ_BIT));
                        if (copied != nullptr) {
                            for (Uint32 i = 0; i < kWords; ++i)
                                if (copied[i] == pattern[i]) ++gpuRead;
                            g_GLESFuncs.glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                        }
                        ok = gpuRead == kWords && heldSaw == kWords;
                        std::snprintf(detail, sizeof(detail),
                                      "64 KiB AHB, lock held throughout: host map read ok, GPU read %u/%u words of "
                                      "the CPU's pattern, GPU write seen through the held lock %u/%u words",
                                      gpuRead, kWords, heldSaw, kWords);
                        if (ok) ok = SelfTestExternalSteady(imported, out, held.ptr, detail, sizeof(detail));
                    }
                }
                if (program != 0) g_GLESFuncs.glDeleteProgram(program);
                if (out != 0) g_GLESFuncs.glDeleteBuffers(1, &out);
                if (imported != 0) g_GLESFuncs.glDeleteBuffers(1, &imported);
                g_GLESFuncs.glFinish();
                restore();
                MG_Remote::Transport::AdoptT0::ReleaseHeld(held);
                return answer(ok, "%s", detail);
            }
#endif // MOBILEGL_BUILD_DISAGGREGATED && __ANDROID__
        } // namespace

            // ---- P5c (tx): the TEXTURE half of the resource family, staged server-side --------
            //
            // The buffer half's R-11 pattern (ServerStaged() above), for texture levels: the
            // staged bytes of resource_subdata's texture half are adopted into the server's
            // StagedTextureStore AT APPLY TIME, because `bytes` names SEG_STAGE and is dead the
            // moment the record retires (rule C). The store, its ownership and its coverage
            // rules are documented in MG_Remote/Server/StagedTextureStore.h; these are only the
            // three hook bodies. All three are no-ops in monolith (CopiesIntoServerStorage()
            // false), which is what keeps the monolith expression character for character.
            //
            // NOTHING here mints or reads a texture twin: the store is keyed by the wire handle
            // the record carried (StagedTextureStore.h explains why not the twin address), so
            // adoption needs no GL call and no frontend object.

            // GetUploadTargets() answered from the descriptor's Target: one static list per
            // target, matching the frontend classes' own lists member for member
            // (TextureObject*.h / TextureObjectStubs.h: a cube is six faces, a cube array is
            // the single CubeMapArray target, everything else its own one).
            const Vector<TextureUploadTarget>& StagedUploadTargetsForPipeTarget(Uint8 pipeResourceTarget) {
                static const Vector<TextureUploadTarget> kUnknown{};
                static const Vector<TextureUploadTarget> kTex1D{TextureUploadTarget::Texture1D};
                static const Vector<TextureUploadTarget> kTex2D{TextureUploadTarget::Texture2D};
                static const Vector<TextureUploadTarget> kTex3D{TextureUploadTarget::Texture3D};
                static const Vector<TextureUploadTarget> kTex1DArray{TextureUploadTarget::Texture1DArray};
                static const Vector<TextureUploadTarget> kTex2DArray{TextureUploadTarget::Texture2DArray};
                static const Vector<TextureUploadTarget> kTexCube{
                    TextureUploadTarget::CubeMapPositiveX, TextureUploadTarget::CubeMapNegativeX,
                    TextureUploadTarget::CubeMapPositiveY, TextureUploadTarget::CubeMapNegativeY,
                    TextureUploadTarget::CubeMapPositiveZ, TextureUploadTarget::CubeMapNegativeZ};
                static const Vector<TextureUploadTarget> kTexCubeArray{TextureUploadTarget::CubeMapArray};
                static const Vector<TextureUploadTarget> kTex2DMS{TextureUploadTarget::Texture2DMultisample};
                static const Vector<TextureUploadTarget> kTex2DMSArray{TextureUploadTarget::Texture2DMultisampleArray};
                static const Vector<TextureUploadTarget> kTexRect{TextureUploadTarget::TextureRectangle};
                static const Vector<TextureUploadTarget> kTexBuffer{TextureUploadTarget::TextureBuffer};
                switch (static_cast<MG_Pipe::MGPipeResourceTarget>(pipeResourceTarget)) {
                case MG_Pipe::MGPipeResourceTarget::Tex1D: return kTex1D;
                case MG_Pipe::MGPipeResourceTarget::Tex2D: return kTex2D;
                case MG_Pipe::MGPipeResourceTarget::Tex3D: return kTex3D;
                case MG_Pipe::MGPipeResourceTarget::Tex1DArray: return kTex1DArray;
                case MG_Pipe::MGPipeResourceTarget::Tex2DArray: return kTex2DArray;
                case MG_Pipe::MGPipeResourceTarget::TexCube: return kTexCube;
                case MG_Pipe::MGPipeResourceTarget::TexCubeArray: return kTexCubeArray;
                case MG_Pipe::MGPipeResourceTarget::Tex2DMS: return kTex2DMS;
                case MG_Pipe::MGPipeResourceTarget::Tex2DMSArray: return kTex2DMSArray;
                case MG_Pipe::MGPipeResourceTarget::TexRect: return kTexRect;
                case MG_Pipe::MGPipeResourceTarget::TexBuffer: return kTexBuffer;
                default: return kUnknown;
                }
            }

// P5e (ID-124): OUT OF THE DISAGGREGATED BLOCK ON PURPOSE. This is a pure
// MGPipeResourceTarget -> TextureTarget switch with no wire, no record and no role in it, but
// it sat inside the nested `#if MOBILEGL_BUILD_DISAGGREGATED` below while its only caller,
// MGB_TEXPARAM_TARGET, is guarded by `#if MOBILEGL_PIPE_PUSH` alone - so a push build without
// disaggregation did not compile. The narrower fix (guarding the macro block too) is wrong:
// that block also carries TextureDiagName and MGB_TEXTURE_RECORD_ARM_SELECTED, which the push
// flavour does need. Caught by the `flavours` gate step; see ID-124.
            // The inverse of MG_Pipe::MGPipeResourceTargetForTextureTarget, for the sync's
            // target reads (ConvertTextureTargetToBackendGLEnum and MapToBackendTextureTarget
            // both want the frontend enum).
            TextureTarget StagedTextureTargetForPipeTarget(Uint8 pipeResourceTarget) {
                switch (static_cast<MG_Pipe::MGPipeResourceTarget>(pipeResourceTarget)) {
                case MG_Pipe::MGPipeResourceTarget::Tex1D: return TextureTarget::Texture1D;
                case MG_Pipe::MGPipeResourceTarget::Tex2D: return TextureTarget::Texture2D;
                case MG_Pipe::MGPipeResourceTarget::Tex3D: return TextureTarget::Texture3D;
                case MG_Pipe::MGPipeResourceTarget::Tex1DArray: return TextureTarget::Texture1DArray;
                case MG_Pipe::MGPipeResourceTarget::Tex2DArray: return TextureTarget::Texture2DArray;
                case MG_Pipe::MGPipeResourceTarget::TexCube: return TextureTarget::TextureCubeMap;
                case MG_Pipe::MGPipeResourceTarget::TexCubeArray: return TextureTarget::TextureCubeMapArray;
                case MG_Pipe::MGPipeResourceTarget::Tex2DMS: return TextureTarget::Texture2DMultisample;
                case MG_Pipe::MGPipeResourceTarget::Tex2DMSArray: return TextureTarget::Texture2DMultisampleArray;
                case MG_Pipe::MGPipeResourceTarget::TexRect: return TextureTarget::TextureRectangle;
                case MG_Pipe::MGPipeResourceTarget::TexBuffer: return TextureTarget::TextureBuffer;
                default: return TextureTarget::Unknown;
                }
            }

        namespace {
            void Ops_H_TextureSubData(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPSubData& record,
                                      const void* bytes, const MG_Pipe::MGPSubRegion* regions) {
                // The region set is the upload planner's shape and stays in the applier's
                // pending set; the store's coverage is the staged run itself
                // (StagedTextureStore.h's coverage ruling). It is ALSO what places that run in
                // the level image when the record carries a piece of the level rather than the
                // whole of it (fix A2), which is why it is read here now.
                auto& store = MG_Record::ServerStagedTexture();
                // P13 W4b: an aliasing store (monolith's record arm) adopts too - it records where
                // the run sits in the frontend's level shadow instead of copying it.
                if (!store.Holds()) return;
                // The applier's gate has already faulted every shape that reaches here without
                // bytes, and under split the codec declared the run's length (Blob.Size) -
                // TextureEmit.h's "the bytes this record declares ARE the level shadow".
                if (bytes == nullptr || record.Blob.Size == 0) return;
                const auto* stored = PipeTextureRecordForHandle(res);
                if (stored == nullptr) return;
                const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(res);
                const Uint16 uploadTarget = MG_Pipe::MGPipeSubDataUploadTargetOf(record.Target);
                const IntVec3 extent = MG_Record::StagedTextureUploadExtent(stored->Desc, record);
                const SizeT runBytes = static_cast<SizeT>(record.Blob.Size);
                // P8-E (docs/archive/notes/p8/E.md): THE LEVEL HOLDS A COPY THE CLIENT NEVER SAW
                // (FollowCopyImageInStagedStore). The run is the client's shadow, stale everywhere it
                // did not write, so only the texels it names land - its regions, or its union box
                // when it carries none (D-D3's "the box is the whole story"). A run the store cannot
                // place that way is adopted as ever, and the store stops standing for the texture.
                if (store.LevelHoldsFollowedCopy(key, uploadTarget, record.Level) &&
                    (record.RegionCount == 0 || regions != nullptr)) {
                    Vector<MG_Record::StagedTextureStore::ClientTexelBox> boxes;
                    if (record.RegionCount == 0) {
                        const auto& box = record.UnionBox;
                        boxes.push_back({box.X, box.Y, box.Z, box.W, box.H, box.D});
                    } else {
                        for (Uint32 i = 0; i < record.RegionCount; ++i) {
                            const auto& region = regions[i];
                            boxes.push_back({region.X, region.Y, region.Z, region.W, region.H, region.D});
                        }
                    }
                    const Uint64 imageOffset = record.RegionCount == 0
                        ? 0 : MG_Record::StagedTextureRunImageOffset(record, regions);
                    if (store.AdoptClientBoxes(key, uploadTarget, record.Level, extent, imageOffset, bytes, runBytes,
                                               boxes.data(), boxes.size())) {
                        return;
                    }
                    store.MarkDriverWritten(key);
                }
                // RegionCount == 0 IS the whole-level spelling - "the run is the level shadow",
                // beginning at the level's first byte - so it is adopted with the spelling that
                // replaces the level; every other record names a RUN of the level by its own box.
                // A client record's run: under monolith's record arm it IS the frontend level shadow,
                // the one source an aliasing store keeps by address.
                if (record.RegionCount == 0) {
                    store.Adopt(key, uploadTarget, record.Level, extent, bytes, runBytes, /*frontendShadow=*/true);
                    return;
                }
                store.AdoptRun(key, uploadTarget, record.Level, extent,
                               MG_Record::StagedTextureRunImageOffset(record, regions), bytes,
                               runBytes, /*frontendShadow=*/true);
            }

            void Ops_H_TextureRespecify(MG_Pipe::MGPipeHandle res, const MG_Pipe::MGPResourceDesc& desc,
                                        const MG_Pipe::MGPRespecifiedLevel* level) {
                auto& store = MG_Record::ServerStagedTexture();
                if (!store.Holds()) return;
                const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(res);
                if (level != nullptr) {
                    // ONE glTexImage*D redefined one level: its exact extent crossed with the
                    // accepted scope carrier, including a legal noncanonical mutable mip.
                    store.NoteLevelDefined(
                        key, MG_Pipe::MGPipeSubDataUploadTargetOf(level->UploadTarget), level->Level,
                        IntVec3{static_cast<Int>(level->Width), static_cast<Int>(level->Height),
                                static_cast<Int>(level->Depth)},
                        desc.Target, desc.InternalFormat);
                    return;
                }
                // A whole-resource redefinition: every level's old coordinate system is gone.
                // glTexStorage* then defines the WHOLE chain at once (GL 4.6 core 8.19 - all six
                // cube faces included), so an immutable descriptor re-marks every level of every
                // upload target; a mutable whole-resource respecify (a texture view) defines
                // nothing here and the store simply forgets the old levels.
                store.ResetLevels(key);
                if (desc.Immutable == 0 || desc.Levels == 0) return;
                for (const auto& uploadTarget : StagedUploadTargetsForPipeTarget(desc.Target)) {
                    for (Uint32 levelIndex = 0; levelIndex < desc.Levels; ++levelIndex) {
                        store.NoteLevelDefined(
                            key, static_cast<Uint16>(uploadTarget), static_cast<Uint16>(levelIndex),
                            MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height,
                                                                      desc.Depth, levelIndex),
                            desc.Target, desc.InternalFormat);
                    }
                }
            }

            void Ops_H_TextureDestroy(MG_Pipe::MGPipeHandle res) {
                // Deliberately NOT gated on CopiesIntoServerStorage(): Drop's own m_any gate
                // makes the monolith call one acquire load, and an unconditional drop cannot
                // strand a key the latch state was misread for.
                MG_Record::ServerStagedTexture().Drop(
                    MG_Record::StagedTextureStore::KeyForHandle(res));
            }

            const MG_Pipe::MGPipeResourceOps g_glesResourceOps = {
                .Create = Ops_H_Create,
                .Respecify = Ops_H_RespecifyTracked,
                .SubData = Ops_H_SubDataTracked,
                .SubDataResident = Ops_H_ResidentSubDataTracked,
                .FlushRange = Ops_H_FlushRangeTracked,
                .Readback = Ops_H_ReadbackTracked,
                .Destroy = Ops_H_DestroyTracked,
                .MapPersistent = Ops_H_MapPersistentTracked,
                .UnmapPersistent = Ops_H_UnmapPersistent,
                .TextureSubData = Ops_H_TextureSubData,
                .TextureRespecify = Ops_H_TextureRespecify,
                .TextureDestroy = Ops_H_TextureDestroy,
#if MOBILEGL_BUILD_DISAGGREGATED && defined(__ANDROID__)
                // P11 B2 (T0). Android only: nowhere else is there an AHardwareBuffer to import.
                .ImportExternal = Ops_H_ImportExternalTracked,
                .SelfTestExternal = Ops_H_SelfTestExternal,
#endif
            };
        } // namespace

        Uint64 CurrentBufferMutationEpoch() {
            return g_bufferMutationEpoch.load(std::memory_order_acquire);
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        namespace {
            // P5e (vi): how many times BumpBufferMutationEpoch ran off the apply thread while
            // one was running, with a live transport. See the note in the bump itself; the
            // accessor is what a test can assert zero on instead of reading a call graph.
            std::atomic<Uint64> g_bufferMutationEpochBumpsOffTheApplyThread{0};
        } // namespace

        Uint64 BufferMutationEpochBumpsOffTheApplyThread() {
            return g_bufferMutationEpochBumpsOffTheApplyThread.load(std::memory_order_relaxed);
        }
#endif

        void BumpBufferMutationEpoch() {
#if MOBILEGL_BUILD_DISAGGREGATED
            // P5e (vi), CONTRACT-P5E §5.1's first pin (scout S1 R2, BRIEF-P5E's open question 2):
            // "does any GL-THREAD path bump this under a transport". PINNED HERE RATHER THAN
            // ARGUED, because the argument is the kind that is true until someone adds a caller.
            //
            // It matters because this counter is the ONE non-serial input left in the record
            // arm's clean gate: SyncVaoAttributeBuffersByRecord stamps the pre-pass value and
            // skips every IsBufferDrawCleanByHandle probe while the stamp holds. A bump from the
            // GL thread would make that stamp a cross-thread read of a value the applier never
            // observed - the memo would go on reading clean over buffers the client had just
            // dirtied - and under run-ahead the client is a frame ahead, so the window is a
            // frame wide rather than a record wide.
            //
            // THE MEASURED ANSWER, and it is not the clean "no" the scout expected: there is
            // EXACTLY ONE class of non-apply-thread bump under a transport, and it happens
            // BEFORE THERE IS AN APPLY THREAD AT ALL. BackendObject_DirectGLES::Initialize()
            // runs on the app thread by design ("the backend object is BUILT on the app thread
            // while its context is never OWNED by it", ServerLoop.h) and calls
            // RegisterBufferBackendOps(), whose last act is a bump. An unguarded refusal aborts
            // the whole split lane in EGL bring-up, which is how this was found rather than
            // reasoned.
            //
            // That class is harmless and the apply-thread key tells it apart from the one that
            // would not be: with g_applyThreadKey == 0 no record has been applied, so no twin
            // exists, so no memo holds a stamp for the bump to slide under.
            //
            // WHAT IS COUNTED AND WHAT IS FATAL, and why they are not the same thing. Every
            // bump taken off the apply thread WHILE ONE IS RUNNING is counted and named once,
            // and is Fatal UNDER THE STRICT LANE - which is where this phase's pins live, and
            // which is the lane the answer is claimed for. It is not Fatal by default because
            // the unit fixtures that drive the production op tables directly
            // (StagedShadowProductionTest / ServerLoopTest, which register the real tables and
            // call them from the test thread beside a started loop) are the server role without
            // being the apply thread, and taking those down would be this pin policing a
            // fixture rather than the draw path.
            const Uint64 applyThreadKey =
                MG_Remote::Server::Detail::g_applyThreadKey.load(std::memory_order_relaxed);
            if (MG_Config::Transport != MG_Config::TransportMode::Monolith && applyThreadKey != 0 &&
                !MG_Remote::Server::ServerLoop::OnApplyThread()) {
                g_bufferMutationEpochBumpsOffTheApplyThread.fetch_add(1, std::memory_order_relaxed);
                if (MG_Config::Ipc.StrictErrors) {
                    MGLOG_F("MGPipe: Fatal{RoleViolation, \"CurrentBufferMutationEpoch\"} - the "
                            "buffer-mutation epoch was bumped off the apply thread while an apply "
                            "thread was running, with a live transport. The draw-path memos stamp "
                            "this counter and skip their clean probes while the stamp holds "
                            "(CONTRACT-P5E §5.1), so a producer on the client's thread means the "
                            "backend is reachable from a thread that owns none of this state");
                    std::abort();
                }
                MGLOG_E_ONCE("MGPipe: the buffer-mutation epoch was bumped off the apply thread "
                             "with a live transport (CONTRACT-P5E §5.1's pin). Run the strict lane "
                             "to make this fatal at the producer");
            }
#endif
            g_bufferMutationEpoch.fetch_add(1, std::memory_order_release);
        }

        // The seventh slot table. A process-lifetime global, like the other six, so it links
        // itself into its own holder list from its own constructor with no initialisation
        // order question to answer (SlotTables.h).
        BackendBufferResourceTable g_backendBufferResources;

        GLESBufferResource* GetOrCreateBufferResourceForHandle(MG_Pipe::MGPipeHandle res) {
            if (MG_Pipe::MGPipeHandleIsNull(res)) return nullptr;
            // The table refuses both of these itself; this is the release-build VOICE for the
            // refusal, because MOBILEGL_ASSERT compiles out at INFO and a resource that silently
            // stops being twinned is the failure mode the refusal exists to replace.
            if (res.Slot >= BackendBufferResourceTable::kMaxHandleSlot) {
#if MOBILEGL_BUILD_DISAGGREGATED
                MG_Pipe::MGPipeSessionFail( // @Ph-declined (ID-P7-1): PH-2 stays Fatal, CONTRACT-P7 §12
                    MG_Pipe::MGPipeFatalFamily::ProtocolCorruption,
                    "MGPipe: Fatal{ProtocolCorruption, \"BackendSlotTable.HandleSlot\"} - "
                    "buffer twin request named slot %u, past this table's %u bound",
                    res.Slot, BackendBufferResourceTable::kMaxHandleSlot);
#else
                MGLOG_E_ONCE("MGPipe: resource handle slot %u is past the backend table's %u bound - "
                             "refusing to twin it",
                             res.Slot, BackendBufferResourceTable::kMaxHandleSlot);
                return nullptr;
#endif
            }
            const Uint32 liveGen = g_backendBufferResources.LiveGenAt(res.Slot);
            if (liveGen != 0 && liveGen > res.Gen) {
#if MOBILEGL_BUILD_DISAGGREGATED
                MG_Pipe::MGPipeSessionFail( // @Ph-declined (ID-P7-1): PH-2 stays Fatal, CONTRACT-P7 §12
                    MG_Pipe::MGPipeFatalFamily::ProtocolCorruption,
                    "MGPipe: Fatal{ProtocolCorruption, \"BackendSlotTable.Generation\"} - "
                    "buffer twin request named generation %u at slot %u, behind live generation %u",
                    res.Gen, res.Slot, liveGen);
#else
                MGLOG_E_ONCE("MGPipe: resource handle {%u, %u} names a generation BEHIND the live twin's "
                             "%u - refusing rather than dropping the incumbent's driver storage",
                             res.Slot, res.Gen, liveGen);
                return nullptr;
#endif
            }
            auto& twin = g_backendBufferResources.GetOrCreate(res);
            if (!twin) {
                twin = MakeShared<GLESBufferResource>();
                // Same seed the lazy legacy path gives a resource it has just minted: nothing
                // has defined storage yet, so the first sync owes a full (re)specification.
                twin->pendingRespecify = true;
            }
            return twin.get();
        }

        GLESBufferResource* FindBufferResourceForHandle(MG_Pipe::MGPipeHandle res) {
            auto* twin = g_backendBufferResources.FindByHandle(res);
            return twin ? twin->get() : nullptr;
        }

        void RequireStagedCoverage(GLESBufferResource& resource, const Uint8* hostBase, SizeT start,
                                   SizeT end, const char* site) {
            ServerStaged().RequireCoverage(&resource, hostBase, start, end, site);
        }

        // M-3's rule for the WHOLE-STORE readers (v1 round 3). The descriptor is the
        // application's own statement about the store: HasDefinedContent set means it SUPPLIED
        // the content (glBufferData(size, data)), which under split arrives as resource_subdata
        // records behind the respecify (table 1 row 19) - so a coverage gap at the draw is a
        // MISSING RECORD and the zero-fill past the coverage is not the application's bytes.
        // Clear means it ORPHANED the store (glBufferData(size, NULL), glBufferStorage(NULL)):
        // every byte it has not staged since is UNDEFINED by its own declaration, the streaming
        // idiom (orphan, partial glBufferSubData, draw) is the ordinary case, and uploading the
        // shadow's zero-fill for the rest is exactly what the monolith arm uploads from
        // MappedData(). Round 2 refused both shapes and aborted six LargeArenaAdoption /
        // ResourceSubsystemControl entries on the joint by name (v1-v3.md 6).
        //
        // AT NAMESPACE SCOPE, not in the anonymous namespace it was written in (P3b/P4b espryt
        // D1 slice 1): the XFB scatter guards ITS whole-store read with the same question, and
        // a second spelling of "did the application declare these bytes" is how the pool-reuse
        // ladder and the scatter end up disagreeing about one buffer.
        Bool ResourceContentIsDeclared(MG_Pipe::MGPipeHandle res) {
            const auto* record = ResourceRecordOf(res);
            return record != nullptr && record->Desc.HasDefinedContent != 0;
        }

        void StageServerWrittenRange(GLESBufferResource& resource, MG_Pipe::MGPipeHandle res, const void* bytes,
                                     SizeT offset, SizeT size) {
            if (bytes == nullptr || size == 0 || resource.externalAhb != nullptr) return;
            // Only a COPYING store may take bytes that die with a mapping: the monolith one keeps
            // the caller's pointer (StagedShadow.h), which here would dangle after the unmap.
            if (!ServerStaged().CopiesIntoServerStorage()) return;
            // Under pendingMutex, as every other hostBytes store is (M-2).
            const std::lock_guard<std::mutex> lock(resource.pendingMutex);
            resource.hostBytes = MGL_SERVER_STAGED_ADOPT(resource, ResourceWidthOf(res), bytes, offset, size);
        }

        SizeT ResourceWidthForHandle(MG_Pipe::MGPipeHandle res) { return ResourceWidthOf(res); }
        Uint64 ResourceSerialForHandle(MG_Pipe::MGPipeHandle res) { return ResourceSerialOf(res); }
        Uint CurrentBufferContextGeneration() { return g_bufferContextGeneration; }

        // See the declaration: re-mints of a live resource's driver id. Written only on
        // the context thread (all re-mint sites run there), read only by the VAO sync.
        Uint64 g_bufferBackendIdGeneration = 0;

        void RegisterBufferBackendOps() {
            // P3a: the handle-shaped resource table (P13: the only one - the legacy
            // BufferBackendOps table is deleted). Registration is UNCONDITIONAL: the subsystem bit is the FRONTEND's dispatch predicate
            // (MGPipeResourceSubsystemEnabled reads the bit AND this table's presence), so a
            // build with bit 7 clear registers a table nobody calls and the A/B stays a pure
            // configuration question rather than a bring-up-order one.
            MG_Pipe::MGPipeSetResourceOps(&g_glesResourceOps);
            // Frontend writes issued while ops were unregistered advanced change
            // serials with no per-op bump; re-open every draw-clean memo. Advanced directly,
            // past BumpBufferMutationEpoch's apply-thread pin: a registration mutates no buffer
            // and only makes every memo probe again, and with several sessions in one process
            // a new session's backend registers on its own thread while another session's
            // apply thread is already running - which the pin would otherwise read as a
            // client-thread producer and, under the strict lane, abort the server for.
            g_bufferMutationEpoch.fetch_add(1, std::memory_order_release);
        }

        void UnregisterBufferBackendOps() {
            if (MG_Pipe::MGPipeGetResourceOps() == &g_glesResourceOps) {
                MG_Pipe::MGPipeSetResourceOps(nullptr);
            }
            // From here on frontend writes bypass the tracked ops entirely.
            BumpBufferMutationEpoch();
            InvalidateArrayBufferBindingCache();
            // Pooled ids belong to the dying context too; drop them without glDeleteBuffers.
            ClearBufferPool();
            const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
            // The ES context owning these ids is going away; just drop the handles.
            g_deferredBufferReleases.clear();
            g_hasDeferredBufferReleases.store(false, std::memory_order_release);
        }

        void OnServerSessionEndedWithOthersLive() {
            // Only what belongs to the ending session: its staged bytes (both stores are
            // session-scoped). The rings, the ops table and the buffer generation are the
            // process's and every other session's GL buffers are still alive in the shared group.
            MGL_SERVER_STAGED_DROP_ALL();
#if MOBILEGL_BUILD_DISAGGREGATED
            MG_Record::ServerStagedTexture().DropAll();
#endif
        }

        void OnBackendContextDestroyed() {
            UnregisterBufferBackendOps(); // also bumps the buffer-mutation epoch
            ++g_bufferContextGeneration;
            // The generation moved AFTER the unregister bump above; re-open the
            // memos again so no stamp can predate the generation change.
            BumpBufferMutationEpoch();
            InvalidateArrayBufferBindingCache();
            InvalidateIndexedBufferBindingCache();
            InvalidatePixelBufferBindingCaches();
            // The rings' ids and persistent maps died with the context; drop the
            // handles (no GL) and let the next draw / texture upload recreate them.
            ResetRingForNewContext(UboRing());
            ResetRingForNewContext(UnpackRing());
            // R-11's server copies die with the context for the same reason the rings' ids do:
            // the resource twins they are keyed by are about to be rebuilt against a new
            // generation, and a shadow that outlived its twin would be looked up by a RECYCLED
            // address on the next allocation - which is the quietest possible wrong answer.
            MGL_SERVER_STAGED_DROP_ALL();
            // tx's texture shadows die for the same reason, keyed by handle rather than address
            // but with the same recycled-identity failure mode: a new context's allocator may
            // hand out a {slot, gen} the old one's store still answers for.
            MG_Record::ServerStagedTexture().DropAll();
            // P11 B2: every retired T0 import's fence named work on the context that just ended,
            // so all of them let go now. LIVE imports keep their reference: the next ensure
            // imports them again on the new context (EnsureBufferResourceForHandle).
#if MOBILEGL_BUILD_DISAGGREGATED
            ProcessDeferredT0Retires();
            SweepT0Retired(/*contextGone=*/true);
#endif
        }

        void ProcessDeferredBufferReleases() {
            // Runs on every draw; skip the context check, mutex and vector churn
            // outright when nothing was enqueued since the last drain.
            if (!g_hasDeferredBufferReleases.load(std::memory_order_acquire)) return;
            if (!CanTouchGLNow()) return;
            Vector<SharedPtr<BackendBufferResource>> releases;
            {
                const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
                releases.swap(g_deferredBufferReleases);
                g_hasDeferredBufferReleases.store(false, std::memory_order_release);
            }
            for (auto& resource : releases) {
                auto* glesResource = static_cast<GLESBufferResource*>(resource.get());
                if (glesResource->contextGeneration != g_bufferContextGeneration) {
                    glesResource->id = 0;
                    continue;
                }
                if (IsPoolable(*glesResource)) {
                    EnrollIntoPool(*glesResource); // recycle instead of glDeleteBuffers
                    continue;
                }
                if (glesResource->id != 0) {
                    if (g_boundArrayBufferKnown && g_boundArrayBufferId == glesResource->id) {
                        InvalidateArrayBufferBindingCache();
                    }
                    ScrubBufferBindingShadowsForId(glesResource->id);
                    g_GLESFuncs.glDeleteBuffers(1, &glesResource->id);
                    glesResource->id = 0;
                }
            }
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        // P11 B2: the T0 half of the deferred-release drain, run right after it at both sync
        // points (ProcessDeferredBufferReleases itself moves verbatim, G5). Retired imports whose
        // fence signalled let go of their AHardwareBuffer; a deferred T0 destroy is retired once
        // the drain deleted its id (the fence then follows the delete) or its context is gone.
        void ProcessDeferredT0Retires() {
            if (!g_t0Retired.empty()) SweepT0Retired(false);
            if (!g_hasT0DeferredDestroys.load(std::memory_order_acquire)) return;
            Vector<SharedPtr<BackendBufferResource>> ready;
            {
                const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
                for (SizeT i = 0; i < g_t0DeferredDestroys.size();) {
                    auto* r = static_cast<GLESBufferResource*>(g_t0DeferredDestroys[i].get());
                    if (r->id == 0 || r->contextGeneration != g_bufferContextGeneration) {
                        ready.push_back(std::move(g_t0DeferredDestroys[i]));
                        g_t0DeferredDestroys[i] = std::move(g_t0DeferredDestroys.back());
                        g_t0DeferredDestroys.pop_back();
                    } else {
                        ++i;
                    }
                }
                g_hasT0DeferredDestroys.store(!g_t0DeferredDestroys.empty(), std::memory_order_release);
            }
            for (auto& resource : ready) RetireT0Import(*static_cast<GLESBufferResource*>(resource.get()));
        }
#endif

        // The same five questions, asked of the applier instead of the frontend object, with
        // IDENTICAL semantics (D-A4):
        //   identity      the slot table's twin at this handle, not GetBackendResource()
        //   size          record.Desc.Width, not GetSize()
        //   freshness     record.Serial vs syncedChangeSerial, not GetChangeSerial()
        //   map state     record.HasLiveHostWrites AND the frontend's IsMapped() - see below
        //   the rest      unchanged, and server-side to begin with
        //
        // HasLiveHostWrites is ALWAYS FALSE in P3a and is written by nobody; it is here so the
        // phase that pushes persistent-mapped host writes can set it with no new record kind,
        // and the assertion below is what stops that phase landing a silent semantic change.
        //
        // BECAUSE it is pinned false, it CANNOT stand in for the frontend's IsMapped() yet, and
        // the probe still asks the object: an EMULATED (non-adopted) persistent map - under the
        // 16 MiB adoption threshold, or with DisableLargeBufferAdoption, or with no
        // EXT_buffer_storage - mutates the client's shadow with no call, no serial and no epoch,
        // which is the entire reason the legacy probe asks a map question instead of a serial
        // one. Answering only HasLiveHostWrites made such a buffer read draw-clean forever, so
        // SyncPersistentMappedRange (D-N keeps it on the ensure path for all of P3a) was never
        // reached again and the frame drew the last uploaded bytes with no diagnostic anywhere.
        // The frontend read retires the moment P5 gives HasLiveHostWrites a producer, and it is
        // the last frontend read in this function.
        Bool IsBufferDrawCleanByHandle(MG_Pipe::MGPipeHandle res, const GLESBufferResource* resource,
                                       const MG_State::GLState::BufferObject* frontend) {
            if (!resource) return false;
            const auto* twin = g_backendBufferResources.FindByHandle(res);
            if (twin == nullptr || twin->get() != resource) return false;
            if (resource->contextGeneration != g_bufferContextGeneration) return false;
            if (resource->id == 0) return false;
            if (resource->persistentMapped) {
                return resource->persistentPtr != nullptr && resource->pendingResidentWrites.empty();
            }
            const auto* record = ResourceRecordOf(res);
            if (record == nullptr) return false;
            if (record->HasLiveHostWrites) return false;
            // THE LAST FRONTEND READ IN THIS FUNCTION, AND P5 b1 RETIRES IT - under split
            // only, because that is the only build where it is both wrong and replaceable.
            //
            // It asked the object whether it was mapped because HasLiveHostWrites was pinned
            // false and an emulated persistent map mutates the shadow with no call, no serial
            // and no epoch; answering from the record alone made such a buffer read
            // draw-CLEAN forever, SyncPersistentMappedRange was never reached again, and the
            // frame drew the last uploaded bytes with no diagnostic
            // (MG_Test/SanityTest.cpp's DirectGLESBufferDrawProbe is exactly that case).
            //
            // TWO THINGS REPLACE IT AND BOTH HAD TO LAND FIRST: the record now carries the map
            // state (the line above, from MGPSubData::HasLiveHostWrites), and the client
            // pushes the mapped span by block at every validate point, so the serial moves for
            // a write the application made with no API call. Under a spawn there is no object
            // on this side to ask, which is why this was never going to stay a choice.
            //
            // A null object is the "no frontend to ask" case and is treated as "not mapped",
            // which is what the record already says.
            if (resource->pendingRespecify || !resource->storageInitialized) return false;
            if (!resource->pendingRanges.empty()) return false;
            if (resource->storageSize != static_cast<SizeT>(record->Desc.Width)) return false;
            return resource->syncedChangeSerial.load(std::memory_order_acquire) == record->Serial;
        }

        // The handle arm of the ensure path. Same shape, same order, same branches; the four
        // frontend reads become the applier's stored descriptor, its Serial and the shadow base
        // the last content-carrying call handed over.
        //
        // It still takes the frontend object for ONE reason, recorded rather than hidden:
        // BufferObject::SyncPersistentMappedRange() is one of the eleven Espryt sites
        // ROADMAP/D-N explicitly keeps where it is for P3a (the per-site attribution table is
        // P8's to execute). Every other line here is handle-shaped. When P8 moves that call to
        // the client this signature loses its last frontend argument.
        GLESBufferResource* EnsureBufferResourceForHandle(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject,
                                                          MG_Pipe::MGPipeHandle res) {
            auto* resource = GetOrCreateBufferResourceForHandle(res);
            if (!resource) return nullptr;

            // The client's shadow base, RE-READ at every use, exactly as the legacy arm re-read
            // MappedData() at every use. It is deliberately NOT resource->hostBytes here: two
            // ordinary events move or free what a base recorded by an earlier call points at -
            // a shadow resize (PipeResource::ResizeShadow is reserve + resize, so a grow past
            // the reserve reallocates) and persistent-map adoption (the shadow is cleared and
            // shrunk to fit) - and this path still holds the frontend object anyway, because
            // D-N keeps SyncPersistentMappedRange here for all of P3a. hostBytes stays the
            // fallback for the drains that have NO object (the readback flush), and it is
            // nulled at both of those events.
            const auto liveHostBase = [&resource, &bufferObject]() -> const Uint8* {
                // M-2 / codex 3 / ID-52 item 3: under an ACTIVE TRANSPORT the server's staged
                // copy (resource->hostBytes, filled by MGL_SERVER_STAGED_ADOPT in Ops_H_SubData /
                // Ops_H_FlushRange) is the ONLY authoritative base. Preferring the frontend
                // object's MappedData() here - which under inproc is always non-null, same process
                // - meant every reduced-path drain read the CLIENT's shadow and NEVER the server's,
                // so a corrupt staged upload rendered the frontend's correct bytes and the R-2.5
                // 0xDD audit could not reach a draw. R-2 / table 3: honest inproc is inproc that
                // does not read the client object's memory. Under monolith nothing changes.
                return resource->hostBytes;
            };

            if (resource->contextGeneration != g_bufferContextGeneration) {
                // The id (if any) belonged to a destroyed ES context.
                resource->id = 0;
                resource->storageInitialized = false;
                resource->storageSize = 0;
                resource->pendingRespecify = true;
                resource->pendingRanges.clear();
                resource->pendingResidentWrites.clear();
                resource->contextGeneration = g_bufferContextGeneration;
                resource->persistentMapped = false;
                resource->persistentPtr = nullptr;
                resource->immutableStorage = false;
                // m-5 / codex 5: OnBackendContextDestroyed ran MGL_SERVER_STAGED_DROP_ALL(), which
                // frees every server shadow but does NOT null the hostBytes that name them - so a
                // twin that SURVIVES a context loss still carries a base into the freed allocation.
                // Null it here, where a surviving twin is re-armed - but ONLY when the shadow is
                // really gone. This block also runs on a twin's FIRST ensure (contextGeneration
                // starts mismatched), and there the shadow a preceding resource_subdata just staged
                // is still live; nulling it then would drop the reduced path's own bytes (it did,
                // and TriangleScenario read the wrong VBO). HasShadow is the discriminator: false
                // after DropAll, true after an ordinary Adopt.
                //
                // TRANSPORT-GUARDED like the other two split hunks in this file (review v2 N-7):
                // under MONOLITH transport in a disaggregated build the store never copies, Adopt
                // never sets m_any, HasShadow answers false for everything, and this null would
                // run on every twin's first ensure - harmless there only because liveHostBase()
                // prefers MappedData() under monolith, and "under monolith nothing changes" should
                // be true by construction rather than by luck.
                if (ServerStaged().CopiesIntoServerStorage() &&
                    !ServerStaged().HasShadow(resource)) {
                    resource->hostBytes = nullptr;
                }
#if defined(__ANDROID__) && MOBILEGL_BUILD_DISAGGREGATED
                // P11 B2: A T0 STORE OUTLIVES ITS CONTEXT. Its pages are the client's
                // AHardwareBuffer, which this twin still holds, so the store is imported again on
                // the new context - the client keeps writing through the same pointer and nothing
                // has to be re-sent. A refusal here leaves the twin without a store and says so.
                if (resource->externalAhb != nullptr) {
                    String why;
                    void* ptr = nullptr;
                    const Uint64 size = ResourceWidthOf(res);
                    const Uint id = ResolveT0EntryPoints(why) ? ImportAhbAsBuffer(resource->externalAhb, size, &ptr, why)
                                                              : 0;
                    if (id != 0) {
                        InstallT0Store(*resource, id, ptr, size);
                        MGLOG_I("Espryt T0: store {slot=%u, gen=%u} re-imported on the new context (%llu bytes)",
                                res.Slot, res.Gen, static_cast<unsigned long long>(size));
                    } else {
                        MGLOG_E("Espryt T0: store {slot=%u, gen=%u} could not be re-imported after a context loss - "
                                "%s; the store has no GPU backing",
                                res.Slot, res.Gen, why.c_str());
                        RetireT0Import(*resource);
                    }
                }
#endif
            }

            // An immutable store nothing maps any more, retired here on the thread that can.
            if (resource->immutableStorage && !resource->persistentMapped && resource->id != 0) {
                NoteBufferIdDeleted(resource->id);
                ++g_bufferBackendIdGeneration;
                g_GLESFuncs.glDeleteBuffers(1, &resource->id);
                resource->id = 0;
                resource->immutableStorage = false;
                resource->storageInitialized = false;
                resource->storageSize = 0;
                resource->pendingRespecify = true;
            }

            // Zero-copy coherent persistent buffer: nothing to (re)upload at draw time.
            if (resource->persistentMapped && resource->persistentPtr && resource->id != 0) {
                DrainResidentWritesNow(*resource);
                return resource;
            }

            if (resource->id == 0) {
                const SizeT poolSize = ResourceWidthOf(res);
                // The pool DECISION is the legacy one, unchanged: extent and residence only.
                // An earlier cut added "and a shadow base was recorded", which silently stopped
                // a buffer recycling pool ids - a change to D-F's pool behaviour, not to "the
                // source of bytes" the conversion is allowed to move, and one that moves
                // AcquireFromPool call counts StreamedArenaScenario observes.
                const Uint reused =
                    (poolSize > 0 && !resource->persistentMapped) ? AcquireFromPool(poolSize) : 0;
                if (reused != 0) {
                    resource->id = reused;
                    resource->storageSize = poolSize;
                    resource->storageInitialized = true;
                    resource->pendingRespecify = false;
                    BindBufferId(TempBufferTarget, reused);
                    // M-3 / codex 4: this is a WHOLE-STORE upload from the base, and under split
                    // the base is the server shadow (M-2). For a store whose content the
                    // application SUPPLIED, zero-filled bytes past the staged coverage are not the
                    // application's - uploading them is the silent data loss the M-6 ruling
                    // forbids, and a gap is a missing record: Fatal by name. For a store the
                    // application ORPHANED the gap is its own undefined content and the upload is
                    // legal (ResourceContentIsDeclared, above). RequireCoverage is a no-op for the
                    // legacy arm's MappedData() and for a non-copying store.
                    if (ResourceContentIsDeclared(res)) {
                        MGL_SERVER_STAGED_REQUIRE(*resource, liveHostBase(), 0, poolSize,
                                                  "pool_reuse_whole_store");
                    }
                    g_GLESFuncs.glBufferSubData(TempBufferTarget, 0, (GLsizeiptr)poolSize, liveHostBase());
                    if (MG_Util::PipeStats::Enabled()) {
                        MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                                     static_cast<Uint64>(poolSize));
                    }
                    {
                        const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                        resource->pendingRanges.clear();
                        resource->pendingResidentWrites.clear();
                    }
                    resource->syncedChangeSerial = ResourceSerialOf(res);
                } else {
                    g_GLESFuncs.glGenBuffers(1, &resource->id);
                    if (resource->id == 0) {
                        MGLOG_E_ONCE("Failed to generate buffer object.");
                        MGLOG_E_ONCE("ES glGetError(): %s",
                                     MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
                        return resource;
                    }
                    resource->storageInitialized = false;
                    resource->pendingRespecify = true;
                }
            }

            // D-N keeps this call here for P3a. It can queue ranges and move the record, so
            // everything below is read AFTER it.
            // P5c (hd, CONTRACT-P5C §3.8): under an active transport the frontend accessor is a
            // layer-1 surface ("buffer-legacy-arm") and the client's own pre-verb
            // persistent-map push is the only producer; the call is skipped outright. Under
            // monolith D-N's placement stands.

            const auto* record = ResourceRecordOf(res);
            const SizeT size = record != nullptr ? static_cast<SizeT>(record->Desc.Width) : 0;
            if (size == 0) {
                return resource;
            }
            const Uint64 serial = record->Serial;
            const GLenum usage = MG_Util::ConvertBufferUsageToGLEnum(static_cast<BufferUsage>(record->Desc.Usage));
            // Read AFTER SyncPersistentMappedRange above, and through liveHostBase for the
            // reason written where it is declared.
            const Uint8* const hostBase = liveHostBase();
            // AND PUBLISH IT, because the readers that have NO frontend object read
            // resource->hostBytes and nothing else: the readback flush (Ops_H_Readback), the
            // kill-switch map arm of Ops_H_FlushRange, and the fp64 narrowing
            // (SyncConvertedAttributeByHandle, whose legacy counterpart reads
            // bufferObject->MappedData()). A twin is created LAZILY - D-A2's row makes
            // Ops_H_Create a no-op - so the very first resource_respecify of a buffer finds
            // FindBufferResourceForHandle == nullptr and its base is dropped on the floor; with
            // glBufferData(..., data) followed by no further content call (the ordinary static
            // vertex array) hostBytes then stayed null for the object's whole life and the fp64
            // narrowing refused every draw, disabling the array. This is the one place that both
            // holds the object and runs before every draw that uses the store, so it is where the
            // base is refreshed. Only reached for a NON-adopted resource (the adopted arm
            // returned above), so C-2's rule is intact: an adopted store's hostBytes stays null
            // and its bytes are read through persistentPtr.
            if (hostBase != nullptr) resource->hostBytes = hostBase;
            // "DOES THE SHADOW THIS PATH IS ABOUT TO UPLOAD HOLD MEANINGFUL BYTES?" - and it has
            // to be asked of the SAME thing the bytes come from. The legacy arm pairs
            // bufferObject.MappedData() with bufferObject.HasDefinedContent() (RespecifyStorageNow)
            // and this arm pairs the same two, so the source of the bytes and the statement about
            // them can never disagree.
            //
            // P5c (hd, CONTRACT-P5C §3.6 / B1): with an active transport the answer is the
            // DESCRIPTOR's bit - which the client publishes on ResourceCreate/Respecify - OR the
            // staged coverage the content records behind it delivered. The coverage half is what
            // keeps the answer equal to the frontend flag's in the one case the descriptor alone
            // cannot see: an ORPHANING respecify (descriptor clear) followed by glBufferSubData,
            // which defines content without a new descriptor - the bytes crossed as
            // resource_subdata and the staged store's coverage is exactly "content defined since
            // the last orphan". With no coverage and a clear bit the store is undefined by the
            // application's own declaration and the upload stays a pure NULL reallocation.
            const Bool shadowHasContent =
                (record->Desc.HasDefinedContent != 0 ||
                       (ServerStaged().CopiesIntoServerStorage() ? ServerStaged().CoveredRunCount(resource) != 0
                                                                 : resource->contentSinceRespecify));
            const void* initialData = shadowHasContent ? hostBase : nullptr;
            // M-3 / codex 4: a RespecifyStorageWith(..., initialData != nullptr) is a WHOLE-STORE
            // [0, size) upload from the base, so it owes the same coverage the pending-range drain
            // below owes - the two respecify arms were the readers M-6's "never widened" rule did
            // not reach. No-op for the legacy arm's MappedData() and for a non-copying store; a
            // Fatal{StageSnapshotTooNarrow, "respecify_whole_store"} under split when the
            // DESCRIPTOR says the application supplied the content and the shadow's coverage does
            // not span the store (a missing record), instead of uploading its zero-fill as content.
            // NOT for a store the application orphaned: there `shadowHasContent` is the frontend
            // object's flag, which the streaming idiom's partial glBufferSubData flips to true, and
            // the bytes it did not write are undefined by its own glBufferData(NULL) - the rule at
            // ResourceContentIsDeclared. Round 2 refused that idiom and aborted six joint entries.
            const auto requireWholeStoreCoverage = [&]() {
                if (initialData != nullptr && record->Desc.HasDefinedContent != 0) {
                    MGL_SERVER_STAGED_REQUIRE(*resource, static_cast<const Uint8*>(initialData), 0,
                                              size, "respecify_whole_store");
                }
            };

            if (resource->pendingRespecify || !resource->storageInitialized || resource->storageSize != size) {
                requireWholeStoreCoverage();
                RespecifyStorageWith(*resource, size, usage, initialData, serial);
            } else if (!resource->pendingRanges.empty()) {
                // Same rule as Ops_H_Readback's, at the draw-time drain: with no frontend object
                // `hostBase` is the server shadow and every queued range must be staged.
                RenameSmallStoreForDrain(*resource, hostBase, size);
                MGL_SERVER_STAGED_REQUIRE_PENDING(*resource, hostBase, size, "ensure_flush_pending");
                FlushPendingRangesFrom(*resource, hostBase, size);
                resource->syncedChangeSerial = serial;
            } else if (resource->syncedChangeSerial != serial) {
                // Mutations this backend could not track (the table was unregistered between
                // contexts); re-upload everything.
                requireWholeStoreCoverage();
                RespecifyStorageWith(*resource, size, usage, initialData, serial);
            }
            return resource;
        }

        void BindBufferId(GLenum target, Uint id) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (target == GL_ARRAY_BUFFER) {
                if (g_boundArrayBufferKnown && g_boundArrayBufferId == id) {
                    return;
                }
                g_boundArrayBufferId = id;
                g_boundArrayBufferKnown = true;
            }
            g_GLESFuncs.glBindBuffer(target, id);
        }

        void InvalidateArrayBufferBindingCache() {
            g_boundArrayBufferId = 0;
            g_boundArrayBufferKnown = false;
        }

        void BindPixelPackBufferId(Uint id) {
            if (g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == id) {
                return;
            }
            g_GLESFuncs.glBindBuffer(GL_PIXEL_PACK_BUFFER, id);
            g_boundPixelPackBufferId = id;
            g_boundPixelPackBufferKnown = true;
        }

        void BindPixelUnpackBufferId(Uint id) {
            if (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == id) {
                return;
            }
            g_GLESFuncs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, id);
            g_boundPixelUnpackBufferId = id;
            g_boundPixelUnpackBufferKnown = true;
        }

        void InvalidatePixelBufferBindingCaches() {
            g_boundPixelPackBufferId = 0;
            g_boundPixelPackBufferKnown = false;
            g_boundPixelUnpackBufferId = 0;
            g_boundPixelUnpackBufferKnown = false;
        }

        void NoteBufferIdDeleted(Uint id) {
            if (id == 0) {
                return;
            }
            if (g_boundArrayBufferKnown && g_boundArrayBufferId == id) {
                InvalidateArrayBufferBindingCache();
            }
            ScrubBufferBindingShadowsForId(id);
        }

        namespace {
            // Shadow of the GL indexed buffer bindings so redundant glBindBufferBase/Range
            // (same index + id + range) are skipped. isBase distinguishes a whole-buffer
            // base bind from a sub-range bind. Fresh/reset context: every point is base(0)
            // == unbound, which matches the GL default.
            struct IndexedBufferBinding {
                Uint id = 0;
                GLintptr offset = 0;
                GLsizeiptr size = 0;
                Bool isBase = true;
                // False when the driver's binding at this point is no longer described by the
                // fields above and the next bind must be issued whatever it asks for. Set by
                // InvalidateIndexedBufferBindingShadowsForId after a store was re-specified at
                // a new size: the id is still bound, so the entry must NOT be scrubbed to
                // base(0) (a later bind of 0 would then be false-skipped) - only distrusted.
                Bool known = true;
            };
            constexpr SizeT kMaxIndexedBufferBindings = 64;
            IndexedBufferBinding g_indexedUBOBindings[kMaxIndexedBufferBindings];
            IndexedBufferBinding g_indexedSSBOBindings[kMaxIndexedBufferBindings];
            // Transform feedback gets a shadow for a reason the other two do not have: the
            // capture points are synced from the application's TOUCHED high-water mark, which
            // deqp/glcts permanently raises to GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS by
            // clearing every point after each test case. Without a shadow every capture that
            // uses fewer points than that (i.e. every INTERLEAVED_ATTRIBS capture) re-issued a
            // redundant glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, i, 0) for the unused
            // tail immediately before glBeginTransformFeedback - calls a plain GL application
            // never makes there, and the only thing MobileGL does differently from one.
            //
            // Unlike the UBO/SSBO points these are NOT context state: they belong to the bound
            // transform feedback OBJECT, so XfbImpl::BindTransformFeedback drops the whole
            // shadow to unknown on every object switch (InvalidateTransformFeedbackBindingShadows).
            IndexedBufferBinding g_indexedXFBBindings[kMaxIndexedBufferBindings];
            IndexedBufferBinding* IndexedBindingShadow(GLenum glTarget, Uint index) {
                if (index >= kMaxIndexedBufferBindings) return nullptr; // out of range: never cache
                if (glTarget == GL_UNIFORM_BUFFER) return &g_indexedUBOBindings[index];
                if (glTarget == GL_SHADER_STORAGE_BUFFER) return &g_indexedSSBOBindings[index];
                if (glTarget == GL_TRANSFORM_FEEDBACK_BUFFER) return &g_indexedXFBBindings[index];
                return nullptr;
            }

            // glDeleteBuffers resets the deleted buffer's bindings (indexed and
            // pixel pack/unpack ones included) to 0 in the current context; mirror
            // that in the shadows, or a later buffer recycling the same name with a
            // matching shadow entry would false-skip its rebind. Default
            // IndexedBufferBinding{} == base(0) == the post-delete GL state.
            void ScrubBufferBindingShadowsForId(Uint id) {
                if (id == 0) return;
                for (auto& binding : g_indexedUBOBindings) {
                    if (binding.id == id) binding = {};
                }
                for (auto& binding : g_indexedSSBOBindings) {
                    if (binding.id == id) binding = {};
                }
                for (auto& binding : g_indexedXFBBindings) {
                    if (binding.id == id) binding = {};
                }
                if (g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == id) {
                    g_boundPixelPackBufferId = 0;
                }
                if (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == id) {
                    g_boundPixelUnpackBufferId = 0;
                }
            }

            void InvalidateIndexedBufferBindingShadowsForId(Uint id) {
                if (id == 0) return;
                for (auto& binding : g_indexedUBOBindings) {
                    if (binding.id == id) binding.known = false;
                }
                for (auto& binding : g_indexedSSBOBindings) {
                    if (binding.id == id) binding.known = false;
                }
                for (auto& binding : g_indexedXFBBindings) {
                    if (binding.id == id) binding.known = false;
                }
            }
        } // namespace

        // The capture points belong to the bound transform feedback object, so a bind (or a
        // delete, which reverts to the default object) replaces all of them at once with
        // state this shadow has never seen. Distrust rather than scrub: the driver's bindings
        // are whatever the newly bound object holds, which is NOT necessarily base(0), and
        // scrubbing would let a later bind of 0 be false-skipped.
        void InvalidateTransformFeedbackBindingShadows() {
            for (auto& binding : g_indexedXFBBindings) binding.known = false;
        }

        void BindBufferBaseCached(GLenum glTarget, Uint index, Uint id) {
            auto* s = IndexedBindingShadow(glTarget, index);
            if (s && s->known && s->isBase && s->id == id) return;
            g_GLESFuncs.glBindBufferBase(glTarget, index, id);
            if (s) *s = {id, 0, 0, true, true};
        }

        void BindBufferRangeCached(GLenum glTarget, Uint index, Uint id, GLintptr offset, GLsizeiptr size) {
            auto* s = IndexedBindingShadow(glTarget, index);
            if (s && s->known && !s->isBase && s->id == id && s->offset == offset && s->size == size) return;
            g_GLESFuncs.glBindBufferRange(glTarget, index, id, offset, size);
            if (s) *s = {id, offset, size, false, true};
        }

        void InvalidateIndexedBufferBindingCache() {
            for (auto& b : g_indexedUBOBindings) b = {};
            for (auto& b : g_indexedSSBOBindings) b = {};
            for (auto& b : g_indexedXFBBindings) b = {};
        }

        void TrimBufferPool() {
            const std::lock_guard<std::mutex> lock(g_poolMutex);
            if (g_pooledBytes <= kMaxPoolBytes) return;
            // Over budget: evict oldest-retireSerial entries with real glDeleteBuffers.
            while (g_pooledBytes > kMaxPoolBytes) {
                SizeT oldestKey = 0, oldestIdx = 0;
                Uint64 oldestSerial = ~Uint64{0};
                Bool found = false;
                for (auto& kv : g_bufferPool) {
                    for (SizeT i = 0; i < kv.second.size(); ++i) {
                        if (kv.second[i].retireSerial < oldestSerial) {
                            oldestSerial = kv.second[i].retireSerial;
                            oldestKey = kv.first;
                            oldestIdx = i;
                            found = true;
                        }
                    }
                }
                if (!found) break;
                auto& bucket = g_bufferPool[oldestKey];
                PooledBuffer& e = bucket[oldestIdx];
                if (e.contextGeneration == g_bufferContextGeneration && e.id != 0) {
                    ScrubBufferBindingShadowsForId(e.id);
                    g_GLESFuncs.glDeleteBuffers(1, &e.id);
                }
                g_pooledBytes -= e.size;
                bucket[oldestIdx] = bucket.back();
                bucket.pop_back();
            }
        }

        void ClearBufferPool() {
            const std::lock_guard<std::mutex> lock(g_poolMutex);
            // Ids belong to the dying context; drop without glDeleteBuffers (mirrors
            // the g_deferredBufferReleases.clear() discipline).
            g_bufferPool.clear();
            g_pooledBytes = 0;
        }

        // --- Persistent-mapped bump rings (see Managers.h) ------------------------
            // (Re)create the ring store with room for at least minBytes. Any live
            // store is retired (deleted once the GPU finished the last frame that
            // could reference its slots), never deleted in place. Returns false and
            // leaves the current store untouched when minBytes cannot fit under the
            // size cap; a GL failure loses the store and latches creationFailed so
            // later uses stop retrying under this context.
            Bool CreateRingStorage(PersistentRing& ring, SizeT minBytes) {
                SizeT newSize = ring.initialBytes;
                while (newSize < minBytes) newSize *= 2;
                if (newSize > ring.maxBytes) return false;

                if (ring.store.id != 0) {
                    ring.retired.push_back(
                        {ring.store.id, ring.store.contextGeneration, DirectGLES::CurrentFrameSerial() + 1});
                }
                const Uint32 nextGeneration = ring.store.generation + 1;
                ring.store.id = 0;
                ring.store.mappedPtr = nullptr;

                Uint id = 0;
                g_GLESFuncs.glGenBuffers(1, &id);
                if (id != 0) {
                    BindBufferId(TempBufferTarget, id);
                    g_GLESFuncs.glBufferStorageEXT(TempBufferTarget, static_cast<GLsizeiptr>(newSize), nullptr,
                                                   GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                    void* ptr = g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(newSize),
                                                             GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                    if (!ptr) {
                        // The dying id is what the array-buffer cache has recorded as
                        // bound; a later buffer recycling the name would false-skip.
                        InvalidateArrayBufferBindingCache();
                        g_GLESFuncs.glDeleteBuffers(1, &id);
                        id = 0;
                    } else {
                        ring.store.mappedPtr = static_cast<Uint8*>(ptr);
                    }
                }
                if (id == 0) {
                    MGLOG_E_ONCE("%s: persistent storage creation failed (%zu bytes); falling back to the "
                                 "client-memory upload path.",
                                 ring.label, newSize);
                    ring.store.creationFailed = true;
                    return false;
                }

                SizeT alignment = ring.fixedAlignment;
                if (alignment == 0) {
                    const GLint capsAlignment = g_GLESCapabilities.UniformBufferOffsetAlignment;
                    alignment = capsAlignment > 0 ? static_cast<SizeT>(capsAlignment) : 256;
                }
                ring.store.id = id;
                ring.store.size = newSize;
                ring.store.head = 0;
                ring.store.tail = 0;
                ring.store.generation = nextGeneration;
                ring.store.alignment = alignment;
                ring.frameMarks.clear();
                MGLOG_D("%s: %zu MiB persistent store ready (id %u, gen %u, align %zu).", ring.label,
                        newSize / (1024u * 1024u), id, nextGeneration, alignment);
                return true;
            }

        namespace {
            // Shared half of the availability gate (the per-ring kill switch sits in
            // the exported wrappers). Reclamation rides the Present fence watermark;
            // without working fences slots would never be provably GPU-idle (same rule
            // as IsPoolable).
            Bool RingAvailable(PersistentRing& ring) {
                if (!g_GLESFuncs.glBufferStorageEXT || !g_GLESFuncs.glMapBufferRange || !g_GLESFuncs.glGenBuffers ||
                    !g_GLESFuncs.glFenceSync || !g_GLESFuncs.glGetSynciv) {
                    return false;
                }
                if (!CanTouchGLNow()) return false;
                if (ring.store.contextGeneration != g_bufferContextGeneration) {
                    ResetRingForNewContext(ring);
                }
                return !ring.store.creationFailed;
            }

            // Division-based rounding fallback: the spec doesn't promise a power-of-two
            // alignment. Slot offsets stay multiples of the alignment because every
            // slot size is, and wrap padding restarts at ring offset 0.
            inline SizeT RingAlignUp(SizeT size, SizeT alignment) {
                if ((alignment & (alignment - 1)) == 0) {
                    return (size + alignment - 1) & ~(alignment - 1);
                }
                return (size + alignment - 1) / alignment * alignment;
            }
        } // namespace

            // Bump-allocate `size` bytes out of `ring`.
            //
            // Fast path: a live store under the current context with room before both
            // the wrap boundary and the in-flight tail. Touches no GL and probes no
            // frame marks - callers sit behind the ring's availability gate, so the
            // context checks have already run for this draw/upload. `tail` may be stale
            // here (marks are only retired on Present and on the slow path); staleness
            // is conservative - the in-flight span reads too large, the check fails,
            // and the slow path retires marks and re-tries.
            Bool RingAllocate(PersistentRing& ring, SizeT size, SizeT& outOffset) {
                if (size == 0) return false;
                auto& store = ring.store;
                if (store.id != 0 && store.contextGeneration == g_bufferContextGeneration) {
                    const SizeT alignedSize = RingAlignUp(size, store.alignment);
                    // Ring sizes are the ring's initial size (a power of two) doubled
                    // some number of times, so the offset modulo reduces to a mask.
                    static_assert((kUboRingInitialBytes & (kUboRingInitialBytes - 1)) == 0,
                                  "ring offset mask below requires power-of-two ring sizes");
                    static_assert((kUnpackRingInitialBytes & (kUnpackRingInitialBytes - 1)) == 0,
                                  "ring offset mask below requires power-of-two ring sizes");
                    static_assert((kUploadRingInitialBytes & (kUploadRingInitialBytes - 1)) == 0,
                                  "ring offset mask below requires power-of-two ring sizes");
                    const SizeT offset = static_cast<SizeT>(store.head & (store.size - 1));
                    if (offset + alignedSize <= store.size && store.head + alignedSize - store.tail <= store.size) {
                        store.head += alignedSize;
                        outOffset = offset;
                        return true;
                    }
                }
                return RingAllocateSlow(ring, size, outOffset);
            }

            Bool RingAllocateSlow(PersistentRing& ring, SizeT size, SizeT& outOffset) {
                if (!RingAvailable(ring)) return false;
                auto& store = ring.store;
                // Sizing the store first, so the request is rounded with the alignment
                // the live store actually carries rather than the pre-creation default.
                if (store.id == 0 && !CreateRingStorage(ring, RingAlignUp(size, store.alignment))) {
                    return false;
                }
                const SizeT alignedSize = RingAlignUp(size, store.alignment);

                // Advance tail past every frame the GPU provably finished.
                const Uint64 completed = DirectGLES::CompletedFrameSerial();
                SizeT retiredMarks = 0;
                for (const auto& mark : ring.frameMarks) {
                    if (mark.frameSerial > completed) break;
                    if (mark.headAtPresent > store.tail) store.tail = mark.headAtPresent;
                    ++retiredMarks;
                }
                if (retiredMarks > 0) {
                    ring.frameMarks.erase(ring.frameMarks.begin(),
                                          ring.frameMarks.begin() + static_cast<std::ptrdiff_t>(retiredMarks));
                }

                // A slot may not straddle the ring end; pad the cursor to the boundary.
                SizeT offset = static_cast<SizeT>(store.head % store.size);
                if (offset + alignedSize > store.size) {
                    store.head += store.size - offset;
                    offset = 0;
                }

                if (store.head + alignedSize - store.tail > store.size) {
                    // In-flight span would overrun live slots: grow instead of overwrite.
                    if (CreateRingStorage(ring, std::max(store.size * 2, alignedSize))) {
                        offset = 0;
                    } else if (store.creationFailed) {
                        return false; // store lost; callers fall back to their legacy path
                    } else {
                        // At the size cap (>maxBytes in flight). First try to free room
                        // by waiting for the OLDEST in-flight frames to retire - a
                        // bounded wait that ends as soon as enough tail space exists,
                        // instead of draining the entire queue.
                        constexpr Uint64 kFrameWaitNs = 50ull * 1000 * 1000; // 50ms per frame
                        while (!ring.frameMarks.empty() &&
                               store.head + alignedSize - store.tail > store.size) {
                            const auto& oldest = ring.frameMarks.front();
                            if (!DirectGLES::WaitForFrameSerialCompleted(oldest.frameSerial, kFrameWaitNs)) {
                                break;
                            }
                            if (oldest.headAtPresent > store.tail) store.tail = oldest.headAtPresent;
                            ring.frameMarks.erase(ring.frameMarks.begin());
                        }
                        if (store.head + alignedSize - store.tail <= store.size) {
                            offset = static_cast<SizeT>(store.head % store.size);
                            if (offset + alignedSize > store.size) {
                                store.head += store.size - offset;
                                offset = 0;
                            }
                            store.head += alignedSize;
                            outOffset = offset;
                            return true;
                        }
                        // No usable fence covers the oldest frames: drain once rather than
                        // corrupt live slots.
                        if (g_GLESFuncs.glFinish) g_GLESFuncs.glFinish();
                        store.tail = store.head;
                        ring.frameMarks.clear();
                        // Same-frame slots written before the drain may now be recycled by
                        // the very next allocations; a generation bump keeps later draws
                        // from rebinding those cached offsets.
                        ++store.generation;
                        offset = static_cast<SizeT>(store.head % store.size);
                        if (offset + alignedSize > store.size) {
                            store.head += store.size - offset;
                            offset = 0;
                        }
                    }
                }

                store.head += alignedSize;
                outOffset = offset;
                return true;
            }

        namespace {
            void DeleteOrphanRingBuffers() {
                const std::lock_guard<std::mutex> lock(g_orphanRingBuffersMutex);
                auto& orphans = OrphanRingBuffers();
                if (orphans.empty()) return;
                for (const OrphanRingBuffer& orphan : orphans) {
                    // A buffer of a share group that is gone was reclaimed with it.
                    if (orphan.contextGeneration != g_bufferContextGeneration) continue;
                    ScrubBufferBindingShadowsForId(orphan.id);
                    g_GLESFuncs.glDeleteBuffers(1, &orphan.id);
                }
                orphans.clear();
            }

            // Present()-time upkeep shared by both rings.
            void RingOnPresent(PersistentRing& ring) {
                if (!CanTouchGLNow()) return;
                DeleteOrphanRingBuffers();

                // Delete grown-away stores the GPU is provably done with.
                const Uint64 completed = DirectGLES::CompletedFrameSerial();
                for (SizeT i = ring.retired.size(); i-- > 0;) {
                    RetiredRingStore& entry = ring.retired[i];
                    const Bool staleContext = entry.contextGeneration != g_bufferContextGeneration;
                    if (!staleContext && entry.retireSerial > completed) continue;
                    if (!staleContext && entry.id != 0) {
                        ScrubBufferBindingShadowsForId(entry.id);
                        g_GLESFuncs.glDeleteBuffers(1, &entry.id);
                    }
                    ring.retired[i] = ring.retired.back();
                    ring.retired.pop_back();
                }

                auto& store = ring.store;
                if (store.id == 0 || store.contextGeneration != g_bufferContextGeneration) return;
                // Retire completed marks here too — RingAllocate is the main consumer,
                // but frames that used the ring for nothing would otherwise let the list
                // grow one entry per Present, unboundedly.
                SizeT retiredMarks = 0;
                for (const auto& mark : ring.frameMarks) {
                    if (mark.frameSerial > completed) break;
                    if (mark.headAtPresent > store.tail) store.tail = mark.headAtPresent;
                    ++retiredMarks;
                }
                if (retiredMarks > 0) {
                    ring.frameMarks.erase(ring.frameMarks.begin(),
                                          ring.frameMarks.begin() + static_cast<std::ptrdiff_t>(retiredMarks));
                }
                // Record this frame's high-water mark (Present just fenced the serial now
                // reported by CurrentFrameSerial()). A fence-less Present repeats the
                // serial; fold into the existing mark.
                const Uint64 serial = DirectGLES::CurrentFrameSerial();
                if (!ring.frameMarks.empty() && ring.frameMarks.back().frameSerial == serial) {
                    ring.frameMarks.back().headAtPresent = store.head;
                } else {
                    ring.frameMarks.push_back({serial, store.head});
                }
            }
        } // namespace

        Bool UboRingAvailable() {
            if (MG_Config::Features.EsprytDisableUboRing) return false;
            return RingAvailable(UboRing());
        }

        Bool UboRingAllocate(SizeT size, SizeT& outOffset) { return RingAllocate(UboRing(), size, outOffset); }

        UboRingSlotState UboRingPrepareSlot(UboRingAllocation& slot, Uint32 contentVersion, Uint64 frameSerial,
                                            SizeT bindSize, void*& outWrite, Uint& outBufferId) {
            if (MG_Config::Features.EsprytDisableUboRing) return UboRingSlotState::Unavailable;
            PersistentRing& ring = UboRing();
            if (!RingAvailable(ring)) return UboRingSlotState::Unavailable;
            if (slot.ringGeneration == ring.store.generation && slot.frameSerial == frameSerial &&
                slot.contentVersion == contentVersion) {
                outBufferId = ring.store.id;
                return UboRingSlotState::Reused;
            }
            SizeT offset = 0;
            if (!RingAllocate(ring, bindSize, offset)) return UboRingSlotState::Failed;
            // Read after the allocation: a grow mints a new store and generation.
            outWrite = static_cast<Uint8*>(ring.store.mappedPtr) + offset;
            slot = {contentVersion, ring.store.generation, frameSerial, offset};
            outBufferId = ring.store.id;
            return UboRingSlotState::Allocated;
        }

        void* UboRingMappedPtr() { return UboRing().store.mappedPtr; }
        Uint UboRingBufferId() { return UboRing().store.id; }
        Uint32 UboRingGeneration() { return UboRing().store.generation; }

        void UboRingOnPresent() { RingOnPresent(UboRing()); }

        Bool UnpackRingAvailable() {
            if (MG_Config::Features.EsprytDisableUnpackRing) return false;
            return RingAvailable(UnpackRing());
        }

        Bool UnpackRingAllocate(SizeT size, SizeT& outOffset) {
            // A request the ring could never satisfy even empty would otherwise walk
            // the whole grow/drain ladder before failing.
            if (size > kUnpackRingMaxBytes) return false;
            return RingAllocate(UnpackRing(), size, outOffset);
        }

        void* UnpackRingMappedPtr() { return UnpackRing().store.mappedPtr; }
        Uint UnpackRingBufferId() { return UnpackRing().store.id; }
        SizeT UnpackRingMaxBytes() { return kUnpackRingMaxBytes; }

        void UnpackRingOnPresent() { RingOnPresent(UnpackRing()); }

        void UploadRingOnPresent() { RingOnPresent(UploadRing()); }
    } // namespace BufferImpl
} // namespace MobileGL::MG_Backend::DirectGLES
