// MobileGL - MobileGL/MG_Backend/DirectGLES/Managers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Managers.h"
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include "MG_State/GLState/TextureState/TextureObject.h"
#include "MG_State/GLState/TextureState/TextureState.h"
#include "MG_Util/Converters/GLToMG/TextureEnumConverter.h"
#include "Utils.h"
#include "DirectGLES.h"

#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/DataTypeConverter.h>
#include <MG_Util/Converters/MGToGL/BufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/FramebufferEnumConverter.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_Util/Converters/GLToMG/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>

namespace MobileGL::MG_Backend::DirectGLES {
    constexpr Bool PREFER_MAP_BUFFER_RANGE_FOR_BUFFER_SYNC = true;

    namespace BufferImpl {
        BackendBufferObject::BackendBufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenBuffers(1, &m_backendBufferId);
            if (m_backendBufferId == 0) {
                MGLOG_E("Failed to generate buffer object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated buffer object with ID: %u.", m_backendBufferId);
            }
        }

        void BackendBufferObject::SyncToBackend(SharedPtr<MG_State::GLState::BufferObject>& stateBufferObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateBufferObject) {
                MGLOG_E("State buffer object is null, cannot sync to backend.");
                return;
            }

            SizeT bufferSize = stateBufferObject->GetSize();
            if (bufferSize == 0) {
                MGLOG_W("Buffer size is zero, skipping sync for object with ID: %u", m_backendBufferId);
                return;
            }

            MGLOG_D("Syncing buffer object with backend ID %u to backend for state ID %u", m_backendBufferId,
                    stateBufferObject->GetExternalIndex());

            // Decide sync method
            // glBufferData
            Bool needsRegeneration =
                !m_isInitialized || (stateBufferObject->GetChangeBits() & BufferChangeBits::PreferReallocationBit);

            if (needsRegeneration) {
                MGLOG_D("Buffer size changed significantly or not initialized, regenerating buffer with ID: %u",
                        m_backendBufferId);
                SyncToBackend_glBufferData(stateBufferObject);
                m_isInitialized = true;
                m_prevBufferSize = bufferSize;
                stateBufferObject->ClearDirty();
                return;
            }

            // glMapBufferRange or glBufferSubData
            Bool useInvalidationMap = !(stateBufferObject->GetChangeBits() & BufferChangeBits::ForbidInvalidationBit);
            Bool useUnsynchronizedMap =
                !(stateBufferObject->GetChangeBits() & BufferChangeBits::ForbidUnsynchronizationBit);
            Bool useMapBufferRange = useInvalidationMap || useUnsynchronizedMap;

            if (!useMapBufferRange && PREFER_MAP_BUFFER_RANGE_FOR_BUFFER_SYNC) {
                auto usage = stateBufferObject->GetUsage();
                if (usage == BufferUsage::DynamicDraw || usage == BufferUsage::StreamDraw ||
                    usage == BufferUsage::StreamCopy || usage == BufferUsage::DynamicCopy) {
                    useMapBufferRange = true;
                }
            }

            if (useMapBufferRange) {
                MGLOG_D("Using glMapBufferRange to sync buffer with ID: %u", m_backendBufferId);
                SyncToBackend_glMapBufferRange(stateBufferObject, useInvalidationMap, useUnsynchronizedMap);
            } else {
                MGLOG_D("Using glBufferSubData to sync buffer with ID: %u", m_backendBufferId);
                SyncToBackend_glBufferSubData(stateBufferObject);
            }

