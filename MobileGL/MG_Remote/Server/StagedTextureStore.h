// MobileGL - MobileGL/MG_Remote/Server/StagedTextureStore.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5c tx - THE SERVER'S OWN COPY OF THE STAGED TEXTURE LEVELS. The texture twin of
// StagedShadow.h (R-11), held to its four rulings (CONTRACT-P5C.md §2.1).
//
// THE DEFECT THIS ENDS (T1/T5). resource_subdata's texture half stages the level bytes into
// SEG_STAGE and ApplyTextureUpload (PipeApply.cpp:989-1036) drops the pointer after the gate,
// the accumulation and the serial. Espryt then re-reads the client's MipmapStorage at sync
// time (Managers.cpp:6940, :7129, :7198, :7315), reads per-level shape off the client object
// (:6928, :6936, :7314, :7332, DirectGLES.cpp:8502-8507, :8528-8529), and Magma WRITES the
// client's level storage outright (VulkanRenderer.cpp:1562-1609: AllocateStorage +
// MarkStorageDirty). In monolith every one of those is correct - the bytes belong to a
// frontend object that outlives the call. Under split the pointer names SEG_STAGE, valid
// only until retiredSeq passes the record, and w1's MOBILEGL_IPC_AUDIT=1 fills retired
// staging with 0xDD precisely so an implementation that kept the pointer is DISTINGUISHABLE
// from one that copied. So this copies, at apply time, into server-owned storage, and the
// sync reads nothing but this store and the descriptor.
//
// WHAT A KEY IS, AND WHY IT IS NOT THE TWIN ADDRESS THE CONTRACT'S FIRST DRAFT SAID.
// CONTRACT-P5C §2.1 rules "keyed by the texture resource twin's address". The buffer half
// keys by twin address because the twin is minted at sub-data time under split
// (Managers.cpp:2139-2142) and GLESBufferResource's constructor is GL-free. The texture
// twin's constructor is NOT: BackendTextureObject() calls glGenTextures (Managers.cpp:5496),
// so minting it at sub-data time would allocate a driver name for every texture that is
// written and never drawn, and would put a GL call into a unit test that has no context
// (R-16's unit case, StagedTextureStoreTest, exercises the REAL ops table headless). The
// wire HANDLE the record carried serves the same purpose the address served - stable for
// the object's life, liveness-exact through the generation, already in every caller's hand
// (rule E: the apply thread resolves every object from a handle the record carried) - so
// KeyForHandle is the primary key. KeyForTwinAddress exists for the one caller that has a
// server-side twin and no handle: Magma's T5 shadow writes key by the TextureResource. The
// two namespaces cannot collide (handle keys carry the top bit; user-space heap addresses
// never do). Every event that ends a key's life has a call site: a named-level respecify
// re-defines the level (NoteLevelDefined), a whole-resource respecify drops every level
// (ResetLevels), resource_destroy drops the key (Ops_H_TextureDestroy), context death drops
// all (OnBackendContextDestroyed, beside MGL_SERVER_STAGED_DROP_ALL).
//
// COVERAGE IS EXACTLY THE STAGED RUNS, NEVER WIDENED AND NEVER NARROWER. One resource_subdata
// stages one RUN of the level shadow - the whole level for a level that fits one record's chunk
// budget (TextureEmit.h: "the bytes this record declares ARE the level shadow", Blob.Size = the
// level's byte count), a whole-width slab of it for one that does not
// (TextureEmit.h's MGPipeForEachTextureSlab) - and what this store covers is the union of the
// runs it has adopted, in the level's own byte coordinates. The record's region set is
// deliberately NOT the coverage unit: it declares which texels CHANGED (the upload planner's
// shape, which the applier's pending set already carries), while the full-level upload paths -
// every conversion fallback, and the immutable/mutable regeneration arms - read the whole level
// including texels no region named. Those texels crossed in the staged run; treating them as
// uncovered would Fatal a legal glTexStorage-then-small-glTexSubImage sequence whose first (and
// only) record names a small box, and moving them into the store is not the buffer half's
// silent-zero case because they are the client's own shadow bytes, delivered and declared.
//
// SO A FULL-LEVEL READ ASKS FOR THE WHOLE LEVEL TO BE COVERED, which is where the texture half
// parts company with the buffer half's one-range shape: the runs of a split level arrive as
// several records of ONE apply batch - the same verb, so all of them land before the barrier
// that lets the sync read - the set is complete exactly when every piece has landed, and a level
// with a hole in it is Fatal{StageSnapshotTooNarrow}: the same words as the buffer half, for the
// same reason, because those bytes have never existed on this side and inventing them is silent
// data loss rather than a missing optimisation.
//
// DEFINED-NESS IS TRACKED, NOT DERIVED ALONE. §1's per-level extent derivation (max(1,
// base >> level), layer axes fixed) computes the extent of a level that EXISTS; it cannot
// say whether the level was ever defined, and the mutable regen arms skip undefined levels
// (a sparse chain stays sparse on the driver). The client does not emit per-level extents,
// but every storage-defining call DOES cross as a respecify, so defined-ness reaches this
// store through the ops table's TextureRespecify hook: a named level marks that (uploadTarget,
// level), an immutable whole-resource respecify (glTexStorage*) marks every level of every
// upload target. Levels the store has never heard of answer extent {0,0,0}, which is the
// exact answer the frontend's GetMipmapTexelSize gives for them - FBO completeness over a
// null-data level and the sparse-chain skip both reproduce the monolith arm.
//
// GPU-GENERATED LEVELS (T5) ARE DEFINED HERE WITH NO BYTES AND A DIRTY MARK. A GPU-side
// generation (Magma's GenerateMipmap) dirties the SERVER's shadow, not the client's: the
// level is NoteLevelDefined + MarkLevelGpuDirty(true), it holds no bytes (they were made on
// the GPU and never crossed), and the mark is the "dirty region, level has NO pending
// upload" answer of §2.2's table. A texel read of such a level is the
// Fatal{StageSnapshotTooNarrow} case above, which is correct: the two readers that could
// ask are both named refusals under split (§2.3).
//
// WHY IT IS A HEADER AND NOT A BLOCK INSIDE Managers.cpp - StagedShadow.h's reason, and it
// is the one that matters here too: a block inside Managers.cpp could only ever be exercised
// by a test that also has a GL context, a resource twin and a live session, which is exactly
// how a rule ends up with no check that can fail for its own reason (R-16). Here `copies` is
// a constructor parameter rather than a read of MG_Config::Transport, so a unit case builds
// one store of each kind and asserts the DIFFERENCE between them; the production wiring is
// asserted separately, through the real ops table, in StagedTextureStoreTest.

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipeTypes.h>
// StagedShadowStore::CoverageAdd / CoverageHas: ONE spelling of the covered set, so the texture
// half's runs and the buffer half's ranges cannot drift apart on what "covered" means.
#include <MG_Remote/Server/StagedShadow.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Math/VectorTypes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

