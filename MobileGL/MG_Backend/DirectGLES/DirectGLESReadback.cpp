// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESReadback.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    // A readback on the server writes REPLY SCRATCH, so it has no pack buffer - and this
    // function is how every pack-PBO lookup in this file says so under a transport.
    //
    // THE REASON IS NOT "THE CLIENT REFUSES PACK_BUFFER BEFORE EMISSION", which is what this
    // comment used to claim and is not true of the tree (P3b/P4b espryt D1 slice 4). The client
    // SERVICES the pack PBO itself, AFTER the reply lands: EmitTables.cpp uploads the reply's
    // tight rows into the bound buffer with pbo->UploadSubData, one row at a time at the stride
    // the application's GL_PACK_* state implies, so padding and untouched bytes survive. A
    // refusal would have been visible as a GL error; what actually happens is an ordinary
    // buffer upload the client performs on its own side.
    //
    // The server's half is ID-49's: read with NEUTRAL pack state into a tight w*h*bpp reply.
    // Which is exactly why the server must see NO pack buffer here - not because the client
    // forbade one, but because the client kept it.
    static const SharedPtr<MG_State::GLState::BufferObject>& SplitReadbackPackBuffer() {
        static const SharedPtr<MG_State::GLState::BufferObject> none;
        return none;
    }

    // The frontend's current PACK parameters, for readbacks the ES driver serves
    // directly with the client's layout.
    static PixelStoreImpl::PackState PackStateFromContext() {
        const auto packParams = MG_Pipe::gPipeInputs->GetPixelStoreParameters(false);
        return {static_cast<GLint>(packParams.Alignment), static_cast<GLint>(packParams.RowLength),
                static_cast<GLint>(packParams.SkipRows), static_cast<GLint>(packParams.SkipPixels)};
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
        const auto packParams = MG_Pipe::gPipeInputs->GetPixelStoreParameters(false);
        const SizeT rowPixels = static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : width);
        const SizeT dstRowStride = AlignPixelRow(rowPixels * dstPixelBytes, packParams.Alignment);
        const SizeT dstOffset = static_cast<SizeT>(std::max(packParams.SkipRows, 0)) * dstRowStride +
                                static_cast<SizeT>(std::max(packParams.SkipPixels, 0)) * dstPixelBytes;
        const SizeT rowBytes = static_cast<SizeT>(width) * dstPixelBytes;
        const SizeT packedSize = dstOffset + static_cast<SizeT>(height - 1) * dstRowStride + rowBytes;
        const auto& pixelPackBufferObject =
            SplitReadbackPackBuffer();
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
    // and llvmpipe's ES. Every native attempt therefore failed with a GL error and wrote
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
        static NativeContextName s_colorFramebuffer; // per context, as are the vertex array and the
                                                     // slots' framebuffers below
        static GLuint s_colorTexture = 0;
        static GLsizei s_colorWidth = 0;
        static GLsizei s_colorHeight = 0;
        static NativeContextName s_vertexArray;
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
        // Each slot's own framebuffer (see StageSlot), one per native context.
        static NativeContextName s_slotFramebuffers[2];
        static NativeContextName& SlotFramebuffer(const StageSlot& slot) { return s_slotFramebuffers[&slot - s_slots]; }

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
                s_slotFramebuffers[0] = 0;
                s_slotFramebuffers[1] = 0;
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
            for (NativeContextName& framebuffer : s_slotFramebuffers) {
                if (framebuffer == 0) {
                    g_GLESFuncs.glGenFramebuffers(1, &framebuffer);
                    if (framebuffer == 0) return false;
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

            FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, SlotFramebuffer(slot));
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
    // parameters. GL 4.6 core 18.2.8 table 8.6: the normalized depth is written as-is for the
    // floating-point client types and scaled into the full range of whichever integer width the
    // client asked for otherwise - 2^n - 1 for an unsigned width, 2^(n-1) - 1 for a signed one.
    // Sharing one denominator across the two signednesses would put a 0.5 depth at 32768, which
    // as a GLshort is -32768, so GL_BYTE/GL_SHORT/GL_INT each keep their own.
    static Bool ReadPixelsDepthComponent(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type,
                                         void* pixels) {
        SizeT dstPixelBytes = 0;
        switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE: dstPixelBytes = sizeof(Uint8); break;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
        case GL_HALF_FLOAT: dstPixelBytes = sizeof(Uint16); break;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT: dstPixelBytes = sizeof(Uint32); break;
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
                                          const Float depthValue = srcRow[col];
                                          switch (type) {
                                          case GL_BYTE:
                                              reinterpret_cast<Int8*>(dst)[col] = static_cast<Int8>(
                                                  NormalizedDepthToUnsigned(depthValue, 127.0));
                                              break;
                                          case GL_UNSIGNED_BYTE:
                                              dst[col] = static_cast<Uint8>(
                                                  NormalizedDepthToUnsigned(depthValue, 255.0));
                                              break;
                                          case GL_SHORT:
                                              reinterpret_cast<Int16*>(dst)[col] = static_cast<Int16>(
                                                  NormalizedDepthToUnsigned(depthValue, 32767.0));
                                              break;
                                          case GL_UNSIGNED_SHORT:
                                              reinterpret_cast<Uint16*>(dst)[col] = static_cast<Uint16>(
                                                  NormalizedDepthToUnsigned(depthValue, 65535.0));
                                              break;
                                          case GL_INT:
                                              reinterpret_cast<Int32*>(dst)[col] = static_cast<Int32>(
                                                  NormalizedDepthToUnsigned(depthValue, 2147483647.0));
                                              break;
                                          case GL_UNSIGNED_INT:
                                              reinterpret_cast<Uint32*>(dst)[col] =
                                                  NormalizedDepthToUnsigned(depthValue, 4294967295.0);
                                              break;
                                          case GL_HALF_FLOAT:
                                              reinterpret_cast<Uint16*>(dst)[col] =
                                                  MG_Util::EncodeFloatToHalfBits(depthValue);
                                              break;
                                          default:
                                              reinterpret_cast<GLfloat*>(dst)[col] = depthValue;
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
        // the client asked for, and converted to a float value for the floating-point widths - both
        // GL_FLOAT and GL_HALF_FLOAT. The signed widths are as legal as the unsigned ones - the CTS
        // reads stencil with GL_INT - and rejecting them here used to let the call fall through to a
        // native ES read the driver refuses, after which nothing was written at all and the caller
        // kept its zeros.
        SizeT dstPixelBytes = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE:
        case GL_BYTE: dstPixelBytes = sizeof(Uint8); break;
        case GL_UNSIGNED_SHORT:
        case GL_SHORT:
        case GL_HALF_FLOAT: dstPixelBytes = sizeof(Uint16); break;
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
                                          case GL_HALF_FLOAT:
                                              reinterpret_cast<Uint16*>(dst)[col] =
                                                  MG_Util::EncodeFloatToHalfBits(static_cast<Float>(srcRow[col]));
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
            SplitReadbackPackBuffer();
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

        if (!ClientReadback::StoreWideRowsToClient(wide.data(), wideType, width, height, /*sliceCount=*/1, mapping,
                                                   type, pixels, honorPackImageParams)) {
            return false;
        }

        MGLOG_D("Readback conversion: converted %s/%s from wide %s/%s", MG_Util::ConvertGLEnumToString(format).c_str(),
                MG_Util::ConvertGLEnumToString(type).c_str(), MG_Util::ConvertGLEnumToString(readFormat).c_str(),
                MG_Util::ConvertGLEnumToString(wideType).c_str());
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
    GLuint g_packedWordScratchTextureId = 0;
    GLsizei g_packedWordScratchWidth = 0;
    GLsizei g_packedWordScratchHeight = 0;

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


// Included in DirectGLES.cpp after the native packed-word readback helper.
// A storage promotion needs the driver's own texels as owned tight rows, not
// client PACK layout or a fallback into a frontend texture's potentially stale
// shadow.
    class ScopedWireTextureReadLevel {
    public:
        ScopedWireTextureReadLevel(GLuint texture, GLenum target, GLint level)
            : m_texture(texture), m_target(target) {
            switch (target) {
            case GL_TEXTURE_2D: m_binding = GL_TEXTURE_BINDING_2D; break;
            case GL_TEXTURE_3D: m_binding = GL_TEXTURE_BINDING_3D; break;
            case GL_TEXTURE_2D_ARRAY: m_binding = GL_TEXTURE_BINDING_2D_ARRAY; break;
            case GL_TEXTURE_CUBE_MAP: m_binding = GL_TEXTURE_BINDING_CUBE_MAP; break;
            case GL_TEXTURE_CUBE_MAP_ARRAY: m_binding = GL_TEXTURE_BINDING_CUBE_MAP_ARRAY; break;
            default: return;
            }
            GLint previous = 0;
            DrainESErrors();
            g_GLESFuncs.glGetIntegerv(m_binding, &previous);
            g_GLESFuncs.glBindTexture(m_target, m_texture);
            g_GLESFuncs.glGetTexParameteriv(m_target, GL_TEXTURE_BASE_LEVEL, &m_baseLevel);
            g_GLESFuncs.glGetTexParameteriv(m_target, GL_TEXTURE_MAX_LEVEL, &m_maxLevel);
            m_saved = g_GLESFuncs.glGetError() == GL_NO_ERROR;
            if (m_saved) {
                // ES framebuffer completeness can reject a defined level whose
                // base level or intervening mip chain is missing/inconsistent.
                // Read this existing native image as the sole base level. The
                // logical GL parameters and every cached copy remain unchanged.
                g_GLESFuncs.glTexParameteri(m_target, GL_TEXTURE_BASE_LEVEL, level);
                g_GLESFuncs.glTexParameteri(m_target, GL_TEXTURE_MAX_LEVEL, level);
                m_valid = g_GLESFuncs.glGetError() == GL_NO_ERROR;
            }
            g_GLESFuncs.glBindTexture(m_target, static_cast<GLuint>(previous));
        }

        ~ScopedWireTextureReadLevel() {
            if (!m_saved) return;
            // The packed-word path can use its own texture scratch binding.
            // Save the binding at restoration time too; do not assume an active
            // texture unit or make the backend's binding cache forget a bind.
            GLint previous = 0;
            g_GLESFuncs.glGetIntegerv(m_binding, &previous);
            g_GLESFuncs.glBindTexture(m_target, m_texture);
            g_GLESFuncs.glTexParameteri(m_target, GL_TEXTURE_BASE_LEVEL, m_baseLevel);
            g_GLESFuncs.glTexParameteri(m_target, GL_TEXTURE_MAX_LEVEL, m_maxLevel);
            g_GLESFuncs.glBindTexture(m_target, static_cast<GLuint>(previous));
        }

        Bool Valid() const { return m_valid; }

    private:
        GLuint m_texture;
        GLenum m_target;
        GLenum m_binding = GL_NONE;
        GLint m_baseLevel = 0, m_maxLevel = 0;
        Bool m_saved = false, m_valid = false;
    };

    static Bool ReadWireColorRowsTight(GLsizei width, GLsizei height, GLenum format, GLenum type,
                                      Bool forceOpaqueAlpha, const NormalizedImageCarrierRead& carrier,
                                      Vector<Uint8>& bytes) {
        ReadbackChannelMapping mapping{};
        if (!GetReadbackChannelMapping(format, mapping)) return false;
        const SizeT pixelBytes = GetReadbackDstPixelSize(mapping, type);
        if (!pixelBytes || width <= 0 || height <= 0) return false;
        const GLenum componentType = QueryReadAttachmentComponentType();
        const Bool integerAttachment = componentType == GL_INT || componentType == GL_UNSIGNED_INT;
        if (carrier.Active() ? (mapping.isInteger || !integerAttachment)
                             : mapping.isInteger != integerAttachment) return false;

        GLint implFormat = 0, implType = 0;
        g_GLESFuncs.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &implFormat);
        g_GLESFuncs.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &implType);
        struct Candidate { GLenum format, type; };
        Vector<Candidate> candidates;
        if (carrier.Active()) {
            candidates.push_back({GL_RGBA_INTEGER, GL_UNSIGNED_INT});
        } else if (mapping.isInteger) {
            if (GetWideReadChannelCount(implFormat) > 0 && IsIntegerReadFormat(implFormat) &&
                (implType == GL_INT || implType == GL_UNSIGNED_INT))
                candidates.push_back({static_cast<GLenum>(implFormat), static_cast<GLenum>(implType)});
            candidates.push_back({GL_RGBA_INTEGER, componentType == GL_INT ? GLenum(GL_INT) : GLenum(GL_UNSIGNED_INT)});
        } else {
            if (GetWideReadChannelCount(implFormat) > 0 && !IsIntegerReadFormat(implFormat) &&
                (CanDecodeWideSourceType(implType) ||
                 (implFormat == GL_RGBA && implType == GL_UNSIGNED_INT_2_10_10_10_REV)))
                candidates.push_back({static_cast<GLenum>(implFormat), static_cast<GLenum>(implType)});
            if (componentType == GL_FLOAT) candidates.push_back({GL_RGBA, GL_FLOAT});
            if (componentType == GL_SIGNED_NORMALIZED) {
                candidates.push_back({GL_RGBA, GL_SHORT});
                candidates.push_back({GL_RGBA, GL_BYTE});
            } else candidates.push_back({GL_RGBA, GL_UNSIGNED_BYTE});
        }
        ScopedPixelPackBuffer packBuffer(0);
        ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});
        const SizeT pixels = static_cast<SizeT>(width) * static_cast<SizeT>(height);
        Vector<Uint8> wide;
        GLenum wideType = GL_NONE;
        Int channels = 0;
        DrainESErrors();
        for (const auto candidate : candidates) {
            channels = GetWideReadChannelCount(candidate.format);
            wide.resize(pixels * static_cast<SizeT>(channels) * GetReadbackComponentSize(candidate.type));
            g_GLESFuncs.glReadPixels(0, 0, width, height, candidate.format, candidate.type, wide.data());
            if (g_GLESFuncs.glGetError() == GL_NO_ERROR) { wideType = candidate.type; break; }
        }
        if (wideType == GL_NONE) return false;

        if (carrier.Active()) {
            Vector<Uint8> decoded(pixels * 4 * sizeof(Float));
            auto* dst = reinterpret_cast<Float*>(decoded.data());
            const auto* src = reinterpret_cast<const Uint32*>(wide.data());
            for (SizeT i = 0; i < pixels; ++i) {
                for (SizeT c = 0; c < 4; ++c) {
                    const Float divisor = static_cast<Float>(carrier.ChannelMax[c]);
                    dst[i * 4 + c] = carrier.SignedNormalized
                        ? std::max(static_cast<Float>(static_cast<Int16>(static_cast<Uint16>(src[i * 4 + c]))) / divisor, -1.0f)
                        : static_cast<Float>(src[i * 4 + c]) / divisor;
                }
            }
            wide = Move(decoded); wideType = GL_FLOAT; channels = 4;
        } else if (wideType == GL_UNSIGNED_INT_2_10_10_10_REV) {
            Vector<Uint8> decoded(pixels * 4 * sizeof(Float));
            auto* dst = reinterpret_cast<Float*>(decoded.data());
            for (SizeT i = 0; i < pixels; ++i) {
                Uint32 word = 0;
                Memcpy(&word, wide.data() + i * sizeof(word), sizeof(word));
                dst[i * 4] = static_cast<Float>(word & 1023u) / 1023.0f;
                dst[i * 4 + 1] = static_cast<Float>((word >> 10) & 1023u) / 1023.0f;
                dst[i * 4 + 2] = static_cast<Float>((word >> 20) & 1023u) / 1023.0f;
                dst[i * 4 + 3] = static_cast<Float>((word >> 30) & 3u) / 3.0f;
            }
            wide = Move(decoded); wideType = GL_FLOAT; channels = 4;
        }
        if (channels < 4) ExpandNarrowWideRead(wide, pixels, channels, wideType);
        if (forceOpaqueAlpha) ForceWideReadAlphaToOne(wide, pixels, wideType);
        Vector<Uint8> converted(pixels * pixelBytes);
        const SizeT sourceRowBytes = static_cast<SizeT>(width) * 4 * GetReadbackComponentSize(wideType);
        for (GLsizei row = 0; row < height; ++row)
            ClientReadback::ConvertWideReadbackRow(wide.data() + static_cast<SizeT>(row) * sourceRowBytes,
                converted.data() + static_cast<SizeT>(row) * width * pixelBytes, width, wideType, mapping, type);
        bytes = Move(converted);
        return true;
    }

    static Bool ReadWireDepthStencilLevelTight(GLuint texture, TextureTarget target,
                                               TextureUploadTarget uploadTarget, GLint level,
                                               const IntVec3& extent, GLenum format, GLenum type,
                                               Vector<Uint8>& bytes) {
        const SizeT pixelBytes = MG_Util::GetInputBytesPerPixel(
            MG_Util::ConvertGLEnumToTextureInputFormat(format), MG_Util::ConvertGLEnumToTexturePixelDataType(type));
        if (!pixelBytes) return false;
        const Bool needDepth = format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL;
        const Bool needStencil = format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL;
        ScopedFramebufferBinding bindings(true, false);
        GLuint fbo = 0;
        g_GLESFuncs.glGenFramebuffers(1, &fbo);
        struct DeleteFbo { GLuint id; ~DeleteFbo() { g_GLESFuncs.glDeleteFramebuffers(1, &id); } } cleanup{fbo};
        FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, fbo);
        ScopedPixelPackBuffer packBuffer(0);
        ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});
        const GLenum nativeTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
        const GLenum point = needDepth ? (needStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT)
                                      : GL_STENCIL_ATTACHMENT;
        const SizeT count = static_cast<SizeT>(extent.x()) * extent.y();
        Vector<Uint8> result(count * extent.z() * pixelBytes);
        if (g_GLESFuncs.glMemoryBarrier) g_GLESFuncs.glMemoryBarrier(GL_ALL_BARRIER_BITS);
        g_GLESFuncs.glFinish();
        for (GLint z = 0; z < extent.z(); ++z) {
            if (nativeTarget == GL_TEXTURE_3D || nativeTarget == GL_TEXTURE_2D_ARRAY ||
                nativeTarget == GL_TEXTURE_CUBE_MAP_ARRAY)
                g_GLESFuncs.glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, point, texture, level, z);
            else
                g_GLESFuncs.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, point,
                    TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(uploadTarget), texture, level);
            g_GLESFuncs.glReadBuffer(GL_NONE);
            if (g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
            Vector<Float> depth;
            Vector<Uint8> stencil;
            if (needDepth && !ReadDepthValuesNative(0, 0, extent.x(), extent.y(), depth)) return false;
            if (needStencil && !ReadStencilBytesNative(0, 0, extent.x(), extent.y(), stencil)) return false;
            auto* dst = result.data() + count * z * pixelBytes;
            for (SizeT i = 0; i < count; ++i) {
                if (format == GL_DEPTH_STENCIL) {
                    const Uint32 s = stencil[i];
                    if (type == GL_UNSIGNED_INT_24_8) {
                        const Uint32 packed = (NormalizedDepthToUnsigned(depth[i], 16777215.0) << 8) | s;
                        Memcpy(dst + i * pixelBytes, &packed, sizeof(packed));
                    } else if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) {
                        Memcpy(dst + i * pixelBytes, &depth[i], sizeof(Float));
                        Memcpy(dst + i * pixelBytes + sizeof(Float), &s, sizeof(s));
                    } else return false;
                } else if (type == GL_FLOAT) {
                    const Float value = needDepth ? depth[i] : static_cast<Float>(stencil[i]);
                    Memcpy(dst + i * pixelBytes, &value, sizeof(value));
                } else if (type == GL_HALF_FLOAT) {
                    const Uint16 value = MG_Util::EncodeFloatToHalfBits(
                        needDepth ? depth[i] : static_cast<Float>(stencil[i]));
                    Memcpy(dst + i * pixelBytes, &value, sizeof(value));
                } else {
                    const Bool signedType = type == GL_BYTE || type == GL_SHORT || type == GL_INT;
                    const Double maximum = signedType
                        ? (pixelBytes == 1 ? 127.0 : pixelBytes == 2 ? 32767.0 : 2147483647.0)
                        : (pixelBytes == 1 ? 255.0 : pixelBytes == 2 ? 65535.0 : 4294967295.0);
                    const Uint32 value = needDepth ? NormalizedDepthToUnsigned(depth[i], maximum) : stencil[i];
                    if (pixelBytes == 1) dst[i] = static_cast<Uint8>(value);
                    else if (pixelBytes == 2) { const Uint16 v = static_cast<Uint16>(value); Memcpy(dst + i * 2, &v, 2); }
                    else if (pixelBytes == 4) Memcpy(dst + i * 4, &value, 4);
                    else return false;
                }
            }
        }
        bytes = Move(result);
        return true;
    }

    Bool ReadTextureLevelTight(GLuint texture, TextureTarget target, TextureUploadTarget uploadTarget,
                               TextureInternalFormat logicalFormat, GLint level, const IntVec3& logicalExtent,
                               Bool sourceUsesImageCarrier, GLenum format, GLenum type, Vector<Uint8>& bytes) {
        if (!texture || level < 0 || TextureImpl::IsMultisampleTextureTarget(target)) return false;
        const IntVec3 extent = TextureImpl::GetBackendUploadSize(target, logicalExtent);
        const GLenum nativeTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
        ScopedWireTextureReadLevel readLevel(texture, nativeTarget, level);
        if (!readLevel.Valid()) return false;
        if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL || format == GL_STENCIL_INDEX)
            return ReadWireDepthStencilLevelTight(texture, target, uploadTarget, level, extent, format, type, bytes);
        ReadbackChannelMapping mapping{};
        if (!GetReadbackChannelMapping(format, mapping)) return false;
        const SizeT pixelBytes = GetReadbackDstPixelSize(mapping, type);
        if (!pixelBytes || extent.x() <= 0 || extent.y() <= 0 || extent.z() <= 0) return false;
        const GLenum faceTarget = TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
        const auto widening = sourceUsesImageCarrier
            ? TextureImpl::GetImageBindableStorageWidening(logicalFormat) : TextureImpl::ImageBindableStorageWidening{};
        const SizeT sliceBytes = static_cast<SizeT>(extent.x()) * extent.y() * pixelBytes;
        Vector<Uint8> result(sliceBytes * static_cast<SizeT>(extent.z()));
        if (g_GLESFuncs.glMemoryBarrier) g_GLESFuncs.glMemoryBarrier(GL_ALL_BARRIER_BITS);
        g_GLESFuncs.glFinish();

        // A raw packed word may have redundant encodings or NaN payloads. Read
        // it through R32UI when the source really still uses that native layout.
        const Bool rawPacked = !widening && MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer(
            logicalFormat, MG_Util::ConvertGLEnumToTextureInputFormat(format),
            MG_Util::ConvertGLEnumToTexturePixelDataType(type));
        if (rawPacked && pixelBytes == sizeof(Uint32)) {
            const GLint face = uploadTarget >= TextureUploadTarget::CubeMapPositiveX &&
                               uploadTarget <= TextureUploadTarget::CubeMapNegativeZ
                ? static_cast<GLint>(uploadTarget) - static_cast<GLint>(TextureUploadTarget::CubeMapPositiveX) : 0;
            for (GLint layer = 0; layer < extent.z(); ++layer)
                if (!ReadPackedLevelWordsViaScratch(texture, nativeTarget, level, face + layer, extent.x(), extent.y(),
                        reinterpret_cast<Uint32*>(result.data() + static_cast<SizeT>(layer) * sliceBytes))) return false;
            bytes = Move(result);
            return true;
        }

        NormalizedImageCarrierRead carrier{};
        if (widening && widening.CarriesNormalizedCodes()) {
            for (SizeT c = 0; c < 4; ++c) carrier.ChannelMax[c] = widening.ChannelMax[c];
            carrier.SignedNormalized = widening.SignedNormalized;
        }
        TempFBOBinder binder(true);
        auto& framebuffer = binder.Framebuffer();
        for (GLint layer = 0; layer < extent.z(); ++layer) {
            if (nativeTarget == GL_TEXTURE_3D || nativeTarget == GL_TEXTURE_2D_ARRAY || nativeTarget == GL_TEXTURE_CUBE_MAP_ARRAY)
                ScratchFBOImpl::EnsureColorAttachmentLayer(framebuffer, GL_READ_FRAMEBUFFER, texture, level, layer);
            else
                ScratchFBOImpl::EnsureColorAttachment2D(framebuffer, GL_READ_FRAMEBUFFER, texture, faceTarget, level);
            ScratchFBOImpl::EnsureReadBuffer(framebuffer, GL_COLOR_ATTACHMENT0);
            const GLenum status = g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                MGLOG_E_ONCE("Espryt wire texture readback: framebuffer status=0x%x texture=%u level=%d target=0x%x",
                            status, texture, level, nativeTarget);
                return false;
            }
            Vector<Uint8> slice;
            if (!ReadWireColorRowsTight(extent.x(), extent.y(), format, type,
                    TextureImpl::BackendTextureFormatAddsAlpha(logicalFormat, target), carrier, slice) ||
                slice.size() != sliceBytes) return false;
            Memcpy(result.data() + static_cast<SizeT>(layer) * sliceBytes, slice.data(), sliceBytes);
        }
        bytes = Move(result);
        return true;
    }

    // P9 W2-a. The store's Defined-ness follows the client's respecifies; the driver's follows the
    // twin's last sync. A re-mint asks this before reading a level back, because a level defined
    // after that sync (null data, or null data plus a partial upload) is not on the driver yet and
    // its FBO attachment is incomplete - which used to end the session at
    // Fatal{ResourceUnavailable, "image-promotion-readback"} on legal GL.
    Bool NativeTextureLevelHasExtent(GLuint texture, TextureTarget target, TextureUploadTarget uploadTarget,
                                     GLint level, const IntVec3& logicalExtent) {
        if (!texture || level < 0 || !g_GLESFuncs.glGetTexLevelParameteriv) return true;
        const IntVec3 extent = TextureImpl::GetBackendUploadSize(target, logicalExtent);
        const GLenum nativeTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
        GLenum binding = GL_NONE;
        switch (nativeTarget) {
        case GL_TEXTURE_2D: binding = GL_TEXTURE_BINDING_2D; break;
        case GL_TEXTURE_3D: binding = GL_TEXTURE_BINDING_3D; break;
        case GL_TEXTURE_2D_ARRAY: binding = GL_TEXTURE_BINDING_2D_ARRAY; break;
        case GL_TEXTURE_CUBE_MAP: binding = GL_TEXTURE_BINDING_CUBE_MAP; break;
        case GL_TEXTURE_CUBE_MAP_ARRAY: binding = GL_TEXTURE_BINDING_CUBE_MAP_ARRAY; break;
        default: return true;
        }
        // A cube's levels are per face; every other target answers for the whole level.
        const GLenum queryTarget = nativeTarget == GL_TEXTURE_CUBE_MAP
            ? TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(uploadTarget) : nativeTarget;
        DrainESErrors();
        GLint previous = 0;
        g_GLESFuncs.glGetIntegerv(binding, &previous);
        g_GLESFuncs.glBindTexture(nativeTarget, texture);
        GLint width = 0, height = 0, depth = 0;
        g_GLESFuncs.glGetTexLevelParameteriv(queryTarget, level, GL_TEXTURE_WIDTH, &width);
        g_GLESFuncs.glGetTexLevelParameteriv(queryTarget, level, GL_TEXTURE_HEIGHT, &height);
        g_GLESFuncs.glGetTexLevelParameteriv(queryTarget, level, GL_TEXTURE_DEPTH, &depth);
        const Bool answered = g_GLESFuncs.glGetError() == GL_NO_ERROR;
        g_GLESFuncs.glBindTexture(nativeTarget, static_cast<GLuint>(previous));
        if (!answered) return true;
        const Bool layered = nativeTarget == GL_TEXTURE_3D || nativeTarget == GL_TEXTURE_2D_ARRAY ||
                             nativeTarget == GL_TEXTURE_CUBE_MAP_ARRAY;
        return width == extent.x() && height == extent.y() && (!layered || depth == extent.z());
    }

    // P8-E (docs/archive/notes/p8/E.md). MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=<w>x<h>, READ BY THE SERVER:
    // a colour level of exactly that extent is treated as one the driver refused to attach - what an
    // Adreno does for RGB16 and the 16-bit SNORM formats, and what no host driver can be made to do.
    // Scoped by the extent so the tcp lane's shared supervisor can carry it for the whole lane: only
    // the scenario that builds that extent (CopyImageStoreReadbackScenario) ever meets it.
    Bool WireTextureReadbackRefusedForTesting(const IntVec3& extent) {
        static const IntVec3 s_refused = [] {
            const char* value = std::getenv("MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT");
            if (value == nullptr) return IntVec3{0, 0, 0};
            char* end = nullptr;
            const long width = std::strtol(value, &end, 10);
            if (end == value || *end != 'x') return IntVec3{0, 0, 0};
            const long height = std::strtol(end + 1, nullptr, 10);
            if (width <= 0 || height <= 0) return IntVec3{0, 0, 0};
            return IntVec3{static_cast<Int>(width), static_cast<Int>(height), 1};
        }();
        return s_refused.x() > 0 && extent == s_refused;
    }

    // P8-E: THE STORE ROUTE, for a level the driver refused to read back. The server's staged store
    // is the only CPU copy of the level on this side, and it answers only while its bytes ARE the
    // level: every byte crossed (IsCovered), at the extent asked for, and the texture was never
    // written by the driver except by copies the store followed (!IsDriverWritten - see
    // StagedTextureStore's P8-E block and FollowCopyImageInStagedStore). The conversion is
    // GetTexImageViaShadowConversion's, into tight rows: a packed format read with its own type
    // takes the stored words verbatim, everything else decodes to wide RGBA and converts row by row.
    // Anything else DECLINES BY NAME: the reply is an error, and the client's
    // Fatal{ReplyError, "ReadPixels"} ends the session instead of handing back bytes the driver has
    // since overwritten.
    static Bool ReadWireLevelFromStagedStore(MG_Pipe::MGPipeHandle res, TextureInternalFormat logicalFormat,
                                             TextureUploadTarget uploadTarget, GLint level, const IntVec3& extent,
                                             GLenum format, GLenum type, Vector<Uint8>& bytes) {
        auto& store = MG_Record::ServerStagedTexture();
        const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(res);
        const auto target = static_cast<Uint16>(uploadTarget);
        const auto mip = static_cast<Uint16>(level);
        ReadbackChannelMapping mapping{};
        const SizeT pixelBytes = GetReadbackChannelMapping(format, mapping) ? GetReadbackDstPixelSize(mapping, type) : 0;
        const SizeT texels = static_cast<SizeT>(std::max(extent.x(), 0)) * static_cast<SizeT>(std::max(extent.y(), 0)) *
                             static_cast<SizeT>(std::max(extent.z(), 0));
        const char* declined = nullptr;
        if (!store.Holds()) {
            declined = "this arm keeps no staged store";
        } else if (store.IsDriverWritten(key)) {
            declined = "the driver has written the texture since its bytes crossed";
        } else if (!store.IsCovered(key, target, mip)) {
            declined = "not every byte of the level crossed";
        } else if (store.LevelExtentOrUndefined(key, target, mip) != extent) {
            declined = "the staged level is not the extent asked for";
        } else if (pixelBytes == 0 || texels == 0) {
            declined = "the format/type pair has no readback mapping";
        }
        Vector<Uint8> out;
        if (declined == nullptr) {
            const Uint8* staged = store.RequireLevelBytes(key, target, mip, "texture-readback-from-store");
            if (MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer(
                    logicalFormat, MG_Util::ConvertGLEnumToTextureInputFormat(format),
                    MG_Util::ConvertGLEnumToTexturePixelDataType(type))) {
                if (store.LevelByteSize(key, target, mip) != texels * pixelBytes) {
                    declined = "the staged words are not the client's word size";
                } else {
                    out.assign(staged, staged + texels * pixelBytes);
                }
            } else {
                Vector<Uint8> wide;
                Bool isInteger = false;
                Bool isSigned = false;
                if (!MG_Util::PixelStoreProcessor::DecodeShadowDataToWideRGBA(logicalFormat, staged, texels, wide,
                                                                             isInteger, isSigned)) {
                    declined = "the staged format has no wide decoding";
                } else if (mapping.isInteger != isInteger) {
                    declined = "an integer read of a normalized level, or the reverse";
                } else {
                    const GLenum wideType = isInteger ? (isSigned ? GL_INT : GL_UNSIGNED_INT) : GL_FLOAT;
                    const SizeT rowTexels = static_cast<SizeT>(extent.x());
                    const SizeT rows = texels / rowTexels;
                    const SizeT wideRowBytes = rowTexels * 4 * GetReadbackComponentSize(wideType);
                    out.resize(texels * pixelBytes);
                    for (SizeT row = 0; row < rows; ++row) {
                        ClientReadback::ConvertWideReadbackRow(wide.data() + row * wideRowBytes,
                                                             out.data() + row * rowTexels * pixelBytes, rowTexels,
                                                             wideType, mapping, type);
                    }
                }
            }
        }
        if (declined != nullptr) {
            MGLOG_E_ONCE("MGPipe: texture {%u,%u} level %d: the driver refused the readback and the staged store "
                         "cannot answer it (%s) - DECLINED (texture-readback-store-declined)",
                         res.Slot, res.Gen, level, declined);
            return false;
        }
        MGLOG_W_ONCE("MGPipe: texture {%u,%u} level %d: the driver refused the readback (internal format %u); "
                     "answered from the server's staged store (texture-readback-from-store)",
                     res.Slot, res.Gen, level, static_cast<unsigned>(logicalFormat));
        bytes = Move(out);
        return true;
    }

    Bool ReadTextureImageWire(const MG_Pipe::MGPReadbackInfo& info, Vector<Uint8>& bytes) {
        const auto* record = PipeTextureRecordForHandle(info.Res);
        if (!record) return false;
        TextureTarget target = TextureTarget::Unknown;
        for (Int i = 0; i < static_cast<Int>(TextureTarget::TextureTargetCount); ++i)
            if (MG_Pipe::MGPipeResourceTargetForTextureTarget(static_cast<TextureTarget>(i)) == record->Desc.Target)
                target = static_cast<TextureTarget>(i);
        if (target == TextureTarget::Unknown) return false;
        const auto twin = TextureImpl::SyncTextureToBackendByHandle(info.Res);
        if (!twin) return false;
        const Bool wholeCube = info.Target == GL_TEXTURE_CUBE_MAP;
        const IntVec3 extent{static_cast<Int>(info.Box.W), static_cast<Int>(info.Box.H),
                            wholeCube ? 1 : static_cast<Int>(info.Box.D)};
        const auto logicalFormat = static_cast<TextureInternalFormat>(record->Desc.InternalFormat);
        // The R32UI word leg never goes through a framebuffer, so the knob leaves it alone (it is
        // ReadTextureLevelTight's first choice for exactly these reads).
        const Bool packedWordRead =
            !twin->RequiresImageBindableStorage() &&
            MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer(
                logicalFormat, MG_Util::ConvertGLEnumToTextureInputFormat(info.Format),
                MG_Util::ConvertGLEnumToTexturePixelDataType(info.Type)) &&
            MG_Util::GetInputBytesPerPixel(MG_Util::ConvertGLEnumToTextureInputFormat(info.Format),
                                           MG_Util::ConvertGLEnumToTexturePixelDataType(info.Type)) == sizeof(Uint32);
        const Bool refusedForTesting = !packedWordRead && WireTextureReadbackRefusedForTesting(extent);
        Vector<Uint8> result;
        for (Int face = 0; face < (wholeCube ? 6 : 1); ++face) {
            const auto uploadTarget = wholeCube
                ? static_cast<TextureUploadTarget>(static_cast<Int>(TextureUploadTarget::CubeMapPositiveX) + face)
                : MG_Util::ConvertGLEnumToTextureUploadTarget(info.Target);
            Vector<Uint8> slice;
            if (refusedForTesting ||
                !ReadTextureLevelTight(twin->GetBackendTextureId(), target, uploadTarget, logicalFormat, info.Level,
                    extent, twin->RequiresImageBindableStorage(), info.Format, info.Type, slice)) {
                if (!ReadWireLevelFromStagedStore(info.Res, logicalFormat, uploadTarget, info.Level, extent,
                                                  info.Format, info.Type, slice)) return false;
            }
            result.insert(result.end(), slice.begin(), slice.end());
        }
        bytes = Move(result);
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
    // The signed widths and GL_HALF_FLOAT belong in it for the same reason: the helpers scale a
    // depth into 127 / 32767 / 2147483647 for the signed ones and hand GL_HALF_FLOAT the value
    // as a half word, so a pair listed here is one they answer in full.
    static Bool IsSupportedDepthStencilReadPixelsPair(GLenum format, GLenum type) {
        switch (format) {
        case GL_DEPTH_COMPONENT:
            return type == GL_BYTE || type == GL_UNSIGNED_BYTE || type == GL_SHORT ||
                   type == GL_UNSIGNED_SHORT || type == GL_INT || type == GL_UNSIGNED_INT ||
                   type == GL_FLOAT || type == GL_HALF_FLOAT;
        case GL_STENCIL_INDEX:
            return type == GL_UNSIGNED_BYTE || type == GL_BYTE || type == GL_UNSIGNED_SHORT || type == GL_SHORT ||
                   type == GL_UNSIGNED_INT || type == GL_INT || type == GL_FLOAT || type == GL_HALF_FLOAT;
        case GL_DEPTH_STENCIL:
            return type == GL_UNSIGNED_INT_24_8 || type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        default:
            return false;
        }
    }

    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        MGLOG_D("ReadPixels: x=%d y=%d w=%d h=%d format=%s type=%s pixels=%p", x, y, width, height,
                MG_Util::ConvertGLEnumToString(format).c_str(), MG_Util::ConvertGLEnumToString(type).c_str(), pixels);
#if MOBILEGL_BUILD_DISAGGREGATED
        // A reset context reads back nothing it rendered: the session is latched and the read declined.
        if (LatchIfDeviceLost("read-pixels")) return;
#endif

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
        const Bool packSwapBytes = MG_Pipe::gPipeInputs->GetPixelStoreParameters(false).SwapBytes;
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
            SplitReadbackPackBuffer();
        Bool usePBO = false;
        GLuint packBufferId = 0;
        // P13: SplitReadbackPackBuffer() is always null - a pack buffer reaches the readback
        // by handle on the verb path - so this entry never packs into a buffer.
        MGLOG_D("ReadPixels: Not using PBO");
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

    void SetEGLFuncsTable(const MG_External::EGLFunctionsTable& eglFuncs) {
        g_EGLFuncs = eglFuncs;
    }

    void SetGLESFuncsTable(const MG_External::GLESFunctionsTable& glesFuncs) {
        g_GLESFuncs = glesFuncs;
    }

    void SetGLESCapabilities(const MG_External::GLESCapabilities& capabilities) {
        g_GLESCapabilities = capabilities;
    }
} // namespace MobileGL::MG_Backend::DirectGLES
