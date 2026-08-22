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
            // r11f_g11f_b10f has no same-width core carrier either, and takes rgba16f anyway,
            // because that carrier is still LOSSLESS: 11f is e5m6 and 10f is e5m5 against a half's
            // s1e5m10 - the SAME 5-bit exponent with a strictly longer mantissa - so every value
            // the packed format can hold has an exact half. Only the reverse direction differs
            // (the carrier also holds negatives, which 11f and 10f cannot sign, and mantissa bits
            // finer than the 6 and 5 they quantise to, so a value written through the image and
            // then SAMPLED lands on half's grid rather than the packed format's). That is measured
            // against the alternative, which is not a truer quantisation but no program at all:
            // the SPIRV-Cross throw takes the whole stage, every image uniform declared beside it
            // included.
            //
            // rgb10_a2ui takes rgba16ui for a simpler reason still: its channels are 10, 10, 10 and
            // 2 bits of UNSIGNED INTEGER, and rgba16ui gives each of them sixteen. Same component
            // type, same channel COUNT, every value representable - so no access is rewritten at
            // all, and only the TRANSFER differs (its shadow is one packed 32-bit word per texel,
            // which the upload splits into four shorts).
            //
            // The other SEVEN (rgb10_a2, rgba16, rg16, r16, rgba16_snorm, rg16_snorm, r16_snorm)
            // are deliberately NOT widened here: core ESSL has no 16-bit normalized format at all
            // and no 10-bit one, so every carrier for them either loses range or changes the
            // component TYPE the texture a `sampler2D` would read presents. They keep the honest
            // "no GLSL ES spelling" diagnostic instead of silently changing an application's
            // numeric domain.
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
                // `onlyFormatsSpirvCrossRefusesToPrint` narrows the pass to the formats that have
                // no ESSL route even on a driver that DOES advertise GL_NV_image_formats.
                // SPIRV-Cross's is_desktop_only_format set - r8ui, rg16f, r16i and fifteen others -
                // makes it THROW for an ESSL target rather than print a token, and the throw takes
                // the stage with it whatever the driver could have accepted. Mesa is exactly that
                // case: it advertises the extension, so nothing else needs widening there, and
                // `layout(r8ui) uimage2D` still lost its whole program until this ran for it.
                //
                // Off, the pass widens every format in the table, which is what a driver without
                // the extension needs. The caller sets it from
                // g_GLESCapabilities.SupportsExtendedImageFormats, and the SAME rule decides
                // whether the ES texture storage and the glBindImageTexture argument widen
                // (TextureImpl::GetImageBindableStorageWidening) - all three have to agree or the
                // shader addresses a texel size the storage does not have.
                explicit WidenImageFormatsPass(bool onlyFormatsSpirvCrossRefusesToPrint = false)
                    : m_onlyFormatsSpirvCrossRefusesToPrint(onlyFormatsSpirvCrossRefusesToPrint) {}

                const char* name() const override { return "mobilegl-widen-image-formats"; }
                Status Process() override;

                // Whether the module declares a storage image whose format this pass would widen,
                // i.e. whether running it could change anything. Answered from a single parse so
                // the caller can skip the optimizer run entirely - which is every shader but a
                // handful. `onlyFormatsSpirvCrossRefusesToPrint` must match what the run will use,
                // or the gate answers a question the pass is not being asked.
                static bool DeclaresWidenableImageFormat(const Vector<Uint32>& binary,
                                                         bool onlyFormatsSpirvCrossRefusesToPrint = false);
                // The same question asked of a module the caller has ALREADY parsed, so a stage
                // that has to answer several gate questions pays one BuildModule rather than one
                // per gate - see ShaderCompiler::ProbeSpirvGateFeatures, and the ~10% it cost
                // compile-heavy CTS cases when two gates each parsed for themselves.
                static bool DeclaresWidenableImageFormat(spvtools::opt::IRContext* context,
                                                         bool onlyFormatsSpirvCrossRefusesToPrint = false);

                // The core-ESSL GL internal format that carries `glInternalFormat` exactly, or 0
                // when the format needs no widening (it is core already) or cannot be widened
                // exactly (the nine above, and anything that is not an image format at all).
                // Used by DirectGLES for the texture storage and the glBindImageTexture argument,
                // so that all three layers pick the same carrier.
                static Uint WidenedCoreEsslImageFormat(Uint glInternalFormat);

                // Channels the GL internal format really has (1-4), or 0 when it is not one of the
                // forty image formats. The count the widened accesses are masked back to.
                static Uint ImageFormatChannelCount(Uint glInternalFormat);

                // Whether the carrier holds this format's channels as the INTEGER CODES of a
                // NORMALIZED value rather than as the values themselves - true for the seven
                // 16-bit and 10-bit normalized formats and nothing else. `outChannelMax` takes the
                // largest code each channel can hold (2^b - 1 unsigned, 2^(b-1) - 1 signed), which
                // is the denominator of GL 4.6 2.3.5 for that channel; `outSignedNormalized` says
                // which of the two conversions applies.
                //
                // DirectGLES asks this on both sides of the transfer: the upload pads a missing
                // alpha with outChannelMax[3] rather than the transfer type's own 1 (through a
                // uint carrier "one" is the saturated CODE, not the integer one), and
                // glGetTexImage divides the codes back out, because the ES storage is an integer
                // texture the client still expects to read as floats.
                static bool NormalizedImageCarrierCodes(Uint glInternalFormat, Uint32 (&outChannelMax)[4],
                                                        bool& outSignedNormalized);

                static spvtools::Optimizer::PassToken CreateWidenImageFormatsPass(
                    bool onlyFormatsSpirvCrossRefusesToPrint = false);

            private:
                bool m_onlyFormatsSpirvCrossRefusesToPrint = false;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