#if MOBILEGL_BUILD_DISAGGREGATED
#include <Config.h>
#endif

namespace MobileGL::MG_Remote::Server {

    // §1's server-side per-level extent (CONTRACT-P5C table 0): max(1, base_extent >> level)
    // per SHRINKING axis, with an array texture's layer count fixed - it is not a dimension of
    // the image (GL 4.6 core 8.14.3), and GetMipmapTexelSize parks it in the slot after the
    // image's own dimensions. Texture1DArray shrinks x only; Texture2DArray and
    // TextureCubeMapArray shrink x and y; every other target shrinks all three. This is the
    // mip chain's definition (MG_State's IsMipmapCompleteForFilter applies the same split,
    // TextureObject.cpp:705-734), so two honest ends compute the same number.
    inline Int StagedTextureShrinkingAxisCount(Uint8 pipeResourceTarget) {
        switch (static_cast<MG_Pipe::MGPipeResourceTarget>(pipeResourceTarget)) {
        case MG_Pipe::MGPipeResourceTarget::Tex1DArray:
            return 1;
        case MG_Pipe::MGPipeResourceTarget::Tex2DArray:
        case MG_Pipe::MGPipeResourceTarget::TexCubeArray:
            return 2;
        default:
            return 3;
        }
    }

    inline IntVec3 StagedTextureMipExtent(Uint8 pipeResourceTarget, Uint32 baseWidth, Uint32 baseHeight,
                                          Uint32 baseDepth, Uint32 level) {
        const Int shrinking = StagedTextureShrinkingAxisCount(pipeResourceTarget);
        const Int shift = static_cast<Int>(level);
        IntVec3 extent{static_cast<Int>(baseWidth), static_cast<Int>(baseHeight), static_cast<Int>(baseDepth)};
        for (Int axis = 0; axis < shrinking && axis < 3; ++axis) {
            extent[axis] = std::max<Int>(extent[axis] >> shift, 1);
        }
        return extent;
    }