            // Clear dirty state
            stateBufferObject->ClearDirty();
            m_prevBufferSize = bufferSize;
        }

        void BackendBufferObject::SyncToBackend_glBufferData(
            SharedPtr<MG_State::GLState::BufferObject>& stateBufferObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            MGLOG_D("Syncing buffer data (glBufferData) for object with ID : %u", m_backendBufferId);

            const void* data = stateBufferObject->GetDataReadOnly()->data();
            SizeT size = stateBufferObject->GetSize();
            GLenum usage = MG_Util::ConvertBufferUsageToGLEnum(stateBufferObject->GetUsage());

            Bind();
            g_GLESFuncs.glBufferData(TempBufferTarget, size, data, usage);
        }

        void BackendBufferObject::SyncToBackend_glBufferSubData(
            SharedPtr<MG_State::GLState::BufferObject>& stateBufferObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            MGLOG_D("Syncing buffer sub-data (glBufferSubData) for object with ID : %u", m_backendBufferId);

            const void* data = stateBufferObject->GetDataReadOnly()->data();
            // dirty range: [range.start, range.end)
            auto ranges = stateBufferObject->GetDirtyRanges();
            if (ranges.empty()) {
                MGLOG_D("No dirty range to sync for buffer with ID: %u", m_backendBufferId);
                return;
            }

            for (const auto& range : ranges) {
                Bind();
                g_GLESFuncs.glBufferSubData(TempBufferTarget, range.start, range.end - range.start,
                                            reinterpret_cast<const char*>(data) + range.start);
            }
        }

        void BackendBufferObject::SyncToBackend_glMapBufferRange(
            SharedPtr<MG_State::GLState::BufferObject>& stateBufferObject, Bool invalidate, Bool unsynchronized) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            MGLOG_D("Syncing buffer map (glMapBuffer) for object with ID : %u", m_backendBufferId);
            MGLOG_D("Mapping buffer with ID: %u", m_backendBufferId);
            auto ranges = stateBufferObject->GetDirtyRanges();
            if (ranges.empty()) {
                MGLOG_D("No dirty range to sync for buffer with ID: %u", m_backendBufferId);
                return;
            }
            SizeT minStart = ranges.GetOverallMinStart();
            SizeT maxEnd = ranges.GetOverallMaxEnd();
            Bind();
            void* mappedData = g_GLESFuncs.glMapBufferRange(TempBufferTarget, minStart, maxEnd - minStart,
                                                            (invalidate ? GL_MAP_INVALIDATE_RANGE_BIT : 0) |
                                                                (unsynchronized ? GL_MAP_UNSYNCHRONIZED_BIT : 0) |
                                                                GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
            const void* data = stateBufferObject->GetDataReadOnly()->data();
            if (mappedData) {
                MGLOG_D("Mapped buffer data successfully for object with ID: %u", m_backendBufferId);
                Memcpy(mappedData, reinterpret_cast<const char*>(data) + minStart, maxEnd - minStart);
                // Explicitly flush the dirty ranges
                for (const auto& range : ranges) {
                    g_GLESFuncs.glFlushMappedBufferRange(TempBufferTarget, range.start - minStart,
                                                         range.end - range.start);
                }
                g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
            } else {
                MGLOG_E("Failed to map buffer with ID: %u", m_backendBufferId);
            }
        }

        void BackendBufferObject::Bind(GLenum target) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (target == GL_ARRAY_BUFFER) {
                if (g_boundVertexBufferObject == this) {
                    return;
                }
                g_boundVertexBufferObject = this;
            }
            g_GLESFuncs.glBindBuffer(target, m_backendBufferId);
        }

        UnorderedMap<SharedPtr<MG_State::GLState::BufferObject>, SharedPtr<BackendBufferObject>> g_backendBufferObjects;
        BackendBufferObject* g_boundVertexBufferObject = nullptr;
    } // namespace BufferImpl

    namespace VertexArrayImpl {
        BackendVertexArrayObject::BackendVertexArrayObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenVertexArrays(1, &m_backendVAOId);
            if (m_backendVAOId == 0) {
                MGLOG_E("Failed to generate vertex array object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated vertex array object with ID: %u.", m_backendVAOId);
            }
        }

        void BackendVertexArrayObject::Bind() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glBindVertexArray(m_backendVAOId);
        }

        void BackendVertexArrayObject::BindAttributeBuffer(Uint index,
                                                           const MG_State::GLState::VertexAttribute& attrib) {
            const auto& bufferObject = attrib.Buffer;
            if (!bufferObject) {
                MGLOG_W("Attribute has no bound buffer, skipping.");
                return;
            }

            const auto& backendBufferIt = BufferImpl::g_backendBufferObjects.find(bufferObject);
            if (backendBufferIt == BufferImpl::g_backendBufferObjects.end()) {
                MGLOG_E("No backend buffer found for attribute's buffer, cannot bind attribute.");
                return;
            }
            const auto& backendBufferObject = backendBufferIt->second;

            backendBufferObject->Bind(GL_ARRAY_BUFFER);
        }

        void BackendVertexArrayObject::SyncToBackend(SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateVAOObject) {
                MGLOG_E("State VAO object is null, cannot sync to backend.");
                return;
            }

            MGLOG_D("Syncing VAO with backend ID %u to backend for state ID %u", m_backendVAOId,
                    stateVAOObject->GetExternalIndex());

            Bind();

            const auto& allAttributeVersions = stateVAOObject->GetAllAttributeVersions();
            const auto& allAttributes = stateVAOObject->GetAllAttributes();
            for (Uint attribIndex = 0; attribIndex < allAttributes.size(); ++attribIndex) {
                const auto& attrib = allAttributes[attribIndex];
                Bool needsSyncSwitch = allAttributeVersions[attribIndex].SwitchVersion !=
                                       m_syncedAttributeVersions[attribIndex].SwitchVersion;
                if (needsSyncSwitch) {
                    if (attrib.Enabled) {
                        g_GLESFuncs.glEnableVertexAttribArray(attribIndex);
                    } else {
                        g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                    }
                }

                Bool needsSyncFormat = allAttributeVersions[attribIndex].FormatVersion !=
                                       m_syncedAttributeVersions[attribIndex].FormatVersion;
                Bool needsSyncBuffer = allAttributeVersions[attribIndex].BufferVersion !=
                                       m_syncedAttributeVersions[attribIndex].BufferVersion;
                if (!needsSyncFormat && !needsSyncBuffer) continue;

                BindAttributeBuffer(attribIndex, attrib);

                if (!attrib.IsInteger) {
                    g_GLESFuncs.glVertexAttribPointer(
                        attribIndex, attrib.Size, MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                        attrib.Normalized ? GL_TRUE : GL_FALSE, attrib.Stride, (const void*)attrib.Offset);
                } else {
                    g_GLESFuncs.glVertexAttribIPointer(attribIndex, attrib.Size,
                                                       MG_Util::ConvertDataTypeToGLEnum(attrib.Type), attrib.Stride,
                                                       (const void*)attrib.Offset);
                }

                if (needsSyncFormat) {
                    g_GLESFuncs.glVertexAttribDivisor(attribIndex, attrib.Divisor);
                }
            }

            Uint16 currentIndexBufferVersion = stateVAOObject->GetIndexBufferBindingSlot().GetVersion();
            if (currentIndexBufferVersion != m_syncedIndexBufferVersion) {
                const auto& indexBufferBinding = stateVAOObject->GetIndexBufferBindingSlot().GetBoundObject();
                if (indexBufferBinding) {
                    const auto& backendBufferIt = BufferImpl::g_backendBufferObjects.find(indexBufferBinding);
                    if (backendBufferIt != BufferImpl::g_backendBufferObjects.end()) {
                        const auto& backendBufferObject = backendBufferIt->second;
                        backendBufferObject->Bind(GL_ELEMENT_ARRAY_BUFFER);
                    } else {
                        MGLOG_W("No backend buffer found for index buffer binding, cannot bind index buffer.");
                    }
                }
                m_syncedIndexBufferVersion = currentIndexBufferVersion;
            }

            m_syncedAttributeVersions = allAttributeVersions;
        }

        UnorderedMap<SharedPtr<MG_State::GLState::VertexArrayObject>, SharedPtr<BackendVertexArrayObject>>
            g_backendVertexArrayObjects;
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        BackendTextureObject::BackendTextureObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenTextures(1, &m_backendTextureId);
            if (m_backendTextureId == 0) {
                MGLOG_E("Failed to generate texture object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated texture object with ID: %u.", m_backendTextureId);
            }
        }

        void BackendTextureObject::Bind(GLenum target, Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (g_activeTextureUnit != unit) {
                ActivateTextureUnit(unit);
            }

            auto targetN = static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(target));
            if (this == g_boundTexturesCache[unit][targetN]) return;

            g_GLESFuncs.glBindTexture(target, m_backendTextureId);
            g_boundTexturesCache[unit][targetN] = this;
        }

        Uint BackendTextureObject::GetBackendTextureId() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            return m_backendTextureId;
        }

        void BackendTextureObject::SyncMipmapsToBackend(
            SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            if (!stateTextureObject) {
                MGLOG_E("State texture object is null, cannot sync to backend.");
                return;
            }

#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            DebugImpl::ErrorLopper errorLopper;

            MGLOG_D("Syncing texture mipmaps with backend ID %u to backend for state ID %u", m_backendTextureId,
                    stateTextureObject->GetExternalIndex());

            GLenum target = MG_Util::ConvertTextureTargetToGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            // The texture needs to be regenerated completely with glTexImage* calls if:
            // 1. Not initialized
            // 2. InternalFormat changed
            // 3. Size changed
            // 4. Mipmap levels changed

            if (!stateTextureObject->IsComplete()) {
                MGLOG_D("Texture object with ID: %u is not complete, skipping sync.",
                        stateTextureObject->GetExternalIndex());
                return;
            }

            Bind(target);
            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            const auto baseSize = stateTextureObject->GetBaseSize();
            StateTextureBasicInfo currentTextureInfo = {stateTextureObject->GetFormat(),
                                                        static_cast<SizeT>(baseSize.x()),
                                                        static_cast<SizeT>(baseSize.y()),
                                                        static_cast<SizeT>(baseSize.z()),
                                                        0,
                                                        0};
            switch (stateTextureObject->GetStorageType()) {
            case TextureStorageType::Mipmap: {
                auto* textureMipmapObject =
                    static_cast<MG_State::GLState::TextureObjectMipmap*>(stateTextureObject.get());
                const auto mipmapCount = textureMipmapObject->GetMipmapLevelCount();
                currentTextureInfo.mipmapLevels = mipmapCount;

                Bool needsRegeneration = !m_isInitialized || (currentTextureInfo != m_prevTextureInfo);

                MGLOG_D("%s: Got texture info: %dx%dx%d, mips %d, format %s", __func__, baseSize.x(), baseSize.y(),
                        baseSize.z(), mipmapCount,
                        MG_Util::ConvertTextureInternalFormatToString(textureMipmapObject->GetFormat()).c_str());

                if (needsRegeneration) {
                    MGLOG_D("Texture state changed significantly or not initialized, regenerating texture with ID: %u",
                            m_backendTextureId);

                    // Regenerate all mipmap levels
                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(textureMipmapObject->GetFormat(), &glInternalFormat,
                                                           &glFormat, &glType);

                    const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                    for (auto uploadTarget : uploadTargets) {
                        for (SizeT level = 0; level < mipmapCount; ++level) {
                            auto levelTexelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                            auto levelByteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                            bool levelDirty = textureMipmapObject->IsStorageDirty(uploadTarget, level);
                            auto glUploadTarget = MG_Util::ConvertTextureUploadTargetToGLEnum(uploadTarget);
                            auto* pData = (levelDirty && levelByteSize != 0)
                                              ? textureMipmapObject->MapMipmapData(uploadTarget, level)
                                              : nullptr;
                            MGLOG_D(
                                "%s: target: %s: syncing mip %d: %dx%dx%d, byteSize = %d, pData = %p, levelDirty = %s",
                                __func__, MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(), level,
                                levelTexelSize.x(), levelTexelSize.y(), levelTexelSize.z(), levelByteSize, pData,
                                levelDirty ? "true" : "false");

                            errorLopper.Clear();
                            g_GLESFuncs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                            auto textureTarget = stateTextureObject->GetTarget();
                            // TODO: handle more texture types
                            switch (textureTarget) {
                            case TextureTarget::Texture2D:
                            case TextureTarget::TextureCubeMap: {
                                g_GLESFuncs.glTexImage2D(glUploadTarget, static_cast<GLint>(level), glInternalFormat,
                                                         static_cast<GLsizei>(levelTexelSize.x()),
                                                         static_cast<GLsizei>(levelTexelSize.y()), 0, glFormat, glType,
                                                         pData);
                                break;
                            }
                            case TextureTarget::Texture3D: {
                                g_GLESFuncs.glTexImage3D(
                                    glUploadTarget, static_cast<GLint>(level), glInternalFormat,
                                    static_cast<GLsizei>(levelTexelSize.x()), static_cast<GLsizei>(levelTexelSize.y()),
                                    static_cast<GLsizei>(levelTexelSize.z()), 0, glFormat, glType, pData);
                                break;
                            }
                            default: {
                                MGLOG_E("Unhandled texture target %s",
                                        MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
                            }
                            }
                            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__, glUploadTarget,
                                              glInternalFormat, glFormat, glType, pData](GLenum err) {
                                MGLOG_D("%s(%s:%d) ES error: %s. glTexImage*: target=%s, internalformat=%s, format=%s, "
                                        "type=%s, pixels=%p",
                                        func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                        MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                        MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                        MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                        MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                            });
                            MGLOG_D("Regenerated mipmap level %d for texture with ID: %u", level, m_backendTextureId);
                            textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                        }
                    }

                    m_isInitialized = true;
                }

                { // Update all dirty mipmap levels
                    const auto mipmapCount = textureMipmapObject->GetMipmapLevelCount();
                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(textureMipmapObject->GetFormat(), &glInternalFormat,
                                                           &glFormat, &glType);
                    const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                    for (auto uploadTarget : uploadTargets) {
                        for (SizeT level = 0; level < mipmapCount; ++level) {
                            if (!textureMipmapObject->IsStorageDirty(uploadTarget, level)) {
                                continue;
                            }

                            auto byteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                            if (byteSize == 0) {
                                MGLOG_W("Mipmap level %d has no data, skipping update.", level);
                                continue;
                            }

                            if (level > 0)
                                MGLOG_D("%s: Updating dirty mip %d for texture ID %u, size: %dx%d, "
                                        "byteSize: %d",
                                        __func__, level, m_backendTextureId,
                                        textureMipmapObject->GetMipmapTexelSize(uploadTarget, level).x(),
                                        textureMipmapObject->GetMipmapTexelSize(uploadTarget, level).y(), byteSize);

                            auto glUploadTarget = MG_Util::ConvertTextureUploadTargetToGLEnum(uploadTarget);
                            g_GLESFuncs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line,
                                        MG_Util::ConvertGLEnumToString(err).c_str());
                            });
                            auto texelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                            g_GLESFuncs.glTexSubImage2D(glUploadTarget, static_cast<GLint>(level), 0, 0,
                                                        static_cast<GLsizei>(texelSize.x()),
                                                        static_cast<GLsizei>(texelSize.y()), glFormat, glType,
                                                        textureMipmapObject->MapMipmapData(uploadTarget, level));
                            textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                        }
                    }
                }
                break;
            }
            case TextureStorageType::Buffer: {
                auto* textureBufferObject =
                    static_cast<MG_State::GLState::TextureObjectBuffer*>(stateTextureObject.get());
                auto& slot = textureBufferObject->GetBufferBindingSlot();
                auto buffer = slot.GetBoundObject();
                auto bufferIndex = buffer->GetExternalIndex();
                currentTextureInfo.bufferExternalIndex = bufferIndex;

                Bool needsRegeneration = !m_isInitialized || (currentTextureInfo != m_prevTextureInfo);
                MGLOG_D("Texture state changed significantly or not initialized, regenerating texture (tex buffer) "
                        "with ID: %u",
                        m_backendTextureId);

                // Need to sync texture buffer if not synced yet
                auto& backendBuffers = BufferImpl::g_backendBufferObjects;
                SharedPtr<BufferImpl::BackendBufferObject> backendBufferObject;
                const auto& backendBufferIt = backendBuffers.find(buffer);
                if (backendBufferIt == backendBuffers.end()) {
                    backendBufferObject = MakeShared<BufferImpl::BackendBufferObject>();
                    backendBuffers[buffer] = backendBufferObject;
                } else {
                    backendBufferObject = backendBufferIt->second;
                }
                backendBufferObject->SyncToBackend(buffer);

                // Bind buffer to texture
                auto backendId = backendBufferObject->GetBackendBufferId();

                GLenum glInternalFormat, glType, glFormat;
                TextureImpl::GenerateTextureFormatInfo(textureBufferObject->GetFormat(), &glInternalFormat, &glFormat,
                                                       &glType);

                g_GLESFuncs.glTexBuffer(GL_TEXTURE_BUFFER, glInternalFormat, backendId);
                break;
            }
            default:
                THROW_UNIMPL_EXCEPTION;
            }

            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            m_prevTextureInfo = currentTextureInfo;
        }

        void BackendTextureObject::SyncBuiltinSamplerToBackend(
            SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            DebugImpl::ErrorLopper errorLopper;
            if (!stateTextureObject) {
                MGLOG_E("State texture object is null, cannot sync to backend.");
                return;
            }

            auto* samplerObject = stateTextureObject->GetSamplerObject().get();
            Uint currentSamplerVersion = samplerObject->GetVersion();
            if (m_syncedSamplerVersion == currentSamplerVersion) {
                MGLOG_D("Sampler parameters have not changed for texture ID: %u, skipping sync.", m_backendTextureId);
                return;
            }

            m_syncedSamplerVersion = currentSamplerVersion;

            MGLOG_D("Syncing texture built-in sampler with backend ID %u to backend for state ID %u",
                    m_backendTextureId, stateTextureObject->GetExternalIndex());

            GLenum target = MG_Util::ConvertTextureTargetToGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            Bind(target);
            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // Update built-in sampler parameters
            MGLOG_D("Updating sampler parameters for texture with ID: %u", m_backendTextureId);
            const auto& samplerParams = samplerObject->GetAllSamplerParameters();

#define SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(internalName, glName, type)                                                  \
    if (m_cacheSamplerParameters.internalName != samplerParams.internalName) {                                         \
        g_GLESFuncs.glTexParameteri(target, glName,                                                                    \
                                    MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName));              \
        m_cacheSamplerParameters.internalName = samplerParams.internalName;                                            \
        errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__,                                           \
                          t = MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName)](GLenum err) {       \
            MGLOG_D("%s(%s:%d) ES error %s, GL_TEXTURE_MIN_FILTER = %s", func, file, line,                             \
                    MG_Util::ConvertGLEnumToString(err).c_str(), MG_Util::ConvertGLEnumToString(t).c_str());           \
        });                                                                                                            \
    }

            if (m_cacheSamplerParameters.minFilter != samplerParams.minFilter ||
                m_cacheSamplerParameters.mipmapMode != samplerParams.mipmapMode) {
                g_GLESFuncs.glTexParameteri(
                    target, GL_TEXTURE_MIN_FILTER,
                    MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.minFilter, samplerParams.mipmapMode));
                m_cacheSamplerParameters.minFilter = samplerParams.minFilter;
                m_cacheSamplerParameters.mipmapMode = samplerParams.mipmapMode;
            }
            if (m_cacheSamplerParameters.magFilter != samplerParams.magFilter) {
                g_GLESFuncs.glTexParameteri(
                    target, GL_TEXTURE_MAG_FILTER,
                    MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.magFilter, SamplerMipmapMode::None));
                m_cacheSamplerParameters.magFilter = samplerParams.magFilter;
            }
            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapS, GL_TEXTURE_WRAP_S, WrapMode)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapT, GL_TEXTURE_WRAP_T, WrapMode)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapR, GL_TEXTURE_WRAP_R, WrapMode)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(compareFunc, GL_TEXTURE_COMPARE_FUNC, CompareFunc)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(compareMode, GL_TEXTURE_COMPARE_MODE, CompareMode)
            if (m_cacheSamplerParameters.minLod != samplerParams.minLod) {
                g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MIN_LOD, samplerParams.minLod);
                m_cacheSamplerParameters.minLod = samplerParams.minLod;
            }
            if (m_cacheSamplerParameters.maxLod != samplerParams.maxLod) {
                g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MAX_LOD, samplerParams.maxLod);
                m_cacheSamplerParameters.maxLod = samplerParams.maxLod;
            }
            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
