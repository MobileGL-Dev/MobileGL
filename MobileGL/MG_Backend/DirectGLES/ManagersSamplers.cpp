// MobileGL - MobileGL/MG_Backend/DirectGLES/ManagersSamplers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ManagersInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    static Bool ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe() {
        // IsAngleLlvmpipeRenderer combined with the
        // MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER feature toggle,
        // both resolved in FillInGLESCapabilities.
        return g_GLESCapabilities.AvoidSamplerMipmapMinFilter;
    }

    namespace SamplerImpl {
        BackendSamplerObject::BackendSamplerObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenSamplers(1, &m_backendSamplerId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendSamplerId == 0) {
                MGLOG_E_ONCE("Failed to generate sampler object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated sampler object with ID: %u.", m_backendSamplerId);
            }
        }

        BackendSamplerObject::~BackendSamplerObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendSamplerId == 0) {
                return;
            }
            // Scrub the unit shadow whether or not the id can still be deleted - the next
            // twin can land on this heap address and would otherwise false-skip its Bind.
            for (auto& boundSampler : g_boundSamplersCache) {
                if (boundSampler == this) {
                    boundSampler = nullptr; // glDeleteSamplers unbinds from every unit
                }
            }
            if (m_contextGeneration == g_backendContextGeneration && g_GLESFuncs.glDeleteSamplers) {
                g_GLESFuncs.glDeleteSamplers(1, &m_backendSamplerId);
            }
            m_backendSamplerId = 0;
        }

        void BackendSamplerObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::SamplerObject>& stateSamplerObject,
            MG_Pipe::MGPipeHandle pushedCso) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateSamplerObject) {
                // A CSO TWIN HAS NO FRONTEND OBJECT (P4a fable seam F-4, ResolveSamplerCsoTwin):
                // the content-addressed handle is its identity and the applier's record its
                // only authority, so a null object beside a live handle is the record arm and
                // not the pre-P4a error. Everything below that names the object is guarded on
                // it; the pull build's text is the three lines the #if brackets.
                if (MG_Pipe::MGPipeHandleIsNull(pushedCso)) {
                MGLOG_E_ONCE("State sampler object is null, cannot sync to backend.");
                return;
                }
            }

            // P4a (D-F1): a SamplerObject is a pure 100-byte value with no driver-side per-object
            // binding state, so its CSO is CONTENT-ADDRESSED on the client at capacity 256 and
            // two identical samplers share one record. What that means here is that the record's
            // server-owned Serial, not the frontend object's version, is what says the values
            // moved - and that the values themselves cross byte for byte INCLUDING
            // borderColorForm, which is why all four border comparands below are still compared.
            //
            // The whole arm choice is a preprocessor #if/#else so the PULL build's text is the
            // pre-P4a text token for token (D-P).
            const SamplerParameters* pushedParams = nullptr;
            // The GL name for the two log lines below, or 0 for a CSO twin, which has no object
            // to name (F-4): the handle in the same line is its name.
            const Uint samplerName = stateSamplerObject ? stateSamplerObject->GetExternalIndex() : 0u;
            // THE HANDLE COMES FROM THE CALLER, NOT FROM THIS TWIN'S REGISTRY, and the
            // difference is the seam D's verification round found on the integrated tree.
            //
            // v2 asked g_backendSamplerObjects.HandleOf(object) - the twin's own identity
            // handle, minted off the frontend lifetime id - and looked the record up by it.
            // That can only ever miss: a SamplerCso is CONTENT-ADDRESSED on the client
            // (D-F1, ID-14/ID-17), its handles come out of MGPipeSlots().Allocate keyed on a
            // parameter hash, and nothing ever emits a create_sampler_state at an identity
            // handle. On the integrated tree every glBindSampler-driven parameter set was
            // therefore refused and the driver sampler kept its defaults - two integration
            // scenarios red, and the twin's own comment below (content-addressed, the
            // record's Serial is the authority) already said why it could not work.
            //
            // The carried fact is MGPipeApplier().BoundSamplerStates[unit], written by the
            // client at bind_sampler_states; the caller that knows the unit passes it as
            // `pushedCso`. See the declaration for why the parameter is push-only.
            if (!MG_Pipe::MGPipeHandleIsNull(pushedCso)) {
                const auto* record = PipeSamplerCsoRecordForHandle(pushedCso);
                if (record == nullptr) {
                    MGLOG_E_ONCE("MGPipe: sampler %u has no applier record on the handle arm, so its "
                                 "parameters cannot be pushed (handle {%u, %u})",
                                 samplerName, pushedCso.Slot, pushedCso.Gen);
                    return;
                }
                if (m_isInitialized && m_syncedSamplerSerial != 0 && m_syncedSamplerSerial == record->Serial) {
                    MGLOG_D("Sampler parameters have not changed for sampler ID: %u, skipping sync.",
                            samplerName);
                    return;
                }
                m_syncedSamplerSerial = record->Serial;
                pushedParams = &record->Params;
            } else {
                // P13: every sampler twin is a CSO twin, synced with its handle
                // (ResolveSamplerCsoTwin); a sampler with no record has nothing to push.
                MGLOG_E_ONCE("MGPipe: sampler %u was synced without a SamplerCso handle", samplerName);
                return;
            }

            MGLOG_D("Syncing sampler with backend ID %u to backend for state ID %u", m_backendSamplerId, samplerName);

            const SamplerParameters& samplerParams = *pushedParams;

