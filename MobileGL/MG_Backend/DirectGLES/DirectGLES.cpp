// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLES.h"
#include "EGL/egl.h"
#include "MG_Util/Types.h"
#include "Utils.h"
#include "Managers.h"
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Classifiers/TextureEnumClassifier.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/RenderStateEnumConverter.h>
#include <MG_Util/Math/HalfFloat.h>
#include <MG_Util/Metrics/BufferMetrics.h>
#include <MG_Util/Texture/PixelStoreProcessor.h>
#include <Config.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#if defined(__linux__) && !defined(__ANDROID__) && __has_include(<X11/Xlib.h>)
#pragma push_macro("Bool")
#pragma push_macro("None")
#include <X11/Xlib.h>
#pragma pop_macro("None")
#pragma pop_macro("Bool")
#endif

namespace MobileGL::MG_Backend::DirectGLES {
    MG_External::EGLFunctionsTable g_EGLFuncs;
    MG_External::GLESFunctionsTable g_GLESFuncs;
    MG_External::GLESCapabilities g_GLESCapabilities;

    static Bool QueryCurrentSurfaceSize(Int& outWidth, Int& outHeight);
    static SharedPtr<MG_State::GLState::SamplerObject> g_rawDepthFetchSamplerState;
    static SharedPtr<SamplerImpl::BackendSamplerObject> g_rawDepthFetchSamplerBackend;

    static Bool IsDualSourceBlendFactor(BlendFactor v) {
        switch (v) {
        case BlendFactor::Src1Color:
        case BlendFactor::OneMinusSrc1Color:
        case BlendFactor::Src1Alpha:
        case BlendFactor::OneMinusSrc1Alpha:
            return true;
        default:
            return false;
        }
    }

    enum class DrawSyncBit : Uint32 {
        None = 0,
        IndexBuffer = 1 << 0,
        IndirectBuffer = 1 << 1,
        Instancing = 1 << 2
    };

    inline DrawSyncBit operator|(DrawSyncBit a, DrawSyncBit b) {
        return static_cast<DrawSyncBit>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline DrawSyncBit& operator|=(DrawSyncBit& a, DrawSyncBit b) {
        a = a | b;
        return a;
    }

    struct DrawElementsIndirectCommand {
        Uint32 count = 0;
        Uint32 instanceCount = 0;
        Uint32 firstIndex = 0;
        Int32 baseVertex = 0;
        Uint32 baseInstance = 0;
    };

    struct DrawArraysIndirectCommand {
        Uint32 count = 0;
        Uint32 instanceCount = 0;
        Uint32 first = 0;
        Uint32 baseInstance = 0;
    };

    SamplerImpl::BackendSamplerObject* GetRawDepthFetchSampler() {
        if (!g_rawDepthFetchSamplerState) {
            g_rawDepthFetchSamplerState = MakeShared<MG_State::GLState::SamplerObject>(0);
            g_rawDepthFetchSamplerState->SetMinFilter(SamplerFilterMode::Nearest);
            g_rawDepthFetchSamplerState->SetMagFilter(SamplerFilterMode::Nearest);
            g_rawDepthFetchSamplerState->SetMipmapMode(SamplerMipmapMode::None);
            g_rawDepthFetchSamplerState->SetCompareMode(SamplerCompareMode::None);
            g_rawDepthFetchSamplerState->SetSamplerCompareFunc(SamplerCompareFunc::Always);
            g_rawDepthFetchSamplerBackend = MakeShared<SamplerImpl::BackendSamplerObject>();
        }
        g_rawDepthFetchSamplerBackend->SyncToBackend(g_rawDepthFetchSamplerState);
        return g_rawDepthFetchSamplerBackend.get();
    }

    Bool NeedsRawDepthFetchSampler(const SharedPtr<MG_State::GLState::SamplerObject>& samplerObject,
                                   TextureInternalFormat textureFormat) {
        if (!MG_Util::IsDepthFormatInternalFormat(textureFormat) || !samplerObject) {
            return false;
        }

        const auto& samplerParams = samplerObject->GetAllSamplerParameters();
        if (samplerParams.compareMode != SamplerCompareMode::None) {
            return false;
        }

        return samplerParams.minFilter != SamplerFilterMode::Nearest ||
               samplerParams.mipmapMode != SamplerMipmapMode::None ||
               samplerParams.magFilter != SamplerFilterMode::Nearest;
    }

    const Uint8* ResolveIndirectCommandBytes(const void* indirect, SizeT requiredBytes, const char* label) {
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (drawBuffer) {
            drawBuffer->SyncPersistentMappedRange();
            const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
            if (commandOffset + requiredBytes > drawBuffer->GetSize()) {
                MGLOG_E("%s skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range", label);
                return nullptr;
            }
            return drawBuffer->MappedData() + commandOffset;
        }

        if (!indirect) {
            MGLOG_E("%s skipped: indirect pointer is null", label);
            return nullptr;
        }

        return reinterpret_cast<const Uint8*>(indirect);
    }

    namespace DebugImpl {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        void ErrorLopper::Loop(const std::function<void(GLenum)>& func) {
            GLenum err = g_GLESFuncs.glGetError();
            while (err != GL_NO_ERROR) {
                func(err);
                err = g_GLESFuncs.glGetError();
            }
        }

        void ErrorLopper::Clear() {
            GLenum err = g_GLESFuncs.glGetError();
            while (err != GL_NO_ERROR) {
                MGLOG_D("Stray GL Error cleared: %s", MG_Util::ConvertGLEnumToString(err).c_str());
                err = g_GLESFuncs.glGetError();
            }
        }

        ErrorLopper::ErrorLopper() {
            Clear();
        }
        ErrorLopper::~ErrorLopper() {
            Clear();
        }
#else
        void ErrorLopper::Loop(const std::function<void(GLenum)>& func) {}
        void ErrorLopper::Clear() {}
        ErrorLopper::ErrorLopper() = default;
        ErrorLopper::~ErrorLopper() = default;
#endif

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        OpenGLScopeMarker::OpenGLScopeMarker(const String& scopeName) {
            g_GLESFuncs.glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, scopeName.c_str());
        }

        OpenGLScopeMarker::~OpenGLScopeMarker() {
            g_GLESFuncs.glPopDebugGroup();
        }
#else
        OpenGLScopeMarker::OpenGLScopeMarker(const String& scopeName) {}

        OpenGLScopeMarker::~OpenGLScopeMarker() {}
#endif
    } // namespace DebugImpl

    // TODO: deletion for deleted objects

    namespace BufferImpl {
        void CreateAndSyncBufferObject(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject) {
            // Immediate BufferBackendOps keep existing storage current; this only
            // needs to materialize the resource (and replay pending ops).
            EnsureBufferResource(bufferObject);
        }

        void SyncBufferBindingPoints(BufferTarget target, GLenum glTarget) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // Only sync up to the high-water mark of app-touched points; the fixed array is 36
            // deep but apps bind a handful, so the never-touched tail is already at GL default 0.
            auto bindingPointCnt = MG_State::pGLContext->GetTouchedBufferBindingPointCount(target);
            for (SizeT i = 0; i < bindingPointCnt; ++i) {
                auto& point = MG_State::pGLContext->GetBufferBindingPoint(target, i);
                auto& obj = point.GetBoundObject();
                if (!obj) {
                    BindBufferBaseCached(glTarget, static_cast<GLuint>(i), 0);
                    continue;
                }

                auto* backendResource = EnsureBufferResource(obj);
                if (!backendResource || backendResource->id == 0) {
                    MGLOG_E("No backend buffer found for %s binding point %zu.",
                            MG_Util::ConvertGLEnumToString(glTarget).c_str(), i);
                    continue;
                }

                const auto& range = point.GetRange();
                auto backendBufferId = backendResource->id;
                if (range.start == 0 && range.end >= obj->GetSize()) {
                    BindBufferBaseCached(glTarget, static_cast<GLuint>(i), backendBufferId);
                } else {
                    const auto start = std::min(range.start, obj->GetSize());
                    const auto end = std::min(range.end, obj->GetSize());
                    BindBufferRangeCached(glTarget, static_cast<GLuint>(i), backendBufferId,
                                          static_cast<GLintptr>(start), static_cast<GLsizeiptr>(end - start));
                }
            }
        }

        void SyncBoundBuffer(BufferTarget target, GLenum glTarget) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            auto& bufferObject = MG_State::pGLContext->GetBufferBindingSlot(target).GetBoundObject();
            if (!bufferObject) {
                g_GLESFuncs.glBindBuffer(glTarget, 0);
                return;
            }

