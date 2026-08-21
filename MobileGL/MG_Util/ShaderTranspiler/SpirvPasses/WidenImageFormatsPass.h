// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/WidenImageFormatsPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "spirv-tools/optimizer.hpp"
#include "source/opt/pass.h"

#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Emulates the desktop-GL image formats GLSL ES cannot spell by CHANNEL WIDENING: a
            // storage image DECLARED `layout(rg32f)` is re-declared `layout(rgba32f)` and every
            // access through it is masked back to the two channels GL says it has.
            //
            // WHY IT IS NEEDED AT ALL. GL 4.2 has forty image formats; GLSL ES 3.1 has thirteen,
            // and GL_NV_image_formats - the only extension that adds the rest - is advertised by
            // none of Adreno 830, Mali-G1-Ultra MC12 or Mali-G925-Immortalis MC12 (probed on all
            // three, with `#extension ... : enable` also rejected, so "the driver implements it
            // unadvertised" is refuted rather than assumed). A shader that declares one of the
            // other twenty-six therefore has NO legal ESSL spelling, and it fails in one of two
            // ways: SPIRV-Cross throws for its is_desktop_only_format set and no text is produced
            // at all, or the token reaches the driver and is rejected ("'rg32f' : not a legal
            // layout qualifier id"). Either way the stage is lost, the backend program is
            // unusable, and every draw with it silently renders nothing while the frontend keeps
            // reporting GL_LINK_STATUS = TRUE. Dropping the qualifier instead is not an escape:
            // all three drivers reject a format-LESS image declaration outright ("all images have
            // to define layout format" / "S0001: Image must specify a format layout qualifier"),
            // readonly and writeonly alike, at both #version 310 es and 320 es. And unlike a
            // numeric limit there is nothing honest to report either - GL has no "this image
            // format is unsupported" query - so the format has to be emulated.
            //
            // WHAT WIDENING MEANS. Seventeen of the twenty-six have a core ESSL format of the
            // SAME PER-CHANNEL WIDTH AND COMPONENT TYPE, differing only in channel count
            // (rg32f -> rgba32f, r8ui -> rgba8ui, rg8_snorm -> rgba8_snorm, ...). Carried in one
            // of those the emulation is EXACT, not approximate: every value is representable bit
            // for bit, and GL's own image semantics do the rest -
            //
            //   * imageLoad on a format with fewer than four channels returns (r, 0, 0, 1);
            //   * imageStore drops the components the format does not have.
            //
            // so the two surplus channels of the carrier are not free storage, they are values GL
            // already defines. This pass pins them: every OpImageWrite through a widened image has
            // its texel replaced by (r[, g[, b]], 0.., 1) and every OpImageRead has its result
            // masked the same way. Masking BOTH is deliberate belt and braces - the write mask
            // alone keeps the storage canonical for a sampler and for glGetTexImage, the read mask
            // alone survives storage this shader never wrote (glTexStorage with no upload, whose
            // surplus channels are undefined).
            //
            // The other NINE (r11f_g11f_b10f, rgb10_a2, rgb10_a2ui, rgba16, rg16, r16,
            // rgba16_snorm, rg16_snorm, r16_snorm) have NO same-width core carrier and are
            // deliberately NOT widened here: every carrier for them is either lossy or changes the
            // numeric domain of the texture a `sampler2D` would read from it. They keep the honest
            // "no GLSL ES spelling" diagnostic instead of silently changing an application's
            // quantisation behaviour.
            //
            // MUST MOVE WITH THE OTHER TWO LAYERS. The widening is not a shader-local rewrite: the
            // ES texture behind the image has to be allocated in the carrier format too, and
            // glBindImageTexture has to be handed the carrier (on Adreno the bind of the narrow
            // format is GL_INVALID_VALUE for nineteen of the twenty-six, and on both Malis for
            // twenty-five). Both are done in DirectGLES against the same table below, so the two
            // sides agree by construction rather than by convention. Binding a narrow texture
            // through a wide image is NOT an option: every tested driver accepts it silently, so
            // it reads and writes out of bounds undetected.
            //
            // ESSL ONLY. DirectVulkan takes the declared format natively and resolves the view
            // format from the same bind state, so the module must reach it unchanged.
            class WidenImageFormatsPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-widen-image-formats"; }
                Status Process() override;

                // Whether the module declares a storage image whose format this pass would widen,
                // i.e. whether running it could change anything. Answered from a single parse so
                // the caller can skip the optimizer run entirely - which is every shader but a
                // handful.
                static bool DeclaresWidenableImageFormat(const Vector<Uint32>& binary);
                // The same question asked of a module the caller has ALREADY parsed, so a stage
                // that has to answer several gate questions pays one BuildModule rather than one
                // per gate - see ShaderCompiler::ProbeSpirvGateFeatures, and the ~10% it cost
                // compile-heavy CTS cases when two gates each parsed for themselves.
                static bool DeclaresWidenableImageFormat(spvtools::opt::IRContext* context);

                // The core-ESSL GL internal format that carries `glInternalFormat` exactly, or 0
                // when the format needs no widening (it is core already) or cannot be widened
                // exactly (the nine above, and anything that is not an image format at all).
                // Used by DirectGLES for the texture storage and the glBindImageTexture argument,
                // so that all three layers pick the same carrier.
                static Uint WidenedCoreEsslImageFormat(Uint glInternalFormat);

                // Channels the GL internal format really has (1-4), or 0 when it is not one of the
                // forty image formats. The count the widened accesses are masked back to.
                static Uint ImageFormatChannelCount(Uint glInternalFormat);

                static spvtools::Optimizer::PassToken CreateWidenImageFormatsPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