#undef SYNC_TEX_SAMPLER_PARAM_IF_CHANGED
        }

        void BackendTextureObject::SyncTextureParamsToBackend(
            SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            DebugImpl::ErrorLopper errorLopper;
            if (!stateTextureObject) {
                MGLOG_E("State texture object is null, cannot sync to backend.");
                return;
            }

            Uint16 currentTextureParamsVersion = stateTextureObject->GetTextureParamsVersion();
            if (m_syncedTextureParamsVersion == currentTextureParamsVersion) {
                MGLOG_D("Texture parameters have not changed for texture ID: %u, skipping sync.", m_backendTextureId);
                return;
            }
            m_syncedTextureParamsVersion = currentTextureParamsVersion;

            MGLOG_D("Syncing texture params with backend ID %u to backend for state ID %u", m_backendTextureId,
                    stateTextureObject->GetExternalIndex());

            GLenum target = MG_Util::ConvertTextureTargetToGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            Bind(target);
            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // Update texture parameters
            MGLOG_D("Updating texture parameters for texture with ID: %u", m_backendTextureId);

            const auto& levelRange = stateTextureObject->GetLevelRange();

            if (m_cacheLodRange.x() != levelRange.x()) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, static_cast<GLint>(levelRange.x()));
                m_cacheLodRange.x() = levelRange.x();
            }
            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            if (m_cacheLodRange.y() != levelRange.y()) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levelRange.y()));
                m_cacheLodRange.y() = levelRange.y();
            }
            errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            const auto& swizzleParams = stateTextureObject->GetAllSwizzleParams();
            if (swizzleParams != m_cacheSwizzleParams) {
#define SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(func, glEnum)                                                                \
    if (m_cacheSwizzleParams.func != swizzleParams.func) {                                                             \
        g_GLESFuncs.glTexParameteri(target, glEnum, MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams.func));  \
        m_cacheSwizzleParams.func = swizzleParams.func;                                                                \
    }
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(r(), GL_TEXTURE_SWIZZLE_R);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(g(), GL_TEXTURE_SWIZZLE_G);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(b(), GL_TEXTURE_SWIZZLE_B);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(a(), GL_TEXTURE_SWIZZLE_A);
#undef SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED
                m_cacheSwizzleParams = swizzleParams;
                errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                    MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
                });
            }

            if (m_cacheBorderColor != stateTextureObject->GetBorderColor()) {
                const auto& borderColor = stateTextureObject->GetBorderColor();
                GLfloat borderColorArray[4] = {borderColor.x(), borderColor.y(), borderColor.z(), borderColor.w()};
                g_GLESFuncs.glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                m_cacheBorderColor = borderColor;
                errorLopper.Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                    MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
                });
            }
        }

        void ActivateTextureUnit(Uint unit) {
            if (unit == g_activeTextureUnit) {
                return;
            }
            g_GLESFuncs.glActiveTexture(GL_TEXTURE0 + unit);
            g_activeTextureUnit = unit;
        }

        void UnbindTexture(Uint unit, GLenum target) { // Active unit will be modified
            if (unit != g_activeTextureUnit) {
                ActivateTextureUnit(unit);
            }

            auto targetN = static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(target));
            if (g_boundTexturesCache[unit][targetN] == nullptr) return;

            g_GLESFuncs.glBindTexture(target, 0);
            g_boundTexturesCache[unit][targetN] = nullptr;
        }

        Uint g_activeTextureUnit = 0;
        Array<Array<BackendTextureObject*, (SizeT)TextureTarget::TextureTargetCount>,
              MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundTexturesCache;
        UnorderedMap<SharedPtr<MG_State::GLState::ITextureObject>, SharedPtr<BackendTextureObject>>
            g_backendTextureObjects;
    } // namespace TextureImpl

    namespace FramebufferImpl {
        BackendFramebufferObject::BackendFramebufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenFramebuffers(1, &m_backendFBOId);
            if (m_backendFBOId == 0) {
                MGLOG_E("Failed to generate framebuffer object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated framebuffer object with ID: %u.", m_backendFBOId);
            }
        }

        void BackendFramebufferObject::Bind(FramebufferTarget target) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (target == FramebufferTarget::Read)
                g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, m_backendFBOId);
            else
                g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_backendFBOId);
        }

        Bool BackendFramebufferObject::SyncAttachmentObject(
            GLenum glFBOTarget, const MG_State::GLState::FramebufferAttachmentObject& attachmentObject,
            GLenum glBackendAttachment) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                const auto& backendTextureIt = TextureImpl::g_backendTextureObjects.find(textureObject);
                if (backendTextureIt == TextureImpl::g_backendTextureObjects.end()) {
                    MGLOG_E("%s: No backend texture found for FBO attachment, cannot bind texture.", __func__);
                    return false;
                }
                const auto& backendTextureObject = backendTextureIt->second;
                auto glTextureTarget = MG_Util::ConvertTextureTargetToGLEnum(textureObject->GetTarget());
                backendTextureObject->Bind(glTextureTarget);
                g_GLESFuncs.glFramebufferTexture2D(glFBOTarget, glBackendAttachment, glTextureTarget,
                                                   backendTextureObject->GetBackendTextureId(),
                                                   static_cast<GLint>(attachmentObject.GetTextureLevel()));
            } else if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                const auto& backendRenderbufferIt =
                    RenderbufferImpl::g_backendRenderbufferObjects.find(renderbufferObject);
                SharedPtr<RenderbufferImpl::BackendRenderbufferObject> backendRenderbufferObject;
                if (backendRenderbufferIt == RenderbufferImpl::g_backendRenderbufferObjects.end()) {
                    backendRenderbufferObject = MakeShared<RenderbufferImpl::BackendRenderbufferObject>();
                    RenderbufferImpl::g_backendRenderbufferObjects[renderbufferObject] = backendRenderbufferObject;
                } else {
                    backendRenderbufferObject = backendRenderbufferIt->second;
                }

                backendRenderbufferObject->SyncToBackend(renderbufferObject);
                backendRenderbufferObject->Bind();
                g_GLESFuncs.glFramebufferRenderbuffer(glFBOTarget, glBackendAttachment, GL_RENDERBUFFER,
                                                      backendRenderbufferObject->GetBackendRenderbufferId());
            }
            return true;
        }

        void BackendFramebufferObject::SyncToBackend(SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject,
                                                     FramebufferTarget asTarget) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateFBOObject) {
                MGLOG_E("State FBO object is null, cannot sync to backend.");
                return;
            }
            MGLOG_D("Syncing FBO with backend ID %u to backend for state ID %u, as %s FBO", m_backendFBOId,
                    stateFBOObject->GetExternalIndex(), (asTarget == FramebufferTarget::Draw ? "DRAW" : "READ"));
            GLenum glFBOTarget = MG_Util::ConvertFramebufferTargetToGLEnum(asTarget);
            Bind(asTarget);

            // -------------------- Connect attachments (set buffers) -----------------------
            // 1. Remap draw buffers
            auto& stateDrawBuffers = stateFBOObject->GetDrawBuffers();
            Bool drawBufferClean = false;
            if (memcmp(m_frontendDrawBuffers, stateDrawBuffers.data(),
                       FramebufferObject::MAX_DRAW_BUFFERS * sizeof(FramebufferAttachmentType)) == 0) {
                drawBufferClean = true;
            }

            if (!drawBufferClean) {
                memcpy(m_frontendDrawBuffers, stateDrawBuffers.data(),
                       FramebufferObject::MAX_DRAW_BUFFERS * sizeof(FramebufferAttachmentType));
                std::fill(m_backendDrawBuffers, m_backendDrawBuffers + FramebufferObject::MAX_DRAW_BUFFERS, GL_NONE);
                int nEffectiveBuffers = 0;
                for (GLint i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
                    auto frontendBuf = stateDrawBuffers[i];
                    if (frontendBuf == FramebufferAttachmentType::None) {
                        m_backendDrawBuffers[i] = GL_NONE;
                        continue;
                    }

                    // Create compacted mapping
                    if (frontendBuf == FramebufferAttachmentType::FrontLeft ||
                        frontendBuf == FramebufferAttachmentType::FrontRight ||
                        frontendBuf == FramebufferAttachmentType::BackLeft ||
                        frontendBuf == FramebufferAttachmentType::BackRight) {
                        MGLOG_D("%s: frontend buf token found for default fbo, shouldn't remap", __func__);
                        m_backendDrawBuffers[i] = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendBuf);
                    } else {
                        m_backendDrawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
                    }
                    nEffectiveBuffers = i + 1;
                }
                g_GLESFuncs.glDrawBuffers(nEffectiveBuffers, m_backendDrawBuffers);
            }

            // 2. Remap read buffer
            auto frontendReadBuf = stateFBOObject->GetReadBuffer();
            if (frontendReadBuf != m_frontendReadBuffer) {
                m_frontendReadBuffer = frontendReadBuf;

                GLenum glBackendReadBuffer = GetBackendAttachmentType(frontendReadBuf);

                if (m_backendReadBuffer != glBackendReadBuffer) {
                    m_backendReadBuffer = glBackendReadBuffer;
                    g_GLESFuncs.glReadBuffer(glBackendReadBuffer);
                }
            }

            // -------------------- Attach texture to backend FBO -----------------------
            const auto& attachments = stateFBOObject->GetAllAttachmentObjects();
            const auto& attachmentVersions = stateFBOObject->GetAllFramebufferAttachmentVersions();
            for (SizeT i = 0; i < attachments.size(); ++i) {
                const auto& attachmentObject = attachments[i];
                FramebufferAttachmentType frontendType = static_cast<FramebufferAttachmentType>(i);
                GLenum glBackendAttachment = GL_NONE;
                if (frontendType >= FramebufferAttachmentType::Color0 &&
                    frontendType <= FramebufferAttachmentType::Color31)
                    glBackendAttachment = GetBackendAttachmentType(frontendType);
                else
                    glBackendAttachment = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendType);

                // relevant FRONTEND!!! version should be checked and updated
                if (m_syncedFrontendAttachmentVersions[i] != attachmentVersions[i]) {
                    SyncAttachmentObject(glFBOTarget, attachmentObject, glBackendAttachment);
                    m_syncedFrontendAttachmentVersions[i] = attachmentVersions[i];
                }
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
                else {
                    MGLOG_D("%s: Skipped SyncAttachmentObject(target=%s, frontendObj=(%dx%dx%d, %s), backendAtt=%s), "
                            "version = %u",
                            __func__, MG_Util::ConvertGLEnumToString(glFBOTarget).c_str(),
                            attachmentObject.GetSize().x(), attachmentObject.GetSize().y(),
                            attachmentObject.GetSize().z(),
                            MG_Util::ConvertFramebufferAttachmentTypeToString(frontendType).c_str(),
                            MG_Util::ConvertGLEnumToString(glBackendAttachment).c_str(),
                            m_syncedFrontendAttachmentVersions[i]);
                    GLint objectType = GL_NONE;
                    g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                        glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
                    MOBILEGL_ASSERT((objectType == GL_NONE) ||
                                        (attachmentObject.IsTexture() && objectType == GL_TEXTURE) ||
                                        (attachmentObject.IsRenderbuffer() && objectType == GL_RENDERBUFFER),
                                    "Attachment type not match!");
                    GLint objectName = 0;
                    g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                        glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objectName);
                    // Verify that the backend object's name and parameters match the frontend attachment state
                    if (attachmentObject.IsTexture()) {
                        const auto& textureObject = attachmentObject.GetTexture();
                        auto backendTextureIt = TextureImpl::g_backendTextureObjects.find(textureObject);
                        MOBILEGL_ASSERT(backendTextureIt != TextureImpl::g_backendTextureObjects.end(),
                                        "No backend texture found while framebuffer reports texture attachment.");
                        GLuint backendTexId = backendTextureIt->second->GetBackendTextureId();
                        MOBILEGL_ASSERT(static_cast<GLint>(backendTexId) == objectName,
                                        "Attachment texture name mismatch between GLES (%d) and backend texture object "
                                        "(%d), frontend texture object ID=%d.",
                                        objectName, backendTexId, textureObject->GetExternalIndex());

                        GLint texLevel = 0;
                        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                            glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &texLevel);
                        MOBILEGL_ASSERT(texLevel == static_cast<GLint>(attachmentObject.GetTextureLevel()),
                                        "Attachment texture level mismatch between GLES and state object.");
                    } else if (attachmentObject.IsRenderbuffer()) {
                        const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                        auto backendRboIt = RenderbufferImpl::g_backendRenderbufferObjects.find(renderbufferObject);
                        MOBILEGL_ASSERT(
                            backendRboIt != RenderbufferImpl::g_backendRenderbufferObjects.end(),
                            "No backend renderbuffer found while framebuffer reports renderbuffer attachment.");
                        GLuint backendRboId = backendRboIt->second->GetBackendRenderbufferId();
                        MOBILEGL_ASSERT(static_cast<GLint>(backendRboId) == objectName,
                                        "Attachment renderbuffer name mismatch between GLES and state object.");
                    }
                }
