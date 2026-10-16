// MobileGL - MobileGL/MG_Impl/Pipe/ImageEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// Client side of set_shader_images, under SamplerEmit.h's subsystem bit (one family, one A/B).
// Two invariants an optimisation must keep. An image window of 0 emits nothing before any hash,
// so a draw that uses no images pays one integer test. The shutter gating the sweep mixes only
// frontend counters (texture content and params, program image units, texture binds), never a
// backend re-mint counter: a texture bound only to an image unit is re-minted inside the sweep,
// after the gate has decided. The record carries the application's format and access; the
// bind-format recast and buffer-texture split view stay on the server, so the content hash
// covers InternalFormat and Access too.
#include <MG_Impl/Pipe/SamplerEmit.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeRoute.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <xxhash.h>

namespace MobileGL::MG_Pipe {

    // GL_READ_ONLY / GL_WRITE_ONLY / GL_READ_WRITE folded into the one byte the wire carries.
    // A value the enum does not name would otherwise truncate silently into a Uint8, which is
    // the class of bug the descriptors exist to close.
    //
    // P5e (CONTRACT-P5E.md §1, ruling 16) MOVED THE NUMBERS OUT FROM UNDER THIS FUNCTION and
    // left the GLenum half here, where the GLenum is. The three values are
    // MGPipeValueTypes.h's MGPipeImageAccess, which the server decodes through, so the encode
    // and the decode are one table instead of two literals that happened to agree. It is also
    // why the function is a free function now rather than a private static: the unit case that
    // pins the three constants has to reach BOTH halves (MG_Test/Pipe/ImageEmitTest.cpp).
    inline Uint32 MGPipeEncodeImageAccess(GLenum access) {
        switch (access) {
        case GL_READ_ONLY:
            return kMGPipeImageAccessReadOnly;
        case GL_WRITE_ONLY:
            return kMGPipeImageAccessWriteOnly;
        case GL_READ_WRITE:
            return kMGPipeImageAccessReadWrite;
        default:
            MOBILEGL_ASSERT(false, "glBindImageTexture access 0x%x is not one of the three GL names",
                            static_cast<Uint>(access));
            return kMGPipeImageAccessReadOnly;
        }
    }

    // GL 4.6 core 8.26: an image access is INVALID - loads return zero, stores are dropped - when
    // the bound texture is incomplete or the bound level lies outside [level_base, q]. Image
    // units ignore the sampler, so "complete" here is the level's own: the base level needs
    // base-level completeness and any other level a complete mip chain up to it, the reading
    // the conformance suite applies (a mutable texture with only level 0 defined, bound at
    // level 2). Such a binding travels with its texture and level kMGPipeImageLevelNone, which
    // both backends answer with a stand-in of the texture's shape that reads zero - an ES driver
    // would otherwise happily address the undefined level, and an EMPTY ES unit reads alpha 1 on
    // Adreno where GL says 0.
    inline Bool MGPipeImageBindingIsAccessible(const MG_State::GLState::ImageTextureBinding& binding) {
        using namespace MG_State::GLState;
        const ITextureObject* texture = binding.Texture.get();
        if (texture == nullptr) return false;
        const TextureObjectMipmap* mipmapTexture = AsMipmapTexture(texture);
        // Buffer and multisample storage have exactly one level and no level range to leave.
        if (mipmapTexture == nullptr) return true;
        if (binding.Level < 0) return false;
        const Uint level = static_cast<Uint>(binding.Level);
        const UintVec2& range = texture->GetLevelRange();
        if (level < range.x() || level > range.y()) return false;
        if (texture->IsImmutable() || texture->IsTextureView()) {
            // Every level of immutable storage exists, so the range against the storage is all.
            return level < texture->GetAddressableLevelCount();
        }
        if (level >= mipmapTexture->GetMipmapLevelCount()) return false;
        for (const auto uploadTarget : texture->GetUploadTargets()) {
            const IntVec3 size = mipmapTexture->GetMipmapTexelSize(uploadTarget, level);
            if (size.x() <= 0 || size.y() <= 0 || size.z() <= 0) return false;
        }
        return IsMipmapCompleteForFilter(texture, /*mipmapped=*/level != range.x());
    }

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

        // set_shader_images. Start is 0; Count is the image-unit window: the highest image unit
        // the current draw program names (memoised in SamplerEmit.h's program inversion),
        // unioned with a sticky mark of every unit already described. TextureState's
        // touched-unit mark is not used: it counts units holding a binding, not units a shader
        // reads, and would defeat the zero early-out for an application that binds an image no
        // shader names. The window only grows, so a unit a later program stops naming is still
        // described and a stale binding stays visible to the server.
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
                entry.Res = binding.Texture != nullptr
                                ? MGPipeSlots().Acquire(MGPipeKind::Texture, binding.Texture->GetLifetimeId())
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
                entry.Level = MGPipeImageBindingIsAccessible(binding) ? static_cast<Uint16>(binding.Level)
                                                                       : kMGPipeImageLevelNone;
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
            MGPipeRouteSetShaderImages(m_lastImages, m_entries.data());
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
