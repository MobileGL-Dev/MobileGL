// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/BakeImageFormatsPass.h
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
            // Gives every format-less storage image in the module the format the application
            // bound to its image unit, so SPIRV-Cross can print a format layout qualifier ESSL
            // demands and desktop GLSL does not.
            //
            // Desktop GLSL 4.2 lets a `writeonly` (or `readonly`) image declaration omit the
            // format qualifier - the access is typeless as far as the shader is concerned:
            //
            //     writeonly uniform uimage2D uni_image;   // legal desktop GLSL
            //
            // GLSL ES has no such relaxation. Every image uniform must carry one, and Adreno
            // says so in as many words - "all images have to define layout format" - failing the
            // whole program, which is how KHR-GL4x.packed_depth_stencil.stencil_texturing's
            // compute half lost its only shader.
            //
            // The one format that is CORRECT to print is the one glBindImageTexture named for
            // that unit: GL requires the shader qualifier, the bind format and the texture's own
            // internal format to belong to the same format class, so the bind format is exactly
            // what the declaration would have said had it been written out. It is not knowable
            // at compile time, only at draw time, which is why this is a bake into the generated
            // program rather than a translation: the caller keys its build on the (unit, format)
            // pairs and rebuilds when a rebind moves one (BackendProgramObjectImpl,
            // MG_Backend/DirectGLES).
            //
            // Where the bake happens is the OpTypeImage's Image Format operand, before
            // SPIRV-Cross runs, rather than in the emitted text: SPIRV-Cross prints the operand
            // it is given, so setting it is the whole of the change, and the result stays a
            // valid module that spirv-val can still check.
            //
            // Deliberately narrow, on four axes:
            //
            //   * UNKNOWN formats only. A declared format is authoritative - a `layout(r32ui)`
            //     image must be read as r32ui whatever the texture behind it is - and this pass
            //     never overrides one. It is also what keeps the rebuild key at zero for the
            //     overwhelming majority of programs.
            //   * STORAGE images (Sampled == 2). A sampled image's format operand must stay
            //     Unknown; it has no format qualifier in any GLSL dialect.
            //   * MATCHING component class only. spirv-val requires the Image Format's component
            //     type to agree with the OpTypeImage's Sampled Type, so a bind format that
            //     disagrees with the declaration (which GL leaves undefined) is DECLINED rather
            //     than baked into an invalid module.
            //   * ESSL only. Vulkan takes an Unknown-format storage image natively given
            //     shaderStorageImageWriteWithoutFormat, and Magma resolves the view format from
            //     the same bind state at descriptor time (UniformManager), so the module must
            //     reach that backend unchanged.
            //
            // A variable whose uses are not the plain access-chain / load / image-op shape - an
            // image passed to a function, stored into a local - is DECLINED individually and
            // left format-less, rather than half-retyped into a module no driver would accept.
            // The decision is made before anything is mutated, so a decline costs nothing.
            class BakeImageFormatsPass final : public spvtools::opt::Pass {
            public:
                // Uniform NAME to the GL internal format bound to the image unit it addresses
                // (the `format` argument of glBindImageTexture). Names are the SPIR-V ones, i.e.
                // an array is named once, without a subscript. Formats with no image-format
                // spelling, and names the module does not declare, are ignored.
                using GLFormatByName = UnorderedMap<String, Uint>;

                explicit BakeImageFormatsPass(GLFormatByName glFormatByName)
                    : m_glFormatByName(Move(glFormatByName)) {}

                const char* name() const override { return "mobilegl-bake-image-formats"; }
                Status Process() override;

                // Whether the module declares a storage image with no format at all, i.e.
                // whether running this pass could change anything. Answered from a single parse
                // so the caller can skip the optimizer run entirely - which is every shader but
                // a handful.
                static bool DeclaresFormatlessStorageImage(const Vector<Uint32>& binary);

                // The GL internal format's SPIR-V ImageFormat, or 0 (Unknown) when the format
                // has no image-format spelling. Exposed for the caller's ESSL-side question of
                // whether an extension directive is needed for it.
                static Uint32 SpirvImageFormatFromGLInternalFormat(Uint glInternalFormat);

                // Whether the SPIR-V ImageFormat is one GLSL ES has in core. The rest exist only
                // under GL_NV_image_formats, whose directive the emitted ESSL must then carry.
                // (It is also exactly the set that needs no StorageImageExtendedFormats
                // capability in the module - both lists are the formats Vulkan requires without
                // an optional feature.)
                static bool IsCoreEsslImageFormat(Uint32 spirvImageFormat);
                // Whether SPIRV-Cross will PRINT the format when it targets ESSL. Its
                // is_desktop_only_format set throws instead of emitting, which loses the whole
                // stage, so those formats are left for the text-level completion in the backend
                // and are never baked into a module bound for SPIRV-Cross. A different question
                // from IsCoreEsslImageFormat, and a different set.
                static bool IsSpirvCrossEsslPrintableFormat(Uint32 spirvImageFormat);
                // The ESSL layout-qualifier spelling of a GL internal format, or empty when the
                // format has no image-format spelling. For the text-level completion above.
                static String EsslSpellingOfGLInternalFormat(Uint glInternalFormat);

                static spvtools::Optimizer::PassToken CreateBakeImageFormatsPass(GLFormatByName glFormatByName);

            private:
                GLFormatByName m_glFormatByName;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
