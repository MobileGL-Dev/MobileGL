// MobileGL - MobileGL/MG_Backend/DirectGLES/Managers.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <atomic>
#include <mutex>
#include "DirectGLES.h"
#include "MG_State/GLState/SamplerState/SamplerObject.h"
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>

namespace MobileGL::MG_Backend::DirectGLES {
    String EmulateBaseInstanceInVertexShader(String source, GLenum shaderType);
    String PromoteDrawParameterGlobalsToUniforms(String source, GLenum shaderType);

    template <typename StateObject, typename BackendObject>
    class StateBackendObjectRegistry {
    public:
        using StatePtr = SharedPtr<StateObject>;
        using StateWeakPtr = std::weak_ptr<StateObject>;
        using BackendPtr = SharedPtr<BackendObject>;

        // The backend twin and the weak reference that decides whether the raw key still
        // names the state object the twin was built for. Both live in one entry: a
        // separate liveness map answered nothing the backend probe had not already found
        // and cost a second hash lookup on every Find, which the draw path runs ~10 times.
        struct Entry {
            BackendPtr backend;
            StateWeakPtr stateRef;
        };
        using BackendMap = UnorderedMap<StateObject*, Entry>;
        using iterator = typename BackendMap::iterator;
        using const_iterator = typename BackendMap::const_iterator;

        BackendPtr& GetOrCreate(const StatePtr& stateObj) {
            MOBILEGL_ASSERT(stateObj != nullptr, "State object must not be null");

            auto& entry = m_entries[stateObj.get()];
            if (entry.stateRef.expired()) {
                // The previous owner of this address is gone and the allocator handed it
                // to a new object: its twin describes ids the new state object never made.
                entry.backend.reset();
            }
            entry.stateRef = stateObj;
            return entry.backend;
        }

        // Null when no live state object owns this key. The result points into the map, so
        // it stays valid only until the next GetOrCreate/Find/CollectGarbage on this registry.
        BackendPtr* Find(StateObject* stateObj) {
            const auto entryIt = m_entries.find(stateObj);
            if (entryIt == m_entries.end()) {
                return nullptr;
            }
            if (entryIt->second.stateRef.expired()) {
                m_entries.erase(entryIt);
                return nullptr;
            }
            return &entryIt->second.backend;
        }

        const BackendPtr* Find(StateObject* stateObj) const {
            return const_cast<StateBackendObjectRegistry*>(this)->Find(stateObj);
        }

        iterator begin() { return m_entries.begin(); }
        const_iterator begin() const { return m_entries.begin(); }
        iterator end() { return m_entries.end(); }
        const_iterator end() const { return m_entries.end(); }

        void CollectGarbageIfNeeded() {
            ++m_gcTick;
            if (m_gcTick < kGCInterval) {
                return;
            }
            CollectGarbage();
            m_gcTick = 0;
        }

        void CollectGarbageNow() { CollectGarbage(); }

    private:
        void CollectGarbage() {
            if (m_isCollecting) {
                return;
            }

            m_isCollecting = true;

            Vector<StateObject*> staleKeys;
            staleKeys.reserve(m_entries.size());
            for (const auto& [stateKey, entry] : m_entries) {
                if (entry.stateRef.expired()) {
                    staleKeys.push_back(stateKey);
                }
            }

            for (auto* stateKey : staleKeys) {
                m_entries.erase(stateKey);
            }

            m_isCollecting = false;
        }

    private:
        static constexpr Uint32 kGCInterval = 1024;
        BackendMap m_entries;
        Uint32 m_gcTick = 0;
        Bool m_isCollecting = false;
    };

    namespace BufferImpl {
        const GLenum TempBufferTarget = GL_ARRAY_BUFFER;

        // The DirectGLES storage behind one frontend buffer. Owned (refcounted) by
        // the frontend BufferObject; immediate BufferBackendOps keep it current, so
        // draw-time "sync" reduces to ensuring the storage exists.
        class GLESBufferResource : public MG_State::GLState::BackendBufferResource {
        public:
            ~GLESBufferResource() override = default;

            Uint id = 0;
            SizeT storageSize = 0;
            Bool storageInitialized = false;
            // ES context generation this resource's id belongs to; ids from a
            // destroyed context are invalid and must not be deleted or reused.
            Uint contextGeneration = 0;
            // Frontend change serial the backend storage reflects. When immediate
            // ops cannot run (ops unregistered, no current context), this lags and
            // EnsureBufferResource falls back to a full re-upload. Atomic: read on
            // the context-owning thread while ops on other threads may update it.
            std::atomic<Uint64> syncedChangeSerial{0};
            // Ops that arrived while no ES context was current on the calling thread
            // (or before storage existed); replayed by EnsureBufferResource. The ES
            // context migrates between app threads, so deferring ops can race with
            // the owning thread replaying them: guard both fields with pendingMutex.
            Bool pendingRespecify = false;
            VecRange1D pendingRanges;
            std::mutex pendingMutex;
            // Zero-copy coherent persistent map (EXT_buffer_storage): the GL store is
            // immutable, persistently+coherently mapped, and persistentPtr is what the app
            // (and the frontend PipeResource) write into directly. While set, draw-time
            // sync is a no-op and no per-draw glBufferSubData is issued. Cleared on ES
            // context loss.
            Bool persistentMapped = false;
            void* persistentPtr = nullptr;
        };

