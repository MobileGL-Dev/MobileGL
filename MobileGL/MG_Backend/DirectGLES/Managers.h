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

namespace MobileGL::MG_Backend::DirectGLES {
    String EmulateBaseInstanceInVertexShader(String source, GLenum shaderType);
    String PromoteDrawParameterGlobalsToUniforms(String source, GLenum shaderType);

    template <typename StateObject, typename BackendObject>
    class StateBackendObjectRegistry {
    public:
        using StatePtr = SharedPtr<StateObject>;
        using StateWeakPtr = std::weak_ptr<StateObject>;
        using BackendPtr = SharedPtr<BackendObject>;
        using BackendMap = UnorderedMap<StateObject*, BackendPtr>;
        using StateRefMap = UnorderedMap<StateObject*, StateWeakPtr>;
        using iterator = typename BackendMap::iterator;
        using const_iterator = typename BackendMap::const_iterator;

        BackendPtr& GetOrCreate(const StatePtr& stateObj) {
            MOBILEGL_ASSERT(stateObj != nullptr, "State object must not be null");

            auto* key = stateObj.get();
            auto trackedStateIt = m_stateRefs.find(key);
            if (trackedStateIt != m_stateRefs.end() && trackedStateIt->second.expired()) {
                EraseByKey(key);
            }
            m_stateRefs[key] = stateObj;
            return m_backendObjects[key];
        }

        iterator find(StateObject* stateObj) {
            if (!IsAlive(stateObj)) {
                EraseByKey(stateObj);
                return m_backendObjects.end();
            }
            return m_backendObjects.find(stateObj);
        }

        const_iterator find(StateObject* stateObj) const {
            return const_cast<StateBackendObjectRegistry*>(this)->find(stateObj);
        }

        iterator begin() { return m_backendObjects.begin(); }
        const_iterator begin() const { return m_backendObjects.begin(); }
        iterator end() { return m_backendObjects.end(); }
        const_iterator end() const { return m_backendObjects.end(); }

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
        bool IsAlive(StateObject* stateObj) const {
            const auto trackedStateIt = m_stateRefs.find(stateObj);
            if (trackedStateIt == m_stateRefs.end()) {
                return false;
            }
            return !trackedStateIt->second.expired();
        }

        void EraseByKey(StateObject* stateObj) {
            m_stateRefs.erase(stateObj);
            m_backendObjects.erase(stateObj);
        }

        void CollectGarbage() {
            if (m_isCollecting) {
                return;
            }

            m_isCollecting = true;

            Vector<StateObject*> staleKeys;
            staleKeys.reserve(m_stateRefs.size());
            for (const auto& [stateKey, stateWeakRef] : m_stateRefs) {
                if (stateWeakRef.expired()) {
                    staleKeys.push_back(stateKey);
                }
            }

            for (auto* stateKey : staleKeys) {
                m_stateRefs.erase(stateKey);
                m_backendObjects.erase(stateKey);
            }

            m_isCollecting = false;
        }

    private:
        static constexpr Uint32 kGCInterval = 1024;
        StateRefMap m_stateRefs;
        BackendMap m_backendObjects;
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

        // Deletes GL buffers whose owning frontend objects died (possibly on a
        // thread without a current ES context). Called from draw-time sync.
        void ProcessDeferredBufferReleases();

        // glBindBuffer with a redundant-bind cache for GL_ARRAY_BUFFER.
        void BindBufferId(GLenum target, Uint id);
        void InvalidateArrayBufferBindingCache();
        // Buffer-storage pool maintenance. TrimBufferPool evicts over-budget entries
        // (called once per frame from Present); ClearBufferPool drops all pooled ids
        // without glDeleteBuffers (called when the ES context is going away).
        void TrimBufferPool();
        void ClearBufferPool();
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

        private:
            Uint m_backendVAOId = 0;
            Array<Uint, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS> m_clientAttributeBufferIds;
            Bool m_isInitialized = false;
            Uint16 m_syncedIndexBufferVersion = 0;
            Array<MG_State::GLState::VertexAttributeVersion, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS>
                m_syncedAttributeVersions;
        };

        extern StateBackendObjectRegistry<MG_State::GLState::VertexArrayObject, BackendVertexArrayObject>
            g_backendVertexArrayObjects;
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        inline Bool IsSupportedTextureTarget(TextureTarget target) {
            if (target == TextureTarget::Texture1D || target == TextureTarget::TextureRectangle ||
                target == TextureTarget::Texture1DArray || target == TextureTarget::Texture2DArray)
                return false;
            return true;
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
            void SyncMipmapsToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            void SyncBuiltinSamplerToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            void SyncTextureParamsToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            void RequireImageBindableStorage();
            void Bind(GLenum target, Uint unit = TempTextureUnit);
            Uint GetBackendTextureId() const;

        private:
            void RecreateBackendTexture();

            Uint m_backendTextureId = 0;
            Bool m_isInitialized = false;
            Bool m_imageBindableStorageRequired = false;
            Bool m_backendStorageImmutable = false;
            StateTextureBasicInfo m_prevTextureInfo;
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
    } // namespace TextureImpl

    namespace FramebufferImpl {
        class BackendFramebufferObject {
        public:
            BackendFramebufferObject();
            void SyncToBackend(const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject,
                               FramebufferTarget asTarget);
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
            FramebufferAttachmentType m_frontendReadBuffer = FramebufferAttachmentType::Color0;
            GLenum m_backendReadBuffer = GL_COLOR_ATTACHMENT0;

            using FramebufferObject = MG_State::GLState::FramebufferObject;
            FramebufferObject::FramebufferAttachmentVersionArray m_syncedFrontendAttachmentVersions = {0};
        };

        extern StateBackendObjectRegistry<MG_State::GLState::FramebufferObject, BackendFramebufferObject>
            g_backendFramebufferObjects;
        extern Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboBindVersions;
    } // namespace FramebufferImpl

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
            Uint GetBackendGlobalUBOId() const { return m_backendGlobalUBOId; }
            Uint32 GetSnormFallbackClampOutputMask() const { return m_snormFallbackClampOutputMask; }
            Uint32 GetUnormFallbackClampOutputMask() const { return m_unormFallbackClampOutputMask; }

            Bool HasGlobalUboBlock() const { return m_globalUboBackendBlockIndex >= 0; }
            const Vector<Int>& GetUniformBlockBackendIndices() const { return m_uniformBlockBackendIndices; }
            Vector<SamplerUniformBinding>& GetSamplerUniformBindings() { return m_samplerUniformBindings; }
            Uint32 GetLastUploadedGlobalUboVersion() const { return m_lastUploadedGlobalUboVersion; }
            void SetLastUploadedGlobalUboVersion(Uint32 version) { m_lastUploadedGlobalUboVersion = version; }
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
            Bool m_isInitialized = false;

            Int m_globalUboBackendBlockIndex = -1;
            Vector<Int> m_uniformBlockBackendIndices; // frontend block index -> backend index (-1 = absent)
            Vector<SamplerUniformBinding> m_samplerUniformBindings;
            Uint32 m_lastUploadedGlobalUboVersion = ~0u;
            Uint32 m_syncedLinkVersion = ~0u;
        };

        extern Uint32 g_snormFallbackClampOutputMask;
        extern Uint32 g_unormFallbackClampOutputMask;
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
