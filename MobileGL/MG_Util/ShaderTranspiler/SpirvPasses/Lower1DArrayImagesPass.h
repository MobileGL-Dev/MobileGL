// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/Lower1DArrayImagesPass.h
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
            // ES has no 1D texture of any kind, so a GL_TEXTURE_1D_ARRAY is stored as an ES 2D
            // array with height 1 and the layers in depth (TextureImpl::MapToBackendTextureTarget
            // and GetBackendUploadSize, MG_Backend/DirectGLES/Managers.h). The shader side has to
            // agree, and for SAMPLERS it does: SPIRV-Cross rewrites a 1D-array lookup into a
            // 2D-array one and moves the layer into the third component itself
            // (spirv_glsl.cpp, `if (imgtype.image.arrayed) ... ".x, 0.0, " ... ".y"`).
            //
            // For IMAGES it does not. The image path applies the same 1D emulation without ever
            // asking whether the type is arrayed:
            //
            //     if (type.image.dim == Dim1D && options.es)
            //         coord_expr = join("ivec2(", coord_expr, ", 0)");
            //
            // For a non-arrayed 1D image that is right - a scalar coordinate becomes (u, 0). For
            // a 1D ARRAY image the coordinate is already the two-component (u, layer), so the
            // result is `ivec2(ivec2(u, layer), 0)`: three components crammed into a two-component
            // constructor. Every ES driver rejects it outright, and the whole program is lost -
            // which is how one uimage1DArray uniform took the entire eleven-image compute shader
            // of KHR-GL44.multi_bind.dispatch_bind_image_textures down with it, with the driver
            // saying only "'constructor' : too many arguments".
            //
            // Widening the constructor would not be enough either. `ivec3(u, layer, 0)` puts the
            // layer in the 2D array's Y and reads layer 0, whereas the storage this has to match
            // puts height at 1 and the layers in Z, so the correct coordinate is (u, 0, layer).
            //
            // So this pass does the whole conversion in the module, before SPIRV-Cross sees it:
            // every 1D-array STORAGE image type becomes a 2D-array one, and every read and write
            // through it has its coordinate widened from (u, layer) to (u, 0, layer). SPIRV-Cross
            // is then looking at an ordinary 2D array image and its 1D path never fires.
            //
            // Deliberately narrow, on three axes:
            //
            //   * STORAGE images only (Sampled == 2). Sampled images reach SPIRV-Cross's sampler
            //     path, which is correct today; rewriting them would replace working emission
            //     with our own for no reason.
            //   * ARRAYED only. A non-arrayed 1D storage image is emitted correctly by the same
            //     SPIRV-Cross code, and is left to it.
            //   * ESSL only. Vulkan has VK_IMAGE_VIEW_TYPE_1D_ARRAY natively and Magma binds it
            //     directly, so the module must reach that backend unchanged.
            //
            // A size query on one of these images is DECLINED rather than half-translated: after
            // the rewrite OpImageQuerySize yields three components where the shader consumes two,
            // and silently handing back a differently-shaped size is worse than refusing. The
            // caller logs it and leaves the module alone.
            //
            // KNOWN LIMITATION - the decline is per MODULE, and a program is several of them. A
            // program whose vertex and fragment stages share a uimage1DArray uniform, where only
            // one stage calls imageSize() on it, gets that stage declined and the other rewritten:
            // the two then declare the same uniform with different types and the ES LINK fails on
            // a type mismatch, rather than the single compile error a reader of the comment above
            // would expect. Correlating the decision across a program's stages needs the decision
            // to be made where the program is known, which is above this pass; it is left undone
            // deliberately rather than papered over, because both outcomes are a refusal and the
            // shape has never been observed outside a deliberately constructed shader.
            class Lower1DArrayImagesPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-lower-1d-array-images"; }
                Status Process() override;

                // What one inspection of a binary tells the caller. Both answers come from a
                // SINGLE parse on purpose: every ESSL shader in the process reaches this, and
                // almost none of them declare a 1D-array storage image, so the common path has to
                // cost one module parse and no optimizer run at all - not one parse to ask about
                // size queries and a second inside an Optimizer that then early-outs.
                struct ModuleTraits {
                    // The module declares a 1D-array storage image, i.e. there is anything to do.
                    bool declaresImage = false;
                    // ...and queries its size, which is the shape this pass refuses to translate:
                    // afterwards the image is a 2D array, so the query yields three components
                    // where the shader consumes two, and there is no correct two-component answer
                    // to substitute. The caller leaves such a module alone rather than half
                    // rewriting it.
                    bool queriesImageSize = false;
                };
                static ModuleTraits InspectBinary(const Vector<Uint32>& binary);

                static spvtools::Optimizer::PassToken CreateLower1DArrayImagesPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