        // Registered as the frontend's BufferBackendOps at backend init and on
        // every MakeCurrent (the ES context can be destroyed and recreated, e.g.
        // by the trace replayer's probe context).
        void RegisterBufferBackendOps();
        void UnregisterBufferBackendOps();
        // The ES context died: unregister ops, invalidate all outstanding GL ids
        // (they belonged to the dead context) and drop deferred deletes.
        void OnBackendContextDestroyed();

        // Get-or-create the backend resource and bring its storage up to date
        // (creates the GL buffer, replays pending ops, pushes persistent-mapped
        // ranges). Requires the ES context to be current. Returns nullptr only
        // for null input.
        GLESBufferResource* EnsureBufferResource(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject);
        // Existing resource or nullptr; performs no GL calls.
        GLESBufferResource* GetBufferResource(MG_State::GLState::BufferObject* bufferObject);
        // True when EnsureBufferResource(frontend) would provably fall straight through
        // every branch and do no work — i.e. `resource` is still the frontend's own
        // resource, its id belongs to the live ES context, and either it is the
        // zero-copy coherent persistent store (draw-time sync is a no-op by design) or
        // the storage is initialized at the right size with no pending ops and a synced
        // change serial while the buffer is not mapped (an active map may owe a
        // per-draw persistent-range push, so it always takes the full path).
        // `frontend` must be non-null and alive; the caller guarantees that by holding
        // (or shadowing something that holds) a SharedPtr to it. Enables the per-VAO
        // resolved-buffers memo to skip EnsureBufferResource on clean static buffers.
        Bool IsBufferDrawClean(const MG_State::GLState::BufferObject* frontend, const GLESBufferResource* resource);

        // Deletes GL buffers whose owning frontend objects died (possibly on a
        // thread without a current ES context). Called from draw-time sync.
        void ProcessDeferredBufferReleases();

        // glBindBuffer with a redundant-bind cache for GL_ARRAY_BUFFER.
        void BindBufferId(GLenum target, Uint id);
        void InvalidateArrayBufferBindingCache();
        // Redundant-bind caches for the driver-level GL_PIXEL_PACK/UNPACK_BUFFER
        // bindings. Every backend readback (glReadPixels / pack-PBO map) and pixel
        // upload site routes its binding through these so the shadow always matches
        // the driver; the resting state between operations is 0, which keeps any
        // path that implicitly assumes "no PBO bound" correct. Scrubbed when a
        // buffer id is deleted/pooled (GL resets a deleted buffer's bindings to 0,
        // and a recycled name matching the shadow would false-skip the rebind) and
        // invalidated on MakeCurrent (context may reset).
        void BindPixelPackBufferId(Uint id);
        void BindPixelUnpackBufferId(Uint id);
        void InvalidatePixelBufferBindingCaches();
        // A GL buffer id is being deleted by code outside BufferImpl (e.g. the VAO
        // client-attribute staging buffers): scrub every buffer-binding shadow that
        // could false-skip when the name is recycled.
        void NoteBufferIdDeleted(Uint id);
        // Redundant-bind cache for INDEXED buffer bindings (glBindBufferBase/Range on
        // GL_UNIFORM_BUFFER / GL_SHADER_STORAGE_BUFFER): skips the GL call when the
        // (id, range) already at that index matches, like the array-buffer/texture/
        // sampler caches already do. Invalidated on MakeCurrent (context may reset).
        void BindBufferBaseCached(GLenum glTarget, Uint index, Uint id);
        void BindBufferRangeCached(GLenum glTarget, Uint index, Uint id, GLintptr offset, GLsizeiptr size);
        void InvalidateIndexedBufferBindingCache();
        // Buffer-storage pool maintenance. TrimBufferPool evicts over-budget entries
        // (called once per frame from Present); ClearBufferPool drops all pooled ids
        // without glDeleteBuffers (called when the ES context is going away).
        void TrimBufferPool();
        void ClearBufferPool();

        // --- Global-UBO ring ------------------------------------------------------
        // One persistently+coherently mapped buffer (EXT_buffer_storage) shared by
        // every program's lowered default-uniform block. Each content change is
        // bump-allocated into a fresh slot and bound with glBindBufferRange, so the
        // CPU never rewrites bytes the GPU may still be reading — the per-draw
        // glBufferSubData into one static UBO forced Adreno to resolve that
        // write-after-read hazard on every uniform-dirtying draw (MC dirties
        // uniforms every draw). Reclamation rides the Present() frame-fence
        // watermark; no ring bytes are recycled before their frame's GPU work
        // completed.
        //
        // A program's cached slot, reusable within one frame while the frontend UBO
        // content version is unchanged. Cross-frame reuse is intentionally not
        // attempted: later same-frame allocations may recycle bytes of completed
        // frames, so re-referencing them would need per-bind pinning — rewriting
        // GetUBOSize() bytes once per program per frame is far cheaper.
        struct UboRingAllocation {
            Uint32 contentVersion = ~0u; // frontend UBO content version held at `offset`
            Uint32 ringGeneration = 0;   // ring identity the slot lives in (0 = never valid)
            Uint64 frameSerial = ~Uint64{0}; // frame the slot was written in
            SizeT offset = 0;
        };
        // False when the feature is disabled, EXT_buffer_storage / fences are
        // missing, the ES context is not current, or ring creation already failed
        // under this context (callers then take the legacy glBufferSubData path).
        Bool UboRingAvailable();
        // Bump-allocate `size` bytes aligned to GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT.
        // Grows the ring (new GL store, generation bump) when the in-flight span
        // would be overrun. Returns false when storage (re)creation fails.
        Bool UboRingAllocate(SizeT size, SizeT& outOffset);
        void* UboRingMappedPtr();
        Uint UboRingBufferId();
        Uint32 UboRingGeneration();
        // Present()-time upkeep: records the frame's high-water mark for reclamation
        // and deletes grown-away ring stores once the GPU is done with them.
        void UboRingOnPresent();
    } // namespace BufferImpl

