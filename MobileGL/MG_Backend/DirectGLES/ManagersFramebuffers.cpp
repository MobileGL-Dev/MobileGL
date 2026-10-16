// MobileGL - MobileGL/MG_Backend/DirectGLES/ManagersFramebuffers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ManagersInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    const MG_Pipe::MGPipeResourceRecord* PipeRenderbufferRecordForHandle(MG_Pipe::MGPipeHandle res) {
        return PipeRecordAt(MG_Pipe::MGPipeApplier().RenderbufferResources, res);
    }

    namespace FramebufferImpl {
        BackendFramebufferObject::BackendFramebufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // Identity until a non-identity draw-buffer array forces a relocation. A framebuffer
            // that is never draw-bound never runs the recompute, so the table has to start out
            // matching what the attachment loop will physically do.
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                m_backendColorSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            g_GLESFuncs.glGenFramebuffers(1, &m_backendFBOId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendFBOId == 0) {
                MGLOG_E_ONCE("Failed to generate framebuffer object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated framebuffer object with ID: %u.", m_backendFBOId);
            }
        }

        BackendFramebufferObject::~BackendFramebufferObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendFBOId == 0) {
                return;
            }
            // Scrub the binding shadow whether or not the id can still be deleted: a
            // recycled name must never satisfy the shadow's dedup.
            NoteFramebufferIdDeleted(m_backendFBOId);
            if (m_contextGeneration == g_backendContextGeneration && g_GLESFuncs.glDeleteFramebuffers) {
                g_GLESFuncs.glDeleteFramebuffers(1, &m_backendFBOId);
            }
            m_backendFBOId = 0;
        }

        void BackendFramebufferObject::Bind(FramebufferTarget target) const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (target == FramebufferTarget::Read)
                BindFramebufferId(GL_READ_FRAMEBUFFER, m_backendFBOId);
            else
                BindFramebufferId(GL_DRAW_FRAMEBUFFER, m_backendFBOId);
        }

        namespace {
            // Driver-level framebuffer-binding shadow (see Managers.h). Indexed by
            // FramebufferTarget {Draw, Read}.
            Array<Uint, SizeT(FramebufferTarget::FramebufferTargetCount)> g_driverFBOBindings = {0, 0};
            Array<Bool, SizeT(FramebufferTarget::FramebufferTargetCount)> g_driverFBOBindingKnown = {false, false};
        } // namespace

        void BindFramebufferId(GLenum fbTarget, Uint id) {
            const Bool bindsDraw = fbTarget == GL_DRAW_FRAMEBUFFER || fbTarget == GL_FRAMEBUFFER;
            const Bool bindsRead = fbTarget == GL_READ_FRAMEBUFFER || fbTarget == GL_FRAMEBUFFER;
            const SizeT drawIdx = SizeT(FramebufferTarget::Draw);
            const SizeT readIdx = SizeT(FramebufferTarget::Read);
            const Bool drawMatches =
                !bindsDraw || (g_driverFBOBindingKnown[drawIdx] && g_driverFBOBindings[drawIdx] == id);
            const Bool readMatches =
                !bindsRead || (g_driverFBOBindingKnown[readIdx] && g_driverFBOBindings[readIdx] == id);
            if (drawMatches && readMatches) {
                return;
            }
            g_GLESFuncs.glBindFramebuffer(fbTarget, id);
            if (bindsDraw) {
                g_driverFBOBindings[drawIdx] = id;
                g_driverFBOBindingKnown[drawIdx] = true;
            }
            if (bindsRead) {
                g_driverFBOBindings[readIdx] = id;
                g_driverFBOBindingKnown[readIdx] = true;
            }
        }

        Uint CurrentFramebufferBinding(FramebufferTarget target) {
            const SizeT idx = SizeT(target);
            if (!g_driverFBOBindingKnown[idx]) {
                // Cold path: pin the shadow from the driver once (init probes and
                // pre-shadow code bind raw but restore what they found).
                GLint binding = 0;
                g_GLESFuncs.glGetIntegerv(target == FramebufferTarget::Read ? GL_READ_FRAMEBUFFER_BINDING
                                                                            : GL_DRAW_FRAMEBUFFER_BINDING,
                                          &binding);
                g_driverFBOBindings[idx] = static_cast<Uint>(binding);
                g_driverFBOBindingKnown[idx] = true;
            }
            return g_driverFBOBindings[idx];
        }

        void NoteFramebufferIdDeleted(Uint id) {
            if (id == 0) {
                return;
            }
            for (SizeT idx = 0; idx < g_driverFBOBindings.size(); ++idx) {
                if (g_driverFBOBindingKnown[idx] && g_driverFBOBindings[idx] == id) {
                    g_driverFBOBindings[idx] = 0; // glDeleteFramebuffers reverts a bound FBO to 0
                }
            }
        }

        void InvalidateFramebufferBindingCache() {
            g_driverFBOBindings = {0, 0};
            g_driverFBOBindingKnown = {false, false};
            // The per-target "already synced" memo describes work pushed into the ES context
            // that is being left or replaced. Both callers (MakeCurrent, DestroyEGLContext)
            // mean the context may have been reset under us, so claim nothing is synced:
            // a null object never matches a real binding, so SyncCurrentFBO re-pushes.
            g_fboSyncedSlotVersions = {0};
            g_fboSyncedObjectVersions = {0};
            g_fboSyncedObjects = {};
#if MOBILEGL_ESPRYT_FBO_HANDLE_ARM_MEMOS_LINKED
            // E's MAJOR-4 / A9: the handle arm's own memos say the same thing about the same
            // targets, so they are invalidated HERE rather than at each of the nine call sites -
            // six in DirectGLES.cpp and three in SanityTest.cpp, and it was the three that were
            // missed. See the declaration in Managers.h for why the call is gated and which line
            // each package flips.
            InvalidateFramebufferHandleArmMemos();
#endif
        }

        void BackendFramebufferObject::InvalidateSyncedState() {
            std::fill(std::begin(m_frontendDrawBuffers), std::end(m_frontendDrawBuffers),
                      FramebufferAttachmentType::Unknown);
            std::fill(std::begin(m_backendDrawBuffers), std::end(m_backendDrawBuffers), GL_NONE);
            // NOTE: this does NOT empty the backend ES framebuffer - m_backendFBOId keeps every
            // attachment it had, possibly under a non-identity permutation. Declaring the table
            // identity here is safe only because every gate that decides which points get
            // re-issued is invalidated too - the attachment versions below, and under
            // MOBILEGL_PIPE_PUSH the record memos stamped at the end of this function - so the
            // next sync re-attaches all non-empty attachments at their identity points AND
            // (see SyncToBackend's attachment loop) detaches any colour point whose frontend owner
            // is empty. Without that detach a stale image would survive under a point the table now
            // claims for a different, empty attachment.
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                m_backendColorSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            m_frontendReadBuffer = FramebufferAttachmentType::Unknown;
            m_backendReadBuffer = GL_NONE;
            std::fill(m_syncedFrontendAttachmentVersions.begin(), m_syncedFrontendAttachmentVersions.end(),
                      static_cast<Uint16>(~0u));
            // Every attachment version is invalidated above, so the next walk re-attaches
            // everything regardless; stamp the generation so it does not re-arm twice.
            m_syncedBackendIdGeneration = g_attachmentBackendIdGeneration;
            // The record memos are the gate the PIPE_PUSH arms' attachment walks actually read -
            // the handle arm has nothing narrower under its key - so leaving them standing here
            // would let a re-sync of the same record skip the re-attach this function exists to
            // force. 0 is never a live hash, so a zeroed memo is a guaranteed miss.
            m_syncedRecordHashes = {0};
            m_syncedAttachmentRecordHash = 0;
            m_syncedNoAttachmentDefaultsValid = false;
        }

        static Bool IsSnormFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return true;
            default:
                return false;
            }
        }

        static Bool IsUnormFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R16:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGBA16:
                return true;
            default:
                return false;
            }
        }

        Uint32 g_alphaWidenedDrawBufferMask = 0;
        Uint32 g_integerColorDrawBufferMask = 0;

        static Bool IsIntegerColorFormat(TextureInternalFormat format) {
            switch (format) {
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
            case TextureInternalFormat::RGB10A2UI:
                return true;
            default:
                return false;
            }
        }

        // THE FOUR CROSS-OBJECT MASKS, ANSWERED FROM THE RECORD (ID-12 DV-5, review M-3 / DV-5).
        //
        // All four reduce to (internal format, TEXTURE TARGET) for a texture attachment and to
        // (internal format) for a renderbuffer one, and MGPSurface now carries both inline -
        // InternalFormat since D-C1 and TextureTarget since c0c widened Pad0 into it. So the
        // handle arm stops reading the frontend attachment objects to compute them, which is
        // what D-C1's "the four cross-object masks fall out at push time with no lookup" meant.
        //
        // ShouldUseCaveatTextureFormat / BackendTextureFormatAddsAlpha / their renderbuffer
        // siblings are UNTOUCHED - two of them are on D-N's byte-identical list. Only what they
        // are ASKED changes, which is the whole point of carrying the target rather than
        // inventing a TextureUploadTarget -> TextureTarget inverse: feeding a D-N-pinned
        // function a guessed input would silently change what every texture is allocated as.
        //
        // Kind IS the gate (c0c: "consulted only when Kind == kMGPipeSurfaceKindTexture"), and
        // kMGPipeSurfaceNoTextureTarget on a texture point is a seam defect - refused loudly and
        // answered false, never guessed. False is the safe direction for all four: the mask
        // turns an emulation ON, and an emulation that does not run leaves the driver's own
        // (correct-for-the-real-format) behaviour, while one that runs on the wrong buffer
        // clamps or overwrites texels the application wrote.
        static Bool PushedSurfaceTextureTarget(const MG_Pipe::MGPSurface& surface, TextureTarget* out) {
            if (surface.TextureTarget == MG_Pipe::kMGPipeSurfaceNoTextureTarget) {
                MGLOG_E_ONCE("MGPipe: attachment surface {%u, %u} says Kind=texture but carries no "
                             "texture target - refusing to guess one for the cross-object masks",
                             surface.Res.Slot, surface.Res.Gen);
                return false;
            }
            *out = static_cast<TextureTarget>(surface.TextureTarget);
            return true;
        }

        static Bool IsSnormFallbackSurface(const MG_Pipe::MGPSurface& surface) {
            const auto format = static_cast<TextureInternalFormat>(surface.InternalFormat);
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindTexture) {
                TextureTarget target = TextureTarget::Unknown;
                return PushedSurfaceTextureTarget(surface, &target) && IsSnormFormat(format) &&
                       TextureImpl::ShouldUseCaveatTextureFormat(format, target);
            }
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindRenderbuffer) {
                return IsSnormFormat(format) && TextureImpl::ShouldUseCaveatRenderbufferFormat(format);
            }
            return false;
        }

        static Bool IsUnormFallbackSurface(const MG_Pipe::MGPSurface& surface) {
            const auto format = static_cast<TextureInternalFormat>(surface.InternalFormat);
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindTexture) {
                TextureTarget target = TextureTarget::Unknown;
                return PushedSurfaceTextureTarget(surface, &target) && IsUnormFormat(format) &&
                       TextureImpl::ShouldUseCaveatTextureFormat(format, target);
            }
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindRenderbuffer) {
                return IsUnormFormat(format) && TextureImpl::ShouldUseCaveatRenderbufferFormat(format);
            }
            return false;
        }

        static Bool IsAlphaWidenedColorSurface(const MG_Pipe::MGPSurface& surface) {
            const auto format = static_cast<TextureInternalFormat>(surface.InternalFormat);
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindTexture) {
                TextureTarget target = TextureTarget::Unknown;
                return PushedSurfaceTextureTarget(surface, &target) &&
                       TextureImpl::BackendTextureFormatAddsAlpha(format, target);
            }
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindRenderbuffer) {
                return TextureImpl::BackendRenderbufferFormatAddsAlpha(format);
            }
            return false;
        }

        static Bool IsIntegerColorSurface(const MG_Pipe::MGPSurface& surface) {
            // No target and no caveat table: integerness is a property of the format alone, so
            // this one is the same question on both arms.
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindNone) {
                return false;
            }
            return IsIntegerColorFormat(static_cast<TextureInternalFormat>(surface.InternalFormat));
        }

        // The record's surface for an attachment POINT, or null when this record does not
        // describe that point at all. MGPFramebufferState carries Color[8] + Depth + Stencil
        // (D-C1), which is every point a framebuffer can hold on the handle arm: D-C3 refuses
        // bit 9 outright on a driver reporting more than 8 colour attachments, and the
        // FRONT/BACK points belong to the DEFAULT framebuffer, which has no twin at all.
        const MG_Pipe::MGPSurface* PushedSurfaceForAttachment(const MG_Pipe::MGPFramebufferState& record,
                                                              FramebufferAttachmentType point) {
            if (point == FramebufferAttachmentType::Depth) return &record.Depth;
            if (point == FramebufferAttachmentType::Stencil) return &record.Stencil;
            if (point < FramebufferAttachmentType::Color0 || point > FramebufferAttachmentType::Color31) {
                return nullptr;
            }
            const Int index = static_cast<Int>(point) - static_cast<Int>(FramebufferAttachmentType::Color0);
            if (index >= static_cast<Int>(MG_Pipe::kMGPipeMaxColorAttachments)) return nullptr;
            return &record.Color[static_cast<SizeT>(index)];
        }

        // SyncAttachmentObject's handle arm (review M-3). WHAT MOVES IS THE RESOLUTION: the twin
        // is adopted from MGPSurface::Res through the slot table instead of being looked up by
        // the frontend object's ADDRESS, and the attach SHAPE - layered, level, layer, the
        // upload target and the texture target - is read off the surface instead of off the
        // frontend attachment object.
        //
        // P5e (fb, CONTRACT-P5E.md §5.4): THE FRONTEND ATTACHMENT PARAMETER IS GONE. It was
        // here for two things and P5e retires both. The storage syncs now have by-handle forms
        // (SyncMipmapsToBackendByHandle / SyncToBackendByHandle), so the object is no longer
        // needed to reach the texels; and the record is now the ONLY statement of what is
        // attached, so the cross-checks had nothing left to corroborate against - a second
        // answer that only a monolith build could produce is not a check, it is a second
        // writer. With the object gone, review N-6's hole ("record empty, frontend still
        // holding") is UNREPRESENTABLE rather than refused: the caller's detach and this
        // function's attach now read the SAME test, `Kind == None || Res null`.
        //
        // The two P5c cross-checks that ran HandleOf on the apply thread go with it. The
        // renderbuffer one (the one that was NOT transport-gated, unlike its texture sibling)
        // is deleted by the rekey rather than gated, which is what CONTRACT-P5E §4.4 asks for:
        // gating it would have left a client-allocator probe compiled into a path a run-ahead
        // apply reaches, and there is no longer anything for it to say.
        //
        // P5e (fix1, ID-81 / §5.8): THE STORAGE SYNC IS ARM-SELECTED AND THE MONOLITH ARM KEEPS
        // ITS FRONTEND OBJECT. This function has TWO callers and they are on opposite arms: the
        // handle form (SyncToBackendByHandle) has no frontend object and must not acquire one,
        // while the OBJECT form (SyncToBackend, the push-monolith arm - P4a drives its
        // attachments from the record too, gated on FramebufferSubsystemEnabled() alone, D-C2)
        // is holding the frontend attachment the whole time. fb dropped the parameter for both,
        // which routed the monolith arm into tx2's by-handle storage seam; that seam passes a
        // NULL SharedPtr and relies on the record arm being selected, and under
        // Transport == Monolith it is not - `stateTextureObject->IsTextureView()` then
        // dereferenced null (the device's Lightmap.<init> -> clearColorTexture -> glClear
        // SIGSEGV). So the parameter comes back as MONOLITH GLUE, used for nothing but the
        // storage sync: the attach SHAPE, the empty-point test and the twin adoption stay the
        // record's on both arms, which is what keeps N-6's hole closed and the cross-checks
        // deleted. It is a pointer and it is null on the handle arm, so "which arm am I on" is
        // never inferred from it - MG_Config::Transport decides, as §5.8 requires, and a
        // monolith arm that somehow arrives without one refuses by name instead of attaching a
        // texture whose levels were never pushed.
        static Bool SyncAttachmentSurface(GLenum glFBOTarget, const MG_Pipe::MGPSurface& surface,
                                          GLenum glBackendAttachment,
                                          const MG_State::GLState::FramebufferAttachmentObject* monolithAttachment) {
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindNone || MG_Pipe::MGPipeHandleIsNull(surface.Res)) {
                // An empty point: the caller detached it on exactly this test, so attaching
                // nothing here is the whole of the agreement. (The legacy arm does the same.)
                return true;
            }
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindTexture) {
                // The record's handle is validated BEFORE the frontend is consulted (N-3): it is
                // the untrusted half, the frontend cross-check below is the corroboration, and
                // asking in this order is what gives the two adoption refusals a reachable
                // caller at all.
                if (!PipeTwinHandleIsAdoptable(TextureImpl::g_backendTextureObjects, surface.Res, "texture")) {
                    return false;
                }
                auto* twinSlot = AdoptTwinByHandle(TextureImpl::g_backendTextureObjects, surface.Res, "texture");
                if (twinSlot == nullptr) {
                    return false; // AdoptTwinByHandle named the refusal
                }
                if (!*twinSlot) {
                    *twinSlot = MakeShared<TextureImpl::BackendTextureObject>();
                }
                // COPIED OUT of the table: SyncMipmapsToBackend can grow it (a re-mint adopts
                // another handle), and Managers.h:381-385's pointer-invalidation warning applies
                // to exactly this sequence.
                SharedPtr<TextureImpl::BackendTextureObject> backendTextureObject = *twinSlot;
                if (!backendTextureObject) {
                    MGLOG_E_ONCE("%s: No backend texture found for FBO attachment, cannot bind texture.", __func__);
                    return false;
                }
                // P5e (fb): the storage sync BY HANDLE (tx2's seam). The level shadow it reads
                // is the applier's staged texture store, not a frontend object, which is what
                // let the object parameter go - UNDER A TRANSPORT. P5e (fix1, ID-81): the
                // push-monolith arm keeps the frontend sync it had at f6cfcbd3, token for token,
                // because tx2's record arm inside SyncMipmapsToBackend is itself selected by
                // `Transport != Monolith` and cannot answer here.
                backendTextureObject->SyncMipmapsToBackendByHandle(surface.Res);
                // From here on every draw, clear and blit into this framebuffer can write the
                // texture behind the staged shadow's back (the frontend arm's note, by handle).
                TextureImpl::NoteDriverSideTextureWriteByHandle(surface.Res);
                const auto uploadTarget = static_cast<TextureUploadTarget>(surface.UploadTarget);
                if (surface.Layered != 0) {
                    g_GLESFuncs.glFramebufferTexture(glFBOTarget, glBackendAttachment,
                                                     backendTextureObject->GetBackendTextureId(),
                                                     static_cast<GLint>(surface.Level));
                } else if (uploadTarget == TextureUploadTarget::Texture3D ||
                           uploadTarget == TextureUploadTarget::Texture2DArray ||
                           uploadTarget == TextureUploadTarget::Texture1DArray ||
                           uploadTarget == TextureUploadTarget::CubeMapArray ||
                           uploadTarget == TextureUploadTarget::Texture2DMultisampleArray) {
                    g_GLESFuncs.glFramebufferTextureLayer(
                        glFBOTarget, glBackendAttachment, backendTextureObject->GetBackendTextureId(),
                        static_cast<GLint>(surface.Level), static_cast<GLint>(surface.Layer));
                } else {
                    auto glTextureTarget = TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                    if (glTextureTarget == GL_UNKNOWN_MGL) {
                        TextureTarget target = TextureTarget::Unknown;
                        if (!PushedSurfaceTextureTarget(surface, &target)) {
                            return false;
                        }
                        glTextureTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
                    }
                    // Same cube-face rule as the legacy arm: glBindTexture rejects the face
                    // enums, so bind through the owning cube target and attach with the face.
                    const Bool isCubeFace = glTextureTarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
                                            glTextureTarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
                    backendTextureObject->Bind(isCubeFace ? GL_TEXTURE_CUBE_MAP : glTextureTarget);
                    g_GLESFuncs.glFramebufferTexture2D(glFBOTarget, glBackendAttachment, glTextureTarget,
                                                       backendTextureObject->GetBackendTextureId(),
                                                       static_cast<GLint>(surface.Level));
                }
                return true;
            }
            if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindRenderbuffer) {
                // Same order as the texture arm above, for the same reason (N-3).
                if (!PipeTwinHandleIsAdoptable(RenderbufferImpl::g_backendRenderbufferObjects, surface.Res,
                                               "renderbuffer")) {
                    return false;
                }
                auto* twinSlot =
                    AdoptTwinByHandle(RenderbufferImpl::g_backendRenderbufferObjects, surface.Res, "renderbuffer");
                if (twinSlot == nullptr) {
                    return false;
                }
                if (!*twinSlot) {
                    *twinSlot = MakeShared<RenderbufferImpl::BackendRenderbufferObject>();
                }
                SharedPtr<RenderbufferImpl::BackendRenderbufferObject> backendRenderbufferObject = *twinSlot;
                if (!backendRenderbufferObject) {
                    MGLOG_E_ONCE("%s: No backend renderbuffer found for FBO attachment.", __func__);
                    return false;
                }
                // P5e (fb): the four allocation values were already record-supplied (D-D2); what
                // moves here is the LOOKUP - the handle the surface carries, never HandleOf on
                // an object the apply thread was handed.
                //
                // P5e (fix1, ID-81): arm-selected for the texture arm's reason, and here it is a
                // BEHAVIOUR difference even though the by-handle body dereferences nothing: the
                // object form reports a refused allocation to the application through the live
                // GLContext (RecordError) and the by-handle form deliberately does not, because
                // under a transport there is no application on this side to report it to. On the
                // push-monolith arm there is, so the monolith arm keeps the object form.
                backendRenderbufferObject->SyncToBackendByHandle(surface.Res);
                backendRenderbufferObject->Bind();
                g_GLESFuncs.glFramebufferRenderbuffer(glFBOTarget, glBackendAttachment, GL_RENDERBUFFER,
                                                      backendRenderbufferObject->GetBackendRenderbufferId());
                return true;
            }
            MGLOG_E_ONCE("MGPipe: attachment surface {%u, %u} carries Kind=%u, which is neither a texture "
                         "nor a renderbuffer nor an empty point - refusing to attach",
                         surface.Res.Slot, surface.Res.Gen, surface.Kind);
            return false;
        }

        // The read attachment's storage carries an alpha channel its frontend format does not
        // (the three-channel colour-renderable widening). GL answers such a read with 1.0, but
        // the storage holds whatever the draw wrote there, so the readback has to overwrite it.
        Bool IsAlphaWidenedFallbackReadAttachment() {
            const auto* record = MG_Pipe::MGPipeApplier().ReadFramebuffer();
            return record && IsAlphaWidenedColorSurface(record->ReadSurface);
        }

        Bool IsFixedPointFallbackReadAttachment() {
            const auto* record = MG_Pipe::MGPipeApplier().ReadFramebuffer();
            return record && record->ReadSurface.Kind != MG_Pipe::kMGPipeSurfaceKindNone &&
                   IsSnormFormat(static_cast<TextureInternalFormat>(record->ReadSurface.InternalFormat));
            const auto& readFBO =
                MG_Pipe::gPipeInputs->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
            if (!readFBO) {
                return false;
            }
            const auto readBuffer = readFBO->GetReadBuffer();
            if (readBuffer < FramebufferAttachmentType::Color0 || readBuffer > FramebufferAttachmentType::Color31) {
                return false;
            }
            // Any signed-normalized attachment, not just the ones currently substituted:
            // ES has no GL_CLAMP_READ_COLOR at all, so even a natively stored SNORM buffer
            // hands back the negative half that desktop GL clamps away.
            const auto& attachmentObject = readFBO->GetAttachment(readBuffer);
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && IsSnormFormat(textureObject->GetFormat());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject && IsSnormFormat(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        const MG_Pipe::MGPFramebufferState* PushedFramebufferRecord(MG_Pipe::MGPipeHandle fbo) {
            // FramebufferRecordFor answers null for the null handle and for a slot nothing has
            // described (both silent - that is every framebuffer before its first emission), and
            // refuses a stale generation loudly while counting it in
            // StaleFramebufferRecordLookups. So there is no second emptiness test here to get out
            // of step with the applier's, and none of the three cases is a binding question.
            return MG_Pipe::MGPipeApplier().FramebufferRecordFor(fbo);
        }

        // The record's DrawBuffers[] is an ATTACHMENT INDEX with -1 for None (D-C1), which is
        // what the frontend array holds for every framebuffer that has a twin. It deliberately
        // cannot spell FrontLeft / FrontRight / BackLeft / BackRight: those are the DEFAULT
        // framebuffer's own tokens, the default framebuffer has no BackendFramebufferObject at
        // all (it is pDefaultFramebufferInfo->defaultFBO, which is why MGPipeHandles.h reserves
        // {0,1} for it), and the legacy arm's branch for them says so in as many words - "shouldn't
        // remap". So the conversion below is total for every object that reaches this twin, and
        // the record's IsDefault is the thing a later phase would test if that ever changed.
        static void DecodePushedDrawBuffers(const MG_Pipe::MGPFramebufferState& record,
                                            FramebufferAttachmentType* out) {
            for (Uint i = 0; i < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
                const Int8 index = record.DrawBuffers[i];
                out[i] = index < 0 ? FramebufferAttachmentType::None
                                   : static_cast<FramebufferAttachmentType>(
                                         static_cast<Int>(FramebufferAttachmentType::Color0) + index);
            }
        }

        // P5e (fb, §5.4): the record half of SyncReadBufferToBackend, lifted out of it so the
        // by-handle sync can run it WITHOUT a frontend object. Nothing inside reads one: the
        // frontend index was only ever a diagnostic, and it is passed as a number now
        // (GlNameForDiag is 0 on the handle arm, which reads as "the record did not say").
        void BackendFramebufferObject::ApplyReadBufferFromRecord(const MG_Pipe::MGPFramebufferState& record,
                                                                 Uint glNameForDiag) {
            // D-C1 makes the NULL Res the spelling of "no attachment" and c0c's
            // kMGPipeSurfaceKindNone makes Kind agree with it on a zero-initialised record.
            // Res stays the test - it is the one D-C1 fixes - and Kind is CROSS-CHECKED
            // rather than consulted: the two are one statement the emitter has to make
            // twice, and a record where they disagree is a seam defect, not an empty point.
            // Picking a winner silently is what this refuses to do.
            const Bool readSurfaceEmpty = MG_Pipe::MGPipeHandleIsNull(record.ReadSurface.Res);
            if (readSurfaceEmpty != (record.ReadSurface.Kind == MG_Pipe::kMGPipeSurfaceKindNone)) {
                MGLOG_E_ONCE("MGPipe: framebuffer %u's resolved read surface says Res={%u, %u} and "
                             "Kind=%u, which disagree about whether it names anything - refusing "
                             "rather than choosing one of them",
                             glNameForDiag, record.ReadSurface.Res.Slot, record.ReadSurface.Res.Gen,
                             record.ReadSurface.Kind);
                return;
            }
            FramebufferAttachmentType pushedReadBuf = FramebufferAttachmentType::None;
            if (!readSurfaceEmpty) {
                // EVERY FIELD THAT IDENTIFIES THE IMAGE, not the three v1 compared (m-4).
                // The same image legally sits at two colour points - a layered attachment of
                // one array and a single slice of it are different surfaces of one Res - so a
                // partial comparison resolves to the lower index and the refusal branch below
                // ("refusing to guess") would then be doing part of the guessing itself.
                // InternalFormat and TextureTarget are properties of the RESOURCE rather than
                // of the point, so they add nothing to the identity and are left out.
                Bool matched = false;
                for (Uint i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i) {
                    const auto& color = record.Color[i];
                    if (color.Res == record.ReadSurface.Res && color.Kind == record.ReadSurface.Kind &&
                        color.Layered == record.ReadSurface.Layered && color.Level == record.ReadSurface.Level &&
                        color.Layer == record.ReadSurface.Layer &&
                        color.UploadTarget == record.ReadSurface.UploadTarget) {
                        pushedReadBuf = static_cast<FramebufferAttachmentType>(
                            static_cast<Int>(FramebufferAttachmentType::Color0) + static_cast<Int>(i));
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    MGLOG_E_ONCE("MGPipe: framebuffer %u's resolved read surface {%u, %u} matches no "
                                 "colour attachment of its own record - refusing to guess a read "
                                 "buffer",
                                 glNameForDiag, record.ReadSurface.Res.Slot, record.ReadSurface.Res.Gen);
                    return;
                }
            }
            if (pushedReadBuf == m_frontendReadBuffer) {
                return;
            }
            m_frontendReadBuffer = pushedReadBuf;

            const GLenum glPushedReadBuffer = GetBackendAttachmentType(pushedReadBuf);
            if (m_backendReadBuffer != glPushedReadBuffer) {
                m_backendReadBuffer = glPushedReadBuffer;
                // Still bound as READ first, for the reason the legacy arm gives below:
                // glReadBuffer targets whatever FBO is bound to GL_READ_FRAMEBUFFER.
                Bind(FramebufferTarget::Read);
                g_GLESFuncs.glReadBuffer(glPushedReadBuffer);
            }
        }

        // P5e (fb): the by-handle entry, for SyncCurrentFBOByRecord's "one object on both
        // bindings" skip - the one path that applies a read buffer without doing the rest of
        // the sync. A framebuffer with no record is the caller's seam defect, not a silent
        // no-op, so it says so.
        void BackendFramebufferObject::SyncReadBufferToBackendByHandle(MG_Pipe::MGPipeHandle fbo) {
            const auto* record = PushedFramebufferRecord(fbo);
            if (record == nullptr) {
                MGLOG_E_ONCE("MGPipe: framebuffer handle {%u, %u} has no applier record, so its read "
                             "buffer cannot be pushed",
                             fbo.Slot, fbo.Gen);
                return;
            }
            ApplyReadBufferFromRecord(*record, 0);
        }

        Bool BackendFramebufferObject::RecomputeBackendColorSlots(
            const FramebufferObject::FramebufferAttachmentArray& stateDrawBuffers) {
            // Only the first GL_MAX_COLOR_ATTACHMENTS points exist in the backend. The frontend's own
            // limit (ValidateColorAttachmentInRange, which reads the clamped
            // GetDynamicParameters().MaxColorAttachments) is never larger than this raw ES cap, so an
            // index the frontend accepted is always < slotCount. Indices at or above it can never own
            // an image and stay on their identity point - never touched, never a GL error.
            const Uint slotCount =
                std::min<Uint>(MAX_COLOR_ATTACHMENT_SLOTS,
                               static_cast<Uint>(std::max<Int>(g_GLESCapabilities.MaxColorAttachments, 1)));

            GLenum newSlots[MAX_COLOR_ATTACHMENT_SLOTS];
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                newSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            Bool assigned[MAX_COLOR_ATTACHMENT_SLOTS] = {false};
            Bool slotTaken[MAX_COLOR_ATTACHMENT_SLOTS] = {false};

            // 1. ES pins draw-buffer slot s to GL_COLOR_ATTACHMENTs, so an attachment named by draw
            //    buffer slot s has no choice: its image must sit at backend point s. This has to
            //    agree with the compaction the caller just pushed through glDrawBuffers.
            for (Uint s = 0; s < FramebufferObject::MAX_DRAW_BUFFERS && s < slotCount; ++s) {
                const auto frontendBuf = stateDrawBuffers[s];
                if (frontendBuf < FramebufferAttachmentType::Color0 ||
                    frontendBuf > FramebufferAttachmentType::Color31) {
                    continue; // GL_NONE, or a default-framebuffer FRONT/BACK token: never relocated.
                }
                const Uint a = static_cast<Uint>(frontendBuf) - static_cast<Uint>(FramebufferAttachmentType::Color0);
                // Neither guard may ever fire: a duplicate draw buffer is already INVALID_OPERATION
                // and an out-of-range one is rejected by ValidateColorAttachmentInRange. If one did
                // fire the table would disagree with the glDrawBuffers the caller already issued,
                // which is the exact non-injectivity this table exists to remove.
                MOBILEGL_ASSERT(a < slotCount && !assigned[a],
                                "Draw buffer %u names colour attachment %u which is out of range or duplicated.", s, a);
                if (a >= slotCount || assigned[a]) {
                    continue;
                }
                newSlots[a] = GL_COLOR_ATTACHMENT0 + s;
                assigned[a] = true;
                slotTaken[s] = true;
            }

            // 2. Everything else keeps its identity point when that point survived step 1. This is
            //    what makes the ordinary drawBuffers[s] == COLOR_ATTACHMENTs case a strict no-op:
            //    the table stays identity, nothing moves, no attachment is re-issued.
            for (Uint a = 0; a < slotCount; ++a) {
                if (assigned[a] || slotTaken[a]) {
                    continue;
                }
                newSlots[a] = GL_COLOR_ATTACHMENT0 + a;
                assigned[a] = true;
                slotTaken[a] = true;
            }

            // 3. What is left are attachments whose identity point step 1 took away. Park them on the
            //    lowest free point. They are not draw buffers, so nothing is rendered through them;
            //    they only have to stay addressable for glReadBuffer and blits, and the map has to
            //    stay injective so reading one of them cannot land on another's image.
            for (Uint a = 0; a < slotCount; ++a) {
                if (assigned[a]) {
                    continue;
                }
                for (Uint s = 0; s < slotCount; ++s) {
                    if (!slotTaken[s]) {
                        newSlots[a] = GL_COLOR_ATTACHMENT0 + s;
                        assigned[a] = true;
                        slotTaken[s] = true;
                        break;
                    }
                }
            }

            Bool moved = false;
            for (Uint a = 0; a < MAX_COLOR_ATTACHMENT_SLOTS; ++a) {
                if (m_backendColorSlots[a] == newSlots[a]) {
                    continue;
                }
                m_backendColorSlots[a] = newSlots[a];
                moved = true;
                // This attachment's image now belongs at a different backend point. Its frontend
                // version has not changed, so the attachment loop would skip it; force it.
                m_syncedFrontendAttachmentVersions[static_cast<SizeT>(FramebufferAttachmentType::Color0) + a] =
                    static_cast<Uint16>(~0u);
            }
            return moved;
        }

        // ---- P5e (fb, CONTRACT-P5E.md §5.4): the reverse index, texture -> framebuffers ------
        //
        // WHY IT EXISTS. ScopedDetachedTextureFramebufferAttachments used to answer "which
        // framebuffers currently have this texture attached" by walking every live twin and
        // reading each one's FRONTEND attachment array. Under P5e that walk is the last place
        // the server holds a frontend framebuffer across records - id re-typed ForEachLive and
        // left the StateForHandle read greppable, naming this package as the one that replaces
        // it. The records already carry the answer: every applied set_framebuffer_state names
        // its eleven surfaces, so the relation is maintained where the record is consumed.
        //
        // KEYED ON THE TEXTURE SLOT rather than on {slot, gen}. The question a detach asks is
        // "does the driver have this texture id hanging off some framebuffer", the points are
        // re-derived from the record on every hit, and a stale entry left by a recycled slot
        // therefore costs one record walk that matches nothing. The FRAMEBUFFER handles are
        // stored whole, so a recycled framebuffer slot never answers for its predecessor
        // (FindByHandle refuses a moved Gen).
        //
        // MAINTAINED AT THE SYNC, NOT AT THE APPLY, and that is the same set rather than a
        // smaller one: a framebuffer whose record was never synced has no driver attachment to
        // detach, and every path that can have one runs this sync first.

        namespace {
            UnorderedMap<Uint32, Vector<MG_Pipe::MGPipeHandle>> g_framebuffersByAttachedTextureSlot;
            UnorderedMap<Uint32, Vector<Uint32>> g_attachedTextureSlotsByFramebufferSlot;

            void ForgetFramebufferTextureAttachmentsLocked(Uint32 fboSlot) {
                auto it = g_attachedTextureSlotsByFramebufferSlot.find(fboSlot);
                if (it == g_attachedTextureSlotsByFramebufferSlot.end()) return;
                for (const Uint32 textureSlot : it->second) {
                    auto listIt = g_framebuffersByAttachedTextureSlot.find(textureSlot);
                    if (listIt == g_framebuffersByAttachedTextureSlot.end()) continue;
                    auto& list = listIt->second;
                    list.erase(std::remove_if(list.begin(), list.end(),
                                              [&](MG_Pipe::MGPipeHandle h) { return h.Slot == fboSlot; }),
                               list.end());
                    if (list.empty()) g_framebuffersByAttachedTextureSlot.erase(listIt);
                }
                g_attachedTextureSlotsByFramebufferSlot.erase(it);
            }
        } // namespace

        void NoteFramebufferTextureAttachments(MG_Pipe::MGPipeHandle fbo, const MG_Pipe::MGPFramebufferState& record) {
            if (MG_Pipe::MGPipeHandleIsNull(fbo)) return;
            ForgetFramebufferTextureAttachmentsLocked(fbo.Slot);
            Vector<Uint32> textureSlots;
            const auto note = [&](const MG_Pipe::MGPSurface& surface) {
                if (surface.Kind != MG_Pipe::kMGPipeSurfaceKindTexture) return;
                if (MG_Pipe::MGPipeHandleIsNull(surface.Res)) return;
                if (std::find(textureSlots.begin(), textureSlots.end(), surface.Res.Slot) != textureSlots.end()) {
                    return; // one texture at two points is one entry
                }
                textureSlots.push_back(surface.Res.Slot);
                g_framebuffersByAttachedTextureSlot[surface.Res.Slot].push_back(fbo);
            };
            for (Uint i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i)
                note(record.Color[i]);
            note(record.Depth);
            note(record.Stencil);
            if (textureSlots.empty()) return; // the common Minecraft frame: renderbuffers only
            g_attachedTextureSlotsByFramebufferSlot[fbo.Slot] = std::move(textureSlots);
        }

        // ---- P5e (fb, §5.4): BackendFramebufferObject::SyncToBackend BY HANDLE ---------------
        //
        // The record's ELEVEN SURFACES ARE THE POINT SET. The object form walks the frontend's
        // 41 attachment points to learn which exist; this one does not have to, because the
        // emitter REFUSES a framebuffer holding a point at or above the wire width
        // (FramebufferEmit.h's two refusals) and the FRONT/BACK tokens belong to the default
        // framebuffer, which has no twin. So Color[0..7] + Depth + Stencil is the whole of it,
        // and a framebuffer that could not be described that way never produced a record at all.
        //
        // THE PER-POINT VERSION MEMO IS GONE and is not replaced. On the object form it is a
        // second, narrower gate under the record hash, hedging against "a frontend attachment
        // moved without the record moving" - which is unrepresentable once the record is the
        // only statement of what is attached (the hash covers every surface, and Fbo is inside
        // it, so a recycled handle cannot be suppressed against its predecessor). Keeping it
        // would have meant keeping the frontend array that carries it, which is the whole point
        // of this arm. The two SERVER-owned gates stay, because no client-side value can answer
        // what they answer: g_attachmentBackendIdGeneration ("did I re-mint a driver id") and
        // the re-entry it drives.
        void BackendFramebufferObject::SyncToBackendByHandle(MG_Pipe::MGPipeHandle fbo, FramebufferTarget asTarget) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const MG_Pipe::MGPFramebufferState* const recordPtr = PushedFramebufferRecord(fbo);
            if (recordPtr == nullptr) {
                MGLOG_E_ONCE("MGPipe: framebuffer handle {%u, %u} has no applier record, so it is not "
                             "configured here; nothing is bound by this refusal, but the caller's own "
                             "bind still targets it",
                             fbo.Slot, fbo.Gen);
                return;
            }
            const MG_Pipe::MGPFramebufferState& record = *recordPtr;
            if (record.IsDefault != 0) {
                // The default framebuffer has no twin (pDefaultFramebufferInfo->defaultFBO is the
                // frontend object and MGPipeHandles.h reserves {0,1} for it), so reaching this
                // body with one is a caller that skipped its own IsDefault branch.
                MGLOG_E_ONCE("MGPipe: framebuffer handle {%u, %u} describes the DEFAULT framebuffer, "
                             "which has no twin to configure",
                             fbo.Slot, fbo.Gen);
                return;
            }
            // Kept in step for the monolith-glue overloads that still resolve their record
            // through it (SyncReadBufferToBackend's push arm).
            m_pushedSyncHandle = fbo;
            const GLenum glFBOTarget = MG_Util::ConvertFramebufferTargetToGLEnum(asTarget);
            Bind(asTarget);

            FramebufferObject::FramebufferAttachmentArray stateDrawBuffers{};
            DecodePushedDrawBuffers(record, stateDrawBuffers.data());

            // 1. Remap draw buffers. Same rule as the object form: glDrawBuffers writes the
            //    DRAW-bound framebuffer's state, so a READ-only sync must neither issue it nor
            //    stamp the memo (the Minecraft 26.x OIT bug the object form names).
            Bool attachmentPointsMoved = false;
            const Bool drawBufferClean =
                memcmp(m_frontendDrawBuffers, stateDrawBuffers.data(),
                       FramebufferObject::MAX_DRAW_BUFFERS * sizeof(FramebufferAttachmentType)) == 0;
            if (!drawBufferClean && asTarget == FramebufferTarget::Draw) {
                memcpy(m_frontendDrawBuffers, stateDrawBuffers.data(),
                       FramebufferObject::MAX_DRAW_BUFFERS * sizeof(FramebufferAttachmentType));
                std::fill(m_backendDrawBuffers, m_backendDrawBuffers + FramebufferObject::MAX_DRAW_BUFFERS, GL_NONE);
                int nEffectiveBuffers = 0;
                for (GLint i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
                    const auto frontendBuf = stateDrawBuffers[i];
                    if (frontendBuf == FramebufferAttachmentType::None) {
                        m_backendDrawBuffers[i] = GL_NONE;
                        continue;
                    }
                    // DecodePushedDrawBuffers cannot produce a FRONT/BACK token - those are the
                    // default framebuffer's own and it has no twin - so the object form's
                    // "shouldn't remap" branch has no counterpart here.
                    m_backendDrawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
                    nEffectiveBuffers = i + 1;
                }
                g_GLESFuncs.glDrawBuffers(nEffectiveBuffers, m_backendDrawBuffers);
                if (RecomputeBackendColorSlots(stateDrawBuffers)) {
                    m_frontendReadBuffer = FramebufferAttachmentType::Unknown;
                    attachmentPointsMoved = true;
                }
            }

            // The four cross-object masks, off the record's own surfaces (ID-12 DV-5).
            if (asTarget == FramebufferTarget::Draw) {
                Uint32 snormClampOutputMask = 0;
                Uint32 unormClampOutputMask = 0;
                Uint32 alphaWidenedMask = 0;
                Uint32 integerColorMask = 0;
                for (Uint i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS && i < 32; ++i) {
                    const auto frontendBuf = stateDrawBuffers[i];
                    if (frontendBuf < FramebufferAttachmentType::Color0 ||
                        frontendBuf > FramebufferAttachmentType::Color31) {
                        continue;
                    }
                    const MG_Pipe::MGPSurface* surface = PushedSurfaceForAttachment(record, frontendBuf);
                    if (surface == nullptr) continue; // unrepresentable: DecodePushedDrawBuffers' range
                    if (IsSnormFallbackSurface(*surface)) {
                        snormClampOutputMask |= (1u << i);
                    } else if (IsUnormFallbackSurface(*surface)) {
                        unormClampOutputMask |= (1u << i);
                    }
                    if (IsAlphaWidenedColorSurface(*surface)) alphaWidenedMask |= (1u << i);
                    if (IsIntegerColorSurface(*surface)) integerColorMask |= (1u << i);
                }
                PrgramImpl::g_snormFallbackClampOutputMask = snormClampOutputMask;
                PrgramImpl::g_unormFallbackClampOutputMask = unormClampOutputMask;
                g_alphaWidenedDrawBufferMask = alphaWidenedMask;
                g_integerColorDrawBufferMask = integerColorMask;
            }

            // 2. Remap read buffer, READ-target only, for the reason the object form gives.
            if (asTarget == FramebufferTarget::Read) {
                ApplyReadBufferFromRecord(record, 0);
            }

            // 3. The attachments. Three gates, and the key is the object's rather than the
            //    binding's: idGenerationMoved ("I re-minted a driver id, so a point may hold a
            //    dead one"), attachmentPointsMoved ("the colour permutation moved, so a point
            //    whose record did not change still has to be re-issued") and the record hash.
            //    NOT m_syncedRecordHashes: the per-target array is the object arm's re-arm
            //    trigger, and the points written below belong to the driver FBO rather than to
            //    the binding they were walked for - see m_syncedAttachmentRecordHash.
            const Bool idGenerationMoved = m_syncedBackendIdGeneration != g_attachmentBackendIdGeneration;
            if (idGenerationMoved) {
                m_syncedBackendIdGeneration = g_attachmentBackendIdGeneration;
            }
            if (idGenerationMoved || attachmentPointsMoved || m_syncedAttachmentRecordHash != record.ContentHash) {
                const auto applyPoint = [&](FramebufferAttachmentType point, const MG_Pipe::MGPSurface& surface) {
                    const Bool isColorPoint =
                        point >= FramebufferAttachmentType::Color0 && point <= FramebufferAttachmentType::Color31;
                    const GLenum glBackendAttachment = isColorPoint
                                                           ? GetBackendAttachmentType(point)
                                                           : MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(point);
                    if (glBackendAttachment == GL_NONE || glBackendAttachment == GL_UNKNOWN_MGL) return;
                    const Bool empty =
                        surface.Kind == MG_Pipe::kMGPipeSurfaceKindNone || MG_Pipe::MGPipeHandleIsNull(surface.Res);
                    // The detach that keeps m_backendColorSlots a permutation of the PHYSICAL
                    // layout: a colour point whose record says empty must not keep the previous
                    // owner's image, or glReadBuffer returns it. Bounded by the driver's cap,
                    // because GL_COLOR_ATTACHMENTn above it is INVALID_ENUM.
                    if (isColorPoint && empty) {
                        const Int index = static_cast<Int>(point) - static_cast<Int>(FramebufferAttachmentType::Color0);
                        if (index < g_GLESCapabilities.MaxColorAttachments) {
                            g_GLESFuncs.glFramebufferRenderbuffer(glFBOTarget, glBackendAttachment, GL_RENDERBUFFER, 0);
                        }
                        return;
                    }
                    // No frontend attachment on this arm, by construction: the record IS the
                    // point set here (§5.4). Null is what selects nothing - the transport test
                    // inside is what selects the by-handle storage syncs.
                    SyncAttachmentSurface(glFBOTarget, surface, glBackendAttachment,
                                          /*monolithAttachment=*/nullptr);
                };
                for (Uint i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i) {
                    applyPoint(static_cast<FramebufferAttachmentType>(
                                   static_cast<Int>(FramebufferAttachmentType::Color0) + static_cast<Int>(i)),
                               record.Color[i]);
                }
                applyPoint(FramebufferAttachmentType::Depth, record.Depth);
                applyPoint(FramebufferAttachmentType::Stencil, record.Stencil);
                m_syncedAttachmentRecordHash = record.ContentHash;
                NoteFramebufferTextureAttachments(fbo, record);
            }

            // 4. ARB_framebuffer_no_attachments. With no attachment at all the record's geometry
            //    IS the framebuffer's default width/height/layers/samples (FillGeometry), and the
            //    driver FBO needs them too: without a default size an attachment-less ES
            //    framebuffer is incomplete and every draw into it is dropped. With attachments the
            //    defaults are ignored by GL, so they are only pushed for the empty shape.
            Bool hasAnyAttachment = !(record.Depth.Kind == MG_Pipe::kMGPipeSurfaceKindNone ||
                                      MG_Pipe::MGPipeHandleIsNull(record.Depth.Res)) ||
                                    !(record.Stencil.Kind == MG_Pipe::kMGPipeSurfaceKindNone ||
                                      MG_Pipe::MGPipeHandleIsNull(record.Stencil.Res));
            for (Uint i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments && !hasAnyAttachment; ++i) {
                hasAnyAttachment = !(record.Color[i].Kind == MG_Pipe::kMGPipeSurfaceKindNone ||
                                     MG_Pipe::MGPipeHandleIsNull(record.Color[i].Res));
            }
            if (!hasAnyAttachment) {
                const Array<Int, 5> defaults = {
                    static_cast<Int>(record.Width), static_cast<Int>(record.Height), static_cast<Int>(record.Layers),
                    std::min<Int>(static_cast<Int>(record.Samples), g_GLESCapabilities.MaxFramebufferSamples),
                    static_cast<Int>(record.FixedSampleLocations != 0 ? GL_TRUE : GL_FALSE)};
                if (!m_syncedNoAttachmentDefaultsValid || m_syncedNoAttachmentDefaults != defaults) {
                    g_GLESFuncs.glFramebufferParameteri(glFBOTarget, GL_FRAMEBUFFER_DEFAULT_WIDTH, defaults[0]);
                    g_GLESFuncs.glFramebufferParameteri(glFBOTarget, GL_FRAMEBUFFER_DEFAULT_HEIGHT, defaults[1]);
                    if (g_GLESCapabilities.SupportsFramebufferDefaultLayers) {
                        g_GLESFuncs.glFramebufferParameteri(glFBOTarget, GL_FRAMEBUFFER_DEFAULT_LAYERS, defaults[2]);
                    }
                    g_GLESFuncs.glFramebufferParameteri(glFBOTarget, GL_FRAMEBUFFER_DEFAULT_SAMPLES, defaults[3]);
                    g_GLESFuncs.glFramebufferParameteri(glFBOTarget, GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS,
                                                        defaults[4]);
                    m_syncedNoAttachmentDefaults = defaults;
                    m_syncedNoAttachmentDefaultsValid = true;
                }
            }

            // The walk itself can re-mint a driver id (the storage sync behind a texture
            // surface), invalidating points this walk already attached. Re-enter until the
            // generation is quiescent, exactly as the object form does - every pass syncs each
            // dirty texture clean, so each repeat finds strictly fewer re-mints.
            if (m_syncedBackendIdGeneration != g_attachmentBackendIdGeneration) {
                SyncToBackendByHandle(fbo, asTarget);
            }
        }

        GLenum BackendFramebufferObject::GetBackendAttachmentType(FramebufferAttachmentType frontendAtt) const {
            // Only colour attachments are ever relocated; depth/stencil, the default framebuffer's
            // FRONT/BACK names and None map straight through.
            if (frontendAtt < FramebufferAttachmentType::Color0 || frontendAtt > FramebufferAttachmentType::Color31) {
                return MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendAtt);
            }
            // The table is a permutation of the backend colour points, so this is the one point that
            // owns this attachment. Searching the draw-buffer array instead returned the identity
            // point for every attachment that was not a draw buffer - which is exactly the point a
            // relocated draw buffer had just taken over, so COLOR_ATTACHMENT0 read back the image of
            // whatever attachment was last made the draw buffer.
            const Uint index = static_cast<Uint>(frontendAtt) - static_cast<Uint>(FramebufferAttachmentType::Color0);
            return m_backendColorSlots[index];
        }

        TwinRegistry<MG_State::GLState::FramebufferObject, BackendFramebufferObject, MG_Pipe::MGPipeKind::Framebuffer>
            g_backendFramebufferObjects;
        Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedSlotVersions = {0};
        // Tracks the bound FBO's object version (bumped on any attachment/drawbuffer change)
        // per target: re-attaching textures or changing draw buffers on an already-bound FBO
        // must re-sync it even when the binding-slot version has not moved.
        Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedObjectVersions = {0};
        Array<MG_State::GLState::FramebufferObject*, SizeT(FramebufferTarget::FramebufferTargetCount)>
            g_fboSyncedObjects = {};
        Uint64 g_attachmentBackendIdGeneration = 0;
        Array<Uint64, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedBackendIdGenerations = {0};
    } // namespace FramebufferImpl

    namespace ScratchFBOImpl {
        namespace {
            // ONE SET PER NATIVE CONTEXT: a framebuffer belongs to the context that made it
            // (DirectGLES.h, PerNativeContext), and so does the attachment shadow kept for it.
            struct ScratchSet {
                ScratchFramebuffer temp;
                ScratchFramebuffer blitRead;
                ScratchFramebuffer blitDraw;
                Uint completeTinyFBOId = 0;
                Uint completeTinyRBOId = 0;
            };
            PerNativeContext<ScratchSet> g_scratchSets;

            // Detach every point the shadow no longer vouches for. Used when the
            // shadow is unknown (context reset, texture id deleted while attached).
            void ScrubAllAttachments(ScratchFramebuffer& fb, GLenum fbTarget) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                fb.colorTex = 0;
                fb.colorTarget = 0;
                fb.colorLevel = 0;
                fb.colorLayer = -1;
                fb.depthTex = 0;
                fb.depthTarget = 0;
                fb.depthLevel = 0;
                fb.depthLayer = -1;
                fb.depthHasStencil = false;
                fb.depthIsStencilOnly = false;
                fb.attachmentsKnown = true;
            }

            void PrepareForUse(ScratchFramebuffer& fb, GLenum fbTarget) {
                if (!fb.attachmentsKnown) {
                    ScrubAllAttachments(fb, fbTarget);
                }
            }

            // The post-attach glGetError probe below must not misread an error some
            // earlier operation left queued; drain before attaching (rare path -
            // only runs when the attachment actually changes).
            void DrainPendingGLErrors() {
                while (g_GLESFuncs.glGetError() != GL_NO_ERROR) {}
            }

            // Record the color point as detached when the shadow said something was
            // there; the actual detach call is the caller's (it may be replaced by
            // the new attach directly when the point is being overwritten).
            void RecordNoColor(ScratchFramebuffer& fb) {
                fb.colorTex = 0;
                fb.colorTarget = 0;
                fb.colorLevel = 0;
                fb.colorLayer = -1;
            }

            void RecordNoDepth(ScratchFramebuffer& fb) {
                fb.depthTex = 0;
                fb.depthTarget = 0;
                fb.depthLevel = 0;
                fb.depthLayer = -1;
                fb.depthHasStencil = false;
                fb.depthIsStencilOnly = false;
            }
        } // namespace

        ScratchFramebuffer& TempFramebuffer() {
            return g_scratchSets.Current().temp;
        }
        ScratchFramebuffer& BlitReadFramebuffer() {
            return g_scratchSets.Current().blitRead;
        }
        ScratchFramebuffer& BlitDrawFramebuffer() {
            return g_scratchSets.Current().blitDraw;
        }

        Uint EnsureId(ScratchFramebuffer& fb) {
            if (fb.id == 0) {
                g_GLESFuncs.glGenFramebuffers(1, &fb.id);
                // A fresh FBO has nothing attached and COLOR_ATTACHMENT0 read/draw
                // buffers (the ES defaults for a non-default framebuffer).
                fb.attachmentsKnown = true;
                RecordNoColor(fb);
                RecordNoDepth(fb);
                fb.readBuffer = GL_COLOR_ATTACHMENT0;
                fb.drawBuffer = GL_COLOR_ATTACHMENT0;
            }
            return fb.id;
        }

        void EnsureColorAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget, GLint level) {
            PrepareForUse(fb, fbTarget);
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                RecordNoDepth(fb);
            }
            if (fb.colorTex == tex && fb.colorTarget == texTarget && fb.colorLevel == level && fb.colorLayer < 0) {
                return;
            }
            if (fb.colorTex != 0) {
                // Detach first: if the new attach fails, the point must read as
                // missing (incomplete FBO), not silently keep the old texture.
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, texTarget, tex, level);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoColor(fb);
                return;
            }
            fb.colorTex = tex;
            fb.colorTarget = texTarget;
            fb.colorLevel = level;
            fb.colorLayer = -1;
        }

        void EnsureColorAttachmentLayer(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLint level, GLint layer) {
            PrepareForUse(fb, fbTarget);
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                RecordNoDepth(fb);
            }
            if (fb.colorTex == tex && fb.colorTarget == 0 && fb.colorLevel == level && fb.colorLayer == layer) {
                return;
            }
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTextureLayer(fbTarget, GL_COLOR_ATTACHMENT0, tex, level, layer);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoColor(fb);
                return;
            }
            fb.colorTex = tex;
            fb.colorTarget = 0;
            fb.colorLevel = level;
            fb.colorLayer = layer;
        }

        void EnsureDepthAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget, GLint level,
                                     Bool withStencil, Bool stencilOnly) {
            PrepareForUse(fb, fbTarget);
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                RecordNoColor(fb);
            }
            if (fb.depthTex == tex && fb.depthTarget == texTarget && fb.depthLevel == level && fb.depthLayer < 0 &&
                fb.depthHasStencil == withStencil && fb.depthIsStencilOnly == stencilOnly) {
                return;
            }
            if (fb.depthTex != 0) {
                // One call clears both depth and stencil points regardless of how
                // the previous attachment was made.
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            // A stencil-only texture has no depth point to attach at, and a depth one has no
            // stencil point; GL_DEPTH_STENCIL_ATTACHMENT is only the combined texture's form.
            g_GLESFuncs.glFramebufferTexture2D(
                fbTarget,
                stencilOnly ? GL_STENCIL_ATTACHMENT : (withStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT),
                texTarget, tex, level);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoDepth(fb);
                return;
            }
            fb.depthTex = tex;
            fb.depthTarget = texTarget;
            fb.depthLayer = -1;
            fb.depthLevel = level;
            fb.depthHasStencil = withStencil;
            fb.depthIsStencilOnly = stencilOnly;
        }

        // The layer-point sibling of EnsureColorAttachmentLayer above, for a view's mip chain in
        // a depth-only storage owner. depthTarget == 0 is the marker that this point was made
        // with glFramebufferTextureLayer, exactly as colorTarget == 0 is on the color side, so a
        // later 2D attach to the same level is never deduped against it.
        void EnsureDepthAttachmentLayer(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLint level, GLint layer) {
            PrepareForUse(fb, fbTarget);
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                RecordNoColor(fb);
            }
            if (fb.depthTex == tex && fb.depthTarget == 0 && fb.depthLevel == level && fb.depthLayer == layer &&
                !fb.depthHasStencil && !fb.depthIsStencilOnly) {
                return;
            }
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTextureLayer(fbTarget, GL_DEPTH_ATTACHMENT, tex, level, layer);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoDepth(fb);
                return;
            }
            fb.depthTex = tex;
            fb.depthTarget = 0;
            fb.depthLevel = level;
            fb.depthLayer = layer;
            fb.depthHasStencil = false;
            fb.depthIsStencilOnly = false;
        }

        void EnsureReadBuffer(ScratchFramebuffer& fb, GLenum readBuffer) {
            if (fb.readBuffer == readBuffer) {
                return;
            }
            g_GLESFuncs.glReadBuffer(readBuffer);
            fb.readBuffer = readBuffer;
        }

        void EnsureDrawBuffer(ScratchFramebuffer& fb, GLenum drawBuffer) {
            if (fb.drawBuffer == drawBuffer) {
                return;
            }
            g_GLESFuncs.glDrawBuffers(1, &drawBuffer);
            fb.drawBuffer = drawBuffer;
        }

        Uint EnsureCompleteTinyFramebufferId() {
            ScratchSet& set = g_scratchSets.Current();
            Uint& g_completeTinyFBOId = set.completeTinyFBOId;
            Uint& g_completeTinyRBOId = set.completeTinyRBOId;
            if (g_completeTinyFBOId != 0) {
                return g_completeTinyFBOId;
            }
            // One-time creation: the renderbuffer binding is context state with no
            // shadow, so save/restore it by query here (cold path only).
            GLint prevRenderbuffer = 0;
            g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
            g_GLESFuncs.glGenFramebuffers(1, &g_completeTinyFBOId);
            g_GLESFuncs.glGenRenderbuffers(1, &g_completeTinyRBOId);
            FramebufferImpl::BindFramebufferId(GL_FRAMEBUFFER, g_completeTinyFBOId);
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, g_completeTinyRBOId);
            g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
            g_GLESFuncs.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                                  g_completeTinyRBOId);
            const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
            g_GLESFuncs.glDrawBuffers(1, &drawBuffer);
            g_GLESFuncs.glReadBuffer(GL_COLOR_ATTACHMENT0);
            MOBILEGL_ASSERT(g_GLESFuncs.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                            "Scratch 1x1 framebuffer is incomplete.");
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<Uint>(prevRenderbuffer));
            return g_completeTinyFBOId;
        }

        void NoteTextureIdDeleted(Uint textureId) {
            if (textureId == 0) {
                return;
            }
            // Textures ARE shared, so the deleted id may sit in any context's scratch FBO.
            g_scratchSets.ForEach([textureId](ScratchSet& set) {
                for (ScratchFramebuffer* fb : {&set.temp, &set.blitRead, &set.blitDraw}) {
                    if (fb->colorTex == textureId || fb->depthTex == textureId) {
                        fb->attachmentsKnown = false;
                    }
                }
            });
        }

        void OnBackendContextDestroyed() {
            g_scratchSets.Clear();
        }
    } // namespace ScratchFBOImpl

    namespace PixelStoreImpl {
        namespace {
            PackState g_packState;
            Bool g_packStateKnown = false;

            void PinPackState(const PackState& value) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ALIGNMENT, value.Alignment);
                g_GLESFuncs.glPixelStorei(GL_PACK_ROW_LENGTH, value.RowLength);
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_ROWS, value.SkipRows);
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_PIXELS, value.SkipPixels);
                g_packState = value;
                g_packStateKnown = true;
            }
        } // namespace

        void ApplyPackState(const PackState& desired) {
            if (!g_packStateKnown) {
                PinPackState(desired);
                return;
            }
            if (desired.Alignment != g_packState.Alignment) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ALIGNMENT, desired.Alignment);
                g_packState.Alignment = desired.Alignment;
            }
            if (desired.RowLength != g_packState.RowLength) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ROW_LENGTH, desired.RowLength);
                g_packState.RowLength = desired.RowLength;
            }
            if (desired.SkipRows != g_packState.SkipRows) {
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_ROWS, desired.SkipRows);
                g_packState.SkipRows = desired.SkipRows;
            }
            if (desired.SkipPixels != g_packState.SkipPixels) {
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_PIXELS, desired.SkipPixels);
                g_packState.SkipPixels = desired.SkipPixels;
            }
        }

        PackState CurrentPackState() {
            if (!g_packStateKnown) {
                // Fresh/unknown context: pin to the GL defaults (what a new context
                // starts with; writing them makes the shadow authoritative either way).
                PinPackState(PackState{});
            }
            return g_packState;
        }

        void InvalidatePackStateCache() {
            g_packStateKnown = false;
        }
    } // namespace PixelStoreImpl

    namespace RenderbufferImpl {
        BackendRenderbufferObject::BackendRenderbufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenRenderbuffers(1, &m_backendRBOId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendRBOId == 0) {
                MGLOG_E_ONCE("Failed to generate renderbuffer object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            }
        }

        BackendRenderbufferObject::~BackendRenderbufferObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendRBOId == 0) {
                return;
            }
            // No driver-level renderbuffer-binding shadow exists (Bind() always issues the
            // call), so there is nothing to scrub here - only the id to release.
            if (m_contextGeneration == g_backendContextGeneration && g_GLESFuncs.glDeleteRenderbuffers) {
                g_GLESFuncs.glDeleteRenderbuffers(1, &m_backendRBOId);
            }
            m_backendRBOId = 0;
        }

        void BackendRenderbufferObject::Bind() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, m_backendRBOId);
        }

        // P4a (D-D2): the four values BackendRenderbufferObject::SyncToBackend allocates from,
        // spelled once per arm. Macros for the reason the texture-parameter block gives: in the
        // PULL build each expands to the pre-P4a expression at its original site, so that
        // build's tokens and its symbol sizes are unchanged (D-P).
        //
        // ARCHITECTURE.md §4.1 keeps the renderbuffer an INDEPENDENT frontend class while giving
        // it the same discriminated MGPResourceDesc a texture gets, and this is where that pays:
        // the same four fields, read out of the same record type, with Desc.Target telling the
        // applier which of the three per-kind tables the record lives in.
