// MobileGL - MobileGL/MG_Impl/Pipe/ImageEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of set_shader_images, the third of P4a's kVarTail unit sets. It rides
// SamplerEmit.h's subsystem bit (kMGPipeWiredSamplerSubsystem): one family, one A/B.
//
// TWO INVARIANTS THAT MUST SURVIVE INTO THE BODY, and they are the kind an optimisation
// deletes:
//   1. THE HIGH-WATER-ZERO EARLY-OUT. An image high-water mark of 0 emits nothing, BEFORE any
//      hash - that is what makes every Minecraft draw pay one integer test for a feature it
//      does not use.
//   2. THE SWEEP'S GATE IS KEYED ON FRONTEND GENERATIONS AND DELIBERATELY NOT ON A BACKEND
//      RE-MINT COUNTER. A texture bound ONLY to an image unit is re-minted INSIDE the sweep,
//      so a server-side epoch would be bumped after the gate had already declined. The
//      client's bit-14 shutter is Mix(Mix(textureContent, textureParams), programImageUnitVersion)
//      - all three FRONTEND counters - so the property is preserved by construction, and it is
//      written here because it is invisible from the shutter itself.
//
// The record carries the APPLICATION's format and access; the bind-format recast (a GL_RG32F
// bind is INVALID_VALUE on 19 of 26 non-core formats on Adreno) and the buffer-texture split
// view stay SERVER-side and unchanged. ContentHash therefore has to cover InternalFormat and
// Access as well as the binding, because the format the shader was built against is live
// glBindImageTexture state and the format-less image bake keys on it.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT - see
// FramebufferEmit.h for why, in full.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SamplerEmit.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <xxhash.h>

namespace MobileGL::MG_Pipe {

    // D-G3. Over the tail with Start and Count mixed in, the same shape the two sampler sets
    // use - and it covers InternalFormat and Access because those are live glBindImageTexture
    // state that the format-less image bake keys on, not decoration.
    inline Uint64 MGPipeShaderImageSetContentHash(const MGPImageView* entries, Uint32 start, Uint32 count) {
        Uint64 hash = XXH64(entries, static_cast<SizeT>(count) * sizeof(MGPImageView), 0);
        hash = MGPipeMixShutter(hash, start);
        hash = MGPipeMixShutter(hash, count);
        return hash;
    }

    class MGPipeImageEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;

