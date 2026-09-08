// MobileGL - MobileGL/MG_IntegrationTest/Harness/PipeApplyPeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PipeApplyPeek.h"

#if !defined(__ANDROID__)
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/MGPipeTypes.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Backend/DirectGLES/DirectGLES.h>
#define MGITEST_PIPE_APPLY_PEEK_LIVE 1
#endif
#endif

namespace MGITest {

#if defined(MGITEST_PIPE_APPLY_PEEK_LIVE)
    namespace {
        namespace MGP = MobileGL::MG_Pipe;
        namespace MGB = MobileGL::MG_Backend::DirectGLES;

        // The frontend texture object a GL name denotes in the CURRENT context, or null. This is
        // a LOOKUP KEY and nothing else: every value this file reports comes from the applier or
        // from Espryt, never from the object found here. (Reading the frontend's own parameter
        // state would answer the question the scenario is asking with the input to it.)
        MobileGL::MG_State::GLState::ITextureObject* FrontendTexture(unsigned glTextureName) {
            if (!MobileGL::MG_State::pGLContext) return nullptr;
            const auto& object = MobileGL::MG_State::pGLContext->GetTextureObject(
                static_cast<MobileGL::Uint>(glTextureName));
            return object ? object.get() : nullptr;
        }

        // Espryt's twin for that texture, or null - which is also this file's "is Espryt even the
        // backend running" answer. On Magma no Espryt twin was ever built, so every entry point
        // below stops here rather than reaching for g_GLESFuncs, whose members are null there.
        MGB::TextureImpl::BackendTextureObject* EsprytTwin(unsigned glTextureName) {
            MobileGL::MG_State::GLState::ITextureObject* const object = FrontendTexture(glTextureName);
            if (object == nullptr) return nullptr;
            auto* const found = MGB::TextureImpl::g_backendTextureObjects.Find(object);
            if (found == nullptr || !*found) return nullptr;
            return found->get();
        }

        int SwizzleToGLEnum(MobileGL::Uint8 encoded) {
            return static_cast<int>(MobileGL::MG_Util::ConvertTextureSwizzleParamToGLEnum(
                static_cast<MobileGL::TextureSwizzleParam>(encoded)));
        }

        // MGPipeTypes.h owns the two numbers and says why depth is 0 (a zeroed record must decode
        // to what an untouched texture already has). This is that decode, and nothing else in
        // this module may open-code it.
        int DepthStencilModeToGLEnum(MobileGL::Uint8 encoded) {
            return encoded == MGP::kMGPipeDepthStencilModeStencil ? GL_STENCIL_INDEX
                                                                  : GL_DEPTH_COMPONENT;
        }

        // The GL_TEXTURE_BINDING_* query for a target, or 0 where this file has no answer. A
        // guess would be worse than a refusal: the binding is what gets RESTORED, so a wrong
        // pname would leave the driver bound to this test's texture.
        int BindingQueryFor(unsigned glTarget) {
            switch (glTarget) {
                case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
                default: return 0;
            }
        }
    } // namespace

    bool PeekPipeTextureParamsRecord(unsigned glTextureName, PipeTextureParamsRecordPeek* out) {
        if (out == nullptr) return false;
        const MGP::MGPipeApplierState& applier = MGP::MGPipeApplier();
        // Slot 0 is the reserved null handle and is never live (MGPipeHandles.h), so the scan
        // starts at 1 and a match at 0 is impossible rather than merely unlikely.
        for (MobileGL::SizeT slot = 1; slot < applier.TextureResources.size(); ++slot) {
            const MGP::MGPipeResourceRecord& record = applier.TextureResources[slot];
            if (!record.Live) continue;
            if (record.Desc.GlNameForDiag != static_cast<MobileGL::Uint32>(glTextureName)) continue;
            out->Slot = static_cast<unsigned>(slot);
            out->Gen = static_cast<unsigned>(record.Gen);
            out->ParamsSerial = static_cast<unsigned long long>(record.ParamsSerial);
            for (int channel = 0; channel < 4; ++channel) {
                out->Swizzle[channel] = SwizzleToGLEnum(record.Params.Swizzle[channel]);
            }
            out->DepthStencilMode = DepthStencilModeToGLEnum(record.Params.DepthStencilMode);
            return true;
        }
        return false;
    }