    namespace VertexArrayImpl {
        class BackendVertexArrayObject {
        public:
            BackendVertexArrayObject();
            ~BackendVertexArrayObject();
            void SyncToBackend(const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject);
            void SyncClientSideAttributesForDrawArrays(
                const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject, GLint first, GLsizei count);
            Uint GetBackendVertexArrayId() const { return m_backendVAOId; }
            void Bind() const;

            // Draw-path memo of SyncNeccessaryBuffers' attribute walk for this VAO: the
            // distinct enabled-attribute buffers (deduped) and the index buffer, resolved
            // to their backend resources once. Valid while the VAO's config version is
            // unchanged — every attach/enable/disable/format mutation bumps it (the same
            // invariant SyncToBackend's gate already leans on), and the VAO's attribute
            // SharedPtrs pin each memoed frontend buffer for exactly that long, so the raw
            // pointers cannot dangle on a hit. Per-buffer cleanliness is NOT memoed here:
            // each hit re-checks IsBufferDrawClean (resource identity, context generation,
            // pending ops, change serial) and falls back to EnsureBufferResource for just
            // the dirty entries via their attribute index. The IBO entry is keyed on the
            // slot's bound-object identity instead (its slot version is a wrapping Uint16
            // and is not covered by the config version).
            struct ResolvedDrawBuffers {
                struct Entry {
                    MG_State::GLState::BufferObject* frontend = nullptr;
                    BufferImpl::GLESBufferResource* resource = nullptr;
                    Uint8 attribIndex = 0;
                };
                Bool valid = false;
                Uint32 configVersion = 0;
                Uint count = 0;
                Array<Entry, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS> entries;
                MG_State::GLState::BufferObject* iboFrontend = nullptr;
                BufferImpl::GLESBufferResource* iboResource = nullptr;
            };
            ResolvedDrawBuffers& GetResolvedDrawBuffersMemo() { return m_resolvedDrawBuffers; }

            // Memo for SyncCurrentVertexAttributeValues: which of a program's ACTIVE
            // attribute locations lack an enabled array in this VAO (those read the
            // context's current generic value instead of a buffer). Keyed on the VAO
            // config version (enable/disable bumps it) and the program's active-location
            // mask. Hosted per twin — the former function-static single entry missed on
            // every draw once the app cycled VAOs, re-reading the cold attribute slots.
            struct PendingAttribValueMask {
                Bool valid = false;
                Uint32 configVersion = 0;
                Uint32 activeMask = 0;
                Uint32 pendingMask = 0;
            };
            PendingAttribValueMask& GetPendingAttribValueMaskMemo() { return m_pendingAttribValueMask; }

        private:
            ResolvedDrawBuffers m_resolvedDrawBuffers;
            PendingAttribValueMask m_pendingAttribValueMask;
            Uint m_backendVAOId = 0;
            Array<Uint, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS> m_clientAttributeBufferIds;
            Bool m_isInitialized = false;
            Uint16 m_syncedIndexBufferVersion = 0;
            // Aggregate gate over the per-attribute walk below: the frontend bumps its config
            // version on every per-attribute version bump (the three Bump*Version functions are
            // its only writers), so an unchanged config version proves every per-attribute
            // compare in SyncToBackend would come up clean. The index-buffer slot has its own
            // version and is NOT covered. The Bool (not a sentinel value) marks "never synced".
            Bool m_hasSyncedConfigVersion = false;
            Uint32 m_syncedConfigVersion = 0;
            Array<MG_State::GLState::VertexAttributeVersion, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS>
                m_syncedAttributeVersions;
        };

        extern StateBackendObjectRegistry<MG_State::GLState::VertexArrayObject, BackendVertexArrayObject>
            g_backendVertexArrayObjects;

        // Shadowed glBindVertexArray: every backend VAO bind goes through here so a
        // draw's second bind of the same VAO (SyncToBackend, then PrepareForDraw's
        // re-bind) reaches the driver once. Invalidate whenever the ES context is
        // replaced - ids restart and the resting binding is 0 again.
        void BindBackendVAOId(Uint id);
        void InvalidateVAOBindingCache();
        // ES resets the binding to 0 when the currently bound VAO is deleted.
        void NoteVAOIdDeleted(Uint id);
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        inline Bool IsSupportedTextureTarget(TextureTarget target) {
            // Every desktop-only target is stored on an ES one; see MapToBackendTextureTarget.
            (void)target;
            return true;
        }