        // set_shader_images. Start is 0 and Count is the image-unit window described below.
        //
        // WHERE THE HIGH-WATER MARK COMES FROM, because the frontend has none and this is the
        // one place a reader will look for it. DirectGLES keeps g_imageUnitHighWaterMark, but
        // that is written from inside its own per-unit sync and lives on the far side of the
        // boundary; TextureState::NoteUnitTouched is the TEXTURE-unit path and
        // glBindImageTexture does not reach it. Adding a counter to TextureState would edit
        // another package's file and resize the pull build's object, which G1 forbids outright.
        //
        // So the window is derived instead, from the one thing that decides whether an image
        // unit can matter at all: the highest image unit the CURRENT PROGRAM names, memoised
        // per program state in SamplerEmit.h's shared inversion, UNIONED with a sticky mark of
        // every unit this emitter has already described. A program with no image uniforms
        // gives MaxImageUnit == -1 and, with nothing sticky yet, a window of 0 - which is the
        // zero early-out, taken BEFORE any hash and before any 192-entry walk, exactly as
        // property 1 requires. The mark is sticky so that a program which stops naming a unit
        // does not silently stop describing it: the window only grows, and shrinking it is how
        // a stale binding would become invisible to the server.
        Uint64 EmitShaderImages(GLContext& ctx) {
            const auto& program = ctx.GetProgramForDraw();
            const auto& resolution = MGPipeProgramOpaqueUnitsShared().For(program.get());
            const Uint32 programWindow =
                resolution.MaxImageUnit < 0 ? 0u : static_cast<Uint32>(resolution.MaxImageUnit) + 1u;
            if (programWindow > m_window) m_window = programWindow;
            const Uint32 count = m_window < kMGPipeMaxImageUnits ? m_window : kMGPipeMaxImageUnits;
            // PROPERTY 1, and it is one integer test on every draw of every application that
            // never binds an image.
            if (count == 0) return 0;

            for (Uint32 unit = 0; unit < count; ++unit) {
                const auto& binding = ctx.GetImageTextureBinding(static_cast<Int>(unit));
                MGPImageView& entry = m_entries[unit];
                entry = MGPImageView{};
                entry.Unit = unit;
                entry.Res = binding.Texture ? MGPipeSlots().Acquire(MGPipeKind::Texture,
                                                                    binding.Texture->GetLifetimeId())
                                            : kMGPipeNullHandle;
                // D-A4: a texture named in an emitted MGPImageView is SHADER-IMAGE-bound from
                // then on - the bit ImageBindableHint is derived from. The bind itself noted it
                // first (TextureState.h, so the hint precedes the first sync); this is the
                // letter of the rule and a one-compare early-out once the bit is set.
                if (!MGPipeHandleIsNull(entry.Res)) {
                    MGPipeNoteTextureBoundAs(entry.Res, static_cast<Uint32>(kMGPipeBindShaderImage));
                }
                // THE APPLICATION's format and access, verbatim. The bind-format recast and the
                // buffer-texture split view are server-side and stay there; so does
                // SupportsLayeredImageBinding's rule, which asks the BACKEND target after
                // MapToBackendTextureTarget and forces layer to 0 for a non-layerable one -
                // Adreno took a stray layer index literally. A client that pre-applied any of
                // that would be answering a driver question from the wrong side.
                entry.InternalFormat = static_cast<Uint32>(binding.Format);
                entry.Layer = static_cast<Uint32>(binding.Layer);
                entry.Level = static_cast<Uint16>(binding.Level);
                entry.Layered = binding.Layered != GL_FALSE ? 1 : 0;
                entry.Access = static_cast<Uint8>(MGPipeEncodeImageAccess(binding.Access));
            }

            const Uint64 hash = MGPipeShaderImageSetContentHash(m_entries.data(), 0, count);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::SetShaderImages, hash)) {
                return 0;
            }
            m_lastImages = MGPShaderImages{};
            m_lastImages.Start = 0;
            m_lastImages.Count = count;
            m_lastImages.ContentHash = hash;
            MGPipeApplySetShaderImages(m_lastImages, m_entries.data());
            ++m_imageSets;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::ShaderImageEmissions, 1);
            }
            return sizeof(MGPShaderImages) + static_cast<Uint64>(count) * sizeof(MGPImageView);
        }

        // The validate point's FreshlyPrimed arm. A fresh context is a fresh set of image
        // bindings, so the sticky window starts over; the suppressor slot this set latches is
        // invalidated beside this call. There is no record half here at all - set_shader_images
        // is pure working state and mints no object of its own.
        void Reset() { m_window = 0; }

        void ResetCounters() { m_imageSets = 0; }

        const MGPShaderImages& LastShaderImages() const { return m_lastImages; }
        const Array<MGPImageView, kMGPipeMaxImageUnits>& LastImageViews() const { return m_entries; }
        Uint64 ImageSetCount() const { return m_imageSets; }
        Uint32 Window() const { return m_window; }

    private:
        // GL_READ_ONLY / GL_WRITE_ONLY / GL_READ_WRITE folded into the one byte the wire
        // carries. A value the enum does not name would otherwise truncate silently into a
        // Uint8, which is the class of bug the descriptors exist to close.
        static Uint32 MGPipeEncodeImageAccess(GLenum access) {
            switch (access) {
            case GL_READ_ONLY:
                return 0;
            case GL_WRITE_ONLY:
                return 1;
            case GL_READ_WRITE:
                return 2;
            default:
                MOBILEGL_ASSERT(false, "glBindImageTexture access 0x%x is not one of the three GL names",
                                static_cast<Uint>(access));
                return 0;
            }
        }

        Array<MGPImageView, kMGPipeMaxImageUnits> m_entries{};
        MGPShaderImages m_lastImages{};
        Uint32 m_window = 0;
        Uint64 m_imageSets = 0;
    };

    inline MGPipeImageEmitter& MGPipeImageEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason; heap-constructed and
        // intentionally leaked at exit, like every other MGPipe process singleton.
        static MGPipeImageEmitter* emitter = new MGPipeImageEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
