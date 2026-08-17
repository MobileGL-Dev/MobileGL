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

    Uint64 ComputeCompileEnvFingerprint(const CompileEnv& env) {
        Uint64 state = 0x9e3779b97f4a7c15ull;
        HashValue(state, env.maxComputeWorkGroupSize[0]);
        HashValue(state, env.maxComputeWorkGroupSize[1]);
        HashValue(state, env.maxComputeWorkGroupSize[2]);
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

    SharedPtr<const CompileEnv> CaptureCompileEnv() {
        auto env = MakeShared<CompileEnv>();

        const auto& activeBackend = MG_Backend::pActiveBackendObject;
        if (activeBackend) {
            env->backend = activeBackend->GetBackendType();
            env->params = activeBackend->GetDynamicParameters();
            env->advertisedExtensions = activeBackend->GetRendererInfo().RendererGLInfo.Extensions;
        }

        // GL_MAX_COMPUTE_WORK_GROUP_SIZE. This is a REAL driver call on DirectGLES; it must
        // happen here, on the context thread, and exactly once per context. The frontend
        // minimum is the floor, matching what GL_Getter reports.
        // TODO: Share these exposed compute limit helpers with GL_Getter.cpp instead of duplicating the frontend minima.
        constexpr Uint kFrontendMinComputeWorkGroupSizes[3] = {1024, 1024, 64};
        for (Uint index = 0; index < 3; ++index) {
            Int backendValue = 0;
            if (MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v) {
                MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, index,
                                                                    &backendValue);
            }
            env->maxComputeWorkGroupSize[index] =
                std::max(static_cast<Uint>(std::max(backendValue, 0)), kFrontendMinComputeWorkGroupSizes[index]);
        }

        constexpr Uint64 kFrontendMaxComputeWorkGroupInvocations = 1024;
        env->maxComputeWorkGroupInvocations =
            activeBackend ? std::max(static_cast<Uint64>(std::max(env->params.MaxComputeWorkGroupInvocations, 0)),
                                     kFrontendMaxComputeWorkGroupInvocations)
                          : kFrontendMaxComputeWorkGroupInvocations;

        env->fingerprint = ComputeCompileEnvFingerprint(*env);
        return env;
    }

    const SharedPtr<const CompileEnv>& GetDefaultCompileEnv() {
        // Function-local static, not a namespace-scope one: the fingerprint has to be
        // computed, and this must not run before MG_Config is loaded.
        static const SharedPtr<const CompileEnv> kDefault = [] {
            auto env = MakeShared<CompileEnv>();
            env->fingerprint = ComputeCompileEnvFingerprint(*env);
            return SharedPtr<const CompileEnv>(Move(env));
        }();
        return kDefault;
    }

    const SharedPtr<const CompileEnv>& GetCurrentCompileEnv() {
        if (MG_State::pGLContext) return MG_State::pGLContext->GetCompileEnv();
        return GetDefaultCompileEnv();
    }
} // namespace MobileGL::MG_Util::ShaderTranspiler