#endif
            }
        }

        GLenum BackendFramebufferObject::GetBackendAttachmentType(FramebufferAttachmentType frontendAtt) const {
            GLenum glBackendReadBuffer = GL_NONE;
            auto it = std::find(m_frontendDrawBuffers, m_frontendDrawBuffers + FramebufferObject::MAX_DRAW_BUFFERS,
                                frontendAtt);
            Bool notFound = (it == m_frontendDrawBuffers + FramebufferObject::MAX_DRAW_BUFFERS);
            if (notFound) {
                MGLOG_D(
                    "%s: frontendAtt not found in draw buffer (probably not remapped), just use the same as frontend",
                    __func__);
                glBackendReadBuffer = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendAtt);
            } else {
                MGLOG_D("%s: frontendAtt found in draw buffer, keep it consistent as in read buffers", __func__);
                auto index = std::distance(m_frontendDrawBuffers, it);
                glBackendReadBuffer = m_backendDrawBuffers[index];
            }
            return glBackendReadBuffer;
        }

        UnorderedMap<SharedPtr<MG_State::GLState::FramebufferObject>, SharedPtr<BackendFramebufferObject>>
            g_backendFramebufferObjects;
        Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboBindVersions = {0};
    } // namespace FramebufferImpl

    namespace PrgramImpl {
        UnorderedMap<SharedPtr<MG_State::GLState::ProgramObject>, SharedPtr<BackendProgramObjectImpl>>
            g_backendProgramObjects;

        BackendProgramObjectImpl::BackendProgramObjectImpl() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            m_backendProgramId = g_GLESFuncs.glCreateProgram();
            if (m_backendProgramId == 0) {
                MGLOG_E("Failed to create program object in backend.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());

            } else {
                MGLOG_D("Created backend program object with ID: %u", m_backendProgramId);
            }
        }

        BackendProgramObjectImpl::~BackendProgramObjectImpl() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (m_backendProgramId != 0) {
                MGLOG_D("Deleting backend program object with ID: %u", m_backendProgramId);
                g_GLESFuncs.glDeleteProgram(m_backendProgramId);
            }
        }

        void BackendProgramObjectImpl::SyncToBackend(SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateProgramObject) {
                MGLOG_E("State program object is null, skipping backend sync.");
                return;
            }

            if (!stateProgramObject->GetLinkStatus()) {
                MGLOG_E("Program object is not linked, skipping backend sync. State program ID: %u",
                        stateProgramObject->GetExternalIndex());
                return;
            }

            MGLOG_D("Syncing program to backend. State program ID: %u, Backend ID: %u",
                    stateProgramObject->GetExternalIndex(), m_backendProgramId);

            // Detach all existing shaders
            GLint attachedCount = 0;
            g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_ATTACHED_SHADERS, &attachedCount);
            MGLOG_D("Currently attached shaders count: %d", attachedCount);

            if (attachedCount > 0) {
                Vector<GLuint> attachedShaders(attachedCount);
                GLsizei actualCount;
                g_GLESFuncs.glGetAttachedShaders(m_backendProgramId, attachedCount, &actualCount,
                                                 attachedShaders.data());
                MGLOG_D("Detaching %d existing shaders from program %u", actualCount, m_backendProgramId);

                for (GLsizei i = 0; i < actualCount; ++i) {
                    MGLOG_D("Detaching shader ID: %u from program %u", attachedShaders[i], m_backendProgramId);
                    g_GLESFuncs.glDetachShader(m_backendProgramId, attachedShaders[i]);
                }
            }

            // Attach current shaders
            auto& attachedShaders = stateProgramObject->GetAttachedShaders();
            MGLOG_D("Attaching %zu shaders to program %u", attachedShaders.size(), m_backendProgramId);
            for (auto& shader : attachedShaders) {
                const auto& src = shader->GetShaderSource();
                const auto& stage =
                    MG_Util::ConvertGLEnumToString(MG_Util::ConvertShaderStageToGLEnum(shader->GetShaderStage()));
                MGLOG_D("Original src @ %s: \n", stage.c_str());
                MGLOG_D("%s:", src.empty() ? "" : src.c_str());
            }
            auto& shaderSpirvs = stateProgramObject->GetGeneratedSpirv();

            for (int index = 0; index < attachedShaders.size(); ++index) {
                auto& shader = attachedShaders[index];
                GLenum glShaderType = MG_Util::ConvertShaderStageToGLEnum(shader->GetShaderStage());
                GLuint backendShaderId = g_GLESFuncs.glCreateShader(glShaderType);

                if (backendShaderId == 0) {
                    MGLOG_E("Failed to create backend shader for attachment.");
                    continue;
                }
                String source;
                auto& spirvCode = shaderSpirvs[index];

                MG_Util::ShaderTranspiler::SpvcSession spvcSession(spirvCode);

                spvc_compiler_options options;
                spvcSession.CreateOptions(&options);

                // TODO: check ESSL version supported by backend driver
                spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
                spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
                spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

                spvcSession.SetOptions(options);

                const char* result = nullptr;
                spvcSession.Compile(&result);

                if (!result) {
                    MG_Util::ShaderTranspiler::ResultInfo r;
                    r.log += "Failed to compile the shader to GLSL: \n";
                    r.log += spvcSession.GetLastErrorString();
                    r.errc = -5;
                    MGLOG_E("%s", r.log.c_str());
                    continue;
                }

                source = result;

                source = RemoveLayoutBinding(source);
                source = ProcessOutColorLocations(source);
                source = ForceSupporterOutput(source);

                // Patch for Photon compiler precision issue
                String findStr = "1000000.0";
                String replaceStr = "65500.0";
                auto pos = source.find(findStr);
                while (pos != String::npos) {
                    MGLOG_D("Applying patch #2 to Photon...");
                    source.replace(pos, findStr.length(), replaceStr);
                    pos = source.find(findStr, pos);
                }

                const char* sourceCStr = source.c_str();
                MGLOG_D("Setting shader source for backend shader ID: %u\nsrc:\n%s", backendShaderId, sourceCStr);
                g_GLESFuncs.glShaderSource(backendShaderId, 1, &sourceCStr, nullptr);
                g_GLESFuncs.glCompileShader(backendShaderId);

                GLint compileStatus;
                g_GLESFuncs.glGetShaderiv(backendShaderId, GL_COMPILE_STATUS, &compileStatus);
                if (compileStatus == GL_FALSE) {
                    GLint logLength;
                    g_GLESFuncs.glGetShaderiv(backendShaderId, GL_INFO_LOG_LENGTH, &logLength);
                    Vector<GLchar> log(logLength);
                    g_GLESFuncs.glGetShaderInfoLog(backendShaderId, logLength, nullptr, log.data());
                    MGLOG_E("Shader compilation failed for backend ID %u: %s", backendShaderId, log.data());
                    continue;
                }

                MGLOG_D("Attaching shader ID: %u to program %u", backendShaderId, m_backendProgramId);
                g_GLESFuncs.glAttachShader(m_backendProgramId, backendShaderId);

                MGLOG_D("Processed shader source length: %zu", source.length());
            }

            // Link program
            MGLOG_D("Linking program %u", m_backendProgramId);
            g_GLESFuncs.glLinkProgram(m_backendProgramId);

            GLint linkStatus;
            g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_LINK_STATUS, &linkStatus);
            if (linkStatus != GL_TRUE) {
                GLint logLength;
                g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_INFO_LOG_LENGTH, &logLength);
                Vector<GLchar> log(logLength);
                g_GLESFuncs.glGetProgramInfoLog(m_backendProgramId, logLength, nullptr, log.data());
                MGLOG_E("Program %u linking failed for %u: %s", stateProgramObject->GetExternalIndex(),
                        m_backendProgramId, log.data());
            } else {
                MGLOG_D("Program linked successfully. ID: %u", m_backendProgramId);
            }

            // Create global UBO
            if (stateProgramObject->GetUBOSize() > 0) {
                g_GLESFuncs.glGenBuffers(1, &m_backendGlobalUBOId);
                g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, m_backendGlobalUBOId);
                g_GLESFuncs.glBufferData(GL_UNIFORM_BUFFER, stateProgramObject->GetUBOSize(), nullptr, GL_STREAM_DRAW);
                g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, 0);
            } else {
                m_backendGlobalUBOId = 0;
            }

            m_isInitialized = true;
            MGLOG_D("Program sync completed. backend ID %u", m_backendProgramId);
        }

        void BackendProgramObjectImpl::Use() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            MGLOG_D("Using program %u", m_backendProgramId);
            g_GLESFuncs.glUseProgram(m_backendProgramId);
        }
    } // namespace PrgramImpl

    namespace SamplerImpl {
        BackendSamplerObject::BackendSamplerObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenSamplers(1, &m_backendSamplerId);
            if (m_backendSamplerId == 0) {
                MGLOG_E("Failed to generate sampler object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated sampler object with ID: %u.", m_backendSamplerId);
            }
        }

        void BackendSamplerObject::SyncToBackend(SharedPtr<MG_State::GLState::SamplerObject>& stateSamplerObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateSamplerObject) {
                MGLOG_E("State sampler object is null, cannot sync to backend.");
                return;
            }

            Uint currentSamplerVersion = stateSamplerObject->GetVersion();
            if (m_isInitialized && m_syncedSamplerVersion == currentSamplerVersion) {
                MGLOG_D("Sampler parameters have not changed for sampler ID: %u, skipping sync.",
                        stateSamplerObject->GetExternalIndex());
                return;
            }

            m_syncedSamplerVersion = currentSamplerVersion;

            MGLOG_D("Syncing sampler with backend ID %u to backend for state ID %u", m_backendSamplerId,
                    stateSamplerObject->GetExternalIndex());

            const auto& samplerParams = stateSamplerObject->GetAllSamplerParameters();

