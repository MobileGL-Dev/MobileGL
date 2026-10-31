// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESBlit.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace FramebufferImpl {
        // Record that `target` now reflects the applier's record as of `serial`. The two
        // generations ride along for the reason D-B3 keeps them: they answer questions about
        // the DRIVER's own ids that no client-side version can answer.
        // The stamp's opposite, for a path that leaves a target correctly bound WITHOUT having
        // consulted the record (MINOR-3, ForceBindCurrentFBO). Costs one extra sync at worst.
        static void InvalidateSyncedFramebufferSerial(FramebufferTarget target) {
            g_fboSyncedSerials[SizeT(target)].valid = false;
        }
    } // namespace FramebufferImpl

    // P5c (hd, CONTRACT-P5C §3.2/§3.3): the handle-keyed form of SyncAndBindFramebufferObject.
    // With an active transport a framebuffer reaches the server as the handle a record
    // carried - the named blit's ReadFbo/DrawFbo, or the applier's own bound handle for a
    // binding restore - and the client's binding slots and slot allocator are never probed
    // (T2/T3). The twin is resolved (or minted) BY HANDLE; its sync is keyed on THIS handle's
    // applier record through m_pushedSyncHandle; and the frontend object the sync body still
    // walks comes from the table's own state note (the object-class channel P3b/P4b retires),
    // never from a client binding slot. A twin with no noted object was never synced through
    // any binding - the DSA-on-a-never-bound-framebuffer case, which the split path already
    // refuses for the named clears - and is loud rather than silently unconfigured.
    void SyncAndBindFramebufferByHandle(MG_Pipe::MGPipeHandle fbo, FramebufferTarget target, Bool forceSync = false) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        if (MG_Pipe::MGPipeHandleIsNull(fbo) || fbo == MG_Pipe::kMGPipeDefaultFramebuffer) {
            // Same reset as the object form's default branch, for its reason.
            if (target == FramebufferTarget::Draw) {
                FramebufferImpl::g_alphaWidenedDrawBufferMask = 0;
                FramebufferImpl::g_integerColorDrawBufferMask = 0;
            }
            if (FramebufferImpl::CurrentFramebufferBinding(target) != 0) FlushAheadOfWindowBufferUse();
            FramebufferImpl::BindFramebufferId(
                target == FramebufferTarget::Draw ? GL_DRAW_FRAMEBUFFER : GL_READ_FRAMEBUFFER, 0);
            return;
        }

        auto& registry = FramebufferImpl::g_backendFramebufferObjects;
        auto* twinSlot = registry.GetOrCreateByHandle(fbo);
        if (twinSlot == nullptr) {
            MGLOG_E_ONCE("MGPipe: framebuffer handle {%u, %u} is refused by the twin slot table "
                         "(live generation %u); nothing is bound for the %s target",
                         fbo.Slot, fbo.Gen, registry.LiveGenAt(fbo.Slot),
                         target == FramebufferTarget::Read ? "READ" : "DRAW");
            return;
        }
        auto& backendObj = *twinSlot;
        if (!backendObj) {
            backendObj = MakeShared<FramebufferImpl::BackendFramebufferObject>();
        }
        backendObj->m_pushedSyncHandle = fbo;
        // P5e (fb, CONTRACT-P5E.md §5.4): THE STATE NOTE IS GONE. The paragraph above described
        // a handle arm that still had to find "the frontend object the sync body walks", and
        // its failure mode - a framebuffer named by a DSA entry point that was never bound, so
        // no note exists and the twin is bound unconfigured - was the last consequence of that
        // detour. SyncToBackendByHandle configures the twin from the RECORD, which every named
        // framebuffer has by construction (a Named record precedes every DSA site, ID-19), so
        // the never-bound case is now configured correctly rather than named and refused.
        if (forceSync) {
            backendObj->InvalidateSyncedState();
        }
        backendObj->SyncToBackendByHandle(fbo, target);
        backendObj->Bind(target);
    }

    void ForceBindCurrentFBO(FramebufferTarget target) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        // P5c (hd): with an active transport the bound framebuffer's handle is the applier's
        // own working state; the object form's registry probe never runs (T2).
        //
        // P5e (fb, CONTRACT-P5E.md §5.4): AND THE BINDING SLOT IS READ INSIDE THE ELSE, NOT
        // BEFORE THE IF. The read and the three stamps below used to straddle this arm - S3's
        // finding #7 - so the handle path still touched a BARRIER_PULLED row on both sides of
        // the one branch that existed to avoid it. The stamps are the LEGACY memo's
        // (g_fboSynced*), and skipping them here is safe for exactly the reason the serial memo
        // below is invalidated rather than stamped: nothing on this path consulted the frontend
        // trio, so the honest thing is to leave it invalid and pay one extra sync. Under a
        // transport the legacy arm is never taken anyway (SyncCurrentFBO refuses first), so the
        // memo it feeds has no reader.
        SyncAndBindFramebufferByHandle(MG_Pipe::MGPipeApplier().BoundFramebuffer[static_cast<SizeT>(
                                           target == FramebufferTarget::Read ? MG_Pipe::MGPipeFramebufferTarget::Read
                                                                             : MG_Pipe::MGPipeFramebufferTarget::Draw)],
                                       target);
        // P4a decline-site F9 / MINOR-3: THE HANDLE ARM'S MEMO IS INVALIDATED HERE, NOT
        //   STAMPED, and it is done per target for the reason the memo is kept per target at
        //   all - this entry point syncs ONE binding and must not say anything about the other.
        //
        //   A stamp would claim "this target reflects the applier's RECORD as of this serial",
        //   and nothing on this path consulted the record: SyncAndBindFramebufferObject above
        //   syncs the FRONTEND object out of the binding slot. I could not turn the stamp into
        //   a wrong-pixel scenario (any edit to the framebuffer moves its ContentHash and hence
        //   the serial), but it is an invariant break with no demonstrable failure, and the
        //   honest form costs at most one extra sync on the next draw.
        FramebufferImpl::InvalidateSyncedFramebufferSerial(target);
    }

    // Sized internal format of the currently bound READ framebuffer's read color
    // attachment, 0 when it cannot be determined.
    static GLenum QueryReadColorAttachmentInternalFormat() {
        GLint attachmentType = 0;
        GLint attachmentName = 0;
        // Ask the point the backend read buffer actually names, not COLOR_ATTACHMENT0. The color map
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
            g_GLESFuncs.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &internalFormat);
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
        static NativeContextName s_resolveFramebuffer; // per context: a framebuffer is never shared
        static Uint s_resolveRenderbuffer = 0;
        static GLenum s_resolveFormat = 0;
        static GLsizei s_resolveWidth = 0;
        static GLsizei s_resolveHeight = 0;
        static Uint s_resolveContextGeneration = ~0u;

        if (!g_GLESFuncs.glGenFramebuffers || !g_GLESFuncs.glGenRenderbuffers || !g_GLESFuncs.glRenderbufferStorage ||
            !g_GLESFuncs.glFramebufferRenderbuffer) {
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
            if (s_resolveFramebuffer == 0) return false;
        }
        if (s_resolveRenderbuffer == 0) {
            g_GLESFuncs.glGenRenderbuffers(1, &s_resolveRenderbuffer);
            if (s_resolveRenderbuffer == 0) return false;
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
            const Bool scissorWasEnabled = RenderStateImpl::DriverScissorTestEnabled();
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
        FramebufferImpl::InvalidateFramebufferHandleArmMemos();
        if (!resolved) {
            MGLOG_E_ONCE("BlitFramebuffer: multisample resolve fallback failed");
        }
        return resolved;
    }

    namespace ReplicateBlitImpl {
        static Uint s_contextGeneration = ~0u;
        static NativeContextName s_framebuffer; // per context, as is the vertex array below
        static GLuint s_texture = 0;
        static GLenum s_textureFormat = 0;
        static GLsizei s_textureWidth = 0;
        static GLsizei s_textureHeight = 0;
        static NativeContextName s_vertexArray;
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

        static const char* const kDepthFragmentSource = "#version 300 es\n"
                                                        "precision highp float;\n"
                                                        "precision highp sampler2D;\n"
                                                        "uniform sampler2D uSource;\n"
                                                        "in vec2 vUv;\n"
                                                        "void main() {\n"
                                                        "    gl_FragDepth = texture(uSource, vUv).r;\n"
                                                        "}\n";

        static const char* const kStencilFragmentSource = "#version 300 es\n"
                                                          "precision highp float;\n"
                                                          "precision highp usampler2D;\n"
                                                          "uniform usampler2D uSource;\n"
                                                          "uniform uint uBit;\n"
                                                          "in vec2 vUv;\n"
                                                          "void main() {\n"
                                                          "    if ((texture(uSource, vUv).r & uBit) == 0u) discard;\n"
                                                          "}\n";

        GLuint BuildProgram(const char* fragmentSource) {
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
        GLenum QueryAttachmentSizedFormat(GLenum attachment) {
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
                    g_GLESFuncs.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
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
        const Bool supportsStencilTextureMode =
            g_GLESCapabilities.GLESVersion.Major > 3 ||
            (g_GLESCapabilities.GLESVersion.Major == 3 && g_GLESCapabilities.GLESVersion.Minor >= 1);
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
        FramebufferImpl::InvalidateFramebufferHandleArmMemos();
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
        // choices - the destination rectangle, and color writes off because it writes only
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
            g_GLESFuncs.glUniform4f(s_depthUvTransform, uvTransform[0], uvTransform[1], uvTransform[2], uvTransform[3]);
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

    // ---- glBlitFramebuffer within one image ------------------------------------------------
    //
    // GL 4.6 lets a blit read and write the same image as long as the two rectangles do not
    // overlap (only the overlap is undefined). ES 3.x makes identical read and draw buffers
    // GL_INVALID_OPERATION outright, so the driver refuses a blit from one half of a texture
    // into the other and nothing is written. Emulate it in two steps: copy the source rectangle
    // into a scratch framebuffer of the source's own native format, then run the caller's blit
    // - same mask, same filter, same orientation - from the scratch into the real destination.
    // Overlapping rectangles stay undefined, as GL leaves them. A LINEAR blit that scales reads
    // clamped texels at the rectangle's edge where GL would read the neighboring ones.

    // What a framebuffer attachment point resolves to on the DRIVER side: the native object and,
    // for a texture, the level, layer and cube face. A null name means the point is empty.
    struct DriverAttachmentImage {
        GLint Type = GL_NONE;
        GLint Name = 0;
        GLint Level = 0;
        GLint Layer = 0;
        GLint Face = 0;
    };

    static DriverAttachmentImage QueryDriverAttachmentImage(GLenum framebufferTarget, GLenum attachment) {
        DriverAttachmentImage image;
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(framebufferTarget, attachment,
                                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &image.Type);
        if (image.Type != GL_TEXTURE && image.Type != GL_RENDERBUFFER) return {};
        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(framebufferTarget, attachment,
                                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &image.Name);
        if (image.Type == GL_TEXTURE) {
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(framebufferTarget, attachment,
                                                              GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &image.Level);
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                framebufferTarget, attachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, &image.Face);
            g_GLESFuncs.glGetFramebufferAttachmentParameteriv(framebufferTarget, attachment,
                                                              GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &image.Layer);
        }
        return image;
    }

    static Bool IsSameDriverImage(const DriverAttachmentImage& a, const DriverAttachmentImage& b) {
        return a.Name != 0 && a.Type == b.Type && a.Name == b.Name && a.Level == b.Level && a.Layer == b.Layer &&
               a.Face == b.Face;
    }

    // The color attachment point the bound READ framebuffer reads from, GL_NONE when it reads
    // nothing.
    static GLenum DriverReadColorAttachment() {
        GLint readBuffer = GL_NONE;
        g_GLESFuncs.glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        if (readBuffer < GL_COLOR_ATTACHMENT0 || readBuffer > GL_COLOR_ATTACHMENT31) return GL_NONE;
        return static_cast<GLenum>(readBuffer);
    }

    // The aspects of `mask` whose read and draw buffers are one image on the driver side - the
    // aspects ES refuses to blit. The window-system framebuffer (driver name 0) has no attachment
    // to compare or take a format from and is not considered.
    static GLbitfield DriverBlitAspectsSharingAnImage(GLbitfield mask) {
        GLint readFramebuffer = 0;
        GLint drawFramebuffer = 0;
        g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        if (readFramebuffer == 0 || drawFramebuffer == 0) return 0;

        GLbitfield shared = 0;
        if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
            const GLenum readAttachment = DriverReadColorAttachment();
            if (readAttachment != GL_NONE) {
                const DriverAttachmentImage source = QueryDriverAttachmentImage(GL_READ_FRAMEBUFFER, readAttachment);
                GLint maxDrawBuffers = 0;
                g_GLESFuncs.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
                for (GLint i = 0; i < maxDrawBuffers && source.Name != 0; ++i) {
                    GLint drawBuffer = GL_NONE;
                    g_GLESFuncs.glGetIntegerv(GL_DRAW_BUFFER0 + i, &drawBuffer);
                    if (drawBuffer < GL_COLOR_ATTACHMENT0 || drawBuffer > GL_COLOR_ATTACHMENT31) continue;
                    if (IsSameDriverImage(
                            source, QueryDriverAttachmentImage(GL_DRAW_FRAMEBUFFER, static_cast<GLenum>(drawBuffer)))) {
                        shared |= GL_COLOR_BUFFER_BIT;
                        break;
                    }
                }
            }
        }
        const struct {
            GLbitfield bit;
            GLenum attachment;
        } depthStencil[] = {{GL_DEPTH_BUFFER_BIT, GL_DEPTH_ATTACHMENT}, {GL_STENCIL_BUFFER_BIT, GL_STENCIL_ATTACHMENT}};
        for (const auto& aspect : depthStencil) {
            if ((mask & aspect.bit) == 0) continue;
            if (IsSameDriverImage(QueryDriverAttachmentImage(GL_READ_FRAMEBUFFER, aspect.attachment),
                                  QueryDriverAttachmentImage(GL_DRAW_FRAMEBUFFER, aspect.attachment))) {
                shared |= aspect.bit;
            }
        }
        return shared;
    }

    // The native sized internal format of what a READ framebuffer attachment point holds, 0 when
    // it cannot be determined. ES has no "which target is this texture" query, so a texture is
    // found by binding it to each candidate target in turn: glBindTexture refuses a name created
    // for another target and leaves the binding alone.
    static GLenum QueryDriverReadAttachmentInternalFormat(GLenum attachment) {
        const DriverAttachmentImage image = QueryDriverAttachmentImage(GL_READ_FRAMEBUFFER, attachment);
        if (image.Name == 0) return 0;
        GLint internalFormat = 0;
        if (image.Type == GL_RENDERBUFFER) {
            if (!g_GLESFuncs.glGetRenderbufferParameteriv) return 0;
            GLint previous = 0;
            g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(image.Name));
            g_GLESFuncs.glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &internalFormat);
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous));
        } else {
            if (!g_GLESFuncs.glGetTexLevelParameteriv) return 0;
            struct Candidate {
                GLenum target;
                GLenum binding;
            };
            const Candidate cube[] = {{GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BINDING_CUBE_MAP}};
            const Candidate others[] = {{GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D},
                                        {GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BINDING_2D_ARRAY},
                                        {GL_TEXTURE_3D, GL_TEXTURE_BINDING_3D},
                                        {GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_BINDING_CUBE_MAP_ARRAY}};
            const Bool isCubeFace = image.Face != 0 && image.Face != GL_NONE;
            const Candidate* candidates = isCubeFace ? cube : others;
            const SizeT candidateCount = isCubeFace ? std::size(cube) : std::size(others);
            for (SizeT i = 0; i < candidateCount && internalFormat == 0; ++i) {
                const Candidate& candidate = candidates[i];
                DrainBlitErrors();
                GLint previous = 0;
                g_GLESFuncs.glGetIntegerv(candidate.binding, &previous);
                g_GLESFuncs.glBindTexture(candidate.target, static_cast<GLuint>(image.Name));
                if (g_GLESFuncs.glGetError() != GL_NO_ERROR) continue;
                const GLenum levelTarget = isCubeFace ? static_cast<GLenum>(image.Face) : candidate.target;
                g_GLESFuncs.glGetTexLevelParameteriv(levelTarget, image.Level, GL_TEXTURE_INTERNAL_FORMAT,
                                                     &internalFormat);
                g_GLESFuncs.glBindTexture(candidate.target, static_cast<GLuint>(previous));
            }
            DrainBlitErrors();
        }
        // A texture specified with an unsized format reports it back; renderbuffer storage
        // takes only sized ones.
        switch (internalFormat) {
        case GL_RGBA:
            return GL_RGBA8;
        case GL_RGB:
            return GL_RGB8;
        case GL_DEPTH_STENCIL:
            return GL_DEPTH24_STENCIL8;
        case GL_DEPTH_COMPONENT:
            return GL_DEPTH_COMPONENT24;
        default:
            return static_cast<GLenum>(internalFormat);
        }
    }

    static Bool IsPackedDepthStencilFormat(GLenum format) {
        return format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8;
    }

    // The two-step copy itself. Both framebuffers are single-sample (ES cannot blit into a
    // multisample one at all). Returns false, having written nothing to the destination, when
    // the scratch cannot hold the source.
    static Bool BlitWithinOneImage(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                   GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        static NativeContextName s_scratchFramebuffer; // per context: a framebuffer is never shared
        static Uint s_colorRenderbuffer = 0;
        static Uint s_depthStencilRenderbuffer = 0;
        static GLenum s_colorFormat = 0;
        static GLenum s_depthStencilFormat = 0;
        static GLsizei s_colorWidth = 0, s_colorHeight = 0;
        static GLsizei s_depthStencilWidth = 0, s_depthStencilHeight = 0;
        static Uint s_scratchContextGeneration = ~0u;

        if (!g_GLESFuncs.glGenFramebuffers || !g_GLESFuncs.glGenRenderbuffers || !g_GLESFuncs.glRenderbufferStorage ||
            !g_GLESFuncs.glFramebufferRenderbuffer) {
            return false;
        }
        const GLint left = std::min(srcX0, srcX1);
        const GLint right = std::max(srcX0, srcX1);
        const GLint bottom = std::min(srcY0, srcY1);
        const GLint top = std::max(srcY0, srcY1);
        const GLsizei width = static_cast<GLsizei>(right - left);
        const GLsizei height = static_cast<GLsizei>(top - bottom);
        if (width <= 0 || height <= 0) return false;

        // The scratch takes each aspect the READ framebuffer has: one it lacks is silently
        // skipped by the blit either way, and leaving it off the scratch keeps it skipped.
        GLenum colorFormat = 0;
        if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
            const GLenum readAttachment = DriverReadColorAttachment();
            if (readAttachment != GL_NONE) {
                colorFormat = QueryDriverReadAttachmentInternalFormat(readAttachment);
                if (colorFormat == 0) return false;
            }
        }
        GLenum depthStencilFormat = 0;
        GLenum depthStencilAttachment = GL_NONE;
        if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0) {
            const GLenum depthFormat =
                (mask & GL_DEPTH_BUFFER_BIT) != 0 ? QueryDriverReadAttachmentInternalFormat(GL_DEPTH_ATTACHMENT) : 0;
            const GLenum stencilFormat = (mask & GL_STENCIL_BUFFER_BIT) != 0
                                             ? QueryDriverReadAttachmentInternalFormat(GL_STENCIL_ATTACHMENT)
                                             : 0;
            // ES blits depth/stencil only between identical formats, which a scratch renderbuffer
            // of the source's own format is. Separate depth and stencil images would need two
            // scratch buffers; that shape is declined.
            if (depthFormat != 0 && stencilFormat != 0 && depthFormat != stencilFormat) return false;
            depthStencilFormat = depthFormat != 0 ? depthFormat : stencilFormat;
            if (depthStencilFormat != 0) {
                depthStencilAttachment = IsPackedDepthStencilFormat(depthStencilFormat) ? GL_DEPTH_STENCIL_ATTACHMENT
                                         : depthFormat != 0                             ? GL_DEPTH_ATTACHMENT
                                                                                        : GL_STENCIL_ATTACHMENT;
            }
        }
        if (colorFormat == 0 && depthStencilFormat == 0) return false;

        if (s_scratchContextGeneration != g_backendContextGeneration) {
            // The ids belonged to a dead context; the context reclaimed them with it.
            s_scratchFramebuffer = 0;
            s_colorRenderbuffer = 0;
            s_depthStencilRenderbuffer = 0;
            s_colorFormat = 0;
            s_depthStencilFormat = 0;
            s_scratchContextGeneration = g_backendContextGeneration;
        }
        if (s_scratchFramebuffer == 0) {
            g_GLESFuncs.glGenFramebuffers(1, &s_scratchFramebuffer);
            if (s_scratchFramebuffer == 0) return false;
        }

        GLint previousRenderbuffer = 0;
        g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
        const auto ensureStorage = [](Uint& renderbuffer, GLenum& currentFormat, GLsizei& currentWidth,
                                      GLsizei& currentHeight, GLenum format, GLsizei w, GLsizei h) {
            if (renderbuffer == 0) {
                g_GLESFuncs.glGenRenderbuffers(1, &renderbuffer);
                currentFormat = 0;
            }
            if (renderbuffer == 0) return;
            if (currentFormat != format || currentWidth < w || currentHeight < h) {
                currentWidth = std::max(currentWidth, w);
                currentHeight = std::max(currentHeight, h);
                g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
                g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, format, currentWidth, currentHeight);
                currentFormat = format;
            }
        };
        if (colorFormat != 0) {
            ensureStorage(s_colorRenderbuffer, s_colorFormat, s_colorWidth, s_colorHeight, colorFormat, width, height);
        }
        if (depthStencilFormat != 0) {
            ensureStorage(s_depthStencilRenderbuffer, s_depthStencilFormat, s_depthStencilWidth, s_depthStencilHeight,
                          depthStencilFormat, width, height);
        }
        g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));

        GLint previousDraw = 0;
        GLint previousRead = 0;
        g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
        g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);

        // Every point is (re)attached on every call: the scratch is shared between color and
        // depth/stencil shapes, and a stale attachment would make it incomplete or blit an
        // aspect the source does not have.
        g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_scratchFramebuffer);
        g_GLESFuncs.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                              colorFormat != 0 ? s_colorRenderbuffer : 0);
        g_GLESFuncs.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        if (depthStencilAttachment != GL_NONE) {
            g_GLESFuncs.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, depthStencilAttachment, GL_RENDERBUFFER,
                                                  s_depthStencilRenderbuffer);
        }
        const GLbitfield stagedMask =
            (colorFormat != 0 ? static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT) : 0) |
            (depthStencilFormat != 0 ? mask & static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) : 0);
        Bool copied = g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (copied) {
            // The staging copy is not the application's blit: its scissor box would clip the
            // copy and leave never-written scratch texels for the second blit to carry over. The
            // second blit keeps the scissor, which is the application's semantics.
            DrainBlitErrors();
            const Bool scissorWasEnabled = RenderStateImpl::DriverScissorTestEnabled();
            if (scissorWasEnabled) g_GLESFuncs.glDisable(GL_SCISSOR_TEST);
            g_GLESFuncs.glBlitFramebuffer(left, bottom, right, top, 0, 0, width, height, stagedMask, GL_NEAREST);
            if (scissorWasEnabled) g_GLESFuncs.glEnable(GL_SCISSOR_TEST);
            copied = g_GLESFuncs.glGetError() == GL_NO_ERROR;
        }
        if (copied) {
            // Mirror the caller's orientation into the scratch-relative source rectangle so a
            // flipped blit stays flipped.
            const GLint blitX0 = srcX0 <= srcX1 ? 0 : width;
            const GLint blitX1 = srcX0 <= srcX1 ? width : 0;
            const GLint blitY0 = srcY0 <= srcY1 ? 0 : height;
            const GLint blitY1 = srcY0 <= srcY1 ? height : 0;
            g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, s_scratchFramebuffer);
            g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
            DrainBlitErrors();
            g_GLESFuncs.glBlitFramebuffer(blitX0, blitY0, blitX1, blitY1, dstX0, dstY0, dstX1, dstY1, stagedMask,
                                          filter);
            copied = g_GLESFuncs.glGetError() == GL_NO_ERROR;
        }

        g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
        g_GLESFuncs.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
        FramebufferImpl::InvalidateFramebufferBindingCache();
        FramebufferImpl::InvalidateFramebufferHandleArmMemos();
        return copied;
    }

    static void IssueBlitWithResolveFallback(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                                             GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        DrainBlitErrors();
        g_GLESFuncs.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        if (g_GLESFuncs.glGetError() == GL_NO_ERROR) {
            return;
        }
        // Two ES restrictions desktop GL does not have are worth a second attempt: a
        // multisample resolve that also converts color format, and any blit into a
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
            FramebufferImpl::InvalidateFramebufferHandleArmMemos();
        }
        if (readSamples <= 0 && drawSamples <= 0 && DriverBlitAspectsSharingAnImage(mask) != 0) {
            // ES refused it because one image is both ends; GL only refuses an overlap, which it
            // leaves undefined. When the scratch cannot take the source, the per-aspect retries
            // below still deliver every aspect that is not shared.
            if (BlitWithinOneImage(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter)) {
                DrainBlitErrors();
                return;
            }
            MGLOG_E_ONCE("BlitFramebuffer: a blit within one image could not be staged through a scratch copy");
        }
        if (readSamples <= 0 && drawSamples > 0) {
            // Single-sample source into a multisample destination: ES rejects the call
            // outright, desktop GL replicates the source sample into every destination one.
            if (ReplicateBlitIntoMultisampleDraw(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask)) {
                if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
                    MGLOG_E_ONCE(
                        "BlitFramebuffer: color replicate into a multisample draw framebuffer is not emulated");
                }
            }
            return;
        }
        // The combined call raised an error, so by GL 4.6 2.3.1 it wrote nothing at all: BOTH
        // aspect groups still owe their copy, and each has to be retried on its own. Re-issuing
        // the depth/stencil half only as a rider on a SUCCESSFUL color resolve dropped it
        // silently whenever the color half could not be emulated - and on a framebuffer whose
        // only attachment is depth it never can, because the color emulation has no attachment
        // to take a format from (KHR-GL33.framebuffer_blit's depth config test blits
        // COLOR|DEPTH|STENCIL across depth-only framebuffers and kept reading the clear value).
        const GLbitfield colourBit = mask & static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
        const GLbitfield dsBits = mask & static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        // The color group's one emulation is the multisample resolve that also converts format,
        // which is the shape this names. It used to double as an early-out for the whole
        // function, which is what cost a depth-only mask its single-aspect retry.
        const Bool multisampleResolve = readSamples > 0 && drawSamples <= 0;
        if (colourBit != 0) {
            DrainBlitErrors();
            g_GLESFuncs.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, colourBit, filter);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                const Bool emulated = multisampleResolve &&
                                      ResolveThenBlit(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, filter);
                if (!emulated) {
                    MGLOG_E_ONCE("BlitFramebuffer: the color aspect was dropped - the driver rejected it on its "
                                 "own and no emulation applies");
                }
            }
        }
        if (dsBits != 0) {
            DrainBlitErrors();
            g_GLESFuncs.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, dsBits, filter);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                // Nothing to fall back on yet: ResolveThenBlit is color-only and the replicate
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

    // ---- P5e (fb, CONTRACT-P5E.md §5.4): the same aspect plan, from the two RECORDS ---------
    //
    // The blit was the last entry point in this family that still reached into two frontend
    // FramebufferObjects on every call - GetDrawBuffers, GetReadBuffer, GetAttachment and then
    // six texture properties per aspect - and it did it on BOTH arms, the named one reaching
    // the objects through the twin table's state note. Every one of those questions is a field
    // of MGPFramebufferState:
    //
    //   GetDrawBuffers()            -> DrawBuffers[8]      (attachment index, -1 = None)
    //   GetReadBuffer()             -> ReadSurface matched against Color[] (the same match
    //                                  SyncReadBufferToBackend makes; ReadSurface is resolved
    //                                  from THIS framebuffer's own read buffer under every
    //                                  Target, Named included)
    //   GetAttachment(point)        -> PushedSurfaceForAttachment
    //   IsTexture / IsLayered       -> Kind / Layered
    //   GetTextureLevel / Layer     -> Level / Layer
    //   GetFormat                   -> InternalFormat (the attachment's, which is the texture's)
    //   GetSamples                  -> record.Samples
    //
    // SAMPLES COMES OFF THE RECORD RATHER THAN OFF EACH TEXTURE, and that is a deliberate
    // narrowing: the frontend form asked each endpoint texture, the record states it per
    // FRAMEBUFFER. A framebuffer whose attachments disagree about sample count is incomplete,
    // so the two cannot differ where this substitution is allowed to fire; and the test the
    // answer feeds ("either end is multisample -> leave it to the driver") is conservative in
    // the direction that hands the call back.
    static FramebufferAttachmentType PushedReadBufferPoint(const MG_Pipe::MGPFramebufferState& record) {
        if (MG_Pipe::MGPipeHandleIsNull(record.ReadSurface.Res)) return FramebufferAttachmentType::None;
        for (Uint i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i) {
            const auto& color = record.Color[i];
            if (color.Res == record.ReadSurface.Res && color.Kind == record.ReadSurface.Kind &&
                color.Layered == record.ReadSurface.Layered && color.Level == record.ReadSurface.Level &&
                color.Layer == record.ReadSurface.Layer && color.UploadTarget == record.ReadSurface.UploadTarget) {
                return static_cast<FramebufferAttachmentType>(static_cast<Int>(FramebufferAttachmentType::Color0) +
                                                              static_cast<Int>(i));
            }
        }
        return FramebufferAttachmentType::None;
    }

    static GLbitfield BlitLayeredDestinationAspects(const MG_Pipe::MGPFramebufferState& readRecord,
                                                    const MG_Pipe::MGPFramebufferState& drawRecord, GLint srcX0,
                                                    GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                                    GLint dstX1, GLint dstY1, GLbitfield mask) {
        if (mask == 0) return 0;
        if (!g_GLESFuncs.glCopyImageSubData) return 0;
        if (!MG_Util::SelfTest::BlitIgnoresDestinationArrayLayer(g_GLESFuncs)) return 0;
        // The default framebuffer has no layers to get wrong.
        if (readRecord.IsDefault != 0 || drawRecord.IsDefault != 0) return 0;

        const Int width = srcX1 - srcX0;
        const Int height = srcY1 - srcY0;
        const Bool oneToOne = width > 0 && height > 0 && (dstX1 - dstX0) == width && (dstY1 - dstY0) == height;
        // The scissor clips a blit and does not clip a copy, so an enabled scissor makes the two
        // different operations no matter how the rectangles line up.
        const Bool scissorEnabled = RenderStateImpl::DriverScissorTestEnabled();

        using MobileGL::FramebufferAttachmentType;
        struct AspectPlan {
            GLbitfield bit;
            FramebufferAttachmentType source;
            FramebufferAttachmentType destination;
        };
        // The buffer is FOUND rather than assumed to be slot 0, for the frontend form's reason:
        // a blit writes every ENABLED draw buffer, and glDrawBuffers(NONE, NONE, NONE,
        // COLOR_ATTACHMENT0) leaves slot 0 empty while still naming exactly one destination.
        // -1 is the record's spelling of GL_NONE and is not "no such attachment".
        Int enabledDrawBuffers = 0;
        FramebufferAttachmentType colorDestination = FramebufferAttachmentType::None;
        for (Uint i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i) {
            const Int8 index = drawRecord.DrawBuffers[i];
            if (index < 0) continue;
            ++enabledDrawBuffers;
            if (enabledDrawBuffers == 1) {
                colorDestination = static_cast<FramebufferAttachmentType>(
                    static_cast<Int>(FramebufferAttachmentType::Color0) + static_cast<Int>(index));
            }
        }
        const AspectPlan plans[] = {
            {GL_COLOR_BUFFER_BIT, PushedReadBufferPoint(readRecord), colorDestination},
            {GL_DEPTH_BUFFER_BIT, FramebufferAttachmentType::Depth, FramebufferAttachmentType::Depth},
            {GL_STENCIL_BUFFER_BIT, FramebufferAttachmentType::Stencil, FramebufferAttachmentType::Stencil},
        };

        GLbitfield handled = 0;
        for (const AspectPlan& plan : plans) {
            if ((mask & plan.bit) == 0) continue;
            if (plan.source == FramebufferAttachmentType::Unknown ||
                plan.destination == FramebufferAttachmentType::Unknown ||
                plan.source == FramebufferAttachmentType::None || plan.destination == FramebufferAttachmentType::None) {
                continue;
            }
            const MG_Pipe::MGPSurface* const sourceSurface =
                FramebufferImpl::PushedSurfaceForAttachment(readRecord, plan.source);
            const MG_Pipe::MGPSurface* const destinationSurface =
                FramebufferImpl::PushedSurfaceForAttachment(drawRecord, plan.destination);
            if (sourceSurface == nullptr || destinationSurface == nullptr) continue;
            // Renderbuffers have no layers, so a destination that is one cannot be hitting this.
            if (sourceSurface->Kind != MG_Pipe::kMGPipeSurfaceKindTexture ||
                destinationSurface->Kind != MG_Pipe::kMGPipeSurfaceKindTexture) {
                continue;
            }
            if (MG_Pipe::MGPipeHandleIsNull(sourceSurface->Res) ||
                MG_Pipe::MGPipeHandleIsNull(destinationSurface->Res)) {
                continue;
            }
            // Layer 0 is the case the driver gets right, and a LAYERED attachment
            // (glFramebufferTexture with no layer) blits its layer 0 by spec - neither is this
            // defect.
            if (destinationSurface->Layer == 0) continue;
            if (destinationSurface->Layered != 0 || sourceSurface->Layered != 0) continue;

            // glCopyImageSubData moves texel blocks: same format both ends, or it is a different
            // operation. Multisample endpoints would additionally have to agree on sample count,
            // which is a resolve the driver still owns.
            if (sourceSurface->InternalFormat != destinationSurface->InternalFormat) continue;
            if (readRecord.Samples > 0 || drawRecord.Samples > 0) continue;
            // Copying an image region onto itself is undefined for glCopyImageSubData, and a blit
            // whose source and destination overlap is undefined for GL too - so this is not a
            // shape to substitute FOR, it is one to leave exactly as the application wrote it.
            if (sourceSurface->Res == destinationSurface->Res && sourceSurface->Level == destinationSurface->Level &&
                sourceSurface->Layer == destinationSurface->Layer) {
                continue;
            }

            // A combined depth-stencil texture is ONE image to glCopyImageSubData: it carries both
            // aspects across whether or not the mask asked for both. Taking only GL_DEPTH_BUFFER_BIT
            // on a DEPTH24_STENCIL8 destination would overwrite a stencil the application asked to
            // keep, so the copy is only allowed when the mask covers everything the format holds.
            const auto format = static_cast<TextureInternalFormat>(destinationSurface->InternalFormat);
            const Bool hasDepth = MG_Util::IsDepthFormatInternalFormat(format);
            const Bool hasStencil = MG_Util::IsStencilFormatInternalFormat(format);
            if (hasDepth && (mask & GL_DEPTH_BUFFER_BIT) == 0) continue;
            if (hasStencil && (mask & GL_STENCIL_BUFFER_BIT) == 0) continue;
            // ... and having carried both, it must be credited with both, or the caller hands the
            // stencil half to the driver and it lands on layer 0 after all.
            const GLbitfield aspectBits = hasDepth || hasStencil
                                              ? static_cast<GLbitfield>((hasDepth ? GL_DEPTH_BUFFER_BIT : 0) |
                                                                        (hasStencil ? GL_STENCIL_BUFFER_BIT : 0))
                                              : static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
            if ((handled & aspectBits) == aspectBits) continue;

            if (!oneToOne || scissorEnabled || (plan.bit == GL_COLOR_BUFFER_BIT && enabledDrawBuffers != 1)) {
                MGLOG_E_ONCE("BlitFramebuffer: this driver ignores a non-zero destination array layer and this "
                             "blit cannot be expressed as a copy (%s), so it will land on layer 0",
                             !oneToOne        ? "it scales or flips"
                             : scissorEnabled ? "the scissor test is enabled"
                                              : "the destination has more than one draw buffer");
                continue;
            }

            auto& backendSource = TextureImpl::SyncTextureToBackendByHandle(sourceSurface->Res,
                                                                            /*imageBindableStorageRequired=*/false);
            if (!backendSource) continue;
            const GLuint sourceName = backendSource->GetBackendTextureId();
            // BY VALUE past this point, for the reason the copy-image endpoint builder gives:
            // the second sync can grow the registry and relocate the first result.
            auto& backendDestinationSlot =
                TextureImpl::SyncTextureToBackendByHandle(destinationSurface->Res,
                                                          /*imageBindableStorageRequired=*/false);
            if (!backendDestinationSlot) continue;
            const GLuint destinationName = backendDestinationSlot->GetBackendTextureId();
            if (sourceName == 0 || destinationName == 0) continue;
            const GLenum sourceTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(
                static_cast<TextureTarget>(sourceSurface->TextureTarget));
            const GLenum destinationTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(
                static_cast<TextureTarget>(destinationSurface->TextureTarget));

            ClearGLErrors();
            g_GLESFuncs.glCopyImageSubData(sourceName, sourceTarget, static_cast<GLint>(sourceSurface->Level), srcX0,
                                           srcY0, static_cast<GLint>(sourceSurface->Layer), destinationName,
                                           destinationTarget, static_cast<GLint>(destinationSurface->Level), dstX0,
                                           dstY0, static_cast<GLint>(destinationSurface->Layer), width, height, 1);
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

    // The two endpoint records of a blit, or nulls. `fbo` null-or-default means "the record of
    // whatever is bound to that target", which is what the bound arm asks; a named blit passes
    // the handles its record carried.
    static const MG_Pipe::MGPFramebufferState* BlitEndpointRecord(MG_Pipe::MGPipeHandle fbo,
                                                                  FramebufferTarget boundTarget) {
        const auto& st = MG_Pipe::MGPipeApplier();
        if (MG_Pipe::MGPipeHandleIsNull(fbo)) {
            return boundTarget == FramebufferTarget::Read ? st.ReadFramebuffer() : st.DrawFramebuffer();
        }
        return st.FramebufferRecordFor(fbo);
    }

    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif

        // P5c (hd, CONTRACT-P5C §3.3): THE NAMED ARM. A blit record whose ReadFbo/DrawFbo are
        // non-null is a glBlitNamedFramebuffer: both framebuffers resolve from the record's
        // handles through the FBO twin table (the client's ScopedBlitBindings staging and this
        // function's binding-slot read at the bottom are gone from the split path), and the
        // sink is told the pair was consumed - a backend that reaches here without consuming
        // it has no named arm and the verb declines there.
        auto& applierState = MG_Pipe::MGPipeApplier();
        const MG_Pipe::MGPipeHandle readFbo = applierState.VerbBlitReadFbo;
        const MG_Pipe::MGPipeHandle drawFbo = applierState.VerbBlitDrawFbo;
        if (!MG_Pipe::MGPipeHandleIsNull(readFbo) || !MG_Pipe::MGPipeHandleIsNull(drawFbo)) {
            applierState.VerbBlitNamedConsumed = true;
            TextureImpl::SyncNeccessaryTextures();
            RenderStateImpl::SyncRenderState();

            SyncAndBindFramebufferByHandle(readFbo, FramebufferTarget::Read, /*forceSync=*/true);
            SyncAndBindFramebufferByHandle(drawFbo, FramebufferTarget::Draw, /*forceSync=*/true);

            MGLOG_D("ES BlitNamedFramebuffer({%u,%u} -> {%u,%u}, %d, %d, %d, %d, %d, %d, %d, %d, 0x%x, %s)",
                    readFbo.Slot, readFbo.Gen, drawFbo.Slot, drawFbo.Gen, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0,
                    dstX1, dstY1, mask, MG_Util::ConvertGLEnumToString(filter).c_str());
            // P5e (fb, §5.4): only the probed defect makes this do anything, and it now
            // asks the two RECORDS. The state-note detour this used to take - the twin
            // table handing back the frontend object it was synced from, plus
            // pDefaultFramebufferInfo->defaultFBO for a default endpoint - is gone with the
            // note itself; a named framebuffer always has a record (a Named record precedes
            // every DSA site, ID-19), so the "missing object skips the workaround" case is
            // now a missing record and means the same thing.
            const auto* readRecord = BlitEndpointRecord(readFbo, FramebufferTarget::Read);
            const auto* drawRecord = BlitEndpointRecord(drawFbo, FramebufferTarget::Draw);
            if (readRecord != nullptr && drawRecord != nullptr) {
                mask &= ~BlitLayeredDestinationAspects(*readRecord, *drawRecord, srcX0, srcY0, srcX1, srcY1, dstX0,
                                                       dstY0, dstX1, dstY1, mask);
            }
            if (mask != 0) {
                IssueBlitWithResolveFallback(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
            }
            // The driver's bindings are now the two NAMED framebuffers; put the bound ones
            // back exactly as the monolith named entry point does.
            ForceBindCurrentFBO(FramebufferTarget::Read);
            ForceBindCurrentFBO(FramebufferTarget::Draw);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
                MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            return;
        }

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
        // A no-op on every driver that honors a non-zero destination array layer, which is all
        // of them but the probed one. Whatever it performs itself is taken out of the mask.
        // P5e (fb, §5.4): the bound arm's two binding-slot reads were the last GetBoundObject
        // pair on this path; under a transport the two bound RECORDS answer instead
        // (FramebufferRecordFor(st.BoundFramebuffer[t]), through BlitEndpointRecord's null
        // case). SyncCurrentFBO ran a few lines up and refuses if either is missing, so a null
        // here means the blit is against the default framebuffer's record, which the overload
        // declines on IsDefault.
        const auto* readRecord = BlitEndpointRecord(MG_Pipe::kMGPipeNullHandle, FramebufferTarget::Read);
        const auto* drawRecord = BlitEndpointRecord(MG_Pipe::kMGPipeNullHandle, FramebufferTarget::Draw);
        if (readRecord != nullptr && drawRecord != nullptr) {
            mask &= ~BlitLayeredDestinationAspects(*readRecord, *drawRecord, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0,
                                                   dstX1, dstY1, mask);
        }
        if (mask != 0) {
            IssueBlitWithResolveFallback(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        }
        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__](auto err) {
            MGLOG_D("ES error (%s:%d): %s", file, line, MG_Util::ConvertGLEnumToString(err).c_str());
        });
    }

    namespace RenderStateImpl {
        void RunScissorDisabledForTesting(void (*inside)(void*), void* data) {
            ScopedScissorDisable scissorOff;
            inside(data);
        }
    } // namespace RenderStateImpl
} // namespace MobileGL::MG_Backend::DirectGLES
