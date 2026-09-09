// MobileGL - MobileGL/MG_IntegrationTest/Harness/P4aSeamPeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "P4aSeamPeek.h"

#if !defined(__ANDROID__)
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Backend/DirectGLES/DirectGLES.h>
#define MGITEST_P4A_SEAM_PEEK_LIVE 1
#endif
#endif

namespace MGITest {

#if defined(MGITEST_P4A_SEAM_PEEK_LIVE)
    namespace {
        namespace MGP = MobileGL::MG_Pipe;
        namespace MGB = MobileGL::MG_Backend::DirectGLES;

        // "Is Espryt the backend running" - the same test PipeApplyPeek.cpp makes through a twin:
        // on Magma no ES entry point was ever resolved and every member of g_GLESFuncs is null.
        // It is asked BEFORE SamplerSubsystemEnabled(), which is Espryt's own latch and must not
        // be resolved on a process whose backend is not Espryt.
        bool EsprytIsRunning() { return MGB::g_GLESFuncs.glBindSampler != nullptr; }
    } // namespace

    bool PeekEsprytSamplerHandleArmIsLive(bool* outLive) {
        if (outLive == nullptr) return false;
        if (!EsprytIsRunning()) return false;
        *outLive = MGB::SamplerSubsystemEnabled();
        return true;
    }

    bool PeekEsprytFramebufferHandleArmIsLive(bool* outLive) {
        if (outLive == nullptr) return false;
        if (!EsprytIsRunning()) return false;
        *outLive = MGB::FramebufferSubsystemEnabled();
        return true;
    }

    bool PeekPipeShaderImageWindow(PipeShaderImageWindowPeek* out) {
        if (out == nullptr) return false;
        const MGP::MGPipeApplierState& applier = MGP::MGPipeApplier();
        out->Start = static_cast<unsigned>(applier.ShaderImageStart);
        out->Count = static_cast<unsigned>(applier.ShaderImageCount);
        out->Serial = static_cast<unsigned long long>(applier.ShaderImagesSerial);
        return true;
    }

    bool PeekEsprytUnitSampler(unsigned unit, unsigned glSamplerName, EsprytUnitSamplerPeek* out) {
        if (out == nullptr) return false;
        if (!EsprytIsRunning()) return false;
        if (!MobileGL::MG_State::pGLContext) return false;
        const MGP::MGPipeApplierState& applier = MGP::MGPipeApplier();
        if (unit >= applier.BoundSamplerStates.size() || unit >= MGB::SamplerImpl::g_boundSamplersCache.size()) {
            return false;
        }
        *out = EsprytUnitSamplerPeek{};

        // Espryt's own binding shadow: every glBindSampler this backend issues routes through it
        // (BackendSamplerObject::Bind / UnbindSampler), so it IS what the driver holds.
        if (MGB::SamplerImpl::BackendSamplerObject* const bound = MGB::SamplerImpl::g_boundSamplersCache[unit]) {
            out->BoundSamplerId = static_cast<unsigned>(bound->GetBackendSamplerId());
        }

        const MGP::MGPipeHandle cso = applier.BoundSamplerStates[unit];
        out->CsoHandleSlot = static_cast<unsigned>(cso.Slot);
        out->CsoHandleGen = static_cast<unsigned>(cso.Gen);
        out->UnitInsideWindow = unit >= applier.SamplerStateStart &&
                                unit - applier.SamplerStateStart < applier.SamplerStateCount;
        // The twin AT THE CSO HANDLE, asked of the same table Espryt asks (FindByHandle): a null
        // here with a live handle is the F-4 shape - a content-addressed handle looked up in a
        // table that only ever held identity-minted slots.
        if (!MGP::MGPipeHandleIsNull(cso)) {
            if (auto* const slot = MGB::SamplerImpl::g_backendSamplerObjects.FindByHandle(cso); slot && *slot) {
                out->CsoTwinSamplerId = static_cast<unsigned>((*slot)->GetBackendSamplerId());
            }
        }

        // And the twin keyed on the frontend OBJECT, which is what the pre-handle program pass
        // used to mint and bind, so a scenario can say which of the two the driver holds.
        const auto& object = MobileGL::MG_State::pGLContext->GetSamplerObject(
            static_cast<MobileGL::Uint>(glSamplerName));
        if (object) {
            if (auto* const slot = MGB::SamplerImpl::g_backendSamplerObjects.Find(object.get()); slot && *slot) {
                out->IdentityTwinSamplerId = static_cast<unsigned>((*slot)->GetBackendSamplerId());
            }
        }
        return true;
    }
#else
    bool PeekEsprytSamplerHandleArmIsLive(bool*) { return false; }
    bool PeekEsprytFramebufferHandleArmIsLive(bool*) { return false; }
    bool PeekPipeShaderImageWindow(PipeShaderImageWindowPeek*) { return false; }
    bool PeekEsprytUnitSampler(unsigned, unsigned, EsprytUnitSamplerPeek*) { return false; }
#endif

} // namespace MGITest