            auto* backendResource = EnsureBufferResource(bufferObject);
            if (!backendResource || backendResource->id == 0) {
                MGLOG_E("No backend buffer found for %s.", MG_Util::ConvertGLEnumToString(glTarget).c_str());
                return;
            }
            BindBufferId(glTarget, backendResource->id);
        }

        void SyncNeccessaryBuffers(Bool includeIBO = false, Bool includeIndirectBuffer = false) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            ProcessDeferredBufferReleases();

            // All buffers we need are:
            //   1.VBO 2.IBO (if needed) 3.UBO 4.IndirectBuffer (if needed)
            // PBO is not needed since it should be handled in frontend

            const auto& currentVAOObject = MG_State::pGLContext->GetBoundVertexArray();
            if (!currentVAOObject) {
                MGLOG_E("No VAO is currently bound, cannot sync necessary buffers.");
                return;
            }

            // VBO
            for (const auto& attrib : currentVAOObject->GetAllAttributes()) {
                if (!attrib.Enabled) continue;
                auto& bufferObject = attrib.Buffer;
                if (bufferObject) {
                    CreateAndSyncBufferObject(bufferObject);
                }
            }

            // IBO
            if (includeIBO) {
                auto& possibleIBO = currentVAOObject->GetIndexBufferBindingSlot().GetBoundObject();
                if (possibleIBO) {
                    CreateAndSyncBufferObject(possibleIBO);
                }
            }

            // Indirect Buffer Object - must also be bound to GL_DRAW_INDIRECT_BUFFER on the ES
            // context since indirect draws now execute natively on the GPU.
            if (includeIndirectBuffer) {
                auto& possibleIndirectBuffer =
                    MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
                if (possibleIndirectBuffer) {
                    SyncBoundBuffer(BufferTarget::DrawIndirect, GL_DRAW_INDIRECT_BUFFER);
                }
            }

            // UBO binding points are (re)established per draw by BindCurrentProgramWithResources at
            // their compacted link-time points: CacheResourceLocations glUniformBlockBinding's the
            // transpiled ESSL blocks to points 0,1,2,... (layout(binding=N) is stripped from the
            // ESSL), so those compacted points are the only ones the shader reads. A frontend-indexed
            // sync here would bind points the shader never reads and is unconditionally overwritten by
            // the program rebind that always follows in PrepareForDraw - i.e. redundant for draws - so
            // it is intentionally omitted (see SyncComputeBuffers for the compute path, which needs it).
            // SSBOs are different: their block bindings are baked into the ESSL at compile time and
            // BindCurrentProgramWithResources binds no SSBO points, so this is their sole draw-path
            // binder (e.g. Flywheel's indirect vertex shaders pull instance data from storage buffers).
            SyncBufferBindingPoints(BufferTarget::ShaderStorage, GL_SHADER_STORAGE_BUFFER);
        }

        void SyncComputeBuffers(Bool includeDispatchIndirectBuffer) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            ProcessDeferredBufferReleases();
            SyncBufferBindingPoints(BufferTarget::Uniform, GL_UNIFORM_BUFFER);
            SyncBufferBindingPoints(BufferTarget::ShaderStorage, GL_SHADER_STORAGE_BUFFER);
            if (includeDispatchIndirectBuffer) {
                SyncBoundBuffer(BufferTarget::DispatchIndirect, GL_DISPATCH_INDIRECT_BUFFER);
            }
        }
    } // namespace BufferImpl

    // Transform feedback is captured by the real ES driver: the backend program
    // declares the capture set at link time (see BackendProgramObjectImpl::SyncToBackend)
    // and the span below wraps the driver's own glBeginTransformFeedback/glEndTransformFeedback.
    //
    // The driver-side Begin is deferred from the frontend's glBeginTransformFeedback to
    // the first draw of the span: ES requires the capturing program to be current and
    // the capture buffers bound when Begin is issued, and both of those only become true
    // once PrepareForDraw has run. A span that never draws therefore never touches the
    // driver at all, which is also what the GL semantics amount to.
    namespace XfbImpl {
        namespace {
            struct XfbCaptureTarget {
                SharedPtr<MG_State::GLState::BufferObject> buffer;
                Uint backendId = 0;
                SizeT start = 0;
                SizeT end = 0;
            };

            Bool g_xfbPending = false; // frontend Begin seen, driver capture not started yet
            Bool g_xfbStarted = false; // driver capture running
            GLenum g_xfbPrimitiveMode = GL_POINTS;
            Vector<XfbCaptureTarget> g_xfbTargets;
        } // namespace

        Bool AreTransformFeedbacksSupported() {
            return g_GLESFuncs.glBeginTransformFeedback != nullptr &&
                   g_GLESFuncs.glEndTransformFeedback != nullptr &&
                   g_GLESFuncs.glTransformFeedbackVaryings != nullptr;
        }

        void BeginTransformFeedback(GLenum primitiveMode) {
            if (!AreTransformFeedbacksSupported()) return;
            g_xfbPrimitiveMode = primitiveMode;
            g_xfbPending = true;
            g_xfbStarted = false;
            g_xfbTargets.clear();
        }

        // Tail of PrepareForDraw: the program is bound and every buffer the draw needs
        // is up to date, so the capture buffers can be bound and the span opened.
        void StartPendingTransformFeedback() {
            if (!g_xfbPending) return;
            g_xfbPending = false;
            const auto& program = MG_State::pGLContext->GetTransformFeedbackProgram();
            if (!program) return;

            // Snapshot what the driver is about to capture into. GL forbids rebinding the
            // capture buffers while the span is open, so this stays valid until End, and
            // recording it here keeps End independent of the frontend capture state.
            const SizeT bufferCount = program->GetTransformFeedbackBufferCount();
            for (SizeT i = 0; i < bufferCount; ++i) {
                auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::TransformFeedback,
                                                                          static_cast<Uint>(i));
                const auto& bufferObject = point.GetBoundObject();
                if (!bufferObject) continue;
                auto* backendResource = BufferImpl::EnsureBufferResource(bufferObject);
                if (!backendResource || backendResource->id == 0) continue;
                const Range1D range = point.GetRange();
                const SizeT start = std::min(range.start, bufferObject->GetSize());
                const SizeT end = std::min(range.end, bufferObject->GetSize());
                if (end <= start) continue;
                g_xfbTargets.push_back({bufferObject, backendResource->id, start, end});
            }

            BufferImpl::SyncBufferBindingPoints(BufferTarget::TransformFeedback, GL_TRANSFORM_FEEDBACK_BUFFER);
            g_GLESFuncs.glBeginTransformFeedback(g_xfbPrimitiveMode);
            g_xfbStarted = true;
        }

        void EndTransformFeedback() {
            g_xfbPending = false;
            if (!g_xfbStarted) return;
            g_xfbStarted = false;
            g_GLESFuncs.glEndTransformFeedback();

            // The GPU wrote the capture buffers behind the frontend's back, so the CPU
            // shadows that back MapBuffer/GetBufferSubData still hold the pre-draw bytes.
            // Mirror the captured ranges into them. Buffers whose storage the backend
            // already owns (coherent persistent map) need nothing: reads resolve against
            // that storage directly.
            if (g_GLESFuncs.glMapBufferRange != nullptr && g_GLESFuncs.glUnmapBuffer != nullptr) {
                for (const auto& target : g_xfbTargets) {
                    if (!target.buffer || target.buffer->IsBackendPersistentMapped()) continue;
                    const SizeT size = target.end - target.start;
                    BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, target.backendId);
                    void* mapped = g_GLESFuncs.glMapBufferRange(BufferImpl::TempBufferTarget,
                                                                static_cast<GLintptr>(target.start),
                                                                static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
                    if (mapped == nullptr) {
                        MGLOG_E("EndTransformFeedback: failed to map backend buffer %u for capture readback",
                                target.backendId);
                        continue;
                    }
                    target.buffer->WritebackFromBackend({mapped, size}, target.start);
                    g_GLESFuncs.glUnmapBuffer(BufferImpl::TempBufferTarget);
                }
            }
            g_xfbTargets.clear();
        }

        // The ES context went away (or is being torn down): the span, its buffer ids and
        // the frontend objects it pinned all belonged to it.
        void OnBackendContextDestroyed() {
            g_xfbPending = false;
            g_xfbStarted = false;
            g_xfbTargets.clear();
        }
    } // namespace XfbImpl

    namespace VertexArrayImpl {
        void SyncCurrentVAO() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_backendVertexArrayObjects.CollectGarbageIfNeeded();

            auto& currentVAOObject = MG_State::pGLContext->GetBoundVertexArray();
            if (!currentVAOObject) {
                MGLOG_E("No VAO is currently bound, cannot sync current VAO.");
                return;
            }

            const auto& backendVAOIt = g_backendVertexArrayObjects.find(currentVAOObject.get());
            Bool exist = (backendVAOIt != g_backendVertexArrayObjects.end());
            auto& backendObj = exist ? backendVAOIt->second : g_backendVertexArrayObjects.GetOrCreate(currentVAOObject);
            if (!exist) {
                backendObj = MakeShared<VertexArrayImpl::BackendVertexArrayObject>();
            }
            backendObj->SyncToBackend(currentVAOObject);
        }

        // GL: a shader input whose generic attribute array is DISABLED reads that attribute's *current
        // value* (context state set by glVertexAttrib*, default (0,0,0,1)) rather than any buffer.
        // MobileGL stores those values in MG_State only, so without this step the ES driver would feed
        // the shader its own current values, which MobileGL never writes -- i.e. always (0,0,0,1).
        // SyncToBackend has already issued glDisableVertexAttribArray for these locations, so the ES
        // current value is what the shader will actually read.
        void SyncCurrentVertexAttributeValues() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const auto& program = MG_State::pGLContext->GetCurrentProgram();
            if (!program) return;

            const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            if (!vao) return;

            const Uint32 activeAttribMask = program->GetActiveAttributeLocationMask();
            if (activeAttribMask == 0) return;

            constexpr Uint32 maxVertexAttribs =
                static_cast<Uint32>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
            for (Uint32 location = 0; location < maxVertexAttribs; ++location) {
                if ((activeAttribMask & (1u << location)) == 0) continue;
                if (vao->GetAttribute(location).Enabled) continue;

                const auto& currentValue = MG_State::pGLContext->GetCurrentVertexAttribute(location);
                const auto typeInfo = MG_State::GLState::ClassifyVertexAttribType(program->GetAttribType(location));
                switch (typeInfo.baseType) {
                case MG_State::GLState::VertexAttribBaseType::Float:
                    g_GLESFuncs.glVertexAttrib4fv(location, currentValue.floatValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Int:
                    g_GLESFuncs.glVertexAttribI4iv(location, currentValue.intValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Uint:
                    g_GLESFuncs.glVertexAttribI4uiv(location, currentValue.uintValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Unsupported:
                    MGLOG_E("SyncCurrentVertexAttributeValues: program=%u location=%u has no enabled array and its "
                            "shader input type 0x%x is not supported as a current generic vertex attribute",
                            program->GetExternalIndex(), location, program->GetAttribType(location));
                    break;
                }
            }
        }
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        SharedPtr<BackendTextureObject>& SyncTextureObjectToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
            Bool imageBindableStorageRequired) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const auto& backendTextureIt = g_backendTextureObjects.find(textureObject.get());
            Bool exist = (backendTextureIt != g_backendTextureObjects.end());
            auto& backendObj = exist ? backendTextureIt->second : g_backendTextureObjects.GetOrCreate(textureObject);
            if (!exist) {
                backendObj = MakeShared<BackendTextureObject>();
            }
            if (imageBindableStorageRequired) {
                backendObj->RequireImageBindableStorage();
            }
            backendObj->SyncTextureParamsToBackend(textureObject);
            backendObj->SyncBuiltinSamplerToBackend(textureObject);
            backendObj->SyncMipmapsToBackend(textureObject);

            return backendObj;
        }

        void SyncNeccessaryTextures() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_backendTextureObjects.CollectGarbageIfNeeded();

            // All textures we need are:
            //   1. textures bound to texture units (TODO: only sync ones that are used in current program)
            //   2. textures used in current FBO
            //   3. textures bound to image units (TODO)

            // Units past the frontend's high-water mark have provably-empty slots.
            const Int maxTouchedUnit = MG_State::pGLContext->GetMaxTouchedTextureUnit();
            for (Int index = 0; index <= maxTouchedUnit; ++index) {
                auto& unit = MG_State::pGLContext->GetTextureUnitObject(index);
                for (const auto& bindingSlot : unit.GetAllBindingSlots()) {
                    auto& textureObject = bindingSlot.GetBoundObject();
                    // An image-less default texture (name 0) is the slot's initial / "unbound"
                    // state; it has nothing to sync, so skip it as cheaply as the old null slot.
                    if (textureObject && !MG_State::GLState::IsUndefinedDefaultTexture(textureObject.get())) {
                        SyncTextureObjectToBackend(textureObject);
                    }
                }
            }

            const auto& currentFBO =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
            if (currentFBO) {
                for (const auto& attachment : currentFBO->GetAllAttachmentObjects()) {
                    if (!attachment.IsTexture()) continue;
                    auto& textureObject = attachment.GetTexture();
                    if (textureObject) {
                        SyncTextureObjectToBackend(textureObject);
                    }
                }
            }
        }

        static Bool SupportsLayeredImageBinding(TextureTarget target) {
            return target == TextureTarget::Texture3D || target == TextureTarget::TextureCubeMap ||
                   target == TextureTarget::Texture2DArray || target == TextureTarget::TextureCubeMapArray ||
                   target == TextureTarget::Texture2DMultisampleArray;
        }

        void SyncImageTextureBinding(Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(unit));
            if (!imageBinding.Texture) {
                g_GLESFuncs.glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                return;
            }

            auto& backendTexture = SyncTextureObjectToBackend(imageBinding.Texture, true);
            const GLboolean layered =
                SupportsLayeredImageBinding(imageBinding.Texture->GetTarget()) ? imageBinding.Layered : GL_FALSE;
            g_GLESFuncs.glBindImageTexture(unit, backendTexture->GetBackendTextureId(), imageBinding.Level,
                                           layered, imageBinding.Layer, imageBinding.Access, imageBinding.Format);
        }

        void SyncImageTextureBindings() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // The frontend tracks more image units than ES exposes; binding past the device
            // limit raises GL_INVALID_VALUE on every dispatch.
            const Uint unitCount = std::min<Uint>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS,
                                                  static_cast<Uint>(std::max(g_GLESCapabilities.MaxImageUnits, 0)));
            for (Uint unit = 0; unit < unitCount; ++unit) {
                SyncImageTextureBinding(unit);
            }
        }
    } // namespace TextureImpl

    namespace FramebufferImpl {
        void SyncCurrentFBO() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_backendFramebufferObjects.CollectGarbageIfNeeded();
            TextureImpl::g_backendTextureObjects.CollectGarbageIfNeeded();
            RenderbufferImpl::g_backendRenderbufferObjects.CollectGarbageIfNeeded();

            const FramebufferTarget fboTargets[] = {FramebufferTarget::Draw, FramebufferTarget::Read};

            MG_State::GLState::FramebufferObject* lastUpdatedFBO = nullptr;

            for (auto& target : fboTargets) {
                auto& slot = MG_State::pGLContext->GetFramebufferBindingSlot(target);
                auto& currentFBO = slot.GetBoundObject();

                // The slot version only tracks rebinds; attachment/drawbuffer edits on an
                // already-bound FBO bump its object version and must re-sync it too (e.g.
                // Minecraft 26.x reuses one FBO for depth-blit destinations with draw
                // buffers NONE and for color clears with draw buffer 0 — dropping the
                // glDrawBuffers change turns every offscreen clear into a no-op).
                const Uint16 slotVersion = slot.GetVersion();
                const Uint16 objectVersion = currentFBO ? currentFBO->GetObjectVersion() : 0;
                auto* currentPtr = currentFBO.get();
                if (slotVersion == g_fboBindVersions[SizeT(target)] &&
                    objectVersion == g_fboSyncedObjectVersions[SizeT(target)] &&
                    currentPtr == g_fboSyncedObjects[SizeT(target)]) {
                    continue;
                }

                if (!currentFBO) {
                    MGLOG_E("No FBO is currently bound, cannot sync current FBO.");
                    continue;
                }

                if (currentFBO == MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo->defaultFBO) {
                    // Default FBO, nothing to sync
                    continue;
                }

                if (currentFBO.get() == lastUpdatedFBO) {
                    MGLOG_D("Draw FBO and read FBO are the same, skipping sync.");
                    // The attachment/draw-buffer work was already done for this GL FBO as the DRAW
                    // target, but the read buffer (glReadBuffer) is READ-target-specific and would
                    // be dropped by this skip. Apply it so reads target the right attachment (e.g.
                    // KHR-GL33.draw_buffers reads each COLOR_ATTACHMENT while the FBO stays bound as
                    // GL_FRAMEBUFFER — without this every glReadBuffer is a no-op and all reads hit
                    // COLOR_ATTACHMENT0).
                    if (target == FramebufferTarget::Read) {
                        const auto& syncedFBOIt = g_backendFramebufferObjects.find(currentFBO.get());
                        if (syncedFBOIt != g_backendFramebufferObjects.end() && syncedFBOIt->second) {
                            syncedFBOIt->second->SyncReadBufferToBackend(currentFBO);
                        }
                    }
                    g_fboSyncedObjectVersions[SizeT(target)] = objectVersion;
                    g_fboSyncedObjects[SizeT(target)] = currentPtr;
                    continue;
                }

                const auto& backendFBOIt = g_backendFramebufferObjects.find(currentFBO.get());
                Bool exist = (backendFBOIt != g_backendFramebufferObjects.end());
                auto& backendObj = exist ? backendFBOIt->second : g_backendFramebufferObjects.GetOrCreate(currentFBO);
                if (!exist) {
                    backendObj = MakeShared<BackendFramebufferObject>();
                }
                backendObj->SyncToBackend(currentFBO, target);

                g_fboSyncedObjectVersions[SizeT(target)] = objectVersion;
                g_fboSyncedObjects[SizeT(target)] = currentPtr;
                lastUpdatedFBO = currentFBO.get();
            }
        }
    } // namespace FramebufferImpl

    namespace RenderStateImpl {
        static Uint16 g_syncedRenderStateVersion = 0;
        static Bool g_hasSyncedRenderState = false;
        static RenderStateParameters g_syncedRenderStateParameters;
        static IntVec4 g_syncedBackendViewport = IntVec4(-1, -1, -1, -1);
        void SyncRenderState() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            Uint16 currentRenderStateVersion = MG_State::pGLContext->GetRenderStateParametersVersion();
            if (g_hasSyncedRenderState && currentRenderStateVersion == g_syncedRenderStateVersion) return;

            const auto& parameters = MG_State::pGLContext->GetRenderStateParameters();

            IntVec4 backendViewport = parameters.Viewport;
            if (backendViewport.z() <= 0 || backendViewport.w() <= 0) {
                Int surfaceWidth = 0;
                Int surfaceHeight = 0;
                if (QueryCurrentSurfaceSize(surfaceWidth, surfaceHeight)) {
                    backendViewport = IntVec4(0, 0, surfaceWidth, surfaceHeight);
                }
            }
            if (backendViewport != g_syncedBackendViewport) {
                g_GLESFuncs.glViewport(backendViewport.x(), backendViewport.y(), backendViewport.z(),
                                       backendViewport.w());
                g_syncedBackendViewport = backendViewport;
            }

#define SYNC_CAPABILITY(cap_mg, cap_gl)                                                                                \
    if (parameters.cap_mg##Enabled != g_syncedRenderStateParameters.cap_mg##Enabled) {                                 \
        if (parameters.cap_mg##Enabled) {                                                                              \
            g_GLESFuncs.glEnable(cap_gl);                                                                              \
        } else {                                                                                                       \
            g_GLESFuncs.glDisable(cap_gl);                                                                             \
        }                                                                                                              \
    }
            SYNC_CAPABILITY(DepthTest, GL_DEPTH_TEST);
            SYNC_CAPABILITY(ColorLogicOp, GL_COLOR_LOGIC_OP);
            SYNC_CAPABILITY(Dither, GL_DITHER);
            SYNC_CAPABILITY(Multisample, GL_MULTISAMPLE);
            SYNC_CAPABILITY(SampleAlphaToCoverage, GL_SAMPLE_ALPHA_TO_COVERAGE);
            SYNC_CAPABILITY(SampleCoverage, GL_SAMPLE_COVERAGE);
            SYNC_CAPABILITY(SampleMask, GL_SAMPLE_MASK);
            SYNC_CAPABILITY(PolygonOffsetFill, GL_POLYGON_OFFSET_FILL);
            SYNC_CAPABILITY(RasterizerDiscard, GL_RASTERIZER_DISCARD);
            SYNC_CAPABILITY(ScissorTest, GL_SCISSOR_TEST);
            SYNC_CAPABILITY(StencilTest, GL_STENCIL_TEST);
            SYNC_CAPABILITY(CullFace, GL_CULL_FACE);

#undef SYNC_CAPABILITY

            { // Primitive restart. GLES core has only GL_PRIMITIVE_RESTART_FIXED_INDEX (fixed all-ones
              // value); both the fixed cap and the (fixed-valued) arbitrary GL_PRIMITIVE_RESTART map to
              // it. An arbitrary non-fixed restart index is rejected at draw time (see DrawElements).
                const Bool restart = parameters.PrimitiveRestartFixedIndexEnabled || parameters.PrimitiveRestartEnabled;
                const Bool syncedRestart = g_syncedRenderStateParameters.PrimitiveRestartFixedIndexEnabled ||
                                           g_syncedRenderStateParameters.PrimitiveRestartEnabled;
                if (restart != syncedRestart) {
                    restart ? g_GLESFuncs.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX)
                            : g_GLESFuncs.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
                }
            }

            const auto& ToGLBoolean = [](Bool b) -> GLboolean { return b ? GL_TRUE : GL_FALSE; };

            { // Blend State
                using FBO = MG_State::GLState::FramebufferObject;
                const auto& targetStates = parameters.BlendStates;
                auto& syncedStates = g_syncedRenderStateParameters.BlendStates;

                // Dual-source blending (GL_SRC1_* factors from glBlendFunc paired with
                // glBindFragDataLocationIndexed) needs GL_EXT_blend_func_extended; GLES core has none.
                // Detected at load and surfaced in the POST. There is no fallback, so if a draw actually
                // enables blending with a SRC1 factor on a driver that lacks it, hard-fail here at use
                // time rather than let the driver reject glBlendFuncSeparate and silently mis-blend.
                if (!g_GLESCapabilities.SupportsDualSourceBlend) {
                    for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                        const auto& s = targetStates[i];
                        if (s.Enabled &&
                            (IsDualSourceBlendFactor(s.SrcFactorRGB) || IsDualSourceBlendFactor(s.DstFactorRGB) ||
                             IsDualSourceBlendFactor(s.SrcFactorAlpha) || IsDualSourceBlendFactor(s.DstFactorAlpha))) {
                            THROW_EXCEPTION(
                                "Dual-source blending (GL_SRC1_* blend factor) was used on draw buffer " +
                                std::to_string(i) +
                                ", but the GLES driver does not expose GL_EXT_blend_func_extended (see the "
                                "dual-source blend row in the driver POST). No fallback exists; the draw "
                                "cannot proceed.");
                        }
                    }
                }

                Bool allEnabled = true;
                Bool allDisabled = true;
                Bool anyCapDirty = false;

                for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                    Bool enabled = targetStates[i].Enabled;
                    if (enabled)
                        allDisabled = false;
                    else
                        allEnabled = false;

                    if (enabled != syncedStates[i].Enabled) {
                        anyCapDirty = true;
                    }
                }

                if (anyCapDirty) {
                    if (allEnabled) {
                        g_GLESFuncs.glEnable(GL_BLEND);
                        for (auto& s : syncedStates)
                            s.Enabled = true;
                    } else if (allDisabled) {
                        g_GLESFuncs.glDisable(GL_BLEND);
                        for (auto& s : syncedStates)
                            s.Enabled = false;
                    } else {
                        for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                            if (targetStates[i].Enabled != syncedStates[i].Enabled) {
                                syncedStates[i].Enabled = targetStates[i].Enabled;
                                syncedStates[i].Enabled ? g_GLESFuncs.glEnablei(GL_BLEND, i)
                                                        : g_GLESFuncs.glDisablei(GL_BLEND, i);
                            }
                        }
                    }
                }

                Bool allFuncsSame = true;
                Bool anyFuncDirty = false;
                const auto& first = targetStates[0];

                for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                    const auto& cur = targetStates[i];
                    const auto& syn = syncedStates[i];

                    Bool isDiffFromSyn =
                        (cur.SrcFactorRGB != syn.SrcFactorRGB || cur.DstFactorRGB != syn.DstFactorRGB ||
                         cur.SrcFactorAlpha != syn.SrcFactorAlpha || cur.DstFactorAlpha != syn.DstFactorAlpha);

                    if (isDiffFromSyn) anyFuncDirty = true;

                    if (allFuncsSame && i > 0) {
                        if (cur.SrcFactorRGB != first.SrcFactorRGB || cur.DstFactorRGB != first.DstFactorRGB ||
                            cur.SrcFactorAlpha != first.SrcFactorAlpha || cur.DstFactorAlpha != first.DstFactorAlpha) {
                            allFuncsSame = false;
                        }
                    }
                }

                if (anyFuncDirty) {
                    if (allFuncsSame) {
                        g_GLESFuncs.glBlendFuncSeparate(MG_Util::ConvertBlendFactorToGLEnum(first.SrcFactorRGB),
                                                        MG_Util::ConvertBlendFactorToGLEnum(first.DstFactorRGB),
                                                        MG_Util::ConvertBlendFactorToGLEnum(first.SrcFactorAlpha),
                                                        MG_Util::ConvertBlendFactorToGLEnum(first.DstFactorAlpha));

                        for (auto& syn : syncedStates) {
                            syn.SrcFactorRGB = first.SrcFactorRGB;
                            syn.DstFactorRGB = first.DstFactorRGB;
                            syn.SrcFactorAlpha = first.SrcFactorAlpha;
                            syn.DstFactorAlpha = first.DstFactorAlpha;
                        }
                    } else {
                        for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                            const auto& cur = targetStates[i];
                            auto& syn = syncedStates[i];

                            if (cur.SrcFactorRGB != syn.SrcFactorRGB || cur.DstFactorRGB != syn.DstFactorRGB ||
                                cur.SrcFactorAlpha != syn.SrcFactorAlpha || cur.DstFactorAlpha != syn.DstFactorAlpha) {
                                syn.SrcFactorRGB = cur.SrcFactorRGB;
                                syn.DstFactorRGB = cur.DstFactorRGB;
                                syn.SrcFactorAlpha = cur.SrcFactorAlpha;
                                syn.DstFactorAlpha = cur.DstFactorAlpha;

                                g_GLESFuncs.glBlendFuncSeparatei(
                                    i, MG_Util::ConvertBlendFactorToGLEnum(cur.SrcFactorRGB),
                                    MG_Util::ConvertBlendFactorToGLEnum(cur.DstFactorRGB),
                                    MG_Util::ConvertBlendFactorToGLEnum(cur.SrcFactorAlpha),
                                    MG_Util::ConvertBlendFactorToGLEnum(cur.DstFactorAlpha));
                            }
                        }
                    }
                }

                Bool allEquationsSame = true;
                Bool anyEquationDirty = false;

                for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                    const auto& cur = targetStates[i];
                    const auto& syn = syncedStates[i];

                    if (cur.ColorEquation != syn.ColorEquation || cur.AlphaEquation != syn.AlphaEquation) {
                        anyEquationDirty = true;
                    }

                    if (allEquationsSame && i > 0) {
                        if (cur.ColorEquation != first.ColorEquation || cur.AlphaEquation != first.AlphaEquation) {
                            allEquationsSame = false;
                        }
                    }
                }

                if (anyEquationDirty) {
                    if (allEquationsSame) {
                        g_GLESFuncs.glBlendEquationSeparate(MG_Util::ConvertBlendEquationToGLEnum(first.ColorEquation),
                                                            MG_Util::ConvertBlendEquationToGLEnum(first.AlphaEquation));

                        for (auto& syn : syncedStates) {
                            syn.ColorEquation = first.ColorEquation;
                            syn.AlphaEquation = first.AlphaEquation;
                        }
                    } else {
                        for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                            const auto& cur = targetStates[i];
                            auto& syn = syncedStates[i];

                            if (cur.ColorEquation != syn.ColorEquation || cur.AlphaEquation != syn.AlphaEquation) {
                                syn.ColorEquation = cur.ColorEquation;
                                syn.AlphaEquation = cur.AlphaEquation;

                                g_GLESFuncs.glBlendEquationSeparatei(
                                    i, MG_Util::ConvertBlendEquationToGLEnum(cur.ColorEquation),
                                    MG_Util::ConvertBlendEquationToGLEnum(cur.AlphaEquation));
                            }
                        }
                    }
                }
            }

            { // Depth state
                if (parameters.DepthFunc != g_syncedRenderStateParameters.DepthFunc) {
                    g_GLESFuncs.glDepthFunc(MG_Util::ConvertDepthTestFuncToGLEnum(parameters.DepthFunc));
                }
                if (parameters.DepthMask != g_syncedRenderStateParameters.DepthMask) {
                    g_GLESFuncs.glDepthMask(parameters.DepthMask ? GL_TRUE : GL_FALSE);
                }
                if (parameters.DepthRange != g_syncedRenderStateParameters.DepthRange) {
                    g_GLESFuncs.glDepthRangef(parameters.DepthRange.x(), parameters.DepthRange.y());
                }
            }

            { // Stencil state
                for (SizeT faceIndex = 0; faceIndex < parameters.StencilStates.size(); ++faceIndex) {
                    const StencilFaceState& current = parameters.StencilStates[faceIndex];
                    const StencilFaceState& synced = g_syncedRenderStateParameters.StencilStates[faceIndex];
                    const GLenum glFace = faceIndex == 0 ? GL_FRONT : GL_BACK;

                    if (current.Func != synced.Func || current.Ref != synced.Ref ||
                        current.ValueMask != synced.ValueMask) {
                        g_GLESFuncs.glStencilFuncSeparate(
                            glFace, MG_Util::ConvertDepthTestFuncToGLEnum(current.Func), current.Ref,
                            current.ValueMask);
                    }
                    if (current.WriteMask != synced.WriteMask) {
                        g_GLESFuncs.glStencilMaskSeparate(glFace, current.WriteMask);
                    }
                    if (current.FailOp != synced.FailOp || current.PassDepthFailOp != synced.PassDepthFailOp ||
                        current.PassDepthPassOp != synced.PassDepthPassOp) {
                        g_GLESFuncs.glStencilOpSeparate(
                            glFace, MG_Util::ConvertStencilOperationToGLEnum(current.FailOp),
                            MG_Util::ConvertStencilOperationToGLEnum(current.PassDepthFailOp),
                            MG_Util::ConvertStencilOperationToGLEnum(current.PassDepthPassOp));
                    }
                }
            }

            { // Color mask. Uniform masks use the non-indexed glColorMask (works everywhere); divergent
              // per-draw-buffer masks use the indexed glColorMaski when draw_buffers_indexed is
              // available, otherwise fall back to broadcasting draw buffer 0. Mirrors the blend block.
                using FBO = MG_State::GLState::FramebufferObject;
                const auto& targetMasks = parameters.ColorMasks;
                const auto& syncedMasks = g_syncedRenderStateParameters.ColorMasks;

                Bool anyDirty = false;
                Bool allSame = true;
                for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                    if (targetMasks[i] != syncedMasks[i]) anyDirty = true;
                    if (i > 0 && targetMasks[i] != targetMasks[0]) allSame = false;
                }

                if (anyDirty) {
                    if (allSame || !g_GLESCapabilities.SupportsIndexedColorMask) {
                        const BoolVec4& m = targetMasks[0];
                        g_GLESFuncs.glColorMask(ToGLBoolean(m.x()), ToGLBoolean(m.y()), ToGLBoolean(m.z()),
                                                ToGLBoolean(m.w()));
                    } else {
                        const auto colorMaskiFn = g_GLESFuncs.glColorMaski      ? g_GLESFuncs.glColorMaski
                                                  : g_GLESFuncs.glColorMaskiEXT ? g_GLESFuncs.glColorMaskiEXT
                                                                                : g_GLESFuncs.glColorMaskiOES;
                        for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                            if (targetMasks[i] != syncedMasks[i]) {
                                const BoolVec4& m = targetMasks[i];
                                colorMaskiFn(i, ToGLBoolean(m.x()), ToGLBoolean(m.y()), ToGLBoolean(m.z()),
                                             ToGLBoolean(m.w()));
                            }
                        }
                    }
                }
            }

            { // Polygon mode. GLES core has no glPolygonMode; use NV/ANGLE_polygon_mode when present.
              // Without the extension the mode stays FILL and non-FILL requests are dropped.
                if (parameters.PolygonModeFront != g_syncedRenderStateParameters.PolygonModeFront &&
                    g_GLESCapabilities.SupportsPolygonMode) {
                    const auto polygonModeFn =
                        g_GLESFuncs.glPolygonModeNV ? g_GLESFuncs.glPolygonModeNV : g_GLESFuncs.glPolygonModeANGLE;
                    polygonModeFn(GL_FRONT_AND_BACK, parameters.PolygonModeFront);
                }
            }

            { // Clear values
                if (parameters.ClearColor != g_syncedRenderStateParameters.ClearColor) {
                    const FloatVec4& clearCol = parameters.ClearColor;
                    g_GLESFuncs.glClearColor(clearCol.x(), clearCol.y(), clearCol.z(), clearCol.w());
                }
                if (parameters.ClearDepth != g_syncedRenderStateParameters.ClearDepth) {
                    g_GLESFuncs.glClearDepthf(parameters.ClearDepth);
                }
                if (parameters.BlendColor != g_syncedRenderStateParameters.BlendColor) {
                    const FloatVec4& blendColor = parameters.BlendColor;
                    g_GLESFuncs.glBlendColor(blendColor.x(), blendColor.y(), blendColor.z(), blendColor.w());
                }
            }

            { // Cull face mode
                if (parameters.CullFaceModeSetting != g_syncedRenderStateParameters.CullFaceModeSetting) {
                    const CullFaceMode& cfm = parameters.CullFaceModeSetting;
                    g_GLESFuncs.glCullFace(MG_Util::ConvertCullFaceModeToGLEnum(cfm));
                }
                if (parameters.FrontFaceModeSetting != g_syncedRenderStateParameters.FrontFaceModeSetting) {
                    const FrontFaceMode& ffm = parameters.FrontFaceModeSetting;
                    g_GLESFuncs.glFrontFace(MG_Util::ConvertFrontFaceModeToGLEnum(ffm));
                }
            }

            { // Scissor box
                if (parameters.ScissorBox != g_syncedRenderStateParameters.ScissorBox) {
                    const IntVec4& scissorBox = parameters.ScissorBox;
                    g_GLESFuncs.glScissor(scissorBox.x(), scissorBox.y(), scissorBox.z(), scissorBox.w());
                }
            }

            { // Logic op
                if (parameters.LogicOp != g_syncedRenderStateParameters.LogicOp) {
                    g_GLESFuncs.glLogicOp(MG_Util::ConvertLogicOperationToGLEnum(parameters.LogicOp));
                }
            }

            { // Polygon offset
                if (parameters.PolygonOffsetFactor != g_syncedRenderStateParameters.PolygonOffsetFactor ||
                    parameters.PolygonOffsetUnits != g_syncedRenderStateParameters.PolygonOffsetUnits) {
                    g_GLESFuncs.glPolygonOffset(parameters.PolygonOffsetFactor, parameters.PolygonOffsetUnits);
                }
            }

            { // Line width
                if (parameters.LineWidth != g_syncedRenderStateParameters.LineWidth) {
                    g_GLESFuncs.glLineWidth(parameters.LineWidth);
                }
            }

            { // Point size
                if (parameters.PointSize != g_syncedRenderStateParameters.PointSize) {
                    g_GLESFuncs.glPointSize(parameters.PointSize);
                }
            }

            { // Sample coverage
                if (parameters.SampleCoverageValue != g_syncedRenderStateParameters.SampleCoverageValue ||
                    parameters.SampleCoverageInvert != g_syncedRenderStateParameters.SampleCoverageInvert) {
                    g_GLESFuncs.glSampleCoverage(parameters.SampleCoverageValue,
                                                ToGLBoolean(parameters.SampleCoverageInvert));
                }
            }

            { // Sample mask
                if (g_GLESFuncs.glSampleMaski && parameters.SampleMaskValue != g_syncedRenderStateParameters.SampleMaskValue) {
                    g_GLESFuncs.glSampleMaski(0, parameters.SampleMaskValue);
                }
            }

            g_syncedRenderStateVersion = currentRenderStateVersion;
            g_syncedRenderStateParameters = parameters;
            g_hasSyncedRenderState = true;
        }
    } // namespace RenderStateImpl

    namespace PrgramImpl {
        void SyncCurrentProgram() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_backendProgramObjects.CollectGarbageIfNeeded();
            SamplerImpl::g_backendSamplerObjects.CollectGarbageIfNeeded();

            auto& currentProgram = MG_State::pGLContext->GetCurrentProgram();
            if (!currentProgram || !currentProgram->GetLinkStatus()) {
                g_GLESFuncs.glUseProgram(0);
                g_lastUsedBackendProgramId = 0;
                return;
            }
            const auto& backendProgramIt = g_backendProgramObjects.find(currentProgram.get());
            Bool exist = (backendProgramIt != g_backendProgramObjects.end());
            auto& backendObj = exist ? backendProgramIt->second : g_backendProgramObjects.GetOrCreate(currentProgram);
            if (!exist) {
                backendObj = MakeShared<BackendProgramObjectImpl>();
                backendObj->SyncToBackend(currentProgram);
            } else {
                // A link-version mismatch means the program was relinked: the backend
                // shaders and every cache built by CacheResourceLocations (block
                // indices, sampler locations, UBO upload gate) are stale.
                if (!backendObj->GetBackendProgramId() ||
                    backendObj->GetSyncedLinkVersion() != currentProgram->GetLinkVersion() ||
                    backendObj->GetSnormFallbackClampOutputMask() != g_snormFallbackClampOutputMask ||
                    backendObj->GetUnormFallbackClampOutputMask() != g_unormFallbackClampOutputMask) {
                    backendObj->SyncToBackend(currentProgram);
                }
            }
        }
    } // namespace PrgramImpl

    void BindCurrentFBO(FramebufferTarget target) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        auto& slot = MG_State::pGLContext->GetFramebufferBindingSlot(target);
        if (slot.GetVersion() == FramebufferImpl::g_fboBindVersions[(SizeT)target]) return;

        const auto& currentFBO = slot.GetBoundObject();
        if (currentFBO && currentFBO != MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo->defaultFBO) {
            const auto& backendFBOIt = FramebufferImpl::g_backendFramebufferObjects.find(currentFBO.get());
            if (backendFBOIt != FramebufferImpl::g_backendFramebufferObjects.end()) {
                backendFBOIt->second->Bind(target);
            } else {
                MGLOG_E("No backend FBO found (maybe not synced) for current %s FBO, cannot bind FBO.",
                        (target == FramebufferTarget::Read ? "READ" : "DRAW"));
            }
        } else {
            MGLOG_D("Binding default framebuffer as %s FBO", (target == FramebufferTarget::Read ? "READ" : "DRAW"));
            // Through the shadow: a raw bind here would leave the shadow claiming
            // the previous user FBO, false-skipping its next re-bind.
            FramebufferImpl::BindFramebufferId(
                target == FramebufferTarget::Draw ? GL_DRAW_FRAMEBUFFER : GL_READ_FRAMEBUFFER, 0);
        }
    }

    void SyncAndBindFramebufferObject(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                      FramebufferTarget target, Bool forceSync = false) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        if (!framebuffer || framebuffer == MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo->defaultFBO) {
            FramebufferImpl::BindFramebufferId(
                target == FramebufferTarget::Draw ? GL_DRAW_FRAMEBUFFER : GL_READ_FRAMEBUFFER, 0);
            return;
        }

        auto& registry = FramebufferImpl::g_backendFramebufferObjects;
        const auto& backendFBOIt = registry.find(framebuffer.get());
        const Bool exists = backendFBOIt != registry.end();
        auto& backendObj = exists ? backendFBOIt->second : registry.GetOrCreate(framebuffer);
        if (!exists) {
            backendObj = MakeShared<FramebufferImpl::BackendFramebufferObject>();
        }
        if (forceSync) {
            backendObj->InvalidateSyncedState();
        }

        backendObj->SyncToBackend(framebuffer, target);
        backendObj->Bind(target);
    }

    void ForceBindCurrentFBO(FramebufferTarget target) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        auto& slot = MG_State::pGLContext->GetFramebufferBindingSlot(target);
        const auto& fbo = slot.GetBoundObject();
        SyncAndBindFramebufferObject(fbo, target);
        FramebufferImpl::g_fboBindVersions[(SizeT)target] = slot.GetVersion();
        FramebufferImpl::g_fboSyncedObjectVersions[(SizeT)target] = fbo ? fbo->GetObjectVersion() : 0;
        FramebufferImpl::g_fboSyncedObjects[(SizeT)target] = fbo.get();
    }

    static void BindCurrentProgramWithResources();

    void PrepareForDraw(DrawSyncBit syncBit) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        BufferImpl::SyncNeccessaryBuffers(syncBit & DrawSyncBit::IndexBuffer, syncBit & DrawSyncBit::IndirectBuffer);
        VertexArrayImpl::SyncCurrentVAO();
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        PrgramImpl::SyncCurrentProgram();
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        {
#ifdef TRACY_ENABLE
            ZoneScopedNC("BindCurrentVAO", TRACY_ZONECOLOR_BACKEND);
#endif
            const auto& currentVAO = MG_State::pGLContext->GetBoundVertexArray();
            if (currentVAO) {
                const auto& backendVAOIt = VertexArrayImpl::g_backendVertexArrayObjects.find(currentVAO.get());
                if (backendVAOIt != VertexArrayImpl::g_backendVertexArrayObjects.end()) {
                    backendVAOIt->second->Bind();
                }
            } else {
                g_GLESFuncs.glBindVertexArray(0);
            }
        }

        VertexArrayImpl::SyncCurrentVertexAttributeValues();

        BindCurrentTextures();
        BindCurrentProgramWithResources();

        // Last: opening the capture span needs the program current and the capture
        // buffers bound, and ES rejects most binding changes once it is open.
        XfbImpl::StartPendingTransformFeedback();
    }

    // Rebinds every frontend texture unit's textures (and sampler objects) on the
    // backend context. Needed before draws AND compute dispatches: content syncs
    // (SyncTextureObjectToBackend) bind scratch textures on the active unit as a
    // side effect, so unit bindings must be re-established afterwards or shaders
    // sample whatever texture the last sync left behind (e.g. Flywheel's depth
    // pyramid downsample reading a stale unit-0 binding instead of the depth
    // attachment).
    void BindCurrentTextures() {
#ifdef TRACY_ENABLE
        ZoneScopedNC("BindCurrentTextures", TRACY_ZONECOLOR_BACKEND);
#endif
        // Units past the frontend's high-water mark have provably-empty slots.
        const Int maxTouchedUnit = MG_State::pGLContext->GetMaxTouchedTextureUnit();
        for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
            auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
            Array<Bool, (SizeT)TextureTarget::TextureTargetCount> boundBackendTargets{};

            // Two passes over the slots, because desktop 1D/1D-array targets alias ES
            // 2D/2D-array targets: a unit can hold a real texture on one of an aliased
            // pair and a default (name 0) object on the other, and one native binding
            // has to serve both. A default object only carries the app's bind-0 state,
            // so a real texture always wins the alias - binding it second would leave
            // the shader sampling an empty texture (an app that gives name 0 an image
            // then still keeps the default's binding when nothing else claims the
            // native target). Pass 0 places the real textures, pass 1 fills in the
            // defaults for native targets that are still unclaimed.
            for (Int pass = 0; pass < 2; ++pass) {
                for (const auto& bindingSlot : textureUnit.GetAllBindingSlots()) {
                    const auto& textureObject = bindingSlot.GetBoundObject();
                    if (!textureObject) continue;

                    // An image-less default texture is the frontend's bind-0 state. Defer native
                    // unbinding until all slots have been considered: a default alias must not
                    // clear a real binding either.
                    if (MG_State::GLState::IsUndefinedDefaultTexture(textureObject.get())) continue;

                    const Bool isDefaultObject = textureObject->GetExternalIndex() == 0;
                    if (isDefaultObject != (pass == 1)) continue;

                    auto target = textureObject->GetTarget();
                    if (!TextureImpl::IsSupportedTextureTarget(target)) {
                        MGLOG_D("    Texture target %s is not supported, skipping.",
                                MG_Util::ConvertTextureTargetToString(target).c_str());
                        continue;
                    }
                    const auto backendTarget = TextureImpl::MapToBackendTextureTarget(target);
                    if (isDefaultObject && boundBackendTargets[static_cast<SizeT>(backendTarget)]) continue;
                    const GLenum targetGL = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);

                    // Bind texture object
                    const auto& backendTextureIt = TextureImpl::g_backendTextureObjects.find(textureObject.get());
                    if (backendTextureIt == TextureImpl::g_backendTextureObjects.end()) continue;

                    backendTextureIt->second->Bind(targetGL, unit);
                    boundBackendTargets[static_cast<SizeT>(backendTarget)] = true;
                }
            }

            // Clear each native target that has no resolved frontend binding. This is the backend
            // half of glBindTexture(..., 0); skipping image-less default objects would otherwise
            // leave the previously sampled ES texture resident. Deduplicate mapped desktop targets
            // so 1D/2D and 1D-array/2D-array aliases do not cause redundant binds.
            Array<Bool, (SizeT)TextureTarget::TextureTargetCount> visitedBackendTargets{};
            for (const auto& bindingSlot : textureUnit.GetAllBindingSlots()) {
                const auto target = bindingSlot.GetTarget();
                if (!TextureImpl::IsSupportedTextureTarget(target)) continue;

                const auto backendTarget = TextureImpl::MapToBackendTextureTarget(target);
                const auto backendTargetIndex = static_cast<SizeT>(backendTarget);
                if (visitedBackendTargets[backendTargetIndex]) continue;
                visitedBackendTargets[backendTargetIndex] = true;

                if (!boundBackendTargets[backendTargetIndex]) {
                    const GLenum targetGL = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
                    TextureImpl::UnbindTexture(unit, targetGL);
                }
            }

            // Bind sampler object if necessary
            const auto& samplerObject = textureUnit.GetSamplerObject();
            if (samplerObject) {
                const auto& backendSamplerIt = SamplerImpl::g_backendSamplerObjects.find(samplerObject.get());
                if (backendSamplerIt != SamplerImpl::g_backendSamplerObjects.end()) {
                    backendSamplerIt->second->Bind(unit);
                }

            } else {
            }
        }
    }

    // Binds the current program's backend object and re-establishes its per-program
    // resources: global UBO contents, uniform-block bindings, and sampler uniform
    // units (layout(binding=N) qualifiers are stripped from transpiled ESSL, so the
    // association must be rebuilt through the API). Compute dispatches depend on
    // this as much as draws do — e.g. Flywheel's cull shader reads the
    // _FlwFrameUniforms block and the _flw_depthPyramid sampler.
    static void BindCurrentProgramWithResources() {
        const auto& currentProgram = MG_State::pGLContext->GetCurrentProgram();
        if (currentProgram && currentProgram->GetLinkStatus()) {
#ifdef TRACY_ENABLE
            ZoneScopedNC("BindCurrentProgram", TRACY_ZONECOLOR_BACKEND);
#endif
            const auto& backendProgramIt = PrgramImpl::g_backendProgramObjects.find(currentProgram.get());
            if (backendProgramIt != PrgramImpl::g_backendProgramObjects.end()) {
                auto& backendProgram = *backendProgramIt->second;
                backendProgram.Use();

                // Global UBO: block index and binding-point assignment are cached at
                // link time (CacheResourceLocations); re-upload only when the CPU shadow
                // actually changed since the last upload for this program.
                if (currentProgram->GetUBOSize() > 0 && backendProgram.HasGlobalUboBlock()) {
#ifdef TRACY_ENABLE
                    ZoneScopedNC("UpdateGlobalUBO", TRACY_ZONECOLOR_BACKEND);
#endif
                    const Uint32 uboContentVersion = currentProgram->GetUBOContentVersion();
                    const SizeT uboSize = static_cast<SizeT>(currentProgram->GetUBOSize());
                    // Preferred path: write changed contents into a fresh slot of the
                    // shared persistent-mapped ring and bind it as a range. The GPU
                    // never reads bytes the CPU is writing, so the driver has no
                    // write-after-read hazard to resolve — the in-place glBufferSubData
                    // below forced Adreno into a ghost/stall on every uniform-dirtying
                    // draw (MC dirties uniforms every draw), which dominated frame time.
                    Bool ringBound = false;
                    if (BufferImpl::UboRingAvailable()) {
                        const SizeT bindSize =
                            std::max(uboSize, static_cast<SizeT>(backendProgram.GetGlobalUboBackendBlockSize()));
                        const Uint64 frameSerial = CurrentFrameSerial();
                        auto& ringSlot = backendProgram.GetGlobalUboRingAllocation();
                        Bool slotValid = ringSlot.ringGeneration == BufferImpl::UboRingGeneration() &&
                                         ringSlot.frameSerial == frameSerial &&
                                         ringSlot.contentVersion == uboContentVersion;
                        if (!slotValid) {
                            SizeT offset = 0;
                            if (BufferImpl::UboRingAllocate(bindSize, offset)) {
                                std::memcpy(static_cast<Uint8*>(BufferImpl::UboRingMappedPtr()) + offset,
                                            currentProgram->MapUBO(), uboSize);
                                ringSlot = {uboContentVersion, BufferImpl::UboRingGeneration(), frameSerial,
                                            offset};
                                slotValid = true;
                            }
                        }
                        if (slotValid) {
                            BufferImpl::BindBufferRangeCached(GL_UNIFORM_BUFFER, 0, BufferImpl::UboRingBufferId(),
                                                              static_cast<GLintptr>(ringSlot.offset),
                                                              static_cast<GLsizeiptr>(bindSize));
                            ringBound = true;
                        }
                    }
                    if (!ringBound) {
                        // Fallback (no EXT_buffer_storage / fences, or ring creation
                        // failed): the original in-place upload.
                        if (backendProgram.GetLastUploadedGlobalUboVersion() != uboContentVersion) {
                            g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, backendProgram.GetBackendGlobalUBOId());
                            g_GLESFuncs.glBufferSubData(GL_UNIFORM_BUFFER, 0, currentProgram->GetUBOSize(),
                                                        currentProgram->MapUBO());
                            g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, 0);
                            backendProgram.SetLastUploadedGlobalUboVersion(uboContentVersion);
                        }
                        BufferImpl::BindBufferBaseCached(GL_UNIFORM_BUFFER, 0,
                                                         backendProgram.GetBackendGlobalUBOId());
                    }
                }

                {
#ifdef TRACY_ENABLE
                    ZoneScopedNC("UpdateUBO", TRACY_ZONECOLOR_BACKEND);
#endif
                    // Normal UBOs: backend block indices and glUniformBlockBinding
                    // assignments are cached at link time; per draw only the buffer
                    // bindings are re-established (they follow frontend binding points).
                    const auto& blockBackendIndices = backendProgram.GetUniformBlockBackendIndices();
                    const auto uboCount = static_cast<Int>(blockBackendIndices.size());
                    Uint lastUBOBinding = 0; // binding 0 is reserved for the global UBO
                    for (Int i = 0; i < uboCount; ++i) {
                        ++lastUBOBinding;
                        if (blockBackendIndices[i] < 0) {
                            continue;
                        }

                        // Connect buffer to backend binding point
                        auto binding = currentProgram->GetUniformBlockBinding(i);
                        auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::Uniform, binding);
                        auto& bufferObj = point.GetBoundObject();
                        auto range = point.GetRange();

                        if (bufferObj) {
                            auto* backendResource = BufferImpl::EnsureBufferResource(bufferObj);
                            if (backendResource && backendResource->id != 0) {
                                // glBindBufferBase/Range set the generic GL_UNIFORM_BUFFER binding
                                // as a side effect, so no separate BindBufferId is needed here.
                                if (range.end == 0) {
                                    BufferImpl::BindBufferBaseCached(GL_UNIFORM_BUFFER, lastUBOBinding,
                                                                     backendResource->id);
                                } else {
                                    BufferImpl::BindBufferRangeCached(
                                        GL_UNIFORM_BUFFER, lastUBOBinding, backendResource->id,
                                        (GLintptr)range.start, (GLintptr)(range.end - range.start));
                                }
                            } else {
                                MGLOG_E("No backend buffer found for UBO binding, cannot bind UBO.");
                            }
                        }
                    }
                }

                {
#ifdef TRACY_ENABLE
                    ZoneScopedNC("BindSamplerUnit", TRACY_ZONECOLOR_BACKEND);
#endif
                    // Sampler unit binding: backend locations are cached at link time;
                    // glUniform1i is program state, so it is only re-issued when the
                    // frontend-assigned unit differs from what this program last saw.
                    for (auto& samplerBinding : backendProgram.GetSamplerUniformBindings()) {
                        const auto unit =
                            currentProgram->GetUniformSamplerOrImageUnitIndex(samplerBinding.frontendLocation);
                        if (unit == -1) continue;
                        if (samplerBinding.lastAssignedUnit != unit) {
                            g_GLESFuncs.glUniform1i(samplerBinding.backendLocation, unit);
                            samplerBinding.lastAssignedUnit = unit;
                        }

                        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
                        auto& samplerObject = textureUnit.GetSamplerObject();
                        const auto& texture2D = textureUnit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
                        const SharedPtr<MG_State::GLState::SamplerObject>* rawDepthSamplerObject = &samplerObject;
                        if (!*rawDepthSamplerObject && texture2D) {
                            rawDepthSamplerObject = &texture2D->GetSamplerObject();
                        }

                        if (samplerBinding.uniformType == GL_SAMPLER_2D && texture2D &&
                            NeedsRawDepthFetchSampler(*rawDepthSamplerObject, texture2D->GetFormat())) {
                            GetRawDepthFetchSampler()->Bind(unit);
                            MGLOG_D("Using raw depth fetch sampler on unit %d.", unit);
                        } else if (samplerObject) {
                            const auto& backendSamplerIt =
                                SamplerImpl::g_backendSamplerObjects.find(samplerObject.get());
                            Bool exist = (backendSamplerIt != SamplerImpl::g_backendSamplerObjects.end());
                            auto& backendObj = exist ? backendSamplerIt->second
                                                     : SamplerImpl::g_backendSamplerObjects.GetOrCreate(samplerObject);
                            if (!exist) {
                                backendObj = MakeShared<SamplerImpl::BackendSamplerObject>();
                            }
                            backendObj->SyncToBackend(samplerObject);
                        } else {
                            SamplerImpl::UnbindSampler(unit);
                        }
                    }
                }
            } else {
                g_GLESFuncs.glUseProgram(0);
                PrgramImpl::g_lastUsedBackendProgramId = 0;
                MGLOG_E("No backend program found (maybe not synced) for current program, cannot use program.");
            }
        }
    }

    static SharedPtr<PrgramImpl::BackendProgramObjectImpl> GetCurrentBackendProgram() {
        const auto& currentProgram = MG_State::pGLContext->GetCurrentProgram();
        if (!currentProgram || !currentProgram->GetLinkStatus()) {
            return nullptr;
        }
        const auto& backendProgramIt = PrgramImpl::g_backendProgramObjects.find(currentProgram.get());
        if (backendProgramIt != PrgramImpl::g_backendProgramObjects.end()) {
            return backendProgramIt->second;
        }
        return nullptr;
    }

    void SetCurrentBaseInstance(Uint32 baseInstance) {
        if (const auto program = GetCurrentBackendProgram()) {
            program->SetBaseInstance(baseInstance);
        }
    }

    void SetCurrentDrawID(Uint32 drawId) {
        if (const auto program = GetCurrentBackendProgram()) {
            program->SetDrawID(drawId);
        }
    }

    static Bool SupportsNativeIndirectDraws() {
        const auto& version = g_GLESCapabilities.GLESVersion;
        const Bool esVersionOk = version.Major > 3 || (version.Major == 3 && version.Minor >= 1);
        return esVersionOk && g_GLESFuncs.glDrawElementsIndirect != nullptr &&
               g_GLESFuncs.glDrawArraysIndirect != nullptr;
    }

    // Runs an (indexed) indirect multi-draw. When a GL_DRAW_INDIRECT_BUFFER is bound the draws
    // execute natively on the GPU so commands written by compute shaders (e.g. Flywheel's
    // culling pipeline updating instanceCount) are honored; the CPU shadow is still consulted
    // for the per-command baseInstance, which is CPU-authored, to feed the mg_BaseInstance
    // shader emulation. Without GL_EXT_base_instance a non-zero baseInstance in the command is
    // technically undefined in ES; mobile drivers ignore the reserved word, instanced-array
    // fetches were never baseInstance-offset here anyway, and the CPU fallback cannot see
    // GPU-written command fields at all - so native is never worse. ANGLE-on-Vulkan instead
    // leaks the word into gl_InstanceID (it becomes vkCmdDrawIndexedIndirect's firstInstance);
    // the shader rewrite compensates by rebasing gl_InstanceID during these draws when
    // IndirectDrawInstanceIdIncludesBaseInstance is set (PromoteDrawParameterGlobalsToUniforms).
    // Only client-memory commands take the CPU per-command loop.
    static void ExecuteIndexedIndirectCommands(GLenum mode, GLenum type, SizeT indexSize, const Uint8* commandBytes,
                                               SizeT commandOffset,
                                               const SharedPtr<MG_State::GLState::BufferObject>& drawIndirectBuffer,
                                               GLsizei drawcount, GLsizei stride, const char* label) {
        (void)label;
        const Bool useNative = drawIndirectBuffer != nullptr && SupportsNativeIndirectDraws();
        if (useNative) {
            // gl_BaseInstance must observe GPU-written command fields; expose the indirect
            // buffer to the program's mg_IndirectParams SSBO view and address it per draw.
            const auto backendProgram = GetCurrentBackendProgram();
            const Int paramsBinding = backendProgram ? backendProgram->GetIndirectParamsBinding() : -1;
            if (paramsBinding >= 0) {
                auto* resource = BufferImpl::EnsureBufferResource(drawIndirectBuffer);
                if (resource && resource->id != 0) {
                    BufferImpl::BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(paramsBinding),
                                                     resource->id);
                }
            }
            for (GLsizei i = 0; i < drawcount; ++i) {
                const SizeT cmdByteOffset = commandOffset + static_cast<SizeT>(i) * stride;
                SetCurrentDrawID(static_cast<Uint32>(i));
                if (paramsBinding >= 0 && backendProgram) {
                    // baseInstance is the 5th word of DrawElementsIndirectCommand.
                    backendProgram->SetBaseInstanceWordIndex(static_cast<Int32>((cmdByteOffset + 16) / 4));
                } else {
                    DrawElementsIndirectCommand cmd{};
                    std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                    SetCurrentBaseInstance(cmd.baseInstance);
                }
                g_GLESFuncs.glDrawElementsIndirect(mode, type, reinterpret_cast<const void*>(cmdByteOffset));
            }
        } else {
            for (GLsizei i = 0; i < drawcount; ++i) {
                DrawElementsIndirectCommand cmd{};
                std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                if (cmd.count == 0 || cmd.instanceCount == 0) {
                    continue;
                }
                SetCurrentDrawID(static_cast<Uint32>(i));
                SetCurrentBaseInstance(cmd.baseInstance);
                const auto indexByteOffset = static_cast<SizeT>(cmd.firstIndex) * indexSize;
                g_GLESFuncs.glDrawElementsInstancedBaseVertex(
                    mode, static_cast<GLsizei>(cmd.count), type, reinterpret_cast<const GLvoid*>(indexByteOffset),
                    static_cast<GLsizei>(cmd.instanceCount), cmd.baseVertex);
            }
        }
        SetCurrentDrawID(0);
        SetCurrentBaseInstance(0);
    }

    static void ExecuteArraysIndirectCommands(GLenum mode, const Uint8* commandBytes, SizeT commandOffset,
                                              const SharedPtr<MG_State::GLState::BufferObject>& drawIndirectBuffer,
                                              GLsizei drawcount, GLsizei stride, const char* label) {
        (void)label;
        const Bool useNative = drawIndirectBuffer != nullptr && SupportsNativeIndirectDraws();
        if (useNative) {
            const auto backendProgram = GetCurrentBackendProgram();
            const Int paramsBinding = backendProgram ? backendProgram->GetIndirectParamsBinding() : -1;
            if (paramsBinding >= 0) {
                auto* resource = BufferImpl::EnsureBufferResource(drawIndirectBuffer);
                if (resource && resource->id != 0) {
                    BufferImpl::BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(paramsBinding),
                                                     resource->id);
                }
            }
            for (GLsizei i = 0; i < drawcount; ++i) {
                const SizeT cmdByteOffset = commandOffset + static_cast<SizeT>(i) * stride;
                SetCurrentDrawID(static_cast<Uint32>(i));
                if (paramsBinding >= 0 && backendProgram) {
                    // baseInstance is the 4th word of DrawArraysIndirectCommand.
                    backendProgram->SetBaseInstanceWordIndex(static_cast<Int32>((cmdByteOffset + 12) / 4));
                } else {
                    DrawArraysIndirectCommand cmd{};
                    std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                    SetCurrentBaseInstance(cmd.baseInstance);
                }
                g_GLESFuncs.glDrawArraysIndirect(mode, reinterpret_cast<const void*>(cmdByteOffset));
            }
        } else {
            for (GLsizei i = 0; i < drawcount; ++i) {
                DrawArraysIndirectCommand cmd{};
                std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                if (cmd.count == 0 || cmd.instanceCount == 0) {
                    continue;
                }
                SetCurrentDrawID(static_cast<Uint32>(i));
                SetCurrentBaseInstance(cmd.baseInstance);
                g_GLESFuncs.glDrawArraysInstanced(mode, static_cast<GLint>(cmd.first),
                                                  static_cast<GLsizei>(cmd.count),
                                                  static_cast<GLsizei>(cmd.instanceCount));
            }
        }
        SetCurrentDrawID(0);
        SetCurrentBaseInstance(0);
    }

    void PrepareForCompute(Bool includeDispatchIndirectBuffer) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        BufferImpl::SyncComputeBuffers(includeDispatchIndirectBuffer);
        TextureImpl::SyncNeccessaryTextures();
        TextureImpl::SyncImageTextureBindings();
        PrgramImpl::SyncCurrentProgram();

        const auto& currentProgram = MG_State::pGLContext->GetCurrentProgram();
        if (!currentProgram || !currentProgram->GetLinkStatus()) {
            g_GLESFuncs.glUseProgram(0);
            PrgramImpl::g_lastUsedBackendProgramId = 0;
            return;
        }

        // Compute shaders sample textures through the same unit bindings as draws
        // (e.g. Flywheel's depth-pyramid downsample reads the depth attachment on
        // unit 0), so re-establish unit bindings after the content syncs above.
        BindCurrentTextures();
        // Compute programs need the same per-program resource sync as draws:
        // uniform-block bindings and sampler units only exist through the API
        // because layout(binding) is stripped from the transpiled ESSL.
        BindCurrentProgramWithResources();
    }

    GLuint GetBackendProgramId(GLuint program) {
        if (!MG_State::pGLContext->ValidateProgramName(program)) {
            MGLOG_E("Invalid frontend program object: %u", program);
            return 0;
        }

        auto& programObject = MG_State::pGLContext->GetProgramObject(program);
        if (!programObject) {
            MGLOG_E("Program object %u is null.", program);
            return 0;
        }

        const auto& backendProgramIt = PrgramImpl::g_backendProgramObjects.find(programObject.get());
        Bool exist = (backendProgramIt != PrgramImpl::g_backendProgramObjects.end());
        auto& backendObj =
            exist ? backendProgramIt->second : PrgramImpl::g_backendProgramObjects.GetOrCreate(programObject);
        if (!exist) {
            backendObj = MakeShared<PrgramImpl::BackendProgramObjectImpl>();
        }
        if (!backendObj->GetBackendProgramId()) {
            backendObj->SyncToBackend(programObject);
        }
        return backendObj->GetBackendProgramId();
    }

    void Clear(GLbitfield mask) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        // Debug-only diagnostics: is each offscreen clear complete and unscissored?
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        {
            static int diagCount = 0;
            GLint fboId = 0;
            g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboId);
            if (fboId != 0 && diagCount++ < 900) {
                GLint color0 = 0, depthName = 0, box[4] = {0};
                GLboolean scissor = g_GLESFuncs.glIsEnabled(GL_SCISSOR_TEST);
                GLboolean cmask[4] = {0};
                GLfloat cc[4] = {0};
                g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &color0);
                g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &depthName);
                g_GLESFuncs.glGetIntegerv(GL_SCISSOR_BOX, box);
                g_GLESFuncs.glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
                g_GLESFuncs.glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
                MGLOG_D("CLEAR fbo=%d color0=%d depth=%d mask=0x%x scissor=%d box=(%d,%d,%d,%d) cmask=%d%d%d%d cc=(%g,%g,%g,%g)",
                        fboId, color0, depthName, mask, (int)scissor, box[0], box[1], box[2], box[3], (int)cmask[0],
                        (int)cmask[1], (int)cmask[2], (int)cmask[3], cc[0], cc[1], cc[2], cc[3]);
            }
        }