#define SYNC_SAMPLER_PARAM_IF_CHANGED(internalName, glName, type)                                                      \
    if (m_cacheSamplerParameters.internalName != samplerParams.internalName) {                                         \
        g_GLESFuncs.glSamplerParameteri(m_backendSamplerId, glName,                                                    \
                                        (GLint)MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName));   \
        m_cacheSamplerParameters.internalName = samplerParams.internalName;                                            \
    }

            if (m_cacheSamplerParameters.minFilter != samplerParams.minFilter ||
                m_cacheSamplerParameters.mipmapMode != samplerParams.mipmapMode) {
                g_GLESFuncs.glSamplerParameteri(m_backendSamplerId, GL_TEXTURE_MIN_FILTER,
                                                (GLint)ResolveBackendMinFilter(
                                                    samplerParams,
                                                    ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe()));
                m_cacheSamplerParameters.minFilter = samplerParams.minFilter;
                m_cacheSamplerParameters.mipmapMode = samplerParams.mipmapMode;
            }
            if (m_cacheSamplerParameters.magFilter != samplerParams.magFilter) {
                g_GLESFuncs.glSamplerParameteri(
                    m_backendSamplerId, GL_TEXTURE_MAG_FILTER,
                    (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.magFilter, SamplerMipmapMode::None));
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
            if (m_cacheSamplerParameters.maxAnisotropy != samplerParams.maxAnisotropy) {
                if (g_GLESCapabilities.SupportsTextureFilterAnisotropy) {
                    g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                                    samplerParams.maxAnisotropy);
                }
                m_cacheSamplerParameters.maxAnisotropy = samplerParams.maxAnisotropy;
            }
            if (m_cacheSamplerParameters.borderColor != samplerParams.borderColor ||
                m_cacheSamplerParameters.borderColorI != samplerParams.borderColorI ||
                m_cacheSamplerParameters.borderColorUI != samplerParams.borderColorUI ||
                m_cacheSamplerParameters.borderColorForm != samplerParams.borderColorForm) {
                // Same gate as the texture-side border colour above, and the same reason for
                // branching on the form: an integer border colour must reach the driver through
                // glSamplerParameterIiv/Iuiv or an integer sampler reads the float's bit pattern
                // back instead of the value.
                if (g_GLESCapabilities.SupportsTextureBorderClamp) {
                    if (samplerParams.borderColorForm == BorderColorForm::Int &&
                        g_GLESFuncs.glSamplerParameterIiv) {
                        const GLint borderColorArray[4] = {
                            samplerParams.borderColorI.x(), samplerParams.borderColorI.y(),
                            samplerParams.borderColorI.z(), samplerParams.borderColorI.w()};
                        g_GLESFuncs.glSamplerParameterIiv(m_backendSamplerId, GL_TEXTURE_BORDER_COLOR,
                                                          borderColorArray);
                    } else if (samplerParams.borderColorForm == BorderColorForm::Uint &&
                               g_GLESFuncs.glSamplerParameterIuiv) {
                        const GLuint borderColorArray[4] = {
                            samplerParams.borderColorUI.x(), samplerParams.borderColorUI.y(),
                            samplerParams.borderColorUI.z(), samplerParams.borderColorUI.w()};
                        g_GLESFuncs.glSamplerParameterIuiv(m_backendSamplerId, GL_TEXTURE_BORDER_COLOR,
                                                           borderColorArray);
                    } else if (g_GLESFuncs.glSamplerParameterfv) {
                        const GLfloat borderColorArray[4] = {
                            samplerParams.borderColor.x(), samplerParams.borderColor.y(),
                            samplerParams.borderColor.z(), samplerParams.borderColor.w()};
                        g_GLESFuncs.glSamplerParameterfv(m_backendSamplerId, GL_TEXTURE_BORDER_COLOR,
                                                         borderColorArray);
                    }
                }
                m_cacheSamplerParameters.borderColor = samplerParams.borderColor;
                m_cacheSamplerParameters.borderColorI = samplerParams.borderColorI;
                m_cacheSamplerParameters.borderColorUI = samplerParams.borderColorUI;
                m_cacheSamplerParameters.borderColorForm = samplerParams.borderColorForm;
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

        Uint BackendSamplerObject::GetBackendSamplerId() const {
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
        TwinRegistry<MG_State::GLState::SamplerObject, BackendSamplerObject, MG_Pipe::MGPipeKind::SamplerCso> g_backendSamplerObjects;

        // P4a fable seam F-4. THE TWIN FOR A CONTENT-ADDRESSED SamplerCso HANDLE, keyed by that
        // handle and synced from its record - see the declaration for why the identity-keyed
        // lookup it replaces could never hit.
        BackendSamplerObject* ResolveSamplerCsoTwin(MG_Pipe::MGPipeHandle cso) {
            if (MG_Pipe::MGPipeHandleIsNull(cso)) return nullptr;
            // THE RECORD FIRST, before the table is touched: a handle with no record is a seam
            // defect (the client minted and named a CSO it never described, or evicted one a
            // standing set still names), and adopting a slot for it would leave a twin that
            // syncs nothing. The census stem is the sampler family's.
            const auto* record = PipeSamplerCsoRecordForHandle(cso);
            if (record == nullptr) {
                MGLOG_E_ONCE("MGPipe: sampler CSO {%u, %u} has no applier record on the handle arm, so no "
                             "driver sampler can be built for it and the unit keeps what it holds",
                             cso.Slot, cso.Gen);
                return nullptr;
            }
            // The same slot table the identity twins live in: one allocator serves both handle
            // families of this kind, so a content-addressed slot and an identity-minted slot can
            // never coincide, and the generation discipline (forward = recycle, backward =
            // refused) is what retires a twin whose CSO the client's LRU evicted and re-minted.
            auto* slot = g_backendSamplerObjects.GetOrCreateByHandle(cso);
            if (slot == nullptr) {
                MGLOG_E_ONCE("MGPipe: sampler CSO {%u, %u} cannot be adopted on the handle arm (the slot's "
                             "live generation is %u), so no driver sampler is built for it",
                             cso.Slot, cso.Gen, g_backendSamplerObjects.LiveGenAt(cso.Slot));
                return nullptr;
            }
            if (!*slot) *slot = MakeShared<BackendSamplerObject>();
            // Serial-gated inside: a CSO whose record did not move since this twin last synced
            // costs the record lookup above and one compare.
            (*slot)->SyncToBackend(nullptr, cso);
            return slot->get();
        }
    } // namespace SamplerImpl
} // namespace MobileGL::MG_Backend::DirectGLES
