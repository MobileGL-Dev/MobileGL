// MobileGL - MobileGL/MG_Backend/DirectGLES/Managers.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "DirectGLES.h"
#include "MG_State/GLState/SamplerState/SamplerObject.h"
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Backend::DirectGLES {
    String EmulateBaseInstanceInVertexShader(String source, GLenum shaderType);

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
        class BackendBufferObject {
        public:
            BackendBufferObject();
            void SyncToBackend(const SharedPtr<MG_State::GLState::BufferObject>& stateBufferObject);
            Uint GetBackendBufferId() const { return m_backendBufferId; }
            void Bind(GLenum target = TempBufferTarget);

        private:
            void SyncToBackend_glBufferData(const SharedPtr<MG_State::GLState::BufferObject>& stateBufferObject);
            void SyncToBackend_glBufferSubData(const SharedPtr<MG_State::GLState::BufferObject>& stateBufferObject);
            void SyncToBackend_glMapBufferRange(const SharedPtr<MG_State::GLState::BufferObject>& stateBufferObject,
                                                Bool invalidate = true, Bool unsynchronized = true);

            Uint m_backendBufferId = 0;
            SizeT m_prevBufferSize = 0;
            Bool m_isInitialized = false;
        };

        extern BackendBufferObject* g_boundVertexBufferObject;
        extern StateBackendObjectRegistry<MG_State::GLState::BufferObject, BackendBufferObject> g_backendBufferObjects;
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
            void Bind(GLenum target, Uint unit = TempTextureUnit);
            Uint GetBackendTextureId() const;

        private:
            Uint m_backendTextureId = 0;
            Bool m_isInitialized = false;
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
            const SharedPtr<MG_State::GLState::ITextureObject>& textureObject);
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

    namespace PrgramImpl {
        class BackendProgramObjectImpl {
        public:
            BackendProgramObjectImpl();
            ~BackendProgramObjectImpl();
            void SyncToBackend(const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject);
            void Use() const;
            void SetBaseInstance(Uint32 baseInstance) const;
            Uint GetBackendProgramId() const { return m_backendProgramId; }
            Uint GetBackendGlobalUBOId() const { return m_backendGlobalUBOId; }
            Uint32 GetRGBA8SnormClampOutputMask() const { return m_rgba8SnormClampOutputMask; }

        private:
            Uint m_backendProgramId = 0;
            Uint m_backendGlobalUBOId = 0;
            Int m_baseInstanceUniformLocation = -1;
            Uint32 m_rgba8SnormClampOutputMask = 0;
            Bool m_isInitialized = false;
        };

        extern Uint32 g_rgba8SnormClampOutputMask;
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