#endif

        // GLES clamps the glClearColor state to [0,1] (desktop GL keeps it unclamped
        // for float color buffers). Minecraft's OIT clears its depth-bounds RGBA32F
        // target to -FLT_MAX as the MAX-blend identity, so an out-of-range clear
        // color must go through glClearBufferfv, which GLES does not clamp.
        GLbitfield remainingMask = mask;
        if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
            const FloatVec4& cc = MG_State::pGLContext->GetRenderStateParameters().ClearColor;
            const Bool outOfRange = cc.x() < 0.f || cc.x() > 1.f || cc.y() < 0.f || cc.y() > 1.f || cc.z() < 0.f ||
                                    cc.z() > 1.f || cc.w() < 0.f || cc.w() > 1.f;
            GLint clearDrawFbo = 0;
            g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &clearDrawFbo);
            if (outOfRange && clearDrawFbo != 0) {
                const GLfloat value[4] = {cc.x(), cc.y(), cc.z(), cc.w()};
                GLint maxDrawBuffers = 0;
                GLint clearedCount = 0;
                GLint firstDb = -1;
                g_GLESFuncs.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
                for (GLint i = 0; i < maxDrawBuffers; ++i) {
                    GLint db = GL_NONE;
                    g_GLESFuncs.glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(i), &db);
                    if (i == 0) firstDb = db;
                    if (db != GL_NONE) {
                        g_GLESFuncs.glClearBufferfv(GL_COLOR, i, value);
                        ++clearedCount;
                    }
                }
                remainingMask &= ~static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
                (void)clearedCount;
                (void)firstDb;
                // Debug-only diagnostics: verify the unclamped clear actually landed.
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
                {
                    static int diagCount = 0;
                    if (diagCount++ < 20) {
                        const GLenum clrErr = g_GLESFuncs.glGetError();
                        GLint prevRead = 0, prevPbo = 0;
                        g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
                        g_GLESFuncs.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPbo);
                        g_GLESFuncs.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                        g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)clearDrawFbo);
                        GLint prevReadBuf = GL_COLOR_ATTACHMENT0;
                        g_GLESFuncs.glGetIntegerv(GL_READ_BUFFER, &prevReadBuf);
                        g_GLESFuncs.glReadBuffer(GL_COLOR_ATTACHMENT0);
                        GLfloat rb[4] = {0};
                        g_GLESFuncs.glReadPixels(100, 100, 1, 1, GL_RGBA, GL_FLOAT, rb);
                        const GLenum rbErr = g_GLESFuncs.glGetError();
                        const auto& feFbo =
                            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
                        int feDb0 = -1, feDb1 = -1;
                        Uint feIdx = 0, feVer = 0;
                        if (feFbo) {
                            feIdx = feFbo->GetExternalIndex();
                            feVer = feFbo->GetObjectVersion();
                            feDb0 = (int)feFbo->GetDrawBuffers()[0];
                            feDb1 = (int)feFbo->GetDrawBuffers()[1];
                        }
                        MGLOG_D("CLEARV fbo=%d clrErr=0x%x rbErr=0x%x cc.x=%g cleared=%d firstDb=0x%x prevReadBuf=0x%x "
                                "feFbo=%u feVer=%u feDb=[%d,%d] stored=(%g,%g,%g,%g)",
                                clearDrawFbo, clrErr, rbErr, cc.x(), clearedCount, firstDb, prevReadBuf, feIdx, feVer,
                                feDb0, feDb1, rb[0], rb[1], rb[2], rb[3]);
                        g_GLESFuncs.glReadBuffer(static_cast<GLenum>(prevReadBuf));
                        g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
                        g_GLESFuncs.glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prevPbo);
                    }
                }
