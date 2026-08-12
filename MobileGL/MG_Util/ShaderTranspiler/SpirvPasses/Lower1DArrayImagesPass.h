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
            class Lower1DArrayImagesPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-lower-1d-array-images"; }
                Status Process() override;

                // True when the module declares a 1D-array storage image whose size is queried,
                // which is the shape this pass refuses to translate. Checked by the caller before
                // running, so a declined module is handed on untouched rather than partly
                // rewritten.
                static bool BinaryQueriesA1DArrayStorageImageSize(const Vector<Uint32>& binary);

                static spvtools::Optimizer::PassToken CreateLower1DArrayImagesPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