        // ES has none of the desktop-only targets: 1D textures are stored as 2D (height 1), 1D
        // arrays as 2D arrays (height 1, layers in depth), and rectangle textures as plain 2D -
        // they are single-level and already clamp, so only the non-normalized coordinates differ.
        // Must match the shader-side emulation: SPIRV-Cross handles 1D/1D-array itself, and
        // ShaderCompiler::LowerRectImages rewrites rectangle images (declining any module
        // whose lookups are not integer-coordinate, which SPIRV-Cross then still rejects).
        inline TextureTarget MapToBackendTextureTarget(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1D:
            case TextureTarget::TextureRectangle:
                return TextureTarget::Texture2D;
            case TextureTarget::Texture1DArray:
                return TextureTarget::Texture2DArray;
            default:
                return target;
            }
        }

        inline GLenum ConvertTextureTargetToBackendGLEnum(TextureTarget target) {
            return MG_Util::ConvertTextureTargetToGLEnum(MapToBackendTextureTarget(target));
        }

        inline GLenum ConvertTextureUploadTargetToBackendGLEnum(TextureUploadTarget uploadTarget) {
            switch (uploadTarget) {
            case TextureUploadTarget::Texture1D:
            case TextureUploadTarget::TextureRectangle:
                return GL_TEXTURE_2D;
            case TextureUploadTarget::Texture1DArray:
                return GL_TEXTURE_2D_ARRAY;
            default:
                return MG_Util::ConvertTextureUploadTargetToGLEnum(uploadTarget);
            }
        }

        // 1D arrays store layers in the state-side height; the ES 2D-array image keeps height 1 and
        // moves the layer count into depth.
        inline IntVec3 GetBackendUploadSize(TextureTarget stateTarget, const IntVec3& texelSize) {
            if (stateTarget == TextureTarget::Texture1DArray) {
                return {texelSize.x(), 1, texelSize.y()};
            }
            return texelSize;
        }

        inline Bool IsMultisampleTextureTarget(TextureTarget target) {
            return target == TextureTarget::Texture2DMultisample ||
                   target == TextureTarget::Texture2DMultisampleArray;
        }

        inline Bool SupportsWrapR(TextureTarget target) {
            return target == TextureTarget::Texture3D || target == TextureTarget::TextureCubeMap;
        }

        struct StateTextureBasicInfo { // Used for tracking texture state changes
            TextureInternalFormat internalFormat = TextureInternalFormat::Unknown;
            SizeT width = 0;
            SizeT height = 0;
            SizeT depth = 0;
            SizeT mipmapLevels = 0;
            Uint bufferExternalIndex = 0;
            Int samples = 0;
            Bool fixedSampleLocations = true;

            bool operator==(const StateTextureBasicInfo& other) const {
                return internalFormat == other.internalFormat && width == other.width && height == other.height &&
                       depth == other.depth && mipmapLevels == other.mipmapLevels &&
                       bufferExternalIndex == other.bufferExternalIndex && samples == other.samples &&
                       fixedSampleLocations == other.fixedSampleLocations;
            }

            bool operator!=(const StateTextureBasicInfo& other) const { return !(*this == other); }
        };

        inline const Uint TempTextureUnit = 0;
        class BackendTextureObject {
        public:
            BackendTextureObject();
            // Deletes the GL texture (frontend glDeleteTextures used to leak every
            // backend id for the context lifetime) and scrubs the binding/scratch-FBO
            // shadows so a recycled name or heap address cannot false-skip a rebind.
            ~BackendTextureObject();
            BackendTextureObject(const BackendTextureObject&) = delete;
            BackendTextureObject& operator=(const BackendTextureObject&) = delete;
            void SyncMipmapsToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            void SyncBuiltinSamplerToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            void SyncTextureParamsToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            void RequireImageBindableStorage();
            void Bind(GLenum target, Uint unit = TempTextureUnit);
            Uint GetBackendTextureId() const;

        private:
            void RecreateBackendTexture();

            Uint m_backendTextureId = 0;
            // ES context generation the id was created under; a dtor running after
            // that context died must not delete a foreign (recycled) name.
            Uint m_contextGeneration = 0;
            Bool m_isInitialized = false;
            Bool m_imageBindableStorageRequired = false;
            Bool m_backendStorageImmutable = false;
            StateTextureBasicInfo m_prevTextureInfo;
            // Frontend content version at the last completed mipmap sync. The per-draw
            // clean probe compares this before rebuilding shape info and scanning
            // per-level dirty flags; 0 never matches a real version (they start at 1).
            Uint64 m_syncedContentVersion = 0;
            // First-level clean gate for SyncMipmapsToBackend, checked before even the
            // IsComplete()/shape-probe walk. Valid only as a trio with the content and
            // texture-params versions: the context's sampling-resolution generation moves on
            // EVERY texture-shape mutation (BumpShapeVersion is the only writer of shape and
            // unconditionally bumps it), the content version on every CPU pixel mutation, and
            // the params version covers SetSamples/SetFixedSampleLocations, which bump neither
            // of the other two but feed the shape probe. The context id pins the generation to
            // the context that produced it - generations restart at 0 with a new context, and a
            // texture is owned by exactly one context (share groups are not implemented), so a
            // mutation can never happen under a context this key does not name. 0 = never
            // stamped (real context ids start at 1). Backend-side invalidation rides on
            // m_isInitialized: RequireImageBindableStorage and RecreateBackendTexture clear it.
            Uint64 m_syncedShapeContextId = 0;
            Uint64 m_syncedShapeGeneration = 0;
            Uint16 m_syncedShapeParamsVersion = 0;
            SamplerParameters m_cacheSamplerParameters;
            UintVec2 m_cacheLodRange = {0, 1000};
            FloatVec4 m_cacheBorderColor = {0.0f, 0.0f, 0.0f, 0.0f};
            Vec4<TextureSwizzleParam> m_cacheSwizzleParams = {TextureSwizzleParam::Red, TextureSwizzleParam::Green,
                                                              TextureSwizzleParam::Blue, TextureSwizzleParam::Alpha};
            Uint16 m_syncedSamplerVersion = 0;
            Uint16 m_syncedTextureParamsVersion = 0;
        };