#endif
            }
        }
        if (remainingMask != 0) {
            g_GLESFuncs.glClear(remainingMask);
        }
    }

    // GLES core supports only GL_PRIMITIVE_RESTART_FIXED_INDEX (fixed all-ones value). If the app
    // enabled the arbitrary GL_PRIMITIVE_RESTART with a non-fixed index, hard-fail at this draw with
    // the reason (a fallback would silently drop restarts and corrupt geometry).
    void CheckPrimitiveRestartSupported(GLenum indexType) {
        if (!MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestart) ||
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex)) {
            return;
        }
        Uint32 fixedMax = 0;
        switch (indexType) {
        case GL_UNSIGNED_BYTE: fixedMax = 0xFFu; break;
        case GL_UNSIGNED_SHORT: fixedMax = 0xFFFFu; break;
        case GL_UNSIGNED_INT: fixedMax = 0xFFFFFFFFu; break;
        default: return;
        }
        const Uint32 restartIndex = MG_State::pGLContext->GetPrimitiveRestartIndex();
        if (restartIndex != fixedMax) {
            THROW_EXCEPTION("GL_PRIMITIVE_RESTART with an arbitrary restart index (" + std::to_string(restartIndex) +
                            ") is not supported by the GLES backend, which only restarts on the fixed index value (" +
                            std::to_string(fixedMax) +
                            ") for this index type; use GL_PRIMITIVE_RESTART_FIXED_INDEX or set glPrimitiveRestartIndex "
                            "to that value.");
        }
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        CheckPrimitiveRestartSupported(type);
        g_GLESFuncs.glDrawElements(mode, count, type, indices);
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncBit syncBit = DrawSyncBit::None;
        PrepareForDraw(syncBit);
        const auto& currentVAO = MG_State::pGLContext->GetBoundVertexArray();
        if (currentVAO) {
            const auto& backendVAOIt = VertexArrayImpl::g_backendVertexArrayObjects.find(currentVAO.get());
            if (backendVAOIt != VertexArrayImpl::g_backendVertexArrayObjects.end()) {
                backendVAOIt->second->SyncClientSideAttributesForDrawArrays(currentVAO, first, count);
            }
        }
        g_GLESFuncs.glDrawArrays(mode, first, count);
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        CheckPrimitiveRestartSupported(type);
        g_GLESFuncs.glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }

    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncBit syncBit = DrawSyncBit::None;
        PrepareForDraw(syncBit);

        const auto& currentVAO = MG_State::pGLContext->GetBoundVertexArray();
        for (GLsizei i = 0; i < drawcount; ++i) {
            // Client-side arrays are uploaded per sub-draw range, like the single DrawArrays path.
            if (currentVAO) {
                const auto& backendVAOIt = VertexArrayImpl::g_backendVertexArrayObjects.find(currentVAO.get());
                if (backendVAOIt != VertexArrayImpl::g_backendVertexArrayObjects.end()) {
                    backendVAOIt->second->SyncClientSideAttributesForDrawArrays(currentVAO, first[i], count[i]);
                }
            }
            g_GLESFuncs.glDrawArrays(mode, first[i], count[i]);
        }
    }

    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        CheckPrimitiveRestartSupported(type);

        for (GLsizei i = 0; i < drawcount; ++i) {
            g_GLESFuncs.glDrawElements(mode, count[i], type, indices[i]);
        }
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        CheckPrimitiveRestartSupported(type);

        for (GLsizei i = 0; i < drawcount; ++i) {
            g_GLESFuncs.glDrawElementsBaseVertex(mode, count[i], type, indices[i], basevertex[i]);
        }
    }

    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        if (drawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawElementsIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawElementsIndirectCommand))) {
            MGLOG_E("MultiDrawElementsIndirect skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawElementsIndirectCommand));
            return;
        }

        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E("MultiDrawElementsIndirect skipped: unsupported index type 0x%x", type);
            return;
        }

        const auto* commandBytes = ResolveIndirectCommandBytes(
            indirect,
            static_cast<SizeT>(stride) * static_cast<SizeT>(drawcount - 1) + sizeof(DrawElementsIndirectCommand),
            "MultiDrawElementsIndirect");
        if (!commandBytes) {
            return;
        }

        const auto& drawIndirectBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        ExecuteIndexedIndirectCommands(mode, type, indexSize, commandBytes, reinterpret_cast<SizeT>(indirect),
                                       drawIndirectBuffer, drawcount, stride, "MultiDrawElementsIndirect");
    }

    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        if (maxdrawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawElementsIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawElementsIndirectCommand))) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawElementsIndirectCommand));
            return;
        }

        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: unsupported index type 0x%x", type);
            return;
        }

        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        auto parameterBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
        if (!drawBuffer) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: no GL_DRAW_INDIRECT_BUFFER is bound");
            return;
        }
        if (!parameterBuffer) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: no GL_PARAMETER_BUFFER is bound");
            return;
        }

        drawBuffer->SyncPersistentMappedRange();
        parameterBuffer->SyncPersistentMappedRange();

        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset + static_cast<SizeT>(stride) * static_cast<SizeT>(maxdrawcount - 1) +
            sizeof(DrawElementsIndirectCommand);
        if (commandBytes > drawBuffer->GetSize()) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }
        if (drawcount < 0 || static_cast<SizeT>(drawcount) + sizeof(Uint32) > parameterBuffer->GetSize()) {
            MGLOG_E("MultiDrawElementsIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
            return;
        }

        Uint32 actualDrawCount = 0;
        std::memcpy(&actualDrawCount, parameterBuffer->MappedData() + drawcount, sizeof(actualDrawCount));
        actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
        ExecuteIndexedIndirectCommands(mode, type, indexSize, drawBuffer->MappedData() + commandOffset, commandOffset,
                                       drawBuffer, static_cast<GLsizei>(actualDrawCount), stride,
                                       "MultiDrawElementsIndirectCount");
    }

    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        if (drawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawArraysIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawArraysIndirectCommand))) {
            MGLOG_E("MultiDrawArraysIndirect skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        DrawSyncBit syncBit = DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const auto* commandBytes = ResolveIndirectCommandBytes(
            indirect,
            static_cast<SizeT>(stride) * static_cast<SizeT>(drawcount - 1) + sizeof(DrawArraysIndirectCommand),
            "MultiDrawArraysIndirect");
        if (!commandBytes) {
            return;
        }

        const auto& drawIndirectBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        ExecuteArraysIndirectCommands(mode, commandBytes, reinterpret_cast<SizeT>(indirect), drawIndirectBuffer,
                                      drawcount, stride, "MultiDrawArraysIndirect");
    }

    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex) {
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        g_GLESFuncs.glDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
    }

    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        g_GLESFuncs.glDrawRangeElements(mode, start, end, count, type, indices);
    }

    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        SetCurrentBaseInstance(baseinstance);
        g_GLESFuncs.glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
        SetCurrentBaseInstance(0);
    }

    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        g_GLESFuncs.glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
    }

    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        SetCurrentBaseInstance(baseinstance);
        g_GLESFuncs.glDrawElementsInstanced(mode, count, type, indices, instancecount);
        SetCurrentBaseInstance(0);
    }

    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        g_GLESFuncs.glDrawElementsInstanced(mode, count, type, indices, instancecount);
    }

    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        DrawSyncBit syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E("DrawElementsIndirect skipped: unsupported index type 0x%x", type);
            return;
        }

        const auto* commandBytes =
            ResolveIndirectCommandBytes(indirect, sizeof(DrawElementsIndirectCommand), "DrawElementsIndirect");
        if (!commandBytes) {
            return;
        }

        const auto& drawIndirectBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        ExecuteIndexedIndirectCommands(mode, type, indexSize, commandBytes, reinterpret_cast<SizeT>(indirect),
                                       drawIndirectBuffer, 1, sizeof(DrawElementsIndirectCommand),
                                       "DrawElementsIndirect");
    }

    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {
        DrawSyncBit syncBit = DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        SetCurrentBaseInstance(baseinstance);
        g_GLESFuncs.glDrawArraysInstanced(mode, first, count, instancecount);
        SetCurrentBaseInstance(0);
    }

    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        DrawSyncBit syncBit = DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        g_GLESFuncs.glDrawArraysInstanced(mode, first, count, instancecount);
    }

    void DrawArraysIndirect(GLenum mode, const void* indirect) {
        DrawSyncBit syncBit = DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const auto* commandBytes =
            ResolveIndirectCommandBytes(indirect, sizeof(DrawArraysIndirectCommand), "DrawArraysIndirect");
        if (!commandBytes) {
            return;
        }

        const auto& drawIndirectBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        ExecuteArraysIndirectCommands(mode, commandBytes, reinterpret_cast<SizeT>(indirect), drawIndirectBuffer, 1,
                                      sizeof(DrawArraysIndirectCommand), "DrawArraysIndirect");
    }

    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif

        TextureImpl::SyncNeccessaryTextures();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        FramebufferImpl::SyncCurrentFBO();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        RenderStateImpl::SyncRenderState();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        BindCurrentFBO(FramebufferTarget::Draw);
        BindCurrentFBO(FramebufferTarget::Read);
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        MGLOG_D("ES %s(%d, %d, %d, %d, %d, %d, %d, %d, 0x%x, %s)", __func__, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0,
                dstX1, dstY1, mask, MG_Util::ConvertGLEnumToString(filter).c_str());
        g_GLESFuncs.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
    }

    void BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
                              const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer,
                              GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                              GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                              GLbitfield mask, GLenum filter) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        TextureImpl::SyncNeccessaryTextures();
        RenderStateImpl::SyncRenderState();

        SyncAndBindFramebufferObject(readFramebuffer, FramebufferTarget::Read, true);
        SyncAndBindFramebufferObject(drawFramebuffer, FramebufferTarget::Draw, true);

        MGLOG_D("ES %s(%d, %d, %d, %d, %d, %d, %d, %d, 0x%x, %s)", __func__, srcX0, srcY0, srcX1, srcY1,
                dstX0, dstY0, dstX1, dstY1, mask, MG_Util::ConvertGLEnumToString(filter).c_str());
        g_GLESFuncs.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        // Debug-only diagnostics: which GLES depth texture did this blit write?
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        if (mask & GL_DEPTH_BUFFER_BIT) {
            static int diagCount = 0;
            if ((diagCount++ % 600) < 4) {
                GLint readFbo = 0, drawFbo = 0, readDepth = 0, drawDepth = 0;
                g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
                g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &readDepth);
                g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &drawDepth);
                MGLOG_D("DBLIT readFbo=%d(depth=%d) -> drawFbo=%d(depth=%d) rect=(%d,%d,%d,%d)->(%d,%d,%d,%d)",
                        readFbo, readDepth, drawFbo, drawDepth, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1,
                        dstY1);
            }
        }
