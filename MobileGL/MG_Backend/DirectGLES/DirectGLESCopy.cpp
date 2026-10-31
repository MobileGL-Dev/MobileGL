// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESCopy.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace BufferImpl {
        // Called by the application's own glMemoryBarrier: a barrier that orders storage writes
        // already did the work.
        static void NoteShaderStorageWritesOrdered() {
            g_shaderStorageWritesUnordered = false;
        }

        void SyncComputeBuffers(Bool includeDispatchIndirectBuffer) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            ProcessDeferredBufferReleases();
#if MOBILEGL_BUILD_DISAGGREGATED
            ProcessDeferredT0Retires(); // P11 B2
#endif
            // THE COMPUTE PATH IS THE ONE THAT NEEDS THE FRONTEND-INDEXED UNIFORM PASS, and it
            // is why the uniform class is emitted at all: compute does NOT go through the
            // per-program block remap in BindCurrentProgramWithResources, so these are the
            // points a compute shader actually reads.
            SyncBufferBindingPointsByRecord(MG_Pipe::kMGPipeShaderBufferClassUniform, GL_UNIFORM_BUFFER);
            SyncBufferBindingPointsByRecord(MG_Pipe::kMGPipeShaderBufferClassShaderStorage, GL_SHADER_STORAGE_BUFFER);
            if (includeDispatchIndirectBuffer) {
                // P5c (hd, CONTRACT-P5C §3.5): see the draw-indirect twin above - the verb
                // record's handle, never the frontend binding slot or the client allocator.
                const MG_Pipe::MGPipeHandle handle = MG_Pipe::MGPipeApplier().VerbDispatchIndirectBuffer;
                if (!MG_Pipe::MGPipeHandleIsNull(handle)) {
                    auto* resource = EnsureBufferResourceForHandle(nullptr, handle);
                    if (resource && resource->id != 0) {
                        BindBufferId(GL_DISPATCH_INDIRECT_BUFFER, resource->id);
                    }
                }
            }
        }
    } // namespace BufferImpl

    namespace TextureImpl {
        // P5e (fb): the eager glBindImageTexture funnel's HANDLE ARM, and it binds nothing.
        //
        // `bind_shader_image` is an unbarriered row (§2.2) whose backend entry point takes GL
        // arguments and a GL NAME - the record's own MGPipeHandle (MGPImageBind::Res) stops at
        // the sink - so there is no way to resolve the texture's twin from what arrives here,
        // and the frontend binding this used to read is a BARRIER_PULLED row.
        //
        // Nothing is lost by deferring: SyncImageTextureBindings at the next validate point
        // re-binds every unit up to the mark from the applier's own MGPImageView array, and the
        // sink bumps the texture shutter serial immediately after this call
        // (MGPipeApplierNoteTextureStateMoved), which is one of the four values the sweep's gate
        // is keyed on - so the sweep that follows this bind cannot be suppressed. An image unit
        // has no reader but a shader, and no shader runs between here and that validate point.
        //
        // WHAT MUST STILL HAPPEN HERE is the high-water mark: it is the "no draw in this context
        // can be reading an image" early-out, so a unit that is given a texture without raising
        // it would be skipped by every sweep afterwards.
        static void NoteImageUnitBoundWithoutReadingTheFrontend(Uint unit, Bool holdsTexture) {
            if (!holdsTexture) return;
            if (unit + 1 > g_imageUnitHighWaterMark) {
                g_imageUnitHighWaterMark = unit + 1;
            }
        }
    } // namespace TextureImpl

    void PrepareForCompute(Bool includeDispatchIndirectBuffer) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        // Single per-dispatch program resolve and texture-key capture, as in
        // PrepareForDraw (nothing below can move either). The DISPATCH accessor: with a
        // pipeline bound this is its compute stage program, which is a whole program on its
        // own - the graphics composite a draw builds carries no compute stage.
        //
        // P5e (pa), kimi row 92: THE DISPATCH TWIN OF THE SAME RETIREMENT, the same arm and the
        // same conjunction - the three callees this value reaches are the ones PrepareForDraw
        // reaches, minus the attribute-values sync (a dispatch has no vertex stage) and the
        // frontend link/SPIR-V gate below, which already has a record arm of its own. These are
        // the 7 strict-lane entries of GetProgramForDispatch@DispatchCompute.
        const SharedPtr<MG_State::GLState::ProgramObject> noFrontendProgram;
        const auto& currentProgram = noFrontendProgram;
        const TextureImpl::DrawTextureSyncKeys textureKeys = TextureImpl::CaptureDrawTextureSyncKeys();

        // Read BEFORE this dispatch's own binding walk sets it again (see the flag's comment).
        const Bool storageWritesUnordered = BufferImpl::g_shaderStorageWritesUnordered;
        BufferImpl::g_shaderStorageWritesUnordered = false;
        BufferImpl::SyncComputeBuffers(includeDispatchIndirectBuffer);
        TextureImpl::SyncNeccessaryTextures(textureKeys);
        TextureImpl::SyncImageTextureBindings();
        TextureImpl::MarkWritableImageBufferTexturesGpuWritten();
        // Both syncs above may have re-uploaded a texture this dispatch reads or stores
        // (Managers.h, NoteInPlaceTextureUpload), and the previous shader work may have left
        // storage writes unordered; one barrier covers whichever of the two applies.
        {
            GLbitfield barriers = TextureImpl::TakeInPlaceTextureUploadBarrierBits();
            if (storageWritesUnordered) barriers |= GL_SHADER_STORAGE_BARRIER_BIT;
            if (barriers != 0 && g_GLESFuncs.glMemoryBarrier) g_GLESFuncs.glMemoryBarrier(barriers);
        }
        // P5e (pg), kimi row 92: the DISPATCH half of the same retirement -
        // MGPipeApplier().DispatchProgram, which set_dispatch_program carried. A compute-only
        // pipeline has no draw program at all, which is why the two applier fields are separate.
        const MG_Pipe::MGPipeHandle dispatchCso = MG_Pipe::MGPipeApplier().DispatchProgram;
        PrgramImpl::SyncCurrentProgramByHandle(dispatchCso, false);

        // The same gate as the frontend one below, from the record: a dispatch of a program
        // the frontend reports unlinked, or one whose SPIR-V never arrived, binds nothing.
        const auto* const record = PrgramImpl::FindShaderCsoRecord(dispatchCso);
        if (record == nullptr || record->Desc.LinkStatus == 0 || record->Desc.SpirvStatus == 0) {
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
        BindCurrentProgramWithResources(currentProgram, textureKeys, dispatchCso);
    }

    Bool UpdateTextureBindingAtTarget(GLenum target) {
#ifdef TRACY_ENABLE
        ZoneScopedNC(__func__, TRACY_ZONECOLOR_BACKEND);
#endif
        auto unit = MG_Pipe::gPipeInputs->GetActiveTextureUnit();

        auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
        if (!TextureImpl::IsSupportedTextureTarget(textureTarget)) {
            MGLOG_E_ONCE("    Texture target %s is not supported, skipping.",
                         MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
            return false;
        }

        // P5c (hd, CONTRACT-P5C §3.4): under an active transport the destination is the handle
        // the copy_framebuffer_to_texture record carried; the unit binding slot and the client
        // allocator are never read (T2/T4).
        const MG_Pipe::MGPipeHandle dstHandle = MG_Pipe::MGPipeApplier().VerbCopyTexDst;
        auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.GetOrCreateByHandle(dstHandle);
        if (backendTextureSlot == nullptr) {
            MGLOG_E_ONCE("%s: the verb's destination texture handle {%u, %u} is refused by "
                         "the twin slot table (live generation %u)",
                         __func__, dstHandle.Slot, dstHandle.Gen,
                         TextureImpl::g_backendTextureObjects.LiveGenAt(dstHandle.Slot));
            return false;
        }
        auto& backendObj = *backendTextureSlot;
        if (!backendObj) {
            backendObj = MakeShared<TextureImpl::BackendTextureObject>();
        }
        backendObj->Bind(TextureImpl::ConvertTextureTargetToBackendGLEnum(textureTarget), unit);
        return true;
    }

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

    // P5e (tx2): the inverse of MG_Pipe::MGPipeResourceTargetForTextureTarget, for the two
    // descriptor reads this file makes (Managers.cpp has the same inverse beside the storage
    // sync; it is file-local there, and a header for eleven cases would be the wrong trade).
    static TextureTarget TextureTargetForPipeResourceTarget(Uint8 pipeResourceTarget) {
        switch (static_cast<MG_Pipe::MGPipeResourceTarget>(pipeResourceTarget)) {
        case MG_Pipe::MGPipeResourceTarget::Tex1D:
            return TextureTarget::Texture1D;
        case MG_Pipe::MGPipeResourceTarget::Tex2D:
            return TextureTarget::Texture2D;
        case MG_Pipe::MGPipeResourceTarget::Tex3D:
            return TextureTarget::Texture3D;
        case MG_Pipe::MGPipeResourceTarget::Tex1DArray:
            return TextureTarget::Texture1DArray;
        case MG_Pipe::MGPipeResourceTarget::Tex2DArray:
            return TextureTarget::Texture2DArray;
        case MG_Pipe::MGPipeResourceTarget::TexCube:
            return TextureTarget::TextureCubeMap;
        case MG_Pipe::MGPipeResourceTarget::TexCubeArray:
            return TextureTarget::TextureCubeMapArray;
        case MG_Pipe::MGPipeResourceTarget::Tex2DMS:
            return TextureTarget::Texture2DMultisample;
        case MG_Pipe::MGPipeResourceTarget::Tex2DMSArray:
            return TextureTarget::Texture2DMultisampleArray;
        case MG_Pipe::MGPipeResourceTarget::TexRect:
            return TextureTarget::TextureRectangle;
        case MG_Pipe::MGPipeResourceTarget::TexBuffer:
            return TextureTarget::TextureBuffer;
        default:
            return TextureTarget::Unknown;
        }
    }

    // P8-B1: THE LEVELS A generate_mipmap RECORD GENERATES, end-exclusive. GL generates levels
    // BASE_LEVEL+1 .. q, q = min(p, MAX_LEVEL) with p the last level the base image's own chain
    // reaches, and an immutable texture's storage bounds the chain too (GL 4.6 core 8.14.4). The
    // client already computed that window (ComputeMipmapGenerationRange) and defined its levels
    // before emitting the verb, so the record's plan is the window and the descriptor is what
    // holds it: Base = VerbMipBaseLevel, End = min(VerbMipLevelCount, Desc.Levels, MaxLevel + 1,
    // Base + the base extent's chain) - the same clip Magma's GenerateWireMipmap makes.
    //
    // What stood here instead demanded the full chain from LEVEL 0 and died with
    // Fatal{UnmigratedEmulation, "generate-mipmap-storage"} whenever the descriptor held less: a
    // MAX_LEVEL below the chain, or glTexStorage2D with fewer levels, both legal GL.
    struct RecordMipWindow {
        Uint32 Base = 0;
        Uint32 End = 0; // end-exclusive; End <= Base + 1 generates nothing
    };

    static RecordMipWindow GenerateMipmapWindowByRecord(const MG_Pipe::MGPipeResourceRecord& record) {
        const auto& applier = MG_Pipe::MGPipeApplier();
        const auto& desc = record.Desc;
        RecordMipWindow window{applier.VerbMipBaseLevel, applier.VerbMipBaseLevel};
        if (desc.Width == 0 || desc.Levels == 0 || window.Base >= desc.Levels) return window;
        Uint32 end = std::min<Uint32>(applier.VerbMipLevelCount, desc.Levels);
        if (record.ParamsSerial) end = std::min<Uint32>(end, static_cast<Uint32>(record.Params.MaxLevel) + 1u);
        const IntVec3 baseExtent =
            MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, window.Base);
        const Int shrinking = MG_Record::StagedTextureShrinkingAxisCount(desc.Target);
        Int largest = 1;
        for (Int axis = 0; axis < shrinking && axis < 3; ++axis)
            largest = std::max<Int>(largest, baseExtent[axis]);
        Uint32 chain = 1;
        while (largest > 1) {
            largest /= 2;
            ++chain;
        }
        window.End = std::max(window.Base, std::min(end, window.Base + chain));
        return window;
    }

    static void AssertNoGLError(const char* operation) {
        const GLenum err = g_GLESFuncs.glGetError();
        MOBILEGL_ASSERT(err == GL_NO_ERROR, "%s failed: %s", operation, MG_Util::ConvertGLEnumToString(err).c_str());
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

        MGLOG_E_ONCE("%s failed: %s. target=%s, format=%s", operation, MG_Util::ConvertGLEnumToString(err).c_str(),
                     MG_Util::ConvertGLEnumToString(target).c_str(),
                     MG_Util::ConvertTextureInternalFormatToString(format).c_str());
        MG_Pipe::gPipeInputs->RecordError(
            ConvertGLESErrorToErrorCode(err),
            MakeUnique<GenericErrorInfo>("DirectGLES", operation, MG_Util::ConvertGLEnumToString(err)));
        return false;
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
        // P5e (tx2). THE SAME SWEEP, KEYED BY THE TEXTURE'S HANDLE.
        //
        // The frontend constructor below answers "which framebuffers attach this texture" by
        // walking every live FBO twin's frontend FramebufferObject and comparing attachment
        // SharedPtrs - kimi audit row 63, an unmemoised walk of client objects. The applier
        // already holds the answer: every framebuffer's record carries its eleven MGPSurfaces
        // and each one names its texture BY HANDLE (`MGPSurface::Res`), so the comparison is
        // {slot, gen} against {slot, gen} and the level / layered / upload-target details the
        // detach needs come off the same surface.
        //
        // This is the same reverse question fb's texture -> FBO-slot index answers for its
        // attachment sync; here it is asked over the records directly, because the walk is on a
        // rare path (per glGenerateMipmap) and does not need an index to be cheap.
        explicit ScopedDetachedTextureFramebufferAttachments(MG_Pipe::MGPipeHandle textureHandle) {
            if (MG_Pipe::MGPipeHandleIsNull(textureHandle)) return;
            auto* twin = TextureImpl::ResolveTextureTwin(textureHandle);
            if (twin == nullptr) return;
            const GLuint backendTextureId = twin->GetBackendTextureId();
            const auto& st = MG_Pipe::MGPipeApplier();

            FramebufferImpl::g_backendFramebufferObjects.ForEachLive(
                [&](MG_Pipe::MGPipeHandle fbo, const SharedPtr<FramebufferImpl::BackendFramebufferObject>& backendFBO) {
                    if (!backendFBO) return;
                    const auto* record = st.FramebufferRecordFor(fbo);
                    if (record == nullptr || record->IsDefault != 0) return;

                    const auto detachPoint = [&](const MG_Pipe::MGPSurface& surface,
                                                 FramebufferAttachmentType frontendType) {
                        if (surface.Kind != MG_Pipe::kMGPipeSurfaceKindTexture) return;
                        if (surface.Res.Slot != textureHandle.Slot || surface.Res.Gen != textureHandle.Gen) {
                            return;
                        }
                        GLenum backendAttachment = GL_NONE;
                        if (frontendType >= FramebufferAttachmentType::Color0 &&
                            frontendType <= FramebufferAttachmentType::Color31) {
                            backendAttachment = backendFBO->GetBackendAttachmentType(frontendType);
                        } else {
                            backendAttachment = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendType);
                        }
                        if (backendAttachment == GL_NONE || backendAttachment == GL_UNKNOWN_MGL) return;

                        GLenum textureTarget = TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(
                            static_cast<TextureUploadTarget>(surface.UploadTarget));
                        if (textureTarget == GL_UNKNOWN_MGL) {
                            textureTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(
                                static_cast<TextureTarget>(surface.TextureTarget));
                        }
                        const GLuint backendFBOId = backendFBO->GetBackendFramebufferId();
                        FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, backendFBOId);
                        if (surface.Layered != 0) {
                            g_GLESFuncs.glFramebufferTexture(GL_DRAW_FRAMEBUFFER, backendAttachment, 0, 0);
                        } else {
                            g_GLESFuncs.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, backendAttachment, textureTarget, 0,
                                                               0);
                        }
                        ClearGLErrors();
                        m_detachedAttachments.push_back({backendFBOId, backendAttachment, textureTarget,
                                                         backendTextureId, static_cast<GLint>(surface.Level),
                                                         surface.Layered != 0});
                    };

                    for (SizeT i = 0; i < 8; ++i) {
                        detachPoint(record->Color[i], static_cast<FramebufferAttachmentType>(
                                                          static_cast<SizeT>(FramebufferAttachmentType::Color0) + i));
                    }
                    detachPoint(record->Depth, FramebufferAttachmentType::Depth);
                    detachPoint(record->Stencil, FramebufferAttachmentType::Stencil);
                });
        }

        ~ScopedDetachedTextureFramebufferAttachments() {
            for (const auto& attachment : m_detachedAttachments) {
                FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, attachment.framebuffer);
                if (attachment.layered) {
                    g_GLESFuncs.glFramebufferTexture(GL_DRAW_FRAMEBUFFER, attachment.attachment, attachment.texture,
                                                     attachment.level);
                } else {
                    g_GLESFuncs.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment.attachment,
                                                       attachment.textureTarget, attachment.texture, attachment.level);
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

        g_GLESFuncs.glBlitFramebuffer(srcX, srcY, srcX + srcWidth, srcY + srcHeight, dstX, dstY, dstX + dstWidth,
                                      dstY + dstHeight, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
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

        g_GLESFuncs.glBlitFramebuffer(srcX, srcY, srcX + srcWidth, srcY + srcHeight, dstX, dstY, dstX + dstWidth,
                                      dstY + dstHeight, GL_COLOR_BUFFER_BIT, filter);
        AssertNoGLError("color texture blit");
    }

    // The layer-point siblings of the two blits above, for a texture VIEW's mip chain: those
    // texels live in the storage owner's layers [MinLayer, MinLayer + NumLayers) and a 2D
    // attach would address layer 0 of every one of them. One layer's extents are the whole
    // rectangle, exactly as in the 2D helpers.
    static void BlitDepthTextureLayer(GLuint srcTexture, GLint srcLevel, GLint srcLayer, GLsizei srcWidth,
                                      GLsizei srcHeight, GLuint dstTexture, GLint dstLevel, GLint dstLayer,
                                      GLsizei dstWidth, GLsizei dstHeight) {
        MOBILEGL_ASSERT(srcTexture != 0 && dstTexture != 0, "Depth blit requires valid backend textures.");
        MOBILEGL_ASSERT(srcLevel >= 0 && dstLevel >= 0, "Depth blit mip levels must be non-negative.");
        MOBILEGL_ASSERT(srcLayer >= 0 && dstLayer >= 0, "Depth blit layers must be non-negative.");
        MOBILEGL_ASSERT(srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0,
                        "Depth blit dimensions must be positive.");

        ClearGLErrors();
        ScopedDepthBlitState state;
        auto& readFB = ScratchFBOImpl::BlitReadFramebuffer();
        auto& drawFB = ScratchFBOImpl::BlitDrawFramebuffer();
        ScratchFBOImpl::EnsureDepthAttachmentLayer(readFB, GL_READ_FRAMEBUFFER, srcTexture, srcLevel, srcLayer);
        AssertNoGLError("attach depth blit source layer");
        ScratchFBOImpl::EnsureDepthAttachmentLayer(drawFB, GL_DRAW_FRAMEBUFFER, dstTexture, dstLevel, dstLayer);
        AssertNoGLError("attach depth blit destination layer");
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

        g_GLESFuncs.glBlitFramebuffer(0, 0, srcWidth, srcHeight, 0, 0, dstWidth, dstHeight, GL_DEPTH_BUFFER_BIT,
                                      GL_NEAREST);
        AssertNoGLError("depth texture layer blit");
    }

    static void BlitColorTextureLayer(GLuint srcTexture, GLint srcLevel, GLint srcLayer, GLsizei srcWidth,
                                      GLsizei srcHeight, GLuint dstTexture, GLint dstLevel, GLint dstLayer,
                                      GLsizei dstWidth, GLsizei dstHeight, GLenum filter) {
        MOBILEGL_ASSERT(srcTexture != 0 && dstTexture != 0, "Color blit requires valid backend textures.");
        MOBILEGL_ASSERT(srcLevel >= 0 && dstLevel >= 0, "Color blit mip levels must be non-negative.");
        MOBILEGL_ASSERT(srcLayer >= 0 && dstLayer >= 0, "Color blit layers must be non-negative.");
        MOBILEGL_ASSERT(srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0,
                        "Color blit dimensions must be positive.");
        MOBILEGL_ASSERT(filter == GL_NEAREST || filter == GL_LINEAR, "Color blit filter must be nearest or linear.");

        ClearGLErrors();
        ScopedDepthBlitState state;
        auto& readFB = ScratchFBOImpl::BlitReadFramebuffer();
        auto& drawFB = ScratchFBOImpl::BlitDrawFramebuffer();
        ScratchFBOImpl::EnsureColorAttachmentLayer(readFB, GL_READ_FRAMEBUFFER, srcTexture, srcLevel, srcLayer);
        AssertNoGLError("attach color blit source layer");
        ScratchFBOImpl::EnsureColorAttachmentLayer(drawFB, GL_DRAW_FRAMEBUFFER, dstTexture, dstLevel, dstLayer);
        AssertNoGLError("attach color blit destination layer");
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

        g_GLESFuncs.glBlitFramebuffer(0, 0, srcWidth, srcHeight, 0, 0, dstWidth, dstHeight, GL_COLOR_BUFFER_BIT,
                                      filter);
        AssertNoGLError("color texture layer blit");
    }

    // A mip chain generated inside a texture VIEW's window. Only the LAYER axis needs the
    // manual path: BASE_LEVEL/MAX_LEVEL are the view's own parameters, so the native
    // glGenerateMipmap already clips the LEVEL axis the way the view describes - but it writes
    // every layer of the storage the view aliases, and a view's layer window is a subset of
    // those.
    //
    // The levels here are the VIEW's own (level 0 aliases the storage's MinLevel), because that
    // is the space a view's descriptor extents and the frontend's own level sizes are measured
    // in. The blits are the one place the two systems meet, so the storage level a blit names is
    // MinLevel + the view's level.
    struct MipmapViewWindow {
        GLuint StorageTextureId = 0;
        Uint32 MinLevel = 0;
        Uint32 MinLayer = 0;
        Uint32 NumLayers = 1;
        Uint32 LogicalBase = 0;
        Uint32 LogicalEnd = 0; // view-relative, end-exclusive
        Bool Depth = false;
        GLenum Filter = GL_LINEAR;
    };

    template <typename ExtentAtLevel>
    static void GenerateMipmapThroughViewWindow(const MipmapViewWindow& window, ExtentAtLevel&& extentAtLevel) {
        if (window.StorageTextureId == 0 || window.NumLayers == 0) return;
        for (Uint32 level = window.LogicalBase + 1; level < window.LogicalEnd; ++level) {
            const IntVec3 srcSize = extentAtLevel(level - 1);
            const IntVec3 dstSize = extentAtLevel(level);
            MOBILEGL_ASSERT(srcSize.x() > 0 && srcSize.y() > 0 && dstSize.x() > 0 && dstSize.y() > 0,
                            "View mipmap generation needs non-empty source and destination extents.");
            const GLint srcLevel = static_cast<GLint>(window.MinLevel + level - 1);
            const GLint dstLevel = static_cast<GLint>(window.MinLevel + level);
            for (Uint32 i = 0; i < window.NumLayers; ++i) {
                const GLint layer = static_cast<GLint>(window.MinLayer + i);
                if (window.Depth) {
                    BlitDepthTextureLayer(window.StorageTextureId, srcLevel, layer, static_cast<GLsizei>(srcSize.x()),
                                          static_cast<GLsizei>(srcSize.y()), window.StorageTextureId, dstLevel, layer,
                                          static_cast<GLsizei>(dstSize.x()), static_cast<GLsizei>(dstSize.y()));
                } else {
                    BlitColorTextureLayer(window.StorageTextureId, srcLevel, layer, static_cast<GLsizei>(srcSize.x()),
                                          static_cast<GLsizei>(srcSize.y()), window.StorageTextureId, dstLevel, layer,
                                          static_cast<GLsizei>(dstSize.x()), static_cast<GLsizei>(dstSize.y()),
                                          window.Filter);
                }
            }
        }
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

    // WHICH LAYERS A VIEW'S MIP CHAIN MAY WRITE. A generation through a view has two coordinate
    // systems in it - the view's own level/layer window (the sampler view CSO) and its storage
    // owner's - and the native arm below carries only part of the first one: BASE_LEVEL and
    // MAX_LEVEL are the view's own parameters, so glGenerateMipmap clips the LEVEL axis the way
    // the view describes, but the layer set it writes is every layer of the storage the view
    // aliases. A windowed view's generation therefore rewrites owner layers outside its window,
    // which the view window itself says it must not.
    //
    // True means this record's generation was handled here (or refuted) and the native arm must
    // NOT run; false means "not this shape": no view, a view whose window covers the whole
    // storage - where the native call already agrees with GL - a multisample view, which has no
    // mip chain to generate, or an aspect the layer blits do not carry.
    static Bool GenerateMipmapThroughViewWindowByRecord(MG_Pipe::MGPipeHandle mipRes,
                                                        const MG_Pipe::MGPipeResourceRecord& record,
                                                        TextureInternalFormat format) {
        if (MG_Pipe::MGPipeHandleIsNull(record.Desc.ViewOf)) return false;
        if (record.Desc.Samples != 0) return false;
        const Bool depth = IsDepthOnlyFormat(format);
        if (!depth && !IsColorOnlyFormat(format)) return false;

        // The view record, validated as SyncTextureViewToBackendByRecord validates it: this
        // window is the only place the four numbers come from, and a stale or foreign one would
        // generate into another texture's levels.
        const auto* viewRecord = PipeSamplerViewRecordForHandle(record.ViewCso);
        if (viewRecord == nullptr || !viewRecord->Live || viewRecord->Gen != record.ViewCso.Gen) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"mipmap-view-record\"} {%u,%u}", mipRes.Slot, mipRes.Gen);
            std::abort();
        }
        const auto& view = viewRecord->View;
        if (view.Texture != mipRes || view.NumLevels == 0 || view.NumLayers == 0) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"mipmap-view-window\"} {%u,%u}", mipRes.Slot, mipRes.Gen);
            std::abort();
        }
        const auto* storageRecord = PipeTextureRecordForHandle(record.Desc.ViewOf);
        if (storageRecord == nullptr) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"mipmap-view-storage\"} {%u,%u}", mipRes.Slot, mipRes.Gen);
            std::abort();
        }
        if (view.MinLayer == 0 && view.NumLayers >= std::max<Uint32>(storageRecord->Desc.ArrayLayers, 1u)) return false;

        const auto& applier = MG_Pipe::MGPipeApplier();
        MipmapViewWindow window;
        window.MinLevel = view.MinLevel;
        window.MinLayer = view.MinLayer;
        window.NumLayers = view.NumLayers;
        window.LogicalBase = applier.VerbMipBaseLevel;
        // LevelCount is the logical END-EXCLUSIVE level, not a count after BaseLevel, and the
        // frontend already clipped it to the object's own range; the view's window is the bound
        // only this side can apply.
        window.LogicalEnd = std::min({static_cast<Uint32>(applier.VerbMipLevelCount),
                                      static_cast<Uint32>(record.Desc.Levels), static_cast<Uint32>(view.NumLevels)});
        window.Depth = depth;
        window.Filter = IsIntegerColorFormat(format) ? GL_NEAREST : GL_LINEAR;
        if (window.LogicalEnd <= window.LogicalBase + 1) return true;

        // The STORAGE's ES name is what the blits write: the view's own name aliases the same
        // texels, but only the owner's name carries every level and layer this window reaches.
        auto& storage = TextureImpl::SyncTextureToBackendByHandle(record.Desc.ViewOf);
        if (!storage || storage->GetBackendTextureId() == 0) {
            MGLOG_E_ONCE("MGPipe: generate_mipmap through view {%u, %u} has no storage texture to write; "
                         "the call is dropped",
                         mipRes.Slot, mipRes.Gen);
            return true;
        }
        window.StorageTextureId = storage->GetBackendTextureId();
        GenerateMipmapThroughViewWindow(window, [&record](Uint32 level) {
            return MG_Record::StagedTextureMipExtent(record.Desc.Target, record.Desc.Width, record.Desc.Height,
                                                     record.Desc.Depth, level);
        });
        return true;
    }

    // P5e (tx2): the two CPU-blit mip chains, driven by the RECORD the verb named. What changes
    // against the P5c arms below is only the resolution: those took the frontend texture and
    // re-derived its handle with a scoped `HandleOf` probe (the two sites §4.4 listed as
    // barriered), and the handle is now simply passed in - the extent derivation, the per-level
    // blits and the format question are §1's descriptor reads exactly as P5c wrote them.
    //
    // P8-B1: both chains run over the record's WINDOW (GenerateMipmapWindowByRecord), not from
    // level 1 to the end of the descriptor: a BASE_LEVEL above 0 is the source and a level past
    // MAX_LEVEL is left as it is, which is what the monolith twins below still get wrong.
    static void GenerateDepthTexture2DMipmapByRecord(const MG_Pipe::MGPipeResourceRecord& record,
                                                     const SharedPtr<TextureImpl::BackendTextureObject>& backendTexture,
                                                     const RecordMipWindow& window) {
        const auto& desc = record.Desc;
        const GLuint textureId = backendTexture->GetBackendTextureId();
        for (Uint32 level = window.Base + 1; level < window.End; ++level) {
            const IntVec3 srcSize =
                MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, level - 1);
            const IntVec3 dstSize =
                MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, level);
            BlitDepthTexture2D(textureId, static_cast<GLint>(level - 1), 0, 0, static_cast<GLsizei>(srcSize.x()),
                               static_cast<GLsizei>(srcSize.y()), textureId, static_cast<GLint>(level), 0, 0,
                               static_cast<GLsizei>(dstSize.x()), static_cast<GLsizei>(dstSize.y()));
        }
    }

    static void GenerateColorTexture2DMipmapByRecord(const MG_Pipe::MGPipeResourceRecord& record,
                                                     const SharedPtr<TextureImpl::BackendTextureObject>& backendTexture,
                                                     const RecordMipWindow& window) {
        const auto& desc = record.Desc;
        const GLenum filter =
            IsIntegerColorFormat(static_cast<TextureInternalFormat>(desc.InternalFormat)) ? GL_NEAREST : GL_LINEAR;
        const GLuint textureId = backendTexture->GetBackendTextureId();
        for (Uint32 level = window.Base + 1; level < window.End; ++level) {
            const IntVec3 srcSize =
                MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, level - 1);
            const IntVec3 dstSize =
                MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, level);
            BlitColorTexture2D(textureId, static_cast<GLint>(level - 1), 0, 0, static_cast<GLsizei>(srcSize.x()),
                               static_cast<GLsizei>(srcSize.y()), textureId, static_cast<GLint>(level), 0, 0,
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
        Uint activeTextureUnit = MG_Pipe::gPipeInputs->GetActiveTextureUnit();
        TextureImpl::BackendTextureObject* dstBackendTexture = nullptr;
        TextureInternalFormat mgInternalFormat{};
        // P5c (hd, CONTRACT-P5C §3.4): with an active transport the destination resolves from
        // the handle the copy_framebuffer_to_texture record carried (MGPCopyFromFramebuffer::
        // Dst, the verb stash) and its format is the applier descriptor's - the client's
        // texture-unit binding slot and the client slot allocator are never read (T2/T4).
        const MG_Pipe::MGPipeHandle dstHandle = MG_Pipe::MGPipeApplier().VerbCopyTexDst;
        const auto* dstRecord = PipeTextureRecordForHandle(dstHandle);
        // P5e (tx2): SYNCED, not merely looked up. The destination of a copy is not sampled
        // by the draw program, so the per-draw unit list - which is the sampler-view window
        // now - never brings it across; before P5e the frontend walk over every slot of every
        // touched unit happened to sync it as a side effect. The named narrowing (P5e-5) says
        // the union that still covers every texture includes "the waited texture ops", and
        // this is one of them: it syncs its own endpoint by handle.
        TextureImpl::NoteDriverSideTextureWriteByHandle(dstHandle);
        auto& backendTexture = TextureImpl::SyncTextureToBackendByHandle(dstHandle);
        if (!backendTexture || dstRecord == nullptr) {
            MGLOG_E_ONCE("CopyTexImage2D: the verb's destination texture {%u, %u} has no twin "
                         "or no applier record on this side",
                         dstHandle.Slot, dstHandle.Gen);
            return;
        }
        dstBackendTexture = backendTexture.get();
        mgInternalFormat = static_cast<TextureInternalFormat>(dstRecord->Desc.InternalFormat);
        dstBackendTexture->Bind(target, activeTextureUnit);

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

            auto currentTex = (GLint)dstBackendTexture->GetBackendTextureId();
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
        auto activeTextureUnit = MG_Pipe::gPipeInputs->GetActiveTextureUnit();
        TextureImpl::BackendTextureObject* dstBackendTexture = nullptr;
        // P5c (hd, CONTRACT-P5C §3.4): see CopyTexImage2D - the record's Dst handle, never the
        // client's unit binding slot or the client allocator (T2/T4).
        const MG_Pipe::MGPipeHandle dstHandle = MG_Pipe::MGPipeApplier().VerbCopyTexDst;
        // P5e (tx2): synced by handle - see CopyTexImage2D's note. A copy destination is not
        // in the sampler-view window, so nothing else on this path would build its twin.
        TextureImpl::NoteDriverSideTextureWriteByHandle(dstHandle);
        auto& backendTexture = TextureImpl::SyncTextureToBackendByHandle(dstHandle);
        if (!backendTexture) {
            MGLOG_E_ONCE("CopyTexSubImage2D: the verb's destination texture {%u, %u} has no "
                         "twin on this side",
                         dstHandle.Slot, dstHandle.Gen);
            return;
        }
        dstBackendTexture = backendTexture.get();
        dstBackendTexture->Bind(target, activeTextureUnit);

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
            auto currentTex = dstBackendTexture->GetBackendTextureId();
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

    // P8-B2: RGB16F / RGB32F MIP GENERATION ON THE SERVER, FROM THE SERVER'S OWN LEVEL.
    //
    // ES generates a chain only for a color-renderable, filterable format, and whether the
    // driver's storage for these two is one depends on the driver: Espryt stores them natively
    // where the driver renders the three-channel form and widens them to RGBA16F / RGBA32F where
    // it does not (TextureImpl::BackendTextureFormatAddsAlpha), and a widened RGBA32F still needs
    // OES_texture_float_linear to be filterable. So GenerateMipmapByRecord asks the driver first
    // and comes here only when it refuses - or when MGITEST_ESPRYT_FORCE_CPU_MIPMAP says to, which
    // is the only way a host whose driver never refuses reaches this arm.
    //
    // THE SOURCE IS THE SERVER'S LEVEL, NEVER THE CLIENT'S. The base level is read back from the
    // driver (ReadTextureLevelTight), because a render target's texels exist only there - the
    // monolith arm (GenerateThreeChannelFloatMipmapOnCpu) filters the frontend's level shadow
    // instead and reads a stale base after a draw. Only when the driver cannot read the level back
    // (storage it cannot attach) does the staged store answer, and a store that does not cover the
    // level declines by name rather than inventing texels.
    static Bool ForcedServerCpuMipmap() {
        // The R-16 control. Read by the SERVER, like the MGITEST_MAGMA_FORCE_* knobs.
        static const Bool s_forced = std::getenv("MGITEST_ESPRYT_FORCE_CPU_MIPMAP") != nullptr;
        return s_forced;
    }

    // The base level as tight RGB floats: the driver's texels first, the staged store second.
    //
    // P8-SE: THE STORE ANSWERS ONLY FOR A TEXTURE THE DRIVER NEVER WROTE (P8-E's sticky mark). A
    // render target's level 0 lives on the driver; the store still holds whatever crossed before
    // the draw, and filtering it would hand the chain stale texels. `driverWrittenBefore` is the
    // mark as it stood BEFORE this generation noted its own write (GenerateMipmap's split arm),
    // because that note is unconditional and would otherwise decline every store read. Anything
    // the store cannot stand for is a named decline, `declined` says why.
    static Bool ReadThreeChannelFloatLevelOnServer(MG_Pipe::MGPipeHandle mipRes,
                                                   const MG_Pipe::MGPipeResourceRecord& record,
                                                   const SharedPtr<TextureImpl::BackendTextureObject>& backendTexture,
                                                   TextureTarget textureTarget, TextureUploadTarget face, Uint32 level,
                                                   const IntVec3& extent, Bool driverWrittenBefore, Vector<Float>& rgb,
                                                   const char*& declined) {
        const auto format = static_cast<TextureInternalFormat>(record.Desc.InternalFormat);
        const SizeT texels = static_cast<SizeT>(extent.x()) * static_cast<SizeT>(extent.y());
        Vector<Uint8> bytes;
        if (!WireTextureReadbackRefusedForTesting(extent) &&
            ReadTextureLevelTight(backendTexture->GetBackendTextureId(), textureTarget, face, format,
                                  static_cast<GLint>(level), extent, backendTexture->RequiresImageBindableStorage(),
                                  GL_RGB, GL_FLOAT, bytes) &&
            bytes.size() == texels * 3 * sizeof(Float)) {
            rgb.resize(texels * 3);
            Memcpy(rgb.data(), bytes.data(), bytes.size());
            return true;
        }
        auto& store = MG_Record::ServerStagedTexture();
        const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(mipRes);
        const auto faceCode = static_cast<Uint16>(face);
        const auto levelCode = static_cast<Uint16>(level);
        const Bool half = format == TextureInternalFormat::RGB16F;
        const SizeT componentBytes = half ? sizeof(Uint16) : sizeof(Float);
        // A view's staged bytes live under its storage owner's key, in the owner's levels; a view
        // whose level the driver cannot read has no answer here.
        if (!MG_Pipe::MGPipeHandleIsNull(record.Desc.ViewOf)) {
            declined = "a view has no staged level of its own";
        } else if (!store.Holds()) {
            declined = "this arm keeps no staged store";
        } else if (driverWrittenBefore) {
            declined = "the driver has written the texture since its bytes crossed";
        } else if (!store.IsCovered(key, faceCode, levelCode)) {
            declined = "not every byte of the level crossed";
        } else if (store.LevelByteSize(key, faceCode, levelCode) != texels * 3 * componentBytes) {
            declined = "the staged level is not the size of the level";
        }
        if (declined != nullptr) return false;
        const Uint8* staged = store.RequireLevelBytes(key, faceCode, levelCode, "generate-mipmap-server-cpu");
        rgb.resize(texels * 3);
        for (SizeT i = 0; i < texels * 3; ++i) {
            if (half) {
                Uint16 bits = 0;
                Memcpy(&bits, staged + i * componentBytes, sizeof(bits));
                rgb[i] = MG_Util::DecodeHalfBitsToFloat(bits);
            } else {
                Memcpy(&rgb[i], staged + i * componentBytes, sizeof(Float));
            }
        }
        MGLOG_W_ONCE("MGPipe: generate-mipmap-server-cpu: texture {%u, %u} level %u could not be read back from "
                     "the driver; filtering the staged store's copy of it instead",
                     mipRes.Slot, mipRes.Gen, level);
        return true;
    }

    static void GenerateThreeChannelFloatMipmapOnServer(
        MG_Pipe::MGPipeHandle mipRes, const MG_Pipe::MGPipeResourceRecord& record,
        const SharedPtr<TextureImpl::BackendTextureObject>& backendTexture, Bool driverWrittenBefore, const char* why) {
        const auto& desc = record.Desc;
        const auto format = static_cast<TextureInternalFormat>(desc.InternalFormat);
        const auto textureTarget = TextureTargetForPipeResourceTarget(desc.Target);
        const RecordMipWindow window = GenerateMipmapWindowByRecord(record);
        if (window.End <= window.Base + 1) return;
        Vector<TextureUploadTarget> faces;
        if (textureTarget == TextureTarget::Texture2D) {
            faces.push_back(TextureUploadTarget::Texture2D);
        } else if (textureTarget == TextureTarget::TextureCubeMap) {
            for (Int face = 0; face < 6; ++face)
                faces.push_back(
                    static_cast<TextureUploadTarget>(static_cast<Int>(TextureUploadTarget::CubeMapPositiveX) + face));
        }
        // The upload spelling the driver's storage takes: GL_FLOAT is legal for all four float
        // internal formats (ES 3.2 table 8.2), and the channel count follows the storage.
        GLenum esInternal = GL_NONE, esFormat = GL_NONE, esType = GL_NONE;
        TextureImpl::GenerateTextureFormatInfo(format, &esInternal, &esFormat, &esType, textureTarget);
        const Bool fourChannel = esInternal == GL_RGBA16F || esInternal == GL_RGBA32F;
        if (faces.empty() || (!fourChannel && esInternal != GL_RGB16F && esInternal != GL_RGB32F)) {
            MGLOG_E_ONCE("MGPipe: generate-mipmap-server-cpu declines texture {%u, %u}: target %s stored as %s has "
                         "no server CPU arm (%s); levels %u..%u are left as they are",
                         mipRes.Slot, mipRes.Gen, MG_Util::ConvertTextureTargetToString(textureTarget).c_str(),
                         MG_Util::ConvertGLEnumToString(esInternal).c_str(), why, window.Base + 1, window.End - 1);
            return;
        }
        MGLOG_I_ONCE("MGPipe: generate-mipmap-server-cpu: %s levels filtered on the server's CPU from its own "
                     "level (%s)",
                     MG_Util::ConvertTextureInternalFormatToString(format).c_str(), why);

        const GLuint textureId = backendTexture->GetBackendTextureId();
        const GLenum bindTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(textureTarget);
        for (const TextureUploadTarget face : faces) {
            IntVec3 extent =
                MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, window.Base);
            Vector<Float> level;
            const char* declined = nullptr;
            if (!ReadThreeChannelFloatLevelOnServer(mipRes, record, backendTexture, textureTarget, face, window.Base,
                                                    extent, driverWrittenBefore, level, declined)) {
                MGLOG_E_ONCE("MGPipe: generate-mipmap-server-cpu declines texture {%u, %u}: base level %u could not "
                             "be read back from the driver and the staged store cannot stand for it (%s); levels "
                             "%u..%u are left as they are - DECLINED (generate-mipmap-store-declined)",
                             mipRes.Slot, mipRes.Gen, window.Base, declined != nullptr ? declined : "?",
                             window.Base + 1, window.End - 1);
                return;
            }
            for (Uint32 dst = window.Base + 1; dst < window.End; ++dst) {
                const IntVec3 dstExtent =
                    MG_Record::StagedTextureMipExtent(desc.Target, desc.Width, desc.Height, desc.Depth, dst);
                // The monolith's box filter: the 2x2 source footprint, clamped where a dimension
                // is already 1 (GL 4.6 core 8.14.4 leaves the filter to the implementation).
                Vector<Float> next(static_cast<SizeT>(dstExtent.x()) * static_cast<SizeT>(dstExtent.y()) * 3);
                const auto at = [&](Int x, Int y, Int c) {
                    return level[(static_cast<SizeT>(y) * static_cast<SizeT>(extent.x()) + static_cast<SizeT>(x)) * 3 +
                                 static_cast<SizeT>(c)];
                };
                for (Int y = 0; y < dstExtent.y(); ++y) {
                    for (Int x = 0; x < dstExtent.x(); ++x) {
                        const Int x0 = std::min(x * 2, extent.x() - 1), x1 = std::min(x * 2 + 1, extent.x() - 1);
                        const Int y0 = std::min(y * 2, extent.y() - 1), y1 = std::min(y * 2 + 1, extent.y() - 1);
                        for (Int c = 0; c < 3; ++c)
                            next[(static_cast<SizeT>(y) * static_cast<SizeT>(dstExtent.x()) + static_cast<SizeT>(x)) *
                                     3 +
                                 static_cast<SizeT>(c)] =
                                0.25f * (at(x0, y0, c) + at(x1, y0, c) + at(x0, y1, c) + at(x1, y1, c));
                    }
                }
                Vector<Float> upload;
                if (fourChannel) {
                    const SizeT count = next.size() / 3;
                    upload.resize(count * 4);
                    for (SizeT i = 0; i < count; ++i) {
                        upload[i * 4] = next[i * 3];
                        upload[i * 4 + 1] = next[i * 3 + 1];
                        upload[i * 4 + 2] = next[i * 3 + 2];
                        upload[i * 4 + 3] = 1.0f;
                    }
                }
                // Tight float rows are a multiple of four bytes, which the resting UNPACK state
                // (4/0/0/0, and the unpack-PBO binding at 0) parses as they are - CopyR32FTexture2D's
                // discipline, including the re-bind that keeps the binding cache truthful.
                ClearGLErrors();
                BufferImpl::BindPixelUnpackBufferId(0);
                TextureImpl::ActivateTextureUnit(TextureImpl::TempTextureUnit);
                g_GLESFuncs.glBindTexture(bindTarget, textureId);
                g_GLESFuncs.glTexSubImage2D(TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(face),
                                            static_cast<GLint>(dst), 0, 0, static_cast<GLsizei>(dstExtent.x()),
                                            static_cast<GLsizei>(dstExtent.y()), fourChannel ? GL_RGBA : GL_RGB,
                                            GL_FLOAT, fourChannel ? upload.data() : next.data());
                const GLenum err = g_GLESFuncs.glGetError();
                auto* cachedBound =
                    TextureImpl::g_boundTexturesCache[TextureImpl::TempTextureUnit][static_cast<SizeT>(textureTarget)];
                g_GLESFuncs.glBindTexture(bindTarget, cachedBound ? cachedBound->GetBackendTextureId() : 0);
                if (err != GL_NO_ERROR) {
                    MGLOG_E_ONCE("MGPipe: generate-mipmap-server-cpu: uploading level %u of texture {%u, %u} "
                                 "failed: %s",
                                 dst, mipRes.Slot, mipRes.Gen, MG_Util::ConvertGLEnumToString(err).c_str());
                    return;
                }
                level = Move(next);
                extent = dstExtent;
            }
        }
    }

    // P5e (tx2), CONTRACT-P5E §5.2: glGenerateMipmap's body once the texture came from
    // VerbMipRes. Same four arms as the frontend body, with every shape question answered from
    // the descriptor.
    //
    // RULING 14's TRAILING FLIP. tx2 kept this verb's PipeCalls.def row at kWaitApplied because
    // one frontend contact was left on the path: the RGB16F/RGB32F CPU filter reached into the
    // client's level shadows. P8-B2 retired it (GenerateThreeChannelFloatMipmapOnServer reads the
    // server's own level), so no arm here reads client memory any more; the WaitClass flip itself
    // is a protocol change and is left to the integrator.
    //
    // `driverWrittenBefore`: P8-E's driver-written mark on the texture's staged store as it stood
    // before this call noted its own write - the CPU arm's store route reads it (P8-SE).
    static void GenerateMipmapByRecord(GLenum target, MG_Pipe::MGPipeHandle mipRes,
                                       const MG_Pipe::MGPipeResourceRecord& record,
                                       const SharedPtr<TextureImpl::BackendTextureObject>& backendTexture,
                                       Bool driverWrittenBefore) {
        const auto format = static_cast<TextureInternalFormat>(record.Desc.InternalFormat);
        const auto textureTarget = TextureTargetForPipeResourceTarget(record.Desc.Target);
        const Bool threeChannelFloat =
            format == TextureInternalFormat::RGB16F || format == TextureInternalFormat::RGB32F;

        // A view's layer window is the one bound the native arm cannot express, so it is asked
        // for before the format arms - those two are 2D-only emulations of the same generation
        // and a view reaches them too.
        if (GenerateMipmapThroughViewWindowByRecord(mipRes, record, format)) return;
        if (IsDepthOnlyFormat(format)) {
            GenerateDepthTexture2DMipmapByRecord(record, backendTexture, GenerateMipmapWindowByRecord(record));
            return;
        }
        if (format == TextureInternalFormat::R11FG11FB10F && textureTarget == TextureTarget::Texture2D) {
            GenerateColorTexture2DMipmapByRecord(record, backendTexture, GenerateMipmapWindowByRecord(record));
            return;
        }
        if (threeChannelFloat && ForcedServerCpuMipmap()) {
            GenerateThreeChannelFloatMipmapOnServer(mipRes, record, backendTexture, driverWrittenBefore,
                                                    "forced by MGITEST_ESPRYT_FORCE_CPU_MIPMAP");
            return;
        }

        const GLenum backendTarget =
            TextureImpl::ConvertTextureTargetToBackendGLEnum(MG_Util::ConvertGLEnumToTextureTarget(target));
        GLenum refused = GL_NO_ERROR;
        {
            // TempTextureUnit, not the active unit: the scratch bind is the backend's own and
            // BindCurrentTextures re-establishes the sampling bindings regardless. The ACTIVE unit
            // was never anything but the frontend's way of naming the texture, and the record
            // names it now.
            backendTexture->Bind(backendTarget, TextureImpl::TempTextureUnit);
            // ANGLE and some other drivers may validate the currently bound FBO while generating mipmaps, so the
            // source texture is detached from every framebuffer that attaches it - by handle,
            // over the applier's own framebuffer records.
            ScopedDetachedTextureFramebufferAttachments detachedAttachments(mipRes);
            ScopedCompleteFramebufferBinding completeFramebuffer;
            ClearGLErrors();
            g_GLESFuncs.glGenerateMipmap(backendTarget);
            if (!threeChannelFloat) {
                RecordGLError("glGenerateMipmap", backendTarget, format);
                return;
            }
            // The driver's refusal of a three-channel float chain is not the application's
            // error: the CPU arm below answers it.
            refused = g_GLESFuncs.glGetError();
        }
        if (refused == GL_NO_ERROR) return;
        GenerateThreeChannelFloatMipmapOnServer(mipRes, record, backendTexture, driverWrittenBefore,
                                                "the driver refused glGenerateMipmap");
    }

    void PatchParameteri(GLenum pname, GLint value) {
        if (g_GLESFuncs.glPatchParameteri == nullptr) return;
        g_GLESFuncs.glPatchParameteri(pname, value);
    }

    void GenerateMipmap(GLenum target) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        // P5e (tx2), CONTRACT-P5E §5.2 / ruling 14 (ID-92's neighbor): GENERATE_MIPMAP RESOLVES
        // ITS TEXTURE FROM VerbMipRes, NEVER FROM THE ACTIVE UNIT.
        //
        // The verb's own record names the texture (MGPMipPlan::Res, stashed as VerbMipRes) and
        // EnsureGenerateMipmapStorageAllocated's disaggregated arm has read it since P5c. What
        // stood here instead was GetActiveTextureUnit + GetTextureUnitObject + the target's
        // binding slot - three BARRIER_PULLED reads to re-derive a handle the record carried -
        // and they are the ONLY reason this row still waits: ClientSession.cpp parks the client
        // so that the unit it reads is the unit the call was made on. With the unit read gone the
        // wait has nothing left to protect, which is what lets PipeCalls.def's WaitClass column
        // for GenerateMipmap move to kWaitNone.
        const auto mipRes = MG_Pipe::MGPipeApplier().VerbMipRes;
        // P8-SE: whether the staged store still stands for the texture is read BEFORE the note
        // below sets the mark, or the CPU arm's store route could never answer at all.
        const Bool driverWrittenBefore =
            MG_Record::ServerStagedTexture().IsDriverWritten(MG_Record::StagedTextureStore::KeyForHandle(mipRes));
        // Every arm GenerateMipmapByRecord takes writes the levels above the base on the
        // driver only (P8-B2's CPU arm uploads its levels there too), so the note is
        // unconditional.
        TextureImpl::NoteDriverSideTextureWriteByHandle(mipRes);
        auto& backendTexture = TextureImpl::SyncTextureToBackendByHandle(mipRes);
        const auto* record = PipeTextureRecordForHandle(mipRes);
        if (!backendTexture || record == nullptr) {
            MGLOG_E_ONCE("MGPipe: generate_mipmap names texture {%u, %u}, which has no applier "
                         "record or no driver texture; the call is dropped",
                         mipRes.Slot, mipRes.Gen);
            return;
        }
        GenerateMipmapByRecord(target, mipRes, *record, backendTexture, driverWrittenBefore);
        return;
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
        barriers = LowerAtomicCounterBarrierBits(barriers);
        if ((barriers & GL_SHADER_STORAGE_BARRIER_BIT) != 0) BufferImpl::NoteShaderStorageWritesOrdered();
        g_GLESFuncs.glMemoryBarrier(barriers);
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

    static Bool MakeGLESCopyImageEndpoint(const CopyImageEndpoint& endpoint, GLenum appTarget, GLint x, GLint y,
                                          GLint z, GLESCopyImageEndpoint& out) {
        if (endpoint.IsRenderbuffer()) {
            if (!PipeRenderbufferRecordForHandle(endpoint.RenderbufferHandle)) return false;
            auto* slot =
                RenderbufferImpl::g_backendRenderbufferObjects.GetOrCreateByHandle(endpoint.RenderbufferHandle);
            if (!slot) return false;
            if (!*slot) *slot = MakeShared<RenderbufferImpl::BackendRenderbufferObject>();
            out.renderbuffer = *slot;
            out.renderbuffer->SyncToBackendByHandle(endpoint.RenderbufferHandle);
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
        out.texture = TextureImpl::SyncTextureToBackendByHandle(endpoint.TextureHandle);
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
        const auto* record = endpoint.IsRenderbuffer() ? PipeRenderbufferRecordForHandle(endpoint.RenderbufferHandle)
                                                       : PipeTextureRecordForHandle(endpoint.TextureHandle);
        return record ? static_cast<TextureInternalFormat>(record->Desc.InternalFormat)
                      : TextureInternalFormat::Unknown;
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
                                                     GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei width,
                                                     GLsizei height, GLsizei depth) {
        if (!CanMirrorCopyImageShadow(srcEndpoint.Texture) || !CanMirrorCopyImageShadow(dstEndpoint.Texture)) return;
        if (srcEndpoint.Texture == dstEndpoint.Texture && srcLevel == dstLevel) return;
        if (width <= 0 || height <= 0 || depth <= 0) return;
        if (srcLevel < 0 || dstLevel < 0 || srcX < 0 || srcY < 0 || srcZ < 0 || dstX < 0 || dstY < 0 || dstZ < 0) {
            return;
        }
        auto* srcMipmap = MG_State::GLState::AsMipmapTexture(srcEndpoint.Texture.get());
        auto* dstMipmap = MG_State::GLState::AsMipmapTexture(dstEndpoint.Texture.get());
        if (!srcMipmap || !dstMipmap) return;
        // UNDER A REAL TRANSPORT THE SERVER HAS NO FRONTEND SHADOW TO MIRROR INTO (P5b i1, ruling
        // §6.7), and P8-E settled where the move lives instead - not on the client (ROADMAP P8's
        // original row) but in the server's staged store: CopyImageSubData's handle arm makes the
        // same texel-block move there (TextureImpl::FollowCopyImageInStagedStore) for EVERY
        // format, not only the redundant-encoding ones this mirror serves, and marks the store
        // driver-written where it cannot. The store is what answers a split glGetTexImage the
        // driver refuses to read back (DirectGLESReadback.cpp's store route); every other split
        // read of the destination reads the GPU level itself.
        //
        // BEHIND #if MOBILEGL_BUILD_DISAGGREGATED so the pull build's code does not move (G1),
        // and the arm is the TRANSPORT and not the build - build-split runs its unit and
        // integration-gpu lanes under MOBILEGL_TRANSPORT=monolith, where this mirror is on an
        // ordinary correct path and must still run.
        return;
    }

    void CopyImageSubData(const CopyImageEndpoint& srcEndpoint, GLenum srcTarget, GLint srcLevel, GLint srcX,
                          GLint srcY, GLint srcZ, const CopyImageEndpoint& dstEndpoint, GLenum dstTarget,
                          GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight,
                          GLsizei srcDepth) {
        GLESCopyImageEndpoint src{};
        GLESCopyImageEndpoint dst{};
        // The DirectVulkan half of this entry point died exactly here, on a texture whose sync
        // produced nothing - and it died in a release build, where the MOBILEGL_ASSERT that was
        // supposed to catch it expands to nothing. The four Name() calls below are the same
        // dereference. The frontend validator is what keeps this unreachable and what reports
        // the error the application is owed; declining is only how a future gap up there stops
        // being a crash. See the level guard in VulkanRenderer::CopyImageSubData.
        // The handle arm's endpoint names its texture by handle, and the server has no frontend
        // object to note. The staged store's half of the note waits for the copy (P8-E, below).
        const Bool handleArmDestination =
            !dstEndpoint.IsRenderbuffer() && !MG_Pipe::MGPipeHandleIsNull(dstEndpoint.TextureHandle);
        if (handleArmDestination) {
            TextureImpl::NoteDriverSideTextureWriteByHandle(dstEndpoint.TextureHandle, /*storeFollowsTheWrite=*/true);
        }
        if (!MakeGLESCopyImageEndpoint(srcEndpoint, srcTarget, srcX, srcY, srcZ, src) ||
            !MakeGLESCopyImageEndpoint(dstEndpoint, dstTarget, dstX, dstY, dstZ, dst)) {
            MGLOG_E_ONCE("%s: source or destination image failed to sync; declining the copy", __func__);
            return;
        }
        // P8-E: THE SERVER'S STAGED STORE FOLLOWS THE COPY. Both endpoints are synced,
        // so every byte the store holds for them is on the driver, and the copy below is a raw
        // texel-block move - the store makes the same move, which keeps the destination level
        // answerable from here when the driver will not read it back (ReadTextureImageWire's store
        // route) and keeps a whole-level replay of it (a re-mint's refused readback, a full-level
        // re-upload) from putting the pre-copy texels back. Where the store cannot follow (a
        // renderbuffer, a cube or 1D-array endpoint, a source the driver has written, a level whose
        // bytes did not all cross) it is marked driver-written instead, so it declines rather than
        // answering with stale bytes. Nothing moves to the client (ROADMAP P8's original row).
        if (handleArmDestination) {
            const Bool followed =
                !srcEndpoint.IsRenderbuffer() && !MG_Pipe::MGPipeHandleIsNull(srcEndpoint.TextureHandle) &&
                TextureImpl::FollowCopyImageInStagedStore(srcEndpoint.TextureHandle, static_cast<Uint32>(srcLevel),
                                                          IntVec3{srcX, srcY, srcZ}, dstEndpoint.TextureHandle,
                                                          static_cast<Uint32>(dstLevel), IntVec3{dstX, dstY, dstZ},
                                                          IntVec3{srcWidth, srcHeight, srcDepth});
            if (!followed) TextureImpl::NoteDriverSideTextureWriteByHandle(dstEndpoint.TextureHandle);
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
            BlitDepthTexture2D(src.Name(), srcLevel, src.x, src.y, srcWidth, copyHeight, dst.Name(), dstLevel, dst.x,
                               dst.y, srcWidth, copyHeight);
            return;
        }

        if (!anyRenderbuffer &&
            (srcFormat == TextureInternalFormat::R32F || dstFormat == TextureInternalFormat::R32F)) {
            // The single glGetError below decides the fallback dispatch, and
            // ErrorLopper::Clear is compiled out at the default log level - drain
            // with the always-live helper so a stale flag cannot misroute a
            // succeeded native copy into the 2D-only fallback.
            ClearGLErrors();
            g_GLESFuncs.glCopyImageSubData(src.Name(), src.target, srcLevel, src.x, src.y, src.z, dst.Name(),
                                           dst.target, dstLevel, dst.x, dst.y, dst.z, srcWidth, copyHeight, copyDepth);
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
            CopyR32FTexture2D(src.Name(), srcLevel, src.x, src.y, srcWidth, copyHeight, dst.Name(), dst.target,
                              dstLevel, dst.x, dst.y);
            return;
        }

        ClearGLErrors();
        g_GLESFuncs.glCopyImageSubData(src.Name(), src.target, srcLevel, src.x, src.y, src.z, dst.Name(), dst.target,
                                       dstLevel, dst.x, dst.y, dst.z, srcWidth, copyHeight, copyDepth);
        // Every error condition glCopyImageSubData has was already ruled out by the frontend
        // validator, so a driver error here is an internal invariant violation, not something
        // the application can provoke. Say so where an INFO build can still see it, then trap
        // in the builds that trap - the previous bare assert left a release build with a
        // destination that silently kept its old contents.
        const GLenum copyImageError = g_GLESFuncs.glGetError();
        if (copyImageError != GL_NO_ERROR) {
            MGLOG_E_ONCE(
                "glCopyImageSubData failed: %s. src target=%s (app %s), dst target=%s (app %s)",
                MG_Util::ConvertGLEnumToString(copyImageError).c_str(),
                MG_Util::ConvertGLEnumToString(src.target).c_str(), MG_Util::ConvertGLEnumToString(srcTarget).c_str(),
                MG_Util::ConvertGLEnumToString(dst.target).c_str(), MG_Util::ConvertGLEnumToString(dstTarget).c_str());
            MOBILEGL_ASSERT(false, "glCopyImageSubData failed after frontend validation accepted the request.");
            // P8-E: the store already made the move the driver just refused.
            if (handleArmDestination) {
                TextureImpl::NoteDriverSideTextureWriteByHandle(dstEndpoint.TextureHandle);
            }
            return;
        }
        // The copy landed on the GPU. For a destination whose readback cannot be bit-exact the
        // CPU shadow is what glGetTexImage answers from, so it has to follow the same move -
        // otherwise it hands back whatever the level held before this copy.
        if (MG_Util::PixelStoreProcessor::HasRedundantPackedEncoding(dstFormat)) {
            MirrorCopyImageIntoDestinationShadow(srcEndpoint, srcLevel, srcX, srcY, srcZ, dstEndpoint, dstLevel, dstX,
                                                 dstY, dstZ, srcWidth, srcHeight, srcDepth);
        }
    }

    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format) {
        (void)level;
        (void)layered;
        (void)layer;
        (void)access;
        (void)format;
        // P5e (fb): see NoteImageUnitBoundWithoutReadingTheFrontend. Under a transport this
        // entry point records the high-water mark and lets the next validate point's sweep do
        // the bind from the record; reading the frontend image binding here is the
        // GetImageTextureBinding row that rule F forbids, and `bind_shader_image` is not a
        // barriered row.
        TextureImpl::NoteImageUnitBoundWithoutReadingTheFrontend(unit, texture != 0);
        return;
    }

    // Only the pnames MG_Impl/GLImpl/Getter/GL_Getter.cpp has no case for reach here. Every
    // indexed pname naming FRONTEND state - the indexed buffer bindings, the per-unit
    // texture/sampler bindings, the image-unit bindings, the viewport rectangles, the indexed
    // capabilities - is answered there and returns before the table is consulted, so the arms
    // this function used to carry for GL_SHADER_STORAGE_BUFFER_* and GL_IMAGE_BINDING_* were
    // unreachable duplicates of the frontend's, and they did not even agree with it (the
    // frontend reports the range glBindBufferRange was ASKED for, verbatim and unclamped; these
    // clamped it to the buffer's current storage). In practice what arrives is
    // GL_MAX_COMPUTE_WORK_GROUP_COUNT / _SIZE, which the driver owns.
    void GetIntegeri_v(GLenum target, GLuint index, GLint* data) {
        if (!data) return;
        if (g_GLESFuncs.glGetIntegeri_v) {
            g_GLESFuncs.glGetIntegeri_v(target, index, data);
        } else {
            *data = 0;
        }
    }

    // NOTE the shape here, and do not "simplify" it back to GetBackendProgramId(): this entry
    // point must never be the thing that BUILDS a backend program.
    //
    // The frontend has already recorded the rebinding on the program object
    // (SetShaderStorageBlockBinding) - that record is what GL_BUFFER_BINDING reports and what
    // BackendProgramObjectImpl::SyncToBackend replays onto every driver program it builds. So
    // the only work left here is an optimization: push the change straight onto a driver
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
        const auto handle = MG_Pipe::MGPipeApplier().VerbStorageBlockProgram;
        auto* slot = PrgramImpl::g_backendProgramObjects.FindByHandle(handle);
        if (slot && *slot && (*slot)->GetBackendProgramId()) {
            PrgramImpl::ApplyShaderStorageBlockBinding((*slot)->GetBackendProgramId(), storageBlockName,
                                                       storageBlockBinding);
        }
        return;
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        g_GLESFuncs.glClearBufferfi(buffer, drawbuffer, depth, stencil);
    }

    namespace {
        // A color attachment the backend widened from three channels to four has to end up
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
} // namespace MobileGL::MG_Backend::DirectGLES