        void ActivateTextureUnit(Uint unit);
        void UnbindTexture(Uint unit, GLenum target);
        extern StateBackendObjectRegistry<MG_State::GLState::ITextureObject, BackendTextureObject>
            g_backendTextureObjects;
        SharedPtr<BackendTextureObject>& SyncTextureObjectToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
            Bool imageBindableStorageRequired = false);
        extern Array<Array<BackendTextureObject*, (SizeT)TextureTarget::TextureTargetCount>,
                     MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundTexturesCache;
        extern Uint g_activeTextureUnit;
        // Bumped when the backend ES context is destroyed; texture ids stamped with
        // an older generation belong to a dead context and must not be deleted.
        extern Uint g_textureContextGeneration;
    } // namespace TextureImpl

    namespace FramebufferImpl {
        class BackendFramebufferObject {
        public:
            BackendFramebufferObject();
            void SyncToBackend(const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject,
                               FramebufferTarget asTarget);
            // Apply only this FBO's read buffer (glReadBuffer) to the backend. Split out so it can
            // still run when SyncCurrentFBO skips the READ-target sync because the same GL FBO is
            // bound as both draw and read (otherwise glReadBuffer changes would be silently dropped).
            void SyncReadBufferToBackend(const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject);
            void InvalidateSyncedState();
            Uint GetBackendFramebufferId() const { return m_backendFBOId; }
            void Bind(FramebufferTarget target) const;
            //            FramebufferAttachmentType GetCompactedAttachmentTypeAtDrawBufferIndex(Int index);
            GLenum GetBackendAttachmentType(FramebufferAttachmentType frontendAtt) const;

        private:
            Uint m_backendFBOId = 0;

            /* this will save buffers in its original form,
               reversion, absence or not consecutive are all allowed, as long as GL spec allows it
               i.e. it could be like [COLOR_ATTACHMENT0, COLOR_ATTACHMENT5, NONE, COLOR_ATTACHMENT4]
               Probably useful to re-link shader output according to this.
               aka. realizing `glBindFragDataLocation`
             */
            FramebufferAttachmentType m_frontendDrawBuffers[MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS] = {
                FramebufferAttachmentType::None};
            /* this will save buffers in stricter ES rules
               reversion, absence or not consecutive are not allowed, according to ES spec
               i.e. it could be like [COLOR_ATTACHMENT0, COLOR_ATTACHMENT1, NONE, COLOR_ATTACHMENT3, ...]
               this array could be provided as data directly to ES `glDrawBuffers` function
             */
            GLenum m_backendDrawBuffers[MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS] = {GL_NONE};

            static constexpr Uint MAX_COLOR_ATTACHMENT_SLOTS =
                static_cast<Uint>(FramebufferAttachmentType::Color31) -
                static_cast<Uint>(FramebufferAttachmentType::Color0) + 1;
            /* Where each frontend GL_COLOR_ATTACHMENTn image physically lives in the backend ES
               framebuffer, as a GL_COLOR_ATTACHMENTm enum. ES only accepts glDrawBuffers bufs[s] ==
               GL_COLOR_ATTACHMENTs, so a GL draw-buffer slot s naming attachment a forces a's image
               under backend slot s. This table is the single owner of that decision and is kept a
               PERMUTATION of the backend colour slots: every other attachment keeps its identity
               slot when that slot survived, and is parked on the lowest free slot when it did not.
               Deriving the point per-query from the draw-buffer array instead handed the identity
               point to any attachment that was not a draw buffer - i.e. exactly the point a
               relocated draw buffer had just taken over. The permutation is only true of the
               PHYSICAL framebuffer because the attachment loop detaches a point whose frontend
               owner is empty; do not remove that detach. */
            GLenum m_backendColorSlots[MAX_COLOR_ATTACHMENT_SLOTS] = {GL_NONE};
            /* Rebuild m_backendColorSlots from the frontend draw-buffer array. Returns true when any
               attachment moved, i.e. when the physical attachments and the memoised read buffer have
               to be re-applied. */
            Bool RecomputeBackendColorSlots(
                const MG_State::GLState::FramebufferObject::FramebufferAttachmentArray& stateDrawBuffers);

            FramebufferAttachmentType m_frontendReadBuffer = FramebufferAttachmentType::Color0;
            GLenum m_backendReadBuffer = GL_COLOR_ATTACHMENT0;

            using FramebufferObject = MG_State::GLState::FramebufferObject;
            FramebufferObject::FramebufferAttachmentVersionArray m_syncedFrontendAttachmentVersions = {0};
        };

        extern StateBackendObjectRegistry<MG_State::GLState::FramebufferObject, BackendFramebufferObject>
            g_backendFramebufferObjects;
        // True when the read buffer names a fixed-point (norm/snorm) attachment that the
        // backend actually stores in a floating-point format. GL clamps a read from a
        // fixed-point colour buffer to [0,1] (GL_CLAMP_READ_COLOR defaults to
        // GL_FIXED_ONLY); the substituted float storage would not, so the readback path
        // has to apply the clamp itself.
        Bool IsFixedPointFallbackReadAttachment();

        // What SyncCurrentFBO last pushed for each target, as a (binding, object, revision)
        // triple; it re-syncs unless all three still match. Stamped by SyncCurrentFBO and
        // ForceBindCurrentFBO, cleared by InvalidateFramebufferBindingCache. The three are
        // only meaningful together - see SyncCurrentFBO.
        //
        // The binding slot's own version, which changes whenever a different object is bound
        // to this target. Distinguishes a rebind from an in-place edit, and keeps the raw
        // pointer below from matching an address the allocator recycled for a new FBO.
        extern Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedSlotVersions;
        // Tracks the bound FBO's object version (bumped on any attachment/drawbuffer change)
        // per target: re-attaching textures or changing draw buffers on an already-bound FBO
        // must re-sync it even when the binding-slot version has not moved.
        extern Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedObjectVersions;
        // Which object was synced. Raw and never dereferenced: only compared for identity.
        extern Array<MG_State::GLState::FramebufferObject*, SizeT(FramebufferTarget::FramebufferTargetCount)>
            g_fboSyncedObjects;

        // Driver-level READ/DRAW framebuffer-binding shadow. Every backend
        // glBindFramebuffer routes through BindFramebufferId so scoped helpers can
        // save/restore the current binding without a glGetIntegerv round-trip (that
        // query forces a driver pipeline sync) and so redundant rebinds no-op.
        // Starts unknown; the first CurrentFramebufferBinding() query pins it from
        // the driver once. Invalidated on MakeCurrent (context may reset).
        // GL_FRAMEBUFFER binds both targets.
        void BindFramebufferId(GLenum fbTarget, Uint id);
        Uint CurrentFramebufferBinding(FramebufferTarget target);
        void InvalidateFramebufferBindingCache();
    } // namespace FramebufferImpl

    // Shared scratch framebuffers for the readback/copy/blit emulation paths, with a
    // driver-side attachment shadow: repeated uses skip redundant detach/attach GL
    // calls, and an attachment left by one use (e.g. a depth copy's DEPTH_STENCIL
    // texture) is detached exactly when a later use of another aspect would
    // otherwise inherit it (stale cross-aspect attachments made the shared temp FBO
    // incomplete and silently degraded later readbacks).
    namespace ScratchFBOImpl {
        struct ScratchFramebuffer {
            Uint id = 0;
            // false => attachment state unknown; scrub every point on next use.
            // A fresh FBO starts with nothing attached, so creation sets it true.
            Bool attachmentsKnown = false;
            Uint colorTex = 0;
            GLenum colorTarget = 0;
            GLint colorLevel = 0;
            GLint colorLayer = -1; // >= 0 => attached via glFramebufferTextureLayer
            Uint depthTex = 0;
            GLenum depthTarget = 0;
            GLint depthLevel = 0;
            Bool depthHasStencil = false;
            // Per-FBO read/draw buffer state (0 = unknown, set on first use).
            GLenum readBuffer = 0;
            GLenum drawBuffer = 0;
        };
        ScratchFramebuffer& TempFramebuffer();     // GetTexImage READ / CopyTex*Image2D depth DRAW
        ScratchFramebuffer& BlitReadFramebuffer(); // texture-to-texture blit source
        ScratchFramebuffer& BlitDrawFramebuffer(); // texture-to-texture blit destination
        // Returns the GL id, generating it if needed (requires a current ES context).
        Uint EnsureId(ScratchFramebuffer& fb);
        // The fb must currently be bound at fbTarget (glReadBuffer/glDrawBuffers
        // target the READ/DRAW binding respectively). Each Ensure* performs the
        // minimal detach/attach set and keeps the shadow in sync; a failed attach
        // records the point as detached so the completeness check fails instead of
        // silently reading a stale attachment.
        void EnsureColorAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget, GLint level);
        void EnsureColorAttachmentLayer(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLint level, GLint layer);
        void EnsureDepthAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget, GLint level,
                                     Bool withStencil);
        void EnsureNoColorAttachment(ScratchFramebuffer& fb, GLenum fbTarget);
        void EnsureNoDepthAttachment(ScratchFramebuffer& fb, GLenum fbTarget);
        void EnsureReadBuffer(ScratchFramebuffer& fb, GLenum readBuffer);
        void EnsureDrawBuffer(ScratchFramebuffer& fb, GLenum drawBuffer);
        // A 1x1 RGBA8-renderbuffer-complete FBO (GenerateMipmap needs a complete
        // binding while respecifying texture storage). Attachment is set once at
        // creation and never changes.
        Uint EnsureCompleteTinyFramebufferId();
        // A backend texture id is being deleted or respecified: a scratch FBO still
        // referencing it would hold a dangling attachment (ES only auto-detaches
        // from the *bound* framebuffer), and a recycled name could false-skip a
        // re-attach; force a full scrub on next use.
        void NoteTextureIdDeleted(Uint textureId);
        // The ES context (and the scratch FBO ids with it) is going away.
        void OnBackendContextDestroyed();
    } // namespace ScratchFBOImpl

    // Driver-level GL_PACK_* pixel-store shadow, the readback-side sibling of the
    // upload path's ScopedDefaultUnpackState (Managers.cpp): the backend PACK state
    // is written ONLY through ApplyPackState, so scoped helpers can save/restore it
    // from the shadow instead of glGetIntegerv (which forces a driver pipeline
    // sync), and redundant glPixelStorei calls no-op. The first Apply/Current call
    // pins the driver to the shadow by writing all fields once. Invalidated on
    // MakeCurrent (context may reset). PACK_IMAGE_HEIGHT/SKIP_IMAGES/SWAP_BYTES/
    // LSB_FIRST have no ES equivalents; readbacks honor them on the CPU from the
    // frontend context state instead.
    namespace PixelStoreImpl {
        struct PackState {
            GLint Alignment = 4;
            GLint RowLength = 0;
            GLint SkipRows = 0;
            GLint SkipPixels = 0;
            Bool operator==(const PackState& o) const {
                return Alignment == o.Alignment && RowLength == o.RowLength && SkipRows == o.SkipRows &&
                       SkipPixels == o.SkipPixels;
            }
        };
        void ApplyPackState(const PackState& desired);
        PackState CurrentPackState();
        void InvalidatePackStateCache();
    } // namespace PixelStoreImpl

    // Image uniforms take their unit from the layout(binding=N) qualifier baked into
    // the transpiled ESSL; unlike samplers they must not (and in ES cannot) be
    // assigned through glUniform1i.
    inline Bool IsImageUniformType(GLenum type) {
        switch (type) {
        case 0x904D: /*GL_IMAGE_2D*/
        case 0x904E: /*GL_IMAGE_3D*/
        case 0x9050: /*GL_IMAGE_CUBE*/
        case 0x9051: /*GL_IMAGE_BUFFER*/
        case 0x9053: /*GL_IMAGE_2D_ARRAY*/
        case 0x9058: /*GL_INT_IMAGE_2D*/
        case 0x9059: /*GL_INT_IMAGE_3D*/
        case 0x905B: /*GL_INT_IMAGE_CUBE*/
        case 0x905C: /*GL_INT_IMAGE_BUFFER*/
        case 0x905E: /*GL_INT_IMAGE_2D_ARRAY*/
        case 0x9063: /*GL_UNSIGNED_INT_IMAGE_2D*/
        case 0x9064: /*GL_UNSIGNED_INT_IMAGE_3D*/
        case 0x9066: /*GL_UNSIGNED_INT_IMAGE_CUBE*/
        case 0x9067: /*GL_UNSIGNED_INT_IMAGE_BUFFER*/
        case 0x9069: /*GL_UNSIGNED_INT_IMAGE_2D_ARRAY*/
            return true;
        default:
            return false;
        }
    }

    namespace PrgramImpl {
        class BackendProgramObjectImpl {
        public:
            // Per-link cache of a sampler-style uniform's backend location: built once in
            // SyncToBackend so draws stop issuing glGetUniformLocation string queries.
            // lastAssignedUnit mirrors the program-state value set through glUniform1i
            // (program state persists across binds, so caching per program is exact).
            struct SamplerUniformBinding {
                Uint frontendLocation = 0;
                Int backendLocation = -1;
                GLenum uniformType = 0;
                Int lastAssignedUnit = -1;
                // Location of this sampler's emulated GL_TEXTURE_LOD_BIAS uniform
                // (PrgramImpl::EmulateTextureLodBias), -1 when the shader has none.
                // lastAssignedLodBias mirrors the value the program currently holds,
                // so an unbiased shader issues no per-draw glUniform1f at all.
                Int lodBiasLocation = -1;
                Float lastAssignedLodBias = 0.0f;
            };

            BackendProgramObjectImpl();
            ~BackendProgramObjectImpl();
            void SyncToBackend(const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject);
            void Use() const;
            void SetBaseInstance(Uint32 baseInstance) const;
            void SetBaseInstanceWordIndex(Int32 wordIndex) const;
            void SetDrawID(Uint32 drawId) const;
            Int GetIndirectParamsBinding() const { return m_indirectParamsBinding; }
            Uint GetBackendProgramId() const { return m_backendProgramId; }
            // False when the last SyncToBackend could not produce a usable program (a
            // shader failed to transpile or compile, or the link itself failed). Use()
            // must not leave the previously bound program current in that case.
            Bool IsBackendProgramUsable() const { return m_backendProgramUsable; }
            Uint GetBackendGlobalUBOId() const { return m_backendGlobalUBOId; }
            Uint32 GetSnormFallbackClampOutputMask() const { return m_snormFallbackClampOutputMask; }
            Uint32 GetUnormFallbackClampOutputMask() const { return m_unormFallbackClampOutputMask; }
            Uint GetFragColorBroadcastCount() const { return m_fragColorBroadcastCount; }

            Bool HasGlobalUboBlock() const { return m_globalUboBackendBlockIndex >= 0; }
            const Vector<Int>& GetUniformBlockBackendIndices() const { return m_uniformBlockBackendIndices; }
            Vector<SamplerUniformBinding>& GetSamplerUniformBindings() { return m_samplerUniformBindings; }
            Uint32 GetLastUploadedGlobalUboVersion() const { return m_lastUploadedGlobalUboVersion; }
            void SetLastUploadedGlobalUboVersion(Uint32 version) { m_lastUploadedGlobalUboVersion = version; }
            // Backend-reported GL_UNIFORM_BLOCK_DATA_SIZE of the global block; ring
            // bindings must span at least this much (may exceed the frontend's
            // reflected size when the transpiled block pads differently).
            Int GetGlobalUboBackendBlockSize() const { return m_globalUboBackendBlockSize; }
            BufferImpl::UboRingAllocation& GetGlobalUboRingAllocation() { return m_globalUboRingAllocation; }
            // Frontend link version this backend program (and its resource caches) was
            // built from; a mismatch means every link-derived cache here is stale.
            Uint32 GetSyncedLinkVersion() const { return m_syncedLinkVersion; }

        private:
            void CacheResourceLocations(const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject);

            Uint m_backendProgramId = 0;
            Uint m_backendGlobalUBOId = 0;
            Int m_baseInstanceUniformLocation = -1;
            Int m_drawIdUniformLocation = -1;
            Int m_baseInstanceWordIndexUniformLocation = -1;
            Int m_indirectParamsBinding = -1;
            Uint32 m_snormFallbackClampOutputMask = 0;
            Uint32 m_unormFallbackClampOutputMask = 0;
            // Draw buffers a legacy gl_FragColor write has to reach (see
            // PrgramImpl::BroadcastLegacyFragColor); 1 keeps the plain single-output shader.
            Uint m_fragColorBroadcastCount = 1;
            Bool m_isInitialized = false;
            Bool m_backendProgramUsable = false;

            Int m_globalUboBackendBlockIndex = -1;
            Int m_globalUboBackendBlockSize = 0;
            Vector<Int> m_uniformBlockBackendIndices; // frontend block index -> backend index (-1 = absent)
            Vector<SamplerUniformBinding> m_samplerUniformBindings;
            Uint32 m_lastUploadedGlobalUboVersion = ~0u;
            BufferImpl::UboRingAllocation m_globalUboRingAllocation;
            Uint32 m_syncedLinkVersion = ~0u;
        };

        extern Uint32 g_snormFallbackClampOutputMask;
        extern Uint32 g_unormFallbackClampOutputMask;
        // Draw buffers the current draw framebuffer enables. Like the clamp masks above it
        // is framebuffer state that the shader has to be compiled against, so a program
        // whose snapshot no longer matches is relinked.
        extern Uint g_fragColorBroadcastCount;
        // Backend id of the last glUseProgram issued through this backend; lets Use()
        // skip redundant rebinds. Reset to 0 wherever glUseProgram(0) is issued or the
        // ES context is recreated.
        extern Uint g_lastUsedBackendProgramId;
        extern StateBackendObjectRegistry<MG_State::GLState::ProgramObject, BackendProgramObjectImpl>
            g_backendProgramObjects;
    } // namespace PrgramImpl

    namespace SamplerImpl {
        class BackendSamplerObject {
        public:
            BackendSamplerObject();
            void SyncToBackend(const SharedPtr<MG_State::GLState::SamplerObject>& stateSamplerObject);
            void Bind(Uint unit);
            Uint GetBackendSamplerId() const;

        private:
            Uint m_backendSamplerId = 0;
            Bool m_isInitialized = false;
            SamplerParameters m_cacheSamplerParameters;
            Uint16 m_syncedSamplerVersion = 0;
        };

        void UnbindSampler(Uint unit);

        extern Array<BackendSamplerObject*, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundSamplersCache;
        extern StateBackendObjectRegistry<MG_State::GLState::SamplerObject, BackendSamplerObject>
            g_backendSamplerObjects;
    } // namespace SamplerImpl

    namespace RenderbufferImpl {
        class BackendRenderbufferObject {
        public:
            BackendRenderbufferObject();
            void SyncToBackend(const SharedPtr<MG_State::GLState::RenderbufferObject>& stateRBOObject);
            Uint GetBackendRenderbufferId() const { return m_backendRBOId; }
            void Bind() const;

        private:
            Uint m_backendRBOId = 0;
            Bool m_isInitialized = false;
            TextureInternalFormat m_cacheInternalFormat = TextureInternalFormat::Unknown;
            Int m_cacheWidth = 0;
            Int m_cacheHeight = 0;
            Int m_cacheSamples = 0;
        };

        extern StateBackendObjectRegistry<MG_State::GLState::RenderbufferObject, BackendRenderbufferObject>
            g_backendRenderbufferObjects;
    } // namespace RenderbufferImpl
} // namespace MobileGL::MG_Backend::DirectGLES