#endif
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        ForceBindCurrentFBO(FramebufferTarget::Read);
        ForceBindCurrentFBO(FramebufferTarget::Draw);
    }

    Bool UpdateTextureBindingAtTarget(GLenum target) {
#ifdef TRACY_ENABLE
        ZoneScopedNC(__func__, TRACY_ZONECOLOR_BACKEND);
#endif
        auto unit = MG_State::pGLContext->GetActiveTextureUnit();
        auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);

        auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::IsSupportedTextureTarget(textureTarget)) {
            MGLOG_E("    Texture target %s is not supported, skipping.",
                    MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
            return false;
        }

        const auto& bindingSlot = textureUnit.GetBindingSlot(textureTarget);
        {
            const auto& textureObject = bindingSlot.GetBoundObject();
            if (!textureObject) {
                MGLOG_W("%s: Texture target %s does not have texture bound.", __func__,
                        MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
            }

            const auto& backendTextureIt = TextureImpl::g_backendTextureObjects.find(textureObject.get());
            Bool exist = (backendTextureIt != TextureImpl::g_backendTextureObjects.end());
            auto& backendObj =
                exist ? backendTextureIt->second : TextureImpl::g_backendTextureObjects.GetOrCreate(textureObject);
            if (!exist) {
                backendObj = MakeShared<TextureImpl::BackendTextureObject>();
            }
            backendObj->Bind(TextureImpl::ConvertTextureTargetToBackendGLEnum(textureTarget), unit);
        }
        return true;
    }

    // ---- Scoped driver-state guards for the readback/copy/blit emulation paths --------------------
    // These paths borrow driver state (FBO bindings, scratch-FBO attachments, PACK
    // pixel-store, the pack-PBO binding, scissor) that the app never asked to
    // change; every mutation is scoped by an RAII guard so no exit path can leak
    // it. Saves/restores go through the DirectGLES driver-state shadows
    // (Managers.h) instead of glGetIntegerv - no driver round-trips, and redundant
    // rebinds/resets no-op.

    // Saves the driver READ/DRAW framebuffer binding(s) and restores them on exit.
    // Per-instance state: nesting-safe.
    class ScopedFramebufferBinding {
    public:
        ScopedFramebufferBinding(Bool saveRead, Bool saveDraw) : m_saveRead(saveRead), m_saveDraw(saveDraw) {
            if (m_saveRead) m_prevRead = FramebufferImpl::CurrentFramebufferBinding(FramebufferTarget::Read);
            if (m_saveDraw) m_prevDraw = FramebufferImpl::CurrentFramebufferBinding(FramebufferTarget::Draw);
        }
        ~ScopedFramebufferBinding() {
            if (m_saveRead) FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, m_prevRead);
            if (m_saveDraw) FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, m_prevDraw);
        }
        ScopedFramebufferBinding(const ScopedFramebufferBinding&) = delete;
        ScopedFramebufferBinding& operator=(const ScopedFramebufferBinding&) = delete;

    private:
        const Bool m_saveRead;
        const Bool m_saveDraw;
        GLuint m_prevRead = 0;
        GLuint m_prevDraw = 0;
    };

    // Applies a PACK pixel-store configuration and restores the previous one on exit.
    class ScopedPackState {
    public:
        explicit ScopedPackState(const PixelStoreImpl::PackState& desired)
            : m_prev(PixelStoreImpl::CurrentPackState()) {
            PixelStoreImpl::ApplyPackState(desired);
        }
        ~ScopedPackState() { PixelStoreImpl::ApplyPackState(m_prev); }
        ScopedPackState(const ScopedPackState&) = delete;
        ScopedPackState& operator=(const ScopedPackState&) = delete;

    private:
        const PixelStoreImpl::PackState m_prev;
    };

    // The frontend's current PACK parameters, for readbacks the ES driver serves
    // directly with the client's layout.
    static PixelStoreImpl::PackState PackStateFromContext() {
        const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
        return {static_cast<GLint>(packParams.Alignment), static_cast<GLint>(packParams.RowLength),
                static_cast<GLint>(packParams.SkipRows), static_cast<GLint>(packParams.SkipPixels)};
    }

    // Binds a pixel PACK buffer (0 = client memory) for the scope and returns the
    // binding to the resting 0 state on exit, so no later readback can accidentally
    // capture into a stale PBO.
    class ScopedPixelPackBuffer {
    public:
        explicit ScopedPixelPackBuffer(GLuint id) { BufferImpl::BindPixelPackBufferId(id); }
        ~ScopedPixelPackBuffer() { BufferImpl::BindPixelPackBufferId(0); }
        ScopedPixelPackBuffer(const ScopedPixelPackBuffer&) = delete;
        ScopedPixelPackBuffer& operator=(const ScopedPixelPackBuffer&) = delete;
    };

    // Force-disables GL_SCISSOR_TEST for the scope (emulation blits and clears are
    // scissored; readback copies must not be clipped by app scissor state) and
    // restores the app state on exit, tracked via the render-state shadow.
    class ScopedScissorDisable {
    public:
        ScopedScissorDisable() : m_wasEnabled(RenderStateImpl::g_syncedRenderStateParameters.ScissorTestEnabled) {
            if (m_wasEnabled) g_GLESFuncs.glDisable(GL_SCISSOR_TEST);
        }
        ~ScopedScissorDisable() {
            if (m_wasEnabled) g_GLESFuncs.glEnable(GL_SCISSOR_TEST);
        }
        ScopedScissorDisable(const ScopedScissorDisable&) = delete;
        ScopedScissorDisable& operator=(const ScopedScissorDisable&) = delete;

    private:
        const Bool m_wasEnabled;
    };

    // Binds the shared scratch FBO at READ (isRead) or DRAW for one temp operation,
    // restoring the previous binding on exit. Attachments are managed through the
    // ScratchFBOImpl attachment shadow by the caller (see Framebuffer()).
    class TempFBOBinder {
    public:
        explicit TempFBOBinder(Bool isRead)
            : m_binding(/*saveRead=*/isRead, /*saveDraw=*/!isRead),
              m_target(isRead ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER) {
            FramebufferImpl::BindFramebufferId(m_target, ScratchFBOImpl::EnsureId(Framebuffer()));
        }
        ScratchFBOImpl::ScratchFramebuffer& Framebuffer() const { return ScratchFBOImpl::TempFramebuffer(); }
        GLenum Target() const { return m_target; }

    private:
        ScopedFramebufferBinding m_binding;
        const GLenum m_target;
    };

    static Bool IsDepthOnlyFormat(TextureInternalFormat format) {
        return MG_Util::IsDepthFormatInternalFormat(format) && !MG_Util::IsStencilFormatInternalFormat(format);
    }

    static Bool IsColorOnlyFormat(TextureInternalFormat format) {
        return !MG_Util::IsDepthFormatInternalFormat(format) && !MG_Util::IsStencilFormatInternalFormat(format);
    }

    static Bool IsIntegerColorFormat(TextureInternalFormat format) {
        switch (format) {
        case TextureInternalFormat::RGB10A2UI:
        case TextureInternalFormat::R8I:
        case TextureInternalFormat::R8UI:
        case TextureInternalFormat::R16I:
        case TextureInternalFormat::R16UI:
        case TextureInternalFormat::R32I:
        case TextureInternalFormat::R32UI:
        case TextureInternalFormat::RG8I:
        case TextureInternalFormat::RG8UI:
        case TextureInternalFormat::RG16I:
        case TextureInternalFormat::RG16UI:
        case TextureInternalFormat::RG32I:
        case TextureInternalFormat::RG32UI:
        case TextureInternalFormat::RGB8I:
        case TextureInternalFormat::RGB8UI:
        case TextureInternalFormat::RGB16I:
        case TextureInternalFormat::RGB16UI:
        case TextureInternalFormat::RGB32I:
        case TextureInternalFormat::RGB32UI:
        case TextureInternalFormat::RGBA8I:
        case TextureInternalFormat::RGBA8UI:
        case TextureInternalFormat::RGBA16I:
        case TextureInternalFormat::RGBA16UI:
        case TextureInternalFormat::RGBA32I:
        case TextureInternalFormat::RGBA32UI:
            return true;
        default:
            return false;
        }
    }

    static Uint ComputeFullMipmapLevelCount(const IntVec3& baseTexelSize) {
        Int maxDimension = std::max<Int>(
            baseTexelSize.x(),
            std::max<Int>(baseTexelSize.y(), std::max<Int>(baseTexelSize.z(), 1)));
        Uint mipLevelCount = 1;
        while (maxDimension > 1) {
            maxDimension = std::max<Int>(maxDimension / 2, 1);
            ++mipLevelCount;
        }
        return mipLevelCount;
    }

    static IntVec3 ComputeMipmapTexelSize(const IntVec3& baseTexelSize, Uint relativeLevel) {
        return {
            std::max<Int>(baseTexelSize.x() >> static_cast<Int>(relativeLevel), 1),
            std::max<Int>(baseTexelSize.y() >> static_cast<Int>(relativeLevel), 1),
            std::max<Int>(baseTexelSize.z() >> static_cast<Int>(relativeLevel), 1),
        };
    }

    static Bool EnsureGenerateMipmapStorageAllocated(MG_State::GLState::TextureObjectMipmap& texture,
                                                    TextureUploadTarget uploadTarget, Bool& allocatedStorage) {
        const Uint existingLevelCount = texture.GetMipmapLevelCount();
        if (existingLevelCount == 0) {
            return false;
        }

        const IntVec3 baseTexelSize = texture.GetMipmapTexelSize(uploadTarget, 0);
        const SizeT baseByteSize = texture.GetMipmapByteSize(uploadTarget, 0);
        const SizeT baseTexelCount = static_cast<SizeT>(baseTexelSize.x()) *
                                     static_cast<SizeT>(baseTexelSize.y()) *
                                     static_cast<SizeT>(baseTexelSize.z());
        if (baseTexelSize.x() <= 0 || baseTexelSize.y() <= 0 || baseTexelSize.z() <= 0 ||
            baseByteSize == 0 || baseTexelCount == 0 || (baseByteSize % baseTexelCount) != 0) {
            return false;
        }

        const SizeT bytesPerTexel = baseByteSize / baseTexelCount;
        const Uint requiredLevelCount = ComputeFullMipmapLevelCount(baseTexelSize);
        if (existingLevelCount < requiredLevelCount) {
            allocatedStorage = true;
        }
        for (Uint level = existingLevelCount; level < requiredLevelCount; ++level) {
            const IntVec3 levelTexelSize = ComputeMipmapTexelSize(baseTexelSize, level);
            const SizeT levelByteSize = bytesPerTexel * static_cast<SizeT>(levelTexelSize.x()) *
                                        static_cast<SizeT>(levelTexelSize.y()) *
                                        static_cast<SizeT>(levelTexelSize.z());
            texture.AllocateStorage(uploadTarget, level, {levelTexelSize, levelByteSize});
            texture.MarkStorageDirty(uploadTarget, level, false);
        }
        return true;
    }

    static Bool EnsureGenerateMipmapStorageAllocated(const SharedPtr<MG_State::GLState::ITextureObject>& texture) {
        auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(texture.get());
        MOBILEGL_ASSERT(mipmapTexture != nullptr, "GenerateMipmap requires mipmap texture storage.");
        Bool allocatedStorage = false;
        for (const TextureUploadTarget uploadTarget : texture->GetUploadTargets()) {
            const Bool allocated = EnsureGenerateMipmapStorageAllocated(*mipmapTexture, uploadTarget, allocatedStorage);
            MOBILEGL_ASSERT(allocated, "GenerateMipmap could not allocate generated mipmap storage.");
        }
        return allocatedStorage;
    }

    static void AssertNoGLError(const char* operation) {
        const GLenum err = g_GLESFuncs.glGetError();
        MOBILEGL_ASSERT(err == GL_NO_ERROR, "%s failed: %s", operation,
                        MG_Util::ConvertGLEnumToString(err).c_str());
    }

    static ErrorCode ConvertGLESErrorToErrorCode(GLenum err) {
        switch (err) {
        case GL_INVALID_ENUM:
            return ErrorCode::InvalidEnum;
        case GL_INVALID_VALUE:
            return ErrorCode::InvalidValue;
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return ErrorCode::InvalidFramebufferOperation;
        case GL_OUT_OF_MEMORY:
            return ErrorCode::OutOfMemory;
        case GL_INVALID_OPERATION:
        default:
            return ErrorCode::InvalidOperation;
        }
    }

    static Bool RecordGLError(const char* operation, GLenum target, TextureInternalFormat format) {
        const GLenum err = g_GLESFuncs.glGetError();
        if (err == GL_NO_ERROR) {
            return true;
        }

        MGLOG_E("%s failed: %s. target=%s, format=%s", operation,
                MG_Util::ConvertGLEnumToString(err).c_str(),
                MG_Util::ConvertGLEnumToString(target).c_str(),
                MG_Util::ConvertTextureInternalFormatToString(format).c_str());
        MG_State::pGLContext->RecordError(
            ConvertGLESErrorToErrorCode(err),
            MakeUnique<GenericErrorInfo>("DirectGLES", operation,
                                         MG_Util::ConvertGLEnumToString(err)));
        return false;
    }

    static void ClearGLErrors() {
        while (g_GLESFuncs.glGetError() != GL_NO_ERROR) {}
    }

    // Binds a guaranteed-complete 1x1 scratch framebuffer at both targets for the
    // scope (GenerateMipmap must respecify texture storage while no incomplete
    // user FBO is bound); restores the previous bindings on exit.
    class ScopedCompleteFramebufferBinding {
    public:
        ScopedCompleteFramebufferBinding() : m_binding(/*saveRead=*/true, /*saveDraw=*/true) {
            FramebufferImpl::BindFramebufferId(GL_FRAMEBUFFER, ScratchFBOImpl::EnsureCompleteTinyFramebufferId());
        }

    private:
        ScopedFramebufferBinding m_binding;
    };

    class ScopedDetachedTextureFramebufferAttachments {
    public:
        explicit ScopedDetachedTextureFramebufferAttachments(
            const SharedPtr<MG_State::GLState::ITextureObject>& texture) {
            if (texture == nullptr) {
                return;
            }

            const auto backendTextureIt = TextureImpl::g_backendTextureObjects.find(texture.get());
            if (backendTextureIt == TextureImpl::g_backendTextureObjects.end() || !backendTextureIt->second) {
                return;
            }
            const GLuint backendTextureId = backendTextureIt->second->GetBackendTextureId();

            for (auto it = FramebufferImpl::g_backendFramebufferObjects.begin();
                 it != FramebufferImpl::g_backendFramebufferObjects.end(); ++it) {
                auto* stateFBO = it->first;
                const auto& backendFBO = it->second;
                if (stateFBO == nullptr || !backendFBO || stateFBO->IsDefaultFramebuffer()) {
                    continue;
                }

                const auto& attachments = stateFBO->GetAllAttachmentObjects();
                for (SizeT i = 0; i < attachments.size(); ++i) {
                    const auto& attachmentObject = attachments[i];
                    if (!attachmentObject.IsTexture() || attachmentObject.GetTexture().get() != texture.get()) {
                        continue;
                    }

                    const auto frontendType = static_cast<FramebufferAttachmentType>(i);
                    GLenum backendAttachment = GL_NONE;
                    if (frontendType >= FramebufferAttachmentType::Color0 &&
                        frontendType <= FramebufferAttachmentType::Color31) {
                        backendAttachment = backendFBO->GetBackendAttachmentType(frontendType);
                    } else {
                        backendAttachment = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendType);
                    }
                    if (backendAttachment == GL_NONE || backendAttachment == GL_UNKNOWN_MGL) {
                        continue;
                    }

                    GLenum textureTarget = TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(
                        attachmentObject.GetTextureUploadTarget());
                    if (textureTarget == GL_UNKNOWN_MGL) {
                        textureTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(texture->GetTarget());
                    }

                    const GLuint backendFBOId = backendFBO->GetBackendFramebufferId();
                    FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, backendFBOId);
                    if (attachmentObject.IsLayered()) {
                        g_GLESFuncs.glFramebufferTexture(GL_DRAW_FRAMEBUFFER, backendAttachment, 0, 0);
                    } else {
                        g_GLESFuncs.glFramebufferTexture2D(
                            GL_DRAW_FRAMEBUFFER, backendAttachment, textureTarget, 0, 0);
                    }
                    ClearGLErrors();
                    m_detachedAttachments.push_back(
                        {backendFBOId, backendAttachment, textureTarget, backendTextureId,
                         static_cast<GLint>(attachmentObject.GetTextureLevel()), attachmentObject.IsLayered()});
                }
            }
        }

        ~ScopedDetachedTextureFramebufferAttachments() {
            for (const auto& attachment : m_detachedAttachments) {
                FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, attachment.framebuffer);
                if (attachment.layered) {
                    g_GLESFuncs.glFramebufferTexture(
                        GL_DRAW_FRAMEBUFFER, attachment.attachment, attachment.texture, attachment.level);
                } else {
                    g_GLESFuncs.glFramebufferTexture2D(
                        GL_DRAW_FRAMEBUFFER, attachment.attachment, attachment.textureTarget,
                        attachment.texture, attachment.level);
                }
            }
        }

    private:
        struct DetachedAttachment {
            GLuint framebuffer = 0;
            GLenum attachment = GL_NONE;
            GLenum textureTarget = GL_TEXTURE_2D;
            GLuint texture = 0;
            GLint level = 0;
            Bool layered = false;
        };

        // Declared first so its restore runs after the reattach loop in the dtor.
        ScopedFramebufferBinding m_binding{/*saveRead=*/true, /*saveDraw=*/true};
        Vector<DetachedAttachment> m_detachedAttachments;
    };

    // Binds the scratch blit READ/DRAW framebuffers with scissor forced off (blits
    // are scissored) for one texture-to-texture copy; restores the bindings and the
    // scissor state on exit. Attachments on the two scratch FBOs are managed by the
    // blit helpers through the ScratchFBOImpl attachment shadow.
    class ScopedDepthBlitState {
    public:
        ScopedDepthBlitState() : m_binding(/*saveRead=*/true, /*saveDraw=*/true) {
            FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER,
                                               ScratchFBOImpl::EnsureId(ScratchFBOImpl::BlitReadFramebuffer()));
            AssertNoGLError("bind depth blit read framebuffer");
            FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER,
                                               ScratchFBOImpl::EnsureId(ScratchFBOImpl::BlitDrawFramebuffer()));
            AssertNoGLError("bind depth blit draw framebuffer");
        }

    private:
        ScopedScissorDisable m_scissorOff;
        ScopedFramebufferBinding m_binding;
    };

    static void BlitDepthTexture2D(GLuint srcTexture, GLint srcLevel, GLint srcX, GLint srcY, GLsizei srcWidth,
                                   GLsizei srcHeight, GLuint dstTexture, GLint dstLevel, GLint dstX, GLint dstY,
                                   GLsizei dstWidth, GLsizei dstHeight) {
        MOBILEGL_ASSERT(srcTexture != 0 && dstTexture != 0, "Depth blit requires valid backend textures.");
        MOBILEGL_ASSERT(srcLevel >= 0 && dstLevel >= 0, "Depth blit mip levels must be non-negative.");
        MOBILEGL_ASSERT(srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0,
                        "Depth blit dimensions must be positive.");

        ClearGLErrors();
        ScopedDepthBlitState state;
        auto& readFB = ScratchFBOImpl::BlitReadFramebuffer();
        auto& drawFB = ScratchFBOImpl::BlitDrawFramebuffer();
        ScratchFBOImpl::EnsureDepthAttachment2D(readFB, GL_READ_FRAMEBUFFER, srcTexture, GL_TEXTURE_2D, srcLevel,
                                                /*withStencil=*/false);
        AssertNoGLError("attach depth blit source texture");
        ScratchFBOImpl::EnsureDepthAttachment2D(drawFB, GL_DRAW_FRAMEBUFFER, dstTexture, GL_TEXTURE_2D, dstLevel,
                                                /*withStencil=*/false);
        AssertNoGLError("attach depth blit destination texture");
        ScratchFBOImpl::EnsureReadBuffer(readFB, GL_NONE);
        AssertNoGLError("set depth blit read buffer");
        ScratchFBOImpl::EnsureDrawBuffer(drawFB, GL_NONE);
        AssertNoGLError("set depth blit draw buffer");
        MOBILEGL_ASSERT(g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                        "Depth blit read framebuffer is incomplete.");
        AssertNoGLError("check depth blit read framebuffer");
        MOBILEGL_ASSERT(g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                        "Depth blit draw framebuffer is incomplete.");
        AssertNoGLError("check depth blit draw framebuffer");

        g_GLESFuncs.glBlitFramebuffer(srcX, srcY, srcX + srcWidth, srcY + srcHeight,
                                      dstX, dstY, dstX + dstWidth, dstY + dstHeight,
                                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        AssertNoGLError("depth texture blit");
    }

    static void BlitColorTexture2D(GLuint srcTexture, GLint srcLevel, GLint srcX, GLint srcY, GLsizei srcWidth,
                                   GLsizei srcHeight, GLuint dstTexture, GLint dstLevel, GLint dstX, GLint dstY,
                                   GLsizei dstWidth, GLsizei dstHeight, GLenum filter) {
        MOBILEGL_ASSERT(srcTexture != 0 && dstTexture != 0, "Color blit requires valid backend textures.");
        MOBILEGL_ASSERT(srcLevel >= 0 && dstLevel >= 0, "Color blit mip levels must be non-negative.");
        MOBILEGL_ASSERT(srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0,
                        "Color blit dimensions must be positive.");
        MOBILEGL_ASSERT(filter == GL_NEAREST || filter == GL_LINEAR, "Color blit filter must be nearest or linear.");

        ClearGLErrors();
        ScopedDepthBlitState state;
        auto& readFB = ScratchFBOImpl::BlitReadFramebuffer();
        auto& drawFB = ScratchFBOImpl::BlitDrawFramebuffer();
        ScratchFBOImpl::EnsureColorAttachment2D(readFB, GL_READ_FRAMEBUFFER, srcTexture, GL_TEXTURE_2D, srcLevel);
        AssertNoGLError("attach color blit source texture");
        ScratchFBOImpl::EnsureColorAttachment2D(drawFB, GL_DRAW_FRAMEBUFFER, dstTexture, GL_TEXTURE_2D, dstLevel);
        AssertNoGLError("attach color blit destination texture");
        ScratchFBOImpl::EnsureReadBuffer(readFB, GL_COLOR_ATTACHMENT0);
        AssertNoGLError("set color blit read buffer");
        ScratchFBOImpl::EnsureDrawBuffer(drawFB, GL_COLOR_ATTACHMENT0);
        AssertNoGLError("set color blit draw buffer");
        MOBILEGL_ASSERT(g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                        "Color blit read framebuffer is incomplete.");
        AssertNoGLError("check color blit read framebuffer");
        MOBILEGL_ASSERT(g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                        "Color blit draw framebuffer is incomplete.");
        AssertNoGLError("check color blit draw framebuffer");

        g_GLESFuncs.glBlitFramebuffer(srcX, srcY, srcX + srcWidth, srcY + srcHeight,
                                      dstX, dstY, dstX + dstWidth, dstY + dstHeight,
                                      GL_COLOR_BUFFER_BIT, filter);
        AssertNoGLError("color texture blit");
    }

    static void CopyR32FTexture2D(GLuint srcTexture, GLint srcLevel, GLint srcX, GLint srcY, GLsizei width,
                                  GLsizei height, GLuint dstTexture, GLenum dstTarget, GLint dstLevel, GLint dstX,
                                  GLint dstY) {
        MOBILEGL_ASSERT(srcTexture != 0 && dstTexture != 0, "R32F copy requires valid backend textures.");
        MOBILEGL_ASSERT(dstTarget == GL_TEXTURE_2D, "R32F copy only supports GL_TEXTURE_2D destinations.");
        MOBILEGL_ASSERT(srcLevel >= 0 && dstLevel >= 0, "R32F copy mip levels must be non-negative.");
        MOBILEGL_ASSERT(width > 0 && height > 0, "R32F copy dimensions must be positive.");

        ClearGLErrors();
        ScopedDepthBlitState state;
        auto& readFB = ScratchFBOImpl::BlitReadFramebuffer();
        ScratchFBOImpl::EnsureColorAttachment2D(readFB, GL_READ_FRAMEBUFFER, srcTexture, GL_TEXTURE_2D, srcLevel);
        AssertNoGLError("attach R32F copy source texture");
        ScratchFBOImpl::EnsureReadBuffer(readFB, GL_COLOR_ATTACHMENT0);
        AssertNoGLError("set R32F copy read buffer");
        MOBILEGL_ASSERT(g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                        "R32F copy read framebuffer is incomplete.");
        AssertNoGLError("check R32F copy read framebuffer");

        Vector<Float> pixels(static_cast<SizeT>(width) * static_cast<SizeT>(height));
        {
            ScopedPixelPackBuffer packBuffer(0);
            ScopedPackState packState(PixelStoreImpl::PackState{4, 0, 0, 0});
            g_GLESFuncs.glReadPixels(srcX, srcY, width, height, GL_RED, GL_FLOAT, pixels.data());
            AssertNoGLError("read R32F copy pixels");
        }

        // Upload side: the rows are tightly packed floats, which the resting driver
        // UNPACK state (4/0/0/0, maintained by ScopedDefaultUnpackState) parses
        // correctly; the unpack-PBO binding rests at 0 by the same discipline (the
        // call below no-ops unless something diverged).
        BufferImpl::BindPixelUnpackBufferId(0);
        TextureImpl::ActivateTextureUnit(TextureImpl::TempTextureUnit);
        g_GLESFuncs.glBindTexture(dstTarget, dstTexture);
        g_GLESFuncs.glTexSubImage2D(dstTarget, dstLevel, dstX, dstY, width, height, GL_RED, GL_FLOAT, pixels.data());
        AssertNoGLError("upload R32F copy pixels");
        // Re-bind what the texture-binding cache says lives on the temp unit so the
        // cache stays truthful without a driver query.
        auto* cachedBound =
            TextureImpl::g_boundTexturesCache[TextureImpl::TempTextureUnit]
                                             [static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(dstTarget))];
        g_GLESFuncs.glBindTexture(dstTarget, cachedBound ? cachedBound->GetBackendTextureId() : 0);
    }

    static void GenerateDepthTexture2DMipmap(
        const SharedPtr<MG_State::GLState::ITextureObject>& texture,
        const SharedPtr<TextureImpl::BackendTextureObject>& backendTexture) {
        MOBILEGL_ASSERT(texture != nullptr && backendTexture != nullptr, "GenerateDepthTexture2DMipmap needs texture.");
        MOBILEGL_ASSERT(texture->GetTarget() == TextureTarget::Texture2D,
                        "DirectGLES depth mipmap generation only supports GL_TEXTURE_2D.");
        MOBILEGL_ASSERT(IsDepthOnlyFormat(texture->GetFormat()),
                        "DirectGLES depth mipmap generation requires a depth-only texture.");

        auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(texture.get());
        MOBILEGL_ASSERT(mipmapTexture != nullptr, "Depth mipmap generation requires mipmap storage.");
        const Uint mipLevelCount = mipmapTexture->GetMipmapLevelCount();
        MOBILEGL_ASSERT(mipLevelCount > 0, "Depth mipmap generation requires allocated storage.");

        const GLuint textureId = backendTexture->GetBackendTextureId();
        for (Uint level = 1; level < mipLevelCount; ++level) {
            const IntVec3 srcSize = mipmapTexture->GetMipmapTexelSize(TextureUploadTarget::Texture2D, level - 1);
            const IntVec3 dstSize = mipmapTexture->GetMipmapTexelSize(TextureUploadTarget::Texture2D, level);
            BlitDepthTexture2D(textureId, static_cast<GLint>(level - 1), 0, 0,
                               static_cast<GLsizei>(srcSize.x()), static_cast<GLsizei>(srcSize.y()),
                               textureId, static_cast<GLint>(level), 0, 0,
                               static_cast<GLsizei>(dstSize.x()), static_cast<GLsizei>(dstSize.y()));
        }
    }

    static void GenerateColorTexture2DMipmap(
        const SharedPtr<MG_State::GLState::ITextureObject>& texture,
        const SharedPtr<TextureImpl::BackendTextureObject>& backendTexture) {
        MOBILEGL_ASSERT(texture != nullptr && backendTexture != nullptr, "GenerateColorTexture2DMipmap needs texture.");
        MOBILEGL_ASSERT(texture->GetTarget() == TextureTarget::Texture2D,
                        "DirectGLES color mipmap generation only supports GL_TEXTURE_2D.");
        MOBILEGL_ASSERT(IsColorOnlyFormat(texture->GetFormat()),
                        "DirectGLES color mipmap generation requires a color-only texture.");

        auto* mipmapTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(texture.get());
        MOBILEGL_ASSERT(mipmapTexture != nullptr, "Color mipmap generation requires mipmap storage.");
        const Uint mipLevelCount = mipmapTexture->GetMipmapLevelCount();
        MOBILEGL_ASSERT(mipLevelCount > 0, "Color mipmap generation requires allocated storage.");

        const GLenum filter = IsIntegerColorFormat(texture->GetFormat()) ? GL_NEAREST : GL_LINEAR;
        const GLuint textureId = backendTexture->GetBackendTextureId();
        for (Uint level = 1; level < mipLevelCount; ++level) {
            const IntVec3 srcSize = mipmapTexture->GetMipmapTexelSize(TextureUploadTarget::Texture2D, level - 1);
            const IntVec3 dstSize = mipmapTexture->GetMipmapTexelSize(TextureUploadTarget::Texture2D, level);
            BlitColorTexture2D(textureId, static_cast<GLint>(level - 1), 0, 0,
                               static_cast<GLsizei>(srcSize.x()), static_cast<GLsizei>(srcSize.y()),
                               textureId, static_cast<GLint>(level), 0, 0,
                               static_cast<GLsizei>(dstSize.x()), static_cast<GLsizei>(dstSize.y()), filter);
        }
    }

    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DebugImpl::ErrorLopper errorLopper;
        MGLOG_D("%s: Backend", __func__);
        TextureImpl::SyncNeccessaryTextures();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        FramebufferImpl::SyncCurrentFBO();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        RenderStateImpl::SyncRenderState();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        if (!UpdateTextureBindingAtTarget(target)) return;

        // Bind necessary FBO and texture
        BindCurrentFBO(FramebufferTarget::Read);
        Uint activeTextureUnit = MG_State::pGLContext->GetActiveTextureUnit();
        const auto& textureObject = MG_State::pGLContext->GetTextureUnitObject((Int)activeTextureUnit)
                                        .GetBindingSlot(MG_Util::ConvertGLEnumToTextureTarget(target))
                                        .GetBoundObject();
        const auto& backendTextureIt = TextureImpl::g_backendTextureObjects.find(textureObject.get());
        if (backendTextureIt == TextureImpl::g_backendTextureObjects.end()) {
            MGLOG_E("CopyTexSubImage2D: No backend texture found for texture %u.",
                    textureObject ? textureObject->GetExternalIndex() : 0);
            return;
        }
        backendTextureIt->second->Bind(target, activeTextureUnit);

        auto mgInternalFormat = textureObject->GetFormat();
        GLenum format = GL_DEPTH_COMPONENT;
        GLenum type = GL_UNSIGNED_INT;
        TextureImpl::GenerateTextureFormatInfo(mgInternalFormat, &internalformat, &format, &type,
                                               MG_Util::ConvertGLEnumToTextureTarget(target));
        MOBILEGL_ASSERT(format != GL_NONE && type != GL_NONE,
                        "%s: cannot GenerateTextureFormatInfo(%s): out internalformat=%s, format=%s, type=%s",
                        MG_Util::ConvertTextureInternalFormatToString(mgInternalFormat).c_str(),
                        MG_Util::ConvertGLEnumToString(internalformat).c_str(),
                        MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        Bool isDepthFormat =
            MG_Util::IsDepthFormatInternalFormat(MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat));
        Bool isStencilFormat =
            MG_Util::IsStencilFormatInternalFormat(MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat));

        if (!isDepthFormat) {
            g_GLESFuncs.glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
        } else {
            MGLOG_D("%s: Backend depth", __func__);
            // nullptr means "uninitialized storage" only while no unpack PBO is
            // bound; enforce the resting 0 state instead of assuming it (no-op
            // through the binding cache unless something diverged).
            BufferImpl::BindPixelUnpackBufferId(0);
            g_GLESFuncs.glTexImage2D(target, level, (GLint)internalformat, width, height, border, format, type,
                                     nullptr);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            auto currentTex = (GLint)backendTextureIt->second->GetBackendTextureId();
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            TempFBOBinder tempFBOBinder(false);
            ScopedScissorDisable scissorOff; // the depth-copy blit below is scissored like any blit
            ScratchFBOImpl::EnsureDepthAttachment2D(tempFBOBinder.Framebuffer(), GL_DRAW_FRAMEBUFFER,
                                                    static_cast<Uint>(currentTex), target, level, isStencilFormat);

            if (g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                MGLOG_E("ES glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE");
                return;
            }

            g_GLESFuncs.glBlitFramebuffer(x, y, x + width, y + height, 0, 0, width, height,
                                          GL_DEPTH_BUFFER_BIT | (isStencilFormat ? GL_STENCIL_BUFFER_BIT : 0),
                                          GL_NEAREST);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
        }
    }

    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DebugImpl::ErrorLopper errorLopper;

        MGLOG_D("%s: Backend", __func__);
        TextureImpl::SyncNeccessaryTextures();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        FramebufferImpl::SyncCurrentFBO();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        RenderStateImpl::SyncRenderState();
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        if (!UpdateTextureBindingAtTarget(target)) return;

        // Bind necessary FBO and texture
        BindCurrentFBO(FramebufferTarget::Read);
        auto activeTextureUnit = MG_State::pGLContext->GetActiveTextureUnit();
        const auto& textureObject = MG_State::pGLContext->GetTextureUnitObject(activeTextureUnit)
                                        .GetBindingSlot(MG_Util::ConvertGLEnumToTextureTarget(target))
                                        .GetBoundObject();
        const auto& backendTextureIt = TextureImpl::g_backendTextureObjects.find(textureObject.get());
        if (backendTextureIt == TextureImpl::g_backendTextureObjects.end()) {
            MGLOG_E("CopyTexSubImage2D: No backend texture found for texture %u.",
                    textureObject ? textureObject->GetExternalIndex() : 0);
            return;
        }
        backendTextureIt->second->Bind(target, activeTextureUnit);

        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        GLenum internalFormat;
        g_GLESFuncs.glGetTexLevelParameteriv(target, level, GL_TEXTURE_INTERNAL_FORMAT, (GLint*)&internalFormat);
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        auto mgInternalFormat = MG_Util::ConvertGLEnumToTextureInternalFormat(internalFormat);

        Bool isDepthFormat = MG_Util::IsDepthFormatInternalFormat(mgInternalFormat);
        Bool isStencilFormat = MG_Util::IsStencilFormatInternalFormat(mgInternalFormat);

        if (!isDepthFormat) {
            g_GLESFuncs.glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
        } else {
            MGLOG_D("%s: Backend depth", __func__);
            auto currentTex = backendTextureIt->second->GetBackendTextureId();
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            TempFBOBinder tempFBOBinder(false);
            ScopedScissorDisable scissorOff; // the depth-copy blit below is scissored like any blit
            ScratchFBOImpl::EnsureDepthAttachment2D(tempFBOBinder.Framebuffer(), GL_DRAW_FRAMEBUFFER, currentTex,
                                                    target, level, isStencilFormat);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            if (g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                MGLOG_E("ES glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE");
                return;
            }

            g_GLESFuncs.glBlitFramebuffer(
                x, y, x + width, y + height, xoffset, yoffset, xoffset + width, yoffset + height,
                GL_DEPTH_BUFFER_BIT | (isStencilFormat ? GL_STENCIL_BUFFER_BIT : 0), GL_NEAREST);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
        }
    }

    void GenerateMipmap(GLenum target) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        auto unitIndex = MG_State::pGLContext->GetActiveTextureUnit();
        auto& unit = MG_State::pGLContext->GetTextureUnitObject(unitIndex);
        auto& slot = unit.GetBindingSlot(MG_Util::ConvertGLEnumToTextureTarget(target));
        auto& texture = slot.GetBoundObject();
        MOBILEGL_ASSERT(texture != nullptr, "GenerateMipmap requires a bound texture.");
        if (texture->GetFormat() == TextureInternalFormat::R11FG11FB10F || IsDepthOnlyFormat(texture->GetFormat())) {
            EnsureGenerateMipmapStorageAllocated(texture);
        }
        auto& backendTexture = TextureImpl::SyncTextureObjectToBackend(texture);

        if (IsDepthOnlyFormat(texture->GetFormat())) {
            GenerateDepthTexture2DMipmap(texture, backendTexture);
            return;
        }
        if (texture->GetFormat() == TextureInternalFormat::R11FG11FB10F &&
            texture->GetTarget() == TextureTarget::Texture2D) {
            GenerateColorTexture2DMipmap(texture, backendTexture);
            return;
        }

        const GLenum backendTarget =
            TextureImpl::ConvertTextureTargetToBackendGLEnum(MG_Util::ConvertGLEnumToTextureTarget(target));
        backendTexture->Bind(backendTarget, unitIndex);
        // ANGLE/Mesa may validate the currently bound FBO while generating mipmaps.
        // Also detach the source texture from synced FBO objects for ANGLE's validation.
        ScopedDetachedTextureFramebufferAttachments detachedAttachments(texture);
        // Bind a complete internal FBO that does not reference the source texture.
        ScopedCompleteFramebufferBinding completeFramebuffer;
        // ErrorLopper is compiled out at the default log level; RecordGLError below
        // forwards the next queued error to the APP, so stale flags from earlier
        // best-effort calls must be drained by the always-live helper.
        ClearGLErrors();
        g_GLESFuncs.glGenerateMipmap(backendTarget);
        RecordGLError("glGenerateMipmap", backendTarget, texture->GetFormat());
    }

    const GLubyte* GetString(GLenum name) {
        return g_GLESFuncs.glGetString(name);
    }

    void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        PrepareForCompute(false);
        g_GLESFuncs.glDispatchCompute(numGroupsX, numGroupsY, numGroupsZ);
    }

    void DispatchComputeIndirect(GLintptr indirect) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        PrepareForCompute(true);
        g_GLESFuncs.glDispatchComputeIndirect(indirect);
    }

    void MemoryBarrier(GLbitfield barriers) {
        g_GLESFuncs.glMemoryBarrier(barriers);
        if (g_GLESCapabilities.IsAngleRenderer) {
            g_GLESFuncs.glFlush();
        }
    }

    void MemoryBarrierByRegion(GLbitfield barriers) {
        g_GLESFuncs.glMemoryBarrierByRegion(barriers);
    }

    void CopyImageSubData(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        auto& srcBackendTexture = TextureImpl::SyncTextureObjectToBackend(srcTexture);
        auto& dstBackendTexture = TextureImpl::SyncTextureObjectToBackend(dstTexture);

        const Bool srcIsDepth = MG_Util::IsDepthFormatInternalFormat(srcTexture->GetFormat());
        const Bool dstIsDepth = MG_Util::IsDepthFormatInternalFormat(dstTexture->GetFormat());
        const Bool srcStencil = MG_Util::IsStencilFormatInternalFormat(srcTexture->GetFormat());
        const Bool dstStencil = MG_Util::IsStencilFormatInternalFormat(dstTexture->GetFormat());
        if (srcIsDepth || dstIsDepth || srcStencil || dstStencil) {
            MOBILEGL_ASSERT(srcIsDepth && dstIsDepth && !srcStencil && !dstStencil,
                            "DirectGLES CopyImageSubData only supports depth-only image copies.");
            MOBILEGL_ASSERT(srcTarget == GL_TEXTURE_2D && dstTarget == GL_TEXTURE_2D,
                            "DirectGLES depth CopyImageSubData only supports GL_TEXTURE_2D.");
            MOBILEGL_ASSERT(srcZ == 0 && dstZ == 0 && srcDepth == 1,
                            "DirectGLES depth CopyImageSubData only supports single-layer copies.");
            BlitDepthTexture2D(srcBackendTexture->GetBackendTextureId(), srcLevel, srcX, srcY, srcWidth, srcHeight,
                               dstBackendTexture->GetBackendTextureId(), dstLevel, dstX, dstY, srcWidth, srcHeight);
            return;
        }

        if (srcTexture->GetFormat() == TextureInternalFormat::R32F ||
            dstTexture->GetFormat() == TextureInternalFormat::R32F) {
            // The single glGetError below decides the fallback dispatch, and
            // ErrorLopper::Clear is compiled out at the default log level - drain
            // with the always-live helper so a stale flag cannot misroute a
            // succeeded native copy into the 2D-only fallback.
            ClearGLErrors();
            g_GLESFuncs.glCopyImageSubData(srcBackendTexture->GetBackendTextureId(), srcTarget, srcLevel, srcX, srcY, srcZ,
                                           dstBackendTexture->GetBackendTextureId(), dstTarget, dstLevel, dstX, dstY, dstZ,
                                           srcWidth, srcHeight, srcDepth);
            const GLenum copyImageError = g_GLESFuncs.glGetError();
            if (copyImageError == GL_NO_ERROR) {
                return;
            }
            MOBILEGL_ASSERT(IsColorOnlyFormat(srcTexture->GetFormat()) && IsColorOnlyFormat(dstTexture->GetFormat()),
                            "DirectGLES CopyImageSubData only supports color-only or depth-only copies.");
            MOBILEGL_ASSERT(srcTarget == GL_TEXTURE_2D && dstTarget == GL_TEXTURE_2D,
                            "DirectGLES color CopyImageSubData only supports GL_TEXTURE_2D.");
            MOBILEGL_ASSERT(srcZ == 0 && dstZ == 0 && srcDepth == 1,
                            "DirectGLES color CopyImageSubData only supports single-layer copies.");
            CopyR32FTexture2D(srcBackendTexture->GetBackendTextureId(), srcLevel, srcX, srcY, srcWidth, srcHeight,
                              dstBackendTexture->GetBackendTextureId(), dstTarget, dstLevel, dstX, dstY);
            return;
        }

        ClearGLErrors();
        g_GLESFuncs.glCopyImageSubData(srcBackendTexture->GetBackendTextureId(), srcTarget, srcLevel, srcX, srcY, srcZ,
                                       dstBackendTexture->GetBackendTextureId(), dstTarget, dstLevel, dstX, dstY, dstZ,
                                       srcWidth, srcHeight, srcDepth);
        AssertNoGLError("glCopyImageSubData");
    }

    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format) {
        (void)texture;
        (void)level;
        (void)layered;
        (void)layer;
        (void)access;
        (void)format;
        TextureImpl::SyncImageTextureBinding(unit);
    }

    void GetIntegeri_v(GLenum target, GLuint index, GLint* data) {
        if (!data) return;

        switch (target) {
        case GL_SHADER_STORAGE_BUFFER_BINDING: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            auto& obj = point.GetBoundObject();
            *data = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_SHADER_STORAGE_BUFFER_START: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            *data = static_cast<GLint>(point.GetRange().start);
            return;
        }
        case GL_SHADER_STORAGE_BUFFER_SIZE: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            auto& obj = point.GetBoundObject();
            if (!obj) {
                *data = 0;
                return;
            }
            const auto& range = point.GetRange();
            const auto start = std::min(range.start, obj->GetSize());
            const auto end = std::min(range.end, obj->GetSize());
            *data = static_cast<GLint>(end - start);
            return;
        }
        case GL_IMAGE_BINDING_NAME: {
            if (index >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
                *data = 0;
                return;
            }
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(index));
            *data = imageBinding.Texture ? static_cast<GLint>(imageBinding.Texture->GetExternalIndex()) : 0;
            return;
        }
        case GL_IMAGE_BINDING_LEVEL: {
            if (index >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
                *data = 0;
                return;
            }
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(index));
            *data = imageBinding.Level;
            return;
        }
        case GL_IMAGE_BINDING_LAYERED: {
            if (index >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
                *data = 0;
                return;
            }
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(index));
            *data = imageBinding.Layered;
            return;
        }
        case GL_IMAGE_BINDING_LAYER: {
            if (index >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
                *data = 0;
                return;
            }
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(index));
            *data = imageBinding.Layer;
            return;
        }
        case GL_IMAGE_BINDING_ACCESS: {
            if (index >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
                *data = 0;
                return;
            }
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(index));
            *data = static_cast<GLint>(imageBinding.Access);
            return;
        }
        case GL_IMAGE_BINDING_FORMAT: {
            if (index >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
                *data = 0;
                return;
            }
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(index));
            *data = static_cast<GLint>(imageBinding.Format);
            return;
        }
        default:
            if (g_GLESFuncs.glGetIntegeri_v) {
                g_GLESFuncs.glGetIntegeri_v(target, index, data);
            } else {
                *data = 0;
            }
            return;
        }
    }

    void GetInteger64i_v(GLenum target, GLuint index, GLint64* data) {
        if (!data) return;

        switch (target) {
        case GL_SHADER_STORAGE_BUFFER_START: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            *data = static_cast<GLint64>(point.GetRange().start);
            return;
        }
        case GL_SHADER_STORAGE_BUFFER_SIZE: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            auto& obj = point.GetBoundObject();
            if (!obj) {
                *data = 0;
                return;
            }
            const auto& range = point.GetRange();
            const auto start = std::min(range.start, obj->GetSize());
            const auto end = std::min(range.end, obj->GetSize());
            *data = static_cast<GLint64>(end - start);
            return;
        }
        default:
            if (g_GLESFuncs.glGetInteger64i_v) {
                g_GLESFuncs.glGetInteger64i_v(target, index, data);
            } else {
                *data = 0;
            }
            return;
        }
    }

    void GetProgramiv(GLuint program, GLenum pname, GLint* params) {
        if (!params) return;
        GLuint backendProgramId = GetBackendProgramId(program);
        if (!backendProgramId) {
            params[0] = 0;
            return;
        }
        g_GLESFuncs.glGetProgramiv(backendProgramId, pname, params);
    }

    void GetProgramInterfaceiv(GLuint program, GLenum programInterface, GLenum pname, GLint* params) {
        GLuint backendProgramId = GetBackendProgramId(program);
        if (!backendProgramId) return;
        g_GLESFuncs.glGetProgramInterfaceiv(backendProgramId, programInterface, pname, params);
    }

    GLuint GetProgramResourceIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        GLuint backendProgramId = GetBackendProgramId(program);
        if (!backendProgramId) return GL_INVALID_INDEX;
        return g_GLESFuncs.glGetProgramResourceIndex(backendProgramId, programInterface, name);
    }

    void GetProgramResourceName(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, GLsizei* length,
                                GLchar* name) {
        GLuint backendProgramId = GetBackendProgramId(program);
        if (!backendProgramId) return;
        g_GLESFuncs.glGetProgramResourceName(backendProgramId, programInterface, index, bufSize, length, name);
    }

    void GetProgramResourceiv(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount,
                              const GLenum* props, GLsizei bufSize, GLsizei* length, GLint* params) {
        GLuint backendProgramId = GetBackendProgramId(program);
        if (!backendProgramId) return;
        g_GLESFuncs.glGetProgramResourceiv(backendProgramId, programInterface, index, propCount, props, bufSize, length,
                                           params);
    }

    GLint GetProgramResourceLocation(GLuint program, GLenum programInterface, const GLchar* name) {
        GLuint backendProgramId = GetBackendProgramId(program);
        if (!backendProgramId) return -1;
        return g_GLESFuncs.glGetProgramResourceLocation(backendProgramId, programInterface, name);
    }

    GLint GetProgramResourceLocationIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        (void)program;
        (void)programInterface;
        (void)name;
        return -1;
    }

    void ShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding) {
        GLuint backendProgramId = GetBackendProgramId(program);
        if (!backendProgramId) return;
        g_GLESFuncs.glShaderStorageBlockBinding(backendProgramId, storageBlockIndex, storageBlockBinding);
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        g_GLESFuncs.glClearBufferfi(buffer, drawbuffer, depth, stencil);
    }

    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        g_GLESFuncs.glClearBufferfv(buffer, drawbuffer, value);
    }

    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState();

        // SyncCurrentFBO early-outs for the default framebuffer, so without this
        // bind a user-FBO -> default-FBO switch would leave the clear landing on
        // the stale driver DRAW binding (the fi/fv/uiv siblings all bind too).
        BindCurrentFBO(FramebufferTarget::Draw);

        g_GLESFuncs.glClearBufferiv(buffer, drawbuffer, value);
    }

    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        g_GLESFuncs.glClearBufferuiv(buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, const GLfloat* value) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        TextureImpl::SyncNeccessaryTextures();
        RenderStateImpl::SyncRenderState();

        SyncAndBindFramebufferObject(framebuffer, FramebufferTarget::Draw, true);
        g_GLESFuncs.glClearBufferfv(buffer, drawbuffer, value);
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        ForceBindCurrentFBO(FramebufferTarget::Draw);
    }

    void ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        TextureImpl::SyncNeccessaryTextures();
        RenderStateImpl::SyncRenderState();

        SyncAndBindFramebufferObject(framebuffer, FramebufferTarget::Draw, true);
        g_GLESFuncs.glClearBufferfi(buffer, drawbuffer, depth, stencil);
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        ForceBindCurrentFBO(FramebufferTarget::Draw);
    }

    static SizeT AlignPixelRow(SizeT rowBytes, Int alignment) {
        const SizeT resolvedAlignment = static_cast<SizeT>(std::max(alignment, 1));
        return (rowBytes + resolvedAlignment - 1) & ~(resolvedAlignment - 1);
    }

    static Bool ReadPixelsDepthFloatViaUnsignedInt(GLint x, GLint y, GLsizei width, GLsizei height, void* pixels) {
        if (width <= 0 || height <= 0) {
            return true;
        }

        Vector<Uint32> raw(static_cast<SizeT>(width) * static_cast<SizeT>(height));
        GLenum readError = GL_NO_ERROR;
        {
            ScopedPixelPackBuffer packBuffer(0);
            ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});
            // Drain first: a stale flag some earlier best-effort call left queued
            // must not be misattributed to this read (it would silently drop the
            // whole readback in production builds where ErrorLopper is compiled out).
            ClearGLErrors();
            g_GLESFuncs.glReadPixels(x, y, width, height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, raw.data());
            readError = g_GLESFuncs.glGetError();
        }
        if (readError != GL_NO_ERROR) {
            MGLOG_E("ReadPixels: depth GL_FLOAT fallback read failed: %s",
                    MG_Util::ConvertGLEnumToString(readError).c_str());
            return true;
        }

        const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
        const SizeT rowPixels = static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : width);
        const SizeT dstPixelBytes = sizeof(Float);
        const SizeT dstRowStride = AlignPixelRow(rowPixels * dstPixelBytes, packParams.Alignment);
        const SizeT dstOffset = static_cast<SizeT>(std::max(packParams.SkipRows, 0)) * dstRowStride +
                                static_cast<SizeT>(std::max(packParams.SkipPixels, 0)) * dstPixelBytes;
        const SizeT packedSize = dstOffset + static_cast<SizeT>(height - 1) * dstRowStride +
                                 static_cast<SizeT>(width) * dstPixelBytes;
        // Only actual pixel rows are written so PACK skip/row-length gap regions stay untouched.
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        const SizeT pboOffset = reinterpret_cast<SizeT>(pixels);
        if (pixelPackBufferObject && pboOffset + packedSize > pixelPackBufferObject->GetSize()) {
            MGLOG_E("ReadPixels: depth GL_FLOAT fallback PBO is too small");
            return true;
        }
        Vector<Float> rowBuf(static_cast<SizeT>(width));
        for (GLsizei row = 0; row < height; ++row) {
            const Uint32* srcRow = raw.data() + static_cast<SizeT>(row) * static_cast<SizeT>(width);
            for (GLsizei col = 0; col < width; ++col) {
                // TODO: preserve native depth precision when GLES exposes float depth readback directly.
                rowBuf[col] = static_cast<Float>(static_cast<Double>(srcRow[col]) / 4294967295.0);
            }
            const SizeT rowOffset = dstOffset + static_cast<SizeT>(row) * dstRowStride;
            if (pixelPackBufferObject) {
                pixelPackBufferObject->WritebackFromBackend(
                    {rowBuf.data(), static_cast<SizeT>(width) * sizeof(Float)}, pboOffset + rowOffset);
            } else if (pixels != nullptr) {
                Memcpy(static_cast<Uint8*>(pixels) + rowOffset, rowBuf.data(),
                       static_cast<SizeT>(width) * sizeof(Float));
            }
        }
        return true;
    }

    static Bool ReadPixelsStencilUintViaUnsignedByte(GLint x, GLint y, GLsizei width, GLsizei height, void* pixels) {
        if (width <= 0 || height <= 0) {
            return true;
        }

        Vector<Uint8> raw(static_cast<SizeT>(width) * static_cast<SizeT>(height));
        GLenum readError = GL_NO_ERROR;
        {
            ScopedPixelPackBuffer packBuffer(0);
            ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});
            // Drain first: see ReadPixelsDepthFloatViaUnsignedInt.
            ClearGLErrors();
            g_GLESFuncs.glReadPixels(x, y, width, height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, raw.data());
            readError = g_GLESFuncs.glGetError();
        }
        if (readError != GL_NO_ERROR) {
            MGLOG_E("ReadPixels: stencil GL_UNSIGNED_INT fallback read failed: %s",
                    MG_Util::ConvertGLEnumToString(readError).c_str());
            return true;
        }

        const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
        const SizeT rowPixels = static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : width);
        const SizeT dstPixelBytes = sizeof(Uint32);
        const SizeT dstRowStride = AlignPixelRow(rowPixels * dstPixelBytes, packParams.Alignment);
        const SizeT dstOffset = static_cast<SizeT>(std::max(packParams.SkipRows, 0)) * dstRowStride +
                                static_cast<SizeT>(std::max(packParams.SkipPixels, 0)) * dstPixelBytes;
        const SizeT packedSize = dstOffset + static_cast<SizeT>(height - 1) * dstRowStride +
                                 static_cast<SizeT>(width) * dstPixelBytes;
        // Only actual pixel rows are written so PACK skip/row-length gap regions stay untouched.
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        const SizeT pboOffset = reinterpret_cast<SizeT>(pixels);
        if (pixelPackBufferObject && pboOffset + packedSize > pixelPackBufferObject->GetSize()) {
            MGLOG_E("ReadPixels: stencil GL_UNSIGNED_INT fallback PBO is too small");
            return true;
        }
        Vector<Uint32> rowBuf(static_cast<SizeT>(width));
        for (GLsizei row = 0; row < height; ++row) {
            const Uint8* srcRow = raw.data() + static_cast<SizeT>(row) * static_cast<SizeT>(width);
            for (GLsizei col = 0; col < width; ++col) {
                // TODO: switch to native uint stencil readback if the GLES backend exposes it.
                rowBuf[col] = srcRow[col];
            }
            const SizeT rowOffset = dstOffset + static_cast<SizeT>(row) * dstRowStride;
            if (pixelPackBufferObject) {
                pixelPackBufferObject->WritebackFromBackend(
                    {rowBuf.data(), static_cast<SizeT>(width) * sizeof(Uint32)}, pboOffset + rowOffset);
            } else if (pixels != nullptr) {
                Memcpy(static_cast<Uint8*>(pixels) + rowOffset, rowBuf.data(),
                       static_cast<SizeT>(width) * sizeof(Uint32));
            }
        }
        return true;
    }

    // ---- Client-format readback conversion ---------------------------------------------------------------------
    // ES 3.x glReadPixels only guarantees GL_RGBA/GL_UNSIGNED_BYTE, GL_RGBA_INTEGER/GL_(UNSIGNED_)INT,
    // GL_RGBA/GL_FLOAT for float buffers plus one implementation-defined pair, while desktop GL clients read
    // back narrower layouts (RED, RG, RGB, BGR, byte-order packed types, ...). For those we read a guaranteed
    // wide RGBA format into scratch memory and repack into the caller's (format, type) layout on the CPU,
    // honoring the client-side PACK pixel-store parameters. The pure repacking helpers live in
    // ReadbackImpl (Utils.cpp) so unit tests can assert the exact packed words.

    using ReadbackImpl::GetReadbackChannelMapping;
    using ReadbackImpl::GetReadbackComponentSize;
    using ReadbackImpl::GetReadbackDstPixelSize;
    using ReadbackImpl::ReadbackChannelMapping;

    static Bool CanDecodeWideSourceType(GLenum type) {
        switch (type) {
        case GL_UNSIGNED_BYTE:
        case GL_BYTE:
        case GL_UNSIGNED_SHORT:
        case GL_SHORT:
        case GL_HALF_FLOAT:
        case GL_FLOAT:
            return true;
        default:
            return false;
        }
    }

    // Component-array read formats usable as (possibly narrow) wide-read sources.
    static Int GetWideReadChannelCount(GLenum format) {
        switch (format) {
        case GL_RED:
        case GL_RED_INTEGER:
            return 1;
        case GL_RG:
        case GL_RG_INTEGER:
            return 2;
        case GL_RGB:
        case GL_RGB_INTEGER:
            return 3;
        case GL_RGBA:
        case GL_RGBA_INTEGER:
            return 4;
        default:
            return 0;
        }
    }

    static Bool IsIntegerReadFormat(GLint format) {
        return format == GL_RED_INTEGER || format == GL_RG_INTEGER || format == GL_RGB_INTEGER ||
               format == GL_RGBA_INTEGER;
    }

    // Expands a tightly-packed narrow read (1-3 channels per texel) into the 4-channel wide RGBA
    // layout ConvertWideReadbackRow expects. Missing G/B read zero; missing A reads one, encoded in
    // the source component type.
    static void ExpandNarrowWideRead(Vector<Uint8>& data, SizeT pixelCount, Int srcChannels, GLenum componentType) {
        const SizeT componentSize = GetReadbackComponentSize(componentType);
        if (componentSize == 0 || srcChannels <= 0 || srcChannels >= 4) {
            return;
        }
        Uint8 zeroBits[4] = {0, 0, 0, 0};
        Uint8 oneBits[4] = {0, 0, 0, 0};
        switch (componentType) {
        case GL_UNSIGNED_BYTE:
            oneBits[0] = 0xFF;
            break;
        case GL_BYTE:
            oneBits[0] = 0x7F;
            break;
        case GL_UNSIGNED_SHORT: {
            const Uint16 one = 0xFFFF;
            Memcpy(oneBits, &one, sizeof(one));
            break;
        }
        case GL_SHORT: {
            const Int16 one = 0x7FFF;
            Memcpy(oneBits, &one, sizeof(one));
            break;
        }
        case GL_HALF_FLOAT: {
            const Uint16 one = 0x3C00;
            Memcpy(oneBits, &one, sizeof(one));
            break;
        }
        case GL_FLOAT: {
            const Float one = 1.0f;
            Memcpy(oneBits, &one, sizeof(one));
            break;
        }
        case GL_UNSIGNED_INT:
        case GL_INT: {
            const Uint32 one = 1;
            Memcpy(oneBits, &one, sizeof(one));
            break;
        }
        default:
            break;
        }

        Vector<Uint8> expanded(pixelCount * 4 * componentSize);
        for (SizeT i = 0; i < pixelCount; ++i) {
            const Uint8* src = data.data() + i * static_cast<SizeT>(srcChannels) * componentSize;
            Uint8* dst = expanded.data() + i * 4 * componentSize;
            for (Int ch = 0; ch < 4; ++ch) {
                if (ch < srcChannels) {
                    Memcpy(dst + static_cast<SizeT>(ch) * componentSize, src + static_cast<SizeT>(ch) * componentSize,
                           componentSize);
                } else {
                    Memcpy(dst + static_cast<SizeT>(ch) * componentSize, ch == 3 ? oneBits : zeroBits, componentSize);
                }
            }
        }
        data = std::move(expanded);
    }

    static void DrainESErrors() {
        for (Int i = 0; i < 32 && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {
        }
    }

    static GLenum QueryReadAttachmentComponentType() {
        GLint framebufferId = 0;
        g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebufferId);
        if (framebufferId == 0) {
            return GL_UNSIGNED_NORMALIZED; // default framebuffers are normalized fixed-point
        }
        GLint readBuffer = GL_COLOR_ATTACHMENT0;
        g_GLESFuncs.glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        if (readBuffer < GL_COLOR_ATTACHMENT0 || readBuffer > GL_COLOR_ATTACHMENT31) {
            readBuffer = GL_COLOR_ATTACHMENT0;
        }
        GLint componentType = 0;
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, static_cast<GLenum>(readBuffer),
                                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
        DrainESErrors();
        return componentType != 0 ? static_cast<GLenum>(componentType) : GL_UNSIGNED_NORMALIZED;
    }


    // Reads the current READ framebuffer as wide RGBA(_INTEGER) and repacks the pixels into the client's
    // (format, type) layout. Returns false when the combination is not convertible (the caller keeps its
    // "not implemented" skip); returns true when the request was handled, even if it degraded to a logged no-op.
    static Bool ReadPixelsViaFormatConversion(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                              GLenum type, void* pixels, Bool honorPackImageParams = false) {
        ReadbackChannelMapping mapping{};
        if (!GetReadbackChannelMapping(format, mapping)) {
            return false;
        }
        // Covers unknown types, packed field-count/format mismatches and float types on integer formats.
        const SizeT dstPixelBytes = GetReadbackDstPixelSize(mapping, type);
        if (dstPixelBytes == 0) {
            return false;
        }

        if (width <= 0 || height <= 0) {
            return true;
        }
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        if (!pixelPackBufferObject && pixels == nullptr) {
            return true;
        }

        const GLenum attachmentComponentType = QueryReadAttachmentComponentType();
        const Bool integerAttachment =
            attachmentComponentType == GL_INT || attachmentComponentType == GL_UNSIGNED_INT;
        if (mapping.isInteger != integerAttachment) {
            MGLOG_E("Readback conversion: integer-ness of format %s does not match the read buffer, skipping",
                    MG_Util::ConvertGLEnumToString(format).c_str());
            return true;
        }

        // Prefer the implementation-defined pair (full precision on e.g. norm16 buffers, and possibly
        // a narrow format like GL_RED/GL_UNSIGNED_SHORT), then the spec/extension-guaranteed pair for
        // the attachment class. Narrow reads are expanded to RGBA on the CPU afterwards.
        GLint implFormat = 0;
        GLint implType = 0;
        g_GLESFuncs.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &implFormat);
        g_GLESFuncs.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &implType);

        struct WideReadCandidate {
            GLenum format;
            GLenum type;
        };
        WideReadCandidate candidates[4];
        Int candidateCount = 0;
        if (mapping.isInteger) {
            if (GetWideReadChannelCount(static_cast<GLenum>(implFormat)) > 0 && IsIntegerReadFormat(implFormat) &&
                (implType == GL_INT || implType == GL_UNSIGNED_INT)) {
                candidates[candidateCount++] = {static_cast<GLenum>(implFormat), static_cast<GLenum>(implType)};
            }
            candidates[candidateCount++] = {
                GL_RGBA_INTEGER,
                attachmentComponentType == GL_INT ? static_cast<GLenum>(GL_INT) : static_cast<GLenum>(GL_UNSIGNED_INT)};
        } else {
            if (GetWideReadChannelCount(static_cast<GLenum>(implFormat)) > 0 && !IsIntegerReadFormat(implFormat) &&
                (CanDecodeWideSourceType(static_cast<GLenum>(implType)) ||
                 (implFormat == GL_RGBA && implType == GL_UNSIGNED_INT_2_10_10_10_REV))) {
                candidates[candidateCount++] = {static_cast<GLenum>(implFormat), static_cast<GLenum>(implType)};
            }
            if (attachmentComponentType == GL_FLOAT) {
                candidates[candidateCount++] = {GL_RGBA, GL_FLOAT};
            }
            if (attachmentComponentType == GL_SIGNED_NORMALIZED) {
                // EXT_render_snorm attachments read back as RGBA/BYTE (8-bit) or RGBA/SHORT (16-bit).
                candidates[candidateCount++] = {GL_RGBA, GL_SHORT};
                candidates[candidateCount++] = {GL_RGBA, GL_BYTE};
            } else {
                candidates[candidateCount++] = {GL_RGBA, GL_UNSIGNED_BYTE};
            }
        }

        ScopedPixelPackBuffer packBuffer(0);
        ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});

        Vector<Uint8> wide;
        GLenum wideType = GL_NONE;
        GLenum readFormat = GL_NONE;
        Int readChannels = 0;
        DrainESErrors();
        for (Int i = 0; i < candidateCount; ++i) {
            const WideReadCandidate candidate = candidates[i];
            Bool alreadyTried = false;
            for (Int j = 0; j < i; ++j) {
                alreadyTried =
                    alreadyTried || (candidates[j].format == candidate.format && candidates[j].type == candidate.type);
            }
            if (alreadyTried) {
                continue;
            }
            const Int channels = GetWideReadChannelCount(candidate.format);
            const SizeT candidateComponentSize = GetReadbackComponentSize(candidate.type);
            wide.resize(static_cast<SizeT>(width) * static_cast<SizeT>(height) *
                        static_cast<SizeT>(channels) * candidateComponentSize);
            g_GLESFuncs.glReadPixels(x, y, width, height, candidate.format, candidate.type, wide.data());
            if (g_GLESFuncs.glGetError() == GL_NO_ERROR) {
                wideType = candidate.type;
                readFormat = candidate.format;
                readChannels = channels;
                break;
            }
        }
        if (wideType == GL_NONE) {
            MGLOG_E("Readback conversion: ES accepted no wide read type for format %s type %s, skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return true;
        }

        if (wideType == GL_UNSIGNED_INT_2_10_10_10_REV) {
            // Unpack the packed words into a float wide buffer (full 10-bit precision on e.g.
            // GL_RGB10_A2 attachments, whose implementation read pair is RGBA/2_10_10_10_REV).
            const SizeT pixelCount = static_cast<SizeT>(width) * static_cast<SizeT>(height);
            Vector<Uint8> floatWide(pixelCount * 4 * sizeof(Float));
            auto* dst = reinterpret_cast<Float*>(floatWide.data());
            for (SizeT i = 0; i < pixelCount; ++i) {
                Uint32 word;
                Memcpy(&word, wide.data() + i * 4, sizeof(word));
                dst[i * 4 + 0] = static_cast<Float>(word & 0x3FFu) / 1023.0f;
                dst[i * 4 + 1] = static_cast<Float>((word >> 10) & 0x3FFu) / 1023.0f;
                dst[i * 4 + 2] = static_cast<Float>((word >> 20) & 0x3FFu) / 1023.0f;
                dst[i * 4 + 3] = static_cast<Float>((word >> 30) & 0x3u) / 3.0f;
            }
            wide = std::move(floatWide);
            wideType = GL_FLOAT;
            readChannels = 4;
        }
        if (readChannels < 4) {
            ExpandNarrowWideRead(wide, static_cast<SizeT>(width) * static_cast<SizeT>(height), readChannels, wideType);
        }

        if (!ReadbackImpl::StoreWideRowsToClient(wide.data(), wideType, width, height, /*sliceCount=*/1, mapping, type, pixels,
                                   honorPackImageParams)) {
            return false;
        }

        MGLOG_D("Readback conversion: converted %s/%s from wide %s/%s", MG_Util::ConvertGLEnumToString(format).c_str(),
                MG_Util::ConvertGLEnumToString(type).c_str(), MG_Util::ConvertGLEnumToString(readFormat).c_str(),
                MG_Util::ConvertGLEnumToString(wideType).c_str());
        return true;
    }

    // GetTexImage fallback for internal formats the ES driver cannot attach to a framebuffer
    // (SNORM, RGB16, RGB9_E5, ...): decodes the canonical CPU shadow-mip storage into wide RGBA
    // rows and repacks them into the client layout. Only valid while the shadow copy is
    // authoritative, which holds for non-renderable formats (they can never be GPU-written).
    static Bool GetTexImageViaShadowConversion(MG_State::GLState::TextureObjectMipmap* textureMipmapObject,
                                               TextureUploadTarget uploadTarget, GLint level, GLsizei width,
                                               GLsizei sliceHeight, GLsizei sliceCount, GLenum format, GLenum type,
                                               void* pixels, Bool applyPackImageParams) {
        ReadbackChannelMapping mapping{};
        if (!GetReadbackChannelMapping(format, mapping)) {
            return false;
        }
        if (GetReadbackDstPixelSize(mapping, type) == 0) {
            return false;
        }
        if (width <= 0 || sliceHeight <= 0 || sliceCount <= 0) {
            return true;
        }
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        if (!pixelPackBufferObject && pixels == nullptr) {
            return true;
        }

        const void* shadow = textureMipmapObject->MapMipmapData(uploadTarget, level);
        if (!shadow) {
            return false;
        }

        Vector<Uint8> wide;
        Bool isInteger = false;
        Bool isSigned = false;
        if (!MG_Util::PixelStoreProcessor::DecodeShadowDataToWideRGBA(
                textureMipmapObject->GetFormat(), shadow,
                static_cast<SizeT>(width) * static_cast<SizeT>(sliceHeight) * static_cast<SizeT>(sliceCount),
                wide, isInteger, isSigned)) {
            return false;
        }
        if (mapping.isInteger != isInteger) {
            // Spec-invalid combinations are rejected with GL errors at the state layer already.
            return false;
        }
        const GLenum wideType = isInteger ? (isSigned ? GL_INT : GL_UNSIGNED_INT) : GL_FLOAT;
        if (!ReadbackImpl::StoreWideRowsToClient(wide.data(), wideType, width, sliceHeight, sliceCount, mapping, type, pixels,
                                   applyPackImageParams)) {
            return false;
        }
        MGLOG_D("GetTexImage: converted %s/%s from the CPU shadow copy",
                MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
        return true;
    }

    static Bool IsLegacyNativeReadPixelsFormat(GLenum format) {
        return format == GL_RGBA || format == GL_RGBA_INTEGER || format == GL_RED || format == GL_RED_INTEGER ||
               format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL;
    }

    static Bool IsLegacyNativeReadPixelsType(GLenum type) {
        // GL_UNSIGNED_INT_24_8 / GL_FLOAT_32_UNSIGNED_INT_24_8_REV are only ever valid
        // paired with GL_DEPTH_STENCIL (packed_depth_stencil.verify_read_pixels); the real
        // driver already implements this readback natively.
        return type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_2_10_10_10_REV ||
               type == GL_INT || type == GL_FLOAT || type == GL_UNSIGNED_INT_24_8 ||
               type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
    }

    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        MGLOG_D("ReadPixels: x=%d y=%d w=%d h=%d format=%s type=%s pixels=%p", x, y, width, height,
                MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str(), pixels);

        // Combinations the ES driver has always handled directly keep the native path; other color layouts go
        // through the wide-format conversion path. Anything still uncovered degrades to a logged no-op instead
        // of killing the process; spec-invalid combinations are already rejected with GL errors at the state layer.
        const Bool useNativeReadback = IsLegacyNativeReadPixelsFormat(format) && IsLegacyNativeReadPixelsType(type);
        ReadbackChannelMapping conversionMapping{};
        const Bool convertible = GetReadbackChannelMapping(format, conversionMapping) &&
                                 GetReadbackDstPixelSize(conversionMapping, type) != 0;
        if (!useNativeReadback && !convertible) {
            MGLOG_E("ReadPixels: format %s with type %s is not implemented yet, skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return;
        }

        MGLOG_D("ReadPixels: SyncNeccessaryTextures()");
        TextureImpl::SyncNeccessaryTextures();

        MGLOG_D("ReadPixels: SyncCurrentFBO()");
        FramebufferImpl::SyncCurrentFBO();

        MGLOG_D("ReadPixels: BindCurrentFBO(Read)");
        BindCurrentFBO(FramebufferTarget::Read);

        MGLOG_D("ReadPixels: Applying the PACK pixel-store scope");
        ScopedPackState packParamsScope(PackStateFromContext());

        GLenum fbStatus = g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        MGLOG_D("ReadPixels: GL_READ_FRAMEBUFFER status = %s", MG_Util::ConvertGLEnumToString(fbStatus).c_str());

        if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
            MGLOG_E("ReadPixels: bound READ FBO is not complete");
            return;
        }
        // ES only guarantees GL_RGBA/GL_UNSIGNED_BYTE and GL_RGBA_INTEGER/GL_(UNSIGNED_)INT for the
        // matching attachment class; every other convertible color layout (including GL_RGBA/GL_FLOAT
        // and legacy GL_RED reads) goes through the wide-format conversion, which picks a wide type
        // the driver accepts for the current attachment. GL_PACK_SWAP_BYTES has no ES equivalent, so
        // it always takes the conversion path (which swaps on the CPU).
        const Bool packSwapBytes = MG_State::pGLContext->GetPixelStoreParameters(false).SwapBytes;
        const Bool nativeFastPair = !packSwapBytes &&
                                    ((format == GL_RGBA && type == GL_UNSIGNED_BYTE) ||
                                     (format == GL_RGBA_INTEGER && (type == GL_UNSIGNED_INT || type == GL_INT)));
        if (convertible && !nativeFastPair) {
            if (ReadPixelsViaFormatConversion(x, y, width, height, format, type, pixels)) {
                MGLOG_D("ReadPixels: finished via client-format conversion");
                return;
            }
            MGLOG_E("ReadPixels: format %s with type %s is not implemented yet, skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return;
        }
        if (format == GL_DEPTH_COMPONENT && type == GL_FLOAT &&
            ReadPixelsDepthFloatViaUnsignedInt(x, y, width, height, pixels)) {
            MGLOG_D("ReadPixels: finished via depth GL_FLOAT fallback");
            return;
        }
        if (format == GL_STENCIL_INDEX && type == GL_UNSIGNED_INT &&
            ReadPixelsStencilUintViaUnsignedByte(x, y, width, height, pixels)) {
            MGLOG_D("ReadPixels: finished via stencil GL_UNSIGNED_INT fallback");
            return;
        }

        // Handle PBO. The pack binding is scoped: it returns to the resting 0 state
        // on every exit path, so a later readback can never land in a stale PBO
        // (the driver-level binding used to stay on the user PBO after this call,
        // capturing subsequent client-memory readbacks into it).
        auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        Bool usePBO = false;
        GLuint packBufferId = 0;
        if (pixelPackBufferObject) {
            auto* backendResource = BufferImpl::EnsureBufferResource(pixelPackBufferObject);
            MGLOG_D("ReadPixels: Using PBO %u", pixelPackBufferObject->GetExternalIndex());
            if (!backendResource || backendResource->id == 0) {
                MGLOG_E("ReadPixels: No backend buffer found for PBO %u.",
                        pixelPackBufferObject ? pixelPackBufferObject->GetExternalIndex() : 0);
                return;
            }
            usePBO = true;
            packBufferId = backendResource->id;
        } else {
            MGLOG_D("ReadPixels: Not using PBO");
        }
        ScopedPixelPackBuffer packBufferBinding(packBufferId);

        MGLOG_D("ReadPixels: glReadPixels()");
        DrainESErrors();
        g_GLESFuncs.glReadPixels(x, y, width, height, format, type, pixels);
        const GLenum nativeReadError = g_GLESFuncs.glGetError();
        if (nativeReadError != GL_NO_ERROR) {
            // ES drivers only guarantee GL_RGBA/GL_UNSIGNED_BYTE, GL_RGBA_INTEGER/(U)INT, float RGBA and one
            // implementation-defined pair; legacy combos like GL_RED/GL_UNSIGNED_INT are rejected by e.g.
            // Adreno with a GL error and an untouched destination (GL CTS packed_pixels r8_format_red). The
            // failed read wrote nothing (client memory and PBO alike), so re-service the request through the
            // wide-format conversion path before any PBO writeback can capture stale contents. The conversion
            // helper saves/restores the ES pixel-pack binding and handles the state-layer PBO itself.
            DrainESErrors();
            MGLOG_D("ReadPixels: native read of %s/%s failed (%s), retrying via client-format conversion",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str(),
                    MG_Util::ConvertGLEnumToString(nativeReadError).c_str());
            if (ReadPixelsViaFormatConversion(x, y, width, height, format, type, pixels)) {
                MGLOG_D("ReadPixels: finished via client-format conversion after native failure");
                return;
            }
            MGLOG_E("ReadPixels: native read of %s/%s failed (%s) and no conversion path covers it, "
                    "skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str(),
                    MG_Util::ConvertGLEnumToString(nativeReadError).c_str());
            return;
        }
        if (usePBO) {
            // pull back to client memory if PBO is used
            MGLOG_D("ReadPixels: PBO used, mapping buffer to client memory");
            GLvoid* pboMappedPtr = g_GLESFuncs.glMapBufferRange(
                GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)pixelPackBufferObject->GetSize(), GL_MAP_READ_BIT);
            if (pboMappedPtr) {
                MGLOG_D("ReadPixels: Copying data from PBO to client memory");
                SizeT size = pixelPackBufferObject->GetSize();
                pixelPackBufferObject->WritebackFromBackend({pboMappedPtr, size}, 0);
                MGLOG_D("ReadPixels: Unmapping PBO");
                g_GLESFuncs.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            } else {
                MGLOG_E("ReadPixels: glMapBufferRange returned nullptr");
            }
        }
        MGLOG_D("ReadPixels: finished");
    }

    // Combinations the ES driver has always handled directly for GetTexImage; everything else that maps
    // to a color channel layout is repacked via ReadPixelsViaFormatConversion.
    static Bool IsNativeGetTexImagePair(GLenum format, GLenum type) {
        if (format == GL_RGBA) {
            return type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_2_10_10_10_REV ||
                   type == GL_INT || type == GL_FLOAT || type == GL_HALF_FLOAT ||
                   type == GL_UNSIGNED_INT_8_8_8_8_REV;
        }
        if (format == GL_RGBA_INTEGER) {
            return type == GL_INT || type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_2_10_10_10_REV;
        }
        if (format == GL_DEPTH_STENCIL) {
            return type == GL_UNSIGNED_INT_24_8 || type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        }
        return false;
    }

    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels) {
        DebugImpl::ErrorLopper errorLopper;
        MGLOG_D("GetTexImage: target=%s level=%d format=%s type=%s pixels=%p",
                MG_Util::ConvertGLEnumToString(target).c_str(), level, MG_Util::ConvertGLEnumToString(format).c_str(),
                MG_Util::ConvertGLEnumToString(type).c_str(), pixels);

        // Unimplemented readback formats degrade to a logged no-op instead of killing the process;
        // spec-invalid combinations are already rejected with GL errors at the state layer.
        const Bool useNativeReadback = IsNativeGetTexImagePair(format, type);
        ReadbackChannelMapping conversionMapping{};
        const Bool convertible = GetReadbackChannelMapping(format, conversionMapping) &&
                                 GetReadbackDstPixelSize(conversionMapping, type) != 0;
        if (!useNativeReadback && !convertible) {
            MGLOG_E("GetTexImage: format %s with type %s is not implemented yet, skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return;
        }

        GLenum esFormat = format, esType = type;
        // On little-endian hosts UNSIGNED_INT_8_8_8_8_REV has the same memory layout as UNSIGNED_BYTE.
        if (esType == GL_UNSIGNED_INT_8_8_8_8_REV) esType = GL_UNSIGNED_BYTE;

        MGLOG_D("GetTexImage: SyncNeccessaryTextures()");
        TextureImpl::SyncNeccessaryTextures();

        MGLOG_D("GetTexImage: SyncCurrentFBO()");
        FramebufferImpl::SyncCurrentFBO();

        auto activeTextureUnit = MG_State::pGLContext->GetActiveTextureUnit();
        MGLOG_D("GetTexImage: active texture unit = %u", activeTextureUnit);

        const auto& textureObject = MG_State::pGLContext->GetTextureUnitObject(activeTextureUnit)
                                        .GetBindingSlot(MG_Util::ConvertGLEnumToTextureTarget(target))
                                        .GetBoundObject();

        MGLOG_D("GetTexImage: bound texture object = %p (name=%u)", textureObject.get(),
                textureObject ? textureObject->GetExternalIndex() : 0);

        const auto& backendTextureIt = TextureImpl::g_backendTextureObjects.find(textureObject.get());

        if (backendTextureIt == TextureImpl::g_backendTextureObjects.end()) {
            MGLOG_E("GetTexImage: No backend texture found for texture %u.",
                    textureObject ? textureObject->GetExternalIndex() : 0);
            return;
        }

        GLuint backendTexId = backendTextureIt->second->GetBackendTextureId();
        MGLOG_D("GetTexImage: backend texture id = %u", backendTexId);

        // Force pending rendering to complete before reading the texture back through the temp READ FBO.
        // Tile-based GPUs (Mali) do not guarantee that a render into this texture through its own FBO has
        // been resolved to memory when it is subsequently sampled through a *different* (temp) FBO: the
        // cross-FBO glReadPixels below races the deferred tile resolve and returns the pre-render (clear)
        // contents, so distinct render targets read back byte-identical (e.g. KHR-GLxx.glsl_noperspective
        // fails on Mali-G715, all four programs reading as the clear colour). glGetTexImage is already a
        // CPU/GPU sync point, so the extra drain is negligible; Adreno resolves eagerly and is unaffected.
        g_GLESFuncs.glFinish();

        MGLOG_D("GetTexImage: Binding temporary FBO");
        TempFBOBinder tempFBOBinder(true);
        auto& tempFB = tempFBOBinder.Framebuffer();

        MGLOG_D("GetTexImage: attaching level %d to the scratch FBO", level);
        const GLenum backendAttachTarget = TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(
            MG_Util::ConvertGLEnumToTextureUploadTarget(target));
        // GL_DEPTH_STENCIL can't be attached as a color attachment (glCheckFramebufferStatus
        // would report it incomplete); it has its own combined depth+stencil attachment point.
        // glReadBuffer only selects among color attachments, so it does not apply here.
        if (format == GL_DEPTH_STENCIL) {
            ScratchFBOImpl::EnsureDepthAttachment2D(
                tempFB, GL_READ_FRAMEBUFFER, backendTexId,
                backendAttachTarget == GL_UNKNOWN_MGL ? target : backendAttachTarget, level, /*withStencil=*/true);
        } else if (backendAttachTarget == GL_TEXTURE_3D || backendAttachTarget == GL_TEXTURE_2D_ARRAY) {
            // ES cannot attach 3D/array textures through glFramebufferTexture2D; read layer 0. Reads
            // of deeper slices are served from the CPU shadow instead (see the shadow-first branch).
            ScratchFBOImpl::EnsureColorAttachmentLayer(tempFB, GL_READ_FRAMEBUFFER, backendTexId, level, 0);
        } else {
            ScratchFBOImpl::EnsureColorAttachment2D(
                tempFB, GL_READ_FRAMEBUFFER, backendTexId,
                backendAttachTarget == GL_UNKNOWN_MGL ? target : backendAttachTarget, level);
        }
        if (format != GL_DEPTH_STENCIL) {
            MGLOG_D("GetTexImage: glReadBuffer(GL_COLOR_ATTACHMENT0)");
            ScratchFBOImpl::EnsureReadBuffer(tempFB, GL_COLOR_ATTACHMENT0);
        }

        GLenum fbStatus = g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        MGLOG_D("GetTexImage: GL_READ_FRAMEBUFFER status = %s", MG_Util::ConvertGLEnumToString(fbStatus).c_str());

        // Non-renderable internal formats (SNORM, RGB16, RGB9_E5, ...) leave the temp FBO incomplete;
        // those readbacks are served from the CPU shadow copy below instead of bailing out.
        const Bool tempFBOComplete = fbStatus == GL_FRAMEBUFFER_COMPLETE;

        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        MGLOG_D("GetTexImage: Applying the PACK pixel-store scope");
        ScopedPackState packParamsScope(PackStateFromContext());

        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        const auto& storageType = textureObject->GetStorageType();
        MGLOG_D("GetTexImage: texture storage type = %d", (int)storageType);

        if (storageType == TextureStorageType::Buffer) {
            MGLOG_E("GetTexImage: Texture storage type Buffer is not supported.");
            return;
        }

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        auto& levelRange = textureMipmapObject->GetLevelRange();
        MGLOG_D("GetTexImage: mipmap level range = [%d, %d]", levelRange.x(), levelRange.y());

        // levelRange.y() is GL_TEXTURE_MAX_LEVEL, an inclusive level index — a single-level
        // texture has range [0, 0] and level 0 must be readable.
        if (static_cast<Uint>(level) < levelRange.x() || static_cast<Uint>(level) > levelRange.y()) {
            MGLOG_E("GetTexImage: Requested level %d is out of range (base level %u, max level %u), skipping readback",
                    level, levelRange.x(), levelRange.y());
            return;
        }

        auto size = textureMipmapObject->GetMipmapTexelSize(MG_Util::ConvertGLEnumToTextureUploadTarget(target), level);

        MGLOG_D("GetTexImage: mip level %d size = %dx%d", level, size.x(), size.y());

        // Prefer the client-format conversion for every convertible combination: the "native" ES pairs
        // are only guaranteed for matching attachment classes (e.g. GL_RGBA/GL_UNSIGNED_INT is invalid
        // for normalized attachments), while the conversion path reads a wide format that is always
        // accepted and repacks on the CPU.
        if (convertible) {
            // GL_PACK_IMAGE_HEIGHT/GL_PACK_SKIP_IMAGES only apply to 3D/array image
            // readbacks (cube-map arrays address as arrays); 2D targets must ignore
            // them (GL 3.3 section 6.1.4).
            const Bool applyPackImageParams = backendAttachTarget == GL_TEXTURE_3D ||
                                              backendAttachTarget == GL_TEXTURE_2D_ARRAY ||
                                              backendAttachTarget == GL_TEXTURE_CUBE_MAP_ARRAY;
            // 3D/array images read back every slice, but the FBO path can only read one layer:
            // multi-slice reads are served from the CPU shadow (slice-major, tight layout).
            const GLsizei sliceCount = std::max(size.z(), 1);
            const Bool multiSlice = size.z() > 1;
            if (multiSlice &&
                GetTexImageViaShadowConversion(textureMipmapObject,
                                               MG_Util::ConvertGLEnumToTextureUploadTarget(target), level, size.x(),
                                               size.y(), sliceCount, format, type, pixels, applyPackImageParams)) {
                MGLOG_D("GetTexImage: finished via shadow conversion");
                return;
            }
            if (tempFBOComplete && ReadPixelsViaFormatConversion(0, 0, size.x(), size.y(), format, type, pixels,
                                                                 applyPackImageParams)) {
                MGLOG_D("GetTexImage: finished via client-format conversion");
                return;
            }
            if (GetTexImageViaShadowConversion(textureMipmapObject,
                                               MG_Util::ConvertGLEnumToTextureUploadTarget(target), level, size.x(),
                                               size.y(), sliceCount, format, type, pixels, applyPackImageParams)) {
                MGLOG_D("GetTexImage: finished via shadow conversion");
                return;
            }
            if (!tempFBOComplete) {
                MGLOG_E("GetTexImage: READ FBO incomplete and no shadow copy available, skipping readback");
                return;
            }
            MGLOG_E("GetTexImage: format %s with type %s is not implemented yet, skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return;
        }
        if (!tempFBOComplete) {
            MGLOG_E("GetTexImage: bound READ FBO is not complete");
            return;
        }

        // Handle PBO. The pack binding is scoped: it returns to the resting 0 state
        // on every exit path, so a later readback can never land in a stale PBO.
        auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        Bool usePBO = false;
        GLuint packBufferId = 0;
        if (pixelPackBufferObject) {
            auto* backendResource = BufferImpl::EnsureBufferResource(pixelPackBufferObject);
            MGLOG_D("GetTexImage: Using PBO %u", pixelPackBufferObject->GetExternalIndex());
            if (!backendResource || backendResource->id == 0) {
                MGLOG_E("GetTexImage: No backend buffer found for PBO %u.",
                        pixelPackBufferObject ? pixelPackBufferObject->GetExternalIndex() : 0);
                return;
            }
            usePBO = true;
            packBufferId = backendResource->id;
        } else {
            MGLOG_D("GetTexImage: Not using PBO");
        }
        ScopedPixelPackBuffer packBufferBinding(packBufferId);

        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        MGLOG_D("GetTexImage: glReadPixels(0, 0, %d, %d, %s, %s, %p)", size.x(), size.y(),
                MG_Util::ConvertGLEnumToString(esFormat).c_str(), MG_Util::ConvertGLEnumToString(esType).c_str(),
                pixels);
        g_GLESFuncs.glReadPixels(0, 0, size.x(), size.y(), esFormat, esType, pixels);

        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        if (usePBO) {
            // pull back to client memory if PBO is used
            MGLOG_D("ReadPixels: PBO used, mapping buffer to client memory");
            GLvoid* pboMappedPtr = g_GLESFuncs.glMapBufferRange(
                GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)pixelPackBufferObject->GetSize(), GL_MAP_READ_BIT);
            if (pboMappedPtr) {
                MGLOG_D("ReadPixels: Copying data from PBO to client memory");
                SizeT size = pixelPackBufferObject->GetSize();
                pixelPackBufferObject->WritebackFromBackend({pboMappedPtr, size}, 0);
                MGLOG_D("ReadPixels: Unmapping PBO");
                g_GLESFuncs.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            } else {
                MGLOG_E("ReadPixels: glMapBufferRange returned nullptr");
            }
        }

        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
        MGLOG_D("GetTexImage: finished");
    }

    void SetEGLFuncsTable(const MG_External::EGLFunctionsTable& eglFuncs) {
        g_EGLFuncs = eglFuncs;
    }

    void SetGLESFuncsTable(const MG_External::GLESFunctionsTable& glesFuncs) {
        g_GLESFuncs = glesFuncs;
    }

    void SetGLESCapabilities(const MG_External::GLESCapabilities& capabilities) {
        g_GLESCapabilities = capabilities;
    }

    static EGLDisplay g_Display = EGL_NO_DISPLAY;
    static EGLContext g_Context = EGL_NO_CONTEXT;
    static EGLSurface g_Surface = EGL_NO_SURFACE;
    static EGLConfig g_Config = nullptr;

    static Bool QueryCurrentSurfaceSize(Int& outWidth, Int& outHeight) {
        outWidth = 0;
        outHeight = 0;
        if (!g_EGLFuncs.eglQuerySurface || g_Display == EGL_NO_DISPLAY || g_Surface == EGL_NO_SURFACE) {
            return false;
        }

        EGLint width = 0;
        EGLint height = 0;
        if (!g_EGLFuncs.eglQuerySurface(g_Display, g_Surface, EGL_WIDTH, &width) ||
            !g_EGLFuncs.eglQuerySurface(g_Display, g_Surface, EGL_HEIGHT, &height) ||
            width <= 0 || height <= 0) {
            return false;
        }

        outWidth = static_cast<Int>(width);
        outHeight = static_cast<Int>(height);
        return true;
    }

#if defined(__linux__) && !defined(__ANDROID__)
    static void* OpenX11Lib() {
        void* x11Lib = dlopen("libX11.so.6", RTLD_LOCAL | RTLD_NOW);
        if (!x11Lib) {
            x11Lib = dlopen("libX11.so", RTLD_LOCAL | RTLD_NOW);
        }
        return x11Lib;
    }
#endif

    static EGLint QueryDefaultX11VisualId() {
#if defined(__linux__) && !defined(__ANDROID__)
        const char* displayName = std::getenv("DISPLAY");
        if (!displayName) {
            return 0;
        }

        void* x11Lib = OpenX11Lib();
        if (!x11Lib) {
            return 0;
        }

        using XOpenDisplayFn = void* (*)(const char*);
        using XDefaultScreenFn = int (*)(void*);
        using XDefaultVisualFn = void* (*)(void*, int);
        using XVisualIDFromVisualFn = unsigned long (*)(void*);
        using XCloseDisplayFn = int (*)(void*);

        auto* xOpenDisplay = reinterpret_cast<XOpenDisplayFn>(dlsym(x11Lib, "XOpenDisplay"));
        auto* xDefaultScreen = reinterpret_cast<XDefaultScreenFn>(dlsym(x11Lib, "XDefaultScreen"));
        auto* xDefaultVisual = reinterpret_cast<XDefaultVisualFn>(dlsym(x11Lib, "XDefaultVisual"));
        auto* xVisualIDFromVisual = reinterpret_cast<XVisualIDFromVisualFn>(dlsym(x11Lib, "XVisualIDFromVisual"));
        auto* xCloseDisplay = reinterpret_cast<XCloseDisplayFn>(dlsym(x11Lib, "XCloseDisplay"));
        if (!xOpenDisplay || !xDefaultScreen || !xDefaultVisual || !xVisualIDFromVisual || !xCloseDisplay) {
            dlclose(x11Lib);
            return 0;
        }

        void* display = xOpenDisplay(displayName);
        if (!display) {
            dlclose(x11Lib);
            return 0;
        }
        const int screen = xDefaultScreen(display);
        void* visual = xDefaultVisual(display, screen);
        const auto visualId = visual ? static_cast<EGLint>(xVisualIDFromVisual(visual)) : 0;
        xCloseDisplay(display);
        dlclose(x11Lib);
        return visualId;
#else
        return 0;
#endif
    }

    static EGLint QueryX11WindowVisualId(NativeWindowType window) {
#if defined(__linux__) && !defined(__ANDROID__) && __has_include(<X11/Xlib.h>)
        if (!window) {
            return 0;
        }
        const char* displayName = std::getenv("DISPLAY");
        if (!displayName) {
            return 0;
        }

        void* x11Lib = OpenX11Lib();
        if (!x11Lib) {
            return 0;
        }

        using XOpenDisplayFn = Display* (*)(const char*);
        using XGetWindowAttributesFn = int (*)(Display*, Window, XWindowAttributes*);
        using XVisualIDFromVisualFn = unsigned long (*)(Visual*);
        using XCloseDisplayFn = int (*)(Display*);

        auto* xOpenDisplay = reinterpret_cast<XOpenDisplayFn>(dlsym(x11Lib, "XOpenDisplay"));
        auto* xGetWindowAttributes =
            reinterpret_cast<XGetWindowAttributesFn>(dlsym(x11Lib, "XGetWindowAttributes"));
        auto* xVisualIDFromVisual = reinterpret_cast<XVisualIDFromVisualFn>(dlsym(x11Lib, "XVisualIDFromVisual"));
        auto* xCloseDisplay = reinterpret_cast<XCloseDisplayFn>(dlsym(x11Lib, "XCloseDisplay"));
        if (!xOpenDisplay || !xGetWindowAttributes || !xVisualIDFromVisual || !xCloseDisplay) {
            dlclose(x11Lib);
            return 0;
        }

        Display* display = xOpenDisplay(displayName);
        if (!display) {
            dlclose(x11Lib);
            return 0;
        }

        XWindowAttributes attrs{};
        EGLint visualId = 0;
        if (xGetWindowAttributes(display, static_cast<Window>(window), &attrs) && attrs.visual) {
            visualId = static_cast<EGLint>(xVisualIDFromVisual(attrs.visual));
        }
        xCloseDisplay(display);
        dlclose(x11Lib);
        return visualId;
#else
        (void)window;
        return 0;
#endif
    }

    static Bool GetConfigAttrib(EGLConfig config, EGLint attr, EGLint& value) {
        return g_EGLFuncs.eglGetConfigAttrib && g_EGLFuncs.eglGetConfigAttrib(g_Display, config, attr, &value);
    }

    static Bool ConfigSupports(EGLConfig config, EGLint surfaceBit) {
        EGLint surfaceType = 0;
        EGLint renderableType = 0;
        if (!GetConfigAttrib(config, EGL_SURFACE_TYPE, surfaceType)) {
            return false;
        }
        if (!GetConfigAttrib(config, EGL_RENDERABLE_TYPE, renderableType)) {
            return false;
        }
        return (surfaceType & surfaceBit) && (renderableType & EGL_OPENGL_ES3_BIT);
    }

    static Bool ChooseConfigForSurface(EGLint surfaceBit, EGLConfig& outConfig,
                                       NativeWindowType window = static_cast<NativeWindowType>(0)) {
        const EGLint configAttribs[] = {EGL_SURFACE_TYPE, surfaceBit, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                        EGL_RED_SIZE,     8,          EGL_GREEN_SIZE,      8,
                                        EGL_BLUE_SIZE,    8,          EGL_ALPHA_SIZE,      8,
                                        EGL_DEPTH_SIZE,   24,         EGL_STENCIL_SIZE,    8,
                                        EGL_NONE};

        EGLint numConfigs = 0;
        if (!g_EGLFuncs.eglChooseConfig(g_Display, configAttribs, nullptr, 0, &numConfigs) || numConfigs == 0) {
            return false;
        }

        Vector<EGLConfig> configs(static_cast<SizeT>(numConfigs));
        if (!g_EGLFuncs.eglChooseConfig(g_Display, configAttribs, configs.data(), numConfigs, &numConfigs) ||
            numConfigs == 0) {
            return false;
        }
        configs.resize(static_cast<SizeT>(numConfigs));

        if (surfaceBit == EGL_WINDOW_BIT) {
            const EGLint windowVisualId = QueryX11WindowVisualId(window);
            const EGLint visualIds[] = {windowVisualId, QueryDefaultX11VisualId()};
            for (const auto visualId : visualIds) {
                if (visualId == 0) {
                    continue;
                }
                for (const auto config : configs) {
                    EGLint nativeVisualId = 0;
                    if (ConfigSupports(config, surfaceBit) &&
                        GetConfigAttrib(config, EGL_NATIVE_VISUAL_ID, nativeVisualId) &&
                        nativeVisualId == visualId) {
                        outConfig = config;
                        return true;
                    }
                }
            }
        }

        for (const auto config : configs) {
            if (ConfigSupports(config, surfaceBit)) {
                outConfig = config;
                return true;
            }
        }

        outConfig = configs.front();
        return true;
    }

    static Bool InitDisplayAndContext(EGLint surfaceBit, NativeWindowType window = static_cast<NativeWindowType>(0)) {
        DestroyEGLContext();

        g_Display = g_EGLFuncs.eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_Display == EGL_NO_DISPLAY) return false;

        if (!g_EGLFuncs.eglInitialize(g_Display, nullptr, nullptr)) return false;
        g_EGLFuncs.eglBindAPI(EGL_OPENGL_ES_API);

        if (!ChooseConfigForSurface(surfaceBit, g_Config, window)) return false;

        // Negotiate the highest ES 3.x context. Version-strict EGL implementations
        // (ANGLE) return exactly the requested minor, and a bare CLIENT_VERSION 3
        // request yields a 3.0 context that lacks the 3.1/3.2 texture targets the
        // capability probes exercise; mobile drivers ignore the minor and hand out
        // their maximum either way.
        for (const EGLint minorVersion : {2, 1, 0}) {
            const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                                             EGL_CONTEXT_MINOR_VERSION, minorVersion,
                                             EGL_NONE};
            g_Context = g_EGLFuncs.eglCreateContext(g_Display, g_Config, EGL_NO_CONTEXT, contextAttribs);
            if (g_Context != EGL_NO_CONTEXT) {
                return true;
            }
        }

        const EGLint legacyContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_Context = g_EGLFuncs.eglCreateContext(g_Display, g_Config, EGL_NO_CONTEXT, legacyContextAttribs);
        return g_Context != EGL_NO_CONTEXT;
    }

    namespace {
        // Last swap interval the app requested through eglSwapInterval; -1 = never
        // requested (keep the EGL default of 1). Re-applied when the window surface
        // is (re)created since interval is per-surface state.
        Int g_requestedSwapInterval = -1;

        void ApplyRequestedSwapInterval() {
            if (g_requestedSwapInterval < 0) return;
            if (!g_EGLFuncs.eglSwapInterval || g_Display == EGL_NO_DISPLAY || g_Surface == EGL_NO_SURFACE) return;
            const EGLBoolean ok = g_EGLFuncs.eglSwapInterval(g_Display, g_requestedSwapInterval);
            MGLOG_I("DirectGLES: applied native swap interval %d (%s)", g_requestedSwapInterval,
                    ok ? "ok" : "failed");
        }
    } // namespace

    void SetSwapInterval(Int interval) {
        g_requestedSwapInterval = interval;
        ApplyRequestedSwapInterval();
    }

    Bool InitWindowSurface(NativeWindowType window) {
        if (!window) return false;

        if (!InitDisplayAndContext(EGL_WINDOW_BIT, window)) return false;

        g_Surface = g_EGLFuncs.eglCreateWindowSurface(g_Display, g_Config, window, nullptr);
        if (g_Surface == EGL_NO_SURFACE) return false;

        if (!MakeCurrent()) return false;

        ApplyRequestedSwapInterval();

        MGLOG_D("EGL context created successfully: display=%p, surface=%p, context=%p. window=%p", g_Display, g_Surface,
                g_Context, window);
        return true;
    }

    Bool InitPbufferSurface(EGLint width, EGLint height) {
        if (width <= 0 || height <= 0) return false;
        if (!InitDisplayAndContext(EGL_PBUFFER_BIT)) return false;

        const EGLint surfaceAttribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        g_Surface = g_EGLFuncs.eglCreatePbufferSurface(g_Display, g_Config, surfaceAttribs);
        if (g_Surface == EGL_NO_SURFACE) return false;

        if (!MakeCurrent()) return false;

        MGLOG_D("EGL pbuffer context created successfully: display=%p, surface=%p, context=%p. size=%dx%d", g_Display,
                g_Surface, g_Context, width, height);
        return true;
    }

    namespace {
        // The single backend ES context migrates between app threads (FCL/pojav-style
        // LWJGL hands the EGL context from JVM thread to JVM thread). Ownership must
        // live in ONE global slot: a per-thread flag can never be cleared on the
        // LOSING thread when another thread takes (or destroys/releases) the context,
        // leaving a stale "current" claim behind. A stale claim makes buffer ops issue
        // GL calls that silently no-op (no context is current on that thread) while
        // still updating shadow bookkeeping (bind cache, synced serials), permanently
        // desynchronizing backend buffer state.
        std::atomic<std::thread::id> g_backendContextOwnerThread{};

        // Bumped whenever the backend ES context is destroyed; fence and
        // timer-query handles created under an older generation belong to a
        // dead context and must never be passed back to GL (mirrors
        // BufferImpl's context tracking).
        Uint g_syncContextGeneration = 1;

        // One-fence-per-frame ring driving the buffer-storage pool's recycle gate.
        // A buffer retired during frame N is safe to reuse once frame N's fence has
        // signaled. Touched only in Present()/DestroyEGLContext() on the owning
        // thread -> no lock. Each fence carries the sync generation so a dead-context
        // GLsync is never polled/deleted. Depth 4 >> the 2-3 frames Adreno keeps in
        // flight; wrap-before-signal only happens during a stall and just degrades to
        // the allocate path.
        std::atomic<Uint64> g_currentFrameSerial{0};
        std::atomic<Uint64> g_completedFrameSerial{0};
        constexpr int kFrameFenceRingDepth = 4;
        struct FrameFence {
            GLsync sync = nullptr;
            Uint contextGeneration = 0;
            Uint64 serial = 0;
        };
        FrameFence g_frameFenceRing[kFrameFenceRingDepth];

        // Backend fence handle: a native ES sync plus the ES context
        // generation it was created under.
        struct GLESSyncObject {
            GLsync esSync = nullptr;
            Uint contextGeneration = 0;
        };

        // Backend timer-query handle: a native GL query object name plus the
        // ES context generation it was created under. Stale-generation
        // handles read as available with a zero result, and deleting them
        // only frees the wrapper (the dead ES context already reclaimed the
        // query object).
        struct GLESQueryObject {
            GLuint queryId = 0;
            Uint contextGeneration = 0;
            // Non-zero for the core (non-timer) query targets - occlusion and transform
            // feedback primitives - and then holds the glBeginQuery target, which glEndQuery
            // needs back. Their results are counts reachable only through the core
            // glGetQueryObjectuiv getter: GL_EXT_disjoint_timer_query's 64-bit
            // glGetQueryObjectui64vEXT is timer-specific and may be entirely absent on
            // drivers that otherwise fully support these core ES queries.
            GLenum coreTarget = 0;
        };
    }

    Bool MakeCurrent() {
        if (!g_EGLFuncs.eglMakeCurrent || g_Display == EGL_NO_DISPLAY || g_Surface == EGL_NO_SURFACE ||
            g_Context == EGL_NO_CONTEXT) {
            MGLOG_E("DirectGLES::MakeCurrent failed: EGL display/surface/context is not initialized");
            return false;
        }
        if (!g_EGLFuncs.eglMakeCurrent(g_Display, g_Surface, g_Surface, g_Context)) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E("DirectGLES::MakeCurrent failed: native eglMakeCurrent returned error 0x%04x", error);
            return false;
        }
        g_backendContextOwnerThread.store(std::this_thread::get_id(), std::memory_order_release);
        // The ops table may have been unregistered when a previous ES context was
        // destroyed (e.g. a probe context); re-register now that GL is usable.
        BufferImpl::RegisterBufferBackendOps();
        // Conservatively drop the redundant-glUseProgram guard: re-issuing one bind
        // after a MakeCurrent is cheaper than trusting a possibly-reset context.
        PrgramImpl::g_lastUsedBackendProgramId = 0;
        BufferImpl::InvalidateIndexedBufferBindingCache();
        BufferImpl::InvalidatePixelBufferBindingCaches();
        FramebufferImpl::InvalidateFramebufferBindingCache();
        PixelStoreImpl::InvalidatePackStateCache();
        // eglSwapInterval requires a current context; a request made while none was
        // current (and dropped by the driver) is retried here.
        ApplyRequestedSwapInterval();
        return true;
    }

    Bool ReleaseCurrent() {
        if (!g_EGLFuncs.eglMakeCurrent || g_Display == EGL_NO_DISPLAY) {
            g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
            return true;
        }
        if (!g_EGLFuncs.eglMakeCurrent(g_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E("DirectGLES::ReleaseCurrent failed: native eglMakeCurrent returned error 0x%04x", error);
            return false;
        }
        // Clearing the global owner works from ANY thread (a release request can
        // legally arrive on a thread other than the current owner); erring towards
        // "not current" only defers buffer ops, which is always safe.
        g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
        return true;
    }

    Bool IsBackendContextCurrentOnThisThread() {
        if (g_Context == EGL_NO_CONTEXT) {
            return false;
        }
        if (g_backendContextOwnerThread.load(std::memory_order_acquire) != std::this_thread::get_id()) {
            return false;
        }
        // Belt and braces: EGL itself is the ground truth. A migration that bypassed
        // MakeCurrent()/ReleaseCurrent() must not leave a stale ownership claim
        // standing, or GL calls would silently no-op while shadow bookkeeping (bind
        // cache, synced serials) still advances.
        if (g_EGLFuncs.eglGetCurrentContext && g_EGLFuncs.eglGetCurrentContext() != g_Context) {
            return false;
        }
        return true;
    }

    BackendSyncHandle FenceSync() {
        // ES fences can only be created on the thread that owns the ES context
        // (Flywheel and friends fence on the render thread, which does).
        // Returning null makes the frontend fall back to an always-signaled
        // sync object.
        if (!IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glFenceSync) {
            return nullptr;
        }
        GLsync esSync = g_GLESFuncs.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (esSync == nullptr) {
            return nullptr;
        }
        return new GLESSyncObject{esSync, g_syncContextGeneration};
    }

    GLenum ClientWaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout) {
        const auto* sync = static_cast<GLESSyncObject*>(handle);
        if (sync == nullptr) {
            return GL_ALREADY_SIGNALED;
        }
        // The creating ES context is gone: its GPU work either completed or
        // died with the context; waiting is meaningless either way.
        if (sync->contextGeneration != g_syncContextGeneration) {
            return GL_ALREADY_SIGNALED;
        }
        // Degraded path: a thread that does not own the ES context cannot
        // issue GL calls, so report signaled instead of blocking on state we
        // cannot observe. Fence waits normally arrive on the render thread,
        // which owns the context.
        if (!IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glClientWaitSync) {
            return GL_ALREADY_SIGNALED;
        }
        return g_GLESFuncs.glClientWaitSync(sync->esSync, flags & GL_SYNC_FLUSH_COMMANDS_BIT, timeout);
    }

    void WaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout) {
        (void)flags;
        (void)timeout;
        const auto* sync = static_cast<GLESSyncObject*>(handle);
        if (sync == nullptr || sync->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glWaitSync) {
            return;
        }
        // ES 3.0 requires flags == 0 and timeout == GL_TIMEOUT_IGNORED.
        g_GLESFuncs.glWaitSync(sync->esSync, 0, GL_TIMEOUT_IGNORED);
    }

    void DeleteSync(BackendSyncHandle handle) {
        auto* sync = static_cast<GLESSyncObject*>(handle);
        if (sync == nullptr) {
            return;
        }
        if (sync->contextGeneration == g_syncContextGeneration && IsBackendContextCurrentOnThisThread() &&
            g_GLESFuncs.glDeleteSync) {
            g_GLESFuncs.glDeleteSync(sync->esSync);
        }
        // Otherwise the ES sync is abandoned; the ES context reclaims all of
        // its sync objects when it is destroyed.
        delete sync;
    }

    Bool GetSyncStatus(BackendSyncHandle handle) {
        const auto* sync = static_cast<GLESSyncObject*>(handle);
        if (sync == nullptr || sync->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glGetSynciv) {
            return true;
        }
        GLint status = GL_SIGNALED;
        GLsizei length = 0;
        g_GLESFuncs.glGetSynciv(sync->esSync, GL_SYNC_STATUS, 1, &length, &status);
        return status == GL_SIGNALED;
    }

    // GL timer queries, backed by GL_EXT_disjoint_timer_query. The desktop
    // tokens from glext.h are used throughout: GL_TIME_ELAPSED (0x88BF),
    // GL_TIMESTAMP (0x8E28), GL_QUERY_RESULT (0x8866) and
    // GL_QUERY_RESULT_AVAILABLE (0x8867) are numerically identical to their
    // _EXT counterparts.

    Bool AreTimerQueriesSupported() {
        return g_GLESCapabilities.SupportsDisjointTimerQuery && g_GLESFuncs.glGenQueries &&
               g_GLESFuncs.glDeleteQueries && g_GLESFuncs.glBeginQuery && g_GLESFuncs.glEndQuery &&
               g_GLESFuncs.glGetQueryObjectuiv && g_GLESFuncs.glQueryCounterEXT &&
               g_GLESFuncs.glGetQueryObjectui64vEXT;
    }

    BackendQueryHandle BeginTimeElapsedQuery() {
        // Query objects can only be created on the thread that owns the ES
        // context (MC's F3 profiler queries on the render thread, which
        // does). Returning null makes the frontend fall back to an
        // immediately available zero result.
        if (!IsBackendContextCurrentOnThisThread() || !AreTimerQueriesSupported()) {
            return nullptr;
        }
        GLuint queryId = 0;
        g_GLESFuncs.glGenQueries(1, &queryId);
        if (queryId == 0) {
            return nullptr;
        }
        g_GLESFuncs.glBeginQuery(GL_TIME_ELAPSED, queryId);
        return new GLESQueryObject{queryId, g_syncContextGeneration};
    }

    void EndTimeElapsedQuery(BackendQueryHandle handle) {
        const auto* query = static_cast<GLESQueryObject*>(handle);
        if (query == nullptr || query->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glEndQuery) {
            return;
        }
        // ES tracks the active query per target, not per object, so the
        // handle only guards the degraded paths above.
        g_GLESFuncs.glEndQuery(GL_TIME_ELAPSED);
    }

    BackendQueryHandle QueryCounterTimestamp() {
        if (!IsBackendContextCurrentOnThisThread() || !AreTimerQueriesSupported()) {
            return nullptr;
        }
        GLuint queryId = 0;
        g_GLESFuncs.glGenQueries(1, &queryId);
        if (queryId == 0) {
            return nullptr;
        }
        g_GLESFuncs.glQueryCounterEXT(queryId, GL_TIMESTAMP);
        return new GLESQueryObject{queryId, g_syncContextGeneration};
    }

    // The core (non-timer) query targets - occlusion and transform feedback primitives.
    // Unlike the timer queries above these are core ES (no GL_EXT_disjoint_timer_query
    // needed), so they share one begin/end pair keyed on the glBeginQuery target.
    static Bool AreCoreQueriesSupported() {
        return g_GLESFuncs.glGenQueries && g_GLESFuncs.glDeleteQueries && g_GLESFuncs.glBeginQuery &&
               g_GLESFuncs.glEndQuery && g_GLESFuncs.glGetQueryObjectuiv;
    }

    static BackendQueryHandle BeginCoreQuery(GLenum target) {
        if (!IsBackendContextCurrentOnThisThread() || !AreCoreQueriesSupported()) {
            return nullptr;
        }
        GLuint queryId = 0;
        g_GLESFuncs.glGenQueries(1, &queryId);
        if (queryId == 0) {
            return nullptr;
        }
        g_GLESFuncs.glBeginQuery(target, queryId);
        return new GLESQueryObject{queryId, g_syncContextGeneration, target};
    }

    static void EndCoreQuery(BackendQueryHandle handle) {
        const auto* query = static_cast<GLESQueryObject*>(handle);
        if (query == nullptr || query->coreTarget == 0 ||
            query->contextGeneration != g_syncContextGeneration || !IsBackendContextCurrentOnThisThread() ||
            !g_GLESFuncs.glEndQuery) {
            return;
        }
        g_GLESFuncs.glEndQuery(query->coreTarget);
    }

    Bool AreOcclusionQueriesSupported() { return AreCoreQueriesSupported(); }

    BackendQueryHandle BeginOcclusionQuery() {
        // ES only implements the boolean ANY_SAMPLES_PASSED variant, not an exact
        // GL_SAMPLES_PASSED count; the frontend already coerces ANY_SAMPLES_PASSED*
        // targets to boolean, and desktop GL_SAMPLES_PASSED reads a 0/1 approximation.
        return BeginCoreQuery(GL_ANY_SAMPLES_PASSED);
    }

    void EndOcclusionQuery(BackendQueryHandle handle) { EndCoreQuery(handle); }

    // GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN / GL_PRIMITIVES_GENERATED. The frontend
    // otherwise counts primitives on the CPU from the draw calls, which cannot see a
    // geometry shader's amplification; the real driver's counters are exact. Returning
    // null keeps that CPU accounting as the fallback.
    BackendQueryHandle BeginXfbPrimitivesQuery(Bool generated) {
        // GL_PRIMITIVES_GENERATED is only a legal query target from ES 3.2 on (it comes
        // with geometry shaders); issuing it earlier just leaves a stray GL_INVALID_ENUM
        // that some later unrelated glGetError would report as its own failure.
        if (generated && g_GLESCapabilities.GLESVersion.Major * 10 + g_GLESCapabilities.GLESVersion.Minor < 32) {
            return nullptr;
        }
        return BeginCoreQuery(generated ? GL_PRIMITIVES_GENERATED : GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    }

    void EndXfbPrimitivesQuery(BackendQueryHandle handle) { EndCoreQuery(handle); }

    Bool IsQueryResultAvailable(BackendQueryHandle handle) {
        const auto* query = static_cast<GLESQueryObject*>(handle);
        // Null/stale handles report available so the frontend proceeds to
        // GetQueryResult64, which finalizes them as zero. A thread that does
        // not own the ES context also reports available: GetQueryResult64
        // then returns false and the frontend keeps the handle for a later
        // read from the owning thread.
        if (query == nullptr || query->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glGetQueryObjectuiv) {
            return true;
        }
        GLuint available = GL_FALSE;
        g_GLESFuncs.glGetQueryObjectuiv(query->queryId, GL_QUERY_RESULT_AVAILABLE, &available);
        return available != GL_FALSE;
    }

    Bool GetQueryResult64(BackendQueryHandle handle, Bool wait, Uint64* outNanoseconds) {
        *outNanoseconds = 0;
        const auto* query = static_cast<GLESQueryObject*>(handle);
        // Null handles never had a GL query object, handles from a
        // since-destroyed ES context lost theirs, and missing entry points
        // can never produce a reading (belt and braces: the creators already
        // require them): zero is the FINAL result in all three cases, so
        // report it as produced and let the frontend cache it and release
        // the handle.
        if (query == nullptr || query->contextGeneration != g_syncContextGeneration ||
            !g_GLESFuncs.glGetQueryObjectuiv ||
            (query->coreTarget == 0 && !g_GLESFuncs.glGetQueryObjectui64vEXT)) {
            return true;
        }
        // A thread that does not own the ES context cannot issue GL calls,
        // but the result still lands on the owning context eventually: report
        // "not obtainable yet" so the frontend keeps the handle and a later
        // availability poll / result read from the owning thread can still
        // produce the real value.
        if (!IsBackendContextCurrentOnThisThread()) {
            return false;
        }
        if (wait) {
            // Reading GL_QUERY_RESULT blocks in the driver until the result
            // lands, but only after the commands were flushed; flush once,
            // then poll availability for a bounded ~100ms before dropping to
            // a glFinish as the last resort (ClientWaitSync has no polling
            // loop to mirror - it delegates its timeout to the driver, which
            // a query-object read cannot do).
            if (g_GLESFuncs.glFlush) {
                g_GLESFuncs.glFlush();
            }
            constexpr Int kMaxAvailabilityPolls = 1000; // ~100ms at 100us per poll
            GLuint available = GL_FALSE;
            for (Int i = 0; i < kMaxAvailabilityPolls && available == GL_FALSE; ++i) {
                g_GLESFuncs.glGetQueryObjectuiv(query->queryId, GL_QUERY_RESULT_AVAILABLE, &available);
                if (available == GL_FALSE) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            if (available == GL_FALSE && g_GLESFuncs.glFinish) {
                g_GLESFuncs.glFinish();
            }
        }
        // GL_EXT_disjoint_timer_query's GPU_DISJOINT_EXT signal is
        // deliberately ignored: after a disjoint event (power state change,
        // context switch) the result may be garbage, which is tolerable for
        // an F3 GPU% readout, and consuming the latched flag here could hide
        // the event from another observer.
        if (query->coreTarget != 0) {
            GLuint result32 = 0;
            g_GLESFuncs.glGetQueryObjectuiv(query->queryId, GL_QUERY_RESULT, &result32);
            *outNanoseconds = static_cast<Uint64>(result32);
            return true;
        }
        GLuint64 result = 0;
        g_GLESFuncs.glGetQueryObjectui64vEXT(query->queryId, GL_QUERY_RESULT, &result);
        *outNanoseconds = static_cast<Uint64>(result);
        return true;
    }

    void DeleteBackendQuery(BackendQueryHandle handle) {
        auto* query = static_cast<GLESQueryObject*>(handle);
        if (query == nullptr) {
            return;
        }
        if (query->contextGeneration == g_syncContextGeneration && IsBackendContextCurrentOnThisThread() &&
            g_GLESFuncs.glDeleteQueries) {
            g_GLESFuncs.glDeleteQueries(1, &query->queryId);
        }
        // Otherwise the GL query object is abandoned; the ES context reclaims
        // all of its query objects when it is destroyed.
        delete query;
    }

    Int64 GetGpuTimestampNs() {
        // Synchronous GPU clock sample; 0 tells the frontend GL_TIMESTAMP
        // getter to fall back.
        if (!IsBackendContextCurrentOnThisThread() || !AreTimerQueriesSupported() ||
            !g_GLESFuncs.glGetInteger64v) {
            return 0;
        }
        GLint64 timestamp = 0;
        g_GLESFuncs.glGetInteger64v(GL_TIMESTAMP, &timestamp);
        return static_cast<Int64>(timestamp);
    }

    Uint64 CurrentFrameSerial() { return g_currentFrameSerial.load(std::memory_order_relaxed); }
    Uint64 CompletedFrameSerial() { return g_completedFrameSerial.load(std::memory_order_relaxed); }

    void Present() {
        // Insert one fence per frame BEFORE the swap (eglSwapBuffers' implicit flush
        // makes it reachable), then non-blocking-poll prior frames' fences AFTER to
        // advance the completed-frame watermark that gates buffer-pool recycling.
        const Bool canFence = IsBackendContextCurrentOnThisThread() && g_GLESFuncs.glFenceSync;
        if (canFence) {
            const Uint64 serial = g_currentFrameSerial.fetch_add(1, std::memory_order_relaxed) + 1;
            FrameFence& slot = g_frameFenceRing[serial % kFrameFenceRingDepth];
            if (slot.sync && slot.contextGeneration == g_syncContextGeneration && g_GLESFuncs.glDeleteSync) {
                g_GLESFuncs.glDeleteSync(slot.sync);
            }
            slot = {g_GLESFuncs.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), g_syncContextGeneration, serial};
        }

        g_EGLFuncs.eglSwapBuffers(g_Display, g_Surface);

        if (canFence && g_GLESFuncs.glGetSynciv) {
            // Fences signal in submission order within one context, so the highest
            // signaled serial is a valid contiguous completion watermark.
            Uint64 completed = g_completedFrameSerial.load(std::memory_order_relaxed);
            for (FrameFence& slot : g_frameFenceRing) {
                if (!slot.sync || slot.contextGeneration != g_syncContextGeneration) continue;
                GLint status = GL_SIGNALED;
                GLsizei length = 0;
                g_GLESFuncs.glGetSynciv(slot.sync, GL_SYNC_STATUS, 1, &length, &status);
                if (status == GL_SIGNALED) {
                    if (slot.serial > completed) completed = slot.serial;
                    if (g_GLESFuncs.glDeleteSync) g_GLESFuncs.glDeleteSync(slot.sync);
                    slot.sync = nullptr;
                }
            }
            g_completedFrameSerial.store(completed, std::memory_order_relaxed);
        }

        // After the watermark advanced: retire grown-away UBO-ring stores and record
        // the frame's ring high-water mark for slot reclamation.
        BufferImpl::UboRingOnPresent();
        BufferImpl::TrimBufferPool();
    }

    void DestroyEGLContext() {
        BufferImpl::OnBackendContextDestroyed();
        XfbImpl::OnBackendContextDestroyed();
        ScratchFBOImpl::OnBackendContextDestroyed();
        FramebufferImpl::InvalidateFramebufferBindingCache();
        PixelStoreImpl::InvalidatePackStateCache();
        // Texture ids belong to the dying context; wrappers destroyed later must
        // not glDeleteTextures a recycled name in a successor context.
        ++TextureImpl::g_textureContextGeneration;
        g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
        // Outstanding fence handles now refer to a dead context; treat them as
        // signaled from here on.
        ++g_syncContextGeneration;
        // The frame-fence ring's syncs belong to the dead context too; abandon them
        // (the context reclaims its syncs) and floor the completed watermark to the
        // current serial so buffers retired under the old context read as GPU-idle.
        for (FrameFence& slot : g_frameFenceRing) slot = {};
        g_completedFrameSerial.store(g_currentFrameSerial.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
        if (g_Display != EGL_NO_DISPLAY) {
            g_EGLFuncs.eglMakeCurrent(g_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (g_Context != EGL_NO_CONTEXT) {
                g_EGLFuncs.eglDestroyContext(g_Display, g_Context);
                g_Context = EGL_NO_CONTEXT;
            }
            if (g_Surface != EGL_NO_SURFACE) {
                g_EGLFuncs.eglDestroySurface(g_Display, g_Surface);
                g_Surface = EGL_NO_SURFACE;
            }
            g_EGLFuncs.eglTerminate(g_Display);
            g_Display = EGL_NO_DISPLAY;
        }
    }

} // namespace MobileGL::MG_Backend::DirectGLES