#define SYNC_SAMPLER_PARAM_IF_CHANGED(internalName, glName, type)                                                      \
    if (m_cacheSamplerParameters.internalName != samplerParams.internalName) {                                         \
        g_GLESFuncs.glSamplerParameteri(m_backendSamplerId, glName,                                                    \
                                        MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName));          \
        m_cacheSamplerParameters.internalName = samplerParams.internalName;                                            \
    }

            if (m_cacheSamplerParameters.minFilter != samplerParams.minFilter ||
                m_cacheSamplerParameters.mipmapMode != samplerParams.mipmapMode) {
                g_GLESFuncs.glSamplerParameteri(
                    m_backendSamplerId, GL_TEXTURE_MIN_FILTER,
                    MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.minFilter, samplerParams.mipmapMode));
                m_cacheSamplerParameters.minFilter = samplerParams.minFilter;
                m_cacheSamplerParameters.mipmapMode = samplerParams.mipmapMode;
            }
            if (m_cacheSamplerParameters.magFilter != samplerParams.magFilter) {
                g_GLESFuncs.glSamplerParameteri(
                    m_backendSamplerId, GL_TEXTURE_MAG_FILTER,
                    MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.magFilter, SamplerMipmapMode::None));
                m_cacheSamplerParameters.magFilter = samplerParams.magFilter;
            }

            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapS, GL_TEXTURE_WRAP_S, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapT, GL_TEXTURE_WRAP_T, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapR, GL_TEXTURE_WRAP_R, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(compareFunc, GL_TEXTURE_COMPARE_FUNC, CompareFunc)
            SYNC_SAMPLER_PARAM_IF_CHANGED(compareMode, GL_TEXTURE_COMPARE_MODE, CompareMode)
            if (m_cacheSamplerParameters.minLod != samplerParams.minLod) {
                g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MIN_LOD, samplerParams.minLod);
                m_cacheSamplerParameters.minLod = samplerParams.minLod;
            }
            if (m_cacheSamplerParameters.maxLod != samplerParams.maxLod) {
                g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MAX_LOD, samplerParams.maxLod);
                m_cacheSamplerParameters.maxLod = samplerParams.maxLod;
            }