    bool PeekEsprytAppliedTextureParams(unsigned glTextureName, unsigned glTarget,
                                        EsprytAppliedTextureParamsPeek* out) {
        if (out == nullptr) return false;
        const int bindingQuery = BindingQueryFor(glTarget);
        if (bindingQuery == 0) return false;
        MGB::TextureImpl::BackendTextureObject* const twin = EsprytTwin(glTextureName);
        if (twin == nullptr) return false;
        const MobileGL::Uint backendId = twin->GetBackendTextureId();
        if (backendId == 0) return false;
        if (MGB::g_GLESFuncs.glGetTexParameteriv == nullptr ||
            MGB::g_GLESFuncs.glBindTexture == nullptr || MGB::g_GLESFuncs.glGetIntegerv == nullptr ||
            MGB::g_GLESFuncs.glGetError == nullptr) {
            return false;
        }

        // SAVE / QUERY / RESTORE ON THE UNIT THAT IS ALREADY ACTIVE. No glActiveTexture, so the
        // only driver state this touches is one unit's binding, and it is put back byte for byte
        // - which is what keeps Espryt's own g_boundTexturesCache true rather than merely
        // consistent. (Binding through the twin's own Bind() would update that shadow and would
        // therefore CHANGE what the scenario measures next; this does not.)
        GLint previousBinding = 0;
        MGB::g_GLESFuncs.glGetIntegerv(static_cast<GLenum>(bindingQuery), &previousBinding);
        MGB::g_GLESFuncs.glBindTexture(static_cast<GLenum>(glTarget), backendId);

        out->BackendTextureId = static_cast<unsigned>(backendId);
        static const GLenum kSwizzlePnames[4] = {GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
                                                 GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A};
        for (int channel = 0; channel < 4; ++channel) {
            GLint value = 0;
            MGB::g_GLESFuncs.glGetTexParameteriv(static_cast<GLenum>(glTarget),
                                                 kSwizzlePnames[channel], &value);
            out->Swizzle[channel] = static_cast<int>(value);
        }

        // The depth/stencil aspect mode is ES 3.1 and is INVALID_ENUM on a driver without it, so
        // it is asked for last and its own error decides whether the answer is usable. The queue
        // is drained first because a stale error from anywhere else would be indistinguishable
        // from this call's - Espryt drains it the same way at every one of its own sync sites
        // (DebugImpl::ErrorLopper), and this module's own GL errors are read from the FRONTEND
        // state (ScenarioTest::FirstGLError), which none of this touches.
        while (MGB::g_GLESFuncs.glGetError() != GL_NO_ERROR) {
        }
        GLint mode = 0;
        MGB::g_GLESFuncs.glGetTexParameteriv(static_cast<GLenum>(glTarget),
                                             GL_DEPTH_STENCIL_TEXTURE_MODE, &mode);
        out->DepthStencilModeIsReadable = MGB::g_GLESFuncs.glGetError() == GL_NO_ERROR;
        out->DepthStencilMode = static_cast<int>(mode);

        MGB::g_GLESFuncs.glBindTexture(static_cast<GLenum>(glTarget),
                                       static_cast<GLuint>(previousBinding));
        while (MGB::g_GLESFuncs.glGetError() != GL_NO_ERROR) {
        }
        return true;
    }

    bool PeekEsprytHasSamplerViewForTexture(unsigned glTextureName, bool* outExists) {
        if (outExists == nullptr) return false;
        MobileGL::MG_State::GLState::ITextureObject* const object = FrontendTexture(glTextureName);
        if (object == nullptr) return false;
        // Espryt must be the backend running, or "no view" would be true of every texture on
        // every other backend and the assertion would be vacuous where it is loudest.
        if (EsprytTwin(glTextureName) == nullptr) return false;
        // HandleOfSamplerViewForTexture is the monolith glue that derives the view's handle from
        // the TEXTURE's lifetime id (D-F2: one view per ITextureObject), so this asks Espryt's
        // own table the same way Espryt asks it - it does not consult the applier record's
        // ViewCso, which is the client's statement about the same fact and would make one side
        // of the seam vouch for the other.
        const MGP::MGPipeHandle view = MGB::SamplerViewImpl::HandleOfSamplerViewForTexture(object);
        if (MGP::MGPipeHandleIsNull(view)) {
            *outExists = false;
            return true;
        }
        *outExists = MGB::SamplerViewImpl::FindSamplerViewForHandle(view) != nullptr;
        return true;
    }

    bool PeekPipeApplierRefusedNoConsumer(unsigned long long* outCount) {
        if (outCount == nullptr) return false;
        *outCount = static_cast<unsigned long long>(MGP::MGPipeApplier().RefusedNoConsumer);
        return true;
    }
#else
    bool PeekPipeTextureParamsRecord(unsigned, PipeTextureParamsRecordPeek*) { return false; }
    bool PeekEsprytAppliedTextureParams(unsigned, unsigned, EsprytAppliedTextureParamsPeek*) {
        return false;
    }
    bool PeekEsprytHasSamplerViewForTexture(unsigned, bool*) { return false; }
    bool PeekPipeApplierRefusedNoConsumer(unsigned long long*) { return false; }
#endif

} // namespace MGITest
