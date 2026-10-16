// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    MG_External::EGLFunctionsTable g_EGLFuncs;
    MG_External::GLESFunctionsTable g_GLESFuncs;
    MG_External::GLESCapabilities g_GLESCapabilities;
    static SharedPtr<MG_State::GLState::SamplerObject> g_rawDepthFetchSamplerState;
    static SharedPtr<SamplerImpl::BackendSamplerObject> g_rawDepthFetchSamplerBackend;

    // P4a (D-F3, [correction]). THE RAW-DEPTH-FETCH SUBSTITUTION STAYS ON THE SERVER, and it
    // keeps constructing a frontend SamplerObject inside MG_Backend on MONOLITH to do it. That is
    // deliberate and it is not this phase's to change: ARCHITECTURE.md assigns the two
    // backend-specific post-processings of the resolved sampler set - this one and Magma's
    // feedback-loop detection - to the server, acting ON the already-resolved set, and the
    // ROADMAP puts "raw-depth-fetch sampler 原生化" in P3b/P4b beside Magma's placeholder
    // textures. What P4a changes is only what the substitution READS: the resolved view's
    // format answers IsDepthFormatInternalFormat and the sampler CSO record's parameters
    // answer compareMode / minFilter / mipmapMode / magFilter.
    //
    // RECORDED HERE ON PURPOSE: this pair of file-static SharedPtrs is the largest surviving
    // MG_State-type usage anywhere under MG_Backend/DirectGLES, and its owner is P3b/P4b - so
    // the include-graph gate at P13 meets a known item rather than a surprise. The purity
    // gates are unaffected either way: they grep MG_Backend for the pull arm's live-GLContext
    // P5f fs's transport arm below uses only a native sampler with fixed values. The legacy
    // pair is not constructed or consulted by the server. The purity gate's live-context
    // pointer token (G13 - deliberately not spelled here, because that grep is a BARE TOKEN
    // grep and a comment naming it is a hit) and for an MG_State type inside
    // MGPipeResourceOps, and this is neither.
    SamplerImpl::BackendSamplerObject* GetRawDepthFetchSampler() {
        static UniquePtr<SamplerImpl::BackendSamplerObject> native;
        static Uint generation = 0;
        if (generation != g_backendContextGeneration) {
            native.reset(); // old generation's destructor never deletes a successor id
            generation = g_backendContextGeneration;
        }
        if (!native) {
            native = MakeUnique<SamplerImpl::BackendSamplerObject>();
            const GLuint id = native->GetBackendSamplerId();
            g_GLESFuncs.glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g_GLESFuncs.glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g_GLESFuncs.glSamplerParameteri(id, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            g_GLESFuncs.glSamplerParameteri(id, GL_TEXTURE_COMPARE_FUNC, GL_ALWAYS);
        }
        return native.get();
    }

    // P5e (tx2), CONTRACT-P5E §5.3: the substitution's decision, over the fifteen sampler VALUES
    // rather than over the object that happens to hold them. ARCHITECTURE.md §5.5 keeps the
    // substitution itself on the server; this is only where its inputs come from, and the CSO
    // record carries all three (MGPipeValueTypes.h). The SharedPtr overload below is the same
    // three lines with one dereference in front, so the two arms cannot part.
    Bool NeedsRawDepthFetchSampler(const SamplerParameters& samplerParams, TextureInternalFormat textureFormat) {
        if (!MG_Util::IsDepthFormatInternalFormat(textureFormat)) {
            return false;
        }
        if (samplerParams.compareMode != SamplerCompareMode::None) {
            return false;
        }

        return samplerParams.minFilter != SamplerFilterMode::Nearest ||
               samplerParams.mipmapMode != SamplerMipmapMode::None ||
               samplerParams.magFilter != SamplerFilterMode::Nearest;
    }

    Bool NeedsRawDepthFetchSampler(const SharedPtr<MG_State::GLState::SamplerObject>& samplerObject,
                                   TextureInternalFormat textureFormat) {
        if (!samplerObject) {
            return false;
        }
        return NeedsRawDepthFetchSampler(samplerObject->GetAllSamplerParameters(), textureFormat);
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
            for (Int i = 0; i < kMaxDrainedESErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {}
        }

        void ErrorLopper::Clear() {
            for (Int i = 0; i < kMaxDrainedESErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {}
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
} // namespace MobileGL::MG_Backend::DirectGLES