#define MGB_RBO_FORMAT                                                                                                 \
    (pushedRecord != nullptr ? static_cast<TextureInternalFormat>(pushedRecord->Desc.InternalFormat)                   \
                             : stateRBOObject->GetInternalFormat())
#define MGB_RBO_WIDTH                                                                                                  \
    (pushedRecord != nullptr ? static_cast<Int>(pushedRecord->Desc.Width)                                              \
                             : static_cast<Int>(stateRBOObject->GetWidth()))
#define MGB_RBO_HEIGHT                                                                                                 \
    (pushedRecord != nullptr ? static_cast<Int>(pushedRecord->Desc.Height)                                             \
                             : static_cast<Int>(stateRBOObject->GetHeight()))
#define MGB_RBO_SAMPLES                                                                                                \
    (pushedRecord != nullptr ? static_cast<Int>(pushedRecord->Desc.Samples)                                            \
                             : static_cast<Int>(stateRBOObject->GetSamples()))

#undef MGB_RBO_FORMAT
#undef MGB_RBO_WIDTH
#undef MGB_RBO_HEIGHT
#undef MGB_RBO_SAMPLES

        // P5e (fb, CONTRACT-P5E.md §5.4): the same allocation, keyed on the renderbuffer HANDLE.
        //
        // THE FOUR VALUES WERE ALREADY THE RECORD'S at P4a (D-D2) - what the object form still
        // needed the frontend for was the LOOKUP, `HandleOf(stateRBOObject.get())`, a
        // client-allocator probe on the apply thread. On this arm the handle arrives from the
        // attachment surface that named it, so nothing is probed and nothing is dereferenced.
        // An OVERLOAD rather than a changed signature (the monolith glue keeps its tokens).
        void BackendRenderbufferObject::SyncToBackendByHandle(MG_Pipe::MGPipeHandle renderbuffer) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const MG_Pipe::MGPipeResourceRecord* const pushedRecord = PipeRenderbufferRecordForHandle(renderbuffer);
            if (pushedRecord == nullptr) {
                MGLOG_E_ONCE("MGPipe: renderbuffer handle {%u, %u} has no applier record, so its "
                             "storage cannot be allocated from the pushed descriptor",
                             renderbuffer.Slot, renderbuffer.Gen);
                return;
            }
            if (m_isInitialized && m_syncedResourceSerial != 0 && m_syncedResourceSerial == pushedRecord->Serial) {
                return;
            }

            // A RE-STORAGE IS A REDEFINITION ON THE SAME DRIVER ID (P4a fable seam F-3): the
            // allocation below re-defines the storage behind the name the framebuffer twins
            // already attached, and no record of THEIRS moves for it, so the attachment memo
            // has to be re-armed the same way a texture re-mint arms it. The first allocation
            // is not a redefinition.
            if (m_isInitialized) {
                ++FramebufferImpl::g_attachmentBackendIdGeneration;
            }

            Bind();

            const auto internalFormat = static_cast<TextureInternalFormat>(pushedRecord->Desc.InternalFormat);
            const Int width = static_cast<Int>(pushedRecord->Desc.Width);
            const Int height = static_cast<Int>(pushedRecord->Desc.Height);
            const Int samples = static_cast<Int>(pushedRecord->Desc.Samples);
            GLenum glInternalFormat, glType, glFormat;
            TextureImpl::GenerateRenderbufferFormatInfo(internalFormat, &glInternalFormat, &glFormat, &glType);

            // Drain first, for the object form's reason: a driver that refuses the allocation
            // (a multi-gigabyte renderbuffer is refused routinely) used to leave the twin
            // claiming storage it does not have, and the attachment then rendered nowhere.
            DebugImpl::ErrorLopper::Clear();
            if (samples > 0) {
                const auto backendSamples = static_cast<GLsizei>(ClampSamplesToBackendSupport(
                    GetRenderbufferFormatCapabilityTargetIndex(), internalFormat, glFormat, samples));
                g_GLESFuncs.glRenderbufferStorageMultisample(GL_RENDERBUFFER, backendSamples, glInternalFormat,
                                                             static_cast<GLsizei>(width), static_cast<GLsizei>(height));
            } else {
                g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, glInternalFormat, static_cast<GLsizei>(width),
                                                  static_cast<GLsizei>(height));
            }
            if (g_GLESFuncs.glGetError() == GL_OUT_OF_MEMORY) {
                MGLOG_E_ONCE("Renderbuffer {%u, %u} storage allocation ran out of memory: %dx%d, "
                             "samples=%d, format=%s",
                             renderbuffer.Slot, renderbuffer.Gen, width, height, samples,
                             MG_Util::ConvertGLEnumToString(glInternalFormat).c_str());
                // NO RecordError HERE. The object form reports the OOM to the application
                // through the live GLContext; on this arm there is no application on this side
                // of the wire to report it to, and gPipeInputs->RecordError is itself one of the
                // BARRIER-PULLED rows (FieldOwnership.def) that rule F forbids an unbarriered
                // apply to touch. The client raises its own errors from its own validator.
            }
            DebugImpl::ErrorLopper::Clear();

            m_cacheInternalFormat = internalFormat;
            m_cacheWidth = width;
            m_cacheHeight = height;
            m_cacheSamples = samples;
            m_syncedResourceSerial = pushedRecord->Serial;
            m_isInitialized = true;
        }

        TwinRegistry<MG_State::GLState::RenderbufferObject, BackendRenderbufferObject,
                     MG_Pipe::MGPipeKind::Renderbuffer>
            g_backendRenderbufferObjects;
    } // namespace RenderbufferImpl
} // namespace MobileGL::MG_Backend::DirectGLES