    inline IntVec3 StagedTextureUploadExtent(const MG_Pipe::MGPResourceDesc& desc,
                                             const MG_Pipe::MGPSubData& upload) {
        if (upload.LevelWidth && upload.LevelHeight && upload.LevelDepth)
            return {static_cast<Int>(upload.LevelWidth), static_cast<Int>(upload.LevelHeight),
                    static_cast<Int>(upload.LevelDepth)};
        return StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, upload.Level);
    }

    // WHERE A RECORD'S STAGED RUN BEGINS IN THE SERVER-OWNED LEVEL IMAGE. The record itself says
    // so, and the two shapes that reach AdoptRun are told apart by the run's own LENGTH - the one
    // field of MGPSubData that differs between them:
    //
    //   * THE WHOLE-LEVEL RUN, which is what resource_subdata's texture half has always staged:
    //     TextureEmit.h's "the bytes this record declares ARE the level shadow", so Blob.Size is
    //     the level's byte count and the run begins at the level's FIRST byte whatever the record's
    //     box says - the box describes the dirty texels, not the run. Answer: 0. That is also the
    //     answer for the whole-level spelling that carries no regions at all (RegionCount == 0,
    //     strides 0 = tightly packed).
    //
    //   * A SLAB of a level too large to stage whole (TextureEmit.h's MGPipeForEachTextureSlab),
    //     whose run is exactly the byte extent of its own box and therefore begins at that box's
    //     first byte: z0 * sliceStride + y0 * rowStride + x0 * bpp, every one of them carried in
    //     the record's own regions. So the box names the placement, and the far side still needs no
    //     knowledge of the level's format: the texel's byte size falls out of the carried row
    //     stride and the carried level width - the two numbers the region and the record both
    //     state.
    //
    // A run SHORTER than its box's byte extent is neither shape and answers 0 - not silently
    // placed: no emitter produces one, and the whole-level reading is the only one that cannot
    // lose texels the record did carry.
    inline Uint64 StagedTextureRunImageOffset(const MG_Pipe::MGPSubData& upload,
                                             const MG_Pipe::MGPSubRegion* regions) {
        const MG_Pipe::MGPBox& box = upload.UnionBox;
        if (regions == nullptr || upload.RegionCount == 0) return 0;
        const Uint64 rowStride = regions[0].SrcRowStride;
        const Uint64 sliceStride = regions[0].SrcSliceStride;
        if (rowStride == 0 || sliceStride == 0 || upload.LevelWidth == 0) return 0;
        const Uint64 texelBytes = rowStride / upload.LevelWidth;
        if (texelBytes == 0 || box.W == 0 || box.H == 0 || box.D == 0) return 0;
        const Uint64 boxBytes = static_cast<Uint64>(box.D - 1) * sliceStride +
                                static_cast<Uint64>(box.H - 1) * rowStride +
                                static_cast<Uint64>(box.W) * texelBytes;
        if (upload.Blob.Size != boxBytes) return 0;
        return static_cast<Uint64>(box.Z) * sliceStride + static_cast<Uint64>(box.Y) * rowStride +
               static_cast<Uint64>(box.X) * texelBytes;
    }

    class StagedTextureStore {
    public:
        // `copies` is "this process is really split". False reproduces the monolith expression
        // character for character: the client shadow answers every question (the sync's macros
        // never consult this store when CopiesIntoServerStorage() is false), nothing is
        // allocated, and every push and verify lane stays byte-identical to what it was
        // before tx.
        explicit StagedTextureStore(Bool copies) : m_copies(copies) {}

        Bool CopiesIntoServerStorage() const { return m_copies; }

        // The wire handle the record carried, tagged so it can never alias a twin address.
        // Slot and Gen are the client allocator's identity for one live object, so a recycled
        // slot's new owner keys a different entry than its predecessor's stale one.
        static Uint64 KeyForHandle(MG_Pipe::MGPipeHandle handle) {
            return (Uint64{1} << 63) | (static_cast<Uint64>(handle.Slot) << 32) |
                   static_cast<Uint64>(handle.Gen);
        }
        // For the caller whose server-side twin has no wire handle (Magma's TextureResource,
        // T5). Node-stable by that table's own ruling (std::unordered_map nodes).
        static Uint64 KeyForTwinAddress(const void* twin) {
            return static_cast<Uint64>(reinterpret_cast<std::uintptr_t>(twin));
        }

        // THE ADOPTION OF ONE RUN OF A LEVEL. `imageOffset` is where the run's first byte belongs
        // in the server-owned level image and `byteSize` (MGPSubData::Blob.Size under split) is
        // its length; the covered set grows by exactly [imageOffset, imageOffset + byteSize), so
        // coverage stays EXACTLY the staged runs and a hole between them is what RequireLevelBytes
        // refuses on. The caller gets the offset from the record itself
        // (StagedTextureRunImageOffset below). Runs may arrive in any order: coverage is a set,
        // and the level's extent comes from the record either way.
        //
        // THE RETURNED BASE IS ONLY VALID UNTIL THE NEXT RUN OF THE SAME LEVEL: growing the image
        // reallocates it, which is StagedShadowStore::Adopt's note one level down. Nothing needs
        // the base while a level is still being assembled - the readers run at the barrier, after
        // the last piece - so callers here ignore it.
        const Uint8* AdoptRun(Uint64 key, Uint16 uploadTarget, Uint16 level, const IntVec3& extent,
                              Uint64 imageOffset, const void* bytes, SizeT byteSize) {
            if (!m_copies) return nullptr;
            const std::lock_guard<std::mutex> lock(m_mutex);
            LevelShadow& shadow = m_shadows[key].Levels[PackLevel(uploadTarget, level)];
            shadow.Extent = extent;
            shadow.Defined = true;
            shadow.GpuDirty = false;
            return CopyRunInto(shadow, imageOffset, bytes, byteSize);
        }

        // THE WHOLE-LEVEL SPELLING: the run is the level's COMPLETE current content, beginning at
        // the level's first byte, so nothing of a previous run survives it - which is what makes a
        // re-adoption the right answer for the image-promotion readback and for every
        // monolith-shaped caller. AdoptRun(offset = 0) with the entry replaced rather than grown
        // into; under monolith both do nothing and return nullptr, and the caller
        // (Ops_H_TextureSubData) has already returned before reaching either.
        const Uint8* Adopt(Uint64 key, Uint16 uploadTarget, Uint16 level, const IntVec3& extent,
                           const void* bytes, SizeT byteSize) {
            if (!m_copies) return nullptr;
            const std::lock_guard<std::mutex> lock(m_mutex);
            LevelShadow& shadow = m_shadows[key].Levels[PackLevel(uploadTarget, level)];
            shadow.Extent = extent;
            shadow.Defined = true;
            shadow.GpuDirty = false;
            shadow.Bytes.clear();
            shadow.Covered.clear();
            return CopyRunInto(shadow, 0, bytes, byteSize);
        }

        // A storage-defining respecify named this level: it EXISTS from here on, at this
        // extent (null-data glTexImage*D, a generated level). An extent move redefines the
        // level's coordinate system, so the bytes and the dirty mark of the old one go with
        // it; an extent-restating one keeps them, which is the same answer the driver gives
        // (a same-shape redefinition that carries no new upload leaves the old texels in
        // place - undefined content is allowed to be the old content).
        void NoteLevelDefined(Uint64 key, Uint16 uploadTarget, Uint16 level, const IntVec3& extent) {
            if (!m_copies) return;
            const std::lock_guard<std::mutex> lock(m_mutex);
            LevelShadow& shadow = m_shadows[key].Levels[PackLevel(uploadTarget, level)];
            if (!shadow.Defined || shadow.Extent != extent) {
                shadow.Bytes.clear();
                // The covered set is a statement about THIS coordinate system, so it goes with
                // the bytes it described.
                shadow.Covered.clear();
                shadow.GpuDirty = false;
            }
            shadow.Extent = extent;
            shadow.Defined = true;
            m_any.store(true, std::memory_order_release);
        }

        // A whole-resource redefinition (glTexStorage*, a texture view): every level's old
        // coordinate system is gone, so every level entry goes. The caller marks the new
        // chain defined level by level afterwards where the call defines one.
        void ResetLevels(Uint64 key) {
            if (!m_any.load(std::memory_order_acquire)) return;
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_shadows.erase(key);
        }

        // T5's dirty mark: a GPU-side generation made this level's texels newer than any
        // shadow. The level typically holds NO bytes - they were generated on the GPU and
        // never crossed - and the mark is what answers "dirty region, level has no pending
        // upload" without asking the client (§2.2's last row).
        void MarkLevelGpuDirty(Uint64 key, Uint16 uploadTarget, Uint16 level, Bool dirty) {
            if (!m_copies) return;
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_shadows[key].Levels[PackLevel(uploadTarget, level)].GpuDirty = dirty;
            m_any.store(true, std::memory_order_release);
        }

        void Drop(Uint64 key) {
            if (!m_any.load(std::memory_order_acquire)) return;
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_shadows.erase(key);
        }

        void DropAll() {
            if (!m_any.load(std::memory_order_acquire)) return;
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_shadows.clear();
        }

        // Fatal when a sync wants texels this store has no covered run for. This is THE data
        // -correctness refusal of the texture half: a pending upload with no adoption behind
        // it, a GPU-generated level (no bytes by construction), a level the client never
        // defined, a level whose pieces did not all arrive, and a monolith-arm misuse all land
        // here, because in every one of them the bytes have never existed on this side and
        // re-reading the client's shadow for them is the cross-role access tx exists to end.
        //
        // THE QUESTION IS "IS THE WHOLE LEVEL COVERED", not "are there any bytes": a split
        // level's pieces arrive as separate records of the same apply batch, and this caller
        // reads the level image WHOLE (LevelByteSize / the sync's texel base), so a level with
        // one piece missing is as unreadable as one with none.
        const Uint8* RequireLevelBytes(Uint64 key, Uint16 uploadTarget, Uint16 level, const char* site) const {
            if (!m_copies) return nullptr;
            const std::lock_guard<std::mutex> lock(m_mutex);
            const LevelShadow* shadow = FindLevel(key, uploadTarget, level);
            if (shadow != nullptr && shadow->Defined && !shadow->Bytes.empty() &&
                StagedShadowStore::CoverageHas(shadow->Covered, 0, shadow->Bytes.size())) {
                return shadow->Bytes.data();
            }
            MGLOG_F("MGPipe: Fatal{StageSnapshotTooNarrow, \"%s\"} - the texture sync wants the "
                    "bytes of (uploadTarget=%u, level=%u) and the server's staged shadow has no "
                    "COMPLETE covered run for it. Under split the authoritative shadow is "
                    "SERVER-OWNED (rule C) and resource_subdata is the only way bytes reach it, "
                    "so these texels have never existed on this side: the record that should "
                    "have carried them is missing, a piece of a level cut into stage-chunk runs "
                    "has not arrived, or the level's texels were generated on the GPU and no "
                    "byte answer exists at all. Re-reading the client's shadow would be the "
                    "cross-role access this store exists to end",
                    site, static_cast<Uint32>(uploadTarget), static_cast<Uint32>(level));
            std::abort();
        }

        // Diagnostics the sync path and the unit cases read, so that a check can assert WHAT
        // HAPPENED rather than that nothing blew up.
        Bool IsCovered(Uint64 key, Uint16 uploadTarget, Uint16 level) const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const LevelShadow* shadow = FindLevel(key, uploadTarget, level);
            if (shadow == nullptr || !shadow->Defined || shadow->Bytes.empty()) return false;
            // THE WHOLE LEVEL, not merely "some bytes": a level assembled out of stage-chunk
            // pieces is covered only once every piece has landed, and both readers of this
            // answer then read the image whole - the image-promotion merge and Magma's
            // unbacked-level readback.
            return StagedShadowStore::CoverageHas(shadow->Covered, 0, shadow->Bytes.size());
        }
        // How many runs this level's coverage is made of: 1 for a whole-level adoption, one per
        // piece for a level cut into stage-chunk slabs. Diagnostics for the unit cases, so a
        // check can assert WHAT happened rather than that nothing blew up.
        SizeT LevelCoveredRunCount(Uint64 key, Uint16 uploadTarget, Uint16 level) const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const LevelShadow* shadow = FindLevel(key, uploadTarget, level);
            return shadow == nullptr ? 0 : shadow->Covered.size();
        }
        Bool IsLevelDefined(Uint64 key, Uint16 uploadTarget, Uint16 level) const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const LevelShadow* shadow = FindLevel(key, uploadTarget, level);
            return shadow != nullptr && shadow->Defined;
        }
        Bool IsLevelGpuDirty(Uint64 key, Uint16 uploadTarget, Uint16 level) const {
            if (!m_any.load(std::memory_order_acquire)) return false;
            const std::lock_guard<std::mutex> lock(m_mutex);
            const LevelShadow* shadow = FindLevel(key, uploadTarget, level);
            return shadow != nullptr && shadow->GpuDirty;
        }
        // {0,0,0} for a level this store has never heard of - the exact answer the frontend's
        // GetMipmapTexelSize gives for an undefined level, which is what keeps the regen
        // arms' sparse-chain skip intact.
        IntVec3 LevelExtentOrUndefined(Uint64 key, Uint16 uploadTarget, Uint16 level) const {
            if (!m_any.load(std::memory_order_acquire)) return IntVec3{0, 0, 0};
            const std::lock_guard<std::mutex> lock(m_mutex);
            const LevelShadow* shadow = FindLevel(key, uploadTarget, level);
            if (shadow == nullptr || !shadow->Defined) return IntVec3{0, 0, 0};
            return shadow->Extent;
        }
        SizeT LevelByteSize(Uint64 key, Uint16 uploadTarget, Uint16 level) const {
            if (!m_any.load(std::memory_order_acquire)) return 0;
            const std::lock_guard<std::mutex> lock(m_mutex);
            const LevelShadow* shadow = FindLevel(key, uploadTarget, level);
            if (shadow == nullptr || !shadow->Defined) return 0;
            return shadow->Bytes.size();
        }
        Bool HasShadow(Uint64 key) const {
            if (!m_any.load(std::memory_order_acquire)) return false;
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_shadows.find(key) != m_shadows.end();
        }
        SizeT TrackedResources() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_shadows.size();
        }
        SizeT TrackedLevelCount(Uint64 key) const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_shadows.find(key);
            return it == m_shadows.end() ? 0 : it->second.Levels.size();
        }

        static Uint32 PackLevel(Uint16 uploadTarget, Uint16 level) {
            return (static_cast<Uint32>(uploadTarget) << 16) | static_cast<Uint32>(level);
        }

        struct LevelShadow {
            IntVec3 Extent{0, 0, 0};
            // The level's whole staged run. EMPTY for a defined-but-byteless level (null-data
            // definition, GPU-generated) - emptiness is the coverage answer, not an error. For a
            // level adopted in pieces it is the assembled image, grown to the high-water mark of
            // the runs.
            Vector<Uint8> Bytes;
            // Sorted, disjoint, in the level image's byte coordinates, and EXACTLY the runs
            // adopted for this level: a gap in it is a piece that never arrived, which is the
            // Fatal below (StagedShadowStore's coverage rules, same helpers).
            Vector<Range1D> Covered;
            Bool Defined = false;
            Bool GpuDirty = false;
        };
        struct TextureShadow {
            ska::flat_hash_map<Uint32, LevelShadow> Levels;
        };

        // The copy both adoption spellings share, with the lock already held: grow the image to
        // hold the run, place it, and widen the covered set by exactly its range. An empty run
        // (a level defined with no bytes, or a null pointer) covers nothing - emptiness is the
        // coverage answer for such a level, not an error.
        const Uint8* CopyRunInto(LevelShadow& shadow, Uint64 imageOffset, const void* bytes,
                                 SizeT byteSize) {
            if (bytes != nullptr && byteSize != 0) {
                const auto* raw = static_cast<const Uint8*>(bytes);
                const SizeT needed = static_cast<SizeT>(imageOffset) + byteSize;
                if (shadow.Bytes.size() < needed) shadow.Bytes.resize(needed, 0);
                std::memcpy(shadow.Bytes.data() + imageOffset, raw, byteSize);
                StagedShadowStore::CoverageAdd(shadow.Covered, static_cast<SizeT>(imageOffset),
                                               needed);
            }
            m_any.store(true, std::memory_order_release);
            return shadow.Bytes.empty() ? nullptr : shadow.Bytes.data();
        }

        const LevelShadow* FindLevel(Uint64 key, Uint16 uploadTarget, Uint16 level) const {
            const auto textureIt = m_shadows.find(key);
            if (textureIt == m_shadows.end()) return nullptr;
            const auto levelIt = textureIt->second.Levels.find(PackLevel(uploadTarget, level));
            return levelIt == textureIt->second.Levels.end() ? nullptr : &levelIt->second;
        }

        const Bool m_copies;
        mutable std::mutex m_mutex;
        ska::flat_hash_map<Uint64, TextureShadow> m_shadows;
        // Read on every IsLevelGpuDirty / LevelExtentOrUndefined, so the monolith cost is one
        // acquire load of a never-written flag rather than a mutex and two hash lookups.
        std::atomic<Bool> m_any{false};
    };

#if MOBILEGL_BUILD_DISAGGREGATED
    // ONE PER PROCESS, and its copying arm is decided ONCE at first use - StagedShadow's
    // ServerStaged() ruling verbatim: the two arms hold the authoritative bytes in DIFFERENT
    // places, so an answer that changed mid-run would strand every level already staged.
    // Leaked at exit like every other MG_Remote singleton (ID-8). In a header rather than in
    // Managers.cpp because TWO backends consume it: Espryt's adoption and sync
    // (Managers.cpp) and Magma's T5 shadow writes (VulkanRenderer.cpp) must name the same
    // store, and an inline function's one static gives them exactly that.
    inline StagedTextureStore& ServerStagedTexture() {
        static StagedTextureStore& store =
            *new StagedTextureStore(MG_Config::Transport != MG_Config::TransportMode::Monolith);
        return store;
    }
#endif

} // namespace MobileGL::MG_Remote::Server
