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
#include "MultiDraw.h"
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Classifiers/TextureEnumClassifier.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/SelfTest/DriverBugProbes.h>
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
#include <bit>
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

    // Two objects are the same binding iff they share a control block. Raw addresses lie
    // (a freed object's heap slot is reused), but a held weak_ptr pins the control block,
    // so no later object can ever owner-equal a snapshot of its predecessor.
    template <typename T>
    static Bool OwnerEquals(const WeakPtr<T>& snapshot, const SharedPtr<T>& current) {
        return !snapshot.owner_before(current) && !current.owner_before(snapshot);
    }

    // Owner-keyed direct-mapped memo over a StateBackendObjectRegistry, for the object
    // kinds the draw path re-Finds every draw (VAO, program). A hit replaces the
    // registry's hash-map Find with one array index plus an owner-equality compare.
    //
    // Why the raw twin pointer is safe to hand back: the twin is owned by the registry
    // entry's SharedPtr, and a LIVE state object's entry is never erased nor has its
    // twin replaced once set — Find and CollectGarbage erase only expired entries,
    // GetOrCreate resets the twin only when the previous owner at that address expired,
    // and the sync paths create a twin only when the slot is null. Owner-equality of
    // the weak snapshot with the live frontend therefore proves the memoed pointer is
    // still that frontend's registered twin. A recycled heap address fails the
    // owner-equality (the weak snapshot pins the predecessor's control block), and a
    // slot collision merely falls back to the registry Find. Nothing here needs an
    // explicit invalidation hook: registry entries survive ES-context loss (twins
    // re-sync via their own generation gates), matching the Find they replace.
    template <typename StateObject, typename BackendObject, SizeT kIndexBits>
    class TwinLookupMemo {
    public:
        BackendObject* Lookup(const SharedPtr<StateObject>& stateObj) const {
            const Slot& slot = m_slots[IndexFor(stateObj.get())];
            if (slot.key == stateObj.get() && slot.twin != nullptr && OwnerEquals(slot.owner, stateObj)) {
                return slot.twin;
            }
            return nullptr;
        }

        void Store(const SharedPtr<StateObject>& stateObj, BackendObject* twin) {
            Slot& slot = m_slots[IndexFor(stateObj.get())];
            slot.key = stateObj.get();
            slot.owner = stateObj;
            slot.twin = twin;
        }

    private:
        struct Slot {
            StateObject* key = nullptr;
            WeakPtr<StateObject> owner;
            BackendObject* twin = nullptr;
        };

        static SizeT IndexFor(const StateObject* ptr) {
            // Fibonacci hashing: heap addresses share alignment zeros and arena locality;
            // the multiply spreads them before the top bits pick the slot.
            const Uint64 h = reinterpret_cast<std::uintptr_t>(ptr) * 0x9E3779B97F4A7C15ull;
            return static_cast<SizeT>(h >> (64u - kIndexBits));
        }

        Array<Slot, (SizeT(1) << kIndexBits)> m_slots;
    };

    // 4096 slots (128 KiB, sparse-touched): Minecraft-shaped workloads cycle hundreds
    // of section VAOs per frame (the driver bench cycles 512), and a colliding pair
    // ping-pongs back onto the hash Find every frame — at 512 keys the expected
    // collided fraction is ~12% here vs ~22% at 2048. Programs are far fewer; 256
    // slots is plenty.
    static TwinLookupMemo<MG_State::GLState::VertexArrayObject, VertexArrayImpl::BackendVertexArrayObject, 12>
        g_vaoTwinLookupMemo;
    static TwinLookupMemo<MG_State::GLState::ProgramObject, PrgramImpl::BackendProgramObjectImpl, 8>
        g_programTwinLookupMemo;
    // Framebuffers qualify for the same memo (see the class comment's registry
    // invariants: g_backendFramebufferObjects only GetOrCreate-resets an expired
    // owner's twin and never erases/replaces a live one). Apps bind a handful of
    // FBOs; 64 slots is plenty.
    static TwinLookupMemo<MG_State::GLState::FramebufferObject, FramebufferImpl::BackendFramebufferObject, 6>
        g_fboTwinLookupMemo;

    // Cached addresses of the frontend's framebuffer binding slots. The frontend
    // getter linear-scans its slot array per call and the draw path asks for these
    // slots several times per draw (FBO sync, FBO texture-attachment sync, the
    // draw-buffer broadcast memo, the draw-time bind). The slots are by-value
    // members of the GLContext, so their addresses are a pure function of the
    // context's address: the cache is keyed on the raw context pointer, and a
    // recreated context landing on the SAME address has its slots at the same
    // addresses again - the cached pointers cannot go stale. Invalidation is
    // exactly the pointer compare below.
    using FbBindingSlot =
        std::remove_reference_t<decltype(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw))>;
    static const MG_State::GLState::GLContext* g_fbSlotCacheContext = nullptr;
    static Array<FbBindingSlot*, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fbSlotCache = {};
    static inline FbBindingSlot& GetFramebufferBindingSlotFast(FramebufferTarget target) {
        MG_State::GLState::GLContext* ctx = MG_State::pGLContext.get();
        if (ctx != g_fbSlotCacheContext) {
            for (SizeT i = 0; i < g_fbSlotCache.size(); ++i) {
                g_fbSlotCache[i] = &ctx->GetFramebufferBindingSlot(static_cast<FramebufferTarget>(i));
            }
            g_fbSlotCacheContext = ctx;
        }
        return *g_fbSlotCache[SizeT(target)];
    }

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

    // Frontend texture target a GLSL sampler uniform samples from. Only used to find
    // which of a unit's bindings carries the GL_TEXTURE_LOD_BIAS the shader needs;
    // targets with no mip chain map to Unknown so the lookup falls back to no bias.
    TextureTarget SamplerUniformTextureTarget(GLenum uniformType) {
        switch (uniformType) {
        case GL_SAMPLER_1D:
        case GL_INT_SAMPLER_1D:
        case GL_UNSIGNED_INT_SAMPLER_1D:
        case GL_SAMPLER_1D_SHADOW:
            return TextureTarget::Texture1D;
        case GL_SAMPLER_2D:
        case GL_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_SAMPLER_2D_SHADOW:
            return TextureTarget::Texture2D;
        case GL_SAMPLER_3D:
        case GL_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
            return TextureTarget::Texture3D;
        case GL_SAMPLER_CUBE:
        case GL_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_SAMPLER_CUBE_SHADOW:
            return TextureTarget::TextureCubeMap;
        case GL_SAMPLER_1D_ARRAY:
        case GL_INT_SAMPLER_1D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
            return TextureTarget::Texture1DArray;
        case GL_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
            return TextureTarget::Texture2DArray;
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
            return TextureTarget::TextureCubeMapArray;
        case GL_SAMPLER_2D_RECT:
        case GL_INT_SAMPLER_2D_RECT:
        case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
        case GL_SAMPLER_2D_RECT_SHADOW:
            return TextureTarget::TextureRectangle;
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
            return TextureTarget::Texture2DMultisample;
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
            return TextureTarget::Texture2DMultisampleArray;
        case GL_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
            return TextureTarget::TextureBuffer;
        default:
            return TextureTarget::Unknown;
        }
    }

    const Uint8* ResolveIndirectCommandBytes(const void* indirect, SizeT requiredBytes, const char* label) {
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (drawBuffer) {
            drawBuffer->SyncPersistentMappedRange();
            const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
            if (commandOffset + requiredBytes > drawBuffer->GetSize()) {
                MGLOG_E_ONCE("%s skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range", label);
                return nullptr;
            }
            return drawBuffer->MappedData() + commandOffset;
        }

        if (!indirect) {
            MGLOG_E_ONCE("%s skipped: indirect pointer is null", label);
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
        // Error HYGIENE is not a debugging feature: every site that brackets a risky ES call with
        // Clear()/Loop() relied on these to empty the driver's queue, and compiling them to
        // nothing left whatever the driver raised sitting there for an unrelated later
        // `glGetError() == GL_NO_ERROR` probe to read as its own failure. The callback stays
        // unused because MGLOG_D is compiled out at this level, but the queue still gets drained.
        // Bounded like DrainESErrors: a driver that never returns GL_NO_ERROR (a lost context is
        // the usual way) must not spin here.
        constexpr Int kMaxDrainedESErrors = 32;

        void ErrorLopper::Loop(const std::function<void(GLenum)>& func) {
            static_cast<void>(func);
            for (Int i = 0; i < kMaxDrainedESErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {
            }
        }

        void ErrorLopper::Clear() {
            for (Int i = 0; i < kMaxDrainedESErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {
            }
        }

        ErrorLopper::ErrorLopper() {
            Clear();
        }
        ErrorLopper::~ErrorLopper() {
            Clear();
        }
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
                    MGLOG_E_ONCE("No backend buffer found for %s binding point %zu.",
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

        // Called once the storage-buffer points are bound and the draw/dispatch is about to
        // go out: whatever the shader writes there lands in the ES driver's buffers, behind
        // the frontend's CPU shadow. Flagging them makes the next MapBuffer/GetBufferSubData
        // pull the real contents back (BufferObject::SyncGpuWrites).
        void MarkShaderStorageBuffersGpuWritten() {
            const SizeT bindingPointCnt =
                MG_State::pGLContext->GetTouchedBufferBindingPointCount(BufferTarget::ShaderStorage);
            for (SizeT i = 0; i < bindingPointCnt; ++i) {
                const auto& obj =
                    MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, i).GetBoundObject();
                if (obj) obj->MarkGpuWritten();
            }
        }

        void SyncAtomicCounterBuffers(const Vector<Int>& glBindings, Int esslBindingTop) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const SizeT pointCount = MG_State::pGLContext->GetBufferBindingPointCount(BufferTarget::AtomicCounter);
            for (const Int glBinding : glBindings) {
                if (glBinding < 0 || static_cast<SizeT>(glBinding) >= pointCount) continue;
                const Int esslBinding = esslBindingTop - glBinding;
                // Already diagnosed once when the block was transpiled; nothing was bound to it
                // there either, so there is nothing to unbind here.
                if (esslBinding < 0) continue;
                auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::AtomicCounter,
                                                                          static_cast<Uint>(glBinding));
                auto& obj = point.GetBoundObject();
                if (!obj) {
                    BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, static_cast<Uint>(esslBinding), 0);
                    continue;
                }

                auto* backendResource = EnsureBufferResource(obj);
                if (!backendResource || backendResource->id == 0) {
                    MGLOG_E_ONCE("No backend buffer found for atomic counter binding point %d.", glBinding);
                    continue;
                }

                const auto& range = point.GetRange();
                if (range.start == 0 && range.end >= obj->GetSize()) {
                    BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, static_cast<Uint>(esslBinding),
                                         backendResource->id);
                } else {
                    const auto start = std::min(range.start, obj->GetSize());
                    const auto end = std::min(range.end, obj->GetSize());
                    BindBufferRangeCached(GL_SHADER_STORAGE_BUFFER, static_cast<Uint>(esslBinding),
                                          backendResource->id, static_cast<GLintptr>(start),
                                          static_cast<GLsizeiptr>(end - start));
                }
                // The whole point of a counter is that the shader INCREMENTS it, and every
                // conformance case reads the result back with glMapBufferRange or
                // glGetBufferSubData - which serve the frontend's CPU shadow until the buffer is
                // flagged (BufferObject::SyncGpuWrites), exactly as for a storage buffer.
                obj->MarkGpuWritten();
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
                MGLOG_E_ONCE("No backend buffer found for %s.", MG_Util::ConvertGLEnumToString(glTarget).c_str());
                return;
            }
            BindBufferId(glTarget, backendResource->id);
        }

        // `vaoConfigVersion` is the caller's early read of currentVAOObject->GetConfigVersion():
        // the VAO's config fields live on a cache line the draw path touches nowhere else, and
        // cycling section VAOs makes that a guaranteed miss - reading it at the top of
        // PrepareForDraw overlaps the miss with the program/texture-key work instead of
        // stalling the memo check below. Nothing between the read and here can move it
        // (only frontend GL entry points mutate VAO config, none run inside a preparation).
        void SyncNeccessaryBuffers(const SharedPtr<MG_State::GLState::VertexArrayObject>& currentVAOObject,
                                   VertexArrayImpl::BackendVertexArrayObject* vaoTwin, Uint32 vaoConfigVersion,
                                   Bool includeIBO = false, Bool includeIndirectBuffer = false) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            ProcessDeferredBufferReleases();

            // All buffers we need are:
            //   1.VBO 2.IBO (if needed) 3.UBO 4.IndirectBuffer (if needed)
            // PBO is not needed since it should be handled in frontend

            if (!currentVAOObject) {
                MGLOG_E_ONCE("No VAO is currently bound, cannot sync necessary buffers.");
                return;
            }

            // VBO. The distinct-buffer set is memoed on the VAO's backend twin: cycling
            // hundreds of section VAOs re-walked 32 cold VertexAttribute slots per draw
            // only to rediscover the same one or two static buffers. While the config
            // version holds (every attach/enable/disable bumps it, and it pins each
            // memoed frontend pointer via the attribute SharedPtrs), the walk reduces to
            // an IsBufferDrawClean probe per distinct buffer; only dirty entries take
            // EnsureBufferResource, re-fetched through their attribute index.
            // Pre-pass epoch read (acquire), stamped into the memo only after a pass in
            // which EVERY probe came up clean: while the stamp still equals the current
            // epoch, no path that can dirty any buffer has run (see the mutation-site
            // enumeration at CurrentBufferMutationEpoch's declaration in Managers.h),
            // so the probes themselves are skipped. A mid-pass bump lands after this
            // read, makes the stamp stale, and re-runs the probes next draw.
            const Uint64 bufferEpoch = CurrentBufferMutationEpoch();
            auto* memo = vaoTwin ? &vaoTwin->GetResolvedDrawBuffersMemo() : nullptr;
            const Uint32 configVersion = vaoConfigVersion;
            if (memo && memo->valid && memo->configVersion == configVersion) {
                if (memo->vboCleanEpoch != bufferEpoch) {
                    Bool allClean = true;
                    for (Uint i = 0; i < memo->count; ++i) {
                        auto& entry = memo->entries[i];
                        if (IsBufferDrawClean(entry.frontend, entry.resource)) continue;
                        allClean = false;
                        // Same object the entry was built from: config version unchanged.
                        entry.resource =
                            EnsureBufferResource(currentVAOObject->GetAttribute(entry.attribIndex).Buffer);
                    }
                    // Entries EnsureBufferResource repaired are not re-probed here; the
                    // next pass's clean probes stamp them (one extra pass, never a skip
                    // of work).
                    memo->vboCleanEpoch = allClean ? bufferEpoch : 0;
                }
            } else {
                // Full walk, once per distinct buffer (an interleaved Minecraft-shaped VAO
                // feeds all attributes out of one VBO), rebuilding the memo as it goes.
                // The twin can only be absent on the error path that has no VAO twin at
                // all; the dedupe scratch keeps this branch correct even then.
                MG_State::GLState::BufferObject*
                    syncedBuffers[MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS];
                Uint syncedBufferCount = 0;
                const auto& allAttributes = currentVAOObject->GetAllAttributes();
                for (Uint attribIndex = 0; attribIndex < allAttributes.size(); ++attribIndex) {
                    const auto& attrib = allAttributes[attribIndex];
                    if (!attrib.Enabled) continue;
                    const auto& bufferObject = attrib.Buffer;
                    if (!bufferObject) continue;

                    auto* const bufferKey = bufferObject.get();
                    Bool alreadySynced = false;
                    for (Uint i = 0; i < syncedBufferCount; ++i) {
                        if (syncedBuffers[i] == bufferKey) {
                            alreadySynced = true;
                            break;
                        }
                    }
                    if (alreadySynced) continue;

                    auto* resource = EnsureBufferResource(bufferObject);
                    if (memo) {
                        auto& entry = memo->entries[syncedBufferCount];
                        entry.frontend = bufferKey;
                        entry.attribIndex = static_cast<Uint8>(attribIndex);
                        entry.resource = resource;
                    }
                    syncedBuffers[syncedBufferCount++] = bufferKey;
                }
                if (memo) {
                    memo->count = syncedBufferCount;
                    memo->configVersion = configVersion;
                    memo->valid = true;
                    // Rebuilt via EnsureBufferResource, not probed clean: the next
                    // probe pass stamps the epoch.
                    memo->vboCleanEpoch = 0;
                }
            }

            // IBO, memoed by bound-object identity (its slot version is not covered by
            // the config version). A stale identity hit is impossible in effect: the
            // clean probe re-validates the resource against the LIVE bound object.
            if (includeIBO) {
                const auto& possibleIBO = currentVAOObject->GetIndexBufferBindingSlot().GetBoundObject();
                if (possibleIBO) {
                    // The epoch stamp alone is NOT enough here: the index slot can
                    // rebind another buffer with no epoch (and no config-version) move,
                    // so the identity compare always runs; only the clean PROBE is
                    // elided while the stamp holds.
                    if (memo && memo->iboFrontend == possibleIBO.get() && memo->iboCleanEpoch == bufferEpoch) {
                        // probed fully clean at this epoch; nothing can have dirtied it
                    } else if (memo && memo->iboFrontend == possibleIBO.get() &&
                               IsBufferDrawClean(memo->iboFrontend, memo->iboResource)) {
                        memo->iboCleanEpoch = bufferEpoch;
                    } else {
                        auto* resource = EnsureBufferResource(possibleIBO);
                        if (memo) {
                            memo->iboFrontend = possibleIBO.get();
                            memo->iboResource = resource;
                            // Repaired, not probed clean: stamp on the next clean probe.
                            memo->iboCleanEpoch = 0;
                        }
                    }
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
            MarkShaderStorageBuffersGpuWritten();
        }

        void SyncComputeBuffers(Bool includeDispatchIndirectBuffer) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            ProcessDeferredBufferReleases();
            SyncBufferBindingPoints(BufferTarget::Uniform, GL_UNIFORM_BUFFER);
            SyncBufferBindingPoints(BufferTarget::ShaderStorage, GL_SHADER_STORAGE_BUFFER);
            MarkShaderStorageBuffersGpuWritten();
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
    //
    // Transform feedback objects (ARB_transform_feedback2) are ES 3.0 core, so each
    // frontend object gets one of the driver's: a paused span lives inside the ES object,
    // which is the only way several of them can be paused at once - and the only reason
    // the default object alone would not do.
    namespace XfbImpl {
        namespace {
            struct XfbCaptureTarget {
                SharedPtr<MG_State::GLState::BufferObject> buffer;
                Uint backendId = 0;
                SizeT start = 0;
                SizeT end = 0;
            };

            // Per frontend transform feedback object. The default object (name 0) maps to
            // the driver's default object (id 0) and is always present.
            struct XfbObjectState {
                GLuint esId = 0;
                Bool pending = false; // frontend Begin seen, driver capture not started yet
                Bool started = false; // driver capture running
                Bool paused = false;  // frontend Pause seen and not yet resumed
                GLenum primitiveMode = GL_POINTS;
                Vector<XfbCaptureTarget> targets;
                // Set for a layout ES cannot express (gl_SkipComponents / gl_NextBuffer):
                // the driver captures gap-free records into the scratch buffer below and
                // End scatters them into `targets`.
                Bool scattered = false;
                SharedPtr<MG_State::GLState::ProgramObject> scatterProgram;
                SizeT scatterCapacityVertices = 0;
            };

            // One scratch ES buffer serves every scattered capture: only one span can be
            // recording at a time (the driver would reject a second Begin), so its contents
            // are consumed by the End that follows.
            GLuint g_scatterBufferId = 0;
            SizeT g_scatterBufferSize = 0;

            UnorderedMap<GLuint, XfbObjectState> g_xfbObjects;
            GLuint g_currentXfbName = 0;
            // Cached address of g_xfbObjects[g_currentXfbName]: PrepareForDraw consults
            // CurrentXfb on EVERY draw (StartPendingTransformFeedback) and the map
            // lookup was pure per-draw overhead for the overwhelmingly common no-capture
            // case. Open addressing keeps values in the bucket array, so ANY insert can
            // rehash and move them - and erase moves them too, by shifting the rest of the
            // probe cluster into the hole, which reaches entries other than the erased one.
            // Every site that mutates the map or rebinds the current name resets this to
            // null instead of reasoning about stability, and CurrentXfb re-resolves lazily.
            XfbObjectState* g_currentXfbState = nullptr;

            XfbObjectState& CurrentXfb() {
                if (g_currentXfbState == nullptr) {
                    g_currentXfbState = &g_xfbObjects[g_currentXfbName];
                }
                return *g_currentXfbState;
            }

            Bool AreTransformFeedbackObjectsSupported() {
                return g_GLESFuncs.glGenTransformFeedbacks != nullptr &&
                       g_GLESFuncs.glBindTransformFeedback != nullptr &&
                       g_GLESFuncs.glDeleteTransformFeedbacks != nullptr &&
                       g_GLESFuncs.glPauseTransformFeedback != nullptr &&
                       g_GLESFuncs.glResumeTransformFeedback != nullptr;
            }

            // Mirrors one capture span's results into the frontend CPU shadows. The GPU wrote
            // the capture buffers behind the frontend's back, so the shadows that back
            // MapBuffer/GetBufferSubData still hold the pre-draw bytes. Buffers whose storage
            // the backend already owns (coherent persistent map) need nothing: reads resolve
            // against that storage directly.
            void ReadbackCapturedRanges(Vector<XfbCaptureTarget>& targets) {
                if (g_GLESFuncs.glMapBufferRange != nullptr && g_GLESFuncs.glUnmapBuffer != nullptr) {
                    for (const auto& target : targets) {
                        if (!target.buffer || target.buffer->IsBackendPersistentMapped()) continue;
                        const SizeT size = target.end - target.start;
                        BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, target.backendId);
                        void* mapped = g_GLESFuncs.glMapBufferRange(BufferImpl::TempBufferTarget,
                                                                    static_cast<GLintptr>(target.start),
                                                                    static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
                        if (mapped == nullptr) {
                            MGLOG_E_ONCE("EndTransformFeedback: failed to map backend buffer %u for capture readback",
                                    target.backendId);
                            continue;
                        }
                        target.buffer->WritebackFromBackend({mapped, size}, target.start);
                        // WritebackFromBackend bumps the frontend change serial with no
                        // backend op, leaving the buffer draw-dirty behind the epoch's
                        // back; re-open the draw-clean memos.
                        BufferImpl::BumpBufferMutationEpoch();
                        g_GLESFuncs.glUnmapBuffer(BufferImpl::TempBufferTarget);
                    }
                }
                targets.clear();
            }

            // Binds a scratch buffer, sized for `capacityVertices` gap-free records, to
            // capture point 0 in place of the application's buffers. Returns false when the
            // scratch storage cannot be provided, in which case the caller falls back to the
            // direct binding (which produces a wrong layout, but is what happened before).
            Bool BindScatterCaptureBuffer(SizeT packedStride, SizeT capacityVertices) {
                if (packedStride == 0 || capacityVertices == 0) return false;
                if (g_GLESFuncs.glGenBuffers == nullptr || g_GLESFuncs.glBufferData == nullptr) return false;
                const SizeT required = packedStride * capacityVertices;
                if (g_scatterBufferId == 0) {
                    g_GLESFuncs.glGenBuffers(1, &g_scatterBufferId);
                    if (g_scatterBufferId == 0) return false;
                    g_scatterBufferSize = 0;
                }
                if (g_scatterBufferSize < required) {
                    BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, g_scatterBufferId);
                    g_GLESFuncs.glBufferData(BufferImpl::TempBufferTarget, static_cast<GLsizeiptr>(required), nullptr,
                                             GL_DYNAMIC_COPY);
                    g_scatterBufferSize = required;
                }
                // Point 0 carries every captured varying (the ES capture is INTERLEAVED); the
                // other points must be cleared or the driver would still write the app's buffers.
                BufferImpl::BindBufferRangeCached(GL_TRANSFORM_FEEDBACK_BUFFER, 0, g_scatterBufferId, 0,
                                                  static_cast<GLsizeiptr>(required));
                const SizeT pointCount =
                    MG_State::pGLContext->GetTouchedBufferBindingPointCount(BufferTarget::TransformFeedback);
                for (SizeT i = 1; i < pointCount; ++i) {
                    BufferImpl::BindBufferBaseCached(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<Uint>(i), 0);
                }
                return true;
            }

            // Distributes the gap-free records the driver captured into the application's
            // buffers at the offsets the GL layout asks for. Only the bytes a varying actually
            // occupies are written, so the holes gl_SkipComponents asks for keep whatever the
            // application had put there - which is the whole point of the feature.
            void ScatterCapturedRecords(XfbObjectState& xfb) {
                const auto& program = xfb.scatterProgram;
                if (!program || xfb.targets.empty()) return;
                if (g_GLESFuncs.glMapBufferRange == nullptr || g_GLESFuncs.glUnmapBuffer == nullptr) return;

                const SizeT packedStride = program->GetTransformFeedbackPackedStride();
                const SizeT vertices = std::min<SizeT>(
                    static_cast<SizeT>(MG_State::pGLContext->GetTransformFeedbackCapturedVertices()),
                    xfb.scatterCapacityVertices);
                if (packedStride == 0 || vertices == 0) return;

                BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, g_scatterBufferId);
                const void* packed = g_GLESFuncs.glMapBufferRange(BufferImpl::TempBufferTarget, 0,
                                                                  static_cast<GLsizeiptr>(packedStride * vertices),
                                                                  GL_MAP_READ_BIT);
                if (packed == nullptr) {
                    MGLOG_E_ONCE("EndTransformFeedback: failed to map the scatter capture buffer");
                    return;
                }

                // One staged copy per destination buffer: start from what the application had
                // (the shadow is authoritative - uploads go shadow -> ES, and previous captures
                // were mirrored back into it), patch the captured varyings in, then push the
                // whole range down once.
                for (SizeT targetIndex = 0; targetIndex < xfb.targets.size(); ++targetIndex) {
                    const auto& target = xfb.targets[targetIndex];
                    if (!target.buffer) continue;
                    const SizeT stride = program->GetTransformFeedbackStride(static_cast<Uint32>(targetIndex));
                    if (stride == 0) continue;
                    const SizeT rangeBytes = target.end - target.start;
                    Vector<Uint8> staged(rangeBytes);
                    Memcpy(staged.data(), target.buffer->MappedData() + target.start, rangeBytes);

                    for (const auto& varying : program->GetTransformFeedbackVaryings()) {
                        if (varying.bufferIndex != targetIndex) continue;
                        for (SizeT v = 0; v < vertices; ++v) {
                            const SizeT dstOffset = v * stride + varying.offsetBytes;
                            if (dstOffset + varying.byteSize > rangeBytes) break;
                            Memcpy(staged.data() + dstOffset,
                                   static_cast<const Uint8*>(packed) + v * packedStride + varying.packedOffsetBytes,
                                   varying.byteSize);
                        }
                    }

                    target.buffer->WritebackFromBackend({staged.data(), rangeBytes}, target.start);
                    // Serial bumped with no backend op (see ReadbackCapturedRanges).
                    BufferImpl::BumpBufferMutationEpoch();
                    if (g_GLESFuncs.glBufferSubData != nullptr) {
                        BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, target.backendId);
                        g_GLESFuncs.glBufferSubData(BufferImpl::TempBufferTarget,
                                                    static_cast<GLintptr>(target.start),
                                                    static_cast<GLsizeiptr>(rangeBytes), staged.data());
                    }
                    BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, g_scatterBufferId);
                }
                g_GLESFuncs.glUnmapBuffer(BufferImpl::TempBufferTarget);
                xfb.targets.clear();
            }
        } // namespace

        Bool AreTransformFeedbacksSupported() {
            return g_GLESFuncs.glBeginTransformFeedback != nullptr &&
                   g_GLESFuncs.glEndTransformFeedback != nullptr &&
                   g_GLESFuncs.glTransformFeedbackVaryings != nullptr;
        }

        Bool IsCaptureSpanOpen() {
            const auto& xfb = CurrentXfb();
            return (xfb.pending || xfb.started) && !xfb.paused;
        }

        void BeginTransformFeedback(GLenum primitiveMode) {
            if (!AreTransformFeedbacksSupported()) return;
            auto& xfb = CurrentXfb();
            xfb.primitiveMode = primitiveMode;
            xfb.pending = true;
            xfb.started = false;
            xfb.paused = false;
            xfb.targets.clear();
        }

        // Tail of PrepareForDraw: the program is bound and every buffer the draw needs
        // is up to date, so the capture buffers can be bound and the span opened.
        void StartPendingTransformFeedback() {
            auto& xfb = CurrentXfb();
            // A span that was paused before its first draw must not open here: the draw is
            // not captured, and opening the span would also subject it to the capture
            // primitive-mode rule the paused draw is exempt from.
            if (!xfb.pending || xfb.paused) return;
            xfb.pending = false;
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
                xfb.targets.push_back({bufferObject, backendResource->id, start, end});
            }

            BufferImpl::SyncBufferBindingPoints(BufferTarget::TransformFeedback, GL_TRANSFORM_FEEDBACK_BUFFER);

            // A layout with holes or several interleaved buffers is not expressible on ES:
            // capture gap-free into scratch storage and place the records at End instead.
            xfb.scattered = false;
            xfb.scatterProgram.reset();
            xfb.scatterCapacityVertices = 0;
            if (program->NeedsScatteredTransformFeedbackCapture()) {
                SizeT capacityVertices = ~SizeT(0);
                for (SizeT i = 0; i < xfb.targets.size(); ++i) {
                    const SizeT stride = program->GetTransformFeedbackStride(static_cast<Uint32>(i));
                    if (stride == 0) continue;
                    capacityVertices =
                        std::min<SizeT>(capacityVertices, (xfb.targets[i].end - xfb.targets[i].start) / stride);
                }
                if (capacityVertices == ~SizeT(0)) capacityVertices = 0;
                if (BindScatterCaptureBuffer(program->GetTransformFeedbackPackedStride(), capacityVertices)) {
                    xfb.scattered = true;
                    xfb.scatterProgram = program;
                    xfb.scatterCapacityVertices = capacityVertices;
                }
            }

            g_GLESFuncs.glBeginTransformFeedback(xfb.primitiveMode);
            xfb.started = true;
        }

        void EndTransformFeedback() {
            auto& xfb = CurrentXfb();
            xfb.pending = false;
            xfb.paused = false;
            if (!xfb.started) return;
            xfb.started = false;
            g_GLESFuncs.glEndTransformFeedback();
            if (xfb.scattered) {
                ScatterCapturedRecords(xfb);
                xfb.scattered = false;
                xfb.scatterProgram.reset();
            } else {
                ReadbackCapturedRanges(xfb.targets);
            }
        }

        void PauseTransformFeedback() {
            auto& xfb = CurrentXfb();
            xfb.paused = true;
            // A span the driver never opened (paused before the first draw) has nothing to
            // pause; the flag above is what holds the deferred Begin back until the resume.
            if (!xfb.started || g_GLESFuncs.glPauseTransformFeedback == nullptr) return;
            g_GLESFuncs.glPauseTransformFeedback();
        }

        void ResumeTransformFeedback() {
            auto& xfb = CurrentXfb();
            xfb.paused = false;
            if (!xfb.started || g_GLESFuncs.glResumeTransformFeedback == nullptr) return;
            g_GLESFuncs.glResumeTransformFeedback();
        }

        void BindTransformFeedback(GLuint name) {
            g_currentXfbState = nullptr; // name changes; operator[] below may also rehash
            if (!AreTransformFeedbackObjectsSupported()) {
                // Without driver objects there is only the default span; keep the frontend
                // name so the bookkeeping below stays consistent.
                g_currentXfbName = name;
                return;
            }
            auto& xfb = g_xfbObjects[name];
            if (name != 0 && xfb.esId == 0) {
                g_GLESFuncs.glGenTransformFeedbacks(1, &xfb.esId);
            }
            g_GLESFuncs.glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, xfb.esId);
            g_currentXfbName = name;
        }

        void DeleteTransformFeedback(GLuint name) {
            const auto it = g_xfbObjects.find(name);
            if (it == g_xfbObjects.end()) return;
            if (it->second.esId != 0 && g_GLESFuncs.glDeleteTransformFeedbacks != nullptr) {
                g_GLESFuncs.glDeleteTransformFeedbacks(1, &it->second.esId);
            }
            g_currentXfbState = nullptr; // erase shifts the probe cluster, moving other entries
            g_xfbObjects.erase(it);
            // The frontend reverts to the default object when the bound one is deleted.
            if (g_currentXfbName == name) {
                BindTransformFeedback(0);
            }
        }

        // The ES context went away (or is being torn down): the spans, their buffer ids, the
        // driver objects and the frontend objects they pinned all belonged to it.
        void OnBackendContextDestroyed() {
            g_currentXfbState = nullptr;
            g_xfbObjects.clear();
            g_currentXfbName = 0;
            g_scatterBufferId = 0;
            g_scatterBufferSize = 0;
        }
    } // namespace XfbImpl

    namespace VertexArrayImpl {
        // Resolve-or-create the VAO's backend twin, once per draw: PrepareForDraw passes
        // the result to the buffer sync (resolved-buffers memo host), the VAO sync and
        // the draw-time bind, which each used to run their own registry Find. The raw
        // pointer stays valid for the whole draw: the frontend VAO is pinned by the
        // context binding, and a live object's registry entry is never erased nor its
        // twin replaced (see TwinLookupMemo's contract).
        BackendVertexArrayObject* ResolveVaoTwin(const SharedPtr<MG_State::GLState::VertexArrayObject>& vao) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (auto* twin = g_vaoTwinLookupMemo.Lookup(vao)) {
                return twin;
            }
            auto* backendVAOSlot = g_backendVertexArrayObjects.Find(vao.get());
            auto& backendObj = backendVAOSlot ? *backendVAOSlot : g_backendVertexArrayObjects.GetOrCreate(vao);
            if (!backendObj) {
                backendObj = MakeShared<BackendVertexArrayObject>();
            }
            g_vaoTwinLookupMemo.Store(vao, backendObj.get());
            return backendObj.get();
        }

        void SyncCurrentVAO(const SharedPtr<MG_State::GLState::VertexArrayObject>& currentVAOObject,
                            BackendVertexArrayObject* vaoTwin) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_backendVertexArrayObjects.CollectGarbageIfNeeded();

            if (!currentVAOObject || !vaoTwin) {
                MGLOG_E_ONCE("No VAO is currently bound, cannot sync current VAO.");
                return;
            }

            vaoTwin->SyncToBackend(currentVAOObject);
        }

        // GL: a shader input whose generic attribute array is DISABLED reads that attribute's *current
        // value* (context state set by glVertexAttrib*, default (0,0,0,1)) rather than any buffer.
        // MobileGL stores those values in MG_State only, so without this step the ES driver would feed
        // the shader its own current values, which MobileGL never writes -- i.e. always (0,0,0,1).
        // SyncToBackend has already issued glDisableVertexAttribArray for these locations, so the ES
        // current value is what the shader will actually read.
        void SyncCurrentVertexAttributeValues(BackendVertexArrayObject* vaoTwin,
                                              const SharedPtr<MG_State::GLState::ProgramObject>& program) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!program) return;

            const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            if (!vao || !vaoTwin) return;

            const Uint32 activeAttribMask = program->GetActiveAttributeLocationMask();
            if (activeAttribMask == 0) return;

            // Which of the program's attributes lack an enabled array cannot change without
            // the VAO's config version moving (Enable/DisableAttribute bump it) or the
            // program's active mask changing, so the per-draw Enabled probes reduce to two
            // compares. The memo lives on the twin (1:1 with the VAO, no identity key
            // needed): a function-static single entry missed on every draw once the app
            // cycled section VAOs, re-reading the cold attribute slots each time; here a
            // cycle re-hits every VAO's own entry. The rebuild visits only ACTIVE locations.
            auto& memo = vaoTwin->GetPendingAttribValueMaskMemo();
            const Uint32 configVersion = vao->GetConfigVersion();
            if (!memo.valid || configVersion != memo.configVersion || activeAttribMask != memo.activeMask) {
                Uint32 pending = 0;
                for (Uint32 remaining = activeAttribMask; remaining != 0; remaining &= remaining - 1) {
                    const Uint32 location = static_cast<Uint32>(std::countr_zero(remaining));
                    if (!vao->GetAttribute(location).Enabled) pending |= (1u << location);
                }
                memo.configVersion = configVersion;
                memo.activeMask = activeAttribMask;
                memo.pendingMask = pending;
                memo.valid = true;
            }
            if (memo.pendingMask == 0) return;

            for (Uint32 remaining = memo.pendingMask; remaining != 0; remaining &= remaining - 1) {
                const Uint32 location = static_cast<Uint32>(std::countr_zero(remaining));

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
                    MGLOG_E_ONCE("SyncCurrentVertexAttributeValues: program=%u location=%u has no enabled array and its "
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
            auto* backendTextureSlot = g_backendTextureObjects.Find(textureObject.get());
            auto& backendSlot = backendTextureSlot ? *backendTextureSlot
                                                   : g_backendTextureObjects.GetOrCreate(textureObject);
            if (!backendSlot) {
                backendSlot = MakeShared<BackendTextureObject>();
            }

            // A by-VALUE copy of the twin for the duration of the syncs below. `backendSlot` is a
            // reference INTO the open-addressed registry, and syncing can RE-ENTER this function:
            // a texture created by glTextureView has to sync the texture whose storage it views
            // first (SyncTextureViewToBackend), and that nested call may insert, grow the map and
            // relocate every entry - leaving the reference dangling. Holding the object itself
            // keeps the calls below working on the right twin regardless; the slot is re-resolved
            // at the end for the reference this function returns.
            const SharedPtr<BackendTextureObject> backendObj = backendSlot;

            if (imageBindableStorageRequired) {
                backendObj->RequireImageBindableStorage(textureObject);
            }
            backendObj->SyncTextureParamsToBackend(textureObject);
            backendObj->SyncBuiltinSamplerToBackend(textureObject);
            backendObj->SyncMipmapsToBackend(textureObject);
            // The storage sync may RE-MINT the driver texture - a fresh glTexStorage after a
            // shape change, an image-bindable widening, or the glTextureView that an
            // ARB_texture_view view is created on - which discards every parameter the two calls
            // above just pushed. Re-push them here rather than leaving it to the next sync: the
            // very next thing that happens is usually the draw this sync was run for, and until
            // the filters land the new texture is at the ES defaults, which for a single-level or
            // integer texture is not merely mis-filtered but INCOMPLETE, i.e. it samples zero.
            if (backendObj->NeedsParameterResync()) {
                backendObj->SyncTextureParamsToBackend(textureObject);
                backendObj->SyncBuiltinSamplerToBackend(textureObject);
            }

            auto* refreshedSlot = g_backendTextureObjects.Find(textureObject.get());
            auto& refreshedBackendObj = refreshedSlot ? *refreshedSlot
                                                      : g_backendTextureObjects.GetOrCreate(textureObject);
            if (!refreshedBackendObj) {
                // A collection ran during the nested sync and took this slot with it; put the
                // twin the caller is about to use back, rather than handing back an empty one.
                refreshedBackendObj = backendObj;
            }
            return refreshedBackendObj;
        }

        // Identity snapshot of what one texture unit has bound: the object in every binding
        // slot plus the unit's sampler object. This - not the context's texture bind
        // generation - is what the per-draw texture memos key on: the generation also bumps
        // on REDUNDANT re-binds (glBindSampler of the sampler the unit already carries, which
        // 26.2 issues around every texture-unit switch), so a generation-keyed memo re-derives
        // everything on draws that changed nothing. The snapshot compares the bindings
        // themselves, so only a real change can invalidate it. What it deliberately does NOT
        // cover, and its users must key on separately:
        //   * membership/completeness flips with no binding moving - a default texture's image
        //     appearing or vanishing, any texture shape or sampler parameter change - all bump
        //     the sampling-resolution generation (BumpShapeVersion / SamplerObject::BumpVersion
        //     are their only writers and bump it unconditionally);
        //   * everything unit bindings say nothing about: the touched-unit high-water mark,
        //     the frontend context identity, the backend ES context generation, and (for the
        //     resolution memo) the program keys that arbitrate aliased targets.
        struct UnitBindingsSnapshot {
            Array<WeakPtr<MG_State::GLState::ITextureObject>, (SizeT)TextureTarget::TextureTargetCount>
                slotObjects{};
            WeakPtr<MG_State::GLState::SamplerObject> samplerObject{};
        };

        static void CaptureUnitBindings(Int maxTouchedUnit, Vector<UnitBindingsSnapshot>& out) {
            out.resize(static_cast<SizeT>(maxTouchedUnit + 1));
            for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
                auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
                auto& snapshot = out[static_cast<SizeT>(unit)];
                const auto& slots = textureUnit.GetAllBindingSlots();
                for (SizeT i = 0; i < slots.size(); ++i) {
                    snapshot.slotObjects[i] = slots[i].GetBoundObject();
                }
                snapshot.samplerObject = textureUnit.GetSamplerObject();
            }
        }

        static Bool UnitBindingsUnchanged(Int maxTouchedUnit, const Vector<UnitBindingsSnapshot>& snapshots) {
            if (snapshots.size() != static_cast<SizeT>(maxTouchedUnit + 1)) return false;
            for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
                auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
                const auto& snapshot = snapshots[static_cast<SizeT>(unit)];
                const auto& slots = textureUnit.GetAllBindingSlots();
                for (SizeT i = 0; i < slots.size(); ++i) {
                    if (!OwnerEquals(snapshot.slotObjects[i], slots[i].GetBoundObject())) return false;
                }
                if (!OwnerEquals(snapshot.samplerObject, textureUnit.GetSamplerObject())) return false;
            }
            return true;
        }

        static Vector<UnitBindingsSnapshot> g_observedUnitBindings;
        static Uint64 g_observedUnitBindingsContextId = 0;
        static Uint64 g_observedUnitBindingsGeneration = 0;
        static Int g_observedUnitBindingsMaxUnit = -1;
        static Uint64 g_unitBindingsEpoch = 0;

        // Epoch of the touched units' bindings: moves exactly when WHAT is bound changes,
        // never on a redundant re-bind. Memos snapshot the returned value instead of holding
        // their own UnitBindingsSnapshot, so the owner-compare walk runs at most once per
        // draw no matter how many memos key on it. The observation triple gates the walk:
        // while (context id, bind generation, high-water mark) are unchanged nothing can
        // have moved, because every path that changes a binding bumps the generation. A
        // given epoch value names one observed (context, high-water mark, bindings) state -
        // a context switch, a high-water-mark move or a bindings change each recapture and
        // bump - so epoch equality alone proves the bindings a consumer resolved against
        // are the bindings on the units now.
        static Uint64 CurrentUnitBindingsEpoch(Int maxTouchedUnit) {
            const Uint64 contextId = MG_State::pGLContext->GetTextureContextId();
            const Uint64 bindGeneration = MG_State::pGLContext->GetTextureBindGeneration();
            if (g_observedUnitBindingsContextId == contextId && g_observedUnitBindingsMaxUnit == maxTouchedUnit &&
                g_observedUnitBindingsGeneration == bindGeneration) {
                return g_unitBindingsEpoch;
            }
            if (g_observedUnitBindingsContextId != contextId || g_observedUnitBindingsMaxUnit != maxTouchedUnit ||
                !UnitBindingsUnchanged(maxTouchedUnit, g_observedUnitBindings)) {
                CaptureUnitBindings(maxTouchedUnit, g_observedUnitBindings);
                ++g_unitBindingsEpoch;
            }
            g_observedUnitBindingsContextId = contextId;
            g_observedUnitBindingsMaxUnit = maxTouchedUnit;
            g_observedUnitBindingsGeneration = bindGeneration;
            return g_unitBindingsEpoch;
        }

        // Work list behind SyncNeccessaryTextures' per-draw unit walk. WHICH textures the
        // touched units hold is a pure function of the unit bindings, so the GLContext identity
        // (a never-reused id, not the heap address a recreated context can land on again), the
        // unit-bindings snapshot and the touched-unit high-water mark are a complete key -
        // WHAT each entry then has to do is still decided per draw by the version compares
        // inside the sync calls, which is why texture content, shape and parameter changes need
        // no key here.
        //
        // Entries borrow, they never own. `slot` points at the binding slot's shared_ptr, whose
        // address is fixed for the context's lifetime (TextureState holds the unit array by
        // value) and whose VALUE cannot change without bumping the bind generation. `backend` is
        // the registry's object for the texture in that slot; the registry only erases an entry
        // once the frontend texture has expired, and a frontend texture cannot expire while a
        // slot the key covers still holds a reference to it. Holding either side by shared_ptr
        // instead would keep dead frontend textures alive and defeat the registry's
        // weak-reference GC.
        //
        // `texture` records WHICH frontend object `backend` was paired with when the entry was
        // built, and PairingsIntact re-checks it before any replay. The keys above are the
        // primary guard, but they are all derived state: a slot swap that never reaches the
        // bind generation (the DSA by-name emulation used to swap a slot silently) would leave
        // every key matching while the borrowed slot pointed at a different texture, and the
        // replay would then drive texture A's backend twin from texture B's frontend state -
        // re-specifying A's backend storage with B's shape and destroying A's contents. A raw
        // pointer compare per entry is far cheaper than the walk it guards, and a stale pairing
        // costs only a list rebuild, so this stays as the structural net under the keys.
        struct UnitTextureSyncEntry {
            const SharedPtr<MG_State::GLState::ITextureObject>* slot = nullptr;
            MG_State::GLState::ITextureObject* texture = nullptr;
            BackendTextureObject* backend = nullptr;
        };
        // True while every entry's borrowed slot still holds the texture the entry was paired
        // with. Callers put it LAST in the key conjunction so it only runs on a key hit.
        static Bool PairingsIntact(const Vector<UnitTextureSyncEntry>& list) {
            for (const auto& entry : list) {
                if (entry.slot->get() != entry.texture) return false;
            }
            return true;
        }
        static Vector<UnitTextureSyncEntry> g_unitTextureSyncList;
        static Bool g_unitTextureSyncListValid = false;
        static Uint64 g_unitTextureSyncListContextId = 0;
        static Int g_unitTextureSyncListMaxUnit = -1;
        static Uint g_unitTextureSyncListContextGeneration = 0;
        static Uint64 g_unitTextureSyncListEpoch = 0;
        static Uint64 g_unitTextureSyncListSamplingGeneration = 0;

        // Sibling memo for the draw FBO's texture attachments (see the use site in
        // SyncNeccessaryTextures for the key derivation and the borrow rules, which are the
        // unit list's). A null FBO pointer means "not stamped".
        static Vector<UnitTextureSyncEntry> g_fboTextureSyncList;
        static MG_State::GLState::FramebufferObject* g_fboTextureSyncListFbo = nullptr;
        static Uint16 g_fboTextureSyncListSlotVersion = 0;
        static Uint16 g_fboTextureSyncListObjectVersion = 0;
        static Uint64 g_fboTextureSyncListContextId = 0;
        static Uint g_fboTextureSyncListContextGeneration = 0;

        // The frontend texture-state keys the per-draw texture stages
        // (SyncNeccessaryTextures, then BindCurrentTextures) both consume. Captured
        // ONCE per draw/dispatch preparation and passed to both: nothing between the
        // two stages can move any of these - only frontend GL entry points mutate
        // them, and none run inside a backend preparation (the FBO/program syncs in
        // between only read frontend state). The no-arg wrappers keep capturing at
        // the call for every non-draw call site (Clear, readbacks).
        struct DrawTextureSyncKeys {
            Uint64 contextId = 0;
            Uint64 samplingGeneration = 0;
            Uint64 unitBindingsEpoch = 0;
            Int maxTouchedUnit = -1;
        };

        DrawTextureSyncKeys CaptureDrawTextureSyncKeys() {
            DrawTextureSyncKeys keys;
            keys.contextId = MG_State::pGLContext->GetTextureContextId();
            // Units past the frontend's high-water mark have provably-empty slots.
            keys.maxTouchedUnit = MG_State::pGLContext->GetMaxTouchedTextureUnit();
            keys.samplingGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
            keys.unitBindingsEpoch = CurrentUnitBindingsEpoch(keys.maxTouchedUnit);
            return keys;
        }

        void SyncNeccessaryTextures(const DrawTextureSyncKeys& keys) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_backendTextureObjects.CollectGarbageIfNeeded();

            // All textures we need are:
            //   1. textures bound to texture units (TODO: only sync ones that are used in current program)
            //   2. textures used in current FBO
            //   3. textures bound to image units (TODO)

            const Int maxTouchedUnit = keys.maxTouchedUnit;
            const Uint64 samplingGeneration = keys.samplingGeneration;
            const Uint64 unitBindingsEpoch = keys.unitBindingsEpoch;
            // The epoch survives redundant re-binds; the sampling-resolution generation
            // covers the one membership input the epoch cannot see - a default texture's
            // image appearing or vanishing flips IsUndefinedDefaultTexture with no binding
            // moving. Its cost is a spare rebuild whenever any texture shape or sampler
            // parameter actually changes, which real frames do at load time, not per draw.
            if (g_unitTextureSyncListValid &&
                g_unitTextureSyncListContextId == keys.contextId &&
                g_unitTextureSyncListMaxUnit == maxTouchedUnit &&
                g_unitTextureSyncListContextGeneration == g_backendContextGeneration &&
                g_unitTextureSyncListEpoch == unitBindingsEpoch &&
                g_unitTextureSyncListSamplingGeneration == samplingGeneration &&
                PairingsIntact(g_unitTextureSyncList)) {
                for (const auto& entry : g_unitTextureSyncList) {
                    // Aggregate gate == the conjunction of the three callees' own
                    // early-outs (see IsDrawSyncClean); skipping on true is
                    // behavior-identical, false falls through to the calls.
                    if (entry.backend->IsDrawSyncClean(entry.slot->get(), keys.contextId, samplingGeneration)) {
                        continue;
                    }
                    entry.backend->SyncTextureParamsToBackend(*entry.slot);
                    entry.backend->SyncBuiltinSamplerToBackend(*entry.slot);
                    entry.backend->SyncMipmapsToBackend(*entry.slot);
                }
            } else {
                g_unitTextureSyncListValid = false;
                g_unitTextureSyncList.clear();
                for (Int index = 0; index <= maxTouchedUnit; ++index) {
                    auto& unit = MG_State::pGLContext->GetTextureUnitObject(index);
                    for (const auto& bindingSlot : unit.GetAllBindingSlots()) {
                        auto& textureObject = bindingSlot.GetBoundObject();
                        // An image-less default texture (name 0) is the slot's initial / "unbound"
                        // state; it has nothing to sync, so skip it as cheaply as the old null slot.
                        if (textureObject && !MG_State::GLState::IsUndefinedDefaultTexture(textureObject.get())) {
                            g_unitTextureSyncList.push_back({&textureObject, textureObject.get(),
                                                             SyncTextureObjectToBackend(textureObject).get()});
                        }
                    }
                }
                g_unitTextureSyncListContextId = keys.contextId;
                g_unitTextureSyncListMaxUnit = maxTouchedUnit;
                g_unitTextureSyncListContextGeneration = g_backendContextGeneration;
                g_unitTextureSyncListEpoch = unitBindingsEpoch;
                g_unitTextureSyncListSamplingGeneration = samplingGeneration;
                g_unitTextureSyncListValid = true;
            }

            // Texture attachments of the draw FBO, memoised like the unit list above: WHICH
            // textures hang off the FBO only changes with a rebind (slot version), an
            // attachment/draw-buffer edit (object version - the documented invariant
            // SyncCurrentFBO's memo already leans on), a different FBO landing on a recycled
            // heap address (pointer + slot version together, the StampSyncedFBO trio), another
            // frontend context (context id) or a rebuilt ES context (backend generation). WHAT
            // each texture then needs is still decided per draw by the version gates inside the
            // three sync calls. Entry lifetime mirrors the unit list: the attachment holds the
            // texture's SharedPtr at a stable address while the FBO is unchanged, and the
            // registry keeps a backend object alive until its frontend texture expires, which an
            // attached texture cannot. A renderbuffer-only FBO - the common Minecraft frame -
            // reduces to the key compare and an empty loop.
            const auto& drawSlot = GetFramebufferBindingSlotFast(FramebufferTarget::Draw);
            const auto& currentFBO = drawSlot.GetBoundObject();
            if (currentFBO) {
                const Uint16 fboSlotVersion = drawSlot.GetVersion();
                const Uint16 fboObjectVersion = currentFBO->GetObjectVersion();
                const Bool fboListValid =
                    g_fboTextureSyncListFbo == currentFBO.get() &&
                    g_fboTextureSyncListSlotVersion == fboSlotVersion &&
                    g_fboTextureSyncListObjectVersion == fboObjectVersion &&
                    g_fboTextureSyncListContextId == keys.contextId &&
                    g_fboTextureSyncListContextGeneration == g_backendContextGeneration &&
                    PairingsIntact(g_fboTextureSyncList);
                if (fboListValid) {
                    for (const auto& entry : g_fboTextureSyncList) {
                        // Same aggregate gate as the unit list above.
                        if (entry.backend->IsDrawSyncClean(entry.slot->get(), keys.contextId,
                                                           samplingGeneration)) {
                            continue;
                        }
                        entry.backend->SyncTextureParamsToBackend(*entry.slot);
                        entry.backend->SyncBuiltinSamplerToBackend(*entry.slot);
                        entry.backend->SyncMipmapsToBackend(*entry.slot);
                    }
                } else {
                    g_fboTextureSyncListFbo = nullptr;
                    g_fboTextureSyncList.clear();
                    for (const auto& attachment : currentFBO->GetAllAttachmentObjects()) {
                        if (!attachment.IsTexture()) continue;
                        auto& textureObject = attachment.GetTexture();
                        if (textureObject) {
                            g_fboTextureSyncList.push_back({&textureObject, textureObject.get(),
                                                            SyncTextureObjectToBackend(textureObject).get()});
                        }
                    }
                    g_fboTextureSyncListFbo = currentFBO.get();
                    g_fboTextureSyncListSlotVersion = fboSlotVersion;
                    g_fboTextureSyncListObjectVersion = fboObjectVersion;
                    g_fboTextureSyncListContextId = keys.contextId;
                    g_fboTextureSyncListContextGeneration = g_backendContextGeneration;
                }
            } else {
                g_fboTextureSyncListFbo = nullptr;
                g_fboTextureSyncList.clear();
            }
        }

        void SyncNeccessaryTextures() { SyncNeccessaryTextures(CaptureDrawTextureSyncKeys()); }

        // Whether glBindImageTexture's `layered` means anything for this target - asked of the
        // target the DRIVER will see, not the one the application named. A GL_TEXTURE_1D_ARRAY
        // is stored as an ES 2D array (MapToBackendTextureTarget), and so is layerable; asking
        // the state target instead answered "no" for it and pinned every 1D-array image binding
        // to layer 0, whatever the application passed.
        //
        // `layer` travels with the answer, because GL 4.6 core 8.26 (and ES 3.2 8.22, word for
        // word) makes them one rule: "If the texture identified by texture does not have
        // multiple layers or faces, the entire texture level is bound, regardless of the values
        // of layered and layer." REGARDLESS means ignored - not clamped, and not an error - so
        // the driver must not be handed a layer index the texture has no room for. Adreno takes
        // such a request literally and leaves the image unit reading zero, which is what failed
        // KHR-GL42.bind_image_texture.single_layer's layer:1 rows on GL_TEXTURE_2D and on the
        // GL_TEXTURE_1D that is stored as one. Normalizing here and not in the frontend shadow
        // is deliberate: GL_IMAGE_BINDING_LAYER must keep echoing what the application passed.
        static Bool SupportsLayeredImageBinding(TextureTarget target) {
            const TextureTarget backendTarget = TextureImpl::MapToBackendTextureTarget(target);
            return backendTarget == TextureTarget::Texture3D || backendTarget == TextureTarget::TextureCubeMap ||
                   backendTarget == TextureTarget::Texture2DArray ||
                   backendTarget == TextureTarget::TextureCubeMapArray ||
                   backendTarget == TextureTarget::Texture2DMultisampleArray;
        }

        // Which image units currently hold a WRITABLE buffer texture, and how many. Kept here
        // rather than recomputed per draw because the frontend tracks 192 image units and
        // almost every program uses none of them: the draw path pays one integer test.
        //
        // Maintained by SyncImageTextureBinding, which is the single funnel for an image-unit
        // change on this backend - glBindImageTextures is a frontend loop over
        // glBindImageTexture, and the whole-sweep SyncImageTextureBindings goes through it too.
        // Anything that clears a binding WITHOUT coming through here (a deleted texture, a
        // recreated context) can only leave a bit set for a unit that no longer has one; the
        // sweep re-reads the binding, finds nothing to mark, and CLEARS the bit on its way past.
        // So the error is self-healing, and in the direction that costs one wasted look rather
        // than one missed write.
        static Array<Bool, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_writableImageBufferUnits{};
        static Uint g_writableImageBufferUnitCount = 0;

        static Bool IsWritableImageBufferTexture(const MG_State::GLState::ImageTextureBinding& binding) {
            return binding.Texture != nullptr && binding.Access != GL_READ_ONLY &&
                   binding.Texture->GetStorageType() == TextureStorageType::Buffer;
        }

        static void TrackWritableImageBufferUnit(Uint unit, Bool writableBufferTexture) {
            Bool& tracked = g_writableImageBufferUnits[unit];
            if (tracked == writableBufferTexture) return;
            tracked = writableBufferTexture;
            // Written as two guarded steps rather than one signed add: the count is unsigned,
            // and a decrement that ever ran one time too many would not saturate at zero, it
            // would wrap to four billion and defeat the early-out for the rest of the process.
            if (writableBufferTexture) {
                ++g_writableImageBufferUnitCount;
            } else if (g_writableImageBufferUnitCount > 0) {
                --g_writableImageBufferUnitCount;
            }
        }

        // Highest image unit that has ever been given a texture, plus one. Maintained by the
        // single funnel below, so it is a sound "no draw in this context can be reading an image"
        // test: nothing reaches an image unit without going through SyncImageTextureBinding.
        // Almost every program (every Minecraft draw) leaves it at zero, which is what keeps the
        // draw-path staleness check below at one integer test.
        static Uint g_imageUnitHighWaterMark = 0;

        void SyncImageTextureBinding(Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(unit));
            TrackWritableImageBufferUnit(unit, IsWritableImageBufferTexture(imageBinding));
            if (imageBinding.Texture && unit + 1 > g_imageUnitHighWaterMark) {
                g_imageUnitHighWaterMark = unit + 1;
            }
            if (!imageBinding.Texture) {
                g_GLESFuncs.glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                return;
            }

            auto& backendTexture = SyncTextureObjectToBackend(imageBinding.Texture, true);
            const Bool layerable = SupportsLayeredImageBinding(imageBinding.Texture->GetTarget());
            const GLboolean layered = layerable ? imageBinding.Layered : GL_FALSE;
            const GLint layer = layerable ? imageBinding.Layer : 0;
            // The bind half of the image-format widening. SyncTextureObjectToBackend has just
            // allocated this texture's storage in the core carrier of its format (the call above
            // is the one that marks it image-bindable), and glBindImageTexture's `format` has to
            // name the storage the texture really has: a GL_RG32F bind is GL_INVALID_VALUE on
            // Adreno for nineteen of the twenty-six non-core formats and on both Malis for
            // twenty-five, and every driver that DOES accept a narrow texture through a wide
            // image accepts it silently, reading and writing out of bounds. The frontend's own
            // ImageTextureBinding keeps the application's format untouched, so
            // GL_IMAGE_BINDING_FORMAT still answers what was passed in.
            //
            // Widened from the format the APPLICATION named rather than from the texture's own,
            // because GL lets the two differ inside one format class and the shader was widened
            // from the class the application named too (an r32ui view of an r32f image is a legal
            // reinterpretation). The two carriers always have the same texel size - every format
            // in a class widens to the four-channel form of that same class - so the storage
            // still describes what the bind claims. Gated on the TEXTURE having been widened, so
            // a bind format that names a class the storage does not have is left alone: GL
            // already calls that undefined, and inventing a carrier for it would only make the
            // out-of-class read wider.
            //
            // A BUFFER texture is excluded from the WIDENING on both sides: it has no storage of
            // its own to widen (its texels are the application's buffer object), so
            // WidenImageFormatsPass declines to widen every buffer image and the bind must decline
            // with it, or the driver would be handed a carrier the shader never addressed. See the
            // Dim::Buffer guard there for the 32-byte GL_RG32F measurement that pinned it.
            //
            // What a buffer image takes instead is the SPLIT, which is the same three-layer move
            // through a different door: a private glTexBuffer view names the single-channel base
            // format, the bind below names it too, and the shader subscripts it two components per
            // original texel. Same gate on all three, so they cannot disagree.
            //
            // The split view is a SEPARATE texture name over the same buffer, and the bind has to
            // name it rather than the application's own: the application's texture keeps the
            // format it asked for so that a samplerBuffer reading the same buffer texture - which
            // is NOT subscript-rewritten - still sees whole texels. See
            // BackendTextureObject::m_bufferImageSplitViewId.
            GLenum bindFormat = imageBinding.Format;
            GLuint bindTextureId = backendTexture->GetBackendTextureId();
            if (imageBinding.Texture->GetTarget() == TextureTarget::TextureBuffer) {
                if (TextureImpl::GetImageBindableBufferSplitFormat(imageBinding.Texture->GetFormat()) !=
                    GL_UNKNOWN_MGL) {
                    if (const GLenum boundFormatSplit = TextureImpl::GetImageBindableBufferSplitFormat(
                            MG_Util::ConvertGLEnumToTextureInternalFormat(imageBinding.Format));
                        boundFormatSplit != GL_UNKNOWN_MGL) {
                        bindFormat = boundFormatSplit;
                        if (const Uint splitViewId = backendTexture->GetBufferImageSplitViewId();
                            splitViewId != 0) {
                            bindTextureId = splitViewId;
                        }
                    }
                }
            } else if (TextureImpl::GetImageBindableStorageWidening(imageBinding.Texture->GetFormat())) {
                const auto boundFormatWidening = TextureImpl::GetImageBindableStorageWidening(
                    MG_Util::ConvertGLEnumToTextureInternalFormat(imageBinding.Format));
                if (boundFormatWidening) {
                    bindFormat = boundFormatWidening.InternalFormat;
                }
            }
            g_GLESFuncs.glBindImageTexture(unit, bindTextureId, imageBinding.Level,
                                           layered, layer, imageBinding.Access, bindFormat);
        }

        // A buffer texture bound to a WRITABLE image unit is a buffer the shader is about to
        // write, and those writes land in the ES driver's buffer object - behind the frontend's
        // CPU shadow, which is what MapBuffer and GetBufferSubData read. Same flag, and for the
        // same reason, as MarkShaderStorageBuffersGpuWritten does for a storage block; the
        // difference is only which binding the shader reaches the buffer through. A GL_READ_ONLY
        // binding is left alone: marking it would make the next map wait on - and then re-read -
        // a dispatch that could not have changed a byte of it.
        //
        // Called from the draw and dispatch preparations rather than from the eager sync
        // glBindImageTexture performs: that one runs before any shader has touched the buffer,
        // and flagging there would pull the driver's copy over a shadow the application may
        // still be writing into.
        void MarkWritableImageBufferTexturesGpuWritten() {
            if (g_writableImageBufferUnitCount == 0) return;
            for (Uint unit = 0; unit < g_writableImageBufferUnits.size(); ++unit) {
                if (!g_writableImageBufferUnits[unit]) continue;
                const auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(unit));
                if (!IsWritableImageBufferTexture(imageBinding)) {
                    TrackWritableImageBufferUnit(unit, false);
                    continue;
                }
                auto* textureBuffer =
                    static_cast<MG_State::GLState::TextureObjectBuffer*>(imageBinding.Texture.get());
                const auto& bufferObject = textureBuffer->GetBufferBindingSlot().GetBoundObject();
                if (bufferObject) bufferObject->MarkGpuWritten();
            }
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

        // What the draw path last swept the image units against. A draw never swept them at all:
        // an image unit was established once, eagerly, by glBindImageTexture and never revisited.
        // That is stale the moment the texture behind it is re-specified with a new size or
        // format, because ES 3.1 only allows IMMUTABLE storage on an image unit
        // (SyncTextureObjectToBackend's imageBindableStorageRequired), immutable storage cannot be
        // redefined, and so the re-spec MINTS A NEW ES TEXTURE NAME - leaving the unit pointing at
        // the deleted one and imageSize() reporting the old dimensions
        // (KHR-GL43.shader_image_size.advanced-changeSize).
        static Uint64 g_imageSweepContextId = 0;
        static Uint64 g_imageSweepSamplingGeneration = 0;
        static Uint g_imageSweepBackendContextGeneration = 0;
        static Bool g_imageSweepValid = false;

        // The sweep is a glBindImageTexture per unit, so it must not run per draw: the gate is the
        // frontend's sampling-resolution generation, which TextureObjectBase::BumpShapeVersion
        // moves on exactly the shape and format changes that can force the re-mint. Deliberately
        // NOT the backend-side re-mint counter (g_attachmentBackendIdGeneration's sibling would be
        // the obvious choice): a texture that is bound ONLY to an image unit is re-minted inside
        // this very sweep, so a backend-side trigger would be bumped after the gate had already
        // declined to run it.
        void SyncImageTextureBindingsForDraw(const DrawTextureSyncKeys& keys) {
            if (g_imageUnitHighWaterMark == 0) return;
            if (g_imageSweepValid && g_imageSweepContextId == keys.contextId &&
                g_imageSweepSamplingGeneration == keys.samplingGeneration &&
                g_imageSweepBackendContextGeneration == g_backendContextGeneration) {
                return;
            }
            SyncImageTextureBindings();
            g_imageSweepContextId = keys.contextId;
            g_imageSweepSamplingGeneration = keys.samplingGeneration;
            g_imageSweepBackendContextGeneration = g_backendContextGeneration;
            g_imageSweepValid = true;
        }
    } // namespace TextureImpl

    namespace FramebufferImpl {
        // Record that `target` now reflects this exact (binding, object, revision) triple.
        // Every one of the three has to move together: stamping a subset leaves an early-out
        // that either never fires or fires on state it never actually pushed.
        static void StampSyncedFBO(FramebufferTarget target, Uint16 slotVersion, Uint16 objectVersion,
                                   MG_State::GLState::FramebufferObject* fbo) {
            g_fboSyncedSlotVersions[SizeT(target)] = slotVersion;
            g_fboSyncedObjectVersions[SizeT(target)] = objectVersion;
            g_fboSyncedObjects[SizeT(target)] = fbo;
            g_fboSyncedBackendIdGenerations[SizeT(target)] = g_attachmentBackendIdGeneration;
        }

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
                auto& slot = GetFramebufferBindingSlotFast(target);
                auto& currentFBO = slot.GetBoundObject();

                // The three memos together say "this target is already synced": which object is
                // bound (pointer), that it is still the same binding and not a recycled address
                // (slot version, bumped by every real rebind), and that the object has not been
                // edited since (object version, bumped by every attachment/draw-buffer/read-buffer
                // change). All three are stamped by every path below that leaves the target synced
                // - including the ones that decide there is nothing to do. Leaving the slot version
                // to ForceBindCurrentFBO alone made it a permanent mismatch for any app that never
                // hits a blit or a DSA clear, so this early-out never fired and a Minecraft-shaped
                // frame re-ran the whole attachment walk on all 5495 draws.
                const Uint16 slotVersion = slot.GetVersion();
                const Uint16 objectVersion = currentFBO ? currentFBO->GetObjectVersion() : 0;
                auto* currentPtr = currentFBO.get();
                // The backend-id generation joins the triple: a backend texture re-mint
                // (RecreateBackendTexture) moves no frontend version, so without it the
                // early-out would keep the driver FBO on the deleted texture name.
                if (slotVersion == g_fboSyncedSlotVersions[SizeT(target)] &&
                    objectVersion == g_fboSyncedObjectVersions[SizeT(target)] &&
                    currentPtr == g_fboSyncedObjects[SizeT(target)] &&
                    g_fboSyncedBackendIdGenerations[SizeT(target)] == g_attachmentBackendIdGeneration) {
                    lastUpdatedFBO = currentPtr;
                    continue;
                }

                if (!currentFBO) {
                    MGLOG_E_ONCE("No FBO is currently bound, cannot sync current FBO.");
                    continue;
                }

                if (currentFBO == MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo->defaultFBO) {
                    // Default FBO, nothing to sync - except the widened-attachment mask, which is
                    // only ever WRITTEN by SyncToBackend and would otherwise still describe the
                    // user FBO that was draw-bound before. The window surface is a real RGBA
                    // buffer, so nothing here is ever widened.
                    if (target == FramebufferTarget::Draw) {
                        g_alphaWidenedDrawBufferMask = 0;
                        g_integerColorDrawBufferMask = 0;
                    }
                    StampSyncedFBO(target, slotVersion, objectVersion, currentPtr);
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
                        auto* syncedFBOSlot = g_backendFramebufferObjects.Find(currentFBO.get());
                        if (syncedFBOSlot && *syncedFBOSlot) {
                            (*syncedFBOSlot)->SyncReadBufferToBackend(currentFBO);
                        }
                    }
                    StampSyncedFBO(target, slotVersion, objectVersion, currentPtr);
                    continue;
                }

                auto* backendFBOSlot = g_backendFramebufferObjects.Find(currentFBO.get());
                auto& backendObj =
                    backendFBOSlot ? *backendFBOSlot : g_backendFramebufferObjects.GetOrCreate(currentFBO);
                if (!backendObj) {
                    backendObj = MakeShared<BackendFramebufferObject>();
                }
                backendObj->SyncToBackend(currentFBO, target);

                StampSyncedFBO(target, slotVersion, objectVersion, currentPtr);
                lastUpdatedFBO = currentFBO.get();
            }
        }
    } // namespace FramebufferImpl

    namespace RenderStateImpl {
        static Uint16 g_syncedRenderStateVersion = 0;
        static Bool g_hasSyncedRenderState = false;
        static RenderStateParameters g_syncedRenderStateParameters;
        static IntVec4 g_syncedBackendViewport = IntVec4(-1, -1, -1, -1);
        // The RESOLVED scissor rectangle last pushed (see the scissor block in SyncRenderState);
        // an impossible value so the first sync always pushes.
        static IntVec4 g_syncedBackendScissorBox = IntVec4(-1, -1, -1, -1);
        // GLES starts with sRGB framebuffer encoding on, so the first sync always has to push the
        // frontend's (desktop-GL default) disabled state down.
        static Bool g_syncedSrgbFramebufferWrites = true;
        // Set when the shadow below stops describing the real ES context. The ES context
        // OUTLIVES every MobileGL context, so a MobileGL context switch leaves it holding the
        // previous context's enable state while the frontend's parameter block AND its
        // version counter both restart from defaults. Every "differs from what I last pushed"
        // test in SyncRenderState would then agree that nothing needs pushing, and the
        // leftover state silently applies to the new context - the class of bug the CTS
        // caught as GL_FRAMEBUFFER_SRGB surviving from vertex_attrib_binding into
        // direct_state_access.renderbuffers_storage. One unconditional push settles the whole
        // block rather than the one cap that happened to be noticed.
        static Bool g_forceFullRenderStateResync = true;
        // Which draw buffers' alpha channel the colour mask last pushed to the driver had forced
        // OFF - i.e. the value of `appliedWidenMask` in the last SyncRenderState that reached the
        // colour-mask block. NOT derivable from the frontend parameter block: it depends on the
        // bound DRAW framebuffer's attachment formats and on whether the caller is a draw or a
        // clear, neither of which bumps the frontend render-state version. Without it a
        // clear-then-draw pair on an unchanged parameter block early-outs and the draw inherits
        // the clear's undoctored mask.
        static Uint32 g_syncedColorMaskAlphaWidenMask = 0;
        void InvalidateSyncedRenderState() {
            g_forceFullRenderStateResync = true;
            g_hasSyncedRenderState = false;
            g_syncedBackendViewport = IntVec4(-1, -1, -1, -1);
            g_syncedBackendScissorBox = IntVec4(-1, -1, -1, -1);
        }
        void SyncRenderState(Bool forColorClear) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            Uint16 currentRenderStateVersion = MG_State::pGLContext->GetRenderStateParametersVersion();
            const Bool forceFullPush = g_forceFullRenderStateResync;
            g_forceFullRenderStateResync = false;
            // The alpha discipline for widened colour attachments (see the header comment on
            // SyncRenderState): a DRAW must not be able to move the stored alpha off 1.0, a CLEAR
            // is what puts it there. So the draw path masks alpha off on every widened draw
            // buffer and the clear path masks nothing.
            const Uint32 appliedWidenMask = forColorClear ? 0u : FramebufferImpl::g_alphaWidenedDrawBufferMask;
            const Bool colorMaskWidenDirty = appliedWidenMask != g_syncedColorMaskAlphaWidenMask;
            if (!forceFullPush && !colorMaskWidenDirty && g_hasSyncedRenderState &&
                currentRenderStateVersion == g_syncedRenderStateVersion) {
                return;
            }

            const auto& parameters = MG_State::pGLContext->GetRenderStateParameters();

            // The frontend has ONE version for the whole parameter block, so a per-draw blend
            // toggle used to re-diff all ~40 pieces of state field by field on every draw
            // (Blaze3D brackets every batch with glEnable/glDisable(GL_BLEND), making this the
            // hottest thing mc_state_toggle did). Split the struct into three contiguous byte
            // spans - the head (viewport/point/line/polygon-offset scalars), the blend array,
            // and everything after it - and let one memcmp per span decide whether its blocks
            // run at all. memcmp can false-DIFFER on padding bytes (harmless: the field-wise
            // block runs and finds nothing) but can never false-match, and after the first full
            // sync the tail memcpy below makes the shadow byte-identical, padding included, so
            // in the steady state a span memcmp is exact. Blocks whose inputs are NOT in the
            // parameter struct (the surface-size viewport fallback, the sRGB context
            // capability) stay outside the gates.
            static_assert(std::is_trivially_copyable_v<RenderStateParameters>,
                          "span memcmp/memcpy below treats the parameter block as raw bytes");
            constexpr SizeT kBlendSpanBegin = offsetof(RenderStateParameters, BlendStates);
            constexpr SizeT kBlendSpanEnd = offsetof(RenderStateParameters, LogicOp);
            const auto* currentBytes = reinterpret_cast<const unsigned char*>(&parameters);
            const auto* syncedBytes = reinterpret_cast<const unsigned char*>(&g_syncedRenderStateParameters);
            const Bool headSpanDirty =
                !g_hasSyncedRenderState || std::memcmp(currentBytes, syncedBytes, kBlendSpanBegin) != 0;
            const Bool blendSpanDirty =
                !g_hasSyncedRenderState || std::memcmp(currentBytes + kBlendSpanBegin, syncedBytes + kBlendSpanBegin,
                                                       kBlendSpanEnd - kBlendSpanBegin) != 0;
            const Bool tailSpanDirty =
                !g_hasSyncedRenderState || std::memcmp(currentBytes + kBlendSpanEnd, syncedBytes + kBlendSpanEnd,
                                                       sizeof(RenderStateParameters) - kBlendSpanEnd) != 0;

            IntVec4 backendViewport = MG_State::pGLContext->GetViewport();
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

            // Every capability bool (and the scissor-test mask below) lives after LogicOp in the
            // struct, i.e. in the tail span.
            if (tailSpanDirty) {
#define SYNC_CAPABILITY(cap_mg, cap_gl)                                                                                \
    if (forceFullPush || parameters.cap_mg##Enabled != g_syncedRenderStateParameters.cap_mg##Enabled) {                                 \
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
                SYNC_CAPABILITY(StencilTest, GL_STENCIL_TEST);
                SYNC_CAPABILITY(CullFace, GL_CULL_FACE);

#undef SYNC_CAPABILITY

                // GL_SCISSOR_TEST is per-viewport enable state (ARB_viewport_array), so it is a
                // 16-bit mask and not a "<Name>Enabled" bool the macro above could key off. ES
                // has exactly one scissor rectangle and one scissor enable, so only bit 0 - the
                // index every ES draw rasterizes against - can be forwarded; a program that
                // enables the test for viewport 3 alone gets viewport 0's answer here. That is
                // the same limitation as the unemulated gl_ViewportIndex on this backend and is
                // why the multi-viewport half of KHR-GL43.viewport_array stays red on Espryt.
                {
                    const Bool scissorTest = (parameters.ScissorTestEnabledMask & 1u) != 0;
                    const Bool syncedScissorTest =
                        (g_syncedRenderStateParameters.ScissorTestEnabledMask & 1u) != 0;
                    if (forceFullPush || scissorTest != syncedScissorTest) {
                        scissorTest ? g_GLESFuncs.glEnable(GL_SCISSOR_TEST) : g_GLESFuncs.glDisable(GL_SCISSOR_TEST);
                    }
                }
            }

            if (tailSpanDirty && g_GLESCapabilities.SupportsClipDistance) {
                // gl_ClipDistance clipping is per-distance enable state in GL, and ES reaches it
                // only through GL_EXT_clip_cull_distance. That extension reuses the desktop enum
                // values for CLIP_DISTANCE0_EXT..7_EXT, but the token is absent from the ES
                // headers this file compiles against, hence the local name. Without the
                // extension there is nowhere to put the state and the shader could not have
                // compiled either, so the whole block is gated rather than silently no-op'ing.
                constexpr GLenum kClipDistance0 = 0x3000;
                constexpr Uint kClipDistanceCount = 8;
                const Uint32 mask = parameters.ClipDistanceEnabledMask;
                const Uint32 syncedMask = g_syncedRenderStateParameters.ClipDistanceEnabledMask;
                if (forceFullPush || mask != syncedMask) {
                    const Uint32 changed = forceFullPush ? ~0u : (mask ^ syncedMask);
                    for (Uint i = 0; i < kClipDistanceCount; ++i) {
                        const Uint32 bit = 1u << i;
                        if ((changed & bit) == 0) continue;
                        const GLenum cap = static_cast<GLenum>(kClipDistance0 + i);
                        (mask & bit) ? g_GLESFuncs.glEnable(cap) : g_GLESFuncs.glDisable(cap);
                    }
                }
            }

            { // sRGB framebuffer writes. GLES core always encodes a write into an sRGB attachment,
              // while GL_FRAMEBUFFER_SRGB is disabled by default in desktop GL and the frontend
              // never turns it on, so the driver has to be told to write raw. Without this a render
              // into an sRGB colour buffer comes back encoded once too often (the shader's own
              // decode on the next fetch then leaves the value one conversion short).
                const Bool srgbWrites = MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::FramebufferSrgb);
                if (g_GLESCapabilities.SupportsSrgbWriteControl &&
                    (forceFullPush || srgbWrites != g_syncedSrgbFramebufferWrites)) {
                    srgbWrites ? g_GLESFuncs.glEnable(GL_FRAMEBUFFER_SRGB)
                               : g_GLESFuncs.glDisable(GL_FRAMEBUFFER_SRGB);
                    g_syncedSrgbFramebufferWrites = srgbWrites;
                }
            }

            if (tailSpanDirty) { // Primitive restart. GLES core has only GL_PRIMITIVE_RESTART_FIXED_INDEX (fixed
              // all-ones value); both the fixed cap and the (fixed-valued) arbitrary GL_PRIMITIVE_RESTART map to
              // it. An arbitrary non-fixed restart index is rejected at draw time (see DrawElements).
                const Bool restart = parameters.PrimitiveRestartFixedIndexEnabled || parameters.PrimitiveRestartEnabled;
                const Bool syncedRestart = g_syncedRenderStateParameters.PrimitiveRestartFixedIndexEnabled ||
                                           g_syncedRenderStateParameters.PrimitiveRestartEnabled;
                if (forceFullPush || restart != syncedRestart) {
                    restart ? g_GLESFuncs.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX)
                            : g_GLESFuncs.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
                }
            }

            const auto& ToGLBoolean = [](Bool b) -> GLboolean { return b ? GL_TRUE : GL_FALSE; };

            if (blendSpanDirty) { // Blend State
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
                Bool anyCapDirty = forceFullPush;

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
                            if (forceFullPush || targetStates[i].Enabled != syncedStates[i].Enabled) {
                                syncedStates[i].Enabled = targetStates[i].Enabled;
                                syncedStates[i].Enabled ? g_GLESFuncs.glEnablei(GL_BLEND, i)
                                                        : g_GLESFuncs.glDisablei(GL_BLEND, i);
                            }
                        }
                    }
                }

                Bool allFuncsSame = true;
                Bool anyFuncDirty = forceFullPush;
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

                            if (forceFullPush || cur.SrcFactorRGB != syn.SrcFactorRGB ||
                                cur.DstFactorRGB != syn.DstFactorRGB ||
                                cur.SrcFactorAlpha != syn.SrcFactorAlpha ||
                                cur.DstFactorAlpha != syn.DstFactorAlpha) {
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
                Bool anyEquationDirty = forceFullPush;

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

                            if (forceFullPush || cur.ColorEquation != syn.ColorEquation ||
                                cur.AlphaEquation != syn.AlphaEquation) {
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

            if (tailSpanDirty) { // Depth state
                if (forceFullPush || parameters.DepthFunc != g_syncedRenderStateParameters.DepthFunc) {
                    g_GLESFuncs.glDepthFunc(MG_Util::ConvertDepthTestFuncToGLEnum(parameters.DepthFunc));
                }
                if (forceFullPush || parameters.DepthMask != g_syncedRenderStateParameters.DepthMask) {
                    g_GLESFuncs.glDepthMask(parameters.DepthMask ? GL_TRUE : GL_FALSE);
                }
                if (forceFullPush || parameters.DepthRanges[0] != g_syncedRenderStateParameters.DepthRanges[0]) {
                    g_GLESFuncs.glDepthRangef(parameters.DepthRanges[0].x(), parameters.DepthRanges[0].y());
                }
            }

            if (tailSpanDirty) { // Stencil state
                for (SizeT faceIndex = 0; faceIndex < parameters.StencilStates.size(); ++faceIndex) {
                    const StencilFaceState& current = parameters.StencilStates[faceIndex];
                    const StencilFaceState& synced = g_syncedRenderStateParameters.StencilStates[faceIndex];
                    const GLenum glFace = faceIndex == 0 ? GL_FRONT : GL_BACK;

                    if (forceFullPush || current.Func != synced.Func || current.Ref != synced.Ref ||
                        current.ValueMask != synced.ValueMask) {
                        g_GLESFuncs.glStencilFuncSeparate(
                            glFace, MG_Util::ConvertDepthTestFuncToGLEnum(current.Func), current.Ref,
                            current.ValueMask);
                    }
                    if (forceFullPush || current.WriteMask != synced.WriteMask) {
                        g_GLESFuncs.glStencilMaskSeparate(glFace, current.WriteMask);
                    }
                    if (forceFullPush || current.FailOp != synced.FailOp ||
                        current.PassDepthFailOp != synced.PassDepthFailOp ||
                        current.PassDepthPassOp != synced.PassDepthPassOp) {
                        g_GLESFuncs.glStencilOpSeparate(
                            glFace, MG_Util::ConvertStencilOperationToGLEnum(current.FailOp),
                            MG_Util::ConvertStencilOperationToGLEnum(current.PassDepthFailOp),
                            MG_Util::ConvertStencilOperationToGLEnum(current.PassDepthPassOp));
                    }
                }
            }

            if (tailSpanDirty || colorMaskWidenDirty) { // Color mask. Uniform masks use the non-indexed glColorMask
              // (works everywhere); divergent per-draw-buffer masks use the indexed glColorMaski when
              // draw_buffers_indexed is available, otherwise fall back to broadcasting draw buffer 0.
              // Mirrors the blend block.
                using FBO = MG_State::GLState::FramebufferObject;
                const auto& targetMasks = parameters.ColorMasks;
                const auto& syncedMasks = g_syncedRenderStateParameters.ColorMasks;

                // What the DRIVER is told for draw buffer i. Identical to the application's mask
                // except on a widened attachment during a draw, where alpha is forced off; the
                // frontend's own array is never written, so glGet(GL_COLOR_WRITEMASK) keeps
                // answering with the application's value.
                const auto driverMask = [&](Uint i) -> BoolVec4 {
                    BoolVec4 m = targetMasks[i];
                    if (i < 32 && (appliedWidenMask & (1u << i)) != 0) {
                        m.w() = false;
                    }
                    return m;
                };

                Bool anyDirty = forceFullPush || colorMaskWidenDirty;
                Bool allSame = true;
                const BoolVec4 driverMask0 = driverMask(0);
                for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                    if (targetMasks[i] != syncedMasks[i]) anyDirty = true;
                    if (i > 0 && driverMask(i) != driverMask0) allSame = false;
                }

                if (anyDirty) {
                    if (allSame || !g_GLESCapabilities.SupportsIndexedColorMask) {
                        // Without draw_buffers_indexed there is only one mask for the whole
                        // framebuffer, so a widened draw buffer 0 costs every other buffer its
                        // alpha writes. ES 3.2 makes glColorMaski core and ES 3.1 has it as
                        // EXT/OES; the only devices that reach this line are ES 3.0-class, where
                        // MRT with a mixed widened/native colour attachment set is already rare.
                        const BoolVec4& m = driverMask0;
                        g_GLESFuncs.glColorMask(ToGLBoolean(m.x()), ToGLBoolean(m.y()), ToGLBoolean(m.z()),
                                                ToGLBoolean(m.w()));
                    } else {
                        const auto colorMaskiFn = g_GLESFuncs.glColorMaski      ? g_GLESFuncs.glColorMaski
                                                  : g_GLESFuncs.glColorMaskiEXT ? g_GLESFuncs.glColorMaskiEXT
                                                                                : g_GLESFuncs.glColorMaskiOES;
                        for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS; ++i) {
                            // colorMaskWidenDirty forces every slot: the previous push may have
                            // been the non-indexed glColorMask above (which set all of them), and
                            // the per-slot diff below only knows about the application's array.
                            if (forceFullPush || colorMaskWidenDirty || targetMasks[i] != syncedMasks[i]) {
                                const BoolVec4 m = driverMask(i);
                                colorMaskiFn(i, ToGLBoolean(m.x()), ToGLBoolean(m.y()), ToGLBoolean(m.z()),
                                             ToGLBoolean(m.w()));
                            }
                        }
                    }
                }
                g_syncedColorMaskAlphaWidenMask = appliedWidenMask;
            }

            if (tailSpanDirty) { // Polygon mode. GLES core has no glPolygonMode; use NV/ANGLE_polygon_mode when present.
              // Without the extension the mode stays FILL and non-FILL requests are dropped.
                if ((forceFullPush || parameters.PolygonModeFront != g_syncedRenderStateParameters.PolygonModeFront) &&
                    g_GLESCapabilities.SupportsPolygonMode) {
                    const auto polygonModeFn =
                        g_GLESFuncs.glPolygonModeNV ? g_GLESFuncs.glPolygonModeNV : g_GLESFuncs.glPolygonModeANGLE;
                    polygonModeFn(GL_FRONT_AND_BACK, parameters.PolygonModeFront);
                }
            }

            if (tailSpanDirty) { // Clear values
                if (forceFullPush || parameters.ClearColor != g_syncedRenderStateParameters.ClearColor) {
                    const FloatVec4& clearCol = parameters.ClearColor;
                    g_GLESFuncs.glClearColor(clearCol.x(), clearCol.y(), clearCol.z(), clearCol.w());
                }
                if (forceFullPush || parameters.ClearDepth != g_syncedRenderStateParameters.ClearDepth) {
                    g_GLESFuncs.glClearDepthf(parameters.ClearDepth);
                }
                if (forceFullPush || parameters.ClearStencil != g_syncedRenderStateParameters.ClearStencil) {
                    g_GLESFuncs.glClearStencil(static_cast<GLint>(parameters.ClearStencil));
                }
                if (forceFullPush || parameters.BlendColor != g_syncedRenderStateParameters.BlendColor) {
                    const FloatVec4& blendColor = parameters.BlendColor;
                    g_GLESFuncs.glBlendColor(blendColor.x(), blendColor.y(), blendColor.z(), blendColor.w());
                }
            }

            if (tailSpanDirty) { // Cull face mode
                if (forceFullPush || parameters.CullFaceModeSetting != g_syncedRenderStateParameters.CullFaceModeSetting) {
                    const CullFaceMode& cfm = parameters.CullFaceModeSetting;
                    g_GLESFuncs.glCullFace(MG_Util::ConvertCullFaceModeToGLEnum(cfm));
                }
                if (forceFullPush || parameters.FrontFaceModeSetting != g_syncedRenderStateParameters.FrontFaceModeSetting) {
                    const FrontFaceMode& ffm = parameters.FrontFaceModeSetting;
                    g_GLESFuncs.glFrontFace(MG_Util::ConvertFrontFaceModeToGLEnum(ffm));
                }
            }

            if (tailSpanDirty) { // Scissor box. Resolved and shadowed like the viewport above, and
              // for the same reason: what has to reach the driver is NOT simply the parameter
              // field. RenderStateParameters::ScissorBoxes starts all-zero, which means "the
              // application has never called glScissor" - it is not a GL scissor box. GL's
              // initial box is the whole window, which the frontend has no way to spell before a
              // surface exists. The pre-resync code got away with pushing the field verbatim only
              // by accident: the shadow held the same default, the field never compared unequal,
              // and the ES context kept its own correct default. Under the forced full push that
              // accident is gone, glScissor(0,0,0,0) shrinks the scissor to an EMPTY rectangle,
              // and everything drawn with GL_SCISSOR_TEST enabled before the app's first
              // glScissor is clipped away - Minecraft 26.2 keeps only its unscissored sky and
              // hand and loses the terrain and the whole GUI.
              //
              // The condition is the WRITTEN FLAG, not the extent. An empty rectangle is a
              // perfectly legal thing to ask for - glScissor(0,0,0,0) means "the scissor test
              // rejects every fragment" - so testing `width <= 0 || height <= 0` substituted the
              // whole surface for a deliberately empty box and inverted the request into "accept
              // every fragment", no matter how many times the application had already called
              // glScissor. KHR-GL43.viewport_array.scissor_zero_dimension is exactly that: all 16
              // boxes zero-sized with the test enabled, requiring the draw to be clipped away
              // entirely. Reading the flag preserves the Minecraft protection bit-for-bit - before
              // the first glScissor the bit is clear and the surface size is still substituted -
              // while an explicit empty box now reaches the driver verbatim. Negative extents
              // cannot arrive here at all: all three entry points reject them with
              // GL_INVALID_VALUE before storing (GL_RenderState.cpp's ValidateNonNegativeExtent).
                IntVec4 backendScissorBox = parameters.ScissorBoxes[0];
                if ((parameters.ScissorBoxWrittenMask & 1u) == 0) {
                    Int surfaceWidth = 0;
                    Int surfaceHeight = 0;
                    if (QueryCurrentSurfaceSize(surfaceWidth, surfaceHeight)) {
                        backendScissorBox = IntVec4(0, 0, surfaceWidth, surfaceHeight);
                    }
                }
                // Compared against what was actually PUSHED, not against the parameter field, so
                // the resolved value and the diff can never disagree.
                if (backendScissorBox != g_syncedBackendScissorBox) {
                    g_GLESFuncs.glScissor(backendScissorBox.x(), backendScissorBox.y(), backendScissorBox.z(),
                                          backendScissorBox.w());
                    g_syncedBackendScissorBox = backendScissorBox;
                }
            }

            if (tailSpanDirty) { // Logic op (first field of the tail span)
              // glLogicOp is GLES 1.x / EXT only - eglGetProcAddress returns null for it on a
              // plain ES 3.x driver. Before the forced resync it was reached only when an app
              // actually set a logic op; now every MakeCurrent would call it, so the null check
              // is mandatory rather than defensive.
                if (g_GLESFuncs.glLogicOp &&
                    (forceFullPush || parameters.LogicOp != g_syncedRenderStateParameters.LogicOp)) {
                    g_GLESFuncs.glLogicOp(MG_Util::ConvertLogicOperationToGLEnum(parameters.LogicOp));
                }
            }

            if (headSpanDirty) { // Polygon offset (head-span scalars, like line width / point size below)
                if (forceFullPush || parameters.PolygonOffsetFactor != g_syncedRenderStateParameters.PolygonOffsetFactor ||
                    parameters.PolygonOffsetUnits != g_syncedRenderStateParameters.PolygonOffsetUnits) {
                    g_GLESFuncs.glPolygonOffset(parameters.PolygonOffsetFactor, parameters.PolygonOffsetUnits);
                }
            }

            if (headSpanDirty) { // Line width
                if (forceFullPush || parameters.LineWidth != g_syncedRenderStateParameters.LineWidth) {
                    g_GLESFuncs.glLineWidth(parameters.LineWidth);
                }
            }

            if (headSpanDirty) { // Point size (GLES 1.x only - ES 2+ sets it from gl_PointSize,
              // so the entry point is absent on most drivers; see the glLogicOp note above)
                if (g_GLESFuncs.glPointSize &&
                    (forceFullPush || parameters.PointSize != g_syncedRenderStateParameters.PointSize)) {
                    g_GLESFuncs.glPointSize(parameters.PointSize);
                }
            }

            if (tailSpanDirty) { // Sample coverage
                if (forceFullPush || parameters.SampleCoverageValue != g_syncedRenderStateParameters.SampleCoverageValue ||
                    parameters.SampleCoverageInvert != g_syncedRenderStateParameters.SampleCoverageInvert) {
                    g_GLESFuncs.glSampleCoverage(parameters.SampleCoverageValue,
                                                ToGLBoolean(parameters.SampleCoverageInvert));
                }
            }

            if (tailSpanDirty) { // Sample mask
                if (g_GLESFuncs.glSampleMaski &&
                    (forceFullPush || parameters.SampleMaskValue != g_syncedRenderStateParameters.SampleMaskValue)) {
                    g_GLESFuncs.glSampleMaski(0, parameters.SampleMaskValue);
                }
            }

            g_syncedRenderStateVersion = currentRenderStateVersion;
            // Byte copy, not member copy: it also clones the frontend struct's padding bytes,
            // which is what lets the span memcmps above answer "unchanged" exactly instead of
            // tripping on indeterminate padding every draw. Only the dirty spans need copying -
            // a clean span's bytes are already identical by the very memcmp that skipped it.
            auto* syncedBytesMut = reinterpret_cast<unsigned char*>(&g_syncedRenderStateParameters);
            if (headSpanDirty) {
                std::memcpy(syncedBytesMut, currentBytes, kBlendSpanBegin);
            }
            if (blendSpanDirty) {
                std::memcpy(syncedBytesMut + kBlendSpanBegin, currentBytes + kBlendSpanBegin,
                            kBlendSpanEnd - kBlendSpanBegin);
            }
            if (tailSpanDirty) {
                std::memcpy(syncedBytesMut + kBlendSpanEnd, currentBytes + kBlendSpanEnd,
                            sizeof(RenderStateParameters) - kBlendSpanEnd);
            }
            g_hasSyncedRenderState = true;
        }
    } // namespace RenderStateImpl

    namespace PrgramImpl {
        // The twin SyncCurrentProgram resolved for the draw/dispatch being prepared,
        // consumed by BindCurrentProgramWithResources and the post-draw draw-parameter
        // updates in the same GL entry point — they all used to repeat the registry
        // Find. Valid only while the keyed frontend program is the current draw
        // program: every consumer compares the raw key against GetProgramForDraw()
        // before trusting the twin, and SyncCurrentProgram rewrites the pair at the
        // top of every PrepareForDraw/PrepareForCompute, so a recycled address can
        // never be consumed (the stale pair is overwritten before any consumer runs).
        static const MG_State::GLState::ProgramObject* g_currentDrawFrontendProgram = nullptr;
        static BackendProgramObjectImpl* g_currentDrawBackendProgram = nullptr;

        // Memo of the per-draw enabled-draw-buffers walk feeding g_fragColorBroadcastCount:
        // the answer is a pure function of WHICH FBO is bound and its draw-buffer edits, so
        // it is keyed exactly like SyncCurrentFBO's synced trio - object pointer (identity),
        // slot version (a recycled heap address cannot re-match: every real rebind bumps
        // it), and object version (every attachment/draw-buffer/read-buffer edit bumps it,
        // the same documented invariant the FBO sync memo already leans on). A null FBO
        // keys on (nullptr, slot version, 0).
        static const MG_State::GLState::FramebufferObject* g_broadcastMemoFbo = nullptr;
        static Uint16 g_broadcastMemoSlotVersion = 0;
        static Uint16 g_broadcastMemoObjectVersion = 0;
        static Bool g_broadcastMemoValid = false;
        static Uint g_broadcastMemoCount = 1;

        // The identity+version key above is only monotonic WITHIN one GLContext: a
        // library teardown + re-init frees every FramebufferObject and restarts the
        // draw slot's counter at zero, so a recycled FBO address with coinciding
        // fresh versions would false-hit. Cleared at the same boundaries as the
        // structurally identical SyncCurrentFBO trio (InvalidateFramebufferBindingCache).
        void InvalidateBroadcastMemo() {
            g_broadcastMemoValid = false;
        }

        void SyncCurrentProgram(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_backendProgramObjects.CollectGarbageIfNeeded();
            SamplerImpl::g_backendSamplerObjects.CollectGarbageIfNeeded();

            g_currentDrawFrontendProgram = nullptr;
            g_currentDrawBackendProgram = nullptr;

            // ... || !GetSpirvStatus(): see BackendProgramObjectImpl::SyncToBackend - a
            // program whose SPIR-V never arrived is linked but not drawable.
            if (!currentProgram || !currentProgram->GetLinkStatus() || !currentProgram->GetSpirvStatus()) {
                g_GLESFuncs.glUseProgram(0);
                g_lastUsedBackendProgramId = 0;
                return;
            }
            // Read from the frontend rather than from the backend framebuffer sync, which
            // only runs later in PrepareForDraw: a program compiled against a stale count
            // would not be relinked until the draw after the one that needed it.
            {
                const auto& drawSlot = GetFramebufferBindingSlotFast(FramebufferTarget::Draw);
                const auto& drawFBO = drawSlot.GetBoundObject();
                const Uint16 slotVersion = drawSlot.GetVersion();
                const Uint16 objectVersion = drawFBO ? drawFBO->GetObjectVersion() : 0;
                if (!g_broadcastMemoValid || g_broadcastMemoFbo != drawFBO.get() ||
                    g_broadcastMemoSlotVersion != slotVersion || g_broadcastMemoObjectVersion != objectVersion) {
                    Uint enabledDrawBuffers = 0;
                    if (drawFBO) {
                        const auto& drawBuffers = drawFBO->GetDrawBuffers();
                        for (Uint i = 0; i < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
                            if (drawBuffers[i] != FramebufferAttachmentType::None) {
                                enabledDrawBuffers = i + 1;
                            }
                        }
                    }
                    g_broadcastMemoFbo = drawFBO.get();
                    g_broadcastMemoSlotVersion = slotVersion;
                    g_broadcastMemoObjectVersion = objectVersion;
                    g_broadcastMemoCount = std::max<Uint>(enabledDrawBuffers, 1);
                    g_broadcastMemoValid = true;
                }
                g_fragColorBroadcastCount = g_broadcastMemoCount;
            }

            BackendProgramObjectImpl* twin = g_programTwinLookupMemo.Lookup(currentProgram);
            if (!twin) {
                auto* backendProgramSlot = g_backendProgramObjects.Find(currentProgram.get());
                auto& backendObj =
                    backendProgramSlot ? *backendProgramSlot : g_backendProgramObjects.GetOrCreate(currentProgram);
                if (!backendObj) {
                    backendObj = MakeShared<BackendProgramObjectImpl>();
                }
                g_programTwinLookupMemo.Store(currentProgram, backendObj.get());
                twin = backendObj.get();
            }
            // A link-version mismatch means the program was relinked: the backend
            // shaders and every cache built by CacheResourceLocations (block
            // indices, sampler locations, UBO upload gate) are stale.
            //
            // The storage-block signature is the same shape of condition: ES cannot move a
            // storage block's binding after link, so glShaderStorageBlockBinding is honoured by
            // baking the effective binding into the generated ESSL - which makes a program built
            // against a different override set stale. It is compared HERE rather than acted on in
            // the entry point because that one must never trigger a build (see
            // ShaderStorageBlockBinding below). The signature is over the values, so an
            // application that re-sets the same bindings every frame rebuilds nothing.
            //
            // The image-unit generation is a third of the same shape, and it used to be
            // carried by accident: glUniform1i on an image uniform bumped the program's backend
            // state version, which was in the program-pipeline composite's cache key, so a
            // pipeline draw got a whole NEW composite object and therefore a fresh twin. Keying
            // that cache on the link version instead (ProgramPipelineObject) removed the
            // accident - and it never covered the monolithic glUseProgram path at all - so the
            // dependency is stated here instead.
            if (!twin->GetBackendProgramId() ||
                twin->GetSyncedLinkVersion() != currentProgram->GetLinkVersion() ||
                twin->GetSyncedImageUnitVersion() != currentProgram->GetImageUnitVersion() ||
                twin->GetSnormFallbackClampOutputMask() != g_snormFallbackClampOutputMask ||
                twin->GetUnormFallbackClampOutputMask() != g_unormFallbackClampOutputMask ||
                twin->GetFragColorBroadcastCount() != g_fragColorBroadcastCount ||
                twin->GetShaderStorageBlockBindingSignature() !=
                    ComputeShaderStorageBlockBindingSignature(*currentProgram) ||
                // A fourth of the same shape, and the reason glBindImageTexture itself does
                // nothing: GLSL ES demands a format layout qualifier on an image where desktop
                // GLSL lets a writeonly declaration omit one, so a format-less declaration is
                // compiled against the format the application BOUND, and a rebind to a
                // different one makes what was built wrong. Asked of the twin because only it
                // knows which units its own images address - and answered by an empty-vector
                // test for every program that declares its formats, which is nearly all of them.
                !twin->ImageUnitFormatsStillMatch() ||
                // A fifth of the same shape, for the programs ES will not link at all: one whose
                // tessellation evaluation stage has no control stage gets a synthesized
                // pass-through one, and GL_PATCH_VERTICES is compiled INTO it as
                // `layout(vertices = N) out` - so a glPatchParameteri between two draws makes the
                // built program wrong. -1 is "this program needed no such stage", which compares
                // equal to itself and costs every other program one integer test.
                (twin->GetPassthroughTessControlPatchVertices() >= 0 &&
                 twin->GetPassthroughTessControlPatchVertices() !=
                     static_cast<Int>(MG_State::pGLContext->GetPatchVertices()))) {
                twin->SyncToBackend(currentProgram);
            }
            g_currentDrawFrontendProgram = currentProgram.get();
            g_currentDrawBackendProgram = twin;
        }
    } // namespace PrgramImpl

    void BindCurrentFBO(FramebufferTarget target) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        auto& slot = GetFramebufferBindingSlotFast(target);
        // No fast path on the binding slot's version. It is a 16-bit counter that only
        // ForceBindCurrentFBO ever stamps here, so the comparison was against an arbitrarily old
        // snapshot and any later slot version that happened to land on it - one wrap of the
        // counter, or simply enough rebinds - read as "already bound" and left the driver on a
        // completely different framebuffer. KHR-GL32.packed_pixels then read its gradient back
        // out of the previous subtest's framebuffer.
        //
        // Skipping the work is BindFramebufferId's job anyway: it shadows the driver's own
        // draw/read bindings and drops the glBindFramebuffer when the target already holds the id,
        // which is where the cost actually is. What is left here is one registry lookup,
        // and the twin memo replaces even that with an array probe on the steady path.
        const auto& currentFBO = slot.GetBoundObject();
        if (currentFBO && currentFBO != MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo->defaultFBO) {
            FramebufferImpl::BackendFramebufferObject* twin = g_fboTwinLookupMemo.Lookup(currentFBO);
            if (!twin) {
                auto* backendFBOSlot = FramebufferImpl::g_backendFramebufferObjects.Find(currentFBO.get());
                if (backendFBOSlot && *backendFBOSlot) {
                    twin = backendFBOSlot->get();
                    g_fboTwinLookupMemo.Store(currentFBO, twin);
                }
            }
            if (twin) {
                twin->Bind(target);
            } else {
                MGLOG_E_ONCE("No backend FBO found (maybe not synced) for current %s FBO, cannot bind FBO.",
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
            // Same reset as SyncCurrentFBO's default-framebuffer branch: SyncToBackend is the
            // only writer of the widened-attachment mask, so a path that skips it has to say so
            // explicitly. It matters here because the DSA clears and glBlitFramebuffer briefly
            // sync a DIFFERENT framebuffer as DRAW and then restore the application's through
            // ForceBindCurrentFBO - which lands right here when that one is the default.
            if (target == FramebufferTarget::Draw) {
                FramebufferImpl::g_alphaWidenedDrawBufferMask = 0;
                FramebufferImpl::g_integerColorDrawBufferMask = 0;
            }
            FramebufferImpl::BindFramebufferId(
                target == FramebufferTarget::Draw ? GL_DRAW_FRAMEBUFFER : GL_READ_FRAMEBUFFER, 0);
            return;
        }

        auto& registry = FramebufferImpl::g_backendFramebufferObjects;
        auto* backendFBOSlot = registry.Find(framebuffer.get());
        auto& backendObj = backendFBOSlot ? *backendFBOSlot : registry.GetOrCreate(framebuffer);
        if (!backendObj) {
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
        auto& slot = GetFramebufferBindingSlotFast(target);
        const auto& fbo = slot.GetBoundObject();
        SyncAndBindFramebufferObject(fbo, target);
        FramebufferImpl::g_fboSyncedSlotVersions[(SizeT)target] = slot.GetVersion();
        FramebufferImpl::g_fboSyncedObjectVersions[(SizeT)target] = fbo ? fbo->GetObjectVersion() : 0;
        FramebufferImpl::g_fboSyncedObjects[(SizeT)target] = fbo.get();
        FramebufferImpl::g_fboSyncedBackendIdGenerations[(SizeT)target] =
            FramebufferImpl::g_attachmentBackendIdGeneration;
    }

    static void BindCurrentProgramWithResources(
        const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
        const TextureImpl::DrawTextureSyncKeys& keys);
    static void BindCurrentTextures(const TextureImpl::DrawTextureSyncKeys& keys,
                                    const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram);

    void PrepareForDraw(DrawSyncFlags syncBit) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        // One twin resolve serves the whole draw: the buffer sync (which hosts the
        // resolved-buffers memo on the twin), the VAO sync and the draw-time bind
        // below. Nothing in between can invalidate it — the bound VAO is pinned by
        // the context, and no step here erases or replaces a live VAO's twin.
        const auto& currentVAO = MG_State::pGLContext->GetBoundVertexArray();
        VertexArrayImpl::BackendVertexArrayObject* vaoTwin =
            currentVAO ? VertexArrayImpl::ResolveVaoTwin(currentVAO) : nullptr;
        // Early config-version read: see the note on SyncNeccessaryBuffers - issuing
        // the (cold-line) load here overlaps its miss with the resolves below.
        const Uint32 vaoConfigVersion = currentVAO ? currentVAO->GetConfigVersion() : 0;
        // One program resolve and one texture-key capture serve the whole draw, for
        // the same reason the twin resolve does: only frontend GL entry points move
        // either, and none can run inside this preparation. GetProgramForDraw is a
        // cross-TU call with a guarded static inside - repeating it per stage showed
        // up in draw-loop profiles.
        const auto& currentProgram = MG_State::pGLContext->GetProgramForDraw();
        const TextureImpl::DrawTextureSyncKeys textureKeys = TextureImpl::CaptureDrawTextureSyncKeys();

        BufferImpl::SyncNeccessaryBuffers(currentVAO, vaoTwin, vaoConfigVersion,
                                          syncBit & DrawSyncBit::IndexBuffer,
                                          syncBit & DrawSyncBit::IndirectBuffer);
        VertexArrayImpl::SyncCurrentVAO(currentVAO, vaoTwin);
        TextureImpl::SyncNeccessaryTextures(textureKeys);
        // A draw reads and writes through its image units too, so the unit bindings have to be
        // as current as the sampled ones. Gated (see the sweep): a program with no image binding
        // pays one integer test, and one with images re-issues them only when a texture shape
        // moved under them.
        TextureImpl::SyncImageTextureBindingsForDraw(textureKeys);
        // A draw writes through its image units too - the conformance case that found this
        // stores into a buffer texture from the FRAGMENT stage, not from a dispatch.
        TextureImpl::MarkWritableImageBufferTexturesGpuWritten();
        FramebufferImpl::SyncCurrentFBO();
        PrgramImpl::SyncCurrentProgram(currentProgram);
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        {
#ifdef TRACY_ENABLE
            ZoneScopedNC("BindCurrentVAO", TRACY_ZONECOLOR_BACKEND);
#endif
            if (vaoTwin) {
                vaoTwin->Bind();
            } else {
                VertexArrayImpl::BindBackendVAOId(0);
            }
        }

        VertexArrayImpl::SyncCurrentVertexAttributeValues(vaoTwin, currentProgram);

        BindCurrentTextures(textureKeys, currentProgram);
        BindCurrentProgramWithResources(currentProgram, textureKeys);

        // Last: opening the capture span needs the program current and the capture
        // buffers bound, and ES rejects most binding changes once it is open.
        XfbImpl::StartPendingTransformFeedback();
    }

    // Resolves every frontend texture unit's textures onto the backend context. Returns
    // false when the resolution could not be completed from the state it read - a bound
    // texture that has no backend object yet is skipped, and a later draw would bind it
    // without any of the memo keys below moving - so the caller must not memoise it.
    static Bool ResolveAndBindUnitTextures(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
                                           Int maxTouchedUnit) {
#ifdef TRACY_ENABLE
        ZoneScopedNC("ResolveAndBindUnitTextures", TRACY_ZONECOLOR_BACKEND);
#endif
        Bool fullyResolved = true;
        // Frontend target the current program samples at a given unit; resolves an
        // aliased native binding when two real textures compete for it (see below).
        // Only consulted on a conflict, so the ordinary unit costs nothing.
        const auto sampledTargetForUnit = [&currentProgram](Int unit) {
            if (!currentProgram || !currentProgram->GetLinkStatus()) {
                return TextureTarget::Unknown;
            }
            const Uint maxUniformLocation = currentProgram->GetMaxUniformLocation();
            for (Uint location = 0; location <= maxUniformLocation; ++location) {
                if (currentProgram->GetUniformSamplerOrImageUnitIndex(location) != unit) continue;
                const auto target = SamplerUniformTextureTarget(currentProgram->GetUniformType(location));
                if (target != TextureTarget::Unknown) {
                    return target;
                }
            }
            return TextureTarget::Unknown;
        };

        for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
            auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
            Array<Bool, (SizeT)TextureTarget::TextureTargetCount> boundBackendTargets{};
            Array<TextureTarget, (SizeT)TextureTarget::TextureTargetCount> claimedByFrontendTarget{};
            claimedByFrontendTarget.fill(TextureTarget::Unknown);

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
                    const SizeT backendTargetIndex = static_cast<SizeT>(backendTarget);
                    if (isDefaultObject && boundBackendTargets[backendTargetIndex]) continue;

                    // Two REAL textures can want the same native target as well - an app is
                    // free to keep a 1D texture and a 2D texture bound to one unit, and GL
                    // resolves which one is sampled from the shader's sampler type. Ask the
                    // program; without an answer the first binding placed stands rather than
                    // being silently overwritten by whichever slot comes last.
                    if (!isDefaultObject && boundBackendTargets[backendTargetIndex] &&
                        claimedByFrontendTarget[backendTargetIndex] != target) {
                        if (sampledTargetForUnit(unit) != target) continue;
                    }
                    const GLenum targetGL = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);

                    // A texture whose mip chain does not satisfy the filter's completeness
                    // rules samples as (0, 0, 0, 1). The ES driver cannot work that out for
                    // itself here: the backend texture is immutable storage, so a level the
                    // application redefined at the wrong size never reached it. Leaving the
                    // native target unbound produces exactly the incomplete-texture result.
                    const auto& effectiveSampler = textureUnit.GetSamplerObject()
                        ? textureUnit.GetSamplerObject()
                        : textureObject->GetSamplerObject();
                    if (MG_State::GLState::SamplesAsIncompleteTexture(textureObject.get(),
                                                                      effectiveSampler.get())) {
                        continue;
                    }

                    // Bind texture object
                    auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get());
                    if (!backendTextureSlot || !*backendTextureSlot) {
                        fullyResolved = false;
                        continue;
                    }

                    (*backendTextureSlot)->Bind(targetGL, unit);
                    boundBackendTargets[backendTargetIndex] = true;
                    claimedByFrontendTarget[backendTargetIndex] = target;
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
        }
        return fullyResolved;
    }

    // Per-unit memo of the sampler-registry lookup, NOT of the resulting unit binding
    // (BackendSamplerObject::Bind already dedups against g_boundSamplersCache). The registry
    // pairs a live frontend sampler with one backend object for the frontend's whole
    // lifetime: GetOrCreate replaces a pairing only when the frontend expired, entries are
    // erased only when the frontend expired, and a unit's sampler cannot expire while the
    // unit holds it - so owner-equality of the unit's current sampler against the snapshot
    // proves the cached pointer is exactly what Find would return. A miss result is never
    // cached: the backend object may not exist yet when the unit pass runs (the program
    // pass creates it later in the same draw), and a cached miss would keep skipping the
    // bind after it appears.
    struct UnitSamplerLookupMemo {
        WeakPtr<MG_State::GLState::SamplerObject> frontend{};
        SamplerImpl::BackendSamplerObject* backend = nullptr;
    };
    static Array<UnitSamplerLookupMemo, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
        g_unitSamplerLookupMemos;

    static SamplerImpl::BackendSamplerObject* ResolveUnitSamplerBackend(
        Int unit, const SharedPtr<MG_State::GLState::SamplerObject>& samplerObject) {
        auto& memo = g_unitSamplerLookupMemos[static_cast<SizeT>(unit)];
        if (memo.backend && OwnerEquals(memo.frontend, samplerObject)) {
            return memo.backend;
        }
        auto* backendSamplerSlot = SamplerImpl::g_backendSamplerObjects.Find(samplerObject.get());
        if (backendSamplerSlot && *backendSamplerSlot) {
            memo.frontend = samplerObject;
            memo.backend = backendSamplerSlot->get();
            return memo.backend;
        }
        return nullptr;
    }

    // Puts each touched unit's frontend sampler object on the backend unit. Not part
    // of the texture memo below (BindCurrentProgramWithResources may rewrite sampled
    // units' samplers right after this runs - raw-depth-fetch substitution,
    // per-program sampler objects); instead the walk carries its own memo:
    //
    // The walk is a pure function of WHICH sampler object each touched unit holds -
    // covered by (context id, unit-bindings epoch, high-water mark; the epoch
    // snapshot includes each unit's sampler object and bumps when the mark moves) -
    // and replaying it as "do nothing" additionally requires the sampler bindings it
    // left to still be on the driver, which the row compare against
    // g_boundSamplersCache (the shadow every sampler bind routes through) proves.
    // Any later writer - the program pass substituting a raw-depth sampler, a
    // frontend sampler bind/unbind (epoch), the ES-context generation moving - lands
    // in one of those inputs. A unit whose sampler had no backend object yet leaves
    // its row untouched; the object is only created by the program pass, whose Bind
    // moves the row and thereby re-opens this memo, so the miss cannot be latched.
    static Uint64 g_unitSamplerWalkContextId = 0;
    static Uint64 g_unitSamplerWalkEpoch = 0;
    static Int g_unitSamplerWalkMaxUnit = -1;
    static Uint g_unitSamplerWalkContextGeneration = 0;
    static Bool g_unitSamplerWalkValid = false;
    static decltype(SamplerImpl::g_boundSamplersCache) g_unitSamplerWalkRows{};

    static void BindCurrentUnitSamplers(const TextureImpl::DrawTextureSyncKeys& keys) {
        const Int maxTouchedUnit = keys.maxTouchedUnit;
        const SizeT rowBytes =
            static_cast<SizeT>(maxTouchedUnit + 1) * sizeof(SamplerImpl::g_boundSamplersCache[0]);
        if (g_unitSamplerWalkValid && g_unitSamplerWalkContextId == keys.contextId &&
            g_unitSamplerWalkEpoch == keys.unitBindingsEpoch && g_unitSamplerWalkMaxUnit == maxTouchedUnit &&
            g_unitSamplerWalkContextGeneration == g_backendContextGeneration &&
            std::memcmp(g_unitSamplerWalkRows.data(), SamplerImpl::g_boundSamplersCache.data(), rowBytes) == 0) {
            return;
        }
        g_unitSamplerWalkValid = false;
        for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
            const auto& samplerObject = MG_State::pGLContext->GetTextureUnitObject(unit).GetSamplerObject();
            if (samplerObject) {
                if (auto* backendSampler = ResolveUnitSamplerBackend(unit, samplerObject)) {
                    backendSampler->Bind(unit);
                }
            } else {
                // Symmetric with the bind above: a sampler object left on the unit by an earlier
                // draw keeps being applied, and on a multisample texture - which takes no sampler
                // object at all - the draw is rejected outright.
                SamplerImpl::UnbindSampler(unit);
            }
        }
        g_unitSamplerWalkContextId = keys.contextId;
        g_unitSamplerWalkEpoch = keys.unitBindingsEpoch;
        g_unitSamplerWalkMaxUnit = maxTouchedUnit;
        g_unitSamplerWalkContextGeneration = g_backendContextGeneration;
        std::memcpy(g_unitSamplerWalkRows.data(), SamplerImpl::g_boundSamplersCache.data(), rowBytes);
        g_unitSamplerWalkValid = true;
    }

    // Memo of the resolved per-unit TEXTURE bindings, so a steady-state draw loop stops
    // re-deriving an answer nothing has invalidated (a Minecraft frame issues thousands of
    // draws that touch none of the inputs below, and the resolution walks every binding slot
    // of every touched unit three times).
    //
    // ResolveAndBindUnitTextures is a pure function of:
    //   * the GLContext identity - a never-reused id, since the counters below restart at 0 in
    //     a new context and a recreated one can land on the old heap address;
    //   * the per-unit bindings - which texture object every slot holds and which sampler
    //     object each unit carries, keyed as TextureImpl::CurrentUnitBindingsEpoch rather
    //     than as the texture bind generation: the generation also bumps on redundant
    //     re-binds (26.2 re-binds the unit's own sampler around every texture-unit switch),
    //     which would force a full re-resolution per draw for an answer that cannot have
    //     moved. The one input the old generation key covered that the epoch does not - a
    //     default texture gaining or losing an image with no bind moving - bumps the
    //     sampling-resolution generation below (SetInternalFormat -> BumpShapeVersion);
    //   * the touched-unit high-water mark (units above it have provably-empty slots);
    //   * the program that arbitrates two real textures aliased onto one native target -
    //     identity, plus the lifetime id because a freed program can be replaced at the same
    //     address, plus the backend-state version which moves on every relink and on every
    //     sampler-uniform unit assignment, plus the link status;
    //   * the sampling-resolution generation - any texture shape change or any sampler
    //     parameter change, i.e. everything mipmap-completeness is computed from, and
    //     completeness is what decides whether a texture is bound at all;
    //   * the ES context generation, because the backend texture ids and the driver's own
    //     binding state die with the context.
    //
    // A matching key only says the ANSWER is unchanged; replaying it as "do nothing" also
    // requires the bindings it established to still be on the driver. Rather than enumerate
    // every writer, the memo keeps the binding shadow it left and compares it: that shadow is
    // already the authority every redundant-bind filter in this backend trusts, and every path
    // that puts a texture on a unit behind this function's back maintains it - the upload
    // path's scratch bind on the temp unit (BackendTextureObject::Bind out of
    // SyncMipmapsToBackend), CopyTexSubImage2D and GenerateMipmap binding on the active unit,
    // the glBindTextures fast path, and the self-scrub a BackendTextureObject performs when it
    // is destroyed or respecified.
    struct ResolvedTextureBindingMemo {
        Bool valid = false;
        Uint64 glContextId = 0;
        Int maxTouchedUnit = -1;
        Uint64 unitBindingsEpoch = 0;
        Uint64 samplingResolutionGeneration = 0;
        const void* program = nullptr;
        Uint64 programLifetimeId = 0;
        Uint32 programBackendStateVersion = 0;
        Bool programLinked = false;
        Uint contextGeneration = 0;
        decltype(TextureImpl::g_boundTexturesCache) boundTextures{};
    };
    // Small per-PROGRAM memo set, not one global: the program is part of the key
    // (it arbitrates aliased native targets), so a frame that cycles a handful of
    // programs over UNCHANGED unit bindings - Sodium issues 62 glUseProgram per
    // frame - invalidated a single memo on every switch and re-ran the full
    // three-pass slot walk for an answer that had not moved. Entries are selected
    // by raw program pointer; everything a recycled address could confuse is
    // caught by the full key compare (lifetime id) exactly as before. Round-robin
    // eviction; 4 entries cover the shader set a Minecraft-shaped frame cycles.
    static Array<ResolvedTextureBindingMemo, 4> g_resolvedTextureBindingMemos;
    static Uint32 g_resolvedTextureBindingMemoCursor = 0;

    // Rebinds every frontend texture unit's textures (and sampler objects) on the
    // backend context. Needed before draws AND compute dispatches: content syncs
    // (SyncTextureObjectToBackend) bind scratch textures on the active unit as a
    // side effect, so unit bindings must be re-established afterwards or shaders
    // sample whatever texture the last sync left behind (e.g. Flywheel's depth
    // pyramid downsample reading a stale unit-0 binding instead of the depth
    // attachment).
    static void BindCurrentTextures(const TextureImpl::DrawTextureSyncKeys& keys,
                                    const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram) {
#ifdef TRACY_ENABLE
        ZoneScopedNC("BindCurrentTextures", TRACY_ZONECOLOR_BACKEND);
#endif
        // Units past the frontend's high-water mark have provably-empty slots.
        const Int maxTouchedUnit = keys.maxTouchedUnit;

        // Entry selection by program pointer; a missing program takes the round-robin
        // victim. WHICH entry is used is only a performance choice - correctness sits
        // entirely in the full key + shadow compare below, unchanged from the single
        // memo this set replaces.
        const void* programKey = static_cast<const void*>(currentProgram.get());
        ResolvedTextureBindingMemo* memoSlot = nullptr;
        for (auto& candidate : g_resolvedTextureBindingMemos) {
            if (candidate.valid && candidate.program == programKey) {
                memoSlot = &candidate;
                break;
            }
        }
        if (!memoSlot) {
            g_resolvedTextureBindingMemoCursor =
                (g_resolvedTextureBindingMemoCursor + 1) % g_resolvedTextureBindingMemos.size();
            memoSlot = &g_resolvedTextureBindingMemos[g_resolvedTextureBindingMemoCursor];
        }
        auto& memo = *memoSlot;
        const SizeT shadowBytes =
            static_cast<SizeT>(maxTouchedUnit + 1) * sizeof(TextureImpl::g_boundTexturesCache[0]);
        const Uint64 unitBindingsEpoch = keys.unitBindingsEpoch;
        const Bool keysMatch = memo.valid && memo.glContextId == keys.contextId &&
                               memo.maxTouchedUnit == maxTouchedUnit &&
                               memo.unitBindingsEpoch == unitBindingsEpoch &&
                               memo.samplingResolutionGeneration == keys.samplingGeneration &&
                               memo.program == programKey &&
                               memo.programLifetimeId == (currentProgram ? currentProgram->GetLifetimeId() : 0) &&
                               memo.programBackendStateVersion ==
                                   (currentProgram ? currentProgram->GetBackendStateVersion() : 0) &&
                               memo.programLinked == (currentProgram && currentProgram->GetLinkStatus()) &&
                               memo.contextGeneration == g_backendContextGeneration;
        // Short-circuited: the shadow compare is only meaningful once the key (and with it the
        // snapshotted row count) matches.
        if (!keysMatch || std::memcmp(memo.boundTextures.data(), TextureImpl::g_boundTexturesCache.data(),
                                      shadowBytes) != 0) {
            memo.valid = false;
            if (ResolveAndBindUnitTextures(currentProgram, maxTouchedUnit)) {
                memo.glContextId = keys.contextId;
                memo.maxTouchedUnit = maxTouchedUnit;
                memo.unitBindingsEpoch = unitBindingsEpoch;
                memo.samplingResolutionGeneration = keys.samplingGeneration;
                memo.program = programKey;
                memo.programLifetimeId = currentProgram ? currentProgram->GetLifetimeId() : 0;
                memo.programBackendStateVersion = currentProgram ? currentProgram->GetBackendStateVersion() : 0;
                memo.programLinked = currentProgram && currentProgram->GetLinkStatus();
                memo.contextGeneration = g_backendContextGeneration;
                std::memcpy(memo.boundTextures.data(), TextureImpl::g_boundTexturesCache.data(), shadowBytes);
                memo.valid = true;
            }
        }

        BindCurrentUnitSamplers(keys);
    }

    void BindCurrentTextures() {
        BindCurrentTextures(TextureImpl::CaptureDrawTextureSyncKeys(), MG_State::pGLContext->GetProgramForDraw());
    }

    // Binds the current program's backend object and re-establishes its per-program
    // resources: global UBO contents, uniform-block bindings, and sampler uniform
    // units (layout(binding=N) qualifiers are stripped from transpiled ESSL, so the
    // association must be rebuilt through the API). Compute dispatches depend on
    // this as much as draws do — e.g. Flywheel's cull shader reads the
    // _FlwFrameUniforms block and the _flw_depthPyramid sampler.
    static void BindCurrentProgramWithResources(
        const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
        const TextureImpl::DrawTextureSyncKeys& keys) {
        if (currentProgram && currentProgram->GetLinkStatus() && currentProgram->GetSpirvStatus()) {
#ifdef TRACY_ENABLE
            ZoneScopedNC("BindCurrentProgram", TRACY_ZONECOLOR_BACKEND);
#endif
            // The twin SyncCurrentProgram just resolved for this draw; the registry
            // Find only runs if the stash somehow does not match (defensive fallback).
            PrgramImpl::BackendProgramObjectImpl* twin =
                PrgramImpl::g_currentDrawFrontendProgram == currentProgram.get()
                    ? PrgramImpl::g_currentDrawBackendProgram
                    : nullptr;
            if (!twin) {
                auto* backendProgramSlot = PrgramImpl::g_backendProgramObjects.Find(currentProgram.get());
                if (backendProgramSlot) {
                    twin = backendProgramSlot->get();
                }
            }
            if (twin) {
                auto& backendProgram = *twin;
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
                    // Pre-loop epoch read; the per-resource drawCleanEpoch stamps below
                    // follow the same stamp-the-pre-read-value rule as the VAO memo
                    // (Managers.h documents the contract and the mutation-site list).
                    const Uint64 bufferEpoch =
                        uboCount > 0 ? BufferImpl::CurrentBufferMutationEpoch() : 0;
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
                            // Clean-probe (or epoch-stamped) fast path before the full
                            // EnsureBufferResource: an unchanged static UBO needs no
                            // storage work, only its binding re-established below.
                            auto* backendResource = BufferImpl::GetBufferResource(bufferObj.get());
                            if (!backendResource || backendResource->drawCleanEpoch != bufferEpoch) {
                                if (backendResource &&
                                    BufferImpl::IsBufferDrawClean(bufferObj.get(), backendResource)) {
                                    backendResource->drawCleanEpoch = bufferEpoch;
                                } else {
                                    backendResource = BufferImpl::EnsureBufferResource(bufferObj);
                                }
                            }
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
                                MGLOG_E_ONCE("No backend buffer found for UBO binding, cannot bind UBO.");
                            }
                        }
                    }
                }

                // Atomic counter buffers. Bound here rather than beside the storage-buffer sync
                // in SyncNeccessaryBuffers because the reserved slot the transpiled ESSL reads
                // them at is PROGRAM state: it is `top - GL binding` for the counter blocks THIS
                // program declares, and no other program's blocks live there. Both the draw and
                // the dispatch path reach this, which is what a compute-shader counter needs.
                if (!backendProgram.GetAtomicCounterBindings().empty()) {
                    BufferImpl::SyncAtomicCounterBuffers(backendProgram.GetAtomicCounterBindings(),
                                                         backendProgram.GetAtomicCounterEsslBindingTop());
                }

                {
#ifdef TRACY_ENABLE
                    ZoneScopedNC("BindSamplerUnit", TRACY_ZONECOLOR_BACKEND);
#endif
                    // Sampler unit binding: backend locations are cached at link time;
                    // glUniform1i is program state, so it is only re-issued when the
                    // frontend-assigned unit differs from what this program last saw.
                    //
                    // The whole pass sits behind the per-twin SamplerPassMemo (see its
                    // declaration for the invalidation enumeration): while the keys hold
                    // and every previously touched unit's sampler-shadow row is exactly
                    // what this pass last left there, re-running it is a provable no-op.
                    auto& samplerPassMemo = backendProgram.GetSamplerPassMemo();
                    const Uint32 programBackendStateVersion = currentProgram->GetBackendStateVersion();
                    Bool samplerPassClean =
                        samplerPassMemo.valid && samplerPassMemo.contextId == keys.contextId &&
                        samplerPassMemo.unitBindingsEpoch == keys.unitBindingsEpoch &&
                        samplerPassMemo.samplingGeneration == keys.samplingGeneration &&
                        samplerPassMemo.backendStateVersion == programBackendStateVersion &&
                        samplerPassMemo.textureContextGeneration == g_backendContextGeneration;
                    if (samplerPassClean) {
                        for (Uint i = 0; i < samplerPassMemo.count; ++i) {
                            if (SamplerImpl::g_boundSamplersCache[samplerPassMemo.units[i]] !=
                                samplerPassMemo.rows[i]) {
                                samplerPassClean = false;
                                break;
                            }
                        }
                    }
                    if (!samplerPassClean) {
                        constexpr SizeT kMaxMemoEntries =
                            PrgramImpl::BackendProgramObjectImpl::SamplerPassMemo::kMaxEntries;
                        samplerPassMemo.valid = false;
                        samplerPassMemo.count = 0;
                        Bool memoisable = true;
                        for (auto& samplerBinding : backendProgram.GetSamplerUniformBindings()) {
                            const auto unit =
                                currentProgram->GetUniformSamplerOrImageUnitIndex(samplerBinding.frontendLocation);
                            if (unit == -1) continue;
                            // Record the touched unit for the memo; a pass touching more
                            // units than the memo can carry (or an out-of-range unit)
                            // simply never memoises.
                            if (memoisable && samplerPassMemo.count < kMaxMemoEntries && unit >= 0 &&
                                unit < static_cast<Int>(SamplerImpl::g_boundSamplersCache.size())) {
                                samplerPassMemo.units[samplerPassMemo.count++] = static_cast<Uint8>(unit);
                            } else {
                                memoisable = false;
                            }
                            if (samplerBinding.lastAssignedUnit != unit) {
                                g_GLESFuncs.glUniform1i(samplerBinding.backendLocation, unit);
                                samplerBinding.lastAssignedUnit = unit;
                            }

                            auto& textureUnit = MG_State::pGLContext->GetTextureUnitObject(unit);
                            auto& samplerObject = textureUnit.GetSamplerObject();
                            const auto& texture2D =
                                textureUnit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();

                            // ES has no per-texture/sampler LOD bias, so the transpiled ESSL folds
                            // it in from a uniform (PrgramImpl::EmulateTextureLodBias). A bound
                            // sampler object overrides the texture's own sampler state, as in GL.
                            if (samplerBinding.lodBiasLocation >= 0) {
                                Float lodBias = 0.0f;
                                if (samplerObject) {
                                    lodBias = samplerObject->GetLodBias();
                                } else if (const auto sampledTarget =
                                               SamplerUniformTextureTarget(samplerBinding.uniformType);
                                           sampledTarget != TextureTarget::Unknown) {
                                    const auto& boundTexture =
                                        textureUnit.GetBindingSlot(sampledTarget).GetBoundObject();
                                    if (boundTexture && boundTexture->GetSamplerObject()) {
                                        lodBias = boundTexture->GetSamplerObject()->GetLodBias();
                                    }
                                }
                                if (lodBias != samplerBinding.lastAssignedLodBias) {
                                    g_GLESFuncs.glUniform1f(samplerBinding.lodBiasLocation, lodBias);
                                    samplerBinding.lastAssignedLodBias = lodBias;
                                }
                            }
                            const SharedPtr<MG_State::GLState::SamplerObject>* rawDepthSamplerObject =
                                &samplerObject;
                            if (!*rawDepthSamplerObject && texture2D) {
                                rawDepthSamplerObject = &texture2D->GetSamplerObject();
                            }

                            if (samplerBinding.uniformType == GL_SAMPLER_2D && texture2D &&
                                NeedsRawDepthFetchSampler(*rawDepthSamplerObject, texture2D->GetFormat())) {
                                GetRawDepthFetchSampler()->Bind(unit);
                                MGLOG_D("Using raw depth fetch sampler on unit %d.", unit);
                            } else if (samplerObject) {
                                auto* backendSampler = ResolveUnitSamplerBackend(unit, samplerObject);
                                if (!backendSampler) {
                                    auto& backendObj =
                                        SamplerImpl::g_backendSamplerObjects.GetOrCreate(samplerObject);
                                    if (!backendObj) {
                                        backendObj = MakeShared<SamplerImpl::BackendSamplerObject>();
                                    }
                                    backendSampler = backendObj.get();
                                }
                                backendSampler->SyncToBackend(samplerObject);
                                // Syncing the object's parameters is not the same as putting it on the
                                // unit: without this the driver kept sampling with the texture's own
                                // parameters and every sampler object was inert.
                                backendSampler->Bind(unit);
                            } else {
                                SamplerImpl::UnbindSampler(unit);
                            }
                        }
                        if (memoisable) {
                            // Snapshot the rows AFTER the pass: they are exactly what a
                            // clean replay must find untouched.
                            for (Uint i = 0; i < samplerPassMemo.count; ++i) {
                                samplerPassMemo.rows[i] = SamplerImpl::g_boundSamplersCache[samplerPassMemo.units[i]];
                            }
                            samplerPassMemo.contextId = keys.contextId;
                            samplerPassMemo.unitBindingsEpoch = keys.unitBindingsEpoch;
                            samplerPassMemo.samplingGeneration = keys.samplingGeneration;
                            samplerPassMemo.backendStateVersion = programBackendStateVersion;
                            samplerPassMemo.textureContextGeneration = g_backendContextGeneration;
                            samplerPassMemo.valid = true;
                        }
                    }
                }
            } else {
                g_GLESFuncs.glUseProgram(0);
                PrgramImpl::g_lastUsedBackendProgramId = 0;
                MGLOG_E_ONCE("No backend program found (maybe not synced) for current program, cannot use program.");
            }
        }
    }

    // Raw pointer: only ever used inside one GL entry point after PrepareForDraw/
    // PrepareForCompute, where the current program (and therefore its registry twin)
    // is pinned for the duration. Prefers the per-draw stash those preparations wrote.
    static PrgramImpl::BackendProgramObjectImpl* GetCurrentBackendProgram() {
        const auto& currentProgram = MG_State::pGLContext->GetProgramForDraw();
        if (!currentProgram || !currentProgram->GetLinkStatus() || !currentProgram->GetSpirvStatus()) {
            return nullptr;
        }
        if (PrgramImpl::g_currentDrawFrontendProgram == currentProgram.get()) {
            return PrgramImpl::g_currentDrawBackendProgram;
        }
        if (auto* backendProgramSlot = PrgramImpl::g_backendProgramObjects.Find(currentProgram.get())) {
            return backendProgramSlot->get();
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

    void SetCurrentBaseVertex(Int32 baseVertex) {
        if (const auto program = GetCurrentBackendProgram()) {
            program->SetBaseVertex(baseVertex);
        }
    }

    Bool CurrentProgramReadsDrawID() {
        const auto program = GetCurrentBackendProgram();
        return program != nullptr && program->ReadsDrawID();
    }

    Bool CurrentProgramReadsBaseVertex() {
        const auto program = GetCurrentBackendProgram();
        return program != nullptr && program->ReadsBaseVertex();
    }

    // The two questions above, asked from BEFORE PrepareForDraw - where neither can be
    // answered honestly. GetCurrentBackendProgram only sees a twin that a previous draw
    // already synced, and a twin from before a relink still carries the previous link's
    // uniform locations, so "no" there means "not known yet" at least as often as it
    // means no. The multi-draw compute tier has to decide whether to flatten a batch
    // before PrepareForDraw runs (its dispatch cannot come after the draw state), and
    // flattening a batch that turns out to need per-sub-draw values is unrecoverable -
    // so an unanswerable program counts as needing them.
    Bool CurrentProgramMayNeedPerSubDrawBuiltins(Bool batchCarriesBaseVertices) {
        const auto& currentProgram = MG_State::pGLContext->GetProgramForDraw();
        const auto program = GetCurrentBackendProgram();
        if (!currentProgram || program == nullptr ||
            program->GetSyncedLinkVersion() != currentProgram->GetLinkVersion()) {
            return true;
        }
        return program->ReadsDrawID() || (batchCarriesBaseVertices && program->ReadsBaseVertex());
    }

    // ---- gl_ViewportIndex routing emulation, draw half ---------------------------------------
    // See the block comment in Managers.h for what this is and why. Here is the state half: one
    // replay pass per DISTINCT viewport state, each pushing that state onto the ES context's one
    // viewport / one scissor / one depth range and telling the fragment gate which indices it
    // serves.
    namespace ViewportRoutingImpl {
        // One replay pass: the state to push, and the set of gl_ViewportIndex values whose
        // fragments this pass is allowed to keep.
        struct RoutingPass {
            IntVec4 viewport{};
            IntVec4 scissorBox{};
            FloatVec2 depthRange{};
            Bool scissorTest = false;
            Uint32 indexMask = 0;
        };

        static constexpr Uint32 kAllViewportsMask =
            (RenderStateParameters::MAX_VIEWPORTS >= 32)
                ? 0xFFFFFFFFu
                : ((1u << RenderStateParameters::MAX_VIEWPORTS) - 1u);

        // The plan for the draw currently being issued. A file-scope buffer rather than a return
        // value because Begin/Apply/End are three calls around a draw the caller writes, and a
        // fixed array of 16 keeps it allocation-free on a path that is per draw. NOT re-entrant,
        // which is a property of the call sites and not an accident: every wrap in this file and
        // in MultiDraw.cpp is around the innermost native glDraw*, so no replay can begin inside
        // another - and a multi-draw tier that replayed its whole loop would be nesting.
        static Array<RoutingPass, RenderStateParameters::MAX_VIEWPORTS> g_passes{};
        static Uint g_passCount = 0;
        static PrgramImpl::BackendProgramObjectImpl* g_routedProgram = nullptr;

        // What index `i` actually rasterizes against, resolved exactly the way SyncRenderState
        // resolves index 0 - including both substitutions it makes, which are not cosmetic:
        //
        //   * a viewport of zero extent means "the application has never called glViewport", and
        //     GL's initial viewport is the whole surface, which the frontend cannot spell before
        //     a surface exists;
        //   * a scissor rectangle is read through the WRITTEN flag and not through its extent,
        //     because glScissor(0, 0, 0, 0) is a legal request meaning "reject every fragment"
        //     and is byte-identical to the never-written default that means the opposite.
        //
        // Resolving them here rather than deferring to SyncRenderState is what makes the grouping
        // below correct: two indices that differ only in a field that resolves to the same
        // rectangle really do rasterize identically and must share one pass.
        static RoutingPass ResolveIndexState(const RenderStateParameters& parameters, Uint index,
                                             Int surfaceWidth, Int surfaceHeight) {
            RoutingPass pass;
            const FloatVec4& viewport = parameters.Viewports[index];
            pass.viewport = IntVec4(static_cast<Int>(std::lround(viewport.x())),
                                    static_cast<Int>(std::lround(viewport.y())),
                                    static_cast<Int>(std::lround(viewport.z())),
                                    static_cast<Int>(std::lround(viewport.w())));
            if ((pass.viewport.z() <= 0 || pass.viewport.w() <= 0) && surfaceWidth > 0 && surfaceHeight > 0) {
                pass.viewport = IntVec4(0, 0, surfaceWidth, surfaceHeight);
            }
            pass.scissorBox = parameters.ScissorBoxes[index];
            if ((parameters.ScissorBoxWrittenMask & (1u << index)) == 0 && surfaceWidth > 0 &&
                surfaceHeight > 0) {
                pass.scissorBox = IntVec4(0, 0, surfaceWidth, surfaceHeight);
            }
            pass.depthRange = parameters.DepthRanges[index];
            pass.scissorTest = (parameters.ScissorTestEnabledMask & (1u << index)) != 0;
            return pass;
        }

        static Bool SameState(const RoutingPass& a, const RoutingPass& b) {
            return a.viewport == b.viewport && a.scissorBox == b.scissorBox &&
                   a.depthRange == b.depthRange && a.scissorTest == b.scissorTest;
        }
    } // namespace ViewportRoutingImpl

    Uint BeginViewportRoutingPasses() {
        using namespace ViewportRoutingImpl;
        g_passCount = 1;
        g_routedProgram = nullptr;

        // The whole emulation behind one static load, for every application that has never built
        // a program writing gl_ViewportIndex - which is all of them but the conformance suite.
        // Without it every draw in the process would pay GetCurrentBackendProgram's chain of
        // frontend lookups for an answer that cannot change.
        if (!g_anyProgramRoutesViewportIndex) {
            return 1;
        }

        auto* program = GetCurrentBackendProgram();
        if (program == nullptr || !program->RoutesViewportIndex()) {
            return 1;
        }
        g_routedProgram = program;
        // The gate reads zero until something writes it, and a zero mask discards every fragment.
        // So this is not an optimization that can be skipped in the one-pass case - it is what
        // keeps a routing program drawing at all.
        program->SetViewportPassMask(kAllViewportsMask);

        // Replaying multiplies every side effect the vertex and geometry stages have, and the
        // fragment gate can only undo the ones that happen in the FRAGMENT stage. Transform
        // feedback records per emitted primitive, so a replayed draw would write its vertices N
        // times; rasterizer discard means there are no fragments to gate at all, so replaying
        // would be pure cost with nothing to show for it. Both fall back to a single pass with an
        // open gate, i.e. to the pre-emulation behaviour, rather than to wrong data.
        if (MG_State::pGLContext->IsTransformFeedbackActive() ||
            MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::RasterizerDiscard)) {
            return 1;
        }

        const auto& parameters = MG_State::pGLContext->GetRenderStateParameters();
        Int surfaceWidth = 0;
        Int surfaceHeight = 0;
        if (!QueryCurrentSurfaceSize(surfaceWidth, surfaceHeight)) {
            surfaceWidth = 0;
            surfaceHeight = 0;
        }

        Uint count = 0;
        for (Uint index = 0; index < RenderStateParameters::MAX_VIEWPORTS; ++index) {
            const RoutingPass resolved =
                ResolveIndexState(parameters, index, surfaceWidth, surfaceHeight);
            Uint existing = 0;
            for (; existing < count; ++existing) {
                if (SameState(g_passes[existing], resolved)) break;
            }
            if (existing == count) {
                g_passes[count] = resolved;
                ++count;
            }
            g_passes[existing].indexMask |= (1u << index);
        }

        // One group is the overwhelmingly common case - it is what glViewport, glScissor and
        // glDepthRange leave behind, because ARB_viewport_array defines all three as writing
        // EVERY index. The mask is already open and index 0's state is what SyncRenderState
        // pushed, so there is nothing to replay and nothing to restore.
        if (count <= 1) {
            g_passCount = 1;
            return 1;
        }
        g_passCount = count;
        return count;
    }

    void ApplyViewportRoutingPass(Uint pass) {
        using namespace ViewportRoutingImpl;
        if (pass >= g_passCount || g_routedProgram == nullptr) {
            return;
        }
        const RoutingPass& entry = g_passes[pass];
        g_GLESFuncs.glViewport(entry.viewport.x(), entry.viewport.y(), entry.viewport.z(),
                               entry.viewport.w());
        g_GLESFuncs.glScissor(entry.scissorBox.x(), entry.scissorBox.y(), entry.scissorBox.z(),
                              entry.scissorBox.w());
        // ES has one scissor-test enable where GL has sixteen, so the per-index bit becomes a
        // per-pass glEnable/glDisable. This is the half DirectVulkan cannot do at all (Vulkan has
        // no per-viewport scissor toggle either and has to widen a disabled index's rectangle to
        // the whole framebuffer instead); here the rectangle stays honest.
        entry.scissorTest ? g_GLESFuncs.glEnable(GL_SCISSOR_TEST) : g_GLESFuncs.glDisable(GL_SCISSOR_TEST);
        g_GLESFuncs.glDepthRangef(entry.depthRange.x(), entry.depthRange.y());
        g_routedProgram->SetViewportPassMask(entry.indexMask);
    }

    void EndViewportRoutingPasses(Uint passCount) {
        using namespace ViewportRoutingImpl;
        if (passCount <= 1) {
            // Nothing was pushed and the mask is already open; leaving the shadow alone here is
            // what keeps a non-routing draw at exactly its previous cost.
            g_routedProgram = nullptr;
            return;
        }
        if (g_routedProgram != nullptr) {
            // Any draw that reaches the driver without going through a replay - an internal blit,
            // or a path this emulation has not been taught about - must not inherit the last
            // pass's mask and paint nothing.
            g_routedProgram->SetViewportPassMask(kAllViewportsMask);
        }
        g_routedProgram = nullptr;
        g_passCount = 0;
        // The viewport, scissor, scissor-test enable and depth range now on the ES context belong
        // to the last replay pass, and the shadow SyncRenderState diffs against does not know it.
        // A full resync is the honest repair and costs one state push on the next draw, which
        // only a viewport-routing workload ever pays.
        RenderStateImpl::InvalidateSyncedRenderState();
    }

    static Bool SupportsNativeIndirectDraws() {
        return g_GLESCapabilities.SupportsDrawIndirect;
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
            // gl_BaseVertex has no SSBO view of its own: the command's baseVertex word is read
            // from the CPU shadow, so a command whose baseVertex a compute shader wrote this
            // frame is not observable here (baseInstance is, through the view above). Feeding
            // the stale-but-usually-correct shadow beats leaving the uniform at the previous
            // draw's value, which is what a program reading gl_BaseVertex saw before.
            const Bool feedBaseVertex = CurrentProgramReadsBaseVertex();
            for (GLsizei i = 0; i < drawcount; ++i) {
                const SizeT cmdByteOffset = commandOffset + static_cast<SizeT>(i) * stride;
                SetCurrentDrawID(static_cast<Uint32>(i));
                if (paramsBinding >= 0 && backendProgram) {
                    // baseInstance is the 5th word of DrawElementsIndirectCommand.
                    backendProgram->SetBaseInstanceWordIndex(static_cast<Int32>((cmdByteOffset + 16) / 4));
                    if (feedBaseVertex) {
                        DrawElementsIndirectCommand cmd{};
                        std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                        SetCurrentBaseVertex(cmd.baseVertex);
                    }
                } else {
                    DrawElementsIndirectCommand cmd{};
                    std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                    SetCurrentBaseInstance(cmd.baseInstance);
                    SetCurrentBaseVertex(cmd.baseVertex);
                }
                ForEachViewportRoutingPass([&] {
                    g_GLESFuncs.glDrawElementsIndirect(mode, type, reinterpret_cast<const void*>(cmdByteOffset));
                });
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
                SetCurrentBaseVertex(cmd.baseVertex);
                const auto indexByteOffset = static_cast<SizeT>(cmd.firstIndex) * indexSize;
                ForEachViewportRoutingPass([&] {
                    g_GLESFuncs.glDrawElementsInstancedBaseVertex(
                        mode, static_cast<GLsizei>(cmd.count), type, reinterpret_cast<const GLvoid*>(indexByteOffset),
                        static_cast<GLsizei>(cmd.instanceCount), cmd.baseVertex);
                });
            }
        }
        SetCurrentDrawID(0);
        SetCurrentBaseInstance(0);
        SetCurrentBaseVertex(0);
    }

    static void ExecuteArraysIndirectCommands(GLenum mode, const Uint8* commandBytes, SizeT commandOffset,
                                              const SharedPtr<MG_State::GLState::BufferObject>& drawIndirectBuffer,
                                              GLsizei drawcount, GLsizei stride, const char* label) {
        (void)label;
        // DrawArraysIndirectCommand has no baseVertex word, so gl_BaseVertex is zero for every
        // command here. Written BEFORE the draws, not merely restored after them: the previous
        // draw is what leaves a stale value, and restoring afterwards would only protect the
        // NEXT draw while these commands ran with the stale one.
        SetCurrentBaseVertex(0);
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
                ForEachViewportRoutingPass([&] {
                    g_GLESFuncs.glDrawArraysIndirect(mode, reinterpret_cast<const void*>(cmdByteOffset));
                });
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
                ForEachViewportRoutingPass([&] {
                    g_GLESFuncs.glDrawArraysInstanced(mode, static_cast<GLint>(cmd.first),
                                                      static_cast<GLsizei>(cmd.count),
                                                      static_cast<GLsizei>(cmd.instanceCount));
                });
            }
        }
        SetCurrentDrawID(0);
        SetCurrentBaseInstance(0);
    }

    void PrepareForCompute(Bool includeDispatchIndirectBuffer) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        // Single per-dispatch program resolve and texture-key capture, as in
        // PrepareForDraw (nothing below can move either). The DISPATCH accessor: with a
        // pipeline bound this is its compute stage program, which is a whole program on its
        // own - the graphics composite a draw builds carries no compute stage.
        const auto& currentProgram = MG_State::pGLContext->GetProgramForDispatch();
        const TextureImpl::DrawTextureSyncKeys textureKeys = TextureImpl::CaptureDrawTextureSyncKeys();

        BufferImpl::SyncComputeBuffers(includeDispatchIndirectBuffer);
        TextureImpl::SyncNeccessaryTextures(textureKeys);
        TextureImpl::SyncImageTextureBindings();
        TextureImpl::MarkWritableImageBufferTexturesGpuWritten();
        PrgramImpl::SyncCurrentProgram(currentProgram);

        if (!currentProgram || !currentProgram->GetLinkStatus() || !currentProgram->GetSpirvStatus()) {
            g_GLESFuncs.glUseProgram(0);
            PrgramImpl::g_lastUsedBackendProgramId = 0;
            return;
        }

        // Compute shaders sample textures through the same unit bindings as draws
        // (e.g. Flywheel's depth-pyramid downsample reads the depth attachment on
        // unit 0), so re-establish unit bindings after the content syncs above.
        BindCurrentTextures(textureKeys, currentProgram);
        // Compute programs need the same per-program resource sync as draws:
        // uniform-block bindings and sampler units only exist through the API
        // because layout(binding) is stripped from the transpiled ESSL.
        BindCurrentProgramWithResources(currentProgram, textureKeys);
    }

    GLuint GetBackendProgramId(GLuint program) {
        if (!MG_State::pGLContext->ValidateProgramName(program)) {
            MGLOG_E_ONCE("Invalid frontend program object: %u", program);
            return 0;
        }

        auto& programObject = MG_State::pGLContext->GetProgramObject(program);
        if (!programObject) {
            MGLOG_E_ONCE("Program object %u is null.", program);
            return 0;
        }

        auto* backendProgramSlot = PrgramImpl::g_backendProgramObjects.Find(programObject.get());
        auto& backendObj =
            backendProgramSlot ? *backendProgramSlot : PrgramImpl::g_backendProgramObjects.GetOrCreate(programObject);
        if (!backendObj) {
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
        // A colour clear is exactly the operation that is allowed to write a widened
        // attachment's alpha - it is what puts the 1.0 there that every later draw is masked
        // away from. SyncCurrentFBO ran first, so g_alphaWidenedDrawBufferMask already describes
        // the framebuffer this clear will land on.
        RenderStateImpl::SyncRenderState(/*forColorClear=*/(mask & GL_COLOR_BUFFER_BIT) != 0);

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
            // A widened attachment's stored alpha has to end up 1.0, and glClear applies ONE
            // clear colour to every draw buffer - so a framebuffer that mixes a widened
            // attachment with a native one cannot be served by doctoring glClearColor. Take the
            // same per-draw-buffer glClearBufferfv route the out-of-range case already uses and
            // substitute the alpha only where it belongs. Scissor and the colour write mask apply
            // to glClearBufferfv exactly as they do to glClear, so a scissored clear stays
            // scissored and an application that masked alpha off still gets its way (the storage
            // then keeps the 1.0 an earlier clear left, which is the same answer).
            //
            // glClearBufferfv on an INTEGER colour buffer is GL_INVALID_OPERATION, so a
            // framebuffer with one of those as a draw buffer keeps plain glClear - which ES
            // leaves undefined for integer colour buffers anyway, and which an application that
            // wants a defined answer must replace with glClearBufferuiv/iv (those DO substitute
            // the widened alpha). The out-of-range trigger is left exactly as it was.
            const Uint32 widenedDrawBuffers = FramebufferImpl::g_alphaWidenedDrawBufferMask;
            const Bool widenedColorClear =
                widenedDrawBuffers != 0 && FramebufferImpl::g_integerColorDrawBufferMask == 0;
            GLint clearDrawFbo = 0;
            g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &clearDrawFbo);
            if ((outOfRange || widenedColorClear) && clearDrawFbo != 0) {
                GLint maxDrawBuffers = 0;
                GLint clearedCount = 0;
                GLint firstDb = -1;
                g_GLESFuncs.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
                for (GLint i = 0; i < maxDrawBuffers; ++i) {
                    GLint db = GL_NONE;
                    g_GLESFuncs.glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(i), &db);
                    if (i == 0) firstDb = db;
                    if (db != GL_NONE) {
                        const Bool widened = i < 32 && (widenedDrawBuffers & (1u << i)) != 0;
                        const GLfloat value[4] = {cc.x(), cc.y(), cc.z(), widened ? 1.0f : cc.w()};
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
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        CheckPrimitiveRestartSupported(type);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawElements(mode, count, type, indices);
        });
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncFlags syncBit = DrawSyncBit::None;
        PrepareForDraw(syncBit);
        const auto& currentVAO = MG_State::pGLContext->GetBoundVertexArray();
        if (currentVAO) {
            auto* backendVAOSlot = VertexArrayImpl::g_backendVertexArrayObjects.Find(currentVAO.get());
            if (backendVAOSlot && *backendVAOSlot) {
                (*backendVAOSlot)->SyncClientSideAttributesForDrawArrays(currentVAO, first, count);
            }
        }
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawArrays(mode, first, count);
        });
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        CheckPrimitiveRestartSupported(type);
        SetCurrentBaseVertex(basevertex);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
        });
        SetCurrentBaseVertex(0);
    }

    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncFlags syncBit = DrawSyncBit::None;
        PrepareForDraw(syncBit);

        // This loop IS the emulation - there is no batched tier for the non-indexed form -
        // so each sub-draw has to be given its own gl_DrawID here, exactly as the indexed
        // ladder and the indirect executors do. Without it every sub-draw of a
        // glMultiDrawArrays read draw index 0.
        const Bool feedDrawID = CurrentProgramReadsDrawID();
        const auto& currentVAO = MG_State::pGLContext->GetBoundVertexArray();
        for (GLsizei i = 0; i < drawcount; ++i) {
            // Client-side arrays are uploaded per sub-draw range, like the single DrawArrays path.
            if (currentVAO) {
                auto* backendVAOSlot = VertexArrayImpl::g_backendVertexArrayObjects.Find(currentVAO.get());
                if (backendVAOSlot && *backendVAOSlot) {
                    (*backendVAOSlot)->SyncClientSideAttributesForDrawArrays(currentVAO, first[i], count[i]);
                }
            }
            if (feedDrawID) SetCurrentDrawID(static_cast<Uint32>(i));
            ForEachViewportRoutingPass([&] {
                g_GLESFuncs.glDrawArrays(mode, first[i], count[i]);
            });
        }
        if (feedDrawID) SetCurrentDrawID(0);
    }

    // Both glMultiDrawElements entry points are emulated - ES has neither in core - by the
    // tier ladder in MultiDraw.cpp, which owns the draw preparation too (its compute tier
    // has to dispatch before the draw state is established). The only difference between
    // them is whether the batch carries per-sub-draw base vertices.
    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        MultiDrawImpl::DrawElementsBatch(mode, count, type, indices, drawcount, nullptr);
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        MultiDrawImpl::DrawElementsBatch(mode, count, type, indices, drawcount, basevertex);
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
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawElementsIndirectCommand));
            return;
        }

        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: unsupported index type 0x%x", type);
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
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawElementsIndirectCommand));
            return;
        }

        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: unsupported index type 0x%x", type);
            return;
        }

        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        auto parameterBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
        if (!drawBuffer) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: no GL_DRAW_INDIRECT_BUFFER is bound");
            return;
        }
        if (!parameterBuffer) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: no GL_PARAMETER_BUFFER is bound");
            return;
        }

        drawBuffer->SyncPersistentMappedRange();
        parameterBuffer->SyncPersistentMappedRange();

        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset + static_cast<SizeT>(stride) * static_cast<SizeT>(maxdrawcount - 1) +
            sizeof(DrawElementsIndirectCommand);
        if (commandBytes > drawBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }
        if (drawcount < 0 || static_cast<SizeT>(drawcount) + sizeof(Uint32) > parameterBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
            return;
        }

        // Both counts are read from the CPU shadow, which a buffer with no shadow does not
        // have - MappedData() is null there and the reads below would be a null dereference,
        // not a wrong picture. The DirectVulkan twin declines the same way.
        if (parameterBuffer->MappedData() == nullptr || drawBuffer->MappedData() == nullptr) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: CPU fallback cannot read the parameter or "
                    "draw-indirect buffer");
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
            MGLOG_E_ONCE("MultiDrawArraysIndirect skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        DrawSyncFlags syncBit = DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
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

    // The non-indexed twin of MultiDrawElementsIndirectCount, and structurally identical to it:
    // ES has no GL_PARAMETER_BUFFER at all, so the draw count is read from the CPU shadow of the
    // bound one and the batch degenerates into an ordinary indirect multi-draw of that many
    // commands. Missing from the backend table until now, which made every
    // glMultiDrawArraysIndirectCount an INVALID_OPERATION ("backend does not support
    // indirect-parameter array draws") on DirectGLES while the extension was advertised.
    void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount,
                                      GLsizei stride) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        if (maxdrawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawArraysIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawArraysIndirectCommand))) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        DrawSyncFlags syncBit = DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        auto parameterBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
        if (!drawBuffer) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: no GL_DRAW_INDIRECT_BUFFER is bound");
            return;
        }
        if (!parameterBuffer) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: no GL_PARAMETER_BUFFER is bound");
            return;
        }

        drawBuffer->SyncPersistentMappedRange();
        parameterBuffer->SyncPersistentMappedRange();

        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset + static_cast<SizeT>(stride) * static_cast<SizeT>(maxdrawcount - 1) +
            sizeof(DrawArraysIndirectCommand);
        if (commandBytes > drawBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }
        if (drawcount < 0 || static_cast<SizeT>(drawcount) + sizeof(Uint32) > parameterBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
            return;
        }

        // See the indexed twin: no CPU shadow means no count to read, not a wrong one.
        if (parameterBuffer->MappedData() == nullptr || drawBuffer->MappedData() == nullptr) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: CPU fallback cannot read the parameter or "
                    "draw-indirect buffer");
            return;
        }

        Uint32 actualDrawCount = 0;
        std::memcpy(&actualDrawCount, parameterBuffer->MappedData() + drawcount, sizeof(actualDrawCount));
        actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
        ExecuteArraysIndirectCommands(mode, drawBuffer->MappedData() + commandOffset, commandOffset, drawBuffer,
                                      static_cast<GLsizei>(actualDrawCount), stride, "MultiDrawArraysIndirectCount");
    }

    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex) {
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        SetCurrentBaseVertex(basevertex);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
        });
        SetCurrentBaseVertex(0);
    }

    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawRangeElements(mode, start, end, count, type, indices);
        });
    }

    // True when the driver will apply baseInstance to the vertex fetch itself, in which case the
    // attribute-offset emulation must stay out of the way. SetCurrentBaseInstance is orthogonal
    // and runs either way - it feeds the shader's gl_BaseInstance, not the fetch.
    inline Bool UseNativeBaseInstance() {
        return g_GLESCapabilities.SupportsBaseInstance;
    }

    // The emulated shift has to be in place before PrepareForDraw, because that is what syncs the
    // VAO; a zero here is what un-shifts the arrays for the next ordinary draw.
    inline Uint32 EmulatedFetchBaseInstance(GLuint baseinstance) {
        return UseNativeBaseInstance() ? 0u : static_cast<Uint32>(baseinstance);
    }

    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        const VertexArrayImpl::ScopedFetchBaseInstance fetchScope(EmulatedFetchBaseInstance(baseinstance));
        PrepareForDraw(syncBit);
        SetCurrentBaseInstance(baseinstance);
        SetCurrentBaseVertex(basevertex);
        ForEachViewportRoutingPass([&] {
            if (UseNativeBaseInstance()) {
                g_GLESFuncs.glDrawElementsInstancedBaseVertexBaseInstanceEXT(mode, count, type, indices, instancecount,
                                                                            basevertex, baseinstance);
            } else {
                g_GLESFuncs.glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
            }
        });
        SetCurrentBaseVertex(0);
        SetCurrentBaseInstance(0);
    }

    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        SetCurrentBaseVertex(basevertex);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
        });
        SetCurrentBaseVertex(0);
    }

    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        const VertexArrayImpl::ScopedFetchBaseInstance fetchScope(EmulatedFetchBaseInstance(baseinstance));
        PrepareForDraw(syncBit);
        SetCurrentBaseInstance(baseinstance);
        ForEachViewportRoutingPass([&] {
            if (UseNativeBaseInstance()) {
                g_GLESFuncs.glDrawElementsInstancedBaseInstanceEXT(mode, count, type, indices, instancecount,
                                                                  baseinstance);
            } else {
                g_GLESFuncs.glDrawElementsInstanced(mode, count, type, indices, instancecount);
            }
        });
        SetCurrentBaseInstance(0);
    }

    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawElementsInstanced(mode, count, type, indices, instancecount);
        });
    }

    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("DrawElementsIndirect skipped: unsupported index type 0x%x", type);
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
        DrawSyncFlags syncBit = DrawSyncBit::Instancing;
        const VertexArrayImpl::ScopedFetchBaseInstance fetchScope(EmulatedFetchBaseInstance(baseinstance));
        PrepareForDraw(syncBit);
        SetCurrentBaseInstance(baseinstance);
        ForEachViewportRoutingPass([&] {
            if (UseNativeBaseInstance()) {
                g_GLESFuncs.glDrawArraysInstancedBaseInstanceEXT(mode, first, count, instancecount, baseinstance);
            } else {
                g_GLESFuncs.glDrawArraysInstanced(mode, first, count, instancecount);
            }
        });
        SetCurrentBaseInstance(0);
    }

    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        DrawSyncFlags syncBit = DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawArraysInstanced(mode, first, count, instancecount);
        });
    }

    void DrawArraysIndirect(GLenum mode, const void* indirect) {
        DrawSyncFlags syncBit = DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
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

    // Empties the ES driver's error queue, BOUNDED. A driver that never answers GL_NO_ERROR - a
    // lost context is the usual way, and GL_CONTEXT_LOST is allowed to keep coming back - would
    // otherwise spin an unbounded drain forever inside whichever GL entry point happened to be
    // cleaning up, which is how a GPU reset reads as an unkillable process whose log simply
    // stops. A healthy context cannot queue anywhere near the cap, so reaching it IS the
    // diagnostic. Every drain in this backend goes through here so the bound cannot drift apart
    // between them.
    static constexpr Int kMaxDrainedGLErrors = 32;

    static void DrainDriverErrors(const char* site) {
        Int drained = 0;
        while (drained < kMaxDrainedGLErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR) {
            ++drained;
        }
        if (drained == kMaxDrainedGLErrors) {
            MGLOG_E_ONCE("%s: the ES driver still reported errors after %d drains - the context is most likely lost",
                         site, kMaxDrainedGLErrors);
        }
    }

    static void DrainBlitErrors() { DrainDriverErrors("BlitFramebuffer"); }

    // Sized internal format of the currently bound READ framebuffer's read colour
    // attachment, 0 when it cannot be determined.
    static GLenum QueryReadColorAttachmentInternalFormat() {
        GLint attachmentType = 0;
        GLint attachmentName = 0;
        // Ask the point the backend read buffer actually names, not COLOR_ATTACHMENT0. The colour map
        // in BackendFramebufferObject is a permutation, so the read attachment's image only sits at
        // CA0 when that map is identity; querying CA0 unconditionally would size the resolve
        // renderbuffer from a different attachment's format and either convert wrongly or fail the
        // blit outright.
        GLint readBuffer = GL_COLOR_ATTACHMENT0;
        g_GLESFuncs.glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        if (readBuffer < GL_COLOR_ATTACHMENT0 || readBuffer > GL_COLOR_ATTACHMENT31) {
            readBuffer = GL_COLOR_ATTACHMENT0;
        }
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, static_cast<GLenum>(readBuffer),
                                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attachmentType);
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, static_cast<GLenum>(readBuffer),
                                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attachmentName);
        if (attachmentName == 0) {
            return 0;
        }
        GLint internalFormat = 0;
        if (attachmentType == GL_RENDERBUFFER) {
            if (!g_GLESFuncs.glGetRenderbufferParameteriv) return 0;
            GLint previous = 0;
            g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(attachmentName));
            g_GLESFuncs.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT,
                                                     &internalFormat);
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous));
        } else if (attachmentType == GL_TEXTURE) {
            if (!g_GLESFuncs.glGetTexLevelParameteriv) return 0;
            // The resolve source of interest is always a multisample 2D texture; a
            // single-sample source would not have taken the fallback in the first place.
            GLint previous = 0;
            g_GLESFuncs.glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &previous);
            g_GLESFuncs.glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, static_cast<GLuint>(attachmentName));
            g_GLESFuncs.glGetTexLevelParameteriv(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_INTERNAL_FORMAT,
                                                 &internalFormat);
            g_GLESFuncs.glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, static_cast<GLuint>(previous));
        }
        return static_cast<GLenum>(internalFormat);
    }

    // ES rejects any blit out of a multisample read framebuffer whose format differs
    // from the draw framebuffer's. Desktop GL only requires identical formats when BOTH
    // framebuffers are multisampled, so a multisample resolve is allowed to convert
    // format on the way out (KHR-GL3x.framebuffer_blit resolves an R8/R16F multisample
    // texture straight into an RGBA8 target). Emulate it in two steps: resolve into a
    // scratch buffer of the source's own format, then run the caller's blit from there -
    // that second one is single-sample on both sides, where ES does allow conversion.
    static Bool ResolveThenBlit(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                GLint dstX1, GLint dstY1, GLenum filter) {
        static Uint s_resolveFramebuffer = 0;
        static Uint s_resolveRenderbuffer = 0;
        static GLenum s_resolveFormat = 0;
        static GLsizei s_resolveWidth = 0;
        static GLsizei s_resolveHeight = 0;
        static Uint s_resolveContextGeneration = ~0u;

        if (!g_GLESFuncs.glGenFramebuffers || !g_GLESFuncs.glGenRenderbuffers ||
            !g_GLESFuncs.glRenderbufferStorage || !g_GLESFuncs.glFramebufferRenderbuffer) {
            return false;
        }
        const GLenum sourceFormat = QueryReadColorAttachmentInternalFormat();
        if (sourceFormat == 0) {
            return false;
        }

        const GLint left = std::min(srcX0, srcX1);
        const GLint right = std::max(srcX0, srcX1);
        const GLint bottom = std::min(srcY0, srcY1);
        const GLint top = std::max(srcY0, srcY1);
        const GLsizei width = static_cast<GLsizei>(right - left);
        const GLsizei height = static_cast<GLsizei>(top - bottom);
        if (width <= 0 || height <= 0) {
            return false;
        }

        if (s_resolveContextGeneration != g_backendContextGeneration) {
            // The ids belonged to a dead context; the context reclaimed them with it.
            s_resolveFramebuffer = 0;
            s_resolveRenderbuffer = 0;
            s_resolveFormat = 0;
            s_resolveContextGeneration = g_backendContextGeneration;
        }
        if (s_resolveFramebuffer == 0) {
            g_GLESFuncs.glGenFramebuffers(1, &s_resolveFramebuffer);
            g_GLESFuncs.glGenRenderbuffers(1, &s_resolveRenderbuffer);
            if (s_resolveFramebuffer == 0 || s_resolveRenderbuffer == 0) return false;
            s_resolveFormat = 0;
        }

        GLint previousRenderbuffer = 0;
        g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
        if (s_resolveFormat != sourceFormat || s_resolveWidth < width || s_resolveHeight < height) {
            s_resolveWidth = std::max(s_resolveWidth, width);
            s_resolveHeight = std::max(s_resolveHeight, height);
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, s_resolveRenderbuffer);
            g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, sourceFormat, s_resolveWidth, s_resolveHeight);
            s_resolveFormat = sourceFormat;
        }
        g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));

        GLint previousDraw = 0;
        GLint previousRead = 0;
        g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
        g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);

        g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_resolveFramebuffer);
        g_GLESFuncs.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                              s_resolveRenderbuffer);
        Bool resolved = g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (resolved) {
            DrainBlitErrors();
            // A blit is scissored like a draw (the replicate path's guard documents the
            // same rule): the application's box would clip this resolve into the
            // scratch, and the second blit would then copy never-written scratch texels
            // into the destination - silently, since scissor clipping raises no GL
            // error. Disable for the staging blit only; the caller-visible blit below
            // keeps the blit's native scissor semantics. Tracked via the render-state
            // shadow, exactly like ScopedScissorDisable.
            const Bool scissorWasEnabled =
                (RenderStateImpl::g_syncedRenderStateParameters.ScissorTestEnabledMask & 1u) != 0;
            if (scissorWasEnabled) g_GLESFuncs.glDisable(GL_SCISSOR_TEST);
            g_GLESFuncs.glBlitFramebuffer(left, bottom, right, top, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                                          GL_NEAREST);
            if (scissorWasEnabled) g_GLESFuncs.glEnable(GL_SCISSOR_TEST);
            resolved = g_GLESFuncs.glGetError() == GL_NO_ERROR;
        }
        if (resolved) {
            // Mirror the caller's orientation into the scratch-relative source rect so a
            // flipped blit stays flipped.
            const GLint blitX0 = srcX0 <= srcX1 ? 0 : width;
            const GLint blitX1 = srcX0 <= srcX1 ? width : 0;
            const GLint blitY0 = srcY0 <= srcY1 ? 0 : height;
            const GLint blitY1 = srcY0 <= srcY1 ? height : 0;
            g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, s_resolveFramebuffer);
            g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
            DrainBlitErrors();
            g_GLESFuncs.glBlitFramebuffer(blitX0, blitY0, blitX1, blitY1, dstX0, dstY0, dstX1, dstY1,
                                          GL_COLOR_BUFFER_BIT, filter);
            resolved = g_GLESFuncs.glGetError() == GL_NO_ERROR;
        }

        g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
        g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
        FramebufferImpl::InvalidateFramebufferBindingCache();
        if (!resolved) {
            MGLOG_E_ONCE("BlitFramebuffer: multisample resolve fallback failed");
        }
        return resolved;
    }

    // ---------------------------------------------------------------------------------
    // Shared guard for the emulation passes that have to DRAW to get their work done: the
    // single-sample -> multisample blit replicate below, and the depth/stencil readback
    // emulation further down. Both borrow the application's ES context for a full-screen
    // pass, so everything they disturb is captured here and put back on the way out - the
    // sync layer's shadow of the driver state has to stay true, and one leaked binding
    // regresses every draw that follows.
    //
    // Three members of that set are not obvious:
    //
    //  - The borrowed texture unit. Both passes bind their scratch texture to
    //    TextureImpl::TempTextureUnit, and the unit's binding is only restored correctly by
    //    asking TextureImpl::g_boundTexturesCache what is supposed to be there: the raw
    //    glBindTexture the passes issue never moves that cache, so restoring a *queried*
    //    id leaves the driver and the cache disagreeing and the per-draw binding memo
    //    false-skips the re-bind - the borrowed-slot failure mode that showed up as
    //    process-wide glyph death under Iris. Reading GL_TEXTURE_BINDING_2D was doubly
    //    wrong here because the replicate path may already have switched units and bound
    //    its own scratch texture by the time it asked.
    //
    //  - The sampler object on that unit. It would override the scratch texture's own
    //    filter and compare parameters. Unbinding through SamplerImpl::UnbindSampler moves
    //    the sampler cache, which is exactly what re-opens BindCurrentUnitSamplers' memo,
    //    so the application's sampler comes back on the next draw with no explicit restore.
    //
    //  - An active transform feedback capture. GL rejects a draw issued with a program
    //    other than the one that began the capture, and would otherwise append the
    //    emulation's vertices to the application's buffers.
    class ScopedEmulationDrawState {
    public:
        ScopedEmulationDrawState() {
            g_GLESFuncs.glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
            g_GLESFuncs.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vertexArray);
            g_GLESFuncs.glGetIntegerv(GL_VIEWPORT, m_viewport);
            g_GLESFuncs.glGetIntegerv(GL_SCISSOR_BOX, m_scissorBox);
            g_GLESFuncs.glGetBooleanv(GL_COLOR_WRITEMASK, m_colorMask);
            g_GLESFuncs.glGetIntegerv(GL_DEPTH_FUNC, &m_depthFunc);
            g_GLESFuncs.glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthMask);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_FUNC, &m_stencilFunc[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_FUNC, &m_stencilFunc[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_REF, &m_stencilRef[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_REF, &m_stencilRef[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_VALUE_MASK, &m_stencilValueMask[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK, &m_stencilValueMask[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_WRITEMASK, &m_stencilWriteMask[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &m_stencilWriteMask[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_FAIL, &m_stencilFail[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_FAIL, &m_stencilFail[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &m_stencilDepthFail[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL, &m_stencilDepthFail[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &m_stencilPass[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &m_stencilPass[1]);
            if (g_GLESCapabilities.SupportsClipDistance) {
                m_capabilityCount = kBaseCapabilityCount + kClipDistanceCapabilityCount;
            }
            for (Uint i = 0; i < m_capabilityCount; ++i) {
                m_capabilities[i].enabled = g_GLESFuncs.glIsEnabled(m_capabilities[i].cap);
            }
            // GL_SAMPLE_MASK is ES 3.1; on an older driver the query above just raised
            // GL_INVALID_ENUM and answered GL_FALSE, which is also the right thing to
            // restore. Drop the flag so it is not misattributed to the emulation's own work.
            DrainBlitErrors();

            if (MG_State::pGLContext->IsTransformFeedbackActive() &&
                !MG_State::pGLContext->IsTransformFeedbackPaused() && g_GLESFuncs.glPauseTransformFeedback) {
                g_GLESFuncs.glPauseTransformFeedback();
                m_pausedTransformFeedback = true;
                DrainBlitErrors();
            }

            m_activeTextureUnit = TextureImpl::g_activeTextureUnit;
            TextureImpl::ActivateTextureUnit(TextureImpl::TempTextureUnit);
            SamplerImpl::UnbindSampler(TextureImpl::TempTextureUnit);

            // The neutral baseline every emulation pass wants: nothing culled, nothing
            // clipped, nothing tested, no coverage games, and colour writes open. Callers
            // turn back on only what they need (the replicate pass wants the depth and
            // stencil tests, and masks colour off because it writes neither).
            for (Uint i = 0; i < m_capabilityCount; ++i) {
                g_GLESFuncs.glDisable(m_capabilities[i].cap);
            }
            g_GLESFuncs.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            g_GLESFuncs.glDepthMask(GL_FALSE);
            DrainBlitErrors();
        }

        ~ScopedEmulationDrawState() {
            g_GLESFuncs.glUseProgram(static_cast<GLuint>(m_program));
            // Put back whatever the binding cache says lives on the borrowed unit, not what
            // the driver happened to hold: see the class comment.
            auto* cachedBound =
                TextureImpl::g_boundTexturesCache[TextureImpl::TempTextureUnit]
                                                 [static_cast<SizeT>(TextureTarget::Texture2D)];
            g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, cachedBound ? cachedBound->GetBackendTextureId() : 0);
            TextureImpl::ActivateTextureUnit(m_activeTextureUnit);
            VertexArrayImpl::BindBackendVAOId(static_cast<GLuint>(m_vertexArray));
            g_GLESFuncs.glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
            g_GLESFuncs.glScissor(m_scissorBox[0], m_scissorBox[1], m_scissorBox[2], m_scissorBox[3]);
            g_GLESFuncs.glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);
            g_GLESFuncs.glDepthFunc(static_cast<GLenum>(m_depthFunc));
            g_GLESFuncs.glDepthMask(m_depthMask);
            const GLenum faces[2] = {GL_FRONT, GL_BACK};
            for (SizeT face = 0; face < 2; ++face) {
                g_GLESFuncs.glStencilFuncSeparate(faces[face], static_cast<GLenum>(m_stencilFunc[face]),
                                                  m_stencilRef[face],
                                                  static_cast<GLuint>(m_stencilValueMask[face]));
                g_GLESFuncs.glStencilOpSeparate(faces[face], static_cast<GLenum>(m_stencilFail[face]),
                                                static_cast<GLenum>(m_stencilDepthFail[face]),
                                                static_cast<GLenum>(m_stencilPass[face]));
                g_GLESFuncs.glStencilMaskSeparate(faces[face], static_cast<GLuint>(m_stencilWriteMask[face]));
            }
            for (Uint i = 0; i < m_capabilityCount; ++i) {
                if (m_capabilities[i].enabled) {
                    g_GLESFuncs.glEnable(m_capabilities[i].cap);
                } else {
                    g_GLESFuncs.glDisable(m_capabilities[i].cap);
                }
            }
            // The per-draw-buffer colour masks are not covered by the non-indexed
            // glColorMask above. Restore what the SYNC actually pushed, not the raw
            // application masks: a widened attachment's alpha write is forced off by
            // SyncRenderState and memoized in g_syncedColorMaskAlphaWidenMask, and the
            // next sync early-outs on an unchanged version - restoring the undoctored
            // mask here would leave alpha writes enabled on the widened buffer with
            // nothing left to repair it. Same three-way pointer fallback as
            // SyncRenderState's push: gating on the core name alone left EXT/OES-only
            // devices holding buffer 0's mask broadcast across every buffer.
            const auto colorMaskiFn = g_GLESFuncs.glColorMaski      ? g_GLESFuncs.glColorMaski
                                      : g_GLESFuncs.glColorMaskiEXT ? g_GLESFuncs.glColorMaskiEXT
                                                                    : g_GLESFuncs.glColorMaskiOES;
            if (colorMaskiFn) {
                for (Uint index = 0; index < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS; ++index) {
                    BoolVec4 colorMask = RenderStateImpl::g_syncedRenderStateParameters.ColorMasks[index];
                    if (index < 32 && (RenderStateImpl::g_syncedColorMaskAlphaWidenMask & (1u << index)) != 0) {
                        colorMask.w() = false;
                    }
                    colorMaskiFn(index, colorMask.x() ? GL_TRUE : GL_FALSE, colorMask.y() ? GL_TRUE : GL_FALSE,
                                 colorMask.z() ? GL_TRUE : GL_FALSE, colorMask.w() ? GL_TRUE : GL_FALSE);
                }
            }
            if (m_pausedTransformFeedback && g_GLESFuncs.glResumeTransformFeedback) {
                g_GLESFuncs.glResumeTransformFeedback();
            }
            DrainBlitErrors();
        }

        ScopedEmulationDrawState(const ScopedEmulationDrawState&) = delete;
        ScopedEmulationDrawState& operator=(const ScopedEmulationDrawState&) = delete;

    private:
        struct CapabilityState {
            GLenum cap;
            GLboolean enabled;
        };

        GLint m_program = 0;
        GLint m_vertexArray = 0;
        GLint m_viewport[4] = {0, 0, 0, 0};
        GLint m_scissorBox[4] = {0, 0, 0, 0};
        GLboolean m_colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        GLint m_depthFunc = GL_LESS;
        GLboolean m_depthMask = GL_TRUE;
        GLint m_stencilFunc[2] = {GL_ALWAYS, GL_ALWAYS};
        GLint m_stencilRef[2] = {0, 0};
        GLint m_stencilValueMask[2] = {~0, ~0};
        GLint m_stencilWriteMask[2] = {~0, ~0};
        GLint m_stencilFail[2] = {GL_KEEP, GL_KEEP};
        GLint m_stencilDepthFail[2] = {GL_KEEP, GL_KEEP};
        GLint m_stencilPass[2] = {GL_KEEP, GL_KEEP};
        Uint m_activeTextureUnit = 0;
        Bool m_pausedTransformFeedback = false;
        // The eight GL_CLIP_DISTANCE0_EXT..7_EXT entries are last so that a driver without
        // GL_EXT_clip_cull_distance can be served by shortening the count instead of asking
        // it about tokens it does not know. They belong here at all because an emulation pass
        // draws its full-screen triangle with its OWN program, which writes no gl_ClipDistance:
        // leaving the app's enables on would clip that triangle by undefined distances.
        static constexpr Uint kBaseCapabilityCount = 10;
        static constexpr Uint kClipDistanceCapabilityCount = 8;
        Uint m_capabilityCount = kBaseCapabilityCount;
        CapabilityState m_capabilities[kBaseCapabilityCount + kClipDistanceCapabilityCount] = {
            {GL_SCISSOR_TEST, GL_FALSE},        {GL_DEPTH_TEST, GL_FALSE},
            {GL_STENCIL_TEST, GL_FALSE},        {GL_CULL_FACE, GL_FALSE},
            {GL_BLEND, GL_FALSE},               {GL_RASTERIZER_DISCARD, GL_FALSE},
            {GL_POLYGON_OFFSET_FILL, GL_FALSE}, {GL_SAMPLE_ALPHA_TO_COVERAGE, GL_FALSE},
            {GL_SAMPLE_COVERAGE, GL_FALSE},     {GL_SAMPLE_MASK, GL_FALSE},
            {0x3000, GL_FALSE},                 {0x3001, GL_FALSE},
            {0x3002, GL_FALSE},                 {0x3003, GL_FALSE},
            {0x3004, GL_FALSE},                 {0x3005, GL_FALSE},
            {0x3006, GL_FALSE},                 {0x3007, GL_FALSE},
        };
    };

    // ---------------------------------------------------------------------------------
    // Single-sample -> multisample blit ("replicate")
    //
    // Desktop GL replicates the source sample into every destination sample when the read
    // framebuffer is single-sampled and the draw framebuffer is not. ES forbids the whole
    // call ("INVALID_OPERATION if SAMPLE_BUFFERS for the draw framebuffer is greater than
    // zero"), so the blit silently did nothing - KHR-GL3x.packed_depth_stencil.blit's
    // second loop then read a destination that still held its clear values.
    //
    // Emulated by drawing a full-screen triangle into the multisample framebuffer: every
    // pixel is fully covered, so every sample of it receives the same value, which is
    // exactly what the replicate rule asks for. Depth comes from gl_FragDepth; stencil has
    // no shader output on ES, so it is written one bit plane at a time with REPLACE and a
    // discard for the pixels whose source bit is clear.
    // Defined further down with the other small GL helpers; the attachment-format probe below
    // needs it to tell a rejected bind apart from a successful one.
    static void ClearGLErrors();

    namespace ReplicateBlitImpl {
        static Uint s_contextGeneration = ~0u;
        static GLuint s_framebuffer = 0;
        static GLuint s_texture = 0;
        static GLenum s_textureFormat = 0;
        static GLsizei s_textureWidth = 0;
        static GLsizei s_textureHeight = 0;
        static GLuint s_vertexArray = 0;
        static GLuint s_depthProgram = 0;
        static GLuint s_stencilProgram = 0;
        static GLint s_depthUvTransform = -1;
        static GLint s_stencilUvTransform = -1;
        static GLint s_stencilBit = -1;
        static Bool s_programsFailed = false;

        static const char* const kVertexSource =
            "#version 300 es\n"
            "uniform vec4 uUvTransform;\n"
            "out vec2 vUv;\n"
            "void main() {\n"
            "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "    vUv = p * uUvTransform.xy + uUvTransform.zw;\n"
            "}\n";

        static const char* const kDepthFragmentSource =
            "#version 300 es\n"
            "precision highp float;\n"
            "precision highp sampler2D;\n"
            "uniform sampler2D uSource;\n"
            "in vec2 vUv;\n"
            "void main() {\n"
            "    gl_FragDepth = texture(uSource, vUv).r;\n"
            "}\n";

        static const char* const kStencilFragmentSource =
            "#version 300 es\n"
            "precision highp float;\n"
            "precision highp usampler2D;\n"
            "uniform usampler2D uSource;\n"
            "uniform uint uBit;\n"
            "in vec2 vUv;\n"
            "void main() {\n"
            "    if ((texture(uSource, vUv).r & uBit) == 0u) discard;\n"
            "}\n";

        static GLuint BuildProgram(const char* fragmentSource) {
            const GLuint vertexShader = g_GLESFuncs.glCreateShader(GL_VERTEX_SHADER);
            const GLuint fragmentShader = g_GLESFuncs.glCreateShader(GL_FRAGMENT_SHADER);
            if (vertexShader == 0 || fragmentShader == 0) {
                return 0;
            }
            g_GLESFuncs.glShaderSource(vertexShader, 1, &kVertexSource, nullptr);
            g_GLESFuncs.glCompileShader(vertexShader);
            g_GLESFuncs.glShaderSource(fragmentShader, 1, &fragmentSource, nullptr);
            g_GLESFuncs.glCompileShader(fragmentShader);

            const GLuint program = g_GLESFuncs.glCreateProgram();
            GLint linked = GL_FALSE;
            if (program != 0) {
                g_GLESFuncs.glAttachShader(program, vertexShader);
                g_GLESFuncs.glAttachShader(program, fragmentShader);
                g_GLESFuncs.glLinkProgram(program);
                g_GLESFuncs.glGetProgramiv(program, GL_LINK_STATUS, &linked);
            }
            g_GLESFuncs.glDeleteShader(vertexShader);
            g_GLESFuncs.glDeleteShader(fragmentShader);
            if (linked != GL_TRUE) {
                if (program != 0) g_GLESFuncs.glDeleteProgram(program);
                return 0;
            }
            return program;
        }

        static Bool EnsureResources() {
            if (s_contextGeneration != g_backendContextGeneration) {
                // The ids belonged to a dead context; the context reclaimed them with it.
                s_framebuffer = 0;
                s_texture = 0;
                s_textureFormat = 0;
                s_textureWidth = 0;
                s_textureHeight = 0;
                s_vertexArray = 0;
                s_depthProgram = 0;
                s_stencilProgram = 0;
                s_programsFailed = false;
                s_contextGeneration = g_backendContextGeneration;
            }
            if (s_programsFailed) {
                return false;
            }
            if (s_depthProgram == 0) {
                s_depthProgram = BuildProgram(kDepthFragmentSource);
                s_stencilProgram = BuildProgram(kStencilFragmentSource);
                if (s_depthProgram == 0 || s_stencilProgram == 0) {
                    s_programsFailed = true;
                    MGLOG_E_ONCE("BlitFramebuffer: could not build the multisample replicate programs");
                    return false;
                }
                s_depthUvTransform = g_GLESFuncs.glGetUniformLocation(s_depthProgram, "uUvTransform");
                s_stencilUvTransform = g_GLESFuncs.glGetUniformLocation(s_stencilProgram, "uUvTransform");
                s_stencilBit = g_GLESFuncs.glGetUniformLocation(s_stencilProgram, "uBit");
            }
            if (s_framebuffer == 0) {
                g_GLESFuncs.glGenFramebuffers(1, &s_framebuffer);
                if (s_framebuffer == 0) return false;
            }
            if (s_vertexArray == 0) {
                g_GLESFuncs.glGenVertexArrays(1, &s_vertexArray);
                if (s_vertexArray == 0) return false;
            }
            return true;
        }

        // Sized internal format of ONE attachment point of the bound READ framebuffer, or 0
        // when there is no object there to ask (the default framebuffer's buffers have no
        // queryable format at all). The scratch copy has to use the very same one: ES rejects
        // a depth/stencil blit between differing formats even when both sides are
        // single-sampled.
        //
        // The texture branch can only ask about a GL_TEXTURE_2D, and a name whose target is
        // something else (an array or cube texture attached by glFramebufferTextureLayer /
        // glFramebufferTexture) makes glBindTexture answer GL_INVALID_OPERATION and change
        // nothing. Reading glGetTexLevelParameteriv after that failed bind does NOT return 0 -
        // it truthfully describes whatever texture was already on GL_TEXTURE_2D, which on this
        // path is the emulation's own staging scratch. That is a wrong answer that looks like a
        // right one, so the bind has to be error-checked rather than trusted.
        static GLenum QueryAttachmentSizedFormat(GLenum attachment) {
            GLint objectType = 0;
            GLint objectName = 0;
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objectName);
            if (objectName == 0) {
                return 0;
            }
            GLint internalFormat = 0;
            if (objectType == GL_RENDERBUFFER) {
                GLint previous = 0;
                g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
                ClearGLErrors();
                g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(objectName));
                const Bool bound = g_GLESFuncs.glGetError() == GL_NO_ERROR;
                if (bound) {
                    g_GLESFuncs.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT,
                                                             &internalFormat);
                }
                g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous));
                ClearGLErrors();
            } else if (objectType == GL_TEXTURE) {
                GLint previous = 0;
                g_GLESFuncs.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
                ClearGLErrors();
                g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(objectName));
                const Bool bound = g_GLESFuncs.glGetError() == GL_NO_ERROR;
                if (bound) {
                    g_GLESFuncs.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                                                         &internalFormat);
                    if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                        internalFormat = 0;
                    }
                }
                g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
                ClearGLErrors();
            }
            return static_cast<GLenum>(internalFormat);
        }

        // The read framebuffer's depth format, or - when it has no depth - its stencil one.
        static GLenum QueryReadDepthStencilFormat(GLenum* outAttachment) {
            const GLenum attachments[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
            for (const GLenum attachment : attachments) {
                const GLenum internalFormat = QueryAttachmentSizedFormat(attachment);
                if (internalFormat != 0) {
                    if (outAttachment) *outAttachment = attachment;
                    return internalFormat;
                }
            }
            return 0;
        }

        static Bool FormatHasDepth(GLenum internalFormat) {
            return internalFormat == GL_DEPTH_COMPONENT16 || internalFormat == GL_DEPTH_COMPONENT24 ||
                   internalFormat == GL_DEPTH_COMPONENT32F || internalFormat == GL_DEPTH24_STENCIL8 ||
                   internalFormat == GL_DEPTH32F_STENCIL8;
        }

        static Bool FormatHasStencil(GLenum internalFormat) {
            return internalFormat == GL_DEPTH24_STENCIL8 || internalFormat == GL_DEPTH32F_STENCIL8 ||
                   internalFormat == GL_STENCIL_INDEX8;
        }

        static GLenum ScratchAttachmentFor(GLenum internalFormat) {
            if (FormatHasDepth(internalFormat) && FormatHasStencil(internalFormat)) {
                return GL_DEPTH_STENCIL_ATTACHMENT;
            }
            return FormatHasDepth(internalFormat) ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT;
        }
    } // namespace ReplicateBlitImpl

    // Returns true when the request was serviced (or is not this fallback's business).
    static Bool ReplicateBlitIntoMultisampleDraw(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                                                 GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask) {
        using namespace ReplicateBlitImpl;
        if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) == 0) {
            return false;
        }
        if (!EnsureResources()) {
            return false;
        }

        const GLsizei srcWidth = static_cast<GLsizei>(std::abs(srcX1 - srcX0));
        const GLsizei srcHeight = static_cast<GLsizei>(std::abs(srcY1 - srcY0));
        const GLsizei dstWidth = static_cast<GLsizei>(std::abs(dstX1 - dstX0));
        const GLsizei dstHeight = static_cast<GLsizei>(std::abs(dstY1 - dstY0));
        if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) {
            return false;
        }

        GLenum readAttachment = GL_DEPTH_ATTACHMENT;
        const GLenum sourceFormat = QueryReadDepthStencilFormat(&readAttachment);
        if (sourceFormat == 0) {
            return false;
        }
        const Bool wantDepth = (mask & GL_DEPTH_BUFFER_BIT) != 0 && FormatHasDepth(sourceFormat);
        const Bool wantStencil = (mask & GL_STENCIL_BUFFER_BIT) != 0 && FormatHasStencil(sourceFormat);
        if (!wantDepth && !wantStencil) {
            return false;
        }
        // Sampling the stencil half of a packed texture goes through
        // GL_DEPTH_STENCIL_TEXTURE_MODE, which is ES 3.1 state; on an older driver the pname
        // would just raise GL_INVALID_ENUM and the shader would read depth bits as stencil.
        const Bool supportsStencilTextureMode = g_GLESCapabilities.GLESVersion.Major > 3 ||
                                                (g_GLESCapabilities.GLESVersion.Major == 3 &&
                                                 g_GLESCapabilities.GLESVersion.Minor >= 1);
        if (wantStencil && !supportsStencilTextureMode) {
            return false;
        }

        GLint previousDraw = 0;
        GLint previousRead = 0;
        g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
        g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);

        // Everything below borrows the application's context - a texture unit for the
        // scratch sampling and the whole rasterization pipeline for the replicate passes -
        // so the guard is taken before the first of those, not just before the draws. It
        // also puts the scissor test where the staging blit below needs it: a blit is
        // scissored like a draw, and the application's box would otherwise clip the copy
        // into the scratch texture.
        ScopedEmulationDrawState emulationState;

        // Copy the source rectangle into a scratch texture of its own format: both sides of
        // that blit are single-sampled, which ES does allow.
        Bool ok = true;
        if (s_texture == 0 || s_textureFormat != sourceFormat || s_textureWidth < srcWidth ||
            s_textureHeight < srcHeight) {
            if (s_texture != 0) {
                g_GLESFuncs.glDeleteTextures(1, &s_texture); // immutable storage cannot be resized
                s_texture = 0;
            }
            s_textureWidth = std::max(s_textureWidth, srcWidth);
            s_textureHeight = std::max(s_textureHeight, srcHeight);
            g_GLESFuncs.glGenTextures(1, &s_texture);
            if (s_texture == 0) {
                ok = false;
            } else {
                // The guard already activated TempTextureUnit and cleared its sampler.
                g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, s_texture);
                DrainBlitErrors();
                g_GLESFuncs.glTexStorage2D(GL_TEXTURE_2D, 1, sourceFormat, s_textureWidth, s_textureHeight);
                ok = g_GLESFuncs.glGetError() == GL_NO_ERROR;
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                s_textureFormat = ok ? sourceFormat : 0;
            }
        }

        if (ok) {
            g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_framebuffer);
            g_GLESFuncs.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, ScratchAttachmentFor(sourceFormat), GL_TEXTURE_2D,
                                               s_texture, 0);
            ok = g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        if (ok) {
            const GLint left = std::min(srcX0, srcX1);
            const GLint bottom = std::min(srcY0, srcY1);
            DrainBlitErrors();
            g_GLESFuncs.glBlitFramebuffer(left, bottom, left + srcWidth, bottom + srcHeight, 0, 0, srcWidth, srcHeight,
                                          mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT), GL_NEAREST);
            ok = g_GLESFuncs.glGetError() == GL_NO_ERROR;
        }

        g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
        g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
        FramebufferImpl::InvalidateFramebufferBindingCache();
        if (!ok) {
            MGLOG_E_ONCE("BlitFramebuffer: could not stage the source for the multisample replicate");
            return false;
        }

        const GLint dstLeft = std::min(dstX0, dstX1);
        const GLint dstBottom = std::min(dstY0, dstY1);
        const Bool mirrorX = (srcX1 > srcX0) != (dstX1 > dstX0);
        const Bool mirrorY = (srcY1 > srcY0) != (dstY1 > dstY0);
        // The scratch holds the source rectangle at its origin, so the texture is larger than
        // the copied region: scale the [0,1] quad coordinates down to the region it occupies.
        const Float uvScaleX = static_cast<Float>(srcWidth) / static_cast<Float>(s_textureWidth);
        const Float uvScaleY = static_cast<Float>(srcHeight) / static_cast<Float>(s_textureHeight);
        const Float uvTransform[4] = {mirrorX ? -uvScaleX : uvScaleX, mirrorY ? -uvScaleY : uvScaleY,
                                      mirrorX ? uvScaleX : 0.0f, mirrorY ? uvScaleY : 0.0f};

        // The guard already left the pipeline neutral (nothing culled, tested, blended or
        // coverage-masked) on the borrowed texture unit; what is left is this pass's own
        // choices - the destination rectangle, and colour writes off because it writes only
        // depth and stencil.
        VertexArrayImpl::BindBackendVAOId(s_vertexArray);
        g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, s_texture);
        g_GLESFuncs.glViewport(dstLeft, dstBottom, dstWidth, dstHeight);
        g_GLESFuncs.glScissor(dstLeft, dstBottom, dstWidth, dstHeight);
        g_GLESFuncs.glEnable(GL_SCISSOR_TEST);
        g_GLESFuncs.glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        DrainBlitErrors();

        if (wantDepth) {
            if (FormatHasStencil(sourceFormat)) {
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
            }
            g_GLESFuncs.glUseProgram(s_depthProgram);
            g_GLESFuncs.glUniform4f(s_depthUvTransform, uvTransform[0], uvTransform[1], uvTransform[2],
                                    uvTransform[3]);
            g_GLESFuncs.glEnable(GL_DEPTH_TEST);
            g_GLESFuncs.glDepthFunc(GL_ALWAYS);
            g_GLESFuncs.glDepthMask(GL_TRUE);
            g_GLESFuncs.glDisable(GL_STENCIL_TEST);
            g_GLESFuncs.glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        if (wantStencil) {
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
            g_GLESFuncs.glUseProgram(s_stencilProgram);
            g_GLESFuncs.glUniform4f(s_stencilUvTransform, uvTransform[0], uvTransform[1], uvTransform[2],
                                    uvTransform[3]);
            g_GLESFuncs.glDisable(GL_DEPTH_TEST);
            g_GLESFuncs.glDepthMask(GL_FALSE);
            g_GLESFuncs.glEnable(GL_STENCIL_TEST);
            // The bit planes are written by ORing in the set bits, so the destination has to
            // start from zero. The blit overwrites the whole rectangle anyway, and the scissor
            // keeps the clear inside it.
            const GLint zero = 0;
            g_GLESFuncs.glStencilMask(0xFFu);
            g_GLESFuncs.glClearBufferiv(GL_STENCIL, 0, &zero);
            g_GLESFuncs.glStencilFunc(GL_ALWAYS, 0xFF, 0xFFu);
            g_GLESFuncs.glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            for (Uint bit = 0; bit < 8; ++bit) {
                g_GLESFuncs.glStencilMask(1u << bit);
                g_GLESFuncs.glUniform1ui(s_stencilBit, 1u << bit);
                g_GLESFuncs.glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
        }

        const Bool replicated = g_GLESFuncs.glGetError() == GL_NO_ERROR;

        // Everything the pass disturbed goes back through emulationState's destructor.
        if (!replicated) {
            MGLOG_E_ONCE("BlitFramebuffer: multisample replicate fallback failed");
        }
        return replicated;
    }

    static void IssueBlitWithResolveFallback(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                                             GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        DrainBlitErrors();
        g_GLESFuncs.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        if (g_GLESFuncs.glGetError() == GL_NO_ERROR) {
            return;
        }
        // Two ES restrictions desktop GL does not have are worth a second attempt: a
        // multisample resolve that also converts colour format, and any blit into a
        // multisample draw framebuffer. Both need to know how the two sides are sampled.
        GLint readSamples = 0;
        GLint drawSamples = 0;
        {
            GLint previousRead = 0;
            g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
            GLint previousDraw = 0;
            g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
            g_GLESFuncs.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousRead));
            g_GLESFuncs.glGetIntegerv(GL_SAMPLES, &readSamples);
            g_GLESFuncs.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
            g_GLESFuncs.glGetIntegerv(GL_SAMPLES, &drawSamples);
            g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
            g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
            FramebufferImpl::InvalidateFramebufferBindingCache();
        }
        if (readSamples <= 0 && drawSamples > 0) {
            // Single-sample source into a multisample destination: ES rejects the call
            // outright, desktop GL replicates the source sample into every destination one.
            if (ReplicateBlitIntoMultisampleDraw(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask)) {
                if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
                    MGLOG_E_ONCE("BlitFramebuffer: colour replicate into a multisample draw framebuffer is not emulated");
                }
            }
            return;
        }
        // The combined call raised an error, so by GL 4.6 2.3.1 it wrote nothing at all: BOTH
        // aspect groups still owe their copy, and each has to be retried on its own. Re-issuing
        // the depth/stencil half only as a rider on a SUCCESSFUL colour resolve dropped it
        // silently whenever the colour half could not be emulated - and on a framebuffer whose
        // only attachment is depth it never can, because the colour emulation has no attachment
        // to take a format from (KHR-GL33.framebuffer_blit's depth config test blits
        // COLOR|DEPTH|STENCIL across depth-only framebuffers and kept reading the clear value).
        const GLbitfield colourBit = mask & static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
        const GLbitfield dsBits = mask & static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        // The colour group's one emulation is the multisample resolve that also converts format,
        // which is the shape this names. It used to double as an early-out for the whole
        // function, which is what cost a depth-only mask its single-aspect retry.
        const Bool multisampleResolve = readSamples > 0 && drawSamples <= 0;
        if (colourBit != 0) {
            DrainBlitErrors();
            g_GLESFuncs.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, colourBit, filter);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                const Bool emulated =
                    multisampleResolve &&
                    ResolveThenBlit(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, filter);
                if (!emulated) {
                    MGLOG_E_ONCE("BlitFramebuffer: the colour aspect was dropped - the driver rejected it on its "
                                 "own and no emulation applies");
                }
            }
        }
        if (dsBits != 0) {
            DrainBlitErrors();
            g_GLESFuncs.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, dsBits, filter);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                // Nothing to fall back on yet: ResolveThenBlit is colour-only and the replicate
                // pass runs in the opposite direction, so a driver that declines a multisample
                // depth/stencil resolve leaves the destination holding its clear value. The log
                // is the whole diagnostic - the frontend performs no validation of its own, so
                // this never reaches the application as a GL error.
                MGLOG_E_ONCE("BlitFramebuffer: the depth/stencil aspect was dropped - the driver rejected it on "
                             "its own and no emulation applies");
            }
        }
        DrainBlitErrors();
    }

    // ---- glBlitFramebuffer onto a non-zero array layer -------------------------------------
    //
    // Some drivers write to layer 0 whatever layer the DRAW framebuffer's
    // glFramebufferTextureLayer attachment names, and raise no error doing it (Adreno 830;
    // SelfTest::ProbeBlitIgnoresDestinationArrayLayer measures it, with the destination-layer-0
    // case as the control). glCopyImageSubData takes the destination layer as an argument rather
    // than reading it off an attachment, and honours it on the same driver - so a blit that is a
    // plain 1:1 copy is issued that way instead.
    //
    // ONLY a plain 1:1 copy. glCopyImageSubData cannot scale, flip, convert format or resolve
    // samples, and it is not clipped by the scissor, so every one of those is a reason to hand
    // the call back to the driver rather than quietly perform a different operation. Those blits
    // still land on the wrong layer; a once-per-process line says so rather than leaving it to be
    // rediscovered.
    //
    // Per aspect, not all-or-nothing: the returned mask is the bits this performed itself, and
    // the caller passes the rest to the driver. A COLOR|DEPTH blit whose colour half scales and
    // whose depth half does not still gets its depth half repaired.
    static GLbitfield BlitLayeredDestinationAspects(
        const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
        const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer, GLint srcX0, GLint srcY0,
        GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask) {
        if (mask == 0 || !readFramebuffer || !drawFramebuffer) return 0;
        if (!g_GLESFuncs.glCopyImageSubData) return 0;
        if (!MG_Util::SelfTest::BlitIgnoresDestinationArrayLayer(g_GLESFuncs)) return 0;

        // The default framebuffer has no layers to get wrong, and a blit between the two halves
        // of the same framebuffer object is not a shape this substitutes for.
        const Int width = srcX1 - srcX0;
        const Int height = srcY1 - srcY0;
        const Bool oneToOne = width > 0 && height > 0 && (dstX1 - dstX0) == width && (dstY1 - dstY0) == height;
        // The scissor clips a blit and does not clip a copy, so an enabled scissor makes the two
        // different operations no matter how the rectangles line up.
        const Bool scissorEnabled =
            (RenderStateImpl::g_syncedRenderStateParameters.ScissorTestEnabledMask & 1u) != 0;

        using MobileGL::FramebufferAttachmentType;
        struct AspectPlan {
            GLbitfield bit;
            FramebufferAttachmentType source;
            FramebufferAttachmentType destination;
        };
        // The colour aspect follows glReadBuffer on the read side and draw buffer 0 on the write
        // side, which is the only draw buffer a blit onto a layered destination can be pinned to
        // here: a blit writes EVERY enabled draw buffer, so a framebuffer with more than one is
        // left to the driver rather than half-repaired.
        const auto& drawBuffers = drawFramebuffer->GetDrawBuffers();
        // GL_NONE is what an unwritten draw-buffer slot holds, and it is a different value from
        // the "no such attachment" one - counting it as enabled made every framebuffer look like
        // it had eight and sent every colour blit to the driver.
        //
        // The buffer is found rather than assumed to be slot 0: a blit writes every ENABLED draw
        // buffer, and glDrawBuffers(GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT0) leaves slot
        // 0 empty while still naming exactly one destination.
        Int enabledDrawBuffers = 0;
        FramebufferAttachmentType colorDestination = FramebufferAttachmentType::None;
        for (const FramebufferAttachmentType buffer : drawBuffers) {
            if (buffer != FramebufferAttachmentType::Unknown && buffer != FramebufferAttachmentType::None) {
                ++enabledDrawBuffers;
                if (enabledDrawBuffers == 1) colorDestination = buffer;
            }
        }
        const AspectPlan plans[] = {
            {GL_COLOR_BUFFER_BIT, readFramebuffer->GetReadBuffer(), colorDestination},
            {GL_DEPTH_BUFFER_BIT, FramebufferAttachmentType::Depth, FramebufferAttachmentType::Depth},
            {GL_STENCIL_BUFFER_BIT, FramebufferAttachmentType::Stencil, FramebufferAttachmentType::Stencil},
        };

        GLbitfield handled = 0;
        for (const AspectPlan& plan : plans) {
            if ((mask & plan.bit) == 0) continue;
            if (plan.source == FramebufferAttachmentType::Unknown ||
                plan.destination == FramebufferAttachmentType::Unknown ||
                plan.source == FramebufferAttachmentType::None ||
                plan.destination == FramebufferAttachmentType::None) {
                continue;
            }
            const auto& sourceAttachment = readFramebuffer->GetAttachment(plan.source);
            const auto& destinationAttachment = drawFramebuffer->GetAttachment(plan.destination);
            // Renderbuffers have no layers, so a destination that is one cannot be hitting this.
            if (!sourceAttachment.IsTexture() || !destinationAttachment.IsTexture()) continue;
            // Layer 0 is the case the driver gets right, and a LAYERED attachment (glFramebufferTexture
            // with no layer) blits its layer 0 by spec - neither is this defect.
            if (destinationAttachment.GetTextureLayer() == 0) continue;
            if (destinationAttachment.IsLayered() || sourceAttachment.IsLayered()) continue;

            const auto& sourceTexture = sourceAttachment.GetTexture();
            const auto& destinationTexture = destinationAttachment.GetTexture();
            if (!sourceTexture || !destinationTexture) continue;
            // glCopyImageSubData moves texel blocks: same format both ends, or it is a different
            // operation. Multisample endpoints would additionally have to agree on sample count,
            // which is a resolve the driver still owns.
            if (sourceTexture->GetFormat() != destinationTexture->GetFormat()) continue;
            if (sourceTexture->GetSamples() > 0 || destinationTexture->GetSamples() > 0) continue;
            // Copying an image region onto itself is undefined for glCopyImageSubData, and a blit
            // whose source and destination overlap is undefined for GL too - so this is not a
            // shape to substitute FOR, it is one to leave exactly as the application wrote it.
            if (sourceTexture == destinationTexture &&
                sourceAttachment.GetTextureLevel() == destinationAttachment.GetTextureLevel() &&
                sourceAttachment.GetTextureLayer() == destinationAttachment.GetTextureLayer()) {
                continue;
            }

            // A combined depth-stencil texture is ONE image to glCopyImageSubData: it carries both
            // aspects across whether or not the mask asked for both. Taking only GL_DEPTH_BUFFER_BIT
            // on a DEPTH24_STENCIL8 destination would overwrite a stencil the application asked to
            // keep, so the copy is only allowed when the mask covers everything the format holds.
            const TextureInternalFormat format = destinationTexture->GetFormat();
            const Bool hasDepth = MG_Util::IsDepthFormatInternalFormat(format);
            const Bool hasStencil = MG_Util::IsStencilFormatInternalFormat(format);
            if (hasDepth && (mask & GL_DEPTH_BUFFER_BIT) == 0) continue;
            if (hasStencil && (mask & GL_STENCIL_BUFFER_BIT) == 0) continue;
            // ... and having carried both, it must be credited with both, or the caller hands the
            // stencil half to the driver and it lands on layer 0 after all.
            const GLbitfield aspectBits =
                hasDepth || hasStencil
                    ? static_cast<GLbitfield>((hasDepth ? GL_DEPTH_BUFFER_BIT : 0) |
                                              (hasStencil ? GL_STENCIL_BUFFER_BIT : 0))
                    : static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
            if ((handled & aspectBits) == aspectBits) continue;

            if (!oneToOne || scissorEnabled || (plan.bit == GL_COLOR_BUFFER_BIT && enabledDrawBuffers != 1)) {
                MGLOG_E_ONCE("BlitFramebuffer: this driver ignores a non-zero destination array layer and this "
                             "blit cannot be expressed as a copy (%s), so it will land on layer 0",
                             !oneToOne          ? "it scales or flips"
                             : scissorEnabled   ? "the scissor test is enabled"
                                                : "the destination has more than one draw buffer");
                continue;
            }

            auto backendSource = TextureImpl::SyncTextureObjectToBackend(sourceTexture);
            auto backendDestination = TextureImpl::SyncTextureObjectToBackend(destinationTexture);
            if (!backendSource || !backendDestination) continue;
            const GLuint sourceName = backendSource->GetBackendTextureId();
            const GLuint destinationName = backendDestination->GetBackendTextureId();
            if (sourceName == 0 || destinationName == 0) continue;
            const GLenum sourceTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(sourceTexture->GetTarget());
            const GLenum destinationTarget =
                TextureImpl::ConvertTextureTargetToBackendGLEnum(destinationTexture->GetTarget());

            ClearGLErrors();
            g_GLESFuncs.glCopyImageSubData(sourceName, sourceTarget, sourceAttachment.GetTextureLevel(), srcX0, srcY0,
                                           sourceAttachment.GetTextureLayer(), destinationName, destinationTarget,
                                           destinationAttachment.GetTextureLevel(), dstX0, dstY0,
                                           destinationAttachment.GetTextureLayer(), width, height, 1);
            if (const GLenum error = g_GLESFuncs.glGetError(); error != GL_NO_ERROR) {
                // The driver blit still runs for this aspect - onto the wrong layer, but the
                // substitute has to leave the call no worse off than it found it.
                MGLOG_E_ONCE("BlitFramebuffer: the layered-destination copy substitute failed with %s; the "
                             "driver blit will run instead and land on layer 0",
                             MG_Util::ConvertGLEnumToString(error).c_str());
                continue;
            }
            handled |= aspectBits;
        }
        return handled & mask;
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
        // A no-op on every driver that honours a non-zero destination array layer, which is all
        // of them but the probed one. Whatever it performs itself is taken out of the mask.
        mask &= ~BlitLayeredDestinationAspects(
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject(),
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), srcX0, srcY0,
            srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask);
        if (mask != 0) {
            IssueBlitWithResolveFallback(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        }
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
        // See the DSA-free entry point above: only the probed defect makes this do anything.
        mask &= ~BlitLayeredDestinationAspects(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0,
                                               dstY0, dstX1, dstY1, mask);
        if (mask != 0) {
            IssueBlitWithResolveFallback(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        }
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
            MGLOG_E_ONCE("    Texture target %s is not supported, skipping.",
                    MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
            return false;
        }

        const auto& bindingSlot = textureUnit.GetBindingSlot(textureTarget);
        {
            const auto& textureObject = bindingSlot.GetBoundObject();
            if (!textureObject) {
                MGLOG_D("%s: Texture target %s does not have texture bound.", __func__,
                        MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
            }

            auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get());
            auto& backendObj = backendTextureSlot
                                   ? *backendTextureSlot
                                   : TextureImpl::g_backendTextureObjects.GetOrCreate(textureObject);
            if (!backendObj) {
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
        ScopedScissorDisable()
            : m_wasEnabled((RenderStateImpl::g_syncedRenderStateParameters.ScissorTestEnabledMask & 1u) != 0) {
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

        MGLOG_E_ONCE("%s failed: %s. target=%s, format=%s", operation,
                MG_Util::ConvertGLEnumToString(err).c_str(),
                MG_Util::ConvertGLEnumToString(target).c_str(),
                MG_Util::ConvertTextureInternalFormatToString(format).c_str());
        MG_State::pGLContext->RecordError(
            ConvertGLESErrorToErrorCode(err),
            MakeUnique<GenericErrorInfo>("DirectGLES", operation,
                                         MG_Util::ConvertGLEnumToString(err)));
        return false;
    }

    static void ClearGLErrors() { DrainDriverErrors("DirectGLES"); }

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

            auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(texture.get());
            if (!backendTextureSlot || !*backendTextureSlot) {
                return;
            }
            const GLuint backendTextureId = (*backendTextureSlot)->GetBackendTextureId();

            for (auto it = FramebufferImpl::g_backendFramebufferObjects.begin();
                 it != FramebufferImpl::g_backendFramebufferObjects.end(); ++it) {
                auto* stateFBO = it->first;
                const auto& backendFBO = it->second.backend;
                // An entry whose state object died is only waiting for the next collection;
                // the key is a dangling address, so it must not be dereferenced here.
                if (stateFBO == nullptr || !backendFBO || it->second.stateRef.expired() ||
                    stateFBO->IsDefaultFramebuffer()) {
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
        auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get());
        if (!backendTextureSlot || !*backendTextureSlot) {
            MGLOG_E_ONCE("CopyTexSubImage2D: No backend texture found for texture %u.",
                    textureObject ? textureObject->GetExternalIndex() : 0);
            return;
        }
        (*backendTextureSlot)->Bind(target, activeTextureUnit);

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

            auto currentTex = (GLint)(*backendTextureSlot)->GetBackendTextureId();
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            TempFBOBinder tempFBOBinder(false);
            ScopedScissorDisable scissorOff; // the depth-copy blit below is scissored like any blit
            ScratchFBOImpl::EnsureDepthAttachment2D(tempFBOBinder.Framebuffer(), GL_DRAW_FRAMEBUFFER,
                                                    static_cast<Uint>(currentTex), target, level, isStencilFormat);

            if (g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                MGLOG_E_ONCE("ES glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE");
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
        auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get());
        if (!backendTextureSlot || !*backendTextureSlot) {
            MGLOG_E_ONCE("CopyTexSubImage2D: No backend texture found for texture %u.",
                    textureObject ? textureObject->GetExternalIndex() : 0);
            return;
        }
        (*backendTextureSlot)->Bind(target, activeTextureUnit);

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
            auto currentTex = (*backendTextureSlot)->GetBackendTextureId();
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
                MGLOG_E_ONCE("ES glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE");
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

    // ES has no colour-renderable three-channel float format, and its glGenerateMipmap
    // requires one, so it rejects GL_RGB16F and GL_RGB32F outright where every desktop
    // driver accepts them. Both store a plain array of floats, and a format the driver
    // cannot render into is a format nothing can have rendered into - so the frontend's own
    // copy of the texels is the authority, and the chain can be filtered there and carried
    // down by the ordinary upload path. Returns false for anything else, leaving the
    // driver's answer (including its error) in place.
    static Bool GenerateThreeChannelFloatMipmapOnCpu(
        const SharedPtr<MG_State::GLState::ITextureObject>& texture) {
        if (!texture) return false;
        const TextureInternalFormat format = texture->GetFormat();
        const Bool isHalf = format == TextureInternalFormat::RGB16F;
        if (!isHalf && format != TextureInternalFormat::RGB32F) return false;

        auto* mipmapTexture = MG_State::GLState::AsMipmapTexture(texture.get());
        if (mipmapTexture == nullptr) return false;
        const Uint levelCount = mipmapTexture->GetMipmapLevelCount();
        constexpr Int kChannels = 3;

        for (const auto uploadTarget : texture->GetUploadTargets()) {
            for (Uint level = 1; level < levelCount; ++level) {
                const IntVec3 srcSize = mipmapTexture->GetMipmapTexelSize(uploadTarget, level - 1);
                const IntVec3 dstSize = mipmapTexture->GetMipmapTexelSize(uploadTarget, level);
                if (srcSize.x() <= 0 || srcSize.y() <= 0 || dstSize.x() <= 0 || dstSize.y() <= 0) return false;
                auto* src = static_cast<Uint8*>(mipmapTexture->MapMipmapData(uploadTarget, level - 1));
                auto* dst = static_cast<Uint8*>(mipmapTexture->MapMipmapData(uploadTarget, level));
                if (src == nullptr || dst == nullptr) return false;

                const SizeT componentBytes = isHalf ? sizeof(Uint16) : sizeof(Float);
                const SizeT texelBytes = componentBytes * kChannels;
                const auto load = [&](const Uint8* base, Int x, Int y, Int channel) {
                    const Uint8* texel = base + (static_cast<SizeT>(y) * srcSize.x() + x) * texelBytes +
                                         channel * componentBytes;
                    if (isHalf) {
                        Uint16 bits = 0;
                        Memcpy(&bits, texel, sizeof(bits));
                        return MG_Util::DecodeHalfBitsToFloat(bits);
                    }
                    Float value = 0.0f;
                    Memcpy(&value, texel, sizeof(value));
                    return value;
                };

                // Box filter over the 2x2 source footprint, clamped where a dimension is
                // already 1 (GL 4.6 core 8.14.4 leaves the exact filter to the implementation
                // and this is the one it describes for power-of-two levels).
                for (Int y = 0; y < dstSize.y(); ++y) {
                    for (Int x = 0; x < dstSize.x(); ++x) {
                        const Int x0 = std::min(x * 2, srcSize.x() - 1);
                        const Int x1 = std::min(x * 2 + 1, srcSize.x() - 1);
                        const Int y0 = std::min(y * 2, srcSize.y() - 1);
                        const Int y1 = std::min(y * 2 + 1, srcSize.y() - 1);
                        for (Int channel = 0; channel < kChannels; ++channel) {
                            const Float average = 0.25f * (load(src, x0, y0, channel) + load(src, x1, y0, channel) +
                                                           load(src, x0, y1, channel) + load(src, x1, y1, channel));
                            Uint8* texel = dst + (static_cast<SizeT>(y) * dstSize.x() + x) * texelBytes +
                                           channel * componentBytes;
                            if (isHalf) {
                                const Uint16 bits = MG_Util::EncodeFloatToHalfBits(average);
                                Memcpy(texel, &bits, sizeof(bits));
                            } else {
                                Memcpy(texel, &average, sizeof(average));
                            }
                        }
                    }
                }
                mipmapTexture->MarkStorageDirty(uploadTarget, level, true);
            }
        }
        return true;
    }

    void PatchParameteri(GLenum pname, GLint value) {
        if (g_GLESFuncs.glPatchParameteri == nullptr) return;
        g_GLESFuncs.glPatchParameteri(pname, value);
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
        if (texture->GetFormat() == TextureInternalFormat::R11FG11FB10F || IsDepthOnlyFormat(texture->GetFormat()) ||
            texture->GetFormat() == TextureInternalFormat::RGB16F ||
            texture->GetFormat() == TextureInternalFormat::RGB32F) {
            EnsureGenerateMipmapStorageAllocated(texture);
        }
        // Filtered on the CPU before the backend sync, so the dirty levels ride down with it.
        if (GenerateThreeChannelFloatMipmapOnCpu(texture)) {
            TextureImpl::SyncTextureObjectToBackend(texture);
            return;
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

    // An atomic counter is a shader storage block by the time it reaches the ES driver (glslang
    // lowers every atomic_uint onto one), so an application that asks only for the counter
    // barrier is asking about memory the driver knows as storage-buffer memory. Ordering one
    // does not oblige a driver to order the other, so the counter bit implies the storage bit
    // here - which is what the lowering costs and the only place it can be paid.
    static GLbitfield LowerAtomicCounterBarrierBits(GLbitfield barriers) {
        if ((barriers & GL_ATOMIC_COUNTER_BARRIER_BIT) != 0) {
            barriers |= GL_SHADER_STORAGE_BARRIER_BIT;
        }
        return barriers;
    }

    void MemoryBarrier(GLbitfield barriers) {
        g_GLESFuncs.glMemoryBarrier(LowerAtomicCounterBarrierBits(barriers));
        if (g_GLESCapabilities.IsAngleRenderer) {
            g_GLESFuncs.glFlush();
        }
    }

    void MemoryBarrierByRegion(GLbitfield barriers) {
        g_GLESFuncs.glMemoryBarrierByRegion(LowerAtomicCounterBarrierBits(barriers));
    }

    // One endpoint of a glCopyImageSubData, expressed the way the ES driver stores it.
    //
    // The frontend hands this backend the target the APPLICATION named, and three of the
    // targets core GL has do not exist in ES at all. They are not missing here either - the
    // texture managers already store a 1D texture as a height-1 2D one, a 1D array as a
    // height-1 2D array and a rectangle texture as a plain 2D one (MapToBackendTextureTarget) -
    // but glCopyImageSubData was the one path that never asked for that translation and passed
    // 0x84F5 / 0x0DE0 / 0x8C18 straight through. ES rejects the enum, the copy does not happen,
    // and with the error only asserted on (asserts are compiled out of an INFO build) the
    // destination silently keeps whatever it held.
    //
    // The 1D-array case is not just a rename: GL addresses its layers with y/height while the
    // ES 2D array that backs it addresses them with z/depth, so the two axes swap with the
    // target.
    //
    // GL_RENDERBUFFER is the exception that must NOT be translated: ES 3.2 core (and
    // GL_EXT_copy_image) take it as a srcTarget/dstTarget verbatim, while
    // ConvertGLEnumToTextureTarget answers Unknown for it and the translation below would hand
    // the driver GL_UNKNOWN_MGL.
    struct GLESCopyImageEndpoint {
        GLenum target = GL_TEXTURE_2D;
        // Exactly one of the two is set. The backend object is kept rather than its id, because
        // the id is only stable until the OTHER endpoint syncs (a sync can re-mint a texture),
        // so it is read at the point of use.
        SharedPtr<TextureImpl::BackendTextureObject> texture;
        SharedPtr<RenderbufferImpl::BackendRenderbufferObject> renderbuffer;
        GLint x = 0;
        GLint y = 0;
        GLint z = 0;

        Bool IsRenderbuffer() const { return renderbuffer != nullptr; }
        GLuint Name() const {
            if (renderbuffer) return renderbuffer->GetBackendRenderbufferId();
            return texture ? texture->GetBackendTextureId() : 0u;
        }
    };

    // The renderbuffer twin of TextureImpl::SyncTextureObjectToBackend: the same
    // find-or-create-then-sync the framebuffer attachment walk does (see SyncAttachmentObject),
    // reachable from a path that has a renderbuffer but no framebuffer.
    static SharedPtr<RenderbufferImpl::BackendRenderbufferObject> SyncRenderbufferObjectToBackend(
        const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbufferObject) {
        if (!renderbufferObject) return nullptr;
        SharedPtr<RenderbufferImpl::BackendRenderbufferObject> backendRenderbufferObject;
        if (auto* slot = RenderbufferImpl::g_backendRenderbufferObjects.Find(renderbufferObject.get())) {
            backendRenderbufferObject = *slot;
        } else {
            auto& newSlot = RenderbufferImpl::g_backendRenderbufferObjects.GetOrCreate(renderbufferObject);
            if (!newSlot) {
                newSlot = MakeShared<RenderbufferImpl::BackendRenderbufferObject>();
            }
            backendRenderbufferObject = newSlot;
        }
        backendRenderbufferObject->SyncToBackend(renderbufferObject);
        return backendRenderbufferObject;
    }

    static Bool MakeGLESCopyImageEndpoint(const CopyImageEndpoint& endpoint, GLenum appTarget, GLint x, GLint y,
                                          GLint z, GLESCopyImageEndpoint& out) {
        if (endpoint.IsRenderbuffer()) {
            out.renderbuffer = SyncRenderbufferObjectToBackend(endpoint.Renderbuffer);
            if (!out.renderbuffer) return false;
            out.target = GL_RENDERBUFFER;
            out.x = x;
            out.y = y;
            out.z = z;
            return true;
        }
        // BY VALUE, not by reference. SyncTextureObjectToBackend hands back a reference to a
        // slot inside the backend texture registry, and the second call mutates that very map:
        // GetOrCreate indexes it (an insert relocates entries - by rehashing, and also by
        // robin-hood displacement well under the load factor), and Find drops any
        // entry whose state object has expired - which, with the map open-addressed and erasing
        // by shifting the probe cluster backwards, relocates entries other than the erased one.
        // Either way a reference taken by the first call is stale by the time the second returns,
        // and it is read four more times below. Copying the SharedPtr costs two refcount bumps on
        // a path that is already doing a texture copy.
        // An endpoint that named nothing is the frontend validator's INVALID_VALUE and never
        // reaches here - but the assertion that says so is compiled out of a release build, and
        // SyncTextureObjectToBackend would register a null state object.
        if (!endpoint.Texture) return false;
        out.texture = TextureImpl::SyncTextureObjectToBackend(endpoint.Texture);
        if (!out.texture) return false;
        const TextureTarget stateTarget = MG_Util::ConvertGLEnumToTextureTarget(appTarget);
        out.target = TextureImpl::ConvertTextureTargetToBackendGLEnum(stateTarget);
        // No axis remap for GL_TEXTURE_1D_ARRAY. The frontend STORES a 1D array with its layers
        // on y (GetBackendUploadSize moves them across to the ES 2D array's z), but this entry
        // point does not ADDRESS it that way: GL 4.6 core 18.3.2 treats every array texture as a
        // stack of slices on z and gives a 1D array a height of 1 - exactly the shape the ES 2D
        // array has - so GL's (x, 0, layer) and the ES image's (x, 0, layer) already agree.
        // Remapping y into z here fetched the wrong slice for every call that spelled the layer
        // the way GL defines it.
        out.x = x;
        out.y = y;
        out.z = z;
        return true;
    }

    static TextureInternalFormat GetCopyImageEndpointFormat(const CopyImageEndpoint& endpoint) {
        if (endpoint.IsRenderbuffer()) return endpoint.Renderbuffer->GetInternalFormat();
        return endpoint.Texture ? endpoint.Texture->GetFormat() : TextureInternalFormat::Unknown;
    }

    // Whether this endpoint's CPU shadow can be addressed texel-exactly by the mirror below: one
    // upload target (so not a cube map, whose six chains the z axis selects between) and layers on
    // the z axis (GL_TEXTURE_1D_ARRAY carries them on y).
    static Bool CanMirrorCopyImageShadow(const SharedPtr<MG_State::GLState::ITextureObject>& texture) {
        if (!texture) return false;
        if (texture->GetTarget() == TextureTarget::Texture1DArray) return false;
        return texture->GetUploadTargets().size() == 1;
    }

    // glCopyImageSubData is defined as a raw texel-block move, so for a destination whose CPU
    // shadow has to stay authoritative - a packed format with redundant encodings, where a GPU
    // readback can only answer with RE-ENCODED words (see the verbatim branch in GetTexImage) -
    // the same move is replayed on the shadow. Nothing is marked dirty: the driver copy already
    // put these texels on the GPU, and flagging the level would only schedule a redundant upload
    // back over them.
    //
    // Declined, leaving the shadow exactly as it was, for every shape whose bytes this cannot
    // address exactly - a renderbuffer (no shadow at all), a cube or 1D-array endpoint, a level
    // whose shadow is missing or not a plain texel grid, a region outside either level, or a
    // self-copy within one level, where the row copies could overlap.
    static void MirrorCopyImageIntoDestinationShadow(const CopyImageEndpoint& srcEndpoint, GLint srcLevel, GLint srcX,
                                                     GLint srcY, GLint srcZ, const CopyImageEndpoint& dstEndpoint,
                                                     GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                                     GLsizei width, GLsizei height, GLsizei depth) {
        if (!CanMirrorCopyImageShadow(srcEndpoint.Texture) || !CanMirrorCopyImageShadow(dstEndpoint.Texture)) return;
        if (srcEndpoint.Texture == dstEndpoint.Texture && srcLevel == dstLevel) return;
        if (width <= 0 || height <= 0 || depth <= 0) return;
        if (srcLevel < 0 || dstLevel < 0 || srcX < 0 || srcY < 0 || srcZ < 0 || dstX < 0 || dstY < 0 || dstZ < 0) {
            return;
        }
        auto* srcMipmap = MG_State::GLState::AsMipmapTexture(srcEndpoint.Texture.get());
        auto* dstMipmap = MG_State::GLState::AsMipmapTexture(dstEndpoint.Texture.get());
        if (!srcMipmap || !dstMipmap) return;

        const auto srcUploadTarget = srcEndpoint.Texture->GetUploadTargets()[0];
        const auto dstUploadTarget = dstEndpoint.Texture->GetUploadTargets()[0];
        const IntVec3 srcSize = srcMipmap->GetMipmapTexelSize(srcUploadTarget, static_cast<Uint>(srcLevel));
        const IntVec3 dstSize = dstMipmap->GetMipmapTexelSize(dstUploadTarget, static_cast<Uint>(dstLevel));
        const SizeT srcSlices = static_cast<SizeT>(std::max(srcSize.z(), 1));
        const SizeT dstSlices = static_cast<SizeT>(std::max(dstSize.z(), 1));
        if (srcSize.x() <= 0 || srcSize.y() <= 0 || dstSize.x() <= 0 || dstSize.y() <= 0) return;
        const SizeT srcTexels = static_cast<SizeT>(srcSize.x()) * static_cast<SizeT>(srcSize.y()) * srcSlices;
        const SizeT dstTexels = static_cast<SizeT>(dstSize.x()) * static_cast<SizeT>(dstSize.y()) * dstSlices;
        const SizeT srcBytes = srcMipmap->GetMipmapByteSize(srcUploadTarget, static_cast<Uint>(srcLevel));
        const SizeT dstBytes = dstMipmap->GetMipmapByteSize(dstUploadTarget, static_cast<Uint>(dstLevel));
        // A shadow that is not exactly texels x texelSize bytes is one this cannot index (a
        // compressed blob, or a level whose allocation disagrees with its recorded extent).
        const SizeT texelBytes = srcTexels == 0 ? 0 : srcBytes / srcTexels;
        if (texelBytes == 0 || srcBytes != srcTexels * texelBytes || dstTexels == 0 ||
            dstBytes != dstTexels * texelBytes) {
            return;
        }
        if (static_cast<SizeT>(srcX) + width > static_cast<SizeT>(srcSize.x()) ||
            static_cast<SizeT>(srcY) + height > static_cast<SizeT>(srcSize.y()) ||
            static_cast<SizeT>(srcZ) + depth > srcSlices ||
            static_cast<SizeT>(dstX) + width > static_cast<SizeT>(dstSize.x()) ||
            static_cast<SizeT>(dstY) + height > static_cast<SizeT>(dstSize.y()) ||
            static_cast<SizeT>(dstZ) + depth > dstSlices) {
            return;
        }

        const auto* srcBase = static_cast<const Uint8*>(
            srcMipmap->MapMipmapData(srcUploadTarget, static_cast<Uint>(srcLevel)));
        auto* dstBase = static_cast<Uint8*>(dstMipmap->MapMipmapData(dstUploadTarget, static_cast<Uint>(dstLevel)));
        if (!srcBase || !dstBase) return;

        const SizeT rowBytes = static_cast<SizeT>(width) * texelBytes;
        for (GLsizei slice = 0; slice < depth; ++slice) {
            for (GLsizei row = 0; row < height; ++row) {
                const SizeT srcOffset = ((static_cast<SizeT>(srcZ + slice) * static_cast<SizeT>(srcSize.y()) +
                                          static_cast<SizeT>(srcY + row)) *
                                             static_cast<SizeT>(srcSize.x()) +
                                         static_cast<SizeT>(srcX)) *
                                        texelBytes;
                const SizeT dstOffset = ((static_cast<SizeT>(dstZ + slice) * static_cast<SizeT>(dstSize.y()) +
                                          static_cast<SizeT>(dstY + row)) *
                                             static_cast<SizeT>(dstSize.x()) +
                                         static_cast<SizeT>(dstX)) *
                                        texelBytes;
                Memcpy(dstBase + dstOffset, srcBase + srcOffset, rowBytes);
            }
        }
        MGLOG_D("CopyImageSubData: mirrored %dx%dx%d texels into the destination's CPU shadow", width, height,
                depth);
    }

    void CopyImageSubData(const CopyImageEndpoint& srcEndpoint,
                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          const CopyImageEndpoint& dstEndpoint,
                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        GLESCopyImageEndpoint src{};
        GLESCopyImageEndpoint dst{};
        // The DirectVulkan half of this entry point died exactly here, on a texture whose sync
        // produced nothing - and it died in a release build, where the MOBILEGL_ASSERT that was
        // supposed to catch it expands to nothing. The four Name() calls below are the same
        // dereference. The frontend validator is what keeps this unreachable and what reports
        // the error the application is owed; declining is only how a future gap up there stops
        // being a crash. See the level guard in VulkanRenderer::CopyImageSubData.
        if (!MakeGLESCopyImageEndpoint(srcEndpoint, srcTarget, srcX, srcY, srcZ, src) ||
            !MakeGLESCopyImageEndpoint(dstEndpoint, dstTarget, dstX, dstY, dstZ, dst)) {
            MGLOG_E_ONCE("%s: source or destination image failed to sync; declining the copy", __func__);
            return;
        }

        // Verbatim: GL already spells a 1D array's extent the way the ES 2D array it maps onto
        // wants it (height 1, layers on depth) - see MakeGLESCopyImageEndpoint.
        const GLsizei copyHeight = srcHeight;
        const GLsizei copyDepth = srcDepth;

        const TextureInternalFormat srcFormat = GetCopyImageEndpointFormat(srcEndpoint);
        const TextureInternalFormat dstFormat = GetCopyImageEndpointFormat(dstEndpoint);
        // Both emulation fallbacks below are written against TEXTURE ids and texture targets, so
        // an endpoint that is a renderbuffer takes the native ES copy - which accepts
        // GL_RENDERBUFFER on both sides - and reports rather than mis-dispatches if the driver
        // turns it down.
        const Bool anyRenderbuffer = src.IsRenderbuffer() || dst.IsRenderbuffer();

        const Bool srcIsDepth = MG_Util::IsDepthFormatInternalFormat(srcFormat);
        const Bool dstIsDepth = MG_Util::IsDepthFormatInternalFormat(dstFormat);
        const Bool srcStencil = MG_Util::IsStencilFormatInternalFormat(srcFormat);
        const Bool dstStencil = MG_Util::IsStencilFormatInternalFormat(dstFormat);
        if (!anyRenderbuffer && (srcIsDepth || dstIsDepth || srcStencil || dstStencil)) {
            MOBILEGL_ASSERT(srcIsDepth && dstIsDepth && !srcStencil && !dstStencil,
                            "DirectGLES CopyImageSubData only supports depth-only image copies.");
            MOBILEGL_ASSERT(src.target == GL_TEXTURE_2D && dst.target == GL_TEXTURE_2D,
                            "DirectGLES depth CopyImageSubData only supports GL_TEXTURE_2D.");
            MOBILEGL_ASSERT(src.z == 0 && dst.z == 0 && copyDepth == 1,
                            "DirectGLES depth CopyImageSubData only supports single-layer copies.");
            BlitDepthTexture2D(src.Name(), srcLevel, src.x, src.y, srcWidth, copyHeight,
                               dst.Name(), dstLevel, dst.x, dst.y, srcWidth, copyHeight);
            return;
        }

        if (!anyRenderbuffer &&
            (srcFormat == TextureInternalFormat::R32F || dstFormat == TextureInternalFormat::R32F)) {
            // The single glGetError below decides the fallback dispatch, and
            // ErrorLopper::Clear is compiled out at the default log level - drain
            // with the always-live helper so a stale flag cannot misroute a
            // succeeded native copy into the 2D-only fallback.
            ClearGLErrors();
            g_GLESFuncs.glCopyImageSubData(src.Name(), src.target, srcLevel, src.x, src.y, src.z,
                                           dst.Name(), dst.target, dstLevel, dst.x, dst.y, dst.z,
                                           srcWidth, copyHeight, copyDepth);
            const GLenum copyImageError = g_GLESFuncs.glGetError();
            if (copyImageError == GL_NO_ERROR) {
                return;
            }
            MOBILEGL_ASSERT(IsColorOnlyFormat(srcFormat) && IsColorOnlyFormat(dstFormat),
                            "DirectGLES CopyImageSubData only supports color-only or depth-only copies.");
            MOBILEGL_ASSERT(src.target == GL_TEXTURE_2D && dst.target == GL_TEXTURE_2D,
                            "DirectGLES color CopyImageSubData only supports GL_TEXTURE_2D.");
            MOBILEGL_ASSERT(src.z == 0 && dst.z == 0 && copyDepth == 1,
                            "DirectGLES color CopyImageSubData only supports single-layer copies.");
            CopyR32FTexture2D(src.Name(), srcLevel, src.x, src.y, srcWidth, copyHeight,
                              dst.Name(), dst.target, dstLevel, dst.x, dst.y);
            return;
        }

        ClearGLErrors();
        g_GLESFuncs.glCopyImageSubData(src.Name(), src.target, srcLevel, src.x, src.y, src.z,
                                       dst.Name(), dst.target, dstLevel, dst.x, dst.y, dst.z,
                                       srcWidth, copyHeight, copyDepth);
        // Every error condition glCopyImageSubData has was already ruled out by the frontend
        // validator, so a driver error here is an internal invariant violation, not something
        // the application can provoke. Say so where an INFO build can still see it, then trap
        // in the builds that trap - the previous bare assert left a release build with a
        // destination that silently kept its old contents.
        const GLenum copyImageError = g_GLESFuncs.glGetError();
        if (copyImageError != GL_NO_ERROR) {
            MGLOG_E_ONCE("glCopyImageSubData failed: %s. src target=%s (app %s), dst target=%s (app %s)",
                         MG_Util::ConvertGLEnumToString(copyImageError).c_str(),
                         MG_Util::ConvertGLEnumToString(src.target).c_str(),
                         MG_Util::ConvertGLEnumToString(srcTarget).c_str(),
                         MG_Util::ConvertGLEnumToString(dst.target).c_str(),
                         MG_Util::ConvertGLEnumToString(dstTarget).c_str());
            MOBILEGL_ASSERT(false, "glCopyImageSubData failed after frontend validation accepted the request.");
            return;
        }
        // The copy landed on the GPU. For a destination whose readback cannot be bit-exact the
        // CPU shadow is what glGetTexImage answers from, so it has to follow the same move -
        // otherwise it hands back whatever the level held before this copy.
        if (MG_Util::PixelStoreProcessor::HasRedundantPackedEncoding(dstFormat)) {
            MirrorCopyImageIntoDestinationShadow(srcEndpoint, srcLevel, srcX, srcY, srcZ, dstEndpoint, dstLevel,
                                                 dstX, dstY, dstZ, srcWidth, srcHeight, srcDepth);
        }
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

    // NOTE the shape here, and do not "simplify" it back to GetBackendProgramId(): this entry
    // point must never be the thing that BUILDS a backend program.
    //
    // The frontend has already recorded the rebinding on the program object
    // (SetShaderStorageBlockBinding) - that record is what GL_BUFFER_BINDING reports and what
    // BackendProgramObjectImpl::SyncToBackend replays onto every driver program it builds. So
    // the only work left here is an optimisation: push the change straight onto a driver
    // program that is ALREADY built and already current with this link, so the next draw does
    // not have to be preceded by a rebuild.
    //
    // Calling GetBackendProgramId() instead would sync-on-demand from a non-draw entry point,
    // i.e. transpile and compile the whole program while the draw-path globals that the ESSL
    // is generated against (PrgramImpl::g_fragColorBroadcastCount, the snorm/unorm clamp
    // masks - established by SyncCurrentProgram) still hold another program's values. That
    // bakes a program against the wrong state, and under CPU load it was also observed to
    // fail the driver compile outright. Deferring is spec-fine: a binding only has to take
    // effect by the block's next use.
    void ShaderStorageBlockBinding(GLuint program, const GLchar* storageBlockName, GLuint storageBlockBinding) {
        if (!storageBlockName) return;
        if (!MG_State::pGLContext->ValidateProgramName(program)) return;
        auto& programObject = MG_State::pGLContext->GetProgramObject(program);
        if (!programObject) return;

        auto* backendProgramSlot = PrgramImpl::g_backendProgramObjects.Find(programObject.get());
        if (!backendProgramSlot || !*backendProgramSlot) return;
        auto& backendObj = *backendProgramSlot;
        // Not merely "a program id exists": a backend object whose synced link version has
        // fallen behind is about to be rebuilt anyway, and its current driver interface is
        // the PREVIOUS link's - applying to it could land the binding on an unrelated block.
        if (!backendObj->GetBackendProgramId() ||
            backendObj->GetSyncedLinkVersion() != programObject->GetLinkVersion()) {
            return; // SyncToBackend's reseed will carry it
        }
        PrgramImpl::ApplyShaderStorageBlockBinding(backendObj->GetBackendProgramId(), storageBlockName,
                                                   storageBlockBinding);
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        g_GLESFuncs.glClearBufferfi(buffer, drawbuffer, depth, stencil);
    }

    namespace {
        using FramebufferImpl::SubstituteWidenedClearAlpha;

        // A colour attachment the backend widened from three channels to four has to end up
        // holding alpha 1.0 - the value GL reports for a channel the application's format does
        // not have - so an explicit per-buffer clear of it writes 1.0 rather than whatever the
        // application passed (SubstituteWidenedClearAlpha, in Managers.h). Draws can never move
        // it again: their alpha write mask is forced off, see SyncRenderState. That pairing is
        // what makes GL_DST_ALPHA blending, glReadPixels and glBlitFramebuffer all see the right
        // value without any of them being intercepted.

        // Whether draw buffer `drawbuffer` of the framebuffer currently bound as DRAW is such an
        // attachment. Answered from the mask SyncCurrentFBO just recomputed, so it costs nothing.
        Bool IsWidenedBoundDrawBuffer(GLenum buffer, GLint drawbuffer) {
            return buffer == GL_COLOR && drawbuffer >= 0 && drawbuffer < 32 &&
                   (FramebufferImpl::g_alphaWidenedDrawBufferMask & (1u << drawbuffer)) != 0;
        }

        // The same question for an explicitly named framebuffer (the DSA clears), which is NOT
        // the one g_alphaWidenedDrawBufferMask describes at the point these run.
        Bool IsWidenedNamedDrawBuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                      GLenum buffer, GLint drawbuffer) {
            using FBO = MG_State::GLState::FramebufferObject;
            if (buffer != GL_COLOR || !framebuffer || drawbuffer < 0 ||
                drawbuffer >= static_cast<GLint>(FBO::MAX_DRAW_BUFFERS)) {
                return false;
            }
            const auto frontendBuf = framebuffer->GetDrawBuffers()[static_cast<SizeT>(drawbuffer)];
            if (frontendBuf < FramebufferAttachmentType::Color0 ||
                frontendBuf > FramebufferAttachmentType::Color31) {
                return false;
            }
            return FramebufferImpl::IsAlphaWidenedColorAttachment(framebuffer->GetAttachment(frontendBuf));
        }
    } // namespace

    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState(/*forColorClear=*/buffer == GL_COLOR);

        BindCurrentFBO(FramebufferTarget::Draw);

        GLfloat widenedValue[4] = {};
        g_GLESFuncs.glClearBufferfv(
            buffer, drawbuffer,
            SubstituteWidenedClearAlpha(value, IsWidenedBoundDrawBuffer(buffer, drawbuffer), 1.0f, widenedValue));
    }

    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState(/*forColorClear=*/buffer == GL_COLOR);

        // SyncCurrentFBO early-outs for the default framebuffer, so without this
        // bind a user-FBO -> default-FBO switch would leave the clear landing on
        // the stale driver DRAW binding (the fi/fv/uiv siblings all bind too).
        BindCurrentFBO(FramebufferTarget::Draw);

        GLint widenedValue[4] = {};
        g_GLESFuncs.glClearBufferiv(
            buffer, drawbuffer,
            SubstituteWidenedClearAlpha(value, IsWidenedBoundDrawBuffer(buffer, drawbuffer), GLint(1), widenedValue));
    }

    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState(/*forColorClear=*/buffer == GL_COLOR);

        BindCurrentFBO(FramebufferTarget::Draw);

        GLuint widenedValue[4] = {};
        g_GLESFuncs.glClearBufferuiv(
            buffer, drawbuffer,
            SubstituteWidenedClearAlpha(value, IsWidenedBoundDrawBuffer(buffer, drawbuffer), GLuint(1), widenedValue));
    }

    void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, const GLfloat* value) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        TextureImpl::SyncNeccessaryTextures();
        RenderStateImpl::SyncRenderState(/*forColorClear=*/buffer == GL_COLOR);

        SyncAndBindFramebufferObject(framebuffer, FramebufferTarget::Draw, true);
        GLfloat widenedValue[4] = {};
        value = SubstituteWidenedClearAlpha(value, IsWidenedNamedDrawBuffer(framebuffer, buffer, drawbuffer), 1.0f,
                                            widenedValue);
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

    void ClearNamedFramebufferiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, const GLint* value) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        TextureImpl::SyncNeccessaryTextures();
        RenderStateImpl::SyncRenderState(/*forColorClear=*/buffer == GL_COLOR);

        SyncAndBindFramebufferObject(framebuffer, FramebufferTarget::Draw, true);
        GLint widenedValue[4] = {};
        value = SubstituteWidenedClearAlpha(value, IsWidenedNamedDrawBuffer(framebuffer, buffer, drawbuffer),
                                            GLint(1), widenedValue);
        g_GLESFuncs.glClearBufferiv(buffer, drawbuffer, value);
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        ForceBindCurrentFBO(FramebufferTarget::Draw);
    }

    void ClearNamedFramebufferuiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                  GLenum buffer, GLint drawbuffer, const GLuint* value) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        TextureImpl::SyncNeccessaryTextures();
        RenderStateImpl::SyncRenderState(/*forColorClear=*/buffer == GL_COLOR);

        SyncAndBindFramebufferObject(framebuffer, FramebufferTarget::Draw, true);
        GLuint widenedValue[4] = {};
        value = SubstituteWidenedClearAlpha(value, IsWidenedNamedDrawBuffer(framebuffer, buffer, drawbuffer),
                                            GLuint(1), widenedValue);
        g_GLESFuncs.glClearBufferuiv(buffer, drawbuffer, value);
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });

        ForceBindCurrentFBO(FramebufferTarget::Draw);
    }

    static SizeT AlignPixelRow(SizeT rowBytes, Int alignment) {
        const SizeT resolvedAlignment = static_cast<SizeT>(std::max(alignment, 1));
        return (rowBytes + resolvedAlignment - 1) & ~(resolvedAlignment - 1);
    }

    // Destination walk shared by the depth, stencil and packed depth-stencil readbacks.
    // Each of those produces its rows tightly packed and has to land them in the caller's
    // destination - client memory or the bound pixel-pack buffer - under the PACK
    // pixel-store parameters. `fillRow` is handed the row index and a buffer of exactly one
    // packed row to populate. Only real pixel rows are written, so PACK skip/row-length gap
    // regions stay untouched.
    template <typename FillRow>
    static Bool StoreReadbackRowsToClient(GLsizei width, GLsizei height, SizeT dstPixelBytes, void* pixels,
                                          const char* what, FillRow&& fillRow) {
        const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
        const SizeT rowPixels = static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : width);
        const SizeT dstRowStride = AlignPixelRow(rowPixels * dstPixelBytes, packParams.Alignment);
        const SizeT dstOffset = static_cast<SizeT>(std::max(packParams.SkipRows, 0)) * dstRowStride +
                                static_cast<SizeT>(std::max(packParams.SkipPixels, 0)) * dstPixelBytes;
        const SizeT rowBytes = static_cast<SizeT>(width) * dstPixelBytes;
        const SizeT packedSize = dstOffset + static_cast<SizeT>(height - 1) * dstRowStride + rowBytes;
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
        const SizeT pboOffset = reinterpret_cast<SizeT>(pixels);
        if (pixelPackBufferObject && pboOffset + packedSize > pixelPackBufferObject->GetSize()) {
            MGLOG_E_ONCE("ReadPixels: %s readback PBO is too small", what);
            return false;
        }
        Vector<Uint8> rowBuf(rowBytes);
        for (GLsizei row = 0; row < height; ++row) {
            fillRow(row, rowBuf.data());
            const SizeT rowOffset = dstOffset + static_cast<SizeT>(row) * dstRowStride;
            if (pixelPackBufferObject) {
                pixelPackBufferObject->WritebackFromBackend({rowBuf.data(), rowBytes}, pboOffset + rowOffset);
            } else if (pixels != nullptr) {
                Memcpy(static_cast<Uint8*>(pixels) + rowOffset, rowBuf.data(), rowBytes);
            }
        }
        if (pixelPackBufferObject) {
            // WritebackFromBackend bumps change serials with no backend op; re-open the
            // buffer draw-clean memos (once for the whole row loop).
            BufferImpl::BumpBufferMutationEpoch();
        }
        return true;
    }

    // A normalized depth scaled into the full range of an unsigned integer of `maxValue`
    // (GL 4.6 core table 18.2), without ever rounding past the top of that range.
    static Uint32 NormalizedDepthToUnsigned(Float depth, Double maxValue) {
        const Double clamped = std::min(std::max(static_cast<Double>(depth), 0.0), 1.0);
        const Double scaled = clamped * maxValue + 0.5;
        return static_cast<Uint32>(scaled >= maxValue ? maxValue : scaled);
    }

    // ---------------------------------------------------------------------------------
    // Depth / stencil readback by shader sampling
    //
    // Desktop GL reads depth and stencil back through glReadPixels; ES has no such call at
    // all. GL_DEPTH_COMPONENT, GL_STENCIL_INDEX and GL_DEPTH_STENCIL are simply not
    // accepted formats there, and the optional extensions that add them (GL_NV_read_depth,
    // GL_NV_read_stencil, GL_NV_read_depth_stencil) are absent on both the Adreno device
    // and Mesa's ES. Every native attempt therefore failed with a GL error and wrote
    // NOTHING, so the caller kept whatever its buffer already held - which is how the CTS
    // reports "expected DEPTH[0.25] but got DEPTH[0.2]": 0.2 is the poison value the test
    // itself put there.
    //
    // What ES *can* do is sample a depth texture, so the emulation goes the long way round:
    //
    //   1. Stage. glBlitFramebuffer the requested rectangle out of the bound READ
    //      framebuffer into a scratch depth(-stencil) TEXTURE of the very same sized
    //      internal format. One staging copy serves every source kind uniformly - a
    //      texture attachment of any target/level/layer, a renderbuffer (not samplable at
    //      all), the default framebuffer, and a multisample attachment (the blit resolves
    //      it on the way). ES rejects a depth/stencil blit between differing formats, so
    //      the scratch has to match the source exactly; see DescribeReadDepthStencilSource.
    //   2. Convert. Draw a full-screen triangle that samples the staged texture into a
    //      scratch R32UI colour target: depth as floatBitsToUint (bit-exact for every depth
    //      format, and an integer colour target needs no float-renderable extension),
    //      stencil through GL_DEPTH_STENCIL_TEXTURE_MODE = GL_STENCIL_INDEX.
    //   3. Read. glReadPixels the colour target with GL_RGBA_INTEGER / GL_UNSIGNED_INT -
    //      the pair ES guarantees for an unsigned-integer attachment - and hand the values
    //      to the existing re-encoders, which already own the client-side (format, type)
    //      layout and the PACK pixel-store parameters.
    //
    // Both scratch images hold the rectangle at their own origin and the pass runs with a
    // matching viewport, so GL's bottom-up row order survives untouched: scratch row 0 is
    // source row `y`, which is exactly the first row glReadPixels(x, y, ...) owes the
    // caller. No flip anywhere.
    namespace DepthStencilSamplingReadImpl {
        static Uint s_contextGeneration = ~0u;
        static GLuint s_colorFramebuffer = 0;
        static GLuint s_colorTexture = 0;
        static GLsizei s_colorWidth = 0;
        static GLsizei s_colorHeight = 0;
        static GLuint s_vertexArray = 0;
        static GLuint s_depthProgram = 0;
        static GLuint s_stencilProgram = 0;
        static GLint s_depthUvTransform = -1;
        static GLint s_stencilUvTransform = -1;
        static Bool s_programsFailed = false;

        // One staging slot per aspect. A framebuffer is allowed to carry its depth and its
        // stencil in two DIFFERENT objects with two different formats - the framebuffer_blit
        // cases pair a DEPTH_COMPONENT* attachment with a separate STENCIL_INDEX8 one - and
        // the two aspects are staged independently for exactly that reason. A single shared
        // slot would also throw its immutable storage away and re-create it on every
        // alternation between the two.
        struct StageSlot {
            // A framebuffer of its own, not a shared one. GL_DEPTH_STENCIL_ATTACHMENT sets
            // the depth AND the stencil point, so a packed scratch staged for one aspect
            // would leave the other aspect's point pointing at it; the next stage of that
            // other aspect attaches its own (differently formatted) texture to its own point
            // and the framebuffer is then incomplete - which reads as "no candidate format
            // worked" and writes nothing at all.
            GLuint framebuffer = 0;
            GLuint texture = 0;
            GLenum format = 0;
            GLsizei width = 0;
            GLsizei height = 0;
            // The default framebuffer has no queryable internal format, so the one that turns
            // out to be blit-compatible is remembered: it cannot change for the life of the
            // context, and re-probing it on every read would cost a failed blit each time.
            GLenum defaultFramebufferFormat = 0;
        };
        static StageSlot s_slots[2]; // [0] depth, [1] stencil

        // `precision highp int` is not decoration: ESSL 3.00 defaults integers to mediump in
        // the fragment language, which is allowed to be 16 bits - it would saw the top half
        // off every depth bit pattern and every stencil fetch.
        static const char* const kDepthFetchFragmentSource =
            "#version 300 es\n"
            "precision highp float;\n"
            "precision highp int;\n"
            "precision highp sampler2D;\n"
            "uniform sampler2D uSource;\n"
            "in vec2 vUv;\n"
            "layout(location = 0) out uvec4 oBits;\n"
            "void main() {\n"
            "    oBits = uvec4(floatBitsToUint(texture(uSource, vUv).r), 0u, 0u, 0u);\n"
            "}\n";

        static const char* const kStencilFetchFragmentSource =
            "#version 300 es\n"
            "precision highp float;\n"
            "precision highp int;\n"
            "precision highp usampler2D;\n"
            "uniform usampler2D uSource;\n"
            "in vec2 vUv;\n"
            "layout(location = 0) out uvec4 oBits;\n"
            "void main() {\n"
            "    oBits = uvec4(texture(uSource, vUv).r, 0u, 0u, 0u);\n"
            "}\n";

        static Bool EnsureResources() {
            if (s_contextGeneration != g_backendContextGeneration) {
                // The ids belonged to a dead context; the context reclaimed them with it.
                s_colorFramebuffer = 0;
                s_colorTexture = 0;
                s_colorWidth = 0;
                s_colorHeight = 0;
                s_vertexArray = 0;
                s_depthProgram = 0;
                s_stencilProgram = 0;
                s_programsFailed = false;
                s_slots[0] = StageSlot{};
                s_slots[1] = StageSlot{};
                s_contextGeneration = g_backendContextGeneration;
            }
            if (s_programsFailed) {
                return false;
            }
            if (s_depthProgram == 0) {
                // Same full-screen vertex shader (and its uUvTransform) as the replicate
                // blit: a staging slot is sized to the largest rectangle seen so far, so the
                // quad's [0,1] coordinates have to be scaled down to the part it occupies.
                s_depthProgram = ReplicateBlitImpl::BuildProgram(kDepthFetchFragmentSource);
                s_stencilProgram = ReplicateBlitImpl::BuildProgram(kStencilFetchFragmentSource);
                if (s_depthProgram == 0 || s_stencilProgram == 0) {
                    s_programsFailed = true;
                    MGLOG_E_ONCE("ReadPixels: could not build the depth/stencil readback programs");
                    return false;
                }
                s_depthUvTransform = g_GLESFuncs.glGetUniformLocation(s_depthProgram, "uUvTransform");
                s_stencilUvTransform = g_GLESFuncs.glGetUniformLocation(s_stencilProgram, "uUvTransform");
            }
            for (StageSlot& slot : s_slots) {
                if (slot.framebuffer == 0) {
                    g_GLESFuncs.glGenFramebuffers(1, &slot.framebuffer);
                    if (slot.framebuffer == 0) return false;
                }
            }
            if (s_colorFramebuffer == 0) {
                g_GLESFuncs.glGenFramebuffers(1, &s_colorFramebuffer);
                if (s_colorFramebuffer == 0) return false;
            }
            if (s_vertexArray == 0) {
                g_GLESFuncs.glGenVertexArrays(1, &s_vertexArray);
                if (s_vertexArray == 0) return false;
            }
            return true;
        }

        // Ordered guesses at the sized internal format backing one aspect. More than one
        // entry only when the source's own format cannot be queried (the default
        // framebuffer), in which case the reported channel sizes narrow it down and the
        // staging blit picks the winner by being the only one that raises no GL error.
        struct AspectCandidates {
            GLenum formats[4] = {0, 0, 0, 0};
            Uint count = 0;
            void Push(GLenum format) {
                for (Uint i = 0; i < count; ++i) {
                    if (formats[i] == format) return;
                }
                if (count < 4) formats[count++] = format;
            }
        };

        static GLenum AttachmentPointFor(Bool isDefault, Bool stencilAspect) {
            // The default framebuffer names its buffers GL_DEPTH / GL_STENCIL; a user
            // framebuffer names them GL_DEPTH_ATTACHMENT / GL_STENCIL_ATTACHMENT, and asking
            // one for the other's spelling is GL_INVALID_ENUM.
            if (isDefault) return stencilAspect ? GL_STENCIL : GL_DEPTH;
            return stencilAspect ? GL_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        }

        // Returns false when the bound READ framebuffer has no such aspect at all.
        static Bool DescribeAspect(Bool stencilAspect, Bool* outIsDefault, AspectCandidates* out) {
            GLint readFramebuffer = 0;
            g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
            const Bool isDefault = readFramebuffer == 0;
            if (outIsDefault) *outIsDefault = isDefault;
            const GLenum point = AttachmentPointFor(isDefault, stencilAspect);

            ClearGLErrors();
            GLint objectType = GL_NONE;
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, point,
                                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
            ClearGLErrors();

            // The channel sizes are read before the "is there anything here" decision, because
            // they are the more trustworthy witness. Adreno answers GL_NONE for OBJECT_TYPE on
            // an attachment made by glFramebufferTexture (a layered cube/array attachment) while
            // still reporting its depth and stencil bits correctly, and taking OBJECT_TYPE at
            // its word there makes the whole readback report "no such aspect" for a framebuffer
            // that plainly has one. The OTHER aspect's size matters as much as this one's - a
            // depth buffer that also carries stencil has to be staged into a packed scratch,
            // because ES only blits depth between identical formats.
            GLint depthBits = 0;
            GLint stencilBits = 0;
            GLint componentType = GL_UNSIGNED_NORMALIZED;
            ClearGLErrors();
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,
                                                              AttachmentPointFor(isDefault, false),
                                                              GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,
                                                              AttachmentPointFor(isDefault, true),
                                                              GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits);
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                GL_READ_FRAMEBUFFER, AttachmentPointFor(isDefault, false),
                GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
            ClearGLErrors();

            const GLint aspectBits = stencilAspect ? stencilBits : depthBits;
            if (objectType == GL_NONE && aspectBits <= 0) {
                return false;
            }

            if (!isDefault) {
                // A real object: ask it directly and try that first. It is only a preference,
                // not a verdict - the probe binds the attachment as GL_TEXTURE_2D, and a name
                // whose target is not GL_TEXTURE_2D can leave it describing the wrong texture
                // (see QueryAttachmentSizedFormat). The size-derived guesses below therefore
                // stay in the list behind it, so a wrong first answer costs one rejected blit
                // instead of the whole readback. The probe's own refusal must not be left on
                // the error queue for the caller's next glGetError to pick up as its own.
                //
                // The cost of keeping the fallbacks: a source whose OWN format cannot back a
                // staging texture (GL_STENCIL_INDEX8 without EXT/OES_texture_stencil8, say) no
                // longer fails cleanly - it retries with a packed format, and a driver lax
                // enough to accept the resulting mismatched depth/stencil blit would hand back
                // data indistinguishable from a correct read. ES conformance forbids that blit,
                // so this trades a spec-guaranteed rejection for a driver-bug-only wrong answer;
                // the shapes it rescues (array and cube attachments) are otherwise unreadable.
                const GLenum exact = ReplicateBlitImpl::QueryAttachmentSizedFormat(point);
                ClearGLErrors();
                if (exact != 0) {
                    out->Push(exact);
                }
            } else if (s_slots[stencilAspect ? 1 : 0].defaultFramebufferFormat != 0) {
                out->Push(s_slots[stencilAspect ? 1 : 0].defaultFramebufferFormat);
            }

            const Bool floatDepth = componentType == GL_FLOAT;
            const Bool packed = depthBits > 0 && stencilBits > 0;
            if (stencilAspect) {
                if (packed) {
                    out->Push(floatDepth ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8);
                    out->Push(floatDepth ? GL_DEPTH24_STENCIL8 : GL_DEPTH32F_STENCIL8);
                }
                out->Push(GL_STENCIL_INDEX8);
                if (!packed) {
                    out->Push(GL_DEPTH24_STENCIL8);
                }
            } else if (packed) {
                out->Push(floatDepth ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8);
                out->Push(floatDepth ? GL_DEPTH24_STENCIL8 : GL_DEPTH32F_STENCIL8);
            } else if (floatDepth) {
                out->Push(GL_DEPTH_COMPONENT32F);
                out->Push(GL_DEPTH32F_STENCIL8);
            } else if (depthBits > 0 && depthBits <= 16) {
                out->Push(GL_DEPTH_COMPONENT16);
                out->Push(GL_DEPTH_COMPONENT24);
            } else {
                out->Push(GL_DEPTH_COMPONENT24);
                out->Push(GL_DEPTH24_STENCIL8);
                out->Push(GL_DEPTH_COMPONENT32F);
            }
            return out->count != 0;
        }

        // Point a staging slot at `format`, growing it if the rectangle needs it, and leave
        // it bound on the borrowed texture unit.
        static Bool EnsureStageTexture(StageSlot& slot, GLenum format, GLsizei width, GLsizei height) {
            if (slot.texture != 0 && slot.format == format && slot.width >= width && slot.height >= height) {
                g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, slot.texture);
                return true;
            }
            if (slot.texture != 0) {
                g_GLESFuncs.glDeleteTextures(1, &slot.texture); // immutable storage cannot be resized
                ScratchFBOImpl::NoteTextureIdDeleted(slot.texture);
                slot.texture = 0;
            }
            slot.format = 0;
            slot.width = std::max(slot.width, width);
            slot.height = std::max(slot.height, height);
            g_GLESFuncs.glGenTextures(1, &slot.texture);
            if (slot.texture == 0) return false;
            g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, slot.texture);
            ClearGLErrors();
            g_GLESFuncs.glTexStorage2D(GL_TEXTURE_2D, 1, format, slot.width, slot.height);
            const Bool ok = g_GLESFuncs.glGetError() == GL_NO_ERROR;
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // A depth texture left in compare mode samples to 0/1 instead of the stored value.
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            ClearGLErrors();
            slot.format = ok ? format : 0;
            return ok;
        }

        static Bool EnsureColorTexture(GLsizei width, GLsizei height) {
            if (s_colorTexture != 0 && s_colorWidth >= width && s_colorHeight >= height) {
                return true;
            }
            if (s_colorTexture != 0) {
                g_GLESFuncs.glDeleteTextures(1, &s_colorTexture);
                ScratchFBOImpl::NoteTextureIdDeleted(s_colorTexture);
                s_colorTexture = 0;
            }
            s_colorWidth = std::max(s_colorWidth, width);
            s_colorHeight = std::max(s_colorHeight, height);
            g_GLESFuncs.glGenTextures(1, &s_colorTexture);
            if (s_colorTexture == 0) return false;
            g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, s_colorTexture);
            ClearGLErrors();
            // R32UI is colour-renderable in ES 3.0 core - no float-renderability extension
            // needed - and carries a depth bit pattern or a stencil index without loss.
            g_GLESFuncs.glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, s_colorWidth, s_colorHeight);
            const Bool ok = g_GLESFuncs.glGetError() == GL_NO_ERROR;
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ClearGLErrors();
            if (!ok) {
                s_colorWidth = 0;
                s_colorHeight = 0;
            }
            return ok;
        }

        // Copy one aspect of the requested rectangle out of the bound READ framebuffer into
        // its staging slot, trying each candidate format until one is blit-compatible.
        // Requires the slot's own framebuffer bound as DRAW.
        static Bool StageAspect(StageSlot& slot, const AspectCandidates& candidates, Bool stencilAspect, Bool isDefault,
                                GLint x, GLint y, GLsizei width, GLsizei height) {
            const GLbitfield aspectBit = stencilAspect ? GL_STENCIL_BUFFER_BIT : GL_DEPTH_BUFFER_BIT;
            for (Uint candidate = 0; candidate < candidates.count; ++candidate) {
                const GLenum format = candidates.formats[candidate];
                const Bool formatHasAspect = stencilAspect ? ReplicateBlitImpl::FormatHasStencil(format)
                                                           : ReplicateBlitImpl::FormatHasDepth(format);
                if (!formatHasAspect) {
                    continue;
                }
                if (!EnsureStageTexture(slot, format, width, height)) {
                    continue;
                }
                g_GLESFuncs.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                                                   ReplicateBlitImpl::ScratchAttachmentFor(format), GL_TEXTURE_2D,
                                                   slot.texture, 0);
                if (g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    continue;
                }
                ClearGLErrors();
                // Only this aspect: a packed scratch standing in for a separate attachment
                // has a second half with nothing to copy into it.
                g_GLESFuncs.glBlitFramebuffer(x, y, x + width, y + height, 0, 0, width, height, aspectBit, GL_NEAREST);
                const GLenum blitErr = g_GLESFuncs.glGetError();
                if (blitErr != GL_NO_ERROR) {
                    continue;
                }
                if (isDefault) {
                    slot.defaultFramebufferFormat = format;
                }
                return true;
            }
            return false;
        }

        // Runs one conversion pass over a staged slot and reads its colour target back.
        // Requires the slot's texture bound on the borrowed unit and s_colorFramebuffer
        // bound as DRAW.
        static Bool ConvertAndRead(const StageSlot& slot, Bool stencilAspect, GLsizei width, GLsizei height,
                                   Vector<Uint32>& outWords) {
            if (ReplicateBlitImpl::FormatHasDepth(slot.format) && ReplicateBlitImpl::FormatHasStencil(slot.format)) {
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE,
                                            stencilAspect ? GL_STENCIL_INDEX : GL_DEPTH_COMPONENT);
            }
            const Float uvScaleX = static_cast<Float>(width) / static_cast<Float>(slot.width);
            const Float uvScaleY = static_cast<Float>(height) / static_cast<Float>(slot.height);
            g_GLESFuncs.glUseProgram(stencilAspect ? s_stencilProgram : s_depthProgram);
            g_GLESFuncs.glUniform4f(stencilAspect ? s_stencilUvTransform : s_depthUvTransform, uvScaleX, uvScaleY,
                                    0.0f, 0.0f);
            g_GLESFuncs.glViewport(0, 0, width, height);
            ClearGLErrors();
            g_GLESFuncs.glDrawArrays(GL_TRIANGLES, 0, 3);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                MGLOG_E_ONCE("ReadPixels: the %s conversion pass failed", stencilAspect ? "stencil" : "depth");
                return false;
            }

            // GL_RGBA_INTEGER / GL_UNSIGNED_INT is the pair ES guarantees for an unsigned
            // integer attachment whatever its channel count, so four words come back per
            // pixel and only the first carries anything.
            outWords.assign(static_cast<SizeT>(width) * static_cast<SizeT>(height) * 4u, 0u);
            ScopedFramebufferBinding readBinding(/*saveRead=*/true, /*saveDraw=*/false);
            FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, s_colorFramebuffer);
            ScopedPixelPackBuffer packBuffer(0);
            ScopedPackState packState(PixelStoreImpl::PackState{4, 0, 0, 0});
            ClearGLErrors();
            g_GLESFuncs.glReadPixels(0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_INT, outWords.data());
            const GLenum readError = g_GLESFuncs.glGetError();
            if (readError != GL_NO_ERROR) {
                MGLOG_E_ONCE("ReadPixels: could not read the %s conversion target back: %s",
                        stencilAspect ? "stencil" : "depth", MG_Util::ConvertGLEnumToString(readError).c_str());
                return false;
            }
            return true;
        }

        // Stage, convert and read one aspect. The caller owns the state guard and the DRAW
        // framebuffer scope.
        static Bool ReadAspect(Bool stencilAspect, GLint x, GLint y, GLsizei width, GLsizei height,
                               Vector<Uint32>& outWords) {
            Bool isDefault = false;
            AspectCandidates candidates;
            if (!DescribeAspect(stencilAspect, &isDefault, &candidates)) {
                return false;
            }
            StageSlot& slot = s_slots[stencilAspect ? 1 : 0];

            FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, slot.framebuffer);
            if (!StageAspect(slot, candidates, stencilAspect, isDefault, x, y, width, height)) {
                MGLOG_E_ONCE("ReadPixels: no ES-compatible scratch format for the %s source",
                        stencilAspect ? "stencil" : "depth");
                return false;
            }

            if (!EnsureColorTexture(width, height)) {
                MGLOG_E_ONCE("ReadPixels: could not allocate the depth/stencil conversion target");
                return false;
            }
            FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, s_colorFramebuffer);
            g_GLESFuncs.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                               s_colorTexture, 0);
            if (g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                MGLOG_E_ONCE("ReadPixels: the depth/stencil conversion target is not renderable");
                return false;
            }

            // EnsureColorTexture may have taken the borrowed unit for its own storage call.
            VertexArrayImpl::BindBackendVAOId(s_vertexArray);
            g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, slot.texture);
            return ConvertAndRead(slot, stencilAspect, width, height, outWords);
        }

        // Fills whichever of the two outputs the caller asked for from the bound READ
        // framebuffer. Returns false when the emulation could not service the request at
        // all, leaving the caller to report the failure the way it always has.
        static Bool Read(GLint x, GLint y, GLsizei width, GLsizei height, Vector<Float>* outDepth,
                         Vector<Uint8>* outStencil) {
            if (width <= 0 || height <= 0 || (outDepth == nullptr && outStencil == nullptr)) {
                return false;
            }
            // Sampling the stencil half of a packed texture goes through
            // GL_DEPTH_STENCIL_TEXTURE_MODE, which is ES 3.1 state; on an older driver the
            // pname would just raise GL_INVALID_ENUM and the shader would read depth bits as
            // stencil.
            const Bool supportsStencilTextureMode =
                g_GLESCapabilities.GLESVersion.Major > 3 ||
                (g_GLESCapabilities.GLESVersion.Major == 3 && g_GLESCapabilities.GLESVersion.Minor >= 1);
            if (outStencil != nullptr && !supportsStencilTextureMode) {
                return false;
            }
            if (!EnsureResources()) {
                return false;
            }

            // Taken before the first scratch texture bind: the guard owns the borrowed
            // texture unit as well as the pipeline, and it is what puts the scissor test out
            // of the way of the staging blit.
            ScopedEmulationDrawState emulationState;
            ScopedFramebufferBinding drawBinding(/*saveRead=*/false, /*saveDraw=*/true);

            const SizeT pixelCount = static_cast<SizeT>(width) * static_cast<SizeT>(height);
            Vector<Uint32> words;
            if (outDepth != nullptr) {
                if (!ReadAspect(/*stencilAspect=*/false, x, y, width, height, words)) {
                    return false;
                }
                outDepth->assign(pixelCount, 0.0f);
                for (SizeT i = 0; i < pixelCount; ++i) {
                    const Uint32 bits = words[i * 4u];
                    Float value = 0.0f;
                    Memcpy(&value, &bits, sizeof(value));
                    (*outDepth)[i] = value;
                }
            }
            if (outStencil != nullptr) {
                if (!ReadAspect(/*stencilAspect=*/true, x, y, width, height, words)) {
                    return false;
                }
                outStencil->assign(pixelCount, 0);
                for (SizeT i = 0; i < pixelCount; ++i) {
                    (*outStencil)[i] = static_cast<Uint8>(words[i * 4u] & 0xFFu);
                }
            }
            return true;
        }
    } // namespace DepthStencilSamplingReadImpl

    // One normalized depth value per pixel, tightly packed. Which native read a driver
    // accepts depends on the attached format: a fixed-point depth buffer takes
    // GL_UNSIGNED_INT, while a floating-point one (DEPTH_COMPONENT32F,
    // DEPTH32F_STENCIL8 - what dEQP's own fbo-surface-type wrapper framebuffer uses)
    // rejects it with GL_INVALID_OPERATION and only reads back as GL_FLOAT. Try both.
    static Bool ReadDepthValuesNative(GLint x, GLint y, GLsizei width, GLsizei height, Vector<Float>& outDepth) {
        outDepth.assign(static_cast<SizeT>(width) * static_cast<SizeT>(height), 0.0f);
        ScopedPixelPackBuffer packBuffer(0);
        ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});
        GLenum floatError = GL_INVALID_OPERATION;
        if (!MG_Config::Features.EsprytForceDepthStencilReadbackEmulation) {
            Vector<Uint32> raw(outDepth.size());
            // Drain first: a stale flag some earlier best-effort call left queued
            // must not be misattributed to this read (it would silently drop the
            // whole readback in production builds where ErrorLopper is compiled out).
            ClearGLErrors();
            g_GLESFuncs.glReadPixels(x, y, width, height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, raw.data());
            if (g_GLESFuncs.glGetError() == GL_NO_ERROR) {
                for (SizeT i = 0; i < outDepth.size(); ++i) {
                    outDepth[i] = static_cast<Float>(static_cast<Double>(raw[i]) / 4294967295.0);
                }
                return true;
            }

            ClearGLErrors();
            g_GLESFuncs.glReadPixels(x, y, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, outDepth.data());
            floatError = g_GLESFuncs.glGetError();
            if (floatError == GL_NO_ERROR) {
                return true;
            }
        }

        // Neither native spelling exists on this driver (which is the ordinary case: ES has
        // no depth readback in core and GL_NV_read_depth is rare), so sample the attachment
        // instead. The scoped pack state above is irrelevant to that path - it reads its own
        // scratch colour target - but harmless, and leaving it in place keeps the restore in
        // one place.
        if (DepthStencilSamplingReadImpl::Read(x, y, width, height, &outDepth, /*outStencil=*/nullptr)) {
            return true;
        }
        MGLOG_E_ONCE("ReadPixels: no depth readback path is available: native reads failed with %s and the "
                "sampling emulation could not service the source",
                MG_Util::ConvertGLEnumToString(floatError).c_str());
        return false;
    }

    // GL_DEPTH_COMPONENT readback into the client's layout, honouring the PACK pixel-store
    // parameters. GL 4.6 core 18.2.8: the normalized depth is written as-is for GL_FLOAT and
    // scaled into the full range of whichever integer width the client asked for otherwise.
    static Bool ReadPixelsDepthComponent(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type,
                                         void* pixels) {
        SizeT dstPixelBytes = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE: dstPixelBytes = sizeof(Uint8); break;
        case GL_UNSIGNED_SHORT: dstPixelBytes = sizeof(Uint16); break;
        case GL_UNSIGNED_INT: dstPixelBytes = sizeof(Uint32); break;
        case GL_FLOAT: dstPixelBytes = sizeof(GLfloat); break;
        default: return false;
        }
        if (width <= 0 || height <= 0) {
            return true;
        }

        Vector<Float> depth;
        if (!ReadDepthValuesNative(x, y, width, height, depth)) {
            return true; // already reported; nothing was written, as before
        }

        StoreReadbackRowsToClient(width, height, dstPixelBytes, pixels, "depth",
                                  [&](GLsizei row, Uint8* dst) {
                                      const Float* srcRow =
                                          depth.data() + static_cast<SizeT>(row) * static_cast<SizeT>(width);
                                      for (GLsizei col = 0; col < width; ++col) {
                                          switch (type) {
                                          case GL_UNSIGNED_BYTE:
                                              dst[col] = static_cast<Uint8>(
                                                  NormalizedDepthToUnsigned(srcRow[col], 255.0));
                                              break;
                                          case GL_UNSIGNED_SHORT:
                                              reinterpret_cast<Uint16*>(dst)[col] = static_cast<Uint16>(
                                                  NormalizedDepthToUnsigned(srcRow[col], 65535.0));
                                              break;
                                          case GL_UNSIGNED_INT:
                                              reinterpret_cast<Uint32*>(dst)[col] =
                                                  NormalizedDepthToUnsigned(srcRow[col], 4294967295.0);
                                              break;
                                          default:
                                              reinterpret_cast<GLfloat*>(dst)[col] = srcRow[col];
                                              break;
                                          }
                                      }
                                  });
        return true;
    }

    // One stencil byte per pixel, tightly packed. ES has no guaranteed stencil readback
    // at all: GL_STENCIL_INDEX needs GL_NV_read_stencil, and a driver without it rejects
    // the read outright. Where the attachment is a combined depth-stencil buffer the
    // packed GL_DEPTH_STENCIL read (which the packed_depth_stencil.verify_* cases already
    // rely on) carries the same bytes in its low octet, so use that as the fallback.
    static Bool ReadStencilBytesNative(GLint x, GLint y, GLsizei width, GLsizei height, Vector<Uint8>& outStencil) {
        outStencil.assign(static_cast<SizeT>(width) * static_cast<SizeT>(height), 0);
        ScopedPixelPackBuffer packBuffer(0);
        ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});
        GLenum packedError = GL_INVALID_OPERATION;
        if (!MG_Config::Features.EsprytForceDepthStencilReadbackEmulation) {
            // Drain first: see ReadDepthValuesNative.
            ClearGLErrors();
            g_GLESFuncs.glReadPixels(x, y, width, height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, outStencil.data());
            if (g_GLESFuncs.glGetError() == GL_NO_ERROR) {
                return true;
            }

            Vector<Uint32> packed(outStencil.size(), 0);
            ClearGLErrors();
            g_GLESFuncs.glReadPixels(x, y, width, height, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, packed.data());
            if (g_GLESFuncs.glGetError() == GL_NO_ERROR) {
                for (SizeT i = 0; i < outStencil.size(); ++i) {
                    outStencil[i] = static_cast<Uint8>(packed[i] & 0xFFu);
                }
                return true;
            }

            // A DEPTH32F_STENCIL8 attachment rejects the 24_8 type: its packed layout is a
            // float depth followed by a padded stencil byte, eight bytes per pixel with the
            // index at offset 4.
            Vector<Uint8> packed32f(outStencil.size() * 8u, 0);
            ClearGLErrors();
            g_GLESFuncs.glReadPixels(x, y, width, height, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV,
                                     packed32f.data());
            packedError = g_GLESFuncs.glGetError();
            if (packedError == GL_NO_ERROR) {
                for (SizeT i = 0; i < outStencil.size(); ++i) {
                    outStencil[i] = packed32f[i * 8u + 4u];
                }
                return true;
            }
        }

        // No native spelling worked, which is the ordinary case on ES: sample the stencil
        // half of the attachment instead.
        if (DepthStencilSamplingReadImpl::Read(x, y, width, height, /*outDepth=*/nullptr, &outStencil)) {
            return true;
        }
        MGLOG_E_ONCE("ReadPixels: no stencil readback path is available: native reads failed with %s and the "
                "sampling emulation could not service the source",
                MG_Util::ConvertGLEnumToString(packedError).c_str());
        return false;
    }

    // GL_STENCIL_INDEX readback into the client's integer layout, honouring the PACK
    // pixel-store parameters. Handles the widths desktop clients ask for; the stencil
    // values themselves are always 8 bits.
    static Bool ReadPixelsStencilViaNative(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type,
                                           void* pixels) {
        // GL 4.6 core 18.2.8: a stencil index is written unconverted into whichever integer width
        // the client asked for, and converted to a float value for GL_FLOAT. The signed widths are
        // as legal as the unsigned ones - the CTS reads stencil with GL_INT - and rejecting them
        // here used to let the call fall through to a native ES read the driver refuses, after
        // which nothing was written at all and the caller kept its zeros.
        SizeT dstPixelBytes = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE:
        case GL_BYTE: dstPixelBytes = sizeof(Uint8); break;
        case GL_UNSIGNED_SHORT:
        case GL_SHORT: dstPixelBytes = sizeof(Uint16); break;
        case GL_UNSIGNED_INT:
        case GL_INT: dstPixelBytes = sizeof(Uint32); break;
        case GL_FLOAT: dstPixelBytes = sizeof(GLfloat); break;
        default: return false;
        }
        if (width <= 0 || height <= 0) {
            return true;
        }

        Vector<Uint8> raw;
        if (!ReadStencilBytesNative(x, y, width, height, raw)) {
            return true;
        }

        StoreReadbackRowsToClient(width, height, dstPixelBytes, pixels, "stencil",
                                  [&](GLsizei row, Uint8* dst) {
                                      const Uint8* srcRow =
                                          raw.data() + static_cast<SizeT>(row) * static_cast<SizeT>(width);
                                      for (GLsizei col = 0; col < width; ++col) {
                                          switch (type) {
                                          case GL_UNSIGNED_BYTE:
                                          case GL_BYTE:
                                              dst[static_cast<SizeT>(col)] = srcRow[col];
                                              break;
                                          case GL_UNSIGNED_SHORT:
                                          case GL_SHORT:
                                              reinterpret_cast<Uint16*>(dst)[col] = srcRow[col];
                                              break;
                                          case GL_FLOAT:
                                              reinterpret_cast<GLfloat*>(dst)[col] = static_cast<GLfloat>(srcRow[col]);
                                              break;
                                          default:
                                              reinterpret_cast<Uint32*>(dst)[col] = srcRow[col];
                                              break;
                                          }
                                      }
                                  });
        return true;
    }

    // GL_DEPTH_STENCIL readback: the two aspects are fetched separately and woven into the
    // packed layout the client asked for (GL 4.6 core table 8.6). This is what
    // KHR-GL3x.packed_depth_stencil.verify_read_pixels / verify_get_tex_image /
    // verify_copy_tex_image read their gradients with.
    static Bool ReadPixelsDepthStencilPacked(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type,
                                             void* pixels) {
        SizeT dstPixelBytes = 0;
        switch (type) {
        case GL_UNSIGNED_INT_24_8: dstPixelBytes = sizeof(Uint32); break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: dstPixelBytes = sizeof(Float) + sizeof(Uint32); break;
        default: return false;
        }
        if (width <= 0 || height <= 0) {
            return true;
        }

        Vector<Float> depth;
        Vector<Uint8> stencil;
        if (!ReadDepthValuesNative(x, y, width, height, depth)) {
            return true;
        }
        if (!ReadStencilBytesNative(x, y, width, height, stencil)) {
            return true;
        }

        StoreReadbackRowsToClient(
            width, height, dstPixelBytes, pixels, "packed depth/stencil", [&](GLsizei row, Uint8* dst) {
                const SizeT base = static_cast<SizeT>(row) * static_cast<SizeT>(width);
                for (GLsizei col = 0; col < width; ++col) {
                    const Uint32 stencilIndex = stencil[base + static_cast<SizeT>(col)];
                    const Float depthValue = depth[base + static_cast<SizeT>(col)];
                    if (type == GL_UNSIGNED_INT_24_8) {
                        reinterpret_cast<Uint32*>(dst)[col] =
                            (NormalizedDepthToUnsigned(depthValue, 16777215.0) << 8) | stencilIndex;
                    } else {
                        // Depth float first, then a word whose low octet is the index and
                        // whose top 24 bits are unused.
                        Uint8* pixel = dst + static_cast<SizeT>(col) * (sizeof(Float) + sizeof(Uint32));
                        Memcpy(pixel, &depthValue, sizeof(depthValue));
                        Memcpy(pixel + sizeof(Float), &stencilIndex, sizeof(stencilIndex));
                    }
                }
            });
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

    // The bit pattern of 1.0 in a wide-read component type: what GL reports for a channel the
    // attachment's format does not have.
    static void FillWideReadOneBits(GLenum componentType, Uint8* oneBits) {
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
    }

    // Overwrites the alpha of a 4-channel wide read with the format's implied 1.0. Used for an
    // attachment the backend widened from three channels to keep it colour-renderable: the storage
    // has a real alpha channel holding whatever the draw wrote, but the format the application
    // asked for has none, and GL reads a missing channel back as one.
    static void ForceWideReadAlphaToOne(Vector<Uint8>& data, SizeT pixelCount, GLenum componentType) {
        const SizeT componentSize = GetReadbackComponentSize(componentType);
        if (componentSize == 0 || data.size() < pixelCount * 4 * componentSize) {
            return;
        }
        Uint8 oneBits[4] = {0, 0, 0, 0};
        FillWideReadOneBits(componentType, oneBits);
        for (SizeT i = 0; i < pixelCount; ++i) {
            Memcpy(data.data() + (i * 4 + 3) * componentSize, oneBits, componentSize);
        }
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
        FillWideReadOneBits(componentType, oneBits);

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

    static void DrainESErrors() { DrainDriverErrors("ReadPixels"); }

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
    // `forceOpaqueAlpha`: the source image is a three-channel format the backend widened to four to
    // keep it colour-renderable, so its alpha channel holds whatever the draw wrote and has to be
    // answered with the 1.0 the application's format implies. Passed in rather than derived here:
    // glReadPixels reads the bound READ framebuffer, but glGetTexImage reads a texture through a
    // scratch framebuffer, so the frontend's READ binding describes a different image entirely -
    // consulting it there would both miss real widenings and corrupt readbacks of ordinary
    // textures taken while some unrelated widened attachment happened to be bound.
    // The image-format widening's READ half, for the seven normalized formats whose carrier holds
    // their channels as INTEGER CODES (GL_RGBA16 stored as a GL_RGBA16UI - see
    // TextureImpl::GetImageBindableStorageWidening). Nothing else in the readback would get those
    // right: the attachment is an integer one while the application's format is normalized, so the
    // class check below would refuse the read outright, and a repack that got past it would hand
    // back 65535.0 where GL owes 1.0.
    //
    // Inactive (ChannelMax all zero) for every other read, which is all but a handful.
    struct NormalizedImageCarrierRead {
        Uint ChannelMax[4] = {0u, 0u, 0u, 0u};
        Bool SignedNormalized = false;

        Bool Active() const { return ChannelMax[0] != 0u; }
    };

    static Bool ReadPixelsViaFormatConversion(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                              GLenum type, void* pixels, Bool honorPackImageParams,
                                              Bool applyFixedPointReadClamp, Bool forceOpaqueAlpha,
                                              const NormalizedImageCarrierRead& normalizedCarrier = {}) {
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
        // A normalized image carrier is EXACTLY the case where the two disagree on purpose, and
        // it is the caller - which knows the TEXTURE being read, not just the attachment - that
        // says so. An integer client format through such a carrier is not a shape GL can ask for
        // (the frontend format is normalized), so it is refused here rather than converted.
        if (normalizedCarrier.Active() && (mapping.isInteger || !integerAttachment)) {
            MGLOG_E_ONCE("Readback conversion: a normalized image carrier was read as %s, which is not a "
                    "normalized client format; skipping",
                    MG_Util::ConvertGLEnumToString(format).c_str());
            return true;
        }
        if (!normalizedCarrier.Active() && mapping.isInteger != integerAttachment) {
            MGLOG_E_ONCE("Readback conversion: integer-ness of format %s does not match the read buffer, skipping",
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
        if (normalizedCarrier.Active()) {
            // The storage IS an integer texture, whatever the application's format says, so the
            // only read that can answer is the integer one. The codes it hands back are turned
            // into the floats the client asked for below.
            candidates[candidateCount++] = {GL_RGBA_INTEGER, GL_UNSIGNED_INT};
        } else if (mapping.isInteger) {
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
            MGLOG_E_ONCE("Readback conversion: ES accepted no wide read type for format %s type %s, skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return true;
        }

        if (normalizedCarrier.Active()) {
            // GL 4.6 2.3.5, the same conversion the shader-side unpack does and with the same
            // denominators, so a texel an imageStore wrote and a texel the upload seeded read back
            // identically: f = c / (2^b - 1) unsigned, f = max(c / (2^(b-1) - 1), -1) signed, with
            // the signed code recovered from the low sixteen bits of the unsigned channel.
            const SizeT pixelCount = static_cast<SizeT>(width) * static_cast<SizeT>(height);
            Vector<Uint8> floatWide(pixelCount * 4 * sizeof(Float));
            auto* dst = reinterpret_cast<Float*>(floatWide.data());
            const auto* src = reinterpret_cast<const Uint32*>(wide.data());
            for (SizeT i = 0; i < pixelCount; ++i) {
                for (SizeT channel = 0; channel < 4; ++channel) {
                    const Uint32 code = src[i * 4 + channel];
                    const auto denominator = static_cast<Float>(normalizedCarrier.ChannelMax[channel]);
                    if (normalizedCarrier.SignedNormalized) {
                        const auto signedCode = static_cast<Int16>(static_cast<Uint16>(code));
                        dst[i * 4 + channel] =
                            std::max(static_cast<Float>(signedCode) / denominator, -1.0f);
                    } else {
                        dst[i * 4 + channel] = static_cast<Float>(code) / denominator;
                    }
                }
            }
            wide = Move(floatWide);
            wideType = GL_FLOAT;
            readChannels = 4;
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

        // Undo the three-channel widening (see the parameter's comment). Deliberately not gated on
        // applyFixedPointReadClamp: that flag implements GL_CLAMP_READ_COLOR, which glGetTexImage
        // is exempt from, whereas "a format without alpha reads as 1.0" is the format's own
        // semantics and applies to every read.
        if (forceOpaqueAlpha) {
            ForceWideReadAlphaToOne(wide, static_cast<SizeT>(width) * static_cast<SizeT>(height), wideType);
        }

        // GL clamps a read from a fixed-point colour buffer to [0,1] (GL_CLAMP_READ_COLOR
        // defaults to GL_FIXED_ONLY). Formats the backend substitutes with a floating-point
        // one keep the out-of-range value the app stored, so apply the clamp here - a
        // GL_R16_SNORM target holding -0.125 must still read back as 0.
        // glReadPixels only: GL_CLAMP_READ_COLOR does not apply to glGetTexImage, which
        // reaches this helper through the same scratch-framebuffer path.
        if (applyFixedPointReadClamp && FramebufferImpl::IsFixedPointFallbackReadAttachment()) {
            const SizeT componentSize = GetReadbackComponentSize(wideType);
            const SizeT valueCount = componentSize != 0 ? wide.size() / componentSize : 0;
            switch (wideType) {
            case GL_FLOAT: {
                auto* values = reinterpret_cast<Float*>(wide.data());
                for (SizeT i = 0; i < valueCount; ++i) values[i] = std::clamp(values[i], 0.0f, 1.0f);
                break;
            }
            case GL_HALF_FLOAT: {
                auto* values = reinterpret_cast<Uint16*>(wide.data());
                for (SizeT i = 0; i < valueCount; ++i) {
                    values[i] = MG_Util::EncodeFloatToHalfBits(
                        std::clamp(MG_Util::DecodeHalfBitsToFloat(values[i]), 0.0f, 1.0f));
                }
                break;
            }
            case GL_SHORT: {
                // Signed normalized: the negative half is exactly what the clamp removes.
                auto* values = reinterpret_cast<Int16*>(wide.data());
                for (SizeT i = 0; i < valueCount; ++i) values[i] = std::max<Int16>(values[i], 0);
                break;
            }
            case GL_BYTE: {
                auto* values = reinterpret_cast<Int8*>(wide.data());
                for (SizeT i = 0; i < valueCount; ++i) values[i] = std::max<Int8>(values[i], 0);
                break;
            }
            default:
                break;
            }
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

        // glGetTexImage returns the stored texels, and for a packed internal format read with the
        // matching client type the shadow word already IS the client word. Decoding it to float and
        // re-encoding would canonicalize an RGB9_E5 shared exponent (0xf8fc0000 -> 0xe7e00000: the
        // same value, different bits), so those pairs copy the words straight through.
        if (MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer(
                textureMipmapObject->GetFormat(), MG_Util::ConvertGLEnumToTextureInputFormat(format),
                MG_Util::ConvertGLEnumToTexturePixelDataType(type))) {
            if (!ReadbackImpl::StorePackedWordsToClient(static_cast<const Uint8*>(shadow), width, sliceHeight,
                                                        sliceCount, type, pixels, applyPackImageParams)) {
                return false;
            }
            MGLOG_D("GetTexImage: copied %s/%s verbatim from the CPU shadow copy",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return true;
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

    // ---- Bit-exact readback of a 32-bit packed colour level ---------------------------------------
    //
    // glGetTexImage of a packed format read with its OWN client type owes the application the words
    // the image HOLDS, and neither of the two routes above can promise that once anything other than
    // a glTexImage has written the level:
    //
    //   * the colour-attachment route reads GL_RGBA/GL_FLOAT and re-encodes, which canonicalizes an
    //     RGB9_E5 shared exponent (0xf8fc0000 -> 0xe7e00000, same value, different bits) and
    //     collapses an R11F_G11F_B10F NaN to the canonical payload 1
    //     (MG_Util::EncodeFloatToUnsignedSmallFloat) - and a copy-image from RGB9_E5 lands exactly
    //     such a NaN in the 10-bit blue field every time, because the source's shared-exponent
    //     field is all ones;
    //   * the CPU shadow only ever holds what was UPLOADED, so for a level glCopyImageSubData wrote
    //     it answers with the PRE-COPY contents. MirrorCopyImageIntoDestinationShadow patches that
    //     up for the shapes it can address texel-exactly and declines for the rest - a renderbuffer
    //     source (which has no shadow to mirror from at all), a cube or 1D-array endpoint, a
    //     self-copy - and the decline is silent, so the stale words are served as truth.
    //
    // glCopyImageSubData is a raw texel-block move and EXT_copy_image puts every 32-bit colour
    // format in one compatibility class, so copying the level into a scratch GL_R32UI image and
    // reading THAT back as unsigned integers hands over the stored words themselves, whoever wrote
    // them. This is what lets the shadow stop being the authority for these formats: it is tried
    // first, and every step reports rather than guesses, so a driver that turns any of it down
    // simply leaves the old shadow/attachment fallbacks to run.
    static GLuint g_packedWordScratchTextureId = 0;
    static GLsizei g_packedWordScratchWidth = 0;
    static GLsizei g_packedWordScratchHeight = 0;

    // Grow-only, so a readback sweep over a mip chain allocates once. Zero when the driver refused
    // the storage, which is a decline and not an error.
    static GLuint EnsurePackedWordScratchTexture(GLsizei width, GLsizei height) {
        if (g_packedWordScratchTextureId != 0 && g_packedWordScratchWidth >= width &&
            g_packedWordScratchHeight >= height) {
            return g_packedWordScratchTextureId;
        }
        const GLsizei newWidth = std::max(width, g_packedWordScratchWidth);
        const GLsizei newHeight = std::max(height, g_packedWordScratchHeight);
        if (g_packedWordScratchTextureId != 0) {
            // A scratch FBO may still name the old id, and the driver is free to hand the same
            // number back for the replacement - which would false-skip the re-attach.
            ScratchFBOImpl::NoteTextureIdDeleted(g_packedWordScratchTextureId);
            g_GLESFuncs.glDeleteTextures(1, &g_packedWordScratchTextureId);
            g_packedWordScratchTextureId = 0;
            g_packedWordScratchWidth = 0;
            g_packedWordScratchHeight = 0;
        }
        GLuint texture = 0;
        g_GLESFuncs.glGenTextures(1, &texture);
        if (texture == 0) return 0;

        ClearGLErrors();
        TextureImpl::ActivateTextureUnit(TextureImpl::TempTextureUnit);
        g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, texture);
        // Immutable single-level storage: glCopyImageSubData wants a complete image, and
        // glTexStorage clamps TEXTURE_MAX_LEVEL, which is what makes a one-level texture complete
        // under the default mipmapping filter.
        g_GLESFuncs.glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, newWidth, newHeight);
        const GLenum storageError = g_GLESFuncs.glGetError();
        // Re-bind whatever the binding cache says lives on the temp unit, so the cache stays
        // truthful without a driver query (same discipline as CopyR32FTexture2D).
        auto* cachedBound = TextureImpl::g_boundTexturesCache[TextureImpl::TempTextureUnit]
                                                            [static_cast<SizeT>(TextureTarget::Texture2D)];
        g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, cachedBound ? cachedBound->GetBackendTextureId() : 0);
        if (storageError != GL_NO_ERROR) {
            g_GLESFuncs.glDeleteTextures(1, &texture);
            MGLOG_D("GetTexImage: no %dx%d GL_R32UI scratch image (%s); the verbatim word readback is unavailable",
                    newWidth, newHeight, MG_Util::ConvertGLEnumToString(storageError).c_str());
            return 0;
        }
        g_packedWordScratchTextureId = texture;
        g_packedWordScratchWidth = newWidth;
        g_packedWordScratchHeight = newHeight;
        return texture;
    }

    static void ReleasePackedWordScratchTexture() {
        // The ES context (and the name with it) is gone; deleting here would target a recycled
        // name in the successor context.
        g_packedWordScratchTextureId = 0;
        g_packedWordScratchWidth = 0;
        g_packedWordScratchHeight = 0;
    }

    // One slice of `backendTarget`'s level, as width*height stored 32-bit words in `outWords`.
    static Bool ReadPackedLevelWordsViaScratch(GLuint texture, GLenum backendTarget, GLint level, GLint slice,
                                               GLsizei width, GLsizei height, Uint32* outWords) {
        if (texture == 0 || outWords == nullptr || width <= 0 || height <= 0 || level < 0 || slice < 0) return false;
        if (!g_GLESFuncs.glCopyImageSubData) return false;

        // Horizontal bands, so neither the scratch image nor the staging buffer scales with the
        // level. The scratch is grow-only on purpose - a sweep down a mip chain must not
        // reallocate per level - which without a band cap would leave a 4096x4096 readback's
        // 64 MiB image parked for the rest of the process. The cap is 1 MiB of GL_R32UI, with
        // 4 MiB of staging behind it because the read lands four words per texel.
        constexpr SizeT kMaxScratchTexels = SizeT{1} << 18;
        const GLsizei bandRows = std::max<GLsizei>(
            1, static_cast<GLsizei>(std::min<SizeT>(kMaxScratchTexels / static_cast<SizeT>(width),
                                                    static_cast<SizeT>(height))));
        const GLuint scratch = EnsurePackedWordScratchTexture(width, bandRows);
        if (scratch == 0) return false;

        ScopedFramebufferBinding readBinding(/*saveRead=*/true, /*saveDraw=*/false);
        auto& scratchFB = ScratchFBOImpl::BlitReadFramebuffer();
        FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, ScratchFBOImpl::EnsureId(scratchFB));
        ScratchFBOImpl::EnsureColorAttachment2D(scratchFB, GL_READ_FRAMEBUFFER, scratch, GL_TEXTURE_2D, 0);
        ScratchFBOImpl::EnsureReadBuffer(scratchFB, GL_COLOR_ATTACHMENT0);
        if (g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            MGLOG_D("GetTexImage: the GL_R32UI scratch attachment is incomplete; falling back");
            return false;
        }

        // GL_RGBA_INTEGER/GL_UNSIGNED_INT is the one combination ES guarantees for an integer
        // colour buffer, so the read lands four words per texel and the red one is compacted out
        // here. The PACK scope is the tight default rather than the application's, so a row comes
        // back packed at exactly `width * 4` words. One glGetError covers the whole loop: it
        // accumulates, and a failure anywhere means the caller falls back rather than trusting a
        // partial result.
        const SizeT wordsPerRow = static_cast<SizeT>(width) * 4;
        Vector<Uint32> staging(static_cast<SizeT>(bandRows) * wordsPerRow);
        ScopedPixelPackBuffer packBuffer(0);
        ScopedPackState packState(PixelStoreImpl::PackState{4, 0, 0, 0});
        ClearGLErrors();
        for (GLsizei y = 0; y < height; y += bandRows) {
            const GLsizei rows = std::min(bandRows, height - y);
            g_GLESFuncs.glCopyImageSubData(texture, backendTarget, level, 0, y, slice, scratch, GL_TEXTURE_2D, 0, 0,
                                           0, 0, width, rows, 1);
            g_GLESFuncs.glReadPixels(0, 0, width, rows, GL_RGBA_INTEGER, GL_UNSIGNED_INT, staging.data());
            for (GLsizei row = 0; row < rows; ++row) {
                const Uint32* srcRow = staging.data() + static_cast<SizeT>(row) * wordsPerRow;
                Uint32* dstRow = outWords + static_cast<SizeT>(y + row) * static_cast<SizeT>(width);
                for (GLsizei x = 0; x < width; ++x) dstRow[x] = srcRow[static_cast<SizeT>(x) * 4];
            }
        }
        const GLenum error = g_GLESFuncs.glGetError();
        if (error != GL_NO_ERROR) {
            MGLOG_D("GetTexImage: the GL_R32UI word readback of %s was refused (%s); falling back",
                    MG_Util::ConvertGLEnumToString(backendTarget).c_str(),
                    MG_Util::ConvertGLEnumToString(error).c_str());
            return false;
        }
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

    // The (format, type) pairs the depth/stencil helpers below can service. They are not
    // covered by the colour tables above - GetReadbackChannelMapping has no entry for any
    // depth or stencil format, so without this gate a read the helpers CAN serve (a
    // GL_UNSIGNED_SHORT depth, a GL_SHORT stencil) is turned away before it reaches them.
    static Bool IsSupportedDepthStencilReadPixelsPair(GLenum format, GLenum type) {
        switch (format) {
        case GL_DEPTH_COMPONENT:
            return type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT ||
                   type == GL_FLOAT;
        case GL_STENCIL_INDEX:
            return type == GL_UNSIGNED_BYTE || type == GL_BYTE || type == GL_UNSIGNED_SHORT || type == GL_SHORT ||
                   type == GL_UNSIGNED_INT || type == GL_INT || type == GL_FLOAT;
        case GL_DEPTH_STENCIL:
            return type == GL_UNSIGNED_INT_24_8 || type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        default:
            return false;
        }
    }

    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        MGLOG_D("ReadPixels: x=%d y=%d w=%d h=%d format=%s type=%s pixels=%p", x, y, width, height,
                MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str(), pixels);

        // Combinations the ES driver has always handled directly keep the native path; other color layouts go
        // through the wide-format conversion path. Anything still uncovered degrades to a logged no-op instead
        // of killing the process; spec-invalid combinations are already rejected with GL errors at the state layer.
        const Bool useNativeReadback = (IsLegacyNativeReadPixelsFormat(format) && IsLegacyNativeReadPixelsType(type)) ||
                                       IsSupportedDepthStencilReadPixelsPair(format, type);
        ReadbackChannelMapping conversionMapping{};
        const Bool convertible = GetReadbackChannelMapping(format, conversionMapping) &&
                                 GetReadbackDstPixelSize(conversionMapping, type) != 0;
        if (!useNativeReadback && !convertible) {
            MGLOG_E_ONCE("ReadPixels: format %s with type %s is not implemented yet, skipping readback",
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
            MGLOG_E_ONCE("ReadPixels: bound READ FBO is not complete");
            return;
        }
        // ES only guarantees GL_RGBA/GL_UNSIGNED_BYTE and GL_RGBA_INTEGER/GL_(UNSIGNED_)INT for the
        // matching attachment class; every other convertible color layout (including GL_RGBA/GL_FLOAT
        // and legacy GL_RED reads) goes through the wide-format conversion, which picks a wide type
        // the driver accepts for the current attachment. GL_PACK_SWAP_BYTES has no ES equivalent, so
        // it always takes the conversion path (which swaps on the CPU).
        const Bool packSwapBytes = MG_State::pGLContext->GetPixelStoreParameters(false).SwapBytes;
        // The read buffer is what glReadPixels reads, so the frontend's READ binding is exactly
        // the right thing to ask here.
        const Bool forceOpaqueAlpha = FramebufferImpl::IsAlphaWidenedFallbackReadAttachment();
        // An attachment widened from three channels to stay colour-renderable also has to leave
        // the fast pair: only the conversion path knows to answer its alpha with the 1.0 the
        // application's format implies instead of whatever the draw wrote into the added channel.
        const Bool nativeFastPair = !packSwapBytes && !forceOpaqueAlpha &&
                                    ((format == GL_RGBA && type == GL_UNSIGNED_BYTE) ||
                                     (format == GL_RGBA_INTEGER && (type == GL_UNSIGNED_INT || type == GL_INT)));
        if (convertible && !nativeFastPair) {
            if (ReadPixelsViaFormatConversion(x, y, width, height, format, type, pixels,
                                              /*honorPackImageParams=*/false, /*applyFixedPointReadClamp=*/true,
                                              forceOpaqueAlpha)) {
                MGLOG_D("ReadPixels: finished via client-format conversion");
                return;
            }
            MGLOG_E_ONCE("ReadPixels: format %s with type %s is not implemented yet, skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return;
        }
        // Every depth and stencil read goes through the helpers, not just the widening ones:
        // ES has no guaranteed readback for either aspect, so even a byte-for-byte case
        // needs the fallback chain (the other native spelling, then the sampling emulation)
        // on a driver without GL_NV_read_depth / GL_NV_read_stencil.
        if (format == GL_DEPTH_COMPONENT && ReadPixelsDepthComponent(x, y, width, height, type, pixels)) {
            MGLOG_D("ReadPixels: finished via depth readback helper");
            return;
        }
        if (format == GL_STENCIL_INDEX && ReadPixelsStencilViaNative(x, y, width, height, type, pixels)) {
            MGLOG_D("ReadPixels: finished via stencil readback helper");
            return;
        }
        if (format == GL_DEPTH_STENCIL && ReadPixelsDepthStencilPacked(x, y, width, height, type, pixels)) {
            MGLOG_D("ReadPixels: finished via packed depth/stencil readback helper");
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
                MGLOG_E_ONCE("ReadPixels: No backend buffer found for PBO %u.",
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
            if (ReadPixelsViaFormatConversion(x, y, width, height, format, type, pixels,
                                              /*honorPackImageParams=*/false, /*applyFixedPointReadClamp=*/true,
                                              forceOpaqueAlpha)) {
                MGLOG_D("ReadPixels: finished via client-format conversion after native failure");
                return;
            }
            MGLOG_E_ONCE("ReadPixels: native read of %s/%s failed (%s) and no conversion path covers it, "
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
                // Serial bumped with no backend op; re-open the buffer draw-clean memos.
                BufferImpl::BumpBufferMutationEpoch();
                MGLOG_D("ReadPixels: Unmapping PBO");
                g_GLESFuncs.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            } else {
                MGLOG_E_ONCE("ReadPixels: glMapBufferRange returned nullptr");
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
        if (format == GL_DEPTH_STENCIL || format == GL_DEPTH_COMPONENT) {
            return IsSupportedDepthStencilReadPixelsPair(format, type);
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
            MGLOG_E_ONCE("GetTexImage: format %s with type %s is not implemented yet, skipping readback",
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

        auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get());

        if (!backendTextureSlot || !*backendTextureSlot) {
            MGLOG_E_ONCE("GetTexImage: No backend texture found for texture %u.",
                    textureObject ? textureObject->GetExternalIndex() : 0);
            return;
        }

        GLuint backendTexId = (*backendTextureSlot)->GetBackendTextureId();
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
        if (format == GL_DEPTH_STENCIL || format == GL_DEPTH_COMPONENT) {
            ScratchFBOImpl::EnsureDepthAttachment2D(
                tempFB, GL_READ_FRAMEBUFFER, backendTexId,
                backendAttachTarget == GL_UNKNOWN_MGL ? target : backendAttachTarget, level,
                /*withStencil=*/format == GL_DEPTH_STENCIL);
        } else if (backendAttachTarget == GL_TEXTURE_3D || backendAttachTarget == GL_TEXTURE_2D_ARRAY ||
                   backendAttachTarget == GL_TEXTURE_CUBE_MAP_ARRAY) {
            // ES cannot attach 3D/array textures through glFramebufferTexture2D; layer 0 here, and
            // the deeper slices one at a time in the per-layer loop below. A CUBE MAP ARRAY is in
            // this list for the same reason its layer-faces are addressed as array layers:
            // glFramebufferTexture2D has no target token for it, so the 2D attach it used to take
            // left the scratch FBO incomplete and every read fell through to the (stale) CPU
            // shadow - which is exactly the all-zero result the conformance suite saw.
            ScratchFBOImpl::EnsureColorAttachmentLayer(tempFB, GL_READ_FRAMEBUFFER, backendTexId, level, 0);
        } else {
            ScratchFBOImpl::EnsureColorAttachment2D(
                tempFB, GL_READ_FRAMEBUFFER, backendTexId,
                backendAttachTarget == GL_UNKNOWN_MGL ? target : backendAttachTarget, level);
        }
        if (format != GL_DEPTH_STENCIL && format != GL_DEPTH_COMPONENT) {
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
            MGLOG_E_ONCE("GetTexImage: Texture storage type Buffer is not supported.");
            return;
        }

        auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());

        auto& levelRange = textureMipmapObject->GetLevelRange();
        MGLOG_D("GetTexImage: mipmap level range = [%d, %d]", levelRange.x(), levelRange.y());

        // levelRange.y() is GL_TEXTURE_MAX_LEVEL, an inclusive level index — a single-level
        // texture has range [0, 0] and level 0 must be readable.
        if (static_cast<Uint>(level) < levelRange.x() || static_cast<Uint>(level) > levelRange.y()) {
            MGLOG_E_ONCE("GetTexImage: Requested level %d is out of range (base level %u, max level %u), skipping readback",
                    level, levelRange.x(), levelRange.y());
            return;
        }

        auto size = textureMipmapObject->GetMipmapTexelSize(MG_Util::ConvertGLEnumToTextureUploadTarget(target), level);

        // GL_TEXTURE_1D_ARRAY keeps its LAYERS in the state-side height (that is what
        // glTexImage2D(GL_TEXTURE_1D_ARRAY, w, layers) means), while the ES texture behind it is a
        // 2D array of height 1 with the layers in depth - GetBackendUploadSize performs exactly
        // that swap on the way in. Everything below addresses the ES image, so the same swap has
        // to happen here: without it the readback asked layer 0 for a `layers`-row rectangle it
        // does not have, and every layer but the first came back undefined (all zeroes on Adreno,
        // KHR-GL4x.shader_image_load_store.basic-allTargets-*).
        const Bool oneDimensionalArray = textureObject->GetTarget() == TextureTarget::Texture1DArray;
        if (oneDimensionalArray) {
            size = TextureImpl::GetBackendUploadSize(TextureTarget::Texture1DArray, size);
        }

        MGLOG_D("GetTexImage: mip level %d size = %dx%d", level, size.x(), size.y());

        // Prefer the client-format conversion for every convertible combination: the "native" ES pairs
        // are only guaranteed for matching attachment classes (e.g. GL_RGBA/GL_UNSIGNED_INT is invalid
        // for normalized attachments), while the conversion path reads a wide format that is always
        // accepted and repacks on the CPU.
        if (convertible) {
            // The image being read is this texture, not whatever the application left bound to
            // GL_READ_FRAMEBUFFER, so the widening question has to be asked of the texture.
            const Bool forceOpaqueAlpha =
                TextureImpl::BackendTextureFormatAddsAlpha(textureObject->GetFormat(), textureObject->GetTarget());
            // An image-bindable texture in one of the seven normalized formats has its ES storage
            // in a GL_RGBA16UI, holding the format's own channel CODES. glGetTexImage still owes
            // the application the NORMALIZED value, so the conversion has to be undone here - and
            // it can only be asked of the TEXTURE, which is why it is not derived from the
            // attachment the scratch framebuffer happens to hold.
            NormalizedImageCarrierRead normalizedCarrier;
            if (const auto imageWidening =
                    TextureImpl::GetImageBindableStorageWidening(textureObject->GetFormat());
                imageWidening && imageWidening.CarriesNormalizedCodes() &&
                (*backendTextureSlot)->RequiresImageBindableStorage()) {
                for (SizeT channel = 0; channel < 4; ++channel) {
                    normalizedCarrier.ChannelMax[channel] = imageWidening.ChannelMax[channel];
                }
                normalizedCarrier.SignedNormalized = imageWidening.SignedNormalized;
            }
            // GL_PACK_IMAGE_HEIGHT/GL_PACK_SKIP_IMAGES only apply to 3D/array image
            // readbacks (cube-map arrays address as arrays); 2D targets must ignore
            // them (GL 3.3 section 6.1.4). A 1D ARRAY is one of those 2D targets: GL hands it back
            // as a single two-dimensional image whose ROWS are the layers, so the layer stride is
            // one packed row and the image parameters do not enter into it - even though the ES
            // texture underneath is an array and is read one layer at a time.
            const Bool applyPackImageParams = !oneDimensionalArray &&
                                              (backendAttachTarget == GL_TEXTURE_3D ||
                                               backendAttachTarget == GL_TEXTURE_2D_ARRAY ||
                                               backendAttachTarget == GL_TEXTURE_CUBE_MAP_ARRAY);
            const GLsizei sliceCount = std::max(size.z(), 1);
            const Bool multiSlice = size.z() > 1;
            // glGetTexImage answers with the STORED texels, and for a packed format whose encoding
            // is not unique the GPU route below cannot: it reads GL_RGBA/GL_FLOAT and re-encodes,
            // which canonicalizes an RGB9_E5 shared exponent (0xf8fc0000 -> 0xe7e00000 - the same
            // value 8064, different words), and the conformance suite compares the words
            // ("CopyImageSubData modified contents of source image"). The scratch FBO does NOT
            // decide this for us: Adreno reports an RGB9_E5 colour attachment complete, so the
            // shadow branch further down was unreachable. Every other format still prefers the
            // GPU, so a rendered-into texture is unaffected.
            const Bool rawPackedWordRead = MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer(
                textureObject->GetFormat(), MG_Util::ConvertGLEnumToTextureInputFormat(format),
                MG_Util::ConvertGLEnumToTexturePixelDataType(type));
            // ...and the GPU CAN answer with the stored words after all, for any 32-bit packed
            // format and whoever wrote the level, by going through a scratch GL_R32UI image (see
            // ReadPackedLevelWordsViaScratch). Preferred over both routes below because it is the
            // only one that is right for a level glCopyImageSubData wrote: the shadow may never
            // have seen that write, and re-encoding the attachment cannot reproduce an RGB9_E5
            // shared exponent or an R11F_G11F_B10F NaN payload. A multisample image is excluded
            // because copy-image requires matching sample counts.
            if (rawPackedWordRead && textureObject->GetSamples() == 0) {
                // Copy-image addresses a cube map as ONE image with the face on z, where
                // glGetTexImage names the face in its target.
                const auto readUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(target);
                const GLint copyBaseSlice =
                    (readUploadTarget >= TextureUploadTarget::CubeMapPositiveX &&
                     readUploadTarget <= TextureUploadTarget::CubeMapNegativeZ)
                        ? static_cast<GLint>(readUploadTarget) -
                              static_cast<GLint>(TextureUploadTarget::CubeMapPositiveX)
                        : 0;
                const GLenum copyTarget =
                    TextureImpl::ConvertTextureTargetToBackendGLEnum(textureObject->GetTarget());
                const SizeT sliceWords = static_cast<SizeT>(size.x()) * static_cast<SizeT>(size.y());
                Vector<Uint32> words(sliceWords * static_cast<SizeT>(sliceCount));
                Bool allSlicesRead = true;
                for (GLsizei slice = 0; slice < sliceCount && allSlicesRead; ++slice) {
                    allSlicesRead = ReadPackedLevelWordsViaScratch(backendTexId, copyTarget, level,
                                                                   copyBaseSlice + slice, size.x(), size.y(),
                                                                   words.data() + sliceWords * static_cast<SizeT>(slice));
                }
                if (allSlicesRead &&
                    ReadbackImpl::StorePackedWordsToClient(reinterpret_cast<const Uint8*>(words.data()), size.x(),
                                                           size.y(), sliceCount, type, pixels,
                                                           applyPackImageParams)) {
                    MGLOG_D("GetTexImage: finished %d slice(s) via the bit-exact GL_R32UI word readback", sliceCount);
                    return;
                }
            }
            // The last resort for the one format the attachment route can never answer for: the
            // shadow is only right while nothing but a glTexImage has written the level, which is
            // why CopyImageSubData mirrors itself into it where it can.
            const Bool verbatimPackedShadowRead =
                MG_Util::PixelStoreProcessor::HasRedundantPackedEncoding(textureObject->GetFormat()) &&
                rawPackedWordRead;
            if (verbatimPackedShadowRead &&
                GetTexImageViaShadowConversion(textureMipmapObject,
                                               MG_Util::ConvertGLEnumToTextureUploadTarget(target), level, size.x(),
                                               size.y(), sliceCount, format, type, pixels, applyPackImageParams)) {
                MGLOG_D("GetTexImage: finished via shadow conversion (verbatim packed words)");
                return;
            }
            // A multi-slice read used to go to the CPU shadow outright, on the grounds that the
            // scratch FBO can only expose one layer at a time. But the shadow only holds what was
            // uploaded, so every slice that was rendered to came back stale - which is exactly what
            // a layered framebuffer produces, and what textures_storage_multisample_3d_* checks.
            // Attach the layers one at a time instead and read each off the GPU, keeping the shadow
            // for the formats the FBO cannot represent at all.
            if (multiSlice && tempFBOComplete &&
                (backendAttachTarget == GL_TEXTURE_3D || backendAttachTarget == GL_TEXTURE_2D_ARRAY ||
                 backendAttachTarget == GL_TEXTURE_CUBE_MAP_ARRAY)) {
                // Each slice is packed as its own 2D image, so the per-slice call must not apply
                // GL_PACK_SKIP_IMAGES / GL_PACK_IMAGE_HEIGHT itself - this walks the destination
                // over them, using the same layout StoreWideRowsToClient computes.
                const auto packParams = MG_State::pGLContext->GetPixelStoreParameters(false);
                const SizeT dstPixelBytes = GetReadbackDstPixelSize(conversionMapping, type);
                const SizeT rowPixels =
                    static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : size.x());
                const SizeT alignment =
                    packParams.Alignment > 0 ? static_cast<SizeT>(packParams.Alignment) : SizeT{1};
                const SizeT dstRowStride = (rowPixels * dstPixelBytes + alignment - 1) / alignment * alignment;
                const SizeT imageRows = applyPackImageParams && packParams.ImageHeight > 0
                    ? static_cast<SizeT>(packParams.ImageHeight)
                    : static_cast<SizeT>(size.y());
                const SizeT dstImageStride = imageRows * dstRowStride;
                const SizeT skipImages =
                    applyPackImageParams ? static_cast<SizeT>(std::max(packParams.SkipImages, 0)) : SizeT{0};

                Bool allSlicesRead = true;
                for (GLsizei slice = 0; slice < sliceCount; ++slice) {
                    ScratchFBOImpl::EnsureColorAttachmentLayer(tempFB, GL_READ_FRAMEBUFFER, backendTexId, level,
                                                               slice);
                    if (g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                        allSlicesRead = false;
                        break;
                    }
                    const SizeT sliceOffset = (skipImages + static_cast<SizeT>(slice)) * dstImageStride;
                    void* sliceDst = static_cast<Uint8*>(pixels) + sliceOffset;
                    if (!ReadPixelsViaFormatConversion(0, 0, size.x(), size.y(), format, type, sliceDst,
                                                       /*honorPackImageParams=*/false,
                                                       /*applyFixedPointReadClamp=*/false, forceOpaqueAlpha,
                                                       normalizedCarrier)) {
                        allSlicesRead = false;
                        break;
                    }
                }
                // Leave the scratch FBO on layer 0 so the single-slice paths below see what they
                // set up.
                ScratchFBOImpl::EnsureColorAttachmentLayer(tempFB, GL_READ_FRAMEBUFFER, backendTexId, level, 0);
                if (allSlicesRead) {
                    MGLOG_D("GetTexImage: finished %d slices via per-layer readback", sliceCount);
                    return;
                }
            }
            if (multiSlice &&
                GetTexImageViaShadowConversion(textureMipmapObject,
                                               MG_Util::ConvertGLEnumToTextureUploadTarget(target), level, size.x(),
                                               size.y(), sliceCount, format, type, pixels, applyPackImageParams)) {
                MGLOG_D("GetTexImage: finished via shadow conversion");
                return;
            }
            if (tempFBOComplete && ReadPixelsViaFormatConversion(0, 0, size.x(), size.y(), format, type, pixels,
                                                                 applyPackImageParams,
                                                                 /*applyFixedPointReadClamp=*/false,
                                                                 forceOpaqueAlpha, normalizedCarrier)) {
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
                MGLOG_E_ONCE("GetTexImage: READ FBO incomplete and no shadow copy available, skipping readback");
                return;
            }
            MGLOG_E_ONCE("GetTexImage: format %s with type %s is not implemented yet, skipping readback",
                    MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str());
            return;
        }
        if (!tempFBOComplete) {
            MGLOG_E_ONCE("GetTexImage: bound READ FBO is not complete");
            return;
        }

        // The level is attached to the scratch READ framebuffer above, so the depth and
        // stencil aspects are read with exactly the same helpers glReadPixels uses - native
        // where the driver has it, shader sampling where it does not. ES accepts neither
        // spelling natively, which is why glGetTexImage(GL_DEPTH_STENCIL) used to leave
        // packed_depth_stencil.verify_get_tex_image reading its own zero-filled buffer.
        if (format == GL_DEPTH_COMPONENT && ReadPixelsDepthComponent(0, 0, size.x(), size.y(), type, pixels)) {
            MGLOG_D("GetTexImage: finished via depth readback helper");
            return;
        }
        if (format == GL_DEPTH_STENCIL && ReadPixelsDepthStencilPacked(0, 0, size.x(), size.y(), type, pixels)) {
            MGLOG_D("GetTexImage: finished via packed depth/stencil readback helper");
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
                MGLOG_E_ONCE("GetTexImage: No backend buffer found for PBO %u.",
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
                // Serial bumped with no backend op; re-open the buffer draw-clean memos.
                BufferImpl::BumpBufferMutationEpoch();
                MGLOG_D("ReadPixels: Unmapping PBO");
                g_GLESFuncs.glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            } else {
                MGLOG_E_ONCE("ReadPixels: glMapBufferRange returned nullptr");
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

    // The frontend's default framebuffer is a placeholder FramebufferObject whose attachments
    // carry a format and nothing else (MG_Impl/Init.cpp), and it is built before any surface
    // exists - so it starts on a guess, GL_DEPTH32F_STENCIL8. Every attachment query about the
    // default framebuffer is answered out of that guess, and being wrong is not cosmetic: GL
    // blits depth/stencil only between IDENTICAL formats, so a caller that reads
    // GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, allocates the buffer it was told about and blits
    // gets GL_INVALID_OPERATION and a silently dropped blit - colour bits included, because a
    // rejected glBlitFramebuffer transfers nothing at all. DirectVulkan already publishes its
    // real format when it creates the swapchain (SwapchainObject::Create); this is the
    // DirectGLES half, and here the answer can simply be asked of the ES default framebuffer.
    //
    // Only the format is published. The placeholder's 512x512 extent is left alone: all three
    // attachments share it, and FramebufferObject::CheckCompleteness requires them to agree, so
    // resizing depth/stencil without colour would report the default framebuffer incomplete.
    static void PublishDefaultFramebufferDepthStencilFormat() {
        auto& defaultFBOInfo = MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo;
        if (!defaultFBOInfo || !g_GLESFuncs.glGetFramebufferAttachmentParameteriv) return;

        // GL_DEPTH / GL_STENCIL are the default framebuffer's spellings; a user framebuffer
        // would need GL_DEPTH_ATTACHMENT / GL_STENCIL_ATTACHMENT and answers GL_INVALID_ENUM
        // for these. Nothing else can be bound this early, but bind explicitly so the answer
        // describes the default framebuffer even if this is ever called later.
        GLint previousDrawFramebuffer = 0;
        g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        if (previousDrawFramebuffer != 0) {
            g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        }

        ClearGLErrors();
        GLint depthBits = 0;
        GLint stencilBits = 0;
        GLint depthComponentType = GL_UNSIGNED_NORMALIZED;
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                                          GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_STENCIL,
                                                          GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits);
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE,
                                                          &depthComponentType);
        ClearGLErrors();

        if (previousDrawFramebuffer != 0) {
            g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
        }
        FramebufferImpl::InvalidateFramebufferBindingCache();

        // A driver that refuses the query leaves both at 0. Fall back to what the EGL config
        // was chosen with, which is what the surface actually has.
        if (depthBits == 0 && stencilBits == 0 && g_EGLFuncs.eglGetConfigAttrib && g_Display != EGL_NO_DISPLAY &&
            g_Config != nullptr) {
            EGLint eglDepth = 0;
            EGLint eglStencil = 0;
            if (g_EGLFuncs.eglGetConfigAttrib(g_Display, g_Config, EGL_DEPTH_SIZE, &eglDepth)) {
                depthBits = static_cast<GLint>(eglDepth);
            }
            if (g_EGLFuncs.eglGetConfigAttrib(g_Display, g_Config, EGL_STENCIL_SIZE, &eglStencil)) {
                stencilBits = static_cast<GLint>(eglStencil);
            }
        }

        if (depthBits <= 0 && stencilBits <= 0) {
            // Nothing usable came back from either witness. Returning here leaves whatever a
            // PREVIOUS surface published in place, which would be stale - this function runs
            // once per surface activation, not once per process. It is written this way anyway
            // because the branch is unreachable for a surface MobileGL chose itself:
            // InitDisplayAndContext asks eglChooseConfig for EGL_DEPTH_SIZE 24 and
            // EGL_STENCIL_SIZE 8, and so does its alpha-free retry, so g_Config always has both
            // and the EGL fallback above always answers. A caller that supplies its own
            // depth-less config would keep the previous surface's description; publishing a
            // guess instead would be a different lie, and the placeholder attachment model has
            // no way to say "this buffer does not exist" short of detaching it.
            MGLOG_D("DirectGLES: default framebuffer reports no depth or stencil; leaving the "
                    "placeholder attachment formats untouched");
            return;
        }

        const Bool floatDepth = depthComponentType == GL_FLOAT;
        TextureInternalFormat depthFormat = TextureInternalFormat::Depth24Stencil8;
        TextureInternalFormat stencilFormat = TextureInternalFormat::Depth24Stencil8;
        if (depthBits > 0 && stencilBits > 0) {
            // Packed: both frontend attachments name the same combined format, as DirectVulkan does.
            depthFormat = (floatDepth || depthBits > 24) ? TextureInternalFormat::Depth32FStencil8
                                                         : TextureInternalFormat::Depth24Stencil8;
            stencilFormat = depthFormat;
        } else if (depthBits > 0) {
            depthFormat = floatDepth              ? TextureInternalFormat::DepthComponent32F
                          : (depthBits <= 16)     ? TextureInternalFormat::DepthComponent16
                                                  : TextureInternalFormat::DepthComponent24;
            stencilFormat = depthFormat;
        } else {
            depthFormat = TextureInternalFormat::StencilIndex8;
            stencilFormat = TextureInternalFormat::StencilIndex8;
        }

        auto* depthTexture = defaultFBOInfo->depthAttachment.get();
        auto* stencilTexture = defaultFBOInfo->stencilAttachment.get();
        if (depthTexture) depthTexture->SetInternalFormat(depthFormat);
        if (stencilTexture) stencilTexture->SetInternalFormat(stencilFormat);
        MGLOG_D("DirectGLES: default framebuffer depth=%d stencil=%d float=%d; published attachment "
                "formats depth=%d stencil=%d",
                depthBits, stencilBits, floatDepth ? 1 : 0, static_cast<int>(depthFormat),
                static_cast<int>(stencilFormat));
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
            // X11 drivers can reserve every alpha-8 config for 32-bit ARGB
            // visuals (NVIDIA), so a default-visual (depth 24) window only
            // matches an alpha-0 config; keep those as a second candidate tier
            // for the visual match or eglCreateWindowSurface hits BAD_CONFIG.
            Vector<EGLConfig> alphaFreeConfigs;
            const EGLint alphaFreeAttribs[] = {EGL_SURFACE_TYPE, surfaceBit, EGL_RENDERABLE_TYPE,
                                               EGL_OPENGL_ES3_BIT,
                                               EGL_RED_SIZE,     8,          EGL_GREEN_SIZE,
                                               8,
                                               EGL_BLUE_SIZE,    8,          EGL_DEPTH_SIZE,
                                               24,
                                               EGL_STENCIL_SIZE, 8,          EGL_NONE};
            EGLint numAlphaFree = 0;
            if (g_EGLFuncs.eglChooseConfig(g_Display, alphaFreeAttribs, nullptr, 0, &numAlphaFree) &&
                numAlphaFree > 0) {
                alphaFreeConfigs.resize(static_cast<SizeT>(numAlphaFree));
                if (!g_EGLFuncs.eglChooseConfig(g_Display, alphaFreeAttribs, alphaFreeConfigs.data(),
                                                numAlphaFree, &numAlphaFree)) {
                    numAlphaFree = 0;
                }
                alphaFreeConfigs.resize(static_cast<SizeT>(numAlphaFree));
            }

            const EGLint windowVisualId = QueryX11WindowVisualId(window);
            const EGLint visualIds[] = {windowVisualId, QueryDefaultX11VisualId()};
            for (const auto visualId : visualIds) {
                if (visualId == 0) {
                    continue;
                }
                for (const auto* candidates : {&configs, &alphaFreeConfigs}) {
                    for (const auto config : *candidates) {
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
            MGLOG_D("DirectGLES: applied native swap interval %d (%s)", g_requestedSwapInterval,
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
        PublishDefaultFramebufferDepthStencilFormat();

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

        PublishDefaultFramebufferDepthStencilFormat();

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

    // Defined next to the stamp globals below; every owner-id writer must reset the
    // EGL verification stamp BEFORE publishing the new owner.
    void InvalidateEglVerifiedStamp();

    Bool MakeCurrent() {
        if (!g_EGLFuncs.eglMakeCurrent || g_Display == EGL_NO_DISPLAY || g_Surface == EGL_NO_SURFACE ||
            g_Context == EGL_NO_CONTEXT) {
            MGLOG_E_ONCE("DirectGLES::MakeCurrent failed: EGL display/surface/context is not initialized");
            return false;
        }
        if (!g_EGLFuncs.eglMakeCurrent(g_Display, g_Surface, g_Surface, g_Context)) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E_ONCE("DirectGLES::MakeCurrent failed: native eglMakeCurrent returned error 0x%04x", error);
            return false;
        }
        InvalidateEglVerifiedStamp();
        g_backendContextOwnerThread.store(std::this_thread::get_id(), std::memory_order_release);
        // The ops table may have been unregistered when a previous ES context was
        // destroyed (e.g. a probe context); re-register now that GL is usable.
        BufferImpl::RegisterBufferBackendOps();
        // Conservatively drop the redundant-glUseProgram guard: re-issuing one bind
        // after a MakeCurrent is cheaper than trusting a possibly-reset context.
        PrgramImpl::g_lastUsedBackendProgramId = 0;
        // The GLContext becoming current may be a fresh one whose slot versions
        // restarted at zero; the broadcast memo's key is only monotonic within one.
        PrgramImpl::InvalidateBroadcastMemo();
        BufferImpl::InvalidateIndexedBufferBindingCache();
        BufferImpl::InvalidatePixelBufferBindingCaches();
        FramebufferImpl::InvalidateFramebufferBindingCache();
        PixelStoreImpl::InvalidatePackStateCache();
        // The render-state shadow belongs in this list for the same reason as the ones above:
        // it describes the real ES context, which outlives the MobileGL context that is
        // becoming current. See InvalidateSyncedRenderState.
        RenderStateImpl::InvalidateSyncedRenderState();
        // eglSwapInterval requires a current context; a request made while none was
        // current (and dropped by the driver) is retried here.
        ApplyRequestedSwapInterval();
        return true;
    }

    Bool ReleaseCurrent() {
        if (!g_EGLFuncs.eglMakeCurrent || g_Display == EGL_NO_DISPLAY) {
            InvalidateEglVerifiedStamp();
            g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
            return true;
        }
        if (!g_EGLFuncs.eglMakeCurrent(g_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
            const EGLint error = g_EGLFuncs.eglGetError ? g_EGLFuncs.eglGetError() : EGL_SUCCESS;
            MGLOG_E_ONCE("DirectGLES::ReleaseCurrent failed: native eglMakeCurrent returned error 0x%04x", error);
            return false;
        }
        // Clearing the global owner works from ANY thread (a release request can
        // legally arrive on a thread other than the current owner); erring towards
        // "not current" only defers buffer ops, which is always safe.
        InvalidateEglVerifiedStamp();
        g_backendContextOwnerThread.store(std::thread::id{}, std::memory_order_release);
        return true;
    }

    namespace {
        // EGL ground-truth verification stamp. glvnd's eglGetCurrentContext performs
        // fork detection with a real getpid() syscall on every call, and this
        // predicate sits 2-3 deep in every draw - measured at 16% of the render
        // thread on a live workload. NOT thread_local although the stamp is
        // per-owner-thread state: a shared-library thread_local costs a
        // __tls_get_addr call per access, which itself showed at 1.5% of the draw
        // loop. Plain globals are equivalent because only the thread that passes the
        // owner-identity check below can ever stamp or trust them, only one thread
        // can be the owner at a time, and the ONLY writers of the owner id -
        // MakeCurrent/ReleaseCurrent - reset the stamps before publishing a new
        // owner, so a stamp can never leak across an ownership change. Atomics
        // (relaxed) because the resets may come from a non-owner thread; the
        // owner-id release/acquire pairing orders them.
        std::atomic<Uint64> g_eglVerifiedFrameSerial{~0ull};
        std::atomic<Uint> g_eglVerifiedContextGeneration{0};
    } // namespace

    void InvalidateEglVerifiedStamp() {
        // ~0 frame serial matches no real frame, so the next owner-thread call
        // re-verifies against EGL itself.
        g_eglVerifiedFrameSerial.store(~0ull, std::memory_order_relaxed);
        g_eglVerifiedContextGeneration.store(0, std::memory_order_relaxed);
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
        // cache, synced serials) still advances. Re-verify once per (frame, context
        // generation) rather than per call: an external migration is caught at the
        // next frame boundary instead of the next call, which recovers the
        // bookkeeping just the same, without paying a syscall on every draw.
        const Uint64 frameSerial = g_currentFrameSerial.load(std::memory_order_relaxed);
        if (g_eglVerifiedFrameSerial.load(std::memory_order_relaxed) == frameSerial &&
            g_eglVerifiedContextGeneration.load(std::memory_order_relaxed) == g_syncContextGeneration) {
            return true;
        }
        if (g_EGLFuncs.eglGetCurrentContext && g_EGLFuncs.eglGetCurrentContext() != g_Context) {
            return false;
        }
        g_eglVerifiedFrameSerial.store(frameSerial, std::memory_order_relaxed);
        g_eglVerifiedContextGeneration.store(g_syncContextGeneration, std::memory_order_relaxed);
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

    namespace {
        // The entry point the resolved tier's support ships, or null when there is none.
        MG_External::GLES::glTexBuffer_PTR ResolveTexBufferEntryPoint() {
            using Tier = MG_External::GLESCapabilities::TextureBufferTier;
            switch (g_GLESCapabilities.TextureBufferSupport) {
            case Tier::ExtensionEXT:
                return g_GLESFuncs.glTexBufferEXT ? g_GLESFuncs.glTexBufferEXT : g_GLESFuncs.glTexBuffer;
            case Tier::ExtensionOES:
                return g_GLESFuncs.glTexBufferOES ? g_GLESFuncs.glTexBufferOES : g_GLESFuncs.glTexBuffer;
            case Tier::CoreEs32:
                return g_GLESFuncs.glTexBuffer;
            case Tier::None:
            default:
                return nullptr;
            }
        }

        MG_External::GLES::glTexBufferRange_PTR ResolveTexBufferRangeEntryPoint() {
            using Tier = MG_External::GLESCapabilities::TextureBufferTier;
            switch (g_GLESCapabilities.TextureBufferSupport) {
            case Tier::ExtensionEXT:
                return g_GLESFuncs.glTexBufferRangeEXT ? g_GLESFuncs.glTexBufferRangeEXT
                                                       : g_GLESFuncs.glTexBufferRange;
            case Tier::ExtensionOES:
                return g_GLESFuncs.glTexBufferRangeOES ? g_GLESFuncs.glTexBufferRangeOES
                                                       : g_GLESFuncs.glTexBufferRange;
            case Tier::CoreEs32:
                return g_GLESFuncs.glTexBufferRange;
            case Tier::None:
            default:
                return nullptr;
            }
        }
    } // namespace

    Bool AreBufferTexturesSupported() {
        // Both halves matter. The tier is what the driver ADVERTISES, and it is only meaningful
        // once the capabilities have been filled in; the resolved pointer is what MobileGL can
        // actually call, through the spelling that tier's support ships. Gating on the
        // unsuffixed name alone would call an entry point an EXT/OES driver never exported.
        return g_GLESCapabilities.TextureBufferSupport !=
                   MG_External::GLESCapabilities::TextureBufferTier::None &&
               ResolveTexBufferEntryPoint() != nullptr;
    }

    void CallTexBuffer(GLenum target, GLenum internalFormat, GLuint buffer) {
        MG_External::GLES::glTexBuffer_PTR entryPoint = ResolveTexBufferEntryPoint();
        if (entryPoint == nullptr) {
            return;
        }
        entryPoint(target, internalFormat, buffer);
    }

    Bool CallTexBufferRange(GLenum target, GLenum internalFormat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        MG_External::GLES::glTexBufferRange_PTR entryPoint = ResolveTexBufferRangeEntryPoint();
        if (entryPoint == nullptr) {
            return false;
        }
        entryPoint(target, internalFormat, buffer, offset, size);
        return true;
    }

    const char* GetBufferTextureTierName() {
        using Tier = MG_External::GLESCapabilities::TextureBufferTier;
        switch (g_GLESCapabilities.TextureBufferSupport) {
        case Tier::CoreEs32:
            return "core (ES 3.2)";
        case Tier::ExtensionEXT:
            return "GL_EXT_texture_buffer";
        case Tier::ExtensionOES:
            return "GL_OES_texture_buffer";
        case Tier::None:
        default:
            return "unsupported";
        }
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

    Bool WaitForFrameSerialCompleted(Uint64 serial, Uint64 timeoutNs) {
        if (CompletedFrameSerial() >= serial) return true;
        if (!IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glClientWaitSync) return false;
        // Fences signal in submission order, so the live fence with the SMALLEST
        // serial at or past the target is the earliest event that proves the
        // target frame retired. A recycled slot (GPU more than ring-depth frames
        // behind) leaves no usable fence; report failure and let the caller pick
        // its own fallback rather than draining the whole queue here.
        FrameFence* best = nullptr;
        for (FrameFence& slot : g_frameFenceRing) {
            if (!slot.sync || slot.contextGeneration != g_syncContextGeneration) continue;
            if (slot.serial < serial) continue;
            if (!best || slot.serial < best->serial) best = &slot;
        }
        if (!best) return false;
        const GLenum status =
            g_GLESFuncs.glClientWaitSync(best->sync, GL_SYNC_FLUSH_COMMANDS_BIT, timeoutNs);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) return false;
        Uint64 completed = g_completedFrameSerial.load(std::memory_order_relaxed);
        if (best->serial > completed) {
            g_completedFrameSerial.store(best->serial, std::memory_order_relaxed);
        }
        if (g_GLESFuncs.glDeleteSync) g_GLESFuncs.glDeleteSync(best->sync);
        best->sync = nullptr;
        return true;
    }

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
        MultiDrawImpl::OnBackendContextDestroyed();
        ScratchFBOImpl::OnBackendContextDestroyed();
        ReleasePackedWordScratchTexture();
        FramebufferImpl::InvalidateFramebufferBindingCache();
        VertexArrayImpl::InvalidateVAOBindingCache();
        PixelStoreImpl::InvalidatePackStateCache();
        PrgramImpl::InvalidateBroadcastMemo();
        // Texture ids belong to the dying context; wrappers destroyed later must
        // not glDeleteTextures a recycled name in a successor context.
        ++g_backendContextGeneration;
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
