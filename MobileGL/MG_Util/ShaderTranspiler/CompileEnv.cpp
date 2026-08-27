// MobileGL - MobileGL/MG_Util/ShaderTranspiler/CompileEnv.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "CompileEnv.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Util::ShaderTranspiler {
    namespace {
        void HashBytes(Uint64& state, const void* data, const SizeT length) {
            state = static_cast<Uint64>(XXH64(data, length, state));
        }

        template <typename T>
        void HashValue(Uint64& state, const T& value) {
            static_assert(std::is_trivially_copyable_v<T>);
            HashBytes(state, &value, sizeof(T));
        }
    } // namespace

    Int ResolveMaxVertexAttribs(const Bool hasBackend, const Int backendMaxVertexAttribs) {
        constexpr Int capacity = static_cast<Int>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
        if (!hasBackend) return capacity;
        if (backendMaxVertexAttribs <= 0) return capacity;
        return std::min(backendMaxVertexAttribs, capacity);
    }

    Uint64 ComputeCompileEnvFingerprint(const CompileEnv& env) {
        Uint64 state = 0x9e3779b97f4a7c15ull;
        HashValue(state, env.maxComputeWorkGroupSize[0]);
        HashValue(state, env.maxComputeWorkGroupSize[1]);
        HashValue(state, env.maxComputeWorkGroupSize[2]);
        HashValue(state, env.maxComputeWorkGroupCount[0]);
        HashValue(state, env.maxComputeWorkGroupCount[1]);
        HashValue(state, env.maxComputeWorkGroupCount[2]);
        HashValue(state, env.maxComputeWorkGroupInvocations);
        HashValue(state, env.backend);
        // DynamicBackendParameters is a plain aggregate of scalars; hashing its object
        // representation is deliberate - it means a new limit cannot be added without also
        // changing the fingerprint, which is exactly the memo-hazard property wanted here.
        HashBytes(state, &env.params, sizeof(env.params));
        if (!env.advertisedExtensions.empty()) {
            HashBytes(state, env.advertisedExtensions.data(),
                      env.advertisedExtensions.size() * sizeof(GLExtension));
        }
        return state;
    }

    Uint64 ComputeFrontendCompileEnvFingerprint(const CompileEnv& env) {
        Uint64 state = 0xff51afd7ed558ccdull;
        // The DynamicBackendParameters limits BuildTBuiltInResource copies into
        // TBuiltInResource. Enumerated ONE BY ONE rather than hashed as a struct,
        // deliberately: hashing all of DynamicBackendParameters would drag ~50 backend-only
        // limits into a key that is supposed to be backend-agnostic, and every one of them
        // would be a false miss. Keep this list in step with BuildTBuiltInResource.
        HashValue(state, env.params.MaxImageUnits);
        HashValue(state, env.params.MaxDrawBuffers);
        HashValue(state, env.params.MaxVertexImageUniforms);
        HashValue(state, env.params.MaxGeometryImageUniforms);
        HashValue(state, env.params.MaxFragmentImageUniforms);
        HashValue(state, env.params.MaxComputeImageUniforms);
        HashValue(state, env.params.MaxCombinedImageUniforms);
        // Added when wave3 (cb155c5b) made this one env-derived. It expands into the
        // gl_MaxComputeTextureImageUnits built-in constant, so a compute module that reads
        // that constant generates DIFFERENT SPIR-V under two backends that disagree on it.
        HashValue(state, env.params.MaxComputeTextureImageUnits);
        // Added when wave4 (4fc3531d) made this one env-derived, and the same class again:
        // glslang REJECTS gl_ClipDistance[i] for i >= maxClipDistances at parse (ParseHelper)
        // and expands gl_MaxClipDistances from the same number, so it decides both whether a
        // shader compiles at all and what a module that reads the constant generates.
        HashValue(state, env.params.MaxClipDistances);
        // The cull-distance pair, added when the GL 4.6 API-surface wave made them env-derived:
        // they were bare literals (8/8) in BuildTBuiltInResource while no backend had ever been
        // asked whether it can host a cull distance. Exactly the MaxClipDistances class - glslang
        // bounds gl_CullDistance[i] against maxCullDistances at parse and expands
        // gl_MaxCullDistances / gl_MaxCombinedClipAndCullDistances from the same numbers.
        HashValue(state, env.params.MaxCullDistances);
        HashValue(state, env.params.MaxCombinedClipAndCullDistances);
        // The texture-image-unit family, made env-derived in the same wave. They were stock
        // glslang defaults (32/32/80) that disagreed with what glGetIntegerv answered, and
        // gl_MaxTextureImageUnits / gl_MaxVertexTextureImageUnits / gl_MaxCombinedTextureImageUnits
        // expand from them.
        HashValue(state, env.params.MaxTextureImageUnits);
        HashValue(state, env.params.MaxVertexTextureImageUnits);
        HashValue(state, env.params.MaxCombinedTextureImageUnits);
        // gl_MaxSamples, which also sizes gl_SampleMask[] / gl_SampleMaskIn[] and bounds a
        // constant index into them, so a module that touches either generates different SPIR-V
        // on two backends that report different sample counts.
        HashValue(state, env.params.MaxSamples);
        // The compute work-group limits, likewise added by wave3 (cb155c5b). They used to be
        // hardcoded maxima in BuildTBuiltInResource, and the L1 key comment said in so many
        // words that the day they became backend-derived they would have to move in here -
        // that day is this merge. glslang expands BOTH of them into built-in constants
        // (Initialize.cpp: "const ivec3 gl_MaxComputeWorkGroupCount = ivec3(%d,%d,%d)" and the
        // same for gl_MaxComputeWorkGroupSize), so this is an INDEPENDENCE break, not merely a
        // reachability one: a compute shader that reads gl_MaxComputeWorkGroupSize compiles to
        // materially different SPIR-V on a driver reporting z=64 than on one reporting z=1024.
        for (Uint index = 0; index < 3; ++index) {
            HashValue(state, env.maxComputeWorkGroupSize[index]);
            HashValue(state, env.maxComputeWorkGroupCount[index]);
        }
        // The two inputs to GetReflectionVertexAttribLimit. Hashed as inputs rather than as
        // the resolved limit so this stays in one translation unit; that is coarser (two
        // envs whose MaxVertexAttribs both exceed the storage capacity resolve to the same
        // limit yet hash differently) but coarser means a false MISS, never a false hit.
        HashValue(state, env.params.MaxVertexAttribs);
        const Uint8 hasBackend = env.HasBackend() ? 1u : 0u;
        HashValue(state, hasBackend);
        return state;
    }

    SharedPtr<const CompileEnv> CaptureCompileEnv() {
        auto env = MakeShared<CompileEnv>();

        const auto& activeBackend = MG_Backend::pActiveBackendObject;
        if (activeBackend) {
            env->backend = activeBackend->GetBackendType();
            env->params = activeBackend->GetDynamicParameters();
            env->advertisedExtensions = activeBackend->GetRendererInfo().RendererGLInfo.Extensions;
        }

        // GL_MAX_COMPUTE_WORK_GROUP_SIZE / _COUNT. These are REAL driver calls on DirectGLES; they
        // must happen here, on the context thread, and exactly once per context. The frontend
        // minimum is the floor, matching what GL_Getter reports - both sides now floor at the
        // shared MIN_COMPUTE_WORK_GROUP_* constants rather than at their own copy of them.
        for (Uint index = 0; index < 3; ++index) {
            Int backendSize = 0;
            Int backendCount = 0;
            if (MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v) {
                MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, index,
                                                                    &backendSize);
                MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, index,
                                                                    &backendCount);
            }
            env->maxComputeWorkGroupSize[index] =
                std::max(static_cast<Uint>(std::max(backendSize, 0)), MIN_COMPUTE_WORK_GROUP_SIZE[index]);
            env->maxComputeWorkGroupCount[index] =
                std::max(static_cast<Uint>(std::max(backendCount, 0)), MIN_COMPUTE_WORK_GROUP_COUNT[index]);
        }

        constexpr Uint64 kFrontendMaxComputeWorkGroupInvocations = 1024;
        env->maxComputeWorkGroupInvocations =
            activeBackend ? std::max(static_cast<Uint64>(std::max(env->params.MaxComputeWorkGroupInvocations, 0)),
                                     kFrontendMaxComputeWorkGroupInvocations)
                          : kFrontendMaxComputeWorkGroupInvocations;

        env->fingerprint = ComputeCompileEnvFingerprint(*env);
        env->frontendFingerprint = ComputeFrontendCompileEnvFingerprint(*env);
        return env;
    }

    const SharedPtr<const CompileEnv>& GetDefaultCompileEnv() {
        // Function-local static, not a namespace-scope one: the fingerprint has to be
        // computed, and this must not run before MG_Config is loaded.
        static const SharedPtr<const CompileEnv> kDefault = [] {
            auto env = MakeShared<CompileEnv>();
            env->fingerprint = ComputeCompileEnvFingerprint(*env);
            env->frontendFingerprint = ComputeFrontendCompileEnvFingerprint(*env);
            return SharedPtr<const CompileEnv>(Move(env));
        }();
        return kDefault;
    }

    const SharedPtr<const CompileEnv>& GetCurrentCompileEnv() {
        if (MG_State::pGLContext) return MG_State::pGLContext->GetCompileEnv();
        return GetDefaultCompileEnv();
    }
} // namespace MobileGL::MG_Util::ShaderTranspiler