#undef SYNC_SAMPLER_PARAM_IF_CHANGED
            m_isInitialized = true;
        }

        void BackendSamplerObject::Bind(Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (g_boundSamplersCache[unit] == this) return;

            g_GLESFuncs.glBindSampler(static_cast<GLenum>(unit), m_backendSamplerId);
            g_boundSamplersCache[unit] = this;
        }

        Uint BackendSamplerObject::GetBackendSamplerId() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            return m_backendSamplerId;
        }

        void UnbindSampler(Uint unit) {
            if (g_boundSamplersCache[unit] == nullptr) return;

            g_GLESFuncs.glBindSampler(static_cast<GLenum>(unit), 0);
            g_boundSamplersCache[unit] = nullptr;
        }

        Array<BackendSamplerObject*, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS> g_boundSamplersCache;
        UnorderedMap<SharedPtr<MG_State::GLState::SamplerObject>, SharedPtr<BackendSamplerObject>>
            g_backendSamplerObjects;
    } // namespace SamplerImpl

    namespace RenderbufferImpl {
        BackendRenderbufferObject::BackendRenderbufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenRenderbuffers(1, &m_backendRBOId);
            if (m_backendRBOId == 0) {
                MGLOG_E("Failed to generate renderbuffer object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            }
        }

        void BackendRenderbufferObject::Bind() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, m_backendRBOId);
        }

        void BackendRenderbufferObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::RenderbufferObject>& stateRBOObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateRBOObject) {
                MGLOG_E("State RBO object is null, cannot sync to backend.");
                return;
            }

            MGLOG_D("Syncing RBO with backend ID %u to backend for state ID %u", m_backendRBOId,
                    stateRBOObject->GetExternalIndex());

            if (m_isInitialized && m_cacheInternalFormat == stateRBOObject->GetInternalFormat() &&
                m_cacheWidth == stateRBOObject->GetWidth() && m_cacheHeight == stateRBOObject->GetHeight()) {
                MGLOG_D("RBO %u already initialized with matching parameters, skipping re-allocation.",
                        stateRBOObject->GetExternalIndex());
                return;
            }

            Bind();

            // Allocate storage
            TextureInternalFormat internalFormat = stateRBOObject->GetInternalFormat();
            Int width = static_cast<Int>(stateRBOObject->GetWidth());
            Int height = static_cast<Int>(stateRBOObject->GetHeight());
            GLenum glInternalFormat, glType, glFormat;
            TextureImpl::GenerateTextureFormatInfo(internalFormat, &glInternalFormat, &glFormat, &glType);

            g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, glInternalFormat, static_cast<GLsizei>(width),
                                              static_cast<GLsizei>(height));

            m_cacheInternalFormat = internalFormat;
            m_cacheWidth = width;
            m_cacheHeight = height;

            m_isInitialized = true;
            MGLOG_D("RBO %u sync completed. backend ID %u", stateRBOObject->GetExternalIndex(), m_backendRBOId);
        }

        UnorderedMap<SharedPtr<MG_State::GLState::RenderbufferObject>, SharedPtr<BackendRenderbufferObject>>
            g_backendRenderbufferObjects;
    } // namespace RenderbufferImpl

    namespace Utils {} // namespace Utils
} // namespace MobileGL::MG_Backend::DirectGLES
